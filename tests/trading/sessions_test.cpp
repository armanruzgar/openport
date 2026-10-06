#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
Timestamp at(md::Date date, int hour, int minute) { return md::new_york_to_utc(date, hour, minute); }
constexpr md::Date kTuesday{2026, 9, 22};
constexpr md::Date kWednesday{2026, 9, 23};
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
/// A new quote and valuation for the contract at `time`.
void tick(TradingSession& s, ScriptedMarket& f, Timestamp time, std::string_view bid = "4.00", std::string_view ask = "4.20") {
  f.time = time;
  ++f.observation;
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
const Order& order(const TradingSession& s, OrderId id) { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(id - 1)); }

TEST(TradingSessions, ExpiringEtfOptionsTradeAQuarterHourAfterTheClose) {
  constexpr md::Date expiry{2026, 10, 22};
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261022C00500000");
  f.time = at(expiry, 16, 5);
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("after the close"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "4.40", Side::Sell), f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->positions.front().awaiting_settlement);
  // At 16:15 the contract expires: the resting order cancels and the position waits for settlement.
  f.time = at(expiry, 16, 15);
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(order(s, 2).reason.code, Reason::EXPIRED);
  EXPECT_TRUE(s.snapshot()->positions.front().awaiting_settlement);
  EXPECT_EQ(s.submit(f.market("too late"), f.time).decision.code, Reason::EXPIRED);

  // Expiring index series stop at 16:00.
  ScriptedMarket index;
  index.contract = *md::parse_osi("SPXW261022C05000000");
  index.time = at(expiry, 16, 5);
  TradingSession t(roomy(), index.time);
  index.seed(t);
  EXPECT_EQ(t.submit(index.market("late"), index.time).decision.code, Reason::EXPIRED);
}

TEST(TradingSessions, TheOvernightSessionTakesLimitOrdersThatLastUntilItEnds) {
  ScriptedMarket f;
  f.time = at(kTuesday, 21, 0);  // Wednesday's overnight session
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto night = s.submit(f.limit("night", 1, "4.10"), f.time);
  ASSERT_TRUE(night.decision.ok()) << night.decision.message;
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 1).day_end, at(kWednesday, 9, 25));
  tick(s, f, at(kWednesday, 1, 0), "4.00", "4.10");
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->recent_fills[0].price, m("4.10"));
  ASSERT_TRUE(s.submit(f.limit("unfilled", 1, "3.50"), f.time).decision.ok());
  tick(s, f, at(kWednesday, 9, 25));
  EXPECT_EQ(order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 2).reason.code, Reason::DAY_END);
  // A regular-session DAY order still ends with the regular session.
  tick(s, f, at(kWednesday, 9, 30));
  ASSERT_TRUE(s.submit(f.limit("regular", 1, "3.50"), f.time).decision.ok());
  EXPECT_EQ(order(s, 3).day_end, at(kWednesday, 16, 15));
}

