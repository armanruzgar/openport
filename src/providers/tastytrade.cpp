#include "openport/providers/tastytrade.hpp"

#include <algorithm>
#include <stdexcept>

#include "broker_json.hpp"

namespace openport::providers {
namespace {
using namespace broker;
constexpr std::string_view kBase = "https://api.tastyworks.com";
std::atomic<unsigned> sessions{0};

Json data(std::string_view json) {
  auto doc = parse(json);
  const auto& result = field(doc, "data");
  if (!result.is_object() || doc.contains("error")) throw std::runtime_error("tastytrade: invalid data response");
  return result;
}

net::Headers headers() {
  return {{"User-Agent", std::string(net::user_agent())}, {"Accept", "application/json"}};
}

bool allowed_path(std::string_view path) {
  // Only the quote token, nested chains and underlying instrument definitions.
  // A symbol is one encoded path component. No account or trading endpoints.
  if (path == "/api-quote-tokens") return true;
  for (const auto prefix : {std::string_view("/option-chains/"), std::string_view("/instruments/equities/")}) {
    if (!path.starts_with(prefix)) continue;
    auto symbol = path.substr(prefix.size());
    if (prefix == "/option-chains/") {
      if (!symbol.ends_with("/nested")) return false;
      symbol.remove_suffix(7);
    }
    if (symbol.empty()) return false;
    for (const char c : symbol)
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '%')) return false;
    // Reject encoded path separators/dot components as well as literal separators.
    std::string decoded;
    for (std::size_t i = 0; i < symbol.size(); ++i) {
      if (symbol[i] != '%') { decoded += symbol[i]; continue; }
      if (i + 2 >= symbol.size()) return false;
      unsigned value = 0;
      const auto [end, error] = std::from_chars(symbol.data() + i + 1, symbol.data() + i + 3, value, 16);
      if (error != std::errc{} || end != symbol.data() + i + 3 || value == '/' || value == '\\' || value == '%') return false;
      decoded += static_cast<char>(value);
      i += 2;
    }
    return decoded != "." && decoded != "..";
  }
  return false;
}
}  // namespace

std::vector<TastytradeContract> parse_tastytrade_chain(std::string_view json, std::string_view underlying) {
  const auto doc = data(json);
  if (!doc.contains("items")) throw std::runtime_error("tastytrade: missing chain items");
  std::vector<TastytradeContract> result;
  std::set<std::string> seen;
  for (const auto& item : list(field(doc, "items"))) {
    if (str(field(item, "underlying-symbol")) != underlying)
      throw std::runtime_error("tastytrade: chain underlying mismatch");
    const auto root = str(field(item, "root-symbol"));
    const double multiplier = number(field(item, "shares-per-contract"), 100);
    if (!(multiplier > 0)) throw std::runtime_error("tastytrade: invalid contract size");
    for (const auto& expiry : list(field(item, "expirations"))) {
      const auto expires = date(field(expiry, "expiration-date"));
      for (const auto& strike : list(field(expiry, "strikes"))) {
        for (const auto& side : {std::string("call"), std::string("put")}) {
          const auto osi = str(field(strike, side));
          const auto streamer = str(field(strike, side + "-streamer-symbol"));
          if (osi.empty() && streamer.empty()) continue;
          auto contract = md::parse_osi(osi);
          if (!contract || streamer.empty() || contract->expiry != expires || contract->root != root ||
              contract->underlying != underlying ||
              (side == "call") != (contract->type == pricing::OptionType::Call) ||
              contract->strike != number(field(strike, "strike-price")))
            throw std::runtime_error("tastytrade: inconsistent nested option definition");
          contract->multiplier = multiplier;
          contract->standard = contract->standard && multiplier == 100 && str(field(item, "option-chain-type")) == "Standard";
          const auto settlement = str(field(expiry, "settlement-type"));
          if (settlement == "AM") contract->settlement = md::Settlement::AM;
          else if (settlement == "PM") contract->settlement = md::Settlement::PM;
          else if (!settlement.empty()) contract->standard = false;
          if (!seen.insert(streamer).second) throw std::runtime_error("tastytrade: duplicate streamer symbol");
          // Use the supplied mapping, never construct a streamer name from OCC.
          result.push_back({*contract, streamer});
        }
      }
    }
  }
  return result;
}

std::string parse_tastytrade_streamer(std::string_view json) {
  auto streamer = str(field(data(json), "streamer-symbol"));
  if (streamer.empty()) throw std::runtime_error("tastytrade: missing underlying streamer symbol");
  return streamer;
}

TastytradeApi::TastytradeApi(Credentials credentials) : credentials_(std::move(credentials)) {
  if (credentials_.client_secret.empty() || credentials_.refresh_token.empty())
    throw std::invalid_argument("tastytrade: set TASTYTRADE_CLIENT_SECRET and TASTYTRADE_REFRESH_TOKEN");
}

