#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config() {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.rules.max_drawdown = m("1000");
  c.limits.price_band_absolute = m("20");
  return c;
}
ProfileConfig grid_profile(const SessionConfig& c, std::vector<double> days) {
  ProfileConfig p;
  p.percent = c.scenarios.spot_percent;
  p.days = std::move(days);
  p.betas = {{"SPX", 1}};
  return p;
}

TEST(TradingProfile, TodaysCurveIsTheScenarioGridsColumnAndItsLevelsAreTheBreachLevels) {
  ScriptedMarket f;
  const auto c = config();
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  const auto snapshot = s.snapshot();
  auto profile_config = grid_profile(c, {0});
  profile_config.variances = {0.0004};
  const auto profile = snapshot_profile(*snapshot, s.config(), s.valuations(), profile_config, 5000);
  ASSERT_TRUE(profile.complete);
  ASSERT_EQ(profile.curves.size(), 1U);
  const auto& curve = profile.curves.front();
  // The grid is spot-major; its zero-volatility column is the second of four.
  const auto& vols = c.scenarios.vol_points;
  const auto column = static_cast<std::size_t>(std::find(vols.begin(), vols.end(), 0.0) - vols.begin());
  ASSERT_EQ(curve.pnl.size(), c.scenarios.spot_percent.size());
  for (std::size_t i = 0; i < curve.pnl.size(); ++i)
    EXPECT_EQ(curve.pnl[i], snapshot->scenarios.cells[i * vols.size() + column].pnl) << i;
  const auto breach = s.breach({{"SPX", 0.0004}});
  ASSERT_EQ(breach.underlyings.size(), 1U);
  ASSERT_TRUE(curve.down); ASSERT_TRUE(breach.underlyings.front().down);
  EXPECT_EQ(curve.down->percent, breach.underlyings.front().down->percent);
  EXPECT_EQ(curve.down->touch_probability, breach.underlyings.front().down->touch_probability);
  EXPECT_EQ(profile.room, breach.room);
  EXPECT_EQ(curve.time, snapshot->time);
}

TEST(TradingProfile, LaterCurvesDecayAndExpiredContractsAreWorthTheirIntrinsicValue) {
  ScriptedMarket f;
  const auto c = config();
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 1), f.time).decision.ok());
  ProfileConfig p;
  p.percent = {-10, 0, 10};
  p.betas = {{"SPX", 1}};
  // The call expires 2026-10-22, thirty days on; a curve forty days on is past it.
  p.days = {0, 1, 40};
  const auto profile = snapshot_profile(*s.snapshot(), s.config(), s.valuations(), p, 5000);
  ASSERT_TRUE(profile.complete);
  ASSERT_EQ(profile.curves.size(), 3U);
  EXPECT_EQ(profile.curves[0].pnl[1], 0.0);
  EXPECT_LT(profile.curves[1].pnl[1], 0.0);  // a day of time value gone
  EXPECT_LT(profile.curves[1].pnl[1], profile.curves[0].pnl[1]);
  EXPECT_EQ(profile.curves[1].time, f.time + md::kNanosPerDay);
  const auto v = f.valuation();
  const double base = pricing::black_price(f.contract.type, v.forward, f.contract.strike, v.years, v.smile_iv, v.discount);
  // At expiry the 5000 call is worth what spot is above its strike: 500 at +10%.
  EXPECT_NEAR(profile.curves[2].pnl[2], 100 * (500 - base), 1e-6);
  EXPECT_NEAR(profile.curves[2].pnl[0], -100 * base, 1e-6);
  // Higher volatility lifts a long call's value today.
  p.days = {0};
  p.vol_points = 5;
  const auto richer = snapshot_profile(*s.snapshot(), s.config(), s.valuations(), p, 5000);
  EXPECT_GT(richer.curves[0].pnl[1], 0.0);
  EXPECT_FALSE(richer.curves[0].clamped);
  p.vol_points = -50;
  EXPECT_TRUE(snapshot_profile(*s.snapshot(), s.config(), s.valuations(), p, 5000).curves[0].clamped);
}

