#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace openport::md {

/// How far write_file_atomic goes to keep new contents through a power loss.
enum class Durability : unsigned char {
  /// A crash of the process leaves the old contents or the new ones, whole. A power
  /// loss can lose the new ones, or leave the file empty on a filesystem that does
  /// not order the rename after the data. For caches that are rebuilt or refetched.
  Cache,
  /// The data reaches the drive before the rename (F_FULLFSYNC where the system has
  /// it) and the directory is synced after it where the filesystem allows, so a power
  /// loss leaves one version whole.
  Durable,
};

/// Asks the drive to keep what was written to `fd`: F_FULLFSYNC where the system has
/// it (macOS, whose fsync leaves data in the drive's cache), else fsync.
[[nodiscard]] bool full_sync(int fd) noexcept;
/// Syncs a directory, so an entry created or renamed in it survives a power loss. An
/// empty path is the working directory.
[[nodiscard]] bool sync_directory(const std::filesystem::path& directory) noexcept;

/// Replaces `file` with `bytes` in one step. They are written to `file` + ".tmp",
/// created afresh owner-only (mode 0600, whatever the umask) without following a
/// symlink, synced as `durability` says, and renamed over `file`, which is therefore
/// owner-only too. A failure throws std::runtime_error naming the file (not its
/// directory) and the cause, and leaves `file` as it was. Writers of one file must
/// not overlap.
void write_file_atomic(const std::filesystem::path& file, std::string_view bytes,
                       Durability durability = Durability::Durable);

/// Appends `bytes` to `file`, creating it owner-only (0600) when it is missing and
/// taking group and other permissions off an existing one. Never follows a symlink and
/// does not sync. A failure throws std::runtime_error naming the file and the cause.
void append_file(const std::filesystem::path& file, std::string_view bytes);

/// Takes group and other permissions off `file` when it is a regular file, as files
/// written before writes were owner-only need. A missing file or a symlink is left alone.
void restrict_file(const std::filesystem::path& file) noexcept;

/// Creates `directory` and any missing parent, each new one owner-only (0700).
/// Directories that already exist keep their permissions. Throws
/// std::filesystem::filesystem_error, or reports through `error`.
void create_private_directories(const std::filesystem::path& directory);
void create_private_directories(const std::filesystem::path& directory, std::error_code& error) noexcept;

}  // namespace openport::md
