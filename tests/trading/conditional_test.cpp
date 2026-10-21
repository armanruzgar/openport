#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
Trigger trigger(TriggerSource source, TriggerDirection direction, std::string_view level) {
  return {source, direction, m(level)};
}
OrderRequest with_bracket(OrderRequest request, std::optional<ExitSpec> stop, std::optional<ExitSpec> target) {
  request.bracket = Bracket{std::move(stop), std::move(target)};
  return request;
}
ExitSpec stop_at(std::string_view bid) { return {trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, bid), {}}; }
ExitSpec target_at(std::string_view limit) { return {{}, m(limit)}; }
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask, Quantity size = 10,
           double spot = 5000) {
  f.next();
  auto valuation = f.valuation();
  valuation.spot = spot;
  s.on_quotes({f.quote(bid, ask, size)}, {valuation}, f.time);
}
const Order& order(const TradingSession& s, OrderId id) { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(id - 1)); }

TEST(TradingConditional, BracketExitsSlipAndTakeProfitKeepsItsLimit) {
  for (const bool stop : {false, true}) {
    ScriptedMarket f;
    AccountRules rules;
    rules.slippage_ticks = 2;
    TradingSession s(roomy(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(with_bracket(f.market("entry"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
    quote(s, f, stop ? "3.50" : "5.10", stop ? "3.70" : "5.30");
    const auto snap = s.snapshot();
    ASSERT_EQ(snap->recent_fills.size(), 2U);
    EXPECT_EQ(snap->recent_fills.front().price, m("4.40"));
    EXPECT_EQ(snap->recent_fills.back().price, m(stop ? "3.30" : "5.00"));
    EXPECT_TRUE(snap->positions.empty());
    EXPECT_EQ(snap->account.fees, m("1.30"));
  }
}

TEST(TradingConditional, ArmedEntryActivatesWhenTheUnderlyingCrossesAndReservesUntilThen) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  auto request = f.market("breakout", 2);
  request.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrAbove, "5010");
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 1).day_end, f.contract.last_trade_time());  // good until expiry
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("841.30"));  // 2 * 100 * ask 4.20 + fees
  EXPECT_EQ(s.snapshot()->open_orders.size(), 1);
  quote(s, f, "4.00", "4.20", 10, 5009.99);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  quote(s, f, "4.50", "4.70", 10, 5010);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 1).triggered_at, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.70"));
}

TEST(TradingConditional, ReachedLevelsActivateAtSubmissionAndOptionTriggersUseTheExecutableSide) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto request = f.limit("now", 1, "4.20");
  request.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "4.20");  // buys compare the ask
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  auto bad = f.limit("bad", 1, "4.20");
  bad.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "0");
  EXPECT_EQ(s.submit(bad, f.time).decision.code, Reason::INVALID_ORDER);
}

TEST(TradingConditional, BracketStopTriggersOnTheBidAndCancelsTheTakeProfit) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  const auto& entry = order(s, 1);
  EXPECT_EQ(entry.status, OrderStatus::Filled);
  EXPECT_EQ(entry.stop_loss, 2);
  EXPECT_EQ(entry.take_profit, 3);
  const auto& stop = order(s, 2);
  EXPECT_EQ(stop.status, OrderStatus::Armed);
  EXPECT_EQ(stop.role, OrderRole::StopLoss);
  EXPECT_EQ(stop.request.side, Side::Sell);
  EXPECT_EQ(stop.request.type, OrderType::Market);
  EXPECT_EQ(stop.request.quantity, 2);
  EXPECT_EQ(stop.request.client_order_id, "entry:stop");
  EXPECT_EQ(stop.parent, 1);
  EXPECT_EQ(stop.oco, 3);
  const auto& target = order(s, 3);
  EXPECT_EQ(target.status, OrderStatus::Working);
  EXPECT_EQ(target.request.limit_price, m("5.00"));
  EXPECT_EQ(target.oco, 2);
  EXPECT_EQ(target.day_end, f.contract.expiry_time());
  quote(s, f, "3.60", "3.80");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // A gap below the stop still executes: exits skip the price band.
  quote(s, f, "2.00", "3.60");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("2.00"));
  EXPECT_EQ(order(s, 3).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, BracketTakeProfitFillsAndCancelsTheStop) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_at("3.50"), target_at("5.00")), f.time);
  quote(s, f, "5.00", "5.20");
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("5.00"));
  EXPECT_EQ(order(s, 2).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, APartialStopFillReArmsForTheRestAndKeepsTheTargetForIt) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 3, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  // The bid that reaches the stop shows one contract: the stop sells it and re-arms for the other two.
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 2).filled_quantity, 1);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).remaining(), 2);
  // The target is not cancelled: it shrinks to the two contracts still held.
  EXPECT_EQ(order(s, 3).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 3).request.quantity, 2);
  // The next quote still at the stop sells the rest; only then is the target cancelled.
  quote(s, f, "3.30", "3.50", 5);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("3.30"));
  EXPECT_EQ(s.snapshot()->recent_fills.back().quantity, 2);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, AStopThatFindsTheBidUsedUpReArmsInsteadOfCancelling) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("first", 1, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  ASSERT_TRUE(s.submit(with_bracket(f.limit("second", 1, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  // Both stops are reached on a bid of one contract: the first takes it, and the
  // second, finding nothing left, re-arms rather than cancel and leave its contract bare.
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 5).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 5).filled_quantity, 0);
  EXPECT_TRUE(order(s, 6).open());
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 5).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 6).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, APartialTargetFillShrinksTheStopInsteadOfCancellingIt) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 3, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  quote(s, f, "5.00", "5.20", 1);
  EXPECT_EQ(order(s, 3).status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(order(s, 3).filled_quantity, 1);
  // The stop still protects the two contracts left.
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).request.quantity, 2);
  quote(s, f, "3.40", "3.60", 5);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().quantity, 2);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, APartlyFilledExitCancelsItsSiblingWithoutClosingAnotherEntry) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 3, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("another entry"), f.time).decision.ok());
  quote(s, f, "3.40", "3.60", 1);
  EXPECT_EQ(order(s, 2).filled_quantity, 1);
  EXPECT_EQ(order(s, 3).remaining(), 2);
  // The stop's last two contracts finish this bracket; the other entry stays held.
  quote(s, f, "3.40", "3.60", 2);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 1);
}

