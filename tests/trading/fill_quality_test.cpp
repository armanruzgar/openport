#include <gtest/gtest.h>

#include "openport/trading/fill_quality.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
Quantity at(const FillQualityGroup& group, FillPlace place) { return group.places[static_cast<std::size_t>(place)]; }
const FillQualityGroup& named(const std::vector<FillQualityGroup>& groups, std::string_view key) {
  for (const auto& group : groups) if (group.key == key) return group;
  throw std::runtime_error("no group " + std::string(key));
}
const Timestamp kTen = md::new_york_to_utc({2026, 10, 21}, 10, 15, 32);

TEST(FillQuality, PlacesPricesFromTheMidTowardTheNaturalInTheFarSidesTicks) {
  const auto place = [](Side side, std::string_view price, std::string_view bid = "4.00", std::string_view ask = "4.20") {
    return fill_position("SPXW", side, m(price), m(bid), m(ask));
  };
  EXPECT_EQ(place(Side::Buy, "4.10")->place, FillPlace::MidOrBetter);
  EXPECT_EQ(place(Side::Buy, "4.05")->place, FillPlace::MidOrBetter);
  EXPECT_EQ(place(Side::Buy, "4.15")->place, FillPlace::MidPlusOneTick);
  EXPECT_EQ(place(Side::Buy, "4.20")->place, FillPlace::Natural);
  EXPECT_EQ(place(Side::Buy, "4.25")->place, FillPlace::BeyondNatural);
  EXPECT_EQ(place(Side::Buy, "4.45", "4.00", "4.60")->place, FillPlace::Inside);
  EXPECT_EQ(place(Side::Sell, "4.00")->place, FillPlace::Natural);
  EXPECT_EQ(place(Side::Sell, "4.15")->place, FillPlace::MidOrBetter);
  const auto natural = *place(Side::Buy, "4.20");
  EXPECT_DOUBLE_EQ(natural.from_mid_ticks, 1) << "SPXW ticks a dime at 3.00 and over";
  EXPECT_DOUBLE_EQ(natural.vs_natural_ticks, 0);
  EXPECT_DOUBLE_EQ(natural.fraction, 1);
  EXPECT_EQ(natural.spread_ticks, 2);
  EXPECT_EQ(natural.mid, m("4.10"));
  EXPECT_DOUBLE_EQ(place(Side::Sell, "3.95")->vs_natural_ticks, 0.5);
  EXPECT_EQ(place(Side::Buy, "2.10", "2.00", "2.20")->spread_ticks, 4) << "and a nickel below";
  EXPECT_FALSE(fill_position("SPXW", Side::Buy, m("4.20"), std::nullopt, m("4.20")));
  EXPECT_FALSE(fill_position("SPXW", Side::Buy, m("4.20"), m("4.30"), m("4.20")));
}

TEST(FillQuality, TheJournalsFillsByTimeSpreadAndDteWithAHeuristicConservativeSuggestion) {
  ScriptedMarket market;
  SessionConfig config;
  config.limits.aggregate = {1e9, 1e9};
  config.limits.per_underlying = {1e9, 1e9};
  config.rules.inside_fill_percent = 50;
  TradingSession session(config, market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.market("natural", 2), market.time).decision.ok());
  ASSERT_TRUE(session.submit(market.limit("mid", 1, "4.10"), market.time).decision.ok());
  const auto snapshot = session.snapshot();
  ASSERT_EQ(snapshot->recent_fills.size(), 2U);
  const auto report = fill_quality(snapshot->recent_fills, session.contracts());
  EXPECT_EQ(report.fills, 2);
  EXPECT_EQ(report.unmeasured, 0);
  EXPECT_EQ(report.overall.contracts, 3);
  EXPECT_EQ(at(report.overall, FillPlace::Natural), 2);
  EXPECT_EQ(at(report.overall, FillPlace::MidOrBetter), 1);
  EXPECT_NEAR(*report.overall.from_mid_ticks, 2.0 / 3, 1e-12);
  EXPECT_EQ(report.overall.cost_vs_mid, m("20")) << "Two contracts a dime above the mid";
  EXPECT_EQ(named(report.time_of_day, "morning").contracts, 3);
  EXPECT_EQ(named(report.spread, "2").contracts, 3);
  EXPECT_EQ(named(report.dte, "8-30").contracts, 3);
  EXPECT_EQ(named(report.dte, "0").contracts, 0);
  EXPECT_FALSE(named(report.dte, "0").from_mid_ticks);
  EXPECT_EQ(report.time_of_day.size(), 7U);
  EXPECT_EQ(report.suggestion.basis, "heuristic");
  EXPECT_EQ(report.suggestion.fill_model, "conservative");
  EXPECT_EQ(report.suggestion.fill_latency_ms, 1000);
  EXPECT_EQ(report.suggestion.slippage_ticks, 1);
  // Only the attempt's fills from its first one on.
  EXPECT_EQ(fill_quality(snapshot->recent_fills, session.contracts(), 2).overall.contracts, 1);
}

