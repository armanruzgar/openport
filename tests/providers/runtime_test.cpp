#include <gtest/gtest.h>

#include <cmath>
#include <thread>

#include "../../apps/probe/state.hpp"
#include "openport/md/event_queue.hpp"
#include "openport/providers/factory.hpp"
#include "openport/providers/massive.hpp"
#include "openport/providers/options.hpp"
#include "openport/providers/snapshot.hpp"
#include "openport/providers/thetadata.hpp"
#include "support/http_stub.hpp"

namespace {
using namespace openport;

class Collector : public md::EventSink {
 public:
  void publish(md::Event event) override { events.push_back(std::move(event)); }
  std::vector<md::Event> events;
};

TEST(SnapshotPublisher, EveryGreekFieldParticipatesInDeduplicationIncludingNan) {
  providers::SnapshotPublisher publisher;
  Collector sink;
  publisher.define("SPY", {}, sink);
  md::VendorGreeks greeks{0, 1, .2, .5, .01, 1, -1, .1};
  publisher.greeks(greeks, sink);
  sink.events.clear();
  for (auto field : {&md::VendorGreeks::iv, &md::VendorGreeks::delta, &md::VendorGreeks::gamma,
                     &md::VendorGreeks::vega, &md::VendorGreeks::theta, &md::VendorGreeks::rho}) {
    greeks.*field += .1;
    publisher.greeks(greeks, sink);
    ASSERT_EQ(sink.events.size(), 1u);
    sink.events.clear();
    ++greeks.ts;
    publisher.greeks(greeks, sink);
    EXPECT_TRUE(sink.events.empty());
    greeks.*field = std::nan("");
    publisher.greeks(greeks, sink);
    ASSERT_EQ(sink.events.size(), 1u);
    sink.events.clear();
    publisher.greeks(greeks, sink);
    EXPECT_TRUE(sink.events.empty());
  }
}

TEST(PollingProvider, CancellationStopsMassivePaginationBeforeTheNextPage) {
  providers::MassiveProvider provider({.api_key = "test"});
  test::HttpStub http;
  Collector sink;
  http.respond = [&](auto) {
    provider.stop();
    return net::HttpResponse{200, R"({"results":[],"next_url":"https://api.massive.com/v3/snapshot/options/SPY?cursor=2"})"};
  };
  provider.poll_once(http, "SPY", {{"SPY"}}, sink);
  EXPECT_EQ(http.urls.size(), 1u);
  EXPECT_TRUE(sink.events.empty());
}

TEST(PollingProvider, CancellationStopsThetaEndpointAndOpenInterestSequences) {
  for (int stop_on = 1; stop_on <= 3; ++stop_on) {
    providers::ThetaDataProvider provider;
    test::HttpStub http;
    Collector sink;
    http.respond = [&](auto) {
      if (static_cast<int>(http.urls.size()) == stop_on) provider.stop();
      return net::HttpResponse{200, ""};
    };
    provider.poll_once(http, "SPX", {{"SPX"}}, sink);
    EXPECT_EQ(http.urls.size(), static_cast<std::size_t>(stop_on));
    EXPECT_TRUE(sink.events.empty());
  }
}

TEST(ProviderOptions, StrictNumericParsingAndRanges) {
  for (const auto* invalid : {"", "2x", " 2", "2 ", "1.5", "9999999999999999999", "-1"})
    EXPECT_THROW(providers::parse_integer(invalid, "expiries"), std::invalid_argument) << invalid;
  EXPECT_EQ(providers::parse_integer("0", "expiries"), 0);
  EXPECT_EQ(providers::parse_integer("65535", "port", 1, 65535), 65535);
  for (const auto* invalid : {"0", "65536", "-1", "80junk"})
    EXPECT_THROW(providers::parse_integer(invalid, "port", 1, 65535), std::invalid_argument);
  for (const auto* invalid : {"", "nan", "inf", "0.1x", " 0.1", "0.1 ", "-0.1", "1.1", "1e999"})
    EXPECT_THROW(providers::parse_fraction(invalid, "window"), std::invalid_argument) << invalid;
  EXPECT_DOUBLE_EQ(providers::parse_fraction("0", "window"), 0);
  EXPECT_DOUBLE_EQ(providers::parse_fraction("1", "window"), 1);
  EXPECT_DOUBLE_EQ(providers::parse_fraction("5e-2", "window"), .05);
}

TEST(ProviderOptions, RejectsUnknownKeysAndInvalidIntervalsForEveryPollingProvider) {
  for (const auto* name : {"cboe", "massive", "thetadata"}) {
    md::ProviderConfig config{name, "test", {{"typo", "1"}}};
    EXPECT_THROW((void)providers::make_provider(config), std::invalid_argument);
    for (const auto* interval : {"0", "-1", "1x", "1.5", "9999999999999999999"}) {
      config.options = {{"poll_seconds", interval}};
      EXPECT_THROW((void)providers::make_provider(config), std::invalid_argument);
    }
    config.options = {{"poll_seconds", "1"}};
    EXPECT_EQ(providers::make_provider(config)->capabilities().poll_interval,
              std::chrono::seconds(1));
  }
}

TEST(ProviderOptions, DatabentoRejectsFiltersThatCannotLimitUpstreamTraffic) {
  EXPECT_NO_THROW(providers::validate_subscription("databento", {{"SPX"}, 0, 0}));
  EXPECT_THROW(providers::validate_subscription("databento", {{"SPX"}, 1, 0}),
               std::invalid_argument);
  EXPECT_THROW(providers::validate_subscription("databento", {{"SPX"}, 0, .1}),
               std::invalid_argument);
#ifdef OPENPORT_WITH_DATABENTO
  EXPECT_THROW((void)providers::make_provider({"databento", "test", {{"trades", "yes"}}}),
               std::invalid_argument);
#endif
}

TEST(Probe, PollingRequiresOneCompleteSnapshotForEveryRequestedUnderlying) {
  probe::Readiness readiness({{"SPX", "SPY"}}, false, 2);
  readiness.apply(md::ProviderStatus{1, md::FeedState::Live, "connected", ""});
  EXPECT_FALSE(readiness.all_ready());
  readiness.apply(md::ProviderStatus{1, md::FeedState::Live, "complete", "SPX"});
  readiness.apply(md::ProviderStatus{2, md::FeedState::Live, "complete again", "SPX"});
  EXPECT_FALSE(readiness.all_ready());
  readiness.apply(md::ProviderStatus{3, md::FeedState::Error, "failed", "SPY"});
  EXPECT_FALSE(readiness.all_ready());
  readiness.apply(md::ProviderStatus{4, md::FeedState::Delayed, "complete", "SPY"});
  EXPECT_TRUE(readiness.all_ready());
}

TEST(Probe, StreamingRequiresDefinitionsAndQuotesPerUnderlying) {
  probe::Readiness readiness({{"SPX", "SPY"}}, true, 2);
  readiness.apply(md::ProviderStatus{1, md::FeedState::Live, "connected", ""});
  readiness.apply(md::OptionQuote{0, 1});  // undefined, cannot establish readiness
  readiness.apply(md::ContractDefinition{0, *md::parse_osi("SPX991218C00500000")});
  readiness.apply(md::ContractDefinition{1, *md::parse_osi("SPY991218C00500000")});
  readiness.apply(md::OptionQuote{0, 2});
  EXPECT_FALSE(readiness.ready("SPX"));
  readiness.apply(md::OptionQuote{0, 3});
  EXPECT_TRUE(readiness.ready("SPX"));
  EXPECT_FALSE(readiness.all_ready());
  readiness.apply(md::OptionQuote{1, 3});
  readiness.apply(md::OptionQuote{1, 4});
  EXPECT_TRUE(readiness.all_ready());
}

TEST(Probe, GroupsSameDateAmAndPmContractsSeparately) {
  auto am = *md::parse_osi("SPX991218C00500000");
  auto pm = *md::parse_osi("SPXW991218C00500000");
  EXPECT_NE(probe::expiry_key(am), probe::expiry_key(pm));
  EXPECT_STREQ(probe::settlement_label(am.settlement), "AM");
  EXPECT_STREQ(probe::settlement_label(pm.settlement), "PM");
}
}  // namespace
