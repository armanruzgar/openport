#include "openport/providers/replay.hpp"

#include <algorithm>
#include <utility>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

#include "openport/providers/factory.hpp"

namespace openport::providers {
namespace {
class SystemReplayClock final : public ReplayClock {
 public:
  TimePoint now() override { return std::chrono::steady_clock::now(); }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override {
    std::unique_lock lock(mutex_);
    return !wake_.wait_until(lock, deadline, [&] { return stop.load(); });
  }
  void interrupt() override {
    const std::lock_guard lock(mutex_);
    wake_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
};

/// Unsigned subtraction covers the full int64 timestamp range without overflow.
/// Backward wall-clock steps add no delay; subsequent positive gaps still count.
ReplayClock::TimePoint advance(ReplayClock::TimePoint deadline, md::Timestamp previous,
                               md::Timestamp received, int speed, std::uint64_t& remainder) {
  if (received <= previous || speed == 0) return deadline;
  const auto gap = static_cast<std::uint64_t>(received) - static_cast<std::uint64_t>(previous);
  const auto divisor = static_cast<std::uint64_t>(speed);
  const auto fractional = remainder + gap % divisor;
  const auto delta = gap / divisor + fractional / divisor;
  remainder = fractional % divisor;
  const auto room =
      std::chrono::duration_cast<std::chrono::nanoseconds>(ReplayClock::TimePoint::max() - deadline)
          .count();
  if (room < 0 || delta > static_cast<std::uint64_t>(room)) return ReplayClock::TimePoint::max();
  return deadline + std::chrono::nanoseconds(delta);
}
}  // namespace

bool ReplayProvider::valid_speed(int speed) noexcept {
  for (const int allowed : {0, 1, 2, 5, 10, 30, 60, 120, 300})
    if (speed == allowed) return true;
  return false;
}

ReplayProvider::ReplayProvider(Options options)
    : options_(std::move(options)),
      reader_(options_.file),
      name_("replay (" + reader_.header().provider + ")"),
      speed_(options_.speed) {
  if (!valid_speed(options_.speed))
    throw std::invalid_argument("replay: speed must be max, 1, 2, 5, 10, 30, 60, 120 or 300");
  if (!options_.clock) options_.clock = std::make_shared<SystemReplayClock>();
  seeking_ = options_.start_at > 0 || options_.paused || options_.start_through > 0;
  preparing_ = seeking_.load();
  if (options_.start_at == 0 && options_.paused) options_.start_at = reader_.header().started;
  set_paused(options_.paused);
  if (options_.known_end > 0) end_ = options_.known_end;
}

void ReplayProvider::wake() {
  interrupted_ = true;
  { const std::lock_guard lock(control_mutex_); }
  control_.notify_all();
  options_.clock->interrupt();
}

void ReplayProvider::set_speed(int speed) {
  if (!valid_speed(speed))
    throw std::invalid_argument("replay: speed must be max, 1, 2, 5, 10, 30, 60, 120 or 300");
  { const std::lock_guard lock(control_mutex_); speed_ = speed; pause_at_ = 0; }
  wake();
}

void ReplayProvider::set_paused(bool paused) {
  {
    const std::lock_guard lock(control_mutex_);
    // Both ends of a pause belong to the controls, even if the pacing thread
    // observes neither before the resume (for example, just after an until).
    if (paused && !paused_.load()) paused_at_ = options_.clock->now().time_since_epoch().count();
    if (!paused || !paused_.load()) pause_settled_ = false;
    if (paused) paused_ = true;
    else resume_locked();
    pause_at_ = 0;
    if (paused && stepping_.load()) abort_requested_ = true;
  }
  wake();
}

void ReplayProvider::resume_locked() {
  if (paused_.load()) {
    const auto since = ReplayClock::TimePoint(ReplayClock::TimePoint::duration(paused_at_.load()));
    const auto away = std::max(ReplayClock::TimePoint::duration::zero(), options_.clock->now() - since);
    paused_duration_ += std::min(away, ReplayClock::TimePoint::duration::max() - paused_duration_);
  }
  paused_ = false;
}

void ReplayProvider::skip(bool pending) {
  skip_ = pending;
  wake();
}

void ReplayProvider::abort() {
  set_paused(true);
}

void ReplayProvider::wait_paused() {
  std::unique_lock lock(control_mutex_);
  control_.wait(lock, [&] { return pause_settled_ || finished_.load() || stopping_.load(); });
}

void ReplayProvider::validate_target(md::Timestamp target) {
  if (target <= 0) throw std::invalid_argument("Target must be a positive market timestamp");
  if (target % md::kNanosPerSecond != 0 && !snapshot_batches_.load())
    throw std::invalid_argument("Streaming replays settle at whole market seconds; target must name a whole second");
  if (target < std::max(market_time_.load(), in_flight_time_)) throw std::invalid_argument("Target must not precede the current market time");
  if (finished_.load() || stopping_.load()) throw std::invalid_argument("Replay has finished");
}

void ReplayProvider::play_until(md::Timestamp target, std::optional<int> speed) {
  if (!driver_) throw std::invalid_argument("Play until requires a deterministic consumer");
  if (speed && !valid_speed(*speed)) throw std::invalid_argument("Invalid replay speed");
  if (const auto end = end_time(); target > end)
    throw std::invalid_argument("play_until exceeds the recording's end at " + md::format_timestamp(end) + "; the replay did not move");
  {
    const std::lock_guard lock(control_mutex_);
    validate_target(target);
    if (stepping_.load() || preparing_.load()) throw std::invalid_argument("Wait for the current step or start state");
    pause_at_ = target;
    if (speed) speed_ = *speed;
    resume_locked();
    pause_settled_ = false;
  }
  wake();
}

bool ReplayProvider::pace(ReplayClock::TimePoint& deadline, int basis) {
  while (!stopping_.load()) {
    std::unique_lock lock(control_mutex_);
    // Read controls and clear the interrupt under the same lock.
    // A later control must interrupt the wait we are about to start.
    interrupted_ = false;
    if (seeking_.load()) return true;
    // Controls accumulate complete pauses even if this loop never observed them.
    // Consume them exactly once before rescaling the remaining gap for speed.
    const auto paused = std::exchange(paused_duration_, ReplayClock::TimePoint::duration{});
    if (deadline < ReplayClock::TimePoint::max() - paused) deadline += paused;
    else deadline = ReplayClock::TimePoint::max();
    if (paused_.load()) {
      lock.unlock();
      if (!synchronize()) return false;
      lock.lock();
      pause_settled_ = true;
      control_.notify_all();
      control_.wait(lock, [&] { return !paused_.load() || !pause_settled_ || seeking_.load() || stopping_.load(); });
      if (stopping_.load()) return false;
      continue;
    }
    const int speed = speed_.load();
    if (skip_.exchange(false) || speed == 0) {
      deadline = options_.clock->now();
      return true;
    }
    // A speed changed since the gap was measured (during a wait, or while paused, as
    // when a drill starts paused at 1x and plays at 300x) stretches or shrinks what is
    // left of the wait.
    const auto now = options_.clock->now();
    if (speed != basis && basis != 0 && deadline != ReplayClock::TimePoint::max() && deadline > now)
      deadline = now + (deadline - now) / speed * basis;
    basis = speed;
    lock.unlock();
    if (stopping_.load() || paused_.load()) continue;
    if (options_.clock->wait_until(deadline, interrupted_)) return true;
    if (stopping_.load()) return false;
    // A control changed during the wait: the loop applies it.
  }
  return false;
}
ReplayProvider::~ReplayProvider() {
  stop();
}
md::Capabilities ReplayProvider::capabilities() const noexcept {
  return reader_.header().capabilities;
}

void ReplayProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  if (started_) throw std::logic_error("ReplayProvider::start may be called only once");
  started_ = true;
  validate_subscription("replay", subscription);
  const auto& available = reader_.header().subscription.underlyings;
  std::string names;
  for (const auto& symbol : available) names += (names.empty() ? "" : ", ") + symbol;
  if (subscription.underlyings.empty())
    throw std::invalid_argument("replay: no symbols subscribed");
  for (const auto& symbol : subscription.underlyings)
    if (std::find(available.begin(), available.end(), symbol) == available.end())
      throw std::invalid_argument("replay: unknown symbol " + symbol + "; file contains: " + names);
  subscription_ = subscription;
  thread_ = std::thread([this, subscription, &sink] {
    if (driver_) run_deterministic(subscription, sink);
    else run(subscription, sink);
  });
}

void ReplayProvider::stop() {
  if (thread_.joinable()) {
    stopping_ = true;
    wake();
    thread_.join();
  }
}

bool ReplayProvider::synchronize() {
  if (!options_.synchronize) return !stopping_.load();
  auto done = options_.synchronize();
  while (!stopping_.load()) {
    if (done.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) { done.get(); return true; }
  }
  return false;
}

void ReplayProvider::run(md::Subscription subscription, md::EventSink& sink) {
  const std::set<std::string> symbols(subscription.underlyings.begin(),
                                      subscription.underlyings.end());
  try {
    do {
      std::unordered_map<md::InstrumentId, bool> admitted;
      auto deadline = options_.clock->now();
      std::optional<md::Timestamp> previous;
      std::uint64_t remainder = 0;
      bool any = false;
      md::Timestamp synchronized_at = 0;
      while (!stopping_.load()) {
        auto record = reader_.next();
        if (!record) break;
        if (seeking_.load() && previous && record->received > *previous) {
          // Finish the entire receipt group, then wait for the engine. Periodic
          // barriers keep inferred candles and the progress tick moving too.
          if (*previous >= options_.start_at || *previous - synchronized_at >= md::kNanosPerMinute) {
            if (!synchronize()) break;
            synchronized_at = *previous;
          }
          const std::lock_guard lock(control_mutex_);
          deadline = options_.clock->now();
          paused_duration_ = {};
          if (*previous >= options_.start_at) {
            seeking_ = false;
            preparing_ = false;
            if (paused_.load()) paused_at_ = deadline.time_since_epoch().count();
          }
        }
        const int measured = speed_.load();
        if (previous && !seeking_.load()) {
          const auto from = options_.max_gap > 0 && record->received - *previous > options_.max_gap ? record->received - options_.max_gap : *previous;
          deadline = advance(deadline, from, record->received, measured, remainder);
        }
        previous = record->received;
        const bool include = std::visit(
            [&](const auto& e) {
              using T = std::decay_t<decltype(e)>;
              if constexpr (std::is_same_v<T, md::ContractDefinition>) {
                return admitted[e.id] = symbols.contains(e.contract.underlying);
              } else if constexpr (std::is_same_v<T, md::UnderlyingQuote> || std::is_same_v<T, md::UnderlyingClose>) {
                return symbols.contains(e.symbol);
              } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
                return e.underlying.empty() || symbols.contains(e.underlying);
              } else if constexpr (std::is_same_v<T, md::SnapshotComplete> || std::is_same_v<T, md::SnapshotHeartbeat>) {
                return symbols.contains(e.underlying);
              } else if constexpr (std::is_same_v<T, md::TradingHalt>) {
                return true;
              } else {
                const auto it = admitted.find(e.id);
                if (it == admitted.end())
                  throw std::runtime_error("replay: event references undefined contract " +
                                           std::to_string(e.id));
                return it->second;
              }
            },
            record->event);
        if (!include) continue;
        if (!seeking_.load() && !pace(deadline, measured)) break;
        if (stopping_.load()) break;
        time_ = record->received;
        sink.publish(std::move(record->event));
        any = true;
      }
      if (stopping_.load()) break;
      if (seeking_.load()) {
        if (!synchronize()) break;
        seeking_ = false;
        preparing_ = false;
      }
      if (!reader_.diagnostic().empty()) {
        sink.publish(md::ProviderStatus{md::now(), md::FeedState::Stopped,
                                        name_ + ": " + reader_.diagnostic(), ""});
        finished_ = true;
        return;  // A damaged input must never loop as though it were complete.
      }
      if (!options_.loop || !any) break;
      if (!pace(deadline, speed_.load())) break;
      reader_.rewind();
    } while (!stopping_.load());
    sink.publish(md::ProviderStatus{md::now(), md::FeedState::Stopped,
                                    name_ + (stopping_.load() ? ": stopped" : ": end of recording"),
                                    ""});
  } catch (const std::exception& error) {
    sink.publish(
        md::ProviderStatus{md::now(), md::FeedState::Error, name_ + ": " + error.what(), ""});
  }
  if (!stopping_.load()) (void)synchronize();
  preparing_ = false;
  finished_ = true;
}

