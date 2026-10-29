#include "openport/providers/ibkr.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <memory>

#include "openport/providers/factory.hpp"
#include "support/broker_sink.hpp"
#include "support/http_stub.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
constexpr md::Timestamp kNow = 1790263800000LL * 1'000'000;  // Thu 2026-09-24 11:30 New York
constexpr std::string_view kBase = "https://localhost:5000/v1/api";

// Shaped after the Client Portal Web API reference's examples, with generated
// SPY values and contract ids; not observed market data.
constexpr std::string_view kTickle = R"({"session":"generated","ssoExpires":460000,"collission":false,"userId":1,
  "hmds":{"error":"no bridge"},"iserver":{"authStatus":{"authenticated":true,"competing":false,"connected":true,
  "message":"","MAC":"00:00:00:00:00:00"}}})";
constexpr std::string_view kSearch = R"([{"conid":"756733","companyHeader":"SPDR S&P 500 ETF TRUST - ARCA",
  "companyName":"SPDR S&P 500 ETF TRUST","symbol":"SPY","description":"ARCA","restricted":null,"fop":null,
  "opt":"20260925;20260930;20261016","war":null,"sections":[{"secType":"STK"},
  {"secType":"OPT","months":"OCT26;SEP26;AUG26","exchange":"SMART;AMEX;CBOE"}]},
  {"conid":"1","symbol":"SPY.OLD","sections":[]}])";
constexpr std::string_view kStrikes = R"({"call":[595,600],"put":[600,605]})";

std::string info(std::string_view url) {
  const bool call = url.find("right=C") != std::string_view::npos;
  const auto strike = url.find("strike=600&") != std::string_view::npos ? 600 : url.find("strike=595") != std::string_view::npos ? 595 : 605;
  std::string rows = "[";
  for (const auto& [date, offset] : {std::pair{"20260925", 0}, std::pair{"20260930", 1}}) {
    if (offset) rows += ",";
    rows += R"({"conid":)" + std::to_string((call ? 700000 : 800000) + strike * 10 + offset) +
            R"(,"symbol":"SPY","secType":"OPT","exchange":"SMART","listingExchange":"AMEX","right":")" +
            (call ? "C" : "P") + R"(","strike":)" + std::to_string(strike) +
            R"(.0,"currency":"USD","cusip":null,"coupon":"No Coupon","desc1":"SPY","maturityDate":")" + date +
            R"(","multiplier":"100","tradingClass":"SPY","validExchanges":"SMART,AMEX,CBOE"})";
  }
  return rows + "]";
}

struct Gateway {
  bool warm = false;
  std::string availability = "RpB";
  net::HttpResponse operator()(std::string_view url) const {
    const auto path = url.substr(kBase.size());
    if (path == "/tickle") return {200, std::string(kTickle)};
    if (path == "/iserver/accounts") return {200, R"({"accounts":["DU0000000"],"selectedAccount":"DU0000000"})"};
    if (path.starts_with("/iserver/secdef/search?")) return {200, std::string(kSearch)};
    if (path.starts_with("/iserver/secdef/strikes?")) return {200, std::string(kStrikes)};
    if (path.starts_with("/iserver/secdef/info?")) return {200, info(path)};
    if (path.starts_with("/iserver/marketdata/unsubscribeall")) return {200, R"({"unsubscribed":true})"};
    if (path.starts_with("/iserver/marketdata/snapshot?conids=756733&"))
      return {200, R"([{"conid":756733,"conidEx":"756733","_updated":1790263799000,"server_id":"q0","31":"600.21",
                      "84":"600.20","86":"600.22","88":"3","85":"2","6509":")" + availability + R"("}])"};
    if (path.starts_with("/iserver/marketdata/snapshot?conids=")) {
      if (!warm) return {200, R"([{"conid":706000,"conidEx":"706000"},{"conid":806000,"conidEx":"806000"}])"};
      return {200, R"([{"conid":706000,"conidEx":"706000","_updated":1790263800000,"31":"C2.95","84":"3.10",
                      "86":"3.20","88":"1,200","85":"1.2K","7308":"0.512","7309":"0.071","7638":"4,321",
                      "7762":"56","6509":")" + availability + R"("},
                     {"conid":806000,"conidEx":"806000","_updated":1790263798000,"31":"2.85","84":"2.80",
                      "86":"2.90","88":"15","85":"9","7762":"1,234","6509":")" + availability + R"("}])"};
    }
    return {404, "{}"};
  }
};

