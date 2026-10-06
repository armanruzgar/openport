#include <cmath>
#include <gtest/gtest.h>
#include <random>
#include <set>

#include "openport/server/api.hpp"
#include "server/paper_csv.hpp"
#include "server/paper_json.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
class ReviewSource final : public server::MetricsSource {
 public:
  std::shared_ptr<server::TradingView> view;
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { server::EngineStatus s; s.provider = "replay (demo)"; return s; }
  using server::MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view() const override { return view; }
  void publish(const trading::TradingSession& session) {
    view = std::make_shared<server::TradingView>();
    view->snapshot = session.snapshot(); view->contracts = session.contracts(); view->config = session.config();
  }
};
/// Parse quoted fields, including embedded newlines, independently of the writer.
std::vector<std::vector<std::string>> csv_rows(const std::string& csv) {
  std::vector<std::vector<std::string>> rows;
  std::vector<std::string> row;
  std::string field;
  bool quoted = false;
  for (std::size_t i = 0; i < csv.size(); ++i) {
    const auto c = csv[i];
    if (c == '"') {
      if (quoted && i + 1 < csv.size() && csv[i + 1] == '"') { field += '"'; ++i; }
      else quoted = !quoted;
    } else if (c == ',' && !quoted) { row.push_back(field); field.clear(); }
    else if (c == '\r' && !quoted && i + 1 < csv.size() && csv[i + 1] == '\n') {
      row.push_back(field); rows.push_back(row); row.clear(); field.clear(); ++i;
    } else field += c;
  }
  EXPECT_FALSE(quoted); EXPECT_TRUE(row.empty()); EXPECT_TRUE(field.empty());
  return rows;
}
json csv_record(const std::vector<std::string>& columns, const std::vector<std::string>& row) {
  EXPECT_EQ(columns.size(), row.size());
  json out = json::object();
  for (std::size_t i = 0; i < std::min(columns.size(), row.size()); ++i) out[columns[i]] = row[i];
  return out;
}
void check_columns(const json& value, const std::set<std::string>& columns, const std::string& prefix = {}) {
  for (const auto& [key, field] : value.items()) {
    const auto path = prefix.empty() ? key : prefix + "." + key;
    if (field.is_object()) check_columns(field, columns, path);
    else if (field.is_null()) {
      EXPECT_TRUE(columns.contains(path) || std::any_of(columns.begin(), columns.end(), [&](const auto& c) { return c.starts_with(path + "."); })) << path;
    } else {
      EXPECT_TRUE(columns.contains(path)) << path;
    }
  }
}

TEST(TradeReviewApi, CsvKeepsItemizedFillFeesExactAndItsEmptyColumnsStable) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.rules.fees = trading::FeeSchedule{Money::parse("1"), {}, {}, Money::parse("0.10"), Money::parse("0.02"),
                                         {{"SPXW", Money::parse("0.600001")}}, {}};
  trading::TradingSession session(config, market.time);
  market.seed(session);
  ReviewSource source;
  source.publish(session);
  const auto columns = csv_rows(server::handle_api({"GET", "/api/fills.csv"}, source).body).front();
  ASSERT_TRUE(session.submit(market.market("open", 3), market.time).decision.ok());
  source.publish(session);
  const auto rows = csv_rows(server::handle_api({"GET", "/api/fills.csv"}, source).body);
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows.front(), columns);
  const auto row = csv_record(columns, rows[1]);
  EXPECT_EQ(row["fee"], "5.160003");
  EXPECT_EQ(row["fees.commission"], "3.00");
  EXPECT_EQ(row["fees.clearing"], "0.30");
  EXPECT_EQ(row["fees.regulatory"], "0.06");
  EXPECT_EQ(row["fees.index"], "1.800003");
  check_columns(json::parse(server::handle_api({"GET", "/api/fills"}, source).body)["fills"][0], {columns.begin(), columns.end()});
}

