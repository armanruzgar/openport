#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/md/recording.hpp"
#include "openport/providers/snapshot.hpp"
#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"
#include "support/contract_capture.hpp"
#include "support/recording.hpp"

// G-11: an underlying can be subscribed, and unsubscribed, while openportd runs.
namespace {
using namespace openport;
using nlohmann::json;

template <typename Predicate>
bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

/// A polled feed that counts its polls; the list it polls is PollingProvider's.
class CountingPoller final : public providers::PollingProvider {
 public:
  CountingPoller() : PollingProvider(std::chrono::seconds(0)) {}
  ~CountingPoller() override { stop(); }
  std::string_view name() const noexcept override { return "counting"; }
  md::Capabilities capabilities() const noexcept override {
    md::Capabilities caps;
    caps.poll_interval = std::chrono::seconds(1);
    return caps;
  }
  std::size_t polls(const std::string& underlying) const {
    const std::lock_guard lock(mutex_);
    const auto found = counts_.find(underlying);
    return found == counts_.end() ? 0 : found->second;
  }

 protected:
  std::string poll(net::HttpClient&, const std::string& underlying, const md::Subscription&, md::EventSink&) override {
    {
      const std::lock_guard lock(mutex_);
      ++counts_[underlying];
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return "counting " + underlying;
  }
  md::FeedState healthy_state() const noexcept override { return md::FeedState::Live; }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::size_t> counts_;
};

TEST(UnderlyingSubscription, APolledFeedTakesANewListAtItsNextCycle) {
  CountingPoller poller;
  test::EventCollector events;
  EXPECT_TRUE(poller.live_subscription());
  poller.start({{"SPX"}}, events);
  ASSERT_TRUE(eventually([&] { return poller.polls("SPX") > 0; }));
  poller.update_underlyings({"SPX", "NVDA"});
  ASSERT_TRUE(eventually([&] { return poller.polls("NVDA") > 0; }));
  poller.update_underlyings({"SPX"});
  const auto removed = poller.polls("NVDA");
  const auto before = poller.polls("SPX");
  // Two whole cycles after the one in flight poll SPX alone.
  ASSERT_TRUE(eventually([&] { return poller.polls("SPX") > before + 2; }));
  EXPECT_LE(poller.polls("NVDA"), removed + 1);
  poller.stop();

  std::optional<std::size_t> connecting, live;
  std::vector<std::string> stopped;
  const auto seen = events.snapshot();
  for (std::size_t i = 0; i < seen.size(); ++i) {
    const auto* feed = std::get_if<md::ProviderStatus>(&seen[i]);
    if (!feed) continue;
    if (feed->underlying == "NVDA" && feed->state == md::FeedState::Connecting && !connecting) connecting = i;
    if (feed->underlying == "NVDA" && feed->state == md::FeedState::Live && !live) live = i;
    if (feed->state == md::FeedState::Stopped) stopped.push_back(feed->underlying);
  }
  // An addition says it is connecting before its first poll; the end names the list it polled last.
  ASSERT_TRUE(connecting && live);
  EXPECT_LT(*connecting, *live);
  EXPECT_EQ(stopped, std::vector<std::string>{"SPX"});
}

/// A provider whose list the test reads back; live_subscription() as the test sets it.
class ListedProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "manual"; }
  md::Capabilities capabilities() const noexcept override {
    md::Capabilities caps;
    caps.poll_interval = std::chrono::seconds(15);
    return caps;
  }
  void start(const md::Subscription& subscription, md::EventSink& out) override {
    const std::lock_guard lock(mutex);
    polled = subscription.underlyings;
    sink = &out;
  }
  void stop() override {}
  bool live_subscription() const noexcept override { return live; }
  void update_underlyings(const std::vector<std::string>& underlyings) override {
    const std::lock_guard lock(mutex);
    polled = underlyings;
  }
  std::vector<std::string> list() const {
    const std::lock_guard lock(mutex);
    return polled;
  }
  bool live = true;
  md::EventSink* sink = nullptr;

 private:
  mutable std::mutex mutex;
  std::vector<std::string> polled;
};

