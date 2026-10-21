#include "openport/server/engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <nlohmann/json.hpp>
#include <thread>

#include "openport/server/api.hpp"

namespace {

using namespace openport;

class ManualProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "manual"; }
  md::Capabilities capabilities() const noexcept override { return caps; }
  void start(const md::Subscription&, md::EventSink& out) override { sink = &out; }
  void stop() override {}
  md::Capabilities caps;
  md::EventSink* sink = nullptr;
};

template <typename Predicate>
bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

TEST(Engine, KeepsErrorsPerUnderlyingAndExpiresHealthUsingReceiptTime) {
  ManualProvider provider;
  provider.caps.poll_interval = std::chrono::seconds(30);
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Error, "SPX: offline", "SPX"});
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Delayed, "SPY: delayed", "SPY"});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPY").last_success > 0; }));
  auto status = engine.status();
  EXPECT_EQ(status.feed_state, md::FeedState::Error);
  EXPECT_EQ(status.underlyings.at("SPX").last_error, "SPX: offline");
  EXPECT_EQ(status.underlyings.at("SPY").state, md::FeedState::Delayed);
  EXPECT_EQ(status.underlyings.at("SPY").last_success, clock.load());
  clock += 90 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Delayed);
  clock += md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  EXPECT_EQ(engine.status().feed_state, md::FeedState::Stale);

  const auto json = nlohmann::json::parse(server::handle_api({"GET", "/api/status"}, engine).body);
  ASSERT_EQ(json["underlyings"].size(), 2u);  // visible before any analytics exist
  EXPECT_EQ(json["underlyings"][0]["last_error"], "SPX: offline");
  EXPECT_EQ(json["underlyings"][1]["state"], "stale");
  EXPECT_TRUE(json["underlyings"][1]["spot"].is_null());
  EXPECT_EQ(json["feed"]["state"], "stale");
  const auto tick = nlohmann::json::parse(server::tick_message(engine));
  EXPECT_EQ(tick["feed"]["state"], "stale");
  EXPECT_EQ(tick["underlyings"][1]["state"], "stale");

  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "SPX: recovered", "SPX"});
  ASSERT_TRUE(eventually(
      [&] { return engine.status().underlyings.at("SPX").state == md::FeedState::Live; }));
  EXPECT_EQ(engine.status().underlyings.at("SPX").last_error, "SPX: offline");
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  engine.stop();
}

TEST(Engine, FeedMessageKeepsEachDistinctMessageOnceInUnderlyingOrder) {
  ManualProvider provider;
  server::Engine::Options options;
  options.clock = [] { return 1000 * md::kNanosPerSecond; };
  server::Engine engine(provider, {{"QQQ", "SPX", "SPY", "XSP"}}, options);
  engine.start();
  for (const auto& symbol : {"QQQ", "SPY", "XSP"})
    provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "demo: simulated prices", symbol});
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Error, "SPX: offline", "SPX"});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPX").last_error_time > 0; }));
  const auto status = engine.status();
  EXPECT_EQ(status.feed_message, "demo: simulated prices; SPX: offline");
  EXPECT_EQ(status.feed_state, md::FeedState::Error);
  EXPECT_EQ(status.underlyings.at("SPY").message, "demo: simulated prices");
  engine.stop();
}

TEST(Engine, StreamingQuotesRefreshOnlyTheirUnderlyingWithASixtySecondMinimum) {
  ManualProvider provider;
  provider.caps.realtime = true;
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(1);
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  clock += 61 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().feed_state, md::FeedState::Stale);
  provider.sink->publish(md::ContractDefinition{0, *md::parse_osi("SPY991218C00500000")});
  provider.sink->publish(md::OptionQuote{0, 1, 5, 6, 1, 1});
  provider.sink->publish(md::ContractDefinition{1, *md::parse_osi("SPY1991218C00500000")});
  ASSERT_TRUE(eventually([&] { return engine.status().nonstandard_contracts == 1; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Live);
  EXPECT_EQ(engine.status().underlyings.at("SPX").state, md::FeedState::Stale);
  clock += 60 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Live);
  clock += md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  const auto json = nlohmann::json::parse(server::handle_api({"GET", "/api/status"}, engine).body);
  EXPECT_EQ(json["engine"]["nonstandard_contracts"], 1);
  engine.stop();
}

