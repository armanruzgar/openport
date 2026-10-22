#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include "openport/server/equity.hpp"
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig config(AccountRules rules) {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
/// A funded account: the floor locks at the starting balance; a payout needs
/// `days` days of $50 net realised profit and takes up to half the profit.
AccountRules funded(std::string_view max_drawdown = "1000", std::int64_t days = 1) {
  AccountRules r;
  r.plan = "Funded test";
  r.phase = Phase::Funded;
  r.max_drawdown = m(max_drawdown);
  r.lock_balance = m("10000");
  r.payouts.qualifying_profit = m("50");
  r.payouts.qualifying_days = days;
  r.payouts.withdrawal_percent = 50;
  r.payouts.split_percent = 80;
  r.payouts.minimum = m("10");
  r.payouts.caps = {m("60"), m("100")};
  return r;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
/// Buy five at 4.20, mark up to 4.40/4.60 and sell at 4.40: $100 gross, $93.50 net.
void winning_trade(TradingSession& s, ScriptedMarket& f) {
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("win-open-" + std::to_string(f.observation), 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  ASSERT_TRUE(s.submit(f.market("win-close-" + std::to_string(f.observation), 5, Side::Sell), f.time).decision.ok());
}
void next_day(TradingSession& s, ScriptedMarket& f, md::Date day) {
  f.time = md::new_york_to_utc(day, 10, 0);
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
}

// Exact net day profit from one round trip, including both fees.
void profit_day(TradingSession& s, ScriptedMarket& f, std::string_view profit) {
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("day-open-" + std::to_string(f.observation)), f.time).decision.ok());
  const auto bid = m("4.20") + (m(profit) + s.config().fee_per_contract * 2).prorate(1, 100);
  quote(s, f, bid.str(), (bid + m("0.20")).str());
  ASSERT_TRUE(s.submit(f.market("day-close-" + std::to_string(f.observation), 1, Side::Sell), f.time).decision.ok());
}

AccountRules size_plan(std::int64_t days = 2, std::int64_t payouts = 1) {
  auto r = funded("1000.03");
  r.daily_loss_limit = m("500.03");
  r.lock_balance = {};
  r.payouts.buffer = m("50");
  r.scaling = {{Money{}, 2}, {m("100"), 4}};
  r.size_scaling = SizeScaling{1, payouts, days, 25, m("14000")};
  return r;
}

TEST(TradingFunded, FillEquitySamplesKeepTheFloorSizeScalingRaised) {
  ScriptedMarket f;
  TradingSession s(config(size_plan()), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
  next_day(s, f, {2026, 9, 24});
  ASSERT_EQ(s.snapshot()->evaluation.size_scaling->history.size(), 1U);
  // The scaled drawdown is wider than the floor's distance below the peak, so a new
  // high must not move the floor down.
  ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("part", 1, "4.50", Side::Sell, TimeInForce::Gtc), f.time).decision.ok());
  const auto before = s.snapshot();
  quote(s, f, "4.50", "4.70");
  ASSERT_GT(s.snapshot()->evaluation.peak, before->evaluation.peak);
  const auto samples = server::fill_equity_samples(s, *before);
  ASSERT_FALSE(samples.empty());
  for (const auto& sample : samples) EXPECT_EQ(sample.floor, s.snapshot()->evaluation.floor);
}

TEST(TradingFunded, SizeScalingCreditsCapitalWithoutChangingProfitOrPayoutStanding) {
  ScriptedMarket f;
  TradingSession s(config(size_plan()), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 23});
  EXPECT_EQ(s.snapshot()->evaluation.size_scaling->period_days, 1);
  ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
  const auto before = *s.snapshot();
  const auto payout_before = payout_quote(before, s.config().rules);
  EXPECT_EQ(size_scaling_profit(before.evaluation, plan_inputs(before).balance), m("200"));
  next_day(s, f, {2026, 9, 24});
  const auto after = *s.snapshot();
  const auto& e = after.evaluation;
  EXPECT_EQ(e.starting_balance, m("12500"));
  EXPECT_EQ(after.account.cash - before.account.cash, m("2500"));
  EXPECT_EQ(plan_inputs(after).balance - plan_inputs(before).balance, m("2500"));
  EXPECT_EQ(after.account.realised, before.account.realised);
  EXPECT_EQ(after.account.fees, before.account.fees);
  EXPECT_EQ(e.floor - before.evaluation.floor, m("2500"));
  EXPECT_EQ(e.peak - before.evaluation.peak, m("2500"));
  EXPECT_EQ(s.config().rules.max_drawdown, m("1250.03"));
  EXPECT_EQ(s.config().rules.daily_loss_limit, m("625.03"));
  EXPECT_EQ(e.day_open_equity, after.equity);
  EXPECT_EQ(e.scaling_limit, 4);
  EXPECT_EQ(size_scaling_profit(e, plan_inputs(after).balance), Money{});
  EXPECT_EQ(e.size_scaling->period_days, 0);
  EXPECT_EQ(e.size_scaling->period_started, (md::Date{2026, 9, 24}));
  ASSERT_EQ(e.size_scaling->history.size(), 1U);
  EXPECT_EQ(e.size_scaling->history[0].old, m("10000"));
  const auto payout_after = payout_quote(after, s.config().rules);
  EXPECT_EQ(payout_before.profit, payout_after.profit);
  EXPECT_EQ(payout_before.withdrawable, payout_after.withdrawable);
  EXPECT_EQ(payout_before.maximum, payout_after.maximum);
  EXPECT_EQ(payout_before.blocked.code, payout_after.blocked.code);
  EXPECT_EQ(*payout_after.buffer_balance - *payout_before.buffer_balance, m("2500"));
  EXPECT_EQ(today_profit(e, s.config().rules, plan_inputs(after)), Money{});
  EXPECT_EQ(attempt_profit(e, s.config().rules, plan_inputs(after)), m("200"));
  // A new high must never undo the capital lift when the larger distance starts trailing.
  profit_day(s, f, "10");
  EXPECT_GE(s.snapshot()->evaluation.floor, e.floor);
}

