#pragma once

#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "openport/analytics/chain_book.hpp"
#include "openport/md/provider.hpp"
#include "openport/md/recording.hpp"
#include "openport/server/candles.hpp"
#include "openport/server/paper.hpp"
#include "openport/trading/dividends.hpp"
#include "openport/analytics/volatility.hpp"

namespace openport::server {
class Playbooks;
class SeriesStore;
class Sandboxes;

/// Single-owner market-to-account kernel. The caller supplies both clocks and
/// drain boundaries; this class has no threads, queue, HTTP or clock reads.
class Desk {
 public:
  struct Options {
    std::shared_ptr<Sandboxes> sandboxes;
    std::string initial_actor = "system";
    std::string initial_playbooks;  ///< Replay provenance snapshot; empty loads the journal sibling.
    std::shared_ptr<SeriesStore> series;
    std::string run_input;  ///< Canonical provenance JSON; empty for legacy/live desks.
    std::string run_id;     ///< The run's id, named in its exports; empty without one.
    bool paper_enabled = true;
    bool replay = false;  ///< Publish the replay market clock and keep halt state isolated.
    md::Timestamp replay_start = 0, replay_end = 0;  ///< Last complete market batch for pending-setting guidance.
    /// Driver 2, recorded in a run's start input: the driver applies every underlying's
    /// snapshot of one market time as one batch, and each underlying's quotes are offered
    /// at the market time its own snapshot arrived. False reproduces older runs, whose
    /// driver batched each snapshot alone and offered every vouched quote at the latest time.
    bool instant_batches = true;
    /// Driver 3, which builds on driver 2 and applies only with instant_batches: a new
    /// trading date's first batch rolls each account over on the finished day's closing
    /// marks before that batch's quotes. False reproduces older runs, which rolled over
    /// after them, so the finished day's P&L by Greek took the overnight move.
    bool closing_rollover = true;
    /// Driver 4, which builds on driver 3 and applies only with it: a command's run input
    /// is recorded before the transactions it causes, at each account's own time so that
    /// it moves no clock, and a journal a crash cut off between them still verifies.
    /// False reproduces older runs, which recorded it after them.
    bool inputs_first = true;
    /// Driver 5 settles AM positions on their expiry date's first print at or after
    /// 09:30 ET in demo and replay. False preserves drivers 1–4's manual AM import.
    bool opening_settlement = true;
    /// Driver 6 labels automatic time-stop cancellations. Drivers 1–5 retain
    /// USER_CANCEL, including their journal bytes during offline verification.
    bool playbook_cancel_labels = true;
    /// Optional run-start capability. Absent on older runs, preserving their fills.
    bool opening_rule_checks = true;
    /// Optional run-start capability: older runs journal the entire input maps.
    bool compact_opening_rule_inputs = true;
    bool exact_margin = true; ///< Recorded allocation algorithm; false for older runs.
    std::filesystem::path paper_journal;  ///< The main account. Empty only for explicit in-process simulations.
    /// More named accounts, one journal each (<id>.jsonl, named in <id>.name). Empty for none.
    std::filesystem::path paper_accounts;
    trading::SessionConfig paper;
    /// Dividends every account pays on held shares at the rollover into each ex-date.
    std::vector<trading::Dividend> dividends;
    std::vector<analytics::EventLabel> events;
    std::shared_ptr<trading::Journal> paper_sink;  ///< Optional in-process test/simulation sink.
    /// A saved replay run to continue, read from paper_journal: the main account starts
    /// afresh and re-executes the run's recorded inputs in order, each batch's boundary
    /// checked against the batch the driver delivers, and its transactions are compared
    /// with the journal's until they reach its end, after which they append to it.
    std::shared_ptr<const trading::JournalRecovery> resume;
    trading::FileJournal::Hooks journal_io;  ///< Injectable I/O only; replay selects the sync policy.
    std::string write_mode = "open";
    analytics::AnalyticsOptions analytics;
    std::shared_ptr<CandleStore> candles;
    std::function<void(std::string_view, const EquitySample&)> equity_sample;
  };
  struct PendingCommand {
    std::uint64_t sequence;
    TradingCommand command;
    TradingCompletion completion;
  };

