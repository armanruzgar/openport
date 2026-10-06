#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <map>
#include <optional>

#include "openport/providers/replay_batches.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "support/recording.hpp"
#include "support/sanitizer.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;

/// Wednesday's regular and curb sessions, then Thursday's overnight session, in a quiet
/// market: SPY goes ex-dividend on Thursday.
constexpr const char* kCarry = R"({"id":"carry","title":"Carry","description":"A day, its curb and the night after.",
  "symbols":["SPX","SPY"],"date":"2026-09-16","seed":5,"generator":1,"volatility":0.02,"iv_shift":0,"spot_vol":-2,
  "sessions":[{"session":"regular","drift":[[1,0]]},{"session":"curb","drift":[[1,0]]},
    {"session":"overnight","drift":[[1,0]],"dividends":[{"symbol":"SPY","per_share":1.75}]}]})";

/// The Friday after Thanksgiving closes early; Sunday evening's overnight session leads
/// into Monday's regular session. SPX alone keeps the run quick.
constexpr const char* kWeekend = R"({"id":"weekend","title":"Weekend","description":"Friday, the night into Monday, and Monday.",
  "symbols":["SPX"],"date":"2026-11-27","seed":7,"generator":1,"volatility":0.05,"iv_shift":0,"spot_vol":-2,
  "sessions":[{"session":"regular","drift":[[1,0.002]]},{"session":"overnight","drift":[[1,-0.001]]},
    {"session":"regular","drift":[[1,0.001]],"events":[{"type":"gap","move":0.002}]}]})";

server::ApiResponse call(server::ReplayHost& host, std::string method, std::string target, const json& body = json::object()) {
  std::promise<server::ApiResponse> done;
  auto result = done.get_future();
  const auto payload = method == "DELETE" && body.empty() ? "" : body.dump();
  server::ApiRequest request{std::move(method), std::move(target), payload};
  request.content_type = "application/json";
  request.actor = "test-actor";
  if (!host.handle(request, [&](server::ApiResponse response) { done.set_value(std::move(response)); }))
    throw std::runtime_error("Replay request was not handled");
  if (result.wait_for(std::chrono::minutes(5)) != std::future_status::ready) throw std::runtime_error("Replay request timed out");
  return result.get();
}
server::TradingReply command(server::Desk& desk, server::TradingCommand request, md::Timestamp time, md::Timestamp driver) {
  std::optional<server::TradingReply> result;
  desk.command(std::move(request), [&](server::TradingReply reply) { result = std::move(reply); }, time, driver);
  if (!result) throw std::runtime_error("Desk did not complete command");
  return *result;
}
server::TradingCommand order(std::string id, const char* osi, trading::Side side, trading::Quantity quantity,
                             trading::OrderType type, trading::TimeInForce tif, std::optional<Money> limit = {}) {
  server::TradingCommand request;
  request.actor = "trader";
  request.order.client_order_id = std::move(id);
  request.order.symbol = md::parse_osi(osi)->osi_symbol();
  request.order.side = side;
  request.order.quantity = quantity;
  request.order.type = type;
  request.order.tif = tif;
  request.order.limit_price = limit;
  return request;
}
const trading::Order* find(const trading::TradingSnapshot& snapshot, const std::string& id) {
  for (const auto& o : snapshot.recent_orders) if (o.request.client_order_id == id) return &o;
  return nullptr;
}

