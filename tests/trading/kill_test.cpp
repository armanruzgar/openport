#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config() {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.max_daily_loss = m("100");
  c.limits.price_band_absolute = m("5");
  return c;
}
OrderRequest spread(std::string id, const ScriptedMarket& a, const ScriptedMarket& b, bool close,
                    Quantity quantity = 1, std::optional<Money> limit = {}) {
  OrderRequest r;
  r.client_order_id = std::move(id);
  r.quantity = quantity;
  r.type = limit ? OrderType::Limit : OrderType::Market;
  r.tif = limit ? TimeInForce::Day : TimeInForce::Ioc;
  r.limit_price = limit;
  r.legs = {{a.symbol(), close ? Side::Sell : Side::Buy, 2}, {b.symbol(), close ? Side::Buy : Side::Sell, 1}};
  return r;
}

TEST(TradingKill, ClosingLimitWorksUnderManualAndDailyLossLatch) {
  for (const bool daily : {false, true}) {
    ScriptedMarket f;
    auto c = config();
    c.rules.defined_risk = true;
    TradingSession s(c, f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
    if (daily) {
      f.next();
      s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
    } else {
      s.trip_kill("manual", f.time);
    }
    ASSERT_TRUE(s.snapshot()->risk.kill_latched);
    EXPECT_EQ(s.submit(f.market("increase"), f.time).decision.code, Reason::KILL_SWITCH);
    EXPECT_EQ(s.submit(f.market("flip", 3, Side::Sell), f.time).decision.code, Reason::KILL_SWITCH);
    const auto close = s.submit(f.limit("close", 2, daily ? "1.50" : "4.50", Side::Sell), f.time);
    ASSERT_TRUE(close.decision.ok());
    ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
    // Only part fills on the next quote; the remainder still counts as a close.
    f.next();
    s.on_quotes({f.quote(daily ? "1.50" : "4.50", daily ? "1.70" : "4.70", 1)}, {f.valuation()}, f.time);
    ASSERT_EQ(s.snapshot()->positions.size(), 1U);
    EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 1);
    f.next();
    s.on_quotes({f.quote(daily ? "1.50" : "4.50", daily ? "1.70" : "4.70", 1)}, {f.valuation()}, f.time);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  }
}

TEST(TradingKill, TripKeepsClosingOrdersAndBothBracketExits) {
  for (const bool daily : {false, true}) {
    for (const bool stop : {false, true}) {
      ScriptedMarket f;
      TradingSession s(config(), f.time);
      f.seed(s);
      auto entry = f.market("entry", 2);
      entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("1")}, {}},
                              ExitSpec{{}, m("5")}};
      ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
      ASSERT_TRUE(s.submit(f.limit("manual-close", 1, "5.50", Side::Sell), f.time).decision.ok());
      ASSERT_TRUE(s.submit(f.limit("opening", 1, "3.50"), f.time).decision.ok());
      if (daily) {
        f.next();
        s.on_quotes({f.quote("2", "2.20")}, {f.valuation()}, f.time);
      } else {
        s.trip_kill("manual", f.time);
      }
      ASSERT_TRUE(s.snapshot()->risk.kill_latched);
      ASSERT_EQ(s.snapshot()->open_orders.size(), 3U);
      s.set_limits(s.config().limits, f.time);
      EXPECT_EQ(s.snapshot()->open_orders.size(), 3U);
      EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::KILL_SWITCH);
      f.next();
      s.on_quotes({f.quote(stop ? "1" : "5", stop ? "1.20" : "5.20")}, {f.valuation()}, f.time);
      EXPECT_TRUE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
      // A surviving manual close cannot flip the position after an exit filled.
      f.next();
      s.on_quotes({f.quote("5.50", "5.70")}, {f.valuation()}, f.time);
      EXPECT_TRUE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
    }
  }
}

