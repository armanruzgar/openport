#pragma once

#include "openport/md/recording.hpp"

namespace openport::md {

/// One independently replayable file per receipt-clock trading date. Open files
/// stay in private staging directories; only clean closes become visible.
class RecordingDirectory final : public EventSink {
 public:
  struct Options {
    RecordingSink::Options recording;
    unsigned keep_days = 0;  ///< calendar-day age by trading date; zero disables
    std::uint64_t max_bytes = 0;  ///< complete files plus this writer; zero disables
  };
  RecordingDirectory(std::filesystem::path directory, RecordingHeader header,
                     EventSink& downstream, Options options);
  ~RecordingDirectory() override;
  void publish(Event event) override;
  /// Call periodically even when the feed is idle, and after publishers stop close().
  void rollover();
  void close();
  [[nodiscard]] std::string error() const;
  [[nodiscard]] RecordingStats stats() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace openport::md