TEST(MultiDayReplay, OneRunCarriesTheAccountThroughEverySessionAndItsJournalVerifies) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile file;
  const auto source = file.directory / "carry.json";
  {
    std::ofstream out(source);
    if (test::sanitizer_build()) {
      // Keep two calendar rollovers as well as regular/curb/overnight transitions
      // in the smaller recording; the assertions below still inspect Thursday.
      auto carry = json::parse(kCarry);
      for (int day = 0; day < 2; ++day)
        carry["sessions"].push_back({{"session", "regular"}, {"drift", {{1, 0}}}});
      out << carry;
    } else {
      out << kCarry;
    }
  }
  const auto scenario = providers::read_scenario(source);
  const auto windows = providers::scenario_windows(scenario, scenario.date);
  ASSERT_EQ(windows.size(), test::sanitizer_scale(3U, 5U));
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  const auto journal = file.directory / "run.jsonl";
  const auto* plan = server::find_plan("eod-100k");
  ASSERT_TRUE(plan);
  const md::Date wednesday{2026, 9, 16}, thursday{2026, 9, 17};
  const auto at = [](md::Date date, int hour, int minute) { return md::new_york_to_utc(date, hour, minute); };
  std::map<std::string, server::TradingReply> replies;
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.replay = true;
    options.run_input = server::scenario_input(scenario, scenario.date, scenario.seed);
    options.paper_journal = journal;
    options.paper.rules = plan->rules;
    options.paper.initial_cash = plan->initial_cash;
    options.dividends = {{"SPY", thursday, Money::parse("1.75")}};
    // European analytics are enough here; the American pass doubles the run's time.
    options.analytics.deamericanize = false;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    const auto send = [&](md::Timestamp time, md::Timestamp receipt, server::TradingCommand request) {
      replies[request.order.client_order_id] = command(desk, request, time, receipt);
    };
    // Resting limits inside the price band, away from the touch: SPX's ticks are $0.10 from $3.
    std::map<std::string, md::InstrumentId> ids;
    const auto away = [&](const char* osi, double factor) {
      const auto* option = desk.book().option(ids.at(md::parse_osi(osi)->osi_symbol()));
      const double price = option->mid() * factor;
      return Money::from_double((factor < 1 ? std::floor(price * 10) : std::ceil(price * 10)) / 10);
    };
    using enum trading::Side;
    using enum trading::OrderType;
    using enum trading::TimeInForce;
    // The run stops a few minutes into Thursday's overnight session: a stopped run
    // verifies through what it played.
    const auto stop = at(wednesday, 21, 1);
    std::optional<providers::ReplayBatch> pending;
    while (const auto batch = batches.next()) {
      if (batch->time > stop) {
        if (test::sanitizer_build()) pending = *batch;
        break;
      }
      for (const auto& event : batch->events)
        if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) ids[definition->contract.osi_symbol()] = definition->id;
      desk.replay_batch(batch->events, batch->received, batch->time);
      const auto time = desk.market_time();
      if (time == at(wednesday, 10, 0)) {
        send(time, batch->received, order("held", "SPXW260918C06000000", Buy, 1, Market, Ioc));
        // GTC orders carry from day to day; a DAY order ends with its session.
        send(time, batch->received, order("gtc", "SPX261016P05800000", Buy, 1, Limit, Gtc, away("SPX261016P05800000", 0.82)));
        send(time, batch->received, order("day", "SPX261016P05800000", Buy, 1, Limit, Day, away("SPX261016P05800000", 0.82)));
        // Calls with less time value than tomorrow's dividend can be assigned overnight.
        send(time, batch->received, order("short", "SPY260925C00580000", Sell, 5, Market, Ioc));
      } else if (time == at(wednesday, 16, 30)) {
        // The curb takes plain limit orders, a DAY one lasting until 17:00.
        send(time, batch->received, order("curb-market", "SPXW260918C06000000", Sell, 1, Market, Ioc));
        send(time, batch->received, order("curb-limit", "SPX261016P05800000", Buy, 1, Limit, Day, away("SPX261016P05800000", 0.82)));
      } else if (time == at(wednesday, 21, 0)) {
        // Thursday's overnight session, after the rollover: a DAY limit lasts until 09:25.
        send(time, batch->received, order("overnight", "SPXW260918C06000000", Sell, 1, Limit, Day, away("SPXW260918C06000000", 1.15)));
      }
    }
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    const auto view = desk.trading_view();
    ASSERT_TRUE(view);
    const auto& snapshot = *view->snapshot;
    for (const auto* id : {"held", "gtc", "day", "short", "curb-limit", "overnight"}) {
      ASSERT_TRUE(replies.contains(id)) << id;
      EXPECT_TRUE(replies.at(id).decision.ok()) << id << ": " << replies.at(id).decision.message;
    }
    EXPECT_EQ(replies.at("curb-market").decision.code, trading::Reason::LIMIT_ONLY);
    // The day rolled over once, at Thursday's first (overnight) batch, on Wednesday's
    // marks, and the end-of-day drawdown floor ratcheted from Wednesday's close.
    ASSERT_EQ(snapshot.evaluation.days.size(), 1U);
    const auto& closed = snapshot.evaluation.days.front();
    EXPECT_EQ(closed.day, wednesday);
    EXPECT_EQ(snapshot.evaluation.day, thursday);
    EXPECT_EQ(closed.peak, std::max(plan->initial_cash, closed.close_equity));
    EXPECT_EQ(closed.floor, closed.peak - plan->rules.max_drawdown);
    EXPECT_EQ(snapshot.evaluation.floor, closed.floor);
    const auto* gtc = find(snapshot, "gtc");
    const auto* day = find(snapshot, "day");
    const auto* curb = find(snapshot, "curb-limit");
    const auto* overnight = find(snapshot, "overnight");
    ASSERT_TRUE(gtc && day && curb && overnight);
    EXPECT_EQ(gtc->status, trading::OrderStatus::Working);
    EXPECT_EQ(day->status, trading::OrderStatus::Cancelled);
    EXPECT_EQ(day->day_end, at(wednesday, 16, 15));
    EXPECT_EQ(curb->status, trading::OrderStatus::Cancelled);
    EXPECT_EQ(curb->day_end, at(wednesday, 17, 0));
    EXPECT_EQ(overnight->status, trading::OrderStatus::Working);
    EXPECT_EQ(overnight->day_end, at(thursday, 9, 25));
    // The call bought on Wednesday is still held on Thursday.
    EXPECT_TRUE(std::any_of(snapshot.positions.begin(), snapshot.positions.end(), [](const auto& p) {
      return p.position.contract.osi_symbol() == md::parse_osi("SPXW260918C06000000")->osi_symbol() && p.position.quantity == 1;
    }));
    // Some short calls were assigned at rollover, delivering short shares held
    // into SPY's ex-date, which owe its dividend.
    ASSERT_EQ(snapshot.stocks.size(), 1U);
    const auto shares = snapshot.stocks.front().position.shares;
    EXPECT_LT(shares, 0);
    EXPECT_EQ(shares % 100, 0);
    ASSERT_EQ(snapshot.dividends.size(), 1U);
    EXPECT_EQ(snapshot.dividends.front().ex_date, thursday);
    EXPECT_EQ(snapshot.dividends.front().shares, shares);
    EXPECT_EQ(snapshot.dividends.front().amount, Money::parse("1.75") * shares);
    if (test::sanitizer_build()) {
      ASSERT_TRUE(pending);
      desk.replay_batch(pending->events, pending->received, pending->time);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (batch->time >= windows.back().first) break;
      }
      EXPECT_EQ(desk.trading_view()->snapshot->evaluation.day, (md::Date{2026, 9, 18}));
      EXPECT_EQ(desk.trading_view()->snapshot->evaluation.days.size(), 2U);
    }
    desk.stop();
  }
  // The first batch starts Wednesday; Thursday's overnight session rolls it over.
  std::vector<std::pair<json, md::Timestamp>> rollovers;
  for (const auto& record : trading::FileJournal::read(journal.string()).records) {
    if (record.type != "day_rollover") continue;
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events"))
      if (event.at("type") == "day_rollover") rollovers.emplace_back(event.at("payload").at("day"), record.time);
  }
  ASSERT_EQ(rollovers.size(), test::sanitizer_scale(2U, 3U));
  EXPECT_EQ(rollovers[0].first, json({{"year", 2026}, {"month", 9}, {"day", 16}}));
  EXPECT_EQ(rollovers[1].first, json({{"year", 2026}, {"month", 9}, {"day", 17}}));
  EXPECT_EQ(rollovers[1].second, windows[2].first);
  if (test::sanitizer_build()) {
    EXPECT_EQ(rollovers[2].first, json({{"year", 2026}, {"month", 9}, {"day", 18}}));
    EXPECT_EQ(rollovers[2].second, windows[4].first);
  }
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
}

