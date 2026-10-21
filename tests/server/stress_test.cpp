#include "support/sanitizer.hpp"
#include "support/recording.hpp"

#include <fstream>
#include <map>
#include <optional>

#include "openport/providers/replay_batches.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/run.hpp"
#include "openport/trading/evaluation.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;

json script() {
  return {{"id", "stress"}, {"title", "Stress"}, {"description", "Authored stress windows."},
    {"symbols", {"SPX"}}, {"session", "regular"}, {"date", "2026-09-16"}, {"seed", 17},
    {"generator", 1}, {"drift", {{1, 0}}}, {"volatility", 0}, {"iv_shift", 0}, {"spot_vol", 0}};
}

struct Replay {
  test::RecordingFile file;
  providers::Scenario scenario;
  std::unique_ptr<md::RecordingReader> reader;
  std::unique_ptr<providers::ReplayBatches> batches;
  std::unique_ptr<server::Desk> desk;
  std::optional<providers::ReplayBatch> next;
  std::map<std::string, md::InstrumentId> ids;
  explicit Replay(const json& data, bool recording_input = false) {
    const auto source = file.directory / "stress.json";
    { std::ofstream out(source); out << data; }
    scenario = providers::read_scenario(source);
    providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
    reader = std::make_unique<md::RecordingReader>(file.path);
    server::Desk::Options options;
    options.replay = true;
    options.run_input = recording_input ? server::recording_input(file.path)
        : server::scenario_input(scenario, scenario.date, scenario.seed);
    options.paper_journal = file.directory / "run.jsonl";
    options.paper.initial_cash = Money::parse("1000000");
    options.paper.limits.max_quote_age = 30 * md::kNanosPerSecond;
    options.paper.limits.aggregate = {1e9, 1e9};
    options.paper.limits.per_underlying = {1e9, 1e9};
    options.analytics.deamericanize = false;
    desk = std::make_unique<server::Desk>("replay (demo)", reader->header().capabilities, reader->header().subscription, options);
    desk->start_trading();
    batches = std::make_unique<providers::ReplayBatches>(*reader, reader->header().subscription, true);
    next = batches->next();
  }
  md::Timestamp at(int hour, int minute) const { return md::new_york_to_utc(scenario.date, hour, minute); }
  void until(int hour, int minute) {
    const auto target = at(hour, minute);
    while (next && next->time <= target) {
      for (const auto& event : next->events)
        if (const auto* d = std::get_if<md::ContractDefinition>(&event)) ids[d->contract.osi_symbol()] = d->id;
      desk->replay_batch(next->events, next->received, next->time);
      next = batches->next();
    }
    EXPECT_EQ(desk->market_time(), target);
    ASSERT_TRUE(desk->trading_status().enabled) << desk->trading_status().reason;
  }
  server::TradingReply order(std::string id, std::string osi = "SPXW260918C06000000", trading::Side side = trading::Side::Buy) {
    server::TradingCommand request;
    request.actor = "test";
    request.order.client_order_id = std::move(id);
    request.order.symbol = md::parse_osi(osi)->osi_symbol();
    request.order.side = side;
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    std::optional<server::TradingReply> reply;
    desk->command(request, [&](auto result) { reply = std::move(result); }, desk->market_time(), desk->market_time());
    if (!reply) throw std::runtime_error("missing command reply");
    return *reply;
  }
  void verify() {
    desk->stop();
    desk.reset();
    const auto result = server::verify_run(file.directory / "run.jsonl");
    EXPECT_TRUE(result.matched) << result.message;
  }
};

TEST(StressReplay, SingleNameScenarioAndCommandHaltsKeepOtherNamesTradingAndVerify) {
  auto data = script(); data["symbols"] = {"SPX", "SPY"};
  data["events"] = json::array({{{"type", "halt"}, {"at", "09:32"}, {"minutes", 2}, {"symbols", {"SPY"}}}});
  for (const bool recorded : {false, true}) {
    Replay replay(data, recorded);
    replay.until(9, 32);
    EXPECT_TRUE(replay.order("index").decision.ok());
    EXPECT_EQ(replay.order("etf", "SPY260918C00600000").decision.code, trading::Reason::MARKET_HALTED);
    const auto view = replay.desk->trading_view();
    ASSERT_FALSE(view->halts.empty()); EXPECT_EQ(view->halts.back().underlying, "SPY");
    replay.until(9, 34);
    EXPECT_NE(replay.order("resumed", "SPY260918C00600000").decision.code, trading::Reason::MARKET_HALTED);
    server::TradingCommand halt; halt.kind = server::TradingCommand::Kind::Halt; halt.symbol = "SPY"; halt.quantity = 2;
    replay.desk->command(halt, [](const auto& reply) { EXPECT_TRUE(reply.decision.ok()) << reply.decision.message; },
                         replay.desk->market_time(), replay.desk->market_time());
    EXPECT_EQ(replay.order("drill", "SPY260918C00600000").decision.code, trading::Reason::MARKET_HALTED);
    EXPECT_TRUE(replay.order("index2").decision.ok());
    replay.until(9, 36);
    EXPECT_NE(replay.order("after", "SPY260918C00600000").decision.code, trading::Reason::MARKET_HALTED);
    replay.verify();
  }
}

