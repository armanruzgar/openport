#pragma once

#include "openport/md/recording.hpp"

namespace openport::md {

/// One independently replayable file per receipt-clock trading date. Open files
/// stay in private staging directories; only clean closes become visible.
/// Retention runs at startup and close, and on a worker after each rollover.
class RecordingDirectory final : public EventSink {
 public:
  struct Options {
    RecordingSink::Options recording;
    unsigned keep_days = 0;  ///< calendar-day age by trading date; zero disables
    std::uint64_t max_bytes = 0;  ///< complete files plus this writer; zero disables
    /// Optional unlink replacement for testing retention failures.
    std::function<bool(const std::filesystem::path&, std::error_code&)> remove{};
  };
  RecordingDirectory(std::filesystem::path directory, RecordingHeader header,
                     EventSink& downstream, Options options);
  ~RecordingDirectory() override;
  void publish(Event event) override;
  /// Call periodically even when the feed is idle, and after publishers stop close().
  void rollover();
  void close();
  [[nodiscard]] std::string error() const;
  /// Retention problems (undeletable files, stale staging folders); recording continues.
  [[nodiscard]] std::string warning() const;
  [[nodiscard]] RecordingStats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace openport::md
