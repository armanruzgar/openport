#include "openport/server/api.hpp"
#include "openport/server/sandboxes.hpp"
#include "metric_cache.hpp"
#include "series_api.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <type_traits>

#include "openport/analytics/svi.hpp"
#include "openport/analytics/realized.hpp"
#include "openport/analytics/ssvi.hpp"
#include "openport/providers/demo.hpp"
#include "paper_json.hpp"
#include "playbook_api.hpp"

namespace openport::server {
namespace {

using analytics::SliceMetrics;
using analytics::UnderlyingMetrics;
using nlohmann::json;

/// Rounds to `digits` significant digits, keeping payloads small without losing
/// anything a screen can show. Non-finite values become JSON null.
json sig(double x, int digits = 6) {
  if (!std::isfinite(x)) return nullptr;
  if (x == 0.0) return 0.0;
  // Scale by an exact power of ten in the direction that keeps it >= 1, so large
  // values come back as clean integers rather than 11665899999.999998.
  const int exponent = digits - 1 - static_cast<int>(std::floor(std::log10(std::abs(x))));
  if (exponent >= 0) {
    const double scale = std::pow(10.0, exponent);
    return std::round(x * scale) / scale;
  }
  const double factor = std::pow(10.0, -exponent);
  return std::round(x / factor) * factor;
}

json price(double x) {
  if (!std::isfinite(x)) return nullptr;
  return std::round(x * 1e4) / 1e4;
}

std::string settlement_of(const SliceMetrics& slice) {
  return slice.expiry_time == md::new_york_to_utc(slice.expiry, 9, 30) ? "AM" : "PM";
}

std::string expiry_id(const SliceMetrics& slice) {
  return md::format_date(slice.expiry) + settlement_of(slice) +
         (slice.root.empty() ? "" : "-" + slice.root);
}

json coverage_json(const analytics::Coverage& c) {
  return {{"options", c.options},
          {"quoted", c.quoted},
          {"priced", c.priced},
          {"open_interest", c.open_interest},
          {"volume", c.volume}};
}

json spot_source_json(const UnderlyingMetrics& m) {
  return m.spot_source.empty() ? json(nullptr) : json(m.spot_source);
}

json market_json(md::Timestamp ts) {
  const auto session = md::market_session(ts);
  return {{"open", session.open},
          {"note", session.note},
          {"next_open",
           session.next_open ? json(md::format_timestamp(*session.next_open)) : json(nullptr)}};
}

json expiry_json(const UnderlyingMetrics& m, const SliceMetrics& slice) {
  const double rate = -std::log(slice.forward.discount) / slice.years;
  // Only OEX and XEO slices name their root; the rest take the underlying's clock,
  // so ETF options keep their 16:15 last trade.
  md::OptionContract contract;
  contract.root = slice.root;
  contract.underlying = slice.root.empty() ? m.symbol : md::conventions_for_root(slice.root).underlying;
  contract.expiry = slice.expiry;
  contract.settlement = settlement_of(slice) == "AM" ? md::Settlement::AM : md::Settlement::PM;
  const auto last_trade = contract.last_trade_time();
  json dividends = json::array();
  for (const auto& d : slice.dividends)
    dividends.push_back({{"ex_date", md::format_date(d.ex_date)}, {"amount", d.amount}});
  return {
      {"id", expiry_id(slice)},
      {"expiry", md::format_date(slice.expiry)},
      {"settlement", settlement_of(slice)},
      {"style", slice.style == pricing::ExerciseStyle::American ? "american" : "european"},
      {"coverage", coverage_json(slice.coverage)},
      {"expiry_time", md::format_timestamp(slice.expiry_time)},
      {"last_trade", md::format_timestamp(last_trade)},
      {"auto_close", md::format_timestamp(last_trade - 5 * md::kNanosPerMinute)},
      {"days", sig(slice.years * 365.0, 5)},
      {"forward", price(slice.forward.forward)},
      {"discount", sig(slice.forward.discount, 9)},
      {"rate", sig(rate, 4)},
      {"rate_fitted", slice.forward.fitted_discount},
      {"rate_source", slice.rate_source},
      {"rate_curve_symbol",
       slice.rate_curve_symbol.empty() ? json(nullptr) : json(slice.rate_curve_symbol)},
      {"deamericanized", slice.deamericanized},
      {"dividends", std::move(dividends)},
      {"atm_iv", sig(slice.atm_iv)},
      {"gex", sig(slice.gex)},
      {"vex", sig(slice.vex)},
      {"strikes", slice.strikes.size()},
  };
}

json exposure_summary(const UnderlyingMetrics& m) {
  return {
      {"oi_coverage", sig(m.exposure.oi_coverage)},
      {"gex", sig(m.exposure.gex)},
      {"vex", sig(m.exposure.vex)},
      {"gamma_flip", price(m.exposure.gamma_flip)},
      {"call_wall", price(m.exposure.call_wall)},
      {"put_wall", price(m.exposure.put_wall)},
  };
}

/// Why paper orders cannot fill against a displayed quote, or null when they can:
/// the simulator trades only a positive, uncrossed two-sided quote with at least
/// one whole contract on each side (trading::valid_quote), as the Desk offers it.
json quote_issue(const analytics::OptionMetrics& o) {
  const auto money = [](double value) -> std::optional<trading::Money> {
    if (!std::isfinite(value) || value <= 0) return std::nullopt;
    try {
      const auto price = trading::Money::from_double(value);
      return price > trading::Money{} ? std::optional(price) : std::nullopt;
    } catch (const trading::TradingError&) { return std::nullopt; }
  };
  const auto bid = money(o.bid);
  const auto ask = money(o.ask);
  if (!bid && !ask) return "no_quote";
  if (!bid) return "no_bid";
  if (!ask) return "no_ask";
  if (*ask < *bid) return "crossed";
  const auto whole = [](double size) { return std::isfinite(size) && size >= 1; };
  if (!whole(o.bid_size) || !whole(o.ask_size)) return "zero_size";
  return nullptr;
}

json option_json(const analytics::OptionMetrics& o) {
  if (o.id == analytics::kNoInstrument) return nullptr;
  auto issue = quote_issue(o);
  return {
      {"symbol", o.contract.osi_symbol()},
      {"bid_size", std::isfinite(o.bid_size) && o.bid_size >= 0 && o.bid_size < 9223372036854775808.0
          ? json(static_cast<std::int64_t>(std::floor(o.bid_size))) : json(nullptr)},
      {"ask_size", std::isfinite(o.ask_size) && o.ask_size >= 0 && o.ask_size < 9223372036854775808.0
          ? json(static_cast<std::int64_t>(std::floor(o.ask_size))) : json(nullptr)},
      {"tradable", trading::eligible(o.contract).ok()},
      {"untradable_reason", trading::eligible(o.contract).ok() ? json(nullptr) : json(trading::to_string(trading::eligible(o.contract).code))},
      {"executable", issue.is_null()},
      {"quote_issue", std::move(issue)},
      {"bid", price(o.bid)},
      {"ask", price(o.ask)},
      {"mid", price(o.mid)},
      {"eep", sig(o.eep)},
      {"iv", sig(o.iv)},
      {"bid_iv", sig(o.bid_iv)},
      {"ask_iv", sig(o.ask_iv)},
      {"delta", sig(o.delta)},
      {"gamma", sig(o.gamma)},
      {"vega", sig(o.vega)},
      {"theta", sig(o.theta)},
      {"vanna", sig(o.vanna)},
      {"oi", o.has_open_interest && std::isfinite(o.open_interest) ? json(o.open_interest)
                                                                   : json(nullptr)},
      {"volume", std::isfinite(o.volume) && o.volume >= 0 ? json(o.volume) : json(nullptr)},
      {"vendor_iv", sig(o.vendor_iv)},
  };
}

// Strict decimal syntax before strtod: libc++ and libstdc++ streams disagree
// on underflow. ERANGE rejects both overflow and underflow, including subnormals.
bool decimal_number(const std::string& text, double& value) {
  std::size_t pos = 0;
  if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
  auto digits = [&] {
    const auto first = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    return pos != first;
  };
  bool mantissa = digits();
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    mantissa = digits() || mantissa;
  }
  if (!mantissa) return false;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
    if (!digits()) return false;
  }
  if (pos != text.size()) return false;
  errno = 0;
  char* end = nullptr;
  value = std::strtod(text.c_str(), &end);
  return errno != ERANGE && end == text.c_str() + text.size() && std::isfinite(value);
}