TEST(TradingConditional, AStopLimitExitRestsAtItsLimitOnceReachedAndWaitsThroughAGap) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ExitSpec stop_limit{trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.50"), m("3.40")};
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_limit, target_at("5.00")), f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).request.type, OrderType::Limit);
  EXPECT_EQ(order(s, 2).request.tif, TimeInForce::Gtc);
  // The bid gaps through the stop and its limit: the stop triggers and rests at 3.40, far
  // below the band around the mid, rather than sell at 3.00 or cancel.
  quote(s, f, "3.00", "3.20");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 2).triggered_at, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1U);
  EXPECT_TRUE(order(s, 3).open());
  // Back at its limit, it sells, and the target is cancelled.
  quote(s, f, "3.40", "3.60");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("3.40"));
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // Its limit, like a take-profit's, takes the product tick.
  ExitSpec off_tick{trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.50"), m("3.42")};
  EXPECT_EQ(s.submit(with_bracket(f.limit("tick", 1, "3.60"), off_tick, {}), f.time).decision.code, Reason::INVALID_TICK);
}

TEST(TradingConditional, AStopLimitOrderIsBandedAroundItsStopAndKeepsWorkingThroughAGap) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("long", 1, "4.20"), f.time).decision.ok());
  // A protective stop-limit well below the market: its limit is 1.20 from the mid
  // (beyond the 0.82 band there) but 0.10 from its stop, which is what it is set against.
  auto protect = f.limit("protect", 1, "2.90", Side::Sell, TimeInForce::Gtc);
  protect.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.00");
  ASSERT_TRUE(s.submit(protect, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // A limit far from its own stop is still refused.
  auto far = f.limit("far", 1, "1.00", Side::Sell, TimeInForce::Gtc);
  far.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.00");
  EXPECT_EQ(s.submit(far, f.time).decision.code, Reason::PRICE_BAND);
  // The market gaps to 2.10: the order triggers and keeps working at 2.90 instead of
  // being cancelled for a limit outside the band around the new mid.
  quote(s, f, "2.00", "2.20");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 2).triggered_at, f.time);
  quote(s, f, "2.90", "3.10");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("2.90"));
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, ExitsGrowWithEntryFillsAndShrinkOrCancelWithManualCloses) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s, "4.00", "4.20", 2);
  s.submit(with_bracket(f.limit("entry", 5, "4.20"), stop_at("3.50"), target_at("5.00")), f.time);
  EXPECT_EQ(order(s, 1).filled_quantity, 2);
  EXPECT_EQ(order(s, 2).request.quantity, 2);
  EXPECT_EQ(order(s, 3).request.quantity, 2);
  quote(s, f, "4.00", "4.20", 3);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).request.quantity, 5);
  EXPECT_EQ(order(s, 3).request.quantity, 5);
  s.submit(f.market("trim", 3, Side::Sell), f.time);
  EXPECT_EQ(order(s, 2).request.quantity, 2);
  EXPECT_EQ(order(s, 3).request.quantity, 2);
  quote(s, f, "4.00", "4.20");
  s.submit(f.market("flat", 2, Side::Sell), f.time);
  EXPECT_EQ(order(s, 2).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(order(s, 3).reason.code, Reason::POSITION_CLOSED);
  EXPECT_TRUE(s.snapshot()->open_orders.empty());
}

TEST(TradingConditional, BuyOnlyAndBuyingPowerCountTheBracketOnceAndNeverBlockManualCloses) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buy_only = true;
  rules.buying_power = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 2, "4.20"), stop_at("3.50"), target_at("5.00")), f.time).decision.ok());
  // Two exits for 2 contracts reserve only one set of fees.
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("1.30"));
  // A sell stop protecting a long is a closing order, even on a buy-only plan.
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.limit("manual", 2, "4.40", Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(f.limit("extra", 1, "4.40", Side::Sell), f.time).decision.code, Reason::BUY_ONLY);
}

