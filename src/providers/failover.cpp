#include "openport/providers/failover.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace openport::providers {
namespace {
bool healthy(md::FeedState state) { return state == md::FeedState::Live || state == md::FeedState::Delayed; }
bool failing(md::FeedState state) {
  return state == md::FeedState::Stale || state == md::FeedState::Error || state == md::FeedState::Stopped;
}
}  // namespace

FailoverProvider::FailoverProvider(std::unique_ptr<md::Provider> primary, std::unique_ptr<md::Provider> backup,
                                   Options options)
    : primary_(std::move(primary)), backup_(std::move(backup)), options_(std::move(options)) {
  if (!primary_ || !backup_) throw std::invalid_argument("failover: needs a primary and a backup provider");
  if (primary_->name() == backup_->name()) throw std::invalid_argument("failover: the backup must be another provider");
  if (primary_->market_controls() || backup_->market_controls())
    throw std::invalid_argument("failover: simulated feeds cannot fail over");
  if (options_.after.count() < 1) throw std::invalid_argument("failover: --fallback-seconds must be positive");
  name_ = std::string(primary_->name()) + "+" + std::string(backup_->name());
}

FailoverProvider::~FailoverProvider() { stop(); }

md::Capabilities FailoverProvider::capabilities() const noexcept {
  const auto primary = primary_->capabilities();
  const auto backup = backup_->capabilities();
  auto result = primary;
  // Health is reported per underlying, naming the feed; quotes alone prove nothing.
  result.realtime = false;
  result.realtime_plan_dependent = true;
  result.poll_interval = std::max(primary.poll_interval, backup.poll_interval);
  if (on_backup_count_.load() > 0) result.delay = std::max(primary.delay, backup.delay);
  result.trades = primary.trades || backup.trades;
  result.open_interest = primary.open_interest || backup.open_interest;
  result.vendor_greeks = primary.vendor_greeks || backup.vendor_greeks;
  result.history = primary.history || backup.history;
  return result;
}

void FailoverProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  {
    const std::lock_guard lock(mutex_);
    if (sink_) throw std::logic_error("failover: start may be called only once");
    sink_ = &sink;
    subscription_ = subscription;
    started_ = options_.clock();
    for (const auto& underlying : subscription.underlyings) routes_.try_emplace(underlying);
  }
  stopping_ = false;
  primary_->start(subscription, primary_feed_);
  if (options_.monitor) monitor_ = std::thread(&FailoverProvider::run_monitor, this);
}

void FailoverProvider::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (monitor_.joinable()) monitor_.join();
  // The primary's thread may be starting the backup; it finishes before this returns.
  primary_->stop();
  backup_->stop();
  const std::lock_guard lock(mutex_);
  if (!sink_) return;
  for (const auto& [underlying, route] : routes_)
    sink_->publish(md::ProviderStatus{options_.clock(), md::FeedState::Stopped, name_ + " " + underlying + ": stopped",
                                      underlying, std::string(route.active == kPrimary ? primary_->name() : backup_->name())});
  sink_ = nullptr;
}

bool FailoverProvider::on_backup(const std::string& underlying) const {
  const std::lock_guard lock(mutex_);
  const auto it = routes_.find(underlying);
  return it != routes_.end() && it->second.active == kBackup;
}

md::Timestamp FailoverProvider::quiet_after(Source source) const {
  // The engine's own staleness rule: three poll intervals, at least a minute.
  const auto poll = (source == kPrimary ? primary_ : backup_)->capabilities().poll_interval;
  return std::max<md::Timestamp>(3 * poll.count(), 60) * md::kNanosPerSecond;
}

bool FailoverProvider::active(Source source, const std::string& underlying) const {
  const auto it = routes_.find(underlying);
  return it == routes_.end() ? source == kPrimary : it->second.active == source;
}

void FailoverProvider::define(md::InstrumentId id) {
  if (published_[id]) return;
  published_[id] = true;
  sink_->publish(md::ContractDefinition{id, contracts_[id]});
}

