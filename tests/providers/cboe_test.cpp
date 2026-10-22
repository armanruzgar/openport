#include "openport/providers/cboe.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#include "support/http_stub.hpp"

namespace {

using namespace openport;

// A hand-written document with the same shape as Cboe's delayed-quotes JSON.
constexpr std::string_view kChain = R"({
  "timestamp": "2026-09-22 19:48:44",
  "data": {
    "options": [
      {"option": "SPXW260925C07800000", "bid": 41.5, "bid_size": 12.0, "ask": 42.3, "ask_size": 9.0,
       "iv": 0.1432, "open_interest": 1520.0, "volume": 873.0, "delta": 0.4812, "gamma": 0.0021,
       "vega": 3.9102, "theta": -8.7311, "rho": 0.2911, "theo": 41.9},
      {"option": "SPXW260925P07800000", "bid": 63.1, "bid_size": 7.0, "ask": 64.0, "ask_size": 10.0,
       "iv": 0.1511, "open_interest": 2210.0, "volume": 1204.0, "delta": -0.5188, "gamma": 0.0021,
       "vega": 3.9107, "theta": -8.1002, "rho": -0.3120, "theo": 63.5},
      {"option": "SPX261016C08000000", "bid": 70.0, "bid_size": 3.0, "ask": 72.4, "ask_size": 3.0,
       "iv": null, "open_interest": 42.0, "volume": 0.0, "delta": 0.3, "gamma": 0.001,
       "vega": 9.1, "theta": -2.2, "rho": 1.1, "theo": 71.2},
      {"option": "NOT-A-SYMBOL", "bid": 1.0, "ask": 2.0}
    ],
    "symbol": "^SPX", "security_type": "index", "current_price": 7777.27,
    "price_change": 12.5, "bid": 7776.0, "ask": 7778.53,
    "last_trade_time": "2026-09-22 15:33:42"
  }
})";

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

TEST(Cboe, ParsesTheChainDocument) {
  const providers::CboeChain chain = providers::parse_cboe_chain(kChain);
  EXPECT_EQ(chain.symbol, "^SPX");
  EXPECT_DOUBLE_EQ(chain.price, 7777.27);
  EXPECT_DOUBLE_EQ(chain.bid, 7776.0);
  EXPECT_DOUBLE_EQ(chain.ask, 7778.53);
  EXPECT_EQ(md::format_timestamp(chain.as_of), "2026-09-22T19:48:44.000Z");
  ASSERT_EQ(chain.options.size(), 4u);
  EXPECT_EQ(chain.options[0].symbol, "SPXW260925C07800000");
  EXPECT_DOUBLE_EQ(chain.options[0].bid, 41.5);
  EXPECT_DOUBLE_EQ(chain.options[0].ask_size, 9.0);
  EXPECT_DOUBLE_EQ(chain.options[0].iv, 0.1432);
  EXPECT_DOUBLE_EQ(chain.options[1].delta, -0.5188);
  EXPECT_DOUBLE_EQ(chain.options[2].iv, 0.0);  // null becomes zero
}

TEST(Cboe, PublishesDefinitionsBeforeDataAndSkipsBadSymbols) {
  providers::CboeDelayedProvider provider;
  Collector sink;
  provider.publish_chain(providers::parse_cboe_chain(kChain), {}, sink);

  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 3u);
  EXPECT_EQ(definitions[0].contract.osi_symbol(), "SPXW  260925C07800000");
  EXPECT_EQ(definitions[2].contract.settlement, md::Settlement::AM);

  // Every event that refers to an id comes after that id's definition.
  std::vector<bool> defined(3, false);
  for (const auto& event : sink.events) {
    if (const auto* d = std::get_if<md::ContractDefinition>(&event)) defined[d->id] = true;
    if (const auto* q = std::get_if<md::OptionQuote>(&event)) {
      EXPECT_TRUE(defined[q->id]);
    }
  }

  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 3u);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 3u);
  EXPECT_EQ(sink.all<md::VendorGreeks>().size(), 2u);  // the null-IV row has none

  const auto underlying = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(underlying.size(), 1u);
  EXPECT_EQ(underlying[0].symbol, "SPX");
  // Underlying and option clocks are independent.
  EXPECT_EQ(md::format_timestamp(underlying[0].ts), "2026-09-22T19:33:42.000Z");
  EXPECT_EQ(md::format_timestamp(sink.all<md::OptionQuote>()[0].ts), "2026-09-22T19:33:44.000Z");
}

TEST(Cboe, RepublishesOnlyWhatChanged) {
  providers::CboeDelayedProvider provider;
  Collector first;
  providers::CboeChain chain = providers::parse_cboe_chain(kChain);
  provider.publish_chain(chain, {}, first);

  Collector second;
  chain.options[1].bid = 63.2;
  provider.publish_chain(chain, {}, second);
  EXPECT_TRUE(second.all<md::ContractDefinition>().empty());
  const auto quotes = second.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 63.2);
  EXPECT_TRUE(second.all<md::OpenInterest>().empty());
  EXPECT_EQ(second.all<md::UnderlyingQuote>().size(), 1u);  // spot is always republished
  // The poll ends by vouching for the quotes it did not send again.
  const auto marks = second.all<md::SnapshotComplete>();
  ASSERT_EQ(marks.size(), 1u);
  EXPECT_EQ(marks[0].underlying, "SPX");
  EXPECT_EQ(marks[0].ts, quotes[0].ts);
  EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(second.events.back()));
}

TEST(Cboe, PublishesThePreviousDaysCloseOnceADayInTheSession) {
  providers::CboeDelayedProvider provider;
  providers::CboeChain chain = providers::parse_cboe_chain(kChain);  // 15:33 ET, Tuesday 2026-09-22
  EXPECT_DOUBLE_EQ(chain.prev_close, 0.0);  // this document has none
  chain.prev_close = 7706.03;
  Collector first;
  provider.publish_chain(chain, {}, first);
  const auto closes = first.all<md::UnderlyingClose>();
  ASSERT_EQ(closes.size(), 1u);
  EXPECT_EQ(closes[0].symbol, "SPX");
  EXPECT_EQ(closes[0].date, (md::Date{2026, 9, 21}));
  EXPECT_DOUBLE_EQ(closes[0].price, 7706.03);
  Collector again;
  provider.publish_chain(chain, {}, again);
  EXPECT_TRUE(again.all<md::UnderlyingClose>().empty());
  // In the evening Cboe may not have rolled it over yet, so it waits for the session.
  chain.as_of = md::new_york_to_utc({2026, 9, 22}, 21, 0);
  chain.prev_close = 7766.40;
  Collector evening;
  provider.publish_chain(chain, {}, evening);
  EXPECT_TRUE(evening.all<md::UnderlyingClose>().empty());
  chain.as_of = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  Collector morning;
  provider.publish_chain(chain, {}, morning);
  ASSERT_EQ(morning.all<md::UnderlyingClose>().size(), 1u);
  EXPECT_EQ(morning.all<md::UnderlyingClose>()[0].date, (md::Date{2026, 9, 22}));
}

