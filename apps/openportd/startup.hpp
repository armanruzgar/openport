#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio/ip/address.hpp>
#include "openport/server/web_policy.hpp"
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace openport::startup {

/// Where a shell finds `name` on `directories`, a PATH value: the first executable
/// regular file. A name with a slash is a path already; an empty entry is the working
/// directory. Empty when nothing is found.
inline std::filesystem::path search_path(std::string_view name, std::string_view directories) {
  if (name.find('/') != std::string_view::npos) return std::filesystem::path(name);
  if (name.empty()) return {};
  std::error_code ec;
  for (std::size_t start = 0;;) {
    const auto colon = directories.find(':', start);
    const auto entry = directories.substr(start, colon == std::string_view::npos ? colon : colon - start);
    const auto candidate = (entry.empty() ? std::filesystem::path(".") : std::filesystem::path(entry)) /
                           std::filesystem::path(name);
    if (std::filesystem::is_regular_file(candidate, ec) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
    if (colon == std::string_view::npos) return {};
    start = colon + 1;
  }
}

/// The running executable, to find what was installed beside it. argv[0] is not
/// enough: a shell that found openportd on PATH passes only the name, which would
/// resolve against the working directory.
inline std::filesystem::path executable_path(const char* argv0) {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  (void)_NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (size > 0 && _NSGetExecutablePath(buffer.data(), &size) == 0) {
    buffer.resize(std::strlen(buffer.c_str()));
    if (!buffer.empty()) return std::filesystem::path(buffer);
  }
#elif defined(__linux__)
  std::error_code ec;
  if (auto self = std::filesystem::read_symlink("/proc/self/exe", ec); !ec && !self.empty()) return self;
#endif
  const char* directories = std::getenv("PATH");
  return search_path(argv0 ? argv0 : "", directories ? directories : "");
}

/// The built web terminal for `executable`: web/ beside it, the web/dist of the source
/// tree a build directory sits in, or share/openport/web of the prefix it is installed
/// in, through any symlink such as Homebrew's bin/ into its Cellar. Else web/dist in
/// `working`, the working directory.
inline std::filesystem::path web_root_beside(const std::filesystem::path& executable,
                                             const std::filesystem::path& working) {
  std::error_code ec;
  std::vector<std::filesystem::path> candidates;
  if (!executable.empty()) {
    auto resolved = std::filesystem::weakly_canonical(executable, ec);
    if (ec || resolved.empty()) resolved = std::filesystem::absolute(executable, ec);
    const auto directory = resolved.parent_path();
    candidates = {directory / "web", directory / "../../web/dist", directory / "../share/openport/web"};
  }
  candidates.push_back(working / "web/dist");
  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate / "index.html", ec)) {
      auto found = std::filesystem::weakly_canonical(candidate, ec);
      return (ec || found.empty()) ? candidate : found;
    }
  }
  return working / "web/dist";
}

/// Set by SIGINT, SIGTERM and a hangup: the server or the backtest stops.
inline std::atomic<bool> stop_requested{false};
/// Set by SIGHUP while a --token-file is configured: the server reloads it.
inline std::atomic<bool> reload_requested{false};

inline void request_stop(int) { stop_requested = true; }
inline void request_reload(int) { reload_requested = true; }

/// Whether `number` is ignored, as nohup leaves SIGHUP for the program it starts.
inline bool signal_ignored(int number) {
  struct sigaction current {};
  if (sigaction(number, nullptr, &current) != 0) return false;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"  // glibc and Darwin spell SIG_IGN as a C cast.
  return current.sa_handler == SIG_IGN;
#pragma GCC diagnostic pop
}

/// SIGINT and SIGTERM stop. SIGHUP reloads the token file when `reloads`; otherwise it
/// is the terminal or SSH session closing, and stops too, unless it was ignored when the
/// process started, as nohup leaves it.
inline void handle_stop_signals(bool reloads) {
  const bool ignored = signal_ignored(SIGHUP);
  if (reloads) std::signal(SIGHUP, request_reload);
  else if (!ignored) std::signal(SIGHUP, request_stop);
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
}