TEST(TradeReviewApi, CsvQuotesNotesAndKeepsColumnsAndExactMoneyWithEmptyHistory) {
  test::ScriptedMarket f;
  trading::SessionConfig config; config.fee_per_contract = Money::parse("0.650001");
  trading::TradingSession s(config, f.time); f.seed(s);
  ReviewSource source; source.publish(s);
  const auto empty = server::handle_api({"GET", "/api/trades.csv"}, source);
  ASSERT_EQ(empty.status, 200);
  const auto header = csv_rows(empty.body).front();
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("close", 1, trading::Side::Sell), f.time).decision.ok());
  const std::string note = "Wait, then \"go\"\nReview the exit.";
  ASSERT_TRUE(s.annotate(1, note, {"setup", "patient"}, f.time).decision.ok());
  source.publish(s);
  const auto result = server::handle_api({"GET", "/api/trades.csv?account=main&from=2026-09-22&to=2026-09-22"}, source);
  ASSERT_EQ(result.status, 200) << result.body;
  EXPECT_EQ(result.content_type, "text/csv; charset=utf-8"); EXPECT_EQ(result.download, "trades.csv");
  const auto rows = csv_rows(result.body);
  ASSERT_EQ(rows.size(), 2U); EXPECT_EQ(rows.front(), header);
  const auto row = csv_record(header, rows[1]);
  EXPECT_EQ(row["note"], note); EXPECT_EQ(row["tags"], "setup;patient");
  EXPECT_EQ(row["fees"], "1.300002"); EXPECT_EQ(row["net"], "-21.300002");
  EXPECT_EQ(row["new_york_date"], "2026-09-22"); EXPECT_EQ(row["prices"], "simulated");
  EXPECT_EQ(row["entry_context.equity"], "100000.00");
  const auto trade = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"][0];
  check_columns(trade, {header.begin(), header.end()});
  const auto filtered = server::handle_api({"GET", "/api/trades.csv?to=2026-09-21"}, source);
  EXPECT_EQ(csv_rows(filtered.body).size(), 1U);
  EXPECT_EQ(csv_rows(filtered.body).front(), header);
  for (const auto* suffix : {"from=2026-02-30", "to=bad", "from=2026-09-23&to=2026-09-22", "from=2026-09-22&from=2026-09-22", "unknown=1"})
    EXPECT_EQ(server::handle_api({"GET", std::string("/api/trades.csv?") + suffix}, source).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/fills.csv?account=missing"}, source).status, 404);
  const auto fills = server::handle_api({"GET", "/api/fills.csv"}, source);
  const auto filled = csv_rows(fills.body);
  ASSERT_EQ(filled.size(), 3U);
  check_columns(json::parse(server::handle_api({"GET", "/api/fills"}, source).body)["fills"][0], {filled[0].begin(), filled[0].end()});
  EXPECT_EQ(csv_record(filled[0], filled[1])["fee"], "0.650001");
  // Each fill carries the book it took: the newest, a sale, left all ten bids.
  const auto book = csv_record(filled[0], filled[1]);
  EXPECT_EQ(book["quote.bid"], "4.00"); EXPECT_EQ(book["quote.ask"], "4.20");
  EXPECT_EQ(book["quote.bid_size"], "10"); EXPECT_EQ(book["quote.size_left"], "10");
  EXPECT_EQ(book["quote.observation"], "1"); EXPECT_EQ(book["quote.quoted_at"], "2026-09-22T14:00:00.000Z");
  EXPECT_EQ(book["quote.age_seconds"], "0.0");
}