TEST(Cboe, PublishesTheDaysCloseAfterItAndItsRevisions) {
  // As on 2026-09-24: after 16:00 the close field stops at the close (767.27, then
  // revised to 767.18) while the price goes on with after-hours trades.
  providers::CboeDelayedProvider provider;
  providers::CboeChain chain = providers::parse_cboe_chain(kChain);
  chain.symbol = "SPY";
  chain.last_trade_time = md::new_york_to_utc({2026, 9, 24}, 16, 0);
  const auto published = [&](md::Timestamp data_time, double price, double close, bool after_close = true) {
    chain.as_of = data_time + 15 * md::kNanosPerMinute;
    chain.price = price;
    chain.close = close;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    const auto quotes = sink.all<md::UnderlyingQuote>();
    EXPECT_EQ(quotes.size(), 1u);
    if (!quotes.empty()) {
      EXPECT_EQ(quotes[0].symbol, "SPY");
      EXPECT_EQ(quotes[0].ts, chain.last_trade_time);
      EXPECT_DOUBLE_EQ(quotes[0].last, after_close ? close : price);
      EXPECT_DOUBLE_EQ(quotes[0].bid, after_close ? 0 : chain.bid);
      EXPECT_DOUBLE_EQ(quotes[0].ask, after_close ? 0 : chain.ask);
    }
    return sink.all<md::UnderlyingClose>();
  };
  const md::Date day{2026, 9, 24};
  EXPECT_TRUE(published(md::new_york_to_utc(day, 15, 58), 767.42, 767.42, false).empty());  // still the price
  auto closes = published(md::new_york_to_utc(day, 16, 0) + 49 * md::kNanosPerSecond, 767.26, 767.27);
  ASSERT_EQ(closes.size(), 1u);
  EXPECT_EQ(closes[0].symbol, "SPY");
  EXPECT_EQ(closes[0].date, day);
  EXPECT_DOUBLE_EQ(closes[0].price, 767.27);
  EXPECT_TRUE(published(md::new_york_to_utc(day, 16, 5), 766.93, 767.27).empty());
  closes = published(md::new_york_to_utc(day, 16, 10) + 49 * md::kNanosPerSecond, 766.75, 767.18);
  ASSERT_EQ(closes.size(), 1u);
  EXPECT_DOUBLE_EQ(closes[0].price, 767.18);
  // After midnight it is the next day, not yet closed.
  EXPECT_TRUE(published(md::new_york_to_utc({2026, 9, 25}, 0, 30), 765.0, 767.18, false).empty());
}

TEST(Cboe, UnderlyingUsesTheCloseAtTheBusinessDaysRegularClose) {
  for (const auto* symbol : {"SPY", "QQQ", "IWM", "DIA", "AAPL", "^SPX"}) {
    for (const auto& [day, hour, business_day] :
         {std::tuple{md::Date{2026, 9, 24}, 16, true}, std::tuple{md::Date{2026, 11, 27}, 13, true},
          std::tuple{md::Date{2026, 9, 26}, 16, false}, std::tuple{md::Date{2026, 12, 25}, 16, false}}) {
      const auto close_time = md::new_york_to_utc(day, hour, 0);
      for (const auto offset : {-md::kNanosPerSecond, md::Timestamp{0}, md::kNanosPerMinute}) {
        SCOPED_TRACE(std::string(symbol) + " " + md::format_timestamp(close_time + offset));
        providers::CboeChain chain;
        chain.symbol = symbol;
        chain.as_of = close_time + offset + 15 * md::kNanosPerMinute;
        chain.last_trade_time = close_time - md::kNanosPerSecond;
        chain.close = 100;
        chain.price = std::string_view(symbol) == "^SPX" ? chain.close : 101;
        chain.bid = 100.5;
        chain.ask = 101.5;
        providers::CboeDelayedProvider provider;
        Collector sink;
        provider.publish_chain(chain, {}, sink);
        const auto quotes = sink.all<md::UnderlyingQuote>();
        ASSERT_EQ(quotes.size(), 1u);
        EXPECT_EQ(quotes[0].ts, chain.last_trade_time);
        const bool closed = business_day && offset >= 0;
        EXPECT_DOUBLE_EQ(quotes[0].last, closed ? 100 : chain.price);
        EXPECT_DOUBLE_EQ(quotes[0].bid, closed ? 0 : 100.5);
        EXPECT_DOUBLE_EQ(quotes[0].ask, closed ? 0 : 101.5);
      }
    }
  }
}

TEST(Cboe, UnderlyingKeepsThePreviousCloseUntilTheOpen) {
  // As on 2026-09-25 before the open: QQQ's price was an after-hours trade (739.41)
  // stamped 15:59:59 the day before, while Cboe had rolled its previous close to 741.10.
  const auto quote_at = [](md::Timestamp data_time) {
    providers::CboeChain chain;
    chain.symbol = "QQQ";
    chain.as_of = data_time + 15 * md::kNanosPerMinute;
    chain.last_trade_time = md::new_york_to_utc({2026, 9, 24}, 15, 59) + 59 * md::kNanosPerSecond;
    chain.price = 739.41;
    chain.bid = 739.30;
    chain.ask = 739.50;
    chain.close = 741.10;
    chain.prev_close = 741.10;
    providers::CboeDelayedProvider provider;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    return sink.all<md::UnderlyingQuote>().at(0);
  };
  for (const auto time : {md::new_york_to_utc({2026, 9, 25}, 8, 0), md::new_york_to_utc({2026, 9, 26}, 12, 0)}) {
    const auto quote = quote_at(time);  // before Friday's open, and on Saturday
    EXPECT_DOUBLE_EQ(quote.last, 741.10);
    EXPECT_DOUBLE_EQ(quote.bid, 0);
    EXPECT_DOUBLE_EQ(quote.ask, 0);
  }
  EXPECT_DOUBLE_EQ(quote_at(md::new_york_to_utc({2026, 9, 25}, 9, 30)).last, 739.41);  // the session's own price
}

