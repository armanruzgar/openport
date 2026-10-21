#include "openport/pricing/american.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "openport/pricing/normal.hpp"

namespace openport::pricing {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// Generalised BSM call with cost of carry b (b = r - q).
double carry_call(double s, double k, double t, double r, double b, double v) noexcept {
  return bsm_price({OptionType::Call, s, k, t, r, r - b, v});
}

// Haug (2007), "The Complete Guide to Option Pricing Formulas", 2nd ed., 3.1.4:
// phi and psi of the Bjerksund-Stensland (2002) call.
double phi(double s, double t, double gamma, double h, double i, double r, double b, double v) noexcept {
  const double v2 = v * v;
  const double lambda = (-r + gamma * b + 0.5 * gamma * (gamma - 1) * v2) * t;
  const double sd = v * std::sqrt(t);
  const double d = -(std::log(s / h) + (b + (gamma - 0.5) * v2) * t) / sd;
  const double kappa = 2 * b / v2 + (2 * gamma - 1);
  return std::exp(lambda) * std::pow(s, gamma) *
         (norm_cdf(d) - std::pow(i / s, kappa) * norm_cdf(d - 2 * std::log(i / s) / sd));
}

double psi(double s, double t2, double gamma, double h, double i2, double i1, double t1, double r,
           double b, double v) noexcept {
  const double v2 = v * v;
  const double drift = b + (gamma - 0.5) * v2;
  const double sd1 = v * std::sqrt(t1);
  const double sd2 = v * std::sqrt(t2);
  const double e1 = (std::log(s / i1) + drift * t1) / sd1;
  const double e2 = (std::log(i2 * i2 / (s * i1)) + drift * t1) / sd1;
  const double e3 = (std::log(s / i1) - drift * t1) / sd1;
  const double e4 = (std::log(i2 * i2 / (s * i1)) - drift * t1) / sd1;
  const double f1 = (std::log(s / h) + drift * t2) / sd2;
  const double f2 = (std::log(i2 * i2 / (s * h)) + drift * t2) / sd2;
  const double f3 = (std::log(i1 * i1 / (s * h)) + drift * t2) / sd2;
  const double f4 = (std::log(s * i1 * i1 / (h * i2 * i2)) + drift * t2) / sd2;
  const double rho = std::sqrt(t1 / t2);
  const double lambda = -r + gamma * b + 0.5 * gamma * (gamma - 1) * v2;
  const double kappa = 2 * b / v2 + (2 * gamma - 1);
  return std::exp(lambda * t2) * std::pow(s, gamma) *
         (bivariate_norm_cdf(-e1, -f1, rho) - std::pow(i2 / s, kappa) * bivariate_norm_cdf(-e2, -f2, rho) -
          std::pow(i1 / s, kappa) * bivariate_norm_cdf(-e3, -f3, -rho) +
          std::pow(i1 / i2, kappa) * bivariate_norm_cdf(-e4, -f4, -rho));
}

double bs2002_call(double s, double k, double t, double r, double b, double v) noexcept {
  if (b >= r) return carry_call(s, k, t, r, b, v);  // never optimal to exercise early
  const double v2 = v * v;
  const double t1 = 0.5 * (std::sqrt(5.0) - 1) * t;
  const double beta = (0.5 - b / v2) + std::sqrt((b / v2 - 0.5) * (b / v2 - 0.5) + 2 * r / v2);
  const double infinity = beta / (beta - 1) * k;
  const double b0 = std::max(k, r / (r - b) * k);
  const double ht1 = -(b * t1 + 2 * v * std::sqrt(t1)) * k * k / ((infinity - b0) * b0);
  const double ht2 = -(b * t + 2 * v * std::sqrt(t)) * k * k / ((infinity - b0) * b0);
  const double i1 = b0 + (infinity - b0) * (1 - std::exp(ht1));
  const double i2 = b0 + (infinity - b0) * (1 - std::exp(ht2));
  const double alpha1 = (i1 - k) * std::pow(i1, -beta);
  const double alpha2 = (i2 - k) * std::pow(i2, -beta);
  if (s >= i2) return s - k;
  return alpha2 * std::pow(s, beta) - alpha2 * phi(s, t1, beta, i2, i2, r, b, v) +
         phi(s, t1, 1, i2, i2, r, b, v) - phi(s, t1, 1, i1, i2, r, b, v) -
         k * phi(s, t1, 0, i2, i2, r, b, v) + k * phi(s, t1, 0, i1, i2, r, b, v) +
         alpha1 * phi(s, t1, beta, i1, i2, r, b, v) - alpha1 * psi(s, t, beta, i1, i2, i1, t1, r, b, v) +
         psi(s, t, 1, i1, i2, i1, t1, r, b, v) - psi(s, t, 1, k, i2, i1, t1, r, b, v) -
         k * psi(s, t, 0, i1, i2, i1, t1, r, b, v) + k * psi(s, t, 0, k, i2, i1, t1, r, b, v);
}

bool usable(const BsmInputs& in) noexcept {
  return std::isfinite(in.spot) && std::isfinite(in.strike) && std::isfinite(in.expiry) &&
         std::isfinite(in.rate) && std::isfinite(in.dividend) && std::isfinite(in.vol) && in.spot > 0 &&
         in.strike > 0 && in.expiry > 0 && in.vol > 0;
}

/// Early exercise cannot pay: the American price is the European one.
bool european_only(const BsmInputs& in) noexcept {
  return in.type == OptionType::Call ? in.dividend <= 0 && in.rate >= 0 : in.rate <= 0 && in.dividend >= 0;
}

/// European and intrinsic values are both lower bounds on the American price;
/// so is BS2002's flat-boundary strategy, which for short, volatile options can
/// fall below the European value. Report the largest of them.
double checked(const BsmInputs& in, double price) noexcept {
  if (!std::isfinite(price)) return kNaN;
  return std::max({price, bsm_price(in), omega(in.type) * (in.spot - in.strike), 0.0});
}

}  // namespace