providers::IbkrProvider::Options options(md::Timestamp* now = nullptr) {
  providers::IbkrProvider::Options settings;
  settings.clock = [now] { return now ? *now : kNow; };
  // Each request is a quarter second after the last, inside the pacing budget.
  settings.pacing_clock = [paced = std::make_shared<md::Timestamp>(kNow)] { return *paced += 250'000'000; };
  return settings;
}

const md::Subscription kSubscription{{"SPY"}, 1, 0.005};

TEST(Ibkr, ParsesDisplayValuesWithoutTreatingClosesAsCurrent) {
  EXPECT_DOUBLE_EQ(providers::parse_ibkr_number("1,234", false), 1234);
  EXPECT_DOUBLE_EQ(providers::parse_ibkr_number("1.2K", false), 1200);
  EXPECT_DOUBLE_EQ(providers::parse_ibkr_number("2M", false), 2e6);
  EXPECT_DOUBLE_EQ(providers::parse_ibkr_number("4.10", true), 4.1);
  EXPECT_EQ(providers::parse_ibkr_number("C4.10", true), 0);
  EXPECT_EQ(providers::parse_ibkr_number("H4.10", true), 0);
  EXPECT_TRUE(std::isnan(providers::parse_ibkr_number("", false)));
  EXPECT_TRUE(std::isnan(providers::parse_ibkr_number("n/a", false)));
}

TEST(Ibkr, SearchFindsTheOptionableContractAndOrdersMonths) {
  const auto underlying = providers::parse_ibkr_search(kSearch, "SPY");
  EXPECT_EQ(underlying.conid, 756733);
  EXPECT_EQ(underlying.months, (std::vector<std::string>{"AUG26", "SEP26", "OCT26"}));
  EXPECT_THROW((void)providers::parse_ibkr_search(kSearch, "QQQ"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_ibkr_search(R"([{"conid":"1","symbol":"SPY","sections":[{"secType":"STK"}]}])", "SPY"),
               std::runtime_error);
  EXPECT_EQ(providers::parse_ibkr_strikes(kStrikes), (std::vector<double>{595, 600, 605}));
}

TEST(Ibkr, InfoMapsTradingClassMaturityRightAndStrikeToOsi) {
  const auto contracts = providers::parse_ibkr_info(R"([{"conid":"9001","symbol":"SPX","secType":"OPT","right":"P",
    "strike":7405,"currency":"USD","maturityDate":"20261005","multiplier":"100","tradingClass":"SPXW"}])", "SPX");
  ASSERT_EQ(contracts.size(), 1u);
  EXPECT_EQ(contracts[0].conid, 9001);
  EXPECT_EQ(contracts[0].contract.osi_symbol(), "SPXW  261005P07405000");
  EXPECT_EQ(contracts[0].contract.settlement, md::Settlement::PM);
  EXPECT_TRUE(contracts[0].contract.standard);
  const auto mini = providers::parse_ibkr_info(R"([{"conid":9002,"secType":"OPT","right":"C","strike":600.5,
    "currency":"USD","maturityDate":"20260925","multiplier":"10","tradingClass":"SPY"}])", "SPY");
  EXPECT_FALSE(mini[0].contract.standard);
  EXPECT_EQ(mini[0].contract.osi_symbol(), "SPY   260925C00600500");
  EXPECT_THROW((void)providers::parse_ibkr_info(info("strike=600&right=C"), "QQQ"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_ibkr_info(R"([{"conid":1,"secType":"FOP","right":"C","strike":1,"maturityDate":"20260925","tradingClass":"SPY"}])", "SPY"),
               std::runtime_error);
}