TEST(TradingKill, FlattenAfterDailyLossClosesEveryPosition) {
  ScriptedMarket a, b;
  b.contract.strike += 10;
  auto c = config();
  c.rules.defined_risk = true;
  TradingSession s(c, a.time);
  a.seed(s);
  b.seed(s);
  ASSERT_TRUE(s.submit(spread("open", a, b, false), a.time).decision.ok());
  a.next(); b.next();
  s.on_quotes({a.quote("1", "1.20"), b.quote()}, {a.valuation(), b.valuation()}, a.time);
  ASSERT_TRUE(s.snapshot()->risk.kill_latched);
  ASSERT_EQ(s.snapshot()->positions.size(), 2U);
  ASSERT_TRUE(s.close_positions({}, a.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // The short closes with one of the two longs as one order (two fills); the other long alone.
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 5U);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
}

TEST(TradingKill, ClosingSpreadWorksButOpeningOrMixedLegsDoNot) {
  for (const bool daily : {false, true}) {
    ScriptedMarket a, b;
    b.contract.strike += 10;
    TradingSession s(config(), a.time);
    a.seed(s); b.seed(s);
    ASSERT_TRUE(s.submit(spread("open", a, b, false), a.time).decision.ok());
    if (daily) {
      a.next(); b.next();
      s.on_quotes({a.quote("1", "1.20"), b.quote()}, {a.valuation(), b.valuation()}, a.time);
    } else {
      s.trip_kill("manual", a.time);
    }
    EXPECT_EQ(s.submit(spread("increase", a, b, false), a.time).decision.code, Reason::KILL_SWITCH);
    auto mixed = spread("mixed", a, b, true);
    mixed.legs[1].side = Side::Sell;
    EXPECT_EQ(s.submit(mixed, a.time).decision.code, Reason::KILL_SWITCH);
    const auto close = s.submit(spread("close", a, b, true, 1, m(daily ? "1.00" : "-4.00")), a.time);
    ASSERT_TRUE(close.decision.ok());
    ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
    a.next(); b.next();
    s.on_quotes({a.quote(daily ? "2" : "5", daily ? "2.20" : "5.20"), b.quote()},
                {a.valuation(), b.valuation()}, a.time);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 4U);
    EXPECT_EQ(s.submit(spread("new", a, b, false), a.time).decision.code, Reason::KILL_SWITCH);
  }
}

TEST(TradingKill, WorkingSingleAndMultiLegOrdersShareClosingCapacity) {
  for (const bool combo_first : {false, true}) {
    ScriptedMarket a, b;
    b.contract.strike += 10;
    TradingSession s(config(), a.time);
    a.seed(s); b.seed(s);
    ASSERT_TRUE(s.submit(spread("open", a, b, false, 2), a.time).decision.ok());
    s.trip_kill("manual", a.time);
    const auto combo = spread("combo", a, b, true, 1, m("-5"));
    const auto single = a.limit("single", 2, "5", Side::Sell);
    ASSERT_TRUE(s.submit(combo_first ? combo : single, a.time).decision.ok());
    ASSERT_TRUE(s.submit(combo_first ? single : combo, a.time).decision.ok());
    EXPECT_EQ(s.submit(a.limit("too-many", 1, "5", Side::Sell), a.time).decision.code, Reason::KILL_SWITCH);
    EXPECT_EQ(s.submit(spread("too-many-combos", a, b, true, 1, m("-5")), a.time).decision.code, Reason::KILL_SWITCH);
    a.next(); b.next();
    s.on_quotes({a.quote("5", "5.20"), b.quote()}, {a.valuation(), b.valuation()}, a.time);
    ASSERT_EQ(s.snapshot()->positions.size(), 1U);
    EXPECT_EQ(s.snapshot()->positions[0].position.contract.osi_symbol(), b.symbol());
    EXPECT_EQ(s.snapshot()->positions[0].position.quantity, -1);
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 5U);
  }
}

TEST(TradingKill, TripCancelsExcessWorkingClosesAndCannotFlip) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  for (const auto id : {"close-1", "close-2", "excess"})
    ASSERT_TRUE(s.submit(f.limit(id, 1, "5", Side::Sell), f.time).decision.ok());
  s.trip_kill("manual", f.time);
  ASSERT_EQ(s.snapshot()->open_orders.size(), 2U);
  const auto id = s.snapshot()->open_orders.front().id;
  OrderChange change;
  change.quantity = 2;
  EXPECT_EQ(s.modify(id, change, f.time).decision.code, Reason::KILL_SWITCH);
  f.next();
  s.on_quotes({f.quote("5", "5.20")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 3U);
}

TEST(TradingKill, TripPreservesCloseAheadOfAnOpeningOrderOnTheSameSide) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("close", 1, "5", Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("flip", 3, "5", Side::Sell), f.time).decision.ok());
  s.trip_kill("manual", f.time);
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  EXPECT_EQ(s.snapshot()->open_orders[0].request.client_order_id, "close");
  f.next();
  s.on_quotes({f.quote("5", "5.20")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 1);
}

TEST(TradingKill, SystemAutoCloseStillWorksWhileLatched) {
  ScriptedMarket f;
  f.contract.expiry = {2026, 9, 22};
  auto c = config();
  c.rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  s.trip_kill("manual", f.time);
  f.time = f.contract.last_trade_time() - c.rules.expiry_cutoff;
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.back().system);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
}

