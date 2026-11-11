#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>

#include "support/scripted_market.hpp"
#include "openport/trading/history.hpp"
#include "support/journal.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money dollars(std::string_view value) { return Money::parse(value); }
struct GroupDirectory {
  std::filesystem::path path;
  GroupDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-groups-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    path = pattern;
  }
  ~GroupDirectory() { std::filesystem::remove_all(path); }
  std::string file(const char* name) const { return (path / name).string(); }
};
/// Four call strikes, 10 apart: a 5000/5010 debit vertical to roll up to 5020/5030.
struct Calls {
  ScriptedMarket a, b, c, d;
  Calls() { b.contract.strike += 10; c.contract.strike += 20; d.contract.strike += 30; }
  void seed(TradingSession& s) { a.seed(s); b.seed(s, "2.00", "2.20"); c.seed(s, "1.00", "1.20"); d.seed(s, "0.40", "0.60"); }
  /// New quotes on every strike, a second later.
  void quote(TradingSession& s, std::string_view a_bid, std::string_view b_bid, std::string_view c_bid = "1.00",
             std::string_view d_bid = "0.40") {
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    for (auto [market, bid] : {std::pair{&a, a_bid}, {&b, b_bid}, {&c, c_bid}, {&d, d_bid}}) {
      market->next();
      market->time = a.time;
      const auto ask = (Money::parse(bid) + Money::parse("0.20")).str();
      quotes.push_back(market->quote(bid, ask));
      valuations.push_back(market->valuation());
    }
    s.on_quotes(quotes, valuations, a.time);
  }
  OrderRequest order(std::string client, std::vector<Leg> legs) const {
    OrderRequest r;
    r.client_order_id = std::move(client);
    r.type = OrderType::Market;
    r.tif = TimeInForce::Ioc;
    r.quantity = 1;
    r.legs = std::move(legs);
    return r;
  }
  OrderRequest open() const { return order("open", {{a.symbol(), Side::Buy, 1}, {b.symbol(), Side::Sell, 1}}); }
  OrderRequest roll() const {
    return order("roll", {{a.symbol(), Side::Sell, 1}, {b.symbol(), Side::Buy, 1}, {c.symbol(), Side::Buy, 1}, {d.symbol(), Side::Sell, 1}});
  }
};
/// The trade each round trip is in, by the contract it trades.
std::map<std::string, std::string> trades_by_symbol(const TradingSession& s) {
  std::map<std::string, std::string> out;
  for (const auto& t : lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts()))
    out[t.symbol] = trade_group(t, s.snapshot()->groups);
  return out;
}

