#include "openport/providers/massive.hpp"

// simdjson 4.6 names std::ranges::input_range under C++20 without including <ranges>.
#include <ranges>
#include <simdjson.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "openport/md/contract.hpp"
#include "openport/net/http.hpp"

namespace openport::providers {
namespace {

using simdjson::ondemand::object;
using simdjson::ondemand::value;

double as_double(value v) {
  double out = 0.0;
  if (v.get_double().get(out) != simdjson::SUCCESS || !std::isfinite(out)) return 0.0;
  return out;
}

std::string as_string(value v) {
  std::string_view out;
  if (v.get_string().get(out) != simdjson::SUCCESS) return {};
  return std::string(out);
}

md::Timestamp as_nanos(value v) {
  std::int64_t out = 0;
  if (v.get_int64().get(out) == simdjson::SUCCESS) return out;
  return static_cast<md::Timestamp>(as_double(v));
}

// Fields are visited in whatever order Massive sends them, and any may be missing.
MassiveContract parse_contract(object result, MassivePage& page) {
  MassiveContract c;
  for (auto field : result) {
    const std::string_view key = field.unescaped_key();
    value v = field.value();
    object nested;
    if ((key == "details" || key == "greeks" || key == "last_quote" || key == "underlying_asset" || key == "day") &&
        v.get_object().get(nested) != simdjson::SUCCESS) {
      continue;  // Missing objects describe unavailable data, not a failed snapshot.
    }
    if (key == "details") {
      for (auto detail : nested) {
        const std::string_view name = detail.unescaped_key();
        if (name == "ticker") {
          c.symbol = as_string(detail.value());
          if (c.symbol.starts_with("O:")) c.symbol.erase(0, 2);
        } else if (name == "exercise_style") {
          c.european = as_string(detail.value()) == "european";
        } else if (name == "shares_per_contract") {
          c.multiplier = as_double(detail.value());
        }
      }
    } else if (key == "greeks") {
      for (auto greek : nested) {
        const std::string_view name = greek.unescaped_key();
        const double x = as_double(greek.value());
        if (name == "delta") c.delta = x;
        if (name == "gamma") c.gamma = x;
        if (name == "theta") c.theta = x;
        if (name == "vega") c.vega = x;
      }
    } else if (key == "implied_volatility") {
      c.iv = as_double(v);
    } else if (key == "last_quote") {
      c.has_quote = true;
      for (auto quote : nested) {
        const std::string_view name = quote.unescaped_key();
        if (name == "bid") c.bid = as_double(quote.value());
        if (name == "ask") c.ask = as_double(quote.value());
        if (name == "bid_size") c.bid_size = as_double(quote.value());
        if (name == "ask_size") c.ask_size = as_double(quote.value());
        if (name == "last_updated") c.quote_ts = as_nanos(quote.value());
        if (name == "timeframe") c.realtime = as_string(quote.value()) == "REAL-TIME";
      }
    } else if (key == "day") {
      for (auto day : nested) {
        const std::string_view name = day.unescaped_key();
        if (name == "volume") {
          double volume = 0.0;
          if (day.value().get_double().get(volume) == simdjson::SUCCESS &&
              std::isfinite(volume) && volume >= 0.0) c.volume = volume;
        } else if (name == "last_updated") {
          c.volume_ts = as_nanos(day.value());
        }
      }
    } else if (key == "open_interest") {
      c.has_open_interest = true;
      c.open_interest = as_double(v);
    } else if (key == "underlying_asset") {
      for (auto asset : nested) {
        const std::string_view name = asset.unescaped_key();
        // Index underlyings (I:SPX) report their level as "value" rather than "price".
        if (name == "price") page.underlying_price = as_double(asset.value());
        if (name == "value" && page.underlying_price <= 0.0) page.underlying_price = as_double(asset.value());
        if (name == "last_updated") page.underlying_ts = as_nanos(asset.value());
      }
    }
  }
  return c;
}

}  // namespace

std::string massive_chain_url(std::string_view base_url, std::string_view underlying) {
  std::string url(base_url);
  url += "/v3/snapshot/options/";
  if (md::is_index_underlying(underlying)) url += "I:";
  url += underlying;
  url += "?limit=250";
  return url;
}

MassivePage parse_massive_chain_page(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);

  MassivePage page;
  std::string status;
  std::string error;
  bool has_results = false;
  for (auto field : doc.get_object()) {
    const std::string_view key = field.unescaped_key();
    if (key == "results") {
      has_results = true;
      for (object result : field.value().get_array()) {
        MassiveContract contract = parse_contract(result, page);
        if (!contract.symbol.empty()) page.contracts.push_back(std::move(contract));
      }
    } else if (key == "next_url") {
      page.next_url = as_string(field.value());
    } else if (key == "status") {
      status = as_string(field.value());
    } else if (key == "error" || key == "message") {
      error = as_string(field.value());
    }
  }
  if (status == "ERROR" || status == "NOT_AUTHORIZED") {
    throw std::runtime_error("Massive: " + (error.empty() ? status : error));
  }
  if (!has_results) throw std::runtime_error("Massive: missing results array; snapshot incomplete");
  return page;
}