TEST(Engine, PlanDependentStreamsKeepAdapterTimingStatusWhileQuotesRefreshReceipt) {
  ManualProvider provider;
  provider.caps.realtime_plan_dependent = true;
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPY"}}, options);
  engine.start();
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Stale, "timestamp units unconfirmed", "SPY"});
  provider.sink->publish(md::UnderlyingQuote{"SPY", 0, 0, 0, 500});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPY").last_success == clock; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  EXPECT_EQ(engine.status().underlyings.at("SPY").message, "timestamp units unconfirmed");
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Delayed, "delayed", "SPY"});
  clock += md::kNanosPerSecond;
  provider.sink->publish(md::UnderlyingQuote{"SPY", 1, 0, 0, 501});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPY").last_success == clock; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Delayed);
  engine.stop();
}

TEST(Engine, StatusUsesRevisedPollBudgetForStaleness) {
  ManualProvider provider;
  provider.caps.poll_interval = std::chrono::seconds(1);
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPY"}}, options);
  // Change before the worker starts; the static startup capability was one second.
  provider.caps.poll_interval = std::chrono::seconds(60);
  engine.start();
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "complete", "SPY"});
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPY").last_success == clock; }));
  clock += 90 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().capabilities.poll_interval, std::chrono::seconds(60));
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Live);
  clock += 91 * md::kNanosPerSecond;
  EXPECT_EQ(engine.status().underlyings.at("SPY").state, md::FeedState::Stale);
  engine.stop();
}

}  // namespace

