#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include "openport/trading/dividends.hpp"
#include "openport/trading/evaluation.hpp"
#include "openport/trading/journal.hpp"
#include "openport/trading/risk.hpp"

namespace openport::trading {

/// Provenance fields retain the integration's exact journal representation.
using SettlementSource = std::map<std::string, std::string>;
struct SettlementRecord {
  md::OptionContract contract;
  Money value;
  Timestamp time = 0;
  Quantity quantity = 0;  ///< Signed contracts settled.
  Money cash;            ///< Option proceeds less settlement fees; excludes share delivery.
  Money realised;        ///< Gross P&L released by this settlement, before fees.
  Money fee;
  std::optional<SettlementSource> source;
};

struct MarkedPosition {
  Position position;
  std::optional<Money> mark;  ///< Never substituted with zero when missing.
  Timestamp mark_time = 0;
  Timestamp mark_age = 0;
  std::optional<Money> market_value;
  std::optional<Money> unrealised;
  bool fresh = false;
  bool awaiting_settlement = false;
  /// It cannot be sold now: its fresh quote shows only an ask, as a far option
  /// nobody bids for does, or it has expired. Such a long can be abandoned.
  bool no_bid = false;
  /// A long option the trader instructed not to be exercised at expiry.
  bool do_not_exercise = false;
};
struct MarkedStock {
  StockPosition position;
  std::optional<Money> mark;  ///< The underlying's latest price.
  Timestamp mark_time = 0;
  std::optional<Money> market_value;
  std::optional<Money> unrealised;
  bool fresh = false;
};
/// What keeps an open order from filling now, read from the account's books,
/// sessions and clocks as matching would read them. Derived for each snapshot,
/// never journaled. `code` is TRIGGER (armed until its level is reached),
/// REGULAR_SESSION (waits for the regular session), INVALID_QUOTE or STALE_QUOTE
/// (no usable quote on a leg), FILL_LATENCY (held for a quote stamped after the
/// account's delay), NEWER_QUOTE (resting orders take quotes newer than their
/// acceptance), LIMIT (the far side is worse than the limit), DISPLAYED_SIZE (the
/// quote's displayed size is used up until a new one) or STALE_DATA (held
/// positions need fresh marks and valuations first).
struct OrderWait {
  std::string code;
  std::string message;
};
struct TradingSnapshot {
  std::uint64_t account_version = 0;
  Timestamp time = 0;
  Account account;
  Money equity;  ///< Last-mark estimate; consult valuation_complete.
  Money start_of_day_equity;
  Money unrealised;
  bool valuation_complete = true;
  bool journal_failed = false;
  std::vector<MarkedPosition> positions;
  std::vector<MarkedStock> stocks;  ///< Shares opened by trading, exercise or assignment.
  std::vector<Order> open_orders;
  SharedVector<Order> recent_orders;  ///< All v1 orders, in acceptance sequence.
  /// Why each open order that is not filling waits, by order ID (see OrderWait).
  std::map<OrderId, OrderWait> waiting;
  /// The kill latch's trips, resets and releases, oldest first.
  std::vector<KillChange> kill_history;
  /// While latched, why reset_kill would not clear it now and when it can (a
  /// personal guardrail's expiry, or a daily loss back within its limit or the
  /// next trading day); NONE when a reset would clear it, or nothing is latched.
  Decision kill_reset;
  SharedVector<Fill> recent_fills;    ///< All v1 fills, in execution sequence.
  RiskSnapshot risk;
  ScenarioGrid scenarios;
  std::vector<Reason> quality_flags;
  Evaluation evaluation;             ///< Current attempt's rule progress.
  std::optional<Limits> pending_limits;
  std::optional<Guardrails> pending_guardrails;
  GuardrailState guardrails;
  std::optional<Money> soft_floor;
  Timestamp pending_applied_at = 0;
  BuyingPower buying_power;
  /// The positions' margin requirement by underlying, with the parts that hold
  /// it. Derived from the positions, so snapshot records do not carry it.
  std::vector<MarginUnderlying> margin;
  SharedVector<SettlementRecord> settlements;  ///< Oldest first, rebuilt from journal events.
  SharedVector<Closure> closures;     ///< Settlements and resets, in sequence.
  AttemptFee fee_charged;
  std::optional<Money> payouts_received; ///< Derived from payout events across all attempts; never serialized.
  std::vector<AttemptSummary> attempts;  ///< Earlier attempts, oldest first.
  SharedVector<StockFill> stock_fills;    ///< Every change in shares held, oldest first.
  SharedVector<DividendPayment> dividends;  ///< Dividends paid on held shares, oldest first.
  /// Notes and tags by trade, named by its opening fill's ID.
  SharedMap<std::string, Annotation> annotations;
  SharedMap<std::string, DayNote> day_notes;
  SharedMap<std::string, TradeReview> trade_reviews;
  SharedMap<std::string, TradeReview> strategy_reviews;
  /// Closing prints recorded for PM settlement, by "UNDERLYING YYYY-MM-DD".
  SharedMap<std::string, ClosingPrint> closing_prints;
  /// Today's P&L by Greek for the account, and for each contract held or traded
  /// today. Positions held from before an upgrade join at the next fill or rollover.
  Attribution attribution;
  std::map<std::string, Attribution> attributions;
  /// Each round trip's P&L by Greek over its life, by trade ID (an option's opening
  /// fill, "s" and the opening stock fill for shares), its open stretch to the marks
  /// now included. Round trips open from before an upgrade have none.
  SharedMap<std::string, Attribution> trip_attributions;
  /// The trade each round trip is in where it is not its root's (see trade_group),
  /// and whole-trade reviews of trades with more than one entry, by trade ID.
  SharedMap<std::string, std::string> groups;
  SharedMap<std::string, TradeReview> group_reviews;
  std::vector<Alert> alerts;  ///< The account's alerts, oldest first.
  /// Equity if every position closed now with a market order at the displayed
  /// quotes (bids for longs, asks for shorts) and the account's slippage and
  /// impact, after fees using the account's fee schedule when it has one. A long
  /// nobody bids for counts as nothing, a short without an ask at its mark, an
  /// expired position at its mark and shares at their price.
  /// Derived from the books, so snapshot_json leaves it out.
  Money exit_equity;
};
/// What the plan's rules read of a published account (see PlanInputs).
[[nodiscard]] PlanInputs plan_inputs(const TradingSnapshot& snapshot);
/// Derived from journaled attempt fees and payout history; never changes the ledger.
struct ProgramCosts {
  Money evaluation, reset, activation, payouts_received;
  std::int64_t resets_used = 0;
  [[nodiscard]] Money total() const { return evaluation + reset + activation; }
};
[[nodiscard]] ProgramCosts program_costs(const TradingSnapshot& snapshot, const AccountRules& rules);

/// A funded account's standing for its next payout: an active, flat account
/// with the required qualifying days since the last payout. `blocked` is the
/// first unmet requirement; when it is NONE, any whole-cent amount from
/// `minimum` to `maximum` is accepted.
struct PayoutQuote {
  Decision blocked;
  std::uint64_t number = 1;      ///< The next payout's number.
  bool funded = false;           ///< Funded phase with payout rules.
  bool active = false;
  bool flat = false;             ///< No positions and no working or armed orders.
  std::uint64_t qualifying_days = 0;
  std::int64_t required_days = 0;
  Money profit;                  ///< Equity less the starting balance.
  Money withdrawable;            ///< withdrawal_percent of positive profit, whole cents.
  std::optional<Money> cap;      ///< This payout number's cap.
  Money maximum;                 ///< min(withdrawable, cap, buffer room), leaving equity above a locked floor.
  Money minimum;
  Money trader_share;            ///< split_percent of the maximum.
  std::optional<std::int64_t> consistency_percent;  ///< This payout number's best-day limit.
  Money cycle_profit;            ///< Net realised on this cycle's finished days and the day in progress.
  std::optional<Money> best_day;  ///< Largest positive day in the cycle; earliest wins ties.
  md::Date best_day_date;
  Money consistency_needed;      ///< Additional cycle profit needed, rounded up to whole cents.
  std::optional<Money> buffer_balance;  ///< Starting balance plus buffer, while it applies.
};
[[nodiscard]] PayoutQuote payout_quote(const TradingSnapshot& snapshot, const AccountRules& rules);
/// The held book's risk profile (see risk_profile) as a snapshot publishes it: its
/// equity, the plan floor under a drawdown rule, the soft floor and its fresh share
/// prices, at its time. A nonempty `underlying` limits it to that underlying's
/// contracts and shares. An incomplete valuation leaves it incomplete.
[[nodiscard]] RiskProfile snapshot_profile(const TradingSnapshot& snapshot, const SessionConfig& config,
    const Valuations& valuations, const ProfileConfig& profile, double reference_spot, const std::string& underlying = {});

/// A position a flatten left open: the contracts still held, those its closes are
/// still working on later quotes, and why the rest are not being closed (a refusal,
/// a settlement still to come, or an expired short a long still covers); the
/// reason is empty while every contract left is being worked.
struct Residual {
  std::string symbol;
  Quantity quantity = 0;  ///< Signed contracts still held.
  Quantity working = 0;   ///< Contracts a reduce-only close is still working.
  Decision reason;
};

struct CommandResult {
  Decision decision;
  std::optional<OrderId> order_id;
  std::uint64_t account_version = 0;
  /// A retried order, its client_order_id and terms already submitted: the first
  /// answer, with nothing recorded.
  bool replayed = false;
  /// Delivered shares a flatten could not close, by symbol, and why.
  std::map<std::string, Decision> kept_stocks = {};
  std::optional<std::uint64_t> alert_id = {};  ///< create_alert: the new alert.
  /// Positions in scope a flatten left open, by symbol.
  std::vector<Residual> residuals = {};
};

/// Current integration inputs for contracts not yet registered by this account.
/// Used only on the preview's private copy.
struct PreviewMarket {
  std::vector<md::OptionContract> contracts;
  std::vector<QuoteObservation> quotes;
  std::vector<Valuation> valuations;
};
/// Read-only pre-trade facts for server gates. Dollar delta is the whole order's
/// signed delta, including its closing legs. Missing Greeks leave decision set
/// to MISSING_VALUATION; contract counts remain available.
struct OpeningOrder {
  bool opening = false;
  std::map<std::string, Quantity> contracts;
  std::map<std::string, double> dollar_delta;
  Decision decision;
};
/// Each other account's held book and working opening orders are compared
/// separately: opposing working orders must not hide each other by netting.
struct CounterExposure {
  std::string account;
  std::string underlying;
  std::optional<double> dollar_delta;
};
/// Explicit integration inputs; the reducer never reads another account or feed.
/// Missing volume means unknown. Recorded with each attempted opening fill.
struct OpeningRuleInputs {
  std::vector<CounterExposure> counter_positions;
  std::map<std::string, std::optional<std::int64_t>> volumes;
};
[[nodiscard]] Decision counter_position_check(const std::map<std::string, double>& direction,
    const std::vector<CounterExposure>& others);
[[nodiscard]] Decision volume_share_check(const std::string& symbol, Quantity held, Quantity opening,
    std::int64_t percent, std::optional<std::int64_t> volume);
/// Advice on accepted terms that are rarely meant: STOP_AS_LIMIT (a bracket stop
/// given only a limit price rests as a limit exit), STOP_REACHED (its trigger is
/// already reached, so it fires at once), TARGET_REACHED (the take-profit is already
/// marketable), TRIGGER_REACHED (a conditional order activates at once) or
/// SLIPPAGE_BAND (the account's slippage alone prices a market order outside the band).
struct OrderWarning {
  std::string code;
  std::string message;
};
/// A block of contracts at one price, as a fill would trade it.
struct PreviewFill {
  std::string symbol;
  Side side = Side::Buy;
  Quantity quantity = 0;
  Money price;
};
/// What submitting an order now would execute, from the same acceptance and
/// matching run on a private copy of the account, and its full size priced at
/// the current far sides.
struct PreviewExecution {
  OrderStatus status = OrderStatus::Rejected;  ///< The order's state just after submission.
  Quantity filled_quantity = 0;   ///< Units (contracts for one leg) that fill at once.
  Quantity remaining_quantity = 0;
  Decision reason;                ///< Why it was refused, or why its rest ended at once (IOC_REMAINDER).
  std::vector<PreviewFill> fills; ///< The fills it gets at once.
  std::optional<Money> average_fill_price;  ///< Per unit; a multi-leg order's net debit, negative a credit.
  /// The full size at the current far sides, slipped and block by block through
  /// impact as a fill would walk them, whatever displayed size there is.
  std::vector<PreviewFill> schedule;
  std::optional<Money> average_price;  ///< The schedule per unit, as average_fill_price.
};
/// The quote one of a previewed order's legs would trade against: whether paper
/// orders can fill on it now (`quote` says why not, as INVALID_QUOTE or
/// STALE_QUOTE), the displayed size on the side the leg takes, and what this
/// account's orders have left of it on that observation.
struct LegLiquidity {
  std::string symbol;
  Side side = Side::Buy;
  Quantity contracts = 0;
  Decision quote;
  Quantity displayed = 0;
  Quantity left = 0;
};
/// What this account's orders have left of a quote's displayed size, by side,
/// for the observation it holds; below zero once impact depth is in use.
struct SizeLeft {
  std::uint64_t observation = 0;
  Quantity bid_size = 0;
  Quantity ask_size = 0;
  Quantity bid = 0;
  Quantity ask = 0;
};
struct OrderPreview {
  Decision decision;
  std::vector<OrderWarning> warnings;
  Money buying_power_required;
  Money buying_power_before;
  /// Available while the order works on its terms, before it fills: for a change,
  /// with what it reserved before released.
  std::optional<Money> buying_power_working;
  /// Available once the order has filled in full at the projected price; for an
  /// exits-only pair, which rests until one exit fills, once it is accepted.
  std::optional<Money> buying_power_after;
  std::optional<Exposure> exposure_change;
  std::optional<Money> max_loss;
  std::string max_loss_basis;  ///< "expiry_payoff" or "scenario_grid"; empty when unavailable.
  std::optional<Money> trade_risk;
  std::optional<Money> trade_risk_limit;
  std::string trade_risk_basis;  ///< "stop_loss", "expiry_payoff" or "unbounded_or_unknown"; empty when not applicable.
  std::optional<Money> equity_at_max_loss;
  std::optional<bool> breaches_floor;
  std::optional<bool> breaches_soft_floor;
  /// Units that fit buying power, the pre-trade limits and the floor share; zero
  /// when none do. Empty when sizing is unavailable: an identical retry, a stopped
  /// account, an order refused at any size for another reason, or a loss that
  /// cannot be projected.
  std::optional<Quantity> max_units;
  /// Binding sizing constraint: "floor", "buying_power" or "limits"; empty when unavailable.
  std::string max_units_basis;
  /// The same sizing with buying power and the limits alone, floor room aside.
  std::optional<Quantity> max_units_buying_power;
  /// The same sizing with the floor share and the pre-trade checks alone: without
  /// the fit to available buying power, though a plan's buying-power rule still
  /// refuses. Empty without a plan or soft floor.
  std::optional<Quantity> max_units_floor;
  BreachRisk breach;
  std::optional<Money> fee;  ///< Full remaining size's fees; empty without a projection.
  std::optional<FillFees> fees;  ///< Itemized schedule only.
  PreviewExecution execution;
  std::vector<LegLiquidity> liquidity;  ///< Each leg's quote, in leg order.
  std::optional<WalkStep> next_walk = {};
};

/// An account as a what-if projects it, measured from today's equity.
struct WhatIfAccount {
  bool projected = true;             ///< False when an order could not be projected: no contract or quote.
  Money equity;
  Money buying_power;                ///< Available.
  std::optional<Exposure> exposure;  ///< The book's Greeks; empty while risk is incomplete.
  /// The worst loss on the configured spot × volatility grid from today's equity,
  /// so what a candidate costs to trade counts; empty without complete marks.
  std::optional<Money> max_loss;
  std::optional<Money> equity_at_max_loss;
  std::optional<bool> breaches_floor;
  std::optional<bool> breaches_soft_floor;
  ScenarioGrid scenarios;            ///< Each cell's P&L from today's equity.
  BreachRisk breach;
};
struct StockPreview {
  Decision decision;
  std::optional<Money> price;
  WhatIfAccount current;
  WhatIfAccount after;
};
struct WhatIfCandidate {
  Decision decision;            ///< The first order's refusal, or success.
  std::vector<Decision> orders; ///< Each order's checks, after the ones before it filled.
  WhatIfAccount after;          ///< Every order filled in full at its projected price.
};
struct WhatIf {
  WhatIfAccount current;
  std::vector<WhatIfCandidate> candidates;
};
/// What a flatten would do now, as close_positions would do it on a private copy.
struct FlattenPricing {
  bool limit = false;
  Quantity limit_ticks = 0;  ///< 0-10 adverse ticks from each leg's displayed touch.
};
struct FlattenPreview {
  Decision decision;                ///< The refusal when nothing in scope can close.
  std::vector<OrderId> cancelled;   ///< Open orders it would cancel.
  std::vector<Order> orders;        ///< Its closing orders as the current quotes would leave them.
  std::vector<Fill> fills;          ///< What they would fill now.
  std::vector<StockFill> stock_fills;
  std::map<std::string, Decision> kept_stocks;
  /// What would still be held in scope: contracts (OSI) and shares (underlying), signed.
  std::map<std::string, Quantity> remaining;
  std::map<std::string, Quantity> remaining_shares;
  WhatIfAccount current;
  WhatIfAccount after;
};

/// New terms for an open order; each field left empty keeps its value.
struct OrderChange {
  std::optional<Quantity> quantity;    ///< The total, filled contracts (or units) included.
  std::optional<Money> limit_price;    ///< Limit orders only; a multi-leg order's net per unit.
  std::optional<Money> trigger_level;  ///< Armed orders with a trigger only.
  /// DAY or GTC, for resting limit orders: a DAY order then ends with this session.
  std::optional<TimeInForce> tif = {};
  std::optional<std::optional<Walk>> walk = {};
  [[nodiscard]] bool empty() const { return !quantity && !limit_price && !trigger_level && !tif && !walk; }
};

/// Single-threaded, deterministic reducer. Every timestamp is caller market
/// time (UTC nanoseconds), monotone and nonnegative. No networking or clocks.
/// Business failures return Decisions; invalid commands/arithmetic/journal
/// failures throw TradingError. A journal failure latches a stop on all commands.
/// Snapshots are immutable owned values, valid after subsequent commands.
class TradingSession {
 public:
  TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal = {}, std::string actor = "system");
  ~TradingSession();
  /// Owner-thread command context; reset to system after applying a command.
  void set_actor(std::string actor);
  /// Re-execute older run drivers with their recorded bounded margin allocator.
  void set_exact_margin(bool exact);
  /// Owner-thread fill context. Null retains legacy acceptance-only behavior.
  /// This supplies values, never a callback; checks journal the values they use.
  /// Full records are retained only when reproducing runs predating compact inputs.
  void set_opening_rule_inputs(std::optional<OpeningRuleInputs> inputs, bool compact = true);
  /// Journal a driver input without introducing a clock or an external dependency.
  void record_input(std::string_view input, Timestamp time);
  TradingSession(TradingSession&&) noexcept;
  TradingSession& operator=(TradingSession&&) noexcept;
  TradingSession(const TradingSession&) = delete;
  TradingSession& operator=(const TradingSession&) = delete;

