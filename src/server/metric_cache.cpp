#include "metric_cache.hpp"

#include <condition_variable>
#include <deque>
#include <exception>
#include <thread>
#include <utility>

namespace openport::server {
namespace {
/// One thread for Heston and SABR fits that API reads defer, so a fit of seconds
/// never holds an HTTP I/O thread. Started on first use; joined at exit, when
/// queued jobs are dropped (the web server has already severed their completions).
class FitWorker {
 public:
  ~FitWorker() {
    {
      const std::lock_guard lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  /// False when the queue is full; the job is then not taken.
  bool post(std::function<void()>& job) {
    const std::lock_guard lock(mutex_);
    if (stop_ || jobs_.size() >= kMaxQueued) return false;
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    jobs_.push_back(std::move(job));
    wake_.notify_one();
    return true;
  }

 private:
  static constexpr std::size_t kMaxQueued = 4;
  void run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool stop_ = false;
  std::thread thread_;
};

ApiResponse fit_busy(std::string message) {
  auto busy = api_error(503, "FIT_BUSY", std::move(message));
  busy.retry_after = kFitRetrySeconds;
  return busy;
}
}  // namespace

void run_deferred(DeferredRead read, ApiCompletion complete) {
  const auto cache = read.cache;
  bool running = false;
  {
    const std::lock_guard lock(cache->mutex);
    running = std::exchange(cache->fitting, true);
  }
  if (running) {
    complete(fit_busy("A model fit for this snapshot is already running; retry shortly"));
    return;
  }
  std::function<void()> job = [read = std::move(read), complete] {
    ApiResponse response;
    try { response = read.run(); }
    catch (const std::exception&) { response = api_error(500, "INTERNAL_ERROR", "Internal error"); }
    complete(std::move(response));
    // Cleared after completing, so a read refused as busy came before the answer.
    const std::lock_guard lock(read.cache->mutex);
    read.cache->fitting = false;
  };
  static FitWorker worker;
  if (worker.post(job)) return;
  {
    const std::lock_guard lock(cache->mutex);
    cache->fitting = false;
  }
  complete(fit_busy("The model fit queue is full; retry shortly"));
}

std::shared_ptr<SurfaceFits> surface_cache(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  using Key = std::weak_ptr<const analytics::UnderlyingMetrics>;
  static std::mutex mutex;
  static std::map<Key, std::shared_ptr<SurfaceFits>, std::owner_less<Key>> cache;
  const std::lock_guard lock(mutex);
  std::erase_if(cache, [](const auto& entry) { return entry.first.expired(); });
  auto& entry = cache[Key(metrics)];
  if (!entry) entry = std::make_shared<SurfaceFits>();
  return entry;
}

analytics::VolatilityMetrics cached_volatility(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  const auto cache = surface_cache(metrics);
  const std::lock_guard lock(cache->mutex);
  if (!cache->volatility) {
    while (cache->fits.size() < metrics->slices.size())
      cache->fits.push_back(analytics::fit_svi(metrics->slices[cache->fits.size()]));
    cache->volatility = analytics::volatility_metrics(*metrics, cache->fits);
  }
  return *cache->volatility;
}
}  // namespace openport::server
