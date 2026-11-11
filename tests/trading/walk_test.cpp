#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "support/scripted_market.hpp"
#include "support/journal.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig settings(std::int64_t percent = 0) {
  SessionConfig c;
  c.rules.inside_fill_percent = percent;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.max_quote_age = 60 * md::kNanosPerSecond;
  c.limits.max_valuation_age = 60 * md::kNanosPerSecond;
  return c;
}
Order latest(const TradingSession& s) { return s.snapshot()->recent_orders.back(); }
void quote(TradingSession& s, ScriptedMarket& f, Timestamp time, std::string_view bid = "4.00",
           std::string_view ask = "4.40", Quantity size = 10, bool fresh = true) {
  f.time = time;
  if (fresh) ++f.observation;
  s.on_quotes({f.quote(bid, ask, size)}, {f.valuation()}, time);
}
OrderRequest walking(const ScriptedMarket& f, Side side = Side::Buy) {
  auto r = f.limit("walk", 1, side == Side::Buy ? "4.00" : "4.40", side);
  r.walk = Walk{m("0.10"), 10, m(side == Side::Buy ? "4.30" : "4.10")};
  return r;
}

TEST(InsideFills, ThresholdBothSidesDefaultAndMarketablePrices) {
  for (const auto percent : {0, 50, 75, 100}) {
    ScriptedMarket f;
    TradingSession s(settings(percent), f.time);
    f.seed(s, "4.00", "4.40");
    ASSERT_TRUE(s.submit(f.limit("mid", 1, "4.20"), f.time).decision.ok());
    EXPECT_EQ(latest(s).filled_quantity, percent == 50 ? 1 : 0);
    ASSERT_TRUE(s.submit(f.limit("sell", 1, "4.20", Side::Sell), f.time).decision.ok());
    EXPECT_EQ(latest(s).filled_quantity, percent == 50 ? 1 : 0);
    ASSERT_TRUE(s.submit(f.limit("below", 1, "4.10"), f.time).decision.ok());
    EXPECT_EQ(latest(s).filled_quantity, 0);
    ASSERT_TRUE(s.submit(f.limit("above", 1, "4.30", Side::Sell), f.time).decision.ok());
    EXPECT_EQ(latest(s).filled_quantity, 0);
    ASSERT_TRUE(s.submit(f.limit("cross", 1, "4.60"), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.40"));
    ASSERT_TRUE(s.submit(f.limit("cross-sell", 1, "3.90", Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.00"));
    ASSERT_TRUE(s.submit(f.market("market"), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.40"));
  }
}

TEST(InsideFills, SlippageImpactLatencyAndFeesKeepDisplayedBudget) {
  ScriptedMarket f;
  auto c = settings(50);
  c.rules.slippage_ticks = 2;
  c.rules.impact_ticks = 1;
  c.rules.fill_latency_ms = 1000;
  c.rules.fees = FeeSchedule{m("1.00"), {}, m("10"), m("0.10"), m("0.02"), {}, {}};
  TradingSession s(c, f.time);
  f.seed(s, "4.00", "4.40", 2);
  const auto start = f.time;
  const auto r = f.limit("mid", 5, "4.20");
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  quote(s, f, start + md::kNanosPerSecond, "4.00", "4.40", 2, false);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 1U);
  const auto fill = s.snapshot()->recent_fills.front();
  EXPECT_EQ(fill.price, m("4.20"));
  EXPECT_EQ(fill.quantity, 2);
  ASSERT_TRUE(fill.quote);
  EXPECT_EQ(fill.quote->left, 2);
  ASSERT_TRUE(fill.fees);
  EXPECT_EQ(fill.fee, m("2.24"));
  EXPECT_EQ(s.snapshot()->waiting.at(1).code, "DISPLAYED_SIZE");
  quote(s, f, start + 2 * md::kNanosPerSecond, "4.00", "4.40", 2, false);
  EXPECT_EQ(latest(s).filled_quantity, 2);
  quote(s, f, start + 3 * md::kNanosPerSecond, "4.00", "4.40", 2);
  EXPECT_EQ(latest(s).filled_quantity, 4);
  s.cancel(1, f.time);
  auto stop = f.limit("stop", 1, "4.20");
  stop.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("4.00")};
  ASSERT_TRUE(s.submit(stop, f.time).decision.ok());
  quote(s, f, f.time + md::kNanosPerSecond);
  EXPECT_EQ(latest(s).filled_quantity, 0);
}

TEST(InsideFills, CombosAllocateExactNetAndConsumeEachLegInRatio) {
  for (const auto credit : {false, true}) {
    ScriptedMarket a, b;
    b.contract = *md::parse_osi("SPXW261022C05010000");
    auto c = settings(50);
    c.rules.impact_ticks = 1;
    c.rules.slippage_ticks = 1;
    TradingSession s(c, a.time);
    a.seed(s, "4.00", "4.40", 3);
    b.seed(s, "2.00", "2.20", 2);
    OrderRequest r;
    r.client_order_id = "combo";
    r.quantity = 3;
    r.legs = {{a.symbol(), credit ? Side::Sell : Side::Buy, 1}, {b.symbol(), credit ? Side::Buy : Side::Sell, 1}};
    r.limit_price = m(credit ? "-2.10" : "2.10");
    ASSERT_TRUE(s.submit(r, a.time).decision.ok());
    EXPECT_EQ(latest(s).filled_quantity, 2);
    EXPECT_EQ(latest(s).filled_notional, *r.limit_price * 2);
    ASSERT_EQ(s.snapshot()->recent_fills.size(), 2U);
    for (const auto& fill : s.snapshot()->recent_fills) {
      ASSERT_TRUE(fill.quote);
      EXPECT_GT(fill.price, *fill.quote->bid);
      EXPECT_LT(fill.price, *fill.quote->ask);
    }
    EXPECT_EQ(s.snapshot()->waiting.at(1).code, "DISPLAYED_SIZE");
  }
}

TEST(InsideFills, RatioRoundingImprovesTheNetAndKeepsWholeUnitsWithinSize) {
  ScriptedMarket a, b;
  b.contract = *md::parse_osi("SPXW261022C05010000");
  TradingSession s(settings(40), a.time);
  a.seed(s, "4.00", "4.40", 5);
  b.seed(s, "2.00", "2.20", 7);
  OrderRequest r;
  r.client_order_id = "ratio";
  r.quantity = 3;
  r.legs = {{a.symbol(), Side::Buy, 2}, {b.symbol(), Side::Sell, 3}};
  r.limit_price = m("2.05");
  ASSERT_TRUE(s.submit(r, a.time).decision.ok());
  EXPECT_EQ(latest(s).filled_quantity, 2);
  EXPECT_EQ(latest(s).filled_notional, m("2.049999") * 2);
  ASSERT_EQ(s.snapshot()->recent_fills.size(), 2U);
  EXPECT_EQ(s.snapshot()->recent_fills[0].quantity, 4);
  EXPECT_EQ(s.snapshot()->recent_fills[1].quantity, 6);
  EXPECT_EQ(s.snapshot()->waiting.at(1).code, "DISPLAYED_SIZE");
}

TEST(WalkingLimits, EveryScheduledStepIsRecordedAndCapRests) {
  for (const auto side : {Side::Buy, Side::Sell}) {
    ScriptedMarket f;
    TradingSession s(settings(), f.time);
    f.seed(s, "4.00", "4.40");
    const auto start = f.time;
    const auto r = walking(f, side);
    const auto preview = s.preview(r, start);
    ASSERT_TRUE(preview.next_walk);
    EXPECT_EQ(preview.next_walk->time, start + 10 * md::kNanosPerSecond);
    ASSERT_TRUE(s.submit(r, start).decision.ok());
    quote(s, f, start + 9 * md::kNanosPerSecond);
    EXPECT_TRUE(latest(s).changes.empty());
    quote(s, f, start + 25 * md::kNanosPerSecond);
    ASSERT_EQ(latest(s).changes.size(), 2U);
    EXPECT_EQ(latest(s).changes[0].actor, "walk");
    EXPECT_EQ(latest(s).changes[0].time, start + 10 * md::kNanosPerSecond);
    EXPECT_EQ(latest(s).request.limit_price, m("4.20"));
    quote(s, f, start + 40 * md::kNanosPerSecond);
    ASSERT_EQ(latest(s).changes.size(), 3U);
    EXPECT_EQ(latest(s).request.limit_price, r.walk->limit);
    EXPECT_FALSE(next_walk(latest(s)));
    EXPECT_EQ(latest(s).status, OrderStatus::Working);
    quote(s, f, start + 41 * md::kNanosPerSecond, "4.10", "4.30");
    EXPECT_EQ(latest(s).status, OrderStatus::Filled);
    EXPECT_TRUE(s.submit(r, f.time).replayed);
  }
}

TEST(WalkingLimits, ClockStepsFillFreshBooksAndNewQuotesReplaceOldBooksFirst) {
  ScriptedMarket f;
  TradingSession s(settings(), f.time);
  f.seed(s, "4.00", "4.20");
  auto r = walking(f);
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  s.on_quotes({}, {}, f.time + 20 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).status, OrderStatus::Filled);
  ScriptedMarket other;
  TradingSession moved(settings(), other.time);
  other.seed(moved, "4.00", "4.20");
  ASSERT_TRUE(moved.submit(walking(other), other.time).decision.ok());
  quote(moved, other, other.time + 20 * md::kNanosPerSecond, "4.30", "4.50");
  EXPECT_EQ(latest(moved).status, OrderStatus::Working);
}

TEST(WalkingLimits, AWalkThatFillsAheadOfAnotherCommandStillPlacesItsThen) {
  for (const bool revised : {true, false}) {
    ScriptedMarket f;
    TradingSession s(settings(), f.time);
    s.set_revised_fills(revised);
    f.seed(s, "4.00", "4.20");
    auto r = walking(f);
    r.then = {f.limit("", 1, "4.60", Side::Sell, TimeInForce::Gtc)};
    ASSERT_TRUE(s.submit(r, f.time).decision.ok());
    // Another command 25 seconds on, before any new quote: the walk's steps reach the
    // ask, 4.20, and the order fills ahead of that command.
    ASSERT_TRUE(s.define(f.contract, f.time + 25 * md::kNanosPerSecond).decision.ok());
    const auto parent = s.snapshot()->recent_orders.front();
    EXPECT_EQ(parent.status, OrderStatus::Filled);
    if (revised) {
      ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
      EXPECT_EQ(parent.chained, 2U);
      EXPECT_EQ(latest(s).request.client_order_id, "walk:then");
      EXPECT_EQ(latest(s).chained_from, 1U);
      EXPECT_EQ(latest(s).status, OrderStatus::Working);
    } else {
      // Replay drivers 1-7 never placed it, not even on later quotes.
      quote(s, f, f.time + 30 * md::kNanosPerSecond, "4.00", "4.20");
      EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
      EXPECT_EQ(s.snapshot()->recent_orders.front().chained, 0U);
    }
  }
}

TEST(WalkingLimits, ChangesRestartIntervalAndMayChangeOrRemoveWalk) {
  ScriptedMarket f;
  TradingSession s(settings(), f.time);
  f.seed(s, "4.00", "4.40");
  const auto start = f.time;
  ASSERT_TRUE(s.submit(walking(f), start).decision.ok());
  OrderChange change;
  change.limit_price = m("4.10");
  ASSERT_TRUE(s.modify(1, change, start + 8 * md::kNanosPerSecond).decision.ok());
  quote(s, f, start + 17 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).request.limit_price, m("4.10"));
  quote(s, f, start + 18 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).request.limit_price, m("4.20"));
  change = {};
  change.walk.emplace(Walk{m("0.10"), 5, m("4.30")});
  ASSERT_TRUE(s.modify(1, change, f.time).decision.ok());
  EXPECT_EQ(next_walk(latest(s))->time, f.time + 5 * md::kNanosPerSecond);
  const auto scheduled = next_walk(latest(s))->time;
  change.walk.emplace(Walk{m("0.10"), 1, m("4.00")});
  EXPECT_EQ(s.modify(1, change, f.time + md::kNanosPerSecond).decision.code, Reason::INVALID_ORDER);
  EXPECT_EQ(next_walk(latest(s))->time, scheduled);
  f.time += md::kNanosPerSecond;
  change.walk.emplace(std::nullopt);
  ASSERT_TRUE(s.modify(1, change, f.time).decision.ok());
  EXPECT_FALSE(latest(s).request.walk);
  quote(s, f, f.time + 30 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).request.limit_price, m("4.20"));
  change.walk.emplace(Walk{m("0.10"), 5, m("4.30")});
  ASSERT_TRUE(s.modify(1, change, f.time).decision.ok());
  ASSERT_TRUE(s.cancel(1, f.time).decision.ok());
  const auto count = latest(s).changes.size();
  quote(s, f, f.time + 30 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).changes.size(), count);
}

