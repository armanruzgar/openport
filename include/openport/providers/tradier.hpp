#pragma once

#include <deque>
#include <functional>
#include <map>

#include "openport/net/http.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {

struct TradierOption {
  md::OptionContract contract;
  md::Timestamp ts = 0;
  double bid = 0, ask = 0;
  double bid_size = 0, ask_size = 0;  // vendor units, not normalised until explicitly confirmed
  double open_interest = -1;
  double volume = -1;
  md::Timestamp volume_ts = 0;
  std::optional<md::VendorGreeks> greeks;
};

[[nodiscard]] std::set<md::Date> parse_tradier_expirations(std::string_view json);
[[nodiscard]] std::map<std::string, std::set<md::Date>> parse_tradier_lookup(std::string_view json);
[[nodiscard]] std::vector<TradierOption> parse_tradier_chain(std::string_view json);
[[nodiscard]] md::UnderlyingQuote parse_tradier_quote(std::string_view json, const std::string& symbol);

/// Per-token rolling request budget. Operational clock, independent of market time.
class TradierBudget {
 public:
  explicit TradierBudget(bool sandbox) : limit_(sandbox ? 60 : 120) {}
  [[nodiscard]] std::chrono::seconds interval(std::size_t requests) const;
  [[nodiscard]] std::chrono::seconds wait(md::Timestamp now);
  void sent(md::Timestamp now);
  /// Returns true when the response itself was throttled, including non-429 signals.
  bool received(const net::HttpResponse& response, md::Timestamp now);
  void succeeded() { failures_ = 0; }

 private:
  int limit_;
  int failures_ = 0;
  md::Timestamp blocked_until_ = 0;
  md::Timestamp remote_reset_ = 0;
  long long remaining_ = -1;
  std::deque<md::Timestamp> sent_;
};

class TradierProvider final : public PollingProvider {
 public:
  struct Options {
    std::string access_token;
    bool sandbox = false;
    std::chrono::seconds poll_seconds{0};  // zero selects the rate-budgeted interval
    /// Contracts per unit of Tradier's option bidsize/asksize: 1 when they are
    /// contracts (the default, as OPRA publishes them), 100 for hundreds, 0 to withhold them.
    double size_multiplier = 1;
    std::function<md::Timestamp()> clock = md::now;
  };
  explicit TradierProvider(Options options);
  ~TradierProvider() override { stop(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "tradier"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

 private:
  std::string poll(net::HttpClient&, const std::string&, const md::Subscription&, md::EventSink&) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override { return state_; }
  [[nodiscard]] std::chrono::seconds poll_interval() const noexcept override;
  std::string get(net::HttpClient&, const std::string& path);
  Options options_;
  TradierBudget budget_;
  SnapshotPublisher publisher_;
  std::map<std::string, std::set<md::Date>> retained_expiries_;
  std::map<std::string, std::size_t> request_counts_;
  std::atomic<int> interval_seconds_{1};
  md::FeedState state_ = md::FeedState::Connecting;
};
}  // namespace openport::providers
