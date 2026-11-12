// Working share orders: resting limits, stops and stop-limits on stocks and ETFs,
// with extended hours, brackets and OCO. They reuse the share and account checks
// in session_internal.hpp.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
namespace {

/// Shares whose open loss is at the plan's open-loss cap can be reduced but not added to.
Decision share_open_loss_check(const State& s, const std::string& symbol, Quantity held, Quantity signed_shares) {
  const auto& cap = s.config.rules.open_loss_cap;
  if (!cap || cap->action == OpenLossAction::Fail || held == 0 || (held > 0) != (signed_shares > 0) ||
      std::find(s.evaluation.open_loss_symbols.begin(), s.evaluation.open_loss_symbols.end(), symbol) == s.evaluation.open_loss_symbols.end())
    return {};
  return {Reason::OPEN_LOSS_CAP, "These shares' open loss is at the plan's open-loss cap; they can be reduced, "
          "but not added to, until they are back within the cap", {}, {}, symbol};
}
}  // namespace

/// The checks a share trade takes before the market's: the account type, the minimum
/// hold, the account and plan entry checks, buy-only and defined risk.
Decision share_entry_checks(const State& s, const std::string& symbol, Quantity signed_shares, const ShareTerms& terms) {
  if (const auto d = account_type_check(s, {}, {{symbol, signed_shares}}); !d.ok()) return d;
  const auto shares = shares_held(s, symbol);
  if (!terms.protective && s.config.rules.min_hold_seconds > 0 && shares != 0 && (shares > 0) != (signed_shares > 0)) {
    for (const auto& life : share_lifecycles(s.stock_fills, s.dividends))
      if (life.symbol == symbol && !life.closed)
        if (const auto d = hold_age_check(s, life.opened, symbol); !d.ok()) return d;
  }
  const bool reduces = shares != 0 && (shares > 0) != (signed_shares > 0) &&
                       (shares > 0 ? -signed_shares <= shares : signed_shares <= -shares);
  if (terms.reduce_only && !reduces)
    return failure(Reason::INVALID_ORDER, terms.protective ? "A bracket exit only reduces the shares it protects"
                                                           : "Stock trades only reduce the shares exercise and assignment delivered");
  if (const auto d = account_check(s, reduces); !d.ok()) return d;
  if (const auto d = share_open_loss_check(s, symbol, shares, signed_shares); !d.ok()) return d;
  if (reduces) return {};
  if (const auto d = plan_entry_check(s.config.rules, symbol, s.time); !d.ok()) return d;
  if (md::is_index_underlying(symbol))
    return failure(Reason::INVALID_ORDER, "Index underlyings settle in cash and have no shares to trade");
  if (signed_shares < 0 && s.config.rules.buy_only)
    return failure(Reason::BUY_ONLY, "This plan is buy-only: share sales may only reduce long shares");
  if (signed_shares < 0 && s.config.rules.defined_risk)
    return failure(Reason::DEFINED_RISK, "This plan allows defined risk only: short shares can lose without limit");
  return {};
}

/// The opening rule inputs an option fill takes (opening_fill_check), for shares that
/// open: other accounts' held and working dollar delta, as the server supplies it for
/// the transaction (none in a replay, for a sandbox or with no other account). A
/// resting share order checks it when it fills, as an option order does.
Decision opening_share_check(const State& s, const std::string& symbol, Quantity signed_shares, Money price,
                             const ShareTerms& terms, Events& events) {
  if (!s.opening_rule_inputs || !s.config.rules.no_counter_positions || s.opening_rule_inputs->counter_positions.empty())
    return {};
  const auto& inputs = *s.opening_rule_inputs;
  OpeningRuleInputs used;
  if (s.compact_opening_rule_inputs)
    for (const auto& other : inputs.counter_positions)
      if (other.underlying == symbol) used.counter_positions.push_back(other);
  Json recorded{{"symbol", symbol}, {"shares", signed_shares}, {"inputs", s.compact_opening_rule_inputs ? used : inputs}};
  if (terms.order) recorded["stock_order"] = terms.order;
  event(events, "opening_rule_inputs", std::move(recorded));
  return counter_position_check({{symbol, static_cast<double>(signed_shares) * price.dollars()}}, inputs.counter_positions);
}

