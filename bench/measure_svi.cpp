// Deterministic SPX-sized workload, not a captured market-data accuracy claim.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "openport/analytics/svi.hpp"
#include "openport/analytics/ssvi.hpp"
#include "openport/analytics/sabr.hpp"
#include "openport/analytics/heston.hpp"
#ifdef OPENPORT_SVI_API_BENCH
#include "openport/server/api.hpp"
#endif

using namespace openport;

analytics::UnderlyingMetrics fixture() {
  analytics::UnderlyingMetrics m;
  m.symbol = "SPX"; m.spot = 5000; m.version = 1;
  m.as_of = md::new_york_to_utc({2026, 9, 23}, 16, 0);
  for (int i = 0; i < 60; ++i) {
    analytics::SliceMetrics s;
    const int days = 2 + i + i * i / 5;
    s.expiry = md::date_from_days(md::days_since_epoch({2026, 9, 23}) + days);
    s.expiry_time = md::new_york_to_utc(s.expiry, 16, 0);
    s.years = static_cast<double>(days) / 365;
    s.forward.forward = 5000 * std::exp(.04 * s.years);
    s.forward.discount = std::exp(-.04 * s.years);
    const analytics::SviParameters p{.015 * s.years, .10 * s.years, -.55, .025, .15};
    s.atm_iv = analytics::svi_iv(p, 0, s.years);
    for (int j = 0; j < 250; ++j) {
      const double k = -.7 + 1.1 * j / 249;
      analytics::StrikeMetrics row;
      row.strike = s.forward.forward * std::exp(k);
      row.iv = analytics::svi_iv(p, k, s.years) + .0002 * std::sin(2.3 * j + i);
      auto& side = k >= 0 ? row.call : row.put;
      side.bid = 1; side.ask = 1.1; side.mid = 1.05;
      const double spread = .002 + .006 * std::abs(k);
      side.bid_iv = row.iv - spread / 2; side.ask_iv = row.iv + spread / 2;
      s.strikes.push_back(row);
    }
    m.slices.push_back(s);
  }
  return m;
}

template <typename F> void measure(const char* label, F work) {
  std::vector<double> times;
  for (int i = 0; i < 30; ++i) {
    const auto start = std::chrono::steady_clock::now();
    if (!work()) { std::fprintf(stderr, "%s: workload failed\n", label); std::exit(1); }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (i >= 5) times.push_back(ms);
  }
  std::sort(times.begin(), times.end());
  std::printf("%s median_ms=%.3f p90_ms=%.3f\n", label, times[12], times[22]);
}

#ifdef OPENPORT_SVI_API_BENCH
class Source final : public server::MetricsSource {
 public:
  std::shared_ptr<const analytics::UnderlyingMetrics> m;
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return m; }
  server::EngineStatus status() const override { return {}; }
};
#endif

int main(int argc, char** argv) {
  const auto m = fixture();
#ifdef OPENPORT_SVI_API_BENCH
  Source source;
  source.m = std::make_shared<const analytics::UnderlyingMetrics>(m);
  if (argc == 3 && std::string(argv[1]) == "--fixture") {
    for (const auto* view : {"surface", "summary"}) {
      std::ofstream out(std::string(argv[2]) + "/" + view + ".json");
      out << server::handle_api({"GET", std::string("/api/underlyings/SPX/") + view + "?expiries=60&window=0"}, source).body;
    }
    return 0;
  }
#else
  (void)argc; (void)argv;
#endif
  measure("ssvi_surface_60x250", [&] { return analytics::fit_ssvi(m.slices).status == analytics::SviStatus::Ok; });
  measure("single_fit_250_points", [&] { return analytics::fit_svi(m.slices[20]).status == analytics::SviStatus::Ok; });
  measure("surface_60x250_fit_and_diagnostics", [&] {
    std::vector<analytics::SviFit> fits;
    for (const auto& s : m.slices) {
      fits.push_back(analytics::fit_svi(s));
      if (fits.back().status != analytics::SviStatus::Ok) return false;
    }
    const auto violations = analytics::svi_calendar(fits);
    return violations.empty();
  });
  // The SVI-shaped fixture is not a SABR smile: these time the optional overlay only.
  measure("sabr_single_fit_250_points_beta_1", [&] {
    return analytics::fit_sabr(m.slices[20], 1.0).status == analytics::SviStatus::Ok;
  });
  measure("sabr_single_fit_250_points_beta_fit", [&] {
    return analytics::fit_sabr(m.slices[20], std::nullopt).status == analytics::SviStatus::Ok;
  });
  measure("sabr_60x250_fits_and_calendar_beta_1", [&] {
    std::vector<analytics::SabrFit> fits;
    for (const auto& s : m.slices) {
      fits.push_back(analytics::fit_sabr(s, 1.0));
      if (fits.back().status != analytics::SviStatus::Ok) return false;
    }
    static_cast<void>(analytics::sabr_calendar(fits));
    return true;
  });
  // The research Heston fit over the eight leading expiries, as the API's default.
  measure("heston_8_expiries", [&] {
    return analytics::fit_heston(std::span(m.slices).first(8)).status == analytics::SviStatus::Ok;
  });
#ifdef OPENPORT_SVI_API_BENCH
  const server::ApiRequest request{"GET", "/api/underlyings/SPX/surface?expiries=60&window=0"};
  measure("api_60_expiries_cold", [&] {
    source.m = std::make_shared<const analytics::UnderlyingMetrics>(m);
    return server::handle_api(request, source).status == 200;
  });
  measure("api_60_expiries_cached", [&] { return server::handle_api(request, source).status == 200; });
  const server::ApiRequest sabr{"GET", "/api/underlyings/SPX/surface?expiries=60&window=0&sabr_beta=1"};
  measure("api_60_expiries_sabr_cold", [&] {
    source.m = std::make_shared<const analytics::UnderlyingMetrics>(m);
    return server::handle_api(sabr, source).status == 200;
  });
#endif
}
