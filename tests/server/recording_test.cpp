#include "support/sanitizer.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <future>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

#include "openport/providers/replay.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/equity.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "openport/server/sandboxes.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;

class SyntheticProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "synthetic-engine"; }
  md::Capabilities capabilities() const noexcept override { return {}; }
  void start(const md::Subscription&, md::EventSink& out) override {
    sink = &out;
    for (int snapshot = 0; snapshot < 3; ++snapshot) {
      const auto ts = as_of + snapshot * md::kNanosPerSecond;
      const double spot = 5000 + snapshot;
      sink->publish(md::UnderlyingQuote{"SPX", ts, spot - .5, spot + .5, spot});
      md::InstrumentId id = 0;
      for (const auto date :
           {md::Date{2026, 10, 22}, md::Date{2026, 12, 22}, md::Date{2027, 9, 22}})
        for (double strike = 4500; strike <= 5500; strike += 25)
          for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
            auto contract = *md::parse_osi("SPXW261022C05000000");
            contract.expiry = date;
            contract.strike = strike;
            contract.type = type;
            if (snapshot == 0) sink->publish(md::ContractDefinition{id, contract});
            const auto years = md::years_between(ts, contract.expiry_time());
            const auto forward = spot * std::exp((.045 - .013) * years);
            const auto log_moneyness = std::log(strike / forward);
            const auto vol = .18 - .25 * log_moneyness + .9 * log_moneyness * log_moneyness;
            const auto price =
                pricing::black_price(type, forward, strike, years, vol, std::exp(-.045 * years));
            sink->publish(md::OptionQuote{id, ts, price - .05, price + .05, 10, 11});
            sink->publish(md::OpenInterest{id, ts, 500});
            sink->publish(md::VendorGreeks{id, ts, vol});
            sink->publish(md::OptionTrade{id, ts, price, 1});
            ++id;
          }
    }
    sink->publish(md::ProviderStatus{as_of, md::FeedState::Live, "complete", "SPX"});
  }
  void stop() override {
    if (sink) {
      sink->publish(md::ProviderStatus{as_of, md::FeedState::Stopped, "synthetic stopped", ""});
      sink = nullptr;
    }
  }
  const md::Timestamp as_of = md::new_york_to_utc({2026, 9, 22}, 15, 0);
  md::EventSink* sink = nullptr;
};

void same_analytics(const analytics::UnderlyingMetrics& expected,
                    const analytics::UnderlyingMetrics& actual) {
  EXPECT_EQ(expected.symbol, actual.symbol);
  EXPECT_EQ(expected.as_of, actual.as_of);
  test::exact_double(expected.spot, actual.spot);
  EXPECT_EQ(expected.spot_source, actual.spot_source);
  EXPECT_EQ(expected.options_priced, actual.options_priced);
  ASSERT_EQ(expected.slices.size(), actual.slices.size());
  for (std::size_t i = 0; i < expected.slices.size(); ++i) {
    const auto& a = expected.slices[i];
    const auto& b = actual.slices[i];
    EXPECT_EQ(a.expiry, b.expiry);
    test::exact_double(a.years, b.years);
    test::exact_double(a.forward.forward, b.forward.forward);
    test::exact_double(a.forward.discount, b.forward.discount);
    test::exact_double(a.atm_iv, b.atm_iv);
    ASSERT_EQ(a.strikes.size(), b.strikes.size());
    for (std::size_t j = 0; j < a.strikes.size(); ++j) {
      const auto& x = a.strikes[j];
      const auto& y = b.strikes[j];
      test::exact_double(x.strike, y.strike);
      test::exact_double(x.iv, y.iv);
      for (auto side : {&analytics::StrikeMetrics::call, &analytics::StrikeMetrics::put}) {
        const auto& c = x.*side;
        const auto& d = y.*side;
        test::exact_double(c.iv, d.iv);
        test::exact_double(c.bid_iv, d.bid_iv);
        test::exact_double(c.ask_iv, d.ask_iv);
        test::exact_double(c.open_interest, d.open_interest);
      }
    }
  }
}

TEST(EngineRecording, SyntheticSessionReplaysIdenticalSpotForwardsAndEveryExpiryIv) {
  test::RecordingFile file;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  options.analytics_interval = 1h;  // EOF and shutdown must still compute the final snapshot.
  std::atomic<md::Timestamp> receipt{10 * md::kNanosPerDay};
  options.clock = [&] { return receipt.fetch_add(1); };
  server::Engine original(provider, {{"SPX"}}, options);
  original.start();
  original.stop();
  ASSERT_TRUE(original.recording_error().empty());
  const auto expected = original.metrics("SPX");
  ASSERT_TRUE(expected);
  ASSERT_EQ(expected->slices.size(), 3u);
  ASSERT_EQ(expected->options_priced, 246);
  EXPECT_EQ(expected->as_of, provider.as_of + 2 * md::kNanosPerSecond);
  md::RecordingReader reader(file.path);
  EXPECT_FALSE(reader.header().market_controls);
  EXPECT_FALSE(reader.header().imported);
  EXPECT_EQ(reader.header().started, 10 * md::kNanosPerDay);
  md::Timestamp previous = reader.header().started;
  std::size_t count = 0;
  while (auto record = reader.next()) {
    EXPECT_GT(record->received, previous);
    previous = record->received;
    ++count;
  }
  EXPECT_EQ(count, original.recording_stats().events);
  EXPECT_GT(count, original.status().events);  // records survive downstream coalescing
  EXPECT_TRUE(reader.diagnostic().empty());
  providers::ReplayProvider replay({.file = file.path, .speed = 0, .loop = false, .clock = {}});
  EXPECT_FALSE(replay.market_controls());
  options.record_file = file.directory / "re-recorded.oprec";
  options.clock = [] { return md::now(); };  // deliberately a different wall clock
  server::Engine replayed(replay, {{"SPX"}}, options);
  replayed.start();
  ASSERT_TRUE(test::recording_eventually([&] {
    return replayed.status().feed_message.find("end of recording") != std::string::npos &&
           replayed.metrics("SPX");
  }));
  replayed.stop();
  EXPECT_TRUE(replayed.recording_error().empty());
  EXPECT_FALSE(md::RecordingReader(options.record_file).header().market_controls);
  same_analytics(*expected, *replayed.metrics("SPX"));
}

TEST(EngineRecording, ReplayPreservesMarketControlCapabilityFromItsSourceHeader) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.market_controls = true;
  const auto at = md::new_york_to_utc({2026, 9, 16}, 10, 0);
  test::record_events(file.path, {md::TradingHalt{at, at + md::kNanosPerMinute},
                                 md::SnapshotHeartbeat{"SPX", at}}, header);
  providers::ReplayProvider provider({.file = file.path, .speed = 0, .loop = false, .clock = {}});
  EXPECT_TRUE(provider.market_controls());
  server::Engine::Options options;
  options.paper_enabled = false;
  options.record_file = file.directory / "re-recorded.oprec";
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_TRUE(test::recording_eventually([&] {
    return engine.status().feed_message.find("end of recording") != std::string::npos;
  }));
  engine.stop();
  EXPECT_TRUE(engine.recording_error().empty());
  md::RecordingReader reader(options.record_file);
  EXPECT_TRUE(reader.header().market_controls);
  // Replay batches bypass the recording sink; this checks header selection only.
  while (reader.next()) {}
  EXPECT_TRUE(reader.diagnostic().empty());
}

// U4: uptime is wall time since the engine started, not time on the market clock a
// replay or the demo keeps (which put it at 107,099 s right after launch).
TEST(Engine, UptimeCountsWallTimeWhateverClockTheMarketKeeps) {
  using nlohmann::json;
  // A replay's clock reads 0 until its first event is published: uptime counts anyway.
  for (const md::Timestamp market : {md::new_york_to_utc({2020, 1, 2}, 9, 30), md::Timestamp{0}}) {
    SyntheticProvider provider;
    server::Engine::Options options;
    options.paper_enabled = false;
    options.clock = [market] { return market; };
    const auto launched = std::chrono::steady_clock::now();
    std::atomic<int> seconds{0};
    options.monotonic_clock = [&] { return launched + std::chrono::seconds(seconds.load()); };
    server::Engine engine(provider, {{"SPX"}}, options);
    EXPECT_EQ(json::parse(server::handle_api({"GET", "/api/status"}, engine).body)["engine"]["uptime_seconds"], 0);
    engine.start();
    seconds = 90;
    EXPECT_EQ(json::parse(server::handle_api({"GET", "/api/status"}, engine).body)["engine"]["uptime_seconds"], 90) << market;
    engine.stop();
  }
}

