#include "openport/server/engine.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/run.hpp"
#include "openport/providers/demo_feed.hpp"
#include "openport/providers/demo.hpp"
#include "openport/providers/failover.hpp"
#include "openport/providers/factory.hpp"
#include "openport/md/contract.hpp"
#include "openport/trading/types.hpp"

#include <algorithm>
#include <iterator>
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
  // Every engine keeps its own tape: a replay must never show the live feed's prints.
  options.flow = std::make_shared<FlowTape>(options.flow ? options.flow->options() : FlowTape::Options{});
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
/// An event's own market-data time; zero for a definition or a feed status, whose
/// time is not market data.
md::Timestamp stamped_at(const md::Event& event) {
  return std::visit([](const auto& value) -> md::Timestamp {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::ContractDefinition> || std::is_same_v<T, md::ProviderStatus>) return 0;
    else return value.ts;
  }, event);
}
/// What an event is, for a feed error, and the underlying it names; none for an option's.
std::pair<const char*, std::string> described(const md::Event& event) {
  return std::visit([](const auto& value) -> std::pair<const char*, std::string> {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::OptionQuote>) return {"an option quote", {}};
    else if constexpr (std::is_same_v<T, md::OptionTrade>) return {"an option trade", {}};
    else if constexpr (std::is_same_v<T, md::OptionVolume>) return {"option volume", {}};
    else if constexpr (std::is_same_v<T, md::OpenInterest>) return {"open interest", {}};
    else if constexpr (std::is_same_v<T, md::VendorGreeks>) return {"vendor Greeks", {}};
    else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) return {"an underlying quote", value.symbol};
    else if constexpr (std::is_same_v<T, md::UnderlyingClose>) return {"a close", value.symbol};
    else if constexpr (std::is_same_v<T, md::SnapshotComplete>) return {"a snapshot", value.underlying};
    else if constexpr (std::is_same_v<T, md::SnapshotHeartbeat>) return {"a heartbeat", value.underlying};
    else if constexpr (std::is_same_v<T, md::TradingHalt>) return {"a halt", value.underlying};
    else return {"an event", {}};
  }, event);
}
/// Whether an underlying last updated at `since` has gone more than `stale_after`
/// without an update at `now`. A polled feed answers every poll, open market or not. A
/// stream proves itself only with quotes, which stop when its market closes: it is
/// overdue only while the market it shows is open and has been for `stale_after`, so a
/// close, a weekend or a holiday is not a stall, and the first quote of a session has
/// `stale_after` from the open to arrive.
bool overdue(std::string_view symbol, md::Timestamp since, md::Timestamp now, md::Timestamp stale_after,
             const md::Capabilities& capabilities) {
  if (since <= 0 || now - since <= stale_after) return false;
  if (capabilities.poll_interval > std::chrono::seconds(0)) return true;
  // A delayed stream shows the market as it was `delay` ago.
  const md::Timestamp shown = now - std::chrono::duration_cast<std::chrono::nanoseconds>(capabilities.delay).count();
  return md::trading_session(symbol, shown).open && md::trading_session(symbol, shown - stale_after).open;
}
}  // namespace

void Engine::Ingress::publish(md::Event event) {
  const auto stamped = stamped_at(event);
  if (stamped <= md::now() + kMaxEventLead) {
    // A recording keeps the underlyings its header names, those the engine started with,
    // so a replay of it plays a fixed set: one added while running goes past the recorder.
    if (extended_.load(std::memory_order_acquire) && next != &owner_.queue_ && !recorded(event))
      owner_.queue_.publish(std::move(event));
    else
      next->publish(std::move(event));
    return;
  }
  const auto [kind, underlying] = described(event);
  const auto at = owner_.options_.clock();
  const std::lock_guard lock(owner_.refusal_mutex_);
  auto& refusal = owner_.refusal_;
  ++refusal.count;
  refusal.message = std::string(owner_.provider_.name()) + ": refused " + kind + " stamped " +
                    md::format_timestamp(stamped) + ", more than 5 minutes after the system clock; " +
                    std::to_string(refusal.count) + (refusal.count == 1 ? " event" : " events") + " refused so far";
  refusal.underlying = underlying;
  refusal.time = at;
}

