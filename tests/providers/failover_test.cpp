#include "openport/providers/failover.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <thread>

#include "openport/providers/factory.hpp"
#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"
#include "support/broker_sink.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
constexpr md::Timestamp kStart = 1790263800000LL * 1'000'000;
constexpr md::Timestamp kSecond = md::kNanosPerSecond;

/// A feed the test drives by hand.
class ManualFeed final : public md::Provider {
 public:
  ManualFeed(std::string name, md::Capabilities capabilities) : name_(std::move(name)), capabilities_(capabilities) {}
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override { return capabilities_; }
  void start(const md::Subscription& requested, md::EventSink& out) override {
    ++starts;
    subscription = requested;
    sink = &out;
  }
  void stop() override { ++stops; }
  void send(md::Event event) const { sink->publish(std::move(event)); }
  md::EventSink* sink = nullptr;
  md::Subscription subscription;
  int starts = 0, stops = 0;

 private:
  std::string name_;
  md::Capabilities capabilities_;
};

md::Capabilities snapshots(std::chrono::seconds poll, std::chrono::seconds delay = 0s) {
  md::Capabilities capabilities;
  capabilities.poll_interval = poll;
  capabilities.delay = delay;
  return capabilities;
}

md::OptionContract contract(std::string_view osi) { return *md::parse_osi(osi); }
const auto kA = contract("SPXW  261005C07410000");
const auto kB = contract("SPXW  261005P07400000");
const auto kC = contract("SPXW  261016C07500000");

struct Failover : testing::Test {
  void SetUp() override {
    auto first = std::make_unique<ManualFeed>("tradier", snapshots(2s));
    auto second = std::make_unique<ManualFeed>("cboe", snapshots(15s, 900s));
    primary = first.get();
    backup = second.get();
    provider = std::make_unique<providers::FailoverProvider>(std::move(first), std::move(second),
        providers::FailoverProvider::Options{30s, [this] { return now; }, false});
    provider->start({{"SPX", "SPY"}}, sink);
  }
  void TearDown() override { provider->stop(); }
  void at(md::Timestamp seconds) {
    now = kStart + seconds * kSecond;
    provider->tick();
  }
  md::ProviderStatus status(md::FeedState state, std::string underlying = "SPX", std::string message = "ok") const {
    return {now, state, std::move(message), std::move(underlying)};
  }
  /// The primary's first complete SPX snapshot: A and C priced.
  void primary_snapshot() {
    primary->send(md::ContractDefinition{7, kA});
    primary->send(md::ContractDefinition{8, kC});
    primary->send(md::OptionQuote{7, kStart, 31.1, 31.6, 12, 30});
    primary->send(md::OptionQuote{8, kStart, 5.0, 5.4, 3, 3});
    primary->send(md::SnapshotComplete{"SPX", kStart});
    primary->send(status(md::FeedState::Live));
  }
  md::Timestamp now = kStart;
  test::BrokerSink sink;
  ManualFeed* primary = nullptr;
  ManualFeed* backup = nullptr;
  std::unique_ptr<providers::FailoverProvider> provider;
};

TEST_F(Failover, NamesBothFeedsAndPassesThePrimaryOnUntilItFails) {
  EXPECT_EQ(provider->name(), "tradier+cboe");
  EXPECT_EQ(primary->starts, 1);
  EXPECT_EQ(backup->starts, 0);  // started only when needed
  primary_snapshot();
  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 2u);
  EXPECT_EQ(definitions[0].id, 0u);  // remapped into the shared id space
  EXPECT_EQ(definitions[1].id, 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>().at(0).id, 0u);
  const auto statuses = sink.all<md::ProviderStatus>();
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_EQ(statuses[0].state, md::FeedState::Live);
  EXPECT_EQ(statuses[0].feed, "tradier");
  const auto capabilities = provider->capabilities();
  EXPECT_TRUE(capabilities.realtime_plan_dependent);
  EXPECT_EQ(capabilities.poll_interval, 15s);
  EXPECT_EQ(capabilities.delay, 0s);
}

