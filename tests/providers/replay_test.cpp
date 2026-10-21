#include "openport/providers/replay.hpp"

#include <condition_variable>
#include <fstream>

#include "openport/providers/factory.hpp"
#include "support/recording.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;

class TestClock final : public providers::ReplayClock {
 public:
  TimePoint now() override { return TimePoint{} + 100s; }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex);
    if (deadlines.size() == block_after) wake.wait(lock, [&] { return stop.load(); });
    if (stop) return false;
    deadlines.push_back(deadline - now());
    return true;
  }
  void interrupt() override {
    const std::lock_guard lock(mutex);
    wake.notify_all();
  }
  std::vector<TimePoint::duration> waits() {
    const std::lock_guard lock(mutex);
    return deadlines;
  }
  std::size_t block_after = std::numeric_limits<std::size_t>::max();

 private:
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<TimePoint::duration> deadlines;
};

std::vector<md::Event> session() {
  return {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
          md::OptionQuote{0, 50, 100, 101, 10, 10},
          md::OptionTrade{0, 51, 100.5, 1},
          md::OpenInterest{0, 49, 120},
          md::VendorGreeks{0, 52, .25, .5, .2, .3, -.2, .1},
          md::UnderlyingQuote{"SPX", 53, 5000, 5001, 5000.5},
          md::ProviderStatus{54, md::FeedState::Delayed, "complete", "SPX"}};
}

bool ended(const test::EventCollector& sink) {
  const auto events = sink.snapshot();
  if (events.empty()) return false;
  const auto* status = std::get_if<md::ProviderStatus>(&events.back());
  return status &&
         (status->state == md::FeedState::Stopped || status->state == md::FeedState::Error);
}

TEST(Replay, PreservesOrderMarketTimeCapabilitiesAndAbsoluteTimingAtEverySpeed) {
  test::RecordingFile file;
  const auto events = session();
  md::RecordingSink::Options recording;
  md::Timestamp received = 100000;
  recording.clock = [&] {
    received += 60 * md::kNanosPerSecond;
    return received;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  for (const int speed : {1, 10, 60, 0}) {
    auto clock = std::make_shared<TestClock>();
    providers::ReplayProvider replay({file.path, speed, false, clock});
    EXPECT_EQ(replay.name(), "replay (synthetic)");
    const auto caps = replay.capabilities();
    EXPECT_EQ(caps.poll_interval, 15s);
    EXPECT_EQ(caps.delay, 900s);
    EXPECT_TRUE(caps.realtime && caps.realtime_plan_dependent && caps.quotes && caps.trades &&
                caps.open_interest && caps.vendor_greeks && caps.history);
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    const auto actual = sink.snapshot();
    ASSERT_EQ(actual.size(), events.size() + 1);
    for (std::size_t i = 0; i < events.size(); ++i) test::exact_event(events[i], actual[i]);
    const auto waits = clock->waits();
    ASSERT_EQ(waits.size(), speed == 0 ? 0u : events.size());
    for (std::size_t i = 0; i < waits.size(); ++i)
      EXPECT_EQ(waits[i], std::chrono::seconds(60 * static_cast<int>(i) / speed));
    EXPECT_EQ(std::get<md::ProviderStatus>(actual.back()).state, md::FeedState::Stopped);
  }
}

TEST(Replay, FiltersEverySymbolScopedEventAndKeepsGapsAcrossFilteredEvents) {
  test::RecordingFile file;
  std::vector<md::Event> events{md::ContractDefinition{0, *md::parse_osi("SPY261022C00500000")},
                                md::OptionQuote{0},
                                md::OptionTrade{0},
                                md::OpenInterest{0},
                                md::VendorGreeks{0},
                                md::UnderlyingQuote{"SPY"},
                                md::ProviderStatus{0, md::FeedState::Live, "SPY", "SPY"},
                                md::ContractDefinition{1, *md::parse_osi("SPXW261022C05000000")},
                                md::OptionQuote{1},
                                md::ProviderStatus{0, md::FeedState::Error, "global", ""},
                                md::ContractDefinition{1, *md::parse_osi("SPY261022C00500000")},
                                md::OptionQuote{1}};
  md::RecordingSink::Options recording;
  md::Timestamp receipt = 0;
  recording.clock = [&] {
    receipt += md::kNanosPerSecond;
    return receipt;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  auto clock = std::make_shared<TestClock>();
  providers::ReplayProvider replay({file.path, 1, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return sink.snapshot().size() == 4; }));
  replay.stop();
  const auto actual = sink.snapshot();
  ASSERT_EQ(actual.size(), 4u);
  for (std::size_t i = 0; i < 3; ++i) test::exact_event(events[7 + i], actual[i]);
  const auto waits = clock->waits();
  ASSERT_EQ(waits.size(), 3u);
  EXPECT_EQ(waits[0], 7s);
  EXPECT_EQ(waits[1], 8s);
  EXPECT_EQ(waits[2], 9s);
}

TEST(Replay, ReRecordingKeepsTheSourceVersion) {
  for (const bool venues : {false, true}) {
    test::RecordingFile file;
    auto header = test::recording_header();
    header.trade_venues = venues;
    test::record_events(file.path, session(), header);
    providers::ReplayProvider replay({.file = file.path, .speed = 1, .loop = false, .clock = {}});
    // A replay of a v5 recording records v5 again; one without venues keeps its older version.
    EXPECT_EQ(replay.trade_venues(), venues);
    EXPECT_FALSE(replay.market_controls());
  }
}

TEST(Replay, UnknownSymbolsFailSynchronouslyAndListAvailableSymbols) {
  test::RecordingFile file;
  test::record_events(file.path, session());
  providers::ReplayProvider replay({.file = file.path, .speed = 1, .loop = false, .clock = {}});
  test::EventCollector sink;
  try {
    replay.start({{"QQQ"}}, sink);
    FAIL();
  } catch (const std::invalid_argument& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("QQQ"), std::string::npos);
    EXPECT_NE(message.find("SPX, SPY"), std::string::npos);
  }
  EXPECT_TRUE(sink.snapshot().empty());
}

