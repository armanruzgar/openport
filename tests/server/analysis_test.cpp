#include "openport/server/analysis.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <nlohmann/json.hpp>

#include "openport/analytics/chain_analytics.hpp"
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
  md::Timestamp wall_time() const override { return md::new_york_to_utc({2026, 9, 22}, 15, 1); }
  bool post_trading(server::TradingCommand, server::TradingCompletion) override {
    ++orders;  // must never be reached
    return false;
  }
  int orders = 0;

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
  EXPECT_EQ(body["views"], json::array({"grid", "profile"}));
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

  EXPECT_EQ(call(store, source, "GET", "/api/analysis/portfolios/options-only/risk/var").status, 404);
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

}  // namespace