TEST(MultiDayReplay, StartsAndStepsToADateAndTimeInTheRunsSessions) {
  test::RecordingFile file;
  const auto scenarios = file.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  { std::ofstream out(scenarios / "weekend.json"); out << kWeekend; }
  server::Engine::Options base;
  base.analytics_interval = std::chrono::milliseconds(1);
  base.paper_journal = file.directory / "paper.jsonl";
  server::ReplayHost host({file.directory, base, true, scenarios});
  const md::Date friday{2026, 11, 27}, sunday{2026, 11, 29}, monday{2026, 11, 30};
  const auto at = [](md::Date date, int hour, int minute) { return md::format_timestamp(md::new_york_to_utc(date, hour, minute)); };
  const auto listing = json::parse(call(host, "GET", "/api/replay").body);
  const auto demo = std::find_if(listing["demos"].begin(), listing["demos"].end(), [](const json& d) { return d["id"] == "weekend"; });
  ASSERT_NE(demo, listing["demos"].end());
  const auto sessions = json::array({
      {{"session", "regular"}, {"date", "2026-11-27"}, {"open", at(friday, 9, 30)}, {"end", at(friday, 13, 15)}},
      {{"session", "overnight"}, {"date", "2026-11-30"}, {"open", at(sunday, 20, 15)}, {"end", at(monday, 9, 25)}},
      {{"session", "regular"}, {"date", "2026-11-30"}, {"open", at(monday, 9, 30)}, {"end", at(monday, 16, 15)}}});
  EXPECT_EQ((*demo)["sessions"], sessions);
  EXPECT_EQ((*demo)["end"], at(monday, 16, 15));
  EXPECT_EQ((*demo)["session"], "regular");
  // A bare time in none of the sessions, and a date and time after the run, are refused.
  const auto bare = call(host, "POST", "/api/replay", {{"scenario", "weekend"}, {"start_at", "17:30"}});
  EXPECT_EQ(bare.status, 400);
  EXPECT_EQ(json::parse(bare.body)["error"]["message"],
      "start_at 17:30 falls in none of this run's sessions at or after 2026-11-27T14:30:00.000Z; give a date and time");
  for (const auto& [time, next] : std::vector<std::pair<std::string, std::string>>{
           {"2026-11-27T18:00", "2026-11-29T20:15"},
           {"2026-11-29T09:00", "2026-11-29T20:15"},
           {"2026-11-30T09:27", "2026-11-30T09:30"},
           {"2026-11-30T14:27:00Z", "2026-11-30T09:30"}}) {
    SCOPED_TRACE(time);
    const auto refused = call(host, "POST", "/api/replay",
        {{"scenario", "weekend"}, {"seed", "5"}, {"paused", true}, {"start_at", time}});
    ASSERT_EQ(refused.status, 400) << refused.body;
    EXPECT_EQ(json::parse(refused.body)["error"]["code"], "INVALID_REQUEST");
    EXPECT_EQ(json::parse(refused.body)["error"]["message"],
        "start_at " + time + " falls between this run's sessions; the next session opens at " + next + " ET");
    const auto state = json::parse(call(host, "GET", "/api/replay").body);
    EXPECT_TRUE(state["replay"].is_null());
    EXPECT_TRUE(state["history"].empty());
    if (std::filesystem::exists(file.directory / "replays")) {
      EXPECT_TRUE(std::filesystem::is_empty(file.directory / "replays"));
    }
  }
  EXPECT_EQ(call(host, "POST", "/api/replay", {{"scenario", "weekend"}, {"start_at", "2026-12-01T09:30"}}).status, 400);
  // A bare start_at is its first occurrence in the run: Friday afternoon.
  const auto started = call(host, "POST", "/api/replay",
      {{"scenario", "weekend"}, {"seed", "scenario"}, {"plan", "eod-50k"}, {"paused", true}, {"start_at", "13:00"}});
  ASSERT_EQ(started.status, 201) << started.body;
  EXPECT_EQ(json::parse(started.body)["replay"]["sessions"], sessions);
  EXPECT_EQ(json::parse(started.body)["replay"]["end"], at(monday, 16, 15));
  const auto replay = [&] { return json::parse(call(host, "GET", "/api/replay").body)["replay"]; };
  ASSERT_TRUE(test::recording_eventually([&] { return !replay()["fast_forwarding"].get<bool>(); }));
  EXPECT_EQ(replay()["time"], at(friday, 13, 0));
  const auto bare_until = call(host, "PUT", "/api/replay", {{"until", "17:30"}});
  EXPECT_EQ(bare_until.status, 400);
  EXPECT_EQ(json::parse(bare_until.body)["error"]["message"],
      "until 17:30 falls in none of this run's sessions at or after 2026-11-27T18:00:00.000Z; give a date and time");
  // A bare until at the current time stays on Friday rather than skipping to Monday.
  const auto same_time = call(host, "PUT", "/api/replay", {{"until", "13:00"}});
  ASSERT_EQ(same_time.status, 200) << same_time.body;
  EXPECT_EQ(json::parse(same_time.body)["settled_through"], at(friday, 13, 0));
  EXPECT_EQ(replay()["time"], at(friday, 13, 0));
  auto risk = json::parse(call(host, "GET", "/api/replay/risk").body);
  risk["limits"]["aggregate"]["dollar_delta"] = 2'000'000;
  const auto pending = call(host, "PUT", "/api/replay/risk/limits",
      {{"expected_revision", risk["limits_revision"]}, {"limits", risk["limits"]}});
  ASSERT_EQ(pending.status, 200) << pending.body;
  EXPECT_EQ(json::parse(pending.body)["pending_requires_reset"], false);
  EXPECT_FALSE(json::parse(pending.body)["pending_limits"].is_null());
  // In a run of several sessions a bare until is the time's next occurrence: Sunday evening.
  const auto evening = call(host, "PUT", "/api/replay", {{"until", "21:00"}});
  ASSERT_EQ(evening.status, 200) << evening.body;
  EXPECT_EQ(json::parse(evening.body)["settled_through"], at(sunday, 21, 0));
  EXPECT_EQ(call(host, "PUT", "/api/replay", {{"until", "17:30"}}).status, 400);
  EXPECT_EQ(call(host, "PUT", "/api/replay", {{"until", "2026-12-01T10:00"}}).status, 400);
  // A date and time is New York's, seconds optional.
  const auto open = call(host, "PUT", "/api/replay", {{"until", "2026-11-30T09:30"}});
  ASSERT_EQ(open.status, 200) << open.body;
  EXPECT_EQ(json::parse(open.body)["settled_through"], at(monday, 9, 30));
  // The account carried over the weekend: Friday closed and Monday is under way.
  const auto account = json::parse(call(host, "GET", "/api/replay/account").body);
  EXPECT_EQ(account["evaluation"]["day"], "2026-11-30");
  risk = json::parse(call(host, "GET", "/api/replay/risk").body);
  EXPECT_TRUE(risk["pending_limits"].is_null());
  EXPECT_EQ(risk["limits"]["aggregate"]["dollar_delta"], 2'000'000);
  risk["limits"]["aggregate"]["dollar_delta"] = 3'000'000;
  const auto last_day = call(host, "PUT", "/api/replay/risk/limits",
      {{"expected_revision", risk["limits_revision"]}, {"limits", risk["limits"]}});
  ASSERT_EQ(last_day.status, 200) << last_day.body;
  EXPECT_EQ(json::parse(last_day.body)["pending_requires_reset"], true);
  ASSERT_EQ(account["evaluation"]["days"].size(), 1U);
  EXPECT_EQ(account["evaluation"]["days"][0]["day"], "2026-11-27");
  host.stop();
  // The finished run keeps its sessions in history.
  const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0]["sessions"], sessions);
  // Dated starts include both boundaries and the interior of a later session.
  server::ReplayHost later_host({file.directory, base, true, scenarios});
  const auto later_replay = [&] { return json::parse(call(later_host, "GET", "/api/replay").body)["replay"]; };
  for (const auto time : {"2026-11-30T09:30", "2026-11-30T10:00", "2026-11-30T16:15"}) {
    SCOPED_TRACE(time);
    const auto later = call(later_host, "POST", "/api/replay",
        {{"scenario", "weekend"}, {"seed", "scenario"}, {"paused", true}, {"start_at", time}});
    ASSERT_EQ(later.status, 201) << later.body;
    ASSERT_TRUE(test::recording_eventually([&] { return !later_replay()["fast_forwarding"].get<bool>(); }));
    EXPECT_EQ(later_replay()["time"], md::format_timestamp(*md::parse_datetime(std::string(time) + ":00", md::Zone::NewYork)));
  }
}