TEST(TradingSessions, OutsideTheRegularSessionOnlyPlainLimitOrdersAreTaken) {
  ScriptedMarket f;
  f.time = at(kTuesday, 16, 30);  // curb
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_EQ(s.submit(f.market("market"), f.time).decision.code, Reason::LIMIT_ONLY);
  auto triggered = f.limit("triggered", 1, "4.10");
  triggered.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("4.00")};
  EXPECT_EQ(s.submit(triggered, f.time).decision.code, Reason::LIMIT_ONLY);
  auto bracket = f.limit("bracket", 1, "4.20");
  bracket.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.00")}, {}}, {}};
  EXPECT_EQ(s.submit(bracket, f.time).decision.code, Reason::LIMIT_ONLY);
  ASSERT_TRUE(s.submit(f.limit("curb", 1, "4.00"), f.time).decision.ok());
  EXPECT_EQ(order(s, 4).day_end, at(kTuesday, 17, 0));
  // Between sessions nothing trades, and the curb order has ended.
  tick(s, f, at(kTuesday, 18, 0));
  EXPECT_EQ(order(s, 4).reason.code, Reason::DAY_END);
  EXPECT_EQ(s.submit(f.limit("evening", 1, "4.00"), f.time).decision.code, Reason::SESSION_CLOSED);
  // Flattening sends market orders, so it waits for the regular session too:
  // refused, it leaves the position and the exits resting on it alone.
  tick(s, f, at(kTuesday, 21, 0));
  ASSERT_TRUE(s.submit(f.limit("long", 2, "4.20"), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->positions.size(), 1);
  ASSERT_TRUE(s.submit(f.limit("exit", 1, "4.60", Side::Sell, TimeInForce::Gtc), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("tonight", 1, "4.60", Side::Sell), f.time).decision.ok());
  const auto orders = s.snapshot()->recent_orders.size();
  const auto refused = s.close_positions(std::nullopt, f.time);
  EXPECT_EQ(refused.decision.code, Reason::LIMIT_ONLY);
  EXPECT_EQ(refused.decision.scope, "SPX");
  EXPECT_EQ(s.snapshot()->recent_orders.size(), orders) << "no closing order is recorded";
  EXPECT_EQ(s.snapshot()->open_orders.size(), 2);
  EXPECT_EQ(s.snapshot()->positions.size(), 1);
  EXPECT_EQ(s.close_positions(std::string("SPX"), f.time).decision.code, Reason::LIMIT_ONLY);
  // Between sessions it is refused SESSION_CLOSED; the GTC exit still waits.
  tick(s, f, at(kWednesday, 9, 26));
  EXPECT_EQ(s.close_positions(std::nullopt, f.time).decision.code, Reason::SESSION_CLOSED);
  EXPECT_EQ(s.snapshot()->open_orders.size(), 1);
  // In the regular session it cancels the exit and closes the position.
  tick(s, f, at(kWednesday, 9, 30));
  ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingSessions, MultiLegOrdersTakeANetLimitOvernight) {
  const auto call = *md::parse_osi("SPXW261022C05100000");
  const auto put = *md::parse_osi("SPXW261022P04900000");
  const auto time = at(kTuesday, 22, 0);
  TradingSession s(roomy(), time);
  std::vector<QuoteObservation> quotes;
  std::vector<Valuation> valuations;
  for (const auto& c : {call, put}) {
    ASSERT_TRUE(s.define(c, time).decision.ok());
    quotes.push_back({c.osi_symbol(), 1, time, m("4.00"), m("4.20"), 10, 10});
    valuations.push_back({c.osi_symbol(), time, 0.3, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                          md::years_between(time, c.expiry_time()), 0.20, true});
  }
  s.on_quotes(quotes, valuations, time);
  OrderRequest strangle;
  strangle.client_order_id = "strangle";
  strangle.type = OrderType::Market;
  strangle.tif = TimeInForce::Ioc;
  strangle.quantity = 1;
  strangle.legs = {{call.osi_symbol(), Side::Buy, 1}, {put.osi_symbol(), Side::Buy, 1}};
  EXPECT_EQ(s.submit(strangle, time).decision.code, Reason::LIMIT_ONLY);
  strangle.client_order_id = "strangle-limit";
  strangle.type = OrderType::Limit;
  strangle.tif = TimeInForce::Day;
  strangle.limit_price = m("8.40");
  const auto placed = s.submit(strangle, time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
}

TEST(TradingSessions, OptionsWithoutExtendedSessionsTradeRegularHoursOnly) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261022C00500000");
  f.time = at(kTuesday, 16, 30);
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_EQ(s.submit(f.limit("after-close", 1, "4.10"), f.time).decision.code, Reason::SESSION_CLOSED);
  tick(s, f, at(kTuesday, 21, 0));
  EXPECT_EQ(s.submit(f.limit("night", 1, "4.10"), f.time).decision.code, Reason::SESSION_CLOSED);
}

