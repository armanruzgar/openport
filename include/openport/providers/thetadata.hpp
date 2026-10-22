#pragma once

#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One row from any ThetaData v3 option snapshot endpoint. Each endpoint fills in
/// its own fields; volume remains absent unless supplied.
struct ThetaRow {
  std::string root;  ///< ThetaData's "symbol" for an option: the OCC root, e.g. "SPXW"
  md::Date expiry;
  double strike = 0.0;
  pricing::OptionType type = pricing::OptionType::Call;
  md::Timestamp ts = 0;

  double bid = 0.0;  // quote
  double ask = 0.0;
  double bid_size = 0.0;
  double ask_size = 0.0;
  std::optional<double> volume;  ///< only when the response supplies day volume
  double open_interest = 0.0;     // open_interest
  double delta = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN();  // normalised per vol point
  double rho = std::numeric_limits<double>::quiet_NaN();  // normalised per rate percentage point
  double implied_vol = std::numeric_limits<double>::quiet_NaN();
  double underlying_price = 0.0;  // greeks/first_order or greeks/implied_volatility
  md::Timestamp underlying_ts = 0;  ///< spot's clock, independent of the option quote
};

/// Parses a ThetaData v3 snapshot response requested with format=ndjson.
/// Timestamps are read as New York wall-clock time. Throws std::runtime_error on
/// malformed input.
[[nodiscard]] std::vector<ThetaRow> parse_theta_rows(std::string_view ndjson);

/// ThetaData, through the Theta Terminal running on your machine. The terminal holds
/// your ThetaData credentials (THETADATA_API_KEY), so OpenPort itself needs no key;
/// it polls the terminal's local REST API for NBBO quotes, vendor Greeks and
/// open interest.
class ThetaDataProvider final : public PollingProvider {
 public:
  struct Options {
    std::string base_url = "http://127.0.0.1:25503";
    std::chrono::seconds poll_interval{2};
    std::chrono::seconds timeout{30};
    int open_interest_every = 30;  ///< polls between open-interest refreshes (it changes daily)
  };

  ThetaDataProvider() : ThetaDataProvider(Options{}) {}
  explicit ThetaDataProvider(Options options)
      : PollingProvider(options.poll_interval), options_(std::move(options)) {}
  ~ThetaDataProvider() override { stop(); }

  [[nodiscard]] std::string_view name() const noexcept override { return "thetadata"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  /// Publishes one complete snapshot of an underlying's chain.
  void publish_chain(const std::string& underlying, const std::vector<ThetaRow>& quotes,
                     const std::vector<ThetaRow>& implied_vols,
                     const std::vector<ThetaRow>& open_interest,
                     const md::Subscription& subscription, md::EventSink& sink);

 protected:
  std::string poll(net::HttpClient& http, const std::string& underlying,
                   const md::Subscription& subscription, md::EventSink& sink) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override {
    return md::FeedState::Live;
  }

 private:
  std::vector<ThetaRow> fetch(net::HttpClient& http, std::string_view endpoint,
                              const std::string& root);

  Options options_;
  SnapshotPublisher publisher_;
  std::unordered_map<std::string, int> polls_;
  /// Each underlying's last snapshot time, for an empty one that retires its quotes.
  std::unordered_map<std::string, md::Timestamp> snapshot_times_;
  std::unordered_map<std::string, double> window_spots_;  ///< Last spot per underlying, for the strike window.
};

}  // namespace openport::providers