TEST(MultiDayReplay, OpeningVolumeAdmitsCappedOrdersAndResetsOnTheNextDate) {
  test::RecordingFile file;
  const auto source = file.directory / "volume.json";
  { std::ofstream out(source); out << R"({"id":"volume","title":"Volume","description":"Two quiet opens.",
    "symbols":["SPX"],"date":"2026-09-16","seed":5,"generator":1,"volatility":0,"iv_shift":0,"spot_vol":0,
    "sessions":[{"session":"regular","drift":[[1,0]]},{"session":"regular","drift":[[1,0]]}]})"; }
  const auto scenario = providers::read_scenario(source);
  const auto windows = providers::scenario_windows(scenario, scenario.date);
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  md::RecordingReader reader(file.path);
  server::Desk::Options options;
  options.replay = true;
  options.paper.rules.max_volume_percent = 10;
  options.paper.limits.aggregate = {1e9, 1e9};
  options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription);
  const auto symbol = md::parse_osi("SPXW260917C06000000")->osi_symbol();
  std::optional<md::InstrumentId> id;
  double previous = 0, opening = 0;
  bool checked = false;
  while (const auto batch = batches.next()) {
    for (const auto& event : batch->events) {
      if (const auto* definition = std::get_if<md::ContractDefinition>(&event);
          definition && definition->contract.osi_symbol() == symbol) id = definition->id;
      if (const auto* volume = std::get_if<md::OptionVolume>(&event); volume && id && volume->id == *id) {
        if (batch->time == windows[1].first) {
          EXPECT_GE(volume->contracts, 10);
          EXPECT_LT(volume->contracts, previous);
          checked = true;
        } else previous = volume->contracts;
        if (batch->time == windows[0].first) opening = volume->contracts;
      }
    }
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (batch->time == windows[0].first) {
      ASSERT_GE(opening, 10);
      auto entry = order("opening", symbol.c_str(), trading::Side::Buy, 1,
                         trading::OrderType::Market, trading::TimeInForce::Ioc);
      const auto accepted = command(desk, entry, batch->time, batch->received);
      ASSERT_TRUE(accepted.decision.ok()) << accepted.decision.message;
      ASSERT_EQ(desk.trading_view()->snapshot->recent_fills.size(), 1U);
      // A positive opening volume still enforces the cap.
      entry.order.client_order_id = "too-large";
      entry.order.quantity = static_cast<trading::Quantity>(opening);
      EXPECT_EQ(command(desk, entry, batch->time, batch->received).decision.code, trading::Reason::MAX_VOLUME_SHARE);
    }
    if (batch->time == windows[1].first) break;
  }
  EXPECT_TRUE(checked);
}

