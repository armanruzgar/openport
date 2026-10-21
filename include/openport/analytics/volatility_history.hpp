#pragma once

#include "openport/analytics/realized.hpp"

namespace openport::analytics {
struct VolatilityDay {
  md::Date date;
  double mfiv = kNaN;
  double atm = kNaN;
};
struct HistoricalIv {
  md::Date date;
  double vol = kNaN;
  std::string source;  ///< own_mfiv, own_atm, or the named Cboe proxy.
};
[[nodiscard]] std::string iv_proxy(std::string_view symbol);
/// Proxies only precede the first local row. Local gaps are not silently filled.
[[nodiscard]] std::vector<HistoricalIv> historical_iv(std::span<const VolatilityDay> own,
    std::span<const md::Bar> proxy, std::string_view proxy_name, md::Timestamp as_of, bool atm_fallback = true);
struct HistoricalValue {
  md::Date date;
  double value = kNaN;
  std::string source = "own";
};
struct HistoryRank {
  double rank = kNaN;
  double percentile = kNaN;  ///< Fraction strictly below current.
  double z_score = kNaN;  ///< Population standard deviation; null for a flat window.
  std::size_t sessions = 0;
  std::size_t own_sessions = 0;
  std::size_t proxy_sessions = 0;
  std::string proxy_start;
};
using IvRank = HistoryRank;
/// Any finite stored value, including negative skew. One observation per completed session.
[[nodiscard]] HistoryRank history_rank(std::span<const HistoricalValue> history, double current, md::Timestamp as_of);
/// The last 252 scheduled completed sessions, including missing days in the window.
[[nodiscard]] IvRank iv_rank(std::span<const HistoricalIv> history, double current, md::Timestamp as_of);
struct ExPostPoint {
  md::Date date;
  md::Date end;
  double implied = kNaN;
  double realized = kNaN;
  double variance = kNaN;
  double vol_points = kNaN;
  std::string source;
};
struct ExPostSummary {
  int years = 0;
  std::size_t observations = 0;
  std::size_t proxy_observations = 0;
  double mean_variance = kNaN, median_variance = kNaN;
  double mean_vol_points = kNaN, median_vol_points = kNaN;
  double positive_share = kNaN;
  std::string from, to;
};
struct ExPostVrp {
  std::vector<ExPostPoint> points;
  std::vector<ExPostSummary> summaries;
};
/// Each starting close requires the following 21 consecutive completed sessions.
[[nodiscard]] ExPostVrp ex_post_vrp(std::span<const HistoricalIv> history,
    std::span<const md::Bar> days, md::Timestamp as_of);
}  // namespace openport::analytics
