#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <functional>
#include <map>
#include <vector>

#include "margin_detail.hpp"
#include "margin_optimizer.hpp"

#include "openport/trading/evaluation.hpp"
#include "openport/trading/risk.hpp"

namespace openport::trading {
namespace {
using pricing::OptionType;
constexpr Quantity kLot = 100;  ///< Shares one contract delivers, and so covers.
/// Strikes are validated to whole thousandths of a dollar.
std::int64_t milli(double strike) { return std::llround(strike * 1000); }
Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
/// One contract's intrinsic value at expiry, times the multiplier, at a spot in thousandths.
Money intrinsic(const md::OptionContract& c, std::int64_t spot) {
  const auto strike = milli(c.strike);
  const auto amount = std::max<std::int64_t>(0, c.type == OptionType::Call ? spot - strike : strike - spot);
  return Money::from_micros(amount * 1000) * 100;
}
/// `shares` of a short stock position hold their value and half again, as a
/// short sale does, the half raised by any house percentage.
Money short_shares(const MarginStock& stock, Quantity shares, std::int64_t house) {
  return stock.value.prorate(shares, magnitude(stock.shares)).prorate(300 + house, 200);
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
/// The buy-back value of `n` of a short option's contracts.
Money value(const Unit& u, Quantity n) { return u.leg->value.prorate(n, -u.leg->quantity); }
/// What `n` contracts or lots of a short hold unpaired: an option its buy-back
/// value plus its naked requirement, shares their value and half again.
Money naked(const Unit& u, Quantity n) {
  if (u.stock) return short_shares(*u.stock, n * kLot, u.house);
  if (u.secured_one > Money{}) return u.secured_one * n;
  return value(u, n) + u.requirement_one * n;
}
/// What `n` of short `s` hold with the long or shares that cover them: a put
/// long below or a call long above its short the strikes' distance (nothing when
/// the long is at or beyond the short's strike), a long call protecting short
/// shares its strike, long shares written against a call nothing, and short
/// shares written against a put the put's buy-back value, so its credit stays.
Money covered(const Unit& s, const Unit& cover, Quantity n) {
  if (cover.stock) return cover.stock->shares > 0 ? Money{} : value(s, n);
  const auto b = milli(cover.leg->contract.strike);
  const auto a = s.stock ? 0 : milli(s.leg->contract.strike);
  const bool put = s.leg && s.leg->contract.type == OptionType::Put;
  const auto points = std::max<std::int64_t>(0, put ? a - b : b - a);
  return Money::from_micros(points * 1000) * 100 * n;
}
/// Whether `cover` can pair with short `s`: shares cover an option whatever it
/// expires, a long call protects short shares while it lasts, and a long option
/// covers a short of its type that expires with it or earlier.
bool covers(const Unit& cover, const Unit& s) {
  if (cover.stock || s.stock) return true;
  return cover.leg->contract.expiry_time() >= s.leg->contract.expiry_time();
}
/// Reg T's short straddle or combination: the greater naked requirement, plus
/// the other side's buy-back value (the larger one when both need the same).
Money straddle(const Unit& put, const Unit& call, Quantity n) {
  const auto p = naked(put, n), c = naked(call, n);
  if (p != c) return p > c ? p + value(call, n) : c + value(put, n);
  return p + std::max(value(put, n), value(call, n));
}
/// What one contract of a short put and a short call save as a straddle.
std::int64_t straddle_saving(const Unit& put, const Unit& call) {
  return (put.naked_one + call.naked_one - straddle(put, call, 1)).micros();
}
/// `n` of a short with the long or shares that cover it, or a short put with
/// the short call of its straddle.
struct Pair {
  const Unit* short_unit;
  const Unit* cover;  ///< The short call, for a straddle.
  Quantity n;
  bool straddle = false;
};
/// Two options, not shares: pairs whose shorts expire together share a worst loss.
bool vertical(const Pair& p) { return !p.straddle && p.short_unit->leg && p.cover->leg; }
/// A pair never holds more than its short would naked.
Money cost(const Pair& p) {
  if (p.straddle) return straddle(*p.short_unit, *p.cover, p.n);
  return std::min(covered(*p.short_unit, *p.cover, p.n), naked(*p.short_unit, p.n));
}
/// One underlying's positions, sorted for pairing. Short shares sit both among
/// the short calls (a short a long call protects) and the long puts (a cover).
struct Book {
  std::vector<Unit> short_puts, long_calls, short_calls, long_puts;
};
Book book_of(const std::vector<const MarginLeg*>& legs, const MarginStock* stock, const MarginPolicy& policy) {
  Book book;
  for (const auto* leg : legs) {
    Unit u{leg, nullptr, magnitude(leg->quantity), {}, {}, {}, policy.house_percent};
    const bool put = leg->contract.type == OptionType::Put;
    if (leg->quantity < 0) {
      u.requirement_one = naked_requirement(leg->contract, leg->spot).prorate(100 + policy.house_percent, 100);
      if (put && policy.account != AccountType::Margin) u.secured_one = Money::from_micros(milli(leg->contract.strike) * 1000) * 100;
      u.naked_one = naked(u, 1);
      (put ? book.short_puts : book.short_calls).push_back(u);
    } else {
      (put ? book.long_puts : book.long_calls).push_back(u);
    }
  }
  if (stock && magnitude(stock->shares) >= kLot) {
    Unit lots{nullptr, stock, magnitude(stock->shares) / kLot, {}, {}, {}, policy.house_percent};
    if (stock->shares > 0) {
      book.long_calls.push_back(lots);
    } else {
      lots.naked_one = naked(lots, 1);
      book.short_calls.push_back(lots);
      if (policy.account == AccountType::Margin) book.long_puts.push_back(lots);
    }
  }
  return book;
}
struct Pairing {
  std::vector<Pair> pairs;
  std::vector<std::pair<const Unit*, Quantity>> unpaired_shorts, unpaired_longs;
};
/// Pair verticals, share covers and, when enabled, straddles in one min-cost
/// flow. Each contract takes at most one pair, maximizing the saving against
/// naked costs before verticals share their worst loss. Without `spreads`, only
/// shares cover; without `straddles`, the two option types pair independently.
Pairing pair_units(const Book& book, bool spreads, bool straddles = false) {
  // X: short puts, long calls. Y: long puts, short calls. Shares join their type.
  std::vector<const Unit*> x, y;
  for (const auto& u : book.short_puts) x.push_back(&u);
  for (const auto& u : book.long_calls) x.push_back(&u);
  for (const auto& u : book.long_puts) y.push_back(&u);
  for (const auto& u : book.short_calls) y.push_back(&u);
  // Nodes: source, X, Y, sink. Edge i's reverse is i ^ 1.
  struct Edge { std::size_t to; Quantity capacity; std::int64_t cost; };
  std::vector<Edge> edges;
  const std::size_t source = 0, sink = x.size() + y.size() + 1;
  std::vector<std::vector<std::size_t>> out(sink + 1);
  const auto link = [&](std::size_t from, std::size_t to, Quantity capacity, std::int64_t cost) {
    out[from].push_back(edges.size());
    edges.push_back({to, capacity, cost});
    out[to].push_back(edges.size());
    edges.push_back({from, 0, -cost});
  };
  std::vector<std::pair<std::size_t, Pair>> matches;
  for (std::size_t i = 0; i < x.size(); ++i) {
    link(source, 1 + i, x[i]->size, 0);
    const bool put = i < book.short_puts.size();
    for (std::size_t j = 0; j < y.size(); ++j) {
      const bool call = j >= book.long_puts.size();
      Pair p{put ? x[i] : y[j], put ? y[j] : x[i], 1, put && call};
      std::int64_t saving;
      if (p.straddle) {
        if (!straddles || !y[j]->leg) continue;
        saving = straddle_saving(*x[i], *y[j]);
      } else {
        if (put == call || !covers(*p.cover, *p.short_unit) ||
            (!spreads && p.cover->leg && p.short_unit->leg)) continue;
        saving = (p.short_unit->naked_one - cost(p)).micros();
      }
      if (saving <= 0) continue;
      matches.emplace_back(edges.size(), p);
      link(1 + i, 1 + x.size() + j, std::min(x[i]->size, y[j]->size), -saving);
    }
  }
  for (std::size_t j = 0; j < y.size(); ++j) link(1 + x.size() + j, sink, y[j]->size, 0);
  // Augment along the cheapest path while it still saves something. The residual
  // graph never has a negative cycle, so a queue-based Bellman-Ford finds it.
  constexpr auto unreached = std::numeric_limits<std::int64_t>::max();
  std::vector<std::int64_t> distance(sink + 1);
  std::vector<std::size_t> via(sink + 1);
  std::vector<char> queued(sink + 1);
  std::deque<std::size_t> queue;
  while (!matches.empty()) {
    std::fill(distance.begin(), distance.end(), unreached);
    distance[source] = 0;
    queue.assign(1, source);
    queued[source] = 1;
    while (!queue.empty()) {
      const auto node = queue.front();
      queue.pop_front();
      queued[node] = 0;
      for (const auto e : out[node]) {
        const auto& edge = edges[e];
        if (edge.capacity <= 0 || distance[node] + edge.cost >= distance[edge.to]) continue;
        distance[edge.to] = distance[node] + edge.cost;
        via[edge.to] = e;
        if (!queued[edge.to]) { queue.push_back(edge.to); queued[edge.to] = 1; }
      }
    }
    if (distance[sink] == unreached || distance[sink] >= 0) break;
    auto push = std::numeric_limits<Quantity>::max();
    for (auto node = sink; node != source; node = edges[via[node] ^ 1].to) push = std::min(push, edges[via[node]].capacity);
    for (auto node = sink; node != source; node = edges[via[node] ^ 1].to) {
      edges[via[node]].capacity -= push;
      edges[via[node] ^ 1].capacity += push;
    }
  }
  Pairing result;
  std::vector<Quantity> x_left, y_left;
  for (const auto* u : x) x_left.push_back(u->size);
  for (const auto* u : y) y_left.push_back(u->size);
  for (const auto& [e, pair] : matches) {
    const auto n = edges[e ^ 1].capacity;  // The flow carried.
    if (n == 0) continue;
    auto p = pair;
    p.n = n;
    result.pairs.push_back(p);
    x_left[edges[e ^ 1].to - 1] -= n;
    y_left[edges[e].to - 1 - x.size()] -= n;
  }
  for (std::size_t i = 0; i < x.size(); ++i)
    if (x_left[i] > 0)
      (i < book.short_puts.size() ? result.unpaired_shorts : result.unpaired_longs).emplace_back(x[i], x_left[i]);
  for (std::size_t j = 0; j < y.size(); ++j)
    if (y_left[j] > 0)
      (j < book.long_puts.size() ? result.unpaired_longs : result.unpaired_shorts).emplace_back(y[j], y_left[j]);
  return result;
}
/// The worst loss at `expiry` of pairs whose shorts all expire then, with that
/// expiry's unpaired longs: a later long counts at its intrinsic value there.
/// Pairs cannot be net short calls, so the minimum is at zero or a strike.
Money paired_worst_loss(const std::vector<const Pair*>& pairs, const std::vector<std::pair<const Unit*, Quantity>>& longs) {
  std::vector<std::int64_t> spots{0};
  for (const auto* p : pairs) {
    spots.push_back(milli(p->short_unit->leg->contract.strike));
    spots.push_back(milli(p->cover->leg->contract.strike));
  }
  for (const auto& [unit, n] : longs) spots.push_back(milli(unit->leg->contract.strike));
  Money worst;
  for (const auto spot : spots) {
    Money payoff;
    for (const auto* p : pairs)
      payoff = payoff + (intrinsic(p->cover->leg->contract, spot) - intrinsic(p->short_unit->leg->contract, spot)) * p->n;
    for (const auto& [unit, n] : longs) payoff = payoff + intrinsic(unit->leg->contract, spot) * n;
    worst = std::min(worst, payoff);
  }
  return -worst;
}
/// The group's worst loss at expiry, or nothing when net short calls leave it unbounded.
std::optional<Money> worst_loss(const std::vector<const MarginLeg*>& group) {
  Quantity calls = 0;
  std::vector<std::int64_t> spots{0};
  for (const auto* leg : group) {
    if (leg->contract.type == OptionType::Call) calls += leg->quantity;
    spots.push_back(milli(leg->contract.strike));
  }
  if (calls < 0) return std::nullopt;
  Money worst;
  for (const auto spot : spots) {
    Money payoff;
    for (const auto* leg : group) payoff = payoff + intrinsic(leg->contract, spot) * leg->quantity;
    worst = std::min(worst, payoff);
  }
  return -worst;
}
using Parts = std::vector<MarginPart>;
/// An unpaired short option: naked, or a cash account's or IRA's put secured with its strike.
MarginPartKind unpaired_kind(const Unit& u) { return u.secured_one > Money{} ? MarginPartKind::CashSecured : MarginPartKind::Naked; }
std::pair<std::string, Quantity> shares_leg(const MarginStock& stock, Quantity shares) {
  return {stock.underlying, stock.shares < 0 ? -shares : shares};
}
std::pair<std::string, Quantity> option_leg(const Unit& u, Quantity n) {
  return {u.leg->contract.osi_symbol(), u.leg->quantity < 0 ? -n : n};
}
/// One leg of a part: an option's contracts, or a lot's shares.
std::pair<std::string, Quantity> leg_of(const Unit& u, Quantity n) {
  return u.stock ? shares_leg(*u.stock, n * kLot) : option_leg(u, n);
}
void add(Parts* parts, MarginPartKind kind, std::vector<std::pair<std::string, Quantity>> legs, Money requirement) {
  if (parts) parts->push_back({kind, std::move(legs), requirement});
}
/// Longs no part holds: their premium is paid in full, as long shares are.
void add_longs(Parts* parts, const std::vector<std::pair<const Unit*, Quantity>>& longs) {
  for (const auto& [unit, n] : longs)
    if (unit->leg) add(parts, MarginPartKind::Long, {leg_of(*unit, n)}, {});
}
/// Shares that no pair takes: short ones hold their value and half again, long ones are paid for.
Money other_shares(const MarginStock* stock, Quantity lots_taken, std::int64_t house, Parts* parts) {
  if (!stock) return {};
  const auto shares = magnitude(stock->shares) - lots_taken * kLot;
  if (shares <= 0) return {};
  if (stock->shares > 0) {
    add(parts, MarginPartKind::Long, {shares_leg(*stock, shares)}, {});
    return {};
  }
  const auto requirement = short_shares(*stock, shares, house);
  add(parts, MarginPartKind::ShortShares, {shares_leg(*stock, shares)}, requirement);
  return requirement;
}
/// What the book holds when its shorts pair across expiries (`pairing`): each
/// pair its cost, the verticals whose shorts expire together at most their
/// combined worst loss then, and unpaired shorts naked.
Money across(const Pairing& pairing, const MarginStock* stock, std::int64_t house, Parts* parts) {
  Money total;
  Quantity lots_taken = 0;  // Short shares a long call protects, or long shares a call is written against.
  std::map<Timestamp, std::pair<std::vector<const Pair*>, std::vector<std::pair<const Unit*, Quantity>>>> by_expiry;
  for (const auto& p : pairing.pairs) {
    if (vertical(p)) {
      by_expiry[p.short_unit->leg->contract.expiry_time()].first.push_back(&p);
      continue;
    }
    const auto requirement = cost(p);
    total = total + requirement;
    if (p.short_unit->stock || (p.cover->stock && p.cover->stock->shares > 0)) lots_taken += p.n;
    add(parts, p.straddle ? MarginPartKind::Straddle : p.short_unit->stock ? MarginPartKind::ProtectedShares : MarginPartKind::Covered,
        {leg_of(*p.short_unit, p.n), leg_of(*p.cover, p.n)}, requirement);
  }
  for (const auto& [unit, n] : pairing.unpaired_longs)
    if (unit->leg) by_expiry[unit->leg->contract.expiry_time()].second.emplace_back(unit, n);
  for (const auto& [expiry, group] : by_expiry) {
    const auto& [pairs, longs] = group;
    if (pairs.empty()) {
      add_longs(parts, longs);
      continue;
    }
    Money paired;
    for (const auto* p : pairs) paired = paired + cost(*p);
    const auto worst = paired_worst_loss(pairs, longs);
    total = total + std::min(paired, worst);
    if (!parts) continue;
    if (worst < paired) {
      std::vector<std::pair<std::string, Quantity>> legs;
      for (const auto* p : pairs) {
        legs.push_back(leg_of(*p->short_unit, p->n));
        legs.push_back(leg_of(*p->cover, p->n));
      }
      for (const auto& [unit, n] : longs) legs.push_back(leg_of(*unit, n));
      add(parts, MarginPartKind::WorstLoss, std::move(legs), worst);
    } else {
      for (const auto* p : pairs) add(parts, MarginPartKind::Vertical, {leg_of(*p->short_unit, p->n), leg_of(*p->cover, p->n)}, cost(*p));
      add_longs(parts, longs);
    }
  }
  for (const auto& [unit, n] : pairing.unpaired_shorts) {
    if (unit->stock) continue;  // Short shares hold below.
    const auto requirement = naked(*unit, n);
    total = total + requirement;
    add(parts, unpaired_kind(*unit), {leg_of(*unit, n)}, requirement);
  }
  return total + other_shares(stock, lots_taken, house, parts);
}
/// What the book holds when each expiry stands on its own: its verticals and
/// naked shorts, or its worst loss at expiry when that is bounded and less.
/// Shares expire with none of them, so here they cover nothing.
Money separate(const std::vector<const MarginLeg*>& all, const MarginStock* stock, const MarginPolicy& policy, Parts* parts) {
  std::map<Timestamp, std::vector<const MarginLeg*>> expiries;
  for (const auto* leg : all) expiries[leg->contract.expiry_time()].push_back(leg);
  Money total;
  for (const auto& [expiry, group] : expiries) {
    const auto book = book_of(group, nullptr, policy);
    Parts own;
    Parts* explain = parts ? &own : nullptr;
    const auto pairing = pair_units(book, true);
    Money requirement;
    for (const auto& p : pairing.pairs) {
      requirement = requirement + cost(p);
      add(explain, MarginPartKind::Vertical, {leg_of(*p.short_unit, p.n), leg_of(*p.cover, p.n)}, cost(p));
    }
    for (const auto& [unit, n] : pairing.unpaired_shorts) {
      requirement = requirement + naked(*unit, n);
      add(explain, unpaired_kind(*unit), {leg_of(*unit, n)}, naked(*unit, n));
    }
    add_longs(explain, pairing.unpaired_longs);
    if (const auto loss = worst_loss(group); loss && *loss < requirement) {
      requirement = *loss;
      own.clear();
      std::vector<std::pair<std::string, Quantity>> legs;
      for (const auto* leg : group) legs.emplace_back(leg->contract.osi_symbol(), leg->quantity);
      add(explain, MarginPartKind::WorstLoss, std::move(legs), *loss);
    }
    total = total + requirement;
    if (parts) parts->insert(parts->end(), own.begin(), own.end());
  }
  return total + other_shares(stock, 0, policy.house_percent, parts);
}
/// Shares take the pairs that save most on their own, then each expiry's
/// remaining options hold their verticals and naked shorts, or bounded worst
/// loss. This lets a share-covered call sit beside an expiry's cash-secured puts
/// without leaving that expiry's offsetting long options unused.
Money shares_first(const Book& book, const MarginStock* stock, const MarginPolicy& policy, Parts* parts) {
  auto pairing = pair_units(book, false);
  std::vector<MarginLeg> remaining;
  for (const auto* side : {&pairing.unpaired_shorts, &pairing.unpaired_longs})
    for (const auto& [unit, n] : *side) {
      if (!unit->leg) continue;
      auto leg = *unit->leg;
      leg.value = leg.value.prorate(n, magnitude(leg.quantity));
      leg.quantity = leg.quantity < 0 ? -n : n;
      remaining.push_back(std::move(leg));
    }
  pairing.unpaired_shorts.clear();
  pairing.unpaired_longs.clear();
  std::vector<const MarginLeg*> options;
  for (const auto& leg : remaining) options.push_back(&leg);
  const auto held = across(pairing, stock, policy.house_percent, parts);
  return held + separate(options, nullptr, policy, parts);
}
/// Improve a feasible partition of whole contracts, scoring expiry pools and
/// pairs together. Share parts stay in the incumbent; their options cannot be
/// spent again. Every trial is itself a valid allocation, never a relaxation.
class JointAllocation {
  struct Holding { std::size_t unit; Quantity n; };
  using Group = std::vector<Holding>;
  struct Score {
    Money requirement;
    MarginPartKind kind = MarginPartKind::Naked;
  };
  struct Attempt { std::size_t a = 0, b = 0, version_a = 0, version_b = 0, work = 0; };
 public:
  JointAllocation(const Book& book, bool straddles) : straddles_(straddles) {
    std::map<std::string, const Unit*> sorted;
    for (const auto* side : {&book.short_puts, &book.short_calls, &book.long_puts, &book.long_calls})
      for (const auto& u : *side)
        if (u.leg) sorted.emplace(u.leg->contract.osi_symbol(), &u);
    for (const auto& [symbol, u] : sorted) {
      index_.emplace(symbol, units_.size());
      units_.push_back(u);
      expiries_.push_back(u->leg->contract.expiry_time());
    }
    std::vector<std::int64_t> spots{0};
    for (std::size_t i = 0; i < units_.size(); ++i) {
      spots.push_back(milli(units_[i]->leg->contract.strike));
    }
    std::sort(spots.begin(), spots.end());
    spots.erase(std::unique(spots.begin(), spots.end()), spots.end());
    for (const auto* u : units_) {
      std::vector<Money> curve;
      for (const auto spot : spots) curve.push_back(intrinsic(u->leg->contract, spot) * (u->leg->quantity < 0 ? -1 : 1));
      payoff_.push_back(std::move(curve));
    }
  }
  Parts improve(const Parts& seed, std::size_t budget) {
    work_ = budget;
    groups_.clear();
    scores_.clear();
    fixed_.clear();
    for (const auto& part : seed) {
      if (std::any_of(part.legs.begin(), part.legs.end(), [&](const auto& leg) { return !index_.contains(leg.first); })) {
        fixed_.push_back(part);
        continue;
      }
      Group group;
      for (const auto& [symbol, n] : part.legs) change(group, index_.at(symbol), magnitude(n));
      groups_.push_back(std::move(group));
      scores_.push_back({part.requirement, part.kind});
    }
    versions_.assign(groups_.size(), 0);
    // Keep the retry cache bounded even for a book of thousands of naked legs.
    attempts_.assign(groups_.size() < 32 ? groups_.size() * groups_.size() : 1024, {});
    // Count both trial setup and payoff work, not elapsed time or contracts.
    // Thus huge quantities and ties have the same deterministic stopping rule.
    while (work_ > 0) {
      bool improved = false;
      // Visit every cheap merge before spending the budget on exchanges near
      // the beginning of the book. In particular, pair residual naked shorts
      // even when earlier, already-optimal pools have many possible transfers.
      for (const bool pools_first : {true, false}) {
        for (std::size_t a = 0; a < groups_.size() && work_ > 0; ++a) {
          if (groups_[a].empty()) continue;
          for (std::size_t b = a + 1; b < groups_.size() && work_ > 0; ++b) {
            if (groups_[b].empty()) continue;
            auto& merged = trial_left_;
            merged = groups_[a];
            for (const auto& h : groups_[b]) change(merged, h.unit, h.n);
            if (pools_first) {
              if (!spend(1 + merged.size())) break;
              std::optional<Timestamp> expiry;
              bool same = true;
              for (const auto& h : merged) {
                if (units_[h.unit]->leg->quantity > 0) continue;
                if (expiry && *expiry != expiries_[h.unit]) { same = false; break; }
                expiry = expiries_[h.unit];
              }
              if (!same) continue;
            }
            // Equal-cost merges expose a pool's free offsetting payoff to later
            // transfers, and terminate because one fewer group remains occupied.
            trial_right_.clear();
            if (accept(a, b, true)) improved = true;
          }
        }
        if (improved) break;
      }
      if (improved) continue;
      for (std::size_t a = 0; a < groups_.size() && work_ > 0; ++a) {
        if (groups_[a].empty()) continue;
        for (std::size_t b = a + 1; b < groups_.size() && work_ > 0; ++b) {
          if (groups_[b].empty()) continue;
          // Transfers require a strict reduction. Two zero-cost groups cannot
          // improve; their useful equal-cost merges were already tried above.
          if (scores_[a].requirement == Money{} && scores_[b].requirement == Money{}) continue;
          auto& attempt = attempts_[(a * groups_.size() + b) % attempts_.size()];
          if (attempt.work && attempt.a == a && attempt.b == b &&
              attempt.version_a == versions_[a] && attempt.version_b == versions_[b]) {
            spend(attempt.work);
            continue;
          }
          const auto before = work_;
          // Transfers and exchanges use the full tranche and one contract.
          // No loop depends on the number of contracts in a position.
          const auto left = groups_[a], right = groups_[b];
          for (const auto& h : left) {
            if (transfer(a, b, h.unit, h.n) || (h.n > 1 && transfer(a, b, h.unit, 1))) { improved = true; break; }
            if (improved || work_ == 0) break;
          }
          if (improved) break;
          for (const auto& h : right) {
            if (transfer(b, a, h.unit, h.n) || (h.n > 1 && transfer(b, a, h.unit, 1))) { improved = true; break; }
            if (work_ == 0) break;
          }
          if (improved) break;
          for (const auto& h : left) {
            for (const auto& k : right) {
              if (h.unit == k.unit) continue;
              const auto n = std::min(h.n, k.n);
              if (exchange(a, b, h.unit, k.unit, n) || (n > 1 && exchange(a, b, h.unit, k.unit, 1))) {
                improved = true;
                break;
              }
              if (work_ == 0) break;
            }
            if (improved || work_ == 0) break;
          }
          if (improved) break;
          if (transfer_bundle(a, b, left) || transfer_bundle(b, a, right)) { improved = true; break; }
          // A failed neighbourhood is unchanged until either group changes.
          // Charge its original work even on a hit so skipping repeated trials
          // affects CPU time, never the search path or budget.
          attempt = {a, b, versions_[a], versions_[b], before - work_};
        }
        if (improved) break;
      }
      if (!improved) break;
    }
    Parts result = fixed_;
    for (std::size_t i = 0; i < groups_.size(); ++i) explain(groups_[i], scores_[i], result);
    return result;
  }
  std::size_t unused_work() const { return work_; }
 private:
  static void change(Group& group, std::size_t unit, Quantity n) {
    const auto it = std::lower_bound(group.begin(), group.end(), unit, [](const Holding& h, std::size_t id) { return h.unit < id; });
    if (it == group.end() || it->unit != unit) { group.insert(it, {unit, n}); return; }
    it->n += n;
    if (it->n == 0) group.erase(it);
  }
  bool spend(std::size_t amount) {
    if (amount > work_) { work_ = 0; return false; }
    work_ -= amount;
    return true;
  }
  std::optional<Pair> pair(const Holding& first, const Holding& second) const {
    auto a = first.unit, b = second.unit;
    if (units_[a]->leg->quantity > 0) std::swap(a, b);
    if (units_[a]->leg->quantity > 0) return std::nullopt;
    const auto n = std::min(first.n, second.n);
    if (units_[b]->leg->quantity < 0) {
      if (!straddles_ || units_[a]->leg->contract.type == units_[b]->leg->contract.type) return std::nullopt;
      if (units_[a]->leg->contract.type == OptionType::Call) std::swap(a, b);
      return Pair{units_[a], units_[b], n, true};
    }
    if (units_[a]->leg->contract.type != units_[b]->leg->contract.type || expiries_[b] < expiries_[a]) return std::nullopt;
    return Pair{units_[a], units_[b], n, false};
  }
  static Quantity paired_quantity(const Unit& unit, const Pair& p) {
    return &unit == p.short_unit || &unit == p.cover ? p.n : 0;
  }
  std::optional<Pair> pair(const Group& group) const {
    if (group.size() == 2) return pair(group[0], group[1]);
    // A pair plus one residual leg permits an equal-cost staging move: put a
    // free cover beside a straddle before replacing that straddle with a pool.
    // Also permit a vertical to release the other straddle short as naked.
    if (group.size() != 3) return std::nullopt;
    std::optional<Pair> best;
    Money saving;
    for (std::size_t i = 0; i < group.size(); ++i) {
      for (std::size_t j = i + 1; j < group.size(); ++j) {
        const auto p = pair(group[i], group[j]);
        if (!p) continue;
        auto saved = -pair_cost(*p);
        for (const auto k : {i, j}) {
          const auto& u = *units_[group[k].unit];
          if (u.leg->quantity < 0) saved = saved + naked_cost(u, group[k].n) - naked_cost(u, group[k].n - p->n);
        }
        if (!best || saved > saving) { best = p; saving = saved; }
      }
    }
    return best;
  }
  // A reallocation can split a position's buy-back value into thirds, etc.
  // Round each new tranche up so splitting cannot manufacture micro-dollar
  // savings. Unchanged incumbent parts remain available as the outer fallback.
  static Money buyback(const Unit& u, Quantity n) {
    if (n == 0) return {};
    if (n == -u.leg->quantity) return u.leg->value;
    if (u.leg->value.micros() % u.size == 0) return Money::from_micros(u.leg->value.micros() / u.size) * n;
    __extension__ using Wide = __int128;
    auto result = value(u, n);
    if (static_cast<Wide>(result.micros()) * -u.leg->quantity < static_cast<Wide>(u.leg->value.micros()) * n)
      result = result + Money::from_micros(1);
    return result;
  }
  static Money naked_cost(const Unit& u, Quantity n) {
    return u.secured_one > Money{} ? u.secured_one * n : buyback(u, n) + u.requirement_one * n;
  }
  static Money pair_cost(const Pair& p) {
    const auto a = naked_cost(*p.short_unit, p.n);
    if (!p.straddle) return std::min(covered(*p.short_unit, *p.cover, p.n), a);
    const auto b = naked_cost(*p.cover, p.n);
    const auto av = buyback(*p.short_unit, p.n), bv = buyback(*p.cover, p.n);
    return a > b ? a + bv : b > a ? b + av : a + std::max(av, bv);
  }
  std::optional<Score> score(const Group& group) {
    if (!spend(1 + group.size())) return std::nullopt;
    Score best;
    std::optional<Timestamp> expiry;
    bool pool = true;
    Quantity calls = 0;
    for (const auto& h : group) {
      const auto& u = *units_[h.unit];
      if (u.leg->quantity < 0) {
        best.requirement = best.requirement + naked_cost(u, h.n);
        const auto time = expiries_[h.unit];
        if (expiry && *expiry != time) pool = false;
        expiry = time;
      }
      if (u.leg->contract.type == OptionType::Call) calls += u.leg->quantity < 0 ? -h.n : h.n;
    }
    if (const auto p = pair(group)) {
      auto requirement = pair_cost(*p);
      for (const auto& h : group) {
        const auto& u = *units_[h.unit];
        if (u.leg->quantity < 0) requirement = requirement + naked_cost(u, h.n - paired_quantity(u, *p));
      }
      if (requirement <= best.requirement) best = {requirement, p->straddle ? MarginPartKind::Straddle : MarginPartKind::Vertical};
    }
    if (!pool || !expiry || calls < 0) return best;
    for (const auto& h : group)
      if (units_[h.unit]->leg->quantity > 0 && expiries_[h.unit] < *expiry) return best;
    if (!spend(group.size() * payoff_.front().size())) return std::nullopt;
    Money loss;
    for (std::size_t s = 0; s < payoff_.front().size(); ++s) {
      Money value;
      for (const auto& h : group) value = value + payoff_[h.unit][s] * h.n;
      loss = std::max(loss, -value);
    }
    if (loss < best.requirement) best = {loss, MarginPartKind::WorstLoss};
    return best;
  }
  bool accept(std::size_t a, std::size_t b, bool merge = false) {
    auto& left = trial_left_;
    auto& right = trial_right_;
    try {
      const auto l = score(left), r = score(right);
      if (!l || !r) return false;
      const auto before = scores_[a].requirement + scores_[b].requirement;
      const auto after = l->requirement + r->requirement;
      if (after > before) return false;
      if (after == before) {
        if (!merge || l->kind == MarginPartKind::Naked) return false;
        if (l->kind != MarginPartKind::WorstLoss) {
          const auto p = pair(left);
          if (!p) return false;
          // Stage unused longs, but do not strand a naked short in a pair on
          // a tie: it may need to join a different expiry pool next.
          Timestamp latest = 0;
          for (const auto& h : left)
            if (units_[h.unit]->leg->quantity < 0) {
              if (h.n != paired_quantity(*units_[h.unit], *p)) return false;
              latest = std::max(latest, expiries_[h.unit]);
            }
          // An earlier free long would prevent the staged group becoming a
          // pool, even though leaving that long outside costs nothing.
          for (const auto& h : left)
            if (units_[h.unit]->leg->quantity > 0 && expiries_[h.unit] < latest) return false;
        }
      }
      groups_[a].swap(left);
      groups_[b].swap(right);
      scores_[a] = *l;
      scores_[b] = *r;
      ++versions_[a];
      ++versions_[b];
      return true;
    } catch (const TradingError& error) {
      // An unneeded naked or intrinsic intermediate can overflow even when the
      // incumbent's spread widths fit. Such a trial cannot replace it.
      if (error.code() != Reason::ARITHMETIC_OVERFLOW) throw;
      return false;
    }
  }
  bool transfer(std::size_t a, std::size_t b, std::size_t unit, Quantity n) {
    if (work_ == 0) return false;
    auto& left = trial_left_;
    auto& right = trial_right_;
    left = groups_[a]; right = groups_[b];
    change(left, unit, -n);
    change(right, unit, n);
    return accept(a, b);
  }
  bool exchange(std::size_t a, std::size_t b, std::size_t first, std::size_t second, Quantity n) {
    if (work_ == 0) return false;
    auto& left = trial_left_;
    auto& right = trial_right_;
    left = groups_[a]; right = groups_[b];
    change(left, first, -n); change(right, first, n);
    change(right, second, -n); change(left, second, n);
    return accept(a, b);
  }
  bool transfer_bundle(std::size_t a, std::size_t b, const Group& source) {
    for (std::size_t i = 0; i < source.size() && work_ > 0; ++i) {
      for (std::size_t j = i + 1; j < source.size() && spend(1); ++j) {
        const auto first = source[i].unit, second = source[j].unit;
        const auto n = std::min(source[i].n, source[j].n);
        const auto move = [&](Quantity first_n, Quantity second_n) {
          auto& left = trial_left_;
          auto& right = trial_right_;
          left = groups_[a]; right = groups_[b];
          change(left, first, -first_n); change(right, first, first_n);
          change(left, second, -second_n); change(right, second, second_n);
          return accept(a, b);
        };
        // An offsetting long need not have the short's type or quantity: a
        // call can reduce a put pool's loss near its strikes. Test both whole
        // holdings and balanced tranches; the scorer checks feasibility.
        if ((source.size() > 2 && move(source[i].n, source[j].n)) ||
            (source[i].n != source[j].n && move(n, n)) || (n > 1 && move(1, 1))) return true;
      }
    }
    return false;
  }
  void explain(const Group& group, const Score& score, Parts& parts) const {
    if (group.empty()) return;
    if (score.kind == MarginPartKind::WorstLoss) {
      std::vector<std::pair<std::string, Quantity>> legs;
      for (const auto& h : group) legs.push_back(leg_of(*units_[h.unit], h.n));
      add(&parts, score.kind, std::move(legs), score.requirement);
      return;
    }
    const auto p = score.kind == MarginPartKind::Vertical || score.kind == MarginPartKind::Straddle ? pair(group) : std::nullopt;
    if (p) add(&parts, score.kind, {leg_of(*p->short_unit, p->n), leg_of(*p->cover, p->n)}, pair_cost(*p));
    for (const auto& h : group) {
      const auto n = h.n - (p ? paired_quantity(*units_[h.unit], *p) : 0);
      if (n == 0) continue;
      const auto& u = *units_[h.unit];
      const bool shorted = u.leg->quantity < 0;
      add(&parts, shorted ? unpaired_kind(u) : MarginPartKind::Long, {leg_of(u, n)}, shorted ? naked_cost(u, n) : Money{});
    }
  }
  bool straddles_;
  std::size_t work_ = 0;
  std::map<std::string, std::size_t> index_;
  std::vector<std::size_t> versions_;
  std::vector<Attempt> attempts_;
  std::vector<const Unit*> units_;
  std::vector<Timestamp> expiries_;
  std::vector<std::vector<Money>> payoff_;
  std::vector<Group> groups_;
  // Most trials fail. Reuse their capacity instead of allocating two vectors
  // for each attempt; an accepted trial swaps storage with the old groups.
  Group trial_left_, trial_right_;
  std::vector<Score> scores_;
  Parts fixed_;
};
#include "margin_exact.inc"

/// First certify the joint integer optimum. Older drivers and capped solves
/// retain the previous allocator below: the least of pairing across expiries,
/// verticals and share covers alone (a condor's shared worst loss may hold less
/// without straddles), and each expiry on its own, both before and after shares
/// cover. A cash account pairs only with shares; neither it nor an IRA pairs
/// straddles. Bounded local improvement then allocates options jointly between
/// those strategy parts and expiry loss pools.
Money underlying_requirement(const std::vector<const MarginLeg*>& all, const MarginStock* stock, const MarginPolicy& policy,
                             Parts* parts, bool improve, std::string* solver) {
  const bool spreads = policy.account != AccountType::Cash;
  const bool search = improve && spreads && all.size() > 2;
  const auto book = book_of(all, stock, policy);
  if (improve && policy.exact) {
    try {
      const bool stock_integral = !stock || stock->shares > 0 ||
          (stock->value.micros() % magnitude(stock->shares) == 0 &&
           (static_cast<detail::MarginWide>(stock->value.micros() / magnitude(stock->shares)) *
            (300 + policy.house_percent)) % 200 == 0);
      const bool integral = stock_integral && std::all_of(all.begin(), all.end(), [](const MarginLeg* leg) {
        return leg->value.micros() % magnitude(leg->quantity) == 0;
      });
      // With no longs, cash-secured puts cannot improve a margin-account naked
      // put whose full cost is already below its strike. Only straddles remain:
      // the existing integer min-cost flow solves this case exactly.
      if (!stock && integral && policy.account == AccountType::Margin && book.long_puts.empty() && book.long_calls.empty() &&
          std::all_of(book.short_puts.begin(), book.short_puts.end(), [](const Unit& u) {
            return u.naked_one <= Money::from_micros(milli(u.leg->contract.strike) * 100000);
          })) {
        if (solver) *solver = "exact";
        return across(pair_units(book, true, true), nullptr, policy.house_percent, parts);
      }
      ExactMargin allocation(book, stock, policy);
      if (integral) {
        Parts seed;
        const auto incumbent = underlying_requirement(all, stock, policy, &seed, false, nullptr);
        allocation.incumbent(incumbent, std::move(seed));
      }
      const auto result = allocation.solve(parts);
      if (solver) *solver = "exact";
      return result;
    } catch (const detail::MarginWorkLimit&) {
      if (solver) *solver = "bounded";
    }
  } else if (solver) *solver = "legacy";
  const auto pairing = pair_units(book, spreads);
  Parts best_parts, candidate;
  std::vector<Parts> seeds;
  Parts* explain = parts || search ? &candidate : nullptr;
  auto best = across(pairing, stock, policy.house_percent, explain);
  if (search) seeds.push_back(candidate);
  best_parts.swap(candidate);
  const auto consider = [&](Money requirement) {
    if (requirement < best) { best = requirement; best_parts.swap(candidate); }
    candidate.clear();
  };
  const auto seed = [&](Money requirement) {
    if (search && std::none_of(seeds.begin(), seeds.end(), [&](const Parts& previous) {
          return previous.size() == candidate.size() && std::equal(previous.begin(), previous.end(), candidate.begin(),
              [](const MarginPart& a, const MarginPart& b) {
                return a.kind == b.kind && a.requirement == b.requirement && a.legs == b.legs;
              });
        })) seeds.push_back(candidate);
    consider(requirement);
  };
  if (policy.account == AccountType::Margin && !book.short_puts.empty() &&
      std::any_of(book.short_calls.begin(), book.short_calls.end(), [](const Unit& u) { return u.leg != nullptr; }))
    seed(across(pair_units(book, true, true), stock, policy.house_percent, explain));
  // Without spreads, each expiry pairs nothing that the share-only flow did not.
  if (spreads) {
    seed(separate(all, stock, policy, explain));
    if (stock && magnitude(stock->shares) >= kLot) seed(shares_first(book, stock, policy, explain));
  }
  if (search && best > Money{}) {
    try {
      JointAllocation allocation(book, policy.account == AccountType::Margin);
      // Every distinct feasible start gets a share of the fixed total budget;
      // a start that finishes early lends its unused work to the next one.
      std::size_t carry = 0;
      for (const auto& start : seeds) {
        candidate = allocation.improve(start, 32768 / seeds.size() + carry);
        carry = allocation.unused_work();
        Money requirement;
        for (const auto& part : candidate) requirement = requirement + part.requirement;
        consider(requirement);
        if (best == Money{}) break;
      }
    } catch (const TradingError& error) {
      if (error.code() != Reason::ARITHMETIC_OVERFLOW) throw;
    }
  }
  if (parts) *parts = std::move(best_parts);
  return best;
}
/// Strategy margin by underlying, shares joining their own.
std::vector<MarginUnderlying> strategy(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks,
                                       const MarginPolicy& policy, bool explain, bool improve = true) {
  std::map<std::string, std::pair<std::vector<const MarginLeg*>, const MarginStock*>> underlyings;
  for (const auto& leg : legs)
    if (leg.quantity != 0) underlyings[leg.contract.underlying].first.push_back(&leg);
  for (const auto& stock : stocks)
    if (stock.shares != 0) underlyings[stock.underlying].second = &stock;
  std::vector<MarginUnderlying> result;
  for (const auto& [underlying, group] : underlyings) {
    MarginUnderlying item;
    item.underlying = underlying;
    item.requirement = underlying_requirement(group.first, group.second, policy, explain ? &item.parts : nullptr, improve, &item.allocation);
    result.push_back(std::move(item));
  }
  return result;
}
}  // namespace

Quantity naked_shorts(const std::vector<MarginLeg>& legs) {
  // Per underlying and type, the latest-expiring shorts take the earliest long
  // that still covers them, which covers the most shorts overall.
  std::map<std::pair<std::string, OptionType>, std::pair<std::map<Timestamp, Quantity>, std::map<Timestamp, Quantity>>> books;
  for (const auto& leg : legs) {
    if (leg.quantity == 0) continue;
    auto& [shorts, longs] = books[{leg.contract.underlying, leg.contract.type}];
    (leg.quantity < 0 ? shorts : longs)[leg.contract.expiry_time()] += leg.quantity < 0 ? -leg.quantity : leg.quantity;
  }
  Quantity naked = 0;
  for (auto& [key, book] : books) {
    auto& [shorts, longs] = book;
    for (auto s = shorts.rbegin(); s != shorts.rend(); ++s) {
      auto left = s->second;
      for (auto l = longs.lower_bound(s->first); l != longs.end() && left > 0; ++l) {
        const auto n = std::min(left, l->second);
        left -= n;
        l->second -= n;
      }
      naked += left;
    }
  }
  return naked;
}

MarginPolicy margin_policy(const AccountRules& rules) {
  return {rules.account_type, rules.house_margin_percent, rules.pm_vol_shock};
}

Quantity disallowed_shorts(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks, AccountType account) {
  if (account == AccountType::Margin) return 0;
  std::map<std::string, std::vector<MarginLeg>> calls;
  for (const auto& leg : legs)
    if (leg.quantity != 0 && leg.contract.type == OptionType::Call) calls[leg.contract.underlying].push_back(leg);
  std::map<std::string, Quantity> shares;
  for (const auto& stock : stocks) shares[stock.underlying] += stock.shares;
  Quantity count = 0;
  for (const auto& [underlying, group] : calls) {
    // Shares cover any call, so long calls cover what they can first.
    Quantity naked = 0;
    if (account == AccountType::Ira) naked = naked_shorts(group);
    else for (const auto& leg : group) naked += std::max<Quantity>(0, -leg.quantity);
    const auto held = shares.contains(underlying) ? shares.at(underlying) : 0;
    count += std::max<Quantity>(0, naked - std::max<Quantity>(0, held) / kLot);
  }
  for (const auto& [underlying, held] : shares)
    if (held < 0) count += (-held + kLot - 1) / kLot;
  return count;
}

Money detail::pairing_margin_requirement(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks,
                                          const MarginPolicy& policy) {
  Money total;
  for (const auto& item : strategy(legs, stocks, policy, false, false)) total = total + item.requirement;
  return total;
}

Money margin_requirement(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks, const MarginPolicy& policy) {
  Money total;
  for (const auto& item : strategy(legs, stocks, policy, false)) total = total + item.requirement;
  return total;
}

std::vector<MarginUnderlying> margin_breakdown(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks,
                                               const MarginPolicy& policy) {
  return strategy(legs, stocks, policy, true);
}

std::optional<std::vector<MarginUnderlying>> portfolio_margin_breakdown(const std::vector<MarginLeg>& legs,
    const Valuations& valuations, Timestamp now, Timestamp max_age,
    const std::map<std::string, StockPosition>& stocks, const std::map<std::string, double>& stock_prices,
    const MarginPolicy& policy) {
  struct Group {
    std::map<std::string, Position> options;
    std::map<std::string, StockPosition> stocks;
    Money minimum;
  };
  std::map<std::string, Group> groups;
  for (const auto& leg : legs) {
    if (leg.quantity == 0) continue;
    auto& group = groups[leg.contract.underlying];
    group.options.emplace(leg.contract.osi_symbol(), Position{leg.contract, leg.quantity, {}, {}, {}});
    group.minimum = group.minimum + Money::from_double(0.375 * leg.contract.multiplier) *
        (leg.quantity < 0 ? -leg.quantity : leg.quantity);
  }
  for (const auto& [symbol, stock] : stocks) groups[symbol].stocks.emplace(symbol, stock);
  std::vector<MarginUnderlying> result;
  for (const auto& [underlying, group] : groups) {
    ScenarioConfig scan;
    scan.spot_percent.clear();
    scan.vol_points = {0};
    if (policy.vol_shock > 0) {
      const auto shock = static_cast<double>(policy.vol_shock);
      scan.vol_points = {-shock, 0, shock};
    }
    // The account's display grid cannot narrow the margin scan.
    const bool index = md::is_index_underlying(underlying);
    const double low = index ? -8.0 : -15.0;
    const double high = index ? 6.0 : 15.0;
    for (int i = 0; i <= 10; ++i) scan.spot_percent.push_back(low + (high - low) * i / 10);
    // Keep the valuation's IV, even when it is below the display grid's floor.
    scan.vol_floor = std::numeric_limits<double>::min();
    const auto ledger = Ledger::restore({}, group.options, group.stocks);
    const auto grid = scenario_grid(ledger, valuations, scan, now, max_age, stock_prices);
    if (!grid.complete) return std::nullopt;
    double loss = 0;
    PortfolioScan worst;
    for (const auto& cell : grid.cells)
      if (-cell.pnl > loss) {
        loss = -cell.pnl;
        worst.spot_percent = cell.spot_percent;
        worst.vol_points = cell.vol_points;
      }
    worst.loss = Money::from_double(loss);
    worst.minimum = group.minimum;
    MarginUnderlying item;
    item.underlying = underlying;
    // The minimum is a floor under the scanned loss, not an addition to it.
    item.requirement = std::max(worst.loss, group.minimum).prorate(100 + policy.house_percent, 100);
    item.scan = worst;
    item.allocation.clear();
    result.push_back(std::move(item));
  }
  return result;
}

std::optional<Money> portfolio_margin_requirement(const std::vector<MarginLeg>& legs,
    const Valuations& valuations, Timestamp now, Timestamp max_age,
    const std::map<std::string, StockPosition>& stocks, const std::map<std::string, double>& stock_prices,
    const MarginPolicy& policy) {
  const auto groups = portfolio_margin_breakdown(legs, valuations, now, max_age, stocks, stock_prices, policy);
  if (!groups) return std::nullopt;
  Money total;
  for (const auto& item : *groups) total = total + item.requirement;
  return total;
}
}  // namespace openport::trading
