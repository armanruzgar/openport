#include "openport/server/candles.hpp"
#include "openport/md/files.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace openport::server {
namespace {

constexpr md::Timestamp kMinute = md::kNanosPerMinute;

md::Timestamp floor_to(md::Timestamp time, md::Timestamp span, md::Timestamp offset = 0) {
  const md::Timestamp shifted = time - offset;
  md::Timestamp buckets = shifted / span;
  if (shifted % span < 0) --buckets;
  return buckets * span + offset;
}

md::Timestamp span_of(BarInterval interval) {
  switch (interval) {
    case BarInterval::Minute:
      return kMinute;
    case BarInterval::FiveMinutes:
      return 5 * kMinute;
    case BarInterval::FifteenMinutes:
      return 15 * kMinute;
    case BarInterval::ThirtyMinutes:
      return 30 * kMinute;
    case BarInterval::Hour:
      return 60 * kMinute;
    case BarInterval::Day:
      return md::kNanosPerDay;
  }
  return kMinute;
}

/// File names come from symbols, so only plain tickers are stored.
bool storable(const std::string& symbol) {
  return !symbol.empty() && symbol.size() <= 32 && symbol.front() != '.' &&
         std::all_of(symbol.begin(), symbol.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_';
         });
}

void extend(md::Bar& into, const md::Bar& bar) {
  into.high = std::max(into.high, bar.high);
  into.low = std::min(into.low, bar.low);
  into.close = bar.close;
}

std::string line_of(const md::Bar& bar, bool official) {
  char buffer[160];
  std::snprintf(buffer, sizeof buffer, "%lld,%.10g,%.10g,%.10g,%.10g,%c\n",
                static_cast<long long>(bar.start / md::kNanosPerSecond), bar.open, bar.high,
                bar.low, bar.close, official ? 'o' : 's');
  return buffer;
}

}  // namespace

std::optional<BarInterval> parse_bar_interval(std::string_view text) noexcept {
  if (text == "1m") return BarInterval::Minute;
  if (text == "5m") return BarInterval::FiveMinutes;
  if (text == "15m") return BarInterval::FifteenMinutes;
  if (text == "30m") return BarInterval::ThirtyMinutes;
  if (text == "1h") return BarInterval::Hour;
  if (text == "1d") return BarInterval::Day;
  return std::nullopt;
}

std::string_view to_string(BarInterval interval) noexcept {
  switch (interval) {
    case BarInterval::Minute:
      return "1m";
    case BarInterval::FiveMinutes:
      return "5m";
    case BarInterval::FifteenMinutes:
      return "15m";
    case BarInterval::ThirtyMinutes:
      return "30m";
    case BarInterval::Hour:
      return "1h";
    case BarInterval::Day:
      return "1d";
  }
  return "1m";
}

CandleStore::CandleStore(Options options) : options_(std::move(options)) {
  if (options_.directory.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(options_.directory, ec);
  if (ec) {
    error_ = "candles: cannot create " + options_.directory.string() + ": " + ec.message();
    return;
  }
  for (const auto& entry : std::filesystem::directory_iterator(options_.directory, ec)) {
    if (entry.path().extension() == ".csv" && storable(entry.path().stem().string()))
      load(entry.path());
  }
  if (ec) error_ = "candles: cannot read " + options_.directory.string() + ": " + ec.message();
}

CandleStore::~CandleStore() { flush(); }

std::filesystem::path CandleStore::file_for(const std::string& symbol) const {
  return options_.directory / (symbol + ".csv");
}

void CandleStore::load(const std::filesystem::path& file) {
  const std::string symbol = file.stem().string();
  Series& series = series_[symbol];
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    ++series.lines;
    long long seconds = 0;
    md::Bar bar;
    char source = 0;
    if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf,%c", &seconds, &bar.open, &bar.high,
                    &bar.low, &bar.close, &source) != 6 ||
        (source != 'o' && source != 's'))
      continue;  // a line cut short by a crash
    bar.start = static_cast<md::Timestamp>(seconds) * md::kNanosPerSecond;
    if (!md::valid_bar(bar) || bar.start != floor_to(bar.start, kMinute)) continue;
    const bool official = source == 'o';
    series.minutes[bar.start] =
        Minute{bar, bar.start, official ? bar.start + kMinute - 1 : bar.start, official, true};
  }
  prune(series);
  if (series.lines != series.minutes.size()) compact(symbol, series);
}

