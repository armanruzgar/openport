#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "openport/trading/fill_quality.hpp"

namespace openport::server {
/// Bounds on one calibration's search of the recorded days, so a large directory
/// or a long export cannot hold a worker for long.
struct RecordingLimits {
  std::size_t headers = 10'000;      ///< Directory entries whose headers are read.
  std::size_t files = 32;            ///< Recordings read through.
  std::uintmax_t bytes = 4ULL << 30; ///< Their combined size.
  std::chrono::steady_clock::duration time = std::chrono::seconds(20);
};
/// How far a search got: `stopped` is "complete" (every candidate read),
/// "all_matched" (every timed fill had a quote), "file_limit", "byte_limit",
/// "time_limit" or "cancelled".
struct RecordingSearch {
  std::size_t files = 0;
  std::uintmax_t bytes = 0;
  std::string stopped = "complete";
};
struct RecordedQuotes {
  std::map<std::size_t, trading::CalibrationQuote> quotes;  ///< By BrokerFill::row.
  RecordingSearch search;
};
/// Timed fills may span at most this many days; longer exports are imported a month at a time.
inline constexpr int kCalibrationSpanDays = 31;
/// Throws std::invalid_argument when the timed fills span more than kCalibrationSpanDays.
void check_calibration_span(const std::vector<trading::BrokerFill>& fills);
/// The last recorded quote at or before each timed real fill, no older than
/// `max_age`, by BrokerFill::row, from the complete recordings in `directory`
/// that start on one of the fills' trading dates (or within the day before the
/// first). Recordings are read newest first, and the search stops once every
/// timed fill has a quote, at `limits`, or when `cancel` is set; a fill keeps the
/// latest quote any recording read gave it. Unreadable files are skipped; nothing
/// is written.
[[nodiscard]] RecordedQuotes recorded_quotes(const std::filesystem::path& directory, const std::vector<trading::BrokerFill>& fills,
                                             md::Timestamp max_age, const RecordingLimits& limits = {},
                                             const std::atomic_bool* cancel = nullptr);

/// Runs one calibration at a time on its own thread, off the web server's threads.
/// The destructor cancels a running search and waits for it.
class CalibrationWorker {
 public:
  CalibrationWorker() = default;
  ~CalibrationWorker();
  CalibrationWorker(const CalibrationWorker&) = delete;
  CalibrationWorker& operator=(const CalibrationWorker&) = delete;
  /// Starts `job` with the cancel flag it should poll; false while one is running.
  bool start(std::function<void(const std::atomic_bool& cancel)> job);

 private:
  std::mutex mutex_;
  bool running_ = false;
  std::atomic_bool cancel_{false};
  std::thread thread_;
};
}  // namespace openport::server