/// Whether `signed_shares` only reduce the shares held, which opening rules let through.
bool reduces_shares(const State& s, const std::string& symbol, Quantity signed_shares) {
  const auto shares = shares_held(s, symbol);
  return shares != 0 && (shares > 0) != (signed_shares > 0) &&
         (shares > 0 ? -signed_shares <= shares : signed_shares <= -shares);
}

/// Trade shares at `price` after the entry checks and those an opening trade takes at
/// its price: the stop requirement, hedging, complete marks, the loss and trade-risk
/// limits and exposure, and then buying power. The caller has checked the session and
/// that the price is fresh.
Decision trade_shares_checked(State& s, const std::string& symbol, Quantity signed_shares, Money price,
                              const ShareTerms& terms, Events& events) {
  if (const auto d = share_entry_checks(s, symbol, signed_shares, terms); !d.ok()) return d;
  const bool reduces = reduces_shares(s, symbol, signed_shares);
  if (!reduces) {
    const auto d = opening_share_rules_check(s, symbol, signed_shares, price, terms, events, [&](State& projected) {
      trade_shares(projected, symbol, signed_shares, price, StockSource::Trade);
    });
    if (!d.ok()) return d;
  }
  // Shares can cover options: selling the ones a short call is written against
  // leaves it naked, which may need more than the sale frees.
  if (s.config.rules.buying_power) {
    State projected = s;
    trade_shares(projected, symbol, signed_shares, price, StockSource::Trade);
    if (free_power(projected) < free_power(s) && buying_power(projected).total.available < Money{}) {
      if (!reduces)
        return {Reason::BUYING_POWER, "These shares need more buying power than the account has available",
                (-buying_power(projected).total.available).dollars(), 0.0, symbol};
      return {Reason::BUYING_POWER, "These shares cover a short option that would need more buying power "
              "than the account has available; buy the option back first", std::nullopt, std::nullopt, symbol};
    }
  }
  const auto realised_before = s.ledger.account().realised;
  trade_shares(s, symbol, signed_shares, price, StockSource::Trade);
  if (terms.order) s.stock_fills.mut(s.stock_fills.size() - 1).stock_order = terms.order;
  count_trade(s);
  if (!reduces) {
    ++s.guardrails.opening_trades;
    const auto limit = s.config.guardrails.max_opening_trades;
    if (limit > 0 && s.guardrails.opening_trades >= limit) latch_guardrail(s, Reason::TRADE_LIMIT, events);
  }
  if (s.config.guardrails.cooldown_loss > Money{} && s.ledger.account().realised - realised_before < -s.config.guardrails.cooldown_loss)
    begin_cooldown(s, events);
  if (reduces) count_close(s, s.ledger.account().realised - realised_before, events);
  Json traded{{"symbol", symbol}, {"shares", signed_shares}, {"price", price}};
  if (terms.order) traded["stock_order"] = terms.order;
  event(events, "stock_trade", traded);
  return {};
}
namespace {

Quantity signed_shares_of(const StockOrderRequest& r) { return r.side == Side::Buy ? r.shares : -r.shares; }
bool stops(const StockOrderRequest& r) { return r.type == StockOrderType::Stop || r.type == StockOrderType::StopLimit; }

/// When a share order accepted at `time` stops working unfilled: GTC never; DAY at the
/// end of its date's regular session, or of its after-hours session with extended hours.
/// An order accepted after that, or on a closed day, works the next business day's.
Timestamp stock_day_end(const StockOrderRequest& r, Timestamp time) {
  if (r.tif == TimeInForce::Gtc) return std::numeric_limits<Timestamp>::max();
  auto date = md::trading_date(time, 24 * 60);
  for (int step = 0; step < 16; ++step) {
    const int close = md::regular_close_hour(date) + (r.extended_hours ? 4 : 0);
    const auto end = md::new_york_to_utc(date, close, 0);
    if (time < end) return end;
    date = md::trading_date(md::new_york_to_utc(date, 23, 59) + 2 * md::kNanosPerMinute, 24 * 60);
  }
  return std::numeric_limits<Timestamp>::max();
}

/// Whether the stock market lets this order execute now: market and stop orders, and
/// limits without extended hours, in the regular session; extended-hours limits also
/// premarket and after hours.
bool stock_order_session(const StockOrder& o, Timestamp time) {
  return o.request.extended_hours ? md::stock_extended_session(time).open : md::stock_session(time).open;
}

/// Whether a print stamped `time` is one of the sessions this order works in: printed
/// while one is open, or the close a session prints at its end. A stop's level is
/// reached only on such a print, not on an after-hours or premarket one.
bool stock_order_print(const StockOrder& o, Timestamp time) {
  const auto session = o.request.extended_hours ? md::stock_extended_session(time) : md::stock_session(time);
  return session.open || session.market_time == time;
}

/// The open OCO partner of a working order that is not an exit, which the order's
/// fill would cancel: the two never both fill. Null without one.
const StockOrder* open_partner(const State& s, const StockOrder& o) {
  if (!s.revised_share_orders || o.oco == 0 || o.exit()) return nullptr;
  const auto& partner = s.stock_orders.at(static_cast<std::size_t>(o.oco - 1));
  return partner.open() && !partner.exit() ? &partner : nullptr;
}
}  // namespace

