#include "openport/server/series.hpp"
#include "openport/server/api.hpp"
#include "support/recording.hpp"
#include "support/contract_capture.hpp"
#include "support/synthetic_chain.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
using namespace openport;
using nlohmann::json;
md::Timestamp at(md::Date date, int hour, int minute = 0) { return md::new_york_to_utc(date, hour, minute); }
std::shared_ptr<const analytics::UnderlyingMetrics> snapshot(md::Timestamp time) {
  test::SyntheticChain chain;
  auto metrics = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  metrics.as_of = time;
  return std::make_shared<const analytics::UnderlyingMetrics>(std::move(metrics));
}
class SurfaceSource final : public server::MetricsSource {
 public:
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return publication; }
  server::EngineStatus status() const override { return state; }
  const server::SeriesStore* series() const override { return &store; }
  std::shared_ptr<const analytics::UnderlyingMetrics> publication = snapshot(at({2026, 9, 22}, 16));
  server::SeriesStore store;
  server::EngineStatus state;
};
TEST(SurfaceHistory, SamplesActualTimesAndEarlyClosesAndRejectsSimulatedFeeds) {
  const md::Date early{2026, 11, 27};
  EXPECT_EQ(server::surface_slot(at(early, 12, 59)), "close");
  EXPECT_TRUE(server::surface_slot(at(early, 14)).empty());
  EXPECT_TRUE(server::surface_slot(at({2026, 9, 26}, 10)).empty());
  EXPECT_TRUE(server::surface_slot(at({2026, 9, 22}, 10, 5)).empty());
  SurfaceSource source;
  source.publication = snapshot(at({2026, 9, 22}, 10, 2) + 123);
  source.state.provider = "demo";
  source.state.underlyings["SPX"].state = md::FeedState::Live;
  server::SeriesWorker worker(source, source.store);
  worker.sample();
  EXPECT_TRUE(source.store.surfaces("SPX", 0, at({2026, 9, 23}, 0)).empty());
  source.state.provider = "cboe";
  worker.sample();
  auto values = source.store.surfaces("SPX", 0, at({2026, 9, 23}, 0));
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0].time, source.publication->as_of);
  EXPECT_EQ(values[0].slot, "10:00");
  source.publication = snapshot(at({2026, 9, 22}, 10, 3)); worker.sample();
  EXPECT_EQ(source.store.surfaces("SPX", 0, at({2026, 9, 23}, 0)).size(), 1u);
}
TEST(SurfaceHistory, ReloadsPreciseFitsAndLastCloseAndPreservesTornFiles) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  auto first = server::surface_snapshot(snapshot(at({2026, 9, 21}, 15, 57)));
  auto last = first; last.time = at({2026, 9, 21}, 15, 59) + 123;
  {
    server::SeriesStore store(directory);
    ASSERT_TRUE(store.insert_surface("SPX", first));
    ASSERT_TRUE(store.insert_surface("SPX", last));
    EXPECT_FALSE(store.insert_surface("SPX", first));
    EXPECT_FALSE(store.insert_surface("SPX", last, true));
    EXPECT_TRUE(store.surfaces("SPX", 0, last.time).empty());
  }
  // The close is re-sampled until the bell but persisted once per session.
  std::ifstream lines(directory / "SPX-2026-09.surfaces");
  EXPECT_EQ(std::count(std::istreambuf_iterator<char>(lines), {}, '\n'), 1);
  server::SeriesStore loaded(directory);
  auto values = loaded.surfaces("SPX", 0, at({2026, 9, 22}, 16));
  ASSERT_EQ(values.size(), 1u);
  EXPECT_EQ(values[0].time, last.time);
  ASSERT_EQ(values[0].expiries.size(), last.expiries.size());
  ASSERT_FALSE(values[0].expiries.empty());
  for (std::size_t i = 0; i < values[0].expiries.size(); ++i) {
    const auto& fit = values[0].expiries[i].fit;
    EXPECT_EQ(fit.status, last.expiries[i].fit.status);
    if (fit.status == analytics::SviStatus::Ok) {
      EXPECT_DOUBLE_EQ(fit.parameters.a, last.expiries[i].fit.parameters.a);
      EXPECT_DOUBLE_EQ(fit.years, last.expiries[i].fit.years);
    }
  }
  { std::ofstream out(directory / "SPX-2026-09.surfaces", std::ios::app); out << "{\"schema\":"; }
  server::SeriesStore damaged(directory);
  EXPECT_NE(damaged.status(last.time).last_error.find("torn"), std::string::npos);
  last.time = at({2026, 9, 22}, 16);
  EXPECT_FALSE(damaged.insert_surface("SPX", last));
}
TEST(SurfaceHistory, ComparisonsUseExactTargetSessionsAndNeverFutureCloses) {
  SurfaceSource source;
  for (const auto date : {md::Date{2026, 9, 21}, md::Date{2026, 9, 15}, md::Date{2026, 8, 21}})
    ASSERT_TRUE(source.store.insert_surface("SPX", server::surface_snapshot(snapshot(at(date, 15, 59)))));
  const auto response = server::handle_api({"GET", "/api/underlyings/SPX/surface-history?compare=true"}, source);
  ASSERT_EQ(response.status, 200);
  test::capture_contract("analytics", "GET", "/api/underlyings/SPX/surface-history?compare=true", response);
  const auto body = json::parse(response.body);
  EXPECT_TRUE(body["snapshots"].empty());
  const auto& comparisons = body["comparisons"];
  ASSERT_EQ(comparisons.size(), 3u);
  EXPECT_EQ(comparisons[0]["date"], "2026-09-21");
  EXPECT_EQ(comparisons[1]["date"], "2026-09-15");
  EXPECT_EQ(comparisons[2]["date"], "2026-08-21");
  for (const auto& comparison : comparisons) EXPECT_FALSE(comparison["snapshot"].is_null());
  source.publication = snapshot(at({2026, 9, 23}, 10));
  const auto missing = json::parse(server::handle_api({"GET", "/api/underlyings/SPX/surface-history"}, source).body);
  EXPECT_TRUE(missing["comparisons"][0]["snapshot"].is_null());
  for (const auto* query : {"compare=1", "from=bad", "to=2026-02-30", "from=2024-01-01", "from=2027-01-01", "unknown=1", "compare=true&compare=false"})
    EXPECT_EQ(server::handle_api({"GET", std::string("/api/underlyings/SPX/surface-history?") + query}, source).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings/NOPE/surface-history"}, source).status, 404);
}
TEST(SurfaceHistory, CloseRewritesKeepOtherSlotsAndOneLinePerSession) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  const auto base = server::surface_snapshot(snapshot(at({2026, 9, 21}, 10, 1)));
  auto morning = base, close = base;
  close.slot = "close";
  {
    server::SeriesStore store(directory);
    ASSERT_TRUE(store.insert_surface("SPX", morning));
    for (int minute = 55; minute < 60; ++minute) {
      close.time = at({2026, 9, 21}, 15, minute);
      ASSERT_TRUE(store.insert_surface("SPX", close));
    }
    auto forced = morning;
    forced.spot += 1;
    ASSERT_TRUE(store.insert_surface("SPX", forced, true));
  }
  std::ifstream input(directory / "SPX-2026-09.surfaces");
  std::vector<json> rows;
  for (std::string line; std::getline(input, line);) rows.push_back(json::parse(line));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0]["slot"], "close");
  EXPECT_EQ(rows[0]["market_ns"], at({2026, 9, 21}, 15, 59));
  EXPECT_EQ(rows[1]["slot"], "10:00");
  EXPECT_EQ(rows[1]["spot"], morning.spot + 1);
  server::SeriesStore loaded(directory);
  EXPECT_EQ(loaded.surfaces("SPX", 0, at({2026, 9, 22}, 0)).size(), 2u);
}
TEST(SurfaceHistory, OldMonthsAreNotLoadedAndOldSnapshotsAreEvicted) {
  test::RecordingFile temp;
  const auto directory = temp.directory / "series";
  const auto base = server::surface_snapshot(snapshot(at({2026, 9, 21}, 10, 1)));
  const auto on = [&](md::Date date) { auto out = base; out.time = at(date, 10, 1); return out; };
  {
    server::SeriesStore store(directory);
    ASSERT_TRUE(store.insert_surface("SPX", on({2025, 7, 1})));
    ASSERT_TRUE(store.insert_surface("SPX", on({2025, 9, 2})));
    ASSERT_TRUE(store.insert_surface("SPX", on({2026, 9, 21})));
    const auto kept = store.surfaces("SPX", 0, at({2026, 9, 22}, 0));
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0].time, at({2025, 9, 2}, 10, 1));
    EXPECT_FALSE(store.insert_surface("SPX", on({2025, 6, 30})));
    EXPECT_TRUE(store.status(0).last_error.empty());
  }
  ASSERT_TRUE(std::filesystem::exists(directory / "SPX-2025-07.surfaces"));
  // A month outside the load window is never read, so even a damaged one is ignored.
  { std::ofstream out(directory / "SPX-2025-07.surfaces", std::ios::app); out << "{\"schema\":"; }
  server::SeriesStore loaded(directory);
  EXPECT_TRUE(loaded.status(0).last_error.empty());
  EXPECT_EQ(loaded.surfaces("SPX", 0, at({2026, 9, 22}, 0)).size(), 2u);
}
TEST(SurfaceHistory, RangeListsAreCappedToTheNewestSnapshots) {
  SurfaceSource source;
  const auto base = server::surface_snapshot(snapshot(at({2026, 9, 21}, 10, 1)));
  std::size_t inserted = 0;
  for (auto date = md::Date{2026, 9, 22}; inserted <= server::kSurfaceHistoryLimit; date = md::previous_business_day(date))
    for (int hour : {10, 12, 14}) {
      auto row = base; row.time = at(date, hour, 1); row.slot = std::to_string(hour) + ":00";
      ASSERT_TRUE(source.store.insert_surface("SPX", row));
      ++inserted;
    }
  auto body = json::parse(server::handle_api({"GET", "/api/underlyings/SPX/surface-history?from=2026-01-02"}, source).body);
  ASSERT_EQ(body["snapshots"].size(), server::kSurfaceHistoryLimit);
  EXPECT_EQ(body["truncated"], true);
  EXPECT_EQ(body["limit"], server::kSurfaceHistoryLimit);
  EXPECT_EQ(body["snapshots"].back()["as_of"], md::format_timestamp(at({2026, 9, 22}, 14, 1)));
  body = json::parse(server::handle_api({"GET", "/api/underlyings/SPX/surface-history?from=2026-09-01"}, source).body);
  EXPECT_EQ(body["truncated"], false);
  EXPECT_LT(body["snapshots"].size(), server::kSurfaceHistoryLimit);
}
}  // namespace
