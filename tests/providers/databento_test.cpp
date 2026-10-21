#include "openport/providers/databento.hpp"

#include <databento/constants.hpp>
#include <databento/record.hpp>
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using namespace openport;
namespace db = databento;

class Collector final : public md::EventSink {
 public:
  void publish(md::Event event) override { events.push_back(std::move(event)); }

  template <typename T>
  std::vector<T> all() const {
    std::vector<T> out;
    for (const auto& event : events) {
      if (const T* e = std::get_if<T>(&event)) out.push_back(*e);
    }
    return out;
  }

  std::vector<md::Event> events;
};

db::UnixNanos at(std::uint64_t nanos) { return db::UnixNanos{db::UnixNanos::duration{nanos}}; }

constexpr std::int64_t kScale = db::kFixedPriceScale;

template <typename Msg>
void stamp(Msg& msg, db::RType rtype, std::uint32_t instrument_id, std::uint64_t ts) {
  msg.hd.length = static_cast<std::uint8_t>(sizeof(Msg) / db::RecordHeader::kLengthMultiplier);
  msg.hd.rtype = rtype;
  msg.hd.instrument_id = instrument_id;
  msg.hd.ts_event = at(ts);
  if constexpr (requires { msg.ts_recv; }) msg.ts_recv = at(ts);
}

db::InstrumentDefMsg definition(std::uint32_t id, const char* raw_symbol, db::InstrumentClass cls) {
  db::InstrumentDefMsg def{};
  stamp(def, db::RType::InstrumentDef, id, 1);
  std::strncpy(def.raw_symbol.data(), raw_symbol, def.raw_symbol.size() - 1);
  def.instrument_class = cls;
  return def;
}

template <typename Msg>
void feed(providers::DatabentoMapper& mapper, Msg& msg) {
  mapper.on_record(db::Record{&msg.hd});
}

TEST(Databento, ParentSymbolsCoverEveryRootOfAnIndex) {
  EXPECT_EQ(providers::databento_parent_symbols("SPX"),
            (std::vector<std::string>{"SPX.OPT", "SPXW.OPT"}));
  EXPECT_EQ(providers::databento_parent_symbols("SPY"), (std::vector<std::string>{"SPY.OPT"}));
}

TEST(Databento, MapsDefinitionsQuotesTradesAndOpenInterest) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);

  auto put = definition(4242, "SPXW  261005P07405000", db::InstrumentClass::Put);
  feed(mapper, put);
  auto future = definition(7, "ESZ6", db::InstrumentClass::Future);  // not an option: ignored
  feed(mapper, future);

  db::CbboMsg quote{};
  stamp(quote, db::RType::Cbbo1S, 4242, 1'790'000'000'000'000'000);
  quote.levels[0].bid_px = 44 * kScale / 10;  // 4.40
  quote.levels[0].ask_px = 46 * kScale / 10;  // 4.60
  quote.levels[0].bid_sz = 314;
  quote.levels[0].ask_sz = 172;
  feed(mapper, quote);

  db::TradeMsg trade{};
  stamp(trade, db::RType::Mbp0, 4242, 1'790'000'001'000'000'000);
  trade.price = 45 * kScale / 10;
  trade.size = 3;
  feed(mapper, trade);

  db::StatMsg open_interest{};
  stamp(open_interest, db::RType::Statistics, 4242, 1'789'990'000'000'000'000);
  open_interest.stat_type = db::StatType::OpenInterest;
  open_interest.quantity = 1520;
  feed(mapper, open_interest);

  db::StatMsg other_stat = open_interest;
  other_stat.stat_type = db::StatType::SettlementPrice;
  feed(mapper, other_stat);

  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 1u);
  EXPECT_EQ(definitions[0].id, 0u);
  EXPECT_EQ(definitions[0].contract.underlying, "SPX");
  EXPECT_DOUBLE_EQ(definitions[0].contract.strike, 7405.0);
  EXPECT_EQ(definitions[0].contract.type, pricing::OptionType::Put);

  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_EQ(quotes[0].id, 0u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 4.4);
  EXPECT_DOUBLE_EQ(quotes[0].ask, 4.6);
  EXPECT_DOUBLE_EQ(quotes[0].bid_size, 314.0);
  EXPECT_EQ(quotes[0].ts, 1'790'000'000'000'000'000);

  const auto trades = sink.all<md::OptionTrade>();
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_DOUBLE_EQ(trades[0].price, 4.5);
  EXPECT_DOUBLE_EQ(trades[0].size, 3.0);

  const auto oi = sink.all<md::OpenInterest>();
  ASSERT_EQ(oi.size(), 1u);
  EXPECT_DOUBLE_EQ(oi[0].contracts, 1520.0);
}

