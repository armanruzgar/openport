#include "openport/server/hedges.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "openport/trading/types.hpp"

namespace openport::server {
namespace {
constexpr double kTolerance = 1e-9;
/// The smallest whole size along one direction: constraints base + r * slope >= 0, r >= 1.
std::optional<long long> along(const std::vector<HedgeConstraint>& constraints, bool buying, long long max_units) {
  double low = 1, high = static_cast<double>(max_units);
  for (const auto& c : constraints) {
    const double slope = buying ? c.buy : -c.sell;
    if (std::abs(slope) <= kTolerance) {
      if (c.base < -kTolerance) return std::nullopt;
      continue;
    }
    const double bound = -c.base / slope;
    if (slope > 0) low = std::max(low, bound);
    else high = std::min(high, bound);
  }
  const double size = std::ceil(low - kTolerance);
  if (!std::isfinite(size) || size > high + kTolerance || size > static_cast<double>(max_units)) return std::nullopt;
  return static_cast<long long>(size);
}
}  // namespace

std::optional<long long> smallest_hedge(const std::vector<HedgeConstraint>& constraints, long long max_units) {
  if (std::all_of(constraints.begin(), constraints.end(), [](const auto& c) { return c.base >= -kTolerance; })) return 0;
  const auto buy = along(constraints, true, max_units), sell = along(constraints, false, max_units);
  if (buy && (!sell || *buy <= *sell)) return *buy;
  if (sell) return -*sell;
  return std::nullopt;
}

std::vector<HedgeConstraint> hedge_constraints(const HedgeBook& book, const HedgeUnit& unit, const HedgeGoal& goal) {
  std::vector<HedgeConstraint> result;
  // |exposure| <= limit at both ends of what open orders can reach.
  const auto within = [&](double low, double high, double slope, double limit) {
    result.push_back({low + limit, slope, slope});
    result.push_back({limit - high, -slope, -slope});
  };
  if (goal.delta_limit) within(book.delta_low, book.delta_high, unit.delta, *goal.delta_limit);
  if (goal.vega_limit) within(book.vega_low, book.vega_high, unit.vega, *goal.vega_limit);
  if (goal.grid_floor)
    for (std::size_t i = 0; i < book.cells.size() && i < unit.cells.size(); ++i)
      result.push_back({book.cells[i] - *goal.grid_floor, unit.cells[i] - unit.buy_cost, unit.cells[i] + unit.sell_cost});
  return result;
}

HedgeResidual hedged(const HedgeBook& book, const HedgeUnit& unit, long long units) {
  const double q = static_cast<double>(units);
  HedgeResidual result{book.delta_low + q * unit.delta, book.delta_high + q * unit.delta,
                       book.vega_low + q * unit.vega, book.vega_high + q * unit.vega, std::nullopt};
  const double cost = std::abs(q) * (units > 0 ? unit.buy_cost : unit.sell_cost);
  for (std::size_t i = 0; i < book.cells.size() && i < unit.cells.size(); ++i) {
    const double cell = book.cells[i] + q * unit.cells[i] - cost;
    result.worst_cell = result.worst_cell ? std::min(*result.worst_cell, cell) : cell;
  }
  return result;
}

const analytics::OptionMetrics* atm_option(const analytics::UnderlyingMetrics& metrics, pricing::OptionType type) {
  const analytics::OptionMetrics* best = nullptr;
  double best_days = std::numeric_limits<double>::infinity(), best_distance = 0;
  for (const auto& slice : metrics.slices) {
    const double days = slice.years * 365;
    // Without a parity forward the spot stands in for the money.
    const double money = slice.forward.ok && slice.forward.forward > 0 ? slice.forward.forward : metrics.spot;
    if (!(days > 1) || !(money > 0) || !std::isfinite(money)) continue;
    const analytics::OptionMetrics* pick = nullptr;
    double distance = std::numeric_limits<double>::infinity();
    for (const auto& row : slice.strikes) {
      const auto& option = type == pricing::OptionType::Call ? row.call : row.put;
      if (option.id == analytics::kNoInstrument || !trading::eligible(option.contract).ok()) continue;
      if (!std::isfinite(option.bid) || !std::isfinite(option.ask) || !(option.bid > 0) || option.ask < option.bid) continue;
      const double gap = std::abs(row.strike - money);
      if (gap < distance) { distance = gap; pick = &option; }
    }
    if (!pick) continue;
    const double off = std::abs(days - 30);
    if (!best || off < std::abs(best_days - 30) || (off == std::abs(best_days - 30) && distance < best_distance)) {
      best = pick; best_days = days; best_distance = distance;
    }
  }
  return best;
}

}  // namespace openport::server
