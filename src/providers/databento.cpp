#include "openport/providers/databento.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <databento/constants.hpp>
#include <databento/dbn.hpp>
#include <databento/enums.hpp>
#include <databento/live.hpp>
#include <databento/live_threaded.hpp>
#include <databento/record.hpp>
#include <limits>
#include <optional>
#include <stdexcept>

#include "openport/md/contract.hpp"
#include "openport/providers/factory.hpp"

namespace openport::providers {
namespace {

namespace db = databento;

/// Databento prices are fixed-point integers in units of 1e-9.
[[nodiscard]] double price(std::int64_t fixed) noexcept {
  if (fixed == db::kUndefPrice) return 0.0;
  return static_cast<double>(fixed) / static_cast<double>(db::kFixedPriceScale);
}

[[nodiscard]] md::Timestamp nanos(db::UnixNanos ts) noexcept {
  const auto value = ts.time_since_epoch().count();
  if (value == db::kUndefTimestamp ||
      value > static_cast<std::uint64_t>(std::numeric_limits<md::Timestamp>::max()))
    return 0;
  return static_cast<md::Timestamp>(value);
}

}  // namespace

bool DatabentoRecovery::recover(const std::function<void()>& reconnect,
                                const std::function<void()>& resubscribe,
                                const std::function<bool(std::chrono::seconds)>& wait,
                                const std::function<void(const std::string&)>& report) {
  while (failures_ < 8) {
    const auto delay = std::chrono::seconds(std::min(1u << failures_++, 30u));
    if (!wait(delay)) return false;
    try {
      reconnect();
      resubscribe();
      return true;
    } catch (const std::exception& error) {
      report(std::string("reconnect failed: ") + error.what());
    }
  }
  report("reconnect failure budget exhausted");
  return false;
}

std::vector<std::string> databento_parent_symbols(std::string_view underlying) {
  std::vector<std::string> symbols = md::option_roots(underlying);
  for (std::string& symbol : symbols) symbol += ".OPT";
  return symbols;
}

void DatabentoMapper::on_record(const db::Record& record) {
  if (const auto* def = record.GetIf<db::InstrumentDefMsg>()) {
    on_definition(*def);
  } else if (const auto* cbbo = record.GetIf<db::CbboMsg>()) {
    on_cbbo(*cbbo);
  } else if (const auto* cmbp = record.GetIf<db::Cmbp1Msg>()) {
    on_cmbp1(*cmbp);
  } else if (const auto* trade = record.GetIf<db::TradeMsg>()) {
    on_trade(*trade);
  } else if (const auto* stat = record.GetIf<db::StatMsg>()) {
    on_statistic(*stat);
  } else if (const auto* error = record.GetIf<db::ErrorMsg>()) {
    sink_.publish(md::ProviderStatus{md::now(), md::FeedState::Error, error->Err(), {}});
  }
}

void DatabentoMapper::on_definition(const db::InstrumentDefMsg& def) {
  if (def.instrument_class != db::InstrumentClass::Call &&
      def.instrument_class != db::InstrumentClass::Put) {
    return;
  }
  // OPRA raw symbols are OSI symbols; parsing them gives the same contract model
  // (and the same exercise/settlement conventions) as every other provider.
  std::optional<md::OptionContract> contract = md::parse_osi(def.RawSymbol());
  if (!contract) return;

  // OPRA IDs are unique only within a UTC day: the start-of-day replay reaches into
  // the previous day, and a session running past midnight gets new IDs. Definitions
  // arrive in time order, so the latest one for a Databento ID wins, and a contract
  // keeps our ID whatever Databento ID it had.
  const auto [it, inserted] =
      contracts_.try_emplace(contract->osi_symbol(), static_cast<md::InstrumentId>(contracts_.size()));
  ids_.insert_or_assign(def.hd.instrument_id, it->second);
  if (inserted) {
    underlyings_.push_back(contract->underlying);
    sink_.publish(md::ContractDefinition{it->second, std::move(*contract)});
  }
}

bool DatabentoMapper::lookup(std::uint32_t databento_id, md::InstrumentId& id) {
  const auto it = ids_.find(databento_id);
  if (it == ids_.end()) {
    ++undefined_records_;
    return false;
  }
  id = it->second;
  return true;
}

void DatabentoMapper::report_live(md::InstrumentId id) {
  const auto& symbol = underlyings_[id];
  if (live_underlyings_.insert(symbol).second) {
    sink_.publish(md::ProviderStatus{md::now(), md::FeedState::Live,
                                     "Databento " + symbol + ": receiving quotes", symbol});
  }
}

void DatabentoMapper::on_cbbo(const db::CbboMsg& msg) {
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  const auto ts = nanos(msg.ts_recv);
  if (ts <= 0) {
    live_underlyings_.erase(underlyings_[id]);
    sink_.publish(md::ProviderStatus{md::now(), md::FeedState::Error,
                                     "Databento " + underlyings_[id] + ": invalid CBBO ts_recv",
                                     underlyings_[id]});
    return;
  }
  const auto& level = msg.levels[0];
  sink_.publish(md::OptionQuote{id, ts, price(level.bid_px), price(level.ask_px),
                                static_cast<double>(level.bid_sz),
                                static_cast<double>(level.ask_sz)});
  report_live(id);
}

void DatabentoMapper::on_cmbp1(const db::Cmbp1Msg& msg) {
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  if (nanos(msg.hd.ts_event) <= 0) {
    live_underlyings_.erase(underlyings_[id]);
    sink_.publish(md::ProviderStatus{md::now(), md::FeedState::Error,
                                     "Databento " + underlyings_[id] + ": invalid CMBP-1 ts_event",
                                     underlyings_[id]});
    return;
  }
  const auto& level = msg.levels[0];
  sink_.publish(md::OptionQuote{id, nanos(msg.hd.ts_event), price(level.bid_px), price(level.ask_px),
                                static_cast<double>(level.bid_sz),
                                static_cast<double>(level.ask_sz)});
  report_live(id);
}

void DatabentoMapper::on_trade(const db::TradeMsg& msg) {
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  sink_.publish(md::OptionTrade{id, nanos(msg.hd.ts_event), price(msg.price),
                                static_cast<double>(msg.size)});
  const auto ts = nanos(msg.hd.ts_event);
  if (ts <= 0) return;
  const auto date = md::trading_date(ts);
  auto& volume = volumes_[id];
  if (date < volume.date) return;
  if (date != volume.date) volume = {date, 0, ts};
  volume.contracts += static_cast<double>(msg.size);
  volume.ts = std::max(volume.ts, ts);
  // After a gap the sum misses trades: it is not the day's volume until the next day.
  if (gap_date_ && date <= *gap_date_) return;
  sink_.publish(md::OptionVolume{id, volume.ts, volume.contracts});
}

void DatabentoMapper::volume_gap(md::Timestamp now) {
  const auto date = md::trading_date(now);
  gap_date_ = gap_date_ ? std::max(*gap_date_, date) : date;
}

void DatabentoMapper::on_statistic(const db::StatMsg& msg) {
  if (msg.stat_type != db::StatType::OpenInterest || msg.quantity == db::kUndefStatQuantity) return;
  md::InstrumentId id = 0;
  if (!lookup(msg.hd.instrument_id, id)) return;
  sink_.publish(md::OpenInterest{id, nanos(msg.hd.ts_event), static_cast<double>(msg.quantity)});
}

struct DatabentoProvider::Session {
  Session(md::EventSink& out, std::vector<std::string> symbols)
      : sink(out), mapper(out), underlyings(std::move(symbols)) {}

