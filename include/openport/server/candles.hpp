#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/bars.hpp"

namespace openport::server {

enum class BarInterval : std::uint8_t { Minute, FiveMinutes, FifteenMinutes, ThirtyMinutes, Hour, Day };

/// "1m", "5m", "15m", "30m", "1h" or "1d".
[[nodiscard]] std::optional<BarInterval> parse_bar_interval(std::string_view text) noexcept;
[[nodiscard]] std::string_view to_string(BarInterval interval) noexcept;

/// Price history of each underlying, for charts. One-minute bars are built from its
/// prints, or from the spot the engine infers from parity when it has none; a
/// vendor's official bars replace them minute by
/// minute where it has them, and later samples never alter an official minute.
/// Intraday intervals group minutes from the Unix epoch, except hours, which start
/// at half past so they line up with the 09:30 ET open. Daily bars are the
/// vendor's, and a session it has not published yet is built from its regular-hours
/// minutes. Thread-safe.
///
/// With a directory, minutes are appended to <directory>/<SYMBOL>.csv as they
/// finish, one "start seconds,open,high,low,close,source" line each (source o for
/// official, s for sampled; a later line for the same minute wins), reloaded on
/// construction, and compacted to the retained window. Daily bars stay in memory.
class CandleStore {
 public:
  struct Options {
    std::filesystem::path directory;  ///< empty keeps history in memory only
    std::chrono::days keep{10};       ///< minute history kept per underlying
    std::size_t max_days = 2600;      ///< daily bars kept per underlying, about ten years
    /// A live feed's wall clock. A minute starting more than kMaxLead after it is
    /// ignored, whether sampled, merged or read back from a file, so one mis-stamped
    /// sample or bar cannot become the newest minute and prune the real ones before it.
    /// Empty for demo and replay, whose market time can run ahead of the wall clock.
    std::function<md::Timestamp()> clock = {};
  };
  static constexpr md::Timestamp kMaxLead = 5 * md::kNanosPerMinute;

  CandleStore() : CandleStore(Options{}) {}
  explicit CandleStore(Options options);
  ~CandleStore();
  CandleStore(const CandleStore&) = delete;
  CandleStore& operator=(const CandleStore&) = delete;

  /// The underlying was at `price` at market-data time `time`. Ignored unless the
  /// price is finite and positive.
  void sample(const std::string& symbol, md::Timestamp time, double price);
  /// Official one-minute bars, each starting on a whole minute. Invalid bars are skipped.
  void merge_minutes(const std::string& symbol, const std::vector<md::Bar>& bars);
  /// Official daily bars, each starting at its session's 09:30 ET open.
  void merge_days(const std::string& symbol, const std::vector<md::Bar>& bars);

  /// The most recent `limit` bars at `interval`, oldest first.
  [[nodiscard]] std::vector<md::Bar> bars(const std::string& symbol, BarInterval interval,
                                          std::size_t limit) const;

  /// Daily history for estimators, including close-only rows with invalid OHLC.
  [[nodiscard]] std::vector<md::Bar> daily_history(const std::string& symbol) const;

  /// Writes every minute that is not on disk yet, including one still forming.
  void flush();
  /// The latest storage failure; storage never throws after construction.
  [[nodiscard]] std::string error() const;

 private:
  struct Minute {
    md::Bar bar;
    md::Timestamp first = 0;  ///< time of the sample that set the open
    md::Timestamp last = 0;   ///< time of the sample that set the close
    bool official = false;
    bool saved = false;
  };
  struct Series {
    std::map<md::Timestamp, Minute> minutes;
    std::map<md::Timestamp, md::Bar> days;
    std::size_t lines = 0;  ///< lines in its file, compacted when mostly superseded
  };

  void load(const std::filesystem::path& file);
  void persist(const std::string& symbol, Series& series, bool forming);
  void compact(const std::string& symbol, Series& series);
  void prune(Series& series) const;
  [[nodiscard]] bool ahead(md::Timestamp start) const;
  [[nodiscard]] std::vector<md::Bar> sessions(const Series& series, std::size_t limit) const;
  [[nodiscard]] std::filesystem::path file_for(const std::string& symbol) const;

  Options options_;
  mutable std::mutex mutex_;
  std::map<std::string, Series> series_;
  std::string error_;
};

}  // namespace openport::server
