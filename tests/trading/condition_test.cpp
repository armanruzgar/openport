#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

// F43: orders conditional on another underlying, a study or the time of day.
namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy() {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  return c;
}
Trigger watching(std::string symbol, std::string study, TriggerDirection direction, std::string_view level) {
  Trigger t{study.empty() ? TriggerSource::Underlying : TriggerSource::Study, direction, m(level)};
  t.symbol = std::move(symbol);
  t.study = std::move(study);
  return t;
}
Trigger at(int hour, int minute) {
  Trigger t{TriggerSource::Time, TriggerDirection::AtOrAbove, {}};
  t.minute = hour * 60 + minute;
  return t;
}
/// One more second of the same quote, with the given indicators.
void tick(TradingSession& s, ScriptedMarket& f, const std::vector<Indicator>& indicators = {}) {
  f.next();
  s.on_quotes({f.quote()}, {f.valuation()}, f.time, {}, indicators);
}
const Order& order(const TradingSession& s, OrderId id) { return s.snapshot()->recent_orders.at(static_cast<std::size_t>(id - 1)); }

TEST(TradingConditions, AnOrderOnOneUnderlyingWaitsForAnotherUnderlyingsFreshPrice) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto request = f.market("vix-spike");
  request.trigger = watching("VIX", "", TriggerDirection::AtOrAbove, "20");
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);  // nothing known about VIX yet
  tick(s, f, {{"VIX", "", f.time + md::kNanosPerSecond, m("19.99")}});
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  // The order's own underlying is not VIX: its spot of 5,000 does not count.
  tick(s, f, {{"SPX", "", f.time + md::kNanosPerSecond, m("25")}});
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  // A value older than the valuation age never triggers.
  f.time += 2 * md::kNanosPerMinute;
  tick(s, f, {{"VIX", "", f.time - 61 * md::kNanosPerSecond, m("21")}});
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  tick(s, f, {{"VIX", "", f.time + md::kNanosPerSecond, m("20")}});
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 1).triggered_at, f.time);
}

TEST(TradingConditions, AStudyTriggerReadsTheOrdersOwnUnderlyingUnlessItNamesAnother) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  auto own = f.limit("calm", 1, "4.20");
  own.trigger = watching("", "iv30", TriggerDirection::AtOrBelow, "15");
  auto other = f.limit("inverted", 1, "4.20");
  other.trigger = watching("QQQ", "term_ratio", TriggerDirection::AtOrAbove, "1");
  ASSERT_TRUE(s.submit(own, f.time).decision.ok());
  ASSERT_TRUE(s.submit(other, f.time).decision.ok());
  tick(s, f, {{"SPX", "iv30", f.time + md::kNanosPerSecond, m("15.5")}, {"QQQ", "term_ratio", f.time + md::kNanosPerSecond, m("0.98")}});
  EXPECT_EQ(order(s, 1).status, OrderStatus::Armed);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  tick(s, f, {{"SPX", "iv30", f.time + md::kNanosPerSecond, m("14.9")}, {"SPX", "term_ratio", f.time + md::kNanosPerSecond, m("1.2")}});
  EXPECT_EQ(order(s, 1).status, OrderStatus::Filled);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  tick(s, f, {{"QQQ", "term_ratio", f.time + md::kNanosPerSecond, m("1.01")}});
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  // One value per indicator per batch; never one from the future.
  f.next();
  EXPECT_THROW(s.on_quotes({}, {}, f.time, {}, {{"QQQ", "", f.time, m("1")}, {"QQQ", "", f.time, m("2")}}), TradingError);
  EXPECT_THROW(s.on_quotes({}, {}, f.time, {}, {{"QQQ", "", f.time + 1, m("1")}}), TradingError);
}

TEST(TradingConditions, ATimeTriggerClosesAtANewYorkMinuteAndFiresAtOnceWhenItHasPassed) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  auto close = f.market("close-1530", 2, Side::Sell);
  close.trigger = at(15, 30);
  ASSERT_TRUE(s.submit(close, f.time).decision.ok());
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  f.time = md::new_york_to_utc({2026, 9, 22}, 15, 29) + 58 * md::kNanosPerSecond;  // the tick makes it 15:29:59
  tick(s, f);
  EXPECT_EQ(order(s, 2).status, OrderStatus::Armed);
  tick(s, f);  // 15:30:00
  EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  // Placed after its time, it is reached at once; at_or_below waits for the next morning.
  auto late = f.limit("late", 1, "4.20");
  late.trigger = at(15, 0);
  const auto warned = s.preview(late, f.time).warnings;
  ASSERT_EQ(warned.size(), 1U);
  EXPECT_EQ(warned.front().message, "The trigger, at or after 15:00 New York time, is already reached: the order activates at once");
  ASSERT_TRUE(s.submit(late, f.time).decision.ok());
  EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);
  auto morning = f.limit("morning", 1, "4.20", Side::Buy, TimeInForce::Gtc);
  morning.trigger = at(10, 0);
  morning.trigger->direction = TriggerDirection::AtOrBelow;
  ASSERT_TRUE(s.submit(morning, f.time).decision.ok());
  EXPECT_EQ(order(s, 4).status, OrderStatus::Armed);
  f.time = md::new_york_to_utc({2026, 9, 23}, 9, 45);
  tick(s, f);
  EXPECT_EQ(order(s, 4).status, OrderStatus::Filled);
}

