#include "openport/providers/tradier.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <nlohmann/json.hpp>

#include "openport/providers/factory.hpp"
#include "../../apps/probe/certification.hpp"
#include "support/broker_sink.hpp"
#include "support/http_stub.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr md::Timestamp kNow = 1790263800000LL * 1'000'000;

// Adapted from Tradier's archived VXX chain and current quote/lookup examples:
// generated SPX/SPXW prices and dates, with singleton objects and null fields.
constexpr std::string_view kChain = R"({"options":{"option":{
  "symbol":"SPXW261005P07405000","underlying":"SPX","bid":4.4,"ask":4.6,
  "bid_date":1790263800000,"ask_date":1790263801000,"bidsize":239,"asksize":611,
  "open_interest":10,"volume":321,"trade_date":1790263800000,"contract_size":100,"greeks":{"delta":-0.2,"gamma":0.001,
  "mid_iv":0.3577,"theta":-0.33,"vega":1.2,"rho":0.1,"updated_at":"2026-09-24 13:59:03"}
}}})";

net::HttpResponse fixture(std::string_view url) {
  if (url.find("/expirations?") != std::string_view::npos)
    return {200, R"({"expirations":{"date":["2026-10-05","2026-10-16"]}})"};
  if (url.find("/lookup?") != std::string_view::npos)
    return {200, R"({"symbols":[{"rootSymbol":"SPXW","options":["SPXW261005P07405000"]},
                    {"rootSymbol":"SPX","options":"SPX261016C07400000"}]})"};
  if (url.find("/quotes?") != std::string_view::npos)
    return {200, R"({"quotes":{"quote":{"symbol":"SPX","last":7405,"trade_date":1790263700000}}})"};
  if (url.find("symbol=SPXW") != std::string_view::npos) return {200, std::string(kChain)};
  return {200, R"({"options":{"option":null}})"};
}

TEST(Tradier, NormalizesBothExpirationShapesAndNullLists) {
  const std::set<md::Date> expected{{2026, 10, 5}};
  EXPECT_EQ(providers::parse_tradier_expirations(R"({"expirations":{"date":"2026-10-05"}})"), expected);
  EXPECT_EQ(providers::parse_tradier_expirations(R"({"expirations":{"expiration":{"date":"2026-10-05","strikes":{"strike":7405}}}})"), expected);
  EXPECT_EQ(providers::parse_tradier_expirations(R"({"expirations":{"date":["2026-10-05"],"strikes":{"strike":{"date":"2026-10-05","strike":[7405]}}}})"), expected);
  EXPECT_TRUE(providers::parse_tradier_expirations(R"({"expirations":null})").empty());
  EXPECT_THROW((void)providers::parse_tradier_expirations(R"({"expirations":{"date":"2026-02-30"}})"), std::runtime_error);
  EXPECT_THROW((void)providers::parse_tradier_expirations(R"({"fault":{"faultstring":"secret"}})"), std::runtime_error);
}

TEST(Tradier, AcceptsBothLookupShapesAndFindsWeeklyRoots) {
  const auto modern = providers::parse_tradier_lookup(R"({"options":{"option":{"symbol":"SPXW261005P07405000","rootsymbol":"SPXW"}}})");
  const auto archived = providers::parse_tradier_lookup(R"({"symbols":{"rootSymbol":"SPXW","options":"SPXW261005P07405000"}})");
  EXPECT_EQ(modern, archived);
  EXPECT_EQ(modern.at("SPXW"), (std::set<md::Date>{{2026, 10, 5}}));
  EXPECT_TRUE(providers::parse_tradier_lookup(R"({"options":null})").empty());
  EXPECT_TRUE(providers::parse_tradier_lookup(R"({"symbols":null})").empty());
}