template <typename T>
bool bounded_number(const std::map<std::string, std::string>& query, const std::string& key,
                    T minimum, T maximum, T& value) {
  const auto it = query.find(key);
  if (it == query.end()) return true;
  const auto& text = it->second;
  if constexpr (std::is_integral_v<T>) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return false;
  } else {
    if (!decimal_number(text, value)) return false;
  }
  return std::isfinite(value) && value >= minimum && value <= maximum;
}

ApiResponse ok(const json& body) {
  return {200, body.dump()};
}

ApiResponse error(int status, const std::string& message) {
  return api_error(status, status == 400 ? "INVALID_REQUEST" : status == 404 ? "NOT_FOUND" : "METHOD_NOT_ALLOWED", message);
}

bool in_window(double strike, double spot, double window) {
  // A hair of tolerance so strikes exactly on the edge (4900 of 5000 at 2%) stay in.
  return !(window > 0.0) || !(spot > 0.0) || std::abs(strike / spot - 1.0) <= window + 1e-12;
}

json underlyings_json(const MetricsSource& source, const EngineStatus& status, bool details,
                      md::Timestamp now) {
  json underlyings = json::array();
  const auto view = source.trading_view();
  const auto max_quote_age = view ? view->config.limits.max_quote_age : trading::Limits{}.max_quote_age;
  std::set<std::string> symbols;
  for (const auto& symbol : source.symbols()) symbols.insert(symbol);
  for (const auto& [symbol, health] : status.underlyings) symbols.insert(symbol);
  for (const std::string& symbol : symbols) {
    const auto session = md::trading_session(symbol, now);
    json item{
        {"symbol", symbol},
        {"spot", nullptr},
        {"as_of", nullptr},
        {"version", 0},
        {"has_tradable_contracts", false},
        {"session", {{"name", session.name}, {"open", session.open}, {"note", session.note}}}};
    if (details) {
      item["expiries"] = 0;
      item["options"] = 0;
    }
    if (const auto health = status.underlyings.find(symbol); health != status.underlyings.end()) {
      const auto& h = health->second;
      item["state"] = md::to_string(h.state);
      item["message"] = h.message;
      item["last_success"] =
          h.last_success > 0 ? json(md::format_timestamp(h.last_success)) : json(nullptr);
      item["last_error"] = h.last_error.empty() ? json(nullptr) : json(h.last_error);
      item["last_error_time"] =
          h.last_error_time > 0 ? json(md::format_timestamp(h.last_error_time)) : json(nullptr);
    }
    const auto m = source.metrics(symbol);
    auto market_time = m ? m->as_of : 0;
    if (view) {
      const auto time = view->market_times.find(symbol);
      market_time = time == view->market_times.end() ? 0 : time->second;
    }
    const auto paper = paper_acceptance(symbol, market_time, now, status.capabilities.delay, max_quote_age,
                                        view ? view->halts : std::vector<MarketHalt>{});
    // The session new orders enter: the market-data clock's, which a delayed
    // feed keeps behind the wall clock's `session`.
    item["paper"] = {{"accepting", paper.ok()},
                     {"reason", paper.ok() ? json(nullptr) : json(trading::to_string(paper.code))},
                     {"message", paper.ok() ? json(nullptr) : json(paper.message)},
                     {"session", market_time > 0 ? json(md::trading_session(symbol, market_time).name) : json(nullptr)}};
    if (m) {
      item["spot"] = price(m->spot);
      item["as_of"] = md::format_timestamp(m->as_of);
      item["version"] = m->version;
      item["has_tradable_contracts"] = std::any_of(m->slices.begin(), m->slices.end(), [](const auto& slice) {
        return slice.years > 0 && std::any_of(slice.strikes.begin(), slice.strikes.end(), [](const auto& row) {
          return (row.call.id != analytics::kNoInstrument && trading::eligible(row.call.contract).ok()) ||
                 (row.put.id != analytics::kNoInstrument && trading::eligible(row.put.contract).ok());
        });
      });
      if (details) {
        item["expiries"] = m->slices.size();
        item["options"] = m->options_priced;
      }
    }
    underlyings.push_back(std::move(item));
  }
  return underlyings;
}

