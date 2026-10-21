#include "support/recording.hpp"

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <random>

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
      md::OptionTrade{id, hi, 1.25, 7, std::numeric_limits<std::uint16_t>::max()},
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
  header.trade_venues = true;
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
  EXPECT_TRUE(h.trade_venues);
  EXPECT_EQ(contents(file.path).substr(8, 4), std::string("\5\0\0\0", 4));
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

TEST(Recording, TradeVenuesWriteVersionFiveAndOthersKeepTheirEarlierBytes) {
  const std::vector<md::Event> plain_events{md::OptionTrade{1, 2, 3, 4}, md::OptionQuote{1, 2, 3, 4, 5, 6}};
  for (const bool controls : {false, true}) {
    SCOPED_TRACE(controls ? "market controls" : "plain");
    auto header = test::recording_header();
    header.market_controls = controls;
    test::RecordingFile before, venued;
    // Providers without venues still write v2 or v4, with no extra header byte.
    test::record_events(before.path, plain_events, header);
    const auto bytes = contents(before.path);
    EXPECT_EQ(bytes.substr(8, 4), controls ? std::string("\4\0\0\0", 4) : std::string("\2\0\0\0", 4));
    header.trade_venues = true;
    test::record_events(venued.path, {md::OptionTrade{1, 2, 3, 4, 9}, md::OptionTrade{1, 2, 3, 4}}, header);
    EXPECT_EQ(contents(venued.path).substr(8, 4), std::string("\5\0\0\0", 4));
    md::RecordingReader reader(venued.path);
    EXPECT_TRUE(reader.header().trade_venues);
    // Version 5 says whether it has market controls; venues do not imply them.
    EXPECT_EQ(reader.header().market_controls, controls);
    const auto first = reader.next(), second = reader.next();
    ASSERT_TRUE(first && second);
    EXPECT_EQ(std::get<md::OptionTrade>(first->event).venue, 9);
    EXPECT_EQ(std::get<md::OptionTrade>(second->event).venue, 0);
    md::RecordingReader old(before.path);
    EXPECT_FALSE(old.header().trade_venues);
    EXPECT_EQ(old.header().market_controls, controls);
    const auto unchanged = old.next();
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(std::get<md::OptionTrade>(unchanged->event).venue, 0);
    EXPECT_TRUE(reader.diagnostic().empty() && old.diagnostic().empty());
  }
}

TEST(Recording, EarlierHeadersRefuseTradeVenues) {
  for (const bool controls : {false, true}) {
    test::RecordingFile file;
    test::EventCollector downstream;
    auto header = test::recording_header();
    header.market_controls = controls;
    md::RecordingSink sink(file.path, header, downstream, {});
    sink.publish(md::OptionTrade{1, 2, 3, 4, 9});
    sink.close();
    EXPECT_NE(sink.error().find("requires a v5 header with trade_venues enabled"), std::string::npos) << sink.error();
    EXPECT_EQ(sink.stats().events, 0U);
    md::RecordingReader reader(file.path);
    EXPECT_FALSE(reader.next());
  }
}

TEST(Recording, VersionFourRejectsATrailingVenue) {
  test::RecordingFile file;
  auto header = test::recording_header();
  header.market_controls = header.trade_venues = true;
  test::record_events(file.path, {md::OptionTrade{1, 2, 3, 4, 9}}, header);
  auto bytes = contents(file.path);
  ASSERT_EQ(bytes[8], 5);
  // Rewrite the v5 header as v4: drop its market-controls byte and shorten the length.
  bytes[8] = 4;
  const auto length = static_cast<unsigned char>(bytes[12]);
  bytes.erase(16 + length - 1, 1);
  bytes[12] = static_cast<char>(length - 1);
  replace(file.path, bytes);
  md::RecordingReader reader(file.path);
  EXPECT_TRUE(reader.header().market_controls);
  EXPECT_FALSE(reader.header().trade_venues);
  EXPECT_THROW((void)reader.next(), std::runtime_error);
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
  bytes[8] = 6;
  rejects(bytes, "version 6");
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
