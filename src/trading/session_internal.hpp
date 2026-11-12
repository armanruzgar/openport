#pragma once

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
#include <iterator>
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

// The paper-trading reducer's internals, shared by its translation units: session.cpp
// (TradingSession's commands) and the files holding the rules they apply. They were
// one translation unit's anonymous namespace; a helper only one file uses stays in an
// anonymous namespace there. Private: only src/trading includes this header.

namespace openport::trading::session_internal {
using detail::State;
using Events = std::vector<Json>;
inline Decision failure(Reason code, std::string message) { return {code, std::move(message), {}, {}, {}}; }
void add_order(State& s, const Order& order);
inline void event(Events& events, std::string_view type, Json payload) {
  events.push_back(Json{{"type", type}, {"payload", std::move(payload)}});
}
SettlementRecord settlement_record(const Position& position, Money value, Money intrinsic, Money fee,
                                   Timestamp time, std::optional<SettlementSource> source);
inline bool regular(const md::OptionContract& c, Timestamp time) { return md::trading_session(c.root, time).name == "regular"; }
inline bool extended(const OrderRequest& r) { return r.tif == TimeInForce::Exto || r.tif == TimeInForce::GtcExto; }
inline bool good_until(const OrderRequest& r) {
  return r.tif == TimeInForce::Gtc || r.tif == TimeInForce::GtcExto || r.tif == TimeInForce::Gtd;
}
inline bool immediate(const OrderRequest& r) {
  return r.tif == TimeInForce::Ioc || (r.type == OrderType::Market && r.trigger && (extended(r) || r.tif == TimeInForce::Gtd));
}
Decision session_check(const md::OptionContract& c, Timestamp time, const OrderRequest& r);
Timestamp session_end(const md::OptionContract& c, Timestamp time);
Timestamp order_expiry(const State& s, const OrderRequest& r);
Timestamp day_deadline(const State& s, const OrderRequest& r, Timestamp time);
/// Outlives a session: extended TIFs, GTC/GTD, bracket exits and armed orders. Once triggered,
/// a DAY order lasts its session like any other.
inline bool persistent(const Order& o) {
  return good_until(o.request) || extended(o.request) || o.role != OrderRole::Normal ||
         (o.request.trigger && (o.status == OrderStatus::Armed || o.request.tif != TimeInForce::Day));
}
Timestamp order_end(const State& s, const OrderRequest& r, Timestamp time);
inline bool trades_now(const Order& o, const md::OptionContract& c, Timestamp time) {
  if (!extended(o.request)) return !persistent(o) || regular(c, time);
  return md::trading_session(c.root, time).open;
}
/// Extended stop-market orders are simulator-held triggers. Outside regular
/// hours they send an IOC limit at the touch, taking displayed liquidity only.
inline bool touch_stop(const State& s, const Order& o) {
  return extended(o.request) && o.request.type == OrderType::Market && o.request.trigger &&
      !regular(s.contracts.at(order_symbols(o.request).front()), s.time);
}
Annotation clean_annotation(std::string note, const std::vector<std::string>& tags);
void store_annotation(State& s, const std::string& key, Annotation annotation, Events& events);
Decision system_check(const State& s, const Order& o, bool now = true);
inline Money mid(const QuoteObservation& q) {
  // Both sides are positive. Difference-first avoids overflowing their sum.
  return *q.bid + (*q.ask - *q.bid).prorate(1, 2);
}
/// A markable quote's mark: its mid, or halfway to the ask when nobody bids.
inline Money mark_of(const QuoteObservation& q) { return q.bid ? mid(q) : q.ask->prorate(1, 2); }
Timestamp halt_end(const State& s, const std::string& underlying);
Decision quote_check(const State& s, const std::string& symbol);
bool marked_now(const State& s, const std::string& symbol);
/// Orders kept within the position they close: bracket exits and reduce-only
/// closes shrink with it and are cancelled once it is flat, so they never open one.
inline bool kept_within(const Order& o) { return o.role != OrderRole::Normal || o.reduce_only; }
bool asks_only(const State& s, const std::string& symbol);
/// A combo exit's (or reduce-only close's) leg whose fresh quote shows only an ask.
/// The order buys such a leg back at that ask and gives it away at zero when it
/// sells it, rather than wait for a bid that may never come, so a worthless wing
/// cannot hold a stop, a target or a flatten back.
inline bool ask_only(const State& s, const Order& o, const Leg& leg) {
  return (kept_within(o) || o.request.exits_only) && asks_only(s, leg.symbol);
}
/// An exit's long leg that nobody bids for: sold at zero, without displayed size.
inline bool given_away(const State& s, const Order& o, const Leg& leg) { return leg.side == Side::Sell && ask_only(s, o, leg); }
bool closing_only(const State& s, const Order& o, bool include_working = true, bool include_armed = true);
bool ask_only_single(const State& s, const Order& o);
Decision price_check(const State& s, const QuoteObservation& q, Money price, std::optional<Money> center = {});
bool keeps_mark(const State& s, const QuoteObservation& previous, const QuoteObservation& quote, Timestamp time);
inline Quantity magnitude(Quantity q) { return q < 0 ? -q : q; }
inline Quantity held(const State& s, const std::string& symbol) {
  const auto it = s.ledger.positions().find(symbol);
  return it == s.ledger.positions().end() ? 0 : it->second.quantity;
}
inline const Valuation* valuation_of(const State& s, const std::string& symbol) {
  const auto it = s.valuations.find(symbol);
  return it == s.valuations.end() || !valid_valuation(it->second) ? nullptr : &it->second;
}
Attribution explain(const md::OptionContract& c, const detail::Reference& r, Money value, const Valuation* now);
void explain_part(State& s, const std::string& symbol, const Attribution& part);
void end_stretch(State& s, const std::string& symbol, Money value, const Valuation* now);
void start_stretch(State& s, const std::string& symbol);
void fill_position(State& s, const std::string& symbol, Quantity signed_quantity, Money price, Money fee);
inline Quantity closing_contracts(Quantity position, Side side, Quantity contracts) {
  return std::min(contracts, side == Side::Buy ? std::max<Quantity>(-position, 0) : std::max<Quantity>(position, 0));
}
Money commission_paid(const State& s, const Order& o, const std::string& symbol);
FillFees fees_for(const State& s, const std::string& symbol, Quantity contracts, Quantity closing, Money paid = {});
Money exercise_fee(const State& s, Quantity contracts);
bool physical(const md::OptionContract& c);
inline Quantity shares_held(const State& s, const std::string& symbol) {
  const auto it = s.ledger.stocks().find(symbol);
  return it == s.ledger.stocks().end() ? 0 : it->second.shares;
}
Money nearest_cent(Money amount);
bool before_split(const State& s, const std::string& underlying, Timestamp time);
std::optional<Money> stock_price(const State& s, const std::string& symbol, Timestamp at, bool extended = false);
std::optional<Money> stock_price(const State& s, const std::string& symbol);
Attribution explain_stock(const detail::Reference& r, Money value);
void start_stock_stretch(State& s, const std::string& symbol);
void trade_shares(State& s, const std::string& symbol, Quantity signed_shares, Money price, StockSource source,
                  const std::string& option = {});
/// What one contract of `symbol` delivers: its adjusted deliverable, or 100 shares.
inline Deliverable deliverable_of(const State& s, const std::string& symbol) {
  const auto it = s.deliverables.find(symbol);
  return it == s.deliverables.end() ? Deliverable{} : it->second;
}
Quantity delivered(const State& s, const md::OptionContract& c, Quantity contracts);
Money effective_strike(const State& s, const md::OptionContract& c);
Money unit_exercise_value(const State& s, const md::OptionContract& c, Money price);
Money unit_dividend(const State& s, const md::OptionContract& c, const Dividend& d, const std::vector<CorporateAction>& splits);
/// A bracket's two exits can fill only once between them: checks count the pair
/// through its earlier order, so the later one is shadowed while both are open.
/// Two orders that cancel each other may differ, so each counts in full.
inline bool shadowed(const State& s, const Order& o) {
  return o.role != OrderRole::Normal && o.oco != 0 && o.oco < o.id && s.orders.at(static_cast<std::size_t>(o.oco - 1)).open();
}
Quantity closing_capacity(const State& s, const std::string& symbol, Side side, OrderId skip = 0, bool include_armed = true);
/// Whether an order trades `symbol`, as its contract or one of its legs.
inline bool touches(const OrderRequest& r, const std::string& symbol) {
  return r.symbol == symbol || std::any_of(r.legs.begin(), r.legs.end(), [&](const Leg& leg) { return leg.symbol == symbol; });
}
inline Quantity signed_contracts(const Leg& leg, Quantity units) { return leg.side == Side::Buy ? units * leg.ratio : -units * leg.ratio; }
std::optional<double> held_direction(const State& s, const std::string& underlying);
OpeningOrder opening_exposure(const State& s, const OrderRequest& request,
                              const std::map<std::string, Quantity>& preceding = {});
Decision hedging_direction_check(const State& s, const std::string& underlying, double delta);
Quantity depth_used(Quantity size, Quantity left, Quantity offset = 0);
bool inside_single(const State& s, const Order& o);
std::optional<std::vector<Money>> inside_combo(const State& s, const Order& o);
Money execution_price(const State& s, const std::string& symbol, Side side, std::optional<Money> limit = {}, Quantity offset = 0,
                      std::int64_t stress = 0);
Money order_price(const State& s, const Order& o, const std::string& symbol, Side side,
                  std::optional<Money> limit = {}, Quantity offset = 0, bool stressed = true);
/// Whether the price band measures a stressed stop's price with its stress. Stress
/// is the simulator's own slippage in a fast market, not a price the market showed,
/// so driver 8 bands the price before it and a stop that the market lets trade
/// fills with its stress instead of being cancelled. Replay drivers 1-7 banded the
/// stressed price.
inline bool stress_in_band(const State& s) { return !s.revised_fills; }
std::vector<std::pair<Money, Quantity>> market_slices(const State& s, const std::string& symbol, Side side, Quantity quantity);
std::optional<Money> executable_net(const State& s, const Order& o, bool stressed = true);
bool latency_ready(const State& s, const Order& o);
/// Positions for margin by symbol: signed contracts and, for shorts, their
/// buy-back value.
using MarginBook = std::map<std::string, std::pair<Quantity, Money>>;
MarginBook held_book(const State& s);
/// Series of one underlying and one deliverable, margined together. An adjusted
/// series pays what `shares`/100 standard contracts struck at its effective strike
/// pay, so its group is margined as those standard series (each contract's values
/// scaled to 100 shares) and the requirement scaled back by shares/100. The shares
/// an adjusted group's shorts are written against are taken in its own lots first;
/// the standard group, first, keeps the rest.
struct LotGroup {
  Quantity shares = 100;
  std::vector<MarginLeg> legs;
  std::vector<MarginStock> stocks;
  std::map<std::string, std::string> symbols;  ///< Standard-equivalent OSI to the adjusted series.
};
std::vector<LotGroup> lot_groups(const State& s, std::vector<MarginLeg> legs, std::vector<MarginStock> stocks);
Money requirement(const State& s, const MarginBook& book);
Money free_power(const State& s);
Money stock_orders_reserved(const State& s);
void expire_stock_orders(State& s, Events& events);
void cancel_opening_stock_orders(State& s, const Decision& reason, Events& events);
void sweep_opening_stock_orders(State& s, bool skipped_end, Events& events);
struct PowerDetail {
  BuyingPower total;
  Money focus_reservation;
  Quantity focus_opening = 0;  ///< Contracts the focus order opens beyond earlier orders' claims.
  bool focus_uses = false;     ///< The focus order would reduce free buying power.
};
PowerDetail buying_power(const State& s, OrderId focus = 0);
struct ProjectedFill {
  Money premium;  ///< Paid, negative when received.
  Money fees;
  FillFees itemized;
};
ProjectedFill project_fill(State& after, const State& before, const OrderRequest& request, Quantity units, const Order& order);
bool frees_power(const State& s, const Order& o, const State& after);
bool fill_frees_power(const State& s, const Order& o);
std::optional<Money> soft_floor_at(const Guardrails& personal, const AccountRules& rules, Money plan_floor);
std::optional<Money> soft_floor_of(const State& s);
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
std::map<std::string, std::vector<std::string>> risk_tags(const State& s);
Measures measure(const State& s);
std::optional<OrderWait> waiting_for(const State& s, const Order& o, bool data_complete);
Decision reset_check(const State& s, const Measures& m);
Money exit_equity_of(const State& s, const TradingSnapshot& out);
TradingSnapshot snapshot_of(const State& s);
/// Rules only act on fully marked equity: every position has a mark, fresh or not.
template <class Account>
std::optional<Money> marked_equity(const Account& account) {
  for (const auto& p : account.positions) if (!p.market_value) return std::nullopt;
  for (const auto& p : account.stocks) if (!p.market_value) return std::nullopt;
  return account.equity;
}
inline Money net_realised(const State& s) { return s.ledger.account().realised - s.ledger.account().fees; }
inline std::string dollars(Money value) { return (value < Money{} ? "-$" + (-value).str() : "$" + value.str()); }
void cancel_order(State& s, OrderId id, Decision reason, Events& events);
Reason guardrail_reason(const State& s);
std::string clock_text(Timestamp time);
Decision kill_decision(const State& s);
void record_kill(State& s, std::string action, std::string reason, std::string previous);
bool completing_trade(const State& s, OrderId id);
void trip(State& s, const std::string& reason, Events& events);
Decision loss_check(const State& s, const Measures& snapshot);
void monitor_loss(State& s, Events& events);
void latch_guardrail(State& s, Reason reason, Events& events);
void refresh_guardrail_latch(State& s, Events& events);
void begin_cooldown(State& s, Events& events);
void flag_tilt(State& s, Reason code, std::string message, Events& events);
void count_close(State& s, Money realised, Events& events);
bool closing_stop(const State& s, const Order& order);
void guardrail_fill(State& s, OrderId id, bool opening, Money realised_before, Events& events);
void send_to_back(const State& s, Order& o, Timestamp time);
void keep_change(const State& s, Order& o, OrderChangeRecord change);
Decision entry_check(const State& s, const Order& o);
void check_rollover_positions(State& s, Events& events, bool date_changed);
void flatten_positions(State& s, std::string_view label, Events& events, const std::string& scope = {},
                       const std::vector<std::string>* only = nullptr);
void advance(State& s, Timestamp time, Events& events);
Decision account_type_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            const std::map<std::string, Quantity>& shares = {}, OrderId except = 0);
Decision defined_risk_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            std::string_view message = "This plan allows defined risk only: cover each short option with a "
                                                       "long of the same type that expires with it or later, or open the "
                                                       "spread as one order");
