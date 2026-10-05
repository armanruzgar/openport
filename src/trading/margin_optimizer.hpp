#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace openport::trading::detail {
// Arithmetic failure is a deterministic work limit, never permission to use an
// uncertified relaxation. All published costs and all pruning use these rationals.
struct MarginWorkLimit {};
__extension__ using MarginWide = __int128;
class MarginRational {
 public:
  MarginRational(std::int64_t n = 0) : n_(n) {}
  MarginRational(MarginWide n, MarginWide d) : n_(n), d_(d) {
    if (d_ <= 0 || n_ == std::numeric_limits<MarginWide>::min()) throw MarginWorkLimit{};
    if (d_ != 1) { const auto g = gcd(n_, d_); n_ /= g; d_ /= g; }
  }
  friend MarginRational operator+(const MarginRational& a, const MarginRational& b) {
    if (a.n_ == 0) return b;
    if (b.n_ == 0) return a;
    if (a.d_ == b.d_) return {sum(a.n_, b.n_), a.d_};
    const auto g = gcd(a.d_, b.d_);
    return {sum(product(a.n_, b.d_ / g), product(b.n_, a.d_ / g)), product(a.d_, b.d_ / g)};
  }
  friend MarginRational operator-(const MarginRational& a, const MarginRational& b) { return a + -b; }
  MarginRational operator-() const { return {-n_, d_}; }
  friend MarginRational operator*(const MarginRational& a, const MarginRational& b) {
    if (a.n_ == 0 || b.n_ == 0) return {};
    if (a.d_ == 1 && b.d_ == 1) return {product(a.n_, b.n_), 1};
    const auto g = gcd(a.n_, b.d_), h = gcd(b.n_, a.d_);
    return {product(a.n_ / g, b.n_ / h), product(a.d_ / h, b.d_ / g)};
  }
  friend MarginRational operator/(const MarginRational& a, const MarginRational& b) {
    if (b.n_ == 0) throw MarginWorkLimit{};
    return a * MarginRational(b.n_ < 0 ? -b.d_ : b.d_, b.n_ < 0 ? -b.n_ : b.n_);
  }
  friend bool operator<(const MarginRational& a, const MarginRational& b) {
    if (a.d_ == b.d_) return a.n_ < b.n_;
    const auto g = gcd(a.d_, b.d_);
    return product(a.n_, b.d_ / g) < product(b.n_, a.d_ / g);
  }
  friend bool operator==(const MarginRational& a, const MarginRational& b) { return a.n_ == b.n_ && a.d_ == b.d_; }
  std::int64_t ceil() const {
    const auto n = n_ / d_ + (n_ % d_ > 0 ? 1 : 0);
    if (n < std::numeric_limits<std::int64_t>::min() || n > std::numeric_limits<std::int64_t>::max()) throw MarginWorkLimit{};
    return static_cast<std::int64_t>(n);
  }
  double approximate() const { return static_cast<double>(n_) / static_cast<double>(d_); }
  // Quantize proposed prices to a common rational grid. A shared denominator
  // prevents irrelevant floating-point noise from creating enormous coprime
  // denominators. Exact feasibility repair below makes rounding direction safe.
  static MarginRational propose(double value) {
    if (!std::isfinite(value) || std::abs(value) > 1e15) throw MarginWorkLimit{};
    const auto whole = std::round(value);
    if (std::abs(value - whole) <= 1e-5 + std::abs(value) * 1e-14)
      return MarginRational(static_cast<std::int64_t>(whole));
    return {static_cast<MarginWide>(std::round(static_cast<long double>(value) * 1000000000.L)), 1000000000};
  }

 private:
  static MarginWide gcd(MarginWide a, MarginWide b) {
    if (a < 0) a = -a;
    while (b) { const auto r = a % b; a = b; b = r; }
    return a;
  }
  static MarginWide product(MarginWide a, MarginWide b) {
    MarginWide result;
    if (__builtin_mul_overflow(a, b, &result)) throw MarginWorkLimit{};
    return result;
  }
  static MarginWide sum(MarginWide a, MarginWide b) {
    MarginWide result;
    if (__builtin_add_overflow(a, b, &result)) throw MarginWorkLimit{};
    return result;
  }
  MarginWide n_ = 0, d_ = 1;
};

