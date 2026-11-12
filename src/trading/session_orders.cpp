// Placing and changing option orders, and previewing them: projected fills, what-if
// accounts and sizing.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
/// A bounded same-expiry payoff is piecewise linear: its minimum is at zero or
/// a strike, unless its terminal call slope is negative (unbounded loss).
std::optional<Money> expiry_loss(const State& s, const OrderRequest& request, Money premium, Money fees) {
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  const auto& first = s.contracts.at(legs.front().symbol);
  Quantity call_slope = 0;
  std::vector<Money> knots{Money{}};
  for (const auto& leg : legs) {
    const auto& contract = s.contracts.at(leg.symbol);
    if (contract.expiry_time() != first.expiry_time() || contract.underlying != first.underlying) return {};
    if (contract.type == pricing::OptionType::Call) call_slope += leg.side == Side::Buy ? leg.ratio : -leg.ratio;
    knots.push_back(effective_strike(s, contract));
  }
  if (call_slope < 0) return {};
  std::optional<Money> minimum;
  for (const auto spot : knots) {
    Money payoff;
    for (const auto& leg : legs) {
      const auto& contract = s.contracts.at(leg.symbol);
      const auto intrinsic = std::max(Money{}, unit_exercise_value(s, contract, spot));
      payoff = payoff + (intrinsic * contract.multiplier) * signed_contracts(leg, request.quantity);
    }
    minimum = minimum ? std::min(*minimum, payoff) : payoff;
  }
  return std::max(Money{}, premium + fees - *minimum);
}
/// Open order `id` in `after`, a copy of `before` that holds it on the terms to
/// preview, which the checks decided `decision`: what it reserves there, and the
/// account once its remaining units fill in full.
PreviewProjection project_working(const State& before, State after_state, OrderId id, Decision decision) {
  PreviewProjection projection{{}, std::move(after_state)};
  auto& result = projection.result;
  auto& after = projection.projected;
  const auto snapshot = snapshot_of(before);
  result.buying_power_before = snapshot.buying_power.available;
  result.decision = std::move(decision);
  const auto total = after.orders.at(static_cast<std::size_t>(id - 1)).request.quantity;
  // The units still to fill: the whole of a new order.
  auto request = after.orders.at(static_cast<std::size_t>(id - 1)).request;
  request.quantity = after.orders.at(static_cast<std::size_t>(id - 1)).remaining();
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  if (request.quantity <= 0 || legs.size() > kMaxRollLegs) {
    after = before;
    return projection;
  }
  for (const auto& leg : legs) {
    if (leg.ratio <= 0 || leg.ratio > kMaxRatio || request.quantity > std::numeric_limits<Quantity>::max() / (100 * leg.ratio) ||
        !before.contracts.contains(leg.symbol) || !quote_check(before, leg.symbol).ok()) {
      // An unprojectable order cannot reserve an unknown contract in the risk view.
      after = before;
      return projection;
    }
  }
  const auto& candidate = after.orders.at(static_cast<std::size_t>(id - 1));
  if (trade_rules_on(before.config.rules) && !candidate.system && !kept_within(candidate) &&
      !request.exits_only && !closing_only(before, candidate)) {
    const auto risk = trade_risk(before, candidate.request);
    result.trade_risk = risk.loss;
    result.trade_risk_basis = risk.basis;
    result.trade_risk_limit = trade_risk_limit(before, snapshot.equity);
  }
  // The full size at the far sides, block by block, as market_slices prices it.
  Money gross, net;
  for (const auto& leg : legs)
    for (const auto& [price, n] : market_slices(before, leg.symbol, leg.side, magnitude(signed_contracts(leg, request.quantity)))) {
      result.execution.schedule.push_back({leg.symbol, leg.side, n, price});
      gross = gross + price * n;
      net = leg.side == Side::Buy ? net + price * n : net - price * n;
    }
  result.execution.average_price = (multi_leg(request) ? net : gross).prorate(1, request.quantity);
  // Held exits rest as a bracket's exits do, holding only their fees, until one fills:
  // buying power after them is what their acceptance leaves.
  if (request.exits_only && request.bracket)
    after.orders.mut(static_cast<std::size_t>(id - 1)).role = request.bracket->take_profit ? OrderRole::TakeProfit : OrderRole::StopLoss;
  const auto power = buying_power(after, id);
  result.buying_power_required = power.focus_reservation;
  result.buying_power_working = power.total.available;
  if (request.exits_only) result.buying_power_after = power.total.available;
  auto& filled = after.orders.mut(static_cast<std::size_t>(id - 1));
  filled.status = OrderStatus::Filled;
  filled.filled_quantity = total;
  const auto [premium, fees, itemized] = project_fill(after, before, request, request.quantity, filled);
  result.fee = fees;
  if (before.config.rules.fees) result.fees = itemized;
  // A filled bracket entry leaves its exits working, and they reserve their fees.
  if (request.bracket && !request.exits_only) {
    Events ignored;
    attach_exits(after, id, ignored);
  }
  const auto projected = snapshot_of(after);
  if (!request.exits_only) result.buying_power_after = projected.buying_power.available;
  if (snapshot.risk.complete && projected.risk.complete) {
    const auto& a = snapshot.risk.aggregate.position;
    const auto& b = projected.risk.aggregate.position;
    result.exposure_change = Exposure{b.dollar_delta - a.dollar_delta, b.dollar_gamma_1pct - a.dollar_gamma_1pct,
                                      b.vega - a.vega, b.theta - a.theta};
  }
  if (!snapshot.valuation_complete || !projected.valuation_complete) return projection;
  // The order's own expiry payoff describes an order that opens every leg. One
  // that reduces a holding is measured on the account after it: selling held
  // longs is not writing new shorts.
  const bool reduces = std::any_of(legs.begin(), legs.end(), [&](const Leg& leg) {
    const auto q = held(before, leg.symbol);
    return q != 0 && (q > 0) != (leg.side == Side::Buy);
  });
  if (!reduces) result.max_loss = expiry_loss(before, request, premium, fees);
  if (result.max_loss) {
    result.max_loss_basis = "expiry_payoff";
  } else if (projected.scenarios.complete) {
    double worst = 0;
    for (const auto& cell : projected.scenarios.cells) worst = std::min(worst, cell.pnl);
    result.max_loss = std::max(Money{}, snapshot.equity - projected.equity - Money::from_double(worst));
    result.max_loss_basis = "scenario_grid";
  }
  if (result.max_loss) {
    result.equity_at_max_loss = snapshot.equity - *result.max_loss;
    if (before.config.rules.max_drawdown > Money{}) result.breaches_floor = *result.equity_at_max_loss <= before.evaluation.floor;
    if (snapshot.soft_floor) result.breaches_soft_floor = *result.equity_at_max_loss <= *snapshot.soft_floor;
  }
  return projection;
}
namespace {
std::vector<OrderId> superseded_stops(const State& s, const OrderRequest& request);
void supersede(State& s, const OrderRequest& request, const std::vector<OrderId>& stops, Events& events);
}  // namespace
/// A new order: accepted, or not, on a copy of `before`, then projected. A buy-only
/// close previews as submit places it, over the stops it supersedes.
PreviewProjection project_order(const State& before, OrderRequest request, const Decision& rejection) {
  State after = before;
  if (rejection.ok()) {
    Events ignored;
    supersede(after, request, superseded_stops(before, request), ignored);
  }
  Order candidate;
  candidate.id = static_cast<OrderId>(after.orders.size() + 1);
  candidate.request = std::move(request);
  candidate.accepted_at = before.time;
  add_order(after, candidate);
  auto decision = acceptance_check(after, after.orders.back(), rejection);
  return project_working(before, std::move(after), candidate.id, std::move(decision));
}
namespace {
/// Whether a change keeps a resting order's place in the queue: only a smaller
/// size does (or terms it already had); a larger size or any other new term
/// sends it to the back.
bool keeps_queue_place(const OrderRequest& before, const OrderRequest& after) {
  const auto level = [](const OrderRequest& r) { return r.trigger ? std::optional(r.trigger->level) : std::nullopt; };
  return after.quantity <= before.quantity && after.limit_price == before.limit_price && after.tif == before.tif &&
         after.walk == before.walk && level(after) == level(before);
}
}  // namespace
/// Order `id` with `change`'s terms in place, as change_order sets them, unchecked.
void change_terms(State& s, OrderId id, const OrderChange& change) {
  auto& order = s.orders.mut(static_cast<std::size_t>(id - 1));
  const auto before = order.request;
  if (change.quantity) order.request.quantity = *change.quantity;
  if (change.limit_price && order.request.type == OrderType::Limit) {
    order.request.limit_price = *change.limit_price;
    order.limit_ticks.reset();
  }
  if (change.trigger_level && order.request.trigger) order.request.trigger->level = *change.trigger_level;
  if (change.tif) order.request.tif = *change.tif;
  if (change.walk) order.request.walk = *change.walk;
  if (order.request.walk || change.walk) order.walked_at = s.time;
  if (!keeps_queue_place(before, order.request)) send_to_back(s, order, s.time);
}
/// The account a preview starts from: the current one with the integration's newer
/// market for contracts it may not hold yet, at the market time, its daily loss checked.
State prepared(const State& state, Timestamp time, const PreviewMarket& market) {
  State before = state;
  for (const auto& contract : market.contracts) {
    const auto symbol = contract.osi_symbol();
    const auto adjusted = state.deliverables.find(symbol);
    if (!eligible(contract, adjusted == state.deliverables.end() ? nullptr : &adjusted->second).ok()) continue;
    if (const auto saved = before.contracts.find(symbol); saved != before.contracts.end() && Json(saved->second) != Json(contract))
      throw TradingError(Reason::INVALID_CONTRACT, "Preview definition conflicts with registered terms");
    before.contracts[symbol] = contract;
  }
  for (const auto& quote : market.quotes) {
    if (!before.contracts.contains(quote.symbol) || quote.time > time || quote.time < 0) continue;
    // As on_quotes: a quote from before a split's ex-date prices the series as they were.
    if (before_split(before, before.contracts.at(quote.symbol).underlying, quote.first_time())) continue;
    const auto prior = before.books.find(quote.symbol);
    const bool keep_mark = prior != before.books.end() && keeps_mark(before, prior->second.quote, quote, quote.time);
    if (prior != before.books.end() && prior->second.quote.observation == quote.observation) {
      auto& book = before.books[quote.symbol];
      const auto first = book.quote.first_time();
      book.quote = quote;
      if (first < quote.time) book.quote.quoted = first;
    } else {
      before.books[quote.symbol] = {quote, valid_quote(quote) ? quote.bid_size : 0, markable_quote(quote) ? quote.ask_size : 0};
    }
    if (markable_quote(quote) && !keep_mark) before.marks[quote.symbol] = {mark_of(quote), quote.time};
  }
  for (const auto& valuation : market.valuations)
    if (before.contracts.contains(valuation.symbol) && valuation.time <= time && valuation.time >= 0)
      before.valuations[valuation.symbol] = valuation;
  Events ignored;
  advance(before, time, ignored);
  monitor_calendar(before, ignored);
  monitor_loss(before, ignored);
  return before;
}

