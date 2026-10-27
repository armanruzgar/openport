// Working share orders: resting limits, stops and stop-limits on stocks and ETFs,
// with extended hours, brackets and OCO. Included by session.cpp inside its
// anonymous namespace, after the share and account checks it reuses.

/// How a share trade came about, for the checks trade_shares_checked applies.
struct ShareTerms {
  bool reduce_only = false;  ///< Only reduce the shares held (a stock close or a bracket exit).
  bool protective = false;   ///< A bracket exit: it bypasses the minimum hold, as option exits do.
  /// An entry's bracket stop: the plan's stop requirement is met, and its trade risk
  /// is the move to the stop rather than the shares' whole value.
  std::optional<Money> stop;
  std::uint64_t order = 0;   ///< The working order it fills, recorded with the fill.
  bool exercise = false;     ///< Shares an exercise delivers: no bracket can protect them.
};

/// Shares whose open loss is at the plan's open-loss cap can be reduced but not added to.
Decision share_open_loss_check(const State& s, const std::string& symbol, Quantity held, Quantity signed_shares) {
  const auto& cap = s.config.rules.open_loss_cap;
  if (!cap || cap->action == OpenLossAction::Fail || held == 0 || (held > 0) != (signed_shares > 0) ||
      std::find(s.evaluation.open_loss_symbols.begin(), s.evaluation.open_loss_symbols.end(), symbol) == s.evaluation.open_loss_symbols.end())
    return {};
  return {Reason::OPEN_LOSS_CAP, "These shares' open loss is at the plan's open-loss cap; they can be reduced, "
          "but not added to, until they are back within the cap", {}, {}, symbol};
}

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

