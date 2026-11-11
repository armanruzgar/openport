#include "openport/providers/tastytrade.hpp"
#include "../../apps/probe/certification.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "openport/analytics/chain_book.hpp"
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
  EXPECT_EQ(mapper.subscriptions().size(), 2u);  // the underlying's Quote and Trade
  auto spot = quote("$SPX");
  spot["bidPrice"] = 7399;
  spot["askPrice"] = 7401;
  mapper.event(spot);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 4u);
  // Quote, Greeks, Summary and Trade (for dayVolume) for each option: the only events read for one.
  EXPECT_EQ(mapper.subscriptions().size(), 18u);
  EXPECT_TRUE(mapper.subscriptions().contains({"Trade", ".SPX261016C7400"}));
  EXPECT_FALSE(mapper.subscriptions().contains({"Profile", "$SPX"}));
  sink.events.clear();
  spot["bidPrice"] = 8999;
  spot["askPrice"] = 9001;
  mapper.event(spot);
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 1u);
  EXPECT_EQ(mapper.subscriptions().size(), 22u);
  mapper.event(quote(".SPX261016C7400"));
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 1u);
}

TEST(Tastytrade, UnderlyingEventsWithoutAUsablePriceLeaveSpotAlone) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}}, sink, true);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  sink.events.clear();
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", "$SPX"}, {"price", 7400}, {"time", 1790263800000LL}});
  // A one-sided quote (premarket, say), an empty one, a crossed one, a trade without a price.
  mapper.event({{"eventType", "Quote"}, {"eventSymbol", "$SPX"}, {"bidPrice", 7399.5}, {"askPrice", "NaN"},
                {"bidTime", 1790263801000LL}, {"askTime", 0}});
  mapper.event({{"eventType", "Quote"}, {"eventSymbol", "$SPX"}, {"bidPrice", "NaN"}, {"askPrice", "NaN"},
                {"bidTime", 0}, {"askTime", 0}});
  mapper.event({{"eventType", "Quote"}, {"eventSymbol", "$SPX"}, {"bidPrice", 7401}, {"askPrice", 7399},
                {"bidTime", 1790263802000LL}, {"askTime", 1790263802000LL}});
  mapper.event({{"eventType", "Trade"}, {"eventSymbol", "$SPX"}, {"price", "NaN"}, {"time", 1790263803000LL}});
  EXPECT_EQ(sink.all<md::UnderlyingQuote>().size(), 1u);
  analytics::ChainBook book;
  for (const auto& event : sink.events) book.apply(event);
  EXPECT_EQ(book.underlyings().at("SPX").spot, 7400);
  EXPECT_EQ(book.underlyings().at("SPX").spot_ts, kNow);
  // A two-sided quote does move it.
  mapper.event({{"eventType", "Quote"}, {"eventSymbol", "$SPX"}, {"bidPrice", 7401}, {"askPrice", 7403},
                {"bidTime", 1790263804000LL}, {"askTime", 1790263804000LL}});
  ASSERT_EQ(sink.all<md::UnderlyingQuote>().size(), 2u);
  book.apply(sink.events.back());
  EXPECT_EQ(book.underlyings().at("SPX").spot, 7402);
  EXPECT_EQ(book.underlyings().at("SPX").spot_ts, kNow + 4 * md::kNanosPerSecond);
}

TEST(Tastytrade, SubscriptionsChangeOnlyWhenTheMappedInstrumentsDo) {
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}, 1, .01}, sink);
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  EXPECT_TRUE(mapper.take_subscription_change());  // the underlying
  EXPECT_FALSE(mapper.take_subscription_change());
  auto spot = quote("$SPX");
  spot["bidPrice"] = 7399;
  spot["askPrice"] = 7401;
  mapper.event(spot);
  EXPECT_TRUE(mapper.take_subscription_change());  // the 7400 strikes
  // Ticks that leave the window on the same strikes, and option events, change nothing.
  for (const double bid : {7398.0, 7390.0, 7420.0}) {
    spot["bidPrice"] = bid;
    spot["askPrice"] = bid + 2;
    mapper.event(spot);
  }
  mapper.event(quote(".SPX261016C7400"));
  EXPECT_FALSE(mapper.take_subscription_change());
  EXPECT_EQ(sink.all<md::ContractDefinition>().size(), 4u);
  spot["bidPrice"] = 8999;
  spot["askPrice"] = 9001;
  mapper.event(spot);
  EXPECT_TRUE(mapper.take_subscription_change());
  // The same chain an hour on: nothing to resubscribe.
  mapper.chain("SPX", "$SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  EXPECT_FALSE(mapper.take_subscription_change());
}

