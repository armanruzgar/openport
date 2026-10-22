#include "openport/pricing/american.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>

#include "openport/pricing/normal.hpp"

namespace {

using openport::pricing::AmericanModel;
using openport::pricing::barone_adesi_whaley_price;
using openport::pricing::binomial_early_exercise_premium;
using openport::pricing::binomial_price;
using openport::pricing::bivariate_norm_cdf;
using openport::pricing::bjerksund_stensland_price;
using openport::pricing::BsmInputs;
using openport::pricing::bsm_price;
using openport::pricing::early_exercise_premium;
using openport::pricing::ExerciseStyle;
using openport::pricing::norm_cdf;
using openport::pricing::OptionType;
using openport::pricing::TreeMethod;

constexpr auto kCall = OptionType::Call;
constexpr auto kPut = OptionType::Put;

TEST(BivariateNormal, MatchesClosedFormsAndLimits) {
  for (double rho : {-0.99, -0.95, -0.9, -0.5, -0.2, 0.0, 0.2, 0.5, 0.9, 0.95, 0.99}) {
    // P(X <= 0, Y <= 0) = 1/4 + asin(rho) / (2 pi).
    EXPECT_NEAR(bivariate_norm_cdf(0, 0, rho), 0.25 + std::asin(rho) / (2 * std::numbers::pi), 1e-14) << rho;
    for (double x : {-3.0, -1.0, 0.3, 2.0})
      for (double y : {-2.0, 0.0, 1.5}) {
        // Symmetry and the complement identity P(X<=x, Y<=y) + P(X<=x, Y>y) = N(x).
        EXPECT_NEAR(bivariate_norm_cdf(x, y, rho), bivariate_norm_cdf(y, x, rho), 1e-14);
        EXPECT_NEAR(bivariate_norm_cdf(x, y, rho) + bivariate_norm_cdf(x, -y, -rho), norm_cdf(x), 1e-14);
      }
  }
  EXPECT_NEAR(bivariate_norm_cdf(0.7, -0.4, 0), norm_cdf(0.7) * norm_cdf(-0.4), 1e-15);
  EXPECT_NEAR(bivariate_norm_cdf(0.7, -0.4, 1), norm_cdf(-0.4), 1e-15);
  EXPECT_NEAR(bivariate_norm_cdf(0.7, -0.4, -1), norm_cdf(0.7) - norm_cdf(0.4), 1e-15);
  EXPECT_EQ(bivariate_norm_cdf(-0.4, 0.3, -1), 0.0);
  EXPECT_TRUE(std::isnan(bivariate_norm_cdf(0, 0, 1.01)));
  EXPECT_TRUE(std::isnan(bivariate_norm_cdf(std::numeric_limits<double>::quiet_NaN(), 0, 0.5)));
}

TEST(BivariateNormal, AgreesWithNumericalIntegration) {
  // P(X <= x, Y <= y) = integral of phi(t) N((y - rho t) / sqrt(1 - rho^2)) up to x.
  for (double rho : {-0.97, -0.6, 0.35, 0.8, 0.96})
    for (double x : {-1.7, 0.4, 2.2})
      for (double y : {-0.8, 1.1}) {
        const int n = 20000;
        const double lo = -9, h = (x - lo) / n;
        double sum = 0;
        for (int i = 0; i <= n; ++i) {
          const double t = lo + h * i;
          const double f = std::exp(-t * t / 2) / std::sqrt(2 * std::numbers::pi) *
                           norm_cdf((y - rho * t) / std::sqrt(1 - rho * rho));
          sum += (i == 0 || i == n ? 1.0 : i % 2 ? 4.0 : 2.0) * f;
        }
        EXPECT_NEAR(bivariate_norm_cdf(x, y, rho), sum * h / 3, 1e-9) << rho << ' ' << x << ' ' << y;
      }
}

TEST(AmericanApproximation, MatchesTheConvergedTreeOnTextbookInputs) {
  // The inputs of Barone-Adesi and Whaley's (1987) tables and Haug's (2007) BS2002
  // example, against a 2001-step LR tree rather than transcribed printed values.
  const BsmInputs cases[] = {{kCall, 90, 100, 0.1, 0.08, 0.12, 0.15}, {kCall, 100, 100, 0.1, 0.08, 0.12, 0.25},
                             {kCall, 110, 100, 0.1, 0.08, 0.12, 0.35}, {kPut, 90, 100, 0.5, 0.08, 0.0, 0.2},
                             {kPut, 100, 100, 0.25, 0.08, 0.04, 0.3}, {kCall, 42, 40, 0.75, 0.04, 0.08, 0.35}};
  for (const auto& in : cases) {
    const double tree = binomial_price(in, ExerciseStyle::American, TreeMethod::LeisenReimer, 2001);
    EXPECT_NEAR(barone_adesi_whaley_price(in), tree, 0.06) << in.spot << ' ' << in.vol;
    EXPECT_NEAR(bjerksund_stensland_price(in), tree, 0.04) << in.spot << ' ' << in.vol;
  }
}

TEST(AmericanApproximation, BjerksundStenslandMatchesHaugsTable) {
  // Haug (2007), The Complete Guide to Option Pricing Formulas, 2nd ed., the
  // Bjerksund-Stensland 2002 table: American calls, X = 100, r = 0.10, b = 0
  // (dividend yield 0.10), at the table's four decimals. Rows are T = 0.1 then
  // 0.5; within each, sigma = 0.15, 0.25, 0.35 at S = 90, 100, 110.
  const double table[2][3][3] = {{{0.0205, 1.8757, 10.0000}, {0.3151, 3.1256, 10.3725}, {0.9479, 4.3746, 11.1578}},
                                 {{0.8099, 4.0628, 10.7898}, {2.7180, 6.7661, 12.9814}, {4.9665, 9.4608, 15.5137}}};
  const double years[] = {0.1, 0.5}, vols[] = {0.15, 0.25, 0.35}, spots[] = {90, 100, 110};
  for (int t = 0; t < 2; ++t)
    for (int v = 0; v < 3; ++v)
      for (int s = 0; s < 3; ++s)
        EXPECT_NEAR(bjerksund_stensland_price({kCall, spots[s], 100, years[t], 0.10, 0.10, vols[v]}), table[t][v][s], 5e-5)
            << years[t] << ' ' << vols[v] << ' ' << spots[s];
}

TEST(AmericanApproximation, BoundedByEuropeanIntrinsicAndTheTree) {
  for (double t : {0.02, 0.25, 1.0, 3.0})
    for (double vol : {0.08, 0.2, 0.6})
      for (double k = 60; k <= 140; k += 10)
        for (auto type : {kCall, kPut}) {
          const BsmInputs in{type, 100, k, t, 0.05, 0.03, vol};
          const double floor = std::max(bsm_price(in), std::max(type == kCall ? 100 - k : k - 100, 0.0));
          const double tree = binomial_price(in, ExerciseStyle::American, TreeMethod::LeisenReimer, 1001);
          for (double price : {bjerksund_stensland_price(in), barone_adesi_whaley_price(in)}) {
            ASSERT_TRUE(std::isfinite(price));
            EXPECT_GE(price, floor - 1e-12);
            // Up to a year and 40% vol both stay within 0.2% of the strike of the
            // converged tree. Beyond that they drift apart (BAW above, BS2002
            // below by over $1 at three years and 60% vol), as documented.
            if (t <= 1 && vol <= 0.4) {
              EXPECT_NEAR(price, tree, 0.002 * k) << t << ' ' << vol << ' ' << k << ' ' << static_cast<int>(type);
            }
          }
          // BS2002's flat boundary is suboptimal exercise: it is a lower bound
          // on the true American value, up to the tree's own error.
          EXPECT_LE(bjerksund_stensland_price(in), tree + 2e-3);
        }
}

TEST(AmericanApproximation, NoPremiumWhereExerciseCannotPay) {
  for (auto price : {bjerksund_stensland_price, barone_adesi_whaley_price}) {
    // Calls without a dividend, puts at a zero rate: exactly European.
    EXPECT_EQ(price({kCall, 100, 95, 1, 0.05, 0, 0.25}), bsm_price({kCall, 100, 95, 1, 0.05, 0, 0.25}));
    EXPECT_EQ(price({kPut, 100, 105, 1, 0, 0.02, 0.25}), bsm_price({kPut, 100, 105, 1, 0, 0.02, 0.25}));
    // Invalid inputs are NaN, not a price.
    EXPECT_TRUE(std::isnan(price({kPut, 100, 105, 0, 0.05, 0, 0.25})));
    EXPECT_TRUE(std::isnan(price({kPut, 100, 105, 1, 0.05, 0, 0})));
    EXPECT_TRUE(std::isnan(price({kPut, -1, 105, 1, 0.05, 0, 0.2})));
  }
  // Deep in the money with a high rate the put is exercised now.
  EXPECT_DOUBLE_EQ(bjerksund_stensland_price({kPut, 50, 100, 1, 0.08, 0, 0.2}), 50);
  EXPECT_DOUBLE_EQ(barone_adesi_whaley_price({kPut, 50, 100, 1, 0.08, 0, 0.2}), 50);
}

TEST(AmericanApproximation, PremiumTracksTheLatticeOverAGrid) {
  // The validation behind keeping LR the default: on this grid both closed forms
  // stay within a few cents of a converged tree's EEP but are not within the 31-step
  // tree's own error; see docs/american-analytics.md for the measured maxima.
  double worst_bs = 0, worst_baw = 0, worst_lr = 0;
  for (double t : {7 / 365.0, 0.25, 1.0, 2.0})
    for (double vol : {0.1, 0.2, 0.4})
      for (double q : {0.0, 0.01, 0.03})
        for (double k = 70; k <= 130; k += 5)
          for (auto type : {kCall, kPut}) {
            const BsmInputs in{type, 100, k, t, 0.045, q, vol};
            const double reference = binomial_early_exercise_premium(in, 2001);
            worst_lr = std::max(worst_lr, std::abs(early_exercise_premium(in, AmericanModel::LeisenReimer, 31) - reference));
            worst_bs = std::max(worst_bs, std::abs(early_exercise_premium(in, AmericanModel::BjerksundStensland2002, 31) - reference));
            worst_baw = std::max(worst_baw, std::abs(early_exercise_premium(in, AmericanModel::BaroneAdesiWhaley, 31) - reference));
          }
  // Measured 2026-10-21: 0.0921, 0.2604 and 0.1530 on the wider bench grid.
  EXPECT_LT(worst_lr, 0.1);
  EXPECT_LT(worst_bs, 0.27);
  EXPECT_LT(worst_baw, 0.16);
  // Neither closed form is within the 31-step tree's own error, so LR stays the default.
  EXPECT_GT(worst_bs, worst_lr);
  EXPECT_GT(worst_baw, worst_lr);
}

TEST(AmericanApproximation, DispatchFallsBackToTheLattice) {
  const BsmInputs in{kPut, 100, 105, 0.5, 0.04, 0.01, 0.3};
  AmericanModel used = AmericanModel::BaroneAdesiWhaley;
  EXPECT_EQ(early_exercise_premium(in, AmericanModel::LeisenReimer, 31, {}, &used),
            binomial_early_exercise_premium(in, 31));
  EXPECT_EQ(used, AmericanModel::LeisenReimer);
  EXPECT_NEAR(early_exercise_premium(in, AmericanModel::BjerksundStensland2002, 31, {}, &used),
              bjerksund_stensland_price(in) - bsm_price(in), 1e-14);
  EXPECT_EQ(used, AmericanModel::BjerksundStensland2002);
  // Cash before expiry needs the escrowed lattice.
  const openport::pricing::CashDividend cash[] = {{0.25, 1.0}};
  EXPECT_EQ(early_exercise_premium(in, AmericanModel::BaroneAdesiWhaley, 31, cash, &used),
            binomial_early_exercise_premium(in, 31, cash));
  EXPECT_EQ(used, AmericanModel::LeisenReimer);
  // Cash after expiry does not change the value, so the closed form stays.
  const openport::pricing::CashDividend later[] = {{0.75, 1.0}};
  static_cast<void>(early_exercise_premium(in, AmericanModel::BaroneAdesiWhaley, 31, later, &used));
  EXPECT_EQ(used, AmericanModel::BaroneAdesiWhaley);
  // A negative rate is outside the closed forms' domain.
  const BsmInputs negative{kCall, 100, 95, 1, -0.01, 0.02, 0.2};
  EXPECT_EQ(early_exercise_premium(negative, AmericanModel::BjerksundStensland2002, 31, {}, &used),
            binomial_early_exercise_premium(negative, 31));
  EXPECT_EQ(used, AmericanModel::LeisenReimer);
  // Calls without carry against them have exactly no premium in every model.
  EXPECT_EQ(early_exercise_premium({kCall, 100, 95, 1, 0.05, 0, 0.2}, AmericanModel::BaroneAdesiWhaley, 31, {}, &used), 0);
  EXPECT_EQ(used, AmericanModel::BaroneAdesiWhaley);
}

TEST(AmericanApproximation, ParsesModelNames) {
  using openport::pricing::parse_american_model;
  for (auto model : {AmericanModel::LeisenReimer, AmericanModel::BjerksundStensland2002, AmericanModel::BaroneAdesiWhaley})
    EXPECT_EQ(parse_american_model(openport::pricing::to_string(model)), model);
  EXPECT_EQ(parse_american_model("lr"), AmericanModel::LeisenReimer);
  EXPECT_EQ(parse_american_model("bs2002"), AmericanModel::BjerksundStensland2002);
  EXPECT_EQ(parse_american_model("baw"), AmericanModel::BaroneAdesiWhaley);
  EXPECT_FALSE(parse_american_model("crr"));
  EXPECT_FALSE(parse_american_model("LR"));
}

}  // namespace