Decision open_orders_risk_check(const State& s, const Order& o);
Decision account_check(const State& s, bool reducing = false, OrderId id = 0);
std::optional<Money> expiry_loss(const State& s, const OrderRequest& request, Money premium, Money fees);
bool trade_rules_on(const AccountRules& rules);
std::optional<Money> trade_risk_limit(const State& s, Money equity);
struct TradeRisk {
  std::optional<Money> loss;
  std::string basis;
};
TradeRisk trade_risk(const State& s, const OrderRequest& r);
/// When an order is checked: as it is accepted or changed, as its trigger is
/// reached, or as it fills. A stop-limit's limit is set against its stop rather
/// than the market: it is banded around an option or combo trigger's level when
/// accepted, and once triggered only the price it fills at is banded, so a market
/// that gaps through the limit leaves it working instead of cancelled.
enum class Stage { Accept, Activate, Fill };
Decision hold_age_check(const State& s, Timestamp opened, const std::string& symbol);
bool reached(const State& s, const Order& o);
bool marketable(const Order& o, const QuoteObservation& q);
std::string decimal(double value, int places);
Quantity opening_size(const OrderRequest& r);
/// How far an opening order exceeds the rolling median, when it does: its size,
/// the median and the limit (median × size_creep_percent / 100).
struct SizeCreep { Quantity size; double median, limit; };
std::optional<SizeCreep> size_creep(const State& s, const Order& o);
std::string size_creep_message(const SizeCreep& c, std::int64_t percent);
Decision order_check(const State& s, const Order& o, Stage stage = Stage::Accept);
void observe_equity(State& s, Events& events);
void count_trade(State& s);
bool data_gap(Reason code);
std::uint64_t mix(std::uint64_t x);
bool queue_eligible(const State& s, const Order& o);
/// Where the order's own side of a valid quote stands against its limit: at it,
/// better (the order rests behind the touch) or worse (nothing displayed is ahead).
enum class Touch { At, Behind, Clear };
Touch touch_of(const QuoteObservation& q, Side side, Money limit);
void update_queues(State& s, const MarketActivity& activity, std::set<std::string>& changed);
std::string ioc_remainder_message(const State& s, const Order& o, std::string exhausted);
void end_ioc(State& s, OrderId id, std::string message, Events& events);
std::optional<Money> close_price(const State& s, const Order& o);
void match_order(State& s, OrderId id, Events& events, std::optional<OrderId> incoming);
void match_symbols(State& s, const std::set<std::string>& symbols, Events& events,
                   std::optional<OrderId> incoming = {});
