#include "openport/analytics/svi.hpp"

#include "svi_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>

namespace openport::analytics {
namespace {

constexpr double kRhoLimit = 1.0 - 1e-7;
constexpr int kGridSteps = 2000;
using Vec = std::array<double, 3>;
using Mat = std::array<Vec, 3>;
using Datum = detail::SviDatum;
struct Plane { Vec normal; double bound; };  // normal . x <= bound
struct Candidate {
  double m = 0, log_sigma = 0;
  Vec x{};  // a, d, c
  double loss = std::numeric_limits<double>::infinity();
  bool converged = false;
};

double dot(const Vec& a, const Vec& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// Pivoted small KKT solve. Extended intermediates protect narrow smiles from
/// cancellation in the normal equations without adding a numerical dependency.
bool solve(std::array<std::array<long double, 7>, 6> a, int n,
           std::array<double, 6>& result) {
  for (int j = 0; j < n; ++j) {
    int pivot = j;
    for (int i = j + 1; i < n; ++i)
      if (std::abs(a[i][j]) > std::abs(a[pivot][j])) pivot = i;
    if (std::abs(a[pivot][j]) < 1e-18L) return false;
    std::swap(a[j], a[pivot]);
    const auto scale = a[j][j];
    for (int k = j; k <= n; ++k) a[j][k] /= scale;
    for (int i = 0; i < n; ++i) {
      if (i == j) continue;
      const auto factor = a[i][j];
      for (int k = j; k <= n; ++k) a[i][k] -= factor * a[j][k];
    }
  }
  for (int i = 0; i < n; ++i) {
    result[i] = static_cast<double>(a[i][n]);
    if (!std::isfinite(result[i])) return false;
  }
  return true;
}

/// Primal active-set QP: start from a feasible constant smile; add blocking
/// planes and remove constraints with negative multipliers. No parameter clipping.
std::optional<Vec> quadratic(const Mat& h, const Vec& q, const std::vector<Plane>& planes) {
  Vec x{q[0], 0, 0};
  std::vector<std::size_t> active;
  for (int iteration = 0; iteration < 100; ++iteration) {
    std::array<std::array<long double, 7>, 6> system{};
    const int n = 3 + static_cast<int>(active.size());
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) system[i][j] = h[i][j];
      system[i][n] = q[i] - dot(h[i], x);
      for (std::size_t j = 0; j < active.size(); ++j) {
        system[i][3 + j] = planes[active[j]].normal[i];
        system[3 + j][i] = planes[active[j]].normal[i];
      }
    }
    std::array<double, 6> solution{};
    if (!solve(system, n, solution)) return std::nullopt;
    const Vec direction{solution[0], solution[1], solution[2]};
    if (dot(direction, direction) <= 1e-24 * (1 + dot(x, x))) {
      double smallest = -1e-12;
      std::size_t remove = active.size();
      for (std::size_t j = 0; j < active.size(); ++j) {
        if (solution[3 + j] < smallest) {
          smallest = solution[3 + j];
          remove = j;
        }
      }
      if (remove == active.size()) return x;
      active.erase(active.begin() + static_cast<std::ptrdiff_t>(remove));
      continue;
    }
    double step = 1;
    std::size_t blocker = planes.size();
    for (std::size_t j = 0; j < planes.size(); ++j) {
      if (std::find(active.begin(), active.end(), j) != active.end()) continue;
      const double movement = dot(planes[j].normal, direction);
      if (movement <= 1e-14) continue;
      const double limit = std::max(0.0, (planes[j].bound - dot(planes[j].normal, x)) / movement);
      if (limit < step) { step = limit; blocker = j; }
    }
    for (int i = 0; i < 3; ++i) x[i] += step * direction[i];
    if (blocker != planes.size()) {
      if (active.size() == 3) return std::nullopt;
      active.push_back(blocker);
    }
  }
  return std::nullopt;
}

std::optional<Vec> inner(const Mat& h, const Vec& q, double wing_cap) {
  std::vector<Plane> planes{{{0, 1, -kRhoLimit}, 0}, {{0, -1, -kRhoLimit}, 0},
                            {{0, 1, 1}, wing_cap}, {{0, -1, 1}, wing_cap},
                            {{-1, 0, -1}, 0}};
  // w(y)>=0 is an intersection of half-spaces. Add a supporting plane at the
  // analytic minimum until the true curved constraint is met; negative a is allowed.
  for (int cut = 0; cut < 32; ++cut) {
    const auto x = quadratic(h, q, planes);
    if (!x) return std::nullopt;
    const auto [a, d, c] = *x;
    const double root = std::sqrt(std::max(0.0, c * c - d * d));
    if (a + root >= -1e-12 && c >= -1e-12) {
      Vec result = *x;
      // Only remove sub-picovariance numerical residuals on active boundaries.
      if (c < 0) result[2] = result[1] = 0;
      result[0] = std::max(result[0], -std::sqrt(std::max(0.0,
          result[2] * result[2] - result[1] * result[1])));
      return result;
    }
    if (!(root > 0)) return std::nullopt;
    planes.push_back({{-1, d / root, -c / root}, 0});
  }
  return std::nullopt;
}

Candidate evaluate(double m, double log_sigma, const std::vector<Datum>& data,
                   double span, double center) {
  Candidate result;
  result.m = m;
  result.log_sigma = log_sigma;
  const double sigma = std::exp(log_sigma);
  if (std::abs(m - center) > 2 * span || sigma < span * 1e-4 || sigma > span * 5)
    return result;
  Mat h{};
  Vec q{};
  for (const auto& p : data) {
    const double y = (p.k - m) / sigma;
    const Vec row{1, y, std::hypot(y, 1.0)};
    for (int i = 0; i < 3; ++i) {
      q[i] += p.weight * row[i] * p.w;
      for (int j = 0; j < 3; ++j) h[i][j] += p.weight * row[i] * row[j];
    }
  }
  const auto x = inner(h, q, sigma * 2.0);
  if (!x) return result;
  result.x = *x;
  result.loss = 0;
  for (const auto& p : data) {
    const double y = (p.k - m) / sigma;
    const double residual = dot(*x, {1, y, std::hypot(y, 1.0)}) - p.w;
    result.loss += p.weight * residual * residual;
  }
  return result;
}

Candidate refine(Candidate seed, const std::vector<Datum>& data,
                 double span, double center) {
  auto at = [&](double m, double s) { return evaluate(m, s, data, span, center); };
  std::array<Candidate, 3> simplex{seed, at(seed.m + span * .07, seed.log_sigma),
                                at(seed.m, seed.log_sigma + .15)};
  for (int iteration = 0; iteration < 400; ++iteration) {
    std::stable_sort(simplex.begin(), simplex.end(), [](const auto& a, const auto& b) {
      return a.loss < b.loss;
    });
    const auto& best = simplex[0];
    const auto& worst = simplex[2];
    if (std::isfinite(best.loss) &&
        std::max(std::abs(worst.m - best.m), std::abs(simplex[1].m - best.m)) < span * 1e-8 &&
        std::max(std::abs(worst.log_sigma - best.log_sigma),
                 std::abs(simplex[1].log_sigma - best.log_sigma)) < 1e-8) {
      simplex[0].converged = true;
      return simplex[0];
    }
    const double cm = (best.m + simplex[1].m) / 2;
    const double cs = (best.log_sigma + simplex[1].log_sigma) / 2;
    const auto reflected = at(2 * cm - worst.m, 2 * cs - worst.log_sigma);
    if (reflected.loss < best.loss) {
      const auto expanded = at(3 * cm - 2 * worst.m, 3 * cs - 2 * worst.log_sigma);
      simplex[2] = expanded.loss < reflected.loss ? expanded : reflected;
    } else if (reflected.loss < simplex[1].loss) {
      simplex[2] = reflected;
    } else {
      const bool outside = reflected.loss < worst.loss;
      const auto contracted = at(cm + (outside ? .5 : -.5) * (cm - worst.m),
                                 cs + (outside ? .5 : -.5) * (cs - worst.log_sigma));
      if (contracted.loss < (outside ? reflected.loss : worst.loss)) simplex[2] = contracted;
      else for (int j = 1; j < 3; ++j)
        simplex[j] = at((best.m + simplex[j].m) / 2, (best.log_sigma + simplex[j].log_sigma) / 2);
    }
  }
  return *std::min_element(simplex.begin(), simplex.end(), [](const auto& a, const auto& b) {
    return a.loss < b.loss;
  });
}

}  // namespace

