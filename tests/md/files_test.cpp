#include "openport/md/files.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

namespace {
using namespace openport;

struct Directory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("openport-files-" + std::to_string(::getpid()) + "-" + ::testing::UnitTest::GetInstance()->current_test_info()->name());
  Directory() { std::filesystem::remove_all(path); std::filesystem::create_directories(path); }
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
/// The shell default, under which files made with plain streams are world-readable.
struct ShellUmask {
  mode_t previous = ::umask(022);
  ~ShellUmask() { ::umask(previous); }
};
std::string contents(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}
void put(const std::filesystem::path& file, const std::string& text, mode_t permissions) {
  { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << text; }
  ASSERT_EQ(::chmod(file.c_str(), permissions), 0);
}
unsigned mode_of(const std::filesystem::path& path) {
  struct stat info {};
  EXPECT_EQ(::lstat(path.c_str(), &info), 0) << path;
  return static_cast<unsigned>(info.st_mode) & 07777U;
}

TEST(Files, AtomicWriteReplacesAWorldReadableFileWithAnOwnerOnlyOne) {
  ShellUmask mask;
  Directory dir;
  const auto file = dir.path / "analysis.json";
  put(file, "old", 0644);
  put(dir.path / "analysis.json.tmp", "a crash's leftover", 0644);
  for (const auto durability : {md::Durability::Durable, md::Durability::Cache}) {
    md::write_file_atomic(file, "new\n", durability);
    EXPECT_EQ(contents(file), "new\n");
    EXPECT_EQ(mode_of(file), 0600U);
    EXPECT_FALSE(std::filesystem::exists(dir.path / "analysis.json.tmp"));
  }
  md::write_file_atomic(dir.path / "created.json", "");
  EXPECT_EQ(contents(dir.path / "created.json"), "");
  EXPECT_EQ(mode_of(dir.path / "created.json"), 0600U);
}

TEST(Files, AtomicWriteNeverWritesThroughASymlinkAtTheTemporaryName) {
  Directory dir;
  const auto file = dir.path / "playbooks.json";
  put(dir.path / "elsewhere", "keep", 0644);
  std::filesystem::create_symlink(dir.path / "elsewhere", dir.path / "playbooks.json.tmp");
  md::write_file_atomic(file, "{}\n");
  EXPECT_EQ(contents(dir.path / "elsewhere"), "keep");
  EXPECT_EQ(mode_of(dir.path / "elsewhere"), 0644U);
  EXPECT_EQ(contents(file), "{}\n");
  EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::symlink_status(file)));
}

TEST(Files, AFailedAtomicWriteLeavesTheFileAndNamesItWithoutItsDirectory) {
  Directory dir;
  const auto file = dir.path / "earnings.json";
  put(file, "old", 0600);
  std::filesystem::create_directories(dir.path / "earnings.json.tmp" / "in-the-way");
  try {
    md::write_file_atomic(file, "new");
    FAIL() << "a directory at the temporary's name must stop the write";
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("earnings.json.tmp"), std::string::npos) << message;
    EXPECT_EQ(message.find(dir.path.string()), std::string::npos) << message;
  }
  EXPECT_EQ(contents(file), "old");
  std::filesystem::remove_all(dir.path / "earnings.json.tmp");
  md::write_file_atomic(file, "new");
  EXPECT_EQ(contents(file), "new");
  EXPECT_THROW(md::write_file_atomic(dir.path / "missing" / "file.json", "x"), std::runtime_error);
}

TEST(Files, AppendsCreateOwnerOnlyFilesAndTightenOldOnes) {
  ShellUmask mask;
  Directory dir;
  const auto created = dir.path / "account.jsonl.equity.csv";
  md::append_file(created, "1\n");
  md::append_file(created, "2\n");
  EXPECT_EQ(contents(created), "1\n2\n");
  EXPECT_EQ(mode_of(created), 0600U);
  const auto old = dir.path / "old.equity.csv";
  put(old, "a\n", 0644);
  md::append_file(old, "b\n");
  EXPECT_EQ(contents(old), "a\nb\n");
  EXPECT_EQ(mode_of(old), 0600U);
  put(dir.path / "target", "keep", 0644);
  std::filesystem::create_symlink(dir.path / "target", dir.path / "link.csv");
  EXPECT_THROW(md::append_file(dir.path / "link.csv", "x"), std::runtime_error);
  EXPECT_EQ(contents(dir.path / "target"), "keep");
  EXPECT_THROW(md::append_file(dir.path, "x"), std::runtime_error);
}

TEST(Files, RestrictTakesGroupAndOtherAccessOffRegularFilesOnly) {
  Directory dir;
  put(dir.path / "readable", "x", 0644);
  put(dir.path / "executable", "x", 0755);
  md::restrict_file(dir.path / "readable");
  md::restrict_file(dir.path / "executable");
  EXPECT_EQ(mode_of(dir.path / "readable"), 0600U);
  EXPECT_EQ(mode_of(dir.path / "executable"), 0700U);
  put(dir.path / "target", "x", 0644);
  std::filesystem::create_symlink(dir.path / "target", dir.path / "link");
  md::restrict_file(dir.path / "link");
  EXPECT_EQ(mode_of(dir.path / "target"), 0644U);
  md::restrict_file(dir.path / "missing");
  EXPECT_FALSE(std::filesystem::exists(dir.path / "missing"));
}

TEST(Files, PrivateDirectoriesAreOwnerOnlyAndExistingOnesKeepTheirMode) {
  ShellUmask mask;
  Directory dir;
  ASSERT_EQ(::chmod(dir.path.c_str(), 0755), 0);
  md::create_private_directories(dir.path / ".openport" / "analysis");
  EXPECT_EQ(mode_of(dir.path), 0755U);
  EXPECT_EQ(mode_of(dir.path / ".openport"), 0700U);
  EXPECT_EQ(mode_of(dir.path / ".openport" / "analysis"), 0700U);
  md::create_private_directories(dir.path / ".openport" / "analysis");
  md::create_private_directories(dir.path / "trailing" / "");
  EXPECT_EQ(mode_of(dir.path / "trailing"), 0700U);
  put(dir.path / "file", "x", 0600);
  std::error_code error;
  md::create_private_directories(dir.path / "file" / "below", error);
  EXPECT_TRUE(error);
  EXPECT_THROW(md::create_private_directories(dir.path / "file" / "below"), std::filesystem::filesystem_error);
}
}  // namespace