void FailoverProvider::receive(Source source, md::Event event) {
  bool start = false;
  {
    const std::lock_guard lock(mutex_);
    if (!sink_ || stopping_) return;
    const auto now = options_.clock();
    auto& cache = caches_[source];
    // A streaming primary that does not report its own health proves it with data,
    // as the engine would credit it.
    const auto primary = primary_->capabilities();
    const bool proves_with_data = primary.poll_interval.count() == 0 && !primary.realtime_plan_dependent;
    const auto credit = [&](const std::string& underlying) {
      if (source != kPrimary || !proves_with_data) return;
      const auto it = routes_.find(underlying);
      if (it == routes_.end()) return;
      auto& route = it->second;
      route.primary_success = now;
      route.primary_bad_since = 0;
      route.primary_state = primary.delay.count() > 0 ? md::FeedState::Delayed : md::FeedState::Live;
      route.primary_message = std::string(primary_->name()) + " " + underlying + ": receiving quotes";
      if (route.active == kBackup) evaluate(underlying, route, now);
      if (route.active == kPrimary && route.reported != route.primary_state) report(underlying, route, now);
    };
    std::visit([&](auto& value) {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, md::ContractDefinition>) {
        const auto [it, added] = ids_.try_emplace(value.contract.osi_symbol(), static_cast<md::InstrumentId>(contracts_.size()));
        if (added) {
          contracts_.push_back(value.contract);
          published_.push_back(false);
        }
        cache.ids[value.id] = it->second;
        if (active(source, value.contract.underlying)) define(it->second);
      } else if constexpr (requires { value.id; }) {
        const auto mapped = cache.ids.find(value.id);
        if (mapped == cache.ids.end()) return;  // data before its definition: never forwarded
        value.id = mapped->second;
        const auto underlying = contracts_[value.id].underlying;
        if constexpr (std::is_same_v<T, md::OptionQuote>) {
          cache.quotes[value.id] = value;
          credit(underlying);
        } else if constexpr (std::is_same_v<T, md::OpenInterest>) {
          cache.interest[value.id] = value;
        } else if constexpr (std::is_same_v<T, md::VendorGreeks>) {
          cache.greeks[value.id] = value;
        }
        if (!active(source, underlying)) return;
        define(value.id);
        sink_->publish(value);
      } else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) {
        cache.spots[value.symbol] = value;
        credit(value.symbol);
        if (active(source, value.symbol)) sink_->publish(value);
      } else if constexpr (std::is_same_v<T, md::UnderlyingClose>) {
        if (active(source, value.symbol)) sink_->publish(value);
      } else if constexpr (std::is_same_v<T, md::SnapshotComplete>) {
        if (!active(source, value.underlying)) return;
        if (const auto it = routes_.find(value.underlying); it != routes_.end()) vouch(value.underlying, it->second);
        sink_->publish(value);
      } else if constexpr (std::is_same_v<T, md::SnapshotHeartbeat>) {
        if (active(source, value.underlying)) sink_->publish(value);
      } else if constexpr (std::is_same_v<T, md::TradingHalt>) {
        if (source == kPrimary) sink_->publish(value);
      } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
        status(source, value);
      }
    }, event);
    start = want_backup_ && !backup_started_;
    if (start) backup_started_ = true;
  }
  if (start) start_backup();
}

void FailoverProvider::status(Source source, const md::ProviderStatus& update) {
  const auto now = options_.clock();
  std::vector<std::string> targets;
  if (update.underlying.empty()) {
    for (const auto& [underlying, route] : routes_) targets.push_back(underlying);
  } else if (routes_.contains(update.underlying)) {
    targets.push_back(update.underlying);
  } else {
    // Not subscribed here: pass the primary's word on unchanged.
    if (source == kPrimary) {
      auto copy = update;
      copy.feed = std::string(primary_->name());
      sink_->publish(std::move(copy));
    }
    return;
  }
  for (const auto& underlying : targets) {
    auto& route = routes_.at(underlying);
    if (source == kPrimary) {
      route.primary_state = update.state;
      route.primary_message = update.message;
      if (healthy(update.state)) {
        route.primary_success = now;
        route.primary_bad_since = 0;
      } else if (failing(update.state) && route.primary_bad_since == 0) {
        route.primary_bad_since = now;
      }
    } else {
      route.backup_state = update.state;
      route.backup_message = update.message;
    }
    // A streaming feed's healthy status vouches for what it now prices.
    if (healthy(update.state) && route.active == source) vouch(underlying, route);
    const auto before = route.active;
    evaluate(underlying, route, now);
    // A switch has reported already; otherwise pass the active feed's word on.
    if (route.active == before && route.active == source) report(underlying, route, now);
  }
}

