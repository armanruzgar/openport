#include "openport/providers/massive.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <stdexcept>
#include <vector>

#include "support/http_stub.hpp"

namespace {

using namespace openport;

// A hand-written page shaped like Massive's /v3/snapshot/options response, with the
// fields in a different order from the documentation to check order independence.
constexpr std::string_view kPage = R"({
  "status": "OK",
  "request_id": "abc",
  "results": [
    {
      "greeks": {"delta": -0.0484, "gamma": 0.0004, "theta": -0.8494, "vega": 1.5783},
      "details": {"contract_type": "put", "exercise_style": "european",
                  "expiration_date": "2036-10-06", "shares_per_contract": 100,
                  "strike_price": 7405, "ticker": "O:SPXW361006P07405000"},
      "implied_volatility": 0.1575,
      "last_quote": {"ask": 4.6, "ask_size": 172, "bid": 4.4, "bid_size": 314,
                     "last_updated": 1790103279803625000, "midpoint": 4.5, "timeframe": "REAL-TIME"},
      "open_interest": 7,
      "underlying_asset": {"price": 7777.27, "ticker": "I:SPX", "last_updated": 1790103279000000000,
                           "timeframe": "REAL-TIME"}
    },
    {
      "details": {"ticker": "O:SPXW361006C09000000", "exercise_style": "european",
                  "shares_per_contract": 100},
      "last_quote": {"bid": 0.05, "ask": 0.1, "bid_size": 10, "ask_size": 20,
                     "last_updated": 1790103279000000000, "timeframe": "REAL-TIME"},
      "open_interest": 1200
    }
  ],
  "next_url": "https://api.massive.com/v3/snapshot/options/I:SPX?cursor=YXA9"
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

TEST(Massive, ParsesAPageInAnyFieldOrder) {
  const providers::MassivePage page = providers::parse_massive_chain_page(kPage);
  ASSERT_EQ(page.contracts.size(), 2u);
  EXPECT_EQ(page.next_url, "https://api.massive.com/v3/snapshot/options/I:SPX?cursor=YXA9");
  EXPECT_DOUBLE_EQ(page.underlying_price, 7777.27);

  const auto& put = page.contracts[0];
  EXPECT_EQ(put.symbol, "SPXW361006P07405000");
  EXPECT_TRUE(put.european);
  EXPECT_TRUE(put.has_quote);
  EXPECT_TRUE(put.realtime);
  EXPECT_DOUBLE_EQ(put.bid, 4.4);
  EXPECT_DOUBLE_EQ(put.ask_size, 172.0);
  EXPECT_EQ(put.quote_ts, 1790103279803625000);  // kept exact, not rounded through a double
  EXPECT_DOUBLE_EQ(put.iv, 0.1575);
  EXPECT_DOUBLE_EQ(put.vega, 1.5783);
  EXPECT_DOUBLE_EQ(put.open_interest, 7.0);

  EXPECT_DOUBLE_EQ(page.contracts[1].iv, 0.0);  // no greeks for this one
}

TEST(Massive, ErrorResponsesThrow) {
  EXPECT_THROW((void)providers::parse_massive_chain_page(
                   R"({"status":"ERROR","request_id":"x","error":"Unknown API Key"})"),
               std::runtime_error);
  EXPECT_THROW((void)providers::parse_massive_chain_page(R"({"status":"OK"})"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_massive_chain_page(
                   R"({"status":"NOT_AUTHORIZED","message":"You are not entitled to this data."})"),
               std::runtime_error);
}

TEST(Massive, PublishesTheChainWithRhoMarkedUnpublished) {
  providers::MassiveProvider provider({.api_key = "test"});
  Collector sink;
  const providers::MassivePage page = providers::parse_massive_chain_page(kPage);
  provider.publish_chain("SPX", page.contracts, page.underlying_price, page.underlying_ts, {}, sink);

  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 2u);
  const auto greeks = sink.all<md::VendorGreeks>();
  ASSERT_EQ(greeks.size(), 1u);
  EXPECT_DOUBLE_EQ(greeks[0].theta, -0.8494);
  EXPECT_TRUE(std::isnan(greeks[0].rho));
  EXPECT_EQ(sink.all<md::UnderlyingQuote>().at(0).symbol, "SPX");
}

TEST(Massive, IndexUnderlyingsUseTheIPrefix) {
  EXPECT_EQ(providers::massive_chain_url("https://api.massive.com", "SPX"),
            "https://api.massive.com/v3/snapshot/options/I:SPX?limit=250");
  EXPECT_EQ(providers::massive_chain_url("https://api.massive.com", "SPY"),
            "https://api.massive.com/v3/snapshot/options/SPY?limit=250");
}

TEST(Massive, RequiresAKey) {
  EXPECT_THROW(providers::MassiveProvider({}), std::invalid_argument);
}

TEST(Massive, NullAndMissingNestedObjectsDoNotDiscardOtherContracts) {
  const auto page = providers::parse_massive_chain_page(R"({"results":[
    {"details":null,"greeks":null,"last_quote":null,"underlying_asset":null},
    {"details":{"ticker":"O:SPY991218C00500000"},"greeks":null,"last_quote":null,"underlying_asset":null},
    {"details":{"ticker":"O:SPY991218P00500000"}}
  ]})");
  ASSERT_EQ(page.contracts.size(), 2u);
  EXPECT_FALSE(page.contracts[0].has_quote);
  EXPECT_FALSE(page.contracts[1].has_quote);
  EXPECT_EQ(page.underlying_price, 0.0);
}