void TastytradeApi::refresh(net::HttpClient& http, md::Timestamp now, const std::atomic<bool>* cancel) {
  if (!access_token_.empty() && now < refresh_at_) return;
  Json body{{"grant_type", "refresh_token"}, {"client_secret", credentials_.client_secret},
            {"refresh_token", credentials_.refresh_token}};
  if (!credentials_.client_id.empty()) body["client_id"] = credentials_.client_id;
  auto sent = headers();
  sent.emplace_back("Content-Type", "application/json");
  // The sole POST path is OAuth refresh. Never follow redirects with this body.
  const auto result = http.post(std::string(kBase) + "/oauth/token", body.dump(), sent, std::chrono::seconds(15), cancel);
  if (result.status != 200) throw std::runtime_error("tastytrade: OAuth HTTP " + std::to_string(result.status));
  const auto doc = parse(result.body);
  auto token = str(field(doc, "access_token"));
  const auto type = str(field(doc, "token_type"));
  const double lifetime = number(field(doc, "expires_in"), 900);
  if (token.empty() || (!type.empty() && type != "Bearer" && type != "bearer") || lifetime < 1 || lifetime > 86400)
    throw std::runtime_error("tastytrade: invalid OAuth token response");
  access_token_ = std::move(token);
  refresh_at_ = now + static_cast<md::Timestamp>((lifetime - std::min(60.0, lifetime / 10)) * 1e9);
}

std::string TastytradeApi::get(net::HttpClient& http, const std::string& path, md::Timestamp now,
                              const std::atomic<bool>* cancel) {
  if (!allowed_path(path)) throw std::invalid_argument("tastytrade: forbidden endpoint");
  for (int attempt = 0; attempt < 2; ++attempt) {
    refresh(http, now, cancel);
    auto sent = headers();
    sent.emplace_back("Authorization", "Bearer " + access_token_);
    const auto result = http.get_direct(std::string(kBase) + path, sent, std::chrono::seconds(15), cancel);
    if (result.status == 200) return result.body;
    if (result.status != 401 || attempt != 0) throw std::runtime_error("tastytrade: HTTP " + std::to_string(result.status));
    access_token_.clear();
    refresh_at_ = 0;
  }
  throw std::runtime_error("tastytrade: authentication failed");
}

TastytradeQuoteToken parse_tastytrade_quote_token(std::string_view json, md::Timestamp now) {
  const auto doc = data(json);
  TastytradeQuoteToken token{str(field(doc, "token")), str(field(doc, "dxlink-url")), now + 23 * 3600 * md::kNanosPerSecond};
  if (token.token.empty() || !token.url.starts_with("wss://")) throw std::runtime_error("tastytrade: invalid DXLink credentials");
  const auto expiry = str(field(doc, "expires-at"));
  if (!expiry.empty()) {
    const auto ts = md::parse_datetime(expiry, md::Zone::Utc);
    if (!ts || *ts <= now + 60 * md::kNanosPerSecond) throw std::runtime_error("tastytrade: expired quote token");
    token.refresh_at = std::min(token.refresh_at, *ts - 60 * md::kNanosPerSecond);
  }
  return token;
}

void TastytradeMapper::chain(std::string underlying, std::string streamer,
                             std::vector<TastytradeContract> contracts, md::Date today) {
  std::set<std::string> present;
  for (const auto& option : contracts)
    if (option.contract.expiry >= today) present.insert(option.streamer_symbol);
  for (auto it = instruments_.begin(); it != instruments_.end();) {
    if (it->second.second == underlying && !present.contains(it->first)) {
      // This instrument disappeared from a complete chain refresh. Invalidate its
      // quote without inventing a market observation time or a stream snapshot.
      sink_.publish(md::OptionQuote{it->second.first, 0, 0, 0, 0, 0});
      it = instruments_.erase(it);
    } else ++it;
  }
  std::erase_if(underlyings_, [&](const auto& entry) { return entry.second == underlying; });
  underlyings_[std::move(streamer)] = underlying;
  auto& chain = chains_[underlying];
  chain.contracts = std::move(contracts);
  chain.today = today;
  select(underlying);
}

void TastytradeMapper::select(const std::string& underlying) {
  const auto& chain = chains_.at(underlying);
  if (subscription_.strike_window > 0 && chain.spot <= 0) return;
  std::set<md::Date> expiries;
  for (const auto& option : chain.contracts) expiries.insert(option.contract.expiry);
  const ChainFilter filter(subscription_, chain.today, chain.spot, expiries);
  for (const auto& option : chain.contracts) {
    if (option.contract.expiry < chain.today) continue;
    const auto osi = option.contract.osi_symbol();
    if (!definitions_.known(osi) && !filter.admits(option.contract)) continue;
    const auto id = definitions_.define(osi, option.contract, sink_);
    instruments_[option.streamer_symbol] = {id, underlying};
  }
}

