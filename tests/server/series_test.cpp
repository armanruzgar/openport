#include "openport/server/series.hpp"
#include "openport/analytics/volatility_history.hpp"
#include "openport/server/api.hpp"
#include "openport/providers/replay.hpp"
#include "server/metric_cache.hpp"
#include "support/recording.hpp"
#include "support/synthetic_chain.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
using namespace openport;
using nlohmann::json;
md::Timestamp ny(int hour, int minute, md::Date day = {2026, 9, 22}) { return md::new_york_to_utc(day, hour, minute); }
server::SeriesRow row(md::Timestamp time, double value = 20) {
  server::SeriesRow out;
  out.time = time; out.values[3] = value;
  return out;
}
std::string read(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}
class SeriesSource final : public server::MetricsSource {
 public:
  SeriesSource() {
    test::SyntheticChain chain;
    snapshot = std::make_shared<const analytics::UnderlyingMetrics>(analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of));
    state.provider = "cboe";
    state.underlyings["SPX"].state = md::FeedState::Delayed;
  }
  std::vector<std::string> symbols() const override { return {snapshot->symbol}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override { return symbol == snapshot->symbol ? snapshot : nullptr; }
  server::EngineStatus status() const override { return state; }
  md::Timestamp wall_time() const override { return ny(17, 0); }
  const server::SeriesStore* series() const override { return &store; }
  const server::CandleStore* candles() const override { return &bars; }
  std::shared_ptr<const analytics::UnderlyingMetrics> snapshot;
  server::EngineStatus state;
  server::SeriesStore store;
  server::CandleStore bars;
};

std::vector<server::SeriesRow> history_samples(bool sessions_only) {
  std::vector<server::SeriesRow> samples;
  auto date = md::Date{2026, 6, 25};
  for (int day = 0; day < 60; ++day) {
    for (const auto minute : {9 * 60 + 30, 15 * 60 + 59, 16 * 60 + 1})
      samples.push_back(row(ny(minute / 60, minute % 60, date), day + 1));
    do { date = md::date_from_days(md::days_since_epoch(date) + 1); }
    while (sessions_only && !md::market_session(ny(9, 30, date)).open);
  }
  return samples;
}

void expect_history(const server::SeriesStore& store, const std::vector<server::SeriesRow>& samples) {
  const auto cutoff = samples.back().time - server::kSeriesMinuteRetention;
  const auto expected = static_cast<std::size_t>(std::count_if(samples.begin(), samples.end(),
      [&](const auto& sample) { return sample.time > cutoff; }));
  EXPECT_EQ(store.cached_minutes("SPX"), expected);
  const auto days = store.rows("SPX", samples.front().time, samples.back().time, true);
  ASSERT_EQ(days.size(), 60u);
  std::vector<analytics::VolatilityDay> own;
  for (std::size_t i = 0; i < days.size(); ++i) {
    EXPECT_EQ(days[i].values[3], static_cast<double>(i + 1));
    own.push_back({md::new_york_time(days[i].time).date, days[i].values[3], days[i].values[8]});
  }
  const auto history = analytics::historical_iv(own, {}, "", samples.back().time);
  const auto rank = analytics::iv_rank(history, 30.5, samples.back().time);
  EXPECT_EQ(rank.sessions, 60u);
  EXPECT_DOUBLE_EQ(rank.rank, .5);
  EXPECT_DOUBLE_EQ(rank.percentile, .5);
}

