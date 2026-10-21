#include "openport/providers/schwab.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <nlohmann/json.hpp>

#include "openport/providers/factory.hpp"
#include "providers/broker_json.hpp"
#include "support/broker_sink.hpp"
#include "support/http_stub.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr md::Timestamp kNow = 1790263800000LL * 1'000'000;

// Shaped after the chain example in Schwab's Trader API market-data reference,
// with generated SPX/SPXW values and dates; not observed market data.
constexpr std::string_view kChain = R"({
  "symbol":"$SPX","status":"SUCCESS","strategy":"SINGLE","isDelayed":false,"isIndex":true,
  "interestRate":4.1,"underlyingPrice":7405.12,"volatility":29.0,"numberOfContracts":2,
  "underlying":{"symbol":"$SPX","description":"S&P 500 INDEX","bid":0,"ask":0,"last":7405.12,
    "mark":7405.12,"quoteTime":1790263799000,"tradeTime":1790263700000,"delayed":false},
  "callExpDateMap":{"2026-10-05:11":{"7410.0":[{"putCall":"CALL","symbol":"SPXW  261005C07410000",
    "bid":31.1,"ask":31.6,"bidSize":12,"askSize":30,"last":31.4,"quoteTimeInLong":1790263800000,
    "tradeTimeInLong":1790263750000,"totalVolume":1520,"openInterest":4200,"volatility":14.02,
    "delta":0.48,"gamma":0.0021,"theta":-2.1,"vega":4.3,"rho":0.9,"strikePrice":7410.0,
    "expirationDate":"2026-10-05T20:00:00.000+00:00","multiplier":100.0,"settlementType":"P",
    "optionRoot":"SPXW","exerciseType":"E","nonStandard":false,"mini":false}]}},
  "putExpDateMap":{"2026-10-05:11":{"7400.0":[{"putCall":"PUT","symbol":"SPXW  261005P07400000",
    "bid":0,"ask":0.05,"bidSize":0,"askSize":5,"quoteTimeInLong":1790263799000,"totalVolume":0,
    "openInterest":null,"volatility":-999.0,"delta":-999.0,"gamma":-999.0,"strikePrice":7400.0,
    "multiplier":100.0,"nonStandard":false}]}}
})";

constexpr std::string_view kExpirations = R"({"expirationList":[
  {"expirationDate":"2026-09-23","daysToExpiration":-1,"expirationType":"W","settlementType":"P","optionRoots":"SPXW"},
  {"expirationDate":"2026-10-05","daysToExpiration":11,"expirationType":"W","settlementType":"P","optionRoots":"SPXW"},
  {"expirationDate":"2026-10-16","daysToExpiration":22,"expirationType":"S","settlementType":"A","optionRoots":"SPX"}]})";

constexpr std::string_view kToken =
    R"({"expires_in":1800,"token_type":"Bearer","scope":"api","refresh_token":"rotated-refresh","access_token":"access-1","id_token":"id"})";

net::HttpResponse fixture(std::string_view url) {
  if (url.find("/v1/oauth/token") != std::string_view::npos) return {200, std::string(kToken)};
  if (url.find("/expirationchain?") != std::string_view::npos) return {200, std::string(kExpirations)};
  if (url.find("/chains?") != std::string_view::npos) return {200, std::string(kChain)};
  return {404, "{}"};
}

providers::SchwabProvider::Options options() {
  return {.credentials = {"app-key", "app-secret", "refresh+token/="}, .clock = [] { return kNow; }};
}

TEST(Schwab, IndexSymbolsTakeTheDollarPrefix) {
  EXPECT_EQ(providers::schwab_symbol("SPX"), "$SPX");
  EXPECT_EQ(providers::schwab_symbol("SPY"), "SPY");
  EXPECT_EQ(providers::broker::symbol_path("$SPX"), "%24SPX");
}

