// Stops and trailing triggers: the values they watch, trailing levels, and activating
// the orders they arm.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
/// What a trigger reads now. Option triggers read the order's executable side
/// from a fresh book, or its mid or mark; underlying triggers read spot from a
/// fresh valuation; combo triggers sum the closing legs the same way, weighted by
/// ratio. Missing data reads nothing; an exit's leg that shows only an ask counts
/// at that ask, or at zero when the exit sells it.
std::optional<Money> trigger_value(const State& s, const Order& o) {
  const auto& t = *o.request.trigger;
  if (t.source == TriggerSource::Time) return std::nullopt;  // reached reads the clock
  if (conditional(t)) {
    // Another underlying's price or a study, as the integration last supplied it.
    const auto symbol = t.symbol.empty() ? s.contracts.at(order_symbols(o.request).front()).underlying : t.symbol;
    const auto it = s.indicators.find(indicator_key(symbol, t.study));
    if (it != s.indicators.end() && it->second.time <= s.time && s.time - it->second.time <= s.config.limits.max_valuation_age)
      return it->second.price;
    return std::nullopt;
  }
  // One contract's value under the trigger's reference, signed by the side it trades.
  const auto read = [&](const std::string& symbol, Side side) -> std::optional<Money> {
    if (t.reference == TriggerReference::Mark) {
      if (!marked_now(s, symbol)) return std::nullopt;
      return mark_of(s.books.at(symbol).quote);
    }
    if (!quote_check(s, symbol).ok()) return std::nullopt;
    const auto& q = s.books.at(symbol).quote;
    if (t.reference == TriggerReference::Mid) return mid(q);
    return side == Side::Buy ? *q.ask : *q.bid;
  };
  if (t.source == TriggerSource::Underlying) {
    const auto it = s.valuations.find(order_symbols(o.request).front());
    if (it != s.valuations.end() && valid_valuation(it->second) && it->second.time <= s.time &&
        s.time - it->second.time <= s.config.limits.max_valuation_age) {
      try { return Money::from_double(it->second.spot); } catch (const TradingError&) {}
    }
    return std::nullopt;
  }
  if (t.source == TriggerSource::Combo && multi_leg(o.request)) {
    Money net;
    for (const auto& leg : o.request.legs) {
      std::optional<Money> value;
      if (t.reference == TriggerReference::BidAsk && given_away(s, o, leg)) continue;
      if (t.reference == TriggerReference::BidAsk && ask_only(s, o, leg)) value = *s.books.at(leg.symbol).quote.ask;
      else value = read(leg.symbol, leg.side);
      if (!value) return std::nullopt;
      net = net + (leg.side == Side::Buy ? *value : -*value) * leg.ratio;
    }
    return net;
  }
  return read(o.request.symbol, o.request.side);
}
bool reached(const State& s, const Order& o) {
  const auto& t = *o.request.trigger;
  if (t.source == TriggerSource::Time) {
    const auto minute = md::new_york_time(s.time).seconds / 60;
    return t.direction == TriggerDirection::AtOrBelow ? minute <= t.minute : minute >= t.minute;
  }
  const auto value = trigger_value(s, o);
  return value && (t.direction == TriggerDirection::AtOrBelow ? *value <= t.level : *value >= t.level);
}
namespace {
/// `value` rounded down (or up) to a multiple of `step`, which is positive.
Money floor_to(Money value, std::int64_t step) {
  auto q = value.micros() / step;
  if (value.micros() % step != 0 && value.micros() < 0) --q;
  return Money::from_micros(q * step);
}
Money ceil_to(Money value, std::int64_t step) { return -floor_to(-value, step); }
/// A trailing trigger follows its reference while armed: when the value it reads
/// less the trail (plus it, for at_or_above) passes the level, the level moves
/// there in whole cents, and it never moves back. An option or combo stop-limit's
/// limit moves with it, kept on the tick by moving both a little less, never more.
void trail_level(State& s, OrderId id, Events& events) {
  const auto& o = s.orders[id - 1];
  const auto& t = *o.request.trigger;
  if (!t.trail) return;
  const auto value = trigger_value(s, o);
  if (!value) return;
  const auto& root = s.contracts.at(order_symbols(o.request).front()).root;
  // The combo tick: the smallest of its legs' ticks.
  const auto combo_tick = [&] {
    Money tick;
    for (const auto& leg : o.request.legs) {
      const auto leg_tick = tick_size(s.contracts.at(leg.symbol).root, Money{});
      tick = tick == Money{} ? leg_tick : std::min(tick, leg_tick);
    }
    return tick;
  };
  const auto magnitude = *value < Money{} ? -*value : *value;
  Money distance;
  switch (t.trail->unit) {
    case TrailUnit::Amount: distance = t.trail->value; break;
    case TrailUnit::Percent: distance = magnitude.prorate(t.trail->value.micros(), 100'000'000); break;
    case TrailUnit::Ticks:
      distance = (multi_leg(o.request) ? combo_tick() : tick_size(root, magnitude)) * (t.trail->value.micros() / 1'000'000);
      break;
  }
  const bool below = t.direction == TriggerDirection::AtOrBelow;
  const auto candidate = below ? floor_to(*value - distance, 10'000) : ceil_to(*value + distance, 10'000);
  auto move = candidate - t.level;
  if (below ? move <= Money{} : move >= Money{}) return;
  std::optional<Money> limit = o.request.limit_price;
  // A limit moves with a level in its own units: an option's or a combo's price. An
  // underlying level is spot, so it trails alone and the limit stays where it was
  // set (driver 8; replay drivers 1-7 moved the limit by spot's move as well).
  if (limit && (t.source != TriggerSource::Underlying || !s.revised_fills)) {
    const auto raw = *limit + move;
    const auto tick = multi_leg(o.request) ? combo_tick() : tick_size(root, raw);
    const auto snapped = move > Money{} ? floor_to(raw, tick.micros()) : ceil_to(raw, tick.micros());
    move = snapped - *limit;
    if (move == Money{} || (!multi_leg(o.request) && snapped <= Money{})) return;
    limit = snapped;
  }
  auto& order = s.orders.mut(id - 1);
  // A retry of the submission still finds the order its trail has moved.
  if (!order.submitted && order.parent == 0) order.submitted = order.request;
  order.request.trigger->level = order.request.trigger->level + move;
  order.request.limit_price = limit;
  event(events, "order_trailed", order);
}
/// A reached order runs its checks now: exits need only an executable book;
/// entries take every pre-trade check. Stale data or a closed session keeps it
/// armed for a later batch; any other failure cancels it with RISK_CHANGED.
/// Stop stress (AccountRules::stop_stress_ticks): a price-triggered order is
/// reached in a fast market when any of its contracts' latest quote was fast.
bool fast_market(const State& s, const Order& o) {
  if (s.config.rules.stop_stress_ticks == 0 || !o.request.trigger || conditional(*o.request.trigger)) return false;
  for (const auto& symbol : order_symbols(o.request)) {
    const auto book = s.books.find(symbol);
    if (book != s.books.end() && book->second.activity && book->second.activity->fast) return true;
  }
  return false;
}
}  // namespace
/// The next measure of normal quoting after a new observation of a contract: an
/// observation is fast when, after eight valid ones, its spread is at least
/// stop_stress_percent of the average, or the time since the one before at most
/// 100 / stop_stress_percent of the average gap (quotes changing that much faster).
detail::QuoteActivity next_activity(const State& s, const detail::Book& before, const QuoteObservation& quote) {
  auto a = before.activity.value_or(detail::QuoteActivity{});
  a.fast = false;
  if (!valid_quote(quote)) return a;
  constexpr std::int64_t kWarm = 8;
  const auto percent = s.config.rules.stop_stress_percent;
  const auto spread = *quote.ask - *quote.bid;
  const bool timed = valid_quote(before.quote) && before.quote.time > 0 && quote.time >= before.quote.time;
  const auto gap = timed ? quote.time - before.quote.time : Timestamp{0};
  __extension__ using Wide = __int128;
  if (a.samples >= kWarm)
    a.fast = static_cast<Wide>(spread.micros()) * 100 >= static_cast<Wide>(a.spread.micros()) * percent ||
             (timed && a.gap > 0 && static_cast<Wide>(gap) * percent <= static_cast<Wide>(a.gap) * 100);
  a.spread = a.samples == 0 ? spread : a.spread + Money::from_micros((spread - a.spread).micros() / 8);
  if (timed) a.gap = a.gap == 0 ? gap : a.gap + (gap - a.gap) / 8;
  a.samples = std::min(a.samples + 1, kWarm);
  return a;
}
namespace {
void activate(State& s, OrderId id, Events& events) {
  {
    const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
    auto d = o.role != OrderRole::Normal ? system_check(s, o) : order_check(s, o, Stage::Activate);
    if (data_gap(d.code) || d.code == Reason::SESSION_CLOSED || d.code == Reason::LIMIT_ONLY) return;
    if (!d.ok()) {
      if (d.code != Reason::SCALING_LIMIT) {
        d.message = std::string(to_string(d.code)) + ": " + d.message;
        d.code = Reason::RISK_CHANGED;
      }
      cancel_order(s, id, d, events);
      return;
    }
    const bool stressed = fast_market(s, o);
    auto& order = s.orders.mut(id - 1);
    order.status = OrderStatus::Working;
    order.triggered_at = s.time;
    // Each activation meets its own market: a stop exit re-armed after a fast one
    // and reached again on an ordinary quote drops its stress (driver 8; replay
    // drivers 1-7 kept it).
    if (stressed) order.stress_ticks = s.config.rules.stop_stress_ticks;
    else if (s.revised_fills) order.stress_ticks = 0;
    if (order.request.tif == TimeInForce::Day) order.day_end = day_deadline(s, order.request, s.time);
    event(events, "order_triggered", order);
  }
  const auto symbols = order_symbols(s.orders.at(static_cast<std::size_t>(id - 1)).request);
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  if (s.config.rules.fill_latency_ms == 0) end_ioc(s, id, "IOC exhausted available displayed liquidity", events);
}
/// Whether a stop exit's book has nothing left to fill it: a leg's closing side, which
/// the quote shows with size, has had that size used up by earlier fills. A market
/// stop exit triggered then would find nothing, re-arm (end_ioc) and trigger again on
/// the next transaction while the same quote stands, so it waits for new size instead
/// (driver 11; replay drivers 1-10 triggered and re-armed it on every transaction). A
/// side the quote does not show is not used up: an exit gives away a long nobody bids
/// for. Only without impact or fill latency, which fill beyond displayed size or later;
/// a stop-limit exit rests as a limit once triggered, so it never re-arms.
bool exit_without_size(const State& s, const Order& o) {
  if (!s.revised_lifecycle || o.role == OrderRole::Normal || !immediate(o.request) ||
      s.config.rules.impact_ticks != 0 || s.config.rules.fill_latency_ms != 0)
    return false;
  auto legs = o.request.legs;
  if (legs.empty()) legs.push_back({o.request.symbol, o.request.side, 1});
  return std::any_of(legs.begin(), legs.end(), [&](const Leg& leg) {
    const auto book = s.books.find(leg.symbol);
    if (book == s.books.end()) return false;
    const auto& q = book->second.quote;
    const bool buy = leg.side == Side::Buy;
    const Quantity shown = buy ? (q.ask ? q.ask_size : 0) : (q.bid ? q.bid_size : 0);
    return shown > 0 && (buy ? book->second.ask_left : book->second.bid_left) / leg.ratio <= 0;
  });
}
}  // namespace
/// Armed orders activate in eligible sessions; only EXTO/GTC_EXTO extend them.
void check_triggers(State& s, Events& events) {
  const auto visit = [&](OrderId id) {
    const auto& o = s.orders[id - 1];
    const auto& contract = s.contracts.at(order_symbols(o.request).front());
    if (o.status != OrderStatus::Armed || !trades_now(o, contract, s.time)) return;
    // A trail follows what the trigger reads first, in the regular session; it
    // cannot reach the level it sets.
    if (regular(contract, s.time)) trail_level(s, id, events);
    if (reached(s, s.orders[id - 1]) && !exit_without_size(s, s.orders[id - 1])) activate(s, id, events);
  };
  // The open orders in ID order, then any placed while activating them (a
  // bracket's exits), in order, as one pass over every order would find them.
  const auto placed = s.orders.size();
  for (const auto id : open_ids(s)) if (id <= placed) visit(id);
  for (auto i = placed; i < s.orders.size(); ++i) visit(static_cast<OrderId>(i + 1));
}
}  // namespace session_internal
}  // namespace openport::trading
