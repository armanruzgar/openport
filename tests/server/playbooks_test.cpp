#include <fstream>
#include "openport/analytics/technical.hpp"
#include <sstream>
#include <gtest/gtest.h>
#include "openport/server/playbooks.hpp"
#include "openport/server/plans.hpp"
#include "support/contract_capture.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/api.hpp"
#include "openport/server/run.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/providers/replay_batches.hpp"
#include "openport/trading/history.hpp"
#include "server/strategy_template.hpp"
#include "server/run_json.hpp"
#include "server/paper_json.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
Money money(std::string_view value) { return Money::parse(value); }
json definition() {
  return {{"id", "morning"}, {"name", "Morning spread"}, {"description", "Synthetic test setup"}, {"underlyings", {"SPX"}},
      {"window", {{"start", "09:30"}, {"end", "11:00"}, {"weekdays", {1, 2, 3, 4, 5}}}}, {"conditions", json::object()},
      {"structure", {{"template", {{"kind", "vertical"}, {"type", "put"}, {"direction", "credit"}, {"target", {{"mode", "delta"}, {"value", 15}}}, {"width", 5}}}, {"expiry", {{"min", 0}, {"max", 60}}}}},
      {"sizing", {{"units", 1}}}, {"management", {{"close_by", "11:30"}, {"take_profit_percent", 50}, {"stop_credit_multiple", 2}}},
      {"guardrails", {{"max_entries_per_day", 1}, {"cooldown_minutes", 30}}}};
}
server::TradingReply command(server::Desk& desk, const json& change, md::Timestamp time) {
  server::TradingCommand request;
  request.kind = server::TradingCommand::Kind::Playbook;
  request.note = change.dump();
  server::TradingReply result;
  desk.command(request, [&](server::TradingReply reply) { result = std::move(reply); }, time, time);
  return result;
}
TEST(Playbooks, TechnicalMathMatchesHandComputedCloses) {
  const std::vector<double> closes{1, 2, 3, 2, 4};
  EXPECT_DOUBLE_EQ(analytics::sma(closes, 3), 3);
  EXPECT_DOUBLE_EQ(analytics::ema(closes, 3), 3);
  EXPECT_NEAR(analytics::rsi(closes, 2), 100 * 1.25 / 1.5, 1e-12);
  EXPECT_NEAR(analytics::bollinger(closes, 3, 2), 3 + 2 * std::sqrt(2.0 / 3), 1e-12);
  const std::vector<double> flat{3, 3, 3}, up{1, 2, 3}, down{3, 2, 1};
  EXPECT_DOUBLE_EQ(analytics::rsi(flat, 2), 50);
  EXPECT_DOUBLE_EQ(analytics::rsi(up, 2), 100);
  EXPECT_DOUBLE_EQ(analytics::rsi(down, 2), 0);
  EXPECT_TRUE(std::isnan(analytics::rsi(flat, 3)));
  EXPECT_TRUE(std::isnan(analytics::sma(flat, 0)));
  EXPECT_TRUE(std::isnan(analytics::ema(std::vector<double>{1, analytics::kNaN}, 2)));
}
TEST(Playbooks, BreadthConditionsHaveSpecificFailuresAndStrictValidation) {
  auto setup = definition();
  server::PlaybookInputs inputs;
  inputs.spot = 105; inputs.prior_close = 100; inputs.day_open = 102; inputs.vix = 20;
  inputs.daily_closes = {99, 100, 101};
  setup["conditions"] = {{"vix", {{"min", 20}, {"max", 20}}}, {"gap", {{"min_percent", 1}, {"max_percent", 3}}},
      {"technical", {{{"indicator", "sma"}, {"interval", "day"}, {"period", 3}, {"direction", "above"}}}}};
  EXPECT_NO_THROW(server::validate_playbook(setup));
  EXPECT_FALSE(server::playbook_condition_reason(setup, inputs, 1));
  inputs.vix = analytics::kNaN;
  EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 1), "VIX unavailable (no current VIX spot)");
  inputs.vix = 21;
  EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 1), "VIX 21.00 is outside 20.00-20.00");
  inputs.vix = 20; inputs.day_open = analytics::kNaN;
  EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 1), "Gap unavailable (no 09:30 minute)");
  inputs.day_open = 99;
  EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 1), "Gap percent -1.00 is outside 1.00-3.00");
  inputs.day_open = 102; inputs.daily_closes = {99, 100};
  EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 1), "SMA(3, day) unavailable: 2 of 3 daily bars");
  inputs.daily_closes.push_back(101); inputs.spot = 100;
  EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 1), "Price 100.00 is not above SMA(3, day) 100.00");
  for (const auto& [path, value] : std::vector<std::pair<std::string, json>>{
      {"/conditions/vix/min", -1}, {"/conditions/gap/max_percent", 0}, {"/conditions/technical/0/period", 2.5},
      {"/conditions/technical/0/interval", "hour"}, {"/conditions/technical/0/k", 2},
      {"/management/trailing_stop", {{"percent", 0}}}, {"/management/trailing_stop", {{"percent", 20}, {"typo", 1}}},
      {"/management/close_at_dte", -1}, {"/management/max_days_in_trade", 0}, {"/management/stop_loss_percent", 20}}) {
    auto bad = setup; bad[json::json_pointer(path)] = value;
    EXPECT_THROW(server::validate_playbook(bad), std::invalid_argument) << path;
  }
  setup["management"] = {{"close_by", "15:45"}, {"max_hold_days", 1}, {"max_days_in_trade", 1}};
  EXPECT_THROW(server::validate_playbook(setup), std::invalid_argument);
  setup["management"] = {{"close_by", "15:45"}, {"stop_loss_percent", 25}};
  EXPECT_NO_THROW(server::validate_playbook(setup));
  const auto stop = server::playbook_bracket(setup["management"], money("2")).stop_loss;
  ASSERT_TRUE(stop && stop->trigger);
  EXPECT_EQ(stop->trigger->level, money("-1.50"));
  EXPECT_EQ(stop->trigger->direction, trading::TriggerDirection::AtOrAbove);
  EXPECT_THROW(server::playbook_bracket(setup["management"], money("-2")), std::invalid_argument);
  setup["management"] = {{"close_by", "15:45"}, {"max_days_in_trade", 1}};
  EXPECT_EQ(server::playbook_deadline(setup, md::new_york_to_utc({2026, 9, 4}, 10, 0)), md::new_york_to_utc({2026, 9, 8}, 15, 45));
}