TEST(StressReplay, AbnormalBooksStallAndHaltRecoverAndVerify) {
  auto data = script();
  data["symbols"] = {"SPX", "XSP"};
  data["events"] = json::array({
    {{"type", "book"}, {"at", "09:32"}, {"minutes", 1}, {"state", "crossed"}},
    {{"type", "book"}, {"at", "09:34"}, {"minutes", 1}, {"state", "zero_size"}},
    {{"type", "stall"}, {"at", "09:36"}, {"minutes", 4}, {"symbols", {"SPX"}}},
    {{"type", "halt"}, {"at", "09:42"}, {"minutes", 3}}});
  Replay replay(data);
  replay.until(9, 31);
  ASSERT_TRUE(replay.order("held").decision.ok());
  replay.until(9, 32);
  EXPECT_EQ(replay.order("crossed").decision.code, trading::Reason::INVALID_QUOTE);
  replay.until(9, 33);
  EXPECT_TRUE(replay.order("normal").decision.ok());
  replay.until(9, 34);
  EXPECT_EQ(replay.order("zero-size").decision.code, trading::Reason::INVALID_QUOTE);
  replay.until(9, 35);
  ASSERT_TRUE(replay.desk->trading_view()->snapshot->valuation_complete);
  replay.until(9, 36);
  const auto mark = replay.desk->trading_view()->snapshot->positions.front().mark;
  replay.until(9, 38);
  EXPECT_FALSE(replay.desk->trading_view()->snapshot->valuation_complete);
  const auto& flags = replay.desk->trading_view()->snapshot->quality_flags;
  EXPECT_NE(std::find(flags.begin(), flags.end(), trading::Reason::STALE_QUOTE), flags.end());
  EXPECT_EQ(replay.order("stalled").decision.code, trading::Reason::FEED_STALLED);
  EXPECT_EQ(replay.order("stale-holding", "XSP260918C00600000").decision.code, trading::Reason::STALE_QUOTE);
  EXPECT_EQ(replay.desk->trading_view()->snapshot->positions.front().mark, mark);
  replay.until(9, 40);
  EXPECT_TRUE(replay.desk->trading_view()->snapshot->valuation_complete);
  EXPECT_TRUE(replay.order("recovered").decision.ok());
  replay.until(9, 43);
  const auto halted = replay.order("halted").decision;
  EXPECT_EQ(halted.code, trading::Reason::MARKET_HALTED);
  EXPECT_EQ(halted.message, "Trading is halted market-wide by the scenario; it resumes at 09:45 ET");
  EXPECT_TRUE(replay.desk->breaker().active);
  replay.until(9, 45);
  EXPECT_FALSE(replay.desk->breaker().active);
  EXPECT_TRUE(replay.order("resumed").decision.ok());
  replay.verify();
}

TEST(StressReplay, AuthoredReferenceTripsRealBreakerLevelsAndRestOfDayClose) {
  auto data = script();
  data["previous_close"] = 0.08;
  data["events"] = json::array({
    {{"type", "spike"}, {"at", "09:50"}, {"move", -0.07}, {"iv", 0}},
    {{"type", "spike"}, {"at", "10:15"}, {"move", -0.10}, {"iv", 0}}});
  Replay replay(data);
  replay.until(9, 31);
  EXPECT_EQ(replay.desk->breaker().level, 1);
  EXPECT_EQ(replay.order("level1").decision.code, trading::Reason::MARKET_HALTED);
  replay.until(9, 46);
  EXPECT_TRUE(replay.order("resume1").decision.ok());
  replay.until(9, 55);
  EXPECT_EQ(replay.desk->breaker().level, 2);
  EXPECT_EQ(replay.order("level2").decision.code, trading::Reason::MARKET_HALTED);
  replay.until(10, 11);
  EXPECT_TRUE(replay.order("resume2", "SPX261016P05800000").decision.ok());
  replay.until(10, 20);
  EXPECT_EQ(replay.desk->breaker().level, 3);
  EXPECT_EQ(replay.order("level3").decision.code, trading::Reason::MARKET_HALTED);
  replay.until(10, 40);
  EXPECT_EQ(replay.order("still-level3").decision.code, trading::Reason::MARKET_HALTED);
  replay.verify();
}