std::vector<detail::SviDatum> detail::prepare_svi(std::span<const SviPoint> points, double years) {
  std::vector<Datum> data;
  for (const auto& p : points) {
    if (!std::isfinite(p.k) || !std::isfinite(p.iv) || !std::isfinite(p.bid_iv) ||
        !std::isfinite(p.ask_iv) || !(p.bid_iv > 0) || p.ask_iv < p.bid_iv ||
        p.iv < p.bid_iv || p.iv > p.ask_iv) continue;
    const double w = p.iv * p.iv * years;
    if (!std::isfinite(w) || !(w > 0)) continue;
    const double spread = std::max(1e-4, p.ask_iv - p.bid_iv);
    data.push_back({p.k, p.iv, w, 1 / (spread * spread), 50 * (p.ask_iv - p.bid_iv)});
  }
  std::sort(data.begin(), data.end(), [](const auto& a, const auto& b) {
    return std::tie(a.k, a.iv, a.weight) < std::tie(b.k, b.iv, b.weight);
  });
  if (data.empty()) return data;
  std::vector<double> weights;
  for (const auto& p : data) weights.push_back(p.weight);
  std::sort(weights.begin(), weights.end());
  double cap = 4 * weights[weights.size() / 2];
  // Water-filling cap bounds an individual point's share even in a sparse smile.
  for (int i = 0; i < 80; ++i) {
    double sum = 0;
    for (double weight : weights) sum += std::min(weight, cap);
    if (cap <= .25 * sum) break;
    cap = .25 * sum;
  }
  double sum = 0;
  for (auto& p : data) { p.weight = std::min(p.weight, cap); sum += p.weight; }
  for (auto& p : data) p.weight /= sum;
  return data;
}