server::ApiResponse call(server::ReplayHost& host, std::string method, std::string target, std::string body = {},
                         std::chrono::seconds timeout = 5min) {
  auto promise = std::make_shared<std::promise<server::ApiResponse>>();
  auto future = promise->get_future();
  server::ApiRequest request{std::move(method), std::move(target), std::move(body)};
  request.content_type = "application/json";
  EXPECT_TRUE(host.handle(request, [promise](server::ApiResponse response) { promise->set_value(std::move(response)); }));
  if (future.wait_for(timeout) != std::future_status::ready) throw std::runtime_error("replay request did not complete");
  return future.get();
}

TEST(ReplayHost, ListsStartsTradesControlsAndStopsARecordedSession) {
  using nlohmann::json;
  test::RecordingFile file;
  const auto recording = file.directory / "synthetic.oprec";
  {
    SyntheticProvider provider;
    server::Engine::Options options;
    options.record_file = recording;
    options.paper_enabled = false;
    // Received a moment after each quote's market time, as a live feed would be.
    std::atomic<md::Timestamp> receipt{provider.as_of};
    options.clock = [&] { return receipt.fetch_add(1); };
    server::Engine original(provider, {{"SPX"}}, options);
    original.start();
    original.stop();
    ASSERT_TRUE(original.recording_error().empty());
  }
  server::Engine::Options base;
  base.analytics_interval = 1ms;
  server::ReplayHost host({file.directory, base});
  server::ApiRequest other{"GET", "/api/status"};
  EXPECT_FALSE(host.handle(other, [](server::ApiResponse) {}));
  EXPECT_FALSE(host.handle({"GET", "/api/replayed"}, [](server::ApiResponse) {}));

  auto listing = json::parse(call(host, "GET", "/api/replay").body);
  ASSERT_EQ(listing["recordings"].size(), 1);
  EXPECT_EQ(listing["recordings"][0]["file"], "synthetic.oprec");
  EXPECT_EQ(listing["recordings"][0]["provider"], "synthetic-engine");
  EXPECT_EQ(listing["recordings"][0]["imported"], false);
  EXPECT_EQ(listing["recordings"][0]["symbols"], json::array({"SPX"}));
  EXPECT_TRUE(listing["replay"].is_null());
  EXPECT_EQ(call(host, "GET", "/api/replay/status").status, 404);

  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "../synthetic.oprec"})").status, 404);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "speed": 3})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "plan": "funded-eod-50k"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "plan": "two-step-verify-25k"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "plan": "two-step-funded-25k"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "extra": 1})").status, 400);
  const auto started = call(host, "POST", "/api/replay", R"({"file": "synthetic.oprec", "speed": 0})");
  ASSERT_EQ(started.status, 201) << started.body;
  EXPECT_EQ(json::parse(started.body)["replay"]["file"], "synthetic.oprec");
  EXPECT_EQ(json::parse(started.body)["replay"]["speed"], 0);

  // The mirrored API serves the replay's own analytics and account.
  ASSERT_TRUE(test::recording_eventually([&] {
    return json::parse(call(host, "GET", "/api/replay").body)["replay"]["finished"] == true &&
           call(host, "GET", "/api/replay/underlyings/SPX/summary").status == 200;
  }));
  const auto status = json::parse(call(host, "GET", "/api/replay/status").body);
  EXPECT_EQ(status["trading"]["enabled"], true);
  EXPECT_EQ(status["provider"]["name"], "replay (synthetic-engine)");
  const auto symbol = *md::parse_osi("SPXW261022C05000000");
  const json order{{"client_order_id", "replayed"}, {"symbol", symbol.osi_symbol()}, {"side", "buy"},
                   {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
  const auto bought = call(host, "POST", "/api/replay/orders", order.dump());
  ASSERT_EQ(bought.status, 403) << bought.body;
  EXPECT_EQ(json::parse(bought.body)["error"]["code"], "REPLAY_READ_ONLY");

  const auto controlled = json::parse(call(host, "PUT", "/api/replay", R"({"speed": 60, "paused": true})").body);
  EXPECT_EQ(controlled["replay"]["speed"], 60);
  EXPECT_EQ(controlled["replay"]["paused"], true);
  EXPECT_EQ(call(host, "PUT", "/api/replay", R"({"speed": 7})").status, 400);
  EXPECT_EQ(call(host, "GET", "/api/replay?x=1").status, 400);
  const auto tick = json::parse(host.tick());
  EXPECT_EQ(tick["type"], "replay_tick");
  EXPECT_EQ(tick["replay"]["file"], "synthetic.oprec");
  EXPECT_EQ(tick["trading"]["enabled"], true);

  EXPECT_TRUE(json::parse(call(host, "DELETE", "/api/replay").body)["replay"].is_null());
  EXPECT_EQ(call(host, "GET", "/api/replay/status").status, 404);
  EXPECT_TRUE(host.tick().empty());
  EXPECT_EQ(call(host, "PUT", "/api/replay", R"({"paused": false})").status, 404);
}

/// The generated demo days of this process's replay hosts, in the temporary directory.
std::vector<std::filesystem::path> demo_directories() {
  std::vector<std::filesystem::path> found;
  const auto prefix = "openport-demo-" + std::to_string(::getpid()) + "-";
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path()))
    if (entry.path().filename().string().starts_with(prefix)) found.push_back(entry.path());
  return found;
}

TEST(ReplayHost, PreparesTheDemoWhenListedAndRemovesItsDaysWithTheHost) {
  using nlohmann::json;
  test::RecordingFile file;
  {
    server::ReplayHost host({file.directory, {}});
    EXPECT_TRUE(demo_directories().empty());  // nothing is generated until asked
    ASSERT_EQ(call(host, "GET", "/api/replay").status, 200);
    ASSERT_TRUE(test::recording_eventually([] {
      const auto directories = demo_directories();
      std::error_code ec;
      return directories.size() == 1 && std::filesystem::exists(directories[0] / "1.oprec", ec);
    }));
  }
  EXPECT_TRUE(demo_directories().empty());
}