TEST(TradingConditional, OnABuyOnlyPlanAManualCloseSupersedesItsOwnArmedStop) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buy_only = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 3), f.time).decision.ok());
  // Two plain stop sells cover the 3 held: one for 2, then one for 1.
  auto stop = f.market("stop-2", 2, Side::Sell);
  stop.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.00");
  ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
  stop.client_order_id = "stop-1";
  stop.quantity = 1;
  ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
  // A close of 1 needs only the newer stop's contract: preview and submit agree,
  // and the older stop keeps protecting the rest.
  quote(s, f, "4.00", "4.20");
  EXPECT_TRUE(s.preview(f.market("close-1", 1, Side::Sell), f.time).decision.ok());
  const auto close = s.submit(f.market("close-1", 1, Side::Sell), f.time);
  ASSERT_TRUE(close.decision.ok()) << close.decision.message;
  EXPECT_EQ(order(s, 3).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 3).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(order(s, 3).reason.message, "Manual close order 4 superseded this stop on a buy-only plan");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // A sell beyond what is held is still refused, and leaves the stop in place.
  EXPECT_EQ(s.submit(f.market("too-many", 3, Side::Sell), f.time).decision.code, Reason::BUY_ONLY);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // A close refused for another reason leaves it too.
  s.trip_kill("pause", f.time);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  EXPECT_EQ(s.submit(f.limit("off-tick", 2, "4.01", Side::Sell), f.time).decision.code, Reason::INVALID_TICK);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  // Under the latch a close still works, and takes over the stop.
  ASSERT_TRUE(s.submit(f.limit("all", 2, "4.40", Side::Sell), f.time).decision.ok());
  EXPECT_EQ(order(s, 2).reason.code, Reason::POSITION_CLOSED);
}

TEST(TradingConditional, PreviewWarnsOfExitsAndTriggersThatActAtOnce) {
  ScriptedMarket f;
  AccountRules rules;
  rules.slippage_ticks = 10;
  auto c = roomy(rules);
  c.limits.price_band_absolute = m("0.50");
  c.limits.price_band_relative = 0.2;
  TradingSession s(c, f.time);
  f.seed(s, "3.00", "3.20");
  const auto codes = [&](const OrderRequest& r) {
    std::vector<std::string> out;
    for (const auto& w : s.preview(r, f.time).warnings) out.push_back(w.code);
    return out;
  };
  // A stop given as a limit price below the bid closes the long at once.
  const auto as_limit = s.preview(with_bracket(f.limit("a", 1, "3.20"), target_at("2.00"), {}), f.time).warnings;
  ASSERT_EQ(as_limit.size(), 1u);
  EXPECT_EQ(as_limit[0].code, "STOP_AS_LIMIT");
  EXPECT_EQ(as_limit[0].message, "The stop_loss has a limit price and no trigger, so it rests as a limit exit at 2.00, not a stop; "
            "the closing price now, 3.00, already reaches it, so it closes the position as soon as the entry fills. Give the stop a trigger instead.");
  // A stop already reached, and a take-profit already marketable.
  EXPECT_EQ(codes(with_bracket(f.limit("b", 1, "3.20"), stop_at("3.10"), target_at("2.90"))),
            (std::vector<std::string>{"STOP_REACHED", "TARGET_REACHED"}));
  EXPECT_TRUE(codes(with_bracket(f.limit("c", 1, "3.20"), stop_at("2.50"), target_at("4.00"))).empty());
  // A conditional order whose level is already reached activates at once.
  auto armed = f.limit("d", 1, "3.20");
  armed.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.50");
  EXPECT_EQ(codes(armed), (std::vector<std::string>{"TRIGGER_REACHED"}));
  // Ten slippage ticks of 0.10 put the market buy at 4.20, outside the 0.62 band around 3.10.
  const auto slipped = s.preview(f.market("e"), f.time);
  EXPECT_EQ(slipped.decision.code, Reason::PRICE_BAND);
  ASSERT_EQ(slipped.warnings.size(), 1u);
  EXPECT_EQ(slipped.warnings[0].code, "SLIPPAGE_BAND");
  EXPECT_NE(slipped.warnings[0].message.find("price this order at 4.20"), std::string::npos) << slipped.warnings[0].message;
}

TEST(TradingConditional, AMarketableLimitReservesItsFillAndAnArmedBuyStopItsLevel) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buying_power = true;
  auto c = roomy(rules);
  c.initial_cash = m("100000");
  c.limits.price_band_absolute = m("5");
  TradingSession s(c, f.time);
  f.seed(s, "41.60", "41.70", 30);
  // A protective limit above the ask: 22 at 45.50 would hold 100,100, but it pays 41.70.
  const auto protective = f.limit("protective", 22, "45.50");
  EXPECT_EQ(s.preview(protective, f.time).buying_power_required, m("91740") + c.fee_per_contract * 22);
  ASSERT_TRUE(s.submit(protective, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("41.70"));
  // An armed buy stop holds at least its level, not today's ask.
  TradingSession t(c, f.time);
  f.seed(t, "8.90", "9.00");
  auto stop = f.market("breakout");
  stop.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrAbove, "50.00");
  ASSERT_TRUE(t.submit(stop, f.time).decision.ok());
  EXPECT_EQ(t.snapshot()->buying_power.reserved, m("5000") + c.fee_per_contract);
}

TEST(TradingConditional, AnOffTickExitPriceIsAnInvalidTick) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  // A take-profit, and a stop exit that rests as a limit, off the contract's tier tick.
  EXPECT_EQ(s.submit(with_bracket(f.limit("target", 1, "4.10"), stop_at("4.00"), target_at("5.03")), f.time).decision.code,
            Reason::INVALID_TICK);
  EXPECT_EQ(s.submit(with_bracket(f.limit("stop", 1, "4.10"), target_at("4.03"), target_at("5.00")), f.time).decision.code,
            Reason::INVALID_TICK);
  EXPECT_TRUE(s.submit(with_bracket(f.limit("on-tick", 1, "4.10"), target_at("4.00"), target_at("5.00")), f.time).decision.ok());
}

