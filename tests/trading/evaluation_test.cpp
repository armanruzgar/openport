#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "openport/trading/history.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig rules_config(std::string_view cash, AccountRules rules) {
  SessionConfig c;
  c.initial_cash = m(cash);
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
AccountRules drawdown(std::string_view target, std::string_view max_drawdown,
                      DrawdownMode mode = DrawdownMode::Intraday) {
  AccountRules r;
  r.plan = "test";
  r.profit_target = m(target);
  r.max_drawdown = m(max_drawdown);
  r.drawdown_mode = mode;
  return r;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}

TEST(TradingEvaluation, OptionalExecutionRulesValidateAtCreationAndReset) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  for (const std::int64_t ticks : {-1, 11}) {
    AccountRules rules;
    rules.slippage_ticks = ticks;
    EXPECT_THROW(TradingSession(rules_config("10000", rules), f.time), TradingError);
    EXPECT_THROW(s.reset_account(m("10000"), rules, "invalid", f.time), TradingError);
  }
  AccountRules rules;
  rules.slippage_ticks = 10;
  rules.margin = MarginMode::Portfolio;
  EXPECT_NO_THROW(validate_rules(rules));
  rules.margin = static_cast<MarginMode>(2);
  EXPECT_THROW(validate_rules(rules), TradingError);
  EXPECT_THROW(s.reset_account(m("10000"), rules, "invalid", f.time), TradingError);
  EXPECT_EQ(s.config().rules.slippage_ticks, 0);
  EXPECT_EQ(s.config().rules.margin, MarginMode::Strategy);
}

TEST(TradingEvaluation, LiquidationAndExpiryClosingOrdersSlip) {
  for (const bool expiry : {false, true}) {
    for (const auto side : {Side::Buy, Side::Sell}) {
      ScriptedMarket f;
      f.time = md::new_york_to_utc({2026, 10, 22}, 15, 50);
      AccountRules rules;
      rules.slippage_ticks = 2;
      if (expiry) rules.expiry_cutoff = 5 * md::kNanosPerMinute;
      else rules.max_drawdown = m("20");
      TradingSession s(rules_config("100000", rules), f.time);
      f.seed(s);
      ASSERT_TRUE(s.submit(f.market("open", 1, side), f.time).decision.ok());
      if (expiry) {
        f.time = md::new_york_to_utc({2026, 10, 22}, 15, 56);
        ++f.observation;
        s.on_quotes({f.quote()}, {f.valuation()}, f.time);
      }
      const auto snap = s.snapshot();
      ASSERT_EQ(snap->recent_fills.size(), 2U);
      EXPECT_TRUE(snap->recent_orders.back().system);
      EXPECT_EQ(snap->recent_fills.back().price, m(side == Side::Buy ? "3.80" : "4.40"));
      EXPECT_TRUE(snap->positions.empty());
      EXPECT_EQ(snap->account.fees, m("1.30"));
    }
  }
}