void ReplayProvider::set_driver(Driver driver, std::function<std::future<void>()> barrier) {
  if (started_) throw std::logic_error("Set replay driver before start");
  if (options_.loop) throw std::invalid_argument("Trading replays require loop=off; start a fresh run to repeat the day");
  driver_ = std::move(driver);
  if (barrier) options_.synchronize = std::move(barrier);
}
md::Timestamp ReplayProvider::end_time(const md::Subscription& subscription) {
  const std::lock_guard lock(end_mutex_);
  if (end_) return *end_;
  const auto& selected = subscription.underlyings.empty() ? (subscription_.underlyings.empty() ? reader_.header().subscription : subscription_) : subscription;
  if (const auto indexed = reader_.indexed_end(selected)) { end_ = *indexed; return *end_; }
  // A reader of its own batches the file as playback will, without touching playback.
  md::Timestamp last = 0;
  try {
    md::RecordingReader reader(options_.file);
    ReplayBatches batches(reader, selected);
    while (!stopping_.load()) {
      const auto batch = batches.next();
      if (!batch) break;
      last = batch->time;
    }
  } catch (const std::exception&) {
    // Playback reports a damaged recording where it reaches the damage.
    last = std::numeric_limits<md::Timestamp>::max();
  }
  if (stopping_.load()) return std::numeric_limits<md::Timestamp>::max();
  end_ = last;
  return last;
}