std::string_view to_string(AmericanModel model) noexcept {
  switch (model) {
    case AmericanModel::LeisenReimer: return "leisen_reimer";
    case AmericanModel::BjerksundStensland2002: return "bjerksund_stensland_2002";
    case AmericanModel::BaroneAdesiWhaley: return "barone_adesi_whaley";
  }
  return "leisen_reimer";
}

std::optional<AmericanModel> parse_american_model(std::string_view text) noexcept {
  if (text == "leisen_reimer" || text == "lr") return AmericanModel::LeisenReimer;
  if (text == "bjerksund_stensland_2002" || text == "bs2002") return AmericanModel::BjerksundStensland2002;
  if (text == "barone_adesi_whaley" || text == "baw") return AmericanModel::BaroneAdesiWhaley;
  return std::nullopt;
}

double bjerksund_stensland_price(const BsmInputs& in) noexcept {
  if (!usable(in)) return kNaN;
  if (european_only(in)) return bsm_price(in);
  const double b = in.rate - in.dividend;
  const double price = in.type == OptionType::Call
                           ? bs2002_call(in.spot, in.strike, in.expiry, in.rate, b, in.vol)
                           : bs2002_call(in.strike, in.spot, in.expiry, in.rate - b, -b, in.vol);
  return checked(in, price);
}