  Desk(std::string provider, md::Capabilities capabilities, md::Subscription subscription, Options options);
  void start_trading();
  void expire_sandboxes(const std::vector<std::string>& expired);
  void halt() { stopping_ = true; }
  void stop() { halt(); flush_journals(); accounts_.clear(); }
  /// Owner-thread durability barrier. A failed sync disables that account.
  void flush_journals();
  void set_dividends(std::vector<trading::Dividend> dividends);
  void set_publication_sink(std::function<void(std::string_view, const TradingView&)> sink) {
    publication_sink_ = std::move(sink);
  }
  void set_removal_sink(std::function<void(const std::string&)> sink) { removal_sink_ = std::move(sink); }
  void observe(const md::Event& event);
  bool refresh_analytics();
  void apply_analytics(std::shared_ptr<const analytics::UnderlyingMetrics> result, md::Timestamp time);
  void update_trading(const std::vector<md::Event>& batch, std::deque<PendingCommand>& commands,
                      md::Timestamp driver_time);
  void apply_command(PendingCommand& pending, md::Timestamp market_time, md::Timestamp driver_time);
  /// Begin a later recording while keeping the account and candle history.
  void replay_source(const std::string& input, const md::RecordingHeader& header);
  void replay_batch(const std::vector<md::Event>& batch, md::Timestamp driver_time, md::Timestamp boundary_time = 0);
  void command(TradingCommand command, TradingCompletion completion, md::Timestamp time, md::Timestamp driver_time);
  [[nodiscard]] bool active() const;
  [[nodiscard]] md::Timestamp market_time() const { return market_time_; }
  [[nodiscard]] const analytics::ChainBook& book() const { return book_; }
  [[nodiscard]] const auto& publications() const { return metrics_; }
  [[nodiscard]] const auto& views() const { return trading_views_; }
  [[nodiscard]] const auto& accounts() const { return account_statuses_; }
  [[nodiscard]] const auto& trading_status() const { return trading_status_; }
  [[nodiscard]] const auto& breaker() const { return breaker_; }
  [[nodiscard]] std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const;
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view(std::string_view account = {}) const;

 private:
  std::function<void(std::string_view, const TradingView&)> publication_sink_;
  std::function<void(const std::string&)> removal_sink_;
  /// One paper account: a reducer and its journal. Single-owner state.
  struct PaperAccount {
    std::string id;
    std::string name;
    std::unique_ptr<trading::TradingSession> session;
    std::string failure;  ///< Why it cannot trade; empty while it can.
    std::unique_ptr<EquityStore> equity;
    std::shared_ptr<const trading::TradingSnapshot> sampled_snapshot;
    std::shared_ptr<trading::Journal> journal;
    bool archived = false;
    std::optional<AccountDamage> damaged = {};
  };
  PaperAccount* find_account(std::string_view id);
  void create_account(const TradingCommand& command, TradingReply& reply);
  void manage_account(const TradingCommand& command, TradingReply& reply);
  void observe_trading(const md::Event& event);
  void publish_trading();
  void record_input(const std::string& input, const std::string& actor = "system", bool at_account_time = false);
  /// Whether a reproducible run records `command` as an input: every one a trader sends but a preview.
  [[nodiscard]] bool recorded_input(const TradingCommand& command) const;
  void record_command(const TradingCommand& command, md::Timestamp driver_time);
  void price_stock_command(TradingCommand& command, md::Timestamp driver_time);
  [[nodiscard]] trading::Decision counter_position_gate(const PaperAccount& account,
      const std::map<std::string, double>& direction, md::Timestamp time) const;
  [[nodiscard]] std::vector<trading::CounterExposure> counter_exposures(const PaperAccount& account,
      md::Timestamp time) const;
  [[nodiscard]] std::optional<std::int64_t> option_volume(const std::string& symbol, md::Timestamp time) const;
  [[nodiscard]] std::optional<trading::OpeningRuleInputs> opening_rule_inputs(const PaperAccount& account,
      md::Timestamp time) const;
  [[nodiscard]] trading::Decision opening_gate(const PaperAccount& account, const trading::OrderRequest& order,
      md::Timestamp time, const trading::PreviewMarket& market = {},
      const std::map<std::string, trading::Quantity>& preceding = {}) const;
  /// Re-executes a resumed run's recorded inputs up to its next boundary.
  void replay_recorded();
  /// Stops every account of a resumed run whose inputs no longer match its recording.
  void resume_failed(const std::string& reason);
  std::deque<std::string> resume_inputs_;  ///< A resumed run's inputs still to re-execute.
  [[nodiscard]] bool inputs_first() const {
    return options_.instant_batches && options_.closing_rollover && options_.inputs_first;
  }
  /// `input_recorded`: Desk::command recorded it already, before the quotes it takes.
  void apply_command(PendingCommand& pending, md::Timestamp market_time, md::Timestamp driver_time, bool input_recorded);
  [[nodiscard]] std::string opening_source() const;
  void sample_equity(PaperAccount& account);
  void evaluate_playbooks(md::Timestamp driver_time);
  void playbook_command(const TradingCommand& command, TradingReply& reply, md::Timestamp driver_time);
  std::shared_ptr<Playbooks> playbooks_;
  bool playbook_running_ = false;
  std::uint64_t analytics_generation_ = 0;  ///< Counts published analytics results.
  std::uint64_t playbook_generation_ = 0;   ///< The generation playbooks last evaluated.
  std::map<std::string, std::pair<std::uint64_t, std::string>> playbook_publications_;  ///< By account, at a revision.
  /// Stops one account after a journal or integration failure; the others carry on.
  void fail_trading(PaperAccount& account, std::string reason);

