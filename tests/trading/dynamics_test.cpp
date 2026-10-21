#include <gtest/gtest.h>

#include <cmath>
#include <optional>

#include "openport/analytics/ssvi.hpp"
#include "openport/trading/risk.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;

// An equity-like skew: IV falls as strikes rise. Thetas are ATM total variance.
SmileSurface skewed() {
  return {{-0.7, 1.1, 0.4}, {{0.05, 0.18 * 0.18 * 0.05}, {0.25, 0.19 * 0.19 * 0.25}, {1.0, 0.2 * 0.2 * 1.0}}};
}
double d1(double k, double vol, double years) { return (-k + 0.5 * vol * vol * years) / (vol * std::sqrt(years)); }

TEST(SmileDynamics, SurfaceInterpolatesThetaThroughTheOriginAndKnots) {
  const auto surface = skewed();
  EXPECT_DOUBLE_EQ(surface.theta(0), 0);
  EXPECT_DOUBLE_EQ(surface.theta(0.025), surface.thetas[0].second / 2);
  EXPECT_DOUBLE_EQ(surface.theta(0.25), surface.thetas[1].second);
  EXPECT_DOUBLE_EQ(surface.theta(0.625), (surface.thetas[1].second + surface.thetas[2].second) / 2);
  // Beyond the last knot ATM IV stays constant.
  EXPECT_NEAR(surface.iv(0, 2.0), 0.2, 1e-15);
  EXPECT_DOUBLE_EQ(surface.iv(0.1, 0.25), analytics::ssvi_iv(surface.parameters, 0.1, surface.thetas[1].second, 0.25));
  EXPECT_GT(surface.iv(-0.1, 0.25), surface.iv(0.1, 0.25));
  EXPECT_TRUE(std::isnan(surface.iv(0, 0)));
  EXPECT_TRUE(std::isnan(SmileSurface{}.iv(0, 0.25)));  // no knots
  EXPECT_EQ(parse_smile_dynamics("sticky_delta"), SmileDynamics::StickyDelta);
  EXPECT_EQ(to_string(SmileDynamics::StickyMoneyness), "sticky_moneyness");
  EXPECT_FALSE(parse_smile_dynamics("sticky"));
}

TEST(SmileDynamics, StickyMoneynessSlidesTheSmileWithTheForward) {
  const auto surface = skewed();
  const SmileModel strike{SmileDynamics::StickyStrike, nullptr};
  const SmileModel moneyness{SmileDynamics::StickyMoneyness, nullptr};
  const double smile_iv = 0.215, k = 5000, f = 5010, t = 0.25;
  // Sticky strike ignores the surface entirely.
  EXPECT_EQ(scenario_vol(strike, &surface, smile_iv, k, f, t, f * 0.9, t, 0.05), smile_iv + 0.05);
  // No move, no change: the market IV, not the fit, is kept.
  EXPECT_EQ(scenario_vol(moneyness, &surface, smile_iv, k, f, t, f, t, 0), smile_iv);
  const double down = *scenario_vol(moneyness, &surface, smile_iv, k, f, t, f * 0.9, t, 0);
  const double up = *scenario_vol(moneyness, &surface, smile_iv, k, f, t, f * 1.1, t, 0);
  EXPECT_NEAR(down - smile_iv, surface.iv(std::log(k / (f * 0.9)), t) - surface.iv(std::log(k / f), t), 1e-15);
  // On a negative skew a fixed strike becomes relatively higher (lower IV) as spot falls.
  EXPECT_LT(down, smile_iv);
  EXPECT_GT(up, smile_iv);
  // The parallel shift adds on top, and a later date reads the surface at the remaining life.
  EXPECT_NEAR(*scenario_vol(moneyness, &surface, smile_iv, k, f, t, f * 0.9, t, 0.03), down + 0.03, 1e-15);
  EXPECT_NEAR(*scenario_vol(moneyness, &surface, smile_iv, k, f, t, f, 0.1, 0) - smile_iv,
              surface.iv(std::log(k / f), 0.1) - surface.iv(std::log(k / f), t), 1e-15);
  // Without a surface or after expiry it is sticky strike.
  EXPECT_EQ(scenario_vol(moneyness, nullptr, smile_iv, k, f, t, f * 0.9, t, 0.01), smile_iv + 0.01);
  EXPECT_EQ(scenario_vol(moneyness, &surface, smile_iv, k, f, t, f * 0.9, 0, 0.01), smile_iv + 0.01);
  const SmileSurface empty;
  EXPECT_FALSE(scenario_vol(moneyness, &empty, smile_iv, k, f, t, f * 0.9, t, 0));
}

