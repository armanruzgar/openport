#include <gtest/gtest.h>

#include <sstream>

#include "openport/trading/history.hpp"
#include "openport/trading/session.hpp"
#include "support/recording.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e12, 1e12};
  c.limits.per_underlying = {1e12, 1e12};
  c.limits.max_daily_loss = m("1000000");
  c.rules = std::move(rules);
  return c;
}
const md::Date kToday{2026, 9, 22}, kExDate{2026, 9, 23};
Timestamp at(md::Date day, int hour = 10) { return md::new_york_to_utc(day, hour, 0); }
/// SPY options with SPY's price in every batch.
struct Spy {
  Timestamp time = at(kToday);
  std::uint64_t observation = 0;
  double spot = 510;
  void quote(TradingSession& s, const md::OptionContract& c, std::string_view bid, std::string_view ask) {
    ++observation;
    s.on_quotes({{c.osi_symbol(), observation, time, m(bid), m(ask), 10, 10}},
                {{c.osi_symbol(), time, 0.6, 0.02, 0.5, -0.05, spot, spot, 0.99, md::years_between(time, c.expiry_time()), 0.2, true}},
                time, {{"SPY", time, Money::from_double(spot)}});
  }
  OrderRequest market(std::string client, const md::OptionContract& c, Quantity quantity, Side side) const {
    return {std::move(client), c.osi_symbol(), side, OrderType::Market, TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  }
};
const MarkedStock* stock(const TradingSession& s) {
  for (const auto& p : s.snapshot()->stocks) if (p.position.symbol == "SPY") return &p;
  return nullptr;
}

TEST(CorporateActions, ParsesSplitLinesAndRefusesTheRest) {
  std::istringstream in("symbol,ex_date,kind,terms\n# a comment\nSPY,2026-09-23,split,2:1\n\nQQQ,2026-09-24,split,1:10\n");
  const auto actions = parse_corporate_actions(in);
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions[0], (CorporateAction{"SPY", kExDate, 2, 1}));
  EXPECT_EQ(actions[1].denominator, 10);
  for (const auto* bad : {"SPY,2026-09-23,dividend,2:1", "SPY,2026-09-23,split,2", "SPY,2026-9-23,split,2:1", "spy,2026-09-23,split,2:1",
                          "SPY,2026-09-23,split,1:1", "SPY,2026-09-23,split,0:3", "SPY,2026-09-23,split,2:1,extra"}) {
    std::istringstream line(bad);
    EXPECT_THROW((void)parse_corporate_actions(line), std::invalid_argument) << bad;
  }
  std::istringstream twice("SPY,2026-09-23,split,2:1\nSPY,2026-09-23,split,3:1\n");
  EXPECT_THROW((void)parse_corporate_actions(twice), std::invalid_argument);
  EXPECT_EQ(corporate_actions_due(actions, kToday, kExDate).size(), 1u);
  EXPECT_TRUE(corporate_actions_due(actions, kExDate, kExDate).empty());
}

TEST(CorporateActions, AForwardSplitKeepsACoveredCallCoveredAndDeliversTheSplitShares) {
  test::RecordingFile file;
  const auto path = (file.directory / "split.jsonl").string();
  const auto call = *md::parse_osi("SPY261016C00500000");
  Spy f;
  TradingSession s(roomy(), f.time, FileJournal::create(path));
  ASSERT_TRUE(s.define(call, f.time).decision.ok());
  f.quote(s, call, "14.00", "14.20");
  ASSERT_TRUE(s.trade_stock("SPY", 150, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("covered", call, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
  const auto equity = s.snapshot()->equity;

  ASSERT_TRUE(s.roll_day(at(kExDate, 9), {}, {{"SPY", kExDate, 2, 1}}).decision.ok());
  const auto& snapshot = *s.snapshot();
  const auto* held = stock(s);
  ASSERT_NE(held, nullptr);
  EXPECT_EQ(held->position.shares, 300);
  EXPECT_EQ(held->position.basis, m("76500"));  // the basis stays with the shares
  EXPECT_EQ(held->mark, m("255"));
  EXPECT_EQ(snapshot.equity, equity);           // a split moves no value
  ASSERT_EQ(snapshot.corporate_actions.size(), 1u);
  EXPECT_EQ(snapshot.corporate_actions[0].shares_after, 300);
  EXPECT_EQ(snapshot.corporate_actions[0].cash_in_lieu, Money{});
  EXPECT_EQ(snapshot.deliverables.at(call.osi_symbol()), (Deliverable{200, {}}));
  EXPECT_EQ(snapshot.stock_fills.back().source, StockSource::Split);
  EXPECT_EQ(snapshot.stock_fills.back().shares, 150);
  // The call now delivers 200 shares, which the 300 held still cover.
  EXPECT_EQ(snapshot.buying_power.short_requirement, Money{});
  // Applied once, whatever is passed again.
  f.time = at(kExDate);
  f.spot = 255;
  // The adjusted contract is worth what it was: 200 shares at $255 are 100 at $510.
  f.quote(s, call, "14.00", "14.20");
  const auto rolled = s.roll_day(at({2026, 9, 24}, 9), {}, {{"SPY", kExDate, 2, 1}});
  ASSERT_TRUE(rolled.decision.ok()) << rolled.decision.message;
  EXPECT_EQ(stock(s)->position.shares, 300);

  // At expiry $260 a split share is $520 a pre-split one: the call is assigned for 200
  // shares at the settlement, together the $50,000 its strike was worth.
  const auto cash = s.snapshot()->account.cash;
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("260"), call.expiry_time()).decision.ok());
  EXPECT_EQ(stock(s)->position.shares, 100);
  EXPECT_EQ(s.snapshot()->closures.back().price, m("20"));  // (200 x 260) / 100 - 500 a unit
  EXPECT_EQ(s.snapshot()->account.cash, cash - m("2000") + m("52000"));
  // Share round trips follow the split: one still open, with every share fill.
  const auto trips = share_lifecycles(s.snapshot()->stock_fills, s.snapshot()->dividends);
  ASSERT_EQ(trips.size(), 1u);
  EXPECT_EQ(trips[0].shares, 100);
  EXPECT_EQ(trips[0].fills.size(), 3u);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), s.snapshot_json());
}

