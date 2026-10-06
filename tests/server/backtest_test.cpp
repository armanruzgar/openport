#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <gtest/gtest.h>
#include "openport/server/backtest.hpp"
#include "openport/trading/events.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/web_policy.hpp"
#include "openport/providers/replay_batches.hpp"
#include "server/run_json.hpp"
#include "support/recording.hpp"
#include "support/sanitizer.hpp"
#include "support/contract_capture.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
json catalogue() {
  json definition{{"id", "batch"}, {"version", 1}, {"name", "Batch fixture"}, {"description", "Simulated test prices"}, {"underlyings", {"SPX"}},
      {"window", {{"start", "09:30"}, {"end", "09:31"}, {"weekdays", {1, 2, 3, 4, 5}}}}, {"conditions", json::object()},
      {"structure", {{"template", {{"kind", "vertical"}, {"type", "put"}, {"direction", "credit"}, {"target", {{"mode", "strike"}, {"value", 5000}}}, {"width", 5}}}, {"expiry", {{"min", 0}, {"max", 1}}}}},
      {"sizing", {{"units", 1}}}, {"management", {{"close_by", "09:32"}}},
      {"guardrails", {{"max_entries_per_day", 1}, {"cooldown_minutes", 0}}}};
  return {{"schema", 1}, {"definitions", {{"batch", {{"versions", {definition}}, {"deleted", false}}}}}, {"modes", json::object()}};
}
std::string bytes(const std::filesystem::path& path) {
  std::ifstream input(path); std::ostringstream out; out << input.rdbuf(); return out.str();
}
providers::Scenario volume_scenario(const std::filesystem::path& directory) {
  const auto file = directory / "volume.json";
  { std::ofstream out(file); out << R"({"id":"volume","title":"Volume","description":"A quiet open.",
    "symbols":["SPX"],"date":"2026-09-16","seed":5,"generator":1,"session":"regular",
    "drift":[[1,0]],"volatility":0,"iv_shift":0,"spot_vol":0})"; }
  return providers::read_scenario(file);
}
std::filesystem::path recorded_day(const std::filesystem::path& directory, md::Date date, bool loss = false, bool breadth = false, std::optional<double> volume = {}, bool stale_end = false) {
  const auto path = directory / (md::format_date(date) + ".oprec");
  auto contract = *md::parse_osi("SPXW  260916P05000000");
  contract.expiry = date;
  auto lower = contract; lower.strike = 4995;
  md::RecordingHeader header;
  header.provider = "simulated test";
  header.market_controls = stale_end;
  header.subscription.underlyings = {"SPX", "SPY"};
  header.started = md::new_york_to_utc(date, 9, 30);
  header.capabilities.poll_interval = std::chrono::seconds(60);
  md::Timestamp time = header.started;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return time; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, recording);
  sink.publish(md::ContractDefinition{0, contract});
  sink.publish(md::ContractDefinition{1, lower});
  if (breadth) sink.publish(md::UnderlyingClose{"SPX", time, md::previous_business_day(date), 5000});
  for (int minute = 0; minute < 3; ++minute) {
    time = header.started + minute * md::kNanosPerMinute;
    if (stale_end && minute == 2) {
      time += 15 * md::kNanosPerMinute;
      // A heartbeat advances time without vouching for unchanged option quotes.
      sink.publish(md::SnapshotHeartbeat{"SPX", time});
      break;
    }
    const double high = minute == 0 ? 10 : breadth && minute == 2 ? 9.5 : loss && minute == 2 ? 13 : 9;
    sink.publish(md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, time, high, high + .1, 20, 20});
    sink.publish(md::OptionQuote{1, time, 8, 8.1, 20, 20});
    if (volume) {
      sink.publish(md::OptionVolume{0, time, *volume});
      sink.publish(md::OptionVolume{1, time, *volume});
    }
    sink.publish(md::SnapshotComplete{"SPX", time});
  }
  sink.close();
  return path;
}
server::BacktestRequest request_for(const std::vector<std::filesystem::path>& files) {
  json days = json::array();
  for (const auto& file : files) days.push_back({{"file", file.string()}});
  auto request = server::parse_backtest({{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", days}}, catalogue(), {}, {}, false);
  request.analytics.fallback_rate = 0;
  return request;
}
TEST(Backtest, GapAndTrailingRulesMatchFourWorkersAndVerify) {
  test::RecordingFile storage;
  std::vector<std::filesystem::path> files;
  for (int day = 14; day <= 17; ++day) files.push_back(recorded_day(storage.directory, {2026, 9, day}, false, true));
  auto request = request_for(files);
  auto& setup = request.playbooks["definitions"]["batch"]["versions"][0];
  setup["conditions"] = {{"gap", {{"min_percent", -1}, {"max_percent", 1}}}};
  setup["management"] = {{"close_by", "15:45"}, {"trailing_stop", {{"percent", 50}}}};
  const std::atomic_bool cancel{false};
  request.workers = 1;
  const auto first = server::run_backtest(request, storage.directory / "one", cancel);
  ASSERT_EQ(first.at("status"), "completed") << first.dump();
  request.workers = 4;
  const auto second = server::run_backtest(request, storage.directory / "four", cancel);
  EXPECT_EQ(first, second);
  for (const auto& day : first.at("days")) {
    ASSERT_EQ(day.at("fills").size(), 4U) << day.dump();
    ASSERT_EQ(day.at("trades").size(), 1U);
    EXPECT_EQ(day.at("trades")[0].at("rules").at("trailing_stop"), true);
    const auto journal = day.at("journal").get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "one" / journal), bytes(storage.directory / "four" / journal));
    EXPECT_NE(bytes(storage.directory / "one" / journal).find("Playbook automatic trailing stop"), std::string::npos);
    const auto verified = server::verify_run(storage.directory / "one" / journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
  for (const auto& attempt : first.at("attempts")) {
    const auto verified = server::verify_run(storage.directory / "one" / attempt.at("journal").get<std::string>());
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(Backtest, CombinedPlanRulesSurvivePlaybookRunsRecoveryAndVerification) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 22});
  auto definitions = catalogue();
  definitions["definitions"]["batch"]["versions"][0]["management"]["stop_credit_multiple"] = 2;
  const json rules{{"plan", "Combined F6 F17 F59 F31 F58 F62 F15 F60"},
      {"profit_target", "1000"}, {"max_drawdown", "1000"}, {"time_limit_days", 30}, {"inactivity_days", 14},
      {"underlyings", {"SPX"}}, {"trading_start", "09:30"}, {"trading_end", "16:00"},
      {"flat_time", "09:31"}, {"no_overnight", true}, {"max_contracts_held", 5}, {"require_stop_loss", true},
      {"max_trade_risk", "500.123456"}, {"max_trade_risk_percent", 100},
      {"news_before_minutes", 1}, {"news_after_minutes", 1}, {"news_action", "flatten"},
      {"hold_cutoff", "09:31"}, {"hold_restrictions", {"earnings", "ex_dividend", "split", "weekend"}},
      {"events", {{{"kind", "news"}, {"time", "2026-09-22T13:32:00Z"}},
                  {{"kind", "split"}, {"time", "2026-09-23"}, {"symbol", "SPX"}}}}};
  auto request = server::parse_backtest({{"playbook", "batch@1"}, {"days", {{{"file", file.string()}}}},
      {"plan", {{"initial_cash", "10000"}, {"rules", rules}}}}, definitions, {}, {}, false);
  request.analytics.fallback_rate = 0;
  auto expected = request.config.rules;
  trading::normalize_event_rules(expected, true);
  const std::atomic_bool cancel{false};
  const auto root = storage.directory / "combined";
  const auto report = server::run_backtest(request, root, cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_EQ(report.at("days").size(), 1U);
  ASSERT_EQ(report.at("days")[0].at("fills").size(), 4U) << report.dump();
  for (const auto* rows : {"days", "attempts"}) {
    ASSERT_FALSE(report.at(rows).empty());
    for (const auto& row : report.at(rows)) {
      const auto path = root / row.at("journal").get<std::string>();
      const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(path.string()));
      EXPECT_EQ(recovered.config().rules, expected);
      EXPECT_TRUE(recovered.snapshot()->positions.empty());
      ASSERT_FALSE(recovered.snapshot()->recent_orders.empty());
      EXPECT_TRUE(recovered.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:flat_time:"));
      const auto verified = server::verify_run(path);
      EXPECT_TRUE(verified.matched) << verified.message;
    }
  }
}
TEST(Backtest, AttemptsCarryBalancesDecidePassAndFailureAndLeaveOpenTail) {
  test::RecordingFile storage;
  std::vector<std::filesystem::path> files;
  for (const auto date : {md::Date{2026, 9, 14}, md::Date{2026, 9, 15}, md::Date{2026, 9, 16}, md::Date{2026, 9, 17}})
    files.push_back(recorded_day(storage.directory, date, date.day == 16));
  auto request = request_for(files);
  request.config.rules.profit_target = Money::parse("150");
  // Admit the spread's initial risk, then fail on a reversal from its marked high.
  request.config.rules.max_drawdown = Money::parse("350");
  request.config.rules.drawdown_mode = trading::DrawdownMode::Intraday;
  const std::atomic_bool cancel{false};
  const auto root = storage.directory / "batch";
  const auto report = server::run_backtest(request, root, cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_EQ(report.at("attempts").size(), 3U) << report.dump();
  EXPECT_EQ(report.at("attempts")[0].at("days"), 2);
  EXPECT_EQ(report.at("attempts")[0].at("outcome"), "passed");
  EXPECT_EQ(report.at("attempts")[1].at("outcome"), "failed");
  EXPECT_EQ(report.at("attempts")[2].at("outcome"), "open");
  EXPECT_EQ(report.at("summary").at("pass_rate"), .5);
  EXPECT_EQ(report.at("days")[0].at("pnl"), "77.40");
  EXPECT_EQ(report.at("days")[0].at("ended"), md::format_timestamp(md::new_york_to_utc({2026, 9, 14}, 9, 32)));
  EXPECT_EQ(report.at("days")[0].at("fills").size(), 4U);
  EXPECT_EQ(report.at("days")[0].at("max_drawdown"), "11.30");
  const auto daily = trading::FileJournal::read((root / "days/000001.jsonl").string());
  bool found_start = false;
  for (const auto& record : daily.records) {
    if (record.type != "run_input") continue;
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events")) {
      if (event.at("type") == "run_input" && event.at("payload").at("kind") == "start") {
        found_start = true;
        EXPECT_EQ(event.at("payload").at("symbols"), json::array({"SPX", "SPY"}));
      }
    }
  }
  EXPECT_EQ(report.at("schema"), 2);
  for (const auto& attempt : report.at("attempts")) {
    auto journal = trading::FileJournal::read((root / attempt.at("journal").get<std::string>()).string());
    ASSERT_EQ(attempt.at("day_rows").size(), attempt.at("days").get<std::size_t>());
    Money previous_balance = request.config.initial_cash, previous_equity = request.config.initial_cash;
    for (const auto& row : attempt.at("day_rows")) {
      auto prefix = journal;
      const auto end = *md::parse_datetime(row.at("ended").get<std::string>(), md::Zone::Utc);
      while (!prefix.records.empty() && prefix.records.back().time > end) prefix.records.pop_back();
      prefix.head = prefix.records.back().hash;
      const auto session = trading::TradingSession::recover(prefix);
      const auto snapshot = session.snapshot();
      const auto inputs = trading::plan_inputs(*snapshot);
      EXPECT_EQ(row.at("start_balance"), previous_balance.str());
      EXPECT_EQ(row.at("start_equity"), previous_equity.str());
      EXPECT_EQ(row.at("end_balance"), inputs.balance.str());
      EXPECT_EQ(row.at("end_equity"), snapshot->equity.str());
      EXPECT_EQ(row.at("pnl"), (snapshot->equity - previous_equity).str());
      EXPECT_EQ(row.at("floor"), snapshot->evaluation.floor.str());
      EXPECT_EQ(row.at("floor_distance"), (snapshot->equity - snapshot->evaluation.floor).str());
      EXPECT_EQ(row.at("peak"), snapshot->evaluation.peak.str());
      EXPECT_EQ(row.at("target_progress"), trading::attempt_profit(snapshot->evaluation, request.config.rules, inputs).str());
      EXPECT_EQ(row.at("day_lock"), trading::to_string(snapshot->evaluation.day_lock));
      EXPECT_EQ(row.at("decision_code"), trading::to_string(snapshot->evaluation.decision_code));
      EXPECT_EQ(row.at("trades_opened"), 1);
      previous_balance = inputs.balance; previous_equity = snapshot->equity;
    }
  }
  EXPECT_TRUE(found_start);
  EXPECT_EQ(report.at("days")[0].at("trades").size(), 1U);
  EXPECT_EQ(report.at("summary").at("worst_days")[0], 2);
  EXPECT_FALSE(report.at("days")[2].at("rule_trips").empty());
  const json state{{"id", "000099"}, {"status", "completed"}, {"phase", "finished"}, {"completed", 8}, {"total", 8},
      {"label", server::kBacktestLabel}, {"directory", root.string()}, {"report", report}};
  test::capture_contract("backtests", "GET", "/api/backtests/000099", {200, state.dump()});
  for (const auto& attempt : report.at("attempts")) {
    const auto verified = server::verify_run(root / attempt.at("journal").get<std::string>());
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(Backtest, EndOfDayFloorsRatchetAcrossRealAccountDays) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14}), recorded_day(storage.directory, {2026, 9, 15})});
  request.config.rules.profit_target = Money::parse("1000");
  request.config.rules.max_drawdown = Money::parse("500");
  const std::atomic_bool cancel{false};
  const auto root = storage.directory / "eod";
  const auto report = server::run_backtest(request, root, cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_EQ(report.at("attempts").size(), 1U);
  EXPECT_EQ(report.at("summary").at("pass_rate"), nullptr);
  const auto recovery = trading::TradingSession::recover(trading::FileJournal::read((root / "attempts/000001.jsonl").string()));
  EXPECT_EQ(recovery.snapshot()->evaluation.peak, request.config.initial_cash + Money::parse("77.40"));
  EXPECT_EQ(recovery.snapshot()->evaluation.floor, request.config.initial_cash + Money::parse("77.40") - Money::parse("500"));
  EXPECT_EQ(report.at("attempts")[0].at("pnl"), "154.80");
}
TEST(Backtest, EntryReasonsGiveWhatBlockedEntriesInsideTheWindow) {
  // B47: the reasons were read at the end of the input, after every window had closed,
  // so each day said only "Outside entry window".
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14})});
  // A dollar of floor room fits no spread.
  request.config.rules.max_drawdown = Money::parse("1");
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "reasons", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const auto& day = report.at("days")[0];
  EXPECT_TRUE(day.at("fills").empty());
  EXPECT_EQ(day.at("ended"), md::format_timestamp(md::new_york_to_utc({2026, 9, 14}, 9, 32)));
  EXPECT_EQ(day.at("entry_reasons"), json({{"batch:SPX", "No units fit buying power, limits and floor room"}})) << day.dump();
  EXPECT_EQ(report.at("attempts")[0].at("entry_reasons"), day.at("entry_reasons"));
  // A day whose window never opens still says so.
  auto closed = catalogue();
  closed["definitions"]["batch"]["versions"][0]["window"]["weekdays"] = {2, 3, 4, 5};
  auto monday = server::parse_backtest({{"playbook", "batch@1"}, {"plan", "eod-50k"},
      {"days", {{{"file", (storage.directory / "2026-09-14.oprec").string()}}}}}, closed, {}, {}, false);
  monday.analytics.fallback_rate = 0;
  const auto skipped = server::run_backtest(monday, storage.directory / "closed", cancel);
  ASSERT_EQ(skipped.at("status"), "completed") << skipped.dump();
  EXPECT_EQ(skipped.at("days")[0].at("entry_reasons"), json({{"batch:SPX", "Outside entry window"}}));
}
TEST(Backtest, EntryReasonsRetainSpecificMissingConditions) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14})});
  request.playbooks["definitions"]["batch"]["versions"][0]["conditions"] = {{"iv_rank", {{"min", .3}, {"max", 1}}}};
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "missing-iv", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const json expected{{"batch:SPX", "IV rank unavailable (no IV history)"}};
  EXPECT_EQ(report.at("days")[0].at("entry_reasons"), expected);
  EXPECT_EQ(report.at("attempts")[0].at("entry_reasons"), expected);
}
TEST(Backtest, GeneratedDaysMatchSingleReplayAndParallelReportsAreByteIdentical) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile storage;
  auto definitions = catalogue();
  definitions["definitions"]["batch"]["versions"][0]["structure"]["template"]["target"] = {{"mode", "delta"}, {"value", 15}};
  auto request = server::parse_backtest({{"playbook", "batch"}, {"plan", "eod-50k"}, {"scenarios", 2}, {"seed", "18446744073709551610"}},
      definitions, providers::builtin_scenarios(), {});
  // SPX-only custom sources keep the scenario test small and remain verifiable.
  for (std::size_t index = 0; index < request.days.size(); ++index) {
    auto source = json::parse(request.days[index].scenario->source);
    source["symbols"] = {"SPX"};
    const auto file = storage.directory / ("scenario-" + std::to_string(index) + ".json");
    std::ofstream(file) << source.dump();
    request.days[index].scenario = providers::read_scenario(file);
  }
  const std::atomic_bool cancel{false};
  request.workers = 1;
  const auto sequential = server::run_backtest(request, storage.directory / "sequential", cancel);
  ASSERT_EQ(sequential.at("status"), "completed") << sequential.dump();
  ASSERT_FALSE(sequential.at("days")[0].at("fills").empty()) << sequential.dump();
  request.workers = 2;
  const auto parallel = server::run_backtest(request, storage.directory / "parallel", cancel);
  EXPECT_EQ(sequential.dump(), parallel.dump());
  EXPECT_EQ(bytes(storage.directory / "sequential/report.json"), bytes(storage.directory / "parallel/report.json"));
  for (std::size_t index = 0; index < request.days.size(); ++index) {
    const auto journal = sequential.at("days")[index].at("journal").get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "sequential" / journal), bytes(storage.directory / "parallel" / journal));
    // verify_run replays one generated day through an independent Desk and
    // compares every transaction, all fills, final equity and the head hash.
    const auto verified = server::verify_run(storage.directory / "sequential" / journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    EXPECT_EQ((verified.equity - request.config.initial_cash).str(), sequential.at("days")[index].at("pnl").get<std::string>());
  }
}
TEST(Backtest, CancellationPreservesCompletedDaysAndNeverInventsAttempts) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14}), recorded_day(storage.directory, {2026, 9, 15})});
  request.workers = 1;
  std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "cancelled", cancel,
      [&](std::size_t completed, std::string_view) { if (completed == 1) cancel = true; });
  EXPECT_EQ(report.at("status"), "cancelled");
  EXPECT_EQ(report.at("summary").at("completed_days"), 1);
  EXPECT_TRUE(report.at("days")[1].is_null());
  EXPECT_TRUE(report.at("attempts").empty());
  const auto verified = server::verify_run(storage.directory / "cancelled/days/000001.jsonl");
  EXPECT_TRUE(verified.matched) << verified.message;
  const auto pre_cancelled = server::run_backtest(request, storage.directory / "pre-cancelled", cancel);
  EXPECT_EQ(pre_cancelled.at("summary").at("completed_days"), 0);
  cancel = false;
  const auto during_attempt = server::run_backtest(request, storage.directory / "cancelled-attempt", cancel,
      [&](std::size_t completed, std::string_view) { if (completed == 3) cancel = true; });
  EXPECT_EQ(during_attempt.at("status"), "cancelled");
  EXPECT_TRUE(during_attempt.at("attempts").empty());
  const auto prefix = server::verify_run(storage.directory / "cancelled-attempt/attempts/000001.jsonl");
  EXPECT_TRUE(prefix.matched) << prefix.message;
}
TEST(Backtest, ExistingSingleSourceJournalStillLoadsWithoutTransitionRecords) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const auto journal = storage.directory / "legacy.jsonl";
  md::RecordingReader reader(file);
  server::Desk::Options options;
  options.replay = true;
  options.paper_journal = journal;
  options.run_input = server::recording_input(file);
  server::Desk desk("replay (simulated test)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  desk.stop();
  const auto recovery = trading::FileJournal::read(journal.string());
  EXPECT_EQ(trading::TradingSession::recover(recovery).snapshot()->equity, options.paper.initial_cash);
  EXPECT_EQ(bytes(journal).find("\"kind\":\"source\""), std::string::npos);
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
}
TEST(Backtest, BrokenRecordingFailsWithoutCountingAZeroReturnDay) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const auto request = request_for({file});
  std::ofstream(file, std::ios::trunc) << "broken";
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "broken", cancel);
  EXPECT_EQ(report.at("status"), "failed");
  EXPECT_EQ(report.at("summary").at("completed_days"), 0);
  EXPECT_EQ(report.at("errors").size(), 1U);
  EXPECT_TRUE(report.at("attempts").empty());
}
TEST(Backtest, RejectsInvalidInputsAndPinsArchivedHistoricalVersions) {
  auto definitions = catalogue();
  auto edited = definitions.at("definitions").at("batch").at("versions")[0];
  edited["version"] = 2; edited["name"] = "Latest";
  definitions["definitions"]["batch"]["versions"].push_back(edited);
  definitions["definitions"]["batch"]["deleted"] = true;
  const auto& scenarios = providers::builtin_scenarios();
  const json good{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"scenarios", 2}, {"seed", "0"}};
  const auto parsed = server::parse_backtest(good, definitions, scenarios, {});
  EXPECT_EQ(parsed.playbooks.at("definitions").at("batch").at("versions").size(), 1U);
  EXPECT_EQ(parsed.days[1].seed, 1U);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{{"plan", "practice"}, {"plan", "funded-eod-50k"},
      {"workers", 0}, {"workers", 17}, {"workers", 1.5}, {"scenarios", 253}, {"seed", "18446744073709551615"},
      {"seed", -1}, {"playbook", "batch@0"}, {"playbook", "batch@1junk"}, {"extra", true}}) {
    auto bad = good; bad[key] = value;
    EXPECT_THROW((void)server::parse_backtest(bad, definitions, scenarios, {}), std::exception) << key;
  }
  for (const auto& [key, message] : std::vector<std::pair<std::string, std::string>>{{"playbook", "Supply exactly one of playbook or playbooks"}, {"plan", "plan is required"}}) {
    auto bad = good; bad.erase(key);
    try { (void)server::parse_backtest(bad, definitions, scenarios, {}); ADD_FAILURE() << key; }
    catch (const std::invalid_argument& error) { EXPECT_EQ(error.what(), message); }
  }
  auto unknown = good; unknown["playbook"] = "missing@1";
  try { (void)server::parse_backtest(unknown, definitions, scenarios, {}); ADD_FAILURE(); }
  catch (const std::invalid_argument& error) { EXPECT_STREQ(error.what(), "Unknown playbook"); }
  auto cashless = good; cashless["plan"] = {{"rules", json::object()}};
  try { (void)server::parse_backtest(cashless, definitions, scenarios, {}); ADD_FAILURE(); }
  catch (const std::invalid_argument& error) { EXPECT_STREQ(error.what(), "plan initial_cash is required"); }
  // A missing seed or day field, and a rule of the wrong type, are named too.
  for (const auto& [request, message] : std::vector<std::pair<json, std::string>>{
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"scenarios", 2}}, "seed is required"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"seed", "1"}}}}}, "day scenario is required"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"scenario", "reversal"}}}}}, "day seed is required"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"scenario", "hold-overnight"}, {"seed", "1"}}}}},
            "Scenario hold-overnight plays several sessions, and a backtest day is one; play it in Replay"},
           {{{"playbook", "batch@1"}, {"plan", "eod-50k"}, {"days", {{{"scenario", "reversal"}, {"seed", "1"}, {"date", 20260916}}}}}, "date must be YYYY-MM-DD"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", 100}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules profit_target must be a decimal string"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}, {"buy_only", "yes"}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules buy_only must be true or false"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}, {"slippage_ticks", "1"}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules slippage_ticks must be an integer"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}, {"plan", 5}}}}}, {"scenarios", 1}, {"seed", 0}},
            "plan rules plan must be text"},
           {{{"playbook", "batch@1"}, {"plan", {{"initial_cash", "1000"}, {"rules", {{"profit_target", "100"}}}, {"fee_per_contract", 0.65}}}, {"scenarios", 1}, {"seed", 0}},
            "plan fee_per_contract must be a decimal string"}}) {
    try { (void)server::parse_backtest(request, definitions, scenarios, {}); ADD_FAILURE() << message; }
    catch (const std::invalid_argument& error) { EXPECT_EQ(error.what(), message); }
  }
  const auto custom = server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "1000"},
      {"rules", {{"profit_target", "100"}, {"max_drawdown", "50"}, {"drawdown_mode", "end_of_day"}}}}}, {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  EXPECT_EQ(custom.config.rules.max_drawdown, Money::parse("50"));
  auto timed = good;
  timed["plan"] = {{"initial_cash", "10000"}, {"rules", {{"profit_target", "100"}, {"time_limit_days", 30},
      {"inactivity_days", 14}, {"flat_time", "15:45"}, {"no_overnight", true}, {"underlyings", {"SPX"}}, {"trading_start", "09:30"}, {"trading_end", "16:00"},
      {"trade_consistency_percent", 40}, {"min_trades", 12}, {"min_hold_seconds", 60},
      {"microscalp_seconds", 30}, {"microscalp_percent", 25}, {"no_hedging", true}, {"no_counter_positions", true},
      {"max_contracts_held", 5}, {"require_stop_loss", true}, {"max_trade_risk", "123.456789"}, {"max_trade_risk_percent", 25},
      {"scaling", {{{"profit", "0.00"}, {"contracts", 2}}}}, {"size_scaling", nullptr}}}};
  timed["plan"]["rules"].update({{"events", {{{"kind", "news"}, {"time", "2026-09-22T14:00:00Z"}}}},
      {"news_before_minutes", 5}, {"news_after_minutes", 10}, {"news_action", "flatten"},
      {"hold_restrictions", {"weekend", "split"}}, {"hold_cutoff", "15:40"}});
  const auto time_rules = server::parse_backtest(timed, definitions, scenarios, {}).config.rules;
  EXPECT_EQ(time_rules.flat_time, 945); EXPECT_TRUE(time_rules.no_overnight);
  EXPECT_EQ(time_rules.events.size(), 1U); EXPECT_EQ(time_rules.news_before_minutes, 5);
  EXPECT_EQ(time_rules.news_action, "flatten"); EXPECT_EQ(time_rules.hold_cutoff, 940);
  EXPECT_EQ(time_rules.hold_restrictions, (std::vector<std::string>{"split", "weekend"}));
  EXPECT_EQ(time_rules.time_limit_days, 30); EXPECT_EQ(time_rules.inactivity_days, 14);
  EXPECT_EQ(time_rules.underlyings, std::vector<std::string>{"SPX"});
  EXPECT_EQ(time_rules.trading_start, 570); EXPECT_EQ(time_rules.trading_end, 960);
  EXPECT_EQ(time_rules.max_contracts_held, 5); EXPECT_TRUE(time_rules.require_stop_loss);
  EXPECT_EQ(time_rules.max_trade_risk, Money::parse("123.456789")); EXPECT_EQ(time_rules.max_trade_risk_percent, 25);
  EXPECT_EQ(time_rules.scaling, (std::vector<trading::ScalingStep>{{Money{}, 2}}));
  EXPECT_FALSE(time_rules.size_scaling);
  EXPECT_EQ(time_rules.trade_consistency_percent, 40); EXPECT_EQ(time_rules.min_trades, 12);
  EXPECT_EQ(time_rules.min_hold_seconds, 60); EXPECT_EQ(time_rules.microscalp_seconds, 30); EXPECT_EQ(time_rules.microscalp_percent, 25);
  EXPECT_TRUE(time_rules.no_hedging); EXPECT_TRUE(time_rules.no_counter_positions);
  for (const auto& patch : std::vector<json>{{{"flat_time", "24:00"}}, {{"flat_time", "17:00"}}, {{"no_overnight", 1}}, {{"time_limit_days", 367}}, {{"inactivity_days", 1.5}},
      {{"underlyings", "SPX"}}, {{"trading_end", nullptr}}, {{"trading_start", "09:60"}}}) {
    auto bad = timed; bad["plan"]["rules"].update(patch);
    EXPECT_THROW((void)server::parse_backtest(bad, definitions, scenarios, {}), std::exception);
  }

  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  auto duplicate = good; duplicate.erase("scenarios"); duplicate.erase("seed");
  duplicate["days"] = {{{"file", file.string()}}, {{"file", file.string()}}};
  EXPECT_THROW((void)server::parse_backtest(duplicate, definitions, scenarios, {}, false), std::invalid_argument);
  duplicate["days"] = {{{"file", "../outside.oprec"}}};
  EXPECT_THROW((void)server::parse_backtest(duplicate, definitions, scenarios, storage.directory), std::invalid_argument);
  const auto confined = storage.directory / "recordings";
  std::filesystem::create_directory(confined);
  std::filesystem::create_symlink(file, confined / "escape.oprec");
  duplicate["days"] = {{{"file", "escape.oprec"}}};
  EXPECT_THROW((void)server::parse_backtest(duplicate, definitions, scenarios, confined), std::invalid_argument);
}
class BacktestSource final : public server::MetricsSource {
 public:
  json saved_catalogue = catalogue();
  using MetricsSource::trading_view;
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { server::EngineStatus result; result.trading.write = "open"; return result; }
  std::shared_ptr<const server::TradingView> trading_view() const override {
    auto result = std::make_shared<server::TradingView>(); result->playbooks_json = saved_catalogue.dump(); return result;
  }
};
server::ApiResponse call(server::BacktestHost& host, const server::ApiRequest& request, bool capture = true) {
  BacktestSource source;
  server::ApiResponse response;
  EXPECT_TRUE(host.handle(request, source, [&](server::ApiResponse value) { response = std::move(value); }));
  if (capture) test::capture_contract("backtests", request.method, request.target, response);
  return response;
}
TEST(Backtest, CustomFundedRulesCarryPayoutConsistencyAndBufferThroughTheRunner) {
  auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  const json payouts{{"qualifying_profit", "50"}, {"qualifying_days", 1}, {"withdrawal_percent", 100},
      {"split_percent", 80}, {"minimum", "10"}, {"caps", {"500", "1000"}},
      {"consistency_percents", {20, 25, 30}}, {"buffer", "2100"}, {"buffer_payouts", 3}};
  json rules{{"phase", "funded"}, {"payouts", payouts}, {"max_drawdown", "3000"}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "50000"}, {"rules", settings}}},
                                  {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  auto parsed = parse(rules);
  EXPECT_EQ(parsed.config.rules.phase, trading::Phase::Funded);
  EXPECT_EQ(parsed.config.rules.payouts.consistency_percents, (std::vector<std::int64_t>{20, 25, 30}));
  EXPECT_EQ(parsed.config.rules.payouts.buffer, Money::parse("2100"));
  EXPECT_EQ(parsed.config.rules.payouts.buffer_payouts, 3);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"buffer", "-1"}, {"buffer", 1}, {"buffer_payouts", 101}, {"buffer_payouts", 1.5},
      {"consistency_percents", {0}}, {"consistency_percents", {101}}, {"consistency_percents", {40.5}},
      {"consistency_percents", {"40"}}, {"consistency_percents", 40}, {"unknown", 1}}) {
    auto invalid = rules;
    invalid["payouts"][key] = value;
    EXPECT_THROW((void)parse(invalid), std::exception) << key << ": " << value;
  }
  auto invalid = rules;
  invalid["phase"] = "evaluation";
  EXPECT_THROW((void)parse(invalid), std::invalid_argument);
  invalid = rules; invalid.erase("payouts");
  EXPECT_THROW((void)parse(invalid), std::invalid_argument);
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  parsed.days = request_for({file}).days;
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(parsed, storage.directory / "funded", cancel);
  EXPECT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_FALSE(report.at("attempts").empty());
  EXPECT_EQ(report.at("attempts").at(0).at("outcome"), "open");
}

