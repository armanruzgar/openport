#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <set>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
AccountRules queued(std::int64_t seed = 0) {
  AccountRules rules;
  rules.queue_fills = true;
  rules.queue_seed = seed;
  return rules;
}
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-fill-models-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};
const Order& order(const TradingSession& session, OrderId id = 1) { return session.snapshot()->recent_orders.at(id - 1); }
using Prints = std::vector<std::pair<std::string_view, Quantity>>;
/// A new quote one second later, with this batch's trade prints.
void prints(TradingSession& session, ScriptedMarket& market, const Prints& trades, std::string_view bid = "4.00",
            std::string_view ask = "4.20", Quantity size = 10) {
  market.next();
  MarketActivity activity;
  activity.prints_supplied = true;
  for (const auto& [price, size_traded] : trades)
    activity.prints.push_back({market.symbol(), market.time, m(price), size_traded, "C"});
  session.on_quotes({market.quote(bid, ask, size)}, {market.valuation()}, market.time, {}, {}, activity);
}
std::string wait_code(const TradingSession& session, OrderId id = 1) {
  const auto& waiting = session.snapshot()->waiting;
  const auto it = waiting.find(id);
  return it == waiting.end() ? "" : it->second.code;
}

TEST(QueueFills, PrintsUseUpTheQueueThenFillAtTheLimitAndTheTouchStillCrosses) {
  ScriptedMarket market;
  TradingSession session(config(queued()), market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.limit("join", 5, "4.00"), market.time).decision.ok());
  ASSERT_TRUE(order(session).queue);
  EXPECT_EQ(order(session).queue->ahead, 10);
  EXPECT_EQ(order(session).queue->source, "joined");
  EXPECT_EQ(wait_code(session), "QUEUE");
  // Six trade at the bid: four are still ahead.
  prints(session, market, {{"4.00", 6}, {"4.10", 3}}, "4.00", "4.20", 4);
  EXPECT_EQ(order(session).queue->ahead, 4);
  EXPECT_EQ(order(session).queue->source, "prints");
  EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  // Seven more: four finish the queue, three trade with the order at its limit.
  prints(session, market, {{"4.00", 7}});
  ASSERT_EQ(session.snapshot()->recent_fills.size(), 1U);
  const auto first = session.snapshot()->recent_fills.back();
  EXPECT_EQ(first.quantity, 3);
  EXPECT_EQ(first.price, m("4.00"));
  EXPECT_TRUE(first.queued);
  ASSERT_TRUE(first.quote);
  EXPECT_EQ(first.quote->left, 10) << "A queued fill takes nothing from the far side's displayed size";
  EXPECT_EQ(order(session).queue->ahead, 0);
  EXPECT_EQ(order(session).queue->earned, 0);
  // A print through the price fills it whatever was ahead.
  prints(session, market, {{"3.95", 1}}, "3.95", "4.15");
  EXPECT_EQ(order(session).filled_quantity, 4);
  // The far side reaching the limit fills the rest as before.
  prints(session, market, {}, "3.90", "4.00");
  EXPECT_EQ(order(session).status, OrderStatus::Filled);
  EXPECT_FALSE(session.snapshot()->recent_fills.back().queued);
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("4.00"));
}

TEST(QueueFills, OrdersBehindTheTouchJoinWhenItReachesThemAndOwnOrdersQueueInOrder) {
  ScriptedMarket market;
  TradingSession session(config(queued()), market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.limit("behind", 1, "3.90"), market.time).decision.ok());
  EXPECT_FALSE(order(session).queue);
  EXPECT_EQ(wait_code(session), "LIMIT");
  prints(session, market, {}, "3.90", "4.10", 4);
  ASSERT_TRUE(order(session).queue);
  EXPECT_EQ(order(session).queue->ahead, 4);
  ASSERT_TRUE(session.submit(market.limit("second", 3, "3.90"), market.time).decision.ok());
  EXPECT_EQ(order(session, 2).queue->ahead, 5) << "The account's own earlier order is ahead too";
  prints(session, market, {{"3.90", 5}}, "3.90", "4.10", 4);
  EXPECT_EQ(order(session).status, OrderStatus::Filled);
  EXPECT_EQ(order(session, 2).filled_quantity, 0);
  EXPECT_EQ(order(session, 2).queue->ahead, 0);
  prints(session, market, {{"3.90", 2}}, "3.90", "4.10", 4);
  EXPECT_EQ(order(session, 2).filled_quantity, 2);
  // A new price joins again behind what is displayed there.
  ASSERT_TRUE(session.modify(2, {.limit_price = m("3.80")}, market.time).decision.ok());
  prints(session, market, {}, "3.80", "4.10", 6);
  EXPECT_EQ(order(session, 2).queue->price, m("3.80"));
  EXPECT_EQ(order(session, 2).queue->ahead, 6);
}