TEST(Ibkr, SnapshotRowsDistinguishWarmingLinesAndKeepTheUpdateClock) {
  const auto warming = providers::parse_ibkr_snapshot(R"([{"conid":706000,"conidEx":"706000"}])");
  ASSERT_EQ(warming.size(), 1u);
  EXPECT_FALSE(warming[0].fields);
  Gateway gateway;
  gateway.warm = true;
  const auto rows = providers::parse_ibkr_snapshot(gateway("https://localhost:5000/v1/api/iserver/marketdata/snapshot?conids=706000,806000&").body);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_TRUE(rows[0].fields);
  EXPECT_EQ(rows[0].ts, kNow);
  EXPECT_EQ(rows[0].last, 0);  // a closing price is not current
  EXPECT_DOUBLE_EQ(rows[0].bid_size, 1200);
  EXPECT_DOUBLE_EQ(rows[0].ask_size, 1200);
  EXPECT_DOUBLE_EQ(rows[0].open_interest, 4321);
  EXPECT_DOUBLE_EQ(rows[0].delta, 0.512);
  EXPECT_EQ(rows[0].availability, 'R');
  EXPECT_LT(rows[0].volume, 0);  // beside a previous close, 56 may be an earlier session's
  EXPECT_LT(rows[1].open_interest, 0);
  EXPECT_TRUE(std::isnan(rows[1].delta));
  EXPECT_DOUBLE_EQ(rows[1].volume, 1234);
  EXPECT_THROW((void)providers::parse_ibkr_snapshot(R"([{"conid":1,"84":"1.00"}])"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_ibkr_snapshot(R"([{"84":"1.00"}])"), std::runtime_error);
}

TEST(Ibkr, VolumeComesFromRealTimeRowsAndAPositiveCountNeedsACurrentTrade) {
  const auto rows = providers::parse_ibkr_snapshot(R"([
    {"conid":1,"_updated":1790263800000,"31":"2.85","7762":12,"6509":"RpB"},
    {"conid":2,"_updated":1790263800000,"31":"C2.95","7762":"0","6509":"RpB"},
    {"conid":3,"_updated":1790263800000,"84":"2.80","7762":"0","6509":"RpB"},
    {"conid":4,"_updated":1790263800000,"84":"2.80","7762":"12","6509":"RpB"},
    {"conid":5,"_updated":1790263800000,"31":"H2.85","7762":"12","6509":"RpB"},
    {"conid":6,"_updated":1790263800000,"31":"2.85","7762":"12","6509":"DpB"},
    {"conid":7,"_updated":1790263800000,"31":"2.85","7762":"12","6509":"ZpB"},
    {"conid":8,"_updated":1790263800000,"31":"2.85","7762":"12","6509":"YpB"},
    {"conid":9,"_updated":1790263800000,"31":"2.85","7762":"1.2K","6509":"RpB"},
    {"conid":10,"_updated":1790263800000,"31":"2.85","7762":"12.5","6509":"RpB"},
    {"conid":11,"_updated":1790263800000,"31":"2.85","7762":"-1","6509":"RpB"},
    {"conid":12,"_updated":1790263800000,"31":"2.85","6509":"RpB"}])");
  ASSERT_EQ(rows.size(), 12u);
  EXPECT_DOUBLE_EQ(rows[0].volume, 12);
  EXPECT_EQ(rows[1].volume, 0);  // no trade yet today, and none counted
  EXPECT_EQ(rows[2].volume, 0);
  for (std::size_t i = 3; i < rows.size(); ++i) EXPECT_LT(rows[i].volume, 0) << rows[i].conid;
}

TEST(Ibkr, TickleReportsAuthenticationAndCompetingSessions) {
  const auto session = providers::parse_ibkr_tickle(kTickle);
  EXPECT_TRUE(session.authenticated);
  EXPECT_TRUE(session.connected);
  EXPECT_FALSE(session.competing);
  EXPECT_FALSE(providers::parse_ibkr_tickle(R"({"iserver":{"authStatus":{"authenticated":false}}})").authenticated);
}