TEST(Backtest, CustomFundedSizeScalingAcceptsExactMoneyAndValidatesTypesAndRanges) {
  const auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  json rules{{"phase", "funded"}, {"payouts", {{"qualifying_days", 1}}},
      {"flat_time", "15:45"}, {"no_overnight", true},
      {"time_limit_days", 0}, {"inactivity_days", 14}, {"underlyings", {"SPX"}},
      {"trading_start", "09:30"}, {"trading_end", "16:00"}, {"scaling", {{{"profit", "0.00"}, {"contracts", 2}}}},
      {"size_scaling", {{"profit_percent", 1}, {"payouts", 0}, {"days", 2},
          {"increase_percent", 25}, {"max_balance", "20000.000001"}}}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"}, {"rules", settings}}},
        {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  const auto parsed = parse(rules);
  ASSERT_TRUE(parsed.config.rules.size_scaling);
  EXPECT_EQ(parsed.config.rules.size_scaling->max_balance, Money::parse("20000.000001"));
  EXPECT_EQ(parsed.config.rules.flat_time, 945); EXPECT_TRUE(parsed.config.rules.no_overnight);
  EXPECT_EQ(parsed.config.rules.inactivity_days, 14);
  EXPECT_EQ(parsed.config.rules.underlyings, std::vector<std::string>{"SPX"});
  EXPECT_EQ(parsed.config.rules.trading_start, 570); EXPECT_EQ(parsed.config.rules.trading_end, 960);
  EXPECT_EQ(parsed.config.rules.scaling, (std::vector<trading::ScalingStep>{{Money{}, 2}}));
  auto timed = rules; timed["time_limit_days"] = 30;
  EXPECT_THROW((void)parse(timed), trading::TradingError);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"days", 0}, {"days", 367}, {"payouts", -1}, {"profit_percent", 101}, {"increase_percent", 0},
      {"max_balance", "9999"}, {"days", 1.5}, {"payouts", "0"}, {"max_balance", 20000}, {"unknown", 1}}) {
    auto bad = rules; bad["size_scaling"][key] = value;
    EXPECT_THROW((void)parse(bad), std::exception);
  }
  for (const auto* phase : {"evaluation", "verification"}) {
    auto evaluation = rules; evaluation["phase"] = phase; evaluation.erase("payouts"); evaluation["profit_target"] = "100";
    EXPECT_THROW((void)parse(evaluation), trading::TradingError);
  }
  rules["size_scaling"] = nullptr;
  EXPECT_FALSE(parse(rules).config.rules.size_scaling);
}