inline std::string terminal_url(const std::string& address, unsigned short port, std::string_view token) {
  // A host name such as localhost is not an IP literal; the link names it as given.
  boost::system::error_code error;
  const auto ip = boost::asio::ip::make_address(address, error);
  const auto host = error ? address : ip.is_unspecified() ? "localhost"
      : ip.is_v6() ? "[" + ip.to_string() + "]" : ip.to_string();
  std::string url = "http://" + host + ":" + std::to_string(port) + "/";
  if (!token.empty()) {
    // Encoded as encodeURIComponent does, which the terminal decodes.
    url += "#token=" + server::url_component(token);
  }
  return url;
}

/// What startup prints about a write token kept in a file. With `with_link` false
/// the token stays out of stdout, which a service manager may keep in a shared log.
inline std::string token_file_notice(const std::filesystem::path& file, const std::string& address,
                                     unsigned short port, std::string_view token, bool with_link) {
  if (!with_link)
    return "write token: kept in " + file.string() + "; for its link, run as a user who can read it:\n"
           "  openportd --print-url --write-token-file " + file.string() + " --address " + address +
           " --port " + std::to_string(port) + "\n";
  return "write token: kept in " + file.string() + "; to trade, open this once in a browser tab:\n  " +
         terminal_url(address, port, token) + "\n";
}

inline bool open_browser(const std::string& url) {
#ifdef __APPLE__
  const char* command = "open";
#else
  const char* command = "xdg-open";
#endif
  // Pass one literal argument, never a token-bearing shell command. The argument
  // is still visible in ps while the launcher runs: open exits once the browser
  // has it, but some xdg-open backends stay as long as the browser does.
#ifndef __APPLE__
  if (url.find("#token=") != std::string::npos)
    std::fprintf(stderr, "openportd: xdg-open's command line shows the token link to other local users "
                         "while it runs; on a shared machine, use --print-url instead of --open\n");
#endif
  char* args[] = {const_cast<char*>(command), const_cast<char*>(url.c_str()), nullptr};
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return false;
  const bool redirected = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
      posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
      posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
  pid_t child;
  const int result = redirected ? posix_spawnp(&child, command, &actions, nullptr, args, environ) : -1;
  posix_spawn_file_actions_destroy(&actions);
  if (result != 0) return false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    int status = 0;
    const auto done = waitpid(child, &status, WNOHANG);
    if (done == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (done < 0 && errno != EINTR) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Some xdg-open backends live as long as the browser. Reap them without
  // holding up ticks, shutdown or the short-lived --print-url command.
  std::thread([child] {
    int status = 0;
    while (waitpid(child, &status, 0) < 0) if (errno != EINTR) return;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
      std::fprintf(stderr, "openportd: browser launcher failed; use --print-url to get the link\n");
  }).detach();
  return true;
}

inline volatile sig_atomic_t crash_fd = -1;

inline void fatal_signal(int signal) {
  // Only async-signal-safe calls and constant text: no heap, stack walk or secrets.
  const char* text = "fatal signal\n";
  std::size_t size = 13;
  switch (signal) {
    case SIGABRT: text = "fatal SIGABRT\n"; size = 14; break;
    case SIGSEGV: text = "fatal SIGSEGV\n"; size = 14; break;
    case SIGBUS: text = "fatal SIGBUS\n"; size = 13; break;
    case SIGILL: text = "fatal SIGILL\n"; size = 13; break;
    case SIGFPE: text = "fatal SIGFPE\n"; size = 13; break;
  }
  if (crash_fd >= 0) { const auto written = ::write(crash_fd, text, size); (void)written; }
  // SA_RESETHAND restores the default action before entering this handler.
  ::raise(signal);
}

inline void install_crash_log(const std::filesystem::path& path, std::string_view build) {
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
  if (fd < 0) throw std::runtime_error("cannot open local crash log");
  struct stat info {};
  if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
      info.st_nlink != 1 || fchmod(fd, 0600) != 0) {
    ::close(fd);
    throw std::runtime_error("crash log must be an owner-only regular file");
  }
  const std::string line = "started " + std::string(build) + "\n";
  if (::write(fd, line.data(), line.size()) != static_cast<ssize_t>(line.size())) {
    ::close(fd);
    throw std::runtime_error("cannot write local crash log");
  }
  crash_fd = fd;
  struct sigaction action {};
  action.sa_handler = fatal_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND;
  for (const int signal : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE})
    if (sigaction(signal, &action, nullptr) != 0) throw std::runtime_error("cannot install fatal signal handler");
}

}  // namespace openport::startup