TEST(TradingFunded, SizeScalingReviewsRestartWhenProfitOrPayoutsAreMissing) {
  for (const bool missing_profit : {false, true}) {
    ScriptedMarket f;
    TradingSession s(config(size_plan()), f.time);
    f.seed(s);
    profit_day(s, f, missing_profit ? "60" : "200");
    next_day(s, f, {2026, 9, 23});
    if (missing_profit) { ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok()); }
    next_day(s, f, {2026, 9, 24});
    const auto& e = s.snapshot()->evaluation;
    EXPECT_EQ(e.starting_balance, m("10000"));
    EXPECT_EQ(e.size_scaling->period_days, 0);
    EXPECT_EQ(e.size_scaling->period_started, (md::Date{2026, 9, 24}));
    EXPECT_EQ(size_scaling_profit(e, plan_inputs(*s.snapshot()).balance), Money{});
    EXPECT_EQ(e.payouts.size(), e.size_scaling->payouts_at_start);
  }
}

TEST(TradingFunded, FlatTimeProfitFeedsBothScalingRulesAfterTheDayCloses) {
  ScriptedMarket f;
  auto rules = size_plan(1, 0);
  rules.flat_time = 601;
  rules.no_overnight = true;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  f.time += md::kNanosPerMinute;
  ++f.observation;
  s.on_quotes({f.quote("6.20", "6.40")}, {f.valuation()}, f.time);
  const auto closed = s.snapshot();
  ASSERT_TRUE(closed->positions.empty());
  EXPECT_TRUE(closed->recent_orders.back().request.client_order_id.starts_with("system:flat_time:"));
  EXPECT_EQ(closed->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(closed->evaluation.scaling_limit, 2);
  EXPECT_EQ(closed->evaluation.starting_balance, m("10000"));
  EXPECT_EQ(size_scaling_profit(closed->evaluation, plan_inputs(*closed).balance), m("198.70"));
  EXPECT_EQ(s.submit(f.market("too-late"), f.time).decision.code, Reason::FLAT_TIME);

  // Record the finished day before publishing the next day's market data.
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  const auto& e = s.snapshot()->evaluation;
  ASSERT_EQ(e.days.size(), 1U);
  EXPECT_EQ(e.days.back().realised, m("198.70"));
  EXPECT_EQ(e.days.back().close_equity, closed->equity);
  EXPECT_EQ(e.days.back().floor, closed->evaluation.floor);
  EXPECT_EQ(e.starting_balance, m("12500"));
  EXPECT_EQ(e.scaling_limit, 4);
  EXPECT_EQ(e.day_open_equity, closed->equity + m("2500"));
  EXPECT_EQ(size_scaling_profit(e, plan_inputs(*s.snapshot()).balance), Money{});
  EXPECT_EQ(e.status, EvaluationStatus::Active);
}

TEST(TradingFunded, OvernightFailureBlocksDueSizeReviewWithPendingFlatCloses) {
  ScriptedMarket f;
  auto rules = size_plan(1, 0);
  rules.flat_time = 16 * 60 + 30;
  rules.no_overnight = true;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  ASSERT_TRUE(s.submit(f.market("overnight"), f.time).decision.ok());
  // A fresh marked close after the option session ends cannot execute the flat.
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 30);
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.snapshot()->evaluation.flat_pending);
  ASSERT_FALSE(s.snapshot()->positions.empty());
  ASSERT_TRUE(s.snapshot()->valuation_complete);
  ASSERT_GE(size_scaling_profit(s.snapshot()->evaluation, plan_inputs(*s.snapshot()).balance), m("100"));

  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  const auto& e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_EQ(e.decision_code, Reason::OVERNIGHT_HOLD);
  EXPECT_EQ(e.days.size(), 1U);
  EXPECT_EQ(e.starting_balance, m("10000"));
  EXPECT_TRUE(e.size_scaling->history.empty());
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:overnight:"));
  EXPECT_TRUE(s.snapshot()->evaluation.size_scaling->history.empty());
}

TEST(TradingFunded, SizeScalingCapsGrowthAndRoundsLossLimitsFromTheOriginalSize) {
  ScriptedMarket f;
  auto rules = size_plan(1, 0);
  rules.drawdown_mode = DrawdownMode::Static;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "150");
  next_day(s, f, {2026, 9, 23});
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("11499.97"));
  profit_day(s, f, "150");
  next_day(s, f, {2026, 9, 24});
  EXPECT_EQ(s.snapshot()->evaluation.starting_balance, m("14000"));
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("12999.97"));
  EXPECT_EQ(s.config().rules.max_drawdown, m("1400.04"));
  EXPECT_EQ(s.config().rules.daily_loss_limit, m("700.04"));
  EXPECT_EQ(s.snapshot()->evaluation.size_scaling->history.size(), 2U);
  profit_day(s, f, "150");
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.starting_balance, m("14000"));
  EXPECT_EQ(s.snapshot()->evaluation.size_scaling->history.size(), 2U);
}

TEST(TradingFunded, SizeScalingGrowthIsLinearAndFinishedDaysIgnoreCalendarGaps) {
  ScriptedMarket f;
  auto rules = size_plan(1, 0);
  rules.size_scaling->max_balance = m("20000");
  rules.drawdown_mode = DrawdownMode::EndOfDay;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 25}); // A three-calendar-day gap still finishes one plan day.
  EXPECT_EQ(s.snapshot()->evaluation.starting_balance, m("12500"));
  EXPECT_EQ(s.snapshot()->evaluation.days.size(), 1U);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 28});
  EXPECT_EQ(s.snapshot()->evaluation.starting_balance, m("15000"));
  EXPECT_EQ(s.config().rules.max_drawdown, m("1500.04"));
  EXPECT_EQ(s.config().rules.daily_loss_limit, m("750.04"));
  // A fractional micro-dollar threshold rounds up, while the credit rounds down.
  auto e = s.snapshot()->evaluation;
  e.size_scaling->original = m("10000.000001");
  e.size_scaling->period_size = m("10000.000001");
  EXPECT_EQ(size_scaling_required(e, *rules.size_scaling), m("100.000001"));
  EXPECT_EQ(size_scaling_increase(e, *rules.size_scaling), m("2500"));
}

