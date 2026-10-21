#include "openport/analytics/sabr.hpp"

#include "svi_data.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

namespace openport::analytics {
namespace {

constexpr double kRhoLimit = 1.0 - 1e-7;
constexpr int kGridSteps = 2000;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
using Datum = detail::SviDatum;

/// Hagan's expansion with alpha scaled to a = alpha * F^(beta - 1): every factor
/// of F cancels, leaving a function of k = ln(K/F) alone.
double scaled_iv(double a, double beta, double rho, double nu, double k, double years) {
  const double omb = 1 - beta;
  const double shift = std::exp(k * omb / 2);  // (F K)^((1 - beta) / 2) / F^(1 - beta)
  const double k2 = k * k;
  const double lead = a / (shift * (1 + omb * omb / 24 * k2 + omb * omb * omb * omb / 1920 * k2 * k2));
  const double z = -nu / a * shift * k;  // nu / alpha (F K)^((1 - beta) / 2) ln(F / K)
  double ratio = 1;                      // z / x(z)
  if (std::abs(z) < 1e-6) {
    ratio = 1 - rho * z / 2 + (2 - 3 * rho * rho) * z * z / 12;
  } else {
    const double argument = (std::sqrt(1 - 2 * rho * z + z * z) + z - rho) / (1 - rho);
    if (!(argument > 0) || !std::isfinite(argument)) return kNaN;
    ratio = z / std::log(argument);
  }
  const double correction =
      1 + (omb * omb / 24 * a * a / (shift * shift) + rho * beta * nu * a / (4 * shift) +
           (2 - 3 * rho * rho) / 24 * nu * nu) * years;
  const double iv = lead * ratio * correction;
  return iv > 0 && std::isfinite(iv) ? iv : kNaN;
}

struct Vertex {
  std::vector<double> x;  // log a, rho, log nu[, beta]
  double loss = kInfinity;
};

struct Problem {
  const std::vector<Datum>& data;
  double years;
  std::optional<double> beta;