TEST(FillQuality, StressedAndThinFillsRaiseTheSuggestedSlippageAndImpact) {
  ScriptedMarket market;
  market.time = md::new_york_to_utc({2026, 10, 22}, 9, 40);  // expiry day, first half hour
  SessionConfig config;
  config.limits.aggregate = {1e9, 1e9};
  config.limits.per_underlying = {1e9, 1e9};
  TradingSession session(config, market.time);
  market.seed(session, "4.00", "4.60", 2);
  ASSERT_TRUE(session.submit(market.market("thin", 2), market.time).decision.ok());
  const auto report = fill_quality(session.snapshot()->recent_fills, session.contracts());
  EXPECT_EQ(named(report.dte, "0").contracts, 2);
  EXPECT_EQ(named(report.time_of_day, "open").contracts, 2);
  EXPECT_EQ(named(report.spread, "5-9").contracts, 2);
  EXPECT_EQ(report.suggestion.slippage_ticks, 2);
  EXPECT_EQ(report.suggestion.impact_ticks, 2);
  EXPECT_EQ(report.suggestion.fill_model, "custom");
}

TEST(BrokerFills, GenericImportsReadTheQuoteAndRejectUnknownFormats) {
  const auto import = parse_broker_fills(
      "time,symbol,side,quantity,price,bid,ask\n"
      "2026-10-21 10:15:32,SPXW261022C05000000,buy,2,4.10,4.00,4.20\n"
      "2026-10-21T14:16:00Z,SPXW  261022C05000000,SELL,-1,$4.00,4.00,4.20\n"
      "2026-10-21 10:17:00,SPY,buy,1,580.10,,\n"
      "junk,SPXW261022C05000000,buy,1,4.10,,\n");
  EXPECT_EQ(import.format, "generic");
  ASSERT_EQ(import.fills.size(), 2U);
  EXPECT_EQ(import.fills[0].time, kTen);
  EXPECT_EQ(import.fills[0].symbol, "SPXW  261022C05000000");
  EXPECT_EQ(import.fills[0].quantity, 2);
  EXPECT_EQ(import.fills[0].bid, m("4.00"));
  EXPECT_EQ(import.fills[1].side, Side::Sell);
  EXPECT_EQ(import.fills[1].quantity, 1);
  EXPECT_EQ(import.fills[1].time, md::new_york_to_utc({2026, 10, 21}, 10, 16));
  ASSERT_EQ(import.skipped.size(), 2U);
  EXPECT_EQ(import.skipped[0].first, 4U);
  EXPECT_EQ(import.skipped[1].second, "Unreadable time");
  EXPECT_THROW((void)parse_broker_fills("a,b\n1,2\n", "robinhood"), std::invalid_argument);
  EXPECT_THROW((void)parse_broker_fills("a,b\n1,2\n", "generic"), std::invalid_argument);
  EXPECT_THROW((void)parse_broker_fills("", "auto"), std::invalid_argument);
}