TEST(TradingFunded, SizeScalingRejectsEvaluationAndInvalidRangesAndNeverScalesAFailedAccount) {
  auto rules = size_plan();
  for (auto field : {&SizeScaling::profit_percent, &SizeScaling::days, &SizeScaling::increase_percent}) {
    auto bad = rules;
    (*bad.size_scaling).*field = 0;
    EXPECT_THROW(validate_rules(bad), TradingError);
    (*bad.size_scaling).*field = 367;
    EXPECT_THROW(validate_rules(bad), TradingError);
  }
  auto bad = rules; bad.size_scaling->payouts = -1;
  EXPECT_THROW(validate_rules(bad), TradingError);
  bad.size_scaling->payouts = 101;
  EXPECT_THROW(validate_rules(bad), TradingError);
  for (const auto phase : {Phase::Evaluation, Phase::Verification}) {
    bad = rules; bad.phase = phase;
    EXPECT_THROW(validate_rules(bad), TradingError);
  }
  bad = rules; bad.size_scaling->max_balance = m("9999");
  EXPECT_THROW(TradingSession(config(bad), ScriptedMarket{}.time), TradingError);
  ScriptedMarket f;
  rules = size_plan(2, 0);
  rules.daily_loss_limit = m("10");
  rules.daily_loss_action = BreachAction::Fail;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  // Start with shares so entry spread cannot trip the very small daily limit.
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("100")}).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", -1, f.time, StockPrice{"SPY", f.time, m("300")}).decision.ok());
  next_day(s, f, {2026, 9, 23});
  s.submit(f.market("fail"), f.time);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
  next_day(s, f, {2026, 9, 24});
  EXPECT_EQ(s.snapshot()->evaluation.starting_balance, m("10000"));
  EXPECT_TRUE(s.snapshot()->evaluation.size_scaling->history.empty());
}

TEST(TradingFunded, SharesHeldBlockAPayoutAsPositionsDo) {
  ScriptedMarket f;
  TradingSession s(config(size_plan()), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("100")}).decision.ok());
  EXPECT_FALSE(payout_quote(*s.snapshot(), s.config().rules).flat);
  const auto refused = s.request_payout(m("40"), f.time);
  EXPECT_EQ(refused.decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_NE(refused.decision.message.find("share holding"), std::string::npos);
  ASSERT_TRUE(s.trade_stock("SPY", -1, f.time, StockPrice{"SPY", f.time, m("100")}).decision.ok());
  EXPECT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
}

TEST(TradingFunded, ConsistencyCountsNetDaysIncludingTodayAndRestartsAtThePayoutDay) {
  ScriptedMarket f;
  auto rules = funded("5000");
  rules.payouts.consistency_percents = {40};
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "300");
  // Qualifying days block first, even though consistency is also unmet.
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.limit, 1);
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.consistency_percent, 40);
  EXPECT_EQ(q.cycle_profit, m("300"));
  EXPECT_EQ(q.best_day, m("300"));
  EXPECT_EQ(q.best_day_date, (md::Date{2026, 9, 22}));
  EXPECT_EQ(q.consistency_needed, m("450"));
  EXPECT_EQ(q.blocked.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_EQ(q.blocked.actual, 100);
  EXPECT_EQ(q.blocked.limit, 40);
  EXPECT_NE(q.blocked.message.find("consistency"), std::string::npos);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 24});
  profit_day(s, f, "250");  // Today's $250 counts before rollover: exactly 40%.
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("750"));
  EXPECT_EQ(q.consistency_needed, Money{});
  ASSERT_TRUE(q.blocked.ok());
  ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok());
  q = payout_quote(*s.snapshot(), rules);
  // The request day belongs to the new cycle in full, as with qualifying_days.
  EXPECT_EQ(q.cycle_profit, m("250"));
  EXPECT_EQ(q.best_day, m("250"));
  EXPECT_EQ(q.best_day_date, (md::Date{2026, 9, 24}));
  next_day(s, f, {2026, 9, 25});
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 28});
  profit_day(s, f, "175");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("625"));
  EXPECT_EQ(q.best_day, m("250"));
  EXPECT_EQ(q.consistency_needed, Money{});
  ASSERT_TRUE(q.blocked.ok()); // The old $300 best would still block this payout.
  ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok());
}

TEST(TradingFunded, ConsistencyEscalatesByPayoutNumberAndRepeatsTheLastPercent) {
  ScriptedMarket f;
  auto rules = funded();
  rules.payouts.consistency_percents = {20, 25, 30};
  TradingSession s(config(rules), f.time);
  f.seed(s);
  const std::vector<md::Date> dates{{2026,9,23}, {2026,9,24}, {2026,9,25}, {2026,9,28}, {2026,9,29},
      {2026,9,30}, {2026,10,1}, {2026,10,2}, {2026,10,5}, {2026,10,6}, {2026,10,7}, {2026,10,8}, {2026,10,9}};
  std::size_t day = 0;
  for (const auto& [percent, count] : {std::pair{20, 5}, {25, 4}, {30, 4}}) {
    EXPECT_EQ(payout_quote(*s.snapshot(), rules).consistency_percent, percent);
    for (int i = 0; i < count; ++i) {
      profit_day(s, f, "60");
      next_day(s, f, dates.at(day++));
    }
    ASSERT_TRUE(s.request_payout(m("10"), f.time).decision.ok());
    EXPECT_EQ(payout_quote(*s.snapshot(), rules).cycle_profit, Money{});
    EXPECT_FALSE(payout_quote(*s.snapshot(), rules).best_day);
  }
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).consistency_percent, 30);
}