TEST(Series, KeepsExactly45DaysOfMinutesPerSymbolAndReadsOlderRangesWithoutCaching) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  server::SeriesStore store(directory);
  const auto samples = history_samples(false);
  for (const auto& sample : samples) ASSERT_TRUE(store.insert("SPX", sample));
  ASSERT_TRUE(store.insert("SPY", samples.front()));
  EXPECT_EQ(server::kSeriesMinuteRetention, 45 * md::kNanosPerDay);
  EXPECT_EQ(store.cached_minutes("SPX"), 45u * 3);
  EXPECT_EQ(store.cached_minutes("SPY"), 1u);
  EXPECT_EQ(store.cached_minutes("QQQ"), 0u);
  // Days 1-10 span June and July, entirely outside the retained window.
  const auto old = store.rows("SPX", samples.front().time, samples[29].time);
  ASSERT_EQ(old.size(), 30u);
  for (std::size_t i = 0; i < old.size(); ++i) {
    EXPECT_EQ(old[i].time, samples[i].time);
    EXPECT_EQ(old[i].values[3], samples[i].values[3]);
    EXPECT_TRUE(std::isnan(old[i].values[0]));
  }
  // Read both sides of the exact cutoff, without duplicate boundary rows.
  const auto crossing = store.rows("SPX", samples[42].time, samples[50].time);
  ASSERT_EQ(crossing.size(), 9u);
  for (std::size_t i = 0; i < crossing.size(); ++i) EXPECT_EQ(crossing[i].time, samples[i + 42].time);
  EXPECT_EQ(store.cached_minutes("SPX"), 45u * 3);
  server::SeriesStore restored(directory);
  EXPECT_EQ(restored.cached_minutes("SPX"), 45u * 3);
  EXPECT_EQ(restored.cached_minutes("SPY"), 1u);
  EXPECT_EQ(restored.rows("SPX", samples.front().time, samples[29].time).size(), 30u);
}

TEST(Series, DailyHistoryAndRankKeepAll60SessionsAcrossEvictionAndRestart) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  const auto samples = history_samples(true);
  {
    server::SeriesStore store(directory);
    // Reverse chronological writes also exercise loading unsorted monthly records.
    for (auto it = samples.rbegin(); it != samples.rend(); ++it) ASSERT_TRUE(store.insert("SPX", *it));
    expect_history(store, samples);
  }
  server::SeriesStore restored(directory);
  expect_history(restored, samples);
  const auto old = restored.rows("SPX", samples.front().time, samples[29].time);
  ASSERT_EQ(old.size(), 30u);
  EXPECT_EQ(old.front().time, samples.front().time);
  EXPECT_EQ(old.back().time, samples[29].time);
  expect_history(restored, samples);
  // Daily reads use cached rows even when the CSV directory is unavailable.
  std::filesystem::rename(directory, temp.directory / "moved");
  expect_history(restored, samples);
  EXPECT_TRUE(restored.status(samples.back().time).last_error.empty());
}

TEST(Series, MemoryOnlyStoreDropsOlderMinutesButKeepsDailyHistoryAndRank) {
  server::SeriesStore store;
  const auto samples = history_samples(true);
  for (const auto& sample : samples) ASSERT_TRUE(store.insert("SPX", sample));
  EXPECT_TRUE(store.rows("SPX", samples.front().time, samples[29].time).empty());
  EXPECT_FALSE(store.contains("SPX", samples.front().time));
  expect_history(store, samples);
  const auto recent = store.rows("SPX", samples.front().time, samples.back().time);
  EXPECT_EQ(recent.size(), store.cached_minutes("SPX"));
  ASSERT_FALSE(recent.empty());
  EXPECT_GT(recent.front().time, samples.back().time - server::kSeriesMinuteRetention);
}

TEST(Series, EvictedMinutesKeepDuplicateAndForceSemanticsAndUpdateTheDailyClose) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  server::SeriesStore store(directory);
  const auto samples = history_samples(true);
  for (const auto& sample : samples) ASSERT_TRUE(store.insert("SPX", sample));
  const auto count = store.cached_minutes("SPX");
  const auto path = directory / "SPX-2026-06.csv";
  const auto before = read(path);
  auto changed = samples[1];  // The first session's selected closing minute.
  changed.values[3] = 80;
  EXPECT_TRUE(store.contains("SPX", changed.time));
  EXPECT_FALSE(store.insert("SPX", changed));
  EXPECT_FALSE(store.insert("SPX", samples[1], true));
  EXPECT_EQ(read(path), before);
  EXPECT_TRUE(store.insert("SPX", changed, true));
  const auto corrected = read(path);
  EXPECT_TRUE(corrected.starts_with(before));
  EXPECT_FALSE(store.insert("SPX", changed, true));
  EXPECT_EQ(read(path), corrected);
  EXPECT_EQ(store.rows("SPX", changed.time, changed.time).front().values[3], 80);
  EXPECT_EQ(store.rows("SPX", samples.front().time, samples.back().time, true).front().values[3], 80);
  EXPECT_EQ(store.cached_minutes("SPX"), count);
  server::SeriesStore restored(directory);
  EXPECT_EQ(restored.cached_minutes("SPX"), count);
  EXPECT_EQ(restored.rows("SPX", samples.front().time, samples.back().time, true).front().values[3], 80);
}

