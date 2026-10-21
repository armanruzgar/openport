#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "openport/md/recording.hpp"

namespace openport::test {
struct RecordingFile {
  RecordingFile() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-recording-XXXXXX").string();
    const auto* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("cannot create recording test directory");
    directory = created;
    path = directory / "session.oprec";
  }
  ~RecordingFile() {
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
  }
  std::filesystem::path directory, path;
};

class EventCollector : public md::EventSink {
 public:
  void publish(md::Event event) override {
    const std::lock_guard lock(mutex);
    events.push_back(std::move(event));
  }
  std::vector<md::Event> snapshot() const {
    const std::lock_guard lock(mutex);
    return events;
  }

 private:
  mutable std::mutex mutex;
  std::vector<md::Event> events;
};

class DiscardEvents : public md::EventSink {
 public:
  void publish(md::Event) override {}
};

template <typename Predicate>
bool recording_eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

template <typename To, typename From>
To recording_bits(const From& value) {
  static_assert(sizeof(To) == sizeof(From));
  To result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}
inline void exact_double(double expected, double actual) {
  EXPECT_EQ(recording_bits<std::uint64_t>(expected), recording_bits<std::uint64_t>(actual));
}

inline void exact_event(const md::Event& expected, const md::Event& actual) {
  ASSERT_EQ(expected.index(), actual.index());
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        const auto& a = std::get<T>(actual);
        if constexpr (std::is_same_v<T, md::ContractDefinition>) {
          EXPECT_EQ(e.id, a.id);
          const auto& c = e.contract;
          const auto& d = a.contract;
          EXPECT_EQ(c.root, d.root);
          EXPECT_EQ(c.underlying, d.underlying);
          EXPECT_EQ(c.expiry, d.expiry);
          exact_double(c.strike, d.strike);
          EXPECT_EQ(c.type, d.type);
          EXPECT_EQ(c.style, d.style);
          EXPECT_EQ(c.settlement, d.settlement);
          exact_double(c.multiplier, d.multiplier);
          EXPECT_EQ(c.standard, d.standard);
        } else {
          EXPECT_EQ(e.ts, a.ts);
          if constexpr (std::is_same_v<T, md::UnderlyingQuote>) {
            EXPECT_EQ(e.symbol, a.symbol);
            exact_double(e.bid, a.bid);
            exact_double(e.ask, a.ask);
            exact_double(e.last, a.last);
          } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
            EXPECT_EQ(e.state, a.state);
            EXPECT_EQ(e.message, a.message);
            EXPECT_EQ(e.underlying, a.underlying);
          } else if constexpr (std::is_same_v<T, md::UnderlyingClose>) {
            EXPECT_EQ(e.symbol, a.symbol);
            EXPECT_EQ(e.date, a.date);
            exact_double(e.price, a.price);
          } else if constexpr (std::is_same_v<T, md::SnapshotComplete> || std::is_same_v<T, md::SnapshotHeartbeat>) {
            EXPECT_EQ(e.underlying, a.underlying);
          } else if constexpr (std::is_same_v<T, md::TradingHalt>) {
            EXPECT_EQ(e.end, a.end);
            EXPECT_EQ(e.underlying, a.underlying);
          } else {
            EXPECT_EQ(e.id, a.id);
            if constexpr (std::is_same_v<T, md::OptionQuote>) {
              exact_double(e.bid, a.bid);
              exact_double(e.ask, a.ask);
              exact_double(e.bid_size, a.bid_size);
              exact_double(e.ask_size, a.ask_size);
            } else if constexpr (std::is_same_v<T, md::OptionTrade>) {
              exact_double(e.price, a.price);
              exact_double(e.size, a.size);
              EXPECT_EQ(e.venue, a.venue);
            } else if constexpr (std::is_same_v<T, md::OpenInterest> || std::is_same_v<T, md::OptionVolume>) {
              exact_double(e.contracts, a.contracts);
            } else if constexpr (std::is_same_v<T, md::VendorGreeks>) {
              exact_double(e.iv, a.iv);
              exact_double(e.delta, a.delta);
              exact_double(e.gamma, a.gamma);
              exact_double(e.vega, a.vega);
              exact_double(e.theta, a.theta);
              exact_double(e.rho, a.rho);
            }
          }
        }
      },
      expected);
}

inline md::RecordingHeader recording_header() {
  md::RecordingHeader header;
  header.provider = "synthetic";
  header.started = 100;
  header.subscription = {{"SPX", "SPY"}, 2, .125};
  header.capabilities = {
      true, true, std::chrono::seconds(15), std::chrono::seconds(900), true, true, true,
      true, true};
  return header;
}

inline void record_events(const std::filesystem::path& path, const std::vector<md::Event>& events,
                          md::RecordingHeader header = recording_header(),
                          md::RecordingSink::Options options = {}) {
  DiscardEvents discard;
  md::RecordingSink recorder(path, header, discard, std::move(options));
  for (const auto& event : events) recorder.publish(event);
  recorder.close();
  EXPECT_TRUE(recorder.error().empty()) << recorder.error();
}
}  // namespace openport::test
