#include "openport/server/engine.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/run.hpp"
#include "openport/providers/demo_feed.hpp"
#include "openport/providers/demo.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace openport::server {
namespace {
Engine::Options driver_options(md::Provider& provider, const md::Subscription& subscription, Engine::Options options) {
  // A hosted replay clears sandbox account state below, but must retain the
  // sandbox server's no-delivery policy.
  if (options.sandboxes) options.notifications.reset();
  if (auto* replay = dynamic_cast<providers::ReplayProvider*>(&provider)) {
    options.replay = true;
    options.replay_start = replay->header().started;
    // Cache even when a caller supplied a display horizon: until must never discover EOF on the control path.
    if (options.paper_enabled) options.replay_end = replay->end_time(subscription);
    // Named accounts and visitor sandboxes belong to the live feed, never a replay's copy of its options.
    options.paper_accounts.clear();
    options.sandboxes.reset();
    if (options.run_input.empty()) options.run_input = recording_input(replay->file());
    options.candles = std::make_shared<CandleStore>();
    options.clock = [replay] { return replay->time(); };
  }
  if (options.sandboxes && (provider.name() != "demo" || !options.paper_enabled || options.paper_journal.empty()))
    throw std::invalid_argument("Sandboxes require --provider demo and a paper journal");
  if (auto* demo = dynamic_cast<providers::DemoProvider*>(&provider)) {
    options.clock = [demo] { return demo->time(); };
    options.demo_dividends = options.demo_dividends && options.dividends.empty() && demo->revision() >= 4;
  }
  if (options.replay || provider.name().starts_with("replay") || provider.name() == "demo") options.series.reset();
  if (options.sandboxes || (options.notifications && !options.notifications->includes_simulated() &&
      (options.replay || provider.name().starts_with("replay") || providers::simulated_provider(provider.name()))))
    options.notifications.reset();
  return options;
}
}  // namespace

Engine::Engine(md::Provider& provider, md::Subscription subscription, Options options)
    : provider_(provider), subscription_(std::move(subscription)), options_(driver_options(provider, subscription_, options)),
      configured_notifications_(options.sandboxes ? nullptr : options.notifications),
      queue_(md::kEventQueueCapacity, options.paper_enabled || options.replay),
      desk_(std::string(provider.name()), provider.capabilities(), subscription_, options_) {
  replay_ = dynamic_cast<providers::ReplayProvider*>(&provider_);
  if (replay_) replay_->set_driver([this](providers::ReplayBatch batch) { return consume_replay(std::move(batch)); },
                                  [this] { return synchronize(); });
  demo_ = dynamic_cast<providers::DemoProvider*>(&provider_) != nullptr;
  if (auto* demo = dynamic_cast<providers::DemoProvider*>(&provider_))
    demo->set_driver([this](providers::ReplayBatch batch) {
      if (recorder_) for (const auto& event : batch.events) recorder_->publish(event);
      return consume_replay(std::move(batch));
    });
  // Replays and the demo market never write the volatility history.
  if (options_.replay || provider.name().starts_with("replay") || provider.name() == "demo") options_.series.reset();
  status_.provider = std::string(provider.name());
  status_.capabilities = provider.capabilities();
  status_.trading.fee_per_contract = options_.paper.fee_per_contract;
  status_.trading.initial_cash = options_.paper.initial_cash;
  for (const auto& symbol : subscription_.underlyings) status_.underlyings.try_emplace(symbol);
  status_.circuit_breaker = desk_.breaker();
  health_ = status_.underlyings;
  if (options_.notifications) desk_.set_publication_sink([this](std::string_view account, const TradingView& view) {
    const auto id = notification_account(account);
    notification_accounts_.insert(id);
    options_.notifications->observe(id, view, replay_ && replay_->fast_forwarding());
  });
  if (options_.notifications) desk_.set_removal_sink([this](const std::string& account) {
    const auto id = notification_account(account);
    options_.notifications->remove_account(id);
    notification_accounts_.erase(id);
  });
}

