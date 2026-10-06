#include <cmath>
#include <cstring>
#include <limits>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view text) { return Money::parse(text); }
TEST(TradingRisk, OrderValidationAndExplicitNumericChecks) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  EXPECT_EQ(s.submit(f.market("unknown"), f.time).decision.code, Reason::UNKNOWN_CONTRACT);
  f.seed(s);
  auto request = f.market("day"); request.tif = TimeInForce::Day;
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_ORDER);
  request = f.limit("tick", 1, "4.21");
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_TICK);
  EXPECT_EQ(s.submit(f.market("zero", 0), f.time).decision.code, Reason::INVALID_ORDER);
  const auto max = s.submit(f.market("max", 101), f.time).decision;
  EXPECT_EQ(max.code, Reason::MAX_ORDER_CONTRACTS);
  EXPECT_EQ(max.actual, 101); EXPECT_EQ(max.limit, 100);
  const auto band = s.submit(f.limit("band", 1, "5.00"), f.time).decision;
  EXPECT_EQ(band.code, Reason::PRICE_BAND);
  EXPECT_DOUBLE_EQ(*band.actual, 0.9);
  EXPECT_DOUBLE_EQ(*band.limit, 0.82);
  const auto okay = s.submit(f.limit("okay", 1, "4.10"), f.time);
  EXPECT_TRUE(okay.decision.ok());
  // A retry with the same terms gets the first answer and records nothing; other terms reject.
  const auto orders = s.snapshot()->recent_orders.size();
  const auto retry = s.submit(f.limit("okay", 1, "4.10"), f.time);
  EXPECT_TRUE(retry.decision.ok());
  EXPECT_TRUE(retry.replayed);
  EXPECT_EQ(retry.order_id, okay.order_id);
  EXPECT_EQ(retry.account_version, okay.account_version);
  const auto band_retry = s.submit(f.limit("band", 1, "5.00"), f.time);
  EXPECT_EQ(band_retry.decision.code, Reason::PRICE_BAND);
  EXPECT_TRUE(band_retry.replayed);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), orders);
  EXPECT_EQ(s.submit(f.limit("okay", 2, "4.10"), f.time).decision.code, Reason::DUPLICATE_CLIENT_ID);
  auto limits = Limits{};
  limits.price_band_relative = 0;
  limits.price_band_absolute = m("0.05");
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.submit(f.market("wide"), f.time).decision.code, Reason::PRICE_BAND);
}
TEST(TradingRisk, FreshQuotesAndValuationsAreRequired) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  s.define(f.contract, f.time);
  EXPECT_EQ(s.submit(f.market("noquote"), f.time).decision.code, Reason::INVALID_QUOTE);
  s.on_quotes({f.quote()}, {}, f.time);
  EXPECT_EQ(s.submit(f.market("noval"), f.time).decision.code, Reason::MISSING_VALUATION);
  s.on_quotes({}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.submit(f.market("position"), f.time).decision.ok());
  f.time += 61 * md::kNanosPerSecond;
  EXPECT_EQ(s.submit(f.market("stale"), f.time).decision.code, Reason::STALE_QUOTE);
  EXPECT_FALSE(s.snapshot()->valuation_complete);
  EXPECT_FALSE(s.snapshot()->risk.complete);
  EXPECT_EQ(s.snapshot()->positions[0].mark, m("4.10"));
  ++f.observation;
  s.on_quotes({f.quote()}, {}, f.time);
  EXPECT_EQ(s.submit(f.market("staleval"), f.time).decision.code, Reason::MISSING_VALUATION);
}
TEST(TradingRisk, LastTradeCapsFreshnessWithoutRevivingStaleValuations) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPX260918C06020000");
  const Limits limits;
  f.time = f.contract.last_trade_time() - limits.max_valuation_age;
  Ledger ledger;
  ledger.fill(f.contract, 1, m("4.20"), {});
  for (const auto now : {md::new_york_to_utc({2026, 9, 17}, 16, 20),
                         md::new_york_to_utc({2026, 9, 17}, 21, 0)}) {
    SCOPED_TRACE(now);
    auto value = f.valuation();
    EXPECT_EQ(observation_time(f.contract, now), f.contract.last_trade_time());
    EXPECT_TRUE(portfolio_risk(ledger, {}, {}, {{f.symbol(), value}}, limits, now).complete);
    --value.time;
    EXPECT_FALSE(portfolio_risk(ledger, {}, {}, {{f.symbol(), value}}, limits, now).complete);
  }
}