TEST(Databento, AReusedIdFollowsItsLatestDefinitionAndAContractKeepsItsId) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);
  const auto quote_for = [&](std::uint32_t vendor, std::uint64_t ts) {
    db::CbboMsg quote{};
    stamp(quote, db::RType::Cbbo1S, vendor, ts);
    quote.levels[0].bid_px = kScale;
    quote.levels[0].ask_px = 2 * kScale;
    feed(mapper, quote);
    return sink.all<md::OptionQuote>().back().id;
  };
  // Yesterday's definition from the start-of-day replay, then today's for the same ID.
  auto yesterday = definition(1234, "SPXW  261021C07400000", db::InstrumentClass::Call);
  feed(mapper, yesterday);
  EXPECT_EQ(quote_for(1234, 10), 0u);
  auto today = definition(1234, "SPXW  261022P07450000", db::InstrumentClass::Put);
  feed(mapper, today);
  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 2u);
  EXPECT_EQ(definitions[1].id, 1u);
  EXPECT_DOUBLE_EQ(definitions[1].contract.strike, 7450.0);
  EXPECT_EQ(quote_for(1234, 20), 1u);
  // After midnight the first contract arrives under a new ID: no second definition.
  auto renumbered = definition(77, "SPXW  261021C07400000", db::InstrumentClass::Call);
  feed(mapper, renumbered);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);
  EXPECT_EQ(quote_for(77, 30), 0u);
}

TEST(Databento, DropsRecordsForUndefinedInstrumentsAndHandlesEmptySides) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);

  db::CbboMsg early{};
  stamp(early, db::RType::Cbbo1S, 99, 1);
  feed(mapper, early);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_EQ(mapper.undefined_records(), 1u);

  auto call = definition(99, "SPY   261016C00800000", db::InstrumentClass::Call);
  feed(mapper, call);
  db::CbboMsg one_sided{};
  stamp(one_sided, db::RType::Cbbo1S, 99, 2);
  one_sided.levels[0].bid_px = db::kUndefPrice;  // no bid
  one_sided.levels[0].ask_px = 5 * kScale / 100;
  feed(mapper, one_sided);
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 0.0);
  EXPECT_DOUBLE_EQ(quotes[0].ask, 0.05);
}

TEST(Databento, CbboUsesReceiveTimeAndRejectsUndefinedTimestamps) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);
  auto def = definition(99, "SPY   261016C00800000", db::InstrumentClass::Call);
  feed(mapper, def);
  db::CbboMsg quote{};
  stamp(quote, db::RType::Cbbo1S, 99, db::kUndefTimestamp);
  quote.ts_recv = at(1'790'000'000'000'000'000);
  feed(mapper, quote);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts, 1'790'000'000'000'000'000);
  sink.events.clear();
  quote.ts_recv = at(db::kUndefTimestamp);
  feed(mapper, quote);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  quote.ts_recv = at(std::uint64_t{1} << 63);
  feed(mapper, quote);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_EQ(sink.all<md::ProviderStatus>().back().underlying, "SPY");
  db::Cmbp1Msg cmbp{};
  stamp(cmbp, db::RType::Cmbp1, 99, 123);
  cmbp.ts_recv = at(456);
  feed(mapper, cmbp);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts, 123);
}

TEST(Databento, ReconnectsBeforeResubscribingAndResetsBudgetOnlyOnRecords) {
  providers::DatabentoRecovery recovery;
  std::vector<std::string> calls;
  std::vector<int> delays;
  auto reconnect = [&] { calls.push_back("reconnect"); };
  auto subscribe = [&] { calls.push_back("subscribe"); };
  auto wait = [&](std::chrono::seconds delay) {
    delays.push_back(static_cast<int>(delay.count()));
    return true;
  };
  auto report = [&](const std::string& error) { calls.push_back(error); };
  for (int i = 0; i < 8; ++i) EXPECT_TRUE(recovery.recover(reconnect, subscribe, wait, report));
  EXPECT_EQ(delays, (std::vector<int>{1, 2, 4, 8, 16, 30, 30, 30}));
  for (std::size_t i = 0; i < 16; i += 2) {
    EXPECT_EQ(calls[i], "reconnect");
    EXPECT_EQ(calls[i + 1], "subscribe");
  }
  EXPECT_FALSE(recovery.recover(reconnect, subscribe, wait, report));
  EXPECT_EQ(calls.back(), "reconnect failure budget exhausted");
  recovery.on_record();
  EXPECT_TRUE(recovery.recover(reconnect, subscribe, wait, report));
  EXPECT_EQ(delays.back(), 1);
}

