#include "openport/providers/alpaca.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/providers/factory.hpp"
#include "support/broker_sink.hpp"
#include "support/http_stub.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr md::Timestamp kNow = 1790263800000LL * 1'000'000;  // 2026-09-24T15:30:00Z

/// Sets an environment variable, or unsets it for nullptr, until the end of the scope.
class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
    if (const char* previous = std::getenv(name)) previous_ = previous;
    if (value) {
      setenv(name, value, 1);
    } else {
      unsetenv(name);
    }
  }
  ~ScopedEnvironment() {
    if (previous_) {
      setenv(name_, previous_->c_str(), 1);
    } else {
      unsetenv(name_);
    }
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  const char* name_;
  std::optional<std::string> previous_;
};

// Shaped after the option chain snapshot and stock snapshot examples in Alpaca's
// market-data reference, with generated SPY values; not observed market data.
constexpr std::string_view kPage1 = R"({"snapshots":{
  "SPY261005C00600000":{"latestQuote":{"ap":4.12,"as":31,"ax":"C","bp":4.08,"bs":12,"bx":"X","c":"A","t":"2026-09-24T15:29:59.871263744Z"},
    "latestTrade":{"c":"I","p":4.1,"s":2,"t":"2026-09-24T15:29:41.103Z","x":"C"},
    "dailyBar":{"c":4.1,"h":4.4,"l":3.9,"n":120,"o":4.0,"t":"2026-09-24T04:00:00Z","v":815,"vw":4.12},
    "greeks":{"delta":0.52,"gamma":0.031,"rho":0.05,"theta":-0.21,"vega":0.33},"impliedVolatility":0.1712}},
  "next_page_token":"U1BZMjYxMDA1UDAwNjAwMDAw"})";
constexpr std::string_view kPage2 = R"({"snapshots":{
  "SPY261005P00600000":{"latestQuote":{"ap":0,"as":0,"bp":0,"bs":0,"t":"2026-09-24T15:29:58Z"}},
  "SPY261005P00700000":{"latestQuote":{"ap":50.5,"as":3,"bp":49.9,"bs":3,"t":"2026-09-24T15:29:30Z"}}},
  "next_page_token":null})";
constexpr std::string_view kStock = R"({"symbol":"SPY",
  "latestTrade":{"t":"2026-09-24T15:29:50.5Z","x":"V","p":600.21,"s":100,"c":["@"],"i":1,"z":"B"},
  "latestQuote":{"t":"2026-09-24T15:29:59Z","ax":"V","ap":600.25,"as":2,"bx":"V","bp":600.19,"bs":3,"c":["R"],"z":"B"}})";

net::HttpResponse fixture(std::string_view url) {
  if (url.find("/v2/stocks/SPY/snapshot") != std::string_view::npos) return {200, std::string(kStock)};
  if (url.find("page_token=") != std::string_view::npos) return {200, std::string(kPage2)};
  if (url.find("/v1beta1/options/snapshots/SPY") != std::string_view::npos) return {200, std::string(kPage1)};
  return {404, "{}"};
}

providers::AlpacaProvider::Options options() {
  return {.key_id = "key-id", .secret_key = "secret-key", .clock = [] { return kNow; }};
}

TEST(Alpaca, MapsCompactOccQuoteClocksAndOnlyDocumentedGreekUnits) {
  const auto page = providers::parse_alpaca_option_page(kPage1, "SPY");
  ASSERT_EQ(page.options.size(), 1u);
  EXPECT_EQ(page.next_page_token, "U1BZMjYxMDA1UDAwNjAwMDAw");
  const auto& option = page.options[0];
  EXPECT_EQ(option.contract.osi_symbol(), "SPY   261005C00600000");
  EXPECT_EQ(option.contract.style, pricing::ExerciseStyle::American);
  EXPECT_EQ(option.ts, kNow - md::kNanosPerSecond + 871263744);
  EXPECT_DOUBLE_EQ(option.bid, 4.08);
  EXPECT_DOUBLE_EQ(option.ask_size, 31);
  EXPECT_DOUBLE_EQ(option.volume, 815);
  EXPECT_EQ(option.volume_ts, kNow - 19 * md::kNanosPerSecond + 103'000'000);
  ASSERT_TRUE(option.greeks);
  EXPECT_DOUBLE_EQ(option.greeks->delta, 0.52);
  EXPECT_TRUE(std::isnan(option.greeks->iv));
  EXPECT_TRUE(std::isnan(option.greeks->theta));
  EXPECT_TRUE(providers::parse_alpaca_option_page(kPage2, "SPY").next_page_token.empty());
}