TEST(Replay, LoopsFromDefinitionsWithTheSameTimingAndStopsPromptly) {
  test::RecordingFile file;
  const auto events = session();
  md::RecordingSink::Options recording;
  md::Timestamp receipt = 0;
  recording.clock = [&] {
    receipt += md::kNanosPerSecond;
    return receipt;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  auto clock = std::make_shared<TestClock>();
  // One extra wait at each loop boundary preserves its trailing filtered gap.
  clock->block_after = 3 * (events.size() + 1);
  providers::ReplayProvider replay({file.path, 10, true, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(
      test::recording_eventually([&] { return sink.snapshot().size() >= 3 * events.size(); }));
  replay.stop();
  const auto actual = sink.snapshot();
  ASSERT_EQ(actual.size(), 3 * events.size() + 1);
  for (std::size_t i = 0; i < 3 * events.size(); ++i)
    test::exact_event(events[i % events.size()], actual[i]);
  const auto waits = clock->waits();
  for (std::size_t i = 0; i < waits.size(); ++i)
    EXPECT_EQ(waits[i], 100ms * std::min(i % (events.size() + 1), events.size() - 1));
}

TEST(Replay, StopInterruptsHoursLongWaitAtEveryPacedSpeedAndMaxLoop) {
  test::RecordingFile file;
  md::RecordingSink::Options recording;
  md::Timestamp receipt = 0;
  recording.clock = [&] {
    receipt += md::kNanosPerDay;
    return receipt;
  };
  test::record_events(file.path, session(), test::recording_header(), recording);
  for (int speed : {1, 10, 60, 0}) {
    providers::ReplayProvider replay({.file = file.path, .speed = speed, .loop = true, .clock = {}});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return !sink.snapshot().empty(); }));
    const auto started = std::chrono::steady_clock::now();
    replay.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_TRUE(ended(sink));
    replay.stop();
    EXPECT_THROW(replay.start({{"SPX"}}, sink), std::logic_error);
  }
}