void CandleStore::sample(const std::string& symbol, md::Timestamp time, double price) {
  if (!std::isfinite(price) || price <= 0.0) return;
  const std::lock_guard lock(mutex_);
  Series& series = series_[symbol];
  const md::Timestamp start = floor_to(time, kMinute);
  const bool newest = series.minutes.empty() || start > series.minutes.rbegin()->first;
  auto [it, inserted] = series.minutes.try_emplace(start);
  Minute& minute = it->second;
  if (inserted) {
    minute = Minute{{start, price, price, price, price}, time, time, false, false};
  } else {
    if (minute.official) return;
    const md::Bar before = minute.bar;
    minute.bar.high = std::max(minute.bar.high, price);
    minute.bar.low = std::min(minute.bar.low, price);
    if (time >= minute.last) {
      minute.bar.close = price;
      minute.last = time;
    }
    if (time < minute.first) {
      minute.bar.open = price;
      minute.first = time;
    }
    if (minute.bar != before) minute.saved = false;
  }
  if (newest) {
    // A new minute finishes the ones before it.
    persist(symbol, series, false);
    prune(series);
  }
}

void CandleStore::merge_minutes(const std::string& symbol, const std::vector<md::Bar>& bars) {
  const std::lock_guard lock(mutex_);
  Series& series = series_[symbol];
  for (const md::Bar& bar : bars) {
    if (!md::valid_bar(bar) || bar.start != floor_to(bar.start, kMinute)) continue;
    Minute& minute = series.minutes[bar.start];
    if (minute.official && minute.bar == bar) continue;
    minute = Minute{bar, bar.start, bar.start + kMinute - 1, true, false};
  }
  persist(symbol, series, false);
  prune(series);
}

void CandleStore::merge_days(const std::string& symbol, const std::vector<md::Bar>& bars) {
  const std::lock_guard lock(mutex_);
  Series& series = series_[symbol];
  for (const md::Bar& bar : bars)
    if (std::isfinite(bar.close) && bar.close > 0) series.days[bar.start] = bar;
  while (series.days.size() > options_.max_days) series.days.erase(series.days.begin());
}

void CandleStore::prune(Series& series) const {
  if (series.minutes.empty()) return;
  const md::Timestamp cutoff =
      series.minutes.rbegin()->first -
      std::chrono::duration_cast<std::chrono::nanoseconds>(options_.keep).count();
  series.minutes.erase(series.minutes.begin(), series.minutes.lower_bound(cutoff));
}

void CandleStore::persist(const std::string& symbol, Series& series, bool forming) {
  if (series.minutes.empty()) return;
  const auto latest = std::prev(series.minutes.end());
  auto due = [&](auto it) {
    return !it->second.saved && (forming || it->second.official || it != latest);
  };
  if (options_.directory.empty() || !storable(symbol)) {
    for (auto it = series.minutes.begin(); it != series.minutes.end(); ++it)
      if (due(it)) it->second.saved = true;
    return;
  }
  std::string text;
  std::size_t count = 0;
  for (auto it = series.minutes.begin(); it != series.minutes.end(); ++it) {
    if (!due(it)) continue;
    text += line_of(it->second.bar, it->second.official);
    ++count;
  }
  if (count == 0) return;
  std::ofstream out(file_for(symbol), std::ios::app | std::ios::binary);
  out << text;
  out.flush();
  if (!out) {
    error_ = "candles: cannot write " + file_for(symbol).string();
    return;
  }
  for (auto it = series.minutes.begin(); it != series.minutes.end(); ++it)
    if (due(it)) it->second.saved = true;
  series.lines += count;
  if (series.lines > 2 * series.minutes.size() + 1024) compact(symbol, series);
}

