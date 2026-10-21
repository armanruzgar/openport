#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "openport/md/provider.hpp"

namespace openport::md {

struct RecordingHeader {
  std::string provider;
  Capabilities capabilities;
  Subscription subscription;
  Timestamp started = 0;
  bool imported = false;
  bool market_controls = false; ///< version 4 heartbeat and halt events
  bool trade_venues = false;    ///< version 5: option trades may carry a venue
};

struct RecordedEvent {
  Timestamp received = 0;
  Event event;
};

/// Streams bounded records; a torn final frame yields its complete records and
/// a diagnostic at EOF. Malformed headers, records and corrupt frames throw.
class RecordingReader {
 public:
  explicit RecordingReader(const std::filesystem::path& path);
  ~RecordingReader();
  RecordingReader(const RecordingReader&) = delete;
  RecordingReader& operator=(const RecordingReader&) = delete;
  [[nodiscard]] const RecordingHeader& header() const;
  [[nodiscard]] std::optional<RecordedEvent> next();
  [[nodiscard]] const std::string& diagnostic() const;
  /// Reuses the open file, so looping cannot switch to a replaced pathname.
  void rewind();
  /// Optional close-time sidecar bound to this recording; no event scan.
  [[nodiscard]] std::optional<Timestamp> indexed_end(const Subscription& subscription) const;
  [[nodiscard]] std::optional<bool> indexed_snapshot_mode() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct RecordingStats {
  std::uint64_t events = 0;
  std::uint64_t bytes = 0;
  std::uint64_t publish_nanoseconds = 0;  ///< includes downstream delivery and backpressure
};

/// Interpose before a coalescing queue. One lock orders receipt, encoding and
/// delivery across publishers; bounded buffers backpressure instead of dropping.
class RecordingSink final : public EventSink {
 public:
  struct Options {
    std::function<Timestamp()> clock = now;
    std::chrono::milliseconds flush_interval{1000};
    std::size_t frame_bytes = 4 * 1024 * 1024;
    bool fixed_frames = false; ///< generated files flush only at size boundaries or close
    /// Optional syscall replacement for testing short storage/error paths.
    /// Must write the entire span or throw, and must not reenter this sink.
    std::function<void(int, std::span<const char>)> write{};
  };

  RecordingSink(const std::filesystem::path& path, const RecordingHeader& header,
                EventSink& downstream, Options options);
  ~RecordingSink() override;
  RecordingSink(const RecordingSink&) = delete;
  RecordingSink& operator=(const RecordingSink&) = delete;
  void publish(Event event) override;
  /// Call after publishers stop. Drains, writes a clean-end marker, and joins;
  /// storage failures remain observable through error() and a downstream status.
  /// Events requiring market_controls stop recording with the same error reporting
  /// if the header did not enable them; downstream delivery continues.
  void close();
  [[nodiscard]] std::string error() const;
  [[nodiscard]] RecordingStats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::md