TEST(TradingConditional, ABracketStopAlreadyReachedWhenTheEntryFillsFiresAtSubmission) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  // The bid, 4.00, is already at the stop: the stop sells at once, at that bid.
  ASSERT_TRUE(s.submit(with_bracket(f.market("entry", 2), stop_at("4.10"), target_at("5.00")), f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).triggered_at, f.time);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.00"));
  EXPECT_EQ(order(s, 3).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // So does one whose entry a change makes marketable, and one a triggered entry's fill creates.
  ASSERT_TRUE(s.submit(with_bracket(f.limit("changed", 1, "4.10"), stop_at("4.00"), target_at("5.00")), f.time).decision.ok());
  EXPECT_EQ(order(s, 4).status, OrderStatus::Working);
  ASSERT_TRUE(s.modify(4, {{}, m("4.20"), {}}, f.time).decision.ok());
  EXPECT_EQ(order(s, 5).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 6).reason.code, Reason::OCO_FILLED);
  auto armed = with_bracket(f.market("armed"), stop_at("4.00"), target_at("5.00"));
  armed.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrAbove, "4990");
  ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
  EXPECT_EQ(order(s, 7).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 8).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 9).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, ATriggeredDayOrderLastsTheSessionItActivatedIn) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto request = f.limit("dip", 1, "3.60");
  request.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrBelow, "4990");
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  // Armed, it lasts until the contract's last trade.
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 1).day_end, f.contract.last_trade_time());
  // The next day it activates, and then lasts that session, like any DAY order.
  f.time = md::new_york_to_utc({2026, 9, 23}, 11, 0);
  quote(s, f, "4.00", "4.20", 10, 4989);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Working);
  const auto end = md::new_york_to_utc({2026, 9, 23}, 16, 15);
  EXPECT_EQ(order(s, 1).day_end, end);
  s.on_quotes({}, {}, end);
  EXPECT_EQ(order(s, 1).reason.code, Reason::DAY_END);
  // A GTC one keeps waiting past the session end.
  f.time = md::new_york_to_utc({2026, 9, 24}, 11, 0);
  quote(s, f, "4.00", "4.20", 10, 5000);
  auto gtc = f.limit("gtc-dip", 1, "3.60", Side::Buy, TimeInForce::Gtc);
  gtc.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrBelow, "5000");
  ASSERT_TRUE(s.submit(gtc, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 2).day_end, f.contract.last_trade_time());
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 24}, 16, 15));
  EXPECT_EQ(order(s, 2).status, OrderStatus::Working);
}

TEST(TradingConditional, ArmedOrdersAndExitsReportTheLastTradeOrAutoCloseAsTheirDeadline) {
  for (const bool cutoff : {false, true}) {
    // An AM-settled monthly stops trading at the close the day before it expires.
    ScriptedMarket f;
    f.contract = *md::parse_osi("SPX261016C05000000");
    auto rules = AccountRules{};
    if (cutoff) rules.expiry_cutoff = 5 * md::kNanosPerMinute;
    TradingSession s(roomy(rules), f.time);
    f.seed(s);
    const auto deadline = md::new_york_to_utc({2026, 10, 15}, 16, cutoff ? 10 : 15);
    ASSERT_EQ(f.contract.last_trade_time() - rules.expiry_cutoff, deadline);
    auto armed = f.limit("armed", 1, "3.60");
    armed.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrBelow, "4900");
    ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
    EXPECT_EQ(order(s, 1).day_end, deadline);
    ASSERT_TRUE(s.submit(with_bracket(f.limit("entry", 1, "4.20"), stop_at("3.00"), target_at("5.00")), f.time).decision.ok());
    // Both exits last until the deadline, and the take-profit says so: it is good until then.
    for (const OrderId id : {OrderId{3}, OrderId{4}}) EXPECT_EQ(order(s, id).day_end, deadline);
    EXPECT_EQ(order(s, 3).request.tif, TimeInForce::Ioc);
    EXPECT_EQ(order(s, 4).request.tif, TimeInForce::Gtc);
    s.on_quotes({}, {}, deadline);
    for (const OrderId id : {OrderId{1}, OrderId{3}, OrderId{4}})
      EXPECT_EQ(order(s, id).reason.code, cutoff ? Reason::EXPIRY_CUTOFF : Reason::EXPIRED) << id;
  }
}

/// A held contract's exits: a sell (or buy) that closes it, carrying the bracket,
/// whose own terms are the take-profit's, or the stop's when there is no target.
OrderRequest held_exits(const ScriptedMarket& f, std::string client, Quantity quantity, std::optional<ExitSpec> stop,
                        std::optional<ExitSpec> target, Side side = Side::Sell) {
  const auto& primary = target ? *target : *stop;
  auto request = primary.limit_price ? f.limit(std::move(client), quantity, primary.limit_price->str(), side, TimeInForce::Gtc)
                                     : f.market(std::move(client), quantity, side);
  request.trigger = primary.trigger;
  request.exits_only = true;
  return with_bracket(std::move(request), std::move(stop), std::move(target));
}