TEST(Backtest, CustomScalingRulesKeepExactThresholdsAndValidateSteps) {
  const auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  json rules{{"profit_target", "100"}, {"scaling", {{{"profit", "0"}, {"contracts", 2}},
      {{"profit", "50.000001"}, {"contracts", 3}}}}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"}, {"rules", settings}}},
                                  {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  const auto parsed = parse(rules);
  EXPECT_EQ(parsed.config.rules.scaling, (std::vector<trading::ScalingStep>{{Money{}, 2}, {Money::parse("50.000001"), 3}}));
  for (const auto& bad : std::vector<json>{nullptr, 3, {{{"profit", 0}, {"contracts", 2}}},
      {{{"profit", "0"}, {"contracts", 2.5}}}, {{{"profit", "1"}, {"contracts", 2}}},
      {{{"profit", "0"}, {"contracts", 2}, {"unknown", 1}}}}) {
    auto invalid = rules; invalid["scaling"] = bad;
    EXPECT_THROW((void)parse(invalid), std::exception);
  }
}

TEST(Backtest, VerificationKeepsScalingTimeRulesAndExactProgramCosts) {
  const auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  json rules{{"phase", "verification"}, {"profit_target", "100"}, {"time_limit_days", 30},
      {"scaling", {{{"profit", "0"}, {"contracts", 2}}}}, {"size_scaling", nullptr},
      {"evaluation_fee", "100.000001"}, {"reset_fee", "25.000002"}, {"activation_fee", "50.000003"}, {"max_resets", 2}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"}, {"rules", settings}}},
        {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  auto parsed = parse(rules);
  EXPECT_EQ(parsed.config.rules.phase, trading::Phase::Verification);
  EXPECT_EQ(parsed.config.rules.time_limit_days, 30);
  EXPECT_EQ(parsed.config.rules.scaling.front().contracts, 2);
  EXPECT_EQ(parsed.config.rules.evaluation_fee, Money::parse("100.000001"));
  EXPECT_EQ(parsed.config.rules.reset_fee, Money::parse("25.000002"));
  EXPECT_EQ(parsed.config.rules.activation_fee, Money::parse("50.000003"));
  EXPECT_EQ(parsed.config.rules.max_resets, 2);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"evaluation_fee", -1}, {"evaluation_fee", "-1"}, {"reset_fee", 25}, {"activation_fee", "-0.000001"},
      {"max_resets", 1.5}, {"max_resets", -1}, {"max_resets", 1000001}, {"phase", "unknown"},
      {"payouts", {{"qualifying_days", 1}}}, {"profit_target", "0"}}) {
    auto bad = rules; bad[key] = value;
    EXPECT_THROW((void)parse(bad), std::exception) << key;
  }
  for (const auto* preset : {"two-step-verify-25k", "two-step-funded-25k"})
    EXPECT_THROW((void)server::parse_backtest({{"playbook", "batch"}, {"plan", preset}, {"scenarios", 1}, {"seed", 0}},
        definitions, scenarios, {}), std::invalid_argument);
  test::RecordingFile storage;
  parsed.days = request_for({recorded_day(storage.directory, {2026, 9, 14})}).days;
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(parsed, storage.directory / "verification", cancel);
  EXPECT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_FALSE(report.at("attempts").empty());
}

TEST(Backtest, VerificationTradeObjectivesHoldPassesAndRecoverRecordedEvidence) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14}, false, false, 100);
  const std::atomic_bool cancel{false};
  for (const auto* phase : {"evaluation", "verification"}) {
    for (const auto* objective : {"none", "min_trades", "trade_consistency_percent", "microscalp_percent"}) {
      SCOPED_TRACE(std::string(phase) + "/" + objective);
      json rules{{"phase", phase}, {"profit_target", "50"}, {"profit_basis", "balance"},
          {"min_hold_seconds", 60}, {"no_hedging", true}, {"no_counter_positions", true}, {"max_volume_percent", 25}};
      if (std::string_view(objective) == "min_trades") rules[objective] = 2;
      if (std::string_view(objective) == "trade_consistency_percent") rules[objective] = 50;
      if (std::string_view(objective) == "microscalp_percent") {
        rules[objective] = 25; rules["microscalp_seconds"] = 3600;
      }
      auto request = server::parse_backtest({{"playbook", "batch"},
          {"plan", {{"initial_cash", "10000"}, {"rules", rules}}}, {"days", {{{"file", file.string()}}}}}, catalogue(), {}, {}, false);
      request.analytics.fallback_rate = 0;
      const auto root = storage.directory / (std::string(phase) + "-" + objective);
      const auto report = server::run_backtest(request, root, cancel);
      ASSERT_EQ(report.at("status"), "completed") << report.dump();
      ASSERT_EQ(report.at("attempts").size(), 1U);
      const auto& attempt = report.at("attempts")[0];
      EXPECT_EQ(attempt.at("outcome"), std::string_view(objective) == "none" ? "passed" : "open");
      // The recorded playbook close meets minimum hold in both phases.
      EXPECT_EQ(attempt.at("fills").size(), 4U);
      const auto journal = root / attempt.at("journal").get<std::string>();
      auto recovered = trading::TradingSession::recover(trading::FileJournal::read(journal.string()));
      const auto snapshot = recovered.snapshot();
      const auto& e = snapshot->evaluation;
      if (rules.contains("min_trades")) {
        EXPECT_EQ(e.closed_trades, 1U);
      }
      if (rules.contains("trade_consistency_percent")) {
        ASSERT_TRUE(e.best_trade);
        EXPECT_EQ(e.best_trade->pnl, Money::parse("77.40"));
      }
      if (rules.contains("microscalp_percent")) {
        EXPECT_GT(e.short_profit, Money{});
      }
      const auto verified = server::verify_run(journal);
      EXPECT_TRUE(verified.matched) << verified.message;
    }
  }
}

