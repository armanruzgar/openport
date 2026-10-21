#include "openport/server/series.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "metric_cache.hpp"
#include "openport/server/engine.hpp"

namespace openport::server {
namespace {
bool storable(std::string_view symbol) {
  return !symbol.empty() && symbol.size() <= 32 && symbol.front() != '.' &&
      std::all_of(symbol.begin(), symbol.end(), [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
      });
}
std::string header() {
  std::string out = "#openport-series,1\nminute";
  for (const auto field : kSeriesFields) { out += ','; out += field; }
  return out + '\n';
}
std::string line_of(const SeriesRow& row) {
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << row.time / md::kNanosPerSecond << std::setprecision(17);
  for (double value : row.values) {
    out << ',';
    if (std::isfinite(value)) out << value;
  }
  return out.str() + '\n';
}
std::string month_of(md::Timestamp time) { return md::format_date(md::new_york_time(time).date).substr(0, 7); }
}

SeriesStore::SeriesStore(std::filesystem::path directory)
    : directory_(std::move(directory)), calendar_(md::scheduled_days()) {
  if (directory_.empty()) return;
  try {
    std::filesystem::create_directories(directory_);
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      if (entry.path().extension() == ".csv") load(entry.path());
      else if (entry.path().extension() == ".surfaces") load_surfaces(entry.path());
    }
  } catch (const std::exception& e) { error_ = "series: " + std::string(e.what()); }
}

void SeriesStore::load(const std::filesystem::path& path) {
  const auto stem = path.stem().string();
  const auto symbol = stem.size() >= 9 ? stem.substr(0, stem.size() - 8) : std::string{};
  if (storable(symbol)) files_[symbol][stem.substr(stem.size() - 7)] = path;
  read_file(path, [&](const SeriesRow& row) { remember(symbol, row); });
}

void SeriesStore::read_file(const std::filesystem::path& path,
                            const std::function<void(const SeriesRow&)>& visit) const {
  try {
    const auto stem = path.stem().string();
    if (stem.size() < 9) throw std::runtime_error("invalid series filename");
    const auto symbol = stem.substr(0, stem.size() - 8);
    const auto month = stem.substr(stem.size() - 7);
    if (!storable(symbol)) throw std::runtime_error("invalid series symbol");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read series");
    std::string version, columns;
    if (!std::getline(input, version) || !std::getline(input, columns) || version + '\n' + columns + '\n' != header())
      throw std::runtime_error("unsupported series header/version");
    for (std::string line; std::getline(input, line);) {
      if (input.eof()) throw std::runtime_error("torn final series row");
      SeriesRow row;
      std::size_t offset = 0;
      for (std::size_t i = 0; i <= row.values.size(); ++i) {
        const auto comma = line.find(',', offset);
        const auto cell = std::string_view(line).substr(offset, comma == std::string::npos ? comma : comma - offset);
        if ((i < row.values.size()) != (comma != std::string::npos)) throw std::runtime_error("invalid series row");
        if (i == 0) {
          std::int64_t seconds = 0;
          const auto parsed = std::from_chars(cell.data(), cell.data() + cell.size(), seconds);
          if (parsed.ec != std::errc{} || parsed.ptr != cell.data() + cell.size() || seconds <= 0 ||
              seconds > std::numeric_limits<md::Timestamp>::max() / md::kNanosPerSecond || seconds % 60 != 0)
            throw std::runtime_error("invalid series minute");
          row.time = seconds * md::kNanosPerSecond;
        } else if (!cell.empty()) {
          std::istringstream value{std::string(cell)};
          value.imbue(std::locale::classic());
          value >> std::noskipws >> row.values[i - 1];
          if (!value || value.peek() != std::char_traits<char>::eof() || !std::isfinite(row.values[i - 1]))
            throw std::runtime_error("invalid series value");
        }
        if (comma != std::string::npos) offset = comma + 1;
      }
      if (month_of(row.time) != month) throw std::runtime_error("series month mismatch");
      visit(row);
    }
    if (input.bad()) throw std::runtime_error("cannot read series");
  } catch (const std::exception& e) {
    damaged_.insert(path);
    error_ = path.string() + ": " + e.what();
  }
}

