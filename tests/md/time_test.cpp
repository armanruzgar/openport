#include "openport/md/time.hpp"

#include <gtest/gtest.h>
#include <future>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

namespace {

using openport::md::Date;
using openport::md::format_timestamp;
using openport::md::new_york_to_utc;
using openport::md::new_york_utc_offset_hours;
using openport::md::parse_datetime;
using openport::md::Zone;

TEST(Time, CivilDateRoundTrips) {
  EXPECT_EQ(openport::md::days_since_epoch(Date{1970, 1, 1}), 0);
  for (std::int64_t days = -1000; days < 40000; days += 37) {
    EXPECT_EQ(openport::md::days_since_epoch(openport::md::date_from_days(days)), days);
  }
  EXPECT_EQ(openport::md::weekday(Date{2026, 9, 22}), 2);  // a Tuesday
  EXPECT_EQ(openport::md::weekday(Date{2026, 3, 8}), 0);   // a Sunday
}

TEST(Time, NewYorkDaylightSavingBoundaries) {
  // 2026: daylight saving starts Sunday 8 March at 02:00 and ends Sunday 1 November at 02:00.
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 3, 7}, 12), -5);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 3, 8}, 1), -5);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 3, 8}, 3), -4);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 7, 4}, 12), -4);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 11, 1}, 1), -4);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 11, 1}, 2), -5);
  EXPECT_EQ(new_york_utc_offset_hours(Date{2026, 12, 18}, 16), -5);
}

TEST(Time, MarketCloseInUtc) {
  EXPECT_EQ(format_timestamp(new_york_to_utc(Date{2026, 9, 22}, 16, 0)),
            "2026-09-22T20:00:00.000Z");
  EXPECT_EQ(format_timestamp(new_york_to_utc(Date{2026, 12, 18}, 9, 30)),
            "2026-12-18T14:30:00.000Z");
}

