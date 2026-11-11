#include "openport/md/recording_directory.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace openport::md {
namespace {
class Discard final : public EventSink {
 public:
  void publish(Event) override {}
};
/// Underlyings whose latest quote and close a new day repeats, bounded like contracts.
constexpr std::size_t kUnderlyingLimit = 100'000;
/// The market time a streaming replay batches an event by; zero for the rest.
Timestamp quoted_at(const Event& event) {
  if (const auto* quote = std::get_if<OptionQuote>(&event)) return quote->ts;
  if (const auto* spot = std::get_if<UnderlyingQuote>(&event)) return spot->ts;
  return 0;
}
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
/// Renames without ever replacing `to`. Where the system cannot promise that (an older
/// kernel, or a filesystem without the flag), it renames only while `to` is absent:
/// a published name carries its writer's unique staging suffix.
std::error_code rename_exclusive(const std::filesystem::path& from, const std::filesystem::path& to) {
#if defined(__APPLE__) && defined(RENAME_EXCL)
  if (::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0) return {};
  const std::error_code refused(errno, std::generic_category());
#elif defined(__linux__) && defined(RENAME_NOREPLACE)
  if (::renameat2(AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), RENAME_NOREPLACE) == 0) return {};
  const std::error_code refused(errno, std::generic_category());
#else
  const auto refused = std::make_error_code(std::errc::function_not_supported);
#endif
  if (refused != std::errc::invalid_argument && refused != std::errc::function_not_supported &&
      refused != std::errc::not_supported && refused != std::errc::operation_not_supported)
    return refused;
  std::error_code ec;
  if (std::filesystem::exists(std::filesystem::symlink_status(to, ec))) return std::make_error_code(std::errc::file_exists);
  std::filesystem::rename(from, to, ec);
  return ec;
}
/// Publishes a staged file as `to` without ever replacing a file there, so a competing
/// writer can never replace a day: a hard link, or an exclusive rename on filesystems
/// without hard links (exFAT, FAT and some network mounts).
std::error_code publish_exclusive(const std::filesystem::path& from, const std::filesystem::path& to,
                                  const std::function<void(const std::filesystem::path&, const std::filesystem::path&,
                                                           std::error_code&)>& hook) {
  std::error_code ec;
  if (hook) hook(from, to, ec);
  else std::filesystem::create_hard_link(from, to, ec);
  if (!ec || ec == std::errc::file_exists) return ec;
  return rename_exclusive(from, to);
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
    carry();
  }
  /// A new day opens with what the feed has already sent, so it replays alone as the
  /// feed stood at the rollover: every definition, then each contract's and underlying's
  /// latest values, which a snapshot feed does not send again until they change. They
  /// go to the file only; downstream has had them.
  void carry() {
    std::vector<Event> values;
    for (const auto& [id, known] : contracts) {
      recording->publish(ContractDefinition{id, known.contract});
      if (known.quote) values.emplace_back(*known.quote);
      if (known.interest) values.emplace_back(*known.interest);
      if (known.volume) values.emplace_back(*known.volume);
      if (known.greeks) values.emplace_back(*known.greeks);
    }
    for (const auto& [symbol, spot] : spots) {
      if (spot.quote) values.emplace_back(*spot.quote);
      if (spot.close) values.emplace_back(*spot.close);
    }
    // A streaming replay batches by the market second of the first quote it meets: the
    // newest leads, so the carried values replay as one batch.
    const auto newest = std::max_element(values.begin(), values.end(), [](const Event& a, const Event& b) {
      return quoted_at(a) < quoted_at(b);
    });
    if (newest != values.end()) std::iter_swap(values.begin(), newest);
    for (auto& value : values) recording->publish(std::move(value));
  }
  /// Keeps what the next day's file opens with, as a chain book holds it: the last
  /// quote, open interest and Greeks, the newest valid volume and the latest close.
  void remember(const Event& event) {
    std::visit([&](const auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, ContractDefinition>) {
        if (contracts.size() >= 500'000 && !contracts.contains(value.id))
          throw std::runtime_error("recording: contract cache limit exceeded");
        contracts[value.id].contract = value.contract;
      } else if constexpr (std::is_same_v<T, OptionQuote> || std::is_same_v<T, OpenInterest> ||
                           std::is_same_v<T, OptionVolume> || std::is_same_v<T, VendorGreeks>) {
        const auto found = contracts.find(value.id);
        if (found == contracts.end()) return;
        auto& known = found->second;
        if constexpr (std::is_same_v<T, OptionQuote>) known.quote = value;
        else if constexpr (std::is_same_v<T, OpenInterest>) known.interest = value;
        else if constexpr (std::is_same_v<T, VendorGreeks>) known.greeks = value;
        else if (std::isfinite(value.contracts) && value.contracts >= 0 && (!known.volume || value.ts >= known.volume->ts))
          known.volume = value;
      } else if constexpr (std::is_same_v<T, UnderlyingQuote> || std::is_same_v<T, UnderlyingClose>) {
        if (spots.size() >= kUnderlyingLimit && !spots.contains(value.symbol))
          throw std::runtime_error("recording: underlying cache limit exceeded");
        auto& spot = spots[value.symbol];
        if constexpr (std::is_same_v<T, UnderlyingQuote>) spot.quote = value;
        else if (!spot.close || value.date >= spot.close->date) spot.close = value;
      }
    }, event);
  }
  void finish() {
    if (!recording) return;
    recording->close();
    if (!recording->error().empty()) throw std::runtime_error(recording->error());
    const auto partial = temporary / "day.oprec";
    if (const auto ec = publish_exclusive(partial, final, options.link)) {
      // The day is complete and cleanly closed: keep it where it is rather than lose it.
      temporary.clear();
      throw std::runtime_error("cannot publish " + final.filename().string() + " (" + ec.message() +
                               "); the complete day is kept at " + partial.string());
    }
    // Only an index: without it, the day is scanned once instead.
    std::error_code ignored;
    if (std::filesystem::exists(partial.string() + ".end", ignored))
      (void)publish_exclusive(partial.string() + ".end", final.string() + ".end", options.link);
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
  struct Known {
    OptionContract contract;
    std::optional<OptionQuote> quote;
    std::optional<OpenInterest> interest;
    std::optional<OptionVolume> volume;
    std::optional<VendorGreeks> greeks;
  };
  struct Spot {
    std::optional<UnderlyingQuote> quote;
    std::optional<UnderlyingClose> close;
  };
  std::map<InstrumentId, Known> contracts;
  std::map<std::string, Spot> spots;
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
      p.remember(event);
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