TEST(TradingFunded, ConsistencyIncludesLossesAndRoundsAdditionalProfitUpToCents) {
  ScriptedMarket f;
  auto rules = funded();
  rules.payouts.consistency_percents = {30};
  auto c = config(rules);
  c.fee_per_contract = m("0.65005");
  TradingSession s(c, f.time);
  f.seed(s);
  profit_day(s, f, "100.0001");
  next_day(s, f, {2026, 9, 23});
  profit_day(s, f, "-0.0001");
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("100"));
  EXPECT_EQ(q.consistency_needed, m("233.34"));
  profit_day(s, f, "-100");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, Money{});
  EXPECT_EQ(q.consistency_needed, m("333.34"));
  EXPECT_FALSE(q.blocked.actual);
  profit_day(s, f, "-50");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("-50"));
  EXPECT_EQ(q.consistency_needed, m("383.34"));
  EXPECT_EQ(q.blocked.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_FALSE(q.blocked.actual);
}

TEST(TradingFunded, ConsistencyDoesNotRoundAwayAMicroDollarBreach) {
  ScriptedMarket f;
  auto rules = funded();
  rules.payouts.consistency_percents = {50};
  auto c = config(rules);
  c.fee_per_contract = m("0.65005");
  TradingSession s(c, f.time);
  f.seed(s);
  profit_day(s, f, "100.0001");
  next_day(s, f, {2026, 9, 23});
  profit_day(s, f, "99.9999");
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("200"));
  EXPECT_EQ(q.consistency_needed, m("0.01"));
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  profit_day(s, f, "0.0002");
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.cycle_profit, m("200.0002"));
  EXPECT_EQ(q.consistency_needed, Money{});
  EXPECT_TRUE(q.blocked.ok());
}

TEST(TradingFunded, ConsistencyValidatesEveryPercent) {
  for (const auto percent : {0, -1, 101}) {
    auto rules = funded();
    rules.payouts.consistency_percents = {40, percent};
    try { validate_rules(rules); FAIL(); }
    catch (const TradingError& e) { EXPECT_EQ(e.code(), Reason::INVALID_RULES); }
  }
  auto rules = funded();
  rules.payouts.consistency_percents = {1, 100};
  EXPECT_NO_THROW(validate_rules(rules));
  rules.payouts.consistency_percents.resize(65, 40);
  EXPECT_THROW(validate_rules(rules), TradingError);
}

TEST(TradingFunded, BufferCapsTheFirstTwoPayoutsThenLifts) {
  ScriptedMarket f;
  auto rules = funded("100");
  rules.payouts.withdrawal_percent = 100;
  rules.payouts.caps.clear();
  rules.payouts.buffer = m("250");
  rules.payouts.buffer_payouts = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.buffer_balance, m("10250"));
  EXPECT_EQ(q.maximum, Money{});
  EXPECT_EQ(q.trader_share, Money{});
  EXPECT_EQ(q.blocked.message, "Equity must stay above the payout buffer of $10250.00");
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 24});
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.withdrawable, m("300"));
  EXPECT_EQ(q.maximum, m("50"));
  EXPECT_EQ(q.trader_share, m("40"));
  EXPECT_EQ(s.request_payout(m("50.01"), f.time).decision.code, Reason::INVALID_PAYOUT);
  ASSERT_TRUE(s.request_payout(m("50"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->equity, m("10250")); // Equality is allowed by the buffer cap.
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).buffer_balance, m("10250"));
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).maximum, m("100"));
  ASSERT_TRUE(s.request_payout(m("100"), f.time).decision.ok());
  EXPECT_FALSE(payout_quote(*s.snapshot(), rules).buffer_balance);
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 28});
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.maximum, m("349.99")); // The locked floor still applies.
  ASSERT_TRUE(s.request_payout(q.maximum, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->equity, m("10000.01"));
}

TEST(TradingFunded, BufferUsesTheAttemptsStartingBalanceAndWholeCents) {
  ScriptedMarket f;
  auto rules = funded();
  rules.max_drawdown = Money{};
  rules.lock_balance = Money{};
  rules.payouts.withdrawal_percent = 100;
  rules.payouts.caps.clear();
  rules.payouts.buffer = m("20.000001");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.reset_account(m("20000"), rules, "different start", f.time).decision.ok());
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.buffer_balance, m("20020.000001"));
  EXPECT_EQ(q.maximum, m("79.99"));
  ASSERT_TRUE(s.request_payout(q.maximum, f.time).decision.ok());
  EXPECT_EQ(payout_quote(*s.snapshot(), rules).buffer_balance, m("20020.000001"));
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 24});
  q = payout_quote(*s.snapshot(), rules);
  EXPECT_EQ(q.maximum, m("100"));
  ASSERT_TRUE(s.request_payout(q.maximum, f.time).decision.ok());
  EXPECT_TRUE(payout_quote(*s.snapshot(), rules).buffer_balance); // Zero payouts means every cycle.
}

TEST(TradingFunded, BufferValidatesAmountAndPayoutCount) {
  for (const auto count : {-1, 101}) {
    auto rules = funded();
    rules.payouts.buffer_payouts = count;
    try { validate_rules(rules); FAIL(); }
    catch (const TradingError& e) { EXPECT_EQ(e.code(), Reason::INVALID_RULES); }
  }
  auto rules = funded();
  rules.payouts.buffer = m("-0.000001");
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules.payouts.buffer = Money{};
  rules.payouts.buffer_payouts = 100;
  EXPECT_NO_THROW(validate_rules(rules));
  ScriptedMarket f;
  TradingSession s(config(rules), f.time);
  EXPECT_FALSE(payout_quote(*s.snapshot(), rules).buffer_balance); // Zero buffer disables even with a count.
}