TEST(Cboe, UnderlyingKeepsTheCurrentQuoteWithoutAValidClose) {
  for (const auto close : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
    SCOPED_TRACE(close);
    auto chain = providers::parse_cboe_chain(kChain);  // no close field
    EXPECT_DOUBLE_EQ(chain.close, 0);
    chain.symbol = "SPY";
    chain.close = close;
    chain.as_of = md::new_york_to_utc({2026, 9, 24}, 17, 0);
    chain.last_trade_time = md::new_york_to_utc({2026, 9, 24}, 16, 0);
    providers::CboeDelayedProvider provider;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    const auto quotes = sink.all<md::UnderlyingQuote>();
    ASSERT_EQ(quotes.size(), 1u);
    EXPECT_EQ(quotes[0].ts, chain.last_trade_time);
    EXPECT_DOUBLE_EQ(quotes[0].last, chain.price);
    EXPECT_DOUBLE_EQ(quotes[0].bid, chain.bid);
    EXPECT_DOUBLE_EQ(quotes[0].ask, chain.ask);
    EXPECT_TRUE(sink.all<md::UnderlyingClose>().empty());
  }
}

TEST(Cboe, AppliesExpiryAndStrikeFilters) {
  providers::CboeDelayedProvider provider;
  Collector sink;
  md::Subscription nearest;
  nearest.max_expiries = 1;
  provider.publish_chain(providers::parse_cboe_chain(kChain), nearest, sink);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);  // only 2026-09-25

  providers::CboeDelayedProvider narrow_provider;
  Collector narrow;
  md::Subscription window;
  window.strike_window = 0.01;  // 7800 is within 1% of 7777; 8000 is not
  narrow_provider.publish_chain(providers::parse_cboe_chain(kChain), window, narrow);
  EXPECT_EQ(narrow.all<md::ContractDefinition>().size(), 2u);
}

TEST(Cboe, IndexChainsUseAnUnderscore) {
  EXPECT_EQ(providers::cboe_chain_url("SPX"),
            "https://cdn-api.cboe.com/api/global/delayed_quotes/options/_SPX.json");
  for (const auto* symbol : {"SPY", "QQQ", "IWM", "DIA"}) {
    EXPECT_EQ(providers::cboe_chain_url(symbol),
              "https://cdn-api.cboe.com/api/global/delayed_quotes/options/" + std::string(symbol) + ".json");
  }
  EXPECT_EQ(providers::cboe_page_url("IWM"), "https://www.cboe.com/delayed_quotes/iwm/quote_table");
  EXPECT_EQ(providers::cboe_page_url("DIA"), "https://www.cboe.com/delayed_quotes/dia/quote_table");
}

TEST(Cboe, UpdatesKnownContractsOutsideTheWindowAndRetiresOnlyTheirUnderlying) {
  providers::CboeDelayedProvider provider;
  auto chain = providers::parse_cboe_chain(kChain);
  Collector sink;
  md::Subscription sub;
  sub.strike_window = 0.01;
  provider.publish_chain(chain, sub, sink);
  sink.events.clear();
  chain.price = 10000;
  chain.options[0].bid = 42;
  provider.publish_chain(chain, sub, sink);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].bid, 42);
  sink.events.clear();
  auto other = chain;
  other.symbol = "SPY";
  other.options.clear();
  provider.publish_chain(other, sub, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  chain.options.clear();
  provider.publish_chain(chain, sub, sink);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  for (const auto& q : sink.all<md::OptionQuote>()) EXPECT_EQ(q.bid + q.ask, 0);
}

}  // namespace

namespace {
TEST(Cboe, CurbQuotesAdvancePastTheUnderlyingLastTrade) {
  // Publication is UTC; last_trade_time is New York local (EDT here).
  const auto chain = providers::parse_cboe_chain(R"({
    "timestamp":"2026-09-22 20:34:59",
    "data":{"options":[{"option":"SPXW260925C07800000","bid":41,"ask":42,"iv":0.2}],
            "symbol":"^SPX","current_price":7777,"last_trade_time":"2026-09-22 16:14:59"}
  })");
  providers::CboeDelayedProvider provider;
  Collector sink;
  provider.publish_chain(chain, {}, sink);
  const auto expected = *md::parse_datetime("2026-09-22 16:14:59", md::Zone::NewYork);
  EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts, expected);
  const auto option_time = *md::parse_datetime("2026-09-22 16:19:59", md::Zone::NewYork);
  EXPECT_EQ(sink.all<md::OptionQuote>().at(0).ts, option_time);
  EXPECT_EQ(sink.all<md::VendorGreeks>().at(0).ts, option_time);
}

TEST(Cboe, UnderlyingRetainsItsOwnClockAndUnknownTimeStaysUnknown) {
  for (const auto* last_trade : {"2026-09-22 16:34:59", "invalid", ""}) {
    const auto chain = providers::parse_cboe_chain(
        std::string(
            R"({"timestamp":"2026-09-22 20:34:59","data":{"options":[],"symbol":"SPY","last_trade_time":")") +
        last_trade + R"("}})");
    providers::CboeDelayedProvider provider;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts,
              md::parse_datetime(last_trade, md::Zone::NewYork).value_or(0));
  }
}
}  // namespace

