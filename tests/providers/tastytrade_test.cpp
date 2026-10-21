#include "openport/providers/tastytrade.hpp"
#include "../../apps/probe/certification.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <cstdlib>

#include "openport/providers/factory.hpp"
#include "openport/providers/tradier.hpp"
#include "support/broker_sink.hpp"
#include "support/http_stub.hpp"
#include "support/recording.hpp"

namespace {
using namespace openport;
using Json = nlohmann::json;
constexpr md::Timestamp kNow = 1790263800000LL * 1'000'000;

// Adapted from the archived SPY nested-chain example: its inconsistent expiry
// dates are corrected; generated SPX/SPXW roots verify AM/PM separation.
constexpr std::string_view kChain = R"({"data":{"items":[
 {"underlying-symbol":"SPX","root-symbol":"SPX","option-chain-type":"Standard","shares-per-contract":100,
 "expirations":[{"expiration-type":"Regular","expiration-date":"2026-10-16","settlement-type":"AM",
 "strikes":[{"strike-price":"7400.0","call":"SPX   261016C07400000","call-streamer-symbol":".SPX261016C7400",
 "put":"SPX   261016P07400000","put-streamer-symbol":".SPX261016P7400"}]}]},
 {"underlying-symbol":"SPX","root-symbol":"SPXW","option-chain-type":"Standard","shares-per-contract":100,
 "expirations":[{"expiration-type":"Weekly","expiration-date":"2026-10-16","settlement-type":"PM",
 "strikes":[{"strike-price":"7400.0","call":"SPXW  261016C07400000","call-streamer-symbol":".SPXW261016C7400",
 "put":"SPXW  261016P07400000","put-streamer-symbol":".SPXW261016P7400"},
 {"strike-price":"9000.0","call":"SPXW  261016C09000000","call-streamer-symbol":".SPXW261016C9000"}]},
 {"expiration-date":"2026-10-23","settlement-type":"PM","strikes":[{"strike-price":"7400",
 "call":"SPXW  261023C07400000","call-streamer-symbol":".SPXW261023C7400"}]}]}
 ]}})";

Json quote(std::string symbol) {
  return {{"eventType", "Quote"}, {"eventSymbol", std::move(symbol)}, {"bidPrice", 5.0}, {"askPrice", 5.2},
          {"bidSize", 10}, {"askSize", 20}, {"bidTime", 1790263800000LL}, {"askTime", 1790263801000LL}};
}

TEST(Tastytrade, NestedChainMapsSuppliedStreamersAndSettlementFamilies) {
  const auto chain = providers::parse_tastytrade_chain(kChain, "SPX");
  ASSERT_EQ(chain.size(), 6u);
  EXPECT_EQ(chain[0].contract.osi_symbol(), "SPX   261016C07400000");
  EXPECT_EQ(chain[0].streamer_symbol, ".SPX261016C7400");
  EXPECT_EQ(chain[0].contract.settlement, md::Settlement::AM);
  EXPECT_EQ(chain[2].contract.osi_symbol(), "SPXW  261016C07400000");
  EXPECT_EQ(chain[2].contract.settlement, md::Settlement::PM);
  EXPECT_EQ(chain[2].contract.style, pricing::ExerciseStyle::European);
  EXPECT_EQ(providers::parse_tastytrade_streamer(R"({"data":{"symbol":"SPX","streamer-symbol":"$SPX"}})"), "$SPX");
  EXPECT_EQ(providers::parse_tastytrade_streamer(R"({"data":{"symbol":"SPX","streamer-symbol":"SPX"}})"), "SPX");
  EXPECT_THROW((void)providers::parse_tastytrade_streamer(R"({"data":{"symbol":"SPX"}})"), std::runtime_error);
}