TEST(TradingFunded, FloorLocksAtTheLockBalanceAndStopsTrailing) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_drawdown = m("100");
  rules.lock_balance = m("10000");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("9900"));
  EXPECT_FALSE(s.snapshot()->evaluation.floor_locked);
  quote(s, f, "4.40", "4.60");  // equity 10146.75: peak - 100 passes the lock balance
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("10000"));
  EXPECT_TRUE(s.snapshot()->evaluation.floor_locked);
  quote(s, f, "4.60", "4.80");  // a new high no longer moves the floor
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10246.75"));
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("10000"));
  quote(s, f, "4.20", "4.40");  // 10046.75 would breach a trailing floor of 10146.75
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.00", "4.20");
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_NE(e.decision.find("drawdown floor $10000.00"), std::string::npos) << e.decision;
}

TEST(TradingFunded, QualifyingDaysCountNetRealisedProfitOnFundedAccountsOnly) {
  ScriptedMarket f;
  TradingSession s(config(funded("1000", 8)), f.time);
  f.seed(s);
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 23});
  auto e = s.snapshot()->evaluation;
  ASSERT_EQ(e.days.size(), 1);
  EXPECT_EQ(e.days[0].realised, m("93.50"));
  EXPECT_TRUE(e.days[0].qualifying);
  EXPECT_EQ(e.qualifying_days, 1);
  // A losing day and a small winning day do not qualify.
  ASSERT_TRUE(s.submit(f.market("lose-open", 5), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("lose-close", 5, Side::Sell), f.time).decision.ok());
  next_day(s, f, {2026, 9, 24});
  ASSERT_TRUE(s.submit(f.market("small-open", 1), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  ASSERT_TRUE(s.submit(f.market("small-close", 1, Side::Sell), f.time).decision.ok());
  next_day(s, f, {2026, 9, 25});
  e = s.snapshot()->evaluation;
  ASSERT_EQ(e.days.size(), 3);
  EXPECT_EQ(e.days[1].realised, m("-106.50"));
  EXPECT_FALSE(e.days[1].qualifying);
  EXPECT_EQ(e.days[2].realised, m("18.70"));
  EXPECT_FALSE(e.days[2].qualifying);
  EXPECT_EQ(e.qualifying_days, 1);

  // The same winning day on an evaluation account records its P&L but never qualifies.
  ScriptedMarket g;
  auto evaluation = funded();
  evaluation.phase = Phase::Evaluation;
  TradingSession t(config(evaluation), g.time);
  g.seed(t);
  winning_trade(t, g);
  next_day(t, g, {2026, 9, 23});
  EXPECT_EQ(t.snapshot()->evaluation.days.at(0).realised, m("93.50"));
  EXPECT_FALSE(t.snapshot()->evaluation.days.at(0).qualifying);
  EXPECT_EQ(t.snapshot()->evaluation.qualifying_days, 0);
}

TEST(TradingFunded, PayoutWithdrawsWithinTheRulesAndStartsANewCycle) {
  ScriptedMarket f;
  TradingSession s(config(funded()), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  auto result = s.request_payout(m("10"), f.time);
  EXPECT_EQ(result.decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_NE(result.decision.message.find("Close every position"), std::string::npos);
  ASSERT_TRUE(s.submit(f.market("close", 5, Side::Sell), f.time).decision.ok());
  result = s.request_payout(m("10"), f.time);
  EXPECT_EQ(result.decision.code, Reason::PAYOUT_NOT_ELIGIBLE);
  EXPECT_EQ(result.decision.limit, 1);
  next_day(s, f, {2026, 9, 23});

  auto quote_now = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_TRUE(quote_now.blocked.ok()) << quote_now.blocked.message;
  EXPECT_EQ(quote_now.number, 1);
  EXPECT_EQ(quote_now.profit, m("93.50"));
  EXPECT_EQ(quote_now.withdrawable, m("46.75"));
  EXPECT_EQ(quote_now.cap, m("60"));
  EXPECT_EQ(quote_now.maximum, m("46.75"));
  EXPECT_EQ(quote_now.trader_share, m("37.40"));
  EXPECT_EQ(s.request_payout(m("5"), f.time).decision.code, Reason::INVALID_PAYOUT);
  result = s.request_payout(m("46.76"), f.time);
  EXPECT_EQ(result.decision.code, Reason::INVALID_PAYOUT);
  EXPECT_EQ(result.decision.limit, 46.75);

  const auto before = s.snapshot();
  EXPECT_EQ(before->evaluation.peak, m("10146.75"));
  ASSERT_TRUE(s.request_payout(m("46.75"), f.time).decision.ok());
  auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, m("10046.75"));
  EXPECT_EQ(snap->account.realised, before->account.realised);
  // Not a loss: the day's baseline and the unlocked trailing peak move with it.
  EXPECT_EQ(snap->start_of_day_equity, m("10046.75"));
  EXPECT_EQ(snap->risk.daily_loss, Money{});
  EXPECT_EQ(snap->evaluation.peak, m("10100"));
  EXPECT_EQ(snap->evaluation.floor, m("9100"));
  ASSERT_EQ(snap->evaluation.payouts.size(), 1);
  const auto payout = snap->evaluation.payouts[0];
  EXPECT_EQ(payout.number, 1);
  EXPECT_EQ(payout.time, f.time);
  EXPECT_EQ(payout.day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(payout.amount, m("46.75"));
  EXPECT_EQ(payout.trader_share, m("37.40"));
  EXPECT_EQ(payout.balance, m("10093.50"));
  EXPECT_EQ(snap->evaluation.qualifying_days, 0);
  EXPECT_EQ(snap->evaluation.cycle_started, f.time);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_NOT_ELIGIBLE);

  // Each day counts once, toward the cycle in progress when it closes: a win
  // after the request on the payout day starts the next cycle.
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 24});
  EXPECT_TRUE(s.snapshot()->evaluation.days.back().qualifying);
  EXPECT_EQ(s.snapshot()->evaluation.days.back().day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(s.snapshot()->evaluation.qualifying_days, 1);
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.qualifying_days, 2);

  quote_now = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_EQ(quote_now.profit, m("233.75"));
  EXPECT_EQ(quote_now.withdrawable, m("116.87"));
  EXPECT_EQ(quote_now.cap, m("100"));
  EXPECT_EQ(quote_now.maximum, m("100"));
  ASSERT_TRUE(s.request_payout(m("100"), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.payouts.at(1).number, 2);
  EXPECT_EQ(snap->evaluation.payouts.at(1).trader_share, m("80"));
  EXPECT_EQ(snap->account.cash, m("10133.75"));
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
}

TEST(TradingFunded, APayoutBeforeRolloverBelongsToTheUnrolledDay) {
  ScriptedMarket f;
  auto rules = funded();
  rules.drawdown_mode = DrawdownMode::EndOfDay;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  winning_trade(s, f);
  next_day(s, f, {2026, 9, 23});
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10093.50"));
  winning_trade(s, f);  // closes at 10187.00
  // After midnight, before the next rollover: the request joins Sep 23.
  f.time = md::new_york_to_utc({2026, 9, 24}, 0, 30);
  ASSERT_TRUE(s.request_payout(m("60"), f.time).decision.ok());
  auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.payouts.at(0).day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(e.day_close_equity, m("10127"));
  EXPECT_EQ(e.peak, m("10033.50"));
  next_day(s, f, {2026, 9, 24});
  e = s.snapshot()->evaluation;
  // The close ratchets net of the withdrawal: $1,000 of room remains, as without it.
  EXPECT_EQ(e.peak, m("10127"));
  EXPECT_EQ(e.floor, m("9127"));
  EXPECT_EQ(s.snapshot()->equity - e.floor, m("1000"));
  const auto& day = e.days.back();
  EXPECT_EQ(day.day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(day.open_equity, m("10033.50"));
  EXPECT_EQ(day.close_equity, m("10127"));
  EXPECT_EQ(day.realised, m("93.50"));
  EXPECT_TRUE(day.qualifying);
  EXPECT_EQ(e.qualifying_days, 1);  // Sep 23 closed after the request, so it starts the next cycle
}

TEST(TradingFunded, PayoutsLeaveEquityAboveALockedFloor) {
  ScriptedMarket f;
  auto rules = funded("100");
  rules.payouts.withdrawal_percent = 100;
  rules.payouts.caps.clear();
  TradingSession s(config(rules), f.time);
  f.seed(s);
  winning_trade(s, f);
  ASSERT_TRUE(s.snapshot()->evaluation.floor_locked);
  next_day(s, f, {2026, 9, 23});
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_EQ(q.withdrawable, m("93.50"));
  EXPECT_FALSE(q.cap);
  EXPECT_EQ(q.maximum, m("93.49"));
  EXPECT_EQ(s.request_payout(m("93.50"), f.time).decision.code, Reason::INVALID_PAYOUT);
  ASSERT_TRUE(s.request_payout(m("93.49"), f.time).decision.ok());
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Active);
  EXPECT_EQ(e.floor, m("10000"));
  EXPECT_EQ(e.peak, m("10146.75"));
  EXPECT_EQ(s.snapshot()->equity, m("10000.01"));
}

TEST(TradingFunded, PayoutsNeedAFundedAccountAndWholeCents) {
  ScriptedMarket f;
  TradingSession s(config({}), f.time);
  f.seed(s);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_UNAVAILABLE);
  EXPECT_FALSE(payout_quote(*s.snapshot(), s.config().rules).funded);
  for (const auto* amount : {"0", "-1", "10.001"}) {
    try {
      (void)s.request_payout(m(amount), f.time);
      ADD_FAILURE() << "expected INVALID_PAYOUT for " << amount;
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::INVALID_PAYOUT); }
  }
  auto invalid = funded();
  invalid.payouts.split_percent = 101;
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid = funded();
  invalid.payouts.caps = {m("0")};
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid = funded();
  invalid.profit_target = m("100");  // a funded account never passes
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid = funded();
  invalid.payouts.qualifying_days = 0;
  EXPECT_THROW(validate_rules(invalid), TradingError);
}

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
TEST(TradingFunded, SizeScalingJournalRoundTripsAndContinuesTheSameReview) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-size-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "account.jsonl").string();
  ScriptedMarket f;
  auto rules = size_plan(2, 1);
  rules.flat_time = 945; rules.no_overnight = true;
  rules.inactivity_days = 14;
  rules.underlyings = {"SPX"};
  rules.trading_start = 570; rules.trading_end = 960;
  TradingSession s(config(rules), f.time, FileJournal::create(path));
  f.seed(s);
  profit_day(s, f, "150");
  const auto last_activity = f.time;
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
  auto recovered = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(recovered.snapshot()->evaluation.last_activity, last_activity);
  EXPECT_EQ(recovered.config().rules, rules);
  auto other = f;
  next_day(s, f, {2026, 9, 24});
  next_day(recovered, other, {2026, 9, 24});
  EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
  auto scaled = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(scaled.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(scaled.config().rules, s.config().rules);
  EXPECT_EQ(scaled.snapshot()->evaluation.last_activity, last_activity);
  EXPECT_EQ(scaled.snapshot()->evaluation.scaling_limit, 4);
  EXPECT_EQ(scaled.snapshot()->evaluation.starting_balance, m("12500"));
  const auto capture = std::make_shared<CapturingJournal>();
  TradingSession::expand(FileJournal::read(path), *capture);
  std::size_t events = 0;
  for (const auto& entry : capture->entries) {
    const auto record = nlohmann::json::parse(entry.payload);
    bool finished_day = false;
    for (const auto& event : record.at("events")) {
      if (event.at("type") == "evaluation_day") finished_day = true;
      if (event.at("type") == "account_scaled") {
        EXPECT_TRUE(finished_day);
        ++events;
      }
    }
  }
  EXPECT_EQ(events, 1U);
  // Older replay inputs predate program bookkeeping and must not acquire new archive fields.
  ASSERT_TRUE(scaled.reset_account(m("10000"), funded(), "legacy reset", f.time, false, false).decision.ok());
  EXPECT_FALSE(scaled.snapshot()->attempts.back().size_scaling);
  EXPECT_FALSE(nlohmann::json::parse(scaled.snapshot_json())["attempts"].back().contains("size_scaling"));
  ASSERT_TRUE(s.reset_account(m("10000"), funded(), "new attempt", f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->attempts.back().rules->size_scaling);
  EXPECT_TRUE(s.snapshot()->attempts.back().size_scaling);
  EXPECT_FALSE(s.snapshot()->evaluation.size_scaling);
  std::filesystem::remove_all(directory);
}

TEST(TradingFunded, SizeScalingPreservesPurchasedTermsResetCountsAndRecovery) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-size-costs-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "account.jsonl").string();
  ScriptedMarket f;
  auto rules = size_plan(1, 0);
  rules.lock_balance = m("10000");
  rules.evaluation_fee = m("100.000001"); rules.reset_fee = m("25.000002"); rules.max_resets = 2;
  TradingSession s(config(rules), f.time, FileJournal::create(path));
  f.seed(s);
  ASSERT_TRUE(s.reset_account(m("10000"), rules, "first reset", f.time).decision.ok());
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 23});
  ASSERT_EQ(s.snapshot()->evaluation.starting_balance, m("12500"));
  EXPECT_EQ(s.config().rules.lock_balance, m("12500"));
  EXPECT_EQ(s.config().rules.max_drawdown, m("1250.03"));
  EXPECT_EQ(s.config().rules.daily_loss_limit, m("625.03"));
  EXPECT_EQ(original_program_rules(s.config().rules, m("12500"), s.snapshot()->evaluation.size_scaling), rules);
  EXPECT_EQ(program_costs(*s.snapshot(), s.config().rules).resets_used, 1);
  auto grown = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(grown.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(program_costs(*grown.snapshot(), grown.config().rules).resets_used, 1);
  // Buying the grown size and its current loss amounts is a different purchase.
  ASSERT_TRUE(grown.reset_account(m("12500"), grown.config().rules, "buy larger plan", f.time).decision.ok());
  EXPECT_EQ(program_costs(*grown.snapshot(), grown.config().rules).resets_used, 0);
  EXPECT_EQ(grown.snapshot()->fee_charged.kind, "evaluation");
  // The terminal resubmits the displayed scaled limits, which must restore the originals.
  auto legacy = TradingSession::recover(FileJournal::read(path));
  ASSERT_TRUE(legacy.reset_account(m("10000"), legacy.config().rules, "old reset", f.time, false, true, false).decision.ok());
  EXPECT_EQ(legacy.config().rules.max_drawdown, m("1250.03"));
  ASSERT_TRUE(s.reset_account(m("10000"), s.config().rules, "restart original plan", f.time).decision.ok());
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.snapshot()->evaluation.size_scaling->original_max_drawdown, rules.max_drawdown);
  EXPECT_EQ(s.snapshot()->evaluation.size_scaling->original_daily_loss_limit, rules.daily_loss_limit);
  EXPECT_TRUE(s.snapshot()->evaluation.size_scaling->history.empty());
  EXPECT_EQ(s.snapshot()->fee_charged.kind, "reset");
  EXPECT_EQ(s.snapshot()->fee_charged.amount, rules.reset_fee);
  EXPECT_EQ(s.snapshot()->attempts.back().starting_balance, m("12500"));
  EXPECT_EQ(s.snapshot()->attempts.back().rules->max_drawdown, m("1250.03"));
  EXPECT_EQ(program_costs(*s.snapshot(), rules).resets_used, 2);
  // Growing again cannot replenish the reset allowance.
  profit_day(s, f, "200");
  next_day(s, f, {2026, 9, 24});
  auto restored = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(restored.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(program_costs(*restored.snapshot(), restored.config().rules).resets_used, 2);
  const auto refused = restored.reset_account(m("10000"), restored.config().rules, "exhausted", f.time);
  EXPECT_EQ(refused.decision.code, Reason::RESET_LIMIT);
  EXPECT_EQ(refused.decision.actual, 3); EXPECT_EQ(refused.decision.limit, 2);
  const auto recovery = FileJournal::read(path);
  const auto compacted = (directory / "compacted.jsonl").string();
  EXPECT_EQ(TradingSession::compact(recovery, *FileJournal::create(compacted)), s.snapshot_json());
  EXPECT_EQ(FileJournal::read(compacted).head, recovery.head);
  for (const bool contracts : {true, false}) {
    auto changed = rules;
    if (contracts) changed.scaling.back().contracts = 5;
    else changed.size_scaling->days = 2;
    EXPECT_FALSE(same_program_rules(rules, changed));
    auto purchase = TradingSession::recover(recovery);
    ASSERT_TRUE(purchase.reset_account(m("10000"), changed, "different scaling rules", f.time).decision.ok());
    EXPECT_EQ(purchase.snapshot()->fee_charged.kind, "evaluation");
    EXPECT_EQ(program_costs(*purchase.snapshot(), changed).resets_used, 0);
  }
  std::filesystem::remove_all(directory);
}