/// The shares the open share orders other than `except` would sell beyond the longs
/// held, and the shares they would buy beyond the shorts: bracket exits excluded, as
/// they only ever reduce. Of an OCO pair of sells only the larger counts, since one
/// filling cancels the other.
Quantity pending_short_shares(const State& s, const std::string& symbol, OrderId except, Quantity extra) {
  Quantity position = shares_held(s, symbol);
  Quantity lowest = std::min<Quantity>(position, 0);
  for (const auto id : open_stock_ids(s)) {
    const auto& o = s.stock_orders[id - 1];
    if (!o.open() || o.exit() || o.id == except || o.request.symbol != symbol || o.request.side != Side::Sell) continue;
    if (const auto* partner = open_partner(s, o); partner && partner->id != except && partner->request.side == Side::Sell &&
        (partner->request.shares > o.request.shares || (partner->request.shares == o.request.shares && partner->id < o.id)))
      continue;
    position -= o.request.shares;
    lowest = std::min(lowest, position);
  }
  position += extra;
  return std::min(lowest, position);
}
namespace {

/// The price bound a working share order is reserved at: its limit, or for market and
/// stop orders the underlying's price, fresh or last (a buy stop at least its stop, a
/// sell stop at most it, and the stop without a price).
std::optional<Money> stock_order_bound(const State& s, const StockOrder& o) {
  if (o.request.limit_price) return *o.request.limit_price;
  auto price = stock_price(s, o.request.symbol);
  if (!price) {
    const auto mark = s.stock_marks.find(o.request.symbol);
    if (mark != s.stock_marks.end()) price = mark->second.price;
  }
  if (o.request.stop_price) price = price ? (o.request.side == Side::Buy ? std::max(*price, *o.request.stop_price)
                                                                         : std::min(*price, *o.request.stop_price))
                                          : *o.request.stop_price;
  return price;
}
}  // namespace

/// What the open share orders reserve: each, in acceptance order after those before it,
/// what filling it at its bound would take from buying power (the change in the
/// requirement plus the cash it pays), never less than nothing. Bracket exits only
/// reduce, so they reserve nothing. An OCO pair fills one of its orders at most: it
/// reserves once, at its first order, for whichever of the two would take more.
Money stock_orders_reserved(const State& s) {
  const auto open = open_stock_ids(s);
  if (std::none_of(open.begin(), open.end(), [&](OrderId id) { return !s.stock_orders[id - 1].exit(); }))
    return {};
  Money reserved;
  State after = s;
  const auto book = held_book(s);
  Money previous = after.ledger.account().cash - requirement(after, book);
  for (const auto id : open) {
    const auto& o = s.stock_orders[id - 1];
    if (!o.open() || o.exit()) continue;
    const auto* partner = open_partner(s, o);
    if (partner && partner->id < o.id) continue;  // reserved with its partner
    if (!partner) {
      const auto bound = stock_order_bound(s, o);
      if (!bound) continue;
      after.ledger.trade_stock(o.request.symbol, signed_shares_of(o.request), *bound, Money{});
      const Money now = after.ledger.account().cash - requirement(after, book);
      reserved = reserved + std::max(Money{}, previous - now);
      previous = now;
      continue;
    }
    // Either order of the pair filled alone, from the ledger the orders before it
    // leave: the one that leaves less is reserved, and the orders after it follow it.
    const auto start = after.ledger;
    std::optional<Ledger> worst;
    Money least;
    for (const auto* one : {&o, partner}) {
      const auto bound = stock_order_bound(s, *one);
      if (!bound) continue;
      after.ledger = start;
      after.ledger.trade_stock(one->request.symbol, signed_shares_of(one->request), *bound, Money{});
      const Money left = after.ledger.account().cash - requirement(after, book);
      if (!worst || left < least) { worst = after.ledger; least = left; }
    }
    after.ledger = worst ? *worst : start;
    if (!worst) continue;
    reserved = reserved + std::max(Money{}, previous - least);
    previous = least;
  }
  return reserved;
}

