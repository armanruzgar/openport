#include "support/recording.hpp"
#include "openport/md/recording_directory.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <thread>

namespace {
using namespace openport;
using namespace std::chrono_literals;

std::string contents(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void replace(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

TEST(Recording, EveryEventAndHeaderFieldRoundTripsBitExactly) {
  test::RecordingFile file;
  const auto hi = std::numeric_limits<md::Timestamp>::max();
  const auto lo = std::numeric_limits<md::Timestamp>::min();
  const double nan = test::recording_bits<double>(UINT64_C(0xfff800000000abcd));
  const double tiny = std::numeric_limits<double>::denorm_min();
  const double huge = std::numeric_limits<double>::max();
  const double inf = std::numeric_limits<double>::infinity();
  auto contract = *md::parse_osi("SPXW261022P05000000");
  contract.multiplier = nan;
  contract.strike = huge;
  contract.standard = false;
  contract.root = std::string("root\0bytes", 10);
  contract.expiry = {std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), -1};
  const auto id = std::numeric_limits<md::InstrumentId>::max();
  std::vector<md::Event> events{
      md::ContractDefinition{id, contract},
      md::OptionQuote{id, hi, nan, -0.0, tiny, inf},
      md::OptionTrade{id, lo, -inf, -huge},
      md::OpenInterest{id, hi, nan},
      md::OptionVolume{id, lo, tiny},
      md::VendorGreeks{id, lo, nan, huge, -tiny, -0.0, inf, -inf},
      md::UnderlyingQuote{std::string("SP\0X", 4), hi, nan, tiny, -0.0},
      md::UnderlyingClose{"SPX", lo, {2026, 9, 23}, nan},
      md::SnapshotComplete{std::string("SP\0Y", 4), hi},
      md::SnapshotHeartbeat{"SPX", hi},
      md::TradingHalt{100, 200},
  };
  for (auto state : {md::FeedState::Connecting, md::FeedState::Live, md::FeedState::Delayed,
                     md::FeedState::Stale, md::FeedState::Error, md::FeedState::Stopped})
    events.push_back(md::ProviderStatus{lo, state, std::string("text\0x", 6), "SPX"});
  contract.type = pricing::OptionType::Call;
  contract.style = pricing::ExerciseStyle::American;
  contract.settlement = md::Settlement::AM;
  contract.standard = true;
  events.push_back(md::ContractDefinition{0, contract});
  auto header = test::recording_header();
  header.market_controls = true;
  header.started = lo;
  header.subscription.strike_window = nan;
  md::RecordingSink::Options options;
  md::Timestamp receipt = lo;
  options.clock = [&] { return receipt++; };
  test::record_events(file.path, events, header, options);
  md::RecordingReader reader(file.path);
  const auto& h = reader.header();
  EXPECT_EQ(h.provider, header.provider);
  EXPECT_EQ(h.started, header.started);
  EXPECT_TRUE(h.market_controls);
  EXPECT_EQ(contents(file.path).substr(8, 4), std::string("\4\0\0\0", 4));
  EXPECT_EQ(h.subscription.underlyings, header.subscription.underlyings);
  EXPECT_EQ(h.subscription.max_expiries, header.subscription.max_expiries);
  test::exact_double(header.subscription.strike_window, h.subscription.strike_window);
  EXPECT_EQ(h.capabilities.realtime, true);
  EXPECT_EQ(h.capabilities.realtime_plan_dependent, true);
  EXPECT_EQ(h.capabilities.poll_interval, 15s);
  EXPECT_EQ(h.capabilities.delay, 900s);
  EXPECT_TRUE(h.capabilities.quotes);
  EXPECT_TRUE(h.capabilities.trades);
  EXPECT_TRUE(h.capabilities.open_interest);
  EXPECT_TRUE(h.capabilities.vendor_greeks);
  EXPECT_TRUE(h.capabilities.history);
  receipt = lo;
  for (const auto& expected : events) {
    const auto record = reader.next();
    ASSERT_TRUE(record);
    EXPECT_EQ(record->received, receipt++);
    test::exact_event(expected, record->event);
  }
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.diagnostic().empty());
  reader.rewind();
  ASSERT_TRUE(reader.next());
}

TEST(Recording, EmptySessionAndFalseCapabilitiesAreClean) {
  test::RecordingFile file;
  md::RecordingHeader header;
  header.capabilities.quotes = false;
  test::record_events(file.path, {}, header);
  md::RecordingReader reader(file.path);
  const auto caps = reader.header().capabilities;
  EXPECT_FALSE(caps.realtime || caps.realtime_plan_dependent || caps.quotes || caps.trades ||
               caps.open_interest || caps.vendor_greeks || caps.history);
  EXPECT_EQ(caps.poll_interval, 0s);
  EXPECT_EQ(caps.delay, 0s);
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.diagnostic().empty());
}