TEST(Backtest, ScalingRefusesAutomaticEntriesAndTheJournalVerifies) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  auto request = request_for({file});
  request.config.rules.plan = "Scaling playbook";
  request.config.rules.scaling = {{Money{}, 1}};
  const std::atomic_bool cancel{false};
  const auto root = storage.directory / "scaling";
  const auto report = server::run_backtest(request, root, cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  ASSERT_FALSE(report.at("attempts").empty());
  for (const auto& attempt : report.at("attempts")) {
    // Playbooks stop at the preview gate, before submitting an order. The report
    // keeps that gate's message; the journal still records the scaling rule and limit.
    EXPECT_TRUE(attempt.at("fills").empty());
    EXPECT_EQ(attempt.at("entry_reasons").at("batch:SPX").get<std::string>().find("Scaling plan:"), 0U);
    const auto path = root / attempt.at("journal").get<std::string>();
    const auto verified = server::verify_run(path);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

TEST(Backtest, CustomPlansAcceptAndValidateAccountMargin) {
  auto definitions = catalogue();
  const auto& scenarios = providers::builtin_scenarios();
  json rules{{"profit_target", "100"}, {"buying_power", true}, {"account_type", "ira"}, {"house_margin_percent", 25}, {"pm_vol_shock", 5}};
  const auto parse = [&](const json& settings) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"}, {"rules", settings}}},
                                  {"scenarios", 1}, {"seed", 0}}, definitions, scenarios, {});
  };
  const auto parsed = parse(rules);
  EXPECT_EQ(parsed.config.rules.account_type, trading::AccountType::Ira);
  EXPECT_EQ(parsed.config.rules.house_margin_percent, 25);
  EXPECT_EQ(parsed.config.rules.pm_vol_shock, 5);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"account_type", "unknown"}, {"house_margin_percent", 401}, {"pm_vol_shock", 51},
      {"pm_vol_shock", 1.5}, {"margin", "portfolio"}, {"buying_power", false}}) {
    auto invalid = rules;
    invalid[key] = value;
    EXPECT_THROW((void)parse(invalid), std::exception);
  }
}

TEST(BacktestApi, ContractFixture) {
  test::RecordingFile storage;
  const server::BacktestHost::Options options{storage.directory / "reports", storage.directory, {}, {}, {}, true};
  std::string id;
  {
    server::BacktestHost host(options);
    EXPECT_EQ(call(host, {"GET", "/api/backtests"}).status, 200);
    const json body{{"playbook", "batch"}, {"plan", "eod-50k"}, {"scenarios", 2}, {"seed", "1"}};
    const auto started = call(host, {"POST", "/api/backtests", body.dump()});
    ASSERT_EQ(started.status, 202) << started.body;
    id = json::parse(started.body).at("id");
    EXPECT_EQ(call(host, {"POST", "/api/backtests", body.dump()}).status, 409);
    EXPECT_EQ(call(host, {"DELETE", "/api/backtests/" + id + "?purge=true"}).status, 409);
    EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=" + id + ",000099"}).status, 409);
    EXPECT_EQ(call(host, {"PUT", "/api/backtests/" + id, "{\"keep\":true}"}).status, 200);
    EXPECT_EQ(call(host, {"DELETE", "/api/backtests/" + id}).status, 200);
    // Malformed routes and unsupported methods are not OpenAPI operations.
    EXPECT_EQ(call(host, {"GET", "/api/backtests/../../escape"}, false).status, 404);
  }
  server::BacktestHost reopened(options);
  const auto saved = call(reopened, {"GET", "/api/backtests/" + id});
  ASSERT_EQ(saved.status, 200) << saved.body;
  EXPECT_EQ(json::parse(saved.body).at("status"), "cancelled");
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/" + id}).status, 409);
  EXPECT_EQ(json::parse(saved.body).at("keep"), true);
  EXPECT_GT(json::parse(saved.body).at("bytes").get<std::uintmax_t>(), 0U);
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/" + id + "?purge=true"}).status, 200);
  EXPECT_FALSE(std::filesystem::exists(options.directory / id));
  EXPECT_EQ(call(reopened, {"GET", "/api/backtests/" + id}).status, 404);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests"}, false).status, 405);
  server::BacktestHost disabled({storage.directory / "disabled", {}, {}, {}, {}, false});
  EXPECT_EQ(call(disabled, {"POST", "/api/backtests", "{}"}).status, 403);
}

TEST(BacktestApi, KeepReplyReportsFinishedRunBytesLikeGet) {
  test::RecordingFile storage;
  const server::BacktestHost::Options options{storage.directory / "reports", storage.directory, {}, {}, {}, true};
  server::BacktestHost host(options);
  const auto input_file = recorded_day(storage.directory, {2026, 9, 14});
  const json request_body{{"playbook", "batch"}, {"plan", "eod-50k"}, {"days", {{{"file", input_file.filename().string()}}}}};
  const auto started = call(host, {"POST", "/api/backtests", request_body.dump()}, false);
  ASSERT_EQ(started.status, 202) << started.body;
  const auto run_id = json::parse(started.body).at("id").get<std::string>();
  json final_state;
  ASSERT_TRUE(test::recording_eventually([&] {
    const auto poll = call(host, {"GET", "/api/backtests/" + run_id}, false);
    if (poll.status != 200) return false;
    final_state = json::parse(poll.body);
    return final_state.at("status") != "running" && final_state.at("status") != "cancelling";
  })) << final_state.dump();
  ASSERT_EQ(final_state.at("status"), "completed") << final_state.dump();
  ASSERT_GT(final_state.at("bytes").get<std::uintmax_t>(), 0U);

  const auto pinned = call(host, {"PUT", "/api/backtests/" + run_id, "{\"keep\":true}"}, false);
  ASSERT_EQ(pinned.status, 200) << pinned.body;
  const auto pinned_state = json::parse(pinned.body);
  EXPECT_EQ(pinned_state.at("status"), "completed");
  EXPECT_EQ(pinned_state.at("keep"), true);
  EXPECT_GT(pinned_state.at("bytes").get<std::uintmax_t>(), 0U);
  const auto fetched_state = json::parse(call(host, {"GET", "/api/backtests/" + run_id}, false).body);
  EXPECT_EQ(pinned_state.at("bytes"), fetched_state.at("bytes"));
}

TEST(BacktestApi, ListingSummarizesSavedReportIdentityWithoutCopyingTrades) {
  test::RecordingFile storage;
  const auto root = storage.directory / "000001";
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request_for({recorded_day(storage.directory, {2026, 9, 14})}), root, cancel);
  ASSERT_EQ(report.at("status"), "completed");
  server::write_backtest_report(root / "report.json", report);
  const json state{{"id", "000001"}, {"status", "completed"}, {"phase", "finished"}, {"completed", 2}, {"total", 2},
      {"label", server::kBacktestLabel}, {"report", nullptr}};
  server::write_backtest_report(root / "state.json", state);
  server::BacktestHost host({storage.directory, {}, {}, {}, {}, true});
  const auto listing = call(host, {"GET", "/api/backtests"});
  ASSERT_EQ(listing.status, 200) << listing.body;
  const auto run = json::parse(listing.body).at("runs")[0];
  EXPECT_EQ(run.at("playbook"), (json{{"id", "batch"}, {"version", 1}}));
  EXPECT_EQ(run.at("summary"), report.at("summary"));
  EXPECT_TRUE(run.at("report").is_null());
  EXPECT_EQ(json::parse(call(host, {"GET", "/api/backtests/000001"}).body).at("report"), report);
}