const char* to_string(SviStatus status) {
  switch (status) {
    case SviStatus::Ok: return "ok";
    case SviStatus::TooFewPoints: return "too_few_points";
    case SviStatus::Failed: return "failed";
  }
  return "failed";
}

double svi_variance(const SviParameters& p, double k) {
  const double x = k - p.m;
  return p.a + p.b * (p.rho * x + std::hypot(x, p.sigma));
}

double svi_iv(const SviParameters& p, double k, double years) {
  const double w = svi_variance(p, k);
  return years > 0 && std::isfinite(years) && w >= 0 ? std::sqrt(w / years) : kNaN;
}

bool svi_admissible(const SviParameters& p, double years) {
  const double minimum = p.a + p.b * p.sigma * std::sqrt(1 - p.rho * p.rho);
  return std::isfinite(minimum) && std::isfinite(p.a) && std::isfinite(p.b) && std::isfinite(p.rho) &&
         std::isfinite(p.m) && std::isfinite(p.sigma) && std::isfinite(years) &&
         years > 0 && p.b >= 0 && std::abs(p.rho) < 1 && p.sigma > 0 &&
         minimum >= -1e-12 &&
         p.b * (1 + std::abs(p.rho)) <= 2.0 + 1e-12;
}

double svi_density(const SviParameters& p, double k) {
  const double w = svi_variance(p, k);
  if (!(w > 0) || !(p.sigma > 0) || !std::isfinite(w)) return kNaN;
  const double x = k - p.m;
  const double h = std::hypot(x, p.sigma);
  const double first = p.b * (p.rho + x / h);
  const double second = p.b * (p.sigma / h) * (p.sigma / h) / h;
  const double term = 1 - k * first / (2 * w);
  return term * term - first * first / 4 * (1 / w + .25) + second / 2;
}

