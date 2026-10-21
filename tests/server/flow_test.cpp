#include "openport/server/flow.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "openport/server/api.hpp"
#include "openport/server/desk.hpp"

namespace {

using namespace openport;
using nlohmann::json;

// Simulated test inputs throughout: hand-picked quotes and prints, not market data.
const md::Timestamp kOpen = md::new_york_to_utc({2026, 10, 21}, 9, 30);

analytics::OptionState option(const char* osi, double bid, double ask) {
  analytics::OptionState state;
  state.contract = *md::parse_osi(osi);
  state.has_quote = bid > 0 || ask > 0;
  state.bid = bid;
  state.ask = ask;
  state.quote_ts = kOpen - md::kNanosPerSecond;
  return state;
}

md::OptionTrade print(md::Timestamp time, double price, double size, std::uint16_t venue = 0) {
  return md::OptionTrade{0, time, price, size, venue};
}

TEST(FlowTape, ClassifiesEachPrintAgainstTheStandingQuote) {
  server::FlowTape tape;
  const auto call = option("SPXW  261021C05800000", 4.10, 4.30);
  tape.add(print(kOpen, 4.30, 1), call);
  tape.add(print(kOpen + 1, 4.10, 1), call);
  tape.add(print(kOpen + 2, 4.20, 1), call);
  tape.add(print(kOpen + 3, 4.50, 1), call);
  tape.add(print(kOpen + 4, 3.90, 1), call);
  tape.add(print(kOpen + 5, 4.20, 1), option("SPXW  261021C05800000", 0, 4.30));  // one-sided
  tape.add(print(kOpen + 6, 0, 1), call);    // no price: ignored
  tape.add(print(kOpen + 7, 4.2, -1), call);  // no size: ignored
  const auto snapshot = tape.snapshot("SPX");
  ASSERT_EQ(snapshot.prints.size(), 6u);
  const std::vector<std::pair<server::PrintSide, int>> expected{
      {server::PrintSide::Ask, 1}, {server::PrintSide::Bid, -1}, {server::PrintSide::Mid, 0},
      {server::PrintSide::Outside, 1}, {server::PrintSide::Outside, -1}, {server::PrintSide::Unquoted, 0}};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(snapshot.prints[i].side, expected[i].first) << i;
    EXPECT_EQ(snapshot.prints[i].direction, expected[i].second) << i;
    EXPECT_EQ(snapshot.prints[i].sequence, i + 1);
  }
  EXPECT_EQ(snapshot.prints[0].expiry, "2026-10-21PM");
  EXPECT_EQ(snapshot.prints[0].underlying, "SPX");
  EXPECT_DOUBLE_EQ(snapshot.prints[0].premium(), 430);
  EXPECT_TRUE(std::isnan(snapshot.prints[5].bid));
  EXPECT_FALSE(snapshot.simulated);
  EXPECT_TRUE(tape.snapshot("SPY").prints.empty());
}

TEST(FlowTape, AQuoteStampedAfterThePrintLeavesItUnquoted) {
  server::FlowTape tape;
  auto call = option("SPXW  261021C05800000", 4.10, 4.30);
  call.quote_ts = kOpen + 5;
  tape.add(print(kOpen + 4, 4.30, 1), call);  // the queue coalesced a later quote ahead of it
  tape.add(print(kOpen + 5, 4.30, 1), call);  // stamped with the quote: classified
  const auto prints = tape.snapshot("SPX").prints;
  ASSERT_EQ(prints.size(), 2u);
  EXPECT_EQ(prints[0].side, server::PrintSide::Unquoted);
  EXPECT_EQ(prints[0].direction, 0);
  EXPECT_TRUE(std::isnan(prints[0].bid) && std::isnan(prints[0].ask));
  EXPECT_EQ(prints[0].quote_time, 0);
  EXPECT_EQ(prints[1].side, server::PrintSide::Ask);
  EXPECT_EQ(prints[1].quote_time, kOpen + 5);
}