TEST(CorporateActions, FractionsArePaidInCashAndReverseSplitsDeliverCashInLieu) {
  const auto put = *md::parse_osi("SPY261016P00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(put, f.time).decision.ok());
  f.quote(s, put, "4.00", "4.20");
  ASSERT_TRUE(s.trade_stock("SPY", 101, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("long", put, 2, Side::Buy), f.time).decision.ok());
  // 3-for-2: 101 shares are 151.5; the half share is paid at the split mark, $340.
  ASSERT_TRUE(s.roll_day(at(kExDate, 9), {}, {{"SPY", kExDate, 3, 2}}).decision.ok());
  EXPECT_EQ(stock(s)->position.shares, 151);
  EXPECT_EQ(s.snapshot()->corporate_actions[0].cash_in_lieu, m("170"));
  EXPECT_EQ(s.snapshot()->deliverables.at(put.osi_symbol()), (Deliverable{150, {}}));
  // 1-for-10 after: 151 shares are 15.1, a series 150 shares is 15.
  f.time = at(kExDate);
  f.spot = 340;
  f.quote(s, put, "100.00", "100.20");
  ASSERT_TRUE(s.roll_day(at({2026, 9, 24}, 9), {}, {{"SPY", {2026, 9, 24}, 1, 10}}).decision.ok());
  EXPECT_EQ(stock(s)->position.shares, 15);
  EXPECT_EQ(stock(s)->mark, m("3400"));
  EXPECT_EQ(s.snapshot()->corporate_actions[1].cash_in_lieu, m("340"));
  EXPECT_EQ(s.snapshot()->deliverables.at(put.osi_symbol()), (Deliverable{15, {}}));
  // A 1-for-3 leaves 5 shares a contract and 5 held.
  f.time = at({2026, 9, 24});
  f.spot = 3400;
  f.quote(s, put, "100.00", "100.20");
  ASSERT_TRUE(s.roll_day(at({2026, 9, 25}, 9), {}, {{"SPY", {2026, 9, 25}, 1, 3}}).decision.ok());
  EXPECT_EQ(s.snapshot()->deliverables.at(put.osi_symbol()), (Deliverable{5, {}}));
  EXPECT_EQ(stock(s)->position.shares, 5);
  // At $9,000 a share its 5 shares are worth $450 a unit against the $500 strike.
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("9000"), put.expiry_time()).decision.ok());
  EXPECT_EQ(s.snapshot()->closures.back().price, m("50"));  // 500 - (5 x 9000) / 100
}