TEST(Ibkr, FirstPollMapsConidsWithinPacingAndWarmsLinesWithoutInventingQuotes) {
  providers::IbkrProvider provider(options());
  test::HttpStub http;
  http.respond = Gateway{};
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", kSubscription, sink);
  std::vector<std::string> paths;
  for (const auto& url : http.urls) {
    ASSERT_TRUE(url.starts_with(kBase)) << url;
    paths.push_back(url.substr(kBase.size(), url.find('?') == std::string::npos ? std::string::npos : url.find('?') - kBase.size()));
  }
  ASSERT_EQ(http.bodies.size(), 1u);  // only the /tickle POST
  EXPECT_EQ(paths[0], "/tickle");
  EXPECT_EQ(paths[1], "/iserver/accounts");
  EXPECT_EQ(paths[2], "/iserver/secdef/search");
  EXPECT_NE(http.urls[2].find("symbol=SPY&secType=STK"), std::string::npos);
  EXPECT_EQ(paths[3], "/iserver/marketdata/snapshot");
  // AUG26 is past; SEP26's nearest expiry is enough for one expiry, so OCT26 is never read.
  EXPECT_NE(http.urls[4].find("month=SEP26"), std::string::npos);
  for (const auto& url : http.urls) EXPECT_EQ(url.find("OCT26"), std::string::npos);
  EXPECT_NE(http.urls[5].find("strike=600&right=C"), std::string::npos);
  EXPECT_NE(http.urls[6].find("strike=600&right=P"), std::string::npos);
  EXPECT_NE(http.urls.back().find("conids=706000,806000&fields=31,84,85,86,88,7308,7309,7638,7762,6509"),
            std::string::npos);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);
  EXPECT_TRUE(sink.all<md::VendorGreeks>().empty());
  for (const auto& quote : sink.all<md::OptionQuote>()) {
    EXPECT_EQ(quote.bid, 0);
    EXPECT_EQ(quote.ask, 0);
  }
  ASSERT_EQ(sink.all<md::UnderlyingQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::UnderlyingQuote>()[0].last, 600.21);
  const auto status = sink.all<md::ProviderStatus>().at(0);
  EXPECT_EQ(status.state, md::FeedState::Live);
  EXPECT_NE(status.message.find("2 contracts warming up"), std::string::npos);
}

TEST(Ibkr, LaterPollsReuseTheMappingAndPublishQuotesOnTheUpdateClock) {
  providers::IbkrProvider provider(options());
  test::HttpStub http;
  Gateway gateway;
  http.respond = gateway;
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", kSubscription, sink);
  sink.events.clear();
  http.urls.clear();
  gateway.warm = true;
  http.respond = gateway;
  provider.poll_once(http, "SPY", kSubscription, sink);
  ASSERT_EQ(http.urls.size(), 2u);  // the underlying and one option batch; no lookups or keepalive
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 2u);
  EXPECT_EQ(quotes[0].ts, kNow);
  EXPECT_DOUBLE_EQ(quotes[0].bid, 3.10);
  EXPECT_DOUBLE_EQ(quotes[0].bid_size, 1200);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 1u);
  const auto volumes = sink.all<md::OptionVolume>();
  ASSERT_EQ(volumes.size(), 1u);  // 706000's count sits beside a previous close
  EXPECT_EQ(volumes[0].ts, 1790263798000LL * 1'000'000);
  EXPECT_DOUBLE_EQ(volumes[0].contracts, 1234);
  ASSERT_EQ(sink.all<md::VendorGreeks>().size(), 1u);
  EXPECT_TRUE(std::isnan(sink.all<md::VendorGreeks>()[0].iv));
  EXPECT_EQ(sink.all<md::SnapshotComplete>().at(0).ts, kNow);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).message, "IBKR: real-time snapshots");
}

TEST(Ibkr, DelayedAndFrozenAvailabilityAreReportedAsSuch) {
  for (const auto& [code, state] : {std::pair{"DpB", md::FeedState::Delayed}, std::pair{"ZpB", md::FeedState::Stale}}) {
    providers::IbkrProvider provider(options());
    test::HttpStub http;
    Gateway gateway;
    gateway.availability = code;
    gateway.warm = true;
    http.respond = gateway;
    test::BrokerSink sink;
    provider.poll_once(http, "SPY", kSubscription, sink);
    EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, state) << code;
    EXPECT_TRUE(sink.all<md::OptionVolume>().empty()) << code;
  }
}

TEST(Ibkr, MarketDataLinesAreCountedAcrossUnderlyings) {
  auto settings = options();
  settings.max_lines = 2;
  providers::IbkrProvider provider(settings);
  test::HttpStub http;
  http.respond = Gateway{};
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", kSubscription, sink);
  const auto status = sink.all<md::ProviderStatus>().at(0);
  EXPECT_EQ(status.state, md::FeedState::Error);
  EXPECT_NE(status.message.find("needs 3 market-data lines; max_lines is 2"), std::string::npos);
  EXPECT_TRUE(sink.all<md::ContractDefinition>().empty());
}