TEST(Schwab, MapsPaddedOsiClocksSizesAndOnlyDocumentedGreekUnits) {
  const auto chain = providers::parse_schwab_chain(kChain, "SPX");
  ASSERT_EQ(chain.options.size(), 2u);
  const auto& call = chain.options[0];
  EXPECT_EQ(call.contract.osi_symbol(), "SPXW  261005C07410000");
  EXPECT_EQ(call.contract.settlement, md::Settlement::PM);
  EXPECT_EQ(call.contract.style, pricing::ExerciseStyle::European);
  EXPECT_TRUE(call.contract.standard);
  EXPECT_EQ(call.ts, kNow);
  EXPECT_DOUBLE_EQ(call.bid, 31.1);
  EXPECT_DOUBLE_EQ(call.ask_size, 30);
  EXPECT_DOUBLE_EQ(call.open_interest, 4200);
  EXPECT_DOUBLE_EQ(call.volume, 1520);
  ASSERT_TRUE(call.greeks);
  EXPECT_DOUBLE_EQ(call.greeks->delta, 0.48);
  EXPECT_TRUE(std::isnan(call.greeks->iv));
  EXPECT_TRUE(std::isnan(call.greeks->vega));
  const auto& put = chain.options[1];
  EXPECT_FALSE(put.greeks);  // -999 is Schwab's "not available"
  EXPECT_LT(put.open_interest, 0);
  EXPECT_EQ(chain.delayed, std::optional<bool>(false));
  // The underlying's last keeps its own, older trade clock.
  EXPECT_EQ(chain.underlying.ts, kNow - 100 * md::kNanosPerSecond);
  EXPECT_DOUBLE_EQ(chain.underlying.last, 7405.12);
}

