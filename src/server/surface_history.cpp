#include "series_api.hpp"
#include "metric_cache.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <set>

namespace openport::server {
namespace {
using nlohmann::json;
json number(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
double value(const json& j, const char* field) { return j.at(field).is_null() ? analytics::kNaN : j.at(field).get<double>(); }
bool valid_symbol(const std::string& symbol) {
  return !symbol.empty() && symbol.size() <= 32 && symbol.front() != '.' &&
      std::all_of(symbol.begin(), symbol.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
      });
}
md::Timestamp close(md::Date date) { return md::new_york_to_utc(date, md::regular_close_hour(date), 0); }
SurfaceSnapshot decode(const json& j) {
  SurfaceSnapshot out;
  if (j.at("schema") != 1) throw std::runtime_error("unsupported surface snapshot version");
  out.time = j.at("market_ns").get<md::Timestamp>();
  out.slot = j.at("slot").get<std::string>();
  if (out.time <= 0 || out.slot.empty() || surface_slot(out.time) != out.slot)
    throw std::runtime_error("invalid surface capture time or slot");
  out.spot = value(j, "spot");
  for (const auto& e : j.at("expiries")) {
    SurfaceExpirySnapshot expiry;
    expiry.id = e.at("id").get<std::string>();
    const auto date = md::parse_datetime(e.at("expiry").get<std::string>() + " 12:00:00", md::Zone::NewYork);
    if (!date || !expiry.id.starts_with(e.at("expiry").get<std::string>())) throw std::runtime_error("invalid surface expiry");
    expiry.expiry = md::new_york_time(*date).date;
    expiry.forward = value(e, "forward");
    expiry.calendar_ok = e.at("calendar_ok").get<bool>();
    auto& fit = expiry.fit;
    const auto status = e.at("svi_status").get<std::string>();
    if (status != "ok" && status != "too_few_points" && status != "failed") throw std::runtime_error("invalid SVI status");
    fit.status = status == "ok" ? analytics::SviStatus::Ok : status == "too_few_points" ? analytics::SviStatus::TooFewPoints : analytics::SviStatus::Failed;
    fit.reason = e.at("svi_reason").is_null() ? "" : e.at("svi_reason").get<std::string>();
    fit.years = value(e, "svi_years"); fit.min_k = value(e, "svi_min_k"); fit.max_k = value(e, "svi_max_k");
    fit.points = e.at("svi_points").get<std::size_t>();
    if (fit.status == analytics::SviStatus::Ok) {
      const auto& p = e.at("svi");
      fit.parameters = {value(p, "a"), value(p, "b"), value(p, "rho"), value(p, "m"), value(p, "sigma")};
      fit.rmse_vol_points = value(p, "rmse_vol_points");
      fit.fit_ms = p.at("fit_ms").get<double>();
      fit.butterfly = {value(p, "butterfly_min_g"), value(p, "butterfly_k"), p.at("butterfly_ok").get<bool>()};
      if (!analytics::svi_admissible(fit.parameters, fit.years) || !std::isfinite(fit.min_k) ||
          !std::isfinite(fit.max_k) || fit.min_k >= fit.max_k) throw std::runtime_error("invalid SVI parameters or range");
    }
    out.expiries.push_back(std::move(expiry));
  }
  return out;
}
}

std::string surface_slot(md::Timestamp time) {
  if (time <= 0) return {};
  const auto date = md::new_york_time(time).date;
  const auto end = close(date);
  if (!md::market_session(md::new_york_to_utc(date, 9, 30)).open || time > end) return {};
  if (time >= end - 5 * md::kNanosPerMinute) return "close";
  for (int hour : {10, 12, 14}) {
    const auto start = md::new_york_to_utc(date, hour, 0);
    if (time >= start && time < start + 5 * md::kNanosPerMinute) return std::to_string(hour) + ":00";
  }
  return {};
}

SurfaceSnapshot surface_snapshot(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  SurfaceSnapshot out{metrics->as_of, surface_slot(metrics->as_of), metrics->spot, {}};
  const auto cache = surface_cache(metrics);
  const std::lock_guard lock(cache->mutex);
  while (cache->fits.size() < metrics->slices.size())
    cache->fits.push_back(analytics::fit_svi(metrics->slices[cache->fits.size()]));
  std::set<std::size_t> bad;
  for (const auto& violation : analytics::svi_calendar(cache->fits)) { bad.insert(violation.earlier); bad.insert(violation.later); }
  for (std::size_t i = 0; i < metrics->slices.size(); ++i) {
    const auto& slice = metrics->slices[i];
    const auto id = md::format_date(slice.expiry) +
        (slice.expiry_time == md::new_york_to_utc(slice.expiry, 9, 30) ? "AM" : "PM") +
        (slice.root.empty() ? "" : "-" + slice.root);
    out.expiries.push_back({id, slice.expiry, slice.forward.forward, cache->fits[i], !bad.contains(i)});
    // Half-spreads are only inputs to the calendar diagnostic, not stored curves.
    out.expiries.back().fit.half_spreads.clear();
  }
  return out;
}

json surface_snapshot_json(const SurfaceSnapshot& snapshot) {
  json expiries = json::array();
  for (const auto& e : snapshot.expiries) {
    const auto& fit = e.fit;
    const auto& p = fit.parameters;
    json parameters = nullptr;
    if (fit.status == analytics::SviStatus::Ok)
      parameters = {{"a", number(p.a)}, {"b", number(p.b)}, {"rho", number(p.rho)}, {"m", number(p.m)}, {"sigma", number(p.sigma)},
          {"status", "ok"}, {"reason", nullptr}, {"points", fit.points}, {"rmse_vol_points", number(fit.rmse_vol_points)},
          {"fit_ms", fit.fit_ms}, {"butterfly_ok", fit.butterfly.ok}, {"butterfly_min_g", number(fit.butterfly.min_g)}, {"butterfly_k", number(fit.butterfly.k)}};
    expiries.push_back({{"id", e.id}, {"expiry", md::format_date(e.expiry)}, {"forward", number(e.forward)},
        {"days", number(fit.years * 365)}, {"atm_iv", number(analytics::svi_iv(p, 0, fit.years))}, {"points", json::array()},
        {"svi", parameters}, {"svi_status", analytics::to_string(fit.status)}, {"svi_reason", fit.reason.empty() ? json(nullptr) : json(fit.reason)},
        {"svi_points", fit.points}, {"svi_years", number(fit.years)}, {"svi_min_k", number(fit.min_k)}, {"svi_max_k", number(fit.max_k)}, {"calendar_ok", e.calendar_ok}});
  }
  return {{"as_of", md::format_timestamp(snapshot.time)}, {"date", md::format_date(md::new_york_time(snapshot.time).date)},
      {"slot", snapshot.slot}, {"spot", number(snapshot.spot)}, {"model", "svi"}, {"expiries", expiries}};
}

void SeriesStore::load_surfaces(const std::filesystem::path& path) {
  try {
    const auto stem = path.stem().string();
    if (stem.size() < 9) throw std::runtime_error("invalid surface filename");
    const auto symbol = stem.substr(0, stem.size() - 8);
    if (!valid_symbol(symbol)) throw std::runtime_error("invalid surface symbol");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read surfaces");
    for (std::string line; std::getline(input, line);) {
      if (input.eof()) throw std::runtime_error("torn final surface row");
      auto snapshot = decode(json::parse(line));
      const auto date = md::new_york_time(snapshot.time).date;
      if (md::format_date(date).substr(0, 7) != stem.substr(stem.size() - 7)) throw std::runtime_error("surface month mismatch");
      last_write_ = std::max(last_write_, snapshot.time);
      surfaces_[symbol][{date, snapshot.slot}] = std::move(snapshot);
    }
    if (input.bad()) throw std::runtime_error("cannot read surfaces");
  } catch (const std::exception& e) { damaged_.insert(path); error_ = path.string() + ": " + e.what(); }
}

bool SeriesStore::insert_surface(const std::string& symbol, const SurfaceSnapshot& snapshot, bool force) {
  const std::lock_guard lock(mutex_);
  if (!valid_symbol(symbol) || snapshot.slot.empty() || surface_slot(snapshot.time) != snapshot.slot) {
    error_ = "series: invalid surface symbol or capture slot"; return false;
  }
  const auto date = md::new_york_time(snapshot.time).date;
  const auto key = std::make_pair(date, snapshot.slot);
  auto& values = surfaces_[symbol];
  const auto found = values.find(key);
  if (found != values.end() && snapshot.time != found->second.time &&
      (snapshot.slot != "close" || snapshot.time < found->second.time)) return false;
  if (found != values.end() && !force && (snapshot.slot != "close" || snapshot.time <= found->second.time)) return false;
  auto j = surface_snapshot_json(snapshot);
  if (found != values.end()) {
    auto previous = surface_snapshot_json(found->second);
    auto comparable = j;
    for (auto* object : {&previous, &comparable})
      for (auto& expiry : (*object)["expiries"]) if (expiry["svi"].is_object()) expiry["svi"].erase("fit_ms");
    if (comparable == previous) return false;
  }
  try {
    if (!directory_.empty()) {
      const auto path = directory_ / (symbol + '-' + md::format_date(date).substr(0, 7) + ".surfaces");
      if (damaged_.contains(path)) throw std::runtime_error("refusing to append to damaged file " + path.string());
      j["schema"] = 1; j["market_ns"] = snapshot.time;
      std::ofstream output(path, std::ios::app);
      output << j.dump() << '\n'; output.flush();
      if (!output) { damaged_.insert(path); throw std::runtime_error("cannot write " + path.string()); }
    }
    values[key] = snapshot;
    last_write_ = std::max(last_write_, snapshot.time);
    return true;
  } catch (const std::exception& e) { error_ = "series: " + std::string(e.what()); return false; }
}

std::vector<SurfaceSnapshot> SeriesStore::surfaces(const std::string& symbol, md::Timestamp from, md::Timestamp to) const {
  const std::lock_guard lock(mutex_);
  std::vector<SurfaceSnapshot> out;
  const auto found = surfaces_.find(symbol);
  if (found == surfaces_.end() || from > to) return out;
  for (const auto& [key, snapshot] : found->second) {
    if (snapshot.time >= from && snapshot.time <= to && surface_slot(snapshot.time) == snapshot.slot &&
        (snapshot.slot != "close" || close(key.first) <= to)) out.push_back(snapshot);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
  return out;
}

ApiResponse surface_history_response(const MetricsSource& source, const std::string& symbol,
                                     const std::map<std::string, std::string>& query) {
  const auto bad = [](const std::string& message) { return ApiResponse{400, json{{"error", message}}.dump()}; };
  const auto symbols = source.symbols();
  if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) return {404, json{{"error", "unknown underlying"}}.dump()};
  for (const auto& [key, val] : query) {
    (void)val;
    if (key != "from" && key != "to" && key != "compare") return bad("unknown surface-history parameter");
  }
  if (query.contains("compare") && query.at("compare") != "true" && query.at("compare") != "false") return bad("compare must be true or false");
  const auto metrics = source.metrics(symbol);
  const auto as_of = metrics ? metrics->as_of : source.wall_time();
  auto from = std::max<md::Timestamp>(0, as_of - 45 * md::kNanosPerDay), to = as_of;
  for (const auto* key : {"from", "to"}) if (query.contains(key)) {
    const auto& text = query.at(key);
    const auto time = md::parse_datetime(text.size() == 10 ? text + (std::string(key) == "from" ? " 00:00:00" : " 23:59:59") : text,
        text.size() == 10 ? md::Zone::NewYork : md::Zone::Utc);
    if (!time || *time < 0) return bad("from/to must be ISO timestamps or YYYY-MM-DD");
    (std::string(key) == "from" ? from : to) = *time;
  }
  if (from > to || to - from > 366 * md::kNanosPerDay) return bad("surface range must be ordered and at most 366 days");
  const auto rows = source.series() ? source.series()->surfaces(symbol, from, std::min(to, as_of)) : std::vector<SurfaceSnapshot>{};
  json snapshots = json::array(), comparisons = json::array();
  if (!query.contains("compare") || query.at("compare") != "true")
    for (const auto& row : rows) snapshots.push_back(surface_snapshot_json(row));
  const auto today = md::new_york_time(as_of).date;
  for (const auto& [label, offset] : std::vector<std::pair<std::string, int>>{{"prior_close", 1}, {"week", 7}, {"month", 30}}) {
    auto date = md::date_from_days(md::days_since_epoch(today) - offset);
    if (label == "month") {
      date = today;
      if (--date.month == 0) { date.month = 12; --date.year; }
      while (!md::valid_date(date)) --date.day;
    }
    if (!md::market_session(md::new_york_to_utc(date, 9, 30)).open) date = md::previous_business_day(date);
    const auto match = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
      return row.slot == "close" && md::new_york_time(row.time).date == date;
    });
    comparisons.push_back({{"label", label}, {"date", md::format_date(date)},
        {"snapshot", match == rows.end() ? json(nullptr) : surface_snapshot_json(*match)}});
  }
  return {200, json{{"symbol", symbol}, {"enabled", source.series() != nullptr}, {"snapshots", snapshots}, {"comparisons", comparisons}}.dump()};
}
}  // namespace openport::server
