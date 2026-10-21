#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "openport/analytics/svi.hpp"

namespace openport::analytics {

/// SABR dF = alpha_t F^beta dW, d alpha_t = nu alpha_t dZ, dW dZ = rho dt, with
/// alpha its initial value (in forward units to the power 1 - beta).
struct SabrParameters {
  double alpha = kNaN;
  double beta = kNaN;
  double rho = kNaN;
  double nu = kNaN;
};

struct SabrFit {
  SabrParameters parameters;
  SviStatus status = SviStatus::Failed;
  std::string reason;
  bool beta_calibrated = false;
  double rmse_vol_points = kNaN;
  std::size_t points = 0;
  double years = kNaN;
  double forward = kNaN;
  double min_k = kNaN;
  double max_k = kNaN;
  double fit_ms = 0.0;
  SviButterfly butterfly;                   ///< Gatheral's g on the calibration range
  std::vector<SviHalfSpread> half_spreads;  ///< as in SviFit, for calendar tolerances
};

/// Hagan, Kumar, Lesniewski and Woodward (2002), Eq. (2.17a): the Black (lognormal)
/// implied volatility of SABR at log-moneyness k = ln(K/F), with the ATM limit
/// of z/x(z) taken by series. An asymptotic expansion, not an exact price: it can
/// imply a negative density far from the money and at long tenors. NaN for
/// inadmissible parameters or where the expansion gives no positive volatility.
[[nodiscard]] double sabr_iv(const SabrParameters& p, double forward, double k, double years);
[[nodiscard]] bool sabr_admissible(const SabrParameters& p);
/// SVI's density diagnostic (g >= 0 on 2,001 points including both ends) applied to
/// SABR's total variance, with its k derivatives by central differences.
[[nodiscard]] SviButterfly sabr_butterfly(const SabrParameters& p, double forward, double years,
                                          double min_k, double max_k);

/// Calibrate one expiry. `beta` fixes the backbone exponent (the usual practice:
/// beta and rho are nearly collinear on a single smile); nullopt fits it in [0, 1].
///
/// Uses SVI's eligible OTM points and exactly its normalised quote weights,
/// minimising weighted total-variance squared residuals. Alpha is searched as the
/// scale-free alpha * F^(beta - 1), so the fit depends on k alone. Deterministic
/// bounded Nelder-Mead from fixed seeds; a local search without a global
/// optimality claim. At least five distinct points are required.
[[nodiscard]] SabrFit fit_sabr(std::span<const SviPoint> points, double forward, double years,
                               std::optional<double> beta = 1.0);
[[nodiscard]] SabrFit fit_sabr(const SliceMetrics& slice, std::optional<double> beta = 1.0);

/// svi_calendar's finite-grid total-variance ordering check, for SABR fits.
[[nodiscard]] std::vector<SviCalendarViolation> sabr_calendar(std::span<const SabrFit> fits);

}  // namespace openport::analytics
