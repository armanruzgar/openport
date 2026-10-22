#include "openport/md/time.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

#include "openport/md/contract.hpp"

namespace openport::md {
namespace {

/// Day of the month of the n-th Sunday (n >= 1) of a month.
[[nodiscard]] int nth_sunday(int year, int month, int n) noexcept {
  const int first = weekday(Date{year, month, 1});
  return 1 + (7 - first) % 7 + 7 * (n - 1);
}

[[nodiscard]] bool parse_int(std::string_view text, std::size_t pos, std::size_t len,
                             int& out) noexcept {
  if (pos + len > text.size()) return false;
  for (std::size_t i = pos; i < pos + len; ++i)
    if (text[i] < '0' || text[i] > '9') return false;
  const char* begin = text.data() + pos;
  const auto [ptr, ec] = std::from_chars(begin, begin + len, out);
  return ec == std::errc() && ptr == begin + len;
}

// Normalize a negative second with a positive fraction before multiplying:
// the floor second at INT64_MIN would overflow even though the full time fits.
std::optional<Timestamp> timestamp(std::int64_t seconds, Timestamp fraction = 0) noexcept {
  if (seconds < 0 && fraction > 0) {
    ++seconds;
    fraction -= kNanosPerSecond;
  }
  constexpr auto low = std::numeric_limits<Timestamp>::min();
  constexpr auto high = std::numeric_limits<Timestamp>::max();
  if (seconds < low / kNanosPerSecond || seconds > high / kNanosPerSecond) return std::nullopt;
  const Timestamp nanos = seconds * kNanosPerSecond;
  if ((fraction > 0 && nanos > high - fraction) || (fraction < 0 && nanos < low - fraction))
    return std::nullopt;
  return nanos + fraction;
}

bool valid_time(Date date, int hour, int minute, int second) noexcept {
  return valid_date(date) && hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 &&
         second < 60;
}

bool spring_gap(Date date, int hour) noexcept {
  return date.month == 3 && date.day == nth_sunday(date.year, 3, 2) && hour == 2;
}

// Holidays follow NYSE's rules (Rule 7.2), which Cboe's options markets observe: a
// holiday on a Saturday closes the Friday before, except New Year's Day, whose
// Friday would end the year; one on a Sunday closes the Monday after. They
// reproduce the published 2025-2028 calendars, which the tests check:
// https://ir.theice.com/press/news-details/2024/NYSE-Group-Announces-2025-2026-and-2027-Holiday-and-Early-Closings-Calendar/default.aspx
// https://ir.theice.com/press/news-details/2025/NYSE-Group-Announces-2026-2027-and-2028-Holiday-and-Early-Closings-Calendar/
// Cboe's hours, and its overnight sessions into seven of the holidays:
// https://www.cboe.com/about/hours/us-options
// Special closures are announced one at a time:
// https://cdn.cboe.com/resources/schedule_update/2025/Update-Cboe-to-Observe-National-Day-of-Mourning-on-Thursday-January-9-2025.pdf
constexpr int kFirstCalendarYear = 2022;  // Juneteenth's first

struct Holiday {
  std::string_view name;
  /// Minutes after midnight ET that Cboe's overnight session, open from 20:15 the
  /// evening before, runs into it until; zero when none does.
  int overnight_until = 0;
};
constexpr int kHolidaySessionEnd = 11 * 60 + 30;

/// The announced days, sorted by date. Every version published stays alive, so a
/// reader holds no lock and a version's address names it; there is one per change
/// to Cboe's schedule.
struct Schedule {
  std::vector<ScheduledDay> days;
};
std::atomic<const Schedule*> g_schedule{nullptr};
thread_local const Schedule* thread_schedule = nullptr;
std::mutex g_schedule_mutex;
std::vector<std::unique_ptr<const Schedule>> g_schedules;

const ScheduledDay* scheduled(const Schedule* schedule, Date date) noexcept {
  if (!schedule) return nullptr;
  const auto it = std::lower_bound(schedule->days.begin(), schedule->days.end(), date,
                                   [](const ScheduledDay& day, Date d) { return day.date < d; });
  return it != schedule->days.end() && it->date == date ? &*it : nullptr;
}

Date add_days(Date date, std::int64_t days) noexcept { return date_from_days(days_since_epoch(date) + days); }
/// The n-th (n >= 1) weekday `wd` (0 for Sunday) of a month.
Date nth_weekday(int year, int month, int wd, int n) noexcept {
  const Date first{year, month, 1};
  return add_days(first, (wd - weekday(first) + 7) % 7 + 7 * (n - 1));
}
/// The last weekday `wd` of a month.
Date last_weekday(int year, int month, int wd) noexcept {
  const Date last = add_days(month == 12 ? Date{year + 1, 1, 1} : Date{year, month + 1, 1}, -1);
  return add_days(last, -((weekday(last) - wd + 7) % 7));
}
/// Easter Sunday, by the anonymous Gregorian algorithm.
Date easter(int year) noexcept {
  const int a = year % 19, b = year / 100, c = year % 100, d = b / 4, e = b % 4, f = (b + 8) / 25;
  const int g = (b - f + 1) / 3, h = (19 * a + b - d - g + 15) % 30, i = c / 4, k = c % 4;
  const int l = (32 + 2 * e + 2 * i - h - k) % 7, m = (a + 11 * h + 22 * l) / 451;
  return {year, (h + l - 7 * m + 114) / 31, (h + l - 7 * m + 114) % 31 + 1};
}
/// The weekday a fixed-date holiday closes the market.
Date observed(Date date) noexcept {
  const int wd = weekday(date);
  return wd == 6 ? add_days(date, -1) : wd == 0 ? add_days(date, 1) : date;
}

std::optional<Holiday> holiday_on(const Schedule* schedule, Date date) noexcept {
  // What the exchange has announced for a date overrides the rules.
  if (const auto* day = scheduled(schedule, date))
    return day->closed ? std::optional(Holiday{day->name, day->overnight_until}) : std::nullopt;
  if (date.year < kFirstCalendarYear) return std::nullopt;
  if (date == Date{2025, 1, 9}) return Holiday{"National Day of Mourning"};
  const int y = date.year;
  const int session = kHolidaySessionEnd;
  if (weekday({y, 1, 1}) != 6 && date == observed({y, 1, 1})) return Holiday{"New Year's Day"};
  if (date == nth_weekday(y, 1, 1, 3)) return Holiday{"Martin Luther King Jr. Day", session};
  if (date == nth_weekday(y, 2, 1, 3)) return Holiday{"Washington's Birthday", session};
  if (date == add_days(easter(y), -2)) return Holiday{"Good Friday"};
  if (date == last_weekday(y, 5, 1)) return Holiday{"Memorial Day", session};
  if (date == observed({y, 6, 19})) return Holiday{"Juneteenth", session};
  if (date == observed({y, 7, 4})) return Holiday{"Independence Day", session};
  if (date == nth_weekday(y, 9, 1, 1)) return Holiday{"Labor Day", session};
  if (date == nth_weekday(y, 11, 4, 4)) return Holiday{"Thanksgiving Day", session};
  if (date == observed({y, 12, 25})) return Holiday{"Christmas Day"};
  return std::nullopt;
}

/// 13:00 closes: the day before Independence Day and Christmas Eve when they fall
/// Monday to Thursday, and the day after Thanksgiving.
bool early_close(const Schedule* schedule, Date date) noexcept {
  if (const auto* day = scheduled(schedule, date)) return !day->closed && day->close_hour < 16;
  if (date.year < kFirstCalendarYear || holiday_on(schedule, date)) return false;
  if ((date.month == 7 && date.day == 3) || (date.month == 12 && date.day == 24))
    return weekday(date) >= 1 && weekday(date) <= 4;
  return date == add_days(nth_weekday(date.year, 11, 4, 4), 1);
}

/// What the rules and one schedule say about a date.
struct Day {
  std::optional<Holiday> holiday;
  bool business = false;  ///< a weekday that no holiday closes
  int close_hour = 0;     ///< regular_close_hour
};
Day day_under(const Schedule* schedule, Date date) noexcept {
  Day day;
  day.holiday = holiday_on(schedule, date);
  const int wd = weekday(date);
  day.business = wd != 0 && wd != 6 && !day.holiday;
  if (const auto* announced = scheduled(schedule, date)) day.close_hour = announced->closed ? 16 : announced->close_hour;
  else day.close_hour = early_close(schedule, date) ? 13 : 16;
  return day;
}

/// A session around a trade date: its end is representable, its start may not be.
struct Span {
  enum class Kind : std::uint8_t { Global, Regular, Curb } kind{};
  Timestamp start = 0;
  Timestamp end = 0;
};
/// Everything session_at weighs for the instants of one New York date and one
/// kind of root: why the date is closed, and the sessions of the trade dates from
/// two weeks before to the next, in the order it weighs them. A trade date adds
/// at most four: a holiday overnight, the overnight before it, regular and curb.
struct Window {
  bool filled = false;
  std::int64_t days = 0;
  bool weekend = false;
  std::optional<Holiday> holiday;  ///< the date's, if closed
  std::size_t count = 0;
  std::array<Span, 4 * 16> spans{};
};

/// Calendar answers this thread has worked out under one schedule. Each thread
/// keeps its own, so reading takes no lock; a schedule published since
/// (set_scheduled_days) empties it before it answers again. It holds no strings
/// (names point into the schedule or the rules' literals, which stay alive), so
/// it needs no destructor and can answer at any time.
class Memo {
 public:
  /// This thread's memo, for the schedule in force now.
  static Memo& current() noexcept {
    thread_local Memo memo;
    const auto* schedule = thread_schedule ? thread_schedule : g_schedule.load(std::memory_order_acquire);
    if (!memo.ready_ || memo.schedule_ != schedule) {
      memo.ready_ = true;
      memo.schedule_ = schedule;
      for (auto& slot : memo.days_) slot.filled = false;
      for (auto& window : memo.windows_) window.filled = false;
    }
    return memo;
  }
  void clear() noexcept { ready_ = false; }
  Day day(Date date) noexcept {
    // Keyed by the fields, so a date out of range is still its own entry.
    const auto hash = static_cast<std::uint32_t>(date.year) * 372u + static_cast<std::uint32_t>(date.month) * 31u +
                      static_cast<std::uint32_t>(date.day);
    auto& slot = days_[hash % days_.size()];
    if (!slot.filled || slot.date != date) {
      slot.day = day_under(schedule_, date);
      slot.date = date;
      slot.filled = true;
    }
    return slot.day;
  }
  const Window& window(bool global, bool curb, bool quarter_hour, std::int64_t days) noexcept {
    // A slot per kind of root for each of two consecutive dates.
    const auto kind = (global ? 1u : 0u) | (curb ? 2u : 0u) | (quarter_hour ? 4u : 0u);
    auto& w = windows_[(static_cast<std::uint64_t>(days) % 2) * 8 + kind];
    if (w.filled && w.days == days) return w;
    const auto date = date_from_days(days);
    w.weekend = weekday(date) == 0 || weekday(date) == 6;
    w.holiday = day(date).holiday;
    w.count = 0;
    const auto add = [&](Span::Kind span, Timestamp start, Timestamp end) {
      // A session that cannot end in the timestamp range never counts.
      if (end != kInvalidTimestamp) w.spans[w.count++] = Span{span, start, end};
    };
    // Enumerate trade dates, including tomorrow for tonight's GTH. Two weeks
    // covers every closure in the supported calendar without subtracting nanos.
    for (int offset = -14; offset <= 1; ++offset) {
      const auto trade_date = date_from_days(days + offset);
      const auto trade_day = day(trade_date);
      // Into most holidays an overnight session runs until 11:30, for the next trade date.
      if (const auto& h = trade_day.holiday; global && h && h->overnight_until > 0)
        add(Span::Kind::Global, new_york_to_utc(date_from_days(days + offset - 1), 20, 15),
            new_york_to_utc(trade_date, h->overnight_until / 60, h->overnight_until % 60));
      if (!trade_day.business) continue;
      if (global) {
        const auto evening = date_from_days(days + offset - 1);
        add(Span::Kind::Global, new_york_to_utc(evening, 20, 15), new_york_to_utc(trade_date, 9, 25));
      }
      add(Span::Kind::Regular, new_york_to_utc(trade_date, 9, 30),
          new_york_to_utc(trade_date, trade_day.close_hour, quarter_hour ? 15 : 0));
      if (curb && trade_day.close_hour == 16)
        add(Span::Kind::Curb, new_york_to_utc(trade_date, 16, 15), new_york_to_utc(trade_date, 17, 0));
    }
    w.days = days;
    w.filled = true;
    return w;
  }

