#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config() {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.rules.max_drawdown = m("1000");
  c.rules.buying_power = true;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.price_band_absolute = m("20");
  return c;
}
void update(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask, 100)}, {f.valuation()}, f.time);
}
void roll(TradingSession& s, ScriptedMarket& f) {
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ++f.observation;
  if (!s.contracts().contains(f.symbol())) s.define(f.contract, f.time);
  s.on_quotes({f.quote("4", "4.20", 100)}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
}
struct File {
  std::filesystem::path directory;
  std::string path;
  File() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-floor-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~File() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};


TEST(TradingFloor, FloorPercentRejects100OnWritesButRecoversStored100) {
  ScriptedMarket f;
  auto c = config();
  TradingSession s(c, f.time);
  for (const auto percent : {-1, 100, 101}) {
    auto g = c.guardrails; g.soft_floor_percent = percent;
    try {
      s.set_guardrails(g, f.time);
      FAIL() << "accepted " << percent;
    } catch (const TradingError& error) {
      EXPECT_EQ(error.code(), Reason::INVALID_LIMITS);
      if (percent == 100) { EXPECT_NE(std::string(error.what()).find("latches at once"), std::string::npos); }
    }
  }
  auto g = c.guardrails; g.soft_floor_percent = 99;
  ASSERT_TRUE(s.set_guardrails(g, f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  // Rewrite the checkpoint before hashing it, as a historical file with 100.
  File source, legacy;
  { TradingSession original(c, f.time, FileJournal::create(source.path)); }
  auto payload = Json::parse(FileJournal::read(source.path).records.front().payload);
  payload["state"]["config"]["guardrails"]["soft_floor_percent"] = 100;
  { auto out = FileJournal::create(legacy.path); out->append(f.time, "session_start", payload.dump()); }
  auto recovered = TradingSession::recover(FileJournal::read(legacy.path));
  EXPECT_EQ(recovered.config().guardrails.soft_floor_percent, 100);
  f.seed(recovered);
  EXPECT_EQ(recovered.snapshot()->risk.kill_reason, "SOFT_FLOOR");
  EXPECT_EQ(recovered.submit(f.market("blocked"), f.time).decision.code, Reason::SOFT_FLOOR);
}

TEST(TradingFloor, AnUnchangedSoftFloorDoesNotBlockOtherGuardrailEditsAfterGains) {
  ScriptedMarket f;
  auto c = config(); c.guardrails.soft_floor = m("9500");
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("winner", 5), f.time).decision.ok());
  update(s, f, "6.00", "6.20");
  ASSERT_TRUE(s.submit(f.market("close", 5, Side::Sell), f.time).decision.ok());
  ASSERT_GT(s.snapshot()->evaluation.floor, c.guardrails.soft_floor);
  EXPECT_TRUE(s.set_guardrails(c.guardrails, f.time).decision.ok());
  auto edited = c.guardrails; edited.max_opening_trades = 5;
  EXPECT_TRUE(s.set_guardrails(edited, f.time).decision.ok());
  EXPECT_EQ(s.config().guardrails.max_opening_trades, 5);
  EXPECT_EQ(s.config().guardrails.soft_floor, m("9500"));
  edited.soft_floor = m("9600");
  try {
    s.set_guardrails(edited, f.time);
    FAIL() << "accepted a new soft floor below the plan floor";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::INVALID_LIMITS);
  }
}

