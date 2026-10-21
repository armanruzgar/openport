#include "openport/analytics/heston.hpp"

#include "svi_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <optional>

#include "openport/pricing/black.hpp"
#include "openport/pricing/implied_vol.hpp"
#include "openport/pricing/normal.hpp"

namespace openport::analytics {
namespace {

using Complex = std::complex<double>;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
// 16-point Gauss-Legendre on [-1, 1], positive half.
constexpr std::array<double, 8> kNodes{0.0950125098376374, 0.2816035507792589, 0.4580167776572274,
                                       0.6178762444026438, 0.7554044083550030, 0.8656312023878318,
                                       0.9445750230732326, 0.9894009349916499};
constexpr std::array<double, 8> kWeights{0.1894506104550685, 0.1826034150449236, 0.1691565193950025,
                                         0.1495959888165767, 0.1246289712555339, 0.0951585116824928,
                                         0.0622535239386479, 0.0271524594117541};

/// log(1 + z) / z, by its series where z is small.
Complex log1p_ratio(Complex z) {
  if (std::abs(z) < 1e-4) return 1.0 - z / 2.0 + z * z / 3.0;
  return std::log(1.0 + z) / z;
}

/// E[exp(i u ln(F_T / F_0))] under the forward measure, the "little trap" form.
/// beta - d and the log term are O(xi^2); both are rewritten without that
/// cancellation (beta - d = -xi^2 (i u + u^2) / (beta + d)), so the small vol of
/// vol limit is exact rather than a ratio of roundoff to xi^2.
Complex characteristic(const HestonParameters& p, Complex u, double years) {
  const Complex i(0, 1);
  const Complex a = i * u + u * u;
  const Complex beta = p.kappa - p.rho * p.xi * i * u;
  const Complex d = std::sqrt(beta * beta + p.xi * p.xi * a);
  const Complex sum = beta + d;
  const Complex scaled = -a / sum;  // (beta - d) / xi^2
  const Complex g = scaled * p.xi * p.xi / sum;
  const Complex decay = std::exp(-d * years);
  // log((1 - g e) / (1 - g)) = log1p(z), z = g (1 - e) / (1 - g).
  const Complex z_scaled = scaled / sum * (1.0 - decay) / (1.0 - g);  // z / xi^2
  const Complex log_scaled = z_scaled * log1p_ratio(z_scaled * p.xi * p.xi);
  const Complex c = p.kappa * p.theta * (scaled * years - 2.0 * log_scaled);
  const Complex dd = scaled * (1.0 - decay) / (1.0 - g * decay);
  return std::exp(c + dd * p.v0);
}

struct Grid {
  std::vector<double> u, w;
};

/// Integration nodes and weights on [0, U] for one tenor, for |ln(F/K)| up to
/// `max_x`. The 1/(u^2 + 1/4) peak at zero needs narrow panels, which then
/// double up to a width that holds a few periods of exp(i u x). U covers the
/// Gaussian decay exp(-w u^2 / 2) of a total variance w to e^-50; the
/// exponential tail of a large vol of vol is cut where it is below ~1e-6 of the
/// peak, and the 1/u^2 factor shrinks that further.
Grid grid(double total_variance, double max_x) {
  const double upper = 10 / std::sqrt(std::max(total_variance, 1e-6)) + 30;
  const double cap = std::min(25.0, 8 * std::numbers::pi / std::max(max_x, 0.05));
  Grid result;
  for (double a = 0, width = 0.5; a < upper; width = std::min(2 * width, cap)) {
    const double b = std::min(a + width, upper);
    const double middle = (a + b) / 2, half = (b - a) / 2;
    for (std::size_t j = 0; j < kNodes.size(); ++j)
      for (double side : {-1.0, 1.0}) {
        result.u.push_back(middle + side * kNodes[j] * half);
        result.w.push_back(kWeights[j] * half);
      }
    a = b;
  }
  return result;
}

/// The integrand's tenor part, phi(u - i/2) / (u^2 + 1/4), at each node.
std::vector<Complex> integrand(const HestonParameters& p, const Grid& g, double years) {
  std::vector<Complex> result(g.u.size());
  for (std::size_t n = 0; n < g.u.size(); ++n) {
    const double u = g.u[n];
    result[n] = characteristic(p, Complex(u, -0.5), years) * (g.w[n] / (u * u + 0.25));
  }
  return result;
}

/// Undiscounted call from precomputed tenor terms and exp(i u x) per node.
double call_from(const std::vector<Complex>& terms, const std::vector<Complex>& phases, double forward, double strike) {
  double sum = 0;
  for (std::size_t n = 0; n < terms.size(); ++n) sum += (phases[n] * terms[n]).real();
  return forward - std::sqrt(forward * strike) / std::numbers::pi * sum;
}

std::vector<Complex> phases(const Grid& g, double x) {
  std::vector<Complex> result(g.u.size());
  for (std::size_t n = 0; n < g.u.size(); ++n) result[n] = std::polar(1.0, g.u[n] * x);
  return result;
}

double mean_variance(const HestonParameters& p, double years) {
  const double kt = p.kappa * years;
  return p.theta + (p.v0 - p.theta) * (kt > 1e-8 ? (1 - std::exp(-kt)) / kt : 1.0);
}

struct Quote {
  double k, strike, iv, otm_price, vega, weight;
  std::vector<Complex> phases;
};
struct Tenor {
  std::size_t index;
  double years, forward;
  Grid nodes;
  std::vector<Quote> quotes;
};

struct Vertex {
  std::array<double, 5> x{};  // log v0, log kappa, log theta, log xi, rho
  double loss = kInfinity;
};

HestonParameters from(const std::array<double, 5>& x) {
  return {std::exp(x[0]), std::exp(x[1]), std::exp(x[2]), std::exp(x[3]), x[4]};
}

double loss_of(const std::array<double, 5>& x, const std::vector<Tenor>& tenors) {
  const auto p = from(x);
  if (!(p.v0 >= 1e-4 && p.v0 <= 4) || !(p.kappa >= 0.05 && p.kappa <= 20) || !(p.theta >= 1e-4 && p.theta <= 4) ||
      !(p.xi >= 0.01 && p.xi <= 5) || !(std::abs(p.rho) <= 0.999))
    return kInfinity;
  double loss = 0;
  for (const auto& tenor : tenors) {
    const auto terms = integrand(p, tenor.nodes, tenor.years);
    for (const auto& q : tenor.quotes) {
      const double call = call_from(terms, q.phases, tenor.forward, q.strike);
      const double model = q.k >= 0 ? call : call - (tenor.forward - q.strike);
      const double residual = (model - q.otm_price) / q.vega;
      if (!std::isfinite(residual)) return kInfinity;
      loss += q.weight * residual * residual;
    }
  }
  return loss / static_cast<double>(tenors.size());
}

std::optional<Vertex> refine(Vertex seed, const std::vector<Tenor>& tenors) {
  constexpr std::size_t n = 5;
  constexpr std::array<double, n> step{0.2, 0.4, 0.2, 0.3, 0.2};
  std::array<Vertex, n + 1> simplex{};
  simplex[0] = seed;
  for (std::size_t i = 0; i < n; ++i) {
    auto x = seed.x;
    x[i] += i == 4 && x[i] > 0 ? -step[i] : step[i];
    simplex[i + 1] = {x, loss_of(x, tenors)};
  }
  auto by_loss = [](const Vertex& a, const Vertex& b) { return a.loss < b.loss; };
  for (int iteration = 0; iteration < 3000; ++iteration) {
    std::stable_sort(simplex.begin(), simplex.end(), by_loss);
    const auto& best = simplex[0];
    double width = 0, spread = 0;
    for (std::size_t j = 1; j <= n; ++j) {
      for (std::size_t i = 0; i < n; ++i) width = std::max(width, std::abs(simplex[j].x[i] - best.x[i]));
      spread = std::max(spread, simplex[j].loss - best.loss);
    }
    // Log-parameter widths of 1e-6 are relative changes far below what the data identify.
    if (std::isfinite(best.loss) && (width < 1e-6 || (width < 1e-4 && spread <= 1e-10 * (best.loss + 1e-14))))
      return best;
    std::array<double, n> centroid{};
    for (std::size_t j = 0; j < n; ++j)
      for (std::size_t i = 0; i < n; ++i) centroid[i] += simplex[j].x[i] / n;
    auto toward = [&](double scale) {
      std::array<double, n> x{};
      for (std::size_t i = 0; i < n; ++i) x[i] = centroid[i] + scale * (simplex[n].x[i] - centroid[i]);
      return Vertex{x, loss_of(x, tenors)};
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
          std::array<double, n> x{};
          for (std::size_t i = 0; i < n; ++i) x[i] = (best.x[i] + simplex[j].x[i]) / 2;
          simplex[j] = {x, loss_of(x, tenors)};
        }
      }
    }
  }
  return std::nullopt;
}

}  // namespace