TEST(TradeGroups, ARollJoinsTheTradeItContinuesAndTheWholeTradeKeepsItsReview) {
  Calls m;
  TradingSession s({}, m.a.time);
  m.seed(s);
  ASSERT_TRUE(s.submit(m.open(), m.a.time).decision.ok());
  m.quote(s, "2.00", "1.50");
  const auto before = s.snapshot()->strategy_reviews.at("1");
  ASSERT_TRUE(before.worst);
  const auto placed = s.submit(m.roll(), m.a.time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  // The new vertical's round trips join the trade the old one opened.
  auto trades = trades_by_symbol(s);
  EXPECT_EQ(trades.at(m.a.symbol()), "1");
  EXPECT_EQ(trades.at(m.c.symbol()), "1");
  EXPECT_EQ(trades.at(m.d.symbol()), "1");
  // The roll's opening legs keep a strategy review of their own.
  EXPECT_TRUE(s.snapshot()->strategy_reviews.contains("2"));
  // The whole trade's review starts as the old strategy's was, extremes included.
  const auto& whole = s.snapshot()->group_reviews.at("1");
  EXPECT_FALSE(whole.finished);
  EXPECT_FALSE(whole.since);
  ASSERT_TRUE(whole.worst);
  EXPECT_LE(whole.worst->pnl, before.worst->pnl);
  m.quote(s, "0.10", "0.10", "3.00", "1.00");
  EXPECT_LT(s.snapshot()->group_reviews.at("1").worst->pnl, before.worst->pnl);
  ASSERT_TRUE(s.submit(m.order("close", {{m.c.symbol(), Side::Sell, 1}, {m.d.symbol(), Side::Buy, 1}}), m.a.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->group_reviews.at("1").finished);
  EXPECT_TRUE(s.snapshot()->strategy_reviews.at("2").finished);
}

TEST(TradeGroups, AnOrderNamingATradeJoinsItAndABadNameIsRefused) {
  Calls m;
  TradingSession s({}, m.a.time);
  m.seed(s);
  ASSERT_TRUE(s.submit(m.a.market("long"), m.a.time).decision.ok());
  auto add = m.b.market("hedge", 1, Side::Sell);
  add.group = "1";
  ASSERT_TRUE(s.submit(add, m.a.time).decision.ok());
  EXPECT_EQ(trades_by_symbol(s).at(m.b.symbol()), "1");
  EXPECT_TRUE(s.snapshot()->group_reviews.contains("1"));
  for (const auto* name : {"99", "x"}) {
    auto bad = m.c.market(std::string("bad ") + name);
    bad.group = name;
    const auto refused = s.submit(bad, m.a.time);
    EXPECT_EQ(refused.decision.code, Reason::INVALID_GROUP) << name;
  }
  // A closed round trip is no longer a trade to join.
  ASSERT_TRUE(s.submit(m.b.market("unhedge"), m.a.time).decision.ok());
  auto late = m.c.market("late");
  late.group = "2";
  EXPECT_EQ(s.submit(late, m.a.time).decision.code, Reason::INVALID_GROUP);
}

TEST(TradeGroups, LeggedInRoundTripsGroupIntoOneTradeAndLeaveIt) {
  Calls m;
  TradingSession s({}, m.a.time);
  m.seed(s);
  ASSERT_TRUE(s.submit(m.a.market("long"), m.a.time).decision.ok());
  ASSERT_TRUE(s.submit(m.b.market("short", 1, Side::Sell), m.a.time).decision.ok());
  EXPECT_EQ(s.group_trades({1}, true, m.a.time).decision.code, Reason::INVALID_GROUP);
  EXPECT_EQ(s.group_trades({1, 9}, true, m.a.time).decision.code, Reason::UNKNOWN_TRADE);
  m.quote(s, "4.00", "2.00");
  ASSERT_TRUE(s.group_trades({2, 1}, true, m.a.time).decision.ok());
  EXPECT_EQ(trades_by_symbol(s).at(m.b.symbol()), "1");
  // The whole trade's review runs from the grouping.
  const auto& whole = s.snapshot()->group_reviews.at("1");
  ASSERT_TRUE(whole.since);
  EXPECT_EQ(*whole.since, m.a.time);
  ASSERT_TRUE(whole.worst);
  EXPECT_EQ(whole.worst->time, m.a.time);
  EXPECT_EQ(whole.planned_risk, dollars("220"));
  // Grouping again changes nothing.
  ASSERT_TRUE(s.group_trades({1, 2}, true, m.a.time).decision.ok());
  ASSERT_TRUE(s.group_trades({2}, false, m.a.time).decision.ok());
  EXPECT_EQ(trades_by_symbol(s).at(m.b.symbol()), "2");
  EXPECT_FALSE(s.snapshot()->group_reviews.contains("1"));
  EXPECT_TRUE(s.snapshot()->groups.empty());
  // Only open round trips change trades.
  ASSERT_TRUE(s.submit(m.b.market("cover"), m.a.time).decision.ok());
  EXPECT_EQ(s.group_trades({1, 2}, true, m.a.time).decision.code, Reason::INVALID_GROUP);
}

TEST(TradeGroups, AGroupedCalendarPlansItsCoveredDebit) {
  Calls m;
  m.a.contract = *md::parse_osi("SPXW261029C05000000");
  TradingSession s({}, m.a.time);
  m.seed(s);
  ASSERT_TRUE(s.submit(m.a.market("long"), m.a.time).decision.ok());
  ASSERT_TRUE(s.submit(m.b.market("short", 1, Side::Sell), m.a.time).decision.ok());
  ASSERT_TRUE(s.group_trades({1, 2}, true, m.a.time).decision.ok());
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("220"));
}

TEST(TradeGroups, GroupingAnUnboundedAdjustmentKeepsItsEarlierPlannedRisk) {
  for (const bool rolled : {false, true}) {
    SCOPED_TRACE(rolled);
    Calls m;
    TradingSession s({}, m.a.time);
    m.seed(s);
    ASSERT_TRUE(s.submit(m.open(), m.a.time).decision.ok());
    if (rolled) {
      ASSERT_TRUE(s.submit(m.roll(), m.a.time).decision.ok());
      EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("220"));
    }
    auto& single = rolled ? m.a : m.c;
    ASSERT_TRUE(s.submit(single.market("short", 1, Side::Sell), m.a.time).decision.ok());
    const auto id = s.snapshot()->recent_fills.back().id;
    m.quote(s, "4.00", "2.00");
    ASSERT_TRUE(s.group_trades({1, id}, true, m.a.time).decision.ok());
    const auto& whole = s.snapshot()->group_reviews.at("1");
    EXPECT_EQ(whole.planned_risk, dollars("220"));
    EXPECT_EQ(whole.since, m.a.time);
    ASSERT_TRUE(whole.worst);
    EXPECT_EQ(whole.worst->time, m.a.time);
    ASSERT_TRUE(s.submit(single.market("cover"), m.a.time).decision.ok());
    const auto close = rolled
        ? m.order("close", {{m.c.symbol(), Side::Sell, 1}, {m.d.symbol(), Side::Buy, 1}})
        : m.order("close", {{m.a.symbol(), Side::Sell, 1}, {m.b.symbol(), Side::Buy, 1}});
    ASSERT_TRUE(s.submit(close, m.a.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->group_reviews.at("1").finished);
    EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("220"));
  }
}