TEST(Tastytrade, RejectsMismatchedDefinitionsAndPreservesNonstandardContracts) {
  auto doc = Json::parse(kChain);
  doc["data"]["items"][0]["shares-per-contract"] = 10;
  doc["data"]["items"][0]["option-chain-type"] = "Adjusted";
  EXPECT_FALSE(providers::parse_tastytrade_chain(doc.dump(), "SPX")[0].contract.standard);
  doc["data"]["items"][0]["expirations"][0]["expiration-date"] = "2022-09-16";
  EXPECT_THROW((void)providers::parse_tastytrade_chain(doc.dump(), "SPX"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_tastytrade_chain(kChain, "SPY"), std::runtime_error);
  EXPECT_TRUE(providers::parse_tastytrade_chain(R"({"data":{"items":null}})", "SPX").empty());
  EXPECT_THROW((void)providers::parse_tastytrade_chain(R"({"error":{"message":"secret"}})", "SPX"), std::runtime_error);
}

TEST(Tastytrade, OAuthUsesJsonAndUserAgentAndRefreshesBeforeExpiresIn) {
  providers::TastytradeApi api({"client-secret", "refresh-secret", "optional-client"});
  test::HttpStub http;
  int refreshes = 0;
  http.respond = [&](auto url) -> net::HttpResponse {
    if (url.ends_with("/oauth/token")) {
      ++refreshes;
      // Synthetic response: the reference documents fields, not a literal reply.
      return {200, Json{{"access_token", "access-" + std::to_string(refreshes)}, {"token_type", "Bearer"}, {"expires_in", 100}}.dump()};
    }
    return {200, R"({"data":{"token":"quote-token","dxlink-url":"wss://example.test/feed"}})"};
  };
  api.get(http, "/api-quote-tokens", kNow);
  EXPECT_EQ(refreshes, 1);
  EXPECT_EQ(api.refresh_at(), kNow + 90 * md::kNanosPerSecond);
  api.get(http, "/api-quote-tokens", kNow + 89 * md::kNanosPerSecond);
  EXPECT_EQ(refreshes, 1);
  api.refresh(http, kNow + 90 * md::kNanosPerSecond);
  EXPECT_EQ(refreshes, 2);
  ASSERT_EQ(http.bodies.size(), 2u);
  const auto body = Json::parse(http.bodies[0]);
  EXPECT_EQ(body["grant_type"], "refresh_token");
  EXPECT_EQ(body["client_secret"], "client-secret");
  EXPECT_EQ(body["refresh_token"], "refresh-secret");
  EXPECT_EQ(body["client_id"], "optional-client");
  for (std::size_t i = 0; i < http.urls.size(); ++i) {
    EXPECT_TRUE(http.urls[i].starts_with("https://api.tastyworks.com/"));
    EXPECT_EQ(http.urls[i].find("secret"), std::string::npos);
    net::HttpResponse headers;
    headers.headers = http.sent_headers[i];
    EXPECT_TRUE(headers.header("User-Agent").starts_with("openport/"));
  }
  net::HttpResponse sent;
  sent.headers = http.sent_headers[1];
  EXPECT_EQ(sent.header("Authorization"), "Bearer access-1");
}

TEST(Tastytrade, OAuthWithoutClientIdUsesDocumentedFifteenMinuteFallback) {
  providers::TastytradeApi api({"secret", "refresh", ""});
  test::HttpStub http;
  http.respond = [](auto) { return net::HttpResponse{200, R"({"access_token":"token"})"}; };
  api.refresh(http, kNow);
  EXPECT_FALSE(Json::parse(http.bodies[0]).contains("client_id"));
  EXPECT_EQ(api.refresh_at(), kNow + 840 * md::kNanosPerSecond);
}

TEST(Tastytrade, UnauthorizedRefreshesOnceButNeverRetriesOAuthFailuresOrRedirects) {
  providers::TastytradeApi api({"secret", "refresh", ""});
  test::HttpStub http;
  http.respond = [](auto url) {
    return url.ends_with("/oauth/token") ? net::HttpResponse{200, R"({"access_token":"token"})"} : net::HttpResponse{401, "secret"};
  };
  EXPECT_THROW((void)api.get(http, "/api-quote-tokens", kNow), std::runtime_error);
  EXPECT_EQ(http.urls.size(), 4u);
  for (int status : {302, 307, 429, 500}) {
    providers::TastytradeApi fresh({"secret", "refresh", ""});
    http.urls.clear();
    http.respond = [status](auto) { return net::HttpResponse{status, "secret"}; };
    try { fresh.refresh(http, kNow); FAIL(); }
    catch (const std::exception& error) { EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos); }
    EXPECT_EQ(http.urls.size(), 1u);
  }
}

TEST(Tastytrade, AllowsOnlyMarketDataPathsAndNeverSendsPreCancelledOAuth) {
  providers::TastytradeApi api({"secret", "refresh", ""});
  test::HttpStub http;
  for (const auto* path : {"/accounts", "/orders", "/sessions", "/option-chains/SPX/other",
                           "/instruments/equities/../accounts", "/instruments/equities/%2e%2e",
                           "/option-chains/SPX%2faccounts/nested", "/api-quote-tokens?secret=x"})
    EXPECT_THROW((void)api.get(http, path, kNow), std::invalid_argument);
  EXPECT_TRUE(http.urls.empty());
  std::atomic<bool> cancelled{true};
  EXPECT_THROW(api.refresh(http, kNow, &cancelled), std::runtime_error);
  EXPECT_TRUE(http.urls.empty());
}

TEST(Tastytrade, QuoteTokensUseExplicitExpiryOrRefreshWithinTwentyFourHours) {
  const auto fallback = providers::parse_tastytrade_quote_token(
      R"({"data":{"token":"fake","dxlink-url":"wss://example.test/realtime","level":"api"}})", kNow);
  EXPECT_EQ(fallback.refresh_at, kNow + 23 * 3600 * md::kNanosPerSecond);
  const auto expires = md::format_timestamp(kNow + 600 * md::kNanosPerSecond);
  const auto token = providers::parse_tastytrade_quote_token(Json{{"data", {{"token", "fake"}, {"dxlink-url", "wss://example.test/feed"}, {"expires-at", expires}}}}.dump(), kNow);
  EXPECT_EQ(token.refresh_at, kNow + 540 * md::kNanosPerSecond);
  EXPECT_THROW((void)providers::parse_tastytrade_quote_token(R"({"data":{"token":"secret","dxlink-url":"ws://example.test"}})", kNow), std::runtime_error);
}

TEST(Tastytrade, FiltersWaitForActualSpotAndRetainPreviouslyDefinedOptions) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}, 1, .01}, sink);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  EXPECT_TRUE(sink.all<md::ContractDefinition>().empty());
  EXPECT_EQ(mapper.subscriptions().size(), 2u);
  auto spot = quote("$SPX");
  spot["bidPrice"] = 7399;
  spot["askPrice"] = 7401;
  mapper.event(spot);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 4u);
  EXPECT_EQ(mapper.subscriptions().size(), 18u);
  sink.events.clear();
  spot["bidPrice"] = 8999;
  spot["askPrice"] = 9001;
  mapper.event(spot);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  EXPECT_EQ(mapper.subscriptions().size(), 22u);
  mapper.event(quote(".SPX261016C7400"));
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 1u);
}