TEST(TradingKill, ClosingStillChecksDefinedRiskAndSurvivesADecision) {
  ScriptedMarket a, b;
  b.contract.strike += 10;
  auto c = config();
  c.rules.defined_risk = true;
  c.rules.profit_target = m("100");
  TradingSession s(c, a.time);
  a.seed(s); b.seed(s);
  auto equal = spread("open", a, b, false);
  equal.legs[0].ratio = 1;
  equal.quantity = 2;
  ASSERT_TRUE(s.submit(equal, a.time).decision.ok());
  s.trip_kill("manual", a.time);
  EXPECT_EQ(s.submit(a.market("uncover", 1, Side::Sell), a.time).decision.code, Reason::DEFINED_RISK);
  a.next(); b.next();
  s.on_quotes({a.quote("6", "6.20", 1), b.quote()}, {a.valuation(), b.valuation()}, a.time);
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  // The rule closes only part of the long on this quote. The trader can close
  // the rest despite both the kill latch and the decided attempt.
  EXPECT_TRUE(s.submit(a.market("decided", 1, Side::Sell), a.time).decision.ok());
  EXPECT_EQ(s.submit(a.market("opening"), a.time).decision.code, Reason::EVALUATION_CLOSED);
}

TEST(TradingKill, TheLatchKeepsItsHistoryAndSaysWhenAResetCanClearIt) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->kill_history.empty());
  EXPECT_TRUE(s.snapshot()->kill_reset.ok()) << "nothing latched, nothing to clear";
  // The marks fall 620 against a 100 allowance: the latch trips.
  f.next();
  s.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
  const auto tripped = f.time;
  auto snap = s.snapshot();
  ASSERT_TRUE(snap->risk.kill_latched);
  ASSERT_EQ(snap->kill_history.size(), 1u);
  EXPECT_EQ(snap->kill_history[0].action, "trip");
  EXPECT_EQ(snap->kill_history[0].reason, "DAILY_LOSS");
  EXPECT_EQ(snap->kill_history[0].time, tripped);
  EXPECT_EQ(snap->kill_reset.code, Reason::DAILY_LOSS);
  EXPECT_NE(snap->kill_reset.message.find("back within the limit, or from the next trading day"), std::string::npos)
      << snap->kill_reset.message;
  EXPECT_EQ(snap->kill_reset.limit, 100.0);
  ASSERT_TRUE(snap->kill_reset.actual);
  EXPECT_GT(*snap->kill_reset.actual, 100.0);
  // Opening orders say which latch refused them.
  const auto refused = s.submit(f.market("more"), f.time).decision;
  EXPECT_EQ(refused.code, Reason::KILL_SWITCH);
  EXPECT_EQ(refused.message, "The daily loss latch is set: the marked loss exceeded the limit of $100.00");
  EXPECT_EQ(refused.limit, 100.0);
  // A manual trip over it names the reason it replaced, and a reset that trips
  // again answers why, as the snapshot said it would.
  f.next();
  s.set_actor("trader");
  ASSERT_TRUE(s.trip_kill("stepping away", f.time).decision.ok());
  snap = s.snapshot();
  ASSERT_EQ(snap->kill_history.size(), 2u);
  EXPECT_EQ(snap->kill_history[1].action, "trip");
  EXPECT_EQ(snap->kill_history[1].reason, "stepping away");
  EXPECT_EQ(snap->kill_history[1].previous, "DAILY_LOSS");
  EXPECT_EQ(snap->kill_history[1].actor, "trader");
  EXPECT_EQ(s.submit(f.market("manual"), f.time).decision.message, "The kill switch is latched: stepping away");
  s.set_actor("trader");
  const auto reset = s.reset_kill("back", f.time).decision;
  EXPECT_EQ(reset.code, Reason::DAILY_LOSS);
  EXPECT_EQ(reset.message, s.snapshot()->kill_reset.message);
  snap = s.snapshot();
  ASSERT_EQ(snap->kill_history.size(), 4u) << "the reset, then the loss tripping it again";
  EXPECT_EQ(snap->kill_history[2].action, "reset");
  EXPECT_EQ(snap->kill_history[2].reason, "back");
  EXPECT_EQ(snap->kill_history[2].previous, "stepping away");
  EXPECT_EQ(snap->kill_history[3].reason, "DAILY_LOSS");
  // Once the loss is back within the limit a reset clears it.
  f.next();
  s.on_quotes({f.quote("4.10", "4.20")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->kill_reset.ok()) << s.snapshot()->kill_reset.message;
  ASSERT_TRUE(s.reset_kill("recovered", f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->kill_history.back().action, "reset");
}

TEST(TradingKill, AGuardrailSaysWhatItMeasuredAndWhenItEnds) {
  ScriptedMarket f;
  auto c = config();
  c.limits.max_daily_loss = m("10000");
  c.guardrails.max_opening_trades = 1;
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("first"), f.time).decision.ok());
  const auto refused = s.submit(f.market("second"), f.time).decision;
  EXPECT_EQ(refused.code, Reason::TRADE_LIMIT);
  EXPECT_EQ(refused.message, "The day's opening trades reached the limit of 1; opening orders resume next trading day");
  EXPECT_EQ(refused.actual, 1.0);
  EXPECT_EQ(refused.limit, 1.0);
  EXPECT_EQ(refused.scope, "aggregate");
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->kill_reset.code, Reason::TRADE_LIMIT);
  EXPECT_NE(snap->kill_reset.message.find("next trading day's rollover"), std::string::npos) << snap->kill_reset.message;
  EXPECT_EQ(s.reset_kill("try", f.time).decision.message, snap->kill_reset.message);
  ASSERT_EQ(snap->kill_history.size(), 1u);
  EXPECT_EQ(snap->kill_history[0].reason, "TRADE_LIMIT");
}