TEST(BacktestApi, VolumeRuleAcceptsScenariosButRejectsRecordingsWithoutVolume) {
  test::RecordingFile storage;
  const auto scenarios = storage.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  const auto scenario = volume_scenario(scenarios);
  const auto missing = recorded_day(storage.directory, {2026, 9, 17});
  server::BacktestHost host({storage.directory / "reports", storage.directory, scenarios, {}, {}, true});
  json body{{"playbook", "batch"},
      {"plan", {{"initial_cash", "10000"}, {"rules", {{"profit_target", "100"}, {"max_volume_percent", 10}}}}},
      {"days", {{{"scenario", scenario.id}, {"seed", "5"}}, {{"file", missing.filename().string()}}}}};
  const auto refused = call(host, {"POST", "/api/backtests", body.dump()}, false);
  EXPECT_EQ(refused.status, 400) << refused.body;
  EXPECT_NE(refused.body.find("current-date option volume"), std::string::npos);

  body.erase("days");
  body.update({{"scenarios", 1}, {"scenario", scenario.id}, {"seed", "5"}});
  const auto started = call(host, {"POST", "/api/backtests", body.dump()}, false);
  ASSERT_EQ(started.status, 202) << started.body;
  const auto id = json::parse(started.body).at("id").get<std::string>();
  json state;
  ASSERT_TRUE(test::recording_eventually([&] {
    const auto poll = call(host, {"GET", "/api/backtests/" + id}, false);
    if (poll.status != 200) return false;
    state = json::parse(poll.body);
    return state.at("status") != "running" && state.at("status") != "cancelling";
  })) << state.dump();
  EXPECT_EQ(state.at("status"), "completed") << state.dump();
  EXPECT_EQ(state.at("report").at("days").size(), 1U);
}
TEST(BacktestApi, ReplayScopeRequiredForStartAndCancelAndReadForProgress) {
  server::WritePolicy policy;
  policy.require_token = true;
  policy.tokens = {{"reader", {"read"}, "reader-secret"}, {"trader", {"read", "trade:*"}, "trader-secret"}, {"runner", {"read", "replay"}, "runner-secret"}};
  for (const auto& route : {"/api/backtests", "/api/backtests/000001"}) {
    for (const auto& method : {"POST", "PUT", "DELETE"}) {
      server::ApiRequest request{method, route, std::string(method) == "POST" ? "{}" : ""};
      request.content_type = "application/json";
      for (const auto& secret : {"reader-secret", "trader-secret"}) {
        request.authorization = std::string("Bearer ") + secret;
        const auto rejected = server::check_api_write(request, policy);
        ASSERT_TRUE(rejected); EXPECT_EQ(rejected->status, 403);
      }
      request.authorization = "Bearer runner-secret";
      EXPECT_FALSE(server::check_api_write(request, policy));
    }
    server::ApiRequest read{"GET", route}; read.authorization = "Bearer reader-secret";
    EXPECT_FALSE(server::check_api_write(read, policy));
  }
}

TEST(Backtest, DailyRowsIncludePlanLocks) {
  test::RecordingFile storage;
  auto request = request_for({recorded_day(storage.directory, {2026, 9, 14}), recorded_day(storage.directory, {2026, 9, 15})});
  request.config.rules.daily_loss_limit = Money::parse("10");
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "locked", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const auto& rows = report.at("attempts")[0].at("day_rows");
  ASSERT_EQ(rows.size(), 2U);
  for (const auto& row : rows) {
    EXPECT_EQ(row.at("day_lock"), "DAILY_LOSS_LIMIT");
    EXPECT_EQ(row.at("outcome"), "open");
    EXPECT_EQ(row.at("trades_opened"), 1);
    EXPECT_EQ(row.at("trades_closed"), 1);
    bool locked = false;
    for (const auto& trip : row.at("rule_trips")) if (trip.at("type") == "day_locked") locked = true;
    EXPECT_TRUE(locked);
  }
}
json comparison_report(const json& days) {
  return {{"schema", 1}, {"playbook", {{"id", "batch"}, {"version", 1}}}, {"config", {{"initial_cash", 50000000000LL}}},
      {"summary", json::object()}, {"days", days}};
}
json comparison_day(const std::string& date, const json& pnl) {
  return {{"date", date}, {"pnl", pnl}, {"input", {{"kind", "scenario"}, {"id", "fixture"}, {"seed", "1"}}}};
}
TEST(Backtest, ComparisonAlignsDatesAndSumsMicroDollarsWithoutInventingMarks) {
  auto first = comparison_report({comparison_day("2026-09-14", "0.000001"), comparison_day("2026-09-15", "-1.000003")});
  auto second = comparison_report({comparison_day("2026-09-14", "0.000002"), comparison_day("2026-09-16", "2.000001")});
  json runs = {{{"id", "000001"}, {"status", "completed"}, {"report", first}}, {{"id", "000002"}, {"status", "cancelled"}, {"report", second}}};
  auto result = server::compare_backtests(runs);
  EXPECT_EQ(result.at("different_inputs"), true);
  EXPECT_EQ(result.at("different_plans"), false);
  EXPECT_EQ(result.at("daily")[1].at("pnl").at("000002"), nullptr);
  EXPECT_EQ(result.at("combined").at("curve")[0].at("pnl"), "0.000003");
  EXPECT_EQ(result.at("combined").at("curve")[2].at("cumulative"), "1.000001");
  EXPECT_EQ(result.at("combined").at("max_drawdown"), "1.000003");
  EXPECT_EQ(result.at("combined").at("worst_days")[0].at("date"), "2026-09-15");
  EXPECT_DOUBLE_EQ(result.at("combined").at("day_win_rate"), 2.0 / 3.0);
  EXPECT_EQ(result.at("combined").at("daily_pnl").at("mean"), "0.333334");
  runs[1]["report"] = first;
  EXPECT_EQ(server::compare_backtests(runs).at("apples_to_oranges"), false);
  runs[1]["report"]["config"]["initial_cash"] = 1000000;
  runs[1]["report"]["days"][0]["pnl"] = nullptr;
  result = server::compare_backtests(runs);
  EXPECT_EQ(result.at("different_plans"), true);
  EXPECT_EQ(result.at("combined").at("curve")[0].at("pnl"), nullptr);
  EXPECT_EQ(result.at("combined").at("curve")[1].at("cumulative"), nullptr);
  EXPECT_EQ(result.at("combined").at("max_drawdown"), nullptr);
}
void saved_fixture(const std::filesystem::path& directory, const std::string& id, bool keep = false, const std::string& status = "completed") {
  std::filesystem::create_directories(directory / id);
  server::write_backtest_report(directory / id / "state.json", {{"id", id}, {"status", status}, {"keep", keep},
      {"phase", "finished"}, {"completed", 0}, {"total", 0}, {"label", server::kBacktestLabel}, {"report", nullptr}});
}
TEST(BacktestApi, RetentionRemovesOldestUnpinnedAndKeepsInterruptedPinsAcrossRestart) {
  test::RecordingFile storage;
  const auto directory = storage.directory / "reports";
  saved_fixture(directory, "000001", true, "running");
  saved_fixture(directory, "000002"); saved_fixture(directory, "000003"); saved_fixture(directory, "000004");
  server::BacktestHost::Options options{directory, storage.directory, {}, {}, {}, true, 1};
  {
    server::BacktestHost host(options);
    const auto listing = json::parse(call(host, {"GET", "/api/backtests"}).body);
    EXPECT_EQ(listing.at("runs")[3].at("status"), "interrupted");
    const auto started = call(host, {"POST", "/api/backtests", json{{"playbook", "batch"}, {"plan", "eod-50k"},
        {"days", {{{"file", recorded_day(storage.directory, {2026, 9, 14}).filename().string()}}}}}.dump()});
    ASSERT_EQ(started.status, 202) << started.body;
    EXPECT_EQ(json::parse(started.body).at("id"), "000005");
    EXPECT_TRUE(std::filesystem::exists(directory / "000001"));
    EXPECT_FALSE(std::filesystem::exists(directory / "000002"));
    EXPECT_FALSE(std::filesystem::exists(directory / "000003"));
    EXPECT_TRUE(std::filesystem::exists(directory / "000004"));
  }
  server::BacktestHost reopened(options);
  EXPECT_EQ(json::parse(call(reopened, {"GET", "/api/backtests/000001"}).body).at("keep"), true);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests/000004", "{\"keep\":true}"}).status, 200);
  EXPECT_EQ(call(reopened, {"PUT", "/api/backtests/000004", "{\"keep\":1}"}).status, 400);
  EXPECT_EQ(call(reopened, {"DELETE", "/api/backtests/000005?purge=true"}).status, 200);
  options.keep = 0;
  server::BacktestHost unlimited(options);
  const auto started = call(unlimited, {"POST", "/api/backtests", json{{"playbook", "batch"}, {"plan", "eod-50k"},
      {"days", {{{"file", "2026-09-14.oprec"}}}}}.dump()});
  EXPECT_EQ(json::parse(started.body).at("id"), "000006");
  EXPECT_TRUE(std::filesystem::exists(directory / "000004"));
}
TEST(BacktestApi, PurgeAndKeepRefuseSymlinkEscapesAndUnknownRuns) {
  test::RecordingFile storage;
  const auto directory = storage.directory / "reports";
  saved_fixture(directory, "000001");
  saved_fixture(storage.directory / "outside", "000002");
  std::filesystem::create_directory_symlink(storage.directory / "outside/000002", directory / "000002");
  server::BacktestHost host({directory, {}, {}, {}, {}, true});
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/000002?purge=true"}).status, 409);
  EXPECT_EQ(call(host, {"PUT", "/api/backtests/000002", "{\"keep\":true}"}).status, 409);
  std::filesystem::create_directory_symlink(storage.directory / "outside", directory / "000001/escape");
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/000001?purge=true"}).status, 409);
  EXPECT_TRUE(std::filesystem::exists(storage.directory / "outside/000002/state.json"));
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/000009?purge=true"}).status, 404);
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests/../outside?purge=true"}, false).status, 404);
  EXPECT_EQ(call(host, {"DELETE", "/api/backtests?purge=true"}).status, 400);
}
TEST(BacktestApi, ComparisonAcceptsLegacyReportsAndRejectsInvalidSelection) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const std::atomic_bool cancel{false};
  auto report = server::run_backtest(request_for({file}), storage.directory / "source", cancel);
  report["schema"] = 1; report.erase("input_set");
  for (auto& attempt : report["attempts"]) attempt.erase("day_rows");
  for (const auto* id : {"000001", "000002"}) {
    saved_fixture(storage.directory, id);
    server::write_backtest_report(storage.directory / id / "report.json", report);
  }
  server::BacktestHost host({storage.directory, {}, {}, {}, {}, true});
  const auto saved = json::parse(call(host, {"GET", "/api/backtests/000001"}).body);
  EXPECT_EQ(saved.at("report"), report);
  std::uintmax_t size = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(storage.directory / "000001"))
    if (entry.is_regular_file()) size += entry.file_size();
  EXPECT_EQ(saved.at("bytes"), size);
  const auto compared = call(host, {"GET", "/api/backtests/compare?ids=000001,000002"});
  ASSERT_EQ(compared.status, 200) << compared.body;
  EXPECT_EQ(json::parse(compared.body).at("apples_to_oranges"), false);
  for (const auto* query : {"", "?ids=", "?ids=000001", "?ids=000001,000001", "?ids=000001,000002,", "?ids=../../escape,000001",
      "?ids=000001,000002,000003,000004,000005,000006,000007,000008,000009"}) {
    EXPECT_EQ(call(host, {"GET", std::string("/api/backtests/compare") + query}).status, 400);
  }
  EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=000001,000099"}).status, 404);
  saved_fixture(storage.directory, "000003");
  EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=000001,000003"}).status, 422);
}
}  // namespace

