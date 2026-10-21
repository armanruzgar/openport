#pragma once

#include <array>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>

#include "openport/analytics/volatility.hpp"

namespace openport::server {
/// Retained minute starts lie in (newest - retention, newest], per symbol.
inline constexpr md::Timestamp kSeriesMinuteRetention = 45 * md::kNanosPerDay;
/// Surface snapshots older than this before a symbol's newest one are not indexed.
inline constexpr int kSurfaceRetentionDays = 400;
/// Surface month files this many months before a symbol's newest file are not loaded.
inline constexpr int kSurfaceLoadMonths = 13;
/// surface-history returns at most this many range snapshots, the newest ones.
inline constexpr std::size_t kSurfaceHistoryLimit = 400;

inline constexpr std::array<std::string_view, 22> kSeriesFields{
    "spot", "forward", "mfiv9", "mfiv30", "mfiv93", "mfiv182", "mfiv365",
    "atm7", "atm30", "atm60", "atm90", "atm180", "rr25", "bf25", "rr10", "bf10",
    "ratio9_30", "ratio30_93", "gex", "gamma_flip", "call_wall", "put_wall"};
struct SeriesRow {
  md::Timestamp time = 0;  ///< Start of the market minute; nanoseconds.
  std::array<double, kSeriesFields.size()> values;
  SeriesRow() { values.fill(analytics::kNaN); }
};
struct SeriesStatus {
  std::string directory;
  std::size_t rows_today = 0;
  md::Timestamp last_write = 0;  ///< Market time of the most recently persisted row.
  std::string last_error;
};
struct SurfaceExpirySnapshot {
  std::string id;
  md::Date expiry;
  double forward = analytics::kNaN;
  analytics::SviFit fit;
  bool calendar_ok = true;
};
struct SurfaceSnapshot {
  md::Timestamp time = 0;  ///< Actual market timestamp, not the scheduled capture time.
  std::string slot;  ///< 10:00, 12:00, 14:00, or close.
  double spot = analytics::kNaN;
  std::vector<SurfaceExpirySnapshot> expiries;
};

/// Thread-safe, append-only monthly CSVs. Empty directory is memory-only.
/// Duplicate minutes keep their first row; force appends a replacement only if changed.
/// Keeps 45 days of minutes and all daily close rows in memory. Older disk-backed
/// minutes are read on demand; older memory-only minutes are discarded.
/// Surface snapshots keep one record per session slot; a replaced record is rewritten
/// in place, and only kSurfaceRetentionDays of snapshots stay indexed.
class SeriesStore {
 public:
  explicit SeriesStore(std::filesystem::path directory = {});
  bool insert(const std::string& symbol, const SeriesRow& row, bool force = false);
  [[nodiscard]] bool contains(const std::string& symbol, md::Timestamp minute) const;
  [[nodiscard]] std::vector<SeriesRow> rows(const std::string& symbol, md::Timestamp from,
      md::Timestamp to, bool daily = false) const;
  [[nodiscard]] std::size_t cached_minutes(const std::string& symbol) const;
  [[nodiscard]] SeriesStatus status(md::Timestamp now) const;
  void report_error(std::string error);
  bool insert_surface(const std::string& symbol, const SurfaceSnapshot& snapshot, bool force = false);
  [[nodiscard]] std::vector<SurfaceSnapshot> surfaces(const std::string& symbol,
      md::Timestamp from, md::Timestamp to) const;
 private:
  void load_surfaces(const std::vector<std::filesystem::path>& paths);
  void load_surface_file(const std::filesystem::path& path);
  void evict_surfaces(const std::string& symbol);
  void load(const std::filesystem::path& path);
  void read_file(const std::filesystem::path& path, const std::function<void(const SeriesRow&)>& visit) const;
  [[nodiscard]] std::map<md::Timestamp, SeriesRow> read_minutes(const std::string& symbol,
      md::Timestamp from, md::Timestamp to) const;
  void remember(const std::string& symbol, const SeriesRow& row);
  void index_daily(const std::string& symbol, const SeriesRow& row) const;
  void refresh_calendar() const;
  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::map<std::string, std::map<md::Timestamp, SeriesRow>> rows_;
  mutable std::map<std::string, std::map<md::Date, SeriesRow>> daily_;
  std::map<std::string, std::map<std::string, std::filesystem::path>> files_;
  mutable std::vector<md::ScheduledDay> calendar_;
  mutable std::set<std::filesystem::path> damaged_;
  md::Timestamp last_write_ = 0;
  mutable std::string error_;
  std::map<std::string, std::map<std::pair<md::Date, std::string>, SurfaceSnapshot>> surfaces_;
};

[[nodiscard]] SeriesRow series_row(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
[[nodiscard]] std::string surface_slot(md::Timestamp time);
[[nodiscard]] SurfaceSnapshot surface_snapshot(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
class MetricsSource;
/// Owns its worker thread. Polling reads only immutable publications, once per minute
/// per symbol. sample() is also exposed for deterministic tests without sleeps.
class SeriesWorker {
 public:
  SeriesWorker(const MetricsSource& source, SeriesStore& store) : source_(source), store_(store) {}
  ~SeriesWorker();
  void start();
  void stop();
  void sample();
 private:
  const MetricsSource& source_;
  SeriesStore& store_;
  std::map<std::string, md::Timestamp> sampled_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stopping_ = false;
  std::thread thread_;
};

/// Includes recordings made by recording a replay of a demo.
[[nodiscard]] bool simulated_series_recording(std::string_view provider) noexcept;

/// Offline, ordered event replay; no provider, network, wall clock or trading engine.
/// Throws for invalid/demo recordings; store errors remain in SeriesStore::status.
std::size_t backfill_series(const std::filesystem::path& recording, SeriesStore& store,
                           bool force = false, const analytics::AnalyticsOptions& options = {});
}  // namespace openport::server