Quantity exit_capacity(const State& s, const OrderRequest& r);
void sync_exits(State& s, const std::string& symbol, Events& events);
void attach_exits(State& s, OrderId entry_id, Events& events);
std::optional<Money> trigger_value(const State& s, const Order& o);
detail::QuoteActivity next_activity(const State& s, const detail::Book& before, const QuoteObservation& quote);
void check_triggers(State& s, Events& events);
/// One of a flatten's closes: a single contract, or a short and the long that covers
/// it closing together (the short bought back, the long sold), `units` of each.
struct PlannedClose {
  std::vector<Leg> legs;
  Quantity units = 0;
};
std::vector<PlannedClose> plan_closes(const State& s, const std::vector<std::string>& symbols,
                                      std::map<std::string, Quantity>& held_back);
void close_shares(State& s, const std::string& symbol, Quantity shares, StockSource source, Events& events);
void monitor_calendar(State& s, Events& events);
PlanInputs plan_inputs(const State& s, Money equity);
void refresh_trade_objectives(State& s);
void monitor_rules(State& s, Events& events);
Decision acceptance_check(const State& s, const Order& candidate, const Decision& rejection);
BreachRisk breach_of(const State& s, const std::map<std::string, double>& close_variances);
std::vector<RiskWarning> warnings_of(const State& s, const std::map<std::string, double>& close_variances,
                                     const std::vector<Dividend>& dividends, const std::vector<CorporateAction>& splits);