TEST(StressReplay, AuthoredHaltMessagesVerifyWithDriverSix) {
  auto data = script();
  data["events"] = json::array({{{"type", "halt"}, {"at", "10:00"}, {"minutes", 30}}});
  for (const bool recording : {false, true}) {
    SCOPED_TRACE(recording ? "recording" : "scenario");
    Replay replay(data, recording);
    replay.until(10, 1);
    const auto halted = replay.order("halted").decision;
    EXPECT_EQ(halted.code, trading::Reason::MARKET_HALTED);
    EXPECT_EQ(halted.message, "Trading is halted market-wide by the scenario; it resumes at 10:30 ET");
    replay.verify();
    const auto inputs = server::run_inputs(trading::FileJournal::read((replay.file.directory / "run.jsonl").string()));
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(json::parse(inputs.front()).at("driver"), 6);
  }
}

TEST(StressReplay, AuthoredHaltMessagesUseNewYorkTimeAndTheRegularOrEarlyClose) {
  for (const md::Date date : {md::Date{2026, 9, 16}, md::Date{2026, 12, 1}, md::Date{2026, 11, 27}}) {
    const auto start = md::new_york_to_utc(date, 10, 0);
    const auto resume = md::new_york_to_utc(date, 10, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    const auto message = [&](md::Timestamp end) {
      // Frozen quotes and a delayed driver clock still show the authored halt.
      return server::paper_acceptance("SPX", start, start + 16 * md::kNanosPerMinute,
          std::chrono::minutes(15), 30 * md::kNanosPerSecond,
          {{0, start, end, 0, 0}, {1, start, resume, 5400, 5000}}).message;
    };
    EXPECT_EQ(message(resume), "Trading is halted market-wide by the scenario; it resumes at 10:30 ET");
    EXPECT_EQ(message(resume - md::kNanosPerMinute), "Trading is halted market-wide by the scenario; it resumes at 10:30 ET");
    EXPECT_EQ(message(close), "Trading is halted market-wide by the scenario for the rest of the day");
    EXPECT_EQ(message(close + md::kNanosPerMinute), "Trading is halted market-wide by the scenario for the rest of the day");
  }
}

TEST(StressReplay, WideChainKeepsStrikesNearSpotAfterLargeMoveAndReachesMarginFloors) {
  const test::SanitizerScenarioScale scale;
  auto data = script();
  data["strike_window"] = 0.3;
  data["events"] = json::array({{{"type", "spike"}, {"at", "09:32"}, {"move", 0.16}, {"iv", 0}}});
  Replay replay(data);
  replay.until(9, 31);
  ASSERT_TRUE(replay.order("short-put", "SPX261016P06000000", trading::Side::Sell).decision.ok());
  replay.until(9, 38);
  const auto spot = replay.desk->book().underlyings().at("SPX").spot;
  EXPECT_GT(spot, 6900);
  bool near = false;
  for (const auto& [osi, id] : replay.ids) {
    (void)osi;
    const auto* option = replay.desk->book().option(id);
    if (std::abs(option->contract.strike - spot) < 10 && option->has_quote) near = true;
  }
  EXPECT_TRUE(near);
  EXPECT_EQ(trading::naked_requirement(*md::parse_osi("SPX261016P06000000"), spot), Money::parse("60000"));
  const auto snapshot = replay.desk->trading_view()->snapshot;
  ASSERT_EQ(snapshot->positions.size(), 1U);
  ASSERT_TRUE(snapshot->positions.front().mark);
  EXPECT_EQ(snapshot->buying_power.short_requirement,
            Money::parse("60000") + *snapshot->positions.front().mark * 100);
  EXPECT_EQ(trading::naked_requirement(*md::parse_osi("SPX261016C07500000"), 6000), Money::parse("60000"));
  replay.verify();
}
}  // namespace
