#pragma once

#include <cmath>

namespace openport::pricing {

inline constexpr double kInvSqrt2Pi = 0.398942280401432677939946059934381868;
inline constexpr double kInvSqrt2 = 0.707106781186547524400844362104849039;

/// Standard normal density.
[[nodiscard]] inline double norm_pdf(double x) noexcept {
  return kInvSqrt2Pi * std::exp(-0.5 * x * x);
}

/// Standard normal cumulative distribution.
///
/// Written with erfc rather than 0.5 * (1 + erf(x)) because the latter cancels
/// catastrophically in the left tail, where deep out-of-the-money prices live.
[[nodiscard]] inline double norm_cdf(double x) noexcept {
  return 0.5 * std::erfc(-x * kInvSqrt2);
}

/// Bivariate standard normal cumulative distribution P(X <= x, Y <= y) with
/// correlation rho, by Genz's (2004) Gauss-Legendre form of Drezner and
/// Wesolowsky; absolute error near 1e-15. NaN outside -1 <= rho <= 1.
[[nodiscard]] double bivariate_norm_cdf(double x, double y, double rho) noexcept;

}  // namespace openport::pricing