TEST(Recording, LegacyHeadersRefuseMarketControlsWithoutWritingUnsupportedTags) {
  for (const bool imported : {false, true}) {
    for (const md::Event& event : {md::Event{md::SnapshotHeartbeat{"SPX", 100}}, md::Event{md::TradingHalt{100, 200}}}) {
      SCOPED_TRACE(imported ? "v3" : "v2");
      SCOPED_TRACE(event.index());
      test::RecordingFile file;
      test::EventCollector downstream;
      auto header = test::recording_header();
      header.imported = imported;
      md::RecordingSink sink(file.path, header, downstream, {});
      sink.publish(event);
      sink.close();
      EXPECT_NE(sink.error().find("require a v4 header with market_controls enabled"), std::string::npos);
      EXPECT_EQ(sink.stats().events, 0U);
      const auto events = downstream.snapshot();
      ASSERT_EQ(events.size(), 2U);
      ASSERT_TRUE(std::holds_alternative<md::ProviderStatus>(events.front()));
      EXPECT_EQ(std::get<md::ProviderStatus>(events.front()).state, md::FeedState::Error);
      EXPECT_EQ(std::get<md::ProviderStatus>(events.front()).message, sink.error());
      test::exact_event(event, events.back());
      EXPECT_EQ(contents(file.path).substr(8, 4), imported ? std::string("\3\0\0\0", 4) : std::string("\2\0\0\0", 4));
      md::RecordingReader reader(file.path);
      EXPECT_FALSE(reader.header().market_controls);
      EXPECT_FALSE(reader.next());
    }
  }
}

TEST(Recording, RejectsBadMagicVersionAndMalformedOrTruncatedHeader) {
  test::RecordingFile file;
  test::record_events(file.path, {});
  const auto original = contents(file.path);
  auto rejects = [&](std::string bytes, const std::string& reason) {
    replace(file.path, bytes);
    try {
      md::RecordingReader reader(file.path);
      FAIL() << "accepted invalid header";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
    }
  };
  auto bytes = original;
  bytes[0] = '!';
  rejects(bytes, "magic");
  bytes = original;
  bytes[8] = 5;
  rejects(bytes, "version 5");
  rejects(original.substr(0, 8), "truncated header");
  rejects(original.substr(0, 20), "truncated header");
  bytes = original;
  bytes[15] = 127;
  rejects(bytes, "header exceeds");
  bytes = original;
  bytes[19] = 127;
  rejects(bytes, "string length");
}

TEST(Recording, NeverOverwritesExistingFilesOrSymlinks) {
  test::RecordingFile file;
  replace(file.path, "keep this file");
  test::DiscardEvents sink;
  EXPECT_THROW((md::RecordingSink(file.path, {}, sink, {})), std::runtime_error);
  EXPECT_EQ(contents(file.path), "keep this file");
  const auto link = file.directory / "link";
  std::filesystem::create_symlink(file.path, link);
  EXPECT_THROW((md::RecordingSink(link, {}, sink, {})), std::runtime_error);
  EXPECT_EQ(contents(file.path), "keep this file");
}

TEST(Recording, ConcurrentPublishersHaveExactlyTheDownstreamOrderWithoutLoss) {
  test::RecordingFile file;
  test::EventCollector sink;
  md::Timestamp receipt = 0;
  md::RecordingSink::Options options;
  options.frame_bytes = 4096;
  options.clock = [&] { return ++receipt; };
  md::RecordingSink recorder(file.path, {}, sink, options);
  std::vector<std::thread> threads;
  for (unsigned publisher = 0; publisher < 8; ++publisher)
    threads.emplace_back([&, publisher] {
      for (int sequence = 0; sequence < 2000; ++sequence)
        recorder.publish(md::OptionTrade{publisher, sequence, static_cast<double>(sequence), 1});
    });
  for (auto& thread : threads) thread.join();
  recorder.close();
  ASSERT_TRUE(recorder.error().empty());
  const auto delivered = sink.snapshot();
  ASSERT_EQ(delivered.size(), 16000u);
  md::RecordingReader reader(file.path);
  std::array<int, 8> sequence{};
  receipt = 0;
  for (const auto& event : delivered) {
    auto record = reader.next();
    ASSERT_TRUE(record);
    EXPECT_EQ(record->received, ++receipt);
    test::exact_event(event, record->event);
    const auto& trade = std::get<md::OptionTrade>(record->event);
    EXPECT_EQ(trade.ts, sequence[trade.id]++);
  }
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.diagnostic().empty());
  EXPECT_EQ(recorder.stats().events, delivered.size());
}