TEST(SmileDynamics, StickyDeltaKeepsTheSmileInDeltaSpace) {
  const auto surface = skewed();
  const SmileModel delta{SmileDynamics::StickyDelta, nullptr};
  const SmileModel moneyness{SmileDynamics::StickyMoneyness, nullptr};
  const double smile_iv = 0.215, k = 5200, f = 5010, t = 0.25;
  // Without a vol shock both definitions coincide at a fixed tenor.
  for (double move : {0.85, 0.95, 1.0, 1.05, 1.2})
    EXPECT_NEAR(*scenario_vol(delta, &surface, smile_iv, k, f, t, f * move, t, 0),
                *scenario_vol(moneyness, &surface, smile_iv, k, f, t, f * move, t, 0), 1e-10) << move;
  // With one they differ, and the result solves the defining equation: the contract's
  // shocked forward delta equals the delta of the smile point k* at its own vol.
  const auto smile = [&](double x) { return surface.iv(x, t) - surface.iv(std::log(k / f), t) + smile_iv; };
  for (double shift : {-0.05, 0.08}) {
    const double moved = f * 0.92;
    const double sigma = *scenario_vol(delta, &surface, smile_iv, k, f, t, moved, t, shift);
    EXPECT_GT(std::abs(sigma - *scenario_vol(moneyness, &surface, smile_iv, k, f, t, moved, t, shift)), 1e-4);
    const double base = sigma - shift;
    const double target = d1(std::log(k / moved), sigma, t);
    const double k_star = -target * base * std::sqrt(t) + 0.5 * base * base * t;  // d1(k*, base) = target
    EXPECT_NEAR(smile(k_star), base, 1e-9) << shift;
  }
  // A shock that drives the vol to zero or below has no delta; the caller floors it.
  EXPECT_LT(*scenario_vol(delta, &surface, smile_iv, k, f, t, f * 0.92, t, -0.5), 0);
}

TEST(SmileDynamics, RootSearchStopsOnAnExactZero) {
  // Roots at -0.04 and +0.08 are exact probes (0.01 doubled); a probe hitting zero
  // is the answer, not a bracket end to bisect away from.
  for (const double root : {-0.04, 0.08}) {
    const auto line = [root](double k) -> std::optional<double> { return root - k; };
    const auto found = detail::decreasing_root(line, 0, line(0).value());
    ASSERT_TRUE(found) << root;
    EXPECT_EQ(*found, root);
  }
  // Otherwise bisection converges on the root from either side.
  const auto cubic = [](double k) -> std::optional<double> { return 0.3 - k * k * k - k; };
  EXPECT_NEAR(*detail::decreasing_root(cubic, 1, *cubic(1)), 0.27841799032, 1e-10);
  EXPECT_NEAR(*detail::decreasing_root(cubic, -1, *cubic(-1)), 0.27841799032, 1e-10);
  // No sign change within the reach, or an undefined value on the way, is no root.
  EXPECT_FALSE(detail::decreasing_root([](double) -> std::optional<double> { return 1.0; }, 0, 1));
  EXPECT_FALSE(detail::decreasing_root([](double k) -> std::optional<double> {
    if (k > 0.05) return std::nullopt;
    return 1 - k;
  }, 0, 1));
}

TEST(SmileDynamics, FlatSurfacesMakeEveryDynamicsStickyStrike) {
  const SmileSurface flat{{0, 1e-9, 0.5}, {{0.25, 0.2 * 0.2 * 0.25}}};
  for (auto dynamics : {SmileDynamics::StickyMoneyness, SmileDynamics::StickyDelta})
    for (double shift : {0.0, 0.05})
      EXPECT_NEAR(*scenario_vol({dynamics, nullptr}, &flat, 0.21, 4800, 5000, 0.25, 4500, 0.25, shift), 0.21 + shift, 1e-9);
}

SessionConfig config() {
  SessionConfig c;
  c.initial_cash = Money::parse("10000");
  c.rules.max_drawdown = Money::parse("1000");
  c.limits.price_band_absolute = Money::parse("20");
  return c;
}

