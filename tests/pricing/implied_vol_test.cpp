#include "openport/pricing/implied_vol.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "openport/pricing/black.hpp"

namespace {

using openport::pricing::black_greeks;
using openport::pricing::black_price;
using openport::pricing::implied_vol_black;
using openport::pricing::implied_vol_bsm;
using openport::pricing::IvStatus;
using openport::pricing::OptionType;

// Round-trips price -> implied vol -> vol across a wide grid: expiries from one
// hour to five years, vols from 1% to 400%, strikes out to six standard
// deviations (capped at a factor of e^5 either side of the forward).
//
// The achievable accuracy depends on how well-conditioned the problem is: a
// price error of e moves the answer by e / vega. So the tolerance scales with
// 1 / vega instead of being one fixed number that is either too loose at the
// money or impossible in the wings.
TEST(ImpliedVol, RoundTripsAcrossAWideGrid) {
  const double forward = 100.0;
  const std::vector<double> expiries = {
      1.0 / (365 * 24), 1.0 / 365, 7.0 / 365, 30.0 / 365, 0.25, 1.0, 5.0};
  const std::vector<double> vols = {0.01, 0.05, 0.2, 0.5, 1.0, 2.0, 4.0};
  int solved = 0;
  int max_iterations = 0;
  for (double t : expiries) {
    for (double vol : vols) {
      for (double z = -6.0; z <= 6.0; z += 0.25) {
        const double log_moneyness = z * vol * std::sqrt(t);
        if (std::abs(log_moneyness) > 5.0) continue;
        const double strike = forward * std::exp(-log_moneyness);
        for (OptionType type : {OptionType::Call, OptionType::Put}) {
          const auto g = black_greeks(type, forward, strike, t, vol);
          const double time_value =
              g.price - std::max(openport::pricing::omega(type) * (forward - strike), 0.0);
          // Below this the time value is lost in the rounding of the intrinsic value.
          if (time_value < 1e-10 * std::max(forward, strike)) continue;

          const auto iv = implied_vol_black(g.price, type, forward, strike, t);
          ASSERT_TRUE(iv.ok()) << "t=" << t << " vol=" << vol << " z=" << z
                               << " status=" << openport::pricing::to_string(iv.status);
          const double tolerance = 1e-13 * std::max(forward, strike) / g.vega + 1e-10;
          EXPECT_NEAR(iv.vol, vol, tolerance) << "t=" << t << " vol=" << vol << " z=" << z;
          max_iterations = std::max(max_iterations, iv.iterations);
          ++solved;
        }
      }
    }
  }
  EXPECT_GT(solved, 1500);
  EXPECT_LE(max_iterations, 40);
}

TEST(ImpliedVol, ConvergesInAFewIterationsNearTheMoney) {
  for (double k : {80.0, 90.0, 100.0, 110.0, 125.0}) {
    const double price = black_price(OptionType::Call, 100.0, k, 0.5, 0.35);
    const auto iv = implied_vol_black(price, OptionType::Call, 100.0, k, 0.5);
    ASSERT_TRUE(iv.ok());
    EXPECT_NEAR(iv.vol, 0.35, 1e-12);
    EXPECT_LE(iv.iterations, 6);
  }
}

TEST(ImpliedVol, InTheMoneyAndOutOfTheMoneyGiveTheSameVol) {
  const double discount = std::exp(-0.03 * 0.75);
  const double call = black_price(OptionType::Call, 100.0, 80.0, 0.75, 0.42, discount);
  const double put = black_price(OptionType::Put, 100.0, 80.0, 0.75, 0.42, discount);
  const auto from_call = implied_vol_black(call, OptionType::Call, 100.0, 80.0, 0.75, discount);
  const auto from_put = implied_vol_black(put, OptionType::Put, 100.0, 80.0, 0.75, discount);
  ASSERT_TRUE(from_call.ok());
  ASSERT_TRUE(from_put.ok());
  EXPECT_NEAR(from_call.vol, 0.42, 1e-10);
  EXPECT_NEAR(from_put.vol, 0.42, 1e-12);
}

TEST(ImpliedVol, SpotVersionMatchesForwardVersion) {
  const double s = 100.0, k = 95.0, t = 0.4, r = 0.05, q = 0.02, vol = 0.27;
  const double forward = s * std::exp((r - q) * t);
  const double discount = std::exp(-r * t);
  const double price = black_price(OptionType::Put, forward, k, t, vol, discount);
  const auto iv = implied_vol_bsm(price, OptionType::Put, s, k, t, r, q);
  ASSERT_TRUE(iv.ok());
  EXPECT_NEAR(iv.vol, vol, 1e-12);
}

TEST(ImpliedVol, RejectsPricesOutsideTheNoArbitrageBounds) {
  // A call on F=100, K=90 is worth at least 10 undiscounted and at most 100.
  EXPECT_EQ(implied_vol_black(9.5, OptionType::Call, 100.0, 90.0, 1.0).status,
            IvStatus::BelowIntrinsic);
  EXPECT_EQ(implied_vol_black(10.0, OptionType::Call, 100.0, 90.0, 1.0).status,
            IvStatus::BelowIntrinsic);
  EXPECT_EQ(implied_vol_black(100.0, OptionType::Call, 100.0, 90.0, 1.0).status,
            IvStatus::AboveMaximum);
  EXPECT_EQ(implied_vol_black(0.0, OptionType::Put, 100.0, 90.0, 1.0).status,
            IvStatus::BelowIntrinsic);
}

TEST(ImpliedVol, NoiseLevelTimeValueDeepInTheMoneyIsNotSolved) {
  // About 19.4 of intrinsic value and 7e-15 of time value: what is left after parity is
  // the price's rounding, not volatility.
  const double price = 0.97 * black_price(OptionType::Put, 100.0, 120.0, 5.0, 0.01);
  EXPECT_EQ(implied_vol_black(price, OptionType::Put, 100.0, 120.0, 5.0, 0.97).status, IvStatus::BelowIntrinsic);
  // A time value well above the rounding still solves.
  const double priced = black_price(OptionType::Put, 100.0, 120.0, 5.0, 0.2);
  const auto iv = implied_vol_black(priced, OptionType::Put, 100.0, 120.0, 5.0);
  ASSERT_TRUE(iv.ok());
  EXPECT_NEAR(iv.vol, 0.2, 1e-9);
}

TEST(ImpliedVol, RejectsInvalidInputs) {
  EXPECT_EQ(implied_vol_black(-1.0, OptionType::Call, 100.0, 90.0, 1.0).status,
            IvStatus::InvalidInput);
  EXPECT_EQ(implied_vol_black(5.0, OptionType::Call, 100.0, 90.0, 0.0).status,
            IvStatus::InvalidInput);
  EXPECT_EQ(implied_vol_black(5.0, OptionType::Call, -100.0, 90.0, 1.0).status,
            IvStatus::InvalidInput);
  EXPECT_EQ(implied_vol_black(std::nan(""), OptionType::Call, 100.0, 90.0, 1.0).status,
            IvStatus::InvalidInput);
}

TEST(ImpliedVol, RejectsEveryNonFiniteInputAndSolverOption) {
  for (double bad : {std::nan(""), double(INFINITY), -double(INFINITY)}) {
    for (int field = 0; field < 5; ++field) {
      double args[] = {5, 100, 100, 1, 1};
      args[field] = bad;
      EXPECT_EQ(
          implied_vol_black(args[0], OptionType::Call, args[1], args[2], args[3], args[4]).status,
          IvStatus::InvalidInput)
          << field;
    }
    for (auto member :
         {&openport::pricing::IvOptions::max_vol, &openport::pricing::IvOptions::vol_tolerance,
          &openport::pricing::IvOptions::price_tolerance}) {
      openport::pricing::IvOptions options;
      options.*member = bad;
      EXPECT_EQ(implied_vol_black(5, OptionType::Call, 100, 100, 1, 1, options).status,
                IvStatus::InvalidInput);
    }
    EXPECT_EQ(implied_vol_bsm(5, OptionType::Call, 100, 100, 1, bad, 0).status,
              IvStatus::InvalidInput);
    EXPECT_EQ(implied_vol_bsm(5, OptionType::Call, 100, 100, 1, 0, bad).status,
              IvStatus::InvalidInput);
  }
}

TEST(ImpliedVol, OverflowingModelEvaluationCannotReportOk) {
  const auto iv = implied_vol_black(1e307, OptionType::Call, 1e308, 1e308, 100);
  EXPECT_FALSE(iv.ok());
  EXPECT_TRUE(std::isnan(iv.vol));
}

TEST(ImpliedVol, NonFiniteModelHasAnExplicitStatus) {
  const auto iv = implied_vol_black(1e307, OptionType::Call, 1e308, 1e308, 100);
  EXPECT_EQ(iv.status, IvStatus::NonFiniteModel);
  EXPECT_EQ(openport::pricing::to_string(iv.status), "non_finite_model");
  for (auto member :
       {&openport::pricing::IvOptions::max_vol, &openport::pricing::IvOptions::vol_tolerance,
        &openport::pricing::IvOptions::price_tolerance}) {
    openport::pricing::IvOptions o;
    o.*member = 0;
    EXPECT_EQ(implied_vol_black(5, OptionType::Call, 100, 100, 1, 1, o).status,
              IvStatus::InvalidInput);
  }
  openport::pricing::IvOptions o;
  o.max_iterations = 0;
  EXPECT_EQ(implied_vol_black(5, OptionType::Call, 100, 100, 1, 1, o).status,
            IvStatus::InvalidInput);
}

}  // namespace
