#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "openport/net/http.hpp"
#include "openport/providers/dxlink.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {
struct TastytradeContract {
  md::OptionContract contract;
  std::string streamer_symbol;
};
[[nodiscard]] std::vector<TastytradeContract> parse_tastytrade_chain(std::string_view json, std::string_view underlying);
[[nodiscard]] std::string parse_tastytrade_streamer(std::string_view json);

class TastytradeApi {
 public:
  struct Credentials { std::string client_secret, refresh_token, client_id; };
  explicit TastytradeApi(Credentials credentials);
  /// Only allowlisted market-data GETs. Auth POSTs are internal and never redirect.
  std::string get(net::HttpClient& http, const std::string& path, md::Timestamp now,
                  const std::atomic<bool>* cancel = nullptr);
  void refresh(net::HttpClient& http, md::Timestamp now, const std::atomic<bool>* cancel = nullptr);
  [[nodiscard]] md::Timestamp refresh_at() const noexcept { return refresh_at_; }

 private:
  Credentials credentials_;
  std::string access_token_;
  md::Timestamp refresh_at_ = 0;
};

struct TastytradeQuoteToken {
  std::string token, url;
  md::Timestamp refresh_at = 0;
};
[[nodiscard]] TastytradeQuoteToken parse_tastytrade_quote_token(std::string_view json, md::Timestamp now);

/// Definitions and event mapping are separate from protocol and transport.
///
/// Contracts the filters admit are subscribed as spot moves; ones that drift out of
/// the strike window stay subscribed (a position may hold them) until DXLink's cap
/// needs their room, when the farthest from spot go first.
///
/// Each underlying has one state for the connection: Live once a timed option quote
/// arrives, Stale while none has, or none for kUntimedGrace. An untimed quote among
/// timed ones (a contract whose priced side has no time) does not change it.
class TastytradeMapper {
 public:
  /// DXLink's limit on one session's event subscriptions.
  static constexpr std::size_t kMaxSubscriptions = 25000;
  /// How long untimed option quotes keep an underlying Live after its last timed one.
  static constexpr md::Timestamp kUntimedGrace = 60 * md::kNanosPerSecond;

  TastytradeMapper(md::Subscription subscription, md::EventSink& sink, bool milliseconds = false,
                   std::function<md::Timestamp()> clock = md::now)
      : subscription_(std::move(subscription)), sink_(sink), milliseconds_(milliseconds), clock_(std::move(clock)) {}
  void chain(std::string underlying, std::string streamer, std::vector<TastytradeContract> contracts, md::Date today);
  void event(const nlohmann::json& event);
  void reset_health() { live_.clear(); }
  /// Throws when the contracts the filters admit need more than kMaxSubscriptions.
  [[nodiscard]] std::set<DxlinkSubscription> subscriptions() const;
  /// True once after the subscribed instruments or underlyings change: only then can
  /// subscriptions() differ.
  [[nodiscard]] bool take_subscription_change() noexcept { return std::exchange(changed_, false); }

 private:
  struct Chain {
    std::vector<TastytradeContract> contracts;
    std::vector<std::string> osi;  ///< each contract's OSI symbol
    std::set<md::Date> expiries;
    std::vector<double> strikes;  ///< ascending, of the contracts the expiry filters admit
    md::Date today;
    double spot = 0;
    /// The strikes, as indices into `strikes`, the window admitted at the last selection.
    std::optional<std::pair<std::size_t, std::size_t>> admitted;
  };
  struct Instrument {
    md::InstrumentId id = 0;
    std::string underlying;
    md::OptionContract contract;
  };
  struct Health {
    md::FeedState state = md::FeedState::Connecting;
    std::optional<md::Timestamp> timed;  ///< when the last timed option quote arrived
  };
  void select(const std::string& underlying);
  [[nodiscard]] std::pair<std::size_t, std::size_t> admitted_strikes(const Chain& held) const;
  /// Unsubscribes drifted contracts, farthest from spot first, while over the cap.
  void fit();
  [[nodiscard]] std::size_t subscription_count() const noexcept {
    return 2 * underlyings_.size() + 4 * instruments_.size();
  }
  md::Subscription subscription_;
  md::EventSink& sink_;
  bool milliseconds_;
  std::function<md::Timestamp()> clock_;
  SnapshotPublisher definitions_;
  std::map<std::string, Chain> chains_;
  std::map<std::string, std::string> underlyings_;
  std::map<std::string, Instrument> instruments_;
  std::map<std::string, Health> live_;
  bool changed_ = true;
};

class TastytradeProvider final : public md::Provider {
 public:
  struct Options {
    TastytradeApi::Credentials credentials;
    bool sandbox = false;
    /// dxFeed's event model gives Quote bidTime/askTime in milliseconds since the epoch;
    /// false (dxlink_time_unit=unknown) leaves market times at zero instead.
    bool timestamps_in_milliseconds = true;
    /// Tests replace the REST client and the wait before each reconnect attempt
    /// (DxlinkProtocol::backoff by default).
    std::function<std::unique_ptr<net::HttpClient>()> http{};
    std::function<std::chrono::milliseconds(unsigned attempt)> retry_wait{};
    /// Tests also replace the DXLink transport (net::make_websocket by default).
    std::function<std::unique_ptr<net::WebSocket>()> websocket{};
    /// How often chains are fetched again, on the open stream.
    std::chrono::milliseconds chain_refresh = std::chrono::hours(1);
  };
  explicit TastytradeProvider(Options options);
  ~TastytradeProvider() override { stop(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "tastytrade"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  void start(const md::Subscription&, md::EventSink&) override;
  void stop() override;

 private:
  void run(md::Subscription subscription, md::EventSink* sink);
  Options options_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::condition_variable wake_;
};
}  // namespace openport::providers
