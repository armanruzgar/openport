#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"
#include "openport/trading/history.hpp"
#include "trading/state.hpp"
#include "support/journal.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money dollars(std::string_view value) { return Money::parse(value); }
void mark(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask, double spot = 5000) {
  f.next();
  auto v = f.valuation();
  v.spot = spot;
  s.on_quotes({f.quote(bid, ask)}, {v}, f.time);
}
struct ReviewDirectory {
  std::filesystem::path path;
  ReviewDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-review-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~ReviewDirectory() { std::filesystem::remove_all(path); }
  std::string file(const char* name) const { return (path / name).string(); }
};

TEST(TradeReview, StampsPreFillMarketAccountAndFirstEntryLastExit) {
  ScriptedMarket f;
  SessionConfig config;
  config.rules.max_drawdown = dollars("5000");
  TradingSession s(config, f.time);
  f.seed(s);
  f.next();
  auto v = f.valuation(); v.spot_source = "parity";
  s.on_quotes({f.quote()}, {v}, f.time);
  const auto before = s.snapshot();
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  const auto entry = s.snapshot()->recent_fills.front().context;
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->spot, 5000);
  EXPECT_EQ(entry->spot_source, "parity");
  EXPECT_EQ(entry->iv, .2);
  EXPECT_EQ(entry->delta, .5);
  EXPECT_EQ(entry->years, v.years);
  EXPECT_EQ(entry->equity, before->equity);
  EXPECT_EQ(entry->floor_room, dollars("5000"));
  EXPECT_EQ(entry->buying_power, before->buying_power.available - dollars("841.30"));  // accepted order reservation
  mark(s, f, "5.00", "5.20", 5002);
  ASSERT_TRUE(s.submit(f.market("trim", 1, Side::Sell), f.time).decision.ok());
  mark(s, f, "6.00", "6.20", 5004);
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  const auto lives = lifecycles(s.snapshot()->recent_fills, {}, s.contracts());
  ASSERT_EQ(lives.size(), 1U);
  EXPECT_EQ(lives.front().entry_context->spot, 5000);
  EXPECT_EQ(lives.front().exit_context->spot, 5004);
}

TEST(TradeReview, UnknownAnalyticsAndFloorRemainNullWithStockSpotFallback) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  auto order = f.market("bracket");
  order.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, dollars("3.00")}, {}}, {}};
  ASSERT_TRUE(s.submit(order, f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->recent_fills.front().context->floor_room);
  EXPECT_TRUE(s.snapshot()->recent_fills.front().context->spot_source.empty());
  f.next(); auto invalid = f.valuation(); invalid.valid = false;
  s.on_quotes({f.quote("2.90", "3.10")}, {invalid}, f.time, {{"SPX", f.time, dollars("4995")}});
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 2U);
  const auto c = s.snapshot()->recent_fills.back().context;
  ASSERT_TRUE(c);
  EXPECT_FALSE(c->iv); EXPECT_FALSE(c->delta);
  EXPECT_EQ(c->spot, 4995); EXPECT_EQ(c->spot_source, "quote");
  EXPECT_TRUE(c->years); EXPECT_FALSE(c->floor_room);
}