struct PreviewProjection {
  OrderPreview result;
  State projected;
};
PreviewProjection project_working(const State& before, State after_state, OrderId id, Decision decision);
PreviewProjection project_order(const State& before, OrderRequest request, const Decision& rejection);
Decision change_check(const State& s, const Order& order, const Decision& rejection);
void change_terms(State& s, OrderId id, const OrderChange& change);
State prepared(const State& state, Timestamp time, const PreviewMarket& market);
void check_alerts(State& s, Events& events);
WhatIfAccount what_if_account(const State& s, Money base_equity, const std::map<std::string, double>& close_variances);
PreviewExecution executed(const State& before, const State& trial, OrderId id, PreviewExecution execution);
struct Sizing {
  std::optional<Quantity> units;
  std::string basis;
  std::optional<Quantity> buying_power;
  std::optional<Quantity> floor;
};
Sizing size_order(const std::function<const OrderPreview&(Quantity)>& sized, Quantity upper,
                  std::optional<Money> room, double floor_share);
CommandResult place(State& s, OrderRequest request, Timestamp time, const Decision& rejection, Events& events);
void place_chained(State& s, Events& events);
CommandResult change_order(State& s, OrderId id, const OrderChange& change, const Decision& rejection, Events& events);

inline constexpr std::uint64_t kCheckpointEvery = 1000;
Json state_change(const State& before, const State& after);
/// When set, each record's change is also found by writing out both states,
/// and a difference throws (OPENPORT_VERIFY_JOURNAL).
inline const bool check_changes = std::getenv("OPENPORT_VERIFY_JOURNAL") != nullptr;
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
Decision share_entry_checks(const State& s, const std::string& symbol, Quantity signed_shares, const ShareTerms& terms);
Decision opening_share_check(const State& s, const std::string& symbol, Quantity signed_shares, Money price,
                             const ShareTerms& terms, Events& events);