TEST(Tradier, MapsCanonicalOsiMarketClockAndOnlyKnownGreekUnits) {
  const auto chain = providers::parse_tradier_chain(kChain);
  ASSERT_EQ(chain.size(), 1u);
  const auto& option = chain.front();
  EXPECT_EQ(option.contract.osi_symbol(), "SPXW  261005P07405000");
  EXPECT_EQ(option.contract.settlement, md::Settlement::PM);
  EXPECT_EQ(option.contract.style, pricing::ExerciseStyle::European);
  EXPECT_EQ(option.ts, kNow + md::kNanosPerSecond);
  EXPECT_DOUBLE_EQ(option.bid, 4.4);
  EXPECT_DOUBLE_EQ(option.open_interest, 10);
  ASSERT_TRUE(option.greeks);
  EXPECT_DOUBLE_EQ(option.greeks->delta, -0.2);
  EXPECT_DOUBLE_EQ(option.greeks->iv, .3577);
  EXPECT_DOUBLE_EQ(option.greeks->vega, 1.2);
  EXPECT_DOUBLE_EQ(option.greeks->theta, -.33);
  EXPECT_DOUBLE_EQ(option.greeks->rho, .1);
  EXPECT_EQ(option.greeks->ts, 0);
}

TEST(Tradier, MissingValuesStayMissingAndBadClocksDoNotBecomeFresh) {
  auto json = Json::parse(kChain);
  auto& option = json["options"]["option"];
  option["open_interest"] = nullptr;
  option["greeks"] = nullptr;
  option["contract_size"] = 10;
  auto parsed = providers::parse_tradier_chain(json.dump()).front();
  EXPECT_LT(parsed.open_interest, 0);
  EXPECT_FALSE(parsed.greeks);
  EXPECT_FALSE(parsed.contract.standard);
  option["bid_date"] = 1790263800;  // conflicting seconds-shaped docs are not guessed
  EXPECT_THROW((void)providers::parse_tradier_chain(json.dump()), std::runtime_error);
  EXPECT_TRUE(providers::parse_tradier_chain(R"({"options":null})").empty());
  EXPECT_TRUE(providers::parse_tradier_chain(R"({"options":{"option":null}})").empty());
  EXPECT_THROW((void)providers::parse_tradier_chain(R"({"options":{"option":{"symbol":"bad"}}})"), std::runtime_error);
}

TEST(Tradier, UnderlyingLastKeepsItsOwnOlderTimestamp) {
  const auto quote = providers::parse_tradier_quote(R"({"quotes":{"quote":[{"symbol":"SPX","last":7405,
    "trade_date":1790263700000,"bid":7404,"ask":7406,"bid_date":1790263800000,"ask_date":1790263800000}]}})", "SPX");
  EXPECT_EQ(quote.ts, kNow - 100 * md::kNanosPerSecond);
  EXPECT_EQ(quote.last, 7405);
  EXPECT_EQ(quote.bid, 0);
  EXPECT_EQ(providers::parse_tradier_quote(R"({"quotes":null})", "SPX").ts, 0);
}

TEST(Tradier, VolumeNeedsAValidCountAndItsOwnTradeClock) {
  for (const Json& volume : {Json(0), Json(12), Json(-1), Json(1.5), Json(), Json("NaN")}) {
    for (const Json& time : {Json(1790263800000LL), Json(), Json(1790263800)}) {
      providers::TradierProvider provider({.access_token = "test", .clock = [] { return kNow; }});
      test::HttpStub http;
      http.respond = [&](auto url) {
        auto reply = fixture(url);
        if (url.find("/chains?symbol=SPXW") != std::string_view::npos) {
          auto doc = Json::parse(reply.body);
          doc["options"]["option"]["volume"] = volume;
          doc["options"]["option"]["trade_date"] = time;
          reply.body = doc.dump();
        }
        return reply;
      };
      test::BrokerSink sink;
      provider.poll_once(http, "SPX", {{"SPX"}, 1}, sink);
      const auto values = sink.all<md::OptionVolume>();
      if ((volume == 0 || volume == 12) && time == 1790263800000LL) {
        ASSERT_EQ(values.size(), 1u);
        EXPECT_EQ(values[0].contracts, volume.get<double>());
        EXPECT_EQ(values[0].ts, kNow);
      } else EXPECT_TRUE(values.empty());
      EXPECT_TRUE(sink.all<md::OptionTrade>().empty());
    }
  }
}

