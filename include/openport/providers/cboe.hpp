#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "openport/md/bars.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {

/// One option row from a Cboe delayed-quotes snapshot, in Cboe's units.
struct CboeOption {
  std::string symbol;  ///< compact OSI, e.g. "SPXW261005P07405000"
  double bid = 0.0;
  double bid_size = 0.0;
  double ask = 0.0;
  double ask_size = 0.0;
  double iv = 0.0;  ///< decimal; 0 when Cboe has none
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double rho = 0.0;
  double theo = 0.0;
  double open_interest = 0.0;
  double volume = std::numeric_limits<double>::quiet_NaN();
};

/// A whole chain as Cboe serves it.
struct CboeChain {
  std::string symbol;       ///< as reported, e.g. "^SPX" or "SPY"
  md::Timestamp as_of = 0;  ///< when Cboe generated the snapshot (UTC)
  double price = 0.0;
  double bid = 0.0;
  double ask = 0.0;
  std::vector<CboeOption> options;
  md::Timestamp last_trade_time = 0;  ///< underlying print clock, independent of option sessions
  double prev_close = 0.0;            ///< the previous trading day's official close, when published
  double close = 0.0;                 ///< after the close, the day's closing price; before, the last
};

/// Parses cdn-api.cboe.com/api/global/delayed_quotes/options/<symbol>.json, or the
/// same document embedded in a quote page. The file stamps it "YYYY-MM-DD
/// HH:MM:SS" in UTC; the page, only "HH:MM:SS", which takes the UTC date of `now`
/// (the day before when that would put it more than an hour ahead of `now`).
/// Throws std::runtime_error if the document does not have the expected shape.
[[nodiscard]] CboeChain parse_cboe_chain(std::string_view json, md::Timestamp now = md::now());

/// Chain URL for an underlying. Index symbols take a leading underscore (_SPX).
[[nodiscard]] std::string cboe_chain_url(std::string_view underlying);

/// Cboe's delayed quote page for an underlying (www.cboe.com/delayed_quotes/spx/quote_table),
/// which embeds the same chain document the CDN file carries.
[[nodiscard]] std::string cboe_page_url(std::string_view underlying);

/// The chain document a quote page embeds (`CTX.contextOptionsData = {...}`),
/// or nothing when the page has none.
[[nodiscard]] std::optional<std::string_view> cboe_page_chain(std::string_view html);

/// Cboe's delayed chart files behind its quote pages: the latest regular session's
/// one-minute bars, and daily bars since the index or fund began.
enum class CboeChart : std::uint8_t { Intraday, Daily };

[[nodiscard]] std::string cboe_chart_url(std::string_view underlying, CboeChart chart);

/// One-minute bars of the latest regular session. Cboe labels a bar with the New
/// York minute it closes; each result starts one minute earlier. Rows that are not
/// valid bars are skipped. Throws std::runtime_error without a data array.
[[nodiscard]] std::vector<md::Bar> parse_cboe_intraday(std::string_view json);

/// Daily bars, oldest first, each starting at its session's 09:30 ET open. Cboe's
/// oldest index history has no open; retain any positive finite close for RV
/// fallback. OHLC consumers must validate the other fields.
/// Throws std::runtime_error without a data array.
[[nodiscard]] std::vector<md::Bar> parse_cboe_daily(std::string_view json);

/// Keeps chart history current from Cboe's free chart files on its own thread: the
/// latest session's minute bars every minute while a regular session is open or
/// just closed (every 15 minutes otherwise), and daily bars every hour. Files Cboe
/// does not have (403/404) are retried hourly.
class CboeChartHistory {
 public:
  using Sink = std::function<void(const std::string& underlying, CboeChart chart,
                                  std::vector<md::Bar> bars)>;
  struct Options {
    bool daily_only = false;  ///< Volatility proxies need daily closes only.
    std::chrono::seconds session_interval{60};
    std::chrono::seconds idle_interval{900};
    std::chrono::seconds daily_interval{3600};
    std::chrono::seconds missing_interval{3600};
    std::chrono::seconds timeout{20};
    std::function<md::Timestamp()> clock = md::now;
  };

  CboeChartHistory(std::vector<std::string> underlyings, Sink sink)
      : CboeChartHistory(std::move(underlyings), std::move(sink), Options{}) {}
  CboeChartHistory(std::vector<std::string> underlyings, Sink sink, Options options);
  ~CboeChartHistory() { stop(); }
  CboeChartHistory(const CboeChartHistory&) = delete;
  CboeChartHistory& operator=(const CboeChartHistory&) = delete;