TEST(Series, OnDemandReadsUseLoaderDamageHandlingAndKeepTheValidPrefix) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  server::SeriesStore store(directory);
  const auto old = row(ny(15, 59, {2026, 6, 25}));
  ASSERT_TRUE(store.insert("SPX", old));
  ASSERT_TRUE(store.insert("SPX", row(ny(15, 59, {2026, 9, 22}))));
  ASSERT_EQ(store.cached_minutes("SPX"), 1u);
  const auto path = directory / "SPX-2026-06.csv";
  { std::ofstream output(path, std::ios::app); output << "123,"; }
  const auto damaged = read(path);
  const auto values = store.rows("SPX", old.time, old.time);
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0].values[3], old.values[3]);
  EXPECT_NE(store.status(0).last_error.find("torn"), std::string::npos);
  EXPECT_EQ(store.cached_minutes("SPX"), 1u);
  EXPECT_FALSE(store.insert("SPX", row(old.time, 50), true));
  EXPECT_EQ(read(path), damaged);
  server::SeriesStore restored(directory);
  EXPECT_EQ(restored.cached_minutes("SPX"), 1u);
  EXPECT_NE(restored.status(0).last_error.find("torn"), std::string::npos);
  EXPECT_EQ(restored.rows("SPX", old.time, old.time).size(), 1u);
  EXPECT_EQ(restored.rows("SPX", old.time, ny(17, 0, {2026, 9, 22}), true).size(), 2u);
  std::filesystem::remove(path);
  EXPECT_TRUE(store.rows("SPX", old.time, old.time).empty());
  EXPECT_NE(store.status(0).last_error.find("cannot read series"), std::string::npos);
}

TEST(Series, CalendarChangesReindexEvictedClosesFromDiskWithoutRefillingMinutes) {
  test::RecordingFile temp;
  server::SeriesStore disk(temp.directory / "series"), memory;
  const md::Date early{2026, 6, 25};
  for (auto* store : {&disk, &memory}) {
    ASSERT_TRUE(store->insert("SPX", row(ny(12, 59, early), 10)));
    ASSERT_TRUE(store->insert("SPX", row(ny(15, 59, early), 20)));
    ASSERT_TRUE(store->insert("SPX", row(ny(15, 59), 30)));
    EXPECT_EQ(store->cached_minutes("SPX"), 1u);
  }
  const auto previous = md::scheduled_days();
  md::set_scheduled_days({{early, "test early close", false, 13}});
  const auto disk_days = disk.rows("SPX", 0, ny(17, 0), true);
  const auto memory_days = memory.rows("SPX", 0, ny(17, 0), true);
  md::set_scheduled_days(previous);
  ASSERT_EQ(disk_days.size(), 2u);
  EXPECT_EQ(disk_days.front().time, ny(12, 59, early));
  EXPECT_EQ(disk_days.front().values[3], 10);
  ASSERT_EQ(memory_days.size(), 1u);
  EXPECT_EQ(memory_days.front().values[3], 30);
  EXPECT_EQ(disk.cached_minutes("SPX"), 1u);
  EXPECT_EQ(memory.cached_minutes("SPX"), 1u);
  EXPECT_TRUE(disk.status(0).last_error.empty());
}