bool Engine::Ingress::recorded(const md::Event& event) {
  const auto& started = owner_.subscription_.underlyings;
  const auto carried = [&](const std::string& symbol) {
    return std::find(started.begin(), started.end(), symbol) != started.end();
  };
  // As ReplayBatches admits events: definitions by their contract's underlying, the
  // other option events by the contract a definition named.
  return std::visit([&](const auto& value) -> bool {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::ContractDefinition>) {
      if (carried(value.contract.underlying)) return true;
      const std::lock_guard lock(ids_mutex_);
      unrecorded_.insert(value.id);
      return false;
    } else if constexpr (std::is_same_v<T, md::UnderlyingQuote> || std::is_same_v<T, md::UnderlyingClose>) {
      return carried(value.symbol);
    } else if constexpr (std::is_same_v<T, md::ProviderStatus> || std::is_same_v<T, md::TradingHalt>) {
      return value.underlying.empty() || carried(value.underlying);
    } else if constexpr (std::is_same_v<T, md::SnapshotComplete> || std::is_same_v<T, md::SnapshotHeartbeat>) {
      return carried(value.underlying);
    } else {
      const std::lock_guard lock(ids_mutex_);
      return !unrecorded_.contains(value.id);
    }
  }, event);
}

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
      // The demo hands its batches to the engine here, never to the provider's sink:
      // whichever recorder is active records them here.
      md::EventSink* recorder = daily_recorder_ ? static_cast<md::EventSink*>(daily_recorder_.get()) : recorder_.get();
      if (recorder) for (const auto& event : batch.events) recorder->publish(event);
      return consume_replay(std::move(batch));
    });
  // Replays and the demo market never write the volatility history.
  if (options_.replay || provider.name().starts_with("replay") || provider.name() == "demo") options_.series.reset();
  status_.provider = std::string(provider.name());
  if (const auto* failover = dynamic_cast<providers::FailoverProvider*>(&provider_))
    status_.fallback = FallbackInfo{std::string(failover->primary_name()), std::string(failover->backup_name()), failover->after()};
  status_.capabilities = provider.capabilities();
  status_.trading.fee_per_contract = options_.paper.fee_per_contract;
  status_.trading.initial_cash = options_.paper.initial_cash;
  for (const auto& symbol : subscription_.underlyings) status_.underlyings.try_emplace(symbol);
  status_.circuit_breaker = desk_.breaker();
  health_ = status_.underlyings;
  if (options_.notifications || options_.event_stream) desk_.set_publication_sink([this](std::string_view account, const TradingView& view) {
    const auto id = notification_account(account);
    notification_accounts_.insert(id);
    if (options_.notifications) options_.notifications->observe(id, view, replay_ && replay_->fast_forwarding());
    if (options_.event_stream) options_.event_stream->observe(id, view, replay_ && replay_->fast_forwarding());
  });
  if (options_.notifications || options_.event_stream) desk_.set_removal_sink([this](const std::string& account) {
    const auto id = notification_account(account);
    if (options_.notifications) options_.notifications->remove_account(id);
    if (options_.event_stream) options_.event_stream->remove_account(id);
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

void Engine::set_opening_quotations(std::vector<OpeningQuotation> quotations) {
  const std::lock_guard lock(dividends_mutex_);
  pending_quotations_ = std::move(quotations);
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
          provider_.capabilities(), subscription_, status_.started, false, provider_.market_controls(),
          provider_.trade_venues()}, queue_, std::move(recording));
    }
    if (!options_.record_dir.empty() && options_.record_file.empty()) {
      auto recording = options_.recording;
      recording.clock = options_.clock;
      daily_recorder_ = std::make_unique<md::RecordingDirectory>(options_.record_dir,
          md::RecordingHeader{std::string(provider_.name()), provider_.capabilities(), subscription_,
                              status_.started, false, provider_.market_controls(), provider_.trade_venues()}, queue_,
          md::RecordingDirectory::Options{std::move(recording), options_.record_keep_days, options_.record_max_bytes});
    }
    md::EventSink* destination = &queue_;
    if (recorder_) destination = recorder_.get();
    if (daily_recorder_) destination = daily_recorder_.get();
    ingress_.next = destination;
    provider_started_ = true;
    provider_.start(subscription_, ingress_);
    // Complete journal startup before returning so the daemon can report failures
    // even if constructing or binding its web server subsequently fails.
    if (!demo_) desk_.start_trading();
    publish_desk();
    {
      const std::lock_guard lock(command_mutex_);
      accepting_commands_ = options_.paper_enabled;
      accepting_underlyings_ = true;
    }
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
    if (daily_recorder_) daily_recorder_->close();
    desk_.stop();
    std::deque<PendingUnderlying> unapplied;
    {
      const std::lock_guard lock(command_mutex_);
      accepting_commands_ = false;
      accepting_underlyings_ = false;
      unapplied.swap(underlying_changes_);
    }
    for (auto& pending : unapplied) apply_underlying(pending);
    throw;
  }
}