TEST(TradingEvaluation, IntradayFloorTrailsPeakAndTouchFailsThenLiquidates) {
  ScriptedMarket f;
  TradingSession s(rules_config("10000", drawdown("1000", "100")), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  auto e = s.snapshot()->evaluation;
  EXPECT_EQ(s.snapshot()->equity, m("9946.75"));  // 10000 - 2100 - 3.25 + 5 * 100 * 4.10
  EXPECT_EQ(e.peak, m("10000"));
  EXPECT_EQ(e.floor, m("9900"));
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10146.75"));
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("10046.75"));
  ASSERT_TRUE(s.submit(f.limit("resting", 1, "3.90"), f.time).decision.ok());
  quote(s, f, "4.00", "4.20");
  const auto snap = s.snapshot();
  e = snap->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_EQ(e.decided_equity, m("9946.75"));
  EXPECT_EQ(e.decided_at, f.time);
  EXPECT_NE(e.decision.find("drawdown floor $10046.75"), std::string::npos) << e.decision;
  ASSERT_EQ(snap->recent_orders.size(), 3);
  EXPECT_EQ(snap->recent_orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(snap->recent_orders[1].reason.code, Reason::EVALUATION_CLOSED);
  const auto& liquidation = snap->recent_orders[2];
  EXPECT_TRUE(liquidation.system);
  EXPECT_TRUE(liquidation.request.client_order_id.starts_with("system:drawdown:"));
  EXPECT_EQ(liquidation.request.side, Side::Sell);
  EXPECT_EQ(liquidation.status, OrderStatus::Filled);
  EXPECT_EQ(snap->recent_fills.back().price, m("4.00"));
  EXPECT_TRUE(snap->positions.empty());
  EXPECT_EQ(snap->account.cash, m("9893.50"));
  const auto rejected = s.submit(f.market("after"), f.time);
  EXPECT_EQ(rejected.decision.code, Reason::EVALUATION_CLOSED);
}

TEST(TradingEvaluation, EndOfDayFloorRatchetsOnlyAtRolloverFromThatDaysClose) {
  ScriptedMarket f;
  TradingSession s(rules_config("10000", drawdown("0", "100", DrawdownMode::EndOfDay)), f.time);
  f.seed(s);
  s.submit(f.market("open", 5), f.time);
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10000"));
  quote(s, f, "4.00", "4.20");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);  // 9946.75 > 9900
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.day_close_equity, m("10146.75"));
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ++f.observation;
  s.on_quotes({f.quote("4.30", "4.50")}, {f.valuation()}, f.time);
  // A new date's quotes never overwrite the previous day's close.
  EXPECT_EQ(s.snapshot()->evaluation.day_close_equity, m("10146.75"));
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  const auto e = s.snapshot()->evaluation;
  ASSERT_EQ(e.days.size(), 1);
  EXPECT_EQ(e.days[0].day, (md::Date{2026, 9, 22}));
  EXPECT_EQ(e.days[0].open_equity, m("10000"));
  EXPECT_EQ(e.days[0].close_equity, m("10146.75"));
  EXPECT_EQ(e.days[0].peak, m("10146.75"));
  EXPECT_EQ(e.days[0].floor, m("10046.75"));
  EXPECT_EQ(e.day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(e.day_open_equity, m("10096.75"));
  EXPECT_EQ(e.status, EvaluationStatus::Active);
  quote(s, f, "4.00", "4.20");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
}

TEST(TradingEvaluation, AccountsWithoutRulesKeepTheirHighWaterMark) {
  // B50: a practice account's peak stayed at the starting balance whatever its equity did.
  for (const auto mode : {DrawdownMode::Intraday, DrawdownMode::EndOfDay}) {
    ScriptedMarket f;
    AccountRules practice;
    practice.drawdown_mode = mode;
    TradingSession s(rules_config("10000", practice), f.time);
    f.seed(s);
    s.submit(f.market("open", 5), f.time);
    quote(s, f, "4.40", "4.60");
    EXPECT_EQ(s.snapshot()->evaluation.peak, m(mode == DrawdownMode::Intraday ? "10146.75" : "10000"));
    quote(s, f, "4.00", "4.20");
    quote(s, f, "4.20", "4.40");
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    ++f.observation;
    s.on_quotes({f.quote("4.00", "4.20")}, {f.valuation()}, f.time);
    ASSERT_TRUE(s.roll_day(f.time).decision.ok());
    const auto e = s.snapshot()->evaluation;
    // Intraday follows every fully marked high; end of day takes the finished day's close.
    EXPECT_EQ(e.peak, m(mode == DrawdownMode::Intraday ? "10146.75" : "10046.75"));
    EXPECT_EQ(e.days.front().peak, e.peak);
    EXPECT_EQ(e.floor, Money{});
    EXPECT_EQ(e.status, EvaluationStatus::Active);
  }
}

TEST(TradingEvaluation, JournalsCreatedAtTimeZeroStartAtTheFirstMarketTime) {
  ScriptedMarket f;
  // The engine creates new journals before any market data arrives.
  TradingSession s(rules_config("10000", drawdown("1000", "100")), 0);
  EXPECT_EQ(s.snapshot()->evaluation.started, 0);
  f.seed(s);
  EXPECT_EQ(s.snapshot()->evaluation.started, f.time);
  EXPECT_EQ(s.snapshot()->evaluation.cycle_started, f.time);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  const auto e = s.snapshot()->evaluation;
  EXPECT_TRUE(e.days.empty());  // 1969-12-31 was a placeholder, not a trading day
  EXPECT_EQ(e.day, (md::Date{2026, 9, 22}));
  EXPECT_EQ(e.day_open_equity, m("10000"));
}