TEST(Series, RoundTripReloadMissingAndForceCorrectionsAreAppendOnly) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  const auto path = directory / "SPX-2026-09.csv";
  std::string first;
  {
    server::SeriesStore store(directory);
    auto sample = row(ny(10, 0), 20.123456789012345);
    sample.values[12] = 0;
    ASSERT_TRUE(store.insert("SPX", sample));
    first = read(path);
    EXPECT_TRUE(first.starts_with("#openport-series,1\nminute,spot,forward"));
    EXPECT_EQ(first.find("nan"), std::string::npos);
    EXPECT_FALSE(store.insert("SPX", row(ny(10, 0), 25)));
    EXPECT_EQ(read(path), first);
    EXPECT_TRUE(store.insert("SPX", row(ny(10, 0), 25), true));
    EXPECT_TRUE(read(path).starts_with(first));
    const auto corrected = read(path);
    EXPECT_FALSE(store.insert("SPX", row(ny(10, 0), 25), true));
    EXPECT_EQ(read(path), corrected);
    EXPECT_TRUE(store.insert("SPY", sample));
    EXPECT_TRUE(store.insert("SPX", row(ny(10, 0, {2026, 10, 1}))));
  }
  server::SeriesStore loaded(directory);
  const auto values = loaded.rows("SPX", ny(9, 0), ny(11, 0));
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0].values[3], 25);
  EXPECT_TRUE(std::isnan(values[0].values[0]));
  EXPECT_EQ(loaded.rows("SPY", 0, ny(17, 0))[0].values[3], 20.123456789012345);
  EXPECT_EQ(loaded.rows("SPY", 0, ny(17, 0))[0].values[12], 0);
  EXPECT_EQ(loaded.status(ny(17, 0)).rows_today, 2u);
  EXPECT_TRUE(loaded.status(0).last_error.empty());
}
TEST(Series, DailySelectsCloseOrLastRegularMinuteIncludingEarlyCloseAndDst) {
  server::SeriesStore store;
  for (const auto time : {ny(9, 29), ny(15, 59), ny(16, 0), ny(16, 1), ny(18, 0)}) store.insert("SPX", row(time));
  EXPECT_TRUE(store.rows("SPX", 0, ny(15, 59), true).empty());
  auto days = store.rows("SPX", 0, ny(19, 0), true);
  ASSERT_EQ(days.size(), 1u);
  EXPECT_EQ(days[0].time, ny(16, 0));
  const md::Date early{2026, 11, 27};
  store.insert("SPX", row(ny(12, 58, early)));
  store.insert("SPX", row(ny(13, 1, early)));
  days = store.rows("SPX", ny(0, 0, early), ny(17, 0, early), true);
  ASSERT_EQ(days.size(), 1u);
  EXPECT_EQ(days[0].time, ny(12, 58, early));
  store.insert("SPX", row(ny(12, 0, {2026, 11, 28})));
  EXPECT_EQ(store.rows("SPX", 0, ny(17, 0, {2026, 11, 28}), true).size(), 2u);
}
TEST(Series, DailySelectionFollowsAnAnnouncedCalendarChangeAfterSampling) {
  server::SeriesStore store;
  store.insert("SPX", row(ny(12, 59), 10));
  store.insert("SPX", row(ny(15, 59), 20));
  const auto previous = md::scheduled_days();
  md::set_scheduled_days({{{2026, 9, 22}, "test early close", false, 13}});
  const auto days = store.rows("SPX", 0, ny(17, 0), true);
  md::set_scheduled_days(previous);
  ASSERT_EQ(days.size(), 1u);
  EXPECT_EQ(days[0].values[3], 10);
}
TEST(Series, BackgroundWorkerPublishesAndStopsWithoutEngineWork) {
  SeriesSource source;
  server::SeriesWorker worker(source, source.store);
  worker.start();
  const bool written = test::recording_eventually([&] { return source.store.contains("SPX", ny(15, 0)); });
  worker.stop();
  EXPECT_TRUE(written);
  EXPECT_EQ(source.store.status(ny(17, 0)).rows_today, 1u);
}
TEST(Series, ExposureStaysMissingWithoutOpenInterestAndKeepsRealZero) {
  auto metrics = std::make_shared<analytics::UnderlyingMetrics>();
  metrics->as_of = ny(10, 0);
  EXPECT_TRUE(std::isnan(server::series_row(metrics).values[18]));
  auto quoted = std::make_shared<analytics::UnderlyingMetrics>(*metrics);
  quoted->exposure.oi_coverage = 1;
  EXPECT_EQ(server::series_row(quoted).values[18], 0);
}
TEST(Series, StorageFailuresAndTornFilesAreReportedWithoutThrowing) {
  test::RecordingFile temp;
  { std::ofstream output(temp.path); output << "file"; }
  server::SeriesStore failed(temp.path);
  EXPECT_FALSE(failed.status(0).last_error.empty());
  EXPECT_FALSE(failed.insert("SPX", row(ny(10, 0))));
  EXPECT_FALSE(failed.insert("../SPX", row(ny(10, 0))));
  const auto directory = temp.directory / "series";
  { server::SeriesStore store(directory); ASSERT_TRUE(store.insert("SPX", row(ny(10, 0)))); }
  const auto path = directory / "SPX-2026-09.csv";
  { std::ofstream output(path, std::ios::app); output << "123,"; }
  const auto before = read(path);
  server::SeriesStore damaged(directory);
  EXPECT_EQ(damaged.rows("SPX", 0, ny(17, 0)).size(), 1u);
  EXPECT_NE(damaged.status(0).last_error.find("torn"), std::string::npos);
  EXPECT_FALSE(damaged.insert("SPX", row(ny(10, 1))));
  EXPECT_EQ(read(path), before);
}
TEST(Series, WorkerThrottlesByMarketMinuteAndReusesSnapshotCache) {
  SeriesSource source;
  server::SeriesWorker worker(source, source.store);
  worker.sample(); worker.sample();
  EXPECT_EQ(source.store.status(ny(17, 0)).rows_today, 1u);
  const auto cache = server::surface_cache(source.snapshot);
  ASSERT_TRUE(cache->volatility);
  EXPECT_EQ(cache->fits.size(), source.snapshot->slices.size());
  auto next = *source.snapshot;
  next.as_of += 30 * md::kNanosPerSecond; next.version++;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(next);
  worker.sample();
  EXPECT_FALSE(server::surface_cache(source.snapshot)->volatility.has_value());
  next.as_of += md::kNanosPerMinute;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(next);
  source.state.underlyings["SPX"].state = md::FeedState::Stale;
  worker.sample();
  EXPECT_EQ(source.store.status(ny(17, 0)).rows_today, 1u);
  source.state.underlyings["SPX"].state = md::FeedState::Delayed;
  worker.sample();
  EXPECT_EQ(source.store.status(ny(17, 0)).rows_today, 2u);
}
TEST(Series, ReplayAndDemoAreExcludedEvenWithInheritedStoreOptions) {
  SeriesSource source;
  server::SeriesWorker worker(source, source.store);
  for (const auto* provider : {"demo", "replay", "replay (demo)", "replay (cboe)"}) {
    source.state.provider = provider;
    worker.sample();
  }
  EXPECT_EQ(source.store.status(ny(17, 0)).rows_today, 0u);
  test::RecordingFile file;
  for (const auto* provider_name : {"demo", "cboe"}) {
    std::filesystem::remove(file.path);
    auto header = test::recording_header(); header.provider = provider_name;
    test::record_events(file.path, {}, header);
    providers::ReplayProvider::Options replay_options;
    replay_options.file = file.path;
    providers::ReplayProvider provider(replay_options);
    server::Engine::Options options;
    options.series = std::make_shared<server::SeriesStore>();
    options.paper_enabled = false;
    server::Engine engine(provider, {{"SPX"}}, options);
    EXPECT_EQ(engine.series(), nullptr);
    EXPECT_EQ(options.series->status(ny(17, 0)).rows_today, 0u);
  }
}
TEST(Series, BackfillIsByteIdenticalOnRepetitionAndFreshRebuildAndRefusesDemo) {
  test::RecordingFile file;
  std::vector<md::Event> events;
  test::SyntheticChain chain;
  for (md::InstrumentId id = 0; id < 82; ++id) {
    const auto* option = chain.book.option(id);
    ASSERT_NE(option, nullptr);
    events.push_back(md::ContractDefinition{id, option->contract});
    events.push_back(md::OptionQuote{id, ny(15, 59), option->bid, option->ask, 10, 10});
    events.push_back(md::OpenInterest{id, ny(15, 59), option->open_interest});
  }
  const std::vector<md::Event> snapshots{
      md::UnderlyingQuote{"SPX", ny(15, 59), 4999, 5001, 5000}, md::SnapshotComplete{"SPX", ny(15, 59)},
      md::UnderlyingQuote{"SPX", ny(15, 59) + md::kNanosPerSecond, 5009, 5011, 5010}, md::SnapshotComplete{"SPX", ny(15, 59) + md::kNanosPerSecond},
      md::UnderlyingQuote{"SPX", ny(16, 0), 5019, 5021, 5020}, md::SnapshotComplete{"SPX", ny(16, 0)}};
  events.insert(events.end(), snapshots.begin(), snapshots.end());
  test::record_events(file.path, events);
  const auto directory = file.directory / "series";
  server::SeriesStore store(directory);
  EXPECT_EQ(server::backfill_series(file.path, store), 2u);
  ASSERT_EQ(store.rows("SPX", 0, ny(17, 0)).size(), 2u);
  EXPECT_TRUE(std::isfinite(store.rows("SPX", 0, ny(17, 0))[1].values[3]));
  const auto bytes = read(directory / "SPX-2026-09.csv");
  EXPECT_EQ(server::backfill_series(file.path, store), 0u);
  EXPECT_EQ(read(directory / "SPX-2026-09.csv"), bytes);
  EXPECT_EQ(server::backfill_series(file.path, store, true), 0u);
  server::SeriesStore rebuild(file.directory / "other");
  EXPECT_EQ(server::backfill_series(file.path, rebuild), 2u);
  EXPECT_EQ(read(file.directory / "other/SPX-2026-09.csv"), bytes);
  auto changed = row(ny(15, 59), 999);
  ASSERT_TRUE(store.insert("SPX", changed, true));
  EXPECT_EQ(server::backfill_series(file.path, store), 0u);
  EXPECT_EQ(server::backfill_series(file.path, store, true), 1u);
  const auto restored = read(directory / "SPX-2026-09.csv");
  EXPECT_EQ(server::backfill_series(file.path, store, true), 0u);
  EXPECT_EQ(read(directory / "SPX-2026-09.csv"), restored);
  const auto demo = file.directory / "demo.oprec";
  for (const auto* provider_name : {"demo", "replay (demo)", "replay (replay (demo))"}) {
    std::filesystem::remove(demo);
    auto header = test::recording_header(); header.provider = provider_name;
    test::record_events(demo, events, header);
    EXPECT_THROW(server::backfill_series(demo, store), std::runtime_error);
  }
}
TEST(SeriesApi, ValidatesRangesFieldsAndIntervalsBeforeReading) {
  SeriesSource source;
  source.store.insert("SPX", row(ny(10, 0)));
  for (const auto* query : {"interval=5m", "fields=missing", "fields=spot,", "fields=", "fields=spot,spot", "from=nan", "from=", "from=-1", "from=999999999999999999", "from=2026-02-30", "from=2026-09-23&to=2026-09-22", "interval=1m&from=2026-09-01&to=2026-09-22", "from=2000-01-01&to=2026-09-22", "interval=1m&fields=rv21", "interval=1d&interval=1m", "limit=4"}) {
    const auto response = server::handle_api({"GET", std::string("/api/underlyings/SPX/series?") + query}, source);
    EXPECT_EQ(response.status, 400) << query << response.body;
  }
  const auto response = server::handle_api({"GET", "/api/underlyings/SPX/series?interval=1m&fields=spot,mfiv30&from=2026-09-22&to=2026-09-22"}, source);
  ASSERT_EQ(response.status, 200);
  const auto body = json::parse(response.body);
  ASSERT_EQ(body["rows"].size(), 1u);
  EXPECT_TRUE(body["rows"][0]["spot"].is_null());
  EXPECT_EQ(body["rows"][0]["mfiv30"], 20);
  EXPECT_EQ(body["rows"][0]["sources"]["mfiv30"], "own");
  EXPECT_FALSE(body["rows"][0].contains("atm30"));
  // Errors take the shape every route uses.
  const auto unknown = server::handle_api({"GET", "/api/underlyings/QQQ/series"}, source);
  EXPECT_EQ(unknown.status, 404);
  EXPECT_EQ(json::parse(unknown.body)["error"]["code"], "NOT_FOUND");
  const auto invalid = server::handle_api({"GET", "/api/underlyings/SPX/series?interval=5m"}, source);
  EXPECT_EQ(invalid.status, 400);
  EXPECT_EQ(json::parse(invalid.body)["error"]["code"], "INVALID_REQUEST");
  const auto unfinished = server::handle_api({"GET", "/api/underlyings/SPX/series?interval=1d&from=2026-09-22&to=2026-09-22"}, source);
  EXPECT_TRUE(json::parse(unfinished.body)["rows"].empty());
}
TEST(SeriesApi, DailyProvenanceStatusAndVolatilityRefreshWithSameSnapshot) {
  SeriesSource source;
  auto metrics = *source.snapshot; metrics.as_of = ny(17, 0);
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(metrics);
  const auto get = [&](std::string target) { return json::parse(server::handle_api({"GET", std::move(target)}, source).body); };
  auto response = get("/api/underlyings/SPX/volatility");
  EXPECT_EQ(response["history_sessions"], 0);
  source.store.insert("SPX", row(ny(15, 59)));
  const auto earlier = ny(9, 30, {2026, 9, 21});
  source.bars.merge_days("VIX", {{earlier, 10, 10, 10, 10}, {ny(9, 30), 99, 99, 99, 99}});
  response = get("/api/underlyings/SPX/volatility");
  EXPECT_EQ(response["history_sessions"], 2);
  EXPECT_EQ(response["proxy"]["used"], true);
  EXPECT_EQ(response["proxy"]["start"], "2026-09-21");
  EXPECT_EQ(response["history_values"][1]["source"], "own_mfiv");
  response = get("/api/underlyings/SPX/series?fields=mfiv30,atm30,proxy_iv30&interval=1d&from=2026-09-21&to=2026-09-22");
  ASSERT_EQ(response["rows"].size(), 2u);
  EXPECT_EQ(response["rows"][0]["proxy_iv30"], 10);
  EXPECT_TRUE(response["rows"][0]["mfiv30"].is_null());
  EXPECT_EQ(response["rows"][1]["mfiv30"], 20);
  EXPECT_TRUE(response["rows"][1]["proxy_iv30"].is_null());
  EXPECT_EQ(response["rows"][1]["sample_time"], ny(15, 59) / md::kNanosPerSecond);
  source.store.report_error("disk full");
  response = get("/api/status");
  EXPECT_EQ(response["series"]["enabled"], true);
  EXPECT_EQ(response["series"]["rows_today"], 1);
  EXPECT_EQ(response["series"]["last_error"], "disk full");
}
TEST(SeriesApi, BoundsTheNumberOfMinuteRows) {
  SeriesSource source;
  const auto first = ny(10, 0);
  for (int i = 0; i <= 10000; ++i) source.store.insert("SPX", row(first + i * md::kNanosPerMinute));
  const auto target = "/api/underlyings/SPX/series?interval=1m&fields=spot&from=" + std::to_string(first / md::kNanosPerSecond) + "&to=";
  EXPECT_EQ(server::handle_api({"GET", target + std::to_string((first + 10000 * md::kNanosPerMinute) / md::kNanosPerSecond)}, source).status, 400);
  EXPECT_EQ(server::handle_api({"GET", target + std::to_string((first + 9999 * md::kNanosPerMinute) / md::kNanosPerSecond)}, source).status, 200);
}
TEST(SeriesApi, SpyExPostLabelsSpxAndVixSourcesAndNeedsCompletedFollowingCloses) {
  SeriesSource source;
  auto metrics = *source.snapshot;
  metrics.symbol = "SPY";
  metrics.as_of = ny(17, 0);
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(metrics);
  std::vector<md::Bar> bars;
  md::Date day{2026, 8, 3};
  for (int i = 0; i < 23; ++i) {
    const double value = 100 + i % 2;
    bars.push_back({md::new_york_to_utc(day, 9, 30), value, value, value, value});
    do { day = md::date_from_days(md::days_since_epoch(day) + 1); }
    while (!md::market_session(md::new_york_to_utc(day, 9, 30)).open);
  }
  source.bars.merge_days("SPX", bars);
  source.bars.merge_days("VIX", {{bars.front().start, 20, 20, 20, 20}, {bars.back().start, 30, 30, 30, 30}});
  const auto response = json::parse(server::handle_api({"GET", "/api/underlyings/SPY/volatility"}, source).body);
  const auto& premium = response["ex_post_vrp"];
  EXPECT_EQ(premium["proxy"], true);
  EXPECT_NE(premium["realized_source"].get<std::string>().find("SPX"), std::string::npos);
  ASSERT_EQ(premium["points"].size(), 1u);
  EXPECT_EQ(premium["points"][0]["source"], "VIX");
  EXPECT_EQ(premium["summaries"][0]["observations"], 1);
}
TEST(SeriesApi, RankUsesFullPrecisionCurrentIvSoTheSameSnapshotIsATie) {
  SeriesSource source;
  auto metrics = *source.snapshot;
  metrics.as_of = ny(16, 0);
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(metrics);
  const auto sample = server::series_row(source.snapshot);
  ASSERT_TRUE(std::isfinite(sample.values[3]));
  source.store.insert("SPX", sample);
  const auto response = json::parse(server::handle_api({"GET", "/api/underlyings/SPX/volatility"}, source).body);
  EXPECT_EQ(response["history_sessions"], 1);
  EXPECT_EQ(response["iv_percentile"], 0);
}
}  // namespace