TEST(Alpaca, RejectsMismatchedOrUntimedQuotesAndUnzonedTimes) {
  EXPECT_THROW((void)providers::parse_alpaca_option_page(kPage1, "QQQ"), std::runtime_error);
  auto json = Json::parse(kPage1);
  json["snapshots"]["SPY261005C00600000"]["latestQuote"]["t"] = "2026-09-24T15:29:59";
  EXPECT_THROW((void)providers::parse_alpaca_option_page(json.dump(), "SPY"), std::runtime_error);
  json["snapshots"]["SPY261005C00600000"]["latestQuote"].erase("t");
  EXPECT_THROW((void)providers::parse_alpaca_option_page(json.dump(), "SPY"), std::runtime_error);
  // A previous day's bar is not today's volume.
  json = Json::parse(kPage1);
  json["snapshots"]["SPY261005C00600000"]["dailyBar"]["t"] = "2026-09-23T04:00:00Z";
  EXPECT_LT(providers::parse_alpaca_option_page(json.dump(), "SPY").options[0].volume, 0);
  EXPECT_TRUE(providers::parse_alpaca_option_page(R"({"snapshots":null})", "SPY").options.empty());
  EXPECT_THROW((void)providers::parse_alpaca_option_page(R"({"snapshots":[1]})", "SPY"), std::runtime_error);
}

TEST(Alpaca, StockSnapshotPrefersTheTradeOnItsOwnClock) {
  const auto quote = providers::parse_alpaca_stock_snapshot(kStock, "SPY");
  EXPECT_EQ(quote.ts, kNow - 10 * md::kNanosPerSecond + 500'000'000);
  EXPECT_DOUBLE_EQ(quote.last, 600.21);
  EXPECT_EQ(quote.bid, 0);
  auto json = Json::parse(kStock);
  json.erase("latestTrade");
  const auto quoted = providers::parse_alpaca_stock_snapshot(json.dump(), "SPY");
  EXPECT_EQ(quoted.ts, kNow - md::kNanosPerSecond);
  EXPECT_DOUBLE_EQ(quoted.bid, 600.19);
  EXPECT_THROW((void)providers::parse_alpaca_stock_snapshot(kStock, "QQQ"), std::runtime_error);
}

TEST(Alpaca, FullPollFollowsPagesWithKeyHeadersAndPublishesOneCompleteSnapshot) {
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}, 0, 0.1}, sink);
  ASSERT_EQ(http.urls.size(), 3u);
  EXPECT_EQ(http.urls[0], "https://data.alpaca.markets/v2/stocks/SPY/snapshot?feed=iex");
  EXPECT_EQ(http.urls[1], "https://data.alpaca.markets/v1beta1/options/snapshots/SPY?feed=indicative&limit=1000"
                          "&expiration_date_gte=2026-09-24&strike_price_gte=540.18&strike_price_lte=660.24");
  EXPECT_EQ(http.urls[2], http.urls[1] + "&page_token=U1BZMjYxMDA1UDAwNjAwMDAw");
  for (std::size_t i = 0; i < http.urls.size(); ++i) {
    EXPECT_EQ(http.urls[i].find("secret"), std::string::npos);
    net::HttpResponse sent;
    sent.headers = http.sent_headers[i];
    EXPECT_EQ(sent.header("APCA-API-KEY-ID"), "key-id");
    EXPECT_EQ(sent.header("APCA-API-SECRET-KEY"), "secret-key");
  }
  EXPECT_TRUE(std::holds_alternative<md::UnderlyingQuote>(sink.events[0]));
  EXPECT_TRUE(std::holds_alternative<md::ContractDefinition>(sink.events[1]));
  // 700 is outside 10% of 600.21; the two 600 strikes are defined.
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 2u);
  EXPECT_EQ(sink.all<md::OptionVolume>().size(), 1u);
  EXPECT_EQ(sink.all<md::SnapshotComplete>()[0].ts, kNow - md::kNanosPerSecond + 871263744);
  const auto status = sink.all<md::ProviderStatus>()[0];
  EXPECT_EQ(status.state, md::FeedState::Delayed);
  EXPECT_NE(status.message.find("not the OPRA NBBO"), std::string::npos);
  EXPECT_EQ(provider.capabilities().poll_interval, 5s);
  EXPECT_FALSE(provider.capabilities().open_interest);
}