TEST(TradingConditional, AHeldContractTakesAStopAndTargetThatCancelEachOther) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("entry", 2, "4.20"), f.time).decision.ok());
  const auto placed = s.submit(held_exits(f, "exits", 2, stop_at("3.50"), target_at("5.00")), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_TRUE(s.submit(held_exits(f, "exits", 2, stop_at("3.50"), target_at("5.00")), f.time).replayed);
  const auto target = order(s, *placed.order_id);
  EXPECT_EQ(target.role, OrderRole::TakeProfit);
  EXPECT_EQ(target.status, OrderStatus::Working);
  EXPECT_EQ(target.filled_quantity, 0);
  EXPECT_EQ(target.day_end, f.contract.expiry_time());
  const auto& stop = order(s, target.oco);
  EXPECT_EQ(stop.role, OrderRole::StopLoss);
  EXPECT_EQ(stop.status, OrderStatus::Armed);
  EXPECT_EQ(stop.request.client_order_id, "exits:stop");
  EXPECT_EQ(stop.request.type, OrderType::Market);
  EXPECT_EQ(stop.oco, target.id);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 1U);  // no entry of their own
  // They count once and hold only their fees, like a bracket's exits.
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("1.30"));
  quote(s, f, "3.50", "3.70");
  EXPECT_EQ(order(s, target.oco).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, target.id).reason.code, Reason::OCO_FILLED);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, HeldContractExitsMustCloseItAndMatchTheirPrimaryExit) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  EXPECT_EQ(s.submit(held_exits(f, "nothing-held", 1, stop_at("3.50"), target_at("5.00")), f.time).decision.code,
            Reason::INVALID_ORDER);
  ASSERT_TRUE(s.submit(f.limit("entry", 2, "4.20"), f.time).decision.ok());
  EXPECT_EQ(s.submit(held_exits(f, "oversize", 3, stop_at("3.50"), target_at("5.00")), f.time).decision.code, Reason::INVALID_ORDER);
  EXPECT_EQ(s.submit(held_exits(f, "adds", 2, stop_at("3.50"), target_at("5.00"), Side::Buy), f.time).decision.code,
            Reason::INVALID_ORDER);
  auto mismatch = held_exits(f, "mismatch", 2, stop_at("3.50"), target_at("5.00"));
  mismatch.limit_price = m("5.10");
  EXPECT_EQ(s.submit(mismatch, f.time).decision.code, Reason::INVALID_ORDER);
  auto day = held_exits(f, "day", 2, stop_at("3.50"), target_at("5.00"));
  day.tif = TimeInForce::Day;
  EXPECT_EQ(s.submit(day, f.time).decision.code, Reason::INVALID_ORDER);
  auto bare = held_exits(f, "bare", 2, stop_at("3.50"), target_at("5.00"));
  bare.bracket.reset();
  EXPECT_EQ(s.submit(bare, f.time).decision.code, Reason::INVALID_ORDER);
  // A working manual close holds one of the two contracts.
  ASSERT_TRUE(s.submit(f.limit("close-one", 1, "4.50", Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.submit(held_exits(f, "beside-close", 2, stop_at("3.50"), target_at("5.00")), f.time).decision.code,
            Reason::INVALID_ORDER);
  // A stop alone is the triggered market IOC itself, and is accepted under the kill latch.
  ASSERT_TRUE(s.trip_kill("reduce only", f.time).decision.ok());
  const auto stop = s.submit(held_exits(f, "stop-only", 1, stop_at("3.50"), {}), f.time);
  ASSERT_TRUE(stop.decision.ok()) << stop.decision.message;
  EXPECT_EQ(order(s, *stop.order_id).role, OrderRole::StopLoss);
  EXPECT_EQ(order(s, *stop.order_id).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, *stop.order_id).oco, 0U);
}

TEST(TradingConditional, HeldContractExitsAreAcceptedAfterTheCloseAndWaitForTheRegularSession) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("entry", 1, "4.20"), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 30);  // the curb session
  ++f.observation;
  s.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
  // A stop-limit as the only exit: its own terms are the GTC limit with the trigger.
  const auto placed = s.submit(held_exits(f, "late", 1, ExitSpec{stop_at("3.50").trigger, m("3.30")}, {}), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_EQ(order(s, *placed.order_id).status, OrderStatus::Armed);
  f.time = md::new_york_to_utc({2026, 9, 23}, 9, 31);
  ++f.observation;
  s.on_quotes({f.quote("3.40", "3.60")}, {f.valuation()}, f.time);
  EXPECT_EQ(order(s, *placed.order_id).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("3.40"));
}

TEST(TradingConditional, HeldContractExitsTakeExtendedSessionsButNotDay) {
  for (const auto tif : {TimeInForce::Exto, TimeInForce::GtcExto, TimeInForce::Day}) {
    ScriptedMarket f;
    TradingSession s(roomy(), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("entry", 1, "4.20"), f.time).decision.ok());
    f.time = md::new_york_to_utc({2026, 9, 22}, 16, 30);
    ++f.observation;
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    auto exits = held_exits(f, "exits", 1, stop_at("3.50"), target_at("5.00"));
    exits.tif = tif;
    const auto placed = s.submit(exits, f.time);
    if (tif == TimeInForce::Day) {
      EXPECT_EQ(placed.decision.code, Reason::INVALID_ORDER);
      continue;
    }
    ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
    ASSERT_TRUE(placed.order_id);
    const auto target = *placed.order_id;
    const auto stop = order(s, target).oco;
    EXPECT_EQ(order(s, target).request.tif, tif);
    EXPECT_EQ(order(s, target).status, OrderStatus::Working);
    ASSERT_NE(stop, 0U);
    EXPECT_EQ(order(s, stop).status, OrderStatus::Armed);
    quote(s, f, "3.50", "3.70");
    EXPECT_EQ(order(s, stop).status, OrderStatus::Filled);
    EXPECT_EQ(order(s, target).reason.code, Reason::OCO_FILLED);
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingConditional, CancellingAPairTakesBothExitsInOneTransaction) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("entry", 2, "4.20"), f.time).decision.ok());
  const auto target = *s.submit(held_exits(f, "exits", 2, stop_at("3.50"), target_at("5.00")), f.time).order_id;
  const auto stop = order(s, target).oco;
  const auto version = s.snapshot()->account_version;
  EXPECT_EQ(s.cancel_orders({target, 99}, f.time).decision.code, Reason::UNKNOWN_ORDER);
  EXPECT_EQ(order(s, target).status, OrderStatus::Working);
  ASSERT_TRUE(s.cancel_orders({target, stop}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->account_version, version + 2);  // the refused list, then both cancels together
  for (const auto id : {target, stop}) {
    EXPECT_EQ(order(s, id).status, OrderStatus::Cancelled);
    EXPECT_EQ(order(s, id).reason.code, Reason::USER_CANCEL);
  }
  EXPECT_EQ(s.cancel_orders({target, stop}, f.time).decision.code, Reason::ORDER_TERMINAL);
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
}

