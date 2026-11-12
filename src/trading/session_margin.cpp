// What a paper account holds and can spend: its strategy or portfolio margin, buying
// power, the measures its rules read, and the snapshot it publishes.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
namespace {
std::optional<double> spot_for(const State& s, const std::string& symbol) {
  const auto it = s.valuations.find(symbol);
  if (it == s.valuations.end() || !valid_valuation(it->second)) return std::nullopt;
  return it->second.spot;
}
/// Whether a held stock's mark is its last print before a simulated halt still in
/// force at the regular close the account is valued at, printed in that session.
/// Prints during the halt are not taken, so the mark is the pre-halt print.
bool halted_at_close(const State& s, const std::string& symbol) {
  const auto it = s.stock_marks.find(symbol);
  const auto session = md::stock_session(s.time);
  if (it == s.stock_marks.end() || session.open || session.market_time == md::kInvalidTimestamp) return false;
  const auto close = session.market_time;
  const auto printed = md::stock_session(it->second.time);
  return halt_end(s, symbol) >= close && it->second.time < close && printed.open && printed.end == close;
}
bool opens(Quantity held_quantity, Quantity signed_fill) {
  return held_quantity == 0 || (held_quantity > 0) == (signed_fill > 0) || magnitude(signed_fill) > magnitude(held_quantity);
}
/// The premium, in dollars, of market_slices.
Money market_premium(const State& s, const std::string& symbol, Side side, Quantity quantity) {
  Money total;
  for (const auto& [price, n] : market_slices(s, symbol, side, quantity)) total = total + (price * 100) * n;
  return total;
}
}  // namespace
/// The held positions; a short's buy-back value is its mark, or its entry credit without one.
MarginBook held_book(const State& s) {
  MarginBook book;
  for (const auto& [symbol, p] : s.ledger.positions()) {
    if (p.quantity == 0) continue;
    Money value;
    if (p.quantity < 0) {
      const auto mark = s.marks.find(symbol);
      value = mark != s.marks.end() ? (mark->second.price * 100) * -p.quantity : -p.basis;
    }
    book[symbol] = {p.quantity, value};
  }
  return book;
}
namespace {
/// Trade `change` contracts of `symbol` in the book: shorts that remain keep
/// their share of the buy-back value, and contracts newly sold are valued at `price`.
void trade(MarginBook& book, const std::string& symbol, Quantity change, Money price) {
  const auto it = book.find(symbol);
  const auto [quantity, value] = it == book.end() ? std::pair<Quantity, Money>{0, {}} : it->second;
  const auto next = quantity + change;
  if (next == 0) { book.erase(symbol); return; }
  Money next_value;
  if (next < 0) {
    const Quantity kept = quantity < 0 ? std::min(-quantity, -next) : 0;
    next_value = (kept > 0 ? value.prorate(kept, -quantity) : Money{}) + (price * 100) * (-next - kept);
  }
  book[symbol] = {next, next_value};
}
/// Shares for strategy margin, at their last mark or their basis without one:
/// short shares hold 150% of their value, as a short sale does, long shares are
/// paid for, and either can cover options.
std::vector<MarginStock> margin_stocks(const State& s) {
  std::vector<MarginStock> stocks;
  for (const auto& [symbol, stock] : s.ledger.stocks()) {
    if (stock.shares == 0) continue;
    const auto mark = s.stock_marks.find(symbol);
    const Money value = mark == s.stock_marks.end() ? (stock.basis < Money{} ? -stock.basis : stock.basis)
                                                    : mark->second.price * magnitude(stock.shares);
    stocks.push_back({symbol, stock.shares, value});
  }
  return stocks;
}
struct Margin {
  Money requirement;  ///< What the book requires, as reported.
  Money held;         ///< What it holds out of cash: the requirement less the positions' value under portfolio margin.
};
/// The positions' signed value at their marks (shorts negative), shares included;
/// a short without a mark counts at its buy-back value and a long without one at nothing.
/// `scanned` leaves out the series with an adjusted deliverable, which a portfolio scan
/// does not reprice.
Money position_value(const State& s, const MarginBook& book, bool scanned = false) {
  Money value;
  for (const auto& [symbol, entry] : book) {
    if (scanned && !standard_deliverable(deliverable_of(s, symbol))) continue;
    const auto mark = s.marks.find(symbol);
    if (mark != s.marks.end()) value = value + (mark->second.price * 100) * entry.first;
    else if (entry.first < 0) value = value - entry.second;
  }
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) value = value + *price * stock.shares;
  return value;
}
std::vector<MarginLeg> margin_legs(const State& s, const MarginBook& book) {
  std::vector<MarginLeg> legs;
  for (const auto& [symbol, entry] : book) legs.push_back({s.contracts.at(symbol), entry.first, entry.second, spot_for(s, symbol)});
  return legs;
}
/// Fresh share prices for the portfolio-margin scan.
std::map<std::string, double> share_prices(const State& s) {
  std::map<std::string, double> prices;
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) prices[symbol] = price->dollars();
  return prices;
}
MarginPolicy allocation_policy(const State& s) {
  auto policy = margin_policy(s.config.rules);
  policy.exact = s.exact_margin;
  policy.revised_budget = s.revised_margin_budget;
  policy.cache = s.margin_cache.get();
  return policy;
}
}  // namespace
std::vector<LotGroup> lot_groups(const State& s, std::vector<MarginLeg> legs, std::vector<MarginStock> stocks) {
  std::vector<LotGroup> groups(1);
  if (s.deliverables.empty()) {
    groups[0].legs = std::move(legs);
    groups[0].stocks = std::move(stocks);
    return groups;
  }
  std::map<std::tuple<std::string, Quantity, std::int64_t>, std::size_t> index;
  for (auto& leg : legs) {
    const auto symbol = leg.contract.osi_symbol();
    const auto d = deliverable_of(s, symbol);
    if (standard_deliverable(d)) { groups[0].legs.push_back(std::move(leg)); continue; }
    const auto key = std::make_tuple(leg.contract.underlying, d.shares, d.cash.micros());
    auto [it, added] = index.try_emplace(key, groups.size());
    if (added) groups.push_back({d.shares, {}, {}, {}});
    auto& group = groups[it->second];
    // A deliverable of cash only pays a fixed amount: no shares cover it and no
    // standard series is its equivalent.
    if (d.shares == 0) {
      group.symbols[symbol] = symbol;
      group.legs.push_back(std::move(leg));
      continue;
    }
    MarginLeg equivalent = leg;
    equivalent.contract.strike = effective_strike(s, leg.contract).dollars();
    equivalent.contract.standard = true;
    equivalent.value = leg.value.prorate(100, d.shares);
    group.symbols[equivalent.contract.osi_symbol()] = symbol;
    group.legs.push_back(std::move(equivalent));
  }
  for (auto& stock : stocks) {
    for (std::size_t g = 1; g < groups.size() && stock.shares != 0; ++g) {
      auto& group = groups[g];
      if (group.shares == 0 || group.legs.empty() || group.legs.front().contract.underlying != stock.underlying) continue;
      // Long shares cover short calls, short shares short puts, a lot a contract.
      Quantity shorts = 0;
      for (const auto& leg : group.legs)
        if (leg.quantity < 0 && (leg.contract.type == pricing::OptionType::Call) == (stock.shares > 0)) shorts -= leg.quantity;
      const auto lots = std::min(shorts, magnitude(stock.shares) / group.shares);
      if (lots == 0) continue;
      const auto taken = lots * group.shares;
      const auto value = stock.value.prorate(taken, magnitude(stock.shares));
      group.stocks.push_back({stock.underlying, (stock.shares > 0 ? 1 : -1) * lots * 100, value.prorate(100, group.shares)});
      stock.value = stock.value - value;
      stock.shares -= (stock.shares > 0 ? 1 : -1) * taken;
    }
    if (stock.shares != 0) groups[0].stocks.push_back(std::move(stock));
  }
  return groups;
}
namespace {
/// What a group of series that deliver cash only requires: each short's buy-back
/// value, the fixed amount it owes less the strike, as its payoff cannot move.
Money cash_only_requirement(const LotGroup& group) {
  Money total;
  for (const auto& leg : group.legs)
    if (leg.quantity < 0) total = total + leg.value;
  return total;
}
/// margin_requirement over the lot groups, each scaled back to its deliverable.
Money grouped_requirement(const std::vector<LotGroup>& groups, const MarginPolicy& policy, bool standard = true) {
  Money total;
  for (std::size_t g = standard ? 0 : 1; g < groups.size(); ++g) {
    const auto& group = groups[g];
    if (group.legs.empty() && group.stocks.empty()) continue;
    total = total + (group.shares == 0 ? cash_only_requirement(group)
                                       : margin_requirement(group.legs, group.stocks, policy).prorate(group.shares, 100));
  }
  return total;
}
Margin margin_of(const State& s, const MarginBook& book) {
  if (!s.deliverables.empty()) {
    const auto groups = lot_groups(s, margin_legs(s, book), margin_stocks(s));
    if (groups.size() > 1) {
      // Portfolio margin scans the standard series and shares; adjusted series take
      // strategy margin on top, which the scan's repricing does not cover. Only what
      // the scan covers is taken from equity: strategy margin already holds a short
      // adjusted series' buy-back and has a long one paid for. Replay drivers 1-8 took
      // the adjusted series' value too, crediting a long one and charging a short twice.
      if (s.config.rules.margin == MarginMode::Portfolio) {
        const auto adjusted = grouped_requirement(groups, allocation_policy(s), false);
        if (const auto scanned = portfolio_margin_requirement(groups[0].legs, s.valuations, s.time,
            s.config.limits.max_valuation_age, s.ledger.stocks(), share_prices(s), margin_policy(s.config.rules)))
          return {*scanned + adjusted, *scanned + adjusted - position_value(s, book, s.revised_corporate_actions)};
      }
      Money minimum;
      if (s.config.rules.margin == MarginMode::Portfolio)
        for (const auto& [symbol, entry] : book)
          minimum = minimum + Money::from_double(0.375 * s.contracts.at(symbol).multiplier) * magnitude(entry.first);
      const auto strategy = grouped_requirement(groups, allocation_policy(s)) + minimum;
      return {strategy, strategy};
    }
  }
  const auto legs = margin_legs(s, book);
  Money minimum;
  if (s.config.rules.margin == MarginMode::Portfolio) {
    // Portfolio margin is taken from equity: cash already holds shorts' credits and
    // paid for longs, so the positions' value is what they are worth on top of it.
    if (const auto scanned = portfolio_margin_requirement(legs, s.valuations, s.time,
        s.config.limits.max_valuation_age, s.ledger.stocks(), share_prices(s), margin_policy(s.config.rules)))
      return {*scanned, *scanned - position_value(s, book)};
    // An incomplete scan falls back to strategy margin plus the option minimum.
    // The snapshot flags missing data and user fills require fresh valuations.
    for (const auto& leg : legs)
      minimum = minimum + Money::from_double(0.375 * leg.contract.multiplier) * magnitude(leg.quantity);
  }
  const auto strategy = margin_requirement(legs, margin_stocks(s), allocation_policy(s)) + minimum;
  return {strategy, strategy};
}
}  // namespace
Money requirement(const State& s, const MarginBook& book) { return margin_of(s, book).held; }
namespace {
/// The held positions' requirement by underlying, with what holds it, as
/// margin_of reckons it: an incomplete portfolio scan falls back to strategy
/// margin's parts, each underlying adding the option minimum.
std::vector<MarginUnderlying> margin_detail(const State& s) {
  if (!s.deliverables.empty()) {
    const auto book = held_book(s);
    const auto groups = lot_groups(s, margin_legs(s, book), margin_stocks(s));
    if (groups.size() > 1) {
      // Each adjusted group's parts, scaled back to its deliverable and named by its series.
      std::vector<MarginUnderlying> detail;
      const bool portfolio = s.config.rules.margin == MarginMode::Portfolio;
      bool scanned_whole = false;
      if (portfolio)
        if (auto scanned = portfolio_margin_breakdown(groups[0].legs, s.valuations, s.time, s.config.limits.max_valuation_age,
            s.ledger.stocks(), share_prices(s), margin_policy(s.config.rules))) {
          detail = std::move(*scanned);
          scanned_whole = true;
        }
      if (detail.empty() && (!groups[0].legs.empty() || !groups[0].stocks.empty()))
        detail = margin_breakdown(groups[0].legs, groups[0].stocks, allocation_policy(s));
      for (std::size_t g = 1; g < groups.size(); ++g) {
        const auto& group = groups[g];
        if (group.shares == 0) {
          MarginUnderlying item{group.legs.front().contract.underlying, cash_only_requirement(group), {}, std::nullopt, "exact"};
          for (const auto& leg : group.legs)
            if (leg.quantity < 0) item.parts.push_back({MarginPartKind::Naked, {{leg.contract.osi_symbol(), leg.quantity}}, leg.value});
          const auto same = std::find_if(detail.begin(), detail.end(), [&](const auto& u) { return u.underlying == item.underlying; });
          if (same == detail.end()) { detail.push_back(std::move(item)); continue; }
          same->requirement = same->requirement + item.requirement;
          same->parts.insert(same->parts.end(), item.parts.begin(), item.parts.end());
          continue;
        }
        for (auto item : margin_breakdown(group.legs, group.stocks, allocation_policy(s))) {
          item.requirement = item.requirement.prorate(group.shares, 100);
          for (auto& part : item.parts) {
            part.requirement = part.requirement.prorate(group.shares, 100);
            for (auto& [symbol, quantity] : part.legs) {
              if (const auto named = group.symbols.find(symbol); named != group.symbols.end()) symbol = named->second;
              else quantity = quantity * group.shares / 100;  // shares, in the deliverable's lots
            }
          }
          const auto same = std::find_if(detail.begin(), detail.end(), [&](const auto& u) { return u.underlying == item.underlying; });
          if (same == detail.end()) { detail.push_back(std::move(item)); continue; }
          same->requirement = same->requirement + item.requirement;
          same->parts.insert(same->parts.end(), item.parts.begin(), item.parts.end());
        }
      }
      // An incomplete scan falls back to strategy margin plus the option minimum on
      // every option held, standard and adjusted, as margin_of reckons it.
      if (portfolio && !scanned_whole)
        for (const auto& [symbol, entry] : book) {
          const auto& contract = s.contracts.at(symbol);
          const auto minimum = Money::from_double(0.375 * contract.multiplier) * magnitude(entry.first);
          const auto same = std::find_if(detail.begin(), detail.end(), [&](const auto& u) { return u.underlying == contract.underlying; });
          if (same != detail.end()) same->requirement = same->requirement + minimum;
          else detail.push_back({contract.underlying, minimum, {}, std::nullopt, "exact"});
        }
      return detail;
    }
  }
  const auto legs = margin_legs(s, held_book(s));
  const bool portfolio = s.config.rules.margin == MarginMode::Portfolio;
  if (portfolio)
    if (auto scanned = portfolio_margin_breakdown(legs, s.valuations, s.time, s.config.limits.max_valuation_age,
        s.ledger.stocks(), share_prices(s), margin_policy(s.config.rules)))
      return std::move(*scanned);
  auto detail = margin_breakdown(legs, margin_stocks(s), allocation_policy(s));
  if (portfolio)
    for (auto& item : detail)
      for (const auto& leg : legs)
        if (leg.contract.underlying == item.underlying)
          item.requirement = item.requirement + Money::from_double(0.375 * leg.contract.multiplier) * magnitude(leg.quantity);
  return detail;
}
}  // namespace
/// Buying power that positions do not hold: cash less their margin requirement.
Money free_power(const State& s) { return s.ledger.account().cash - requirement(s, held_book(s)); }
namespace {
struct Use {
  Money reservation;   ///< Fees plus any net cost, never less than the fees.
  bool uses = false;   ///< Filling would reduce free buying power.
};
/// An order's use of buying power if it filled now: its fees, plus the change
/// in the margin requirement from `before` to `after`, plus the premium it pays
/// (`cash`, negative when it receives premium). An order that releases more
/// than it costs reserves only its fees.
Use use_from(Money change, Money fees) { return {fees + std::max(Money{}, change), fees + change > Money{}}; }
Use use_of(const State& s, const MarginBook& before, const MarginBook& after, Money cash, Money fees) {
  return use_from(requirement(s, after) - requirement(s, before) + cash, fees);
}
/// A multi-leg order on the held positions: legs sold short at their marks,
/// and the net debit (or credit) per unit at its limit or the current far sides.
Use combo_use(const State& s, const Order& o, const MarginBook& book) {
  const auto units = o.remaining();
  auto after = book;
  Money fees;
  for (const auto& leg : o.request.legs) {
    const auto contracts = signed_contracts(leg, units);
    fees = fees + fees_for(s, leg.symbol, magnitude(contracts), closing_contracts(held(s, leg.symbol), leg.side, magnitude(contracts)),
                           commission_paid(s, o, leg.symbol)).total();
    const auto mark = s.marks.find(leg.symbol);
    trade(after, leg.symbol, contracts, mark != s.marks.end() ? mark->second.price : Money{});
  }
  Money premium;
  if (o.request.walk) premium = (o.request.walk->limit * 100) * units;
  else if (o.request.limit_price) premium = (*o.request.limit_price * 100) * units;
  else for (const auto& leg : o.request.legs) {
    const auto cost = market_premium(s, leg.symbol, leg.side, units * leg.ratio);
    premium = leg.side == Side::Buy ? premium + cost : premium - cost;
  }
  return use_of(s, book, after, premium, fees);
}
}  // namespace
/// Positions hold their margin requirement (margin_requirement, spreads netted).
/// Each working order reserves what filling it now would cost (use_of): fees,
/// the change in margin on the held positions and the premium it pays, less
/// what it receives, never below the fees. Single-leg orders see the positions
/// less the contracts earlier orders already claim to close, in acceptance
/// order, so two sells cannot both claim the same long. Bracket exits stay
/// within their position and reserve only fees. Orders without a limit use
/// market_slices; a marketable buy limit reserves its expected fill there, without
/// fill latency, and an armed buy stop at least its trigger level.
PowerDetail buying_power(const State& s, OrderId focus) {
  PowerDetail out;
  const auto book = held_book(s);
  const auto margin = margin_of(s, book);
  // A single-leg order's books differ from the held one in its own contract alone,
  // so the requirement of each such book is computed once: a ladder of working
  // orders on one contract costs one margin computation, not one per order.
  std::map<std::tuple<std::string, Quantity, std::int64_t>, Money> requirements;
  std::map<std::string, bool> quotable;
  const auto requirement_with = [&](const MarginBook& changed, const std::string& symbol) {
    const auto it = changed.find(symbol);
    const auto key = it == changed.end() ? std::tuple<std::string, Quantity, std::int64_t>{symbol, 0, 0}
                                         : std::tuple<std::string, Quantity, std::int64_t>{symbol, it->second.first, it->second.second.micros()};
    if (const auto saved = requirements.find(key); saved != requirements.end()) return saved->second;
    return requirements.emplace(key, requirement(s, changed)).first->second;
  };
  std::map<std::string, std::pair<Quantity, Quantity>> capacity;
  Money reserved;
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (o.remaining() <= 0 || shadowed(s, o)) continue;
    const auto remaining = o.remaining();
    Use use;
    Quantity opening = 0;
    if (multi_leg(o.request)) {
      if (kept_within(o)) {
        Money fees;
        for (const auto& leg : o.request.legs)
          fees = fees + fees_for(s, leg.symbol, remaining * leg.ratio, remaining * leg.ratio, commission_paid(s, o, leg.symbol)).total();
        use = {fees, false};
      } else use = combo_use(s, o, book);
      for (const auto& leg : o.request.legs)
        if (opens(held(s, leg.symbol), signed_contracts(leg, remaining))) opening = remaining;
    } else {
      const auto& symbol = o.request.symbol;
      if (!s.contracts.contains(symbol)) continue;
      const auto q = held(s, symbol);
      auto& [long_left, short_left] = capacity.try_emplace(symbol, std::max<Quantity>(q, 0), std::max<Quantity>(-q, 0)).first->second;
      const bool buy = o.request.side == Side::Buy;
      const auto closing = kept_within(o) ? remaining : std::min(remaining, buy ? short_left : long_left);
      const Money fees = fees_for(s, symbol, remaining, closing, commission_paid(s, o, symbol)).total();
      if (kept_within(o)) {
        // Bracket exits and reduce-only closes stay within the position, so they only
        // ever close; they leave closing capacity to ordinary orders such as a manual close.
        use = {fees, false};
      } else {
        Money premium = o.request.walk ? (o.request.walk->limit * 100) * remaining
                        : o.request.limit_price ? (*o.request.limit_price * 100) * remaining
                                              : market_premium(s, symbol, o.request.side, remaining);
        if (buy && o.status == OrderStatus::Armed) {
          // An armed buy stop pays at least its level once the ask reaches it.
          const auto& t = *o.request.trigger;
          if (!o.request.limit_price && t.source == TriggerSource::Option && t.direction == TriggerDirection::AtOrAbove)
            premium = std::max(premium, (t.level * 100) * remaining);
        } else if (buy && o.request.limit_price && !o.request.walk && s.config.rules.fill_latency_ms == 0) {
          // A marketable buy limit pays the expected fill, not its limit; one below
          // the ask rests and pays its limit.
          const auto quoted = s.books.find(symbol);
          if (quoted != s.books.end() && quoted->second.quote.ask && *o.request.limit_price >= *quoted->second.quote.ask) {
            auto usable = quotable.find(symbol);
            if (usable == quotable.end()) usable = quotable.emplace(symbol, quote_check(s, symbol).ok()).first;
            if (usable->second) premium = std::min(premium, market_premium(s, symbol, o.request.side, remaining));
          }
        }
        auto before = book;
        if (q > 0 && !buy) trade(before, symbol, long_left - q, {});
        if (q < 0 && buy) trade(before, symbol, -q - short_left, {});
        auto& left = buy ? short_left : long_left;
        left -= closing;
        opening = remaining - closing;
        auto after = before;
        trade(after, symbol, buy ? remaining : -remaining, premium.prorate(1, 100 * remaining));
        use = use_from(requirement_with(after, symbol) - requirement_with(before, symbol) + (buy ? premium : -premium), fees);
      }
    }
    reserved = reserved + use.reservation;
    if (o.id == focus) { out.focus_reservation = use.reservation; out.focus_opening = opening; out.focus_uses = use.uses; }
  }
  // Working share orders reserve what filling them at their bound would take.
  reserved = reserved + stock_orders_reserved(s);
  out.total.reserved = reserved;
  out.total.short_requirement = margin.requirement;
  out.total.available = s.ledger.account().cash - margin.held - reserved;
  return out;
}
/// Fill `units` of `request` into `after` (a copy of `before`) now: every leg at
/// market through impact blocks, as a real fill would walk them, and a limit
/// order at its limit's debit or credit instead, never an impossible fill at
/// today's far sides. This also keeps size-to-floor conservative at limits.
ProjectedFill project_fill(State& after, const State& before, const OrderRequest& request, Quantity units, const Order& order) {
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  ProjectedFill result;
  for (const auto& leg : legs) {
    const auto quantity = signed_contracts(leg, units);
    const auto fees = fees_for(before, leg.symbol, magnitude(quantity),
                               closing_contracts(held(before, leg.symbol), leg.side, magnitude(quantity)),
                               commission_paid(before, order, leg.symbol));
    auto fee = fees.total();
    result.fees = result.fees + fee;
    result.itemized += fees;
    for (const auto& [price, n] : market_slices(before, leg.symbol, leg.side, magnitude(quantity))) {
      const auto contracts = quantity < 0 ? -n : n;
      result.premium = result.premium + (price * 100) * contracts;
      after.ledger.fill(before.contracts.at(leg.symbol), contracts, price, fee);
      fee = {};
    }
  }
  if (request.limit_price) {
    const auto limit = request.walk ? request.walk->limit : *request.limit_price;
    const auto limit_premium = (limit * 100) * units * (legs.size() == 1 && request.side == Side::Sell ? -1 : 1);
    auto account = after.ledger.account();
    account.cash = account.cash + result.premium - limit_premium;
    after.ledger = Ledger::restore(account, after.ledger.positions(), after.ledger.stocks());
    result.premium = limit_premium;
  }
  return result;
}
/// Whether `after`, the account once working order `o` has filled in part or in
/// full, leaves more buying power available than cancelling `o`: its fill changes
/// what the other working orders reserve, as a long bought back re-covers a
/// working sell that had become naked.
bool frees_power(const State& s, const Order& o, const State& after) {
  State without = s;
  without.orders.mut(static_cast<std::size_t>(o.id - 1)).status = OrderStatus::Cancelled;
  return buying_power(after).total.available > buying_power(without).total.available;
}
/// Whether filling working order `o` now, in full, would free buying power (frees_power).
bool fill_frees_power(const State& s, const Order& o) {
  State filled = s;
  auto& order = filled.orders.mut(static_cast<std::size_t>(o.id - 1));
  order.status = OrderStatus::Filled;
  order.filled_quantity = order.request.quantity;
  (void)project_fill(filled, s, o.request, o.remaining(), o);
  return frees_power(s, o, filled);
}
/// The personal soft floor now: an absolute level, or a share of the plan's
/// drawdown above its floor, whichever is higher.
/// The soft floor `personal` sets above a plan floor of `plan_floor`.
std::optional<Money> soft_floor_at(const Guardrails& personal, const AccountRules& rules, Money plan_floor) {
  std::optional<Money> floor;
  if (personal.soft_floor > Money{}) floor = personal.soft_floor;
  if (personal.soft_floor_percent > 0 && rules.max_drawdown > Money{}) {
    const auto level = plan_floor + rules.max_drawdown.prorate(personal.soft_floor_percent, 100);
    floor = floor ? std::max(*floor, level) : level;
  }
  return floor;
}
std::optional<Money> soft_floor_of(const State& s) { return soft_floor_at(s.config.guardrails, s.config.rules, s.evaluation.floor); }
std::map<std::string, std::vector<std::string>> risk_tags(const State& s) {
  std::map<std::string, std::vector<std::string>> result;
  if (s.config.limits.tag_overrides.empty() && s.config.limits.playbook_overrides.empty()) return result;
  // Every round trip, rebuilt from the account's whole history only when needed: the
  // review builder holds the open ones once it has applied every fill and closure.
  std::optional<std::vector<Lifecycle>> all;
  const auto lives = [&]() -> const std::vector<Lifecycle>& {
    if (!all) all = lifecycles(s.fills, s.closures, s.contracts);
    return *all;
  };
  const auto tags = [&](const Lifecycle& life) {
    std::set<std::string> found;
    for (const auto id : life.fills) {
      if (!id || id > s.fills.size()) continue;
      const auto order = s.fills[id - 1].order_id;
      if (order && order <= s.orders.size())
        for (const auto& tag : s.orders[order - 1].request.tags) found.insert(tag);
    }
    for (const auto& id : {std::to_string(life.first_fill), trade_group(life, s.groups)})
      if (const auto note = s.annotations.find(id); note != s.annotations.end())
        found.insert(note->second.tags.begin(), note->second.tags.end());
    return found;
  };
  if (const auto& built = s.reviewing; built.ready && built.fills == s.fills.size() && built.closures == s.closures.size()) {
    for (const auto& [symbol, open] : built.builder.open) {
      const auto found = tags(open.life);
      result[symbol] = {found.begin(), found.end()};
    }
  } else {
    for (const auto& life : lives()) if (!life.closed) {
      const auto found = tags(life);
      result[life.symbol] = {found.begin(), found.end()};
    }
  }
  // Delivered shares retain their originating option trade's scope. Fungible
  // positions shared by tags count in each scope; retagging does not erase entry tags.
  for (const auto& life : share_lifecycles(s.stock_fills)) if (!life.closed) {
    std::set<std::string> found;
    for (const auto id : life.fills) {
      const auto& fill = s.stock_fills[id - 1];
      if (fill.option.empty()) continue;  // Only delivered shares name an option trade.
      for (const auto& option : lives())
        if (option.symbol == fill.option && option.opened <= fill.time && (!option.closed || *option.closed >= fill.time)) {
          const auto original = tags(option); found.insert(original.begin(), original.end());
        }
    }
    if (const auto trip = s.trips.find(life.symbol); trip != s.trips.end())
      if (const auto note = s.annotations.find(trip->second); note != s.annotations.end())
        found.insert(note->second.tags.begin(), note->second.tags.end());
    result[life.symbol] = {found.begin(), found.end()};
  }
  return result;
}
Measures measure(const State& s) {
  const DeliverablesScope adjusted(&s.deliverables);
  Measures out;
  out.equity = s.ledger.account().cash;
  for (const auto& [symbol, position] : s.ledger.positions()) {
    MarkedPosition p;
    p.position = position;
    p.awaiting_settlement = s.time >= position.contract.expiry_time();
    const auto it = s.marks.find(symbol);
    if (it != s.marks.end()) {
      p.mark = it->second.price;
      p.mark_time = it->second.time;
      p.mark_age = s.time - p.mark_time;
      p.market_value = (*p.mark * 100) * position.quantity;
      p.unrealised = *p.market_value - position.basis;
      p.fresh = !p.awaiting_settlement && marked_now(s, symbol);
      out.equity = out.equity + *p.market_value;
      out.unrealised = out.unrealised + *p.unrealised;
    }
    p.no_bid = p.awaiting_settlement || asks_only(s, symbol);
    if (const auto book = s.books.find(symbol); book != s.books.end()) p.modelled_mark = it != s.marks.end() && book->second.quote.modelled;
    p.do_not_exercise = s.do_not_exercise.contains(symbol);
    out.valuation_complete &= p.fresh;
    out.positions.push_back(std::move(p));
  }
  for (const auto& [symbol, stock] : s.ledger.stocks()) {
    MarkedStock m;
    m.position = stock;
    if (const auto it = s.stock_marks.find(symbol); it != s.stock_marks.end()) {
      m.mark = it->second.price;
      m.mark_time = it->second.time;
      m.market_value = it->second.price * stock.shares;
      m.unrealised = *m.market_value - stock.basis;
      m.fresh = stock_price(s, symbol).has_value();
      m.halted = !m.fresh && halted_at_close(s, symbol);
      out.equity = out.equity + *m.market_value;
      out.unrealised = out.unrealised + *m.unrealised;
    }
    out.valuation_complete &= m.fresh;
    out.stocks.push_back(std::move(m));
  }
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) out.stock_prices[symbol] = price->dollars();
  // Reachable exposure counts each open bracket pair once: the pair's later exit
  // is left out while both are open.
  std::vector<const Order*> working;
  for (const auto id : open_ids(s))
    if (const auto& o = s.orders[id - 1]; !shadowed(s, o)) working.push_back(&o);
  out.risk = portfolio_risk(s.ledger, working, s.contracts, s.valuations, s.config.limits, s.time, out.stock_prices, s.config.scenarios, risk_tags(s));
  out.risk.daily_loss = std::max(Money{}, s.start_equity - out.equity);
  out.risk.kill_latched = s.kill;
  out.risk.kill_reason = s.kill_reason;
  out.risk.limits_revision = s.limits_revision;
  out.soft_floor = soft_floor_of(s);
  out.buying_power = buying_power(s).total;
  return out;
}
/// The equity closing every position now would leave (TradingSnapshot::exit_equity).
Money exit_equity_of(const State& s, const TradingSnapshot& out) {
  auto value = out.equity;
  for (const auto& p : out.positions) {
    const auto& position = p.position;
    const auto symbol = position.contract.osi_symbol();
    if (p.awaiting_settlement || !p.market_value) continue;
    const auto size = magnitude(position.quantity);
    const auto closing = market_premium(s, symbol, position.quantity > 0 ? Side::Sell : Side::Buy, size);
    const auto fees = fees_for(s, symbol, size, size, {});
    value = value - *p.market_value + (position.quantity > 0 ? closing : -closing) - fees.total();
  }
  return value;
}
TradingSnapshot snapshot_of(const State& s) {
  const DeliverablesScope adjusted(&s.deliverables);
  auto m = measure(s);
  TradingSnapshot out;
  out.account_version = s.version;
  out.time = s.time;
  out.account = s.ledger.account();
  out.equity = m.equity;
  out.start_of_day_equity = s.start_equity;
  out.unrealised = m.unrealised;
  out.valuation_complete = m.valuation_complete;
  out.recent_orders = s.orders;
  out.recent_fills = s.fills;
  const bool data = m.valuation_complete && m.risk.complete;
  for (const auto id : open_ids(s)) {
    out.open_orders.push_back(s.orders[id - 1]);
    if (auto wait = waiting_for(s, s.orders[id - 1], data)) out.waiting.emplace(id, std::move(*wait));
  }
  out.kill_history = s.kill_history;
  out.kill_reset = reset_check(s, m);
  out.positions = std::move(m.positions);
  out.stocks = std::move(m.stocks);
  out.risk = std::move(m.risk);
  out.scenarios = scenario_grid(s.ledger, s.valuations, s.config.scenarios, s.time, s.config.limits.max_valuation_age, m.stock_prices);
  if (!out.valuation_complete) out.quality_flags.push_back(Reason::STALE_QUOTE);
  if (!out.risk.complete || !out.scenarios.complete) out.quality_flags.push_back(Reason::MISSING_VALUATION);
  if (std::any_of(out.positions.begin(), out.positions.end(), [](const auto& p) { return p.awaiting_settlement; }))
    out.quality_flags.push_back(Reason::AWAITING_SETTLEMENT);
  out.evaluation = s.evaluation;
  out.pending_limits = s.pending_limits;
  out.pending_guardrails = s.pending_guardrails;
  out.guardrails = s.guardrails;
  out.pending_applied_at = s.pending_applied_at;
  out.soft_floor = m.soft_floor;
  out.buying_power = m.buying_power;
  out.margin = margin_detail(s);
  out.settlements = s.settlements;
  out.closures = s.closures;
  out.attempts = s.attempts;
  out.fee_charged = s.fee_charged;
  out.payouts_received = s.payouts_received;
  out.stock_fills = s.stock_fills;
  out.stock_orders = s.stock_orders;
  out.deliverables = s.deliverables;
  out.corporate_actions = s.corporate_actions;
  out.dividends = s.dividends;
  out.cash_flows = s.cash_flows;
  out.closing_prints = s.closing_prints;
  out.annotations = s.annotations;
  out.day_notes = s.day_notes;
  out.trade_reviews = s.trade_reviews;
  out.strategy_reviews = s.strategy_reviews;
  out.groups = s.groups;
  out.group_reviews = s.group_reviews;
  // Today's P&L by Greek: the finished stretches, and the open ones to the marks now.
  // Each round trip's, from its start: its finished stretches and the open one.
  out.attributions = s.explained;
  out.trip_attributions = s.trip_attribution;
  for (const auto& [symbol, reference] : s.references) {
    Attribution open;
    if (s.contracts.contains(symbol)) {
      const auto mark = s.marks.find(symbol);
      if (mark == s.marks.end()) continue;
      open = explain(s.contracts.at(symbol), reference, mark->second.price, valuation_of(s, symbol));
    } else if (const auto mark = s.stock_marks.find(symbol); mark != s.stock_marks.end()) {
      open = explain_stock(reference, mark->second.price);
    } else {
      continue;
    }
    out.attributions[symbol] += open;
    if (const auto trip = s.trips.find(symbol); trip != s.trips.end()) out.trip_attributions[trip->second] += open;
  }
  for (const auto& [symbol, attribution] : out.attributions) out.attribution += attribution;
  out.exit_equity = exit_equity_of(s, out);
  out.alerts = s.alerts.items;
  return out;
}
}  // namespace session_internal
}  // namespace openport::trading