TEST(Recording, IdleFlushBoundsTheCrashWindowAndCloseIsIdempotent) {
  test::RecordingFile file;
  test::DiscardEvents sink;
  md::RecordingSink::Options options;
  options.flush_interval = 20ms;
  md::RecordingSink recorder(file.path, {}, sink, options);
  const auto header_size = std::filesystem::file_size(file.path);
  recorder.publish(md::UnderlyingQuote{"SPX", 1, 2, 3, 2.5});
  ASSERT_TRUE(test::recording_eventually(
      [&] { return std::filesystem::file_size(file.path) > header_size; }));
  md::RecordingReader active(file.path);
  ASSERT_TRUE(active.next());
  EXPECT_FALSE(active.next());
  EXPECT_NE(active.diagnostic().find("truncated"), std::string::npos);
  recorder.close();
  recorder.close();
  md::RecordingReader complete(file.path);
  ASSERT_TRUE(complete.next());
  EXPECT_FALSE(complete.next());
  EXPECT_TRUE(complete.diagnostic().empty());
}

TEST(Recording, FixedFramesKeepTheSameEofBoundaryWhetherCloseOrFlushWins) {
  test::RecordingFile file;
  test::DiscardEvents sink;
  md::RecordingSink::Options options;
  options.fixed_frames = true;
  options.frame_bytes = 1;
  options.clock = [] { return md::Timestamp{123}; };
  std::string expected;
  for (int attempt = 0; attempt < 20; ++attempt) {
    const auto path = file.directory / (std::to_string(attempt) + ".oprec");
    md::RecordingSink recorder(path, {}, sink, options);
    const auto header_size = std::filesystem::file_size(path);
    recorder.publish(md::UnderlyingQuote{"SPX", 1, 2, 3, 2.5});
    if (attempt % 2 == 0) {
      ASSERT_TRUE(test::recording_eventually([&] { return std::filesystem::file_size(path) > header_size; }));
    }
    recorder.close();
    ASSERT_TRUE(recorder.error().empty());
    if (attempt == 0) expected = contents(path);
    else { EXPECT_EQ(contents(path), expected); }
  }
}

TEST(Recording, TruncatedLastFrameRecoversEveryCompleteRecordIncludingInsideTheFrame) {
  test::RecordingFile file;
  std::mt19937_64 random(12);
  std::vector<md::Event> events;
  // Incompressible payload spans several zstd blocks and input/output buffers.
  for (md::InstrumentId id = 0; id < 20000; ++id)
    events.push_back(md::OptionQuote{
        id, static_cast<md::Timestamp>(id), test::recording_bits<double>(random()),
        test::recording_bits<double>(random()), test::recording_bits<double>(random()),
        test::recording_bits<double>(random())});
  test::record_events(file.path, events);
  const auto original = contents(file.path);
  const auto header_size = 16u + static_cast<unsigned char>(original[12]);
  for (const auto size : {static_cast<std::size_t>(header_size), original.size() / 2,
                          original.size() - 20, original.size() - 1}) {
    replace(file.path, original.substr(0, size));
    md::RecordingReader reader(file.path);
    std::size_t recovered = 0;
    while (auto record = reader.next()) {
      ASSERT_LT(recovered, events.size());
      test::exact_event(events[recovered++], record->event);
    }
    EXPECT_FALSE(reader.diagnostic().empty());
    if (size > original.size() / 2) {
      EXPECT_GT(recovered, 10000u);
    }
    if (size == original.size() - 1) {
      EXPECT_EQ(recovered, events.size());
    }
  }
  replace(file.path, original);
  md::RecordingReader complete(file.path);
  std::size_t count = 0;
  while (complete.next()) ++count;
  EXPECT_EQ(count, events.size());
  EXPECT_TRUE(complete.diagnostic().empty());
}