TEST(ReplayHost, PlaysTheSimulatedDemoMarketWithItsOwnAccount) {
  using nlohmann::json;
  test::RecordingFile file;
  server::Engine::Options base;
  base.analytics_interval = 1ms;
  base.series = std::make_shared<server::SeriesStore>();
  server::ReplayHost host({file.directory, base});
  const auto listing = json::parse(call(host, "GET", "/api/replay").body);
  EXPECT_EQ(listing["demo"]["provider"], "demo");
  EXPECT_EQ(listing["demo"]["symbols"], json::array({"SPX", "SPY", "QQQ"}));
  EXPECT_EQ(listing["demo"]["id"], "reversal");
  ASSERT_EQ(listing["demos"].size(), 20);
  EXPECT_EQ(listing["demos"][4]["id"], "overnight");
  EXPECT_EQ(listing["demos"][4]["symbols"], json::array({"SPX"}));
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": "sideways"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": true, "file": "synthetic.oprec"})").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", "{}").status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"demo": 1})").status, 400);

  // Generated on start; at 1x the opening snapshot plays at once and the next waits 15 seconds.
  const auto started = call(host, "POST", "/api/replay", R"({"demo": true, "seed": "scenario", "plan": "intraday-25k"})", 60s);
  ASSERT_EQ(started.status, 201) << started.body;
  const auto replay = json::parse(started.body)["replay"];
  EXPECT_EQ(replay["demo"], true);
  EXPECT_EQ(replay["file"], "Demo market: Slide and rebound");
  EXPECT_EQ(replay["provider"], "demo");

  // Next month's SPY series are the last defined: wait until the one traded here is quoted.
  ASSERT_TRUE(test::recording_eventually([&] {
    const auto chain = call(host, "GET", "/api/replay/underlyings/SPY/chain?expiry=2026-10-16PM");
    if (chain.status != 200) return false;
    const auto parsed = json::parse(chain.body);
    for (const auto& row : parsed["strikes"])
      if (row["strike"] == 600 && row["call"].is_object() && row["call"]["ask"].is_number()) return true;
    return false;
  }));
  const auto status = json::parse(call(host, "GET", "/api/replay/status").body);
  EXPECT_EQ(status["series"]["enabled"], false);
  EXPECT_TRUE(base.series->rows("SPX", 0, md::now()).empty());
  EXPECT_EQ(status["provider"]["name"], "replay (demo)");
  EXPECT_EQ(status["provider"]["simulated"], true);
  EXPECT_EQ(status["trading"]["plan"], "Intraday 25K");
  const json order{{"client_order_id", "demo"}, {"symbol", md::parse_osi("SPY261016C00600000")->osi_symbol()},
                   {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
  const auto bought = call(host, "POST", "/api/replay/orders", order.dump());
  ASSERT_EQ(bought.status, 201) << bought.body;
  EXPECT_EQ(json::parse(bought.body)["order"]["status"], "filled");
  EXPECT_EQ(json::parse(host.tick())["replay"]["demo"], true);
  ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  // Started again, the day is already generated.
  const auto again = std::chrono::steady_clock::now();
  ASSERT_EQ(call(host, "POST", "/api/replay", R"({"demo": true, "seed": "scenario"})", 60s).status, 201);
  EXPECT_LT(std::chrono::steady_clock::now() - again, 500ms);
  host.stop();

  server::ReplayHost off({file.directory, base, false});
  EXPECT_TRUE(json::parse(call(off, "GET", "/api/replay").body)["demo"].is_null());
  EXPECT_EQ(call(off, "POST", "/api/replay", R"({"demo": true})").status, 404);
}

TEST(EngineRecording, ExistingFileFailsBeforeProviderStarts) {
  test::RecordingFile file;
  test::record_events(file.path, {});
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  server::Engine engine(provider, {{"SPX"}}, options);
  EXPECT_THROW(engine.start(), std::runtime_error);
  EXPECT_EQ(provider.sink, nullptr);
}

TEST(EngineRecording, WriteFailureStaysVisibleAfterHealthyQuotesAndCleanShutdown) {
  test::RecordingFile file;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  options.recording.frame_bytes = 1;
  options.recording.write = [writes = 0](int fd, std::span<const char> bytes) mutable {
    if (++writes > 1) throw std::runtime_error("injected ENOSPC");
    if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()))
      throw std::runtime_error("test write failed");
  };
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_TRUE(test::recording_eventually([&] { return !engine.recording_error().empty(); }));
  provider.sink->publish(md::ProviderStatus{1, md::FeedState::Live, "healthy", "SPX"});
  engine.stop();
  const auto status = engine.status();
  EXPECT_EQ(status.feed_state, md::FeedState::Error);
  EXPECT_NE(status.feed_message.find("ENOSPC"), std::string::npos);
  EXPECT_EQ(status.underlyings.at("SPX").state, md::FeedState::Error);
  EXPECT_TRUE(engine.metrics("SPX"));
}

TEST(EngineRecording, ConsumerLaunchFailureClosesRecordingAfterStoppingProvider) {
  test::RecordingFile file;
  SyntheticProvider provider;
  server::Engine::Options options;
  options.record_file = file.path;
  options.launch = [](auto) -> std::thread { throw std::runtime_error("injected launch failure"); };
  server::Engine engine(provider, {{"SPX"}}, options);
  EXPECT_THROW(engine.start(), std::runtime_error);
  EXPECT_EQ(provider.sink, nullptr);
  md::RecordingReader reader(file.path);
  std::optional<md::RecordedEvent> last;
  while (auto record = reader.next()) last = std::move(record);
  ASSERT_TRUE(last);
  EXPECT_EQ(std::get<md::ProviderStatus>(last->event).state, md::FeedState::Stopped);
  EXPECT_TRUE(reader.diagnostic().empty());
}

/// A reproducible storage/publish benchmark, with no timing threshold in CI.
/// 30,000 contracts (60 expiries, 250 strikes, call/put), 30 quote updates each.
TEST(RecordingPerformance, SyntheticChain) {
  test::RecordingFile file;
  auto publish = [](md::EventSink& sink) {
    sink.publish(md::UnderlyingQuote{"SPX", 1, 5000, 5001, 5000.5});
    auto contract = *md::parse_osi("SPXW261022C05000000");
    for (md::InstrumentId id = 0; id < 30000; ++id) {
      contract.expiry = md::date_from_days(md::days_since_epoch({2026, 10, 22}) + id / 500);
      contract.strike = 4000 + static_cast<double>((id / 2) % 250) * 10;
      contract.type = id % 2 == 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
      sink.publish(md::ContractDefinition{id, contract});
    }
    for (int update = 0; update < 30; ++update)
      for (md::InstrumentId id = 0; id < 30000; ++id) {
        const double price = 100 + static_cast<double>(id % 250) * .17 + update * .01;
        sink.publish(
            md::OptionQuote{id, 1'790'000'000 * md::kNanosPerSecond + update * md::kNanosPerSecond,
                            price, price + .1, 10, 11});
      }
  };
  constexpr double count = 930001;
  md::EventQueue baseline;
  auto started = std::chrono::steady_clock::now();
  publish(baseline);
  const double baseline_ns =
      std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() /
      count;
  md::EventQueue downstream;
  md::RecordingSink recorder(file.path, test::recording_header(), downstream, {});
  started = std::chrono::steady_clock::now();
  publish(recorder);
  const auto publish_finished = std::chrono::steady_clock::now();
  recorder.close();
  const auto finished = std::chrono::steady_clock::now();
  ASSERT_TRUE(recorder.error().empty()) << recorder.error();
  const auto stats = recorder.stats();
  EXPECT_EQ(stats.events, static_cast<std::uint64_t>(count));
  EXPECT_EQ(stats.bytes, std::filesystem::file_size(file.path));
  const double total_ns =
      std::chrono::duration<double, std::nano>(finished - started).count() / count;
  const double publish_ns =
      std::chrono::duration<double, std::nano>(publish_finished - started).count() / count;
  std::printf(
      "recording benchmark: events=%llu bytes=%llu bytes/event=%.3f sink_ns/event=%.1f "
      "publish_ns/event=%.1f drained_ns/event=%.1f baseline_ns/event=%.1f overhead_ns/event=%.1f\n",
      static_cast<unsigned long long>(stats.events), static_cast<unsigned long long>(stats.bytes),
      static_cast<double>(stats.bytes) / count,
      static_cast<double>(stats.publish_nanoseconds) / count, publish_ns, total_ns, baseline_ns,
      total_ns - baseline_ns);
  md::RecordingReader reader(file.path);
  std::uint64_t recovered = 0;
  while (reader.next()) ++recovered;
  EXPECT_EQ(recovered, stats.events);
  EXPECT_TRUE(reader.diagnostic().empty());
}

void drill_recording(const std::filesystem::path& path) {
  auto header = test::recording_header();
  const md::Date date{2026, 9, 16};
  header.started = md::new_york_to_utc(date, 9, 30);
  md::Timestamp now = header.started;
  md::RecordingSink::Options options;
  options.clock = [&] { return now; };
  test::DiscardEvents discard;
  md::RecordingSink sink(path, header, discard, options);
  std::vector<md::OptionContract> contracts;
  for (int i = 0; i < 7; ++i) {
    for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
      auto contract = *md::parse_osi("SPXW261022C06000000");
      contract.strike = 5925 + i * 25;
      contract.type = type;
      sink.publish(md::ContractDefinition{static_cast<md::InstrumentId>(contracts.size()), contract});
      contracts.push_back(contract);
    }
  }
  for (int minute = 0; minute <= 390; minute += 5) {
    now = header.started + minute * md::kNanosPerMinute;
    const double spot = 6000 + minute * .01;
    sink.publish(md::UnderlyingQuote{"SPX", now, 0, 0, spot});
    for (std::size_t i = 0; i < contracts.size(); ++i) {
      const auto& c = contracts[i];
      const auto years = md::years_between(now, c.expiry_time());
      const auto mid = pricing::black_price(c.type, spot * std::exp(.027 * years), c.strike, years, .15, std::exp(-.04 * years));
      const auto bid = std::floor(mid * 10) / 10;
      sink.publish(md::OptionQuote{static_cast<md::InstrumentId>(i), now, bid, bid + .2, 20, 20});
    }
    sink.publish(md::ProviderStatus{now, md::FeedState::Live, "fixture", "SPX"});
  }
  sink.close();
}