TEST(Tradier, MissingGreekFieldsStayNan) {
  auto doc = Json::parse(kChain);
  doc["options"]["option"]["greeks"] = {{"delta", .5}, {"mid_iv", 0}, {"vega", "NaN"}};
  const auto greek = *providers::parse_tradier_chain(doc.dump())[0].greeks;
  EXPECT_EQ(greek.delta, .5);
  for (double value : {greek.iv, greek.vega, greek.theta, greek.rho}) EXPECT_TRUE(std::isnan(value));
}

TEST(TradierBudget, BudgetsWholeCyclesAndRollingRequests) {
  providers::TradierBudget production(false), sandbox(true);
  EXPECT_EQ(production.interval(5), 3s);
  EXPECT_EQ(sandbox.interval(5), 5s);
  EXPECT_EQ(production.interval(241), 121s);
  for (int i = 0; i < 120; ++i) production.sent(kNow);
  EXPECT_EQ(production.wait(kNow), 60s);
  EXPECT_EQ(production.wait(kNow + 59 * md::kNanosPerSecond), 1s);
  EXPECT_EQ(production.wait(kNow + md::kNanosPerMinute), 0s);
}

TEST(TradierBudget, HonorsCaseInsensitiveHeadersAndOtherTokenConsumers) {
  providers::TradierBudget budget(false);
  net::HttpResponse response{200, "{}"};
  response.headers = {{"x-ratelimit-allowed", "60"}, {"X-Ratelimit-Used", "59"}, {"X-Ratelimit-Available", "1"}};
  EXPECT_FALSE(budget.received(response, kNow));
  EXPECT_EQ(budget.interval(5), 5s);
  EXPECT_EQ(budget.wait(kNow), 0s);
  budget.sent(kNow);
  EXPECT_EQ(budget.wait(kNow), 60s);
  EXPECT_EQ(budget.wait(kNow + md::kNanosPerMinute), 0s);
}

TEST(TradierBudget, BacksOffOn429AndUndocumentedQuotaSignalsWithACap) {
  providers::TradierBudget budget(false);
  auto now = kNow;
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(budget.received({i % 2 == 0 ? 429 : 403, "Quota Violation"}, now));
    const auto delay = budget.wait(now);
    EXPECT_EQ(delay, std::chrono::seconds(std::min(300, 60 * (1 << std::min(i, 3)))));
    now += delay.count() * md::kNanosPerSecond;
  }
  budget.succeeded();
  EXPECT_TRUE(budget.received({200, R"({"error":"Rate limit exceeded"})"}, now));
  EXPECT_EQ(budget.wait(now), 60s);
}

TEST(TradierBudget, ExhaustionWithSuccessWaitsForExpiryWithoutDiscardingResponse) {
  providers::TradierBudget budget(false);
  net::HttpResponse response{200, "{}"};
  response.headers = {{"X-Ratelimit-Available", "0"}, {"X-Ratelimit-Expiry", "1790263920000"}};
  EXPECT_FALSE(budget.received(response, kNow));
  EXPECT_EQ(budget.wait(kNow), 121s);
  response.headers = {{"Retry-After", "200"}};
  response.status = 429;
  EXPECT_TRUE(budget.received(response, kNow));
  EXPECT_EQ(budget.wait(kNow), 200s);
}

