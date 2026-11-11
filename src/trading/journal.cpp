#include "openport/trading/journal.hpp"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>

namespace openport::trading {
namespace {
using Json = nlohmann::json;
const std::string genesis(64, '0');
[[noreturn]] void corrupt(std::string message) { throw TradingError(Reason::JOURNAL_CORRUPT, std::move(message)); }
[[noreturn]] void io(std::string message) { throw TradingError(Reason::JOURNAL_IO, std::move(message)); }
std::string digest(const std::string& input) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (EVP_Digest(input.data(), input.size(), bytes, &size, EVP_sha256(), nullptr) != 1)
    io("SHA-256 failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned int i = 0; i < size; ++i) {
    result.push_back(hex[bytes[i] >> 4]);
    result.push_back(hex[bytes[i] & 15]);
  }
  return result;
}
/// A record must survive power loss, such as a laptop's battery running out. On macOS
/// fsync leaves data in the drive's cache; F_FULLFSYNC asks the drive to write it.
bool full_sync(int fd) {
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}
/// A write the disk runs out for would tear the journal, so appends stop short of it.
void reserve_space(std::string_view directory, std::uint64_t device, std::uint64_t available, std::uint64_t bytes) {
  if (available < kMinimumFreeBytes || available - kMinimumFreeBytes < bytes)
    io("Disk nearly full: filesystem device " + std::to_string(device) + " (directory " + std::string(directory) +
       ") has " + std::to_string(available) + " free bytes; reserve " + std::to_string(kMinimumFreeBytes) +
       " bytes (64 MiB); free space and restart");
}
std::string directory_name(const std::filesystem::path& file) {
  const auto name = std::filesystem::absolute(file).parent_path().filename().string();
  return name.empty() ? "root" : name;
}
std::string read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) io("Cannot read journal");
  std::ostringstream contents;
  contents << file.rdbuf();
  if (file.bad()) io("Journal read failed");
  return contents.str();
}
int open_locked(const std::string& path, bool create, bool read_only = false) {
  const int fd = ::open(path.c_str(), (read_only ? O_RDONLY : O_WRONLY | O_APPEND) | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0), 0600);
  if (fd < 0 && create && errno == EEXIST) {
    // Preserve create's no-overwrite contract, but report contention consistently,
    // including a concurrent creator winning the Engine's existence-check race.
    const int existing = open_locked(path, false);
    ::close(existing);
    io("Cannot create journal: file already exists");
  }
  if (fd < 0) io("Cannot open journal: " + std::string(std::strerror(errno)));
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    io("Journal must be a regular file");
  }
  // flock belongs to this open file description, unlike process-owned fcntl locks:
  // another open in this process conflicts, and closing recovery reads cannot unlock it.
  int result;
  do { result = ::flock(fd, LOCK_EX | LOCK_NB); } while (result != 0 && errno == EINTR);
  if (result != 0) {
    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK || error == EAGAIN)
      throw TradingError(Reason::JOURNAL_LOCKED, "paper journal is in use by another openportd; use --paper-journal to choose another file or --no-paper");
    io("Cannot lock paper journal: " + std::string(std::strerror(error)));
  }
  // The lock's holder may have removed an empty journal between this open and the
  // lock: writing to the file the path no longer names would lose every record.
  struct stat named {};
  if (::stat(path.c_str(), &named) != 0 || named.st_dev != info.st_dev || named.st_ino != info.st_ino) {
    ::close(fd);
    io("Journal was removed or replaced while opening it");
  }
  return fd;
}
JournalRecovery verify_prefix(std::string_view jsonl, std::string_view expected_head, bool prefix) {
  JournalRecovery result;
  result.head = genesis;
  std::size_t start = 0;
  try {
    while (start < jsonl.size()) {
      const auto end = jsonl.find('\n', start);
      if (end == std::string_view::npos) { result.truncated_final_line = true; result.bytes_cut = jsonl.size() - start; break; }
      const auto line = jsonl.substr(start, end - start);
      Json j = Json::parse(line);
      if (!j.is_object() || j.size() != 6 || !j.at("payload").is_object() ||
          !j.at("seq").is_number_unsigned() || !j.at("time").is_number_integer())
        corrupt("Invalid journal envelope");
      const auto hash = j.at("hash").get<std::string>();
      // Require our canonical serialization, rejecting duplicate keys and whitespace rewrites.
      if (j.dump() != line) corrupt("Noncanonical journal line");
      j.erase("hash");
      const auto seq = j.at("seq").get<std::uint64_t>();
      const auto time = j.at("time").get<Timestamp>();
      if (seq != result.records.size() + 1 || time < 0 ||
          (!result.records.empty() && time < result.records.back().time) ||
          j.at("prev_hash") != result.head || hash != digest(j.dump()))
        corrupt("Broken journal sequence, time or SHA-256 chain at record " + std::to_string(seq));
      result.records.push_back({seq, time, j.at("type").get<std::string>(),
          j.at("payload").dump(), result.head, hash, j.at("payload").value("actor", std::string("unknown"))});
      result.head = hash;
      start = end + 1;
    }
    if (!expected_head.empty() && result.head != expected_head) corrupt("Journal does not match trusted head");
  } catch (const Json::exception&) {
    if (!prefix) corrupt("Invalid journal JSON at record " + std::to_string(result.records.size() + 1));
    result.damage = "Invalid journal JSON at record " + std::to_string(result.records.size() + 1);
  } catch (const TradingError& error) {
    if (!prefix || error.code() != Reason::JOURNAL_CORRUPT) throw;
    result.damage = error.what();
  }
  if (prefix && result.truncated_final_line) result.damage = "Torn final journal line";
  return result;
}
}  // namespace
void check_storage_space(const std::filesystem::path& file, std::uint64_t bytes) {
  auto directory = std::filesystem::absolute(file).parent_path();
  struct statvfs space {};
  struct stat info {};
  if (::statvfs(directory.c_str(), &space) == 0 && ::stat(directory.c_str(), &info) == 0)
    reserve_space(directory_name(file), info.st_dev, static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize, bytes);
}
JournalRecovery verify_journal(std::string_view jsonl, std::string_view expected_head) {
  return verify_prefix(jsonl, expected_head, false);
}
FileJournal::FileJournal(int fd, std::uint64_t sequence, std::string head, Timestamp last_time, Options options, std::string directory)
    : fd_(fd), sequence_(sequence), head_(std::move(head)), last_time_(last_time), options_(std::move(options)), directory_(std::move(directory)) {}
