#include "openport/providers/thetadata.hpp"

// simdjson 4.6 names std::ranges::input_range under C++20 without including <ranges>.
#include <ranges>
#include <simdjson.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "openport/md/contract.hpp"
#include "openport/net/http.hpp"

namespace openport::providers {
namespace {

using simdjson::ondemand::value;

double as_double(value v, double fallback = 0.0) {
  double out = 0.0;
  if (v.get_double().get(out) != simdjson::SUCCESS || !std::isfinite(out)) return fallback;
  return out;
}

std::string_view as_view(value v) {
  std::string_view out;
  if (v.get_string().get(out) != simdjson::SUCCESS) return {};
  return out;
}

/// "2026-10-05" or "20261005".
bool parse_date(std::string_view text, md::Date& out) {
  auto number = [&](std::size_t pos, std::size_t len, int& n) {
    const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + pos + len, n);
    return ec == std::errc() && ptr == text.data() + pos + len;
  };
  if (text.size() == 10 && text[4] == '-' && text[7] == '-') {
    return number(0, 4, out.year) && number(5, 2, out.month) && number(8, 2, out.day);
  }
  if (text.size() == 8) return number(0, 4, out.year) && number(4, 2, out.month) && number(6, 2, out.day);
  return false;
}

/// Identity of a contract within one underlying's snapshot.
using Key = std::tuple<std::string, md::Date, long long, pricing::OptionType>;

Key key_of(const ThetaRow& row) {
  return {row.root, row.expiry, std::llround(row.strike * 1000.0), row.type};
}

}  // namespace

std::vector<ThetaRow> parse_theta_rows(std::string_view ndjson) {
  std::vector<ThetaRow> rows;
  if (ndjson.find_first_not_of(" \r\n\t") == std::string_view::npos) return rows;

  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(ndjson);
  simdjson::ondemand::document_stream stream = parser.iterate_many(padded);
  for (auto doc : stream) {
    ThetaRow row;
    bool has_right = false;
    for (auto field : doc.get_object()) {
      const std::string_view key = field.unescaped_key();
      value v = field.value();
      if (key == "symbol") {
        row.root = std::string(as_view(v));
      } else if (key == "expiration") {
        if (!parse_date(as_view(v), row.expiry)) throw std::runtime_error("ThetaData: bad expiration");
      } else if (key == "strike") {
        row.strike = as_double(v);
      } else if (key == "right") {
        const std::string_view right = as_view(v);
        has_right = true;
        if (right == "call" || right == "C" || right == "CALL") {
          row.type = pricing::OptionType::Call;
        } else if (right == "put" || right == "P" || right == "PUT") {
          row.type = pricing::OptionType::Put;
        } else {
          has_right = false;
        }
      } else if (key == "timestamp") {
        row.ts = md::parse_datetime(as_view(v), md::Zone::NewYork).value_or(0);
      } else if (key == "bid") {
        row.bid = as_double(v);
      } else if (key == "ask") {
        row.ask = as_double(v);
      } else if (key == "bid_size") {
        row.bid_size = as_double(v);
      } else if (key == "ask_size") {
        row.ask_size = as_double(v);
      } else if (key == "volume") {
        double volume = 0.0;
        if (v.get_double().get(volume) == simdjson::SUCCESS &&
            std::isfinite(volume) && volume >= 0.0) row.volume = volume;
      } else if (key == "open_interest") {
        row.open_interest = as_double(v);
      } else if (key == "implied_vol") {
        row.implied_vol = as_double(v, std::numeric_limits<double>::quiet_NaN());
      } else if (key == "delta") {
        row.delta = as_double(v, std::numeric_limits<double>::quiet_NaN());
      } else if (key == "vega") {
        row.vega = as_double(v, std::numeric_limits<double>::quiet_NaN()) / 100.0;
      } else if (key == "rho") {
        row.rho = as_double(v, std::numeric_limits<double>::quiet_NaN()) / 100.0;
      } else if (key == "underlying_price") {
        row.underlying_price = as_double(v);
      } else if (key == "underlying_timestamp") {
        row.underlying_ts = md::parse_datetime(as_view(v), md::Zone::NewYork).value_or(0);
      }
    }
    if (!row.root.empty() && has_right && row.strike > 0.0) rows.push_back(std::move(row));
  }
  return rows;
}

md::Capabilities ThetaDataProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.realtime = true;
  caps.poll_interval = options_.poll_interval;
  caps.quotes = true;
  caps.trades = false;
  caps.open_interest = true;
  caps.vendor_greeks = true;
  caps.history = true;
  return caps;
}

std::vector<ThetaRow> ThetaDataProvider::fetch(net::HttpClient& http, std::string_view endpoint,
                                               const std::string& root) {
  check_cancelled();
  const std::string url = options_.base_url + "/v3/option/snapshot/" + std::string(endpoint) +
                          "?symbol=" + root + "&expiration=*&format=ndjson";
  net::HttpResponse response;
  try {
    response = http.get(url, {}, options_.timeout, cancellation());
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string(error.what()) +
                             " (is Theta Terminal running? java -jar ThetaTerminalv3.jar)");
  }
  if (response.status != 200) {
    throw std::runtime_error(std::string(endpoint) + ": HTTP " + std::to_string(response.status) +
                             " " + response.body.substr(0, 200));
  }
  return parse_theta_rows(response.body);
}