TEST(Backtest, CustomContractCap) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const auto parse = [&](json cap) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"},
        {"rules", {{"profit_target", "100"}, {"max_contracts_held", cap}}}}}, {"days", {{{"file", file.string()}}}}}, catalogue(), {}, {}, false);
  };
  EXPECT_EQ(parse(5).config.rules.max_contracts_held, 5);
  EXPECT_THROW((void)parse(100001), std::exception);
  EXPECT_THROW((void)parse(1.5), std::exception);
}

TEST(Backtest, CustomStopAndRiskRules) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  const auto parse = [&](const json& rules) {
    return server::parse_backtest({{"playbook", "batch"}, {"plan", {{"initial_cash", "10000"}, {"rules", rules}}},
                                  {"days", {{{"file", file.string()}}}}}, catalogue(), {}, {}, false);
  };
  const json rules{{"profit_target", "100"}, {"no_hedging", true}, {"no_counter_positions", true}, {"require_stop_loss", true}, {"max_trade_risk", "123.456789"}, {"max_trade_risk_percent", 10}, {"trade_consistency_percent", 40}, {"min_trades", 12}, {"min_hold_seconds", 60}, {"microscalp_seconds", 30}, {"microscalp_percent", 25}};
  const auto result = parse(rules);
  EXPECT_TRUE(result.config.rules.no_hedging);
  EXPECT_TRUE(result.config.rules.no_counter_positions);
  EXPECT_TRUE(result.config.rules.require_stop_loss);
  EXPECT_EQ(result.config.rules.max_trade_risk, Money::parse("123.456789"));
  EXPECT_EQ(result.config.rules.max_trade_risk_percent, 10);
  EXPECT_EQ(result.config.rules.trade_consistency_percent, 40);
  EXPECT_EQ(result.config.rules.min_trades, 12);
  EXPECT_EQ(result.config.rules.min_hold_seconds, 60);
  EXPECT_EQ(result.config.rules.microscalp_seconds, 30);
  EXPECT_EQ(result.config.rules.microscalp_percent, 25);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"min_hold_seconds", 3601}, {"min_hold_seconds", 1.5}, {"microscalp_seconds", 0}, {"microscalp_percent", 101}, {"min_trades", 10001}, {"min_trades", 1.5}, {"trade_consistency_percent", 101}, {"trade_consistency_percent", 1.5},
      {"max_trade_risk", "-1"}, {"max_trade_risk", 100}, {"max_trade_risk_percent", 101},
      {"max_trade_risk_percent", 1.5}, {"require_stop_loss", 1}, {"no_hedging", 1}, {"no_counter_positions", "true"}}) {
    auto invalid = rules; invalid[key] = value;
    EXPECT_THROW((void)parse(invalid), std::exception);
  }
}

TEST(Backtest, VolumeRuleRejectsMissingDataAndGatesRecordedPlaybookEntries) {
  test::RecordingFile storage;
  const auto missing = recorded_day(storage.directory, {2026, 9, 14});
  const auto present = recorded_day(storage.directory, {2026, 9, 15}, false, false, 3);
  const auto parse = [&](const std::filesystem::path& file, const json& percent) {
    return server::parse_backtest({{"playbook", "batch"},
        {"plan", {{"initial_cash", "10000"}, {"rules", {{"profit_target", "100"}, {"max_volume_percent", percent}}}}},
        {"days", {{{"file", file.string()}}}}}, catalogue(), {}, {}, false);
  };
  try {
    (void)parse(missing, 10);
    FAIL() << "Expected a clear unsupported-volume error";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string(e.what()).find("current-date option volume"), std::string::npos);
  }
  EXPECT_NO_THROW((void)parse(missing, 0));
  for (const auto& value : {json(-1), json(101), json(1.5), json(true)}) {
    EXPECT_THROW((void)parse(present, value), std::exception);
  }
  const auto request = parse(present, 10);
  EXPECT_EQ(request.config.rules.max_volume_percent, 10);
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "volume-report", cancel);
  EXPECT_TRUE(report.at("days")[0].at("fills").empty());
  EXPECT_NE(report.at("days")[0].at("entry_reasons").dump().find("volume"), std::string::npos);
}

namespace {
json joint_catalogue() {
  auto result = catalogue();
  auto first = result.at("definitions").at("batch").at("versions")[0];
  first["id"] = "alpha";
  first["name"] = "Alpha put";
  first["structure"]["template"]["target"] = {{"mode", "delta"}, {"value", 15}};
  auto second = first;
  second["id"] = "zeta";
  second["name"] = "Zeta call";
  second["structure"]["template"]["type"] = "call";
  result["definitions"] = {{"alpha", {{"versions", {first}}, {"deleted", false}}},
      {"zeta", {{"versions", {second}}, {"deleted", false}}}};
  return result;
}
TEST(Backtest, JointSelectorsValidateDistinctIdsAndPinLatest) {
  auto definitions = joint_catalogue();
  auto next = definitions["definitions"]["alpha"]["versions"][0];
  next["version"] = 2;
  definitions["definitions"]["alpha"]["versions"].push_back(next);
  const auto scenarios = providers::builtin_scenarios();
  const json good{{"playbooks", {"zeta@1", "alpha"}}, {"plan", "eod-50k"}, {"scenarios", 1}, {"seed", "1"}};
  const auto parsed = server::parse_backtest(good, definitions, scenarios, {});
  EXPECT_TRUE(parsed.playbook.empty());
  EXPECT_EQ(parsed.playbooks.at("definitions").size(), 2U);
  EXPECT_EQ(parsed.playbooks.at("definitions").at("alpha").at("versions").back().at("version"), 2);
  EXPECT_EQ(parsed.playbooks.at("modes").at("main"), (json{{"alpha", "auto"}, {"zeta", "auto"}}));
  for (const auto& selection : {json::array(), json{"alpha"}, json{"alpha", "alpha@2"}, json{"alpha", "missing"},
      json{"alpha@0", "zeta"}, json{"alpha@3", "zeta"}, json{"alpha@x", "zeta"}, json{"alpha", 2}, json("alpha,zeta"),
      json{"a", "b", "c", "d", "e", "f", "g", "h", "i"}}) {
    auto bad = good; bad["playbooks"] = selection;
    EXPECT_THROW((void)server::parse_backtest(bad, definitions, scenarios, {}), std::invalid_argument) << bad.dump();
  }
  for (const bool both : {false, true}) {
    auto bad = good;
    if (both) bad["playbook"] = "alpha";
    else bad.erase("playbooks");
    try { (void)server::parse_backtest(bad, definitions, scenarios, {}); ADD_FAILURE(); }
    catch (const std::invalid_argument& error) { EXPECT_STREQ(error.what(), "Supply exactly one of playbook or playbooks"); }
  }
  auto eight = good;
  eight["playbooks"] = json::array();
  for (int index = 0; index < 8; ++index) {
    const auto id = "book-" + std::to_string(index);
    auto definition = next; definition["id"] = id; definition["version"] = 1;
    definitions["definitions"][id] = {{"versions", {definition}}, {"deleted", false}};
    eight["playbooks"].push_back(id);
  }
  EXPECT_EQ(server::parse_backtest(eight, definitions, scenarios, {}).playbooks.at("definitions").size(), 8U);
  auto single = good; single.erase("playbooks"); single["playbook"] = "alpha@1";
  const auto pinned = server::parse_backtest(single, definitions, scenarios, {});
  EXPECT_EQ(pinned.playbook, "alpha");
  EXPECT_EQ(pinned.playbooks.at("definitions").size(), 1U);
  EXPECT_EQ(pinned.playbooks.at("definitions").at("alpha").at("versions").size(), 1U);
}
TEST(Backtest, TwentyDeltaSpyCallCreditSpreadTradesAloneAndJointly) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile storage;
  auto definitions = joint_catalogue();
  for (auto& [id, item] : definitions["definitions"].items()) {
    auto& setup = item["versions"][0];
    setup["underlyings"] = {"SPY"};
    setup["structure"]["template"]["target"]["value"] = 20;
    setup["structure"]["expiry"] = {{"min", 0}, {"max", 7}};
    setup["sizing"]["units"] = 5;
    setup["window"]["start"] = "09:45";
    setup["window"]["end"] = "09:46";
    setup["management"] = {{"close_by", "09:47"}, {"take_profit_percent", 50}, {"stop_credit_multiple", 2}};
  }
  const auto& builtins = providers::builtin_scenarios();
  const auto reversal = std::find_if(builtins.begin(), builtins.end(), [](const auto& s) { return s.id == "reversal"; });
  ASSERT_NE(reversal, builtins.end());
  auto source = json::parse(reversal->source);
  source["symbols"] = {"SPY"};
  const auto file = storage.directory / "spy-reversal.json";
  std::ofstream(file) << source.dump();
  const auto scenario = providers::read_scenario(file);
  const std::atomic_bool cancel{false};
  for (const bool joint : {false, true}) {
    SCOPED_TRACE(joint);
    json body{{"plan", {{"initial_cash", "100000"}, {"rules", {{"profit_target", "10000"}, {"max_drawdown", "10000"}}}}},
        {"days", {{{"scenario", scenario.id}, {"seed", "8642"}, {"date", "2026-09-16"}},
        {{"scenario", scenario.id}, {"seed", "1"}, {"date", "2026-09-17"}}}}};
    if (joint) body["playbooks"] = {"alpha", "zeta"};
    else body["playbook"] = "zeta";
    auto request = server::parse_backtest(body, definitions, {scenario}, {}, false);
    request.workers = 1;
    const auto directory = storage.directory / (joint ? "joint" : "single");
    const auto report = server::run_backtest(request, directory, cancel);
    ASSERT_EQ(report.at("status"), "completed") << report.dump();
    for (const auto& day : report.at("days")) {
      const auto& trades = day.at("trades");
      const auto call = std::find_if(trades.begin(), trades.end(), [](const auto& trade) {
        return trade.at("tag") == "playbook:zeta@v1";
      });
      ASSERT_NE(call, trades.end()) << day.dump();
      EXPECT_FALSE(call->at("closed").is_null());
      ASSERT_FALSE(day.at("fills").empty()) << day.dump();
      EXPECT_EQ(day.at("fills")[0].at("time"), md::format_timestamp(*md::parse_datetime(
          day.at("date").get<std::string>() + "T09:45:00", md::Zone::NewYork)));
      if (joint) { EXPECT_GT(day.at("per_playbook").at("zeta").at("trades").get<int>(), 0); }
    }
    request.workers = 2;
    const auto parallel = storage.directory / (joint ? "joint-parallel" : "single-parallel");
    const auto repeated = server::run_backtest(request, parallel, cancel);
    EXPECT_EQ(report, repeated);
    for (const auto& name : report.at("journals")) {
      const auto journal = name.get<std::string>();
      EXPECT_EQ(bytes(directory / journal), bytes(parallel / journal));
      const auto verified = server::verify_run(directory / journal);
      EXPECT_TRUE(verified.matched) << verified.message;
    }
  }
}
TEST(Backtest, JointGeneratedAccountSharesCapAndIsIdenticalAtOneAndEightWorkers) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile storage;
  const auto definitions = joint_catalogue();
  const json body{{"playbooks", {"zeta@1", "alpha"}},
      {"plan", {{"initial_cash", "50000"}, {"rules", {{"profit_target", "10000"}, {"max_drawdown", "2000"}, {"max_contracts_held", 2}}}}},
      {"scenarios", 2}, {"seed", "18446744073709551610"}};
  auto request = server::parse_backtest(body, definitions, providers::builtin_scenarios(), {});
  for (std::size_t index = 0; index < request.days.size(); ++index) {
    auto source = json::parse(request.days[index].scenario->source);
    source["symbols"] = {"SPX"};
    const auto file = storage.directory / ("joint-scenario-" + std::to_string(index) + ".json");
    std::ofstream(file) << source.dump();
    request.days[index].scenario = providers::read_scenario(file);
  }
  const std::atomic_bool cancel{false};
  request.workers = 1;
  const auto first = server::run_backtest(request, storage.directory / "one", cancel);
  ASSERT_EQ(first.at("status"), "completed") << first.dump();
  EXPECT_EQ(first.at("schema"), 3);
  EXPECT_EQ(first.at("mode"), "joint");
  EXPECT_FALSE(first.contains("playbook"));
  ASSERT_EQ(first.at("playbooks").size(), 2U);
  EXPECT_EQ(first.at("playbooks")[0].at("id"), "alpha");
  EXPECT_EQ(first.at("playbooks")[1].at("id"), "zeta");
  request.workers = test::sanitizer_scale(8U, 2U);
  const auto second = server::run_backtest(request, storage.directory / "eight", cancel);
  EXPECT_EQ(bytes(storage.directory / "one/report.json"), bytes(storage.directory / "eight/report.json"));
  EXPECT_EQ(first.dump(), second.dump());
  for (const auto& name : first.at("journals")) {
    const auto path = name.get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "one" / path), bytes(storage.directory / "eight" / path));
    const auto verified = server::verify_run(storage.directory / "one" / path);
    EXPECT_TRUE(verified.matched) << path << ": " << verified.message;
  }
  for (const auto* collection : {"days", "attempts"}) for (const auto& day : first.at(collection)) {
    const auto& stats = day.at("per_playbook");
    EXPECT_GT(stats.at("alpha").at("trades").get<int>(), 0) << day.dump();
    EXPECT_EQ(stats.at("zeta").at("trades"), 0);
    EXPECT_EQ(stats.at("zeta").at("realised_pnl"), "0.00");
    EXPECT_EQ(stats.at("alpha").at("marked_pnl"), day.at("pnl"));
    EXPECT_NE(stats.at("zeta").at("entry_reasons").at("zeta:SPX").get<std::string>().find("Held options"), std::string::npos) << stats.dump();
    for (const auto& trade : day.at("trades")) { EXPECT_EQ(trade.at("tag"), "playbook:alpha@v1"); }
  }
  EXPECT_EQ(first.at("summary").at("per_playbook").at("alpha").at("trades"), first.at("summary").at("trades"));
  // The other strategy trades under exactly the same cap when it has its own account.
  auto alone = request;
  alone.playbook = "zeta";
  alone.playbooks["definitions"].erase("alpha");
  alone.playbooks["modes"]["main"].erase("alpha");
  const auto single = server::run_backtest(alone, storage.directory / "alone", cancel);
  ASSERT_EQ(single.at("status"), "completed") << single.dump();
  EXPECT_GT(single.at("summary").at("trades").get<int>(), 0);
  EXPECT_EQ(single.at("schema"), 2);
  EXPECT_FALSE(single.contains("mode"));
  EXPECT_FALSE(single.contains("playbooks"));
  EXPECT_FALSE(single.at("summary").contains("per_playbook"));
  EXPECT_FALSE(single.at("days")[0].contains("per_playbook"));

  const auto compared = server::compare_backtests(json::array({{{"id", "000001"}, {"status", "completed"}, {"report", first}},
      {{"id", "000002"}, {"status", "completed"}, {"report", single}}}));
  EXPECT_EQ(compared.at("combined").at("includes_joint"), true);
  EXPECT_EQ(compared.at("runs")[0].at("mode"), "joint");
  EXPECT_EQ(compared.at("runs")[0].at("playbooks")[0].at("name"), "Alpha put");
  for (const auto* id : {"000001", "000002"}) saved_fixture(storage.directory, id);
  server::write_backtest_report(storage.directory / "000001/report.json", first);
  server::write_backtest_report(storage.directory / "000002/report.json", single);
  server::BacktestHost host({storage.directory, {}, {}, {}, {}, true});
  const auto listed = json::parse(call(host, {"GET", "/api/backtests?playbook=alpha@1"}).body);
  ASSERT_EQ(listed.at("runs").size(), 1U);
  EXPECT_EQ(listed.at("runs")[0].at("playbooks"), compared.at("runs")[0].at("playbooks"));
  EXPECT_EQ(json::parse(call(host, {"GET", "/api/backtests?playbook=zeta"}).body).at("runs").size(), 2U);
  EXPECT_TRUE(json::parse(call(host, {"GET", "/api/backtests?playbook=alpha@2"}).body).at("runs").empty());
  EXPECT_EQ(call(host, {"GET", "/api/backtests/compare?ids=000001,000002"}).status, 200);
  EXPECT_EQ(call(host, {"PUT", "/api/backtests/000001", "{\"keep\":true}"}).status, 200);
  EXPECT_EQ(json::parse(call(host, {"GET", "/api/backtests/000001"}).body).at("report"), first);
}
}  // namespace