double barone_adesi_whaley_price(const BsmInputs& in) noexcept {
  if (!usable(in)) return kNaN;
  if (european_only(in)) return bsm_price(in);
  const double r = in.rate, b = r - in.dividend, v = in.vol, t = in.expiry, x = in.strike;
  // The quadratic's coefficient 1 - exp(-rT) must be positive.
  if (!(r > 0)) return kNaN;
  const double v2 = v * v;
  const double sd = v * std::sqrt(t);
  const double n = 2 * b / v2;
  const double m = 2 * r / v2;
  const double kk = 1 - std::exp(-r * t);
  const double carry = std::exp((b - r) * t);
  const bool call = in.type == OptionType::Call;
  const double root = std::sqrt((n - 1) * (n - 1) + 4 * m / kk);
  const double q = call ? (-(n - 1) + root) / 2 : (-(n - 1) - root) / 2;
  const double root_inf = std::sqrt((n - 1) * (n - 1) + 4 * m);
  const double q_inf = call ? (-(n - 1) + root_inf) / 2 : (-(n - 1) - root_inf) / 2;
  const double s_inf = x / (1 - 1 / q_inf);
  double si = call ? x + (s_inf - x) * (1 - std::exp(-(b * t + 2 * sd) * x / (s_inf - x)))
                   : s_inf + (x - s_inf) * std::exp((b * t - 2 * sd) * x / (x - s_inf));
  const BsmInputs at{in.type, 0, x, t, r, in.dividend, v};
  auto european = [&](double s) {
    auto inputs = at;
    inputs.spot = s;
    return bsm_price(inputs);
  };
  // Newton's method on the smooth-pasting condition (Haug 2007, 3.1.3).
  bool converged = false;
  for (int i = 0; i < 100 && std::isfinite(si) && si > 0; ++i) {
    const double d1 = (std::log(si / x) + (b + v2 / 2) * t) / sd;
    const double w = call ? 1.0 : -1.0;
    const double lhs = w * (si - x);
    const double rhs = european(si) + w * (1 - carry * norm_cdf(w * d1)) * si / q;
    if (std::abs(lhs - rhs) <= 1e-12 * x) { converged = true; break; }
    const double slope = call ? carry * norm_cdf(d1) * (1 - 1 / q) + (1 - carry * norm_pdf(d1) / sd) / q
                              : -carry * norm_cdf(-d1) * (1 - 1 / q) - (1 + carry * norm_pdf(-d1) / sd) / q;
    si = call ? (x + rhs - slope * si) / (1 - slope) : (x - rhs + slope * si) / (1 + slope);
  }
  if (!converged || !(si > 0)) return kNaN;
  const double d1 = (std::log(si / x) + (b + v2 / 2) * t) / sd;
  double price = 0;
  if (call) {
    const double a2 = si / q * (1 - carry * norm_cdf(d1));
    price = in.spot < si ? european(in.spot) + a2 * std::pow(in.spot / si, q) : in.spot - x;
  } else {
    const double a1 = -si / q * (1 - carry * norm_cdf(-d1));
    price = in.spot > si ? european(in.spot) + a1 * std::pow(in.spot / si, q) : x - in.spot;
  }
  return checked(in, price);
}

double early_exercise_premium(const BsmInputs& in, AmericanModel model, int steps,
                              std::span<const CashDividend> dividends, AmericanModel* used) {
  auto lattice = [&] {
    if (used) *used = AmericanModel::LeisenReimer;
    return binomial_early_exercise_premium(in, steps, dividends);
  };
  if (model == AmericanModel::LeisenReimer) return lattice();
  // Only cash strictly inside the option's life changes its value.
  for (const auto& d : dividends)
    if (!(d.time <= 0 || d.time >= in.expiry || d.amount == 0)) return lattice();
  if (!usable(in) || !(in.rate > 0)) return lattice();
  if (european_only(in)) {
    if (used) *used = model;
    return 0;
  }
  const double american = model == AmericanModel::BjerksundStensland2002 ? bjerksund_stensland_price(in)
                                                                         : barone_adesi_whaley_price(in);
  const double premium = american - bsm_price(in);
  if (!std::isfinite(premium)) return lattice();
  if (used) *used = model;
  return std::max(premium, 0.0);
}

}  // namespace openport::pricing
