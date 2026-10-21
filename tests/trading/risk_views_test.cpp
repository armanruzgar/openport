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
  c.rules.buying_power = true;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.price_band_absolute = m("20");
  return c;
}
OrderRequest spread(const ScriptedMarket& f, const ScriptedMarket& wing, std::string client, Quantity units) {
  OrderRequest order = f.market(std::move(client), units);
  order.symbol.clear();
  order.legs = {{f.symbol(), Side::Buy, 1}, {wing.symbol(), Side::Sell, 1}};
  return order;
}

TEST(TradingBreach, TheSoftFloorHasItsOwnLevelsBesideThePlanFloors) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  Ledger ledger(m("10000")); ledger.trade_stock("SPY", 100, m("100"), {});
  // 100 shares lose 1,000 at -10% (the plan floor) and 500 at -5% (the soft floor).
  const auto both = breach_risk(ledger, {}, m("10000"), m("9000"), m("9500"), time,
      md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
  ASSERT_EQ(both.underlyings.size(), 1U);
  const auto& item = both.underlyings.front();
  ASSERT_TRUE(item.down); ASSERT_TRUE(item.soft_down);
  EXPECT_NEAR(item.down->percent, -10, 1e-7);
  EXPECT_NEAR(item.soft_down->percent, -5, 1e-7);
  EXPECT_NEAR(item.soft_down->points, -5, 1e-7);
  ASSERT_TRUE(item.soft_down->touch_probability);
  EXPECT_GT(*item.soft_down->touch_probability, *item.down->touch_probability);
  EXPECT_FALSE(item.up); EXPECT_FALSE(item.soft_up);
  // Without a plan floor the levels are the soft floor's, under both names.
  const auto soft = breach_risk(ledger, {}, m("10000"), {}, m("9500"), time,
      md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
  ASSERT_TRUE(soft.underlyings.front().down); ASSERT_TRUE(soft.underlyings.front().soft_down);
  EXPECT_NEAR(soft.underlyings.front().down->percent, -5, 1e-7);
  EXPECT_EQ(soft.underlyings.front().soft_down->percent, soft.underlyings.front().down->percent);
  // Without a soft floor there are none.
  const auto plan = breach_risk(ledger, {}, m("10000"), m("9000"), {}, time,
      md::kNanosPerMinute, {{"SPY", 100}}, {{"SPY", 0.0004}});
  ASSERT_TRUE(plan.underlyings.front().down);
  EXPECT_FALSE(plan.underlyings.front().soft_down);
}
TEST(TradingBreach, AccountsWithBothFloorsPublishTheSoftFloorsLevels) {
  ScriptedMarket f;
  SessionConfig c;
  c.initial_cash = m("10000");
  c.rules.max_drawdown = m("1000");
  c.limits.price_band_absolute = m("20");
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  Guardrails g; g.soft_floor = m("9700"); s.set_guardrails(g, f.time);
  const auto risk = s.breach({{"SPX", 0.0004}});
  ASSERT_EQ(risk.underlyings.size(), 1U);
  const auto& item = risk.underlyings.front();
  ASSERT_TRUE(item.down); ASSERT_TRUE(item.soft_down);
  // The soft floor is nearer: a smaller fall reaches it.
  EXPECT_LT(item.down->percent, item.soft_down->percent);
  EXPECT_LT(item.soft_down->percent, 0);
}
TEST(TradingPreview, BuyingPowerFitIsReportedApartFromFloorFit) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  // A call costs 420.65: half the 1,000 floor room fits one, and 10,000 of
  // buying power fits 23 (24 would cost 10,095.60).
  const auto p = s.preview(f.market("size"), f.time);
  EXPECT_EQ(p.max_units, 1);
  EXPECT_EQ(p.max_units_basis, "floor");
  EXPECT_EQ(p.max_units_buying_power, 23);
  EXPECT_EQ(p.max_units_floor, 1);
  // The whole room fits two, still within buying power.
  const auto whole = s.preview(f.market("size"), f.time, 1);
  EXPECT_EQ(whole.max_units, 2);
  EXPECT_EQ(whole.max_units_floor, 2);
  // Without a floor, sizing is buying power and the limits alone.
  auto c = config(); c.rules.max_drawdown = {};
  TradingSession practice(c, f.time); f.seed(practice);
  const auto free = practice.preview(f.market("size"), f.time);
  EXPECT_EQ(free.max_units, 23);
  EXPECT_EQ(free.max_units_buying_power, 23);
  EXPECT_FALSE(free.max_units_floor);
  EXPECT_EQ(free.max_units_basis, "buying_power");
}
TEST(TradingPreview, SizingNamesOrderAndExposureLimitsWithoutAFloor) {
  ScriptedMarket f;
  auto c = config(); c.rules.max_drawdown = {}; c.initial_cash = m("1000000");
  c.limits.max_order_contracts = 100;
  TradingSession count(c, f.time); f.seed(count);
  const auto capped = count.preview(f.market("size"), f.time);
  EXPECT_EQ(capped.max_units, 100);
  EXPECT_EQ(capped.max_units_basis, "limits");
  EXPECT_FALSE(capped.max_units_floor);
  // Each call has 250,000 dollar delta; the cap allows two, not three.
  c.limits.aggregate = {600000, 1e9};
  TradingSession delta(c, f.time); f.seed(delta);
  const auto exposure = delta.preview(f.market("size"), f.time);
  EXPECT_EQ(exposure.max_units, 2);
  EXPECT_EQ(exposure.max_units_basis, "limits");
  // The same basis accompanies sizing a resting order on changed terms.
  const auto order = delta.submit(f.limit("rest", 1, "3.90"), f.time);
  ASSERT_TRUE(order.decision.ok());
  OrderChange change; change.limit_price = m("3.80");
  const auto edited = delta.preview_change(*order.order_id, change, f.time);
  EXPECT_EQ(edited.max_units_basis, "limits");
  // A personal soft floor is a real floor even without an evaluation plan.
  Guardrails g; g.soft_floor = m("999000"); count.set_guardrails(g, f.time);
  const auto soft = count.preview(f.market("size"), f.time);
  EXPECT_EQ(soft.max_units, 1);
  EXPECT_EQ(soft.max_units_basis, "floor");
}
TEST(TradingPreview, SizingIsNullWhenUnavailableAndZeroWhenNothingFits) {
  ScriptedMarket f; auto c = config(); c.initial_cash = m("300");
  TradingSession poor(c, f.time); f.seed(poor);
  // One call needs 420.65 of the 300 available: no size fits.
  const auto short_of_power = poor.preview(f.market("poor"), f.time);
  EXPECT_EQ(short_of_power.decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(short_of_power.max_units, 0);
  EXPECT_EQ(short_of_power.max_units_basis, "buying_power");
  EXPECT_EQ(short_of_power.max_units_buying_power, 0);
  EXPECT_EQ(short_of_power.max_units_floor, 0);
  TradingSession s(config(), f.time); f.seed(s);
  // An off-tick limit is refused at any size: sizing is unavailable, not zero.
  const auto off_tick = s.preview(f.limit("tick", 1, "4.01"), f.time);
  EXPECT_EQ(off_tick.decision.code, Reason::INVALID_TICK);
  EXPECT_FALSE(off_tick.max_units);
  EXPECT_TRUE(off_tick.max_units_basis.empty());
  EXPECT_FALSE(off_tick.max_units_buying_power);
  EXPECT_FALSE(off_tick.max_units_floor);
  // So is a paused account's.
  s.trip_kill("pause", f.time);
  const auto paused = s.preview(f.market("paused"), f.time);
  EXPECT_EQ(paused.decision.code, Reason::KILL_SWITCH);
  EXPECT_FALSE(paused.max_units);
  s.reset_kill("resume", f.time);
  // And a stale book's: the loss cannot be projected.
  f.time += 61 * md::kNanosPerSecond;
  const auto stale = s.preview(f.market("stale"), f.time);
  EXPECT_EQ(stale.decision.code, Reason::STALE_QUOTE);
  EXPECT_FALSE(stale.max_units);
}
TEST(TradingPreview, HeldExitsShowBuyingPowerOnceAcceptedNotAsIfATargetFilled) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  ASSERT_TRUE(s.submit(spread(f, wing, "open", 2), f.time).decision.ok());
  // A target far from the market: the pair rests, holding its fees.
  OrderRequest exits = spread(f, wing, "exits", 2);
  for (auto& leg : exits.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
  exits.type = OrderType::Limit;
  exits.tif = TimeInForce::Gtc;
  exits.limit_price = m("-6");
  exits.exits_only = true;
  exits.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, m("1")}, {}}, ExitSpec{{}, m("-6")}};
  const auto before = s.snapshot()->buying_power.available;
  const auto p = s.preview(exits, f.time);
  ASSERT_TRUE(p.decision.ok()) << p.decision.message;
  ASSERT_TRUE(p.buying_power_after);
  EXPECT_EQ(p.buying_power_before, before);
  EXPECT_EQ(p.buying_power_required, m("2.60"));
  EXPECT_EQ(*p.buying_power_after, before - m("2.60"));
  EXPECT_FALSE(p.max_units);
  ASSERT_TRUE(s.submit(exits, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, *p.buying_power_after);
}
TEST(TradingPreview, ExecutionShowsWhatFillsAtOnceAndWhatHappensToTheRest) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s, "4", "4.20", 15);
  // As displayed: 15 of 20 fill at the ask and the other 5 cancel.
  const auto p = s.preview(f.market("twenty", 20), f.time);
  ASSERT_TRUE(p.decision.ok());
  EXPECT_EQ(p.execution.status, OrderStatus::Cancelled);
  EXPECT_EQ(p.execution.filled_quantity, 15);
  EXPECT_EQ(p.execution.remaining_quantity, 5);
  EXPECT_EQ(p.execution.reason.code, Reason::IOC_REMAINDER);
  ASSERT_EQ(p.execution.fills.size(), 1U);
  EXPECT_EQ(p.execution.fills[0].quantity, 15);
  EXPECT_EQ(p.execution.fills[0].price, m("4.20"));
  EXPECT_EQ(p.execution.average_fill_price, m("4.20"));
  // The full-size projection still prices all 20 at the far side.
  ASSERT_EQ(p.execution.schedule.size(), 1U);
  EXPECT_EQ(p.execution.schedule[0].quantity, 20);
  EXPECT_EQ(p.execution.average_price, m("4.20"));
  // Nothing was taken: submitting gets the same fills.
  ASSERT_TRUE(s.submit(f.market("twenty", 20), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.back().filled_quantity, 15);
  // A limit below the ask rests.
  const auto resting = s.preview(f.limit("rest", 2, "3.00"), f.time);
  EXPECT_EQ(resting.execution.status, OrderStatus::Working);
  EXPECT_EQ(resting.execution.filled_quantity, 0);
  EXPECT_EQ(resting.execution.remaining_quantity, 2);
  EXPECT_TRUE(resting.execution.fills.empty());
  // A refused order executes nothing.
  const auto refused = s.preview(f.limit("tick", 1, "4.01"), f.time);
  EXPECT_EQ(refused.execution.status, OrderStatus::Rejected);
  EXPECT_EQ(refused.execution.reason.code, Reason::INVALID_TICK);
}
TEST(TradingPreview, ExecutionWalksImpactBlocksAndWaitsForFillLatency) {
  // An ask of 1.05 for 8 contracts; each further block of 8 costs one tick (0.05) more.
  ScriptedMarket f; auto c = config();
  c.initial_cash = m("100000"); c.rules.max_drawdown = m("6000"); c.rules.impact_ticks = 1;
  TradingSession s(c, f.time); f.seed(s, "1.00", "1.05", 8);
  const auto p = s.preview(f.market("26", 26), f.time);
  ASSERT_EQ(p.execution.schedule.size(), 4U);
  const std::vector<std::pair<Quantity, Money>> blocks{{8, m("1.05")}, {8, m("1.10")}, {8, m("1.15")}, {2, m("1.20")}};
  for (std::size_t i = 0; i < blocks.size(); ++i) {
    EXPECT_EQ(p.execution.schedule[i].quantity, blocks[i].first);
    EXPECT_EQ(p.execution.schedule[i].price, blocks[i].second);
  }
  // 2,880 of premium over 26 contracts.
  EXPECT_EQ(p.execution.average_price, m("1.107692"));
  EXPECT_EQ(p.execution.status, OrderStatus::Filled);
  EXPECT_EQ(p.execution.filled_quantity, 26);
  EXPECT_EQ(p.execution.fills.size(), 4U);
  EXPECT_EQ(p.execution.average_fill_price, m("1.107692"));
  // With fill latency nothing fills on acceptance: the order works until a later quote.
  c.rules.impact_ticks = 0; c.rules.fill_latency_ms = 1000;
  TradingSession delayed(c, f.time); f.seed(delayed, "1.00", "1.05", 8);
  const auto waiting = delayed.preview(f.market("delayed", 2), f.time);
  EXPECT_EQ(waiting.execution.status, OrderStatus::Working);
  EXPECT_EQ(waiting.execution.filled_quantity, 0);
}
TEST(TradingPreview, ASpreadsScheduleNetsItsLegs) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  const auto p = s.preview(spread(f, wing, "spread", 2), f.time);
  ASSERT_EQ(p.execution.schedule.size(), 2U);
  EXPECT_EQ(p.execution.schedule[0].side, Side::Buy);
  EXPECT_EQ(p.execution.schedule[1].side, Side::Sell);
  // Buy at 4.20, sell at 2.00: a net debit of 2.20 a unit.
  EXPECT_EQ(p.execution.average_price, m("2.20"));
  EXPECT_EQ(p.execution.average_fill_price, m("2.20"));
  EXPECT_EQ(p.execution.filled_quantity, 2);
}
TEST(TradingPreview, AChangeIsPreviewedAsModifyWouldMakeItWithoutMakingIt) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 2, "3.00"), f.time).decision.ok());
  const auto id = s.snapshot()->recent_orders.back().id;
  const auto state = s.snapshot_json();
  // Repricing to the ask makes it marketable: it would fill 3 at once.
  OrderChange change; change.quantity = 3; change.limit_price = m("4.20");
  const auto p = s.preview_change(id, change, f.time);
  ASSERT_TRUE(p.decision.ok()) << p.decision.message;
  EXPECT_EQ(p.execution.status, OrderStatus::Filled);
  EXPECT_EQ(p.execution.filled_quantity, 3);
  ASSERT_EQ(p.execution.fills.size(), 1U);
  EXPECT_EQ(p.execution.fills[0].price, m("4.20"));
  // It holds 2 x 300.65 now; 3 at the new limit reserve 3 x 420.65.
  const auto before = s.snapshot()->buying_power.available;
  EXPECT_EQ(p.buying_power_before, before);
  EXPECT_EQ(p.buying_power_required, m("1261.95"));
  EXPECT_EQ(p.buying_power_working, before + m("601.30") - m("1261.95"));
  EXPECT_EQ(p.buying_power_after, m("10000") - m("1261.95"));
  ASSERT_TRUE(p.exposure_change);
  EXPECT_EQ(p.exposure_change->dollar_delta, 750000);
  EXPECT_EQ(p.max_loss, m("1261.95"));
  EXPECT_EQ(s.snapshot_json(), state);
  // The change itself does what the preview said.
  ASSERT_TRUE(s.modify(id, change, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.at(id - 1).filled_quantity, 3);
  EXPECT_EQ(s.snapshot()->buying_power.available, *p.buying_power_after);
}
TEST(TradingPreview, AChangeCountsOnlyWhatItFillsNow) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  // 10 of 15 fill at 4.20; the other 5 rest once the ask moves to 4.40.
  ASSERT_TRUE(s.submit(f.limit("part", 15, "4.20"), f.time).decision.ok());
  const auto id = s.snapshot()->recent_orders.back().id;
  f.next(); s.on_quotes({f.quote("4.20", "4.40")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->recent_orders.at(id - 1).filled_quantity, 10);
  OrderChange reprice; reprice.limit_price = m("4.40");
  const auto p = s.preview_change(id, reprice, f.time);
  ASSERT_TRUE(p.decision.ok()) << p.decision.message;
  EXPECT_EQ(p.execution.status, OrderStatus::Filled);
  EXPECT_EQ(p.execution.filled_quantity, 5);
  EXPECT_EQ(p.execution.remaining_quantity, 0);
  EXPECT_EQ(p.execution.average_fill_price, m("4.40"));
  ASSERT_EQ(p.execution.fills.size(), 1U);
  EXPECT_EQ(p.execution.fills[0].quantity, 5);
  // The schedule and the projection are for the 5 still to fill.
  ASSERT_EQ(p.execution.schedule.size(), 1U);
  EXPECT_EQ(p.execution.schedule[0].quantity, 5);
  // Sizing counts units beside the 10 filled: the quantity to send is 10 + max_units.
  ASSERT_TRUE(p.max_units_buying_power);
  OrderChange most; most.quantity = 10 + *p.max_units_buying_power;
  EXPECT_TRUE(s.preview_change(id, most, f.time).decision.ok());
  most.quantity = *most.quantity + 1;
  EXPECT_EQ(s.preview_change(id, most, f.time).decision.code, Reason::BUYING_POWER);
}
TEST(TradingPreview, AChangePreviewGivesTheRefusalAndSizesTheUnitsStillWorking) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "3.00"), f.time).decision.ok());
  const auto id = s.snapshot()->recent_orders.back().id;
  // 40 at 3.00 need 12,026 of the 10,000: refused, and the order stays as it is.
  OrderChange bigger; bigger.quantity = 40;
  const auto refused = s.preview_change(id, bigger, f.time);
  EXPECT_EQ(refused.decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(refused.execution.status, OrderStatus::Working);
  EXPECT_EQ(refused.execution.remaining_quantity, 1);
  EXPECT_TRUE(refused.execution.fills.empty());
  // A unit at 3.00 loses at most 300.65: one fits half the 1,000 room, 33 fit buying power.
  EXPECT_EQ(refused.max_units, 1);
  EXPECT_EQ(refused.max_units_buying_power, 33);
  EXPECT_EQ(refused.max_units_floor, 1);
  OrderChange none;
  EXPECT_EQ(s.preview_change(id, none, f.time).decision.code, Reason::INVALID_ORDER);
  EXPECT_EQ(s.preview_change(99, bigger, f.time).decision.code, Reason::UNKNOWN_ORDER);
  s.cancel(id, f.time);
  EXPECT_EQ(s.preview_change(id, bigger, f.time).decision.code, Reason::ORDER_TERMINAL);
  EXPECT_THROW((void)s.preview_change(id, bigger, f.time, 0), TradingError);
}
TEST(TradingWhatIf, CandidatesAreComparedOnAPrivateCopyAgainstTheBookAsItIs) {
  ScriptedMarket f, wing; wing.contract.strike = 5010;
  TradingSession s(config(), f.time); f.seed(s); wing.seed(s, "2", "2.20");
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  const auto state = s.snapshot_json();
  const auto equity = s.snapshot()->equity;
  // Close the calls; sell the wing against them; buy 22 more, which buying power
  // refuses alone (9,254.30 against 9,158.70) but takes once the close has filled.
  const auto close = f.market("close", 2, Side::Sell);
  const auto more = f.market("more", 22);
  const auto w = s.what_if({{close}, {wing.market("wing", 2, Side::Sell)}, {more}, {close, more}}, f.time);
  EXPECT_EQ(w.current.equity, equity);
  ASSERT_TRUE(w.current.max_loss);
  ASSERT_TRUE(w.current.exposure);
  ASSERT_EQ(w.candidates.size(), 4U);
  // Closing at the 4.00 bid costs 0.10 a contract against the mark and the fees, and
  // leaves nothing to lose on the grid.
  const auto& closed = w.candidates[0];
  EXPECT_TRUE(closed.decision.ok());
  EXPECT_EQ(closed.after.equity, equity - m("21.30"));
  EXPECT_EQ(closed.after.max_loss, m("21.30"));
  EXPECT_EQ(closed.after.equity_at_max_loss, equity - m("21.30"));
  ASSERT_TRUE(closed.after.exposure);
  EXPECT_DOUBLE_EQ(closed.after.exposure->dollar_delta, 0);
  EXPECT_EQ(closed.after.breach.room, equity - m("21.30") - m("9000"));
  // A call spread loses less than the calls alone.
  EXPECT_TRUE(w.candidates[1].decision.ok());
  ASSERT_TRUE(w.candidates[1].after.max_loss);
  EXPECT_LT(*w.candidates[1].after.max_loss, *w.current.max_loss);
  EXPECT_LT(w.candidates[1].after.exposure->dollar_delta, w.current.exposure->dollar_delta);
  // Refused, but still projected beside the reason.
  EXPECT_EQ(w.candidates[2].decision.code, Reason::BUYING_POWER);
  ASSERT_EQ(w.candidates[2].orders.size(), 1U);
  EXPECT_TRUE(w.candidates[2].after.projected);
  ASSERT_TRUE(w.candidates[2].after.max_loss);
  // Each order is checked after the ones before it filled.
  ASSERT_EQ(w.candidates[3].orders.size(), 2U);
  EXPECT_TRUE(w.candidates[3].orders[0].ok());
  EXPECT_TRUE(w.candidates[3].orders[1].ok()) << w.candidates[3].orders[1].message;
  EXPECT_TRUE(w.candidates[3].decision.ok());
  // Nothing was written.
  EXPECT_EQ(s.snapshot_json(), state);
}
TEST(TradingWhatIf, ChainsFollowSubmissionFillsReservationsAndChildRejections) {
  ScriptedMarket f;
  TradingSession s(config(), f.time); f.seed(s, "4", "4.20", 2);
  const auto before = s.snapshot_json();
  auto entry = f.market("entry", 1);
  entry.then = {f.market("", 1, Side::Sell)};
  auto resting = f.limit("resting", 1, "3.90");
  resting.then = entry.then;
  auto partial = f.limit("partial", 3, "4.20");
  partial.then = entry.then;
  auto refused = entry;
  refused.then.front().quantity = 1000;
  auto oco = entry;
  oco.then.clear();
  oco.oco = {f.limit("", 1, "3.90")};
  const auto results = s.what_if({{entry}, {resting}, {partial}, {refused}, {oco}}, f.time);
  EXPECT_DOUBLE_EQ(results.candidates[0].after.exposure->dollar_delta, 0);
  EXPECT_EQ(results.candidates[0].after.equity, s.snapshot()->equity - m("21.30"));
  EXPECT_EQ(results.candidates[1].after.equity, s.snapshot()->equity);
  EXPECT_GT(results.candidates[2].after.exposure->dollar_delta, 0);
  EXPECT_FALSE(results.candidates[3].decision.ok());
  EXPECT_TRUE(results.candidates[4].decision.ok());
  EXPECT_EQ(results.candidates[4].after.equity, s.snapshot()->equity - m("10.65"));
  EXPECT_EQ(s.snapshot_json(), before);
}
TEST(TradingWhatIf, AnOrderWithoutAContractOrQuoteLeavesTheAccountUnprojected) {
  ScriptedMarket f, missing; missing.contract.strike = 6000;
  TradingSession s(config(), f.time); f.seed(s);
  const auto w = s.what_if({{missing.market("unknown", 1)}, {f.market("known", 1)}}, f.time);
  ASSERT_EQ(w.candidates.size(), 2U);
  EXPECT_FALSE(w.candidates[0].decision.ok());
  EXPECT_FALSE(w.candidates[0].after.projected);
  EXPECT_TRUE(w.candidates[1].after.projected);
  // The integration's gate refuses an order as it would a submission.
  const auto gated = s.what_if({{f.market("gated", 1)}}, f.time, {{Decision{Reason::FEED_STALLED, "Feed stalled", {}, {}, {}}}});
  EXPECT_EQ(gated.candidates[0].decision.code, Reason::FEED_STALLED);
}
TEST(TradingFlattenPreview, AFlattenDryRunCancelsAndClosesOnAPrivateCopy) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  const auto resting = s.submit(f.limit("resting", 1, "3.00"), f.time);
  ASSERT_TRUE(resting.decision.ok());
  const auto state = s.snapshot_json();
  const auto equity = s.snapshot()->equity;
  const auto p = s.preview_close_positions(std::nullopt, f.time);
  EXPECT_TRUE(p.decision.ok());
  ASSERT_EQ(p.cancelled.size(), 1U);
  EXPECT_EQ(p.cancelled.front(), *resting.order_id);
  // The calls sell at the 4.00 bid: 0.10 a contract below the mark and the fees.
  ASSERT_EQ(p.orders.size(), 1U);
  EXPECT_EQ(p.orders.front().status, OrderStatus::Filled);
  EXPECT_EQ(p.orders.front().request.side, Side::Sell);
  EXPECT_EQ(p.orders.front().filled_quantity, 2);
  ASSERT_EQ(p.fills.size(), 1U);
  EXPECT_EQ(p.fills.front().price, m("4.00"));
  EXPECT_TRUE(p.remaining.empty());
  EXPECT_EQ(p.current.equity, equity);
  EXPECT_EQ(p.after.equity, equity - m("21.30"));
  ASSERT_TRUE(p.after.exposure);
  EXPECT_DOUBLE_EQ(p.after.exposure->dollar_delta, 0);
  // Out of scope, nothing closes or cancels.
  const auto other = s.preview_close_positions("SPY", f.time);
  EXPECT_TRUE(other.orders.empty());
  EXPECT_TRUE(other.cancelled.empty());
  EXPECT_EQ(other.remaining.size(), 0U);
  // A gate that refuses every close refuses the flatten, cancelling nothing.
  const auto gated = s.preview_close_positions(std::nullopt, f.time, {{"SPX", Decision{Reason::FEED_STALLED, "Feed stalled", {}, {}, {}}}});
  EXPECT_EQ(gated.decision.code, Reason::FEED_STALLED);
  EXPECT_TRUE(gated.cancelled.empty());
  EXPECT_EQ(gated.remaining.size(), 1U);
  // Nothing was written, and the flatten itself still does what its dry run showed.
  EXPECT_EQ(s.snapshot_json(), state);
  const auto closed = s.close_positions(std::nullopt, f.time);
  EXPECT_TRUE(closed.decision.ok());
  EXPECT_EQ(s.snapshot()->equity, p.after.equity);
}
}  // namespace
}  // namespace openport::trading