namespace {
TEST(Backtest, JointOpenTradeAttributionUsesOptionMarksAndFees) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  auto definitions = catalogue();
  auto second = definitions["definitions"]["batch"]["versions"][0];
  second["id"] = "zeta";
  definitions["definitions"]["zeta"] = {{"versions", {second}}, {"deleted", false}};
  definitions["definitions"]["batch"]["versions"][0]["management"]["close_by"] = "15:45";
  auto request = server::parse_backtest({{"playbooks", {"zeta", "batch"}}, {"plan", "eod-50k"},
      {"days", {{{"file", file.string()}}}}}, definitions, {}, {}, false);
  request.analytics.fallback_rate = 0;
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "marked", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  const auto& day = report.at("days")[0];
  const auto& own = day.at("per_playbook").at("batch");
  EXPECT_EQ(own.at("trades"), 0);
  EXPECT_EQ(own.at("open_trades"), 1);
  EXPECT_EQ(own.at("expectancy"), nullptr);
  EXPECT_EQ(own.at("realised_pnl"), "-1.30");
  test::capture_contract("backtests", "GET", "/api/backtests/000098", {200, json{{"id", "000098"}, {"status", "completed"},
      {"phase", "finished"}, {"completed", 2}, {"total", 2}, {"label", server::kBacktestLabel}, {"report", report}}.dump()});
  EXPECT_EQ(own.at("marked_pnl"), day.at("pnl"));
  EXPECT_EQ(report.at("summary").at("per_playbook").at("batch").at("marked_pnl"), own.at("marked_pnl"));
  // Stale option marks must not be presented as known per-playbook P&L.
  const auto stale = recorded_day(storage.directory, {2026, 9, 15}, false, false, {}, true);
  request.days[0].file = stale;
  request.days[0].date = {2026, 9, 15};
  const auto unmarked = server::run_backtest(request, storage.directory / "unmarked", cancel);
  ASSERT_EQ(unmarked.at("status"), "completed") << unmarked.dump();
  EXPECT_EQ(unmarked.at("days")[0].at("pnl"), nullptr);
  EXPECT_EQ(unmarked.at("days")[0].at("per_playbook").at("batch").at("marked_pnl"), nullptr);
  EXPECT_EQ(unmarked.at("summary").at("per_playbook").at("batch").at("marked_pnl"), nullptr);
}
TEST(BacktestApi, JointPostValidationNamesConflictingAndUnknownSelectors) {
  test::RecordingFile storage;
  server::BacktestHost host({storage.directory, {}, {}, {}, {}, true});
  const json base{{"plan", "eod-50k"}, {"scenarios", 1}, {"seed", "1"}};
  for (const auto& selectors : {json{{"playbook", "batch"}, {"playbooks", {"batch", "missing"}}},
      json{{"playbooks", {"batch", "batch@1"}}}, json{{"playbooks", {"batch", "missing"}}}, json::object()}) {
    auto body = base; body.update(selectors);
    const auto response = call(host, {"POST", "/api/backtests", body.dump()});
    EXPECT_EQ(response.status, 400) << response.body;
    EXPECT_NE(response.body.find(selectors.empty() || selectors.contains("playbook") ? "exactly one" :
        selectors.at("playbooks")[1] == "missing" ? "Unknown playbook: missing" : "Duplicate playbook ID: batch"), std::string::npos);
  }
}
}  // namespace

namespace {
TEST(BacktestApi, JointPostRunsPinnedDefinitionsAndListsEveryMember) {
  test::RecordingFile storage;
  const auto file = recorded_day(storage.directory, {2026, 9, 14});
  BacktestSource source;
  auto second = source.saved_catalogue["definitions"]["batch"]["versions"][0];
  second["id"] = "zeta";
  source.saved_catalogue["definitions"]["zeta"] = {{"versions", {second}}, {"deleted", false}};
  server::BacktestHost host({storage.directory / "runs", storage.directory, {}, {}, {}, true});
  const json body{{"playbooks", {"zeta", "batch"}}, {"plan", "eod-50k"}, {"days", {{{"file", file.filename().string()}}}}};
  server::ApiResponse response;
  ASSERT_TRUE(host.handle({"POST", "/api/backtests", body.dump()}, source, [&](server::ApiResponse value) { response = std::move(value); }));
  ASSERT_EQ(response.status, 202) << response.body;
  const auto id = json::parse(response.body).at("id").get<std::string>();
  // Editing the source publication after start cannot change the pinned job.
  source.saved_catalogue["definitions"].erase("zeta");
  json saved;
  ASSERT_TRUE(test::recording_eventually([&] {
    saved = json::parse(call(host, {"GET", "/api/backtests/" + id}, false).body);
    return saved.at("status") != "running";
  }));
  ASSERT_EQ(saved.at("status"), "completed") << saved.dump();
  EXPECT_EQ(saved.at("report").at("playbooks").size(), 2U);
  test::capture_contract("backtests", "GET", "/api/backtests/" + id, {200, saved.dump()});
  const auto listing = json::parse(call(host, {"GET", "/api/backtests?playbook=zeta"}).body);
  ASSERT_EQ(listing.at("runs").size(), 1U);
  EXPECT_EQ(listing.at("runs")[0].at("mode"), "joint");
}
}  // namespace