std::string Engine::notification_account(std::string_view account) const {
  if (options_.replay || provider_.name().starts_with("replay"))
    return "replay/" + (options_.run_id.empty() ? std::string("main") : options_.run_id) + "/" + std::string(account);
  if (providers::simulated_provider(provider_.name())) return "demo/" + std::string(account);
  return std::string(account);
}

void Engine::set_dividends(std::vector<trading::Dividend> dividends) {
  const std::lock_guard lock(dividends_mutex_);
  pending_dividends_ = std::move(dividends);
}

Engine::~Engine() { stop(); }

void Engine::start() {
  if (started_)
    throw std::logic_error("Engine::start may be called only once; construct a new Engine");
  started_ = true;
  stopping_ = false;
  try {
    if (auto* demo = dynamic_cast<providers::DemoProvider*>(&provider_)) {
      // Recover all accounts before selecting the demo's first market day.
      desk_.start_trading();
      demo->start_after(desk_.market_time());
    }
    {
      const std::lock_guard lock(mutex_);
      status_.started = options_.clock();
      started_at_ = options_.monotonic_clock();
    }
    if (!options_.record_file.empty()) {
      auto recording = options_.recording;
      recording.clock = options_.clock;
      recorder_ = std::make_unique<md::RecordingSink>(
          options_.record_file, md::RecordingHeader{std::string(provider_.name()),
          provider_.capabilities(), subscription_, status_.started, false, provider_.market_controls()}, queue_, std::move(recording));
    }
    provider_started_ = true;
    provider_.start(subscription_, recorder_ ? static_cast<md::EventSink&>(*recorder_) : queue_);
    // Complete journal startup before returning so the daemon can report failures
    // even if constructing or binding its web server subsequently fails.
    if (!demo_) desk_.start_trading();
    publish_desk();
    { const std::lock_guard lock(command_mutex_); accepting_commands_ = options_.paper_enabled; }
    thread_ = options_.launch([this] { run(); });
    if (options_.series) {
      series_worker_ = std::make_unique<SeriesWorker>(*this, *options_.series);
      series_worker_->start();
    }
  } catch (...) {
    stopping_ = true;
    if (series_worker_) series_worker_->stop();
    // Even a partially started provider must stop before the queue can be destroyed.
    if (provider_started_) provider_.stop();
    if (thread_.joinable()) thread_.join();
    provider_started_ = false;
    if (recorder_) recorder_->close();
    desk_.stop();
    {
      const std::lock_guard lock(command_mutex_);
      accepting_commands_ = false;
    }
    throw;
  }
}

void Engine::stop() {
  if (series_worker_) series_worker_->stop();
  if (provider_started_) {
    provider_.stop();
    provider_started_ = false;
  }
  if (recorder_) recorder_->close();
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
  if (options_.notifications) {
    for (const auto& account : notification_accounts_) options_.notifications->remove_account(account, false);
    notification_accounts_.clear();
  }
  const std::lock_guard lock(mutex_);
  if (status_.trading.enabled && status_.trading.reason.empty()) status_.trading.reason = "ENGINE_STOPPING";
  status_.trading.write = "disabled";
}

md::RecordingStats Engine::recording_stats() const {
  return recorder_ ? recorder_->stats() : md::RecordingStats{};
}

std::string Engine::recording_error() const {
  return recorder_ ? recorder_->error() : std::string{};
}

std::vector<std::string> Engine::symbols() const {
  const std::lock_guard lock(mutex_);
  std::vector<std::string> out;
  for (const auto& [symbol, health] : status_.underlyings) out.push_back(symbol);
  return out;
}

std::shared_ptr<const analytics::UnderlyingMetrics> Engine::metrics(const std::string& symbol) const {
  const std::lock_guard lock(mutex_);
  const auto it = metrics_.find(symbol);
  return it == metrics_.end() ? nullptr : it->second;
}

