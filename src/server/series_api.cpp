#include "series_api.hpp"
#include "metric_cache.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include "openport/analytics/volatility_history.hpp"
#include "openport/providers/demo.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
json number(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
std::vector<md::Bar> daily_bars(const MetricsSource& source, const std::string& symbol) {
  return source.candles() ? source.candles()->daily_history(symbol) : std::vector<md::Bar>{};
}
std::vector<analytics::VolatilityDay> own_days(const MetricsSource& source, const std::string& symbol, md::Timestamp as_of) {
  std::vector<analytics::VolatilityDay> days;
  if (source.series())
    for (const auto& row : source.series()->rows(symbol, 0, as_of, true))
      days.push_back({md::new_york_time(row.time).date, row.values[3], row.values[8]});
  return days;
}
std::optional<md::Timestamp> timestamp(const std::string& value, bool end) {
  std::int64_t seconds = 0;
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
  if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && seconds >= 0 &&
      seconds <= std::numeric_limits<md::Timestamp>::max() / md::kNanosPerSecond)
    return seconds * md::kNanosPerSecond;
  if (value.size() == 10) return md::parse_datetime(value + (end ? " 23:59:59" : " 00:00:00"), md::Zone::NewYork);
  return md::parse_datetime(value, md::Zone::Utc);
}
md::Timestamp close_time(md::Date date) { return md::new_york_to_utc(date, md::regular_close_hour(date), 0); }
json scanner_metrics(const MetricsSource& source, const std::string& symbol,
                     const std::shared_ptr<const analytics::UnderlyingMetrics>& m) {
  const auto v = cached_volatility(m);
  const auto& iv = std::isfinite(v.mfiv[1].vol) ? v.mfiv[1] : v.atm[1];
  const auto proxy = analytics::iv_proxy(symbol);
  const auto rank = analytics::iv_rank(analytics::historical_iv(own_days(source, symbol, m->as_of), daily_bars(source, proxy), proxy, m->as_of), iv.vol, m->as_of);
  std::vector<analytics::HistoricalValue> skew;
  if (source.series()) for (const auto& sample : source.series()->rows(symbol, 0, m->as_of, true))
    skew.push_back({md::new_york_time(sample.time).date, sample.values[12], "own"});
  const auto rr = analytics::history_rank(skew, v.skew25.rr, m->as_of);
  auto bars = daily_bars(source, symbol);
  std::erase_if(bars, [&](const auto& bar) {
    const auto date = md::new_york_time(bar.start).date;
    return close_time(date) > m->as_of || !md::market_session(md::new_york_to_utc(date, 9, 30)).open;
  });
  const auto rv = analytics::realized_window(bars, 21).close_to_close.vol;
  const auto moves = analytics::implied_moves(*m, v);
  return {{"as_of", md::format_timestamp(m->as_of)}, {"version", m->version}, {"iv30", number(iv.vol)},
      {"iv_basis", std::isfinite(v.mfiv[1].vol) ? "own_mfiv" : std::isfinite(iv.vol) ? "own_atm" : "missing"},
      {"iv_rank", number(rank.rank)}, {"iv_percentile", number(rank.percentile)}, {"history_sessions", rank.sessions},
      {"proxy_sessions", rank.proxy_sessions}, {"proxy_name", proxy.empty() ? json(nullptr) : json(proxy)},
      {"vrp", number(analytics::variance_risk_premium(iv, rv).spread)}, {"rr25", number(v.skew25.rr)},
      {"realized_as_of", bars.empty() ? json(nullptr) : json(md::format_date(md::new_york_time(bars.back().start).date))},
      {"rr25_rank", number(rr.rank)}, {"rr25_sessions", rr.sessions}, {"term_ratio", number(v.ratio9_30)},
      {"today_percent", number(moves.today_percent)}, {"today_points", number(moves.today_points)},
      {"truncated", iv.truncated || v.mfiv[0].truncated || moves.today_truncated},
      {"proxy", iv.proxy || !std::isfinite(v.mfiv[1].vol) || v.skew_proxy || moves.today_proxy},
      {"reason", iv.reason.empty() ? json(nullptr) : json(iv.reason)}};
}
}

json series_status_json(const MetricsSource& source) {
  if (!source.series()) return {{"enabled", false}, {"directory", nullptr}, {"rows_today", 0}, {"last_write", nullptr}, {"last_error", nullptr}};
  const auto state = source.series()->status(source.wall_time());
  return {{"enabled", true}, {"directory", state.directory}, {"rows_today", state.rows_today},
          {"last_write", state.last_write > 0 ? json(md::format_timestamp(state.last_write)) : json(nullptr)},
          {"last_error", state.last_error.empty() ? json(nullptr) : json(state.last_error)}};
}