// A simplex dictionary proposes a basis and primal/dual prices. It cannot prove
// anything on its own. The caller validates integer primal allocations and exact
// rational dual bounds, including the phase-I infeasibility certificate.
class MarginSimplex {
 public:
  using Matrix = std::vector<std::vector<std::int64_t>>;
  MarginSimplex(const Matrix& a, const std::vector<std::int64_t>& b,
                const std::vector<MarginRational>& c, const std::vector<double>& variable_scale, double quantity_scale, std::size_t& work)
      : m_(b.size()), n_(c.size()), basis_(m_), nonbasis_(n_ + 1),
        d_(m_ + 2, std::vector<double>(n_ + 2)), row_scale_(m_, 1), variable_scale_(variable_scale), objective_scale_(100000000 * quantity_scale), work_(work) {
    spend((m_ + 2) * (n_ + 2));
    for (std::size_t i = 0; i < m_; ++i) {
      for (std::size_t j = 0; j < n_; ++j)
        row_scale_[i] = std::max(row_scale_[i], std::abs(static_cast<double>(a[i][j]) * variable_scale[j]));
      for (std::size_t j = 0; j < n_; ++j) d_[i][j] = static_cast<double>(a[i][j]) * variable_scale[j] / row_scale_[i];
      basis_[i] = n_ + i; d_[i][n_] = -1; d_[i][n_ + 1] = static_cast<double>(b[i]) / row_scale_[i];
    }
    for (std::size_t j = 0; j < n_; ++j) { nonbasis_[j] = j; d_[m_][j] = -c[j].approximate() * variable_scale[j] / objective_scale_; }
    nonbasis_[n_] = artificial; d_[m_ + 1][n_] = 1;
  }
  bool solve() {
    const auto r = static_cast<std::size_t>(std::min_element(d_.begin(), d_.begin() + static_cast<std::ptrdiff_t>(m_),
        [&](const auto& a, const auto& b) { return a[n_ + 1] < b[n_ + 1]; }) - d_.begin());
    if (d_[r][n_ + 1] < -epsilon) {
      pivot(r, n_);
      optimize(m_ + 1);
      if (d_[m_ + 1][n_ + 1] < -epsilon) return false;
      for (std::size_t i = 0; i < m_; ++i) {
        if (basis_[i] != artificial) continue;
        std::size_t s = n_ + 1;
        for (std::size_t j = 0; j <= n_; ++j)
          if (std::abs(d_[i][j]) > epsilon && (s > n_ || nonbasis_[j] < nonbasis_[s])) s = j;
        if (s <= n_) pivot(i, s);
      }
    }
    optimize(m_);
    return true;
  }
  std::vector<double> primal() const {
    std::vector<double> x(n_);
    for (std::size_t i = 0; i < m_; ++i) if (basis_[i] < n_) x[basis_[i]] = d_[i][n_ + 1] * variable_scale_[basis_[i]];
    return x;
  }
  std::vector<MarginRational> dual(bool feasible) const {
    std::vector<MarginRational> y(m_);
    for (std::size_t j = 0; j <= n_; ++j)
      if (nonbasis_[j] >= n_ && nonbasis_[j] != artificial)
        y[nonbasis_[j] - n_] = MarginRational::propose(std::max(0.0, d_[feasible ? m_ : m_ + 1][j]) * (feasible ? objective_scale_ : 1) / row_scale_[nonbasis_[j] - n_]);
    return y;
  }
  std::vector<std::size_t> basic_columns() const {
    std::vector<std::size_t> result;
    for (const auto index : basis_) if (index < n_) result.push_back(index);
    return result;
  }
  std::vector<std::size_t> price_rows() const {
    std::vector<std::size_t> result;
    for (const auto index : nonbasis_)
      if (index >= n_ && index != artificial) result.push_back(index - n_);
    return result;
  }

 private:
  void spend(std::size_t amount) {
    if (amount > work_) throw MarginWorkLimit{};
    work_ -= amount;
  }
  void pivot(std::size_t r, std::size_t s) {
    spend((m_ + 2) * (n_ + 2));
    const auto inverse = 1 / d_[r][s];
    for (std::size_t i = 0; i < m_ + 2; ++i) {
      if (i == r || d_[i][s] == 0) continue;
      const auto ratio = d_[i][s] * inverse;
      for (std::size_t j = 0; j < n_ + 2; ++j) d_[i][j] -= d_[r][j] * ratio;
      d_[i][s] = -ratio;
    }
    for (std::size_t j = 0; j < n_ + 2; ++j) d_[r][j] *= inverse;
    d_[r][s] = inverse;
    std::swap(basis_[r], nonbasis_[s]);
  }
  void optimize(std::size_t objective) {
    std::size_t stagnant = 0;
    bool bland = false;
    while (true) {
      std::size_t s = n_ + 1;
      for (std::size_t j = 0; j <= n_; ++j) {
        if (nonbasis_[j] == artificial || d_[objective][j] >= -epsilon) continue;
        if (s > n_ || (bland ? nonbasis_[j] < nonbasis_[s] :
            d_[objective][j] < d_[objective][s] - epsilon ||
            (std::abs(d_[objective][j] - d_[objective][s]) <= epsilon && nonbasis_[j] < nonbasis_[s]))) s = j;
      }
      if (s > n_) return;
      std::size_t r = m_;
      for (std::size_t i = 0; i < m_; ++i) {
        if (d_[i][s] <= epsilon) continue;
        const auto ratio = d_[i][n_ + 1] / d_[i][s];
        if (r == m_ || ratio < d_[r][n_ + 1] / d_[r][s] - epsilon ||
            (std::abs(ratio - d_[r][n_ + 1] / d_[r][s]) <= epsilon && basis_[i] < basis_[r])) r = i;
      }
      if (r == m_) throw MarginWorkLimit{};
      const auto before = d_[objective][n_ + 1];
      pivot(r, s);
      stagnant = std::abs(before - d_[objective][n_ + 1]) <= epsilon ? stagnant + 1 : 0;
      if (stagnant > m_ + n_) bland = true;
    }
  }
  static constexpr double epsilon = 1e-11;
  static constexpr auto artificial = std::numeric_limits<std::size_t>::max();
  std::size_t m_, n_;
  std::vector<std::size_t> basis_, nonbasis_;
  std::vector<std::vector<double>> d_;
  std::vector<double> row_scale_, variable_scale_;
  double objective_scale_;
  std::size_t& work_;
};
}  // namespace openport::trading::detail