  void start();
  /// Interrupts a request in flight and joins.
  void stop();

  /// Fetches every chart that is due, on the caller's thread. Returns when the next
  /// one falls due.
  md::Timestamp poll_once(net::HttpClient& http);

  /// The latest failure of each chart that is failing, one per line; empty when none is.
  [[nodiscard]] std::string error() const;

 private:
  void run();

  std::vector<std::string> underlyings_;
  Sink sink_;
  Options options_;
  std::map<std::pair<std::string, CboeChart>, md::Timestamp> due_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  mutable std::mutex error_mutex_;
  std::map<std::pair<std::string, CboeChart>, std::string> errors_;
};

/// Cboe's free delayed option quotes. No account or key needed; the data runs
/// 15 minutes behind the market and includes Cboe's own IV and Greeks.
class CboeDelayedProvider final : public PollingProvider {
 public:
  struct Options {
    std::chrono::seconds poll_interval{15};
    /// Per step of a request. Symbols are polled in turn, so one slow response delays
    /// them all; a quote page, the largest response, takes up to about ten seconds.
    std::chrono::seconds timeout{15};
    /// A CDN file this far behind the clock is stale, and the quote page is read instead.
    std::chrono::minutes stale_after{5};
    /// After the page proves fresher, how long to read it before trying the CDN again.
    std::chrono::minutes page_hold{5};
    /// Cboe refreshes its quote pages about once a minute: fetch one no more often.
    std::chrono::seconds page_interval{45};
    std::function<md::Timestamp()> clock = md::now;
  };

  static constexpr std::chrono::minutes kDelay{15};

  CboeDelayedProvider() : CboeDelayedProvider(Options{}) {}
  explicit CboeDelayedProvider(Options options)
      : PollingProvider(options.poll_interval), options_(options) {}
  ~CboeDelayedProvider() override { stop(); }

  [[nodiscard]] std::string_view name() const noexcept override { return "cboe"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  /// Turns one parsed chain into events, publishing only what changed since the
  /// previous call. The official close is published as an md::UnderlyingClose, each
  /// time it changes: after the close, the day's (Cboe's close field stops there while
  /// the price goes on with after-hours trades, and may be revised within minutes);
  /// during the regular session, the previous day's.
  void publish_chain(const CboeChain& chain, const md::Subscription& subscription,
                     md::EventSink& sink);

 protected:
  std::string poll(net::HttpClient& http, const std::string& underlying,
                   const md::Subscription& subscription, md::EventSink& sink) override;
  [[nodiscard]] md::FeedState healthy_state() const noexcept override {
    return md::FeedState::Delayed;
  }

 private:
  Options options_;
  SnapshotPublisher publisher_;
  /// Per underlying: read the quote page instead of the CDN file until then, and
  /// after the page was no fresher, leave it alone until then.
  std::map<std::string, md::Timestamp> page_until_;
  std::map<std::string, md::Timestamp> skip_page_until_;
  std::map<std::string, md::Timestamp> page_fetched_;
  /// The last official close published per underlying, by date.
  std::map<std::string, std::pair<md::Date, double>> closes_;
};

/// Cboe's published options holiday schedule.
inline constexpr std::string_view kCboeHolidaysUrl = "https://www.cboe.com/us/options/holidays/csv/";

/// Reads Cboe's holiday schedule CSV: after the "##" line, a "Holiday Name,Date,Regular
/// Trading Hours,Global Trading Hours" header and a row per holiday or early close.
/// Regular hours of "None" close the day, and an end before 16:00 closes it early; an
/// overnight session ending on the holiday itself ("to 11:30 AM (Mon)") runs into it.
/// Throws std::runtime_error for a document without that header.
[[nodiscard]] std::vector<md::ScheduledDay> parse_cboe_holidays(std::string_view csv);

/// Fetches Cboe's holiday schedule when started and daily after (hourly while it
/// fails), and passes `sink` every day it has seen, the latest listing of a date
/// winning: openportd hands them to md::set_scheduled_days, so a closure Cboe
/// announces takes effect without a new build.
class CboeHolidaySchedule {
 public:
  using Sink = std::function<void(std::vector<md::ScheduledDay>)>;
  struct Options {
    std::chrono::seconds interval{24 * 3600};
    std::chrono::seconds retry{3600};
    std::chrono::seconds timeout{30};
    std::function<md::Timestamp()> clock = md::now;
  };