// Explicit two-leg snapshots exercise the same Desk callbacks in live and replay.
std::vector<md::Event> breadth_batch(md::Timestamp time, double high, double spot = 5000) {
  auto upper = *md::parse_osi("SPXW260924P05000000");
  auto lower = upper; lower.strike = 4995;
  return {md::ContractDefinition{0, upper}, md::ContractDefinition{1, lower},
      md::UnderlyingQuote{"SPX", time, spot, spot, spot},
      md::OptionQuote{0, time, high, high + .1, 20, 20}, md::OptionQuote{1, time, 8, 8.1, 20, 20},
      md::SnapshotComplete{"SPX", time}};
}
json breadth_setup() {
  auto setup = definition();
  setup["structure"]["template"]["target"] = {{"mode", "strike"}, {"value", 5000}};
  setup["management"] = {{"close_by", "15:45"}, {"take_profit_percent", 1}, {"trailing_stop", {{"percent", 50}}}};
  return setup;
}
TEST(Playbooks, StageCarriesReachedStopPreviewWarnings) {
  for (const bool replay : {false, true}) {
    const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
    server::Desk::Options options; options.replay = replay;
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    // Entry credit 0.10; closing natural 0.30 already exceeds the 0.20 stop.
    desk.replay_batch(breadth_batch(time, 8.2), time, time);
    auto setup = breadth_setup();
    setup["management"] = {{"close_by", "15:45"}, {"stop_credit_multiple", 2}};
    ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, time).decision.ok());
    const auto staged = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, time);
    const auto stages = json::parse(staged.playbook_result).at("staged");
    ASSERT_EQ(stages.size(), 1U) << staged.playbook_result;
    EXPECT_EQ(stages[0].at("net"), "-0.10");
    const auto& warnings = stages[0].at("warnings");
    ASSERT_FALSE(warnings.empty());
    const auto found = std::find_if(warnings.begin(), warnings.end(), [](const auto& warning) {
      return warning.at("code") == "STOP_REACHED";
    });
    ASSERT_NE(found, warnings.end()) << warnings;
    EXPECT_NE(found->at("message").get<std::string>().find("fires as soon as the entry fills"), std::string::npos);
    EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
    desk.replay_batch(breadth_batch(time + md::kNanosPerMinute, 10), time + md::kNanosPerMinute, time + md::kNanosPerMinute);
    const auto refreshed = json::parse(desk.trading_view()->playbooks_json).at("staged");
    ASSERT_EQ(refreshed.size(), 1U);
    EXPECT_TRUE(refreshed[0].at("warnings").empty()) << refreshed;
  }
}
TEST(Playbooks, TrailingExitUsesNaturalProfitAndRecovers) {
  for (const bool replay : {false, true}) {
    test::RecordingFile file;
    const auto opened = md::new_york_to_utc({2026, 9, 22}, 10, 0);
    server::Desk::Options options; options.replay = replay;
    options.paper_journal = file.directory / "trail.jsonl";
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    {
      server::Desk desk("test", {}, {{"SPX"}}, options);
      desk.start_trading(); desk.replay_batch(breadth_batch(opened, 10), opened, opened);
      ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", breadth_setup()}}, opened).decision.ok());
      ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, opened).decision.ok());
      ASSERT_EQ(desk.trading_view()->snapshot->positions.size(), 2U) << desk.trading_view()->playbooks_json;
      desk.replay_batch(breadth_batch(opened + md::kNanosPerMinute, 9), opened + md::kNanosPerMinute, opened + md::kNanosPerMinute);
      const auto& review = desk.trading_view()->snapshot->strategy_reviews.at("1");
      ASSERT_TRUE(review.trailing);
      EXPECT_EQ(review.trailing->peak, money("80"));
      EXPECT_EQ(review.trailing->triggered, 0);
      desk.stop();
    }
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    const auto fire = opened + 2 * md::kNanosPerMinute;
    desk.replay_batch(breadth_batch(fire, 9.5), fire, fire);
    const auto view = desk.trading_view();
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->open_orders.empty());
    EXPECT_EQ(view->snapshot->recent_orders.back().request.note, "Playbook automatic trailing stop; entry 1");
    EXPECT_EQ(view->snapshot->recent_orders[1].reason.code, trading::Reason::PLAYBOOK_TRAILING_STOP);
    const auto& review = view->snapshot->strategy_reviews.at("1");
    ASSERT_TRUE(review.trailing);
    EXPECT_EQ(review.trailing->triggered, fire);
    const auto report = server::playbook_report(json::parse(view->playbooks_json), *view).at("morning");
    EXPECT_EQ(report.at("trades")[0].at("rules").at("trailing_stop"), true);
    desk.stop();
    const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(options.paper_journal));
    EXPECT_EQ(recovered.snapshot()->strategy_reviews.at("1").trailing->peak, money("80"));
  }
}
TEST(Playbooks, StageTrailingRuleLatchesAndAutoWaitsWithoutCancellingProtection) {
  const auto opened = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading(); desk.replay_batch(breadth_batch(opened, 10), opened, opened);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", breadth_setup()}}, opened).decision.ok());
  auto staged = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, opened);
  const auto stages = json::parse(staged.playbook_result).at("staged");
  ASSERT_EQ(stages.size(), 1U);
  EXPECT_EQ(stages[0].at("management").at("trailing_stop").at("percent"), 50);
  ASSERT_TRUE(command(desk, {{"action", "send"}, {"staged", stages[0].at("id")}}, opened).decision.ok());
  const auto peak = opened + md::kNanosPerMinute, fire = peak + md::kNanosPerMinute;
  desk.replay_batch(breadth_batch(peak, 9), peak, peak);
  desk.replay_batch(breadth_batch(fire, 9.5), fire, fire);
  EXPECT_EQ(desk.trading_view()->snapshot->positions.size(), 2U);
  const auto bad = fire + md::kNanosPerMinute;
  auto unusable = breadth_batch(bad, 9.5);
  // Closing the long wing needs a positive bid; don't cancel its bracket yet.
  std::get<md::OptionQuote>(unusable[4]).bid = 0;
  desk.replay_batch(unusable, bad, bad);
  ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, bad).decision.ok());
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 2U);
  EXPECT_EQ(desk.trading_view()->snapshot->open_orders.size(), 1U);
  const auto usable = bad + md::kNanosPerMinute;
  desk.replay_batch(breadth_batch(usable, 9), usable, usable);
  const auto view = desk.trading_view();
  EXPECT_TRUE(view->snapshot->positions.empty());
  EXPECT_EQ(view->snapshot->recent_orders.back().request.note, "Playbook automatic trailing stop; entry 1");
  EXPECT_EQ(server::playbook_report(json::parse(view->playbooks_json), *view).at("morning").at("trades")[0].at("rules").at("trailing_stop"), false);
}
TEST(Playbooks, TechnicalInputsExcludeCurrentAndFutureBars) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  auto setup = breadth_setup();
  setup["conditions"] = {{"technical", {{{"indicator", "sma"}, {"interval", "minute"}, {"period", 2}, {"direction", "above"}},
      {{"indicator", "ema"}, {"interval", "day"}, {"period", 2}, {"direction", "above"}}}}};
  server::Desk::Options options; options.replay = true;
  options.candles = std::make_shared<server::CandleStore>();
  for (const auto t : {time - 2 * md::kNanosPerMinute, time - md::kNanosPerMinute}) options.candles->sample("SPX", t, 4900);
  options.candles->sample("SPX", time + md::kNanosPerMinute, 10000);
  for (const int day : {18, 21, 22, 23}) {
    const auto t = md::new_york_to_utc({2026, 9, day}, 9, 30);
    const double price = day < 22 ? 4900 : 10000;
    options.candles->merge_days("SPX", {{t, price, price, price, price}});
  }
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading(); desk.replay_batch(breadth_batch(time, 10), time, time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, time).decision.ok());
  const auto result = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, time);
  EXPECT_EQ(json::parse(result.playbook_result).at("staged").size(), 1U) << result.playbook_result;
}
TEST(Playbooks, VixConditionUsesObservedIndexAndRejectsStaleOrFuturePrints) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (const int age : {-1, 0, 6}) {
    auto setup = breadth_setup(); setup["conditions"] = {{"vix", {{"min", 20}, {"max", 20}}}};
    server::Desk::Options options; options.replay = true;
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    server::Desk desk("test", {}, {{"SPX", "VIX"}}, options);
    desk.start_trading();
    auto batch = breadth_batch(time, 10);
    batch.insert(batch.begin(), md::UnderlyingQuote{"VIX", time - age * md::kNanosPerMinute, 20, 20, 20});
    desk.replay_batch(batch, time, time);
    ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, time).decision.ok());
    const auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, time);
    const auto publication = json::parse(reply.playbook_result);
    EXPECT_EQ(publication.at("staged").size(), age == 0 ? 1U : 0U) << publication.dump();
    if (age != 0) { EXPECT_EQ(publication.at("reasons").at("morning:SPX"), "VIX unavailable (no current VIX spot)"); }
  }
}
TEST(Playbooks, DebitStopIsSubmittedAsAFixedComboBracket) {
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  auto setup = breadth_setup();
  setup["structure"]["template"]["direction"] = "debit";
  setup["structure"]["template"]["target"]["value"] = 4995;
  setup["management"] = {{"close_by", "15:45"}, {"stop_loss_percent", 25}};
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading(); desk.replay_batch(breadth_batch(time, 10), time, time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, time).decision.ok());
  ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, time).decision.ok());
  const auto snapshot = desk.trading_view()->snapshot;
  ASSERT_EQ(snapshot->positions.size(), 2U) << desk.trading_view()->playbooks_json;
  ASSERT_TRUE(snapshot->recent_orders[0].request.bracket);
  EXPECT_EQ(snapshot->recent_orders[0].request.bracket->stop_loss->trigger->level, money("-1.58"));
  const auto next = time + md::kNanosPerMinute;
  desk.replay_batch(breadth_batch(next, 9.5), next, next);
  EXPECT_TRUE(desk.trading_view()->snapshot->positions.empty());
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders[1].status, trading::OrderStatus::Filled);
}
TEST(Playbooks, DteAndBusinessDayExitsCloseAtTheirExactUpdate) {
  for (const bool dte : {false, true}) {
    const auto opened = md::new_york_to_utc(dte ? md::Date{2026, 9, 22} : md::Date{2026, 9, 4}, 10, 0);
    const auto deadline = md::new_york_to_utc(dte ? md::Date{2026, 9, 22} : md::Date{2026, 9, 8}, dte ? 16 : 15, dte ? 0 : 45);
    auto setup = breadth_setup();
    setup["management"].erase("trailing_stop");
    if (dte) { setup["management"]["close_at_dte"] = 2; setup["management"]["max_hold_days"] = 2; }
    else setup["management"]["max_days_in_trade"] = 1;
    server::Desk::Options options; options.replay = true;
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading(); desk.replay_batch(breadth_batch(opened, 10), opened, opened);
    ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, opened).decision.ok());
    ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, opened).decision.ok());
    ASSERT_EQ(desk.trading_view()->snapshot->positions.size(), 2U) << desk.trading_view()->playbooks_json;
    const auto before = deadline - md::kNanosPerSecond;
    desk.replay_batch(breadth_batch(before, 10), before, before);
    EXPECT_EQ(desk.trading_view()->snapshot->positions.size(), 2U);
    desk.replay_batch(breadth_batch(deadline, 10), deadline, deadline);
    const auto view = desk.trading_view();
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_EQ(view->snapshot->recent_orders.back().request.note,
        dte ? "Playbook automatic DTE stop; entry 1" : "Playbook automatic days in trade stop; entry 1");
    EXPECT_EQ(view->snapshot->recent_orders[1].reason.code,
        dte ? trading::Reason::PLAYBOOK_DTE_STOP : trading::Reason::PLAYBOOK_DAYS_IN_TRADE_STOP);
    EXPECT_EQ(server::playbook_report(json::parse(view->playbooks_json), *view).at("morning").at("trades")[0].at("rules").at(dte ? "close_at_dte" : "max_days_in_trade"), true);
  }
}

TEST(Playbooks, DefinitionValidationRejectsUnknownAndInconsistentFields) {
  EXPECT_NO_THROW(server::validate_playbook(definition()));
  for (const auto& [path, value] : std::vector<std::pair<std::string, json>>{
      {"/surprise", 1}, {"/id", "../../x"}, {"/window/start", "25:00"}, {"/window/end", "09:00"},
      {"/window/weekdays", {1, 1}}, {"/sizing/units", 1.5}, {"/sizing/floor_share", .5},
      {"/management/close_by", "09:00"}, {"/conditions/term_inverted", 1}, {"/structure/template/width", 0},
      {"/underlyings", {"SPX", "SPX"}}, {"/guardrails/max_entries_per_day", 0}}) {
    auto bad = definition(); bad[json::json_pointer(path)] = value;
    EXPECT_THROW(server::validate_playbook(bad), std::exception) << path;
  }
  // B62: a missing field is named, not reported as the JSON library's lookup error.
  for (const auto& [path, message] : std::vector<std::pair<std::string, std::string>>{
      {"/management/close_by", "management.close_by is required"}, {"/guardrails", "guardrails is required"},
      {"/window/weekdays", "window.weekdays is required"}, {"/structure/template/kind", "template kind is required"},
      {"/structure/template/target/value", "target value is required"}, {"/structure/expiry/max", "expiry DTE max is required"},
      {"/guardrails/cooldown_minutes", "guardrails.cooldown_minutes is required"}, {"/name", "name is required"}}) {
    auto bad = definition();
    const json::json_pointer pointer(path);
    bad.at(pointer.parent_pointer()).erase(pointer.back());
    try { server::validate_playbook(bad); ADD_FAILURE() << path; }
    catch (const std::invalid_argument& error) { EXPECT_EQ(error.what(), message) << path; }
  }
}
TEST(Playbooks, EditsVersionAndArchiveRetainsHistoryOnDisk) {
  test::RecordingFile file;
  const auto path = file.directory / "playbooks.json";
  server::Playbooks store(path);
  store.change({{"action", "create"}, {"definition", definition()}}, "main", false);
  auto edited = definition(); edited["version"] = 1; edited["name"] = "Changed";
  store.change({{"action", "update"}, {"definition", edited}}, "main", false);
  EXPECT_THROW(store.change({{"action", "update"}, {"definition", edited}}, "main", false), std::invalid_argument);
  store.change({{"action", "delete"}, {"id", "morning"}, {"version", 2}}, "main", false);
  server::Playbooks recovered(path);
  const auto record = recovered.catalogue().at("definitions").at("morning");
  EXPECT_TRUE(record.at("deleted").get<bool>());
  ASSERT_EQ(record.at("versions").size(), 2U);
  EXPECT_EQ(record.at("versions")[0].at("name"), "Morning spread");
  EXPECT_EQ(record.at("versions")[1].at("version"), 2);
  EXPECT_THROW(recovered.change({{"action", "create"}, {"definition", definition()}}, "main", false), std::invalid_argument);
}
TEST(Playbooks, LiveAutoBindingsAreAccountSpecific) {
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", definition()}}, "main", false);
  const json automatic{{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}};
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  EXPECT_NO_THROW(store.change(automatic, "main", false, now));
  EXPECT_NO_THROW(store.change(automatic, "practice", false, now));
  EXPECT_NO_THROW(store.change(automatic, "main", true));
  EXPECT_EQ(store.publication("main", true).at("modes").at("morning"), "auto");
  EXPECT_EQ(store.publication("practice", false).at("modes").at("morning"), "auto");
  EXPECT_TRUE(store.publication("other", false).at("modes").empty());
}
TEST(Playbooks, StartupFinishesRemovingBindingsOfADeletedAccount) {
  test::RecordingFile file;
  const auto path = file.directory / "playbooks.json";
  {
    server::Playbooks store(path);
    store.change({{"action", "create"}, {"definition", definition()}}, "main", false);
    const json mode{{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}};
    store.change(mode, "deleted", false);
    store.change(mode, "main", false);
  }
  server::Desk::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.paper_accounts = file.directory / "accounts";
  std::filesystem::create_directories(options.paper_accounts / "deleted/deleted");
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  server::Playbooks recovered(path);
  EXPECT_FALSE(recovered.enabled("deleted"));
  EXPECT_TRUE(recovered.enabled("main"));
  EXPECT_EQ(recovered.catalogue().at("definitions").size(), 1U);
  desk.stop();
}