TEST(TradeReview, ExcursionsIncludeRealisedPnlAcrossPartialClosesAndKeepFirstTie) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  mark(s, f, "2.90", "3.10", 4990); const auto worst_time = f.time;
  mark(s, f, "5.90", "6.10", 5010); const auto best_time = f.time;
  mark(s, f, "5.90", "6.10", 5011);
  ASSERT_TRUE(s.submit(f.market("trim", 1, Side::Sell), f.time).decision.ok());
  mark(s, f, "4.40", "4.60", 5005);
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  const auto review = s.snapshot()->trade_reviews.at("1");
  EXPECT_EQ(review.worst->pnl, dollars("-241.30"));
  EXPECT_EQ(review.best->pnl, dollars("358.70"));
  EXPECT_EQ(review.worst->time, worst_time); EXPECT_EQ(review.best->time, best_time);
  EXPECT_EQ(review.worst->spot, 4990); EXPECT_EQ(review.best->spot, 5010);
  EXPECT_TRUE(review.finished); EXPECT_FALSE(review.planned_risk);
  mark(s, f, "10.00", "10.20");
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").best->pnl, review.best->pnl);
  ASSERT_TRUE(s.submit(f.market("again"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.size(), 2U);
}

TEST(TradeReview, BracketRiskUsesEntryStopAndScalesWithOpeningContracts) {
  ScriptedMarket f;
  TradingSession s({}, f.time); f.seed(s);
  auto order = f.market("stop", 2);
  order.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, dollars("3.00")}, {}}, {}};
  ASSERT_TRUE(s.submit(order, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").planned_risk, dollars("240"));
  ASSERT_TRUE(s.submit(f.market("trim", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").planned_risk, dollars("240"));
}

TEST(TradeReview, AddsKeepTheEntrysRiskPerContractWhereverTheyFill) {
  ScriptedMarket f;
  TradingSession s({}, f.time); f.seed(s);
  auto order = f.market("stop");
  order.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, dollars("3.00")}, {}}, {}};
  ASSERT_TRUE(s.submit(order, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").planned_risk, dollars("120"));
  // The stop moves to 1.00, which does not rewrite the plan, and the price falls
  // below the entry's stop: adds there still risk 1.20 a contract each.
  const auto opened = s.snapshot();
  const auto stop = std::find_if(opened->open_orders.begin(), opened->open_orders.end(),
                                 [](const Order& o) { return o.role == OrderRole::StopLoss; });
  ASSERT_NE(stop, opened->open_orders.end());
  OrderChange lower; lower.trigger_level = dollars("1.00");
  ASSERT_TRUE(s.modify(stop->id, lower, f.time).decision.ok());
  mark(s, f, "1.90", "2.10");
  ASSERT_TRUE(s.submit(f.market("add below the stop"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").planned_risk, dollars("240"));
  mark(s, f, "1.10", "1.30");
  ASSERT_TRUE(s.submit(f.market("deeper add"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").planned_risk, dollars("360"));
  mark(s, f, "5.90", "6.10");
  ASSERT_TRUE(s.submit(f.market("add above the entry"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->trade_reviews.at("1").planned_risk, dollars("480"));
  ASSERT_TRUE(s.submit(f.market("close", 4, Side::Sell), f.time).decision.ok());
  const auto review = s.snapshot()->trade_reviews.at("1");
  EXPECT_TRUE(review.finished);
  EXPECT_EQ(review.planned_risk, dollars("480"));
}

TEST(TradeReview, ASettledRoundTripsLastSampleCarriesTheSettlementAsItsSpot) {
  const auto expiry = md::new_york_to_utc({2026, 10, 22}, 15, 59);
  ScriptedMarket a, b, c; b.contract.strike += 10; c.contract.strike += 20;
  a.time = b.time = c.time = expiry;
  TradingSession s({}, a.time); a.seed(s); b.seed(s, "2.00", "2.20"); c.seed(s);
  ASSERT_TRUE(s.submit(c.market("single"), c.time).decision.ok());
  OrderRequest vertical; vertical.client_order_id = "vertical"; vertical.type = OrderType::Market; vertical.tif = TimeInForce::Ioc;
  vertical.quantity = 1; vertical.legs = {{a.symbol(), Side::Buy, 1}, {b.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(vertical, a.time).decision.ok());
  // The last quotes before the close carry spot 5001; the index settles at 4994,
  // and the contracts' valuations are still fresh when they settle at 16:00.
  a.time = b.time = c.time = expiry + 44 * md::kNanosPerSecond;
  a.next(); b.next(); c.next();
  auto va = a.valuation(), vb = b.valuation(), vc = c.valuation();
  va.spot = vb.spot = vc.spot = 5001;
  s.on_quotes({a.quote("0.90", "1.10"), b.quote("0.40", "0.60"), c.quote("0.90", "1.10")}, {va, vb, vc}, a.time);
  for (const auto* contract : {&c, &a, &b})
    ASSERT_TRUE(s.settle(contract->symbol(), dollars("4994"), contract->contract.expiry_time()).decision.ok());
  const auto single = s.snapshot()->trade_reviews.at("1");
  EXPECT_TRUE(single.finished);
  EXPECT_EQ(single.worst->pnl, dollars("-420.65"));
  EXPECT_EQ(single.worst->time, c.contract.expiry_time());
  EXPECT_EQ(single.worst->spot, 4994);
  EXPECT_EQ(single.best->spot, 5000);
  const auto strategy = s.snapshot()->strategy_reviews.at("2");
  EXPECT_TRUE(strategy.finished);
  EXPECT_EQ(strategy.worst->time, a.contract.expiry_time());
  EXPECT_EQ(strategy.worst->spot, 4994);
}

TEST(TradeReview, StrategySamplesCombinedPnlAndExactDefinedRisk) {
  ScriptedMarket a, b; b.contract.strike += 10;
  TradingSession s({}, a.time); a.seed(s); b.seed(s, "2.00", "2.20");
  OrderRequest open; open.client_order_id = "vertical"; open.type = OrderType::Market; open.tif = TimeInForce::Ioc;
  open.quantity = 1; open.legs = {{a.symbol(), Side::Buy, 1}, {b.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(open, a.time).decision.ok());
  const auto fills = s.snapshot()->recent_fills;
  ASSERT_EQ(fills.size(), 2U);
  EXPECT_EQ(fills[0].context->equity, fills[1].context->equity);
  EXPECT_EQ(fills[0].context->buying_power, fills[1].context->buying_power);
  EXPECT_EQ(s.snapshot()->strategy_reviews.at("1").planned_risk, dollars("220"));
  a.next(); b.next();
  s.on_quotes({a.quote("6.00", "6.20"), b.quote("3.00", "3.20")}, {a.valuation(), b.valuation()}, a.time);
  const auto best = a.time;
  a.next(); b.next();
  s.on_quotes({a.quote("8.00", "8.20"), b.quote("7.00", "7.20")}, {a.valuation(), b.valuation()}, a.time);
  const auto review = s.snapshot()->strategy_reviews.at("1");
  EXPECT_EQ(review.best->pnl, dollars("78.70")); EXPECT_EQ(review.best->time, best);
  EXPECT_EQ(review.worst->pnl, dollars("-121.30"));
  ASSERT_TRUE(s.submit(b.market("close short", 1, Side::Buy), a.time).decision.ok());
  ASSERT_TRUE(s.submit(a.market("close long", 1, Side::Sell), a.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->strategy_reviews.at("1").finished);
  EXPECT_EQ(s.snapshot()->strategy_reviews.at("1").worst->pnl, dollars("-142.60"));
}

TEST(TradeReview, AStrategysComboStopAndACoveredCalendarPlanTheirRisk) {
  // A debit vertical with a combo stop: 2.20 paid a unit, closed at a 1.00 credit.
  ScriptedMarket a, b; b.contract.strike += 10;
  TradingSession s({}, a.time); a.seed(s); b.seed(s, "2.00", "2.20");
  OrderRequest vertical; vertical.client_order_id = "vertical"; vertical.type = OrderType::Market; vertical.tif = TimeInForce::Ioc;
  vertical.quantity = 2; vertical.legs = {{a.symbol(), Side::Buy, 1}, {b.symbol(), Side::Sell, 1}};
  vertical.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Combo, TriggerDirection::AtOrAbove, dollars("-1.00")}, {}}, {}};
  const auto placed = s.submit(vertical, a.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  EXPECT_EQ(s.snapshot()->strategy_reviews.at("1").planned_risk, dollars("240"));
  // A call calendar: sell the near month at 4.00, buy the next at 6.20, a 2.20 debit.
  ScriptedMarket near, later; later.contract.expiry = {2026, 11, 20};
  TradingSession c({}, near.time); near.seed(c); later.seed(c, "6.00", "6.20");
  OrderRequest calendar; calendar.client_order_id = "calendar"; calendar.type = OrderType::Market; calendar.tif = TimeInForce::Ioc;
  calendar.quantity = 1; calendar.legs = {{near.symbol(), Side::Sell, 1}, {later.symbol(), Side::Buy, 1}};
  ASSERT_TRUE(c.submit(calendar, near.time).decision.ok());
  EXPECT_EQ(c.snapshot()->strategy_reviews.at("1").planned_risk, dollars("220"));
  // A diagonal whose long sits above its short call does not cover it.
  ScriptedMarket wide; wide.contract.expiry = {2026, 11, 20}; wide.contract.strike += 50;
  TradingSession d({}, near.time); near.seed(d); wide.seed(d, "3.00", "3.20");
  OrderRequest diagonal = calendar; diagonal.client_order_id = "diagonal";
  diagonal.legs = {{near.symbol(), Side::Sell, 1}, {wide.symbol(), Side::Buy, 1}};
  const auto uncovered = d.submit(diagonal, near.time);
  if (uncovered.decision.ok()) {
    EXPECT_FALSE(d.snapshot()->strategy_reviews.at("1").planned_risk);
  }
}

TEST(TradeReview, DayNotesValidateClearAndSurviveResetAndRecoveryWithExcursions) {
  ReviewDirectory directory;
  ScriptedMarket f;
  auto sink = test::create_journal(directory.file("journal"));
  TradingSession s({}, f.time, sink); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  mark(s, f, "5.00", "5.20", 5001);
  ASSERT_TRUE(s.annotate_day({2026, 9, 22}, "  Wait for the open.  ", "Good exit.\nKeep size small.", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->day_notes.at("2026-09-22").plan, "Wait for the open.");
  for (const auto& invalid : {std::string(2001, 'x'), std::string("bell\a"), std::string("\xff")})
    EXPECT_THROW(s.annotate_day({2026, 9, 22}, invalid, "", f.time), TradingError);
  EXPECT_THROW(s.annotate_day({2026, 2, 30}, "", "", f.time), TradingError);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(directory.file("journal"))).snapshot_json(), s.snapshot_json());
  ASSERT_TRUE(s.reset_account(dollars("100000"), {}, "new attempt", f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->day_notes.contains("2026-09-22"));
  EXPECT_TRUE(s.snapshot()->trade_reviews.at("1").finished);
  EXPECT_TRUE(s.annotate_day({2026, 9, 22}, " ", "", f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->day_notes.empty());
}

TEST(TradeReview, JournalsFromBeforeReviewLoadWithoutInventingContextOrExcursions) {
  ReviewDirectory directory;
  ScriptedMarket f;
  {
    TradingSession s({}, f.time, test::create_journal(directory.file("new"))); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  }
  {
    auto expanded = test::create_journal(directory.file("expanded"));
    TradingSession::expand(FileJournal::read(directory.file("new")), *expanded);
  }
  {
    auto old = test::create_journal(directory.file("old"));
    for (const auto& record : FileJournal::read(directory.file("expanded")).records) {
      auto payload = Json::parse(record.payload);
      for (auto* object : {&payload["state"], &payload["snapshot"]}) {
        object->erase("day_notes"); object->erase("trade_reviews"); object->erase("strategy_reviews");
        for (auto key : {"fills", "recent_fills"})
          if (object->contains(key)) for (auto& fill : (*object)[key]) fill.erase("context");
      }
      for (auto& valuation : payload["state"]["valuations"]) valuation.erase("spot_source");
      old->append(record.time, record.type, payload.dump());
    }
  }
  auto recovered = TradingSession::recover(FileJournal::read(directory.file("old")));
  EXPECT_FALSE(recovered.snapshot()->recent_fills.front().context);
  EXPECT_TRUE(recovered.snapshot()->trade_reviews.empty()); EXPECT_TRUE(recovered.snapshot()->day_notes.empty());
  mark(recovered, f, "5.00", "5.20");
  EXPECT_TRUE(recovered.snapshot()->trade_reviews.empty());
}

TEST(TradeReview, RecoveryContinuesIdenticallyAcrossScaleInsAndReversals) {
  ReviewDirectory directory;
  ScriptedMarket f;
  TradingSession s({}, f.time, test::create_journal(directory.file("journal"))); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  mark(s, f, "5.00", "5.20", 5003);
  auto recovered = TradingSession::recover(FileJournal::read(directory.file("journal")));
  for (auto* session : {&s, &recovered}) {
    ASSERT_TRUE(session->submit(f.market("add"), f.time).decision.ok());
    ASSERT_TRUE(session->submit(f.market("reverse", 4, Side::Sell), f.time).decision.ok());
  }
  f.next();
  for (auto* session : {&s, &recovered}) session->on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot_json(), recovered.snapshot_json());
  EXPECT_EQ(lifecycles(s.snapshot()->recent_fills, {}, s.contracts()).front().basis, Money{});
  ASSERT_EQ(s.snapshot()->trade_reviews.size(), 2U);
  EXPECT_TRUE(s.snapshot()->trade_reviews.at("1").finished);
  EXPECT_FALSE(s.snapshot()->trade_reviews.at("3").finished);
  EXPECT_EQ(s.snapshot()->trade_reviews.at("3").best->pnl, dollars("189.35"));
}

TEST(TradeReview, UnderlyingStopsAndUnavailableSpotDoNotInventOptionRiskOrPrices) {
  ScriptedMarket f;
  TradingSession s({}, f.time); f.seed(s);
  auto order = f.market("stop");
  order.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Underlying, TriggerDirection::AtOrBelow, dollars("4900")}, {}},
                          ExitSpec{{}, dollars("5.00")}};
  ASSERT_TRUE(s.submit(order, f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->trade_reviews.at("1").planned_risk);
  f.next(); auto invalid = f.valuation(); invalid.valid = false;
  s.on_quotes({f.quote("2.00", "2.20")}, {invalid}, f.time);
  EXPECT_FALSE(s.snapshot()->trade_reviews.at("1").worst->spot);
  // A system close can execute without analytics; all unknown market fields stay null.
  f.next(); invalid.time = f.time;
  s.on_quotes({f.quote("5.00", "5.20")}, {invalid}, f.time);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 2U);
  const auto c = s.snapshot()->recent_fills.back().context;
  ASSERT_TRUE(c); EXPECT_FALSE(c->spot); EXPECT_TRUE(c->spot_source.empty()); EXPECT_FALSE(c->delta); EXPECT_FALSE(c->iv);
}

TEST(TradeReview, PartialAssignmentKeepsTheRestOfTheRoundTripOpen) {
  ScriptedMarket f;
  Fill fill{1, 1, f.symbol(), Side::Sell, 3, dollars("4.20"), dollars("1.95"), 1, f.time, f.time};
  const Closure partial{f.symbol(), -1, dollars("5"), f.time, ClosureKind::Assignment, 1};
  const auto lives = lifecycles({fill}, {partial}, {{f.symbol(), f.contract}});
  ASSERT_EQ(lives.size(), 1U);
  EXPECT_EQ(lives[0].quantity, -2); EXPECT_FALSE(lives[0].closed); EXPECT_EQ(lives[0].closed_contracts, 1);
  EXPECT_EQ(lives[0].gross, dollars("-80")); EXPECT_EQ(lives[0].basis, dollars("-840"));
}

}  // namespace
}  // namespace openport::trading