TEST(Alpaca, OpraFeedIsLiveAndAWidenedRequestKeepsDefinedStrikes) {
  auto settings = options();
  settings.feed = providers::AlpacaProvider::Feed::Opra;
  settings.stock_feed = "sip";
  providers::AlpacaProvider provider(settings);
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}, 0, 0.1}, sink);
  EXPECT_NE(http.urls[1].find("feed=opra"), std::string::npos);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Live);
  http.urls.clear();
  http.respond = [](std::string_view url) {
    auto response = fixture(url);
    if (url.find("/v2/stocks/") != std::string_view::npos) {
      auto doc = Json::parse(response.body);
      doc["latestTrade"]["p"] = 800;
      response.body = doc.dump();
    }
    return response;
  };
  provider.poll_once(http, "SPY", {{"SPY"}, 0, 0.1}, sink);
  EXPECT_NE(http.urls[1].find("strike_price_gte=600.00&strike_price_lte=880.00"), std::string::npos) << http.urls[1];
}

TEST(Alpaca, FailedPageNeverPublishesAPartialChain) {
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  http.respond = [](std::string_view url) {
    if (url.find("page_token=") != std::string_view::npos) return net::HttpResponse{500, "secret"};
    return fixture(url);
  };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_TRUE(sink.all<md::ContractDefinition>().empty());
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  const auto status = sink.all<md::ProviderStatus>().at(0);
  EXPECT_EQ(status.state, md::FeedState::Error);
  EXPECT_EQ(status.message.find("secret"), std::string::npos);
}

TEST(Alpaca, ThrottlingAndMissingEntitlementAreExplained) {
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  http.respond = [](std::string_view) { return net::HttpResponse{403, "{\"message\":\"subscription does not permit\"}"}; };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_NE(sink.all<md::ProviderStatus>().at(0).message.find("subscription includes"), std::string::npos);
  sink.events.clear();
  http.respond = [](std::string_view) { return net::HttpResponse{429, "{}"}; };
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_NE(sink.all<md::ProviderStatus>().at(0).message.find("rate limited"), std::string::npos);
}

TEST(Alpaca, RateLimitHoldsRequestsUntilItsRetryAfter) {
  // A Retry-After beyond the one-minute window holds the next request until it passes;
  // dropped or misread, the minute alone would send it into the same limit again.
  md::Timestamp reading = kNow;
  bool advancing = false;
  auto settings = options();
  settings.clock = [&] { return advancing ? (reading += 1'000'000) : reading; };
  providers::AlpacaProvider provider(settings);
  std::vector<md::Timestamp> sent_at;
  test::HttpStub http;
  http.respond = [&](std::string_view url) {
    sent_at.push_back(reading);
    if (sent_at.size() > 1u) return fixture(url);
    net::HttpResponse throttled{429, "{}"};
    throttled.headers = {{"Retry-After", "90"}};
    return throttled;
  };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).message, "alpaca SPY: rate limited; backing off");
  // The next poll starts 5 ms short of the 90 s, its clock moving 1 ms at every reading.
  sink.events.clear();
  reading = kNow + 90 * md::kNanosPerSecond - 6'000'000;
  advancing = true;
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, md::FeedState::Delayed);
  ASSERT_EQ(sent_at.size(), 4u);
  for (std::size_t i = 1; i < sent_at.size(); ++i) EXPECT_GE(sent_at[i], kNow + 90 * md::kNanosPerSecond) << i;
}

