#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/analytics/chain_book.hpp"
#include "openport/md/event_queue.hpp"
#include "openport/md/provider.hpp"
#include "openport/md/recording.hpp"
#include "openport/server/candles.hpp"
#include "openport/server/series.hpp"
#include "openport/analytics/volatility.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/notifications.hpp"
#include "openport/providers/replay.hpp"
#include "openport/trading/dividends.hpp"

namespace openport::server {

struct UnderlyingHealth {
  md::FeedState state = md::FeedState::Connecting;
  std::string message;
  md::Timestamp last_success = 0;  ///< local receipt time, never the delayed market-data clock
  std::string last_error;
  md::Timestamp last_error_time = 0;
};

/// A consistent picture of the feed and the engine for the status endpoint.
struct EngineStatus {
  CircuitBreakerStatus circuit_breaker;
  TradingStatus trading;  ///< The main account's.
  std::vector<AccountStatus> accounts;  ///< Every account, the main one first.
  std::string provider;
  md::Capabilities capabilities;
  md::FeedState feed_state = md::FeedState::Connecting;
  std::string feed_message;
  md::Timestamp feed_updated = 0;
  std::size_t queue_depth = 0;
  std::uint64_t coalesced_events = 0;
  std::uint64_t dropped_events = 0;
  bool overloaded = false;
  std::uint64_t events = 0;
  double events_per_second = 0.0;
  double analytics_ms = 0.0;  ///< time the last analytics pass took
  std::size_t contracts = 0;
  std::size_t nonstandard_contracts = 0;
  md::Timestamp started = 0;  ///< on the engine's clock, a recording's for a replay
  /// Wall time since the engine started, whatever clock its market data keeps.
  std::chrono::steady_clock::duration uptime{};
  std::map<std::string, UnderlyingHealth> underlyings;
};

/// Read-only view of the engine's results. The HTTP API depends only on this, so it
/// can be tested without a provider or a thread.
class MetricsSource {
 public:
  virtual ~MetricsSource() = default;
  [[nodiscard]] virtual std::vector<std::string> symbols() const = 0;
  /// nullptr until the first analytics pass for `symbol` has run.
  [[nodiscard]] virtual std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const = 0;
  [[nodiscard]] virtual EngineStatus status() const = 0;
  [[nodiscard]] virtual Notifications* notifications() const { return nullptr; }
  /// Public settings remain visible even when this source cannot forward events.
  [[nodiscard]] virtual const Notifications* configured_notifications() const { return notifications(); }
  [[nodiscard]] virtual Sandboxes* sandboxes() const { return nullptr; }
  [[nodiscard]] virtual md::Timestamp wall_time() const { return md::now(); }
  /// The main account's publication.
  [[nodiscard]] virtual std::shared_ptr<const TradingView> trading_view() const { return {}; }
  /// A named account's publication; the main account's for an empty id. Null when unknown.
  [[nodiscard]] virtual std::shared_ptr<const TradingView> trading_view(std::string_view account) const {
    return account.empty() || account == kMainAccount ? trading_view() : nullptr;
  }
  /// Supplied labels for the volatility view; they do not affect trading.
  [[nodiscard]] virtual std::vector<analytics::EventLabel> events() const { return {}; }
  /// Price history for charts and realized volatility; nullptr when absent.
  [[nodiscard]] virtual const CandleStore* candles() const { return nullptr; }
  [[nodiscard]] virtual const SeriesStore* series() const { return nullptr; }
  /// False means unavailable or full. Completion runs on the engine thread;
  /// network callers must dispatch it onto their own executor.
  virtual bool post_trading(TradingCommand, TradingCompletion) { return false; }
};

/// Owns the provider's event stream: applies every event to the chain book on one
/// thread, and re-runs the analytics at a fixed cadence for each underlying whose
/// data changed. Results are published as immutable snapshots, so readers on other
/// threads never block the engine for longer than a pointer copy.
class Engine final : public MetricsSource {
 public:
  struct Options : Desk::Options {
    std::shared_ptr<Notifications> notifications;
    std::chrono::milliseconds analytics_interval{1000};
    std::size_t command_capacity = 256;
    /// Revision 4 demo accounts use the generated calendar unless a source is explicit.
    bool demo_dividends = true;
    std::filesystem::path record_file;
    md::RecordingSink::Options recording;
    /// Volatility history written by the live engine only; replays and the demo reset it.
    std::function<md::Timestamp()> clock = md::now;
    /// Monotonic cadence for publishing receipt timestamps, independent of wall-clock jumps.
    std::function<std::chrono::steady_clock::time_point()> monotonic_clock =
        std::chrono::steady_clock::now;
    /// Injectable so failure to create the consumer thread can be tested.
    std::function<std::thread(std::function<void()>)> launch = [](std::function<void()> run) {
      return std::thread(std::move(run));
    };
  };