MassiveProvider::MassiveProvider(Options options)
    : PollingProvider(options.poll_interval), options_(std::move(options)) {
  if (options_.api_key.empty()) {
    throw std::invalid_argument("massive: an API key is required (set MASSIVE_API_KEY)");
  }
}

md::Capabilities MassiveProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.realtime_plan_dependent = true;  // the feed status reports the actual entitlement
  caps.poll_interval = options_.poll_interval;
  caps.quotes = true;
  caps.trades = false;
  caps.open_interest = true;
  caps.vendor_greeks = true;
  caps.history = false;
  return caps;
}

std::string MassiveProvider::poll(net::HttpClient& http, const std::string& underlying,
                                  const md::Subscription& subscription, md::EventSink& sink) {
  // The key goes in a header rather than the URL, so it never lands in a log line.
  const net::Headers headers{{"Authorization", "Bearer " + options_.api_key}};
  std::vector<MassiveContract> contracts;
  double underlying_price = 0.0;
  md::Timestamp underlying_ts = 0;
  std::size_t pages = 0;
  const auto started = std::chrono::steady_clock::now();

  std::string url = massive_chain_url(options_.base_url, underlying);
  while (!url.empty()) {
    const net::HttpResponse response = http.get(url, headers, options_.timeout, cancellation());
    if (response.status == 401 || response.status == 403) {
      throw std::runtime_error("the API key was rejected, or the plan does not include options");
    }
    if (response.status == 429) {
      throw std::runtime_error("rate limited: the plan's request limit is too low to poll whole chains");
    }
    if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));

    MassivePage page = parse_massive_chain_page(response.body);
    if (page.underlying_price > 0.0) {
      underlying_price = page.underlying_price;
      underlying_ts = page.underlying_ts;
    }
    for (MassiveContract& contract : page.contracts) contracts.push_back(std::move(contract));
    url = std::move(page.next_url);
    if (++pages > 2000) throw std::runtime_error("pagination did not end");
  }

  check_cancelled();
  publish_chain(underlying, contracts, underlying_price, underlying_ts, subscription, sink);
  char summary[192];
  std::snprintf(
      summary, sizeof summary, "massive %s: %s, %zu options in %zu pages, %.0f ms",
      underlying.c_str(), realtime_ ? "live" : "delayed", contracts.size(), pages,
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
          .count());
  return summary;
}

void MassiveProvider::publish_chain(const std::string& underlying,
                                    const std::vector<MassiveContract>& contracts,
                                    double underlying_price, md::Timestamp underlying_ts,
                                    const md::Subscription& subscription, md::EventSink& sink) {
  // Without a price (a plan without the underlying's entitlement) no spot of 0 is
  // published over the last one, and a strike window keeps the last spot it had.
  if (underlying_price > 0.0) sink.publish(md::UnderlyingQuote{underlying, underlying_ts, 0.0, 0.0, underlying_price});
  auto& window_spot = window_spots_[underlying];
  if (underlying_price > 0.0) window_spot = underlying_price;
  const bool waiting = subscription.strike_window > 0 && window_spot <= 0.0;

  std::vector<std::pair<const MassiveContract*, md::OptionContract>> parsed;
  parsed.reserve(contracts.size());
  std::set<md::Date> expiries;
  bool any_quote = false;
  bool all_realtime = true;
  for (const MassiveContract& c : contracts) {
    std::optional<md::OptionContract> contract = md::parse_osi(c.symbol);
    if (!contract) continue;
    if (c.european) contract->style = pricing::ExerciseStyle::European;
    if (c.multiplier > 0.0) contract->multiplier = c.multiplier;
    expiries.insert(contract->expiry);
    if (c.has_quote) {
      any_quote = true;
      all_realtime = all_realtime && c.realtime;
    }
    parsed.emplace_back(&c, std::move(*contract));
  }
  realtime_ = any_quote && all_realtime;

  const ChainFilter filter(subscription, md::date_from_days(md::now() / md::kNanosPerDay),
                           window_spot, expiries);
  constexpr double kUnpublished = std::numeric_limits<double>::quiet_NaN();
  std::set<md::InstrumentId> seen;
  for (auto& [c, contract] : parsed) {
    if (!publisher_.known(c->symbol) && (waiting || !filter.admits(contract))) continue;
    check_cancelled();
    const md::InstrumentId id = publisher_.define(c->symbol, std::move(contract), sink);
    seen.insert(id);
    if (c->has_quote) publisher_.quote(id, c->quote_ts, c->bid, c->ask, c->bid_size, c->ask_size, sink);
    else
      publisher_.quote(id, underlying_ts, 0.0, 0.0, 0.0, 0.0, sink);
    if (c->volume) publisher_.volume(id, c->volume_ts, *c->volume, sink);
    if (c->has_open_interest) publisher_.open_interest(id, c->quote_ts, c->open_interest, sink);
    publisher_.greeks(md::VendorGreeks{id, c->quote_ts, c->iv, c->delta, c->gamma, c->vega, c->theta,
                                       kUnpublished},
                      sink);
  }
  publisher_.finish(underlying, seen, underlying_ts, sink);
}

