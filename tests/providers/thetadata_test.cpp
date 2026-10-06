#include "openport/providers/thetadata.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "support/http_stub.hpp"

namespace {

using namespace openport;

constexpr std::string_view kQuotes =
    R"({"timestamp":"2026-09-22T15:33:42.125","symbol":"SPXW","expiration":"2036-10-06","strike":7405.0,"right":"put","bid_size":314,"bid_exchange":5,"bid":4.4,"bid_condition":50,"ask_size":172,"ask_exchange":5,"ask":4.6,"ask_condition":50}
{"timestamp":"2026-09-22T15:33:42.125","symbol":"SPXW","expiration":"20361006","strike":7800.0,"right":"call","bid_size":12,"bid":41.5,"ask_size":9,"ask":42.3}
{"timestamp":"2026-09-22T15:33:41.000","symbol":"SPX","expiration":"2036-10-17","strike":8000.0,"right":"call","bid":70.0,"ask":72.4,"bid_size":3,"ask_size":3}
)";

constexpr std::string_view kImpliedVols =
    R"({"symbol":"SPXW","expiration":"2036-10-06","strike":7405.0,"right":"put","timestamp":"2026-09-22T15:33:42.125","bid":4.4,"ask":4.6,"implied_vol":0.1575,"iv_error":0.0001,"underlying_timestamp":"2026-09-22T15:33:42.100","underlying_price":7777.27}
)";

constexpr std::string_view kOpenInterest =
    R"({"symbol":"SPXW","expiration":"2036-10-06","strike":7405.0,"right":"put","timestamp":"2026-09-22T06:30:00.000","open_interest":1520}
)";

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

TEST(ThetaData, ParsesNdjsonRows) {
  const auto rows = providers::parse_theta_rows(kQuotes);
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].root, "SPXW");
  EXPECT_EQ(rows[0].expiry, (md::Date{2036, 10, 6}));
  EXPECT_EQ(rows[1].expiry, (md::Date{2036, 10, 6}));  // YYYYMMDD also accepted
  EXPECT_DOUBLE_EQ(rows[0].strike, 7405.0);
  EXPECT_EQ(rows[0].type, pricing::OptionType::Put);
  EXPECT_DOUBLE_EQ(rows[0].bid, 4.4);
  EXPECT_DOUBLE_EQ(rows[0].ask_size, 172.0);
  // New York wall clock, with milliseconds.
  EXPECT_EQ(md::format_timestamp(rows[0].ts), "2026-09-22T19:33:42.125Z");
}

TEST(ThetaData, EmptyResponsesParseToNothing) {
  EXPECT_TRUE(providers::parse_theta_rows("").empty());
  EXPECT_TRUE(providers::parse_theta_rows("\n").empty());
}

TEST(ThetaData, MergesQuotesImpliedVolsAndOpenInterest) {
  providers::ThetaDataProvider provider;
  Collector sink;
  provider.publish_chain("SPX", providers::parse_theta_rows(kQuotes),
                         providers::parse_theta_rows(kImpliedVols),
                         providers::parse_theta_rows(kOpenInterest), {}, sink);

  const auto definitions = sink.all<md::ContractDefinition>();
  ASSERT_EQ(definitions.size(), 3u);
  EXPECT_EQ(definitions[0].contract.osi_symbol(), "SPXW  361006P07405000");
  EXPECT_EQ(definitions[2].contract.settlement, md::Settlement::AM);  // monthly SPX

  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 3u);
  const auto greeks = sink.all<md::VendorGreeks>();
  ASSERT_EQ(greeks.size(), 1u);
  EXPECT_DOUBLE_EQ(greeks[0].iv, 0.1575);
  EXPECT_TRUE(std::isnan(greeks[0].vega));  // undocumented units are not guessed

  const auto oi = sink.all<md::OpenInterest>();
  ASSERT_EQ(oi.size(), 1u);
  EXPECT_DOUBLE_EQ(oi[0].contracts, 1520.0);

  const auto spot = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(spot.size(), 1u);
  EXPECT_DOUBLE_EQ(spot[0].last, 7777.27);
  EXPECT_EQ(md::format_timestamp(spot[0].ts), "2026-09-22T19:33:42.100Z");
}