TEST(Tradier, FullPollUsesOnlyMarketPathsAndPublishesDefinitionBeforeData) {
  providers::TradierProvider provider({.access_token = "secret", .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 1, .1}, sink);
  ASSERT_EQ(http.urls.size(), 4u);
  EXPECT_NE(http.urls[0].find("includeAllRoots=true"), std::string::npos);
  EXPECT_NE(http.urls.back().find("symbol=SPXW&expiration=2026-10-05&greeks=true"), std::string::npos);
  for (std::size_t i = 0; i < http.urls.size(); ++i) {
    EXPECT_TRUE(http.urls[i].starts_with("https://api.tradier.com/v1/markets/"));
    EXPECT_EQ(http.urls[i].find("secret"), std::string::npos);
    net::HttpResponse headers;
    headers.headers = http.sent_headers[i];
    EXPECT_EQ(headers.header("Authorization"), "Bearer secret");
    EXPECT_EQ(headers.header("Accept"), "application/json");
  }
  ASSERT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].bid_size, 239);  // option sizes read as contracts by default
  EXPECT_TRUE(std::holds_alternative<md::UnderlyingQuote>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<md::ContractDefinition>(sink.events[1]));
  EXPECT_TRUE(std::holds_alternative<md::OptionQuote>(sink.events[2]));
  EXPECT_EQ(sink.all<md::SnapshotComplete>()[0].ts, kNow + md::kNanosPerSecond);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Stale);
  EXPECT_NE(sink.all<md::ProviderStatus>()[0].message.find("unconfirmed"), std::string::npos);
  ASSERT_EQ(sink.all<md::OptionVolume>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionVolume>()[0].contracts, 321);
  EXPECT_EQ(sink.all<md::OptionVolume>()[0].ts, kNow);
  EXPECT_EQ(provider.capabilities().poll_interval, 2s);
  sink.events.clear();
  provider.poll_once(http, "SPX", {{"SPX"}, 1, .1}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_EQ(sink.all<md::SnapshotComplete>().size(), 1u);
}

TEST(Tradier, CertificationUsesTheExistingChainAndHttpFixtures) {
  providers::TradierProvider provider({.access_token = "redact-this-token", .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  probe::Conformance monitor({{"SPX"}, 1, .1}, false, 1);
  provider.poll_once(http, "SPX", {{"SPX"}, 1, .1}, monitor);
  const auto report = monitor.report();
  EXPECT_EQ(report["contracts_seen"], 1);
  EXPECT_EQ(report["presence"]["volume_contracts"], 1);
  EXPECT_EQ(report["timestamps"]["VendorGreeks"]["unknown"], 1);
  EXPECT_EQ(report["greeks"]["iv"]["minimum"], .3577);
  EXPECT_EQ(report["outcome"], "review");
  EXPECT_EQ(report.dump().find("redact-this-token"), std::string::npos);
}

TEST(Tradier, FailedRootDoesNotPublishOrRetireTheLastSnapshot) {
  providers::TradierProvider provider({.access_token = "secret", .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  sink.events.clear();
  http.respond = [](auto url) {
    if (url.find("/chains?symbol=SPX&") != std::string_view::npos) return net::HttpResponse{503, "secret"};
    return fixture(url);
  };
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  EXPECT_TRUE(sink.all<md::UnderlyingQuote>().empty());
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Error);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].message.find("secret"), std::string::npos);
}

TEST(Tradier, FilterDriftKeepsDefinedContractsAndCompleteEmptyChainRetiresThem) {
  providers::TradierProvider provider({.access_token = "secret", .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 1, .01}, sink);
  ASSERT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  sink.events.clear();
  http.respond = [](auto url) {
    auto response = fixture(url);
    if (url.find("/quotes?") != std::string_view::npos) {
      auto doc = Json::parse(response.body);
      doc["quotes"]["quote"]["last"] = 9000;
      response.body = doc.dump();
    }
    if (url.find("/chains?symbol=SPXW") != std::string_view::npos) {
      auto doc = Json::parse(response.body);
      doc["options"]["option"]["bid"] = 4.5;
      response.body = doc.dump();
    }
    return response;
  };
  provider.poll_once(http, "SPX", {{"SPX"}, 1, .01}, sink);
  ASSERT_EQ(sink.all<md::OptionQuote>().size(), 1u);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].bid, 4.5);
  sink.events.clear();
  http.respond = [](auto url) {
    if (url.find("/chains?") != std::string_view::npos) return net::HttpResponse{200, R"({"options":null})"};
    return fixture(url);
  };
  provider.poll_once(http, "SPX", {{"SPX"}, 1, .01}, sink);
  EXPECT_EQ(sink.all<md::OptionQuote>()[0].bid, 0);
}