TEST(WalkingLimits, TierTicksAndValidationAndBuyingPowerAtCap) {
  ScriptedMarket f;
  TradingSession s(settings(), f.time);
  f.seed(s, "2.50", "3.50");
  auto r = f.limit("tier", 1, "2.90");
  r.walk = Walk{m("0.05"), 1, m("3.20")};
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.reserved, m("320.65"));
  const auto start = f.time;
  for (const auto price : {"2.95", "3.00", "3.10", "3.20"}) {
    quote(s, f, f.time + md::kNanosPerSecond, "2.50", "3.50");
    EXPECT_EQ(latest(s).request.limit_price, m(price));
  }
  EXPECT_EQ(latest(s).walked_at, start + 4 * md::kNanosPerSecond);
  int n = 0;
  const auto check = [&](auto edit, Reason code) {
    auto bad = r;
    bad.client_order_id = "bad-" + std::to_string(++n);
    edit(bad);
    EXPECT_EQ(s.submit(bad, f.time).decision.code, code);
  };
  check([](auto& o) { o.tif = TimeInForce::Ioc; }, Reason::INVALID_ORDER);
  check([](auto& o) { o.tif = TimeInForce::Exto; }, Reason::INVALID_ORDER);
  check([](auto& o) { o.type = OrderType::Market; o.limit_price.reset(); o.tif = TimeInForce::Ioc; }, Reason::INVALID_ORDER);
  check([](auto& o) { o.walk->step = {}; }, Reason::INVALID_ORDER);
  check([](auto& o) { o.walk->step = m("0.03"); }, Reason::INVALID_TICK);
  check([](auto& o) { o.walk->limit = m("3.25"); }, Reason::INVALID_TICK);
  check([](auto& o) { o.walk->limit = m("2.80"); }, Reason::INVALID_ORDER);
  check([](auto& o) { o.walk->limit = m("100.00"); }, Reason::INVALID_ORDER);
  check([](auto& o) { o.walk->seconds = 0; }, Reason::INVALID_ORDER);
  check([](auto& o) { o.walk->seconds = 3601; }, Reason::INVALID_ORDER);
  check([](auto& o) { o.walk->limit = m("10.00"); }, Reason::PRICE_BAND);
  check([](auto& o) { o.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("4.00")}; }, Reason::INVALID_ORDER);
}