TEST(TradingSessions, AmSettledSeriesStopAtTheRegularCloseBeforeExpiry) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPX261016C05000000");  // expires at Friday's open
  const md::Date thursday{2026, 10, 15};
  f.time = at(thursday, 9, 0);  // Thursday's overnight session still trades it
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_TRUE(s.submit(f.limit("thursday-morning", 1, "4.00"), f.time).decision.ok());
  tick(s, f, at(thursday, 16, 30));
  EXPECT_EQ(s.submit(f.limit("curb", 1, "4.00"), f.time).decision.code, Reason::SESSION_CLOSED);
  tick(s, f, at(thursday, 21, 0));
  EXPECT_EQ(s.submit(f.limit("night", 1, "4.00"), f.time).decision.code, Reason::SESSION_CLOSED);
  // The PM weekly of the same date trades that night.
  ScriptedMarket weekly;
  weekly.contract = *md::parse_osi("SPXW261016C05000000");
  weekly.time = f.time;
  weekly.seed(s);
  EXPECT_TRUE(s.submit(weekly.limit("weekly-night", 1, "4.00"), weekly.time).decision.ok());
}

TEST(TradingSessions, AmClosingMarksAndValuationsStayFreshUntilSettlementTime) {
  constexpr md::Date thursday{2026, 9, 17};
  ScriptedMarket am, weekly;
  am.contract = *md::parse_osi("SPX   260918C06020000");
  weekly.contract = *md::parse_osi("SPXW260918P06020000");
  am.time = weekly.time = at(thursday, 10, 0);
  TradingSession s(roomy(), am.time);
  am.seed(s);
  weekly.seed(s);
  ASSERT_TRUE(s.submit(am.market("held-am"), am.time).decision.ok());
  ASSERT_EQ(am.contract.last_trade_time(), at(thursday, 16, 15));
  // A closing observation at the inclusive freshness boundary stays usable.
  const auto close = am.contract.last_trade_time() - s.config().limits.max_quote_age;
  tick(s, am, close);
  for (const auto time : {at(thursday, 16, 20), at(thursday, 21, 0)}) {
    SCOPED_TRACE(time);
    tick(s, weekly, time);
    const auto snapshot = s.snapshot();
    ASSERT_FALSE(snapshot->positions.empty());
    const auto& held = snapshot->positions.front();
    EXPECT_EQ(held.position.contract.osi_symbol(), am.symbol());
    EXPECT_EQ(held.mark, m("4.10"));
    EXPECT_EQ(held.mark_time, close);
    EXPECT_TRUE(held.fresh);
    EXPECT_FALSE(held.awaiting_settlement);
    EXPECT_TRUE(snapshot->valuation_complete);
    EXPECT_TRUE(snapshot->risk.complete);
    EXPECT_TRUE(snapshot->scenarios.complete);
    EXPECT_TRUE(snapshot->quality_flags.empty());
    const auto placed = s.submit(weekly.limit(std::to_string(time), 1, "4.10"), time);
    ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
    ASSERT_TRUE(placed.order_id);
    EXPECT_EQ(order(s, *placed.order_id).status, OrderStatus::Working);
    tick(s, weekly, time + md::kNanosPerSecond, "4.00", "4.10");
    EXPECT_EQ(order(s, *placed.order_id).status, OrderStatus::Filled);
  }
  tick(s, weekly, am.contract.expiry_time() + md::kNanosPerSecond);
  const auto snapshot = s.snapshot();
  ASSERT_FALSE(snapshot->positions.empty());
  EXPECT_TRUE(snapshot->positions.front().awaiting_settlement);
  EXPECT_FALSE(snapshot->positions.front().fresh);
  EXPECT_EQ(snapshot->positions.front().mark, m("4.10"));
  EXPECT_FALSE(snapshot->valuation_complete);
  EXPECT_FALSE(snapshot->risk.complete);
  EXPECT_FALSE(snapshot->scenarios.complete);
}

