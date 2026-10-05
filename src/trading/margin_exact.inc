// Included in margin.cpp's anonymous namespace to share the documented strategy
// primitives. There is one pool per short expiry: combining two pools with that
// expiry never increases their worst loss. The only other blocks are naked
// residuals and legal pairs. Thus these integer allocations represent every
// legal partition, without enumerating partitions or individual contracts.
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
  ExactMargin(const Book& book, const MarginStock* stock, const MarginPolicy& policy)
      : stock_(stock), policy_(policy) {
    for (const auto* side : {&book.short_puts, &book.short_calls, &book.long_puts, &book.long_calls})
      for (const auto& unit : *side) {
        units_.push_back(&unit);
        if (side == &book.short_puts || side == &book.short_calls) shorts_.push_back(&unit);
      }
    std::stable_sort(units_.begin(), units_.end(), [&](const Unit* a, const Unit* b) {
      const auto key = [&](const Unit* u) { return u->leg ? u->leg->contract.osi_symbol() :
          short_unit(*u) ? std::string("!short") : std::string("!cover"); };
      return key(a) < key(b);
    });
    if (units_.size() > 128) throw detail::MarginWorkLimit{};
    for (const auto* u : units_) {
      a_.emplace_back(); b_.push_back(u->size);
      const auto cost = naked_price(*u);
      prices_.push_back(cost);
      base_ = base_ + cost * Rational(u->size);
    }
    if (stock_ && stock_->shares < 0) {
      const auto remainder = magnitude(stock_->shares) % kLot;
      odd_shares_ = short_shares(*stock_, remainder, policy_.house_percent);
      base_ = base_ + Rational(odd_shares_.micros());
    }
    std::map<Timestamp, std::size_t> expiries;
    if (policy.account != AccountType::Cash) {
      for (const auto* u : units_)
        if (u->leg && u->leg->quantity < 0) expiries.emplace(u->leg->contract.expiry_time(), 0);
    }
    for (auto& [expiry, index] : expiries) {
      index = pools_.size();
      Pool pool{expiry, {}, {0}, 0};
      // A minimum cannot occur only at a short strike: the payoff slope jumps
      // down there. Long strikes, zero and the call tail are sufficient.
      for (const auto* u : units_)
        if (u->leg && u->leg->quantity > 0 && u->leg->contract.expiry_time() >= expiry)
          pool.spots.push_back(milli(u->leg->contract.strike));
      std::sort(pool.spots.begin(), pool.spots.end());
      pool.spots.erase(std::unique(pool.spots.begin(), pool.spots.end()), pool.spots.end());
      for (std::size_t i = 0; i < pool.spots.size(); ++i) { pool.rows.push_back(a_.size()); a_.emplace_back(); b_.push_back(0); }
      pool.tail = a_.size(); a_.emplace_back(); b_.push_back(0);
      pools_.push_back(std::move(pool));
    }
    for (std::size_t u = 0; u < units_.size(); ++u) {
      if (!units_[u]->leg) continue;
      const auto& leg = *units_[u]->leg;
      for (const auto& [expiry, index] : expiries) {
        if (leg.quantity < 0 ? leg.contract.expiry_time() != expiry : leg.contract.expiry_time() < expiry) continue;
        const auto c = column({u, units_.size(), static_cast<int>(index), false, prices_[u]});
        a_[u][c] = 1;
        const auto& pool = pools_[index];
        for (std::size_t k = 0; k < pool.spots.size(); ++k)
          a_[pool.rows[k]][c] = intrinsic(leg.contract, pool.spots[k]).micros() / 100000 * (leg.quantity < 0 ? 1 : -1);
        if (leg.contract.type == OptionType::Call) a_[pool.tail][c] = leg.quantity < 0 ? 1 : -1;
      }
    }
    for (std::size_t i = 0; i < pools_.size(); ++i) {
      const auto c = column({units_.size(), units_.size(), static_cast<int>(i), true, -Rational(100000)});
      for (const auto row : pools_[i].rows) a_[row][c] = -1;
    }
    for (std::size_t i = 0; i < units_.size(); ++i) {
      for (std::size_t j = i + 1; j < units_.size(); ++j) {
        auto s = i, cover = j;
        if (!is_short(s)) std::swap(s, cover);
        if (!is_short(s)) continue;
        const auto& x = *units_[s]; const auto& y = *units_[cover];
        const bool straddle_pair = is_short(cover);
        Rational paired;
        if (straddle_pair) {
          if (policy.account != AccountType::Margin || !x.leg || !y.leg || x.leg->contract.type == y.leg->contract.type) continue;
          if (x.leg->contract.type == OptionType::Call) std::swap(s, cover);
          const auto av = buyback(*units_[s]), bv = buyback(*units_[cover]);
          const auto ap = prices_[s], bp = prices_[cover];
          paired = bp < ap ? ap + bv : ap < bp ? bp + av : ap + std::max(av, bv);
        } else {
          // Option verticals are already represented exactly by the pools.
          if (x.leg && y.leg) continue;
          if (x.stock && y.stock) continue;
          if (x.stock && (!y.leg || y.leg->contract.type != OptionType::Call)) continue;
          if (y.stock && (!x.leg || (y.stock->shares > 0) != (x.leg->contract.type == OptionType::Call))) continue;
          paired = y.stock && y.stock->shares < 0 ? buyback(x) : Rational(covered(x, y, 1).micros());
        }
        const auto saving = prices_[s] + prices_[cover] - paired;
        if (!(Rational{} < saving)) continue;
        const Column edge{s, cover, -1, false, saving};
        if (straddle_pair) pairs_.push_back(edge);
        else {
          const auto c = column(edge);
          a_[s][c] = 1; a_[cover][c] = 1;
        }
      }
    }
    // Integer allocations live on a monetary lattice. Reserves are maxima of
    // integer payoffs, so their step is the gcd of the payoff coefficients,
    // not one arbitrary reserve unit. This exact bound closes integrality gaps
    // without branching over dollar amounts (notably the seeded $250 books).
    for (std::size_t j = 0; j < columns_.size(); ++j) {
      auto amount = objective_[j];
      if (columns_[j].reserve) {
        std::int64_t step = 0;
        for (const auto row : pools_[static_cast<std::size_t>(columns_[j].pool)].rows)
          for (std::size_t k = 0; k < columns_.size(); ++k)
            if (!columns_[k].reserve) step = std::gcd(step, a_[row][k]);
        amount = Rational(step) * Rational(100000);
      }
      const auto integer = amount.ceil();
      if (!(amount == Rational(integer))) { lattice_ = false; break; }
      savings_step_ = std::gcd(savings_step_, integer);
    }
    for (const auto& pair : pairs_) {
      const auto integer = pair.saving.ceil();
      if (!(pair.saving == Rational(integer))) { lattice_ = false; break; }
      savings_step_ = std::gcd(savings_step_, integer);
    }
    savings_step_ = std::max<std::int64_t>(1, savings_step_);
    best_ = base_.ceil();
    best_x_.assign(columns_.size(), 0);
  }
  void incumbent(Money requirement, Parts parts) {
    if (requirement.micros() < best_) { best_ = requirement.micros(); incumbent_ = std::move(parts); }
  }
  Money solve(Parts* parts) {
    if (!columns_.empty() || !pairs_.empty()) search(0);
    if (parts) {
      if (!incumbent_.empty()) *parts = incumbent_;
      else explain(parts);
    }
    return Money::from_micros(best_);
  }
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
  Rational bound(std::vector<Rational> y, bool feasible, bool& price_pairs) const {
    {
      // The only columns without a capacity row are reserves. Their dual
      // constraint is sum(payoff prices) <= 100000 micro-dollars.
      for (const auto& pool : pools_) {
        Rational sum;
        for (const auto row : pool.rows) sum = sum + y[row];
        auto excess = sum - Rational(feasible ? 100000 : 0);
        for (const auto row : pool.rows) {
          if (!(Rational{} < excess)) break;
          const auto take = std::min(excess, y[row]);
          y[row] = y[row] - take; excess = excess - take;
        }
      }
    }
    for (std::size_t j = 0; j < columns_.size(); ++j) {
      Rational price;
      for (std::size_t i = 0; i < a_.size(); ++i) if (a_[i][j]) price = price + y[i] * Rational(a_[i][j]);
      const auto needed = feasible ? objective_[j] : Rational{};
      if (price < needed) {
        if (columns_[j].reserve) throw detail::MarginWorkLimit{};
        if (columns_[j].pool < 0 && is_short(columns_[j].other)) price_pairs = true;
        y[columns_[j].unit] = y[columns_[j].unit] + needed - price;
      }
    }
    for (const auto& pair : pairs_) {
      const auto price = y[pair.unit] + y[pair.other];
      const auto needed = feasible ? pair.saving : Rational{};
      if (price < needed) { price_pairs = true; y[pair.unit] = y[pair.unit] + needed - price; }
    }
    Rational result;
    for (std::size_t i = 0; i < b_.size(); ++i) if (b_[i]) result = result + y[i] * Rational(b_[i]);
    return result;
  }
  std::vector<Rational> basis_prices(const detail::MarginSimplex& lp,
      const std::vector<std::size_t>& active, const std::vector<std::size_t>& rows) {
    const auto columns = lp.basic_columns(), prices = lp.price_rows();
    const auto size = prices.size();
    if (columns.size() != size || size * size * (size + 1) > work_) throw detail::MarginWorkLimit{};
    work_ -= size * size * (size + 1);
    std::vector<std::vector<Rational>> equations(size, std::vector<Rational>(size + 1));
    for (std::size_t i = 0; i < size; ++i) {
      for (std::size_t j = 0; j < size; ++j) equations[i][j] = Rational(a_[rows[prices[j]]][active[columns[i]]]);
      equations[i][size] = objective_[active[columns[i]]];
    }
    for (std::size_t j = 0; j < size; ++j) {
      auto pivot = j;
      while (pivot < size && equations[pivot][j] == Rational{}) ++pivot;
      if (pivot == size) throw detail::MarginWorkLimit{};
      std::swap(equations[j], equations[pivot]);
      for (std::size_t i = j + 1; i < size; ++i) {
        if (equations[i][j] == Rational{}) continue;
        const auto ratio = equations[i][j] / equations[j][j];
        for (std::size_t k = j + 1; k <= size; ++k) equations[i][k] = equations[i][k] - ratio * equations[j][k];
        equations[i][j] = {};
      }
    }
    std::vector<Rational> y(b_.size());
    for (std::size_t i = size; i-- > 0;) {
      auto value = equations[i][size];
      for (std::size_t j = i + 1; j < size; ++j) value = value - equations[i][j] * y[rows[prices[j]]];
      value = value / equations[i][i];
      if (value < Rational{}) throw detail::MarginWorkLimit{};
      y[rows[prices[i]]] = value;
    }
    return y;
  }
  std::int64_t lower_bound(const Rational& upper_saving) const {
    if (!lattice_) return (base_ - upper_saving).ceil();
    const auto steps = -Rational((-upper_saving * Rational(1, savings_step_)).ceil());
    return (base_ - steps * Rational(savings_step_)).ceil();
  }
  void search(int stage = 2) {
    if (stage == 2 && !pairs_.empty()) {
      for (const auto& pair : pairs_) {
        const auto c = column(pair);
        a_[pair.unit][c] = 1; a_[pair.other][c] = 1;
      }
      pairs_.clear();
      best_x_.resize(columns_.size());
    }
    const bool pools_first = stage < 2;
    // Price pools before introducing the dense set of straddle edges. A dual
    // certificate checked against *all* columns can already prove optimality.
    // Otherwise the full model is solved; no relation is permanently omitted.
    std::vector<std::size_t> active;
    for (std::size_t j = 0; j < columns_.size(); ++j) {
      const auto& c = columns_[j];
      if (!pools_first || c.pool >= 0 || !is_short(c.other)) active.push_back(j);
    }
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < a_.size(); ++i) {
      bool include = stage > 0 || i < units_.size();
      if (!include) {
        for (const auto& pool : pools_)
          if (i == pool.tail || i == pool.rows.front() || i == pool.rows.back()) include = true;
      }
      if (include) rows.push_back(i);
    }
    detail::MarginSimplex::Matrix matrix(rows.size());
    std::vector<std::int64_t> rhs;
    for (const auto row : rows) rhs.push_back(b_[row]);
    std::vector<Rational> objective;
    std::vector<double> scale;
    double quantity_scale = 1;
    for (const auto* unit : units_) quantity_scale = std::max(quantity_scale, static_cast<double>(unit->size));
    for (const auto j : active) {
      objective.push_back(objective_[j]);
      scale.push_back((columns_[j].reserve ? 100000 : 1) * quantity_scale);
      for (std::size_t i = 0; i < rows.size(); ++i) matrix[i].push_back(a_[rows[i]][j]);
    }
    detail::MarginSimplex lp(matrix, rhs, objective, scale, quantity_scale, work_);
    const bool feasible = lp.solve();
    const auto proposed_dual = lp.dual(feasible);
    std::vector<Rational> dual(b_.size());
    for (std::size_t i = 0; i < rows.size(); ++i) dual[rows[i]] = proposed_dual[i];
    bool price_pairs = false;
    auto upper = bound(std::move(dual), feasible, price_pairs);
    // Large quantities amplify price quantization into several micros. Solve
    // the proposed basis's dual equations exactly before branching on that gap.
    if (feasible && lower_bound(upper) < best_ && Rational(best_) - Rational(lower_bound(upper)) < Rational(1000000)) {
      try {
        bool needs_pairs = false;
        const auto tighter = bound(basis_prices(lp, active, rows), true, needs_pairs);
        if (tighter < upper) { upper = tighter; price_pairs = needs_pairs; }
      } catch (const detail::MarginWorkLimit&) {
        // An oversized rational basis leaves the fixed-grid certificate valid.
      }
    }
    if (!feasible) {
      if (!(upper < Rational{})) throw detail::MarginWorkLimit{};
      return;
    }
    if (lower_bound(upper) >= best_) return;
    const auto proposed = lp.primal();
    std::vector<double> x(columns_.size());
    for (std::size_t j = 0; j < active.size(); ++j) x[active[j]] = proposed[j];
    std::vector<Quantity> integer(x.size());
    std::size_t split = x.size();
    double fraction = 0;
    int priority = 3;
    for (std::size_t j = 0; j < x.size(); ++j) {
      if (columns_[j].reserve) continue;
      if (!std::isfinite(x[j]) || x[j] < -1e-6 || x[j] > 1e12) throw detail::MarginWorkLimit{};
      integer[j] = static_cast<Quantity>(std::llround(x[j]));
      const auto part = std::abs(x[j] - static_cast<double>(integer[j]));
      const int rank = columns_[j].pool < 0 ? 2 : is_short(columns_[j].unit) ? 0 : 1;
      if (part > 1e-7 && (rank < priority || (rank == priority && part > fraction))) {
        split = j; fraction = part; priority = rank;
      }
    }
    if (split == x.size()) {
      // Recompute reserves from integer payoffs. Validate capacity and branch
      // constraints exactly; floats never enter the requirement or the parts.
      for (std::size_t j = 0; j < columns_.size(); ++j) {
        if (!columns_[j].reserve) continue;
        for (const auto row : pools_[static_cast<std::size_t>(columns_[j].pool)].rows) {
          Rational payoff;
          for (std::size_t k = 0; k < columns_.size(); ++k)
            if (!columns_[k].reserve && a_[row][k]) payoff = payoff + Rational(a_[row][k]) * Rational(integer[k]);
          integer[j] = std::max(integer[j], payoff.ceil());
        }
      }
      for (std::size_t i = 0; i < a_.size(); ++i) {
        Rational used;
        for (std::size_t j = 0; j < columns_.size(); ++j) if (a_[i][j]) used = used + Rational(a_[i][j]) * Rational(integer[j]);
        if (Rational(b_[i]) < used) throw detail::MarginWorkLimit{};
      }
      Rational cost = base_;
      for (std::size_t j = 0; j < columns_.size(); ++j) if (integer[j]) cost = cost - objective_[j] * Rational(integer[j]);
      if (cost.ceil() < best_) { best_ = cost.ceil(); best_x_ = std::move(integer); incumbent_.clear(); }
      if (lower_bound(upper) < best_) {
        if (pools_first) search(stage + 1);
        else throw detail::MarginWorkLimit{};
      }
      return;
    }
    if (stage == 0) { search(1); return; }
    if (stage == 1 && price_pairs) { search(2); return; }
    // Exhaust the two disjoint integer domains, lower first. A failed numerical
    // proposal or cap aborts the entire proof, returning the bounded allocator.
    const auto floor = static_cast<std::int64_t>(std::floor(x[split]));
    a_.emplace_back(columns_.size(), 0); a_.back()[split] = 1; b_.push_back(floor);
    search(stage);
    a_.back()[split] = -1; b_.back() = -floor - 1;
    search(stage);
    a_.pop_back(); b_.pop_back();
  }
  void explain(Parts* parts) const {
    std::vector<Quantity> left;
    for (const auto* u : units_) left.push_back(u->size);
    Rational cumulative;
    std::int64_t charged = 0;
    const auto emit = [&](MarginPartKind kind, std::vector<std::pair<std::string, Quantity>> legs, Rational cost) {
      cumulative = cumulative + cost;
      const auto next = cumulative.ceil();
      add(parts, kind, std::move(legs), Money::from_micros(next - charged)); charged = next;
    };
    for (std::size_t p = 0; p < pools_.size(); ++p) {
      std::vector<std::pair<std::string, Quantity>> legs;
      Rational cost;
      for (std::size_t j = 0; j < columns_.size(); ++j) {
        const auto& c = columns_[j]; const auto n = best_x_[j];
        if (c.pool != static_cast<int>(p) || !n) continue;
        if (c.reserve) cost = Rational(n) * Rational(100000);
        else { legs.push_back(leg_of(*units_[c.unit], n)); left[c.unit] -= n; }
      }
      if (!legs.empty()) emit(MarginPartKind::WorstLoss, std::move(legs), cost);
    }
    for (std::size_t j = 0; j < columns_.size(); ++j) {
      const auto& c = columns_[j]; const auto n = best_x_[j];
      if (c.pool >= 0 || !n) continue;
      const auto& u = *units_[c.unit]; const auto& v = *units_[c.other];
      const auto kind = is_short(c.other) ? MarginPartKind::Straddle : u.stock ? MarginPartKind::ProtectedShares : MarginPartKind::Covered;
      emit(kind, {leg_of(u, n), leg_of(v, n)}, (prices_[c.unit] + prices_[c.other] - c.saving) * Rational(n));
      left[c.unit] -= n; left[c.other] -= n;
    }
    for (std::size_t i = 0; i < units_.size(); ++i) {
      if (!left[i]) continue;
      const auto& u = *units_[i];
      if (u.stock && u.stock->shares < 0 && !is_short(i)) continue;
      const auto kind = !is_short(i) ? MarginPartKind::Long : u.stock ? MarginPartKind::ShortShares : unpaired_kind(u);
      emit(kind, {leg_of(u, left[i])}, prices_[i] * Rational(left[i]));
    }
    if (stock_ && magnitude(stock_->shares) % kLot)
      emit(stock_->shares < 0 ? MarginPartKind::ShortShares : MarginPartKind::Long,
           {shares_leg(*stock_, magnitude(stock_->shares) % kLot)}, Rational(odd_shares_.micros()));
  }
  const MarginStock* stock_;
  MarginPolicy policy_;
  std::vector<const Unit*> units_, shorts_;
  std::vector<Rational> prices_, objective_;
  Rational base_;
  Money odd_shares_;
  std::vector<Pool> pools_;
  std::vector<Column> columns_, pairs_;
  detail::MarginSimplex::Matrix a_;
  std::vector<std::int64_t> b_;
  std::int64_t best_ = 0, savings_step_ = 0;
  bool lattice_ = true;
  std::vector<Quantity> best_x_;
  Parts incumbent_;
  std::size_t work_ = 50000000;
};
