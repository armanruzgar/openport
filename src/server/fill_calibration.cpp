#include "fill_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>

#include "openport/md/recording.hpp"

namespace openport::server {
namespace {
struct Recorded {
  md::Timestamp time = 0;
  trading::CalibrationQuote quote;
};
std::optional<trading::Money> price_of(double value) {
  if (!std::isfinite(value) || value <= 0) return std::nullopt;
  try { return trading::Money::from_double(value); } catch (const std::exception&) { return std::nullopt; }
}
trading::Quantity size_of(double value) {
  if (!std::isfinite(value) || value <= 0 || value >= 9e18) return 0;
  return static_cast<trading::Quantity>(std::floor(value));
}
}  // namespace

void check_calibration_span(const std::vector<trading::BrokerFill>& fills) {
  md::Timestamp first = 0, last = 0;
  for (const auto& fill : fills) {
    if (fill.time <= 0) continue;
    first = first == 0 ? fill.time : std::min(first, fill.time);
    last = std::max(last, fill.time);
  }
  if (last - first > kCalibrationSpanDays * md::kNanosPerDay)
    throw std::invalid_argument("Timed fills span more than " + std::to_string(kCalibrationSpanDays) +
                                " days; import at most a month at a time");
}

RecordedQuotes recorded_quotes(const std::filesystem::path& directory, const std::vector<trading::BrokerFill>& fills,
                               md::Timestamp max_age, const RecordingLimits& limits, const std::atomic_bool* cancel) {
  RecordedQuotes out;
  auto& found = out.quotes;
  auto& search = out.search;
  const auto deadline = std::chrono::steady_clock::now() + limits.time;
  const auto stop = [&](const char* reason) {
    if (cancel && cancel->load()) search.stopped = "cancelled";
    else search.stopped = reason;
  };
  const auto halted = [&] { return (cancel && cancel->load()) || std::chrono::steady_clock::now() >= deadline; };
  std::map<std::string, std::vector<const trading::BrokerFill*>> wanted;
  std::set<md::Date> dates;
  std::size_t timed = 0;
  md::Timestamp first = 0, last = 0;
  for (const auto& fill : fills) {
    if (fill.time <= 0) continue;
    ++timed;
    wanted[fill.symbol].push_back(&fill);
    dates.insert(md::trading_date(fill.time));
    first = first == 0 ? fill.time : std::min(first, fill.time);
    last = std::max(last, fill.time);
  }
  std::error_code ec;
  if (wanted.empty() || directory.empty() || !std::filesystem::is_directory(directory, ec)) return out;
  // Headers first, so only the recordings of the fills' days are read through, newest first.
  std::vector<std::pair<md::Timestamp, std::filesystem::path>> candidates;
  std::size_t headers = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    const auto name = entry.path().filename().string();
    if (!std::filesystem::is_regular_file(entry.symlink_status(ec)) || name.starts_with('.') || entry.path().extension() != ".oprec")
      continue;
    if (headers++ >= limits.headers || halted()) {
      stop(headers > limits.headers ? "file_limit" : "time_limit");
      break;
    }
    try {
      const auto started = md::RecordingReader(entry.path()).header().started;
      if (started <= 0 || started > last || (!dates.contains(md::trading_date(started)) && started < first - md::kNanosPerDay))
        continue;
      candidates.emplace_back(started, entry.path());
    } catch (const std::exception&) {
    }
  }
  if (search.stopped != "complete") return out;
  std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    return a.first != b.first ? a.first > b.first : a.second > b.second;
  });
  std::map<std::size_t, md::Timestamp> best;  // the found quote's time, by row
  for (const auto& [started, file] : candidates) {
    if (best.size() == timed) { stop("all_matched"); break; }
    if (search.files >= limits.files) { stop("file_limit"); break; }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) continue;
    if (search.bytes + size > limits.bytes) { stop("byte_limit"); break; }
    if (halted()) { stop("time_limit"); break; }
    ++search.files;
    search.bytes += size;
    try {
      md::RecordingReader reader(file);
      std::map<md::InstrumentId, std::string> ids;
      std::map<std::string, std::vector<Recorded>> quotes;
      std::size_t records = 0;
      bool interrupted = false;
      while (const auto record = reader.next()) {
        if (++records % 4096 == 0 && halted()) { interrupted = true; break; }
        if (const auto* definition = std::get_if<md::ContractDefinition>(&record->event)) {
          auto symbol = definition->contract.osi_symbol();
          if (wanted.contains(symbol)) ids[definition->id] = std::move(symbol);
          else ids.erase(definition->id);
        } else if (const auto* quote = std::get_if<md::OptionQuote>(&record->event)) {
          const auto id = ids.find(quote->id);
          if (id == ids.end() || quote->ts <= 0 || quote->ts > last) continue;
          const auto bid = price_of(quote->bid), ask = price_of(quote->ask);
          quotes[id->second].push_back({quote->ts, {bid.value_or(trading::Money{}), ask.value_or(trading::Money{}),
                                                    size_of(quote->bid_size), size_of(quote->ask_size), quote->ts, "recording"}});
        }
      }
      // A recording read only in part is not used: its last quotes may be missing.
      if (interrupted) { stop("time_limit"); break; }
      for (auto& [symbol, list] : quotes) {
        std::stable_sort(list.begin(), list.end(), [](const Recorded& a, const Recorded& b) { return a.time < b.time; });
        for (const auto* fill : wanted.at(symbol)) {
          const auto after = std::upper_bound(list.begin(), list.end(), fill->time,
                                              [](md::Timestamp time, const Recorded& r) { return time < r.time; });
          if (after == list.begin()) continue;
          const auto& at = *(after - 1);
          if (fill->time - at.time > max_age) continue;
          if (const auto previous = best.find(fill->row); previous != best.end() && previous->second >= at.time) continue;
          best[fill->row] = at.time;
          found[fill->row] = at.quote;
        }
      }
    } catch (const std::exception&) {
      continue;
    }
  }
  return out;
}

CalibrationWorker::~CalibrationWorker() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
}

bool CalibrationWorker::start(std::function<void(const std::atomic_bool& cancel)> job) {
  const std::lock_guard lock(mutex_);
  if (running_) return false;
  if (thread_.joinable()) thread_.join();
  running_ = true;
  try {
    thread_ = std::thread([this, job = std::move(job)] {
      try { job(cancel_); } catch (...) {}
      const std::lock_guard done(mutex_);
      running_ = false;
    });
  } catch (...) {
    running_ = false;
    throw;
  }
  return true;
}
}  // namespace openport::server