namespace {
std::filesystem::path joint_recorded_day(const std::filesystem::path& directory, md::Date date, bool loss = false) {
  const auto path = directory / (md::format_date(date) + ".oprec");
  std::vector<md::OptionContract> contracts;
  for (const auto* symbol : {"SPXW  260916P05000000", "SPXW  260916P04995000", "SPY   260916P00500000", "SPY   260916P00495000"}) {
    auto contract = *md::parse_osi(symbol);
    contract.expiry = date;
    contracts.push_back(contract);
  }
  md::RecordingHeader header;
  header.provider = "simulated joint test";
  header.subscription.underlyings = {"SPX", "SPY"};
  header.started = md::new_york_to_utc(date, 9, 30);
  header.capabilities.poll_interval = std::chrono::seconds(60);
  md::Timestamp time = header.started;
  md::RecordingSink::Options options;
  options.clock = [&] { return time; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, options);
  for (std::size_t index = 0; index < contracts.size(); ++index)
    sink.publish(md::ContractDefinition{static_cast<md::InstrumentId>(index), contracts[index]});
  for (int minute = 0; minute < 3; ++minute) {
    time = header.started + minute * md::kNanosPerMinute;
    for (const auto* symbol : {"SPX", "SPY"}) {
      const bool spy = std::string_view(symbol) == "SPY";
      const double spot = spy ? 500 : 5000, high = minute == 0 ? 10 : loss ? 13 : 9;
      const md::InstrumentId id = spy ? 2 : 0;
      sink.publish(md::UnderlyingQuote{symbol, time, spot, spot, spot});
      sink.publish(md::OptionQuote{id, time, high, high + .1, 20, 20});
      sink.publish(md::OptionQuote{id + 1, time, 8, 8.1, 20, 20});
      sink.publish(md::SnapshotComplete{symbol, time});
    }
  }
  sink.close();
  return path;
}
server::BacktestRequest joint_recorded_request(const json& days, const json& plan) {
  auto definitions = joint_catalogue();
  auto& alpha = definitions["definitions"]["alpha"]["versions"][0];
  alpha["structure"]["template"]["target"] = {{"mode", "strike"}, {"value", 5000}};
  auto& zeta = definitions["definitions"]["zeta"]["versions"][0];
  zeta["name"] = "Zeta SPY put";
  zeta["underlyings"] = {"SPY"};
  zeta["structure"]["template"]["type"] = "put";
  zeta["structure"]["template"]["target"] = {{"mode", "strike"}, {"value", 500}};
  auto request = server::parse_backtest({{"playbooks", {"zeta", "alpha"}}, {"plan", plan}, {"days", days}}, definitions, {}, {}, false);
  request.analytics.fallback_rate = 0;
  return request;
}
server::BacktestRequest alone(server::BacktestRequest request, const std::string& id) {
  const auto definitions = request.playbooks.at("definitions");
  request.playbook = id;
  request.playbooks["definitions"] = {{id, definitions.at(id)}};
  request.playbooks["modes"]["main"] = {{id, "auto"}};
  return request;
}
TEST(Backtest, JointBuyingPowerBlocksSecondPlaybookThatCouldTradeAlone) {
  test::RecordingFile storage;
  const auto file = joint_recorded_day(storage.directory, {2026, 9, 14});
  const auto request = joint_recorded_request(json::array({{{"file", file.string()}}}),
      {{"initial_cash", "500"}, {"rules", {{"profit_target", "1000"}, {"max_drawdown", "0"}, {"buying_power", true}}}});
  const std::atomic_bool cancel{false};
  const auto joint = server::run_backtest(request, storage.directory / "joint", cancel);
  ASSERT_EQ(joint.at("status"), "completed") << joint.dump();
  for (const auto* collection : {"days", "attempts"}) {
    const auto& result = joint.at(collection)[0];
    EXPECT_EQ(result.at("per_playbook").at("alpha").at("trades"), 1);
    EXPECT_EQ(result.at("per_playbook").at("zeta").at("trades"), 0);
    EXPECT_NE(result.at("entry_reasons").at("zeta:SPY").get<std::string>().find("buying power"), std::string::npos) << result.dump();
  }
  for (const auto* id : {"alpha", "zeta"}) {
    const auto single = server::run_backtest(alone(request, id), storage.directory / id, cancel);
    ASSERT_EQ(single.at("status"), "completed") << single.dump();
    EXPECT_EQ(single.at("summary").at("trades"), 1);
  }
}
TEST(Backtest, JointTargetPoolsTwoPlaybooksAndEightWorkersKeepIdenticalJournals) {
  test::RecordingFile storage;
  json days = json::array();
  for (const int day : {14, 15, 16, 17, 18, 21, 22, 23})
    days.push_back({{"file", joint_recorded_day(storage.directory, {2026, 9, day}).string()}});
  auto request = joint_recorded_request(days,
      {{"initial_cash", "50000"}, {"rules", {{"profit_target", "100"}, {"profit_basis", "balance"}, {"max_drawdown", "0"}}}});
  const std::atomic_bool cancel{false};
  request.workers = 1;
  const auto first = server::run_backtest(request, storage.directory / "one", cancel);
  ASSERT_EQ(first.at("status"), "completed") << first.dump();
  EXPECT_EQ(first.at("summary").at("trades"), 16);
  EXPECT_EQ(first.at("summary").at("passed"), 8);
  for (const auto& day : first.at("days")) {
    const auto& rows = day.at("per_playbook");
    EXPECT_EQ(rows.at("alpha").at("trades"), 1);
    EXPECT_EQ(rows.at("zeta").at("trades"), 1);
    EXPECT_EQ((Money::parse(rows.at("alpha").at("marked_pnl").get<std::string>()) +
        Money::parse(rows.at("zeta").at("marked_pnl").get<std::string>())).str(), day.at("pnl").get<std::string>());
  }
  request.workers = 8;
  const auto second = server::run_backtest(request, storage.directory / "eight", cancel);
  EXPECT_EQ(first, second);
  EXPECT_EQ(bytes(storage.directory / "one/report.json"), bytes(storage.directory / "eight/report.json"));
  for (const auto& journal : first.at("journals")) {
    const auto path = journal.get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "one" / path), bytes(storage.directory / "eight" / path));
    const auto verified = server::verify_run(storage.directory / "one" / path);
    EXPECT_TRUE(verified.matched) << path << ": " << verified.message;
  }
  request.days.resize(1);
  for (const auto* id : {"alpha", "zeta"}) {
    const auto single = server::run_backtest(alone(request, id), storage.directory / id, cancel);
    ASSERT_EQ(single.at("status"), "completed") << single.dump();
    EXPECT_EQ(single.at("summary").at("trades"), 1);
    EXPECT_EQ(single.at("attempts")[0].at("outcome"), "open");
  }
}
TEST(Backtest, JointDailyLossFailsTheAccountWhenNeitherPlaybookAloneWouldFail) {
  test::RecordingFile storage;
  const auto file = joint_recorded_day(storage.directory, {2026, 9, 14}, true);
  const auto request = joint_recorded_request(json::array({{{"file", file.string()}}}),
      {{"initial_cash", "50000"}, {"rules", {{"profit_target", "10000"}, {"max_drawdown", "0"},
      {"daily_loss_limit", "500"}, {"daily_loss_action", "fail"}}}});
  const std::atomic_bool cancel{false};
  const auto joint = server::run_backtest(request, storage.directory / "joint", cancel);
  ASSERT_EQ(joint.at("status"), "completed") << joint.dump();
  EXPECT_EQ(joint.at("attempts")[0].at("outcome"), "failed");
  EXPECT_EQ(joint.at("attempts")[0].at("day_rows")[0].at("decision_code"), "DAILY_LOSS_LIMIT");
  EXPECT_NE(joint.at("attempts")[0].at("rule_trips").dump().find("evaluation_failed"), std::string::npos);
  for (const auto* id : {"alpha", "zeta"}) {
    const auto single = server::run_backtest(alone(request, id), storage.directory / id, cancel);
    ASSERT_EQ(single.at("status"), "completed") << single.dump();
    EXPECT_EQ(single.at("attempts")[0].at("outcome"), "open");
  }
}
TEST(Backtest, CliJointSelectorsRunDeterministicallyAndRejectInvalidLists) {
  test::RecordingFile storage;
  const auto file = joint_recorded_day(storage.directory, {2026, 9, 14});
  const json plan{{"initial_cash", "50000"}, {"rules", {{"profit_target", "1000"}, {"max_drawdown", "0"}}}};
  const auto request = joint_recorded_request(json::array({{{"file", file.string()}}}), plan);
  std::ofstream(storage.directory / "playbooks.json") << request.playbooks.dump();
  std::ofstream(storage.directory / "plan.json") << plan.dump();
  std::ofstream(storage.directory / "days.json") << json::array({{{"file", file.filename().string()}}}).dump();
  const auto run = [&](const std::string& selection, unsigned workers, const std::string& name) {
    const auto root = storage.directory.string();
    const auto command = "HOME='" + root + "' TMPDIR='" + root + "' \"" + OPENPORT_APPS_DIR +
        "/openportd\" --backtest '" + selection + "' --playbooks '" + root + "/playbooks.json' --plan '" + root +
        "/plan.json' --days '" + root + "/days.json' --workers " + std::to_string(workers) + " --out '" + root + "/" + name + ".json' 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("Cannot launch backtest CLI");
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
    const auto status = pclose(pipe);
    EXPECT_TRUE(WIFEXITED(status)) << output;
    return std::pair{WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
  };
  const auto first = run("zeta@1,alpha", 1, "one"), second = run("alpha@1,zeta", 8, "eight");
  ASSERT_EQ(first.first, 0) << first.second;
  ASSERT_EQ(second.first, 0) << second.second;
  EXPECT_EQ(bytes(storage.directory / "one.json"), bytes(storage.directory / "eight.json"));
  const auto report = json::parse(bytes(storage.directory / "one.json"));
  EXPECT_EQ(report.at("mode"), "joint");
  EXPECT_EQ(report.at("summary").at("trades"), 2);
  for (const auto& journal : report.at("journals")) {
    const auto path = journal.get<std::string>();
    EXPECT_EQ(bytes(storage.directory / "one.json.d" / path), bytes(storage.directory / "eight.json.d" / path));
  }
  const auto single = run("alpha@1", 1, "single");
  ASSERT_EQ(single.first, 0) << single.second;
  EXPECT_EQ(json::parse(bytes(storage.directory / "single.json")).at("schema"), 2);
  for (const auto* selection : {"alpha,alpha@1", "alpha,missing", "alpha,", ",zeta", "alpha@2,zeta"}) {
    const auto failed = run(selection, 1, "invalid");
    EXPECT_EQ(failed.first, 2) << failed.second;
    EXPECT_FALSE(std::filesystem::exists(storage.directory / "invalid.json.d"));
  }
}
}  // namespace

TEST(Backtest, VolumeRuleAllowsOpeningFillsForGeneratedAndExplicitScenarioDays) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile storage;
  const auto scenario = volume_scenario(storage.directory);
  auto definitions = catalogue();
  definitions["definitions"]["batch"]["versions"][0]["structure"]["template"]["target"]["value"] = 6000;
  const std::atomic_bool cancel{false};
  json previous;
  for (const bool explicit_days : {false, true}) {
    json body{{"playbook", "batch"},
        {"plan", {{"initial_cash", "10000"}, {"rules", {{"profit_target", "100"}, {"max_volume_percent", 10}}}}}};
    if (explicit_days) body["days"] = {{{"scenario", scenario.id}, {"date", md::format_date(scenario.date)}, {"seed", "5"}}};
    else body.update({{"scenarios", 1}, {"seed", "5"}});
    const auto request = server::parse_backtest(body, definitions, {scenario}, {}, false);
    EXPECT_EQ(request.config.rules.max_volume_percent, 10);
    const auto directory = storage.directory / (explicit_days ? "explicit" : "generated");
    const auto report = server::run_backtest(request, directory, cancel);
    ASSERT_EQ(report.at("status"), "completed") << report.dump();
    for (const auto* section : {"days", "attempts"}) {
      const auto& result = report.at(section).at(0);
      ASSERT_GE(result.at("fills").size(), 2U) << result.dump();
      for (std::size_t leg = 0; leg < 2; ++leg) {
        EXPECT_EQ(result.at("fills")[leg].at("quantity"), 1);
        EXPECT_EQ(result.at("fills")[leg].at("time"), md::format_timestamp(providers::scenario_open(scenario, scenario.date)));
      }
      const auto verified = server::verify_run(directory / result.at("journal").get<std::string>());
      EXPECT_TRUE(verified.matched) << verified.message;
    }
    if (explicit_days) { EXPECT_EQ(report, previous); }
    previous = report;
  }
}

TEST(Backtest, VolumeRuleRefusesOversizedScenarioEntries) {
  test::RecordingFile storage;
  const auto scenario = volume_scenario(storage.directory);
  auto definitions = catalogue();
  auto& definition = definitions["definitions"]["batch"]["versions"][0];
  definition["structure"]["template"]["target"]["value"] = 6000;
  definition["sizing"]["units"] = 100;
  auto request = server::parse_backtest({{"playbook", "batch"},
      {"plan", {{"initial_cash", "1000000"}, {"rules", {{"profit_target", "100000"}, {"max_volume_percent", 10}}}}},
      {"scenarios", 1}, {"seed", "5"}}, definitions, {scenario}, {}, false);
  // Keep ordinary sizing limits from trimming the oversized entry before the volume gate.
  request.config.limits.max_order_contracts = 1000;
  request.config.limits.aggregate = {1e9, 1e9};
  request.config.limits.per_underlying = {1e9, 1e9};
  const std::atomic_bool cancel{false};
  const auto report = server::run_backtest(request, storage.directory / "oversized", cancel);
  ASSERT_EQ(report.at("status"), "completed") << report.dump();
  for (const auto* section : {"days", "attempts"}) {
    const auto& result = report.at(section).at(0);
    EXPECT_TRUE(result.at("fills").empty()) << result.dump();
    // Entry reasons retain the MAX_VOLUME_SHARE message, not the structured code.
    EXPECT_NE(result.at("entry_reasons").at("batch:SPX").get<std::string>().find("exceeds 10% of current-date volume"),
        std::string::npos) << result.dump();
  }
}
