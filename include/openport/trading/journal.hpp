#pragma once

#include <chrono>
#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "openport/trading/types.hpp"

namespace openport::trading {

struct JournalRecord {
  std::uint64_t seq = 0;
  Timestamp time = 0;
  std::string type;
  std::string payload;  ///< Canonical JSON object, sorted keys, compact UTF-8.
  std::string prev_hash;
  std::string hash;
  std::string actor = "unknown"; ///< Payload actor, or unknown for older records.
};
struct JournalRecovery {
  std::vector<JournalRecord> records;
  bool truncated_final_line = false;
  std::size_t bytes_cut = 0;  ///< Bytes a repair would remove from the torn final line.
  std::string head;
  std::string damage = {};  ///< Read-only inspection stops at the first unverified record.
};
/// What FileJournal::repair did.
struct JournalRepair {
  std::size_t bytes_cut = 0;  ///< the torn final line's length; 0 when the journal was whole
  std::string backup;         ///< where the original was copied, when a line was cut
  bool empty = false;         ///< it holds no record now, so its account starts afresh
  std::size_t records_kept = 0;
};

inline constexpr std::uint64_t kMinimumFreeBytes = 64ull * 1024 * 1024;
/// Path-free diagnostic naming the directory and filesystem device, for API use too.
void check_storage_space(const std::filesystem::path& file, std::uint64_t bytes);

/// Optional durable sink; all other trading components are filesystem-free.
/// One reducer transaction per newline contains all typed outcomes plus the
/// resulting state. append must write the complete line or throw JOURNAL_IO.
/// File journals sync each record by default; replay callers may defer durability
/// until an interval or flush. Completed writes survive a process crash either way.
/// A failure is indeterminate on disk: stop trading and recover before retrying.
class Journal {
 public:
  virtual ~Journal() = default;
  virtual void append(Timestamp time, std::string_view type, std::string_view payload) = 0;
  /// Sync pending records now. Memory sinks need no durability barrier.
  virtual void flush() {}
  [[nodiscard]] virtual std::uint64_t sequence() const = 0;
  [[nodiscard]] virtual std::string head() const = 0;
  [[nodiscard]] virtual std::uint64_t bytes() const { return 0; }
};

/// Exclusive single-writer file, O_EXCL on creation, immediate writes with optional
/// batched syncs. Single owner: append and flush must not run concurrently.
/// Non-blocking flock excludes other opens (including in this process) until destruction,
/// and an open fails if the path stops naming the file it locked (a holder removed it).
/// Resume requires a clean verified file. A torn suffix is never silently erased.
class FileJournal final : public Journal {
 public:
  enum class SyncPolicy { PerRecord, Batched };
  struct Hooks {
    /// Empty uses full disk sync. Injectable for counting and failure tests.
    std::function<bool(int)> sync;
    std::function<std::chrono::steady_clock::time_point()> clock = std::chrono::steady_clock::now;
    std::function<std::uint64_t(int)> free_bytes = {};  ///< Empty uses fstatvfs.
  };
  struct Options {
    SyncPolicy sync_policy = SyncPolicy::PerRecord;
    std::chrono::milliseconds sync_interval{250};
    Hooks hooks;
  };
  static std::shared_ptr<FileJournal> create(const std::string& path);
  static std::shared_ptr<FileJournal> create(const std::string& path, Options options);
  static std::shared_ptr<FileJournal> resume(const std::string& path);
  static std::shared_ptr<FileJournal> resume(const std::string& path, Options options);
  /// resume, with the verified records it read to find where the journal ends, so an
  /// account recovering from them reads and checks the file once, not twice.
  static std::pair<std::shared_ptr<FileJournal>, JournalRecovery> resume_and_read(const std::string& path, Options options);
  static JournalRecovery read(const std::string& path, std::string_view expected_head = {});
  /// Read-only diagnostics, stopping before the first damaged record; holds no writer lock.
  static JournalRecovery read_prefix(const std::string& path);
  /// Holds the file's lock without writing; returns only the verified prefix.
  static std::pair<std::shared_ptr<FileJournal>, JournalRecovery> inspect(const std::string& path);
  /// Cuts a torn final line, as a write the disk ran out for leaves, off a journal so
  /// that it resumes, after copying the original beside it as FILE.torn-YYYYMMDDTHHMMSSZ
  /// (with -1, -2, ... after it when an earlier repair that second took the name).
  /// A torn first record leaves the journal empty. It takes the writer's lock, so
  /// openportd must be stopped. A journal that verifies is left alone; damage before
  /// the last line throws JOURNAL_CORRUPT, changing nothing.
  static JournalRepair repair(const std::string& path, bool dry_run = false);
  /// Deletes a journal whatever its contents, torn or edited ones included, under the
  /// writer's lock: a file another open is writing throws JOURNAL_LOCKED and stays.
  static void remove(const std::string& path);
  ~FileJournal() override;
  FileJournal(const FileJournal&) = delete;
  FileJournal& operator=(const FileJournal&) = delete;
  void append(Timestamp time, std::string_view type, std::string_view payload) override;
  void flush() override;
  [[nodiscard]] std::uint64_t sequence() const override { return sequence_; }
  [[nodiscard]] std::string head() const override { return head_; }
  [[nodiscard]] std::uint64_t bytes() const override;
 private:
  FileJournal(int fd, std::uint64_t sequence, std::string head, Timestamp last_time, Options options, std::string directory);
  int fd_ = -1;
  std::uint64_t sequence_ = 0;
  std::string head_;
  bool failed_ = false;
  Timestamp last_time_ = 0;
  Options options_;
  std::chrono::steady_clock::time_point last_sync_{};
  bool synced_ = false;
  bool pending_ = false;
  std::string directory_;
};

/// Same verifier for in-memory captured JSONL; useful for imported audit files.
[[nodiscard]] JournalRecovery verify_journal(std::string_view jsonl,
                                             std::string_view expected_head = {});

}  // namespace openport::trading