void end_stock_order(State& s, OrderId id, OrderStatus status, Decision reason, Events& events) {
  auto& o = s.stock_orders.mut(static_cast<std::size_t>(id - 1));
  if (!o.open()) return;
  o.status = status;
  o.reason = std::move(reason);
  o.ended_at = s.time;
  event(events, status == OrderStatus::Rejected ? "stock_order_rejected" : "stock_order_cancelled",
        Json{{"id", id}, {"symbol", o.request.symbol}, {"reason", o.reason}});
}
namespace {

/// The acceptance checks: the price's freshness only for an order that executes now,
/// the account, the plan and the account type for opening orders, which sees the
/// working sells too, and the buying power the order would reserve. An OCO partner
/// names its first order as `pair`: it sells instead of that order, not beside it.
Decision stock_acceptance_check(const State& s, const StockOrderRequest& r, OrderRole role, OrderId pair = 0) {
  const auto signed_shares = signed_shares_of(r);
  const auto held = shares_held(s, r.symbol);
  if (role != OrderRole::Normal) return {};
  if (s.time < halt_end(s, r.symbol)) return failure(Reason::MARKET_HALTED, "Trading is halted for " + r.symbol);
  // A sell reduces only what the working sells before it leave of the longs.
  const bool reduces = signed_shares > 0 ? held < 0 && signed_shares <= -held
                                         : held > 0 && pending_short_shares(s, r.symbol, pair, signed_shares) >= 0;
  if (const auto d = account_check(s, reduces); !d.ok()) return d;
  if (const auto d = share_open_loss_check(s, r.symbol, held, signed_shares); !d.ok()) return d;
  if (reduces) return {};
  if (const auto d = plan_entry_check(s.config.rules, r.symbol, s.time); !d.ok()) return d;
  if (md::is_index_underlying(r.symbol))
    return failure(Reason::INVALID_ORDER, "Index underlyings settle in cash and have no shares to trade");
  if (r.side == Side::Sell && pending_short_shares(s, r.symbol, pair, -r.shares) < 0) {
    if (s.config.rules.buy_only) return failure(Reason::BUY_ONLY, "This plan is buy-only: share sales may only reduce long shares");
    if (s.config.rules.defined_risk) return failure(Reason::DEFINED_RISK, "This plan allows defined risk only: short shares can lose without limit");
    if (s.config.rules.account_type != AccountType::Margin)
      return failure(Reason::ACCOUNT_TYPE, "With your open share orders filled, this would leave shares short; "
                                           "a cash account or IRA never sells shares short");
  }
  if (s.config.rules.require_stop_loss && !r.stop_loss)
    return failure(Reason::STOP_REQUIRED, "This plan requires a stop-loss: enter shares with a bracket stop_loss");
  // Opening orders reserve buying power at acceptance (stock_order_bound): a limit at
  // its limit, a stop at least at its stop, and a market order at the last price,
  // which it therefore needs.
  if (s.config.rules.buying_power && r.type == StockOrderType::Market && !s.stock_marks.contains(r.symbol))
    return failure(Reason::STALE_QUOTE, "A market share order needs a price for " + r.symbol + " to reserve buying power against");
  return {};
}
}  // namespace