std::set<DxlinkSubscription> TastytradeMapper::subscriptions() const {
  std::set<DxlinkSubscription> result;
  for (const auto& [symbol, underlying] : underlyings_) {
    (void)underlying;
    for (const auto* type : {"Quote", "Trade"}) result.insert({type, symbol});
  }
  // Only the events event() reads: each subscription counts toward the 25000 cap.
  // An option's Trade carries its session dayVolume.
  for (const auto& [symbol, instrument] : instruments_) {
    (void)instrument;
    for (const auto* type : {"Quote", "Greeks", "Summary", "Trade"}) result.insert({type, symbol});
  }
  if (result.size() > 25000) throw std::runtime_error("tastytrade: exceeds 25000 event subscriptions; use expiry/strike filters");
  return result;
}

void TastytradeMapper::event(const Json& event) {
  const auto symbol = str(field(event, "eventSymbol"));
  const auto type = str(field(event, "eventType"));
  const auto timestamp = [&](std::string_view key) { return milliseconds_ ? milliseconds(field(event, key)) : 0; };
  const auto underlying = underlyings_.find(symbol);
  if (underlying != underlyings_.end()) {
    if (type != "Quote" && type != "Trade") return;
    const auto name = underlying->second;
    md::UnderlyingQuote quote{name, 0, 0, 0, 0};
    if (type == "Quote") {
      quote.ts = std::max(timestamp("bidTime"), timestamp("askTime"));
      quote.bid = std::max(0.0, number(field(event, "bidPrice"), 0));
      quote.ask = std::max(0.0, number(field(event, "askPrice"), 0));
      if ((quote.bid > 0 && timestamp("bidTime") == 0) || (quote.ask > 0 && timestamp("askTime") == 0)) quote.ts = 0;
      if (quote.bid > 0 && quote.ask > 0) chains_.at(name).spot = (quote.bid + quote.ask) / 2;
    } else {
      quote.ts = timestamp("time");
      quote.last = std::max(0.0, number(field(event, "price"), 0));
      if (quote.last > 0) chains_.at(name).spot = quote.last;
    }
    sink_.publish(quote);
    select(name);
    return;
  }
  const auto it = instruments_.find(symbol);
  if (it == instruments_.end()) return;
  const auto [id, name] = it->second;
  if (type == "Quote") {
    auto ts = std::max(timestamp("bidTime"), timestamp("askTime"));
    const double bid = std::max(0.0, number(field(event, "bidPrice"), 0));
    const double ask = std::max(0.0, number(field(event, "askPrice"), 0));
    if ((bid > 0 && timestamp("bidTime") == 0) || (ask > 0 && timestamp("askTime") == 0)) ts = 0;
    // Streams publish repeated quotes too: they are observations, not snapshots.
    sink_.publish(md::OptionQuote{id, ts, bid, ask, std::max(0.0, number(field(event, "bidSize"), 0)),
                                                 std::max(0.0, number(field(event, "askSize"), 0))});
    const auto state = ts > 0 ? md::FeedState::Live : md::FeedState::Stale;
    if ((bid > 0 || ask > 0) && (!live_.contains(name) || live_.at(name) != state)) {
      live_[name] = state;
      sink_.publish(md::ProviderStatus{md::now(), state,
                    ts > 0 ? "tastytrade: real-time DXLink quotes (operator-selected millisecond units)" :
                             "tastytrade: quotes received; DXLink timestamp units unconfirmed", name});
    }
  } else if (type == "Greeks") {
    // dxFeed defines vega/rho per percentage point and theta per day.
    // Volatility is the Black-Scholes decimal. See docs/providers.md for sources.
    const double iv = number(field(event, "volatility"));
    sink_.publish(md::VendorGreeks{id, timestamp("time"), iv > 0 ? iv : missing,
                                   number(field(event, "delta")), number(field(event, "gamma")),
                                   number(field(event, "vega")), number(field(event, "theta")),
                                   number(field(event, "rho"))});
  } else if (type == "Summary") {
    const double oi = number(field(event, "openInterest"));
    // eventTime is the vendor's event creation time, not the OI effective date.
    if (oi >= 0) sink_.publish(md::OpenInterest{id, timestamp("eventTime"), oi});
  }
  if (type == "Trade" || type == "Summary") {
    // Trade is a last-trade snapshot, not a tape of distinct executions.
    // Summary's standard schema has no dayVolume; accept it only when supplied
    // by a negotiated extension. Never substitute prevDayVolume or trade size.
    const double volume = number(field(event, "dayVolume"));
    const auto ts = timestamp(type == "Trade" ? "time" : "eventTime");
    const double day = number(field(event, "dayId"));
    if (ts > 0 && volume >= 0 && volume == std::floor(volume) &&
        (!std::isfinite(day) || day == static_cast<double>(md::days_since_epoch(md::trading_date(ts)))))
      definitions_.volume(id, ts, volume, sink_);
  }
}