 private:
  // Only filled entries are read; zeros elsewhere keep a new thread's copy free.
  struct DaySlot {
    bool filled = false;
    Date date{0, 0, 0};
    Day day;
  };
  bool ready_ = false;
  const Schedule* schedule_ = nullptr;
  std::array<DaySlot, 256> days_{};
  std::array<Window, 16> windows_{};
};
static_assert(std::is_trivially_destructible_v<Memo>, "a thread's memo must stay usable until the thread ends");

}  // namespace

bool valid_date(Date date) noexcept {
  if (date.year < 1 || date.year > 9999 || date.month < 1 || date.month > 12 || date.day < 1 ||
      date.day > 31)
    return false;
  return std::chrono::year_month_day{std::chrono::year{date.year},
                                     std::chrono::month{static_cast<unsigned>(date.month)},
                                     std::chrono::day{static_cast<unsigned>(date.day)}}
      .ok();
}

std::int64_t days_since_epoch(Date date) noexcept {
  const std::chrono::year_month_day ymd{std::chrono::year{date.year},
                                        std::chrono::month{static_cast<unsigned>(date.month)},
                                        std::chrono::day{static_cast<unsigned>(date.day)}};
  return std::chrono::sys_days{ymd}.time_since_epoch().count();
}

Date date_from_days(std::int64_t days) noexcept {
  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{days}}};
  return {static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
          static_cast<int>(static_cast<unsigned>(ymd.day()))};
}