TEST(ReplayHost, CopiedSettingsVerifyAndSingleDayPendingSettingsRequireReset) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::ReplayHost::Options options;
  options.recordings = file.directory;
  options.demo = false;
  options.engine.paper_journal = file.directory / "main.jsonl";
  auto source = std::make_shared<server::TradingView>();
  source->config.limits.aggregate.dollar_delta = 250'000;
  source->config.guardrails.max_opening_trades = 3;
  options.settings_source = [source](std::string_view id) { return id == "main" ? source : nullptr; };
  server::ReplayHost host(options);
  const auto request = json{{"file", "session.oprec"}, {"plan", "eod-50k"}, {"copy_settings_from", "main"},
      {"paused", true}, {"start_at", "10:00"}, {"speed", 0}};
  auto bad = request;
  bad["copy_settings_from"] = "missing";
  EXPECT_EQ(call(host, "POST", "/api/replay", bad.dump()).status, 404);
  bad["copy_settings_from"] = 42;
  EXPECT_EQ(call(host, "POST", "/api/replay", bad.dump()).status, 400);
  const auto start = call(host, "POST", "/api/replay", request.dump());
  ASSERT_EQ(start.status, 201) << start.body;
  const auto id = json::parse(start.body)["replay"]["id"].get<std::string>();
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  auto risk = json::parse(call(host, "GET", "/api/replay/risk").body);
  EXPECT_EQ(risk["limits"]["aggregate"]["dollar_delta"], 250'000);
  EXPECT_EQ(risk["guardrails"]["max_opening_trades"], 3);
  EXPECT_EQ(risk["pending_requires_reset"], false);
  risk["limits"]["aggregate"]["dollar_delta"] = 500'000;
  const auto limits = call(host, "PUT", "/api/replay/risk/limits",
      json{{"expected_revision", risk["limits_revision"]}, {"limits", risk["limits"]}}.dump());
  ASSERT_EQ(limits.status, 200) << limits.body;
  risk = json::parse(limits.body);
  EXPECT_EQ(risk["pending_requires_reset"], true);
  EXPECT_EQ(json::parse(call(host, "GET", "/api/replay/risk").body)["pending_requires_reset"], true);
  risk["guardrails"]["max_opening_trades"] = 6;
  ASSERT_EQ(call(host, "PUT", "/api/replay/risk/guardrails",
      json{{"expected_revision", risk["limits_revision"]}, {"guardrails", risk["guardrails"]}}.dump()).status, 200);
  ASSERT_EQ(call(host, "POST", "/api/replay/account/reset", R"({"plan":"eod-50k","reason":"Apply queued settings"})").status, 200);
  risk = json::parse(call(host, "GET", "/api/replay/risk").body);
  EXPECT_EQ(risk["limits"]["aggregate"]["dollar_delta"], 500'000);
  EXPECT_EQ(risk["guardrails"]["max_opening_trades"], 6);
  EXPECT_EQ(risk["pending_requires_reset"], false);
  host.stop();
  source.reset();
  const auto verified = server::verify_run(file.directory / "replays" / (id + ".jsonl"));
  EXPECT_TRUE(verified.matched) << verified.message;
  const auto history = json::parse(call(host, "GET", "/api/replay/history/" + id + "/risk").body);
  EXPECT_EQ(history["limits"]["aggregate"]["dollar_delta"], 500'000);
}

TEST(ReplayHost, DrillsReachTheEngineBeforeTradingAndKeepTheirJournalsAcrossRestart) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  std::string id;
  json expected;
  {
    server::ReplayHost host({file.directory, base, false});
    for (const auto* time : {"9:30", "24:00", "15:60", "garbage", "08:00", "17:00", ""}) {
      const json body{{"file", "session.oprec"}, {"start_at", time}};
      EXPECT_EQ(call(host, "POST", "/api/replay", body.dump()).status, 400) << time;
    }
    const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","start_at":"15:00","paused":true,"speed":60,"plan":"intraday-25k"})");
    ASSERT_EQ(started.status, 201) << started.body;
    const auto state = json::parse(started.body)["replay"];
    id = state["id"];
    EXPECT_TRUE(state["durable"]);
    EXPECT_NE(id.find("2026-09-16-recording-15-00"), std::string::npos);
    const json order{{"client_order_id", "drill"}, {"symbol", md::parse_osi("SPXW261022C06000000")->osi_symbol()},
                     {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}};
    const auto early = call(host, "POST", "/api/replay/orders", order.dump());
    ASSERT_EQ(early.status, 409) << early.body;
    EXPECT_EQ(json::parse(early.body)["error"]["code"], "REPLAY_FAST_FORWARD");
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(call(host, "GET", "/api/replay").body)["replay"]["fast_forwarding"].get<bool>(); }));
    const auto replay = json::parse(call(host, "GET", "/api/replay").body)["replay"];
    EXPECT_TRUE(replay["paused"]);
    EXPECT_EQ(replay["time"], "2026-09-16T19:00:00.000Z");
    EXPECT_EQ(replay["speed"], 60);
    const auto account = json::parse(call(host, "GET", "/api/replay/account").body);
    EXPECT_EQ(account["time"], "2026-09-16T19:00:00.000Z");
    const auto fills = json::parse(call(host, "GET", "/api/replay/fills").body);
    EXPECT_TRUE(fills["fills"].empty());
    const auto chain = json::parse(call(host, "GET", "/api/replay/underlyings/SPX/summary").body);
    EXPECT_EQ(chain["as_of"], "2026-09-16T19:00:00.000Z");
    const auto candles = json::parse(call(host, "GET", "/api/replay/underlyings/SPX/candles?interval=1m&limit=500").body);
    EXPECT_GT(candles["bars"].size(), 50U);
    const auto bought = call(host, "POST", "/api/replay/orders", order.dump());
    ASSERT_EQ(bought.status, 201) << bought.body;
    EXPECT_EQ(json::parse(bought.body)["order"]["status"], "filled");
    expected = json::parse(call(host, "GET", "/api/replay/fills").body);
    EXPECT_TRUE(json::parse(call(host, "GET", "/api/replay").body)["history"].empty());
    ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
    const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
    ASSERT_EQ(history.size(), 1U) << history;
    EXPECT_FALSE(history[0].contains("error")) << history;
    EXPECT_EQ(history[0]["result"], "open");
    EXPECT_EQ(history[0]["plan"], "intraday-25k");
    EXPECT_EQ(history[0]["plan_name"], "Intraday 25K");
    EXPECT_TRUE(history[0]["pnl"].is_string());
    EXPECT_FALSE(std::filesystem::exists(base.paper_journal));
    EXPECT_EQ(call(host, "POST", "/api/replay/history/" + id + "/orders", order.dump()).status, 403);
  }
  server::ReplayHost restarted({file.directory, base, false});
  const auto history = json::parse(call(restarted, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0]["id"], id);
  EXPECT_EQ(json::parse(call(restarted, "GET", "/api/replay/history/" + id + "/fills").body), expected);
  EXPECT_EQ(call(restarted, "GET", "/api/replay/history/" + id + "/account").status, 200);
  EXPECT_EQ(call(restarted, "GET", "/api/replay/history/" + id + "/trades").status, 200);
  EXPECT_EQ(call(restarted, "DELETE", "/api/replay/history/" + id).status, 200);
  EXPECT_TRUE(json::parse(call(restarted, "GET", "/api/replay").body)["history"].empty());
  EXPECT_EQ(call(restarted, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})").status, 201);
  restarted.stop();
  const auto practice = json::parse(call(restarted, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(practice.size(), 1U);
  EXPECT_EQ(practice[0]["plan"], "practice");
  EXPECT_EQ(practice[0]["plan_name"], "Practice");
  EXPECT_FALSE(practice[0].contains("error")) << practice;
}

TEST(ReplayHost, AServerOfferingSandboxesStillStartsReplaysWithoutThem) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  server::Sandboxes::Options limits;
  limits.capacity = 2;
  base.sandboxes = std::make_shared<server::Sandboxes>(limits);
  server::ReplayHost host({file.directory, base, false});
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})");
  ASSERT_EQ(started.status, 201) << started.body;
  // Visitor sandboxes belong to the live demo: the replay neither advertises nor creates them.
  const auto status = call(host, "GET", "/api/replay/status");
  ASSERT_EQ(status.status, 200) << status.body;
  EXPECT_FALSE(json::parse(status.body).contains("sandboxes"));
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(call(host, "GET", "/api/replay").body)["replay"]["fast_forwarding"].get<bool>(); }));
  EXPECT_EQ(call(host, "POST", "/api/replay/sandboxes", "{}").status, 404);
  host.stop();
}