namespace {
TEST(Cboe, OptionMarketTimeTracksEachProductsSessionsAndNeverRunsPastDelayedTime) {
  for (auto symbol : {"SPXW", "SPY", "AAPL"}) {
    for (const auto& [date, hour, minute] :
         {std::tuple{md::Date{2026, 9, 22}, 21, 25}, std::tuple{md::Date{2026, 9, 22}, 17, 10},
          std::tuple{md::Date{2026, 9, 23}, 9, 42}, std::tuple{md::Date{2026, 9, 26}, 21, 25},
          std::tuple{md::Date{2026, 11, 27}, 14, 0}}) {
      providers::CboeChain chain;
      chain.symbol = md::conventions_for_root(symbol).underlying;
      chain.as_of = md::new_york_to_utc(date, hour, minute);
      chain.last_trade_time = md::new_york_to_utc({2026, 9, 22}, 16, 0);
      chain.price = 100;
      chain.options.push_back({std::string(symbol) + "261218C00100000", 1, 1, 2, 1, .2});
      providers::CboeDelayedProvider provider;
      Collector sink;
      provider.publish_chain(chain, {}, sink);
      const auto delayed = chain.as_of - 15 * md::kNanosPerMinute;
      const auto expected = md::trading_session(symbol, delayed).market_time;
      EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).ts, chain.last_trade_time);
      EXPECT_EQ(sink.all<md::OptionQuote>().at(0).ts, expected);
      EXPECT_LE(expected, delayed);
      if (hour == 21 && date == md::Date{2026, 9, 22}) {
        EXPECT_EQ(expected,
                  std::string_view(symbol) == "SPXW"
                      ? delayed
                      : md::new_york_to_utc(date, 16, std::string_view(symbol) == "SPY" ? 15 : 0));
        // An overnight changed quote must advance even while current_price and
        // last_trade_time stay frozen; SnapshotPublisher still deduplicates.
        chain.as_of += 2 * md::kNanosPerMinute;
        chain.options[0].bid += .1;
        sink.events.clear();
        provider.publish_chain(chain, {}, sink);
        ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
        EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts,
                  expected + (std::string_view(symbol) == "SPXW" ? 2 * md::kNanosPerMinute : 0));
      }
    }
  }
}

// Cboe's chart files: intraday prices are numbers, daily prices are strings.
constexpr std::string_view kIntraday = R"({"timestamp": "2026-09-22 20:14:23", "symbol": "_SPX", "data": [
  {"datetime": "2026-09-22T09:32:00", "sequence_number": 2,
   "price": {"open": 7772.9399, "high": 7779.8799, "low": 7772.8999, "close": 7779.1499},
   "volume": {"stock_volume": 0, "total_options_volume": 25886}},
  {"datetime": "2026-09-22T09:31:00", "sequence_number": 1,
   "price": {"open": 7770.8101, "high": 7773.5801, "low": 7769.6001, "close": 7772.6499}},
  {"datetime": "2026-09-22T09:33:00", "price": {"open": 0, "high": 0, "low": 0, "close": 0}},
  {"datetime": "not a time", "price": {"open": 1, "high": 1, "low": 1, "close": 1}},
  {"datetime": "2026-09-22T09:34:00", "price": {"open": "7779.5", "high": "7780.68", "low": "7778.11", "close": "7779.2"}}
]})";

constexpr std::string_view kDaily = R"({"timestamp": "2026-09-23 02:02:04", "symbol": "_SPX", "data": [
  {"date": "1975-01-02", "volume": "0.0", "open": "0.000000", "high": "70.920000", "low": "68.650000", "close": "70.230000"},
  {"date": "2026-09-22", "volume": "0.0", "open": "7770.810000", "high": "7782.190000", "low": "7756.260000", "close": "7764.640000"},
  {"date": "2026-09-21", "volume": "0.0", "open": "7692.830000", "high": "7779.220000", "low": "7691.190000", "close": "7764.700000"},
  {"date": "2026-02-30", "open": "1", "high": "1", "low": "1", "close": "1"}
]})";

namespace {
md::Timestamp utc(const char* text) { return *md::parse_datetime(text, md::Zone::Utc); }
std::string restamped(std::string_view document, std::string_view timestamp) {
  std::string out(document);
  const auto at = out.find("2026-09-22 19:48:44");
  out.replace(at, 19, timestamp);
  return out;
}
std::string page(std::string_view document) {
  return "<html><script>CTX.Cdn_Data_Time_Gap = 300000;\nCTX.contextOptionsData = " + std::string(document) +
         ";\nCTX.symbolBook = [{\"name\":\"A\"}];</script></html>";
}
}  // namespace

TEST(Cboe, QuotePagesEmbedTheChainDocument) {
  const auto html = page(R"({"timestamp":"15:03:40","data":{"options":[{"option":"SPXW260925C07800000","bid":41.5,)"
                         R"("bid_size":12.0,"ask":42.3,"ask_size":9.0,"note":"a } and a \"quote\" in a string"}],)"
                         R"("symbol":"^SPX","current_price":7685.36,"bid":0,"ask":0,"last_trade_time":"2026-09-24T10:50:18"}})");
  const auto json = providers::cboe_page_chain(html);
  ASSERT_TRUE(json);
  EXPECT_EQ(json->front(), '{');
  EXPECT_EQ(json->substr(json->size() - 3), "\"}}");
  const auto chain = providers::parse_cboe_chain(*json, utc("2026-09-24 15:04:13"));
  EXPECT_EQ(chain.as_of, utc("2026-09-24 15:03:40"));
  ASSERT_EQ(chain.options.size(), 1U);
  EXPECT_EQ(chain.price, 7685.36);
  EXPECT_EQ(chain.last_trade_time, md::new_york_to_utc({2026, 9, 24}, 10, 50, 18));
  // Just after midnight UTC, a time of day later than now was stamped the day before.
  EXPECT_EQ(providers::parse_cboe_chain(*json, utc("2026-09-25 00:01:00")).as_of, utc("2026-09-24 15:03:40"));
  EXPECT_FALSE(providers::cboe_page_chain("<html>no chain here</html>"));
  EXPECT_FALSE(providers::cboe_page_chain("CTX.contextOptionsData = {\"unterminated\": ["));
  EXPECT_EQ(providers::cboe_page_url("SPX"), "https://www.cboe.com/delayed_quotes/spx/quote_table");
}