TEST(TradingSessions, ExitsAndTriggeredOrdersWaitForTheRegularSession) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto entry = f.limit("entry", 1, "4.20");
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")}, {}}, {}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  auto dip = f.limit("dip", 1, "3.40");
  dip.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.40")};
  ASSERT_TRUE(s.submit(dip, f.time).decision.ok());
  ASSERT_EQ(order(s, 2).status, OrderStatus::Armed);  // the stop
  // Overnight the stop and the trigger are crossed; neither acts.
  tick(s, f, at(kTuesday, 21, 0), "2.90", "3.10");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 3).status, OrderStatus::Armed);
  EXPECT_EQ(s.snapshot()->positions.size(), 1);
  // Plain overnight limit orders still trade beside them.
  ASSERT_TRUE(s.submit(f.limit("overnight", 1, "3.10"), f.time).decision.ok());
  EXPECT_EQ(order(s, 4).status, OrderStatus::Filled);
  tick(s, f, at(kWednesday, 9, 31), "2.90", "3.10");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
}

TEST(TradingSessions, AnOvernightTradeBelongsToTheNextTradingDate) {
  ScriptedMarket f;
  AccountRules rules;
  rules.plan = "Overnight";
  rules.profit_target = m("10000");
  rules.max_drawdown = m("5000");
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.trading_day(), kTuesday);
  tick(s, f, at(kTuesday, 16, 59));
  EXPECT_EQ(s.roll_day(at(kTuesday, 16, 59)).decision.code, Reason::INVALID_TIME);
  // The day ends with the curb session: Tuesday closes and the evening is Wednesday's.
  ASSERT_TRUE(s.roll_day(at(kTuesday, 17, 0)).decision.ok());
  EXPECT_EQ(s.trading_day(), kWednesday);
  ASSERT_FALSE(s.snapshot()->evaluation.days.empty());
  EXPECT_EQ(s.snapshot()->evaluation.days.back().day, kTuesday);
  tick(s, f, at(kTuesday, 21, 0));
  ASSERT_TRUE(s.submit(f.limit("night", 1, "4.20"), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1);
  EXPECT_EQ(s.snapshot()->evaluation.day, kWednesday);
  tick(s, f, at(kWednesday, 9, 0), "3.00", "3.20");
  EXPECT_EQ(s.trading_day(), kWednesday);  // no second rollover overnight
  EXPECT_EQ(s.roll_day(at(kWednesday, 10, 0)).decision.code, Reason::INVALID_TIME);
}

TEST(TradingSessions, AClosedMarketsCloseKeepsItsPositionsMarked) {
  // An SPY position held overnight keeps its closing mark, so SPX trades on.
  ScriptedMarket spy;
  spy.contract = *md::parse_osi("SPY261022C00500000");
  spy.time = at(kTuesday, 16, 14);
  TradingSession s(roomy(), spy.time);
  spy.seed(s);
  ASSERT_TRUE(s.submit(spy.limit("spy", 1, "4.20"), spy.time).decision.ok());
  tick(s, spy, at(kTuesday, 16, 15));  // the close
  ScriptedMarket spx;
  spx.time = at(kTuesday, 21, 0);
  spx.seed(s);
  EXPECT_TRUE(s.snapshot()->valuation_complete);
  EXPECT_TRUE(s.snapshot()->risk.complete);
  EXPECT_TRUE(s.submit(spx.limit("overnight", 1, "4.20"), spx.time).decision.ok());
  EXPECT_TRUE(s.roll_day(spx.time).decision.ok());
  // A feed that stops while its market is open is still stale.
  s.on_quotes({}, {}, at(kTuesday, 21, 5));
  EXPECT_FALSE(s.snapshot()->valuation_complete);
  EXPECT_EQ(s.submit(spx.limit("stale", 1, "4.20"), at(kTuesday, 21, 5)).decision.code, Reason::STALE_QUOTE);
}