json circuit_breaker_json(const CircuitBreakerStatus& breaker) {
  json halts = json::array();
  for (const auto& halt : breaker.halts)
    halts.push_back({{"level", halt.level}, {"start", md::format_timestamp(halt.start)},
                     {"end", md::format_timestamp(halt.end)}, {"reference", halt.reference}, {"price", halt.price},
                     {"active", halt.start <= breaker.market_time && breaker.market_time < halt.end}});
  const auto& close = breaker.previous_close;
  return {{"symbol", breaker.symbol}, {"day", breaker.market_time > 0 ? json(md::format_date(breaker.day)) : json(nullptr)},
          {"previous_close", close ? json{{"date", md::format_date(close->date)}, {"price", close->price}} : json(nullptr)},
          {"level", breaker.level}, {"halts", halts}, {"active", breaker.active},
          {"inactive_reason", close ? json(nullptr) : json("MISSING_PREVIOUS_CLOSE")},
          {"market_time", breaker.market_time > 0 ? json(md::format_timestamp(breaker.market_time)) : json(nullptr)},
          {"error", breaker.error.empty() ? json(nullptr) : json(breaker.error)}};
}

json status_json(const MetricsSource& source) {
  const EngineStatus s = source.status();
  const auto now = source.wall_time();
  auto notifications = source.configured_notifications() ? source.configured_notifications()->status() : json{
      {"enabled", false}, {"include_simulated", false}, {"queue_depth", 0}, {"queue_capacity", 0}, {"dropped", 0}, {"channels", json::array()}};
  // Availability is source-specific; channel settings and counters belong to the shared service.
  if (!source.notifications()) notifications["enabled"] = false;
  return {
      {"version", OPENPORT_VERSION},
      {"trading", trading_status_json(s.trading)},
      {"accounts", account_ticks_json(s)},
      {"series", series_status_json(source)},
      {"notifications", std::move(notifications)},
      {"circuit_breaker", circuit_breaker_json(s.circuit_breaker)},
      {"market", market_json(now)},
      {"provider",
       {{"name", s.provider},
        {"simulated", providers::simulated_provider(s.provider)},
        {"realtime", s.capabilities.realtime},
        {"realtime_plan_dependent", s.capabilities.realtime_plan_dependent},
        {"poll_interval_seconds", s.capabilities.poll_interval.count()},
        {"delay_seconds", s.capabilities.delay.count()},
        {"trades", s.capabilities.trades},
        {"open_interest", s.capabilities.open_interest},
        {"vendor_greeks", s.capabilities.vendor_greeks}}},
      {"feed",
       {{"state", md::to_string(s.feed_state)},
        {"message", s.feed_message},
        {"updated",
         s.feed_updated > 0 ? json(md::format_timestamp(s.feed_updated)) : json(nullptr)}}},
      {"underlyings", underlyings_json(source, s, true, now)},
      {"engine",
       {{"events", s.events},
        {"events_per_second", sig(s.events_per_second, 4)},
        {"analytics_ms", sig(s.analytics_ms, 4)},
        {"queue_depth", s.queue_depth},
        {"coalesced_events", s.coalesced_events},
        {"dropped_events", s.dropped_events},
        {"overloaded", s.overloaded},
        {"contracts", s.contracts},
        {"nonstandard_contracts", s.nonstandard_contracts},
        {"uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(s.uptime).count()}}},
  };
}

json summary_json(const UnderlyingMetrics& m) {
  json expiries = json::array();
  for (const SliceMetrics& slice : m.slices) expiries.push_back(expiry_json(m, slice));
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"compute_ms", sig(m.compute_ms, 4)},
          {"american_approximation", m.american_approximation},
          {"coverage", coverage_json(m.coverage)},
          {"exposure", exposure_summary(m)},
          {"expiries", expiries}};
}

json chain_json(const UnderlyingMetrics& m, const SliceMetrics& slice, double window) {
  json strikes = json::array();
  for (const auto& row : slice.strikes) {
    if (!in_window(row.strike, m.spot, window)) continue;
    strikes.push_back({{"strike", row.strike},
                       {"iv", sig(row.iv)},
                       {"gex", sig(row.gex)},
                       {"vex", sig(row.vex)},
                       {"call", option_json(row.call)},
                       {"put", option_json(row.put)}});
  }
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"expiry", expiry_json(m, slice)},
          {"strikes", strikes}};
}