// B52, B32, B54: a delete takes a run's sidecars with it and works on a damaged
// journal; an unknown id is a 404 that names no file.
TEST(ReplayHost, HistoryDeletesDamagedRunsWithTheirSidecarsAndAnswersUnknownIds) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  const auto replays = file.directory / "replays";
  server::ReplayHost host({file.directory, base, false});
  std::vector<std::string> ids;
  for (int run = 0; run < 3; ++run) {
    const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","start_at":"10:00","paused":true,"speed":0})");
    ASSERT_EQ(started.status, 201) << started.body;
    ids.push_back(json::parse(started.body)["replay"]["id"]);
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"10:30"})").status, 200);
    ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  }
  for (const auto& id : ids) {
    ASSERT_TRUE(std::filesystem::exists(replays / (id + ".json")));
    ASSERT_TRUE(std::filesystem::exists(replays / (id + ".jsonl.equity.csv"))) << id;
  }
  // An edited record breaks the chain; a torn last line is what a full disk leaves.
  const auto edited = replays / (ids[1] + ".jsonl");
  std::string lines;
  { std::ifstream in(edited); lines.assign(std::istreambuf_iterator<char>(in), {}); }
  const auto second = lines.find('\n') + 1;
  lines.replace(lines.find("\"seq\":2", second), 7, "\"seq\":9");
  { std::ofstream out(edited, std::ios::trunc); out << lines; }
  { std::ofstream out(replays / (ids[2] + ".jsonl"), std::ios::app); out << "{\"seq\":"; }
  const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 3U);
  for (const auto& entry : history) {
    EXPECT_GT(entry.at("journal_size").at("bytes").get<std::uint64_t>(), 0u);
    EXPECT_GT(entry.at("verification_cost").at("estimated_seconds").get<std::uint64_t>(), 0u);
    const auto expected_cost = server::verification_cost(replays / (entry.at("id").get<std::string>() + ".jsonl"));
    EXPECT_EQ(entry.at("verification_cost"), expected_cost);
    EXPECT_EQ(entry.at("verification").at("cost"), expected_cost);
  }
  const json* torn_entry = nullptr;
  for (const auto& entry : history)
    if (entry.at("id") == ids[2]) torn_entry = &entry;
  ASSERT_NE(torn_entry, nullptr);
  EXPECT_EQ(torn_entry->at("torn"), true) << *torn_entry;
  EXPECT_EQ(torn_entry->at("mismatch"), true) << *torn_entry;
  const auto expected_bytes = torn_entry->at("journal").at("bytes").get<std::uint64_t>();
  const auto found_bytes = torn_entry->at("journal_found").at("bytes").get<std::uint64_t>();
  EXPECT_GT(found_bytes, expected_bytes);
  const auto integrity_message = torn_entry->at("integrity_message").get<std::string>();
  EXPECT_NE(integrity_message.find("expected " + std::to_string(expected_bytes) + " bytes, found " + std::to_string(found_bytes)),
            std::string::npos) << integrity_message;
  EXPECT_EQ(integrity_message.find("expected " + torn_entry->at("journal").at("transactions").dump() +
                                   " transactions, found " + torn_entry->at("journal_found").at("transactions").dump()),
            std::string::npos) << integrity_message;
  EXPECT_EQ(integrity_message.find("expected head " + torn_entry->at("journal").at("head").get<std::string>() + ", found " +
                                   torn_entry->at("journal_found").at("head").get<std::string>()),
            std::string::npos) << integrity_message;
  EXPECT_EQ(call(host, "GET", "/api/replay/history/" + ids[1]).status, 422);
  for (const auto& id : ids) {
    const auto deleted = call(host, "DELETE", "/api/replay/history/" + id);
    ASSERT_EQ(deleted.status, 200) << deleted.body;
    for (const auto* suffix : {".jsonl", ".json", ".jsonl.equity.csv", ".playbooks.json"})
      EXPECT_FALSE(std::filesystem::exists(replays / (id + suffix))) << id << suffix;
  }
  EXPECT_TRUE(json::parse(call(host, "GET", "/api/replay").body)["history"].empty());
  for (const auto* method : {"GET", "DELETE"}) {
    for (const auto* target : {"/api/replay/history/no-such-run", "/api/replay/history/no-such-run/fills", "/api/replay/history/..",
                               "/api/replay/history/"}) {
      const auto response = call(host, method, target);
      EXPECT_EQ(response.status, 404) << method << ' ' << target << ' ' << response.body;
      EXPECT_EQ(json::parse(response.body)["error"]["code"], "NOT_FOUND");
      EXPECT_EQ(response.body.find(file.directory.string()), std::string::npos) << response.body;
    }
  }
  // Another process's running run keeps its files.
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})");
  ASSERT_EQ(started.status, 201) << started.body;
  const std::string running = json::parse(started.body)["replay"]["id"];
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  server::Engine::Options other = base;
  server::ReplayHost second_host({file.directory, other, false});
  const auto refused = call(second_host, "DELETE", "/api/replay/history/" + running);
  EXPECT_EQ(refused.status, 409) << refused.body;
  EXPECT_EQ(json::parse(refused.body)["error"]["code"], "REPLAY_RUNNING");
  EXPECT_EQ(refused.body.find(file.directory.string()), std::string::npos) << refused.body;
  EXPECT_TRUE(std::filesystem::exists(replays / (running + ".jsonl")));
  host.stop();
}

