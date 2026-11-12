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
Decision failure(Reason code, std::string message);
void event(Events& events, std::string_view type, Json payload);
Timestamp halt_end(const State& s, const std::string& underlying);
Quantity shares_held(const State& s, const std::string& symbol);
std::optional<Money> stock_price(const State& s, const std::string& symbol, Timestamp at, bool extended = false);
std::optional<Money> stock_price(const State& s, const std::string& symbol);
void trade_shares(State& s, const std::string& symbol, Quantity signed_shares, Money price, StockSource source,
                  const std::string& option = {});
Decision hedging_direction_check(const State& s, const std::string& underlying, double delta);
/// Positions for margin by symbol: signed contracts and, for shorts, their
/// buy-back value.
using MarginBook = std::map<std::string, std::pair<Quantity, Money>>;
MarginBook held_book(const State& s);
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
Measures measure(const State& s);
Decision loss_check(const State& s, const Measures& snapshot);
void latch_guardrail(State& s, Reason reason, Events& events);
void begin_cooldown(State& s, Events& events);
void count_close(State& s, Money realised, Events& events);
Decision account_type_check(const State& s, const std::vector<std::pair<std::string, Quantity>>& order,
                            const std::map<std::string, Quantity>& shares = {}, OrderId except = 0);
Decision account_check(const State& s, bool reducing = false, OrderId id = 0);
std::optional<Money> trade_risk_limit(const State& s, Money equity);
Decision hold_age_check(const State& s, Timestamp opened, const std::string& symbol);
void count_trade(State& s);
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
}  // namespace openport::trading