json exposure_json(const UnderlyingMetrics& m, std::size_t max_expiries, double window) {
  const std::size_t count = std::min(max_expiries, m.slices.size());
  std::set<double> strike_set;
  for (std::size_t i = 0; i < count; ++i) {
    for (const auto& row : m.slices[i].strikes) {
      if (in_window(row.strike, m.spot, window)) strike_set.insert(row.strike);
    }
  }
  const std::vector<double> strikes(strike_set.begin(), strike_set.end());
  std::map<double, std::size_t> column;
  for (std::size_t i = 0; i < strikes.size(); ++i) column[strikes[i]] = i;

  std::vector<double> total(strikes.size(), 0.0);
  json expiries = json::array();
  for (std::size_t i = 0; i < count; ++i) {
    const SliceMetrics& slice = m.slices[i];
    std::vector<double> gex(strikes.size(), 0.0);
    std::vector<double> vex(strikes.size(), 0.0);
    for (const auto& row : slice.strikes) {
      const auto it = column.find(row.strike);
      if (it == column.end()) continue;
      gex[it->second] = row.gex;
      vex[it->second] = row.vex;
      total[it->second] += row.gex;
    }
    json gex_json = json::array();
    json vex_json = json::array();
    for (std::size_t j = 0; j < strikes.size(); ++j) {
      gex_json.push_back(sig(gex[j], 4));
      vex_json.push_back(sig(vex[j], 4));
    }
    expiries.push_back({{"id", expiry_id(slice)},
                        {"expiry", md::format_date(slice.expiry)},
                        {"days", sig(slice.years * 365.0, 4)},
                        {"gex", gex_json},
                        {"vex", vex_json}});
  }
  json total_json = json::array();
  for (double x : total) total_json.push_back(sig(x, 4));
  return {{"symbol", m.symbol},
          {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)},
          {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version},
          {"strikes", strikes},
          {"expiries", expiries},
          {"total_gex", total_json},
          {"exposure", exposure_summary(m)}};
}

json svi_json(const analytics::SviFit& fit) {
  if (fit.status != analytics::SviStatus::Ok) return nullptr;
  const auto& p = fit.parameters;
  // Full precision: rounding parameters independently can spoil a narrow smile.
  return {{"a", p.a}, {"b", p.b}, {"rho", p.rho}, {"m", p.m}, {"sigma", p.sigma},
          {"rmse_vol_points", sig(fit.rmse_vol_points)}, {"points", fit.points},
          {"status", analytics::to_string(fit.status)}, {"reason", nullptr},
          {"fit_ms", fit.fit_ms}, {"butterfly_min_g", sig(fit.butterfly.min_g)},
          {"butterfly_k", sig(fit.butterfly.k)}, {"butterfly_ok", fit.butterfly.ok}};
}