TEST(TradeGroups, UngroupingPlansFromTheRemainingOpenLegsThenKeepsThatRiskWithoutACover) {
  Calls m;
  TradingSession s({}, m.a.time);
  m.seed(s);
  ASSERT_TRUE(s.submit(m.open(), m.a.time).decision.ok());
  ASSERT_TRUE(s.submit(m.roll(), m.a.time).decision.ok());
  ASSERT_TRUE(s.submit(m.a.market("single"), m.a.time).decision.ok());
  const auto id = s.snapshot()->recent_fills.back().id;
  ASSERT_TRUE(s.group_trades({1, id}, true, m.a.time).decision.ok());
  // Only the current vertical and the new long plan risk; the old vertical is closed.
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("500"));
  m.quote(s, "4.00", "2.00");
  ASSERT_TRUE(s.group_trades({id}, false, m.a.time).decision.ok());
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("80"));
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").since, m.a.time);
  const auto lives = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  const auto long_leg = std::find_if(lives.begin(), lives.end(), [&](const Lifecycle& life) { return life.symbol == m.c.symbol(); });
  ASSERT_NE(long_leg, lives.end());
  m.quote(s, "4.00", "2.00");
  ASSERT_TRUE(s.group_trades({long_leg->first_fill}, false, m.a.time).decision.ok());
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("80"));
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").since, m.a.time);
  ASSERT_TRUE(s.submit(m.d.market("cover"), m.a.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->group_reviews.at("1").finished);
  EXPECT_EQ(s.snapshot()->group_reviews.at("1").planned_risk, dollars("80"));
}