TEST(Tastytrade, MapsDocumentedGreeksWithoutInventingClocksOrSnapshots) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  mapper.event(quote(".SPXW261016C7400"));
  mapper.event({{"eventType", "Greeks"}, {"eventSymbol", ".SPXW261016C7400"}, {"delta", .5}, {"gamma", .01},
                {"volatility", .2}, {"theta", -1}, {"vega", 2}, {"rho", .1}, {"time", 1790263800000LL}});
  mapper.event({{"eventType", "Summary"}, {"eventSymbol", ".SPXW261016C7400"}, {"openInterest", 42}});
  mapper.event({{"eventType", "Summary"}, {"eventSymbol", ".SPXW261016C7400"}, {"openInterest", "NaN"}});
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts, 0);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Stale);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 1u);
  EXPECT_EQ(sink.all<md::OpenInterest>()[0].contracts, 42);
  const auto greeks = sink.all<md::VendorGreeks>()[0];
  EXPECT_DOUBLE_EQ(greeks.delta, .5);
  EXPECT_DOUBLE_EQ(greeks.iv, .2);
  EXPECT_DOUBLE_EQ(greeks.theta, -1);
  EXPECT_DOUBLE_EQ(greeks.vega, 2);
  EXPECT_DOUBLE_EQ(greeks.rho, .1);
  EXPECT_EQ(greeks.ts, 0);
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
}