TEST(FlowTape, SweepsNeedOneContractOneDirectionAndTwoVenuesWithinASecond) {
  server::FlowTape tape;
  const auto call = option("SPY   261021C00580000", 2.00, 2.05);
  const auto put = option("SPY   261021P00570000", 1.50, 1.55);
  tape.add(print(kOpen, 2.05, 10, 1), call);
  tape.add(print(kOpen + 300'000'000, 2.05, 10, 1), call);  // same venue: not yet a sweep
  EXPECT_FALSE(tape.snapshot("SPY").prints[1].sweep);
  tape.add(print(kOpen + 600'000'000, 2.05, 10, 2), call);  // a second venue
  tape.add(print(kOpen + 700'000'000, 2.00, 10, 3), call);  // the other direction
  tape.add(print(kOpen + 3 * md::kNanosPerSecond, 2.05, 10, 4), call);  // too late
  tape.add(print(kOpen + 3 * md::kNanosPerSecond + 1, 1.55, 10, 5), put);  // another contract
  tape.add(print(kOpen + 5 * md::kNanosPerSecond, 2.05, 10, 0), call);  // venue unknown
  const auto prints = tape.snapshot("SPY").prints;
  ASSERT_EQ(prints.size(), 7u);
  EXPECT_TRUE(prints[0].sweep);
  EXPECT_TRUE(prints[1].sweep);
  EXPECT_TRUE(prints[2].sweep);
  EXPECT_FALSE(prints[3].sweep);
  EXPECT_FALSE(prints[4].sweep);
  EXPECT_FALSE(prints[5].sweep);
  EXPECT_FALSE(prints[6].sweep);
}