TEST(Playbooks, ConditionsFailClosedAndWindowsUseNewYorkWeekdays) {
  auto setup = definition();
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  EXPECT_TRUE(server::playbook_window(setup, time));
  EXPECT_FALSE(server::playbook_window(setup, md::new_york_to_utc({2026, 9, 22}, 11, 0)));
  EXPECT_FALSE(server::playbook_window(setup, md::new_york_to_utc({2026, 9, 26}, 10, 0)));
  setup["conditions"] = {{"price", {{"reference", "prior_close"}, {"direction", "above"}, {"value", 2}}},
      {"iv_rank", {{"min", .3}, {"max", .7}}}, {"vrp_min", 2}, {"term_inverted", true}, {"dte", {{"min", 0}, {"max", 5}}}};
  server::PlaybookInputs inputs{105, 100, 101, .5, 3, 1.2};
  EXPECT_TRUE(server::playbook_conditions(setup, inputs, 1));
  EXPECT_FALSE(server::playbook_conditions(setup, {}, 1));
  inputs.prior_close = 104; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 1));
  inputs.prior_close = 100; inputs.term_ratio = 1; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 1));
  inputs.term_ratio = 1.2; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 6));
  setup["conditions"]["price"]["reference"] = "day_open";
  inputs.day_open = 106; EXPECT_FALSE(server::playbook_conditions(setup, inputs, 1));
  setup["conditions"]["price"] = {{"reference", "level"}, {"direction", "below"}, {"value", 110}};
  EXPECT_TRUE(server::playbook_conditions(setup, inputs, 1));
}
TEST(Playbooks, ConditionReasonsNameTheFirstFailureAndItsInputs) {
  auto setup = definition();
  const server::PlaybookInputs inputs{5912.3, 5920, 5920, .12, 1.2, .94};
  for (const auto& [conditions, expected] : std::vector<std::pair<json, std::string>>{
      {{{"dte", {{"min", 0}, {"max", 1}}}, {"iv_rank", {{"min", .3}, {"max", 1}}}}, "DTE 3.2 is outside 0.0-1.0"},
      {{{"iv_rank", {{"min", .3}, {"max", 1}}}}, "IV rank 0.12 is outside 0.30-1.00"},
      {{{"vrp_min", 2}}, "VRP 1.2 is not above 2.0"},
      {{{"term_inverted", true}}, "Term structure is not inverted (9d/30d 0.94)"},
      {{{"price", {{"reference", "day_open"}, {"direction", "above"}, {"value", 0}}}}, "Price 5912.30 is not above day open 5920.00 + 0.00"},
      {{{"price", {{"reference", "prior_close"}, {"direction", "above"}, {"value", -2}}}}, "Price 5912.30 is not above prior close 5920.00 - 2.00"},
      {{{"price", {{"reference", "level"}, {"direction", "below"}, {"value", 5900}}}}, "Price 5912.30 is not below 5900.00"}}) {
    setup["conditions"] = conditions;
    EXPECT_EQ(server::playbook_condition_reason(setup, inputs, 3.2), expected);
  }
  for (const auto& [conditions, expected] : std::vector<std::pair<json, std::string>>{
      {{{"dte", {{"min", 0}, {"max", 1}}}}, "DTE unavailable (no expiry)"},
      {{{"iv_rank", {{"min", .3}, {"max", 1}}}}, "IV rank unavailable (no IV history)"},
      {{{"vrp_min", 2}}, "VRP unavailable (missing IV or realized volatility)"},
      {{{"term_inverted", true}}, "Term structure unavailable (missing 9d or 30d IV)"},
      {{{"price", {{"reference", "day_open"}, {"direction", "above"}, {"value", 0}}}}, "Day open unavailable (no 09:30 minute)"},
      {{{"price", {{"reference", "prior_close"}, {"direction", "above"}, {"value", 0}}}}, "Prior close unavailable"},
      {{{"price", {{"reference", "level"}, {"direction", "above"}, {"value", 0}}}}, "Price unavailable"}}) {
    setup["conditions"] = conditions;
    EXPECT_EQ(server::playbook_condition_reason(setup, {}, analytics::kNaN), expected);
  }
  setup["conditions"] = {{"iv_rank", {{"min", .12}, {"max", .12}}}, {"term_inverted", false}};
  EXPECT_FALSE(server::playbook_condition_reason(setup, inputs, 0));
}
TEST(PlaybookTemplates, MatchesOriginalWebPickerGoldenCases) {
  std::ifstream input(std::filesystem::path(OPENPORT_TEST_DATA_DIR) / "template-parity.json");
  const auto fixtures = json::parse(input);
  std::size_t index = 0;
  for (const auto& test : fixtures.at("cases")) {
    SCOPED_TRACE(index++);
    const auto& near = fixtures.at("chains").at(test.at("near").get<std::size_t>());
    const auto& far = fixtures.at("chains").at(test.at("far").get<std::size_t>());
    if (test.at("result").contains("reason")) {
      // The terminal shows these reasons; they name the contract, strike or range.
      try { (void)server::build_template(test.at("template"), near, far); ADD_FAILURE() << "Built a failing case"; }
      catch (const std::exception& error) { EXPECT_EQ(error.what(), test.at("result").at("reason").get<std::string>()); }
    } else {
      auto actual = server::build_template(test.at("template"), near, far);
      for (auto& leg : actual["legs"]) leg.erase("quote");
      EXPECT_EQ(actual, test.at("result"));
    }
  }
}
TEST(Playbooks, ManagementUsesClosingPremiumAndEarlyCloseDeadline) {
  auto setup = definition(); setup["version"] = 1;
  const auto bracket = server::playbook_bracket(setup.at("management"), money("-2"));
  EXPECT_EQ(bracket.take_profit->limit_price, money("1"));
  EXPECT_EQ(bracket.stop_loss->trigger->level, money("4"));
  EXPECT_THROW(server::playbook_bracket(setup.at("management"), money("2")), std::invalid_argument);
  setup["management"] = {{"close_by", "15:45"}, {"take_profit_percent", 150}, {"stop_underlying", {{"direction", "below"}, {"level", 5900}}}};
  EXPECT_EQ(server::playbook_bracket(setup.at("management"), money("2")).take_profit->limit_price, money("-3"));
  // SPX combos price in nickels: half of a 1.25 credit is 0.625, asked at 0.60, not 0.63.
  trading::OrderRequest spread;
  spread.legs = {{"SPXW  261127P05995000", trading::Side::Sell, 1}, {"SPXW  261127P05990000", trading::Side::Buy, 1}};
  EXPECT_EQ(server::playbook_tick(spread), money("0.05"));
  setup["management"] = {{"close_by", "15:45"}, {"take_profit_percent", 50}};
  EXPECT_EQ(server::playbook_bracket(setup.at("management"), money("-1.25"), server::playbook_tick(spread)).take_profit->limit_price, money("0.60"));
  spread.legs = {{"QQQ   260916P00474000", trading::Side::Sell, 1}, {"QQQ   260916P00469000", trading::Side::Buy, 1}};
  EXPECT_EQ(server::playbook_tick(spread), money("0.01"));
  EXPECT_EQ(server::playbook_bracket(setup.at("management"), money("-1.25"), server::playbook_tick(spread)).take_profit->limit_price, money("0.62"));
  EXPECT_EQ(server::playbook_deadline(setup, md::new_york_to_utc({2026, 11, 27}, 10, 0)), md::new_york_to_utc({2026, 11, 27}, 13, 0));
  EXPECT_EQ(server::playbook_tag(setup), "playbook:morning@v1");
}