TEST(Time, ParsesUtcAndNewYorkWallClock) {
  EXPECT_EQ(format_timestamp(*parse_datetime("2026-09-22 19:48:44", Zone::Utc)),
            "2026-09-22T19:48:44.000Z");
  EXPECT_EQ(format_timestamp(*parse_datetime("2026-09-22T15:33:42", Zone::NewYork)),
            "2026-09-22T19:33:42.000Z");
  EXPECT_EQ(format_timestamp(*parse_datetime("2026-09-22T15:33:42.125", Zone::NewYork)),
            "2026-09-22T19:33:42.125Z");
  EXPECT_EQ(*parse_datetime("2026-09-22 19:48:44.000000001", Zone::Utc) % 1'000'000'000, 1);
  EXPECT_FALSE(parse_datetime("2026-09-22", Zone::Utc));
  EXPECT_FALSE(parse_datetime("2026/09/22 19:48:44", Zone::Utc));
  EXPECT_FALSE(parse_datetime("2026-13-22 19:48:44", Zone::Utc));
}

TEST(Time, YearFractionIsActual365) {
  const auto from = new_york_to_utc(Date{2026, 1, 1}, 0, 0);
  const auto to = new_york_to_utc(Date{2027, 1, 1}, 0, 0);
  EXPECT_DOUBLE_EQ(openport::md::years_between(from, to), 1.0);
}

TEST(Time, RejectsImpossibleDatesFieldsAndTimestampOverflow) {
  for (const auto text :
       {"2026-02-31T12:00:00", "2025-02-29T12:00:00", "2026-04-31T12:00:00", "2026-00-01T12:00:00",
        "2026-01-00T12:00:00", "2026-01-01T-1:00:00", "2026-01-01T12:-1:00", "2026-01-01T12:00:-1",
        "2026-01-01T24:00:00", "2026-01-01T12:60:00", "2026-01-01T12:00:60", "9999-01-01T00:00:00",
        "1600-01-01T00:00:00", "-001-01-01T00:00:00"}) {
    EXPECT_FALSE(parse_datetime(text, Zone::Utc)) << text;
  }
  EXPECT_TRUE(parse_datetime("2028-02-29T12:00:00", Zone::Utc));
}

TEST(Time, AppliesExplicitZonesAndRejectsUnrecognisedSuffixes) {
  for (auto zone : {Zone::Utc, Zone::NewYork}) {
    EXPECT_EQ(parse_datetime("2026-09-22T15:30:00-04:00", zone),
              parse_datetime("2026-09-22T19:30:00Z", Zone::Utc));
    EXPECT_EQ(parse_datetime("2026-09-22T15:30:00+05:30", zone),
              parse_datetime("2026-09-22T10:00:00Z", Zone::Utc));
    EXPECT_EQ(parse_datetime("2026-09-22T15:30:00Z", zone),
              parse_datetime("2026-09-22T15:30:00", Zone::Utc));
    for (const auto suffix :
         {"junk", "EST", "+24:00", "-04:60", "+0400", ".", ".a", ".1234567890", "Zjunk"})
      EXPECT_FALSE(parse_datetime(std::string("2026-09-22T15:30:00") + suffix, zone)) << suffix;
  }
}

TEST(Time, RejectsSpringGapAndUsesFirstFallOccurrence) {
  EXPECT_FALSE(parse_datetime("2026-03-08T02:30:00", Zone::NewYork));
  EXPECT_EQ(parse_datetime("2026-11-01T01:30:00", Zone::NewYork),
            parse_datetime("2026-11-01T05:30:00Z", Zone::Utc));
}

TEST(Time, ChecksNanosecondRangeIncludingFractionsAndOffsets) {
  using openport::md::Timestamp;
  EXPECT_EQ(parse_datetime("2262-04-11T23:47:16.854775807Z", Zone::Utc),
            std::numeric_limits<Timestamp>::max());
  EXPECT_FALSE(parse_datetime("2262-04-11T23:47:16.854775808Z", Zone::Utc));
  EXPECT_EQ(parse_datetime("1677-09-21T00:12:43.145224192Z", Zone::Utc),
            std::numeric_limits<Timestamp>::min());
  EXPECT_FALSE(parse_datetime("1677-09-21T00:12:43.145224191Z", Zone::Utc));
  EXPECT_FALSE(parse_datetime("2262-04-11T23:00:00-01:00", Zone::Utc));
  EXPECT_EQ(new_york_to_utc({2026, 1, 257}, 12, 0), openport::md::kInvalidTimestamp);
  EXPECT_EQ(new_york_to_utc({2026, 3, 8}, 2, 30), openport::md::kInvalidTimestamp);
  EXPECT_EQ(new_york_to_utc({9999, 1, 1}, 12, 0), openport::md::kInvalidTimestamp);
  EXPECT_EQ(new_york_to_utc({2026, 1, 1}, -1, 0), openport::md::kInvalidTimestamp);
}

TEST(Time, MarketSessionNotesBoundariesAndNextOpen) {
  using openport::md::market_session;
  auto session = [](Date date, int h, int m) {
    return market_session(new_york_to_utc(date, h, m));
  };
  EXPECT_EQ(session({2026, 9, 22}, 9, 29).note, "closed (pre-market)");
  EXPECT_EQ(session({2026, 9, 22}, 9, 29).next_open, new_york_to_utc({2026, 9, 22}, 9, 30));
  EXPECT_TRUE(session({2026, 9, 22}, 9, 30).open);
  EXPECT_FALSE(session({2026, 9, 22}, 9, 30).next_open);
  EXPECT_EQ(session({2026, 9, 22}, 12, 0).note, "open");
  EXPECT_TRUE(session({2026, 9, 22}, 15, 59).open);
  EXPECT_EQ(session({2026, 9, 22}, 16, 0).note, "closed (after hours)");
  EXPECT_EQ(session({2026, 9, 22}, 16, 0).next_open, new_york_to_utc({2026, 9, 23}, 9, 30));
  EXPECT_EQ(session({2026, 9, 26}, 12, 0).note, "closed (weekend)");
  EXPECT_EQ(session({2026, 9, 26}, 12, 0).next_open, new_york_to_utc({2026, 9, 28}, 9, 30));
  EXPECT_EQ(session({2026, 11, 26}, 12, 0).note, "closed (holiday: Thanksgiving Day)");
  EXPECT_EQ(session({2026, 11, 26}, 12, 0).next_open, new_york_to_utc({2026, 11, 27}, 9, 30));
  EXPECT_EQ(session({2026, 11, 27}, 12, 59).note, "open, early close 13:00 ET");
  EXPECT_EQ(session({2026, 11, 27}, 13, 0).note, "closed (after hours)");
  EXPECT_EQ(session({2026, 11, 27}, 13, 0).next_open, new_york_to_utc({2026, 11, 30}, 9, 30));
  // A UTC Saturday can still be Friday in New York, including across DST changes.
  EXPECT_EQ(market_session(*parse_datetime("2026-03-07T01:00:00Z", Zone::Utc)).note,
            "closed (after hours)");
  EXPECT_EQ(market_session(*parse_datetime("2026-03-07T01:00:00Z", Zone::Utc)).next_open,
            parse_datetime("2026-03-09T13:30:00Z", Zone::Utc));
  EXPECT_EQ(market_session(*parse_datetime("2026-10-31T01:00:00Z", Zone::Utc)).next_open,
            parse_datetime("2026-11-02T14:30:00Z", Zone::Utc));
}

TEST(Time, CalendarCoversPublishedHolidaysAndOnlyPublishedEarlyCloses) {
  using namespace openport::md;
  const int holidays[][11] = {{101, 109, 120, 217, 418, 526, 619, 704, 901, 1127, 1225},
                              {101, 119, 216, 403, 525, 619, 703, 907, 1126, 1225, 0},
                              {101, 118, 215, 326, 531, 618, 705, 906, 1125, 1224, 0},
                              {117, 221, 414, 529, 619, 704, 904, 1123, 1225, 0, 0}};
  for (int y = 2025; y <= 2028; ++y)
    for (int md : holidays[y - 2025]) {
      if (md == 0) continue;
      const auto s = market_session(new_york_to_utc({y, md / 100, md % 100}, 12, 0));
      EXPECT_FALSE(s.open) << y << ' ' << md;
      EXPECT_EQ(s.note.find("closed (holiday:"), 0u) << s.note;
    }
  for (Date d : {Date{2025, 7, 3},
                 {2025, 11, 28},
                 {2025, 12, 24},
                 {2026, 11, 27},
                 {2026, 12, 24},
                 {2027, 11, 26},
                 {2028, 7, 3},
                 {2028, 11, 24}}) {
    EXPECT_EQ(market_session(new_york_to_utc(d, 12, 0)).note, "open, early close 13:00 ET");
    EXPECT_FALSE(market_session(new_york_to_utc(d, 13, 0)).open);
  }
  // July 2 is not an early close in 2026; New Year 2028 has no observed Friday closure.
  EXPECT_TRUE(market_session(new_york_to_utc({2026, 7, 2}, 15, 0)).open);
  EXPECT_TRUE(market_session(new_york_to_utc({2027, 12, 31}, 12, 0)).open);
  // The rules make exactly the published calendars: no other day of 2025-2028 closes.
  std::set<int> closed, early;
  for (int y = 2025; y <= 2028; ++y)
    for (int md : holidays[y - 2025])
      if (md != 0) closed.insert(y * 10000 + md);
  for (int key : {20250703, 20251128, 20251224, 20261127, 20261224, 20271126, 20280703, 20281124}) early.insert(key);
  for (auto days = days_since_epoch({2025, 1, 1}); days <= days_since_epoch({2028, 12, 31}); ++days) {
    const auto d = date_from_days(days);
    if (weekday(d) == 0 || weekday(d) == 6) continue;
    const int key = d.year * 10000 + d.month * 100 + d.day;
    const auto s = market_session(new_york_to_utc(d, 12, 0));
    EXPECT_EQ(s.open, !closed.contains(key)) << key;
    EXPECT_EQ(s.note == "open, early close 13:00 ET", early.contains(key)) << key;
  }
}

TEST(Time, CalendarRulesCarryPastThePublishedYears) {
  using namespace openport::md;
  const auto note = [](Date d) { return market_session(new_york_to_utc(d, 12, 0)).note; };
  // 2029: New Year's Day on a Monday, Easter on April 1, Christmas Eve on a Monday.
  EXPECT_EQ(note({2029, 1, 1}), "closed (holiday: New Year's Day)");
  EXPECT_EQ(note({2029, 3, 30}), "closed (holiday: Good Friday)");
  EXPECT_EQ(note({2029, 11, 22}), "closed (holiday: Thanksgiving Day)");
  for (Date d : {Date{2029, 7, 3}, Date{2029, 11, 23}, Date{2029, 12, 24}}) EXPECT_EQ(note(d), "open, early close 13:00 ET");
  EXPECT_EQ(regular_close_hour({2029, 12, 24}), 13);
  // 2032: Juneteenth and Christmas on Saturdays close the Fridays before.
  EXPECT_EQ(note({2032, 6, 18}), "closed (holiday: Juneteenth)");
  EXPECT_EQ(note({2032, 12, 24}), "closed (holiday: Christmas Day)");
  EXPECT_EQ(note({2033, 1, 3}), "open");  // New Year's Day 2033 is a Saturday
  // 2022-2024 follow the same rules, including Sunday holidays moving to Monday.
  EXPECT_EQ(note({2022, 6, 20}), "closed (holiday: Juneteenth)");
  EXPECT_EQ(note({2022, 12, 26}), "closed (holiday: Christmas Day)");
  EXPECT_EQ(note({2023, 1, 2}), "closed (holiday: New Year's Day)");
  EXPECT_EQ(note({2024, 3, 29}), "closed (holiday: Good Friday)");
  EXPECT_EQ(note({2024, 7, 3}), "open, early close 13:00 ET");
  // Before 2022 only weekdays are known.
  EXPECT_EQ(note({2021, 12, 24}), "open");
}

TEST(Time, CboeRunsAnOvernightSessionIntoMostHolidays) {
  using namespace openport::md;
  const auto at = [](std::string_view root, Date d, int h, int m) { return trading_session(root, new_york_to_utc(d, h, m)); };
  // Martin Luther King Jr. Day 2026: Sunday 20:15 to Monday 11:30, then Tuesday's from 20:15.
  EXPECT_EQ(at("SPXW", {2026, 1, 18}, 20, 14).name, "closed");
  EXPECT_EQ(at("SPXW", {2026, 1, 18}, 20, 15).name, "global");
  EXPECT_EQ(at("SPXW", {2026, 1, 19}, 11, 29).end, new_york_to_utc({2026, 1, 19}, 11, 30));
  EXPECT_EQ(at("VIX", {2026, 1, 19}, 11, 30).name, "closed");
  EXPECT_EQ(at("VIX", {2026, 1, 19}, 15, 0).market_time, new_york_to_utc({2026, 1, 19}, 11, 30));
  EXPECT_EQ(at("XSP", {2026, 1, 19}, 20, 15).name, "global");
  EXPECT_EQ(trading_date(new_york_to_utc({2026, 1, 18}, 21, 0)), (Date{2026, 1, 20}));
  EXPECT_EQ(trading_date(new_york_to_utc({2026, 1, 19}, 10, 0)), (Date{2026, 1, 20}));
  EXPECT_EQ(at("SPY", {2026, 1, 19}, 10, 0).name, "closed");  // only the overnight products
  // Thanksgiving: Wednesday 20:15 to Thursday 11:30. A Friday holiday leaves Friday evening closed.
  EXPECT_EQ(at("SPX", {2026, 11, 26}, 9, 30).name, "global");
  EXPECT_EQ(at("SPX", {2026, 6, 19}, 11, 0).name, "global");
  EXPECT_EQ(at("SPX", {2026, 6, 19}, 21, 0).name, "closed");
  // Not into New Year's Day, Good Friday or Christmas.
  EXPECT_EQ(at("SPX", {2025, 12, 31}, 21, 0).name, "closed");
  EXPECT_EQ(at("SPX", {2026, 4, 2}, 21, 0).name, "closed");
  EXPECT_EQ(at("SPX", {2025, 12, 24}, 21, 0).name, "closed");
  EXPECT_EQ(at("SPX", {2025, 12, 25}, 9, 0).name, "closed");
  EXPECT_EQ(at("SPX", {2025, 12, 25}, 20, 15).name, "global");  // Friday's, from Christmas evening
}

TEST(Time, YearFractionPreservesNanosecondsWithoutSignedOverflow) {
  const auto t = new_york_to_utc({2026, 9, 22}, 12, 0);
  EXPECT_DOUBLE_EQ(openport::md::years_between(t, t + 1), 1 / openport::md::kNanosPerYear);
  EXPECT_GT(openport::md::years_between(std::numeric_limits<openport::md::Timestamp>::min(),
                                        std::numeric_limits<openport::md::Timestamp>::max()),
            584);
}

}  // namespace