TEST(TradingEvaluation, ClosestFloorStartsWithItsTimeAtTheFirstMarketTime) {
  // B51: the session_start record at time zero set closest_floor, so closest_floor_at
  // stayed null for as long as later observations only tied it.
  ScriptedMarket f;
  TradingSession s(rules_config("25000", drawdown("2500", "1250")), 0);
  EXPECT_FALSE(s.snapshot()->evaluation.closest_floor);
  f.seed(s);
  quote(s, f, "4.00", "4.20");
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.closest_floor, m("1250"));
  EXPECT_EQ(e.closest_floor_at, e.started);
  EXPECT_GT(e.closest_floor_at, 0);
}

TEST(TradingEvaluation, ProfitTargetPassesAndLiquidatesAtTheBid) {
  ScriptedMarket f;
  TradingSession s(rules_config("10000", drawdown("100", "1000")), f.time);
  f.seed(s);
  s.submit(f.market("open", 5), f.time);
  quote(s, f, "4.40", "4.60");
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(snap->evaluation.decided_equity, m("10146.75"));
  EXPECT_TRUE(snap->recent_orders.back().request.client_order_id.starts_with("system:target:"));
  EXPECT_TRUE(snap->positions.empty());
  EXPECT_EQ(snap->account.cash, m("10093.50"));
  EXPECT_EQ(s.submit(f.market("after"), f.time).decision.code, Reason::EVALUATION_CLOSED);
}

TEST(TradingEvaluation, BuyOnlySellsMustCloseHeldContractsIncludingWorkingSells) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buy_only = true;
  TradingSession s(rules_config("100000", rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.submit(f.market("naked", 1, Side::Sell), f.time).decision.code, Reason::BUY_ONLY);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.limit("close-one", 1, "4.40", Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(f.limit("too-many", 2, "4.40", Side::Sell), f.time).decision.code, Reason::BUY_ONLY);
  EXPECT_TRUE(s.submit(f.limit("close-two", 1, "4.40", Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.limit("more-long", 1, "4.10"), f.time).decision.ok());
}

TEST(TradingEvaluation, BuyingPowerReservesWorkingOrdersAndRejectsShortfalls) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(rules_config("1000", rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 2, "4.10"), f.time).decision.ok());
  auto power = s.snapshot()->buying_power;
  EXPECT_EQ(power.reserved, m("821.30"));
  EXPECT_EQ(power.available, m("178.70"));
  const auto shortfall = s.submit(f.limit("more", 1, "4.10"), f.time).decision;
  EXPECT_EQ(shortfall.code, Reason::BUYING_POWER);
  EXPECT_DOUBLE_EQ(*shortfall.actual, 410.65);
  EXPECT_DOUBLE_EQ(*shortfall.limit, 178.70);
  quote(s, f, "4.00", "4.10");
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Filled);
  power = s.snapshot()->buying_power;
  EXPECT_EQ(power.reserved, m("0"));
  EXPECT_EQ(power.available, m("178.70"));
  EXPECT_EQ(s.submit(f.market("market"), f.time).decision.code, Reason::BUYING_POWER);
  // Closing orders reserve only fees and remain available.
  EXPECT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
}

TEST(TradingEvaluation, NakedShortsHoldBuyBackValuePlusRequirement) {
  const auto call = *md::parse_osi("SPXW261022C05000000");
  EXPECT_EQ(naked_requirement(call, 5000.0), m("100000"));
  auto put = call;
  put.type = pricing::OptionType::Put;
  EXPECT_EQ(naked_requirement(put, 5200.0), m("84000"));  // 20% of 5200 less 200 OTM
  auto high_call = call;
  high_call.strike = 5200;
  EXPECT_EQ(naked_requirement(high_call, 5000.0), m("80000"));
  EXPECT_EQ(naked_requirement(put, std::nullopt), m("100000"));  // strike stands in for spot

  ScriptedMarket f;
  AccountRules rules;
  rules.buying_power = true;
  {
    TradingSession s(rules_config("100000", rules), f.time);
    f.seed(s);
    const auto decision = s.submit(f.market("naked", 1, Side::Sell), f.time).decision;
    EXPECT_EQ(decision.code, Reason::BUYING_POWER);
    EXPECT_DOUBLE_EQ(*decision.actual, 100000.65);
  }
  TradingSession s(rules_config("200000", rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("naked", 1, Side::Sell), f.time).decision.ok());
  const auto power = s.snapshot()->buying_power;
  EXPECT_EQ(s.snapshot()->account.cash, m("200399.35"));
  EXPECT_EQ(power.short_requirement, m("100410"));
  EXPECT_EQ(power.available, m("99989.35"));
}

