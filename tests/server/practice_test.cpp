#include <gtest/gtest.h>
#include <algorithm>
#include "openport/server/practice.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
TEST(Practice, GradesEveryCommittedStateAndDoesNotEraseAnEarlierBreach) {
  test::RecordingFile file;
  test::ScriptedMarket f;
  const auto path = (file.directory / "journal.jsonl").string();
  trading::SessionConfig config;
  config.rules.max_drawdown = trading::Money::parse("1000");
  {
    trading::TradingSession session(config, f.time, trading::FileJournal::create(path));
    f.seed(session);
    ASSERT_TRUE(session.submit(f.market("buy", 2), f.time).decision.ok());
    f.next();
    session.on_quotes({f.quote("1", "1.20")}, {f.valuation()}, f.time);
    ASSERT_TRUE(session.submit(f.market("sell", 2, trading::Side::Sell), f.time).decision.ok());
  }
  const auto deadline = md::format_timestamp(f.time - md::kNanosPerSecond);
  const json objectives = {
      {{"id", "vega"}, {"type", "max_net_vega"}, {"limit", 100}, {"at", deadline}},
      {{"id", "loss"}, {"type", "max_loss"}, {"limit", 100}},
      {{"id", "room"}, {"type", "min_floor_room"}, {"limit", 90}},
      {{"id", "flat"}, {"type", "flat_by"}, {"limit", 0}, {"at", deadline}},
      {{"id", "generous"}, {"type", "max_loss"}, {"limit", 900}}};
  const auto result = server::grade_drill(trading::FileJournal::read(path), objectives);
  EXPECT_EQ(result["score"], 20);
  EXPECT_EQ(result["passed"], 1);
  EXPECT_EQ(result["objectives"][0]["observed"], 400);
  EXPECT_FALSE(result["objectives"][3]["passed"].get<bool>());
  const json future = {{{"id", "later"}, {"type", "flat_by"}, {"limit", 0}, {"at", md::format_timestamp(f.time + md::kNanosPerMinute)}}};
  EXPECT_TRUE(server::grade_drill(trading::FileJournal::read(path), future)["score"].is_null());
}
TEST(Practice, APlanWithoutAFloorCannotPassAFloorRoomObjective) {
  test::RecordingFile file;
  test::ScriptedMarket f;
  const auto path = (file.directory / "journal.jsonl").string();
  { trading::TradingSession session({}, f.time, trading::FileJournal::create(path)); f.seed(session); }
  const json objectives = {{{"id", "floor"}, {"type", "min_floor_room"}, {"limit", 10}}};
  EXPECT_TRUE(server::grade_drill(trading::FileJournal::read(path), objectives)["score"].is_null());
}
TEST(Practice, LossBudgetKeepsExactMicroDollarBoundary) {
  test::RecordingFile file;
  test::ScriptedMarket f;
  const auto path = (file.directory / "journal.jsonl").string();
  trading::SessionConfig config;
  config.fee_per_contract = trading::Money::parse("0.650001");
  {
    trading::TradingSession session(config, f.time, trading::FileJournal::create(path));
    f.seed(session);
    ASSERT_TRUE(session.submit(f.market("entry"), f.time).decision.ok());
  }
  const json objectives = {{{"id", "loss"}, {"type", "max_loss"}, {"limit", 10.650001}}};
  const auto grade = server::grade_drill(trading::FileJournal::read(path), objectives);
  EXPECT_EQ(grade["score"], 100);
  EXPECT_EQ(grade["objectives"][0]["observed"], 10.650001);
}
TEST(Practice, EventsResolveEarlyClosesAndOvernightSessions) {
  const auto& scenarios = providers::builtin_scenarios();
  const auto crush = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "morning-crush"; });
  ASSERT_NE(crush, scenarios.end());
  const auto marks = server::scenario_marks(*crush, {2026, 11, 27});
  ASSERT_FALSE(marks.empty());
  const auto at = md::parse_datetime(marks[0]["time"].get<std::string>(), md::Zone::Utc);
  ASSERT_TRUE(at);
  EXPECT_LT(md::new_york_time(*at).seconds, 13 * 3600);
  const auto objectives = server::drill_objectives(*crush, {2026, 11, 27});
  EXPECT_EQ(objectives.back()["at"], "2026-11-27T17:51:00.000Z");
  // An overnight session belongs to the trading date it leads into. Hold-overnight's
  // starts on Wednesday evening, so its 02:30 spike is Thursday's, before Thursday's
  // opening gap and 10:30 crush.
  const auto hold = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "hold-overnight"; });
  ASSERT_NE(hold, scenarios.end());
  const auto held = server::scenario_marks(*hold, hold->date);
  ASSERT_EQ(held.size(), 3U);
  EXPECT_EQ(held[0]["type"], "spike");
  EXPECT_EQ(held[0]["time"], "2026-09-17T06:30:00.000Z");
  EXPECT_EQ(held[1]["time"], "2026-09-17T13:30:00.000Z");
  EXPECT_EQ(held[2]["time"], "2026-09-17T14:30:00.000Z");
  // A gap opens its session: overnight-gap's at 20:15 ET on the evening before its date.
  const auto gap = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "overnight-gap"; });
  ASSERT_NE(gap, scenarios.end());
  const auto gapped = server::scenario_marks(*gap, gap->date);
  ASSERT_EQ(gapped.size(), 1U);
  EXPECT_EQ(gapped[0]["type"], "gap");
  EXPECT_EQ(gapped[0]["time"], "2026-09-16T00:15:00.000Z");
  // A deadline in an overnight session from 20:15 ET is on the evening before its
  // trading date; one earlier is on the trading date itself.
  const auto night = providers::parse_scenario(R"({"id":"night","title":"Night","description":"One overnight session.",
      "symbols":["SPX"],"session":"overnight","date":"2026-09-16","seed":5,"generator":1,"drift":[[1,0]],
      "volatility":0,"iv_shift":0,"spot_vol":0,
      "objectives":[{"id":"evening","type":"flat_by","at":"23:00"},{"id":"dawn","type":"flat_by","at":"08:00"}]})",
      "night.json");
  const auto deadlines = server::drill_objectives(night, night.date);
  ASSERT_EQ(deadlines.size(), 2U);
  EXPECT_EQ(deadlines[0]["at"], "2026-09-16T03:00:00.000Z");
  EXPECT_EQ(deadlines[1]["at"], "2026-09-16T12:00:00.000Z");
}
// A deadline in a later session of a run is resolved in that session's window: the
// next trading date's, scaled into its early close where it has one.
TEST(Practice, ALaterSessionsDeadlineResolvesInThatSession) {
  const auto scenario = providers::parse_scenario(R"({"id":"two-days","title":"Two days","description":"Two quiet sessions.",
      "symbols":["SPX"],"date":"2026-09-16","seed":5,"generator":1,"volatility":0,"iv_shift":0,"spot_vol":0,
      "sessions":[{"session":"regular","drift":[[1,0]]},{"session":"regular","drift":[[1,0]]}],
      "objectives":[{"id":"first","type":"flat_by","at":"15:45"},{"id":"second","type":"flat_by","at":"15:45","session":2}]})",
      "two-days.json");
  auto objectives = server::drill_objectives(scenario, scenario.date);
  ASSERT_EQ(objectives.size(), 2U);
  EXPECT_EQ(objectives[0]["at"], "2026-09-16T19:45:00.000Z");
  EXPECT_EQ(objectives[1]["at"], "2026-09-17T19:45:00.000Z");
  // From the day before Thanksgiving, the second session is Friday's, which closes at 13:00.
  objectives = server::drill_objectives(scenario, {2026, 11, 25});
  ASSERT_EQ(objectives.size(), 2U);
  EXPECT_EQ(objectives[0]["at"], "2026-11-25T20:45:00.000Z");
  EXPECT_EQ(objectives[1]["at"], "2026-11-27T17:51:00.000Z");
}
// Shares, which trades, exercise and assignment open, are held as contracts are: their
// mark counts in equity, and an account holding them is not flat.
TEST(Practice, SharesCountInEquityAndKeepTheAccountFromBeingFlat) {
  test::RecordingFile file;
  test::ScriptedMarket f;
  const auto path = (file.directory / "journal.jsonl").string();
  {
    trading::TradingSession session({}, f.time, trading::FileJournal::create(path));
    f.seed(session);
    ASSERT_TRUE(session.trade_stock("SPY", 10, f.time, trading::StockPrice{"SPY", f.time, trading::Money::parse("500")}).decision.ok());
    f.next();
    session.on_quotes({}, {}, f.time, {trading::StockPrice{"SPY", f.time, trading::Money::parse("490")}});
  }
  const json objectives = {
      {{"id", "flat"}, {"type", "flat_by"}, {"limit", 0}, {"at", md::format_timestamp(f.time)}},
      {{"id", "loss"}, {"type", "max_loss"}, {"limit", 1000}}};
  const auto grade = server::grade_drill(trading::FileJournal::read(path), objectives);
  EXPECT_EQ(grade["objectives"][0]["passed"], false);
  // $10 lost on each of the 10 shares.
  EXPECT_EQ(grade["objectives"][1]["observed"], 100);
  EXPECT_EQ(grade["objectives"][1]["passed"], true);
}
// A held contract or share without a mark leaves equity unknown: an objective read
// from equity can neither pass nor fail on that state, and the drill earns no score.
TEST(Practice, AnUnmarkedHoldingLeavesEquityObjectivesUngraded) {
  test::RecordingFile file;
  test::ScriptedMarket f;
  const auto path = (file.directory / "journal.jsonl").string();
  {
    trading::TradingSession session({}, f.time, trading::FileJournal::create(path));
    f.seed(session);
    ASSERT_TRUE(session.submit(f.market("entry"), f.time).decision.ok());
    ASSERT_TRUE(session.trade_stock("SPY", 10, f.time, trading::StockPrice{"SPY", f.time, trading::Money::parse("500")}).decision.ok());
  }
  const json objectives = {
      {{"id", "loss"}, {"type", "max_loss"}, {"limit", 1000}},
      {{"id", "vega"}, {"type", "max_net_vega"}, {"limit", 1000}}};
  for (const auto& [marks, symbol] : std::vector<std::pair<std::string, std::string>>{{"marks", f.symbol()}, {"stock_marks", "SPY"}}) {
    SCOPED_TRACE(marks);
    // The last committed state loses the holding's mark.
    auto journal = trading::FileJournal::read(path);
    ASSERT_FALSE(journal.records.empty());
    auto unmarked = journal.records.back();
    ++unmarked.seq;
    json dropped = json::object();
    dropped["o"] = json::object();
    dropped["d"] = json::array({symbol});
    json delta = json::object();
    delta["o"][marks] = dropped;
    unmarked.payload = json{{"delta", delta}}.dump();
    journal.records.push_back(unmarked);
    const auto grade = server::grade_drill(journal, objectives);
    EXPECT_TRUE(grade["objectives"][0]["passed"].is_null());
    EXPECT_TRUE(grade["score"].is_null());
    // Net vega reads valuations, not marks, and is still graded.
    EXPECT_EQ(grade["objectives"][1]["passed"], true);
  }
}
}

namespace {
TEST(Practice, ResetCannotEarnAScoreOnAFixedPlan) {
  test::RecordingFile file;
  test::ScriptedMarket f;
  const auto path = (file.directory / "journal.jsonl").string();
  {
    trading::TradingSession session({}, f.time, trading::FileJournal::create(path));
    f.seed(session);
    ASSERT_TRUE(session.reset_account(trading::Money::parse("100000"), {}, "again", f.time).decision.ok());
  }
  const json objectives = {{{"id", "loss"}, {"type", "max_loss"}, {"limit", 500}}};
  const auto grade = server::grade_drill(trading::FileJournal::read(path), objectives);
  EXPECT_TRUE(grade["score"].is_null());
  EXPECT_EQ(grade["basis"], "ungraded: plan changed or account reset");
}
}
