#include "openport/server/candles.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/recording.hpp"

namespace {

using namespace openport;
using server::BarInterval;
using server::CandleStore;

/// 2026-09-22 (a Tuesday) at a New York wall-clock time.
md::Timestamp ny(int hour, int minute, int second = 0, md::Date day = {2026, 9, 22}) {
  return md::new_york_to_utc(day, hour, minute, second);
}

md::Bar bar(md::Timestamp start, double open, double high, double low, double close) {
  return {start, open, high, low, close};
}

std::size_t lines_in(const std::filesystem::path& file) {
  std::ifstream in(file);
  std::size_t count = 0;
  for (std::string line; std::getline(in, line);) ++count;
  return count;
}

TEST(Candles, IntervalsParseAndPrint) {
  for (const char* text : {"1m", "5m", "15m", "30m", "1h", "1d"}) {
    const auto interval = server::parse_bar_interval(text);
    ASSERT_TRUE(interval) << text;
    EXPECT_EQ(server::to_string(*interval), text);
  }
  EXPECT_FALSE(server::parse_bar_interval("2m"));
  EXPECT_FALSE(server::parse_bar_interval(""));
}

TEST(Candles, SamplesBuildMinuteBarsInMarketTime) {
  CandleStore store;
  store.sample("SPX", ny(10, 0, 10), 100.0);
  store.sample("SPX", ny(10, 0, 40), 103.0);
  store.sample("SPX", ny(10, 0, 20), 99.0);
  store.sample("SPX", ny(10, 0, 5), 101.0);  // earlier than every other sample: the open
  store.sample("SPX", ny(10, 1, 0), 104.0);
  store.sample("SPX", ny(10, 1, 30), std::nan(""));
  store.sample("SPX", ny(10, 1, 30), -1.0);
  const auto bars = store.bars("SPX", BarInterval::Minute, 10);
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_EQ(bars[0], bar(ny(10, 0), 101.0, 103.0, 99.0, 103.0));
  EXPECT_EQ(bars[1], bar(ny(10, 1), 104.0, 104.0, 104.0, 104.0));
  EXPECT_TRUE(store.bars("SPY", BarInterval::Minute, 10).empty());
  EXPECT_TRUE(store.bars("SPX", BarInterval::Minute, 0).empty());
}

TEST(Candles, OfficialMinutesReplaceSamplesAndLaterSamplesLeaveThemAlone) {
  CandleStore store;
  store.sample("SPX", ny(10, 0, 10), 100.0);
  store.sample("SPX", ny(10, 1, 10), 102.0);
  store.merge_minutes("SPX", {bar(ny(10, 0), 100.5, 101.0, 99.5, 100.2),
                              bar(ny(10, 0, 30), 1, 1, 1, 1),        // not on a whole minute
                              bar(ny(10, 2), 100.0, 99.0, 98.0, 100.0)});  // high below the open
  store.sample("SPX", ny(10, 0, 50), 120.0);
  const auto bars = store.bars("SPX", BarInterval::Minute, 10);
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_EQ(bars[0], bar(ny(10, 0), 100.5, 101.0, 99.5, 100.2));
  EXPECT_EQ(bars[1], bar(ny(10, 1), 102.0, 102.0, 102.0, 102.0));
}

TEST(Candles, IntervalsGroupMinutesAndHoursStartAtHalfPast) {
  CandleStore store;
  // 09:30 to 11:29, one minute each, rising by one.
  std::vector<md::Bar> minutes;
  for (int i = 0; i < 120; ++i) {
    const double open = 100.0 + i;
    minutes.push_back(bar(ny(9, 30) + i * md::kNanosPerMinute, open, open + 1.5, open - 0.5, open + 1));
  }
  store.merge_minutes("SPY", minutes);
  const auto five = store.bars("SPY", BarInterval::FiveMinutes, 1000);
  ASSERT_EQ(five.size(), 24u);
  EXPECT_EQ(five[0], bar(ny(9, 30), 100.0, 105.5, 99.5, 105.0));
  EXPECT_EQ(five[1].start, ny(9, 35));
  const auto hours = store.bars("SPY", BarInterval::Hour, 1000);
  ASSERT_EQ(hours.size(), 2u);
  EXPECT_EQ(hours[0], bar(ny(9, 30), 100.0, 160.5, 99.5, 160.0));
  EXPECT_EQ(hours[1], bar(ny(10, 30), 160.0, 220.5, 159.5, 220.0));
  const auto last = store.bars("SPY", BarInterval::FifteenMinutes, 3);
  ASSERT_EQ(last.size(), 3u);
  EXPECT_EQ(last.back().start, ny(11, 15));
  EXPECT_EQ(store.bars("SPY", BarInterval::ThirtyMinutes, 1000).size(), 4u);
}

TEST(Candles, DailyBarsAreTheVendorsAndUnpublishedSessionsComeFromRegularHours) {
  CandleStore store;
  const md::Date monday{2026, 9, 21};
  const md::Date tuesday{2026, 9, 22};
  store.merge_days("SPX", {bar(ny(9, 30, 0, monday), 7692.83, 7779.22, 7691.19, 7764.70)});
  // Monday's minutes never override its published bar.
  store.merge_minutes("SPX", {bar(ny(15, 0, 0, monday), 7700, 7900, 7600, 7800)});
  // Tuesday: overnight and after-hours minutes stay out of the session's bar.
  store.sample("SPX", ny(3, 0, 0, tuesday), 7000.0);
  store.merge_minutes("SPX", {bar(ny(9, 30, 0, tuesday), 7770.81, 7773.58, 7769.60, 7772.65),
                              bar(ny(12, 0, 0, tuesday), 7772.0, 7782.19, 7771.0, 7780.0),
                              bar(ny(15, 59, 0, tuesday), 7766.0, 7767.0, 7756.26, 7764.64)});
  store.sample("SPX", ny(16, 5, 0, tuesday), 7900.0);
  const auto days = store.bars("SPX", BarInterval::Day, 10);
  ASSERT_EQ(days.size(), 2u);
  EXPECT_EQ(days[0], bar(ny(9, 30, 0, monday), 7692.83, 7779.22, 7691.19, 7764.70));
  EXPECT_EQ(days[1], bar(ny(9, 30, 0, tuesday), 7770.81, 7782.19, 7756.26, 7764.64));
  ASSERT_EQ(store.bars("SPX", BarInterval::Day, 1).size(), 1u);
  EXPECT_EQ(store.bars("SPX", BarInterval::Day, 1)[0].start, ny(9, 30, 0, tuesday));
  // Once the vendor publishes Tuesday, its bar wins.
  store.merge_days("SPX", {bar(ny(9, 30, 0, tuesday), 7770.81, 7782.19, 7756.26, 7764.64),
                           bar(ny(9, 30, 0, {2026, 9, 18}), 0, 7657.17, 7610.52, 7650.5)});  // no open
  EXPECT_EQ(store.bars("SPX", BarInterval::Day, 10).size(), 2u);
}

TEST(Candles, DailyHistoryIsBounded) {
  CandleStore store(CandleStore::Options{{}, std::chrono::days{10}, 3});
  std::vector<md::Bar> days;
  for (int d = 14; d <= 18; ++d) days.push_back(bar(ny(9, 30, 0, {2026, 9, d}), 100, 101, 99, 100));
  store.merge_days("SPX", days);
  const auto kept = store.bars("SPX", BarInterval::Day, 10);
  ASSERT_EQ(kept.size(), 3u);
  EXPECT_EQ(kept.front().start, ny(9, 30, 0, {2026, 9, 16}));
}

TEST(Candles, MinutesOlderThanTheRetainedWindowAreDropped) {
  CandleStore store(CandleStore::Options{{}, std::chrono::days{1}});
  store.sample("SPY", ny(10, 0, 0, {2026, 9, 20}), 100.0);
  store.sample("SPY", ny(9, 59, 0, {2026, 9, 21}), 101.0);
  store.sample("SPY", ny(10, 0, 0, {2026, 9, 22}), 102.0);
  const auto bars = store.bars("SPY", BarInterval::Minute, 10);
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_EQ(bars[0].close, 102.0);
}

// Each test's candle directory is new, inside a fresh mkdtemp directory removed with it,
// so concurrent runs of one test never share files.
TEST(Candles, FinishedMinutesPersistAndReload) {
  const test::RecordingFile scratch;
  const auto directory = scratch.directory / "candles";
  {
    CandleStore store(CandleStore::Options{directory});
    ASSERT_EQ(store.error(), "");
    store.sample("SPX", ny(10, 0, 10), 100.0);
    store.sample("SPX", ny(10, 0, 40), 101.0);
    EXPECT_EQ(lines_in(directory / "SPX.csv"), 0u) << "a forming minute is not written";
    store.sample("SPX", ny(10, 1, 5), 102.0);
    EXPECT_EQ(lines_in(directory / "SPX.csv"), 1u);
    store.merge_minutes("SPX", {bar(ny(9, 59), 99.0, 100.0, 98.0, 99.5)});
    EXPECT_EQ(lines_in(directory / "SPX.csv"), 2u);
    store.sample("BRK/B", ny(10, 0), 1.0);  // not a file name: kept in memory only
  }  // destruction writes the forming minute
  EXPECT_EQ(lines_in(directory / "SPX.csv"), 3u);
  {
    std::ofstream(directory / "SPX.csv", std::ios::app) << "1790000000,1,2\n";  // cut short by a crash
    CandleStore store(CandleStore::Options{directory});
    const auto bars = store.bars("SPX", BarInterval::Minute, 10);
    ASSERT_EQ(bars.size(), 3u);
    EXPECT_EQ(bars[0], bar(ny(9, 59), 99.0, 100.0, 98.0, 99.5));
    EXPECT_EQ(bars[1], bar(ny(10, 0), 100.0, 101.0, 100.0, 101.0));
    EXPECT_EQ(bars[2], bar(ny(10, 1), 102.0, 102.0, 102.0, 102.0));
    // The reload compacted away the broken line; an official minute stays official.
    EXPECT_EQ(lines_in(directory / "SPX.csv"), 3u);
    store.sample("SPX", ny(9, 59, 30), 500.0);
    EXPECT_EQ(store.bars("SPX", BarInterval::Minute, 10)[0].high, 100.0);
    EXPECT_FALSE(std::filesystem::exists(directory / "BRK" / "B.csv"));
  }
}

TEST(Candles, RewrittenMinutesCompactTheFileOnReload) {
  const test::RecordingFile scratch;
  const auto directory = scratch.directory / "candles";
  {
    CandleStore store(CandleStore::Options{directory});
    store.sample("QQQ", ny(10, 0), 100.0);
    store.sample("QQQ", ny(10, 1), 101.0);
    store.merge_minutes("QQQ", {bar(ny(10, 0), 100.0, 100.5, 99.5, 100.25)});
    store.merge_minutes("QQQ", {bar(ny(10, 0), 100.0, 100.75, 99.5, 100.25)});
    store.flush();
    EXPECT_EQ(lines_in(directory / "QQQ.csv"), 4u);
  }
  CandleStore store(CandleStore::Options{directory});
  EXPECT_EQ(lines_in(directory / "QQQ.csv"), 2u);
  // A reload that lost the minutes fails here instead of indexing an empty vector.
  const auto bars = store.bars("QQQ", BarInterval::Minute, 10);
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_EQ(bars[0].high, 100.75);
}

// One mis-stamped minute, as a vendor's millisecond time read as seconds would give,
// became the newest and pruned every real minute ten days before it, in memory and
// at the next start on disk, and later real samples were no longer persisted. A live
// store's minutes are bounded by its wall clock instead.
TEST(Candles, AFarFutureMinuteNeitherPrunesHistoryNorStopsPersisting) {
  const test::RecordingFile scratch;
  const auto directory = scratch.directory / "candles";
  const auto now = ny(16, 0);
  const auto far = now + 365 * md::kNanosPerDay;
  const CandleStore::Options options{directory, std::chrono::days{10}, 2600, [now] { return now; }};
  {
    CandleStore store(options);
    for (int minute = 0; minute < 30; ++minute) store.sample("SPX", ny(10, minute), 100.0 + minute);
    store.sample("SPX", far, 50.0);
    store.merge_minutes("SPX", {bar(now + 30 * md::kNanosPerDay, 1, 1, 1, 1)});
    store.sample("SPX", ny(10, 30), 130.0);
    store.sample("SPX", ny(10, 31), 131.0);
    const auto bars = store.bars("SPX", BarInterval::Minute, 100);
    ASSERT_EQ(bars.size(), 32u);
    EXPECT_EQ(bars.front().start, ny(10, 0));
    EXPECT_EQ(bars.back().start, ny(10, 31));
  }
  EXPECT_EQ(lines_in(directory / "SPX.csv"), 32u);
  // A file an earlier version wrote with such a minute heals at the next start.
  { std::ofstream out(directory / "SPX.csv", std::ios::app); out << far / md::kNanosPerSecond << ",1,1,1,1,s\n"; }
  {
    CandleStore reopened(options);
    EXPECT_EQ(reopened.bars("SPX", BarInterval::Minute, 100).size(), 32u);
  }
  EXPECT_EQ(lines_in(directory / "SPX.csv"), 32u);
  // Demo and replay market time can run ahead of the wall clock: no clock, no bound.
  CandleStore simulated;
  simulated.sample("SPX", far, 50.0);
  EXPECT_EQ(simulated.bars("SPX", BarInterval::Minute, 10).size(), 1u);
}

TEST(Candles, AnUnwritableDirectoryIsReportedNotThrown) {
  const test::RecordingFile scratch;
  const auto directory = scratch.directory / "candles";
  std::ofstream(directory.string()) << "a file, not a directory";
  CandleStore store(CandleStore::Options{directory / "candles"});
  EXPECT_NE(store.error(), "");
  store.sample("SPX", ny(10, 0), 100.0);
  EXPECT_EQ(store.bars("SPX", BarInterval::Minute, 10).size(), 1u);
}

}  // namespace

TEST(Candles, CloseOnlyDailyHistoryDoesNotHideValidMinuteBuiltChartCandles) {
  using namespace openport;
  server::CandleStore store;
  const auto time = md::new_york_to_utc({2026, 9, 25}, 9, 30);
  store.merge_days("SPX", {{time, 0, 102, 99, 101}});
  EXPECT_TRUE(store.bars("SPX", server::BarInterval::Day, 10).empty());
  store.merge_minutes("SPX", {{time, 100, 101, 100, 101}});
  const auto chart = store.bars("SPX", server::BarInterval::Day, 10);
  ASSERT_EQ(chart.size(), 1u);
  EXPECT_DOUBLE_EQ(chart[0].open, 100);
  EXPECT_TRUE(md::valid_bar(chart[0]));
  const auto history = store.daily_history("SPX");
  ASSERT_EQ(history.size(), 1u);
  EXPECT_DOUBLE_EQ(history[0].open, 0);
  EXPECT_DOUBLE_EQ(history[0].close, 101);
}
