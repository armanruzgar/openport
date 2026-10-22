#include "openport/pricing/implied_vol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

#include "openport/pricing/normal.hpp"

namespace openport::pricing {
namespace {

struct PriceVega {
  double price;
  double vega;
};

/// Undiscounted Black price and vega, with the per-solve constants hoisted out.
[[nodiscard]] PriceVega undiscounted(double w, double forward, double strike, double sqrt_t,
                                     double log_moneyness, double vol) noexcept {
  const double sd = vol * sqrt_t;
  const double d1 = log_moneyness / sd + 0.5 * sd;
  const double d2 = d1 - sd;
  return {w * (forward * norm_cdf(w * d1) - strike * norm_cdf(w * d2)),
          forward * norm_pdf(d1) * sqrt_t};
}

/// Corrado and Miller (1996) closed-form estimate of vol * sqrt(T) from an
/// undiscounted call price. Very good near the money; in the wings it can come
/// out negative or wildly off, which the caller checks for.
[[nodiscard]] double corrado_miller_total_vol(double call, double forward, double strike) noexcept {
  const double gap = forward - strike;
  const double a = call - 0.5 * gap;
  const double discriminant = a * a - gap * gap / std::numbers::pi;
  return std::sqrt(2.0 * std::numbers::pi) / (forward + strike) *
         (a + std::sqrt(std::max(discriminant, 0.0)));
}

}  // namespace

std::string_view to_string(IvStatus status) noexcept {
  switch (status) {
    case IvStatus::Ok:
      return "ok";
    case IvStatus::BelowIntrinsic:
      return "below_intrinsic";
    case IvStatus::AboveMaximum:
      return "above_maximum";
    case IvStatus::NotConverged:
      return "not_converged";
    case IvStatus::InvalidInput:
      return "invalid_input";
    case IvStatus::NonFiniteModel:
      return "non_finite_model";
  }
  return "unknown";
}

IvResult implied_vol_black(double price, OptionType type, double forward, double strike,
                           double expiry, double discount, const IvOptions& options) noexcept {
  IvResult result;
  if (!std::isfinite(price) || !(price >= 0.0) || !(forward > 0.0) || !(strike > 0.0) ||
      !(expiry > 0.0) || !(discount > 0.0) || !std::isfinite(forward) || !std::isfinite(strike) ||
      !std::isfinite(expiry) || !std::isfinite(discount) || !std::isfinite(options.max_vol) ||
      !(options.max_vol > 0.0) || !std::isfinite(options.vol_tolerance) ||
      !(options.vol_tolerance > 0.0) || !std::isfinite(options.price_tolerance) ||
      !(options.price_tolerance > 0.0) || options.max_iterations < 1) {
    return result;
  }

  // Work with the undiscounted out-of-the-money option.
  double target = price / discount;
  OptionType otm = type;
  if (type == OptionType::Call && forward > strike) {
    target -= forward - strike;
    otm = OptionType::Put;
  } else if (type == OptionType::Put && strike > forward) {
    target -= strike - forward;
    otm = OptionType::Call;
  }

  if (!std::isfinite(target)) {
    result.status = IvStatus::NonFiniteModel;
    return result;
  }
  // Deep in the money the time value left after parity can be rounding noise of the
  // price: solving it would report a precise-looking but wrong volatility.
  if (!(target > 1024 * std::numeric_limits<double>::epsilon() * (price / discount))) {
    result.status = IvStatus::BelowIntrinsic;
    return result;
  }
  const double upper_bound = otm == OptionType::Call ? forward : strike;
  if (target >= upper_bound) {
    result.status = IvStatus::AboveMaximum;
    return result;
  }

  const double w = omega(otm);
  const double sqrt_t = std::sqrt(expiry);
  const double x = std::log(forward / strike);
  const double log_target = std::log(target);

  double lo = 0.0;
  double hi = options.max_vol;
  auto finite_model = [&](const PriceVega& pv) {
    if (std::isfinite(pv.price) && std::isfinite(pv.vega)) return true;
    result.status = IvStatus::NonFiniteModel;
    return false;
  };
  const auto top = undiscounted(w, forward, strike, sqrt_t, x, hi);
  if (!finite_model(top)) return result;
  if (top.price < target) {
    result.status = IvStatus::AboveMaximum;
    return result;
  }

  // Start from the Corrado-Miller estimate. Where it breaks down, fall back to the
  // inflection point of price(vol), from which Newton converges monotonically.
  const double call_equivalent = otm == OptionType::Call ? target : target + (forward - strike);
  double vol = corrado_miller_total_vol(call_equivalent, forward, strike) / sqrt_t;
  if (!(vol > lo && vol < hi)) vol = std::sqrt(2.0 * std::abs(x) / expiry);
  if (!(vol > lo && vol < hi)) vol = 0.5 * (lo + hi);

  for (int i = 1; i <= options.max_iterations; ++i) {
    const PriceVega pv = undiscounted(w, forward, strike, sqrt_t, x, vol);
    result.iterations = i;
    if (!finite_model(pv)) return result;

    const double diff = pv.price - target;
    if (std::abs(diff) <= options.price_tolerance * target) {
      result.vol = vol;
      result.status = IvStatus::Ok;
      return result;
    }
    if (diff > 0.0) {
      hi = vol;
    } else {
      lo = vol;
    }

    // Newton on f(vol) = log(price(vol)) - log(target), with f' = vega / price.
    double next = std::numeric_limits<double>::quiet_NaN();
    if (pv.price > 0.0 && pv.vega > 0.0) {
      next = vol - (std::log(pv.price) - log_target) * pv.price / pv.vega;
    }
    if (!(next > lo && next < hi)) next = 0.5 * (lo + hi);  // left the bracket: bisect

    if (std::abs(next - vol) <= options.vol_tolerance) {
      if (!finite_model(undiscounted(w, forward, strike, sqrt_t, x, next))) return result;
      result.vol = next;
      result.status = IvStatus::Ok;
      return result;
    }
    vol = next;
  }

  result.vol = vol;
  result.status = IvStatus::NotConverged;
  return result;
}

IvResult implied_vol_bsm(double price, OptionType type, double spot, double strike, double expiry,
                         double rate, double dividend, const IvOptions& options) noexcept {
  if (!std::isfinite(spot) || !std::isfinite(rate) || !std::isfinite(dividend)) return {};
  const double discount = std::exp(-rate * expiry);
  const double forward = spot * std::exp((rate - dividend) * expiry);
  return implied_vol_black(price, type, forward, strike, expiry, discount, options);
}

}  // namespace openport::pricing