json surface_json(const std::shared_ptr<const UnderlyingMetrics>& metrics,
                  std::size_t max_expiries, double window) {
  const auto& m = *metrics;
  const auto count = std::min(max_expiries, m.slices.size());
  const auto cache = surface_cache(metrics);
  std::vector<analytics::SviFit> fits;
  analytics::SsviFit ssvi;
  {
    const std::lock_guard lock(cache->mutex);
    // Only newly requested expiries are fitted; display windows never change fits.
    while (cache->fits.size() < count)
      cache->fits.push_back(analytics::fit_svi(m.slices[cache->fits.size()]));
    if (!cache->ssvi) cache->ssvi = analytics::fit_ssvi(m.slices);
    ssvi = *cache->ssvi;
    fits.assign(cache->fits.begin(), cache->fits.begin() + static_cast<std::ptrdiff_t>(count));
  }
  json expiries = json::array();
  for (std::size_t i = 0; i < count; ++i) {
    const SliceMetrics& slice = m.slices[i];
    const auto& fit = fits[i];
    const auto& surface_fit = ssvi.expiries[i];
    json points = json::array();
    for (const auto& row : slice.strikes) {
      if (!std::isfinite(row.iv) || !in_window(row.strike, m.spot, window)) continue;
      // Preserve the market display filter; calibration uses every usable two-sided
      // OTM point, with wide IV spreads downweighted rather than removed.
      const auto& otm = row.strike >= slice.forward.forward ? row.call : row.put;
      if (!(otm.bid > 0.0) || !(otm.ask - otm.bid <= 0.5 * otm.mid)) continue;
      const double k = std::log(row.strike / slice.forward.forward);
      points.push_back({{"strike", row.strike}, {"k", sig(k, 6)},
                        {"iv", sig(row.iv)}, {"bid_iv", sig(otm.bid_iv)},
                        {"ask_iv", sig(otm.ask_iv)},
                        {"svi_iv", fit.status == analytics::SviStatus::Ok
                            ? sig(analytics::svi_iv(fit.parameters, k, slice.years)) : json(nullptr)},
                        {"ssvi_iv", sig(analytics::ssvi_iv(ssvi.parameters, k, surface_fit.theta, slice.years))}});
    }
    expiries.push_back({{"id", expiry_id(slice)}, {"expiry", md::format_date(slice.expiry)},
                        {"days", sig(slice.years * 365.0, 4)},
                        {"forward", price(slice.forward.forward)},
                        {"atm_iv", sig(slice.atm_iv)}, {"points", points}, {"svi", svi_json(fit)},
                        {"ssvi_theta", sig(surface_fit.theta, 17)},
                        {"ssvi_rmse_vol_points", sig(surface_fit.rmse_vol_points)},
                        {"ssvi_reason", surface_fit.reason.empty() ? json(nullptr) : json(surface_fit.reason)},
                        {"ssvi_min_k", sig(surface_fit.min_k, 15)}, {"ssvi_max_k", sig(surface_fit.max_k, 15)},
                        {"svi_status", analytics::to_string(fit.status)},
                        {"svi_reason", fit.reason.empty() ? json(nullptr) : json(fit.reason)},
                        {"svi_points", fit.points}, {"svi_fit_ms", fit.fit_ms},
                        {"svi_years", sig(slice.years, 15)},
                        {"svi_min_k", sig(fit.min_k, 15)}, {"svi_max_k", sig(fit.max_k, 15)}});
  }
  json violations = json::array();
  for (const auto& pair : analytics::svi_calendar(fits))
    violations.push_back({{"earlier", expiry_id(m.slices[pair.earlier])},
                          {"later", expiry_id(m.slices[pair.later])}, {"k", sig(pair.k)},
                          {"vol_points", sig(pair.vol_points)},
                          {"tolerance_vol_points", sig(pair.tolerance_vol_points)}});
  return {{"symbol", m.symbol}, {"spot", price(m.spot)},
          {"spot_source", spot_source_json(m)}, {"as_of", md::format_timestamp(m.as_of)},
          {"version", m.version}, {"expiries", expiries}, {"calendar_violations", violations},
          {"ssvi", {{"rho", sig(ssvi.parameters.rho, 17)}, {"eta", sig(ssvi.parameters.eta, 17)},
                    {"gamma", sig(ssvi.parameters.gamma, 17)}, {"rmse_vol_points", sig(ssvi.rmse_vol_points)},
                    {"status", analytics::to_string(ssvi.status)},
                    {"reason", ssvi.reason.empty() ? json(nullptr) : json(ssvi.reason)},
                    {"monotone_adjusted", ssvi.monotone_adjusted}, {"fit_ms", ssvi.fit_ms}}}};
}

json reason_json(const std::string& reason) {
  return reason.empty() ? json(nullptr) : json(reason);
}

json delta_json(const analytics::DeltaSkew& skew) {
  auto point = [](const analytics::DeltaPoint& p) {
    return json{{"strike", price(p.strike)}, {"vol", sig(p.vol)}, {"reason", reason_json(p.reason)}};
  };
  return {{"call", point(skew.call)}, {"put", point(skew.put)}, {"rr", sig(skew.rr)}, {"bf", sig(skew.bf)}};
}