TEST(Databento, RecoveryHandlesReconnectFailuresAndCanBeInterrupted) {
  providers::DatabentoRecovery recovery;
  int attempts = 0;
  int subscriptions = 0;
  std::vector<std::string> errors;
  auto reconnect = [&] {
    if (++attempts < 3) throw std::runtime_error("offline");
  };
  auto subscribe = [&] { ++subscriptions; };
  auto report = [&](const std::string& error) { errors.push_back(error); };
  EXPECT_TRUE(recovery.recover(reconnect, subscribe, [](auto) { return true; }, report));
  EXPECT_EQ(attempts, 3);
  EXPECT_EQ(subscriptions, 1);
  EXPECT_EQ(errors.size(), 2u);
  EXPECT_FALSE(recovery.recover(reconnect, subscribe, [](auto) { return false; }, report));
  EXPECT_EQ(attempts, 3);
}

TEST(Databento, ConfiguresFiniteConnectAndAuthenticationTimeouts) {
  struct Builder {
    struct Timeouts {
      std::chrono::seconds connect{0};
      std::chrono::seconds auth{0};
    } timeouts;
    Builder& SetTimeoutConf(Timeouts value) {
      timeouts = value;
      return *this;
    }
  } builder;
  providers::bound_databento_waits(builder);
  EXPECT_EQ(builder.timeouts.connect, std::chrono::seconds(5));
  EXPECT_EQ(builder.timeouts.auth, std::chrono::seconds(5));
}

TEST(Databento, QuotesReportLiveOnlyForTheirUnderlyingAfterReconnect) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);
  auto spx = definition(1, "SPXW  261016C05000000", db::InstrumentClass::Call);
  auto spy = definition(2, "SPY   261016C00500000", db::InstrumentClass::Call);
  feed(mapper, spx);
  feed(mapper, spy);
  db::CbboMsg quote{};
  stamp(quote, db::RType::Cbbo1S, 2, 123);
  feed(mapper, quote);
  auto statuses = sink.all<md::ProviderStatus>();
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_EQ(statuses[0].underlying, "SPY");
  EXPECT_EQ(statuses[0].state, md::FeedState::Live);
  sink.events.clear();
  mapper.reset_health();
  feed(mapper, quote);
  statuses = sink.all<md::ProviderStatus>();
  ASSERT_EQ(statuses.size(), 1u);
  EXPECT_EQ(statuses[0].underlying, "SPY");
}


TEST(Databento, TradeVolumeSumsPerContractAndResetsAtTheTradingDateRoll) {
  Collector sink;
  providers::DatabentoMapper mapper(sink);
  mapper.on_definition(definition(42, "SPXW  261005P07405000", db::InstrumentClass::Put));
  mapper.on_definition(definition(43, "SPXW  261005C07800000", db::InstrumentClass::Call));
  const auto trade = [&](std::uint32_t id, md::Date day, int hour, std::uint32_t size) {
    db::TradeMsg msg{};
    stamp(msg, db::RType::Mbp0, id, static_cast<std::uint64_t>(md::new_york_to_utc(day, hour, 0)));
    msg.price = kScale;
    msg.size = size;
    mapper.on_trade(msg);
  };
  trade(42, {2026, 9, 22}, 15, 3);
  trade(42, {2026, 9, 22}, 16, 4);
  trade(43, {2026, 9, 22}, 16, 9);
  mapper.reset_health();  // reconnect does not erase session totals
  trade(42, {2026, 9, 22}, 16, 2);
  trade(42, {2026, 9, 22}, 21, 1);  // following trading date, before midnight
  trade(42, {2026, 9, 23}, 1, 5);   // midnight does not reset it
  trade(42, {2026, 9, 23}, 0, 2);   // late same-day print still adds to the total
  trade(43, {2026, 9, 23}, 2, 1);   // each contract resets independently
  trade(42, {2026, 9, 22}, 16, 100);  // late prior-day print must not roll back
  trade(999, {2026, 9, 23}, 2, 100);  // unknown contract
  const auto volumes = sink.all<md::OptionVolume>();
  ASSERT_EQ(volumes.size(), 8u);
  for (std::size_t i = 0; i < volumes.size(); ++i) {
    EXPECT_EQ(volumes[i].contracts, (std::vector<double>{3, 7, 9, 9, 1, 6, 8, 1})[i]);
  }
  EXPECT_EQ(volumes[2].id, 1u);
  EXPECT_EQ(volumes[6].id, 0u);
  EXPECT_EQ(volumes[6].ts, md::new_york_to_utc({2026, 9, 23}, 1, 0));
  EXPECT_EQ(volumes.back().id, 1u);
}

}  // namespace