  void status(md::FeedState state, const std::string& message) {
    for (const auto& symbol : underlyings) {
      sink.publish(
          md::ProviderStatus{md::now(), state, "Databento " + symbol + ": " + message, symbol});
    }
  }

  bool wait(std::chrono::seconds delay) {
    std::unique_lock lock(wake_mutex);
    return !wake.wait_for(lock, delay, [this] { return stopping.load(); });
  }

  md::EventSink& sink;
  DatabentoMapper mapper;
  std::optional<db::LiveThreaded> client;
  std::atomic<bool> stopping{false};
  std::vector<std::string> underlyings;
  DatabentoRecovery recovery;
  std::mutex client_mutex;
  std::mutex wake_mutex;
  std::condition_variable wake;
};

DatabentoProvider::DatabentoProvider(Options options) : options_(std::move(options)) {
  if (options_.api_key.empty()) {
    throw std::invalid_argument("databento: an API key is required (set DATABENTO_API_KEY)");
  }
}

DatabentoProvider::~DatabentoProvider() { stop(); }

md::Capabilities DatabentoProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.realtime = true;
  caps.quotes = true;
  caps.trades = options_.trades;
  caps.open_interest = true;
  caps.vendor_greeks = false;
  caps.history = true;
  return caps;
}

void DatabentoProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  validate_subscription("databento", subscription);
  stop();
  const std::lock_guard lock(mutex_);
  auto session = std::make_unique<Session>(sink, subscription.underlyings);

  std::vector<std::string> symbols;
  for (const std::string& underlying : subscription.underlyings) {
    for (std::string& symbol : databento_parent_symbols(underlying)) symbols.push_back(std::move(symbol));
  }

  session->status(md::FeedState::Connecting, "OPRA");
  auto builder = db::LiveThreaded::Builder();
  session->client.emplace(bound_databento_waits(builder)
                              .SetKey(options_.api_key)
                              .SetDataset(db::Dataset::OpraPillar)
                              .BuildThreaded());

  // The live gateway replays up to 24 hours on request: replay today's definitions
  // and open interest so the full chain is known before the first quote.
  const auto replay_from = db::UnixNanos{std::chrono::duration_cast<db::UnixNanos::duration>(
      std::chrono::system_clock::now().time_since_epoch() - std::chrono::hours(23))};
  session->client->Subscribe(symbols, db::Schema::Definition, db::SType::Parent, replay_from);
  session->client->Subscribe(symbols, db::Schema::Statistics, db::SType::Parent, replay_from);
  session->client->Subscribe(symbols,
                             options_.quotes == QuoteSchema::Cbbo1s ? db::Schema::Cbbo1S
                                                                    : db::Schema::Cmbp1,
                             db::SType::Parent);
  // Trades replay too: OptionVolume is the day's cumulative volume, not since connect.
  if (options_.trades) session->client->Subscribe(symbols, db::Schema::Trades, db::SType::Parent, replay_from);

  Session* raw = session.get();
  session->client->Start(
      [raw](db::Metadata&&) {
        raw->status(md::FeedState::Connecting, "metadata received; waiting for quotes");
      },
      [raw](const db::Record& record) {
        if (raw->stopping) return db::KeepGoing::Stop;
        if (const auto* error = record.GetIf<db::ErrorMsg>()) {
          raw->mapper.reset_health();
          raw->status(md::FeedState::Error, error->Err());
          return db::KeepGoing::Continue;
        }
        raw->recovery.on_record();
        raw->mapper.on_record(record);
        return db::KeepGoing::Continue;
      },
      [raw](const std::exception& error) {
        const std::lock_guard client_lock(raw->client_mutex);
        if (raw->stopping) return db::LiveThreaded::ExceptionAction::Stop;
        auto report = [raw](const std::string& message) {
          raw->status(md::FeedState::Error, message);
        };
        report(error.what());
        raw->mapper.reset_health();
        // A resubscription replays nothing, so the trades of the outage are lost.
        raw->mapper.volume_gap(md::now());
        const bool recovered = raw->recovery.recover(
            [raw] { raw->client->Reconnect(); }, [raw] { raw->client->Resubscribe(); },
            [raw](std::chrono::seconds delay) { return raw->wait(delay); }, report);
        return recovered && !raw->stopping ? db::LiveThreaded::ExceptionAction::Restart
                                           : db::LiveThreaded::ExceptionAction::Stop;
      });
  session_ = std::move(session);
}

void DatabentoProvider::stop() {
  const std::lock_guard lock(mutex_);
  if (!session_) return;
  {
    const std::lock_guard wake_lock(session_->wake_mutex);
    session_->stopping = true;
  }
  session_->wake.notify_all();
  std::optional<db::LiveThreaded> client;
  {
    // Let an in-flight recovery finish before moving the SDK object it uses.
    const std::lock_guard client_lock(session_->client_mutex);
    client = std::move(session_->client);
  }
  client.reset();  // joins the SDK thread before the session and sink can disappear
  session_->status(md::FeedState::Stopped, "OPRA");
  session_.reset();
}

}  // namespace openport::providers
