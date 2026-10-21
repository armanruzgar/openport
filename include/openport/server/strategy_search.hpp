#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"

namespace openport::server {

/// What a strategy search looks for: candidates on listed expiries, valued at a
/// target date under the market's risk-neutral distribution and under the user's,
/// which is centred on a target price with an optional volatility change.
struct StrategySearchRequest {
  double target_price = 0;
  md::Date target_date;                  ///< Valued at this date's regular close, or a leg's earlier expiry.
  bool dated = false;                    ///< false: the first chosen expiry's date.
  std::vector<std::string> expiries;     ///< Expiry ids; empty chooses the first four on or after the date.
  std::vector<std::string> structures{"vertical", "condor", "butterfly", "calendar", "single", "covered"};
  double budget = 0;                     ///< Largest loss one candidate's quantity may risk, in dollars.
  double vol_points = 0;                 ///< The user's volatility change, in vol points.
  std::string rank = "expected";         ///< expected, risk_neutral, pop or return_on_risk.
  bool mid = false;                      ///< Enter at mids instead of the far sides.
  std::size_t limit = 20;                ///< Candidates returned, 1-100.
  std::size_t max_candidates = 5000;     ///< Candidates valued, 1-25000.
};

inline constexpr std::size_t kMaxSearchCandidates = 25'000;
inline constexpr std::size_t kMaxSearchExpiries = 4;
/// Strikes per expiry considered, nearest the spot and the target first.
inline constexpr std::size_t kMaxSearchStrikes = 40;

/// The query of GET /api/underlyings/{symbol}/optimizer. Throws std::invalid_argument
/// naming the parameter that is missing or out of range.
[[nodiscard]] StrategySearchRequest parse_strategy_search(const std::map<std::string, std::string>& query);

/// Searches verticals, short iron condors, long butterflies, long calendars,
/// single options and covered calls within the budget, best first by `rank`.
/// Each candidate is valued per unit at the evaluation time, the target date's
/// close or its first leg's expiry if earlier, with risk_profile's Black-76 model:
/// smile IV (plus the user's change), remaining life, carry and discounting
/// shrinking with it. Distributions are lognormal on the at-the-money implied
/// variance to that time (implied_variance_until): forward at spot for the
/// risk-neutral one, mean at the target price for the user's. A cancelled search
/// returns what it valued so far, marked truncated.
[[nodiscard]] nlohmann::json search_strategies(const analytics::UnderlyingMetrics& metrics,
    const StrategySearchRequest& request, const std::atomic_bool* cancel = nullptr);

/// A small pool for searches, so HTTP I/O threads never wait for one. `submit`
/// returns false when every worker is busy and the queue is full.
class StrategySearchPool {
 public:
  StrategySearchPool(std::size_t workers, std::size_t queue);
  ~StrategySearchPool();
  StrategySearchPool(const StrategySearchPool&) = delete;
  StrategySearchPool& operator=(const StrategySearchPool&) = delete;
  bool submit(std::function<void()> job);
  /// The process's pool: two workers and four queued searches.
  static StrategySearchPool& shared();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::server