SviButterfly svi_butterfly(const SviParameters& p, double min_k, double max_k) {
  SviButterfly result;
  if (!std::isfinite(min_k) || !std::isfinite(max_k) || min_k > max_k) return result;
  result.min_g = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= kGridSteps; ++i) {
    const double k = min_k + (max_k - min_k) * i / kGridSteps;
    const double g = svi_density(p, k);
    if (!std::isfinite(g)) return {kNaN, k, false};
    if (g < result.min_g) { result.min_g = g; result.k = k; }
  }
  result.ok = result.min_g >= 0;
  return result;
}

SviFit fit_svi(std::span<const SviPoint> points, double years) {
  const auto start = std::chrono::steady_clock::now();
  SviFit fit;
  fit.years = years;
  auto finish = [&] {
    fit.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return fit;
  };
  if (!(years > 0) || !std::isfinite(years)) {
    fit.reason = "time to expiry must be finite and positive";
    return finish();
  }
  const auto data = detail::prepare_svi(points, years);
  for (const auto& p : data) {
    if (!fit.half_spreads.empty() && fit.half_spreads.back().k == p.k)
      fit.half_spreads.back().vol_points = std::max(fit.half_spreads.back().vol_points, p.half_spread);
    else fit.half_spreads.push_back({p.k, p.half_spread});
  }
  fit.points = data.size();
  std::size_t distinct = 0;
  for (std::size_t i = 0; i < data.size(); ++i)
    if (i == 0 || data[i].k != data[i - 1].k) ++distinct;
  if (distinct < 5) {
    fit.status = SviStatus::TooFewPoints;
    fit.reason = "need at least five distinct two-sided OTM IV points";
    return finish();
  }
  fit.min_k = data.front().k;
  fit.max_k = data.back().k;
  const double span = fit.max_k - fit.min_k;
  const double center = (fit.min_k + fit.max_k) / 2;
  if (span < 1e-5 || !std::isfinite(span)) {
    fit.reason = "log-moneyness range is too narrow or non-finite";
    return finish();
  }
  std::vector<Candidate> seeds;
  for (double offset : {-.5, -.25, 0.0, .25, .5})
    for (double width : {.05, .15, .4, 1.0})
      seeds.push_back(evaluate(center + offset * span, std::log(width * span), data, span, center));
  std::stable_sort(seeds.begin(), seeds.end(), [](const auto& a, const auto& b) { return a.loss < b.loss; });
  Candidate best;
  for (std::size_t i = 0; i < 4; ++i) {
    if (!std::isfinite(seeds[i].loss)) continue;
    const auto candidate = refine(seeds[i], data, span, center);
    if (candidate.converged && candidate.loss < best.loss) best = candidate;
  }
  if (!std::isfinite(best.loss)) {
    fit.reason = "constrained least squares or outer search did not converge";
    return finish();
  }
  const auto [a, d, c] = best.x;
  const double sigma = std::exp(best.log_sigma);
  const SviParameters parameters{a, c / sigma, c > 0 ? d / c : 0, best.m, sigma};
  if (!svi_admissible(parameters, years)) {
    fit.reason = "solution failed parameter admissibility checks";
    return finish();
  }
  double vol_error = 0;
  for (const auto& p : data) {
    const double residual = svi_iv(parameters, p.k, years) - p.iv;
    vol_error += p.weight * residual * residual;
  }
  fit.parameters = parameters;
  fit.rmse_vol_points = 100 * std::sqrt(vol_error);
  fit.butterfly = svi_butterfly(parameters, fit.min_k, fit.max_k);
  fit.status = SviStatus::Ok;
  return finish();
}