TEST(TradingKill, ACooldownSaysWhenItEndsAndItsReleaseIsKept) {
  ScriptedMarket f;
  auto c = config();
  c.limits.max_daily_loss = m("10000");
  c.guardrails.cooldown_minutes = 30;
  c.guardrails.cooldown_loss = m("50");
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long"), f.time).decision.ok());
  f.next();
  s.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("out", 1, Side::Sell), f.time).decision.ok());
  const auto refused = s.submit(f.market("again"), f.time).decision;
  EXPECT_EQ(refused.code, Reason::COOLDOWN);
  EXPECT_EQ(refused.message, "Cooldown after a stop-loss exit or loss until 10:30:01 ET");
  EXPECT_EQ(s.snapshot()->kill_reset.message,
            "The kill-switch reset waits for the cooldown to end at 10:30:01 ET; an account reset also preserves this cooldown");
  // It releases on market time, and the history keeps the release.
  f.time += 30 * md::kNanosPerMinute;
  ++f.observation;
  s.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
  const auto snap = s.snapshot();
  EXPECT_FALSE(snap->risk.kill_latched);
  ASSERT_EQ(snap->kill_history.size(), 2u);
  EXPECT_EQ(snap->kill_history[1].action, "release");
  EXPECT_EQ(snap->kill_history[1].previous, "COOLDOWN");
}

TEST(TradingKill, AnAccountResetThatAppliesPendingSettingsSaysWhen) {
  ScriptedMarket f;
  auto c = config();
  c.guardrails.max_opening_trades = 3;
  TradingSession s(c, f.time);
  f.seed(s);
  auto looser = c.guardrails;
  looser.max_opening_trades = 5;
  ASSERT_TRUE(s.set_guardrails(looser, f.time).decision.ok());
  ASSERT_TRUE(s.snapshot()->pending_guardrails);
  EXPECT_EQ(s.snapshot()->pending_applied_at, 0);
  f.next();
  ASSERT_TRUE(s.trip_kill("pause", f.time).decision.ok());
  ASSERT_TRUE(s.reset_account(m("100000"), {}, "fresh start", f.time).decision.ok());
  const auto snap = s.snapshot();
  EXPECT_FALSE(snap->pending_guardrails);
  EXPECT_EQ(s.config().guardrails.max_opening_trades, 5);
  EXPECT_EQ(snap->pending_applied_at, f.time);
  EXPECT_EQ(snap->kill_history.back().action, "reset");
  EXPECT_EQ(snap->kill_history.back().reason, "fresh start");
  EXPECT_EQ(snap->kill_history.back().previous, "pause");
}


TEST(TradingKill, AResetAfterTheDayEndedStartsTheNextDayWithoutItsLatches) {
  const auto at = [](md::Date day, int hour, int minute) { return md::new_york_to_utc(day, hour, minute); };
  const md::Date today{2026, 9, 22}, next{2026, 9, 23};
  for (const bool revised : {false, true}) {
    SCOPED_TRACE(revised ? "driver 11" : "driver 10");
    auto c = config();
    c.guardrails.max_opening_trades = 1;
    TradingSession s(c, at(today, 10, 0));
    s.set_revised_lifecycle(revised);
    ASSERT_TRUE(s.trade_stock("SPY", 10, at(today, 10, 0), StockPrice{"SPY", at(today, 10, 0), m("500")}).decision.ok());
    EXPECT_EQ(s.trade_stock("SPY", 1, at(today, 10, 1), StockPrice{"SPY", at(today, 10, 1), m("500")}).decision.code,
              Reason::TRADE_LIMIT);
    // Reset at 17:05, after the plan day ended at 17:00 and before its rollover ran.
    ASSERT_TRUE(s.reset_account(m("100000"), {}, "fresh", at(today, 17, 5)).decision.ok());
    const auto opened = s.trade_stock("SPY", 5, at(next, 9, 45), StockPrice{"SPY", at(next, 9, 45), m("500")});
    // Drivers 1-10 kept the finished day's latch through the whole next day.
    EXPECT_EQ(opened.decision.code, revised ? Reason::NONE : Reason::TRADE_LIMIT) << opened.decision.message;
  }
}
}  // namespace
}  // namespace openport::trading