TEST(MultiDayReplay, EvaluationDeadlineFailsAfterItsLastDateAndRestartReproducesIt) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile file;
  const auto scenarios = file.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  { std::ofstream out(scenarios / "deadline.json"); out << R"({"id":"deadline","title":"Deadline","description":"Three quiet days.",
    "symbols":["SPX"],"date":"2026-09-16","seed":5,"generator":1,"volatility":0,"iv_shift":0,"spot_vol":0,
    "sessions":[{"session":"regular","drift":[[1,0]]},{"session":"regular","drift":[[1,0]]},
      {"session":"regular","drift":[[1,0]]}]})"; }
  server::Engine::Options base;
  base.paper_journal = file.directory / "paper.jsonl";
  base.paper.limits.price_band_absolute = Money::parse("100.00");  // keep the tiny GTC limit resting
  server::ReplayHost host({file.directory, base, true, scenarios});
  const auto start = call(host, "POST", "/api/replay",
      {{"scenario", "deadline"}, {"seed", "scenario"}, {"plan", "eod-100k"}, {"paused", true}});
  ASSERT_EQ(start.status, 201) << start.body;
  const auto id = json::parse(start.body).at("replay").at("id").get<std::string>();
  const auto ready = [&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); };
  ASSERT_TRUE(test::recording_eventually(ready));
  const auto account = [&] { return json::parse(call(host, "GET", "/api/replay/account").body); };
  auto rules = account().at("rules");
  rules["plan"] = "Deadline rehearsal";
  rules["time_limit_days"] = 1;
  const auto reset = call(host, "POST", "/api/replay/account/reset",
      {{"initial_cash", "100000"}, {"rules", rules}, {"reason", "rehearse the evaluation deadline"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  EXPECT_EQ(account().at("evaluation").at("deadline"), "2026-09-17");
  EXPECT_EQ(account().at("evaluation").at("days_left"), 1);
  const json entry{{"client_order_id", "held"}, {"symbol", "SPXW  260918C06000000"},
      {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
  const auto held = call(host, "POST", "/api/replay/orders", entry);
  ASSERT_EQ(held.status, 201) << held.body;
  auto resting = entry;
  resting.update({{"client_order_id", "resting"}, {"type", "limit"}, {"limit_price", "0.05"}, {"time_in_force", "gtc"}});
  const auto pending = call(host, "POST", "/api/replay/orders", resting);
  ASSERT_EQ(pending.status, 201) << pending.body;
  const auto pending_id = json::parse(pending.body).at("order").at("id").get<std::string>();
  const auto last_date = call(host, "PUT", "/api/replay", {{"until", "2026-09-17T16:00"}});
  ASSERT_EQ(last_date.status, 200) << last_date.body;
  EXPECT_EQ(account().at("evaluation").at("status"), "active");
  EXPECT_EQ(account().at("evaluation").at("days_left"), 0);
  const auto fail = [&] { return call(host, "PUT", "/api/replay", {{"until", "2026-09-18T09:30"}}); };
  const auto crossed = fail();
  ASSERT_EQ(crossed.status, 200) << crossed.body;
  const auto evaluation = account().at("evaluation");
  EXPECT_EQ(evaluation.at("status"), "failed");
  EXPECT_EQ(evaluation.at("decision_code"), "TIME_LIMIT");
  EXPECT_EQ(evaluation.at("deadline"), "2026-09-17");
  EXPECT_EQ(evaluation.at("decided_at"), md::format_timestamp(md::new_york_to_utc({2026, 9, 18}, 9, 30)));
  const auto portfolio = json::parse(call(host, "GET", "/api/replay/portfolio").body);
  EXPECT_TRUE(portfolio.at("positions").empty());
  const auto cancelled = json::parse(call(host, "GET", "/api/replay/orders/" + pending_id).body);
  EXPECT_EQ(cancelled.at("order").at("status"), "cancelled");
  auto late = entry; late["client_order_id"] = "late";
  const auto refused = call(host, "POST", "/api/replay/orders", late);
  ASSERT_EQ(refused.status, 422) << refused.body;
  EXPECT_EQ(json::parse(refused.body).at("error").at("code"), "EVALUATION_CLOSED");

  // The reset, held position and final active date are restored from the saved
  // inputs; crossing the deadline again must reproduce the same frozen decision.
  const auto restart = call(host, "POST", "/api/replay", {{"restart", id}, {"at", "2026-09-17T16:00"}});
  ASSERT_EQ(restart.status, 201) << restart.body;
  ASSERT_TRUE(test::recording_eventually(ready));
  EXPECT_EQ(account().at("evaluation").at("status"), "active");
  ASSERT_EQ(fail().status, 200);
  EXPECT_EQ(account().at("evaluation"), evaluation);
  const auto restarted = json::parse(restart.body).at("replay").at("id").get<std::string>();
  host.stop();
  for (const auto& run : {id, restarted}) {
    const auto verified = server::verify_run(file.directory / "replays" / (run + ".jsonl"));
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

class ScenarioCommandRestart : public testing::TestWithParam<int> {};

TEST_P(ScenarioCommandRestart, InclusiveBoundaryAndGapCommandsRestartAndResumeByteExactly) {
  test::RecordingFile file;
  const auto scenarios = file.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  { std::ofstream out(scenarios / "seconds.json"); out << R"({"id":"seconds","title":"Seconds","description":"Command clock fixture.",
      "symbols":["SPX"],"date":"2026-09-16","seed":19,"generator":1,"session":"regular",
      "volatility":0.02,"iv_shift":0,"spot_vol":-2,"drift":[[1,0]]})"; }
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.analytics.deamericanize = false;
  // Keep the tiny limits resting while allowing both price and quantity changes.
  options.paper.limits.price_band_absolute = Money::parse("100.00");
  options.paper.limits.per_underlying.dollar_delta = 10000000;
  options.paper.limits.aggregate.dollar_delta = 10000000;
  server::ReplayHost host({file.directory, options, true, scenarios});
  const auto started = call(host, "POST", "/api/replay", {{"scenario", "seconds"}, {"seed", "scenario"},
      {"plan", "practice"}, {"paused", true}, {"speed", 0}});
  ASSERT_EQ(started.status, 201) << started.body;
  const auto metadata = json::parse(started.body).at("replay");
  const auto id = metadata.at("id").get<std::string>();
  const auto source = file.directory / "replays" / (id + ".jsonl");
  const auto ready = [](server::ReplayHost& replay) {
    return !json::parse(replay.tick()).at("replay").at("fast_forwarding").get<bool>();
  };
  ASSERT_TRUE(test::recording_eventually([&] { return ready(host); }));
  const auto bytes = [](const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
  };
  const auto submit = [&](server::ReplayHost& replay, const std::string& client) {
    return call(replay, "POST", "/api/replay/orders", {{"client_order_id", client}, {"symbol", "SPXW  260916C06000000"},
        {"side", "buy"}, {"type", "limit"}, {"limit_price", "0.05"}, {"quantity", 1}, {"time_in_force", "gtc"}});
  };
  std::vector<std::string> ids;
  for (int index = 0; index < 3; ++index) {
    const auto placed = submit(host, "resting-" + std::to_string(index));
    ASSERT_EQ(placed.status, 201) << placed.body;
    ids.push_back(json::parse(placed.body).at("order").at("id").get<std::string>());
  }
  std::vector<std::string> prefixes;
  std::vector<json> books;
  const auto boundary = md::new_york_to_utc({2026, 9, 16}, 9, 30, 30);
  for (int phase = 0; phase < 2; ++phase) {
    const auto at = phase == 0 ? "09:30:30" : "09:30:32";
    ASSERT_EQ(call(host, "PUT", "/api/replay", {{"until", at}}).status, 200);
    const auto before = trading::FileJournal::read(source.string()).records.size();
    // Rotate the first command: input-first timestamps are not specific to modify.
    for (int index = 0; index < 3; ++index) {
      const auto kind = (GetParam() + index) % 3;
      server::ApiResponse response;
      if (kind == 0) {
        response = call(host, "PUT", "/api/replay/orders/" + ids[0],
            {{"limit_price", phase == 0 ? "0.10" : "0.15"}, {"quantity", phase + 2}});
      } else if (kind == 1) {
        response = submit(host, "at-" + std::to_string(phase));
      } else {
        response = call(host, "DELETE", "/api/replay/orders/" + ids[static_cast<std::size_t>(phase + 1)]);
      }
      ASSERT_EQ(response.status, kind == 1 ? 201 : 200) << response.body;
    }
    ASSERT_EQ(call(host, "PUT", "/api/replay", {{"paused", true}}).status, 200);
    const auto recovery = trading::FileJournal::read(source.string());
    ASSERT_GT(recovery.records.size(), before);
    EXPECT_EQ(recovery.records[before].time, boundary);
    const auto inputs = server::run_inputs(recovery);
    ASSERT_GE(inputs.size(), 3U);
    for (std::size_t index = inputs.size() - 3; index < inputs.size(); ++index) {
      const auto input = json::parse(inputs[index]);
      EXPECT_EQ(input.at("kind"), "command");
      EXPECT_EQ(input.at("time"), boundary + phase * 2 * md::kNanosPerSecond);
    }
    prefixes.push_back(bytes(source));
    books.push_back(json::parse(call(host, "GET", "/api/replay/orders").body).at("orders"));
  }
  // Preserve the gap-time commands as an interrupted run, with its original metadata.
  const auto crashed = file.directory / "crashed";
  std::filesystem::create_directories(crashed / "replays");
  const auto resumed_journal = crashed / "replays" / (id + ".jsonl");
  std::filesystem::copy_file(source, resumed_journal);
  { std::ofstream out(crashed / "replays" / (id + ".json")); out << metadata; }
  ASSERT_EQ(call(host, "PUT", "/api/replay", {{"until", "09:30:45"}}).status, 200);
  ASSERT_EQ(submit(host, "after-target").status, 201);
  ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  const auto original = bytes(source);
  for (const auto* at : {"09:30:30", "09:30:31", "09:30:32"}) {
    SCOPED_TRACE(at);
    const auto restarted = call(host, "POST", "/api/replay", {{"restart", id}, {"at", at}});
    ASSERT_EQ(restarted.status, 201) << restarted.body;
    const auto replay = json::parse(restarted.body).at("replay");
    const auto journal = file.directory / "replays" / (replay.at("id").get<std::string>() + ".jsonl");
    const auto phase = std::string(at) == "09:30:32" ? 1U : 0U;
    EXPECT_EQ(bytes(journal), prefixes[phase]);
    EXPECT_EQ(json::parse(call(host, "GET", "/api/replay/orders").body).at("orders"), books[phase]);
    EXPECT_EQ(bytes(source), original);
    ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
  options.paper_journal = crashed / "paper.jsonl";
  server::ReplayHost resumed({file.directory, options, true, scenarios});
  const auto response = call(resumed, "POST", "/api/replay", {{"resume", id}, {"speed", 0}});
  ASSERT_EQ(response.status, 201) << response.body;
  ASSERT_TRUE(test::recording_eventually([&] { return ready(resumed); }));
  EXPECT_EQ(bytes(resumed_journal), prefixes.back());
  EXPECT_EQ(json::parse(call(resumed, "GET", "/api/replay/orders").body).at("orders"), books.back());
  ASSERT_EQ(call(resumed, "PUT", "/api/replay", {{"until", "09:30:45"}}).status, 200);
  ASSERT_EQ(submit(resumed, "after-target").status, 201);
  ASSERT_EQ(call(resumed, "DELETE", "/api/replay").status, 200);
  EXPECT_EQ(bytes(resumed_journal), original);
  const auto verified = server::verify_run(resumed_journal);
  EXPECT_TRUE(verified.matched) << verified.message;
}

INSTANTIATE_TEST_SUITE_P(ModifySubmitCancelFirst, ScenarioCommandRestart, testing::Values(0, 1, 2));

TEST(MultiDayReplay, ScenarioSecondsAndRestartKeepTheSeedPlanSettingsAndCommandPrefix) {
  test::RecordingFile file;
  const auto scenarios = file.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  { std::ofstream out(scenarios / "seconds.json"); out << R"({"id":"seconds","title":"Seconds","description":"Small replay control fixture.",
      "symbols":["SPX"],"date":"2026-09-16","seed":19,"generator":1,"session":"regular",
      "volatility":0.02,"iv_shift":0,"spot_vol":-2,"drift":[[1,0]]})"; }
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.analytics.deamericanize = false;
  options.paper.limits.max_order_contracts = 7;
  server::ReplayHost host({file.directory, options, true, scenarios});
  const auto ready = [&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); };
  const auto at = [](int minute, int second) { return md::format_timestamp(md::new_york_to_utc({2026, 9, 16}, 9, minute, second)); };
  std::string id;
  for (const auto* start : {"09:30:01", "2026-09-16T09:30:01"}) {
    const auto started = call(host, "POST", "/api/replay", {{"scenario", "seconds"}, {"start_at", start}, {"seed", "scenario"}, {"plan", "intraday-25k"}, {"paused", true}});
    ASSERT_EQ(started.status, 201) << started.body;
    id = json::parse(started.body).at("replay").at("id").get<std::string>();
    ASSERT_TRUE(test::recording_eventually(ready));
    EXPECT_EQ(json::parse(host.tick()).at("replay").at("time"), at(30, 15));
  }
  ASSERT_EQ(call(host, "PUT", "/api/replay", {{"until", "next"}}).status, 200);
  EXPECT_EQ(json::parse(host.tick()).at("replay").at("settled_through"), at(30, 30));
  const auto buy = call(host, "POST", "/api/replay/orders", {{"client_order_id", "scenario-prefix"}, {"symbol", "SPXW  260916C06000000"},
      {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
  ASSERT_EQ(buy.status, 201) << buy.body;
  ASSERT_EQ(call(host, "PUT", "/api/replay", {{"paused", true}}).status, 200);
  const auto source = file.directory / "replays" / (id + ".jsonl");
  const auto prefix = trading::FileJournal::read(source.string());
  ASSERT_EQ(call(host, "PUT", "/api/replay", {{"until", "+1m"}}).status, 200);
  EXPECT_EQ(json::parse(host.tick()).at("replay").at("settled_through"), at(31, 30));
  const auto restart = call(host, "POST", "/api/replay", {{"restart", id}, {"at", "09:30:30"}});
  ASSERT_EQ(restart.status, 201) << restart.body;
  const auto state = json::parse(restart.body).at("replay");
  EXPECT_EQ(state.at("seed"), "19");
  EXPECT_EQ(state.at("scenario"), "seconds");
  EXPECT_EQ(state.at("plan"), "intraday-25k");
  EXPECT_EQ(state.at("settled_through"), at(30, 30));
  const auto journal = file.directory / "replays" / (state.at("id").get<std::string>() + ".jsonl");
  const auto recovered = trading::FileJournal::read(journal.string());
  EXPECT_EQ(recovered.head, prefix.head);
  EXPECT_EQ(recovered.records.size(), prefix.records.size());
  EXPECT_EQ(json::parse(call(host, "GET", "/api/replay/risk").body).at("limits").at("max_order_contracts"), 7);
  ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
  // A supported older generator revision on driver 6 regenerates at its saved
  // revision, even when this host has already cached the current revision.
  const auto scenario = providers::read_scenario(scenarios / "seconds.json");
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed, 1);
  const auto legacy_journal = file.directory / "replays" / "revision-one.jsonl";
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options legacy;
    legacy.replay = true;
    legacy.paper_journal = legacy_journal;
    legacy.analytics.deamericanize = false;
    auto identity = json::parse(server::scenario_input(scenario, scenario.date, scenario.seed));
    identity["revision"] = 1;
    legacy.run_input = identity.dump();
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, legacy);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    while (const auto batch = batches.next()) {
      if (batch->time > md::new_york_to_utc(scenario.date, 9, 30, 30)) break;
      desk.replay_batch(batch->events, batch->received, batch->time);
    }
    desk.stop();
  }
  { std::ofstream sidecar(file.directory / "replays" / "revision-one.json");
    sidecar << json{{"id", "revision-one"}, {"file", "Seconds"}, {"date", "2026-09-16"}, {"finished", true},
        {"start_at", ""}, {"plan", "practice"}, {"settled_through", at(30, 30)}}; }
  const auto legacy_restart = call(host, "POST", "/api/replay", {{"restart", "revision-one"}, {"at", "09:30:30"}});
  ASSERT_EQ(legacy_restart.status, 201) << legacy_restart.body;
  const auto legacy_id = json::parse(legacy_restart.body).at("replay").at("id").get<std::string>();
  const auto copied = file.directory / "replays" / (legacy_id + ".jsonl");
  EXPECT_EQ(trading::FileJournal::read(legacy_journal.string()).head, trading::FileJournal::read(copied.string()).head);
  ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  EXPECT_TRUE(server::verify_run(copied).matched);
  { std::ofstream changed(scenarios / "seconds.json", std::ios::app); changed << "\n"; }
  const auto changed = call(host, "POST", "/api/replay", {{"restart", id}, {"at", "09:30:30"}});
  EXPECT_EQ(changed.status, 409) << changed.body;
  EXPECT_EQ(json::parse(changed.body).at("error").at("code"), "REPLAY_NOT_RESTARTABLE");
}

}  // namespace

TEST(MultiDayReplay, WaterfallStartsAndStepsOnAnEarlyCloseDate) {
  test::RecordingFile file;
  server::Engine::Options base;
  base.analytics_interval = std::chrono::milliseconds(1);
  base.paper_journal = file.directory / "paper.jsonl";
  server::ReplayHost host({file.directory, base, true});
  const auto started = call(host, "POST", "/api/replay", {{"scenario", "afternoon-waterfall"},
      {"date", "2026-11-27"}, {"seed", "17"}, {"paused", true}, {"start_at", "12:00"}});
  ASSERT_EQ(started.status, 201) << started.body;
  ASSERT_TRUE(test::recording_eventually([&] {
    return !json::parse(call(host, "GET", "/api/replay").body)["replay"]["fast_forwarding"].get<bool>();
  }));
  const auto step = call(host, "PUT", "/api/replay", {{"until", "12:30"}});
  ASSERT_EQ(step.status, 200) << step.body;
  EXPECT_EQ(json::parse(step.body)["settled_through"], "2026-11-27T17:30:00.000Z");
  EXPECT_EQ(call(host, "DELETE", "/api/replay").status, 200);
}