TEST(BrokerFills, TastytradeThinkorswimSchwabAndIbkrExportsAreDetected) {
  const auto tasty = parse_broker_fills(
      "Date,Type,Sub Type,Action,Symbol,Instrument Type,Description,Value,Quantity,Average Price,Commissions,Fees,Multiplier,"
      "Root Symbol,Underlying Symbol,Expiration Date,Strike Price,Call or Put,Order #,Currency\n"
      "2026-10-21T10:15:32-0400,Trade,Buy to Open,BUY_TO_OPEN,SPXW  261022C05000000,Equity Option,"
      "\"Bought 1 SPXW 10/22/26 Call 5000.00 @ 4.20\",-420.00,1,-420.00,-1.00,-0.66,100,SPXW,SPX,10/22/26,5000,CALL,1,USD\n"
      "2026-10-21T09:00:00-0400,Money Movement,Deposit,,,,Deposit,1000.00,0,,,,,,,,,,,USD\n");
  EXPECT_EQ(tasty.format, "tastytrade");
  ASSERT_EQ(tasty.fills.size(), 1U);
  EXPECT_EQ(tasty.fills[0].price, m("4.20"));
  EXPECT_EQ(tasty.fills[0].time, kTen);
  EXPECT_EQ(tasty.fills[0].side, Side::Buy);
  EXPECT_EQ(tasty.skipped.front().second, "Not a trade");

  const auto tos = parse_broker_fills(
      "Account Statement for 123 since 10/1/26 through 10/21/26\n\n"
      "Account Trade History\n"
      ",Exec Time,Spread,Side,Qty,Pos Effect,Symbol,Exp,Strike,Type,Price,Net Price,Order Type\n"
      ",10/21/26 10:15:32,VERTICAL,BUY,+1,TO OPEN,SPXW,22 OCT 26,5000,CALL,4.20,1.10,LMT\n"
      ",,,SELL,-1,TO OPEN,SPXW,22 OCT 26,5010,CALL,3.10,CREDIT,\n"
      "\nProfits and Losses\nSymbol,Description\n");
  EXPECT_EQ(tos.format, "schwab");
  ASSERT_EQ(tos.fills.size(), 2U);
  EXPECT_EQ(tos.fills[1].time, kTen) << "A spread's later legs share its execution time";
  EXPECT_EQ(tos.fills[1].symbol, "SPXW  261022C05010000");
  EXPECT_EQ(tos.fills[1].side, Side::Sell);

  const auto schwab = parse_broker_fills(
      "\"Date\",\"Action\",\"Symbol\",\"Description\",\"Quantity\",\"Price\",\"Fees & Comm\",\"Amount\"\n"
      "\"10/21/2026\",\"Buy to Open\",\"SPXW 10/22/2026 5000.00 C\",\"CALL\",\"1\",\"$4.20\",\"$0.66\",\"-$420.66\"\n"
      "\"10/21/2026\",\"Qualified Dividend\",\"SPY\",\"SPDR\",\"\",\"\",\"\",\"$1.00\"\n");
  EXPECT_EQ(schwab.format, "schwab");
  ASSERT_EQ(schwab.fills.size(), 1U);
  EXPECT_EQ(schwab.fills[0].time, 0) << "Schwab's transactions give a date only";
  EXPECT_EQ(schwab.fills[0].symbol, "SPXW  261022C05000000");

  const auto flex = parse_broker_fills(
      "\"Symbol\",\"AssetClass\",\"UnderlyingSymbol\",\"Expiry\",\"Strike\",\"Put/Call\",\"DateTime\",\"Buy/Sell\",\"Quantity\",\"TradePrice\"\n"
      "\"SPXW  261022C05000000\",\"OPT\",\"SPX\",\"20261022\",\"5000\",\"C\",\"20261021;101532\",\"BUY\",\"1\",\"4.2\"\n"
      "\"SPY\",\"STK\",\"\",\"\",\"\",\"\",\"20261021;101600\",\"BUY\",\"100\",\"580.10\"\n");
  EXPECT_EQ(flex.format, "ibkr");
  ASSERT_EQ(flex.fills.size(), 1U);
  EXPECT_EQ(flex.fills[0].time, kTen);
  EXPECT_EQ(flex.fills[0].price, m("4.20"));

  const auto statement = parse_broker_fills(
      "Statement,Header,Field Name,Field Value\n"
      "Trades,Header,DataDiscriminator,Asset Category,Currency,Symbol,Date/Time,Quantity,T. Price,C. Price,Proceeds,Comm/Fee\n"
      "Trades,Data,Order,Equity and Index Options,USD,SPXW 22OCT26 5000 C,\"2026-10-21, 10:15:32\",-1,4.20,4.25,420,-0.65\n"
      "Trades,SubTotal,,Equity and Index Options,USD,SPXW 22OCT26 5000 C,,-1,,,420,-0.65\n"
      "Trades,Data,Order,Stocks,USD,SPY,\"2026-10-21, 10:16:00\",100,580.10,580,-58010,-1\n");
  EXPECT_EQ(statement.format, "ibkr");
  ASSERT_EQ(statement.fills.size(), 1U);
  EXPECT_EQ(statement.fills[0].side, Side::Sell) << "The quantity's sign gives the side";
  EXPECT_EQ(statement.fills[0].symbol, "SPXW  261022C05000000");
  EXPECT_EQ(statement.fills[0].time, kTen);
}