int weekday(Date date) noexcept {
  const std::chrono::weekday wd{std::chrono::sys_days{std::chrono::days{days_since_epoch(date)}}};
  return static_cast<int>(wd.c_encoding());
}

int new_york_utc_offset_hours(Date date, int hour) noexcept {
  const Date dst_start{date.year, 3, nth_sunday(date.year, 3, 2)};
  const Date dst_end{date.year, 11, nth_sunday(date.year, 11, 1)};
  const bool after_start = date > dst_start || (date == dst_start && hour >= 2);
  const bool before_end = date < dst_end || (date == dst_end && hour < 2);
  return after_start && before_end ? -4 : -5;
}

Timestamp new_york_to_utc(Date date, int hour, int minute, int second) noexcept {
  if (!valid_time(date, hour, minute, second) || spring_gap(date, hour)) return kInvalidTimestamp;
  const std::int64_t local_seconds =
      days_since_epoch(date) * 86'400 + hour * 3'600 + minute * 60 + second;
  const std::int64_t utc_seconds = local_seconds - new_york_utc_offset_hours(date, hour) * 3'600LL;
  return timestamp(utc_seconds).value_or(kInvalidTimestamp);
}

Timestamp now() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::optional<Timestamp> parse_datetime(std::string_view text, Zone zone) noexcept {
  Date date;
  int hour = 0, minute = 0, second = 0;
  if (text.size() < 19 || text[4] != '-' || text[7] != '-' ||
      (text[10] != ' ' && text[10] != 'T') || text[13] != ':' || text[16] != ':')
    return std::nullopt;
  if (!parse_int(text, 0, 4, date.year) || !parse_int(text, 5, 2, date.month) ||
      !parse_int(text, 8, 2, date.day) || !parse_int(text, 11, 2, hour) ||
      !parse_int(text, 14, 2, minute) || !parse_int(text, 17, 2, second) ||
      !valid_time(date, hour, minute, second))
    return std::nullopt;

  std::size_t pos = 19;
  Timestamp fraction = 0;
  if (pos < text.size() && text[pos] == '.') {
    const auto first = ++pos;
    Timestamp scale = kNanosPerSecond;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      if (pos - first == 9) return std::nullopt;
      scale /= 10;
      fraction += (text[pos++] - '0') * scale;
    }
    if (pos == first) return std::nullopt;
  }
  int offset_minutes = 0;
  if (pos == text.size()) {
    if (zone == Zone::NewYork) {
      if (spring_gap(date, hour)) return std::nullopt;
      offset_minutes = 60 * new_york_utc_offset_hours(date, hour);
    }
  } else if (text.substr(pos) == "Z") {
    offset_minutes = 0;
  } else {
    int zh = 0, zm = 0;
    if (text.size() - pos != 6 || (text[pos] != '+' && text[pos] != '-') || text[pos + 3] != ':' ||
        !parse_int(text, pos + 1, 2, zh) || !parse_int(text, pos + 4, 2, zm) || zh > 23 || zm > 59)
      return std::nullopt;
    offset_minutes = (text[pos] == '+' ? 1 : -1) * (zh * 60 + zm);
  }
  const auto seconds =
      days_since_epoch(date) * 86400 + hour * 3600 + minute * 60 + second - offset_minutes * 60;
  return timestamp(seconds, fraction);
}