/// The account in `s` as a what-if shows it, against `base_equity`, today's.
WhatIfAccount what_if_account(const State& s, Money base_equity, const std::map<std::string, double>& close_variances) {
  WhatIfAccount out;
  const auto snapshot = snapshot_of(s);
  out.equity = snapshot.equity;
  out.buying_power = snapshot.buying_power.available;
  if (snapshot.risk.complete) out.exposure = snapshot.risk.aggregate.position;
  out.scenarios = snapshot.scenarios;
  const double cost = (snapshot.equity - base_equity).dollars();
  for (auto& cell : out.scenarios.cells) cell.pnl += cost;
  if (snapshot.valuation_complete && snapshot.scenarios.complete) {
    double worst = 0;
    for (const auto& cell : snapshot.scenarios.cells) worst = std::min(worst, cell.pnl);
    out.max_loss = std::max(Money{}, base_equity - snapshot.equity - Money::from_double(worst));
    out.equity_at_max_loss = base_equity - *out.max_loss;
    if (s.config.rules.max_drawdown > Money{}) out.breaches_floor = *out.equity_at_max_loss <= s.evaluation.floor;
    if (snapshot.soft_floor) out.breaches_soft_floor = *out.equity_at_max_loss <= *snapshot.soft_floor;
  }
  out.breach = breach_of(s, close_variances);
  return out;
}
/// What order `id` did in `trial`, a private copy of `before` that took it or a
/// change to it: its state and the fills it got there, beside `execution`'s
/// full-size schedule. Fills from before the copy do not count.
PreviewExecution executed(const State& before, const State& trial, OrderId id, PreviewExecution execution) {
  const auto& order = trial.orders.at(static_cast<std::size_t>(id - 1));
  const auto* prior = id <= before.orders.size() ? &before.orders.at(static_cast<std::size_t>(id - 1)) : nullptr;
  execution.status = order.status;
  execution.reason = order.reason;
  execution.filled_quantity = order.filled_quantity - (prior ? prior->filled_quantity : 0);
  execution.remaining_quantity = order.status == OrderStatus::Rejected ? 0 : order.remaining();
  if (execution.filled_quantity > 0)
    execution.average_fill_price = (order.filled_notional - (prior ? prior->filled_notional : Money{})).prorate(1, execution.filled_quantity);
  for (auto i = before.fills.size(); i < trial.fills.size(); ++i)
    if (const auto& fill = trial.fills[i]; fill.order_id == id) execution.fills.push_back({fill.symbol, fill.side, fill.quantity, fill.price});
  return execution;
}
/// How many units of an order fit (OrderPreview's max_units and its parts):
/// `sized` projects a number of units, `upper` is the most the order-size limit
/// allows, and `room` the nearer floor's room, of which `floor_share` may go.
Sizing size_order(const std::function<const OrderPreview&(Quantity)>& sized, Quantity upper,
                  std::optional<Money> room, double floor_share) {
  Sizing out;
  const auto none = [&](std::string basis) {
    out.basis = std::move(basis);
    out.units = out.buying_power = 0;
    if (room) out.floor = 0;
    return out;
  };
  if (upper < 1) return none("limits");
  const auto fits_limits = [&](Quantity quantity) {
    const auto& value = sized(quantity);
    return value.decision.ok() && value.buying_power_after && value.max_loss;
  };
  // Convert the requested share once to millionths; floor sizing then compares
  // fixed-point dollars. Touching either floor is never an admissible size.
  const auto share = static_cast<std::int64_t>(std::floor(floor_share * 1'000'000));
  const auto fits_floor = [&](Quantity quantity) {
    const auto& loss = sized(quantity).max_loss;
    return loss && (!room || (room->micros() > 0 && *loss <= room->prorate(share, 1'000'000) && *loss < *room));
  };
  if (!fits_limits(1)) {
    // Buying power and the risk limits refuse a larger order too, so none fits. Any
    // other refusal, or a loss that cannot be projected, leaves sizing unavailable.
    const auto code = sized(1).decision.code;
    if (code == Reason::BUYING_POWER || code == Reason::DELTA_LIMIT || code == Reason::VEGA_LIMIT ||
        code == Reason::MAX_ORDER_CONTRACTS || code == Reason::MAX_CONTRACTS_HELD || code == Reason::MAX_TRADE_RISK ||
        code == Reason::SCALING_LIMIT || code == Reason::GAMMA_LIMIT || code == Reason::THETA_LIMIT ||
        code == Reason::GRID_LOSS_LIMIT || code == Reason::SHORT_VEGA_LIMIT) return none(code == Reason::BUYING_POWER ? "buying_power" : "limits");
    return out;
  }
  Quantity low = 1, high = upper;
  // Pre-trade reservations include both the held position and the candidate,
  // so increasing size cannot repair an already excessive reachable exposure.
  while (low < high) {
    const auto middle = low + (high - low) / 2 + (high - low) % 2;
    if (fits_limits(middle)) low = middle; else high = middle - 1;
  }
  // A cap at the order limit needs no extra projection. Otherwise the first
  // refused size identifies the pre-trade constraint that stopped the search.
  out.basis = low < upper && sized(low + 1).decision.code == Reason::BUYING_POWER ? "buying_power" : "limits";
  upper = low;
  // Scenario losses can first fall as an order hedges the book, then rise.
  // Their maximum of linear per-cell losses is convex: find its minimum before
  // searching the upper feasible edge, rather than assuming one unit fits.
  const auto floor_fit = [&](Quantity from, Quantity to) -> Quantity {
    Quantity a = from, b = to;
    while (a < b) {
      const auto middle = a + (b - a) / 2;
      const auto& left = sized(middle).max_loss;
      const auto& right = sized(middle + 1).max_loss;
      if (left && right && *left > *right) a = middle + 1; else b = middle;
    }
    if (!fits_floor(a)) return 0;
    b = to;
    while (a < b) {
      const auto middle = a + (b - a) / 2 + (b - a) % 2;
      if (fits_floor(middle)) a = middle; else b = middle - 1;
    }
    return a;
  };
  if (room) out.floor = floor_fit(1, upper);
  // Free buying power can improve while a hedge closes shorts, then decline as
  // it starts a long position. Find its feasible interval before sizing to loss.
  low = 1; high = upper;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (*sized(middle).buying_power_after < *sized(middle + 1).buying_power_after) low = middle + 1;
    else high = middle;
  }
  const auto best_power = low;
  if (*sized(best_power).buying_power_after < Money{}) {
    out.units = out.buying_power = 0;
    out.basis = "buying_power";
    return out;
  }
  low = 1; high = best_power;
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    if (*sized(middle).buying_power_after >= Money{}) high = middle; else low = middle + 1;
  }
  const auto lower = low;
  low = best_power; high = upper;
  while (low < high) {
    const auto middle = low + (high - low) / 2 + (high - low) % 2;
    if (*sized(middle).buying_power_after >= Money{}) low = middle; else high = middle - 1;
  }
  out.buying_power = low;
  if (low < upper) out.basis = "buying_power";
  out.units = floor_fit(lower, low);
  if (out.units < out.buying_power) out.basis = "floor";
  return out;
}
namespace {

/// Accept or reject one new order; once accepted, arm it or match it at once.
/// On a buy-only plan, the account's own armed stop sells that keep a manual close
/// from fitting within its held contracts, newest first: the close supersedes them.
/// Nothing when the order fits as it is, or would not fit even without them.
std::vector<OrderId> superseded_stops(const State& s, const OrderRequest& request) {
  Order candidate;
  candidate.id = static_cast<OrderId>(s.orders.size() + 1);
  candidate.request = request;
  if (!s.config.rules.buy_only || request.trigger || request.exits_only || request.bracket ||
      closing_only(s, candidate) || !closing_only(s, candidate, true, false))
    return {};
  std::vector<OrderId> stops;
  const auto ids = open_ids(s);
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
    const auto& other = s.orders[*it - 1];
    if (other.status != OrderStatus::Armed || other.system || other.role != OrderRole::Normal) continue;
    const auto shares = [&](const std::string& symbol, Side side) {
      if (other.request.symbol == symbol && other.request.side == side) return true;
      return std::any_of(other.request.legs.begin(), other.request.legs.end(),
                         [&](const Leg& leg) { return leg.symbol == symbol && leg.side == side; });
    };
    const bool overlaps = multi_leg(request)
        ? std::any_of(request.legs.begin(), request.legs.end(), [&](const Leg& leg) { return shares(leg.symbol, leg.side); })
        : shares(request.symbol, request.side);
    if (overlaps) stops.push_back(other.id);
  }
  return stops;
}
/// Cancels `stops` (superseded_stops), newest first, until the close fits.
void supersede(State& s, const OrderRequest& request, const std::vector<OrderId>& stops, Events& events) {
  Order candidate;
  candidate.id = static_cast<OrderId>(s.orders.size() + 1);
  candidate.request = request;
  for (const auto id : stops) {
    cancel_order(s, id, failure(Reason::POSITION_CLOSED, "Manual close order " + std::to_string(candidate.id) +
                                " superseded this stop on a buy-only plan"), events);
    if (closing_only(s, candidate)) break;
  }
}
CommandResult place_order(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events, OrderId paired = 0);
/// Places one order. On a buy-only plan a manual close that only its own armed stops
/// keep from fitting cancels them (POSITION_CLOSED), newest first, as far as it needs
/// to; if the close is refused anyway, the stops stay and it records that refusal.
/// An accepted opening order joins the size-creep window, flagged as tilt first when it
/// exceeds the rolling median (the guardrail warns; with a block it never gets here).
void record_opening_size(State& s, const Order& o, Events& events) {
  const auto& g = s.config.guardrails;
  if (g.size_creep_percent == 0 || o.system || kept_within(o) || o.request.exits_only || closing_only(s, o)) return;
  if (const auto creep = size_creep(s, o)) flag_tilt(s, Reason::SIZE_CREEP, size_creep_message(*creep, g.size_creep_percent), events);
  auto& sizes = s.guardrails.opening_sizes;
  sizes.push_back(opening_size(o.request));
  const auto window = static_cast<std::size_t>(g.size_creep_window == 0 ? 20 : g.size_creep_window);
  if (sizes.size() > window) sizes.erase(sizes.begin(), sizes.end() - static_cast<std::ptrdiff_t>(window));
}
CommandResult place_one(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events, OrderId paired = 0) {
  const auto stops = rejection.ok() ? superseded_stops(s, request) : std::vector<OrderId>{};
  if (stops.empty()) return place_order(s, std::move(request), time, rejection, events, paired);
  State trial = s;
  Events trial_events = events;
  supersede(trial, request, stops, trial_events);
  auto result = place_order(trial, request, time, rejection, trial_events, paired);
  if (result.decision.ok()) {
    s = std::move(trial);
    events = std::move(trial_events);
    return result;
  }
  return place_order(s, std::move(request), time, result.decision, events, paired);
}
CommandResult place_order(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events, OrderId paired) {
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = std::move(request);
  order.actor = s.actor;
  order.accepted_at = time;
  if (s.config.rules.min_hold_seconds > 0) order.oco = paired;
  add_order(s, order);
  // Written until matching starts; nothing copies the orders before that.
  auto& stored = s.orders.mut_back();
  auto decision = acceptance_check(s, stored, rejection);
  if (!decision.ok()) {
    stored.status = OrderStatus::Rejected;
    stored.reason = decision;
    stored.ended_at = time;
    event(events, "order_rejected", stored);
    if (decision.code == Reason::SIZE_CREEP) flag_tilt(s, Reason::SIZE_CREEP, decision.message, events);
    return CommandResult{decision, stored.id, 0};
  }
  record_opening_size(s, stored, events);
  const auto id = stored.id;
  const auto symbols = order_symbols(stored.request);
  if (stored.request.exits_only) {
    const auto bracket = *stored.request.bracket;
    stored.role = bracket.take_profit ? OrderRole::TakeProfit : OrderRole::StopLoss;
    stored.day_end = extended(stored.request) || stored.request.tif == TimeInForce::Gtd
        ? order_end(s, stored.request, time) : order_expiry(s, stored.request);
    stored.status = stored.request.trigger ? OrderStatus::Armed : OrderStatus::Working;
    stored.take_profit = bracket.take_profit ? id : 0;
    stored.stop_loss = bracket.take_profit ? 0 : id;
    if (bracket.take_profit && bracket.stop_loss) {
      Order stop = stored;
      stop.id = static_cast<OrderId>(s.orders.size() + 1);
      stop.request.client_order_id += ":stop";
      stop.request.bracket.reset();
      stop.request.exits_only = false;
      stop.request.limit_price = bracket.stop_loss->limit_price;
      stop.request.trigger = bracket.stop_loss->trigger;
      stop.request.type = stop.request.limit_price ? OrderType::Limit : OrderType::Market;
      stop.request.tif = extended(stored.request) || stored.request.tif == TimeInForce::Gtd
          ? stored.request.tif : stop.request.limit_price ? TimeInForce::Gtc : TimeInForce::Ioc;
      stop.status = stop.request.trigger ? OrderStatus::Armed : OrderStatus::Working;
      stop.role = OrderRole::StopLoss;
      stop.oco = id;
      stop.stop_loss = 0;
      stop.take_profit = 0;
      stored.stop_loss = stop.id;
      stored.oco = stop.id;
      event(events, "order_accepted", stored);
      add_order(s, stop);
      event(events, "order_accepted", stop);
    } else event(events, "order_accepted", stored);
    match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
    check_triggers(s, events);
    return CommandResult{{}, id, 0};
  }
  if (stored.request.trigger) {
    // Armed until reached, and good until expiry; a level already reached
    // activates at once, and so does a reached stop of the exits its fills create.
    stored.status = OrderStatus::Armed;
    stored.day_end = extended(stored.request) || stored.request.tif == TimeInForce::Gtd
        ? order_end(s, stored.request, time) : order_expiry(s, stored.request);
    event(events, "order_accepted", stored);
    check_triggers(s, events);
    return CommandResult{{}, id, 0};
  }
  stored.day_end = order_end(s, stored.request, time);
  event(events, "order_accepted", stored);
  // Existing better orders share any remaining budget even on command ingress.
  // Matching can append bracket exits, so re-read the order by ID afterwards.
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  const auto& accepted = s.orders.at(static_cast<std::size_t>(id - 1));
  if (s.config.rules.fill_latency_ms == 0 && accepted.open() && accepted.request.tif == TimeInForce::Ioc)
    cancel_order(s, id, failure(Reason::IOC_REMAINDER, ioc_remainder_message(s, accepted, "IOC exhausted available displayed liquidity")), events);
  // A bracket stop the entry's fills created may already be reached.
  check_triggers(s, events);
  return CommandResult{{}, id, 0};
}
}  // namespace
/// Accept an order, and the order that cancels it with it: both or neither. If
/// the other is refused, the order is recorded refused for that reason and the
/// other not at all. Once both are accepted they are linked; one that filled on
/// acceptance cancels the other at once.
CommandResult place(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events) {
  if (request.oco.empty()) return place_one(s, std::move(request), time, rejection, events);
  const State before = s;
  const auto recorded = events.size();
  auto other = request.oco.front();
  other.client_order_id = request.client_order_id + ":oco";
  const auto first = place_one(s, request, time, rejection, events);
  if (!first.decision.ok()) return first;
  const auto a = *first.order_id;
  // A first order that filled on acceptance leaves the other nothing to trade: it is
  // checked for acceptance on a copy, then recorded and cancelled before it can match.
  const bool first_filled = s.orders.at(static_cast<std::size_t>(a - 1)).filled_quantity > 0;
  std::optional<State> trial;
  if (first_filled) trial.emplace(s);
  Events trial_events;
  const auto second = place_one(first_filled ? *trial : s, other, time, {}, first_filled ? trial_events : events, a);
  if (!second.decision.ok()) {
    s = before;
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(recorded), events.end());
    auto refused = second.decision;
    refused.message = "The order that cancels it: " + refused.message;
    return place_one(s, std::move(request), time, refused, events);
  }
  if (first_filled) {
    Order order;
    order.id = static_cast<OrderId>(s.orders.size() + 1);
    order.request = std::move(other);
    order.actor = s.actor;
    order.accepted_at = time;
    order.status = order.request.trigger ? OrderStatus::Armed : OrderStatus::Working;
    order.day_end = order_end(s, order.request, time);
    add_order(s, order);
    event(events, "order_accepted", s.orders.back());
  }
  const auto b = first_filled ? static_cast<OrderId>(s.orders.size()) : *second.order_id;
  s.orders.mut(a - 1).oco = b;
  s.orders.mut(b - 1).oco = a;
  for (const auto& [filled, open] : {std::pair{a, b}, std::pair{b, a}})
    if (s.orders.at(static_cast<std::size_t>(filled - 1)).filled_quantity > 0)
      cancel_order(s, open, failure(Reason::OCO_FILLED, "The other order of this pair filled"), events);
  return first;
}
/// One-triggers-other: each order that filled completely places the order
/// chained to it, once, as its own submission by the same actor. It runs after
/// each command over the orders open when it began and those placed since, so a
/// chained order that fills at once places its own in turn.
void place_chained(State& s, Events& events) {
  const auto actor = s.actor;
  const auto visit = [&](OrderId id) {
    const auto& o = s.orders[id - 1];
    if (o.status != OrderStatus::Filled || o.request.then.empty() || o.chained != 0) return;
    auto next = o.request.then.front();
    next.client_order_id = o.request.client_order_id + ":then";
    s.actor = o.actor;
    const auto placed = place(s, std::move(next), s.time, {}, events);
    s.actor = actor;
    if (!placed.order_id) return;
    s.orders.mut(id - 1).chained = *placed.order_id;
    s.orders.mut(*placed.order_id - 1).chained_from = id;
  };
  // place() can replace the whole State (a superseded stop's trial, a refused pair),
  // so iterate a copy of the orders that were open.
  const auto working = s.working;
  for (const auto id : working) visit(id);
  for (auto i = s.indexed; i < s.orders.size(); ++i) visit(static_cast<OrderId>(i + 1));
}
/// Apply new terms to a resting order, or leave it untouched with the reason.
/// Each change asked of an open order stays on it, applied or refused, with the
/// terms it had then.
CommandResult change_order(State& s, OrderId id, const OrderChange& change, const Decision& rejection, Events& events) {
  if (id == 0 || id > s.orders.size()) return {failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
  if (!s.orders.at(static_cast<std::size_t>(id - 1)).open()) return {failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
  // Written until matching or activation; nothing copies the orders before that.
  auto& order = s.orders.mut(static_cast<std::size_t>(id - 1));
  const auto& r = order.request;
  OrderChangeRecord record{s.time, s.actor, change.quantity, change.limit_price, change.trigger_level, r.quantity, r.limit_price,
                           r.trigger ? std::optional(r.trigger->level) : std::nullopt, {}};
  if (change.walk) {
    record.walk = change.walk;
    record.previous_walk = r.walk;
  }
  if (change.tif) {
    record.time_in_force = change.tif;
    record.previous_time_in_force = r.tif;
  }
  const auto refuse = [&](Decision decision) {
    record.decision = decision;
    keep_change(s, order, record);
    return CommandResult{std::move(decision), id, 0};
  };
  const bool exit = kept_within(order);
  const bool resting = order.status == OrderStatus::Armed || (r.type == OrderType::Limit && r.tif != TimeInForce::Ioc);
  if (order.system || !resting)
    return refuse(failure(Reason::INVALID_ORDER, "Only resting orders change: DAY/GTC limit orders, armed orders and bracket exits"));
  if (change.empty()) return refuse(failure(Reason::INVALID_ORDER, "Give a new quantity, limit price, trigger level or time in force"));
  if (change.walk && *change.walk && exit)
    return refuse(failure(Reason::INVALID_ORDER, "Managed exits cannot walk"));
  if (change.quantity && order.reduce_only)
    return refuse(failure(Reason::INVALID_ORDER, "A reduce-only close's size follows its position; change its price instead"));
  if (s.config.rules.require_stop_loss && order.role == OrderRole::StopLoss && change.quantity &&
      *change.quantity < r.quantity && exit_capacity(s, r) > 0)
    return refuse(failure(Reason::STOP_REQUIRED, "Close the protected position before reducing its required stop-loss size"));
  // An exit may close part of what it protects, or all of it again, never more.
  if (change.quantity && exit && *change.quantity > order.filled_quantity + exit_capacity(s, r))
    return refuse({Reason::INVALID_ORDER, "A bracket exit closes at most the position it protects",
                   static_cast<double>(*change.quantity), static_cast<double>(order.filled_quantity + exit_capacity(s, r)), {}});
  if (change.tif && (exit || r.type != OrderType::Limit || (r.tif != TimeInForce::Day && r.tif != TimeInForce::Gtc) ||
                     (*change.tif != TimeInForce::Day && *change.tif != TimeInForce::Gtc)))
    return refuse(failure(Reason::INVALID_ORDER, "Only resting limit orders change between DAY and GTC; bracket exits are good until expiry"));
  if (change.quantity && *change.quantity <= order.filled_quantity)
    return refuse({Reason::INVALID_ORDER, "The new quantity must exceed the filled quantity; cancel the order instead",
                   static_cast<double>(*change.quantity), static_cast<double>(order.filled_quantity), {}});
  if (change.limit_price && r.type != OrderType::Limit)
    return refuse(failure(Reason::INVALID_ORDER, "Only limit orders have a limit price"));
  if (change.trigger_level && (!r.trigger || order.status != OrderStatus::Armed))
    return refuse(failure(Reason::INVALID_ORDER, "Only armed orders with a trigger have a trigger level"));
  if (s.evaluation.revised_rules && trade_rules_on(s.config.rules) && order.role == OrderRole::StopLoss &&
      change.trigger_level && r.trigger && exit_capacity(s, r) > 0) {
    const bool away = r.trigger->direction == TriggerDirection::AtOrBelow
        ? *change.trigger_level < r.trigger->level : *change.trigger_level > r.trigger->level;
    if (away) return refuse(failure(Reason::STOP_REQUIRED, "Close the protected position before loosening its required stop-loss level"));
  }
  if (change.trigger_level && r.trigger->source == TriggerSource::Time)
    return refuse(failure(Reason::INVALID_ORDER, "A time trigger has no level; cancel the order and place it for the new time"));
  if (!rejection.ok()) return refuse(rejection);
  const Order before = order;
  if (!order.submitted) order.submitted = order.request;
  if (change.quantity) order.request.quantity = *change.quantity;
  if (change.limit_price) order.request.limit_price = *change.limit_price;
  if (change.trigger_level) order.request.trigger->level = *change.trigger_level;
  if (change.tif) order.request.tif = *change.tif;
  if (change.walk) order.request.walk = *change.walk;
  if (order.request.walk || change.walk) order.walked_at = s.time;
  Decision decision;
  if (exit) {
    // Exits only ever reduce a position, so they skip the entry checks; their terms must still be valid.
    const auto& root = s.contracts.at(order_symbols(order.request).front()).root;
    const auto& limit = order.request.limit_price;
    if ((order.request.trigger && order.request.trigger->source != TriggerSource::Combo && order.request.trigger->level <= Money{}) ||
        (!multi_leg(order.request) && limit && *limit <= Money{}))
      decision = failure(Reason::INVALID_ORDER, "Trigger levels and exit prices must be positive");
    else if (limit && limit->micros() % tick_size(root, multi_leg(order.request) ? Money{} : *limit).micros() != 0)
      decision = failure(Reason::INVALID_TICK, "An exit's limit price is not a positive multiple of the product tier tick");
  } else {
    decision = order_check(s, order);
    if (decision.ok()) decision = open_orders_risk_check(s, order);
  }
  if (!decision.ok()) {
    order = before;
    return refuse(decision);
  }
  // A new time in force sets when a working order ends; an armed one keeps expiry.
  if (change.tif && order.status != OrderStatus::Armed)
    order.day_end = order.request.tif == TimeInForce::Gtc ? order_expiry(s, order.request) : day_deadline(s, order.request, s.time);
  if (change.limit_price) order.limit_ticks.reset();
  if (!keeps_queue_place(before.request, order.request)) send_to_back(s, order, s.time);
  keep_change(s, order, record);
  event(events, "order_modified", order);
  const auto symbols = order_symbols(order.request);
  if (order.status != OrderStatus::Armed) match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  // An armed order whose new level is reached activates, as does a reached stop
  // of the exits a fill just created.
  check_triggers(s, events);
  return {{}, id, 0};
}
/// Trims a note and its tags, and lowercases the tags: text without control
/// characters (a note may keep newlines and tabs, with a CRLF or lone CR line
/// break kept as a newline), a note of at most 2,000 bytes, at most eight distinct
/// tags of 1 to 32 bytes without commas.
Annotation clean_annotation(std::string note, const std::vector<std::string>& tags) {
  const auto invalid = [](std::string message) { throw TradingError(Reason::INVALID_NOTE, std::move(message)); };
  const auto newlines = [](std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '\r') out.push_back(text[i]);
      else if (i + 1 == text.size() || text[i + 1] != '\n') out.push_back('\n');
    }
    return out;
  };
  const auto trim = [](std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string{};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
  };
  const auto text = [](std::string_view value, bool lines) {
    return std::none_of(value.begin(), value.end(), [&](unsigned char c) {
      return (c < 0x20 && !(lines && (c == '\n' || c == '\t'))) || c == 0x7f;
    });
  };
  Annotation a;
  a.note = trim(newlines(std::move(note)));
  if (a.note.size() > 2000) invalid("A note is at most 2,000 bytes of text");
  if (!text(a.note, true)) invalid("A note is text: no control characters other than newlines and tabs");
  for (const auto& tag : tags) {
    auto clean = trim(tag);
    for (auto& c : clean) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (clean.empty() || clean.size() > 32 || clean.find(',') != std::string::npos || !text(clean, false))
      invalid("A tag is 1 to 32 bytes of text without commas");
    if (std::find(a.tags.begin(), a.tags.end(), clean) == a.tags.end()) a.tags.push_back(std::move(clean));
  }
  if (a.tags.size() > 8) invalid("A trade takes at most eight tags");
  // The journal records UTF-8 only; reject anything else before it could fail a commit.
  try { (void)Json{{"note", a.note}, {"tags", a.tags}}.dump(); }
  catch (const Json::exception&) { invalid("Notes and tags must be UTF-8 text"); }
  return a;
}
}  // namespace session_internal
}  // namespace openport::trading