std::pair<Decision, OrderId> accept_stock_order(State& s, StockOrderRequest request, OrderRole role, OrderId parent,
                                                Events& events, OrderId pair) {
  if (const auto d = stock_acceptance_check(s, request, role, pair); !d.ok()) return {d, 0};
  auto partners = std::move(request.oco);
  request.oco.clear();
  StockOrder order;
  order.id = s.stock_orders.size() + 1;
  order.request = std::move(request);
  order.status = stops(order.request) ? OrderStatus::Armed : OrderStatus::Working;
  order.accepted_at = s.time;
  order.day_end = stock_day_end(order.request, s.time);
  order.role = role;
  order.parent = parent;
  order.actor = s.actor;
  const auto id = order.id;
  s.stock_orders.push_back(std::move(order));
  // Its client order ID names it unless an order before it in this attempt took it.
  if (role == OrderRole::Normal)
    if (const auto& key = s.stock_orders.back().request.client_order_id;
        !s.stock_clients.contains(key) || s.stock_clients.find(key)->second < s.evaluation.first_stock_order)
      s.stock_clients[key] = id;
  // An OCO partner is paired with its first order before its reservation is checked,
  // so the two reserve as one.
  if (pair != 0) {
    s.stock_orders.mut(static_cast<std::size_t>(id - 1)).oco = pair;
    s.stock_orders.mut(static_cast<std::size_t>(pair - 1)).oco = id;
  }
  // The account's buying power, with this order reserved, must stay available.
  if (role == OrderRole::Normal && s.config.rules.buying_power && buying_power(s).total.available < Money{}) {
    State without = s;
    without.stock_orders.mut(static_cast<std::size_t>(id - 1)).status = OrderStatus::Rejected;
    if (buying_power(without).total.available > buying_power(s).total.available) {
      const auto short_by = -buying_power(s).total.available;
      const auto symbol = s.stock_orders.back().request.symbol;
      if (pair != 0) s.stock_orders.mut(static_cast<std::size_t>(pair - 1)).oco = 0;
      // The refused order names nothing: no order before it in this attempt took its ID.
      if (const auto key = s.stock_orders.back().request.client_order_id; s.stock_clients.contains(key) && s.stock_clients.find(key)->second == id)
        s.stock_clients.erase(key);
      s.stock_orders.truncate(s.stock_orders.size() - 1);
      return {{Reason::BUYING_POWER, "This share order needs more buying power than the account has available",
               short_by.dollars(), 0.0, symbol}, 0};
    }
  }
  event(events, "stock_order_accepted", Json{{"id", id}, {"order", s.stock_orders.back().request}});
  for (auto& partner : partners) {
    partner.client_order_id = s.stock_orders.at(static_cast<std::size_t>(id - 1)).request.client_order_id + ":oco";
    auto terms = partner;
    auto [d, other] = accept_stock_order(s, std::move(partner), OrderRole::Normal, 0, events, s.revised_share_orders ? id : 0);
    if (!d.ok()) {
      // The refused partner's terms stay on the order, so a retry of the pair gets
      // this answer again rather than DUPLICATE_CLIENT_ID.
      if (s.revised_share_orders) s.stock_orders.mut(static_cast<std::size_t>(id - 1)).request.oco.push_back(std::move(terms));
      end_stock_order(s, id, OrderStatus::Rejected, d, events);
      return {d, id};
    }
    s.stock_orders.mut(static_cast<std::size_t>(id - 1)).oco = other;
    s.stock_orders.mut(static_cast<std::size_t>(other - 1)).oco = id;
  }
  return {{}, id};
}
namespace {

/// After an entry fills, its bracket: a stop and a target for the shares it traded,
/// each the other's OCO, good until cancelled and always within the position.
void place_stock_bracket(State& s, OrderId entry, Events& events) {
  const auto& e = s.stock_orders.at(static_cast<std::size_t>(entry - 1));
  if (!e.request.stop_loss && !e.request.take_profit) return;
  const auto base = e.request;
  OrderId stop = 0, target = 0;
  StockOrderRequest exit;
  exit.symbol = base.symbol;
  exit.side = base.side == Side::Buy ? Side::Sell : Side::Buy;
  exit.tif = TimeInForce::Gtc;
  exit.shares = e.filled_shares;
  if (base.stop_loss) {
    auto r = exit;
    r.client_order_id = base.client_order_id + ":stop";
    r.type = StockOrderType::Stop;
    r.stop_price = base.stop_loss;
    stop = accept_stock_order(s, std::move(r), OrderRole::StopLoss, entry, events).second;
  }
  if (base.take_profit) {
    auto r = exit;
    r.client_order_id = base.client_order_id + ":target";
    r.type = StockOrderType::Limit;
    r.limit_price = base.take_profit;
    target = accept_stock_order(s, std::move(r), OrderRole::TakeProfit, entry, events).second;
  }
  auto& placed = s.stock_orders.mut(static_cast<std::size_t>(entry - 1));
  placed.stop_loss = stop;
  placed.take_profit = target;
  if (stop && target) {
    s.stock_orders.mut(static_cast<std::size_t>(stop - 1)).oco = target;
    s.stock_orders.mut(static_cast<std::size_t>(target - 1)).oco = stop;
  }
}

/// Reasons a share order keeps waiting instead of ending: the market or its data, not
/// the order or the account.
bool share_data_gap(Reason code) {
  return code == Reason::STALE_QUOTE || code == Reason::SESSION_CLOSED || code == Reason::MARKET_HALTED ||
         code == Reason::MISSING_VALUATION;
}
}  // namespace

