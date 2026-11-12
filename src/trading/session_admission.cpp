// The checks an option order passes to be accepted, activated or filled: the account
// type and defined risk, exposure, the account and plan rules, its terms, combos and
// groups, minimum holds, discipline, and the market's own; and the counter-position
// and volume-share checks other accounts' positions take part in.

#include "session_internal.hpp"

namespace openport::trading {
Decision counter_position_check(const std::map<std::string, double>& direction,
    const std::vector<CounterExposure>& others) {
  for (const auto& [underlying, delta] : direction) {
    if (delta == 0) continue;
    for (const auto& other : others) {
      if (other.underlying != underlying) continue;
      if (!other.dollar_delta) return {Reason::MISSING_VALUATION,
          "Fresh held and working valuations required for account " + other.account + " on " + underlying, {}, {}, underlying};
      const auto held = *other.dollar_delta;
      if (held != 0 && (held > 0) != (delta > 0))
        return {Reason::COUNTER_POSITION, "Opening " + underlying + " dollar delta " + format_dollar_delta(delta) +
            " opposes account " + other.account + " held or working dollar delta " + format_dollar_delta(held),
            delta, held, underlying, RuleEvidence{underlying, delta, held, other.account}};
    }
  }
  return {};
}
Decision volume_share_check(const std::string& symbol, Quantity held, Quantity opening,
    std::int64_t percent, std::optional<std::int64_t> volume) {
  __extension__ using Wide = __int128;
  const Wide contracts = (held < 0 ? -static_cast<Wide>(held) : held) + opening;
  if (contracts > std::numeric_limits<Quantity>::max())
    return {Reason::ARITHMETIC_OVERFLOW, "Volume-share contract count overflow", {}, {}, symbol};
  if (volume && contracts * 100 <= static_cast<Wide>(percent) * *volume) return {};
  RuleEvidence evidence;
  evidence.contract = symbol; evidence.contracts = static_cast<Quantity>(contracts);
  evidence.volume = volume; evidence.percent = percent;
  return {Reason::MAX_VOLUME_SHARE, symbol + ": held plus opening contracts " + std::to_string(evidence.contracts) +
      " exceeds " + std::to_string(percent) + "% of current-date volume " +
      (volume ? std::to_string(*volume) : "unknown (missing, invalid or stale)"),
      static_cast<double>(contracts), volume ? std::optional(static_cast<double>(*volume) * static_cast<double>(percent) / 100) : std::nullopt,
      symbol, evidence};
}
Decision minute_volume_check(const std::string& symbol, Quantity opening, std::int64_t percent,
    std::optional<std::int64_t> volume) {
  __extension__ using Wide = __int128;
  if (opening <= 0) return {};
  RuleEvidence evidence;
  evidence.contract = symbol; evidence.contracts = opening; evidence.volume = volume; evidence.percent = percent;
  if (!volume)
    return {Reason::MINUTE_VOLUME, symbol + ": the plan caps opening orders at " + std::to_string(percent) +
        "% of the last minute's volume, and this feed has not reported the contract's volume both a minute ago and within "
        "the last minute, so the cap cannot be checked", static_cast<double>(opening), std::nullopt, symbol, evidence};
  if (static_cast<Wide>(opening) * 100 <= static_cast<Wide>(percent) * *volume) return {};
  return {Reason::MINUTE_VOLUME, symbol + ": opening " + std::to_string(opening) + " contracts exceeds " + std::to_string(percent) +
      "% of the last minute's volume " + std::to_string(*volume), static_cast<double>(opening),
      static_cast<double>(*volume) * static_cast<double>(percent) / 100, symbol, evidence};
}
namespace session_internal {
namespace {
Decision tif_check(const State& s, const OrderRequest& r, Timestamp accepted) {
  if (r.tif != TimeInForce::Gtd)
    return r.good_till ? failure(Reason::INVALID_ORDER, "Only GTD takes good_till") : Decision{};
  constexpr auto bound = 366LL * 24 * 60 * 60 * md::kNanosPerSecond;
  if (!r.good_till || *r.good_till <= s.time || *r.good_till - accepted > bound)
    return failure(Reason::INVALID_ORDER, "GTD needs a future good_till timestamp within 366 days of acceptance");
  return {};
}
// Same freshness policy as portfolio_risk: invalid, future or stale Greeks
// cannot establish direction. Dollar delta includes spot, as the risk buckets do.
std::optional<double> option_dollar_delta(const State& s, const std::string& symbol, Quantity quantity) {
  const auto c = s.contracts.find(symbol);
  const auto v = s.valuations.find(symbol);
  if (c == s.contracts.end() || v == s.valuations.end() || !valid_valuation(v->second) ||
      v->second.time < 0 || v->second.time > s.time ||
      observation_time(c->second, s.time) - v->second.time > s.config.limits.max_valuation_age) return std::nullopt;
  const double delta = static_cast<double>(quantity) * c->second.multiplier * v->second.delta * v->second.spot;
  return std::isfinite(delta) ? std::optional(delta) : std::nullopt;
}
}  // namespace
std::optional<double> held_direction(const State& s, const std::string& underlying) {
  double total = 0;
  for (const auto& [symbol, p] : s.ledger.positions()) {
    if (p.contract.underlying != underlying) continue;
    const auto delta = option_dollar_delta(s, symbol, p.quantity);
    if (!delta) return std::nullopt;
    total += *delta;
  }
  if (const auto shares = shares_held(s, underlying); shares != 0) {
    const auto price = stock_price(s, underlying);
    if (!price) return std::nullopt;
    total += static_cast<double>(shares) * price->dollars();
  }
  return std::isfinite(total) ? std::optional(total) : std::nullopt;
}
OpeningOrder opening_exposure(const State& s, const OrderRequest& request,
                              const std::map<std::string, Quantity>& preceding) {
  OpeningOrder result;
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  for (const auto& leg : legs) {
    if (request.quantity <= 0 || leg.ratio < 1 || leg.ratio > kMaxRatio ||
        request.quantity > std::numeric_limits<Quantity>::max() / leg.ratio ||
        (leg.side != Side::Buy && leg.side != Side::Sell) || result.contracts.contains(leg.symbol)) {
      result.decision = failure(Reason::INVALID_ORDER, "Invalid opening contract count or side");
      return result;
    }
    const auto q = signed_contracts(leg, request.quantity);
    auto position = held(s, leg.symbol);
    if (const auto it = preceding.find(leg.symbol); it != preceding.end() && __builtin_add_overflow(position, it->second, &position)) {
      result.decision = failure(Reason::ARITHMETIC_OVERFLOW, "Chained holdings overflow");
      return result;
    }
    const auto closing = position != 0 && (position > 0) != (q > 0) ? std::min(magnitude(position), magnitude(q)) : 0;
    const auto opening = magnitude(q) - closing;
    result.contracts[leg.symbol] = opening;
    result.opening = result.opening || opening > 0;
  }
  if (!result.opening) return result;
  for (const auto& leg : legs) {
    const auto c = s.contracts.find(leg.symbol);
    if (c == s.contracts.end()) {
      result.decision = failure(Reason::UNKNOWN_CONTRACT, "Opening order needs a registered contract");
      return result;
    }
    const auto delta = option_dollar_delta(s, leg.symbol, signed_contracts(leg, request.quantity));
    if (!delta) {
      result.decision = failure(Reason::MISSING_VALUATION, "Fresh complete order valuations required to determine direction");
      return result;
    }
    auto& total = result.dollar_delta[c->second.underlying];
    total += *delta;
    if (!std::isfinite(total)) {
      result.decision = failure(Reason::MISSING_VALUATION, "Order dollar delta is not finite");
      return result;
    }
  }
  return result;
}
Decision hedging_direction_check(const State& s, const std::string& underlying, double delta) {
  if (!s.config.rules.no_hedging || delta == 0) return {};
  const auto held_delta = held_direction(s, underlying);
  if (!held_delta) return failure(Reason::MISSING_VALUATION, "Fresh held valuations required to determine hedging direction");
  if (*held_delta == 0 || (*held_delta > 0) == (delta > 0)) return {};
  return {Reason::HEDGING, "Opening " + underlying + " dollar delta " + format_dollar_delta(delta) +
      " opposes held dollar delta " + format_dollar_delta(*held_delta), delta, *held_delta, underlying,
      RuleEvidence{underlying, delta, *held_delta}};
}
namespace {
Decision hedging_check(const State& s, const Order& order) {
  if (!s.config.rules.no_hedging || order.system || kept_within(order) || closing_only(s, order, false)) return {};
  auto request = order.request;
  request.quantity = order.remaining();
  const auto exposure = opening_exposure(s, request);
  if (!exposure.decision.ok()) return exposure.decision;
  for (const auto& [underlying, delta] : exposure.dollar_delta)
    if (auto d = hedging_direction_check(s, underlying, delta); !d.ok()) return d;
  return {};
}
/// Short contracts no long covers, after the account's positions take `extra`
/// (a projected order's signed contracts).
Quantity uncovered(const State& s, const std::vector<std::pair<std::string, Quantity>>& extra) {
  std::map<std::string, Quantity> held;
  for (const auto& [symbol, p] : s.ledger.positions()) held[symbol] = p.quantity;
  for (const auto& [symbol, q] : extra) held[symbol] += q;
  std::vector<MarginLeg> legs;
  for (const auto& [symbol, q] : held)
    if (q != 0) legs.push_back({s.contracts.at(symbol), q, {}, std::nullopt});
  return naked_shorts(legs);
}
/// What the account type cannot hold (disallowed_shorts), after the account
/// takes `extra` contracts and `shares` (signed, by underlying).
Quantity disallowed(const State& s, const std::vector<std::pair<std::string, Quantity>>& extra,
                    const std::map<std::string, Quantity>& shares) {
  std::map<std::string, Quantity> held;
  for (const auto& [symbol, p] : s.ledger.positions()) held[symbol] = p.quantity;
  for (const auto& [symbol, q] : extra) held[symbol] += q;
  std::vector<MarginLeg> legs;
  for (const auto& [symbol, q] : held)
    if (q != 0) legs.push_back({s.contracts.at(symbol), q, {}, std::nullopt});
  std::map<std::string, Quantity> held_shares;
  for (const auto& [symbol, stock] : s.ledger.stocks()) held_shares[symbol] = stock.shares;
  for (const auto& [symbol, q] : shares) held_shares[symbol] += q;
  std::vector<MarginStock> stocks;
  for (const auto& [symbol, q] : held_shares)
    if (q != 0) stocks.push_back({symbol, q, {}});
  // Adjusted series need their own lots of shares to be covered.
  Quantity count = 0;
  for (const auto& group : lot_groups(s, std::move(legs), std::move(stocks)))
    if (group.shares != 0) count += disallowed_shorts(group.legs, group.stocks, s.config.rules.account_type);
  return count;
}
/// A cash account or IRA refuses additional uncovered calls or short shares.
/// Pending orders' sells reserve their covers, just as for defined risk.
std::vector<std::pair<std::string, Quantity>> pending_sells(const State& s, OrderId except,
    std::vector<std::pair<std::string, Quantity>> extra);
}  // namespace
Decision account_type_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            const std::map<std::string, Quantity>& shares, OrderId except) {
  const auto type = s.config.rules.account_type;
  if (type == AccountType::Margin) return {};
  // Delivery can leave a short share position. It may be bought back, but a
  // sale must not extend it even inside the same started lot of 100 shares.
  const bool sells_short = std::any_of(shares.begin(), shares.end(), [&](const auto& item) {
    return item.second < 0 && shares_held(s, item.first) + item.second < 0;
  });
  if (sells_short || disallowed(s, order, shares) > disallowed(s, {}, {}))
    return failure(Reason::ACCOUNT_TYPE, type == AccountType::Cash
        ? "A cash account sells a call only against 100 shares it holds for each contract, and never sells shares short"
        : "An IRA cannot hold a naked call or short shares: cover each short call with 100 shares, or with a long call of "
          "the same underlying that expires with it or later");
  if (disallowed(s, pending_sells(s, except, order), shares) > disallowed(s, pending_sells(s, except, {}), {}))
    return failure(Reason::ACCOUNT_TYPE, "With your open orders filled, this would leave a call uncovered or shares short: "
                   "cancel the order that needs its cover first, or trade the spread as one order");
  return {};
}
/// A defined-risk plan refuses an order that would leave more shorts uncovered.
Decision defined_risk_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            std::string_view message) {
  if (s.config.rules.defined_risk && uncovered(s, order) > uncovered(s, {})) return failure(Reason::DEFINED_RISK, std::string(message));
  return {};
}
namespace {
/// Contracts once every open order but `except` has
/// sold all it offers and bought nothing; a multi-leg order fills whole, and a
/// bracket's two exits sell its position once.
std::vector<std::pair<std::string, Quantity>> pending_sells(const State& s, OrderId except,
    std::vector<std::pair<std::string, Quantity>> extra) {
  for (const auto id : open_ids(s)) {
    const auto& o = s.orders[id - 1];
    if (o.id == except || (except != 0 && o.oco == except) || shadowed(s, o)) continue;
    if (multi_leg(o.request)) {
      for (const auto& leg : o.request.legs) extra.emplace_back(leg.symbol, signed_contracts(leg, o.remaining()));
    } else if (o.request.side == Side::Sell) {
      extra.emplace_back(o.request.symbol, -o.remaining());
    }
  }
  return extra;
}
}  // namespace
/// A new or changed order must not leave more shorts uncovered with the open
/// orders filled either, so a working sell never takes the long a short needs;
/// nor, in a cash account or IRA, more calls it cannot hold.
Decision open_orders_risk_check(const State& s, const Order& o) {
  const bool account = s.config.rules.account_type != AccountType::Margin;
  if (!s.config.rules.defined_risk && !account) return {};
  std::vector<std::pair<std::string, Quantity>> order;
  if (multi_leg(o.request)) {
    for (const auto& leg : o.request.legs) order.emplace_back(leg.symbol, signed_contracts(leg, o.remaining()));
  } else {
    order.emplace_back(o.request.symbol, o.request.side == Side::Buy ? o.remaining() : -o.remaining());
  }
  if (s.config.rules.defined_risk && uncovered(s, pending_sells(s, o.id, order)) > uncovered(s, pending_sells(s, o.id, {})))
    return failure(Reason::DEFINED_RISK, "With your open orders filled, this would leave a short option uncovered: cancel "
                   "the order that sells its long, or that opens the short, first, or trade the spread as one order");
  return account_type_check(s, order, {}, o.id);
}
namespace {
/// Exposure limits guard what pending orders could add: an order is refused only
/// when it raises a bucket's worst reachable exposure above its limit, so a book
/// that is already over a limit can still close, hedge and trade elsewhere.
Decision exposure_check(const State& s, const Order& o, const Measures& snapshot) {
  if (!snapshot.risk.complete) return check_exposure(snapshot.risk);
  std::vector<const Order*> working;
  for (const auto id : open_ids(s))
    if (const auto& other = s.orders[id - 1]; other.id != o.id && !shadowed(s, other)) working.push_back(&other);
  const auto without = portfolio_risk(s.ledger, working, s.contracts, s.valuations, s.config.limits, s.time,
      snapshot.stock_prices, s.config.scenarios, risk_tags(s), {&o});
  return check_exposure(snapshot.risk, without);
}
Decision walk_check(const Order& o, Money tick, Money cap_tick) {
  const auto& r = o.request;
  if (!r.walk) return {};
  const auto& w = *r.walk;
  const bool up = multi_leg(r) || r.side == Side::Buy;
  if (r.type != OrderType::Limit || !r.limit_price || (r.tif != TimeInForce::Day && r.tif != TimeInForce::Gtc) ||
      r.trigger || r.exits_only || o.role != OrderRole::Normal || o.system || o.reduce_only ||
      w.step <= Money{} || w.seconds < 1 || w.seconds > 3600 || (!multi_leg(r) && w.limit <= Money{}) ||
      (up ? w.limit < *r.limit_price : w.limit > *r.limit_price))
    return failure(Reason::INVALID_ORDER, "A walk needs a DAY or GTC limit without a trigger or managed exit, a positive step, "
                   "1 to 3600 seconds and a cap at least as aggressive as the current limit");
  if (w.step.micros() % tick.micros() != 0 || w.limit.micros() % cap_tick.micros() != 0)
    return failure(Reason::INVALID_TICK, "A walk's step and cap must be on the product tick");
  __extension__ using Wide = __int128;
  const auto distance = up ? static_cast<Wide>(w.limit.micros()) - r.limit_price->micros()
                           : static_cast<Wide>(r.limit_price->micros()) - w.limit.micros();
  if (distance > static_cast<Wide>(w.step.micros()) * 1000)
    return failure(Reason::INVALID_ORDER, "A walk takes at most 1000 steps to its cap");
  return {};
}
}  // namespace
Decision account_check(const State& s, bool reducing, OrderId id) {
  if (s.config.rules.evaluation() && s.evaluation.status != EvaluationStatus::Active && !reducing)
    return failure(Reason::EVALUATION_CLOSED, std::string("The evaluation has ") +
        (s.evaluation.status == EvaluationStatus::Passed ? "passed" : "failed") +
        "; only closing orders are allowed; reset the account to open positions in a new attempt");
  if (s.kill && !reducing && !completing_trade(s, id)) return kill_decision(s);
  // A plan limit that locked the day leaves only closing orders until rollover.
  if (s.evaluation.day_lock != Reason::NONE && !reducing)
    return failure(s.evaluation.day_lock, day_lock_message(s.evaluation.day_lock));
  return {};
}
namespace {
/// Both contract caps reserve closing capacity once per side across working
/// orders. Shares and managed exits never reserve option contracts.
Quantity projected_contracts_held(const State& s, const Order& candidate) {
  std::map<std::string, std::pair<Quantity, Quantity>> pending;
  const auto add = [&](const Order& o) {
    if (o.system || kept_within(o) || o.request.exits_only || o.remaining() <= 0) return;
    auto legs = o.request.legs;
    if (legs.empty()) legs.push_back({o.request.symbol, o.request.side, 1});
    for (const auto& leg : legs) {
      auto& [buys, sells] = pending[leg.symbol];
      auto& count = leg.side == Side::Buy ? buys : sells;
      if (__builtin_add_overflow(count, magnitude(signed_contracts(leg, o.remaining())), &count))
        throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Working contract count overflow");
    }
  };
  for (const auto id : open_ids(s))
    if (id != candidate.id) add(s.orders[id - 1]);
  add(candidate);
  Quantity projected = 0;
  const auto count = [&](Quantity value) {
    if (__builtin_add_overflow(projected, value, &projected))
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Held contract count overflow");
  };
  for (const auto& [symbol, position] : s.ledger.positions()) {
    (void)symbol;
    count(magnitude(position.quantity));
  }
  for (const auto& [symbol, sides] : pending) {
    const auto q = held(s, symbol);
    count(std::max<Quantity>(0, sides.first - std::max<Quantity>(0, -q)));
    count(std::max<Quantity>(0, sides.second - std::max<Quantity>(0, q)));
  }
  return projected;
}
Decision contracts_held_check(const State& s, const Order& candidate) {
  const auto fixed = s.config.rules.max_contracts_held;
  const auto scaling = s.evaluation.scaling_limit;
  if ((fixed == 0 && scaling == 0) || candidate.system || kept_within(candidate) || closing_only(s, candidate)) return {};
  bool scale_applies = scaling > 0;
  if (scale_applies) {
    // F39 allows atomic reductions above a stepped-down cap. F15 still reserves
    // their opening legs (including reversals); its cap remains independent.
    __extension__ using Wide = __int128;
    Wide change = 0;
    const auto leg_change = [&](const std::string& symbol, Side side, Quantity quantity) {
      const Wide before = held(s, symbol);
      const Wide after = before + (side == Side::Buy ? static_cast<Wide>(quantity) : -static_cast<Wide>(quantity));
      change += (after < 0 ? -after : after) - (before < 0 ? -before : before);
    };
    if (multi_leg(candidate.request)) {
      for (const auto& leg : candidate.request.legs)
        leg_change(leg.symbol, leg.side, magnitude(signed_contracts(leg, candidate.remaining())));
    } else leg_change(candidate.request.symbol, candidate.request.side, candidate.remaining());
    scale_applies = change > 0;
  }
  const bool scaled = scale_applies && (fixed == 0 || scaling < fixed);
  const auto cap = scaled ? scaling : fixed;
  if (cap == 0) return {};
  const auto projected = projected_contracts_held(s, candidate);
  if (projected > cap)
    return {scaled ? Reason::SCALING_LIMIT : Reason::MAX_CONTRACTS_HELD,
            scaled ? "Scaling plan: held options and working openings exceed this session's contract cap"
                   : "Held options and working opening contracts exceed the plan cap",
            static_cast<double>(projected), static_cast<double>(cap), scaled ? "aggregate" : "account"};
  return {};
}
}  // namespace
bool trade_rules_on(const AccountRules& rules) {
  return rules.require_stop_loss || rules.max_trade_risk > Money{} || rules.max_trade_risk_percent > 0;
}
std::optional<Money> trade_risk_limit(const State& s, Money equity) {
  const auto& r = s.config.rules;
  std::optional<Money> limit;
  if (r.max_trade_risk > Money{}) limit = r.max_trade_risk;
  if (r.max_trade_risk_percent > 0 && r.max_drawdown > Money{}) {
    const auto room = std::max(Money{}, equity - s.evaluation.floor).micros();
    // Round down to a micro-dollar without an overflowing multiplication.
    const auto share = Money::from_micros((room / 100) * r.max_trade_risk_percent +
                                        ((room % 100) * r.max_trade_risk_percent) / 100);
    limit = limit ? std::min(*limit, share) : share;
  }
  return limit;
}
namespace {
const Trigger* protective_stop(const OrderRequest& r) {
  if (!r.bracket || !r.bracket->stop_loss || !r.bracket->stop_loss->trigger) return nullptr;
  const auto& t = *r.bracket->stop_loss->trigger;
  if (t.source == TriggerSource::Underlying) return &t;
  const auto direction = multi_leg(r) || r.side == Side::Sell ? TriggerDirection::AtOrAbove : TriggerDirection::AtOrBelow;
  return t.direction == direction ? &t : nullptr;
}
}  // namespace
TradeRisk trade_risk(const State& s, const OrderRequest& r) {
  auto legs = r.legs;
  if (legs.empty()) legs.push_back({r.symbol, r.side, 1});
  Money net;
  if (r.limit_price) {
    // A walking order can fill anywhere up to its walk's cap: the worst entry.
    const auto price = r.walk ? r.walk->limit : *r.limit_price;
    net = !multi_leg(r) && r.side == Side::Sell ? -price : price;
  } else {
    for (const auto& leg : legs) {
      const auto& q = s.books.at(leg.symbol).quote;
      net = net + (leg.side == Side::Buy ? *q.ask : -*q.bid) * leg.ratio;
    }
  }
  const auto multiplier = static_cast<std::int64_t>(s.contracts.at(legs.front().symbol).multiplier);
  if (const auto* stop = protective_stop(r); stop && stop->source != TriggerSource::Underlying) {
    const auto close = multi_leg(r) || r.side == Side::Sell ? stop->level : -stop->level;
    return {std::max(Money{}, ((net + close) * r.quantity) * multiplier), "stop_loss"};
  }
  const auto loss = expiry_loss(s, r, (net * r.quantity) * multiplier, Money{});
  return {loss, loss ? "expiry_payoff" : "unbounded_or_unknown"};
}
namespace {
Decision trade_rules_check(const State& s, const Order& o, Money equity) {
  const auto& rules = s.config.rules;
  if (!trade_rules_on(rules) || o.system || kept_within(o) || closing_only(s, o)) return {};
  if (rules.require_stop_loss && !protective_stop(o.request))
    return failure(Reason::STOP_REQUIRED, "This plan requires a protective stop-loss trigger in every entry bracket");
  if (const auto limit = trade_risk_limit(s, equity)) {
    const auto risk = trade_risk(s, o.request);
    if (!risk.loss || *risk.loss > *limit)
      return {Reason::MAX_TRADE_RISK, risk.loss ? "Order trade risk before fees exceeds the plan limit"
                  : "Order trade risk is unbounded or cannot be bounded at expiry; add an option or combo stop",
              risk.loss ? std::optional(risk.loss->dollars()) : std::nullopt, limit->dollars(), "trade"};
  }
  return {};
}
/// The level an untriggered stop-limit's limit is banded around, if it has one.
std::optional<Money> stop_level(const Order& o) {
  const auto& t = o.request.trigger;
  if (!t || o.triggered_at > 0 || (t->source != TriggerSource::Option && t->source != TriggerSource::Combo)) return std::nullopt;
  return t->level;
}
/// A trigger's reference and trail fit its source: an underlying trigger reads
/// spot only and trails in dollars or percent; a trail is a positive distance,
/// under 100 percent, or one to 1,000 whole ticks.
bool trail_terms_ok(const std::optional<Trigger>& t) {
  if (!t) return true;
  if (t->reference != TriggerReference::BidAsk && t->reference != TriggerReference::Mid && t->reference != TriggerReference::Mark)
    return false;
  if (t->source == TriggerSource::Underlying && t->reference != TriggerReference::BidAsk) return false;
  if (!t->trail) return true;
  const auto value = t->trail->value.micros();
  switch (t->trail->unit) {
    case TrailUnit::Amount: return value > 0;
    case TrailUnit::Percent: return value > 0 && value < 100'000'000;
    case TrailUnit::Ticks:
      return t->source != TriggerSource::Underlying && value > 0 && value % 1'000'000 == 0 && value <= 1'000'000'000;
  }
  return false;
}
bool trail_terms_ok(const OrderRequest& r) {
  const auto exit_ok = [](const std::optional<ExitSpec>& e) { return !e || trail_terms_ok(e->trigger); };
  return trail_terms_ok(r.trigger) && (!r.bracket || (exit_ok(r.bracket->stop_loss) && exit_ok(r.bracket->take_profit)));
}
/// A reference and a trail read the order's own market, so a trigger on another
/// underlying, a study or the clock takes neither, as the API refuses them (exits
/// never watch those). Driver 8; replay drivers 1-7 did not ask.
Decision conditional_terms_check(const State& s, const OrderRequest& r) {
  const auto& t = r.trigger;
  if (!s.revised_fills || !t || !conditional(*t) || (t->reference == TriggerReference::BidAsk && !t->trail)) return {};
  return failure(Reason::INVALID_ORDER, "A trigger on another underlying, a study or the clock takes no reference or trail");
}
/// Checks a multi-leg order like a single-leg one, per leg where it applies:
/// contracts, sessions, size and quotes on every leg; the net price on the
/// smallest leg tick and inside the band around the net mid (as wide as the
/// band for the legs' gross premium); then rules, exposure and buying power.
Decision combo_check(const State& s, const Order& o, Stage stage) {
  const auto& r = o.request;
  const auto& rules = s.config.rules;
  const bool shape = r.legs.size() >= 2 && r.legs.size() <= kMaxRollLegs && r.symbol.empty() && r.side == Side::Buy &&
      !r.client_order_id.empty() && r.quantity > 0 &&
      ((r.type == OrderType::Market && immediate(r) && !r.limit_price) ||
       (r.type == OrderType::Limit && r.limit_price && r.tif >= TimeInForce::Day && r.tif <= TimeInForce::Gtd));
  if (!shape)
    return failure(Reason::INVALID_ORDER, "Multi-leg orders take two to four legs (eight for a roll), a unit quantity and a net limit "
                   "(negative for a credit) or market IOC");
  if (r.legs.size() > kMaxLegs) {
    // Past four legs, a roll: every leg but four closes held contracts, in full for its units.
    const auto opening = std::count_if(r.legs.begin(), r.legs.end(), [&](const Leg& leg) {
      const auto q = held(s, leg.symbol);
      return q == 0 || (q > 0) == (leg.side == Side::Buy) || magnitude(q) < o.remaining() * leg.ratio;
    });
    if (static_cast<std::size_t>(opening) > kMaxLegs)
      return {Reason::INVALID_ORDER, "Orders past four legs are rolls: at most four legs may open, the rest close held contracts",
              static_cast<double>(opening), static_cast<double>(kMaxLegs), {}};
  }
  if (const auto d = tif_check(s, r, o.accepted_at); !d.ok()) return d;
  std::set<std::string> seen;
  const md::OptionContract* first = nullptr;
  Money tick;
  for (const auto& leg : r.legs) {
    if (leg.ratio < 1 || leg.ratio > kMaxRatio || (leg.side != Side::Buy && leg.side != Side::Sell) || !seen.insert(leg.symbol).second)
      return failure(Reason::INVALID_ORDER, "Each leg needs its own contract, a side and a ratio from 1 to 10");
    const auto c = s.contracts.find(leg.symbol);
    if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Every leg must reference a registered canonical OSI definition");
    if (first && c->second.underlying != first->underlying) return failure(Reason::INVALID_ORDER, "All legs must share one underlying");
    first = &c->second;
    if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "A leg's contract has expired");
    if (auto d = session_check(c->second, s.time, r); !d.ok()) { d.scope = leg.symbol; return d; }
    if (r.quantity > s.config.limits.max_order_contracts / leg.ratio)
      return {Reason::MAX_ORDER_CONTRACTS, "A leg's contract count exceeds the order limit",
              static_cast<double>(r.quantity) * static_cast<double>(leg.ratio), static_cast<double>(s.config.limits.max_order_contracts), leg.symbol};
    const auto leg_tick = tick_size(c->second.root, Money{});
    tick = tick == Money{} ? leg_tick : std::min(tick, leg_tick);
  }
  if (r.limit_price && r.limit_price->micros() % tick.micros() != 0)
    return failure(Reason::INVALID_TICK, "Net price is not a multiple of the legs' smallest tick");
  if (const auto d = walk_check(o, tick, tick); !d.ok()) return d;
  // A conditional combo may also wait for another underlying, a study or the clock;
  // its exits watch only its own net or underlying.
  const auto trigger_ok = [](const std::optional<Trigger>& t, bool exit) {
    return !t || (t->source != TriggerSource::Option && valid_trigger(*t) && (!exit || !conditional(*t)));
  };
  const auto exit_ok = [&](const std::optional<ExitSpec>& e) {
    return !e || ((e->trigger || e->limit_price) && trigger_ok(e->trigger, true));
  };
  if (!trigger_ok(r.trigger, false) || (r.bracket && ((!r.bracket->stop_loss && !r.bracket->take_profit) ||
      !exit_ok(r.bracket->stop_loss) || !exit_ok(r.bracket->take_profit))))
    return failure(Reason::INVALID_ORDER, "Combo exits need a combo/underlying trigger, a signed net limit, or both");
  if (!trail_terms_ok(r))
    return failure(Reason::INVALID_ORDER, "A trigger reads the bid/ask, mid or mark (spot for the underlying) and trails by a positive "
                   "amount, a percentage under 100 or 1 to 1,000 ticks (not ticks of the underlying)");
  if (const auto d = conditional_terms_check(s, r); !d.ok()) return d;
  const auto on_tick = [&](const std::optional<ExitSpec>& e) {
    return !e || !e->limit_price || e->limit_price->micros() % tick.micros() == 0;
  };
  if (r.bracket && (!on_tick(r.bracket->stop_loss) || !on_tick(r.bracket->take_profit)))
    return failure(Reason::INVALID_TICK, "An exit's net price is not a multiple of the legs' smallest tick");
  if ((r.trigger || r.exits_only) && !closing_only(s, o))
    return failure(Reason::INVALID_ORDER, "Conditional combos and held exits must close held legs in ratio without exceeding them");
  if (r.exits_only) {
    if (!r.bracket || (r.type == OrderType::Limit && !good_until(r) && !extended(r)))
      return failure(Reason::INVALID_ORDER, "Held exits require a bracket and GTC for a limit exit");
    const auto& primary = r.bracket->take_profit ? *r.bracket->take_profit : *r.bracket->stop_loss;
    if (r.trigger != primary.trigger || r.limit_price != primary.limit_price)
      return failure(Reason::INVALID_ORDER, "Held exit terms must match the take-profit, or the stop when there is no target");
    return system_check(s, o, false);
  }
  if (r.bracket && std::any_of(r.legs.begin(), r.legs.end(), [&](const Leg& leg) {
        const auto q = held(s, leg.symbol);
        return q != 0 && (q > 0) != (leg.side == Side::Buy);
      }))
    return failure(Reason::INVALID_ORDER, "Entry brackets require opening legs; attach exits to held closing legs instead");
  if (rules.buy_only && !closing_only(s, o))
    return failure(Reason::BUY_ONLY, "This plan is buy-only; multi-leg orders may only close held positions");
  {
    std::vector<std::pair<std::string, Quantity>> legs;
    for (const auto& leg : r.legs) {
      const auto q = o.remaining() * leg.ratio;
      legs.emplace_back(leg.symbol, leg.side == Side::Buy ? q : -q);
    }
    if (auto d = account_type_check(s, legs, {}, o.id); !d.ok()) return d;
    if (auto d = defined_risk_check(s, legs); !d.ok()) return d;
  }
  for (const auto& leg : r.legs) {
    if (rules.expiry_cutoff > 0 && s.time >= s.contracts.at(leg.symbol).last_trade_time() - rules.expiry_cutoff && !closing_only(s, o))
      return failure(Reason::EXPIRY_CUTOFF, "A leg is inside the pre-expiry cutoff; only closing orders are accepted");
    if (auto d = quote_check(s, leg.symbol); !d.ok()) { d.scope = leg.symbol; return d; }
  }
  const auto net = *executable_net(s, o, stress_in_band(s));
  Money middle, gross;
  for (const auto& leg : r.legs) {
    const auto value = mid(s.books.at(leg.symbol).quote) * leg.ratio;
    middle = leg.side == Side::Buy ? middle + value : middle - value;
    gross = gross + value;
  }
  const bool limited = stage != Stage::Fill && r.limit_price;
  const auto level = limited ? stop_level(o) : std::nullopt;
  const auto price = limited ? *r.limit_price : net;
  const auto center = level ? *level : middle;
  const long double difference = std::abs(static_cast<long double>(price.micros()) - static_cast<long double>(center.micros()));
  const long double band = std::max(static_cast<long double>(s.config.limits.price_band_absolute.micros()),
      static_cast<long double>(s.config.limits.price_band_relative) * static_cast<long double>(gross.micros()));
  if (!(limited && stage == Stage::Activate) && difference > band)
    return {Reason::PRICE_BAND, level ? "Net limit is outside the configured band around the trigger level"
                                      : "Net price is outside the configured band around the net mid",
            static_cast<double>(difference / 1'000'000), static_cast<double>(band / 1'000'000), first->underlying};
  if (stage == Stage::Accept && r.walk &&
      std::abs(static_cast<long double>(r.walk->limit.micros()) - middle.micros()) > band)
    return failure(Reason::PRICE_BAND, "The walk cap is outside the configured band around the net mid");
  const auto snapshot = measure(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (const auto d = hedging_check(s, o); !d.ok()) return d;
  if (const auto d = contracts_held_check(s, o); !d.ok()) return d;
  if (stage == Stage::Accept)
    if (const auto d = trade_rules_check(s, o, snapshot.equity); !d.ok()) return d;
  if (!closing_only(s, o))
    if (const auto d = loss_check(s, snapshot); !d.ok()) return d;
  if (const auto d = exposure_check(s, o, snapshot); !d.ok()) return d;
  if (rules.buying_power && stage != Stage::Fill) {
    const auto power = buying_power(s, o.id);
    if (power.focus_uses && power.total.available < Money{} && !fill_frees_power(s, o))
      return {Reason::BUYING_POWER, power.focus_opening > 0 ? "Order needs more buying power than the account has available"
                : "Closing these legs uncovers a short option they protect; close that short too, or first",
              power.focus_reservation.dollars(), (power.total.available + power.focus_reservation).dollars(), first->underlying};
  }
  return {};
}
/// An order naming a trade to join: it must open, and name a round trip held now
/// on its underlying, or a whole trade holding one, by the trades view's ID.
Decision group_check(const State& s, const OrderRequest& r) {
  if (r.exits_only) return failure(Reason::INVALID_GROUP, "Held exits open nothing to join to a trade");
  const auto symbols = order_symbols(r);
  const auto contract = s.contracts.find(symbols.front());
  if (contract == s.contracts.end()) return {};
  const auto check = [&](const std::string& symbol, const Lifecycle& life) -> std::optional<Decision> {
    if (std::to_string(life.first_fill) != r.group && trade_group(life, s.groups) != r.group) return std::nullopt;
    if (s.contracts.at(symbol).underlying != contract->second.underlying)
      return failure(Reason::INVALID_GROUP, "A trade's round trips share one underlying");
    return Decision{};
  };
  if (s.reviewing.ready) {
    for (const auto& [symbol, open] : s.reviewing.builder.open)
      if (const auto d = check(symbol, open.life)) return *d;
  } else {
    for (const auto& life : lifecycles(s.fills, s.closures, s.contracts))
      if (!life.closed) if (const auto d = check(life.symbol, life)) return *d;
  }
  return failure(Reason::INVALID_GROUP, "group must name an open round trip, or a whole trade holding one, by its trade ID");
}
}  // namespace
Decision hold_age_check(const State& s, Timestamp opened, const std::string& symbol) {
  const auto required = s.config.rules.min_hold_seconds;
  const auto elapsed = std::max<Timestamp>(0, s.time - opened);
  if (required == 0 || elapsed >= required * md::kNanosPerSecond) return {};
  const double seconds = static_cast<double>(elapsed) / md::kNanosPerSecond;
  std::array<char, 32> text{};  // Refusals are below the 3600-second rule maximum.
  const auto result = std::to_chars(text.data(), text.data() + text.size(), seconds, std::chars_format::fixed, 1);
  std::string held(text.data(), result.ptr);
  if (held.ends_with(".0")) held.resize(held.size() - 2);
  return {Reason::MIN_HOLD, "Held " + held + (held == "1" ? " second" : " seconds") + "; this plan requires " +
          std::to_string(required) + (required == 1 ? " second" : " seconds") + " before a user reduction",
          seconds, static_cast<double>(required), symbol};
}
namespace {
Decision min_hold_check(const State& s, const Order& o, Stage stage) {
  if (s.config.rules.min_hold_seconds == 0 || o.system || kept_within(o) || o.request.exits_only) return {};
  const bool paired = o.oco != 0 || !o.request.oco.empty();
  if (!s.evaluation.revised_rules) {
    if (o.request.trigger || paired) return {};
  } else if (const auto symbols = order_symbols(o.request); std::any_of(symbols.begin(), symbols.end(),
                 [&](const std::string& symbol) { return !s.contracts.contains(symbol); })) {
    return {};  // Refused below as an unregistered contract: it holds nothing to close.
  } else if (o.request.trigger) {
    // A condition not yet met is checked when it triggers. There a protective stop
    // closes as any stop does, and any other triggered close is a user reduction.
    if (stage == Stage::Accept && !reached(s, o)) return {};
    Order triggered = o;
    triggered.triggered_at = std::max<Timestamp>(1, s.time);
    if (stage != Stage::Accept && closing_stop(s, triggered)) return {};
  } else if (paired) {
    // A resting OCO leg closes like a bracket exit; one marketable when accepted is
    // an immediate close like any other.
    if (stage != Stage::Accept) return {};
    if (o.request.type == OrderType::Limit && o.request.limit_price) {
      if (multi_leg(o.request)) {
        if (const auto net = executable_net(s, o); net && *net > *o.request.limit_price) return {};
      } else if (const auto book = s.books.find(o.request.symbol); book != s.books.end() &&
                 valid_quote(book->second.quote) && !marketable(o, book->second.quote)) return {};
    }
  }
  const auto closes = [&](const Lifecycle& life) {
    if (life.closed) return false;
    const auto opposite = [&](const std::string& symbol, Side side) {
      return life.symbol == symbol && (life.quantity > 0) != (side == Side::Buy);
    };
    if (!multi_leg(o.request)) return opposite(o.request.symbol, o.request.side);
    return std::any_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& l) { return opposite(l.symbol, l.side); });
  };
  if (s.reviewing.ready) {
    for (const auto& [symbol, open] : s.reviewing.builder.open)
      if (closes(open.life)) if (const auto d = hold_age_check(s, open.life.opened, symbol); !d.ok()) return d;
  } else {
    for (const auto& life : lifecycles(s.fills, s.closures, s.contracts))
      if (closes(life)) if (const auto d = hold_age_check(s, life.opened, life.symbol); !d.ok()) return d;
  }
  return {};
}
/// Adding to a leg of a trade over the open-loss cap: under BlockAdds refused until
/// an observation finds the trade back within the cap, and under Flatten while its
/// close is pending. Reductions always work.
Decision open_loss_check(const State& s, const Order& o) {
  const auto& cap = s.config.rules.open_loss_cap;
  const auto& over = s.evaluation.open_loss_symbols;
  if (!cap || cap->action == OpenLossAction::Fail || over.empty() || o.system || kept_within(o)) return {};
  const auto adds = [&](const std::string& symbol, Side side) {
    const auto q = held(s, symbol);
    return q != 0 && (q > 0) == (side == Side::Buy) && std::find(over.begin(), over.end(), symbol) != over.end();
  };
  std::vector<std::pair<std::string, Side>> legs;
  if (multi_leg(o.request)) for (const auto& leg : o.request.legs) legs.emplace_back(leg.symbol, leg.side);
  else legs.emplace_back(o.request.symbol, o.request.side);
  for (const auto& [symbol, side] : legs)
    if (adds(symbol, side))
      return {Reason::OPEN_LOSS_CAP, cap->action == OpenLossAction::Flatten
              ? "This position's trade reached the plan's open-loss cap and is being closed; it cannot be added to"
              : "This position's open loss is at the plan's open-loss cap; it can be reduced or closed, "
                "but not added to, until it is back within the cap", {}, {}, symbol};
  return {};
}
/// The median of the recent opening sizes the size-creep guardrail compares with,
/// once it has three of them.
std::optional<double> size_median(const GuardrailState& g) {
  if (g.opening_sizes.size() < 3) return std::nullopt;
  auto sizes = g.opening_sizes;
  std::sort(sizes.begin(), sizes.end());
  const auto n = sizes.size();
  return n % 2 ? static_cast<double>(sizes[n / 2]) : (static_cast<double>(sizes[n / 2 - 1]) + static_cast<double>(sizes[n / 2])) / 2;
}
}  // namespace
/// An opening order's size for size creep: its contracts, every leg counted.
Quantity opening_size(const OrderRequest& r) {
  if (!multi_leg(r)) return r.quantity;
  Quantity ratios = 0;
  for (const auto& leg : r.legs) ratios += leg.ratio;
  Quantity size = 0;
  if (__builtin_mul_overflow(r.quantity, ratios, &size)) return std::numeric_limits<Quantity>::max();
  return size;
}
std::optional<SizeCreep> size_creep(const State& s, const Order& o) {
  const auto& g = s.config.guardrails;
  if (g.size_creep_percent == 0 || o.system || kept_within(o) || o.request.exits_only || closing_only(s, o)) return std::nullopt;
  const auto median = size_median(s.guardrails);
  if (!median) return std::nullopt;
  const auto size = opening_size(o.request);
  const double limit = *median * static_cast<double>(g.size_creep_percent) / 100;
  if (static_cast<double>(size) <= limit) return std::nullopt;
  return SizeCreep{size, *median, limit};
}
std::string size_creep_message(const SizeCreep& c, std::int64_t percent) {
  return std::to_string(c.size) + " contracts is more than " + std::to_string(percent) + "% of the median of your recent opening orders (" +
         decimal(c.median, c.median == std::floor(c.median) ? 0 : 1) + ")";
}
namespace {
Decision discipline_check(const State& s, const Order& o, Stage stage) {
  if (stage != Stage::Accept || !s.config.guardrails.size_creep_block) return {};
  if (const auto creep = size_creep(s, o))
    return {Reason::SIZE_CREEP, size_creep_message(*creep, s.config.guardrails.size_creep_percent) +
            "; your size-creep guardrail refuses it", static_cast<double>(creep->size), creep->limit, "account"};
  return {};
}
}  // namespace
Decision order_check(const State& s, const Order& o, Stage stage) {
  if (const auto d = min_hold_check(s, o, stage); !d.ok()) return d;
  if (const auto d = account_check(s, closing_only(s, o), o.id); !d.ok()) return d;
  if (const auto d = entry_check(s, o); !d.ok()) return d;
  if (const auto d = open_loss_check(s, o); !d.ok()) return d;
  if (const auto d = discipline_check(s, o, stage); !d.ok()) return d;
  try { (void)clean_annotation(o.request.note, o.request.tags); }
  catch (const TradingError& e) { return failure(e.code(), e.what()); }
  if (stage == Stage::Accept && !o.request.group.empty())
    if (const auto d = group_check(s, o.request); !d.ok()) return d;
  if (multi_leg(o.request)) return combo_check(s, o, stage);
  const auto& rules = s.config.rules;
  const auto& request = o.request;
  const auto c = s.contracts.find(request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (const auto d = session_check(c->second, s.time, request); !d.ok()) return d;
  if (request.client_order_id.empty() || request.quantity <= 0 ||
      (request.side != Side::Buy && request.side != Side::Sell) ||
      (request.type != OrderType::Market && request.type != OrderType::Limit) ||
      (request.tif < TimeInForce::Day || request.tif > TimeInForce::Gtd) ||
      (request.type == OrderType::Market && (!immediate(request) || request.limit_price)) ||
      (request.type == OrderType::Limit && (!request.limit_price || *request.limit_price <= Money{})))
    return failure(Reason::INVALID_ORDER, "Positive quantity/client ID required; market is IOC without price; limit requires positive price");
  if (const auto d = tif_check(s, request, o.accepted_at); !d.ok()) return d;
  if (request.quantity > s.config.limits.max_order_contracts)
    return {Reason::MAX_ORDER_CONTRACTS, "Order contract count exceeds limit", static_cast<double>(request.quantity),
            static_cast<double>(s.config.limits.max_order_contracts), request.symbol};
  if (request.limit_price && request.limit_price->micros() % tick_size(c->second.root, *request.limit_price).micros() != 0)
    return failure(Reason::INVALID_TICK, "Limit price is not a positive multiple of the product tier tick");
  if (request.walk) {
    if (const auto d = walk_check(o, tick_size(c->second.root, Money{}), tick_size(c->second.root, request.walk->limit)); !d.ok())
      return d;
  }
  const auto positive = [](const std::optional<Trigger>& t) { return !t || valid_trigger(*t); };
  // An exit watches its own option or underlying; an order may watch more.
  const auto exit_ok = [&](const std::optional<ExitSpec>& e) {
    return !e || ((e->trigger || e->limit_price) && positive(e->trigger) && (!e->trigger || !conditional(*e->trigger)) &&
                  (!e->limit_price || *e->limit_price > Money{}));
  };
  const auto& bracket = request.bracket;
  if ((request.trigger && request.trigger->source == TriggerSource::Combo) ||
      (bracket && ((bracket->stop_loss && bracket->stop_loss->trigger && bracket->stop_loss->trigger->source == TriggerSource::Combo) ||
                   (bracket->take_profit && bracket->take_profit->trigger && bracket->take_profit->trigger->source == TriggerSource::Combo))) ||
      !positive(request.trigger) ||
      (bracket && (!exit_ok(bracket->stop_loss) || !exit_ok(bracket->take_profit) || (!bracket->stop_loss && !bracket->take_profit))))
    return failure(Reason::INVALID_ORDER, "Trigger levels and exit prices must be positive; each exit takes a trigger, a limit price or both");
  if (!trail_terms_ok(request))
    return failure(Reason::INVALID_ORDER, "A trigger reads the bid/ask, mid or mark (spot for the underlying) and trails by a positive "
                   "amount, a percentage under 100 or 1 to 1,000 ticks (not ticks of the underlying)");
  if (const auto d = conditional_terms_check(s, request); !d.ok()) return d;
  const auto on_tick = [&](const std::optional<ExitSpec>& e) {
    return !e || !e->limit_price || e->limit_price->micros() % tick_size(c->second.root, *e->limit_price).micros() == 0;
  };
  if (bracket && (!on_tick(bracket->stop_loss) || !on_tick(bracket->take_profit)))
    return failure(Reason::INVALID_TICK, "An exit's limit price is not a positive multiple of the product tier tick");
  if (request.exits_only) {
    // A held contract's exits, as a held spread's: they only close, so they take
    // an exit's checks rather than an entry's.
    if (!closing_only(s, o))
      return failure(Reason::INVALID_ORDER, "Held exits must close a held position without exceeding it, after the other "
                     "working orders that close it");
    if (!bracket || (request.type == OrderType::Limit && !good_until(request) && !extended(request)))
      return failure(Reason::INVALID_ORDER, "Held exits require a bracket and GTC for a limit exit");
    const auto& primary = bracket->take_profit ? *bracket->take_profit : *bracket->stop_loss;
    if (request.trigger != primary.trigger || request.limit_price != primary.limit_price)
      return failure(Reason::INVALID_ORDER, "Held exit terms must match the take-profit, or the stop when there is no target");
    return system_check(s, o, false);
  }
  if (rules.buy_only && request.side == Side::Sell && !closing_only(s, o))
    return failure(Reason::BUY_ONLY, "This plan is buy-only: sells may only close contracts you already hold");
  if (auto d = account_type_check(s, {{request.symbol, request.side == Side::Buy ? o.remaining() : -o.remaining()}}, {}, o.id); !d.ok())
    return d;
  if (auto d = defined_risk_check(s, {{request.symbol, request.side == Side::Buy ? o.remaining() : -o.remaining()}}); !d.ok())
    return d;
  if (rules.expiry_cutoff > 0 && s.time >= c->second.last_trade_time() - rules.expiry_cutoff && !closing_only(s, o))
    return failure(Reason::EXPIRY_CUTOFF, "Contract is inside the pre-expiry cutoff; only closing orders are accepted");
  // A buy that only closes a short can take a quote that shows only an ask (ask_only_single).
  if (const auto d = quote_check(s, request.symbol); !d.ok() && !ask_only_single(s, o)) return d;
  const auto& quote = s.books.at(request.symbol).quote;
  if (stage == Stage::Accept && request.walk) {
    if (const auto d = price_check(s, quote, request.walk->limit); !d.ok()) return d;
  }
  if (stage == Stage::Fill || !request.limit_price) {
    const auto price = order_price(s, o, request.symbol, request.side, request.limit_price, 0, stress_in_band(s));
    if (const auto d = price_check(s, quote, price); !d.ok()) return d;
  } else if (stage == Stage::Accept) {
    if (const auto d = price_check(s, quote, *request.limit_price, stop_level(o)); !d.ok()) return d;
  }
  const auto snapshot = measure(s);
  if (!snapshot.valuation_complete) return failure(Reason::STALE_QUOTE, "All held positions need fresh marks before trading");
  if (const auto d = hedging_check(s, o); !d.ok()) return d;
  if (const auto d = contracts_held_check(s, o); !d.ok()) return d;
  if (stage == Stage::Accept)
    if (const auto d = trade_rules_check(s, o, snapshot.equity); !d.ok()) return d;
  if (!closing_only(s, o))
    if (const auto d = loss_check(s, snapshot); !d.ok()) return d;
  if (const auto d = exposure_check(s, o, snapshot); !d.ok()) return d;
  if (rules.buying_power && stage != Stage::Fill) {
    // Orders that free buying power are always allowed, including one whose fill
    // lowers other working orders' reservations; fills recheck against the
    // projected ledger instead.
    const auto power = buying_power(s, o.id);
    if (power.focus_uses && power.total.available < Money{} && !fill_frees_power(s, o))
      return {Reason::BUYING_POWER, power.focus_opening > 0 ? "Order needs more buying power than the account has available"
                : "Selling this long uncovers a short option it protects; buy the short back first, or close both together as one order",
              power.focus_reservation.dollars(), (power.total.available + power.focus_reservation).dollars(), request.symbol};
  }
  return {};
}
/// Liquidation and expiry auto-close reduce risk, so only the contract,
/// session and executable-quote gates apply, including under the kill latch;
/// so do bracket exits and reduce-only closes, which stay within their position.
/// Held exits are accepted outside the regular session (`now` false) and trade
/// in the next one.
Decision system_check(const State& s, const Order& o, bool now) {
  if (multi_leg(o.request)) {
    if (!closing_only(s, o, false)) return failure(Reason::POSITION_CLOSED, "Exit legs no longer fit the held positions");
    std::vector<std::pair<std::string, Quantity>> legs;
    for (const auto& leg : o.request.legs) {
      const auto& c = s.contracts.at(leg.symbol);
      if (s.time >= c.last_trade_time()) return failure(Reason::EXPIRED, "An exit leg reached its last trade");
      if (now && (o.system ? !regular(c, s.time) : !trades_now(o, c, s.time)))
        return failure(Reason::SESSION_CLOSED, "Combo exits wait for the regular session");
      if (auto d = quote_check(s, leg.symbol); !d.ok() && !ask_only(s, o, leg)) return d;
      legs.emplace_back(leg.symbol, signed_contracts(leg, o.remaining()));
    }
    if (auto d = account_type_check(s, legs, {}, o.id); !d.ok()) return d;
    return defined_risk_check(s, legs);
  }
  const auto c = s.contracts.find(o.request.symbol);
  if (c == s.contracts.end()) return failure(Reason::UNKNOWN_CONTRACT, "Order must reference a registered canonical OSI definition");
  if (s.time >= c->second.expiry_time()) return failure(Reason::EXPIRED, "Contract has expired");
  if (now && (o.system ? !regular(c->second, s.time) : !trades_now(o, c->second, s.time)))
    return failure(Reason::SESSION_CLOSED, "Closing orders the account places itself and bracket exits trade in the regular session only");
  if (!o.system)
    if (auto d = account_type_check(s, {{o.request.symbol, o.request.side == Side::Buy ? o.remaining() : -o.remaining()}}, {}, o.id); !d.ok())
      return d;
  // A bracket exit keeps a defined-risk plan's shorts covered; the account's own closing orders need not.
  if (!o.system && o.request.side == Side::Sell)
    if (auto d = defined_risk_check(s, {{o.request.symbol, -o.remaining()}},
                                    "Selling this long would leave a short option uncovered; close the short first");
        !d.ok())
      return d;
  if (ask_only_single(s, o)) return {};
  return quote_check(s, o.request.symbol);
}
bool marketable(const Order& o, const QuoteObservation& q) {
  if (o.request.type == OrderType::Market) return true;
  return o.request.side == Side::Buy ? *q.ask <= *o.request.limit_price : *q.bid >= *o.request.limit_price;
}
namespace {
/// Both submit and preview check the candidate while its reservation is present.
/// Orders in a request and its chained orders.
std::size_t chain_size(const OrderRequest& r) {
  std::size_t size = 1;
  for (const auto& next : r.then) size += chain_size(next);
  for (const auto& other : r.oco) size += chain_size(other);
  return size;
}
/// A chain's shape: at most one order placed by a complete fill and one that
/// cancels it, an oco order taking neither oco nor exits_only, nor any chained
/// order exits_only, nor a held exit any chain; four orders in all.
bool chain_ok(const OrderRequest& r, bool chained = false) {
  if (r.then.size() > 1 || r.oco.size() > 1 || (r.exits_only && (chained || !r.then.empty() || !r.oco.empty())))
    return false;
  if (!r.oco.empty() && !r.oco.front().oco.empty()) return false;
  for (const auto& next : r.then) if (!chain_ok(next, true)) return false;
  for (const auto& other : r.oco) if (!chain_ok(other, true)) return false;
  return chained || chain_size(r) <= kMaxChain;
}
}  // namespace
Decision acceptance_check(const State& s, const Order& candidate, const Decision& rejection) {
  // Another order used this ID first.
  if (const auto first = s.clients.find(candidate.request.client_order_id);
      first != s.clients.end() && first->second != candidate.id)
    return failure(Reason::DUPLICATE_CLIENT_ID, "client_order_id already used");
  if (!rejection.ok()) return rejection;
  if (!chain_ok(candidate.request))
    return failure(Reason::INVALID_ORDER, "A chain is at most four orders: an order takes at most one order placed when it fills "
                   "(then) and one that cancels it (oco, which takes none of its own); chained orders are not held exits");
  auto decision = order_check(s, candidate);
  return decision.ok() ? open_orders_risk_check(s, candidate) : decision;
}
/// The checks a resting entry's new terms take in change_order, on `s`, where the order has them.
Decision change_check(const State& s, const Order& order, const Decision& rejection) {
  if (!rejection.ok()) return rejection;
  auto decision = order_check(s, order);
  return decision.ok() ? open_orders_risk_check(s, order) : decision;
}
}  // namespace session_internal
}  // namespace openport::trading
