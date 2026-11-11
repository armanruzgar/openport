#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace openport::test {

/// A new, empty directory under the system temp directory, removed with all it
/// holds when the holder goes out of scope. A test that fails an ASSERT_ or throws
/// leaves its body early, and the destructor still runs, so a failing run leaves
/// no journal behind. mkdtemp names it, so test processes running at once (or a
/// reused process id) never share one.
class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view prefix = "openport-test") {
    std::string pattern = (std::filesystem::temp_directory_path() / (std::string(prefix) + "-XXXXXX")).string();
    if (::mkdtemp(pattern.data()) == nullptr)
      throw std::runtime_error("cannot create a " + std::string(prefix) + " test directory");
    path_ = pattern;
  }
  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  /// A path for `name` inside the directory.
  [[nodiscard]] std::string file(std::string_view name) const { return (path_ / name).string(); }

 private:
  std::filesystem::path path_;
};

}  // namespace openport::test