TEST(Massive, CapabilitiesDoNotPromiseARealtimePlan) {
  providers::MassiveProvider provider({.api_key = "test"});
  EXPECT_FALSE(provider.capabilities().realtime);
  EXPECT_TRUE(provider.capabilities().realtime_plan_dependent);
}

TEST(Massive, ReportsEachUnderlyingsActualEntitlement) {
  providers::MassiveProvider provider({.api_key = "test"});
  test::HttpStub http;
  bool realtime = false;
  http.respond = [&](std::string_view) -> net::HttpResponse {
    std::string body(kPage);
    if (!realtime) {
      for (auto pos = body.find("REAL-TIME"); pos != std::string::npos;
           pos = body.find("REAL-TIME")) {
        body.replace(pos, 9, "DELAYED");
      }
    }
    return {200, body.substr(0, body.find(",\n  \"next_url\"")) + "}"};
  };
  Collector sink;
  provider.poll_once(http, "SPX", {}, sink);
  realtime = true;
  provider.poll_once(http, "SPY", {}, sink);
  const auto statuses = sink.all<md::ProviderStatus>();
  ASSERT_EQ(statuses.size(), 2u);
  EXPECT_EQ(statuses[0].state, md::FeedState::Delayed);
  EXPECT_EQ(statuses[0].underlying, "SPX");
  EXPECT_EQ(statuses[1].state, md::FeedState::Live);
  EXPECT_EQ(statuses[1].underlying, "SPY");
  EXPECT_FALSE(provider.capabilities().realtime);
}

TEST(Massive, MissingQuoteClearsPreviouslyPublishedPrices) {
  providers::MassiveProvider provider({.api_key = "test"});
  auto contracts = providers::parse_massive_chain_page(kPage).contracts;
  Collector sink;
  provider.publish_chain("SPX", contracts, 7777, 1, {}, sink);
  sink.events.clear();
  contracts[0].has_quote = false;
  provider.publish_chain("SPX", contracts, 7777, 2, {}, sink);
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_EQ(quotes[0].bid + quotes[0].ask, 0);
}