TEST(CorporateActions, ContractsDeliveringLessThanAShareDeliverCashOnly) {
  const auto call = *md::parse_osi("SPY261016C00500000");
  // 100 shares at $510 split into 150 or 200, or half or a tenth of a share: the
  // fraction is all cash at the split price, $51,000 either way, not a whole share.
  const std::vector<std::tuple<std::int64_t, std::int64_t, Deliverable>> splits{
      {3, 2, {150, {}}}, {2, 1, {200, {}}}, {1, 200, {0, m("51000")}}, {1, 1000, {0, m("51000")}}};
  for (const auto& [numerator, denominator, expected] : splits) {
    SCOPED_TRACE(std::to_string(numerator) + ":" + std::to_string(denominator));
    Spy f;
    TradingSession s(roomy(), f.time);
    ASSERT_TRUE(s.define(call, f.time).decision.ok());
    f.quote(s, call, "14.00", "14.20");
    ASSERT_TRUE(s.submit(f.market("long", call, 1, Side::Buy), f.time).decision.ok());
    ASSERT_TRUE(s.roll_day(at(kExDate, 9), {}, {{"SPY", kExDate, numerator, denominator}}).decision.ok());
    EXPECT_EQ(s.snapshot()->deliverables.at(call.osi_symbol()), expected);
    EXPECT_EQ(s.snapshot()->corporate_actions[0].series.at(call.osi_symbol()), expected);
    if (expected.shares != 0) continue;
    // It pays $510 a unit against the $500 strike whatever the shares do, and
    // exercise delivers no shares.
    ASSERT_TRUE(s.settle(call.osi_symbol(), m("1"), call.expiry_time()).decision.ok());
    EXPECT_EQ(s.snapshot()->closures.back().price, m("10"));
    EXPECT_EQ(stock(s), nullptr);
    EXPECT_TRUE(s.snapshot()->stock_fills.empty());
  }
}

