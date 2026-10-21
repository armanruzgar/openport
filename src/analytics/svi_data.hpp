#pragma once

#include <functional>

#include "openport/analytics/svi.hpp"

namespace openport::analytics::detail {
struct SviDatum { double k, iv, w, weight, half_spread; };
// Shared eligibility and per-expiry normalized weights for SVI and SSVI.
std::vector<SviDatum> prepare_svi(std::span<const SviPoint> points, double years);
std::vector<SviPoint> svi_points(const SliceMetrics& slice);

/// One successful smile fit, as the calendar check needs it.
struct CalendarSlice {
  std::size_t index;  ///< position in the caller's fits
  double years, min_k, max_k, rmse_vol_points;
  const std::vector<SviHalfSpread>* half_spreads;
  std::function<double(double k, double years)> iv;  ///< fitted IV of total variance at k, over `years`
};
/// svi_calendar's check for any per-expiry smile model; see svi.hpp.
std::vector<SviCalendarViolation> calendar(std::vector<CalendarSlice> slices);
}  // namespace openport::analytics::detail
