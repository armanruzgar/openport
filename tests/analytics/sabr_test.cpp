#include "openport/analytics/sabr.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "openport/pricing/black.hpp"

namespace {
using namespace openport::analytics;
constexpr double kForward = 5000;
const SabrParameters known{.18, 1, -.65, 1.4};

std::vector<SviPoint> smile(const SabrParameters& p = known, double years = .25, double noise = 0,
                            double lo = -.25, double hi = .15) {
  std::vector<SviPoint> points;
  for (int i = 0; i <= 40; ++i) {
    const double k = lo + (hi - lo) * i / 40;
    const double iv = sabr_iv(p, kForward, k, years) + noise * std::sin(i * 2.3);
    points.push_back({k, iv, iv - .002, iv + .002});
  }
  return points;
}

TEST(Sabr, MatchesHaganAtTheMoneyAndIsScaleFree) {
  // ATM, z/x(z) = 1 and the expansion reduces to Hagan's Eq. (2.18).
  const SabrParameters p{.6, .5, -.3, .8};
  const double f = 100, t = 1.5;
  const double atm = p.alpha / std::pow(f, .5) *
                     (1 + (.25 / 24 * p.alpha * p.alpha / f + p.rho * p.beta * p.nu * p.alpha / (4 * std::pow(f, .5)) +
                           (2 - 3 * p.rho * p.rho) / 24 * p.nu * p.nu) * t);
  EXPECT_NEAR(sabr_iv(p, f, 0, t), atm, 1e-15);
  // Continuous through the series branch for z near zero.
  EXPECT_NEAR(sabr_iv(p, f, 1e-9, t), sabr_iv(p, f, 1e-5, t), 2e-5);
  // Scaling F and alpha together (alpha ~ F^(1 - beta)) leaves the smile unchanged.
  const SabrParameters scaled{p.alpha * std::pow(10.0, .5), .5, -.3, .8};
  for (double k : {-.4, -.1, 0.0, .2}) EXPECT_NEAR(sabr_iv(scaled, 1000, k, t), sabr_iv(p, f, k, t), 1e-14);
  // With nu = 0 and beta = 1 SABR is Black-Scholes at vol alpha.
  EXPECT_NEAR(sabr_iv({.2, 1, 0, 0}, f, -.3, t), .2, 1e-15);
  EXPECT_TRUE(std::isnan(sabr_iv({-.2, 1, 0, .5}, f, 0, t)));
  EXPECT_TRUE(std::isnan(sabr_iv({.2, 1.2, 0, .5}, f, 0, t)));
  EXPECT_TRUE(std::isnan(sabr_iv({.2, 1, 1, .5}, f, 0, t)));
  EXPECT_TRUE(std::isnan(sabr_iv(p, f, 0, 0)));
}

TEST(Sabr, RecoversParametersWithFixedBeta) {
  for (const auto& p : {known, SabrParameters{.18, 1, .3, .6}, SabrParameters{.18 * std::pow(kForward, .5), .5, -.4, 2.5}}) {
    const auto fit = fit_sabr(smile(p), kForward, .25, p.beta);
    ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
    EXPECT_FALSE(fit.beta_calibrated);
    EXPECT_EQ(fit.parameters.beta, p.beta);
    EXPECT_NEAR(fit.parameters.alpha / p.alpha, 1, 1e-5);
    EXPECT_NEAR(fit.parameters.rho, p.rho, 1e-5);
    EXPECT_NEAR(fit.parameters.nu, p.nu, 1e-4);
    EXPECT_LT(fit.rmse_vol_points, 1e-5);
    EXPECT_EQ(fit.points, 41u);
    EXPECT_GT(fit.fit_ms, 0);
  }
}

TEST(Sabr, SmallNoiseKeepsTheSmileAndCalibratedBetaFitsAsWell) {
  const auto fixed = fit_sabr(smile(known, .25, .0002), kForward, .25, 1.0);
  ASSERT_EQ(fixed.status, SviStatus::Ok) << fixed.reason;
  EXPECT_NEAR(fixed.parameters.rho, known.rho, .02);
  EXPECT_NEAR(fixed.parameters.nu, known.nu, .05);
  EXPECT_LT(fixed.rmse_vol_points, .03);
  const auto free = fit_sabr(smile(known, .25, .0002), kForward, .25, std::nullopt);
  ASSERT_EQ(free.status, SviStatus::Ok) << free.reason;
  EXPECT_TRUE(free.beta_calibrated);
  EXPECT_GE(free.parameters.beta, 0);
  EXPECT_LE(free.parameters.beta, 1);
  // One more free parameter can only fit at least as well, up to search tolerance.
  EXPECT_LE(free.rmse_vol_points, fixed.rmse_vol_points + 1e-3);
}

TEST(Sabr, DeterministicAndInsensitiveToInputOrder) {
  auto points = smile(known, .25, .0002);
  const auto a = fit_sabr(points, kForward, .25);
  std::reverse(points.begin(), points.end());
  const auto b = fit_sabr(points, kForward, .25);
  ASSERT_EQ(a.status, SviStatus::Ok);
  EXPECT_DOUBLE_EQ(a.parameters.alpha, b.parameters.alpha);
  EXPECT_DOUBLE_EQ(a.parameters.rho, b.parameters.rho);
  EXPECT_DOUBLE_EQ(a.parameters.nu, b.parameters.nu);
  EXPECT_DOUBLE_EQ(a.rmse_vol_points, b.rmse_vol_points);
}

TEST(Sabr, FailsClosedOnMissingInvalidAndDegenerateData) {
  auto points = smile();
  for (std::size_t i = 4; i < points.size(); ++i) points[i].ask_iv = kNaN;
  auto fit = fit_sabr(points, kForward, .25);
  EXPECT_EQ(fit.status, SviStatus::TooFewPoints);
  EXPECT_EQ(fit.points, 4u);
  EXPECT_TRUE(std::isnan(fit.parameters.alpha));
  EXPECT_EQ(fit_sabr(smile(), kForward, 0).status, SviStatus::Failed);
  EXPECT_EQ(fit_sabr(smile(), kNaN, .25).status, SviStatus::Failed);
  EXPECT_EQ(fit_sabr(smile(), kForward, .25, 1.5).status, SviStatus::Failed);
  EXPECT_EQ(fit_sabr(std::vector<SviPoint>(8, {.1, .2, .19, .21}), kForward, .25).status, SviStatus::TooFewPoints);
  points = smile();
  for (auto& p : points) p.k *= 1e-8;
  EXPECT_EQ(fit_sabr(points, kForward, .25).status, SviStatus::Failed);
}

TEST(Sabr, FlatSmileFitsWithoutSpuriousVolOfVol) {
  std::vector<SviPoint> flat;
  for (int i = 0; i <= 20; ++i) flat.push_back({-.2 + .02 * i, .2, .198, .202});
  const auto fit = fit_sabr(flat, kForward, .5);
  ASSERT_EQ(fit.status, SviStatus::Ok) << fit.reason;
  EXPECT_LT(fit.rmse_vol_points, .01);
  for (double k : {-.2, 0.0, .2}) EXPECT_NEAR(sabr_iv(fit.parameters, kForward, k, .5), .2, 2e-4);
}

TEST(Sabr, ButterflyDiagnosticMatchesPriceConvexity) {
  // A mild smile has a positive density on its quoted range.
  const auto good = sabr_butterfly(known, kForward, .25, -.25, .15);
  EXPECT_TRUE(good.ok);
  EXPECT_GT(good.min_g, 0);
  // Hagan's expansion is known to imply a negative density at low strikes for long
  // tenors and large vol of vol; the check must flag it, and the call prices
  // themselves must fail convexity where it does.
  const SabrParameters wild{.25, 1, -.5, 1};
  const auto bad = sabr_butterfly(wild, kForward, 5, -2, .5);
  ASSERT_TRUE(std::isfinite(bad.min_g));
  EXPECT_FALSE(bad.ok);
  const double k = bad.k, h = .01;
  auto call = [&](double x) {
    return openport::pricing::black_price(openport::pricing::OptionType::Call, 1, std::exp(x), 5,
                                          sabr_iv(wild, kForward, x, 5));
  };
  // d2C/dK2 with K = e^x: (C'' - C') e^(-2x) in x.
  const double second = (call(k + h) - 2 * call(k) + call(k - h)) / (h * h);
  const double first = (call(k + h) - call(k - h)) / (2 * h);
  EXPECT_LT(second - first, 0);
  EXPECT_FALSE(sabr_butterfly(known, kForward, .25, .2, .1).ok);
}

TEST(Sabr, CalendarFlagsCrossingTotalVariance) {
  const auto near = fit_sabr(smile(known, .25), kForward, .25);
  auto later_parameters = known;
  later_parameters.alpha = .12;  // far lower vol at a later tenor: total variance falls
  const auto later = fit_sabr(smile(later_parameters, .3), kForward, .3);
  ASSERT_EQ(near.status, SviStatus::Ok);
  ASSERT_EQ(later.status, SviStatus::Ok);
  const std::vector<SabrFit> fits{later, near};
  const auto violations = sabr_calendar(fits);
  ASSERT_EQ(violations.size(), 1u);
  EXPECT_EQ(violations[0].earlier, 1u);
  EXPECT_EQ(violations[0].later, 0u);
  EXPECT_GT(violations[0].vol_points, violations[0].tolerance_vol_points);
  later_parameters.alpha = .2;
  const std::vector<SabrFit> ordered{near, fit_sabr(smile(later_parameters, 1), kForward, 1)};
  EXPECT_TRUE(sabr_calendar(ordered).empty());
}

}  // namespace