ApiResponse volatility_scanner_response(const MetricsSource& source) {
  json rows = json::array();
  const auto status = source.status();
  for (const auto& symbol : source.symbols()) {
    json row{{"symbol", symbol}, {"as_of", nullptr}, {"realized_as_of", nullptr}, {"version", 0}, {"iv_basis", "missing"},
        {"history_sessions", 0}, {"rr25_sessions", 0}, {"proxy_sessions", 0}, {"proxy_name", nullptr},
        {"truncated", false}, {"proxy", false}, {"reason", "no_snapshot"}};
    for (const auto* field : {"iv30", "iv_rank", "iv_percentile", "vrp", "rr25", "rr25_rank", "term_ratio", "today_percent", "today_points"}) row[field] = nullptr;
    const auto health = status.underlyings.find(symbol);
    row["feed_state"] = std::string(md::to_string(health == status.underlyings.end() ? status.feed_state : health->second.state));
    if (const auto m = source.metrics(symbol)) {
      // Rows are recomputed only when the symbol publishes a new snapshot; polling tabs reuse them.
      const auto cache = surface_cache(m);
      std::optional<json> cached;
      {
        const std::lock_guard lock(cache->mutex);
        if (const auto found = cache->scanner_rows.find(&source); found != cache->scanner_rows.end()) cached = found->second;
      }
      if (!cached) {
        cached = scanner_metrics(source, symbol, m);
        const std::lock_guard lock(cache->mutex);
        cache->scanner_rows[&source] = *cached;
      }
      row.update(*cached);
    }
    rows.push_back(std::move(row));
  }
  return {200, json{{"rows", rows}, {"simulated", providers::simulated_provider(status.provider)},
      {"provider", status.provider}, {"units", "vol_points"}}.dump()};
}

void add_volatility_history(json& response, const MetricsSource& source, const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  const auto& m = *metrics;
  const auto volatility = cached_volatility(metrics);
  const auto proxy_name = analytics::iv_proxy(m.symbol);
  const auto own = own_days(source, m.symbol, m.as_of);
  const auto proxy_bars = daily_bars(source, proxy_name);
  const auto history = analytics::historical_iv(own, proxy_bars, proxy_name, m.as_of);
  const auto& mfiv = response["mfiv"]["constant"][1]["vol"];
  const auto& atm = response["atm"]["constant"][1]["vol"];
  const double current = std::isfinite(volatility.mfiv[1].vol) ? volatility.mfiv[1].vol : volatility.atm[1].vol;
  const auto rank = analytics::iv_rank(history, current, m.as_of);
  response["iv_rank"] = number(rank.rank);
  response["iv_percentile"] = number(rank.percentile);
  response["iv_z_score"] = number(rank.z_score);
  response["history_sessions"] = rank.sessions;
  response["history_basis"] = {{"current", mfiv.is_number() ? "own_mfiv" : atm.is_number() ? "own_atm" : "missing"},
      {"own_sessions", rank.own_sessions}, {"proxy_sessions", rank.proxy_sessions}, {"window", 252}};
  response["proxy"] = {{"name", proxy_name.empty() ? json(nullptr) : json(proxy_name)},
      {"used", rank.proxy_sessions > 0}, {"start", rank.proxy_start.empty() ? json(nullptr) : json(rank.proxy_start)}};
  json observations = json::array();
  // Rank inputs are returned individually so provenance is inspectable.
  auto cutoff = md::new_york_time(m.as_of).date;
  if (close_time(cutoff) > m.as_of || !md::market_session(md::new_york_to_utc(cutoff, 9, 30)).open)
    cutoff = md::previous_business_day(cutoff);
  for (int i = 1; i < 252; ++i) cutoff = md::previous_business_day(cutoff);
  for (const auto& value : history)
    if (value.date >= cutoff) observations.push_back({{"date", md::format_date(value.date)}, {"value", value.vol}, {"source", value.source}});
  response["history_values"] = observations;
  const auto rows = source.series() ? source.series()->rows(m.symbol, 0, m.as_of, true) : std::vector<SeriesRow>{};
  const auto sample = series_row(metrics);
  response["history_ranks"] = json::object();
  for (std::size_t i = 0; i < kSeriesFields.size(); ++i) {
    std::vector<analytics::HistoricalValue> values;
    for (const auto& row : rows) values.push_back({md::new_york_time(row.time).date, row.values[i], "own"});
    const auto field_rank = analytics::history_rank(values, sample.values[i], m.as_of);
    response["history_ranks"][kSeriesFields[i]] = {{"current", number(sample.values[i])},
        {"rank", number(field_rank.rank)}, {"percentile", number(field_rank.percentile)},
        {"z_score", number(field_rank.z_score)}, {"sessions", field_rank.sessions}, {"window", 252}};
  }
  response["ex_post_vrp"] = nullptr;
  if (m.symbol != "SPX" && m.symbol != "SPY") return;
  const auto spx = own_days(source, "SPX", m.as_of);
  const auto implied = analytics::historical_iv(spx, daily_bars(source, "VIX"), "VIX", m.as_of, false);
  const auto realized = daily_bars(source, "SPX");
  const auto vrp = analytics::ex_post_vrp(implied, realized, m.as_of);
  json points = json::array(), summaries = json::array();
  for (const auto& point : vrp.points) {
    // The endpoint has the same ten-year bound as daily series.
    if (md::years_between(close_time(point.date), m.as_of) > 10.03) continue;
    points.push_back({{"date", md::format_date(point.date)}, {"end", md::format_date(point.end)},
        {"implied", number(point.implied)}, {"realized", number(point.realized)}, {"variance", number(point.variance)},
        {"vol_points", number(point.vol_points)}, {"source", point.source}});
  }
  for (const auto& summary : vrp.summaries)
    summaries.push_back({{"years", summary.years}, {"observations", summary.observations}, {"proxy_observations", summary.proxy_observations},
        {"mean_variance", number(summary.mean_variance)}, {"median_variance", number(summary.median_variance)},
        {"mean_vol_points", number(summary.mean_vol_points)}, {"median_vol_points", number(summary.median_vol_points)},
        {"positive_share", number(summary.positive_share)}, {"from", summary.from}, {"to", summary.to}});
  response["ex_post_vrp"] = {{"proxy", m.symbol == "SPY"}, {"points", points}, {"summaries", summaries},
      {"implied_source", "SPX local 30d model-free IV; Cboe VIX before local history"},
      {"realized_source", "SPX CandleStore daily closes, following 21 sessions"},
      {"convention", "Overlapping 21-session months; annualized demeaned close-return sample variance; vol points = IV minus RV"}};
}