TEST_F(Failover, AnErrorLastingTheThresholdSwitchesThatUnderlyingToTheBackup) {
  primary_snapshot();
  at(10);
  primary->send(status(md::FeedState::Error, "SPX", "HTTP 503"));
  at(39);
  EXPECT_FALSE(provider->on_backup("SPX"));
  EXPECT_EQ(backup->starts, 0);
  sink.events.clear();
  at(40);
  EXPECT_TRUE(provider->on_backup("SPX"));
  EXPECT_FALSE(provider->on_backup("SPY"));
  EXPECT_EQ(backup->starts, 1);
  const auto switched = sink.all<md::ProviderStatus>();
  ASSERT_EQ(switched.size(), 1u);
  EXPECT_EQ(switched[0].state, md::FeedState::Connecting);
  EXPECT_EQ(switched[0].feed, "cboe");
  EXPECT_EQ(switched[0].message, "fallback cboe: connecting; primary tradier error");
  // Until the backup vouches for its chain, the primary's aging quotes stay.
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_EQ(provider->capabilities().delay, 900s);

  // The backup knows A and B, not C. Its ids differ and map by OSI symbol.
  sink.events.clear();
  backup->send(md::ContractDefinition{0, kB});
  backup->send(md::ContractDefinition{1, kA});
  backup->send(md::OptionQuote{0, kStart - 900 * kSecond, 2.0, 2.2, 5, 5});
  backup->send(md::OptionQuote{1, kStart - 900 * kSecond, 30.9, 31.4, 10, 10});
  primary->send(md::OptionQuote{7, kStart + 45 * kSecond, 1, 2, 1, 1});  // muted while on the backup
  backup->send(md::SnapshotComplete{"SPX", kStart - 900 * kSecond});
  backup->send(status(md::FeedState::Delayed, "SPX", "Cboe: 15-minute delayed"));
  ASSERT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  EXPECT_EQ(sink.all<md::ContractDefinition>()[0].id, 2u);  // B is new
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 3u);
  EXPECT_EQ(quotes[0].id, 2u);
  EXPECT_EQ(quotes[1].id, 0u);
  EXPECT_DOUBLE_EQ(quotes[1].bid, 30.9);
  EXPECT_EQ(quotes[1].ts, kStart - 900 * kSecond);  // the backup's own market time
  // C, which the backup does not price, is cleared before its snapshot vouches for the chain.
  EXPECT_EQ(quotes[2].id, 1u);
  EXPECT_EQ(quotes[2].ts, 0);
  EXPECT_EQ(quotes[2].bid, 0);
  EXPECT_TRUE(std::holds_alternative<md::OptionQuote>(sink.events[sink.events.size() - 3]));
  EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(sink.events[sink.events.size() - 2]));
  const auto healthy = sink.all<md::ProviderStatus>().back();
  EXPECT_EQ(healthy.state, md::FeedState::Delayed);  // backup data is never reported Live
  EXPECT_EQ(healthy.feed, "cboe");
  EXPECT_NE(healthy.message.find("fallback cboe: Cboe: 15-minute delayed"), std::string::npos);
}