TEST(Recording, CorruptFrameAndTrailingDataAreErrorsRatherThanTruncation) {
  test::RecordingFile file;
  test::record_events(file.path, {md::OptionTrade{1, 2, 3, 4}});
  const auto original = contents(file.path);
  auto broken = original;
  broken.back() ^= 1;  // frame checksum
  replace(file.path, broken);
  md::RecordingReader corrupt(file.path);
  EXPECT_THROW(while (corrupt.next()){}, std::runtime_error);
  replace(file.path, original + "junk");
  md::RecordingReader trailing(file.path);
  EXPECT_THROW(while (trailing.next()){}, std::runtime_error);
}

TEST(Recording, StorageFailureIsExplicitAndFurtherEventsStillReachTheLiveSink) {
  test::RecordingFile file;
  test::EventCollector sink;
  md::RecordingSink::Options options;
  options.write = [writes = 0](int fd, std::span<const char> bytes) mutable {
    if (++writes > 1) throw std::runtime_error("injected disk full");
    ASSERT_EQ(::write(fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
  };
  options.frame_bytes = 1;
  md::RecordingSink recorder(file.path, {}, sink, options);
  recorder.publish(md::OptionTrade{});
  ASSERT_TRUE(test::recording_eventually([&] { return !recorder.error().empty(); }));
  EXPECT_NE(recorder.error().find("disk full"), std::string::npos);
  recorder.publish(md::UnderlyingQuote{"SPX"});
  recorder.close();
  const auto events = sink.snapshot();
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(std::get<md::ProviderStatus>(events[1]).state, md::FeedState::Error);
  EXPECT_TRUE(std::holds_alternative<md::UnderlyingQuote>(events[2]));
}
TEST(Recording, LargeCompressibleRecordsCrossDecoderBuffersWithoutLosingFields) {
  test::RecordingFile file;
  const md::ProviderStatus event{1, md::FeedState::Error, std::string(800000, 'x'), "SPX"};
  test::record_events(file.path, {event});
  md::RecordingReader reader(file.path);
  const auto record = reader.next();
  ASSERT_TRUE(record);
  test::exact_event(event, record->event);
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.diagnostic().empty());
}

TEST(Recording, LimitsFailExplicitlyAndDoNotAllocateFromUnboundedLengths) {
  test::RecordingFile file;
  test::EventCollector sink;
  md::RecordingSink::Options options;
  options.frame_bytes = 0;
  EXPECT_THROW((md::RecordingSink(file.path, {}, sink, options)), std::runtime_error);
  options.frame_bytes = 4 * 1024 * 1024;
  options.flush_interval = 1001ms;
  EXPECT_THROW((md::RecordingSink(file.path, {}, sink, options)), std::runtime_error);
  options.flush_interval = 1s;
  md::RecordingSink recorder(file.path, {}, sink, options);
  recorder.publish(
      md::ProviderStatus{1, md::FeedState::Error, std::string(1024 * 1024 + 1, 'x'), ""});
  recorder.close();
  EXPECT_NE(recorder.error().find("1 MiB limit"), std::string::npos);
  ASSERT_EQ(sink.snapshot().size(), 2u);
  EXPECT_EQ(std::get<md::ProviderStatus>(sink.snapshot()[0]).state, md::FeedState::Error);
}

TEST(Recording, BackpressureAllowsOnlyOneUncommittedFrame) {
  test::RecordingFile file;
  test::EventCollector sink;
  std::mutex mutex;
  std::condition_variable ready;
  bool writing = false, release = false;
  md::RecordingSink::Options options;
  options.frame_bytes = 1;
  options.write = [&, count = 0](int fd, std::span<const char> bytes) mutable {
    if (++count == 2) {
      std::unique_lock lock(mutex);
      writing = true;
      ready.notify_all();
      ready.wait(lock, [&] { return release; });
    }
    if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size()))
      throw std::runtime_error("test write failed");
  };
  md::RecordingSink recorder(file.path, {}, sink, options);
  recorder.publish(md::OptionTrade{0, 1});
  {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(ready.wait_for(lock, 5min, [&] { return writing; }));
  }
  std::atomic<bool> attempted{false}, published{false};
  std::thread producer([&] {
    attempted = true;
    recorder.publish(md::OptionTrade{0, 2});
    published = true;
  });
  EXPECT_TRUE(test::recording_eventually([&] { return attempted.load(); }));
  // The worker is blocked in storage, so no later publication may be admitted.
  EXPECT_FALSE(published);
  EXPECT_EQ(sink.snapshot().size(), 1u);
  {
    const std::lock_guard lock(mutex);
    release = true;
    ready.notify_all();
  }
  producer.join();
  recorder.close();
  EXPECT_TRUE(published);
  EXPECT_EQ(sink.snapshot().size(), 2u);
  md::RecordingReader reader(file.path);
  ASSERT_TRUE(reader.next());
  ASSERT_TRUE(reader.next());
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.diagnostic().empty());
}