ApiResponse series_response(const MetricsSource& source, const std::string& symbol,
                            const std::map<std::string, std::string>& query) {
  // Errors share every other route's {"error": {"code", "message"}} shape.
  const auto bad = [](const std::string& message) { return api_error(400, "INVALID_REQUEST", message); };
  const auto symbols = source.symbols();
  // A finished replay's archive is its account alone: it keeps no market data.
  if (symbols.empty()) return api_error(404, "NOT_FOUND", "This source has no market data or metric series");
  if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) return api_error(404, "NOT_FOUND", "unknown underlying");
  for (const auto& [key, value] : query) {
    (void)value;
    if (key != "fields" && key != "interval" && key != "from" && key != "to") return bad("unknown series query parameter");
  }
  const auto interval = query.contains("interval") ? query.at("interval") : "1d";
  if (interval != "1m" && interval != "1d") return bad("interval must be 1m or 1d");
  const bool daily = interval == "1d";
  const auto metrics = source.metrics(symbol);
  md::Timestamp to = metrics ? metrics->as_of : source.wall_time();
  if (query.contains("to")) {
    const auto parsed = timestamp(query.at("to"), true);
    if (!parsed || *parsed < 0) return bad("to must be Unix seconds, an ISO timestamp or YYYY-MM-DD");
    to = *parsed;
  }
  md::Timestamp from = std::max<md::Timestamp>(0, to - (daily ? 366 : 1) * md::kNanosPerDay);
  if (query.contains("from")) {
    const auto parsed = timestamp(query.at("from"), false);
    if (!parsed || *parsed < 0) return bad("from must be Unix seconds, an ISO timestamp or YYYY-MM-DD");
    from = *parsed;
  }
  if (from > to || to - from > (daily ? 3660 : 7) * md::kNanosPerDay)
    return bad("series range must be ordered and at most 7 days for 1m or 3660 days for 1d");
  std::vector<std::string> fields;
  if (!query.contains("fields")) {
    for (auto name : kSeriesFields) fields.emplace_back(name);
    if (daily) { fields.emplace_back("proxy_iv30"); fields.emplace_back("rv21"); fields.emplace_back("vix"); fields.emplace_back("vix_minus_mfiv"); }
  } else {
    std::istringstream input(query.at("fields"));
    for (std::string name; std::getline(input, name, ',');) {
      if (std::find(kSeriesFields.begin(), kSeriesFields.end(), name) == kSeriesFields.end() &&
          !(daily && (name == "proxy_iv30" || name == "rv21" || name == "vix" || name == "vix_minus_mfiv"))) return bad("unknown series field or daily-only field");
      if (std::find(fields.begin(), fields.end(), name) != fields.end()) return bad("duplicate series field");
      fields.push_back(name);
    }
    if (fields.empty() || query.at("fields").back() == ',') return bad("fields must be a nonempty comma-separated list");
  }
  const auto observed_to = daily ? std::min(to, metrics ? metrics->as_of : source.wall_time()) : to;
  std::map<md::Timestamp, json> rows;
  const auto own = source.series() ? source.series()->rows(symbol, from, observed_to, daily) : std::vector<SeriesRow>{};
  auto make = [&](md::Timestamp time) -> json& {
    auto& row = rows[time];
    if (row.is_null()) {
      row = {{"t", time / md::kNanosPerSecond}, {"sources", json::object()}};
      for (const auto& field : fields) { row[field] = nullptr; row["sources"][field] = nullptr; }
    }
    return row;
  };
  for (const auto& sample : own) {
    const auto time = daily ? close_time(md::new_york_time(sample.time).date) : sample.time;
    auto& row = make(time);
    row["sample_time"] = sample.time / md::kNanosPerSecond;
    for (const auto& field : fields) {
      const auto found = std::find(kSeriesFields.begin(), kSeriesFields.end(), field);
      if (found == kSeriesFields.end()) continue;
      const double value = sample.values[static_cast<std::size_t>(found - kSeriesFields.begin())];
      row[field] = number(value);
      if (std::isfinite(value)) row["sources"][field] = "own";
    }
  }
  const auto wants = [&](const std::string& field) { return std::find(fields.begin(), fields.end(), field) != fields.end(); };
  if (daily && wants("proxy_iv30")) {
    const auto proxy_name = analytics::iv_proxy(symbol);
    const auto combined = analytics::historical_iv(own_days(source, symbol, observed_to), daily_bars(source, proxy_name), proxy_name, observed_to);
    for (const auto& sample : combined) {
      const auto time = close_time(sample.date);
      if (time < from || time > observed_to || sample.source.starts_with("own_")) continue;
      auto& row = make(time); row["proxy_iv30"] = sample.vol; row["sources"]["proxy_iv30"] = sample.source;
    }
  }
  if (daily && wants("rv21")) {
    const auto bars = daily_bars(source, symbol);
    for (std::size_t i = 0; i < bars.size(); ++i) {
      const auto time = close_time(md::new_york_time(bars[i].start).date);
      if (time < from || time > observed_to) continue;
      const auto value = analytics::realized_window(std::span<const md::Bar>(bars).first(i + 1), 21).close_to_close.vol;
      auto& row = make(time); row["rv21"] = number(value);
      if (std::isfinite(value)) row["sources"]["rv21"] = "daily_closes";
    }
  }
  if (daily && (symbol == "SPX" || symbol == "SPY") && (wants("vix") || wants("vix_minus_mfiv"))) {
    for (const auto& bar : daily_bars(source, "VIX")) {
      const auto date = md::new_york_time(bar.start).date;
      const auto time = close_time(date);
      if (time < from || time > observed_to || !md::market_session(md::new_york_to_utc(date, 9, 30)).open ||
          !std::isfinite(bar.close) || bar.close <= 0) continue;
      auto& row = make(time);
      row["vix_sample_time"] = bar.start / md::kNanosPerSecond;
      if (wants("vix")) { row["vix"] = bar.close; row["sources"]["vix"] = "VIX_daily_close"; }
      const auto sample = std::find_if(own.begin(), own.end(), [&](const auto& value) {
        return md::new_york_time(value.time).date == date;
      });
      if (wants("vix_minus_mfiv") && sample != own.end() && std::isfinite(sample->values[3])) {
        row["vix_minus_mfiv"] = bar.close - sample->values[3];
        row["sources"]["vix_minus_mfiv"] = "VIX_daily_close_minus_own_mfiv30";
      }
    }
  }
  // Explicit missing sessions break chart lines instead of bridging a data outage.
  if (daily && !rows.empty()) {
    const auto first = md::days_since_epoch(md::new_york_time(rows.begin()->first).date);
    const auto last = md::days_since_epoch(md::new_york_time(rows.rbegin()->first).date);
    for (auto day = first; day <= last; ++day) {
      const auto date = md::date_from_days(day);
      if (md::market_session(md::new_york_to_utc(date, 9, 30)).open) make(close_time(date));
    }
  }
  if (rows.size() > 10000) return bad("series response exceeds 10000 rows; narrow the range");
  json output = json::array();
  for (auto& [time, row] : rows) { (void)time; output.push_back(std::move(row)); }
  return {200, json{{"symbol", symbol}, {"interval", interval}, {"from", from / md::kNanosPerSecond},
      {"to", to / md::kNanosPerSecond}, {"fields", fields}, {"rows", output}}.dump()};
}
}  // namespace openport::server
