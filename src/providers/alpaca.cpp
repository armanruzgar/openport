#include "openport/providers/alpaca.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <stdexcept>

#include "broker_json.hpp"

namespace openport::providers {
namespace {
using namespace broker;
constexpr std::string_view kOrigin = "https://data.alpaca.markets";
constexpr std::size_t kMaxPages = 100;

std::chrono::seconds retry_after(const net::HttpResponse& response) {
  const auto value = response.header("Retry-After");
  int seconds = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
  return std::chrono::seconds(error == std::errc{} && end == value.data() + value.size() ? seconds : 0);
}

/// A strike bound to the cent, rounded outward so the band never shrinks.
std::string strike_bound(double value, bool upper) {
  // Ignore binary noise such as 800 * 1.1 = 880.0000000000001.
  const double cents = upper ? std::ceil(value * 100 - 1e-6) : std::floor(value * 100 + 1e-6);
  char text[32];
  const auto [end, error] = std::to_chars(text, text + sizeof text, std::max(0.0, cents) / 100, std::chars_format::fixed, 2);
  if (error != std::errc{}) throw std::runtime_error("Alpaca: strike out of range");
  return std::string(text, end);
}

bool allowed_path(std::string_view path) {
  // Only the two snapshot endpoints, each with one encoded symbol component.
  path = path.substr(0, path.find('?'));
  for (const auto& [prefix, suffix] : {std::pair<std::string_view, std::string_view>{"/v2/stocks/", "/snapshot"},
                                       std::pair<std::string_view, std::string_view>{"/v1beta1/options/snapshots/", ""}}) {
    if (!path.starts_with(prefix) || !path.ends_with(suffix)) continue;
    const auto symbol = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (symbol.empty() || symbol.size() > 32) return false;
    return std::all_of(symbol.begin(), symbol.end(), [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    });
  }
  return false;
}
}  // namespace

AlpacaPage parse_alpaca_option_page(std::string_view json, std::string_view underlying) {
  const auto doc = parse(json);
  const auto& snapshots = field(doc, "snapshots");
  if (!snapshots.is_object() && !snapshots.is_null()) throw std::runtime_error("Alpaca: invalid option snapshots");
  AlpacaPage page;
  page.next_page_token = str(field(doc, "next_page_token"));
  if (snapshots.is_null()) return page;
  for (const auto& [symbol, item] : snapshots.items()) {
    auto contract = md::parse_osi(symbol);
    if (!contract || contract->underlying != underlying) throw std::runtime_error("Alpaca: option symbol does not match the underlying");
    AlpacaOption option;
    option.contract = *contract;
    const auto& quote = field(item, "latestQuote");
    option.ts = zoned_time(field(quote, "t"));
    option.bid = std::max(0.0, number(field(quote, "bp"), 0));
    option.ask = std::max(0.0, number(field(quote, "ap"), 0));
    option.bid_size = std::max(0.0, number(field(quote, "bs"), 0));
    option.ask_size = std::max(0.0, number(field(quote, "as"), 0));
    if ((option.bid > 0 || option.ask > 0) && option.ts == 0)
      throw std::runtime_error("Alpaca: quote timestamp missing");
    const auto& bar = field(item, "dailyBar");
    const auto trade_ts = zoned_time(field(field(item, "latestTrade"), "t"));
    const auto bar_ts = zoned_time(field(bar, "t"));
    // The daily bar's volume is cumulative for its trading date; it takes the time
    // of that date's latest trade, never the receipt time.
    if (bar_ts > 0 && trade_ts > 0 && md::trading_date(bar_ts) == md::trading_date(trade_ts)) {
      option.volume = number(field(bar, "v"), -1);
      option.volume_ts = trade_ts;
    }
    // Delta and gamma are price derivatives. IV and the scaling of vega, theta and
    // rho are not documented, so they stay missing.
    const auto& greeks = field(item, "greeks");
    if (greeks.is_object())
      option.greeks = md::VendorGreeks{0, 0, missing, number(field(greeks, "delta")),
                                       number(field(greeks, "gamma")), missing, missing, missing};
    page.options.push_back(std::move(option));
  }
  return page;
}

md::UnderlyingQuote parse_alpaca_stock_snapshot(std::string_view json, const std::string& symbol) {
  const auto doc = parse(json);
  if (doc.contains("symbol") && str(field(doc, "symbol")) != symbol) throw std::runtime_error("Alpaca: stock snapshot mismatch");
  const auto& trade = field(doc, "latestTrade");
  const auto trade_ts = zoned_time(field(trade, "t"));
  const double last = number(field(trade, "p"), 0);
  if (last > 0 && trade_ts > 0) return {symbol, trade_ts, 0, 0, last};
  const auto& quote = field(doc, "latestQuote");
  const auto quote_ts = zoned_time(field(quote, "t"));
  if (quote_ts == 0) return {symbol, 0, 0, 0, 0};
  return {symbol, quote_ts, std::max(0.0, number(field(quote, "bp"), 0)), std::max(0.0, number(field(quote, "ap"), 0)), 0};
}

AlpacaProvider::AlpacaProvider(Options options)
    : PollingProvider(std::chrono::seconds(5)), options_(std::move(options)),
      budget_(options_.requests_per_minute, md::kNanosPerMinute) {
  if (options_.key_id.empty() || options_.secret_key.empty())
    throw std::invalid_argument("alpaca: set APCA_API_KEY_ID and APCA_API_SECRET_KEY");
  if (options_.stock_feed != "iex" && options_.stock_feed != "sip" && options_.stock_feed != "delayed_sip")
    throw std::invalid_argument("alpaca: stock_feed must be iex, sip or delayed_sip");
  if (options_.poll_seconds.count() < 0) throw std::invalid_argument("alpaca: poll interval must be positive");
}

md::Capabilities AlpacaProvider::capabilities() const noexcept {
  return {.realtime = false, .realtime_plan_dependent = true, .poll_interval = poll_interval(),
          .trades = false, .open_interest = false, .vendor_greeks = true};
}

std::chrono::seconds AlpacaProvider::poll_interval() const noexcept {
  return std::chrono::seconds(interval_seconds_.load());
}

std::string AlpacaProvider::get(net::HttpClient& http, const std::string& path) {
  if (!allowed_path(path)) throw std::logic_error("alpaca: forbidden endpoint");
  for (auto delay = budget_.wait(options_.clock()); delay.count() > 0; delay = budget_.wait(options_.clock())) {
    if (!sleep(delay)) check_cancelled();
  }
  check_cancelled();
  budget_.sent(options_.clock());
  const auto result = http.get_direct(std::string(kOrigin) + path,
                                      {{"APCA-API-KEY-ID", options_.key_id}, {"APCA-API-SECRET-KEY", options_.secret_key},
                                       {"Accept", "application/json"}},
                                      std::chrono::seconds(30), cancellation());
  if (result.status == 429) {
    budget_.throttled(options_.clock(), retry_after(result));
    throw std::runtime_error("rate limited; backing off");
  }
  if (result.status == 403) throw std::runtime_error("HTTP 403; check that your subscription includes this feed");
  if (result.status != 200) throw std::runtime_error("HTTP " + std::to_string(result.status));
  return result.body;
}

std::string AlpacaProvider::poll(net::HttpClient& http, const std::string& underlying,
                                 const md::Subscription& sub, md::EventSink& sink) {
  if (md::is_index_underlying(underlying)) throw std::runtime_error("Alpaca has no index options; use an ETF such as SPY");
  const auto symbol = symbol_path(underlying);
  const auto spot = parse_alpaca_stock_snapshot(get(http, "/v2/stocks/" + symbol + "/snapshot?feed=" + options_.stock_feed), underlying);
  const auto today = md::new_york_time(spot.ts > 0 ? spot.ts : options_.clock()).date;
  const double price = spot.last > 0 ? spot.last : (spot.bid > 0 && spot.ask > 0 ? (spot.bid + spot.ask) / 2 : 0);
  if (sub.strike_window > 0 && price <= 0) throw std::runtime_error("strike filter needs an underlying price");
  auto query = "/v1beta1/options/snapshots/" + symbol + "?feed=" +
               (options_.feed == Feed::Opra ? "opra" : "indicative") + "&limit=1000&expiration_date_gte=" + md::format_date(today);
  if (sub.strike_window > 0) {
    // Ask only for strikes near spot, widened to keep every contract already defined.
    double low = price * (1 - sub.strike_window), high = price * (1 + sub.strike_window);
    if (const auto it = defined_strikes_.find(underlying); it != defined_strikes_.end()) {
      low = std::min(low, it->second.first);
      high = std::max(high, it->second.second);
    }
    query += "&strike_price_gte=" + strike_bound(low, false) + "&strike_price_lte=" + strike_bound(high, true);
  }
  std::vector<AlpacaOption> options;
  std::string token;
  std::size_t pages = 0;
  do {
    if (++pages > kMaxPages) throw std::runtime_error("more than 100 pages; narrow --window");
    auto page = parse_alpaca_option_page(get(http, token.empty() ? query : query + "&page_token=" + form_value(token)), underlying);
    std::move(page.options.begin(), page.options.end(), std::back_inserter(options));
    token = std::move(page.next_page_token);
  } while (!token.empty());
  request_counts_[underlying] = 1 + pages;
  std::size_t cycle = 0;
  for (const auto& name : sub.underlyings) cycle += request_counts_.contains(name) ? request_counts_.at(name) : 1 + pages;
  if (sub.underlyings.empty()) cycle = 1 + pages;
  const auto required = budget_.interval(cycle);
  if (options_.poll_seconds.count() > 0 && options_.poll_seconds < required)
    throw std::runtime_error("poll_seconds is below the request budget; needs at least " + std::to_string(required.count()));
  interval_seconds_ = static_cast<int>(options_.poll_seconds.count() > 0 ? options_.poll_seconds.count()
                                                                         : std::max<long long>(5, required.count()));
  std::set<md::Date> expiries;
  md::Timestamp market_time = spot.ts;
  for (const auto& option : options) {
    expiries.insert(option.contract.expiry);
    market_time = std::max(market_time, option.ts);
  }
  if (market_time <= 0) throw std::runtime_error("snapshot has no known market timestamp");
  std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) {
    return a.contract.osi_symbol() < b.contract.osi_symbol();
  });
  const ChainFilter filter(sub, today, price, expiries);
  check_cancelled();
  if (spot.ts > 0) sink.publish(spot);
  std::set<md::InstrumentId> seen;
  for (const auto& option : options) {
    const auto osi = option.contract.osi_symbol();
    if (!publisher_.known(osi) && !filter.admits(option.contract)) continue;
    const auto id = publisher_.define(osi, option.contract, sink);
    const auto [strikes, inserted] = defined_strikes_.try_emplace(underlying, option.contract.strike, option.contract.strike);
    if (!inserted) {
      strikes->second.first = std::min(strikes->second.first, option.contract.strike);
      strikes->second.second = std::max(strikes->second.second, option.contract.strike);
    }
    seen.insert(id);
    publisher_.quote(id, option.ts, option.bid, option.ask, option.bid_size, option.ask_size, sink);
    if (option.volume >= 0) publisher_.volume(id, option.volume_ts, option.volume, sink);
    if (option.greeks) {
      auto greeks = *option.greeks;
      greeks.id = id;
      publisher_.greeks(greeks, sink);
    }
  }
  publisher_.finish(underlying, seen, market_time, sink);
  budget_.succeeded();
  if (options_.feed == Feed::Opra) {
    state_ = md::FeedState::Live;
    return "Alpaca OPRA: real-time option snapshots";
  }
  // The free feed's quotes are derived from OPRA, not the NBBO itself.
  state_ = md::FeedState::Delayed;
  return "Alpaca indicative feed: derived option quotes, not the OPRA NBBO";
}

}  // namespace openport::providers