TEST(Recording, VersionOneKeepsItsOriginalRecordLayout) {
  test::RecordingFile file;
  const std::vector<md::Event> events{
      md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
      md::OptionQuote{0, 123, 2, 3, 4, 5}, md::OptionTrade{0, 124, 2.5, 6},
      md::OpenInterest{0, 125, 7}, md::VendorGreeks{0, 126, .2, .5, .01, 1, -1, .1},
      md::UnderlyingQuote{"SPX", 127, 4999, 5001, 5000},
      md::ProviderStatus{128, md::FeedState::Live, "synthetic", "SPX"},
      md::UnderlyingClose{"SPX", 129, {2026, 9, 22}, 5000}, md::SnapshotComplete{"SPX", 130}};
  test::record_events(file.path, events);
  auto bytes = contents(file.path);
  ASSERT_EQ(bytes[8], 2);
  // V1 has the identical header and tags 0–8. Only the version word differs.
  bytes[8] = 1;
  replace(file.path, bytes);
  md::RecordingReader reader(file.path);
  for (const auto& event : events) {
    const auto record = reader.next();
    ASSERT_TRUE(record);
    test::exact_event(event, record->event);
  }
  EXPECT_FALSE(reader.next());
  EXPECT_TRUE(reader.diagnostic().empty());
}

TEST(Recording, VersionOneRejectsTheNewVolumeTag) {
  test::RecordingFile file;
  test::record_events(file.path, {md::OptionVolume{0, 123, 0}});
  auto bytes = contents(file.path);
  bytes[8] = 1;
  replace(file.path, bytes);
  md::RecordingReader reader(file.path);
  EXPECT_THROW((void)reader.next(), std::runtime_error);
}

}  // namespace