TEST(TradingSessions, GtcSurvivesDailyRollAndWaitsForTheRegularSession) {
  ScriptedMarket f;
  auto c = roomy();
  c.rules.buying_power = true;
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("gtc", 2, "4.10", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  const auto reserve = s.snapshot()->buying_power.reserved;
  tick(s, f, at(kTuesday, 16, 30), "4.00", "4.10");
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  ASSERT_TRUE(s.roll_day(at(kTuesday, 21, 0)).decision.ok());
  tick(s, f, at(kWednesday, 1, 0), "4.00", "4.10");
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  EXPECT_EQ(s.snapshot()->buying_power.reserved, reserve);
  tick(s, f, at(kWednesday, 9, 30), "4.00", "4.10");
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
}

TEST(TradingSessions, GtcCanBePlacedOvernightModifiedAndCancelled) {
  ScriptedMarket f;
  f.time = at(kTuesday, 21, 0);
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("gtc", 1, "4.20", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  ASSERT_TRUE(s.modify(1, {{}, m("4.30"), {}}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  ASSERT_TRUE(s.cancel(1, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).reason.code, Reason::USER_CANCEL);
  auto bad = f.market("market-gtc");
  bad.tif = TimeInForce::Gtc;
  EXPECT_FALSE(s.submit(bad, f.time).decision.ok());
}

TEST(TradingSessions, GtcExpiresAtLastTradeOrAutoCloseEvenWithoutAHolding) {
  for (const bool am : {false, true}) {
    for (const bool cutoff : {false, true}) {
      ScriptedMarket f;
      if (am) f.contract = *md::parse_osi("SPX261022C05000000");
      auto c = roomy();
      if (cutoff) c.rules.expiry_cutoff = 5 * 60 * md::kNanosPerSecond;
      TradingSession s(c, f.time);
      f.seed(s);
      ASSERT_TRUE(s.submit(f.limit("gtc", 1, "4.10", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
      const auto end = f.contract.last_trade_time() - c.rules.expiry_cutoff;
      EXPECT_EQ(order(s, 1).day_end, end);
      s.on_quotes({}, {}, end - 1);
      EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
      s.on_quotes({}, {}, end);
      // The account's cutoff says so; otherwise the contract stopped trading.
      EXPECT_EQ(order(s, 1).reason.code, cutoff ? Reason::EXPIRY_CUTOFF : Reason::EXPIRED);
    }
  }
}

TEST(TradingSessions, DayOrdersOnAnExpiringContractEndAtItsLastTrade) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPXW260922C05000000");  // expires today at 16:00
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("day", 1, "4.10"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("gtc", 1, "4.10", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  auto legs = f.limit("combo", 1, "4.10");
  const auto later = *md::parse_osi("SPXW260923C05000000");
  ASSERT_TRUE(s.define(later, f.time).decision.ok());
  s.on_quotes({{later.osi_symbol(), 2, f.time, m("5.00"), m("5.20"), 10, 10}},
              {{later.osi_symbol(), f.time, 0.5, 0.001, 2.0, -0.1, 5000, 5010, 0.99,
                md::years_between(f.time, later.expiry_time()), 0.20, true}}, f.time);
  legs.symbol.clear();
  legs.limit_price = m("1.00");
  legs.legs = {{later.osi_symbol(), Side::Buy, 1}, {f.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(legs, f.time).decision.ok());
  // Each ends at the contract's last trade, 16:00, not at the 16:15 session end.
  for (const OrderId id : {OrderId{1}, OrderId{2}, OrderId{3}}) EXPECT_EQ(order(s, id).day_end, at(kTuesday, 16, 0)) << id;
  s.on_quotes({}, {}, at(kTuesday, 16, 0));
  for (const OrderId id : {OrderId{1}, OrderId{2}, OrderId{3}}) EXPECT_EQ(order(s, id).reason.code, Reason::EXPIRED) << id;
}

TEST(TradingSessions, EveryOrderOnAContractCancelsAtThePreExpiryCutoff) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPXW260922C05000000");
  auto c = roomy();
  c.rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  TradingSession s(c, f.time);
  f.seed(s);
  auto entry = f.limit("entry", 2, "4.20");
  entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.00")}, {}}, ExitSpec{{}, m("5.00")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("day-sell", 1, "4.40", Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("gtc-sell", 1, "4.40", Side::Sell, TimeInForce::Gtc), f.time).decision.ok());
  ScriptedMarket put = f;
  put.contract = *md::parse_osi("SPXW260922P05000000");
  put.seed(s);
  ASSERT_TRUE(s.submit(put.limit("unheld-day", 1, "4.10"), f.time).decision.ok());
  const auto cutoff = at(kTuesday, 15, 55);
  for (const OrderId id : {OrderId{2}, OrderId{3}, OrderId{4}, OrderId{5}, OrderId{6}}) EXPECT_EQ(order(s, id).day_end, cutoff) << id;
  s.on_quotes({}, {}, cutoff);
  // DAY and GTC orders, bracket exits and orders on contracts not held all give the same reason.
  for (const OrderId id : {OrderId{2}, OrderId{3}, OrderId{4}, OrderId{5}, OrderId{6}})
    EXPECT_EQ(order(s, id).reason.code, Reason::EXPIRY_CUTOFF) << id;
}

TEST(TradingSessions, GtcRechecksRiskOnTheNextDayAndWaitsOutMissingData) {
  ScriptedMarket f;
  auto c = roomy();
  c.limits.aggregate.dollar_delta = 600000;
  TradingSession s(c, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("gtc", 2, "4.10", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  ASSERT_TRUE(s.roll_day(at(kTuesday, 21, 0)).decision.ok());
  f.time = at(kWednesday, 10, 0);
  ++f.observation;
  auto v = f.valuation();
  v.valid = false;
  s.on_quotes({f.quote("4.00", "4.10")}, {v}, f.time);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  ASSERT_TRUE(s.set_limits(c.limits, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  v = f.valuation(0.9);
  s.on_quotes({f.quote("4.00", "4.10")}, {v}, f.time);
  EXPECT_EQ(order(s, 1).reason.code, Reason::RISK_CHANGED);
  EXPECT_NE(order(s, 1).reason.message.find("DELTA_LIMIT"), std::string::npos);
}


TEST(TradingSessions, GtcCancelsForBuyingPowerOnALaterDayAndForResetOrFailure) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPXW261022C09000000");
  auto c = roomy();
  c.initial_cash = m("50");
  c.rules.margin = MarginMode::Portfolio;
  c.rules.buying_power = true;
  TradingSession s(c, f.time);
  s.define(f.contract, f.time);
  auto v = f.valuation(0, 0);
  v.forward = 5000; v.years = 0.01; v.smile_iv = 0.01;
  s.on_quotes({f.quote("0.05", "0.10")}, {v}, f.time);
  ASSERT_TRUE(s.submit(f.limit("gtc", 1, "0.05", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  ASSERT_TRUE(s.roll_day(at(kTuesday, 21, 0)).decision.ok());
  f.time = at(kWednesday, 10, 0); ++f.observation;
  v.time = f.time; v.spot = 9000; v.forward = 9000;
  s.on_quotes({f.quote("0.04", "0.05")}, {v}, f.time);
  EXPECT_EQ(order(s, 1).reason.code, Reason::RISK_CHANGED);
  EXPECT_NE(order(s, 1).reason.message.find("BUYING_POWER"), std::string::npos);

  for (const bool reset : {true, false}) {
    ScriptedMarket market;
    auto config = roomy();
    config.rules.max_drawdown = m("100");
    TradingSession account(config, market.time);
    market.seed(account);
    ASSERT_TRUE(account.submit(market.limit("gtc", 1, "3.70", Side::Buy, TimeInForce::Gtc), market.time).decision.ok());
    if (reset) {
      ASSERT_TRUE(account.reset_account(config.initial_cash, config.rules, "new attempt", market.time).decision.ok());
    } else {
      ASSERT_TRUE(account.submit(market.market("held", 3), market.time).decision.ok());
      tick(account, market, market.time + md::kNanosPerSecond, "3.50", "3.80");
    }
    EXPECT_EQ(order(account, 1).reason.code, reset ? Reason::ACCOUNT_RESET : Reason::EVALUATION_CLOSED);
  }
}

TEST(TradingSessions, ExtendedStopsTriggerOnTheOvernightGapAtTheTouch) {
  for (const auto root : {"SPXW", "SPX", "XSP", "VIX", "RUT"}) {
    ScriptedMarket f;
    f.contract = *md::parse_osi(std::string(root) + "261022C05000000");
    auto config = roomy();
    config.rules.slippage_ticks = 2;
    config.rules.impact_ticks = 2;
    TradingSession s(config, f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
    auto stop = f.market("overnight stop", 2, Side::Sell);
    stop.tif = TimeInForce::GtcExto;
    stop.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")};
    ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
    EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
    f.time = at(kTuesday, 21, 0); ++f.observation;
    s.on_quotes({f.quote("2.00", "2.20", 1)}, {f.valuation()}, f.time);
    EXPECT_EQ(order(s, 2).triggered_at, f.time);
    EXPECT_EQ(order(s, 2).filled_quantity, 1);
    EXPECT_EQ(order(s, 2).reason.code, Reason::IOC_REMAINDER);
    EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("2.00"));
  }
}

TEST(TradingSessions, ExtendedBracketsRearmAndStopLimitsKeepTheirPrice) {
  for (const bool limited : {false, true}) {
    ScriptedMarket f;
    TradingSession s(roomy(), f.time); f.seed(s);
    auto entry = f.limit("entry", 2, "4.20", Side::Buy, TimeInForce::Exto);
    entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")},
        limited ? std::optional(m("3.40")) : std::nullopt}, ExitSpec{{}, m("5.00")}};
    ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
    EXPECT_EQ(order(s, 2).request.tif, TimeInForce::GtcExto);
    EXPECT_EQ(order(s, 3).request.tif, TimeInForce::GtcExto);
    f.time = at(kTuesday, 21, 0); ++f.observation;
    s.on_quotes({f.quote("2.00", "2.20", 1)}, {f.valuation()}, f.time);
    EXPECT_EQ(order(s, 2).triggered_at, f.time);
    if (limited) {
      EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
      EXPECT_EQ(order(s, 2).filled_quantity, 0);
      tick(s, f, f.time + md::kNanosPerSecond, "3.40", "3.60");
    } else {
      EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
      EXPECT_EQ(order(s, 2).filled_quantity, 1);
      tick(s, f, f.time + md::kNanosPerSecond, "2.00", "2.20");
    }
    EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
    EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingSessions, ExtoEndsWithCurbAndGtdEndsAtItsTimestamp) {
  ScriptedMarket f;
  f.time = at(kTuesday, 21, 0);
  TradingSession s(roomy(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("exto", 1, "4.10", Side::Buy, TimeInForce::Exto), f.time).decision.ok());
  auto gtd = f.limit("gtd", 1, "4.10", Side::Buy, TimeInForce::Gtd);
  gtd.good_till = at(kWednesday, 16, 45);
  ASSERT_TRUE(s.submit(gtd, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).day_end, at(kWednesday, 17, 0));
  s.on_quotes({}, {}, at(kWednesday, 16, 15));
  EXPECT_TRUE(order(s, 1).open());
  s.on_quotes({}, {}, *gtd.good_till - 1);
  EXPECT_TRUE(order(s, 2).open());
  s.on_quotes({}, {}, *gtd.good_till);
  EXPECT_EQ(order(s, 2).reason.code, Reason::GTD_END);
  s.on_quotes({}, {}, at(kWednesday, 17, 0));
  EXPECT_EQ(order(s, 1).reason.code, Reason::DAY_END);
}

TEST(TradingSessions, NewTimeInForceValidationAndRegularOnlyGtd) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time); f.seed(s);
  auto request = f.limit("missing", 1, "4.10", Side::Buy, TimeInForce::Gtd);
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_ORDER);
  request.client_order_id = "past"; request.good_till = f.time;
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_ORDER);
  request.client_order_id = "distant"; request.good_till = f.time + 367LL * 24 * 60 * 60 * md::kNanosPerSecond;
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_ORDER);
  request.client_order_id = "unneeded"; request.tif = TimeInForce::Day;
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::INVALID_ORDER);
  request.client_order_id = "gtd"; request.tif = TimeInForce::Gtd; request.good_till = at(kWednesday, 12, 0);
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  tick(s, f, at(kTuesday, 21, 0), "3.90", "4.00");
  EXPECT_TRUE(order(s, 5).open());
  EXPECT_EQ(s.submit(f.market("plain market"), f.time).decision.code, Reason::LIMIT_ONLY);
  tick(s, f, at(kWednesday, 9, 30), "3.90", "4.00");
  EXPECT_EQ(order(s, 5).status, OrderStatus::Filled);
}

TEST(TradingSessions, LimitFlattenWorksAcrossCurbAndOvernightQuotesAndManualRepricing) {
  for (const auto hour : {16, 21}) {
    ScriptedMarket f;
    TradingSession s(roomy(), f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 3), f.time).decision.ok());
    f.time = at(kTuesday, hour, 30); ++f.observation;
    s.on_quotes({f.quote("4.00", "4.20", 1)}, {f.valuation()}, f.time);
    EXPECT_EQ(s.close_positions({}, f.time).decision.code, Reason::LIMIT_ONLY);
    const auto preview = s.preview_close_positions({}, f.time, {}, {}, {}, {true, 1});
    ASSERT_TRUE(preview.decision.ok());
    const auto result = s.close_positions({}, f.time, {}, {true, 1});
    ASSERT_TRUE(result.decision.ok());
    ASSERT_EQ(result.residuals.size(), 1);
    EXPECT_EQ(result.residuals.front().working, 2);
    EXPECT_TRUE(order(s, 2).reduce_only);
    EXPECT_EQ(order(s, 2).request.type, OrderType::Limit);
    EXPECT_EQ(order(s, 2).request.tif, TimeInForce::Exto);
    EXPECT_EQ(order(s, 2).request.limit_price, m("3.90"));
    f.next();
    s.on_quotes({f.quote("3.00", "3.20", 1)}, {f.valuation()}, f.time);
    EXPECT_EQ(order(s, 2).filled_quantity, 2);
    EXPECT_EQ(order(s, 2).request.limit_price, m("2.90"));
    EXPECT_EQ(s.modify(2, {4, {}, {}}, f.time).decision.code, Reason::INVALID_ORDER);
    ASSERT_TRUE(s.modify(2, {{}, m("3.50"), {}}, f.time).decision.ok());
    EXPECT_FALSE(order(s, 2).limit_ticks);
    tick(s, f, f.time + md::kNanosPerSecond, "3.10", "3.30");
    EXPECT_EQ(order(s, 2).filled_quantity, 2);
    tick(s, f, f.time + md::kNanosPerSecond, "3.50", "3.70");
    EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingSessions, ExtoHonorsEarlyAndHolidayClosesAndDoesNotTradeBetweenSessions) {
  for (const bool holiday : {false, true}) {
    ScriptedMarket f;
    f.contract = *md::parse_osi("SPXW261218C05000000");
    const md::Date date = holiday ? md::Date{2026, 11, 26} : md::Date{2026, 11, 27};
    f.time = at(date, holiday ? 9 : 10, 0);
    TradingSession s(roomy(), f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("exto", 1, "4.10", Side::Buy, TimeInForce::Exto), f.time).decision.ok());
    const auto end = at(date, holiday ? 11 : 13, holiday ? 30 : 15);
    EXPECT_EQ(order(s, 1).day_end, end);
    s.on_quotes({}, {}, end);
    EXPECT_EQ(order(s, 1).reason.code, Reason::DAY_END);
  }
  ScriptedMarket f;
  TradingSession s(roomy(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("carry", 1, "4.10", Side::Buy, TimeInForce::GtcExto), f.time).decision.ok());
  tick(s, f, at(kTuesday, 18, 0), "3.90", "4.10");
  EXPECT_TRUE(order(s, 1).open());
  EXPECT_EQ(s.snapshot()->waiting.at(1).code, "SESSION_CLOSED");
  tick(s, f, at(kTuesday, 21, 0), "3.90", "4.10");
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
}

}  // namespace
}  // namespace openport::trading