TEST(BrokerFills, QuantitiesBeyondAMillionContractsAreSkippedNotRead) {
  const auto import = parse_broker_fills(
      "time,symbol,side,quantity,price\n"
      "2026-10-21 10:15:32,SPXW261022C05000000,buy,1000000,4.10\n"
      "2026-10-21 10:15:32,SPXW261022C05000000,buy,1000001,4.10\n"
      "2026-10-21 10:15:32,SPXW261022C05000000,,-9223372036854775808,4.10\n", "generic");
  ASSERT_EQ(import.fills.size(), 1U);
  EXPECT_EQ(import.fills[0].quantity, 1'000'000);
  ASSERT_EQ(import.skipped.size(), 2U);
  EXPECT_EQ(import.skipped[0].second, "Quantity beyond 1,000,000 contracts");
  EXPECT_EQ(import.skipped[1].second, "Quantity beyond 1,000,000 contracts");
  const auto tasty = parse_broker_fills(
      "Date,Type,Action,Symbol,Instrument Type,Value,Quantity,Multiplier\n"
      "2026-10-21T10:15:32-0400,Trade,BUY_TO_OPEN,SPXW  261022C05000000,Equity Option,-410.00,-9223372036854775808,100\n",
      "tastytrade");
  EXPECT_TRUE(tasty.fills.empty());
  ASSERT_EQ(tasty.skipped.size(), 1U);
}

BrokerImport priced(std::vector<std::pair<std::string_view, Quantity>> buys, std::string_view bid = "4.00", std::string_view ask = "4.20") {
  BrokerImport import;
  import.format = "generic";
  std::size_t row = 1;
  for (const auto& [price, quantity] : buys) {
    BrokerFill fill;
    fill.row = ++row;
    fill.time = kTen;
    fill.symbol = "SPXW  261022C05000000";
    fill.quantity = quantity;
    fill.price = m(price);
    fill.bid = m(bid);
    fill.ask = m(ask);
    import.fills.push_back(fill);
  }
  return import;
}

TEST(FillCalibration, RealFillsAtTheMidSuggestInsideFillsAndShowWhatEachModelWouldDo) {
  const auto report = calibrate(priced({{"4.10", 1}, {"4.10", 2}, {"4.15", 1}}), {});
  EXPECT_EQ(report.matched, 3);
  EXPECT_EQ(report.real.contracts, 4);
  EXPECT_EQ(at(report.real, FillPlace::MidOrBetter), 3);
  const auto& row = report.rows.front();
  EXPECT_EQ(row.quote->source, "import");
  EXPECT_FALSE(row.as_displayed.fills) << "As displayed, a limit at the mid rests";
  EXPECT_FALSE(row.conservative.fills);
  EXPECT_TRUE(row.midpoint.fills);
  EXPECT_EQ(row.midpoint.price, m("4.10"));
  EXPECT_EQ(report.paper_fills[0], 0);
  EXPECT_EQ(report.paper_fills[2], 3);
  EXPECT_EQ(report.suggestion.basis, "real_fills");
  EXPECT_EQ(report.suggestion.inside_fill_percent, 50);
  EXPECT_EQ(report.suggestion.fill_model, "midpoint");
  EXPECT_FALSE(report.suggestion.fill_latency_ms);
}