server::UnderlyingReply change(server::Engine& engine, std::string symbol, bool add) {
  std::promise<server::UnderlyingReply> done;
  auto reply = done.get_future();
  if (!engine.post_underlying({std::move(symbol), add}, [&](server::UnderlyingReply answer) { done.set_value(std::move(answer)); })) {
    ADD_FAILURE() << "the engine took no change";
    return {};
  }
  return reply.get();
}
server::TradingReply trade(server::Engine& engine, server::TradingCommand command) {
  std::promise<server::TradingReply> done;
  auto reply = done.get_future();
  if (!engine.post_trading(std::move(command), [&](server::TradingReply answer) { done.set_value(std::move(answer)); })) {
    ADD_FAILURE() << "the engine took no command";
    return {};
  }
  return reply.get();
}
server::ApiResponse request(server::MetricsSource& source, std::string method, std::string target, std::string body) {
  std::promise<server::ApiResponse> done;
  auto response = done.get_future();
  server::handle_api_async({std::move(method), std::move(target), std::move(body)}, source,
                           [&](server::ApiResponse answer) { done.set_value(std::move(answer)); });
  return response.get();
}

TEST(UnderlyingSubscription, TheEngineAddsAndRemovesUnderlyingsWhileRunning) {
  ListedProvider provider;
  server::Engine::Options options;
  options.paper_enabled = false;
  options.clock = [] { return md::new_york_to_utc({2026, 9, 22}, 10, 0); };
  std::vector<std::string> fetched;
  options.added_underlying = [&](const std::string& symbol) { fetched.push_back(symbol); };
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  const auto initial = engine.underlying_subscription();
  EXPECT_TRUE(initial.live);
  EXPECT_TRUE(initial.reason.empty());
  EXPECT_EQ(initial.configured, std::vector<std::string>{"SPX"});
  EXPECT_TRUE(initial.added.empty());

  auto reply = change(engine, "NVDA", true);
  EXPECT_EQ(reply.status, 201) << reply.message;
  EXPECT_TRUE(reply.code.empty());
  EXPECT_EQ(reply.subscription.added, std::vector<std::string>{"NVDA"});
  EXPECT_EQ(provider.list(), (std::vector<std::string>{"SPX", "NVDA"}));
  EXPECT_EQ(fetched, std::vector<std::string>{"NVDA"});
  EXPECT_EQ(engine.symbols(), (std::vector<std::string>{"NVDA", "SPX"}));
  EXPECT_EQ(engine.status().underlyings.at("NVDA").state, md::FeedState::Connecting);
  // Carried and waiting for its first snapshot, not unknown.
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings/NVDA/summary"}, engine).status, 503);

  EXPECT_EQ(change(engine, "NVDA", true).status, 200);
  EXPECT_EQ(change(engine, "SPX", true).status, 200);
  reply = change(engine, "SPX", false);
  EXPECT_EQ(reply.status, 409);
  EXPECT_EQ(reply.code, "UNDERLYING_CONFIGURED");
  reply = change(engine, "QQQ", false);
  EXPECT_EQ(reply.status, 404);
  EXPECT_EQ(reply.code, "UNKNOWN_UNDERLYING");

  reply = change(engine, "NVDA", false);
  EXPECT_EQ(reply.status, 200) << reply.message;
  EXPECT_TRUE(reply.subscription.added.empty());
  EXPECT_EQ(provider.list(), std::vector<std::string>{"SPX"});
  // Its last poll can still report after it went: it stays gone.
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "manual NVDA: late poll", "NVDA"});
  engine.synchronize().get();
  EXPECT_EQ(engine.symbols(), std::vector<std::string>{"SPX"});
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings/NVDA/summary"}, engine).status, 404);

  // The limit counts those the engine started with.
  for (std::size_t i = 1; i < server::kMaxUnderlyings; ++i) {
    ASSERT_EQ(change(engine, "S" + std::to_string(i), true).status, 201) << i;
  }
  reply = change(engine, "LAST", true);
  EXPECT_EQ(reply.status, 409);
  EXPECT_EQ(reply.code, "UNDERLYING_LIMIT");
  EXPECT_EQ(engine.underlying_subscription().added.size(), server::kMaxUnderlyings - 1);
  engine.stop();
  EXPECT_FALSE(engine.post_underlying({"NVDA", true}, [](server::UnderlyingReply) {}));
}