json volatility_json(const std::shared_ptr<const UnderlyingMetrics>& metrics, const MetricsSource& source) {
  const auto cache = surface_cache(metrics);
  const std::lock_guard response_lock(cache->mutex);
  if (const auto found = cache->volatility_responses.find(&source); found != cache->volatility_responses.end())
    return found->second;
  const auto& m = *metrics;
  analytics::VolatilityMetrics v;
  analytics::RealizedMetrics r;
  {
    if (!cache->volatility) {
      while (cache->fits.size() < m.slices.size())
        cache->fits.push_back(analytics::fit_svi(m.slices[cache->fits.size()]));
      cache->volatility = analytics::volatility_metrics(m, cache->fits);
    }
    v = *cache->volatility;
    const auto* store = source.candles();
    auto found = cache->realized.find(store);
    if (found == cache->realized.end()) {
      const auto days = store ? store->daily_history(m.symbol) : std::vector<md::Bar>{};
      const auto minutes = store ? store->bars(m.symbol, BarInterval::Minute, 20000) : std::vector<md::Bar>{};
      found = cache->realized.emplace(store, analytics::realized_metrics(days, minutes, m.as_of)).first;
    }
    r = found->second;
  }
  json mfiv_expiries = json::array(), atm_expiries = json::array(), skew_expiries = json::array();
  double front = analytics::kNaN;
  md::Timestamp front_time = std::numeric_limits<md::Timestamp>::max();
  for (std::size_t i = 0; i < m.slices.size(); ++i) {
    const auto& slice = m.slices[i];
    const auto& expiry = v.expiries[i];
    const auto& mfiv = expiry.mfiv;
    const auto& smile = expiry.smile;
    const auto id = expiry_id(slice);
    if (slice.expiry_time > m.as_of && slice.expiry_time < front_time) {
      front_time = slice.expiry_time; front = slice.forward.forward;
    }
    mfiv_expiries.push_back({{"id", id}, {"minutes", sig(expiry.minutes, 12)},
        {"variance", sig(mfiv.variance, 12)}, {"vol", sig(mfiv.vol)}, {"k0", price(mfiv.k0)},
        {"low", price(mfiv.low)}, {"high", price(mfiv.high)}, {"strikes", mfiv.strikes},
        {"lower_stop", mfiv.lower_stop}, {"upper_stop", mfiv.upper_stop},
        {"truncated", mfiv.truncated}, {"proxy", mfiv.proxy}, {"eep", mfiv.eep}, {"reason", reason_json(mfiv.reason)}});
    atm_expiries.push_back({{"id", id}, {"minutes", sig(expiry.minutes, 12)}, {"vol", sig(smile.atm)},
        {"source", smile.source}, {"slope", sig(smile.slope)}, {"curvature", sig(smile.curvature)},
        {"reason", reason_json(smile.reason)}});
    skew_expiries.push_back({{"id", id}, {"source", smile.source}, {"delta25", delta_json(smile.delta25)},
        {"delta10", delta_json(smile.delta10)}, {"slope", sig(smile.slope)}, {"curvature", sig(smile.curvature)}});
  }
  auto constant = [&](const std::vector<analytics::ConstantVol>& values) {
    json rows = json::array();
    for (const auto& value : values) rows.push_back({{"days", value.days}, {"variance", sig(value.variance, 12)},
        {"vol", sig(value.vol)}, {"near", value.reason == "target_not_bracketed" ? json(nullptr) : json(expiry_id(m.slices[value.near]))},
        {"next", value.reason == "target_not_bracketed" ? json(nullptr) : json(expiry_id(m.slices[value.next]))},
        {"truncated", value.truncated}, {"proxy", value.proxy}, {"reason", reason_json(value.reason)}});
    return rows;
  };
  json windows = json::array(), cones = json::array(), intraday = json::array();
  auto estimate = [](const analytics::RealizedEstimate& value) {
    return json{{"vol", sig(value.vol)}, {"fallback", value.fallback}, {"reason", reason_json(value.reason)}};
  };
  double rv21 = analytics::kNaN;
  for (const auto& window : r.windows) {
    if (window.sessions == 21) rv21 = window.close_to_close.vol;
    windows.push_back({{"sessions", window.sessions}, {"close_to_close", estimate(window.close_to_close)},
        {"parkinson", estimate(window.parkinson)}, {"garman_klass", estimate(window.garman_klass)},
        {"yang_zhang", estimate(window.yang_zhang)}});
  }
  for (const auto& cone : r.cones) {
    const auto implied = analytics::session_implied_vol(v, m.as_of, cone.sessions);
    cones.push_back({{"sessions", cone.sessions}, {"days_used", cone.days_used},
        {"observations", cone.observations}, {"min", sig(cone.min)}, {"p10", sig(cone.p10)}, {"p25", sig(cone.p25)},
        {"p50", sig(cone.p50)}, {"p75", sig(cone.p75)}, {"p90", sig(cone.p90)}, {"max", sig(cone.max)},
        {"current", sig(cone.current)}, {"current_percentile", sig(cone.current_percentile)},
        {"implied_days", sig(implied.days)}, {"implied_vol", sig(implied.vol)}, {"implied_proxy", implied.proxy},
        {"implied_reason", reason_json(implied.reason)}});
  }
  for (const auto& day : r.intraday) intraday.push_back({{"date", md::format_date(day.date)}, {"vol", sig(day.vol)},
      {"returns", day.returns}, {"observed_minutes", day.observed_minutes}, {"session_minutes", day.session_minutes},
      {"partial", day.partial}, {"reason", reason_json(day.reason)}});
  const auto moves = analytics::implied_moves(m, v, source.events());
  json sessions = json::array();
  for (const auto& move : moves.sessions) sessions.push_back({{"date", md::format_date(move.date)},
      {"points", sig(move.points)}, {"percent", sig(move.percent)}, {"forward", price(move.forward)},
      {"shared", move.shared}, {"calendar_arbitrage", move.calendar_arbitrage},
      {"proxy", move.proxy}, {"truncated", move.truncated}, {"label", reason_json(move.label)}, {"reason", reason_json(move.reason)}});
  json intervals = json::array();
  for (const auto& interval : moves.intervals) intervals.push_back({{"from", md::format_timestamp(interval.from)},
      {"to", md::format_timestamp(interval.to)}, {"forward_variance", sig(interval.forward_variance, 12)},
      {"sessions", interval.sessions}, {"calendar_arbitrage", interval.calendar_arbitrage},
      {"proxy", interval.proxy}, {"truncated", interval.truncated}, {"reason", reason_json(interval.reason)}});
  const auto vrp = analytics::variance_risk_premium(v.mfiv[1], rv21);
  json response = {{"symbol", m.symbol}, {"as_of", md::format_timestamp(m.as_of)}, {"version", m.version},
      {"spot", price(m.spot)}, {"forward", price(front)}, {"units", "vol_points"},
      {"mfiv", {{"expiries", mfiv_expiries}, {"constant", constant(v.mfiv)}}},
      {"atm", {{"expiries", atm_expiries}, {"constant", constant(v.atm)}}},
      {"skew", {{"delta_convention", analytics::kDeltaConvention}, {"expiries", skew_expiries},
          {"days", 30}, {"delta25", delta_json(v.skew25)}, {"delta10", delta_json(v.skew10)},
          {"proxy", v.skew_proxy}, {"reason", reason_json(v.skew_reason)}}},
      {"term", {{"mfiv9_30", sig(v.ratio9_30)}, {"mfiv30_93", sig(v.ratio30_93)}, {"atm30_7", sig(v.atm30_7)},
          {"truncated", v.mfiv[0].truncated || v.mfiv[1].truncated || v.mfiv[2].truncated},
          {"proxy", v.mfiv[0].proxy || v.mfiv[1].proxy || v.mfiv[2].proxy || v.atm[0].proxy || v.atm[1].proxy}}},
      {"realized", {{"daily_as_of", reason_json(r.daily_as_of)}, {"windows", windows}, {"cones", cones}, {"intraday", intraday}, {"today", sig(r.today)}}},
      {"vrp", {{"spread", sig(vrp.spread)}, {"ratio", sig(vrp.ratio)},
          {"truncated", vrp.truncated}, {"proxy", vrp.proxy}, {"reason", reason_json(vrp.reason)}}},
      {"implied_moves", {{"sessions", sessions}, {"intervals", intervals}, {"today_points", sig(moves.today_points)},
          {"today_percent", sig(moves.today_percent)}, {"today_reason", reason_json(moves.today_reason)},
          {"today_proxy", moves.today_proxy}, {"today_truncated", moves.today_truncated},
          {"today_calendar_arbitrage", moves.today_calendar_arbitrage}}},
      {"sources", {{"mfiv", "OTM quote mids, parity forward/discount; known American EEP removed; subscribed strike window"},
          {"atm", "Checked SVI at forward; otherwise observed smile IV interpolated in log-moneyness; total variance in time"},
          {"skew", "Own-smile forward delta solves within quote range; each delta point interpolated in total variance"},
          {"term", "Ratios of model-free IV; ATM 30d minus 7d, in vol points"},
          {"realized", "CandleStore completed daily bars and complete 5-minute blocks; 252 sessions/year; OHLC fallback is close-to-close"},
          {"vrp", "Ex ante: 30d model-free IV minus/divided by 21-session close-to-close RV"},
          {"implied_moves", "Consecutive-expiry total-variance increments, MFIV else ATM; equal business sessions with fractional remaining regular hours; AM overnight assigned to settlement session"}}}};
  cache->volatility_responses[&source] = response;
  return response;
}

