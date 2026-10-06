#pragma once

#include <optional>
#include <string>
#include <vector>

#include "openport/trading/ledger.hpp"

namespace openport::trading {

enum class EvaluationStatus { Active, Passed, Failed };

/// One finished trading day of an attempt, recorded at rollover. Days follow the
/// plan's boundary (AccountRules::day_end_minutes; 17:00 New York time by default).
struct EvaluationDay {
  md::Date day;
  Money open_equity;   ///< Fully marked equity when the day began (its baseline).
  Money close_equity;  ///< Last fully marked equity observed on that date.
  Money peak;          ///< High-water mark after that day's ratchet.
  Money floor;         ///< Drawdown floor after that day's ratchet; zero without a rule.
  Money realised;      ///< Net realised P&L of the day (after fees).
  bool qualifying = false;  ///< Funded accounts: the day counted toward a payout.
  Attribution attribution;  ///< The day's P&L by Greek.
  std::optional<Money> low_equity;
  std::optional<Money> high_equity;
  Timestamp low_at = 0;
  Timestamp high_at = 0;
  /// Executions of the trader's own orders that day, counted while a rule reads them
  /// (counts_executions); zero otherwise.
  std::uint64_t executions = 0;
  Reason locked = Reason::NONE;  ///< The plan limit that locked the day, if one did.
};

/// A funded-account withdrawal. The account pays out `amount`; the trader keeps
/// `trader_share` of it.
struct Payout {
  std::uint64_t number = 0;
  Timestamp time = 0;
  md::Date day;   ///< Trading day in progress when requested; it and later days count toward the next payout.
  Money amount;
  Money trader_share;
  Money balance;  ///< Equity when requested, before the withdrawal.
};

struct SizeScale {
  md::Date day;
  Money old;
  Money size;
};
/// Recorded only for enabled account-size scaling. Rules retain the original
/// loss amounts here, so every resize rounds from the same original ratio.
struct SizeScalingProgress {
  Money original;
  Money original_max_drawdown;
  Money original_daily_loss_limit;
  md::Date period_started;
  std::uint64_t period_days = 0;
  Money period_balance;
  Money period_size;
  std::uint64_t payouts_at_start = 0;
  std::vector<SizeScale> history;
};

/// Best profitable closed whole option trade, net of fees.
struct BestTrade {
  std::string id;
  Money pnl;  ///< Positive net P&L of a fully closed whole option trade.
};
/// Capital growth changes live loss/lock amounts, not the purchased plan's identity.
[[nodiscard]] AccountRules original_program_rules(AccountRules rules, Money current_size,
    const std::optional<SizeScalingProgress>& scaling);

/// Rule progress for the current attempt. Fully marked equity (every position
/// has a mark, fresh or not) drives equity rules; time and overnight rules need no marks.
struct Evaluation {
  Money short_profit;  ///< Positive net closed short-round-trip P&L, only with microscalping on.
  std::uint64_t first_stock_fill = 1;  ///< Attempt boundary for microscalping shares; otherwise left at 1.
  std::uint64_t closed_trades = 0;  ///< Only counted with min_trades enabled.
  std::optional<BestTrade> best_trade;  ///< Kept only with trade consistency enabled.
  std::uint64_t attempt = 1;
  Timestamp started = 0;
  Money starting_balance;
  Money peak;   ///< High-water mark that the trailing floor follows.
  Money floor;  ///< peak - max_drawdown with a drawdown rule, otherwise zero.
  EvaluationStatus status = EvaluationStatus::Active;
  Timestamp decided_at = 0;
  Money decided_equity;
  std::string decision;           ///< Human-readable pass/fail explanation.
  OrderId first_order = 1;        ///< Orders with smaller IDs belong to earlier attempts.
  std::uint64_t first_fill = 1;   ///< Likewise for fills.
  md::Date day;                   ///< Trading day in progress.
  Money day_open_equity;
  Money day_close_equity;         ///< Latest fully marked equity observed on `day`.
  std::vector<EvaluationDay> days;  ///< Finished days of this attempt, oldest first.
  bool floor_locked = false;      ///< The floor reached the lock balance and stopped trailing.
  Money day_open_realised;        ///< Net realised P&L when `day` began.
  std::uint64_t qualifying_days = 0;  ///< Days closed since the last payout (or the start) that qualified.
  Timestamp cycle_started = 0;
  std::vector<Payout> payouts;
  std::optional<Money> day_low_equity;
  std::optional<Money> day_high_equity;
  Timestamp day_low_at = 0;
  Timestamp day_high_at = 0;
  std::optional<Money> closest_floor;
  Timestamp closest_floor_at = 0;
  /// What decided the attempt (PROFIT_TARGET, DRAWDOWN_FLOOR, DAILY_LOSS_LIMIT, ...);
  /// NONE while active. Journals leave out the code a status implies (PROFIT_TARGET
  /// for a pass, DRAWDOWN_FLOOR for a failure), which older decisions also imply.
  Reason decision_code = Reason::NONE;
  /// A plan limit that closed the positions and locks the account against opening
  /// orders until the next trading day (DAILY_LOSS_LIMIT), and when it did.
  Reason day_lock = Reason::NONE;
  Timestamp day_locked_at = 0;
  std::uint64_t day_executions = 0;  ///< Today's, as EvaluationDay::executions.
  Timestamp last_activity = 0;  ///< Own execution, only when inactivity is set; zero uses started.
  std::optional<md::Date> flat_time_day;  ///< Last date whose mandatory flatten started.
  bool flat_pending = false;  ///< Retry unfilled mandatory closes, including after rollover.
  std::optional<SizeScalingProgress> size_scaling;
  std::int64_t scaling_limit = 0;  ///< In force this session; zero without scaling.
  std::vector<std::string> holding_violations;  ///< Boundaries crossed while still holding, decided at rollover.
  std::vector<std::string> event_actions;  ///< Stable keys of once-only calendar actions.
  Timestamp event_checked = 0;  ///< Last transaction checked for holding cutoffs.
};

/// The highest reached step, or the first step below zero profit; zero without scaling.
[[nodiscard]] std::int64_t scaling_limit(const AccountRules& rules, Money profit);

/// Exact period profit, adding back withdrawals made during the review.
[[nodiscard]] Money size_scaling_profit(const Evaluation& evaluation, Money balance);
[[nodiscard]] Money size_scaling_required(const Evaluation& evaluation, const SizeScaling& rule);
[[nodiscard]] Money size_scaling_increase(const Evaluation& evaluation, const SizeScaling& rule);

/// Shared plan arithmetic. Observations check the floor before the target; rollover
/// ratchets an end-of-day floor from the last fully marked close. The peak follows
/// the drawdown mode even without rules, which then never decide; a static plan's
/// peak follows every high like an intraday one's, though its floor stays put.
/// The level the trailing floor locks at: the starting balance with lock_at_start,
/// otherwise lock_balance; zero for none.
[[nodiscard]] Money lock_level(const AccountRules& rules, Money starting_balance);
/// peak - max_drawdown, capped by the lock level (sticky once reached); a static
/// floor is the starting balance less the drawdown, locked from the start.
[[nodiscard]] Money evaluation_floor(const AccountRules& rules, Money peak, bool& locked, Money starting_balance);
/// The equity part of the rules alone: ratchets an intraday peak, then fails on the
/// floor and passes on an equity target. evaluate_plan adds the rest.
[[nodiscard]] EvaluationStatus evaluate_equity(Evaluation& evaluation, const AccountRules& rules, Money equity);
void evaluation_rollover(Evaluation& evaluation, const AccountRules& rules);
/// The floor rollover would leave if the trading day closed at `equity`: an end-of-day
/// ratchet while the attempt is active, otherwise (decided, locked, intraday) the floor.
[[nodiscard]] Money evaluation_tomorrow_floor(const Evaluation& evaluation, const AccountRules& rules, Money equity);
/// The plan's trading date at `time`: its day ends at AccountRules::day_end_minutes.
[[nodiscard]] md::Date plan_trading_date(const AccountRules& rules, Timestamp time);

/// What the plan's rules read of the account at one observation.
struct PlanInputs {
  Money equity;        ///< Fully marked equity.
  Money balance;       ///< Closed balance: cash plus the positions' cost, open P&L left out.
  Money net_realised;  ///< The ledger's realised P&L less fees (Evaluation::day_open_realised's measure).
  bool flat = true;    ///< No option positions and no shares.
  Timestamp time = 0;  ///< Observation market time; zero skips calendar checks in timeless callers.
};
/// Whether the rules read each day's executions, so the reducer counts them
/// (accounts without such a rule keep their journal unchanged).
[[nodiscard]] bool counts_executions(const AccountRules& rules);
/// A finished day's profit under the plan's basis: its equity change, or its net
/// realised P&L (closed balance).
[[nodiscard]] Money day_profit(const EvaluationDay& day, const AccountRules& rules);
/// The day in progress's profit so far under the plan's basis.
[[nodiscard]] Money today_profit(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now);
/// The attempt's profit so far under the plan's basis, counting payouts withdrawn.
[[nodiscard]] Money attempt_profit(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now);
/// The day in progress's opening closed balance: today's realised P&L taken off
/// the balance, so a payout today moves it down as it moves the opening equity.
[[nodiscard]] Money day_open_balance(const Evaluation& evaluation, const PlanInputs& now);

/// The attempt's days as the objectives count them: the finished days and the one in
/// progress, which counts as soon as it qualifies.
struct DayStats {
  std::uint64_t days = 0;             ///< Days of the attempt, today included.
  std::uint64_t trading_days = 0;     ///< With an execution (counted only under counts_executions).
  std::uint64_t profitable_days = 0;  ///< Profit at least profitable_day_profit, and above zero.
  std::optional<Money> best_day;      ///< The largest day profit, when one is above zero.
  md::Date best_day_date;
  Money positive_total;               ///< The profitable days' profits added up.
};
[[nodiscard]] DayStats day_stats(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now);

/// One condition a pass needs: dollars for the target, counts for day/trade
/// minimums, percentages for day/trade consistency and microscalping.
struct Objective {
  Reason code = Reason::NONE;  ///< The target, minimum count, or consistency objective.
  bool met = false;
  std::optional<double> actual;  ///< Absent for consistency while the basis is not above zero.
  double required = 0;
  std::string message;           ///< Where it stands, in a trader's words.
};
/// The pass conditions in force, as they stand: an attempt passes on the first
/// observation that meets them all. A target on the closed balance also needs
/// the account flat, so the balance is what the trader actually closed.
[[nodiscard]] std::vector<Objective> evaluation_objectives(const Evaluation& evaluation, const AccountRules& rules,
                                                           const PlanInputs& now);
/// With a consistency rule, the profit the best day so far requires: the target
/// rises to best day × 100 / percent when that is more (whole cents, rounded up).
[[nodiscard]] std::optional<Money> consistency_target(const Evaluation& evaluation, const AccountRules& rules,
                                                      const PlanInputs& now);

/// The daily loss limit as it stands: the basis it is measured from and the
/// equity level that reaches it (touching it counts).
struct DailyLossLevel {
  Money reference;
  Money level;
};
[[nodiscard]] std::optional<DailyLossLevel> daily_loss_level(const Evaluation& evaluation, const AccountRules& rules,
                                                             const PlanInputs& now);

/// What one observation decides: a pass or failure (status), or a lock of the
/// trading day (lock), with the reason code, the level reached and a message.
struct PlanVerdict {
  EvaluationStatus status = EvaluationStatus::Active;
  bool lock = false;
  Reason code = Reason::NONE;
  Money level;
  std::string message;
  [[nodiscard]] bool decided() const { return status != EvaluationStatus::Active; }
};
/// The plan's rules on one fully marked observation of an active attempt, in
/// order: fail overdue time rules; ratchet an intraday peak; fail on the floor;
/// lock or fail on the daily loss limit; pass once every objective is met. The caller
/// applies the verdict (the reducer decides, or locks and liquidates).
[[nodiscard]] PlanVerdict evaluate_plan(Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now);
/// Time-only failures, checked before executions even when marks are missing.
[[nodiscard]] PlanVerdict evaluate_time_rules(const Evaluation& evaluation, const AccountRules& rules, Timestamp time);
/// Last allowed trading dates and nonnegative calendar-day counts; absent while off or not started.
struct TimeRuleProgress {
  std::optional<md::Date> deadline;
  std::optional<std::int64_t> days_left;
  Timestamp last_activity = 0;
  std::optional<std::int64_t> inactive_days;
  std::optional<md::Date> inactivity_deadline;
};
[[nodiscard]] TimeRuleProgress time_rule_progress(const Evaluation& evaluation, const AccountRules& rules, Timestamp time);
/// Whether today's flat time has passed and the plan day has not ended.
[[nodiscard]] bool plan_flat_now(const AccountRules& rules, Timestamp time);
/// Opening restrictions, bypassed by the caller for reductions and system orders.
[[nodiscard]] Decision plan_entry_check(const AccountRules& rules, std::string_view underlying, Timestamp time);
/// Why opening orders are refused while the day is locked.
[[nodiscard]] std::string day_lock_message(Reason lock);

struct PassOdds {
  double pass = 0, fail = 0, neither = 0;
  std::optional<double> median_days_to_pass;
  std::size_t historical_days = 0;
  std::uint64_t seed = 0;
};
/// Circular blocks of three consecutive completed days. Low/high order follows
/// their recorded timestamps, widened to cover each day's open and close. Older
/// days without recorded extrema are left out, not fabricated.
[[nodiscard]] PassOdds pass_odds(const Evaluation& current, const AccountRules& rules,
    Money equity, const std::vector<EvaluationDay>& history, int days, int samples, std::uint64_t seed);

/// The fee charged when an attempt starts; zero fees have no kind or journal field.
struct AttemptFee {
  Money amount;
  std::string kind;
  bool operator==(const AttemptFee&) const = default;
};

/// An earlier attempt, summarised when the account is reset.
struct AttemptSummary {
  std::uint64_t attempt = 0;
  std::string plan;
  Timestamp started = 0;
  Timestamp ended = 0;
  Money starting_balance;
  Money final_equity;  ///< Last-mark estimate at reset.
  EvaluationStatus status = EvaluationStatus::Active;
  std::string decision;
  OrderId first_order = 1;
  std::uint64_t first_fill = 1;
  Reason decision_code = Reason::NONE;  ///< As Evaluation::decision_code.
  std::optional<AccountRules> rules{};  ///< Rules at reset; absent in older summaries.
  Timestamp decided_at = 0;
  std::optional<Money> decided_equity{};
  std::optional<Money> peak{};
  std::optional<Money> floor{};
  AttemptFee fee_charged{};
  Money payouts_received{};
  std::optional<SizeScalingProgress> size_scaling{}; ///< Original plan terms for reset counting after growth.
};

/// Exercise: contracts exercised early into shares, at intrinsic value.
/// Assignment: a short American equity or ETF option assigned early, overnight.
/// Abandon: a long nobody bids for, given up at zero without a fee.
enum class ClosureKind { Settlement, Reset, Exercise, Assignment, Abandon };

/// A position that left the ledger without a fill, so trade history can close it.
struct Closure {
  std::string symbol;
  Quantity quantity = 0;  ///< Signed position that was closed.
  Money price;            ///< Per unit: settlement intrinsic, or the last mark at reset.
  Timestamp time = 0;
  ClosureKind kind = ClosureKind::Settlement;
  std::uint64_t after_fill = 0;  ///< Fills recorded before the closure, for ordering.
  Money fee{};  ///< Total exercise, assignment or delivery fee.
};

/// How shares changed hands: delivered by an option at settlement, delivered by
/// an early exercise, traded to reduce them, closed by the account when an
/// evaluation is decided, dropped at their mark by an account reset, or
/// delivered by a short option's early assignment.
enum class StockSource { Delivery, Exercise, Trade, Rule, Reset, Assignment };

/// An underlying's closing print for a date: what its PM-settled options settle on.
struct ClosingPrint {
  Money price;
  Timestamp time = 0;  ///< When the print was stamped.
};

/// A dividend paid on (or, by short shares, charged to) the shares held into its ex-date.
struct DividendPayment {
  std::string symbol;
  md::Date ex_date;
  Money per_share;
  Quantity shares = 0;  ///< Signed shares held into the ex-date.
  Money amount;         ///< per_share * shares: negative when short shares pay it.
  Timestamp time = 0;
  /// Stock fills recorded before the payment, for ordering; empty on payments
  /// recorded before it was kept.
  std::optional<std::uint64_t> after_stock_fill;
};

/// One change in the shares an account holds, so trade history can follow them.
struct StockFill {
  std::uint64_t id = 0;  ///< 1, 2, ...: its place in the account's stock fills.
  std::string symbol;    ///< The underlying, e.g. "SPY".
  Quantity shares = 0;   ///< Signed: bought positive.
  Money price;
  Timestamp time = 0;
  StockSource source = StockSource::Trade;
  std::string option;  ///< The OSI that delivered them, for Delivery and Exercise.
};

/// Buying power. Under strategy margin it is cash: long premium is paid in full and
/// shorts hold a requirement. Under portfolio margin it is equity (cash and the
/// positions at their marks) less the requirement, so longs and shares are collateral.
/// Working orders reserve their worst-case use.
struct BuyingPower {
  Money available;          ///< cash - short_requirement - reserved; may be negative.
  Money reserved;
  Money short_requirement;  ///< The margin requirement: in portfolio mode, the portfolio's whole.
};

/// Per-contract naked short requirement excluding premium: 100 * max(20% of spot
/// less the out-of-the-money amount, 10% of spot for calls or of strike for puts).
/// Without a valid spot the strike stands in for it.
[[nodiscard]] Money naked_requirement(const md::OptionContract& contract, std::optional<double> spot);

/// An option position for margin: one entry per contract.
struct MarginLeg {
  md::OptionContract contract;
  Quantity quantity = 0;         ///< Signed contracts.
  Money value;                   ///< Shorts: buy-back value of all the contracts.
  std::optional<double> spot;    ///< For the naked rule.
};
/// Shares for strategy margin: one underlying's signed shares and their value.
struct MarginStock {
  std::string underlying;
  Quantity shares = 0;  ///< Signed.
  Money value;          ///< What they are worth, positive: at their mark, or their basis without one.
};
/// What the account type and the broker add to margin (AccountRules).
struct MarginPolicy {
  AccountType account = AccountType::Margin;
  std::int64_t house_percent = 0;  ///< On top of the naked and short-sale requirements, or of the scan.
  std::int64_t vol_shock = 0;      ///< Portfolio margin's implied-volatility shock, in points.
  bool exact = true;             ///< False only when re-executing older run drivers.
};
[[nodiscard]] MarginPolicy margin_policy(const AccountRules& rules);
/// Requirement for option positions and shares. Shorts pair with longs of the
/// same type on the same underlying that expire with them or later, as verticals:
/// a put long below or a call long above its short costs the width, one at or
/// beyond it nothing, and no pair costs more than naked; unpaired shorts are
/// naked at their buy-back value plus naked_requirement. Every 100 shares are a
/// cover too, whatever the option expires: long shares make a short call
/// covered, for nothing more, short shares make a short put covered for its
/// buy-back value, and a long call caps 100 short shares' requirement at its
/// strike. Verticals, share covers, straddles and expiry pools are allocated jointly.
/// Pools use same-expiry shorts and same-or-later longs, with no net short calls.
/// Whole-contract primal allocations and exact rational dual bounds certify the
/// integer minimum. Branching handles fractional LP allocations. A deterministic
/// work/arithmetic cap falls back to the older bounded search; margin_breakdown
/// reports exact, bounded or legacy. See docs/margin-allocation.md for the model
/// and proof. Older run drivers retain their recorded allocator.
/// Longs need nothing: their premium is paid in full, as long shares are; short
/// shares hold their value and half again.
/// A house percentage raises each naked requirement (beyond the buy-back value)
/// and each short sale's margin (beyond the shares' value) by that much. A cash
/// account or an IRA secures each unpaired short put with its strike in cash,
/// pairs no straddles, and a cash account nets no spreads either: its shorts
/// pair only with shares.
[[nodiscard]] Money margin_requirement(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks = {},
    const MarginPolicy& policy = {});
enum class MarginPartKind {
  Naked,            ///< A short option alone: its buy-back value plus its naked requirement.
  Vertical,         ///< A short with the long of its type that covers it: the width, at most naked.
  Covered,          ///< A short call against 100 long shares (nothing), or a short put against 100 short shares (its buy-back value).
  Straddle,         ///< A short put with a short call: the greater naked requirement plus the other's buy-back value.
  ShortShares,      ///< Short shares: their value and half again.
  ProtectedShares,  ///< 100 short shares with a long call: at most its strike.
  WorstLoss,        ///< Same-expiry shorts with same-or-later longs, held at their worst loss at the shorts' expiry.
  Long,             ///< A long option or long shares that nothing else needs: paid in full.
  CashSecured,      ///< A cash account's or IRA's short put alone: its strike, in cash.
};
/// One part of an underlying's requirement: which positions hold what.
struct MarginPart {
  MarginPartKind kind = MarginPartKind::Naked;
  /// OSI symbols, or the underlying for shares, with the signed contracts or
  /// shares the part takes; each symbol appears once, in first-appearance order.
  /// A position can be split between parts.
  std::vector<std::pair<std::string, Quantity>> legs;
  Money requirement;
};
/// Where a portfolio-margin scan loses most, and the minimum under it.
struct PortfolioScan {
  Money loss;  ///< The largest loss over the scan points; zero when none loses.
  double spot_percent = 0;
  double vol_points = 0;
  Money minimum;  ///< $0.375 times the multiplier for every option contract.
};
/// One underlying's requirement and what makes it up: strategy margin's parts,
/// or a portfolio-margin scan.
struct MarginUnderlying {
  std::string underlying;
  Money requirement;
  std::vector<MarginPart> parts;
  std::optional<PortfolioScan> scan;
  std::string allocation = "exact"; ///< exact, bounded (work cap), or legacy; empty for portfolio scans.
};
/// margin_requirement by underlying, with the parts that hold it: every position
/// is in one or more of them, and they add up to it.
[[nodiscard]] std::vector<MarginUnderlying> margin_breakdown(const std::vector<MarginLeg>& legs,
    const std::vector<MarginStock>& stocks = {}, const MarginPolicy& policy = {});
/// Portfolio margin (Cboe Rule 12.4, FINRA Rule 4210(g)): per underlying, the
/// largest Black-76 loss over 11 evenly spaced price shocks, -8% to +6% for index
/// products and -15% to +15% otherwise, or $0.375 times the multiplier for every
/// option contract if that is larger. Shares move linearly. With a vol shock,
/// each price shock is also taken with implied volatility that many points up
/// and down; a house percentage raises each underlying's requirement by that
/// much. No result if the scan lacks fresh valuations or share prices.
[[nodiscard]] std::optional<Money> portfolio_margin_requirement(const std::vector<MarginLeg>& legs,
    const Valuations& valuations, Timestamp now, Timestamp max_age,
    const std::map<std::string, StockPosition>& stocks = {}, const std::map<std::string, double>& stock_prices = {},
    const MarginPolicy& policy = {});
/// portfolio_margin_requirement by underlying, each with its scan.
[[nodiscard]] std::optional<std::vector<MarginUnderlying>> portfolio_margin_breakdown(const std::vector<MarginLeg>& legs,
    const Valuations& valuations, Timestamp now, Timestamp max_age,
    const std::map<std::string, StockPosition>& stocks = {}, const std::map<std::string, double>& stock_prices = {},
    const MarginPolicy& policy = {});
/// Short contracts that no long covers: each short pairs with a long of the same
/// type on the same underlying that expires with it or later, whatever the
/// strikes, as a defined-risk rule counts it.
[[nodiscard]] Quantity naked_shorts(const std::vector<MarginLeg>& legs);
/// What an account type cannot hold: short calls no 100 long shares cover (or,
/// in an IRA, no long call either, as naked_shorts pairs them), and each started
/// 100 short shares. Nothing under a margin account.
[[nodiscard]] Quantity disallowed_shorts(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks,
    AccountType account);

}  // namespace openport::trading
