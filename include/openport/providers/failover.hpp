#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "openport/md/provider.hpp"

namespace openport::providers {

/// A primary feed with a backup that takes over an underlying while the primary
/// has been in Error, Stopped or silent for longer than `after`, and hands it back
/// once the primary has been fresh again for `after`.
///
/// Freshness is separate from the state label: a Live or Delayed status, or a
/// complete snapshot whose market time advanced, proves the primary, so a feed
/// whose healthy label is Stale (timing unconfirmed) keeps its underlyings. The
/// label still reaches downstream unchanged. No underlying moves onto a backup
/// that is in Error or Stopped; its status says why it stays.
///
/// Both feeds' contracts are remapped by OSI symbol into one id space, as the demo
/// feed does across recordings. Only the active feed's events reach the engine.
/// A switch first publishes the underlying's status, naming the feed in
/// ProviderStatus::feed, then the new feed's latest values; quotes the new feed
/// does not price are cleared before its first complete snapshot or healthy
/// status can vouch for them. Quotes keep their own market times, so paper
/// trading's freshness checks apply to the backup as to any feed.
class FailoverProvider final : public md::Provider {
 public:
  struct Options {
    std::chrono::seconds after{30};
    /// Operational clock for health and switching, independent of market time.
    std::function<md::Timestamp()> clock = md::now;
    /// Check routes once a second on a thread; tests call tick() themselves.
    bool monitor = true;
  };

  FailoverProvider(std::unique_ptr<md::Provider> primary, std::unique_ptr<md::Provider> backup, Options options);
  ~FailoverProvider() override;

  /// "primary+backup", so recordings and status name both sources.
  [[nodiscard]] std::string_view name() const noexcept override { return name_; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

  /// Re-evaluates every underlying's route at the operational clock.
  void tick();

  [[nodiscard]] std::string_view primary_name() const noexcept { return primary_->name(); }
  [[nodiscard]] std::string_view backup_name() const noexcept { return backup_->name(); }
  [[nodiscard]] std::chrono::seconds after() const noexcept { return options_.after; }
  [[nodiscard]] bool on_backup(const std::string& underlying) const;

 private:
  enum Source : std::size_t { kPrimary = 0, kBackup = 1 };

  class Feed final : public md::EventSink {
   public:
    Feed(FailoverProvider& owner, Source source) : owner_(owner), source_(source) {}
    void publish(md::Event event) override { owner_.receive(source_, std::move(event)); }

   private:
    FailoverProvider& owner_;
    Source source_;
  };

  /// What one feed has said, in the shared id space.
  struct Cache {
    std::unordered_map<md::InstrumentId, md::InstrumentId> ids;  ///< feed id -> shared id
    std::map<md::InstrumentId, md::OptionQuote> quotes;
    std::map<md::InstrumentId, md::OpenInterest> interest;
    std::map<md::InstrumentId, md::VendorGreeks> greeks;
    std::map<std::string, md::UnderlyingQuote> spots;
  };

  struct Route {
    Source active = kPrimary;
    md::FeedState primary_state = md::FeedState::Connecting;
    std::string primary_message;
    md::Timestamp primary_success = 0;  ///< last fresh update from the primary
    md::Timestamp primary_bad_since = 0;  ///< when it turned Error or Stopped
    md::Timestamp primary_healthy_since = 0;  ///< start of its current fresh run
    md::Timestamp primary_market = 0;  ///< market time of its last complete snapshot
    md::FeedState backup_state = md::FeedState::Connecting;
    std::string backup_message;
    md::Timestamp switched = 0;
    bool held = false;  ///< due to fail over, but the backup is unavailable
    md::FeedState reported = md::FeedState::Connecting;  ///< last status sent downstream
    std::set<md::InstrumentId> unpriced;  ///< to clear once the active feed vouches for its chain
  };

  void receive(Source source, md::Event event);
  void status(Source source, const md::ProviderStatus& status);
  void evaluate(const std::string& underlying, Route& route, md::Timestamp now);
  void switch_to(const std::string& underlying, Route& route, Source source, md::Timestamp now);
  void report(const std::string& underlying, Route& route, md::Timestamp now, std::string_view note = {});
  void vouch(const std::string& underlying, Route& route);
  void prove(Route& route, md::Timestamp now) const;
  void define(md::InstrumentId id);
  bool active(Source source, const std::string& underlying) const;
  [[nodiscard]] md::Timestamp quiet_after(Source source) const;
  void start_backup();
  void run_monitor();

  std::unique_ptr<md::Provider> primary_;
  std::unique_ptr<md::Provider> backup_;
  Options options_;
  std::string name_;
  Feed primary_feed_{*this, kPrimary};
  Feed backup_feed_{*this, kBackup};

  mutable std::mutex mutex_;
  md::EventSink* sink_ = nullptr;
  md::Subscription subscription_;
  md::Timestamp started_ = 0;
  Cache caches_[2];
  std::unordered_map<std::string, md::InstrumentId> ids_;  ///< OSI -> shared id
  std::vector<md::OptionContract> contracts_;
  std::vector<bool> published_;  ///< definition sent downstream
  std::map<std::string, Route> routes_;
  bool want_backup_ = false;
  bool backup_started_ = false;
  std::atomic<int> on_backup_count_{0};
  std::atomic<bool> stopping_{false};

  std::thread monitor_;
  std::mutex wake_mutex_;
  std::condition_variable wake_;
};

}  // namespace openport::providers