void CandleStore::compact(const std::string& symbol, Series& series) {
  if (options_.directory.empty() || !storable(symbol)) return;
  std::string text;
  for (const auto& [start, minute] : series.minutes) text += line_of(minute.bar, minute.official);
  try {
    // Chart history, rebuilt from samples and refetched bars: replaced whole, not synced.
    md::write_file_atomic(file_for(symbol), text, md::Durability::Cache);
  } catch (const std::exception& error) {
    error_ = std::string("candles: ") + error.what();
    return;
  }
  for (auto& [start, minute] : series.minutes) minute.saved = true;
  series.lines = series.minutes.size();
}

std::vector<md::Bar> CandleStore::sessions(const Series& series, std::size_t limit) const {
  // Regular-hours minutes build each session the vendor has no daily bar for yet.
  std::map<md::Timestamp, md::Bar> built;
  for (const auto& [start, minute] : series.minutes) {
    const auto local = md::new_york_time(start);
    if (local.seconds < 9 * 3600 + 30 * 60 ||
        local.seconds >= md::regular_close_hour(local.date) * 3600)
      continue;
    const md::Timestamp open = md::new_york_to_utc(local.date, 9, 30);
    if (const auto day = series.days.find(open); day != series.days.end() && md::valid_bar(day->second)) continue;
    auto [it, inserted] = built.try_emplace(open, minute.bar);
    if (inserted)
      it->second.start = open;
    else
      extend(it->second, minute.bar);
  }
  // Both are ordered by start and never share one.
  std::vector<md::Bar> out;
  out.reserve(series.days.size() + built.size());
  auto vendor = series.days.begin();
  auto own = built.begin();
  while (vendor != series.days.end() || own != built.end()) {
    while (vendor != series.days.end() && !md::valid_bar(vendor->second)) ++vendor;
    if (vendor == series.days.end() && own == built.end()) break;
    if (own == built.end() || (vendor != series.days.end() && vendor->first < own->first))
      out.push_back((vendor++)->second);
    else
      out.push_back((own++)->second);
  }
  if (out.size() > limit) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
  return out;
}

std::vector<md::Bar> CandleStore::bars(const std::string& symbol, BarInterval interval,
                                       std::size_t limit) const {
  const std::lock_guard lock(mutex_);
  const auto found = series_.find(symbol);
  if (found == series_.end() || limit == 0) return {};
  const Series& series = found->second;
  if (interval == BarInterval::Day) {
    auto daily = sessions(series, options_.max_days);
    std::erase_if(daily, [](const md::Bar& bar) { return !md::valid_bar(bar); });
    if (daily.size() > limit) daily.erase(daily.begin(), daily.end() - static_cast<std::ptrdiff_t>(limit));
    return daily;
  }
  const md::Timestamp span = span_of(interval);
  const md::Timestamp offset = interval == BarInterval::Hour ? 30 * kMinute : 0;
  std::vector<md::Bar> out;
  for (const auto& [start, minute] : series.minutes) {
    const md::Timestamp bucket = floor_to(start, span, offset);
    if (out.empty() || out.back().start != bucket) {
      out.push_back(minute.bar);
      out.back().start = bucket;
    } else {
      extend(out.back(), minute.bar);
    }
  }
  if (out.size() > limit) out.erase(out.begin(), out.end() - static_cast<std::ptrdiff_t>(limit));
  return out;
}

std::vector<md::Bar> CandleStore::daily_history(const std::string& symbol) const {
  const std::lock_guard lock(mutex_);
  const auto found = series_.find(symbol);
  if (found == series_.end()) return {};
  // Charts can build a missing valid candle from minutes. Estimators retain the
  // official close-only row, so an absent vendor open is never filled in silently.
  std::map<md::Timestamp, md::Bar> daily;
  for (const auto& bar : sessions(found->second, options_.max_days)) daily[bar.start] = bar;
  for (const auto& [start, bar] : found->second.days) daily[start] = bar;
  while (daily.size() > options_.max_days) daily.erase(daily.begin());
  std::vector<md::Bar> out;
  out.reserve(daily.size());
  for (const auto& [start, bar] : daily) out.push_back(bar);
  return out;
}

void CandleStore::flush() {
  const std::lock_guard lock(mutex_);
  for (auto& [symbol, series] : series_) persist(symbol, series, true);
}

std::string CandleStore::error() const {
  const std::lock_guard lock(mutex_);
  return error_;
}

}  // namespace openport::server
