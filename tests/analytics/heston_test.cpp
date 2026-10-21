#include "openport/analytics/heston.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "openport/pricing/black.hpp"

namespace {
using namespace openport::analytics;
const HestonParameters known{0.03, 2.5, 0.05, 0.6, -0.7};

std::vector<HestonSlice> surface(const HestonParameters& p = known, double noise = 0) {
  std::vector<HestonSlice> slices;
  for (double years : {30 / 365.0, 0.25, 0.75}) {
    HestonSlice slice{years, 5000, {}};
    const double width = 0.25 * std::sqrt(years / 0.25);
    for (int i = 0; i <= 30; ++i) {
      const double k = -1.6 * width + 2.4 * width * i / 30;
      const double iv = heston_iv(p, 5000, k, years) + noise * std::sin(2.3 * i + years);
      slice.points.push_back({k, iv, iv - 0.003, iv + 0.003});
    }
    slices.push_back(std::move(slice));
  }
  return slices;
}

TEST(Heston, NoVolOfVolIsBlackAtTheMeanVariance) {
  for (double years : {0.02, 0.5, 3.0}) {  // a week, six months, three years
    HestonParameters p{0.09, 1.5, 0.04, 1e-5, -0.5};
    const double mean = p.theta + (p.v0 - p.theta) * (1 - std::exp(-p.kappa * years)) / (p.kappa * years);
    for (double k : {-0.3, 0.0, 0.2})
      EXPECT_NEAR(heston_iv(p, 100, k * std::sqrt(years), years), std::sqrt(mean), 1e-5) << years << ' ' << k;
  }
}

TEST(Heston, PricesAgreeWithMonteCarlo) {
  // Full-truncation Euler, fixed seed: an independent check of the transform.
  const double years = 0.5, forward = 100;
  std::mt19937_64 rng(7);
  std::normal_distribution<double> normal;
  const int paths = 100000, steps = 100;
  const double dt = years / steps;
  std::vector<double> terminal(paths);
  for (int i = 0; i < paths; ++i) {
    double x = 0, v = known.v0;
    for (int j = 0; j < steps; ++j) {
      const double z1 = normal(rng), z2 = known.rho * z1 + std::sqrt(1 - known.rho * known.rho) * normal(rng);
      const double positive = std::max(v, 0.0);
      x += -0.5 * positive * dt + std::sqrt(positive * dt) * z1;
      v += known.kappa * (known.theta - positive) * dt + known.xi * std::sqrt(positive * dt) * z2;
    }
    terminal[i] = forward * std::exp(x);
  }
  for (double strike : {80.0, 95.0, 100.0, 110.0}) {
    double sum = 0, squares = 0;
    for (double s : terminal) {
      const double payoff = strike < forward ? std::max(strike - s, 0.0) : std::max(s - strike, 0.0);
      sum += payoff;
      squares += payoff * payoff;
    }
    const double mean = sum / paths;
    const double error = std::sqrt((squares / paths - mean * mean) / paths);
    const double call = heston_call(known, forward, strike, years);
    const double model = strike < forward ? call - (forward - strike) : call;
    // Four standard errors plus Euler's small discretisation bias.
    EXPECT_NEAR(model, mean, 4 * error + 0.01) << strike;
  }
  // A negative correlation gives a negative skew.
  EXPECT_GT(heston_iv(known, forward, -0.2, years), heston_iv(known, forward, 0.2, years));
  EXPECT_TRUE(std::isnan(heston_call({0.03, 2, 0.04, 0.5, 1.0}, 100, 100, 1)));
  EXPECT_TRUE(std::isnan(heston_call(known, 100, 100, 0)));
}

TEST(Heston, CalibrationRecoversASyntheticSurface) {
  const auto fit = fit_heston(surface());
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_LT(fit.rmse_vol_points, 0.01);
  // The surface is well identified; the individual parameters less so.
  EXPECT_NEAR(fit.parameters.v0, known.v0, 0.002);
  EXPECT_NEAR(fit.parameters.rho, known.rho, 0.05);
  EXPECT_NEAR(fit.parameters.xi, known.xi, 0.1);
  EXPECT_NEAR(fit.parameters.kappa * fit.parameters.theta, known.kappa * known.theta, 0.03);
  EXPECT_EQ(fit.feller, 2 * fit.parameters.kappa * fit.parameters.theta >= fit.parameters.xi * fit.parameters.xi);
  ASSERT_EQ(fit.expiries.size(), 3U);
  for (const auto& expiry : fit.expiries) {
    // Within three ATM standard deviations, at most 25 of the 31 quotes.
    EXPECT_GT(expiry.points, 15U);
    EXPECT_LE(expiry.points, 25U);
    EXPECT_LT(expiry.rmse_vol_points, 0.02);
    EXPECT_TRUE(expiry.reason.empty());
  }
  EXPECT_GT(fit.fit_ms, 0);
}

TEST(Heston, NoisySurfaceFitsWithinTheNoiseAndDeterministically) {
  const auto a = fit_heston(surface(known, 0.002));
  const auto b = fit_heston(surface(known, 0.002));
  ASSERT_EQ(a.status, SviStatus::Ok) << a.reason;
  EXPECT_LT(a.rmse_vol_points, 0.3);
  EXPECT_DOUBLE_EQ(a.parameters.kappa, b.parameters.kappa);
  EXPECT_DOUBLE_EQ(a.rmse_vol_points, b.rmse_vol_points);
}

TEST(Heston, FailsClosedWithoutTwoUsableExpiries) {
  auto slices = surface();
  slices.resize(1);
  auto fit = fit_heston(slices);
  EXPECT_EQ(fit.status, SviStatus::TooFewPoints);
  EXPECT_FALSE(fit.reason.empty());
  EXPECT_TRUE(std::isnan(fit.parameters.kappa));
  slices = surface();
  slices[1].years = 5 / 365.0;
  slices[2].points.resize(4);
  fit = fit_heston(slices);
  EXPECT_EQ(fit.status, SviStatus::TooFewPoints);
  EXPECT_EQ(fit.expiries[1].reason, "expiry is less than a week away");
  EXPECT_EQ(fit.expiries[2].reason, "need at least five distinct two-sided OTM IV points");
  slices = surface();
  slices[0].forward = 0;
  fit = fit_heston(slices);
  EXPECT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_EQ(fit.expiries[0].reason, "forward must be finite and positive");
  EXPECT_TRUE(std::isnan(fit.expiries[0].rmse_vol_points));
}

}  // namespace