  Options options_;
  bool scenario_source_ = false;
  std::optional<RunIdentity> run_;  ///< What options_.run_input replays, for exports.
  std::string provider_;
  md::Subscription subscription_;
  md::Capabilities capabilities_;
  analytics::ChainBook book_;
  bool stopping_ = false;
  TradingStatus trading_status_;
  std::vector<AccountStatus> account_statuses_;
  std::map<std::string, std::shared_ptr<const TradingView>, std::less<>> trading_views_;
  std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>> metrics_;
  // Initialized/recovered by start_trading(), then owned by the caller.
  std::vector<PaperAccount> accounts_;  // the main account first
  std::set<std::string> sandbox_ids_;
  /// Each underlying's first print at or after a date's regular close, and its
  /// last one before it, for the last week of dates.
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> opening_prints_;
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> closing_prints_;
  std::map<std::pair<std::string, md::Date>, md::UnderlyingQuote> before_close_;
  /// Official closes by symbol and date (md::UnderlyingClose), for a week of dates.
  std::map<std::pair<std::string, md::Date>, md::UnderlyingClose> official_closes_;
  CircuitBreakerStatus breaker_;
  bool breaker_dirty_ = false;
  bool breaker_storage_ = false;  // only while the main journal's writer lock is held
  std::vector<trading::Dividend> dividends_;
  std::uint64_t dividends_version_ = 0;
  void check_circuit_breaker(const md::UnderlyingQuote& spot);
  void advance_circuit_breaker(md::Timestamp time);
  void publish_circuit_breaker();
  void load_circuit_breaker();
  void save_circuit_breaker();
  md::Timestamp market_time_ = 0;
  std::map<std::string, md::InstrumentId> instruments_;
  std::map<std::string, std::uint64_t> observations_;
  /// Each underlying's latest md::SnapshotComplete time, from snapshot providers.
  std::map<std::string, md::Timestamp> snapshots_;
  /// The market time each underlying's latest complete snapshot arrived at. It vouches
  /// for that underlying's quotes as of then; another underlying's data cannot refresh them.
  std::map<std::string, md::Timestamp> vouched_at_;

  std::map<std::string, std::uint64_t> analysed_versions_;
  std::uint64_t analysed_dividends_version_ = 0;
  std::map<std::string, std::shared_ptr<const analytics::DiscountCurve>> discount_curves_;
  std::shared_ptr<const analytics::DiscountCurve> discount_curve_;
};

}  // namespace openport::server