bool heston_admissible(const HestonParameters& p) {
  return std::isfinite(p.v0) && std::isfinite(p.kappa) && std::isfinite(p.theta) && std::isfinite(p.xi) &&
         std::isfinite(p.rho) && p.v0 > 0 && p.kappa > 0 && p.theta > 0 && p.xi > 0 && std::abs(p.rho) < 1;
}

double heston_call(const HestonParameters& p, double forward, double strike, double years) {
  if (!heston_admissible(p) || !(forward > 0) || !(strike > 0) || !(years > 0) || !std::isfinite(forward) ||
      !std::isfinite(strike) || !std::isfinite(years))
    return kNaN;
  const double x = std::log(forward / strike);
  const auto nodes = grid(mean_variance(p, years) * years, std::abs(x));
  const double price = call_from(integrand(p, nodes, years), phases(nodes, x), forward, strike);
  return std::isfinite(price) ? std::clamp(price, std::max(forward - strike, 0.0), forward) : kNaN;
}

double heston_iv(const HestonParameters& p, double forward, double k, double years) {
  const double strike = forward * std::exp(k);
  const double call = heston_call(p, forward, strike, years);
  if (!std::isfinite(call)) return kNaN;
  const auto iv = pricing::implied_vol_black(call, pricing::OptionType::Call, forward, strike, years);
  return iv.ok() ? iv.vol : kNaN;
}

HestonFit fit_heston(std::span<const HestonSlice> slices) {
  const auto start = std::chrono::steady_clock::now();
  HestonFit fit;
  fit.expiries.resize(slices.size());
  auto finish = [&] {
    fit.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return fit;
  };
  std::vector<Tenor> tenors;
  double shortest = kNaN, longest = kNaN, shortest_years = kInfinity, longest_years = 0;
  for (std::size_t s = 0; s < slices.size(); ++s) {
    const auto& slice = slices[s];
    auto& expiry = fit.expiries[s];
    if (!(slice.years >= 7 / 365.0) || !std::isfinite(slice.years)) {
      expiry.reason = "expiry is less than a week away";
      continue;
    }
    if (!(slice.forward > 0) || !std::isfinite(slice.forward)) {
      expiry.reason = "forward must be finite and positive";
      continue;
    }
    auto data = detail::prepare_svi(slice.points, slice.years);
    data.erase(std::unique(data.begin(), data.end(), [](const auto& a, const auto& b) { return a.k == b.k; }), data.end());
    // Far wings carry prices near roundoff and vegas near zero, which a vega-weighted
    // fit would amplify: keep quotes within three ATM standard deviations.
    if (!data.empty()) {
      const double atm = std::min_element(data.begin(), data.end(), [](const auto& a, const auto& b) {
        return std::abs(a.k) < std::abs(b.k);
      })->iv;
      const double reach = 3 * atm * std::sqrt(slice.years);
      std::erase_if(data, [&](const auto& d) { return std::abs(d.k) > reach; });
    }
    if (data.size() < 5) {
      expiry.reason = "need at least five distinct two-sided OTM IV points";
      continue;
    }
    // Thin evenly in k: Heston's few parameters do not need every strike.
    constexpr std::size_t kMaxQuotes = 25;
    std::vector<detail::SviDatum> kept;
    for (std::size_t j = 0; j < std::min(kMaxQuotes, data.size()); ++j)
      kept.push_back(data[j * (data.size() - 1) / (std::min(kMaxQuotes, data.size()) - 1)]);
    double total = 0;
    for (const auto& d : kept) total += d.weight;
    // The ATM level seeds and scales the integration range.
    const auto atm = std::min_element(kept.begin(), kept.end(), [](const auto& a, const auto& b) {
      return std::abs(a.k) < std::abs(b.k);
    })->iv;
    const double widest = std::max(std::abs(kept.front().k), std::abs(kept.back().k));
    Tenor tenor{s, slice.years, slice.forward, grid(atm * atm * slice.years, widest), {}};
    for (const auto& d : kept) {
      const double strike = slice.forward * std::exp(d.k);
      const auto type = d.k >= 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
      const double sd = d.iv * std::sqrt(slice.years);
      const double vega = slice.forward * pricing::norm_pdf(-d.k / sd + sd / 2) * std::sqrt(slice.years);
      if (!(vega > 0)) continue;
      tenor.quotes.push_back({d.k, strike, d.iv, pricing::black_price(type, slice.forward, strike, slice.years, d.iv),
                              vega, d.weight / total, phases(tenor.nodes, -d.k)});
    }
    expiry.points = tenor.quotes.size();
    if (slice.years < shortest_years) { shortest_years = slice.years; shortest = atm; }
    if (slice.years > longest_years) { longest_years = slice.years; longest = atm; }
    tenors.push_back(std::move(tenor));
  }
  for (const auto& t : tenors) fit.points += t.quotes.size();
  if (tenors.size() < 2) {
    fit.status = SviStatus::TooFewPoints;
    fit.reason = "need at least two expiries a week or more away with five usable quotes each";
    return finish();
  }
  std::vector<Vertex> seeds;
  for (double kappa : {0.5, 2.0, 6.0})
    for (double xi : {0.3, 1.0})
      for (double rho : {-0.7, -0.3, 0.2}) {
        const std::array<double, 5> x{std::log(shortest * shortest), std::log(kappa), std::log(longest * longest),
                                      std::log(xi), rho};
        seeds.push_back({x, loss_of(x, tenors)});
      }
  std::stable_sort(seeds.begin(), seeds.end(), [](const auto& a, const auto& b) { return a.loss < b.loss; });
  std::optional<Vertex> best;
  for (std::size_t i = 0; i < 2; ++i) {
    if (!std::isfinite(seeds[i].loss)) continue;
    const auto candidate = refine(seeds[i], tenors);
    if (candidate && (!best || candidate->loss < best->loss)) best = candidate;
  }
  if (!best) {
    fit.reason = "bounded least squares did not converge";
    return finish();
  }
  const auto p = from(best->x);
  // Exact IV residuals: invert each fitted price rather than trusting the vega weighting.
  double sum = 0;
  for (const auto& tenor : tenors) {
    double error = 0;
    for (const auto& q : tenor.quotes) {
      const double iv = heston_iv(p, tenor.forward, q.k, tenor.years);
      if (!std::isfinite(iv)) {
        fit.reason = "a fitted price has no implied volatility";
        return finish();
      }
      error += q.weight * (iv - q.iv) * (iv - q.iv);
    }
    fit.expiries[tenor.index].rmse_vol_points = 100 * std::sqrt(error);
    sum += error;
  }
  fit.parameters = p;
  fit.feller = 2 * p.kappa * p.theta >= p.xi * p.xi;
  fit.rmse_vol_points = 100 * std::sqrt(sum / static_cast<double>(tenors.size()));
  fit.status = SviStatus::Ok;
  return finish();
}

HestonFit fit_heston(std::span<const SliceMetrics> slices) {
  const auto start = std::chrono::steady_clock::now();
  std::vector<HestonSlice> converted;
  for (const auto& slice : slices) converted.push_back({slice.years, slice.forward.forward, detail::svi_points(slice)});
  auto result = fit_heston(converted);
  result.fit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return result;
}

}  // namespace openport::analytics