TEST(UnderlyingSubscription, AnUnderlyingAnAccountWatchesIsNotRemoved) {
  test::RecordingFile file;
  ListedProvider provider;
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.clock = [] { return md::new_york_to_utc({2026, 9, 22}, 10, 0); };
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_EQ(change(engine, "NVDA", true).status, 201);
  server::TradingCommand watch;
  watch.kind = server::TradingCommand::Kind::CreateAlert;
  watch.alert.condition.scope = trading::AlertScope::Underlying;
  watch.alert.condition.metric = "price";
  watch.alert.condition.symbol = "NVDA";
  watch.alert.condition.direction = trading::TriggerDirection::AtOrAbove;
  watch.alert.condition.level = trading::Money::parse("250");
  const auto created = trade(engine, watch);
  ASSERT_TRUE(created.decision.ok()) << created.decision.message << " " << created.error_code;
  ASSERT_TRUE(created.alert_id);
  const auto refused = change(engine, "NVDA", false);
  EXPECT_EQ(refused.status, 409);
  EXPECT_EQ(refused.code, "UNDERLYING_IN_USE");
  EXPECT_NE(refused.message.find("main"), std::string::npos) << refused.message;
  EXPECT_EQ(refused.subscription.added, std::vector<std::string>{"NVDA"});
  server::TradingCommand forget;
  forget.kind = server::TradingCommand::Kind::DeleteAlert;
  forget.alert_id = *created.alert_id;
  const auto deleted = trade(engine, forget);
  ASSERT_TRUE(deleted.decision.ok()) << deleted.decision.message;
  EXPECT_EQ(change(engine, "NVDA", false).status, 200);
  engine.stop();
}

TEST(UnderlyingSubscription, ARecordingKeepsTheUnderlyingsItStartedWith) {
  test::RecordingFile file;
  ListedProvider provider;
  server::Engine::Options options;
  options.paper_enabled = false;
  options.record_file = file.path;
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_EQ(change(engine, "NVDA", true).status, 201);
  const auto now = md::now();
  auto& sink = *provider.sink;
  sink.publish(md::ContractDefinition{0, *md::parse_osi("SPX   261016C07000000")});
  sink.publish(md::ContractDefinition{1, *md::parse_osi("NVDA  261016C00200000")});
  sink.publish(md::OptionQuote{0, now, 4.5, 4.6, 10, 10});
  sink.publish(md::OptionQuote{1, now, 2.0, 2.1, 5, 5});
  sink.publish(md::UnderlyingQuote{"SPX", now, 0, 0, 7000});
  sink.publish(md::UnderlyingQuote{"NVDA", now, 0, 0, 200});
  sink.publish(md::SnapshotComplete{"NVDA", now});
  sink.publish(md::ProviderStatus{now, md::FeedState::Live, "manual NVDA", "NVDA"});
  sink.publish(md::ProviderStatus{now, md::FeedState::Live, "manual SPX", "SPX"});
  engine.synchronize().get();
  // The engine has both; the recording only what its header names.
  EXPECT_EQ(engine.status().contracts, 2U);
  engine.stop();
  ASSERT_TRUE(engine.recording_error().empty()) << engine.recording_error();
  md::RecordingReader reader(file.path);
  EXPECT_EQ(reader.header().subscription.underlyings, std::vector<std::string>{"SPX"});
  std::vector<std::string> recorded;
  while (auto record = reader.next()) {
    std::visit([&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, md::ContractDefinition>) recorded.push_back("definition " + value.contract.underlying);
      else if constexpr (std::is_same_v<T, md::OptionQuote>) recorded.push_back("quote " + std::to_string(value.id));
      else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) recorded.push_back("spot " + value.symbol);
      else if constexpr (std::is_same_v<T, md::SnapshotComplete>) recorded.push_back("snapshot " + value.underlying);
      else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
        if (value.state == md::FeedState::Live) recorded.push_back("status " + value.underlying);
      }
    }, record->event);
  }
  EXPECT_TRUE(reader.diagnostic().empty()) << reader.diagnostic();
  EXPECT_EQ(recorded, (std::vector<std::string>{"definition SPX", "quote 0", "spot SPX", "status SPX"}));
}

