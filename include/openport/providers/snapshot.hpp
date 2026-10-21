#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "openport/md/provider.hpp"

namespace openport::net {
class HttpClient;
}

/// Building blocks for providers that deliver whole-chain snapshots on request
/// (Cboe, Massive, ThetaData) rather than a stream of updates.
namespace openport::providers {

/// Turns successive snapshots of a chain into events: each contract is defined
/// once, then only values that changed since the previous snapshot are published.
class SnapshotPublisher {
 public:
  /// Returns the contract's id, publishing its definition the first time `key`
  /// (any provider-stable identifier, usually the OSI symbol) is seen.
  md::InstrumentId define(const std::string& key, md::OptionContract contract, md::EventSink& sink);
  [[nodiscard]] bool known(const std::string& key) const { return ids_.contains(key); }

  /// Call only after a complete snapshot. Quotes absent from it must stop pricing;
  /// a failed fetch or partial pagination must never retire the previous chain. It
  /// ends with md::SnapshotComplete, which vouches for the quotes not sent again.
  void finish(const std::string& underlying, const std::set<md::InstrumentId>& seen,
              md::Timestamp ts, md::EventSink& sink);

  void quote(md::InstrumentId id, md::Timestamp ts, double bid, double ask, double bid_size,
             double ask_size, md::EventSink& sink);
  void open_interest(md::InstrumentId id, md::Timestamp ts, double contracts, md::EventSink& sink);
  void volume(md::InstrumentId id, md::Timestamp ts, double contracts, md::EventSink& sink);
  void greeks(const md::VendorGreeks& greeks, md::EventSink& sink);

  [[nodiscard]] std::size_t size() const noexcept { return last_.size(); }

 private:
  struct Last {
    std::string underlying;
    double bid = -1.0;
    double ask = -1.0;
    double bid_size = -1.0;
    double ask_size = -1.0;
    double open_interest = -1.0;
    double volume = -1.0;
    md::Timestamp volume_ts = 0;
    std::optional<md::VendorGreeks> greeks;
  };

  std::unordered_map<std::string, md::InstrumentId> ids_;
  std::vector<Last> last_;
};

/// A subscription's optional filters (nearest N expiries, strikes near spot)
/// evaluated against one snapshot.
class ChainFilter {
 public:
  /// `expiries` are all expiries present in the snapshot; `today` drops expired ones.
  ChainFilter(const md::Subscription& subscription, md::Date today, double spot,
              const std::set<md::Date>& expiries);

  [[nodiscard]] bool admits(const md::OptionContract& contract) const;

 private:
  double spot_;
  double strike_window_;
  bool limit_expiries_;
  md::Date today_;
  std::set<md::Date> expiries_;
};

/// Runs `poll()` for every subscription on a background thread at a fixed interval.
/// Derived classes must call `stop()` in their own destructor, before their members
/// (which `poll()` uses) are destroyed.
class PollingProvider : public md::Provider {
 public:
  void start(const md::Subscription& subscription, md::EventSink& sink) final;
  /// Interrupts requests and joins promptly, except while system DNS is blocked
  /// in getaddrinfo (see HttpClient::get).
  void stop() final;

  /// Runs one poll and reports its underlying's outcome. Call without the background
  /// loop running; an injectable HTTP client also permits deterministic replay.
  void poll_once(net::HttpClient& http, const std::string& underlying,
                 const md::Subscription& subscription, md::EventSink& sink);

 protected:
  explicit PollingProvider(std::chrono::seconds interval) : interval_(interval) {}

  /// Fetches and publishes one snapshot of `underlying`. Throws on failure; the
  /// loop reports the error and carries on with the next underlying.
  /// Returns a one-line summary for the status message.
  virtual std::string poll(net::HttpClient& http, const std::string& underlying,
                           const md::Subscription& subscription, md::EventSink& sink) = 0;

  void check_cancelled() const;
  [[nodiscard]] const std::atomic<bool>* cancellation() const { return &stopping_; }

  /// Status to report after a successful poll: Live or Delayed.
  [[nodiscard]] virtual md::FeedState healthy_state() const noexcept = 0;
  [[nodiscard]] virtual std::chrono::seconds poll_interval() const noexcept { return interval_; }
  /// Waits unless stopped; false once stop() is called. Seconds convert implicitly.
  bool sleep(std::chrono::milliseconds duration);

 private:
  void run(md::Subscription subscription, md::EventSink* sink);

  std::chrono::seconds interval_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;
};

}  // namespace openport::providers