TEST(TradingConditions, ConditionalTermsAreCheckedAndExitsWatchOnlyTheirOwnMarket) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto refused = [&](std::string client, Trigger t, bool exit = false) {
    auto request = f.limit(std::move(client), 1, "4.20");
    if (exit) request.bracket = Bracket{ExitSpec{t, {}}, {}};
    else request.trigger = t;
    return s.submit(request, f.time).decision.code == Reason::INVALID_ORDER;
  };
  EXPECT_TRUE(refused("study", watching("SPX", "iv_rank", TriggerDirection::AtOrAbove, "1")));
  EXPECT_TRUE(refused("zero", watching("VIX", "", TriggerDirection::AtOrAbove, "0")));
  EXPECT_TRUE(refused("lower", watching("vix", "", TriggerDirection::AtOrAbove, "20")));
  auto leveled = at(15, 30);
  leveled.level = m("1");
  EXPECT_TRUE(refused("leveled", leveled));
  auto midnight = at(24, 0);
  EXPECT_TRUE(refused("midnight", midnight));
  EXPECT_TRUE(refused("exit-time", at(15, 30), true));
  EXPECT_TRUE(refused("exit-vix", watching("VIX", "", TriggerDirection::AtOrAbove, "20"), true));
  // A time trigger has no level to change.
  auto timed = f.limit("timed", 1, "4.20");
  timed.trigger = at(15, 30);
  const auto id = s.submit(timed, f.time).order_id;
  ASSERT_TRUE(id);
  EXPECT_EQ(s.modify(*id, OrderChange{{}, {}, m("1")}, f.time).decision.code, Reason::INVALID_ORDER);
}

TEST(TradingConditions, IndicatorsAreJournaledOnlyOnceUsedAndRecoverWithTheAccount) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-conditions-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("plain", 1, "4.20"), f.time).decision.ok());
    {
      std::ifstream in(path);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(text.find("indicators"), std::string::npos);
      EXPECT_EQ(text.find("\"minute\""), std::string::npos);
    }
    auto request = f.market("vix", 1, Side::Sell);
    request.trigger = watching("VIX", "", TriggerDirection::AtOrAbove, "30");
    ASSERT_TRUE(s.submit(request, f.time).decision.ok());
    tick(s, f, {{"VIX", "", f.time + md::kNanosPerSecond, m("31")}});
    EXPECT_EQ(order(s, 2).status, OrderStatus::Filled);
    auto again = f.market("vix-again", 1);
    again.trigger = watching("VIX", "", TriggerDirection::AtOrAbove, "30");
    ASSERT_TRUE(s.submit(again, f.time).decision.ok());
    EXPECT_EQ(order(s, 3).status, OrderStatus::Filled);  // the kept value is still fresh
    tick(s, f, {{"VIX", "", f.time + md::kNanosPerSecond, m("29")}});
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  // The kept value recovers with the account: a new order reads it at submission.
  auto low = f.market("vix-low", 1, Side::Sell);
  low.trigger = watching("VIX", "", TriggerDirection::AtOrBelow, "29.50");
  ASSERT_TRUE(recovered.submit(low, f.time).decision.ok());
  EXPECT_EQ(order(recovered, 4).status, OrderStatus::Filled);
  // A minute later it is stale, and the next order waits for a fresh one.
  auto stale = f.market("vix-stale", 1);
  stale.trigger = watching("VIX", "", TriggerDirection::AtOrBelow, "29.50");
  f.time += 60 * md::kNanosPerSecond;
  tick(recovered, f);
  ASSERT_TRUE(recovered.submit(stale, f.time).decision.ok());
  EXPECT_EQ(order(recovered, 5).status, OrderStatus::Armed);
  tick(recovered, f, {{"VIX", "", f.time + md::kNanosPerSecond, m("29.5")}});
  EXPECT_EQ(order(recovered, 5).status, OrderStatus::Filled);
  std::filesystem::remove_all(directory);
}

}  // namespace
}  // namespace openport::trading