TEST(TradingFloor, PercentWithoutDrawdownWarnsAndAbsoluteFloorRechecksAtReset) {
  ScriptedMarket f;
  auto c = config(); c.rules = {}; c.guardrails.soft_floor_percent = 50;
  TradingSession s(c, f.time);
  const auto warnings = s.warnings();
  ASSERT_EQ(warnings.size(), 1U);
  EXPECT_EQ(warnings[0].code, "SOFT_FLOOR_UNUSED");
  EXPECT_EQ(warnings[0].severity, "info");
  EXPECT_FALSE(s.snapshot()->soft_floor);
  auto g = c.guardrails; g.soft_floor = m("9000");
  ASSERT_TRUE(s.set_guardrails(g, f.time).decision.ok());
  ASSERT_TRUE(s.reset_account(m("9000"), {}, "smaller", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "SOFT_FLOOR");
  const auto after = s.warnings();
  ASSERT_EQ(after.size(), 2U);
  EXPECT_EQ(after[0].code, "SOFT_FLOOR");
  EXPECT_NE(after[0].message.find("a reset applies pending settings"), std::string::npos);
  g.soft_floor = m("8000");
  ASSERT_TRUE(s.set_guardrails(g, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "SOFT_FLOOR");
  ASSERT_TRUE(s.reset_account(m("9000"), {}, "apply lower floor", f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.config().guardrails.soft_floor, m("8000"));
}

TEST(TradingFloor, ResetPreservesDailyLatchesAndCooldownThroughJournalRecovery) {
  for (const auto reason : {Reason::PROFIT_LOCK, Reason::TRADE_LIMIT, Reason::COOLDOWN}) {
    File file; ScriptedMarket f;
    std::string expected;
    Timestamp until = 0;
    {
      auto c = config(); c.fee_per_contract = {};
      if (reason == Reason::PROFIT_LOCK) c.guardrails.profit_lock = m("50");
      if (reason == Reason::TRADE_LIMIT) c.guardrails.max_opening_trades = 1;
      if (reason == Reason::COOLDOWN) { c.guardrails.cooldown_minutes = 30; c.guardrails.cooldown_loss = m("1"); }
      TradingSession s(c, f.time, FileJournal::create(file.path)); f.seed(s);
      ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
      if (reason == Reason::PROFIT_LOCK) update(s, f, "5", "5.20");
      if (reason == Reason::COOLDOWN) { ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok()); }
      ASSERT_EQ(s.snapshot()->risk.kill_reason, to_string(reason));
      until = s.snapshot()->guardrails.cooldown_until;
      // Pending disabling applies to the new attempt but must not bypass the latch.
      ASSERT_TRUE(s.set_guardrails({}, f.time).decision.ok());
      const auto changes = s.snapshot()->kill_history.size();
      ASSERT_TRUE(s.reset_account(m("10000"), c.rules, "another attempt", f.time).decision.ok());
      EXPECT_EQ(s.snapshot()->kill_history.size(), changes) << "the discipline latch was never cleared";
      EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
      EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, until);
      const auto refused = s.submit(f.market("blocked"), f.time).decision;
      EXPECT_EQ(refused.code, reason);
      EXPECT_EQ(s.snapshot()->kill_reset.code, reason);
      if (reason == Reason::TRADE_LIMIT) {
        EXPECT_EQ(refused.actual, 1);
        EXPECT_FALSE(refused.limit);
        EXPECT_NE(refused.message.find("remains latched after the account reset"), std::string::npos);
      }
      expected = s.snapshot_json();
    }
    auto recovered = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(recovered.snapshot_json(), expected);
    EXPECT_EQ(recovered.submit(f.market("still-blocked"), f.time).decision.code, reason);
    if (reason == Reason::COOLDOWN) {
      recovered.on_quotes({}, {}, until);
      EXPECT_FALSE(recovered.snapshot()->risk.kill_latched);
    } else {
      roll(recovered, f);
      EXPECT_FALSE(recovered.snapshot()->risk.kill_latched);
      EXPECT_EQ(recovered.snapshot()->guardrails.opening_trades, 0);
    }
  }
}

TEST(TradingFloor, AccountResetHistoryOnlyClearsReasonsItActuallyRemoves) {
  for (const bool manual : {false, true}) {
    File file; ScriptedMarket f;
    auto c = config(); c.guardrails.soft_floor = m("9990");
    std::string expected;
    {
      TradingSession s(c, f.time, FileJournal::create(file.path)); f.seed(s);
      ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
      ASSERT_EQ(s.snapshot()->risk.kill_reason, "SOFT_FLOOR");
      EXPECT_NE(s.snapshot()->kill_reset.message.find("account reset"), std::string::npos);
      auto g = c.guardrails; g.max_opening_trades = 1; g.soft_floor = {};
      ASSERT_TRUE(s.set_guardrails(g, f.time).decision.ok());
      if (manual) { ASSERT_TRUE(s.trip_kill("manual pause", f.time).decision.ok()); }
      const auto history_size = s.snapshot()->kill_history.size();
      ASSERT_TRUE(s.reset_account(m("10000"), c.rules, "fresh ledger", f.time).decision.ok());
      const auto snap = s.snapshot();
      EXPECT_EQ(snap->guardrails.latched, std::vector<Reason>{Reason::TRADE_LIMIT});
      EXPECT_EQ(snap->risk.kill_reason, "TRADE_LIMIT");
      EXPECT_EQ(snap->kill_reset.code, Reason::TRADE_LIMIT);
      ASSERT_EQ(snap->kill_history.size(), history_size + (manual ? 2U : 1U));
      if (manual) {
        EXPECT_EQ(snap->kill_history[history_size].action, "reset");
        EXPECT_EQ(snap->kill_history[history_size].previous, "manual pause");
      }
      EXPECT_EQ(snap->kill_history.back().action, "trip");
      EXPECT_EQ(snap->kill_history.back().previous, manual ? "" : "SOFT_FLOOR");
      EXPECT_EQ(snap->kill_history.back().reason, "TRADE_LIMIT");
      expected = s.snapshot_json();
    }
    auto recovered = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(recovered.snapshot_json(), expected);
  }
}

TEST(TradingFloor, PartialOpeningOrderFinishesAtLimitAcrossRecoveryButOtherOrdersCancel) {
  File file; ScriptedMarket f;
  OrderId id = 0;
  {
    auto c = config(); c.guardrails.max_opening_trades = 1;
    TradingSession s(c, f.time, FileJournal::create(file.path)); f.seed(s, "4", "4.20", 1);
    ASSERT_TRUE(s.submit(f.limit("other", 1, "3.50"), f.time).decision.ok());
    const auto first = s.submit(f.limit("parts", 3), f.time);
    ASSERT_TRUE(first.decision.ok()); id = *first.order_id;
    ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
    EXPECT_EQ(s.snapshot()->open_orders[0].id, id);
    EXPECT_EQ(s.snapshot()->open_orders[0].filled_quantity, 1);
    EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::TRADE_LIMIT);
    EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
    EXPECT_EQ(s.submit(f.market("new"), f.time).decision.code, Reason::TRADE_LIMIT);
  }
  auto s = TradingSession::recover(FileJournal::read(file.path));
  for (int n = 0; n < 2; ++n) {
    f.next(); s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
    EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  }
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 3);
  EXPECT_EQ(s.submit(f.market("new-after-fill"), f.time).decision.code, Reason::TRADE_LIMIT);
}

TEST(TradingFloor, AtomicComboCountsOnceAcrossPartialExecutions) {
  ScriptedMarket a, b; b.contract.strike += 10;
  auto c = config(); c.guardrails.max_opening_trades = 1;
  TradingSession s(c, a.time); a.seed(s, "4", "4.20", 1); b.seed(s, "4", "4.20", 1);
  auto r = a.limit("combo", 2, "0.20"); r.symbol.clear();
  r.legs = {{a.symbol(), Side::Buy, 1}, {b.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(r, a.time).decision.ok());
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  a.next(); b.next();
  s.on_quotes({a.quote(), b.quote()}, {a.valuation(), b.valuation()}, a.time);
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 4U);
}

TEST(TradingFloor, GtcOpeningCountsOnceAcrossRolloverAndKeepsPlanChecks) {
  ScriptedMarket f; auto c = config(); c.guardrails.max_opening_trades = 1;
  c.rules.max_contracts_held = 3;
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 1);
  auto request = f.limit("overnight", 3, "4.20", Side::Buy, TimeInForce::Gtc);
  const auto placed = s.submit(request, f.time);
  ASSERT_TRUE(placed.decision.ok());
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  // The trade-limit exemption must still honour the plan's held-contract cap.
  OrderChange change; change.quantity = 4;
  EXPECT_EQ(s.modify(*placed.order_id, change, f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 0);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  f.next(); s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 0);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  f.next(); s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 0);
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("today's order"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "TRADE_LIMIT");
}