TEST(Cboe, ReadsTheQuotePageWhileTheCdnFileIsStale) {
  md::Timestamp now = utc("2026-09-24 15:04:13");
  providers::CboeDelayedProvider::Options options;
  options.clock = [&] { return now; };
  providers::CboeDelayedProvider provider(options);
  std::string file = restamped(kChain, "2026-09-23 03:54:59");  // stopped the night before
  std::string quote_page = page(restamped(kChain, "15:03:40"));
  test::HttpStub http;
  http.respond = [&](std::string_view url) {
    return url.find("cdn-api.cboe.com") != std::string_view::npos ? net::HttpResponse{200, file} : net::HttpResponse{200, quote_page};
  };
  const md::Subscription subscription{{"SPX"}, 0, 0.0};
  Collector sink;
  const auto fetched = [&] {
    std::vector<std::string> sources;
    for (const auto& url : http.urls) sources.push_back(url.find("cdn-api.cboe.com") != std::string::npos ? "file" : "page");
    http.urls.clear();
    return sources;
  };
  provider.poll_once(http, "SPX", subscription, sink);
  EXPECT_EQ(fetched(), (std::vector<std::string>{"file", "page"}));
  const auto statuses = sink.all<md::ProviderStatus>();
  ASSERT_FALSE(statuses.empty());
  EXPECT_NE(statuses.back().message.find("(page)"), std::string::npos) << statuses.back().message;
  EXPECT_FALSE(sink.all<md::OptionQuote>().empty());
  // While it holds, the page is read directly, and no more than about once a minute.
  now += 15 * md::kNanosPerSecond;
  provider.poll_once(http, "SPX", subscription, sink);
  EXPECT_TRUE(fetched().empty());
  now += 45 * md::kNanosPerSecond;
  provider.poll_once(http, "SPX", subscription, sink);
  EXPECT_EQ(fetched(), (std::vector<std::string>{"page"}));
  // Five minutes on the file is tried first, and once current it is all that is read.
  now += 5 * 60 * md::kNanosPerSecond;
  file = restamped(kChain, "2026-09-24 15:09:30");
  provider.poll_once(http, "SPX", subscription, sink);
  EXPECT_EQ(fetched(), (std::vector<std::string>{"file"}));
  // Overnight both idle: a page no fresher than the file is left alone for a while.
  now = utc("2026-09-25 03:00:00");
  quote_page = page(restamped(kChain, "15:09:30"));
  provider.poll_once(http, "SPX", subscription, sink);
  EXPECT_EQ(fetched(), (std::vector<std::string>{"file", "page"}));
  now += 15 * md::kNanosPerSecond;
  provider.poll_once(http, "SPX", subscription, sink);
  EXPECT_EQ(fetched(), (std::vector<std::string>{"file"}));
}

TEST(CboeCharts, UrlsFollowTheChainConvention) {
  EXPECT_EQ(providers::cboe_chart_url("SPX", providers::CboeChart::Intraday),
            "https://cdn-api.cboe.com/api/global/delayed_quotes/charts/intraday/_SPX.json");
  for (const auto* symbol : {"SPY", "QQQ", "IWM", "DIA"}) {
    EXPECT_EQ(providers::cboe_chart_url(symbol, providers::CboeChart::Intraday),
              "https://cdn-api.cboe.com/api/global/delayed_quotes/charts/intraday/" + std::string(symbol) + ".json");
    EXPECT_EQ(providers::cboe_chart_url(symbol, providers::CboeChart::Daily),
              "https://cdn-api.cboe.com/api/global/delayed_quotes/charts/historical/" + std::string(symbol) + ".json");
  }
}

TEST(CboeCharts, IntradayBarsStartAMinuteBeforeTheirLabel) {
  const auto bars = providers::parse_cboe_intraday(kIntraday);
  ASSERT_EQ(bars.size(), 3u);
  EXPECT_EQ(bars[0], (md::Bar{md::new_york_to_utc({2026, 9, 22}, 9, 30), 7770.8101, 7773.5801,
                              7769.6001, 7772.6499}));
  EXPECT_EQ(bars[1].start, md::new_york_to_utc({2026, 9, 22}, 9, 31));
  EXPECT_EQ(bars[2], (md::Bar{md::new_york_to_utc({2026, 9, 22}, 9, 33), 7779.5, 7780.68, 7778.11,
                              7779.2}));
  EXPECT_THROW((void)providers::parse_cboe_intraday(R"({"data": {}})"), std::runtime_error);
}

TEST(CboeCharts, VolatilityProxyHistoryRequestsOnlyDailyIndexFiles) {
  providers::CboeChartHistory::Options options;
  options.daily_only = true;
  options.clock = [] { return md::new_york_to_utc({2026, 9, 22}, 11, 0); };
  std::size_t received = 0;
  providers::CboeChartHistory history({"_VIX", "_VXN", "_RVX", "_VXD"},
      [&](const std::string&, providers::CboeChart chart, std::vector<md::Bar> bars) {
        EXPECT_EQ(chart, providers::CboeChart::Daily);
        ASSERT_FALSE(bars.empty());
        ++received;
      }, options);
  test::HttpStub http;
  http.respond = [](std::string_view) { return net::HttpResponse{200, std::string(kDaily)}; };
  const auto next = history.poll_once(http);
  EXPECT_EQ(received, 4u);
  EXPECT_EQ(next, options.clock() + 3600 * md::kNanosPerSecond);
  for (std::size_t i = 0; i < http.urls.size(); ++i)
    EXPECT_NE(http.urls[i].find("/charts/historical/_"), std::string::npos);
  history.poll_once(http);
  EXPECT_EQ(http.urls.size(), 4u);
}

TEST(CboeCharts, DailyBarsRetainCloseOnlyHistoryForRealizedFallback) {
  const auto bars = providers::parse_cboe_daily(kDaily);
  ASSERT_EQ(bars.size(), 3u);
  EXPECT_EQ(bars[0].open, 0);
  EXPECT_EQ(bars[0].close, 70.23);
  EXPECT_EQ(bars[1], (md::Bar{md::new_york_to_utc({2026, 9, 21}, 9, 30), 7692.83, 7779.22, 7691.19,
                              7764.70}));
  EXPECT_EQ(bars[2].start, md::new_york_to_utc({2026, 9, 22}, 9, 30));
  EXPECT_EQ(bars[2].close, 7764.64);
  EXPECT_THROW((void)providers::parse_cboe_daily("{}"), std::runtime_error);
}

