#include "openport/providers/databento_history.hpp"

#include <algorithm>
#include <cstddef>
#include <databento/historical.hpp>
#include <databento/log.hpp>
#include <databento/record.hpp>
#include <map>
#include <stdexcept>
#include <thread>

#include "openport/providers/databento.hpp"

namespace openport::providers {
namespace {
namespace db = databento;
class DatabentoHistory final : public HistorySource {
 public:
  explicit DatabentoHistory(DatabentoHistoryFetch fetch) : fetch_(std::move(fetch)) {}
  md::Timestamp receipt_time() const noexcept override { return forward_.received; }
  bool trade_venues() const noexcept override { return true; }
  void prepare(const ImportDay& request, HistoryWindow day, md::EventSink&) override {
    day_ = day;
    for (const auto& symbol : request.subscription.underlyings) {
      for (const auto& parent : databento_parent_symbols(symbol)) symbols_.push_back(parent);
    }
  }
  void read(HistoryWindow window, md::EventSink& sink) override {
    forward_.sink = &sink;
    const auto utc_day = window.begin / md::kNanosPerDay;
    if (utc_day != utc_day_) {
      // Historical IDs are unique only within a UTC day. Rebuild the vendor map
      // at midnight, retaining recording IDs by OSI across overnight sessions.
      forward_.interest.erase(forward_.interest.begin(), forward_.interest.begin() + static_cast<std::ptrdiff_t>(interest_position_));
      interest_position_ = 0;
      if (utc_day_ < 0) {
        const auto previous = md::previous_business_day(md::date_from_days(utc_day));
        const auto start = md::days_since_epoch(previous) * md::kNanosPerDay;
        load_date({start, start + md::kNanosPerDay});
      }
      utc_day_ = utc_day;
      const HistoryWindow date{utc_day * md::kNanosPerDay,
                               std::min((utc_day + 1) * md::kNanosPerDay, day_.end)};
      load_date(date);
      std::stable_sort(forward_.interest.begin(), forward_.interest.end(), [](const auto& left, const auto& right) { return left.received < right.received; });
    }
    while (interest_position_ < forward_.interest.size() && forward_.interest[interest_position_].received < window.end) {
      const auto& observation = forward_.interest[interest_position_++];
      forward_.received = observation.received;
      sink.publish(observation.event);
    }
    for (const auto schema : {db::Schema::Cbbo1S, db::Schema::Trades})
      fetch({window, schema, symbols_});
  }
 private:
  class Forward final : public md::EventSink {
   public:
    void publish(md::Event event) override {
      if (const auto* status = std::get_if<md::ProviderStatus>(&event)) {
        if (status->state == md::FeedState::Error) throw std::runtime_error("Databento history: invalid provider record");
        return;
      }
      if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) {
        const auto [it, inserted] = ids.emplace(definition->contract.osi_symbol(), static_cast<md::InstrumentId>(ids.size()));
        local_ids[definition->id] = it->second;
        if (inserted) sink->publish(md::ContractDefinition{it->second, definition->contract});
        return;
      }
      std::visit([&](auto& value) {
        if constexpr (requires { value.id; }) value.id = local_ids.at(value.id);
      }, event);
      // The import merger accumulates trades across schemas on market time.
      if (cache_interest) {
        if (const auto* value = std::get_if<md::OpenInterest>(&event)) {
          if (interest.size() == 1'000'000) throw std::runtime_error("Databento history: pending statistics limit exceeded");
          interest.push_back({*value, std::max(received, value->ts)});
          return;
        }
      }
      if (!std::holds_alternative<md::OptionVolume>(event)) sink->publish(std::move(event));
    }
    md::EventSink* sink = nullptr;
    std::map<std::string, md::InstrumentId> ids;
    std::map<md::InstrumentId, md::InstrumentId> local_ids;
    bool cache_interest = false;
    md::Timestamp received = 0;
    struct Interest { md::OpenInterest event; md::Timestamp received; };
    std::vector<Interest> interest;
  };
  void load_date(HistoryWindow date) {
    forward_.local_ids.clear();
    mapper_ = std::make_unique<DatabentoMapper>(forward_);
    fetch({date, db::Schema::Definition, symbols_});
    forward_.cache_interest = true;
    fetch({date, db::Schema::Statistics, symbols_});
    forward_.cache_interest = false;
  }
  void fetch(const DatabentoHistoryRequest& request) {
    // OPRA parent requests remain well below the API's 2000-symbol limit.
    fetch_(request, [&](const db::Record& record) {
      forward_.received = 0;
      if (const auto* trade = record.GetIf<db::TradeMsg>())
        forward_.received = static_cast<md::Timestamp>(trade->ts_recv.time_since_epoch().count());
      else if (const auto* statistic = record.GetIf<db::StatMsg>())
        forward_.received = static_cast<md::Timestamp>(statistic->ts_recv.time_since_epoch().count());
      if (forward_.received < 0) throw std::runtime_error("Databento history: invalid capture timestamp");
      const auto before = mapper_->undefined_records();
      mapper_->on_record(record);
      if (mapper_->undefined_records() != before) throw std::runtime_error("Databento history: missing contract definition");
      return db::KeepGoing::Continue;
    });
  }
  DatabentoHistoryFetch fetch_;
  std::vector<std::string> symbols_;
  Forward forward_;
  std::unique_ptr<DatabentoMapper> mapper_;
  md::Timestamp utc_day_ = -1;
  HistoryWindow day_;
  std::size_t interest_position_ = 0;
};
}
std::unique_ptr<HistorySource> databento_history(DatabentoHistoryFetch fetch) {
  return std::make_unique<DatabentoHistory>(std::move(fetch));
}
std::unique_ptr<HistorySource> databento_history(std::string key) {
  struct Client {
    db::NullLogReceiver logs;
    db::Historical historical;
    explicit Client(std::string secret)
        : historical(db::Historical::Builder().SetKey(std::move(secret)).SetLogReceiver(&logs).Build()) {}
  };
  const auto client = std::make_shared<Client>(std::move(key));
  return databento_history([client](const DatabentoHistoryRequest& request, const db::RecordCallback& callback) {
    // One outstanding request, at most ten starts/second. No automatic retry of
    // billable streams: a failure removes the incomplete recording.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto at = [](md::Timestamp time) { return db::UnixNanos{db::UnixNanos::duration{static_cast<std::uint64_t>(time)}}; };
    try {
      client->historical.TimeseriesGetRange("OPRA.PILLAR", db::DateTimeRange<db::UnixNanos>{at(request.window.begin), at(request.window.end)},
          request.symbols, request.schema, db::SType::Parent, db::SType::InstrumentId, 0,
          [](db::Metadata&&) {}, callback);
    } catch (const std::exception&) {
      // SDK exceptions can include server text. Never expose keys or bodies.
      throw std::runtime_error("Databento history: request failed; check key, entitlement, date availability and request limits");
    }
  });
}
}  // namespace openport::providers
