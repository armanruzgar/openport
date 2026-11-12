// What a session watches as time and prices move: guardrails and the kill switch,
// walking limits, the clock, flattening, the plan's calendar and open-loss cap, and
// the plan's rules.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
namespace {
bool personal_reason(std::string_view reason) {
  return reason == "SOFT_FLOOR" || reason == "TRADE_LIMIT" || reason == "COOLDOWN" || reason == "PROFIT_LOCK";
}
}  // namespace
Reason guardrail_reason(const State& s) {
  if (!s.guardrails.latched.empty()) return s.guardrails.latched.front();
  return s.time < s.guardrails.cooldown_until ? Reason::COOLDOWN : Reason::NONE;
}
namespace {
/// A latched personal guardrail's reason: what it measured against its setting,
/// and when opening orders may resume.
Decision guardrail_decision(const State& s, Reason reason) {
  const auto& g = s.config.guardrails;
  switch (reason) {
    case Reason::COOLDOWN:
      return failure(reason, "Cooldown after a stop-loss exit or loss until " + clock_text(s.guardrails.cooldown_until));
    case Reason::TRADE_LIMIT:
      if (g.max_opening_trades == 0 || s.guardrails.opening_trades < g.max_opening_trades)
        return {reason, "Today's opening-trade limit remains latched after the account reset; opening orders resume next trading day",
                static_cast<double>(s.guardrails.opening_trades), {}, "aggregate"};
      return {reason, "The day's opening trades reached the limit of " + std::to_string(g.max_opening_trades) +
              "; opening orders resume next trading day",
              static_cast<double>(s.guardrails.opening_trades), static_cast<double>(g.max_opening_trades), "aggregate"};
    case Reason::PROFIT_LOCK:
      return {reason, "Today's profit lock is latched (current setting " + dollars(g.profit_lock) + "); opening orders resume next trading day",
              {}, g.profit_lock.dollars(), "aggregate"};
    case Reason::SOFT_FLOOR: {
      const auto level = soft_floor_of(s);
      return {reason, "Equity reached the soft floor" + (level ? " of " + dollars(*level) : std::string()) +
              "; rollover or an account reset re-checks it against equity", {},
              level ? std::optional(level->dollars()) : std::nullopt, "aggregate"};
    }
    default: return failure(reason, std::string(to_string(reason)));
  }
}
}  // namespace
/// Why the kill latch stops an opening order: an active personal guardrail's own code,
/// otherwise the switch. Orders it refuses and orders it cancels give the same reason.
Decision kill_decision(const State& s) {
  const auto personal = guardrail_reason(s);
  if (personal != Reason::NONE) return guardrail_decision(s, personal);
  if (s.kill_reason == "DAILY_LOSS")
    return {Reason::KILL_SWITCH, "The daily loss latch is set: the marked loss exceeded the limit of " +
            dollars(s.config.limits.max_daily_loss), {}, s.config.limits.max_daily_loss.dollars(), "aggregate"};
  return failure(Reason::KILL_SWITCH, "The kill switch is latched: " + s.kill_reason);
}
/// Keeps one change of the kill latch in its history.
void record_kill(State& s, std::string action, std::string reason, std::string previous) {
  s.kill_history.push_back({s.time, std::move(action), std::move(reason), std::move(previous), s.actor});
}
/// Only the order that reached the trade limit may finish, and no other latch yields.
bool completing_trade(const State& s, OrderId id) {
  return id != 0 && id == s.guardrails.trade_limit_order && s.guardrails.owns_kill &&
         s.kill_reason == "TRADE_LIMIT" && s.guardrails.latched.size() == 1 && s.guardrails.latched.front() == Reason::TRADE_LIMIT &&
         s.time >= s.guardrails.cooldown_until;
}
void trip(State& s, const std::string& reason, Events& events) {
  if (!s.kill || (s.guardrails.owns_kill && s.kill_reason != reason)) {
    // A rule that replaces a personal guardrail's reason names the one it replaced.
    const auto previous = s.kill ? s.kill_reason : std::string();
    s.kill = true;
    s.kill_reason = reason;
    s.guardrails.owns_kill = personal_reason(reason);
    Json payload{{"reason", reason}};
    if (!previous.empty()) payload["previous"] = previous;
    event(events, "kill_trip", payload);
    record_kill(s, "trip", reason, previous);
  }
  // Remove opening orders first, so they cannot take a close's capacity; then any
  // closes that together exceed the position, newest first, so older ones keep priority.
  const auto cancel_unless_closing = [&](OrderId id, bool include_working) {
    const auto& o = s.orders[id - 1];
    if (o.open() && !o.system && !kept_within(o) && !completing_trade(s, id) && !closing_only(s, o, include_working))
      cancel_order(s, id, kill_decision(s), events);
  };
  const auto ids = open_ids(s);
  for (const auto id : ids) cancel_unless_closing(id, false);
  for (auto it = ids.rbegin(); it != ids.rend(); ++it) cancel_unless_closing(*it, true);
  // Working share orders that could open go too (driver 11; replay drivers 1-10 left
  // them working until an execution check rejected them).
  if (s.revised_lifecycle) cancel_opening_stock_orders(s, kill_decision(s), events);
}
Decision loss_check(const State& s, const Measures& snapshot) {
  if (snapshot.risk.daily_loss > s.config.limits.max_daily_loss)
    return {Reason::DAILY_LOSS, "Marked loss from start-of-day equity exceeds limit",
        snapshot.risk.daily_loss.dollars(), s.config.limits.max_daily_loss.dollars(), "aggregate"};
  return {};
}
/// Why reset_kill cannot clear the latch now, and when it can: a personal
/// guardrail until its expiry, a daily loss until it is back within the limit or
/// the next trading day's baseline; NONE when a reset would clear it.
Decision reset_check(const State& s, const Measures& m) {
  if (!s.kill) return {};
  if (const auto personal = guardrail_reason(s); personal != Reason::NONE) {
    if (personal == Reason::COOLDOWN)
      return failure(personal, "The kill-switch reset waits for the cooldown to end at " + clock_text(s.guardrails.cooldown_until) +
                     "; an account reset also preserves this cooldown");
    if (personal == Reason::SOFT_FLOOR)
      return failure(personal, "The kill-switch reset cannot clear SOFT_FLOOR; it clears at the next trading day's rollover or an account reset, "
                     "which applies pending settings and re-checks the floor against new equity");
    return failure(personal, std::string(to_string(personal)) +
                   " is a personal daily latch; it clears at the next trading day's rollover; neither a kill-switch reset nor an account reset clears it");
  }
  if (const auto loss = loss_check(s, m); !loss.ok())
    return {Reason::DAILY_LOSS, "A reset trips again while the marked loss " + dollars(m.risk.daily_loss) + " exceeds the limit of " +
            dollars(s.config.limits.max_daily_loss) + "; it clears once the loss is back within the limit, or from the next trading day's baseline",
            loss.actual, loss.limit, loss.scope};
  return {};
}
void monitor_loss(State& s, Events& events) {
  if (!loss_check(s, measure(s)).ok()) trip(s, "DAILY_LOSS", events);
}
void latch_guardrail(State& s, Reason reason, Events& events) {
  auto& latched = s.guardrails.latched;
  if (reason != Reason::COOLDOWN && std::find(latched.begin(), latched.end(), reason) == latched.end()) {
    latched.push_back(reason);
    event(events, "guardrail_latched", Json{{"reason", reason}});
  }
  trip(s, std::string(to_string(guardrail_reason(s))), events);
}
void refresh_guardrail_latch(State& s, Events& events) {
  const auto reason = guardrail_reason(s);
  if (reason != Reason::NONE) trip(s, std::string(to_string(reason)), events);
  else if (s.kill && s.guardrails.owns_kill) {
    record_kill(s, "release", {}, s.kill_reason);
    s.kill = false;
    s.kill_reason.clear();
    s.guardrails.owns_kill = false;
    event(events, "guardrail_released", Json::object());
  }
}
void begin_cooldown(State& s, Events& events) {
  if (s.config.guardrails.cooldown_minutes == 0) return;
  const auto duration = s.config.guardrails.cooldown_minutes * md::kNanosPerMinute;
  if (s.time > std::numeric_limits<Timestamp>::max() - duration)
    throw TradingError(Reason::INVALID_TIME, "Cooldown time exceeds timestamp range");
  s.guardrails.cooldown_until = std::max(s.guardrails.cooldown_until, s.time + duration);
  event(events, "guardrail_latched", Json{{"reason", Reason::COOLDOWN}, {"until", s.guardrails.cooldown_until}});
  latch_guardrail(s, Reason::COOLDOWN, events);
}
/// A discipline guardrail's flag: kept for the day (the latest 50) and journaled.
void flag_tilt(State& s, Reason code, std::string message, Events& events) {
  auto& tilt = s.guardrails.tilt;
  tilt.push_back({s.time, code, std::move(message)});
  if (tilt.size() > 50) tilt.erase(tilt.begin());
  event(events, "tilt_flag", tilt.back());
}
/// A close's realised P&L for the losing-streak guardrail: losses in a row reaching
/// loss_streak start the cooldown and flag tilt; a winning close ends the streak.
void count_close(State& s, Money realised, Events& events) {
  const auto streak = s.config.guardrails.loss_streak;
  if (streak == 0 || realised == Money{}) return;
  if (realised > Money{}) { s.guardrails.losing_closes = 0; return; }
  if (++s.guardrails.losing_closes < streak) return;
  flag_tilt(s, Reason::LOSS_STREAK, std::to_string(s.guardrails.losing_closes) + " losing closes in a row; opening orders "
            "wait out the " + std::to_string(s.config.guardrails.cooldown_minutes) + "-minute cooldown", events);
  s.guardrails.losing_closes = 0;
  begin_cooldown(s, events);
}
/// A triggered close is a stop only when the watched move hurts the held position.
bool closing_stop(const State& s, const Order& order) {
  if (order.role == OrderRole::StopLoss) return true;
  if (order.role == OrderRole::TakeProfit || order.triggered_at <= 0 || !order.request.trigger) return false;
  const auto& trigger = *order.request.trigger;
  if (trigger.trail) return true;
  const bool below = trigger.direction == TriggerDirection::AtOrBelow;
  // A combo watches its signed closing debit: a rise hurts the held position.
  if (trigger.source == TriggerSource::Combo) return !below;
  if (trigger.source == TriggerSource::Option)
    return below == (order.request.side == Side::Sell);
  if (trigger.source != TriggerSource::Underlying) return false;
  if (!multi_leg(order.request)) {
    const bool call = s.contracts.at(order.request.symbol).type == pricing::OptionType::Call;
    return below == ((order.request.side == Side::Sell) == call);
  }
  // The held combo has the opposite delta to its closing legs.
  double closing_delta = 0;
  for (const auto& leg : order.request.legs) {
    const auto* v = valuation_of(s, leg.symbol);
    if (!v || v->time > s.time || s.time - v->time > s.config.limits.max_valuation_age) return false;
    closing_delta += v->delta * static_cast<double>(leg.ratio) * (leg.side == Side::Buy ? 1 : -1);
  }
  return below ? closing_delta < 0 : closing_delta > 0;
}
void guardrail_fill(State& s, OrderId id, bool opening, Money realised_before, Events& events) {
  auto& order = s.orders.mut(static_cast<std::size_t>(id - 1));
  const auto& g = s.config.guardrails;
  if (opening && !order.opening_counted) {
    ++s.guardrails.opening_trades;
    // Only partial openings need extra journal state; whole executions keep their encoding.
    order.opening_counted = order.open();
    if (g.max_opening_trades > 0 && s.guardrails.opening_trades == g.max_opening_trades && order.open())
      s.guardrails.trade_limit_order = id;
  }
  if (s.guardrails.trade_limit_order == id && !order.open()) s.guardrails.trade_limit_order = 0;
  const bool stopped = !opening && !order.system && closing_stop(s, order);
  if (g.max_opening_trades > 0 && s.guardrails.opening_trades >= g.max_opening_trades)
    latch_guardrail(s, Reason::TRADE_LIMIT, events);
  if (stopped ||
      (g.cooldown_loss > Money{} && s.ledger.account().realised - realised_before < -g.cooldown_loss))
    begin_cooldown(s, events);
  if (!opening) count_close(s, s.ledger.account().realised - realised_before, events);
}
/// The queue fill model sends a changed order to the back at its price, as an
/// exchange does: it joins again behind what is displayed then (see join_queue).
void send_to_back(const State& s, Order& o, Timestamp time) {
  if (!s.config.rules.queue_fills) return;
  o.queue.reset();
  o.rejoined_at = time;
}
/// Keeps a change asked of an order: the latest kMaxOrderChanges, counting those it
/// drops (driver 11; replay drivers 1-10 kept every one).
void keep_change(const State& s, Order& o, OrderChangeRecord change) {
  o.changes.push_back(std::move(change));
  if (!s.revised_lifecycle || o.changes.size() <= kMaxOrderChanges) return;
  const auto dropped = o.changes.size() - kMaxOrderChanges;
  o.changes.erase(o.changes.begin(), o.changes.begin() + static_cast<std::ptrdiff_t>(dropped));
  o.changes_dropped += dropped;
}
namespace {
/// Catch up every elapsed interval, retaining each scheduled step in history.
/// Execution uses the current observation, never an invented intermediate quote.
void walk_limits(State& s, Events& events) {
  for (const auto id : open_ids(s)) {
    for (;;) {
      const auto next = next_walk(s.orders[id - 1]);
      if (!next || next->time > s.time) break;
      auto& o = s.orders.mut(id - 1);
      if (!o.submitted) o.submitted = o.request;
      OrderChangeRecord change;
      change.time = next->time;
      change.actor = "walk";
      change.previous_quantity = o.request.quantity;
      change.previous_limit_price = o.request.limit_price;
      change.limit_price = next->price;
      o.request.limit_price = next->price;
      o.walked_at = next->time;
      send_to_back(s, o, next->time);
      keep_change(s, o, change);
      event(events, "order_walked", Json{{"order_id", id}, {"change", change}});
      for (const auto& symbol : order_symbols(o.request)) s.walked.insert(symbol);
    }
  }
}
void decide(State& s, const PlanVerdict& verdict, Money equity, Events& events);
}  // namespace
Decision entry_check(const State& s, const Order& o) {
  if (s.config.rules.underlyings.empty() && !s.config.rules.trading_start && !s.config.rules.flat_time && s.config.rules.events.empty() &&
      s.config.rules.hold_restrictions.empty()) return {};
  if (o.system || kept_within(o) || closing_only(s, o)) return {};
  for (const auto& symbol : order_symbols(o.request))
    if (const auto c = s.contracts.find(symbol); c != s.contracts.end())
      if (const auto d = plan_entry_check(s.config.rules, c->second.underlying, s.time); !d.ok()) return d;
  return {};
}
namespace {
/// Settlement-pending options are no longer tradable; shares always count.
bool holds_scope(const State& s, const std::string& scope, Timestamp time) {
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& contract = s.contracts.at(symbol);
    if (position.quantity != 0 && (scope.empty() || contract.underlying == scope) && time < contract.expiry_time()) return true;
  }
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (stock.shares != 0 && (scope.empty() || symbol == scope)) return true;
  return false;
}
void record_holding_boundaries(State& s, Events& events);
}  // namespace
/// Broader no-overnight wins if both position rules fail at the same rollover.
void check_rollover_positions(State& s, Events& events, bool date_changed) {
  if (s.evaluation.status != EvaluationStatus::Active) return;
  if (date_changed && s.config.rules.no_overnight && holds_scope(s, {}, s.time))
    decide(s, {EvaluationStatus::Failed, false, Reason::OVERNIGHT_HOLD, {},
        "Positions held at the plan day rollover; this plan does not allow overnight holds"}, measure(s).equity, events);
  else if (!s.evaluation.holding_violations.empty())
    decide(s, {EvaluationStatus::Failed, false, Reason::HOLD_RESTRICTED, {},
        "Position held across a restricted boundary: " + s.evaluation.holding_violations.front()}, measure(s).equity, events);
}
namespace {
/// The date latch is separate from the close work, which may need later quotes.
void start_flat_time(State& s, Events& events) {
  auto& e = s.evaluation;
  const auto& rules = s.config.rules;
  if (e.status != EvaluationStatus::Active || !plan_flat_now(rules, s.time)) return;
  const auto day = plan_trading_date(rules, s.time);
  if (e.flat_time_day != day) {
    e.flat_time_day = day;
    e.flat_pending = holds_scope(s, {}, s.time);
    event(events, "flat_time", Json{{"day", day}, {"minute", *rules.flat_time}});
  }
  // Reductions keep working, but cannot become openings after mandatory closes.
  // Run on later transactions too, for newly submitted reducing orders.
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (o.system || kept_within(o)) continue;
    if (closing_only(s, o, false)) {
      s.orders.mut(id - 1).reduce_only = true;
      event(events, "order_reduce_only", s.orders[id - 1]);
    } else {
      cancel_order(s, id, plan_entry_check(rules, {}, s.time), events);
    }
  }
}
}  // namespace
void advance(State& s, Timestamp time, Events& events) {
  if (time < 0 || time < s.time) throw TradingError(Reason::INVALID_TIME, "Market time must be nonnegative and monotone");
  const auto previous_time = s.time;
  s.time = time;
  const auto& rules = s.config.rules;
  record_holding_boundaries(s, events);
  if (previous_time > 0 && plan_trading_date(rules, time) > plan_trading_date(rules, previous_time))
    check_rollover_positions(s, events, true);
  start_flat_time(s, events);
  if (const auto verdict = evaluate_time_rules(s.evaluation, s.config.rules, time); verdict.decided())
    decide(s, verdict, measure(s).equity, events);
  refresh_guardrail_latch(s, events);
  // Every order ends at its earliest leg's last trade; before that, the account's
  // pre-expiry cutoff ends all but its own closing orders, whatever their time in force.
  const auto cutoff = s.config.rules.expiry_cutoff;
  // A gap straight into the next window still crossed the previous window's end.
  const bool skipped_end = s.config.rules.trading_end &&
      md::new_york_time(time).date > md::new_york_time(previous_time).date;
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    auto last = std::numeric_limits<Timestamp>::max();
    for (const auto& symbol : order_symbols(o.request)) last = std::min(last, s.contracts.at(symbol).last_trade_time());
    if (skipped_end && !o.system && !kept_within(o) && !closing_only(s, o))
      cancel_order(s, id, {Reason::OUTSIDE_PLAN_HOURS, "Working opening order reached the end of the plan's trading window",
          static_cast<double>(md::new_york_time(time).seconds / 60), static_cast<double>(*s.config.rules.trading_end),
          s.contracts.at(order_symbols(o.request).front()).underlying}, events);
    else if (const auto d = entry_check(s, o); !d.ok())
      cancel_order(s, id, d, events);
    else if (time >= last)
      cancel_order(s, id, failure(Reason::EXPIRED, "A contract reached its last trade"), events);
    else if (!o.system && cutoff > 0 && time >= last - cutoff)
      cancel_order(s, id, failure(Reason::EXPIRY_CUTOFF, "A contract reached the account's pre-expiry cutoff"), events);
    else if (time >= o.day_end)
      cancel_order(s, id, o.request.tif == TimeInForce::Gtd
          ? failure(Reason::GTD_END, "The order reached its good_till timestamp")
          : failure(Reason::DAY_END, "The order's session ended"), events);
  }
  if (s.revised_lifecycle) sweep_opening_stock_orders(s, skipped_end, events);
  walk_limits(s, events);
  expire_stock_orders(s, events);
}
namespace {
std::int64_t milli_strike(const md::OptionContract& c) { return std::llround(c.strike * 1000); }
}  // namespace
/// How a flatten closes `symbols`, the positions in scope (expired ones included,
/// since a short that has expired still holds its cover until it settles). Per
/// underlying and type, the latest-expiring shorts take the longs that cover them
/// (expiring with them or later; the nearest expiry, then the nearest strike, first),
/// which covers the most shorts, as naked_shorts counts them. Each such pair closes
/// as one two-leg order; the shorts and longs left over close alone. A long whose
/// short has expired waits for its settlement, in `held_back`.
std::vector<PlannedClose> plan_closes(const State& s, const std::vector<std::string>& symbols,
                                      std::map<std::string, Quantity>& held_back) {
  const auto expired = [&](const std::string& symbol) { return s.time >= s.contracts.at(symbol).expiry_time(); };
  std::map<std::pair<std::string, pricing::OptionType>, std::pair<std::vector<std::string>, std::vector<std::string>>> books;
  for (const auto& symbol : symbols) {
    const auto& c = s.contracts.at(symbol);
    auto& [shorts, longs] = books[{c.underlying, c.type}];
    (held(s, symbol) < 0 ? shorts : longs).push_back(symbol);
  }
  std::vector<PlannedClose> pairs, buys, sells;
  for (auto& [key, book] : books) {
    auto& [shorts, longs] = book;
    std::map<std::string, Quantity> left;
    for (const auto& symbol : longs) left[symbol] = held(s, symbol);
    std::stable_sort(shorts.begin(), shorts.end(), [&](const std::string& a, const std::string& b) {
      return s.contracts.at(a).expiry_time() > s.contracts.at(b).expiry_time();
    });
    for (const auto& short_symbol : shorts) {
      const auto& sold = s.contracts.at(short_symbol);
      auto need = -held(s, short_symbol);
      std::vector<std::string> covers;
      for (const auto& long_symbol : longs)
        if (s.contracts.at(long_symbol).expiry_time() >= sold.expiry_time()) covers.push_back(long_symbol);
      std::stable_sort(covers.begin(), covers.end(), [&](const std::string& a, const std::string& b) {
        const auto& x = s.contracts.at(a);
        const auto& y = s.contracts.at(b);
        if (x.expiry_time() != y.expiry_time()) return x.expiry_time() < y.expiry_time();
        return std::llabs(milli_strike(x) - milli_strike(sold)) < std::llabs(milli_strike(y) - milli_strike(sold));
      });
      for (const auto& long_symbol : covers) {
        const auto n = std::min(need, left[long_symbol]);
        if (n <= 0) continue;
        need -= n;
        left[long_symbol] -= n;
        if (expired(short_symbol)) {
          if (!expired(long_symbol)) held_back[long_symbol] += n;
        } else {
          pairs.push_back({{{short_symbol, Side::Buy, 1}, {long_symbol, Side::Sell, 1}}, n});
        }
      }
      if (need > 0 && !expired(short_symbol)) buys.push_back({{{short_symbol, Side::Buy, 1}}, need});
    }
    for (const auto& long_symbol : longs)
      if (left[long_symbol] > 0 && !expired(long_symbol)) sells.push_back({{{long_symbol, Side::Sell, 1}}, left[long_symbol]});
  }
  pairs.insert(pairs.end(), buys.begin(), buys.end());
  pairs.insert(pairs.end(), sells.begin(), sells.end());
  return pairs;
}
namespace {
/// Submit a reducer-owned market IOC that closes one position against the
/// current fresh book. Without executable liquidity nothing is recorded, so a
/// rule keeps retrying on later transactions instead of accumulating orders.
void flatten(State& s, const std::string& symbol, std::string_view why, Events& events, Quantity keep = 0) {
  const auto q = held(s, symbol);
  const auto contract = s.contracts.find(symbol);
  if (q == 0 || contract == s.contracts.end() || s.time >= contract->second.expiry_time() ||
      !regular(contract->second, s.time) || !quote_check(s, symbol).ok())
    return;
  const auto side = q > 0 ? Side::Sell : Side::Buy;
  // `keep` contracts of a long stay as cover for a short that could not be closed.
  const auto quantity = std::min(s.evaluation.status != EvaluationStatus::Active ? closing_capacity(s, symbol, side) : magnitude(q),
                                 std::max<Quantity>(magnitude(q) - keep, 0));
  if (quantity == 0) return;
  const auto& book = s.books.at(symbol);
  if (s.config.rules.impact_ticks == 0 && (side == Side::Sell ? book.bid_left : book.ask_left) <= 0) return;
  for (const auto id : open_ids(s))
    if (const auto& pending = s.orders[id - 1]; pending.open() && pending.system && pending.request.symbol == symbol) return;
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = {"system:" + std::string(why) + ":" + std::to_string(order.id), symbol, side,
                   OrderType::Market, TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  order.accepted_at = s.time;
  order.day_end = s.config.rules.fill_latency_ms > 0 ? session_end(contract->second, s.time) : s.time;
  order.system = true;
  order.actor = "system";
  add_order(s, order);
  event(events, "order_accepted", order);
  match_symbols(s, {symbol}, events, order.id);
  if (s.config.rules.fill_latency_ms == 0 && s.orders.at(static_cast<std::size_t>(order.id - 1)).open())
    cancel_order(s, order.id, failure(Reason::IOC_REMAINDER, ioc_remainder_message(s, s.orders.at(static_cast<std::size_t>(order.id - 1)),
                 "IOC exhausted available displayed liquidity")), events);
}
}  // namespace
/// Close shares at the underlying's fresh price in the regular session; without
/// one they stay, and a rule retries on later transactions.
void close_shares(State& s, const std::string& symbol, Quantity shares, StockSource source, Events& events) {
  const auto price = stock_price(s, symbol);
  if (shares == 0 || !price || !md::market_session(s.time).open) return;
  trade_shares(s, symbol, -shares, *price, source);
  if (source == StockSource::Trade) count_trade(s);
  event(events, "stock_trade", Json{{"symbol", symbol}, {"shares", -shares}, {"price", *price}});
}
/// All scheduled closes and position-rule liquidations share the manual flatten planner. Only executable
/// liquidity creates system IOCs; later transactions retry the remaining holdings.
void flatten_positions(State& s, std::string_view label, Events& events, const std::string& scope,
                       const std::vector<std::string>* only) {
  const auto named = [&](const std::string& symbol) {
    return !only || std::find(only->begin(), only->end(), symbol) != only->end();
  };
  std::vector<std::string> symbols;
  for (const auto& [symbol, position] : s.ledger.positions())
    if (scope.empty() || s.contracts.at(symbol).underlying == scope) symbols.push_back(symbol);
  std::map<std::string, Quantity> held_back;
  // Planned across every position in scope, so a named long that covers a short
  // closes with it; `only` then keeps the closes that touch a named position.
  const auto closes = plan_closes(s, symbols, held_back);
  for (const auto& close : closes) {
    if (std::none_of(close.legs.begin(), close.legs.end(), [&](const Leg& leg) { return named(leg.symbol); })) continue;
    // A delayed system IOC already owns these legs until it fills or ends.
    bool pending = false;
    for (const auto id : open_ids(s)) {
      const auto& o = s.orders[id - 1];
      if (o.system && std::any_of(close.legs.begin(), close.legs.end(), [&](const Leg& leg) { return touches(o.request, leg.symbol); }))
        pending = true;
    }
    if (pending) continue;
    for (Quantity placed = 0; placed < close.units;) {
      Order order;
      order.id = static_cast<OrderId>(s.orders.size() + 1);
      auto& r = order.request;
      r.client_order_id = "system:" + std::string(label) + ":" + std::to_string(order.id);
      r.type = OrderType::Market; r.tif = TimeInForce::Ioc;
      r.quantity = std::min(close.units - placed, s.config.limits.max_order_contracts);
      // A decided attempt keeps later user closes' reservations, as the other
      // decision liquidations do. Flat-time reductions are already reduce-only.
      if (s.evaluation.status != EvaluationStatus::Active)
        for (const auto& leg : close.legs) r.quantity = std::min(r.quantity, closing_capacity(s, leg.symbol, leg.side));
      if (r.quantity == 0) break;
      if (close.legs.size() == 1) { r.symbol = close.legs.front().symbol; r.side = close.legs.front().side; }
      else r.legs = close.legs;
      order.system = true; order.reduce_only = true; order.actor = "system";
      order.accepted_at = s.time;
      // Calendar rules retain their two-sided quote requirement. Mandatory flat
      // and overnight closes retain Flatten's ask-only reduction policy.
      if ((label == "news" || label == "hold") && std::any_of(close.legs.begin(), close.legs.end(),
          [&](const Leg& leg) { return !quote_check(s, leg.symbol).ok(); })) break;
      if (!system_check(s, order).ok()) break;
      order.day_end = order_end(s, r, s.time);
      bool liquid = true;
      for (const auto& leg : close.legs) {
        const auto& book = s.books.at(leg.symbol);
        if (s.config.rules.impact_ticks == 0 && (leg.side == Side::Buy ? book.ask_left : book.bid_left) <= 0 && !given_away(s, order, leg))
          liquid = false;
      }
      if (!liquid) break;
      placed += r.quantity;
      add_order(s, order);
      event(events, "order_accepted", order);
      // Only this mandatory close matches here; unrelated orders follow it.
      match_order(s, order.id, events, order.id);
      if (s.config.rules.fill_latency_ms == 0 && s.orders[order.id - 1].open())
        cancel_order(s, order.id, failure(Reason::IOC_REMAINDER,
            ioc_remainder_message(s, s.orders[order.id - 1], "IOC exhausted available displayed liquidity")), events);
      if (s.config.rules.fill_latency_ms > 0) break;
    }
  }
  std::vector<std::pair<std::string, Quantity>> stocks;
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if ((scope.empty() || symbol == scope) && named(symbol)) stocks.emplace_back(symbol, stock.shares);
  for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Rule, events);
  if (!holds_scope(s, {}, s.time)) s.evaluation.flat_pending = false;
}
namespace {
/// The status's own code (a pass on the target, a failure on the floor) is the
/// default a journal leaves out; any other rule's code is recorded with the decision.
bool default_code(EvaluationStatus status, Reason code) {
  return code == (status == EvaluationStatus::Passed ? Reason::PROFIT_TARGET : Reason::DRAWDOWN_FLOOR);
}
void decide(State& s, const PlanVerdict& verdict, Money equity, Events& events) {
  auto& e = s.evaluation;
  e.status = verdict.status;
  e.decided_at = s.time;
  e.decided_equity = equity;
  e.decision = verdict.message;
  e.decision_code = verdict.code;
  Json payload{{"attempt", e.attempt}, {"equity", equity}, {"peak", e.peak}, {"floor", e.floor}, {"message", verdict.message}};
  if (!default_code(verdict.status, verdict.code)) payload["code"] = verdict.code;
  event(events, verdict.status == EvaluationStatus::Passed ? "evaluation_passed" : "evaluation_failed", std::move(payload));
  for (const auto id : open_ids(s))
    if (!s.orders[id - 1].system) cancel_order(s, id, failure(Reason::EVALUATION_CLOSED, verdict.message), events);
  if (s.revised_lifecycle) cancel_opening_stock_orders(s, failure(Reason::EVALUATION_CLOSED, verdict.message), events);
}
/// Runs before any executions, including late mandatory closes after a gap.
void record_holding_boundaries(State& s, Events& events) {
  const auto& r = s.config.rules;
  if (r.hold_restrictions.empty()) return;
  auto& e = s.evaluation;
  const auto from = e.event_checked > 0 ? e.event_checked : (e.started > 0 ? e.started : s.time);
  const auto windows = event_windows(r, from, s.time);
  // Record every crossed boundary before any late calendar close can remove
  // evidence of a position held across it (including an overlapping news close).
  for (const auto& w : windows) {
    if (w.kind != "news" && w.end <= s.time && from < w.end && holds_scope(s, w.symbol, w.end)) {
      const auto violation = w.kind + ":" + (w.symbol.empty() ? "account" : w.symbol);
      if (std::find(e.holding_violations.begin(), e.holding_violations.end(), violation) == e.holding_violations.end()) {
        e.holding_violations.push_back(violation);
        event(events, "holding_boundary_crossed", Json{{"key", w.key}, {"scope", violation}, {"boundary", w.end}});
      }
    }
  }
}
}  // namespace
void monitor_calendar(State& s, Events& events) {
  const auto& r = s.config.rules;
  if (r.events.empty() && r.hold_restrictions.empty()) return;
  auto& e = s.evaluation;
  const auto from = e.event_checked > 0 ? e.event_checked : (e.started > 0 ? e.started : s.time);
  const auto windows = event_windows(r, from, s.time);
  // Same-transaction close ties go to flat time. Without a new calendar close,
  // preserve F6's normal equity-observation order, including future-only calendars.
  const bool calendar_close = std::any_of(windows.begin(), windows.end(), [&](const EventWindow& w) {
    return (w.kind != "news" || (s.time < w.end && r.news_action == "flatten")) && holds_scope(s, w.symbol, s.time) &&
        std::find(e.event_actions.begin(), e.event_actions.end(), w.key) == e.event_actions.end();
  });
  if (calendar_close && e.status == EvaluationStatus::Active && e.day_lock == Reason::NONE && e.flat_pending)
    flatten_positions(s, "flat_time", events);
  for (const auto& w : windows) {
    // A news window skipped in its entirety never acts retroactively.
    if (w.kind == "news" && s.time >= w.end) continue;
    if (std::find(e.event_actions.begin(), e.event_actions.end(), w.key) != e.event_actions.end()) continue;
    for (const auto id : open_ids(s)) {
      const auto& order = s.orders[id - 1];
      if (order.system || kept_within(order) || closing_only(s, order)) continue;
      const auto symbols = order_symbols(order.request);
      if (std::any_of(symbols.begin(), symbols.end(), [&](const auto& symbol) {
        return w.symbol.empty() || s.contracts.at(symbol).underlying == w.symbol;
      })) cancel_order(s, id, event_decision(r, w, s.time), events);
    }
    e.event_actions.push_back(w.key);
    event(events, "calendar_action", Json{{"key", w.key}, {"kind", w.kind}, {"start", w.start}, {"end", w.end}});
    if (e.status == EvaluationStatus::Active && (w.kind != "news" || r.news_action == "flatten"))
      flatten_positions(s, w.kind == "news" ? "news" : "hold", events, w.symbol);
  }
  e.event_checked = s.time;
}
namespace {
/// A plan limit locks the trading day: open orders cancel with its code, the
/// positions close (monitor_rules) and only closing orders are accepted until rollover.
void lock_day(State& s, const PlanVerdict& verdict, Money equity, Events& events) {
  auto& e = s.evaluation;
  e.day_lock = verdict.code;
  e.day_locked_at = s.time;
  event(events, "day_locked", Json{{"attempt", e.attempt}, {"code", verdict.code}, {"equity", equity},
                                   {"level", verdict.level}, {"message", verdict.message}});
  for (const auto id : open_ids(s))
    if (!s.orders[id - 1].system) cancel_order(s, id, failure(verdict.code, verdict.message), events);
  if (s.revised_lifecycle) cancel_opening_stock_orders(s, failure(verdict.code, verdict.message), events);
}
}  // namespace
/// What the plan's rules read of the account now, at marked `equity`.
PlanInputs plan_inputs(const State& s, Money equity) {
  PlanInputs in;
  in.time = s.time;
  in.equity = equity;
  // Cash plus the positions' cost: the closed balance, as if nothing were open.
  in.balance = s.ledger.account().cash;
  for (const auto& [symbol, position] : s.ledger.positions()) in.balance = in.balance + position.basis;
  for (const auto& [symbol, stock] : s.ledger.stocks()) in.balance = in.balance + stock.basis;
  in.net_realised = net_realised(s);
  in.flat = s.ledger.positions().empty() && s.ledger.stocks().empty();
  return in;
}
/// Runs after every command: tracks the day's closing equity, then applies the
/// plan's verdict on fully marked equity (evaluate_plan: the floor, the daily
/// loss limit, the target and its objectives); monitor_rules then liquidates a
/// decided attempt or a locked day and auto-closes expiring positions.
// Derive enabled trade objectives from reducer history, including non-fill closures.
// Whole trades use the same grouping as the trade journal, so rolls count once.
void refresh_trade_objectives(State& s) {
  const auto& rules = s.config.rules;
  const bool funded = rules.phase == Phase::Funded;
  const bool objectives = !funded && (rules.trade_consistency_percent > 0 || rules.min_trades > 0 ||
                                      rules.microscalp_percent > 0 || rules.best_position_percent > 0);
  const bool review = funded && rules.payouts.review.has_value();
  if (!objectives && !review) return;
  auto& e = s.evaluation;
  struct Whole { Money pnl; bool closed = true; std::uint64_t first = 0; Timestamp opened = 0, last = 0; bool open_loss = false; };
  const State::TradeObjectives inputs{s.fills.size(), s.closures.size(), s.stock_fills.size(), s.dividends.size(),
                                      e.attempt, e.first_fill, e.first_stock_fill, s.groups, e.cycle_started, e.open_loss_trades.size()};
  if (const auto& cached = s.trade_objectives; cached && cached->fills == inputs.fills &&
      cached->closures == inputs.closures && cached->stock_fills == inputs.stock_fills &&
      cached->dividends == inputs.dividends && cached->attempt == inputs.attempt &&
      cached->first_fill == inputs.first_fill && cached->first_stock_fill == inputs.first_stock_fill &&
      cached->groups.same(inputs.groups) && cached->cycle_started == inputs.cycle_started &&
      cached->open_loss_trades == inputs.open_loss_trades) return;
  std::map<std::string, Whole> trades;
  e.short_profit = {};
  for (const auto& life : lifecycles(s.fills, s.closures, s.contracts)) {
    if (life.first_fill < e.first_fill) continue;
    if (rules.microscalp_percent > 0 && life.closed && *life.closed - life.opened < rules.microscalp_seconds * md::kNanosPerSecond)
      e.short_profit = e.short_profit + std::max(Money{}, life.gross - life.fees);
    auto& trade = trades[trade_group(life, s.groups)];
    trade.pnl = trade.pnl + life.gross - life.fees;
    trade.closed = trade.closed && life.closed.has_value();
    if (trade.first == 0 || life.first_fill < trade.first) trade.first = life.first_fill;
    if (trade.opened == 0 || life.opened < trade.opened) trade.opened = life.opened;
    if (life.closed) trade.last = std::max(trade.last, *life.closed);
    const auto id = std::to_string(life.first_fill);
    trade.open_loss = trade.open_loss || std::find(e.open_loss_trades.begin(), e.open_loss_trades.end(), id) != e.open_loss_trades.end();
  }
  if (rules.microscalp_percent > 0)
    for (const auto& life : share_lifecycles(s.stock_fills, s.dividends))
      if (life.fills.front() >= e.first_stock_fill && life.closed &&
          *life.closed - life.opened < rules.microscalp_seconds * md::kNanosPerSecond)
        e.short_profit = e.short_profit + std::max(Money{}, life.gross + life.dividends);
  e.best_trade.reset();
  e.best_position.reset();
  e.position_excess = {};
  e.closed_trades = 0;
  e.cycle_trades.clear();
  std::uint64_t best_first = 0, position_first = 0;
  const auto better = [](const std::optional<BestTrade>& best, std::uint64_t best_at, Money pnl, std::uint64_t first) {
    return !best || pnl > best->pnl || (pnl == best->pnl && first < best_at);
  };
  for (const auto& [id, trade] : trades) {
    if (!trade.closed) continue;
    if (review && trade.pnl > Money{} && in_payout_cycle(e, trade.last))
      e.cycle_trades.push_back({id, trade.pnl, trade.last - trade.opened, trade.last, trade.open_loss});
    if (!objectives) continue;
    if (rules.min_trades > 0) ++e.closed_trades;
    if (trade.pnl <= Money{}) continue;
    if (rules.best_position_percent > 0) {
      e.position_excess = e.position_excess + std::max(Money{}, trade.pnl - best_position_cap(rules));
      if (better(e.best_position, position_first, trade.pnl, trade.first)) {
        e.best_position = BestTrade{id, trade.pnl};
        position_first = trade.first;
      }
    }
    if (rules.trade_consistency_percent == 0) continue;
    if (better(e.best_trade, best_first, trade.pnl, trade.first)) {
      e.best_trade = BestTrade{id, trade.pnl};
      best_first = trade.first;
    }
  }
  // Share round trips (exercise and assignment deliveries, and share trades) are
  // trades of their own, net of dividends; they carry no fees.
  if (review)
    for (const auto& life : share_lifecycles(s.stock_fills, s.dividends)) {
      const auto pnl = life.gross + life.dividends;
      if (!life.closed || pnl <= Money{} || !in_payout_cycle(e, *life.closed)) continue;
      const auto id = "s" + std::to_string(life.fills.front());
      e.cycle_trades.push_back({id, pnl, *life.closed - life.opened, *life.closed,
          std::find(e.open_loss_trades.begin(), e.open_loss_trades.end(), id) != e.open_loss_trades.end()});
    }
  // The review reads them in the order they closed.
  std::stable_sort(e.cycle_trades.begin(), e.cycle_trades.end(), [](const auto& a, const auto& b) { return a.closed < b.closed; });
  s.trade_objectives = inputs;
}
namespace {
/// The trade each held contract or stock is in, by the trades view's ID: an
/// option's trade_group, else its round trip; shares, which never join a trade, are
/// a trade of their own ("s" and the opening stock fill).
std::map<std::string, std::string> held_trades(const State& s) {
  std::map<std::string, std::string> out;
  if (s.reviewing.ready) {
    for (const auto& [symbol, open] : s.reviewing.builder.open) out[symbol] = trade_group(open.life, s.groups);
  } else {
    for (const auto& life : lifecycles(s.fills, s.closures, s.contracts))
      if (!life.closed) out[life.symbol] = trade_group(life, s.groups);
  }
  for (const auto& [symbol, trip] : s.trips) out.try_emplace(symbol, trip);
  return out;
}
/// The open-loss cap (OpenLossCap) on each trade's marked open P&L, net across its
/// legs, so a hedged spread is measured as one: a trade whose open loss reaches the
/// cap is journaled once (`open_loss_cap`) and its round trips remembered for payout
/// reviews; a Fail action fails the attempt, a Flatten action has monitor_rules close
/// the whole trade, and BlockAdds refuses adding to its legs while it stays there.
/// Trades with a leg without a mark are not measured.
void check_open_loss(State& s, const PlanInputs& inputs, Events& events) {
  auto& e = s.evaluation;
  const auto& rules = s.config.rules;
  if (!rules.open_loss_cap) return;
  const auto limit = open_loss_limit(e, rules, inputs);
  struct Trade { std::vector<std::string> symbols; Money pnl; bool marked = true; };
  std::map<std::string, Trade> trades;
  if (limit) {
    const auto m = measure(s);
    const auto of = held_trades(s);
    const auto add = [&](const std::string& symbol, const std::optional<Money>& unrealised) {
      const auto it = of.find(symbol);
      auto& trade = trades[it == of.end() ? symbol : it->second];
      trade.symbols.push_back(symbol);
      if (unrealised) trade.pnl = trade.pnl + *unrealised;
      else trade.marked = false;
    };
    for (const auto& p : m.positions) add(p.position.contract.osi_symbol(), p.unrealised);
    for (const auto& p : m.stocks) add(p.position.symbol, p.unrealised);
  }
  std::vector<std::string> symbols;
  if (e.revised_rules && rules.open_loss_cap->action == OpenLossAction::Flatten)
    for (const auto& symbol : e.open_loss_symbols)
      if (held(s, symbol) != 0 || shares_held(s, symbol) != 0) symbols.push_back(symbol);
  std::optional<std::pair<std::string, Money>> first;
  constexpr const char* actions[] = {"flatten", "block_adds", "fail"};
  for (const auto& [id, trade] : trades) {
    if (!trade.marked || trade.pnl >= Money{} || -trade.pnl < *limit) continue;
    const auto loss = -trade.pnl;
    if (!first) first.emplace(id, loss);
    const bool known = std::any_of(trade.symbols.begin(), trade.symbols.end(), [&](const std::string& symbol) {
      return std::find(e.open_loss_symbols.begin(), e.open_loss_symbols.end(), symbol) != e.open_loss_symbols.end();
    });
    for (const auto& symbol : trade.symbols)
      if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) symbols.push_back(symbol);
    if (known) continue;
    for (const auto& symbol : trade.symbols)
      if (const auto trip = s.trips.find(symbol); trip != s.trips.end() &&
          std::find(e.open_loss_trades.begin(), e.open_loss_trades.end(), trip->second) == e.open_loss_trades.end())
        e.open_loss_trades.push_back(trip->second);
    event(events, "open_loss_cap", Json{{"trade", id}, {"symbols", trade.symbols}, {"symbol", trade.symbols.front()},
        {"loss", loss}, {"limit", *limit}, {"action", actions[static_cast<int>(rules.open_loss_cap->action)]}});
  }
  e.open_loss_symbols = std::move(symbols);
  if (first && rules.open_loss_cap->action == OpenLossAction::Fail)
    decide(s, {EvaluationStatus::Failed, false, Reason::OPEN_LOSS_CAP, *limit,
        "The open loss of trade " + first->first + ", " + dollars(first->second) + ", reached the open-loss cap of " +
        dollars(*limit)}, inputs.equity, events);
}
}  // namespace
void observe_equity(State& s, Events& events) {
  refresh_trade_objectives(s);
  const auto& rules = s.config.rules;
  auto& e = s.evaluation;
  // A journal created before any market data starts at time zero; the attempt
  // begins at the first real market time instead.
  if (e.started == 0 && s.time > 0) e.started = s.time;
  // Likewise the first payout cycle; journals from before payouts start it here too.
  if (e.cycle_started == 0) e.cycle_started = e.started;
  // Older live journals have no opening floor; freeze the first available level
  // until the next rollover supplies an exact daily opening baseline.
  if (e.revised_rules && !e.day_open_floor && rules.open_loss_cap && rules.open_loss_cap->basis != OpenLossBasis::Amount)
    e.day_open_floor = e.floor;
  if (e.size_scaling && e.size_scaling->period_started < plan_trading_date(rules, e.started))
    e.size_scaling->period_started = plan_trading_date(rules, e.started);
  if (const auto equity = marked_equity(measure(s))) {
    if (plan_trading_date(rules, s.time) == e.day) {
      e.day_close_equity = *equity;
      if (!e.day_low_equity || *equity < *e.day_low_equity) { e.day_low_equity = *equity; e.day_low_at = s.time; }
      if (!e.day_high_equity || *equity > *e.day_high_equity) { e.day_high_equity = *equity; e.day_high_at = s.time; }
    }
    const auto inputs = plan_inputs(s, *equity);
    if (rules.drawdown_basis == DrawdownBasis::Balance && plan_trading_date(rules, s.time) == e.day)
      e.day_close_balance = inputs.balance;
    // A crossed holding boundary is decided at rollover; a close in the meantime
    // cannot turn that pending failure into a pass.
    if (e.status == EvaluationStatus::Active && e.holding_violations.empty()) {
      const auto verdict = evaluate_plan(e, rules, inputs);
      // Include the deciding observation, then freeze the closest approach. Fills
      // after the decision still mark equity but never evaluate the attempt again.
      const auto measured = rules.drawdown_basis == DrawdownBasis::Balance ? inputs.balance : *equity;
      if (rules.max_drawdown > Money{} && e.started > 0 && (!e.closest_floor || measured - e.floor < *e.closest_floor)) {
        e.closest_floor = measured - e.floor;
        e.closest_floor_at = s.time;
      }
      if (verdict.decided()) decide(s, verdict, *equity, events);
      else if (verdict.lock) lock_day(s, verdict, *equity, events);
      else check_open_loss(s, inputs, events);
    }
    const auto soft_floor = soft_floor_of(s);
    const auto& g = s.config.guardrails;
    if (soft_floor && *equity <= *soft_floor) latch_guardrail(s, Reason::SOFT_FLOOR, events);
    if (g.profit_lock > Money{} && *equity - s.start_equity >= g.profit_lock) latch_guardrail(s, Reason::PROFIT_LOCK, events);
    if (g.max_opening_trades > 0 && s.guardrails.opening_trades >= g.max_opening_trades)
      latch_guardrail(s, Reason::TRADE_LIMIT, events);
  }
}
namespace {
/// The client ID label of a plan's liquidation: the decision's, else a locked day's.
std::string_view liquidation_label(const Evaluation& e, bool decided) {
  const auto code = decided ? e.decision_code : e.day_lock;
  if (code == Reason::DAILY_LOSS_LIMIT) return "daily_loss";
  if (code == Reason::TIME_LIMIT) return "time_limit";
  if (code == Reason::INACTIVITY) return "inactivity";
  if (code == Reason::OVERNIGHT_HOLD) return "overnight";
  if (code == Reason::HOLD_RESTRICTED) return "hold";
  if (code == Reason::OPEN_LOSS_CAP) return "open_loss";
  if (decided) return e.status == EvaluationStatus::Passed ? "target" : "drawdown";
  return "day_lock";
}
}  // namespace
void monitor_rules(State& s, Events& events) {
  start_flat_time(s, events);
  observe_equity(s, events);
  const auto& rules = s.config.rules;
  const auto& e = s.evaluation;
  const bool soft = std::find(s.guardrails.latched.begin(), s.guardrails.latched.end(), Reason::SOFT_FLOOR) != s.guardrails.latched.end();
  const bool decided = rules.evaluation() && e.status != EvaluationStatus::Active;
  const bool locked = e.day_lock != Reason::NONE;
  if (decided && (e.decision_code == Reason::OVERNIGHT_HOLD || e.decision_code == Reason::HOLD_RESTRICTED)) {
    flatten_positions(s, liquidation_label(e, true), events);
    return;
  }
  if (!decided && !locked && e.flat_pending) {
    flatten_positions(s, "flat_time", events);
    return;
  }
  if (soft || decided || locked) {
    std::vector<std::pair<std::string, Quantity>> stocks;
    for (const auto& [symbol, stock] : s.ledger.stocks()) stocks.emplace_back(symbol, stock.shares);
    for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Rule, events);
  } else if (rules.open_loss_cap && rules.open_loss_cap->action == OpenLossAction::Flatten && !e.open_loss_symbols.empty()) {
    // The whole trade closes as Flatten closes positions: a spread as one order and
    // shorts before the longs that cover them, so no leg is left naked.
    const auto over = e.open_loss_symbols;
    flatten_positions(s, "open_loss", events, {}, &over);
  }
  std::vector<std::string> symbols;
  for (const auto& [symbol, position] : s.ledger.positions()) symbols.push_back(symbol);
  // Shorts first, as Flatten closes them: a short bought back before the long that
  // covers it is sold never leaves a naked short, even for one event.
  std::stable_partition(symbols.begin(), symbols.end(), [&](const std::string& symbol) { return held(s, symbol) < 0; });
  // A short this pass could not buy back in full (stale quote, thin ask, partial IOC)
  // still needs its cover: longs of its underlying and type keep that many.
  std::map<std::pair<std::string, pricing::OptionType>, Quantity> uncovered;
  const auto cover = [&](const std::string& symbol) -> Quantity& {
    const auto& c = s.contracts.at(symbol);
    return uncovered[{c.underlying, c.type}];
  };
  const auto close = [&](const std::string& symbol, std::string_view why) {
    if (held(s, symbol) < 0) {
      flatten(s, symbol, why, events);
      cover(symbol) += magnitude(held(s, symbol));
      return;
    }
    auto& need = cover(symbol);
    const auto keep = std::min(need, magnitude(held(s, symbol)));
    need -= keep;
    flatten(s, symbol, why, events, keep);
  };
  for (const auto& symbol : symbols) {
    if (soft || decided || locked) {
      // The plan's decision names the liquidation, then a locked day, even when the
      // soft floor latched with it.
      close(symbol, decided || locked ? liquidation_label(e, decided) : "soft_floor");
      continue;
    }
    // The cutoff counts back from the last trade: 15:55 for SPXW, 16:10 for SPY,
    // and the afternoon before expiry for AM-settled series.
    const auto& contract = s.contracts.at(symbol);
    if (rules.expiry_cutoff > 0 && s.time >= contract.last_trade_time() - rules.expiry_cutoff &&
        s.time < contract.last_trade_time()) {
      for (const auto id : open_ids(s))
        if (const auto& o = s.orders[id - 1]; !o.system && touches(o.request, symbol))
          cancel_order(s, id, failure(Reason::EXPIRY_CUTOFF, "Pre-expiry cutoff: the position is being closed"), events);
      close(symbol, "expiry");
    }
  }
}
}  // namespace session_internal
}  // namespace openport::trading
