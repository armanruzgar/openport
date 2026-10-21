#pragma once

#include <functional>
#include <map>

#include "openport/net/http.hpp"
#include "openport/providers/request_budget.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One contract from Schwab's `GET /marketdata/v1/chains`.
struct SchwabOption {
  md::OptionContract contract;
  md::Timestamp ts = 0;  ///< quoteTimeInLong, milliseconds since the epoch
  double bid = 0, ask = 0;
  double bid_size = 0, ask_size = 0;  ///< contracts, as OPRA publishes them
  double open_interest = -1;
  double volume = -1;  ///< totalVolume: cumulative for the trading date
  std::optional<md::VendorGreeks> greeks;
};

struct SchwabChain {
  std::vector<SchwabOption> options;
  md::UnderlyingQuote underlying;  ///< ts 0 when the chain carries no timed underlying quote
  double underlying_price = 0;     ///< untimed; only used to place the strike window
  std::optional<bool> delayed;     ///< isDelayed, when the response states it
};

/// Schwab names indexes with a dollar sign: SPX -> "$SPX".
[[nodiscard]] std::string schwab_symbol(std::string_view underlying);
[[nodiscard]] SchwabChain parse_schwab_chain(std::string_view json, std::string_view underlying);
[[nodiscard]] std::set<md::Date> parse_schwab_expirations(std::string_view json);

/// Schwab Trader API market data with an OAuth refresh token. Only allowlisted
/// market-data GETs; the token refresh is the only POST and never redirects.
class SchwabApi {
 public:
  struct Credentials { std::string client_id, client_secret, refresh_token; };
  explicit SchwabApi(Credentials credentials);
  /// `path` is relative to /marketdata/v1 and must be /chains or /expirationchain.
  /// Returns the response; HTTP errors other than one retried 401 are returned.
  net::HttpResponse get(net::HttpClient& http, const std::string& path, md::Timestamp now,
                        const std::atomic<bool>* cancel = nullptr);
  void refresh(net::HttpClient& http, md::Timestamp now, const std::atomic<bool>* cancel = nullptr);
  [[nodiscard]] md::Timestamp refresh_at() const noexcept { return refresh_at_; }

 private:
  Credentials credentials_;
  std::string access_token_;
  md::Timestamp refresh_at_ = 0;
};

class SchwabProvider final : public PollingProvider {
 public:
  struct Options {
    SchwabApi::Credentials credentials;
    std::chrono::seconds poll_seconds{0};  // zero selects the rate-budgeted interval
    std::function<md::Timestamp()> clock = md::now;
  };
  explicit SchwabProvider(Options options);
  ~SchwabProvider() override { stop(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "schwab"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

 private:
  std::string poll(net::HttpClient&, const std::string&, const md::Subscription&, md::EventSink&) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override { return state_; }
  [[nodiscard]] std::chrono::seconds poll_interval() const noexcept override;
  std::string get(net::HttpClient&, const std::string& path);
  Options options_;
  SchwabApi api_;
  RequestBudget budget_{120, md::kNanosPerMinute};
  SnapshotPublisher publisher_;
  std::map<std::string, std::set<md::Date>> retained_expiries_;
  std::map<std::string, std::size_t> request_counts_;
  std::atomic<int> interval_seconds_{5};
  md::FeedState state_ = md::FeedState::Connecting;
};

}  // namespace openport::providers