TEST(Tastytrade, CompleteChainRefreshUnsubscribesRemovedAndExpiredOptions) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink);
  auto chain = providers::parse_tastytrade_chain(kChain, "SPX");
  mapper.chain("SPX", "$SPX", chain, {2026, 9, 24});
  mapper.event(quote(".SPX261016C7400"));
  sink.events.clear();
  chain.erase(chain.begin());
  mapper.chain("SPX", "SPX", chain, {2026, 9, 24});
  EXPECT_FALSE(mapper.subscriptions().contains({"Quote", ".SPX261016C7400"}));
  EXPECT_FALSE(mapper.subscriptions().contains({"Quote", "$SPX"}));
  EXPECT_TRUE(mapper.subscriptions().contains({"Quote", "SPX"}));
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].bid, 0);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts, 0);
  sink.events.clear();
  mapper.chain("SPX", "SPX", chain, {2026, 10, 17});
  EXPECT_EQ(mapper.subscriptions().size(), 6u);  // only the later expiry and underlying
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
}

TEST(Tastytrade, ExplicitClockUnitsPreserveSourceTimeAndReportLiveOnlyOnOptionQuotes) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink, true);
  mapper.chain("SPX", "SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  mapper.event(quote("SPX"));
  EXPECT_TRUE(sink.all<md::ProviderStatus>().empty());
  auto underlying_without_clock = quote("SPX");
  underlying_without_clock["askTime"] = nullptr;
  mapper.event(underlying_without_clock);
  EXPECT_EQ(sink.all<md::UnderlyingQuote>().back().ts, 0);
  mapper.event(quote("unknown"));
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  mapper.event(quote(".SPX261016C7400"));
  mapper.event(quote(".SPX261016C7400"));
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].ts, kNow + md::kNanosPerSecond);
  EXPECT_EQ(sink.all<md::ProviderStatus>().size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Live);
  mapper.reset_health();
  mapper.event(quote(".SPX261016C7400"));
  EXPECT_EQ(sink.all<md::ProviderStatus>().size(), 2u);
  auto missing = quote(".SPX261016C7400");
  missing["bidTime"] = nullptr;
  mapper.event(missing);
  EXPECT_EQ(sink.all<md::ProviderStatus>().back().state, md::FeedState::Stale);
}

TEST(Tastytrade, TradeSnapshotsOnlySupplyUnderlyingLastWithItsOwnClock) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink, true);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", "$SPX"}, {"price", 7400}, {"time", 1790263700000LL}});
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", ".SPX261016C7400"}, {"price", 5}, {"size", 1}, {"time", 1790263800000LL}});
  EXPECT_EQ(sink.all<md::UnderlyingQuote>()[0].ts, kNow - 100 * md::kNanosPerSecond);
  EXPECT_EQ(sink.all<md::UnderlyingQuote>()[0].last, 7400);
  EXPECT_TRUE(sink.all<md::OptionTrade>().empty());
}