TEST(Schwab, RejectsInconsistentOrUntimedContractsAndFailedChains) {
  auto json = Json::parse(kChain);
  auto& call = json["callExpDateMap"]["2026-10-05:11"]["7410.0"][0];
  call["putCall"] = "PUT";
  EXPECT_THROW((void)providers::parse_schwab_chain(json.dump(), "SPX"), std::runtime_error);
  call["putCall"] = "CALL";
  call["quoteTimeInLong"] = 1790263800;  // seconds-shaped clocks are not guessed
  EXPECT_THROW((void)providers::parse_schwab_chain(json.dump(), "SPX"), std::runtime_error);
  call["quoteTimeInLong"] = 1790263800000;
  call["nonStandard"] = true;
  EXPECT_FALSE(providers::parse_schwab_chain(json.dump(), "SPX").options[0].contract.standard);
  // The published schema's spellings count as well.
  for (const auto* flag : {"isNonStandard", "isMini", "mini"}) {
    call["nonStandard"] = false;
    call[flag] = true;
    EXPECT_FALSE(providers::parse_schwab_chain(json.dump(), "SPX").options[0].contract.standard) << flag;
    call[flag] = false;
  }
  EXPECT_TRUE(providers::parse_schwab_chain(json.dump(), "SPX").options[0].contract.standard);
  EXPECT_THROW((void)providers::parse_schwab_chain(kChain, "SPY"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_schwab_chain(R"({"symbol":"$SPX","status":"FAILED"})", "SPX"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_schwab_chain("not json secret", "SPX"), std::runtime_error);
  const auto empty = providers::parse_schwab_chain(R"({"symbol":"SPY","status":"SUCCESS","callExpDateMap":{},"putExpDateMap":{}})", "SPY");
  EXPECT_TRUE(empty.options.empty());
  EXPECT_FALSE(empty.delayed);
  EXPECT_EQ(empty.underlying.ts, 0);
}

TEST(Schwab, ExpirationChainListsDates) {
  const auto dates = providers::parse_schwab_expirations(kExpirations);
  EXPECT_EQ(dates.size(), 3u);
  EXPECT_TRUE(dates.contains(md::Date{2026, 10, 16}));
  EXPECT_THROW((void)providers::parse_schwab_expirations(R"({"error":"x"})"), std::runtime_error);
}

TEST(Schwab, RefreshUsesBasicClientCredentialsAndAFormBodyNeverTheUrl) {
  providers::SchwabApi api({"app-key", "app-secret", "refresh+token/="});
  test::HttpStub http;
  http.respond = fixture;
  const auto response = api.get(http, "/chains?symbol=SPY", kNow);
  EXPECT_EQ(response.status, 200);
  ASSERT_EQ(http.urls.size(), 2u);
  EXPECT_EQ(http.urls[0], "https://api.schwabapi.com/v1/oauth/token");
  ASSERT_EQ(http.bodies.size(), 1u);
  EXPECT_EQ(http.bodies[0], "grant_type=refresh_token&refresh_token=refresh%2Btoken%2F%3D");
  net::HttpResponse sent;
  sent.headers = http.sent_headers[0];
  EXPECT_EQ(sent.header("Authorization"), "Basic " + providers::broker::base64("app-key:app-secret"));
  EXPECT_EQ(sent.header("Content-Type"), "application/x-www-form-urlencoded");
  EXPECT_EQ(http.urls[1], "https://api.schwabapi.com/marketdata/v1/chains?symbol=SPY");
  sent.headers = http.sent_headers[1];
  EXPECT_EQ(sent.header("Authorization"), "Bearer access-1");
  for (const auto& url : http.urls) EXPECT_EQ(url.find("secret"), std::string::npos);
  // A valid token is reused until shortly before it expires; a 401 refreshes once.
  (void)api.get(http, "/expirationchain?symbol=SPY", kNow + 60 * md::kNanosPerSecond);
  EXPECT_EQ(http.bodies.size(), 1u);
  EXPECT_EQ(api.refresh_at(), kNow + 1740 * md::kNanosPerSecond);
  int calls = 0;
  http.respond = [&](std::string_view url) {
    if (url.find("/oauth/") != std::string_view::npos) return fixture(url);
    return net::HttpResponse{++calls == 1 ? 401 : 200, "{}"};
  };
  EXPECT_EQ(api.get(http, "/chains?symbol=SPY", kNow + 120 * md::kNanosPerSecond).status, 200);
  ASSERT_EQ(http.bodies.size(), 2u);
  EXPECT_EQ(http.bodies[1], "grant_type=refresh_token&refresh_token=rotated-refresh");
}

TEST(Schwab, AllowlistsOnlyMarketDataPathsAndValidatesTokens) {
  providers::SchwabApi api({"app-key", "app-secret", "refresh"});
  test::HttpStub http;
  http.respond = fixture;
  for (const auto* path : {"/accounts", "/orders", "/quotes?symbols=SPY", "/../trader/v1/accounts"})
    EXPECT_THROW((void)api.get(http, path, kNow), std::logic_error) << path;
  EXPECT_TRUE(http.urls.empty());
  http.respond = [](std::string_view) { return net::HttpResponse{200, R"({"access_token":"x","token_type":"MAC"})"}; };
  EXPECT_THROW(api.refresh(http, kNow), std::runtime_error);
  http.respond = [](std::string_view) { return net::HttpResponse{400, R"({"error":"invalid_client secret"})"}; };
  try {
    api.refresh(http, kNow);
    FAIL();
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
  }
  EXPECT_THROW(providers::SchwabApi({"", "secret", "refresh"}), std::invalid_argument);
}

TEST(Schwab, FullPollSelectsNearestExpiriesAndPublishesDefinitionsBeforeData) {
  providers::SchwabProvider provider(options());
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 1, 0}, sink);
  ASSERT_EQ(http.urls.size(), 3u);
  EXPECT_EQ(http.urls[1], "https://api.schwabapi.com/marketdata/v1/expirationchain?symbol=%24SPX");
  EXPECT_EQ(http.urls[2], "https://api.schwabapi.com/marketdata/v1/chains?symbol=%24SPX&contractType=ALL"
                          "&includeUnderlyingQuote=true&strategy=SINGLE&fromDate=2026-10-05&toDate=2026-10-05");
  EXPECT_TRUE(std::holds_alternative<md::UnderlyingQuote>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<md::ContractDefinition>(sink.events[1]));
  ASSERT_EQ(sink.all<md::ContractDefinition>().size(), 2u);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  EXPECT_EQ(sink.all<md::OpenInterest>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionVolume>().size(), 2u);
  EXPECT_EQ(sink.all<md::VendorGreeks>().size(), 1u);
  EXPECT_EQ(sink.all<md::SnapshotComplete>()[0].ts, kNow);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Live);
  EXPECT_EQ(provider.capabilities().poll_interval, 5s);
  EXPECT_TRUE(provider.capabilities().realtime_plan_dependent);
  sink.events.clear();
  provider.poll_once(http, "SPX", {{"SPX"}, 1, 0}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_EQ(sink.all<md::SnapshotComplete>().size(), 1u);
}

TEST(Schwab, WholeChainNeedsOneRequestAndStrikeWindowFilters) {
  providers::SchwabProvider provider(options());
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 0, 0.0001}, sink);
  ASSERT_EQ(http.urls.size(), 2u);
  EXPECT_EQ(http.urls[1].find("fromDate"), std::string::npos);
  // 7400 is outside 0.01% of 7405.12; 7410 too. Nothing is defined, but the
  // snapshot is still complete for the underlying.
  EXPECT_TRUE(sink.all<md::ContractDefinition>().empty());
  EXPECT_EQ(sink.all<md::SnapshotComplete>().size(), 1u);
}