EngineStatus Engine::status() const {
  const std::lock_guard lock(mutex_);
  EngineStatus out = status_;
  if (started_at_) out.uptime = options_.monotonic_clock() - *started_at_;
  out.capabilities = provider_.capabilities();  // a broker can revise its request-budgeted interval
  const auto queue = queue_.status();
  out.queue_depth = queue.depth;
  out.coalesced_events = queue.coalesced;
  out.dropped_events = queue.dropped;
  out.overloaded = queue.overloaded;
  const auto now = options_.clock();
  const auto stale_after =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::max(3 * out.capabilities.poll_interval, std::chrono::seconds(60)))
          .count();
  auto severity = [](md::FeedState state) {
    switch (state) {
      case md::FeedState::Stale:
        return 5;
      case md::FeedState::Error:
        return 4;
      case md::FeedState::Connecting:
        return 3;
      case md::FeedState::Stopped:
        return 2;
      case md::FeedState::Delayed:
        return 1;
      case md::FeedState::Live:
        return 0;
    }
    return 5;
  };
  int worst = -1;
  out.feed_message.clear();
  std::vector<std::string> messages;
  for (auto& [symbol, health] : out.underlyings) {
    const auto since = health.last_success > 0 ? health.last_success : out.started;
    if (health.state != md::FeedState::Stopped && since > 0 && now - since > stale_after) {
      health.state = md::FeedState::Stale;
      health.message = symbol + ": no successful update within " +
                       std::to_string(stale_after / md::kNanosPerSecond) + " seconds";
    }
    if (severity(health.state) > worst) {
      worst = severity(health.state);
      out.feed_state = health.state;
    }
    const auto message = health.message.empty() ? symbol + ": connecting" : health.message;
    if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
      messages.push_back(message);
      if (!out.feed_message.empty()) out.feed_message += "; ";
      out.feed_message += message;
    }
  }
  // Persist storage failures even if subsequent market events report healthy.
  if (const auto error = recording_error(); !error.empty()) {
    out.feed_state = md::FeedState::Error;
    out.feed_message = error;
    for (auto& [symbol, health] : out.underlyings) {
      health.state = md::FeedState::Error;
      health.message = error;
      health.last_error = error;
    }
  }
  return out;
}

void Engine::update_health(const md::Event& event, md::Timestamp received) {
  if (const auto* feed = std::get_if<md::ProviderStatus>(&event)) {
    auto update = [&](UnderlyingHealth& health) {
      if (health.state != feed->state || health.message != feed->message) {
        health_changed_ = true;
        health.state = feed->state;
        health.message = feed->message;
      }
      if (feed->state == md::FeedState::Live || feed->state == md::FeedState::Delayed) {
        health.last_success = received;
      } else if (feed->state == md::FeedState::Error) {
        health.last_error = feed->message;
        health.last_error_time = received;
      }
    };
    if (feed->underlying.empty()) {
      for (auto& [symbol, health] : health_) update(health);
    } else {
      const auto [it, inserted] = health_.try_emplace(feed->underlying);
      health_changed_ = health_changed_ || inserted;
      update(it->second);
    }
    feed_updated_ = received;
    health_dirty_ = true;
  } else if (status_.capabilities.poll_interval == std::chrono::seconds(0)) {
    // Streaming feeds prove liveness with quotes. Snapshot feeds report success
    // after the whole poll, including unchanged or empty chains.
    const std::string* symbol = nullptr;
    if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
      if (const auto* option = desk_.book().option(quote->id)) symbol = &option->contract.underlying;
    } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
      symbol = &spot->symbol;
    }
    if (symbol) {
      auto it = health_.find(*symbol);
      if (it == health_.end()) return;
      auto& health = it->second;
      const auto state =
          status_.capabilities.delay.count() > 0 ? md::FeedState::Delayed : md::FeedState::Live;
      // Plan-dependent streams report entitlement and unknown timing explicitly.
      if (!status_.capabilities.realtime_plan_dependent && health.state != state) {
        health_changed_ = true;
        health.state = state;
        health.message = status_.provider + " " + *symbol + ": receiving quotes";
      }
      health.last_success = received;
      feed_updated_ = received;
      health_dirty_ = true;
    }
  }
}