  Engine(md::Provider& provider, md::Subscription subscription, Options options);
  ~Engine() override;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  /// One attempt per instance; construct a new Engine to restart.
  /// Returns after paper journal initialization; failures are reported in status.
  void start();
  void stop();
  /// Flush already-published events through analytics and account publication,
  /// including pending journal syncs for replays.
  /// The provider waits interruptibly, so shutdown and startup failure cannot deadlock.
  [[nodiscard]] std::future<void> synchronize();
  /// Replaces dividends for day rollovers and American analytics (Options::dividends
  /// at first), as a fetcher learns of new ones. Thread-safe.
  void set_dividends(std::vector<trading::Dividend> dividends);

  [[nodiscard]] std::vector<std::string> symbols() const override;
  [[nodiscard]] std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const override;
  [[nodiscard]] EngineStatus status() const override;
  [[nodiscard]] Notifications* notifications() const override { return options_.notifications.get(); }
  [[nodiscard]] const Notifications* configured_notifications() const override { return configured_notifications_.get(); }
  [[nodiscard]] Sandboxes* sandboxes() const override { return options_.sandboxes.get(); }
  [[nodiscard]] md::Timestamp wall_time() const override { return options_.clock(); }
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view() const override;
  [[nodiscard]] std::shared_ptr<const TradingView> trading_view(std::string_view account) const override;
  [[nodiscard]] std::vector<analytics::EventLabel> events() const override { return options_.events; }
  [[nodiscard]] const CandleStore* candles() const override { return options_.candles.get(); }
  [[nodiscard]] const SeriesStore* series() const override { return options_.series.get(); }
  bool post_trading(TradingCommand command, TradingCompletion completion) override;
  [[nodiscard]] md::RecordingStats recording_stats() const;
  [[nodiscard]] std::string recording_error() const;

 private:
  using PendingCommand = Desk::PendingCommand;
  struct PendingReplay {
    providers::ReplayBatch batch;
    std::promise<void> done;
  };
  std::future<void> consume_replay(providers::ReplayBatch batch);
  void run();
  void publish_desk();
  void refresh_analytics();
  void update_health(const md::Event& event, md::Timestamp received);
  [[nodiscard]] std::string notification_account(std::string_view account) const;

  md::Provider& provider_;
  md::Subscription subscription_;
  Options options_;
  std::shared_ptr<Notifications> configured_notifications_;
  md::EventQueue queue_;
  std::unique_ptr<md::RecordingSink> recorder_;
  Desk desk_;  // engine thread only
  std::set<std::string> notification_accounts_;  // engine thread; forgotten after joining it

  std::unique_ptr<SeriesWorker> series_worker_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  bool started_ = false;
  bool provider_started_ = false;

  mutable std::mutex mutex_;  // guards everything below
  std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>> metrics_;
  EngineStatus status_;
  std::map<std::string, std::shared_ptr<const TradingView>, std::less<>> trading_views_;

  std::mutex sync_mutex_;
  std::vector<std::promise<void>> synchronizations_;
  std::mutex command_mutex_;
  std::deque<PendingCommand> commands_;
  std::deque<PendingReplay> replay_batches_;
  providers::ReplayProvider* replay_ = nullptr;
  bool demo_ = false;
  std::uint64_t next_command_ = 1;
  bool accepting_commands_ = false;

  std::mutex dividends_mutex_;
  std::optional<std::vector<trading::Dividend>> pending_dividends_;

  // Engine thread only: quote receipt never locks the reader-facing status mutex.
  std::map<std::string, UnderlyingHealth> health_;
  std::set<std::string> stalled_;
  md::Timestamp feed_updated_ = 0;
  bool health_dirty_ = false;
  bool health_changed_ = false;
  std::uint64_t events_ = 0;
  std::uint64_t events_at_last_rate_ = 0;
  std::chrono::steady_clock::time_point last_rate_time_;
  /// Set by start(). A replay's clock reads 0 until its first event, so its
  /// start on that clock cannot say whether the engine is running.
  std::optional<std::chrono::steady_clock::time_point> started_at_;  // mutex_
};

}  // namespace openport::server