TEST(TradingRisk, PendingOrdersReserveWorstSubsetWithoutNettingOppositeSides) {
  ScriptedMarket f;
  SessionConfig c;
  c.limits.per_underlying.dollar_delta = 600'000;
  c.limits.aggregate.dollar_delta = 600'000;
  TradingSession s(c, f.time);
  f.seed(s, "4", "4.40");
  EXPECT_TRUE(s.submit(f.limit("buy", 2, "4.10"), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.limit("sell", 2, "4.30", Side::Sell), f.time).decision.ok());
  auto risk = s.snapshot()->risk;
  EXPECT_DOUBLE_EQ(risk.aggregate.reachable.delta_low, -500'000);
  EXPECT_DOUBLE_EQ(risk.aggregate.reachable.delta_high, 500'000);
  const auto reject = s.submit(f.limit("buy2", 1, "4.10"), f.time).decision;
  EXPECT_EQ(reject.code, Reason::DELTA_LIMIT);
  EXPECT_EQ(reject.scope, "SPX");
  EXPECT_EQ(reject.actual, 750'000);
  EXPECT_EQ(reject.limit, 600'000);
  EXPECT_NEAR(s.snapshot()->risk.aggregate.delta_utilisation, 5.0 / 6, 1e-12);
}
TEST(TradingRisk, ABookOverItsDeltaLimitStillClosesHedgesAndTradesOtherUnderlyings) {
  // Three calls at delta 0.5 are 750,000 dollar delta; the SPX limit then tightens to 300,000.
  ScriptedMarket f;
  ScriptedMarket put = f;
  put.contract = *md::parse_osi("SPXW261022P05000000");
  ScriptedMarket xsp = f;
  xsp.contract = *md::parse_osi("XSP261022C00500000");
  TradingSession s({}, f.time);
  f.seed(s); xsp.seed(s);
  s.define(put.contract, f.time);
  s.on_quotes({put.quote()}, {put.valuation(-0.5)}, f.time);
  ASSERT_TRUE(s.submit(f.market("open", 3), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("target", 1, "4.50", Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("add", 1, "3.90"), f.time).decision.ok());
  auto limits = Limits{};
  limits.per_underlying.dollar_delta = 300'000;
  s.set_limits(limits, f.time);
  // The resting buy would raise SPX's delta and is cancelled; the resting close stays.
  EXPECT_EQ(s.snapshot()->recent_orders[2].reason.code, Reason::RISK_CHANGED);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Working);
  // A close fills although the position alone is over the limit.
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 2);
  // So do an order on another underlying inside its own limits, and a hedge.
  EXPECT_TRUE(s.submit(xsp.market("other"), f.time).decision.ok());
  EXPECT_TRUE(s.submit(put.market("hedge"), f.time).decision.ok());
  // Adding to the position still refuses.
  const auto more = s.submit(f.limit("more", 1, "4.20"), f.time).decision;
  EXPECT_EQ(more.code, Reason::DELTA_LIMIT);
  EXPECT_EQ(more.scope, "SPX");
  // The resting take-profit fills when it becomes marketable, with the book over the limit again.
  f.next();
  s.on_quotes({f.quote("4.60", "4.80")}, {f.valuation(0.9)}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders[1].filled_notional, m("4.60"));
}
TEST(TradingRisk, ClosingAHedgeOverTheDeltaLimitIsRefusedButClosingExposureFills) {
  ScriptedMarket call;
  ScriptedMarket put = call;
  put.contract = *md::parse_osi("SPXW261022P05000000");
  TradingSession s({}, call.time);
  call.seed(s);
  s.define(put.contract, call.time);
  s.on_quotes({put.quote()}, {put.valuation(-0.5)}, call.time);
  ASSERT_TRUE(s.submit(call.market("calls", 3), call.time).decision.ok());
  ASSERT_TRUE(s.submit(put.market("hedge"), call.time).decision.ok());
  const auto held_delta = s.snapshot()->risk.underlyings.at("SPX").position.dollar_delta;
  ASSERT_DOUBLE_EQ(held_delta, 500'000);
  auto limits = s.config().limits;
  limits.per_underlying.dollar_delta = 0.8 * held_delta;
  ASSERT_TRUE(s.set_limits(limits, call.time).decision.ok());

  const auto warnings = s.warnings();
  const RiskWarning* delta_warning = nullptr;
  for (const auto& warning : warnings) {
    if (warning.code == "DELTA_LIMIT" && warning.scope == "SPX") {
      delta_warning = &warning;
    }
  }
  ASSERT_NE(delta_warning, nullptr);
  EXPECT_EQ(delta_warning->message,
            "SPX's dollar delta 500,000 is over its 400,000 limit: orders that raise it are refused; "
            "closes and hedges that lower it still go");

  const auto rejected = s.submit(put.market("close-hedge", 1, Side::Sell), call.time).decision;
  EXPECT_EQ(rejected.code, Reason::DELTA_LIMIT);
  EXPECT_EQ(rejected.scope, "SPX");
  EXPECT_EQ(rejected.actual, 750'000);
  EXPECT_EQ(rejected.limit, 400'000);
  EXPECT_DOUBLE_EQ(s.snapshot()->risk.underlyings.at("SPX").position.dollar_delta, held_delta);
  const auto close = s.submit(call.market("close-call", 1, Side::Sell), call.time);
  ASSERT_TRUE(close.decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
  EXPECT_DOUBLE_EQ(s.snapshot()->risk.underlyings.at("SPX").position.dollar_delta, 250'000);
}
TEST(TradingRisk, AggregateAndUnderlyingVegaLimitsHaveIndependentChecks) {
  ScriptedMarket a;
  ScriptedMarket b;
  b.contract = *md::parse_osi("XSP261022C00500000");
  SessionConfig config;
  config.limits.aggregate.vega = 300;
  config.limits.per_underlying.vega = 250;
  TradingSession s(config, a.time);
  a.seed(s); b.seed(s);
  EXPECT_TRUE(s.submit(a.limit("spx", 1, "4.10"), a.time).decision.ok());
  auto d = s.submit(b.limit("xsp", 1, "4.10"), a.time).decision;
  EXPECT_EQ(d.code, Reason::VEGA_LIMIT);
  EXPECT_EQ(d.scope, "aggregate"); EXPECT_EQ(d.actual, 400); EXPECT_EQ(d.limit, 300);
  config.limits.underlying_overrides["XSP"] = {1e6, 150};
  s.set_limits(config.limits, a.time);
  d = s.submit(b.limit("xsp2", 1, "4.10"), a.time).decision;
  EXPECT_EQ(d.code, Reason::VEGA_LIMIT); EXPECT_EQ(d.scope, "XSP");
  config.limits.aggregate.dollar_delta = 300'000;
  config.limits.aggregate.vega = 10000;
  config.limits.underlying_overrides.clear();
  s.set_limits(config.limits, a.time);
  d = s.submit(b.limit("xsp3", 1, "4.10"), a.time).decision;
  EXPECT_EQ(d.code, Reason::DELTA_LIMIT); EXPECT_EQ(d.scope, "aggregate");
}
TEST(TradingRisk, FillRecheckCancelsRemainderWhenGreeksChangeAndWaitsOutMissingValuations) {
  for (const bool missing : {false, true}) {
    ScriptedMarket f;
    SessionConfig c;
    c.limits.aggregate.dollar_delta = 300'000;
    TradingSession s(c, f.time);
    f.seed(s);
    s.submit(f.limit("rest", 1, "4.10"), f.time);
    f.next();
    auto valuation = f.valuation(1);
    if (missing) valuation.delta = std::numeric_limits<double>::quiet_NaN();
    s.on_quotes({f.quote("4", "4.10")}, {valuation}, f.time);
    EXPECT_TRUE(s.snapshot()->recent_fills.empty());
    if (!missing) {
      EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
      EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Cancelled);
      continue;
    }
    // Without a valuation the fill waits; the same quote, offered again with one, fills.
    EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Working);
    s.on_quotes({f.quote("4", "4.10")}, {f.valuation()}, f.time);
    ASSERT_EQ(s.snapshot()->recent_fills.size(), 1U);
    EXPECT_EQ(s.snapshot()->recent_fills[0].price, m("4.10"));
    EXPECT_EQ(s.snapshot()->recent_fills[0].observation, f.observation);
  }
}
TEST(TradingRisk, LimitTighteningRechecksAndInvalidLimitsLeaveStateUntouched) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  s.submit(f.limit("rest", 2, "4.10"), f.time);
  auto limits = Limits{};
  limits.max_order_contracts = 1;
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
  EXPECT_EQ(s.snapshot()->risk.limits_revision, 2);
  const auto before = s.snapshot_json();
  limits.aggregate.vega = -1;
  EXPECT_THROW(s.set_limits(limits, f.time), TradingError);
  EXPECT_EQ(s.snapshot_json(), before);
}
TEST(TradingRisk, ManualKillLatchAndExplicitReset) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  s.submit(f.limit("rest", 1, "4.10"), f.time);
  s.trip_kill("operator pause", f.time);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::KILL_SWITCH);
  EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::KILL_SWITCH);
  EXPECT_THROW(s.reset_kill("  ", f.time), TradingError);
  s.reset_kill("reviewed", f.time);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_TRUE(s.submit(f.market("allowed"), f.time).decision.ok());
}
TEST(TradingRisk, DailyLossCancelsBeforeMatchingAndResetImmediatelyRetrips) {
  ScriptedMarket f;
  SessionConfig c;
  c.limits.max_daily_loss = m("100");
  TradingSession s(c, f.time);
  f.seed(s);
  s.submit(f.market("position"), f.time);
  s.submit(f.limit("rest", 1, "4.10"), f.time);
  f.next();
  s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "DAILY_LOSS");
  EXPECT_EQ(s.snapshot()->risk.daily_loss, m("310.65"));
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->recent_orders[1].reason.code, Reason::KILL_SWITCH);
  EXPECT_EQ(s.reset_kill("reviewed", f.time).decision.code, Reason::DAILY_LOSS);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_TRUE(s.submit(f.limit("reduce-after-reset", 1, "1.50", Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(f.market("still-blocked"), f.time).decision.code, Reason::KILL_SWITCH);
  EXPECT_EQ(s.roll_day(f.time).decision.code, Reason::INVALID_TIME);
  // The day's marks went stale before it ended, so its close is unknown until new quotes.
  s.on_quotes({}, {}, f.time + 2 * md::kNanosPerMinute);
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ++f.observation;
  EXPECT_EQ(s.roll_day(f.time).decision.code, Reason::STALE_QUOTE);
  s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
  s.roll_day(f.time);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->risk.daily_loss, Money{});
  s.reset_kill("new daily baseline approved", f.time);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingRisk, ProposedFillIncludesSpreadAndFeesInDailyLossCheck) {
  ScriptedMarket f;
  SessionConfig c;
  c.limits.max_daily_loss = m("10");
  TradingSession s(c, f.time);
  f.seed(s);
  s.submit(f.market("tooexpensive"), f.time);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::RISK_CHANGED);
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.actual, 10.65);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingRisk, GreekUnitsAndScenarioSignsForCallsPutsLongsShorts) {
  ScriptedMarket f;
  for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
    for (const Quantity direction : {Quantity{-1}, Quantity{1}}) {
      f.contract.type = type;
      Ledger ledger(m("100000"));
      ledger.fill(f.contract, direction, m("4.20"), m("0.65"));
      const auto v = f.valuation();
      const Valuations values{{f.symbol(), v}};
      const auto risk = portfolio_risk(ledger, {}, {{f.symbol(), f.contract}}, values, {}, f.time);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.dollar_delta, static_cast<double>(direction) * 250000);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.dollar_gamma_1pct, static_cast<double>(direction) * 25000);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.vega, static_cast<double>(direction) * 200);
      EXPECT_DOUBLE_EQ(risk.aggregate.position.theta, static_cast<double>(direction) * -10);
      ScenarioConfig config;
      config.spot_percent = {-1, 0, 1}; config.vol_points = {0, 5};
      const auto grid = scenario_grid(ledger, values, config, f.time, md::kNanosPerMinute);
      ASSERT_TRUE(grid.complete);
      EXPECT_EQ(grid.cells[2].pnl, 0);
      EXPECT_GT(grid.cells[3].pnl * static_cast<double>(direction), 0);
      const double call_sign = type == pricing::OptionType::Call ? 1 : -1;
      EXPECT_GT(grid.cells[4].pnl * static_cast<double>(direction) * call_sign, 0);
      EXPECT_LT(grid.cells[0].pnl * static_cast<double>(direction) * call_sign, 0);
      config.vol_points = {-50};
      const auto clamped = scenario_grid(ledger, values, config, f.time, md::kNanosPerMinute);
      EXPECT_TRUE(clamped.cells[0].clamped);
      EXPECT_TRUE(std::isfinite(clamped.cells[0].pnl));
      const auto missing = scenario_grid(ledger, {}, config, f.time, md::kNanosPerMinute);
      EXPECT_FALSE(missing.complete);
      config.spot_percent = {-100};
      EXPECT_THROW((void)scenario_grid(ledger, values, config, f.time, md::kNanosPerMinute), TradingError);
    }
  }
}
TEST(TradingRisk, ScenarioGridKeepsCellAndMixedBookSummationOrder) {
  ScriptedMarket call, put;
  put.contract.type = pricing::OptionType::Put;
  auto call_value = call.valuation(), put_value = put.valuation();
  call_value.smile_iv = 0.1;
  put_value.smile_iv = 0.3;
  const Valuations values{{call.symbol(), call_value}, {put.symbol(), put_value}};
  Ledger calls, puts, mixed;
  calls.fill(call.contract, 3, m("4.20"), {});
  puts.fill(put.contract, -2, m("4.20"), {});
  // Insert in reverse order; the ledger sums calls, puts, then stocks by symbol.
  mixed.fill(put.contract, -2, m("4.20"), {});
  mixed.fill(call.contract, 3, m("4.20"), {});
  mixed.trade_stock("QQQ", -11, m("411.17"), {});
  mixed.trade_stock("AAPL", 7, m("199.13"), {});
  const ScenarioConfig config{{1, 0, -2, 1}, {5, 0, -50}, 0.2};
  const auto call_grid = scenario_grid(calls, values, config, call.time, md::kNanosPerMinute);
  const auto put_grid = scenario_grid(puts, values, config, call.time, md::kNanosPerMinute);
  const auto grid = scenario_grid(mixed, values, config, call.time, md::kNanosPerMinute,
                                  {{"AAPL", 199.13}, {"QQQ", 411.17}});
  ASSERT_TRUE(grid.complete);
  ASSERT_EQ(grid.cells.size(), 12u);
  for (std::size_t i = 0; i < grid.cells.size(); ++i) {
    const auto& cell = grid.cells[i];
    EXPECT_EQ(cell.spot_percent, config.spot_percent[i / 3]);
    EXPECT_EQ(cell.vol_points, config.vol_points[i % 3]);
    double expected = call_grid.cells[i].pnl;
    expected += put_grid.cells[i].pnl;
    expected += 7.0 * 199.13 * cell.spot_percent / 100;
    expected += -11.0 * 411.17 * cell.spot_percent / 100;
    EXPECT_EQ(std::memcmp(&cell.pnl, &expected, sizeof expected), 0);
    EXPECT_TRUE(cell.clamped);
  }
  EXPECT_EQ(grid.cells[4].pnl, 0);  // Zero shock still bypasses the volatility floor.
}
TEST(TradingRisk, ScenarioGridSkipsUnvaluedPositionsWithoutClampingThem) {
  for (const std::string_view problem : {"missing", "stale", "expired", "invalid", "future", "negative", "nonfinite"}) {
    SCOPED_TRACE(problem);
    ScriptedMarket good, bad;
    bad.contract.type = pricing::OptionType::Put;
    if (problem == "expired") bad.contract.expiry = {2026, 9, 21};
    auto value = bad.valuation();
    value.years = 0.1;
    value.smile_iv = 0.01;  // Would clamp even the zero-shock cell if it were valued.
    if (problem == "stale") value.time -= md::kNanosPerMinute + 1;
    if (problem == "invalid") value.valid = false;
    if (problem == "future") ++value.time;
    if (problem == "negative") value.time = -1;
    if (problem == "nonfinite") value.forward = std::numeric_limits<double>::infinity();
    Ledger ledger;
    ledger.fill(good.contract, 1, m("4.20"), {});
    ledger.fill(bad.contract, -1, m("4.20"), {});
    auto fresh_value = good.valuation();
    fresh_value.time -= md::kNanosPerMinute;  // The inclusive freshness boundary.
    Valuations values{{good.symbol(), fresh_value}};
    if (problem != "missing") values[bad.symbol()] = value;
    const ScenarioConfig config{{-1, 0, 1}, {0, 5}, 0.1};
    const auto grid = scenario_grid(ledger, values, config, good.time, md::kNanosPerMinute);
    EXPECT_FALSE(grid.complete);
    ASSERT_EQ(grid.cells.size(), 6u);
    for (const auto& cell : grid.cells) {
      EXPECT_EQ(cell.pnl, 0);
      EXPECT_FALSE(cell.clamped);
    }
  }
}
TEST(TradingRisk, ScenarioGridRejectsEmptyAxes) {
  ScriptedMarket f;
  Ledger ledger;
  ledger.fill(f.contract, 1, m("4.20"), {});
  for (const auto& config : {ScenarioConfig{{}, {0}}, ScenarioConfig{{0}, {}}, ScenarioConfig{{}, {}}}) {
    EXPECT_THROW((void)scenario_grid(ledger, {}, config, f.time, md::kNanosPerMinute), TradingError);
  }
}
TEST(TradingSettlement, AmSettlementRequiresExplicitValueAndWorksWhileKilled) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPX260923C05000000");
  TradingSession s({}, f.time);
  f.seed(s);
  s.submit(f.market("position"), f.time);
  s.trip_kill("expiry review", f.time);
  f.time = f.contract.expiry_time();
  s.on_quotes({}, {}, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 1);
  EXPECT_TRUE(s.snapshot()->positions[0].awaiting_settlement);
  EXPECT_EQ(s.snapshot()->account.cash, m("99579.35"));
  EXPECT_TRUE(s.settle(f.symbol(), m("5010"), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->account.cash, m("100579.35"));
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
}
TEST(TradingRisk, RolloverFollowsTheTradingDateAcrossDst) {
  // A trading date ends at 17:00 New York time, 22:00 UTC in winter and 21:00 in summer.
  const auto at = [](const char* text) { return *md::parse_datetime(text, md::Zone::Utc); };
  TradingSession s({}, at("2026-03-06T17:00:00Z"));  // Friday before the spring-forward weekend
  EXPECT_EQ(s.trading_day(), (md::Date{2026, 3, 6}));
  EXPECT_EQ(s.roll_day(at("2026-03-06T21:59:59Z")).decision.code, Reason::INVALID_TIME);
  EXPECT_TRUE(s.roll_day(at("2026-03-06T22:00:00Z")).decision.ok());
  EXPECT_EQ(s.trading_day(), (md::Date{2026, 3, 9}));
  // The weekend, and Sunday evening's overnight session, belong to Monday.
  EXPECT_EQ(s.roll_day(at("2026-03-08T23:00:00Z")).decision.code, Reason::INVALID_TIME);
  EXPECT_EQ(s.roll_day(at("2026-03-09T20:59:59Z")).decision.code, Reason::INVALID_TIME);
  EXPECT_TRUE(s.roll_day(at("2026-03-09T21:00:00Z")).decision.ok());
  EXPECT_EQ(s.trading_day(), (md::Date{2026, 3, 10}));
}
TEST(TradingSettlement, AwaitingAndItmOtmCashSettlementForBothSignsAndRights) {
  for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
    for (const Side side : {Side::Buy, Side::Sell}) {
      for (const bool itm : {false, true}) {
        ScriptedMarket f;
        f.contract.type = type;
        f.contract.expiry = {2026, 9, 22};
        TradingSession s({}, f.time);
        f.seed(s);
        s.submit(f.market("position", 1, side), f.time);
        EXPECT_EQ(s.settle(f.symbol(), m("5000"), f.time).decision.code, Reason::INVALID_SETTLEMENT);
        const auto expiry = f.contract.expiry_time();
        s.on_quotes({}, {}, expiry);
        EXPECT_TRUE(s.snapshot()->positions[0].awaiting_settlement);
        EXPECT_FALSE(s.snapshot()->valuation_complete);
        EXPECT_EQ(s.snapshot()->positions[0].mark, m("4.10"));
        EXPECT_EQ(s.submit(f.market("expired"), expiry).decision.code, Reason::EXPIRED);
        const auto settle_price = !itm ? m("5000") : type == pricing::OptionType::Call ? m("5010") : m("4990");
        const auto before = s.snapshot();
        s.settle(f.symbol(), settle_price, expiry);
        const auto sign = side == Side::Buy ? 1 : -1;
        EXPECT_TRUE(s.snapshot()->positions.empty());
        EXPECT_EQ(s.snapshot()->account.cash, before->account.cash + m(itm ? "1000" : "0") * sign);
        EXPECT_EQ(s.snapshot()->account.cash, m("100000") + s.snapshot()->account.realised - s.snapshot()->account.fees);
        EXPECT_EQ(s.settle(f.symbol(), settle_price, expiry).decision.code, Reason::ALREADY_SETTLED);
      }
    }
  }
}
}  // namespace
}  // namespace openport::trading