std::vector<trading::EvaluationDay> history(Money net, Money low, Money high, bool high_first = false) {
  std::vector<trading::EvaluationDay> days;
  for (int index = 0; index < 10; ++index) {
    trading::EvaluationDay day;
    day.day = md::date_from_days(md::days_since_epoch({2026, 8, 3}) + index);
    day.open_equity = money("1000"); day.close_equity = day.open_equity + net;
    day.low_equity = day.open_equity + low; day.high_equity = day.open_equity + high;
    day.low_at = md::new_york_to_utc(day.day, high_first ? 11 : 10, 0);
    day.high_at = md::new_york_to_utc(day.day, high_first ? 10 : 11, 0);
    days.push_back(day);
  }
  return days;
}
TEST(PassOdds, HandCheckedPassFailAndNeitherUseExactPlanRules) {
  trading::Evaluation current;
  current.starting_balance = current.peak = current.day_close_equity = money("1000");
  current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("100"); rules.max_drawdown = money("100");
  auto result = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("-5"), money("20")), 5, 100, 7);
  EXPECT_DOUBLE_EQ(result.pass, 1); EXPECT_EQ(result.median_days_to_pass, 5);
  EXPECT_DOUBLE_EQ(result.fail, 0); EXPECT_DOUBLE_EQ(result.neither, 0);
  result = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("-5"), money("20")), 2, 100, 7);
  EXPECT_DOUBLE_EQ(result.neither, 1);
  result = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("-100"), money("20")), 1, 100, 7);
  EXPECT_DOUBLE_EQ(result.fail, 1); EXPECT_FALSE(result.median_days_to_pass);
  EXPECT_THROW((void)trading::pass_odds(current, rules, money("1000"), {}, 1, 100, 7), std::invalid_argument);
}
TEST(PassOdds, IntradayVersusEndOfDayRatchetAndExtremaOrder) {
  trading::Evaluation current; current.starting_balance = current.peak = money("1000"); current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("200"); rules.max_drawdown = money("100");
  const auto days = history(money("10"), money("-60"), money("50"), true);
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), days, 1, 100, 12).fail, 1);
  rules.drawdown_mode = trading::DrawdownMode::EndOfDay;
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), days, 1, 100, 12).neither, 1);
  rules.drawdown_mode = trading::DrawdownMode::Intraday;
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), history(money("10"), money("-60"), money("50")), 1, 100, 12).neither, 1);
  current.floor_locked = true; rules.lock_balance = money("900");
  EXPECT_DOUBLE_EQ(trading::pass_odds(current, rules, money("1000"), days, 1, 100, 12).neither, 1);
}
TEST(PassOdds, TrendingDaysWhoseExtremesMissTheOpenStillCount) {
  trading::Evaluation current; current.starting_balance = current.peak = money("1000"); current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("100"); rules.max_drawdown = money("100");
  // Each day gapped up, so its first marked observation, and its low, sit above the open.
  const auto up = trading::pass_odds(current, rules, money("1000"), history(money("20"), money("5"), money("20")), 5, 100, 7);
  EXPECT_EQ(up.historical_days, 10U);
  EXPECT_DOUBLE_EQ(up.pass, 1);
  // Gap-down days whose highs stay under the open still reach the floor through their lows.
  const auto down = trading::pass_odds(current, rules, money("1000"), history(money("-40"), money("-40"), money("-10")), 3, 100, 7);
  EXPECT_DOUBLE_EQ(down.fail, 1);
  EXPECT_DOUBLE_EQ(down.pass + down.fail + down.neither, 1);
}
TEST(PassOdds, SeedIsRepeatableAndOlderMissingExtremaAreNotInvented) {
  trading::Evaluation current; current.starting_balance = current.peak = money("1000"); current.floor = money("900");
  trading::AccountRules rules; rules.profit_target = money("100"); rules.max_drawdown = money("100");
  auto days = history(money("10"), money("-10"), money("20"));
  days[0].close_equity = money("920"); days[0].low_equity = money("900");
  const auto first = trading::pass_odds(current, rules, money("1000"), days, 10, 100, 42);
  const auto second = trading::pass_odds(current, rules, money("1000"), days, 10, 100, 42);
  EXPECT_EQ(first.pass, second.pass); EXPECT_EQ(first.fail, second.fail); EXPECT_EQ(first.seed, 42U);
  days[0].low_equity.reset();
  EXPECT_THROW((void)trading::pass_odds(current, rules, money("1000"), days, 10, 100, 42), std::invalid_argument);
  // Old run commands have no playbook fields and keep their serialized shape.
  server::TradingCommand request;
  const auto restored = json(request).get<server::TradingCommand>();
  EXPECT_EQ(restored.kind, server::TradingCommand::Kind::Submit);
  EXPECT_EQ(restored.cancel_reason, trading::Reason::USER_CANCEL);
  EXPECT_FALSE(json(request).contains("cancel_reason"));
  request.kind = server::TradingCommand::Kind::Cancel;
  request.cancel_reason = trading::Reason::PLAYBOOK_TIME_STOP;
  EXPECT_EQ(json(request).get<server::TradingCommand>().cancel_reason, trading::Reason::PLAYBOOK_TIME_STOP);
}
struct ScenarioFixture {
  test::RecordingFile file;
  providers::Scenario scenario;
  explicit ScenarioFixture(bool multiple_symbols = false)
      : ScenarioFixture(multiple_symbols ? std::vector<std::string>{"SPX", "SPY"} : std::vector<std::string>{"SPX"}) {}
  explicit ScenarioFixture(const std::vector<std::string>& symbols) {
    const auto path = file.directory / "playbook-scenario.json";
    { std::ofstream out(path); out << R"({"id":"playbook-test","title":"Playbook test","description":"Simulated test day","symbols":["SPX"],"session":"regular","date":"2026-11-27","seed":81723,"generator":1,"drift":[[1,0.001]],"volatility":0.12,"iv_shift":0,"spot_vol":-2})"; }
    if (symbols != std::vector<std::string>{"SPX"}) {
      std::ifstream input(path); auto document = json::parse(input); input.close();
      document["symbols"] = symbols;
      std::ofstream output(path); output << document.dump();
    }
    scenario = providers::read_scenario(path);
    providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  }
  json setup() const {
    auto value = definition();
    value["window"]["end"] = "09:31";
    value["management"] = {{"close_by", "09:32"}};
    value["structure"]["template"]["target"] = {{"mode", "delta"}, {"value", 40}};
    value["structure"]["expiry"]["max"] = 1;
    return value;
  }
};
TEST(Playbooks, LiveAutoRecoversEntryAndOverdueTimeStopWithoutDuplicates) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  server::Desk::Options options;
  options.paper_journal = fixture.file.directory / "main.jsonl";
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  {
    server::Desk desk("live test", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading(); desk.replay_batch(first->events, first->received, first->time);
    ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
    ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, desk.market_time()).decision.ok());
    auto view = desk.trading_view();
    ASSERT_EQ(view->snapshot->recent_orders.size(), 1U);
    EXPECT_EQ(view->snapshot->recent_orders.front().actor, "system");
    EXPECT_EQ(view->snapshot->recent_orders.front().status, trading::OrderStatus::Filled);
    const auto forward = server::playbook_forward_report(json::parse(view->playbooks_json), *view).at("morning");
    EXPECT_TRUE(forward.at("running").get<bool>());
    EXPECT_EQ(forward.at("entries"), 1);
    EXPECT_EQ(forward.at("report").at("all").at("trades"), 0);
    desk.stop();
  }
  {
    server::Desk desk("live test", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading(); desk.replay_batch(first->events, first->received, first->time);
    ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U);
    // Skip the deadline while the server is down; its first newer snapshot catches up.
    while (const auto batch = batches.next()) {
      if (batch->time < first->time + 3 * md::kNanosPerMinute) continue;
      desk.replay_batch(batch->events, batch->received, batch->time);
      break;
    }
    const auto view = desk.trading_view();
    ASSERT_EQ(view->snapshot->recent_orders.size(), 2U);
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->recent_orders.back().request.note.starts_with("Playbook automatic time stop"));
    EXPECT_EQ(view->snapshot->recent_orders.back().actor, "system");
    const auto forward = server::playbook_forward_report(json::parse(view->playbooks_json), *view).at("morning");
    EXPECT_EQ(forward.at("windows").size(), 1U);
    EXPECT_EQ(forward.at("entries"), 1);
    EXPECT_EQ(forward.at("time_stops"), 1);
    EXPECT_EQ(forward.at("rejected_entries"), 0);
    EXPECT_EQ(forward.at("report").at("all"), server::playbook_report(json::parse(view->playbooks_json), *view).at("morning").at("all"));
    EXPECT_EQ(forward.at("report").at("all").at("trades"), 1);
    EXPECT_NEAR(forward.at("days_running").get<double>(), 3.0 / 1440, 1e-9);
    EXPECT_EQ(forward.at("versions").at("1"), forward.at("report"));
    ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time()).decision.ok());
    desk.stop();
  }
  server::Playbooks recovered(fixture.file.directory / "playbooks.json");
  const auto windows = recovered.publication("main", false).at("forward_tests").at("morning").at("windows");
  ASSERT_EQ(windows.size(), 1U);
  EXPECT_FALSE(windows.front().at("ended").is_null());
  EXPECT_EQ(windows.front().at("end_order"), "3");
}
TEST(Playbooks, LiveStaleFeedBlocksEntryAndCloseUntilAFreshSnapshot) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  server::Desk::Options options;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  options.paper.limits.max_quote_age = 30 * md::kNanosPerSecond;
  server::Desk desk("live test", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); desk.replay_batch(first->events, first->received, first->time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
  server::TradingCommand mode;
  mode.kind = server::TradingCommand::Kind::Playbook;
  mode.note = json{{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}.dump();
  server::TradingReply reply;
  // Market clock stays at 09:30; the driver's 09:32 shows this feed has stopped.
  desk.command(mode, [&](auto result) { reply = std::move(result); }, first->time, first->received + 2 * md::kNanosPerMinute);
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
  EXPECT_NE(reply.playbook_result.find("stalled"), std::string::npos);
  const auto next = batches.next(); ASSERT_TRUE(next);
  desk.replay_batch(next->events, next->received, next->time);
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U);
  // Force evaluation past close_by while no fresh underlying snapshot has arrived.
  desk.command(mode, [&](auto result) { reply = std::move(result); }, first->time + 3 * md::kNanosPerMinute, first->received + 3 * md::kNanosPerMinute);
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U);
  EXPECT_FALSE(desk.trading_view()->snapshot->positions.empty());
  while (const auto batch = batches.next()) {
    if (batch->time < first->time + 3 * md::kNanosPerMinute) continue;
    desk.replay_batch(batch->events, batch->received, batch->time);
    break;
  }
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 2U);
  EXPECT_TRUE(desk.trading_view()->snapshot->positions.empty());
}
TEST(Playbooks, LiveAutoTradesNamedPracticeAndEvaluationAccounts) {
  for (const auto* plan : {"practice", "eod-100k"}) {
    SCOPED_TRACE(plan);
    ScenarioFixture fixture;
    md::RecordingReader reader(fixture.file.path);
    providers::ReplayBatches batches(reader, reader.header().subscription);
    const auto first = batches.next(); ASSERT_TRUE(first);
    server::Desk::Options options;
    options.paper_journal = fixture.file.directory / "main.jsonl";
    options.paper_accounts = fixture.file.directory / "accounts";
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    server::Desk desk("live test", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading(); desk.replay_batch(first->events, first->received, first->time);
    server::TradingCommand create;
    create.kind = server::TradingCommand::Kind::CreateAccount;
    create.name = "Forward"; create.rules = server::find_plan(plan)->rules;
    create.initial_cash = server::find_plan(plan)->initial_cash;
    server::TradingReply reply;
    desk.command(create, [&](auto result) { reply = std::move(result); }, first->time, first->received);
    ASSERT_FALSE(reply.account.empty()) << reply.decision.message;
    const auto account = reply.account;
    ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
    server::TradingCommand mode;
    mode.account = account; mode.actor = "alice"; mode.kind = server::TradingCommand::Kind::Playbook;
    mode.note = json{{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}.dump();
    desk.command(mode, [&](auto result) { reply = std::move(result); }, first->time, first->received);
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    ASSERT_EQ(desk.trading_view(account)->snapshot->recent_orders.size(), 1U) << reply.playbook_result;
    EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
    const auto view = desk.trading_view(account);
    const auto forward = server::playbook_forward_report(json::parse(view->playbooks_json), *view).at("morning");
    EXPECT_EQ(forward.at("entries"), 1);
    EXPECT_EQ(forward.at("windows")[0].at("actor"), "alice");
    EXPECT_EQ(forward.at("windows")[0].at("account"), account);
    desk.stop();
  }
}
TEST(Playbooks, JournaledAutomaticRejectionSurvivesRestartAndCountsOnlySubmissions) {
  test::RecordingFile file;
  test::ScriptedMarket market; market.time = md::new_york_to_utc({2026, 9, 22}, 9, 35);
  const auto path = file.directory / "playbooks.json";
  server::Playbooks store(path);
  store.change({{"action", "create"}, {"definition", definition()}}, "main", false);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", false, market.time);
  trading::TradingSession session({}, market.time);
  market.seed(session);
  session.set_actor("system");
  session.trip_kill("test", market.time);
  auto rejected = market.market("playbook:rejected");
  rejected.tags = {"playbook:morning@v1"}; rejected.note = R"({"automatic":true})";
  EXPECT_FALSE(session.submit(rejected, market.time).decision.ok());
  session.reset_kill("test", market.time);
  server::TradingView view; view.snapshot = session.snapshot(); view.contracts = session.contracts(); view.config = session.config();
  server::Playbooks recovered(path);
  bool sent = false;
  recovered.evaluate("main", false, market.time, {}, view, [](const auto&, const auto&) { return server::PlaybookInputs{}; },
      [](const auto&, double) { return trading::OrderPreview{}; }, [&](const auto&) { sent = true; return server::TradingReply{}; }, [](auto, auto) {});
  EXPECT_FALSE(sent);
  EXPECT_NE(recovered.publication("main", false).at("reasons").at("morning:SPX").get<std::string>().find("Automatic entry refused for this day"), std::string::npos);
  const auto forward = server::playbook_forward_report(recovered.publication("main", false), view).at("morning");
  EXPECT_EQ(forward.at("rejected_entries"), 1);
  EXPECT_EQ(forward.at("entries"), 0);
  EXPECT_EQ(forward.at("report").at("all").at("trades"), 0);
}
TEST(Playbooks, ForwardWindowsVersionArchiveAndLegacyCatalogues) {
  test::RecordingFile file;
  const auto now = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  server::Playbooks legacy;
  legacy.change({{"action", "create"}, {"definition", definition()}}, "main", true);
  EXPECT_FALSE(legacy.catalogue().contains("forward_tests"));
  server::Playbooks store(file.directory / "playbooks.json", legacy.catalogue());
  const json automatic{{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}};
  store.change(automatic, "main", false, now, "alice");
  store.change(automatic, "main", false, now, "alice");
  store.change(automatic, "practice", false, now, "bob");
  auto edit = definition(); edit["version"] = 1;
  store.change({{"action", "update"}, {"definition", edit}}, "main", false, now + md::kNanosPerMinute, "editor");
  for (const auto* owner : {"main", "practice"}) {
    const auto windows = store.publication(owner, false).at("forward_tests").at("morning").at("windows");
    ASSERT_EQ(windows.size(), 2U);
    EXPECT_EQ(windows.front().at("ended"), windows.back().at("started"));
    EXPECT_EQ(windows.back().at("version"), 2);
    EXPECT_EQ(windows.back().at("actor"), "editor");
  }
  store.change({{"action", "delete"}, {"id", "morning"}, {"version", 2}}, "main", false, now + 2 * md::kNanosPerMinute);
  server::Playbooks recovered(file.directory / "playbooks.json");
  EXPECT_EQ(recovered.catalogue(), store.catalogue());
  EXPECT_EQ(recovered.publication("main", false).at("modes").at("morning"), "off");
  const auto catalogue = recovered.catalogue();
  for (const auto& window : catalogue.at("forward_tests")) { EXPECT_FALSE(window.at("ended").is_null()); }
  EXPECT_FALSE(recovered.publication("main", true).contains("forward_tests"));
}
TEST(Playbooks, StagingUsesPreviewWithoutJournalOrdersAndExpiresOrDismisses) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
  const auto count = desk.trading_view()->snapshot->account_version;
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  auto publication = json::parse(reply.playbook_result);
  ASSERT_EQ(publication.at("staged").size(), 1U) << publication.dump();
  EXPECT_EQ(desk.trading_view()->snapshot->account_version, count);
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
  const auto stage = publication.at("staged")[0];
  EXPECT_EQ(stage.at("units"), 1);
  EXPECT_FALSE(stage.at("max_loss").is_null());
  reply = command(desk, {{"action", "dismiss"}, {"staged", stage.at("id")}}, desk.market_time());
  EXPECT_TRUE(json::parse(reply.playbook_result).at("staged").empty());
  reply = command(desk, {{"action", "send"}, {"staged", stage.at("id")}}, desk.market_time());
  EXPECT_FALSE(reply.decision.ok());
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= md::new_york_to_utc(fixture.scenario.date, 9, 31)) break;
  }
  EXPECT_TRUE(json::parse(desk.trading_view()->playbooks_json).at("staged").empty());
}
TEST(Playbooks, AStageKeepsItsIdWhileQuotesMoveAndSendsTheLatestOrder) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  auto setup = fixture.setup();
  setup["window"]["end"] = "09:45";
  // SPX exits: the take-profit must land on a nickel for the combo to be accepted.
  setup["management"] = {{"close_by", "09:50"}, {"take_profit_percent", 50}, {"stop_credit_multiple", 2}};
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  ASSERT_EQ(json::parse(reply.playbook_result).at("staged").size(), 1U) << reply.playbook_result;
  auto staged = json::parse(reply.playbook_result).at("staged")[0];
  // The market moves on before the trader clicks Send. While the same contracts are
  // selected the stage keeps its ID; other contracts are a different stage.
  const auto contracts = [](const json& stage) {
    std::string symbols;
    for (const auto& leg : stage.at("legs")) symbols += leg.at("symbol").get<std::string>() + ";";
    return symbols;
  };
  for (bool repriced = false; !repriced;) {
    const auto batch = batches.next(); ASSERT_TRUE(batch);
    desk.replay_batch(batch->events, batch->received, batch->time);
    ASSERT_LT(desk.market_time(), md::new_york_to_utc(fixture.scenario.date, 9, 45));
    const auto stages = json::parse(desk.trading_view()->playbooks_json).at("staged");
    ASSERT_EQ(stages.size(), 1U) << json::parse(desk.trading_view()->playbooks_json).at("reasons").dump();
    const bool same = contracts(stages[0]) == contracts(staged);
    EXPECT_EQ(stages[0].at("id") == staged.at("id"), same);
    repriced = same && stages[0].at("time") != staged.at("time");
    if (!same) staged = stages[0];  // What the trader now sees.
  }
  server::TradingCommand send;
  send.kind = server::TradingCommand::Kind::Playbook;
  send.actor = "alice";
  send.note = json{{"action", "send"}, {"staged", staged.at("id")}}.dump();
  desk.command(send, [&](server::TradingReply result) { reply = std::move(result); }, desk.market_time(), desk.market_time());
  ASSERT_TRUE(reply.decision.ok() && reply.error_code.empty()) << reply.decision.message;
  const auto& orders = desk.trading_view()->snapshot->recent_orders;
  ASSERT_FALSE(orders.empty());
  EXPECT_TRUE(orders[0].request.client_order_id.starts_with("playbook:" + staged.at("id").get<std::string>() + ":"));
  EXPECT_EQ(orders[0].actor, "alice");
  ASSERT_TRUE(orders[0].request.bracket && orders[0].request.bracket->take_profit);
  EXPECT_EQ(orders[0].request.bracket->take_profit->limit_price->micros() % 50'000, 0);
}
TEST(Playbooks, ScenarioAutoEntriesAndTimeStopsReproduceJournalAndVerification) {
  ScenarioFixture fixture;
  server::Playbooks store;
  auto setup = fixture.setup();
  setup["conditions"]["price"] = {{"reference", "day_open"}, {"direction", "above"}, {"value", -1000}};
  setup["management"]["take_profit_percent"] = 1;
  setup["management"]["stop_credit_multiple"] = 100;
  setup["sizing"] = {{"floor_share", .8}};
  store.change({{"action", "create"}, {"definition", setup}}, "main", true);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", true);
  std::string golden;
  for (int repeat = 0; repeat < 4; ++repeat) {
    md::RecordingReader reader(fixture.file.path);
    const auto journal = fixture.file.directory / ("auto-" + std::to_string(repeat) + ".jsonl");
    server::Desk::Options options; options.replay = true;
    options.playbook_cancel_labels = repeat >= 2;
    options.paper_journal = journal;
    options.initial_playbooks = store.catalogue().dump();
    options.paper.initial_cash = money("50000");
    options.paper.guardrails.soft_floor = money("49000");
    options.candles = std::make_shared<server::CandleStore>();
    options.run_input = server::scenario_input(fixture.scenario, fixture.scenario.date, fixture.scenario.seed);
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    while (const auto batch = batches.next()) {
      desk.replay_batch(batch->events, batch->received, batch->time);
      if (desk.market_time() >= md::new_york_to_utc(fixture.scenario.date, 9, 33)) break;
    }
    const auto view = desk.trading_view();
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    ASSERT_GE(view->snapshot->recent_fills.size(), 4U) << view->playbooks_json;
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->open_orders.empty());
    EXPECT_EQ(view->snapshot->recent_orders.front().request.tags.front(), "playbook:morning@v1");
    // Automatic entries and time stops are the system's actions.
    for (const auto& order : view->snapshot->recent_orders) {
      EXPECT_EQ(order.actor, "system") << order.request.client_order_id;
    }
    const auto expected_reason = repeat >= 2 ? trading::Reason::PLAYBOOK_TIME_STOP : trading::Reason::USER_CANCEL;
    int cancelled_exits = 0;
    for (const auto& order : view->snapshot->recent_orders) {
      if (order.parent && order.status == trading::OrderStatus::Cancelled) {
        ++cancelled_exits;
        EXPECT_EQ(order.reason.code, expected_reason);
      }
    }
    EXPECT_GE(cancelled_exits, 2);
    const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(journal));
    for (const auto& order : recovered.snapshot()->recent_orders) {
      if (order.parent && order.status == trading::OrderStatus::Cancelled) { EXPECT_EQ(order.reason.code, expected_reason); }
    }
    const auto report = server::playbook_report(store.catalogue(), *view).at("morning");
    EXPECT_EQ(report.at("all").at("trades"), 1);
    EXPECT_EQ(report.at("all").at("adherence"), 1);
    EXPECT_EQ(report.at("trades")[0].at("rules").at("time_stop"), true);
    auto malformed = *view;
    auto snapshot = std::make_shared<trading::TradingSnapshot>(*view->snapshot);
    snapshot->recent_orders.mut(0).request.note = R"({"playbook":42,"max_units":1,"floor_share":0.8})";
    malformed.snapshot = snapshot;
    EXPECT_NO_THROW(server::playbook_report(store.catalogue(), malformed));
    std::ifstream input(journal); std::ostringstream bytes; bytes << input.rdbuf();
    if (repeat % 2 == 0) { golden = bytes.str(); } else { EXPECT_EQ(bytes.str(), golden); }
    desk.stop();
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
/// Each contract's quote in a recording as of `time`: its latest quote then.
std::map<std::string, std::pair<Money, Money>> quotes_at(const std::filesystem::path& file, md::Timestamp time) {
  md::RecordingReader reader(file);
  std::map<md::InstrumentId, std::string> symbols;
  std::map<std::string, std::pair<Money, Money>> quotes;
  while (const auto record = reader.next()) {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&record->event))
      symbols[definition->id] = definition->contract.osi_symbol();
    else if (const auto* quote = std::get_if<md::OptionQuote>(&record->event); quote && quote->ts <= time)
      quotes[symbols.at(quote->id)] = {Money::from_double(quote->bid), Money::from_double(quote->ask)};
  }
  return quotes;
}
TEST(Playbooks, AutomaticEntriesTradeTheBookOfTheirOwnMarketInstant) {
  // B46: a replay writes QQQ's snapshot of each market time before SPX's. QQQ's
  // analytics at 09:45:00 started the playbook, whose SPX entry traded SPX's 09:44:45
  // book, stamped 09:45:00. Backtests replay days the same way.
  ScenarioFixture fixture(std::vector<std::string>{"QQQ", "SPX", "SPY"});
  auto setup = fixture.setup();
  setup["window"] = {{"start", "09:45"}, {"end", "10:00"}, {"weekdays", {1, 2, 3, 4, 5}}};
  setup["management"] = {{"close_by", "10:30"}};
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", true);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", true);
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.initial_playbooks = store.catalogue().dump();
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto entry = md::new_york_to_utc(fixture.scenario.date, 9, 45);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= entry) break;
  }
  const auto view = desk.trading_view();
  ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
  const auto fills = view->snapshot->recent_fills.to_vector();
  ASSERT_EQ(fills.size(), 2U) << view->playbooks_json;
  const auto book = quotes_at(fixture.file.path, entry);
  for (const auto& fill : fills) {
    EXPECT_EQ(fill.time, entry);
    EXPECT_EQ(fill.quote_time, entry);
    const auto& [bid, ask] = book.at(fill.symbol);
    EXPECT_EQ(fill.price, fill.side == trading::Side::Buy ? ask : bid) << fill.symbol;
  }
}
TEST(Playbooks, WinRateLeavesBreakevenTradesOutAsTheJournalDoes) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.fee_per_contract = Money{};
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession session(config, market.time);
  auto setup = definition(); setup["management"] = {{"close_by", "11:30"}};
  setup["guardrails"]["max_entries_per_day"] = 10;
  setup["guardrails"]["cooldown_minutes"] = 0;
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", false);
  // A win, a loss and a breakeven: bought at the 4.20 ask, sold at 5.20, 3.20 and 4.20 bids.
  for (const auto& [bid, ask] : {std::pair{"5.20", "5.40"}, std::pair{"3.20", "3.40"}, std::pair{"4.20", "4.40"}}) {
    market.seed(session);
    auto entry = market.market(std::string("entry ") + bid);
    entry.tags = {"playbook:morning@v1"};
    ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
    market.next();
    session.on_quotes({market.quote(bid, ask)}, {market.valuation()}, market.time);
    ASSERT_TRUE(session.submit(market.market(std::string("close ") + bid, 1, trading::Side::Sell), market.time).decision.ok());
    market.next();
  }
  server::TradingView view; view.snapshot = session.snapshot(); view.contracts = session.contracts(); view.config = config;
  const auto report = server::playbook_report(store.catalogue(), view).at("morning");
  EXPECT_EQ(report.at("all").at("trades"), 3);
  EXPECT_EQ(report.at("trades")[2].at("net"), "0.00");
  EXPECT_DOUBLE_EQ(report.at("all").at("win_rate").get<double>(), .5);
}
TEST(Playbooks, ExpectancyAndRuleSplitsUseWholeStrategies) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession session(config, market.time);
  auto setup = definition(); setup["management"] = {{"close_by", "11:30"}};
  setup["guardrails"]["max_entries_per_day"] = 10;
  setup["guardrails"]["cooldown_minutes"] = 0;
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", false);
  for (int index = 0; index < 2; ++index) {
    market.seed(session);
    auto entry = market.market("entry" + std::to_string(index), index + 1);
    entry.tags = {"playbook:morning@v1"};
    ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
    market.next();
    session.on_quotes({market.quote(index == 0 ? "5.20" : "3.20", index == 0 ? "5.40" : "3.40")}, {market.valuation()}, market.time);
    ASSERT_TRUE(session.submit(market.market("close" + std::to_string(index), index + 1, trading::Side::Sell), market.time).decision.ok());
    market.next();
  }
  server::TradingView view; view.snapshot = session.snapshot(); view.contracts = session.contracts(); view.config = config;
  const auto report = server::playbook_report(store.catalogue(), view).at("morning");
  EXPECT_EQ(report.at("all").at("trades"), 2);
  EXPECT_DOUBLE_EQ(report.at("all").at("win_rate").get<double>(), .5);
  EXPECT_EQ(report.at("followed").at("trades"), 1);
  EXPECT_EQ(report.at("deviated").at("trades"), 1);
  const auto first = Money::parse(report.at("trades")[0].at("net").get<std::string>());
  const auto second = Money::parse(report.at("trades")[1].at("net").get<std::string>());
  EXPECT_EQ(report.at("all").at("expectancy"), (first + second).prorate(1, 2).str());
  EXPECT_DOUBLE_EQ(report.at("all").at("profit_factor").get<double>(), first.dollars() / -second.dollars());
  EXPECT_TRUE(report.at("all").at("average_r").is_null());
  // Long calls need their premium: one bought at 4.20, two at 4.20.
  EXPECT_EQ(report.at("trades")[0].at("buying_power"), "420.00");
  EXPECT_EQ(report.at("trades")[1].at("buying_power"), "840.00");
  const auto returns = first.dollars() / 420 + second.dollars() / 840;
  EXPECT_NEAR(report.at("trades")[0].at("return_on_buying_power").get<double>(), first.dollars() / 420, 1e-12);
  EXPECT_NEAR(report.at("all").at("average_return_on_buying_power").get<double>(), returns / 2, 1e-12);
}
class DeskSource final : public server::MetricsSource {
 public:
  explicit DeskSource(server::Desk& desk) : desk_(desk) {}
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override { return desk_.metrics(symbol); }
  server::EngineStatus status() const override { server::EngineStatus value; value.trading = desk_.trading_status(); value.accounts = desk_.accounts(); return value; }
  using MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view() const override { return desk_.trading_view(); }
  std::shared_ptr<const server::TradingView> trading_view(std::string_view account) const override { return desk_.trading_view(account); }
  md::Timestamp wall_time() const override { return desk_.market_time(); }
  bool post_trading(server::TradingCommand request, server::TradingCompletion complete) override {
    desk_.command(std::move(request), std::move(complete), desk_.market_time(), desk_.market_time()); return true;
  }
 private:
  server::Desk& desk_;
};
server::ApiResponse api(DeskSource& source, std::string method, std::string route, json body = nullptr) {
  server::ApiResponse reply;
  server::handle_api_async({std::move(method), std::move(route), body.is_null() ? "" : body.dump()}, source,
                          [&](server::ApiResponse value) { reply = std::move(value); });
  return reply;
}
TEST(PlaybookApi, LiveForwardPublicationIncludesWindowsAndJournalStats) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  server::Desk::Options options;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("live test", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); desk.replay_batch(first->events, first->received, first->time);
  DeskSource source(desk);
  ASSERT_EQ(api(source, "POST", "/api/playbooks", fixture.setup()).status, 200);
  const auto enabled = api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "auto"}});
  ASSERT_EQ(enabled.status, 200) << enabled.body;
  test::capture_contract("playbooks", "PUT", "/api/playbooks/morning/mode", enabled);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= first->time + 2 * md::kNanosPerMinute) break;
  }
  const auto response = api(source, "GET", "/api/playbooks?account=main");
  ASSERT_EQ(response.status, 200) << response.body;
  const auto forward = json::parse(response.body).at("forward_tests").at("morning");
  EXPECT_EQ(forward.at("report").at("all").at("trades"), 1);
  EXPECT_EQ(forward.at("time_stops"), 1);
  EXPECT_EQ(forward.at("windows")[0].at("account"), "main");
  test::capture_contract("playbooks", "GET", "/api/playbooks?account=main", response);
}
TEST(PlaybookApi, StageHttpOrderRoundTripsThroughTheSubmissionParser) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  DeskSource source(desk);
  auto setup = fixture.setup();
  setup["management"]["take_profit_percent"] = 50;
  setup["management"]["stop_credit_multiple"] = 2;
  ASSERT_EQ(api(source, "POST", "/api/playbooks", setup).status, 200);
  ASSERT_EQ(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "stage"}}).status, 200);
  const auto publication = json::parse(api(source, "GET", "/api/playbooks").body);
  ASSERT_EQ(publication.at("staged").size(), 1U) << publication.dump();
  const auto stage = publication.at("staged")[0];
  const auto& body = stage.at("order");
  EXPECT_TRUE(body.at("limit_price").is_string());
  EXPECT_EQ(body.at("type"), "limit");
  EXPECT_EQ(body.at("time_in_force"), "gtc");
  EXPECT_FALSE(body.contains("symbol"));
  EXPECT_FALSE(body.at("bracket").at("take_profit").contains("trigger"));
  EXPECT_FALSE(body.at("bracket").at("stop_loss").contains("limit_price"));
  const auto sent = api(source, "POST", "/api/orders", body);
  ASSERT_EQ(sent.status, 201) << sent.body;
  const auto id = std::stoull(json::parse(sent.body).at("order").at("id").get<std::string>());
  const auto& submitted = desk.trading_view()->snapshot->recent_orders[id - 1].request;
  EXPECT_EQ(json(submitted), stage.at("request"));
  EXPECT_EQ(server::order_request_json(submitted), body);
  EXPECT_EQ(api(source, "GET", "/api/orders/999999").status, 404);
  EXPECT_EQ(api(source, "GET", "/api/orders/not-an-id").status, 400);
  ASSERT_EQ(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "auto"}}).status, 200);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= md::new_york_to_utc(fixture.scenario.date, 9, 33)) break;
  }
  const auto orders = json::parse(api(source, "GET", "/api/orders").body).at("orders");
  int labelled = 0;
  for (const auto& order : orders) {
    if (!order.at("reason").is_null() && order.at("reason").at("code") == "PLAYBOOK_TIME_STOP") {
      ++labelled;
      const auto detail = api(source, "GET", "/api/orders/" + order.at("id").get<std::string>());
      EXPECT_NE(detail.body.find("Playbook time stop"), std::string::npos);
    }
  }
  EXPECT_GE(labelled, 1);
  EXPECT_NE(api(source, "GET", "/api/trades.csv").body.find("PLAYBOOK_TIME_STOP"), std::string::npos);
}
TEST(PlaybookApi, TimeStopLabelsAnUnfilledEntry) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  DeskSource source(desk);
  ASSERT_EQ(api(source, "POST", "/api/playbooks", fixture.setup()).status, 200);
  ASSERT_EQ(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "stage"}}).status, 200);
  const auto publication = json::parse(api(source, "GET", "/api/playbooks").body);
  ASSERT_EQ(publication.at("staged").size(), 1U) << publication.dump();
  auto body = publication.at("staged")[0].at("order");
  // Single-contract conditional entries may open; conditional combos only close.
  for (const auto& leg : body.at("legs")) if (leg.at("side") == "buy") body["symbol"] = leg.at("symbol");
  body.erase("legs"); body.erase("limit_price");
  body["side"] = "buy"; body["type"] = "market"; body["time_in_force"] = "ioc";
  body["trigger"] = {{"source", "underlying"}, {"direction", "at_or_below"}, {"level", "1.00"}};
  const auto sent = api(source, "POST", "/api/orders", body);
  ASSERT_EQ(sent.status, 201) << sent.body;
  const auto id = json::parse(sent.body).at("order").at("id").get<std::string>();
  ASSERT_EQ(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "auto"}}).status, 200);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() >= md::new_york_to_utc(fixture.scenario.date, 9, 33)) break;
  }
  const auto cancelled = json::parse(api(source, "GET", "/api/orders/" + id).body).at("order");
  EXPECT_EQ(cancelled.at("status"), "cancelled");
  EXPECT_EQ(cancelled.at("filled_quantity"), 0);
  EXPECT_EQ(cancelled.at("reason").at("code"), "PLAYBOOK_TIME_STOP");
}
TEST(PlaybookApi, VersionReadsAndMissingMarketTimeUseAccountCommandPath) {
  server::Desk desk("test", {}, {{"SPX"}}, {}); desk.start_trading();
  DeskSource source(desk);
  ASSERT_EQ(api(source, "POST", "/api/playbooks", definition()).status, 200);
  auto edit = definition(); edit["version"] = 1; edit["name"] = "Version two";
  ASSERT_EQ(api(source, "PUT", "/api/playbooks/morning", edit).status, 200);
  EXPECT_EQ(json::parse(api(source, "GET", "/api/playbooks/morning?version=1").body).at("name"), "Morning spread");
  EXPECT_EQ(json::parse(api(source, "GET", "/api/playbooks/morning").body).at("name"), "Version two");
  EXPECT_EQ(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", "auto"}}).status, 400);
  const auto missing = api(source, "PUT", "/api/playbooks/morning/mode", json::object());
  EXPECT_EQ(missing.status, 400);
  EXPECT_EQ(json::parse(missing.body).at("error").at("message"), "mode is required");
  EXPECT_EQ(json::parse(api(source, "PUT", "/api/playbooks/morning/mode", {{"mode", 3}}).body).at("error").at("message"), "Mode must be off, stage or auto");
  auto unnamed = definition(); unnamed.erase("management");
  EXPECT_EQ(json::parse(api(source, "POST", "/api/playbooks", unnamed).body).at("error").at("message"), "management is required");
  EXPECT_EQ(json::parse(api(source, "GET", "/api/strategy-template?template=%7B%7D").body).at("error").at("message"), "symbol is required");
  EXPECT_EQ(api(source, "GET", "/api/playbooks?version=1&version=2").status, 400);
  EXPECT_EQ(api(source, "GET", "/api/playbooks/morning?version=99").status, 404);
  EXPECT_EQ(api(source, "DELETE", "/api/playbooks/morning?version=1").status, 400);
  EXPECT_EQ(api(source, "DELETE", "/api/playbooks/morning?version=2").status, 200);
  EXPECT_EQ(api(source, "GET", "/api/playbooks/morning?version=1").status, 200);
  // An unknown account is 404 UNKNOWN_ACCOUNT on writes as on reads, not a playbook error.
  const auto unknown = api(source, "PUT", "/api/playbooks/morning/mode?account=zzz", {{"mode", "stage"}});
  EXPECT_EQ(unknown.status, 404);
  EXPECT_EQ(json::parse(unknown.body).at("error").at("code"), "UNKNOWN_ACCOUNT");
  EXPECT_EQ(json::parse(api(source, "GET", "/api/playbooks?account=zzz").body).at("error").at("code"), "UNKNOWN_ACCOUNT");
  EXPECT_TRUE(desk.trading_status().enabled);
}
TEST(Playbooks, FloorShareAndFixedSizesUseTheNormalPreviewAndLiveSendPath) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options;
  options.paper.initial_cash = money("50000"); options.paper.rules.max_drawdown = money("1000");
  options.paper.limits.max_order_contracts = 2;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first);
  desk.replay_batch(first->events, first->received, first->time);
  // A template request without its expiry names it, as the other refusals do.
  DeskSource source(desk);
  const auto undated = api(source, "GET", "/api/strategy-template?symbol=SPX&template=%7B%22kind%22%3A%22iron-butterfly%22%2C%22width%22%3A5%7D");
  EXPECT_EQ(undated.status, 400);
  EXPECT_EQ(json::parse(undated.body).at("error").at("message"), "expiry is required");
  auto setup = fixture.setup(); setup["sizing"] = {{"floor_share", .8}};
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  auto stages = json::parse(reply.playbook_result).at("staged");
  ASSERT_EQ(stages.size(), 1U) << reply.playbook_result;
  const auto request = stages[0].at("request").get<trading::OrderRequest>();
  server::TradingCommand preview; preview.kind = server::TradingCommand::Kind::Preview; preview.order = request; preview.floor_share = .8;
  server::TradingReply projection;
  desk.command(preview, [&](server::TradingReply result) { projection = std::move(result); }, desk.market_time(), desk.market_time());
  ASSERT_TRUE(projection.preview);
  EXPECT_EQ(std::optional(request.quantity), projection.preview->max_units);
  EXPECT_LE(request.quantity, 2);
  EXPECT_LE(*projection.preview->max_loss, money("800"));
  setup["version"] = 1; setup["sizing"] = {{"units", 100}};
  reply = command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  stages = json::parse(reply.playbook_result).at("staged");
  ASSERT_EQ(stages.size(), 1U);
  EXPECT_LE(stages[0].at("units").get<int>(), 2);
  const auto stage_id = stages[0].at("id");
  reply = command(desk, {{"action", "send"}, {"staged", stage_id}}, desk.market_time());
  EXPECT_TRUE(reply.decision.ok()) << reply.decision.message;
  ASSERT_FALSE(desk.trading_view()->snapshot->recent_fills.empty());
  const auto count = desk.trading_view()->snapshot->recent_orders.size();
  reply = command(desk, {{"action", "send"}, {"staged", stage_id}}, desk.market_time());
  EXPECT_FALSE(reply.decision.ok());
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.size(), count);
}
TEST(Playbooks, ConditionsExpireStagesAndFloorSizingRequiresAnActualFloor) {
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  server::Desk desk("test", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first); desk.replay_batch(first->events, first->received, first->time);
  auto setup = fixture.setup();
  command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time());
  auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "stage"}}, desk.market_time());
  ASSERT_FALSE(json::parse(reply.playbook_result).at("staged").empty());
  setup["version"] = 1; setup["conditions"]["price"] = {{"reference", "level"}, {"direction", "above"}, {"value", 100000}};
  reply = command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time());
  EXPECT_TRUE(json::parse(reply.playbook_result).at("staged").empty());
  EXPECT_NE(reply.playbook_result.find("is not above 100000.00"), std::string::npos);
  setup["version"] = 2; setup["conditions"] = json::object(); setup["sizing"] = {{"floor_share", .5}};
  reply = command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time());
  EXPECT_TRUE(json::parse(reply.playbook_result).at("staged").empty());
  EXPECT_NE(reply.playbook_result.find("needs a plan or personal floor"), std::string::npos);
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_orders.empty());
}
TEST(Playbooks, AdherenceDetectsWindowSizeExitTimeAndLossCooldownDeviations) {
  test::ScriptedMarket market; market.time = md::new_york_to_utc({2026, 9, 22}, 9, 35);
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession session(config, market.time);
  auto setup = definition(); setup["window"]["start"] = "09:36"; setup["guardrails"]["max_entries_per_day"] = 10;
  server::Playbooks store; store.change({{"action", "create"}, {"definition", setup}}, "main", false);
  market.seed(session);
  auto entry = market.market("first"); entry.tags = {"playbook:morning@v1"};
  ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
  market.next(); session.on_quotes({market.quote("3.20", "3.40")}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("close-first", 1, trading::Side::Sell), market.time).decision.ok());
  market.time = md::new_york_to_utc({2026, 9, 22}, 9, 36); ++market.observation; market.seed(session);
  entry = market.market("second", 2); entry.tags = {"playbook:morning@v1"};
  ASSERT_TRUE(session.submit(entry, market.time).decision.ok());
  market.time = md::new_york_to_utc({2026, 9, 22}, 12, 0); ++market.observation;
  session.on_quotes({market.quote()}, {market.valuation()}, market.time);
  ASSERT_TRUE(session.submit(market.market("close-second", 2, trading::Side::Sell), market.time).decision.ok());
  server::TradingView view; view.snapshot = session.snapshot(); view.config = config; view.contracts = session.contracts();
  const auto report = server::playbook_report(store.catalogue(), view).at("morning");
  EXPECT_FALSE(report.at("trades")[0].at("rules").at("entry_window").get<bool>());
  const auto rules = report.at("trades")[1].at("rules");
  EXPECT_TRUE(rules.at("entry_window").get<bool>());
  for (const auto* rule : {"size", "exits", "time_stop", "guardrails"}) { EXPECT_FALSE(rules.at(rule).get<bool>()) << rule; }
  ASSERT_TRUE(session.reset_account(config.initial_cash, config.rules, "Next attempt", market.time).decision.ok());
  view.snapshot = session.snapshot();
  const auto retained = server::playbook_report(store.catalogue(), view, false).at("morning");
  EXPECT_EQ(retained.at("trades")[1].at("rules").at("guardrails"), false);
  EXPECT_TRUE(server::playbook_report(store.catalogue(), view).at("morning").at("trades").empty());
}
TEST(Playbooks, OlderRunWithoutCatalogueStillVerifiesAndArchivesUseRecordedVersions) {
  ScenarioFixture fixture;
  const auto directory = fixture.file.directory / "replays";
  std::filesystem::create_directories(directory);
  for (const bool legacy : {true, false}) {
    md::RecordingReader reader(fixture.file.path);
    const auto id = legacy ? "legacy" : "versions";
    const auto file = directory / (std::string(id) + ".jsonl");
    server::Desk::Options options; options.replay = true; options.paper_journal = file;
    options.run_input = server::scenario_input(fixture.scenario, fixture.scenario.date, fixture.scenario.seed);
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
    const auto first = batches.next(); ASSERT_TRUE(first); desk.replay_batch(first->events, first->received, first->time);
    if (!legacy) {
      auto setup = fixture.setup();
      ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
      setup["version"] = 1; setup["name"] = "Later version";
      ASSERT_TRUE(command(desk, {{"action", "update"}, {"definition", setup}}, desk.market_time()).decision.ok());
    }
    desk.stop();
    const auto verified = server::verify_run(file); EXPECT_TRUE(verified.matched) << verified.message;
    const auto recovery = trading::FileJournal::read(file.string());
    bool checked = false;
    for (const auto& record : recovery.records) if (record.type == "run_input") {
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events")) if (event.at("type") == "run_input" && event.at("payload").at("kind") == "start") {
        EXPECT_FALSE(event.at("payload").contains("playbooks")); checked = true;
      }
    }
    EXPECT_TRUE(checked);
  }
  server::ReplayHost::Options options; options.demo = false; options.engine.paper_journal = fixture.file.directory / "main.jsonl";
  server::ReplayHost host(options);
  server::ApiResponse reply;
  ASSERT_TRUE(host.handle({"GET", "/api/replay/history/versions/playbooks/morning?version=1"}, [&](server::ApiResponse result) { reply = std::move(result); }));
  ASSERT_EQ(reply.status, 200) << reply.body;
  EXPECT_EQ(json::parse(reply.body).at("name"), "Morning spread");
  ASSERT_TRUE(host.handle({"GET", "/api/replay/history/versions/playbooks/morning"}, [&](server::ApiResponse result) { reply = std::move(result); }));
  EXPECT_EQ(json::parse(reply.body).at("name"), "Later version");
}
class OddsSource final : public server::MetricsSource {
 public:
  std::shared_ptr<server::TradingView> view = std::make_shared<server::TradingView>();
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { return {}; }
  using MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view() const override { return view; }
};
TEST(PlaybookApi, PassOddsReturnsSeedLabelsAndClearHistoryAndParameterErrors) {
  OddsSource source;
  auto snapshot = std::make_shared<trading::TradingSnapshot>(); snapshot->equity = money("1000"); snapshot->valuation_complete = true;
  snapshot->evaluation.starting_balance = snapshot->evaluation.peak = money("1000"); snapshot->evaluation.floor = money("900");
  snapshot->evaluation.days = history(money("20"), money("-5"), money("20"));
  source.view->snapshot = snapshot; source.view->config.rules.profit_target = money("100"); source.view->config.rules.max_drawdown = money("100");
  const auto response = server::handle_api({"GET", "/api/account/pass-odds?days=5&samples=100&seed=42"}, source);
  ASSERT_EQ(response.status, 200) << response.body;
  const auto result = json::parse(response.body);
  EXPECT_EQ(result.at("seed"), "42"); EXPECT_EQ(result.at("historical_days"), 10); EXPECT_EQ(result.at("pass"), 1);
  EXPECT_EQ(result.at("label"), "Estimate from past results, not a prediction");
  const auto code = [](const server::ApiResponse& r) { return json::parse(r.body).at("error").at("code").get<std::string>(); };
  // A malformed query is the request's fault, not a playbook's.
  const auto days = server::handle_api({"GET", "/api/account/pass-odds?days=0"}, source);
  EXPECT_EQ(days.status, 400); EXPECT_EQ(code(days), "INVALID_REQUEST");
  EXPECT_EQ(code(server::handle_api({"GET", "/api/account/pass-odds?seed=42&seed=42"}, source)), "INVALID_REQUEST");
  EXPECT_EQ(server::handle_api({"GET", "/api/account/pass-odds?playbook=unknown"}, source).status, 404);
  const auto unknown = server::handle_api({"GET", "/api/account/pass-odds?account=zzz"}, source);
  EXPECT_EQ(unknown.status, 404); EXPECT_EQ(code(unknown), "UNKNOWN_ACCOUNT");
  // Too little history is the account's state, named for what it is.
  snapshot->evaluation.days.pop_back();
  const auto short_history = server::handle_api({"GET", "/api/account/pass-odds"}, source);
  EXPECT_EQ(short_history.status, 422); EXPECT_EQ(code(short_history), "PASS_ODDS_UNAVAILABLE");
  EXPECT_NE(short_history.body.find("at least 10 completed days"), std::string::npos);
  snapshot->valuation_complete = false;
  EXPECT_EQ(code(server::handle_api({"GET", "/api/account/pass-odds"}, source)), "PASS_ODDS_UNAVAILABLE");
}
TEST(Playbooks, AutomaticEntryLimitIsSharedAcrossUnderlyingsWithinOneUpdate) {
  ScenarioFixture fixture(true);
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.metrics("SPX") && desk.metrics("SPY")) break;
  }
  auto setup = fixture.setup(); setup["underlyings"] = {"SPX", "SPY"};
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", setup}}, desk.market_time()).decision.ok());
  const auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U) << reply.playbook_result;
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.front().request.tags.front(), "playbook:morning@v1");
  EXPECT_EQ(json::parse(reply.playbook_result).at("reasons").at("morning:SPY"), "Playbook entry limit or loss cooldown");
}
TEST(Playbooks, AnAutomaticOrderLeavesItsEvaluationPublished) {
  // B43: an automatic order publishes the account mid-evaluation. That publication,
  // cached by revision, hid the evaluation's own reasons and stages.
  ScenarioFixture fixture;
  md::RecordingReader reader(fixture.file.path);
  server::Desk::Options options; options.replay = true;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading(); providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto first = batches.next(); ASSERT_TRUE(first); desk.replay_batch(first->events, first->received, first->time);
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", fixture.setup()}}, desk.market_time()).decision.ok());
  auto calls = fixture.setup(); calls["id"] = "staged"; calls["structure"]["template"]["type"] = "call";
  ASSERT_TRUE(command(desk, {{"action", "create"}, {"definition", calls}}, desk.market_time()).decision.ok());
  ASSERT_TRUE(command(desk, {{"action", "mode"}, {"id", "staged"}, {"mode", "stage"}}, desk.market_time()).decision.ok());
  const auto reply = command(desk, {{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, desk.market_time());
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 1U) << reply.playbook_result;
  const auto published = json::parse(desk.trading_view()->playbooks_json);
  EXPECT_EQ(published.at("reasons").at("morning:SPX"), "Automatic order sent") << published.dump();
  EXPECT_EQ(published.at("reasons").at("staged:SPX"), "Ready");
  ASSERT_EQ(published.at("staged").size(), 1U);
  EXPECT_EQ(published.at("staged"), json::parse(reply.playbook_result).at("staged"));
}
/// A 0DTE SPX put spread's day, one snapshot a minute from 09:30. The 4995 wing has no
/// bid from 09:32 until `bid_back` minutes after the open.
std::filesystem::path no_bid_wing_day(const std::filesystem::path& directory, int bid_back) {
  const md::Date date{2026, 9, 16};
  const auto path = directory / "no-bid-wing.oprec";
  auto contract = *md::parse_osi("SPXW  260916P05000000");
  auto lower = contract; lower.strike = 4995;
  md::RecordingHeader header;
  header.provider = "simulated test";
  header.subscription.underlyings = {"SPX"};
  header.started = md::new_york_to_utc(date, 9, 30);
  header.capabilities.poll_interval = std::chrono::seconds(60);
  md::Timestamp time = header.started;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return time; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, recording);
  sink.publish(md::ContractDefinition{0, contract});
  sink.publish(md::ContractDefinition{1, lower});
  for (int minute = 0; minute <= bid_back + 1; ++minute) {
    time = header.started + minute * md::kNanosPerMinute;
    const bool bid = minute < 2 || minute >= bid_back;
    sink.publish(md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, time, 10, 10.1, 20, 20});
    sink.publish(md::OptionQuote{1, time, bid ? 8.0 : 0.0, 8.1, bid ? 20.0 : 0.0, 20});
    sink.publish(md::SnapshotComplete{"SPX", time});
  }
  sink.close();
  return path;
}
TEST(Playbooks, ATimeStopWaitsForLiquidityWithoutJournalingRejections) {
  // B44: while a wing had no bid, the time stop's combo close was submitted and
  // rejected at every update, piling up rejected orders in the journal.
  test::RecordingFile file;
  const auto recording = no_bid_wing_day(file.directory, 6);
  auto setup = definition();
  setup["window"]["end"] = "09:31";
  setup["structure"]["template"]["target"] = {{"mode", "strike"}, {"value", 5000}};
  setup["structure"]["expiry"]["max"] = 1;
  setup["management"] = {{"close_by", "09:32"}, {"take_profit_percent", 50}};
  server::Playbooks store;
  store.change({{"action", "create"}, {"definition", setup}}, "main", true);
  store.change({{"action", "mode"}, {"id", "morning"}, {"mode", "auto"}}, "main", true);
  md::RecordingReader reader(recording);
  server::Desk::Options options; options.replay = true;
  options.initial_playbooks = store.catalogue().dump();
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (test)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto count = [&](auto predicate) {
    const auto orders = desk.trading_view()->snapshot->recent_orders.to_vector();
    return std::count_if(orders.begin(), orders.end(), predicate);
  };
  const auto closes = [](const trading::Order& order) { return order.request.client_order_id.starts_with("pb-close:"); };
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    if (desk.market_time() < md::new_york_to_utc({2026, 9, 16}, 9, 36)) { EXPECT_EQ(count(closes), 0); }
  }
  const auto view = desk.trading_view();
  EXPECT_EQ(count([](const trading::Order& order) { return order.status == trading::OrderStatus::Rejected; }), 0);
  ASSERT_EQ(count(closes), 1);
  // The exits were cancelled once, at the deadline, and the close filled when the wing was bid.
  for (const auto& order : view->snapshot->recent_orders) {
    if (order.parent) { EXPECT_EQ(order.status, trading::OrderStatus::Cancelled); }
    if (closes(order)) {
      EXPECT_EQ(order.status, trading::OrderStatus::Filled);
      EXPECT_EQ(order.accepted_at, md::new_york_to_utc({2026, 9, 16}, 9, 36));
    }
  }
  EXPECT_TRUE(view->snapshot->positions.empty());
}
TEST(Playbooks, AdherenceCountsASameSecondLossOnlyIfItClosedBeforeTheEntry) {
  // B45: an entry accepted while the losing strategy was still open broke no cooldown,
  // even when that strategy closed later in the same market second.
  test::ScriptedMarket first, second;
  second.contract = *md::parse_osi("SPXW261022C05010000");
  first.time = second.time = md::new_york_to_utc({2026, 9, 22}, 9, 40);
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession session(config, first.time);
  auto setup = definition(); setup["guardrails"]["max_entries_per_day"] = 10;
  server::Playbooks store; store.change({{"action", "create"}, {"definition", setup}}, "main", false);
  const auto report = [&] {
    server::TradingView view; view.snapshot = session.snapshot(); view.config = config; view.contracts = session.contracts();
    return server::playbook_report(store.catalogue(), view).at("morning").at("trades");
  };
  for (const bool close_first : {false, true}) {
    SCOPED_TRACE(close_first);
    first.seed(session); second.seed(session);
    auto entry = first.market("losing" + std::to_string(close_first)); entry.tags = {"playbook:morning@v1"};
    ASSERT_TRUE(session.submit(entry, first.time).decision.ok());
    first.time = second.time = first.time + 10 * md::kNanosPerMinute;
    ++first.observation; ++second.observation;
    session.on_quotes({first.quote("3.20", "3.40"), second.quote()}, {first.valuation(), second.valuation()}, first.time);
    auto next = second.market("next" + std::to_string(close_first)); next.tags = {"playbook:morning@v1"};
    const auto close = first.market("close" + std::to_string(close_first), 1, trading::Side::Sell);
    if (close_first) { ASSERT_TRUE(session.submit(close, first.time).decision.ok()); }
    ASSERT_TRUE(session.submit(next, first.time).decision.ok());
    if (!close_first) { ASSERT_TRUE(session.submit(close, first.time).decision.ok()); }
    const auto trades = report();
    const auto& losing = trades[trades.size() - 2];
    ASSERT_LT(trading::Money::parse(losing.at("net").get<std::string>()), trading::Money{});
    EXPECT_EQ(trades.back().at("rules").at("guardrails").get<bool>(), !close_first) << trades.dump();
    ASSERT_TRUE(session.submit(second.market("flat" + std::to_string(close_first), 1, trading::Side::Sell), first.time).decision.ok());
    first.time = second.time = first.time + 60 * md::kNanosPerMinute;
    ++first.observation; ++second.observation;
  }
}
}  // namespace
