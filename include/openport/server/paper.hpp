#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "openport/trading/session.hpp"
#include "openport/analytics/chain_analytics.hpp"
#include "openport/md/bars.hpp"
#include "openport/server/equity.hpp"

namespace openport::server {

/// A market-wide trading halt from the S&P 500's circuit breakers (NYSE Rule 7.12,
/// which the options exchanges follow).
struct MarketHalt {
  int level = 0;
  md::Timestamp start = 0;
  md::Timestamp end = 0;   ///< when trading resumes
  double reference = 0.0;  ///< the previous close the fall is measured from
  double price = 0.0;      ///< the print that tripped it
};

/// Published with engine status, independently of the selected paper account.
struct CircuitBreakerStatus {
  struct Close {
    md::Date date;
    double price = 0.0;
    bool operator==(const Close&) const = default;
  };
  std::string symbol;
  md::Date day;
  std::optional<Close> previous_close;
  int level = 0;
  std::vector<MarketHalt> halts;
  md::Timestamp market_time = 0;
  bool active = false;
  std::string error;
};

/// The halt a print of the S&P 500 trips, if any. In the regular session a fall of 7%
/// (level 1) or 13% (level 2) from the previous close halts trading for 15 minutes,
/// each once a day and only until 35 minutes before the close (15:25 ET); a fall of
/// 20% (level 3) halts it for the rest of the day. `tripped` is the highest level
/// already tripped that day.
[[nodiscard]] std::optional<MarketHalt> circuit_breaker(double reference, double price, md::Timestamp time,
                                                        int tripped);

/// Shared session/feed gate for new orders and status/ticks: open while the
/// underlying's options are in any session (regular, overnight or curb; the
/// reducer decides which orders each takes) and no market-wide halt covers the
/// market time. Contract, risk and write-access checks remain separate. Never
/// advances the reducer's data clock.
[[nodiscard]] trading::Decision paper_acceptance(std::string_view underlying,
    md::Timestamp market_time, md::Timestamp wall_time, std::chrono::seconds delay,
    md::Timestamp max_quote_age, const std::vector<MarketHalt>& halts = {});

/// A live demo restart may only capture an AM opening print through 09:35 ET.
inline constexpr md::Timestamp kOpeningPrintWait = 5 * md::kNanosPerMinute;

/// How long after the regular close an expired PM position waits for its closing
/// print before the last print before the close may stand in; past it, a position
/// with neither settles by hand.
inline constexpr md::Timestamp kLastPrintWait = 30 * md::kNanosPerMinute;

struct TradingStatus {
  bool enabled = false;
  std::string reason = "PAPER_DISABLED";
  std::uint64_t account_version = 0;
  bool kill_latched = false;
  std::string write = "disabled";
  trading::Money fee_per_contract;
  trading::Money initial_cash;
  std::string plan;        ///< Active rules' display name; empty without a plan.
  std::string evaluation;  ///< active/passed/failed with a target or drawdown rule, else empty.
  std::string plan_id = {};  ///< Preset ID; empty for custom rules.
};

/// The replay run an account belongs to, so exports from several runs can be told
/// apart: its history id and what it replayed. Live accounts have none.
struct RunIdentity {
  std::string id;         ///< The run's id, as replay state and history name it.
  std::string scenario;   ///< The scenario's id; empty for a recording.
  std::string seed;       ///< The scenario's seed, in decimal; empty for a recording.
  std::string recording;  ///< The recording's file name; empty for a scenario.
  std::string date;       ///< The replayed session's New York date, YYYY-MM-DD.
};

struct AccountDamage {
  std::string reason;
  std::uint64_t last_good_seq = 0;
  md::Timestamp last_good_time = 0;
};
inline constexpr std::uint64_t kLargeJournalBytes = 256ull * 1024 * 1024;
inline constexpr std::uint64_t kLargeJournalRecords = 100'000;
[[nodiscard]] std::string journal_warning(std::uint64_t bytes, std::uint64_t records, bool replay);

/// The reducer snapshot and its pricing inputs are published together so HTTP
/// readers never combine portfolio state with a different risk frame.
struct TradingView {
  std::uint64_t journal_transactions = 0;
  std::string journal_head;
  std::uint64_t journal_bytes = 0;
  std::optional<AccountDamage> damaged;
  std::shared_ptr<const trading::TradingSnapshot> snapshot;
  trading::SessionConfig config;
  trading::Contracts contracts;
  trading::Valuations valuations;
  /// Per-underlying data clocks, seeded from persisted quotes on recovery.
  std::map<std::string, md::Timestamp> market_times;
  /// What this account's orders have left of the quotes still current, where they took some.
  std::map<std::string, trading::SizeLeft> sizes_left;
  /// Circuit-breaker halts of the last day.
  std::vector<MarketHalt> halts;
  trading::BreachRisk breach;
  std::vector<trading::RiskWarning> warnings;
  std::vector<EquitySample> equity_samples;
  std::string equity_error;
  md::Timestamp equity_error_time = 0, equity_error_market_time = 0;
  bool equity_error_recovered = false;
  std::string playbooks_json;  ///< Immutable catalogue and transient stages for this account.
  bool opening_settlement = false;  ///< This source settles AM options on the opening print.
  std::optional<RunIdentity> run;  ///< The replay run, for replay accounts.
  md::Timestamp replay_start = 0, replay_end = 0;  ///< Risk-setting rollover horizon; zero for live accounts.
};

/// The market's implied variance of an underlying's log price from its analytics'
/// market time to that day's regular close: the nearest expiry's at-the-money IV²
/// times its years (as the IV was solved, on calendar time), with today's share of it
/// in regular-session time. Zero once today's session is over; empty without an IV.
[[nodiscard]] std::optional<double> implied_variance_to_close(const analytics::UnderlyingMetrics& metrics);
/// The market's implied variance of an underlying's log price from its analytics'
/// market time to `target`. Before today's close it is implied_variance_to_close's,
/// in proportion to the regular-session time to `target`; later, the at-the-money
/// term structure's total variance interpolated linearly in calendar time (flat
/// volatility outside the listed expiries), never less than the variance to today's
/// close. Zero for a target already past; empty without an IV.
[[nodiscard]] std::optional<double> implied_variance_until(const analytics::UnderlyingMetrics& metrics, md::Timestamp target);
/// Where a probability horizon `days` ahead of `now` ends: today's regular close for
/// zero, otherwise `days` calendar days after `now`.
[[nodiscard]] md::Timestamp probability_horizon(md::Timestamp now, double days);

/// What the market's implied volatility says about where an underlying can be by a
/// date, with S the spot, v the variance of its log price to the date and Φ the
/// normal CDF: finishing above K has probability Φ((ln(S/K) - v/2)/√v), the
/// risk-neutral lognormal with the forward at spot; touching K at any time before
/// then 2Φ(-|ln(K/S)|/√v), the driftless reflection estimate; and a cone of k
/// standard deviations runs from S e^(-k√v) to S e^(k√v).
struct PriceOdds {
  double price = 0;
  double above = 0;
  double below = 0;
  double touch = 0;
};
[[nodiscard]] PriceOdds price_odds(double spot, double price, double variance);

/// An underlying's beta to a benchmark, from paired log returns: covariance over
/// the benchmark's variance. `source` says where it came from: "reference" (the
/// benchmark itself), "given" (the caller's), "index" (the same index, as SPY and
/// SPX are: 1), "daily" (completed daily closes, at most the last 252 returns, at
/// least 20) or "intraday" (5-minute closes within each session, at least 30).
/// Empty source: no beta.
struct BetaEstimate {
  double beta = 0;
  double correlation = analytics::kNaN;
  std::size_t observations = 0;
  std::string source;
};
/// Beta from bars matched by start time; consecutive bars of one series whose gap
/// exceeds `max_gap` (0 for any) are not paired into a return. A beta beyond +/-10
/// is no estimate (empty source), as risk profiles accept none.
[[nodiscard]] BetaEstimate estimate_beta(const std::vector<md::Bar>& asset, const std::vector<md::Bar>& benchmark,
                                         std::size_t max_returns, md::Timestamp max_gap = 0);
/// Whether two symbols follow the same index (SPX, SPXW, XSP and SPY; NDX, NDXP, XND
/// and QQQ; RUT, RUTW and IWM), so their beta to each other is 1.
[[nodiscard]] bool same_index(std::string_view a, std::string_view b);

/// The main account keeps the original journal; others are named alongside it.
inline constexpr std::string_view kMainAccount = "main";
/// Account display names: 1 to 64 characters (Unicode code points) of UTF-8 text,
/// none of them a control character (U+0000-U+001F, U+007F-U+009F).
[[nodiscard]] bool valid_account_name(std::string_view name);

/// One paper account's standing, for the account list and ticks.
struct AccountStatus {
  std::string id;
  std::string name;
  TradingStatus trading;
  std::int64_t sandbox_idle_seconds = 0;
  bool archived = false;
  std::optional<AccountDamage> damaged = {};
  std::uint64_t journal_bytes = 0, journal_records = 0;
  bool replay = false;
};

struct TradingCommand {
  /// Recorded runs keep a kind's number: new kinds append.
  enum class Kind { Submit, Cancel, Limits, Trip, Reset, Settle, ResetAccount, Payout, Modify, CancelAll, ClosePositions, CreateAccount, Annotate,
                    Exercise, CloseStock, DayNote, Preview, Guardrails, Playbook, CreateSandbox, PreviewChange, WhatIf, PreviewClose,
                    Abandon, ExerciseInstruction, Group, Ungroup, TradeStock, PreviewStock, CreateAlert, DeleteAlert,
                    UpdateAccount, DeleteAccount };
  Kind kind = Kind::Submit;
  std::string actor = "unknown";
  trading::OrderRequest order;
  trading::OrderId order_id = 0;
  trading::Reason cancel_reason = trading::Reason::USER_CANCEL;
  trading::Limits limits;
  trading::Guardrails guardrails;
  double floor_share = 0.5;
  std::uint64_t expected_revision = 0;
  std::string reason;
  std::string symbol;
  trading::Money settlement;
  trading::Money initial_cash;   ///< ResetAccount: the new starting balance.
  trading::AccountRules rules;   ///< ResetAccount: the new attempt's rules.
  /// ResetAccount: a plan name whose evaluation the current attempt must have
  /// passed (funded presets); empty for no requirement.
  std::string required_pass;
  std::uint64_t program_cost_overrides = 0; ///< Bits 0..3: evaluation/reset/activation fee, max_resets explicitly supplied.
  bool restore_scaled_rules = false; ///< Reset displayed scaled rules to original terms; absent on older commands.
  bool program_costs = false; ///< New reset semantics; absent on old recorded commands for exact re-execution.
  trading::Money amount;         ///< Payout: the withdrawal.
  trading::OrderChange change;   ///< Modify: the order's new terms.
  std::string underlying;        ///< CancelAll, ClosePositions and PreviewClose: one underlying, or empty for all.
  /// CancelAll: the orders to cancel together instead of an underlying's or all.
  std::vector<trading::OrderId> order_ids;
  std::string account;           ///< The account it acts on; empty for the main account.
  std::string name;              ///< CreateAccount: the new account's display name.
  std::optional<bool> archived;   ///< UpdateAccount: freeze or resume the account.
  std::string copy_settings_from = {};  ///< CreateAccount: copy active limits and guardrails.
  std::uint64_t trade = 0;        ///< Annotate: the trade, by its opening fill's ID.
  bool shares = false;            ///< Annotate: a share round trip, by its opening stock fill ("s" + ID).
  md::Date day;
  std::string plan;
  std::string review;
  std::string note;               ///< Annotate: the note; empty with no tags clears it.
  std::vector<std::string> tags;  ///< Annotate: the trade's tags.
  /// Group and Ungroup: the round trips, by trade ID, whose trades join or which leave theirs.
  std::vector<std::uint64_t> trades = {};
  /// Exercise: contracts of `symbol`; CloseStock: shares of `symbol` to close, 0 for all.
  trading::Quantity quantity = 0;
  /// TradeStock/PreviewStock: the feed price, captured before recording the command.
  std::optional<trading::StockPrice> stock_price;
  /// WhatIf: each candidate's orders, and its name. Never recorded.
  std::vector<std::vector<trading::OrderRequest>> candidates = {};
  std::vector<std::string> candidate_names = {};
  /// ExerciseInstruction: instruct that `symbol` is not exercised at expiry, or withdraw it.
  bool do_not_exercise = false;
  trading::FlattenPricing close_pricing;
  trading::AlertSpec alert = {};    ///< CreateAlert: what to watch.
  std::uint64_t alert_id = 0;       ///< DeleteAlert: the alert.
};

/// Previews and dry runs: answered from a private copy of the account, never recorded.
[[nodiscard]] inline bool dry_run(TradingCommand::Kind kind) {
  return kind == TradingCommand::Kind::Preview || kind == TradingCommand::Kind::PreviewChange || kind == TradingCommand::Kind::WhatIf ||
         kind == TradingCommand::Kind::PreviewClose || kind == TradingCommand::Kind::PreviewStock;
}

struct TradingReply {
  trading::Decision decision;
  std::string error_code;  ///< Transport/revision errors outside reducer Reasons.
  std::optional<trading::OrderId> order_id;
  std::optional<std::uint64_t> alert_id;  ///< CreateAlert: the new alert.
  std::shared_ptr<const TradingView> view;
  std::vector<trading::OrderId> cancelled_orders;
  std::vector<trading::OrderId> created_orders;  ///< Orders the command added, in sequence.
  std::vector<std::uint64_t> stock_fills;        ///< Share changes the command made, by StockFill ID.
  std::map<std::string, trading::Decision> kept_stocks;  ///< Shares a flatten left, and why.
  std::vector<trading::Residual> residuals;              ///< Positions a flatten left open, and why.
  std::string account;  ///< The account the command acted on (a new account's ID for CreateAccount).
  bool replayed = false;  ///< A retried order answered as first submitted.
  std::optional<trading::OrderPreview> preview;
  std::optional<trading::WhatIf> what_if;
  std::optional<trading::FlattenPreview> flatten;
  std::optional<trading::StockPreview> stock_preview;
  std::string playbook_result;
  std::string account_result;  ///< Lifecycle response; also available without a working reducer.
};
using TradingCompletion = std::function<void(TradingReply)>;

/// A journal as compact_paper_journals left it.
struct JournalCompaction {
  std::filesystem::path file;
  std::uintmax_t bytes_before = 0;
  std::uintmax_t bytes_after = 0;
  std::filesystem::path backup;  ///< The journal as it was; empty if nothing was rewritten.
  std::string error;             ///< Why the journal was left as it was.
};
/// Rewrite the main paper journal and the account journals in `accounts` whose
/// records carry whole states (schemas 1 and 2) in the compact schema. Each takes the
/// writer's lock, so a running openportd's journals are refused. The rewrite
/// must recover to the same account before it replaces the journal, and the
/// original stays beside it as FILE.bak.
[[nodiscard]] std::vector<JournalCompaction> compact_paper_journals(
    const std::filesystem::path& journal, const std::filesystem::path& accounts);

}  // namespace openport::server