TastytradeProvider::TastytradeProvider(Options options) : options_(std::move(options)) {
  if (options_.sandbox) throw std::invalid_argument("tastytrade: sandbox has no market data; use a funded production account's OAuth credentials");
  (void)TastytradeApi(options_.credentials);
}

md::Capabilities TastytradeProvider::capabilities() const noexcept {
  return {.realtime = false, .realtime_plan_dependent = true, .open_interest = true, .vendor_greeks = true};
}

void TastytradeProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  stop();
  if (sessions.fetch_add(1) >= 5) {
    --sessions;
    throw std::runtime_error("tastytrade: five sessions already active in this process");
  }
  stopping_ = false;
  try { thread_ = std::thread(&TastytradeProvider::run, this, subscription, &sink); }
  catch (...) { --sessions; throw; }
}

void TastytradeProvider::stop() {
  { const std::lock_guard lock(mutex_); stopping_ = true; }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void TastytradeProvider::run(md::Subscription subscription, md::EventSink* sink) {
  struct SessionGuard { ~SessionGuard() { --sessions; } } guard;
  const auto status = [&](md::FeedState state, const std::string& message) {
    for (const auto& underlying : subscription.underlyings)
      sink->publish(md::ProviderStatus{md::now(), state, message, underlying});
  };
  try {
    TastytradeApi api(options_.credentials);
    net::HttpClient http;
    auto transport = net::make_websocket();
    DxlinkProtocol protocol(*transport);
    TastytradeMapper mapper(subscription, *sink, options_.timestamps_in_milliseconds);
    std::optional<TastytradeQuoteToken> token;
    unsigned attempts = 0;
    while (!stopping_) {
      try {
        status(md::FeedState::Connecting, "tastytrade: connecting; waiting for option quotes");
        mapper.reset_health();
        for (const auto& underlying : subscription.underlyings) {
          if (stopping_) break;
          const auto path = symbol_path(underlying);
          const auto chain = parse_tastytrade_chain(api.get(http, "/option-chains/" + path + "/nested", md::now(), &stopping_), underlying);
          const auto streamer = parse_tastytrade_streamer(api.get(http, "/instruments/equities/" + path, md::now(), &stopping_));
          mapper.chain(underlying, streamer, chain, md::new_york_time(md::now()).date);
        }
        if (stopping_) break;
        if (!token || md::now() >= token->refresh_at)
          token = parse_tastytrade_quote_token(api.get(http, "/api-quote-tokens", md::now(), &stopping_), md::now());
        transport->connect(token->url, &stopping_);
        auto subscriptions = mapper.subscriptions();
        protocol.begin(token->token, subscriptions, md::now());
        const auto chain_refresh = md::now() + 3600 * md::kNanosPerSecond;
        const auto connected = md::now();
        while (!stopping_ && md::now() < std::min(token->refresh_at, chain_refresh)) {
          if (const auto message = transport->receive(std::chrono::milliseconds(100))) {
            for (const auto& event : protocol.on_message(*message, md::now())) mapper.event(event);
            const auto updated = mapper.subscriptions();
            if (updated != subscriptions) {
              protocol.replace(updated, md::now());
              subscriptions = updated;
            }
          }
          protocol.tick(md::now());
          api.refresh(http, md::now(), &stopping_);
          if (protocol.ready() && md::now() - connected > md::kNanosPerMinute) attempts = 0;
        }
        transport->close();
        // Refresh chains hourly, or credentials earlier if their explicit expiry requires it.
      } catch (const std::exception& error) {
        transport->close();
        if (stopping_) break;
        // Never expose arbitrary server text, TLS URLs or parser diagnostics with tokens.
        const std::string message = error.what();
        if (message == "DXLink: quote token rejected; refresh required") token.reset();
        const bool safe = message.starts_with("tastytrade:") || message.starts_with("DXLink:");
        status(md::FeedState::Error, safe ? message : "tastytrade: transport or response failure");
        if (++attempts >= 8) break;
        std::unique_lock lock(mutex_);
        if (wake_.wait_for(lock, DxlinkProtocol::backoff(attempts), [this] { return stopping_.load(); })) break;
      }
    }
    transport->close();
    status(md::FeedState::Stopped, attempts >= 8 ? "tastytrade: retry limit reached; restart to reconnect" : "tastytrade: stopped");
  } catch (const std::exception&) {
    status(md::FeedState::Error, "tastytrade: worker initialization failed");
    status(md::FeedState::Stopped, "tastytrade: stopped");
  }
}
}  // namespace openport::providers