int regular_close_hour(Date date) noexcept { return Memo::current().day(date).close_hour; }

struct ScheduledDaysScope::State {
  Schedule schedule;
  const Schedule* previous = thread_schedule;
};
ScheduledDaysScope::ScheduledDaysScope(std::vector<ScheduledDay> days) : state_(std::make_unique<State>()) {
  std::stable_sort(days.begin(), days.end(), [](const auto& a, const auto& b) { return a.date < b.date; });
  days.erase(std::unique(days.begin(), days.end(), [](const auto& a, const auto& b) { return a.date == b.date; }), days.end());
  state_->schedule.days = std::move(days);
  Memo::current().clear();
  thread_schedule = &state_->schedule;
}
ScheduledDaysScope::~ScheduledDaysScope() {
  Memo::current().clear();
  thread_schedule = state_->previous;
}

void set_scheduled_days(std::vector<ScheduledDay> days) {
  std::stable_sort(days.begin(), days.end(), [](const ScheduledDay& a, const ScheduledDay& b) { return a.date < b.date; });
  days.erase(std::unique(days.begin(), days.end(), [](const auto& a, const auto& b) { return a.date == b.date; }), days.end());
  const std::lock_guard lock(g_schedule_mutex);
  const auto* current = g_schedule.load(std::memory_order_relaxed);
  if (current ? current->days == days : days.empty()) return;
  auto next = std::make_unique<const Schedule>(Schedule{std::move(days)});
  g_schedule.store(next.get(), std::memory_order_release);
  g_schedules.push_back(std::move(next));
}

