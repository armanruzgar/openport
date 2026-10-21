#include "openport/providers/dxlink.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
using namespace openport;
using Json = nlohmann::json;
constexpr auto kSecond = md::kNanosPerSecond;

class FakeTransport final : public net::WebSocket {
 public:
  void connect(std::string_view, const std::atomic<bool>*) override { ++connections; }
  void send(std::string_view frame) override { frames.push_back(Json::parse(frame)); }
  std::optional<std::string> receive(std::chrono::milliseconds) override { return std::nullopt; }
  void close() noexcept override {}
  std::vector<Json> frames;
  int connections = 0;
};

void handshake(providers::DxlinkProtocol& protocol, md::Timestamp now = 0) {
  (void)protocol.on_message(R"({"type":"SETUP","channel":0,"keepaliveTimeout":60,"acceptKeepaliveTimeout":60,"version":"1.0-1.2.1"})", now);
  (void)protocol.on_message(R"({"type":"AUTH_STATE","channel":0,"state":"UNAUTHORIZED"})", now);
  (void)protocol.on_message(R"({"type":"AUTH_STATE","channel":0,"state":"AUTHORIZED"})", now);
  (void)protocol.on_message(R"({"type":"CHANNEL_OPENED","channel":3,"service":"FEED"})", now);
  (void)protocol.on_message(R"({"type":"FEED_CONFIG","channel":3,"dataFormat":"COMPACT","aggregationPeriod":0.1})", now);
}

TEST(Dxlink, SendsRecordedHandshakeInOrderAndRestoresSubscriptionsOnReconnect) {
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  const std::set<providers::DxlinkSubscription> subscriptions{{"Quote", ".SPY230731C393"}, {"Trade", "SPY"}};
  protocol.begin("fake-token", subscriptions, 0);
  handshake(protocol);
  ASSERT_EQ(transport.frames.size(), 6u);
  EXPECT_EQ(transport.frames[0]["type"], "SETUP");
  EXPECT_EQ(transport.frames[1]["type"], "AUTH");
  EXPECT_EQ(transport.frames[1]["token"], "fake-token");
  EXPECT_EQ(transport.frames[2]["type"], "CHANNEL_REQUEST");
  EXPECT_EQ(transport.frames[2]["service"], "FEED");
  EXPECT_EQ(transport.frames[3]["type"], "FEED_SETUP");
  EXPECT_EQ(transport.frames[3]["acceptDataFormat"], "COMPACT");
  EXPECT_EQ(transport.frames[4]["reset"], true);
  EXPECT_EQ(transport.frames[5]["add"].size(), 2u);
  const auto added = transport.frames[5]["add"];
  protocol.begin("new-token", subscriptions, kSecond);
  EXPECT_FALSE(protocol.ready());
  handshake(protocol, kSecond);
  EXPECT_EQ(transport.frames.back()["add"], added);
}

TEST(Dxlink, RequestsSummaryEventTimeAndTradeVolumeWithoutUnusedProfile) {
  const auto fields = providers::DxlinkProtocol::event_fields();
  EXPECT_FALSE(fields.contains("Profile"));
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  protocol.begin("token", {}, 0);
  handshake(protocol);
  const auto events = protocol.on_message(R"({"type":"FEED_DATA","channel":3,"data":[
    "Summary",["Summary",".SPY230731C393",42,1790263800000],
    "Trade",["Trade",".SPY230731C393",5,321,1,1790263800000,20720]]})", 0);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0]["eventTime"], 1790263800000LL);
  EXPECT_EQ(events[1]["dayVolume"], 321);
}

TEST(Dxlink, RecordedCompactExchangeDecodesMultipleRowsAndSpecialValues) {
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  protocol.begin("token", {}, 0);
  handshake(protocol);
  // Recorded exchange shape, with the field subset the reference requested.
  (void)protocol.on_message(R"({"type":"FEED_CONFIG","channel":3,"eventFields":{"Trade":["eventType","eventSymbol","price","dayVolume","size"]}})", 0);
  const auto events = protocol.on_message(R"({"type":"FEED_DATA","channel":3,"data":["Trade",
    ["Trade","SPY",559.36,1.3743299E7,100.0,"Trade","BTC/USD:CXTALP",58356.71,"NaN","NaN"]]})", 0);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0]["eventSymbol"], "SPY");
  EXPECT_EQ(events[0]["dayVolume"], 13743299);
  EXPECT_EQ(events[1]["size"], "NaN");
}

