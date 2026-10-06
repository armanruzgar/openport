#include "openport/trading/session.hpp"
#include "openport/trading/contracts.hpp"
#include "openport/trading/events.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "openport/trading/format.hpp"
#include "openport/trading/history.hpp"
#include "state.hpp"
#include "review.hpp"
#include "state_delta.hpp"

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
namespace {
using detail::State;
using Events = std::vector<Json>;
Decision failure(Reason code, std::string message) { return {code, std::move(message), {}, {}, {}}; }
/// Appends an order; its client order ID keeps naming the first order that used it
/// in the current attempt, so an account reset frees the IDs earlier attempts used.
void add_order(State& s, const Order& order) {
  const auto& key = order.request.client_order_id;
  if (const auto first = s.clients.find(key); first == s.clients.end() || first->second < s.evaluation.first_order)
    s.clients[key] = order.id;
  s.orders.push_back(order);
}
void event(Events& events, std::string_view type, Json payload) {
  events.push_back(Json{{"type", type}, {"payload", std::move(payload)}});
}
SettlementRecord settlement_record(const Position& position, Money value, Money intrinsic, Money fee,
                                   Timestamp time, std::optional<SettlementSource> source) {
  const auto proceeds = (intrinsic * 100) * position.quantity;
  return {position.contract, value, time, position.quantity, proceeds - fee, proceeds - position.basis, fee, std::move(source)};
}
bool regular(const md::OptionContract& c, Timestamp time) { return md::trading_session(c.root, time).name == "regular"; }
bool extended(const OrderRequest& r) { return r.tif == TimeInForce::Exto || r.tif == TimeInForce::GtcExto; }
bool good_until(const OrderRequest& r) {
  return r.tif == TimeInForce::Gtc || r.tif == TimeInForce::GtcExto || r.tif == TimeInForce::Gtd;
}
bool immediate(const OrderRequest& r) {
  return r.tif == TimeInForce::Ioc || (r.type == OrderType::Market && r.trigger && (extended(r) || r.tif == TimeInForce::Gtd));
}
Decision tif_check(const State& s, const OrderRequest& r, Timestamp accepted) {
  if (r.tif != TimeInForce::Gtd)
    return r.good_till ? failure(Reason::INVALID_ORDER, "Only GTD takes good_till") : Decision{};
  constexpr auto bound = 366LL * 24 * 60 * 60 * md::kNanosPerSecond;
  if (!r.good_till || *r.good_till <= s.time || *r.good_till - accepted > bound)
    return failure(Reason::INVALID_ORDER, "GTD needs a future good_till timestamp within 366 days of acceptance");
  return {};
}
/// Whether the contract trades now, and the order with it: every order in the
/// regular session; plain limit orders in the overnight (global) and curb sessions.
/// Persistent limits and stops can wait between sessions. EXTO/GTC_EXTO triggers
/// and brackets execute as simulator-managed limits outside regular hours. Legacy
/// protection and GTC/GTD limits wait for the regular session.
Decision session_check(const md::OptionContract& c, Timestamp time, const OrderRequest& r) {
  if (time >= c.last_trade_time())
    return failure(Reason::SESSION_CLOSED, "AM-settled series stop trading at the regular close before expiry");
  if ((r.type == OrderType::Limit && (good_until(r) || extended(r))) ||
      (r.trigger && (r.tif == TimeInForce::GtcExto || r.tif == TimeInForce::Gtd)) || r.exits_only) return {};
  const auto session = md::trading_session(c.root, time);
  if (!session.open) return failure(Reason::SESSION_CLOSED, "Outside the contract's trading sessions");
  if (session.name != "regular" && (extended(r) ? r.type != OrderType::Limit && !r.trigger
                                                : r.type != OrderType::Limit || r.trigger || r.bracket))
    return failure(Reason::LIMIT_ONLY, "The overnight and curb sessions take plain limit orders only: no market "
                   "orders, and triggers or brackets only on a GTC limit, which waits for the regular session");
  return {};
}
Timestamp session_end(const md::OptionContract& c, Timestamp time) {
  const auto session = md::trading_session(c.root, time);
  if (!session.open || session.end == md::kInvalidTimestamp)
    throw TradingError(Reason::SESSION_CLOSED, "No representable session end");
  return session.end;
}
/// Persistent orders stop at the first leg's last trade or account auto-close.
Timestamp order_expiry(const State& s, const OrderRequest& r) {
  auto end = std::numeric_limits<Timestamp>::max();
  for (const auto& symbol : order_symbols(r))
    end = std::min(end, s.contracts.at(symbol).last_trade_time() - s.config.rules.expiry_cutoff);
  return end;
}
/// A DAY order lasts the session it was accepted in (a triggered one, the session
/// it activated in), and no longer than a persistent order would.
Timestamp day_deadline(const State& s, const OrderRequest& r, Timestamp time) {
  return std::min(session_end(s.contracts.at(order_symbols(r).front()), time), order_expiry(s, r));
}
/// Outlives a session: extended TIFs, GTC/GTD, bracket exits and armed orders. Once triggered,
/// a DAY order lasts its session like any other.
bool persistent(const Order& o) {
  return good_until(o.request) || extended(o.request) || o.role != OrderRole::Normal ||
         (o.request.trigger && (o.status == OrderStatus::Armed || o.request.tif != TimeInForce::Day));
}
/// EXTO lasts through the trading date's last session; expiry and account cutoff
/// still bound it. GTD trades regular hours until its explicit timestamp.
Timestamp order_end(const State& s, const OrderRequest& r, Timestamp time) {
  const auto expiry = order_expiry(s, r);
  if (r.tif == TimeInForce::Gtd) return std::min(expiry, *r.good_till);
  if (good_until(r)) return expiry;
  if (r.tif != TimeInForce::Exto) return day_deadline(s, r, time);
  const auto& root = s.contracts.at(order_symbols(r).front()).root;
  const auto current = md::trading_session(root, time);
  const auto date = current.name == "global" ? md::new_york_time(current.end).date : md::trading_date(time);
  const auto noon = md::new_york_to_utc(date, 12, 0);
  const auto regular_session = md::trading_session(root, noon);
  auto end = regular_session.open ? regular_session.end : current.end;
  const auto curb = md::trading_session(root, end);
  if (curb.open && curb.name == "curb") end = curb.end;
  if (end == md::kInvalidTimestamp || end <= time)
    throw TradingError(Reason::SESSION_CLOSED, "No remaining session for this trading date");
  return std::min(expiry, end);
}
bool trades_now(const Order& o, const md::OptionContract& c, Timestamp time) {
  if (!extended(o.request)) return !persistent(o) || regular(c, time);
  return md::trading_session(c.root, time).open;
}
/// Extended stop-market orders are simulator-held triggers. Outside regular
/// hours they send an IOC limit at the touch, taking displayed liquidity only.
bool touch_stop(const State& s, const Order& o) {
  return extended(o.request) && o.request.type == OrderType::Market && o.request.trigger &&
      !regular(s.contracts.at(order_symbols(o.request).front()), s.time);
}
Annotation clean_annotation(std::string note, const std::vector<std::string>& tags);
void store_annotation(State& s, const std::string& key, Annotation annotation, Events& events);
Decision system_check(const State& s, const Order& o, bool now = true);
Money mid(const QuoteObservation& q) {
  // Both sides are positive. Difference-first avoids overflowing their sum.
  return *q.bid + (*q.ask - *q.bid).prorate(1, 2);
}
/// A markable quote's mark: its mid, or halfway to the ask when nobody bids.
Money mark_of(const QuoteObservation& q) { return q.bid ? mid(q) : q.ask->prorate(1, 2); }
Decision quote_check(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !valid_quote(it->second.quote))
    return failure(Reason::INVALID_QUOTE, "A noncrossed positive two-sided quote with both sizes is required");
  const auto time = it->second.quote.time;
  if (time > s.time || observation_time(s.contracts.at(symbol), s.time) - time > s.config.limits.max_quote_age)
    return failure(Reason::STALE_QUOTE, "Quote is outside the configured market-time freshness window");
  return {};
}
/// Why a quote that is not executable cannot fill paper orders, in words.
std::string quote_problem(const QuoteObservation& q) {
  if (q.observation == 0 || (!q.bid && !q.ask)) return "No quote";
  if (!q.bid || *q.bid <= Money{}) return "No bid: the quote is one-sided";
  if (!q.ask || *q.ask <= Money{}) return "No ask: the quote is one-sided";
  if (*q.ask < *q.bid) return "Crossed: the bid is above the ask";
  return "A side shows no displayed size";
}
/// Whether a position's quote marks it now: markable, and inside the freshness window.
/// A far option nobody bids for is marked; only a valid quote trades.
bool marked_now(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !markable_quote(it->second.quote)) return false;
  const auto time = it->second.quote.time;
  return time <= s.time && observation_time(s.contracts.at(symbol), s.time) - time <= s.config.limits.max_quote_age;
}
/// Orders kept within the position they close: bracket exits and reduce-only
/// closes shrink with it and are cancelled once it is flat, so they never open one.
bool kept_within(const Order& o) { return o.role != OrderRole::Normal || o.reduce_only; }
/// A fresh quote that shows only an ask, as a far option nobody bids for does.
bool asks_only(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  return it != s.books.end() && !valid_quote(it->second.quote) && marked_now(s, symbol);
}
/// A combo exit's (or reduce-only close's) leg whose fresh quote shows only an ask.
/// The order buys such a leg back at that ask and gives it away at zero when it
/// sells it, rather than wait for a bid that may never come, so a worthless wing
/// cannot hold a stop, a target or a flatten back.
bool ask_only(const State& s, const Order& o, const Leg& leg) {
  return (kept_within(o) || o.request.exits_only) && asks_only(s, leg.symbol);
}
/// An exit's long leg that nobody bids for: sold at zero, without displayed size.
bool given_away(const State& s, const Order& o, const Leg& leg) { return leg.side == Side::Sell && ask_only(s, o, leg); }
bool closing_only(const State& s, const Order& o, bool include_working = true, bool include_armed = true);
/// The same for a single contract: a buy that only closes a short takes an ask-only
/// quote within its size (a short quoted 0.00/0.05 can be bought back), and a
/// reduce-only market close gives a long nobody bids for away at zero.
bool ask_only_single(const State& s, const Order& o) {
  if (multi_leg(o.request) || !asks_only(s, o.request.symbol)) return false;
  if (o.request.side == Side::Buy) return kept_within(o) || closing_only(s, o, false);
  return o.reduce_only && o.request.type == OrderType::Market;
}
/// The price band around the quote's mid, or around `center` (a stop-limit's trigger level).
Decision price_check(const State& s, const QuoteObservation& q, Money price, std::optional<Money> center = {}) {
  const auto middle = center ? *center : mark_of(q);
  const long double difference = std::abs(static_cast<long double>(price.micros()) - static_cast<long double>(middle.micros()));
  const long double band = std::max(static_cast<long double>(s.config.limits.price_band_absolute.micros()),
      static_cast<long double>(s.config.limits.price_band_relative) * static_cast<long double>(middle.micros()));
  if (difference > band)
    return {Reason::PRICE_BAND, center ? "Limit is outside the configured band around the trigger level"
                                       : "Price is outside the configured band around mid",
            static_cast<double>(difference / 1'000'000), static_cast<double>(band / 1'000'000), q.symbol};
  return {};
}
Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
Quantity held(const State& s, const std::string& symbol) {
  const auto it = s.ledger.positions().find(symbol);
  return it == s.ledger.positions().end() ? 0 : it->second.quantity;
}
std::optional<double> spot_for(const State& s, const std::string& symbol) {
  const auto it = s.valuations.find(symbol);
  if (it == s.valuations.end() || !valid_valuation(it->second)) return std::nullopt;
  return it->second.spot;
}
const Valuation* valuation_of(const State& s, const std::string& symbol) {
  const auto it = s.valuations.find(symbol);
  return it == s.valuations.end() || !valid_valuation(it->second) ? nullptr : &it->second;
}
/// A stretch's change in value to `value` per unit, split by the Greeks at its
/// start; without valuations at both ends, it is all `other`.
Attribution explain(const md::OptionContract& c, const detail::Reference& r, Money value, const Valuation* now) {
  Attribution a;
  const double size = static_cast<double>(r.quantity) * c.multiplier;
  if (r.valuation && now) {
    const auto& v = *r.valuation;
    const double move = now->spot - v.spot;
    a.delta = size * v.delta * move;
    a.gamma = size * 0.5 * v.gamma * move * move;
    a.vega = size * v.vega * (now->smile_iv - v.smile_iv) * 100;
    a.theta = size * v.theta * (v.years - now->years) * 365;
  } else {
    a.fallback = value != r.mark;
  }
  a.other = size * (value - r.mark).dollars() - a.delta - a.gamma - a.vega - a.theta;
  return a;
}
/// Books part of today's explanation to `symbol`, and to the round trip it is in.
void explain_part(State& s, const std::string& symbol, const Attribution& part) {
  s.explained[symbol] += part;
  if (const auto trip = s.trips.find(symbol); trip != s.trips.end()) s.trip_attribution[trip->second] += part;
}
/// End a held contract's stretch at `value` per unit, into today's explanation.
void end_stretch(State& s, const std::string& symbol, Money value, const Valuation* now) {
  const auto it = s.references.find(symbol);
  if (it == s.references.end()) return;
  explain_part(s, symbol, explain(s.contracts.at(symbol), it->second, value, now));
  s.references.erase(it);
}
/// A fill's costs, from `before` held to `before + change`, go to the round trip
/// it trades in: a fill from flat opens one as `trade`, and one that reverses the
/// holding books its closing part to the round trip it ends and the rest to the
/// one it opens. A holding that becomes flat leaves its round trip.
void book_costs(State& s, const std::string& symbol, Quantity before, Quantity change, double costs, const std::string& trade) {
  Attribution part;
  part.costs = costs;
  s.explained[symbol] += part;
  const auto after = before + change;
  if (before == 0 || (after != 0 && (after > 0) != (before > 0))) {
    if (before != 0) {
      const double closing = costs * static_cast<double>(before < 0 ? -before : before) / static_cast<double>(change < 0 ? -change : change);
      if (const auto trip = s.trips.find(symbol); trip != s.trips.end()) s.trip_attribution[trip->second].costs += closing;
      part.costs = costs - closing;
    }
    s.trips[symbol] = trade;
    s.trip_attribution[trade] += part;
  } else if (const auto trip = s.trips.find(symbol); trip != s.trips.end()) {
    s.trip_attribution[trip->second] += part;
  }
  if (after == 0) s.trips.erase(symbol);
}
/// Start a stretch at the contract's size, mark and valuation now.
void start_stretch(State& s, const std::string& symbol) {
  const auto quantity = held(s, symbol);
  const auto mark = s.marks.find(symbol);
  if (quantity == 0 || mark == s.marks.end()) return;
  const auto* valuation = valuation_of(s, symbol);
  s.references[symbol] = {quantity, mark->second.price, valuation ? std::optional(*valuation) : std::nullopt};
}
/// Book a fill: the stretch before it ends at the mark, the spread paid against
/// the mark and the fee are costs, and a stretch at the new size starts.
void fill_position(State& s, const std::string& symbol, Quantity signed_quantity, Money price, Money fee) {
  const auto& contract = s.contracts.at(symbol);
  const auto mark = s.marks.find(symbol);
  const Money value = mark == s.marks.end() ? price : mark->second.price;
  end_stretch(s, symbol, value, valuation_of(s, symbol));
  const auto before = held(s, symbol);
  s.ledger.fill(contract, signed_quantity, price, fee);
  // A fill that opens a round trip is the next fill recorded.
  book_costs(s, symbol, before, signed_quantity,
             static_cast<double>(signed_quantity) * contract.multiplier * (value - price).dollars() - fee.dollars(),
             std::to_string(s.fills.size() + 1));
  start_stretch(s, symbol);
  if (held(s, symbol) <= 0 && s.do_not_exercise.contains(symbol)) s.do_not_exercise.erase(symbol);
}
Quantity closing_contracts(Quantity position, Side side, Quantity contracts) {
  return std::min(contracts, side == Side::Buy ? std::max<Quantity>(-position, 0) : std::max<Quantity>(position, 0));
}
/// The commission cap belongs to a leg of an order, across all partial fills.
Money commission_paid(const State& s, const Order& o, const std::string& symbol) {
  Money paid;
  if (!s.config.rules.fees || s.config.rules.fees->leg_cap <= Money{}) return paid;
  if (const auto it = s.projected_commission.find({o.id, symbol}); it != s.projected_commission.end()) paid = it->second;
  for (auto i = s.fills.size(); i > 0 && s.fills[i - 1].time >= o.accepted_at; --i) {
    const auto& f = s.fills[i - 1];
    if (f.order_id == o.id && f.symbol == symbol && f.fees) paid = paid + f.fees->commission;
  }
  return paid;
}
FillFees fees_for(const State& s, const std::string& symbol, Quantity contracts, Quantity closing, Money paid = {}) {
  const auto& schedule = s.config.rules.fees;
  if (!schedule) return {s.config.fee_per_contract * contracts, {}, {}, {}};
  FillFees fees{schedule->open * (contracts - closing) + schedule->close * closing,
                schedule->clearing * contracts, schedule->regulatory * contracts, {}};
  if (schedule->leg_cap > Money{}) fees.commission = std::min(fees.commission, std::max(Money{}, schedule->leg_cap - paid));
  if (const auto contract = s.contracts.find(symbol); contract != s.contracts.end())
    if (const auto index = schedule->index.find(contract->second.root); index != schedule->index.end())
      fees.index = index->second * contracts;
  return fees;
}
Money exercise_fee(const State& s, Quantity contracts) {
  return s.config.rules.fees ? s.config.rules.fees->exercise * magnitude(contracts) : Money{};
}
/// American equity and ETF options deliver shares; index options settle in cash.
bool physical(const md::OptionContract& c) {
  return c.style == pricing::ExerciseStyle::American && !md::is_index_underlying(c.underlying);
}
Quantity shares_held(const State& s, const std::string& symbol) {
  const auto it = s.ledger.stocks().find(symbol);
  return it == s.ledger.stocks().end() ? 0 : it->second.shares;
}
/// The underlying's price if fresh at `at`: within max_quote_age of it while the
/// stock market is open, or of its last close while it is not (the 16:00 close
/// stays current while the options trade on to 16:15, and overnight).
std::optional<Money> stock_price(const State& s, const std::string& symbol, Timestamp at) {
  const auto it = s.stock_marks.find(symbol);
  if (it == s.stock_marks.end() || it->second.time > at) return std::nullopt;
  const auto session = md::stock_session(at);
  const auto observed = session.open || session.market_time == md::kInvalidTimestamp ? at : std::min(at, session.market_time);
  if (observed - it->second.time > s.config.limits.max_quote_age) return std::nullopt;
  return it->second.price;
}
/// The underlying's price if fresh at the market time.
std::optional<Money> stock_price(const State& s, const std::string& symbol) { return stock_price(s, symbol, s.time); }
/// Shares' stretches are all delta: the price's move times the shares.
Attribution explain_stock(const detail::Reference& r, Money value) {
  Attribution a;
  a.delta = static_cast<double>(r.quantity) * (value - r.mark).dollars();
  return a;
}
void start_stock_stretch(State& s, const std::string& symbol) {
  const auto shares = shares_held(s, symbol);
  const auto mark = s.stock_marks.find(symbol);
  if (shares == 0 || mark == s.stock_marks.end()) return;
  s.references[symbol] = {shares, mark->second.price, std::nullopt};
}
/// Trade shares at `price`: the stretch before ends at the underlying's mark,
/// any difference from it is a cost, and a stretch at the new size starts. The
/// fill goes into the trade history with how it came about.
void trade_shares(State& s, const std::string& symbol, Quantity signed_shares, Money price, StockSource source,
                  const std::string& option = {}) {
  const auto mark = s.stock_marks.find(symbol);
  const Money value = mark == s.stock_marks.end() ? price : mark->second.price;
  if (const auto it = s.references.find(symbol); it != s.references.end()) {
    explain_part(s, symbol, explain_stock(it->second, value));
    s.references.erase(it);
  }
  const auto before = shares_held(s, symbol);
  s.ledger.trade_stock(symbol, signed_shares, price, Money{});
  book_costs(s, symbol, before, signed_shares, static_cast<double>(signed_shares) * (value - price).dollars(),
             "s" + std::to_string(s.stock_fills.size() + 1));
  s.stock_fills.push_back({s.stock_fills.size() + 1, symbol, signed_shares, price, s.time, source, option});
  start_stock_stretch(s, symbol);
}
/// Shares an option position becomes: 100 a contract, bought by long calls and
/// short puts, sold by long puts and short calls.
Quantity delivered(const md::OptionContract& c, Quantity contracts) {
  return (c.type == pricing::OptionType::Call ? 1 : -1) * static_cast<Quantity>(c.multiplier) * contracts;
}
/// A bracket's two exits can fill only once between them: checks count the pair
/// through its earlier order, so the later one is shadowed while both are open.
/// Two orders that cancel each other may differ, so each counts in full.
bool shadowed(const State& s, const Order& o) {
  return o.role != OrderRole::Normal && o.oco != 0 && o.oco < o.id && s.orders.at(static_cast<std::size_t>(o.oco - 1)).open();
}
/// Contracts not reserved by working user orders on the closing side. Managed
/// exits shrink with the position and system IOCs are capped at execution too.
Quantity closing_capacity(const State& s, const std::string& symbol, Side side, OrderId skip = 0, bool include_armed = true) {
  const auto q = held(s, symbol);
  auto capacity = side == Side::Sell ? std::max<Quantity>(q, 0) : std::max<Quantity>(-q, 0);
  const auto reserve = [&](Quantity units, Quantity ratio) {
    capacity -= units > capacity / ratio ? capacity : units * ratio;
  };
  for (const auto id : open_ids(s)) {
    const auto& other = s.orders[id - 1];
    if (other.system || kept_within(other) || other.id == skip) continue;
    if (!include_armed && other.status == OrderStatus::Armed) continue;
    if (multi_leg(other.request)) {
      for (const auto& leg : other.request.legs)
        if (leg.symbol == symbol && leg.side == side) reserve(other.remaining(), leg.ratio);
    } else if (other.request.symbol == symbol && other.request.side == side) reserve(other.remaining(), 1);
  }
  return capacity;
}
/// Every leg must oppose its holding and fit within it after the other working
/// user orders on that side. Managed exits do not reserve a manual close's capacity.
bool closing_only(const State& s, const Order& o, bool include_working, bool include_armed) {
  const auto closes = [&](const std::string& symbol, Side side, Quantity ratio) {
    const auto q = held(s, symbol);
    if (q == 0 || (side != Side::Buy && side != Side::Sell) ||
        (q > 0) == (side == Side::Buy) || o.remaining() <= 0 || ratio < 1 || ratio > kMaxRatio)
      return false;
    if (o.remaining() > magnitude(q) / ratio) return false;
    const auto capacity = include_working ? closing_capacity(s, symbol, side, o.id, include_armed) : magnitude(q);
    return o.remaining() <= capacity / ratio;
  };
  if (multi_leg(o.request))
    return std::all_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) {
      return closes(leg.symbol, leg.side, leg.ratio);
    });
  return closes(o.request.symbol, o.request.side, 1);
}
bool opens(Quantity held_quantity, Quantity signed_fill) {
  return held_quantity == 0 || (held_quantity > 0) == (signed_fill > 0) || magnitude(signed_fill) > magnitude(held_quantity);
}
/// Whether an order trades `symbol`, as its contract or one of its legs.
bool touches(const OrderRequest& r, const std::string& symbol) {
  return r.symbol == symbol || std::any_of(r.legs.begin(), r.legs.end(), [&](const Leg& leg) { return leg.symbol == symbol; });
}
Quantity signed_contracts(const Leg& leg, Quantity units) { return leg.side == Side::Buy ? units * leg.ratio : -units * leg.ratio; }
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
                              const std::map<std::string, Quantity>& preceding = {}) {
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
Decision hedging_direction_check(const State& s, const std::string& underlying, double delta) {
  if (!s.config.rules.no_hedging || delta == 0) return {};
  const auto held_delta = held_direction(s, underlying);
  if (!held_delta) return failure(Reason::MISSING_VALUATION, "Fresh held valuations required to determine hedging direction");
  if (*held_delta == 0 || (*held_delta > 0) == (delta > 0)) return {};
  return {Reason::HEDGING, "Opening " + underlying + " dollar delta " + format_dollar_delta(delta) +
      " opposes held dollar delta " + format_dollar_delta(*held_delta), delta, *held_delta, underlying,
      RuleEvidence{underlying, delta, *held_delta}};
}
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
Quantity depth_used(Quantity size, Quantity left, Quantity offset = 0) {
  Quantity used = 0;
  if (__builtin_sub_overflow(size, left, &used) || __builtin_add_overflow(used, offset, &used))
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Simulated depth exceeds quantity range");
  return used;
}
void consume_depth(Quantity& left, Quantity quantity) {
  if (__builtin_sub_overflow(left, quantity, &left))
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Liquidity budget exceeds quantity range");
}
/// Strictly inside, at the configured fraction of the way to the far side.
bool inside_reach(const State& s, Money near, Money far, Money limit) {
  if (s.config.rules.inside_fill_percent == 0 || limit <= near || limit >= far) return false;
  __extension__ using Wide = __int128;
  return (static_cast<Wide>(limit.micros()) - near.micros()) * 100 >=
      (static_cast<Wide>(far.micros()) - near.micros()) * s.config.rules.inside_fill_percent;
}
bool inside_single(const State& s, const Order& o) {
  const auto& r = o.request;
  if (s.config.rules.inside_fill_percent == 0 || !r.limit_price || r.trigger || multi_leg(r)) return false;
  const auto book = s.books.find(r.symbol);
  if (book == s.books.end() || !valid_quote(book->second.quote)) return false;
  const auto& q = book->second.quote;
  return r.side == Side::Buy ? inside_reach(s, *q.bid, *q.ask, *r.limit_price)
                             : inside_reach(s, -*q.ask, -*q.bid, -*r.limit_price);
}
/// Allocate a net's improvement in proportion to each leg's spread. Round toward
/// improvement in micro-dollars, then return excess through ratio-one legs. A
/// ratio-only combo can improve on its limit by a few micro-dollars per unit.
std::optional<std::vector<Money>> inside_combo(const State& s, const Order& o) {
  const auto& r = o.request;
  if (!r.limit_price || r.trigger || !multi_leg(r) || s.config.rules.inside_fill_percent == 0) return std::nullopt;
  Money natural, width;
  for (const auto& leg : r.legs) {
    const auto book = s.books.find(leg.symbol);
    if (book == s.books.end() || !valid_quote(book->second.quote)) return std::nullopt;
    const auto& q = book->second.quote;
    natural = natural + (leg.side == Side::Buy ? *q.ask : -*q.bid) * leg.ratio;
    width = width + (*q.ask - *q.bid) * leg.ratio;
  }
  if (!inside_reach(s, natural - width, natural, *r.limit_price)) return std::nullopt;
  const auto improvement = natural - *r.limit_price;
  __extension__ using Wide = __int128;
  std::vector<Money> given;
  Money total;
  for (const auto& leg : r.legs) {
    const auto& q = s.books.at(leg.symbol).quote;
    const auto share = (static_cast<Wide>(improvement.micros()) * (*q.ask - *q.bid).micros() + width.micros() - 1) / width.micros();
    given.push_back(Money::from_micros(static_cast<std::int64_t>(share)));
    total = total + given.back() * leg.ratio;
  }
  for (std::size_t i = 0; i < given.size() && total > improvement; ++i) {
    if (r.legs[i].ratio != 1) continue;
    const auto back = std::min(given[i], total - improvement);
    given[i] = given[i] - back;
    total = total - back;
  }
  for (std::size_t i = 0; i < given.size(); ++i) {
    const auto& leg = r.legs[i];
    const auto& q = s.books.at(leg.symbol).quote;
    given[i] = leg.side == Side::Buy ? *q.ask - given[i] : *q.bid + given[i];
  }
  return given;
}
/// The book a fill on `side` takes, before it takes it: the quote as held and
/// what is left of that side's budget.
FillQuote fill_quote(const detail::Book& book, Side side) {
  const auto& q = book.quote;
  return {q.bid, q.ask, q.bid_size, q.ask_size, side == Side::Buy ? book.ask_left : book.bid_left, q.first_time()};
}
/// Slippage and simulated depth use the displayed price's tick tier. A negative
/// budget carries depth consumed by earlier orders on this same observation.
Money execution_price(const State& s, const std::string& symbol, Side side, std::optional<Money> limit = {}, Quantity offset = 0) {
  const auto& book = s.books.at(symbol);
  const auto& quote = book.quote;
  const bool buy = side == Side::Buy;
  const auto displayed = buy ? *quote.ask : *quote.bid;
  const auto size = buy ? quote.ask_size : quote.bid_size;
  const auto left = buy ? book.ask_left : book.bid_left;
  const auto blocks = s.config.rules.impact_ticks > 0 ? depth_used(size, left, offset) / size : 0;
  const auto tick = tick_size(s.contracts.at(symbol).root, displayed);
  const auto slip = tick * s.config.rules.slippage_ticks + tick * s.config.rules.impact_ticks * blocks;
  const auto price = buy ? displayed + slip : std::max(Money{}, displayed - slip);
  return limit && s.config.rules.impact_ticks == 0 ? (buy ? std::min(*limit, price) : std::max(*limit, price)) : price;
}
Money order_price(const State& s, const Order& o, const std::string& symbol, Side side,
                  std::optional<Money> limit = {}, Quantity offset = 0) {
  if (inside_single(s, o)) return *o.request.limit_price;
  if (touch_stop(s, o)) {
    const auto& q = s.books.at(symbol).quote;
    return side == Side::Buy ? *q.ask : *q.bid;
  }
  return execution_price(s, symbol, side, limit, offset);
}
/// What a market order for `quantity` contracts would trade at now, as
/// (price, contracts): an executable book's slipped far side, block by block
/// with impact as the fills would walk it; with only the far side quoted, that
/// side slipped; a buy without an ask at its last mark; otherwise nothing.
std::vector<std::pair<Money, Quantity>> market_slices(const State& s, const std::string& symbol, Side side, Quantity quantity) {
  if (quantity <= 0) return {};
  const bool buy = side == Side::Buy;
  if (const auto book = s.books.find(symbol); book != s.books.end()) {
    const auto& quote = book->second.quote;
    if (valid_quote(quote)) {
      if (s.config.rules.impact_ticks == 0) return {{execution_price(s, symbol, side), quantity}};
      std::vector<std::pair<Money, Quantity>> slices;
      const auto size = buy ? quote.ask_size : quote.bid_size;
      const auto left = buy ? book->second.ask_left : book->second.bid_left;
      for (Quantity k = 0; k < quantity;) {
        // The rest of the block the k-th contract falls in trades at one price.
        const auto take = std::min(quantity - k, size - depth_used(size, left, k) % size);
        slices.emplace_back(execution_price(s, symbol, side, {}, k), take);
        k += take;
      }
      return slices;
    }
    const auto far = buy ? quote.ask : quote.bid;
    if (far && *far > Money{} && (buy ? quote.ask_size : quote.bid_size) > 0) {
      const auto slip = tick_size(s.contracts.at(symbol).root, *far) * s.config.rules.slippage_ticks;
      return {{buy ? *far + slip : std::max(Money{}, *far - slip), quantity}};
    }
  }
  if (const auto mark = s.marks.find(symbol); buy && mark != s.marks.end()) return {{mark->second.price, quantity}};
  return {};
}
/// The premium, in dollars, of market_slices.
Money market_premium(const State& s, const std::string& symbol, Side side, Quantity quantity) {
  Money total;
  for (const auto& [price, n] : market_slices(s, symbol, side, quantity)) total = total + (price * 100) * n;
  return total;
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
/// A multi-leg order's next executable net debit per unit.
std::optional<Money> executable_net(const State& s, const Order& o) {
  if (const auto prices = inside_combo(s, o)) {
    Money net;
    for (std::size_t i = 0; i < prices->size(); ++i) {
      const auto& leg = o.request.legs[i];
      net = net + (leg.side == Side::Buy ? (*prices)[i] : -(*prices)[i]) * leg.ratio;
    }
    return net;
  }
  for (const auto& leg : o.request.legs) {
    const auto book = s.books.find(leg.symbol);
    if ((book == s.books.end() || !valid_quote(book->second.quote)) && !ask_only(s, o, leg)) return std::nullopt;
  }
  Money net;
  for (const auto& leg : o.request.legs) {
    if (given_away(s, o, leg)) continue;
    if (s.config.rules.impact_ticks == 0) {
      const auto price = order_price(s, o, leg.symbol, leg.side) * leg.ratio;
      net = leg.side == Side::Buy ? net + price : net - price;
    } else {
      for (Quantity offset = 0; offset < leg.ratio; ++offset) {
        const auto price = order_price(s, o, leg.symbol, leg.side, {}, offset);
        net = leg.side == Side::Buy ? net + price : net - price;
      }
    }
  }
  return net;
}
/// Quote time, not transaction time alone, must reach the delay. Reconfirmed
/// snapshots may qualify but do not replenish the shared liquidity budget.
bool latency_ready(const State& s, const Order& o) {
  if (s.config.rules.fill_latency_ms == 0) return true;
  const auto start = std::max(o.accepted_at, o.triggered_at);
  const auto delay = s.config.rules.fill_latency_ms * (md::kNanosPerSecond / 1000);
  for (const auto& symbol : order_symbols(o.request)) {
    const auto& quote = s.books.at(symbol).quote;
    const auto leg = std::find_if(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& l) { return l.symbol == symbol; });
    const bool usable = quote_check(s, symbol).ok() ||
                        (leg != o.request.legs.end() ? ask_only(s, o, *leg) : ask_only_single(s, o));
    if (quote.time < start || quote.time - start < delay || !usable) return false;
    if (o.system ? !regular(s.contracts.at(symbol), s.time) : !trades_now(o, s.contracts.at(symbol), s.time)) return false;
  }
  return true;
}
/// Positions for margin by symbol: signed contracts and, for shorts, their
/// buy-back value.
using MarginBook = std::map<std::string, std::pair<Quantity, Money>>;
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
Money position_value(const State& s, const MarginBook& book) {
  Money value;
  for (const auto& [symbol, entry] : book) {
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
  return policy;
}
Margin margin_of(const State& s, const MarginBook& book) {
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
Money requirement(const State& s, const MarginBook& book) { return margin_of(s, book).held; }
/// The held positions' requirement by underlying, with what holds it, as
/// margin_of reckons it: an incomplete portfolio scan falls back to strategy
/// margin's parts, each underlying adding the option minimum.
std::vector<MarginUnderlying> margin_detail(const State& s) {
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
/// Buying power that positions do not hold: cash less their margin requirement.
Money free_power(const State& s) { return s.ledger.account().cash - requirement(s, held_book(s)); }
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
Money average_unit_price(const Position& p) {
  const auto size = magnitude(p.quantity);
  if (size == 0) return {};
  return (p.basis < Money{} ? -p.basis : p.basis).prorate(1, size * 100);
}
struct PowerDetail {
  BuyingPower total;
  Money focus_reservation;
  Quantity focus_opening = 0;  ///< Contracts the focus order opens beyond earlier orders' claims.
  bool focus_uses = false;     ///< The focus order would reduce free buying power.
};
/// Positions hold their margin requirement (margin_requirement, spreads netted).
/// Each working order reserves what filling it now would cost (use_of): fees,
/// the change in margin on the held positions and the premium it pays, less
/// what it receives, never below the fees. Single-leg orders see the positions
/// less the contracts earlier orders already claim to close, in acceptance
/// order, so two sells cannot both claim the same long. Bracket exits stay
/// within their position and reserve only fees. Orders without a limit use
/// market_slices; a marketable buy limit reserves its expected fill there, without
/// fill latency, and an armed buy stop at least its trigger level.
PowerDetail buying_power(const State& s, OrderId focus = 0) {
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
  out.total.reserved = reserved;
  out.total.short_requirement = margin.requirement;
  out.total.available = s.ledger.account().cash - margin.held - reserved;
  return out;
}
struct ProjectedFill {
  Money premium;  ///< Paid, negative when received.
  Money fees;
  FillFees itemized;
};
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
/// What rules and checks read of the account now: its equity at the marks and
/// the positions behind it, risk, the soft floor and buying power. Unlike a
/// snapshot it copies no history and builds no scenario grid or attribution,
/// so it costs the open positions and orders, not the account's past.
struct Measures {
  Money equity;
  Money unrealised;
  bool valuation_complete = true;
  std::vector<MarkedPosition> positions;
  std::vector<MarkedStock> stocks;
  std::map<std::string, double> stock_prices;
  RiskSnapshot risk;
  std::optional<Money> soft_floor;
  BuyingPower buying_power;
};
Measures measure(const State& s) {
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
  out.risk = portfolio_risk(s.ledger, working, s.contracts, s.valuations, s.config.limits, s.time, out.stock_prices);
  out.risk.daily_loss = std::max(Money{}, s.start_equity - out.equity);
  out.risk.kill_latched = s.kill;
  out.risk.kill_reason = s.kill_reason;
  out.risk.limits_revision = s.limits_revision;
  out.soft_floor = soft_floor_of(s);
  out.buying_power = buying_power(s).total;
  return out;
}
std::optional<OrderWait> waiting_for(const State& s, const Order& o, bool data_complete);
Decision reset_check(const State& s, const Measures& m);
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
  out.dividends = s.dividends;
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
/// Rules only act on fully marked equity: every position has a mark, fresh or not.
template <class Account>
std::optional<Money> marked_equity(const Account& account) {
  for (const auto& p : account.positions) if (!p.market_value) return std::nullopt;
  for (const auto& p : account.stocks) if (!p.market_value) return std::nullopt;
  return account.equity;
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
Money net_realised(const State& s) { return s.ledger.account().realised - s.ledger.account().fees; }
Money whole_cents(Money value) {
  return value > Money{} ? Money::from_micros(value.micros() / 10'000 * 10'000) : Money{};
}
std::string dollars(Money value) { return (value < Money{} ? "-$" + (-value).str() : "$" + value.str()); }
Evaluation fresh_evaluation(const State& s, std::uint64_t attempt) {
  Evaluation e;
  e.attempt = attempt;
  e.started = s.time;
  e.starting_balance = s.ledger.account().cash;
  e.scaling_limit = scaling_limit(s.config.rules, Money{});
  if (s.config.rules.size_scaling) {
    if (e.starting_balance <= Money{} || s.config.rules.size_scaling->max_balance < e.starting_balance)
      throw TradingError(Reason::INVALID_RULES, "Account size scaling maximum must be at least the positive starting balance");
    e.size_scaling = SizeScalingProgress{e.starting_balance, s.config.rules.max_drawdown, s.config.rules.daily_loss_limit,
        s.day, 0, e.starting_balance, e.starting_balance, 0, {}};
  }
  e.peak = e.starting_balance;
  e.floor = evaluation_floor(s.config.rules, e.peak, e.floor_locked, e.starting_balance);
  e.day_open_realised = net_realised(s);
  e.cycle_started = s.time;
  e.first_order = static_cast<OrderId>(s.orders.size() + 1);
  e.first_fill = s.fills.size() + 1;
  if (s.config.rules.phase != Phase::Funded && s.config.rules.microscalp_percent > 0)
    e.first_stock_fill = s.stock_fills.size() + 1;
  e.day = s.day;
  e.day_open_equity = e.starting_balance;
  e.day_close_equity = e.starting_balance;
  return e;
}
/// Capital changes neither P&L nor the account's distance above its floor.
/// A review always restarts once its finished-day count is reached.
Money review_size_scaling(State& s, md::Date next_day, Money balance, Events& events) {
  auto& e = s.evaluation;
  auto& rules = s.config.rules;
  if (!rules.size_scaling || !e.size_scaling) return {};
  auto& period = *e.size_scaling;
  const auto& rule = *rules.size_scaling;
  if (++period.period_days < static_cast<std::uint64_t>(rule.days)) return {};
  Money increase;
  if (e.status == EvaluationStatus::Active && e.starting_balance < rule.max_balance &&
      size_scaling_profit(e, balance) >= size_scaling_required(e, rule) &&
      e.payouts.size() - period.payouts_at_start >= static_cast<std::uint64_t>(rule.payouts)) {
    increase = size_scaling_increase(e, rule);
    if (increase > Money{}) {
      const auto old = e.starting_balance;
      e.starting_balance = old + increase;
      s.ledger.add_capital(increase);
      e.peak = e.peak + increase;
      if (rules.max_drawdown > Money{}) e.floor = e.floor + increase;
      if (rules.lock_balance > Money{}) rules.lock_balance = rules.lock_balance + increase;
      const auto scale_loss = [&](Money original) {
        __extension__ using Wide = __int128;
        const Wide cents = static_cast<Wide>(original.micros()) * e.starting_balance.micros() /
            period.original.micros() / 10'000;
        if (cents > std::numeric_limits<std::int64_t>::max() / 10'000)
          throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Scaled loss limit overflow");
        return Money::from_micros(static_cast<std::int64_t>(cents) * 10'000);
      };
      rules.max_drawdown = scale_loss(period.original_max_drawdown);
      rules.daily_loss_limit = scale_loss(period.original_daily_loss_limit);
      period.history.push_back({next_day, old, e.starting_balance});
      event(events, "account_scaled", Json{{"old", old}, {"size", e.starting_balance}, {"increase", increase},
          {"period_start", period.period_started}, {"period_end", e.day}, {"day", next_day}});
    }
  }
  period.period_started = next_day;
  period.period_days = 0;
  period.period_balance = balance + increase;
  period.period_size = e.starting_balance;
  period.payouts_at_start = e.payouts.size();
  return increase;
}
/// Cancel an open order now, with the reason it records.
void cancel_order(State& s, OrderId id, Decision reason, Events& events) {
  if (!s.orders.at(static_cast<std::size_t>(id - 1)).open()) return;
  auto& order = s.orders.mut(id - 1);
  order.status = OrderStatus::Cancelled;
  order.reason = std::move(reason);
  order.ended_at = s.time;
  event(events, "cancel", order);
}
bool personal_reason(std::string_view reason) {
  return reason == "SOFT_FLOOR" || reason == "TRADE_LIMIT" || reason == "COOLDOWN" || reason == "PROFIT_LOCK";
}
Reason guardrail_reason(const State& s) {
  if (!s.guardrails.latched.empty()) return s.guardrails.latched.front();
  return s.time < s.guardrails.cooldown_until ? Reason::COOLDOWN : Reason::NONE;
}
std::string clock_text(Timestamp time);
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
}
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
      o.changes.push_back(change);
      event(events, "order_walked", Json{{"order_id", id}, {"change", change}});
      for (const auto& symbol : order_symbols(o.request)) s.walked.insert(symbol);
    }
  }
}
void decide(State& s, const PlanVerdict& verdict, Money equity, Events& events);
Decision entry_check(const State& s, const Order& o) {
  if (s.config.rules.underlyings.empty() && !s.config.rules.trading_start && !s.config.rules.flat_time && s.config.rules.events.empty() &&
      s.config.rules.hold_restrictions.empty()) return {};
  if (o.system || kept_within(o) || closing_only(s, o)) return {};
  for (const auto& symbol : order_symbols(o.request))
    if (const auto c = s.contracts.find(symbol); c != s.contracts.end())
      if (const auto d = plan_entry_check(s.config.rules, c->second.underlying, s.time); !d.ok()) return d;
  return {};
}
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
void flatten_positions(State& s, std::string_view label, Events& events, const std::string& scope = {});
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
  walk_limits(s, events);
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
  return disallowed_shorts(legs, stocks, s.config.rules.account_type);
}
/// A cash account or IRA refuses additional uncovered calls or short shares.
/// Pending orders' sells reserve their covers, just as for defined risk.
std::vector<std::pair<std::string, Quantity>> pending_sells(const State& s, OrderId except,
    std::vector<std::pair<std::string, Quantity>> extra);
Decision account_type_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            const std::map<std::string, Quantity>& shares = {}, OrderId except = 0) {
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
                            std::string_view message = "This plan allows defined risk only: cover each short option with a "
                                                       "long of the same type that expires with it or later, or open the "
                                                       "spread as one order") {
  if (s.config.rules.defined_risk && uncovered(s, order) > uncovered(s, {})) return failure(Reason::DEFINED_RISK, std::string(message));
  return {};
}
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
/// Exposure limits guard what pending orders could add: an order is refused only
/// when it raises a bucket's worst reachable exposure above its limit, so a book
/// that is already over a limit can still close, hedge and trade elsewhere.
Decision exposure_check(const State& s, const Order& o, const Measures& snapshot) {
  if (!snapshot.risk.complete) return check_exposure(snapshot.risk);
  std::vector<const Order*> working;
  for (const auto id : open_ids(s))
    if (const auto& other = s.orders[id - 1]; other.id != o.id && !shadowed(s, other)) working.push_back(&other);
  const auto without = portfolio_risk(s.ledger, working, s.contracts, s.valuations, s.config.limits, s.time, snapshot.stock_prices);
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
Decision account_check(const State& s, bool reducing = false, OrderId id = 0) {
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
/// Trade risk uses the same bounded expiry payoff as preview, but no fees and
/// no scenario-grid substitution for an unbounded or unknown loss.
std::optional<Money> expiry_loss(const State& s, const OrderRequest& request, Money premium, Money fees);
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
const Trigger* protective_stop(const OrderRequest& r) {
  if (!r.bracket || !r.bracket->stop_loss || !r.bracket->stop_loss->trigger) return nullptr;
  const auto& t = *r.bracket->stop_loss->trigger;
  if (t.source == TriggerSource::Underlying) return &t;
  const auto direction = multi_leg(r) || r.side == Side::Sell ? TriggerDirection::AtOrAbove : TriggerDirection::AtOrBelow;
  return t.direction == direction ? &t : nullptr;
}
struct TradeRisk {
  std::optional<Money> loss;
  std::string basis;
};
TradeRisk trade_risk(const State& s, const OrderRequest& r) {
  auto legs = r.legs;
  if (legs.empty()) legs.push_back({r.symbol, r.side, 1});
  Money net;
  if (r.limit_price) {
    net = !multi_leg(r) && r.side == Side::Sell ? -*r.limit_price : *r.limit_price;
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
/// When an order is checked: as it is accepted or changed, as its trigger is
/// reached, or as it fills. A stop-limit's limit is set against its stop rather
/// than the market: it is banded around an option or combo trigger's level when
/// accepted, and once triggered only the price it fills at is banded, so a market
/// that gaps through the limit leaves it working instead of cancelled.
enum class Stage { Accept, Activate, Fill };
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
  const auto net = *executable_net(s, o);
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
Decision min_hold_check(const State& s, const Order& o) {
  if (s.config.rules.min_hold_seconds == 0 || o.system || kept_within(o) || o.request.exits_only ||
      o.request.trigger || o.oco != 0 || !o.request.oco.empty()) return {};
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
Decision order_check(const State& s, const Order& o, Stage stage = Stage::Accept) {
  if (const auto d = min_hold_check(s, o); !d.ok()) return d;
  if (const auto d = account_check(s, closing_only(s, o), o.id); !d.ok()) return d;
  if (const auto d = entry_check(s, o); !d.ok()) return d;
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
    if (const auto d = price_check(s, quote, order_price(s, o, request.symbol, request.side, request.limit_price)); !d.ok()) return d;
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
void on_fill(State& s, OrderId id, Quantity units, Events& events);
void annotate_opening(State& s, const OrderRequest& r, const std::string& symbol, Quantity contracts, Events& events) {
  const auto q = held(s, symbol);
  if ((q != 0 && ((q > 0) == (contracts > 0) || magnitude(contracts) <= magnitude(q))) ||
      (r.note.empty() && r.tags.empty())) return;
  auto a = clean_annotation(r.note, r.tags);
  store_annotation(s, std::to_string(s.fills.size() + 1), std::move(a), events);
}
void observe_equity(State& s, Events& events);
/// One trade of the trader's own, counted toward the day's trading while a rule reads
/// it: an execution of their order (a bracket exit included, the account's own closes
/// not), or a sale or buy-back of delivered shares.
void count_trade(State& s) {
  if (counts_executions(s.config.rules)) ++s.evaluation.day_executions;
  if (s.config.rules.inactivity_days > 0) s.evaluation.last_activity = s.time;
}
void count_execution(State& s, const Order& order) {
  if (!order.system) count_trade(s);
}
/// Stale marks, an invalid quote or a missing valuation hold a fill back until a
/// later batch brings the data; they say nothing about the order itself.
bool data_gap(Reason code) {
  return code == Reason::STALE_QUOTE || code == Reason::INVALID_QUOTE || code == Reason::MISSING_VALUATION;
}
void match_one(State& s, OrderId id, Events& events, std::optional<OrderId> incoming) {
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
  if (!given && !inside && !marketable(o, s.books.at(o.request.symbol).quote)) return;
  if (s.config.rules.impact_ticks > 0 && o.request.limit_price && !given && !inside) {
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
  if (!given && !inside && !marketable(o, book.quote)) return;
  const Money price = given ? Money{} : order_price(s, o, symbol, side, o.request.limit_price);
  const auto remaining_depth = side == Side::Buy ? book.ask_left : book.bid_left;
  const auto size = side == Side::Buy ? book.quote.ask_size : book.quote.bid_size;
  const auto position = held(s, o.request.symbol);
  const auto capacity = o.system && s.evaluation.status != EvaluationStatus::Active
      ? closing_capacity(s, o.request.symbol, o.request.side)
      : o.request.side == Side::Sell ? std::max<Quantity>(position, 0) : std::max<Quantity>(-position, 0);
  // A long given away needs no bid, so no displayed size limits it.
  const auto budget = given ? capacity
      : s.config.rules.impact_ticks > 0 && !touch_stop(s, o) && !inside ? size - depth_used(size, remaining_depth) % size : remaining_depth;
  const Quantity quantity = reducing ? std::min({o.remaining(), budget, capacity}) : std::min(o.remaining(), budget);
  if (quantity <= 0) return;
  decision = reducing ? Decision{} : opening_fill_check(s, o, quantity, events);
  if (data_gap(decision.code)) return;
  if (decision.ok() && !reducing) decision = price_check(s, book.quote, price);
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
  if (!given) {
    auto& left = s.books[symbol];
    consume_depth(side == Side::Buy ? left.ask_left : left.bid_left, quantity);
  }
  auto& order = s.orders.mut(id - 1);
  order.filled_quantity += quantity;
  order.filled_notional = order.filled_notional + price * quantity;
  order.status = order.remaining() == 0 ? OrderStatus::Filled : OrderStatus::PartiallyFilled;
  if (order.status == OrderStatus::Filled) order.ended_at = s.time;
  Fill fill{static_cast<std::uint64_t>(s.fills.size() + 1), id, symbol, side,
            quantity, price, fee, quote.observation, quote.time, s.time, context, order.actor, taken,
            s.config.rules.fees ? std::optional(fees) : std::nullopt};
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
              s.config.rules.fees ? std::optional(slice_fees[i]) : std::nullopt};
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
    event(events, "order_repriced", changed);
  }
  return true;
}
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
  if (s.config.rules.fill_latency_ms > 0) end_ioc(s, id, "IOC exhausted eligible liquidity", events);
}
void match_symbols(State& s, const std::set<std::string>& symbols, Events& events,
                   std::optional<OrderId> incoming = {}) {
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
void shrink_exit(State& s, OrderId id, Quantity capacity, Events& events) {
  if (s.orders[id - 1].remaining() <= capacity) return;
  auto& exit = s.orders.mut(id - 1);
  // A held spread's exits were submitted; a retry still finds their original terms.
  if (exit.request.exits_only && !exit.submitted) exit.submitted = exit.request;
  exit.request.quantity = exit.filled_quantity + capacity;
  event(events, "order_resized", exit);
}
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
/// `value` rounded down (or up) to a multiple of `step`, which is positive.
Money floor_to(Money value, std::int64_t step) {
  auto q = value.micros() / step;
  if (value.micros() % step != 0 && value.micros() < 0) --q;
  return Money::from_micros(q * step);
}
Money ceil_to(Money value, std::int64_t step) { return -floor_to(-value, step); }
/// A trailing trigger follows its reference while armed: when the value it reads
/// less the trail (plus it, for at_or_above) passes the level, the level moves
/// there in whole cents, and it never moves back. A stop-limit's limit moves
/// with it, kept on the tick by moving both a little less, never more.
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
  if (limit) {
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
    auto& order = s.orders.mut(id - 1);
    order.status = OrderStatus::Working;
    order.triggered_at = s.time;
    if (order.request.tif == TimeInForce::Day) order.day_end = day_deadline(s, order.request, s.time);
    event(events, "order_triggered", order);
  }
  const auto symbols = order_symbols(s.orders.at(static_cast<std::size_t>(id - 1)).request);
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  if (s.config.rules.fill_latency_ms == 0) end_ioc(s, id, "IOC exhausted available displayed liquidity", events);
}
/// Armed orders activate in eligible sessions; only EXTO/GTC_EXTO extend them.
void check_triggers(State& s, Events& events) {
  const auto visit = [&](OrderId id) {
    const auto& o = s.orders[id - 1];
    const auto& contract = s.contracts.at(order_symbols(o.request).front());
    if (o.status != OrderStatus::Armed || !trades_now(o, contract, s.time)) return;
    // A trail follows what the trigger reads first, in the regular session; it
    // cannot reach the level it sets.
    if (regular(contract, s.time)) trail_level(s, id, events);
    if (reached(s, s.orders[id - 1])) activate(s, id, events);
  };
  // The open orders in ID order, then any placed while activating them (a
  // bracket's exits), in order, as one pass over every order would find them.
  const auto placed = s.orders.size();
  for (const auto id : open_ids(s)) if (id <= placed) visit(id);
  for (auto i = placed; i < s.orders.size(); ++i) visit(static_cast<OrderId>(i + 1));
}
/// One of a flatten's closes: a single contract, or a short and the long that covers
/// it closing together (the short bought back, the long sold), `units` of each.
struct PlannedClose {
  std::vector<Leg> legs;
  Quantity units = 0;
};
std::int64_t milli_strike(const md::OptionContract& c) { return std::llround(c.strike * 1000); }
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
/// Submit a reducer-owned market IOC that closes one position against the
/// current fresh book. Without executable liquidity nothing is recorded, so a
/// rule keeps retrying on later transactions instead of accumulating orders.
void flatten(State& s, const std::string& symbol, std::string_view why, Events& events) {
  const auto q = held(s, symbol);
  const auto contract = s.contracts.find(symbol);
  if (q == 0 || contract == s.contracts.end() || s.time >= contract->second.expiry_time() ||
      !regular(contract->second, s.time) || !quote_check(s, symbol).ok())
    return;
  const auto side = q > 0 ? Side::Sell : Side::Buy;
  const auto quantity = s.evaluation.status != EvaluationStatus::Active ? closing_capacity(s, symbol, side) : magnitude(q);
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
/// Why shares cannot trade now: they need the stock market's regular session and
/// the underlying's fresh price.
Decision share_close_check(const State& s, const std::string& symbol) {
  if (!md::market_session(s.time).open) return failure(Reason::SESSION_CLOSED, "Stock trades in the regular session");
  if (!stock_price(s, symbol)) return failure(Reason::STALE_QUOTE, "Needs a fresh price for " + symbol);
  return {};
}
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
void flatten_positions(State& s, std::string_view label, Events& events, const std::string& scope) {
  std::vector<std::string> symbols;
  for (const auto& [symbol, position] : s.ledger.positions())
    if (scope.empty() || s.contracts.at(symbol).underlying == scope) symbols.push_back(symbol);
  std::map<std::string, Quantity> held_back;
  const auto closes = plan_closes(s, symbols, held_back);
  for (const auto& close : closes) {
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
    if (scope.empty() || symbol == scope) stocks.emplace_back(symbol, stock.shares);
  for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Rule, events);
  if (!holds_scope(s, {}, s.time)) s.evaluation.flat_pending = false;
}
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
}
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
  if (rules.phase == Phase::Funded || (rules.trade_consistency_percent == 0 && rules.min_trades == 0 && rules.microscalp_percent == 0)) return;
  auto& e = s.evaluation;
  struct Whole { Money pnl; bool closed = true; std::uint64_t first = 0; };
  const State::TradeObjectives inputs{s.fills.size(), s.closures.size(), s.stock_fills.size(), s.dividends.size(),
                                      e.attempt, e.first_fill, e.first_stock_fill, s.groups};
  if (const auto& cached = s.trade_objectives; cached && cached->fills == inputs.fills &&
      cached->closures == inputs.closures && cached->stock_fills == inputs.stock_fills &&
      cached->dividends == inputs.dividends && cached->attempt == inputs.attempt &&
      cached->first_fill == inputs.first_fill && cached->first_stock_fill == inputs.first_stock_fill &&
      cached->groups.same(inputs.groups)) return;
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
  }
  if (rules.microscalp_percent > 0)
    for (const auto& life : share_lifecycles(s.stock_fills, s.dividends))
      if (life.fills.front() >= e.first_stock_fill && life.closed &&
          *life.closed - life.opened < rules.microscalp_seconds * md::kNanosPerSecond)
        e.short_profit = e.short_profit + std::max(Money{}, life.gross + life.dividends);
  e.best_trade.reset();
  e.closed_trades = 0;
  std::uint64_t best_first = 0;
  for (const auto& [id, trade] : trades) {
    if (!trade.closed) continue;
    if (rules.min_trades > 0) ++e.closed_trades;
    if (rules.trade_consistency_percent == 0 || trade.pnl <= Money{}) continue;
    if (!e.best_trade || trade.pnl > e.best_trade->pnl ||
        (trade.pnl == e.best_trade->pnl && trade.first < best_first)) {
      e.best_trade = BestTrade{id, trade.pnl};
      best_first = trade.first;
    }
  }
  s.trade_objectives = inputs;
}
void observe_equity(State& s, Events& events) {
  refresh_trade_objectives(s);
  const auto& rules = s.config.rules;
  auto& e = s.evaluation;
  // A journal created before any market data starts at time zero; the attempt
  // begins at the first real market time instead.
  if (e.started == 0 && s.time > 0) e.started = s.time;
  // Likewise the first payout cycle; journals from before payouts start it here too.
  if (e.cycle_started == 0) e.cycle_started = e.started;
  if (e.size_scaling && e.size_scaling->period_started < plan_trading_date(rules, e.started))
    e.size_scaling->period_started = plan_trading_date(rules, e.started);
  if (const auto equity = marked_equity(measure(s))) {
    if (plan_trading_date(rules, s.time) == e.day) {
      e.day_close_equity = *equity;
      if (!e.day_low_equity || *equity < *e.day_low_equity) { e.day_low_equity = *equity; e.day_low_at = s.time; }
      if (!e.day_high_equity || *equity > *e.day_high_equity) { e.day_high_equity = *equity; e.day_high_at = s.time; }
    }
    // A crossed holding boundary is decided at rollover; a close in the meantime
    // cannot turn that pending failure into a pass.
    if (e.status == EvaluationStatus::Active && e.holding_violations.empty()) {
      const auto verdict = evaluate_plan(e, rules, plan_inputs(s, *equity));
      // Include the deciding observation, then freeze the closest approach. Fills
      // after the decision still mark equity but never evaluate the attempt again.
      if (rules.max_drawdown > Money{} && e.started > 0 && (!e.closest_floor || *equity - e.floor < *e.closest_floor)) {
        e.closest_floor = *equity - e.floor;
        e.closest_floor_at = s.time;
      }
      if (verdict.decided()) decide(s, verdict, *equity, events);
      else if (verdict.lock) lock_day(s, verdict, *equity, events);
    }
    const auto soft_floor = soft_floor_of(s);
    const auto& g = s.config.guardrails;
    if (soft_floor && *equity <= *soft_floor) latch_guardrail(s, Reason::SOFT_FLOOR, events);
    if (g.profit_lock > Money{} && *equity - s.start_equity >= g.profit_lock) latch_guardrail(s, Reason::PROFIT_LOCK, events);
    if (g.max_opening_trades > 0 && s.guardrails.opening_trades >= g.max_opening_trades)
      latch_guardrail(s, Reason::TRADE_LIMIT, events);
  }
}
/// The client ID label of a plan's liquidation: the decision's, else a locked day's.
std::string_view liquidation_label(const Evaluation& e, bool decided) {
  const auto code = decided ? e.decision_code : e.day_lock;
  if (code == Reason::DAILY_LOSS_LIMIT) return "daily_loss";
  if (code == Reason::TIME_LIMIT) return "time_limit";
  if (code == Reason::INACTIVITY) return "inactivity";
  if (code == Reason::OVERNIGHT_HOLD) return "overnight";
  if (code == Reason::HOLD_RESTRICTED) return "hold";
  if (decided) return e.status == EvaluationStatus::Passed ? "target" : "drawdown";
  return "day_lock";
}
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
  }
  std::vector<std::string> symbols;
  for (const auto& [symbol, position] : s.ledger.positions()) symbols.push_back(symbol);
  // Shorts first, as Flatten closes them: a short bought back before the long that
  // covers it is sold never leaves a naked short, even for one event.
  std::stable_partition(symbols.begin(), symbols.end(), [&](const std::string& symbol) { return held(s, symbol) < 0; });
  for (const auto& symbol : symbols) {
    if (soft || decided || locked) {
      // The plan's decision names the liquidation, then a locked day, even when the
      // soft floor latched with it.
      flatten(s, symbol, decided || locked ? liquidation_label(e, decided) : "soft_floor", events);
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
      flatten(s, symbol, "expiry", events);
    }
  }
}
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
std::map<std::string, double> fresh_stock_prices(const State& s) {
  std::map<std::string, double> prices;
  for (const auto& [symbol, stock] : s.ledger.stocks())
    if (const auto price = stock_price(s, symbol)) prices[symbol] = price->dollars();
  return prices;
}
BreachRisk breach_of(const State& s, const std::map<std::string, double>& close_variances) {
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
std::string decimal(double value, int places) {
  std::array<char, 64> text{};
  std::snprintf(text.data(), text.size(), "%.*f", places, value);
  return text.data();
}
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
std::vector<RiskWarning> warnings_of(const State& s, const std::map<std::string, double>& close_variances,
                                     const std::vector<Dividend>& dividends) {
  std::vector<RiskWarning> out;
  const auto now = measure(s);
  const auto add = [&](std::string code, bool urgent, std::string scope, std::string symbol, std::string message,
                       std::optional<double> actual, std::optional<double> limit) {
    out.push_back({std::move(code), urgent ? "warning" : "info", std::move(scope), std::move(symbol), std::move(message), actual, limit});
  };
  if (now.risk.complete) {
    // Over a limit, orders that add to the excess are refused; closes and hedges still go.
    const auto over = [&](const std::string& scope, const RiskBucket& b) {
      const auto name = scope == "aggregate" ? std::string("The account's") : scope + "'s";
      if (const auto delta = std::abs(b.position.dollar_delta); delta > b.limits.dollar_delta)
        add("DELTA_LIMIT", true, scope, {}, name + " dollar delta " + whole(b.position.dollar_delta) + " is over its " +
            whole(b.limits.dollar_delta) + " limit: orders that add to it are refused, while closes and hedges still go",
            delta, b.limits.dollar_delta);
      if (const auto vega = std::abs(b.position.vega); vega > b.limits.vega)
        add("VEGA_LIMIT", true, scope, {}, name + " vega " + whole(b.position.vega) + " is over its " + whole(b.limits.vega) +
            " limit: orders that add to it are refused, while closes and hedges still go", vega, b.limits.vega);
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
    const Money strike = Money::from_double(c.strike);
    const Money intrinsic = std::max(Money{}, c.type == pricing::OptionType::Call ? *price - strike : strike - *price);
    if (intrinsic >= Money::from_micros(10'000)) expiring.push_back({symbol, *price, intrinsic});
  }
  if (!expiring.empty()) {
    // Buying power once every one of them has delivered, at today's prices.
    State delivered_state = s;
    for (const auto& x : expiring) {
      const auto& c = delivered_state.contracts.at(x.symbol);
      const auto shares = x.intrinsic > Money{} ? delivered(c, held(delivered_state, x.symbol)) : 0;
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
      const auto shares = delivered(c, contracts);
      const Money strike = Money::from_double(c.strike);
      const auto cutoff = c.last_trade_time() - rules.expiry_cutoff;
      const bool before_cutoff = rules.expiry_cutoff > 0 && s.time < cutoff;
      const auto expiry = before_cutoff
          ? "the account closes the position at its pre-expiry cutoff at " + clock_text(cutoff) +
            " at the market; only contracts still held into expiry because a close cannot fill are "
          : std::string("held into expiry ") + (magnitude(contracts) == 1 ? "it is " : "they are ");
      add("EXPIRY_DELIVERY", !before_cutoff && power < Money{}, c.underlying, x.symbol,
          std::to_string(magnitude(contracts)) + " " + (contracts > 0 ? "long " : "short ") + contract_name(c) + " expiring today " +
          (magnitude(contracts) == 1 ? "is " : "are ") + dollars(x.intrinsic) + " in the money: " + expiry +
          (contracts > 0 ? "exercised" : "assigned") + ", " +
          (shares > 0 ? "buying " : "selling ") + std::to_string(magnitude(shares)) + " " + c.underlying + " shares at the strike (" +
          dollars(strike * magnitude(shares)) + "). Buying power once the options expiring in the money today deliver: " + dollars(power),
          power.dollars(), 0.0);
    }
  }
  // Short American equity and ETF options the simulator assigns at a rollover:
  // those trading below their exercise value, and calls with less time value than
  // a dividend going ex on the new day (TradingSession::roll_day).
  const auto next_day = next_trading_day(s.day);
  const auto horizon = md::date_from_days(md::days_since_epoch(s.day) + 7);
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& c = s.contracts.at(symbol);
    if (position.quantity >= 0 || !physical(c) || c.expiry <= s.day) continue;
    const auto mark = s.marks.find(symbol);
    const auto price = underlying_price(s, symbol);
    if (mark == s.marks.end() || !price) continue;
    const Money strike = Money::from_double(c.strike);
    const bool call = c.type == pricing::OptionType::Call;
    const Money intrinsic = std::max(Money{}, call ? *price - strike : strike - *price);
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
    std::map<md::Date, Money> by_date;
    for (const auto& d : dividends)
      if (d.symbol == c.underlying && d.ex_date > s.day && d.ex_date <= std::min(horizon, c.expiry)) by_date[d.ex_date] = by_date[d.ex_date] + d.per_share;
    for (const auto& [date, dividend] : by_date) {
      if (time_value >= dividend) continue;
      const bool tonight = date <= next_day;
      add("EARLY_ASSIGNMENT", tonight, c.underlying, symbol, name + " ha" + (position.quantity == -1 ? "s " : "ve ") +
          dollars(time_value) + " of time value, less than the " + dollars(dividend) + " dividend going ex on " + md::format_date(date) +
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
    auto message = symbol + " goes ex-dividend on " + md::format_date(date) + ", " + dollars(per_share) + " a share";
    if (shares > 0) message += ": your " + std::to_string(shares) + " shares are paid " + dollars(per_share * shares);
    else if (shares < 0) message += ": your " + std::to_string(-shares) + " short shares pay " + dollars(per_share * -shares);
    else message += ": option prices allow for it, and short calls with less time value than it may be assigned the night before";
    add("EX_DIVIDEND", shares < 0, symbol, {}, message, per_share.dollars(), std::nullopt);
  }
  std::stable_sort(out.begin(), out.end(), [](const RiskWarning& a, const RiskWarning& b) {
    return a.severity == "warning" && b.severity != "warning";
  });
  return out;
}
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
    knots.push_back(Money::from_double(contract.strike));
  }
  if (call_slope < 0) return {};
  std::optional<Money> minimum;
  for (const auto spot : knots) {
    Money payoff;
    for (const auto& leg : legs) {
      const auto& contract = s.contracts.at(leg.symbol);
      const auto strike = Money::from_double(contract.strike);
      const auto intrinsic = std::max(Money{}, contract.type == pricing::OptionType::Call ? spot - strike : strike - spot);
      payoff = payoff + (intrinsic * contract.multiplier) * signed_contracts(leg, request.quantity);
    }
    minimum = minimum ? std::min(*minimum, payoff) : payoff;
  }
  return std::max(Money{}, premium + fees - *minimum);
}
struct PreviewProjection {
  OrderPreview result;
  State projected;
};
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
std::vector<OrderId> superseded_stops(const State& s, const OrderRequest& request);
void supersede(State& s, const OrderRequest& request, const std::vector<OrderId>& stops, Events& events);
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
/// The checks a resting entry's new terms take in change_order, on `s`, where the order has them.
Decision change_check(const State& s, const Order& order, const Decision& rejection) {
  if (!rejection.ok()) return rejection;
  auto decision = order_check(s, order);
  return decision.ok() ? open_orders_risk_check(s, order) : decision;
}
/// Order `id` with `change`'s terms in place, as change_order sets them, unchecked.
void change_terms(State& s, OrderId id, const OrderChange& change) {
  auto& order = s.orders.mut(static_cast<std::size_t>(id - 1));
  if (change.quantity) order.request.quantity = *change.quantity;
  if (change.limit_price && order.request.type == OrderType::Limit) {
    order.request.limit_price = *change.limit_price;
    order.limit_ticks.reset();
  }
  if (change.trigger_level && order.request.trigger) order.request.trigger->level = *change.trigger_level;
  if (change.tif) order.request.tif = *change.tif;
  if (change.walk) order.request.walk = *change.walk;
  if (order.request.walk || change.walk) order.walked_at = s.time;
}
/// The account a preview starts from: the current one with the integration's newer
/// market for contracts it may not hold yet, at the market time, its daily loss checked.
State prepared(const State& state, Timestamp time, const PreviewMarket& market) {
  State before = state;
  for (const auto& contract : market.contracts) {
    const auto symbol = contract.osi_symbol();
    if (!eligible(contract).ok()) continue;
    if (const auto saved = before.contracts.find(symbol); saved != before.contracts.end() && Json(saved->second) != Json(contract))
      throw TradingError(Reason::INVALID_CONTRACT, "Preview definition conflicts with registered terms");
    before.contracts[symbol] = contract;
  }
  for (const auto& quote : market.quotes) {
    if (!before.contracts.contains(quote.symbol) || quote.time > time || quote.time < 0) continue;
    const auto prior = before.books.find(quote.symbol);
    if (prior != before.books.end() && prior->second.quote.observation == quote.observation) {
      auto& book = before.books[quote.symbol];
      const auto first = book.quote.first_time();
      book.quote = quote;
      if (first < quote.time) book.quote.quoted = first;
    } else {
      before.books[quote.symbol] = {quote, valid_quote(quote) ? quote.bid_size : 0, markable_quote(quote) ? quote.ask_size : 0};
    }
    if (markable_quote(quote)) before.marks[quote.symbol] = {mark_of(quote), quote.time};
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
/// The room sizing shares: above the nearer of the plan and soft floors, if any.
std::optional<Money> floor_room(const State& s) {
  const auto snapshot = measure(s);
  std::optional<Money> room;
  if (s.config.rules.max_drawdown > Money{}) room = snapshot.equity - s.evaluation.floor;
  if (snapshot.soft_floor) room = room ? std::min(*room, snapshot.equity - *snapshot.soft_floor) : snapshot.equity - *snapshot.soft_floor;
  return room;
}
/// The value an alert watches now, when it is known and fresh: quotes and marks
/// within the quote age, valuations and indicators within the valuation age,
/// and account measures on complete marks (exposures on complete valuations).
std::optional<Money> alert_value(const State& s, const AlertCondition& c, const std::function<const Measures&()>& measures) {
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
        if (book == s.books.end() || !markable_quote(book->second.quote) ||
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
      if (c.metric == "dollar_delta" || c.metric == "vega" || c.metric == "theta") {
        if (!m.risk.complete) return std::nullopt;
        const auto& e = m.risk.aggregate.position;
        return money(c.metric == "dollar_delta" ? e.dollar_delta : c.metric == "vega" ? e.vega : e.theta);
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
  for (auto& alert : s.alerts.items) {
    if (!alert.armed && !alert.spec.repeat) continue;
    const auto& c = alert.spec.condition;
    std::optional<Money> value;
    try { value = alert_value(s, c, measures); }
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
void check_floor_share(double floor_share) {
  if (!std::isfinite(floor_share) || floor_share <= 0 || floor_share > 1)
    throw TradingError(Reason::INVALID_ORDER, "floor_share must be greater than zero and at most one");
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
struct Sizing {
  std::optional<Quantity> units;
  std::string basis;
  std::optional<Quantity> buying_power;
  std::optional<Quantity> floor;
};
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
        code == Reason::SCALING_LIMIT) return none(code == Reason::BUYING_POWER ? "buying_power" : "limits");
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
    return CommandResult{decision, stored.id, 0};
  }
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
  const auto second = place_one(s, std::move(other), time, {}, events, *first.order_id);
  if (!second.decision.ok()) {
    s = before;
    events.erase(events.begin() + static_cast<std::ptrdiff_t>(recorded), events.end());
    auto refused = second.decision;
    refused.message = "The order that cancels it: " + refused.message;
    return place_one(s, std::move(request), time, refused, events);
  }
  const auto a = *first.order_id, b = *second.order_id;
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
  for (const auto id : s.working) visit(id);
  for (auto i = s.indexed; i < s.orders.size(); ++i) visit(static_cast<OrderId>(i + 1));
}
std::string underlying_of(const State& s, const Order& o) {
  const auto c = s.contracts.find(order_symbols(o.request).front());
  return c == s.contracts.end() ? std::string{} : c->second.underlying;
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
    order.changes.push_back(record);
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
  order.changes.push_back(record);
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
void require_reason(const std::string& reason) {
  if (reason.find_first_not_of(" \t\r\n") == std::string::npos)
    throw TradingError(Reason::INVALID_REASON, "An explicit nonblank reason is required");
}

constexpr std::uint64_t kCheckpointEvery = 1000;
/// Keeps schema 3 records small: each carries its state as the change from
/// the record before ("delta") or whole, as a checkpoint ("state"). A journal
/// starts with a checkpoint and has one every kCheckpointEvery records, and
/// any change more than half the size of the last checkpoint is recorded whole.
class StateRecorder {
 public:
  /// Whether `state` is the one last recorded: a transaction that changed nothing.
  [[nodiscard]] bool recorded(const Json& state) const { return base_ && *base_ == state; }
  void add(Json& payload, Json state) {
    if (base_ && since_ + 1 < kCheckpointEvery) {
      auto delta = detail::state_delta(*base_, state);
      if (delta.dump().size() * 2 <= checkpoint_bytes_) {
        payload["delta"] = std::move(delta);
        base_ = std::move(state);
        ++since_;
        return;
      }
    }
    checkpoint_bytes_ = state.dump().size();
    payload["state"] = state;
    base_ = std::move(state);
    since_ = 0;
  }
 private:
  std::optional<Json> base_;
  std::uint64_t since_ = 0;
  std::size_t checkpoint_bytes_ = 0;
};

/// state_delta of one field's JSON, before and after, or nothing when it is
/// equal. History containers write out and compare only the subtrees they do not
/// share: a transaction copies the account, so the rest is untouched.
template <class T>
std::optional<Json> field_change(const T& before, const T& after) {
  const Json a = before, b = after;
  if (a == b) return std::nullopt;
  return detail::state_delta(a, b);
}
template <class T, std::size_t N>
std::optional<Json> field_change(const SharedVector<T, N>& before, const SharedVector<T, N>& after) {
  if (before.same(after)) return std::nullopt;
  const auto common = std::min(before.size(), after.size());
  Json changes = Json::object();
  std::size_t changed = 0;
  SharedVector<T, N>::compare(before, after, [&](std::size_t i, const T& old, const T& now) {
    const Json a = old, b = now;
    if (a == b) return;
    changes[std::to_string(i)] = detail::state_delta(a, b);
    ++changed;
  });
  if (changed == 0 && before.size() == after.size()) return std::nullopt;
  // As state_delta: an array rewritten in place is smaller whole.
  if (changed * 2 > common && common > 0) return Json{{"v", after}};
  for (std::size_t i = common; i < after.size(); ++i) changes[std::to_string(i)] = Json{{"v", after[i]}};
  return Json{{"a", std::move(changes)}, {"n", after.size()}};
}
template <class V, std::size_t N>
std::optional<Json> field_change(const SharedMap<std::string, V, N>& before, const SharedMap<std::string, V, N>& after) {
  if (before.same(after)) return std::nullopt;
  Json changes = Json::object();
  Json removed = Json::array();
  SharedMap<std::string, V, N>::compare(before, after,
      [&](const std::string& key, const V& value) { changes[key] = Json{{"v", value}}; },
      [&](const std::string& key) { removed.push_back(key); },
      [&](const std::string& key, const V& old, const V& now) {
        const Json a = old, b = now;
        if (a != b) changes[key] = detail::state_delta(a, b);
      });
  if (changes.empty() && removed.empty()) return std::nullopt;
  Json node{{"o", std::move(changes)}};
  if (!removed.empty()) node["d"] = std::move(removed);
  return node;
}
template <class K>
std::optional<Json> field_change(const SharedSet<K>& before, const SharedSet<K>& after) {
  if (before.same(after)) return std::nullopt;
  const auto common = std::min(before.size(), after.size());
  Json changes = Json::object();
  std::size_t changed = 0;
  SharedSet<K>::compare(before, after,
      [&](std::size_t i, const K& old, const K& now) {
        const Json a = old, b = now;
        if (a == b) return;
        changes[std::to_string(i)] = detail::state_delta(a, b);
        ++changed;
      },
      [&](std::size_t i, const K& key) { changes[std::to_string(i)] = Json{{"v", key}}; });
  if (changed == 0 && before.size() == after.size()) return std::nullopt;
  if (changed * 2 > common && common > 0) return Json{{"v", after}};
  return Json{{"a", std::move(changes)}, {"n", after.size()}};
}
/// state_delta(Json(before), Json(after)), found field by field without writing
/// out either state: the change a record carries. Equal states give {"o": {}}.
Json state_change(const State& before, const State& after) {
  Json changes = Json::object();
#define OPENPORT_STATE_CHANGE(field) \
  if (auto change = field_change(before.field, after.field)) changes[#field] = std::move(*change);
  OPENPORT_STATE_FIELDS(OPENPORT_STATE_CHANGE)
#undef OPENPORT_STATE_CHANGE
  // Indicators are written once there is one, and never emptied again.
  if (before.indicators.empty() && !after.indicators.empty()) changes["indicators"] = Json{{"v", after.indicators}};
  else if (auto change = field_change(before.indicators, after.indicators)) changes["indicators"] = std::move(*change);
  // Alerts likewise, once the account has created one.
  if (before.alerts.created == 0 && after.alerts.created > 0) changes["alerts"] = Json{{"v", after.alerts}};
  else if (auto change = field_change(before.alerts, after.alerts)) changes["alerts"] = std::move(*change);
  // Exercise instructions are written only while there are any (see to_json).
  const bool had = !before.do_not_exercise.empty(), has = !after.do_not_exercise.empty();
  if (has && !had) changes["do_not_exercise"] = Json{{"v", after.do_not_exercise}};
  else if (has) {
    if (auto change = field_change(before.do_not_exercise, after.do_not_exercise)) changes["do_not_exercise"] = std::move(*change);
  }
  Json node{{"o", std::move(changes)}};
  if (had && !has) node["d"] = Json::array({"do_not_exercise"});
  if (after.fee_charged.amount != Money{}) {
    if (before.fee_charged.amount == Money{}) node["o"]["fee_charged"] = Json{{"v", after.fee_charged}};
    else if (auto change = field_change(before.fee_charged, after.fee_charged)) node["o"]["fee_charged"] = std::move(*change);
  } else if (before.fee_charged.amount != Money{}) {
    if (!node.contains("d")) node["d"] = Json::array();
    node["d"].push_back("fee_charged");
  }
  return node;
}
/// When set, each record's change is also found by writing out both states,
/// and a difference throws (OPENPORT_VERIFY_JOURNAL).
const bool check_changes = std::getenv("OPENPORT_VERIFY_JOURNAL") != nullptr;
/// StateRecorder's records for a live session, from the committed state and the
/// next one instead of their JSON; a checkpoint writes the whole state.
class ChangeRecorder {
 public:
  /// Whether an earlier record wrote the committed state, to record changes from.
  [[nodiscard]] bool based() const { return based_; }
  /// Records `next` into `payload`: `change` (its change from the committed state,
  /// found before the version advanced) with the new version, or the whole state.
  void add(Json& payload, const State& next, std::optional<Json> change) {
    if (based_ && change && since_ + 1 < kCheckpointEvery) {
      auto delta = std::move(*change);
      delta["o"]["version"] = Json{{"v", next.version}};
      if (delta.dump().size() * 2 <= checkpoint_bytes_) {
        payload["delta"] = std::move(delta);
        ++since_;
        return;
      }
    }
    Json whole = next;
    checkpoint_bytes_ = whole.dump().size();
    payload["state"] = std::move(whole);
    based_ = true;
    since_ = 0;
  }
 private:
  bool based_ = false;
  std::uint64_t since_ = 0;
  std::size_t checkpoint_bytes_ = 0;
};

/// Reverify even caller-constructed recovery objects instead of trusting them.
JournalRecovery reverify(const JournalRecovery& recovery) {
  if (recovery.records.empty()) throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery requires a session_start record");
  std::string lines;
  try {
    for (const auto& r : recovery.records) {
      lines += Json{{"seq", r.seq}, {"time", r.time}, {"type", r.type}, {"payload", Json::parse(r.payload)},
                    {"prev_hash", r.prev_hash}, {"hash", r.hash}}.dump() + '\n';
    }
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  auto verified = verify_journal(lines, recovery.head);
  if (verified.records.front().type != "session_start") throw TradingError(Reason::JOURNAL_CORRUPT, "Missing session start");
  return verified;
}

/// Visit each transaction of a verified journal in order with the whole state
/// it left: read from the record (schemas 1 and 2, and schema 3 checkpoints) or
/// rebuilt by applying the record's delta. Each state must carry its record's
/// sequence and time, and each new config passes validation. The visitor gets
/// the payload without its state; returns the last state.
template <class Visit>
Json walk_states(const std::vector<JournalRecord>& records, Visit&& visit,
                 SharedVector<SettlementRecord>* settlements = nullptr) {
  Json state;
  bool chained = false;  // A delta may follow only a schema 3 record.
  Json validated;
  for (const auto& r : records) {
    auto payload = Json::parse(r.payload);
    std::map<std::string, Position> settling_positions;
    if (settlements && r.type == "settlement" && !state.is_null())
      settling_positions = state.at("ledger").at("positions").get<std::map<std::string, Position>>();
    const auto schema = payload.at("schema");
    const auto& ticks = payload.at("tick_policy");
    if ((schema != 1 && schema != 2 && schema != 3) || (ticks != "index-v1" && ticks != "v2"))
      throw TradingError(Reason::JOURNAL_CORRUPT, "Unsupported trading journal schema/policy");
    const auto whole = payload.find("state");
    if (schema == 3) {
      const auto delta = payload.find("delta");
      if ((whole == payload.end()) == (delta == payload.end()))
        throw TradingError(Reason::JOURNAL_CORRUPT, "A schema 3 record carries either a whole state or a delta");
      if (delta != payload.end()) {
        if (!chained) throw TradingError(Reason::JOURNAL_CORRUPT, "A state delta without a checkpoint before it");
        try { detail::apply_state_delta(state, *delta); }
        catch (const std::invalid_argument& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
      }
    } else {
      const auto& snapshot = payload.at("snapshot");
      if (snapshot.at("account_version") != r.seq || snapshot.at("time") != r.time)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded snapshot version/time does not match transaction");
      if (whole == payload.end()) throw TradingError(Reason::JOURNAL_CORRUPT, "Record without its state");
    }
    if (whole != payload.end()) {
      state = std::move(*whole);
      payload.erase(whole);
    }
    chained = schema == 3;
    if (!state.is_object() || state.at("version") != r.seq || state.at("time") != r.time)
      throw TradingError(Reason::JOURNAL_CORRUPT, "Recorded state version/time does not match transaction");
    if (const auto& config = state.at("config"); config != validated) {
      const auto c = config.get<SessionConfig>();
      validate_limits(c.limits);
      validate_scenarios(c.scenarios);
      validate_rules(c.rules);
      validate_guardrails(c.guardrails, true);
      validated = config;
    }
    if (settlements && r.type == "settlement") {
      for (const auto& e : payload.at("events")) {
        if (e.at("type") != "settlement") continue;
        const auto& data = e.at("payload");
        const auto symbol = data.at("symbol").get<std::string>();
        Money fee;
        if (state.contains("closures") && !state.at("closures").empty())
          fee = state.at("closures").back().value("fee", Money{});
        std::optional<SettlementSource> source;
        if (payload.contains("settlement_source") && !payload.at("settlement_source").is_null())
          source = payload.at("settlement_source").get<SettlementSource>();
        settlements->push_back(settlement_record(settling_positions.at(symbol), data.at("reference").get<Money>(),
            data.at("intrinsic").get<Money>(), fee, r.time, std::move(source)));
      }
    }
    visit(r, std::move(payload), std::as_const(state));
  }
  return state;
}
}  // namespace

PlanInputs plan_inputs(const TradingSnapshot& s) {
  PlanInputs in;
  in.time = s.time;
  in.equity = s.equity;
  in.balance = s.account.cash;
  for (const auto& p : s.positions) in.balance = in.balance + p.position.basis;
  for (const auto& p : s.stocks) in.balance = in.balance + p.position.basis;
  in.net_realised = s.account.realised - s.account.fees;
  in.flat = s.positions.empty() && s.stocks.empty();
  return in;
}
ProgramCosts program_costs(const TradingSnapshot& snapshot, const AccountRules& rules) {
  ProgramCosts out;
  const auto add = [&](const AttemptFee& fee) {
    if (fee.kind == "evaluation") out.evaluation = out.evaluation + fee.amount;
    else if (fee.kind == "reset") out.reset = out.reset + fee.amount;
    else if (fee.kind == "activation") out.activation = out.activation + fee.amount;
  };
  add(snapshot.fee_charged);
  for (const auto& a : snapshot.attempts) { add(a.fee_charged); out.payouts_received = out.payouts_received + a.payouts_received; }
  for (const auto& p : snapshot.evaluation.payouts) out.payouts_received = out.payouts_received + p.trader_share;
  if (snapshot.payouts_received) out.payouts_received = *snapshot.payouts_received;
  const auto& e = snapshot.evaluation;
  const auto original = e.size_scaling ? e.size_scaling->original : e.starting_balance;
  const auto original_rules = original_program_rules(rules, e.starting_balance, e.size_scaling);
  for (auto it = snapshot.attempts.rbegin(); it != snapshot.attempts.rend(); ++it) {
    const auto previous = it->size_scaling ? it->size_scaling->original : it->starting_balance;
    if (!it->rules || previous != original ||
        !same_program_rules(original_program_rules(*it->rules, it->starting_balance, it->size_scaling), original_rules)) break;
    ++out.resets_used;
  }
  return out;
}
PayoutQuote payout_quote(const TradingSnapshot& s, const AccountRules& rules) {
  __extension__ using Wide = __int128;
  const auto& p = rules.payouts;
  const auto& e = s.evaluation;
  PayoutQuote q;
  q.number = e.payouts.size() + 1;
  q.funded = rules.phase == Phase::Funded && p.qualifying_days > 0;
  q.active = e.status == EvaluationStatus::Active;
  q.flat = s.positions.empty() && s.open_orders.empty();
  q.qualifying_days = e.qualifying_days;
  q.required_days = p.qualifying_days;
  const auto add_day = [&](md::Date day, Money profit) {
    q.cycle_profit = q.cycle_profit + profit;
    if (profit > Money{} && (!q.best_day || profit > *q.best_day)) {
      q.best_day = profit;
      q.best_day_date = day;
    }
  };
  // Like qualifying_days, a finished day belongs to the cycle in progress at
  // rollover. The payout request's trading day belongs to the next cycle too.
  if (q.funded) {
    for (const auto& day : e.days)
      if (e.payouts.empty() || day.day >= e.payouts.back().day) add_day(day.day, day.realised);
    if (e.started > 0) add_day(e.day, s.account.realised - s.account.fees - e.day_open_realised);
  }
  bool consistent = true;
  if (!p.consistency_percents.empty()) {
    q.consistency_percent = p.consistency_percents.at(std::min<std::size_t>(q.number, p.consistency_percents.size()) - 1);
    const Wide best = q.best_day.value_or(Money{}).micros();
    const Wide missing = best * 100 - static_cast<Wide>(q.cycle_profit.micros()) * *q.consistency_percent;
    consistent = q.cycle_profit > Money{} && missing <= 0;
    if (missing > 0) {
      const Wide divisor = *q.consistency_percent * 10'000;
      const Wide micros = (missing + divisor - 1) / divisor * 10'000;
      if (micros > std::numeric_limits<std::int64_t>::max())
        throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Payout consistency profit needed exceeds the money range");
      q.consistency_needed = Money::from_micros(static_cast<std::int64_t>(micros));
    }
  }
  q.profit = s.equity - e.starting_balance;
  q.withdrawable = whole_cents(q.profit > Money{} ? q.profit.prorate(p.withdrawal_percent, 100) : Money{});
  if (!p.caps.empty()) q.cap = p.caps.at(std::min<std::size_t>(q.number, p.caps.size()) - 1);
  q.maximum = q.cap ? std::min(q.withdrawable, *q.cap) : q.withdrawable;
  // Touching the floor fails the account; an unlocked floor moves down with the
  // withdrawal, a locked one does not.
  if (rules.max_drawdown > Money{} && e.floor_locked)
    q.maximum = std::min(q.maximum, whole_cents(s.equity - e.floor - Money::from_micros(10'000)));
  bool buffer_blocks = false;
  if (p.buffer > Money{} && (p.buffer_payouts == 0 || q.number <= static_cast<std::uint64_t>(p.buffer_payouts))) {
    q.buffer_balance = e.starting_balance + p.buffer;
    const auto room = whole_cents(s.equity - *q.buffer_balance);
    buffer_blocks = room <= Money{};
    q.maximum = std::min(q.maximum, room);
  }
  q.minimum = p.minimum;
  q.trader_share = q.maximum.prorate(p.split_percent, 100);
  auto block = [&](Reason code, std::string message, std::optional<double> actual = {}, std::optional<double> limit = {}) {
    if (q.blocked.ok()) q.blocked = {code, std::move(message), actual, limit, "aggregate"};
  };
  if (!q.funded) block(Reason::PAYOUT_UNAVAILABLE, "Payouts are available on funded accounts");
  if (!q.active) block(Reason::PAYOUT_UNAVAILABLE, "The funded account is closed");
  if (!q.flat) block(Reason::PAYOUT_NOT_ELIGIBLE, "Close every position and working order before requesting a payout");
  if (static_cast<std::int64_t>(q.qualifying_days) < q.required_days)
    block(Reason::PAYOUT_NOT_ELIGIBLE, "Not enough qualifying days in this payout cycle",
          static_cast<double>(q.qualifying_days), static_cast<double>(q.required_days));
  if (!consistent)
    block(Reason::PAYOUT_NOT_ELIGIBLE, "Payout consistency requires a positive cycle profit and a best day of at most " +
          std::to_string(*q.consistency_percent) + "% of its net realised profit",
          q.cycle_profit > Money{} ? std::optional<double>{q.best_day.value_or(Money{}).dollars() / q.cycle_profit.dollars() * 100} : std::nullopt,
          static_cast<double>(*q.consistency_percent));
  if (q.maximum <= Money{} || q.maximum < q.minimum)
    block(Reason::PAYOUT_NOT_ELIGIBLE, buffer_blocks ? "Equity must stay above the payout buffer of " + dollars(*q.buffer_balance)
                                                   : "The payout available is below the minimum", q.maximum.dollars(), q.minimum.dollars());
  return q;
}

struct TradingSession::Impl {
  State state;
  std::optional<OpeningRuleInputs> opening_rule_inputs;
  bool compact_opening_rule_inputs = true;
  std::string actor = "system";
  std::shared_ptr<Journal> journal;
  std::shared_ptr<const TradingSnapshot> snapshot;
  bool stopped = false;
  /// A new session's first record, and the first after recovery, are checkpoints.
  ChangeRecorder recorder;
  /// Whether moving the clock alone to `time` could change anything. Flat with
  /// no open orders, and once the attempt has its start time, only an overdue
  /// calendar rule can decide it. Flat-time plans also publish clock progress and
  /// journal their daily trigger even with nothing held. Rollover is its own command.
  bool idle(Timestamp time) const {
    const auto& s = state;
    return !stopped && time >= s.time && s.ledger.positions().empty() && s.ledger.stocks().empty() &&
           !s.config.rules.flat_time &&
           s.evaluation.started > 0 && s.evaluation.cycle_started > 0 &&
           !evaluate_time_rules(s.evaluation, s.config.rules, time).decided() &&
           s.config.rules.events.empty() && s.config.rules.hold_restrictions.empty() &&
           s.guardrails.cooldown_until <= s.time &&
           open_ids(s).empty() && s.alerts.items.empty();
  }

  CommandResult transact(Timestamp time, std::string_view type,
                         const std::function<CommandResult(State&, Events&)>& action,
                         const std::optional<SettlementSource>& source = {}) {
    if (stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
    State next = state;
    next.opening_rule_inputs = opening_rule_inputs;
    next.compact_opening_rule_inputs = compact_opening_rule_inputs;
    Events events;
    advance(next, time, events);
    // Quote batches install their new book before monitor_rules closes positions.
    // Other commands must close against the current book before matching orders.
    if (type != "market" && next.evaluation.flat_pending && next.evaluation.status == EvaluationStatus::Active)
      flatten_positions(next, "flat_time", events);
    if (type != "market") monitor_calendar(next, events);
    next.actor = actor;
    auto result = action(next, events);
    place_chained(next, events);
    next.actor = "system";
    monitor_loss(next, events);
    monitor_rules(next, events);
    if (!next.walked.empty()) {
      match_symbols(next, next.walked, events);
      next.walked.clear();
      check_triggers(next, events);
    }
    next.opening_rule_inputs.reset();
    detail::update_reviews(next);
    refresh_trade_objectives(next);
    reindex(next);
    std::optional<Json> change;
    if (journal && recorder.based()) {
      change = state_change(state, next);
      if (check_changes && detail::state_delta(Json(state), Json(next)) != *change)
        throw std::logic_error("The journal change differs from a comparison of both states");
      // A transaction that changed nothing, as a batch at the same market time can,
      // leaves no record, version or publication; a rejection is still recorded.
      if (events.empty() && result.decision.ok() && change->at("o").empty()) {
        result.account_version = state.version;
        return result;
      }
    }
    if (next.version == std::numeric_limits<std::uint64_t>::max())
      throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Account version exhausted");
    ++next.version;
    auto publication = std::make_shared<TradingSnapshot>(snapshot_of(next));
    if (journal) {
      // Schema 3 records the state as a change from the record before, with
      // checkpoints, and no snapshot: recovery derives it from the state.
      // Tick policy v2 extends index-v1 with equity and ETF classes.
      Json payload{{"schema", 3}, {"tick_policy", "v2"}, {"actor", actor}, {"events", events}, {"decision", result.decision}};
      if (source) payload["settlement_source"] = *source;
      recorder.add(payload, next, std::move(change));
      try { journal->append(time, type, payload.dump()); }
      catch (...) {
        stopped = true;
        auto failed = std::make_shared<TradingSnapshot>(*snapshot);
        failed->journal_failed = true;
        failed->quality_flags.push_back(Reason::JOURNAL_IO);
        snapshot = std::move(failed);
        std::string message = "Journal commit failed";
        try { throw; }
        catch (const std::exception& error) { message += ": " + std::string(error.what()); }
        catch (...) {}  // Keep the recovery guidance for non-standard exceptions too.
        throw TradingError(Reason::JOURNAL_IO, message + "; no in-memory transition published; stop trading and recover");
      }
    }
    state = std::move(next);
    snapshot = std::move(publication);
    result.account_version = state.version;
    return result;
  }
};
TradingSession::TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal, std::string actor)
    : impl_(std::make_unique<Impl>()) {
  validate_limits(config.limits);
  validate_scenarios(config.scenarios);
  normalize_event_rules(config.rules, true);
  validate_rules(config.rules);
  validate_guardrails(config.guardrails);
  if (time < 0) throw TradingError(Reason::INVALID_TIME, "Negative session time");
  if (config.fee_per_contract < Money{}) throw TradingError(Reason::INVALID_MONEY, "Fee cannot be negative");
  if (journal && journal->sequence() != 0) throw TradingError(Reason::JOURNAL_CORRUPT, "Use recover for a nonempty journal");
  impl_->actor = std::move(actor);
  impl_->state.config = std::move(config);
  impl_->state.time = time;
  impl_->state.day = plan_trading_date(impl_->state.config.rules, time);
  impl_->state.ledger = Ledger(impl_->state.config.initial_cash);
  impl_->state.start_equity = impl_->state.config.initial_cash;
  impl_->state.evaluation = fresh_evaluation(impl_->state, 1);
  impl_->journal = std::move(journal);
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
  impl_->transact(time, "session_start", [](State& s, Events& events) {
    s.fee_charged = {s.config.rules.evaluation_fee, s.config.rules.evaluation_fee > Money{} ? "evaluation" : ""};
    Json start = s.config;
    if (s.fee_charged.amount != Money{}) start["fee_charged"] = s.fee_charged;
    event(events, "session_start", start);
    return CommandResult{};
  });
}
TradingSession::TradingSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TradingSession::~TradingSession() = default;
void TradingSession::set_actor(std::string actor) { impl_->actor = std::move(actor); }
void TradingSession::set_exact_margin(bool exact) {
  impl_->state.exact_margin = exact;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_opening_rule_inputs(std::optional<OpeningRuleInputs> inputs, bool compact) {
  impl_->opening_rule_inputs = std::move(inputs);
  impl_->compact_opening_rule_inputs = compact;
}
void TradingSession::record_input(std::string_view input, Timestamp time) {
  const auto data = Json::parse(input);
  if (!data.is_object()) throw std::invalid_argument("Run input must be an object");
  impl_->transact(time, "run_input", [&](State&, Events& events) {
    event(events, "run_input", data);
    return CommandResult{};
  });
}
TradingSession::TradingSession(TradingSession&&) noexcept = default;
TradingSession& TradingSession::operator=(TradingSession&&) noexcept = default;
CommandResult TradingSession::define(const md::OptionContract& contract, Timestamp time) {
  return impl_->transact(time, "definition", [&](State& s, Events& events) {
    const auto decision = eligible(contract);
    if (!decision.ok()) return CommandResult{decision, {}, 0};
    const auto symbol = contract.osi_symbol();
    const auto old = s.contracts.find(symbol);
    if (old != s.contracts.end() && Json(old->second) != Json(contract))
      return CommandResult{failure(Reason::INVALID_CONTRACT, "OSI definition conflicts with registered terms"), {}, 0};
    s.contracts[symbol] = contract;
    event(events, "definition", contract);
    return CommandResult{};
  });
}
CommandResult TradingSession::submit(OrderRequest request, Timestamp time, Decision rejection) {
  // A client retrying an order it may have lost the answer to gets that answer,
  // not a second order or a duplicate-key rejection, even after the order changed.
  // Other terms are refused without recording an order, so the ID keeps naming
  // one. Only this attempt's orders hold their IDs: after an account reset, an
  // ID an earlier attempt used names a new order.
  if (!impl_->stopped) {
    const auto& state = impl_->state;
    if (const auto named = state.clients.find(request.client_order_id);
        named != state.clients.end() && named->second >= state.evaluation.first_order) {
      const auto& first = state.orders.at(static_cast<std::size_t>(named->second - 1));
      if (first.submission() == request)
        return CommandResult{first.status == OrderStatus::Rejected ? first.reason : Decision{}, first.id, state.version, true};
      return CommandResult{failure(Reason::DUPLICATE_CLIENT_ID, "client_order_id already names order " + std::to_string(first.id) +
                                   ", submitted with other terms"), first.id, state.version, false};
    }
  }
  return impl_->transact(time, "submit", [&](State& s, Events& events) {
    monitor_loss(s, events);
    return place(s, std::move(request), time, rejection, events);
  });
}
RiskProfile snapshot_profile(const TradingSnapshot& snapshot, const SessionConfig& config,
    const Valuations& valuations, const ProfileConfig& profile, double reference_spot, const std::string& underlying) {
  std::map<std::string, Position> positions;
  std::map<std::string, StockPosition> stocks;
  std::map<std::string, double> prices;
  for (const auto& marked : snapshot.positions)
    if (underlying.empty() || marked.position.contract.underlying == underlying)
      positions.emplace(marked.position.contract.osi_symbol(), marked.position);
  for (const auto& marked : snapshot.stocks) {
    if (!underlying.empty() && marked.position.symbol != underlying) continue;
    stocks.emplace(marked.position.symbol, marked.position);
    if (marked.fresh && marked.mark) prices[marked.position.symbol] = marked.mark->dollars();
  }
  auto result = risk_profile(Ledger::restore({}, std::move(positions), std::move(stocks)), valuations, profile,
      reference_spot, snapshot.equity,
      config.rules.max_drawdown > Money{} ? std::optional(snapshot.evaluation.floor) : std::nullopt,
      snapshot.soft_floor, snapshot.time, config.limits.max_valuation_age, prices);
  if (!snapshot.valuation_complete) {
    result.complete = false;
    result.room.reset(); result.soft_room.reset();
    for (auto& curve : result.curves) {
      std::fill(curve.pnl.begin(), curve.pnl.end(), 0.0);
      curve.down.reset(); curve.up.reset(); curve.soft_down.reset(); curve.soft_up.reset();
    }
  }
  return result;
}
BreachRisk TradingSession::breach(const std::map<std::string, double>& close_variances) const {
  return breach_of(impl_->state, close_variances);
}
std::vector<RiskWarning> TradingSession::warnings(const std::map<std::string, double>& close_variances,
                                                  const std::vector<Dividend>& dividends) const {
  return warnings_of(impl_->state, close_variances, dividends);
}
/// What order `o` would trade at now on its executable sides: a single contract's
/// ask (buys) or bid (sells), a combo's signed net; nothing without fresh quotes.
std::optional<Money> executable_value(const State& s, const Order& o) {
  Order probe = o;
  probe.request.trigger = Trigger{multi_leg(o.request) ? TriggerSource::Combo : TriggerSource::Option, TriggerDirection::AtOrBelow, Money{}};
  return trigger_value(s, probe);
}
/// Advice on terms that are accepted but rarely meant (OrderPreview::warnings).
std::vector<OrderWarning> order_warnings(const State& s, const OrderRequest& r) {
  std::vector<OrderWarning> out;
  for (const auto& symbol : order_symbols(r))
    if (!s.contracts.contains(symbol) || !s.books.contains(symbol)) return out;
  const auto level_text = [](const Trigger& t) {
    return std::string(t.direction == TriggerDirection::AtOrBelow ? "at or below " : "at or above ") + t.level.str();
  };
  // A bracket's exits as attach_exits makes them; held exits already close.
  const auto exit_of = [&](const ExitSpec& spec, OrderRole role) {
    Order exit;
    exit.role = role;
    exit.request = r;
    exit.request.bracket.reset();
    exit.request.walk.reset();
    exit.request.exits_only = false;
    exit.request.trigger = spec.trigger;
    exit.request.limit_price = spec.limit_price;
    if (!r.exits_only) {
      exit.request.side = multi_leg(r) ? Side::Buy : r.side == Side::Buy ? Side::Sell : Side::Buy;
      for (auto& leg : exit.request.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
    }
    return exit;
  };
  const std::string fires = r.exits_only ? "as soon as it is accepted" : "as soon as the entry fills";
  // A combo's net limit is what it pays: an exit pays at most its limit.
  const auto marketable = [&](const Order& exit, Money value) {
    return exit.request.side == Side::Buy ? value <= *exit.request.limit_price : value >= *exit.request.limit_price;
  };
  if (r.bracket && r.bracket->stop_loss) {
    const auto exit = exit_of(*r.bracket->stop_loss, OrderRole::StopLoss);
    const auto value = executable_value(s, exit);
    if (!exit.request.trigger && exit.request.limit_price) {
      std::string message = "The stop_loss has a limit price and no trigger, so it rests as a limit exit at " +
          exit.request.limit_price->str() + ", not a stop";
      if (value && marketable(exit, *value)) message += "; the closing price now, " + value->str() + ", already reaches it, so it closes the position " + fires;
      out.push_back({"STOP_AS_LIMIT", message + ". Give the stop a trigger instead."});
    } else if (exit.request.trigger && reached(s, exit)) {
      out.push_back({"STOP_REACHED", "The stop's trigger, " + level_text(*exit.request.trigger) + ", is already reached (now " +
                     trigger_value(s, exit)->str() + "): it fires " + fires});
    }
  }
  if (r.bracket && r.bracket->take_profit && r.bracket->take_profit->limit_price && !r.bracket->take_profit->trigger) {
    const auto exit = exit_of(*r.bracket->take_profit, OrderRole::TakeProfit);
    if (const auto value = executable_value(s, exit); value && marketable(exit, *value))
      out.push_back({"TARGET_REACHED", "The take-profit, " + exit.request.limit_price->str() + ", is already marketable (now " +
                     value->str() + "): it fills " + fires});
  }
  if (r.trigger && !r.exits_only) {
    Order armed;
    armed.request = r;
    if (reached(s, armed))
      out.push_back({"TRIGGER_REACHED", "The trigger, " + level_text(*r.trigger) + ", is already reached (now " +
                     trigger_value(s, armed)->str() + "): the order activates at once"});
  }
  // Slippage that alone pushes a market order's price outside the band.
  if (!multi_leg(r) && !r.limit_price && s.config.rules.slippage_ticks > 0 && quote_check(s, r.symbol).ok()) {
    const auto& quote = s.books.at(r.symbol).quote;
    const auto displayed = r.side == Side::Buy ? *quote.ask : *quote.bid;
    const auto slipped = execution_price(s, r.symbol, r.side);
    if (price_check(s, quote, displayed).ok() && !price_check(s, quote, slipped).ok())
      out.push_back({"SLIPPAGE_BAND", "The account's " + std::to_string(s.config.rules.slippage_ticks) + " slippage ticks price this order at " +
                     slipped.str() + ", outside the price band around the mid " + mid(quote).str() +
                     ", so it is refused PRICE_BAND; widen the band or lower the slippage"});
  }
  return out;
}
OrderPreview TradingSession::preview(const OrderRequest& request, Timestamp time, double floor_share,
    Decision rejection, const std::map<std::string, double>& close_variances, const PreviewMarket& market) const {
  check_floor_share(floor_share);
  // Submit answers an identical retry before advancing market time. It cannot
  // create another fill or reserve more buying power, even if the feed changed.
  if (!impl_->stopped) {
    const auto& state = impl_->state;
    const auto named = state.clients.find(request.client_order_id);
    const auto* first = named == state.clients.end() || named->second < state.evaluation.first_order
        ? nullptr : &state.orders.at(static_cast<std::size_t>(named->second - 1));
    if (first && first->submission() == request) {
      const auto snapshot = snapshot_of(state);
      OrderPreview retry;
      retry.decision = first->status == OrderStatus::Rejected ? first->reason : Decision{};
      retry.buying_power_before = snapshot.buying_power.available;
      retry.buying_power_after = retry.buying_power_before;
      retry.exposure_change = Exposure{};
      retry.breach = breach_of(state, close_variances);
      retry.execution.status = first->status;
      retry.execution.reason = first->reason;
      return retry;
    }
  }
  const auto before = prepared(impl_->state, time, market);
  Events ignored;
  auto projection = project_order(before, request, rejection);
  auto result = projection.result;
  result.warnings = order_warnings(before, request);
  if (impl_->stopped) result.decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
  // What submitting it now would execute: the same acceptance and matching, on a copy.
  if (!impl_->stopped) {
    State trial = before;
    try {
      if (const auto placed = place(trial, request, time, rejection, ignored); placed.order_id) {
        result.execution = executed(before, trial, *placed.order_id, std::move(result.execution));
        result.next_walk = next_walk(trial.orders[*placed.order_id - 1]);
      }
    } catch (const TradingError&) {}
  }
  // Each leg's quote: whether it can fill now, its displayed size on the leg's
  // side and what this account's orders have left of that observation.
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  for (const auto& leg : legs) {
    Quantity contracts = 0;
    if (!before.contracts.contains(leg.symbol) || leg.ratio < 1 || request.quantity < 1 ||
        __builtin_mul_overflow(request.quantity, leg.ratio, &contracts))
      continue;
    LegLiquidity item{leg.symbol, leg.side, contracts, quote_check(before, leg.symbol), 0, 0};
    if (const auto book = before.books.find(leg.symbol); book != before.books.end()) {
      const bool buy = leg.side == Side::Buy;
      const auto& q = book->second.quote;
      const auto far = buy ? q.ask : q.bid;
      item.displayed = far && *far > Money{} ? (buy ? q.ask_size : q.bid_size) : 0;
      item.left = std::max<Quantity>(0, buy ? book->second.ask_left : book->second.bid_left);
      if (item.quote.code == Reason::INVALID_QUOTE) item.quote.message = quote_problem(book->second.quote);
    }
    result.liquidity.push_back(std::move(item));
  }
  result.breach = breach_of(projection.projected, close_variances);
  std::map<Quantity, OrderPreview> sized_previews;
  const auto sized_preview = [&](Quantity quantity) -> const OrderPreview& {
    const auto saved = sized_previews.find(quantity);
    if (saved != sized_previews.end()) return saved->second;
    auto sized = request;
    sized.quantity = quantity;
    OrderPreview value;
    try { value = project_order(before, sized, rejection).result; }
    catch (const TradingError& error) { value.decision = failure(error.code(), error.what()); }
    return sized_previews.emplace(quantity, std::move(value)).first->second;
  };
  // Held exits follow their position's size, so sizing them means nothing.
  if (impl_->stopped || request.exits_only) return result;
  Quantity upper = before.config.limits.max_order_contracts;
  for (const auto& leg : request.legs) {
    if (leg.ratio <= 0) return result;
    upper = std::min(upper, before.config.limits.max_order_contracts / leg.ratio);
  }
  const auto sizing = size_order(sized_preview, upper, floor_room(before), floor_share);
  result.max_units = sizing.units;
  result.max_units_basis = sizing.basis;
  result.max_units_buying_power = sizing.buying_power;
  result.max_units_floor = sizing.floor;
  return result;
}
OrderPreview TradingSession::preview_change(OrderId id, const OrderChange& change, Timestamp time, double floor_share,
    Decision rejection, const std::map<std::string, double>& close_variances, const PreviewMarket& market) const {
  check_floor_share(floor_share);
  const auto before = prepared(impl_->state, time, market);
  OrderPreview result;
  // An unknown or finished order is the change's answer, as modify gives it.
  if (id == 0 || id > before.orders.size()) {
    result.decision = failure(Reason::UNKNOWN_ORDER, "Unknown order ID");
    return result;
  }
  if (!before.orders.at(static_cast<std::size_t>(id - 1)).open()) {
    result.decision = failure(Reason::ORDER_TERMINAL, "Order is already terminal");
    return result;
  }
  // The change itself, on a copy: its decision, and what it executes at once.
  State trial = before;
  Events ignored;
  auto decision = change_order(trial, id, change, rejection, ignored).decision;
  if (impl_->stopped) decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
  // The order on its new terms, unmatched: what it reserves, and the account once it fills.
  State changed = before;
  change_terms(changed, id, change);
  auto projection = project_working(before, std::move(changed), id, decision);
  result = std::move(projection.result);
  result.breach = breach_of(projection.projected, close_variances);
  result.execution = executed(before, trial, id, std::move(result.execution));
  result.next_walk = next_walk(trial.orders[id - 1]);
  // Sizing counts the units the order could still work, its filled ones aside.
  const auto& order = before.orders.at(static_cast<std::size_t>(id - 1));
  if (impl_->stopped || kept_within(order) || order.request.exits_only) return result;
  Quantity upper = before.config.limits.max_order_contracts;
  for (const auto& leg : order.request.legs) {
    if (leg.ratio <= 0) return result;
    upper = std::min(upper, before.config.limits.max_order_contracts / leg.ratio);
  }
  std::map<Quantity, OrderPreview> sized_previews;
  const auto sized_preview = [&](Quantity units) -> const OrderPreview& {
    const auto saved = sized_previews.find(units);
    if (saved != sized_previews.end()) return saved->second;
    auto sized = change;
    sized.quantity = order.filled_quantity + units;
    State resized = before;
    change_terms(resized, id, sized);
    OrderPreview value;
    try {
      auto check = change_check(resized, resized.orders.at(static_cast<std::size_t>(id - 1)), rejection);
      value = project_working(before, std::move(resized), id, std::move(check)).result;
    } catch (const TradingError& error) { value.decision = failure(error.code(), error.what()); }
    return sized_previews.emplace(units, std::move(value)).first->second;
  };
  const auto sizing = size_order(sized_preview, upper - order.filled_quantity, floor_room(before), floor_share);
  result.max_units = sizing.units;
  result.max_units_basis = sizing.basis;
  result.max_units_buying_power = sizing.buying_power;
  result.max_units_floor = sizing.floor;
  return result;
}
WhatIf TradingSession::what_if(const std::vector<std::vector<OrderRequest>>& candidates, Timestamp time,
    const std::vector<std::vector<Decision>>& rejections, const std::map<std::string, double>& close_variances,
    const PreviewMarket& market) const {
  WhatIf result;
  const auto before = prepared(impl_->state, time, market);
  const auto base = measure(before).equity;
  result.current = what_if_account(before, base, close_variances);
  for (std::size_t c = 0; c < candidates.size(); ++c) {
    WhatIfCandidate candidate;
    State state = before;
    bool projected = true;
    for (std::size_t i = 0; i < candidates[c].size(); ++i) {
      const auto rejection = c < rejections.size() && i < rejections[c].size() ? rejections[c][i] : Decision{};
      const auto id = static_cast<OrderId>(state.orders.size() + 1);
      PreviewProjection projection{{}, state};
      try {
        projection = project_order(state, candidates[c][i], rejection);
      } catch (const TradingError& error) {
        projection.result.decision = failure(error.code(), error.what());
      }
      if (impl_->stopped) projection.result.decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
      // An order without a contract or a quote cannot be projected, so the account after it is unknown.
      const auto& orders = projection.projected.orders;
      projected &= orders.size() >= id && orders.at(static_cast<std::size_t>(id - 1)).status == OrderStatus::Filled;
      if (!projection.result.decision.ok() && candidate.decision.ok()) candidate.decision = projection.result.decision;
      candidate.orders.push_back(projection.result.decision);
      state = std::move(projection.projected);
    }
    if (projected) candidate.after = what_if_account(state, base, close_variances);
    candidate.after.projected = projected;
    result.candidates.push_back(std::move(candidate));
  }
  return result;
}
FlattenPreview TradingSession::preview_close_positions(std::optional<std::string> underlying, Timestamp time,
    const std::map<std::string, Decision>& rejections, const std::map<std::string, double>& close_variances,
    const PreviewMarket& market, FlattenPricing pricing) const {
  if (impl_->stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
  // The flatten itself, on a journal-less copy, so the dry run does what it would.
  auto copy = std::make_unique<Impl>();
  copy->state = prepared(impl_->state, time, market);
  copy->actor = impl_->actor;
  copy->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(copy->state));
  const State before = copy->state;
  TradingSession trial(std::move(copy));
  FlattenPreview result;
  const auto base = measure(before).equity;
  result.current = what_if_account(before, base, close_variances);
  const auto closed = trial.close_positions(underlying, time, rejections, pricing);
  const auto& after = trial.impl_->state;
  result.decision = closed.decision;
  result.kept_stocks = closed.kept_stocks;
  for (const auto id : open_ids(before))
    if (after.orders.at(static_cast<std::size_t>(id - 1)).status == OrderStatus::Cancelled) result.cancelled.push_back(id);
  for (auto i = before.orders.size(); i < after.orders.size(); ++i) result.orders.push_back(after.orders[i]);
  for (auto i = before.fills.size(); i < after.fills.size(); ++i) result.fills.push_back(after.fills[i]);
  for (auto i = before.stock_fills.size(); i < after.stock_fills.size(); ++i) result.stock_fills.push_back(after.stock_fills[i]);
  const auto in_scope = [&](const std::string& name) { return !underlying || name == *underlying; };
  for (const auto& [symbol, position] : after.ledger.positions())
    if (position.quantity != 0 && in_scope(after.contracts.at(symbol).underlying)) result.remaining[symbol] = position.quantity;
  for (const auto& [symbol, stock] : after.ledger.stocks())
    if (stock.shares != 0 && in_scope(symbol)) result.remaining_shares[symbol] = stock.shares;
  result.after = what_if_account(after, base, close_variances);
  return result;
}
CommandResult TradingSession::cancel(OrderId id, Timestamp time, Reason reason) {
  if (reason != Reason::USER_CANCEL && reason != Reason::PLAYBOOK_TIME_STOP && reason != Reason::PLAYBOOK_TRAILING_STOP &&
      reason != Reason::PLAYBOOK_DTE_STOP && reason != Reason::PLAYBOOK_DAYS_IN_TRADE_STOP)
    throw TradingError(Reason::INVALID_REASON, "Invalid cancellation reason");
  return impl_->transact(time, "cancel", [&](State& s, Events& events) {
    if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
    if (!s.orders.at(static_cast<std::size_t>(id - 1)).open())
      return CommandResult{failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
    const auto& order = s.orders.at(static_cast<std::size_t>(id - 1));
    if (reason == Reason::USER_CANCEL && s.config.rules.require_stop_loss && order.role == OrderRole::StopLoss &&
        exit_capacity(s, order.request) > 0)
      return CommandResult{failure(Reason::STOP_REQUIRED, "Close the protected position before cancelling its required stop-loss"), id, 0};
    cancel_order(s, id, failure(reason, reason == Reason::PLAYBOOK_TIME_STOP ? "Playbook time stop" : reason == Reason::PLAYBOOK_TRAILING_STOP ? "Playbook trailing stop" :
        reason == Reason::PLAYBOOK_DTE_STOP ? "Playbook DTE stop" : reason == Reason::PLAYBOOK_DAYS_IN_TRADE_STOP ? "Playbook days in trade stop" : "Cancelled by caller"), events);
    return CommandResult{{}, id, 0};
  });
}
CommandResult TradingSession::modify(OrderId id, OrderChange change, Timestamp time, Decision rejection) {
  return impl_->transact(time, "modify", [&](State& s, Events& events) {
    monitor_loss(s, events);
    return change_order(s, id, change, rejection, events);
  });
}
CommandResult TradingSession::cancel_all(std::optional<std::string> underlying, Timestamp time) {
  return impl_->transact(time, "cancel_all", [&](State& s, Events& events) {
    if (s.config.rules.require_stop_loss)
      for (const auto id : open_ids(s)) {
        const auto& o = s.orders[id - 1];
        if (o.role == OrderRole::StopLoss && (!underlying || underlying_of(s, o) == *underlying) && exit_capacity(s, o.request) > 0)
          return CommandResult{failure(Reason::STOP_REQUIRED, "Close protected positions before cancelling their required stops"), id, 0};
      }
    for (const auto id : open_ids(s))
      if (const auto& o = s.orders[id - 1]; o.open() && (!underlying || underlying_of(s, o) == *underlying))
        cancel_order(s, id, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{};
  });
}
namespace {
/// Accept or reject one of a flatten's reduce-only closes. The integration's gate,
/// the account and the session can refuse it whatever the price; missing or stale
/// quotes only make it wait. Accepted, it trades at once where it can and works
/// the rest on later quotes until its session ends.
Decision place_close(State& s, OrderRequest request, const Decision& rejection, FlattenPricing pricing, Events& events) {
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = std::move(request);
  order.actor = s.actor;
  order.accepted_at = s.time;
  order.reduce_only = true;
  if (pricing.limit) {
    order.limit_ticks = pricing.limit_ticks;
    order.request.limit_price = close_price(s, order).value_or(Money{});
  }
  add_order(s, order);
  // Written until matching starts; nothing copies the orders before that.
  auto& stored = s.orders.mut_back();
  auto decision = rejection.ok() ? account_check(s, true) : rejection;
  for (const auto& symbol : order_symbols(stored.request)) {
    if (!decision.ok()) break;
    const auto& contract = s.contracts.at(symbol);
    decision = s.time >= contract.expiry_time() ? failure(Reason::EXPIRED, "Contract has expired")
        : pricing.limit && !md::trading_session(contract.root, s.time).open
            ? failure(Reason::SESSION_CLOSED, "Flatten requires an open trading session")
            : session_check(contract, s.time, stored.request);
    if (!decision.ok() && multi_leg(stored.request)) decision.scope = symbol;
  }
  if (!decision.ok()) {
    stored.status = OrderStatus::Rejected;
    stored.reason = decision;
    event(events, "order_rejected", stored);
    return decision;
  }
  const auto id = stored.id;
  stored.day_end = order_end(s, stored.request, s.time);
  event(events, "order_accepted", stored);
  const auto symbols = order_symbols(stored.request);
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  return {};
}
}  // namespace
CommandResult TradingSession::cancel_orders(const std::vector<OrderId>& ids, Timestamp time) {
  return impl_->transact(time, "cancel_orders", [&](State& s, Events& events) {
    if (ids.empty()) return CommandResult{failure(Reason::INVALID_ORDER, "Name at least one order to cancel"), {}, 0};
    for (const auto id : ids)
      if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), id, 0};
    if (std::none_of(ids.begin(), ids.end(), [&](OrderId id) { return s.orders.at(static_cast<std::size_t>(id - 1)).open(); }))
      return CommandResult{failure(Reason::ORDER_TERMINAL, "Every listed order is already terminal"), ids.front(), 0};
    if (s.config.rules.require_stop_loss)
      for (const auto id : ids) {
        const auto& o = s.orders[id - 1];
        if (o.open() && o.role == OrderRole::StopLoss && exit_capacity(s, o.request) > 0)
          return CommandResult{failure(Reason::STOP_REQUIRED, "Close protected positions before cancelling their required stops"), id, 0};
      }
    for (const auto id : ids) cancel_order(s, id, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::close_positions(std::optional<std::string> underlying, Timestamp time,
                                              const std::map<std::string, Decision>& rejections, FlattenPricing pricing) {
  if (pricing.limit_ticks < 0 || pricing.limit_ticks > 10 || (!pricing.limit && pricing.limit_ticks != 0))
    throw TradingError(Reason::INVALID_ORDER, "limit_ticks needs a limit flatten and must be 0 to 10");
  return impl_->transact(time, "close_positions", [&](State& s, Events& events) {
    monitor_loss(s, events);
    const auto in_scope = [&](const std::string& name) { return !underlying || name == *underlying; };
    // Before anything is cancelled, what in scope could close: the account, the
    // integration's gate on the underlying and the session a market order (or a
    // share trade) needs can each refuse a close whatever the price. By
    // underlying, whether anything in it can close.
    std::map<std::string, bool> closable;
    Decision refusal;
    const auto gate = [&](const std::string& name, Decision decision) {
      closable[name] |= decision.ok();
      if (!decision.ok() && refusal.ok()) {
        refusal = std::move(decision);
        if (refusal.scope.empty()) refusal.scope = name;
      }
    };
    OrderRequest market;
    market.type = pricing.limit ? OrderType::Limit : OrderType::Market;
    market.tif = pricing.limit ? TimeInForce::Exto : TimeInForce::Day;
    // Every position in scope, expired ones included: they cannot trade and close
    // at settlement, but an expired short keeps the long that covers it until then.
    std::vector<std::string> positions;
    for (const auto& [symbol, position] : s.ledger.positions()) {
      const auto& contract = s.contracts.at(symbol);
      if (position.quantity == 0 || !in_scope(contract.underlying)) continue;
      positions.push_back(symbol);
      if (s.time >= contract.expiry_time()) continue;
      const auto rejection = rejections.find(contract.underlying);
      auto decision = account_check(s, true);
      if (decision.ok() && rejection != rejections.end()) decision = rejection->second;
      if (decision.ok()) decision = pricing.limit && !md::trading_session(contract.root, s.time).open
          ? failure(Reason::SESSION_CLOSED, "Flatten requires an open trading session") : session_check(contract, s.time, market);
      gate(contract.underlying, std::move(decision));
    }
    // Shares close at the underlying's fresh price in the regular session.
    std::vector<std::pair<std::string, Quantity>> stocks;
    CommandResult result;
    for (const auto& [symbol, stock] : s.ledger.stocks()) {
      if (stock.shares == 0 || !in_scope(symbol)) continue;
      const auto rejection = rejections.find(symbol);
      auto decision = account_check(s, true);
      if (decision.ok() && rejection != rejections.end()) decision = rejection->second;
      if (decision.ok()) decision = share_close_check(s, symbol);
      if (decision.ok()) stocks.emplace_back(symbol, stock.shares);
      else result.kept_stocks.emplace(symbol, decision);
      gate(symbol, std::move(decision));
    }
    // Nothing in scope can close: refused, with nothing cancelled or recorded.
    if (!closable.empty() && std::none_of(closable.begin(), closable.end(), [](const auto& c) { return c.second; }))
      return CommandResult{refusal, {}, 0};
    // The open orders in scope are cancelled, but the bracket exits, which keep
    // protecting whatever stays open until it is flat, and the account's own
    // closes. An underlying where nothing can close keeps all its orders.
    for (const auto id : open_ids(s))
      if (const auto& o = s.orders[id - 1]; o.open() && !o.system && o.role == OrderRole::Normal && in_scope(underlying_of(s, o))) {
        const auto it = closable.find(underlying_of(s, o));
        if (it == closable.end() || it->second)
          cancel_order(s, id, failure(Reason::USER_CANCEL, "Cancelled to close positions"), events);
      }
    std::map<std::string, Quantity> held_back;
    const auto plan = plan_closes(s, positions, held_back);
    // A client ID the trader already used stays theirs: take the next free number.
    const auto prefix = "openport-close-" + std::to_string(s.version + 1) + "-";
    std::size_t count = 0;
    const auto client_id = [&] {
      auto id = prefix + std::to_string(++count);
      while (s.clients.contains(id)) id = prefix + std::to_string(++count);
      return id;
    };
    // Each close is split at the order size limit. A refused close is still
    // recorded, rejected with its reason.
    std::map<std::string, Decision> refused;
    const auto limit = s.config.limits.max_order_contracts;
    for (const auto& close : plan) {
      const auto rejection = rejections.find(s.contracts.at(close.legs.front().symbol).underlying);
      for (Quantity placed = 0; placed < close.units;) {
        OrderRequest request;
        request.client_order_id = client_id();
        if (close.legs.size() == 1) {
          request.symbol = close.legs.front().symbol;
          request.side = close.legs.front().side;
        } else {
          request.legs = close.legs;
        }
        request.type = market.type;
        request.tif = market.tif;
        request.quantity = std::min(close.units - placed, limit);
        placed += request.quantity;
        const auto decision = place_close(s, std::move(request), rejection == rejections.end() ? Decision{} : rejection->second, pricing, events);
        if (!decision.ok())
          for (const auto& leg : close.legs) refused.emplace(leg.symbol, decision);
      }
    }
    for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Trade, events);
    // What is left open: the contracts still being worked, and why the rest are not.
    for (const auto& symbol : positions) {
      const auto quantity = held(s, symbol);
      if (quantity == 0) continue;
      Quantity working = 0;
      for (const auto id : open_ids(s)) {
        const auto& o = s.orders[id - 1];
        if (!o.reduce_only) continue;
        if (!multi_leg(o.request)) { if (o.request.symbol == symbol) working += o.remaining(); continue; }
        for (const auto& leg : o.request.legs) if (leg.symbol == symbol) working += o.remaining() * leg.ratio;
      }
      Residual residual{symbol, quantity, std::min(working, magnitude(quantity)), {}};
      if (residual.working < magnitude(quantity)) {
        if (s.time >= s.contracts.at(symbol).expiry_time())
          residual.reason = failure(Reason::AWAITING_SETTLEMENT, "The contract has expired; it closes at its settlement");
        else if (held_back.contains(symbol))
          residual.reason = failure(Reason::AWAITING_SETTLEMENT, "It covers a short that has expired; it stays until that short settles");
        else if (const auto it = refused.find(symbol); it != refused.end())
          residual.reason = it->second;
      }
      result.residuals.push_back(std::move(residual));
    }
    return result;
  });
}
CommandResult TradingSession::on_quotes(const std::vector<QuoteObservation>& quotes,
    const std::vector<Valuation>& valuations, Timestamp time, const std::vector<StockPrice>& stocks,
    const std::vector<Indicator>& indicators) {
  // An empty batch on an idle account only moves the clock: not a transaction,
  // so nothing is journaled. The next transaction advances the clock itself.
  if (quotes.empty() && valuations.empty() && stocks.empty() && indicators.empty() && impl_->idle(time))
    return CommandResult{{}, {}, impl_->state.version};
  return impl_->transact(time, "market", [&](State& s, Events& events) {
    std::set<std::string> seen;
    std::set<std::string> changed;
    std::set<std::string> offered_again;
    for (auto quote : quotes) {
      if (!s.contracts.contains(quote.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Quote references unregistered OSI");
      if (quote.time < 0 || quote.time > time) throw TradingError(Reason::INVALID_TIME, "Quote is future-dated or negative");
      if (quote.quoted < 0 || quote.quoted > quote.time) throw TradingError(Reason::INVALID_TIME, "Quote was first given after its time");
      if (quote.quoted == quote.time) quote.quoted = 0;
      if (!seen.insert(quote.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One observation per contract per batch is required");
      auto& book = s.books[quote.symbol];
      if (quote.observation == book.quote.observation && quote.time >= book.quote.time) {
        // The same quote, confirmed current at a later time: it and its mark stay
        // fresh, and it keeps what is left of its displayed size and when it was
        // first given.
        if (quote.time > book.quote.time) {
          book.quote.quoted = book.quote.first_time();
          book.quote.time = quote.time;
          if (markable_quote(book.quote)) s.marks[quote.symbol] = {mark_of(book.quote), quote.time};
        }
        offered_again.insert(quote.symbol);
        continue;
      }
      if (quote.observation <= book.quote.observation || quote.time < book.quote.time) continue;
      // A quote with only an ask offers it to the exits that may buy there (ask_only).
      book = {quote, valid_quote(quote) ? quote.bid_size : 0, markable_quote(quote) ? quote.ask_size : 0};
      changed.insert(quote.symbol);
      if (markable_quote(quote) && time - quote.time <= s.config.limits.max_quote_age)
        s.marks[quote.symbol] = {mark_of(quote), quote.time};
    }
    seen.clear();
    for (auto valuation : valuations) {
      if (!s.contracts.contains(valuation.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Valuation references unregistered OSI");
      if (valuation.time < 0 || valuation.time > time) throw TradingError(Reason::INVALID_TIME, "Valuation is future-dated or negative");
      if (!seen.insert(valuation.symbol).second) throw TradingError(Reason::MISSING_VALUATION, "One valuation per contract per batch is required");
      const auto prior = s.valuations.find(valuation.symbol);
      if (prior != s.valuations.end() && valuation.time < prior->second.time) continue;
      if (!valid_valuation(valuation)) {
        // Null/nonfinite data is explicitly invalid, never implicitly zero risk.
        valuation = Valuation{valuation.symbol, valuation.time, 0, 0, 0, 0, 0, 0, 0, 0, 0, false};
      }
      s.valuations[valuation.symbol] = valuation;
    }
    seen.clear();
    for (const auto& price : stocks) {
      if (price.symbol.empty() || price.price <= Money{}) throw TradingError(Reason::INVALID_QUOTE, "A stock price needs a symbol and a positive price");
      if (price.time < 0 || price.time > time) throw TradingError(Reason::INVALID_TIME, "Stock price is future-dated or negative");
      if (!seen.insert(price.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One price per underlying per batch is required");
      const auto prior = s.stock_marks.find(price.symbol);
      if (prior != s.stock_marks.end() && price.time < prior->second.time) continue;
      s.stock_marks[price.symbol] = {price.price, price.time};
    }
    seen.clear();
    for (const auto& indicator : indicators) {
      if (indicator.symbol.empty()) throw TradingError(Reason::INVALID_QUOTE, "An indicator needs a symbol");
      if (indicator.time < 0 || indicator.time > time) throw TradingError(Reason::INVALID_TIME, "Indicator is future-dated or negative");
      const auto key = indicator_key(indicator.symbol, indicator.study);
      if (!seen.insert(key).second) throw TradingError(Reason::INVALID_QUOTE, "One value per indicator per batch is required");
      const auto prior = s.indicators.find(key);
      if (prior != s.indicators.end() && indicator.time <= prior->second.time) continue;
      s.indicators[key] = {indicator.value, indicator.time};
    }
    detail::update_reviews(s);
    monitor_calendar(s, events);
    monitor_loss(s, events);
    monitor_rules(s, events);
    // Invalid quotes provide no liquidity. Keep orders until a new valid quote
    // permits the fill-time risk check (or a clock/kill command cancels them). A
    // fresh quote with only an ask still serves the closes that may take it.
    const auto usable = [&](const std::string& symbol) { return quote_check(s, symbol).ok() || asks_only(s, symbol); };
    for (auto it = changed.begin(); it != changed.end();) {
      if (!usable(*it)) it = changed.erase(it); else ++it;
    }
    for (const auto& symbol : s.walked)
      if (usable(symbol)) changed.insert(symbol);
    s.walked.clear();
    // Offered again, a quote's remaining displayed size can fill the orders that
    // a data gap held back when it was new.
    for (const auto id : open_ids(s)) {
      const auto& o = s.orders[id - 1];
      if (o.status == OrderStatus::Armed) continue;
      for (const auto& symbol : order_symbols(o.request))
        if (offered_again.contains(symbol) && usable(symbol)) changed.insert(symbol);
    }
    match_symbols(s, changed, events);
    // Triggers read the batch's books and valuations after resting orders match.
    check_triggers(s, events);
    check_alerts(s, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::set_limits(Limits limits, Timestamp time) {
  validate_limits(limits);
  return impl_->transact(time, "limit_change", [&](State& s, Events& events) {
    auto effective = s.config.rules.evaluation() ? tightened_limits(s.config.limits, limits) : limits;
    // A removed override needs no queue when its tightened cap is the common cap.
    // No driver gate is needed: older runs cannot contain underlying overrides,
    // so removing redundant entries changes none of their journal bytes.
    std::erase_if(effective.underlying_overrides, [&](const auto& entry) {
      return !limits.underlying_overrides.contains(entry.first) &&
          entry.second.dollar_delta == effective.per_underlying.dollar_delta &&
          entry.second.vega == effective.per_underlying.vega;
    });
    s.pending_limits = Json(effective) != Json(limits) ? std::optional(limits) : std::nullopt;
    s.config.limits = effective;
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "limit_change", Json{{"effective", s.config.limits}, {"pending", s.pending_limits}});
    monitor_loss(s, events);
    for (const auto id : open_ids(s)) {
      const auto& order = s.orders[id - 1];
      if (!order.open()) continue;
      // A triggered stop-limit keeps its limit beyond the band, as it may after a gap.
      auto d = order.system || kept_within(order) ? system_check(s, order)
          : order_check(s, order, order.triggered_at > 0 ? Stage::Activate : Stage::Accept);
      if ((persistent(order) || order.reduce_only) &&
          (data_gap(d.code) || d.code == Reason::SESSION_CLOSED || d.code == Reason::LIMIT_ONLY)) continue;
      if (!d.ok()) {
        if (d.code != Reason::SCALING_LIMIT) {
          d.message = std::string(to_string(d.code)) + ": " + d.message;
          d.code = Reason::RISK_CHANGED;
        }
        cancel_order(s, id, d, events);
      }
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::set_guardrails(Guardrails guardrails, Timestamp time) {
  validate_guardrails(guardrails);
  return impl_->transact(time, "guardrail_change", [&](State& s, Events& events) {
    if (guardrails.soft_floor > Money{} && s.config.rules.max_drawdown > Money{} && guardrails.soft_floor <= s.evaluation.floor)
      throw TradingError(Reason::INVALID_LIMITS, "Soft floor equity must be above the plan floor");
    const auto effective = tightened_guardrails(s.config.guardrails, guardrails);
    if (s.guardrails.cooldown_until > s.time && effective.cooldown_minutes > s.config.guardrails.cooldown_minutes) {
      const auto extension = (effective.cooldown_minutes - s.config.guardrails.cooldown_minutes) * md::kNanosPerMinute;
      if (s.guardrails.cooldown_until > std::numeric_limits<Timestamp>::max() - extension)
        throw TradingError(Reason::INVALID_TIME, "Cooldown time exceeds timestamp range");
      s.guardrails.cooldown_until += extension;
    }
    s.pending_guardrails = effective != guardrails ? std::optional(guardrails) : std::nullopt;
    s.config.guardrails = effective;
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "guardrail_change", Json{{"effective", effective}, {"pending", s.pending_guardrails}});
    return CommandResult{};
  });
}
CommandResult TradingSession::trip_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_trip", [&](State& s, Events& events) {
    // A manual trip over a daily-loss or earlier manual latch replaces its reason;
    // its event and the history name the reason it replaced.
    const bool replaces = s.kill && !s.guardrails.owns_kill && s.kill_reason != reason;
    const auto previous = s.kill_reason;
    trip(s, reason, events);
    if (replaces) {
      event(events, "kill_trip", Json{{"reason", reason}, {"previous", previous}});
      record_kill(s, "trip", reason, previous);
    }
    s.kill_reason = reason;
    s.guardrails.owns_kill = false;
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_reset", [&](State& s, Events& events) {
    if (guardrail_reason(s) != Reason::NONE) return CommandResult{reset_check(s, measure(s)), {}, 0};
    if (s.kill) record_kill(s, "reset", reason, s.kill_reason);
    s.kill = false;
    s.kill_reason.clear();
    event(events, "kill_reset", Json{{"reason", reason}});
    monitor_loss(s, events);
    if (s.kill) return CommandResult{reset_check(s, measure(s)), {}, 0};
    return CommandResult{};
  });
}
CommandResult TradingSession::settle(const std::string& symbol, Money settlement, Timestamp time,
                                     std::optional<SettlementSource> source) {
  return impl_->transact(time, "settlement", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Unknown settlement contract"), {}, 0};
    if (s.settled.contains(symbol)) return CommandResult{failure(Reason::ALREADY_SETTLED, "Settlement is exactly once per OSI"), {}, 0};
    if (time < it->second.expiry_time() || settlement < Money{} || !s.ledger.positions().contains(symbol))
      return CommandResult{failure(Reason::INVALID_SETTLEMENT, "Settlement requires expiry, a position and nonnegative reference"), {}, 0};
    const Money strike = Money::from_double(it->second.strike);
    const auto quantity = held(s, symbol);
    // A long instructed not to be exercised expires worthless.
    const bool unexercised = quantity > 0 && s.do_not_exercise.contains(symbol);
    const Money intrinsic = unexercised ? Money{}
        : std::max(Money{}, it->second.type == pricing::OptionType::Call ? settlement - strike : strike - settlement);
    // The last stretch ends at intrinsic value, at expiry with the settlement
    // as the underlying's price and the volatility unchanged.
    std::optional<Valuation> at_expiry;
    if (const auto r = s.references.find(symbol); r != s.references.end() && r->second.valuation) {
      at_expiry = *r->second.valuation;
      at_expiry->spot = settlement.dollars();
      at_expiry->years = 0;
    }
    end_stretch(s, symbol, intrinsic, at_expiry ? &*at_expiry : nullptr);
    const bool delivers = physical(it->second) && intrinsic >= Money::from_micros(10'000);
    const Money fee = delivers ? exercise_fee(s, quantity) : Money{};
    s.settlements.push_back(settlement_record(s.ledger.positions().at(symbol), settlement, intrinsic, fee, time, source));
    s.ledger.settle(symbol, intrinsic, fee);
    if (fee > Money{}) s.explained[symbol].costs -= fee.dollars();
    s.trips.erase(symbol);
    s.settled.insert(symbol);
    s.settling[symbol] = settlement;
    s.closures.push_back({symbol, quantity, intrinsic, time, ClosureKind::Settlement, s.fills.size(), fee});
    if (unexercised) s.do_not_exercise.erase(symbol);
    sync_exits(s, symbol, events);
    Json settled{{"symbol", symbol}, {"reference", settlement}, {"intrinsic", intrinsic}};
    if (unexercised) settled["do_not_exercise"] = true;
    event(events, "settlement", std::move(settled));
    // American equity and ETF options a cent or more in the money are exercised
    // or assigned: settled at intrinsic value, they deliver shares at the
    // settlement price, which together cost the strike.
    if (delivers) {
      const auto underlying = it->second.underlying;
      const auto shares = delivered(it->second, quantity);
      if (const auto mark = s.stock_marks.find(underlying); mark == s.stock_marks.end() || mark->second.time <= time)
        s.stock_marks[underlying] = {settlement, time};
      trade_shares(s, underlying, shares, settlement, StockSource::Delivery, symbol);
      event(events, "delivery", Json{{"symbol", symbol}, {"underlying", underlying}, {"shares", shares}, {"price", settlement}});
    }
    return CommandResult{};
  }, source);
}
namespace {
/// Overnight, short American equity and ETF options that the market valued below
/// their exercise value at the close are assigned: a holder then does better
/// exercising than selling, as with a deep put or a call before its dividend. Each
/// assigned contract is bought back at intrinsic value and delivers shares at the
/// underlying's close, together the strike. Options that expire today settle instead.
/// splitmix64, so a replay draws the same assignments on every platform.
std::uint64_t mix(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
/// How many of `contracts` short contracts holders' exercises reach: each with even
/// odds, as the OCC allocates exercises at random, drawn from the attempt, the
/// contract and the date so a replay assigns the same ones.
Quantity assigned_contracts(const State& s, const std::string& symbol, Quantity contracts) {
  auto seed = mix(static_cast<std::uint64_t>(s.evaluation.started));
  for (const unsigned char c : symbol + " " + md::format_date(s.day)) seed = mix(seed ^ c);
  Quantity assigned = 0;
  for (Quantity i = 0; i < contracts; ++i) assigned += static_cast<Quantity>((seed = mix(seed)) >> 63);
  return assigned;
}
/// `closing` is when the finished day's marks were last published: the marks and
/// the underlying's price decide as they stood then.
void assign_early(State& s, const std::vector<Dividend>& dividends, Timestamp closing, Events& events) {
  std::vector<std::string> shorts;
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& c = s.contracts.at(symbol);
    if (position.quantity < 0 && physical(c) && c.expiry > s.day) shorts.push_back(symbol);
  }
  for (const auto& symbol : shorts) {
    const auto contract = s.contracts.at(symbol);
    const auto mark = s.marks.find(symbol);
    const auto price = stock_price(s, contract.underlying, closing);
    if (mark == s.marks.end() || !price) continue;
    const Money strike = Money::from_double(contract.strike);
    const bool call = contract.type == pricing::OptionType::Call;
    const Money intrinsic = std::max(Money{}, call ? *price - strike : strike - *price);
    if (intrinsic < Money::from_micros(10'000)) continue;
    // Holders exercise an option trading below its exercise value, and a call
    // whose time value is less than a dividend going ex on the new day.
    Money dividend;
    for (const auto& d : dividends)
      if (call && d.symbol == contract.underlying) dividend = dividend + d.per_share;
    const char* reason = mark->second.price < intrinsic ? "below_intrinsic"
                       : mark->second.price - intrinsic < dividend ? "dividend" : nullptr;
    if (!reason) continue;
    const auto short_contracts = -held(s, symbol);
    const auto contracts = assigned_contracts(s, symbol, short_contracts);
    if (contracts == 0) continue;
    const auto shares = delivered(contract, -contracts);
    const auto fee = exercise_fee(s, contracts);
    fill_position(s, symbol, contracts, intrinsic, fee);
    s.closures.push_back({symbol, -contracts, intrinsic, s.time, ClosureKind::Assignment, s.fills.size(), fee});
    trade_shares(s, contract.underlying, shares, *price, StockSource::Assignment, symbol);
    sync_exits(s, symbol, events);
    event(events, "assignment", Json{{"symbol", symbol}, {"contracts", contracts}, {"of", short_contracts}, {"reason", reason},
                                     {"intrinsic", intrinsic}, {"mark", mark->second.price},
                                     {"underlying", contract.underlying}, {"shares", shares}, {"price", *price}});
  }
}
/// A payment in whole cents, as brokers pay one: nearest cent, ties away from zero.
Money nearest_cent(Money amount) {
  constexpr std::int64_t cent = 10'000;
  const auto micros = amount.micros();
  const auto rest = micros % cent;
  auto rounded = micros - rest;
  if (2 * (rest < 0 ? -rest : rest) >= cent) rounded += micros < 0 ? -cent : cent;
  return Money::from_micros(rounded);
}
/// On an ex-date the shares held into it are paid the dividend, and short shares
/// pay it, in whole cents: once per symbol and date, into the new day's P&L as other.
void pay_dividends(State& s, const std::vector<Dividend>& dividends, Events& events) {
  for (const auto& d : dividends) {
    const auto shares = shares_held(s, d.symbol);
    const bool paid = std::any_of(s.dividends.begin(), s.dividends.end(),
        [&](const DividendPayment& p) { return p.symbol == d.symbol && p.ex_date == d.ex_date; });
    if (shares == 0 || paid || d.per_share <= Money{}) continue;
    const Money amount = nearest_cent(d.per_share * shares);
    s.ledger.receive_dividend(d.symbol, amount);
    Attribution part;
    part.other = amount.dollars();
    explain_part(s, d.symbol, part);
    s.dividends.push_back({d.symbol, d.ex_date, d.per_share, shares, amount, s.time, s.stock_fills.size()});
    event(events, "dividend", Json{{"symbol", d.symbol}, {"ex_date", d.ex_date}, {"per_share", d.per_share},
                                   {"shares", shares}, {"amount", amount}});
  }
}
}  // namespace
namespace {
std::string close_key(const std::string& underlying, md::Date date) { return underlying + " " + md::format_date(date); }
}  // namespace
CommandResult TradingSession::record_close(const std::string& underlying, md::Date date, Money price, Timestamp print_time,
                                           Timestamp time) {
  if (underlying.empty() || price <= Money{} || !md::valid_date(date))
    throw TradingError(Reason::INVALID_SETTLEMENT, "A closing print needs an underlying, a valid date and a positive price");
  return impl_->transact(time, "closing_print", [&](State& s, Events& events) {
    auto& print = s.closing_prints[close_key(underlying, date)];
    if (print.price != price) {
      print = ClosingPrint{price, print_time};
      event(events, "closing_print", Json{{"underlying", underlying}, {"date", date}, {"price", price}, {"time", print_time}});
    }
    return CommandResult{};
  });
}
std::optional<ClosingPrint> TradingSession::closing_print(const std::string& underlying, md::Date date) const {
  const auto& prints = impl_->state.closing_prints;
  const auto it = prints.find(close_key(underlying, date));
  return it == prints.end() ? std::nullopt : std::optional(it->second);
}
CommandResult TradingSession::roll_day(Timestamp time, const std::vector<Dividend>& dividends) {
  // The finished day closes on the marks it last published, before a new day's
  // quotes replace them: its close, P&L by Greek and the night's assignments come
  // from them, and the move from them to the new day's first quotes is the new day's.
  const auto closing = impl_->snapshot;
  const auto closing_time = impl_->state.time;
  return impl_->transact(time, "day_rollover", [&](State& s, Events& events) {
    const auto day = plan_trading_date(s.config.rules, time);
    if (day <= s.day) return CommandResult{failure(Reason::INVALID_TIME, "Rollover requires a later trading date"), {}, 0};
    // advance already checked the actual date change. Do not classify positions
    // opened on the new date before this bookkeeping rollover as overnight holds.
    check_rollover_positions(s, events, false);
    monitor_loss(s, events);
    const auto& snapshot = *closing;
    if (!snapshot.valuation_complete) return CommandResult{failure(Reason::STALE_QUOTE, "Rollover requires complete marked equity"), {}, 0};
    // Close the finished day. An end-of-day floor ratchets only here, from the
    // last fully marked equity observed on that date; breaches are still checked
    // on every transaction by monitor_rules.
    auto& e = s.evaluation;
    const auto& rules = s.config.rules;
    evaluation_rollover(e, rules);
    if (!rules.scaling.empty()) {
      const auto profit = plan_inputs(snapshot).balance - e.starting_balance;
      const auto next = scaling_limit(rules, profit);
      if (next != e.scaling_limit) {
        event(events, "scaling_limit", Json{{"old", e.scaling_limit}, {"limit", next}, {"profit", profit}});
        e.scaling_limit = next;
      }
    }
    Money capital;
    // A placeholder date from before the attempt started is not a trading day.
    // On a funded account, a day with enough net realised profit counts once,
    // toward the payout cycle in progress when it closes.
    if (e.started > 0 && e.day >= plan_trading_date(rules, e.started)) {
      const auto realised = net_realised(s) - e.day_open_realised;
      const auto& payouts = rules.payouts;
      const bool qualifying = rules.phase == Phase::Funded && e.status == EvaluationStatus::Active &&
          payouts.qualifying_days > 0 && realised >= payouts.qualifying_profit && realised > Money{};
      if (qualifying) ++e.qualifying_days;
      e.days.push_back({e.day, e.day_open_equity, e.day_close_equity, e.peak, e.floor, realised, qualifying, snapshot.attribution,
                        e.day_low_equity, e.day_high_equity, e.day_low_at, e.day_high_at, e.day_executions, e.day_lock});
      event(events, "evaluation_day", e.days.back());
      capital = review_size_scaling(s, day, plan_inputs(snapshot).balance, events);
    }
    // A plan limit's lock and the day's executions end with the day.
    e.day_lock = Reason::NONE;
    e.day_locked_at = 0;
    e.day_executions = 0;
    if (s.pending_limits || s.pending_guardrails) {
      if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
      if (s.pending_limits) { s.config.limits = *s.pending_limits; s.pending_limits.reset(); }
      if (s.pending_guardrails) { s.config.guardrails = *s.pending_guardrails; s.pending_guardrails.reset(); }
      ++s.limits_revision;
      s.pending_applied_at = s.time;
      event(events, "pending_limits_applied", Json{{"limits", s.config.limits}, {"guardrails", s.config.guardrails}});
    }
    const bool personal_latch = s.guardrails.owns_kill;
    const auto cooldown_until = s.guardrails.cooldown_until;
    s.guardrails = {};
    s.guardrails.owns_kill = personal_latch;
    if (cooldown_until > s.time) s.guardrails.cooldown_until = cooldown_until;
    refresh_guardrail_latch(s, events);
    const auto opening_equity = snapshot.equity + capital;
    e.day_low_equity = opening_equity; e.day_high_equity = opening_equity;
    e.day_low_at = s.time; e.day_high_at = s.time;
    e.day_open_realised = net_realised(s);
    e.day = day;
    e.day_open_equity = opening_equity;
    e.day_close_equity = opening_equity;
    s.start_equity = opening_equity;
    s.day = day;
    // Each round trip keeps its stretches to the close; the new day's P&L by
    // Greek runs from these marks.
    for (const auto& [symbol, reference] : s.references) {
      const auto trip = s.trips.find(symbol);
      if (trip == s.trips.end()) continue;
      if (s.contracts.contains(symbol)) {
        if (const auto mark = s.marks.find(symbol); mark != s.marks.end())
          s.trip_attribution[trip->second] += explain(s.contracts.at(symbol), reference, mark->second.price, valuation_of(s, symbol));
      } else if (const auto mark = s.stock_marks.find(symbol); mark != s.stock_marks.end()) {
        s.trip_attribution[trip->second] += explain_stock(reference, mark->second.price);
      }
    }
    s.explained.clear();
    s.references.clear();
    for (const auto& [symbol, position] : s.ledger.positions()) start_stretch(s, symbol);
    for (const auto& [symbol, stock] : s.ledger.stocks()) start_stock_stretch(s, symbol);
    event(events, "day_rollover", Json{{"day", day}, {"equity", s.start_equity}});
    // Assignments arrive overnight, so the new day takes them, and then the
    // ex-date's dividends pay the shares held into it.
    assign_early(s, dividends, closing_time, events);
    pay_dividends(s, dividends, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::request_payout(Money amount, Timestamp time) {
  if (amount <= Money{} || amount.micros() % 10'000 != 0)
    throw TradingError(Reason::INVALID_PAYOUT, "Payout amount must be a positive whole number of cents");
  return impl_->transact(time, "payout", [&](State& s, Events& events) {
    const auto& rules = s.config.rules;
    auto& e = s.evaluation;
    const auto snapshot = snapshot_of(s);
    const auto quote = payout_quote(snapshot, rules);
    auto reject = [&](std::string message, Money limit) {
      return CommandResult{{Reason::INVALID_PAYOUT, std::move(message), amount.dollars(), limit.dollars(), "aggregate"}, {}, 0};
    };
    if (!quote.blocked.ok()) return CommandResult{quote.blocked, {}, 0};
    if (amount < quote.minimum) return reject("Below the minimum payout", quote.minimum);
    if (amount > quote.maximum) return reject("Above the maximum for this payout", quote.maximum);
    const Money equity = snapshot.equity;  // Flat, so this is cash.
    s.ledger.withdraw(amount);
    // The trading day in progress takes the withdrawal, even after its date
    // ends and before rollover, so its P&L and an end-of-day ratchet ignore it.
    s.start_equity = s.start_equity - amount;
    e.day_open_equity = e.day_open_equity - amount;
    e.day_close_equity = e.day_close_equity - amount;
    // The day's extremes too, so a daily loss limit trailing the day's high
    // does not count the withdrawal as a loss.
    if (e.day_low_equity) e.day_low_equity = *e.day_low_equity - amount;
    if (e.day_high_equity) e.day_high_equity = *e.day_high_equity - amount;
    if (!e.floor_locked) {
      e.peak = e.peak - amount;
      const auto floor = evaluation_floor(rules, e.peak, e.floor_locked, e.starting_balance);
      e.floor = e.size_scaling ? std::max(e.floor - amount, floor) : floor;
    }
    e.payouts.push_back({quote.number, s.time, e.day, amount, amount.prorate(rules.payouts.split_percent, 100), equity});
    s.payouts_received = s.payouts_received + e.payouts.back().trader_share;
    e.qualifying_days = 0;
    e.cycle_started = s.time;
    event(events, "payout", e.payouts.back());
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_account(Money initial_cash, AccountRules rules, std::string reason, Timestamp time, bool activated, bool archive_payouts, bool restore_scaled_rules) {
  require_reason(reason);
  const auto& current = impl_->state;
  if (restore_scaled_rules && initial_cash == current.config.initial_cash &&
      same_program_rules(rules, current.config.rules))
    rules = original_program_rules(std::move(rules), current.evaluation.starting_balance, current.evaluation.size_scaling);
  normalize_event_rules(rules, true);
  validate_rules(rules);
  if (initial_cash <= Money{}) throw TradingError(Reason::INVALID_MONEY, "Starting balance must be positive");
  if (rules.size_scaling && rules.size_scaling->max_balance < initial_cash)
    throw TradingError(Reason::INVALID_RULES, "Account size scaling maximum must be at least the starting balance");
  return impl_->transact(time, "account_reset", [&](State& s, Events& events) {
    const auto snapshot = snapshot_of(s);
    const bool same = initial_cash == s.config.initial_cash && same_program_rules(rules,
        original_program_rules(s.config.rules, s.evaluation.starting_balance, s.evaluation.size_scaling));
    const auto costs = program_costs(snapshot, s.config.rules);
    if (same && rules.max_resets > 0 && costs.resets_used >= rules.max_resets)
      return CommandResult{{Reason::RESET_LIMIT, "This plan has no resets left; choose a different plan to start a new purchase",
                            static_cast<double>(costs.resets_used + 1), static_cast<double>(rules.max_resets), {}}, {}, 0};
    if (activated && (same || s.evaluation.status != EvaluationStatus::Passed))
      return CommandResult{failure(Reason::PLAN_LOCKED, "Activation requires a pass into a different plan"), {}, 0};
    const auto fee = same ? rules.reset_fee : activated ? rules.activation_fee : rules.evaluation_fee;
    // Check aggregate overflow before changing anything, even though the fees never touch paper cash.
    static_cast<void>(costs.total() + fee);
    for (const auto id : open_ids(s)) cancel_order(s, id, failure(Reason::ACCOUNT_RESET, reason), events);
    for (const auto& p : snapshot.positions) {
      const auto& position = p.position;
      s.closures.push_back({position.contract.osi_symbol(), position.quantity,
                            p.mark ? *p.mark : average_unit_price(position), s.time, ClosureKind::Reset, s.fills.size()});
    }
    // Shares leave with the old ledger too, at their mark or else their cost.
    for (const auto& held : snapshot.stocks) {
      const auto& p = held.position;
      const auto magnitude = p.shares < 0 ? -p.shares : p.shares;
      const auto price = held.mark ? *held.mark : (p.shares < 0 ? -p.basis : p.basis).prorate(1, magnitude);
      s.stock_fills.push_back({s.stock_fills.size() + 1, p.symbol, -p.shares, price, s.time, StockSource::Reset, {}});
    }
    const auto& e = s.evaluation;
    s.attempts.push_back({e.attempt, s.config.rules.plan, e.started, s.time, e.starting_balance, snapshot.equity,
                          e.status, e.decision, e.first_order, e.first_fill, e.decision_code, s.config.rules, e.decided_at,
                          e.status == EvaluationStatus::Active ? std::nullopt : std::optional(e.decided_equity), e.peak,
                          s.config.rules.max_drawdown > Money{} ? std::optional(e.floor) : std::nullopt, s.fee_charged, {},
                          archive_payouts ? e.size_scaling : std::nullopt});
    if (archive_payouts)
      for (const auto& payout : e.payouts) s.attempts.back().payouts_received = s.attempts.back().payouts_received + payout.trader_share;
    s.fee_charged = {fee, fee == Money{} ? "" : same ? "reset" : activated ? "activation" : "evaluation"};
    const auto attempt = e.attempt + 1;
    s.config.initial_cash = initial_cash;
    s.config.rules = std::move(rules);
    s.ledger = Ledger(initial_cash);
    s.explained.clear();
    s.references.clear();
    s.do_not_exercise = {};
    s.trips.clear();
    s.start_equity = initial_cash;
    // The new ledger clears only the equity-based latch. Daily discipline survives the attempt.
    std::erase(s.guardrails.latched, Reason::SOFT_FLOOR);
    if (!s.guardrails.owns_kill || guardrail_reason(s) == Reason::NONE) {
      if (s.kill) record_kill(s, "reset", reason, s.kill_reason);
      s.kill = false;
      s.kill_reason.clear();
      s.guardrails.owns_kill = false;
    }
    // The new attempt starts with any pending settings, and says when they applied.
    const bool pending = s.pending_limits || s.pending_guardrails;
    if (s.pending_limits) { s.config.limits = *s.pending_limits; s.pending_limits.reset(); ++s.limits_revision; }
    if (s.pending_guardrails) { s.config.guardrails = *s.pending_guardrails; s.pending_guardrails.reset(); ++s.limits_revision; }
    s.guardrails.trade_limit_order = 0;
    refresh_guardrail_latch(s, events);
    s.pending_applied_at = pending ? s.time : 0;
    s.day = plan_trading_date(s.config.rules, s.time);
    s.evaluation = fresh_evaluation(s, attempt);
    Json reset{{"reason", reason}, {"attempt", attempt}, {"initial_cash", initial_cash}, {"rules", s.config.rules}};
    if (s.fee_charged.amount != Money{}) reset["fee_charged"] = s.fee_charged;
    event(events, "account_reset", reset);
    return CommandResult{};
  });
}
std::shared_ptr<const TradingSnapshot> TradingSession::snapshot() const { return impl_->snapshot; }
std::string TradingSession::snapshot_json() const { return Json(*impl_->snapshot).dump(); }
const SessionConfig& TradingSession::config() const { return impl_->state.config; }
const Contracts& TradingSession::contracts() const { return impl_->state.contracts; }
const Valuations& TradingSession::valuations() const { return impl_->state.valuations; }
OpeningOrder TradingSession::opening_order(const OrderRequest& request, Timestamp time, const PreviewMarket& market,
                                            const std::map<std::string, Quantity>& preceding) const {
  return opening_exposure(prepared(impl_->state, time, market), request, preceding);
}
std::optional<double> TradingSession::held_dollar_delta(const std::string& underlying, Timestamp time) const {
  auto state = impl_->state;
  state.time = time;
  return held_direction(state, underlying);
}

std::vector<CounterExposure> TradingSession::counter_exposures(const std::string& account, Timestamp time,
    const std::vector<Valuation>& valuations, const std::vector<StockPrice>& stocks) const {
  // Overlay current marks, then age orders on a private copy. Expired or
  // clock-cancelled entries do not reserve direction; holdings never execute.
  auto state = impl_->state;
  for (const auto& v : valuations)
    if (v.time > 0 && v.time <= time && state.contracts.contains(v.symbol)) state.valuations[v.symbol] = v;
  for (const auto& p : stocks)
    if (p.time > 0 && p.time <= time && p.price > Money{}) state.stock_marks[p.symbol] = {p.price, p.time};
  Events ignored;
  advance(state, time, ignored);
  std::set<std::string> underlyings;
  for (const auto& [symbol, p] : state.ledger.positions()) underlyings.insert(p.contract.underlying);
  for (const auto& [symbol, p] : state.ledger.stocks()) underlyings.insert(symbol);
  std::vector<CounterExposure> result;
  for (const auto& underlying : underlyings) result.push_back({account, underlying, held_direction(state, underlying)});
  for (const auto id : open_ids(state)) {
    const auto& order = state.orders[id - 1];
    if (order.system || kept_within(order)) continue;
    auto request = order.request;
    request.quantity = order.remaining();
    const auto exposure = opening_exposure(state, request);
    if (!exposure.opening) continue;
    if (exposure.decision.ok()) {
      for (const auto& [underlying, delta] : exposure.dollar_delta) result.push_back({account, underlying, delta});
    } else {
      std::set<std::string> missing;
      for (const auto& symbol : order_symbols(request))
        if (const auto c = state.contracts.find(symbol); c != state.contracts.end()) missing.insert(c->second.underlying);
      for (const auto& underlying : missing) result.push_back({account, underlying, std::nullopt});
    }
  }
  return result;
}

std::optional<QuoteObservation> TradingSession::quote(const std::string& symbol) const {
  const auto it = impl_->state.books.find(symbol);
  return it == impl_->state.books.end() ? std::nullopt : std::optional(it->second.quote);
}
std::map<std::string, SizeLeft> TradingSession::sizes_left() const {
  std::map<std::string, SizeLeft> out;
  for (const auto& [symbol, book] : impl_->state.books) {
    const auto& q = book.quote;
    // The budgets an observation starts with: an ask-only quote offers its ask to exits.
    const auto bid = valid_quote(q) ? q.bid_size : 0;
    const auto ask = markable_quote(q) ? q.ask_size : 0;
    if (book.bid_left < bid || book.ask_left < ask) out[symbol] = {q.observation, q.bid_size, q.ask_size, book.bid_left, book.ask_left};
  }
  return out;
}
md::Date TradingSession::trading_day() const { return impl_->state.day; }
md::Date TradingSession::trading_date(Timestamp time) const { return plan_trading_date(impl_->state.config.rules, time); }
namespace {
/// Records (or, empty, clears) a trade's note and tags under `key`.
void store_annotation(State& s, const std::string& key, Annotation annotation, Events& events) {
  event(events, "trade_annotated", Json{{"trade", key}, {"note", annotation.note}, {"tags", annotation.tags}});
  if (annotation.note.empty() && annotation.tags.empty()) {
    s.annotations.erase(key);
  } else {
    annotation.time = s.time;
    s.annotations[key] = std::move(annotation);
  }
}
}  // namespace
CommandResult TradingSession::annotate(std::uint64_t trade, std::string note, std::vector<std::string> tags, Timestamp time) {
  auto annotation = clean_annotation(std::move(note), tags);
  return impl_->transact(time, "annotate", [&](State& s, Events& events) {
    const auto trades = lifecycles(s.fills, s.closures, s.contracts);
    if (std::none_of(trades.begin(), trades.end(), [&](const Lifecycle& t) { return t.first_fill == trade; }))
      return CommandResult{failure(Reason::UNKNOWN_TRADE, "No trade opens with fill " + std::to_string(trade)), {}, 0};
    store_annotation(s, std::to_string(trade), std::move(annotation), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::group_trades(std::vector<std::uint64_t> trades, bool together, Timestamp time) {
  return impl_->transact(time, together ? "group" : "ungroup", [&](State& s, Events& events) {
    const auto decision = detail::regroup(s, trades, together);
    if (!decision.ok()) return CommandResult{decision, {}, 0};
    Json ids = Json::array();
    for (const auto id : trades) ids.push_back(std::to_string(id));
    event(events, together ? "trades_grouped" : "trades_ungrouped", Json{{"trades", ids}});
    return CommandResult{};
  });
}
CommandResult TradingSession::create_alert(AlertSpec spec, Timestamp time) {
  validate_alert(spec);
  return impl_->transact(time, "alert", [&](State& s, Events& events) {
    const auto& c = spec.condition;
    std::vector<std::string> symbols;
    if (c.scope == AlertScope::Contract) symbols.push_back(c.symbol);
    for (const auto& leg : c.legs) symbols.push_back(leg.symbol);
    for (const auto& symbol : symbols)
      if (!s.contracts.contains(symbol))
        return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "An alert's contracts must be registered canonical OSI definitions"), {}, 0};
    if (s.alerts.items.size() >= kMaxAlerts)
      return CommandResult{failure(Reason::INVALID_ALERT, "An account keeps at most 100 alerts; delete one first"), {}, 0};
    if (s.alerts.created == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Alert IDs exhausted");
    Alert alert;
    alert.id = ++s.alerts.created;
    alert.spec = std::move(spec);
    alert.created = s.time;
    alert.actor = s.actor;
    s.alerts.items.push_back(alert);
    event(events, "alert_created", Json{{"alert", alert}});
    check_alerts(s, events);
    CommandResult result;
    result.alert_id = alert.id;
    return result;
  });
}
CommandResult TradingSession::delete_alert(std::uint64_t id, Timestamp time) {
  return impl_->transact(time, "alert", [&](State& s, Events& events) {
    auto& items = s.alerts.items;
    const auto it = std::find_if(items.begin(), items.end(), [&](const Alert& a) { return a.id == id; });
    if (it == items.end()) return CommandResult{failure(Reason::UNKNOWN_ALERT, "No alert " + std::to_string(id)), {}, 0};
    event(events, "alert_deleted", Json{{"alert", *it}});
    items.erase(it);
    return CommandResult{};
  });
}
CommandResult TradingSession::annotate_day(md::Date day, std::string plan, std::string review, Timestamp time) {
  if (!md::valid_date(day))
    throw TradingError(Reason::INVALID_NOTE, "A day note needs a valid YYYY-MM-DD date");
  DayNote note{clean_annotation(std::move(plan), {}).note, clean_annotation(std::move(review), {}).note, time};
  return impl_->transact(time, "day_note", [&](State& s, Events& events) {
    const auto key = md::format_date(day);
    if (note.plan.empty() && note.review.empty()) s.day_notes.erase(key);
    else s.day_notes[key] = note;
    event(events, "day_annotated", Json{{"day", key}, {"note", note}});
    return CommandResult{};
  });
}
CommandResult TradingSession::annotate_shares(std::uint64_t first_fill, std::string note, std::vector<std::string> tags, Timestamp time) {
  auto annotation = clean_annotation(std::move(note), tags);
  return impl_->transact(time, "annotate", [&](State& s, Events& events) {
    const auto trips = share_lifecycles(s.stock_fills, s.dividends);
    if (std::none_of(trips.begin(), trips.end(), [&](const ShareLifecycle& t) { return t.fills.front() == first_fill; }))
      return CommandResult{failure(Reason::UNKNOWN_TRADE, "No share trade opens with stock fill " + std::to_string(first_fill)), {}, 0};
    store_annotation(s, "s" + std::to_string(first_fill), std::move(annotation), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::exercise(const std::string& symbol, Quantity contracts, Timestamp time) {
  if (contracts <= 0) throw TradingError(Reason::INVALID_ORDER, "Exercise a positive number of contracts");
  return impl_->transact(time, "exercise", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Exercise references an unregistered OSI"), {}, 0};
    const auto contract = it->second;
    if (!physical(contract))
      return CommandResult{failure(Reason::INVALID_ORDER, "Only American equity and ETF options deliver shares; index options settle in cash"), {}, 0};
    if (s.time >= contract.expiry_time()) return CommandResult{failure(Reason::EXPIRED, "The contract has expired; settlement exercises it"), {}, 0};
    if (const auto d = account_check(s); !d.ok()) return CommandResult{d, {}, 0};
    if (held(s, symbol) < contracts) return CommandResult{failure(Reason::INVALID_ORDER, "Exercise needs that many long contracts"), {}, 0};
    // Stock trades execute synchronously: there are no working share sells to reserve.
    if (s.config.rules.buy_only && contract.type == pricing::OptionType::Put &&
        -delivered(contract, contracts) > std::max<Quantity>(0, shares_held(s, contract.underlying)))
      return CommandResult{failure(Reason::BUY_ONLY, "This plan is buy-only: put exercise would sell shares short; hold enough long shares first"), {}, 0};
    if (const auto d = defined_risk_check(s, {{symbol, -contracts}},
                                          "Exercising this long would leave a short option uncovered; close the short first");
        !d.ok())
      return CommandResult{d, {}, 0};
    if (const auto d = account_type_check(s, {{symbol, -contracts}}, {{contract.underlying, delivered(contract, contracts)}}); !d.ok())
      return CommandResult{d, {}, 0};
    const auto price = stock_price(s, contract.underlying);
    if (!price) return CommandResult{failure(Reason::STALE_QUOTE, "Exercise needs a fresh price for " + contract.underlying), {}, 0};
    const Money strike = Money::from_double(contract.strike);
    const Money intrinsic = std::max(Money{}, contract.type == pricing::OptionType::Call ? *price - strike : strike - *price);
    if (intrinsic <= Money{}) return CommandResult{failure(Reason::INVALID_ORDER, "Exercise needs the option in the money"), {}, 0};
    const auto shares = delivered(contract, contracts);
    // The contracts close at intrinsic value, so any time value left is a cost,
    // and the shares change hands at the underlying's price.
    const auto apply = [&](State& t) {
      const auto fee = exercise_fee(t, contracts);
      fill_position(t, symbol, -contracts, intrinsic, fee);
      t.closures.push_back({symbol, contracts, intrinsic, t.time, ClosureKind::Exercise, t.fills.size(), fee});
      trade_shares(t, contract.underlying, shares, *price, StockSource::Exercise, symbol);
    };
    if (s.config.rules.buying_power) {
      State projected = s;
      apply(projected);
      const auto power = buying_power(projected).total;
      if (free_power(projected) < free_power(s) && power.available < Money{})
        return CommandResult{{Reason::BUYING_POWER, "Exercise needs more buying power than the account has available",
                              (-power.available).dollars(), 0.0, contract.underlying}, {}, 0};
    }
    apply(s);
    sync_exits(s, symbol, events);
    event(events, "exercise", Json{{"symbol", symbol}, {"contracts", contracts}, {"intrinsic", intrinsic},
                                   {"underlying", contract.underlying}, {"shares", shares}, {"price", *price}});
    return CommandResult{};
  });
}
CommandResult TradingSession::abandon(const std::string& symbol, Timestamp time) {
  return impl_->transact(time, "abandon", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Abandon references an unregistered OSI"), {}, 0};
    const auto contracts = held(s, symbol);
    if (contracts <= 0) return CommandResult{failure(Reason::INVALID_ORDER, "Abandon needs a long position; buy a short back instead"), {}, 0};
    if (const auto d = account_check(s, true); !d.ok()) return CommandResult{d, {}, 0};
    // Only what cannot be sold: nobody bids for it, or it has expired.
    if (s.time < it->second.expiry_time() && !asks_only(s, symbol)) {
      auto d = quote_check(s, symbol);
      if (d.ok()) d = failure(Reason::INVALID_ORDER, "Someone bids for it: sell it instead");
      return CommandResult{d, {}, 0};
    }
    if (const auto d = defined_risk_check(s, {{symbol, -contracts}},
                                          "Abandoning this long would leave a short option uncovered; close the short first");
        !d.ok())
      return CommandResult{d, {}, 0};
    // These orders disappear with the long. They must not reserve its cover
    // during the account-type check either (including bracket siblings).
    const auto cancel_sales = [&](State& target, Events& changes) {
      for (const auto id : open_ids(target)) {
        const auto& o = target.orders[id - 1];
        const bool sells = std::any_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) {
          return leg.symbol == symbol && leg.side == Side::Sell;
        }) || (o.request.symbol == symbol && o.request.side == Side::Sell);
        if (sells) cancel_order(target, id, failure(Reason::POSITION_CLOSED, "The position this order sold was abandoned"), changes);
      }
    };
    if (s.config.rules.account_type != AccountType::Margin) {
      State checked = s;
      Events ignored;
      cancel_sales(checked, ignored);
      if (const auto d = account_type_check(checked, {{symbol, -contracts}}); !d.ok()) return CommandResult{d, {}, 0};
    }
    // The contracts leave at zero, without a fee: the whole basis is the loss.
    const auto apply = [&](State& t) {
      fill_position(t, symbol, -contracts, Money{}, Money{});
      t.closures.push_back({symbol, contracts, Money{}, t.time, ClosureKind::Abandon, t.fills.size()});
    };
    if (s.config.rules.buying_power) {
      State projected = s;
      apply(projected);
      Events ignored;
      cancel_sales(projected, ignored);
      const auto power = buying_power(projected).total;
      if (free_power(projected) < free_power(s) && power.available < Money{})
        return CommandResult{{Reason::BUYING_POWER, "Abandoning this long leaves the short it covers needing more buying power than the account has",
                              (-power.available).dollars(), 0.0, it->second.underlying}, {}, 0};
    }
    apply(s);
    sync_exits(s, symbol, events);
    cancel_sales(s, events);
    event(events, "abandon", Json{{"symbol", symbol}, {"contracts", contracts}});
    return CommandResult{};
  });
}
CommandResult TradingSession::instruct_exercise(const std::string& symbol, bool do_not_exercise, Timestamp time) {
  return impl_->transact(time, "exercise_instruction", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "The instruction references an unregistered OSI"), {}, 0};
    if (held(s, symbol) <= 0)
      return CommandResult{failure(Reason::INVALID_ORDER, "Only a long can be left unexercised; a short is assigned at its holder's choice"), {}, 0};
    if (const auto d = account_check(s, true); !d.ok()) return CommandResult{d, {}, 0};
    if (do_not_exercise) s.do_not_exercise.insert(symbol);
    else s.do_not_exercise.erase(symbol);
    event(events, "exercise_instruction", Json{{"symbol", symbol}, {"do_not_exercise", do_not_exercise}});
    return CommandResult{};
  });
}
CommandResult TradingSession::trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time,
                                          std::optional<StockPrice> feed_price, bool reduce_only) {
  if (signed_shares == 0 || (!reduce_only && (signed_shares < -10'000'000 || signed_shares > 10'000'000)))
    throw TradingError(Reason::INVALID_ORDER, "Trade a nonzero number of shares, at most 10,000,000");
  if (!reduce_only && (symbol.empty() || symbol.size() > 16 ||
      !std::all_of(symbol.begin(), symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; })))
    throw TradingError(Reason::INVALID_ORDER, "Use an uppercase stock or ETF symbol");
  if (feed_price && (feed_price->symbol != symbol || feed_price->price <= Money{} || feed_price->time < 0 || feed_price->time > time))
    throw TradingError(Reason::INVALID_QUOTE, "A stock price needs the traded symbol, a positive price and no later time");
  return impl_->transact(time, "stock_trade", [&](State& s, Events& events) {
    if (const auto d = account_type_check(s, {}, {{symbol, signed_shares}}); !d.ok()) return CommandResult{d, {}, 0};
    const auto shares = shares_held(s, symbol);
    if (s.config.rules.min_hold_seconds > 0 && shares != 0 && (shares > 0) != (signed_shares > 0)) {
      for (const auto& life : share_lifecycles(s.stock_fills, s.dividends))
        if (life.symbol == symbol && !life.closed)
          if (const auto d = hold_age_check(s, life.opened, symbol); !d.ok()) return CommandResult{d, {}, 0};
    }
    const bool reduces = shares != 0 && (shares > 0) != (signed_shares > 0) &&
                         (shares > 0 ? -signed_shares <= shares : signed_shares <= -shares);
    if (reduce_only && !reduces)
      return CommandResult{failure(Reason::INVALID_ORDER, "Stock trades only reduce the shares exercise and assignment delivered"), {}, 0};
    if (const auto d = account_check(s, reduces); !d.ok()) return CommandResult{d, {}, 0};
    if (!reduces)
      if (const auto d = plan_entry_check(s.config.rules, symbol, s.time); !d.ok()) return CommandResult{d, {}, 0};
    if (!reduces) {
      if (md::is_index_underlying(symbol))
        return CommandResult{failure(Reason::INVALID_ORDER, "Index underlyings settle in cash and have no shares to trade"), {}, 0};
      if (signed_shares < 0 && s.config.rules.buy_only)
        return CommandResult{failure(Reason::BUY_ONLY, "This plan is buy-only: share sales may only reduce long shares"), {}, 0};
      if (signed_shares < 0 && s.config.rules.defined_risk)
        return CommandResult{failure(Reason::DEFINED_RISK, "This plan allows defined risk only: short shares can lose without limit"), {}, 0};
    }
    if (feed_price) {
      const auto prior = s.stock_marks.find(symbol);
      if (prior == s.stock_marks.end() || prior->second.time <= feed_price->time)
        s.stock_marks[symbol] = {feed_price->price, feed_price->time};
    }
    if (const auto d = share_close_check(s, symbol); !d.ok()) return CommandResult{d, {}, 0};
    const auto price = stock_price(s, symbol);
    if (!reduces) {
      if (s.config.rules.require_stop_loss)
        return CommandResult{failure(Reason::STOP_REQUIRED, "This plan requires a stop-loss; share entries do not support brackets"), {}, 0};
      if (const auto d = hedging_direction_check(s, symbol, static_cast<double>(signed_shares) * price->dollars()); !d.ok())
        return CommandResult{d, {}, 0};
      const auto before = measure(s);
      if (!before.valuation_complete)
        return CommandResult{failure(Reason::STALE_QUOTE, "All held positions need fresh marks before opening shares"), {}, 0};
      if (const auto d = loss_check(s, before); !d.ok()) return CommandResult{d, {}, 0};
      if (const auto limit = trade_risk_limit(s, before.equity)) {
        const auto loss = signed_shares > 0 ? std::optional(*price * signed_shares) : std::nullopt;
        if (!loss || *loss > *limit)
          return CommandResult{{Reason::MAX_TRADE_RISK, "Share entry risk before fees exceeds the plan limit",
                                loss ? std::optional(loss->dollars()) : std::nullopt, limit->dollars(), "trade"}, {}, 0};
      }
      State projected = s;
      trade_shares(projected, symbol, signed_shares, *price, StockSource::Trade);
      if (const auto d = check_exposure(measure(projected).risk, before.risk); !d.ok()) return CommandResult{d, {}, 0};
    }
    // Shares can cover options: selling the ones a short call is written against
    // leaves it naked, which may need more than the sale frees.
    if (s.config.rules.buying_power) {
      State projected = s;
      trade_shares(projected, symbol, signed_shares, *price, StockSource::Trade);
      if (free_power(projected) < free_power(s) && buying_power(projected).total.available < Money{}) {
        if (!reduces)
          return CommandResult{{Reason::BUYING_POWER, "These shares need more buying power than the account has available",
                                (-buying_power(projected).total.available).dollars(), 0.0, symbol}, {}, 0};
        return CommandResult{{Reason::BUYING_POWER, "These shares cover a short option that would need more buying power "
                              "than the account has available; buy the option back first", std::nullopt, std::nullopt, symbol}, {}, 0};
      }
    }
    const auto realised_before = s.ledger.account().realised;
    trade_shares(s, symbol, signed_shares, *price, StockSource::Trade);
    count_trade(s);
    if (!reduces) {
      ++s.guardrails.opening_trades;
      const auto limit = s.config.guardrails.max_opening_trades;
      if (limit > 0 && s.guardrails.opening_trades >= limit) latch_guardrail(s, Reason::TRADE_LIMIT, events);
    }
    if (s.config.guardrails.cooldown_loss > Money{} && s.ledger.account().realised - realised_before < -s.config.guardrails.cooldown_loss)
      begin_cooldown(s, events);
    event(events, "stock_trade", Json{{"symbol", symbol}, {"shares", signed_shares}, {"price", *price}});
    return CommandResult{};
  });
}
StockPreview TradingSession::preview_trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time,
    std::optional<StockPrice> price, Decision rejection) const {
  if (impl_->stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
  auto copy = std::make_unique<Impl>();
  copy->state = prepared(impl_->state, time, {});
  copy->actor = impl_->actor;
  copy->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(copy->state));
  const auto base = measure(copy->state).equity;
  StockPreview result;
  result.current = what_if_account(copy->state, base, {});
  TradingSession trial(std::move(copy));
  result.decision = rejection.ok() ? trial.trade_stock(symbol, signed_shares, time, price).decision : rejection;
  result.price = stock_price(trial.impl_->state, symbol);
  result.after = what_if_account(trial.impl_->state, base, {});
  return result;
}
TradingSession TradingSession::recover(const JournalRecovery& recovery, std::shared_ptr<Journal> journal) {
  const auto verified = reverify(recovery);
  if (journal && (recovery.truncated_final_line || journal->head() != verified.head || journal->sequence() != verified.records.size()))
    throw TradingError(Reason::JOURNAL_CORRUPT, "Recovery sink does not match verified journal head");
  auto impl = std::make_unique<Impl>();
  try {
    Json last;
    SharedVector<SettlementRecord> settlements;
    Money payouts_received;
    auto state = walk_states(verified.records, [&](const JournalRecord&, Json&& payload, const Json&) {
      for (const auto& event : payload.at("events"))
        if (event.at("type") == "payout") payouts_received = payouts_received + event.at("payload").at("trader_share").get<Money>();
      last = std::move(payload);
    }, &settlements);
    impl->state = state.get<State>();
    impl->state.settlements = std::move(settlements);
    impl->state.payouts_received = payouts_received;
    // Schema 3 records no snapshot; the reducer derives it from the state.
    impl->snapshot = std::make_shared<TradingSnapshot>(
        last.at("schema") == 3 ? snapshot_of(impl->state) : last.at("snapshot").get<TradingSnapshot>());
    // Older snapshots predate the exit value, which the books give.
    if (last.at("schema") != 3) {
      auto snapshot = std::make_shared<TradingSnapshot>(*impl->snapshot);
      snapshot->exit_equity = exit_equity_of(impl->state, *snapshot);
      snapshot->settlements = impl->state.settlements;
      snapshot->payouts_received = impl->state.payouts_received;
      impl->snapshot = std::move(snapshot);
    }
    if (last.at("schema") == 1) {
      // A schema 1 account has no rules; start its progress record from the
      // journal's first transaction so later records continue it.
      auto& s = impl->state;
      Evaluation e;
      for (const auto& r : verified.records) if (r.time > 0) { e.started = r.time; break; }
      e.starting_balance = s.config.initial_cash;
      e.peak = e.starting_balance;
      e.day = s.day;
      e.day_open_equity = s.start_equity;
      e.day_close_equity = impl->snapshot->equity;
      e.day_open_realised = net_realised(s);  // Unknown for the day in progress; count from here.
      e.cycle_started = e.started;
      s.evaluation = std::move(e);
      auto snapshot = std::make_shared<TradingSnapshot>(*impl->snapshot);
      snapshot->evaluation = s.evaluation;
      snapshot->buying_power = buying_power(s).total;
      impl->snapshot = std::move(snapshot);
    }
  } catch (const TradingError& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
  impl->journal = std::move(journal);
  return TradingSession(std::move(impl));
}
std::string TradingSession::compact(const JournalRecovery& recovery, Journal& out) {
  if (out.sequence() != 0) throw TradingError(Reason::JOURNAL_IO, "Compaction writes into an empty journal");
  const auto verified = reverify(recovery);
  (void)recover(recovery);  // Only a journal that recovers is rewritten.
  StateRecorder recorder;
  Json reread;
  try {
    const auto final_seq = verified.records.size();
    const auto last = walk_states(verified.records, [&](const JournalRecord& r, Json&& payload, const Json& state) {
      if (r.seq == final_seq && payload.at("schema") == 1) {
        // Recovery completes the evaluation of a journal whose last record is
        // schema 1 from that record, so it stays as it is.
        out.append(r.time, r.type, r.payload);
        return;
      }
      payload["schema"] = 3;
      payload.erase("snapshot");
      payload.erase("delta");
      recorder.add(payload, state);
      const auto line = payload.dump();
      // Read the record back as recovery will, before it is written.
      const auto check = Json::parse(line);
      if (const auto delta = check.find("delta"); delta != check.end()) detail::apply_state_delta(reread, *delta);
      else reread = check.at("state");
      if (reread != state)
        throw TradingError(Reason::JOURNAL_CORRUPT, "Compaction would change transaction " + std::to_string(r.seq));
      out.append(r.time, r.type, line);
    });
    if (Json::parse(verified.records.back().payload).at("schema") == 1) return recover(recovery).snapshot_json();
    return Json(snapshot_of(last.get<State>())).dump();
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
    catch (const std::invalid_argument& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
}
void TradingSession::expand(const JournalRecovery& recovery, Journal& out) {
  if (out.sequence() != 0) throw TradingError(Reason::JOURNAL_IO, "Expansion writes into an empty journal");
  const auto verified = reverify(recovery);
  (void)recover(recovery);
  try {
    (void)walk_states(verified.records, [&](const JournalRecord& r, Json&& payload, const Json& state) {
      if (payload.at("schema") != 3) { out.append(r.time, r.type, r.payload); return; }
      payload["schema"] = 2;
      payload.erase("delta");
      payload["snapshot"] = snapshot_of(state.get<State>());
      payload["state"] = state;
      out.append(r.time, r.type, payload.dump());
    });
  } catch (const Json::exception& e) { throw TradingError(Reason::JOURNAL_CORRUPT, e.what()); }
}
}  // namespace openport::trading