TEST(Tastytrade, DriftedContractsMakeRoomAtTheSubscriptionCapFarthestFirst) {
  // One expiry with a strike every point from 5000 to 12000: 14,002 contracts.
  std::vector<providers::TastytradeContract> chain;
  for (int strike = 5000; strike <= 12000; ++strike) {
    for (const char side : {'C', 'P'}) {
      char osi[32];
      std::snprintf(osi, sizeof osi, "SPXW  261016%c%08d", side, strike * 1000);
      chain.push_back({*md::parse_osi(osi), ".SPXW261016" + std::string(1, side) + std::to_string(strike)});
    }
  }
  test::BrokerSink sink;
  providers::TastytradeMapper mapper({{"SPX"}, 0, .01}, sink);
  mapper.chain("SPX", "$SPX", chain, {2026, 9, 24});
  // Spot drifts from 6000 to 9500 through a 1% window, which admits 7,312 contracts
  // along the way: more than DXLink's 25,000 subscriptions hold.
  for (int spot = 6000; spot <= 9500; spot += 25)
    mapper.event({{"eventType", "Trade"}, {"eventSymbol", "$SPX"}, {"price", spot}});
  std::set<providers::DxlinkSubscription> subscriptions;
  ASSERT_NO_THROW(subscriptions = mapper.subscriptions());
  EXPECT_LE(subscriptions.size(), providers::TastytradeMapper::kMaxSubscriptions);
  EXPECT_GT(subscriptions.size(), providers::TastytradeMapper::kMaxSubscriptions - 8);
  // Everything the window admits now, and the drifted strikes nearest spot, stay.
  for (const auto* symbol : {".SPXW261016P9405", ".SPXW261016C9500", ".SPXW261016C9595", ".SPXW261016P9000",
                             ".SPXW261016C7000"})
    EXPECT_TRUE(subscriptions.contains({"Quote", symbol})) << symbol;
  // The farthest drifted ones made the room, and their events are no longer mapped.
  for (const auto* symbol : {".SPXW261016C5940", ".SPXW261016P6000", ".SPXW261016C6300"})
    EXPECT_FALSE(subscriptions.contains({"Quote", symbol})) << symbol;
  sink.events.clear();
  mapper.event(quote(".SPXW261016C5940"));
  EXPECT_TRUE(sink.all<md::OptionQuote>().empty());
}

TEST(Tastytrade, AFailingFeedKeepsReconnectingUntilStopped) {
  std::atomic<int> requests{0};
  test::EventCollector sink;
  providers::TastytradeProvider::Options options;
  options.credentials = {"secret", "refresh", ""};
  options.http = [&requests] {
    auto http = std::make_unique<test::HttpStub>();
    // The OAuth service is down for as long as the test runs.
    http->respond = [&requests](std::string_view) {
      ++requests;
      return net::HttpResponse{503, "{}"};
    };
    return http;
  };
  options.retry_wait = [](unsigned) { return std::chrono::milliseconds(1); };
  providers::TastytradeProvider provider(options);
  provider.start({{"SPX"}}, sink);
  // Eight failures used to stop the feed for good.
  ASSERT_TRUE(test::recording_eventually([&] { return requests.load() >= 20; }));
  provider.stop();
  std::vector<md::ProviderStatus> statuses;
  for (const auto& event : sink.snapshot())
    if (const auto* status = std::get_if<md::ProviderStatus>(&event)) statuses.push_back(*status);
  const auto count = [&](md::FeedState state) {
    return std::count_if(statuses.begin(), statuses.end(), [state](const auto& status) { return status.state == state; });
  };
  EXPECT_GE(count(md::FeedState::Error), 19);
  EXPECT_EQ(count(md::FeedState::Stopped), 1);
  ASSERT_FALSE(statuses.empty());
  EXPECT_EQ(statuses.back().state, md::FeedState::Stopped);
  EXPECT_EQ(statuses.back().message, "tastytrade: stopped");
}