// B53: a finished run's entry reports its final playback state and the plan's id,
// also when a crash left the metadata written at start.
TEST(ReplayHost, FinishedRunsListTheirFinalPlaybackStateAndPlanId) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({file.directory, base, false});
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","start_at":"15:50","paused":true,"speed":0,"plan":"intraday-25k"})");
  ASSERT_EQ(started.status, 201) << started.body;
  const auto start_state = json::parse(started.body)["replay"];
  EXPECT_TRUE(start_state["fast_forwarding"]);
  const std::string id = start_state["id"];
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"15:55"})").status, 200);
  const auto check = [&](const json& entry) {
    EXPECT_EQ(entry["finished"], true) << entry;
    EXPECT_EQ(entry["read_only"], true);
    EXPECT_EQ(entry["fast_forwarding"], false);
    EXPECT_EQ(entry["progress"], 1.0);
    EXPECT_EQ(entry["paused"], true);
    EXPECT_EQ(entry["settled_through"], "2026-09-16T19:55:00.000Z");
    EXPECT_EQ(entry["plan"], "intraday-25k");
    EXPECT_EQ(entry["plan_name"], "Intraday 25K");
    EXPECT_EQ(entry["plan_id"], "intraday-25k");
  };
  host.stop();
  auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  check(history[0]);
  // A crash leaves the metadata create wrote at the start.
  { std::ofstream out(file.directory / "replays" / (id + ".json"), std::ios::trunc); out << start_state.dump() << '\n'; }
  server::ReplayHost restarted({file.directory, base, false});
  history = json::parse(call(restarted, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  check(history[0]);
}

// F69: a run a crash interrupted could only be opened read-only. Resuming re-executes
// its recorded inputs against its journal and continues it, so the journal ends as if
// the run had never stopped.
TEST(ReplayHost, AnInterruptedDriverSixRunResumesWhereItStoppedWithTheSameJournal) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  const auto replays = file.directory / "replays";
  const auto crashed = file.directory / "crashed";
  std::filesystem::create_directories(crashed / "replays");
  std::string id;
  std::string uninterrupted;
  {
    server::ReplayHost host({file.directory, base, false});
    const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true,"speed":0})");
    ASSERT_EQ(started.status, 201) << started.body;
    id = json::parse(started.body)["replay"]["id"];
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"10:00"})").status, 200);
    const json order{{"client_order_id", "before-crash"}, {"symbol", md::parse_osi("SPXW261022C06000000")->osi_symbol()},
                     {"side", "buy"}, {"type", "limit"}, {"limit_price", "100.00"}, {"quantity", 1}, {"time_in_force", "gtc"}};
    const auto placed = call(host, "POST", "/api/replay/orders", order.dump());
    ASSERT_EQ(placed.status, 201) << placed.body;
    const json bought{{"client_order_id", "bought"}, {"symbol", md::parse_osi("SPXW261022P06000000")->osi_symbol()},
                      {"side", "buy"}, {"type", "market"}, {"quantity", 2}, {"time_in_force", "ioc"}};
    ASSERT_EQ(call(host, "POST", "/api/replay/orders", bought.dump()).status, 201);
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"10:30"})").status, 200);
    const auto inputs = server::run_inputs(trading::FileJournal::read((replays / (id + ".jsonl")).string()));
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(json::parse(inputs.front()).at("driver"), 6);
    // What a crash here leaves: the journal so far and the metadata written at the start.
    std::filesystem::copy_file(replays / (id + ".jsonl"), crashed / "replays" / (id + ".jsonl"));
    { std::ofstream out(crashed / "replays" / (id + ".json")); out << json::parse(started.body)["replay"].dump() << '\n'; }
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"11:00"})").status, 200);
    host.stop();
    std::ifstream in(replays / (id + ".jsonl"));
    uninterrupted.assign(std::istreambuf_iterator<char>(in), {});
  }
  server::Engine::Options other = base;
  other.paper_journal = crashed / "main.jsonl";
  server::ReplayHost host({file.directory, other, false});
  auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_EQ(history[0]["interrupted"], true) << history[0];
  EXPECT_EQ(call(host, "POST", "/api/replay", json{{"resume", id}, {"plan", "practice"}}.dump()).status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"resume":"no-such-run"})").status, 404);
  const auto resumed = call(host, "POST", "/api/replay", json{{"resume", id}, {"speed", 0}}.dump());
  ASSERT_EQ(resumed.status, 201) << resumed.body;
  EXPECT_EQ(json::parse(resumed.body)["replay"]["id"], id);
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  const auto state = json::parse(call(host, "GET", "/api/replay").body)["replay"];
  EXPECT_EQ(state["paused"], true) << state;
  EXPECT_EQ(state["time"], "2026-09-16T14:30:00.000Z");
  // The account is the one the run had: its position and resting order are back.
  const auto account = json::parse(call(host, "GET", "/api/replay/orders?status=open").body)["orders"];
  ASSERT_EQ(account.size(), 1U) << account;
  EXPECT_EQ(account[0]["client_order_id"], "before-crash");
  EXPECT_EQ(call(host, "POST", "/api/replay", json{{"resume", id}}.dump()).status, 409);
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"11:00"})").status, 200);
  ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  std::string continued;
  { std::ifstream in(crashed / "replays" / (id + ".jsonl")); continued.assign(std::istreambuf_iterator<char>(in), {}); }
  EXPECT_EQ(continued, uninterrupted);
  EXPECT_TRUE(server::verify_run(crashed / "replays" / (id + ".jsonl")).matched);
  history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  // A stopped run has ended, as one that played to its end has.
  EXPECT_EQ(history[0]["interrupted"], false);
  const auto final = trading::FileJournal::read((crashed / "replays" / (id + ".jsonl")).string());
  EXPECT_EQ(history[0]["journal"]["head"], final.head);
  EXPECT_EQ(history[0]["journal"]["transactions"], final.records.size());
  const auto ended = call(host, "POST", "/api/replay", json{{"resume", id}}.dump());
  EXPECT_EQ(ended.status, 409) << ended.body;
  EXPECT_EQ(json::parse(ended.body)["error"]["code"], "REPLAY_NOT_RESUMABLE");
}

TEST(ReplayHost, InterruptedMultiSessionRunKeepsSessionsDividendsAndVerifiesAtTheEnd) {
  const test::SanitizerScenarioScale scale;
  using nlohmann::json;
  test::RecordingFile file;
  const auto scenarios = file.directory / "scenarios";
  std::filesystem::create_directory(scenarios);
  { std::ofstream out(scenarios / "quarter.json"); out << R"({"id":"quarter","title":"Quarter","description":"Across an ex-date.",
    "symbols":["SPY"],"date":"2026-09-17","seed":1,"generator":1,"volatility":0,"iv_shift":0,"spot_vol":0,
    "sessions":[{"session":"regular","drift":[[1,0]]},{"session":"regular","drift":[[1,0]]},
    {"session":"regular","drift":[[1,0]],"dividends":[{"symbol":"SPY","per_share":2.25}]}]})"; }
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  base.analytics.deamericanize = false;
  base.dividends = {{"SPY", {2026, 9, 18}, trading::Money::parse("9.99")},
                    {"SPY", {2026, 9, 21}, trading::Money::parse("9.99")}};
  const auto crashed = file.directory / "crashed";
  std::filesystem::create_directories(crashed / "replays");
  std::string id, uninterrupted;
  json sessions;
  const auto ex = providers::demo_dividends({2026, 9, 18}, {2026, 9, 18}).front().per_share;
  {
    server::ReplayHost host({file.directory, base, true, scenarios});
    const auto started = call(host, "POST", "/api/replay", R"({"scenario":"quarter","paused":true,"speed":0,"seed":"scenario"})");
    ASSERT_EQ(started.status, 201) << started.body;
    const auto state = json::parse(started.body)["replay"];
    id = state["id"];
    sessions = state["sessions"];
    ASSERT_EQ(sessions.size(), 3U);
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
    const auto call_symbol = md::parse_osi("SPY260925C00580000")->osi_symbol();
    for (const auto& symbol : {call_symbol, md::parse_osi("SPY261016P00630000")->osi_symbol()}) {
      const auto bought = call(host, "POST", "/api/replay/orders", json{{"client_order_id", symbol}, {"symbol", symbol}, {"side", "buy"},
          {"quantity", 1}, {"type", "market"}, {"time_in_force", "ioc"}}.dump());
      ASSERT_EQ(bought.status, 201) << bought.body;
    }
    const auto exercised = call(host, "POST", "/api/replay/positions/exercise", json{{"symbol", call_symbol}, {"quantity", 1}}.dump());
    ASSERT_EQ(exercised.status, 200) << exercised.body;
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"2026-09-18T10:00"})").status, 200);
    const auto payments = json::parse(call(host, "GET", "/api/replay/trades").body)["dividends"];
    ASSERT_EQ(payments.size(), 1U);
    EXPECT_EQ(payments[0]["per_share"], ex.str());
    EXPECT_EQ(payments[0]["amount"], (ex * 100).str());
    const auto journal = file.directory / "replays" / (id + ".jsonl");
    std::filesystem::copy_file(journal, crashed / "replays" / (id + ".jsonl"));
    { std::ofstream out(crashed / "replays" / (id + ".json")); out << state.dump() << '\n'; }
    ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"2026-09-21T16:15"})").status, 200);
    ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick())["replay"]["finished"].get<bool>(); }));
    host.stop();
    { std::ifstream in(journal); uninterrupted.assign(std::istreambuf_iterator<char>(in), {}); }
  }
  base.paper_journal = crashed / "main.jsonl";
  // Recovery must use the start record, not a newly supplied server calendar.
  base.dividends.clear();
  server::ReplayHost host({file.directory, base, true, scenarios});
  const auto resumed = call(host, "POST", "/api/replay", json{{"resume", id}, {"speed", 300}}.dump());
  ASSERT_EQ(resumed.status, 201) << resumed.body;
  EXPECT_EQ(json::parse(resumed.body)["replay"]["sessions"], sessions);
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  // Resume paced playback just before Friday ends: max_gap must compress the
  // weekend, which would otherwise take almost thirteen minutes even at 300x.
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"16:14:45"})").status, 200);
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"paused":false})").status, 200);
  ASSERT_TRUE(test::recording_eventually([&] {
    return json::parse(host.tick())["replay"]["time"].get<std::string>() >= "2026-09-21T13:30:00.000Z";
  }));
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"paused":true})").status, 200);
  const auto payments = json::parse(call(host, "GET", "/api/replay/trades").body)["dividends"];
  ASSERT_EQ(payments.size(), 2U);
  EXPECT_EQ(payments[1]["per_share"], "2.25");
  EXPECT_EQ(payments[1]["amount"], "225.00");
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"16:15"})").status, 200);
  ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick())["replay"]["finished"].get<bool>(); }));
  host.stop();
  const auto journal = crashed / "replays" / (id + ".jsonl");
  std::string continued;
  { std::ifstream in(journal); continued.assign(std::istreambuf_iterator<char>(in), {}); }
  EXPECT_EQ(continued, uninterrupted);
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
}