TEST(Replay, TruncatedInputReportsRecoveryAndNeverLoopsIt) {
  test::RecordingFile file;
  test::record_events(file.path, session());
  std::filesystem::resize_file(file.path, std::filesystem::file_size(file.path) - 1);
  auto clock = std::make_shared<TestClock>();
  providers::ReplayProvider replay({file.path, 0, true, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
  replay.stop();
  const auto events = sink.snapshot();
  ASSERT_EQ(events.size(), session().size() + 1);
  const auto& status = std::get<md::ProviderStatus>(events.back());
  EXPECT_EQ(status.state, md::FeedState::Stopped);
  EXPECT_NE(status.message.find("truncated"), std::string::npos);
}

TEST(Replay, EmptyFileTerminatesEvenWhenLoopingAndUndefinedContractsFailClosed) {
  for (bool undefined : {false, true}) {
    test::RecordingFile file;
    test::record_events(file.path, undefined ? std::vector<md::Event>{md::OptionQuote{42}}
                                             : std::vector<md::Event>{});
    providers::ReplayProvider replay({.file = file.path, .speed = 0, .loop = true, .clock = {}});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    ASSERT_EQ(sink.snapshot().size(), 1u);
    EXPECT_EQ(std::get<md::ProviderStatus>(sink.snapshot()[0]).state,
              undefined ? md::FeedState::Error : md::FeedState::Stopped);
  }
}

TEST(Replay, BackwardAndExtremeReceiptTimesNeverOverflowOrReorder) {
  test::RecordingFile file;
  const std::vector<md::Timestamp> receipts{std::numeric_limits<md::Timestamp>::max(),
                                            std::numeric_limits<md::Timestamp>::min(),
                                            std::numeric_limits<md::Timestamp>::max()};
  std::size_t index = 0;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return receipts[index++]; };
  const std::vector<md::Event> events{md::UnderlyingQuote{"SPX", 1}, md::UnderlyingQuote{"SPX", 2},
                                      md::UnderlyingQuote{"SPX", 3}};
  test::record_events(file.path, events, test::recording_header(), recording);
  auto clock = std::make_shared<TestClock>();
  providers::ReplayProvider replay({file.path, 1, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
  replay.stop();
  const auto waits = clock->waits();
  ASSERT_EQ(waits.size(), 3u);
  EXPECT_EQ(waits[0], 0s);
  EXPECT_EQ(waits[1], 0s);
  EXPECT_EQ(waits[2], providers::ReplayClock::TimePoint::max() - clock->now());
  for (std::size_t i = 0; i < events.size(); ++i) test::exact_event(events[i], sink.snapshot()[i]);
}

TEST(Replay, FactoryRejectsUnknownKeysBadValuesMissingFilesAndUnsupportedFilters) {
  test::RecordingFile file;
  test::record_events(file.path, {});
  EXPECT_THROW((void)providers::make_provider({"replay", "", {}}), std::invalid_argument);
  for (const auto& [key, value] :
       std::vector<std::pair<std::string, std::string>>{{"typo", "1"},
                                                        {"speed", "0"},
                                                        {"speed", "2"},
                                                        {"speed", "1x"},
                                                        {"speed", "nan"},
                                                        {"loop", "true"},
                                                        {"poll_seconds", "1"}})
    EXPECT_THROW((void)providers::make_provider(
                     {"replay", "", {{"file", file.path.string()}, {key, value}}}),
                 std::invalid_argument);
  EXPECT_THROW(
      (void)providers::make_provider({"replay", "", {{"file", "/missing/openport.oprec"}}}),
      std::runtime_error);
  auto provider = providers::make_provider(
      {"replay", "", {{"file", file.path.string()}, {"speed", "max"}, {"loop", "off"}}});
  EXPECT_EQ(provider->name(), "replay (synthetic)");
  EXPECT_THROW(providers::validate_subscription("replay", {{"SPX"}, 1, 0}), std::invalid_argument);
  EXPECT_THROW(providers::validate_subscription("replay", {{"SPX"}, 0, .1}), std::invalid_argument);
}
TEST(Replay, SpeedScalingCarriesFractionalNanosecondsWithoutAccumulatingDrift) {
  test::RecordingFile file;
  std::vector<md::Event> events(121, md::UnderlyingQuote{"SPX", 1});
  md::Timestamp receipt = 0;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return ++receipt; };
  test::record_events(file.path, events, test::recording_header(), recording);
  for (int speed : {10, 60}) {
    auto clock = std::make_shared<TestClock>();
    providers::ReplayProvider replay({file.path, speed, false, clock});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    const auto waits = clock->waits();
    ASSERT_EQ(waits.size(), events.size());
    for (std::size_t i = 0; i < waits.size(); ++i)
      EXPECT_EQ(waits[i], std::chrono::nanoseconds(i / static_cast<std::size_t>(speed)));
  }
}


/// A clock that moves only when the test advances it; waits block until then.
class ManualClock final : public providers::ReplayClock {
 public:
  TimePoint now() override {
    const std::lock_guard lock(mutex);
    return current;
  }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex);
    ++generation;
    waiting = deadline;
    changed.notify_all();
    changed.wait(lock, [&] { return !wait_held && (stop.load() || current >= deadline); });
    waiting.reset();
    return !stop.load();
  }
  void interrupt() override {
    const std::lock_guard lock(mutex);
    changed.notify_all();
  }
  void advance(TimePoint::duration by) {
    const std::lock_guard lock(mutex);
    current += by;
    changed.notify_all();
  }
  /// Keep the provider inside its current wait while controls change.
  void hold_wait(bool held) {
    const std::lock_guard lock(mutex);
    wait_held = held;
    changed.notify_all();
  }
  std::size_t wait_generation() {
    const std::lock_guard lock(mutex);
    return generation;
  }
  /// Capture the generation before changing controls/time; inspect the next wait's
  /// actual deadline, so a wrong deadline fails rather than being filtered out.
  std::optional<TimePoint> waiting_for(std::size_t after = 0) {
    std::unique_lock lock(mutex);
    if (!changed.wait_for(lock, 5min, [&] { return waiting && generation > after; }))
      return std::nullopt;
    return waiting;
  }

 private:
  std::mutex mutex;
  std::condition_variable changed;
  TimePoint current{};
  std::optional<TimePoint> waiting;
  std::size_t generation = 0;
  bool wait_held = false;
};