namespace {
TEST(Time, YearFractionsRetainNanosecondDifferencesAndHandleInt64Extremes) {
  using namespace openport::md;
  constexpr auto low = std::numeric_limits<Timestamp>::min();
  constexpr auto high = std::numeric_limits<Timestamp>::max();
  EXPECT_DOUBLE_EQ(years_between(low, low + 1), 1 / kNanosPerYear);
  EXPECT_DOUBLE_EQ(years_between(high - 1, high), 1 / kNanosPerYear);
  EXPECT_DOUBLE_EQ(years_between(high, high - 1), -1 / kNanosPerYear);
  EXPECT_DOUBLE_EQ(years_between(low, high), -years_between(high, low));
  EXPECT_GT(years_between(low, high), 584);
}

TEST(Time, ProductSessionsHaveDistinctRegularCurbAndGlobalBoundaries) {
  using namespace openport::md;
  const Date date{2026, 9, 22};
  auto session = [&](std::string_view root, int h, int m) {
    return trading_session(root, new_york_to_utc(date, h, m));
  };
  for (auto root : {"SPX", "SPXW", "XSP", "VIX", "VIXW", "RUT", "RUTW"}) {
    EXPECT_EQ(session(root, 9, 24).name, "global") << root;
    EXPECT_EQ(session(root, 9, 25).name, "closed");
    EXPECT_EQ(session(root, 9, 25).market_time, new_york_to_utc(date, 9, 25));
    EXPECT_EQ(session(root, 9, 30).name, "regular");
    EXPECT_EQ(session(root, 16, 14).name, "regular");
    EXPECT_EQ(session(root, 16, 15).name, "curb");
    EXPECT_EQ(session(root, 17, 0).name, "closed");
    EXPECT_EQ(session(root, 20, 14).market_time, new_york_to_utc(date, 17, 0));
    EXPECT_EQ(session(root, 20, 15).name, "global");
    EXPECT_EQ(session(root, 23, 59).market_time, new_york_to_utc(date, 23, 59));
  }
  for (auto root : {"SPY", "QQQ", "IWM", "DIA", "GLD", "TLT", "XLF", "NDX", "NDXP", "XEO"}) {
    EXPECT_EQ(session(root, 16, 14).name, "regular");
    EXPECT_EQ(session(root, 16, 15).name, "closed");
    EXPECT_EQ(session(root, 21, 0).name, "closed");
    EXPECT_EQ(session(root, 21, 0).market_time, new_york_to_utc(date, 16, 15));
  }
  // The stock market closes at 16:00 whatever its options do.
  EXPECT_EQ(stock_session(new_york_to_utc(date, 16, 5)).market_time, new_york_to_utc(date, 16, 0));
  EXPECT_TRUE(stock_session(new_york_to_utc(date, 15, 59)).open);
  EXPECT_EQ(stock_session(new_york_to_utc({2026, 11, 27}, 14, 0)).market_time, new_york_to_utc({2026, 11, 27}, 13, 0));
  EXPECT_EQ(session("AAPL", 15, 59).name, "regular");
  EXPECT_EQ(session("AAPL", 16, 0).name, "closed");
  EXPECT_EQ(session("AAPL", 21, 0).market_time, new_york_to_utc(date, 16, 0));
}

TEST(Time, GlobalTradeDateSkipsWeekendsHolidaysAndHandlesDst) {
  using namespace openport::md;
  auto session = [](Date d, int h, int m) {
    return trading_session("SPXW", new_york_to_utc(d, h, m));
  };
  EXPECT_EQ(session({2026, 9, 25}, 9, 0).name, "global");  // Friday morning
  EXPECT_EQ(session({2026, 9, 25}, 21, 0).name, "closed");
  EXPECT_EQ(session({2026, 9, 26}, 1, 0).name, "closed");
  EXPECT_EQ(session({2026, 9, 27}, 20, 14).name, "closed");
  EXPECT_EQ(session({2026, 9, 27}, 20, 15).name, "global");
  EXPECT_EQ(session({2026, 9, 28}, 1, 0).name, "global");
  EXPECT_EQ(session({2026, 9, 6}, 20, 14).name, "closed");  // before Labor Day
  EXPECT_EQ(session({2026, 9, 6}, 20, 14).market_time, new_york_to_utc({2026, 9, 4}, 17, 0));
  EXPECT_EQ(session({2026, 9, 6}, 21, 0).name, "global");  // into the holiday, until 11:30
  EXPECT_EQ(session({2026, 9, 7}, 12, 0).name, "closed");
  EXPECT_EQ(session({2026, 9, 7}, 20, 15).name, "global");  // Tuesday's session
  for (auto date : {Date{2026, 3, 8}, Date{2026, 11, 1}}) {
    EXPECT_EQ(session(date, 20, 14).name, "closed");
    EXPECT_EQ(session(date, 20, 15).name, "global");
    EXPECT_EQ(session(date, 20, 15).market_time, new_york_to_utc(date, 20, 15));
  }
}

TEST(Time, OpenSessionsReportWhenTheyEnd) {
  using namespace openport::md;
  const Date date{2026, 9, 22};
  auto end = [&](std::string_view root, int h, int m) { return trading_session(root, new_york_to_utc(date, h, m)).end; };
  EXPECT_EQ(end("SPXW", 9, 0), new_york_to_utc(date, 9, 25));
  EXPECT_EQ(end("SPXW", 12, 0), new_york_to_utc(date, 16, 15));
  EXPECT_EQ(end("SPXW", 16, 30), new_york_to_utc(date, 17, 0));
  EXPECT_EQ(end("SPXW", 21, 0), new_york_to_utc({2026, 9, 23}, 9, 25));
  EXPECT_EQ(end("AAPL", 12, 0), new_york_to_utc(date, 16, 0));
  EXPECT_EQ(end("SPY", 18, 0), kInvalidTimestamp);
  EXPECT_EQ(trading_session("SPXW", new_york_to_utc({2026, 11, 27}, 12, 0)).end, new_york_to_utc({2026, 11, 27}, 13, 15));
}

TEST(Time, TradingDatesEndWithTheCurbSessionAndSkipClosures) {
  using namespace openport::md;
  auto date = [](Date d, int h, int m) { return trading_date(new_york_to_utc(d, h, m)); };
  EXPECT_EQ(date({2026, 9, 22}, 0, 30), (Date{2026, 9, 22}));
  EXPECT_EQ(date({2026, 9, 22}, 16, 59), (Date{2026, 9, 22}));
  // Tonight's overnight session trades the next date.
  EXPECT_EQ(date({2026, 9, 22}, 17, 0), (Date{2026, 9, 23}));
  EXPECT_EQ(date({2026, 9, 22}, 21, 0), (Date{2026, 9, 23}));
  EXPECT_EQ(date({2026, 9, 25}, 17, 0), (Date{2026, 9, 28}));  // Friday evening
  EXPECT_EQ(date({2026, 9, 26}, 12, 0), (Date{2026, 9, 28}));
  EXPECT_EQ(date({2026, 9, 27}, 20, 15), (Date{2026, 9, 28}));
  EXPECT_EQ(date({2026, 9, 4}, 18, 0), (Date{2026, 9, 8}));    // over Labor Day
  EXPECT_EQ(date({2026, 9, 7}, 12, 0), (Date{2026, 9, 8}));
  EXPECT_EQ(date({2026, 11, 27}, 14, 0), (Date{2026, 11, 27}));  // after an early close
  for (auto day : {Date{2026, 3, 6}, Date{2026, 3, 9}}) {        // either side of DST
    EXPECT_EQ(date(day, 16, 59), day);
    EXPECT_NE(date(day, 17, 0), day);
  }
  EXPECT_EQ(previous_business_day({2026, 9, 28}), (Date{2026, 9, 25}));
  EXPECT_EQ(previous_business_day({2026, 9, 8}), (Date{2026, 9, 4}));
  EXPECT_EQ(previous_business_day({2026, 9, 23}), (Date{2026, 9, 22}));
}

TEST(Time, ProductEarlyClosesHaveNoCurbAndResumeOnTheNextTradeDate) {
  using namespace openport::md;
  const Date date{2026, 11, 27};
  for (auto root : {"SPXW", "SPY", "QQQ", "RUTW"}) {
    EXPECT_TRUE(trading_session(root, new_york_to_utc(date, 13, 14)).open);
    const auto closed = trading_session(root, new_york_to_utc(date, 16, 30));
    EXPECT_FALSE(closed.open);
    EXPECT_EQ(closed.market_time, new_york_to_utc(date, 13, 15));
  }
  EXPECT_EQ(trading_session("AAPL", new_york_to_utc(date, 13, 1)).market_time,
            new_york_to_utc(date, 13, 0));
  const auto christmas = trading_session("SPX", new_york_to_utc({2026, 12, 24}, 21, 0));
  EXPECT_FALSE(christmas.open);
  EXPECT_EQ(christmas.market_time, new_york_to_utc({2026, 12, 24}, 13, 15));
}
}  // namespace