TEST_F(Failover, ARecoveredPrimaryTakesBackWithItsLatestValues) {
  primary_snapshot();
  primary->send(status(md::FeedState::Stale));
  at(31);
  ASSERT_TRUE(provider->on_backup("SPX"));
  backup->send(md::ContractDefinition{0, kB});
  backup->send(md::OptionQuote{0, kStart, 2.0, 2.2, 5, 5});
  backup->send(md::SnapshotComplete{"SPX", kStart});
  backup->send(status(md::FeedState::Delayed));
  // The primary still polls in the background; its values are kept, not forwarded.
  sink.events.clear();
  now = kStart + 50 * kSecond;
  primary->send(md::OptionQuote{7, kStart + 49 * kSecond, 31.0, 31.5, 9, 9});
  EXPECT_TRUE(sink.events.empty());
  primary->send(md::SnapshotComplete{"SPX", kStart + 49 * kSecond});
  primary->send(status(md::FeedState::Live, "SPX", "Tradier: real-time options"));
  EXPECT_FALSE(provider->on_backup("SPX"));
  ASSERT_FALSE(sink.events.empty());
  const auto& first = std::get<md::ProviderStatus>(sink.events[0]);
  EXPECT_EQ(first.state, md::FeedState::Live);
  EXPECT_EQ(first.feed, "tradier");
  EXPECT_EQ(first.message, "tradier recovered; Tradier: real-time options");
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 2u);
  EXPECT_EQ(quotes[0].id, 0u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 31.0);
  EXPECT_EQ(quotes[1].id, 1u);  // C's last primary quote is offered again
  EXPECT_EQ(provider->capabilities().delay, 0s);
  // B, priced only by the backup, is cleared at the primary's next complete snapshot.
  sink.events.clear();
  primary->send(md::SnapshotComplete{"SPX", kStart + 51 * kSecond});
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].id, 2u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].ask, 0);
}

TEST_F(Failover, SilenceCountsAsFailureAfterTheEnginesStaleWindow) {
  // No word from the primary: stale after max(3 x 2 s, 60 s), then 30 s more.
  at(89);
  EXPECT_FALSE(provider->on_backup("SPX"));
  at(90);
  EXPECT_TRUE(provider->on_backup("SPX"));
  EXPECT_TRUE(provider->on_backup("SPY"));
  EXPECT_EQ(backup->starts, 1);
  EXPECT_EQ(backup->subscription.underlyings, (std::vector<std::string>{"SPX", "SPY"}));
}

TEST_F(Failover, UnderlyingsFailOverIndependently) {
  primary_snapshot();
  primary->send(status(md::FeedState::Live, "SPY"));
  at(20);
  primary->send(status(md::FeedState::Error, "SPX"));
  primary->send(status(md::FeedState::Live, "SPY"));
  at(50);
  primary->send(status(md::FeedState::Live, "SPY"));
  EXPECT_TRUE(provider->on_backup("SPX"));
  EXPECT_FALSE(provider->on_backup("SPY"));
  sink.events.clear();
  primary->send(md::UnderlyingQuote{"SPY", kStart, 600, 600.1, 0});
  backup->send(md::UnderlyingQuote{"SPY", kStart, 599, 599.1, 0});
  backup->send(md::UnderlyingQuote{"SPX", kStart, 7400, 7401, 0});
  primary->send(md::UnderlyingQuote{"SPX", kStart, 7405, 7406, 0});
  const auto spots = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(spots.size(), 2u);
  EXPECT_EQ(spots[0].bid, 600);
  EXPECT_EQ(spots[1].bid, 7400);
}

TEST_F(Failover, AProviderWideErrorAppliesToEveryUnderlying) {
  primary_snapshot();
  primary->send(md::ProviderStatus{now, md::FeedState::Error, "tradier: token rejected", ""});
  at(30);
  EXPECT_TRUE(provider->on_backup("SPX"));
  EXPECT_TRUE(provider->on_backup("SPY"));
}

TEST(FailoverStreaming, AStreamThatProvesItselfWithQuotesIsCreditedLikeTheEngineWould) {
  md::Timestamp now = kStart;
  auto first = std::make_unique<ManualFeed>("databento", snapshots(0s));
  auto* primary = first.get();
  providers::FailoverProvider provider(std::move(first), std::make_unique<ManualFeed>("cboe", snapshots(15s, 900s)),
                                       {30s, [&] { return now; }, false});
  test::BrokerSink sink;
  provider.start({{"SPX"}}, sink);
  primary->send(md::ContractDefinition{3, kA});
  primary->send(md::OptionQuote{3, kStart, 1, 2, 3, 4});
  const auto statuses = sink.all<md::ProviderStatus>();
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_EQ(statuses[0].state, md::FeedState::Live);
  EXPECT_EQ(statuses[0].feed, "databento");
  now += 80 * kSecond;
  provider.tick();
  EXPECT_FALSE(provider.on_backup("SPX"));
  now += 10 * kSecond;
  provider.tick();
  EXPECT_TRUE(provider.on_backup("SPX"));
  provider.stop();
  EXPECT_EQ(sink.all<md::ProviderStatus>().back().state, md::FeedState::Stopped);
  // Nothing passes after stop.
  const auto count = sink.events.size();
  primary->send(md::OptionQuote{3, kStart, 1, 2, 3, 4});
  EXPECT_EQ(sink.events.size(), count);
}