/// Four events a minute apart, then one eight hours later: an overnight close.
test::RecordingFile paced_recording() {
  test::RecordingFile file;
  std::vector<md::Event> events = {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
                                   md::OptionQuote{0, 50, 100, 101, 10, 10}, md::OptionQuote{0, 51, 100, 101, 9, 9},
                                   md::OptionQuote{0, 52, 100, 101, 8, 8}, md::OptionQuote{0, 53, 100, 101, 7, 7}};
  md::RecordingSink::Options recording;
  md::Timestamp received = 1'000 * md::kNanosPerSecond;
  std::size_t index = 0;
  recording.clock = [&] {
    const auto now = received;
    received += index++ == 3 ? 8 * 3600 * md::kNanosPerSecond : 60 * md::kNanosPerSecond;
    return now;
  };
  test::record_events(file.path, events, test::recording_header(), recording);
  return file;
}

std::size_t quotes(const test::EventCollector& sink) {
  std::size_t count = 0;
  for (const auto& event : sink.snapshot()) count += std::holds_alternative<md::OptionQuote>(event);
  return count;
}

TEST(Replay, ASpeedChangeRescalesTheWaitInProgressAndThePausedTimeDoesNotCount) {
  const auto file = paced_recording();
  auto clock = std::make_shared<ManualClock>();
  std::atomic<bool> pause_observed{false};
  providers::ReplayProvider::Options options{file.path, 1, false, clock};
  options.synchronize = [&] {
    pause_observed = true;
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  };
  providers::ReplayProvider replay(options);
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  const auto start = clock->now();
  // The first quote is a minute after the definition, at 1x.
  ASSERT_EQ(clock->waiting_for(), start + 60s);
  EXPECT_EQ(quotes(sink), 0u);
  auto generation = clock->wait_generation();
  replay.set_speed(60);
  EXPECT_EQ(replay.speed(), 60);
  ASSERT_EQ(clock->waiting_for(generation), start + 1s) << "a minute at 60x is a second";
  generation = clock->wait_generation();
  clock->advance(1s);
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 1; }));
  EXPECT_EQ(replay.time(), 1'060 * md::kNanosPerSecond);
  // The next quote is a minute of replay, a second of wall time at 60x; pause half way.
  ASSERT_EQ(clock->waiting_for(generation), start + 2s);
  replay.set_paused(true);
  ASSERT_TRUE(test::recording_eventually([&] { return pause_observed.load(); }));
  EXPECT_TRUE(replay.paused());
  clock->advance(10min);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(quotes(sink), 1u) << "nothing plays while paused";
  generation = clock->wait_generation();
  replay.set_paused(false);
  // Ten paused minutes move the deadline back by ten minutes.
  ASSERT_EQ(clock->waiting_for(generation), start + 2s + 10min);
  clock->advance(1s);
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 2; }));
  replay.stop();
}