TEST(TradingProfile, UnderlyingsMoveByTheirBetaAndOneWithoutABetaLeavesTheProfileIncomplete) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  Ledger ledger(m("10000"));
  ledger.trade_stock("SPY", 100, m("100"), {});
  ledger.trade_stock("QQQ", -50, m("200"), {});
  ProfileConfig p;
  p.percent = {-5, 0, 1};
  p.betas = {{"SPY", 1}, {"QQQ", 1.5}};
  const std::map<std::string, double> prices{{"SPY", 100}, {"QQQ", 200}};
  // SPY gains 100 at +1%; short QQQ moves 1.5% and loses 150.
  const auto profile = risk_profile(ledger, {}, p, 600, m("10000"), m("9000"), {}, time, md::kNanosPerMinute, prices);
  ASSERT_TRUE(profile.complete);
  EXPECT_NEAR(profile.curves[0].pnl[2], 100 - 150, 1e-9);
  // Equity falls 50 per 1% up: the floor's 1,000 is 20% up, and 1% up is 600 * 1.01.
  ASSERT_TRUE(profile.curves[0].up);
  EXPECT_NEAR(profile.curves[0].up->percent, 20, 1e-7);
  EXPECT_NEAR(profile.curves[0].up->points, 120, 1e-5);
  EXPECT_FALSE(profile.curves[0].down);
  p.betas.erase("QQQ");
  const auto missing = risk_profile(ledger, {}, p, 600, m("10000"), m("9000"), {}, time, md::kNanosPerMinute, prices);
  EXPECT_FALSE(missing.complete);
  EXPECT_EQ(missing.curves[0].pnl[2], 0.0);
  EXPECT_FALSE(missing.curves[0].up);
  p.days = {0, 400};
  EXPECT_THROW((void)risk_profile(ledger, {}, p, 600, m("10000"), {}, {}, time, md::kNanosPerMinute, prices), TradingError);
}

TEST(TradingProfile, ScenarioAxesReuseBetaAndTimePricingAndRequireAllBetas) {
  test::ScriptedMarket f;
  Ledger ledger;
  ledger.fill(f.contract, 1, Money::parse("4.20"), {});
  ledger.trade_stock("QQQ", 10, Money::parse("100"), {});
  Valuations values{{f.symbol(), f.valuation()}};
  ScenarioConfig grid;
  grid.spot_percent = {-1, 0, 1}; grid.vol_points = {0, 5}; grid.days = {0, 1, 40};
  grid.beta_weighted = true; grid.betas = {{"SPX", 1}, {"QQQ", 2}};
  const auto result = scenario_grid(ledger, values, grid, f.time, md::kNanosPerMinute, {{"QQQ", 100}});
  ASSERT_TRUE(result.complete);
  ASSERT_EQ(result.cells.size(), 18U);
  EXPECT_EQ(result.cells[2].pnl, 0);
  EXPECT_EQ(result.cells[6].days, 1);
  EXPECT_LT(result.cells[8].pnl, 0);
  grid.betas.erase("QQQ");
  const auto missing = scenario_grid(ledger, values, grid, f.time, md::kNanosPerMinute, {{"QQQ", 100}});
  EXPECT_FALSE(missing.complete);
  for (const auto& cell : missing.cells) EXPECT_EQ(cell.pnl, 0);
  grid.days = {-1};
  EXPECT_THROW(validate_scenarios(grid), TradingError);
}
TEST(TradingProfile, TermScalingIncreasesShortTenorVolShocksAndClampsAtTheFloor) {
  test::ScriptedMarket f;
  Ledger ledger; ledger.fill(f.contract, 1, Money::parse("4.20"), {});
  auto value = f.valuation(); value.years = 7.0 / 365;
  Valuations values{{f.symbol(), value}};
  ScenarioConfig grid; grid.spot_percent = {0}; grid.vol_points = {5};
  const auto flat = scenario_grid(ledger, values, grid, f.time, md::kNanosPerMinute);
  grid.term_scaled = true;
  const auto scaled = scenario_grid(ledger, values, grid, f.time, md::kNanosPerMinute);
  EXPECT_GT(scaled.cells[0].pnl, flat.cells[0].pnl);
  grid.vol_points = {-50};
  EXPECT_TRUE(scenario_grid(ledger, values, grid, f.time, md::kNanosPerMinute).cells[0].clamped);
}
}  // namespace
}  // namespace openport::trading