TEST(FillCalibration, RealFillsBeyondTheNaturalSuggestSlippageAndLargeOnesImpact) {
  auto import = priced({{"4.40", 3}, {"4.40", 3}, {"4.20", 1}});
  std::map<std::size_t, CalibrationQuote> quotes;
  for (auto& fill : import.fills) {
    fill.bid.reset();
    fill.ask.reset();
    quotes[fill.row] = {m("4.00"), m("4.20"), 2, 2, kTen - md::kNanosPerSecond, "recording"};
  }
  const auto report = calibrate(import, quotes);
  EXPECT_EQ(report.rows.front().quote->source, "recording");
  EXPECT_EQ(report.suggestion.slippage_ticks, 2);
  EXPECT_EQ(report.suggestion.impact_ticks, 1);
  EXPECT_EQ(report.suggestion.fill_model, "custom");
  const auto& large = report.rows.front();
  EXPECT_FALSE(large.as_displayed.fills) << "Three contracts on a two-lot exceed the displayed size";
  EXPECT_TRUE(large.conservative.fills);
  EXPECT_EQ(large.conservative.price, m("4.333333")) << "4.30, 4.30 and 4.40 a contract";
  const auto& natural = report.rows.back();
  EXPECT_TRUE(natural.as_displayed.fills);
  EXPECT_EQ(natural.as_displayed.price, m("4.20"));
  EXPECT_FALSE(natural.conservative.fills);
  EXPECT_EQ(calibrate(priced({{"4.20", 1}}), {}).suggestion.fill_model, "as_displayed");
}

TEST(FillCalibration, PaperPricesSumWholeBlocksSoAMillionContractsAreCheap) {
  // A million-lot on a thousand-lot book: block b sells at 4.20 + (1 + b) ticks.
  auto import = priced({{"104.20", 1'000'000}});  // the last block's price
  import.fills[0].bid.reset();
  import.fills[0].ask.reset();
  const std::map<std::size_t, CalibrationQuote> quotes{{import.fills[0].row, {m("4.00"), m("4.20"), 1000, 1000, kTen, "recording"}}};
  const auto large = calibrate(import, quotes).rows.front();
  EXPECT_FALSE(large.as_displayed.fills);
  EXPECT_FALSE(large.midpoint.fills);
  ASSERT_TRUE(large.conservative.fills);
  EXPECT_EQ(large.conservative.price, m("54.25"));
  // A sell's blocks stop at zero: 0.35, 0.30 ... 0.05, then 23 contracts at nothing.
  BrokerImport sells;
  BrokerFill sell;
  sell.row = 2;
  sell.time = kTen;
  sell.symbol = "SPXW  261022C05000000";
  sell.side = Side::Sell;
  sell.quantity = 30;
  sell.price = m("0.00");
  sells.fills.push_back(sell);
  const auto cheap = calibrate(sells, {{2, {m("0.40"), m("0.60"), 1, 1, kTen, "recording"}}}).rows.front();
  ASSERT_TRUE(cheap.conservative.fills);
  EXPECT_EQ(cheap.conservative.price, m("0.046667")) << "1.40 over 30 contracts";
  EXPECT_FALSE(cheap.as_displayed.fills);
}

TEST(FillCalibration, FillsWithoutATimeOrAQuoteAreListedButNotCompared) {
  auto import = priced({{"4.10", 1}, {"4.10", 1}});
  import.fills[0].time = 0;
  import.fills[0].bid.reset();
  import.fills[1].bid.reset();
  const auto report = calibrate(import, {});
  EXPECT_EQ(report.matched, 0);
  EXPECT_EQ(report.unmatched, 2);
  EXPECT_NE(report.rows[0].note.find("no execution time"), std::string::npos);
  EXPECT_NE(report.rows[1].note.find("No recorded quote"), std::string::npos);
  EXPECT_EQ(report.suggestion.fill_model, "as_displayed");
}

}  // namespace
}  // namespace openport::trading
