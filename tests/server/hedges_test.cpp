#include "openport/server/hedges.hpp"

#include <gtest/gtest.h>

namespace {
using namespace openport;
using server::HedgeBook;
using server::HedgeGoal;
using server::HedgeUnit;

long long solve(const HedgeBook& book, const HedgeUnit& unit, const HedgeGoal& goal, long long most = 1'000'000) {
  const auto units = server::smallest_hedge(server::hedge_constraints(book, unit, goal), most);
  EXPECT_TRUE(units.has_value());
  return units.value_or(0);
}

// $250k of long delta against a $100k limit: 1,500 shares at $100 is the least that
// brings it inside, and selling the 0.5-delta call needs 3 contracts.
TEST(Hedges, TheSmallestWholeHedgeBringsDeltaInsideItsLimit) {
  const HedgeBook book{250'000, 250'000, 0, 0, {}};
  const HedgeGoal goal{100'000, std::nullopt, std::nullopt};
  EXPECT_EQ(solve(book, {100, 0, {}, 0, 0}, goal), -1500);
  EXPECT_EQ(solve(book, {100 * 0.5 * 1000, 0, {}, 0, 0}, goal), -3);
  // A put's negative delta is bought.
  EXPECT_EQ(solve(book, {100 * -0.4 * 1000, 0, {}, 0, 0}, goal), 4);
  // Open orders that could add $50k more must also stay inside.
  EXPECT_EQ(solve({250'000, 300'000, 0, 0, {}}, {100, 0, {}, 0, 0}, goal), -2000);
  // Already inside: no hedge.
  EXPECT_EQ(solve({50'000, 50'000, 0, 0, {}}, {100, 0, {}, 0, 0}, goal), 0);
  // Shares cannot move vega, and a range wider than twice the limit cannot fit.
  EXPECT_FALSE(server::smallest_hedge(server::hedge_constraints({0, 0, 900, 900, {}}, {100, 0, {}, 0, 0}, {std::nullopt, 500, std::nullopt}), 1000));
  EXPECT_FALSE(server::smallest_hedge(server::hedge_constraints({-150'000, 150'000, 0, 0, {}}, {100, 0, {}, 0, 0}, goal), 1'000'000));
  // A size beyond the cap is refused rather than clipped.
  EXPECT_FALSE(server::smallest_hedge(server::hedge_constraints(book, {100, 0, {}, 0, 0}, goal), 1000));
}

TEST(Hedges, VegaHedgesSellOptionsAgainstLongVega) {
  const HedgeBook book{0, 0, 1200, 1200, {}};
  EXPECT_EQ(solve(book, {5000, 25, {}, 0, 0}, {std::nullopt, 1000, std::nullopt}), -8);
  const auto after = server::hedged(book, {5000, 25, {}, 0, 0}, -8);
  EXPECT_DOUBLE_EQ(after.vega_high, 1000);
  EXPECT_DOUBLE_EQ(after.delta_low, -40'000);
}

// Each cell is linear in the hedge, less the spread paid: the worst cell rises to the
// floor at the first size where every cell clears it.
TEST(Hedges, GridHedgesLiftTheWorstCellAboveTheFloorAfterSpreadCost) {
  const HedgeBook book{0, 0, 0, 0, {-3000, -1000, 0, 800, 1500}};
  // A put that gains in the down cells and loses its premium in the up ones.
  const HedgeUnit put{0, 0, {900, 400, -50, -150, -200}, 10, 10};
  const HedgeGoal goal{std::nullopt, std::nullopt, -1500};
  const auto units = solve(book, put, goal);
  EXPECT_EQ(units, 2);
  const auto after = server::hedged(book, put, units);
  ASSERT_TRUE(after.worst_cell);
  EXPECT_GE(*after.worst_cell, -1500);
  EXPECT_LT(server::hedged(book, put, 1).worst_cell.value(), -1500);
  // Too many puts sink the up cells: a floor they can never all meet has no hedge.
  EXPECT_FALSE(server::smallest_hedge(server::hedge_constraints(book, put, {std::nullopt, std::nullopt, -100}), 1000));
  // A tie in size prefers buying.
  EXPECT_EQ(server::smallest_hedge({{-1, 1, -1}}, 10), 1);
}

TEST(Hedges, AtTheMoneyMeansNearestThirtyDaysAndTheForward) {
  analytics::UnderlyingMetrics metrics;
  metrics.symbol = "SPY";
  const auto slice = [](const char* expiry, double years, double forward) {
    analytics::SliceMetrics s;
    s.years = years;
    s.forward.forward = forward;
    s.forward.ok = true;
    for (int k = 490; k <= 510; k += 5) {
      analytics::StrikeMetrics row;
      row.strike = k;
      char symbol[32];
      std::snprintf(symbol, sizeof symbol, "SPY%sC%08d", expiry, k * 1000);
      row.call.contract = *md::parse_osi(symbol);
      row.call.id = static_cast<md::InstrumentId>(k);
      row.call.bid = 4;
      row.call.ask = 4.1;
      s.strikes.push_back(row);
    }
    return s;
  };
  metrics.slices = {slice("261002", 10 / 365.0, 500), slice("261023", 31 / 365.0, 502), slice("261120", 59 / 365.0, 504)};
  const auto* option = server::atm_option(metrics, pricing::OptionType::Call);
  ASSERT_NE(option, nullptr);
  EXPECT_EQ(option->contract.osi_symbol(), "SPY   261023C00500000");
  EXPECT_EQ(server::atm_option(metrics, pricing::OptionType::Put), nullptr);
}
}  // namespace