TEST(UnderlyingSubscription, ApiListsAddsAndRemovesUnderlyings) {
  ListedProvider provider;
  server::Engine::Options options;
  options.paper_enabled = false;
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  const auto listed = server::handle_api({"GET", "/api/underlyings"}, engine);
  ASSERT_EQ(listed.status, 200) << listed.body;
  test::capture_contract("analytics", "GET", "/api/underlyings", listed);
  auto body = json::parse(listed.body);
  EXPECT_EQ(body.at("underlyings"), json::array({"SPX"}));
  EXPECT_EQ(body.at("configured"), json::array({"SPX"}));
  EXPECT_EQ(body.at("live"), true);
  EXPECT_EQ(body.at("reason"), nullptr);
  EXPECT_EQ(body.at("limit"), server::kMaxUnderlyings);
  EXPECT_FALSE(body.contains("message"));
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings?x=1"}, engine).status, 400);

  // Lowercase is taken as capitals.
  const auto added = request(engine, "POST", "/api/underlyings", R"({"symbol":"nvda"})");
  ASSERT_EQ(added.status, 201) << added.body;
  test::capture_contract("analytics", "POST", "/api/underlyings", added);
  body = json::parse(added.body);
  EXPECT_EQ(body.at("added"), json::array({"NVDA"}));
  EXPECT_EQ(body.at("underlyings"), json::array({"SPX", "NVDA"}));
  EXPECT_TRUE(body.at("message").is_string());
  EXPECT_EQ(request(engine, "POST", "/api/underlyings", R"({"symbol":"NVDA"})").status, 200);
  for (const auto* bad : {R"({"symbol":""})", R"({"symbol":"NV DA"})", R"({"symbol":".X"})", R"({"symbol":"ABCDEFGHIJKLMNOPQ"})",
                          R"({"symbol":7})", R"({"ticker":"NVDA"})", R"({"symbol":"NVDA","x":1})", "[]"}) {
    const auto refused = request(engine, "POST", "/api/underlyings", bad);
    EXPECT_EQ(refused.status, 400) << bad;
  }
  EXPECT_EQ(request(engine, "POST", "/api/underlyings?x=1", R"({"symbol":"NVDA"})").status, 400);
  const auto configured = request(engine, "DELETE", "/api/underlyings/SPX", "");
  EXPECT_EQ(configured.status, 409);
  EXPECT_EQ(json::parse(configured.body).at("error").at("code"), "UNDERLYING_CONFIGURED");
  test::capture_contract("analytics", "DELETE", "/api/underlyings/SPX", configured);
  const auto removed = request(engine, "DELETE", "/api/underlyings/NVDA", "");
  ASSERT_EQ(removed.status, 200) << removed.body;
  test::capture_contract("analytics", "DELETE", "/api/underlyings/NVDA", removed);
  EXPECT_EQ(json::parse(removed.body).at("added"), json::array());
  EXPECT_EQ(request(engine, "DELETE", "/api/underlyings/NVDA", "").status, 404);
  // A wrong method names the ones the route takes.
  const auto put = request(engine, "PUT", "/api/underlyings", "{}");
  EXPECT_EQ(put.status, 405);
  EXPECT_EQ(put.allow, "GET, HEAD, POST");
  EXPECT_EQ(server::allowed_methods("/api/underlyings/SPX"), "GET, HEAD, DELETE");
  EXPECT_EQ(request(engine, "DELETE", "/api/underlyings/SPX/chain", "").status, 405);
  engine.stop();

  // A provider that subscribes when it connects says so, and nothing changes.
  ListedProvider fixed;
  fixed.live = false;
  server::Engine still(fixed, {{"SPX"}}, options);
  still.start();
  const auto refused = request(still, "POST", "/api/underlyings", R"({"symbol":"NVDA"})");
  EXPECT_EQ(refused.status, 409);
  EXPECT_EQ(json::parse(refused.body).at("error").at("code"), "SUBSCRIPTION_FIXED");
  test::capture_contract("analytics", "POST", "/api/underlyings", refused);
  const auto reason = json::parse(server::handle_api({"GET", "/api/underlyings"}, still).body);
  EXPECT_EQ(reason.at("live"), false);
  EXPECT_NE(reason.at("reason").get<std::string>().find("restart openportd with --symbols"), std::string::npos);
  EXPECT_EQ(fixed.list(), std::vector<std::string>{"SPX"});
  still.stop();
}

}  // namespace