namespace {
std::vector<std::filesystem::path> published(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().extension() == ".oprec") files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}
TEST(RecordingDirectory, RolloverPublishesCompleteIndependentDaysAndKeepsMarketClocks) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.provider = "massive";
  std::atomic<md::Timestamp> time{md::new_york_to_utc({2026, 9, 4}, 16, 59)};
  md::RecordingDirectory::Options options;
  options.recording.clock = [&] { return time.load(); };
  test::EventCollector sink;
  md::RecordingDirectory recording(file.directory, header, sink, options);
  const auto contract = *md::parse_osi("SPXW261022C05000000");
  recording.publish(md::ContractDefinition{5, contract});
  const auto original = time.load() - 900 * md::kNanosPerSecond;
  recording.publish(md::OptionQuote{5, original, 5, 6, 1, 2});
  EXPECT_TRUE(published(file.directory).empty());
  // Friday 17:00 advances to Tuesday after Labor Day, without waiting for a tick.
  time = md::new_york_to_utc({2026, 9, 4}, 17, 0);
  recording.rollover();
  auto files = published(file.directory);
  ASSERT_EQ(files.size(), 1U);
  EXPECT_NE(files[0].filename().string().find("2026-09-04"), std::string::npos);
  md::RecordingReader first(files[0]);
  ASSERT_TRUE(first.next());
  const auto quote = first.next();
  ASSERT_TRUE(quote);
  EXPECT_EQ(std::get<md::OptionQuote>(quote->event).ts, original);
  EXPECT_FALSE(first.next());
  EXPECT_TRUE(first.diagnostic().empty());
  recording.publish(md::OptionQuote{5, time, 6, 7, 2, 3});
  EXPECT_EQ(sink.snapshot().size(), 3U) << "rollover definitions must not be delivered twice downstream";
  recording.close();
  EXPECT_TRUE(recording.error().empty());
  files = published(file.directory);
  ASSERT_EQ(files.size(), 2U);
  EXPECT_NE(files[1].filename().string().find("2026-09-08"), std::string::npos);
  md::RecordingReader second(files[1]);
  const auto definition = second.next();
  ASSERT_TRUE(definition);
  EXPECT_EQ(std::get<md::ContractDefinition>(definition->event).id, 5U);
  ASSERT_TRUE(second.next());
  EXPECT_FALSE(second.next());
  EXPECT_TRUE(second.diagnostic().empty());
  EXPECT_EQ(recording.stats().events, 4U);
  EXPECT_TRUE(second.indexed_end(header.subscription).has_value());
}
TEST(RecordingDirectory, DstAndBackwardClocksDoNotSplitOrReopenDays) {
  test::RecordingFile file;
  auto header = test::recording_header();
  md::Timestamp time = md::new_york_to_utc({2026, 10, 30}, 17, 0);
  md::RecordingDirectory::Options options;
  options.recording.clock = [&] { return time; };
  test::DiscardEvents discard;
  md::RecordingDirectory recording(file.directory, header, discard, options);
  time = md::new_york_to_utc({2026, 11, 2}, 9, 30);
  recording.rollover();
  EXPECT_TRUE(published(file.directory).empty());
  time = md::new_york_to_utc({2026, 10, 30}, 16, 0);
  recording.rollover();
  EXPECT_TRUE(published(file.directory).empty());
  time = md::new_york_to_utc({2026, 11, 2}, 17, 0);
  recording.rollover();
  EXPECT_EQ(published(file.directory).size(), 1U);
  recording.close();
  EXPECT_EQ(published(file.directory).size(), 2U);
}
TEST(RecordingDirectory, RetentionPrunesOldestCompleteFilesAndTheirIndexesOnly) {
  test::RecordingFile file;
  auto header = test::recording_header();
  std::vector<std::filesystem::path> old;
  for (int day : {21, 22, 23}) {
    header.started = md::new_york_to_utc({2026, 9, day}, 9, 30);
    old.push_back(file.directory / (std::to_string(24 - day) + ".oprec"));
    test::record_events(old.back(), {md::UnderlyingQuote{"SPX", header.started, 0, 0, 5000}}, header);
  }
  const auto active = file.directory / "active.oprec";
  test::DiscardEvents discard;
  md::RecordingSink writing(active, header, discard, {});
  const auto broken = file.directory / "broken.oprec";
  replace(broken, "not a recording");
  const auto link = file.directory / "linked.oprec";
  std::filesystem::create_symlink(old[2], link);
  md::RecordingDirectory::Options options;
  options.recording.clock = [] { return md::new_york_to_utc({2026, 9, 24}, 9, 30); };
  options.keep_days = 2;
  md::RecordingDirectory recording(file.directory, header, discard, options);
  EXPECT_FALSE(std::filesystem::exists(old[0]));
  EXPECT_FALSE(std::filesystem::exists(old[1]));
  EXPECT_FALSE(std::filesystem::exists(old[0].string() + ".end"));
  EXPECT_TRUE(std::filesystem::exists(old[2]));
  EXPECT_TRUE(std::filesystem::exists(active));
  EXPECT_TRUE(std::filesystem::exists(broken));
  EXPECT_TRUE(std::filesystem::is_symlink(link));
  recording.close();
  writing.close();
}
TEST(RecordingDirectory, ByteLimitProtectsTheWriterEvenWhenItAloneExceedsTheLimit) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.started = md::new_york_to_utc({2026, 9, 22}, 9, 30);
  test::record_events(file.path, {md::UnderlyingQuote{"SPX", header.started, 0, 0, 5000}}, header);
  md::RecordingDirectory::Options options;
  options.recording.clock = [] { return md::new_york_to_utc({2026, 9, 24}, 9, 30); };
  options.max_bytes = 1;
  test::EventCollector sink;
  md::RecordingDirectory recording(file.directory, header, sink, options);
  EXPECT_FALSE(std::filesystem::exists(file.path));
  recording.publish(md::UnderlyingQuote{"SPX", options.recording.clock(), 0, 0, 5001});
  EXPECT_TRUE(recording.error().empty());
  EXPECT_EQ(sink.snapshot().size(), 1U);
  EXPECT_FALSE(std::filesystem::is_empty(file.directory));
  recording.close();
  EXPECT_TRUE(recording.error().empty());
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}
TEST(RecordingDirectory, StorageFailureNeverPublishesPartialFilesOrStopsDownstream) {
  test::RecordingFile file;
  test::EventCollector sink;
  md::RecordingDirectory::Options options;
  options.recording.write = [](int fd, std::span<const char> bytes) {
    if (bytes.size() >= 8 && std::string_view(bytes.data(), 5) == "OPREC") {
      if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) throw std::runtime_error("test header write");
    } else throw std::runtime_error("test disk full");
  };
  md::RecordingDirectory recording(file.directory, test::recording_header(), sink, options);
  recording.publish(md::UnderlyingQuote{"SPX", md::now(), 0, 0, 5000});
  recording.close();
  EXPECT_NE(recording.error().find("test disk full"), std::string::npos);
  EXPECT_TRUE(published(file.directory).empty());
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
  EXPECT_EQ(sink.snapshot().size(), 2U);
}
}  // namespace

