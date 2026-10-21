#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "openport/md/recording.hpp"
#include "openport/providers/replay_batches.hpp"

namespace openport::providers {

/// A monotonic clock and interruptible wait keep timing tests independent of
/// scheduler jitter. interrupt() must wake a pending wait after stop is set.
class ReplayClock {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  virtual ~ReplayClock() = default;
  virtual TimePoint now() = 0;
  virtual bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) = 0;
  virtual void interrupt() = 0;
};

class ReplayProvider final : public md::Provider {
 public:
  struct Options {
    std::filesystem::path file;
    int speed = 1;  ///< 0 means maximum throughput; otherwise one of valid_speed's
    bool loop = false;
    std::shared_ptr<ReplayClock> clock;
    md::Timestamp start_at = 0;
    bool paused = false;
    /// Paced playback waits at most this much receipt time between batches, so a
    /// closed market between sessions passes in one step; zero waits every gap out.
    md::Timestamp max_gap = 0;
    std::function<std::future<void>()> synchronize = {};
    md::Timestamp known_end = 0;  ///< Trusted generated scenario window, avoiding a discovery scan.
    md::Timestamp start_through = 0;  ///< Restart preparation uses market time, including through T.
  };

  /// 0 (as fast as possible), 1, 2, 5, 10, 30, 60, 120 or 300 times real time.
  [[nodiscard]] static bool valid_speed(int speed) noexcept;

  explicit ReplayProvider(Options options);
  ~ReplayProvider() override;
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  [[nodiscard]] bool market_controls() const noexcept override { return reader_.header().market_controls; }
  [[nodiscard]] bool trade_venues() const noexcept override { return reader_.header().trade_venues; }
  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

  /// Controls for a running replay, safe from any thread. A new speed applies to
  /// what is left of the current wait; time spent paused does not count; skip
  /// publishes the next event now, cutting short a long gap such as a closed market.
  void set_speed(int speed);
  void set_paused(bool paused);
  void skip(bool pending = true);
  [[nodiscard]] bool skip_pending() const noexcept { return skip_.load(); }
  /// Interrupt a step at a complete, synchronized batch boundary.
  void abort();
  void wait_paused();
  /// Paced asynchronous playback; optional speed and target validate before mutation.
  void play_until(md::Timestamp target, std::optional<int> speed = {});
  [[nodiscard]] md::Timestamp pause_at() const noexcept { return pause_at_.load(); }
  [[nodiscard]] md::Timestamp next_time() const noexcept { return next_time_.load(); }
  /// Install before start. Each completed batch waits for its consumer; no queue
  /// coalescing or playback-clock scheduling can change its contents.
  using Driver = std::function<std::future<void>(ReplayBatch)>;
  /// The optional barrier settles pending I/O at pauses, steps and EOF.
  void set_driver(Driver driver, std::function<std::future<void>()> barrier = {});
  /// Synchronous lockstep advance; returns only after all complete input through
  /// the target is settled. Controls and shutdown interrupt the pacing wait. A target
  /// past the recording's last batch throws before anything plays.
  bool until(md::Timestamp target);  ///< true if interrupted by abort/pause/stop
  /// The market time of the recording's last complete batch, read once on first use.
  [[nodiscard]] md::Timestamp end_time(const md::Subscription& subscription = {});
  [[nodiscard]] md::Timestamp settled_through() const { return settled_.load(); }
  [[nodiscard]] md::Timestamp market_time() const { return market_time_.load(); }
  [[nodiscard]] const std::filesystem::path& file() const { return options_.file; }
  [[nodiscard]] int speed() const noexcept { return speed_.load(); }
  [[nodiscard]] bool paused() const noexcept { return paused_.load(); }
  /// The replay's clock: the recorded receipt time of the latest event published, 0 before the first.
  [[nodiscard]] md::Timestamp time() const noexcept { return time_.load(); }
  /// Preparing the start state: playing unpaced up to start_at before trading opens.
  [[nodiscard]] bool fast_forwarding() const noexcept { return preparing_.load(); }
  /// A lockstep advance (until) is under way: checking its target or playing to it.
  [[nodiscard]] bool stepping() const noexcept { return stepping_.load(); }
  [[nodiscard]] bool finished() const noexcept { return finished_.load(); }
  [[nodiscard]] const md::RecordingHeader& header() const { return reader_.header(); }

 private:
  void run_deterministic(md::Subscription subscription, md::EventSink& sink);
  void run(md::Subscription subscription, md::EventSink& sink);
  /// Waits until `deadline`, honouring the controls; false once stopping.
  /// `basis` is the speed `deadline` was measured at; a different speed rescales
  /// what is left of the wait, including one chosen while paused.
  bool pace(ReplayClock::TimePoint& deadline, int basis);
  bool synchronize();
  void resume_locked();  // control_mutex_ held; account for the pause at resume time
  void validate_target(md::Timestamp target);  // control_mutex_ held; end checked by caller
  void wake();
  Options options_;
  md::RecordingReader reader_;
  std::string name_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  /// Set by stop or a control change: it ends a paced wait early.
  std::atomic<bool> interrupted_{false};
  std::atomic<int> speed_;
  std::atomic<bool> paused_{false};
  /// When the current pause was asked for, on the replay clock's time line.
  std::atomic<ReplayClock::TimePoint::rep> paused_at_{0};
  std::atomic<bool> skip_{false};
  std::atomic<md::Timestamp> time_{0};
  std::atomic<bool> finished_{false};
  /// Playing unpaced: preparing the start state or stepping.
  std::atomic<bool> seeking_{false};
  std::atomic<bool> preparing_{false};
  std::mutex control_mutex_;
  ReplayClock::TimePoint::duration paused_duration_{};  // control_mutex_; consumed once by pace()
  std::condition_variable control_;
  bool started_ = false;
  Driver driver_;
  std::atomic<md::Timestamp> settled_{0};
  std::atomic<bool> snapshot_batches_{false};
  std::atomic<md::Timestamp> market_time_{0};
  md::Timestamp in_flight_time_ = 0;  // control_mutex_
  md::Timestamp step_target_ = 0;  // control_mutex_
  bool step_pending_ = false;  // control_mutex_
  bool abort_requested_ = false;  // control_mutex_
  bool step_aborted_ = false;  // control_mutex_
  bool pause_settled_ = false;  // control_mutex_
  std::atomic<md::Timestamp> pause_at_{0};
  std::atomic<md::Timestamp> next_time_{0};
  std::atomic<bool> stepping_{false};
  std::string playback_error_;
  md::Subscription subscription_;
  std::mutex end_mutex_;
  std::optional<md::Timestamp> end_;  // end_mutex_
};

}  // namespace openport::providers