TEST(QueueFills, VolumeIncreasesStandInForPrintsHalfOnEachSide) {
  ScriptedMarket market;
  TradingSession session(config(queued()), market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.limit("join", 2, "4.00"), market.time).decision.ok());
  const auto volume = [&](std::int64_t contracts) {
    market.next();
    MarketActivity activity;
    activity.volumes_supplied = true;
    activity.volumes[market.symbol()] = contracts;
    session.on_quotes({market.quote()}, {market.valuation()}, market.time, {}, {}, activity);
  };
  volume(100);
  EXPECT_EQ(order(session).queue->ahead, 10) << "The first reading is only a baseline";
  EXPECT_EQ(order(session).queue->volume, 100);
  volume(120);
  EXPECT_EQ(order(session).queue->ahead, 0);
  EXPECT_EQ(order(session).queue->source, "volume");
  volume(126);
  EXPECT_EQ(order(session).status, OrderStatus::Filled);
  EXPECT_TRUE(session.snapshot()->recent_fills.back().queued);
}

TEST(QueueFills, WithoutTradesTheVolumeRateHeuristicTurnsTheTouchOverInFiveMinutes) {
  ScriptedMarket market;
  TradingSession session(config(queued()), market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.limit("join", 1, "4.00"), market.time).decision.ok());
  const auto minute = [&] {
    ++market.observation;
    market.time += 60 * md::kNanosPerSecond;
    session.on_quotes({market.quote()}, {market.valuation()}, market.time);
  };
  for (int i = 0; i < 5; ++i) minute();
  EXPECT_EQ(order(session).queue->ahead, 0);
  EXPECT_EQ(order(session).queue->source, "rate");
  EXPECT_TRUE(session.snapshot()->recent_fills.empty());
  minute();
  EXPECT_EQ(order(session).status, OrderStatus::Filled);
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("4.00"));
}

TEST(QueueFills, TheSeedDecidesFractionsAndTheSameFeedAlwaysGivesTheSameAccount) {
  const auto run = [](std::int64_t seed, const std::string& path = {}) {
    ScriptedMarket market;
    std::shared_ptr<Journal> journal;
    if (!path.empty()) journal = FileJournal::create(path);
    auto session = std::make_unique<TradingSession>(config(queued(seed)), market.time, journal);
    market.seed(*session);
    EXPECT_TRUE(session->submit(market.limit("join", 1, "4.00"), market.time).decision.ok());
    // 45 seconds of a ten-lot is a turnover of 1.5 contracts; cancellations shrink the queue too.
    for (const Quantity size : {10, 7, 7, 3}) {
      ++market.observation;
      market.time += 45 * md::kNanosPerSecond;
      session->on_quotes({market.quote("4.00", "4.20", size)}, {market.valuation()}, market.time);
    }
    return session;
  };
  std::set<Quantity> ahead;
  for (std::int64_t seed = 0; seed < 16; ++seed) {
    const auto a = run(seed);
    EXPECT_EQ(a->snapshot_json(), run(seed)->snapshot_json());
    ahead.insert(order(*a).queue ? order(*a).queue->ahead : -1);
  }
  EXPECT_GT(ahead.size(), 1U) << "Rounding at random depends on the seed";
  JournalFile file;
  const auto live = run(7, file.path);
  const auto recovered = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(recovered.snapshot_json(), live->snapshot_json());
  EXPECT_EQ(recovered.config().rules.queue_seed, 7);
}

std::string bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(QueueFills, OffByDefaultSoTradesChangeNoJournalByte) {
  const auto run = [](bool activity) {
    JournalFile file;
    ScriptedMarket market;
    TradingSession session(config(), market.time, FileJournal::create(file.path));
    market.seed(session);
    EXPECT_TRUE(session.submit(market.limit("rest", 1, "4.00"), market.time).decision.ok());
    market.next();
    MarketActivity trades;
    if (activity) {
      trades.prints_supplied = trades.volumes_supplied = true;
      trades.prints.push_back({market.symbol(), market.time, m("3.90"), 50});
      trades.volumes[market.symbol()] = 500;
    }
    session.on_quotes({market.quote()}, {market.valuation()}, market.time, {}, {}, trades);
    EXPECT_FALSE(order(session).queue);
    EXPECT_EQ(wait_code(session), "LIMIT");
    return bytes(file.path);
  };
  const auto plain = run(false);
  EXPECT_EQ(run(true), plain);
  EXPECT_EQ(plain.find("queue"), std::string::npos);
}

