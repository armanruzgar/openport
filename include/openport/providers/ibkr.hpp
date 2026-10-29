#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "openport/net/http.hpp"
#include "openport/providers/request_budget.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// The underlying's contract id and its option months ("OCT26") from
/// `GET /iserver/secdef/search`.
struct IbkrUnderlying {
  std::int64_t conid = 0;
  std::vector<std::string> months;
};

struct IbkrContract {
  std::int64_t conid = 0;
  md::OptionContract contract;
};

/// One row of `GET /iserver/marketdata/snapshot`. The gateway answers a contract's
/// first request with its id alone while the line warms up: `fields` is then false.
struct IbkrQuote {
  std::int64_t conid = 0;
  bool fields = false;
  md::Timestamp ts = 0;  ///< _updated: when IBKR last updated the row, ms since the epoch
  double bid = 0, ask = 0, bid_size = 0, ask_size = 0;
  double last = 0;  ///< 0 for a closing ("C") or halted ("H") price
  double open_interest = -1;
  double volume = -1;  ///< 7762, the day's count, from real-time rows; -1 when unknown
  double delta = std::numeric_limits<double>::quiet_NaN();
  double gamma = std::numeric_limits<double>::quiet_NaN();
  char availability = 0;  ///< 6509's first letter: R real-time, D delayed, Z/Y frozen, N none
};

struct IbkrSession {
  bool authenticated = false, connected = false, competing = false;
};

[[nodiscard]] IbkrUnderlying parse_ibkr_search(std::string_view json, std::string_view symbol);
/// The union of call and put strikes, ascending.
[[nodiscard]] std::vector<double> parse_ibkr_strikes(std::string_view json);
[[nodiscard]] std::vector<IbkrContract> parse_ibkr_info(std::string_view json, std::string_view underlying);
[[nodiscard]] std::vector<IbkrQuote> parse_ibkr_snapshot(std::string_view json);
[[nodiscard]] IbkrSession parse_ibkr_tickle(std::string_view json);
/// IBKR's display values: "1,234", "1.2K", "C4.10" (a close) and "H4.10" (halted).
[[nodiscard]] double parse_ibkr_number(std::string_view text, bool price);

/// Interactive Brokers' Client Portal Web API through the local gateway the
/// operator has logged in to. Read-only: secdef lookups and market-data snapshots,
/// a /tickle keepalive and the /iserver/accounts call the gateway requires first.
class IbkrProvider final : public PollingProvider {
 public:
  struct Options {
    std::string base_url = "https://localhost:5000/v1/api";  ///< loopback only
    int max_lines = 100;          ///< the account's concurrent market-data lines
    int requests_per_second = 5;  ///< below the gateway's documented 10 per second
    std::chrono::seconds poll_seconds{5};
    std::function<md::Timestamp()> clock = md::now;
    /// Paces requests; operational time, kept apart from the market-date clock.
    std::function<md::Timestamp()> pacing_clock = md::now;
  };
  explicit IbkrProvider(Options options);
  ~IbkrProvider() override { stop(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "ibkr"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

 private:
  struct Chain {
    md::Date mapped_on;
    IbkrUnderlying underlying;
    std::map<std::string, std::vector<double>> strikes;  ///< by month
    std::set<std::tuple<std::string, double, char>> looked_up;  ///< (month, strike, right) info fetched
    std::map<std::int64_t, md::OptionContract> contracts;
    std::set<std::int64_t> defined;  ///< contracts already published; they keep updating
  };
  std::string poll(net::HttpClient&, const std::string&, const md::Subscription&, md::EventSink&) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override { return state_; }
  std::string request(net::HttpClient&, const std::string& path, bool post = false);
  void keepalive(net::HttpClient&);
  void look_up(net::HttpClient&, Chain&, const std::string& underlying, const std::string& month, double strike,
               char right);
  std::vector<IbkrQuote> snapshot(net::HttpClient&, const std::vector<std::int64_t>& conids);
  Options options_;
  RequestBudget budget_;
  SnapshotPublisher publisher_;
  std::map<std::string, Chain> chains_;
  std::map<std::string, std::size_t> lines_;
  md::Timestamp tickled_ = 0;
  bool accounts_read_ = false;
  md::Date unsubscribed_on_;
  md::FeedState state_ = md::FeedState::Connecting;
};

}  // namespace openport::providers
