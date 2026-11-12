// What an account warns of: why an order waits, the risk of breaching the plan, risk
// warnings, and the trader's alerts.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
/// New York wall time of a market timestamp, for messages: "10:31:05 ET".
std::string clock_text(Timestamp time) {
  const auto seconds = md::new_york_time(time).seconds;
  const auto two = [](int value) { return std::string(value < 10 ? "0" : "") + std::to_string(value); };
  return two(seconds / 3600) + ":" + two(seconds / 60 % 60) + ":" + two(seconds % 60) + " ET";
}
/// What keeps open order `o` from filling now (OrderWait), checked in the order
/// matching checks it; nothing when nothing visible does. `data_complete`: the held
/// positions' marks and every valuation risk needs are fresh.
std::optional<OrderWait> waiting_for(const State& s, const Order& o, bool data_complete) {
  const auto wait = [](std::string code, std::string message) { return std::optional<OrderWait>(OrderWait{std::move(code), std::move(message)}); };
  const auto& r = o.request;
  const auto symbols = order_symbols(r);
  for (const auto& symbol : symbols) if (!s.contracts.contains(symbol)) return std::nullopt;
  for (const auto& symbol : symbols)
    if (s.time < halt_end(s, s.contracts.at(symbol).underlying))
      return wait("MARKET_HALTED", "Underlying trading is halted; waits for reopening quotes");
  if (o.status == OrderStatus::Armed) {
    if (extended(r) && !trades_now(o, s.contracts.at(symbols.front()), s.time))
      return wait("SESSION_CLOSED", "Waits for the next trading session");
    if (!extended(r) && !regular(s.contracts.at(symbols.front()), s.time))
      return wait("REGULAR_SESSION", "Armed orders trigger in the regular session only");
    const auto& t = *r.trigger;
    if (t.source == TriggerSource::Time) {
      const auto two = [](std::int64_t value) { return std::string(value < 10 ? "0" : "") + std::to_string(value); };
      return wait("TRIGGER", "Waits for " + two(t.minute / 60) + ":" + two(t.minute % 60) + " ET" +
                  (t.direction == TriggerDirection::AtOrBelow ? " or earlier" : ""));
    }
    const auto value = trigger_value(s, o);
    const auto watched = t.symbol.empty() ? std::string("the underlying") : t.symbol;
    const std::string reference = t.reference == TriggerReference::Mid ? "mid" : t.reference == TriggerReference::Mark ? "mark" : "";
    const std::string source = t.source == TriggerSource::Study ? watched + "'s " + t.study
        : t.source == TriggerSource::Underlying ? watched : t.source == TriggerSource::Combo
        ? "the closing legs' " + (reference.empty() ? std::string("net") : reference + " net")
        : !reference.empty() ? "the " + reference : r.side == Side::Buy ? "the ask" : "the bid";
    return wait("TRIGGER", "Waits for " + source + " to reach " + (t.direction == TriggerDirection::AtOrBelow ? "at or below " : "at or above ") +
                t.level.str() + (t.trail ? " (a trailing level)" : "") +
                (value ? " (now " + value->str() + ")" : " (no fresh value now; missing or stale data never triggers)"));
  }
  if (persistent(o) || o.system) {
    for (const auto& symbol : symbols) {
      if (extended(r) && !trades_now(o, s.contracts.at(symbol), s.time))
        return wait("SESSION_CLOSED", "Waits for the next trading session");
      else if (!extended(r) && !regular(s.contracts.at(symbol), s.time))
        return wait("REGULAR_SESSION", std::string(r.tif == TimeInForce::Gtc && o.role == OrderRole::Normal ? "GTC orders" : "Bracket exits and triggered orders") +
                    " fill in the regular session only");
    }
  }
  for (const auto& symbol : symbols) {
    const auto leg = std::find_if(r.legs.begin(), r.legs.end(), [&](const Leg& l) { return l.symbol == symbol; });
    if (const auto d = quote_check(s, symbol); !d.ok() &&
        !(leg != r.legs.end() ? ask_only(s, o, *leg) : ask_only_single(s, o)))
      return wait(d.code == Reason::STALE_QUOTE ? "STALE_QUOTE" : "INVALID_QUOTE",
                  d.code == Reason::STALE_QUOTE ? symbol + "'s quote is older than the freshness window"
                                                : symbol + " has no two-sided quote with sizes; a one-sided book supplies no liquidity");
  }
  if (!latency_ready(s, o)) {
    const auto start = std::max(o.accepted_at, o.triggered_at);
    return wait("FILL_LATENCY", "Fill latency holds it for a quote stamped at or after " +
                clock_text(start + s.config.rules.fill_latency_ms * (md::kNanosPerSecond / 1000)));
  }
  for (const auto& symbol : symbols)
    if (s.books.at(symbol).quote.time < o.accepted_at)
      return wait("NEWER_QUOTE", "Waits for a quote newer than its acceptance at " + clock_text(o.accepted_at));
  const bool impact = s.config.rules.impact_ticks > 0;
  if (!multi_leg(r)) {
    const auto& book = s.books.at(r.symbol);
    const bool buy = r.side == Side::Buy;
    const bool given = !buy && ask_only_single(s, o);
    const bool inside = inside_single(s, o);
    // A queue spot counts only on a two-sided quote: an ask-only book that lets a
    // buy-to-close through has no bid for touch_of to read.
    if (!given && !inside && !marketable(o, book.quote) && valid_quote(book.quote) && queue_eligible(s, o) && o.queue &&
        o.queue->price == *r.limit_price && touch_of(book.quote, r.side, *r.limit_price) != Touch::Behind)
      return wait("QUEUE", "Waits behind an estimated " + std::to_string(o.queue->ahead) + " contracts at " +
                  r.limit_price->str() + " until trades use them up, or the far side reaches the limit");
    if (!given && !inside && !marketable(o, book.quote))
      return wait("LIMIT", std::string(buy ? "The ask " : "The bid ") + (buy ? *book.quote.ask : *book.quote.bid).str() +
                  (buy ? " is above" : " is below") + " the limit " + r.limit_price->str());
    if (impact && r.limit_price && !inside) {
      const auto next = execution_price(s, r.symbol, r.side);
      if (buy ? next > *r.limit_price : next < *r.limit_price)
        return wait("LIMIT", "The next simulated block's price " + next.str() + " is beyond the limit " + r.limit_price->str());
    }
    if (!given && (!impact || inside) && (buy ? book.ask_left : book.bid_left) <= 0)
      return wait("DISPLAYED_SIZE", "Paper orders used this quote's displayed size; a new quote refreshes it");
  } else {
    const auto net = executable_net(s, o);
    if (!net) return wait("INVALID_QUOTE", "A leg has no executable quote");
    if (r.limit_price && *net > *r.limit_price)
      return wait("LIMIT", "The net at the far sides, " + net->str() + ", is worse than the limit " + r.limit_price->str());
    if (!impact || inside_combo(s, o))
      for (const auto& leg : r.legs) {
        if (given_away(s, o, leg)) continue;
        const auto& book = s.books.at(leg.symbol);
        if ((leg.side == Side::Buy ? book.ask_left : book.bid_left) / leg.ratio <= 0)
          return wait("DISPLAYED_SIZE", "Paper orders used " + leg.symbol + "'s displayed size; a new quote refreshes it");
      }
  }
  if (!data_complete) return wait("STALE_DATA", "Held positions need fresh marks and valuations before it can fill");
  return std::nullopt;
}
namespace {
std::map<std::string, double> fresh_stock_prices(const State& s) {
  std::map<std::string, double> prices;
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) prices[symbol] = price->dollars();
  return prices;
}
}  // namespace
BreachRisk breach_of(const State& s, const std::map<std::string, double>& close_variances) {
  const DeliverablesScope adjusted(&s.deliverables);
  const auto snapshot = measure(s);
  auto result = breach_risk(s.ledger, s.valuations, snapshot.equity,
      s.config.rules.max_drawdown > Money{} ? std::optional(s.evaluation.floor) : std::nullopt,
      snapshot.soft_floor, s.time, s.config.limits.max_valuation_age, fresh_stock_prices(s), close_variances);
  result.complete &= snapshot.valuation_complete;
  if (!snapshot.valuation_complete) {
    result.room.reset(); result.soft_room.reset();
    for (auto& underlying : result.underlyings) {
      underlying.complete = false;
      underlying.down.reset(); underlying.up.reset();
      underlying.soft_down.reset(); underlying.soft_up.reset();
    }
  }
  return result;
}
namespace {
/// Whole units with thousands separators, as warnings print exposures.
std::string whole(double value) {
  if (!std::isfinite(value)) return "unavailable";
  std::array<char, std::numeric_limits<double>::max_exponent10 + 3> text{};
  std::snprintf(text.data(), text.size(), "%.0f", std::round(std::abs(value)));
  std::string digits = text.data();
  const bool negative = value < 0 && digits != "0";
  for (auto i = static_cast<std::ptrdiff_t>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<std::size_t>(i), ",");
  return (negative ? "-" : "") + digits;
}
}  // namespace
std::string decimal(double value, int places) {
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), "%.*f", places, value);
  return text.data();
}
namespace {
/// "SPY 2026-10-16 500C", as a warning names a contract.
std::string contract_name(const md::OptionContract& c) {
  const auto strike = std::abs(c.strike - std::round(c.strike)) < 1e-9 ? whole(c.strike) : decimal(c.strike, 2);
  return c.underlying + " " + md::format_date(c.expiry) + " " + strike + (c.type == pricing::OptionType::Call ? "C" : "P");
}
/// The trading date after `day`: the one whose overnight session opens that evening.
md::Date next_trading_day(md::Date day) { return md::trading_date(md::new_york_to_utc(day, 17, 1)); }
/// An option's underlying price: its shares' fresh price, or the valuation's spot.
std::optional<Money> underlying_price(const State& s, const std::string& symbol) {
  if (auto price = stock_price(s, s.contracts.at(symbol).underlying)) return price;
  if (const auto* valuation = valuation_of(s, symbol); valuation && valuation->spot > 0) return Money::from_double(valuation->spot);
  return std::nullopt;
}
}  // namespace
std::vector<RiskWarning> warnings_of(const State& s, const std::map<std::string, double>& close_variances,
                                     const std::vector<Dividend>& dividends, const std::vector<CorporateAction>& splits) {
  std::vector<RiskWarning> out;
  const auto now = measure(s);
  const auto add = [&](std::string code, bool urgent, std::string scope, std::string symbol, std::string message,
                       std::optional<double> actual, std::optional<double> limit) {
    out.push_back({std::move(code), urgent ? "warning" : "info", std::move(scope), std::move(symbol), std::move(message), actual, limit});
  };
  if (now.risk.complete) {
    // Over a limit, orders that raise reserved risk are refused; orders that lower it still go.
    const auto over = [&](const std::string& scope, const RiskBucket& b) {
      const auto name = scope == "aggregate" ? std::string("The account's") : scope + "'s";
      if (const auto delta = std::abs(b.position.dollar_delta); delta > b.limits.dollar_delta)
        add("DELTA_LIMIT", true, scope, {}, name + " dollar delta " + whole(b.position.dollar_delta) + " is over its " +
            whole(b.limits.dollar_delta) + " limit: orders that raise it are refused; closes and hedges that lower it still go",
            delta, b.limits.dollar_delta);
      if (const auto vega = std::abs(b.position.vega); vega > b.limits.vega)
        add("VEGA_LIMIT", true, scope, {}, name + " vega " + whole(b.position.vega) + " is over its " + whole(b.limits.vega) +
            " limit: orders that raise it are refused; closes and hedges that lower it still go", vega, b.limits.vega);
    };
    for (const auto& [name, bucket] : now.risk.underlyings) over(name, bucket);
    over("aggregate", now.risk.aggregate);
    // A move changes dollar delta by gamma and by the price itself: per 1%,
    // dollar gamma plus 1% of dollar delta. How far each underlying can move
    // before its delta reaches its own limit or the account's, to first order.
    for (const auto& [name, bucket] : now.risk.underlyings) {
      const auto& p = bucket.position;
      const double rate = p.dollar_gamma_1pct + 0.01 * p.dollar_delta;
      if (rate == 0 || !std::isfinite(rate)) continue;
      std::optional<double> nearest;
      std::string reached;
      const auto reach = [&](double delta, double limit, const std::string& which) {
        if (std::abs(delta) > limit) return;  // Already over: DELTA_LIMIT says so.
        const double up = ((rate > 0 ? limit : -limit) - delta) / rate;
        const double down = ((rate > 0 ? -limit : limit) - delta) / rate;
        const double move = up <= -down ? up : down;
        if (!nearest || std::abs(move) < std::abs(*nearest)) { nearest = move; reached = which; }
      };
      reach(p.dollar_delta, bucket.limits.dollar_delta, "its " + whole(bucket.limits.dollar_delta) + " limit");
      reach(now.risk.aggregate.position.dollar_delta, now.risk.aggregate.limits.dollar_delta,
            "the account's " + whole(now.risk.aggregate.limits.dollar_delta) + " limit");
      if (!nearest) continue;
      const auto variance = close_variances.find(name);
      const double sigma = variance == close_variances.end() || !(variance->second > 0) ? 0 : 100 * std::sqrt(variance->second);
      const double within = std::max(1.0, sigma);
      if (std::abs(*nearest) > within) continue;
      add("DELTA_HEADROOM", true, name, {}, "A " + decimal(std::abs(*nearest), 2) + "% " + (*nearest > 0 ? "rise" : "fall") + " in " +
          name + " would take its dollar delta to " + reached + " (dollar gamma " + whole(p.dollar_gamma_1pct) +
          " per 1%, a first-order estimate): orders that add to it would then be refused", *nearest, within);
    }
  }
  const auto& rules = s.config.rules;
  const auto& e = s.evaluation;
  if (s.config.guardrails.soft_floor_percent > 0 && rules.max_drawdown == Money{})
    add("SOFT_FLOOR_UNUSED", false, "aggregate", {},
        "Your soft floor percent does nothing on this plan because it has no drawdown floor; use an absolute soft floor instead", {}, {});
  const bool at_soft_floor = now.valuation_complete && now.soft_floor && now.equity <= *now.soft_floor;
  if (at_soft_floor)
    add("SOFT_FLOOR", true, "aggregate", {}, "Equity " + dollars(now.equity) + " is at or below your soft floor " +
        dollars(*now.soft_floor) + ": the soft floor latches at once, closes positions and refuses opening orders. "
        "Lower it in personal guardrails; a lower setting is pending until rollover, and a reset applies pending settings",
        now.equity.dollars(), now.soft_floor->dollars());
  if (now.valuation_complete) {
    // Tonight's close at today's equity: an end-of-day floor ratchets, pending
    // guardrails apply, and a percent soft floor follows the plan floor.
    const auto tomorrow = rules.max_drawdown > Money{} ? evaluation_tomorrow_floor(e, rules, now.equity) : e.floor;
    if (rules.max_drawdown > Money{} && tomorrow > e.floor) {
      const bool locks = rules.lock_balance > Money{} && tomorrow == rules.lock_balance && !e.floor_locked;
      add("FLOOR_RATCHET", false, "aggregate", {}, "Closing at today's equity " + dollars(now.equity) + " raises the floor tonight from " +
          dollars(e.floor) + " to " + dollars(tomorrow) + (locks ? ", where it locks" : "") +
          ": what the day gained is then no longer room to lose", tomorrow.dollars(), e.floor.dollars());
    }
    const auto soft = soft_floor_at(s.pending_guardrails ? *s.pending_guardrails : s.config.guardrails, rules, tomorrow);
    if (!at_soft_floor && soft && now.equity <= *soft)
      add("SOFT_FLOOR_ROLLOVER", true, "aggregate", {}, "At rollover your soft floor becomes " + dollars(*soft) +
          ", at or above today's equity " + dollars(now.equity) + ": unless equity rises, it closes positions on the next trading day",
          now.equity.dollars(), soft->dollars());
  }
  // Held into expiry, American equity and ETF options a cent or more in the money
  // are exercised or assigned and deliver shares, which together cost the strike.
  struct Expiring { std::string symbol; Money price; Money intrinsic; };
  std::vector<Expiring> expiring;
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& c = s.contracts.at(symbol);
    if (position.quantity == 0 || !physical(c) || c.expiry != s.day || s.time >= c.expiry_time()) continue;
    const auto price = underlying_price(s, symbol);
    if (!price) continue;
    if (position.quantity > 0 && s.do_not_exercise.contains(symbol)) {
      expiring.push_back({symbol, *price, Money{}});
      continue;
    }
    const Money strike = effective_strike(s, c);
    const Money value = unit_exercise_value(s, c, *price);
    if (rules.pin_risk && position.quantity < 0 && value >= Money::parse("-0.10") && value <= Money::parse("0.10"))
      add("PIN_RISK", true, c.underlying, symbol,
          "Simulated pin risk: short " + contract_name(c) + " expires today within $0.10 of the strike. "
          "Each contract has a seeded 25% chance of contrary exercise, modelling after-hours uncertainty before the 17:30 ET cutoff. "
          "Out-of-money shorts can deliver shares; in-money shorts may not. No after-hours price is observed or invented.",
          (*price - strike).dollars(), 0.10);
    const Money intrinsic = std::max(Money{}, value);
    if (intrinsic >= Money::from_micros(10'000)) expiring.push_back({symbol, *price, intrinsic});
  }
  if (!expiring.empty()) {
    // Buying power once every one of them has delivered, at today's prices.
    State delivered_state = s;
    for (const auto& x : expiring) {
      const auto& c = delivered_state.contracts.at(x.symbol);
      const auto shares = x.intrinsic > Money{} ? delivered(delivered_state, c, held(delivered_state, x.symbol)) : 0;
      delivered_state.ledger.settle(x.symbol, x.intrinsic, shares != 0 ? exercise_fee(delivered_state, held(delivered_state, x.symbol)) : Money{});
      if (shares != 0) {
        delivered_state.stock_marks[c.underlying] = {x.price, delivered_state.time};
        delivered_state.ledger.trade_stock(c.underlying, shares, x.price, Money{});
      }
    }
    const auto power = buying_power(delivered_state).total.available;
    for (const auto& x : expiring) {
      if (x.intrinsic == Money{}) continue;
      const auto& c = s.contracts.at(x.symbol);
      const auto contracts = held(s, x.symbol);
      const auto shares = delivered(s, c, contracts);
      const Money strike = effective_strike(s, c);
      const auto cutoff = c.last_trade_time() - rules.expiry_cutoff;
      const bool before_cutoff = rules.expiry_cutoff > 0 && s.time < cutoff;
      const auto expiry = before_cutoff
          ? "the account closes the position at its pre-expiry cutoff at " + clock_text(cutoff) +
            " at the market; only contracts still held into expiry because a close cannot fill are "
          : std::string("held into expiry ") + (magnitude(contracts) == 1 ? "it is " : "they are ");
      add("EXPIRY_DELIVERY", power < Money{}, c.underlying, x.symbol,
          std::to_string(magnitude(contracts)) + " " + (contracts > 0 ? "long " : "short ") + contract_name(c) + " expiring today " +
          (magnitude(contracts) == 1 ? "is " : "are ") + dollars(x.intrinsic) + " in the money: " + expiry +
          (contracts > 0 ? "exercised" : "assigned") + ", " +
          (shares > 0 ? "buying " : "selling ") + std::to_string(magnitude(shares)) + " " + c.underlying + " shares at the strike (" +
          dollars(strike * magnitude(shares)) + "). Buying power once the options expiring in the money today deliver: " + dollars(power) +
          (power < Money{} && rules.buying_power
              ? ". Openings that use buying power are refused until the shortfall is resolved; closes that free buying power remain allowed. Close or reduce before expiry to avoid this shortfall"
              : ""),
          power.dollars(), 0.0);
    }
  }
  // Short American equity and ETF options the simulator assigns at a rollover:
  // those trading below their exercise value, and calls with less time value than
  // a dividend going ex on the new day (TradingSession::roll_day).
  const auto next_day = next_trading_day(s.day);
  const auto horizon = md::date_from_days(md::days_since_epoch(s.day) + 7);
  // Splits still to go ex, which split a unit's shares before a dividend after them.
  std::vector<CorporateAction> ahead;
  for (const auto& a : splits)
    if (a.ex_date > s.day) ahead.push_back(a);
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& c = s.contracts.at(symbol);
    if (position.quantity >= 0 || !physical(c) || c.expiry <= s.day) continue;
    const auto mark = s.marks.find(symbol);
    const auto price = underlying_price(s, symbol);
    if (mark == s.marks.end() || !price) continue;
    const bool call = c.type == pricing::OptionType::Call;
    const Money intrinsic = std::max(Money{}, unit_exercise_value(s, c, *price));
    if (intrinsic < Money::from_micros(10'000)) continue;
    const auto name = std::to_string(-position.quantity) + " short " + contract_name(c);
    if (mark->second.price < intrinsic) {
      add("EARLY_ASSIGNMENT", true, c.underlying, symbol, name + " trade" + (position.quantity == -1 ? "s" : "") + " at " +
          dollars(mark->second.price) + ", below " + dollars(intrinsic) + " of exercise value: holders exercise rather than sell, " +
          "and the simulator assigns about half of them at the next rollover", mark->second.price.dollars(), intrinsic.dollars());
      continue;
    }
    if (!call) continue;
    const auto time_value = mark->second.price - intrinsic;
    // What each dividend pays on what a unit delivers, as the rollover weighs it.
    std::map<md::Date, Money> by_date;
    for (const auto& d : dividends)
      if (d.symbol == c.underlying && d.ex_date > s.day && d.ex_date <= std::min(horizon, c.expiry))
        by_date[d.ex_date] = by_date[d.ex_date] + unit_dividend(s, c, d, ahead);
    const bool standard = standard_deliverable(deliverable_of(s, symbol));
    for (const auto& [date, dividend] : by_date) {
      if (time_value >= dividend) continue;
      const bool tonight = date <= next_day;
      add("EARLY_ASSIGNMENT", tonight, c.underlying, symbol, name + " ha" + (position.quantity == -1 ? "s " : "ve ") +
          dollars(time_value) + " of time value, less than " +
          (standard ? "the " + dollars(dividend) + " dividend" : dollars(dividend) + " a unit, what its deliverable is paid of the dividend") +
          " going ex on " + md::format_date(date) +
          ": holders exercise to collect it, and the simulator assigns about half of them " +
          (tonight ? "at tonight's rollover" : "at the rollover before it") + ", delivering short shares that owe the dividend",
          time_value.dollars(), dividend.dollars());
      break;
    }
  }
  // Ex-dates within a week on underlyings the account holds options or shares of.
  std::set<std::string> held_underlyings;
  for (const auto& [symbol, position] : s.ledger.positions())
    if (position.quantity != 0 && s.time < s.contracts.at(symbol).expiry_time()) held_underlyings.insert(s.contracts.at(symbol).underlying);
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (stock.shares != 0) held_underlyings.insert(symbol);
  std::map<std::pair<std::string, md::Date>, Money> upcoming;
  for (const auto& d : dividends)
    if (held_underlyings.contains(d.symbol) && d.ex_date > s.day && d.ex_date <= horizon && d.per_share > Money{})
      upcoming[{d.symbol, d.ex_date}] = upcoming[{d.symbol, d.ex_date}] + d.per_share;
  for (const auto& [key, per_share] : upcoming) {
    const auto& [symbol, date] = key;
    const auto shares = shares_held(s, symbol);
    // In whole cents, as the rollover pays it: less than half a cent is nothing.
    const auto amount = nearest_cent(per_share * shares);
    auto message = symbol + " goes ex-dividend on " + md::format_date(date) + ", " + dollars(per_share) + " a share";
    if (shares != 0 && amount == Money{})
      message += ": on your " + std::to_string(shares < 0 ? -shares : shares) + (shares < 0 ? " short" : "") +
                 " shares it comes to less than half a cent, so nothing is paid";
    else if (shares > 0) message += ": your " + std::to_string(shares) + " shares are paid " + dollars(amount);
    else if (shares < 0) message += ": your " + std::to_string(-shares) + " short shares pay " + dollars(-amount);
    else message += ": option prices allow for it, and short calls with less time value than it may be assigned the night before";
    add("EX_DIVIDEND", shares < 0, symbol, {}, message, per_share.dollars(), std::nullopt);
  }
  std::stable_sort(out.begin(), out.end(), [](const RiskWarning& a, const RiskWarning& b) {
    return a.severity == "warning" && b.severity != "warning";
  });
  return out;
}
namespace {
/// The value an alert watches now, when it is known and fresh: quotes and marks
/// within the quote age, valuations and indicators within the valuation age,
/// and account measures on complete marks (exposures on complete valuations).
std::optional<Money> alert_value(const State& s, const AlertCondition& c, const std::function<const Measures&()>& measures) {
  const DeliverablesScope adjusted(&s.deliverables);
  const auto recent = [&](Timestamp time, Timestamp age) { return time <= s.time && s.time - time <= age; };
  const auto money = [](double value) -> std::optional<Money> {
    if (!std::isfinite(value)) return std::nullopt;
    try { return Money::from_double(value); } catch (const TradingError&) { return std::nullopt; }
  };
  const auto mark = [&](const std::string& symbol) -> std::optional<Money> {
    const auto it = s.marks.find(symbol);
    if (it == s.marks.end() || !recent(it->second.time, s.config.limits.max_quote_age)) return std::nullopt;
    return it->second.price;
  };
  switch (c.scope) {
    case AlertScope::Contract: {
      if (c.metric == "mark") return mark(c.symbol);
      if (c.metric == "bid" || c.metric == "ask") {
        const auto book = s.books.find(c.symbol);
        if (book == s.books.end() || !markable_quote(book->second.quote) || book->second.quote.modelled ||
            !recent(book->second.quote.time, s.config.limits.max_quote_age)) return std::nullopt;
        return c.metric == "bid" ? book->second.quote.bid : book->second.quote.ask;
      }
      const auto it = s.valuations.find(c.symbol);
      if (it == s.valuations.end() || !valid_valuation(it->second) || !recent(it->second.time, s.config.limits.max_valuation_age))
        return std::nullopt;
      const auto& v = it->second;
      return money(c.metric == "iv" ? v.smile_iv * 100 : c.metric == "delta" ? v.delta : c.metric == "gamma" ? v.gamma
                   : c.metric == "theta" ? v.theta : v.vega);
    }
    case AlertScope::Spread: {
      Money net;
      for (const auto& leg : c.legs) {
        const auto price = mark(leg.symbol);
        if (!price) return std::nullopt;
        net = net + (leg.side == Side::Buy ? *price : -*price) * leg.ratio;
      }
      return net;
    }
    case AlertScope::Underlying: {
      const auto it = s.indicators.find(indicator_key(c.symbol, c.metric == "price" ? "" : c.metric));
      if (it == s.indicators.end() || !recent(it->second.time, s.config.limits.max_valuation_age)) return std::nullopt;
      return it->second.price;
    }
    case AlertScope::Account: {
      const auto& m = measures();
      if (c.metric == "buying_power") {
        if (!m.valuation_complete || !m.risk.complete) return std::nullopt;
        // Working reservations also depend on their legs' current quotes.
        for (const auto id : open_ids(s))
          for (const auto& symbol : order_symbols(s.orders[id - 1].request))
            if (!quote_check(s, symbol).ok()) return std::nullopt;
        return m.buying_power.available;
      }
      if (c.metric == "dollar_delta" || c.metric == "vega" || c.metric == "theta" || c.metric == "dollar_gamma_1pct") {
        if (!m.risk.complete) return std::nullopt;
        const auto& e = m.risk.aggregate.position;
        return money(c.metric == "dollar_delta" ? e.dollar_delta : c.metric == "vega" ? e.vega : c.metric == "theta" ? e.theta : e.dollar_gamma_1pct);
      }
      if (c.metric == "delta_utilisation" || c.metric == "vega_utilisation") {
        if (!m.risk.complete) return std::nullopt;
        return money(c.metric == "delta_utilisation" ? m.risk.aggregate.delta_utilisation : m.risk.aggregate.vega_utilisation);
      }
      if (c.metric == "grid_loss") {
        if (!m.valuation_complete) return std::nullopt;
        const auto grid = scenario_grid(s.ledger, s.valuations, s.config.scenarios, s.time,
            s.config.limits.max_valuation_age, m.stock_prices);
        if (!grid.complete) return std::nullopt;
        double loss = 0;
        for (const auto& cell : grid.cells) loss = std::max(loss, -cell.pnl);
        return money(loss);
      }
      if (c.metric == "floor_touch_probability") {
        if (!m.valuation_complete || !m.risk.complete || (s.config.rules.max_drawdown <= Money{} && !m.soft_floor)) return std::nullopt;
        std::map<std::string, double> variances;
        for (const auto& [symbol, bucket] : m.risk.underlyings) {
          const auto sigma = s.indicators.find(indicator_key(symbol, "close_sigma"));
          if (sigma == s.indicators.end() || !recent(sigma->second.time, s.config.limits.max_valuation_age)) return std::nullopt;
          variances[symbol] = std::pow(sigma->second.price.dollars() / 100, 2);
        }
        const auto breach = breach_of(s, variances);
        if (!breach.complete) return std::nullopt;
        double probability = 0;
        for (const auto& item : breach.underlyings) {
          if (!item.complete) return std::nullopt;
          for (const auto* level : {&item.down, &item.up, &item.soft_down, &item.soft_up})
            if (*level) {
              if (!(*level)->touch_probability) return std::nullopt;
              probability = std::max(probability, *(*level)->touch_probability);
            }
        }
        return money(probability);
      }
      if (!m.valuation_complete) return std::nullopt;
      if (c.metric == "equity") return m.equity;
      if (c.metric == "day_pnl") return m.equity - s.start_equity;
      if (c.metric == "unrealised") return m.unrealised;
      // Floor room: to the plan's floor and the personal soft floor, whichever is nearer.
      std::optional<Money> room;
      if (s.config.rules.max_drawdown > Money{}) room = m.equity - s.evaluation.floor;
      if (m.soft_floor) room = room ? std::min(*room, m.equity - *m.soft_floor) : m.equity - *m.soft_floor;
      return room;
    }
  }
  return std::nullopt;
}
}  // namespace
/// Each alert fires as its condition is reached: a one-shot alert once, a
/// repeating one again each time the condition returns after a known value
/// showed it lapsed.
void check_alerts(State& s, Events& events) {
  if (s.alerts.items.empty()) return;
  std::optional<Measures> measured;
  const std::function<const Measures&()> measures = [&]() -> const Measures& {
    if (!measured) measured = measure(s);
    return *measured;
  };
  std::map<std::string, std::optional<Money>> account_values;
  for (auto& alert : s.alerts.items) {
    if (!alert.armed && !alert.spec.repeat) continue;
    const auto& c = alert.spec.condition;
    std::optional<Money> value;
    try {
      if (c.scope == AlertScope::Account) {
        const auto [it, added] = account_values.try_emplace(c.metric);
        if (added) it->second = alert_value(s, c, measures);
        value = it->second;
      } else value = alert_value(s, c, measures);
    }
    catch (const TradingError& error) {
      // An unrepresentable alert value must not stop market processing.
      if (error.code() != Reason::ARITHMETIC_OVERFLOW) throw;
    }
    if (!value) continue;
    if (!(c.direction == TriggerDirection::AtOrBelow ? *value <= c.level : *value >= c.level)) {
      alert.armed = true;
      continue;
    }
    if (!alert.armed) continue;
    alert.armed = false;
    ++alert.fired;
    alert.fired_at = s.time;
    alert.value = *value;
    event(events, "alert_fired", Json{{"alert", alert}});
  }
}
}  // namespace session_internal
}  // namespace openport::trading