// A run whose recorded inputs no longer match its recording stops trading as it
// resumes, and its journal is left as it was.
TEST(ReplayHost, AResumedRunThatDiffersFromItsRecordingStopsAndWritesNothing) {
  using nlohmann::json;
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  const auto replays = file.directory / "replays";
  server::ReplayHost host({file.directory, base, false});
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true,"speed":0})");
  ASSERT_EQ(started.status, 201) << started.body;
  const std::string id = json::parse(started.body)["replay"]["id"];
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  ASSERT_EQ(call(host, "PUT", "/api/replay", R"({"until":"10:00"})").status, 200);
  ASSERT_EQ(call(host, "DELETE", "/api/replay").status, 200);
  // A boundary that names another batch than the recording's: re-executing reaches it.
  auto recovery = trading::FileJournal::read((replays / (id + ".jsonl")).string());
  const auto changed = file.directory / "changed.jsonl";
  {
    auto journal = trading::FileJournal::create(changed.string());
    int boundaries = 0;
    for (const auto& record : recovery.records) {
      auto payload = json::parse(record.payload);
      if (record.type == "run_input") {
        for (auto& event : payload.at("events")) {
          auto& input = event.at("payload");
          if (event.at("type") == "run_input" && input.at("kind") == "boundary" && ++boundaries == 3)
            input["events"] = input.at("events").get<std::size_t>() + 1;
        }
      }
      journal->append(record.time, record.type, payload.dump());
    }
    ASSERT_GE(boundaries, 3);
  }
  std::filesystem::rename(changed, replays / (id + ".jsonl"));
  std::string before;
  { std::ifstream in(replays / (id + ".jsonl")); before.assign(std::istreambuf_iterator<char>(in), {}); }
  // The metadata a crash leaves.
  { std::ofstream out(replays / (id + ".json"), std::ios::trunc); out << json::parse(started.body)["replay"].dump() << '\n'; }
  const auto resumed = call(host, "POST", "/api/replay", json{{"resume", id}, {"speed", 0}}.dump());
  ASSERT_EQ(resumed.status, 201) << resumed.body;
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  const auto status = json::parse(call(host, "GET", "/api/replay/status").body);
  EXPECT_EQ(status["trading"]["enabled"], false);
  EXPECT_NE(status["trading"]["reason"].get<std::string>().find("cannot resume"), std::string::npos) << status["trading"];
  host.stop();
  std::string after;
  { std::ifstream in(replays / (id + ".jsonl")); after.assign(std::istreambuf_iterator<char>(in), {}); }
  EXPECT_EQ(after, before);
}

TEST(ReplayHost, PaperDisabledAndReadOnlyNeverCreateReplayJournals) {
  test::RecordingFile file;
  drill_recording(file.path);
  server::Engine::Options base;
  base.paper_journal = file.directory / "main.jsonl";
  base.paper_enabled = false;
  {
    server::ReplayHost host({file.directory, base, false});
    EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file":"session.oprec","paused":true})").status, 201);
    EXPECT_FALSE(std::filesystem::exists(file.directory / "replays"));
  }
  base.paper_enabled = true;
  base.write_mode = "disabled";
  server::ReplayHost host({file.directory, base, false});
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"file":"session.oprec"})").status, 403);
  EXPECT_FALSE(std::filesystem::exists(file.directory / "replays"));
}


TEST(ReplayHost, FreshAndExplicitSeedsAndScenarioDrillsUseABoundedCache) {
  using nlohmann::json;
  test::RecordingFile file;
  server::Engine::Options base;
  server::ReplayHost host({file.directory, base});
  std::vector<std::string> fresh;
  for (int i = 0; i < 2; ++i) {
    const auto started = call(host, "POST", "/api/replay", R"({"scenario":"overnight","paused":true})", 60s);
    ASSERT_EQ(started.status, 201) << started.body;
    const auto state = json::parse(started.body)["replay"];
    fresh.push_back(state["seed"]);
    EXPECT_EQ(state["generator"], 1);
    EXPECT_EQ(state["scenario"], "overnight");
  }
  EXPECT_NE(fresh[0], fresh[1]);
  for (const auto* seed : {"0", "18446744073709551615", "scenario"}) {
    const json body{{"scenario", "overnight"}, {"seed", seed}, {"paused", true}};
    const auto started = call(host, "POST", "/api/replay", body.dump(), 60s);
    ASSERT_EQ(started.status, 201) << started.body;
    EXPECT_EQ(json::parse(started.body)["replay"]["seed"], std::string(seed) == "scenario" ? "2026091600" : seed);
  }
  const auto directories = demo_directories();
  ASSERT_EQ(directories.size(), 1U);
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directories.front()))
    if (entry.path().extension() == ".oprec") ++count;
  EXPECT_LE(count, 4U);
  for (const auto& seed : {json(-1), json(1.5), json("18446744073709551616"), json("fresh")})
    EXPECT_EQ(call(host, "POST", "/api/replay", json{{"scenario", "overnight"}, {"seed", seed}}.dump()).status, 400);
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"scenario":"overnight","start_at":"19:00"})").status, 400);
  const auto started = call(host, "POST", "/api/replay", R"({"scenario":"overnight","seed":"scenario","start_at":"21:00","paused":true})", 60s);
  ASSERT_EQ(started.status, 201) << started.body;
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  EXPECT_EQ(json::parse(host.tick())["replay"]["time"], "2026-09-16T01:00:00.000Z");
  EXPECT_TRUE(json::parse(host.tick())["replay"]["paused"]);
  // U3: scenario: true plays the default scenario, as demo: true does.
  const auto default_day = call(host, "POST", "/api/replay", R"({"scenario":true,"seed":"scenario","paused":true})", 60s);
  ASSERT_EQ(default_day.status, 201) << default_day.body;
  EXPECT_EQ(json::parse(default_day.body)["replay"]["scenario"], "reversal");
  EXPECT_EQ(call(host, "POST", "/api/replay", R"({"scenario":7})").status, 400);
}


TEST(ReplayHost, SavedRunSummariesOutliveTheFullAccountCache) {
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.rules.plan = "Practice";
  for (int i = 0; i < 20; ++i) {
    const auto path = replays / ("run-" + std::to_string(i) + ".jsonl");
    const trading::TradingSession session(config, market.time, trading::FileJournal::create(path.string()));
  }
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto list = [&] { return json::parse(call(host, "GET", "/api/replay").body)["history"]; };
  const auto first = list();
  ASSERT_EQ(first.size(), 20U);
  for (const auto& item : first) {
    EXPECT_FALSE(item.contains("error")) << item;
    EXPECT_EQ(item["result"], "open");
    EXPECT_EQ(item["pnl"], "0.00");
    EXPECT_TRUE(item["valuation_complete"]);
    // Without a metadata sidecar the plan's id comes from its recorded name.
    EXPECT_EQ(item["plan"], "practice");
    EXPECT_EQ(item["plan_name"], "Practice");
    EXPECT_EQ(item["time"], md::format_timestamp(market.time));
  }
  EXPECT_EQ(host.history_recoveries(), 20U);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 20U);

  // Opening more full archives than fit in their cache must not evict summaries.
  for (int i = 0; i < 20; ++i)
    ASSERT_EQ(call(host, "GET", "/api/replay/history/run-" + std::to_string(i) + "/account").status, 200);
  EXPECT_EQ(host.history_recoveries(), 40U);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 40U);

  const auto changed = replays / "run-0.jsonl";
  const auto modified = std::filesystem::last_write_time(changed) + 1s;
  std::filesystem::last_write_time(changed, modified);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 41U);
  EXPECT_EQ(list(), first);
  EXPECT_EQ(host.history_recoveries(), 41U);

  std::string pnl;
  {
    auto session = trading::TradingSession::recover(trading::FileJournal::read(changed.string()),
                                                    trading::FileJournal::resume(changed.string()));
    market.next();
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("buy"), market.time).decision.ok());
    pnl = (session.snapshot()->equity - session.config().initial_cash).str();
  }
  // Preserve mtime to prove size changes alone invalidate a cached summary.
  std::filesystem::last_write_time(changed, modified);
  const auto updated = list();
  EXPECT_EQ(host.history_recoveries(), 42U);
  EXPECT_NE(updated, first);
  for (const auto& item : updated) if (item["id"] == "run-0") {
    EXPECT_EQ(item["pnl"], pnl);
    EXPECT_NE(item["pnl"], "0.00");
    EXPECT_EQ(item["time"], md::format_timestamp(market.time));
  }
  EXPECT_EQ(list(), updated);
  EXPECT_EQ(host.history_recoveries(), 42U);
  EXPECT_EQ(call(host, "DELETE", "/api/replay/history/run-0").status, 200);
  EXPECT_EQ(list().size(), 19U);
  EXPECT_EQ(host.history_recoveries(), 42U);
}