TEST(QueueFills, RulesAreValidatedOmittedAtDefaultsAndCountAsProgramRules) {
  const AccountRules defaults;
  const Json encoded = defaults;
  for (const auto* key : {"queue_fills", "queue_seed", "stop_stress_ticks", "stop_stress_percent"}) EXPECT_FALSE(encoded.contains(key));
  auto rules = queued(42);
  rules.stop_stress_ticks = 2;
  rules.stop_stress_percent = 300;
  const Json full = rules;
  EXPECT_EQ(full.at("queue_fills"), true);
  EXPECT_EQ(full.at("queue_seed"), 42);
  EXPECT_EQ(full.at("stop_stress_percent"), 300);
  EXPECT_EQ(full.get<AccountRules>(), rules);
  EXPECT_NO_THROW(validate_rules(rules));
  for (const auto& change : std::vector<std::function<void(AccountRules&)>>{
           [](auto& r) { r.queue_seed = -1; }, [](auto& r) { r.stop_stress_ticks = 11; },
           [](auto& r) { r.stop_stress_percent = 100; }, [](auto& r) { r.stop_stress_percent = 1001; }}) {
    auto bad = rules;
    change(bad);
    EXPECT_THROW(validate_rules(bad), TradingError);
  }
  auto corrupt = full;
  corrupt["queue_fills"] = 1;
  EXPECT_THROW((void)corrupt.get<AccountRules>(), TradingError);
  // Unlike latency, slippage, impact and inside fills, they decide whether and
  // where an order fills, so an account that changes them leaves its preset.
  for (const auto& change : std::vector<std::function<void(AccountRules&)>>{
           [](auto& r) { r.queue_fills = true; }, [](auto& r) { r.queue_seed = 3; },
           [](auto& r) { r.stop_stress_ticks = 1; }, [](auto& r) { r.stop_stress_percent = 150; }}) {
    auto changed = defaults;
    change(changed);
    EXPECT_FALSE(same_program_rules(defaults, changed));
  }
}

/// A long contract with an armed sell stop at 3.50, then eight ordinary quotes a
/// second apart with a 0.20 spread: the normal the stop's trigger is measured by.
TradingSession stopped(ScriptedMarket& market, std::int64_t ticks, int ordinary = 8, const std::string& journal = {}) {
  AccountRules rules;
  rules.stop_stress_ticks = ticks;
  TradingSession session(config(rules), market.time, journal.empty() ? nullptr : FileJournal::create(journal));
  market.seed(session);
  EXPECT_TRUE(session.submit(market.market("entry"), market.time).decision.ok());
  auto stop = market.market("stop", 1, Side::Sell);
  stop.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")};
  EXPECT_TRUE(session.submit(stop, market.time).decision.ok());
  for (int i = 0; i < ordinary; ++i) {
    market.next();
    session.on_quotes({market.quote()}, {market.valuation()}, market.time);
  }
  return session;
}

TEST(StopStress, AStopReachedOnAWideSpreadSlipsTheExtraTicks) {
  ScriptedMarket market;
  auto session = stopped(market, 2);
  market.next();
  session.on_quotes({market.quote("3.40", "3.80")}, {market.valuation()}, market.time);
  EXPECT_EQ(order(session, 2).status, OrderStatus::Filled);
  EXPECT_EQ(order(session, 2).stress_ticks, 2);
  const auto& fill = session.snapshot()->recent_fills.back();
  EXPECT_EQ(fill.price, m("3.20"));
  EXPECT_EQ(fill.stress_ticks, 2);
}

TEST(StopStress, AStopReachedInAnOrdinaryMarketDoesNot) {
  ScriptedMarket market;
  auto session = stopped(market, 2);
  market.next();
  session.on_quotes({market.quote("3.40", "3.60")}, {market.valuation()}, market.time);
  EXPECT_EQ(order(session, 2).stress_ticks, 0);
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("3.40"));
  EXPECT_EQ(session.snapshot()->recent_fills.back().stress_ticks, 0);
}

TEST(StopStress, QuotesChangingFasterThanNormalAlsoCount) {
  ScriptedMarket market;
  auto session = stopped(market, 1);
  ++market.observation;
  market.time += md::kNanosPerSecond / 4;
  session.on_quotes({market.quote("3.40", "3.60")}, {market.valuation()}, market.time);
  EXPECT_EQ(order(session, 2).stress_ticks, 1);
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("3.30"));
}

TEST(StopStress, NeedsEightQuotesOfNormalAndIsOffByDefault) {
  {
    ScriptedMarket market;
    auto session = stopped(market, 2, 5);
    market.next();
    session.on_quotes({market.quote("3.40", "3.80")}, {market.valuation()}, market.time);
    EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("3.40"));
  }
  JournalFile file;
  ScriptedMarket market;
  auto session = stopped(market, 0, 8, file.path);
  market.next();
  session.on_quotes({market.quote("3.40", "3.80")}, {market.valuation()}, market.time);
  EXPECT_EQ(session.snapshot()->recent_fills.back().price, m("3.40"));
  const auto journal = bytes(file.path);
  EXPECT_EQ(journal.find("activity"), std::string::npos);
  EXPECT_EQ(journal.find("stress"), std::string::npos);
}

}  // namespace
}  // namespace openport::trading