/// DXLink in memory: answers the handshake and keeps what the client sends.
struct DxlinkServer {
  std::mutex mutex;
  std::deque<std::string> replies;
  std::vector<Json> subscriptions;  ///< FEED_SUBSCRIPTION messages, in order
  int connections = 0;
};

class FakeDxlink final : public net::WebSocket {
 public:
  explicit FakeDxlink(std::shared_ptr<DxlinkServer> server) : server_(std::move(server)) {}
  void connect(std::string_view, const std::atomic<bool>*) override {
    const std::lock_guard lock(server_->mutex);
    ++server_->connections;
  }
  void send(std::string_view frame) override {
    const auto message = Json::parse(frame);
    const auto type = message.value("type", std::string());
    const std::lock_guard lock(server_->mutex);
    auto& replies = server_->replies;
    if (type == "SETUP") {
      replies.emplace_back(R"({"type":"SETUP","channel":0,"keepaliveTimeout":60,"acceptKeepaliveTimeout":60,"version":"1.0"})");
      replies.emplace_back(R"({"type":"AUTH_STATE","channel":0,"state":"UNAUTHORIZED"})");
    } else if (type == "AUTH") {
      replies.emplace_back(R"({"type":"AUTH_STATE","channel":0,"state":"AUTHORIZED"})");
    } else if (type == "CHANNEL_REQUEST") {
      replies.emplace_back(R"({"type":"CHANNEL_OPENED","channel":3,"service":"FEED"})");
    } else if (type == "FEED_SETUP") {
      replies.emplace_back(R"({"type":"FEED_CONFIG","channel":3,"dataFormat":"COMPACT","aggregationPeriod":0.1})");
    } else if (type == "FEED_SUBSCRIPTION") {
      server_->subscriptions.push_back(message);
    }
  }
  std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
    {
      const std::lock_guard lock(server_->mutex);
      if (!server_->replies.empty()) {
        auto reply = std::move(server_->replies.front());
        server_->replies.pop_front();
        return reply;
      }
    }
    std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds(5)));
    return std::nullopt;
  }
  void close() noexcept override {}

 private:
  std::shared_ptr<DxlinkServer> server_;
};

/// A nested SPXW chain of one call at `strike`, expiring in 2099.
std::string chain_with_call_at(int strike) {
  const auto price = std::to_string(strike);
  return R"({"data":{"items":[{"underlying-symbol":"SPX","root-symbol":"SPXW","option-chain-type":"Standard",
    "shares-per-contract":100,"expirations":[{"expiration-type":"Weekly","expiration-date":"2099-10-16",
    "settlement-type":"PM","strikes":[{"strike-price":")" + price + R"(","call":"SPXW  991016C0)" + price +
         R"(000","call-streamer-symbol":".SPXW991016C)" + price + R"("}]}]}]}})";
}