TEST(ReplayHost, OlderJournalRecordsNeedNoReplayFieldsAndStillCompactAndRepair) {
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  const auto journal = replays / "older.jsonl";
  std::string expected;
  {
    test::ScriptedMarket market;
    trading::TradingSession session({}, market.time, trading::FileJournal::create(file.path.string()));
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("buy"), market.time).decision.ok());
    expected = session.snapshot_json();
  }
  // Expand to the whole-state records older builds wrote; no replay sidecar.
  trading::TradingSession::expand(trading::FileJournal::read(file.path.string()), *trading::FileJournal::create(journal.string()));
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto history = json::parse(call(host, "GET", "/api/replay").body)["history"];
  ASSERT_EQ(history.size(), 1U);
  EXPECT_FALSE(history[0].contains("error")) << history;
  EXPECT_EQ(history[0]["result"], "open");
  EXPECT_EQ(call(host, "GET", "/api/replay/history/older/account").status, 200);
  const auto compacted = server::compact_paper_journals(options.paper_journal, {});
  ASSERT_EQ(compacted.size(), 1U);
  EXPECT_TRUE(compacted[0].error.empty());
  EXPECT_FALSE(compacted[0].backup.empty());
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(journal.string())).snapshot_json(), expected);
  { std::ofstream tail(journal, std::ios::app); tail << "{torn"; }
  const auto repaired = trading::FileJournal::repair(journal.string());
  EXPECT_GT(repaired.bytes_cut, 0U);
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(journal.string())).snapshot_json(), expected);
  EXPECT_EQ(call(host, "GET", "/api/replay/history/older/fills").status, 200);
}

TEST(ReplayHost, FinishedRunsServeTheirEquityHistoryWithoutRewritingIt) {
  // B22: the archive recovered the account from its journal but never read the
  // equity history beside it, so a finished run's Dashboard had no chart.
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  const auto journal = replays / "finished.jsonl";
  test::ScriptedMarket market;
  {
    trading::TradingSession session({}, market.time, trading::FileJournal::create(journal.string()));
    market.seed(session);
    ASSERT_TRUE(session.submit(market.market("buy"), market.time).decision.ok());
  }
  const auto history = journal.string() + ".equity.csv";
  {
    server::EquityStore store(history);
    store.append({market.time, 1, trading::Money::parse("100000"), {}, trading::Money::parse("100000"), {}, {}, 0});
    store.append({market.time, 1, trading::Money::parse("99989.35"), {}, trading::Money::parse("100000"), {}, {}, 1});
    store.append({market.time + md::kNanosPerMinute, 1, trading::Money::parse("99979.35"), {}, trading::Money::parse("100000"), {}, {}, 0});
  }
  { std::ofstream torn(history, std::ios::app); torn << "torn"; }
  const auto bytes = std::filesystem::file_size(history);
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto response = call(host, "GET", "/api/replay/history/finished/account/equity");
  ASSERT_EQ(response.status, 200) << response.body;
  const auto body = json::parse(response.body);
  ASSERT_EQ(body["samples"].size(), 3U) << body;
  EXPECT_EQ(body["samples"][1]["equity"], "99989.35");
  EXPECT_EQ(body["samples"][1]["fill"], "1");
  EXPECT_EQ(body["samples"][2]["time"], md::format_timestamp(market.time + md::kNanosPerMinute));
  EXPECT_TRUE(body["error"].is_string());  // the torn row is reported, not repaired
  EXPECT_TRUE(body["error_time"].is_string());
  EXPECT_FALSE(body["error_recovered"].get<bool>());
  const auto first = json::parse(call(host, "GET", "/api/replay/history/finished/account/equity?limit=1").body);
  ASSERT_TRUE(first["next"].is_string());
  const auto second = json::parse(call(host, "GET", "/api/replay/history/finished/account/equity?limit=2&cursor=" +
      first["next"].get<std::string>()).body);
  ASSERT_EQ(second["samples"].size(), 2U);
  EXPECT_EQ(first["samples"][0], body["samples"][0]);
  EXPECT_EQ(second["samples"][0], body["samples"][1]);
  EXPECT_EQ(second["samples"][1], body["samples"][2]);
  EXPECT_TRUE(second["next"].is_null());
  const auto bounded = json::parse(call(host, "GET", "/api/replay/history/finished/account/equity?from=" +
      md::format_timestamp(market.time + md::kNanosPerMinute)).body);
  EXPECT_EQ(bounded["samples"].size(), 1U);
  EXPECT_EQ(std::filesystem::file_size(history), bytes);
}

TEST(ReplayHost, AnArchivedRunsOpenOrdersEndWithTheRun) {
  // F69: a stopped or finished run showed its resting GTC limit and armed stop as
  // working forever, in an account that can never fill or cancel them.
  using nlohmann::json;
  test::RecordingFile file;
  const auto replays = file.directory / "replays";
  std::filesystem::create_directory(replays);
  const auto journal = replays / "ended.jsonl";
  test::ScriptedMarket market;
  {
    trading::TradingSession session({}, market.time, trading::FileJournal::create(journal.string()));
    market.seed(session);
    ASSERT_TRUE(session.submit(market.limit("resting", 1, "3.90", trading::Side::Buy, trading::TimeInForce::Gtc), market.time).decision.ok());
    auto stop = market.market("stop");
    stop.trigger = trading::Trigger{trading::TriggerSource::Option, trading::TriggerDirection::AtOrAbove, trading::Money::parse("5.00")};
    ASSERT_TRUE(session.submit(stop, market.time).decision.ok());
    ASSERT_TRUE(session.submit(market.market("filled"), market.time).decision.ok());
    ASSERT_EQ(session.snapshot()->open_orders.size(), 2U);
  }
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({{}, options, false});
  const auto response = call(host, "GET", "/api/replay/history/ended/orders");
  ASSERT_EQ(response.status, 200) << response.body;
  const auto orders = json::parse(response.body)["orders"];
  ASSERT_EQ(orders.size(), 3U) << orders;
  for (const auto& order : orders) {
    if (order["client_order_id"] == "filled") {
      EXPECT_EQ(order["status"], "filled");
      continue;
    }
    EXPECT_EQ(order["status"], "cancelled") << order;
    EXPECT_EQ(order["reason"]["code"], "RUN_ENDED") << order;
  }
  EXPECT_TRUE(json::parse(call(host, "GET", "/api/replay/history/ended/orders?status=open").body)["orders"].empty());
  // The journal keeps them as they were.
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(journal.string())).snapshot()->open_orders.size(), 2U);
}

TEST(ReplayHost, ImportedHeaderIsListedAndRetainedInActiveReplay) {
  using namespace openport;
  using nlohmann::json;
  test::RecordingFile file;
  auto header = test::recording_header();
  header.provider = "thetadata";
  header.imported = true;
  header.started = md::new_york_to_utc({2026, 9, 22}, 9, 30);
  header.subscription = {{"SPY"}};
  header.capabilities.delay = std::chrono::seconds(0);
  header.capabilities.poll_interval = std::chrono::seconds(0);
  test::record_events(file.path,
      {md::ContractDefinition{0, *md::parse_osi("SPY260923C00500000")},
       md::OptionQuote{0, header.started, 5, 5.2, 10, 10}}, header,
      {.clock = [&] { return header.started; }});
  server::Engine::Options base;
  base.paper_enabled = false;
  server::ReplayHost host({file.directory, base, false});
  auto listing = json::parse(call(host, "GET", "/api/replay").body);
  ASSERT_EQ(listing["recordings"].size(), 1U);
  EXPECT_EQ(listing["recordings"][0]["imported"], true);
  EXPECT_EQ(listing["recordings"][0]["provider"], "thetadata");
  EXPECT_EQ(listing["recordings"][0]["simulated"], false);
  const auto started = call(host, "POST", "/api/replay", R"({"file":"session.oprec","speed":0})");
  ASSERT_EQ(started.status, 201) << started.body;
  EXPECT_EQ(json::parse(started.body)["replay"]["imported"], true);
  host.stop();
}

}  // namespace
