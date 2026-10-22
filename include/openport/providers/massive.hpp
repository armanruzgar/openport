#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <string_view>
#include <thread>
#include <vector>

#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One contract from Massive's option chain snapshot.
struct MassiveContract {
  std::string symbol;  ///< compact OSI, "O:" prefix removed
  bool european = false;
  double multiplier = 100.0;

  bool has_quote = false;
  double bid = 0.0;
  double ask = 0.0;
  double bid_size = 0.0;
  double ask_size = 0.0;
  md::Timestamp quote_ts = 0;
  bool realtime = false;  ///< the quote's timeframe is "REAL-TIME" rather than "DELAYED"

  bool has_open_interest = false;
  double open_interest = 0.0;
  std::optional<double> volume;
  md::Timestamp volume_ts = 0;

  double iv = 0.0;  ///< decimal; 0 when Massive returns none (e.g. deep in the money)
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;   ///< per vol point
  double theta = 0.0;  ///< per calendar day
};

/// One page of `GET /v3/snapshot/options/{underlying}`.
struct MassivePage {
  std::vector<MassiveContract> contracts;
  std::string next_url;  ///< empty on the last page
  double underlying_price = 0.0;
  md::Timestamp underlying_ts = 0;
};

/// Parses one page of Massive's option chain snapshot. Throws std::runtime_error on
/// an error response or a document of the wrong shape.
[[nodiscard]] MassivePage parse_massive_chain_page(std::string_view json);

/// First-page URL for an underlying; indexes use Massive's "I:" prefix (I:SPX).
[[nodiscard]] std::string massive_chain_url(std::string_view base_url, std::string_view underlying);

/// Massive (formerly Polygon.io), using your own API key. Real-time or 15-minute
/// delayed depending on your plan; the feed status says which. Polls the whole chain
/// snapshot, so plans with per-minute request limits are too slow for large chains.
class MassiveProvider final : public PollingProvider {
 public:
  struct Options {
    std::string api_key;
    std::string base_url = "https://api.massive.com";
    std::chrono::seconds poll_interval{5};
    std::chrono::seconds timeout{30};
  };

  explicit MassiveProvider(Options options);
  ~MassiveProvider() override { stop(); }

  [[nodiscard]] std::string_view name() const noexcept override { return "massive"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  /// Publishes one complete chain (all pages of one poll).
  void publish_chain(const std::string& underlying, const std::vector<MassiveContract>& contracts,
                     double underlying_price, md::Timestamp underlying_ts,
                     const md::Subscription& subscription, md::EventSink& sink);

 protected:
  std::string poll(net::HttpClient& http, const std::string& underlying,
                   const md::Subscription& subscription, md::EventSink& sink) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override {
    return realtime_ ? md::FeedState::Live : md::FeedState::Delayed;
  }

 private:
  Options options_;
  SnapshotPublisher publisher_;
  std::atomic<bool> realtime_{false};
  std::unordered_map<std::string, double> window_spots_;  ///< Last spot per underlying, for the strike window.
};

/// A cash dividend as Massive's dividends endpoint lists it.
struct MassiveDividend {
  std::string ticker;
  md::Date ex_date;
  double cash_amount = 0.0;
  std::string currency;
};
struct MassiveDividendPage {
  std::vector<MassiveDividend> dividends;
  std::string next_url;
};

/// Massive's dividends endpoint (GET /stocks/v1/dividends, in every stocks plan) for a
/// ticker's ex-dates from `from` on.
[[nodiscard]] std::string massive_dividends_url(std::string_view base_url, std::string_view ticker, md::Date from);
/// Reads one page of it. Throws std::runtime_error for an error status or another shape.
[[nodiscard]] MassiveDividendPage parse_massive_dividends(std::string_view json);

/// Fetches the tickers' cash dividends in US dollars from Massive when started and
/// every six hours after (hourly while it fails), those going ex from a month back on,
/// and passes all it knows to `sink`: openportd's --dividends massive. A ticker that
/// fails keeps what was fetched for it before.
class MassiveDividends {
 public:
  using Sink = std::function<void(std::vector<MassiveDividend>)>;
  struct Options {
    std::string api_key;
    std::string base_url = "https://api.massive.com";
    std::chrono::seconds interval{6 * 3600};
    std::chrono::seconds retry{3600};
    std::chrono::seconds timeout{30};
    std::function<md::Timestamp()> clock = md::now;
  };

  MassiveDividends(std::vector<std::string> tickers, Sink sink, Options options);
  ~MassiveDividends() { stop(); }
  MassiveDividends(const MassiveDividends&) = delete;
  MassiveDividends& operator=(const MassiveDividends&) = delete;

  void start();
  /// Interrupts a request in flight and joins.
  void stop();
  /// Fetches every ticker if they are due, on the caller's thread; returns when they next are.
  md::Timestamp poll_once(net::HttpClient& http);
  /// The latest failures, one per line, or empty.
  [[nodiscard]] std::string error() const;

 private:
  void run();

  std::vector<std::string> tickers_;
  Sink sink_;
  Options options_;
  md::Timestamp due_ = 0;
  std::map<std::string, std::vector<MassiveDividend>> known_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  mutable std::mutex error_mutex_;
  std::string error_;
};

}  // namespace openport::providers
