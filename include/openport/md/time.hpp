#pragma once

#include <memory>

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openport::md {

/// Nanoseconds since the Unix epoch, UTC.
using Timestamp = std::int64_t;
inline constexpr Timestamp kInvalidTimestamp = std::numeric_limits<Timestamp>::min();

inline constexpr Timestamp kNanosPerSecond = 1'000'000'000;
inline constexpr Timestamp kNanosPerMinute = 60 * kNanosPerSecond;
inline constexpr Timestamp kNanosPerDay = 86'400 * kNanosPerSecond;
inline constexpr double kNanosPerYear = 365.0 * 86'400.0 * 1e9;

/// A calendar date in the proleptic Gregorian calendar.
struct Date {
  int year = 1970;
  int month = 1;
  int day = 1;

  friend constexpr auto operator<=>(const Date&, const Date&) = default;
};

[[nodiscard]] bool valid_date(Date date) noexcept;

[[nodiscard]] std::int64_t days_since_epoch(Date date) noexcept;
[[nodiscard]] Date date_from_days(std::int64_t days) noexcept;

/// Day of the week, 0 = Sunday.
[[nodiscard]] int weekday(Date date) noexcept;

/// UTC offset of New York at a local wall-clock time, in hours: -4 while daylight
/// saving is in force (second Sunday of March, 02:00, to first Sunday of November,
/// 02:00), otherwise -5.
[[nodiscard]] int new_york_utc_offset_hours(Date date, int hour) noexcept;

/// Converts a New York wall-clock time to UTC. Invalid fields, the spring DST
/// gap, or overflow return kInvalidTimestamp. Ambiguous fall times use the first
/// (EDT) occurrence. Uses the post-2007 US DST rules.
[[nodiscard]] Timestamp new_york_to_utc(Date date, int hour, int minute, int second = 0) noexcept;

/// An instant as New York wall-clock time: its local date and the seconds since
/// local midnight.
struct NewYorkTime {
  Date date;
  int seconds = 0;
};
[[nodiscard]] NewYorkTime new_york_time(Timestamp ts) noexcept;

/// Year fraction between two instants on an ACT/365 basis.
[[nodiscard]] constexpr double years_between(Timestamp from, Timestamp to) noexcept {
  // Preserve nanosecond differences when subtraction fits; only intervals
  // spanning opposite int64 extremes need a floating-point subtraction.
  if ((from < 0 && to > std::numeric_limits<Timestamp>::max() + from) ||
      (from > 0 && to < std::numeric_limits<Timestamp>::min() + from))
    return (static_cast<double>(to) - static_cast<double>(from)) / kNanosPerYear;
  return static_cast<double>(to - from) / kNanosPerYear;
}

/// Current wall-clock time.
[[nodiscard]] Timestamp now() noexcept;

enum class Zone : std::uint8_t { Utc, NewYork };

/// Parses "YYYY-MM-DD HH:MM:SS" or "YYYY-MM-DDTHH:MM:SS", with an optional fraction
/// of a second (1-9 digits), as a wall-clock time in `zone`. Z or +/-hh:mm
/// overrides `zone`; all other suffixes, impossible dates/fields and nanosecond
/// overflow are rejected. Unzoned New York times in the spring-forward gap are
/// rejected; ambiguous fall-back times use the first (EDT) occurrence.
[[nodiscard]] std::optional<Timestamp> parse_datetime(std::string_view text, Zone zone) noexcept;

/// Regular US options session calendar: NYSE/Cboe holidays and 13:00 ET early
/// closes by NYSE's rules from 2022 on, with the special closures announced so far
/// (a future one is unknown until added). Before 2022 only weekdays are considered.
/// This models 09:30-16:00 ET regular hours, excluding extended/product-specific
/// sessions and unscheduled halts.
struct MarketSession {
  bool open = false;
  std::string note;
  std::optional<Timestamp> next_open;  ///< null while open, or beyond timestamp range
};
[[nodiscard]] MarketSession market_session(Timestamp ts);

/// A date the exchange has announced a closure or an early close for. The calendar
/// takes it over NYSE's rules for that date, so a special closure (a national day of
/// mourning, say) needs no new build: openportd loads Cboe's published holiday
/// schedule (providers::CboeHolidaySchedule).
struct ScheduledDay {
  Date date;
  std::string name;
  bool closed = true;
  int close_hour = 13;       ///< the regular close, ET, of a day that is open
  int overnight_until = 0;   ///< a closed day: minutes after midnight ET that Cboe's
                             ///< overnight session runs into it until, or zero
  friend bool operator==(const ScheduledDay&, const ScheduledDay&) = default;
};
/// Replaces the announced days, one per date (the first of any repeats). Thread-safe;
/// every calendar function sees them at once.
void set_scheduled_days(std::vector<ScheduledDay> days);
[[nodiscard]] std::vector<ScheduledDay> scheduled_days();
/// Use a recorded calendar on this thread only; other engines keep the live schedule.
class ScheduledDaysScope {
 public:
  explicit ScheduledDaysScope(std::vector<ScheduledDay> days);
  ~ScheduledDaysScope();
  ScheduledDaysScope(const ScheduledDaysScope&) = delete;
  ScheduledDaysScope& operator=(const ScheduledDaysScope&) = delete;
 private:
  struct State;
  std::unique_ptr<State> state_;
};


/// Per-root option sessions, independent of the underlying stock/index clock.
/// GTH belongs to the following trading date. Before a weekend, New Year's Day,
/// Good Friday or Christmas it does not run; into the other holidays it runs from
/// 20:15 the evening before until 11:30 ET, as Cboe schedules it. Early-close days
/// have no curb, as at Cboe. Expiry settlement times remain separate.
struct TradingSession {
  std::string name = "closed";  ///< regular, curb, global, closed
  bool open = false;
  std::string note;
  /// ts while open, otherwise the most recent session end <= ts. Invalid if no
  /// preceding session end is representable in the nanosecond timestamp range.
  Timestamp market_time = kInvalidTimestamp;
  /// While open, when this session ends.
  Timestamp end = kInvalidTimestamp;
};
[[nodiscard]] TradingSession trading_session(std::string_view root, Timestamp ts);
/// The stock market's regular session, 09:30-16:00 ET (13:00 on an early-close
/// day): stocks and ETFs print their close at 16:00 even where their options
/// trade on until 16:15.
[[nodiscard]] TradingSession stock_session(Timestamp ts);
/// The stock market's regular session with the extended hours around it on its
/// business days: premarket from 04:00 ET to the open and after hours from the close
/// for four hours (20:00 ET, 17:00 on an early-close day), as the main US exchanges
/// run them. Named "premarket", "regular" and "after_hours".
[[nodiscard]] TradingSession stock_extended_session(Timestamp ts);

/// The trading date a moment belongs to: a business day's own New York date
/// until 17:00 ET, when its last session (curb) ends; after that, and on
/// weekends and holidays, the next business day, whose overnight session opens
/// that evening. Before 2022 only weekdays are known.
[[nodiscard]] Date trading_date(Timestamp ts) noexcept;
/// The same with the day ending `day_end_minutes` after New York midnight instead
/// of 17:00, as a plan's trading day can (1440 ends it at midnight); 1020 is
/// trading_date(ts).
[[nodiscard]] Date trading_date(Timestamp ts, int day_end_minutes) noexcept;

/// The business day before `date`, skipping weekends and calendar holidays.
[[nodiscard]] Date previous_business_day(Date date) noexcept;

/// Scheduled PM close hour (13 or 16). Before 2022 assumes 16:00 ET; does not
/// roll a contract date off holidays or weekends.
[[nodiscard]] int regular_close_hour(Date date) noexcept;

/// "YYYY-MM-DD".
[[nodiscard]] std::string format_date(Date date);

/// ISO 8601 in UTC with milliseconds, e.g. "2026-09-22T19:48:44.000Z".
[[nodiscard]] std::string format_timestamp(Timestamp ts);

}  // namespace openport::md