TEST(TradeReviewApi, GivesBackProfitAndReportsRAndHeatOnlyWithKnownRisk) {
  test::ScriptedMarket f;
  trading::TradingSession s({}, f.time); f.seed(s);
  auto order = f.market("open", 2);
  order.bracket = trading::Bracket{trading::ExitSpec{trading::Trigger{trading::TriggerSource::Option,
      trading::TriggerDirection::AtOrBelow, Money::parse("3")}, {}}, {}};
  ASSERT_TRUE(s.submit(order, f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("6.00", "6.20")}, {f.valuation()}, f.time);
  f.next(); s.on_quotes({f.quote("5.00", "5.20")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("close", 2, trading::Side::Sell), f.time).decision.ok());
  ReviewSource source; source.publish(s);
  const auto trade = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"][0];
  const auto review = trade["review"];
  EXPECT_EQ(review["mae"], "21.30"); EXPECT_EQ(review["mfe"], "378.70");
  EXPECT_EQ(review["give_back"], "221.30"); EXPECT_EQ(review["planned_risk"], "240.00");
  EXPECT_NEAR(review["r_multiple"].get<double>(), 157.4 / 240, 1e-12);
  EXPECT_NEAR(review["heat"].get<double>(), 21.3 / 240, 1e-12);
  ASSERT_TRUE(s.submit(f.market("unplanned"), f.time).decision.ok());
  source.publish(s);
  const auto unknown = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"][0]["review"];
  EXPECT_TRUE(unknown["planned_risk"].is_null()); EXPECT_TRUE(unknown["r_multiple"].is_null()); EXPECT_TRUE(unknown["heat"].is_null());
}