  /// Caller must resolve from a known definition. Conflicting OSI terms reject.
  CommandResult define(const md::OptionContract& contract, Timestamp time);
  /// The integration may reject a resolved but unsupported definition before
  /// registration; it still consumes the client ID and journals a rejected order.
  CommandResult submit(OrderRequest request, Timestamp time, Decision rejection = {});
  /// Pure pre-trade check and full-size projection; never takes displayed size,
  /// allocates IDs or writes a journal. floor_share is in (0, 1], default 0.5.
  [[nodiscard]] OrderPreview preview(const OrderRequest& request, Timestamp time, double floor_share = 0.5,
      Decision rejection = {}, const std::map<std::string, double>& close_variances = {}, const PreviewMarket& market = {}) const;
  /// The same preview of a change to open order `id`, as modify would make it: the
  /// change's decision and what it executes at once, and the order on its new terms
  /// projected as a new order is, its remaining units filling in full. Sizing counts
  /// the units it could still work (its filled ones aside); exits have none.
  [[nodiscard]] OrderPreview preview_change(OrderId id, const OrderChange& change, Timestamp time, double floor_share = 0.5,
      Decision rejection = {}, const std::map<std::string, double>& close_variances = {}, const PreviewMarket& market = {}) const;
  /// Candidate adjustments side by side, each a list of orders checked as
  /// submission checks them and filled in full at the preview's projected prices
  /// (slipped far sides for market orders, the limit for limit orders), each after
  /// the ones before it, on a private copy of the account; beside it the account as
  /// it is. Allocates no IDs and writes nothing. `rejections` holds the
  /// integration's gate for each candidate's orders, as for submit.
  [[nodiscard]] WhatIf what_if(const std::vector<std::vector<OrderRequest>>& candidates, Timestamp time,
      const std::vector<std::vector<Decision>>& rejections = {}, const std::map<std::string, double>& close_variances = {},
      const PreviewMarket& market = {}) const;
  /// A flatten's dry run: close_positions on a private copy of the account with the
  /// integration's newer market, the account before and after it beside its orders.
  /// Allocates no IDs and writes nothing.
  [[nodiscard]] FlattenPreview preview_close_positions(std::optional<std::string> underlying, Timestamp time,
      const std::map<std::string, Decision>& rejections = {}, const std::map<std::string, double>& close_variances = {},
      const PreviewMarket& market = {}, FlattenPricing pricing = {}) const;
  /// `close_variances`: each underlying's implied variance of its log price to today's close.
  [[nodiscard]] BreachRisk breach(const std::map<std::string, double>& close_variances = {}) const;
  /// The held book's warnings, most urgent first: a bucket over its delta or vega
  /// limit (DELTA_LIMIT, VEGA_LIMIT: actual and limit are the exposure and its
  /// limit); an underlying within max(1%, one standard deviation to today's
  /// close) of a move that takes its delta to its own or the account's limit, to
  /// first order in gamma (DELTA_HEADROOM: the signed percent move and that
  /// threshold); equity at or below the soft floor now (SOFT_FLOOR) or at
  /// rollover, when pending guardrails apply and a percent soft floor follows a
  /// ratcheted plan floor (SOFT_FLOOR_ROLLOVER: equity and the floor); an
  /// end-of-day floor that tonight's close at today's equity would raise
  /// (FLOOR_RATCHET: the floor tomorrow and today); American equity and ETF
  /// options expiring today a cent or more in the money, which deliver shares
  /// (EXPIRY_DELIVERY: buying power once every one of them has delivered, and
  /// zero; info before the account's pre-expiry cutoff, which closes at market,
  /// leaving delivery only if a close cannot fill); short ones the simulator may
  /// assign early, trading below their
  /// exercise value or with less time value than a dividend going ex within a week
  /// (EARLY_ASSIGNMENT: the mark and intrinsic value, or the time value and the
  /// dividend); and a held underlying's ex-date within a week (EX_DIVIDEND: the
  /// dividend a share). `dividends` is the calendar the rollover pays from.
  [[nodiscard]] std::vector<RiskWarning> warnings(const std::map<std::string, double>& close_variances = {},
                                                  const std::vector<Dividend>& dividends = {}) const;
  CommandResult cancel(OrderId id, Timestamp time, Reason reason = Reason::USER_CANCEL);
  /// Change a resting order in place: a DAY limit order, an armed order or a
  /// bracket exit. Its ID, fills and place among equal prices stay. The new
  /// terms pass every check a new order takes, its own reservation released
  /// first; a failure leaves the order unchanged. A limit that becomes
  /// marketable trades at once, and an armed order whose new level is already
  /// reached activates in the regular session. The quantity must stay above
  /// the filled quantity. Bracket exits change only their level or price:
  /// their size follows the position. `rejection` is the integration's
  /// acceptance gate, as for submit.
  CommandResult modify(OrderId id, OrderChange change, Timestamp time, Decision rejection = {});
  /// Cancel every open order, or every open order on one underlying.
  CommandResult cancel_all(std::optional<std::string> underlying, Timestamp time);
  /// Cancel the listed orders together, such as both exits of a pair: those still
  /// open are cancelled in one transaction. An unknown ID changes nothing
  /// (UNKNOWN_ORDER), nor does a list with none still open (ORDER_TERMINAL).
  CommandResult cancel_orders(const std::vector<OrderId>& ids, Timestamp time);
  /// Flatten the account, or one underlying: cancel the open orders in scope but
  /// the bracket exits, then close each unexpired position in scope with
  /// reduce-only market DAY orders under the account's fill model. Limit pricing
  /// instead uses EXTO and follows each fresh touch by 0-10 adverse ticks; manual
  /// price changes turn off repricing. It works in overnight and curb too. A short and
  /// the long that covers it (as for `defined_risk`) close together as one
  /// two-leg order, so no short is ever left naked; the other shorts and longs
  /// close alone, and each close is split at `max_order_contracts`. The closes
  /// work on later quotes until they fill, the session ends, they are cancelled
  /// or the position closes otherwise; they shrink with the position like the
  /// exits, which keep protecting whatever is still open and are cancelled once
  /// it is flat. Shares in scope close at their fresh price in the
  /// stock market's regular session. Expired positions wait for their settlement,
  /// and a long that covers an expired short waits with it. A close the account
  /// (an attempt no longer open), `rejections` (the integration's gate) or the
  /// session refuses is recorded as rejected and the others still go; missing or
  /// stale quotes only make a close wait. An underlying where every close is
  /// refused keeps its open orders; shares that cannot close are in
  /// `kept_stocks`, and positions left open in `residuals`. When nothing in scope
  /// can close, the decision is the first such refusal (scoped to its
  /// underlying) and nothing changes; otherwise it is success.
  CommandResult close_positions(std::optional<std::string> underlying, Timestamp time,
                                const std::map<std::string, Decision>& rejections = {}, FlattenPricing pricing = {});
  /// Apply a whole batch before risk/matching. Unknown symbols and future data
  /// reject the whole batch; duplicate/older observations are ignored. Supply
  /// at most one quote and one valuation per OSI per batch. An empty batch
  /// advances expiry, DAY cancellation, freshness and daily-loss monitoring.
  /// On an idle account (flat, no open orders, attempt started) an empty batch
  /// changes nothing but the clock unless a time rule acts or flat_time is set,
  /// so otherwise it is not journaled; the snapshot keeps its time until the next
  /// transaction. Flat-time plans publish clock progress and journal daily triggers.
  /// `stocks` prices the underlyings: the shares that exercise and assignment
  /// deliver are marked and traded at them. `indicators` are the prices and
  /// studies conditional triggers watch; older ones than those kept are ignored.
  CommandResult on_quotes(const std::vector<QuoteObservation>& quotes,
                          const std::vector<Valuation>& valuations, Timestamp time,
                          const std::vector<StockPrice>& stocks = {},
                          const std::vector<Indicator>& indicators = {});
  CommandResult set_limits(Limits limits, Timestamp time);
  CommandResult set_guardrails(Guardrails guardrails, Timestamp time);
  CommandResult trip_kill(std::string reason, Timestamp time);
  CommandResult reset_kill(std::string reason, Timestamp time);
  /// Explicit authoritative reference, including AM imports. Only after expiry.
  CommandResult settle(const std::string& symbol, Money settlement, Timestamp time,
                       std::optional<SettlementSource> source = {});
  /// Keeps an underlying's closing print for a date in the journal, so PM
  /// settlement uses it after a restart. A different price for the date replaces
  /// it, as an official close replaces a provisional print; positions already
  /// settled keep what they settled on.
  CommandResult record_close(const std::string& underlying, md::Date date, Money price, Timestamp print_time, Timestamp time);
  [[nodiscard]] std::optional<ClosingPrint> closing_print(const std::string& underlying, md::Date date) const;
  /// Explicit baseline reset, once per later trading date of the plan
  /// (trading_date); requires full marks.
  /// Kill latch persists across rollover.
  /// `dividends` are those going ex on the new trading date (and any skipped while
  /// the server was down): each pays the shares held into it, once.
  CommandResult roll_day(Timestamp time, const std::vector<Dividend>& dividends = {});
  /// Start a new attempt: cancel working orders, close positions at their last
  /// mark as Reset closures (no fills, no fees), restore cash, clear the kill
  /// latch and apply the given rules. Order and fill history is kept.
  /// activated is a caller-verified prerequisite pass; archive_payouts is false only for old replay inputs.
  CommandResult reset_account(Money initial_cash, AccountRules rules, std::string reason, Timestamp time,
                              bool activated = false, bool archive_payouts = true, bool restore_scaled_rules = true);
  /// Withdraw a whole-cent amount from a funded account under its payout rules
  /// (see payout_quote). The withdrawal is not a loss: the day's baseline and
  /// an unlocked trailing peak move down with it. Resets the qualifying days.
  CommandResult request_payout(Money amount, Timestamp time);
  /// Note and tag a trade, named by its opening fill's ID (the `id` of the
  /// trades view): a note of at most 2,000 bytes and up to eight tags of 1 to
  /// 32 bytes without commas, lowercased and each kept once. Invalid text
  /// throws INVALID_NOTE; an unknown trade returns UNKNOWN_TRADE. An empty note
  /// without tags clears it. Allowed whatever the account's state or session.
  CommandResult annotate(std::uint64_t trade, std::string note, std::vector<std::string> tags, Timestamp time);
  /// The same for a share round trip, named by the stock fill that opened it; its
  /// note is kept under "s" and that fill's ID.
  CommandResult annotate_shares(std::uint64_t first_fill, std::string note, std::vector<std::string> tags, Timestamp time);
  /// Joins the trades of the listed open round trips, by their trades-view IDs, into
  /// one whole trade named by the oldest (`together`), or takes each listed round
  /// trip out of its trade into one of its own: legs entered one by one become one
  /// trade, and a leg can leave a roll. A trade it changes that still holds more
  /// than one entry restarts its whole-trade review now. Unknown round trips return
  /// UNKNOWN_TRADE; closed ones, mixed underlyings or too few round trips return
  /// INVALID_GROUP. Allowed whatever the account's state or session.
  CommandResult group_trades(std::vector<std::uint64_t> trades, bool together, Timestamp time);
  /// Plan and review for a New York date, each at most 2,000 bytes. Empty clears it.
  CommandResult annotate_day(md::Date day, std::string plan, std::string review, Timestamp time);
  /// Exercise `contracts` of a long, in-the-money American equity or ETF option
  /// before expiry: they close at intrinsic value against the underlying's fresh
  /// price, and 100 shares each are bought (calls) or sold (puts) at that price,
  /// which together cost the strike. Held into expiry, such options are exercised
  /// or assigned at settlement when a cent or more in the money.
  CommandResult exercise(const std::string& symbol, Quantity contracts, Timestamp time);
  /// Give up a long that cannot be sold: nobody bids for it (its fresh quote shows
  /// only an ask) or it has expired and waits for its settlement. It leaves the
  /// account at zero, without a fee, as an Abandon closure; its exits and the
  /// orders selling it are cancelled. Allowed under the kill switch and after a
  /// decision; it may not
  /// leave a defined-risk plan's short uncovered, and with the `buying_power` rule
  /// a short it covered must still fit.
  CommandResult abandon(const std::string& symbol, Timestamp time);
  /// Instruct that a long option held into expiry is not
  /// exercised, however far in the money (`do_not_exercise`), or withdraw that
  /// instruction. At settlement such a long expires worthless and delivers no
  /// shares or cash. The instruction ends with the position.
  CommandResult instruct_exercise(const std::string& symbol, bool do_not_exercise, Timestamp time);
  /// Open, add, reduce or reverse shares at a fresh underlying price in the stock
  /// regular session, without a fee. Opening takes account, exposure and buying-power
  /// checks; a reversal closes the old position and opens the excess. An optional
  /// feed price seeds an underlying not held yet. reduce_only preserves stock close.
  CommandResult trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time,
                            std::optional<StockPrice> price = {}, bool reduce_only = false);
  /// The same trade on a private copy, without recording anything.
  [[nodiscard]] StockPreview preview_trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time,
      std::optional<StockPrice> price = {}, Decision rejection = {}) const;
  /// Watch a value and announce, with an alert_fired event, when it reaches a
  /// level: see AlertSpec. Alerts are checked after each market batch and when
  /// created, so one whose condition already holds fires at once; a missing or
  /// stale value neither fires nor rearms one. Invalid terms throw INVALID_ALERT;
  /// a contract or leg not registered returns UNKNOWN_CONTRACT, and a 101st alert
  /// INVALID_ALERT. Allowed whatever the account's state or session.
  CommandResult create_alert(AlertSpec spec, Timestamp time);
  /// Delete an alert, fired or not; an unknown ID returns UNKNOWN_ALERT.
  CommandResult delete_alert(std::uint64_t id, Timestamp time);
  [[nodiscard]] std::shared_ptr<const TradingSnapshot> snapshot() const;

  /// Read-only integration context, owned by the reducer. The engine copies it
  /// into its publication; recovery therefore needs no parallel journal schema.
  [[nodiscard]] const SessionConfig& config() const;
  [[nodiscard]] const Contracts& contracts() const;
  [[nodiscard]] const Valuations& valuations() const;
  [[nodiscard]] OpeningOrder opening_order(const OrderRequest& request, Timestamp time, const PreviewMarket& market = {},
      const std::map<std::string, Quantity>& preceding = {}) const;
  /// Held positions only, on one underlying, at fresh marks/valuations. Unknown
  /// direction is null, never zero; no position is zero.
  [[nodiscard]] std::optional<double> held_dollar_delta(const std::string& underlying, Timestamp time) const;
  [[nodiscard]] std::vector<CounterExposure> counter_exposures(const std::string& account, Timestamp time,
      const std::vector<Valuation>& valuations = {}, const std::vector<StockPrice>& stocks = {}) const;
  [[nodiscard]] std::optional<QuoteObservation> quote(const std::string& symbol) const;
  /// The quotes whose displayed size this account's orders have taken some of.
  [[nodiscard]] std::map<std::string, SizeLeft> sizes_left() const;
  [[nodiscard]] md::Date trading_day() const;
  /// The plan's trading date at `time` (AccountRules::day_end_minutes): rollover
  /// moves trading_day() to it once it is later.
  [[nodiscard]] md::Date trading_date(Timestamp time) const;

  /// Rebuild recorded outcomes without matching/repricing. Optional resumed
  /// sink must have exactly the verified recovered head and sequence.
  static TradingSession recover(const JournalRecovery& recovery,
                                std::shared_ptr<Journal> journal = {});
  /// Rewrite a journal that recovers into `out`, an empty journal, in the
  /// current schema: the same transactions, times, types, events and
  /// decisions, each state recorded as a change from the one before, and no
  /// snapshots. A final schema 1 record stays as it is, since recovery
  /// completes that journal's evaluation from it. Every rewritten record is
  /// read back before it is written; returns the snapshot_json that `out`
  /// recovers to.
  static std::string compact(const JournalRecovery& recovery, Journal& out);
  /// The reverse: every transaction with its whole state and the snapshot
  /// derived from it (schema 2), for audit tools and builds from before schema 3.
  static void expand(const JournalRecovery& recovery, Journal& out);
  /// Stable diagnostic representation, also convenient for exact replay checks.
  [[nodiscard]] std::string snapshot_json() const;
 private:
  struct Impl;
  explicit TradingSession(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::trading
