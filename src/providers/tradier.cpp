#include "openport/providers/tradier.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>

#include "broker_json.hpp"

namespace openport::providers {
namespace {
using namespace broker;

Json response(std::string_view text, std::string_view wrapper) {
  auto doc = parse(text);
  if (doc.contains("fault") || doc.contains("errors") || !doc.contains(wrapper))
    throw std::runtime_error("Tradier: invalid market-data response");
  return doc;
}

long long header_integer(const net::HttpResponse& response, std::string_view name) {
  const auto value = response.header(name);
  if (value.empty()) return -1;
  long long result = -1;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  return error == std::errc{} && end == value.data() + value.size() ? result : -1;
}

std::chrono::seconds seconds_up(md::Timestamp value) {
  return std::chrono::seconds(value <= 0 ? 0 : 1 + (value - 1) / md::kNanosPerSecond);
}
}  // namespace

std::set<md::Date> parse_tradier_expirations(std::string_view json) {
  const auto doc = response(json, "expirations");
  const auto& expirations = field(doc, "expirations");
  std::set<md::Date> result;
  for (const auto& value : list(field(expirations, "date"))) result.insert(date(value));
  for (const auto& value : list(field(expirations, "expiration")))
    result.insert(date(field(value, "date")));
  return result;
}

std::map<std::string, std::set<md::Date>> parse_tradier_lookup(std::string_view json) {
  const auto doc = parse(json);
  if (!doc.contains("options") && !doc.contains("symbols"))
    throw std::runtime_error("Tradier: invalid options lookup");
  std::map<std::string, std::set<md::Date>> result;
  const auto add = [&](const Json& symbol) {
    const auto contract = md::parse_osi(str(symbol));
    if (!contract) throw std::runtime_error("Tradier: invalid lookup option symbol");
    result[contract->root].insert(contract->expiry);
  };
  for (const auto& option : list(field(field(doc, "options"), "option"))) add(field(option, "symbol"));
  for (const auto& root : list(field(doc, "symbols")))
    for (const auto& option : list(field(root, "options"))) add(option);
  return result;
}

std::vector<TradierOption> parse_tradier_chain(std::string_view json) {
  const auto doc = response(json, "options");
  std::vector<TradierOption> result;
  for (const auto& item : list(field(field(doc, "options"), "option"))) {
    auto contract = md::parse_osi(str(field(item, "symbol")));
    if (!contract) throw std::runtime_error("Tradier: invalid option symbol");
    const double multiplier = number(field(item, "contract_size"), contract->multiplier);
    if (!(multiplier > 0)) throw std::runtime_error("Tradier: invalid contract size");
    contract->multiplier = multiplier;
    contract->standard = contract->standard && multiplier == 100;
    TradierOption option;
    option.contract = *contract;
    const auto bid_ts = milliseconds(field(item, "bid_date"));
    const auto ask_ts = milliseconds(field(item, "ask_date"));
    option.ts = std::max(bid_ts, ask_ts);
    option.bid = std::max(0.0, number(field(item, "bid"), 0));
    option.ask = std::max(0.0, number(field(item, "ask"), 0));
    option.bid_size = std::max(0.0, number(field(item, "bidsize"), 0));
    option.ask_size = std::max(0.0, number(field(item, "asksize"), 0));
    if ((option.bid > 0 && bid_ts == 0) || (option.ask > 0 && ask_ts == 0))
      throw std::runtime_error("Tradier: quote timestamp missing or units unconfirmed");
    option.open_interest = number(field(item, "open_interest"), -1);
    option.volume = number(field(item, "volume"), -1);
    option.volume_ts = milliseconds(field(item, "trade_date"));
    if (option.volume != std::floor(option.volume)) option.volume = -1;
    const auto& greeks = field(item, "greeks");
    if (greeks.is_object()) {
      // ORATS: decimal IV, vega/rho per percentage point, theta per day.
      // An unzoned updated_at still has no known market timestamp.
      const double iv = number(field(greeks, "mid_iv"));
      option.greeks = md::VendorGreeks{0, 0, iv > 0 ? iv : missing,
                                      number(field(greeks, "delta")), number(field(greeks, "gamma")),
                                      number(field(greeks, "vega")), number(field(greeks, "theta")),
                                      number(field(greeks, "rho"))};
    }
    result.push_back(std::move(option));
  }
  return result;
}

md::UnderlyingQuote parse_tradier_quote(std::string_view json, const std::string& symbol) {
  const auto doc = response(json, "quotes");
  for (const auto& quote : list(field(field(doc, "quotes"), "quote"))) {
    if (str(field(quote, "symbol")) != symbol) continue;
    const auto trade_ts = milliseconds(field(quote, "trade_date"));
    const double last = number(field(quote, "last"), 0);
    // The closing print keeps its own clock, even while options trade overnight.
    if (last > 0 && trade_ts > 0) return {symbol, trade_ts, 0, 0, last};
    const auto bid_ts = milliseconds(field(quote, "bid_date"));
    const auto ask_ts = milliseconds(field(quote, "ask_date"));
    const double bid = bid_ts ? std::max(0.0, number(field(quote, "bid"), 0)) : 0;
    const double ask = ask_ts ? std::max(0.0, number(field(quote, "ask"), 0)) : 0;
    return {symbol, std::max(bid_ts, ask_ts), bid, ask, 0};
  }
  return {symbol, 0, 0, 0, 0};
}

std::chrono::seconds TradierBudget::interval(std::size_t requests) const {
  return std::chrono::seconds(std::max<std::size_t>(1, (requests * 60 + static_cast<std::size_t>(limit_) - 1) /
                                                     static_cast<std::size_t>(limit_)));
}

std::chrono::seconds TradierBudget::wait(md::Timestamp now) {
  while (!sent_.empty() && sent_.front() <= now - md::kNanosPerMinute) sent_.pop_front();
  auto due = blocked_until_;
  if (now >= remote_reset_) remaining_ = -1;
  if (remaining_ == 0) due = std::max(due, remote_reset_);
  if (sent_.size() >= static_cast<std::size_t>(limit_)) due = std::max(due, sent_.front() + md::kNanosPerMinute);
  return seconds_up(due - now);
}

void TradierBudget::sent(md::Timestamp now) {
  sent_.push_back(now);
  if (remaining_ > 0) --remaining_;
}

bool TradierBudget::received(const net::HttpResponse& response, md::Timestamp now) {
  const auto allowed = header_integer(response, "X-Ratelimit-Allowed");
  if (allowed > 0 && allowed < limit_) limit_ = static_cast<int>(allowed);
  const auto available = header_integer(response, "X-Ratelimit-Available");
  const auto used = header_integer(response, "X-Ratelimit-Used");
  if (available >= 0 || used >= 0) {
    remaining_ = available >= 0 ? available : std::max(0LL, static_cast<long long>(limit_) - used);
    remote_reset_ = now + md::kNanosPerMinute;
  }
  std::string body = response.body;
  std::transform(body.begin(), body.end(), body.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const bool signal = body.find("rate limit") != std::string::npos || body.find("rate-limit") != std::string::npos ||
                      body.find("ratelimit") != std::string::npos ||
                      body.find("quota violation") != std::string::npos;
  const bool throttled = response.status == 429 || signal ||
                         (response.status >= 400 && (available == 0 || used >= limit_));
  if (available == 0 || used >= limit_ || throttled) {
    auto delay = std::chrono::seconds(60);
    // Expiry units are undocumented. Recognise plausible epoch seconds or ms,
    // conservatively waiting at least one complete window for other shapes.
    const auto expiry = header_integer(response, "X-Ratelimit-Expiry");
    const auto now_seconds = now / md::kNanosPerSecond;
    const auto expiry_seconds = expiry >= 1'000'000'000'000LL ? expiry / 1000 : expiry;
    if (expiry_seconds > now_seconds && expiry_seconds - now_seconds <= 3600)
      delay = std::max(delay, std::chrono::seconds(expiry_seconds - now_seconds + 1));
    const auto retry = header_integer(response, "Retry-After");
    if (retry > 0 && retry <= 3600) delay = std::max(delay, std::chrono::seconds(retry));
    if (throttled) {
      failures_ = std::min(failures_ + 1, 4);
      delay = std::max(delay, std::chrono::seconds(std::min(300, 30 * (1 << failures_))));
    }
    blocked_until_ = std::max(blocked_until_, now + delay.count() * md::kNanosPerSecond);
  }
  return throttled;
}

TradierProvider::TradierProvider(Options options)
    : PollingProvider(std::chrono::seconds(1)), options_(std::move(options)), budget_(options_.sandbox) {
  if (options_.access_token.empty()) throw std::invalid_argument("tradier: set TRADIER_ACCESS_TOKEN");
  if (options_.poll_seconds.count() < 0) throw std::invalid_argument("tradier: poll interval must be positive");
}

md::Capabilities TradierProvider::capabilities() const noexcept {
  return {.realtime = false, .realtime_plan_dependent = !options_.sandbox,
          .poll_interval = poll_interval(), .delay = std::chrono::seconds(options_.sandbox ? 900 : 0),
          .open_interest = true, .vendor_greeks = true};
}

std::chrono::seconds TradierProvider::poll_interval() const noexcept {
  return std::chrono::seconds(interval_seconds_.load());
}

std::string TradierProvider::get(net::HttpClient& http, const std::string& path) {
  // Only these GET paths are allowed. No redirects, account reads or broker trading calls.
  const auto endpoint = path.substr(0, path.find('?'));
  if (endpoint != "/markets/options/expirations" && endpoint != "/markets/options/lookup" &&
      endpoint != "/markets/options/chains" && endpoint != "/markets/quotes")
    throw std::logic_error("tradier: forbidden endpoint");
  for (auto delay = budget_.wait(options_.clock()); delay.count() > 0; delay = budget_.wait(options_.clock())) {
    if (!sleep(delay)) check_cancelled();
  }
  check_cancelled();
  budget_.sent(options_.clock());
  const std::string base = options_.sandbox ? "https://sandbox.tradier.com/v1" : "https://api.tradier.com/v1";
  const auto result = http.get_direct(base + path, {{"Authorization", "Bearer " + options_.access_token},
                                                   {"Accept", "application/json"}},
                                      std::chrono::seconds(30), cancellation());
  if (budget_.received(result, options_.clock())) throw std::runtime_error("rate limited; backing off");
  if (result.status != 200) throw std::runtime_error("HTTP " + std::to_string(result.status));
  return result.body;
}

std::string TradierProvider::poll(net::HttpClient& http, const std::string& underlying,
                                 const md::Subscription& sub, md::EventSink& sink) {
  const auto symbol = symbol_path(underlying);
  const auto expiries = parse_tradier_expirations(get(http, "/markets/options/expirations?symbol=" + symbol + "&includeAllRoots=true"));
  const auto roots = parse_tradier_lookup(get(http, "/markets/options/lookup?underlying=" + symbol));
  const auto spot = parse_tradier_quote(get(http, "/markets/quotes?symbols=" + symbol), underlying);
  const auto today = md::new_york_time(spot.ts > 0 ? spot.ts : options_.clock()).date;
  const double price = spot.last > 0 ? spot.last : (spot.bid > 0 && spot.ask > 0 ? (spot.bid + spot.ask) / 2 : 0);
  if (sub.strike_window > 0 && price <= 0) throw std::runtime_error("strike filter needs an underlying price");
  const ChainFilter filter(sub, today, price, expiries);
  std::set<md::Date> selected;
  for (const auto& expiry : expiries) {
    md::OptionContract candidate;
    candidate.expiry = expiry;
    candidate.strike = price;
    if (filter.admits(candidate) || retained_expiries_[underlying].contains(expiry)) selected.insert(expiry);
  }
  std::vector<std::pair<std::string, md::Date>> requests;
  for (const auto& [root, dates] : roots)
    for (const auto& expiry : selected)
      if (dates.contains(expiry)) requests.emplace_back(root, expiry);
  for (const auto& expiry : selected)
    if (std::none_of(requests.begin(), requests.end(), [&](const auto& request) { return request.second == expiry; }))
      throw std::runtime_error("option lookup did not resolve selected expirations");
  request_counts_[underlying] = 3 + requests.size();
  std::size_t cycle = 0;
  for (const auto& name : sub.underlyings)
    cycle += request_counts_.contains(name) ? request_counts_.at(name) : 3 + requests.size();
  if (sub.underlyings.empty()) cycle = 3 + requests.size();
  const auto required = budget_.interval(cycle);
  if (options_.poll_seconds.count() > 0 && options_.poll_seconds < required)
    throw std::runtime_error("poll_seconds is below the request budget; needs at least " + std::to_string(required.count()));
  interval_seconds_ = static_cast<int>(options_.poll_seconds.count() > 0 ? options_.poll_seconds.count() : required.count());
  std::map<std::string, TradierOption> contracts;
  md::Timestamp market_time = spot.ts;
  for (const auto& [root, expiry] : requests) {
    auto chain = parse_tradier_chain(get(http, "/markets/options/chains?symbol=" + symbol_path(root) +
                                        "&expiration=" + md::format_date(expiry) + "&greeks=true"));
    for (auto& option : chain) {
      if (option.contract.underlying != underlying || option.contract.expiry != expiry)
        throw std::runtime_error("chain does not match requested underlying and expiry");
      market_time = std::max(market_time, option.ts);
      contracts.insert_or_assign(option.contract.osi_symbol(), std::move(option));
    }
  }
  if (market_time <= 0) throw std::runtime_error("snapshot has no known market timestamp");
  check_cancelled();
  if (spot.ts > 0) sink.publish(spot);
  std::set<md::InstrumentId> seen;
  for (const auto& [osi, option] : contracts) {
    if (!publisher_.known(osi) && !filter.admits(option.contract)) continue;
    const auto id = publisher_.define(osi, option.contract, sink);
    retained_expiries_[underlying].insert(option.contract.expiry);
    seen.insert(id);
    // Tradier's field reference says sizes are "in hundreds", which fits its stock
    // quotes (round lots); its option-chain examples read as contracts, the unit OPRA
    // publishes. option_size_unit overrides the default of contracts.
    publisher_.quote(id, option.ts, option.bid, option.ask, option.bid_size * options_.size_multiplier,
                     option.ask_size * options_.size_multiplier, sink);
    publisher_.volume(id, option.volume_ts, option.volume, sink);
    if (option.open_interest >= 0) publisher_.open_interest(id, 0, option.open_interest, sink);
    if (option.greeks) {
      auto greeks = *option.greeks;
      greeks.id = id;
      publisher_.greeks(greeks, sink);
    }
  }
  publisher_.finish(underlying, seen, market_time, sink);
  budget_.succeeded();
  state_ = options_.sandbox ? md::FeedState::Delayed :
           md::is_index_underlying(underlying) ? md::FeedState::Stale : md::FeedState::Live;
  return options_.sandbox ? "Tradier sandbox: 15-minute delayed" :
         md::is_index_underlying(underlying) ? "Tradier: real-time options; index timing unconfirmed, values may be derived" :
         "Tradier: real-time options and underlying";
}
}  // namespace openport::providers