TEST(TradingFloor, AnotherLatchStillCancelsTheOrderFinishingAtTradeLimit) {
  ScriptedMarket f; auto c = config(); c.guardrails.max_opening_trades = 1;
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 1);
  ASSERT_TRUE(s.submit(f.limit("parts", 3), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  ASSERT_TRUE(s.trip_kill("manual", f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
}

TEST(TradingFloor, TradeLimitCompletionCannotReopenAPassedAttempt) {
  ScriptedMarket f; auto c = config(); c.guardrails.max_opening_trades = 1;
  c.rules.profit_target = m("50");
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 1);
  ASSERT_TRUE(s.submit(f.limit("parts", 3), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
  update(s, f, "5", "5.20");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
  EXPECT_EQ(s.submit(f.market("new-attempt-needed"), f.time).decision.code, Reason::EVALUATION_CLOSED);
}

TEST(TradingFloor, StandaloneStopsTrailingAndOcoStopsStartCooldownOnFill) {
  for (const auto kind : {"option", "underlying", "limit", "trailing", "trailing-up", "oco", "target"}) {
    SCOPED_TRACE(kind);
    ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 5;
    // Stops start it even if the optional loss threshold is not reached.
    c.guardrails.cooldown_loss = m("10000");
    TradingSession s(c, f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
    auto close = f.market("close", 1, Side::Sell);
    close.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.90")};
    const std::string type = kind;
    if (type == "underlying") close.trigger = Trigger{TriggerSource::Underlying, TriggerDirection::AtOrBelow, m("4990")};
    if (type == "limit") { close.type = OrderType::Limit; close.tif = TimeInForce::Gtc; close.limit_price = m("3.80"); }
    if (type == "trailing") close.trigger->trail = Trail{TrailUnit::Amount, m("0.10")};
    if (type == "trailing-up") {
      close.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("5")};
      close.trigger->trail = Trail{TrailUnit::Amount, m("0.10")};
    }
    if (type == "oco" || type == "target") {
      close.type = OrderType::Limit; close.tif = TimeInForce::Gtc; close.limit_price = m("5"); close.trigger.reset(); close.exits_only = true;
      close.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.90")}, {}},
                              ExitSpec{{}, m("5")}};
    }
    ASSERT_TRUE(s.submit(close, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, 0);
    if (type == "limit") {
      f.next(); s.on_quotes({f.quote("3.60", "3.80")}, {f.valuation()}, f.time);
      ASSERT_FALSE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, 0) << "triggering without a fill starts no cooldown";
    }
    f.next(); auto v = f.valuation(); if (type == "underlying") v.spot = 4980;
    const bool higher = type == "target" || type == "trailing-up";
    s.on_quotes({f.quote(higher ? "5" : "3.80", higher ? "5.20" : "4")}, {v}, f.time);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, type == "target" ? 0 : f.time + 5 * md::kNanosPerMinute);
  }
}

TEST(TradingFloor, OwnPriceTriggeredClosesStartCooldownOnlyInTheAdverseDirection) {
  for (const auto side : {Side::Buy, Side::Sell}) for (const bool stop : {false, true}) {
    SCOPED_TRACE(side == Side::Buy ? "long" : "short");
    SCOPED_TRACE(stop);
    ScriptedMarket f; auto c = config(); c.rules.buying_power = false;
    c.guardrails.cooldown_minutes = 5; c.guardrails.cooldown_loss = m("10000");
    TradingSession s(c, f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("entry", 1, side), f.time).decision.ok());
    const bool below = stop == (side == Side::Buy);
    auto close = f.market("close", 1, side == Side::Buy ? Side::Sell : Side::Buy);
    close.trigger = Trigger{TriggerSource::Option, below ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove,
                            m(below ? "3.80" : "5")};
    ASSERT_TRUE(s.submit(close, f.time).decision.ok());
    ASSERT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Armed);
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, 0);
    update(s, f, below ? "3.60" : "5", below ? "3.80" : "5.20");
    ASSERT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->recent_orders.back().triggered_at, f.time);
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, stop ? f.time + 5 * md::kNanosPerMinute : 0);
    EXPECT_EQ(s.submit(f.market("again"), f.time).decision.code, stop ? Reason::COOLDOWN : Reason::NONE);
  }
}

TEST(TradingFloor, LinkedOcoStopStartsCooldownButItsProfitSideDoesNot) {
  for (const bool stopped : {false, true}) {
    ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 5;
    c.rules.buying_power = false;  // General OCO legs reserve independently on this plan.
    TradingSession s(c, f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
    auto stop = f.market("oco", 1, Side::Sell);
    stop.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.90")};
    auto target = f.limit("", 1, "5", Side::Sell, TimeInForce::Gtc);
    stop.oco = {target};
    ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, 0);
    update(s, f, stopped ? "3.80" : "5", stopped ? "4" : "5.20");
    ASSERT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, stopped ? f.time + 5 * md::kNanosPerMinute : 0);
    EXPECT_TRUE(s.snapshot()->open_orders.empty());
  }
}

TEST(TradingFloor, RequiredBracketStopStartsCooldownWithTradeRiskLimit) {
  ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 5;
  c.rules.require_stop_loss = true;
  c.rules.max_trade_risk = m("100");
  TradingSession s(c, f.time); f.seed(s);
  auto request = f.market("entry");
  request.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.90")}, {}}, {}};
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  update(s, f, "3.80", "4");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, f.time + 5 * md::kNanosPerMinute);
}

TEST(TradingFloor, StandaloneStopAfterPlanDecisionStillStartsCooldown) {
  ScriptedMarket f; auto c = config();
  c.guardrails.cooldown_minutes = 5; c.rules.profit_target = m("50");
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("entry", 2), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("5", "5.20", 1)}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  ASSERT_EQ(s.snapshot()->positions[0].position.quantity, 1);
  // F33 permits a reducing order after the decision; it reserves the remaining
  // close against the system's liquidation retry on the next quote.
  auto stop = f.market("close-after-pass", 1, Side::Sell);
  stop.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("4.90")};
  ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
  update(s, f, "3.80", "4");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, f.time + 5 * md::kNanosPerMinute);
  ASSERT_TRUE(s.reset_account(c.initial_cash, c.rules, "next attempt", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
}

TEST(TradingFloor, UnderlyingTriggeredClosesUseTheHeldOptionsDirectionForCooldown) {
  for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put})
    for (const auto side : {Side::Buy, Side::Sell}) for (const bool stop : {false, true}) {
      SCOPED_TRACE(type == pricing::OptionType::Call ? "call" : "put");
      SCOPED_TRACE(side == Side::Buy ? "long" : "short");
      SCOPED_TRACE(stop);
      ScriptedMarket f; f.contract.type = type;
      auto c = config(); c.rules.buying_power = false;
      c.guardrails.cooldown_minutes = 5; c.guardrails.cooldown_loss = m("10000");
      TradingSession s(c, f.time); s.define(f.contract, f.time);
      auto v = f.valuation(type == pricing::OptionType::Call ? 0.5 : -0.5);
      s.on_quotes({f.quote()}, {v}, f.time);
      ASSERT_TRUE(s.submit(f.market("entry", 1, side), f.time).decision.ok());
      // Long calls and short puts lose on a fall; long puts and short calls on a rise.
      const bool loses_on_fall = (side == Side::Buy) == (type == pricing::OptionType::Call);
      const bool below = stop == loses_on_fall;
      auto close = f.market("close", 1, side == Side::Buy ? Side::Sell : Side::Buy);
      close.trigger = Trigger{TriggerSource::Underlying, below ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove,
                              m(below ? "4990" : "5010")};
      ASSERT_TRUE(s.submit(close, f.time).decision.ok());
      ASSERT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Armed);
      EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, 0);
      f.next(); v.time = f.time; v.spot = below ? 4990 : 5010;
      const bool cheaper = stop == (side == Side::Buy);
      s.on_quotes({f.quote(cheaper ? "3.60" : "5", cheaper ? "3.80" : "5.20")}, {v}, f.time);
      ASSERT_TRUE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->recent_orders.back().triggered_at, f.time);
      EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, stop ? f.time + 5 * md::kNanosPerMinute : 0);
      EXPECT_EQ(s.submit(f.market("again"), f.time).decision.code, stop ? Reason::COOLDOWN : Reason::NONE);
    }
}