TEST(Tastytrade, MissingGreeksAndUnconfirmedSummaryClocksStayUnknown) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  mapper.event({{"eventType", "Greeks"}, {"eventSymbol", ".SPX261016C7400"}, {"volatility", 0},
                {"vega", "NaN"}, {"theta", nullptr}, {"rho", "Infinity"}});
  const auto greek = sink.all<md::VendorGreeks>()[0];
  for (const double value : {greek.iv, greek.vega, greek.theta, greek.rho}) EXPECT_TRUE(std::isnan(value));
  mapper.event({{"eventType", "Summary"}, {"eventSymbol", ".SPX261016C7400"}, {"openInterest", 42},
                {"eventTime", 1790263800000LL}, {"dayVolume", 321}});
  EXPECT_EQ(sink.all<md::OpenInterest>()[0].ts, 0);
  EXPECT_TRUE(sink.all<md::OptionVolume>().empty());
}

TEST(Tastytrade, MapsSummaryClockAndSessionVolumeWithoutInventingTrades) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink, true);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  EXPECT_FALSE(mapper.subscriptions().contains({"Profile", "$SPX"}));
  mapper.event({{"eventType", "Summary"}, {"eventSymbol", ".SPX261016C7400"},
                {"openInterest", 42}, {"eventTime", 1790263800000LL}});
  ASSERT_EQ(sink.all<md::OpenInterest>().size(), 1u);
  EXPECT_EQ(sink.all<md::OpenInterest>()[0].ts, kNow);
  for (const auto* type : {"Trade", "Summary"}) {
    for (const Json& volume : {Json(0), Json(321), Json(-1), Json(1.5), Json(), Json("NaN")}) {
      sink.events.clear();
      Json event{{"eventType", type}, {"eventSymbol", ".SPX261016C7400"}, {"dayVolume", volume},
                 {"time", 1790263800000LL}, {"eventTime", 1790263800000LL}};
      mapper.event(event);
      const auto values = sink.all<md::OptionVolume>();
      if (volume == 0 || volume == 321) {
        ASSERT_EQ(values.size(), 1u);
        EXPECT_EQ(values[0].contracts, volume.get<double>());
        EXPECT_EQ(values[0].ts, kNow);
      } else EXPECT_TRUE(values.empty());
      EXPECT_TRUE(sink.all<md::OptionTrade>().empty());
    }
  }
  sink.events.clear();
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", ".SPX261016C7400"}, {"dayVolume", 600}});
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", ".SPX261016C7400"}, {"dayVolume", 600},
                {"time", 1790263800000LL}, {"dayId", 1}});
  EXPECT_TRUE(sink.all<md::OptionVolume>().empty());
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", ".SPX261016C7400"}, {"dayVolume", 321},
                {"time", 1790350200000LL}});
  ASSERT_EQ(sink.all<md::OptionVolume>().size(), 1u);  // same count, new trading day
}

TEST(Tastytrade, CertificationUsesTheExistingChainAndMapperFixtures) {
  probe::Conformance monitor({{"SPX"}}, true, 1);
  providers::TastytradeMapper mapper({{"SPX"}}, monitor, true);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  auto update = quote(".SPX261016C7400");
  update["bidTime"] = update["askTime"] = 1790263800000LL;
  mapper.event(update);
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", ".SPX261016C7400"},
                {"dayVolume", 321}, {"time", 1790263800000LL}});
  const auto report = monitor.report();
  EXPECT_EQ(report["event_counts"]["OptionQuote"], 1);
  EXPECT_EQ(report["event_counts"]["OptionTrade"], 0);
  EXPECT_EQ(report["event_counts"]["SnapshotComplete"], 0);
  EXPECT_EQ(report["presence"]["volume_contracts"], 1);
  EXPECT_EQ(report["timestamps"]["OptionVolume"]["unknown"], 0);
  EXPECT_EQ(report["exit_code"], 0);
}

