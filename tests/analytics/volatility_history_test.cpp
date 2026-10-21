#include "openport/analytics/volatility_history.hpp"
#include <gtest/gtest.h>
#include <cmath>

namespace {
using namespace openport;
md::Timestamp close(md::Date day) { return md::new_york_to_utc(day, md::regular_close_hour(day), 0); }
md::Bar daily(md::Date day, double value) { return {md::new_york_to_utc(day, 9, 30), value, value, value, value}; }

TEST(VolatilityHistory, OwnRowsWinAndProxiesOnlyPrecedeLocalHistory) {
  const std::vector<analytics::VolatilityDay> own{{{2026, 9, 22}, 20, 50}, {{2026, 9, 24}, analytics::kNaN, 30},
      {{2026, 9, 25}, analytics::kNaN, analytics::kNaN}};
  const std::vector<md::Bar> proxy{daily({2026, 9, 21}, 10), daily({2026, 9, 22}, 99), daily({2026, 9, 23}, 99), daily({2026, 9, 25}, 99)};
  const auto values = analytics::historical_iv(own, proxy, "VIX", close({2026, 9, 25}));
  ASSERT_EQ(values.size(), 3u);
  EXPECT_EQ(values[0].source, "VIX");
  EXPECT_EQ(values[1].source, "own_mfiv");
  EXPECT_DOUBLE_EQ(values[1].vol, 20);
  EXPECT_EQ(values[2].source, "own_atm");
  auto rank = analytics::iv_rank(values, 20, close({2026, 9, 25}));
  EXPECT_EQ(rank.sessions, 3u);
  EXPECT_EQ(rank.own_sessions, 2u);
  EXPECT_EQ(rank.proxy_sessions, 1u);
  EXPECT_EQ(rank.proxy_start, "2026-09-21");
  EXPECT_DOUBLE_EQ(rank.rank, 0.5);
  EXPECT_DOUBLE_EQ(rank.percentile, 1.0 / 3);
  EXPECT_EQ(analytics::historical_iv(own, proxy, "VIX", close({2026, 9, 25}), false).size(), 2u);
  EXPECT_EQ(analytics::historical_iv(own, {}, "", close({2026, 9, 25})).size(), 2u);
}
TEST(VolatilityHistory, FixedSessionWindowDoesNotExtendAcrossGapsOrIncludeFuture) {
  const std::vector<analytics::HistoricalIv> values{{{2024, 1, 2}, 90, "VIX"}, {{2026, 9, 22}, 20, "own_mfiv"},
      {{2026, 9, 23}, 30, "own_mfiv"}};
  const auto rank = analytics::iv_rank(values, 20, close({2026, 9, 23}) - md::kNanosPerSecond);
  EXPECT_EQ(rank.sessions, 1u);
  EXPECT_TRUE(std::isnan(rank.rank));
  EXPECT_EQ(rank.percentile, 0);
  EXPECT_TRUE(std::isnan(analytics::iv_rank({}, 20, close({2026, 9, 23})).percentile));
  EXPECT_TRUE(std::isnan(analytics::iv_rank(values, analytics::kNaN, close({2026, 9, 23})).rank));
}
TEST(VolatilityHistory, RankIsNotClampedAndTiesAreStrictlyBelow) {
  const std::vector<analytics::HistoricalIv> values{{{2026, 9, 21}, 10, "VIX"}, {{2026, 9, 22}, 20, "VIX"}};
  EXPECT_EQ(analytics::iv_rank(values, 30, close({2026, 9, 23})).rank, 2);
  EXPECT_EQ(analytics::iv_rank(values, 20, close({2026, 9, 23})).percentile, .5);
  EXPECT_EQ(analytics::iv_rank(values, 5, close({2026, 9, 23})).rank, -.5);
  EXPECT_EQ(analytics::iv_proxy("SPY"), "VIX");
  EXPECT_EQ(analytics::iv_proxy("QQQ"), "VXN");
  EXPECT_EQ(analytics::iv_proxy("IWM"), "RVX");
  EXPECT_EQ(analytics::iv_proxy("DIA"), "VXD");
  EXPECT_TRUE(analytics::iv_proxy("AAPL").empty());
}
TEST(VolatilityHistory, ExPostArithmeticUsesFollowingClosesAndExcludesUnfinishedMonths) {
  std::vector<md::Bar> bars;
  md::Date date{2026, 8, 3};
  double value = 100;
  for (int i = 0; i < 22; ++i) {
    if (i > 0) value *= std::exp(i % 2 == 1 ? .01 : -.01);
    bars.push_back(daily(date, value));
    do { date = md::date_from_days(md::days_since_epoch(date) + 1); }
    while (!md::market_session(md::new_york_to_utc(date, 9, 30)).open);
  }
  const auto end = md::new_york_time(bars.back().start).date;
  const std::vector<analytics::HistoricalIv> implied{{{2026, 8, 3}, 20, "own_mfiv"}, {{2026, 8, 4}, 30, "VIX"}};
  auto result = analytics::ex_post_vrp(implied, bars, close(end));
  ASSERT_EQ(result.points.size(), 1u);
  // Eleven +1% and ten -1% log returns: sample variance = (21*.0001 - .01²/21)/20.
  const double rv_variance = 252 * (21 * .0001 - .0001 / 21) / 20;
  EXPECT_NEAR(result.points[0].variance, .04 - rv_variance, 1e-12);
  EXPECT_NEAR(result.points[0].vol_points, 20 - 100 * std::sqrt(rv_variance), 1e-12);
  ASSERT_EQ(result.summaries.size(), 3u);
  for (const auto& summary : result.summaries) {
    EXPECT_EQ(summary.observations, 1u);
    EXPECT_EQ(summary.positive_share, 1);
    EXPECT_EQ(summary.mean_variance, result.points[0].variance);
    EXPECT_EQ(summary.median_variance, result.points[0].variance);
    EXPECT_EQ(summary.proxy_observations, 0u);
  }
  EXPECT_TRUE(analytics::ex_post_vrp(implied, bars, close(end) - md::kNanosPerSecond).points.empty());
  bars.erase(bars.begin() + 5);
  bars.push_back(daily(date, value));
  EXPECT_TRUE(analytics::ex_post_vrp(implied, bars, close(date)).points.empty());
}
TEST(VolatilityHistory, SummaryMeansMediansAndPositiveSharesUseAllCompletedWindows) {
  std::vector<md::Bar> bars;
  std::vector<analytics::HistoricalIv> implied;
  md::Date date{2026, 8, 3};
  double value = 100;
  for (int i = 0; i < 24; ++i) {
    if (i > 0) value *= std::exp(i % 2 == 1 ? .01 : -.01);
    bars.push_back(daily(date, value));
    if (i < 3) implied.push_back({date, i == 0 ? 0.0 : i == 1 ? 20.0 : 30.0, i == 0 ? "own_mfiv" : "VIX"});
    do { date = md::date_from_days(md::days_since_epoch(date) + 1); }
    while (!md::market_session(md::new_york_to_utc(date, 9, 30)).open);
  }
  const auto result = analytics::ex_post_vrp(implied, bars, close(date));
  ASSERT_EQ(result.points.size(), 3u);
  const double rv_variance = 252 * (21 * .0001 - .0001 / 21) / 20;
  for (const auto& summary : result.summaries) {
    EXPECT_NEAR(summary.mean_variance, .13 / 3 - rv_variance, 1e-12);
    EXPECT_NEAR(summary.median_variance, .04 - rv_variance, 1e-12);
    EXPECT_NEAR(summary.mean_vol_points, 50.0 / 3 - 100 * std::sqrt(rv_variance), 1e-12);
    EXPECT_NEAR(summary.median_vol_points, 20 - 100 * std::sqrt(rv_variance), 1e-12);
    EXPECT_EQ(summary.positive_share, 2.0 / 3);
    EXPECT_EQ(summary.proxy_observations, 2u);
  }
}
TEST(VolatilityHistory, RankUsesExactly252CompletedSessionSlots) {
  std::vector<analytics::HistoricalIv> values;
  md::Date date{2026, 9, 22};
  for (int i = 0; i < 260; ++i) {
    values.push_back({date, static_cast<double>(i), "own_mfiv"});
    date = md::previous_business_day(date);
  }
  const auto rank = analytics::iv_rank(values, 125.5, close({2026, 9, 22}));
  EXPECT_EQ(rank.sessions, 252u);
  EXPECT_EQ(rank.rank, .5);
  EXPECT_EQ(rank.percentile, .5);
}
TEST(VolatilityHistory, SignedSeriesRanksUseCompletedUniqueSessionsAndPopulationDeviation) {
  const std::vector<analytics::HistoricalValue> values{{{2026, 9, 21}, -9, "own"},
      {{2026, 9, 21}, -4, "own"}, {{2026, 9, 22}, -2, "own"},
      {{2026, 9, 23}, 8, "own"}, {{2026, 9, 20}, 500, "own"},
      {{2026, 9, 18}, analytics::kNaN, "own"}};
  const auto rank = analytics::history_rank(values, -2, close({2026, 9, 23}) - 1);
  EXPECT_EQ(rank.sessions, 2u);
  EXPECT_EQ(rank.own_sessions, 2u);
  EXPECT_EQ(rank.proxy_sessions, 0u);
  EXPECT_DOUBLE_EQ(rank.rank, 1);
  EXPECT_DOUBLE_EQ(rank.percentile, .5);
  EXPECT_DOUBLE_EQ(rank.z_score, 1);
  EXPECT_TRUE(std::isnan(analytics::history_rank({}, -2, close({2026, 9, 23})).z_score));
  EXPECT_TRUE(std::isnan(analytics::history_rank(std::span(values).subspan(2, 1), -2, close({2026, 9, 23})).z_score));
}
TEST(VolatilityHistory, FlatFractionalSeriesHasNoZScoreAndMissingCorrectionsStayMissing) {
  std::vector<analytics::HistoricalValue> values{{{2026, 9, 21}, .1, "own"},
      {{2026, 9, 22}, .1, "own"}, {{2026, 9, 23}, .1, "own"}};
  const auto flat = analytics::history_rank(values, .1, close({2026, 9, 23}));
  EXPECT_TRUE(std::isnan(flat.rank));
  EXPECT_TRUE(std::isnan(flat.z_score));
  EXPECT_EQ(flat.percentile, 0);
  values.push_back({{2026, 9, 23}, analytics::kNaN, "own"});
  EXPECT_EQ(analytics::history_rank(values, .1, close({2026, 9, 23})).sessions, 2u);
}
}  // namespace
