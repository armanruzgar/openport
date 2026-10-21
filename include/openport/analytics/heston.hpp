#pragma once

#include <span>
#include <string>
#include <vector>

#include "openport/analytics/svi.hpp"

namespace openport::analytics {

/// Heston (1993): dv = kappa (theta - v) dt + xi sqrt(v) dZ, with spot-variance
/// correlation rho and initial variance v0, all annualised.
struct HestonParameters {
  double v0 = kNaN;
  double kappa = kNaN;
  double theta = kNaN;
  double xi = kNaN;
  double rho = kNaN;
};

/// Undiscounted Heston call on a forward, by Lewis's (2001) single integral
/// C = F - sqrt(F K)/pi * int_0^inf Re[exp(i u x) phi(u - i/2)] / (u^2 + 1/4) du,
/// x = ln(F/K), with the characteristic function in the "little Heston trap" form
/// of Albrecher et al. (2007), which avoids the complex-log branch jump, written
/// without its O(xi^2) cancellation. The integral uses 16-point Gauss-Legendre
/// panels, narrow at the origin and widening, to a range scaled to the expected
/// total variance. NaN for invalid inputs.
[[nodiscard]] double heston_call(const HestonParameters& p, double forward, double strike, double years);
/// The Black-76 implied volatility of heston_call (the OTM side), NaN when it has none.
[[nodiscard]] double heston_iv(const HestonParameters& p, double forward, double k, double years);
[[nodiscard]] bool heston_admissible(const HestonParameters& p);

struct HestonSlice {
  double years;
  double forward;
  std::vector<SviPoint> points;
};

struct HestonExpiryFit {
  double rmse_vol_points = kNaN;
  std::size_t points = 0;  ///< quotes calibrated, after thinning
  std::string reason;      ///< why the expiry was left out, if it was
};

struct HestonFit {
  HestonParameters parameters;
  SviStatus status = SviStatus::Failed;
  std::string reason;
  bool feller = false;  ///< 2 kappa theta >= xi^2: variance stays positive
  double rmse_vol_points = kNaN;
  std::size_t points = 0;
  double fit_ms = 0;
  std::vector<HestonExpiryFit> expiries;  ///< same order as input
};

/// One Heston parameter set for a whole underlying, as a research comparison with
/// SVI/SSVI; nothing in OpenPort's analytics or trading uses it.
///
/// Uses SVI's eligible OTM quotes for expiries at least a week away, within three
/// ATM standard deviations of the forward (|k| <= 3 IV_atm sqrt(T)), thinned evenly
/// in k to at most 25 per expiry, with SVI's normalised weights and equal expiry
/// weight. Minimises vega-weighted price residuals (a first-order IV residual),
/// by bounded Nelder-Mead in (log v0, log kappa, log theta, log xi, rho) from 18
/// fixed seeds, refining the best two. At least two usable expiries with five
/// quotes each are needed. Feller's condition is reported, not imposed. Heston's
/// parameters are weakly identified (kappa against theta and xi above all), so the
/// fitted surface is more meaningful than any single parameter.
[[nodiscard]] HestonFit fit_heston(std::span<const HestonSlice> slices);
[[nodiscard]] HestonFit fit_heston(std::span<const SliceMetrics> slices);

}  // namespace openport::analytics