std::future<void> Engine::synchronize() {
  std::promise<void> done;
  auto future = done.get_future();
  const std::lock_guard lock(sync_mutex_);
  synchronizations_.push_back(std::move(done));
  queue_.wake();
  return future;
}

std::future<void> Engine::consume_replay(providers::ReplayBatch batch) {
  PendingReplay pending{std::move(batch), {}};
  auto result = pending.done.get_future();
  { const std::lock_guard lock(command_mutex_); replay_batches_.push_back(std::move(pending)); }
  queue_.wake();
  return result;
}

void Engine::run() {
  std::vector<md::Event> batch;
  auto last_analytics = std::chrono::steady_clock::now();
  last_rate_time_ = last_analytics;
  auto last_health = options_.monotonic_clock();
  constexpr auto kHealthInterval = std::chrono::milliseconds(100);
  md::Date dividend_date;
  const auto dividend_first = md::trading_date(desk_.market_time() > 0 ? desk_.market_time() : options_.clock());

  while (true) {
    if (stopping_ && queue_.status().depth == 0) {
      const std::lock_guard lock(command_mutex_);
      if (replay_batches_.empty()) break;
    }
    std::vector<std::promise<void>> synchronized;
    {
      const std::lock_guard lock(sync_mutex_);
      synchronized.swap(synchronizations_);
    }
    if (options_.sandboxes) desk_.expire_sandboxes(options_.sandboxes->expired());
    batch.clear();
    queue_.drain(batch, std::chrono::milliseconds(synchronized.empty() ? 50 : 0));
    std::deque<PendingCommand> commands;
    std::deque<PendingReplay> replay_batches;
    {
      const std::lock_guard lock(command_mutex_);
      commands.swap(commands_);
      replay_batches.swap(replay_batches_);
    }
    {
      const std::lock_guard lock(dividends_mutex_);
      if (pending_dividends_) {
        options_.demo_dividends = false;
        desk_.set_dividends(std::move(*pending_dividends_));
        pending_dividends_.reset();
      }
    }
    const auto received = options_.clock();
    if (replay_ || demo_) {
      for (auto& command : commands) {
        const auto market_time = replay_ ? replay_->market_time() : desk_.market_time();
        desk_.command(std::move(command.command), std::move(command.completion), market_time,
                      demo_ ? market_time : received);
      }
      for (auto& pending : replay_batches) {
        try {
          if (demo_ && options_.demo_dividends) {
            const auto date = md::trading_date(pending.batch.time);
            if (date != dividend_date) {
              // Install before rollover and analytics; keep payments since recovery
              // and a year's lookahead for assignment and ex-dividend warnings.
              desk_.set_dividends(providers::demo_dividends(dividend_first,
                  md::date_from_days(md::days_since_epoch(date) + 366)));
              dividend_date = date;
            }
          }
          desk_.replay_batch(pending.batch.events, pending.batch.received, pending.batch.time);
          for (const auto& event : pending.batch.events) update_health(event, pending.batch.received);
          events_ += pending.batch.events.size();
          publish_desk();
          pending.done.set_value();
        } catch (...) { pending.done.set_exception(std::current_exception()); }
      }
      for (const auto& event : batch) update_health(event, received);
      if (!synchronized.empty() || (replay_ && replay_->paused() && !replay_->fast_forwarding() && !replay_->stepping()))
        desk_.flush_journals();
      publish_desk();
      for (auto& done : synchronized) done.set_value();
      continue;
    }
    for (const md::Event& event : batch) {
      desk_.observe(event);
      ++events_;
      update_health(event, received);
    }
    const auto health_now = options_.monotonic_clock();
    const bool publish_health =
        health_dirty_ && (health_changed_ || health_now - last_health >= kHealthInterval);
    if (!batch.empty() || publish_health) {
      const std::lock_guard lock(mutex_);
      status_.events = events_;
      if (publish_health) {
        if (health_changed_) {
          status_.underlyings = health_;
        } else {
          // Quote receipt changes only timestamps; reuse published nodes and strings.
          for (const auto& [symbol, health] : health_) {
            auto& published = status_.underlyings.at(symbol);
            published.last_success = health.last_success;
            published.last_error_time = health.last_error_time;
          }
        }
        status_.feed_updated = feed_updated_;
        last_health = health_now;
        health_dirty_ = false;
        health_changed_ = false;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    const bool ended = std::any_of(batch.begin(), batch.end(), [](const md::Event& event) {
      const auto* status = std::get_if<md::ProviderStatus>(&event);
      return status && status->state == md::FeedState::Stopped;
    });
    if (ended || stopping_ || !synchronized.empty() || !commands.empty() ||
        (!batch.empty() && desk_.active()) ||
        now - last_analytics >= options_.analytics_interval) {
      last_analytics = now;
      refresh_analytics();
    }
    // Each account catches its own failures; nothing else here can fail the others.
    if (stopping_) desk_.halt();
    desk_.update_trading(batch, commands, options_.clock());
    for (auto& command : commands) {
      desk_.apply_command(command, desk_.market_time(), options_.clock());
    }
    publish_desk();
    if (options_.notifications && !stopping_) {
      const auto stale_after = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::max(3 * provider_.capabilities().poll_interval, std::chrono::seconds(60))).count();
      for (const auto& [symbol, health] : health_) {
        const auto since = health.last_success > 0 ? health.last_success : status_.started;
        const bool stalled = health.state == md::FeedState::Stale || health.state == md::FeedState::Stopped ||
            (since > 0 && received - since > stale_after);
        if (stalled && stalled_.insert(symbol).second)
          options_.notifications->publish({"feed_stalled", "", desk_.market_time(),
              symbol + ": feed stalled; check the terminal's feed status.", {{"underlying", symbol}}});
        if (!stalled) stalled_.erase(symbol);
      }
    }
    for (auto& done : synchronized) done.set_value();
  }
  std::deque<PendingCommand> remaining;
  {
    const std::lock_guard lock(command_mutex_);
    accepting_commands_ = false;
    remaining.swap(commands_);
  }
  desk_.halt();
  for (auto& command : remaining) desk_.apply_command(command, desk_.market_time(), options_.clock());
  refresh_analytics();
  // Release the exclusive journal writers on their owner thread. Published values
  // remain readable, and a replacement Engine can recover as soon as stop returns.
  desk_.stop();
  publish_desk();
}

void Engine::publish_desk() {
  const std::lock_guard lock(mutex_);
  if (replay_ || demo_) {
    // Publish a driven batch's health with its account/analytics views, before
    // acknowledging it. Otherwise a new day's account can be visible beside the
    // previous day's health, which status() correctly considers stale.
    status_.events = events_;
    status_.underlyings = health_;
    status_.feed_updated = feed_updated_;
  }
  metrics_ = desk_.publications();
  trading_views_ = desk_.views();
  status_.trading = desk_.trading_status();
  status_.accounts = desk_.accounts();
  status_.circuit_breaker = desk_.breaker();
  status_.contracts = desk_.book().contracts();
  status_.nonstandard_contracts = desk_.book().nonstandard_contracts();
}
void Engine::refresh_analytics() {
  const auto started = std::chrono::steady_clock::now();
  const bool recomputed = desk_.refresh_analytics();
  publish_desk();
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_rate_time_).count();
  const std::lock_guard lock(mutex_);
  if (recomputed) status_.analytics_ms = std::chrono::duration<double, std::milli>(now - started).count();
  if (elapsed >= 1.0) {
    status_.events_per_second = static_cast<double>(events_ - events_at_last_rate_) / elapsed;
    events_at_last_rate_ = events_;
    last_rate_time_ = now;
  }
}

}  // namespace openport::server
