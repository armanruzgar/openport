#include "openport/md/recording_directory.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>
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
    try { open(options.recording.clock()); prune(); }
    catch (...) { cleanup(); throw; }
  }
  ~Impl() { cleanup(); }
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
      downstream.publish(ProviderStatus{options.recording.clock(), FeedState::Error, failure, ""});
    }
    if (recording) add(completed, recording->stats());
    cleanup();
  }
  void roll() {
    if (closed || !failure.empty()) return;
    try {
      if (!recording->error().empty()) throw std::runtime_error(recording->error());
      const auto time = options.recording.clock();
      // A backward wall-clock adjustment cannot reopen an earlier trading day.
      if (trading_date(time) <= date) return;
      finish();
      open(time);
      prune();
    } catch (const std::exception& error) { fail(error.what()); }
  }
  void prune() {
    if (!options.keep_days && !options.max_bytes) return;
    struct Candidate {
      std::filesystem::path path;
      Timestamp started;
      std::uint64_t bytes, file_bytes;
      std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    std::uint64_t total = recording ? recording->stats().bytes : 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      std::error_code status_error;
      if (entry.path().extension() != ".oprec" || entry.path().filename().string().starts_with('.') ||
          !std::filesystem::is_regular_file(entry.symlink_status(status_error)) || status_error) continue;
      try {
        RecordingReader reader(entry.path());
        const auto started = reader.header().started;
        if (started <= 0) continue;
        // The bound sidecar is written only after clean close. Legacy files are
        // scanned so neither an active --record FILE nor a torn file is pruned.
        if (!reader.indexed_snapshot_mode().has_value()) {
          while (reader.next()) {}
          if (!reader.diagnostic().empty()) continue;
        }
        const auto bytes = entry.file_size();
        auto accounted = bytes;
        std::error_code ec;
        const auto sidecar = std::filesystem::file_size(entry.path().string() + ".end", ec);
        if (!ec) accounted += sidecar;
        candidates.push_back({entry.path(), started, accounted, bytes, entry.last_write_time()});
        total += accounted;
      } catch (const std::exception&) { /* Unknown/damaged files belong to the user. */ }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
      return a.started != b.started ? a.started < b.started : a.path < b.path;
    });
    const auto today = days_since_epoch(trading_date(options.recording.clock()));
    for (const auto& candidate : candidates) {
      const bool expired = options.keep_days && today - days_since_epoch(trading_date(candidate.started)) >= options.keep_days;
      if (!expired && (!options.max_bytes || total <= options.max_bytes)) continue;
      std::error_code ec;
      if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(candidate.path, ec)) ||
          std::filesystem::file_size(candidate.path, ec) != candidate.file_bytes ||
          std::filesystem::last_write_time(candidate.path, ec) != candidate.modified || ec) continue;
      if (std::filesystem::remove(candidate.path, ec)) {
        total -= candidate.bytes;
        std::filesystem::remove(candidate.path.string() + ".end", ec);
      } else if (ec) throw std::runtime_error("recording: retention could not remove " + candidate.path.filename().string());
    }
  }
  std::filesystem::path directory, temporary, final;
  RecordingHeader header;
  EventSink& downstream;
  Options options;
  Discard discard;
  std::unique_ptr<RecordingSink> recording;
  std::map<InstrumentId, OptionContract> contracts;
  std::string name, failure;
  Date date;
  bool closed = false;
  RecordingStats completed;
  mutable std::mutex mutex;
};
RecordingDirectory::RecordingDirectory(std::filesystem::path directory, RecordingHeader header,
                                       EventSink& downstream, Options options)
    : impl_(std::make_unique<Impl>(std::move(directory), std::move(header), downstream, std::move(options))) {}
RecordingDirectory::~RecordingDirectory() { close(); }
void RecordingDirectory::publish(Event event) {
  auto& p = *impl_;
  const std::lock_guard lock(p.mutex);
  if (p.closed) throw std::logic_error("recording: publish after close");
  p.roll();
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
  impl_->roll();
}
void RecordingDirectory::close() {
  auto& p = *impl_;
  const std::lock_guard lock(p.mutex);
  if (p.closed) return;
  p.closed = true;
  if (!p.failure.empty()) return;
  try { p.finish(); p.prune(); }
  catch (const std::exception& error) { p.fail(error.what()); }
}
std::string RecordingDirectory::error() const {
  const std::lock_guard lock(impl_->mutex);
  return impl_->failure;
}
RecordingStats RecordingDirectory::stats() const {
  const std::lock_guard lock(impl_->mutex);
  auto result = impl_->completed;
  if (impl_->recording) add(result, impl_->recording->stats());
  return result;
}
}  // namespace openport::md