std::string massive_dividends_url(std::string_view base_url, std::string_view ticker, md::Date from) {
  return std::string(base_url) + "/stocks/v1/dividends?ticker=" + std::string(ticker) +
         "&ex_dividend_date.gte=" + md::format_date(from) + "&sort=ex_dividend_date.asc&limit=1000";
}

MassiveDividendPage parse_massive_dividends(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);
  MassiveDividendPage page;
  std::string status;
  std::string error;
  bool has_results = false;
  for (auto field : doc.get_object()) {
    const std::string_view key = field.unescaped_key();
    if (key == "results") {
      has_results = true;
      for (object result : field.value().get_array()) {
        MassiveDividend dividend;
        std::string date;
        for (auto item : result) {
          const std::string_view name = item.unescaped_key();
          if (name == "ticker") dividend.ticker = as_string(item.value());
          else if (name == "ex_dividend_date") date = as_string(item.value());
          else if (name == "cash_amount") dividend.cash_amount = as_double(item.value());
          else if (name == "currency") dividend.currency = as_string(item.value());
        }
        const auto midnight = md::parse_datetime(date + " 00:00:00", md::Zone::Utc);
        if (dividend.ticker.empty() || !midnight || !(dividend.cash_amount > 0)) continue;
        dividend.ex_date = md::date_from_days(*midnight / md::kNanosPerDay);
        page.dividends.push_back(std::move(dividend));
      }
    } else if (key == "next_url") {
      page.next_url = as_string(field.value());
    } else if (key == "status") {
      status = as_string(field.value());
    } else if (key == "error" || key == "message") {
      error = as_string(field.value());
    }
  }
  if (status == "ERROR" || status == "NOT_AUTHORIZED") throw std::runtime_error("Massive: " + (error.empty() ? status : error));
  if (!has_results) throw std::runtime_error("Massive dividends: missing results array");
  return page;
}

MassiveDividends::MassiveDividends(std::vector<std::string> tickers, Sink sink, Options options)
    : tickers_(std::move(tickers)), sink_(std::move(sink)), options_(std::move(options)) {
  if (options_.api_key.empty()) throw std::invalid_argument("massive dividends: an API key is required (set MASSIVE_API_KEY)");
}

void MassiveDividends::start() {
  stop();
  stopping_ = false;
  thread_ = std::thread(&MassiveDividends::run, this);
}

void MassiveDividends::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void MassiveDividends::run() {
  net::HttpClient http;
  while (!stopping_) {
    const md::Timestamp next = poll_once(http);
    const md::Timestamp wait = std::clamp<md::Timestamp>(next - options_.clock(), md::kNanosPerSecond, 3600 * md::kNanosPerSecond);
    std::unique_lock lock(wake_mutex_);
    if (wake_.wait_for(lock, std::chrono::nanoseconds(wait), [this] { return stopping_.load(); })) break;
  }
}

md::Timestamp MassiveDividends::poll_once(net::HttpClient& http) {
  const md::Timestamp now = options_.clock();
  if (due_ > now || stopping_) return due_;
  // The key goes in a header rather than the URL, so it never lands in a log line.
  const net::Headers headers{{"Authorization", "Bearer " + options_.api_key}};
  const auto from = md::date_from_days(md::days_since_epoch(md::new_york_time(now).date) - 31);
  std::string failures;
  for (const auto& ticker : tickers_) {
    try {
      std::vector<MassiveDividend> found;
      std::string url = massive_dividends_url(options_.base_url, ticker, from);
      for (int page = 0; !url.empty() && page < 10; ++page) {
        const auto response = http.get(url, headers, options_.timeout, &stopping_);
        if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));
        auto parsed = parse_massive_dividends(response.body);
        for (auto& dividend : parsed.dividends)
          if (dividend.ticker == ticker && (dividend.currency.empty() || dividend.currency == "USD"))
            found.push_back(std::move(dividend));
        url = std::move(parsed.next_url);
      }
      known_[ticker] = std::move(found);
    } catch (const std::exception& e) {
      if (stopping_) return now;
      failures += (failures.empty() ? "" : "\n") + ("massive dividends " + ticker + ": " + e.what());
    }
  }
  std::vector<MassiveDividend> all;
  for (const auto& [ticker, dividends] : known_) all.insert(all.end(), dividends.begin(), dividends.end());
  if (!known_.empty()) sink_(std::move(all));
  due_ = now + (failures.empty() ? options_.interval : options_.retry).count() * md::kNanosPerSecond;
  const std::lock_guard lock(error_mutex_);
  error_ = std::move(failures);
  return due_;
}

std::string MassiveDividends::error() const {
  const std::lock_guard lock(error_mutex_);
  return error_;
}

}  // namespace openport::providers
