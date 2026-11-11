#include "openport/server/analysis.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/server/candles.hpp"
#include "openport/trading/stress.hpp"
#include "support/synthetic_chain.hpp"

namespace {

using namespace openport;
using nlohmann::json;

// The CSVs below are hand-written in each broker's column layout with made-up
// positions; none is a downloaded statement.

TEST(AnalysisImport, GenericReadsOsiTickersAndSplitColumnsAndNetsLots) {
  auto p = server::parse_analysis_csv(
      "Symbol,Quantity,Cost\n"
      "SPXW261022C05000000,2,40.50\n"
      "\"SPXW  261022C05000000\",1,43.50\n"
      "spy,100,$571.20\n"
      "SPXW  261022P04900000,-3,\n",
      "auto");
  EXPECT_EQ(p.format, "generic");
  ASSERT_EQ(p.holdings.size(), 3u);
  EXPECT_EQ(p.holdings[0].symbol, "SPXW  261022C05000000");
  EXPECT_EQ(p.holdings[0].quantity, 3);
  EXPECT_DOUBLE_EQ(*p.holdings[0].cost, 41.5);  // weighted by lot
  EXPECT_EQ(p.holdings[1].symbol, "SPY");
  EXPECT_FALSE(p.holdings[1].contract);
  EXPECT_EQ(p.holdings[2].quantity, -3);
  EXPECT_FALSE(p.holdings[2].cost);

  p = server::parse_analysis_csv("underlying,expiry,strike,type,quantity\nSPY,2026-12-18,600,put,-1\n", "generic");
  ASSERT_EQ(p.holdings.size(), 1u);
  EXPECT_EQ(p.holdings[0].symbol, "SPY   261218P00600000");

  EXPECT_THROW((void)server::parse_analysis_csv("symbol,quantity\nSPY,1.5\n", "generic"), std::invalid_argument);
  EXPECT_THROW((void)server::parse_analysis_csv("symbol,quantity\n???,1\n", "generic"), std::invalid_argument);
  EXPECT_THROW((void)server::parse_analysis_csv("ticker,shares\nSPY,1\n", "generic"), std::invalid_argument);
  EXPECT_THROW((void)server::parse_analysis_csv("symbol,quantity\nSPY,1\nSPY,-1\n", "generic"), std::invalid_argument);  // nets to nothing
  EXPECT_THROW((void)server::parse_analysis_csv("symbol,quantity\n\"SPY,1\n", "generic"), std::invalid_argument);
  EXPECT_THROW((void)server::parse_analysis_csv("symbol,quantity\nSPY,1\n", "fidelity"), std::invalid_argument);
}

TEST(AnalysisImport, ABrokerSizedFileStillStopsAtTwoThousandHoldings) {
  // Over 64 KiB, as the import route now accepts, but each holding is still counted.
  const auto file = [](std::size_t holdings) {
    std::string csv = "symbol,quantity,description\n";
    for (std::size_t i = 0; i < holdings; ++i)
      csv += std::string{char('A' + i / 676), char('A' + i / 26 % 26), char('A' + i % 26)} + ",1," + std::string(40, 'x') + "\n";
    return csv;
  };
  const auto most = file(server::kAnalysisMaxHoldings);
  EXPECT_GT(most.size(), 64u * 1024);
  EXPECT_EQ(server::parse_analysis_csv(most, "generic").holdings.size(), server::kAnalysisMaxHoldings);
  EXPECT_THROW((void)server::parse_analysis_csv(file(server::kAnalysisMaxHoldings + 1), "generic"), std::invalid_argument);
}

TEST(AnalysisImport, TastytradeSchwabAndIbkrLayouts) {
  auto p = server::parse_analysis_csv(
      "Account,Symbol,Type,Quantity,Exp Date,DTE,Strike Price,Call/Put,Underlying Symbol,Trade Price\n"
      "5WX00000,SPY   261218C00600000,EQUITY_OPTION,-2,12/18/2026,58,600,CALL,SPY,4.10\n"
      "5WX00000,QQQ   261218P00480000,Equity Option,1,12/18/2026,58,480,PUT,QQQ,6.25\n"
      "5WX00000,AAPL,Equity,50,,,,,AAPL,180.00\n"
      "5WX00000,/ESZ6,Future,1,,,,,/ES,5800\n",
      "auto");
  EXPECT_EQ(p.format, "tastytrade");
  ASSERT_EQ(p.holdings.size(), 3u);
  EXPECT_EQ(p.holdings[0].quantity, -2);
  EXPECT_DOUBLE_EQ(*p.holdings[0].cost, 4.10);
  EXPECT_EQ(p.holdings[2].symbol, "AAPL");
  ASSERT_EQ(p.skipped.size(), 1u);  // the future

  p = server::parse_analysis_csv(
      "\"Positions for account Individual ...123 as of 10:15 AM ET, 2026/10/21\"\n"
      "\n"
      "\"Symbol\",\"Description\",\"Qty (Quantity)\",\"Price\",\"Cost Basis\",\"Security Type\"\n"
      "\"SPY 12/18/2026 600.00 C\",\"CALL SPDR S&P500 ETF $600 EXP 12/18/26\",\"-2\",\"$4.30\",\"-$820.00\",\"Option\"\n"
      "\"MSFT\",\"MICROSOFT CORP\",\"10\",\"$420.00\",\"$4,000.00\",\"Equity\"\n"
      "\"Cash & Cash Investments\",\"--\",\"--\",\"--\",\"--\",\"Cash and Money Market\"\n"
      "\"Account Total\",\"--\",\"--\",\"--\",\"$12,000.00\",\"--\"\n",
      "auto");
  EXPECT_EQ(p.format, "schwab");
  ASSERT_EQ(p.holdings.size(), 2u);
  EXPECT_EQ(p.holdings[0].symbol, "SPY   261218C00600000");
  EXPECT_EQ(p.holdings[0].quantity, -2);
  EXPECT_DOUBLE_EQ(*p.holdings[0].cost, 4.10);  // $820 over 2 x 100
  EXPECT_DOUBLE_EQ(*p.holdings[1].cost, 400);
  EXPECT_EQ(p.skipped.size(), 2u);

  // Some Schwab exports write two-digit years: MM/DD/YY is 20YY.
  p = server::parse_analysis_csv(
      "\"Symbol\",\"Qty (Quantity)\",\"Price\",\"Security Type\"\n"
      "\"SPY 12/18/26 600.00 C\",\"-2\",\"$4.30\",\"Option\"\n"
      "\"QQQ 1/15/27 480 P\",\"1\",\"$6.25\",\"Option\"\n"
      "\"IWM 12/18/226 200 P\",\"1\",\"$1.00\",\"Option\"\n",
      "schwab");
  ASSERT_EQ(p.holdings.size(), 2u);
  EXPECT_EQ(p.holdings[0].symbol, "SPY   261218C00600000");
  EXPECT_EQ(p.holdings[1].symbol, "QQQ   270115P00480000");
  EXPECT_EQ(p.skipped.size(), 1u);  // a three-digit year is not a date

  p = server::parse_analysis_csv(
      "Statement,Header,Field Name,Field Value\n"
      "Open Positions,Header,DataDiscriminator,Asset Category,Currency,Symbol,Quantity,Mult,Cost Price,Close Price\n"
      "Open Positions,Data,Summary,Stocks,USD,IWM,100,1,210.5,220\n"
      "Open Positions,Data,Lot,Stocks,USD,IWM,100,1,210.5,220\n"
      "Open Positions,Data,Summary,Equity and Index Options,USD,SPX 20NOV26 5800 P,-1,100,31.2,30\n"
      "Open Positions,Total,,Stocks,USD,,,,,\n",
      "auto");
  EXPECT_EQ(p.format, "ibkr");
  ASSERT_EQ(p.holdings.size(), 2u);
  EXPECT_EQ(p.holdings[0].quantity, 100);  // the lot repeats the summary and is not added
  EXPECT_EQ(p.holdings[1].symbol, "SPX   261120P05800000");
  EXPECT_EQ(p.holdings[1].contract->settlement, md::Settlement::AM);

  p = server::parse_analysis_csv(
      "AssetClass,Symbol,UnderlyingSymbol,Expiry,Strike,Put/Call,Quantity,Multiplier,CostBasisPrice\n"
      "OPT,QQQ   261218C00500000,QQQ,20261218,500,C,3,100,7.5\n"
      "STK,QQQ,QQQ,,,,-100,1,495\n",
      "ibkr");
  ASSERT_EQ(p.holdings.size(), 2u);
  EXPECT_EQ(p.holdings[0].quantity, 3);
  EXPECT_EQ(p.holdings[1].quantity, -100);
}

// Lots of opposite sides close against each other, as an average-cost ledger reduces a
// position, so what remains keeps its own side's cost. Their signed mix could be
// negative, which no position has, and a portfolio stored with it failed every read.
TEST(AnalysisImport, OppositeLotsKeepTheCostOfTheSideThatRemains) {
  const auto holding = [](const std::string& lots) {
    const auto p = server::parse_analysis_csv("symbol,quantity,cost\n" + lots, "generic");
    EXPECT_EQ(p.holdings.size(), 1u) << lots;
    return p.holdings.at(0);
  };
  auto h = holding("SPY,2,1\nSPY,-1,3\n");  // was (2 - 3) / 1 = -1
  EXPECT_EQ(h.quantity, 1);
  EXPECT_EQ(h.cost, 1.0);
  h = holding("SPY,-1,3\nSPY,2,1\n");
  EXPECT_EQ(h.quantity, 1);
  EXPECT_EQ(h.cost, 1.0);
  h = holding("SPY,-2,3\nSPY,1,1\n");  // a net short keeps the short lots' cost
  EXPECT_EQ(h.quantity, -1);
  EXPECT_EQ(h.cost, 3.0);
  h = holding("SPY,1,1\nSPY,-3,2\n");  // through zero: the side it ends on
  EXPECT_EQ(h.quantity, -2);
  EXPECT_EQ(h.cost, 2.0);
  h = holding("SPY,2,1\nSPY,-1,5\nSPY,2,3\n");  // the long lots average 2, whatever comes between
  EXPECT_EQ(h.quantity, 3);
  EXPECT_EQ(h.cost, 2.0);
  h = holding("SPY,2,1\nSPY,-1,\n");  // the closing side needs no cost
  EXPECT_EQ(h.cost, 1.0);
  h = holding("SPY,2,\nSPY,1,3\nSPY,-1,3\n");  // a remaining lot without one leaves none
  EXPECT_FALSE(h.cost);
  h = holding("SPXW261022C05000000,2,1\nSPXW261022C05000000,-1,3\n");
  EXPECT_EQ(h.quantity, 1);
  EXPECT_EQ(h.cost, 1.0);
}

// A file that names only the underlying gets the root that lists the expiry: SPX, NDX
// and RUT settle their monthlies in the morning and every other expiry on the close.
TEST(AnalysisImport, SplitColumnsTakeTheClosingRootOffTheMonthlyExpiry) {
  const auto symbol = [](const std::string& row) {
    const auto p = server::parse_analysis_csv("underlying,expiry,strike,type,quantity\n" + row + ",1\n", "generic");
    return p.holdings.at(0).symbol;
  };
  EXPECT_EQ(symbol("SPX,2026-11-09,6000,put"), "SPXW  261109P06000000");
  EXPECT_EQ(symbol("$SPX,2026-11-10,6000,call"), "SPXW  261110C06000000");
  EXPECT_EQ(symbol("SPX,2026-11-20,6000,put"), "SPX   261120P06000000");  // the third Friday
  EXPECT_EQ(symbol("SPXW,2026-11-20,6000,put"), "SPXW  261120P06000000");  // a root written out is kept
  EXPECT_EQ(symbol("NDX,2026-11-13,20000,call"), "NDXP  261113C20000000");
  EXPECT_EQ(symbol("RUT,2026-12-31,2500,call"), "RUTW  261231C02500000");
  EXPECT_EQ(symbol("RUT,2026-12-18,2500,call"), "RUT   261218C02500000");
  // Good Friday 2030 is the third Friday of April: the monthly expires the day before.
  EXPECT_EQ(symbol("SPX,2030-04-18,6000,put"), "SPX   300418P06000000");
  // VIX weeklies settle in the morning too, and other underlyings have one root.
  EXPECT_EQ(symbol("VIX,2026-11-25,20,call"), "VIX   261125C00020000");
  EXPECT_EQ(symbol("SPY,2026-11-09,600,put"), "SPY   261109P00600000");
  // tastytrade's and IBKR's columns name the underlying too.
  auto p = server::parse_analysis_csv(
      "Symbol,Type,Quantity,Exp Date,Strike Price,Call/Put,Underlying Symbol\n"
      "SPXW 6000P,Equity Option,1,11/09/2026,6000,PUT,SPX\n",
      "tastytrade");
  EXPECT_EQ(p.holdings.at(0).symbol, "SPXW  261109P06000000");
  p = server::parse_analysis_csv(
      "AssetClass,Symbol,UnderlyingSymbol,Expiry,Strike,Put/Call,Quantity\n"
      "OPT,SPXW 6000P,SPX,20261109,6000,P,1\n",
      "ibkr");
  EXPECT_EQ(p.holdings.at(0).symbol, "SPXW  261109P06000000");
}

// Futures and options on them are left out and listed, as the docs say, rather than
// read as equity options on the futures root or refusing the whole file.
TEST(AnalysisImport, FuturesAndFuturesOptionsAreSkipped) {
  auto p = server::parse_analysis_csv(
      "Symbol,Type,Quantity,Exp Date,Strike Price,Call/Put,Underlying Symbol\n"
      "./ESZ6 EW3X6 261120P5800,Future Option,1,11/20/2026,5800,PUT,/ES\n"
      "./ESZ6 EW3X6 261120C6000,Future Option,-1,,,,/ES\n"
      "SPY,Equity,10,,,,SPY\n",
      "tastytrade");
  ASSERT_EQ(p.holdings.size(), 1u);
  EXPECT_EQ(p.holdings[0].symbol, "SPY");
  EXPECT_EQ(p.skipped.size(), 2u);

  p = server::parse_analysis_csv(
      "Open Positions,Header,DataDiscriminator,Asset Category,Currency,Symbol,Quantity,Mult,Cost Price,Close Price\n"
      "Open Positions,Data,Summary,Stocks,USD,SPY,100,1,500,510\n"
      "Open Positions,Data,Summary,Futures Options,USD,ESZ6 P5800,1,50,20,25\n"
      "Open Positions,Data,Summary,Options On Futures,USD,ES 20NOV26 5800 P,1,50,20,25\n",
      "auto");
  EXPECT_EQ(p.format, "ibkr");
  ASSERT_EQ(p.holdings.size(), 1u);
  EXPECT_EQ(p.holdings[0].symbol, "SPY");
  ASSERT_EQ(p.skipped.size(), 2u);
  EXPECT_NE(p.skipped[1].find("Options On Futures is not supported"), std::string::npos) << p.skipped[1];

  p = server::parse_analysis_csv("symbol,quantity\n/ESZ6,1\n./ESZ6 EW3X6 261120P5800,1\nSPY,1\n", "generic");
  ASSERT_EQ(p.holdings.size(), 1u);
  EXPECT_EQ(p.skipped.size(), 2u);
  p = server::parse_analysis_csv("underlying,expiry,strike,type,quantity\n/ES,2026-11-20,5800,put,1\nSPY,2026-12-18,600,put,1\n",
                                 "generic");
  ASSERT_EQ(p.holdings.size(), 1u);
  EXPECT_EQ(p.holdings[0].symbol, "SPY   261218P00600000");
  EXPECT_EQ(p.skipped.size(), 1u);
  p = server::parse_analysis_csv("\"Symbol\",\"Qty (Quantity)\",\"Security Type\"\n\"/ESZ26\",\"1\",\"\"\n\"MSFT\",\"10\",\"Equity\"\n",
                                 "schwab");
  ASSERT_EQ(p.holdings.size(), 1u);
  EXPECT_EQ(p.skipped.size(), 1u);
}

class AnalysisSource final : public server::MetricsSource {
 public:
  AnalysisSource() {
    test::SyntheticChain chain;
    metrics_ = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of));
  }
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override {
    return symbol == "SPX" ? metrics_ : nullptr;
  }
  server::EngineStatus status() const override { return {}; }
  const server::CandleStore* candles() const override { return &candles_; }
  md::Timestamp wall_time() const override { return md::new_york_to_utc({2026, 9, 22}, 15, 1); }
  bool post_trading(server::TradingCommand, server::TradingCompletion) override {
    ++orders;  // must never be reached
    return false;
  }
  int orders = 0;
  server::CandleStore candles_;

 private:
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics_;
};