TEST(TradeReviewApi, CsvDateUsesNewYorkCalendarDateIncludingDst) {
  const json fills = json::array({{{"time", "2026-03-09T01:00:00Z"}, {"id", "1"}}, {{"time", "2026-11-02T01:00:00Z"}, {"id", "2"}}});
  const auto rows = csv_rows(server::paper_csv(fills, true, "practice", "cboe", 1, "", ""));
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(csv_record(rows[0], rows[1])["new_york_date"], "2026-03-08");
  EXPECT_EQ(csv_record(rows[0], rows[2])["new_york_date"], "2026-11-01");
  EXPECT_EQ(csv_record(rows[0], rows[1])["prices"], "price provenance unrecorded; simulated trading");
  EXPECT_EQ(server::csv_quote("a\rb"), "\"a\rb\"");
  const std::vector<std::string> fill_columns = {"account", "account_version", "provider", "prices", "new_york_date",
      "run_id", "scenario", "seed", "recording", "id", "attempt", "order_id", "actor", "symbol", "underlying", "side", "quantity", "price", "fee", "quote_time", "time", "context.spot",
      "context.spot_source", "context.iv", "context.delta", "context.years", "context.equity", "context.floor_room", "context.buying_power",
      "quote.observation", "quote.bid", "quote.ask", "quote.bid_size", "quote.ask_size", "quote.size_left", "quote.quoted_at",
      "quote.age_seconds", "fees.commission", "fees.clearing", "fees.regulatory", "fees.index"};
  EXPECT_EQ(server::paper_csv_columns(true), fill_columns);
  const auto trade_columns = server::paper_csv_columns(false);
  ASSERT_EQ(trade_columns.size(), 108U);
  EXPECT_EQ(trade_columns[105], "settlement_value");
  EXPECT_EQ(trade_columns[106], "settlement_source");
  EXPECT_EQ(trade_columns[107], "time_stop_orders");
}
TEST(TradeReviewApi, ExportsNameTheirRunAttemptAndClosingTradingDay) {
  test::ScriptedMarket f;
  trading::TradingSession s({}, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("first"), f.time).decision.ok());
  ASSERT_TRUE(s.reset_account(Money::parse("100000"), {}, "again", f.time).decision.ok());
  f.next(); s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("second"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("close", 1, trading::Side::Sell), f.time).decision.ok());
  ReviewSource source; source.publish(s);
  source.view->run = server::RunIdentity{"reversal-2026-09-22-7", "reversal", "7", "", "2026-09-22"};
  const auto body = json::parse(server::handle_api({"GET", "/api/trades?attempt=all"}, source).body);
  EXPECT_EQ(body["run"], json({{"id", "reversal-2026-09-22-7"}, {"scenario", "reversal"}, {"seed", "7"}, {"recording", nullptr},
                               {"date", "2026-09-22"}}));
  ASSERT_EQ(body["trades"].size(), 2U);
  EXPECT_EQ(body["trades"][0]["trading_day"], "2026-09-22");
  EXPECT_EQ(body["trades"][1]["trading_day"], "2026-09-22");  // the reset closed it
  const auto fills = json::parse(server::handle_api({"GET", "/api/fills"}, source).body)["fills"];
  ASSERT_EQ(fills.size(), 3U);
  EXPECT_EQ(fills[0]["attempt"], 2); EXPECT_EQ(fills[2]["attempt"], 1);
  const auto rows = csv_rows(server::handle_api({"GET", "/api/fills.csv"}, source).body);
  ASSERT_EQ(rows.size(), 4U);
  const auto newest = csv_record(rows[0], rows[1]), oldest = csv_record(rows[0], rows[3]);
  EXPECT_EQ(newest["attempt"], "2"); EXPECT_EQ(oldest["attempt"], "1");
  EXPECT_EQ(newest["run_id"], "reversal-2026-09-22-7"); EXPECT_EQ(newest["scenario"], "reversal");
  for (std::size_t i = 1; i < rows.size(); ++i)
    EXPECT_EQ(csv_record(rows[0], rows[i])["account"], body["run"]["id"]);
  EXPECT_EQ(newest["seed"], "7"); EXPECT_EQ(newest["recording"], "");
  const auto trades = csv_rows(server::handle_api({"GET", "/api/trades.csv"}, source).body);
  for (std::size_t i = 1; i < trades.size(); ++i)
    EXPECT_EQ(csv_record(trades[0], trades[i])["account"], body["run"]["id"]);
  EXPECT_EQ(csv_record(trades[0], trades[1])["trading_day"], "2026-09-22");
  EXPECT_EQ(csv_record(trades[0], trades[1])["seed"], "7");
  // A live account leaves the run columns empty.
  source.view->run.reset();
  const auto live = csv_rows(server::handle_api({"GET", "/api/trades.csv"}, source).body);
  EXPECT_EQ(csv_record(live[0], live[1])["run_id"], "");
  EXPECT_EQ(csv_record(live[0], live[1])["account"], "main");
  EXPECT_TRUE(json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["run"].is_null());
}
TEST(TradeReviewApi, ClosedTradesSayWhetherAStopAFlattenOrALiquidationClosedThem) {
  test::ScriptedMarket f, g; g.contract.strike += 10;
  trading::SessionConfig config;
  config.initial_cash = Money::parse("10000");
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  config.rules.plan = "test"; config.rules.profit_target = Money::parse("1000"); config.rules.max_drawdown = Money::parse("300");
  trading::TradingSession s(config, f.time); f.seed(s); g.seed(s, "2.00", "2.20");
  const auto stopped = [&](std::string client) {
    auto order = f.market(std::move(client));
    order.bracket = trading::Bracket{trading::ExitSpec{trading::Trigger{trading::TriggerSource::Option,
        trading::TriggerDirection::AtOrBelow, Money::parse("3.95")}, {}}, {}};
    return order;
  };
  ASSERT_TRUE(s.submit(stopped("flattened with its stop"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(g.market("flattened"), f.time).decision.ok());
  ASSERT_TRUE(s.close_positions(std::string("SPX"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("manual"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("manual close", 1, trading::Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("liquidated", 5), f.time).decision.ok());
  f.next(); g.next();
  s.on_quotes({f.quote("3.40", "3.50"), g.quote("2.00", "2.20")}, {f.valuation(), g.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->evaluation.status, trading::EvaluationStatus::Failed);
  ASSERT_TRUE(s.snapshot()->positions.empty());
  ReviewSource source; source.publish(s);
  const auto trades = json::parse(server::handle_api({"GET", "/api/trades?status=closed"}, source).body)["trades"];
  ASSERT_EQ(trades.size(), 4U);  // newest first
  EXPECT_EQ(trades[0]["closed_by"], "system"); EXPECT_EQ(trades[0]["system_reason"], "drawdown");
  EXPECT_EQ(trades[1]["closed_by"], "order"); EXPECT_TRUE(trades[1]["system_reason"].is_null());
  EXPECT_EQ(trades[2]["closed_by"], "flatten"); EXPECT_EQ(trades[3]["closed_by"], "flatten");
  ASSERT_TRUE(s.reset_account(Money::parse("10000"), config.rules, "again", f.time).decision.ok());
  f.next(); s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(stopped("stopped"), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("3.90", "4.00")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.snapshot()->positions.empty());
  ASSERT_TRUE(s.submit(f.market("reset"), f.time).decision.ok());
  ASSERT_TRUE(s.reset_account(Money::parse("10000"), config.rules, "third", f.time).decision.ok());
  source.publish(s);
  const auto later = json::parse(server::handle_api({"GET", "/api/trades?status=closed&attempt=all"}, source).body)["trades"];
  ASSERT_EQ(later.size(), 6U);
  EXPECT_EQ(later[0]["closed_by"], "reset"); EXPECT_EQ(later[0]["closure"], "reset");
  EXPECT_EQ(later[1]["closed_by"], "stop_loss"); EXPECT_TRUE(later[1]["system_reason"].is_null());
  EXPECT_TRUE(json::parse(server::handle_api({"GET", "/api/trades?status=open"}, source).body)["trades"].empty());
}
TEST(TradeReviewApi, AnEveningCloseBeforeAHolidayCountsTowardTheNextSession) {
  test::ScriptedMarket f;
  f.time = md::new_york_to_utc({2026, 11, 25}, 10, 0);
  f.contract = *md::parse_osi("SPXW261127C05000000");
  trading::TradingSession s({}, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("scaled"), f.time).decision.ok());
  // 16:10 ET still trades for Wednesday; at 20:30 ET the overnight session trades for Friday, as
  // Thursday is Thanksgiving. A weekday count without holidays would say Thursday.
  f.time = md::new_york_to_utc({2026, 11, 25}, 16, 10); f.next();
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("close", 1, trading::Side::Sell), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 11, 25}, 20, 30); f.next();
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  const auto overnight = s.submit(f.limit("overnight close", 1, "4.00", trading::Side::Sell), f.time);
  ASSERT_TRUE(overnight.decision.ok()) << overnight.decision.message;
  ReviewSource source; source.publish(s);
  const auto trades = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"];
  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0]["status"], "closed");
  EXPECT_EQ(trades[0]["trading_day"], "2026-11-27");
}
TEST(TradeReviewApi, PositionsShowTheirRoundTripBesideTheContractsLifetimeAndClosedContractsKeepTheirDay) {
  test::ScriptedMarket f;
  trading::TradingSession s({}, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("4.60", "4.80")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.submit(f.market("close", 2, trading::Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("again"), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("4.50", "4.70")}, {f.valuation()}, f.time);
  ReviewSource source; source.publish(s);
  auto portfolio = json::parse(server::handle_api({"GET", "/api/portfolio"}, source).body);
  const auto trades = json::parse(server::handle_api({"GET", "/api/trades"}, source).body)["trades"];
  ASSERT_EQ(trades.size(), 2U);
  const auto& first = trades[1];
  const auto& second = trades[0];
  ASSERT_EQ(portfolio["positions"].size(), 1U);
  const auto& position = portfolio["positions"][0];
  // The row's realised and fees are its round trip's; the lifetime adds the earlier one.
  EXPECT_EQ(position["trade"], second["id"]);
  EXPECT_EQ(position["fees"], second["fees"]);
  EXPECT_EQ(position["lifetime"]["round_trips"], 2);
  EXPECT_EQ(position["lifetime"]["realised"], first["gross"]);
  EXPECT_EQ(Money::parse(position["lifetime"]["fees"].get<std::string>()),
            Money::parse(first["fees"].get<std::string>()) + Money::parse(second["fees"].get<std::string>()));
  // Each round trip's P&L by Greek adds up to its own P&L.
  EXPECT_NEAR(first["attribution"]["total"].get<double>(), std::stod(first["net"].get<std::string>()), 1e-6);
  EXPECT_NEAR(second["attribution"]["total"].get<double>(),
              std::stod(second["net"].get<std::string>()) + std::stod(second["unrealised"].get<std::string>()), 1e-6);
  EXPECT_TRUE(portfolio["closed"].empty());
  ASSERT_TRUE(s.submit(f.market("flat", 1, trading::Side::Sell), f.time).decision.ok());
  source.publish(s);
  portfolio = json::parse(server::handle_api({"GET", "/api/portfolio"}, source).body);
  EXPECT_TRUE(portfolio["positions"].empty());
  // A contract closed today keeps the day's P&L by Greek.
  ASSERT_EQ(portfolio["closed"].size(), 1U);
  EXPECT_EQ(portfolio["closed"][0]["symbol"], f.symbol());
  EXPECT_EQ(portfolio["closed"][0]["kind"], "option");
  EXPECT_EQ(portfolio["closed"][0]["underlying"], "SPX");
  EXPECT_EQ(portfolio["closed"][0]["attribution"], portfolio["attribution"]);
}
TEST(TradeReviewApi, PnlByGreekPartsAddUpToTheTotalInMicroDollars) {
  const auto micros = [](const json& value) { return std::llround(value.get<double>() * 1e6); };
  const char* keys[] = {"delta", "gamma", "vega", "theta", "other", "costs"};
  const auto check = [&](const trading::Attribution& a) {
    const auto j = server::attribution_json(a);
    const double exact[] = {a.delta, a.gamma, a.vega, a.theta, a.other, a.costs};
    long long sum = 0;
    for (int i = 0; i < 6; ++i) {
      sum += micros(j[keys[i]]);
      EXPECT_LT(std::abs(j[keys[i]].get<double>() - exact[i]), 1e-6 + 1e-9) << keys[i];
    }
    EXPECT_EQ(sum, micros(j["total"]));
    EXPECT_EQ(micros(j["total"]), std::llround(a.total() * 1e6));
    return j;
  };
  // Rounded one by one, these would be 1.000001 three times against a total of 3.000004.
  auto j = check({1.0000013, 1.0000013, 1.0000013, 0, 0, 0});
  EXPECT_EQ(j["delta"], 1.000002); EXPECT_EQ(j["gamma"], 1.000001); EXPECT_EQ(j["vega"], 1.000001); EXPECT_EQ(j["total"], 3.000004);
  j = check({-1.0000013, -1.0000013, -1.0000013, 0, 0, 0});
  EXPECT_EQ(j["delta"], -1.000001); EXPECT_EQ(j["gamma"], -1.000001); EXPECT_EQ(j["vega"], -1.000002); EXPECT_EQ(j["total"], -3.000004);
  // Sub-cent parts keep their micro-dollars, as the account's money does.
  j = check({-313.504123, 43.404, -7.044, -18.224, 0.354, -15.666667});
  EXPECT_EQ(j["delta"], -313.504123); EXPECT_EQ(j["costs"], -15.666667); EXPECT_EQ(j["total"], -310.68079);
  EXPECT_EQ(j["fallback"], false);
  trading::Attribution fallback;
  fallback.other = 5;
  fallback.fallback = true;
  EXPECT_EQ(server::attribution_json(fallback)["fallback"], true);
  std::mt19937_64 random(7);
  std::uniform_real_distribution<double> part(-5000, 5000);
  for (int i = 0; i < 2000; ++i) check({part(random), part(random), part(random), part(random), part(random), part(random)});
}
}  // namespace
