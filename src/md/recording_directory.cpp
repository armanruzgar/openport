#include "openport/md/recording_directory.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace openport::md {
namespace {
class Discard final : public EventSink {
 public:
  void publish(Event) override {}
};
void add(RecordingStats& total, const RecordingStats& part) {
  total.events += part.events;
  total.bytes += part.bytes;
  total.publish_nanoseconds += part.publish_nanoseconds;
}
std::uint64_t folder_bytes(const std::filesystem::path& folder) {
  std::uint64_t bytes = 0;
  std::error_code ec;
  for (std::filesystem::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code status_error;
    if (std::filesystem::is_regular_file(it->symlink_status(status_error)) && !status_error) {
      const auto size = it->file_size(status_error);
      if (!status_error) bytes += size;
    }
  }
  return bytes;
}
}
struct RecordingDirectory::Impl {
  Impl(std::filesystem::path path, RecordingHeader original, EventSink& sink, Options settings)
      : directory(std::move(path)), header(std::move(original)), downstream(sink), options(std::move(settings)) {
    if (directory.empty()) throw std::invalid_argument("recording: directory is empty");
    std::filesystem::create_directories(directory);
    // Provider names may contain spaces/parentheses (replay), but never paths.
    name = header.provider;
    for (auto& ch : name)
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-')) ch = '_';
    if (name.empty()) name = "recording";
    // Staging folders present before this writer's own belong to killed (or
    // concurrent) writers. They count toward the byte budget but are never removed.
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code status_error;
      if (it->path().filename().string().starts_with(".record-") &&
          std::filesystem::is_directory(it->symlink_status(status_error)) && !status_error)
        stale.push_back(it->path());
    }
    std::sort(stale.begin(), stale.end());
    // One receipt time per event decides its file and stamps it.
    clock = options.recording.clock;
    options.recording.clock = [this] { return stamp.load(); };
    const auto time = clock();
    try { open(time); }
    catch (...) { cleanup(); throw; }
    retain(time);
    if (retaining()) pruner = std::thread([this] { work(); });
  }
  ~Impl() {
    stop();
    cleanup();
  }
  void cleanup() {
    recording.reset();
    if (!temporary.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(temporary, ignored);
      temporary.clear();
    }
  }
  void open(Timestamp time) {
    date = trading_date(time);
    header.started = time;
    stamp = time;
    auto pattern = (directory / ".record-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("recording: cannot create staging directory");
    temporary = pattern;
    const auto suffix = temporary.filename().string().substr(8);
    final = directory / (name + "-" + format_date(date) + "-" + std::to_string(time) + "-" + suffix + ".oprec");
    recording = std::make_unique<RecordingSink>(temporary / "day.oprec", header, discard, options.recording);
    for (const auto& [id, contract] : contracts) recording->publish(ContractDefinition{id, contract});
  }
  void finish() {
    if (!recording) return;
    recording->close();
    if (!recording->error().empty()) throw std::runtime_error(recording->error());
    const auto partial = temporary / "day.oprec";
    // Exclusive publication: a competing writer can never replace this day.
    std::filesystem::create_hard_link(partial, final);
    std::error_code ignored;
    if (std::filesystem::exists(partial.string() + ".end", ignored))
      std::filesystem::create_hard_link(partial.string() + ".end", final.string() + ".end", ignored);
    add(completed, recording->stats());
    cleanup();
  }
  void fail(const std::string& reason) {
    if (failure.empty()) {
      failure = "recording stopped: " + reason;
      downstream.publish(ProviderStatus{clock(), FeedState::Error, failure, ""});
    }
    if (recording) add(completed, recording->stats());
    cleanup();
  }
  /// Caller holds mutex; retention is only requested, never run, on this path.
  void roll(Timestamp time) {
    if (closed || !failure.empty()) return;
    try {
      if (!recording->error().empty()) throw std::runtime_error(recording->error());
      // A backward wall-clock adjustment cannot reopen an earlier trading day.
      if (trading_date(time) <= date) return;
      finish();
      open(time);
      request(time);
    } catch (const std::exception& error) { fail(error.what()); }
  }
  bool retaining() const { return options.keep_days || options.max_bytes; }
  void request(Timestamp time) {
    if (!retaining()) return;
    {
      const std::lock_guard lock(queue_mutex);
      requested = time;
    }
    wake.notify_one();
  }
  void work() {
    std::unique_lock lock(queue_mutex);
    for (;;) {
      wake.wait(lock, [&] { return stopping || requested.has_value(); });
      if (stopping) return;
      const auto time = *requested;
      requested.reset();
      lock.unlock();
      retain(time);
      lock.lock();
    }
  }
  void stop() {
    {
      const std::lock_guard lock(queue_mutex);
      stopping = true;
    }
    wake.notify_one();
    if (pruner.joinable()) pruner.join();
  }
  /// Never throws: retention problems become a warning and recording continues.
  void retain(Timestamp time) {
    const std::lock_guard serial(retention_mutex);
    std::vector<std::string> notes;
    std::uint64_t stale_bytes = 0;
    std::size_t stale_count = 0;
    for (const auto& folder : stale) {
      std::error_code ec;
      if (!std::filesystem::is_directory(std::filesystem::symlink_status(folder, ec)) || ec) continue;
      ++stale_count;
      stale_bytes += folder_bytes(folder);
    }
    if (stale_count)
      notes.push_back(std::to_string(stale_count) + " stale staging director" + (stale_count == 1 ? "y" : "ies") +
                      " (" + std::to_string(stale_bytes) + " bytes) in " + directory.string() +
                      "; remove .record-* folders once their writers have stopped");
    if (retaining()) {
      try { prune(time, stale_bytes, notes); }
      catch (const std::exception& error) { notes.push_back(error.what()); }
    }
    std::string message;
    for (const auto& note : notes) message += (message.empty() ? "recording retention: " : "; ") + note;
    const std::lock_guard lock(mutex);
    warning = std::move(message);
  }
  std::uint64_t writer_bytes() const {
    const std::lock_guard lock(mutex);
    return recording ? recording->stats().bytes : 0;
  }
  /// Caller holds retention_mutex.
  void prune(Timestamp time, std::uint64_t stale_bytes, std::vector<std::string>& notes) {
    struct Candidate {
      std::filesystem::path path;
      Timestamp started;
      std::uint64_t bytes, file_bytes;
      std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    std::map<std::filesystem::path, Scan> seen;
    std::uint64_t total = writer_bytes() + stale_bytes;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      std::error_code status_error;
      if (entry.path().extension() != ".oprec" || entry.path().filename().string().starts_with('.') ||
          !std::filesystem::is_regular_file(entry.symlink_status(status_error)) || status_error) continue;
      Scan scan;
      scan.file_bytes = entry.file_size(status_error);
      scan.modified = entry.last_write_time(status_error);
      if (status_error) continue;
      std::error_code ec;
      const auto sidecar = std::filesystem::file_size(entry.path().string() + ".end", ec);
      scan.sidecar_bytes = ec ? std::uint64_t{0} : static_cast<std::uint64_t>(sidecar);
      scan.sidecar = !ec;
      // Unchanged files keep their verdict; only new or changed files are read.
      if (const auto cached = scanned.find(entry.path()); cached != scanned.end() && cached->second.same(scan))
        scan.started = cached->second.started;
      else scan.started = verify(entry.path());
      seen.emplace(entry.path(), scan);
      if (scan.started <= 0) continue;
      const auto accounted = scan.file_bytes + scan.sidecar_bytes;
      candidates.push_back({entry.path(), scan.started, accounted, scan.file_bytes, scan.modified});
      total += accounted;
    }
    scanned = std::move(seen);
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      return a.started != b.started ? a.started < b.started : a.path < b.path;
    });
    const auto today = days_since_epoch(trading_date(time));
    for (const auto& candidate : candidates) {
      const bool expired = options.keep_days && today - days_since_epoch(trading_date(candidate.started)) >= options.keep_days;
      if (!expired && (!options.max_bytes || total <= options.max_bytes)) continue;
      std::error_code ec;
      if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(candidate.path, ec)) ||
          std::filesystem::file_size(candidate.path, ec) != candidate.file_bytes ||
          std::filesystem::last_write_time(candidate.path, ec) != candidate.modified || ec) continue;
      const bool removed = options.remove ? options.remove(candidate.path, ec) : std::filesystem::remove(candidate.path, ec);
      if (removed) {
        total -= candidate.bytes;
        scanned.erase(candidate.path);
        std::filesystem::remove(candidate.path.string() + ".end", ec);
      } else if (ec) {
        notes.push_back("could not remove " + candidate.path.filename().string() + " (" + ec.message() + ")");
      }
    }
  }
  /// Header start time of a complete recording, or zero to leave the file alone.
  static Timestamp verify(const std::filesystem::path& path) {
    try {
      RecordingReader reader(path);
      const auto started = reader.header().started;
      if (started <= 0) return 0;
      // The bound sidecar is written only after clean close. Legacy files are
      // scanned so neither an active --record FILE nor a torn file is pruned.
      if (!reader.indexed_snapshot_mode().has_value()) {
        while (reader.next()) {}
        if (!reader.diagnostic().empty()) return 0;
      }
      return started;
    } catch (const std::exception&) { return 0; /* Unknown/damaged files belong to the user. */ }
  }
  struct Scan {
    std::uint64_t file_bytes = 0, sidecar_bytes = 0;
    std::filesystem::file_time_type modified{};
    bool sidecar = false;
    Timestamp started = 0;
    [[nodiscard]] bool same(const Scan& other) const {
      return file_bytes == other.file_bytes && modified == other.modified && sidecar == other.sidecar &&
             sidecar_bytes == other.sidecar_bytes;
    }
  };
  std::filesystem::path directory, temporary, final;
  std::vector<std::filesystem::path> stale;
  RecordingHeader header;
  EventSink& downstream;
  Options options;
  std::function<Timestamp()> clock;
  std::atomic<Timestamp> stamp{0};
  Discard discard;
  std::unique_ptr<RecordingSink> recording;
  std::map<InstrumentId, OptionContract> contracts;
  std::string name, failure, warning;
  Date date;
  bool closed = false;
  RecordingStats completed;
  mutable std::mutex mutex;
  // Retention: one pass at a time, on the constructor/close caller or the worker.
  std::mutex retention_mutex;
  std::map<std::filesystem::path, Scan> scanned;
  std::mutex queue_mutex;
  std::condition_variable wake;
  std::optional<Timestamp> requested;
  bool stopping = false;
  std::thread pruner;
};
RecordingDirectory::RecordingDirectory(std::filesystem::path directory, RecordingHeader header,
                                       EventSink& downstream, Options options)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(header), downstream, std::move(options))) {}