std::string ThetaDataProvider::poll(net::HttpClient& http, const std::string& underlying,
                                    const md::Subscription& subscription, md::EventSink& sink) {
  const auto started = std::chrono::steady_clock::now();
  int& polls = polls_[underlying];
  const bool refresh_open_interest = polls == 0;
  polls = (polls + 1) % std::max(1, options_.open_interest_every);

  std::vector<ThetaRow> quotes;
  std::vector<ThetaRow> implied_vols;
  std::vector<ThetaRow> open_interest;
  std::string degraded;
  auto auxiliary = [&](std::string_view endpoint, const std::string& root,
                       std::vector<ThetaRow>& rows) {
    try {
      for (ThetaRow& row : fetch(http, endpoint, root)) rows.push_back(std::move(row));
      return true;
    } catch (const std::exception& error) {
      check_cancelled();
      degraded += "; degraded " + root + " " + std::string(endpoint) + ": " + error.what();
      return false;
    }
  };
  for (const std::string& root : md::option_roots(underlying)) {
    for (ThetaRow& row : fetch(http, "quote", root)) quotes.push_back(std::move(row));
    if (!auxiliary("greeks/first_order", root, implied_vols))
      auxiliary("greeks/implied_volatility", root, implied_vols);
    if (refresh_open_interest) {
      auxiliary("open_interest", root, open_interest);
    }
  }
  check_cancelled();
  publish_chain(underlying, quotes, implied_vols, open_interest, subscription, sink);

  char summary[160];
  std::snprintf(summary, sizeof summary, "thetadata %s: %zu quotes, %zu IVs in %.0f ms",
                underlying.c_str(), quotes.size(), implied_vols.size(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                    .count());
  return std::string(summary) + degraded;
}

void ThetaDataProvider::publish_chain(const std::string& underlying,
                                      const std::vector<ThetaRow>& quotes,
                                      const std::vector<ThetaRow>& implied_vols,
                                      const std::vector<ThetaRow>& open_interest,
                                      const md::Subscription& subscription, md::EventSink& sink) {
  // ThetaData reports the underlying's price alongside each implied volatility.
  double spot = 0.0;
  md::Timestamp spot_ts = 0;
  for (const ThetaRow& row : implied_vols) {
    if (row.underlying_price > 0.0 && row.underlying_ts > 0 && row.underlying_ts >= spot_ts) {
      spot = row.underlying_price;
      spot_ts = row.underlying_ts;
    }
  }
  if (spot > 0.0) sink.publish(md::UnderlyingQuote{underlying, spot_ts, 0.0, 0.0, spot});
  // The IV snapshot may fail or carry no clock: a strike window then uses the last
  // spot, and with none yet defines nothing new rather than the whole chain.
  auto& window_spot = window_spots_[underlying];
  if (spot > 0.0) window_spot = spot;
  const bool waiting = subscription.strike_window > 0 && window_spot <= 0.0;

  std::set<md::Date> expiries;
  for (const ThetaRow& row : quotes) expiries.insert(row.expiry);
  const ChainFilter filter(subscription, md::date_from_days(md::now() / md::kNanosPerDay), window_spot,
                           expiries);

  auto contract_of = [](const ThetaRow& row) {
    md::OptionContract contract;
    contract.root = row.root;
    const md::RootConventions conventions = md::conventions_for_root(row.root);
    contract.underlying = conventions.underlying;
    contract.style = conventions.style;
    contract.settlement = conventions.settlement;
    contract.standard = conventions.standard;
    contract.expiry = row.expiry;
    contract.strike = row.strike;
    contract.type = row.type;
    return contract;
  };

  std::map<Key, md::InstrumentId> ids;
  std::set<md::InstrumentId> seen;
  // The snapshot's time is its data's, never the wall clock, which a delayed plan's
  // data runs behind: stamped on it, the snapshot would move the market clock ahead.
  md::Timestamp latest = spot_ts;
  for (const ThetaRow& row : quotes) latest = std::max(latest, row.ts);
  auto& snapshot_time = snapshot_times_[underlying];
  snapshot_time = std::max(snapshot_time, latest);
  for (const ThetaRow& row : quotes) {
    md::OptionContract contract = contract_of(row);
    const std::string symbol = contract.osi_symbol();
    if (!publisher_.known(symbol) && (waiting || !filter.admits(contract))) continue;
    check_cancelled();
    const md::InstrumentId id = publisher_.define(symbol, std::move(contract), sink);
    seen.insert(id);
    ids.emplace(key_of(row), id);
    publisher_.quote(id, row.ts, row.bid, row.ask, row.bid_size, row.ask_size, sink);
  }
  for (const auto* rows : {&quotes, &implied_vols, &open_interest}) {
    for (const auto& row : *rows) {
      const auto it = ids.find(key_of(row));
      if (it != ids.end() && row.volume) publisher_.volume(it->second, row.ts, *row.volume, sink);
    }
  }
  constexpr double kUnpublished = std::numeric_limits<double>::quiet_NaN();
  for (const ThetaRow& row : implied_vols) {
    const auto it = ids.find(key_of(row));
    if (it == ids.end()) continue;
    // ThetaData documents vega/rho / 100. Its theta time basis is unspecified;
    // first_order does not supply gamma. Preserve those fields as unknown.
    publisher_.greeks(md::VendorGreeks{it->second, row.ts, row.implied_vol, row.delta,
                                       kUnpublished, row.vega, kUnpublished, row.rho},
                      sink);
  }
  for (const ThetaRow& row : open_interest) {
    const auto it = ids.find(key_of(row));
    if (it != ids.end()) publisher_.open_interest(it->second, row.ts, row.open_interest, sink);
  }
  publisher_.finish(underlying, seen, snapshot_time, sink);
}

}  // namespace openport::providers