server::ApiResponse call(server::AnalysisPortfolios& store, AnalysisSource& source, std::string method,
                         std::string target, std::string body = {}) {
  server::ApiRequest request(std::move(method), std::move(target), std::move(body));
  auto response = server::analysis_request(request, store, source);
  EXPECT_TRUE(response);
  return response.value_or(server::ApiResponse{0, "{}"});
}

TEST(AnalysisApi, ImportsValuesRunsRiskViewsAndNeverTrades) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-analysis-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  AnalysisSource source;
  server::AnalysisPortfolios store(directory);
  const std::string csv = "symbol,quantity,cost\nSPXW261022C05000000,2,40\nSPXW261022P04900000,-1,30\nSPY,100,570\n";
  auto response = call(store, source, "POST", "/api/analysis/portfolios",
                       json{{"name", "IRA at Schwab"}, {"csv", csv}, {"format", "generic"}}.dump());
  ASSERT_EQ(response.status, 201) << response.body;
  auto body = json::parse(response.body);
  EXPECT_EQ(body["id"], "ira-at-schwab");
  EXPECT_EQ(body["analysis_only"], true);
  EXPECT_EQ(body["read_only"], true);
  EXPECT_NE(body["note"].get<std::string>().find("never places orders"), std::string::npos);
  ASSERT_EQ(body["holdings"].size(), 3u);
  EXPECT_EQ(body["holdings"][0]["status"], "priced");
  EXPECT_GT(body["holdings"][0]["delta"].get<double>(), 0);
  EXPECT_EQ(body["holdings"][2]["status"], "no_market_data");  // SPY is not subscribed here
  EXPECT_EQ(body["unpriced"], json::array({"SPY"}));
  EXPECT_TRUE(body["value"].is_null());
  EXPECT_EQ(body["views"], json::array({"grid", "profile", "stress", "var"}));
  EXPECT_FALSE(body["exposure"]["complete"]);

  response = call(store, source, "GET", "/api/analysis/portfolios/ira-at-schwab/risk/grid?spot=-5,0,5&vol=0");
  ASSERT_EQ(response.status, 200) << response.body;
  body = json::parse(response.body);
  EXPECT_EQ(body["complete"], false);  // the shares have no price and are left out
  const auto without_shares = body["pnl"][0][0];
  EXPECT_LT(without_shares.get<double>(), 0);

  // Without the shares the book is complete.
  call(store, source, "POST", "/api/analysis/portfolios",
       json{{"name", "Options only"}, {"csv", "symbol,quantity\nSPXW261022C05000000,2\nSPXW261022P04900000,-1\n"}}.dump());
  body = json::parse(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/grid?spot=-5,0,5&vol=0").body);
  ASSERT_EQ(body["complete"], true) << body.dump();
  EXPECT_EQ(body["pnl"][0][0], without_shares);  // the same options, valued alike
  EXPECT_EQ(body["pnl"][1][0], 0.0);
  EXPECT_LT(body["pnl"][0][0].get<double>(), 0);  // long calls, short puts: a fall loses
  EXPECT_GT(body["pnl"][2][0].get<double>(), 0);
  body = json::parse(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/profile?underlying=SPX&days=0,7&steps=5").body);
  ASSERT_EQ(body["curves"].size(), 2u);
  EXPECT_EQ(body["curves"][0]["pnl"].size(), 5u);
  EXPECT_EQ(body["curves"][0]["pnl"][2], 0.0);
  EXPECT_EQ(body["reference"], "SPX");
  EXPECT_EQ(body["analysis_only"], true);

  EXPECT_EQ(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/surface").status, 404);
  EXPECT_EQ(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/grid?spot=x").status, 400);
  EXPECT_EQ(call(store, source, "POST", "/api/analysis/portfolios/options-only/risk/grid", "{}").status, 405);
  EXPECT_EQ(call(store, source, "POST", "/api/analysis/portfolios", json{{"name", "x"}, {"csv", "nothing"}}.dump()).status, 400);
  EXPECT_EQ(call(store, source, "POST", "/api/analysis/portfolios", json{{"name", ""}, {"csv", csv}}.dump()).status, 400);
  EXPECT_EQ(call(store, source, "POST", "/api/analysis/portfolios", json{{"name", "y"}, {"csv", csv}, {"account", "main"}}.dump()).status, 400);
  EXPECT_FALSE(server::analysis_request(server::ApiRequest("GET", "/api/orders"), store, source));

  // Stored portfolios survive a restart; a deleted one is gone.
  server::AnalysisPortfolios reopened(directory);
  EXPECT_TRUE(reopened.error().empty()) << reopened.error();
  ASSERT_EQ(reopened.list().size(), 2u);
  EXPECT_EQ(reopened.find("ira-at-schwab")->holdings[0].cost, 40);
  EXPECT_EQ(call(reopened, source, "DELETE", "/api/analysis/portfolios/ira-at-schwab").status, 200);
  EXPECT_EQ(call(reopened, source, "GET", "/api/analysis/portfolios/ira-at-schwab").status, 404);
  body = json::parse(call(reopened, source, "GET", "/api/analysis/portfolios").body);
  ASSERT_EQ(body["portfolios"].size(), 1u);
  EXPECT_EQ(body["portfolios"][0]["analysis_only"], true);
  EXPECT_EQ(source.orders, 0);
  std::filesystem::remove_all(directory);
}

// The risk branch's stress tests and historical VaR run on an imported book as
// further views; with no account there is no floor or paper margin to report.
TEST(AnalysisApi, StressTestsAndValueAtRiskRunOnTheValuedHoldings) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-analysis-stress-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  AnalysisSource source;
  server::AnalysisPortfolios store(directory);
  ASSERT_EQ(call(store, source, "POST", "/api/analysis/portfolios",
                 json{{"name", "Options only"}, {"csv", "symbol,quantity\nSPXW261022C05000000,2\nSPXW261022P04900000,-1\n"}}.dump()).status, 201);
  auto response = call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/stress");
  ASSERT_EQ(response.status, 200) << response.body;
  auto body = json::parse(response.body);
  EXPECT_EQ(body["view"], "stress");
  EXPECT_EQ(body["analysis_only"], true);
  EXPECT_TRUE(body["complete"]);
  ASSERT_EQ(body["stress"].size(), trading::historical_stresses().size());
  for (const auto& stress : body["stress"]) {
    EXPECT_TRUE(stress["complete"]) << stress.dump();
    EXPECT_TRUE(stress["pnl"].is_number());
    EXPECT_EQ(stress["missing_underlyings"], json::array());
    EXPECT_FALSE(stress.contains("margin"));
    EXPECT_FALSE(stress.contains("floor_breach"));
  }
  const auto covid = std::find_if(body["stress"].begin(), body["stress"].end(), [](const json& s) { return s["id"] == "covid-2020"; });
  ASSERT_NE(covid, body["stress"].end());
  EXPECT_LT((*covid)["pnl"].get<double>(), 0);  // long calls and short puts lose in a crash
  EXPECT_EQ(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/stress?days=1").status, 400);

  // No daily history yet: VaR says it cannot be estimated rather than reading zero.
  body = json::parse(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/var").body);
  EXPECT_EQ(body["view"], "var");
  EXPECT_FALSE(body["complete"]);
  EXPECT_TRUE(body["sticky_strike"]["var95"].is_null());
  EXPECT_TRUE(body["sticky_strike"]["tail99_reason"].is_string());
  // Thirty completed sessions of SPX closes give the 95% figures; 99% needs 100.
  std::vector<md::Bar> days;
  double close = 5000;
  for (auto day = md::days_since_epoch({2026, 7, 1}); days.size() < 30; ++day) {
    const auto date = md::date_from_days(day);
    if (!md::stock_session(md::new_york_to_utc(date, 12, 0)).open) continue;
    close *= days.size() % 3 ? 1.004 : 0.99;
    days.push_back({md::new_york_to_utc(date, 9, 30), close, close, close, close});
  }
  source.candles_.merge_days("SPX", days);
  body = json::parse(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/var").body);
  ASSERT_TRUE(body["complete"]) << body.dump();
  EXPECT_EQ(body["sticky_strike"]["observations"], 29);
  EXPECT_GT(body["sticky_strike"]["var95"].get<double>(), 0);
  EXPECT_GE(body["sticky_strike"]["es95"].get<double>(), body["sticky_strike"]["var95"].get<double>());
  EXPECT_TRUE(body["sticky_strike"]["var99"].is_null());
  EXPECT_FALSE(body["atm_vol_change"]["complete"]);  // no recorded ATM IV series here

  // Unpriced holdings make both views incomplete, as every analysis view is.
  ASSERT_EQ(call(store, source, "POST", "/api/analysis/portfolios",
                 json{{"name", "With shares"}, {"csv", "symbol,quantity\nSPXW261022C05000000,2\nSPY,100\n"}}.dump()).status, 201);
  body = json::parse(call(store, source, "GET", "/api/analysis/portfolios/with-shares/risk/var").body);
  EXPECT_FALSE(body["complete"]);
  EXPECT_FALSE(body["sticky_strike"]["complete"]);
  body = json::parse(call(store, source, "GET", "/api/analysis/portfolios/with-shares/risk/stress").body);
  EXPECT_FALSE(body["complete"]);
  EXPECT_FALSE(body["stress"][0]["complete"]);
  EXPECT_EQ(source.orders, 0);
  std::filesystem::remove_all(directory);
}

// Opposite lots used to average to a negative cost: the import answered 500 yet stored
// the portfolio, and every read of it answered 500 after. A file an earlier version
// stored that way reads again, its negative costs left out of the book.
TEST(AnalysisApi, OppositeLotsImportAndAStoredNegativeCostStillReads) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-analysis-lots-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  AnalysisSource source;
  {
    server::AnalysisPortfolios store(directory);
    const auto response = call(store, source, "POST", "/api/analysis/portfolios",
        json{{"name", "Lots"}, {"csv", "symbol,quantity,cost\nSPXW261022C05000000,2,1\nSPXW261022C05000000,-1,3\n"}}.dump());
    ASSERT_EQ(response.status, 201) << response.body;
    const auto body = json::parse(response.body);
    EXPECT_EQ(body["holdings"][0]["quantity"], 1);
    EXPECT_EQ(body["holdings"][0]["cost"], 1);
    EXPECT_EQ(body["holdings"][0]["status"], "priced");
    EXPECT_EQ(call(store, source, "GET", "/api/analysis/portfolios/lots/risk/grid").status, 200);
  }
  {
    std::ofstream file(directory / "old.json");
    file << json{{"id", "old"}, {"name", "Old"}, {"format", "generic"}, {"imported", 0}, {"analysis_only", true},
                 {"holdings", {{{"symbol", "SPXW  261022C05000000"}, {"quantity", 1}, {"cost", -1}, {"source", "line 2"}},
                               {{"symbol", "SPX"}, {"quantity", 1}, {"cost", -1}, {"source", "line 4"}}}},
                 {"skipped", json::array()}}.dump();
  }
  server::AnalysisPortfolios reopened(directory);
  EXPECT_TRUE(reopened.error().empty()) << reopened.error();
  ASSERT_EQ(reopened.list().size(), 2u);
  const auto response = call(reopened, source, "GET", "/api/analysis/portfolios/old");
  ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_TRUE(json::parse(response.body)["unpriced"].empty()) << response.body;
  for (const auto* view : {"profile?underlying=SPX", "grid", "stress", "var"})
    EXPECT_EQ(call(reopened, source, "GET", std::string("/api/analysis/portfolios/old/risk/") + view).status, 200) << view;
  std::filesystem::remove_all(directory);
}

// Imported holdings are a real brokerage account's. Under a shell's umask 022 they used
// to land 0644 in a 0755 directory beside an owner-only journal.
TEST(AnalysisApi, ImportedHoldingsAreOwnerOnlyOnDiskAndOlderFilesAreTightened) {
  const auto home = std::filesystem::temp_directory_path() / ("openport-analysis-modes-" + std::to_string(::getpid()));
  std::filesystem::remove_all(home);
  const auto previous = ::umask(022);
  std::filesystem::create_directories(home);
  const auto mode_of = [](const std::filesystem::path& path) {
    struct stat info {};
    EXPECT_EQ(::stat(path.c_str(), &info), 0) << path;
    return static_cast<unsigned>(info.st_mode) & 0777u;
  };
  const auto directory = home / ".openport" / "analysis";
  {
    server::AnalysisPortfolios store(directory);
    store.add(server::parse_analysis_csv("symbol,quantity\nSPY,100\n", "generic"), "Brokerage", 1);
  }
  EXPECT_EQ(mode_of(home), 0755u);
  EXPECT_EQ(mode_of(home / ".openport"), 0700u);
  EXPECT_EQ(mode_of(directory), 0700u);
  EXPECT_EQ(mode_of(directory / "brokerage.json"), 0600u);
  ASSERT_EQ(::chmod((directory / "brokerage.json").c_str(), 0644), 0);
  {
    server::AnalysisPortfolios reopened(directory);
    EXPECT_EQ(reopened.list().size(), 1u);
  }
  EXPECT_EQ(mode_of(directory / "brokerage.json"), 0600u);
  ::umask(previous);
  std::filesystem::remove_all(home);
}

// The split-column layout names SPX; the chain lists that expiry under SPXW.
TEST(AnalysisApi, SplitColumnWeekliesFindTheirChain) {
  AnalysisSource source;
  server::AnalysisPortfolios store;
  const auto response = call(store, source, "POST", "/api/analysis/portfolios",
      json{{"name", "Weekly"}, {"csv", "underlying,expiry,strike,type,quantity\nSPX,2026-10-22,5000,call,1\n"}}.dump());
  ASSERT_EQ(response.status, 201) << response.body;
  const auto body = json::parse(response.body);
  EXPECT_EQ(body["holdings"][0]["symbol"], "SPXW  261022C05000000");
  EXPECT_EQ(body["holdings"][0]["status"], "priced");
  EXPECT_TRUE(body["unpriced"].empty());
}

}  // namespace