TEST(Tastytrade, ChainRefreshesKeepTheStreamAndSendDxlinkOnlyTheChange) {
  // Every hour the stream was closed, every underlying set Connecting, and DXLink
  // handshaken and subscribed again from nothing, with no quotes in between.
  std::atomic<int> chains{0};
  auto server = std::make_shared<DxlinkServer>();
  test::EventCollector sink;
  providers::TastytradeProvider::Options options;
  options.credentials = {"secret", "refresh", ""};
  options.http = [&chains] {
    auto http = std::make_unique<test::HttpStub>();
    http->respond = [&chains](std::string_view url) -> net::HttpResponse {
      if (url.ends_with("/oauth/token")) return {200, R"({"access_token":"token","expires_in":900})"};
      if (url.ends_with("/api-quote-tokens")) return {200, R"({"data":{"token":"quote","dxlink-url":"wss://example.test/feed"}})"};
      if (url.find("/instruments/equities/") != std::string_view::npos) return {200, R"({"data":{"streamer-symbol":"SPX"}})"};
      // A strike is listed from the second fetch on, in place of the first one.
      return {200, chain_with_call_at(chains++ == 0 ? 7400 : 7500)};
    };
    return http;
  };
  options.websocket = [server] { return std::make_unique<FakeDxlink>(server); };
  options.retry_wait = [](unsigned) { return std::chrono::milliseconds(1); };
  options.chain_refresh = std::chrono::milliseconds(20);
  providers::TastytradeProvider provider(options);
  provider.start({{"SPX"}}, sink);
  ASSERT_TRUE(test::recording_eventually([&] { return chains.load() >= 4; }));
  provider.stop();
  std::vector<md::ProviderStatus> statuses;
  for (const auto& event : sink.snapshot())
    if (const auto* status = std::get_if<md::ProviderStatus>(&event)) statuses.push_back(*status);
  const auto count = [&](md::FeedState state) {
    return std::count_if(statuses.begin(), statuses.end(), [state](const auto& status) { return status.state == state; });
  };
  EXPECT_EQ(count(md::FeedState::Connecting), 1);
  EXPECT_EQ(count(md::FeedState::Error), 0);
  const std::lock_guard lock(server->mutex);
  EXPECT_EQ(server->connections, 1);
  // What the stream is subscribed to follows the refreshed chain.
  std::set<std::pair<std::string, std::string>> active;
  for (const auto& message : server->subscriptions) {
    if (message.value("reset", false)) active.clear();
    for (const auto& entry : message.value("remove", Json::array()))
      active.erase({entry.value("type", std::string()), entry.value("symbol", std::string())});
    for (const auto& entry : message.value("add", Json::array()))
      active.insert({entry.value("type", std::string()), entry.value("symbol", std::string())});
  }
  std::set<std::pair<std::string, std::string>> expected{{"Quote", "SPX"}, {"Trade", "SPX"}};
  for (const auto* type : {"Quote", "Greeks", "Summary", "Trade"}) expected.insert({type, ".SPXW991016C7500"});
  EXPECT_EQ(active, expected);
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
  md::Timestamp clock = kNow;
  providers::TastytradeMapper mapper({{"SPX"}}, sink, true, [&] { return clock; });
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
  // An untimed quote among timed ones leaves the underlying Live; a minute without a
  // timed quote makes it Stale, and the next timed one Live again.
  mapper.event(missing);
  EXPECT_EQ(sink.all<md::ProviderStatus>().size(), 2u);
  clock += providers::TastytradeMapper::kUntimedGrace + md::kNanosPerSecond;
  mapper.event(missing);
  ASSERT_EQ(sink.all<md::ProviderStatus>().size(), 3u);
  EXPECT_EQ(sink.all<md::ProviderStatus>().back().state, md::FeedState::Stale);
  EXPECT_EQ(sink.all<md::ProviderStatus>().back().ts, clock);
  mapper.event(quote(".SPX261016C7400"));
  ASSERT_EQ(sink.all<md::ProviderStatus>().size(), 4u);
  EXPECT_EQ(sink.all<md::ProviderStatus>().back().state, md::FeedState::Live);
  // Untimed from the start, an underlying is Stale at once.
  test::BrokerSink untimed_sink;
  providers::TastytradeMapper untimed({{"SPX"}}, untimed_sink, true, [&] { return clock; });
  untimed.chain("SPX", "SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  untimed.event(missing);
  ASSERT_EQ(untimed_sink.all<md::ProviderStatus>().size(), 1u);
  EXPECT_EQ(untimed_sink.all<md::ProviderStatus>()[0].state, md::FeedState::Stale);
}

TEST(Tastytrade, MixedTimedAndUntimedQuotesKeepOneStatePerUnderlying) {
  // One contract's quotes carried both times, another's a priced bid without one: each
  // quote flipped the underlying Live and Stale, 1,000 status events for 1,000 quotes.
  test::BrokerSink sink;
  md::Timestamp clock = kNow;
  providers::TastytradeMapper mapper({{"SPX"}}, sink, true, [&] { return clock; });
  mapper.chain("SPX", "SPX", providers::parse_tastytrade_chain(kChain, "SPX"), {2026, 9, 24});
  auto untimed = quote(".SPXW261016C7400");
  untimed["bidTime"] = nullptr;
  for (int i = 0; i < 500; ++i) {
    mapper.event(quote(".SPX261016C7400"));
    mapper.event(untimed);
    clock += md::kNanosPerSecond / 10;
  }
  EXPECT_EQ(sink.all<md::OptionQuote>().size(), 1000u);
  ASSERT_EQ(sink.all<md::ProviderStatus>().size(), 1u);
  EXPECT_EQ(sink.all<md::ProviderStatus>()[0].state, md::FeedState::Live);
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