TEST(Massive, FailedPaginationDoesNotRetireTheLastCompleteSnapshot) {
  providers::MassiveProvider provider({.api_key = "test"});
  auto page = providers::parse_massive_chain_page(kPage);
  Collector sink;
  provider.publish_chain("SPX", page.contracts, 7777, 1, {}, sink);
  sink.events.clear();
  test::HttpStub http;
  int requests = 0;
  http.respond = [&](std::string_view) -> net::HttpResponse {
    if (++requests == 1)
      return {200, R"({"results":[],"next_url":"https://api.massive.com/next"})"};
    return {503, "offline"};
  };
  provider.poll_once(http, "SPX", {}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  ASSERT_EQ(sink.all<md::ProviderStatus>().size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Error);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].underlying, "SPX");
  sink.events.clear();
  provider.publish_chain("SPX", page.contracts, 7777, 2, {}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());  // the last prices were retained
}

TEST(Massive, PublishedContractsSurviveFilterDriftAndRetireWhenMissing) {
  providers::MassiveProvider provider({.api_key = "test"});
  auto contracts = providers::parse_massive_chain_page(kPage).contracts;
  md::Subscription sub;
  sub.strike_window = 0.05;
  Collector sink;
  provider.publish_chain("SPX", contracts, 7405, 1, sub, sink);
  ASSERT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  const auto id = sink.all<md::ContractDefinition>()[0].id;
  sink.events.clear();
  contracts[0].bid = 4.5;
  provider.publish_chain("SPX", contracts, 9000, 2, sub, sink);
  const auto changed = sink.all<md::OptionQuote>();
  ASSERT_EQ(changed.size(), 2u);
  EXPECT_EQ(changed[0].id, id);
  EXPECT_EQ(changed[0].bid, 4.5);
  sink.events.clear();
  provider.publish_chain("SPX", {}, 9000, 3, sub, sink);
  const auto retired = sink.all<md::OptionQuote>();
  ASSERT_EQ(retired.size(), 2u);
  for (const auto& q : retired) EXPECT_EQ(q.bid + q.ask, 0.0);
}

TEST(MassiveDividends, ReadsTheDividendsEndpoint) {
  EXPECT_EQ(providers::massive_dividends_url("https://api.massive.com", "SPY", {2026, 8, 24}),
            "https://api.massive.com/stocks/v1/dividends?ticker=SPY&ex_dividend_date.gte=2026-08-24"
            "&sort=ex_dividend_date.asc&limit=1000");
  // The example from Massive's documentation, and a row without an amount.
  const auto page = providers::parse_massive_dividends(R"({"request_id": 1, "results": [
    {"cash_amount": 0.26, "currency": "USD", "declaration_date": "2025-07-31", "distribution_type": "recurring",
     "ex_dividend_date": "2025-08-11", "frequency": 4, "historical_adjustment_factor": 0.997899,
     "id": "Ed2c9da6", "pay_date": "2025-08-14", "record_date": "2025-08-11", "split_adjusted_cash_amount": 0.26,
     "ticker": "AAPL"},
    {"ticker": "AAPL", "ex_dividend_date": "2025-11-10", "cash_amount": null}],
    "status": "OK", "next_url": "https://api.massive.com/stocks/v1/dividends?cursor=abc"})");
  ASSERT_EQ(page.dividends.size(), 1u);
  EXPECT_EQ(page.dividends[0].ticker, "AAPL");
  EXPECT_EQ(page.dividends[0].ex_date, (md::Date{2025, 8, 11}));
  EXPECT_DOUBLE_EQ(page.dividends[0].cash_amount, 0.26);
  EXPECT_EQ(page.dividends[0].currency, "USD");
  EXPECT_EQ(page.next_url, "https://api.massive.com/stocks/v1/dividends?cursor=abc");
  EXPECT_THROW((void)providers::parse_massive_dividends(R"({"status": "NOT_AUTHORIZED", "message": "plan"})"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_massive_dividends(R"({"status": "OK"})"), std::runtime_error);
}