TEST(TradingFunded, DisabledSizeScalingLeavesJournalBytesUntouched) {
  ScriptedMarket f;
  auto a = std::make_shared<CapturingJournal>(), b = std::make_shared<CapturingJournal>();
  auto rules = funded();
  TradingSession first(config(rules), f.time, a);
  rules.size_scaling = std::nullopt;
  TradingSession second(config(rules), f.time, b);
  f.seed(first); f.seed(second);
  ASSERT_EQ(a->entries.size(), b->entries.size());
  for (std::size_t i = 0; i < a->entries.size(); ++i) {
    EXPECT_EQ(a->entries[i].payload, b->entries[i].payload);
    EXPECT_EQ(a->entries[i].payload.find("size_scaling"), std::string::npos);
    EXPECT_EQ(a->entries[i].payload.find("account_scaled"), std::string::npos);
  }
}

/// Rewrite a transaction as the schema 2 record written before funded accounts
/// and multi-leg orders.
std::string earlier_schema_two(std::string_view payload) {
  auto j = nlohmann::json::parse(payload);
  for (auto* e : {&j["state"]["evaluation"], &j["snapshot"]["evaluation"]}) {
    for (const auto* key : {"floor_locked", "day_open_realised", "qualifying_days", "cycle_started", "payouts"}) e->erase(key);
    for (auto& day : (*e)["days"]) { day.erase("realised"); day.erase("qualifying"); }
  }
  for (const auto* key : {"phase", "lock_balance", "payouts"}) j["state"]["config"]["rules"].erase(key);
  for (auto& order : j["state"]["orders"]) order["request"].erase("legs");
  for (const auto* list : {"recent_orders", "open_orders"})
    for (auto& order : j["snapshot"][list]) order["request"].erase("legs");
  return j.dump();
}