TEST(CboeCharts, HistoryFetchesWhatIsDueAndBacksOffFromMissingFiles) {
  md::Timestamp clock = md::new_york_to_utc({2026, 9, 22}, 11, 0);  // regular session
  std::vector<std::tuple<std::string, providers::CboeChart, std::size_t>> received;
  providers::CboeChartHistory::Options options;
  options.clock = [&] { return clock; };
  providers::CboeChartHistory history(
      {"SPX", "XYZ"},
      [&](const std::string& symbol, providers::CboeChart chart, std::vector<md::Bar> bars) {
        received.emplace_back(symbol, chart, bars.size());
      },
      options);
  test::HttpStub http;
  http.respond = [](std::string_view url) {
    if (url.find("XYZ") != std::string_view::npos) return net::HttpResponse{403, "denied"};
    if (url.find("intraday") != std::string_view::npos) return net::HttpResponse{200, std::string(kIntraday)};
    return net::HttpResponse{200, std::string(kDaily)};
  };
  auto next = history.poll_once(http);
  EXPECT_EQ(http.urls.size(), 4u);
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[0], std::make_tuple(std::string("SPX"), providers::CboeChart::Intraday, std::size_t{3}));
  EXPECT_EQ(received[1], std::make_tuple(std::string("SPX"), providers::CboeChart::Daily, std::size_t{3}));
  EXPECT_EQ(next, clock + md::kNanosPerMinute);
  EXPECT_NE(history.error().find("cboe XYZ minute bars: Cboe publishes no chart (HTTP 403)"), std::string::npos);

  // A minute later only the session's minute bars are due; XYZ waits an hour.
  clock += md::kNanosPerMinute;
  next = history.poll_once(http);
  EXPECT_EQ(http.urls.size(), 5u);
  EXPECT_EQ(http.urls.back(), providers::cboe_chart_url("SPX", providers::CboeChart::Intraday));

  // Overnight the minute bars refresh every 15 minutes; a bad document is an error, not a crash.
  clock = md::new_york_to_utc({2026, 9, 22}, 22, 0);
  http.respond = [](std::string_view) { return net::HttpResponse{200, "{}"}; };
  next = history.poll_once(http);
  EXPECT_EQ(next, clock + md::kNanosPerMinute) << "a failure retries after a minute";
  EXPECT_NE(history.error().find("cboe SPX minute bars: Cboe intraday chart: missing data array"), std::string::npos);
  http.respond = [](std::string_view url) {
    return net::HttpResponse{200, std::string(url.find("intraday") != std::string_view::npos ? kIntraday : kDaily)};
  };
  clock += md::kNanosPerMinute;
  next = history.poll_once(http);
  EXPECT_EQ(history.error().find("SPX"), std::string::npos) << history.error();
  EXPECT_EQ(next, clock + 15 * md::kNanosPerMinute);
}
}  // namespace

namespace {
using namespace openport;

/// Cboe's schedule as published on 2026-09-24.
constexpr std::string_view kHolidays2026 = R"(# Generated: 2026:09:24 00:15:58
#
# Start CSV parsing at the line after the "##".
#
##
Holiday Name,Date,Regular Trading Hours,Global Trading Hours
New Year's Day,2026-01-01,None,8:15 PM (Thu) to 9:25 AM (Fri)
Martin Luther King Jr. Day,2026-01-19,None,8:15 PM (Sun) to 11:30 AM (Mon) and 8:15 PM (Mon) to 9:25 AM (Tue)
Presidents' Day,2026-02-16,None,8:15 PM (Sun) to 11:30 AM (Mon) and 8:15 PM (Mon) to 9:25 AM (Tue)
Good Friday,2026-04-03,None,None
Memorial Day,2026-05-25,None,8:15 PM (Sun) to 11:30 AM (Mon) and 8:15 PM (Mon) to 9:25 AM (Tue)
Juneteenth Holiday,2026-06-19,None,8:15 PM (Thu) to 11:30 AM (Fri)
Independence Day Observed,2026-07-03,None,8:15 PM (Thu) to 11:30 AM (Fri)
Labor Day,2026-09-07,None,8:15 PM (Sun) to 11:30 AM (Mon) and 8:15 PM (Mon) to 9:25 AM (Tue)
Thanksgiving Day,2026-11-26,None,8:15 PM (Wed) to 11:30 AM (Thu) and 8:15 PM (Thu) to 9:25 AM (Fri)
Thanksgiving Early Close,2026-11-27,09:30:00 - 13:00:00,8:15 PM (Wed) to 11:30 AM (Thu) and 8:15 PM (Thu) to 9:25 AM (Fri)
Christmas Early Close,2026-12-24,09:30:00 - 13:00:00,8:15 PM (Wed) to 9:25 AM (Thu)
Christmas Day,2026-12-25,None,None
)";

TEST(CboeHolidays, ReadsClosuresEarlyClosesAndOvernightSessionsIntoHolidays) {
  const auto days = providers::parse_cboe_holidays(kHolidays2026);
  ASSERT_EQ(days.size(), 12u);
  EXPECT_EQ(days[0], (md::ScheduledDay{{2026, 1, 1}, "New Year's Day", true, 13, 0}));
  EXPECT_EQ(days[1].overnight_until, 11 * 60 + 30);  // Sunday 20:15 to Monday 11:30
  EXPECT_EQ(days[3].name, "Good Friday");
  EXPECT_EQ(days[3].overnight_until, 0);
  EXPECT_EQ(days[5].overnight_until, 11 * 60 + 30);  // a Friday holiday, from Thursday evening
  EXPECT_EQ(days[9], (md::ScheduledDay{{2026, 11, 27}, "Thanksgiving Early Close", false, 13, 0}));
  EXPECT_EQ(days[11].overnight_until, 0);
  EXPECT_THROW((void)providers::parse_cboe_holidays("<html>moved</html>"), std::runtime_error);
  EXPECT_TRUE(providers::parse_cboe_holidays("Holiday Name,Date,Regular Trading Hours,Global Trading Hours\n").empty());
}

TEST(CboeHolidays, NyseRulesAgreeWithCboesPublishedSchedule) {
  // Every listed day, and every overnight session into a holiday, is what the rules give.
  for (const auto& day : providers::parse_cboe_holidays(kHolidays2026)) {
    const auto noon = md::new_york_to_utc(day.date, 12, 0);
    EXPECT_EQ(md::market_session(noon).open, !day.closed) << day.name;
    if (!day.closed) {
      EXPECT_EQ(md::regular_close_hour(day.date), day.close_hour) << day.name;
    }
    const auto morning = md::trading_session("SPX", md::new_york_to_utc(day.date, 11, 0));
    if (day.closed) {
      EXPECT_EQ(morning.open, day.overnight_until > 0) << day.name;
    }
  }
}