TEST(TradeGroups, ARecoveredSessionGroupsARollAsTheLiveOneDoes) {
  GroupDirectory directory;
  Calls m;
  TradingSession s({}, m.a.time, test::create_journal(directory.file("journal")));
  m.seed(s);
  ASSERT_TRUE(s.submit(m.open(), m.a.time).decision.ok());
  m.quote(s, "4.50", "2.20");
  auto recovered = TradingSession::recover(FileJournal::read(directory.file("journal")));
  for (auto* session : {&s, &recovered}) ASSERT_TRUE(session->submit(m.roll(), m.a.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->groups.empty());
  EXPECT_EQ(s.snapshot_json(), recovered.snapshot_json());
  const auto again = TradingSession::recover(FileJournal::read(directory.file("journal")));
  EXPECT_EQ(again.snapshot_json(), s.snapshot_json());
}

TEST(TradeGroups, AClosedRootNamesTheSameTradeBeforeAndAfterRecovery) {
  GroupDirectory directory;
  Calls m;
  auto live_journal = test::create_journal(directory.file("live"));
  auto repeated_journal = test::create_journal(directory.file("repeated"));
  auto recovered_journal = test::create_journal(directory.file("recovered"));
  TradingSession live({}, m.a.time, live_journal);
  TradingSession repeated({}, m.a.time, repeated_journal);
  TradingSession recovered({}, m.a.time, recovered_journal);
  for (auto* s : {&live, &repeated, &recovered}) {
    m.seed(*s);
    ASSERT_TRUE(s->submit(m.open(), m.a.time).decision.ok());
    ASSERT_TRUE(s->submit(m.roll(), m.a.time).decision.ok());
  }
  recovered = TradingSession::recover(FileJournal::read(directory.file("recovered")), recovered_journal);
  // The live session uses the incremental review builder; recovery checks the
  // complete history before its first command rebuilds that cache.
  auto hedge = m.a.market("hedge");
  hedge.group = "1";
  for (auto* s : {&live, &repeated, &recovered}) {
    ASSERT_TRUE(s->submit(hedge, m.a.time).decision.ok());
    EXPECT_EQ(trades_by_symbol(*s).at(m.a.symbol()), "1");
    const auto trips = lifecycles(s->snapshot()->recent_fills, s->snapshot()->closures, s->contracts());
    ASSERT_TRUE(trips.front().closed);
    const auto hedge_id = trips.back().first_fill;
    // The closed root can name the whole trade for grouping, but cannot leave it.
    EXPECT_EQ(s->group_trades({1}, false, m.a.time).decision.code, Reason::INVALID_GROUP);
    ASSERT_TRUE(s->group_trades({hedge_id}, false, m.a.time).decision.ok());
    ASSERT_TRUE(s->group_trades({1, hedge_id}, true, m.a.time).decision.ok());
    EXPECT_EQ(trades_by_symbol(*s).at(m.a.symbol()), "1");
    ASSERT_TRUE(s->snapshot()->group_reviews.at("1").since);
    EXPECT_EQ(*s->snapshot()->group_reviews.at("1").since, m.a.time);
  }
  EXPECT_EQ(live_journal->head(), repeated_journal->head());
  EXPECT_EQ(live.snapshot_json(), recovered.snapshot_json());
  EXPECT_EQ(live_journal->sequence(), recovered_journal->sequence());
  // Recovery deliberately writes a checkpoint on the next command. Expanding
  // both histories compares every recorded state and event regardless of that.
  auto live_expanded = test::create_journal(directory.file("live-expanded"));
  auto recovered_expanded = test::create_journal(directory.file("recovered-expanded"));
  TradingSession::expand(FileJournal::read(directory.file("live")), *live_expanded);
  TradingSession::expand(FileJournal::read(directory.file("recovered")), *recovered_expanded);
  EXPECT_EQ(live_expanded->head(), recovered_expanded->head());
  const auto again = TradingSession::recover(FileJournal::read(directory.file("recovered")));
  EXPECT_EQ(again.snapshot_json(), live.snapshot_json());
}

TEST(TradeGroups, ClosedMembersOnlyNameTradesThatStillHoldTheSameUnderlying) {
  Calls m;
  TradingSession s({}, m.a.time);
  m.seed(s);
  ScriptedMarket other;
  other.contract = *md::parse_osi("SPY261022C00500000");
  other.seed(s);
  ASSERT_TRUE(s.submit(m.open(), m.a.time).decision.ok());
  ASSERT_TRUE(s.submit(m.roll(), m.a.time).decision.ok());
  auto wrong = other.market("wrong underlying");
  wrong.group = "1";
  EXPECT_EQ(s.submit(wrong, m.a.time).decision.code, Reason::INVALID_GROUP);
  ASSERT_TRUE(s.submit(other.market("other"), m.a.time).decision.ok());
  const auto other_id = s.snapshot()->recent_fills.back().id;
  EXPECT_EQ(s.group_trades({1, other_id}, true, m.a.time).decision.code, Reason::INVALID_GROUP);
  // Any closed member can identify its whole trade for manual grouping.
  ASSERT_TRUE(s.submit(m.a.market("separate"), m.a.time).decision.ok());
  const auto separate_id = s.snapshot()->recent_fills.back().id;
  ASSERT_TRUE(s.group_trades({2, separate_id}, true, m.a.time).decision.ok());
  EXPECT_EQ(trades_by_symbol(s).at(m.a.symbol()), "1");
  EXPECT_EQ(s.group_trades({2}, false, m.a.time).decision.code, Reason::INVALID_GROUP);
  ASSERT_TRUE(s.submit(m.a.market("close separate", 1, Side::Sell), m.a.time).decision.ok());
  ASSERT_TRUE(s.submit(m.order("close", {{m.c.symbol(), Side::Sell, 1}, {m.d.symbol(), Side::Buy, 1}}), m.a.time).decision.ok());
  auto late = m.a.market("late");
  late.group = "1";
  EXPECT_EQ(s.submit(late, m.a.time).decision.code, Reason::INVALID_GROUP);
  EXPECT_EQ(s.group_trades({1, other_id}, true, m.a.time).decision.code, Reason::INVALID_GROUP);
}

TEST(TradeGroups, ReturnOnBuyingPowerDividesByWhatTheEntryNeeded) {
  Calls m;
  TradingSession s({}, m.a.time);
  m.seed(s);
  // A debit vertical needs its debit: 4.20 paid less 2.00 received.
  ASSERT_TRUE(s.submit(m.open(), m.a.time).decision.ok());
  // A credit vertical needs its width less the credit.
  auto credit = m.order("credit", {{m.c.symbol(), Side::Sell, 1}, {m.d.symbol(), Side::Buy, 1}});
  ASSERT_TRUE(s.submit(credit, m.a.time).decision.ok());
  const auto lives = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  ASSERT_EQ(lives.size(), 4U);
  EXPECT_EQ(entry_buying_power({&lives[0], &lives[1]}), dollars("220"));
  // The 5020 call sold at 1.00 and the 5030 bought at 0.60: 40 received against 1,000 of width.
  EXPECT_EQ(entry_buying_power({&lives[2], &lives[3]}), dollars("960"));
  // Alone, the short call is naked.
  EXPECT_GT(entry_buying_power({&lives[2]}), dollars("960"));
  // The later entry hedges the first, even at the same market time; its earlier need stays the peak.
  EXPECT_EQ(entry_buying_power({&lives[0], &lives[1], &lives[2], &lives[3]}), dollars("180"));
  EXPECT_EQ(peak_buying_power({&lives[0], &lives[1], &lives[2], &lives[3]}), dollars("220"));
}

TEST(TradeGroups, PeakBuyingPowerCountsOpeningExecutionsEvenAtTheSameMarketTime) {
  for (const bool later : {false, true}) {
    for (const bool open_first : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "later=" << later << " open_first=" << open_first);
      Calls m;
      TradingSession s({}, m.a.time);
      m.seed(s);
      ASSERT_TRUE(s.submit(m.order("credit", {{m.a.symbol(), Side::Sell, 1}, {m.b.symbol(), Side::Buy, 1}}), m.a.time).decision.ok());
      if (later) m.quote(s, "4.00", "2.00");
      auto roll = m.order("roll", {{m.a.symbol(), Side::Buy, 1}, {m.b.symbol(), Side::Sell, 1},
                                    {m.c.symbol(), Side::Sell, 1}, {m.d.symbol(), Side::Buy, 1}});
      if (open_first) std::rotate(roll.legs.begin(), roll.legs.begin() + 2, roll.legs.end());
      ASSERT_TRUE(s.submit(roll, m.a.time).decision.ok());
      ASSERT_TRUE(s.submit(m.order("close", {{m.c.symbol(), Side::Buy, 1}, {m.d.symbol(), Side::Sell, 1}}), m.a.time).decision.ok());
      const auto lives = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
      ASSERT_EQ(lives.size(), 4U);
      for (const auto& life : lives) {
        ASSERT_TRUE(life.closed);
        EXPECT_EQ(trade_group(life, s.snapshot()->groups), "1");
      }
      const auto old_need = entry_buying_power({&lives[0], &lives[1]});
      const auto new_need = entry_buying_power({&lives[2], &lives[3]});
      EXPECT_EQ(old_need, dollars("820"));
      EXPECT_EQ(new_need, dollars("960"));
      EXPECT_EQ(peak_buying_power({&lives[0], &lives[1], &lives[2], &lives[3]}), std::max(old_need, new_need));
    }
  }
}