std::map<md::Timestamp, SeriesRow> SeriesStore::read_minutes(const std::string& symbol,
                                                           md::Timestamp from, md::Timestamp to) const {
  std::map<md::Timestamp, SeriesRow> out;
  if (from > to) return out;
  const auto found = files_.find(symbol);
  if (found == files_.end()) return out;
  const auto last_month = month_of(to);
  for (auto it = found->second.lower_bound(month_of(from)); it != found->second.end() && it->first <= last_month; ++it)
    read_file(it->second, [&](const SeriesRow& row) {
      if (row.time >= from && row.time <= to) out[row.time] = row;
    });
  return out;
}

void SeriesStore::remember(const std::string& symbol, const SeriesRow& row) {
  auto& minutes = rows_[symbol];
  minutes[row.time] = row;
  index_daily(symbol, row);
  const auto cutoff = minutes.rbegin()->first - kSeriesMinuteRetention;
  minutes.erase(minutes.begin(), minutes.upper_bound(cutoff));
  last_write_ = std::max(last_write_, row.time);
}

bool SeriesStore::insert(const std::string& symbol, const SeriesRow& row, bool force) {
  const std::lock_guard lock(mutex_);
  if (!storable(symbol) || row.time <= 0 || row.time % md::kNanosPerMinute != 0) {
    error_ = "series: invalid symbol or minute"; return false;
  }
  auto& series = rows_[symbol];
  const auto found = series.find(row.time);
  std::map<md::Timestamp, SeriesRow> archived;
  if (found == series.end() && (series.empty() || row.time <= series.rbegin()->first - kSeriesMinuteRetention)) {
    archived = read_minutes(symbol, row.time, row.time);
  }
  const SeriesRow* previous = !archived.empty() ? &archived.begin()->second
      : found != series.end() ? &found->second : nullptr;
  if (previous && (!force || line_of(*previous) == line_of(row))) return false;
  try {
    refresh_calendar();
    if (!directory_.empty()) {
      const auto month = month_of(row.time);
      const auto path = directory_ / (symbol + '-' + month + ".csv");
      if (damaged_.contains(path)) throw std::runtime_error("refusing to append to damaged file " + path.string());
      const bool empty = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
      std::ofstream output(path, std::ios::app);
      if (empty) output << header();
      output << line_of(row);
      output.flush();
      if (!output) {
        damaged_.insert(path);
        throw std::runtime_error("cannot write " + path.string());
      }
      files_[symbol][month] = path;
    }
    remember(symbol, row);
    return true;
  } catch (const std::exception& e) { error_ = "series: " + std::string(e.what()); return false; }
}

bool SeriesStore::contains(const std::string& symbol, md::Timestamp minute) const {
  const std::lock_guard lock(mutex_);
  const auto found = rows_.find(symbol);
  if (found != rows_.end() && !found->second.empty()) {
    if (found->second.contains(minute)) return true;
    if (minute > found->second.rbegin()->first - kSeriesMinuteRetention) return false;
  }
  return !read_minutes(symbol, minute, minute).empty();
}

void SeriesStore::index_daily(const std::string& symbol, const SeriesRow& row) const {
  const auto date = md::new_york_time(row.time).date;
  const auto open = md::new_york_to_utc(date, 9, 30);
  const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
  if (row.time < open || row.time > close || !md::market_session(open).open) return;
  auto& selected = daily_[symbol][date];
  if (row.time >= selected.time) selected = row;
}