TEST(Dxlink, AcceptedFieldOrderOverridesRequestedOrderAndHandlesMultiplePairs) {
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  protocol.begin("token", {}, 0);
  handshake(protocol);
  (void)protocol.on_message(R"({"type":"FEED_CONFIG","channel":3,"eventFields":{
    "Quote":["askPrice","eventSymbol","bidPrice"],"Summary":["openInterest","eventSymbol"]}})", 0);
  const auto events = protocol.on_message(R"({"type":"FEED_DATA","channel":3,"data":[
    "Quote",[5.2,".SPY230731C393",5.0,3.0,".SPY230731P393",2.8],"Summary",[42,".SPY230731C393"]]})", 0);
  ASSERT_EQ(events.size(), 3u);
  EXPECT_EQ(events[0]["bidPrice"], 5.0);
  EXPECT_EQ(events[1]["eventSymbol"], ".SPY230731P393");
  EXPECT_EQ(events[2]["openInterest"], 42);
  EXPECT_EQ(events[2]["eventType"], "Summary");
}

TEST(Dxlink, RejectsMalformedRowsUnsupportedFormatsAndServerErrorsWithoutEchoingSecrets) {
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  protocol.begin("token", {}, 0);
  handshake(protocol);
  for (const auto* message : {
      R"({"type":"FEED_DATA","channel":3,"data":["Quote"]})",
      R"({"type":"FEED_DATA","channel":3,"data":["Quote",[1]]})",
      R"({"type":"FEED_DATA","channel":3,"data":["Unknown",[1]]})",
      R"({"type":"FEED_CONFIG","channel":3,"eventFields":{"Quote":[]}})",
      R"({"type":"FEED_CONFIG","channel":3,"dataFormat":"FULL"})",
      R"({"type":"ERROR","channel":0,"message":"secret"})",
      R"({"type":"AUTH_STATE","channel":0,"state":"UNAUTHORIZED"})"}) {
    try { (void)protocol.on_message(message, 0); FAIL(); }
    catch (const std::exception& error) { EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos); }
  }
}

TEST(Dxlink, FakeClockDrivesKeepalivesReceiveTimeoutAndHandshakeTimeout) {
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  protocol.begin("token", {}, 0);
  handshake(protocol);
  auto count = transport.frames.size();
  protocol.tick(29 * kSecond);
  EXPECT_EQ(transport.frames.size(), count);
  protocol.tick(30 * kSecond);
  EXPECT_EQ(transport.frames.back()["type"], "KEEPALIVE");
  EXPECT_THROW(protocol.tick(60 * kSecond), std::runtime_error);
  protocol.begin("token", {}, 0);
  EXPECT_THROW(protocol.tick(15 * kSecond), std::runtime_error);
  protocol.begin("token", {}, 0);
  handshake(protocol);
  (void)protocol.on_message(R"({"type":"SETUP","channel":0,"keepaliveTimeout":10})", 0);
  protocol.tick(5 * kSecond);
  EXPECT_EQ(transport.frames.back()["type"], "KEEPALIVE");
}

TEST(Dxlink, SubscriptionChangesRemoveOldSymbolsAndEnforceSessionAndMinuteCaps) {
  FakeTransport transport;
  providers::DxlinkProtocol protocol(transport);
  protocol.begin("token", {{"Quote", "SPY"}}, 0);
  handshake(protocol);
  protocol.replace({{"Quote", "SPX"}}, 0);
  EXPECT_EQ(transport.frames.back()["remove"][0]["symbol"], "SPY");
  EXPECT_EQ(transport.frames.back()["add"][0]["symbol"], "SPX");
  std::set<providers::DxlinkSubscription> many;
  for (int i = 0; i < 25001; ++i) many.insert({"Quote", "generated-" + std::to_string(i)});
  EXPECT_THROW(protocol.replace(many, 0), std::runtime_error);
  many.erase(many.begin());
  // A new protocol starts with an empty rolling change budget.
  providers::DxlinkProtocol limited(transport);
  limited.begin("token", many, 0);
  handshake(limited);
  EXPECT_EQ(transport.frames.back()["add"].size(), 10000u);
  const auto count = transport.frames.size();
  limited.tick(1 * kSecond);
  EXPECT_EQ(transport.frames.size(), count);
  (void)limited.on_message(R"({"type":"KEEPALIVE","channel":0})", 60 * kSecond);
  limited.tick(60 * kSecond);
  EXPECT_EQ(transport.frames.back()["add"].size(), 10000u);
  // Reconnecting in the same minute does not reset the rate cap.
  limited.begin("token", many, 61 * kSecond);
  handshake(limited, 61 * kSecond);
  EXPECT_EQ(transport.frames.back()["reset"], true);
}

TEST(Dxlink, ReconnectBackoffIsBounded) {
  EXPECT_EQ(providers::DxlinkProtocol::backoff(0).count(), 1);
  EXPECT_EQ(providers::DxlinkProtocol::backoff(3).count(), 8);
  EXPECT_EQ(providers::DxlinkProtocol::backoff(100).count(), 60);
}
}  // namespace