/// A trailing stop on an option, reading `reference`.
ExitSpec trailing(TriggerDirection direction, std::string_view level, TrailUnit unit, std::string_view value,
                  TriggerReference reference = TriggerReference::BidAsk, std::optional<std::string_view> limit = {}) {
  Trigger t{.source = TriggerSource::Option, .direction = direction, .level = m(level), .reference = reference, .trail = Trail{unit, m(value)}};
  return {t, limit ? std::optional(m(*limit)) : std::nullopt};
}

TEST(TradingConditional, ATrailingStopFollowsTheBidByAnAmountAndNeverMovesBack) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("entry", 1, "4.20"), f.time).decision.ok());
  // Its level starts loose; the bid of 4.00 at once lifts it to 3.50.
  const auto placed = s.submit(held_exits(f, "trail", 1, trailing(TriggerDirection::AtOrBelow, "1.00", TrailUnit::Amount, "0.50"), {}), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  const auto id = *placed.order_id;
  EXPECT_EQ(order(s, id).request.trigger->level, m("3.50"));
  EXPECT_TRUE(s.submit(held_exits(f, "trail", 1, trailing(TriggerDirection::AtOrBelow, "1.00", TrailUnit::Amount, "0.50"), {}), f.time).replayed);
  quote(s, f, "4.60", "4.80");
  EXPECT_EQ(order(s, id).request.trigger->level, m("4.10"));
  quote(s, f, "4.30", "4.50");
  EXPECT_EQ(order(s, id).request.trigger->level, m("4.10"));
  EXPECT_EQ(order(s, id).status, OrderStatus::Armed);
  quote(s, f, "4.10", "4.30");
  EXPECT_EQ(order(s, id).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.10"));
}

TEST(TradingConditional, ATrailingStopLimitOnTheMidMovesItsLimitOnTheTick) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("entry", 1, "4.20"), f.time).decision.ok());
  const auto stop = trailing(TriggerDirection::AtOrBelow, "3.00", TrailUnit::Percent, "10", TriggerReference::Mid, "2.90");
  const auto placed = s.submit(held_exits(f, "trail", 1, stop, {}), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  const auto id = *placed.order_id;
  // Mid 4.10 less 10% is 3.69; the limit moves as far on the 0.10 tick, 0.60, and so does the level.
  EXPECT_EQ(order(s, id).request.trigger->level, m("3.60"));
  EXPECT_EQ(order(s, id).request.limit_price, m("3.50"));
  quote(s, f, "5.00", "5.20");
  EXPECT_EQ(order(s, id).request.trigger->level, m("4.50"));
  EXPECT_EQ(order(s, id).request.limit_price, m("4.40"));
  quote(s, f, "4.40", "4.60");  // mid 4.50 reaches it, and the limit sells at the bid
  EXPECT_EQ(order(s, id).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.40"));
}

TEST(TradingConditional, ATrailingBuyStopFollowsTheAskDownByTicks) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("short", 1, "4.00", Side::Sell), f.time).decision.ok());
  const auto placed = s.submit(held_exits(f, "trail", 1, trailing(TriggerDirection::AtOrAbove, "9.00", TrailUnit::Ticks, "3"), {},
                                          Side::Buy), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  const auto id = *placed.order_id;
  EXPECT_EQ(order(s, id).request.trigger->level, m("4.50"));  // ask 4.20 plus three 0.10 ticks
  quote(s, f, "3.00", "3.20");
  EXPECT_EQ(order(s, id).request.trigger->level, m("3.50"));
  quote(s, f, "2.60", "2.80");  // below 3.00 the tick is 0.05
  EXPECT_EQ(order(s, id).request.trigger->level, m("2.95"));
  quote(s, f, "2.80", "2.95");
  EXPECT_EQ(order(s, id).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("2.95"));
}