std::vector<ScheduledDay> scheduled_days() {
  const auto* schedule = thread_schedule ? thread_schedule : g_schedule.load(std::memory_order_acquire);
  return schedule ? schedule->days : std::vector<ScheduledDay>{};
}

namespace {
struct LocalTime {
  Date date;
  std::int64_t days;
  std::int64_t seconds;
};
LocalTime local_time(Timestamp ts) {
  // Determine DST from UTC transition instants, so the repeated fall hour cannot
  // choose the wrong local date. Work in seconds to avoid nanosecond overflow.
  auto seconds = ts / kNanosPerSecond;
  if (ts % kNanosPerSecond < 0) --seconds;
  auto utc_days = seconds / 86400;
  if (seconds % 86400 < 0) --utc_days;
  const auto utc_date = date_from_days(utc_days);
  const auto start =
      days_since_epoch({utc_date.year, 3, nth_sunday(utc_date.year, 3, 2)}) * 86400 + 7 * 3600;
  const auto end =
      days_since_epoch({utc_date.year, 11, nth_sunday(utc_date.year, 11, 1)}) * 86400 + 6 * 3600;
  const auto local = seconds + (seconds >= start && seconds < end ? -4 : -5) * 3600;
  auto days = local / 86400;
  auto rem = local % 86400;
  if (rem < 0) {
    rem += 86400;
    --days;
  }
  return {date_from_days(days), days, rem};
}
}  // namespace

NewYorkTime new_york_time(Timestamp ts) noexcept {
  const auto local = local_time(ts);
  return {local.date, static_cast<int>(local.seconds)};
}

MarketSession market_session(Timestamp ts) {
  auto& memo = Memo::current();
  const auto [date, days, rem] = local_time(ts);
  const auto today = memo.day(date);
  MarketSession result;
  const int wd = weekday(date);
  if (wd == 0 || wd == 6)
    result.note = "closed (weekend)";
  else if (const auto& h = today.holiday; h)
    result.note = h->name.empty() ? "closed (holiday)" : "closed (holiday: " + std::string(h->name) + ")";
  else if (rem < 9 * 3600 + 30 * 60)
    result.note = "closed (pre-market)";
  else if (rem >= today.close_hour * 3600)
    result.note = "closed (after hours)";
  else {
    result.open = true;
    result.note = today.close_hour == 13 ? "open, early close 13:00 ET" : "open";
    return result;
  }
  for (int ahead = 0; ahead < 14; ++ahead) {
    const auto candidate = date_from_days(days + ahead);
    if (!memo.day(candidate).business) continue;
    const auto next = new_york_to_utc(candidate, 9, 30);
    if (next != kInvalidTimestamp && next > ts) {
      result.next_open = next;
      break;
    }
  }
  return result;
}