void work_stock_orders(State& s, Events& events, std::optional<OrderId> only) {
  const auto work = [&](std::size_t i) {
    const auto& o = s.stock_orders[i];
    if (!o.open() || (only && o.id != *only)) return;
    const auto& symbol = o.request.symbol;
    const auto mark = s.stock_marks.find(symbol);
    // An extended-hours order fills only on a price fresh within the session it
    // fills in: after hours, not the regular close.
    const auto price = stock_price(s, symbol, s.time, o.request.extended_hours);
    // A resting order takes a price printed after it was accepted; the one it was
    // accepted on executes it at once.
    if (!price || mark == s.stock_marks.end() || (mark->second.time < o.accepted_at && !only)) return;
    if (s.time < halt_end(s, symbol)) return;
    const bool buy = o.request.side == Side::Buy;
    // A bracket exit protects what is still held: it closes no more than that, and
    // ends once nothing is left in its direction.
    Quantity shares = o.request.shares;
    if (o.exit()) {
      const auto held = shares_held(s, symbol);
      const auto available = buy ? std::max<Quantity>(-held, 0) : std::max<Quantity>(held, 0);
      if (available == 0) {
        end_stock_order(s, o.id, OrderStatus::Cancelled, failure(Reason::POSITION_CLOSED, "The shares it protected are closed"), events);
        return;
      }
      shares = std::min(shares, available);
    }
    if (o.status == OrderStatus::Armed) {
      // Stops are reached on the price itself, printed in a session the order works
      // in (its close included), and execute only while one is open.
      const auto level = *o.request.stop_price;
      if (!(buy ? *price >= level : *price <= level)) return;
      if (s.revised_share_orders && !stock_order_print(o, mark->second.time)) return;
      auto& armed = s.stock_orders.mut(i);
      armed.status = OrderStatus::Working;
      armed.triggered_at = s.time;
      event(events, "stock_order_triggered", Json{{"id", armed.id}, {"symbol", symbol}, {"price", *price}});
    }
    const auto& working = s.stock_orders[i];
    if (!stock_order_session(working, s.time)) return;
    if (working.request.limit_price && (buy ? *price > *working.request.limit_price : *price < *working.request.limit_price)) return;
    const auto id = working.id;
    const auto role = working.role;
    ShareTerms terms;
    terms.reduce_only = working.exit();
    terms.protective = working.exit();
    terms.stop = working.request.stop_loss;
    terms.order = id;
    // Filled before it trades, so its own reservation does not count against it.
    s.stock_orders.mut(i).status = OrderStatus::Filled;
    const auto d = trade_shares_checked(s, symbol, buy ? shares : -shares, *price, terms, events);
    auto& done = s.stock_orders.mut(i);
    if (!d.ok()) {
      done.status = OrderStatus::Working;
      if (share_data_gap(d.code)) return;
      end_stock_order(s, id, OrderStatus::Rejected, d, events);
      return;
    }
    done.filled_shares = shares;
    done.fill_price = *price;
    done.stock_fill = s.stock_fills.size();
    done.ended_at = s.time;
    event(events, "stock_order_filled", Json{{"id", id}, {"symbol", symbol}, {"shares", buy ? shares : -shares}, {"price", *price}});
    if (const auto other = s.stock_orders[i].oco; other != 0)
      end_stock_order(s, other, OrderStatus::Cancelled,
                      failure(Reason::OCO_FILLED, role == OrderRole::Normal ? "Its one-cancels-other partner filled" : "The bracket's other exit filled"),
                      events);
    if (role == OrderRole::Normal) place_stock_bracket(s, id, events);
  };
  // The orders that can be open, in ID order: those open when the share orders were
  // last indexed, then each placed since, the brackets placed here as entries fill
  // included. An order that has ended never works again.
  const auto indexed = s.stock_working;
  for (const auto id : indexed) work(static_cast<std::size_t>(id - 1));
  for (auto i = s.stock_indexed; i < s.stock_orders.size(); ++i) work(i);
}
namespace {

/// Whether an open share order could open or add to a position: with the working
/// orders on its side accepted before it filled first (of an OCO pair only the larger
/// one, once), it would trade past the shares held. Bracket exits only ever reduce.
bool share_order_opens(const State& s, const StockOrder& o) {
  if (o.exit()) return false;
  const bool buy = o.request.side == Side::Buy;
  const auto held = shares_held(s, o.request.symbol);
  Quantity room = buy ? std::max<Quantity>(-held, 0) : std::max<Quantity>(held, 0);
  for (const auto id : open_stock_ids(s)) {
    if (id >= o.id) break;
    const auto& other = s.stock_orders[id - 1];
    if (other.exit() || other.request.symbol != o.request.symbol || other.request.side != o.request.side) continue;
    Quantity shares = other.request.shares;
    if (const auto* partner = open_partner(s, other); partner && partner->request.side == other.request.side) {
      if (partner->id == o.id || partner->id < other.id) continue;  // `o` trades instead of it, or it counted with its partner
      shares = std::max(shares, partner->request.shares);
    }
    room -= shares;
  }
  return o.request.shares > room;
}
}  // namespace