  double beta_of(const std::vector<double>& x) const { return beta ? *beta : x[3]; }
  Vertex at(std::vector<double> x) const {
    Vertex v{std::move(x), kInfinity};
    const double a = std::exp(v.x[0]), rho = v.x[1], nu = std::exp(v.x[2]), b = beta_of(v.x);
    if (!(a >= 1e-4 && a <= 10) || !(std::abs(rho) <= kRhoLimit) || !(nu >= 1e-4 && nu <= 50) ||
        !(b >= 0 && b <= 1))
      return v;
    double loss = 0;
    for (const auto& p : data) {
      const double iv = scaled_iv(a, b, rho, nu, p.k, years);
      if (!std::isfinite(iv)) return v;
      const double residual = iv * iv * years - p.w;
      loss += p.weight * residual * residual;
    }
    v.loss = loss;
    return v;
  }
};

/// Bounded Nelder-Mead: points outside the box have infinite loss.
std::optional<Vertex> refine(const Problem& problem, const Vertex& seed) {
  const std::size_t n = seed.x.size();
  const std::vector<double> step{0.1, 0.2, 0.3, 0.15};
  std::vector<Vertex> simplex{seed};
  for (std::size_t i = 0; i < n; ++i) {
    auto x = seed.x;
    // Step toward the interior so a seed on a bound still spans the box.
    x[i] += (i == 1 && x[i] > 0) || (i == 3 && x[i] > 0.5) ? -step[i] : step[i];
    simplex.push_back(problem.at(std::move(x)));
  }
  auto by_loss = [](const Vertex& a, const Vertex& b) { return a.loss < b.loss; };
  for (int iteration = 0; iteration < 1000; ++iteration) {
    std::stable_sort(simplex.begin(), simplex.end(), by_loss);
    const auto& best = simplex.front();
    double width = 0;
    double spread = 0;
    for (std::size_t j = 1; j <= n; ++j) {
      for (std::size_t i = 0; i < n; ++i) width = std::max(width, std::abs(simplex[j].x[i] - best.x[i]));
      spread = std::max(spread, simplex[j].loss - best.loss);
    }
    // Flat directions (rho once nu is at its floor, say) leave the loss unchanged.
    if (std::isfinite(best.loss) && (width < 1e-8 || (width < 1e-5 && spread <= 1e-13 * (best.loss + 1e-12))))
      return best;
    std::vector<double> centroid(n, 0.0);
    for (std::size_t j = 0; j < n; ++j)
      for (std::size_t i = 0; i < n; ++i) centroid[i] += simplex[j].x[i] / static_cast<double>(n);
    auto toward = [&](double scale) {
      std::vector<double> x(n);
      for (std::size_t i = 0; i < n; ++i) x[i] = centroid[i] + scale * (simplex[n].x[i] - centroid[i]);
      return problem.at(std::move(x));
    };
    const auto reflected = toward(-1);
    if (reflected.loss < best.loss) {
      const auto expanded = toward(-2);
      simplex[n] = expanded.loss < reflected.loss ? expanded : reflected;
    } else if (reflected.loss < simplex[n - 1].loss) {
      simplex[n] = reflected;
    } else {
      const bool outside = reflected.loss < simplex[n].loss;
      const auto contracted = toward(outside ? -0.5 : 0.5);
      if (contracted.loss < (outside ? reflected.loss : simplex[n].loss)) {
        simplex[n] = contracted;
      } else {
        for (std::size_t j = 1; j <= n; ++j) {
          std::vector<double> x(n);
          for (std::size_t i = 0; i < n; ++i) x[i] = (best.x[i] + simplex[j].x[i]) / 2;
          simplex[j] = problem.at(std::move(x));
        }
      }
    }
  }
  return std::nullopt;
}

/// IV at k = 0, linear between the quotes bracketing it, else the nearest quote's.
double atm_iv(const std::vector<Datum>& data) {
  const auto hi = std::lower_bound(data.begin(), data.end(), 0.0, [](const auto& p, double k) { return p.k < k; });
  if (hi == data.begin()) return hi->iv;
  if (hi == data.end()) return data.back().iv;
  const auto& lo = *(hi - 1);
  return std::lerp(lo.iv, hi->iv, (0 - lo.k) / (hi->k - lo.k));
}

}  // namespace

bool sabr_admissible(const SabrParameters& p) {
  return std::isfinite(p.alpha) && std::isfinite(p.beta) && std::isfinite(p.rho) && std::isfinite(p.nu) &&
         p.alpha > 0 && p.beta >= 0 && p.beta <= 1 && std::abs(p.rho) < 1 && p.nu >= 0;
}

double sabr_iv(const SabrParameters& p, double forward, double k, double years) {
  if (!sabr_admissible(p) || !(forward > 0) || !std::isfinite(forward) || !std::isfinite(k) ||
      !(years > 0) || !std::isfinite(years))
    return kNaN;
  return scaled_iv(p.alpha * std::pow(forward, p.beta - 1), p.beta, p.rho, p.nu, k, years);
}

SviButterfly sabr_butterfly(const SabrParameters& p, double forward, double years, double min_k, double max_k) {
  SviButterfly result;
  if (!std::isfinite(min_k) || !std::isfinite(max_k) || min_k > max_k) return result;
  auto variance = [&](double k) {
    const double iv = sabr_iv(p, forward, k, years);
    return iv * iv * years;
  };
  constexpr double h = 1e-4;
  result.min_g = kInfinity;
  for (int i = 0; i <= kGridSteps; ++i) {
    const double k = min_k + (max_k - min_k) * i / kGridSteps;
    const double w = variance(k), up = variance(k + h), down = variance(k - h);
    const double first = (up - down) / (2 * h);
    const double second = (up - 2 * w + down) / (h * h);
    const double term = 1 - k * first / (2 * w);
    const double g = term * term - first * first / 4 * (1 / w + .25) + second / 2;
    if (!std::isfinite(g) || !(w > 0)) return {kNaN, k, false};
    if (g < result.min_g) { result.min_g = g; result.k = k; }
  }
  result.ok = result.min_g >= 0;
  return result;
}

SabrFit fit_sabr(std::span<const SviPoint> points, double forward, double years, std::optional<double> beta) {
  const auto start = std::chrono::steady_clock::now();
  SabrFit fit;
  fit.years = years;
  fit.forward = forward;
  fit.beta_calibrated = !beta;
  auto finish = [&] {
    fit.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return fit;
  };
  if (!(years > 0) || !std::isfinite(years)) {
    fit.reason = "time to expiry must be finite and positive";
    return finish();
  }
  if (!(forward > 0) || !std::isfinite(forward)) {
    fit.reason = "forward must be finite and positive";
    return finish();
  }
  if (beta && !(*beta >= 0 && *beta <= 1)) {
    fit.reason = "beta must be between 0 and 1";
    return finish();
  }
  const auto data = detail::prepare_svi(points, years);
  for (const auto& p : data) {
    if (!fit.half_spreads.empty() && fit.half_spreads.back().k == p.k)
      fit.half_spreads.back().vol_points = std::max(fit.half_spreads.back().vol_points, p.half_spread);
    else fit.half_spreads.push_back({p.k, p.half_spread});
  }
  fit.points = data.size();
  if (fit.half_spreads.size() < 5) {
    fit.status = SviStatus::TooFewPoints;
    fit.reason = "need at least five distinct two-sided OTM IV points";
    return finish();
  }
  fit.min_k = data.front().k;
  fit.max_k = data.back().k;
  if (!(fit.max_k - fit.min_k >= 1e-5)) {
    fit.reason = "log-moneyness range is too narrow or non-finite";
    return finish();
  }
  const Problem problem{data, years, beta};
  const double level = std::log(std::clamp(atm_iv(data), 1.1e-4, 9.0));
  std::vector<Vertex> seeds;
  for (double b : beta ? std::vector<double>{*beta} : std::vector<double>{0.25, 0.75})
    for (double rho : {-0.75, -0.4, 0.0, 0.4})
      for (double nu : {0.3, 1.0, 3.0, 8.0}) {
        std::vector<double> x{level, rho, std::log(nu)};
        if (!beta) x.push_back(b);
        seeds.push_back(problem.at(std::move(x)));
      }
  std::stable_sort(seeds.begin(), seeds.end(), [](const auto& a, const auto& b) { return a.loss < b.loss; });
  std::optional<Vertex> best;
  for (std::size_t i = 0; i < 4 && i < seeds.size(); ++i) {
    if (!std::isfinite(seeds[i].loss)) continue;
    const auto candidate = refine(problem, seeds[i]);
    if (candidate && (!best || candidate->loss < best->loss)) best = candidate;
  }
  if (!best) {
    fit.reason = "bounded least squares did not converge";
    return finish();
  }
  const double b = problem.beta_of(best->x);
  const SabrParameters parameters{std::exp(best->x[0]) * std::pow(forward, 1 - b), b, best->x[1],
                                  std::exp(best->x[2])};
  if (!sabr_admissible(parameters)) {
    fit.reason = "solution failed parameter admissibility checks";
    return finish();
  }
  double vol_error = 0;
  for (const auto& p : data) {
    const double residual = sabr_iv(parameters, forward, p.k, years) - p.iv;
    vol_error += p.weight * residual * residual;
  }
  fit.parameters = parameters;
  fit.rmse_vol_points = 100 * std::sqrt(vol_error);
  fit.butterfly = sabr_butterfly(parameters, forward, years, fit.min_k, fit.max_k);
  fit.status = SviStatus::Ok;
  return finish();
}

SabrFit fit_sabr(const SliceMetrics& slice, std::optional<double> beta) {
  const auto start = std::chrono::steady_clock::now();
  auto result = fit_sabr(detail::svi_points(slice), slice.forward.forward, slice.years, beta);
  result.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return result;
}

std::vector<SviCalendarViolation> sabr_calendar(std::span<const SabrFit> fits) {
  std::vector<detail::CalendarSlice> slices;
  for (std::size_t i = 0; i < fits.size(); ++i) {
    const auto& fit = fits[i];
    if (fit.status != SviStatus::Ok) continue;
    // Total variance at k, re-expressed over the comparison tenor.
    slices.push_back({i, fit.years, fit.min_k, fit.max_k, fit.rmse_vol_points, &fit.half_spreads,
                      [&fit](double k, double years) {
                        const double iv = sabr_iv(fit.parameters, fit.forward, k, fit.years);
                        return iv * std::sqrt(fit.years / years);
                      }});
  }
  return detail::calendar(std::move(slices));
}

}  // namespace openport::analytics