TEST(CboeHolidays, TheScheduleIsFetchedDailyAndKeepsWhatItSaw) {
  md::Timestamp clock = md::new_york_to_utc({2026, 9, 24}, 9, 0);
  std::vector<std::vector<md::ScheduledDay>> received;
  providers::CboeHolidaySchedule::Options options;
  options.clock = [&] { return clock; };
  providers::CboeHolidaySchedule schedule([&](std::vector<md::ScheduledDay> days) { received.push_back(std::move(days)); }, options);
  test::HttpStub http;
  http.respond = [](std::string_view) { return net::HttpResponse{200, std::string(kHolidays2026)}; };
  EXPECT_EQ(schedule.poll_once(http), clock + 24 * 3600 * md::kNanosPerSecond);
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].size(), 12u);
  EXPECT_EQ(http.urls.at(0), providers::kCboeHolidaysUrl);
  EXPECT_EQ(schedule.poll_once(http), clock + 24 * 3600 * md::kNanosPerSecond);  // not due yet
  EXPECT_EQ(http.urls.size(), 1u);
  // A failure is reported and retried within the hour; what was seen stays.
  clock += 24 * 3600 * md::kNanosPerSecond;
  http.respond = [](std::string_view) { return net::HttpResponse{503, ""}; };
  EXPECT_EQ(schedule.poll_once(http), clock + 3600 * md::kNanosPerSecond);
  EXPECT_EQ(schedule.error(), "cboe holiday schedule: HTTP 503");
  EXPECT_EQ(received.size(), 1u);
  // A special closure appears; next year's schedule no longer lists this year's, which stay.
  clock += 3600 * md::kNanosPerSecond;
  http.respond = [](std::string_view) {
    return net::HttpResponse{200, "##\nHoliday Name,Date,Regular Trading Hours,Global Trading Hours\n"
                                  "National Day of Mourning,2026-10-07,None,None\n"};
  };
  schedule.poll_once(http);
  EXPECT_TRUE(schedule.error().empty());
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[1].size(), 13u);
  EXPECT_EQ(received[1][8], (md::ScheduledDay{{2026, 10, 7}, "National Day of Mourning", true, 13, 0}));
}

TEST(Cboe, PublishesReportedVolumeIncludingZeroAndDeduplicatesWithinTheDate) {
  providers::CboeDelayedProvider provider;
  Collector sink;
  auto chain = providers::parse_cboe_chain(kChain);
  provider.publish_chain(chain, {}, sink);
  auto volumes = sink.all<md::OptionVolume>();
  ASSERT_EQ(volumes.size(), 3u);
  EXPECT_EQ(volumes[0].contracts, 873);
  EXPECT_EQ(volumes[1].contracts, 1204);
  EXPECT_EQ(volumes[2].contracts, 0);
  const auto first_time = volumes[0].ts;
  sink.events.clear();
  provider.publish_chain(chain, {}, sink);
  EXPECT_TRUE(sink.all<md::OptionVolume>().empty());
  chain.as_of += md::kNanosPerDay;
  provider.publish_chain(chain, {}, sink);
  volumes = sink.all<md::OptionVolume>();
  ASSERT_EQ(volumes.size(), 3u);
  EXPECT_GT(volumes[0].ts, first_time);
}

TEST(Cboe, AbsentNullAndNegativeVolumesStayUnknown) {
  for (const std::string field : {"", ",\"volume\":null", ",\"volume\":-1"}) {
    const auto chain = providers::parse_cboe_chain(
        "{\"timestamp\":\"2026-09-22 19:33:00\",\"symbol\":\"SPX\",\"data\":{\"options\":["
        "{\"option\":\"SPXW261005C07800000\"" + field + "}]}}");
    ASSERT_EQ(chain.options.size(), 1u);
    EXPECT_TRUE(std::isnan(chain.options[0].volume));
    providers::CboeDelayedProvider provider;
    Collector sink;
    provider.publish_chain(chain, {}, sink);
    EXPECT_TRUE(sink.all<md::OptionVolume>().empty());
  }
}

}  // namespace

TEST(CboeCharts, PublishedIndexShapesSkipZeroMinutesAndKeepDailyCloseOnlyRows) {
  using namespace openport;
  const auto bars = providers::parse_cboe_intraday(R"({"timestamp":"2026-09-25 20:15:21","symbol":"_VIX","data":[
    {"datetime":"2026-09-25T09:31:00","sequence_number":197374702,"price":{"open":0.0,"high":0.0,"low":0.0,"close":0.0},"volume":{}},
    {"datetime":"2026-09-25T16:00:00","price":{"open":14.82,"high":14.84,"low":14.82,"close":14.84},"volume":{}}
  ]})");
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_EQ(bars[0].start, md::new_york_to_utc({2026, 9, 25}, 15, 59));
  EXPECT_DOUBLE_EQ(bars[0].close, 14.84);
  const auto daily = providers::parse_cboe_daily(R"({"timestamp":"2026-09-25 20:15:21","symbol":"_VIX","data":[
    {"date":"1990-01-02","volume":"0.0","open":"17.240000","high":"17.240000","low":"17.240000","close":"17.240000"},
    {"date":"1990-01-03","volume":"0.0","open":"0.000000","high":"18.000000","low":"17.000000","close":"17.500000"}
  ]})");
  ASSERT_EQ(daily.size(), 2u);
  EXPECT_DOUBLE_EQ(daily[0].close, 17.24);
  EXPECT_FALSE(md::valid_bar(daily[1]));
  for (const auto* symbol : {"_VIX", "_VIX9D", "_VIX3M", "_VIX6M", "_VIX1Y"})
    EXPECT_EQ(providers::cboe_chart_url(symbol, providers::CboeChart::Intraday),
              "https://cdn-api.cboe.com/api/global/delayed_quotes/charts/intraday/" + std::string(symbol) + ".json");
}