/// Ends, with `reason`, the working share orders that could open, as the kill switch,
/// a decided attempt and a locked day cancel the option orders that would: one at a
/// time in acceptance order, so an older order keeps the shares it reduces first.
/// Bracket exits and orders that only reduce keep working.
void cancel_opening_stock_orders(State& s, const Decision& reason, Events& events) {
  for (const auto id : open_stock_ids(s)) {
    const auto& o = s.stock_orders[id - 1];
    if (o.open() && share_order_opens(s, o)) end_stock_order(s, id, OrderStatus::Cancelled, reason, events);
  }
}

/// The plan's entry rules for working share orders that could open, as advance applies
/// them to option orders: one its trading window, flat time, calendar or underlyings
/// now refuse ends with that rule's code, and one that waited across the window's end
/// with OUTSIDE_PLAN_HOURS.
void sweep_opening_stock_orders(State& s, bool skipped_end, Events& events) {
  const auto& rules = s.config.rules;
  for (const auto id : open_stock_ids(s)) {
    const auto& o = s.stock_orders[id - 1];
    if (!o.open() || o.exit()) continue;
    auto d = skipped_end
        ? Decision{Reason::OUTSIDE_PLAN_HOURS, "Working opening order reached the end of the plan's trading window",
                   static_cast<double>(md::new_york_time(s.time).seconds / 60), static_cast<double>(*rules.trading_end),
                   o.request.symbol}
        : plan_entry_check(rules, o.request.symbol, s.time);
    if (!d.ok() && share_order_opens(s, o)) end_stock_order(s, id, OrderStatus::Cancelled, std::move(d), events);
  }
}

/// Day orders end with their session; every share order ends with a reset or when
/// its attempt is decided and it would open.
void expire_stock_orders(State& s, Events& events) {
  for (const auto id : open_stock_ids(s)) {
    const auto& o = s.stock_orders[id - 1];
    if (o.open() && s.time >= o.day_end)
      end_stock_order(s, o.id, OrderStatus::Cancelled, failure(Reason::DAY_END, "The order's session ended"), events);
  }
}
}  // namespace session_internal
}  // namespace openport::trading
