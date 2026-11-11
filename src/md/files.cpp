#include "openport/md/files.hpp"
#include "openport/md/time.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace openport::md {
namespace {
/// Messages name the file, not its directory, so they can reach the API and status.
[[noreturn]] void fail(const char* action, const std::filesystem::path& file, const char* cause) {
  throw std::runtime_error(std::string(action) + " " + file.filename().string() + ": " + cause);
}
[[noreturn]] void fail(const char* action, const std::filesystem::path& file, int error) {
  fail(action, file, std::strerror(error));
}
/// Writes all of `bytes`, resuming after a short write or a signal; errno says why not.
bool write_all(int fd, std::string_view bytes) noexcept {
  while (!bytes.empty()) {
    const auto written = ::write(fd, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      if (written == 0) errno = EIO;
      return false;
    }
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  return true;
}
constexpr mode_t owner_only(mode_t mode) { return static_cast<mode_t>(mode & S_IRWXU); }
}  // namespace

bool full_sync(int fd) noexcept {
#ifdef F_FULLFSYNC
  if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
  return ::fsync(fd) == 0;
}

bool sync_directory(const std::filesystem::path& directory) noexcept {
  const int fd = ::open(directory.empty() ? "." : directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0;
  const int cause = errno;
  ::close(fd);
  errno = cause;
  return synced;
}

void write_file_atomic(const std::filesystem::path& file, std::string_view bytes, Durability durability) {
  auto temporary = file;
  temporary += ".tmp";
  // Whatever holds the temporary's name (a crash's leftover, a symlink) goes first, so
  // the exclusive create below makes a new file that only this process can have opened.
  // Anything unlink cannot remove, such as a directory, makes the create fail.
  ::unlink(temporary.c_str());
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) fail("cannot create", temporary, errno);
  // 0600 even under a umask that would have left the owner less.
  bool written = ::fchmod(fd, 0600) == 0 && write_all(fd, bytes) &&
                 (durability == Durability::Cache || full_sync(fd));
  int cause = written ? 0 : errno;
  if (::close(fd) != 0 && written) {
    written = false;
    cause = errno;
  }
  if (!written) {
    ::unlink(temporary.c_str());
    fail("cannot write", temporary, cause);
  }
  if (::rename(temporary.c_str(), file.c_str()) != 0) {
    cause = errno;
    ::unlink(temporary.c_str());
    fail("cannot replace", file, cause);
  }
  // The new contents are in place now. A directory the filesystem cannot sync leaves
  // them there rather than reporting a write that took effect as failed.
  if (durability == Durability::Durable) (void)sync_directory(file.parent_path());
}

void append_file(const std::filesystem::path& file, std::string_view bytes) {
  const int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (fd < 0) fail("cannot open", file, errno);
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    fail("cannot append to", file, "not a regular file");
  }
  // A file made before writes were owner-only loses group and other access here; one
  // owned by another user stays as it is and is still appended to.
  if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) ::fchmod(fd, owner_only(info.st_mode));
  bool written = write_all(fd, bytes);
  int cause = written ? 0 : errno;
  if (::close(fd) != 0 && written) {
    written = false;
    cause = errno;
  }
  if (!written) fail("cannot append to", file, cause);
}

void restrict_file(const std::filesystem::path& file) noexcept {
  const int fd = ::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return;
  struct stat info {};
  if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && (info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    ::fchmod(fd, owner_only(info.st_mode));
  ::close(fd);
}

std::filesystem::path keep_copy(const std::filesystem::path& file, std::string_view tag) {
  std::string suffix = "." + std::string(tag) + "-";
  for (const char c : format_timestamp(now()).substr(0, 19))  // 2026-11-11T01:23:45
    if (c != '-' && c != ':') suffix += c;
  suffix += 'Z';
  auto copy = file;
  copy += suffix;
  for (int n = 2; std::filesystem::exists(copy); ++n) {
    copy = file;
    copy += suffix + "-" + std::to_string(n);
  }
  std::filesystem::copy_file(file, copy);
  restrict_file(copy);
  return copy;
}

std::optional<std::filesystem::path> cut_torn_tail(const std::filesystem::path& file) {
  const int fd = ::open(file.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT) return std::nullopt;
    fail("cannot open", file, errno);
  }
  struct Close {
    int fd;
    ~Close() { ::close(fd); }
  } closer{fd};
  struct stat info {};
  if (::fstat(fd, &info) != 0) fail("cannot read", file, errno);
  if (!S_ISREG(info.st_mode)) fail("cannot repair", file, "not a regular file");
  const auto size = static_cast<std::uint64_t>(info.st_size);
  char last = '\n';
  if (size == 0) return std::nullopt;
  if (::pread(fd, &last, 1, static_cast<off_t>(size - 1)) != 1) fail("cannot read", file, errno);
  if (last == '\n') return std::nullopt;
  // Everything up to and including the last newline stays.
  std::uint64_t keep = 0;
  std::array<char, 4096> block{};
  for (std::uint64_t end = size; end > 0 && keep == 0;) {
    const std::uint64_t start = end > block.size() ? end - block.size() : 0;
    const auto count = static_cast<std::size_t>(end - start);
    if (::pread(fd, block.data(), count, static_cast<off_t>(start)) != static_cast<ssize_t>(count)) fail("cannot read", file, errno);
    for (auto i = count; i > 0; --i)
      if (block[i - 1] == '\n') {
        keep = start + i;
        break;
      }
    end = start;
  }
  auto copy = keep_copy(file, "torn");
  if (::ftruncate(fd, static_cast<off_t>(keep)) != 0 || !full_sync(fd)) fail("cannot cut the torn line of", file, errno);
  return copy;
}

void create_private_directories(const std::filesystem::path& directory, std::error_code& error) noexcept {
  error.clear();
  try {
    std::filesystem::path path;
    for (const auto& part : directory) {
      if (part.empty()) continue;  // a trailing separator
      path /= part;
      struct stat info {};
      if (::stat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) continue;
        error = std::make_error_code(std::errc::not_a_directory);
        return;
      }
      if (errno != ENOENT) {
        error = std::error_code(errno, std::generic_category());
        return;
      }
      if (::mkdir(path.c_str(), 0700) == 0) continue;
      const int cause = errno;
      // Another creator can win between the stat and the mkdir.
      if (cause == EEXIST && ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) continue;
      error = std::error_code(cause, std::generic_category());
      return;
    }
  } catch (const std::exception&) {
    error = std::make_error_code(std::errc::not_enough_memory);
  }
}

void create_private_directories(const std::filesystem::path& directory) {
  std::error_code error;
  create_private_directories(directory, error);
  if (error) throw std::filesystem::filesystem_error("cannot create directory", directory, error);
}

}  // namespace openport::md