TEST(MassiveDividends, FetchesEachTickerAndKeepsWhatAFailureCannotRefresh) {
  md::Timestamp clock = md::new_york_to_utc({2026, 9, 24}, 12, 0);
  std::vector<std::vector<providers::MassiveDividend>> received;
  providers::MassiveDividends::Options options;
  options.api_key = "key";
  options.clock = [&] { return clock; };
  providers::MassiveDividends dividends({"SPY", "QQQ"}, [&](auto found) { received.push_back(std::move(found)); }, options);
  test::HttpStub http;
  bool qqq_fails = true;
  http.respond = [&](std::string_view url) {
    if (url.find("cursor=2") != std::string_view::npos)
      return net::HttpResponse{200, R"({"status": "OK", "results": [
        {"ticker": "SPY", "ex_dividend_date": "2026-12-18", "cash_amount": 1.9, "currency": "USD"}]})"};
    if (url.find("SPY") != std::string_view::npos)
      return net::HttpResponse{200, R"({"status": "OK", "next_url": "https://api.massive.com/stocks/v1/dividends?cursor=2",
        "results": [{"ticker": "SPY", "ex_dividend_date": "2026-09-18", "cash_amount": 1.83, "currency": "USD"},
                    {"ticker": "SPY", "ex_dividend_date": "2026-09-18", "cash_amount": 2.5, "currency": "CAD"},
                    {"ticker": "SPYG", "ex_dividend_date": "2026-09-18", "cash_amount": 0.2, "currency": "USD"}]})"};
    if (qqq_fails) return net::HttpResponse{429, "slow down"};
    return net::HttpResponse{200, R"({"status": "OK", "results": [
      {"ticker": "QQQ", "ex_dividend_date": "2026-09-21", "cash_amount": 0.71, "currency": "USD"}]})"};
  };
  // A month back from today, both pages of SPY; QQQ fails and is retried within the hour.
  EXPECT_EQ(dividends.poll_once(http), clock + 3600 * md::kNanosPerSecond);
  EXPECT_NE(http.urls.at(0).find("ex_dividend_date.gte=2026-08-24"), std::string::npos);
  EXPECT_EQ(dividends.error(), "massive dividends QQQ: HTTP 429");
  ASSERT_EQ(received.size(), 1u);
  ASSERT_EQ(received[0].size(), 2u);  // US dollars and SPY's own only
  EXPECT_EQ(received[0][1].ex_date, (md::Date{2026, 12, 18}));
  clock += 3600 * md::kNanosPerSecond;
  qqq_fails = false;
  EXPECT_EQ(dividends.poll_once(http), clock + 6 * 3600 * md::kNanosPerSecond);
  EXPECT_TRUE(dividends.error().empty());
  ASSERT_EQ(received.size(), 2u);
  EXPECT_EQ(received[1].size(), 3u);
  // Later failures keep the dividends already known.
  clock += 6 * 3600 * md::kNanosPerSecond;
  http.respond = [](std::string_view) { return net::HttpResponse{500, ""}; };
  dividends.poll_once(http);
  ASSERT_EQ(received.size(), 3u);
  EXPECT_EQ(received[2].size(), 3u);
  EXPECT_THROW(providers::MassiveDividends({"SPY"}, [](auto) {}, {}), std::invalid_argument);
}


TEST(Massive, DayVolumeUsesItsOwnClockAndMissingValuesStayUnknown) {
  for (const std::string value : {"0", "873", "null", "-1"}) {
    const auto page = providers::parse_massive_chain_page(
        "{\"status\":\"OK\",\"results\":[{\"details\":{\"ticker\":\"O:SPXW361006P07405000\"},"
        "\"day\":{\"volume\":" + value + ",\"last_updated\":1790103279000000000}}]}");
    providers::MassiveProvider provider({"test-key"});
    Collector sink;
    provider.publish_chain("SPX", page.contracts, 7777, 1790103379000000000, {}, sink);
    const auto volumes = sink.all<md::OptionVolume>();
    if (value == "null" || value == "-1") {
      EXPECT_TRUE(volumes.empty());
    } else {
      ASSERT_EQ(volumes.size(), 1u);
      EXPECT_EQ(volumes[0].contracts, std::stod(value));
      EXPECT_EQ(volumes[0].ts, 1790103279000000000);
    }
  }
  const auto page = providers::parse_massive_chain_page(kPage);
  EXPECT_FALSE(page.contracts[0].volume);
}

}  // namespace