bool ReplayProvider::until(md::Timestamp target) {
  if (!driver_) throw std::invalid_argument("Lockstep requires a deterministic consumer");
  {
    const std::lock_guard lock(control_mutex_);
    if (stepping_.exchange(true)) throw std::invalid_argument("Another lockstep advance is in progress");
    abort_requested_ = false;
    step_aborted_ = false;
  }
  // stepping() holds from here until this returns, the end's first lookup included.
  struct Stepped { std::atomic<bool>& stepping; ~Stepped() { stepping = false; } } stepped{stepping_};
  // Refuse a target past EOF before playing anything: an error leaves the run where it was.
  if (const auto end = end_time(); target > end)
    throw std::invalid_argument("until exceeds the recording's end at " + md::format_timestamp(end) + "; the replay did not move");
  std::unique_lock lock(control_mutex_);
  validate_target(target);
  pause_at_ = 0;
  pause_settled_ = false;
  step_target_ = target;
  step_pending_ = true;
  seeking_ = true;
  lock.unlock();
  wake();
  lock.lock();
  control_.wait(lock, [&] { return !step_pending_ || finished_.load() || stopping_.load(); });
  if (!playback_error_.empty()) throw std::runtime_error(playback_error_);
  if (!step_aborted_ && settled_.load() < target)
    throw std::invalid_argument(stopping_.load() ? "The replay stopped before until" : "until exceeds the recording's end");
  return step_aborted_;
}

