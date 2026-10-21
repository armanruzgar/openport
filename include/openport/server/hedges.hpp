#pragma once

#include <optional>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"

namespace openport::server {

/// One constraint on a hedge of q units: base + q * buy >= 0 when buying (q > 0),
/// base + q * sell >= 0 when selling (q < 0). The slopes differ by what crossing
/// the spread costs on each side.
struct HedgeConstraint {
  double base = 0;
  double buy = 0;
  double sell = 0;
};
/// The smallest whole hedge, by size, that keeps every constraint: zero when they
/// already hold, buying on a tie, nullopt when no size up to `max_units` does.
[[nodiscard]] std::optional<long long> smallest_hedge(const std::vector<HedgeConstraint>& constraints, long long max_units);

/// The held book's exposure in one risk bucket: dollar delta and vega across what
/// its open orders can reach, and its P&L in each cell of the rules' scenario grid.
struct HedgeBook {
  double delta_low = 0, delta_high = 0, vega_low = 0, vega_high = 0;
  std::vector<double> cells;
};
/// One unit of a hedge (a share or an option contract): its exposure, its P&L in
/// each grid cell, and what crossing the spread costs to buy or sell it, in dollars.
struct HedgeUnit {
  double delta = 0, vega = 0;
  std::vector<double> cells;
  double buy_cost = 0, sell_cost = 0;
};
/// What the hedge must achieve: dollar delta and vega within their limits, or every
/// grid cell's P&L at or above `grid_floor` (a negative number: the worst loss allowed).
struct HedgeGoal {
  std::optional<double> delta_limit, vega_limit, grid_floor;
};
[[nodiscard]] std::vector<HedgeConstraint> hedge_constraints(const HedgeBook& book, const HedgeUnit& unit, const HedgeGoal& goal);

/// The book after `units` of the hedge, its spread cost included in the grid.
struct HedgeResidual {
  double delta_low = 0, delta_high = 0, vega_low = 0, vega_high = 0;
  std::optional<double> worst_cell;
};
[[nodiscard]] HedgeResidual hedged(const HedgeBook& book, const HedgeUnit& unit, long long units);

/// The listed option a hedge trades at the money: in the expiry nearest 30 calendar
/// days that has more than a day to run, the strike nearest the forward (the spot
/// without a parity forward) with a tradable two-sided quote of `type`. Null when
/// none is listed.
[[nodiscard]] const analytics::OptionMetrics* atm_option(const analytics::UnderlyingMetrics& metrics, pricing::OptionType type);

}  // namespace openport::server