TEST(ThetaData, UsesTheDocumentedIvRoute) {
  providers::ThetaDataProvider provider;
  test::HttpStub http;
  http.respond = [](std::string_view url) -> net::HttpResponse {
    if (url.find("/snapshot/quote?") != std::string_view::npos) return {200, std::string(kQuotes)};
    if (url.find("/snapshot/greeks/implied_volatility?") != std::string_view::npos) {
      return {200, std::string(kImpliedVols)};
    }
    if (url.find("/snapshot/open_interest?") != std::string_view::npos) return {200, {}};
    return {404, "unknown endpoint"};
  };
  Collector sink;
  provider.poll_once(http, "SPX", {}, sink);
  EXPECT_FALSE(sink.all<md::VendorGreeks>().empty());
  ASSERT_EQ(sink.all<md::ProviderStatus>().size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Live);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].underlying, "SPX");
}

TEST(ThetaData, AuxiliaryFailuresPreserveQuotesAndReportDegradation) {
  providers::ThetaDataProvider provider;
  test::HttpStub http;
  http.respond = [](std::string_view url) -> net::HttpResponse {
    if (url.find("/snapshot/quote?") != std::string_view::npos) return {200, std::string(kQuotes)};
    return {503, "unavailable"};
  };
  Collector sink;
  provider.poll_once(http, "SPX", {}, sink);
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 3u);
  const auto status = sink.all<md::ProviderStatus>();
  ASSERT_EQ(status.size(), 1u);
  EXPECT_EQ(status[0].state, md::FeedState::Live);
  EXPECT_NE(status[0].message.find("degraded SPX greeks/implied_volatility"), std::string::npos);
  EXPECT_NE(status[0].message.find("open_interest"), std::string::npos);
  EXPECT_NE(status[0].message.find("HTTP 503"), std::string::npos);
}

TEST(ThetaData, RefreshesOpenInterestIndependentlyForEachUnderlying) {
  providers::ThetaDataProvider provider({.open_interest_every = 2});
  test::HttpStub http;
  http.respond = [](std::string_view) -> net::HttpResponse { return {200, {}}; };
  Collector sink;
  for (int i = 0; i < 3; ++i) {
    provider.poll_once(http, "SPY", {}, sink);
    provider.poll_once(http, "QQQ", {}, sink);
  }
  for (const std::string symbol : {"SPY", "QQQ"}) {
    std::size_t count = 0;
    for (const auto& url : http.urls) {
      if (url.find("/open_interest?symbol=" + symbol) != std::string::npos) ++count;
    }
    EXPECT_EQ(count, 2u) << symbol;
  }
}

TEST(ThetaData, SelectsSpotByUnderlyingTimestampAndIgnoresMissingClocks) {
  auto rows = providers::parse_theta_rows(kImpliedVols);
  ASSERT_EQ(rows.size(), 1u);
  const auto expected = rows[0].underlying_ts;
  auto older_spot = rows[0];
  older_spot.ts += md::kNanosPerSecond;
  older_spot.underlying_ts -= md::kNanosPerSecond;
  older_spot.underlying_price = 100;
  rows.push_back(older_spot);
  older_spot.underlying_ts = 0;
  rows.push_back(older_spot);
  providers::ThetaDataProvider provider;
  Collector sink;
  provider.publish_chain("SPX", {}, rows, {}, {}, sink);
  const auto spots = sink.all<md::UnderlyingQuote>();
  ASSERT_EQ(spots.size(), 1u);
  EXPECT_EQ(spots[0].ts, expected);
  EXPECT_EQ(spots[0].last, 7777.27);
}

TEST(ThetaData, KnownContractsSurviveExpiryFilterDriftAndMissingQuotesAreRetired) {
  providers::ThetaDataProvider provider;
  providers::ThetaRow row;
  row.root = "SPY";
  row.expiry = {2099, 12, 18};
  row.strike = 500;
  row.bid = 5;
  row.ask = 6;
  md::Subscription sub;
  sub.max_expiries = 1;
  Collector sink;
  provider.publish_chain("SPY", {row}, {}, {}, sub, sink);
  sink.events.clear();
  auto earlier = row;
  earlier.expiry = {2099, 11, 20};
  row.bid = 5.5;
  provider.publish_chain("SPY", {row, earlier}, {}, {}, sub, sink);
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 2u);
  EXPECT_EQ(quotes[0].id, 0u);
  EXPECT_EQ(quotes[0].bid, 5.5);
  sink.events.clear();
  provider.publish_chain("SPY", {}, {}, {}, sub, sink);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  for (const auto& quote : sink.all<md::OptionQuote>()) EXPECT_EQ(quote.bid + quote.ask, 0);
}