void SeriesStore::refresh_calendar() const {
  const auto schedule = md::scheduled_days();
  if (schedule == calendar_) return;
  calendar_ = schedule;
  if (!directory_.empty()) {
    daily_.clear();
    for (const auto& [symbol, months] : files_)
      for (const auto& [month, path] : months) {
        (void)month;
        read_file(path, [&](const SeriesRow& row) { index_daily(symbol, row); });
      }
  } else {
    // Old memory-only minutes cannot reconstruct a newly announced earlier close.
    for (auto& [symbol, days] : daily_) {
      (void)symbol;
      std::erase_if(days, [](const auto& entry) {
        const auto close = md::new_york_to_utc(entry.first, md::regular_close_hour(entry.first), 0);
        return entry.second.time > close || !md::market_session(md::new_york_to_utc(entry.first, 9, 30)).open;
      });
    }
  }
  for (const auto& [symbol, minutes] : rows_)
    for (const auto& [time, row] : minutes) { (void)time; index_daily(symbol, row); }
}

std::vector<SeriesRow> SeriesStore::rows(const std::string& symbol, md::Timestamp from,
                                      md::Timestamp to, bool daily) const {
  const std::lock_guard lock(mutex_);
  std::vector<SeriesRow> out;
  if (from > to) return out;
  if (daily) {
    refresh_calendar();
    const auto found = daily_.find(symbol);
    if (found == daily_.end()) return out;
    for (auto it = found->second.lower_bound(md::new_york_time(from).date); it != found->second.end(); ++it) {
      const auto close = md::new_york_to_utc(it->first, md::regular_close_hour(it->first), 0);
      if (close > to) break;
      if (close >= from) out.push_back(it->second);
    }
  } else {
    const auto found = rows_.find(symbol);
    const auto cutoff = found == rows_.end() || found->second.empty() ? to
        : found->second.rbegin()->first - kSeriesMinuteRetention;
    auto selected = read_minutes(symbol, from, std::min(to, cutoff));
    if (found != rows_.end())
      for (auto it = found->second.lower_bound(from); it != found->second.end() && it->first <= to; ++it)
        selected[it->first] = it->second;
    for (const auto& [time, row] : selected) { (void)time; out.push_back(row); }
  }
  return out;
}

std::size_t SeriesStore::cached_minutes(const std::string& symbol) const {
  const std::lock_guard lock(mutex_);
  const auto found = rows_.find(symbol);
  return found == rows_.end() ? 0 : found->second.size();
}

SeriesStatus SeriesStore::status(md::Timestamp now) const {
  const std::lock_guard lock(mutex_);
  SeriesStatus out{directory_.string(), 0, last_write_, error_};
  const auto date = md::new_york_time(now).date;
  const auto start = md::new_york_to_utc(date, 0, 0);
  for (const auto& [symbol, series] : rows_) {
    (void)symbol;
    for (auto it = series.lower_bound(start); it != series.end() && md::new_york_time(it->first).date == date; ++it)
      ++out.rows_today;
  }
  return out;
}
void SeriesStore::report_error(std::string error) {
  const std::lock_guard lock(mutex_);
  error_ = std::move(error);
}

SeriesRow series_row(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  const auto v = cached_volatility(metrics);
  SeriesRow row;
  row.time = metrics->as_of / md::kNanosPerMinute * md::kNanosPerMinute;
  row.values[0] = metrics->spot;
  md::Timestamp first = std::numeric_limits<md::Timestamp>::max();
  for (const auto& slice : metrics->slices)
    if (slice.expiry_time > metrics->as_of && slice.expiry_time < first) {
      first = slice.expiry_time; row.values[1] = slice.forward.forward;
    }
  for (std::size_t i = 0; i < 5; ++i) { row.values[2 + i] = v.mfiv[i].vol; row.values[7 + i] = v.atm[i].vol; }
  row.values[12] = v.skew25.rr; row.values[13] = v.skew25.bf;
  row.values[14] = v.skew10.rr; row.values[15] = v.skew10.bf;
  row.values[16] = v.ratio9_30; row.values[17] = v.ratio30_93;
  // ExposureSummary defaults to zero even when no OI was received.
  if (metrics->exposure.oi_coverage > 0) {
    row.values[18] = metrics->exposure.gex; row.values[19] = metrics->exposure.gamma_flip;
    row.values[20] = metrics->exposure.call_wall; row.values[21] = metrics->exposure.put_wall;
  }
  return row;
}