  explicit CboeHolidaySchedule(Sink sink) : CboeHolidaySchedule(std::move(sink), Options{}) {}
  CboeHolidaySchedule(Sink sink, Options options);
  ~CboeHolidaySchedule() { stop(); }
  CboeHolidaySchedule(const CboeHolidaySchedule&) = delete;
  CboeHolidaySchedule& operator=(const CboeHolidaySchedule&) = delete;

  void start();
  /// Interrupts a request in flight and joins.
  void stop();
  /// Fetches the schedule if it is due, on the caller's thread; returns when it next is.
  md::Timestamp poll_once(net::HttpClient& http);
  /// The latest failure, or empty.
  [[nodiscard]] std::string error() const;

 private:
  void run();

  Sink sink_;
  Options options_;
  md::Timestamp due_ = 0;
  std::map<md::Date, md::ScheduledDay> days_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  mutable std::mutex error_mutex_;
  std::string error_;
};

/// Cboe's special opening quotation (SOQ) indices: the values AM-settled index options
/// settle on, published by Cboe Global Indices under their own symbols (SET is SPX's,
/// RLS RUT's, DJS DJX's and VRO VIX's). NDX's SOQ is Nasdaq's and is not listed.
struct CboeSoqIndex {
  std::string_view underlying;
  std::string_view index;
};
inline constexpr CboeSoqIndex kCboeSoqIndices[] = {{"SPX", "SET"}, {"RUT", "RLS"}, {"DJX", "DJS"}, {"VIX", "VRO"}};
/// The SOQ index of an underlying's AM-settled options, or empty.
[[nodiscard]] std::string_view cboe_soq_index(std::string_view underlying);
/// Cboe's daily history of an SOQ index ("DATE,SET" then "MM/DD/YYYY,value" rows).
/// Its row for a date is the published record AM options settle on. The index's
/// delayed quote is not used: when it shows the day's value cannot be told reliably
/// from its trade time, which can come before every component has opened.
[[nodiscard]] std::string cboe_soq_history_url(std::string_view index);

/// One published special opening quotation: a history row, with its date.
struct SettlementQuotation {
  std::string underlying;
  std::string index;
  md::Date date;
  double value = 0;
  std::string url;
  bool operator==(const SettlementQuotation&) const = default;
};
/// The rows of a history CSV dated on or after `since`, skipping any that are not a
/// date and a positive finite value. Throws std::runtime_error for a document without
/// the "DATE,<index>" header.
[[nodiscard]] std::vector<SettlementQuotation> parse_cboe_soq_history(std::string_view csv, std::string_view underlying,
                                                                      std::string_view index, md::Date since = {});
/// Fetches the special opening quotations of the AM-settled underlyings it is given:
/// their daily history when started and every `interval` (Cboe's CDN caches it for 15
/// minutes), so a date's row is found soon after Cboe publishes it and a restart still
/// finds the values of the last `lookback_days`. `sink` receives every value it holds
/// each time one is new or changed.
class CboeSettlementQuotations {
 public:
  using Sink = std::function<void(std::vector<SettlementQuotation>)>;
  struct Options {
    std::chrono::seconds interval{15 * 60};
    std::chrono::seconds retry{15 * 60};
    std::chrono::seconds timeout{30};
    std::int64_t lookback_days = 14;
    std::function<md::Timestamp()> clock = md::now;
  };

  CboeSettlementQuotations(std::vector<std::string> underlyings, Sink sink, Options options);
  ~CboeSettlementQuotations() { stop(); }
  CboeSettlementQuotations(const CboeSettlementQuotations&) = delete;
  CboeSettlementQuotations& operator=(const CboeSettlementQuotations&) = delete;

  /// The underlyings it fetches: those with a Cboe SOQ index, once each.
  [[nodiscard]] const std::vector<std::string>& underlyings() const { return underlyings_; }
  void start();
  /// Interrupts a request in flight and joins.
  void stop();
  /// Fetches what is due, on the caller's thread; returns when it next is.
  md::Timestamp poll_once(net::HttpClient& http);
  /// The latest failures, one per line, or empty.
  [[nodiscard]] std::string error() const;

 private:
  void run();

  std::vector<std::string> underlyings_;
  Sink sink_;
  Options options_;
  md::Timestamp due_ = 0;
  std::map<std::pair<std::string, md::Date>, SettlementQuotation> known_;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::mutex wake_mutex_;
  std::condition_variable wake_;
  mutable std::mutex error_mutex_;
  std::string error_;
};

}  // namespace openport::providers