namespace {
TEST(RecordingDirectory, ByteRetentionUsesHeaderAgeRatherThanFilenameOrder) {
  test::RecordingFile file;
  auto header = test::recording_header();
  std::mt19937 random(42);
  std::string payload(100'000, ' ');
  for (auto& ch : payload) ch = static_cast<char>('a' + random() % 26);
  const auto older = file.directory / "z-old.oprec", newer = file.directory / "a-new.oprec";
  for (const auto& path : {older, newer}) {
    header.started = md::new_york_to_utc({2026, 9, path == older ? 22 : 23}, 9, 30);
    test::record_events(path, {md::ProviderStatus{header.started, md::FeedState::Live, payload, "SPX"}}, header);
  }
  md::RecordingDirectory::Options options;
  options.recording.clock = [] { return md::new_york_to_utc({2026, 9, 24}, 9, 30); };
  options.max_bytes = std::filesystem::file_size(newer) + std::filesystem::file_size(newer.string() + ".end") + 1024;
  test::DiscardEvents discard;
  md::RecordingDirectory recording(file.directory, header, discard, options);
  EXPECT_FALSE(std::filesystem::exists(older));
  EXPECT_TRUE(std::filesystem::exists(newer));
  recording.close();
  EXPECT_TRUE(std::filesystem::exists(newer));
}
}  // namespace

namespace {
template <typename Predicate>
bool eventually(Predicate done) {
  for (int attempt = 0; attempt < 500 && !done(); ++attempt) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return done();
}
TEST(RecordingDirectory, RetentionFailuresWarnAndNeverStopStartupOrRollover) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.started = md::new_york_to_utc({2026, 9, 21}, 9, 30);
  const auto old = file.directory / "old.oprec";
  test::record_events(old, {md::UnderlyingQuote{"SPX", header.started, 0, 0, 5000}}, header);
  std::atomic<md::Timestamp> time{md::new_york_to_utc({2026, 9, 24}, 9, 30)};
  std::atomic<int> refused{0};
  md::RecordingDirectory::Options options;
  options.recording.clock = [&] { return time.load(); };
  options.keep_days = 1;
  options.remove = [&](const std::filesystem::path&, std::error_code& ec) {
    ++refused;
    ec = std::make_error_code(std::errc::permission_denied);
    return false;
  };
  test::EventCollector sink;
  std::unique_ptr<md::RecordingDirectory> recording;
  ASSERT_NO_THROW(recording = std::make_unique<md::RecordingDirectory>(file.directory, header, sink, options));
  EXPECT_EQ(refused, 1);
  EXPECT_TRUE(std::filesystem::exists(old));
  EXPECT_NE(recording->warning().find("could not remove old.oprec"), std::string::npos) << recording->warning();
  EXPECT_TRUE(recording->error().empty());
  time = md::new_york_to_utc({2026, 9, 24}, 17, 0);
  recording->rollover();
  recording->publish(md::UnderlyingQuote{"SPX", time, 0, 0, 5001});
  EXPECT_TRUE(eventually([&] { return refused >= 2; }));
  EXPECT_TRUE(recording->error().empty());
  recording->close();
  EXPECT_TRUE(recording->error().empty());
  EXPECT_EQ(published(file.directory).size(), 3U) << "both days publish beside the undeletable file";
  EXPECT_TRUE(std::filesystem::exists(old));
  EXPECT_NE(recording->warning().find("ermission denied"), std::string::npos) << recording->warning();
  EXPECT_EQ(sink.snapshot().size(), 1U) << "retention problems are not feed errors";
}
TEST(RecordingDirectory, RolloverRetentionRunsOffThePublishingThread) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.started = md::new_york_to_utc({2026, 9, 21}, 9, 30);
  const auto old = file.directory / "old.oprec";
  test::record_events(old, {md::UnderlyingQuote{"SPX", header.started, 0, 0, 5000}}, header);
  std::atomic<md::Timestamp> time{md::new_york_to_utc({2026, 9, 21}, 10, 0)};
  std::mutex lock;
  std::vector<std::thread::id> removers;
  md::RecordingDirectory::Options options;
  options.recording.clock = [&] { return time.load(); };
  options.keep_days = 2;
  options.remove = [&](const std::filesystem::path& path, std::error_code& ec) {
    { const std::lock_guard guard(lock); removers.push_back(std::this_thread::get_id()); }
    return std::filesystem::remove(path, ec);
  };
  test::DiscardEvents discard;
  md::RecordingDirectory recording(file.directory, header, discard, options);
  EXPECT_TRUE(std::filesystem::exists(old));
  time = md::new_york_to_utc({2026, 9, 23}, 17, 0);
  recording.publish(md::UnderlyingQuote{"SPX", time, 0, 0, 5001});
  EXPECT_TRUE(eventually([&] { return !std::filesystem::exists(old); }));
  EXPECT_FALSE(std::filesystem::exists(old.string() + ".end"));
  {
    const std::lock_guard guard(lock);
    ASSERT_FALSE(removers.empty());
    for (const auto& remover : removers) EXPECT_NE(remover, std::this_thread::get_id());
  }
  recording.close();
  EXPECT_TRUE(recording.error().empty());
  EXPECT_TRUE(recording.warning().empty()) << recording.warning();
}
TEST(RecordingDirectory, ChangedFilesAreRecheckedAfterAnEarlierScanSkippedThem) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.started = md::new_york_to_utc({2026, 9, 21}, 9, 30);
  const auto active = file.directory / "active.oprec";
  test::DiscardEvents discard;
  md::RecordingSink writing(active, header, discard, {});
  writing.publish(md::UnderlyingQuote{"SPX", header.started, 0, 0, 5000});
  md::RecordingDirectory::Options options;
  options.recording.clock = [] { return md::new_york_to_utc({2026, 9, 24}, 9, 30); };
  options.keep_days = 2;
  md::RecordingDirectory recording(file.directory, header, discard, options);
  EXPECT_TRUE(std::filesystem::exists(active)) << "an open explicit recording is never pruned";
  writing.close();
  recording.close();
  EXPECT_FALSE(std::filesystem::exists(active)) << "the completed file is no longer served from the cached verdict";
}
TEST(RecordingDirectory, StaleStagingFoldersCountTowardTheByteBudgetAndAreKept) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.started = md::new_york_to_utc({2026, 9, 23}, 9, 30);
  const auto complete = file.directory / "complete.oprec";
  test::record_events(complete, {md::UnderlyingQuote{"SPX", header.started, 0, 0, 5000}}, header);
  const auto staging = file.directory / ".record-crash1";
  std::filesystem::create_directory(staging);
  replace(staging / "day.oprec", std::string(200'000, 'x'));
  md::RecordingDirectory::Options options;
  options.recording.clock = [] { return md::new_york_to_utc({2026, 9, 24}, 9, 30); };
  options.max_bytes = 100'000;
  test::DiscardEvents discard;
  md::RecordingDirectory recording(file.directory, header, discard, options);
  EXPECT_FALSE(std::filesystem::exists(complete)) << "the crashed writer's bytes count toward the budget";
  EXPECT_TRUE(std::filesystem::exists(staging / "day.oprec")) << "stale staging data is never deleted";
  EXPECT_NE(recording.warning().find("1 stale staging directory (200000 bytes)"), std::string::npos) << recording.warning();
  recording.close();
  EXPECT_TRUE(std::filesystem::exists(staging / "day.oprec"));
  std::size_t staging_folders = 0;
  for (const auto& entry : std::filesystem::directory_iterator(file.directory))
    if (entry.path().filename().string().starts_with(".record-")) ++staging_folders;
  EXPECT_EQ(staging_folders, 1U) << "this writer's own staging folder is not counted as stale";
}
TEST(RecordingDirectory, OneReceiptTimeDecidesTheDayAndStampsTheEvent) {
  test::RecordingFile file;
  const auto boundary = md::new_york_to_utc({2026, 9, 24}, 17, 0);
  // Every clock read advances, so a second read would cross into the next day.
  std::atomic<md::Timestamp> next{boundary - 2};
  md::RecordingDirectory::Options options;
  options.recording.clock = [&] { return next.fetch_add(1); };
  test::DiscardEvents discard;
  md::RecordingDirectory recording(file.directory, test::recording_header(), discard, options);
  recording.publish(md::UnderlyingQuote{"SPX", boundary - 1, 0, 0, 5000});
  recording.close();
  const auto files = published(file.directory);
  ASSERT_EQ(files.size(), 1U);
  EXPECT_NE(files[0].filename().string().find("2026-09-24"), std::string::npos);
  md::RecordingReader reader(files[0]);
  const auto record = reader.next();
  ASSERT_TRUE(record);
  EXPECT_EQ(record->received, boundary - 1);
  EXPECT_FALSE(reader.next());
}
}  // namespace
