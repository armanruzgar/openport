#include <gtest/gtest.h>

#include "openport/trading/risk.hpp"
#include "openport/trading/stress.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
TEST(HistoricalStress, BuiltinsHaveSourcesTenorsAndRepriceSharesAndMargin) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  Ledger ledger; ledger.trade_stock("SPY", 100, Money::parse("100"), {});
  ASSERT_EQ(historical_stresses().size(), 4U);
  for (const auto& shock : historical_stresses()) {
    EXPECT_EQ(shock.label, "derived from public closes");
    EXPECT_FALSE(shock.sources.empty()); EXPECT_FALSE(shock.source_note.empty());
    ASSERT_EQ(shock.factors.at("SPY").vol_points.size(), 3U);
    const auto value = stress_book(ledger, {}, shock.factors, 0, Money::parse("10000"), Money::parse("9800"),
        time, md::kNanosPerMinute, {{"SPY", 100}});
    ASSERT_TRUE(value.complete);
    EXPECT_NEAR(value.pnl, 100 * shock.factors.at("SPY").spot_percent, 1e-8);
    ASSERT_TRUE(value.floor_breach); EXPECT_TRUE(*value.floor_breach);
    ASSERT_TRUE(value.margin);
  }
  const auto unknown = stress_book(ledger, {}, {}, 0, {}, {}, time, md::kNanosPerMinute, {{"SPY", 100}});
  EXPECT_FALSE(unknown.complete); EXPECT_FALSE(unknown.margin); EXPECT_FALSE(unknown.floor_breach);
}
TEST(HistoricalStress, OptionsUseTenorVolAndBothMarginModelsAndRefuseStaleBooks) {
  test::ScriptedMarket f;
  f.contract.type = pricing::OptionType::Put;
  Ledger ledger; ledger.fill(f.contract, -2, Money::parse("4"), {});
  Valuations values{{f.symbol(), f.valuation()}};
  const auto& shock = historical_stresses()[1];
  AccountRules rules;
  const auto strategy = stress_book(ledger, values, shock.factors, 0, Money::parse("10000"), Money::parse("9000"),
      f.time, md::kNanosPerMinute, {}, rules);
  ASSERT_TRUE(strategy.complete); ASSERT_TRUE(strategy.margin); EXPECT_LT(strategy.pnl, 0);
  rules.margin = MarginMode::Portfolio;
  const auto portfolio = stress_book(ledger, values, shock.factors, 0, Money::parse("10000"), {}, f.time, md::kNanosPerMinute, {}, rules);
  ASSERT_TRUE(portfolio.margin); EXPECT_GT(portfolio.margin->dollars(), 0);
  EXPECT_FALSE(stress_book(ledger, values, shock.factors, 0, {}, {}, f.time + 2 * md::kNanosPerMinute,
      md::kNanosPerMinute, {}).complete);
}
TEST(HistoricalStress, AShortDeliveringCashOnlyIsMarginedAtItsFixedPayoff) {
  test::ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261016C00500000");
  // After a 1:200 split the call delivers $51,000 in cash: $510 a unit against its
  // $500 strike, $10 a unit whatever the shares do, worth that discounted (0.99).
  const SharedMap<std::string, Deliverable> deliverables{{f.symbol(), Deliverable{0, Money::parse("51000")}}};
  const DeliverablesScope adjusted(&deliverables);
  Ledger ledger; ledger.fill(f.contract, -1, Money::parse("9.90"), {});
  Valuations values{{f.symbol(), f.valuation()}};
  for (const auto mode : {MarginMode::Strategy, MarginMode::Portfolio}) {
    AccountRules rules;
    rules.margin = mode;
    for (const auto& shock : historical_stresses()) {
      SCOPED_TRACE(shock.id);
      const auto stressed = stress_book(ledger, values, shock.factors, 0, Money::parse("10000"), {}, f.time,
          md::kNanosPerMinute, {}, rules);
      ASSERT_TRUE(stressed.complete);
      EXPECT_DOUBLE_EQ(stressed.pnl, 0);
      ASSERT_TRUE(stressed.margin);
      EXPECT_EQ(*stressed.margin, Money::parse("990"));
    }
  }
}
TEST(HistoricalRisk, EmpiricalQuantilesAndFractionalTailMean) {
  std::vector<double> losses;
  for (int i = 1; i <= 100; ++i) losses.push_back(i);
  const auto risk = loss_quantiles(losses);
  ASSERT_TRUE(risk.complete);
  EXPECT_EQ(risk.var95, 95); EXPECT_NEAR(*risk.es95, 98, 1e-10);
  EXPECT_EQ(risk.var99, 99); EXPECT_NEAR(*risk.es99, 100, 1e-10);
  EXPECT_FALSE(risk.tail99_reason);
  losses.resize(99);
  const auto thin = loss_quantiles(losses);
  ASSERT_TRUE(thin.complete);  // 95% stands; the 1% tail is under one observation
  EXPECT_TRUE(thin.var95); EXPECT_FALSE(thin.var99); EXPECT_FALSE(thin.es99);
  EXPECT_EQ(thin.tail99_reason, "Needs at least 100 observations; 99 available");
  losses.resize(21);
  const auto small = loss_quantiles(losses);
  EXPECT_EQ(small.var95, 20); EXPECT_NEAR(*small.es95, (21 + 0.05 * 20) / 1.05, 1e-10);
  EXPECT_FALSE(small.var99); EXPECT_FALSE(small.es99);
  EXPECT_FALSE(loss_quantiles({1, 2, 3}).complete);
  EXPECT_TRUE(loss_quantiles({1, 2, 3}).tail99_reason);
  EXPECT_EQ(loss_quantiles(std::vector<double>(20, -5)).var95, 0);
  EXPECT_EQ(loss_quantiles(std::vector<double>(100, -5)).var99, 0);
}
TEST(HistoricalRisk, AlignsBothEndpointsExcludesTodayAndCapsAt252Closes) {
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  Ledger ledger; ledger.trade_stock("SPY", 100, Money::parse("100"), {});
  ledger.trade_stock("QQQ", -50, Money::parse("200"), {});
  std::map<std::string, std::vector<HistoricalClose>> history;
  double price = 100;
  const auto first = md::days_since_epoch({2025, 1, 2});
  for (int i = 0; i < 400; ++i) {
    const auto date = md::date_from_days(first + i);
    if (!md::stock_session(md::new_york_to_utc(date, 12, 0)).open) continue;
    price *= 1 + (i % 2 ? 0.01 : -0.02);
    history["SPY"].push_back({date, price, 0.2 + 0.001 * (i % 5)});
    history["QQQ"].push_back({date, price * 2, 0.25 + 0.002 * (i % 5)});
  }
  const std::map<std::string, double> prices{{"SPY", 100}, {"QQQ", 200}};
  const auto paired = historical_risk(ledger, {}, history, now, md::kNanosPerMinute, prices);
  ASSERT_TRUE(paired.sticky_strike.complete); EXPECT_EQ(paired.sticky_strike.observations, 251U);
  EXPECT_NEAR(*paired.sticky_strike.var99, 0, 1e-8); // equal and opposite dollar exposures
  EXPECT_TRUE(paired.atm_vol_change.complete);
  history["QQQ"].erase(history["QQQ"].end() - 5);
  history["SPY"].push_back({{2026, 9, 22}, 1, {}}); // today's partial close is not a daily factor
  const auto missing = historical_risk(ledger, {}, history, now, md::kNanosPerMinute, prices);
  EXPECT_EQ(missing.sticky_strike.observations, 249U);
  for (auto& close : history["SPY"]) close.atm_iv.reset();
  const auto no_iv = historical_risk(ledger, {}, history, now, md::kNanosPerMinute, prices);
  EXPECT_TRUE(no_iv.sticky_strike.complete); EXPECT_FALSE(no_iv.atm_vol_change.complete);
}
TEST(HistoricalRisk, VolVariantRepricesOptionsWithObservedAtmChanges) {
  test::ScriptedMarket f;
  Ledger ledger; ledger.fill(f.contract, -1, Money::parse("4"), {});
  Valuations values{{f.symbol(), f.valuation()}};
  std::map<std::string, std::vector<HistoricalClose>> history;
  for (int i = 0; i < 45; ++i) {
    const auto date = md::date_from_days(md::days_since_epoch({2026, 7, 1}) + i);
    if (!md::stock_session(md::new_york_to_utc(date, 12, 0)).open) continue;
    history["SPX"].push_back({date, 5000, 0.2 + 0.001 * i});
  }
  const auto risk = historical_risk(ledger, values, history, f.time, md::kNanosPerMinute, {});
  ASSERT_TRUE(risk.atm_vol_change.complete);
  EXPECT_GT(*risk.atm_vol_change.es95, *risk.sticky_strike.es95);
  EXPECT_FALSE(risk.atm_vol_change.es99);  // too few moves for a 1% tail
}
}
}