namespace {
TradingSession session_at(bool global, bool curb, bool quarter_hour, Timestamp ts) {
  const auto& window = Memo::current().window(global, curb, quarter_hour, local_time(ts).days);
  // The last session holding ts is in force; otherwise the market time is the
  // latest end at or before ts.
  const Span* in_force = nullptr;
  Timestamp latest_end = kInvalidTimestamp;
  for (std::size_t i = 0; i < window.count; ++i) {
    const auto& span = window.spans[i];
    if (span.start != kInvalidTimestamp && span.start <= ts && ts < span.end)
      in_force = &span;
    else if (span.end <= ts && span.end > latest_end)
      latest_end = span.end;
  }
  TradingSession result;
  if (!in_force) {
    result.market_time = latest_end;
    result.note = window.holiday ? (window.holiday->name.empty() ? "closed (holiday)"
                                     : "closed (holiday: " + std::string(window.holiday->name) + ")")
                  : window.weekend ? "closed (weekend)"
                                   : "closed (between sessions)";
    return result;
  }
  result.open = true;
  result.market_time = ts;
  result.end = in_force->end;
  switch (in_force->kind) {
    case Span::Kind::Global:
      result.name = "global";
      result.note = "overnight session";
      break;
    case Span::Kind::Curb:
      result.name = "curb";
      result.note = "curb session";
      break;
    case Span::Kind::Regular:
      result.name = "regular";
      result.note = "regular session";
      break;
  }
  return result;
}
}  // namespace

TradingSession trading_session(std::string_view root, Timestamp ts) {
  const auto underlying = conventions_for_root(root).underlying;
  const bool global =
      underlying == "SPX" || underlying == "XSP" || underlying == "VIX" || underlying == "RUT";
  return session_at(global, global, is_late_close_underlying(underlying), ts);
}

TradingSession stock_session(Timestamp ts) { return session_at(false, false, false, ts); }

TradingSession stock_extended_session(Timestamp ts) {
  auto regular = stock_session(ts);
  if (regular.open) return regular;
  const auto local = local_time(ts);
  if (!Memo::current().day(local.date).business) return regular;
  const int close = regular_close_hour(local.date);
  const auto premarket = new_york_to_utc(local.date, 4, 0), open = new_york_to_utc(local.date, 9, 30);
  const auto close_at = new_york_to_utc(local.date, close, 0), evening = new_york_to_utc(local.date, close + 4, 0);
  TradingSession result;
  if (premarket <= ts && ts < open) {
    result = {"premarket", true, "premarket session", ts, open};
  } else if (close_at <= ts && ts < evening) {
    result = {"after_hours", true, "after-hours session", ts, evening};
  } else {
    result = regular;
    result.market_time = ts >= evening ? evening : regular.market_time;
  }
  return result;
}

Date trading_date(Timestamp ts) noexcept { return trading_date(ts, 17 * 60); }
Date trading_date(Timestamp ts, int day_end_minutes) noexcept {
  auto& memo = Memo::current();
  const auto local = local_time(ts);
  if (memo.day(local.date).business && local.seconds < day_end_minutes * 60) return local.date;
  // Two weeks covers every closure in the supported calendar.
  for (int ahead = 1; ahead <= 14; ++ahead)
    if (const auto date = date_from_days(local.days + ahead); memo.day(date).business) return date;
  return date_from_days(local.days + 1);
}

Date previous_business_day(Date date) noexcept {
  auto& memo = Memo::current();
  const auto days = days_since_epoch(date);
  for (int back = 1; back <= 14; ++back)
    if (const auto earlier = date_from_days(days - back); memo.day(earlier).business) return earlier;
  return date_from_days(days - 1);
}

std::string format_date(Date date) {
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d", date.year, date.month, date.day);
  return buffer;
}

std::string format_timestamp(Timestamp ts) {
  std::int64_t days = ts / kNanosPerDay;
  std::int64_t rem = ts % kNanosPerDay;
  if (rem < 0) {
    rem += kNanosPerDay;
    --days;
  }
  const Date date = date_from_days(days);
  const std::int64_t millis = rem / 1'000'000;
  char buffer[32];
  std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02dT%02lld:%02lld:%02lld.%03lldZ", date.year,
                date.month, date.day, static_cast<long long>(millis / 3'600'000),
                static_cast<long long>(millis / 60'000 % 60),
                static_cast<long long>(millis / 1'000 % 60),
                static_cast<long long>(millis % 1'000));
  return buffer;
}

}  // namespace openport::md
