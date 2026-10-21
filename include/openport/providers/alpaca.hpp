#pragma once

#include <functional>
#include <map>

#include "openport/net/http.hpp"
#include "openport/providers/request_budget.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One contract from Alpaca's `GET /v1beta1/options/snapshots/{underlying}`.
struct AlpacaOption {
  md::OptionContract contract;
  md::Timestamp ts = 0;  ///< latestQuote.t
  double bid = 0, ask = 0;
  double bid_size = 0, ask_size = 0;  ///< contracts
  double volume = -1;                 ///< dailyBar.v, cumulative for the bar's trading date
  md::Timestamp volume_ts = 0;        ///< latestTrade.t, when it falls on that date
  std::optional<md::VendorGreeks> greeks;
};

struct AlpacaPage {
  std::vector<AlpacaOption> options;
  std::string next_page_token;  ///< empty on the last page
};

[[nodiscard]] AlpacaPage parse_alpaca_option_page(std::string_view json, std::string_view underlying);
/// `GET /v2/stocks/{symbol}/snapshot`: latest trade on its own clock, else the quote.
[[nodiscard]] md::UnderlyingQuote parse_alpaca_stock_snapshot(std::string_view json, const std::string& symbol);

/// Alpaca market data with an API key pair. Equity and ETF underlyings only:
/// Alpaca has no index options, so no SPX, NDX, RUT or VIX.
class AlpacaProvider final : public PollingProvider {
 public:
  enum class Feed : std::uint8_t { Indicative, Opra };
  struct Options {
    std::string key_id, secret_key;
    Feed feed = Feed::Indicative;
    std::string stock_feed = "iex";  ///< iex, sip or delayed_sip
    int requests_per_minute = 200;   ///< the Basic plan's limit
    std::chrono::seconds poll_seconds{0};  // zero selects the rate-budgeted interval
    std::function<md::Timestamp()> clock = md::now;
  };
  explicit AlpacaProvider(Options options);
  ~AlpacaProvider() override { stop(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "alpaca"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

 private:
  std::string poll(net::HttpClient&, const std::string&, const md::Subscription&, md::EventSink&) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override { return state_; }
  [[nodiscard]] std::chrono::seconds poll_interval() const noexcept override;
  std::string get(net::HttpClient&, const std::string& path);
  Options options_;
  RequestBudget budget_;
  SnapshotPublisher publisher_;
  std::map<std::string, std::pair<double, double>> defined_strikes_;  ///< lowest and highest defined
  std::map<std::string, std::size_t> request_counts_;
  std::atomic<int> interval_seconds_{5};
  md::FeedState state_ = md::FeedState::Connecting;
};

}  // namespace openport::providers