namespace {
/// Announced days apply only inside a test.
struct Announced {
  explicit Announced(std::vector<openport::md::ScheduledDay> days) { openport::md::set_scheduled_days(std::move(days)); }
  ~Announced() { openport::md::set_scheduled_days({}); }
};

TEST(Time, AnnouncedClosuresAndEarlyClosesOverrideTheRules) {
  using namespace openport::md;
  const Date mourning{2026, 10, 7}, short_day{2026, 10, 8}, labor{2026, 9, 7};
  ASSERT_TRUE(market_session(new_york_to_utc(mourning, 12, 0)).open);
  {
    const Announced announced({{mourning, "National Day of Mourning", true, 13, 0},
                               {short_day, "Early close", false, 13, 0},
                               {labor, "Labor Day", true, 13, 0}});  // no overnight session into it this time
    EXPECT_EQ(market_session(new_york_to_utc(mourning, 12, 0)).note, "closed (holiday: National Day of Mourning)");
    EXPECT_EQ(trading_date(new_york_to_utc({2026, 10, 6}, 18, 0)), short_day);
    EXPECT_EQ(previous_business_day(short_day), (Date{2026, 10, 6}));
    EXPECT_FALSE(trading_session("SPX", new_york_to_utc(mourning, 10, 0)).open);
    EXPECT_EQ(regular_close_hour(short_day), 13);
    EXPECT_EQ(market_session(new_york_to_utc(short_day, 12, 0)).note, "open, early close 13:00 ET");
    EXPECT_EQ(trading_session("SPXW", new_york_to_utc(short_day, 12, 0)).end, new_york_to_utc(short_day, 13, 15));
    EXPECT_EQ(trading_session("SPX", new_york_to_utc({2026, 9, 6}, 21, 0)).name, "closed");
    EXPECT_EQ(scheduled_days().size(), 3u);
  }
  EXPECT_TRUE(scheduled_days().empty());
  EXPECT_TRUE(market_session(new_york_to_utc(mourning, 12, 0)).open);
  EXPECT_EQ(trading_session("SPX", new_york_to_utc({2026, 9, 6}, 21, 0)).name, "global");
}

TEST(Time, UnnamedAnnouncedClosureClosesOptionsAndStocks) {
  using namespace openport::md;
  const Date before{2026, 10, 6}, closed{2026, 10, 7}, after{2026, 10, 8};
  const auto noon = new_york_to_utc(closed, 12, 0);
  const Announced announced({{closed, "", true, 13, 0}});
  const auto market = market_session(noon);
  EXPECT_FALSE(market.open);
  EXPECT_EQ(market.note, "closed (holiday)");
  EXPECT_EQ(market.next_open, new_york_to_utc(after, 9, 30));
  const auto options = trading_session("SPX", noon);
  EXPECT_FALSE(options.open);
  EXPECT_EQ(options.note, "closed (holiday)");
  EXPECT_EQ(options.market_time, new_york_to_utc(before, 17, 0));
  const auto stocks = stock_session(noon);
  EXPECT_FALSE(stocks.open);
  EXPECT_EQ(stocks.note, "closed (holiday)");
  EXPECT_EQ(stocks.market_time, new_york_to_utc(before, 16, 0));
  EXPECT_FALSE(trading_session("SPX", new_york_to_utc(before, 21, 0)).open);
  EXPECT_EQ(trading_date(new_york_to_utc(before, 21, 0)), after);
  EXPECT_EQ(previous_business_day(after), before);
  EXPECT_EQ(regular_close_hour(closed), 16);
}

TEST(Time, UnnamedAnnouncedClosureKeepsItsOvernightSession) {
  using namespace openport::md;
  const Date before{2026, 10, 6}, closed{2026, 10, 7}, after{2026, 10, 8};
  const Announced announced({{closed, "", true, 13, 11 * 60 + 30}});
  const auto end = new_york_to_utc(closed, 11, 30);
  for (const auto time : {new_york_to_utc(before, 20, 15), new_york_to_utc(closed, 9, 30), end - 1}) {
    const auto session = trading_session("SPX", time);
    EXPECT_TRUE(session.open);
    EXPECT_EQ(session.name, "global");
    EXPECT_EQ(session.market_time, time);
    EXPECT_EQ(session.end, end);
    EXPECT_EQ(trading_date(time), after);
  }
  for (const auto time : {end, new_york_to_utc(closed, 16, 30)}) {
    const auto session = trading_session("SPX", time);
    EXPECT_FALSE(session.open);
    EXPECT_EQ(session.note, "closed (holiday)");
    EXPECT_EQ(session.market_time, end);
  }
  EXPECT_FALSE(market_session(new_york_to_utc(closed, 10, 0)).open);
  EXPECT_FALSE(stock_session(new_york_to_utc(closed, 10, 0)).open);
}

// Each thread remembers the calendar answers it has worked out; an announcement
// published on one thread must reach another's next answer, however warm.
TEST(Time, AnnouncementsReachEveryThreadsNextAnswer) {
  using namespace openport::md;
  const Date day{2026, 10, 7};
  const auto noon = new_york_to_utc(day, 12, 0);
  const std::vector<ScheduledDay> closure{{day, "Test closure", true, 13, 0}};
  std::atomic<int> published{0}, checked{0}, wrong{0};
  std::thread reader([&] {
    for (int step = 1; step <= 6; ++step) {
      // Keep this thread's answers warm under the schedule before the step.
      while (published.load(std::memory_order_acquire) < step) (void)trading_session("SPX", noon);
      const bool closed = step % 2 == 1;
      if (market_session(noon).open == closed) ++wrong;
      if (trading_session("SPX", noon).open == closed) ++wrong;
      if (stock_session(noon).open == closed) ++wrong;
      if ((trading_date(noon) == day) == closed) ++wrong;
      checked.store(step, std::memory_order_release);
    }
  });
  for (int step = 1; step <= 6; ++step) {
    set_scheduled_days(step % 2 == 1 ? closure : std::vector<ScheduledDay>{});
    published.store(step, std::memory_order_release);
    while (checked.load(std::memory_order_acquire) < step) std::this_thread::yield();
  }
  reader.join();
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_TRUE(scheduled_days().empty());
}
}  // namespace