namespace {
using namespace openport;

// Recorded from cdn.cboe.com on 2026-10-21 (the history trimmed to its header, first
// row and last six rows; the quotes as served, after the SOQs were computed that morning).
constexpr std::string_view kSetHistory = "DATE,SET\n07/09/1998,1162.440000\n10/14/2026,7771.780000\n"
    "10/15/2026,7690.080000\n10/16/2026,7757.090000\n10/19/2026,7700.250000\n10/20/2026,7681.230000\n"
    "10/21/2026,7736.880000\n";
constexpr std::string_view kSetQuote = R"({"timestamp": "2026-10-21 22:38:45", "data": {"symbol": "^SET", "security_type": "index", "exchange_id": 5, "current_price": 7736.8799, "price_change": 55.6499, "price_change_percent": 0.7245, "bid": 0.0, "ask": 0.0, "bid_size": 0, "ask_size": 0, "open": 7736.8799, "high": 7736.8799, "low": 7736.8799, "close": 7736.8799, "prev_day_close": 7681.23, "volume": 0, "iv30": 0.0, "iv30_change": 0.0, "iv30_change_percent": null, "seqno": 20933921831, "last_trade_time": "2026-10-21T10:35:51", "tick": "up"}, "symbol": "_SET"})";
constexpr std::string_view kRlsQuote = R"({"timestamp": "2026-10-21 22:40:46", "data": {"symbol": "^RLS", "security_type": "index", "exchange_id": 5, "current_price": 2787.0601, "price_change": 56.4401, "price_change_percent": 2.0669, "bid": 0.0, "ask": 0.0, "bid_size": 0, "ask_size": 0, "open": 2787.0601, "high": 2787.0601, "low": 2787.0601, "close": 2787.0601, "prev_day_close": 2730.6201, "volume": 0, "iv30": 0.0, "iv30_change": 0.0, "iv30_change_percent": null, "seqno": 21456146304, "last_trade_time": "2026-10-21T11:01:19", "tick": "up"}, "symbol": "_RLS"})";

TEST(CboeSoq, MapsAmSettledUnderlyingsToTheirPublishedIndices) {
  EXPECT_EQ(providers::cboe_soq_index("SPX"), "SET");
  EXPECT_EQ(providers::cboe_soq_index("RUT"), "RLS");
  EXPECT_EQ(providers::cboe_soq_index("VIX"), "VRO");
  EXPECT_TRUE(providers::cboe_soq_index("NDX").empty());
  EXPECT_TRUE(providers::cboe_soq_index("SPY").empty());
  EXPECT_EQ(providers::cboe_soq_history_url("SET"), "https://cdn.cboe.com/api/global/us_indices/daily_prices/SET_History.csv");
}

TEST(CboeSoq, ReadsTheHistoryFromADateOn) {
  const auto rows = providers::parse_cboe_soq_history(kSetHistory, "SPX", "SET", {2026, 10, 16});
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_EQ(rows[0].date, (md::Date{2026, 10, 16}));
  EXPECT_DOUBLE_EQ(rows[0].value, 7757.09);  // the October monthly's settlement
  EXPECT_EQ(rows[0].underlying, "SPX");
  EXPECT_EQ(rows[3].date, (md::Date{2026, 10, 21}));
  EXPECT_EQ(providers::parse_cboe_soq_history(kSetHistory, "SPX", "SET").size(), 7u);
  // Junk rows are skipped; another index's document or a page is refused.
  EXPECT_EQ(providers::parse_cboe_soq_history("DATE,SET\n10/21/2026,\nnot,a row\n10/22/2026,-1\n10/23/2026,7700.5\n", "SPX", "SET").size(), 1u);
  EXPECT_THROW((void)providers::parse_cboe_soq_history(kSetHistory, "RUT", "RLS"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_cboe_soq_history("<html>moved</html>", "SPX", "SET"), std::runtime_error);
}

TEST(CboeSoq, HoldsOnlyTheHistoryRowOfADateFetchedEachInterval) {
  md::Timestamp clock = md::new_york_to_utc({2026, 10, 21}, 11, 0);
  std::vector<std::vector<providers::SettlementQuotation>> received;
  providers::CboeSettlementQuotations::Options options;
  options.clock = [&] { return clock; };
  options.lookback_days = 3;
  providers::CboeSettlementQuotations soq({"SPX", "SPY", "RUT", "SPX"},
      [&](std::vector<providers::SettlementQuotation> all) { received.push_back(std::move(all)); }, options);
  EXPECT_EQ(soq.underlyings(), (std::vector<std::string>{"SPX", "RUT"}));
  test::HttpStub http;
  // The delayed quote already shows 10/21's value; the history does not have it yet.
  http.respond = [](std::string_view url) {
    if (url.ends_with("SET_History.csv")) return net::HttpResponse{200, std::string(kSetHistory.substr(0, kSetHistory.find("10/21")))};
    if (url.ends_with("_SET.json")) return net::HttpResponse{200, std::string(kSetQuote)};
    if (url.ends_with("_RLS.json")) return net::HttpResponse{200, std::string(kRlsQuote)};
    return net::HttpResponse{404, ""};
  };
  EXPECT_EQ(soq.poll_once(http), clock + 15 * 60 * md::kNanosPerSecond);
  EXPECT_EQ(soq.error(), "cboe RLS history: HTTP 404");
  for (const auto& url : http.urls) EXPECT_TRUE(url.ends_with("_History.csv")) << url;
  // 10/19 and 10/20 from the history; older rows fall outside the lookback, and 10/21
  // waits for its row.
  ASSERT_EQ(received.size(), 1u);
  ASSERT_EQ(received[0].size(), 2u);
  EXPECT_EQ(received[0][1].date, (md::Date{2026, 10, 20}));
  EXPECT_EQ(received[0][1].underlying, "SPX");
  // Nothing new: no publication.
  clock += 15 * 60 * md::kNanosPerSecond;
  soq.poll_once(http);
  EXPECT_EQ(received.size(), 1u);
  // The row appears.
  http.respond = [](std::string_view url) {
    if (url.ends_with("SET_History.csv")) return net::HttpResponse{200, "DATE,SET\n10/21/2026,7736.880000\n"};
    if (url.ends_with("RLS_History.csv")) return net::HttpResponse{200, "DATE,RLS\n10/21/2026,2787.060100\n"};
    return net::HttpResponse{503, ""};
  };
  clock += 15 * 60 * md::kNanosPerSecond;
  soq.poll_once(http);
  EXPECT_TRUE(soq.error().empty()) << soq.error();
  ASSERT_EQ(received.size(), 2u);
  ASSERT_EQ(received[1].size(), 4u);
  EXPECT_EQ(received[1][0].index, "RLS");
  EXPECT_DOUBLE_EQ(received[1][0].value, 2787.0601);
  EXPECT_EQ(received[1][3].date, (md::Date{2026, 10, 21}));
  EXPECT_DOUBLE_EQ(received[1][3].value, 7736.88);
}
}  // namespace