TEST(CorporateActions, AShortDeliveringCashOnlyRequiresWhatItOwes) {
  const auto call = *md::parse_osi("SPY261016C00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(call, f.time).decision.ok());
  f.quote(s, call, "14.00", "14.20");
  ASSERT_TRUE(s.submit(f.market("short", call, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.roll_day(at(kExDate, 9), {}, {{"SPY", kExDate, 1, 200}}).decision.ok());
  f.time = at(kExDate);
  f.spot = 102000;
  f.quote(s, call, "9.90", "10.10");
  const auto& snapshot = *s.snapshot();
  // Its payoff is fixed: what buying it back costs, and no move in the shares changes it.
  ASSERT_EQ(snapshot.margin.size(), 1u);
  EXPECT_EQ(snapshot.margin[0].requirement, snapshot.positions[0].market_value ? -*snapshot.positions[0].market_value : Money{});
  EXPECT_GT(snapshot.margin[0].requirement, Money{});
  EXPECT_TRUE(snapshot.risk.complete);
}

TEST(CorporateActions, AdjustedSeriesListedWithTheirDeliverableTrade) {
  auto adjusted = *md::parse_osi("SPY261016C00500000");
  adjusted.root = "SPY1";
  adjusted.standard = false;
  Spy f;
  TradingSession s(roomy(), f.time);
  EXPECT_EQ(s.define(adjusted, f.time).decision.code, Reason::NONSTANDARD_UNSUPPORTED);
  ASSERT_TRUE(s.define(adjusted, f.time, Deliverable{150, m("25")}).decision.ok());
  EXPECT_EQ(s.deliverables().at(adjusted.osi_symbol()), (Deliverable{150, m("25")}));
  // A later definition keeps it; another deliverable conflicts.
  ASSERT_TRUE(s.define(adjusted, f.time).decision.ok());
  EXPECT_EQ(s.define(adjusted, f.time, Deliverable{100, {}}).decision.code, Reason::INVALID_CONTRACT);
  f.quote(s, adjusted, "300.00", "301.00");
  ASSERT_TRUE(s.submit(f.market("buy", adjusted, 1, Side::Buy), f.time).decision.ok());
  auto index = *md::parse_osi("SPXW261016C05000000");
  index.standard = false;
  EXPECT_EQ(s.define(index, f.time, Deliverable{150, {}}).decision.code, Reason::NONSTANDARD_UNSUPPORTED);
  ASSERT_TRUE(s.settle(adjusted.osi_symbol(), m("400"), adjusted.expiry_time()).decision.ok());
  // (150 x 400 + 25) / 100 - 500 = 100.25 a unit; it delivers 150 shares.
  EXPECT_EQ(s.snapshot()->closures.back().price, m("100.25"));
  EXPECT_EQ(stock(s)->position.shares, 150);
}

TEST(CorporateActions, AdjustedSeriesRepriceAsTheStandardSeriesThatPayTheSame) {
  // One 2-for-1 adjusted 500 call is two standard 250 calls at the same volatility.
  const auto call = *md::parse_osi("SPY261016C00500000");
  const auto half = *md::parse_osi("SPY261016C00250000");
  Spy f;
  TradingSession adjusted(roomy(), f.time), standard(roomy(), f.time);
  ASSERT_TRUE(adjusted.define(call, f.time).decision.ok());
  ASSERT_TRUE(standard.define(half, f.time).decision.ok());
  f.quote(adjusted, call, "14.00", "14.20");
  f.quote(standard, half, "7.00", "7.10");
  ASSERT_TRUE(adjusted.submit(f.market("a", call, 1, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(standard.submit(f.market("b", half, 2, Side::Buy), f.time).decision.ok());
  ASSERT_TRUE(adjusted.roll_day(at(kExDate, 9), {}, {{"SPY", kExDate, 2, 1}}).decision.ok());
  ASSERT_TRUE(standard.roll_day(at(kExDate, 9)).decision.ok());
  f.time = at(kExDate, 10);
  f.spot = 255;
  f.quote(adjusted, call, "14.00", "14.20");
  f.quote(standard, half, "7.00", "7.10");
  const auto& a = adjusted.snapshot()->scenarios.cells;
  const auto& b = standard.snapshot()->scenarios.cells;
  ASSERT_EQ(a.size(), b.size());
  ASSERT_TRUE(adjusted.snapshot()->scenarios.complete);
  for (std::size_t i = 0; i < a.size(); ++i) EXPECT_NEAR(a[i].pnl, b[i].pnl, 1e-6) << i;
}

TEST(CorporateActions, AdjustedSeriesAreMarginedInTheirOwnLots) {
  // A naked adjusted call needs what the two standard calls that pay the same need.
  const auto call = *md::parse_osi("SPY261016C00500000");
  const auto half = *md::parse_osi("SPY261016C00250000");
  Spy f;
  f.spot = 255;
  TradingSession adjusted(roomy(), f.time), standard(roomy(), f.time);
  ASSERT_TRUE(adjusted.define([&] { auto c = call; c.root = "SPY1"; c.standard = false; return c; }(), f.time, Deliverable{200, {}}).decision.ok());
  auto listed = call; listed.root = "SPY1"; listed.standard = false;
  ASSERT_TRUE(standard.define(half, f.time).decision.ok());
  f.quote(adjusted, listed, "14.00", "14.20");
  f.quote(standard, half, "7.00", "7.10");
  ASSERT_TRUE(adjusted.submit(f.market("a", listed, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(standard.submit(f.market("b", half, 2, Side::Sell), f.time).decision.ok());
  EXPECT_GT(adjusted.snapshot()->buying_power.short_requirement, Money{});
  EXPECT_EQ(adjusted.snapshot()->buying_power.short_requirement, standard.snapshot()->buying_power.short_requirement);
  const auto& detail = adjusted.snapshot()->margin;
  ASSERT_EQ(detail.size(), 1u);
  EXPECT_EQ(detail[0].parts.at(0).legs.at(0).first, listed.osi_symbol());  // named by the series itself
  // A cash account covers it with its 200 shares, not 100.
  auto cash = roomy();
  cash.rules.account_type = AccountType::Cash;
  cash.rules.buying_power = true;
  TradingSession c(cash, f.time);
  ASSERT_TRUE(c.define(listed, f.time, Deliverable{200, {}}).decision.ok());
  f.quote(c, listed, "14.00", "14.20");
  ASSERT_TRUE(c.trade_stock("SPY", 100, f.time).decision.ok());
  EXPECT_EQ(c.submit(f.market("short", listed, 1, Side::Sell), f.time).decision.code, Reason::ACCOUNT_TYPE);
  ASSERT_TRUE(c.trade_stock("SPY", 100, f.time).decision.ok());
  ASSERT_TRUE(c.submit(f.market("covered", listed, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(c.snapshot()->buying_power.short_requirement, Money{});
}

TEST(CorporateActions, WorkingShareOrdersOnTheStockAreCancelled) {
  Spy f;
  TradingSession s(roomy(), f.time);
  StockOrderRequest r;
  r.client_order_id = "dip";
  r.symbol = "SPY";
  r.type = StockOrderType::Limit;
  r.tif = TimeInForce::Gtc;
  r.shares = 10;
  r.limit_price = m("400");
  ASSERT_TRUE(s.submit_stock_order(r, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  ASSERT_TRUE(s.roll_day(at(kExDate, 9), {}, {{"SPY", kExDate, 2, 1}}).decision.ok());
  EXPECT_EQ(s.snapshot()->stock_orders[0].reason.code, Reason::CORPORATE_ACTION);
  EXPECT_THROW(s.roll_day(at({2026, 9, 24}, 9), {}, {{"SPY", {2026, 9, 24}, 2, 2}}), TradingError);
}
}  // namespace
}  // namespace openport::trading