TEST(TradingFloor, ComboTriggeredClosesStartCooldownOnlyInTheAdverseDirection) {
  for (const auto source : {TriggerSource::Combo, TriggerSource::Underlying})
    for (const bool long_delta : {false, true}) for (const bool stop : {false, true}) {
    SCOPED_TRACE(source == TriggerSource::Combo ? "combo" : "underlying");
    SCOPED_TRACE(stop);
    ScriptedMarket a, b; b.contract.strike += 10;
    auto c = config(); c.guardrails.cooldown_minutes = 5; c.guardrails.cooldown_loss = m("10000");
    TradingSession s(c, a.time);
    s.define(a.contract, a.time); s.define(b.contract, b.time);
    s.on_quotes({a.quote("5", "5.20"), b.quote()}, {a.valuation(0.6), b.valuation(0.4)}, a.time);
    auto entry = a.market("entry"); entry.symbol.clear();
    entry.legs = {{a.symbol(), long_delta ? Side::Buy : Side::Sell, 1}, {b.symbol(), long_delta ? Side::Sell : Side::Buy, 1}};
    ASSERT_TRUE(s.submit(entry, a.time).decision.ok());
    auto close = a.market("close"); close.symbol.clear();
    close.legs = {{a.symbol(), long_delta ? Side::Sell : Side::Buy, 1}, {b.symbol(), long_delta ? Side::Buy : Side::Sell, 1}};
    // Combos read their signed closing debit: a rise loses even when the debit is negative.
    const bool cheaper = stop == long_delta;
    const bool below = source == TriggerSource::Combo ? !stop : cheaper;
    const auto debit_level = long_delta ? (stop ? "-0.20" : "-1.80") : (stop ? "1.80" : "0.60");
    close.trigger = Trigger{source, below ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove,
                            m(source == TriggerSource::Combo ? debit_level : (cheaper ? "4990" : "5010"))};
    ASSERT_TRUE(s.submit(close, a.time).decision.ok());
    ASSERT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Armed);
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, 0);
    a.next(); b.next(); auto av = a.valuation(0.6), bv = b.valuation(0.4);
    av.spot = bv.spot = cheaper ? 4990 : 5010;
    s.on_quotes({a.quote(cheaper ? "4.40" : "6", cheaper ? "4.60" : "6.20"), b.quote()}, {av, bv}, a.time);
    ASSERT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->recent_orders.back().triggered_at, a.time);
    EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, stop ? a.time + 5 * md::kNanosPerMinute : 0);
    EXPECT_EQ(s.submit(a.market("again"), a.time).decision.code, stop ? Reason::COOLDOWN : Reason::NONE);
  }
}