RecordingDirectory::~RecordingDirectory() { close(); }
void RecordingDirectory::publish(Event event) {
  auto& p = *impl_;
  const std::lock_guard lock(p.mutex);
  if (p.closed) throw std::logic_error("recording: publish after close");
  p.stamp = p.clock();
  p.roll(p.stamp);
  if (p.failure.empty()) {
    try {
      if (const auto* definition = std::get_if<ContractDefinition>(&event)) {
        if (p.contracts.size() >= 500'000 && !p.contracts.contains(definition->id))
          throw std::runtime_error("recording: contract cache limit exceeded");
        p.contracts[definition->id] = definition->contract;
      }
      p.recording->publish(event);
      if (!p.recording->error().empty()) throw std::runtime_error(p.recording->error());
    } catch (const std::exception& error) { p.fail(error.what()); }
  }
  p.downstream.publish(std::move(event));
}
void RecordingDirectory::rollover() {
  const std::lock_guard lock(impl_->mutex);
  impl_->roll(impl_->clock());
}
void RecordingDirectory::close() {
  auto& p = *impl_;
  {
    const std::lock_guard lock(p.mutex);
    if (p.closed) return;
    p.closed = true;
    if (!p.failure.empty()) return;
    try { p.finish(); }
    catch (const std::exception& error) { p.fail(error.what()); return; }
  }
  p.stop();
  p.retain(p.clock());
}
std::string RecordingDirectory::error() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->failure;
}
std::string RecordingDirectory::warning() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->warning;
}
RecordingStats RecordingDirectory::stats() const {
  const std::lock_guard lock(impl_->mutex);
  auto result = impl_->completed;
  if (impl_->recording) add(result, impl_->recording->stats());
  return result;
}
}  // namespace openport::md