TEST(FlowTape, SweepsSurviveEvictionAndAFullTape) {
  server::FlowTape::Options options;
  options.capacity = 2;
  server::FlowTape small(options);
  const auto call = option("SPY   261021C00580000", 2.00, 2.05);
  small.add(print(kOpen, 2.05, 10, 1), call);
  small.add(print(kOpen + 1, 2.05, 10, 1), call);
  small.add(print(kOpen + 2, 2.05, 10, 2), call);  // evicts the first; the other two sweep
  auto prints = small.snapshot("SPY").prints;
  ASSERT_EQ(prints.size(), 2u);
  EXPECT_TRUE(prints[0].sweep && prints[1].sweep);
  small.add(print(kOpen + 3, 2.05, 10, 3), call);
  small.add(print(kOpen + 4, 2.05, 10, 3), call);
  prints = small.snapshot("SPY").prints;
  EXPECT_TRUE(prints[0].sweep && prints[1].sweep);

  // A full tape of one contract, every print a tenth of a second apart: each
  // print looks back only over its window, not the whole tape.
  server::FlowTape tape;
  for (int i = 0; i < 20'000; ++i)
    tape.add(print(kOpen + i * md::kNanosPerSecond / 10, 2.05, 1, static_cast<std::uint16_t>(1 + (i / 30) % 2 * (i % 2))), call);
  const auto full = tape.snapshot("SPY");
  ASSERT_EQ(full.prints.size(), 5000u);
  EXPECT_EQ(full.evicted, 15'000u);
  // Runs of 30 alternate between one venue and two: only the two-venue runs sweep.
  for (std::size_t i = 0; i < full.prints.size(); ++i) {
    const auto n = 15'000 + i;
    const bool two_venues = (n / 30) % 2 == 1;
    const bool edge = n % 30 < 10 || n % 30 >= 20;  // within a second of a neighbouring run
    if (!edge) EXPECT_EQ(full.prints[i].sweep, two_venues) << n;
  }
}

TEST(FlowTape, BlocksMultiLegGroupsAndCapacity) {
  server::FlowTape::Options options;
  options.capacity = 4;
  options.block_size = 50;
  server::FlowTape tape(options);
  const auto lower = option("QQQ   261121P00480000", 3.00, 3.10);
  const auto upper = option("QQQ   261121P00470000", 2.00, 2.10);
  tape.add(print(kOpen, 3.10, 20, 7), lower);
  tape.add(print(kOpen + 200'000, 2.00, 40, 7), upper);  // 1:2, same venue, within a millisecond
  tape.add(print(kOpen + 400'000, 2.00, 30, 7), upper);  // 2:3 against the first: not whole
  tape.add(print(kOpen + 10 * md::kNanosPerSecond, 3.10, 50, 8), lower);
  auto prints = tape.snapshot("QQQ").prints;
  ASSERT_EQ(prints.size(), 4u);
  EXPECT_NE(prints[0].group, 0u);
  EXPECT_EQ(prints[0].group, prints[1].group);
  EXPECT_EQ(prints[2].group, 0u);
  EXPECT_FALSE(prints[2].block);
  EXPECT_TRUE(prints[3].block);
  EXPECT_EQ(prints[3].group, 0u);
  tape.add(print(kOpen + 11 * md::kNanosPerSecond, 3.05, 1, 8), lower);
  const auto snapshot = tape.snapshot("QQQ");
  ASSERT_EQ(snapshot.prints.size(), 4u);
  EXPECT_EQ(snapshot.prints.front().sequence, 2u);
  EXPECT_EQ(snapshot.total, 5u);
  EXPECT_EQ(snapshot.evicted, 1u);
}

TEST(FlowTape, SimulatedPrintsSitOnTheQuoteAndRepeat) {
  const auto call = option("SPY   261021C00580000", 2.00, 2.06);
  int bids = 0, asks = 0, mids = 0;
  for (int i = 0; i < 200; ++i) {
    const md::OptionVolume volume{static_cast<md::InstrumentId>(i % 7), kOpen + i * 15 * md::kNanosPerSecond, 100};
    const auto first = server::simulated_print(volume, call, 3);
    const auto again = server::simulated_print(volume, call, 3);
    ASSERT_TRUE(first && again);
    EXPECT_EQ(first->price, again->price);
    EXPECT_EQ(first->size, 3);
    EXPECT_EQ(first->ts, volume.ts);
    EXPECT_EQ(first->venue, 0);
    (first->price == 2.00 ? bids : first->price == 2.06 ? asks : mids)++;
    if (first->price != 2.00 && first->price != 2.06) EXPECT_DOUBLE_EQ(first->price, 2.03);
  }
  EXPECT_GT(bids, 0);
  EXPECT_GT(asks, 0);
  EXPECT_GT(mids, 0);
  EXPECT_FALSE(server::simulated_print({0, kOpen, 1}, option("SPY   261021C00580000", 0, 2.06), 1));
  EXPECT_FALSE(server::simulated_print({0, kOpen, 1}, call, 0));
}

md::Event define(md::InstrumentId id, const char* osi) { return md::ContractDefinition{id, *md::parse_osi(osi)}; }

TEST(FlowDesk, FeedTradesReachTheTapeAndOnlyTheDemoSimulatesThem) {
  const auto run = [](const std::string& provider, bool trades) {
    server::Desk::Options options;
    options.paper_enabled = false;
    options.flow = std::make_shared<server::FlowTape>();
    md::Capabilities capabilities;
    capabilities.trades = trades;
    server::Desk desk(provider, capabilities, {{"SPY"}}, options);
    desk.observe(define(0, "SPY   261021C00580000"));
    desk.observe(md::OptionQuote{0, kOpen, 2.00, 2.06, 10, 10});
    desk.observe(md::OptionVolume{0, kOpen, 10});
    desk.observe(md::OptionTrade{0, kOpen + 1, 2.06, 4, 3});
    desk.observe(md::OptionVolume{0, kOpen + 2, 14});
    desk.observe(md::OptionQuote{0, kOpen + 3, 1.90, 1.96, 10, 10});
    desk.observe(md::OptionTrade{0, kOpen + 4, 1.96, 1, 3});
    return options.flow->snapshot("SPY");
  };
  const auto feed = run("databento", true);
  ASSERT_EQ(feed.prints.size(), 2u);
  EXPECT_EQ(feed.prints[0].side, server::PrintSide::Ask);
  // Each print meets the newest quote applied before it.
  EXPECT_EQ(feed.prints[1].side, server::PrintSide::Ask);
  EXPECT_DOUBLE_EQ(feed.prints[1].ask, 1.96);
  EXPECT_FALSE(feed.simulated);

  const auto cboe = run("cboe", false);
  EXPECT_EQ(cboe.prints.size(), 2u);  // only what was fed: volume alone never becomes a print
  EXPECT_FALSE(cboe.simulated);

  const auto demo = run("demo", false);
  ASSERT_EQ(demo.prints.size(), 3u);  // the volume rise, plus the trades a test fed it
  EXPECT_TRUE(demo.prints[1].simulated);
  EXPECT_DOUBLE_EQ(demo.prints[1].size, 4);
  EXPECT_EQ(demo.prints[1].group, 0u);  // simulated prints are never grouped into spreads
  EXPECT_TRUE(demo.simulated);
}

class FlowSource final : public server::MetricsSource {
 public:
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return nullptr; }
  server::EngineStatus status() const override { return status_; }
  const server::FlowTape* flow() const override { return tape.get(); }
  server::EngineStatus status_;
  std::shared_ptr<server::FlowTape> tape = std::make_shared<server::FlowTape>();
};

json get(const server::MetricsSource& source, const std::string& target, int expected = 200) {
  const auto response = server::handle_api({"GET", target}, source);
  EXPECT_EQ(response.status, expected) << target << " -> " << response.body;
  return json::parse(response.body);
}

TEST(FlowApi, SaysPlainlyWhenTheFeedHasNoTrades) {
  FlowSource source;
  source.status_.provider = "cboe";
  auto body = get(source, "/api/underlyings/SPX/flow");
  EXPECT_EQ(body["source"], "none");
  EXPECT_EQ(body["trades"], false);
  EXPECT_EQ(body["message"], "cboe does not deliver option trades, so there is no time and sales or flow for SPX.");
  EXPECT_TRUE(body["prints"].empty());
  EXPECT_TRUE(body["as_of"].is_null());

  source.status_.provider = "databento";
  source.status_.capabilities.trades = true;
  body = get(source, "/api/underlyings/SPX/flow");
  EXPECT_EQ(body["source"], "feed");
  EXPECT_EQ(body["message"], "No option trades for SPX have arrived from databento yet.");

  source.status_ = {};
  source.status_.provider = "replay (demo)";
  body = get(source, "/api/underlyings/SPX/flow");
  EXPECT_EQ(body["source"], "simulated");
  EXPECT_NE(body["message"].get<std::string>().find("not market trades"), std::string::npos);
  get(source, "/api/underlyings/SPY/flow", 404);
}

TEST(FlowApi, NetsPremiumByExpiryAndStrikeOverTheFilteredTape) {
  FlowSource source;
  source.status_.provider = "databento";
  source.status_.capabilities.trades = true;
  const auto call = option("SPXW  261021C05800000", 4.10, 4.30);
  const auto put = option("SPXW  261021P05700000", 3.00, 3.20);
  const auto later = option("SPXW  261023C05800000", 9.00, 9.40);
  source.tape->add(print(kOpen, 4.30, 10, 1), call);        // +4,300
  source.tape->add(print(kOpen + 1, 4.10, 2, 1), call);     // -820
  source.tape->add(print(kOpen + 2, 3.00, 5, 1), put);      // -1,500
  source.tape->add(print(kOpen + 3, 9.20, 1, 1), later);    // mid: unclassified 920
  source.tape->add(print(kOpen + 4, 9.40, 200, 2), later);  // block +188,000
  auto body = get(source, "/api/underlyings/SPX/flow");
  EXPECT_EQ(body["source"], "feed");
  EXPECT_TRUE(body["message"].is_null());
  EXPECT_EQ(body["retained"], 5);
  EXPECT_EQ(body["matched"], 5);
  EXPECT_EQ(body["totals"]["call_net"], 191480.0);
  EXPECT_EQ(body["totals"]["put_net"], -1500.0);
  EXPECT_EQ(body["totals"]["net"], 189980.0);
  EXPECT_EQ(body["totals"]["unclassified"], 920.0);
  ASSERT_EQ(body["expiries"].size(), 2u);
  EXPECT_EQ(body["expiries"][0]["expiry"], "2026-10-21PM");
  EXPECT_EQ(body["expiries"][0]["call_net"], 3480.0);
  EXPECT_EQ(body["expiries"][0]["put_net"], -1500.0);
  ASSERT_EQ(body["strikes"].size(), 3u);
  EXPECT_EQ(body["strikes"][0]["strike"], 5700.0);
  EXPECT_EQ(body["strikes"][0]["put_net"], -1500.0);
  ASSERT_EQ(body["prints"].size(), 5u);
  EXPECT_EQ(body["prints"][0]["sequence"], 5);  // newest first
  EXPECT_EQ(body["prints"][0]["block"], true);
  EXPECT_EQ(body["prints"][0]["aggressor"], "buy");
  EXPECT_EQ(body["prints"][1]["side"], "mid");
  EXPECT_TRUE(body["prints"][1]["aggressor"].is_null());

  body = get(source, "/api/underlyings/SPX/flow?type=put");
  EXPECT_EQ(body["matched"], 1);
  EXPECT_EQ(body["totals"]["net"], -1500.0);
  body = get(source, "/api/underlyings/SPX/flow?side=bid,ask&expiry=2026-10-21PM&limit=1");
  EXPECT_EQ(body["matched"], 3);
  EXPECT_EQ(body["prints"].size(), 1u);
  body = get(source, "/api/underlyings/SPX/flow?only=block&min_premium=1000");
  EXPECT_EQ(body["matched"], 1);
  body = get(source, "/api/underlyings/SPX/flow?min_size=6");
  EXPECT_EQ(body["matched"], 2);
  for (const auto* bad : {"limit=0", "limit=1001", "type=both", "side=lifted", "side=", "min_size=-1",
                          "min_premium=x", "only=spread", "since=1"})
    get(source, std::string("/api/underlyings/SPX/flow?") + bad, 400);
}

}  // namespace