TEST(FailoverEngine, StatusNamesTheFallbackAndEachUnderlyingsFeed) {
  std::atomic<md::Timestamp> now{kStart};
  auto first = std::make_unique<ManualFeed>("tradier", snapshots(2s));
  auto second = std::make_unique<ManualFeed>("cboe", snapshots(15s, 900s));
  auto* primary = first.get();
  auto* backup = second.get();
  providers::FailoverProvider provider(std::move(first), std::move(second), {30s, [&] { return now.load(); }, false});
  server::Engine::Options options;
  options.clock = [&] { return now.load(); };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  const auto eventually = [](auto predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  };
  primary->send(md::ProviderStatus{now, md::FeedState::Live, "Tradier: real-time options", "SPX"});
  primary->send(md::ProviderStatus{now, md::FeedState::Live, "Tradier: real-time options", "SPY"});
  primary->send(md::ProviderStatus{now, md::FeedState::Error, "tradier SPX: HTTP 503", "SPX"});
  now += 30 * kSecond;
  provider.tick();
  backup->send(md::ProviderStatus{now, md::FeedState::Delayed, "Cboe: delayed", "SPX"});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPX").state == md::FeedState::Delayed; }));
  const auto json = nlohmann::json::parse(server::handle_api({"GET", "/api/status"}, engine).body);
  EXPECT_EQ(json["provider"]["name"], "tradier+cboe");
  EXPECT_EQ(json["provider"]["fallback"], (nlohmann::json{{"primary", "tradier"}, {"backup", "cboe"}, {"after_seconds", 30}}));
  EXPECT_EQ(json["provider"]["delay_seconds"], 900);
  EXPECT_EQ(json["underlyings"][0]["symbol"], "SPX");
  EXPECT_EQ(json["underlyings"][0]["feed"], "cboe");
  EXPECT_EQ(json["underlyings"][0]["fallback"], true);
  EXPECT_EQ(json["underlyings"][0]["state"], "delayed");
  EXPECT_EQ(json["underlyings"][0]["message"], "fallback cboe: Cboe: delayed; primary tradier error");
  EXPECT_EQ(json["underlyings"][1]["feed"], "tradier");
  EXPECT_EQ(json["underlyings"][1]["fallback"], false);
  engine.stop();
}

TEST(FailoverFactory, RejectsSimulatedOrIdenticalFeeds) {
  EXPECT_THROW((void)providers::make_failover_provider({"cboe", "", {}}, {"cboe", "", {}}, 30s), std::invalid_argument);
  EXPECT_THROW((void)providers::make_failover_provider({"cboe", "", {}}, {"demo", "", {}}, 30s), std::invalid_argument);
  EXPECT_THROW((void)providers::make_failover_provider({"replay", "", {}}, {"cboe", "", {}}, 30s), std::invalid_argument);
  EXPECT_THROW((void)providers::make_failover_provider({"cboe", "", {}}, {"nope", "", {}}, 30s), std::invalid_argument);
  EXPECT_THROW((void)providers::make_failover_provider({"massive", "key", {}}, {"cboe", "", {}}, 0s), std::invalid_argument);
  const auto provider = providers::make_failover_provider({"massive", "key", {}}, {"cboe", "", {{"poll_seconds", "20"}}}, 45s);
  EXPECT_EQ(provider->name(), "massive+cboe");
  const auto* failover = dynamic_cast<const providers::FailoverProvider*>(provider.get());
  ASSERT_NE(failover, nullptr);
  EXPECT_EQ(failover->after(), 45s);
  EXPECT_EQ(failover->backup_name(), "cboe");
}
}  // namespace
