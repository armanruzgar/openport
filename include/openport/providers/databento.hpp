#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "openport/md/provider.hpp"

namespace databento {
class Record;
struct InstrumentDefMsg;
struct CbboMsg;
struct Cmbp1Msg;
struct TradeMsg;
struct StatMsg;
}  // namespace databento

namespace openport::providers {

/// LiveBuilder's timeout configuration covers connection and authentication.
/// Kept together so initial connections and reconnects use the same limits.
template <typename Builder>
Builder& bound_databento_waits(Builder& builder) {
  return builder.SetTimeoutConf({std::chrono::seconds(5), std::chrono::seconds(5)});
}

/// Recovery is run on the SDK's exception-callback thread. A successful socket
/// connection does not reset the budget: only receiving records proves recovery.
class DatabentoRecovery {
 public:
  bool recover(const std::function<void()>& reconnect, const std::function<void()>& resubscribe,
               const std::function<bool(std::chrono::seconds)>& wait,
               const std::function<void(const std::string&)>& report);
  void on_record() noexcept { failures_ = 0; }

 private:
  unsigned int failures_ = 0;
};

/// Databento's OPRA feed is subscribed by option root ("parent" symbology). An
/// index usually has more than one root: SPX -> {"SPX.OPT", "SPXW.OPT"}.
[[nodiscard]] std::vector<std::string> databento_parent_symbols(std::string_view underlying);

/// Turns Databento OPRA records into OpenPort events. Separate from the network
/// client so it can be exercised with hand-built records.
class DatabentoMapper {
 public:
  explicit DatabentoMapper(md::EventSink& sink) : sink_(sink) {}

  /// Dispatches any record; types the mapper does not use are ignored.
  void on_record(const databento::Record& record);

  void on_definition(const databento::InstrumentDefMsg& def);
  void on_cbbo(const databento::CbboMsg& msg);
  void on_cmbp1(const databento::Cmbp1Msg& msg);
  void on_trade(const databento::TradeMsg& msg);
  void on_statistic(const databento::StatMsg& msg);
  void reset_health() { live_underlyings_.clear(); }
  /// Trades from `now`'s trading date may be missing (a reconnect replays none): its
  /// volume is no longer published as the day's cumulative volume.
  void volume_gap(md::Timestamp now);

  /// Records dropped because their instrument's definition had not arrived yet.
  [[nodiscard]] std::size_t undefined_records() const noexcept { return undefined_records_; }

 private:
  [[nodiscard]] bool lookup(std::uint32_t databento_id, md::InstrumentId& id);
  void report_live(md::InstrumentId id);

  md::EventSink& sink_;
  std::unordered_map<std::uint32_t, md::InstrumentId> ids_;  // Databento id -> ours
  std::unordered_map<std::string, md::InstrumentId> contracts_;  // OSI symbol -> ours
  std::vector<std::string> underlyings_;
  std::set<std::string> live_underlyings_;
  struct Volume {
    md::Date date;
    double contracts = 0;
    md::Timestamp ts = 0;
  };
  std::unordered_map<md::InstrumentId, Volume> volumes_;
  std::size_t undefined_records_ = 0;
  std::optional<md::Date> gap_date_;
};

/// Real-time OPRA options data from Databento, using your own API key.
/// Parent subscriptions stream the whole chain upstream. Nonzero max_expiries
/// or strike_window are rejected because they cannot reduce upstream traffic.
///
/// Definitions and open interest are replayed from the start of the trading day so
/// the whole chain arrives on connect; quotes (consolidated BBO, one-second samples
/// by default) and trades stream live. Databento sends no Greeks: OpenPort computes
/// them from these quotes.
///
/// Connect and auth each have a five-second SDK timeout; retry sleeps are
/// interruptible. The pinned SDK still performs synchronous DNS resolution,
/// subscription writes and an unbounded metadata read in LiveBlocking::Start().
/// Its destructor joins that read, so stop() can still wait for the gateway/OS
/// during those phases. TimeoutConf does not cover metadata in this SDK version.
class DatabentoProvider final : public md::Provider {
 public:
  enum class QuoteSchema : std::uint8_t {
    Cbbo1s,  ///< consolidated BBO sampled every second: light enough for whole chains
    Cmbp1,   ///< every change to the consolidated BBO: far more data
  };

  struct Options {
    std::string api_key;
    QuoteSchema quotes = QuoteSchema::Cbbo1s;
    bool trades = true;
  };

  explicit DatabentoProvider(Options options);
  ~DatabentoProvider() override;

  [[nodiscard]] std::string_view name() const noexcept override { return "databento"; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;

  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;

 private:
  struct Session;

  Options options_;
  std::mutex mutex_;
  std::unique_ptr<Session> session_;
};

}  // namespace openport::providers