/// The checks shares that open or add to a holding take at their price, after
/// share_entry_checks: the stop requirement, hedging, the other accounts' positions,
/// complete marks, the loss and trade-risk limits, and exposure once `project` has
/// applied the whole trade to a copy of the state. Exercise delivers shares through
/// the same checks as a share trade, since the plan's share-entry rules are the same.
template <class Project>
Decision opening_share_rules_check(State& s, const std::string& symbol, Quantity signed_shares, Money price,
                                   const ShareTerms& terms, Events& events, const Project& project) {
  if (s.config.rules.require_stop_loss && !terms.stop)
    return failure(Reason::STOP_REQUIRED, terms.exercise ? "This plan requires a stop-loss: exercise would open shares without one; sell the option instead"
                                          : terms.order ? "This plan requires a stop-loss: enter shares with a bracket stop_loss"
                                                        : "This plan requires a stop-loss; share entries do not support brackets");
  if (const auto d = hedging_direction_check(s, symbol, static_cast<double>(signed_shares) * price.dollars()); !d.ok()) return d;
  if (const auto d = opening_share_check(s, symbol, signed_shares, price, terms, events); !d.ok()) return d;
  const auto before = measure(s);
  if (!before.valuation_complete)
    return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before opening shares");
  if (const auto d = loss_check(s, before); !d.ok()) return d;
  if (const auto limit = trade_risk_limit(s, before.equity)) {
    const auto magnitude_shares = signed_shares < 0 ? -signed_shares : signed_shares;
    // To its stop, a bracketed entry risks the move there; without one, a long
    // risks its whole value and a short an unlimited loss.
    std::optional<Money> loss;
    if (terms.stop) loss = (signed_shares > 0 ? std::max(Money{}, price - *terms.stop) : std::max(Money{}, *terms.stop - price)) * magnitude_shares;
    else if (signed_shares > 0) loss = price * signed_shares;
    if (!loss || *loss > *limit)
      return {Reason::MAX_TRADE_RISK, terms.exercise ? "Exercise delivers shares whose risk before fees exceeds the plan's trade limit"
                                                     : "Share entry risk before fees exceeds the plan limit",
              loss ? std::optional(loss->dollars()) : std::nullopt, limit->dollars(), "trade"};
  }
  State projected = s;
  project(projected);
  return check_exposure(measure(projected).risk, before.risk);
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

/// The shares the open share orders other than `except` would sell beyond the longs
/// held, and the shares they would buy beyond the shorts: bracket exits excluded, as
/// they only ever reduce.
Quantity pending_short_shares(const State& s, const std::string& symbol, OrderId except, Quantity extra) {
  Quantity position = shares_held(s, symbol);
  Quantity lowest = std::min<Quantity>(position, 0);
  for (const auto& o : s.stock_orders) {
    if (!o.open() || o.exit() || o.id == except || o.request.symbol != symbol || o.request.side != Side::Sell) continue;
    position -= o.request.shares;
    lowest = std::min(lowest, position);
  }
  position += extra;
  return std::min(lowest, position);
}

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

/// What the open share orders reserve: each, in acceptance order after those before it,
/// what filling it at its bound would take from buying power (the change in the
/// requirement plus the cash it pays), never less than nothing. Bracket exits only
/// reduce, so they reserve nothing.
Money stock_orders_reserved(const State& s) {
  if (std::none_of(s.stock_orders.begin(), s.stock_orders.end(), [](const StockOrder& o) { return o.open() && !o.exit(); }))
    return {};
  Money reserved;
  State after = s;
  const auto book = held_book(s);
  Money previous = after.ledger.account().cash - requirement(after, book);
  for (const auto& o : s.stock_orders) {
    if (!o.open() || o.exit()) continue;
    const auto bound = stock_order_bound(s, o);
    if (!bound) continue;
    after.ledger.trade_stock(o.request.symbol, signed_shares_of(o.request), *bound, Money{});
    const Money now = after.ledger.account().cash - requirement(after, book);
    reserved = reserved + std::max(Money{}, previous - now);
    previous = now;
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

/// Add a validated order (and an OCO partner) after the acceptance checks, and work it
/// at once: a limit that crosses the current price fills now, a stop that is already
/// reached activates. Returns the refusal, if any, and the new order's ID.
std::pair<Decision, OrderId> accept_stock_order(State& s, StockOrderRequest request, OrderRole role, OrderId parent,
                                                Events& events);
void work_stock_orders(State& s, Events& events, std::optional<OrderId> only = std::nullopt);

/// The acceptance checks: the price's freshness only for an order that executes now,
/// the account, the plan and the account type for opening orders, which sees the
/// working sells too, and the buying power the order would reserve.
Decision stock_acceptance_check(const State& s, const StockOrderRequest& r, OrderRole role) {
  const auto signed_shares = signed_shares_of(r);
  const auto held = shares_held(s, r.symbol);
  if (role != OrderRole::Normal) return {};
  if (s.time < halt_end(s, r.symbol)) return failure(Reason::MARKET_HALTED, "Trading is halted for " + r.symbol);
  // A sell reduces only what the working sells before it leave of the longs.
  const bool reduces = signed_shares > 0 ? held < 0 && signed_shares <= -held
                                         : held > 0 && pending_short_shares(s, r.symbol, 0, signed_shares) >= 0;
  if (const auto d = account_check(s, reduces); !d.ok()) return d;
  if (const auto d = share_open_loss_check(s, r.symbol, held, signed_shares); !d.ok()) return d;
  if (reduces) return {};
  if (const auto d = plan_entry_check(s.config.rules, r.symbol, s.time); !d.ok()) return d;
  if (md::is_index_underlying(r.symbol))
    return failure(Reason::INVALID_ORDER, "Index underlyings settle in cash and have no shares to trade");
  if (r.side == Side::Sell && pending_short_shares(s, r.symbol, 0, -r.shares) < 0) {
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

std::pair<Decision, OrderId> accept_stock_order(State& s, StockOrderRequest request, OrderRole role, OrderId parent,
                                                Events& events) {
  if (const auto d = stock_acceptance_check(s, request, role); !d.ok()) return {d, 0};
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
  // The account's buying power, with this order reserved, must stay available.
  if (role == OrderRole::Normal && s.config.rules.buying_power && buying_power(s).total.available < Money{}) {
    State without = s;
    without.stock_orders.mut(static_cast<std::size_t>(id - 1)).status = OrderStatus::Rejected;
    if (buying_power(without).total.available > buying_power(s).total.available) {
      const auto short_by = -buying_power(s).total.available;
      const auto symbol = s.stock_orders.back().request.symbol;
      s.stock_orders.truncate(s.stock_orders.size() - 1);
      return {{Reason::BUYING_POWER, "This share order needs more buying power than the account has available",
               short_by.dollars(), 0.0, symbol}, 0};
    }
  }
  event(events, "stock_order_accepted", Json{{"id", id}, {"order", s.stock_orders.back().request}});
  for (auto& partner : partners) {
    partner.client_order_id = s.stock_orders.at(static_cast<std::size_t>(id - 1)).request.client_order_id + ":oco";
    auto [d, other] = accept_stock_order(s, std::move(partner), OrderRole::Normal, 0, events);
    if (!d.ok()) {
      end_stock_order(s, id, OrderStatus::Rejected, d, events);
      return {d, id};
    }
    s.stock_orders.mut(static_cast<std::size_t>(id - 1)).oco = other;
    s.stock_orders.mut(static_cast<std::size_t>(other - 1)).oco = id;
  }
  return {{}, id};
}

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

void work_stock_orders(State& s, Events& events, std::optional<OrderId> only) {
  for (std::size_t i = 0; i < s.stock_orders.size(); ++i) {
    const auto& o = s.stock_orders[i];
    if (!o.open() || (only && o.id != *only)) continue;
    const auto& symbol = o.request.symbol;
    const auto mark = s.stock_marks.find(symbol);
    // An extended-hours order fills only on a price fresh within the session it
    // fills in: after hours, not the regular close.
    const auto price = stock_price(s, symbol, s.time, o.request.extended_hours);
    // A resting order takes a price printed after it was accepted; the one it was
    // accepted on executes it at once.
    if (!price || mark == s.stock_marks.end() || (mark->second.time < o.accepted_at && !only)) continue;
    if (s.time < halt_end(s, symbol)) continue;
    const bool buy = o.request.side == Side::Buy;
    // A bracket exit protects what is still held: it closes no more than that, and
    // ends once nothing is left in its direction.
    Quantity shares = o.request.shares;
    if (o.exit()) {
      const auto held = shares_held(s, symbol);
      const auto available = buy ? std::max<Quantity>(-held, 0) : std::max<Quantity>(held, 0);
      if (available == 0) {
        end_stock_order(s, o.id, OrderStatus::Cancelled, failure(Reason::POSITION_CLOSED, "The shares it protected are closed"), events);
        continue;
      }
      shares = std::min(shares, available);
    }
    if (o.status == OrderStatus::Armed) {
      // Stops are reached on the price itself; they activate in any session the
      // order works in, and execute only in one.
      const auto level = *o.request.stop_price;
      if (!(buy ? *price >= level : *price <= level)) continue;
      auto& armed = s.stock_orders.mut(i);
      armed.status = OrderStatus::Working;
      armed.triggered_at = s.time;
      event(events, "stock_order_triggered", Json{{"id", armed.id}, {"symbol", symbol}, {"price", *price}});
    }
    const auto& working = s.stock_orders[i];
    if (!stock_order_session(working, s.time)) continue;
    if (working.request.limit_price && (buy ? *price > *working.request.limit_price : *price < *working.request.limit_price)) continue;
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
      if (share_data_gap(d.code)) continue;
      end_stock_order(s, id, OrderStatus::Rejected, d, events);
      continue;
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
  }
}

/// Day orders end with their session; every share order ends with a reset or when
/// its attempt is decided and it would open.
void expire_stock_orders(State& s, Events& events) {
  for (std::size_t i = 0; i < s.stock_orders.size(); ++i) {
    const auto& o = s.stock_orders[i];
    if (o.open() && s.time >= o.day_end)
      end_stock_order(s, o.id, OrderStatus::Cancelled, failure(Reason::DAY_END, "The order's session ended"), events);
  }
}