TEST(TradingFloor, EvaluationLimitsTightenNowLoosenAtRollIncludingMixedEdits) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  auto limits = s.config().limits;
  limits.max_order_contracts = 5;
  limits.max_daily_loss = m("100");
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.config().limits.max_order_contracts, 5);
  EXPECT_FALSE(s.snapshot()->pending_limits);
  limits.max_order_contracts = 20;
  limits.max_daily_loss = m("50");
  limits.price_band_relative = 0.5;
  s.set_limits(limits, f.time);
  EXPECT_EQ(s.config().limits.max_order_contracts, 5);
  EXPECT_EQ(s.config().limits.max_daily_loss, m("50"));
  ASSERT_TRUE(s.snapshot()->pending_limits);
  EXPECT_EQ(s.snapshot()->pending_limits->max_order_contracts, 20);
  EXPECT_EQ(s.submit(f.market("too-big", 6), f.time).decision.code, Reason::MAX_ORDER_CONTRACTS);
  roll(s, f);
  EXPECT_EQ(s.config().limits.max_order_contracts, 20);
  EXPECT_DOUBLE_EQ(s.config().limits.price_band_relative, 0.5);
  EXPECT_FALSE(s.snapshot()->pending_limits);
  EXPECT_EQ(s.snapshot()->pending_applied_at, f.time);
}
TEST(TradingFloor, PracticeLimitsAreImmediateAndOverridesCannotBeLoosenedByRemoval) {
  ScriptedMarket f;
  TradingSession practice({}, f.time);
  auto limits = practice.config().limits;
  limits.max_order_contracts = 200;
  practice.set_limits(limits, f.time);
  EXPECT_EQ(practice.config().limits.max_order_contracts, 200);
  EXPECT_FALSE(practice.snapshot()->pending_limits);
  auto c = config();
  c.limits.underlying_overrides["SPX"] = {100, 50};
  TradingSession evaluation(c, f.time);
  auto changed = c.limits;
  changed.underlying_overrides.clear();
  changed.aggregate.dollar_delta = 2e9;
  evaluation.set_limits(changed, f.time);
  EXPECT_DOUBLE_EQ(evaluation.config().limits.underlying_overrides.at("SPX").dollar_delta, 100);
  ASSERT_TRUE(evaluation.snapshot()->pending_limits);
}
TEST(TradingFloor, OverrideChangesAndRolloverRecoverWithBothRemovalDirections) {
  File file; ScriptedMarket f;
  auto c = config();
  c.limits.per_underlying = {1000, 100};
  c.limits.underlying_overrides["SPX"] = {2000, 200};
  c.limits.underlying_overrides["QQQ"] = {500, 50};
  std::string expected;
  {
    TradingSession s(c, f.time, FileJournal::create(file.path)); f.seed(s);
    auto limits = c.limits; limits.underlying_overrides.clear();
    limits.underlying_overrides["IWM"] = {2000, 50};
    ASSERT_TRUE(s.set_limits(limits, f.time).decision.ok());
    EXPECT_FALSE(s.config().limits.underlying_overrides.contains("SPX"));
    EXPECT_EQ(s.config().limits.underlying_overrides.at("QQQ").dollar_delta, 500);
    EXPECT_EQ(s.config().limits.underlying_overrides.at("IWM").dollar_delta, 1000);
    EXPECT_EQ(s.config().limits.underlying_overrides.at("IWM").vega, 50);
    ASSERT_TRUE(s.snapshot()->pending_limits);
    expected = s.snapshot_json();
  }
  {
    auto restored = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
    EXPECT_EQ(restored.snapshot_json(), expected);
    roll(restored, f);
    EXPECT_FALSE(restored.snapshot()->pending_limits);
    EXPECT_FALSE(restored.config().limits.underlying_overrides.contains("QQQ"));
    EXPECT_EQ(restored.config().limits.underlying_overrides.at("IWM").dollar_delta, 2000);
    expected = restored.snapshot_json();
  }
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), expected);
  TradingSession tightening(c, f.time);
  auto limits = c.limits; limits.underlying_overrides.erase("SPX");
  tightening.set_limits(limits, f.time);
  EXPECT_FALSE(tightening.snapshot()->pending_limits);
  EXPECT_FALSE(tightening.config().limits.underlying_overrides.contains("SPX"));
}
TEST(TradingFloor, AttemptResetAppliesPendingAndKeepsDailyDiscipline) {
  ScriptedMarket f;
  auto c = config();
  c.guardrails.max_opening_trades = 1;
  TradingSession s(c, f.time);
  f.seed(s);
  s.submit(f.market("entry"), f.time);
  s.set_guardrails({}, f.time);
  auto limits = s.config().limits;
  limits.max_order_contracts = 200;
  s.set_limits(limits, f.time);
  s.reset_account(m("10000"), c.rules, "new attempt", f.time);
  EXPECT_EQ(s.snapshot()->evaluation.attempt, 2);
  EXPECT_EQ(s.config().limits.max_order_contracts, 200);
  EXPECT_EQ(s.config().guardrails.max_opening_trades, 0);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 1);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.submit(f.market("still-blocked"), f.time).decision.code, Reason::TRADE_LIMIT);
  EXPECT_FALSE(s.snapshot()->pending_guardrails);
}
TEST(TradingFloor, TradeLimitCountsOpeningExecutionsAndKeepsClosingOrdersAndFlatten) {
  ScriptedMarket f;
  auto c = config(); c.guardrails.max_opening_trades = 2;
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("first"), f.time);
  ASSERT_FALSE(s.snapshot()->risk.kill_latched);
  s.submit(f.limit("rest", 1, "3.50"), f.time);
  s.submit(f.market("second"), f.time);
  ASSERT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 2);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(s.submit(f.market("third"), f.time).decision.code, Reason::TRADE_LIMIT);
  EXPECT_EQ(s.reset_kill("bypass", f.time).decision.code, Reason::TRADE_LIMIT);
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  roll(s, f);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 0);
  EXPECT_TRUE(s.submit(f.market("next-day"), f.time).decision.ok());
}
TEST(TradingFloor, SoftFloorTouchFlattensAndLatchesWithoutFailingAttempt) {
  for (const bool percent : {false, true}) {
    ScriptedMarket f;
    auto c = config(); c.fee_per_contract = {};
    if (percent) c.guardrails.soft_floor_percent = 99; else c.guardrails.soft_floor = m("9990");
    TradingSession s(c, f.time); f.seed(s);
    s.submit(f.market("entry"), f.time);
    const auto snapshot = s.snapshot();
    EXPECT_TRUE(snapshot->positions.empty());
    EXPECT_EQ(snapshot->recent_fills.size(), 2U);
    EXPECT_EQ(snapshot->risk.kill_reason, "SOFT_FLOOR");
    EXPECT_EQ(snapshot->evaluation.status, EvaluationStatus::Active);
    EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::SOFT_FLOOR);
    EXPECT_EQ(s.reset_kill("cannot", f.time).decision.code, Reason::SOFT_FLOOR);
    roll(s, f);
    // The configured absolute soft floor is still above equity, so it trips again;
    // the percentage floor also follows the plan, not a freshly invented balance.
    EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  }
}
TEST(TradingFloor, SoftFloorRetriesUnfilledLiquidation) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  s.submit(f.market("entry", 3), f.time);
  Guardrails g; g.soft_floor = m("9990");
  f.next(); s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
  s.set_guardrails(g, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 2);
  update(s, f, "4", "4.20");
  EXPECT_TRUE(s.snapshot()->positions.empty());
}
TEST(TradingFloor, ProfitLockLeavesPositionsAndExitsWorkingUntilNextDay) {
  ScriptedMarket f; auto c = config(); c.guardrails.profit_lock = m("20");
  TradingSession s(c, f.time); f.seed(s);
  auto order = f.market("entry");
  order.bracket = Bracket{{}, ExitSpec{{}, m("5")}};
  s.submit(order, f.time);
  update(s, f, "4.50", "4.70");
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "PROFIT_LOCK");
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->open_orders.size(), 1U);
  update(s, f, "5", "5.20");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.submit(f.market("again"), f.time).decision.code, Reason::PROFIT_LOCK);
  roll(s, f);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingFloor, GuardrailCancelsCarryTheCodeNewOrdersAreRefusedWith) {
  // B35: a latch cancelled working opening orders with KILL_SWITCH, a switch the trader
  // never used, while it refused new ones with its own code.
  for (const bool trade_limit : {false, true}) {
    ScriptedMarket f; auto c = config();
    if (trade_limit) c.guardrails.max_opening_trades = 1; else c.guardrails.profit_lock = m("20");
    TradingSession s(c, f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("rest", 1, "3.50"), f.time).decision.ok());
    s.submit(f.market("entry"), f.time);
    if (!trade_limit) update(s, f, "4.50", "4.70");
    const auto cancelled = s.snapshot()->recent_orders[0];
    ASSERT_EQ(cancelled.status, OrderStatus::Cancelled);
    const auto refused = s.submit(f.market("again"), f.time).decision;
    EXPECT_EQ(refused.code, trade_limit ? Reason::TRADE_LIMIT : Reason::PROFIT_LOCK);
    EXPECT_EQ(cancelled.reason.code, refused.code);
    EXPECT_EQ(cancelled.reason.message, refused.message);
  }
  // The trader's own trip still cancels with KILL_SWITCH.
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "3.50"), f.time).decision.ok());
  ASSERT_TRUE(s.trip_kill("stop for the day", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders[0].reason.code, Reason::KILL_SWITCH);
}
TEST(TradingFloor, APlanDecisionLabelsItsLiquidationEvenWhenTheSoftFloorLatchesWithIt) {
  // D23: a soft floor that latched in the batch that failed the plan labelled the
  // plan's liquidation system:soft_floor:N.
  ScriptedMarket f; auto c = config(); c.guardrails.soft_floor = m("9500");
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("entry", 5), f.time).decision.ok());
  update(s, f, "2.00", "2.20");
  auto snapshot = s.snapshot();
  EXPECT_EQ(snapshot->evaluation.status, EvaluationStatus::Failed);
  EXPECT_EQ(snapshot->risk.kill_reason, "SOFT_FLOOR");
  EXPECT_TRUE(snapshot->recent_orders.back().request.client_order_id.starts_with("system:drawdown:"));
  EXPECT_TRUE(snapshot->positions.empty());
  // While the attempt is active, the soft floor's own label remains.
  ScriptedMarket g; auto soft = config(); soft.guardrails.soft_floor = m("9990");
  TradingSession t(soft, g.time); g.seed(t);
  t.submit(g.market("entry"), g.time);
  EXPECT_TRUE(t.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:soft_floor:"));
}
TEST(TradingFloor, CooldownAfterStopUsesOnlyMarketTimeAndAllowsCloses) {
  ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 5;
  TradingSession s(c, f.time); f.seed(s);
  auto order = f.market("entry", 2);
  order.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.90")}, {}}, {}};
  s.submit(order, f.time);
  f.next(); s.on_quotes({f.quote("3.80", "4", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, f.time + 5 * md::kNanosPerMinute);
  EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::COOLDOWN);
  EXPECT_EQ(s.reset_kill("blocked", f.time).decision.code, Reason::COOLDOWN);
  update(s, f, "4", "4.20");
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
  auto until = s.snapshot()->guardrails.cooldown_until;
  s.on_quotes({}, {}, until - 1);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  s.on_quotes({}, {}, until);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);
}
TEST(TradingFloor, CooldownOnLosingCloseAndManualKillSurvivesExpiryAndRoll) {
  ScriptedMarket f; auto c = config(); c.guardrails.cooldown_minutes = 1; c.guardrails.cooldown_loss = m("10");
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("open"), f.time);
  s.submit(f.market("close", 1, Side::Sell), f.time);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  s.trip_kill("manual pause", f.time);
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "manual pause");
  roll(s, f);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_TRUE(s.reset_kill("ready", f.time).decision.ok());
}
TEST(TradingFloor, GuardrailsAreTightenOnlyEvenInPractice) {
  ScriptedMarket f; TradingSession s({}, f.time);
  Guardrails g; g.max_opening_trades = 10; g.cooldown_minutes = 5; g.cooldown_loss = m("100"); g.profit_lock = m("500");
  s.set_guardrails(g, f.time);
  g.max_opening_trades = 5; g.cooldown_loss = m("50"); g.cooldown_minutes = 10; g.profit_lock = m("200");
  s.set_guardrails(g, f.time);
  EXPECT_EQ(s.config().guardrails, g);
  EXPECT_FALSE(s.snapshot()->pending_guardrails);
  s.set_guardrails({}, f.time);
  EXPECT_EQ(s.config().guardrails, g);
  ASSERT_TRUE(s.snapshot()->pending_guardrails);
  roll(s, f);
  EXPECT_EQ(s.config().guardrails, Guardrails{});
}
TEST(TradingFloor, CooldownKeepsItsExpiryAcrossRollAndTighteningExtendsIt) {
  ScriptedMarket f; auto c = config();
  c.guardrails.cooldown_minutes = 1440; c.guardrails.cooldown_loss = m("10");
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("open"), f.time);
  s.submit(f.market("close", 1, Side::Sell), f.time);
  const auto until = s.snapshot()->guardrails.cooldown_until;
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->time, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, until);
  const auto next_day = md::new_york_to_utc({2026, 9, 23}, 9, 30);
  s.roll_day(next_day);
  EXPECT_EQ(s.snapshot()->guardrails.cooldown_until, until);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  EXPECT_EQ(s.submit(f.market("blocked"), next_day).decision.code, Reason::COOLDOWN);
  s.on_quotes({}, {}, until);
  EXPECT_FALSE(s.snapshot()->risk.kill_latched);

  // Start a separate, five-minute cooldown, then extend its original expiry.
  c.guardrails.cooldown_minutes = 5;
  TradingSession tightened(c, f.time); f.seed(tightened);
  tightened.submit(f.market("entry"), f.time);
  tightened.submit(f.market("exit", 1, Side::Sell), f.time);
  c.guardrails.cooldown_minutes = 10;
  tightened.set_guardrails(c.guardrails, f.time + md::kNanosPerMinute);
  EXPECT_EQ(tightened.snapshot()->guardrails.cooldown_until, f.time + 10 * md::kNanosPerMinute);
}
TEST(TradingFloor, DailyGuardrailKeepsTheSharedLatchAfterCooldownExpires) {
  ScriptedMarket f; auto c = config();
  c.guardrails.cooldown_minutes = 1; c.guardrails.cooldown_loss = m("10");
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("entry"), f.time); s.submit(f.market("exit", 1, Side::Sell), f.time);
  ASSERT_EQ(s.snapshot()->risk.kill_reason, "COOLDOWN");
  auto guardrails = c.guardrails; guardrails.max_opening_trades = 1;
  s.set_guardrails(guardrails, f.time);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "TRADE_LIMIT");
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  EXPECT_TRUE(s.snapshot()->risk.kill_latched);
  EXPECT_EQ(s.snapshot()->risk.kill_reason, "TRADE_LIMIT");
  EXPECT_EQ(s.reset_kill("cannot bypass", f.time + md::kNanosPerMinute).decision.code, Reason::TRADE_LIMIT);
}
TEST(TradingFloor, DailyExtremesAndClosestFloorRecordFirstTouchTimes) {
  ScriptedMarket f; auto c = config(); c.rules.drawdown_mode = DrawdownMode::EndOfDay;
  TradingSession s(c, f.time); f.seed(s);
  s.submit(f.market("open"), f.time);
  const auto low = s.snapshot()->equity;
  const auto low_at = f.time;
  update(s, f, "5", "5.20");
  const auto high = s.snapshot()->equity;
  const auto high_at = f.time;
  s.submit(f.market("close", 1, Side::Sell), f.time);
  roll(s, f);
  const auto& day = s.snapshot()->evaluation.days.front();
  EXPECT_EQ(day.low_equity, low);
  EXPECT_EQ(day.low_at, low_at);
  EXPECT_EQ(day.high_equity, high);
  EXPECT_EQ(day.high_at, high_at);
  EXPECT_EQ(s.snapshot()->evaluation.closest_floor, low - m("9000"));
  EXPECT_EQ(s.snapshot()->evaluation.closest_floor_at, low_at);
}
TEST(TradingFloor, PendingAndGuardrailStateSurviveJournalRecoveryAndOlderRecordsLoad) {
  File file; ScriptedMarket f;
  std::string expected;
  {
    auto c = config(); c.guardrails.max_opening_trades = 1;
    TradingSession s(c, f.time, FileJournal::create(file.path)); f.seed(s);
    s.submit(f.market("open"), f.time);
    s.set_guardrails({}, f.time);
    auto limits = s.config().limits; limits.max_order_contracts = 200; s.set_limits(limits, f.time);
    expected = s.snapshot_json();
  }
  auto restored = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(restored.snapshot_json(), expected);
  EXPECT_EQ(restored.submit(f.market("blocked"), f.time).decision.code, Reason::TRADE_LIMIT);
  // The historical fixture predates every field introduced by this feature.
  const auto old = TradingSession::recover(FileJournal::read(std::string(OPENPORT_TEST_DATA_DIR) + "/kill-before-reduce-only.jsonl"));
  EXPECT_EQ(old.config().guardrails, Guardrails{});
  EXPECT_FALSE(old.snapshot()->pending_limits);
  EXPECT_FALSE(old.snapshot()->pending_guardrails);
  EXPECT_FALSE(old.snapshot()->evaluation.day_low_equity);
  EXPECT_FALSE(old.snapshot()->evaluation.closest_floor);
  EXPECT_EQ(old.snapshot()->guardrails.opening_trades, 0);
  EXPECT_EQ(old.snapshot()->guardrails.trade_limit_order, 0U);
  for (const auto& order : old.snapshot()->recent_orders) { EXPECT_FALSE(order.opening_counted); }
  EvaluationDay day = Json{{"day", md::Date{2026, 9, 22}}, {"open_equity", 1000}, {"close_equity", 1010}, {"peak", 1010}, {"floor", 900}}.get<EvaluationDay>();
  EXPECT_FALSE(day.low_equity);
  EXPECT_EQ(day.high_at, 0);
}
TEST(TradingPreview, PureChecksMatchSubmitAndDoNotConsumeIdsJournalOrLiquidity) {
  File file; ScriptedMarket f;
  auto sink = FileJournal::create(file.path);
  TradingSession s(config(), f.time, sink); f.seed(s, "4", "4.20", 1);
  const auto snapshot = s.snapshot_json(), head = sink->head();
  const auto sequence = sink->sequence();
  auto order = f.market("entry");
  const auto p = s.preview(order, f.time);
  EXPECT_TRUE(p.decision.ok());
  EXPECT_EQ(p.max_loss, m("420.65"));
  EXPECT_EQ(p.max_loss_basis, "expiry_payoff");
  EXPECT_EQ(p.max_units, 1);  // half of $1000 room, independently of displayed size
  EXPECT_EQ(p.buying_power_after, m("9579.35"));
  EXPECT_EQ(p.exposure_change->dollar_delta, 250000);
  EXPECT_EQ(s.snapshot_json(), snapshot);
  EXPECT_EQ(sink->head(), head);
  EXPECT_EQ(sink->sequence(), sequence);
  const auto submitted = s.submit(order, f.time);
  EXPECT_EQ(submitted.decision.code, p.decision.code);
  EXPECT_EQ(submitted.order_id, 1);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1U);
  const auto retry = s.preview(order, f.time - 1);
  EXPECT_EQ(retry.decision.code, submitted.decision.code);
  EXPECT_EQ(retry.buying_power_after, retry.buying_power_before);
  EXPECT_EQ(retry.buying_power_required, Money{});
  EXPECT_EQ(retry.exposure_change->dollar_delta, 0);
  EXPECT_FALSE(retry.max_units);
}
TEST(TradingPreview, SameRejectionsAndSizeHonoursPowerLimitsAndRoom) {
  ScriptedMarket f; auto c = config(); c.limits.max_order_contracts = 3;
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 1);
  for (int i = 0; i < 3; ++i) {
    auto request = f.market("reject-" + std::to_string(i), i == 0 ? 4 : 1);
    if (i == 1) request.symbol = "unknown";
    if (i == 2) s.trip_kill("pause", f.time);
    const auto preview = s.preview(request, f.time);
    EXPECT_EQ(preview.decision.code, s.submit(request, f.time).decision.code);
  }
  s.reset_kill("resume", f.time);
  EXPECT_EQ(s.preview(f.market("size", 1), f.time, 1).max_units, 2);
  EXPECT_THROW((void)s.preview(f.market("invalid"), f.time, 0), TradingError);
  c.rules.max_drawdown = {}; c.initial_cash = m("500");
  TradingSession small(c, f.time); f.seed(small);
  EXPECT_EQ(small.preview(f.market("small"), f.time).max_units, 1);
}
TEST(TradingPreview, ExactVerticalAndUnboundedScenarioLossAndFloorWarnings) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  OrderRequest spread = f.market("spread", 2);
  spread.symbol.clear(); spread.legs = {{f.symbol(), Side::Buy, 1}, {wing.symbol(), Side::Sell, 1}};
  const auto p = s.preview(spread, f.time);
  EXPECT_EQ(p.max_loss, m("442.60"));
  EXPECT_EQ(p.max_units, 2);
  EXPECT_FALSE(*p.breaches_floor);
  auto credit = spread; credit.client_order_id = "credit";
  credit.legs[0].side = Side::Sell; credit.legs[1].side = Side::Buy;
  EXPECT_EQ(s.preview(credit, f.time).max_loss, m("1642.60"));
  auto short_call = f.market("naked", 1, Side::Sell);
  const auto naked = s.preview(short_call, f.time);
  EXPECT_EQ(naked.max_loss_basis, "scenario_grid");
  ASSERT_TRUE(naked.max_loss);
  EXPECT_GT(*naked.max_loss, Money{});
  EXPECT_TRUE(*naked.breaches_floor);
  Guardrails g; g.soft_floor = m("9700"); s.set_guardrails(g, f.time);
  EXPECT_TRUE(*s.preview(f.market("long"), f.time).breaches_soft_floor);
  EXPECT_EQ(s.preview(f.market("long"), f.time).max_units, 0);
}
TEST(TradingPreview, SizeCanHedgeAnExistingBookEvenWhenOneUnitDoesNotFitTheFloor) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s, "4", "4.20", 100);
  ASSERT_TRUE(s.submit(f.market("ten-calls", 10), f.time).decision.ok());
  const auto room = s.snapshot()->equity - s.snapshot()->evaluation.floor;
  const auto request = f.market("hedge", 1, Side::Sell);
  const auto preview = s.preview(request, f.time);
  ASSERT_TRUE(preview.max_loss);
  EXPECT_GT(*preview.max_loss, room.prorate(1, 2));
  Quantity expected = 0;
  for (Quantity size = 1; size <= 12; ++size) {
    auto sized = request; sized.quantity = size;
    const auto candidate = s.preview(sized, f.time);
    if (candidate.decision.ok() && candidate.buying_power_after && *candidate.buying_power_after >= Money{} &&
        candidate.max_loss && *candidate.max_loss <= room.prorate(1, 2)) expected = size;
  }
  EXPECT_GT(expected, 1);
  EXPECT_EQ(preview.max_units, expected);
}
TEST(TradingPreview, SizeCanRestoreBuyingPowerWhenAPartialCloseLeavesADeficit) {
  ScriptedMarket f; auto c = config(); c.rules.buying_power = false;
  TradingSession s(c, f.time); f.seed(s, "4", "4.20", 100);
  ASSERT_TRUE(s.submit(f.market("shorts", 2, Side::Sell), f.time).decision.ok());
  const auto preview = s.preview(f.market("close", 1), f.time, 1);
  ASSERT_TRUE(preview.buying_power_after);
  EXPECT_LT(*preview.buying_power_after, Money{});
  EXPECT_EQ(preview.max_units, 2);
  const auto sized = s.preview(f.market("close", *preview.max_units), f.time, 1);
  EXPECT_GE(*sized.buying_power_after, Money{});
  EXPECT_FALSE(*sized.breaches_floor);
}
TEST(TradingPreview, ImpactBlocksPriceTheProjectionAndTheSize) {
  // An ask of 1.05 for 8 contracts; each further block of 8 costs one tick (0.05) more.
  ScriptedMarket f; auto c = config();
  c.initial_cash = m("100000"); c.rules.max_drawdown = m("6000"); c.rules.impact_ticks = 1;
  TradingSession s(c, f.time); f.seed(s, "1.00", "1.05", 8);
  // 26 contracts cost 8 x 105 + 8 x 110 + 8 x 115 + 2 x 120 + 26 x 0.65 = 2,896.90;
  // 27 would cost 3,017.55, more than half the 6,000 room.
  EXPECT_EQ(s.preview(f.market("size"), f.time).max_units, 26);
  const auto sized = s.preview(f.market("26", 26), f.time);
  EXPECT_EQ(sized.max_loss, m("2896.90"));
  EXPECT_EQ(sized.buying_power_required, m("2896.90"));
  EXPECT_EQ(sized.buying_power_after, m("100000") - m("2896.90"));
  // The fill walks the same blocks.
  ASSERT_TRUE(s.submit(f.market("26", 26), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->account.cash, m("100000") - m("2896.90"));
}
TEST(TradingPreview, ABracketEntryCountsItsExitsFeeReservation) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  auto entry = f.market("bracket");
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3")}, {}},
                          ExitSpec{{}, m("6")}};
  // 10,000 - 420 premium - 0.65 fee - 0.65 held for the exit pair.
  const auto preview = s.preview(entry, f.time);
  EXPECT_EQ(preview.buying_power_after, m("9578.70"));
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, *preview.buying_power_after);
}
TEST(TradingPreview, SellingHeldLongsIsMeasuredOnTheAccountNotAsNewShorts) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW261022P05000000");
  TradingSession s(config(), f.time);
  s.define(f.contract, f.time);
  s.on_quotes({f.quote()}, {f.valuation(-0.5)}, f.time);
  ASSERT_TRUE(s.submit(f.market("longs", 2), f.time).decision.ok());
  // Selling both puts at 4.00 against a 4.10 mark costs 20.00 and 1.30 of fees; the account is then flat.
  const auto close = s.preview(f.limit("close", 2, "4.00", Side::Sell), f.time);
  EXPECT_EQ(close.max_loss_basis, "scenario_grid");
  EXPECT_EQ(close.max_loss, m("21.30"));
  EXPECT_FALSE(*close.breaches_floor);
  // Selling more than is held still writes shorts, and the grid shows their risk.
  const auto flip = s.preview(f.limit("flip", 3, "4.00", Side::Sell), f.time);
  EXPECT_EQ(flip.max_loss_basis, "scenario_grid");
  EXPECT_GT(*flip.max_loss, *close.max_loss);
}
TEST(TradingBuyingPower, AMarketOrderOnAOneSidedBookReservesTheSideItWouldTake) {
  ScriptedMarket f; auto c = config(); c.initial_cash = m("100000");
  TradingSession s(c, f.time); f.seed(s);
  auto armed = f.market("armed");
  armed.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("50")};
  ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
  // An armed buy stop holds its level, the least it pays once the ask reaches it.
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("5000.65"));
  // Nobody bids now, and the ask is 0.10: it still holds its level.
  f.next();
  s.on_quotes({{f.symbol(), f.observation, f.time, std::nullopt, m("0.10"), 0, 27}}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Armed);
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("5000.65"));
}
TEST(TradingBreach, SolvesUpAndDownWithReflectionAndKeepsMissingInputsAbsent) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (const Quantity shares : {100, -100}) {
    Ledger ledger(m("10000")); ledger.trade_stock("SPY", shares, m("100"), {});
    const auto risk = breach_risk(ledger, {}, m("10000"), m("9000"), m("9500"), time,
        md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
    EXPECT_EQ(risk.room, m("1000")); EXPECT_EQ(risk.soft_room, m("500"));
    ASSERT_EQ(risk.underlyings.size(), 1U);
    const auto& item = risk.underlyings.front();
    const auto level = shares > 0 ? item.down : item.up;
    ASSERT_TRUE(level);
    EXPECT_NEAR(level->points, shares > 0 ? -10 : 10, 1e-7);
    EXPECT_NEAR(level->percent, shares > 0 ? -10 : 10, 1e-7);
    ASSERT_TRUE(level->touch_probability);
    EXPECT_GE(*level->touch_probability, 0);
    EXPECT_LE(*level->touch_probability, 1);
    EXPECT_FALSE(shares > 0 ? item.up : item.down);
    const auto missing = breach_risk(ledger, {}, m("10000"), m("9000"), {}, time, md::kNanosPerMinute);
    EXPECT_FALSE(missing.complete);
    EXPECT_FALSE(missing.underlyings.front().up);
  }
}
TEST(TradingBreach, WithoutAPlanFloorLevelsReachTheSoftFloor) {
  ScriptedMarket f; auto c = config(); c.rules.max_drawdown = {};
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  // Held underlyings are listed without a floor, with nothing to reach.
  auto risk = s.breach({{"SPX", 0.0004}});
  EXPECT_FALSE(risk.room); EXPECT_FALSE(risk.soft_room);
  ASSERT_EQ(risk.underlyings.size(), 1U);
  EXPECT_TRUE(risk.underlyings[0].complete);
  EXPECT_EQ(risk.underlyings[0].spot, 5000);
  EXPECT_FALSE(risk.underlyings[0].down); EXPECT_FALSE(risk.underlyings[0].up);
  // A soft floor 278.70 below equity: the long calls reach it on a fall.
  Guardrails g; g.soft_floor = m("9700"); s.set_guardrails(g, f.time);
  risk = s.breach({{"SPX", 0.0004}});
  EXPECT_FALSE(risk.room);
  EXPECT_EQ(risk.soft_room, m("278.70"));
  ASSERT_EQ(risk.underlyings.size(), 1U);
  ASSERT_TRUE(risk.underlyings[0].down);
  EXPECT_LT(risk.underlyings[0].down->percent, 0);
  EXPECT_FALSE(risk.underlyings[0].up);
}
TEST(TradingBreach, StaleMarksNeverProduceAccountBreachLevelsFromFreshGreeks) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  s.submit(f.market("long"), f.time);
  f.time += 61 * md::kNanosPerSecond;
  s.on_quotes({}, {f.valuation()}, f.time);
  const auto risk = s.breach({{"SPX", 0.2}});
  EXPECT_FALSE(risk.complete);
  EXPECT_FALSE(risk.room);
  ASSERT_EQ(risk.underlyings.size(), 1U);
  EXPECT_FALSE(risk.underlyings[0].complete);
  EXPECT_FALSE(risk.underlyings[0].down);
  EXPECT_FALSE(risk.underlyings[0].up);
}
}  // namespace
}  // namespace openport::trading