bool reduces_shares(const State& s, const std::string& symbol, Quantity signed_shares);

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
Decision trade_shares_checked(State& s, const std::string& symbol, Quantity signed_shares, Money price,
                              const ShareTerms& terms, Events& events);
Quantity pending_short_shares(const State& s, const std::string& symbol, OrderId except, Quantity extra);
void end_stock_order(State& s, OrderId id, OrderStatus status, Decision reason, Events& events);
std::pair<Decision, OrderId> accept_stock_order(State& s, StockOrderRequest request, OrderRole role, OrderId parent,
                                                Events& events, OrderId pair = 0);
void work_stock_orders(State& s, Events& events, std::optional<OrderId> only = std::nullopt);
}  // namespace openport::trading::session_internal

namespace openport::trading {
// TradingSession's members call the helpers unqualified, as they did when the helpers
// were an anonymous namespace in the same file.
using namespace session_internal;

/// A session's state, journal and published snapshot, and the transaction every
/// command runs in. session.cpp and session_journal.cpp define TradingSession's members.
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
           open_ids(s).empty() && s.alerts.items.empty() && open_stock_ids(s).empty();
  }

  CommandResult transact(Timestamp time, std::string_view type,
                         const std::function<CommandResult(State&, Events&)>& action,
                         const std::optional<SettlementSource>& source = {},
                         const std::function<void(State&)>& prepare = {}) {
    if (stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
    State next = state;
    next.opening_rule_inputs = opening_rule_inputs;
    next.compact_opening_rule_inputs = compact_opening_rule_inputs;
    Events events;
    if (prepare) prepare(next);
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
      // An order its walk filled here places its `then` now: reindex below drops it
      // from the orders a later place_chained visits. Replay drivers 1-7 never placed it.
      if (next.revised_fills) place_chained(next, events);
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
}  // namespace openport::trading
