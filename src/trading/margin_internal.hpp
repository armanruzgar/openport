#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "margin_detail.hpp"
#include "margin_optimizer.hpp"
#include "openport/trading/evaluation.hpp"

// Strategy margin's shared primitives: margin.cpp pairs and allocates positions with
// them, and margin_exact.cpp certifies the joint integer optimum over the same units.
// They were margin.cpp's anonymous namespace. Private: only those two files include
// this header.
namespace openport::trading::margin_internal {
using pricing::OptionType;
inline constexpr Quantity kLot = 100;  ///< Shares one contract delivers, and so covers.
/// Strikes are validated to whole thousandths of a dollar.
inline std::int64_t milli(double strike) { return std::llround(strike * 1000); }
inline Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
/// One contract's intrinsic value at expiry, times the multiplier, at a spot in thousandths.
inline Money intrinsic(const md::OptionContract& c, std::int64_t spot) {
  const auto strike = milli(c.strike);
  const auto amount = std::max<std::int64_t>(0, c.type == OptionType::Call ? spot - strike : strike - spot);
  return Money::from_micros(amount * 1000) * 100;
}
/// A position as strategy margin pairs it: an option leg, or whole lots of 100
/// shares. Long shares cover a short call and short shares a short put, whatever
/// either expires; short shares are also a short of their own that a long call
/// can protect.
struct Unit {
  const MarginLeg* leg = nullptr;      ///< Null for shares.
  const MarginStock* stock = nullptr;  ///< The shares, for lots.
  Quantity size = 0;                   ///< Contracts, or lots.
  Money requirement_one;               ///< An option short's naked requirement for one contract, without its value.
  Money naked_one;                     ///< What one contract or lot of a short holds unpaired.
  Money secured_one;                   ///< A cash account's or IRA's short put: its strike, held in cash instead.
  std::int64_t house = 0;              ///< Short shares: the house percentage on their margin.
};
Money covered(const Unit& s, const Unit& cover, Quantity n);
/// One underlying's positions, sorted for pairing. Short shares sit both among
/// the short calls (a short a long call protects) and the long puts (a cover).
struct Book {
  std::vector<Unit> short_puts, long_calls, short_calls, long_puts;
};
using Parts = std::vector<MarginPart>;
MarginPartKind unpaired_kind(const Unit& u);
std::pair<std::string, Quantity> shares_leg(const MarginStock& stock, Quantity shares);
std::pair<std::string, Quantity> leg_of(const Unit& u, Quantity n);
void add(Parts* parts, MarginPartKind kind, std::vector<std::pair<std::string, Quantity>> legs, Money requirement);
/// The joint integer optimum (margin_exact.cpp), over the documented strategy
/// primitives. There is one pool per short expiry: combining two pools with that
/// expiry never increases their worst loss. The only other blocks are naked
/// residuals and legal pairs. Thus these integer allocations represent every
/// legal partition, without enumerating partitions or individual contracts.
class ExactMargin {
  using Rational = detail::MarginRational;
  struct Column {
    std::size_t unit, other;
    int pool = -1;
    bool reserve = false;
    Rational saving;
  };
  struct Pool {
    Timestamp expiry;
    std::vector<std::size_t> rows;
    std::vector<std::int64_t> spots;
    std::size_t tail;
  };
 public:
  ExactMargin(const Book& book, const MarginStock* stock, const MarginPolicy& policy);
  void incumbent(Money requirement, Parts parts) {
    if (requirement.micros() < best_) { best_ = requirement.micros(); incumbent_ = std::move(parts); }
  }
  Money solve(Parts* parts);
 private:
  bool short_unit(const Unit& u) const { return std::find(shorts_.begin(), shorts_.end(), &u) != shorts_.end(); }
  bool is_short(std::size_t i) const { return short_unit(*units_[i]); }
  static Rational buyback(const Unit& u) { return {u.leg->value.micros(), u.size}; }
  Rational naked_price(const Unit& u) const {
    if (u.leg) {
      if (u.leg->quantity > 0) return {};
      return u.secured_one > Money{} ? Rational(u.secured_one.micros()) : buyback(u) + Rational(u.requirement_one.micros());
    }
    if (!short_unit(u)) return {};
    return Rational(u.stock->value.micros(), magnitude(u.stock->shares)) * Rational(kLot) * Rational(300 + u.house, 200);
  }
  std::size_t column(Column c) {
    if ((a_.size() + 2) * (columns_.size() + 3) > 1000000) throw detail::MarginWorkLimit{};
    for (auto& row : a_) row.push_back(0);
    objective_.push_back(c.saving); columns_.push_back(c);
    return columns_.size() - 1;
  }
  /// The exact checks' work, as it is done: counted on the thread (detail::margin_work)
  /// and, from replay driver 13 on, charged to the budget the simplex pivots draw on,
  /// a matrix cell swept one unit, as a dictionary cell is, and an exact rational
  /// operation kExactRationalWork. Exhausting it ends the proof.
  void account(std::uint64_t cells, std::uint64_t operations) {
    auto& work = detail::margin_work();
    work.checks += cells;
    work.rational += operations;
    if (!counted_) return;
    const auto units = cells + detail::kExactRationalWork * operations;
    if (units > work_) { work_ = 0; throw detail::MarginWorkLimit{}; }
    work_ -= units;
  }
  // The solver's recursion: defined in margin_exact.cpp, the only file that calls
  // them, and inline, as they were in the class body. The trading library is built
  // position-independent, where the compiler may neither inline nor specialize an
  // exported function that is not inline, even within its own file.
  inline Rational bound(std::vector<Rational> y, bool feasible, bool& price_pairs);
  inline std::vector<Rational> basis_prices(const detail::MarginSimplex& lp,
      const std::vector<std::size_t>& active, const std::vector<std::size_t>& rows);
  std::int64_t lower_bound(const Rational& upper_saving) const {
    if (!lattice_) return (base_ - upper_saving).ceil();
    const auto steps = -Rational((-upper_saving * Rational(1, savings_step_)).ceil());
    return (base_ - steps * Rational(savings_step_)).ceil();
  }
  inline void search(int stage = 2);
  inline void explain(Parts* parts) const;
  const MarginStock* stock_;
  MarginPolicy policy_;
  std::vector<const Unit*> units_, shorts_;
  std::vector<Rational> prices_, objective_;
  Rational base_;
  Rational odd_shares_;
  std::vector<Pool> pools_;
  std::vector<Column> columns_, pairs_;
  detail::MarginSimplex::Matrix a_;
  std::vector<std::int64_t> b_;
  std::int64_t best_ = 0, savings_step_ = 0;
  bool lattice_ = true;
  std::vector<Quantity> best_x_;
  Parts incumbent_;
  /// Replay driver 13 on: the exact checks are charged to the budget too.
  bool counted_;
  std::size_t work_;
};
}  // namespace openport::trading::margin_internal

namespace openport::trading {
// margin.cpp's public functions call the helpers unqualified, as they did when the
// helpers were an anonymous namespace in the same file.
using namespace margin_internal;
}  // namespace openport::trading