TEST(WalkingLimits, RecoveryContinuesScheduleAndRepeatedRunsHaveIdenticalJournals) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-walk-" + std::to_string(md::now()));
  std::filesystem::create_directories(directory);
  std::string first;
  for (int run = 0; run < 2; ++run) {
    const auto path = (directory / (std::to_string(run) + ".jsonl")).string();
    ScriptedMarket f;
    const auto start = f.time;
    std::string snapshot;
    {
      TradingSession s(settings(75), start, test::create_journal(path));
      f.seed(s, "4.00", "4.40");
      ASSERT_TRUE(s.submit(walking(f), start).decision.ok());
      quote(s, f, start + 10 * md::kNanosPerSecond);
      OrderChange change;
      change.walk.emplace(std::nullopt);
      ASSERT_TRUE(s.modify(1, change, f.time).decision.ok());
      change.walk.emplace(Walk{m("0.10"), 10, m("4.30")});
      ASSERT_TRUE(s.modify(1, change, f.time).decision.ok());
      snapshot = s.snapshot_json();
    }
    {
      auto s = TradingSession::recover(FileJournal::read(path), test::resume_journal(path));
      EXPECT_EQ(s.snapshot_json(), snapshot);
      quote(s, f, start + 30 * md::kNanosPerSecond);
      EXPECT_EQ(latest(s).status, OrderStatus::Filled);
      EXPECT_EQ(s.snapshot()->recent_fills.back().price, m("4.30"));
    }
    std::ifstream input(path);
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    if (run == 0) first = bytes;
    else { EXPECT_EQ(bytes, first); }
  }
  std::filesystem::remove_all(directory);
}