// A drill that starts paused at 1x and is played at 300x must not first sit out the
// gap it measured at 1x: a speed chosen while paused rescales the wait on resume.
TEST(Replay, ASpeedChosenWhilePausedAppliesWhenPlayResumes) {
  const auto file = paced_recording();
  auto clock = std::make_shared<ManualClock>();
  std::atomic<bool> pause_observed{false};
  providers::ReplayProvider::Options options{file.path, 1, false, clock};
  options.synchronize = [&] {
    pause_observed = true;
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  };
  providers::ReplayProvider replay(options);
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  const auto start = clock->now();
  ASSERT_EQ(clock->waiting_for(), start + 60s);
  replay.set_paused(true);
  ASSERT_TRUE(test::recording_eventually([&] { return pause_observed.load(); }));
  clock->advance(5min);
  const auto generation = clock->wait_generation();
  replay.set_speed(60);
  replay.set_paused(false);
  // The minute still owed at 1x is a second at 60x, counted from the resume.
  const auto resumed = clock->now();
  ASSERT_EQ(clock->waiting_for(generation), resumed + 1s);
  clock->advance(1s);
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 1; }));
  replay.stop();
}

/// A streamed quote every 15 seconds of market and receipt time, from 10:00:00.
test::RecordingFile quarter_minute_recording(int quotes) {
  test::RecordingFile file;
  const auto open = md::new_york_to_utc({2026, 9, 16}, 10, 0);
  std::vector<md::Event> events = {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")}};
  for (int i = 0; i < quotes; ++i)
    events.push_back(md::OptionQuote{0, open + i * 15 * md::kNanosPerSecond, 100, 101, 10, 10});
  md::RecordingSink::Options recording;
  std::size_t index = 0;
  recording.clock = [&] {
    const auto quote = index == 0 ? 0 : static_cast<int>(index) - 1;
    ++index;
    return open + quote * 15 * md::kNanosPerSecond;
  };
  auto header = test::recording_header();
  header.started = open;
  test::record_events(file.path, events, header, recording);
  return file;
}

providers::ReplayProvider::Driver immediate_driver() {
  return [](providers::ReplayBatch) {
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  };
}

// B34: a lockstep step plays unpaced, so resuming at 1x waits one receipt gap, not
// one more gap for every step taken while paused.
TEST(Replay, ResumingAfterLockstepStepsWaitsOneGap) {
  const auto file = quarter_minute_recording(8);
  const auto open = md::new_york_to_utc({2026, 9, 16}, 10, 0);
  auto clock = std::make_shared<ManualClock>();
  providers::ReplayProvider::Options options{file.path, 1, false, clock};
  options.paused = true;
  providers::ReplayProvider replay(options);
  replay.set_driver(immediate_driver());
  test::DiscardEvents discard;
  replay.start({{"SPX"}}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  for (int step = 1; step <= 3; ++step) {
    clock->advance(1s);
    ASSERT_NO_THROW(replay.until(open + step * 15 * md::kNanosPerSecond));
    EXPECT_EQ(replay.settled_through(), open + step * 15 * md::kNanosPerSecond);
  }
  clock->advance(1s);
  const auto resumed = clock->now();
  // A wait from before the resume must not satisfy the timing assertion.
  const auto generation = clock->wait_generation();
  replay.set_paused(false);
  const auto waiting = clock->waiting_for(generation);
  ASSERT_TRUE(waiting.has_value());
  EXPECT_EQ(*waiting - resumed, std::chrono::duration_cast<ManualClock::TimePoint::duration>(15s));
  replay.stop();
}

TEST(Replay, ResumingBeforeThePacingWaitObservesPausePreservesTheGap) {
  const auto file = quarter_minute_recording(8);
  const auto open = md::new_york_to_utc({2026, 9, 16}, 10, 0);
  enum class Resume { Playback, Deterministic, PlayUntil };
  for (const auto resume : {Resume::Playback, Resume::Deterministic, Resume::PlayUntil}) {
    SCOPED_TRACE(static_cast<int>(resume));
    auto clock = std::make_shared<ManualClock>();
    test::DiscardEvents discard;
    providers::ReplayProvider replay({file.path, 1, false, clock});
    if (resume != Resume::Playback) replay.set_driver(immediate_driver());
    replay.start({{"SPX"}}, discard);
    ASSERT_EQ(clock->waiting_for(), clock->now() + 15s);

    // Force both controls to arrive before the interrupted wait can return. The
    // provider must account for the pause even if pace() never sees paused=true.
    clock->hold_wait(true);
    replay.set_paused(true);
    clock->advance(1s);
    const auto resumed = clock->now();
    const auto generation = clock->wait_generation();
    if (resume == Resume::PlayUntil) replay.play_until(open + 30 * md::kNanosPerSecond);
    else replay.set_paused(false);
    // Time after the resume does count, even before the provider observes it.
    clock->advance(1s);
    clock->hold_wait(false);
    const auto waiting = clock->waiting_for(generation);
    ASSERT_TRUE(waiting.has_value());
    EXPECT_EQ(*waiting - resumed, std::chrono::duration_cast<ManualClock::TimePoint::duration>(15s));
    clock->advance(13s);
    EXPECT_EQ(replay.time(), open);
    clock->advance(1s);
    ASSERT_TRUE(test::recording_eventually([&] { return replay.time() == open + 15 * md::kNanosPerSecond; }));
    replay.stop();
  }
}

// B05: a target past EOF is an error that leaves the run where it was, paused and
// still able to trade and step.
TEST(Replay, UntilPastTheEndIsRefusedBeforeAnythingPlays) {
  const auto file = quarter_minute_recording(8);
  const auto open = md::new_york_to_utc({2026, 9, 16}, 10, 0);
  std::atomic<md::Timestamp> applied{0};
  providers::ReplayProvider::Options options{file.path, 1, false, std::make_shared<ManualClock>()};
  options.paused = true;
  providers::ReplayProvider replay(options);
  replay.set_driver([&](providers::ReplayBatch batch) {
    applied = batch.time;
    return immediate_driver()(std::move(batch));
  });
  test::DiscardEvents discard;
  replay.start({{"SPX"}}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  ASSERT_NO_THROW(replay.until(open + 30 * md::kNanosPerSecond));
  const auto last = open + 105 * md::kNanosPerSecond;
  EXPECT_EQ(replay.end_time(), last);
  EXPECT_THROW(replay.until(last + md::kNanosPerSecond), std::invalid_argument);
  EXPECT_EQ(replay.settled_through(), open + 30 * md::kNanosPerSecond);
  EXPECT_EQ(applied.load(), open + 30 * md::kNanosPerSecond);
  EXPECT_FALSE(replay.finished());
  EXPECT_TRUE(replay.paused());
  ASSERT_NO_THROW(replay.until(open + 60 * md::kNanosPerSecond));
  EXPECT_EQ(applied.load(), open + 60 * md::kNanosPerSecond);
  ASSERT_NO_THROW(replay.until(last));
  EXPECT_EQ(replay.settled_through(), last);
  ASSERT_TRUE(test::recording_eventually([&] { return replay.finished(); }));
  replay.stop();
}

TEST(Replay, PlayUntilPacesOnTheTestClockAndSettlesExactlyLikeAStep) {
  const auto file = quarter_minute_recording(8);
  const auto open = md::new_york_to_utc({2026, 9, 16}, 10, 0);
  auto clock = std::make_shared<ManualClock>();
  providers::ReplayProvider::Options options{file.path, 1, false, clock};
  options.paused = true;
  providers::ReplayProvider replay(options);
  replay.set_driver(immediate_driver());
  test::DiscardEvents discard;
  replay.start({{"SPX"}}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  auto generation = clock->wait_generation();
  replay.play_until(open + 45 * md::kNanosPerSecond, 60);
  EXPECT_EQ(replay.pause_at(), open + 45 * md::kNanosPerSecond);
  EXPECT_FALSE(replay.stepping());
  for (int step = 1; step <= 3; ++step) {
    const auto expected = providers::ReplayClock::TimePoint{} + step * 250ms;
    ASSERT_EQ(clock->waiting_for(generation), expected);
    generation = clock->wait_generation();
    clock->advance(250ms);
    ASSERT_TRUE(test::recording_eventually([&] { return replay.settled_through() == open + step * 15 * md::kNanosPerSecond; }));
  }
  ASSERT_TRUE(test::recording_eventually([&] { return replay.paused() && replay.pause_at() == 0; }));
  replay.wait_paused();
  EXPECT_THROW(replay.play_until(open + 200 * md::kNanosPerSecond, 1), std::invalid_argument);
  EXPECT_EQ(replay.speed(), 60);
  EXPECT_TRUE(replay.paused());
  replay.play_until(open + 90 * md::kNanosPerSecond);
  replay.set_speed(1);
  EXPECT_EQ(replay.pause_at(), 0);
  replay.abort();
  replay.wait_paused();
  EXPECT_TRUE(replay.paused());
  EXPECT_EQ(replay.settled_through(), open + 45 * md::kNanosPerSecond);
  replay.stop();
}

TEST(Replay, SkipCutsAnOvernightGapShortAndTheEndIsReported) {
  const auto file = paced_recording();
  auto clock = std::make_shared<ManualClock>();
  providers::ReplayProvider replay({file.path, 60, false, clock});
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  std::size_t generation = 0;
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(clock->waiting_for(generation), clock->now() + 1s);
    generation = clock->wait_generation();
    clock->advance(1s);
    ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == static_cast<std::size_t>(i + 1); }));
  }
  // Eight hours at 60x is eight minutes; skip plays the next quote now.
  const auto now = clock->now();
  ASSERT_EQ(clock->waiting_for(generation), now + 8min);
  replay.skip();
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 4; }));
  EXPECT_EQ(replay.time(), (1'000 + 180 + 8 * 3600) * md::kNanosPerSecond);
  ASSERT_TRUE(test::recording_eventually([&] { return replay.finished(); }));
  EXPECT_THROW(replay.set_speed(3), std::invalid_argument);
  replay.stop();
}

TEST(Replay, AMaximumGapPassesTheClosedMarketBetweenSessionsInOneStep) {
  const auto file = paced_recording();
  auto clock = std::make_shared<ManualClock>();
  providers::ReplayProvider::Options options{file.path, 60, false, clock};
  options.max_gap = 60 * md::kNanosPerSecond;
  providers::ReplayProvider replay(options);
  test::EventCollector sink;
  replay.start({{"SPX"}}, sink);
  std::size_t generation = 0;
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(clock->waiting_for(generation), clock->now() + 1s);
    generation = clock->wait_generation();
    clock->advance(1s);
    ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == static_cast<std::size_t>(i + 1); }));
  }
  // The eight hours wait one minute of receipt time, a second at 60x.
  const auto now = clock->now();
  ASSERT_EQ(clock->waiting_for(generation), now + 1s);
  clock->advance(1s);
  ASSERT_TRUE(test::recording_eventually([&] { return quotes(sink) == 4; }));
  EXPECT_EQ(replay.time(), (1'000 + 180 + 8 * 3600) * md::kNanosPerSecond);
  replay.stop();
}

TEST(Replay, PlaysVersionOneAndVersionTwoWithVolume) {
  for (const int version : {1, 2}) {
    test::RecordingFile file;
    auto events = session();
    if (version == 2) events.push_back(md::OptionVolume{0, 55, 12});
    test::record_events(file.path, events);
    if (version == 1) {
      std::fstream bytes(file.path, std::ios::binary | std::ios::in | std::ios::out);
      bytes.seekp(8);
      bytes.put(1);
    }
    auto clock = std::make_shared<TestClock>();
    providers::ReplayProvider replay({file.path, 0, false, clock});
    test::EventCollector sink;
    replay.start({{"SPX"}}, sink);
    ASSERT_TRUE(test::recording_eventually([&] { return ended(sink); }));
    replay.stop();
    const auto actual = sink.snapshot();
    ASSERT_EQ(actual.size(), events.size() + 1);
    for (std::size_t i = 0; i < events.size(); ++i) test::exact_event(events[i], actual[i]);
  }
}

TEST(Replay, EndIndexAvoidsScanningAndRejectsStaleIndexes) {
  for (const bool snapshots : {false, true}) {
    test::RecordingFile file;
    test::DiscardEvents sink;
    auto header = test::recording_header();
    const auto first = md::kNanosPerDay;
    const auto last = first + 9999 * md::kNanosPerSecond + 123;
    {
      md::RecordingSink writer(file.path, header, sink, {});
      for (int i = 0; i < 10000; ++i) {
        const auto time = first + i * md::kNanosPerSecond + 123;
        writer.publish(md::UnderlyingQuote{"SPX", time, 5000, 5001, 5000});
        if (snapshots) writer.publish(md::SnapshotComplete{"SPX", time});
      }
      writer.close();
      ASSERT_TRUE(writer.error().empty());
    }
    md::RecordingReader indexed(file.path);
    const auto expected = snapshots ? last : last + md::kNanosPerSecond - 123;
    EXPECT_EQ(indexed.indexed_end({{"SPX"}}), expected);
    EXPECT_EQ(indexed.indexed_snapshot_mode(), snapshots);
    providers::ReplayProvider::Options options;
    options.file = file.path;
    providers::ReplayProvider replay(options);
    // A new scan would fail to open the path: the indexed answer belongs to the reader already open.
    std::filesystem::rename(file.path, file.directory / "moved.oprec");
    EXPECT_EQ(replay.end_time({{"SPX"}}), expected);
    std::filesystem::rename(file.directory / "moved.oprec", file.path);
    { std::ofstream out(file.path, std::ios::app); out << 'x'; }
    EXPECT_FALSE(md::RecordingReader(file.path).indexed_end({{"SPX"}}));
  }
}
}  // namespace