TEST(Tastytrade, SandboxRefusalAndCredentialValidationDoNotStartNetwork) {
  EXPECT_THROW(providers::TastytradeProvider({}), std::invalid_argument);
  EXPECT_THROW(providers::TradierProvider({}), std::invalid_argument);
  try {
    providers::TastytradeProvider provider({.credentials = {"test", "test", ""}, .sandbox = true});
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("funded production"), std::string::npos);
  }
  providers::TastytradeProvider provider({.credentials = {"test", "test", ""}});
  EXPECT_EQ(provider.capabilities().poll_interval.count(), 0);
  EXPECT_TRUE(provider.capabilities().realtime_plan_dependent);
  provider.stop();
  provider.stop();
}

TEST(Tastytrade, MappedEventsRoundTripWithoutNewRecordingFields) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  mapper.event(quote(".SPX261016C7400"));
  mapper.event({{"eventType", "Greeks"}, {"eventSymbol", ".SPX261016C7400"}, {"delta", .5}});
  mapper.event({{"eventType", "Summary"}, {"eventSymbol", ".SPX261016C7400"}, {"openInterest", 0}});
  test::RecordingFile file;
  test::record_events(file.path, sink.events);
  md::RecordingReader reader(file.path);
  for (const auto& expected : sink.events) {
    const auto actual = reader.next();
    ASSERT_TRUE(actual);
    test::exact_event(expected, actual->event);
  }
  EXPECT_FALSE(reader.next());
}

TEST(BrokerFactory, NamesAndStrictOptionsKeepCredentialsOutOfFlags) {
  const auto names = providers::provider_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "tradier"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "tastytrade"), names.end());
  for (const auto* name : {"tradier", "tastytrade"}) {
    for (const auto* option : {"access_token", "client_secret", "refresh_token", "base_url"})
      EXPECT_THROW((void)providers::make_provider({name, "", {{option, "secret"}}}), std::invalid_argument);
    EXPECT_THROW((void)providers::make_provider({name, "", {{"sandbox", "yes"}}}), std::invalid_argument);
  }
  EXPECT_THROW((void)providers::make_provider({"tastytrade", "", {{"poll_seconds", "10"}}}), std::invalid_argument);
  EXPECT_THROW((void)providers::make_provider({"tastytrade", "", {{"dxlink_time_unit", "seconds"}}}), std::invalid_argument);
  EXPECT_THROW((void)providers::make_provider({"tradier", "", {{"poll_seconds", "0"}}}), std::invalid_argument);
  EXPECT_THROW((void)providers::make_provider({"tradier", "", {{"option_size_unit", "lots"}}}), std::invalid_argument);
  // "hundreds" is a valid unit: the only complaint left is the missing token.
  try {
    (void)providers::make_provider({"tradier", "", {{"option_size_unit", "hundreds"}}});
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("TRADIER_ACCESS_TOKEN"), std::string::npos) << error.what();
  }
}

TEST(BrokerFactory, ReadsBrokerCredentialsFromEnvironment) {
  struct Environment {
    const char* name;
    std::optional<std::string> previous;
    explicit Environment(const char* variable) : name(variable) {
      if (const auto* value = std::getenv(name)) previous = value;
      setenv(name, "generated-test-credential", 1);
    }
    ~Environment() {
      if (previous) setenv(name, previous->c_str(), 1);
      else unsetenv(name);
    }
  } tradier("TRADIER_ACCESS_TOKEN"), secret("TASTYTRADE_CLIENT_SECRET"),
    refresh("TASTYTRADE_REFRESH_TOKEN"), client("TASTYTRADE_CLIENT_ID");
  const auto polling = providers::make_provider({"tradier", "", {}});
  EXPECT_EQ(polling->name(), "tradier");
  const auto streaming = providers::make_provider({"tastytrade", "", {}});
  EXPECT_EQ(streaming->name(), "tastytrade");
  EXPECT_EQ(streaming->capabilities().poll_interval.count(), 0);
}
}  // namespace