TEST(TradingConditional, TrailsAndReferencesMustFitTheirTrigger) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("entry", 1, "4.20"), f.time).decision.ok());
  const auto rejects = [&](std::string client, Trigger t) {
    auto request = f.market(std::move(client), 1, Side::Sell);
    request.trigger = t;
    return s.submit(request, f.time).decision.code == Reason::INVALID_ORDER;
  };
  const auto spot = [](TriggerReference reference, std::optional<Trail> trail) {
    return Trigger{.source = TriggerSource::Underlying, .direction = TriggerDirection::AtOrBelow, .level = m("4990"), .reference = reference, .trail = trail};
  };
  const auto option = [](Trail trail) { return Trigger{.source = TriggerSource::Option, .direction = TriggerDirection::AtOrBelow, .level = m("3.00"), .trail = trail}; };
  EXPECT_TRUE(rejects("spot-mid", spot(TriggerReference::Mid, {})));
  EXPECT_TRUE(rejects("spot-ticks", spot(TriggerReference::BidAsk, Trail{TrailUnit::Ticks, m("2")})));
  EXPECT_TRUE(rejects("whole", option({TrailUnit::Percent, m("100")})));
  EXPECT_TRUE(rejects("zero", option({TrailUnit::Amount, m("0")})));
  EXPECT_TRUE(rejects("half-tick", option({TrailUnit::Ticks, m("1.5")})));
  // An underlying trail in dollars is accepted and follows spot.
  auto request = f.market("spot-trail", 1, Side::Sell);
  request.trigger = spot(TriggerReference::BidAsk, Trail{TrailUnit::Amount, m("5")});
  const auto placed = s.submit(request, f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_EQ(order(s, *placed.order_id).request.trigger->level, m("4995.00"));
  quote(s, f, "4.00", "4.20", 10, 5012.345);
  EXPECT_EQ(order(s, *placed.order_id).request.trigger->level, m("5007.34"));
}

TEST(TradingConditional, ATrailedLevelSurvivesRecovery) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-trail-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    s.submit(with_bracket(f.limit("entry", 1, "4.20"), trailing(TriggerDirection::AtOrBelow, "3.00", TrailUnit::Amount, "0.40",
                                                                  TriggerReference::Mark), target_at("6.00")), f.time);
    quote(s, f, "4.80", "5.00");
    EXPECT_EQ(order(s, 2).request.trigger->level, m("4.50"));  // mark 4.90 less 0.40
    EXPECT_EQ(order(s, 2).request.trigger->reference, TriggerReference::Mark);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  quote(recovered, f, "5.20", "5.40");
  EXPECT_EQ(order(recovered, 2).request.trigger->level, m("4.90"));
  std::filesystem::remove_all(directory);
}

/// `request` with `next` placed when it fills completely.
OrderRequest then(OrderRequest request, OrderRequest next) {
  next.client_order_id.clear();
  request.then = {std::move(next)};
  return request;
}
/// `request` with `other` accepted beside it, the first fill of either cancelling the other.
OrderRequest or_else(OrderRequest request, OrderRequest other) {
  other.client_order_id.clear();
  request.oco = {std::move(other)};
  return request;
}

TEST(TradingConditional, AnOrderPlacesItsNextOrderOnlyOnceItFillsCompletely) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto placed = s.submit(then(f.limit("scale", 2, "3.90"), f.limit("", 2, "4.30", Side::Sell, TimeInForce::Gtc)), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
  quote(s, f, "3.80", "3.90", 1);
  EXPECT_EQ(order(s, 1).status, OrderStatus::PartiallyFilled);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);  // not yet
  quote(s, f, "3.80", "3.90");
  ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
  EXPECT_EQ(order(s, 1).chained, 2U);
  const auto& next = order(s, 2);
  EXPECT_EQ(next.chained_from, 1U);
  EXPECT_EQ(next.request.client_order_id, "scale:then");
  EXPECT_EQ(next.status, OrderStatus::Working);
  EXPECT_EQ(next.accepted_at, f.time);
  EXPECT_TRUE(s.submit(then(f.limit("scale", 2, "3.90"), f.limit("", 2, "4.30", Side::Sell, TimeInForce::Gtc)), f.time).replayed);
  quote(s, f, "4.30", "4.50");
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradingConditional, AChainedOrderIsCheckedWhenPlacedAndAnUnfilledOrderPlacesNone) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  // Off the tick: refused when its turn comes, and recorded so.
  ASSERT_TRUE(s.submit(then(f.limit("entry", 1, "4.20"), f.limit("", 1, "5.03", Side::Sell, TimeInForce::Gtc)), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Rejected);
  EXPECT_EQ(order(s, 2).reason.code, Reason::INVALID_TICK);
  EXPECT_EQ(order(s, 1).chained, 2U);
  // Cancelled without a fill: nothing follows.
  const auto resting = *s.submit(then(f.limit("resting", 1, "3.50"), f.limit("", 1, "5.00", Side::Sell)), f.time).order_id;
  ASSERT_TRUE(s.cancel(resting, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.size(), 3U);
  EXPECT_EQ(order(s, resting).chained, 0U);
}

TEST(TradingConditional, TwoOrdersThatCancelEachOtherEndOnTheFirstFill) {
  for (const bool breakout : {false, true}) {
    ScriptedMarket f;
    TradingSession s(roomy(), f.time);
    f.seed(s);
    auto up = f.market("");
    up.trigger = trigger(TriggerSource::Underlying, TriggerDirection::AtOrAbove, "5010");
    const auto placed = s.submit(or_else(f.limit("dip", 1, "3.90"), up), f.time);
    ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
    ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
    EXPECT_EQ(order(s, 1).oco, 2U);
    EXPECT_EQ(order(s, 2).oco, 1U);
    EXPECT_EQ(order(s, 2).request.client_order_id, "dip:oco");
    EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
    if (breakout) quote(s, f, "4.40", "4.60", 10, 5010);
    else quote(s, f, "3.80", "3.90");
    const auto filled = breakout ? 2U : 1U, cancelled = breakout ? 1U : 2U;
    EXPECT_EQ(order(s, filled).status, OrderStatus::Filled);
    EXPECT_EQ(order(s, cancelled).status, OrderStatus::Cancelled);
    EXPECT_EQ(order(s, cancelled).reason.code, Reason::OCO_FILLED);
    EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 1);
  }
}

