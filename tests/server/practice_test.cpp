#include <gtest/gtest.h>
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
  const auto overnight = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "overnight"; });
  ASSERT_NE(overnight, scenarios.end());
  EXPECT_TRUE(server::scenario_marks(*overnight, overnight->date).is_array());
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