/// Where the market's implied volatility says the underlying can be by each horizon:
/// a one and two standard deviation cone, and the odds of finishing above, below
/// or touching each price (see price_odds).
ApiResponse probability_response(const UnderlyingMetrics& m, const std::map<std::string, std::string>& query) {
  const auto list = [](const std::string& text, std::size_t most, double minimum, double maximum) -> std::optional<std::vector<double>> {
    std::vector<double> values;
    std::size_t start = 0;
    while (true) {
      const auto comma = text.find(',', start);
      double value = 0;
      if (!decimal_number(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start), value) ||
          value < minimum || value > maximum || values.size() == most) return std::nullopt;
      if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
      if (comma == std::string::npos) return values;
      start = comma + 1;
    }
  };
  std::vector<double> days{0, 1, 7, 30}, prices;
  for (const auto& [key, value] : query) {
    if (key == "days") {
      const auto parsed = list(value, 8, 0, 366);
      if (!parsed) return error(400, "days must be at most 8 numbers of days from 0 to 366");
      days = *parsed;
    } else if (key == "prices") {
      const auto parsed = list(value, 20, std::numeric_limits<double>::min(), 1e9);
      if (!parsed) return error(400, "prices must be at most 20 positive prices");
      prices = *parsed;
    } else if (key != "window" && key != "expiries") {
      return error(400, "unknown query parameter " + key);
    }
  }
  const bool priced = m.spot > 0 && std::isfinite(m.spot);
  json horizons = json::array();
  for (const double d : days) {
    const auto until = probability_horizon(m.as_of, d);
    const auto variance = priced ? implied_variance_until(m, until) : std::nullopt;
    const auto cone = [&](double k) -> json {
      if (!variance) return nullptr;
      const double spread = k * std::sqrt(*variance);
      return {{"low", price(m.spot * std::exp(-spread))}, {"high", price(m.spot * std::exp(spread))}};
    };
    json odds = json::array();
    for (const double level : prices) {
      if (!variance) {
        odds.push_back({{"price", level}, {"above", nullptr}, {"below", nullptr}, {"touch", nullptr}});
        continue;
      }
      const auto o = price_odds(m.spot, level, *variance);
      odds.push_back({{"price", level}, {"above", sig(o.above)}, {"below", sig(o.below)}, {"touch", sig(o.touch)}});
    }
    horizons.push_back({{"days", d}, {"until", md::format_timestamp(until)},
                        {"sigma", variance ? sig(std::sqrt(*variance)) : json(nullptr)},
                        {"one_sd", cone(1)}, {"two_sd", cone(2)}, {"prices", odds}});
  }
  return ok({{"symbol", m.symbol}, {"as_of", md::format_timestamp(m.as_of)}, {"spot", priced ? price(m.spot) : json(nullptr)},
             {"horizons", horizons},
             {"model", "Lognormal on the at-the-money implied variance to each horizon, forward at spot; "
                       "touch is the driftless reflection estimate"}});
}