SeriesWorker::~SeriesWorker() { stop(); }
void SeriesWorker::start() {
  thread_ = std::thread([this] {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      lock.unlock();
      try { sample(); }
      catch (const std::exception& e) { store_.report_error("series worker: " + std::string(e.what())); }
      lock.lock();
      wake_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_; });
    }
  });
}
void SeriesWorker::stop() {
  { const std::lock_guard lock(mutex_); stopping_ = true; }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}
void SeriesWorker::sample() {
  const auto status = source_.status();
  if (status.provider.starts_with("replay") || status.provider == "demo") return;
  for (const auto& symbol : source_.symbols()) {
    const auto health = status.underlyings.find(symbol);
    if (health == status.underlyings.end() ||
        (health->second.state != md::FeedState::Live && health->second.state != md::FeedState::Delayed)) continue;
    const auto metrics = source_.metrics(symbol);
    if (!metrics || metrics->as_of <= 0) continue;
    const auto minute = metrics->as_of / md::kNanosPerMinute * md::kNanosPerMinute;
    if (minute <= sampled_[symbol]) continue;
    sampled_[symbol] = minute;
    if (!store_.contains(symbol, minute)) store_.insert(symbol, series_row(metrics));
    if (!surface_slot(metrics->as_of).empty()) store_.insert_surface(symbol, surface_snapshot(metrics));
  }
}

bool simulated_series_recording(std::string_view provider) noexcept {
  while (provider.starts_with("replay (") && provider.ends_with(')')) {
    provider.remove_prefix(8);
    provider.remove_suffix(1);
  }
  return provider == "demo";
}

std::size_t backfill_series(const std::filesystem::path& recording, SeriesStore& store,
                           bool force, const analytics::AnalyticsOptions& options) {
  md::RecordingReader reader(recording);
  if (simulated_series_recording(reader.header().provider)) throw std::runtime_error("series: demo recordings are simulated and cannot be backfilled");
  analytics::ChainBook book;
  std::set<std::pair<std::string, md::Timestamp>> seen;
  std::map<std::string, std::shared_ptr<const analytics::DiscountCurve>> curves;
  auto curve = options.discount_curve;
  std::size_t written = 0;
  while (const auto event = reader.next()) {
    book.apply(event->event);
    const auto* complete = std::get_if<md::SnapshotComplete>(&event->event);
    if (!complete || complete->ts <= 0) continue;
    const auto found = book.underlyings().find(complete->underlying);
    if (found == book.underlyings().end()) continue;
    const auto minute = complete->ts / md::kNanosPerMinute * md::kNanosPerMinute;
    auto settings = options;
    settings.discount_curve = curve;
    const auto metrics = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(found->second, book, complete->ts, settings));
    const bool european = std::any_of(metrics->slices.begin(), metrics->slices.end(), [](const auto& slice) {
      return slice.style == pricing::ExerciseStyle::European;
    });
    if (european) {
      if (auto updated = analytics::make_discount_curve(*metrics)) {
        curves[complete->underlying] = updated; curve = std::move(updated);
      } else {
        curves.erase(complete->underlying);
        if (curve && curve->symbol() == complete->underlying) curve.reset();
      }
      if (curves.contains("SPX")) curve = curves.at("SPX");
      else if (!curve && !curves.empty()) curve = curves.begin()->second;
    }
    if (!seen.emplace(complete->underlying, minute).second) continue;
    if (!surface_slot(metrics->as_of).empty()) store.insert_surface(complete->underlying, surface_snapshot(metrics), force);
    if (!force && store.contains(complete->underlying, minute)) continue;
    if (store.insert(complete->underlying, series_row(metrics), force)) ++written;
  }
  if (!reader.diagnostic().empty()) throw std::runtime_error(reader.diagnostic());
  return written;
}
}  // namespace openport::server