TEST(TradingFunded, EarlierSchemaTwoJournalsRecoverAsEvaluations) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-funded-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto current = (directory / "current.jsonl").string();
  const auto path = (directory / "earlier.jsonl").string();
  ScriptedMarket f;
  auto capture = std::make_shared<CapturingJournal>();
  AccountRules rules;
  rules.plan = "Earlier";
  rules.max_drawdown = m("1000");
  {
    TradingSession s(config(rules), f.time, FileJournal::create(current));
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
  }
  // Every state whole, as schema 2 wrote it, to rewrite as the earlier schema 2.
  TradingSession::expand(FileJournal::read(current), *capture);
  {
    auto file = FileJournal::create(path);
    for (const auto& entry : capture->entries) file->append(entry.time, entry.type, earlier_schema_two(entry.payload));
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.config().rules.phase, Phase::Evaluation);
  EXPECT_FALSE(s.snapshot()->recent_orders.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.front().request.legs.empty());
  EXPECT_EQ(s.config().rules.lock_balance, Money{});
  auto e = s.snapshot()->evaluation;
  EXPECT_FALSE(e.floor_locked);
  EXPECT_TRUE(e.payouts.empty());
  EXPECT_EQ(e.days.at(0).realised, Money{});
  EXPECT_FALSE(e.days.at(0).qualifying);
  EXPECT_EQ(e.cycle_started, 0);
  EXPECT_FALSE(payout_quote(*s.snapshot(), s.config().rules).funded);
  // The first new transaction starts the payout cycle with the attempt.
  quote(s, f, "4.00", "4.20");
  e = s.snapshot()->evaluation;
  EXPECT_EQ(e.cycle_started, e.started);
  EXPECT_GT(e.started, 0);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_UNAVAILABLE);
  std::filesystem::remove_all(directory);
}