std::uint64_t FileJournal::bytes() const {
  struct stat info {};
  return ::fstat(fd_, &info) == 0 ? static_cast<std::uint64_t>(info.st_size) : 0;
}
std::pair<std::shared_ptr<FileJournal>, JournalRecovery> FileJournal::inspect(const std::string& path) {
  const int fd = open_locked(path, false, true);
  try {
    auto recovery = read_prefix(path);
    auto journal = std::shared_ptr<FileJournal>(new FileJournal(fd, recovery.records.size(), recovery.head,
        recovery.records.empty() ? 0 : recovery.records.back().time, {}, directory_name(path)));
    journal->failed_ = true;
    return {std::move(journal), std::move(recovery)};
  } catch (...) { ::close(fd); throw; }
}
FileJournal::~FileJournal() {
  try { flush(); } catch (...) {}  // Explicit boundaries report failures; destruction cannot throw.
  if (fd_ >= 0) ::close(fd_);
}
std::shared_ptr<FileJournal> FileJournal::create(const std::string& path) {
  return create(path, Options{});
}
std::shared_ptr<FileJournal> FileJournal::create(const std::string& path, Options options) {
  return std::shared_ptr<FileJournal>(new FileJournal(open_locked(path, true), 0, genesis, 0, std::move(options), directory_name(path)));
}
JournalRecovery FileJournal::read(const std::string& path, std::string_view expected_head) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) io("Journal must be a readable regular file");
  return verify_journal(read_file(path), expected_head);
}
JournalRecovery FileJournal::read_prefix(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) io("Journal must be a readable regular file");
  return verify_prefix(read_file(path), {}, true);
}
std::shared_ptr<FileJournal> FileJournal::resume(const std::string& path) {
  return resume(path, Options{});
}
std::shared_ptr<FileJournal> FileJournal::resume(const std::string& path, Options options) {
  return resume_and_read(path, std::move(options)).first;
}
std::pair<std::shared_ptr<FileJournal>, JournalRecovery> FileJournal::resume_and_read(const std::string& path, Options options) {
  const int fd = open_locked(path, false);
  try {
    auto recovery = read(path);
    if (recovery.truncated_final_line)
      io("Torn journal suffix, as a full disk leaves: with openportd stopped, "
         "openportd --repair-journals cuts it off and keeps the original");
    auto journal = std::shared_ptr<FileJournal>(new FileJournal(fd, recovery.records.size(), recovery.head,
        recovery.records.empty() ? 0 : recovery.records.back().time, std::move(options), directory_name(path)));
    return {std::move(journal), std::move(recovery)};
  } catch (...) { ::close(fd); throw; }
}
JournalRepair FileJournal::repair(const std::string& path, bool dry_run) {
  const int fd = open_locked(path, false, dry_run);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  const auto contents = read_file(path);
  const auto recovery = verify_journal(contents);
  if (!recovery.truncated_final_line) return {0, {}, contents.empty(), recovery.records.size()};
  // A torn first record, the journal's only content, leaves an empty journal: it
  // held no transaction, so its account starts afresh.
  const auto end = contents.rfind('\n');
  const std::size_t keep = end == std::string::npos ? 0 : end + 1;
  if (dry_run) return {contents.size() - keep, {}, keep == 0, recovery.records.size()};
  check_storage_space(path, contents.size());
  char stamp[32];
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  ::gmtime_r(&now, &utc);
  std::strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &utc);
  JournalRepair result{contents.size() - keep, {}, keep == 0, recovery.records.size()};
  // A journal torn and repaired again within the same second takes the next free
  // name (-1, -2, ...): an earlier backup is never overwritten.
  int copy = -1;
  for (int taken = 0; copy < 0 && taken < 100; ++taken) {
    result.backup = path + ".torn-" + stamp + (taken == 0 ? std::string() : "-" + std::to_string(taken));
    copy = ::open(result.backup.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (copy < 0 && errno != EEXIST) break;
  }
  if (copy < 0) io("Cannot create journal repair backup: " + std::string(std::strerror(errno)));
  std::size_t done = 0;
  while (done < contents.size()) {
    const auto count = ::write(copy, contents.data() + done, contents.size() - done);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) { ::close(copy); io("Cannot write journal repair backup: " + std::string(std::strerror(errno))); }
    done += static_cast<std::size_t>(count);
  }
  const bool copied = full_sync(copy);
  ::close(copy);
  if (!copied) io("Cannot sync journal repair backup");
  if (::ftruncate(fd, static_cast<off_t>(keep)) != 0 || !full_sync(fd))
    io("Cannot cut the torn journal line: " + std::string(std::strerror(errno)));
  return result;
}
void FileJournal::remove(const std::string& path) {
  const int fd = open_locked(path, false);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  // Nothing is read: removing a damaged journal needs no verified chain.
  if (::unlink(path.c_str()) != 0) io("Cannot remove journal: " + std::string(std::strerror(errno)));
}
void FileJournal::append(Timestamp time, std::string_view type, std::string_view payload) {
  if (failed_) io("Journal is latched failed; recover before trading");
  if (sequence_ == std::numeric_limits<std::uint64_t>::max()) io("Journal sequence exhausted");
  try {
    Json data = Json::parse(payload);
    if (!data.is_object() || time < last_time_) io("Invalid journal payload/time");
    Json j{{"seq", sequence_ + 1}, {"time", time}, {"type", type}, {"payload", std::move(data)}, {"prev_hash", head_}};
    const std::string hash = digest(j.dump());
    j["hash"] = hash;
    const std::string line = j.dump() + '\n';
    struct statvfs space {};
    struct stat info {};
    if (::fstat(fd_, &info) == 0 && (options_.hooks.free_bytes || ::fstatvfs(fd_, &space) == 0)) {
      const auto available = options_.hooks.free_bytes ? options_.hooks.free_bytes(fd_) :
          static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize;
      reserve_space(directory_, info.st_dev, available, line.size());
    }
    std::size_t done = 0;
    while (done < line.size()) {
      const auto count = ::write(fd_, line.data() + done, line.size() - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) io("Journal write failed: " + std::string(std::strerror(errno)));
      done += static_cast<std::size_t>(count);
    }
    pending_ = true;
    if (options_.sync_policy == SyncPolicy::PerRecord || !synced_ ||
        options_.hooks.clock() - last_sync_ >= options_.sync_interval) flush();
    ++sequence_;
    head_ = hash;
    last_time_ = time;
  } catch (const TradingError&) { failed_ = true; throw; }
    catch (const std::exception& e) { failed_ = true; io("Journal append failed: " + std::string(e.what())); }
}
void FileJournal::flush() {
  if (failed_) io("Journal is latched failed; recover before trading");
  if (!pending_) return;
  try {
    if (!(options_.hooks.sync ? options_.hooks.sync(fd_) : full_sync(fd_)))
      io("Journal sync failed: " + std::string(std::strerror(errno)));
    last_sync_ = options_.hooks.clock();
    synced_ = true;
    pending_ = false;
  } catch (const TradingError&) { failed_ = true; throw; }
    catch (const std::exception& e) { failed_ = true; io("Journal sync failed: " + std::string(e.what())); }
    catch (...) { failed_ = true; io("Journal sync failed"); }
}
}  // namespace openport::trading