void Engine::stop() {
  // Providers publish Stopped as they shut down: that is not a stalled feed.
  shutting_down_ = true;
  if (series_worker_) series_worker_->stop();
  if (provider_started_) {
    provider_.stop();
    provider_started_ = false;
  }
  if (recorder_) recorder_->close();
  if (daily_recorder_) daily_recorder_->close();
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
  if (options_.notifications || options_.event_stream) {
    for (const auto& account : notification_accounts_) {
      if (options_.notifications) options_.notifications->remove_account(account, false);
      if (options_.event_stream) options_.event_stream->remove_account(account, false);
    }
    notification_accounts_.clear();
  }
  const std::lock_guard lock(mutex_);
  if (status_.trading.enabled && status_.trading.reason.empty()) status_.trading.reason = "ENGINE_STOPPING";
  status_.trading.write = "disabled";
}

md::RecordingStats Engine::recording_stats() const {
  return daily_recorder_ ? daily_recorder_->stats() : recorder_ ? recorder_->stats() : md::RecordingStats{};
}

std::string Engine::recording_error() const {
  return daily_recorder_ ? daily_recorder_->error() : recorder_ ? recorder_->error() : std::string{};
}

std::string Engine::recording_warning() const {
  return daily_recorder_ ? daily_recorder_->warning() : std::string{};
}

void Engine::roll_recording() {
  if (daily_recorder_) daily_recorder_->rollover();
}

bool Engine::live_subscription() const {
  return provider_.live_subscription() && !replay_ && !demo_ && !options_.replay;
}

std::string Engine::fixed_subscription() const {
  const std::string name(provider_.name());
  if (demo_) return "The demo market generates its own underlyings and cannot add others";
  if (replay_ || options_.replay || name.starts_with("replay")) return "A replay plays the underlyings its recording holds";
  if (dynamic_cast<const providers::FailoverProvider*>(&provider_))
    return name + " (a feed with --fallback-provider) keeps the underlyings it started with; "
           "restart openportd with --symbols to change them";
  return name + " subscribes to its underlyings when it connects; restart openportd with --symbols to change them";
}

UnderlyingSubscription Engine::underlying_subscription() const {
  UnderlyingSubscription out;
  out.live = live_subscription();
  if (!out.live) out.reason = fixed_subscription();
  out.configured = subscription_.underlyings;
  const std::lock_guard lock(mutex_);
  for (const auto& item : added_) out.added.push_back(item.symbol);
  return out;
}

bool Engine::post_underlying(UnderlyingChange change, UnderlyingCompletion completion) {
  {
    const std::lock_guard lock(command_mutex_);
    if (!accepting_underlyings_ || underlying_changes_.size() >= kUnderlyingInbox) return false;
    underlying_changes_.push_back({std::move(change), std::move(completion)});
  }
  queue_.wake();
  return true;
}

md::Timestamp Engine::watched_since(const std::string& symbol, md::Timestamp started) const {
  for (const auto& item : added_)
    if (item.symbol == symbol) return item.at;
  return started;
}

std::vector<std::string> Engine::accounts_using(const std::string& symbol) const {
  const auto option_on = [&](const std::string& osi) {
    const auto contract = md::parse_osi(osi);
    return contract && contract->underlying == symbol;
  };
  const auto chain_on = [&](const trading::OrderRequest& request) {
    const auto names = trading::chain_symbols(request);
    return std::any_of(names.begin(), names.end(), option_on);
  };
  std::vector<std::string> users;
  for (const auto& [id, view] : desk_.views()) {
    if (!view || !view->snapshot) continue;
    const auto& s = *view->snapshot;
    // As affects_trading gathers them: what it holds, works, or watches with an alert.
    bool uses = false;
    for (const auto& p : s.positions) uses = uses || p.position.contract.underlying == symbol;
    for (const auto& order : s.open_orders) uses = uses || chain_on(order.request);
    for (const auto& stock : s.stocks) uses = uses || stock.position.symbol == symbol;
    for (const auto& order : s.stock_orders) uses = uses || (order.open() && order.request.symbol == symbol);
    for (const auto& alert : s.alerts) {
      if (uses || (!alert.armed && !alert.spec.repeat)) continue;
      const auto& c = alert.spec.condition;
      uses = (c.scope == trading::AlertScope::Underlying && c.symbol == symbol) ||
             (c.scope == trading::AlertScope::Contract && option_on(c.symbol));
      for (const auto& leg : c.legs) uses = uses || option_on(leg.symbol);
    }
    if (uses) users.push_back(id);
  }
  return users;
}