TEST(TradingConditional, AnOrderThatFillsOnAcceptanceCancelsItsPairBeforeItTrades) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto placed = s.submit(or_else(f.market("now"), f.market("")), f.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 2).reason.code, Reason::OCO_FILLED);
  EXPECT_EQ(order(s, 2).filled_quantity, 0);
  EXPECT_EQ(order(s, 1).oco, 2U);
  EXPECT_EQ(order(s, 2).oco, 1U);
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 1);
  // The other is still checked: a refused other refuses the pair, and nothing fills.
  const auto refused = s.submit(or_else(f.market("again"), f.limit("", 1, "3.53")), f.time);
  EXPECT_EQ(refused.decision.code, Reason::INVALID_TICK);
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 1);
}

TEST(TradingConditional, AChainedCloseThatSupersedesAStopOnABuyOnlyPlanIsPlaced) {
  ScriptedMarket f;
  AccountRules rules;
  rules.buy_only = true;
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 1), f.time).decision.ok());
  auto stop = f.market("stop", 1, Side::Sell);
  stop.trigger = trigger(TriggerSource::Option, TriggerDirection::AtOrBelow, "3.00");
  ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
  // Resting until the next quote, so it is among the open orders when it fills.
  // Placing the close replaces the account state while the open orders are visited,
  // and a later open order is still to be visited.
  ASSERT_TRUE(s.submit(then(f.limit("add", 1, "3.90"), f.limit("", 2, "4.40", Side::Sell, TimeInForce::Gtc)), f.time).decision.ok());
  const auto low = s.submit(f.limit("low", 1, "3.80", Side::Buy, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(low.decision.ok()) << low.decision.message;
  quote(s, f, "3.80", "3.90");
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
  ASSERT_EQ(s.snapshot()->recent_orders.size(), 5U);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Cancelled);
  EXPECT_EQ(order(s, 2).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(order(s, 5).status, OrderStatus::Working);
  EXPECT_EQ(order(s, 3).chained, 5U);
  EXPECT_EQ(order(s, 4).status, OrderStatus::Working);
}

TEST(TradingConditional, TwoOrdersThatCancelEachOtherAreAcceptedTogetherOrNotAtAll) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto refused = s.submit(or_else(f.limit("pair", 1, "3.90"), f.limit("", 1, "3.53")), f.time);
  EXPECT_EQ(refused.decision.code, Reason::INVALID_TICK);
  ASSERT_EQ(s.snapshot()->recent_orders.size(), 1U);  // the order, refused for the other; the other not recorded
  EXPECT_EQ(order(s, 1).status, OrderStatus::Rejected);
  EXPECT_EQ(order(s, 1).reason.message.rfind("The order that cancels it: ", 0), 0U);
  EXPECT_EQ(s.snapshot()->buying_power.reserved, Money{});
  // An oco takes no oco, and a chain is at most four orders.
  EXPECT_EQ(s.submit(or_else(f.limit("nested", 1, "3.90"), or_else(f.limit("", 1, "3.80"), f.limit("", 1, "3.70"))), f.time).decision.code,
            Reason::INVALID_ORDER);
  auto chain = f.limit("", 1, "3.10");
  for (const auto* price : {"3.20", "3.30", "3.40"}) chain = then(f.limit("", 1, price), chain);
  chain = then(f.limit("long", 1, "3.50"), chain);
  EXPECT_EQ(s.submit(chain, f.time).decision.code, Reason::INVALID_ORDER);
  // One that fills on acceptance cancels the other at once.
  const auto now = s.submit(or_else(f.limit("now", 1, "4.20"), f.limit("", 1, "3.80")), f.time);
  ASSERT_TRUE(now.decision.ok()) << now.decision.message;
  EXPECT_EQ(order(s, *now.order_id).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, *now.order_id + 1).reason.code, Reason::OCO_FILLED);
}

TEST(TradingConditional, APendingChainSurvivesRecovery) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-chain-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    s.submit(then(f.limit("scale", 1, "3.90"), or_else(f.limit("", 1, "3.50"), f.limit("", 1, "4.30", Side::Sell))), f.time);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  quote(recovered, f, "3.80", "3.90");
  // The fill places the next order and the one that cancels it, together.
  ASSERT_EQ(recovered.snapshot()->recent_orders.size(), 3U);
  EXPECT_EQ(order(recovered, 2).request.client_order_id, "scale:then");
  EXPECT_EQ(order(recovered, 3).request.client_order_id, "scale:then:oco");
  EXPECT_EQ(order(recovered, 2).oco, 3U);
  EXPECT_EQ(order(recovered, 2).chained_from, 1U);
  std::filesystem::remove_all(directory);
}

TEST(TradingConditional, TriggersWaitForTheRegularSessionAndSurviveRecovery) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-conditional-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    s.submit(with_bracket(f.limit("entry", 1, "4.20"), stop_at("3.50"), target_at("5.00")), f.time);
    // After the close a crossed stop waits for the regular session.
    f.time = md::new_york_to_utc({2026, 9, 22}, 16, 30);
    ++f.observation;
    s.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
    EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  f.time = md::new_york_to_utc({2026, 9, 23}, 9, 31);
  ++f.observation;
  recovered.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
  EXPECT_EQ(order(recovered, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(recovered, 3).reason.code, Reason::OCO_FILLED);
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading
