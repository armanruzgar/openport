#include "openport/pricing/normal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace openport::pricing {
namespace {

struct Rule {
  std::array<double, 10> x;
  std::array<double, 10> w;
  int size;
};

// Half of the 6, 12 and 20 point Gauss-Legendre rules on [-1, 1].
constexpr Rule kSix{{-0.9324695142031522, -0.6612093864662647, -0.2386191860831970},
                    {0.1713244923791705, 0.3607615730481384, 0.4679139345726904}, 3};
constexpr Rule kTwelve{{-0.9815606342467191, -0.9041172563704750, -0.7699026741943050,
                        -0.5873179542866171, -0.3678314989981802, -0.1252334085114692},
                       {0.04717533638651177, 0.1069393259953183, 0.1600783285433464,
                        0.2031674267230659, 0.2334925365383547, 0.2491470458134029}, 6};
constexpr Rule kTwenty{{-0.9931285991850949, -0.9639719272779138, -0.9122344282513259,
                        -0.8391169718222188, -0.7463319064601508, -0.6360536807265150,
                        -0.5108670019508271, -0.3737060887154196, -0.2277858511416451,
                        -0.07652652113349733},
                       {0.01761400713915212, 0.04060142980038694, 0.06267204833410906,
                        0.08327674157670475, 0.1019301198172404, 0.1181945319615184,
                        0.1316886384491766, 0.1420961093183821, 0.1491729864726037,
                        0.1527533871307259}, 10};

/// Genz's BVND: P(X > h, Y > k) with correlation r.
double upper(double h, double k, double r) noexcept {
  const auto& rule = std::abs(r) < 0.3 ? kSix : std::abs(r) < 0.75 ? kTwelve : kTwenty;
  constexpr double two_pi = 2 * std::numbers::pi;
  double hk = h * k;
  double bvn = 0;
  if (std::abs(r) < 0.925) {
    if (r != 0) {
      const double hs = (h * h + k * k) / 2;
      const double asr = std::asin(r);
      for (int i = 0; i < rule.size; ++i) {
        for (double side : {-1.0, 1.0}) {
          const double sn = std::sin(asr * (side * rule.x[i] + 1) / 2);
          bvn += rule.w[i] * std::exp((sn * hk - hs) / (1 - sn * sn));
        }
      }
      bvn *= asr / (2 * two_pi);
    }
    return bvn + norm_cdf(-h) * norm_cdf(-k);
  }
  if (r < 0) {
    k = -k;
    hk = -hk;
  }
  if (std::abs(r) < 1) {
    const double as = (1 - r) * (1 + r);
    double a = std::sqrt(as);
    const double bs = (h - k) * (h - k);
    const double c = (4 - hk) / 8;
    const double d = (12 - hk) / 16;
    double asr = -(bs / as + hk) / 2;
    if (asr > -100) bvn = a * std::exp(asr) * (1 - c * (bs - as) * (1 - d * bs / 5) / 3 + c * d * as * as / 5);
    if (-hk < 100) {
      const double b = std::sqrt(bs);
      bvn -= std::exp(-hk / 2) * std::sqrt(two_pi) * norm_cdf(-b / a) * b * (1 - c * bs * (1 - d * bs / 5) / 3);
    }
    a /= 2;
    for (int i = 0; i < rule.size; ++i) {
      for (double side : {-1.0, 1.0}) {
        const double xs = (a * (side * rule.x[i] + 1)) * (a * (side * rule.x[i] + 1));
        const double rs = std::sqrt(1 - xs);
        asr = -(bs / xs + hk) / 2;
        if (asr > -100)
          bvn += a * rule.w[i] * std::exp(asr) *
                 (std::exp(-hk * (1 - rs) / (2 * (1 + rs))) / rs - (1 + c * xs * (1 + d * xs)));
      }
    }
    bvn = -bvn / two_pi;
  }
  if (r > 0) return bvn + norm_cdf(-std::max(h, k));
  bvn = -bvn;
  if (k > h) bvn += h < 0 ? norm_cdf(k) - norm_cdf(h) : norm_cdf(-h) - norm_cdf(-k);
  return bvn;
}

}  // namespace

double bivariate_norm_cdf(double x, double y, double rho) noexcept {
  if (std::isnan(x) || std::isnan(y) || !(rho >= -1 && rho <= 1))
    return std::numeric_limits<double>::quiet_NaN();
  return std::clamp(upper(-x, -y, rho), 0.0, 1.0);
}

}  // namespace openport::pricing
