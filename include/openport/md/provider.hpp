#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/events.hpp"

namespace openport::md {

/// What a provider can deliver. The engine and the UI use this to decide which
/// features to offer, instead of showing empty panels.
struct Capabilities {
  bool realtime = false;
  bool realtime_plan_dependent =
      false;  ///< entitlement is reported by FeedState, not promised here
  std::chrono::seconds poll_interval{0};  ///< zero for streaming feeds
  std::chrono::seconds delay{0};  ///< how far behind the market the data is
  bool quotes = true;
  bool trades = false;
  bool open_interest = false;
  bool vendor_greeks = false;
  bool history = false;
};

/// Which option chains to deliver.
struct Subscription {
  std::vector<std::string> underlyings;  ///< e.g. {"SPX", "SPY"}
  int max_expiries = 0;                  ///< nearest N expiries; 0 means all
  double strike_window = 0.0;            ///< keep strikes within +-this fraction of spot; 0 = all
};

/// Receives events from a provider's own threads. Implementations must be thread-safe.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void publish(Event event) = 0;
};

/// A market-data provider adapter.
class Provider {
 public:
  virtual ~Provider() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  /// May be read while running, including a snapshot adapter's revised poll budget.
  [[nodiscard]] virtual Capabilities capabilities() const noexcept = 0;
  /// Whether recordings need v4 heartbeat and halt events. Fixed before start().
  [[nodiscard]] virtual bool market_controls() const noexcept { return false; }
  /// Whether option trades can carry a venue, which recordings keep in version 5. Fixed before start().
  [[nodiscard]] virtual bool trade_venues() const noexcept { return false; }

  /// Starts delivering events to `sink` on the adapter's own thread(s) and returns
  /// immediately. `sink` must outlive the provider.
  virtual void start(const Subscription& subscription, EventSink& sink) = 0;

  /// Stops delivery and joins the adapter's threads. Safe to call more than once.
  virtual void stop() = 0;
};

/// Settings for one provider: an API key plus provider-specific options.
struct ProviderConfig {
  std::string name;
  std::string api_key;
  std::map<std::string, std::string> options;
};

}  // namespace openport::md