TEST(TradeGroups, ACondorRollsWholeInOneEightLegOrderThatOpensNoMoreThanFour) {
  const auto at = [](std::string_view osi) {
    ScriptedMarket m;
    m.contract = *md::parse_osi(osi);
    return m;
  };
  std::vector<ScriptedMarket> held{at("SPXW261022P04950000"), at("SPXW261022P04960000"), at("SPXW261022C05040000"), at("SPXW261022C05050000")};
  std::vector<ScriptedMarket> next{at("SPXW261022P04900000"), at("SPXW261022P04910000"), at("SPXW261022C05090000"), at("SPXW261022C05100000")};
  TradingSession s({}, held[0].time);
  for (const auto* group : {&held, &next})
    for (const auto& m : *group) m.seed(s, "1.00", "1.10");
  const auto legs = [](const std::vector<ScriptedMarket>& m, bool open) {
    const auto side = [&](bool buy) { return buy == open ? Side::Buy : Side::Sell; };
    return std::vector<Leg>{{m[0].symbol(), side(true), 1}, {m[1].symbol(), side(false), 1},
                            {m[2].symbol(), side(false), 1}, {m[3].symbol(), side(true), 1}};
  };
  Calls helper;
  ASSERT_TRUE(s.submit(helper.order("condor", legs(held, true)), held[0].time).decision.ok());
  // Five legs that open: refused, the opening legs counted.
  auto five = legs(next, true);
  five.push_back({held[0].symbol(), Side::Buy, 1});
  five.push_back({held[1].symbol(), Side::Buy, 1});
  five.push_back({held[2].symbol(), Side::Buy, 1});
  const auto refused = s.submit(helper.order("too many", five), held[0].time).decision;
  EXPECT_EQ(refused.code, Reason::INVALID_ORDER);
  EXPECT_EQ(refused.actual, 5);
  // Close all four and open four: one order, and one whole trade.
  auto roll = legs(held, false);
  for (const auto& leg : legs(next, true)) roll.push_back(leg);
  const auto placed = s.submit(helper.order("roll", roll), held[0].time);
  ASSERT_TRUE(placed.decision.ok()) << placed.decision.message;
  const auto trades = trades_by_symbol(s);
  for (const auto& m : next) EXPECT_EQ(trades.at(m.symbol()), "1") << m.symbol();
  EXPECT_EQ(s.snapshot()->positions.size(), 4U);
  EXPECT_TRUE(s.snapshot()->group_reviews.contains("1"));
}

}  // namespace
}  // namespace openport::trading