TEST(Time, RecordedCalendarScopeIsThreadLocalAndRestoresNestedScopes) {
  using namespace openport::md;
  const Date day{2026, 9, 22};
  const auto global = scheduled_days();
  const auto original = regular_close_hour(day);
  {
    const ScheduledDaysScope recorded({{day, "Recorded early close", false, 13, 0}});
    EXPECT_EQ(regular_close_hour(day), 13);
    EXPECT_EQ(scheduled_days().front().name, "Recorded early close");
    auto other = std::async(std::launch::async, [&] {
      EXPECT_EQ(scheduled_days(), global);
      EXPECT_EQ(regular_close_hour(day), original);
    });
    other.get();
    { const ScheduledDaysScope nested({}); EXPECT_EQ(regular_close_hour(day), 16); }
    EXPECT_EQ(regular_close_hour(day), 13);
  }
  EXPECT_EQ(scheduled_days(), global);
  EXPECT_EQ(regular_close_hour(day), original);
}

namespace {
TEST(Calendar, StockExtendedHoursRunPremarketAndAfterHoursOnBusinessDays) {
  using namespace openport::md;
  const Date day{2026, 9, 22};
  EXPECT_FALSE(stock_extended_session(new_york_to_utc(day, 3, 59)).open);
  const auto pre = stock_extended_session(new_york_to_utc(day, 4, 0));
  EXPECT_EQ(pre.name, "premarket");
  EXPECT_EQ(pre.end, new_york_to_utc(day, 9, 30));
  EXPECT_EQ(stock_extended_session(new_york_to_utc(day, 12, 0)).name, "regular");
  const auto after = stock_extended_session(new_york_to_utc(day, 16, 0));
  EXPECT_EQ(after.name, "after_hours");
  EXPECT_EQ(after.end, new_york_to_utc(day, 20, 0));
  EXPECT_FALSE(stock_extended_session(new_york_to_utc(day, 20, 0)).open);
  // An early close ends after hours at 17:00; weekends and holidays have none.
  EXPECT_EQ(stock_extended_session(new_york_to_utc({2026, 11, 27}, 13, 30)).end, new_york_to_utc({2026, 11, 27}, 17, 0));
  EXPECT_FALSE(stock_extended_session(new_york_to_utc({2026, 11, 27}, 17, 0)).open);
  EXPECT_FALSE(stock_extended_session(new_york_to_utc({2026, 9, 26}, 8, 0)).open);
  EXPECT_FALSE(stock_extended_session(new_york_to_utc({2026, 11, 26}, 8, 0)).open);
}
}  // namespace