TEST(Ibkr, KeepaliveRunsEveryMinuteAndAnUnauthenticatedGatewayIsAnError) {
  md::Timestamp now = kNow;
  providers::IbkrProvider provider(options(&now));
  test::HttpStub http;
  http.respond = Gateway{};
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", kSubscription, sink);
  now += 61 * md::kNanosPerSecond;
  http.urls.clear();
  provider.poll_once(http, "SPY", kSubscription, sink);
  ASSERT_FALSE(http.urls.empty());
  EXPECT_EQ(http.urls[0], std::string(kBase) + "/tickle");
  now += 61 * md::kNanosPerSecond;
  sink.events.clear();
  http.respond = [](std::string_view url) {
    if (url.ends_with("/tickle")) return net::HttpResponse{200, R"({"iserver":{"authStatus":{"authenticated":false,"connected":true}}})"};
    return Gateway{}(url);
  };
  provider.poll_once(http, "SPY", kSubscription, sink);
  EXPECT_NE(sink.all<md::ProviderStatus>().at(0).message.find("not authenticated"), std::string::npos);
  sink.events.clear();
  http.respond = [](std::string_view url) {
    if (url.ends_with("/tickle")) return net::HttpResponse{200, R"({"iserver":{"authStatus":{"authenticated":true,"connected":true,"competing":true}}})"};
    return Gateway{}(url);
  };
  provider.poll_once(http, "SPY", kSubscription, sink);
  EXPECT_NE(sink.all<md::ProviderStatus>().at(0).message.find("another session"), std::string::npos);
}

TEST(Ibkr, ANewDateReleasesLinesOnceAndMapsAgain) {
  md::Timestamp now = kNow;
  providers::IbkrProvider provider(options(&now));
  test::HttpStub http;
  http.respond = Gateway{};
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", kSubscription, sink);
  now += md::kNanosPerDay;
  http.urls.clear();
  provider.poll_once(http, "SPY", kSubscription, sink);
  const auto unsubscribe = std::find_if(http.urls.begin(), http.urls.end(), [](const auto& url) {
    return url.ends_with("/iserver/marketdata/unsubscribeall");
  });
  ASSERT_NE(unsubscribe, http.urls.end());
  EXPECT_NE(std::find_if(unsubscribe, http.urls.end(), [](const auto& url) { return url.find("/secdef/search?") != std::string::npos; }),
            http.urls.end());
}

TEST(Ibkr, PacingViolationsBackOffAndNeverLeakBodies) {
  providers::IbkrProvider provider(options());
  test::HttpStub http;
  http.respond = [](std::string_view) { return net::HttpResponse{429, "secret"}; };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", kSubscription, sink);
  const auto status = sink.all<md::ProviderStatus>().at(0);
  EXPECT_NE(status.message.find("pacing"), std::string::npos);
  EXPECT_EQ(status.message.find("secret"), std::string::npos);
}

TEST(Ibkr, OnlyALoopbackGatewayAndBoundedSettingsAreAccepted) {
  for (const auto* url : {"https://example.com/v1/api", "http://10.0.0.2:5000/v1/api", "not a url"}) {
    auto settings = options();
    settings.base_url = url;
    EXPECT_THROW(providers::IbkrProvider{settings}, std::invalid_argument) << url;
  }
  for (const auto* url : {"http://127.0.0.1:5000/v1/api/", "https://localhost:5000/v1/api"}) {
    auto settings = options();
    settings.base_url = url;
    EXPECT_NO_THROW(providers::IbkrProvider{settings}) << url;
  }
  EXPECT_THROW((void)providers::make_provider({"ibkr", "", {{"requests_per_second", "11"}}}), std::invalid_argument);
  EXPECT_THROW((void)providers::make_provider({"ibkr", "", {{"max_lines", "1"}}}), std::invalid_argument);
  EXPECT_THROW((void)providers::make_provider({"ibkr", "", {{"account", "DU1"}}}), std::invalid_argument);
  EXPECT_EQ(providers::make_provider({"ibkr", "", {}})->name(), "ibkr");
  EXPECT_THROW(providers::validate_subscription("ibkr", {{"SPY"}, 0, 0.01}), std::invalid_argument);
  EXPECT_THROW(providers::validate_subscription("ibkr", {{"SPY"}, 1, 0}), std::invalid_argument);
  EXPECT_NO_THROW(providers::validate_subscription("ibkr", kSubscription));
  const auto names = providers::provider_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "ibkr"), names.end());
}
}  // namespace