TEST(WalkingLimits, CreditCombosWalkTowardZeroAndFillTogether) {
  ScriptedMarket a, b;
  b.contract = *md::parse_osi("SPXW261022C05010000");
  TradingSession s(settings(), a.time);
  a.seed(s, "4.00", "4.40");
  b.seed(s, "2.00", "2.20");
  OrderRequest r;
  r.client_order_id = "credit-walk";
  r.quantity = 1;
  r.legs = {{a.symbol(), Side::Sell, 1}, {b.symbol(), Side::Buy, 1}};
  r.limit_price = m("-2.10");
  r.walk = Walk{m("0.10"), 5, m("-1.80")};
  ASSERT_TRUE(s.submit(r, a.time).decision.ok());
  s.on_quotes({}, {}, a.time + 10 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).request.limit_price, m("-1.90"));
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  s.on_quotes({}, {}, a.time + 15 * md::kNanosPerSecond);
  EXPECT_EQ(latest(s).status, OrderStatus::Filled);
  EXPECT_EQ(latest(s).filled_notional, m("-1.80"));
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 2U);
}

TEST(WalkingLimits, PartialsKeepWalkingAndBracketExitsDoNotInheritWalks) {
  ScriptedMarket f;
  TradingSession s(settings(50), f.time);
  f.seed(s, "4.00", "4.40", 1);
  auto r = walking(f);
  r.quantity = 2;
  r.bracket = Bracket{std::nullopt, ExitSpec{std::nullopt, m("4.60")}};
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  const auto start = f.time;
  quote(s, f, start + 20 * md::kNanosPerSecond, "4.00", "4.40", 1);
  const auto entry = s.snapshot()->recent_orders.front();
  EXPECT_EQ(entry.status, OrderStatus::PartiallyFilled);
  ASSERT_GT(entry.take_profit, 0U);
  const auto exit = s.snapshot()->recent_orders.at(entry.take_profit - 1);
  EXPECT_FALSE(exit.request.walk);
  EXPECT_EQ(exit.request.limit_price, m("4.60"));
  OrderChange change;
  change.walk.emplace(Walk{m("0.10"), 1, m("4.40")});
  EXPECT_EQ(s.modify(exit.id, change, f.time).decision.code, Reason::INVALID_ORDER);
  quote(s, f, start + 30 * md::kNanosPerSecond, "4.00", "4.40", 1);
  EXPECT_EQ(s.snapshot()->recent_orders.front().status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders.front().request.limit_price, m("4.30"));
}

TEST(InsideFills, RiskProjectionAndQuoteValidityStillGateFills) {
  ScriptedMarket f;
  auto c = settings(50);
  c.rules.fill_latency_ms = 1000;
  c.limits.max_daily_loss = m("0.10");
  TradingSession s(c, f.time);
  f.seed(s, "4.00", "4.40");
  ASSERT_TRUE(s.submit(f.limit("risk", 1, "4.30"), f.time).decision.ok());
  quote(s, f, f.time + md::kNanosPerSecond, "4.00", "4.40", 0);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  quote(s, f, f.time + md::kNanosPerSecond);
  EXPECT_TRUE(s.snapshot()->recent_fills.empty());
  EXPECT_EQ(latest(s).reason.code, Reason::RISK_CHANGED);
}
}  // namespace
}  // namespace openport::trading