TEST(SmileDynamics, GridMatchesStickyStrikeWithoutASurfaceAndMovesWithOne) {
  ScriptedMarket f;
  const auto c = config();
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  const auto snapshot = s.snapshot();
  // Recomputed under sticky strike, the snapshot's own grid is reproduced exactly.
  const auto same = snapshot_scenarios(*snapshot, s.config(), s.valuations(), {});
  ASSERT_EQ(same.cells.size(), snapshot->scenarios.cells.size());
  for (std::size_t i = 0; i < same.cells.size(); ++i) EXPECT_EQ(same.cells[i].pnl, snapshot->scenarios.cells[i].pnl);
  EXPECT_TRUE(same.surface_fallback.empty());
  // Asked to move smiles without a surface, the grid says so and stays sticky strike.
  const SmileSurfaces none;
  const auto missing = snapshot_scenarios(*snapshot, s.config(), s.valuations(), {SmileDynamics::StickyMoneyness, &none});
  EXPECT_EQ(missing.surface_fallback, std::vector<std::string>{"SPX"});
  for (std::size_t i = 0; i < missing.cells.size(); ++i) EXPECT_EQ(missing.cells[i].pnl, snapshot->scenarios.cells[i].pnl);
  const SmileSurfaces surfaces{{"SPX", skewed()}};
  const auto& spots = c.scenarios.spot_percent;
  const auto& vols = c.scenarios.vol_points;
  for (auto dynamics : {SmileDynamics::StickyMoneyness, SmileDynamics::StickyDelta}) {
    const auto moved = snapshot_scenarios(*snapshot, s.config(), s.valuations(), {dynamics, &surfaces});
    ASSERT_TRUE(moved.complete);
    EXPECT_TRUE(moved.surface_fallback.empty());
    for (std::size_t i = 0; i < spots.size(); ++i)
      for (std::size_t j = 0; j < vols.size(); ++j) {
        const auto index = i * vols.size() + j;
        const double sticky = snapshot->scenarios.cells[index].pnl, pnl = moved.cells[index].pnl;
        // No move, same smile; sticky delta still applies a vol shock in delta space.
        if (spots[i] == 0 && (dynamics == SmileDynamics::StickyMoneyness || vols[j] == 0))
          EXPECT_NEAR(pnl, sticky, 1e-9) << index;
        // A long call on a negative skew: the smile's slide lowers its vol in a
        // selloff and raises it in a rally, relative to sticky strike.
        if (spots[i] < 0) EXPECT_LT(pnl, sticky) << index;
        if (spots[i] > 0) EXPECT_GT(pnl, sticky) << index;
      }
  }
}

TEST(SmileDynamics, ProfileCurvesMoveWithTheSurfaceAndReportFallbacks) {
  ScriptedMarket f;
  const auto c = config();
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 1), f.time).decision.ok());
  ProfileConfig p;
  p.percent = {-10, 0, 10};
  p.days = {0, 5};
  p.betas = {{"SPX", 1}};
  const auto sticky = snapshot_profile(*s.snapshot(), s.config(), s.valuations(), p, 5000);
  const SmileSurfaces surfaces{{"SPX", skewed()}};
  p.smile = {SmileDynamics::StickyMoneyness, &surfaces};
  const auto moved = snapshot_profile(*s.snapshot(), s.config(), s.valuations(), p, 5000);
  ASSERT_TRUE(moved.complete);
  EXPECT_TRUE(moved.surface_fallback.empty());
  EXPECT_EQ(moved.curves[0].pnl[1], 0.0);
  EXPECT_LT(moved.curves[0].pnl[0], sticky.curves[0].pnl[0]);
  EXPECT_GT(moved.curves[0].pnl[2], sticky.curves[0].pnl[2]);
  // Today's sticky-moneyness curve is the moved grid's zero-volatility column.
  ScenarioConfig scan;
  scan.spot_percent = p.percent;
  scan.vol_points = {0};
  auto grid_config = s.config();
  grid_config.scenarios = scan;
  const auto grid = snapshot_scenarios(*s.snapshot(), grid_config, s.valuations(), p.smile);
  for (std::size_t i = 0; i < p.percent.size(); ++i) EXPECT_DOUBLE_EQ(moved.curves[0].pnl[i], grid.cells[i].pnl);
  // Five days on the contract rolls down the surface even without a move.
  EXPECT_NE(moved.curves[1].pnl[1], sticky.curves[1].pnl[1]);
  const SmileSurfaces none;
  p.smile = {SmileDynamics::StickyDelta, &none};
  const auto fallback = snapshot_profile(*s.snapshot(), s.config(), s.valuations(), p, 5000);
  EXPECT_EQ(fallback.surface_fallback, std::vector<std::string>{"SPX"});
  EXPECT_EQ(fallback.curves[0].pnl, sticky.curves[0].pnl);
}

}  // namespace
}  // namespace openport::trading