void FailoverProvider::evaluate(const std::string& underlying, Route& route, md::Timestamp now) {
  const auto quiet = quiet_after(kPrimary);
  if (route.active == kPrimary) {
    auto bad = route.primary_bad_since;
    if (bad == 0) {
      // Silence counts too: no healthy update for longer than the engine allows.
      const auto since = route.primary_success > 0 ? route.primary_success : started_;
      if (now - since > quiet) bad = since + quiet;
    }
    if (bad > 0 && now - bad >= options_.after.count() * md::kNanosPerSecond) switch_to(underlying, route, kBackup, now);
  } else if (route.primary_bad_since == 0 && healthy(route.primary_state) && route.primary_success > route.switched &&
             now - route.primary_success <= quiet) {
    switch_to(underlying, route, kPrimary, now);
  }
}

void FailoverProvider::switch_to(const std::string& underlying, Route& route, Source source, md::Timestamp now) {
  route.active = source;
  route.switched = now;
  if (source == kBackup) {
    ++on_backup_count_;
    want_backup_ = true;
  } else {
    --on_backup_count_;
  }
  // The status first, so everything after it is known to come from this feed.
  report(underlying, route, now, source == kPrimary ? std::string(primary_->name()) + " recovered; " : std::string{});
  // Then the new feed's latest values. Contracts it does not price keep their old,
  // aging quotes until it vouches for its chain, when vouch() clears them.
  auto& cache = caches_[source];
  if (const auto spot = cache.spots.find(underlying); spot != cache.spots.end()) sink_->publish(spot->second);
  route.unpriced.clear();
  for (md::InstrumentId id = 0; id < contracts_.size(); ++id) {
    if (contracts_[id].underlying != underlying) continue;
    const auto quote = cache.quotes.find(id);
    if (quote == cache.quotes.end()) {
      if (published_[id]) route.unpriced.insert(id);
      continue;
    }
    define(id);
    sink_->publish(quote->second);
    if (const auto it = cache.interest.find(id); it != cache.interest.end()) sink_->publish(it->second);
    if (const auto it = cache.greeks.find(id); it != cache.greeks.end()) sink_->publish(it->second);
  }
}

void FailoverProvider::vouch(const std::string& underlying, Route& route) {
  (void)underlying;
  const auto& cache = caches_[route.active];
  for (const auto id : route.unpriced) {
    // Cleared without inventing a market time: this feed has never priced it.
    if (!cache.quotes.contains(id)) sink_->publish(md::OptionQuote{id, 0, 0, 0, 0, 0});
  }
  route.unpriced.clear();
}

void FailoverProvider::report(const std::string& underlying, Route& route, md::Timestamp now, std::string_view note) {
  md::ProviderStatus out{now, route.primary_state, std::string(note) + route.primary_message, underlying,
                         std::string(primary_->name())};
  if (route.active == kBackup) {
    // Data from the backup is labelled as such: healthy is Delayed, never Live.
    out.state = healthy(route.backup_state) ? md::FeedState::Delayed : route.backup_state;
    out.message = "fallback " + std::string(backup_->name()) + ": " +
                  (route.backup_message.empty() ? "connecting" : route.backup_message) + "; primary " +
                  std::string(primary_->name()) + " " + std::string(md::to_string(route.primary_state));
    out.feed = std::string(backup_->name());
  }
  route.reported = out.state;
  sink_->publish(std::move(out));
}

void FailoverProvider::start_backup() {
  if (stopping_) return;
  try {
    backup_->start(subscription_, backup_feed_);
  } catch (const std::exception&) {
    const std::lock_guard lock(mutex_);
    if (!sink_) return;
    const auto now = options_.clock();
    for (auto& [underlying, route] : routes_) {
      route.backup_state = md::FeedState::Error;
      route.backup_message = "could not start";
      if (route.active == kBackup) report(underlying, route, now);
    }
  }
}

void FailoverProvider::tick() {
  bool start = false;
  {
    const std::lock_guard lock(mutex_);
    if (!sink_ || stopping_) return;
    const auto now = options_.clock();
    for (auto& [underlying, route] : routes_) evaluate(underlying, route, now);
    start = want_backup_ && !backup_started_;
    if (start) backup_started_ = true;
  }
  if (start) start_backup();
}

void FailoverProvider::run_monitor() {
  while (true) {
    {
      std::unique_lock lock(wake_mutex_);
      if (wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_.load(); })) return;
    }
    tick();
  }
}

}  // namespace openport::providers
