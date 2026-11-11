#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/notifications.hpp"
#include "support/contract_capture.hpp"

namespace {
using namespace openport;
using nlohmann::json;

std::shared_ptr<server::Notifications> stream(std::size_t history = server::kNotificationHistory) {
  server::Notifications::Options options;
  options.worker = false;
  options.history = history;
  options.wall_clock = [] { return md::new_york_to_utc({2026, 9, 22}, 10, 0); };
  return std::make_shared<server::Notifications>(server::NotificationConfig{}, nullptr, options);
}
server::NotificationEvent event(std::string kind, std::string account, std::string message, json details = json::object()) {
  return {std::move(kind), std::move(account), md::new_york_to_utc({2026, 9, 22}, 9, 45), std::move(message), std::move(details)};
}
std::vector<std::string> messages(const json& events) {
  std::vector<std::string> out;
  for (const auto& item : events) out.push_back(item.at("message").get<std::string>());
  return out;
}

// G-139: the stream keeps what it published, so a terminal opened later can read it.
TEST(NotificationHistory, KeepsEachAccountsRecentEventsBesideTheFeedsOwn) {
  const auto events = stream(3);
  for (const auto* text : {"fill 1", "fill 2", "fill 3", "fill 4"}) events->publish(event("fill", "main", text));
  // Order transitions are frequent and /api/orders keeps them: they are not kept.
  events->publish(event("order_state", "main", "order 1 changed"));
  auto kept = events->history("main", "live", 0, server::kNotificationHistory);
  EXPECT_EQ(messages(kept.events), (std::vector<std::string>{"fill 2", "fill 3", "fill 4"}));
  EXPECT_EQ(kept.sequence, 5U);
  const auto& first = kept.events.at(0);
  EXPECT_EQ(first.at("type"), "event");
  EXPECT_EQ(first.at("version"), 1);
  EXPECT_EQ(first.at("sequence"), "2");
  EXPECT_EQ(first.at("event"), "fill");
  EXPECT_EQ(first.at("account"), "main");
  EXPECT_EQ(first.at("source"), "live");
  EXPECT_EQ(first.at("run_id"), nullptr);
  EXPECT_EQ(first.at("market_time"), md::format_timestamp(md::new_york_to_utc({2026, 9, 22}, 9, 45)));
  EXPECT_EQ(first.at("received"), md::format_timestamp(md::new_york_to_utc({2026, 9, 22}, 10, 0)));

  // The feed's own events belong to every account on its source, in publication order.
  events->publish(event("feed_stalled", "", "SPX: feed stalled", {{"underlying", "SPX"}}));
  events->publish(event("fill", "demo/practice", "practice fill"));
  EXPECT_EQ(messages(events->history("main", "live", 0, 10).events),
            (std::vector<std::string>{"fill 2", "fill 3", "fill 4", "SPX: feed stalled"}));
  EXPECT_EQ(messages(events->history("practice", "live", 0, 10).events),
            (std::vector<std::string>{"SPX: feed stalled", "practice fill"}));
  // After a sequence, and the newest `limit` of them.
  EXPECT_EQ(messages(events->history("main", "live", 3, 10).events), (std::vector<std::string>{"fill 4", "SPX: feed stalled"}));
  EXPECT_EQ(messages(events->history("main", "live", 0, 1).events), (std::vector<std::string>{"SPX: feed stalled"}));
  EXPECT_TRUE(events->history("main", "replay", 0, 10).events.empty());

  // Replay runs share their account's key; each run's events carry its run and go with it.
  events->publish(event("rule_trip", "replay/run-1/main", "run 1 trip"));
  events->publish(event("rule_trip", "replay/run-2/main", "run 2 trip"));
  kept = events->history("main", "replay", 0, 10);
  ASSERT_EQ(kept.events.size(), 2U);
  EXPECT_EQ(kept.events[0].at("run_id"), "run-1");
  EXPECT_EQ(kept.events[1].at("source"), "replay");
  events->remove_account("replay/run-1/main");
  EXPECT_EQ(messages(events->history("main", "replay", 0, 10).events), (std::vector<std::string>{"run 2 trip"}));

  // A deleted account's history goes with it; the feed's stays.
  events->remove_account("main");
  EXPECT_EQ(messages(events->history("main", "live", 0, 10).events), (std::vector<std::string>{"SPX: feed stalled"}));
  EXPECT_EQ(messages(events->history("practice", "live", 0, 10).events),
            (std::vector<std::string>{"SPX: feed stalled", "practice fill"}));
}

TEST(NotificationHistory, FloorWarningsAreKeptOnEnteringAndLeavingTheBandOnly) {
  const auto events = stream();
  const auto room_left = [&](const char* room) { events->publish(event("floor", "main", "Floor room", {{"room", room}, {"complete", true}})); };
  room_left("2000.00");
  room_left("400.00");
  room_left("300.00");
  room_left("900.00");
  const auto kept = events->history("main", "live", 0, 10).events;
  ASSERT_EQ(kept.size(), 2U);
  EXPECT_EQ(kept[0].at("details").at("warning"), true);
  EXPECT_EQ(kept[0].at("details").at("floor_distance"), "500.00");
  EXPECT_EQ(kept[1].at("details").at("warning"), false);
}

TEST(NotificationHistory, AllAccountsTogetherStayBoundedAndTheOldestGoFirst) {
  const auto events = stream();
  const std::size_t accounts = server::kNotificationHistoryTotal / server::kNotificationHistory + 10;
  for (std::size_t a = 0; a < accounts; ++a)
    for (std::size_t i = 0; i < server::kNotificationHistory; ++i)
      events->publish(event("fill", "account-" + std::to_string(a), "fill"));
  std::size_t total = 0;
  for (std::size_t a = 0; a < accounts; ++a)
    total += events->history("account-" + std::to_string(a), "live", 0, server::kNotificationHistory).events.size();
  EXPECT_EQ(total, server::kNotificationHistoryTotal);
  EXPECT_TRUE(events->history("account-0", "live", 0, server::kNotificationHistory).events.empty());
  EXPECT_TRUE(events->history("account-9", "live", 0, server::kNotificationHistory).events.empty());
  EXPECT_EQ(events->history("account-10", "live", 0, server::kNotificationHistory).events.size(), server::kNotificationHistory);
}

TEST(NotificationHistory, WithoutHistoryNothingIsKeptAndListenersStillHearEvents) {
  const auto events = stream(0);
  std::vector<json> heard;
  events->subscribe("main", "live", [&](const json& message) { heard.push_back(message); });
  events->publish(event("fill", "main", "fill 1"));
  ASSERT_EQ(heard.size(), 1U);
  EXPECT_FALSE(heard[0].contains("received"));
  EXPECT_TRUE(events->history("main", "live", 0, 10).events.empty());
  EXPECT_EQ(events->history("main", "live", 0, 10).sequence, 1U);
}

class HistorySource final : public server::MetricsSource {
 public:
  std::shared_ptr<server::Notifications> events;
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { return {}; }
  const server::Notifications* event_stream() const override { return events.get(); }
};
server::ApiResponse get(const server::MetricsSource& source, const std::string& target, std::string sandbox = {}) {
  server::ApiRequest request{"GET", target};
  request.access.sandbox = std::move(sandbox);
  return server::handle_api(request, source);
}

TEST(NotificationHistory, ApiReadsAnAccountsKeptEventsAndRefusesBadQueries) {
  HistorySource source;
  source.events = stream();
  source.events->publish(event("fill", "main", "Bought 1 SPY at $4.20", {{"id", "1"}, {"order_id", "1"}}));
  source.events->publish(event("feed_stalled", "", "SPX: feed stalled", {{"underlying", "SPX"}}));
  source.events->publish(event("playbook_ready", "demo/practice", "Staged playbook order ready", {{"stage_id", "one"}}));
  source.events->publish(event("alert", "replay/run-1/main", "Replay alert", {{"alert_id", "1"}}));

  const auto all = get(source, "/api/notifications/history");
  ASSERT_EQ(all.status, 200) << all.body;
  test::capture_contract("paper", "GET", "/api/notifications/history", all);
  auto body = json::parse(all.body);
  EXPECT_EQ(body.at("account"), "main");
  EXPECT_EQ(body.at("source"), "live");
  EXPECT_EQ(body.at("capacity"), server::kNotificationHistory);
  EXPECT_EQ(body.at("sequence"), "4");
  EXPECT_EQ(messages(body.at("events")), (std::vector<std::string>{"Bought 1 SPY at $4.20", "SPX: feed stalled"}));

  const auto practice = get(source, "/api/notifications/history?account=practice&since=2&limit=5");
  ASSERT_EQ(practice.status, 200) << practice.body;
  test::capture_contract("paper", "GET", "/api/notifications/history?account=practice&since=2&limit=5", practice);
  EXPECT_EQ(messages(json::parse(practice.body).at("events")), (std::vector<std::string>{"Staged playbook order ready"}));
  const auto replay = get(source, "/api/notifications/history?source=replay");
  ASSERT_EQ(replay.status, 200) << replay.body;
  body = json::parse(replay.body);
  EXPECT_EQ(body.at("source"), "replay");
  ASSERT_EQ(body.at("events").size(), 1U);
  EXPECT_EQ(body.at("events")[0].at("run_id"), "run-1");
  // A sandbox token reads its own live account: the default is its own.
  const auto own = get(source, "/api/notifications/history", "sbox-one");
  ASSERT_EQ(own.status, 200) << own.body;
  EXPECT_EQ(json::parse(own.body).at("account"), "sbox-one");

  for (const auto* bad : {"?source=demo", "?limit=0", "?limit=201", "?since=-1", "?since=x", "?account=Main", "?expiries=8", "?account=main&account=other"}) {
    const auto refused = get(source, std::string("/api/notifications/history") + bad);
    EXPECT_EQ(refused.status, 400) << bad << ": " << refused.body;
    EXPECT_EQ(json::parse(refused.body).at("error").at("code"), "INVALID_REQUEST") << bad;
  }
  const auto bad_source = get(source, "/api/notifications/history?source=demo");
  test::capture_contract("paper", "GET", "/api/notifications/history?source=demo", bad_source);
  EXPECT_EQ(get(source, "/api/notifications/history?source=replay", "sbox-one").status, 403);

  std::optional<server::ApiResponse> posted;
  server::handle_api_async({"POST", "/api/notifications/history", "{}"}, source, [&](server::ApiResponse response) { posted = std::move(response); });
  ASSERT_TRUE(posted);
  EXPECT_EQ(posted->status, 405);
  EXPECT_EQ(posted->allow, "GET, HEAD");

  HistorySource without;
  const auto unavailable = get(without, "/api/notifications/history");
  EXPECT_EQ(unavailable.status, 503);
  EXPECT_EQ(json::parse(unavailable.body).at("error").at("code"), "NOTIFICATIONS_UNAVAILABLE");
}

class StalledProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "manual"; }
  md::Capabilities capabilities() const noexcept override {
    md::Capabilities caps;
    caps.poll_interval = std::chrono::seconds(30);
    return caps;
  }
  void start(const md::Subscription&, md::EventSink&) override {}
  void stop() override {}
};

// A stall reached only outbound channels before; without one it left no record at all.
TEST(NotificationHistory, AFeedStallIsKeptWithoutAnOutboundChannel) {
  StalledProvider provider;
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.paper_enabled = false;
  options.clock = [&] { return clock.load(); };
  options.event_stream = stream();
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_EQ(engine.notifications(), nullptr);
  ASSERT_EQ(engine.event_stream(), options.event_stream.get());
  // A polled feed is overdue after three polls or a minute, whichever is longer.
  clock += 91 * md::kNanosPerSecond;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  std::optional<json> stall;
  while (!stall && std::chrono::steady_clock::now() < deadline) {
    for (const auto& item : json::parse(get(engine, "/api/notifications/history").body).at("events"))
      if (item.at("event") == "feed_stalled") stall = item;
    if (!stall) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  engine.stop();
  ASSERT_TRUE(stall);
  EXPECT_EQ(stall->at("details").at("underlying"), "SPX");
  EXPECT_EQ(stall->at("account"), "");
  EXPECT_EQ(stall->at("source"), "live");
}

}  // namespace