void ReplayProvider::run_deterministic(md::Subscription subscription, md::EventSink& sink) {
  try {
    ReplayBatches batches(reader_, subscription);
    snapshot_batches_ = batches.snapshot_feed();
    auto next = batches.next();
    next_time_ = next ? next->time : 0;
    auto deadline = options_.clock->now();
    md::Timestamp previous = 0;
    std::uint64_t remainder = 0;
    while (!stopping_.load() && next) {
      {
        std::unique_lock lock(control_mutex_);
        if (preparing_.load() && options_.start_through > 0 && next->time > options_.start_through) {
          lock.unlock();
          if (!synchronize()) break;
          lock.lock();
          market_time_ = options_.start_through;
          settled_ = options_.start_through;
          seeking_ = false;
          preparing_ = false;
          deadline = options_.clock->now();
          paused_duration_ = {};
          paused_at_ = deadline.time_since_epoch().count();
        }
        const auto paced_target = pause_at_.load();
        if ((step_pending_ && (abort_requested_ || next->time > step_target_)) ||
            (paced_target > 0 && next->time > paced_target)) {
          lock.unlock();
          if (!synchronize()) break;
          lock.lock();
          // A manual control can supersede a play-to while its barrier runs.
          if (!step_pending_ && pause_at_.load() != paced_target) continue;
          step_aborted_ = abort_requested_;
          if (!step_pending_ || !step_aborted_) {
            market_time_ = step_pending_ ? step_target_ : paced_target;
            settled_ = market_time_.load();
          }
          paused_ = true;
          pause_settled_ = true;
          pause_at_ = 0;
          // The step played its batches unpaced: the next gap counts from here, not
          // from a deadline measured before it, or each step would add a stale gap.
          deadline = options_.clock->now();
          paused_duration_ = {};
          paused_at_ = deadline.time_since_epoch().count();
          seeking_ = false;
          step_pending_ = false;
          control_.notify_all();
        }
      }
      const int measured = speed_.load();
      if (previous > 0 && !seeking_.load()) {
        const auto from = options_.max_gap > 0 && next->received - previous > options_.max_gap ? next->received - options_.max_gap : previous;
        deadline = advance(deadline, from, next->received, measured, remainder);
      }
      if (!preparing_.load() && !pace(deadline, measured)) break;
      if (stopping_.load()) break;
      // An until request may have interrupted the wait before this future batch.
      {
        const std::lock_guard lock(control_mutex_);
        if (step_pending_ && (abort_requested_ || next->time > step_target_)) continue;
        if (pause_at_.load() > 0 && next->time > pause_at_.load()) continue;
        if (paused_.load() && !seeking_.load()) continue;
        in_flight_time_ = next->time;
      }
      const auto receipt = next->received;
      const auto through = next->time;
      auto done = driver_(std::move(*next));
      while (!stopping_.load() && done.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {}
      if (stopping_.load()) break;
      done.get();
      { const std::lock_guard lock(control_mutex_); time_ = receipt; market_time_ = through; in_flight_time_ = 0; }
      settled_ = through;
      previous = receipt;
      next = batches.next();
      next_time_ = next ? next->time : 0;
      if (preparing_.load() && options_.start_through == 0 && receipt >= options_.start_at && (!next || next->received > receipt)) {
        if (paused_.load() && !synchronize()) break;
        const std::lock_guard lock(control_mutex_);
        seeking_ = false;
        preparing_ = false;
        deadline = options_.clock->now();
        paused_duration_ = {};
        if (paused_.load()) paused_at_ = deadline.time_since_epoch().count();
      }

    }
    sink.publish(md::ProviderStatus{time_.load(), md::FeedState::Stopped,
        name_ + (stopping_.load() ? ": stopped" : ": end of recording"), ""});
  } catch (const std::exception& error) {
    { const std::lock_guard lock(control_mutex_); playback_error_ = error.what(); }
    sink.publish(md::ProviderStatus{time_.load(), md::FeedState::Error, name_ + ": " + error.what(), ""});
  }
  if (!stopping_.load()) (void)synchronize();
  {
    const std::lock_guard lock(control_mutex_);
    if (step_pending_) {
      paused_ = true;
      step_aborted_ = abort_requested_;
    }
    if (pause_at_.load() > 0) paused_ = true;
    step_pending_ = false;
    pause_at_ = 0;
    pause_settled_ = true;
    seeking_ = false;
    preparing_ = false;
    finished_ = true;
  }
  control_.notify_all();
}

}  // namespace openport::providers