TEST(TradingFunded, JournalRecoveryRestoresPayoutsAndQualifyingDays) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-funded-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "funded.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(config(funded("100")), f.time, journal);
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
    ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
    winning_trade(s, f);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto s = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  const auto e = s.snapshot()->evaluation;
  EXPECT_TRUE(e.floor_locked);
  ASSERT_EQ(e.payouts.size(), 1);
  EXPECT_EQ(e.payouts[0].amount, m("40"));
  EXPECT_EQ(e.days.at(0).realised, m("93.50"));
  EXPECT_TRUE(e.days.at(0).qualifying);
  EXPECT_EQ(s.config().rules.payouts.caps.size(), 2);
  EXPECT_EQ(s.config().rules.phase, Phase::Funded);
  const auto capture = std::make_shared<CapturingJournal>();
  TradingSession::expand(FileJournal::read(path), *capture);
  for (const auto& entry : capture->entries) {
    const auto recorded = nlohmann::json::parse(entry.payload)["state"]["config"]["rules"]["payouts"];
    EXPECT_EQ(recorded.size(), 6); // The original funded journal fields and bytes.
    EXPECT_FALSE(recorded.contains("consistency_percents"));
    EXPECT_FALSE(recorded.contains("buffer"));
    EXPECT_FALSE(recorded.contains("buffer_payouts"));
  }
  EXPECT_TRUE(s.config().rules.payouts.consistency_percents.empty());
  std::filesystem::remove_all(directory);
}

TEST(TradingFunded, ConsistencyJournalRecoversRulesAndTheCycle) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-consistency-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "funded.jsonl").string();
  auto rules = funded();
  rules.payouts.consistency_percents = {50, 60};
  rules.payouts.buffer = m("50");
  rules.payouts.buffer_payouts = 2;
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(path));
    f.seed(s);
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 23});
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 24});
    ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
    winning_trade(s, f);
    next_day(s, f, {2026, 9, 25});
    expected = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  EXPECT_EQ(s.config().rules, rules);
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  EXPECT_EQ(q.consistency_percent, 60);
  EXPECT_EQ(q.buffer_balance, m("10050"));
  EXPECT_EQ(q.cycle_profit, m("93.50"));
  EXPECT_EQ(q.best_day_date, (md::Date{2026, 9, 24}));
  EXPECT_EQ(q.consistency_needed, m("62.34"));
  winning_trade(s, f);
  EXPECT_TRUE(payout_quote(*s.snapshot(), rules).blocked.ok());
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading
