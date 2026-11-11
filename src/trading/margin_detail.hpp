#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "margin_optimizer.hpp"
#include "openport/trading/evaluation.hpp"

namespace openport::trading::detail {
/// The exact allocation's work budget per underlying, in units of one simplex
/// dictionary cell visited. Replay drivers 1-12 gave it 50,000,000 units of pivots
/// alone (and the cubic bound of each basis elimination), which let an irregular book
/// run 20-50 ms before it gave up. From driver 13 its exact checks count as they are
/// done, a matrix cell swept one unit and an exact rational operation 400, about what
/// one costs against a dictionary cell; 10,000,000 units are 3-4 ms on a GitHub
/// runner. Running out returns the bounded allocator. bench/bench_margin.cpp
/// measures them (docs/margin-allocation.md).
inline constexpr std::uint64_t kLegacyExactMarginWork = 50'000'000;
inline constexpr std::uint64_t kExactMarginWork = 10'000'000;
inline constexpr std::uint64_t kExactRationalWork = 400;
/// The pre-improvement allocator, retained as an incumbent and a benchmark/test baseline.
Money pairing_margin_requirement(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks = {},
                                 const MarginPolicy& policy = {});

/// Strategy margin already solved, one underlying at a time, by every input the
/// solver reads (margin.cpp, solve_key): the policy, the shares, and each option's
/// contract, the instant it expires under the calendar in force, its quantity, its
/// buy-back value and the spot its naked rule reads. A session keeps one for its
/// account, shared by the copies its transactions, previews and what-ifs make: the
/// same inputs always give the same answer, so a copy with other positions or marks
/// solves its own. It is never journaled. Past its capacity, the answers used least
/// recently make room. Safe to share between threads.
class MarginCache {
 public:
  explicit MarginCache(std::size_t capacity = 256) : capacity_(capacity) {}
  /// The answer kept for `key`, with its parts only when `parts` asks for them.
  [[nodiscard]] std::optional<MarginUnderlying> find(const std::string& key, bool parts);
  void insert(std::string key, MarginUnderlying answer);
  [[nodiscard]] std::size_t size() const;

 private:
  struct Entry {
    MarginUnderlying answer;
    std::uint64_t used = 0;
  };
  mutable std::mutex mutex_;
  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t clock_ = 0;
  std::size_t capacity_;
};
}  // namespace openport::trading::detail