ApiResponse candles_response(const MetricsSource& source, const std::string& symbol,
                             const std::map<std::string, std::string>& query) {
  const auto symbols = source.symbols();
  if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
    return error(404, "no data for " + symbol + " yet");
  const auto requested = query.find("interval");
  const auto interval = parse_bar_interval(requested == query.end() ? "5m" : requested->second);
  if (!interval) return error(400, "interval must be 1m, 5m, 15m, 30m, 1h or 1d");
  int limit = 500;
  if (!bounded_number(query, "limit", 1, 5000, limit))
    return error(400, "limit must be an integer in [1, 5000]");
  json bars = json::array();
  if (const auto* store = source.candles()) {
    for (const md::Bar& bar : store->bars(symbol, *interval, static_cast<std::size_t>(limit)))
      bars.push_back({{"t", bar.start / md::kNanosPerSecond}, {"o", price(bar.open)},
                      {"h", price(bar.high)}, {"l", price(bar.low)}, {"c", price(bar.close)}});
  }
  return ok({{"symbol", symbol}, {"interval", std::string(to_string(*interval))},
             {"bars", std::move(bars)}});
}

}  // namespace

bool query_decimal(const std::string& text, double& value) { return decimal_number(text, value); }

ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source) {
  if (request.method != "GET") return error(405, "only GET is supported");
  if (auto rejection = sandbox_visibility(request, source)) return *rejection;
  if (auto response = playbook_read(request, source)) return *response;
  if (auto response = paper_read(request, source)) return *response;
  const std::string_view target = request.target;
  const std::size_t question = target.find('?');
  const std::string_view path = target.substr(0, question);
  const auto parsed = query_parameters(question == std::string_view::npos ? std::string_view{}
                                                                          : target.substr(question + 1));
  if (!parsed) return error(400, "duplicate or malformed query parameter");
  const auto& query = *parsed;
  int expiries = 8;
  double window = 0.0;
  if (!bounded_number(query, "expiries", 1, 500, expiries)) {
    return error(400, "expiries must be an integer in [1, 500]");
  }
  if (!bounded_number(query, "window", 0.0, 1.0, window)) {
    return error(400, "window must be a finite number in [0, 1]");
  }

  if (path == "/api/status") {
    auto status = status_json(source);
    if (source.sandboxes()) status["sandboxes"] = {{"enabled", true}, {"idle_seconds", source.sandboxes()->idle().count()}};
    return {200, sandbox_tick(status.dump(), request.access)};
  }

  constexpr std::string_view prefix = "/api/underlyings/";
  if (!path.starts_with(prefix)) return error(404, "unknown endpoint");
  std::string_view rest = path.substr(prefix.size());
  const std::size_t slash = rest.find('/');
  const std::string symbol(rest.substr(0, slash));
  const std::string_view view =
      slash == std::string_view::npos ? "summary" : rest.substr(slash + 1);

  // History can arrive before the first analysis.
  if (view == "candles") return candles_response(source, symbol, query);
  if (view == "series") return series_response(source, symbol, query);
  const auto metrics = source.metrics(symbol);
  if (!metrics) return error(404, "no data for " + symbol + " yet");

  if (view == "summary") return ok(summary_json(*metrics));
  if (view == "probability") return probability_response(*metrics, query);
  if (view == "volatility") {
    auto response = volatility_json(metrics, source);
    add_volatility_history(response, source, metrics);
    return ok(response);
  }
  if (view == "chain") {
    const auto it = query.find("expiry");
    const SliceMetrics* slice = nullptr;
    for (const SliceMetrics& s : metrics->slices) {
      if (it == query.end() || expiry_id(s) == it->second) {
        slice = &s;
        break;
      }
    }
    if (!slice) return error(404, "no expiry " + (it == query.end() ? "" : it->second));
    return ok(chain_json(*metrics, *slice, window));
  }
  if (view == "exposure")
    return ok(exposure_json(*metrics, expiries, query.contains("window") ? window : 0.08));
  if (view == "surface")
    return ok(surface_json(metrics, expiries, query.contains("window") ? window : 0.2));
  return error(404, "unknown view " + std::string(view));
}

std::string tick_message(const MetricsSource& source, const ApiAccess& access) {
  const EngineStatus s = source.status();
  const auto now = source.wall_time();
  return sandbox_tick(json{{"type", "tick"},
              {"trading", trading_status_json(s.trading)},
              {"accounts", account_ticks_json(s)},
              {"circuit_breaker", circuit_breaker_json(s.circuit_breaker)},
              {"market", market_json(now)},
              {"feed", {{"state", md::to_string(s.feed_state)}, {"message", s.feed_message}}},
              {"underlyings", underlyings_json(source, s, false, now)},
              {"engine",
               {{"events_per_second", sig(s.events_per_second, 4)},
                {"analytics_ms", sig(s.analytics_ms, 4)},
                {"queue_depth", s.queue_depth},
                {"coalesced_events", s.coalesced_events},
                {"dropped_events", s.dropped_events},
                {"overloaded", s.overloaded},
                {"contracts", s.contracts},
                {"nonstandard_contracts", s.nonstandard_contracts}}}}
      .dump(), access);
}

}  // namespace openport::server