TEST(ThetaData, ASnapshotIsStampedWithItsDataNotTheWallClock) {
  // On a delayed plan the wall clock runs ahead of the data, and a snapshot stamped
  // with it would move the market clock ahead.
  providers::ThetaDataProvider provider;
  providers::ThetaRow row;
  row.root = "SPY";
  row.expiry = {2099, 12, 18};
  row.strike = 500;
  row.bid = 5;
  row.ask = 6;
  row.ts = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  auto later = row;
  later.strike = 510;
  later.ts = row.ts + 30 * md::kNanosPerSecond;
  Collector sink;
  provider.publish_chain("SPY", {row, later}, {}, {}, {}, sink);
  const auto marks = sink.all<md::SnapshotComplete>();
  ASSERT_EQ(marks.size(), 1u);
  EXPECT_EQ(marks[0].ts, later.ts);
  // An empty snapshot retires the quotes at the last snapshot's time.
  sink.events.clear();
  provider.publish_chain("SPY", {}, {}, {}, {}, sink);
  for (const auto& quote : sink.all<md::OptionQuote>()) EXPECT_EQ(quote.ts, later.ts);
  ASSERT_EQ(sink.all<md::SnapshotComplete>().size(), 1u);
  EXPECT_EQ(sink.all<md::SnapshotComplete>()[0].ts, later.ts);
}

TEST(ThetaData, AdjustedRootsAreMarkedNonstandard) {
  providers::ThetaDataProvider provider;
  auto row = providers::parse_theta_rows(kQuotes)[0];
  row.root = "SPY1";
  Collector sink;
  provider.publish_chain("SPY", {row}, {}, {}, {}, sink);
  ASSERT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  EXPECT_FALSE(sink.all<md::ContractDefinition>()[0].contract.standard);
}

TEST(ThetaData, AFailedQuoteRootDoesNotRetireTheLastCompleteSnapshot) {
  providers::ThetaDataProvider provider;
  Collector sink;
  provider.publish_chain("SPX", providers::parse_theta_rows(kQuotes), {}, {}, {}, sink);
  sink.events.clear();
  test::HttpStub http;
  http.respond = [](std::string_view url) -> net::HttpResponse {
    if (url.find("/quote?symbol=SPXW") != std::string_view::npos) return {503, "offline"};
    return {200, {}};
  };
  provider.poll_once(http, "SPX", {}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  ASSERT_EQ(sink.all<md::ProviderStatus>().size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Error);
}


TEST(ThetaData, DoesNotInventVolumeAndMapsItOnlyWhenSupplied) {
  providers::ThetaDataProvider provider;
  Collector sink;
  provider.publish_chain("SPX", providers::parse_theta_rows(kQuotes), {}, {}, {}, sink);
  EXPECT_TRUE(sink.all<md::OptionVolume>().empty());
  for (const std::string value : {"0", "321", "null", "-1"}) {
    const auto rows = providers::parse_theta_rows(
        "{\"timestamp\":\"2026-09-22T15:33:42.125\",\"symbol\":\"SPXW\","
        "\"expiration\":\"2036-10-06\",\"strike\":7405,\"right\":\"put\",\"volume\":" + value + "}");
    sink.events.clear();
    provider.publish_chain("SPX", rows, {}, {}, {}, sink);
    const auto volumes = sink.all<md::OptionVolume>();
    if (value == "null" || value == "-1") {
      EXPECT_TRUE(volumes.empty());
    } else {
      ASSERT_EQ(volumes.size(), 1u);
      EXPECT_EQ(volumes[0].contracts, std::stod(value));
      EXPECT_EQ(volumes[0].ts, rows[0].ts);
    }
  }
}

}  // namespace