TEST(Alpaca, PagesThatNeverEndStopAtOneHundred) {
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  int served = 0;
  http.respond = [&](std::string_view url) {
    if (url.find("page_token=") == std::string_view::npos) return fixture(url);
    // The same token again and again; it ends after 150 pages, so a missing limit fails instead of hanging.
    return net::HttpResponse{200, ++served < 150 ? R"({"snapshots":{},"next_page_token":"U1BZMjYxMDA1UDAwNjAwMDAw"})"
                                                 : R"({"snapshots":{},"next_page_token":null})"};
  };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  // The stock snapshot, then the first page and 99 more.
  EXPECT_EQ(http.urls.size(), 101u);
  EXPECT_TRUE(sink.all<md::ContractDefinition>().empty());
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  const auto status = sink.all<md::ProviderStatus>().at(0);
  EXPECT_EQ(status.state, md::FeedState::Error);
  EXPECT_EQ(status.message, "alpaca SPY: more than 100 pages; narrow --window");
}

TEST(Alpaca, PollIntervalKeepsEveryUnderlyingsRequestsInTheBudget) {
  // SPY takes three requests (its stock snapshot and two pages), and QQQ, not polled
  // yet, is counted as many: six a cycle at ten a minute is one cycle every 36 s.
  auto settings = options();
  settings.requests_per_minute = 10;
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  providers::AlpacaProvider budgeted(settings);
  budgeted.poll_once(http, "SPY", {{"SPY", "QQQ"}}, sink);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, md::FeedState::Delayed);
  EXPECT_EQ(budgeted.capabilities().poll_interval, 36s);
  // A poll_seconds the budget cannot keep is refused; one it can is taken as given.
  settings.poll_seconds = 1s;
  providers::AlpacaProvider hurried(settings);
  sink.events.clear();
  hurried.poll_once(http, "SPY", {{"SPY", "QQQ"}}, sink);
  EXPECT_TRUE(sink.all<md::SnapshotComplete>().empty());
  const auto refused = sink.all<md::ProviderStatus>().at(0);
  EXPECT_EQ(refused.state, md::FeedState::Error);
  EXPECT_EQ(refused.message, "alpaca SPY: poll_seconds is below the request budget; needs at least 36");
  settings.poll_seconds = 40s;
  providers::AlpacaProvider patient(settings);
  sink.events.clear();
  patient.poll_once(http, "SPY", {{"SPY", "QQQ"}}, sink);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, md::FeedState::Delayed);
  EXPECT_EQ(patient.capabilities().poll_interval, 40s);
}

TEST(Alpaca, AQuoteMidpointCentersTheStrikeWindowWithoutATrade) {
  auto stock = Json::parse(kStock);
  stock.erase("latestTrade");
  const auto quoted = stock.dump();
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  http.respond = [&](std::string_view url) {
    if (url.find("/v2/stocks/") != std::string_view::npos) return net::HttpResponse{200, quoted};
    return fixture(url);
  };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}, 0, 0.1}, sink);
  ASSERT_EQ(http.urls.size(), 3u);
  // 10% around 600.22, halfway between the 600.19 bid and the 600.25 ask, rounded outward.
  EXPECT_NE(http.urls[1].find("&strike_price_gte=540.19&strike_price_lte=660.25"), std::string::npos) << http.urls[1];
  const auto spot = sink.all<md::UnderlyingQuote>().at(0);
  EXPECT_DOUBLE_EQ(spot.bid, 600.19);
  EXPECT_DOUBLE_EQ(spot.ask, 600.25);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 2u);  // 700 is outside 10% of 600.22
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, md::FeedState::Delayed);
}