void Engine::apply_underlying(PendingUnderlying& pending) {
  UnderlyingReply reply;
  const auto& symbol = pending.change.symbol;
  const auto& configured = subscription_.underlyings;
  const bool started_with = std::find(configured.begin(), configured.end(), symbol) != configured.end();
  const bool added = std::any_of(added_.begin(), added_.end(), [&](const Added& item) { return item.symbol == symbol; });
  const auto refuse = [&](int answer, const char* reason, std::string text) {
    reply.status = answer;
    reply.code = reason;
    reply.message = std::move(text);
  };
  // What the provider polls once the change is made: every underlying but `except`.
  const auto polled = [&](const std::string& except) {
    std::vector<std::string> list = configured;
    for (const auto& item : added_)
      if (item.symbol != except) list.push_back(item.symbol);
    return list;
  };
  if (!live_subscription()) {
    refuse(409, "SUBSCRIPTION_FIXED", fixed_subscription());
  } else if (stopping_ || shutting_down_) {
    refuse(503, "ENGINE_STOPPING", "The server is stopping");
  } else if (pending.change.add) {
    if (started_with || added) {
      reply.message = symbol + " is already subscribed";
    } else if (configured.size() + added_.size() >= kMaxUnderlyings) {
      refuse(409, "UNDERLYING_LIMIT", "The server carries " + std::to_string(configured.size() + added_.size()) +
             " underlyings, the most it adds to; remove one first");
    } else {
      auto list = polled(std::string());
      list.push_back(symbol);
      std::string unsupported;
      try {
        providers::validate_subscription(provider_.name(), md::Subscription{list, subscription_.max_expiries, subscription_.strike_window});
      } catch (const std::invalid_argument& error) {
        unsupported = error.what();
      }
      if (!unsupported.empty()) {
        refuse(422, "UNDERLYING_UNSUPPORTED", unsupported);
      } else {
        ingress_.extend();
        dropped_.erase(symbol);
        UnderlyingHealth health;
        health.message = std::string(provider_.name()) + " " + symbol + ": subscribing";
        health_[symbol] = health;
        health_changed_ = true;
        health_dirty_ = true;
        {
          const std::lock_guard lock(mutex_);
          added_.push_back({symbol, options_.clock()});
          status_.underlyings[symbol] = health;
        }
        provider_.update_underlyings(list);
        if (options_.added_underlying) options_.added_underlying(symbol);
        reply.status = 201;
        reply.message = "Subscribed to " + symbol + "; its data arrives with the feed's next poll";
      }
    }
  } else if (started_with) {
    refuse(409, "UNDERLYING_CONFIGURED", symbol + " is one of the underlyings the server started with (--symbols); "
           "it stays until openportd restarts without it");
  } else if (!added) {
    refuse(404, "UNKNOWN_UNDERLYING", "The server does not carry " + symbol);
  } else if (const auto users = accounts_using(symbol); !users.empty()) {
    std::string names;
    for (const auto& id : users) names += (names.empty() ? "" : ", ") + id;
    refuse(409, "UNDERLYING_IN_USE", symbol + " has positions, working orders or alerts in " + names +
           "; close, cancel or delete them first");
  } else {
    const auto list = polled(symbol);
    // Its last poll may still report: dropped_ keeps it out of health and analytics.
    dropped_.insert(symbol);
    health_.erase(symbol);
    stalled_.erase(symbol);
    health_changed_ = true;
    health_dirty_ = true;
    {
      const std::lock_guard lock(mutex_);
      std::erase_if(added_, [&](const Added& item) { return item.symbol == symbol; });
      status_.underlyings.erase(symbol);
      metrics_.erase(symbol);
    }
    provider_.update_underlyings(list);
    reply.message = "Unsubscribed from " + symbol;
  }
  reply.subscription = underlying_subscription();
  // A disconnected client cannot take down the engine thread.
  try { pending.completion(std::move(reply)); } catch (...) {}
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
  // The recorder's own lock can be held through a frame write or a day's rollover on a
  // provider thread. Read it before taking mutex_, which the engine thread needs to
  // publish, so a status read never makes the engine wait on the disk.
  const auto recording_failure = recording_error();
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
    const auto since = health.last_success > 0 ? health.last_success : watched_since(symbol, out.started);
    // A feed that reports an error keeps its own cause (rejected credentials, a missing
    // entitlement), which says more than how long it has gone without an update.
    if (health.state != md::FeedState::Stopped && health.state != md::FeedState::Error &&
        overdue(symbol, since, now, stale_after, out.capabilities)) {
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
  // A refused event is the latest error of the underlying it names (of every one for an
  // option's), while each feed state stays the feed's own.
  {
    const std::lock_guard refusals(refusal_mutex_);
    if (refusal_.count > 0)
      for (auto& [symbol, health] : out.underlyings)
        if ((refusal_.underlying.empty() || refusal_.underlying == symbol) && refusal_.time >= health.last_error_time) {
          health.last_error = refusal_.message;
          health.last_error_time = refusal_.time;
        }
  }
  // Persist storage failures even if subsequent market events report healthy.
  if (!recording_failure.empty()) {
    out.feed_state = md::FeedState::Error;
    out.feed_message = recording_failure;
    for (auto& [symbol, health] : out.underlyings) {
      health.state = md::FeedState::Error;
      health.message = recording_failure;
      health.last_error = recording_failure;
    }
  }
  return out;
}

void Engine::update_health(const md::Event& event, md::Timestamp received) {
  if (const auto* feed = std::get_if<md::ProviderStatus>(&event)) {
    // An underlying removed while running can still report from its last poll; it stays gone.
    if (!feed->underlying.empty() && dropped_.contains(feed->underlying)) return;
    auto update = [&](UnderlyingHealth& health) {
      if (health.state != feed->state || health.message != feed->message ||
          (!feed->feed.empty() && health.feed != feed->feed)) {
        health_changed_ = true;
        health.state = feed->state;
        health.message = feed->message;
        if (!feed->feed.empty()) health.feed = feed->feed;
      }
      // A finished poll is an update whatever state it reports: a polled feed that says
      // Stale (Tradier's indexes, Schwab without isDelayed, IBKR's frozen values) cannot
      // confirm its timing, but it did answer. Streams prove themselves with quotes.
      const bool polled = status_.capabilities.poll_interval > std::chrono::seconds(0);
      if (feed->state == md::FeedState::Live || feed->state == md::FeedState::Delayed ||
          (polled && feed->state == md::FeedState::Stale)) {
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
  // Live market data drained since the accounts' last market transaction, in order.
  std::vector<md::Event> unsettled;
  auto last_analytics = std::chrono::steady_clock::now();
  last_rate_time_ = last_analytics;
  auto last_health = options_.monotonic_clock();
  constexpr auto kHealthInterval = std::chrono::milliseconds(100);
  md::Date dividend_date;
  const auto dividend_first = md::trading_date(desk_.market_time() > 0 ? desk_.market_time() : options_.clock());

  while (true) {
    if (stopping_ && queue_.status().depth == 0 && unsettled.empty()) {
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
    std::deque<PendingUnderlying> underlying_changes;
    {
      const std::lock_guard lock(command_mutex_);
      commands.swap(commands_);
      replay_batches.swap(replay_batches_);
      underlying_changes.swap(underlying_changes_);
    }
    {
      const std::lock_guard lock(dividends_mutex_);
      if (pending_dividends_) {
        options_.demo_dividends = false;
        desk_.set_dividends(std::move(*pending_dividends_));
        pending_dividends_.reset();
      }
      if (pending_quotations_) {
        desk_.set_opening_quotations(std::move(*pending_quotations_));
        pending_quotations_.reset();
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
          // A driven batch is this engine's analytics pass.
          const auto pass = std::chrono::steady_clock::now();
          const bool recomputed = desk_.replay_batch(pending.batch.events, pending.batch.received, pending.batch.time);
          for (const auto& event : pending.batch.events) update_health(event, pending.batch.received);
          events_ += pending.batch.events.size();
          publish_desk();
          publish_rates(pass, recomputed);
          pending.done.set_value();
        } catch (...) { pending.done.set_exception(std::current_exception()); }
      }
      for (const auto& event : batch) update_health(event, received);
      if (!synchronized.empty() || (replay_ && replay_->paused() && !replay_->fast_forwarding() && !replay_->stepping()))
        desk_.flush_journals();
      publish_desk();
      // The event rate falls to zero while the replay is paused or has ended.
      publish_rates(std::chrono::steady_clock::now(), false);
      for (auto& pending : underlying_changes) apply_underlying(pending);
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
    // Commands, synchronizations, the feed's end and shutdown act on the market as it
    // stands, and so does market data an account trades on at once. The rest of a
    // streaming market reaches the accounts with the next analytics pass: otherwise a
    // resting order would journal, and sync, a market transaction on every drain.
    const bool due = ended || stopping_ || !synchronized.empty() || !commands.empty() ||
                     now - last_analytics >= options_.analytics_interval ||
                     unsettled.size() + batch.size() >= md::kEventQueueCapacity ||
                     (!batch.empty() && desk_.affects_trading(batch));
    if (unsettled.empty()) unsettled.swap(batch);
    else unsettled.insert(unsettled.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
    if (due) {
      last_analytics = now;
      refresh_analytics();
    }
    // Each account catches its own failures; nothing else here can fail the others.
    if (stopping_) desk_.halt();
    if (due) {
      desk_.update_trading(unsettled, commands, options_.clock());
      unsettled.clear();
    }
    for (auto& command : commands) {
      desk_.apply_command(command, desk_.market_time(), options_.clock());
    }
    publish_desk();
    // After the pass's commands, so a removal sees the orders sent before it.
    for (auto& pending : underlying_changes) apply_underlying(pending);
    // Outbound channels and the local event stream both hear of a stall: the stream keeps
    // it in its history, so a terminal opened later can still see it happened.
    if ((options_.notifications || options_.event_stream) && !stopping_ && !shutting_down_) {
      const auto capabilities = provider_.capabilities();
      const auto stale_after = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::max(3 * capabilities.poll_interval, std::chrono::seconds(60))).count();
      for (const auto& [symbol, health] : health_) {
        const auto since = health.last_success > 0 ? health.last_success : watched_since(symbol, status_.started);
        // A Stale label is not a stall: a feed that says it while its updates arrive is
        // healthy, and one that stops updating is caught by the timeout like any other.
        // A stream's market closing is not a stall either (see overdue).
        const bool stalled = health.state == md::FeedState::Stopped ||
                             overdue(symbol, since, received, stale_after, capabilities);
        if (stalled && stalled_.insert(symbol).second) {
          const NotificationEvent stall{"feed_stalled", "", desk_.market_time(),
              symbol + ": feed stalled; check the terminal's feed status.", {{"underlying", symbol}}};
          if (options_.notifications) options_.notifications->publish(stall);
          if (options_.event_stream) options_.event_stream->publish(stall);
        }
        if (!stalled) stalled_.erase(symbol);
      }
    }
    for (auto& done : synchronized) done.set_value();
  }
  std::deque<PendingCommand> remaining;
  std::deque<PendingUnderlying> unapplied;
  {
    const std::lock_guard lock(command_mutex_);
    accepting_commands_ = false;
    accepting_underlyings_ = false;
    remaining.swap(commands_);
    unapplied.swap(underlying_changes_);
  }
  desk_.halt();
  for (auto& command : remaining) desk_.apply_command(command, desk_.market_time(), options_.clock());
  for (auto& pending : unapplied) apply_underlying(pending);
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
  for (const auto& symbol : dropped_) metrics_.erase(symbol);
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
  publish_rates(started, recomputed);
}
void Engine::publish_rates(std::chrono::steady_clock::time_point pass, bool recomputed) {
  const auto now = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(now - last_rate_time_).count();
  const std::lock_guard lock(mutex_);
  if (recomputed) status_.analytics_ms = std::chrono::duration<double, std::milli>(now - pass).count();
  if (elapsed >= 1.0) {
    status_.events_per_second = static_cast<double>(events_ - events_at_last_rate_) / elapsed;
    events_at_last_rate_ = events_;
    last_rate_time_ = now;
  }
}

}  // namespace openport::server
