#include "openport/providers/schwab.hpp"

#include <algorithm>
#include <charconv>
#include <stdexcept>

#include "broker_json.hpp"

namespace openport::providers {
namespace {
using namespace broker;
constexpr std::string_view kOrigin = "https://api.schwabapi.com";

net::Headers headers() {
  return {{"User-Agent", std::string(net::user_agent())}, {"Accept", "application/json"}};
}

/// Schwab reports an unavailable Greek as -999.
double greek(const Json& value) {
  const double n = number(value);
  return n <= -999 ? missing : n;
}

std::chrono::seconds retry_after(const net::HttpResponse& response) {
  const auto value = response.header("Retry-After");
  int seconds = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
  return std::chrono::seconds(error == std::errc{} && end == value.data() + value.size() ? seconds : 0);
}

void add_options(const Json& map, std::string_view underlying, bool call, std::vector<SchwabOption>& out) {
  if (map.is_null()) return;
  if (!map.is_object()) throw std::runtime_error("Schwab: invalid expiration map");
  for (const auto& [expiry_key, strikes] : map.items()) {
    // Keys are "YYYY-MM-DD:days"; the option's own symbol is authoritative.
    const auto expiry = date(Json(expiry_key.substr(0, expiry_key.find(':'))));
    if (!strikes.is_object()) throw std::runtime_error("Schwab: invalid strike map");
    for (const auto& [strike_key, items] : strikes.items()) {
      (void)strike_key;
      for (const auto& item : list(items)) {
        auto contract = md::parse_osi(str(field(item, "symbol")));
        if (!contract || contract->underlying != underlying || contract->expiry != expiry ||
            (contract->type == pricing::OptionType::Call) != call ||
            str(field(item, "putCall")) != (call ? "CALL" : "PUT") ||
            contract->strike != number(field(item, "strikePrice")))
          throw std::runtime_error("Schwab: inconsistent option definition");
        const double multiplier = number(field(item, "multiplier"), 100);
        if (!(multiplier > 0)) throw std::runtime_error("Schwab: invalid contract size");
        contract->multiplier = multiplier;
        contract->standard = contract->standard && multiplier == 100 &&
                             field(item, "nonStandard") != Json(true) && field(item, "mini") != Json(true);
        SchwabOption option;
        option.contract = *contract;
        option.ts = milliseconds(field(item, "quoteTimeInLong"));
        option.bid = std::max(0.0, number(field(item, "bid"), 0));
        option.ask = std::max(0.0, number(field(item, "ask"), 0));
        option.bid_size = std::max(0.0, number(field(item, "bidSize"), 0));
        option.ask_size = std::max(0.0, number(field(item, "askSize"), 0));
        if ((option.bid > 0 || option.ask > 0) && option.ts == 0)
          throw std::runtime_error("Schwab: quote timestamp missing or units unconfirmed");
        option.open_interest = number(field(item, "openInterest"), -1);
        option.volume = number(field(item, "totalVolume"), -1);
        // Delta and gamma are price derivatives. The scaling of volatility (a
        // percentage in the examples), vega, theta and rho is not documented.
        const double delta = greek(field(item, "delta"));
        const double gamma = greek(field(item, "gamma"));
        if (std::isfinite(delta) || std::isfinite(gamma))
          option.greeks = md::VendorGreeks{0, 0, missing, delta, gamma, missing, missing, missing};
        out.push_back(std::move(option));
      }
    }
  }
}
}  // namespace

std::string schwab_symbol(std::string_view underlying) {
  return md::is_index_underlying(underlying) ? "$" + std::string(underlying) : std::string(underlying);
}

SchwabChain parse_schwab_chain(std::string_view json, std::string_view underlying) {
  const auto doc = parse(json);
  if (str(field(doc, "status")) != "SUCCESS") throw std::runtime_error("Schwab: chain request failed");
  if (str(field(doc, "symbol")) != schwab_symbol(underlying))
    throw std::runtime_error("Schwab: chain underlying mismatch");
  SchwabChain chain;
  chain.underlying.symbol = std::string(underlying);
  add_options(field(doc, "callExpDateMap"), underlying, true, chain.options);
  add_options(field(doc, "putExpDateMap"), underlying, false, chain.options);
  if (const auto& delayed = field(doc, "isDelayed"); delayed.is_boolean()) chain.delayed = delayed.get<bool>();
  chain.underlying_price = std::max(0.0, number(field(doc, "underlyingPrice"), 0));
  const auto& quote = field(doc, "underlying");
  if (quote.is_object()) {
    const auto trade_ts = milliseconds(field(quote, "tradeTime"));
    const double last = number(field(quote, "last"), 0);
    // The closing print keeps its own clock, as with the other brokers.
    if (last > 0 && trade_ts > 0) {
      chain.underlying.ts = trade_ts;
      chain.underlying.last = last;
    } else if (const auto quote_ts = milliseconds(field(quote, "quoteTime")); quote_ts > 0) {
      chain.underlying.ts = quote_ts;
      chain.underlying.bid = std::max(0.0, number(field(quote, "bid"), 0));
      chain.underlying.ask = std::max(0.0, number(field(quote, "ask"), 0));
    }
  }
  return chain;
}

std::set<md::Date> parse_schwab_expirations(std::string_view json) {
  const auto doc = parse(json);
  if (!doc.contains("expirationList")) throw std::runtime_error("Schwab: invalid expiration chain");
  std::set<md::Date> result;
  for (const auto& item : list(field(doc, "expirationList"))) result.insert(date(field(item, "expirationDate")));
  return result;
}

SchwabApi::SchwabApi(Credentials credentials) : credentials_(std::move(credentials)) {
  if (credentials_.client_id.empty() || credentials_.client_secret.empty() || credentials_.refresh_token.empty())
    throw std::invalid_argument("schwab: set SCHWAB_CLIENT_ID, SCHWAB_CLIENT_SECRET and SCHWAB_REFRESH_TOKEN");
}

void SchwabApi::refresh(net::HttpClient& http, md::Timestamp now, const std::atomic<bool>* cancel) {
  if (!access_token_.empty() && now < refresh_at_) return;
  auto sent = headers();
  sent.emplace_back("Authorization", "Basic " + base64(credentials_.client_id + ":" + credentials_.client_secret));
  sent.emplace_back("Content-Type", "application/x-www-form-urlencoded");
  const auto body = "grant_type=refresh_token&refresh_token=" + form_value(credentials_.refresh_token);
  // The sole POST is the OAuth refresh. Never follow redirects with this body.
  const auto result = http.post(std::string(kOrigin) + "/v1/oauth/token", body, sent, std::chrono::seconds(15), cancel);
  if (result.status != 200) throw std::runtime_error("Schwab: OAuth HTTP " + std::to_string(result.status));
  const auto doc = parse(result.body);
  auto token = str(field(doc, "access_token"));
  const auto type = str(field(doc, "token_type"));
  const double lifetime = number(field(doc, "expires_in"), 1800);
  if (token.empty() || (!type.empty() && type != "Bearer" && type != "bearer") || lifetime < 1 || lifetime > 86400)
    throw std::runtime_error("Schwab: invalid OAuth token response");
  access_token_ = std::move(token);
  // Schwab may rotate the refresh token; keep the newest one in memory only.
  if (auto rotated = str(field(doc, "refresh_token")); !rotated.empty()) credentials_.refresh_token = std::move(rotated);
  refresh_at_ = now + static_cast<md::Timestamp>((lifetime - std::min(60.0, lifetime / 10)) * 1e9);
}

net::HttpResponse SchwabApi::get(net::HttpClient& http, const std::string& path, md::Timestamp now,
                                 const std::atomic<bool>* cancel) {
  // Only these market-data GETs. No accounts, orders or trading endpoints.
  const auto endpoint = path.substr(0, path.find('?'));
  if (endpoint != "/chains" && endpoint != "/expirationchain") throw std::logic_error("schwab: forbidden endpoint");
  for (int attempt = 0; attempt < 2; ++attempt) {
    refresh(http, now, cancel);
    auto sent = headers();
    sent.emplace_back("Authorization", "Bearer " + access_token_);
    auto result = http.get_direct(std::string(kOrigin) + "/marketdata/v1" + path, sent, std::chrono::seconds(30), cancel);
    if (result.status != 401 || attempt != 0) return result;
    access_token_.clear();
    refresh_at_ = 0;
  }
  throw std::runtime_error("Schwab: authentication failed");
}

SchwabProvider::SchwabProvider(Options options)
    : PollingProvider(std::chrono::seconds(5)), options_(std::move(options)), api_(options_.credentials) {
  if (options_.poll_seconds.count() < 0) throw std::invalid_argument("schwab: poll interval must be positive");
}

md::Capabilities SchwabProvider::capabilities() const noexcept {
  return {.realtime = false, .realtime_plan_dependent = true, .poll_interval = poll_interval(),
          .open_interest = true, .vendor_greeks = true};
}

std::chrono::seconds SchwabProvider::poll_interval() const noexcept {
  return std::chrono::seconds(interval_seconds_.load());
}

std::string SchwabProvider::get(net::HttpClient& http, const std::string& path) {
  for (auto delay = budget_.wait(options_.clock()); delay.count() > 0; delay = budget_.wait(options_.clock())) {
    if (!sleep(delay)) check_cancelled();
  }
  check_cancelled();
  budget_.sent(options_.clock());
  const auto result = api_.get(http, path, options_.clock(), cancellation());
  if (result.status == 429) {
    budget_.throttled(options_.clock(), retry_after(result));
    throw std::runtime_error("Schwab: rate limited; backing off");
  }
  if (result.status != 200) throw std::runtime_error("Schwab: HTTP " + std::to_string(result.status));
  return result.body;
}

std::string SchwabProvider::poll(net::HttpClient& http, const std::string& underlying,
                                 const md::Subscription& sub, md::EventSink& sink) {
  const auto symbol = symbol_path(schwab_symbol(underlying));
  const auto today = md::new_york_time(options_.clock()).date;
  auto path = "/chains?symbol=" + symbol + "&contractType=ALL&includeUnderlyingQuote=true&strategy=SINGLE";
  std::size_t requests = 1;
  std::optional<std::pair<md::Date, md::Date>> range;
  if (sub.max_expiries > 0) {
    // The nearest expiries, plus any already defined so their quotes keep updating.
    const auto expiries = parse_schwab_expirations(get(http, "/expirationchain?symbol=" + symbol));
    ++requests;
    std::set<md::Date> selected;
    for (const auto& expiry : expiries) {
      if (expiry < today) continue;
      if (selected.size() >= static_cast<std::size_t>(sub.max_expiries)) break;
      selected.insert(expiry);
    }
    for (const auto& expiry : retained_expiries_[underlying])
      if (expiry >= today) selected.insert(expiry);
    if (!selected.empty()) {
      range = {*selected.begin(), *selected.rbegin()};
      path += "&fromDate=" + md::format_date(range->first) + "&toDate=" + md::format_date(range->second);
    }
  }
  request_counts_[underlying] = requests;
  std::size_t cycle = 0;
  for (const auto& name : sub.underlyings) cycle += request_counts_.contains(name) ? request_counts_.at(name) : requests;
  if (sub.underlyings.empty()) cycle = requests;
  const auto required = budget_.interval(cycle);
  if (options_.poll_seconds.count() > 0 && options_.poll_seconds < required)
    throw std::runtime_error("poll_seconds is below the request budget; needs at least " + std::to_string(required.count()));
  interval_seconds_ = static_cast<int>(options_.poll_seconds.count() > 0 ? options_.poll_seconds.count()
                                                                         : std::max<long long>(5, required.count()));
  auto chain = parse_schwab_chain(get(http, path), underlying);
  std::set<md::Date> expiries;
  md::Timestamp market_time = chain.underlying.ts;
  for (const auto& option : chain.options) {
    if (range && (option.contract.expiry < range->first || option.contract.expiry > range->second))
      throw std::runtime_error("chain does not match the requested expirations");
    expiries.insert(option.contract.expiry);
    market_time = std::max(market_time, option.ts);
  }
  const auto& spot = chain.underlying;
  double price = spot.last > 0 ? spot.last : (spot.bid > 0 && spot.ask > 0 ? (spot.bid + spot.ask) / 2 : 0);
  if (price <= 0) price = chain.underlying_price;
  if (sub.strike_window > 0 && price <= 0) throw std::runtime_error("strike filter needs an underlying price");
  if (market_time <= 0) throw std::runtime_error("snapshot has no known market timestamp");
  const ChainFilter filter(sub, today, price, expiries);
  std::sort(chain.options.begin(), chain.options.end(), [](const auto& a, const auto& b) {
    return a.contract.osi_symbol() < b.contract.osi_symbol();
  });
  check_cancelled();
  if (spot.ts > 0) sink.publish(spot);
  std::set<md::InstrumentId> seen;
  for (const auto& option : chain.options) {
    const auto osi = option.contract.osi_symbol();
    if (!publisher_.known(osi) && !filter.admits(option.contract)) continue;
    const auto id = publisher_.define(osi, option.contract, sink);
    retained_expiries_[underlying].insert(option.contract.expiry);
    seen.insert(id);
    publisher_.quote(id, option.ts, option.bid, option.ask, option.bid_size, option.ask_size, sink);
    if (option.open_interest >= 0) publisher_.open_interest(id, 0, option.open_interest, sink);
    if (option.volume >= 0) publisher_.volume(id, option.ts, option.volume, sink);
    if (option.greeks) {
      auto greeks = *option.greeks;
      greeks.id = id;
      publisher_.greeks(greeks, sink);
    }
  }
  publisher_.finish(underlying, seen, market_time, sink);
  budget_.succeeded();
  if (!chain.delayed) {
    state_ = md::FeedState::Stale;
    return "Schwab: chain did not state whether it is delayed; timing unconfirmed";
  }
  state_ = *chain.delayed ? md::FeedState::Delayed : md::FeedState::Live;
  return *chain.delayed ? "Schwab: delayed chain snapshot" : "Schwab: real-time chain snapshot";
}

}  // namespace openport::providers