TEST(Alpaca, NoPriceForTheStrikeWindowOrNoMarketTimeFailsThePoll) {
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  // A stock snapshot with neither a trade nor a quote, and an option quoted without a time.
  http.respond = [](std::string_view url) {
    if (url.find("/v2/stocks/") != std::string_view::npos) return net::HttpResponse{200, R"({"symbol":"SPY"})"};
    return net::HttpResponse{
        200, R"({"snapshots":{"SPY261005P00600000":{"latestQuote":{"ap":0,"as":0,"bp":0,"bs":0}}},"next_page_token":null})"};
  };
  test::BrokerSink sink;
  provider.poll_once(http, "SPY", {{"SPY"}, 0, 0.1}, sink);
  EXPECT_EQ(http.urls.size(), 1u);  // no chain without a price to center the window on
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).message, "alpaca SPY: strike filter needs an underlying price");
  // Without a window the chain is fetched, but nothing in it has a market time to publish at.
  sink.events.clear();
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_EQ(http.urls.size(), 3u);
  ASSERT_EQ(sink.events.size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).message, "alpaca SPY: snapshot has no known market timestamp");
}

TEST(Alpaca, IndexUnderlyingsAreRejected) {
  providers::AlpacaProvider provider(options());
  test::HttpStub http;
  http.respond = fixture;
  test::BrokerSink sink;
  provider.poll_once(http, "SPX", {{"SPX"}}, sink);
  EXPECT_TRUE(http.urls.empty());
  EXPECT_EQ(sink.all<md::ProviderStatus>().at(0).state, md::FeedState::Error);
  EXPECT_THROW(providers::validate_subscription("alpaca", {{"SPY", "SPX"}}), std::invalid_argument);
  EXPECT_NO_THROW(providers::validate_subscription("alpaca", {{"SPY", "QQQ", "IWM"}, 2, .05}));
}

TEST(BrokerFactory, AlpacaValidatesFeedsAndReadsKeysFromTheEnvironment) {
  EXPECT_THROW(providers::AlpacaProvider({.key_id = "a", .secret_key = "b", .stock_feed = "otc"}), std::invalid_argument);
  EXPECT_THROW(providers::AlpacaProvider({.key_id = "", .secret_key = "b"}), std::invalid_argument);
  const auto names = providers::provider_names();
  EXPECT_NE(std::find(names.begin(), names.end(), "alpaca"), names.end());
  const ScopedEnvironment key_id("APCA_API_KEY_ID", "generated-test-key-id");
  const ScopedEnvironment secret_key("APCA_API_SECRET_KEY", "generated-test-secret-key");
  const auto provider = providers::make_provider({"alpaca", "", {}});
  EXPECT_EQ(provider->name(), "alpaca");
  EXPECT_NO_THROW((void)providers::make_provider({"alpaca", "", {{"feed", "opra"}, {"stock_feed", "delayed_sip"},
                                                                 {"requests_per_minute", "10000"}, {"poll_seconds", "60"}}}));
  // With both keys set, a refusal can only be the option's own: a missing key no longer
  // passes for it.
  const auto refuses = [](const std::map<std::string, std::string>& chosen, std::string_view reason) {
    try {
      (void)providers::make_provider({"alpaca", "", chosen});
      ADD_FAILURE() << "accepted, expected: " << reason;
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string_view(error.what()).find(reason), std::string_view::npos) << error.what();
    }
  };
  refuses({{"feed", "sip"}}, "alpaca: feed must be indicative or opra");
  refuses({{"base_url", "https://example.com"}}, "unknown provider option base_url");
  refuses({{"requests_per_minute", "0"}}, "requests_per_minute must be an integer");
  refuses({{"stock_feed", "otc"}}, "stock_feed must be iex, sip or delayed_sip");
  refuses({{"poll_seconds", "0"}}, "poll_seconds must be an integer");
  // Each key is read: with either one unset, no provider is made.
  for (const auto* variable : {"APCA_API_KEY_ID", "APCA_API_SECRET_KEY"}) {
    SCOPED_TRACE(variable);
    const ScopedEnvironment unset(variable, nullptr);
    refuses({}, "set APCA_API_KEY_ID and APCA_API_SECRET_KEY");
  }
}
}  // namespace
