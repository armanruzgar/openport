// Matching paper orders against quotes: the queue model, single-leg and combo fills,
// IOC remainders, closing prices, and the bracket exits a fill attaches.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
namespace {
Decision opening_fill_check(const State& s, const Order& order, Quantity quantity, Events& events) {
  if (!s.opening_rule_inputs || order.system || kept_within(order)) return {};
  const auto& inputs = *s.opening_rule_inputs;
  const bool counter = s.config.rules.no_counter_positions && !inputs.counter_positions.empty();
  const auto percent = s.config.rules.max_volume_percent;
  if (!counter && percent == 0) return {};
  auto request = order.request;
  request.quantity = quantity;
  const auto exposure = opening_exposure(s, request);
  if (!exposure.opening) return {};
  OpeningRuleInputs used;
  if (s.compact_opening_rule_inputs) {
    for (const auto& [symbol, opening] : exposure.contracts)
      if (const auto it = inputs.volumes.find(symbol); it != inputs.volumes.end()) used.volumes.insert(*it);
    for (const auto& other : inputs.counter_positions)
      if (exposure.dollar_delta.contains(other.underlying)) used.counter_positions.push_back(other);
  }
  event(events, "opening_rule_inputs", Json{{"order_id", order.id}, {"quantity", quantity},
      {"inputs", s.compact_opening_rule_inputs ? used : inputs}});
  if (!exposure.decision.ok() && (counter || exposure.decision.code != Reason::MISSING_VALUATION)) return exposure.decision;
  if (counter)
    if (const auto d = counter_position_check(exposure.dollar_delta, inputs.counter_positions); !d.ok()) return d;
  if (percent != 0) for (const auto& [symbol, opening] : exposure.contracts) {
    const auto volume = inputs.volumes.find(symbol);
    if (const auto d = volume_share_check(symbol, held(s, symbol), opening, percent,
        volume == inputs.volumes.end() ? std::nullopt : volume->second); !d.ok()) return d;
  }
  return {};
}
void consume_depth(Quantity& left, Quantity quantity) {
  if (__builtin_sub_overflow(left, quantity, &left))
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Liquidity budget exceeds quantity range");
}
/// The book a fill on `side` takes, before it takes it: the quote as held and
/// what is left of that side's budget.
FillQuote fill_quote(const detail::Book& book, Side side) {
  const auto& q = book.quote;
  return {q.bid, q.ask, q.bid_size, q.ask_size, side == Side::Buy ? book.ask_left : book.bid_left, q.first_time()};
}
struct ExecutionSlice {
  std::string symbol;
  Side side;
  Quantity quantity;
  Money price;
  bool given = false;  ///< An exit's long leg nobody bids for (given_away).
};
/// Impact combos execute one whole unit at a time. A ratio can span depth tiers;
/// keep those prices separate so both the ledger and the net stay exact.
std::vector<ExecutionSlice> combo_slices(const State& s, const Order& o, Quantity units) {
  std::vector<ExecutionSlice> slices;
  if (const auto prices = inside_combo(s, o)) {
    for (std::size_t i = 0; i < prices->size(); ++i) {
      const auto& leg = o.request.legs[i];
      slices.push_back({leg.symbol, leg.side, units * leg.ratio, (*prices)[i], false});
    }
    return slices;
  }
  for (const auto& leg : o.request.legs) {
    if (given_away(s, o, leg)) {
      slices.push_back({leg.symbol, leg.side, units * leg.ratio, Money{}, true});
    } else if (s.config.rules.impact_ticks == 0) {
      slices.push_back({leg.symbol, leg.side, units * leg.ratio, order_price(s, o, leg.symbol, leg.side), false});
    } else {
      for (Quantity offset = 0; offset < units * leg.ratio; ++offset) {
        const auto price = order_price(s, o, leg.symbol, leg.side, {}, offset);
        if (!slices.empty() && slices.back().symbol == leg.symbol && slices.back().price == price) ++slices.back().quantity;
        else slices.push_back({leg.symbol, leg.side, 1, price, false});
      }
    }
  }
  return slices;
}
/// All legs in an atomic execution share the account's pre-execution context.
FillContext fill_context(const State& s, const std::string& symbol, const Measures& snapshot) {
  FillContext context;
  if (const auto* v = valuation_of(s, symbol); v && s.time - v->time <= s.config.limits.max_valuation_age) {
    context.spot = v->spot;
    context.spot_source = v->spot_source;
    context.iv = v->smile_iv;
    context.delta = v->delta;
    context.years = v->years;
  } else if (const auto price = stock_price(s, s.contracts.at(symbol).underlying)) {
    context.spot = price->dollars();
    context.spot_source = "quote";
  }
  if (!context.years) context.years = static_cast<double>(std::max<Timestamp>(0, s.contracts.at(symbol).expiry_time() - s.time)) /
      static_cast<double>(md::kNanosPerDay) / 365.0;
  context.equity = marked_equity(snapshot);
  if (context.equity && s.config.rules.max_drawdown > Money{}) context.floor_room = *context.equity - s.evaluation.floor;
  context.buying_power = snapshot.buying_power.available;
  return context;
}
void on_fill(State& s, OrderId id, Quantity units, Events& events);
void annotate_opening(State& s, const OrderRequest& r, const std::string& symbol, Quantity contracts, Events& events) {
  const auto q = held(s, symbol);
  if ((q != 0 && ((q > 0) == (contracts > 0) || magnitude(contracts) <= magnitude(q))) ||
      (r.note.empty() && r.tags.empty())) return;
  auto a = clean_annotation(r.note, r.tags);
  store_annotation(s, std::to_string(s.fills.size() + 1), std::move(a), events);
}
}  // namespace
/// One trade of the trader's own, counted toward the day's trading while a rule reads
/// it: an execution of their order (a bracket exit included, the account's own closes
/// not), or a sale or buy-back of delivered shares.
void count_trade(State& s) {
  if (counts_executions(s.config.rules)) ++s.evaluation.day_executions;
  if (s.config.rules.inactivity_days > 0) s.evaluation.last_activity = s.time;
}
namespace {
void count_execution(State& s, const Order& order) {
  if (!order.system) count_trade(s);
}
}  // namespace
/// Stale marks, an invalid quote or a missing valuation hold a fill back until a
/// later batch brings the data; they say nothing about the order itself.
bool data_gap(Reason code) {
  return code == Reason::STALE_QUOTE || code == Reason::INVALID_QUOTE || code == Reason::MISSING_VALUATION || code == Reason::MARKET_HALTED;
}
namespace {
/// The queue fill model (AccountRules::queue_fills). A resting single-leg limit
/// at its own side's touch waits behind the size displayed there when it joins
/// (and the account's own earlier orders at its price). Trade prints at its price
/// consume that queue; without prints, the contract's volume increases while the
/// touch stays at the price, half of each counted on this side; without either,
/// the volume-rate heuristic: the displayed size it last saw trades once every
/// kQueueTurnover of market time. Size that leaves the displayed level without
/// trading is cancelled from ahead of it in proportion to its place in the queue.
/// What trades past the queue, and every print through its price, it may fill at
/// its limit. Fractions round at random from the seed, the attempt, the order
/// and the market time, so the same feed always gives the same fills.
constexpr Timestamp kQueueTurnover = 300 * md::kNanosPerSecond;
}  // namespace
bool queue_eligible(const State& s, const Order& o) {
  const auto& r = o.request;
  return s.config.rules.queue_fills && r.type == OrderType::Limit && r.limit_price && !multi_leg(r) && !o.system &&
         o.status != OrderStatus::Armed && !touch_stop(s, o) && !inside_single(s, o);
}
Touch touch_of(const QuoteObservation& q, Side side, Money limit) {
  const auto own = side == Side::Buy ? *q.bid : *q.ask;
  if (own == limit) return Touch::At;
  return (side == Side::Buy ? own > limit : own < limit) ? Touch::Behind : Touch::Clear;
}
namespace {
/// What the order's queue earned at this market instant. A queued fill trades at the
/// limit while the market is there, so driver 8 never fills what an earlier instant
/// earned and could not fill then (a data gap, a closed session) where the market
/// has gone since; replay drivers 1-7 kept it.
Quantity earned_now(const State& s, const QueueSpot& q) { return !s.revised_fills || q.updated == s.time ? q.earned : 0; }
/// Driver 8: an order is in no queue while it cannot trade (a GTC/GTD limit or a
/// bracket exit outside the regular session, an EXTO order between sessions). It
/// leaves the one it was in and joins again, behind what is displayed then, once
/// its session opens. Replay drivers 1-7 kept queueing, and earning, meanwhile.
bool out_of_session(State& s, OrderId id) {
  const auto& o = s.orders[id - 1];
  if (!s.revised_fills || trades_now(o, s.contracts.at(o.request.symbol), s.time)) return false;
  if (o.queue) s.orders.mut(id - 1).queue.reset();
  return true;
}
/// When the order arrived at its current price as the queue sees it: its acceptance
/// (or trigger) once the fill latency has passed, or the later change that sent it
/// to the back (see send_to_back).
Timestamp queue_arrival(const State& s, const Order& o) {
  const auto delay = s.config.rules.fill_latency_ms * (md::kNanosPerSecond / 1000);
  return std::max(std::max(o.accepted_at, o.triggered_at) + delay, o.rejoined_at);
}
/// Time priority at one price: the earlier arrival there, then the lower ID.
bool queued_before(const State& s, const Order& a, const Order& b) {
  const auto x = queue_arrival(s, a), y = queue_arrival(s, b);
  return x != y ? x < y : a.id < b.id;
}
/// The account's own orders that reached the same price earlier, still unfilled.
Quantity own_ahead(const State& s, const Order& o) {
  Quantity total = 0;
  for (const auto id : open_ids(s)) {
    const auto& other = s.orders[id - 1];
    if (id == o.id || !queued_before(s, other, o)) continue;
    if (other.queue && other.request.symbol == o.request.symbol && other.request.side == o.request.side &&
        other.queue->price == *o.request.limit_price && other.request.limit_price == o.request.limit_price)
      total += other.remaining() - earned_now(s, *other.queue);
  }
  return total;
}
__extension__ using QueueWide = __int128;
__extension__ using QueueUnsigned = unsigned __int128;
/// num / den, its fraction rounded up with that probability from the seed.
Quantity queue_round(const State& s, OrderId id, std::uint64_t purpose, QueueWide num, QueueWide den) {
  if (num <= 0 || den <= 0) return 0;
  auto whole = num / den;
  const auto part = num % den;
  if (part != 0) {
    auto seed = mix(static_cast<std::uint64_t>(s.evaluation.started) ^ mix(static_cast<std::uint64_t>(s.config.rules.queue_seed)));
    seed = mix(seed ^ id);
    seed = mix(seed ^ static_cast<std::uint64_t>(s.time));
    seed = mix(seed ^ purpose);
    if (static_cast<QueueUnsigned>(seed) % static_cast<QueueUnsigned>(den) < static_cast<QueueUnsigned>(part)) ++whole;
  }
  return static_cast<Quantity>(std::min<QueueWide>(whole, std::numeric_limits<Quantity>::max()));
}
/// A new spot for an order joining at its limit now, or none while it rests behind the touch.
std::optional<QueueSpot> join_spot(const State& s, const Order& o, bool through) {
  const auto& q = s.books.at(o.request.symbol).quote;
  const auto touch = touch_of(q, o.request.side, *o.request.limit_price);
  if (touch == Touch::Behind && !through) return std::nullopt;
  const auto shown = touch == Touch::At ? (o.request.side == Side::Buy ? q.bid_size : q.ask_size) : 0;
  const auto own = own_ahead(s, o);
  return QueueSpot{*o.request.limit_price, touch == Touch::Behind ? own : shown + own, 0, shown, q.observation,
                   s.time, s.time, std::nullopt, "joined"};
}
/// Join an order that rests now, on its submission or once its price changed.
void join_queue(State& s, OrderId id, std::optional<OrderId> incoming) {
  if (!s.orders[id - 1].open() || !queue_eligible(s, s.orders[id - 1]) || out_of_session(s, id)) return;
  const auto& o = s.orders[id - 1];
  if (!latency_ready(s, o) || !s.books.contains(o.request.symbol)) return;
  const auto& book = s.books.at(o.request.symbol);
  if ((incoming != id && book.quote.time < o.accepted_at) || !quote_check(s, o.request.symbol).ok() ||
      marketable(o, book.quote) || (o.queue && o.queue->price == *o.request.limit_price))
    return;
  auto spot = join_spot(s, o, false);
  if (spot != o.queue) s.orders.mut(id - 1).queue = std::move(spot);
}
/// One market batch's effect on an order's queue: see kQueueTurnover. `unspent`
/// is what is left of each print through a price for the orders still to see it,
/// best price and oldest order first.
void queue_step(State& s, OrderId id, const MarketActivity& activity, const std::vector<const TradePrint*>& prints,
                std::map<const TradePrint*, Quantity>& unspent) {
  if (!s.orders[id - 1].open() || !queue_eligible(s, s.orders[id - 1]) || out_of_session(s, id)) return;
  const auto& o = s.orders[id - 1];
  if (!latency_ready(s, o)) return;
  const auto& symbol = o.request.symbol;
  const auto& book = s.books.at(symbol);
  if (book.quote.time < o.accepted_at || !quote_check(s, symbol).ok() || marketable(o, book.quote)) return;
  const auto limit = *o.request.limit_price;
  const auto side = o.request.side;
  auto spot = o.queue && o.queue->price == limit ? o.queue : std::nullopt;
  // What earlier instants earned and did not fill is not this batch's (see earned_now).
  if (spot) spot->earned = earned_now(s, *spot);
  // Prints count from when it reached this price: after its latency, or its last change.
  const auto since = spot ? spot->updated : queue_arrival(s, o);
  Quantity at = 0;
  std::vector<const TradePrint*> through;
  for (const auto* print : prints) {
    if (print->time <= since || print->time > s.time) continue;
    if (print->price == limit) at += print->size;
    else if (side == Side::Buy ? print->price < limit : print->price > limit) through.push_back(print);
  }
  const auto touch = touch_of(book.quote, side, limit);
  const auto shown = touch == Touch::At ? (side == Side::Buy ? book.quote.bid_size : book.quote.ask_size) : 0;
  const auto volume = activity.volumes.find(symbol);
  if (!spot) {
    // Prints in the batch it joins in came before it rested at the touch, except
    // those through its price, which any order there would have met.
    spot = join_spot(s, o, !through.empty());
    if (!spot) return;
  } else {
    const bool shown_before = spot->displayed > 0 && touch == Touch::At;
    Quantity traded = 0;
    if (activity.prints_supplied) {
      traded = at;
      spot->source = "prints";
    } else if (activity.volumes_supplied && volume != activity.volumes.end()) {
      if (spot->volume && volume->second >= *spot->volume && shown_before)
        traded = queue_round(s, id, 1, static_cast<QueueWide>(volume->second) - *spot->volume, 2);
      spot->source = "volume";
    } else {
      if (shown_before) {
        const auto elapsed = std::min(s.time - spot->updated, s.config.limits.max_quote_age);
        traded = queue_round(s, id, 2, static_cast<QueueWide>(spot->displayed) * std::max<Timestamp>(elapsed, 0), kQueueTurnover);
      }
      spot->source = "rate";
    }
    const auto taken = std::min(spot->ahead, traded);
    spot->ahead -= taken;
    spot->earned = static_cast<Quantity>(std::min<QueueWide>(static_cast<QueueWide>(spot->earned) + traded - taken, o.remaining()));
    // Size that left the level without trading: cancelled ahead of the order in
    // proportion to its place in the queue.
    if (shown_before && book.quote.observation != spot->observation) {
      const auto left = static_cast<QueueWide>(spot->displayed) - shown - traded;
      if (left > 0 && spot->ahead > 0)
        spot->ahead -= std::min(spot->ahead, queue_round(s, id, 3, left * spot->ahead, spot->displayed));
    }
    // Nothing displayed is ahead once the level clears, and never more than is displayed.
    const auto own = own_ahead(s, o);
    if (touch == Touch::Clear) spot->ahead = std::min(spot->ahead, own);
    if (touch == Touch::At) spot->ahead = std::min(spot->ahead, shown + own);
  }
  for (const auto* print : through) {
    auto& left = unspent.try_emplace(print, print->size).first->second;
    const auto give = std::min(left, o.remaining() - spot->earned);
    spot->earned += give;
    left -= give;
  }
  spot->displayed = shown;
  spot->observation = book.quote.observation;
  spot->updated = s.time;
  spot->volume = activity.volumes_supplied && volume != activity.volumes.end() ? std::optional(volume->second) : std::nullopt;
  if (spot != o.queue) s.orders.mut(id - 1).queue = std::move(spot);
}
}  // namespace
/// Advance every resting order's queue for a market batch, in price/time priority,
/// and add the contracts where one may now fill to `changed`.
void update_queues(State& s, const MarketActivity& activity, std::set<std::string>& changed) {
  std::map<std::string, std::vector<const TradePrint*>> prints;
  for (const auto& print : activity.prints)
    if (print.size > 0 && print.price > Money{}) prints[print.symbol].push_back(&print);
  std::vector<OrderId> ids;
  for (const auto id : open_ids(s))
    if (queue_eligible(s, s.orders[id - 1]) && s.books.contains(s.orders[id - 1].request.symbol)) ids.push_back(id);
  std::stable_sort(ids.begin(), ids.end(), [&](OrderId a, OrderId b) {
    const auto& x = s.orders[a - 1].request;
    const auto& y = s.orders[b - 1].request;
    if (x.symbol != y.symbol) return x.symbol < y.symbol;
    if (x.side != y.side) return x.side == Side::Buy;
    if (x.limit_price == y.limit_price) return queued_before(s, s.orders[a - 1], s.orders[b - 1]);
    return x.side == Side::Buy ? *x.limit_price > *y.limit_price : *x.limit_price < *y.limit_price;
  });
  std::map<const TradePrint*, Quantity> unspent;
  static const std::vector<const TradePrint*> none;
  for (const auto id : ids) {
    const auto& symbol = s.orders[id - 1].request.symbol;
    const auto found = prints.find(symbol);
    queue_step(s, id, activity, found == prints.end() ? none : found->second, unspent);
    const auto& o = s.orders[id - 1];
    if (o.queue && o.queue->earned > 0) changed.insert(symbol);
  }
}
namespace {
void match_one(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  if (s.config.rules.queue_fills) join_queue(s, id, incoming);
  // Read here; every write goes through mut() below, after which `o` is not read.
  const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open() || o.status == OrderStatus::Armed || !latency_ready(s, o)) return;
  if (incoming != id && s.books.at(o.request.symbol).quote.time < o.accepted_at) return;
  // Good-until-expiry exits and triggered orders outlive a session; outside
  // their eligible sessions they wait for the next one.
  if (!trades_now(o, s.contracts.at(o.request.symbol), s.time)) return;
  // A close kept within its position may meet a quote with only an ask: it buys at
  // that ask, or a reduce-only market close gives the long away at zero.
  const bool one_sided = ask_only_single(s, o);
  const bool given = one_sided && o.request.side == Side::Sell;
  if (!one_sided && !quote_check(s, o.request.symbol).ok()) return;
  const bool inside = inside_single(s, o);
  // A resting limit the queue model lets fill: it trades at its limit, on its own side.
  const bool queued = !given && !inside && queue_eligible(s, o) && !marketable(o, s.books.at(o.request.symbol).quote) &&
                      o.queue && o.queue->price == *o.request.limit_price && earned_now(s, *o.queue) > 0;
  if (!given && !inside && !queued && !marketable(o, s.books.at(o.request.symbol).quote)) return;
  // Stop stress worsens the price a stop-limit fills at, never past its limit, so
  // the next block's unstressed price decides whether it may fill.
  if (s.config.rules.impact_ticks > 0 && o.request.limit_price && !given && !inside && !queued) {
    const auto candidate = execution_price(s, o.request.symbol, o.request.side);
    if (o.request.side == Side::Buy ? candidate > *o.request.limit_price : candidate < *o.request.limit_price) return;
  }
  // System orders, bracket exits and reduce-only closes only ever reduce a position
  // (they are kept within it), so they skip the price band and loss projection.
  const bool reducing = o.system || kept_within(o);
  auto decision = reducing ? system_check(s, o) : order_check(s, o, Stage::Fill);
  if (data_gap(decision.code)) return;
  if (!decision.ok()) {
    if (decision.code != Reason::SCALING_LIMIT && decision.code != Reason::COUNTER_POSITION && decision.code != Reason::MAX_VOLUME_SHARE) {
      decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
      decision.code = Reason::RISK_CHANGED;
    }
    cancel_order(s, id, decision, events);
    return;
  }
  const auto symbol = o.request.symbol;
  const auto side = o.request.side;
  const auto& book = s.books.at(symbol);
  if (!given && !inside && !queued && !marketable(o, book.quote)) return;
  const auto stress = given || inside || queued || touch_stop(s, o) ? 0 : o.stress_ticks;
  Money price = given ? Money{} : queued ? *o.request.limit_price : order_price(s, o, symbol, side, o.request.limit_price);
  if (stress != 0 && o.request.limit_price)
    price = side == Side::Buy ? std::min(price, *o.request.limit_price) : std::max(price, *o.request.limit_price);
  const auto remaining_depth = side == Side::Buy ? book.ask_left : book.bid_left;
  const auto size = side == Side::Buy ? book.quote.ask_size : book.quote.bid_size;
  const auto position = held(s, o.request.symbol);
  const auto capacity = o.system && s.evaluation.status != EvaluationStatus::Active
      ? closing_capacity(s, o.request.symbol, o.request.side)
      : o.request.side == Side::Sell ? std::max<Quantity>(position, 0) : std::max<Quantity>(-position, 0);
  // A long given away needs no bid, so no displayed size limits it.
  const auto budget = given ? capacity : queued ? o.queue->earned
      : s.config.rules.impact_ticks > 0 && !touch_stop(s, o) && !inside ? size - depth_used(size, remaining_depth) % size : remaining_depth;
  const Quantity quantity = reducing ? std::min({o.remaining(), budget, capacity}) : std::min(o.remaining(), budget);
  if (quantity <= 0) return;
  decision = reducing ? Decision{} : opening_fill_check(s, o, quantity, events);
  if (data_gap(decision.code)) return;
  if (decision.ok() && !reducing)
    decision = price_check(s, book.quote, stress == 0 || stress_in_band(s) ? price
                                              : order_price(s, o, symbol, side, o.request.limit_price, 0, false));
  const auto fees = fees_for(s, symbol, quantity, std::min(quantity, capacity), commission_paid(s, o, symbol));
  const Money fee = fees.total();
  if (decision.ok() && !reducing) {
    // Check the proposed accounting before committing any liquidity or fill.
    const Quantity signed_quantity = side == Side::Buy ? quantity : -quantity;
    State projected = s;
    projected.ledger.fill(s.contracts.at(symbol), signed_quantity, price, fee);
    if (s.config.rules.fees) projected.projected_commission[{id, symbol}] = fees.commission;
    projected.orders.mut(static_cast<std::size_t>(id - 1)).filled_quantity += quantity;
    if (!closing_only(s, o)) decision = loss_check(projected, measure(projected));
    // A fill that reduces free buying power must leave it nonnegative, or leave
    // more available than cancelling the order, as its acceptance allowed.
    if (decision.ok() && s.config.rules.buying_power && free_power(projected) < free_power(s)) {
      const auto power = buying_power(projected).total;
      if (power.available < Money{} && !frees_power(s, o, projected))
        decision = {Reason::BUYING_POWER, "Fill needs more buying power than the account has available",
                    (-power.available).dollars(), 0.0, o.request.symbol};
    }
  }
  if (!decision.ok()) {
    if (decision.code != Reason::SCALING_LIMIT && decision.code != Reason::COUNTER_POSITION && decision.code != Reason::MAX_VOLUME_SHARE) {
      decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
      decision.code = Reason::RISK_CHANGED;
    }
    cancel_order(s, id, decision, events);
    return;
  }
  const auto context = fill_context(s, symbol, measure(s));
  annotate_opening(s, o.request, symbol, side == Side::Buy ? quantity : -quantity, events);
  const auto realised_before = s.ledger.account().realised;
  const bool opening = quantity > capacity;
  const auto quote = book.quote;
  const auto taken = fill_quote(book, side);
  fill_position(s, symbol, side == Side::Buy ? quantity : -quantity, price, fee);
  // A queued fill trades with the other side's flow at its own price, not the far side's displayed size.
  if (!given && !queued) {
    auto& left = s.books[symbol];
    consume_depth(side == Side::Buy ? left.ask_left : left.bid_left, quantity);
  }
  auto& order = s.orders.mut(id - 1);
  if (queued) order.queue->earned -= quantity;
  order.filled_quantity += quantity;
  order.filled_notional = order.filled_notional + price * quantity;
  order.status = order.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  if (order.status == OrderStatus::Filled) order.ended_at = s.time;
  Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, symbol, side,
            quantity, price, fee, quote.observation, quote.time, s.time, context, order.actor, taken,
            s.config.rules.fees ? std::optional(fees) : std::nullopt, quote.feed, stress, queued};
  s.fills.push_back(fill);
  event(events, "fill", fill);
  count_execution(s, order);
  on_fill(s, id, quantity, events);
  guardrail_fill(s, id, opening, realised_before, events);
  observe_equity(s, events);
}
/// Later slices of a leg see the position and commission its earlier slices leave.
std::vector<FillFees> combo_fees(const State& s, const Order& o, const std::vector<ExecutionSlice>& slices) {
  std::map<std::string, std::pair<Quantity, Money>> legs;
  std::vector<FillFees> out;
  for (const auto& slice : slices) {
    auto& [position, paid] = legs.try_emplace(slice.symbol, held(s, slice.symbol), commission_paid(s, o, slice.symbol)).first->second;
    out.push_back(fees_for(s, slice.symbol, slice.quantity, closing_contracts(position, slice.side, slice.quantity), paid));
    paid = paid + out.back().commission;
    position += slice.side == Side::Buy ? slice.quantity : -slice.quantity;
  }
  return out;
}
/// A multi-leg order fills all its legs together, in ratio, at each leg's slipped
/// far side when the net debit is at or below its limit; units are bounded by every
/// leg's remaining displayed size. The whole projected fill is rechecked first.
void match_combo(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  // Read here; every write goes through mut() below, after which `o` is not read.
  const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open() || o.status == OrderStatus::Armed || !latency_ready(s, o)) return;
  for (const auto& leg : o.request.legs) {
    if (!trades_now(o, s.contracts.at(leg.symbol), s.time)) return;
    if (!quote_check(s, leg.symbol).ok() && !ask_only(s, o, leg)) return;
    if (incoming != id && s.books.at(leg.symbol).quote.time < o.accepted_at) return;
  }
  const auto net = executable_net(s, o);
  if (!net || (o.request.limit_price && *net > *o.request.limit_price)) return;
  const bool exit = kept_within(o);
  auto decision = exit ? system_check(s, o) : order_check(s, o, Stage::Fill);
  if (data_gap(decision.code)) return;
  const bool inside = inside_combo(s, o).has_value();
  Quantity units = s.config.rules.impact_ticks > 0 && !touch_stop(s, o) && !inside ? 1 : o.remaining();
  if (s.config.rules.impact_ticks == 0 || touch_stop(s, o) || inside) for (const auto& leg : o.request.legs) {
    if (given_away(s, o, leg)) continue;
    const auto& book = s.books.at(leg.symbol);
    units = std::min(units, (leg.side == Side::Buy ? book.ask_left : book.bid_left) / leg.ratio);
  }
  if (decision.ok() && units <= 0) return;
  if (decision.ok() && !exit) decision = opening_fill_check(s, o, units, events);
  if (data_gap(decision.code)) return;
  const auto slices = combo_slices(s, o, units);
  const auto slice_fees = combo_fees(s, o, slices);
  if (decision.ok() && !exit) {
    State projected = s;
    for (std::size_t i = 0; i < slices.size(); ++i) {
      const auto& slice = slices[i];
      const auto contracts = slice.side == Side::Buy ? slice.quantity : -slice.quantity;
      projected.ledger.fill(s.contracts.at(slice.symbol), contracts, slice.price, slice_fees[i].total());
      if (s.config.rules.fees) {
        auto& paid = projected.projected_commission[{id, slice.symbol}];
        paid = paid + slice_fees[i].commission;
      }
    }
    projected.orders.mut(static_cast<std::size_t>(id - 1)).filled_quantity += units;
    if (!closing_only(s, o)) decision = loss_check(projected, measure(projected));
    if (decision.ok() && s.config.rules.buying_power && free_power(projected) < free_power(s)) {
      const auto power = buying_power(projected).total;
      if (power.available < Money{} && !frees_power(s, o, projected))
        decision = {Reason::BUYING_POWER, "Fill needs more buying power than the account has available",
                    (-power.available).dollars(), 0.0, s.contracts.at(o.request.legs.front().symbol).underlying};
    }
  }
  if (!decision.ok()) {
    if (decision.code != Reason::SCALING_LIMIT && decision.code != Reason::COUNTER_POSITION && decision.code != Reason::MAX_VOLUME_SHARE) {
      decision.message = std::string(to_string(decision.code)) + ": " + decision.message;
      decision.code = Reason::RISK_CHANGED;
    }
    cancel_order(s, id, decision, events);
    return;
  }
  const auto request = o.request;
  const auto actor = o.actor;
  const auto stress = touch_stop(s, o) ? 0 : o.stress_ticks;
  const auto before = measure(s);
  const auto realised_before = s.ledger.account().realised;
  const bool opening = std::any_of(request.legs.begin(), request.legs.end(), [&](const Leg& leg) {
    const auto position = held(s, leg.symbol);
    return units * leg.ratio > (leg.side == Side::Buy ? std::max<Quantity>(0, -position) : std::max<Quantity>(0, position));
  });
  for (std::size_t i = 0; i < slices.size(); ++i) {
    const auto& slice = slices[i];
    const auto context = fill_context(s, slice.symbol, before);
    const auto quote = s.books.at(slice.symbol).quote;
    const auto taken = fill_quote(s.books.at(slice.symbol), slice.side);
    const auto contracts = slice.side == Side::Buy ? slice.quantity : -slice.quantity;
    const auto size = magnitude(contracts);
    const Money price = slice.price;
    const Money fee = slice_fees[i].total();
    annotate_opening(s, request, slice.symbol, contracts, events);
    fill_position(s, slice.symbol, contracts, price, fee);
    auto& book = s.books[slice.symbol];
    if (!slice.given) consume_depth(slice.side == Side::Buy ? book.ask_left : book.bid_left, size);
    Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, slice.symbol, slice.side,
              size, price, fee, quote.observation, quote.time, s.time, context, actor, taken,
              s.config.rules.fees ? std::optional(slice_fees[i]) : std::nullopt, quote.feed, slice.given || inside ? 0 : stress};
    s.fills.push_back(fill);
    event(events, "fill", fill);
  }
  auto& order = s.orders.mut(id - 1);
  order.filled_quantity += units;
  order.filled_notional = order.filled_notional + *net * units;
  order.status = order.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  if (order.status == OrderStatus::Filled) order.ended_at = s.time;
  count_execution(s, order);
  on_fill(s, id, units, events);
  guardrail_fill(s, id, opening, realised_before, events);
  observe_equity(s, events);
}
}  // namespace
/// Why an IOC's remainder found nothing more to fill: no fresh two-sided quote, a
/// limit short of the executable price, or else the liquidity it could use was used
/// up (`exhausted`).
std::string ioc_remainder_message(const State& s, const Order& o, std::string exhausted) {
  const auto& r = o.request;
  if (multi_leg(r) || !s.books.contains(r.symbol)) return exhausted;
  if (!quote_check(s, r.symbol).ok()) return "IOC found no fresh two-sided quote to fill against";
  if (r.limit_price) {
    const bool buy = r.side == Side::Buy;
    const auto price = execution_price(s, r.symbol, r.side);
    const bool slipped = s.config.rules.slippage_ticks > 0 || s.config.rules.impact_ticks > 0;
    if (buy ? *r.limit_price < price : *r.limit_price > price)
      return "IOC limit " + r.limit_price->str() + " did not reach the " + (buy ? "ask" : "bid") +
             (slipped ? " after slippage, " : ", ") + price.str() + "; nothing more was marketable";
  }
  return exhausted;
}
/// An IOC's unfilled remainder cancels, except a stop exit's: a thin book, or one
/// other orders have used up, re-arms it for what it still protects, and it fires
/// again on the next quote that reaches its level, so no remainder is left bare.
void end_ioc(State& s, OrderId id, std::string message, Events& events) {
  const auto& o = s.orders.at(static_cast<std::size_t>(id - 1));
  if (!o.open() || !immediate(o.request)) return;
  if (o.role != OrderRole::Normal && o.request.trigger) {
    auto& stop = s.orders.mut(id - 1);
    stop.status = OrderStatus::Armed;
    event(events, "order_rearmed", stop);
    return;
  }
  cancel_order(s, id, failure(Reason::IOC_REMAINDER, ioc_remainder_message(s, o, std::move(message))), events);
}
/// A flatten limit follows fresh executable touches, each leg rounded outwards to
/// its valid price tick. Missing books leave it working for a later quote.
std::optional<Money> close_price(const State& s, const Order& o) {
  Money net;
  auto legs = o.request.legs;
  if (legs.empty()) legs.push_back({o.request.symbol, o.request.side, 1});
  for (const auto& leg : legs) {
    if (given_away(s, o, leg)) continue;
    if (!quote_check(s, leg.symbol).ok() && !(leg.side == Side::Buy && asks_only(s, leg.symbol))) return {};
    const auto& q = s.books.at(leg.symbol).quote;
    if (q.time < o.accepted_at) return {};
    const bool buy = leg.side == Side::Buy;
    const auto touch = buy ? *q.ask : *q.bid;
    const auto& root = s.contracts.at(leg.symbol).root;
    auto price = buy ? touch + tick_size(root, touch) * *o.limit_ticks
                    : std::max(tick_size(root, Money{}), touch - tick_size(root, touch) * *o.limit_ticks);
    // Crossing a tier boundary can change the valid tick, so round once more there.
    for (int pass = 0; pass < 2; ++pass) {
      const auto tick = tick_size(root, price).micros();
      const auto remainder = price.micros() % tick;
      if (remainder != 0) price = price + Money::from_micros(buy ? tick - remainder : -remainder);
    }
    net = net + (buy ? price : -price) * leg.ratio;
  }
  return multi_leg(o.request) || o.request.side == Side::Buy ? net : -net;
}
namespace {
bool reprice_close(State& s, OrderId id, Events& events) {
  const auto& o = s.orders[id - 1];
  if (!o.limit_ticks) return true;
  for (const auto& symbol : order_symbols(o.request))
    if (!trades_now(o, s.contracts.at(symbol), s.time)) return false;
  const auto price = close_price(s, o);
  if (!price) return false;
  if (o.request.limit_price != price) {
    auto& changed = s.orders.mut(id - 1);
    if (!changed.submitted) changed.submitted = changed.request;
    changed.request.limit_price = price;
    send_to_back(s, changed, s.time);
    event(events, "order_repriced", changed);
  }
  return true;
}
}  // namespace
/// Sweep successive simulated tiers, retaining the original one-fill path when
/// impact is off. A delayed IOC gets its one attempt on an eligible quote.
void match_order(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
  const auto& initial = s.orders[id - 1];
  if (!initial.open() || initial.status == OrderStatus::Armed || !latency_ready(s, initial)) return;
  const bool combo = multi_leg(initial.request);
  if (!reprice_close(s, id, events)) return;
  for (;;) {
    const auto before = s.orders[id - 1].filled_quantity;
    if (combo) match_combo(s, id, events, incoming); else match_one(s, id, events, incoming);
    const auto& current = s.orders[id - 1];
    if (s.config.rules.impact_ticks == 0 || !current.open() || current.filled_quantity == before) break;
  }
  // Stop stress belongs to the fast market its trigger met: a stop-limit still
  // resting after that first attempt fills later like any other limit.
  if (const auto& o = s.orders[id - 1]; o.open() && o.request.type == OrderType::Limit && o.stress_ticks != 0)
    s.orders.mut(id - 1).stress_ticks = 0;
  if (s.config.rules.fill_latency_ms > 0) end_ioc(s, id, "IOC exhausted eligible liquidity", events);
}
void match_symbols(State& s, const std::set<std::string>& symbols, Events& events,
                   std::optional<OrderId> incoming) {
  for (const auto& symbol : symbols) {
    for (const auto side : {Side::Buy, Side::Sell}) {
      std::vector<OrderId> priority;
      for (const auto id : open_ids(s)) {
        const auto& o = s.orders[id - 1];
        if (o.status != OrderStatus::Armed && o.request.symbol == symbol && o.request.side == side) priority.push_back(id);
      }
      std::stable_sort(priority.begin(), priority.end(), [&](OrderId a, OrderId b) {
        const auto& x = s.orders.at(static_cast<std::size_t>(a - 1));
        const auto& y = s.orders.at(static_cast<std::size_t>(b - 1));
        if (x.request.type != y.request.type) return x.request.type == OrderType::Market;
        if (x.request.limit_price == y.request.limit_price) return a < b;
        return side == Side::Buy ? x.request.limit_price > y.request.limit_price : x.request.limit_price < y.request.limit_price;
      });
      for (auto id : priority) match_order(s, id, events, incoming);
    }
  }
  // Multi-leg orders then take the displayed liquidity left, in acceptance order.
  std::vector<OrderId> combos;
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (multi_leg(o.request) &&
        std::any_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) { return symbols.contains(leg.symbol); }))
      combos.push_back(id);
  }
  for (const auto id : combos) match_order(s, id, events, incoming);
}
/// The units an exit's closing legs can still close: the least the position holds
/// against any leg, in its ratio.
Quantity exit_capacity(const State& s, const OrderRequest& r) {
  auto capacity = std::numeric_limits<Quantity>::max();
  const auto cap = [&](const std::string& leg_symbol, Side side, Quantity ratio) {
    const auto q = held(s, leg_symbol);
    capacity = std::min(capacity, (side == Side::Sell ? std::max<Quantity>(q, 0) : std::max<Quantity>(-q, 0)) / ratio);
  };
  if (multi_leg(r)) for (const auto& leg : r.legs) cap(leg.symbol, leg.side, leg.ratio);
  else cap(r.symbol, r.side, 1);
  return capacity;
}
namespace {
void shrink_exit(State& s, OrderId id, Quantity capacity, Events& events) {
  if (s.orders[id - 1].remaining() <= capacity) return;
  auto& exit = s.orders.mut(id - 1);
  // A held spread's exits were submitted; a retry still finds their original terms.
  if (exit.request.exits_only && !exit.submitted) exit.submitted = exit.request;
  exit.request.quantity = exit.filled_quantity + capacity;
  event(events, "order_resized", exit);
}
}  // namespace
/// Keep bracket exits within the position they protect: shrink them when it
/// shrinks and cancel them once it is closed, so an exit can never open one.
void sync_exits(State& s, const std::string& symbol, Events& events) {
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (!o.open() || !kept_within(o) || !touches(o.request, symbol)) continue;
    const auto capacity = exit_capacity(s, o.request);
    if (capacity == 0) {
      cancel_order(s, id, failure(Reason::POSITION_CLOSED, "The position this exit protected is closed"), events);
    } else shrink_exit(s, id, capacity, events);
  }
}
/// Create a bracket's exits on the entry's first fill and grow them with later
/// fills. A stop is armed until reached, then trades at market (IOC); a limit
/// take-profit rests (GTC); a stop-limit is armed until reached, then rests as a
/// GTC limit. All are good until expiry and sized to the filled quantity, and one
/// filling completely cancels the other.
void attach_exits(State& s, OrderId entry_id, Events& events) {
  const auto entry = s.orders.at(static_cast<std::size_t>(entry_id - 1));  // pushes below invalidate references
  const auto& bracket = *entry.request.bracket;
  const auto expiry = order_expiry(s, entry.request);
  auto make = [&](const ExitSpec& spec, OrderRole role) {
    Order exit;
    exit.actor = entry.actor;
    exit.id = static_cast<OrderId>(s.orders.size() + 1);
    const auto tif = extended(entry.request) ? TimeInForce::GtcExto : spec.limit_price ? TimeInForce::Gtc : TimeInForce::Ioc;
    exit.request = {entry.request.client_order_id + (role == OrderRole::StopLoss ? ":stop" : ":target"),
                    entry.request.symbol, entry.request.side == Side::Buy ? Side::Sell : Side::Buy,
                    spec.limit_price ? OrderType::Limit : OrderType::Market, tif,
                    entry.filled_quantity, spec.limit_price, spec.trigger, {}, {}};
    if (multi_leg(entry.request)) {
      exit.request.side = Side::Buy;
      exit.request.legs = entry.request.legs;
      for (auto& leg : exit.request.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
    }
    exit.role = role;
    exit.parent = entry.id;
    exit.status = spec.trigger ? OrderStatus::Armed : OrderStatus::Working;
    exit.accepted_at = s.time;
    exit.day_end = expiry;
    add_order(s, exit);
    event(events, "order_accepted", exit);
    return exit.id;
  };
  auto grow = [&](OrderId id) {
    const auto& exit = s.orders.at(static_cast<std::size_t>(id - 1));
    if (exit.open() && exit.filled_quantity == 0 && exit.request.quantity < entry.filled_quantity) {
      auto& resized = s.orders.mut(id - 1);
      resized.request.quantity = entry.filled_quantity;
      send_to_back(s, resized, s.time);
      event(events, "order_resized", resized);
    }
  };
  auto stop = entry.stop_loss, target = entry.take_profit;
  const bool created = (bracket.stop_loss && stop == 0) || (bracket.take_profit && target == 0);
  if (bracket.stop_loss) { if (stop == 0) stop = make(*bracket.stop_loss, OrderRole::StopLoss); else grow(stop); }
  if (bracket.take_profit) { if (target == 0) target = make(*bracket.take_profit, OrderRole::TakeProfit); else grow(target); }
  auto& stored = s.orders.mut(entry_id - 1);
  stored.stop_loss = stop;
  stored.take_profit = target;
  if (created && stop != 0 && target != 0) {
    s.orders.mut(stop - 1).oco = target;
    s.orders.mut(target - 1).oco = stop;
  }
}
namespace {
void on_fill(State& s, OrderId id, Quantity units, Events& events) {
  detail::update_reviews(s);
  const auto order = s.orders.at(static_cast<std::size_t>(id - 1));
  // One exit filling completely cancels the other; a partial fill leaves it to
  // protect what is still held, shrunk to that by sync_exits below, as does the
  // complete fill of an exit the trader made smaller than the other.
  // Compare this fill with the sibling's remainder: either may have filled before.
  if (order.oco != 0) {
    const auto& sibling = s.orders.at(static_cast<std::size_t>(order.oco - 1));
    if (sibling.open()) {
      // Two orders that cancel each other do so on the first fill of either.
      if (order.role == OrderRole::Normal)
        cancel_order(s, order.oco, failure(Reason::OCO_FILLED, "The other order of this pair filled"), events);
      else if (order.status == OrderStatus::Filled && units >= sibling.remaining())
        cancel_order(s, order.oco, failure(Reason::OCO_FILLED, "The other exit of this bracket filled"), events);
      else
        // This bracket's remaining protection, even when another entry holds the same contract.
        shrink_exit(s, order.oco, std::max(order.remaining(), sibling.remaining() - units), events);
    }
  }
  if (order.role != OrderRole::Normal && order.parent != 0 && s.orders.at(static_cast<std::size_t>(order.parent - 1)).open())
    cancel_order(s, order.parent, failure(Reason::OCO_FILLED, "An exit filled; the remaining entry is cancelled"), events);
  if (order.request.bracket && !order.request.exits_only) attach_exits(s, id, events);
  for (const auto& symbol : order_symbols(s.orders.at(static_cast<std::size_t>(id - 1)).request)) sync_exits(s, symbol, events);
}
}  // namespace
}  // namespace session_internal
}  // namespace openport::trading