TEST(TradingEvaluation, ExpiryCutoffCancelsWorkingOrdersAutoClosesAndBlocksOpening) {
  ScriptedMarket f;
  f.time = md::new_york_to_utc({2026, 10, 22}, 15, 50);
  AccountRules rules;
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  TradingSession s(rules_config("100000", rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("exit", 1, "4.40", Side::Sell), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 10, 22}, 15, 56);
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders[1].reason.code, Reason::EXPIRY_CUTOFF);
  EXPECT_TRUE(snap->recent_orders.back().request.client_order_id.starts_with("system:expiry:"));
  EXPECT_TRUE(snap->positions.empty());
  EXPECT_EQ(s.submit(f.market("late"), f.time).decision.code, Reason::EXPIRY_CUTOFF);
}

TEST(TradingEvaluation, TheAutoCloseKeepsCoverForAShortItCouldBuyBackOnlyInPart) {
  ScriptedMarket f;  // the short 5000 call
  f.time = md::new_york_to_utc({2026, 10, 22}, 15, 50);
  ScriptedMarket g = f;  // the long 4900 call
  g.contract = *md::parse_osi("SPXW261022C04900000");
  AccountRules rules;
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  TradingSession s(rules_config("100000", rules), f.time);
  s.define(f.contract, f.time);
  s.define(g.contract, f.time);
  s.on_quotes({f.quote(), g.quote("9.00", "9.20")}, {f.valuation(), g.valuation(0.6)}, f.time);
  ASSERT_TRUE(s.submit(g.market("long", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short", 2, Side::Sell), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 10, 22}, 15, 56);
  ++f.observation;
  g.time = f.time;
  g.observation = f.observation;
  // Only one is offered to buy the short back: one long stays to cover the other.
  s.on_quotes({f.quote("4.00", "4.20", 1), g.quote("9.00", "9.20")}, {f.valuation(), g.valuation(0.6)}, f.time);
  const auto held = [&](const std::string& symbol) {
    for (const auto& p : s.snapshot()->positions) if (p.position.contract.osi_symbol() == symbol) return p.position.quantity;
    return Quantity{0};
  };
  EXPECT_EQ(held(f.symbol()), -1);
  EXPECT_EQ(held(g.symbol()), 1);
}

TEST(TradingEvaluation, TheAutoCloseBuysShortsBackBeforeSellingTheLongsThatCoverThem) {
  ScriptedMarket f;  // the short 5000 call
  f.time = md::new_york_to_utc({2026, 10, 22}, 15, 50);
  ScriptedMarket g = f;  // the long 4900 call, listed first
  g.contract = *md::parse_osi("SPXW261022C04900000");
  AccountRules rules;
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  TradingSession s(rules_config("100000", rules), f.time);
  s.define(f.contract, f.time);
  s.define(g.contract, f.time);
  s.on_quotes({f.quote(), g.quote("9.00", "9.20")}, {f.valuation(), g.valuation(0.6)}, f.time);
  ASSERT_TRUE(s.submit(g.market("long", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short", 2, Side::Sell), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 10, 22}, 15, 56);
  ++f.observation;
  g.time = f.time;
  g.observation = f.observation;
  s.on_quotes({f.quote(), g.quote("9.00", "9.20")}, {f.valuation(), g.valuation(0.6)}, f.time);
  const auto snap = s.snapshot();
  ASSERT_EQ(snap->recent_orders.size(), 4u);
  EXPECT_EQ(snap->recent_orders[2].request.symbol, f.symbol()) << "the short closes first";
  EXPECT_EQ(snap->recent_orders[2].request.side, Side::Buy);
  EXPECT_EQ(snap->recent_orders[3].request.symbol, g.symbol());
  EXPECT_TRUE(snap->positions.empty());
}

TEST(TradingEvaluation, TheCutoffCountsBackFromEachContractsLastTrade) {
  AccountRules rules;
  rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  // SPY options trade until 16:15, so they open until 16:10 and close then.
  ScriptedMarket spy;
  spy.contract = *md::parse_osi("SPY261022C00500000");
  spy.time = md::new_york_to_utc({2026, 10, 22}, 16, 5);
  TradingSession s(rules_config("100000", rules), spy.time);
  spy.seed(s);
  ASSERT_TRUE(s.submit(spy.market("open", 2), spy.time).decision.ok());
  spy.time = md::new_york_to_utc({2026, 10, 22}, 16, 10);
  ++spy.observation;
  s.on_quotes({spy.quote()}, {spy.valuation()}, spy.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:expiry:"));
  EXPECT_EQ(s.submit(spy.market("late"), spy.time).decision.code, Reason::EXPIRY_CUTOFF);

  // AM-settled series last trade at the regular close the afternoon before expiry.
  ScriptedMarket am;
  am.contract = *md::parse_osi("SPX261016C05000000");
  am.time = md::new_york_to_utc({2026, 10, 15}, 16, 0);
  TradingSession t(rules_config("100000", rules), am.time);
  am.seed(t);
  ASSERT_TRUE(t.submit(am.market("open"), am.time).decision.ok());
  am.time = md::new_york_to_utc({2026, 10, 15}, 16, 10);
  ++am.observation;
  t.on_quotes({am.quote()}, {am.valuation()}, am.time);
  EXPECT_TRUE(t.snapshot()->positions.empty());
  EXPECT_TRUE(t.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:expiry:"));
}

TEST(TradingEvaluation, ResetArchivesAttemptClosesAtMarkAndRestoresCash) {
  ScriptedMarket f;
  auto first = drawdown("1000", "100");
  first.plan = "A";
  TradingSession s(rules_config("10000", first), f.time);
  const auto started = f.time;
  f.seed(s);
  s.submit(f.market("open", 2), f.time);
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "4.00"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->equity, m("9978.70"));
  f.next();
  auto second = drawdown("2000", "500");
  second.plan = "B";
  EXPECT_THROW(s.reset_account(m("20000"), second, " ", f.time), TradingError);
  EXPECT_THROW(s.reset_account(m("0"), second, "again", f.time), TradingError);
  ASSERT_TRUE(s.reset_account(m("20000"), second, "fresh start", f.time).decision.ok());
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders[1].reason.code, Reason::ACCOUNT_RESET);
  ASSERT_EQ(snap->closures.size(), 1);
  EXPECT_EQ(snap->closures[0].kind, ClosureKind::Reset);
  EXPECT_EQ(snap->closures[0].quantity, 2);
  EXPECT_EQ(snap->closures[0].price, m("4.10"));
  EXPECT_EQ(snap->closures[0].after_fill, 1);
  ASSERT_EQ(snap->attempts.size(), 1);
  EXPECT_EQ(snap->attempts[0].plan, "A");
  EXPECT_EQ(snap->attempts[0].started, started);
  EXPECT_EQ(snap->attempts[0].final_equity, m("9978.70"));
  EXPECT_EQ(snap->evaluation.attempt, 2);
  EXPECT_EQ(snap->evaluation.starting_balance, m("20000"));
  EXPECT_EQ(snap->evaluation.floor, m("19500"));
  EXPECT_EQ(snap->evaluation.first_order, 3);
  EXPECT_EQ(snap->evaluation.first_fill, 2);
  EXPECT_EQ(snap->account.cash, m("20000"));
  EXPECT_EQ(snap->account.fees, m("0"));
  EXPECT_TRUE(snap->positions.empty());
  EXPECT_EQ(s.config().rules.plan, "B");
  EXPECT_EQ(s.config().initial_cash, m("20000"));

  s.trip_kill("pause", f.time);
  ASSERT_TRUE(s.reset_account(m("20000"), second, "again", f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->evaluation.attempt, 3);

  const auto trades = lifecycles(snap->recent_fills, snap->closures, s.contracts());
  ASSERT_EQ(trades.size(), 1);
  EXPECT_EQ(trades[0].closure, ClosureKind::Reset);
  EXPECT_EQ(trades[0].gross, m("-20"));
  EXPECT_EQ(trades[0].fees, m("1.30"));
}

TEST(TradingEquityOptions, AmericanContractsUseTheirSessionTicksAndSettleAtIntrinsic) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261022C00500000");
  TradingSession s(rules_config("100000", {}), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("spy", 2, "4.20"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_fills.at(0).price, m("4.20"));
  EXPECT_TRUE(s.submit(f.limit("penny", 1, "4.01"), f.time).decision.ok());  // SPY quotes in pennies
  // SPY options trade through 16:15 like the index roots.
  EXPECT_EQ(s.snapshot()->recent_orders.at(1).day_end, md::new_york_to_utc({2026, 9, 22}, 16, 15));

  ScriptedMarket g;
  g.contract = *md::parse_osi("AAPL  261022P00200000");
  TradingSession single(rules_config("100000", {}), g.time);
  g.seed(single);
  EXPECT_EQ(single.submit(g.limit("tick", 1, "4.03"), g.time).decision.code, Reason::INVALID_TICK);
  ASSERT_TRUE(single.submit(g.limit("aapl", 1, "4.05"), g.time).decision.ok());
  // Single-stock options stop at 16:00.
  EXPECT_EQ(single.snapshot()->recent_orders.at(1).day_end, md::new_york_to_utc({2026, 9, 22}, 16, 0));

  // Held into expiry, the SPY calls settle at intrinsic from the reference price.
  const auto expiry = f.contract.expiry_time();
  ASSERT_TRUE(s.settle(f.symbol(), m("510.25"), expiry).decision.ok());
  const auto snap = s.snapshot();
  EXPECT_TRUE(snap->positions.empty());
  ASSERT_EQ(snap->closures.size(), 1);
  EXPECT_EQ(snap->closures[0].price, m("10.25"));
  EXPECT_EQ(snap->account.realised, m("1210"));  // 2 * 100 * (10.25 - 4.20)
}

TEST(TradingHistory, LifecyclesSplitReversalsAndCloseOnSettlement) {
  const auto contract = *md::parse_osi("SPXW261022C05000000");
  const auto symbol = contract.osi_symbol();
  const Contracts contracts{{symbol, contract}};
  auto fill = [&](std::uint64_t id, Side side, Quantity q, std::string_view price, std::string_view fee) {
    return Fill{id, id, symbol, side, q, m(price), m(fee), id, static_cast<Timestamp>(id), static_cast<Timestamp>(id * 10)};
  };
  const SharedVector<Fill> fills{fill(1, Side::Buy, 2, "4.20", "1.30"), fill(2, Side::Buy, 1, "4.40", "0.65"),
                                fill(3, Side::Sell, 3, "4.00", "1.95"), fill(4, Side::Buy, 2, "4.00", "1.30"),
                                fill(5, Side::Sell, 5, "4.50", "3.25")};
  const SharedVector<Closure> closures{{symbol, -3, m("0"), 60, ClosureKind::Settlement, 5}};
  const auto trades = lifecycles(fills, closures, contracts);
  ASSERT_EQ(trades.size(), 3);
  EXPECT_EQ(trades[0].direction, 1);
  EXPECT_EQ(trades[0].opened, 10);
  EXPECT_EQ(trades[0].closed, 30);
  EXPECT_EQ(trades[0].max_quantity, 3);
  EXPECT_EQ(trades[0].opened_contracts, 3);
  EXPECT_EQ(trades[0].closed_contracts, 3);
  EXPECT_EQ(trades[0].open_notional, m("12.80"));
  EXPECT_EQ(trades[0].close_notional, m("12.00"));
  EXPECT_EQ(trades[0].gross, m("-80"));
  EXPECT_EQ(trades[0].fees, m("3.90"));
  EXPECT_EQ(trades[0].fills, (std::vector<std::uint64_t>{1, 2, 3}));
  EXPECT_EQ(trades[1].gross, m("100"));
  EXPECT_EQ(trades[1].fees, m("2.60"));  // 1.30 plus 2/5 of the reversing fill's fee
  EXPECT_EQ(trades[1].closed, 50);
  EXPECT_EQ(trades[2].direction, -1);
  EXPECT_EQ(trades[2].opened, 50);
  EXPECT_EQ(trades[2].first_fill, 5);
  EXPECT_EQ(trades[2].fees, m("1.95"));
  EXPECT_EQ(trades[2].closure, ClosureKind::Settlement);
  EXPECT_EQ(trades[2].closed, 60);
  EXPECT_EQ(trades[2].gross, m("1350"));
  // Lifecycles reconcile exactly with one ledger over the whole history.
  Ledger ledger;
  for (const auto& f : fills) ledger.fill(contract, f.side == Side::Buy ? f.quantity : -f.quantity, f.price, f.fee);
  ledger.settle(symbol, m("0"));
  EXPECT_EQ(ledger.account().realised, trades[0].gross + trades[1].gross + trades[2].gross);
  EXPECT_EQ(ledger.account().fees, trades[0].fees + trades[1].fees + trades[2].fees);
  // Open lifecycles keep their quantity and have no close time.
  const auto open = lifecycles({fills[0]}, {}, contracts);
  ASSERT_EQ(open.size(), 1);
  EXPECT_FALSE(open[0].closed);
  EXPECT_EQ(open[0].quantity, 2);
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-evaluation-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};
class CapturingJournal final : public Journal {
 public:
  struct Entry { Timestamp time; std::string type; std::string payload; };
  std::vector<Entry> entries;
  void append(Timestamp time, std::string_view type, std::string_view payload) override {
    entries.push_back({time, std::string(type), std::string(payload)});
  }
  std::uint64_t sequence() const override { return entries.size(); }
  std::string head() const override { return std::string(64, '0'); }
};
/// Rewrite a schema 2 transaction as the schema 1 record an older build wrote.
std::string schema_one(std::string_view payload) {
  auto j = nlohmann::json::parse(payload);
  j["schema"] = 1;
  auto& state = j["state"];
  for (const auto* key : {"evaluation", "attempts", "closures"}) state.erase(key);
  state["config"].erase("rules");
  for (auto& order : state["orders"]) order.erase("system");
  auto& snapshot = j["snapshot"];
  for (const auto* key : {"evaluation", "buying_power", "closures", "attempts"}) snapshot.erase(key);
  for (const auto* list : {"recent_orders", "open_orders"})
    for (auto& order : snapshot[list]) order.erase("system");
  for (auto& event : j["events"]) {
    event["payload"].erase("system");
    event["payload"].erase("rules");
  }
  return j.dump();
}

TEST(TradingEvaluation, JournalRecoveryRestoresRulesProgressAndSystemOrders) {
  TemporaryDirectory directory;
  const auto path = (directory.path / "rules.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(rules_config("10000", drawdown("1000", "100")), f.time, journal);
    f.seed(s);
    s.submit(f.market("open", 5), f.time);
    quote(s, f, "4.40", "4.60");
    quote(s, f, "4.00", "4.20");
    ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
    s.reset_account(m("10000"), drawdown("500", "200"), "retry", f.time);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto resumed = FileJournal::resume(path);
  auto s = TradingSession::recover(FileJournal::read(path, head), resumed);
  EXPECT_EQ(s.snapshot_json(), expected);
  EXPECT_EQ(s.snapshot()->evaluation.attempt, 2);
  EXPECT_EQ(s.snapshot()->attempts.at(0).status, EvaluationStatus::Failed);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(1).system);
  EXPECT_EQ(s.config().rules.max_drawdown, m("200"));
}

TEST(TradingEvaluation, SchemaOneJournalsRecoverWithoutRulesAndContinueInTheCurrentSchema) {
  TemporaryDirectory directory;
  const auto current = (directory.path / "current.jsonl").string();
  const auto path = (directory.path / "legacy.jsonl").string();
  ScriptedMarket f;
  auto capture = std::make_shared<CapturingJournal>();
  std::shared_ptr<const TradingSnapshot> original;
  {
    TradingSession s({}, f.time, FileJournal::create(current));
    f.seed(s);
    s.submit(f.market("open", 2), f.time);
    s.submit(f.limit("rest", 1, "4.00"), f.time);
    original = s.snapshot();
  }
  // Every state whole, as schema 2 wrote it, to rewrite as schema 1.
  TradingSession::expand(FileJournal::read(current), *capture);
  {
    auto file = FileJournal::create(path);
    for (const auto& entry : capture->entries) file->append(entry.time, entry.type, schema_one(entry.payload));
  }
  // Keys that schema 1 always had remain required.
  {
    const auto broken_path = (directory.path / "broken.jsonl").string();
    auto broken = FileJournal::create(broken_path);
    for (const auto& entry : capture->entries) {
      auto j = nlohmann::json::parse(schema_one(entry.payload));
      j["state"].erase("kill_reason");
      broken->append(entry.time, entry.type, j.dump());
    }
    try {
      (void)TradingSession::recover(FileJournal::read(broken_path));
      ADD_FAILURE() << "expected JOURNAL_CORRUPT";
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::JOURNAL_CORRUPT); }
  }
  // Compaction keeps the final schema 1 record, whose evaluation recovery completes.
  const auto legacy = FileJournal::read(path);
  {
    const auto compacted = (directory.path / "compacted-legacy.jsonl").string();
    EXPECT_EQ(TradingSession::compact(legacy, *FileJournal::create(compacted)), TradingSession::recover(legacy).snapshot_json());
    const auto records = FileJournal::read(compacted).records;
    ASSERT_EQ(records.size(), legacy.records.size());
    for (std::size_t i = 0; i < records.size(); ++i)
      EXPECT_EQ(nlohmann::json::parse(records[i].payload).at("schema"), i + 1 < records.size() ? 3 : 1);
    EXPECT_EQ(records.back().payload, legacy.records.back().payload);
    EXPECT_EQ(TradingSession::recover(FileJournal::read(compacted)).snapshot_json(), TradingSession::recover(legacy).snapshot_json());
  }
  auto resumed = FileJournal::resume(path);
  auto s = TradingSession::recover(legacy, resumed);
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, original->account.cash);
  EXPECT_EQ(snap->equity, original->equity);
  EXPECT_EQ(snap->open_orders.size(), 1);
  EXPECT_FALSE(s.config().rules.evaluation());
  EXPECT_EQ(snap->evaluation.attempt, 1);
  EXPECT_EQ(snap->evaluation.starting_balance, SessionConfig{}.initial_cash);
  EXPECT_EQ(snap->evaluation.started, f.time);
  EXPECT_EQ(snap->evaluation.day, (md::Date{2026, 9, 22}));
  EXPECT_EQ(snap->buying_power.reserved, m("400.65"));
  EXPECT_TRUE(s.submit(f.market("after-upgrade"), f.time).decision.ok());
  std::ifstream in(path);
  std::string line, last;
  while (std::getline(in, line)) last = line;
  // The first record after recovery carries the whole state.
  const auto payload = nlohmann::json::parse(last).at("payload");
  EXPECT_EQ(payload.at("schema"), 3);
  EXPECT_TRUE(payload.contains("state"));
  EXPECT_FALSE(payload.contains("snapshot"));
  // Schema 1 records followed by schema 2 ones, as a schema 2 build left them,
  // compact whole.
  const auto upgraded = (directory.path / "upgraded.jsonl").string();
  TradingSession::expand(FileJournal::read(path), *FileJournal::create(upgraded));
  const auto both = FileJournal::read(upgraded);
  EXPECT_EQ(nlohmann::json::parse(both.records.front().payload).at("schema"), 1);
  EXPECT_EQ(nlohmann::json::parse(both.records.back().payload).at("schema"), 2);
  const auto compacted = (directory.path / "compacted-upgraded.jsonl").string();
  EXPECT_EQ(TradingSession::compact(both, *FileJournal::create(compacted)), s.snapshot_json());
  for (const auto& record : FileJournal::read(compacted).records)
    EXPECT_EQ(nlohmann::json::parse(record.payload).at("schema"), 3);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(compacted)).snapshot_json(), s.snapshot_json());
}

}  // namespace
}  // namespace openport::trading