SviFit fit_svi(const SliceMetrics& slice) {
  const auto start = std::chrono::steady_clock::now();
  auto finish = [&](SviFit result) {
    result.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return result;
  };
  if (!(slice.forward.forward > 0) || !std::isfinite(slice.forward.forward)) {
    SviFit result;
    result.years = slice.years;
    result.reason = "forward must be finite and positive";
    return finish(result);
  }
  return finish(fit_svi(detail::svi_points(slice), slice.years));
}

std::vector<SviPoint> detail::svi_points(const SliceMetrics& slice) {
  if (!(slice.forward.forward > 0) || !std::isfinite(slice.forward.forward)) return {};
  std::vector<SviPoint> points;
  for (const auto& row : slice.strikes) {
    const auto& otm = row.strike >= slice.forward.forward ? row.call : row.put;
    if (!(row.strike > 0) || !std::isfinite(row.strike) || !(otm.bid > 0) ||
        !std::isfinite(otm.bid) || !std::isfinite(otm.ask) || otm.ask < otm.bid) continue;
    points.push_back({std::log(row.strike / slice.forward.forward), row.iv, otm.bid_iv, otm.ask_iv});
  }
  return points;
}

namespace {
double half_spread_at(const std::vector<SviHalfSpread>& quotes, double k) {
  if (quotes.empty()) return 0;
  const auto hi = std::lower_bound(quotes.begin(), quotes.end(), k,
      [](const auto& quote, double x) { return quote.k < x; });
  if (hi == quotes.begin()) return hi->vol_points;
  if (hi == quotes.end()) return quotes.back().vol_points;
  const auto& lo = *(hi - 1);
  return std::lerp(lo.vol_points, hi->vol_points, (k - lo.k) / (hi->k - lo.k));
}
}  // namespace

std::vector<SviCalendarViolation> detail::calendar(std::vector<CalendarSlice> slices) {
  std::stable_sort(slices.begin(), slices.end(), [](const auto& a, const auto& b) { return a.years < b.years; });
  std::vector<SviCalendarViolation> result;
  for (std::size_t j = 1; j < slices.size(); ++j) {
    const auto& earlier = slices[j - 1];
    const auto& later = slices[j];
    const double lo = std::max(earlier.min_k, later.min_k);
    const double hi = std::min(earlier.max_k, later.max_k);
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo > hi) continue;
    const double floor = std::max({0.1, earlier.rmse_vol_points, later.rmse_vol_points});
    double worst = 0;
    double used_tolerance = floor;
    double location = kNaN;
    for (int i = 0; i <= kGridSteps; ++i) {
      const double k = lo + (hi - lo) * i / kGridSteps;
      // Express both total variances at the later tenor, so the tolerance is
      // an IV increase there, not a comparison of the two annualized smiles.
      const double increase = 100 * (earlier.iv(k, later.years) - later.iv(k, later.years));
      const double tolerance = std::max({floor, half_spread_at(*earlier.half_spreads, k),
                                         half_spread_at(*later.half_spreads, k)});
      // Do not turn roundoff at the tolerance boundary into a violation.
      if (increase > tolerance + 1e-12 && increase > worst) {
        worst = increase; location = k; used_tolerance = tolerance;
      }
    }
    if (std::isfinite(location)) result.push_back({earlier.index, later.index, location, worst, used_tolerance});
  }
  return result;
}

std::vector<SviCalendarViolation> svi_calendar(std::span<const SviFit> fits) {
  std::vector<detail::CalendarSlice> slices;
  for (std::size_t i = 0; i < fits.size(); ++i) {
    const auto& fit = fits[i];
    if (fit.status != SviStatus::Ok) continue;
    slices.push_back({i, fit.years, fit.min_k, fit.max_k, fit.rmse_vol_points, &fit.half_spreads,
                      [&fit](double k, double years) { return svi_iv(fit.parameters, k, years); }});
  }
  return detail::calendar(std::move(slices));
}

}  // namespace openport::analytics
