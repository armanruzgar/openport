// Helpers over a paper account's state: when an order may trade and until when, quotes
// and marks, positions and the attribution of their P&L, fees, shares and splits,
// adjusted deliverables, closing capacity, and the prices orders fill at.

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
/// Appends an order; its client order ID keeps naming the first order that used it
/// in the current attempt, so an account reset frees the IDs earlier attempts used.
void add_order(State& s, const Order& order) {
  const auto& key = order.request.client_order_id;
  if (const auto first = s.clients.find(key); first == s.clients.end() || first->second < s.evaluation.first_order)
    s.clients[key] = order.id;
  s.orders.push_back(order);
}
SettlementRecord settlement_record(const Position& position, Money value, Money intrinsic, Money fee,
                                   Timestamp time, std::optional<SettlementSource> source) {
  const auto proceeds = (intrinsic * 100) * position.quantity;
  return {position.contract, value, time, position.quantity, proceeds - fee, proceeds - position.basis, fee, std::move(source)};
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
Timestamp halt_end(const State& s, const std::string& underlying) {
  Timestamp end = 0;
  for (const auto& symbol : {underlying, std::string("*")})
    if (const auto it = s.market_halts.find(symbol); it != s.market_halts.end()) end = std::max(end, it->second);
  return end;
}
Decision quote_check(const State& s, const std::string& symbol) {
  const auto contract = s.contracts.find(symbol);
  const auto end = contract == s.contracts.end() ? 0 : halt_end(s, contract->second.underlying);
  if (s.time < end) return failure(Reason::MARKET_HALTED, "Underlying trading is halted");
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !valid_quote(it->second.quote))
    return failure(Reason::INVALID_QUOTE, "A noncrossed positive two-sided quote with both sizes is required");
  const auto time = it->second.quote.time;
  if (time < end || time > s.time || observation_time(s.contracts.at(symbol), s.time) - time > s.config.limits.max_quote_age)
    return failure(Reason::STALE_QUOTE, "Quote is outside the configured market-time freshness window");
  return {};
}
/// Whether a position's quote marks it now: markable, and inside the freshness window.
/// A far option nobody bids for is marked; only a valid quote trades.
bool marked_now(const State& s, const std::string& symbol) {
  const auto it = s.books.find(symbol);
  if (it == s.books.end() || !markable_quote(it->second.quote)) return false;
  const auto time = it->second.quote.time;
  return time <= s.time && observation_time(s.contracts.at(symbol), s.time) - time <= s.config.limits.max_quote_age;
}
/// A fresh quote that shows only an ask, as a far option nobody bids for does.
bool asks_only(const State& s, const std::string& symbol) {
  if (const auto it = s.contracts.find(symbol); it != s.contracts.end()) {
    const auto end = halt_end(s, it->second.underlying);
    const auto book = s.books.find(symbol);
    if (s.time < end || (book != s.books.end() && book->second.quote.time < end)) return false;
  }
  const auto it = s.books.find(symbol);
  return it != s.books.end() && !it->second.quote.modelled && !valid_quote(it->second.quote) && marked_now(s, symbol);
}
/// The same for a single contract: a buy that only closes a short takes an ask-only
/// quote within its size (a short quoted 0.00/0.05 can be bought back), and a
/// reduce-only market close gives a long nobody bids for away at zero. So does a
/// limit flatten's close while it follows the touch (`limit_ticks`), as its combo
/// closes give such a leg away (driver 8; replay drivers 1-7 left it at 0.00).
bool ask_only_single(const State& s, const Order& o) {
  if (multi_leg(o.request) || !asks_only(s, o.request.symbol)) return false;
  if (o.request.side == Side::Buy) return kept_within(o) || closing_only(s, o, false);
  return o.reduce_only && (o.request.type == OrderType::Market || (s.revised_fills && o.limit_ticks));
}
/// The price band around the quote's mid, or around `center` (a stop-limit's trigger level).
Decision price_check(const State& s, const QuoteObservation& q, Money price, std::optional<Money> center) {
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
namespace {
/// A two-sided quote so wide that its bid or ask lies outside the price band around
/// its own midpoint: no order could trade at either side.
bool wide_quote(const State& s, const QuoteObservation& q) {
  return valid_quote(q) && (!price_check(s, q, *q.bid).ok() || !price_check(s, q, *q.ask).ok());
}
}  // namespace
/// Whether `quote`, observed at `time` after `previous`, leaves its contract's mark as
/// it is. Width alone does not move a mark: a wide quote keeps the mark a narrower
/// quote set within the quote age, so it moves no equity and decides no plan. A wide
/// market that outlasts that marks again, and then follows its midpoints. Replay
/// drivers 1-6 mark every quote.
bool keeps_mark(const State& s, const QuoteObservation& previous, const QuoteObservation& quote, Timestamp time) {
  if (!s.evaluation.revised_rules || !wide_quote(s, quote)) return false;
  const auto mark = s.marks.find(quote.symbol);
  if (mark == s.marks.end() || time - mark->second.time > s.config.limits.max_quote_age) return false;
  // The mark already follows a wide market when the wide quote before this one set it.
  return !(wide_quote(s, previous) && previous.time == mark->second.time && mark_of(previous) == mark->second.price);
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
namespace {
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
}  // namespace
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
FillFees fees_for(const State& s, const std::string& symbol, Quantity contracts, Quantity closing, Money paid) {
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
/// A payment in whole cents, as brokers pay one: nearest cent, ties away from zero.
Money nearest_cent(Money amount) {
  constexpr std::int64_t cent = 10'000;
  const auto micros = amount.micros();
  const auto rest = micros % cent;
  auto rounded = micros - rest;
  if (2 * (rest < 0 ? -rest : rest) >= cent) rounded += micros < 0 ? -cent : cent;
  return Money::from_micros(rounded);
}
/// Whether an option quote first given at `time` on a series of `underlying` predates a
/// split of it the account has applied: given before the ex-date's regular open, when
/// the series first trade split, it prices them as they were, and an adjusted series'
/// standard equivalent was another series then (driver 9).
bool before_split(const State& s, const std::string& underlying, Timestamp time) {
  return s.revised_corporate_actions &&
      std::any_of(s.corporate_actions.begin(), s.corporate_actions.end(), [&](const CorporateActionRecord& r) {
        return r.symbol == underlying && time < md::new_york_to_utc(r.ex_date, 9, 30);
      });
}
/// The underlying's price if fresh at `at`: within max_quote_age of it while the
/// stock market is open, or of its last close while it is not (the 16:00 close
/// stays current while the options trade on to 16:15, and overnight). `extended`
/// measures it against the session with premarket and after hours instead, as an
/// extended-hours share order fills in it: at 18:30 the 16:00 close is stale.
std::optional<Money> stock_price(const State& s, const std::string& symbol, Timestamp at, bool extended) {
  const auto end = halt_end(s, symbol);
  if (at < end) return std::nullopt;
  const auto it = s.stock_marks.find(symbol);
  if (it == s.stock_marks.end() || it->second.time < end || it->second.time > at) return std::nullopt;
  const auto session = extended ? md::stock_extended_session(at) : md::stock_session(at);
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
                  const std::string& option) {
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
/// Shares an option position becomes: its deliverable's shares a contract (100 for a
/// standard series), bought by long calls and short puts, sold by long puts and short calls.
Quantity delivered(const State& s, const md::OptionContract& c, Quantity contracts) {
  return (c.type == pricing::OptionType::Call ? 1 : -1) * deliverable_of(s, c.osi_symbol()).shares * contracts;
}
/// What a share of the deliverable costs at the strike: the strike, for a standard
/// series; 100 times it, less the cash in lieu, over the deliverable's shares. A
/// deliverable of cash only has no share to price; it keeps the strike.
Money effective_strike(const State& s, const md::OptionContract& c) {
  const Money strike = Money::from_double(c.strike);
  const auto d = deliverable_of(s, c.osi_symbol());
  if (standard_deliverable(d) || d.shares == 0) return strike;
  return (strike * 100 - d.cash).prorate(1, d.shares);
}
/// What exercising one unit (a contract over its multiplier of 100) is worth with the
/// underlying at `price`, negative out of the money: the deliverable's shares and cash
/// over 100, against the strike.
Money unit_exercise_value(const State& s, const md::OptionContract& c, Money price) {
  const Money strike = Money::from_double(c.strike);
  const auto d = deliverable_of(s, c.osi_symbol());
  const Money worth = standard_deliverable(d) ? price : (price * d.shares + d.cash).prorate(1, 100);
  return c.type == pricing::OptionType::Call ? worth - strike : strike - worth;
}
/// What a dividend pays on what exercising one unit (a contract over its multiplier of
/// 100) delivers: a share's dividend times the deliverable's shares over 100, once the
/// stock's splits in `splits` going ex by the dividend's ex-date have split them, as the
/// rollover splits shares before it pays. A series delivering cash only collects none.
/// Replay drivers 1-8 weighed a share's dividend against a unit, whatever it delivered.
Money unit_dividend(const State& s, const md::OptionContract& c, const Dividend& d, const std::vector<CorporateAction>& splits) {
  if (!s.revised_corporate_actions) return d.per_share;
  auto shares = deliverable_of(s, c.osi_symbol()).shares;
  for (const auto& a : splits) {
    if (a.symbol != c.underlying || a.ex_date > d.ex_date) continue;
    const bool applied = std::any_of(s.corporate_actions.begin(), s.corporate_actions.end(),
        [&](const CorporateActionRecord& r) { return r.symbol == a.symbol && r.ex_date == a.ex_date; });
    if (!applied) shares = shares * a.numerator / a.denominator;
  }
  return d.per_share.prorate(shares, 100);
}
/// Contracts not reserved by working user orders on the closing side. Managed
/// exits shrink with the position and system IOCs are capped at execution too.
Quantity closing_capacity(const State& s, const std::string& symbol, Side side, OrderId skip, bool include_armed) {
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
Quantity depth_used(Quantity size, Quantity left, Quantity offset) {
  Quantity used = 0;
  if (__builtin_sub_overflow(size, left, &used) || __builtin_add_overflow(used, offset, &used))
    throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Simulated depth exceeds quantity range");
  return used;
}
namespace {
/// Strictly inside, at the configured fraction of the way to the far side.
bool inside_reach(const State& s, Money near, Money far, Money limit) {
  if (s.config.rules.inside_fill_percent == 0 || limit <= near || limit >= far) return false;
  __extension__ using Wide = __int128;
  return (static_cast<Wide>(limit.micros()) - near.micros()) * 100 >=
      (static_cast<Wide>(far.micros()) - near.micros()) * s.config.rules.inside_fill_percent;
}
}  // namespace
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
/// Slippage and simulated depth use the displayed price's tick tier. A negative
/// budget carries depth consumed by earlier orders on this same observation.
/// `stress` adds a stressed stop's extra adverse ticks (see stressed).
Money execution_price(const State& s, const std::string& symbol, Side side, std::optional<Money> limit, Quantity offset,
                      std::int64_t stress) {
  const auto& book = s.books.at(symbol);
  const auto& quote = book.quote;
  const bool buy = side == Side::Buy;
  const auto displayed = buy ? *quote.ask : *quote.bid;
  const auto size = buy ? quote.ask_size : quote.bid_size;
  const auto left = buy ? book.ask_left : book.bid_left;
  const auto blocks = s.config.rules.impact_ticks > 0 ? depth_used(size, left, offset) / size : 0;
  const auto tick = tick_size(s.contracts.at(symbol).root, displayed);
  const auto slip = tick * (s.config.rules.slippage_ticks + stress) + tick * s.config.rules.impact_ticks * blocks;
  const auto price = buy ? displayed + slip : std::max(Money{}, displayed - slip);
  return limit && s.config.rules.impact_ticks == 0 ? (buy ? std::min(*limit, price) : std::max(*limit, price)) : price;
}
/// `stressed` false leaves out a stressed stop's extra ticks, as the price band may
/// (see stress_in_band).
Money order_price(const State& s, const Order& o, const std::string& symbol, Side side,
                  std::optional<Money> limit, Quantity offset, bool stressed) {
  if (inside_single(s, o)) return *o.request.limit_price;
  if (touch_stop(s, o)) {
    const auto& q = s.books.at(symbol).quote;
    return side == Side::Buy ? *q.ask : *q.bid;
  }
  return execution_price(s, symbol, side, limit, offset, stressed ? o.stress_ticks : 0);
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
/// A multi-leg order's next executable net debit per unit; `stressed` as for order_price.
std::optional<Money> executable_net(const State& s, const Order& o, bool stressed) {
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
      const auto price = order_price(s, o, leg.symbol, leg.side, {}, 0, stressed) * leg.ratio;
      net = leg.side == Side::Buy ? net + price : net - price;
    } else {
      for (Quantity offset = 0; offset < leg.ratio; ++offset) {
        const auto price = order_price(s, o, leg.symbol, leg.side, {}, offset, stressed);
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
/// Cancel an open order now, with the reason it records.
void cancel_order(State& s, OrderId id, Decision reason, Events& events) {
  if (!s.orders.at(static_cast<std::size_t>(id - 1)).open()) return;
  auto& order = s.orders.mut(id - 1);
  order.status = OrderStatus::Cancelled;
  order.reason = std::move(reason);
  order.ended_at = s.time;
  event(events, "cancel", order);
}
}  // namespace session_internal
}  // namespace openport::trading