TEST(Tradier, PollOverrideIsValidatedAcrossAllUnderlyings) {
  providers::TradierProvider provider({.access_token = "test", .poll_seconds = 1s, .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX", "SPY"}, 1}, sink);
  EXPECT_EQ(http.urls.size(), 3u);
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  EXPECT_NE(sink.all<md::ProviderStatus>()[0].message.find("needs at least 4"), std::string::npos);
}

TEST(Tradier, IncompleteLookupNeverVouchesForAnUnfetchedExpiry) {
  providers::TradierProvider provider({.access_token = "test", .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = [](auto url) {
    if (url.find("/lookup?") != std::string_view::npos)
      return net::HttpResponse{200, R"({"options":{"option":{"symbol":"SPXW261005P07405000"}}})"};
    return fixture(url);
  };
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  EXPECT_TRUE(sink.all<md::ContractDefinition>().empty());
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Error);
}

TEST(Tradier, EquityAndIndexHealthAreReportedSeparately) {
  providers::TradierProvider provider({.access_token = "test", .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX", "SPY"}, 1}, sink);
  http.respond = [](auto url) {
    if (url.find("/expirations?") != std::string_view::npos)
      return net::HttpResponse{200, R"({"expirations":null})"};
    if (url.find("/lookup?") != std::string_view::npos)
      return net::HttpResponse{200, R"({"options":null})"};
    return net::HttpResponse{200, R"({"quotes":{"quote":{"symbol":"SPY","last":500,"trade_date":1790263800000}}})"};
  };
  provider.poll_once(http, "SPY", {{"SPX", "SPY"}, 1}, sink);
  const auto statuses = sink.all<md::ProviderStatus>();
  ASSERT_EQ(statuses.size(), 2u);
  EXPECT_EQ(statuses[0].underlying, "SPX");
  EXPECT_EQ(statuses[0].state, md::FeedState::Stale);
  EXPECT_EQ(statuses[1].underlying, "SPY");
  EXPECT_EQ(statuses[1].state, md::FeedState::Live);
  EXPECT_FALSE(provider.capabilities().realtime);
}

TEST(Tradier, CancellationStopsEndpointSequenceAndStopIsRepeatable) {
  providers::TradierProvider provider({.access_token = "test"});
  test::HttpStub http;
  http.respond = [&](auto url) { provider.stop(); return fixture(url); };
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  EXPECT_EQ(http.urls.size(), 1u);
  EXPECT_TRUE(sink.events.empty());
  provider.stop();
}

TEST(Tradier, SandboxUsesItsOriginAndReportsDelay) {
  providers::TradierProvider provider({.access_token = "test", .sandbox = true, .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 1}, sink);
  EXPECT_TRUE(http.urls[0].starts_with("https://sandbox.tradier.com/v1/"));
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Delayed);
  EXPECT_EQ(provider.capabilities().delay, 900s);
  EXPECT_FALSE(provider.capabilities().realtime);
  EXPECT_EQ(provider.capabilities().poll_interval, 4s);
}

TEST(Tradier, ConfirmedContractSizeUnitsPublishOnlyActualDisplayedLiquidity) {
  providers::TradierProvider provider({.access_token = "test", .size_multiplier = 1,
                                       .clock = [] { return kNow; }});
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}, 1}, sink);
  const auto quotes = sink.all<md::OptionQuote>();
  ASSERT_EQ(quotes.size(), 1u);
  EXPECT_EQ(quotes[0].bid_size, 239);
  EXPECT_EQ(quotes[0].ask_size, 611);
}
}  // namespace