namespace {
TEST(Engine, ACleanShutdownSendsNoFeedStalledNotifications) {
  class SilentHttp final : public net::HttpClient {
   public:
    net::HttpResponse post(std::string_view, std::string_view, const net::Headers&, std::chrono::seconds,
                           const std::atomic<bool>*) override { return {200, "{}"}; }
  };
  // Like the polling and streaming providers, it reports Stopped while it shuts down.
  class StoppingProvider final : public md::Provider {
   public:
    std::string_view name() const noexcept override { return "stopping"; }
    md::Capabilities capabilities() const noexcept override { return {}; }
    void start(const md::Subscription&, md::EventSink& out) override { sink = &out; }
    void stop() override {
      if (!sink) return;
      sink->publish(md::ProviderStatus{1, md::FeedState::Stopped, "SPY: stopped", "SPY"});
      ASSERT_TRUE(eventually([&] { return engine->status().underlyings.at("SPY").state == md::FeedState::Stopped; }));
      // Let the engine finish the pass that saw it before stop() goes on.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      sink = nullptr;
    }
    md::EventSink* sink = nullptr;
    server::Engine* engine = nullptr;
  } provider;
  server::Notifications::Options delivery;
  delivery.worker = false;
  auto notifications = std::make_shared<server::Notifications>(server::parse_notification_config(
      R"({"channels":[{"id":"phone","type":"webhook","url":"https://example.invalid/hook"}]})"),
      std::make_unique<SilentHttp>(), delivery);
  server::Engine::Options options;
  options.notifications = notifications;
  server::Engine engine(provider, {{"SPY"}}, options);
  provider.engine = &engine;
  engine.start();
  engine.stop();
  EXPECT_EQ(notifications->status().at("queue_depth"), 0) << notifications->status().dump();
}

TEST(Engine, RejectsEverySecondStartIncludingAfterStop) {
  ManualProvider provider;
  server::Engine engine(provider, {{"SPY"}}, {});
  engine.start();
  EXPECT_THROW(engine.start(), std::logic_error);
  engine.stop();
  EXPECT_THROW(engine.start(), std::logic_error);
}

TEST(Engine, StopsProviderWhenConsumerThreadLaunchThrows) {
  class TrackedProvider final : public md::Provider {
   public:
    std::string_view name() const noexcept override { return "tracked"; }
    md::Capabilities capabilities() const noexcept override { return {}; }
    void start(const md::Subscription&, md::EventSink&) override { running = true; }
    void stop() override {
      running = false;
      ++stops;
    }
    bool running = false;
    int stops = 0;
  } provider;
  server::Engine::Options options;
  options.launch = [](auto) -> std::thread { throw std::runtime_error("no thread resources"); };
  server::Engine engine(provider, {{"SPY"}}, options);
  EXPECT_THROW(engine.start(), std::runtime_error);
  EXPECT_FALSE(provider.running);
  EXPECT_EQ(provider.stops, 1);
  EXPECT_THROW(engine.start(), std::logic_error);
}

TEST(Engine, BatchedHealthPreservesErrorOrderingAndLatestReceipt) {
  ManualProvider provider;
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return clock.load(); };
  server::Engine engine(provider, {{"SPY", "SPX"}}, options);
  engine.start();
  provider.sink->publish(md::UnderlyingQuote{"SPY", 1, 0, 0, 500});
  ASSERT_TRUE(
      eventually([&] { return engine.status().underlyings.at("SPY").last_success == clock; }));
  const auto message = engine.status().underlyings.at("SPY").message;
  clock += md::kNanosPerSecond;
  for (int i = 0; i < 10000; ++i) provider.sink->publish(md::UnderlyingQuote{"SPY", i, 0, 0, 501});
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Error, "disconnected", "SPY"});
  ASSERT_TRUE(eventually(
      [&] { return engine.status().underlyings.at("SPY").state == md::FeedState::Error; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").last_success, clock);
  EXPECT_EQ(engine.status().underlyings.at("SPX").last_success, 0);
  clock += md::kNanosPerSecond;
  provider.sink->publish(md::UnderlyingQuote{"SPY", 1, 0, 0, 502});
  ASSERT_TRUE(
      eventually([&] { return engine.status().underlyings.at("SPY").last_success == clock; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").message, message);
  EXPECT_EQ(engine.status().underlyings.at("SPY").last_error, "disconnected");
}

TEST(Engine, SerializesQueueTelemetryInStatusAndTicks) {
  class BacklogProvider final : public md::Provider {
   public:
    std::string_view name() const noexcept override { return "backlog"; }
    md::Capabilities capabilities() const noexcept override { return {}; }
    void start(const md::Subscription&, md::EventSink& sink) override {
      // Option quotes still coalesce; spot prints preserve settlement order.
      sink.publish(md::OptionQuote{0, 1});
      sink.publish(md::OptionQuote{0, 2});
      for (std::size_t i = 0; i < md::kEventQueueCapacity; ++i) sink.publish(md::OptionTrade{});
    }
    void stop() override {}
  } provider;
  server::Engine::Options options;
  // Hold the consumer until telemetry is inspected, without a scheduling race.
  options.launch = [](auto) { return std::thread([] {}); };
  server::Engine engine(provider, {{"SPY"}}, options);
  engine.start();
  for (const auto& text :
       {server::handle_api({"GET", "/api/status"}, engine).body, server::tick_message(engine)}) {
    const auto json = nlohmann::json::parse(text)["engine"];
    EXPECT_EQ(json["queue_depth"], md::kEventQueueCapacity);
    EXPECT_EQ(json["coalesced_events"], 1);
    EXPECT_EQ(json["dropped_events"], 1);
    EXPECT_EQ(json["overloaded"], true);
  }
}
}  // namespace

namespace {
TEST(Engine, OneDrainUsesOneReceiptClockForAllQuoteHealthUpdates) {
  class BatchProvider final : public md::Provider {
   public:
    std::string_view name() const noexcept override { return "batch"; }
    md::Capabilities capabilities() const noexcept override { return {}; }
    void start(const md::Subscription&, md::EventSink& sink) override {
      sink.publish(md::UnderlyingQuote{"SPX", 1});
      sink.publish(md::UnderlyingQuote{"SPY", 1});
    }
    void stop() override {}
  } provider;
  std::atomic<md::Timestamp> clock{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return ++clock; };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  ASSERT_TRUE(eventually([&] { return engine.status().events == 2; }));
  const auto status = engine.status();
  EXPECT_EQ(status.underlyings.at("SPX").last_success, status.underlyings.at("SPY").last_success);
}
}  // namespace

namespace {
TEST(Engine, PublishesUnchangedHealthTimestampsAtMostEveryHundredMilliseconds) {
  ManualProvider provider;
  std::atomic<md::Timestamp> receipt{1000 * md::kNanosPerSecond};
  std::atomic<int> monotonic_ms{0};
  server::Engine::Options options;
  options.clock = [&] { return receipt.load(); };
  options.monotonic_clock = [&] {
    return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds(monotonic_ms.load());
  };
  server::Engine engine(provider, {{"SPY"}}, options);
  engine.start();
  provider.sink->publish(md::UnderlyingQuote{"SPY", 1});
  ASSERT_TRUE(eventually([&] { return engine.status().events == 1; }));
  const auto first = engine.status();
  ASSERT_EQ(first.underlyings.at("SPY").last_success, receipt);
  for (int batch = 2; batch <= 20; ++batch) {
    receipt += md::kNanosPerSecond;
    monotonic_ms = batch;
    provider.sink->publish(md::UnderlyingQuote{"SPY", batch});
    ASSERT_TRUE(eventually([&] { return engine.status().events == static_cast<std::uint64_t>(batch); }));
    const auto status = engine.status();
    EXPECT_EQ(status.underlyings.at("SPY").last_success, first.underlyings.at("SPY").last_success);
    EXPECT_EQ(status.feed_updated, first.feed_updated);
  }
  monotonic_ms = 99;
  provider.sink->publish(md::UnderlyingQuote{"SPY", 21});
  ASSERT_TRUE(eventually([&] { return engine.status().events == 21; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").last_success,
            first.underlyings.at("SPY").last_success);
  // A pending receipt must be published when due even if there are no more events.
  monotonic_ms = 100;
  ASSERT_TRUE(eventually([&] { return engine.status().underlyings.at("SPY").last_success == receipt; }));
  const auto published = engine.status();
  EXPECT_EQ(published.feed_updated, receipt);
  EXPECT_EQ(published.underlyings.at("SPY").message, first.underlyings.at("SPY").message);
  receipt += md::kNanosPerSecond;
  monotonic_ms = 101;
  provider.sink->publish(md::UnderlyingQuote{"SPY", 22});
  ASSERT_TRUE(eventually([&] { return engine.status().events == 22; }));
  EXPECT_EQ(engine.status().underlyings.at("SPY").last_success,
            published.underlyings.at("SPY").last_success);
}

TEST(Engine, StateAndMessageChangesBypassHealthPublicationThrottle) {
  ManualProvider provider;
  std::atomic<md::Timestamp> receipt{1000 * md::kNanosPerSecond};
  server::Engine::Options options;
  options.clock = [&] { return receipt.load(); };
  options.monotonic_clock = [] { return std::chrono::steady_clock::time_point{}; };
  server::Engine engine(provider, {{"SPY"}}, options);
  engine.start();
  const auto publish = [&](md::Event event, std::uint64_t count) {
    provider.sink->publish(std::move(event));
    return eventually([&] { return engine.status().events == count; });
  };
  ASSERT_TRUE(publish(md::ProviderStatus{1, md::FeedState::Live, "connected", "SPY"}, 1));
  receipt += md::kNanosPerSecond;
  ASSERT_TRUE(publish(md::ProviderStatus{2, md::FeedState::Live, "receiving", "SPY"}, 2));
  EXPECT_EQ(engine.status().underlyings.at("SPY").message, "receiving");
  EXPECT_EQ(engine.status().underlyings.at("SPY").last_success, receipt);
  receipt += md::kNanosPerSecond;
  ASSERT_TRUE(publish(md::ProviderStatus{3, md::FeedState::Error, "disconnected", "SPY"}, 3));
  auto health = engine.status().underlyings.at("SPY");
  EXPECT_EQ(health.state, md::FeedState::Error);
  EXPECT_EQ(health.last_error_time, receipt);
  EXPECT_EQ(health.last_error, "disconnected");
  receipt += md::kNanosPerSecond;
  ASSERT_TRUE(publish(md::UnderlyingQuote{"SPY", 4}, 4));
  health = engine.status().underlyings.at("SPY");
  EXPECT_EQ(health.state, md::FeedState::Live);
  EXPECT_EQ(health.last_success, receipt);
  EXPECT_EQ(health.last_error, "disconnected");
}
}  // namespace

namespace {
// Feed an entire snapshot before launching the consumer so the first published
// QQQ result must already use SPX, despite QQQ preceding SPX alphabetically.
class CurveProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "curve-test"; }
  md::Capabilities capabilities() const noexcept override { return {}; }
  void start(const md::Subscription&, md::EventSink& out) override {
    sink = &out;
    publish("QQQ", .045, 0, pricing::ExerciseStyle::American);
    publish("SPX", .045, 1000, pricing::ExerciseStyle::European);
    publish("XSP", .06, 2000, pricing::ExerciseStyle::European);
  }
  void stop() override {}
  void publish(const std::string& symbol, double rate, md::InstrumentId id,
               pricing::ExerciseStyle style) {
    const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
    if (quote_underlying) sink->publish(md::UnderlyingQuote{symbol, as_of, 100, 100, 100});
    for (auto date : {md::Date{2026, 12, 22}, md::Date{2027, 9, 22}})
      for (double strike = 90; strike <= 110; strike += 2.5)
        for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
          auto c = *md::parse_osi("SPY261218C00100000");
          c.underlying = c.root = symbol;
          c.style = style;
          c.expiry = date;
          c.strike = strike;
          c.type = type;
          sink->publish(md::ContractDefinition{id, c});
          const double years = md::years_between(as_of, c.expiry_time());
          const double price = pricing::bsm_price({type, 100, strike, years, rate, .01, .2});
          sink->publish(md::OptionQuote{id++, as_of, price - .001, price + .001, 1, 1});
        }
  }
  md::EventSink* sink = nullptr;
  bool quote_underlying = true;
};

TEST(Engine, EuropeanCurveReachesAmericanInSamePassAndRefreshesUnchangedChain) {
  CurveProvider provider;
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(1);
  server::Engine engine(provider, {{"QQQ", "SPX", "XSP"}}, options);
  engine.start();
  ASSERT_TRUE(eventually([&] { return engine.metrics("QQQ") != nullptr; }));
  const auto first = engine.metrics("QQQ");
  for (const auto& slice : first->slices) {
    EXPECT_EQ(slice.rate_source, "curve");
    EXPECT_EQ(slice.rate_curve_symbol, "SPX");
    EXPECT_NEAR(-std::log(slice.forward.discount) / slice.years, .045, 1e-9);
  }
  provider.publish("SPX", .05, 1000, pricing::ExerciseStyle::European);
  ASSERT_TRUE(eventually([&] {
    const auto m = engine.metrics("QQQ");
    return std::abs(-std::log(m->slices[0].forward.discount) / m->slices[0].years - .05) < 1e-9;
  }));
  EXPECT_EQ(engine.metrics("QQQ")->version, first->version);
  // A later non-SPX update must not displace the preferred SPX curve.
  const auto xsp_version = engine.metrics("XSP")->version;
  provider.publish("XSP", .07, 2000, pricing::ExerciseStyle::European);
  ASSERT_TRUE(eventually([&] { return engine.metrics("XSP")->version != xsp_version; }));
  EXPECT_EQ(engine.metrics("QQQ")->slices[0].rate_curve_symbol, "SPX");
  EXPECT_NEAR(-std::log(engine.metrics("QQQ")->slices[0].forward.discount) /
                  engine.metrics("QQQ")->slices[0].years,
              .05, 1e-9);
}

TEST(Engine, DividendSeedsAndReplacementsReachOnlyTheirUnderlyingWithoutNewQuotes) {
  CurveProvider provider;
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(1);
  options.dividends = {{"QQQ", {2026, 10, 22}, trading::Money::parse("0.75")},
                       {"SPY", {2026, 10, 23}, trading::Money::parse("2")},
                       {"SPX", {2026, 10, 24}, trading::Money::parse("5")}};
  server::Engine engine(provider, {{"QQQ", "SPX", "XSP"}}, options);
  engine.start();
  ASSERT_TRUE(eventually([&] {
    const auto m = engine.metrics("QQQ");
    return m && m->slices[0].rate_curve_symbol == "SPX" && m->slices[0].dividends.size() == 1;
  }));
  const auto first = engine.metrics("QQQ");
  EXPECT_DOUBLE_EQ(first->slices[0].dividends[0].amount, .75);
  EXPECT_EQ(first->slices[0].dividends[0].ex_date, (md::Date{2026, 10, 22}));
  EXPECT_TRUE(engine.metrics("SPX")->slices[0].dividends.empty());
  EXPECT_TRUE(engine.metrics("XSP")->slices[0].dividends.empty());
  engine.set_dividends({{"QQQ", {2026, 11, 2}, trading::Money::parse("1.25")}});
  ASSERT_TRUE(eventually([&] {
    const auto m = engine.metrics("QQQ");
    return m->slices[0].dividends.size() == 1 && m->slices[0].dividends[0].amount == 1.25;
  }));
  const auto replaced = engine.metrics("QQQ");
  EXPECT_EQ(replaced->version, first->version);
  EXPECT_EQ(replaced->as_of, first->as_of);
  EXPECT_NE(replaced->slices[0].strikes[0].call.eep, first->slices[0].strikes[0].call.eep);
  const auto summary = nlohmann::json::parse(
      server::handle_api({"GET", "/api/underlyings/QQQ/summary"}, engine).body);
  EXPECT_EQ(summary["expiries"][0]["dividends"],
            (nlohmann::json::array({{{"ex_date", "2026-11-02"}, {"amount", 1.25}}})));
  engine.set_dividends({});
  ASSERT_TRUE(eventually([&] { return engine.metrics("QQQ")->slices[0].dividends.empty(); }));
  EXPECT_EQ(engine.metrics("QQQ")->version, first->version);
}

TEST(Engine, SamplesEachAnalysedSpotAtThePriceTimeForCharts) {
  CurveProvider provider;
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(1);
  options.candles = std::make_shared<server::CandleStore>();
  server::Engine engine(provider, {{"QQQ", "SPX", "XSP"}}, options);
  engine.start();
  ASSERT_TRUE(eventually([&] { return engine.metrics("SPX") != nullptr; }));
  EXPECT_EQ(engine.candles(), options.candles.get());
  const auto bars = options.candles->bars("SPX", server::BarInterval::Minute, 10);
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_EQ(bars[0], (md::Bar{md::new_york_to_utc({2026, 9, 22}, 16, 0), 100, 100, 100, 100}));
  const auto response =
      server::handle_api({"GET", "/api/underlyings/SPX/candles?interval=1m"}, engine);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(nlohmann::json::parse(response.body)["bars"].size(), 1u);
}

TEST(Engine, ChartsEveryPrintBetweenAnalyticsPasses) {
  ManualProvider provider;
  server::Engine::Options options;
  options.analytics_interval = std::chrono::hours(1);
  options.candles = std::make_shared<server::CandleStore>();
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  const auto minute = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (const auto& [second, price] : std::vector<std::pair<int, double>>{{5, 100}, {20, 103}, {40, 98}, {55, 101}})
    provider.sink->publish(md::UnderlyingQuote{"SPX", minute + second * md::kNanosPerSecond, 0, 0, price});
  ASSERT_TRUE(eventually([&] { return engine.status().events == 4; }));
  EXPECT_EQ(engine.metrics("SPX"), nullptr);  // no analytics pass has run
  EXPECT_EQ(options.candles->bars("SPX", server::BarInterval::Minute, 10),
            (std::vector<md::Bar>{{minute, 100, 103, 98, 101}}));
}

TEST(Engine, ChartsTheParitySpotOfAnUnderlyingWithoutPrints) {
  CurveProvider provider;
  provider.quote_underlying = false;
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(1);
  options.candles = std::make_shared<server::CandleStore>();
  server::Engine engine(provider, {{"QQQ", "SPX", "XSP"}}, options);
  engine.start();
  ASSERT_TRUE(eventually([&] { return engine.metrics("SPX") != nullptr; }));
  EXPECT_EQ(engine.metrics("SPX")->spot_source, "parity");
  const auto bars = options.candles->bars("SPX", server::BarInterval::Minute, 10);
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_EQ(bars[0].start, md::new_york_to_utc({2026, 9, 22}, 16, 0));
  EXPECT_NEAR(bars[0].close, engine.metrics("SPX")->spot, 1e-9);
}
}  // namespace