TEST(Schwab, DelayedAndUnstatedEntitlementAreReportedAsSuch) {
  for (const auto& [value, state] : {std::pair{Json(true), md::FeedState::Delayed}, std::pair{Json(), md::FeedState::Stale}}) {
    providers::SchwabProvider provider(options());
    test::HttpStub http;
    http.respond = [value](std::string_view url) {
      auto response = fixture(url);
      if (url.find("/chains?") != std::string_view::npos) {
        auto doc = Json::parse(response.body);
        if (value.is_null()) doc.erase("isDelayed");
        else doc["isDelayed"] = value;
        response.body = doc.dump();
      }
      return response;
    };
    test::BrokerSink sink;
    provider.poll_once(http, "SPX", {{"SPX"}}, sink);
    EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, state);
  }
}

TEST(Schwab, FailureAndThrottlingNeverRetireTheLastSnapshotOrLeakBodies) {
  providers::SchwabProvider provider(options());
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  sink.events.clear();
  http.respond = [](std::string_view url) {
    if (url.find("/chains?") != std::string_view::npos) {
      net::HttpResponse response{429, "secret body"};
      response.headers = {{"Retry-After", "90"}};
      return response;
    }
    return fixture(url);
  };
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  const auto status = sink.all<md::ProviderStatus>().at(0);
  EXPECT_EQ(status.state, md::FeedState::Error);
  EXPECT_NE(status.message.find("rate limited"), std::string::npos);
  EXPECT_EQ(status.message.find("secret"), std::string::npos);
}

TEST(Schwab, PollOverrideIsValidatedAgainstTheBudget) {
  auto settings = options();
  settings.poll_seconds = 1s;
  providers::SchwabProvider provider(settings);
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  std::vector<std::string> many;
  for (int i = 0; i < 70; ++i) many.push_back("SPX");
  provider.poll_once(http, "SPX", {many, 1}, sink);
  EXPECT_NE(sink.all<md::ProviderStatus>().at(0).message.find("needs at least"), std::string::npos);
}

TEST(Schwab, CancellationStopsTheRequestSequence) {
  providers::SchwabProvider provider(options());
  test::HttpStub http;
  http.respond = [&](std::string_view url) { provider.stop(); return fixture(url); };
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 1}, sink);
  EXPECT_LE(http.urls.size(), 2u);
  EXPECT_TRUE(sink.events.empty());
}

TEST(BrokerJson, FormValuesAndBase64FollowTheirRfcs) {
  EXPECT_EQ(providers::broker::form_value("a b&c=d~"), "a%20b%26c%3Dd~");
  EXPECT_EQ(providers::broker::base64(""), "");
  EXPECT_EQ(providers::broker::base64("f"), "Zg==");
  EXPECT_EQ(providers::broker::base64("fo"), "Zm8=");
  EXPECT_EQ(providers::broker::base64("foo"), "Zm9v");
  EXPECT_EQ(providers::broker::base64("foobar"), "Zm9vYmFy");
  EXPECT_EQ(providers::broker::zoned_time(Json("2026-09-24T15:30:00Z")), kNow);
  EXPECT_EQ(providers::broker::zoned_time(Json("2026-09-24T11:30:00.5-04:00")), kNow + 500'000'000);
  EXPECT_EQ(providers::broker::zoned_time(Json("2026-09-24T15:30:00")), 0);
  EXPECT_EQ(providers::broker::zoned_time(Json(5)), 0);
}

TEST(RequestBudget, RollsItsWindowAndBacksOffAfterThrottling) {
  providers::RequestBudget budget(2, md::kNanosPerSecond);
  EXPECT_EQ(budget.wait(kNow), 0ms);
  budget.sent(kNow);
  budget.sent(kNow + 100'000'000);
  EXPECT_EQ(budget.wait(kNow + 200'000'000), 800ms);
  EXPECT_EQ(budget.wait(kNow + md::kNanosPerSecond), 0ms);
  budget.throttled(kNow, 0s);
  EXPECT_EQ(budget.wait(kNow), 2000ms);
  budget.throttled(kNow, 30s);
  EXPECT_EQ(budget.wait(kNow), 30000ms);
  EXPECT_EQ(providers::RequestBudget(120, md::kNanosPerMinute).interval(3), 2s);
  EXPECT_THROW(providers::RequestBudget(0, md::kNanosPerSecond), std::invalid_argument);
}

TEST(BrokerFactory, SchwabReadsCredentialsOnlyFromTheEnvironment) {
  EXPECT_THROW((void)providers::make_provider({"schwab", "", {{"refresh_token", "x"}}}), std::invalid_argument);
  EXPECT_THROW((void)providers::make_provider({"schwab", "", {{"poll_seconds", "0"}}}), std::invalid_argument);
  const auto names = providers::provider_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "schwab"), names.end());
}
}  // namespace
