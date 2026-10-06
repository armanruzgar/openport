#include "support/contract_capture.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <future>
#include <limits>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <tuple>
#include <vector>

#include "openport/server/api.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/web_policy.hpp"
#include "server/paper_json.hpp"
#include "server/event_calendar.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;

class PaperProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "scripted paper"; }
  md::Capabilities capabilities() const noexcept override {
    md::Capabilities result;
    result.delay = delay;
    return result;
  }
  void start(const md::Subscription&, md::EventSink& out) override { sink = &out; }
  void stop() override {}
  md::EventSink* sink = nullptr;
  std::chrono::seconds delay{0};
};
template <class F> bool wait_for(F predicate) {
  const auto end = std::chrono::steady_clock::now() + std::chrono::minutes(5);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}
server::ApiResponse write(server::MetricsSource& source, std::string method,
                           std::string target, json body = nullptr) {
  auto promise = std::make_shared<std::promise<server::ApiResponse>>();
  auto future = promise->get_future();
  server::ApiRequest request{std::move(method), std::move(target)};
  if (!body.is_null()) request.body = body.dump();
  request.content_type = "application/json";
  server::handle_api_async(request, source, [promise](auto response) { promise->set_value(std::move(response)); });
  if (future.wait_for(std::chrono::minutes(5)) != std::future_status::ready)
    throw std::runtime_error("Engine command did not complete");
  return future.get();
}
json read(const server::MetricsSource& source, std::string path) {
  const auto response = server::handle_api({"GET", std::move(path)}, source);
  EXPECT_EQ(response.status, 200) << response.body;
  return json::parse(response.body);
}
json order(const test::ScriptedMarket& market, std::string client = "one", std::string price = "4.00") {
  return {{"client_order_id", client}, {"symbol", market.symbol()}, {"side", "buy"},
          {"type", "limit"}, {"quantity", 1}, {"limit_price", price}, {"time_in_force", "day"}};
}
server::Engine::Options paper_options() {
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(0);
  options.analytics.fallback_rate = 0;
  options.clock = [] { return md::new_york_to_utc({2026, 9, 22}, 10, 0); };
  return options;
}
std::filesystem::path paper_path() {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-paper-" + std::to_string(md::now()));
  std::filesystem::create_directories(directory);
  return directory / "paper.jsonl";
}
class PaperEngine : public testing::Test {
 protected:
  void SetUp() override {
    engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, paper_options());
    engine->start();
    ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  }
  void seed(std::string bid = "4.00", std::string ask = "4.20", double size = 10) {
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    quote(std::move(bid), std::move(ask), size);
  }
  void quote(std::string bid = "4.00", std::string ask = "4.20", double size = 10) {
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    provider.sink->publish(md::OptionQuote{0, market.time, Money::parse(bid).dollars(), Money::parse(ask).dollars(), size, size});
    ASSERT_TRUE(wait_for([&] {
      const auto metrics = engine->metrics("SPX");
      return metrics && metrics->as_of == market.time && !metrics->slices.empty() &&
             metrics->slices[0].strikes[0].call.ask == Money::parse(ask).dollars();
    }));
    // Analytics publish before the account's view of the same quotes. Flush the
    // whole batch so seed() and later quote() calls return with both ready.
    engine->synchronize().get();
  }
  void expect_error(const server::ApiResponse& response, int status, const char* code) {
    EXPECT_EQ(response.status, status) << response.body;
    const auto body = json::parse(response.body);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"].size(), 5);
    EXPECT_EQ(body["error"]["code"], code);
    EXPECT_TRUE(body["error"]["message"].is_string());
  }
  PaperProvider provider;
  test::ScriptedMarket market;
  std::unique_ptr<server::Engine> engine;
};

TEST_F(PaperEngine, TwoStepProgressionAndProgramCosts) {
  seed();
  const auto capture = [&](const server::ApiResponse& response) {
    test::capture_contract("paper", "POST", "/api/account/reset", response);
    return response;
  };
  const auto reset = [&](const std::string& id) {
    return capture(write(*engine, "POST", "/api/account/reset", {{"plan", id}, {"reason", "step"}}));
  };
  expect_error(reset("two-step-verify-25k"), 422, "PLAN_LOCKED");
  expect_error(reset("two-step-funded-25k"), 422, "PLAN_LOCKED");
  EXPECT_EQ(write(*engine, "POST", "/api/accounts", {{"name", "Skip"}, {"plan", "two-step-verify-25k"}}).status, 400);
  ASSERT_EQ(reset("two-step-25k").status, 200);
  EXPECT_EQ(read(*engine, "/api/account")["costs"]["evaluation"], "100.00");
  ASSERT_EQ(reset("two-step-25k").status, 200);
  ASSERT_EQ(reset("two-step-25k").status, 200);
  auto limited = reset("two-step-25k");
  expect_error(limited, 409, "RESET_LIMIT");
  EXPECT_EQ(json::parse(limited.body)["error"]["actual"], 3);
  EXPECT_EQ(json::parse(limited.body)["error"]["limit"], 2);
  EXPECT_EQ(read(*engine, "/api/account")["costs"]["total"], "200.00");
  // A real four-day challenge pass, then a three-day verification pass.
  const auto trade_day = [&](int day, const std::string& bid, const std::string& ask) {
    market.time = md::new_york_to_utc({2026, 9, day}, 10, 0);
    quote();
    ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open-" + std::to_string(day), "4.20")).status, 201);
    market.next(); quote(bid, ask);
    auto close = order(market, "close-" + std::to_string(day), bid); close["side"] = "sell";
    ASSERT_EQ(write(*engine, "POST", "/api/orders", close).status, 201);
  };
  for (int day : {22, 23, 24}) { trade_day(day, "4.00", "4.20"); }
  trade_day(25, "30.00", "30.20");
  auto passed = read(*engine, "/api/account");
  ASSERT_EQ(passed["evaluation"]["status"], "passed");
  EXPECT_EQ(passed["next_plans"], json::array({"two-step-verify-25k"}));
  auto response = reset("two-step-verify-25k"); ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["rules"]["phase"], "verification");
  EXPECT_EQ(json::parse(response.body)["costs"]["resets_used"], 0);
  EXPECT_EQ(json::parse(response.body)["costs"]["total"], "200.00");
  // A read-back preset name cannot skip the next prerequisite either.
  auto locked_rules = read(*engine, "/api/plans")["plans"].back()["rules"];
  expect_error(capture(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", locked_rules}, {"reason", "skip"}})), 422, "PLAN_LOCKED");
  for (int day : {28, 29}) { trade_day(day, "4.00", "4.20"); }
  trade_day(30, "18.00", "18.20");
  passed = read(*engine, "/api/account");
  ASSERT_EQ(passed["evaluation"]["status"], "passed");
  EXPECT_EQ(passed["next_plans"], json::array({"two-step-funded-25k"}));
  response = reset("two-step-funded-25k"); ASSERT_EQ(response.status, 200) << response.body;
  const auto account = json::parse(response.body);
  EXPECT_EQ(account["costs"]["activation"], "50.00"); EXPECT_EQ(account["costs"]["net"], "-250.00");
  EXPECT_EQ(account["evaluation"]["equity"], "25000.00");
  EXPECT_EQ(account["attempts"].back()["rules"]["phase"], "verification");
  EXPECT_TRUE(account["next_plans"].empty());
}

TEST_F(PaperEngine, ProgramCostOverridesAndCustomVerification) {
  engine->stop();
  const auto directory = paper_path().parent_path();
  auto options = paper_options(); options.paper_accounts = directory / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  const auto created = write(*engine, "POST", "/api/accounts", {{"name", "Costs"}, {"plan", "two-step-25k"},
      {"evaluation_fee", "12.000001"}, {"reset_fee", "3.000002"}, {"max_resets", 1}});
  ASSERT_EQ(created.status, 201) << created.body;
  test::capture_contract("paper", "POST", "/api/accounts", created);
  const auto id = json::parse(created.body)["account"]["id"].get<std::string>();
  const auto purchased = read(*engine, "/api/account?account=" + id);
  EXPECT_EQ(purchased["costs"]["evaluation"], "12.000001");
  EXPECT_EQ(purchased["costs"]["resets_used"], 0);
  EXPECT_EQ(purchased["costs"]["resets_left"], 1);
  EXPECT_EQ(purchased["evaluation"]["equity"], "25000.00");
  auto restarted = write(*engine, "POST", "/api/account/reset?account=" + id, {{"plan", "two-step-25k"}, {"reason", "same"}});
  ASSERT_EQ(restarted.status, 200) << restarted.body;
  EXPECT_EQ(json::parse(restarted.body)["costs"]["total"], "15.000003");
  EXPECT_EQ(json::parse(restarted.body)["rules"]["max_resets"], 1);
  expect_error(write(*engine, "POST", "/api/account/reset?account=" + id, {{"plan", "two-step-25k"}, {"reason", "over"}}), 409, "RESET_LIMIT");
  // Explicitly changing fee terms or raising the allowance keeps the used count.
  restarted = write(*engine, "POST", "/api/account/reset?account=" + id,
      {{"plan", "two-step-25k"}, {"reason", "new terms"}, {"reset_fee", "4.000001"}, {"max_resets", 2}});
  ASSERT_EQ(restarted.status, 200) << restarted.body;
  EXPECT_EQ(json::parse(restarted.body)["costs"]["resets_used"], 2);
  EXPECT_EQ(json::parse(restarted.body)["costs"]["total"], "19.000004");
  auto response = write(*engine, "POST", "/api/account/reset", {{"plan", "eod-25k"}, {"reason", "purchase"},
      {"evaluation_fee", "1.000001"}, {"reset_fee", "2.000002"}, {"max_resets", 1}});
  ASSERT_EQ(response.status, 200) << response.body;
  auto account = json::parse(response.body);
  EXPECT_EQ(account["rules"]["plan_id"], "eod-25k");
  EXPECT_EQ(account["costs"]["total"], "1.000001");
  auto rules = account["rules"]; rules["phase"] = "verification"; rules["plan"] = "Custom verification";
  response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "25000"}, {"rules", rules}, {"reason", "custom"}});
  ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["rules"]["phase"], "verification");
  for (const auto* key : {"evaluation_fee", "reset_fee", "activation_fee"}) {
    expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "bad"}, {key, "-1"}}), 400, "INVALID_RULES");
  }
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "bad"}, {"max_resets", 1.5}}), 400, "INVALID_REQUEST");
  engine->stop();
  std::filesystem::remove_all(directory);
}

TEST_F(PaperEngine, InsideFillRulesAndWalkingOrdersRoundTrip) {
  engine->stop();
  const auto directory = paper_path().parent_path();
  auto options = paper_options();
  options.paper_accounts = directory / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  seed();
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"reason", "inside"}, {"plan", "practice"}, {"fill_model", "midpoint"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  EXPECT_EQ(read(*engine, "/api/account")["rules"]["inside_fill_percent"], 50);
  const auto filled = write(*engine, "POST", "/api/orders", order(market, "mid", "4.10"));
  ASSERT_EQ(filled.status, 201) << filled.body;
  EXPECT_EQ(json::parse(filled.body)["order"]["average_fill_price"], "4.10");
  auto request = order(market, "walk", "3.90");
  request["walk"] = {{"step", "0.10"}, {"seconds", 10}, {"limit", "4.20"}};
  const auto preview = write(*engine, "POST", "/api/orders/preview", request);
  ASSERT_EQ(preview.status, 200) << preview.body;
  EXPECT_EQ(json::parse(preview.body)["next_walk"]["limit_price"], "4.00");
  const auto placed = write(*engine, "POST", "/api/orders", request);
  ASSERT_EQ(placed.status, 201) << placed.body;
  const auto walking = json::parse(placed.body)["order"];
  EXPECT_EQ(walking["walk"], request["walk"]);
  EXPECT_EQ(walking["next_walk"]["limit_price"], "4.00");
  const auto path = "/api/orders/" + walking["id"].get<std::string>();
  auto changed = write(*engine, "PUT", path, {{"walk", {{"step", "0.10"}, {"seconds", 5}, {"limit", "4.10"}}}});
  ASSERT_EQ(changed.status, 200) << changed.body;
  EXPECT_EQ(json::parse(changed.body)["order"]["walk"]["seconds"], 5);
  changed = write(*engine, "PUT", path, {{"walk", nullptr}});
  ASSERT_EQ(changed.status, 200) << changed.body;
  const auto cleared = json::parse(changed.body)["order"];
  EXPECT_TRUE(cleared["walk"].is_null());
  EXPECT_TRUE(cleared["next_walk"].is_null());
  EXPECT_TRUE(cleared["changes"].back()["walk"].is_null());
  EXPECT_EQ(cleared["changes"].back()["previous"]["walk"]["seconds"], 5);
  for (const auto seconds : {0, 3601}) {
    request["client_order_id"] = "bad-" + std::to_string(seconds);
    request["walk"]["seconds"] = seconds;
    expect_error(write(*engine, "POST", "/api/orders", request), 422, "INVALID_ORDER");
  }
  request["walk"]["seconds"] = 1.5;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  auto rules = read(*engine, "/api/account")["rules"];
  rules["plan"] = "Custom inside";
  for (const auto& value : {json(-1), json(101), json(1.5)}) {
    rules["inside_fill_percent"] = value;
    const auto bad = write(*engine, "POST", "/api/account/reset", {{"reason", "bad"}, {"initial_cash", "100000.00"}, {"rules", rules}});
    expect_error(bad, 400, value.is_number_integer() ? "INVALID_RULES" : "INVALID_REQUEST");
  }
  const auto created = write(*engine, "POST", "/api/accounts", {{"name", "Inside"}, {"plan", "practice"}, {"fill_model", "midpoint"}});
  ASSERT_EQ(created.status, 201) << created.body;
  EXPECT_EQ(read(*engine, "/api/account?account=inside")["rules"]["inside_fill_percent"], 50);
  engine->stop();
  std::filesystem::remove_all(directory);
}

TEST_F(PaperEngine, AlertsWatchUntradedContractsAndUnderlyingStudiesAndKeepAccountScope) {
  // Named accounts require durable journals; the ordinary fixture is memory-only.
  engine.reset();
  const auto path = paper_path();
  auto options = paper_options();
  options.paper_journal = path.string();
  options.paper_accounts = path.parent_path() / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  seed();
  const json contract = {{"scope", "contract"}, {"metric", "bid"}, {"symbol", market.symbol()},
                         {"direction", "at_or_above"}, {"level", "4.50"}, {"label", "Bid up"}, {"repeat", true}};
  auto response = write(*engine, "POST", "/api/alerts", contract);
  ASSERT_EQ(response.status, 201) << response.body;
  auto alert = json::parse(response.body).at("alert");
  EXPECT_EQ(alert["id"], "1");
  EXPECT_TRUE(alert["armed"]);
  EXPECT_EQ(alert["fired"], 0);
  EXPECT_TRUE(alert["fired_at"].is_null());
  EXPECT_TRUE(read(*engine, "/api/orders")["orders"].empty());
  market.next();
  quote("4.50", "4.70");
  alert = read(*engine, "/api/alerts")["alerts"][0];
  EXPECT_EQ(alert["fired"], 1);
  EXPECT_EQ(alert["value"], "4.50");
  EXPECT_FALSE(alert["armed"]);
  EXPECT_EQ(alert["fired_at"], md::format_timestamp(market.time));

  auto put = *md::parse_osi("SPXW261022P05000000");
  provider.sink->publish(md::ContractDefinition{1, put});
  provider.sink->publish(md::OptionQuote{1, market.time, 3, 3.2, 10, 10});
  engine->synchronize().get();
  response = write(*engine, "POST", "/api/alerts", {{"scope", "spread"}, {"metric", "mark"},
      {"legs", {{{"symbol", market.symbol()}, {"side", "buy"}, {"ratio", 1}},
                {{"symbol", put.osi_symbol()}, {"side", "sell"}, {"ratio", 2}}}},
      {"direction", "at_or_below"}, {"level", "0"}});
  ASSERT_EQ(response.status, 201) << response.body;
  EXPECT_EQ(json::parse(response.body)["alert"]["value"], "-1.60");
  response = write(*engine, "POST", "/api/alerts", {{"scope", "underlying"}, {"metric", "iv30"}, {"symbol", "SPX"},
                                                   {"direction", "at_or_above"}, {"level", "0"}});
  ASSERT_EQ(response.status, 201) << response.body;
  // This small chain cannot bracket a 30-day tenor; unavailable is not zero.
  EXPECT_EQ(json::parse(response.body)["alert"]["fired"], 0);
  response = write(*engine, "POST", "/api/alerts", {{"scope", "underlying"}, {"metric", "price"}, {"symbol", "SPX"},
                                                   {"direction", "at_or_above"}, {"level", "5000"}});
  ASSERT_EQ(response.status, 201) << response.body;
  EXPECT_EQ(json::parse(response.body)["alert"]["fired"], 1);
  response = write(*engine, "POST", "/api/alerts", {{"scope", "account"}, {"metric", "equity"},
                                                   {"direction", "at_or_above"}, {"level", "0"}});
  ASSERT_EQ(response.status, 201) << response.body;
  EXPECT_EQ(json::parse(response.body)["alert"]["value"], "100000.00");
  response = write(*engine, "POST", "/api/accounts", {{"name", "Other"}, {"plan", "practice"}});
  ASSERT_EQ(response.status, 201) << response.body;
  const auto other = json::parse(response.body)["account"]["id"].get<std::string>();
  EXPECT_TRUE(read(*engine, "/api/alerts?account=" + other)["alerts"].empty());
  expect_error(write(*engine, "DELETE", "/api/alerts/1?account=" + other), 404, "UNKNOWN_ALERT");
  response = write(*engine, "DELETE", "/api/alerts/1");
  EXPECT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["deleted"], "1");
  expect_error(write(*engine, "DELETE", "/api/alerts/1"), 404, "UNKNOWN_ALERT");
  engine.reset();
  std::filesystem::remove_all(path.parent_path());
}

TEST_F(PaperEngine, AlertsRejectMalformedBodiesAndRefuseUnknownContractsOrAFullBook) {
  seed();
  const json request = {{"scope", "account"}, {"metric", "day_pnl"}, {"direction", "at_or_below"}, {"level", "-500"}};
  for (const auto& [field, value] : std::vector<std::pair<std::string, json>>{
      {"scope", "portfolio"}, {"metric", "bid"}, {"symbol", "SPX"}, {"direction", "below"},
      {"level", 500}, {"repeat", "true"}, {"label", std::string(101, 'x')}, {"extra", true}}) {
    auto bad = request;
    bad[field] = value;
    expect_error(write(*engine, "POST", "/api/alerts", bad), 400, "INVALID_REQUEST");
  }
  auto missing = request;
  missing.erase("level");
  expect_error(write(*engine, "POST", "/api/alerts", missing), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "DELETE", "/api/alerts/0"), 404, "UNKNOWN_ALERT");
  expect_error(write(*engine, "DELETE", "/api/alerts/not-an-id"), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "DELETE", "/api/alerts/1", json::object()), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/alerts", {{"scope", "contract"}, {"metric", "bid"},
      {"symbol", "SPXW  261022C09000000"}, {"direction", "at_or_above"}, {"level", "1"}}), 404, "UNKNOWN_CONTRACT");
  for (int i = 0; i < 100; ++i) { ASSERT_EQ(write(*engine, "POST", "/api/alerts", request).status, 201); }
  expect_error(write(*engine, "POST", "/api/alerts", request), 422, "INVALID_ALERT");
}

TEST_F(PaperEngine, PreviewIsPureEvenBeforeContractRegistration) {
  seed();
  const auto before = engine->trading_view();
  const auto account_before = read(*engine, "/api/account");
  ASSERT_TRUE(before->contracts.empty());
  auto request = order(market, "preview-client", "4.20");
  request["floor_share"] = 0.5;
  const auto response = write(*engine, "POST", "/api/orders/preview", request);
  ASSERT_EQ(response.status, 200) << response.body;
  const auto preview = json::parse(response.body);
  EXPECT_EQ(preview["decision"], "ok");
  EXPECT_EQ(preview["max_loss"], "420.65");
  ASSERT_TRUE(preview.contains("max_units_basis"));
  EXPECT_TRUE(preview["max_units_basis"] == "limits" || preview["max_units_basis"] == "buying_power");
  EXPECT_TRUE(preview["simulated"]);
  EXPECT_EQ(engine->trading_view()->snapshot->account_version, before->snapshot->account_version);
  EXPECT_EQ(read(*engine, "/api/account"), account_before);
  EXPECT_TRUE(engine->trading_view()->contracts.empty());
  EXPECT_TRUE(engine->trading_view()->snapshot->recent_orders.empty());
  request.erase("floor_share");
  const auto submitted = write(*engine, "POST", "/api/orders", request);
  ASSERT_EQ(submitted.status, 201) << submitted.body;
  EXPECT_EQ(json::parse(submitted.body)["order"]["id"], "1");
  EXPECT_EQ(json::parse(submitted.body)["fills"].size(), 1U);
  request["floor_share"] = 0;
  expect_error(write(*engine, "POST", "/api/orders/preview", request), 400, "INVALID_REQUEST");
}

TEST_F(PaperEngine, AChangePreviewAnswersLikeTheChangeWithoutMakingIt) {
  seed();
  const auto placed = write(*engine, "POST", "/api/orders", order(market, "resting", "3.40"));
  ASSERT_EQ(placed.status, 201) << placed.body;
  const auto id = json::parse(placed.body)["order"]["id"].get<std::string>();
  const auto before = read(*engine, "/api/account");
  auto response = write(*engine, "POST", "/api/orders/" + id + "/preview", {{"limit_price", "4.20"}, {"floor_share", 1}});
  ASSERT_EQ(response.status, 200) << response.body;
  const auto preview = json::parse(response.body);
  EXPECT_EQ(preview["decision"], "ok");
  EXPECT_TRUE(preview["simulated"]);
  EXPECT_EQ(preview["execution"]["status"], "filled");
  EXPECT_EQ(preview["execution"]["fills"][0]["price"], "4.20");
  EXPECT_TRUE(preview["buying_power"]["working"].is_string());
  // The order is untouched: still working at its old price, and nothing filled.
  const auto orders = read(*engine, "/api/orders")["orders"];
  EXPECT_EQ(orders[0]["status"], "working");
  EXPECT_EQ(orders[0]["limit_price"], "3.40");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  EXPECT_EQ(read(*engine, "/api/account"), before);
  expect_error(write(*engine, "POST", "/api/orders/99/preview", {{"limit_price", "4.20"}}), 404, "UNKNOWN_ORDER");
  expect_error(write(*engine, "POST", "/api/orders/" + id + "/preview", json::object()), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/orders/" + id + "/preview", {{"limit_price", "4.20"}, {"floor_share", 2}}), 400, "INVALID_REQUEST");
}

TEST_F(PaperEngine, WhatIfComparesCandidatesWithoutTrading) {
  seed();
  const auto before = read(*engine, "/api/account");
  auto buy = order(market, "ignored", "4.20");
  buy.erase("client_order_id");
  auto sell = buy;
  sell["side"] = "sell";
  const json candidates = json::array({{{"name", "Buy one"}, {"orders", json::array({buy})}},
                                       {{"orders", json::array({buy, sell})}}});
  const auto response = write(*engine, "POST", "/api/orders/what-if", {{"candidates", candidates}});
  ASSERT_EQ(response.status, 200) << response.body;
  const auto result = json::parse(response.body);
  EXPECT_TRUE(result["simulated"]);
  EXPECT_TRUE(result["current"]["equity"].is_string());
  ASSERT_EQ(result["candidates"].size(), 2U);
  EXPECT_EQ(result["candidates"][0]["name"], "Buy one");
  EXPECT_EQ(result["candidates"][0]["decision"], "ok");
  EXPECT_TRUE(result["candidates"][0]["after"]["max_loss"].is_string());
  EXPECT_EQ(result["candidates"][1]["name"], "Candidate 2");
  EXPECT_EQ(result["candidates"][1]["orders"].size(), 2U);
  // A round trip leaves the book as it was, less what it cost.
  EXPECT_EQ(result["candidates"][1]["after"]["exposure"]["dollar_delta"], 0);
  EXPECT_TRUE(read(*engine, "/api/orders")["orders"].empty());
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  EXPECT_EQ(read(*engine, "/api/account"), before);
  expect_error(write(*engine, "POST", "/api/orders/what-if", {{"candidates", json::array()}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/orders/what-if", {{"candidates", json::array({{{"orders", json::array()}}})}}), 400, "INVALID_REQUEST");
  auto broken = buy;
  broken.erase("quantity");
  const auto refused = write(*engine, "POST", "/api/orders/what-if", {{"candidates", json::array({{{"orders", json::array({buy, broken})}}})}});
  expect_error(refused, 400, "INVALID_REQUEST");
  EXPECT_NE(json::parse(refused.body)["error"]["message"].get<std::string>().find("candidates[0].orders[1]"), std::string::npos) << refused.body;
}

TEST_F(PaperEngine, WhatIfRefusesOrderChainsWithoutTrading) {
  seed();
  auto buy = order(market, "candidate", "4.20");
  auto next = order(market, "", "3.90");
  next.erase("client_order_id");
  const auto before = read(*engine, "/api/account");
  for (const auto* key : {"then", "oco"}) {
    for (const auto& terms : {next, json(nullptr)}) {
      auto chained = buy;
      chained[key] = terms;
      const auto response = write(*engine, "POST", "/api/orders/what-if",
          {{"candidates", json::array({{{"orders", json::array({buy, chained})}}})}});
      expect_error(response, 400, "INVALID_REQUEST");
      const auto message = json::parse(response.body)["error"]["message"].get<std::string>();
      EXPECT_NE(message.find("candidates[0].orders[1]"), std::string::npos);
      EXPECT_NE(message.find("What-if does not model order chains"), std::string::npos);
    }
  }
  EXPECT_EQ(read(*engine, "/api/account"), before);
  EXPECT_TRUE(read(*engine, "/api/orders")["orders"].empty());
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
}

TEST_F(PaperEngine, WhatIfProjectsAHeldContractExitWithoutTrading) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "entry", "4.20")).status, 201);
  auto exits = order(market, "", "5.00");
  exits.erase("client_order_id");
  exits["side"] = "sell";
  exits["time_in_force"] = "gtc";
  exits["exits_only"] = true;
  exits["bracket"] = {{"take_profit", {{"limit_price", "5.00"}}}};
  const auto before = read(*engine, "/api/account");
  const auto response = write(*engine, "POST", "/api/orders/what-if",
      {{"candidates", json::array({{{"orders", json::array({exits})}}})}});
  ASSERT_EQ(response.status, 200) << response.body;
  const auto candidate = json::parse(response.body)["candidates"][0];
  EXPECT_EQ(candidate["decision"], "ok");
  EXPECT_EQ(candidate["orders"][0]["decision"], "ok");
  ASSERT_FALSE(candidate["after"].is_null());
  EXPECT_EQ(candidate["after"]["exposure"]["dollar_delta"], 0);
  EXPECT_EQ(read(*engine, "/api/account"), before);
  EXPECT_EQ(read(*engine, "/api/orders")["orders"].size(), 1U);
  EXPECT_EQ(read(*engine, "/api/fills")["fills"].size(), 1U);
}


TEST_F(PaperEngine, GuardrailPercentValidationAndUnusedWarning) {
  seed();
  auto risk = read(*engine, "/api/risk");
  auto g = risk["guardrails"];
  g["soft_floor_percent"] = 100;
  const auto invalid = write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", g}});
  expect_error(invalid, 422, "INVALID_LIMITS");
  EXPECT_NE(json::parse(invalid.body)["error"]["message"].get<std::string>().find("latches at once"), std::string::npos);
  g["soft_floor_percent"] = 99;
  const auto okay = write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", g}});
  ASSERT_EQ(okay.status, 200) << okay.body;
  risk = json::parse(okay.body);
  const auto warning = risk["warnings"].back();
  EXPECT_EQ(warning["code"], "SOFT_FLOOR_UNUSED");
  EXPECT_EQ(warning["severity"], "info");
  EXPECT_EQ(risk["guardrail_state"]["soft_floor"], nullptr);
  EXPECT_EQ(read(*engine, "/api/account")["warnings"].back(), warning);
}

TEST_F(PaperEngine, AccountResetWarnsForRetainedAbsoluteFloorAndAppliesPendingFloor) {
  seed();
  auto risk = read(*engine, "/api/risk");
  auto g = risk["guardrails"]; g["soft_floor"] = "90000";
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", g}}).status, 200);
  auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "intraday-25k"}, {"reason", "smaller plan"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  auto account = json::parse(reset.body);
  EXPECT_EQ(account["guardrail_state"]["latched"], json::array({"SOFT_FLOOR"}));
  const auto warning = account["warnings"].back();
  EXPECT_EQ(warning["code"], "SOFT_FLOOR");
  EXPECT_NE(warning["message"].get<std::string>().find("latches at once"), std::string::npos);
  EXPECT_NE(warning["message"].get<std::string>().find("a reset applies pending settings"), std::string::npos);
  const auto kill = read(*engine, "/api/risk")["kill"];
  EXPECT_EQ(kill["reset_blocked"]["code"], "SOFT_FLOOR");
  EXPECT_NE(kill["reset_blocked"]["message"].get<std::string>().find("account reset"), std::string::npos);
  // Equality still warns, including when it comes from the pending setting.
  for (const auto floor : {"25000", "24000"}) {
    risk = read(*engine, "/api/risk"); g["soft_floor"] = floor;
    ASSERT_EQ(write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", g}}).status, 200);
    reset = write(*engine, "POST", "/api/account/reset", {{"plan", "intraday-25k"}, {"reason", "apply pending floor"}});
    ASSERT_EQ(reset.status, 200) << reset.body;
    account = json::parse(reset.body);
    if (std::string_view(floor) == "25000") { EXPECT_EQ(account["warnings"].back()["code"], "SOFT_FLOOR"); }
    else { EXPECT_TRUE(account["warnings"].empty()); EXPECT_TRUE(account["guardrail_state"]["latched"].empty()); }
  }
  const auto cleared = read(*engine, "/api/risk")["kill"];
  EXPECT_EQ(cleared["latched"], false);
  EXPECT_EQ(cleared["reset_blocked"], nullptr);
  EXPECT_EQ(cleared["history"].back()["action"], "reset");
  EXPECT_EQ(cleared["history"].back()["previous"], "SOFT_FLOOR");
}

TEST_F(PaperEngine, AccountResetKeepsTradeLimitThroughTheDesk) {
  seed();
  const auto risk = read(*engine, "/api/risk");
  auto g = risk["guardrails"]; g["max_opening_trades"] = 1;
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", g}}).status, 200);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "entry", "4.20")).status, 201);
  const auto before = read(*engine, "/api/risk")["kill"];
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "another attempt"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto account = json::parse(reset.body);
  EXPECT_EQ(account["guardrail_state"]["opening_trades"], 1);
  EXPECT_EQ(account["guardrail_state"]["latched"], json::array({"TRADE_LIMIT"}));
  const auto after = read(*engine, "/api/risk")["kill"];
  EXPECT_EQ(after["history"], before["history"]);
  EXPECT_EQ(after["reset_blocked"]["code"], "TRADE_LIMIT");
  EXPECT_NE(after["reset_blocked"]["message"].get<std::string>().find("neither a kill-switch reset nor an account reset"), std::string::npos);
  const auto refused = write(*engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "try to resume"}});
  expect_error(refused, 422, "TRADE_LIMIT");
  EXPECT_EQ(json::parse(refused.body)["error"]["message"], after["reset_blocked"]["message"]);
  expect_error(write(*engine, "POST", "/api/orders", order(market, "new-entry", "4.20")), 422, "TRADE_LIMIT");
}

TEST_F(PaperEngine, PendingLimitsGuardrailsAndBreachAreExposedWithRevisionChecks) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"plan", "eod-25k"}, {"reason", "evaluation"}}).status, 200);
  auto risk = read(*engine, "/api/risk");
  auto limits = risk["limits"];
  limits["max_order_contracts"] = 200;
  auto response = write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", risk["limits_revision"]}, {"limits", limits}});
  ASSERT_EQ(response.status, 200) << response.body;
  risk = json::parse(response.body);
  EXPECT_EQ(risk["limits"]["max_order_contracts"], 100);
  EXPECT_EQ(risk["pending_limits"]["max_order_contracts"], 200);
  EXPECT_EQ(risk["pending_effective"], "next_trading_day");
  auto guardrails = risk["guardrails"];
  guardrails["max_opening_trades"] = 1;
  response = write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", guardrails}});
  ASSERT_EQ(response.status, 200) << response.body;
  expect_error(write(*engine, "PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", guardrails}}), 409, "LIMITS_REVISION");
  auto opening = order(market, "entry", "4.20");
  ASSERT_EQ(write(*engine, "POST", "/api/orders", opening).status, 201);
  const auto account = read(*engine, "/api/account");
  EXPECT_EQ(account["guardrail_state"]["latched"][0], "TRADE_LIMIT");
  EXPECT_TRUE(account["breach"]["room"].is_string());
  EXPECT_EQ(account["breach"]["underlyings"].size(), 1U);
  EXPECT_TRUE(account["evaluation"]["day_low_equity"].is_string());
  opening["client_order_id"] = "blocked";
  const auto preview = write(*engine, "POST", "/api/orders/preview", opening);
  ASSERT_EQ(preview.status, 200);
  EXPECT_EQ(json::parse(preview.body)["decision"], "TRADE_LIMIT");
  expect_error(write(*engine, "POST", "/api/orders", opening), 422, "TRADE_LIMIT");
  // A limit tightened below the held book's delta is warned on the risk and account views.
  // Within 1% of the 1,000,000 limit by gamma, the call is close to it already.
  risk = read(*engine, "/api/risk");
  ASSERT_EQ(risk["warnings"].size(), 1U);
  EXPECT_EQ(risk["warnings"][0]["code"], "DELTA_HEADROOM");
  limits = risk["limits"];
  limits["per_underlying"]["dollar_delta"] = 1;
  response = write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", risk["limits_revision"]}, {"limits", limits}});
  ASSERT_EQ(response.status, 200) << response.body;
  const auto warning = read(*engine, "/api/risk")["warnings"][0];
  EXPECT_EQ(warning["code"], "DELTA_LIMIT");
  EXPECT_EQ(warning["severity"], "warning");
  EXPECT_EQ(warning["scope"], "SPX");
  EXPECT_TRUE(warning["symbol"].is_null());
  EXPECT_EQ(warning["limit"], 1);
  EXPECT_EQ(read(*engine, "/api/account")["warnings"][0], warning);
  EXPECT_EQ(read(*engine, "/api/account/equity")["samples"].size(), 0U); // no journal, no history
  EXPECT_EQ(server::handle_api({"GET", "/api/account/equity?from=bad"}, *engine).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/account/equity?from=2026-09-23T00:00:00Z&to=2026-09-22T00:00:00Z"}, *engine).status, 400);
}

class PaperFeed : public PaperEngine {
 protected:
  void TearDown() override { engine->stop(); }
  void SetUp() override {
    provider.delay = std::chrono::minutes(15);
    auto options = paper_options();
    options.clock = [this] { return wall_now.load(); };
    engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX", "XSP"}}, options);
    engine->start();
    ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  }
  json paper_status() {
    // The engine's data clock shows the batch was processed; an idle account
    // records no transaction for it, so the account's own time may stay behind.
    EXPECT_TRUE(wait_for([&] {
      const auto view = engine->trading_view();
      const auto time = view->market_times.find(market.contract.underlying);
      return time != view->market_times.end() && time->second >= market.time;
    }));
    const auto status = read(*engine, "/api/status")["underlyings"][0]["paper"];
    EXPECT_EQ(json::parse(server::tick_message(*engine))["underlyings"][0]["paper"], status);
    return status;
  }
  void expect_gate(std::string client, const char* code, const char* message = nullptr) {
    const auto paper = paper_status();
    EXPECT_EQ(paper["accepting"], false);
    EXPECT_EQ(paper["reason"], code);
    if (message) {
      EXPECT_EQ(paper["message"], message);
    }
    const auto response = write(*engine, "POST", "/api/orders", order(market, std::move(client)));
    expect_error(response, 422, code);
    EXPECT_EQ(json::parse(response.body)["error"]["message"], paper["message"]);
    EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["reason"]["code"], code);
  }
  std::atomic<md::Timestamp> wall_now{md::new_york_to_utc({2026, 9, 22}, 10, 15)};
};

TEST_F(PaperFeed, OvernightStallDuringRegularSessionAgreesWithStatusAndTick) {
  market.time = md::new_york_to_utc({2026, 9, 21}, 23, 45);
  wall_now = md::new_york_to_utc({2026, 9, 22}, 10, 5);
  seed();
  EXPECT_EQ(read(*engine, "/api/status")["underlyings"][0]["session"]["name"], "regular");
  expect_gate("stalled", "FEED_STALLED",
              "SPX quotes are 10h 20m behind the market; the feed appears to have stalled");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  EXPECT_EQ(engine->trading_view()->snapshot->time, market.time);
}

TEST_F(PaperFeed, FirstMinutesOfEachDelayedSessionRemainSessionClosed) {
  // SPX's overnight session opens at 20:15 and its regular one at 09:30; a
  // 15-minute delayed feed still shows the closed market just after each.
  const std::vector<std::tuple<md::Date, int, int, int, std::string>> openings{
      {{2026, 9, 21}, 20, 0, 15, "overnight"}, {{2026, 9, 22}, 9, 25, 5, "regular"}};
  bool seeded = false;
  for (const auto& [date, hour, minute, closed, name] : openings) {
    for (int i = 0; i < closed; ++i) {
      market.time = md::new_york_to_utc(date, hour, minute + i);
      wall_now = market.time + 15 * md::kNanosPerMinute;
      if (seeded) quote(); else seed();
      seeded = true;
      expect_gate(name + "-" + std::to_string(i), "SESSION_CLOSED");
    }
    market.time = md::new_york_to_utc(date, hour, minute + closed);
    wall_now = market.time + 15 * md::kNanosPerMinute;
    quote();
    EXPECT_EQ(paper_status(), (json{{"accepting", true}, {"reason", nullptr}, {"message", nullptr},
                                    {"session", name == "overnight" ? "global" : "regular"}}));
    const auto accepted = write(*engine, "POST", "/api/orders", order(market, name, "4.20"));
    ASSERT_EQ(accepted.status, 201) << accepted.body;
    EXPECT_EQ(json::parse(accepted.body)["order"]["status"], "filled");
  }
}

TEST_F(PaperFeed, ClosedMarketsDoNotReportStall) {
  // After the curb session, and over a weekend, a healthy feed sits at the last close.
  market.time = md::new_york_to_utc({2026, 9, 22}, 17, 0);
  wall_now = md::new_york_to_utc({2026, 9, 22}, 19, 0);
  seed();
  expect_gate("evening", "SESSION_CLOSED", "SPX options are closed (between sessions)");
  market.time = md::new_york_to_utc({2026, 9, 25}, 17, 0);
  wall_now = md::new_york_to_utc({2026, 9, 26}, 12, 0);
  quote();
  expect_gate("weekend", "SESSION_CLOSED", "SPX options are closed (weekend)");
}

TEST_F(PaperFeed, AFeedThatStopsInsideASessionIsStalledOnceItShouldHaveEnded) {
  market.time = md::new_york_to_utc({2026, 9, 22}, 16, 50);  // curb
  wall_now = md::new_york_to_utc({2026, 9, 22}, 18, 0);
  seed();
  expect_gate("frozen-curb", "FEED_STALLED",
              "SPX quotes are 1h 10m behind the market; the feed appears to have stalled");
}

TEST_F(PaperFeed, AnotherUnderlyingCannotHideAStalledFeed) {
  market.time = md::new_york_to_utc({2026, 9, 21}, 23, 45);
  wall_now = md::new_york_to_utc({2026, 9, 22}, 10, 5);
  seed();
  test::ScriptedMarket fresh;
  fresh.contract = *md::parse_osi("XSP261022C00500000");
  fresh.time = wall_now.load() - 15 * md::kNanosPerMinute;
  provider.sink->publish(md::ContractDefinition{1, fresh.contract});
  provider.sink->publish(md::UnderlyingQuote{"XSP", fresh.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{1, fresh.time, 4, 4.2, 10, 10});
  ASSERT_TRUE(wait_for([&] {
    const auto view = engine->trading_view();
    const auto time = view->market_times.find("XSP");
    return time != view->market_times.end() && time->second >= fresh.time;
  }));
  const auto underlyings = read(*engine, "/api/status")["underlyings"];
  ASSERT_EQ(underlyings.size(), 2);
  EXPECT_EQ(underlyings[1]["symbol"], "XSP");
  EXPECT_EQ(underlyings[1]["paper"]["accepting"], true);
  expect_gate("stalled-spx", "FEED_STALLED");
  const auto accepted = write(*engine, "POST", "/api/orders", order(fresh, "fresh-xsp"));
  EXPECT_EQ(accepted.status, 201) << accepted.body;
}

TEST_F(PaperFeed, StallThresholdUsesProviderDelayAndActiveQuoteAge) {
  seed();
  // A delayed feed stalls past its delay plus the larger of max_quote_age and three minutes.
  wall_now = market.time + 18 * md::kNanosPerMinute;
  EXPECT_EQ(paper_status()["accepting"], true);
  EXPECT_EQ(write(*engine, "POST", "/api/orders", order(market, "boundary")).status, 201);
  wall_now.fetch_add(1);
  expect_gate("over-boundary", "FEED_STALLED");
  auto risk = read(*engine, "/api/risk");
  risk["limits"]["max_quote_age_seconds"] = 240;
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits",
      {{"expected_revision", risk["limits_revision"]}, {"limits", risk["limits"]}}).status, 200);
  EXPECT_EQ(paper_status()["accepting"], true);
  EXPECT_EQ(write(*engine, "POST", "/api/orders", order(market, "larger-age")).status, 201);
}

TEST_F(PaperFeed, StalledRestingOrderWaitsForFreshQuotesWithoutCancellation) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "resting")).status, 201);
  wall_now = market.time + 19 * md::kNanosPerMinute;
  market.next();
  quote("3.80", "4.00");  // a newly received but stale crossing quote
  expect_gate("stalled", "FEED_STALLED");
  const auto resting = read(*engine, "/api/orders?status=open")["orders"];
  ASSERT_EQ(resting.size(), 1);
  EXPECT_EQ(resting[0]["status"], "working");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  EXPECT_EQ(engine->trading_view()->snapshot->time, market.time);
  market.time = wall_now.load() - 15 * md::kNanosPerMinute;
  quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return !engine->trading_view()->snapshot->recent_fills.empty(); }));
  EXPECT_EQ(paper_status()["accepting"], true);
  EXPECT_EQ(read(*engine, "/api/fills")["fills"][0]["order_id"], "1");
}

TEST_F(PaperEngine, RestingLimitFillsOnlyOnLaterObservationAndPublishesContractJson) {
  seed();
  const auto response = write(*engine, "POST", "/api/orders", order(market));
  ASSERT_EQ(response.status, 201) << response.body;
  const auto accepted = json::parse(response.body);
  EXPECT_EQ(accepted["order"]["status"], "working");
  EXPECT_TRUE(accepted["fills"].empty());
  const auto immutable = engine->trading_view();
  EXPECT_EQ(read(*engine, "/api/orders?status=open")["orders"].size(), 1);
  market.next(); quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return !engine->trading_view()->snapshot->recent_fills.empty(); }));
  const auto portfolio = read(*engine, "/api/portfolio");
  EXPECT_EQ(portfolio["cash"], "99599.35");
  EXPECT_EQ(portfolio["fees"], "0.65");
  EXPECT_EQ(portfolio["equity"], "99989.35");
  EXPECT_EQ(portfolio["positions"][0]["average_price"], "4.00");
  EXPECT_EQ(portfolio["positions"][0]["mark"], "3.90");
  EXPECT_TRUE(portfolio["positions"][0]["greeks"]["delta"].is_number());
  EXPECT_TRUE(immutable->snapshot->positions.empty());
  const auto fills = read(*engine, "/api/fills");
  EXPECT_EQ(fills["fills"][0]["price"], "4.00");
  EXPECT_EQ(fills["fills"][0]["id"], "1");
  EXPECT_EQ(fills["fills"][0]["quote_time"], md::format_timestamp(market.time));
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "filled");
  const auto status = read(*engine, "/api/status")["trading"];
  EXPECT_EQ(status["account_version"], portfolio["account_version"]);
  EXPECT_EQ(status["write"], "open");
  EXPECT_EQ(status["fee_per_contract"], "0.65");
  EXPECT_EQ(status["initial_cash"], "100000.00");
  EXPECT_EQ(json::parse(server::tick_message(*engine))["trading"], status);
  const auto chain = read(*engine, "/api/underlyings/SPX/chain");
  EXPECT_EQ(chain["strikes"][0]["call"]["symbol"], market.symbol());
  EXPECT_EQ(chain["strikes"][0]["call"]["bid_size"], 10);
  EXPECT_EQ(chain["strikes"][0]["call"]["tradable"], true);
  EXPECT_TRUE(chain["strikes"][0]["call"]["untradable_reason"].is_null());
}

TEST_F(PaperEngine, OrdersChangeInPlaceAndPositionsCloseOverHttp) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "rest", "3.90")).status, 201);
  auto changed = write(*engine, "PUT", "/api/orders/1", {{"limit_price", "4.00"}});
  ASSERT_EQ(changed.status, 200) << changed.body;
  auto body = json::parse(changed.body);
  EXPECT_EQ(body["order"]["id"], "1");
  EXPECT_EQ(body["order"]["limit_price"], "4.00");
  EXPECT_EQ(body["order"]["status"], "working");
  EXPECT_TRUE(body["fills"].empty());
  expect_error(write(*engine, "PUT", "/api/orders/1", json::object()), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "PUT", "/api/orders/1", {{"side", "sell"}}), 400, "INVALID_REQUEST");
  const auto version = engine->trading_view()->snapshot->account_version;
  const auto changes = body["order"]["changes"].size();
  for (const auto quantity : {0, -1}) {
    for (const auto& [method, path] : {std::pair{"PUT", "/api/orders/1"},
                                     std::pair{"POST", "/api/orders/1/preview"}}) {
      SCOPED_TRACE(std::string(method) + " " + path + " quantity=" + std::to_string(quantity));
      const auto refused = write(*engine, method, path, {{"quantity", quantity}});
      expect_error(refused, 400, "INVALID_REQUEST");
      EXPECT_EQ(json::parse(refused.body)["error"]["message"],
                "quantity must be a positive whole number of contracts or units");
      EXPECT_EQ(read(*engine, "/api/orders/1")["order"]["changes"].size(), changes);
      EXPECT_EQ(engine->trading_view()->snapshot->account_version, version);
    }
  }
  expect_error(write(*engine, "PUT", "/api/orders/1", {{"limit_price", "4.03"}}), 422, "INVALID_TICK");
  expect_error(write(*engine, "PUT", "/api/orders/9", {{"quantity", 2}}), 404, "UNKNOWN_ORDER");
  // Marketable at its new price, it fills in place.
  changed = write(*engine, "PUT", "/api/orders/1", {{"limit_price", "4.20"}, {"quantity", 2}});
  ASSERT_EQ(changed.status, 200) << changed.body;
  body = json::parse(changed.body);
  EXPECT_EQ(body["order"]["status"], "filled");
  EXPECT_EQ(body["fills"].size(), 1);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "second", "3.90")).status, 201);
  expect_error(write(*engine, "POST", "/api/orders/cancel", {{"underlying", "spx"}}), 400, "INVALID_REQUEST");
  const auto cancelled = write(*engine, "POST", "/api/orders/cancel", {{"underlying", "SPX"}});
  ASSERT_EQ(cancelled.status, 200) << cancelled.body;
  EXPECT_EQ(json::parse(cancelled.body)["cancelled_orders"], json::array({"2"}));
  // Flatten: resting orders go first, then each position closes at market.
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "third", "3.90")).status, 201);
  // Its dry run first: the same cancellations and closes, with nothing changed.
  const auto dry = write(*engine, "POST", "/api/positions/close/preview", json::object());
  ASSERT_EQ(dry.status, 200) << dry.body;
  body = json::parse(dry.body);
  EXPECT_TRUE(body["simulated"]);
  EXPECT_EQ(body["decision"], "ok");
  EXPECT_EQ(body["cancelled_orders"], json::array({"3"}));
  ASSERT_EQ(body["orders"].size(), 1);
  EXPECT_EQ(body["orders"][0]["side"], "sell");
  EXPECT_TRUE(body["orders"][0].contains("legs"));
  EXPECT_EQ(body["orders"][0]["legs"], nullptr);
  EXPECT_EQ(body["orders"][0]["status"], "filled");
  EXPECT_EQ(body["fills"].size(), 1);
  EXPECT_EQ(body["remaining"], json::array());
  EXPECT_TRUE(body["after"]["equity"].is_string());
  const auto after = body["after"]["equity"];
  EXPECT_EQ(read(*engine, "/api/orders")["orders"].size(), 3U);
  expect_error(write(*engine, "POST", "/api/positions/close/preview", {{"underlying", "spx"}}), 400, "INVALID_REQUEST");
  const auto closed = write(*engine, "POST", "/api/positions/close", json::object());
  ASSERT_EQ(closed.status, 200) << closed.body;
  body = json::parse(closed.body);
  EXPECT_EQ(body["cancelled_orders"], json::array({"3"}));
  ASSERT_EQ(body["orders"].size(), 1);
  EXPECT_EQ(body["orders"][0]["side"], "sell");
  EXPECT_EQ(body["orders"][0]["type"], "market");
  EXPECT_EQ(body["orders"][0]["quantity"], 2);
  EXPECT_EQ(body["orders"][0]["status"], "filled");
  EXPECT_EQ(body["fills"].size(), 1);
  EXPECT_EQ(body["stock_fills"], json::array());
  EXPECT_EQ(read(*engine, "/api/portfolio")["equity"], after);
  EXPECT_EQ(body["kept_stocks"], json::array());
  EXPECT_TRUE(read(*engine, "/api/portfolio")["positions"].empty());
}

TEST(PaperStatus, PublishesConfiguredMoneyStringsInStatusAndTick) {
  PaperProvider provider;
  auto options = paper_options();
  options.paper.fee_per_contract = Money::parse("1.234567");
  options.paper.initial_cash = Money::parse("234567.89");
  server::Engine engine(provider, md::Subscription{{"SPX"}}, options);
  const auto configured = read(engine, "/api/status")["trading"];
  EXPECT_EQ(configured["fee_per_contract"], "1.234567");
  EXPECT_EQ(configured["initial_cash"], "234567.89");
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto status = read(engine, "/api/status")["trading"];
  EXPECT_EQ(status["fee_per_contract"], "1.234567");
  EXPECT_EQ(status["initial_cash"], "234567.89");
  EXPECT_EQ(json::parse(server::tick_message(engine))["trading"], status);
}

TEST_F(PaperEngine, CancelFillOrderingKillAndRevisionChecks) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "cancel-first")).status, 201);
  auto cancelled = write(*engine, "DELETE", "/api/orders/1");
  ASSERT_EQ(cancelled.status, 200) << cancelled.body;
  EXPECT_EQ(json::parse(cancelled.body)["order"]["reason"]["code"], "USER_CANCEL");
  market.next(); quote("3.80", "4.00");
  EXPECT_TRUE(read(*engine, "/api/fills")["fills"].empty());
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "fill-first")).status, 201);
  expect_error(write(*engine, "DELETE", "/api/orders/2"), 409, "ORDER_TERMINAL");
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "kill", "3.80")).status, 201);
  auto killed = write(*engine, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "operator test"}});
  ASSERT_EQ(killed.status, 200) << killed.body;
  EXPECT_EQ(json::parse(killed.body)["cancelled_orders"], json::array({"3"}));
  EXPECT_EQ(json::parse(killed.body)["kill"]["reason"], "operator test");
  expect_error(write(*engine, "POST", "/api/orders", order(market, "blocked")), 422, "KILL_SWITCH");
  auto reset = write(*engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "reviewed"}});
  EXPECT_EQ(reset.status, 200);
  EXPECT_FALSE(json::parse(reset.body)["kill"]["latched"].get<bool>());
  auto risk = read(*engine, "/api/risk");
  EXPECT_EQ(risk["scenarios"]["pnl"].size(), 9);
  EXPECT_EQ(risk["scenarios"]["pnl"][0].size(), 4);
  EXPECT_EQ(risk["scenarios"]["pnl"][4][1], 0);
  auto limits = risk["limits"];
  limits["max_order_contracts"] = 2;
  const auto updated = write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}});
  EXPECT_EQ(updated.status, 200) << updated.body;
  EXPECT_EQ(json::parse(updated.body)["limits_revision"], "2");
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}), 409, "LIMITS_REVISION");
  limits["max_order_contracts"] = 0;
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}}), 422, "INVALID_LIMITS");
}

TEST_F(PaperEngine, UnderlyingOverridesRoundTripAndValidateWithoutASubscription) {
  auto limits = read(*engine, "/api/risk")["limits"];
  limits["underlying_overrides"] = {{"SPX", {{"dollar_delta", 500000}, {"vega", 5000}}},
                                     {"QQQ", {{"dollar_delta", 2000000}, {"vega", 20000}}}};
  auto reply = write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}});
  ASSERT_EQ(reply.status, 200) << reply.body;
  EXPECT_EQ(json::parse(reply.body)["limits"]["underlying_overrides"], limits["underlying_overrides"]);
  EXPECT_EQ(read(*engine, "/api/risk")["limits"]["underlying_overrides"], limits["underlying_overrides"]);
  for (const auto& bad : {json::array(), json(nullptr), json{{"spx", {{"dollar_delta", 1}, {"vega", 1}}}},
                         json{{"SPX", {{"dollar_delta", "1"}, {"vega", 1}}}}}) {
    limits["underlying_overrides"] = bad;
    expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}}), 400, "INVALID_REQUEST");
  }
  limits["underlying_overrides"] = {{"SPX", {{"dollar_delta", -1}, {"vega", 1}}}};
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}}), 422, "INVALID_LIMITS");
  limits.erase("underlying_overrides");
  reply = write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}});
  ASSERT_EQ(reply.status, 200);
  EXPECT_EQ(json::parse(reply.body)["limits"]["underlying_overrides"], json::object());
}
TEST_F(PaperEngine, OrderReasonsCarryTheirNumbersAndScope) {
  seed();
  auto resting = order(market, "resting", "3.50");
  resting["quantity"] = 3;
  ASSERT_EQ(write(*engine, "POST", "/api/orders", resting).status, 201);
  auto limits = read(*engine, "/api/risk")["limits"];
  limits["max_order_contracts"] = 2;
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}).status, 200);
  // The cancel keeps the check's numbers: three contracts against the new limit of
  // two, scoped to the underlying (the journal keeps the contract's OSI).
  const auto reason = read(*engine, "/api/orders")["orders"][0]["reason"];
  EXPECT_EQ(reason["code"], "RISK_CHANGED");
  EXPECT_EQ(reason["message"], "MAX_ORDER_CONTRACTS: Order contract count exceeds limit");
  EXPECT_EQ(reason["actual"], 3);
  EXPECT_EQ(reason["limit"], 2);
  EXPECT_EQ(reason["scope"], "SPX");
  // A reason without numbers still has every field.
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "cancelled", "3.50")).status, 201);
  ASSERT_EQ(write(*engine, "DELETE", "/api/orders/2").status, 200);
  const auto cancelled = read(*engine, "/api/orders")["orders"][0]["reason"];
  EXPECT_EQ(cancelled, json({{"code", "USER_CANCEL"}, {"message", "Cancelled by caller"}, {"actual", nullptr}, {"limit", nullptr}, {"scope", nullptr}}));
}

TEST_F(PaperEngine, OrdersReportTheirHistoryAndWhatTheyWaitFor) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "resting", "3.90")).status, 201);
  auto listed = read(*engine, "/api/orders")["orders"][0];
  EXPECT_EQ(listed["waiting"], json({{"code", "LIMIT"}, {"message", "The ask 4.20 is above the limit 3.90"}}));
  EXPECT_EQ(listed["ended_at"], nullptr);
  EXPECT_EQ(listed["modified_at"], nullptr);
  EXPECT_EQ(listed["changes"], json::array());
  ASSERT_EQ(write(*engine, "PUT", "/api/orders/1", {{"limit_price", "4.00"}}).status, 200);
  expect_error(write(*engine, "PUT", "/api/orders/1", {{"trigger_level", "5.00"}}), 422, "INVALID_ORDER");
  ASSERT_EQ(write(*engine, "DELETE", "/api/orders/1").status, 200);
  listed = read(*engine, "/api/orders")["orders"][0];
  const auto time = md::format_timestamp(market.time);
  EXPECT_EQ(listed["ended_at"], time);
  EXPECT_EQ(listed["modified_at"], time);
  EXPECT_EQ(listed["waiting"], nullptr) << "finished orders wait for nothing";
  ASSERT_EQ(listed["changes"].size(), 2);
  EXPECT_EQ(listed["changes"][0], json({{"time", time}, {"actor", "unknown"}, {"quantity", nullptr}, {"limit_price", "4.00"},
      {"trigger_level", nullptr}, {"previous", {{"quantity", 1}, {"limit_price", "3.90"}, {"trigger_level", nullptr}}},
      {"applied", true}, {"reason", nullptr}}));
  EXPECT_FALSE(listed["changes"][1]["applied"]);
  EXPECT_EQ(listed["changes"][1]["trigger_level"], "5.00");
  EXPECT_EQ(listed["changes"][1]["reason"]["code"], "INVALID_ORDER");
}

TEST_F(PaperEngine, PositiveChangeQuantitiesAtOrBelowFilledRemainRecordedBusinessRejections) {
  seed("4.00", "4.20", 2);
  auto request = order(market, "partial", "4.20");
  request["quantity"] = 3;
  const auto placed = write(*engine, "POST", "/api/orders", request);
  ASSERT_EQ(placed.status, 201) << placed.body;
  auto listed = json::parse(placed.body)["order"];
  ASSERT_EQ(listed["filled_quantity"], 2);
  for (const auto quantity : {1, 2}) {
    SCOPED_TRACE(quantity);
    const auto version = engine->trading_view()->snapshot->account_version;
    const auto changes = listed["changes"].size();
    const auto refused = write(*engine, "PUT", "/api/orders/1", {{"quantity", quantity}});
    expect_error(refused, 422, "INVALID_ORDER");
    const auto reason = json::parse(refused.body)["error"];
    EXPECT_EQ(reason["actual"], quantity);
    EXPECT_EQ(reason["limit"], 2);
    listed = read(*engine, "/api/orders/1")["order"];
    ASSERT_EQ(listed["changes"].size(), changes + 1);
    EXPECT_EQ(listed["changes"].back()["quantity"], quantity);
    EXPECT_FALSE(listed["changes"].back()["applied"]);
    EXPECT_EQ(listed["changes"].back()["reason"], reason);
    EXPECT_EQ(listed["quantity"], 3);
    EXPECT_EQ(listed["filled_quantity"], 2);
    EXPECT_GT(engine->trading_view()->snapshot->account_version, version);
  }
}

TEST_F(PaperEngine, ErrorsRejectMalformedUnknownFieldsAndRecordBusinessRejections) {
  seed();
  auto request = order(market);
  request["unexpected"] = true;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["quantity"] = 1.5;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request["quantity"] = "1";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["quantity"] = std::numeric_limits<std::uint64_t>::max();
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["limit_price"] = 4.0;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["limit_price"] = "4e0";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  // The schema's bounds: a client ID of 1 to 128 bytes without control characters,
  // a positive quantity, IOC for market orders and a positive single-contract limit.
  for (const auto& id : {std::string(), std::string(129, 'x'), std::string("tab\tid")}) {
    request = order(market); request["client_order_id"] = id;
    expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  }
  request = order(market); request["quantity"] = 0;
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market, "zero", "0.00");
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["type"] = "market"; request.erase("limit_price");
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  EXPECT_TRUE(read(*engine, "/api/orders")["orders"].empty());
  for (const auto* body : {"{", "[]", "{\"action\":\"trip\",\"action\":\"reset\",\"reason\":\"x\"}"}) {
    server::ApiResponse response;
    server::ApiRequest raw{"POST", "/api/risk/kill", body};
    server::handle_api_async(raw, *engine, [&](auto r) { response = std::move(r); });
    expect_error(response, 400, "INVALID_REQUEST");
  }
  expect_error(write(*engine, "DELETE", "/api/orders/bad"), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "DELETE", "/api/orders/999"), 404, "UNKNOWN_ORDER");
  request = order(market); request["symbol"] = "SPXW  261022C05100000";
  expect_error(write(*engine, "POST", "/api/orders", request), 404, "UNKNOWN_CONTRACT");
  request = order(market, "bad-tick", "4.01");
  expect_error(write(*engine, "POST", "/api/orders", request), 422, "INVALID_TICK");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "rejected");
  // A retry gets the first answer; the same key with other terms is a conflict.
  const auto recorded = read(*engine, "/api/orders")["orders"].size();
  expect_error(write(*engine, "POST", "/api/orders", request), 422, "INVALID_TICK");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"].size(), recorded);
  request["limit_price"] = "4.05";
  expect_error(write(*engine, "POST", "/api/orders", request), 409, "DUPLICATE_CLIENT_ID");
  // The conflict records nothing, and the ID still finds the one order it names.
  EXPECT_EQ(read(*engine, "/api/orders")["orders"].size(), recorded);
  const auto named = read(*engine, "/api/orders?client_order_id=bad-tick")["orders"];
  ASSERT_EQ(named.size(), 1u);
  EXPECT_EQ(named[0]["status"], "rejected");
  EXPECT_EQ(named[0]["attempt"], 1);
  EXPECT_TRUE(read(*engine, "/api/orders?client_order_id=nobody")["orders"].empty());
  expect_error(server::handle_api({"GET", "/api/orders?client_order_id="}, *engine), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5000.00"}}), 422, "INVALID_SETTLEMENT");
  expect_error(server::handle_api({"GET", "/api/orders?status=closed"}, *engine), 400, "INVALID_REQUEST");
  expect_error(server::handle_api({"GET", "/api/missing"}, *engine), 404, "NOT_FOUND");
}

TEST(CircuitBreakers, TripOnTheSandPsFallFromThePreviousClose) {
  const auto at = [](int h, int m) { return md::new_york_to_utc({2026, 9, 22}, h, m); };
  EXPECT_FALSE(server::circuit_breaker(5000, 4700, at(10, 0), 0));  // 6%
  const auto first = server::circuit_breaker(5000, 4640, at(10, 0), 0);  // 7.2%
  ASSERT_TRUE(first);
  EXPECT_EQ(first->level, 1);
  EXPECT_EQ(first->end, at(10, 15));
  EXPECT_FALSE(server::circuit_breaker(5000, 4600, at(10, 30), 1));  // once a day
  const auto second = server::circuit_breaker(5000, 4340, at(11, 0), 1);  // 13.2%
  ASSERT_TRUE(second);
  EXPECT_EQ(second->level, 2);
  EXPECT_EQ(server::circuit_breaker(5000, 4340, at(10, 0), 0)->level, 2);  // a gap past level 1
  EXPECT_FALSE(server::circuit_breaker(5000, 4600, at(15, 25), 0));  // no 15-minute halt from 15:25
  const auto third = server::circuit_breaker(5000, 3990, at(15, 40), 2);  // 20.2%, at any time
  ASSERT_TRUE(third);
  EXPECT_EQ(third->level, 3);
  EXPECT_EQ(third->end, at(17, 0));
  EXPECT_FALSE(server::circuit_breaker(5000, 3900, at(15, 45), 3));
  EXPECT_FALSE(server::circuit_breaker(5000, 4000, at(9, 0), 0));  // the regular session only
  EXPECT_FALSE(server::circuit_breaker(0, 4000, at(10, 0), 0));    // no previous close
  const auto early = [](int h, int m) { return md::new_york_to_utc({2026, 11, 27}, h, m); };
  EXPECT_TRUE(server::circuit_breaker(5000, 4600, early(12, 24), 0));  // 12:25 on an early-close day
  EXPECT_FALSE(server::circuit_breaker(5000, 4600, early(12, 25), 0));
}

TEST_F(PaperEngine, ACircuitBreakerHaltsOrdersAndFillsForFifteenMinutes) {
  market.contract = *md::parse_osi("SPXW261022P05000000");  // out of the money until the fall
  // Monday's close, as Cboe publishes it during Tuesday's session.
  provider.sink->publish(md::UnderlyingClose{"SPX", market.time, {2026, 9, 21}, 5400});
  const auto at = [&](md::Timestamp time, double spx, std::string ask) {
    provider.sink->publish(md::UnderlyingQuote{"SPX", time, spx, spx, spx});
    provider.sink->publish(md::OptionQuote{0, time, 3.80, Money::parse(ask).dollars(), 10, 10});
    ASSERT_TRUE(wait_for([&] { return engine->metrics("SPX") && engine->metrics("SPX")->as_of == time; }));
    // Status reads the trading view, published after the batch reaches the accounts.
    ASSERT_TRUE(wait_for([&] {
      const auto view = engine->trading_view();
      const auto seen = view->market_times.find("SPX");
      return seen != view->market_times.end() && seen->second == time;
    }));
  };
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  at(market.time - md::kNanosPerMinute, 5390, "4.20");
  const auto resting = write(*engine, "POST", "/api/orders", order(market, "resting", "4.00"));
  ASSERT_EQ(resting.status, 201) << resting.body;
  EXPECT_EQ(json::parse(resting.body)["order"]["status"], "working");
  // SPX falls 7.4% below 5,400: the market halts until 10:15, and even a marketable ask does not fill.
  at(market.time, 5000, "3.90");
  const auto status = read(*engine, "/api/status")["underlyings"][0]["paper"];
  EXPECT_EQ(status["accepting"], false);
  EXPECT_EQ(status["reason"], "MARKET_HALTED");
  EXPECT_EQ(status["message"], "Trading is halted market-wide: the S&P 500 fell 7.4% from its previous close of 5400.00 "
                               "(a level 1 circuit breaker); it resumes at 10:15 ET");
  expect_error(write(*engine, "POST", "/api/orders", order(market, "halted", "4.20")), 422, "MARKET_HALTED");
  EXPECT_EQ(read(*engine, "/api/orders?status=open")["orders"].size(), 1);
  // At 10:15 trading resumes; level 1 does not trip again that day.
  at(market.time + 15 * md::kNanosPerMinute, 5010, "3.90");
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->positions.size() == 1; }));
  EXPECT_EQ(read(*engine, "/api/status")["underlyings"][0]["paper"]["accepting"], true);
  EXPECT_EQ(write(*engine, "POST", "/api/orders", order(market, "after", "4.00")).status, 201);
}

TEST(CircuitBreakers, PublishesReferenceAndHaltsInStatusAndTicksWithoutPaperTrading) {
  PaperProvider provider;
  auto options = paper_options();
  options.paper_enabled = false;
  server::Engine engine(provider, {{"SPX", "SPY"}}, options);
  engine.start();
  const auto at = [](int hour, int minute) { return md::new_york_to_utc({2026, 9, 22}, hour, minute); };
  const auto state = [&] {
    const auto status = read(engine, "/api/status")["circuit_breaker"];
    EXPECT_EQ(json::parse(server::tick_message(engine))["circuit_breaker"], status);
    return status;
  };
  EXPECT_EQ(state(), (json{{"symbol", "SPX"}, {"day", nullptr}, {"previous_close", nullptr},
                           {"level", 0}, {"halts", json::array()}, {"market_time", nullptr},
                           {"active", false}, {"error", nullptr}, {"inactive_reason", "MISSING_PREVIOUS_CLOSE"}}));
  provider.sink->publish(md::UnderlyingClose{"SPX", at(9, 59), {2026, 9, 21}, 5400});
  provider.sink->publish(md::UnderlyingClose{"SPY", at(9, 59), {2026, 9, 21}, 540});
  provider.sink->publish(md::UnderlyingQuote{"SPY", at(9, 59), 400, 400, 400});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.market_time == at(9, 59); }));
  EXPECT_EQ(state()["previous_close"], (json{{"date", "2026-09-21"}, {"price", 5400}}));
  EXPECT_TRUE(state()["inactive_reason"].is_null());
  EXPECT_EQ(state()["level"], 0);
  provider.sink->publish(md::UnderlyingQuote{"SPX", at(10, 0), 5000, 5000, 5000});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.active; }));
  EXPECT_EQ(state()["halts"], (json::array({{{"level", 1}, {"start", md::format_timestamp(at(10, 0))},
      {"end", md::format_timestamp(at(10, 15))}, {"reference", 5400}, {"price", 5000}, {"active", true}}})));
  EXPECT_EQ(state()["day"], "2026-09-22");
  EXPECT_EQ(state()["level"], 1);
  // The wall clock is still 10:00. An option event alone can end the active flag.
  provider.sink->publish(md::OptionTrade{0, at(10, 15), 4, 1});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.market_time == at(10, 15); }));
  EXPECT_EQ(state()["active"], false);
  EXPECT_EQ(state()["halts"][0]["active"], false);
  EXPECT_EQ(state()["level"], 1);
}

TEST(CircuitBreakers, SpyUsesClosingPrintUntilAnOfficialCloseArrivesAndRollsTheDay) {
  PaperProvider provider;
  server::Engine engine(provider, {{"SPY"}}, paper_options());
  engine.start();
  const auto yesterday = md::new_york_to_utc({2026, 9, 21}, 16, 0);
  const auto today = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  provider.sink->publish(md::UnderlyingQuote{"SPY", yesterday, 540, 540, 540});
  provider.sink->publish(md::UnderlyingQuote{"SPY", today, 539, 539, 539});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.market_time == today; }));
  auto state = engine.status().circuit_breaker;
  EXPECT_EQ(state.symbol, "SPY");
  ASSERT_TRUE(state.previous_close);
  EXPECT_EQ(state.previous_close->date, (md::Date{2026, 9, 21}));
  EXPECT_EQ(state.previous_close->price, 540);
  provider.sink->publish(md::UnderlyingClose{"SPY", today, {2026, 9, 21}, 541});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.previous_close->price == 541; }));
  provider.sink->publish(md::UnderlyingQuote{"SPY", today + 1, 500, 500, 500});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.active; }));
  const auto tomorrow = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  provider.sink->publish(md::UnderlyingQuote{"SPY", tomorrow, 490, 490, 490});
  ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.market_time == tomorrow; }));
  state = engine.status().circuit_breaker;
  EXPECT_EQ(state.day, (md::Date{2026, 9, 23}));
  EXPECT_EQ(state.level, 0);
  EXPECT_FALSE(state.previous_close);
  EXPECT_FALSE(state.active);
  ASSERT_EQ(state.halts.size(), 1u);  // retain the last day with halts
  EXPECT_EQ(state.halts[0].reference, 541);
  // An out-of-order print cannot roll the breaker back and trip yesterday again.
  provider.sink->publish(md::UnderlyingQuote{"SPY", today + 2, 430, 430, 430});
  engine.stop();
  EXPECT_EQ(engine.status().circuit_breaker.level, 0);
  EXPECT_EQ(engine.status().circuit_breaker.halts.size(), 1u);
}

TEST(CircuitBreakers, RestartKeepsEachLevelsOriginalEndAndDoesNotTripItAgain) {
  for (const auto& [level, price] : std::vector<std::pair<int, double>>{{1, 5000}, {2, 4600}, {3, 4200}}) {
    SCOPED_TRACE(level);
    auto options = paper_options();
    options.paper_journal = paper_path();
    test::ScriptedMarket market;
    const auto start = market.time;
    const auto end = level == 3 ? md::new_york_to_utc({2026, 9, 22}, 17, 0) : start + 15 * md::kNanosPerMinute;
    std::atomic<md::Timestamp> now{start};
    options.clock = [&] { return now.load(); };
    {
      PaperProvider provider;
      server::Engine engine(provider, {{"SPX"}}, options);
      engine.start();
      EXPECT_TRUE(engine.status().circuit_breaker.error.empty());  // no file yet
      provider.sink->publish(md::UnderlyingClose{"SPX", start, {2026, 9, 21}, 5400});
      provider.sink->publish(md::UnderlyingQuote{"SPX", start, price, price, price});
      ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.active; }));
      EXPECT_TRUE(std::filesystem::exists(options.paper_journal.parent_path() / "market-halts.json"));
    }
    now = start + 5 * md::kNanosPerMinute;
    {
      PaperProvider provider;
      server::Engine engine(provider, {{"SPX"}}, options);
      engine.start();
      auto state = engine.status().circuit_breaker;
      ASSERT_EQ(state.halts.size(), 1u);
      EXPECT_EQ(state.halts[0].end, end);
      EXPECT_EQ(state.level, level);
      EXPECT_TRUE(state.active);
      ASSERT_TRUE(state.previous_close);
      EXPECT_EQ(state.previous_close->price, 5400);
      EXPECT_EQ(engine.trading_view()->halts[0].end, end);
      // Recovery needs neither another official close nor a new 15-minute timer.
      provider.sink->publish(md::ContractDefinition{0, market.contract});
      provider.sink->publish(md::UnderlyingQuote{"SPX", now.load(), price, price, price});
      provider.sink->publish(md::OptionQuote{0, now.load(), 4, 4.2, 10, 10});
      ASSERT_TRUE(wait_for([&] { return engine.trading_view()->market_times.contains("SPX"); }));
      const auto rejected = write(engine, "POST", "/api/orders", order(market, "during-recovery"));
      EXPECT_EQ(rejected.status, 422) << rejected.body;
      EXPECT_EQ(json::parse(rejected.body)["error"]["code"], "MARKET_HALTED");
      state = engine.status().circuit_breaker;
      ASSERT_EQ(state.halts.size(), 1u);
      EXPECT_EQ(state.halts[0].start, start);
      EXPECT_EQ(state.halts[0].end, end);
      now = end;
      provider.sink->publish(md::UnderlyingQuote{"SPX", end, price, price, price});
      ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.market_time == end; }));
      EXPECT_FALSE(engine.status().circuit_breaker.active);
      EXPECT_EQ(engine.status().circuit_breaker.halts.size(), 1u);
    }
    std::filesystem::remove_all(options.paper_journal.parent_path());
  }
}

TEST(CircuitBreakers, RecoveryCanEscalateAndANewDayReplacesTheHaltHistory) {
  auto options = paper_options();
  options.paper_journal = paper_path();
  const auto start = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  for (int level = 1; level <= 3; ++level) {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    EXPECT_EQ(engine.status().circuit_breaker.level, level - 1);
    if (level == 1) provider.sink->publish(md::UnderlyingClose{"SPX", start, {2026, 9, 21}, 5400});
    const double price = 5400 - 400 * level;
    provider.sink->publish(md::UnderlyingQuote{"SPX", start + level * md::kNanosPerMinute, price, price, price});
    ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.level == level; }));
    const auto state = engine.status().circuit_breaker;
    EXPECT_TRUE(state.error.empty()) << state.error;
    ASSERT_EQ(state.halts.size(), static_cast<std::size_t>(level));
    EXPECT_EQ(state.halts[0].end, start + 16 * md::kNanosPerMinute);
    EXPECT_TRUE(state.active);
  }
  const auto tomorrow = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    EXPECT_EQ(engine.status().circuit_breaker.level, 3);
    provider.sink->publish(md::UnderlyingClose{"SPX", tomorrow, {2026, 9, 22}, 4200});
    provider.sink->publish(md::UnderlyingQuote{"SPX", tomorrow, 3850, 3850, 3850});
    ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.market_time == tomorrow; }));
    const auto state = engine.status().circuit_breaker;
    EXPECT_TRUE(state.error.empty()) << state.error;
    EXPECT_EQ(state.level, 1);
    ASSERT_EQ(state.halts.size(), 1u);
    EXPECT_EQ(state.halts[0].reference, 4200);
    EXPECT_EQ(state.halts[0].start, tomorrow);
  }
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    const auto state = engine.status().circuit_breaker;
    EXPECT_TRUE(state.error.empty()) << state.error;
    EXPECT_EQ(state.day, (md::Date{2026, 9, 23}));
    EXPECT_EQ(state.level, 1);
    ASSERT_EQ(state.halts.size(), 1u);
    EXPECT_EQ(state.halts[0].start, tomorrow);
  }
  std::filesystem::remove_all(options.paper_journal.parent_path());
}

TEST(CircuitBreakers, InvalidRecoveredStateIsRejectedAsAWhole) {
  auto options = paper_options();
  options.paper_journal = paper_path();
  const auto file = options.paper_journal.parent_path() / "market-halts.json";
  const auto start = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  json state;
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    provider.sink->publish(md::UnderlyingClose{"SPX", start, {2026, 9, 21}, 5400});
    provider.sink->publish(md::UnderlyingQuote{"SPX", start, 5000, 5000, 5000});
    ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.active; }));
    std::ifstream in(file);
    state = json::parse(in);
  }
  for (int problem = 0; problem < 3; ++problem) {
    auto bad = state;
    if (problem == 0) bad["halts"][0]["end"] = start;
    if (problem == 1) bad["level"] = 0;
    if (problem == 2) bad["previous_close"]["price"] = -5400;
    { std::ofstream out(file); out << bad.dump(); }
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    const auto recovered = engine.status().circuit_breaker;
    EXPECT_TRUE(engine.status().trading.enabled);
    EXPECT_FALSE(recovered.error.empty());
    EXPECT_EQ(recovered.level, 0);
    EXPECT_TRUE(recovered.halts.empty());
    EXPECT_FALSE(recovered.previous_close);
  }
  std::filesystem::remove_all(options.paper_journal.parent_path());
}

TEST(CircuitBreakers, BadRecoveryAndFailedWritesReportStorageErrorsWithoutStoppingTheEngine) {
  for (const bool unreadable : {false, true}) {
    auto options = paper_options();
    options.paper_journal = paper_path();
    const auto file = options.paper_journal.parent_path() / "market-halts.json";
    if (unreadable) std::filesystem::create_directory(file);
    else { std::ofstream out(file); out << "{broken"; }
    {
      PaperProvider provider;
      server::Engine engine(provider, {{"SPX"}}, options);
      engine.start();
      EXPECT_TRUE(engine.status().trading.enabled);
      EXPECT_NE(engine.status().circuit_breaker.error.find("cannot read"), std::string::npos);
      std::filesystem::create_directory(file.string() + ".tmp");
      const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
      provider.sink->publish(md::UnderlyingClose{"SPX", time, {2026, 9, 21}, 5400});
      provider.sink->publish(md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000});
      ASSERT_TRUE(wait_for([&] { return engine.status().circuit_breaker.active; }));
      EXPECT_TRUE(engine.status().trading.enabled);
      const auto state = read(engine, "/api/status")["circuit_breaker"];
      EXPECT_NE(state["error"].get<std::string>().find("cannot write"), std::string::npos);
      EXPECT_EQ(json::parse(server::tick_message(engine))["circuit_breaker"], state);
      if (!unreadable) {
        std::ifstream in(file);
        std::string original;
        std::getline(in, original);
        EXPECT_EQ(original, "{broken");
      }
    }
    std::filesystem::remove_all(options.paper_journal.parent_path());
  }
}

TEST_F(PaperEngine, ARetriedOrderGetsItsFirstAnswer) {
  seed();
  const auto first = write(*engine, "POST", "/api/orders", order(market, "retry", "4.20"));
  ASSERT_EQ(first.status, 201) << first.body;
  const auto again = write(*engine, "POST", "/api/orders", order(market, "retry", "4.20"));
  ASSERT_EQ(again.status, 200) << again.body;
  EXPECT_EQ(json::parse(again.body)["order"], json::parse(first.body)["order"]);
  EXPECT_EQ(json::parse(again.body)["fills"], json::parse(first.body)["fills"]);
  EXPECT_EQ(read(*engine, "/api/orders")["orders"].size(), 1);
}

TEST_F(PaperEngine, FractionalSizesNeverRoundUpAndCachedObservationsDoNotRefill) {
  seed("4.00", "4.20", 1.9);
  auto first = write(*engine, "POST", "/api/orders", order(market, "first", "4.20"));
  ASSERT_EQ(first.status, 201) << first.body;
  EXPECT_EQ(json::parse(first.body)["order"]["status"], "filled");
  auto second = write(*engine, "POST", "/api/orders", order(market, "second", "4.20"));
  ASSERT_EQ(second.status, 201) << second.body;
  EXPECT_EQ(json::parse(second.body)["order"]["status"], "working");
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time + 1, 5000, 5000, 5000});
  EXPECT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == market.time + 1; }));
  EXPECT_EQ(engine->trading_view()->snapshot->recent_fills.size(), 1);
  market.next(); quote("4.00", "4.20", 1.9);
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->recent_fills.size() == 2; }));
}

TEST_F(PaperEngine, ChainsAndPreviewsShowExecutableQuotesAndTheDisplayedSizeAnAccountTook) {
  seed("4.00", "4.20", 3);
  auto take = order(market, "take", "4.20");
  take["quantity"] = 2;
  take["time_in_force"] = "ioc";
  ASSERT_EQ(write(*engine, "POST", "/api/orders", take).status, 201);
  const auto call = read(*engine, "/api/underlyings/SPX/chain")["strikes"][0]["call"];
  EXPECT_EQ(call["executable"], true);
  EXPECT_TRUE(call["quote_issue"].is_null());
  const auto used = read(*engine, "/api/portfolio")["liquidity_used"];
  ASSERT_EQ(used.size(), 1U);
  EXPECT_EQ(used[0], (json{{"symbol", market.symbol()}, {"bid_size", 3}, {"ask_size", 3}, {"bid_left", 3}, {"ask_left", 1}}));
  auto more = order(market, "preview", "4.20");
  more["quantity"] = 3;
  const auto preview = write(*engine, "POST", "/api/orders/preview", more);
  ASSERT_EQ(preview.status, 200) << preview.body;
  const auto leg = json::parse(preview.body)["liquidity"][0];
  EXPECT_EQ(leg["symbol"], market.symbol());
  EXPECT_EQ(leg["side"], "buy");
  EXPECT_EQ(leg["contracts"], 3);
  EXPECT_EQ(leg["executable"], true);
  EXPECT_EQ(leg["displayed"], 3);
  EXPECT_EQ(leg["size_left"], 1);
  // A new observation refreshes the size; a one-sided one cannot fill at all.
  market.next(); quote("0.00", "4.20", 3);
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/portfolio")["liquidity_used"].empty(); }));
  const auto one_sided = read(*engine, "/api/underlyings/SPX/chain")["strikes"][0]["call"];
  EXPECT_EQ(one_sided["executable"], false);
  EXPECT_EQ(one_sided["quote_issue"], "no_bid");
  more["side"] = "sell";
  more["limit_price"] = "4.00";
  const auto refused = json::parse(write(*engine, "POST", "/api/orders/preview", more).body)["liquidity"][0];
  EXPECT_EQ(refused["executable"], false);
  EXPECT_EQ(refused["reason"]["code"], "INVALID_QUOTE");
  EXPECT_EQ(refused["reason"]["message"], "No bid: the quote is one-sided");
  EXPECT_EQ(refused["displayed"], 0);
}

TEST_F(PaperEngine, EligibilityUsesTheDefinitionAndRecordsRejection) {
  // An American contract on a European index root is not listed.
  market.contract.style = pricing::ExerciseStyle::American;
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  ASSERT_TRUE(wait_for([&] { return engine->status().contracts == 1; }));
  expect_error(write(*engine, "POST", "/api/orders", order(market)), 422, "AMERICAN_UNSUPPORTED");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "rejected");
}

TEST(PaperEquity, SpyOptionsAreTradableAndFillAgainstTheirOwnBook) {
  PaperProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPY261022C00500000");
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{0, market.time, 4.00, 4.20, 10, 10});
  ASSERT_TRUE(wait_for([&] {
    const auto metrics = engine.metrics("SPY");
    return metrics && metrics->as_of == market.time && !metrics->slices.empty();
  }));
  EXPECT_TRUE(read(engine, "/api/status")["underlyings"][0]["has_tradable_contracts"].get<bool>());
  const auto response = write(engine, "POST", "/api/orders", order(market, "spy", "4.20"));
  ASSERT_EQ(response.status, 201) << response.body;
  const auto body = json::parse(response.body);
  EXPECT_EQ(body["order"]["status"], "filled");
  EXPECT_EQ(body["order"]["day_end"], md::format_timestamp(md::new_york_to_utc({2026, 9, 22}, 16, 15)));
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["symbol"], market.symbol());
  engine.stop();
}

TEST_F(PaperEngine, PmUsesFirstExpiryDatePrintAndAmWaitsForImport) {
  market.contract = *md::parse_osi("SPXW260922C05000000");
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "pm", "4.20")).status, 201);
  auto expiry = market.contract.expiry_time();
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry - 1, 5010, 5010, 5010});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == expiry - 1; }));
  EXPECT_EQ(read(*engine, "/api/portfolio")["positions"].size(), 1);
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry, 5012, 5012, 5012});
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry + 1, 5020, 5020, 5020});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->positions.empty(); }));
  EXPECT_EQ(read(*engine, "/api/portfolio")["cash"], "100779.35");

  market.contract = *md::parse_osi("SPX260923C05000000");
  market.time = md::new_york_to_utc({2026, 9, 22}, 16, 5);
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "am", "4.20")).status, 201);
  expiry = market.contract.expiry_time();
  provider.sink->publish(md::UnderlyingQuote{"SPX", expiry, 5050, 5050, 5050});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == expiry; }));
  const auto waiting = read(*engine, "/api/portfolio");
  ASSERT_EQ(waiting["positions"].size(), 1);
  EXPECT_EQ(waiting["positions"][0]["awaiting_settlement"], true);
  EXPECT_EQ(waiting["valuation_complete"], false);
  EXPECT_TRUE(waiting["positions"][0]["greeks"]["delta"].is_null());
  auto settlement = write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5010.00"}});
  ASSERT_EQ(settlement.status, 200) << settlement.body;
  EXPECT_EQ(json::parse(settlement.body)["position_closed"], true);
  expect_error(write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5010.00"}}), 422, "ALREADY_SETTLED");
}

TEST(PaperAccounts, NamedAccountsTradeApartAndRecoverFromTheirOwnJournals) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-accounts-" + std::to_string(md::now()));
  auto options = paper_options();
  options.paper_journal = directory / "paper-journal.jsonl";
  options.paper_accounts = directory / "accounts";
  test::ScriptedMarket market;
  std::atomic<md::Timestamp> wall_now{market.time};
  options.clock = [&] { return wall_now.load(); };
  std::string swing;
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    EXPECT_EQ(read(engine, "/api/accounts")["accounts"].size(), 1);
    auto created = write(engine, "POST", "/api/accounts", {{"name", "Swing 50k"}, {"plan", "eod-50k"}});
    ASSERT_EQ(created.status, 201) << created.body;
    EXPECT_EQ(json::parse(created.body)["account"]["id"], "swing-50k");
    EXPECT_EQ(json::parse(created.body)["account"]["equity"], "50000.00");
    EXPECT_EQ(json::parse(write(engine, "POST", "/api/accounts", {{"name", "Swing 50k"}, {"plan", "practice"}}).body)["account"]["id"], "swing-50k-2");
    EXPECT_EQ(write(engine, "POST", "/api/accounts", {{"name", "Funded"}, {"plan", "funded-eod-50k"}}).status, 400);
    EXPECT_EQ(write(engine, "POST", "/api/accounts", {{"name", ""}, {"plan", "practice"}}).status, 400);
    const auto accounts = read(engine, "/api/accounts")["accounts"];
    ASSERT_EQ(accounts.size(), 3);
    EXPECT_EQ(accounts[0]["id"], "main");
    EXPECT_EQ(accounts[1]["name"], "Swing 50k");
    EXPECT_EQ(accounts[1]["trading"]["plan"], "End-of-day 50K");
    EXPECT_EQ(accounts[1]["trading"]["plan_id"], "eod-50k");
    EXPECT_EQ(json::parse(created.body)["account"]["plan_id"], "eod-50k");
    EXPECT_EQ(read(engine, "/api/account?account=swing-50k")["rules"]["plan_id"], "eod-50k");
    EXPECT_EQ(json::parse(server::tick_message(engine))["accounts"].size(), 3);

    provider.sink->publish(md::ContractDefinition{0, market.contract});
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 10, 10});
    ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
    const auto bought = write(engine, "POST", "/api/orders?account=swing-50k", order(market, "swing", "4.20"));
    ASSERT_EQ(bought.status, 201) << bought.body;
    EXPECT_EQ(json::parse(bought.body)["order"]["status"], "filled");
    EXPECT_EQ(read(engine, "/api/portfolio?account=swing-50k")["positions"].size(), 1);
    EXPECT_TRUE(read(engine, "/api/portfolio")["positions"].empty());
    EXPECT_TRUE(read(engine, "/api/portfolio?account=main")["positions"].empty());
    EXPECT_EQ(read(engine, "/api/orders?status=open&account=swing-50k")["orders"].size(), 0);
    // Unknown and malformed accounts are errors, reads and writes alike.
    EXPECT_EQ(server::handle_api({"GET", "/api/portfolio?account=nobody"}, engine).status, 404);
    EXPECT_EQ(server::handle_api({"GET", "/api/portfolio?account=Swing"}, engine).status, 400);
    EXPECT_EQ(write(engine, "POST", "/api/orders?account=nobody", order(market, "lost")).status, 404);
    EXPECT_EQ(write(engine, "POST", "/api/orders?account=swing-50k&x=1", order(market, "extra")).status, 400);
    EXPECT_EQ(write(engine, "PATCH", "/api/accounts/main", {{"name", "No"}}).status, 403);
    EXPECT_EQ(write(engine, "PATCH", "/api/accounts/missing", {{"archived", true}}).status, 404);
    for (const auto& body : {json::object(), json{{"name", ""}}, json{{"archived", "yes"}}, json{{"unknown", true}}}) {
      EXPECT_EQ(write(engine, "PATCH", "/api/accounts/swing-50k", body).status, 400);
    }
    EXPECT_EQ(write(engine, "PATCH", "/api/accounts/swing-50k", {{"archived", true}}).status, 200);
    EXPECT_EQ(read(engine, "/api/accounts")["accounts"].size(), 2);
    EXPECT_EQ(read(engine, "/api/accounts?archived=true")["accounts"].size(), 3);
    EXPECT_EQ(json::parse(server::tick_message(engine))["accounts"].size(), 2);
    EXPECT_EQ(write(engine, "POST", "/api/orders?account=swing-50k", order(market, "frozen")).status, 409);
    EXPECT_EQ(write(engine, "PATCH", "/api/accounts/swing-50k", {{"archived", false}}).status, 200);
    swing = read(engine, "/api/portfolio?account=swing-50k").dump();
    engine.stop();
  }
  EXPECT_TRUE(std::filesystem::exists(directory / "accounts" / "swing-50k.jsonl"));
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view("swing-50k") != nullptr; }));
    const auto accounts = read(engine, "/api/accounts")["accounts"];
    ASSERT_EQ(accounts.size(), 3);
    EXPECT_EQ(accounts[1]["name"], "Swing 50k");
    EXPECT_EQ(read(engine, "/api/portfolio?account=swing-50k").dump(), swing);
    EXPECT_TRUE(read(engine, "/api/portfolio")["positions"].empty());
    engine.stop();
  }
  std::filesystem::remove_all(directory);
}

TEST(PaperAccounts, AccountNamesCountCharactersNotBytes) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-names-" + std::to_string(md::now()));
  auto options = paper_options();
  options.paper_journal = directory / "paper-journal.jsonl";
  options.paper_accounts = directory / "accounts";
  std::string accented;
  for (int i = 0; i < 64; ++i) accented += "\u00e9";  // 64 characters, 128 bytes
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    const auto created = write(engine, "POST", "/api/accounts", {{"name", accented}, {"plan", "practice"}});
    ASSERT_EQ(created.status, 201) << created.body;
    EXPECT_EQ(write(engine, "POST", "/api/accounts", {{"name", std::string(64, 'n')}, {"plan", "practice"}}).status, 201);
    EXPECT_EQ(write(engine, "POST", "/api/accounts", {{"name", "\U0001F4C8 Swing"}, {"plan", "practice"}}).status, 201);
    for (const auto& name : {accented + "\u00e9", std::string(65, 'n'), std::string("tab\there"), std::string("next\u0085line"), std::string()}) {
      const auto refused = write(engine, "POST", "/api/accounts", {{"name", name}, {"plan", "practice"}});
      EXPECT_EQ(refused.status, 400) << name;
      EXPECT_NE(refused.body.find("1 to 64 characters"), std::string::npos) << refused.body;
    }
    engine.stop();
  }
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto accounts = read(engine, "/api/accounts")["accounts"];
  ASSERT_EQ(accounts.size(), 4U);
  EXPECT_TRUE(std::any_of(accounts.begin(), accounts.end(), [&](const auto& account) { return account["name"] == accented; })) << accounts;
  engine.stop();
  std::filesystem::remove_all(directory);
}

TEST(PaperAccounts, JournalsAFullDiskLeftEmptyStartAfreshAndFailedCreatesLeaveNothing) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-empty-" + std::to_string(md::now()));
  const auto accounts = directory / "accounts";
  auto options = paper_options();
  options.paper_journal = directory / "paper-journal.jsonl";
  options.paper_accounts = accounts;
  auto fail = std::make_shared<std::atomic<bool>>(false);
  options.journal_io.sync = [fail](int) { return !fail->load(); };
  // What a disk that refused every first record leaves: empty journals and a name.
  std::filesystem::create_directories(accounts);
  std::ofstream(options.paper_journal).close();
  std::ofstream(accounts / "side.jsonl").close();
  std::ofstream(accounts / "side.name") << "side\n";
  const auto enabled = [](const json& listed) {
    std::vector<std::string> ids;
    for (const auto& account : listed["accounts"]) {
      EXPECT_EQ(account["trading"]["enabled"], true) << account.dump();
      ids.push_back(account["id"].get<std::string>());
    }
    return ids;
  };
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    // The main account starts afresh; the named one never existed, so its files go.
    EXPECT_EQ(enabled(read(engine, "/api/accounts")), std::vector<std::string>{"main"});
    EXPECT_EQ(read(engine, "/api/portfolio")["equity"], "100000.00");
    EXPECT_GT(std::filesystem::file_size(options.paper_journal), 0U);
    EXPECT_FALSE(std::filesystem::exists(accounts / "side.jsonl"));
    EXPECT_FALSE(std::filesystem::exists(accounts / "side.name"));
    auto created = write(engine, "POST", "/api/accounts", {{"name", "side"}, {"plan", "practice"}});
    ASSERT_EQ(created.status, 201) << created.body;
    EXPECT_EQ(json::parse(created.body)["account"]["id"], "side");
    // A create whose name cannot be written, or whose first record fails, removes
    // the journal it made, and the same name is then created as asked.
    std::filesystem::create_directory(accounts / "probe.name");
    auto failed = write(engine, "POST", "/api/accounts", {{"name", "Probe"}, {"plan", "practice"}});
    EXPECT_EQ(failed.status, 503) << failed.body;
    EXPECT_FALSE(std::filesystem::exists(accounts / "probe.jsonl"));
    EXPECT_TRUE(std::filesystem::is_directory(accounts / "probe.name"));
    std::filesystem::remove(accounts / "probe.name");
    created = write(engine, "POST", "/api/accounts", {{"name", "Probe"}, {"plan", "practice"}});
    ASSERT_EQ(created.status, 201) << created.body;
    EXPECT_EQ(json::parse(created.body)["account"]["id"], "probe");
    fail->store(true);
    failed = write(engine, "POST", "/api/accounts", {{"name", "Disk"}, {"plan", "practice"}});
    EXPECT_EQ(failed.status, 503) << failed.body;
    EXPECT_NE(failed.body.find("JOURNAL_IO"), std::string::npos) << failed.body;
    EXPECT_FALSE(std::filesystem::exists(accounts / "disk.jsonl"));
    EXPECT_FALSE(std::filesystem::exists(accounts / "disk.name"));
    fail->store(false);
    created = write(engine, "POST", "/api/accounts", {{"name", "Disk"}, {"plan", "practice"}});
    ASSERT_EQ(created.status, 201) << created.body;
    EXPECT_EQ(json::parse(created.body)["account"]["id"], "disk");
    engine.stop();
  }
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view("side") != nullptr; }));
    EXPECT_EQ(enabled(read(engine, "/api/accounts")), (std::vector<std::string>{"main", "disk", "probe", "side"}));
    engine.stop();
  }
  std::filesystem::remove_all(directory);
}

TEST(PaperAccounts, AServerWithoutAnAccountsDirectoryKeepsOneAccount) {
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  // Lasting, so not a 503 a client would retry.
  const auto response = write(engine, "POST", "/api/accounts", {{"name", "Second"}, {"plan", "practice"}});
  EXPECT_EQ(response.status, 409) << response.body;
  EXPECT_EQ(json::parse(response.body)["error"]["code"], "ACCOUNTS_UNSUPPORTED");
  EXPECT_EQ(response.retry_after, 0);
  EXPECT_EQ(read(engine, "/api/accounts")["accounts"].size(), 1);
}

json sell(const test::ScriptedMarket& market, std::string client, std::string price) {
  auto request = order(market, std::move(client), std::move(price));
  request["side"] = "sell";
  return request;
}

TEST(PaperFreshness, AHeldQuoteThatDoesNotChangeStaysCurrentWithoutRefillingItsSize) {
  // Snapshot feeds send only the quotes that changed, then mark the snapshot
  // complete. A held contract whose quote sits unchanged through them is still the
  // market: it must not go stale and block the account's orders, nor offer its size
  // again.
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, paper_options()); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  test::ScriptedMarket held;
  test::ScriptedMarket other;
  other.contract = *md::parse_osi("SPXW261022C05100000");
  provider.sink->publish(md::ContractDefinition{0, held.contract});
  provider.sink->publish(md::ContractDefinition{1, other.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", held.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, held.time, 4, 4.2, 1, 1});
  provider.sink->publish(md::OptionQuote{1, held.time, 2, 2.2, 5, 5});
  provider.sink->publish(md::SnapshotComplete{"SPX", held.time});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == held.time; }));
  const auto bought = write(engine, "POST", "/api/orders", order(held, "buy", "4.20"));
  ASSERT_EQ(bought.status, 201) << bought.body;
  ASSERT_EQ(json::parse(bought.body)["order"]["status"], "filled");
  // Two minutes on, only the other contract and the index have changed.
  auto later = held.time;
  for (int step = 1; step <= 4; ++step) {
    later += 30 * md::kNanosPerSecond;
    provider.sink->publish(md::UnderlyingQuote{"SPX", later, 5000, 5000, 5000.0 + step});
    provider.sink->publish(md::OptionQuote{1, later, 2, 2.2, 5.0 + step, 5.0 + step});
    provider.sink->publish(md::SnapshotComplete{"SPX", later});
  }
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == later; }));
  const auto opened = write(engine, "POST", "/api/orders", order(other, "open", "2.20"));
  ASSERT_EQ(opened.status, 201) << opened.body;
  EXPECT_EQ(json::parse(opened.body)["order"]["status"], "filled");
  const auto closed = write(engine, "POST", "/api/orders", sell(held, "close", "4.00"));
  ASSERT_EQ(closed.status, 201) << closed.body;
  EXPECT_EQ(json::parse(closed.body)["order"]["status"], "filled");
  const auto fills = engine.trading_view()->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 3U);
  EXPECT_EQ(fills.back().observation, 1U);  // the held quote as first seen, confirmed since
  EXPECT_EQ(fills.back().quote_time, later);
  // The one contract its ask showed was bought at the start; confirming the quote
  // does not show it again.
  const auto again = write(engine, "POST", "/api/orders", order(held, "again", "4.20"));
  ASSERT_EQ(again.status, 201) << again.body;
  EXPECT_EQ(json::parse(again.body)["order"]["status"], "working");
}

TEST(PaperFreshness, AHeldWingNobodyBidsForStillLetsTheAccountTrade) {
  // Far wings often lose their bid (0.00 / 0.05) as the market moves away, and then
  // their strike has no smile IV. Held, such a wing is marked halfway to its ask and
  // valued at its ask's IV, so the account can still trade; the wing itself cannot be
  // sold into a bid that is not there.
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, paper_options()); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  test::ScriptedMarket atm;
  test::ScriptedMarket wing;
  wing.contract = *md::parse_osi("SPXW261022C05600000");
  provider.sink->publish(md::ContractDefinition{0, atm.contract});
  provider.sink->publish(md::ContractDefinition{1, wing.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", atm.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, atm.time, 4, 4.2, 10, 10});
  provider.sink->publish(md::OptionQuote{1, atm.time, 0.05, 0.10, 10, 10});
  provider.sink->publish(md::SnapshotComplete{"SPX", atm.time});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == atm.time; }));
  const auto bought = write(engine, "POST", "/api/orders", order(wing, "wing", "0.10"));
  ASSERT_EQ(bought.status, 201) << bought.body;
  ASSERT_EQ(json::parse(bought.body)["order"]["status"], "filled");
  const auto later = atm.time + 30 * md::kNanosPerSecond;
  provider.sink->publish(md::UnderlyingQuote{"SPX", later, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{1, later, 0, 0.05, 0, 10});
  provider.sink->publish(md::SnapshotComplete{"SPX", later});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == later; }));
  const auto portfolio = read(engine, "/api/portfolio");
  EXPECT_EQ(portfolio["valuation_complete"], true) << portfolio.dump();
  EXPECT_EQ(portfolio["positions"][0]["fresh"], true);
  const auto opened = write(engine, "POST", "/api/orders", order(atm, "atm", "4.20"));
  ASSERT_EQ(opened.status, 201) << opened.body;
  EXPECT_EQ(json::parse(opened.body)["order"]["status"], "filled");
  const auto sold = write(engine, "POST", "/api/orders", sell(wing, "no-bid", "0.05"));
  ASSERT_EQ(sold.status, 422) << sold.body;
  EXPECT_EQ(json::parse(sold.body)["error"]["code"], "INVALID_QUOTE");
}

TEST(PaperFreshness, AfterAGapQuotesWaitForTheNextCompleteSnapshot) {
  // Overnight, the day's first index print can come a batch before its option
  // quotes. Until a snapshot of the new day completes, yesterday's quotes are not
  // current, however little they have changed; one it repeats unchanged then is.
  PaperProvider provider;
  provider.delay = std::chrono::minutes(15);
  test::ScriptedMarket market;
  std::atomic<md::Timestamp> wall{market.time + 15 * md::kNanosPerMinute};
  auto options = paper_options();
  options.clock = [&] { return wall.load(); };
  server::Engine engine(provider, {{"SPX"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 10, 10});
  provider.sink->publish(md::SnapshotComplete{"SPX", market.time});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "buy", "4.20")).status, 201);
  const auto next = market.time + md::kNanosPerDay;
  wall = next + 15 * md::kNanosPerMinute;
  provider.sink->publish(md::UnderlyingQuote{"SPX", next, 5000, 5000, 5000});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->market_times.at("SPX") == next; }));
  const auto waiting = write(engine, "POST", "/api/orders", order(market, "early", "4.20"));
  ASSERT_EQ(waiting.status, 422) << waiting.body;
  EXPECT_EQ(json::parse(waiting.body)["error"]["code"], "STALE_QUOTE");
  provider.sink->publish(md::SnapshotComplete{"SPX", next});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->valuation_complete; }));
  const auto current = write(engine, "POST", "/api/orders", order(market, "current", "4.20"));
  ASSERT_EQ(current.status, 201) << current.body;
  EXPECT_EQ(json::parse(current.body)["order"]["status"], "filled");
}

TEST(PaperFreshness, AnUnderlyingWhoseFeedRunsBehindAnothersTradesUntilItStalls) {
  // Cboe's quote pages trail its data files by a minute or two, so one underlying's
  // data can run behind another's. Its quotes are judged by its own feed, not the
  // other's: it trades within a delayed feed's stall tolerance and stalls past it.
  PaperProvider provider;
  provider.delay = std::chrono::minutes(15);
  test::ScriptedMarket market;
  std::atomic<md::Timestamp> wall{market.time + 15 * md::kNanosPerMinute};
  auto options = paper_options();
  options.clock = [&] { return wall.load(); };
  server::Engine engine(provider, {{"SPX", "SPY"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 5, 5});
  provider.sink->publish(md::SnapshotComplete{"SPX", market.time});
  provider.sink->publish(md::SnapshotComplete{"SPY", market.time});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "buy", "4.20")).status, 201);
  // SPY's data runs 90 seconds ahead of SPX's, which has not changed.
  const auto ahead = [&](md::Timestamp by) {
    wall = market.time + 15 * md::kNanosPerMinute + by;
    provider.sink->publish(md::UnderlyingQuote{"SPY", market.time + by, 500, 500, 500});
    provider.sink->publish(md::SnapshotComplete{"SPY", market.time + by});
    return wait_for([&] { return engine.trading_view()->market_times.at("SPY") == market.time + by; });
  };
  ASSERT_TRUE(ahead(90 * md::kNanosPerSecond));
  const auto spx = [&] { return read(engine, "/api/status")["underlyings"][0]["paper"]; };
  EXPECT_EQ(spx()["accepting"], true) << spx();
  const auto closed = write(engine, "POST", "/api/orders", sell(market, "close", "4.00"));
  ASSERT_EQ(closed.status, 201) << closed.body;
  EXPECT_EQ(json::parse(closed.body)["order"]["status"], "filled");
  // Four minutes behind, past the three-minute tolerance, SPX has stalled.
  ASSERT_TRUE(ahead(4 * md::kNanosPerMinute));
  EXPECT_EQ(spx()["reason"], "FEED_STALLED") << spx();
  const auto stalled = write(engine, "POST", "/api/orders", order(market, "stalled", "4.20"));
  ASSERT_EQ(stalled.status, 422) << stalled.body;
  EXPECT_EQ(json::parse(stalled.body)["error"]["code"], "FEED_STALLED");
}

TEST_F(PaperEngine, EquityHistoryReloadsPerAccountWithFillSamplesBoundsAndAGap) {
  engine->stop(); engine.reset();
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  options.paper_accounts = path.parent_path() / "accounts";
  options.clock = [this] { return market.time; };
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  ASSERT_EQ(write(*engine, "POST", "/api/accounts", {{"name", "Evaluation"}, {"plan", "eod-25k"}}).status, 201);
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders?account=evaluation", order(market, "entry", "4.20")).status, 201);
  const auto initial = read(*engine, "/api/account/equity?account=evaluation")["samples"];
  ASSERT_GE(initial.size(), 2U);
  EXPECT_EQ(initial.back()["fill"], "1");
  EXPECT_EQ(initial.back()["equity"], "24989.35");
  EXPECT_EQ(initial.back()["floor"], "23500.00");
  EXPECT_EQ(initial.back()["tomorrow_floor"], "23500.00");
  EXPECT_EQ(read(*engine, "/api/account/equity")["samples"].back()["equity"], "100000.00");
  engine->stop(); engine.reset();
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view("evaluation") != nullptr; }));
  EXPECT_EQ(read(*engine, "/api/account/equity?account=evaluation")["samples"], initial);
  market.time += 10 * md::kNanosPerMinute;
  seed();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view("evaluation")->equity_samples.size() > initial.size(); }));
  const auto resumed = read(*engine, "/api/account/equity?account=evaluation")["samples"];
  EXPECT_EQ(resumed.size(), initial.size() + 1);
  EXPECT_EQ(resumed.back()["time"], md::format_timestamp(market.time));
  const auto bounded = read(*engine, "/api/account/equity?account=evaluation&from=2026-09-22T14%3A10%3A00Z&to=2026-09-22T14%3A10%3A00Z")["samples"];
  ASSERT_EQ(bounded.size(), 1U);
  EXPECT_EQ(bounded.front(), resumed.back());
  EXPECT_EQ(server::handle_api({"GET", "/api/account/equity?from=%zz"}, *engine).status, 400);
  engine->stop(); engine.reset();
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, RestartRestoresIdenticalPortfolioRiskAndLiquidityBudget) {
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  std::atomic<md::Timestamp> wall_now{market.time};
  options.clock = [&] { return wall_now.load(); };
  std::string portfolio, risk;
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
    auto response = write(engine, "POST", "/api/orders", order(market, "buy", "4.20"));
    ASSERT_EQ(response.status, 201) << response.body;
    engine.stop();
    portfolio = server::handle_api({"GET", "/api/portfolio"}, engine).body;
    risk = server::handle_api({"GET", "/api/risk"}, engine).body;
  }
  // A resumed journal owns its original cash and fee schedule, not new options.
  options.paper.fee_per_contract = Money::parse("9.00");
  options.paper.initial_cash = Money::parse("500.00");
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    EXPECT_EQ(server::handle_api({"GET", "/api/portfolio"}, engine).body, portfolio);
    // Breach lists the held underlyings with the market's close sigma, which is
    // analytics the restarted engine recomputes, not recovered account state.
    const auto account_risk = [](const std::string& body) {
      auto value = json::parse(body);
      for (auto& item : value["breach"]["underlyings"]) item.erase("close_sigma");
      return value;
    };
    EXPECT_EQ(account_risk(server::handle_api({"GET", "/api/risk"}, engine).body), account_risk(risk));
    const auto status = read(engine, "/api/status")["trading"];
    EXPECT_EQ(status["fee_per_contract"], "0.65");
    EXPECT_EQ(status["initial_cash"], "100000.00");
    EXPECT_EQ(json::parse(server::tick_message(engine))["trading"], status);
    auto response = write(engine, "POST", "/api/orders", order(market, "rest", "4.20"));
    ASSERT_EQ(response.status, 201) << response.body;
    EXPECT_EQ(json::parse(response.body)["order"]["status"], "working");
    wall_now = market.time + 20 * md::kNanosPerMinute;
    const auto paper = read(engine, "/api/status")["underlyings"][0]["paper"];
    EXPECT_EQ(paper["accepting"], false);
    EXPECT_EQ(paper["reason"], "FEED_STALLED");
    EXPECT_EQ(json::parse(server::tick_message(engine))["underlyings"][0]["paper"], paper);
    const auto stalled = write(engine, "POST", "/api/orders", order(market, "recovered-stall", "4.20"));
    ASSERT_EQ(stalled.status, 422) << stalled.body;
    EXPECT_EQ(json::parse(stalled.body)["error"]["code"], "FEED_STALLED");
    EXPECT_EQ(json::parse(stalled.body)["error"]["message"], paper["message"]);
    EXPECT_EQ(read(engine, "/api/orders?status=open")["orders"].size(), 1);
    EXPECT_EQ(read(engine, "/api/fills")["fills"].size(), 1);
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    market.time = wall_now.load();
    market.next();
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->recent_fills.size() == 2; }));
  }
  const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(path.string()));
  EXPECT_EQ(recovered.snapshot()->recent_orders.back().reason.code, trading::Reason::FEED_STALLED);
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, AnUnderlyingWithoutAPriceIsNotAnalysedAndCannotDisableTrading) {
  // After a restart an underlying's definitions can arrive a batch before any of its
  // prices. With no market time to value it at, it waits for one: analytics stamped
  // with the wall clock would be ahead of the feed, and fail the account's valuations.
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 10, 10});
    ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
    ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "buy", "4.20")).status, 201);
    engine.stop();
  }
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  // A command runs after the events published before it, so its answer shows how they went.
  const auto again = write(engine, "POST", "/api/orders", order(market, "again", "4.20"));
  EXPECT_EQ(again.status, 201) << again.body;
  EXPECT_EQ(read(engine, "/api/status")["trading"]["reason"], nullptr);
  EXPECT_EQ(engine.metrics("SPX"), nullptr);
  market.next();
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 10, 10});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  engine.stop();
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperAvailability, DisabledFailedJournalFullInboxAndStoppingFailClosed) {
  for (int mode = 0; mode < 4; ++mode) {
    PaperProvider provider;
    auto options = paper_options();
    if (mode == 0) options.paper_enabled = false;
    if (mode == 1) options.paper_journal = std::filesystem::temp_directory_path();
    if (mode == 2) options.command_capacity = 0;
    server::Engine engine(provider, {{"SPX"}}, options); engine.start();
    if (mode == 1) {
      ASSERT_TRUE(wait_for([&] { return engine.status().trading.reason.starts_with("JOURNAL_IO"); }));
    }
    if (mode >= 2) {
      ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    }
    if (mode == 3) engine.stop();
    const auto response = write(engine, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}});
    EXPECT_EQ(response.status, 503) << response.body;
    EXPECT_EQ(json::parse(response.body)["error"]["code"], "TRADING_UNAVAILABLE");
    // A full inbox or stopping engine asks the client to retry; a failed journal does not.
    if (mode >= 2) {
      EXPECT_EQ(response.retry_after, server::kInboxRetrySeconds);
    }
    if (mode == 0) {
      EXPECT_FALSE(engine.status().trading.enabled);
    }
    if (mode == 1) {
      EXPECT_EQ(engine.status().trading.write, "disabled");
    }
  }
}

TEST(PaperAvailability, LockedJournalDisablesEveryWriteButKeepsAnalyticsAndOwnerWorking) {
  const auto path = paper_path();
  auto bytes = [&] {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
  };
  auto options = paper_options();
  options.paper_journal = path;
  PaperProvider first_provider;
  server::Engine first(first_provider, {{"SPX"}}, options);
  first.start();
  ASSERT_NE(first.trading_view(), nullptr);
  const auto limits = read(first, "/api/risk")["limits"];
  const auto before = bytes();
  ASSERT_FALSE(before.empty());
  {
    PaperProvider second_provider;
    server::Engine second(second_provider, {{"SPX"}}, options);
    second.start();
    const auto reason = "JOURNAL_LOCKED: paper journal is in use by another openportd; use --paper-journal to choose another file or --no-paper";
    const auto status = read(second, "/api/status")["trading"];
    EXPECT_FALSE(status["enabled"].get<bool>());
    EXPECT_EQ(status["write"], "disabled");
    EXPECT_EQ(status["reason"], reason);
    EXPECT_EQ(second.trading_view(), nullptr);
    EXPECT_EQ(bytes(), before);

    test::ScriptedMarket market;
    second_provider.sink->publish(md::ContractDefinition{0, market.contract});
    second_provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    second_provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    ASSERT_TRUE(wait_for([&] {
      const auto metrics = second.metrics("SPX");
      return metrics && metrics->as_of == market.time && !metrics->slices.empty();
    }));
    struct Request { std::string method; std::string path; json body; };
    const std::vector<Request> requests{
        {"POST", "/api/orders", order(market)},
        {"DELETE", "/api/orders/1", nullptr},
        {"PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}},
        {"POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}}},
        {"POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "test"}}},
        {"POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5000.00"}}},
        {"POST", "/api/positions/abandon", {{"symbol", market.symbol()}}},
        {"POST", "/api/positions/instruction", {{"symbol", market.symbol()}, {"do_not_exercise", true}}},
        // Unavailability also takes precedence over malformed command bodies.
        {"POST", "/api/orders", json::object()}};
    for (const auto& request : requests) {
      const auto response = write(second, request.method, request.path, request.body);
      EXPECT_EQ(response.status, 503) << response.body;
      EXPECT_EQ(json::parse(response.body)["error"]["code"], "TRADING_UNAVAILABLE");
      EXPECT_EQ(json::parse(response.body)["error"]["message"], reason);
    }
    EXPECT_EQ(bytes(), before);
    ASSERT_EQ(write(first, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "owner"}}).status, 200);
    EXPECT_TRUE(first.status().trading.enabled);
    EXPECT_TRUE(first.status().trading.reason.empty());
    const auto after_owner_write = bytes();
    EXPECT_NE(after_owner_write, before);
    second.stop();
    EXPECT_EQ(bytes(), after_owner_write);
    // Neither the rejected opens nor the second engine's shutdown released the owner's lock.
    EXPECT_THROW(trading::FileJournal::resume(path.string()), trading::TradingError);
  }
  first.stop();
  const auto recovery = trading::FileJournal::read(path.string());
  EXPECT_FALSE(recovery.truncated_final_line);
  const auto recovered = trading::TradingSession::recover(recovery).snapshot();
  EXPECT_TRUE(recovered->risk.kill_latched);
  EXPECT_EQ(recovered->risk.kill_reason, "owner");
  EXPECT_EQ(recovered->account_version, first.trading_view()->snapshot->account_version);
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperWritePolicy, ProtectsEveryWriteAndLeavesReadsOpen) {
  for (const std::string method : {"POST", "PUT", "DELETE"}) {
    server::ApiRequest request{method, "/api/anything"};
    request.content_type = "application/json"; request.host = "localhost:8080";
    for (const std::string address : {"127.0.0.1", "127.0.0.2", "::1"})
      EXPECT_FALSE(server::check_api_write(request, {address, "", {}}));
    auto error = server::check_api_write(request, {"0.0.0.0", "", {}});
    ASSERT_TRUE(error); EXPECT_EQ(error->status, 403);
    EXPECT_EQ(json::parse(error->body)["error"]["code"], "WRITE_DISABLED");
    for (const std::string address : {"127.0.0.1", "0.0.0.0", "::"}) {
      request.authorization = "Bearer wrong";
      error = server::check_api_write(request, {address, "secret", {}});
      ASSERT_TRUE(error);
      EXPECT_EQ(json::parse(error->body)["error"]["code"], "WRITE_TOKEN_REQUIRED");
      request.authorization = "Bearer secret";
      EXPECT_FALSE(server::check_api_write(request, {address, "secret", {}}));
    }
    request.authorization.clear();  // Unauthenticated loopback compatibility, with no configured token.
    request.origin = "https://evil.test";
    error = server::check_api_write(request, {});
    ASSERT_TRUE(error); EXPECT_EQ(json::parse(error->body)["error"]["code"], "ORIGIN_REJECTED");
    EXPECT_FALSE(server::check_api_write(request, {"127.0.0.1", "", {"https://evil.test"}}));
    request.origin = "http://localhost:8080";
    EXPECT_FALSE(server::check_api_write(request, {}));
    request.content_type = "text/plain";
    if (method != "DELETE") {
      error = server::check_api_write(request, {});
      ASSERT_TRUE(error); EXPECT_EQ(error->status, 400);
    }
    request.method = "GET";
    EXPECT_FALSE(server::check_api_write(request, {"0.0.0.0", "secret", {}}));
  }
}
}  // namespace

namespace {
TEST_F(PaperEngine, DailyLossTripsBeforeCrossingOrdersAndTheNextDayStartsFromTheClose) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "position", "4.20")).status, 201);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "rest", "4.00")).status, 201);
  auto limits = read(*engine, "/api/risk")["limits"];
  limits["max_daily_loss"] = "20.00";
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}).status, 200);
  market.next(); quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->risk.kill_latched; }));
  EXPECT_EQ(engine->trading_view()->snapshot->recent_fills.size(), 1);
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "cancelled");
  // The next day's first batch rolls over on the closing marks, before its own option
  // quotes make the account's marks fresh again.
  market.time += md::kNanosPerDay;
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->time == market.time; }));
  EXPECT_EQ(read(*engine, "/api/portfolio")["start_of_day_equity"], "99969.35");
  EXPECT_FALSE(engine->trading_view()->snapshot->valuation_complete);
  quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return engine->trading_view()->snapshot->valuation_complete; }));
  const auto portfolio = read(*engine, "/api/portfolio");
  EXPECT_EQ(portfolio["start_of_day_equity"], portfolio["equity"]);
  EXPECT_EQ(portfolio["day_pnl"], "0.00");
  EXPECT_TRUE(engine->status().trading.kill_latched);
}

TEST(PaperOrdering, MarketBatchPrecedesCancelAndInboxIsBoundedWhileOwnerIsBusy) {
  struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool armed = false, paused = false, released = false;
  } gate;
  auto options = paper_options(); options.command_capacity = 2;
  options.monotonic_clock = [&] {
    std::unique_lock lock(gate.mutex);
    if (gate.armed) {
      gate.armed = false; gate.paused = true; gate.changed.notify_all();
      gate.changed.wait(lock, [&] { return gate.released; });
    }
    return std::chrono::steady_clock::now();
  };
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  struct ReleaseGate {
    Gate& gate;
    ~ReleaseGate() {
      const std::lock_guard lock(gate.mutex);
      gate.released = true;
      gate.changed.notify_all();
    }
  } release{gate};
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  test::ScriptedMarket market;
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(market)).status, 201);
  {
    std::unique_lock lock(gate.mutex);
    gate.armed = true;
    ASSERT_TRUE(gate.changed.wait_for(lock, std::chrono::minutes(5), [&] { return gate.paused; }));
  }
  market.next();
  provider.sink->publish(md::OptionQuote{0, market.time, 3.8, 4, 1, 1});
  server::TradingCommand cancel; cancel.kind = server::TradingCommand::Kind::Cancel; cancel.order_id = 1;
  std::promise<server::TradingReply> first, second;
  const bool accepted_first = engine.post_trading(cancel, [&](auto reply) { first.set_value(std::move(reply)); });
  const bool accepted_second = engine.post_trading(cancel, [&](auto reply) { second.set_value(std::move(reply)); });
  const bool full = engine.post_trading(cancel, [](auto) {});
  {
    const std::lock_guard lock(gate.mutex);
    gate.released = true; gate.changed.notify_all();
  }
  ASSERT_TRUE(accepted_first); ASSERT_TRUE(accepted_second); EXPECT_FALSE(full);
  const auto a = first.get_future().get(), b = second.get_future().get();
  EXPECT_EQ(a.decision.code, trading::Reason::ORDER_TERMINAL);
  EXPECT_EQ(a.view->snapshot->recent_fills.size(), 1);
  EXPECT_EQ(b.view->snapshot->account_version, a.view->snapshot->account_version + 1);
}

class FailingPaperJournal final : public trading::Journal {
 public:
  void append(md::Timestamp, std::string_view, std::string_view) override {
    if (failed) throw trading::TradingError(trading::Reason::JOURNAL_IO, "injected disk failure");
    ++sequence_;
  }
  std::uint64_t sequence() const override { return sequence_; }
  std::string head() const override { return std::string(64, '0'); }
  std::atomic<bool> failed{false};
 private:
  std::uint64_t sequence_ = 0;
};
TEST(PaperAvailability, RuntimeJournalFailurePreservesAccountAndDisablesWrites) {
  auto journal = std::make_shared<FailingPaperJournal>();
  auto options = paper_options(); options.paper_sink = journal;
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto before = engine.trading_view()->snapshot;
  journal->failed = true;
  auto response = write(engine, "POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}});
  EXPECT_EQ(response.status, 503);
  EXPECT_EQ(json::parse(response.body)["error"]["code"], "TRADING_UNAVAILABLE");
  const auto after = engine.trading_view()->snapshot;
  EXPECT_EQ(after->account.cash, before->account.cash);
  EXPECT_EQ(after->account_version, before->account_version);
  EXPECT_TRUE(after->journal_failed);
  EXPECT_EQ(engine.status().trading.write, "disabled");
  EXPECT_TRUE(engine.status().trading.reason.starts_with("JOURNAL_IO"));
  EXPECT_EQ(write(engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "test"}}).status, 503);
}
}  // namespace

namespace {
TEST(PaperQueue, PreservesClosingPrintsWhileOptionQuotesCoalesce) {
  md::EventQueue queue(10, true);
  const test::ScriptedMarket market;
  const auto expiry = market.contract.expiry_time();
  queue.publish(md::UnderlyingQuote{"SPX", expiry - 1, 0, 0, 4999});
  queue.publish(md::UnderlyingQuote{"SPX", expiry, 0, 0, 5001});
  queue.publish(md::UnderlyingQuote{"SPX", expiry + 1, 0, 0, 5010});
  queue.publish(md::OptionQuote{0, expiry - 2, 3, 3.1, 1, 1});
  queue.publish(md::OptionQuote{0, expiry - 1, 4, 4.1, 1, 1});
  std::vector<md::Event> events;
  EXPECT_EQ(queue.drain(events, std::chrono::milliseconds(0)), 4);
  EXPECT_EQ(std::get<md::UnderlyingQuote>(events[1]).last, 5001);
  EXPECT_EQ(std::get<md::OptionQuote>(events[3]).bid, 4);
  EXPECT_EQ(queue.status().coalesced, 1);
}

TEST_F(PaperEngine, ValidatesConditionalOrderFieldsAndReturnsNumericRiskEvidence) {
  seed();
  auto oversized = order(market, "oversized"); oversized["quantity"] = 101;
  const auto size_rejection = write(*engine, "POST", "/api/orders", oversized);
  expect_error(size_rejection, 422, "MAX_ORDER_CONTRACTS");
  EXPECT_EQ(json::parse(size_rejection.body)["error"]["scope"], "SPX");
  auto request = order(market);
  request.erase("limit_price");
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request = order(market); request["type"] = "market";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request.erase("limit_price"); request["time_in_force"] = "day";
  expect_error(write(*engine, "POST", "/api/orders", request), 400, "INVALID_REQUEST");
  request["client_order_id"] = "market"; request["time_in_force"] = "ioc";
  const auto market_order = write(*engine, "POST", "/api/orders", request);
  ASSERT_EQ(market_order.status, 201) << market_order.body;
  EXPECT_EQ(json::parse(market_order.body)["order"]["limit_price"], nullptr);
  EXPECT_EQ(json::parse(market_order.body)["order"]["status"], "filled");
  auto limits = read(*engine, "/api/risk")["limits"];
  limits["aggregate"]["dollar_delta"] = 0;
  limits["per_underlying"]["dollar_delta"] = 0;
  ASSERT_EQ(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "1"}, {"limits", limits}}).status, 200);
  const auto rejection = write(*engine, "POST", "/api/orders", order(market, "risk"));
  expect_error(rejection, 422, "DELTA_LIMIT");
  const auto error = json::parse(rejection.body)["error"];
  EXPECT_GT(error["actual"].get<double>(), 0);
  EXPECT_EQ(error["limit"], 0);
  EXPECT_EQ(error["scope"], "SPX");
  limits["aggregate"]["unexpected"] = 1;
  expect_error(write(*engine, "PUT", "/api/risk/limits", {{"expected_revision", "2"}, {"limits", limits}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", " "}}), 422, "INVALID_REASON");
  expect_error(write(*engine, "POST", "/api/risk/kill", {{"action", "bad"}, {"reason", "test"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", 5000}}), 400, "INVALID_REQUEST");
}
}  // namespace

namespace {
TEST(PaperRecovery, SettlementProvenanceIsDurableAndStopReleasesJournalWriter) {
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPXW260922C05000000");
  PaperProvider provider;
  server::Engine original(provider, {{"SPX"}}, options); original.start();
  ASSERT_TRUE(wait_for([&] { return original.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return original.metrics("SPX") && original.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(original, "POST", "/api/orders", order(market, "pm", "4.20")).status, 201);
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.contract.expiry_time(), 0, 0, 5012});
  ASSERT_TRUE(wait_for([&] { return original.trading_view()->snapshot->positions.empty(); }));
  original.stop();
  const auto recovery = trading::FileJournal::read(path.string());
  bool found = false;
  for (const auto& record : recovery.records) {
    if (record.type != "settlement") continue;
    const auto source = json::parse(record.payload)["settlement_source"];
    EXPECT_EQ(source["kind"], "provider_closing_print");
    EXPECT_EQ(source["provider"], "scripted paper");
    EXPECT_EQ(source["quote_time"], md::format_timestamp(market.contract.expiry_time()));
    found = true;
  }
  EXPECT_TRUE(found);
  PaperProvider replacement_provider;
  server::Engine replacement(replacement_provider, {{"SPX"}}, options); replacement.start();
  ASSERT_TRUE(wait_for([&] { return replacement.trading_view() != nullptr; }));
  EXPECT_EQ(server::handle_api({"GET", "/api/portfolio"}, replacement).body,
            server::handle_api({"GET", "/api/portfolio"}, original).body);
  const auto settlements = read(replacement, "/api/settlements");
  EXPECT_EQ(settlements, read(original, "/api/settlements"));
  ASSERT_EQ(settlements["settlements"].size(), 1);
  const auto record = settlements["settlements"][0];
  EXPECT_EQ(record["symbol"], market.symbol());
  EXPECT_EQ(record["value"], "5012.00");
  EXPECT_EQ(record["cash"], "1200.00");
  EXPECT_EQ(record["realised"], "780.00");
  EXPECT_EQ(record["source"]["kind"], "provider_closing_print");
  const auto trade = read(replacement, "/api/trades")["trades"][0];
  EXPECT_EQ(trade["settlement_value"], record["value"]);
  EXPECT_EQ(trade["settlement_source"], record["source"]["kind"]);
  const auto csv = server::handle_api({"GET", "/api/trades.csv"}, replacement).body;
  EXPECT_NE(csv.find("settlement_value,settlement_source,time_stop_orders\r\n"), std::string::npos);
  EXPECT_NE(csv.find("5012.00,provider_closing_print,\r\n"), std::string::npos);
  const auto missing = server::handle_api({"GET", "/api/settlements?account=missing"}, replacement);
  EXPECT_EQ(missing.status, 404);
  EXPECT_EQ(json::parse(missing.body)["error"]["code"], "UNKNOWN_ACCOUNT");
  const auto invalid = server::handle_api({"GET", "/api/settlements?status=all"}, replacement);
  EXPECT_EQ(invalid.status, 400);
  EXPECT_EQ(json::parse(invalid.body)["error"]["code"], "INVALID_REQUEST");
  replacement.stop();
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, EtfOptionsSettleAtTheQuarterHourOnTheClosingPrint) {
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  options.paper.rules.buying_power = true;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPY260922C00500000");
  PaperProvider provider;
  server::Engine engine(provider, {{"SPY"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPY") && engine.metrics("SPY")->as_of == market.time; }));
  const auto placed = write(engine, "POST", "/api/orders", order(market, "spy", "4.20"));
  ASSERT_EQ(placed.status, 201) << placed.body;
  // The 16:00 print is the close, but the options trade on until 16:15.
  const auto close = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  const auto later = close + 10 * md::kNanosPerMinute;
  provider.sink->publish(md::UnderlyingQuote{"SPY", close, 0, 0, 501});
  provider.sink->publish(md::UnderlyingQuote{"SPY", later, 0, 0, 510});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == later; }));
  EXPECT_EQ(engine.trading_view()->snapshot->positions.size(), 1);
  // At 16:15 it settles on the close, a dollar in the money: 100 shares at the strike.
  provider.sink->publish(md::UnderlyingQuote{"SPY", market.contract.expiry_time(), 0, 0, 499});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->positions.empty(); }));
  const auto snapshot = engine.trading_view()->snapshot;
  ASSERT_EQ(snapshot->stocks.size(), 1);
  EXPECT_EQ(snapshot->stocks[0].position.shares, 100);
  // The journal follows the shares: an open round trip from the expiry exercise.
  const auto trades = read(engine, "/api/trades?status=all");
  ASSERT_EQ(trades["share_trades"].size(), 1);
  const auto& shares = trades["share_trades"][0];
  EXPECT_EQ(shares["kind"], "shares");
  EXPECT_EQ(shares["id"], "s1");
  EXPECT_EQ(shares["symbol"], "SPY");
  EXPECT_EQ(shares["status"], "open");
  EXPECT_EQ(shares["direction"], "long");
  EXPECT_EQ(shares["shares"], 100);
  EXPECT_EQ(shares["average_open"], "501.00");
  EXPECT_EQ(shares["dividends"], "0.00");
  const auto fills = read(engine, "/api/trades?status=all")["stock_fills"];
  ASSERT_EQ(fills.size(), 1);
  EXPECT_EQ(fills[0], json({{"id", "1"}, {"symbol", "SPY"}, {"shares", 100}, {"price", "501.00"},
      {"time", md::format_timestamp(market.contract.expiry_time())}, {"source", "expiry_exercise"}, {"option", market.symbol()}, {"buying_power_after", snapshot->buying_power.available.str()}}));
  EXPECT_EQ(shares["opened_by"], "expiry_exercise");
  EXPECT_EQ(shares["option"], market.symbol());
  EXPECT_EQ(shares["closed_by"], nullptr);
  EXPECT_EQ(read(engine, "/api/trades?status=closed")["share_trades"].size(), 0);
  // A note on the shares goes by their own ID.
  const auto noted = write(engine, "PUT", "/api/trades/s1/note", {{"note", "delivered at expiry"}, {"tags", {"expiry"}}});
  ASSERT_EQ(noted.status, 200) << noted.body;
  EXPECT_EQ(json::parse(noted.body)["trade"], "s1");
  EXPECT_EQ(read(engine, "/api/trades?status=all")["share_trades"][0]["tags"], json::array({"expiry"}));
  EXPECT_EQ(write(engine, "PUT", "/api/trades/s9/note", {{"note", "x"}}).status, 404);
  engine.stop();
  bool found = false;
  for (const auto& record : trading::FileJournal::read(path.string()).records) {
    if (record.type != "settlement") continue;
    EXPECT_EQ(json::parse(record.payload)["settlement_source"]["quote_time"], md::format_timestamp(close));
    found = true;
  }
  EXPECT_TRUE(found);
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, EachShareDeliveryOfOneSettlementKeepsItsEquitySample) {
  // B41: after the first of two deliveries at 16:15 the second position still awaited
  // settlement, equity was not fully marked, and the first delivery lost its sample.
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket low, high;
  low.contract = *md::parse_osi("SPY260922C00495000");
  high.contract = *md::parse_osi("SPY260922C00498000");
  PaperProvider provider;
  server::Engine engine(provider, {{"SPY"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, low.contract});
  provider.sink->publish(md::ContractDefinition{1, high.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPY", low.time, 500, 500, 500});
  provider.sink->publish(md::OptionQuote{0, low.time, 5.4, 5.6, 1, 1});
  provider.sink->publish(md::OptionQuote{1, low.time, 2.8, 3, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPY") && engine.metrics("SPY")->as_of == low.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(low, "long", "5.60")).status, 201);
  auto sell = order(high, "short", "2.80");
  sell["side"] = "sell";
  const auto sold = write(engine, "POST", "/api/orders", sell);
  ASSERT_EQ(sold.status, 201) << sold.body;
  // Both calls finish in the money on the 16:00 close and settle together at 16:15.
  provider.sink->publish(md::UnderlyingQuote{"SPY", md::new_york_to_utc({2026, 9, 22}, 16, 0), 0, 0, 501});
  provider.sink->publish(md::UnderlyingQuote{"SPY", low.contract.expiry_time(), 0, 0, 501});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->positions.empty(); }));
  ASSERT_EQ(engine.trading_view()->snapshot->stock_fills.size(), 2U);
  std::vector<std::string> shares;
  const auto history = read(engine, "/api/account/equity");
  for (const auto& sample : history["samples"])
    if (sample["fill"].is_string() && sample["fill"].get<std::string>().starts_with("s")) shares.push_back(sample["fill"]);
  EXPECT_EQ(shares, (std::vector<std::string>{"s1", "s2"}));
  engine.stop();
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, EtfOptionsSettleOnTheOfficialCloseAsRevised) {
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPY260922C00500000");
  const auto close = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  const auto revised = close + 10 * md::kNanosPerMinute + 49 * md::kNanosPerSecond;
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPY"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    provider.sink->publish(md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    ASSERT_TRUE(wait_for([&] { return engine.metrics("SPY") && engine.metrics("SPY")->as_of == market.time; }));
    ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "spy", "4.20")).status, 201);
    // The first print after the close is an after-hours trade at 500.40. Cboe's close
    // field says 500.20, then revises it to 499.95: out of the money at the pin.
    provider.sink->publish(md::UnderlyingQuote{"SPY", close, 0, 0, 500.40});
    provider.sink->publish(md::UnderlyingClose{"SPY", close + 49 * md::kNanosPerSecond, {2026, 9, 22}, 500.20});
    provider.sink->publish(md::UnderlyingClose{"SPY", revised, {2026, 9, 22}, 499.95});
    ASSERT_TRUE(wait_for([&] {
      const auto view = engine.trading_view();
      const auto& prints = view->snapshot->closing_prints;
      const auto it = prints.find("SPY 2026-09-22");
      return it != prints.end() && it->second.price == Money::parse("499.95");
    }));
    // At 16:15 the call settles on the official close and is not exercised.
    provider.sink->publish(md::UnderlyingQuote{"SPY", market.contract.expiry_time(), 0, 0, 500.30});
    ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->positions.empty(); }));
    EXPECT_TRUE(engine.trading_view()->snapshot->stocks.empty());
    engine.stop();
  }
  bool found = false;
  for (const auto& record : trading::FileJournal::read(path.string()).records) {
    if (record.type != "settlement") continue;
    const auto source = json::parse(record.payload)["settlement_source"];
    EXPECT_EQ(source["kind"], "provider_official_close");
    EXPECT_EQ(source["quote_time"], md::format_timestamp(revised));
    found = true;
  }
  EXPECT_TRUE(found);
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, TheClosingPrintSurvivesARestartBeforeETFOptionsExpire) {
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPY260922C00500000");
  const auto close = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPY"}}, options); engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    provider.sink->publish(md::ContractDefinition{0, market.contract});
    provider.sink->publish(md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
    provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
    ASSERT_TRUE(wait_for([&] { return engine.metrics("SPY") && engine.metrics("SPY")->as_of == market.time; }));
    ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "spy", "4.20")).status, 201);
    provider.sink->publish(md::UnderlyingQuote{"SPY", close, 0, 0, 501});
    ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->closing_prints.contains("SPY 2026-09-22"); }));
    EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["settle_by"], nullptr);
    engine.stop();
  }
  // After the restart the feed's next prints are after-hours ones; the recorded close still settles it.
  PaperProvider provider;
  server::Engine engine(provider, {{"SPY"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPY", close + 10 * md::kNanosPerMinute, 0, 0, 510});
  provider.sink->publish(md::UnderlyingQuote{"SPY", market.contract.expiry_time(), 0, 0, 499});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->positions.empty(); }));
  ASSERT_EQ(engine.trading_view()->snapshot->stocks.size(), 1);
  EXPECT_EQ(engine.trading_view()->snapshot->stock_fills.at(0).price, Money::parse("501"));
  engine.stop();
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperRecovery, WithoutAClosingPrintPMPositionsSettleByHand) {
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPXW260922C05000000");
  const auto later = *md::parse_osi("SPXW260923C05000000");
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, paper_options()); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::ContractDefinition{1, later});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  provider.sink->publish(md::OptionQuote{1, market.time, 9, 9.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "pm", "4.20")).status, 201);
  // Market time passes the close on option quotes alone: no SPX print at or after 16:00.
  const auto after = market.contract.expiry_time() + 5 * md::kNanosPerMinute;
  provider.sink->publish(md::OptionQuote{1, after, 8, 8.2, 1, 1});
  ASSERT_TRUE(wait_for([&] {
    const auto view = engine.trading_view();
    return !view->snapshot->positions.empty() && view->snapshot->positions[0].awaiting_settlement;
  }));
  // For half an hour after the close it waits for the closing print or the last print
  // before the close, and can still settle by itself.
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["settle_by"], "closing_print");
  // Its last print, at 10:00, is too old to stand in for the close, so from 16:30 it
  // settles by hand, even long after.
  const auto deadline = market.contract.expiry_time() + 30 * md::kNanosPerMinute;
  provider.sink->publish(md::OptionQuote{1, deadline - md::kNanosPerSecond, 8, 8.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == deadline - md::kNanosPerSecond; }));
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["settle_by"], "closing_print");
  provider.sink->publish(md::OptionQuote{1, deadline, 8, 8.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == deadline; }));
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["settle_by"], "manual");
  const auto evening = market.contract.expiry_time() + 45 * md::kNanosPerMinute;
  provider.sink->publish(md::OptionQuote{1, evening, 8, 8.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == evening; }));
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["settle_by"], "manual");
  const auto settled = write(engine, "POST", "/api/settlements", {{"symbol", market.symbol()}, {"value", "5010.25"}});
  ASSERT_EQ(settled.status, 200) << settled.body;
  EXPECT_TRUE(engine.trading_view()->snapshot->positions.empty());
  engine.stop();
}

TEST(PaperRecovery, WithoutAClosingPrintTheLastPrintInTheCloseLastMinutesSettles) {
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPXW260922C05000000");
  const auto later = *md::parse_osi("SPXW260923C05000000");
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, options); engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::ContractDefinition{1, later});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 1, 1});
  provider.sink->publish(md::OptionQuote{1, market.time, 9, 9.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.metrics("SPX") && engine.metrics("SPX")->as_of == market.time; }));
  ASSERT_EQ(write(engine, "POST", "/api/orders", order(market, "pm", "4.20")).status, 201);
  // The index prints at 15:58, then the feed carries no more SPX prints that day.
  const auto close = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  const auto last = close - 2 * md::kNanosPerMinute;
  provider.sink->publish(md::UnderlyingQuote{"SPX", last, 0, 0, 5007.5});
  provider.sink->publish(md::OptionQuote{1, close + 5 * md::kNanosPerMinute, 8, 8.2, 1, 1});
  ASSERT_TRUE(wait_for([&] {
    const auto view = engine.trading_view();
    return !view->snapshot->positions.empty() && view->snapshot->positions[0].awaiting_settlement;
  }));
  // Half an hour on, it stands in for the close.
  provider.sink->publish(md::OptionQuote{1, close + 30 * md::kNanosPerMinute, 8, 8.2, 1, 1});
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->positions.empty(); }));
  EXPECT_EQ(engine.trading_view()->snapshot->closing_prints.at("SPX 2026-09-22").price, Money::parse("5007.5"));
  engine.stop();
  bool found = false;
  for (const auto& record : trading::FileJournal::read(path.string()).records) {
    if (record.type != "settlement") continue;
    const auto source = json::parse(record.payload)["settlement_source"];
    EXPECT_EQ(source["kind"], "provider_last_print_before_close");
    EXPECT_EQ(source["quote_time"], md::format_timestamp(last));
    found = true;
  }
  EXPECT_TRUE(found);
  std::filesystem::remove_all(path.parent_path());
}

TEST(PaperPlans, ScalingRulesArePartOfPresetIdentity) {
  const auto* preset = server::find_plan("funded-scaling-50k");
  ASSERT_NE(preset, nullptr);
  auto rules = preset->rules;
  EXPECT_TRUE(server::follows_plan(*preset, preset->initial_cash, rules));
  rules.scaling.front().contracts = 1;
  EXPECT_FALSE(server::follows_plan(*preset, preset->initial_cash, rules));
  rules = preset->rules;
  rules.size_scaling = trading::SizeScaling{10, 2, 80, 25, Money::parse("100000")};
  EXPECT_FALSE(server::follows_plan(*preset, preset->initial_cash, rules));
  auto growing = *preset;
  growing.rules = rules;
  trading::SessionConfig config; config.initial_cash = preset->initial_cash; config.rules = rules;
  trading::TradingSession session(config, 0);
  auto progress = session.snapshot()->evaluation.size_scaling;
  ASSERT_TRUE(progress);
  rules.max_drawdown = rules.max_drawdown.prorate(125, 100);
  rules.daily_loss_limit = rules.daily_loss_limit.prorate(125, 100);
  if (rules.lock_balance > Money{}) rules.lock_balance = rules.lock_balance + Money::parse("12500");
  EXPECT_TRUE(server::follows_plan(growing, progress->original,
      trading::original_program_rules(rules, Money::parse("62500"), progress)));
}

TEST(PaperPlans, PresetsListExactRules) {
  PaperProvider provider;
  server::Engine engine(provider, {{"SPX"}}, paper_options());
  const auto plans = read(engine, "/api/plans")["plans"];
  ASSERT_EQ(plans.size(), 29);
  EXPECT_EQ(plans[0]["id"], "practice");
  EXPECT_EQ(plans[0]["rules"]["profit_target"], nullptr);
  EXPECT_EQ(plans[0]["rules"]["buying_power"], true);
  EXPECT_EQ(plans[0]["unlocked_by"], nullptr);
  // These presets leave the later evaluation rules off.
  const auto off = [](json rules) {
    rules["plan_id"] = server::find_plan_named(rules.at("plan").get<std::string>())->id;
    rules.update({{"evaluation_fee", "0.00"}, {"reset_fee", "0.00"}, {"activation_fee", "0.00"}, {"max_resets", 0},
                  {"min_hold_seconds", 0}, {"microscalp_seconds", 0}, {"microscalp_percent", 0}, {"min_trades", 0}, {"trade_consistency_percent", 0}, {"lock_at_start", false}, {"profit_basis", "equity"}, {"daily_loss_limit", nullptr},
                  {"daily_loss_basis", "equity"}, {"daily_loss_action", "lock"}, {"consistency_percent", 0},
                  {"consistency_basis", "total"}, {"min_trading_days", 0}, {"min_profitable_days", 0},
                  {"profitable_day_profit", nullptr}, {"day_end", "17:00"}, {"max_contracts_held", 0},
                  {"max_volume_percent", 0}, {"no_hedging", false}, {"no_counter_positions", false},
                  {"require_stop_loss", false}, {"max_trade_risk", nullptr}, {"max_trade_risk_percent", 0},
                  {"time_limit_days", 0}, {"inactivity_days", 0}, {"underlyings", json::array()},
                  {"trading_start", nullptr}, {"trading_end", nullptr}, {"flat_time", nullptr}, {"no_overnight", false},
                  {"scaling", json::array()}, {"size_scaling", nullptr}, {"events", json::array()},
                  {"news_before_minutes", 0}, {"news_after_minutes", 0}, {"news_action", "block"},
                  {"hold_restrictions", json::array()}, {"hold_cutoff", "15:45"}});
    return rules;
  };
  const auto scaling = plans[19];
  EXPECT_EQ(scaling["id"], "funded-scaling-50k");
  EXPECT_EQ(scaling["unlocked_by"], "intraday-50k");
  EXPECT_EQ(scaling["initial_cash"], "50000.00");
  auto scaling_rules = plans[8]["rules"];
  scaling_rules.update({{"plan", "Funded Scaling 50K"}, {"plan_id", "funded-scaling-50k"}});
  scaling_rules["scaling"] = {{{"profit", "0.00"}, {"contracts", 2}}, {{"profit", "1500.00"}, {"contracts", 3}},
                              {{"profit", "2000.00"}, {"contracts", 5}}};
  EXPECT_EQ(scaling["rules"], scaling_rules);
  const auto intraday = plans[3];
  EXPECT_EQ(intraday["id"], "intraday-100k");
  EXPECT_EQ(intraday["name"], "Intraday 100K");
  EXPECT_EQ(intraday["initial_cash"], "100000.00");
  EXPECT_EQ(intraday["rules"], off({{"plan", "Intraday 100K"}, {"phase", "evaluation"}, {"profit_target", "10000.00"},
      {"max_drawdown", "5000.00"}, {"drawdown_mode", "intraday"}, {"lock_balance", nullptr}, {"buy_only", true},
      {"defined_risk", false}, {"buying_power", true}, {"slippage_ticks", 0}, {"margin", "strategy"},
      {"account_type", "margin"}, {"house_margin_percent", 0}, {"pm_vol_shock", 0}, {"expiry_cutoff_seconds", 300}, {"payouts", nullptr}}));
  const auto funded = plans[9];
  EXPECT_EQ(funded["id"], "funded-intraday-100k");
  EXPECT_EQ(funded["name"], "Funded Intraday 100K");
  EXPECT_EQ(funded["unlocked_by"], "intraday-100k");
  EXPECT_EQ(funded["initial_cash"], "100000.00");
  EXPECT_EQ(funded["rules"], off({{"plan", "Funded Intraday 100K"}, {"phase", "funded"}, {"profit_target", nullptr},
      {"max_drawdown", "5000.00"}, {"drawdown_mode", "intraday"}, {"lock_balance", "100000.00"}, {"buy_only", true},
      {"defined_risk", false}, {"buying_power", true}, {"slippage_ticks", 0}, {"margin", "strategy"},
      {"account_type", "margin"}, {"house_margin_percent", 0}, {"pm_vol_shock", 0}, {"expiry_cutoff_seconds", 300},
      {"payouts", {{"qualifying_profit", "200.00"}, {"qualifying_days", 8}, {"withdrawal_percent", 50},
                   {"split_percent", 80}, {"minimum", "1000.00"},
                   {"caps", {"2000.00", "3000.00", "4000.00", "6000.00"}}, {"consistency_percents", json::array()}, {"buffer", "0.00"}, {"buffer_payouts", 0}}}}));
  EXPECT_EQ(plans[7]["rules"]["payouts"]["qualifying_profit"], "100.00");
  EXPECT_EQ(plans[8]["rules"]["payouts"]["qualifying_profit"], "150.00");
  EXPECT_EQ(plans[10]["id"], "funded-eod-25k");
  EXPECT_EQ(plans[10]["rules"]["payouts"]["qualifying_profit"], "100.00");
  // The plan objectives' presets: a static floor, and one that locks at the start.
  const auto fixed = plans[14];
  EXPECT_EQ(fixed["id"], "static-50k");
  EXPECT_EQ(fixed["initial_cash"], "50000.00");
  EXPECT_EQ(fixed["unlocked_by"], nullptr);
  auto fixed_rules = off({{"plan", "Static 50K"}, {"phase", "evaluation"}, {"profit_target", "5000.00"},
      {"max_drawdown", "4000.00"}, {"drawdown_mode", "static"}, {"lock_balance", nullptr}, {"buy_only", false},
      {"defined_risk", false}, {"buying_power", true}, {"slippage_ticks", 0}, {"margin", "strategy"},
      {"account_type", "margin"}, {"house_margin_percent", 0}, {"pm_vol_shock", 0}, {"expiry_cutoff_seconds", 300},
      {"payouts", nullptr}});
  fixed_rules.update({{"profit_basis", "balance"}, {"daily_loss_limit", "2000.00"}, {"daily_loss_basis", "balance"},
                      {"daily_loss_action", "fail"}, {"min_trading_days", 4}, {"day_end", "18:00"}});
  EXPECT_EQ(fixed["rules"], fixed_rules);
  const auto locking = plans[18];
  EXPECT_EQ(locking["id"], "locking-100k");
  EXPECT_EQ(locking["name"], "Locking 100K");
  auto locking_rules = off({{"plan", "Locking 100K"}, {"phase", "evaluation"}, {"profit_target", "6000.00"},
      {"max_drawdown", "4000.00"}, {"drawdown_mode", "end_of_day"}, {"lock_balance", nullptr}, {"buy_only", false},
      {"defined_risk", false}, {"buying_power", true}, {"slippage_ticks", 0}, {"margin", "strategy"},
      {"account_type", "margin"}, {"house_margin_percent", 0}, {"pm_vol_shock", 0}, {"expiry_cutoff_seconds", 300},
      {"payouts", nullptr}});
  locking_rules.update({{"lock_at_start", true}, {"daily_loss_limit", "2000.00"}, {"consistency_percent", 50}});
  EXPECT_EQ(locking["rules"], locking_rules);
  EXPECT_EQ(plans[10]["rules"]["drawdown_mode"], "end_of_day");
  EXPECT_EQ(plans[10]["rules"]["buy_only"], false);
  const auto eod = plans[4];
  EXPECT_EQ(eod["id"], "eod-25k");
  EXPECT_EQ(eod["rules"]["profit_target"], "3000.00");
  EXPECT_EQ(eod["rules"]["max_drawdown"], "1500.00");
  EXPECT_EQ(eod["rules"]["drawdown_mode"], "end_of_day");
  EXPECT_EQ(eod["rules"]["buy_only"], false);
}

TEST(PaperAttempts, OldSummariesExposeNullProvenance) {
  class Source final : public server::MetricsSource {
   public:
    std::shared_ptr<server::TradingView> view = std::make_shared<server::TradingView>();
    std::vector<std::string> symbols() const override { return {}; }
    std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
    server::EngineStatus status() const override { return {}; }
    using server::MetricsSource::trading_view;
    std::shared_ptr<const server::TradingView> trading_view() const override { return view; }
  } source;
  test::ScriptedMarket market;
  trading::TradingSession session({}, market.time);
  auto snapshot = std::make_shared<trading::TradingSnapshot>(*session.snapshot());
  trading::AttemptSummary old;
  old.attempt = 1; old.started = market.time; old.ended = market.time;
  old.status = trading::EvaluationStatus::Failed;
  old.decision_code = trading::Reason::DRAWDOWN_FLOOR;
  snapshot->attempts.push_back(old);
  source.view->snapshot = snapshot;
  source.view->config = session.config();
  const auto attempt = read(source, "/api/account")["attempts"][0];
  for (const auto* key : {"rules", "decided_at", "decided_equity", "peak", "floor"}) {
    ASSERT_TRUE(attempt.contains(key)) << key;
    EXPECT_TRUE(attempt[key].is_null()) << key;
  }
  EXPECT_EQ(attempt["decision_code"], "DRAWDOWN_FLOOR");
}

TEST_F(PaperEngine, ScalingFundedPresetRequiresItsOwnPassedEvaluation) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"plan", "intraday-50k"}, {"reason", "evaluation"}}).status, 200);
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "funded-scaling-50k"}, {"reason", "too soon"}}), 422, "PLAN_LOCKED");
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open", "4.20")).status, 201);
  quote("54.15", "54.35");
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/account")["evaluation"]["status"] == "passed"; }));
  const auto response = write(*engine, "POST", "/api/account/reset", {{"plan", "funded-scaling-50k"}, {"reason", "funded"}});
  ASSERT_EQ(response.status, 200) << response.body;
  const auto account = json::parse(response.body);
  EXPECT_EQ(account["rules"]["phase"], "funded");
  EXPECT_EQ(account["evaluation"]["scaling"]["limit"], 2);
  EXPECT_EQ(account["evaluation"]["scaling"]["profit"], "0.00");
  engine->stop();
}

TEST_F(PaperEngine, SizeScalingRulesParseValidateAndExposeReviewAndAttemptProvenance) {
  seed();
  auto account = read(*engine, "/api/account");
  EXPECT_EQ(account["rules"]["size_scaling"], nullptr);
  EXPECT_EQ(account["evaluation"]["size_scaling"], nullptr);
  auto rules = account["rules"];
  rules["plan"] = "Custom capital growth";
  rules["phase"] = "funded";
  rules["profit_target"] = nullptr;
  rules["payouts"] = {{"qualifying_profit", "1"}, {"qualifying_days", 1}, {"withdrawal_percent", 50},
      {"split_percent", 80}, {"minimum", "1"}, {"caps", json::array()}};
  const json size{{"profit_percent", 10}, {"payouts", 2}, {"days", 80}, {"increase_percent", 25}, {"max_balance", "100000.00"}};
  rules["size_scaling"] = size;
  const auto reset = [&](const json& r) { return write(*engine, "POST", "/api/account/reset",
      {{"initial_cash", "50000"}, {"rules", r}, {"reason", "capital growth test"}}); };
  auto response = reset(rules);
  ASSERT_EQ(response.status, 200) << response.body;
  account = json::parse(response.body);
  EXPECT_EQ(account["rules"]["size_scaling"], size);
  const auto status = account["evaluation"]["size_scaling"];
  EXPECT_EQ(status["size"], "50000.00"); EXPECT_EQ(status["original"], "50000.00");
  EXPECT_EQ(status["max_balance"], "100000.00"); EXPECT_EQ(status["next_size"], "62500.00");
  EXPECT_EQ(status["period_days"], 0); EXPECT_EQ(status["days_required"], 80);
  EXPECT_EQ(status["period_profit"], "0.00"); EXPECT_EQ(status["profit_required"], "5000.00");
  EXPECT_EQ(status["period_payouts"], 0); EXPECT_EQ(status["payouts_required"], 2);
  EXPECT_EQ(status["period_started"], account["evaluation"]["day"]);
  EXPECT_EQ(status["history"], json::array());
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"profit_percent", 0}, {"profit_percent", 101}, {"payouts", -1}, {"payouts", 101},
      {"days", 0}, {"days", 367}, {"increase_percent", 0}, {"increase_percent", 101}, {"max_balance", "49999"}}) {
    auto bad = rules; bad["size_scaling"][key] = value;
    expect_error(reset(bad), 400, "INVALID_RULES");
  }
  for (const auto& value : std::vector<json>{false, 5, json::array(), {{"profit_percent", 10}}}) {
    auto bad = rules; bad["size_scaling"] = value;
    expect_error(reset(bad), 400, "INVALID_REQUEST");
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"days", 1.5}, {"payouts", "2"}, {"profit_percent", true}, {"increase_percent", nullptr},
      {"max_balance", 100000}, {"unknown", 1}}) {
    auto bad = rules; bad["size_scaling"][key] = value;
    expect_error(reset(bad), 400, "INVALID_REQUEST");
  }
  for (const auto* phase : {"evaluation", "verification"}) {
    auto evaluation = rules; evaluation["phase"] = phase; evaluation["payouts"] = nullptr;
    expect_error(reset(evaluation), 400, "INVALID_RULES");
  }
  auto disabled = rules; disabled["size_scaling"] = nullptr;
  response = reset(disabled);
  ASSERT_EQ(response.status, 200) << response.body;
  account = json::parse(response.body);
  EXPECT_EQ(account["evaluation"]["size_scaling"], nullptr);
  EXPECT_EQ(account["attempts"].back()["rules"]["size_scaling"], size);
}

TEST_F(PaperEngine, SizeScalingAccountViewReportsTheCapitalCreditAndNewReview) {
  seed();
  const json rules{{"plan", "Size review"}, {"phase", "funded"}, {"profit_target", nullptr}, {"max_drawdown", "5000"},
      {"drawdown_mode", "intraday"}, {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0},
      {"payouts", {{"qualifying_profit", "10"}, {"qualifying_days", 1}, {"withdrawal_percent", 50},
          {"split_percent", 80}, {"minimum", "10"}, {"caps", json::array()}}},
      {"size_scaling", {{"profit_percent", 1}, {"payouts", 0}, {"days", 1}, {"increase_percent", 25}, {"max_balance", "100000"}}}};
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules}, {"reason", "size view"},
      {"evaluation_fee", "100.000001"}, {"reset_fee", "25.000002"}, {"max_resets", 2}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules},
      {"reason", "first reset"}}).status, 200);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "size-open", "4.20")).status, 201);
  market.next(); quote("10.00", "10.20");
  auto close = order(market, "size-close", "10.00"); close["side"] = "sell";
  ASSERT_EQ(write(*engine, "POST", "/api/orders", close).status, 201);
  market.time = md::new_york_to_utc({2026, 9, 23}, 10, 0); quote();
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/account")["evaluation"]["day"] == "2026-09-23"; }));
  const auto account = read(*engine, "/api/account");
  const auto& size = account["evaluation"]["size_scaling"];
  EXPECT_EQ(size["size"], "62500.00"); EXPECT_EQ(size["original"], "50000.00");
  EXPECT_EQ(size["original_max_drawdown"], "5000.00"); EXPECT_EQ(size["original_daily_loss_limit"], "0.00");
  EXPECT_EQ(size["period_days"], 0); EXPECT_EQ(size["period_profit"], "0.00");
  EXPECT_EQ(size["profit_required"], "625.00"); EXPECT_EQ(size["next_size"], "75000.00");
  EXPECT_EQ(size["history"], (json::array({{{"day", "2026-09-23"}, {"old", "50000.00"}, {"size", "62500.00"}}})));
  EXPECT_EQ(account["payout"]["profit"], "578.70");
  EXPECT_EQ(account["rules"]["max_drawdown"], "6250.00");
  EXPECT_EQ(account["costs"]["resets_used"], 1);
  auto response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", account["rules"]},
      {"reason", "restart original size"}});
  ASSERT_EQ(response.status, 200) << response.body;
  test::capture_contract("time-rules", "POST", "/api/account/reset", response);
  const auto restarted = json::parse(response.body);
  EXPECT_EQ(restarted["costs"]["resets_used"], 2);
  EXPECT_EQ(restarted["costs"]["fee_kind"], "reset");
  EXPECT_EQ(restarted["costs"]["fee_charged"], "25.000002");
  EXPECT_EQ(restarted["rules"]["max_resets"], 2);
  EXPECT_EQ(restarted["rules"]["max_drawdown"], "5000.00");
  EXPECT_EQ(restarted["evaluation"]["size_scaling"]["original_max_drawdown"], "5000.00");
  EXPECT_EQ(restarted["evaluation"]["starting_balance"], "50000.00");
  EXPECT_EQ(restarted["attempts"].back()["starting_balance"], "62500.00");
  EXPECT_EQ(restarted["attempts"].back()["rules"]["max_drawdown"], "6250.00");
  response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules}, {"reason", "exhausted"}});
  expect_error(response, 409, "RESET_LIMIT");
  test::capture_contract("time-rules", "POST", "/api/account/reset", response);
}

TEST_F(PaperEngine, ScalingRulesAccountViewPreviewsOrdersAndValidation) {
  seed();
  EXPECT_EQ(read(*engine, "/api/account")["evaluation"]["scaling"], nullptr);
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "funded-scaling-50k"}, {"reason", "skip"}}), 422, "PLAN_LOCKED");
  auto rules = read(*engine, "/api/account")["rules"];
  rules["plan"] = "Custom scaling";
  rules["scaling"] = {{{"profit", "0.00"}, {"contracts", 2}}, {{"profit", "1500.00"}, {"contracts", 3}}};
  auto reset = [&](const json& r) { return write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", r}, {"reason", "scaling test"}}); };
  auto response = reset(rules);
  ASSERT_EQ(response.status, 200) << response.body;
  auto account = json::parse(response.body);
  EXPECT_EQ(account["rules"]["scaling"], rules["scaling"]);
  EXPECT_EQ(account["evaluation"]["scaling"], (json{{"limit", 2}, {"held", 0}, {"profit", "0.00"},
      {"next", {{"profit", "1500.00"}, {"contracts", 3}}}}));
  auto large = order(market, "too-many", "4.20"); large["quantity"] = 3;
  auto preview = write(*engine, "POST", "/api/orders/preview", large);
  ASSERT_EQ(preview.status, 200) << preview.body;
  EXPECT_EQ(json::parse(preview.body)["reason"]["code"], "SCALING_LIMIT");
  expect_error(write(*engine, "POST", "/api/orders", large), 422, "SCALING_LIMIT");
  large["client_order_id"] = "fits"; large["quantity"] = 2;
  ASSERT_EQ(write(*engine, "POST", "/api/orders", large).status, 201);
  EXPECT_EQ(read(*engine, "/api/account")["evaluation"]["scaling"]["held"], 2);
  for (const auto& steps : std::vector<json>{
      {{{"profit", "1"}, {"contracts", 2}}}, {{{"profit", "0"}, {"contracts", 0}}},
      {{{"profit", "0"}, {"contracts", 10001}}},
      {{{"profit", "0"}, {"contracts", 2}}, {{"profit", "0"}, {"contracts", 3}}},
      {{{"profit", "0"}, {"contracts", 3}}, {{"profit", "1"}, {"contracts", 2}}}}) {
    auto bad = rules; bad["scaling"] = steps;
    expect_error(reset(bad), 400, "INVALID_RULES");
  }
  for (const auto& value : std::vector<json>{nullptr, 3, {{{"profit", 0}, {"contracts", 2}}},
      {{{"profit", "0"}, {"contracts", 2.5}}}, {{{"profit", "0"}, {"contracts", "2"}}},
      {{{"profit", "0"}, {"contracts", 2}, {"unknown", 1}}}}) {
    auto bad = rules; bad["scaling"] = value;
    expect_error(reset(bad), 400, "INVALID_REQUEST");
  }
  engine->stop();
}

TEST_F(PaperEngine, AccountViewWithoutRulesHasNoTargetOrFloor) {
  const auto account = read(*engine, "/api/account");
  EXPECT_EQ(account["rules"]["plan"], nullptr);
  EXPECT_EQ(account["rules"]["plan_id"], nullptr);
  EXPECT_EQ(account["evaluation"]["enabled"], false);
  EXPECT_EQ(account["evaluation"]["status"], "active");
  EXPECT_EQ(account["evaluation"]["attempt"], 1);
  EXPECT_EQ(account["evaluation"]["starting_balance"], "100000.00");
  EXPECT_EQ(account["evaluation"]["floor"], nullptr);
  EXPECT_EQ(account["evaluation"]["target_equity"], nullptr);
  EXPECT_EQ(account["buying_power"]["available"], "100000.00");
  EXPECT_TRUE(account["attempts"].empty());
  const auto trading = read(*engine, "/api/status")["trading"];
  EXPECT_EQ(trading["plan"], nullptr);
  EXPECT_EQ(trading["evaluation"], nullptr);
  engine->stop();
}

TEST_F(PaperEngine, ResetToPresetStartsAttemptAndTradesSeparateAttempts) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open", "4.20")).status, 201);
  auto trades = read(*engine, "/api/trades")["trades"];
  ASSERT_EQ(trades.size(), 1);
  EXPECT_EQ(trades[0]["status"], "open");
  EXPECT_EQ(trades[0]["direction"], "long");
  EXPECT_EQ(trades[0]["average_open"], "4.20");
  EXPECT_EQ(trades[0]["mark"], "4.10");
  EXPECT_EQ(trades[0]["unrealised"], "-10.00");
  EXPECT_EQ(trades[0]["attempt"], 1);
  EXPECT_EQ(read(*engine, "/api/orders?status=all")["orders"][0]["origin"], "user");

  const auto before_reset = read(*engine, "/api/account");
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "intraday-25k"}, {"reason", "start evaluation"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto account = json::parse(reset.body);
  EXPECT_EQ(account["rules"]["plan"], "Intraday 25K");
  const auto evaluation = account["evaluation"];
  EXPECT_EQ(evaluation["enabled"], true);
  EXPECT_EQ(evaluation["attempt"], 2);
  EXPECT_EQ(evaluation["starting_balance"], "25000.00");
  EXPECT_EQ(evaluation["equity"], "25000.00");
  EXPECT_EQ(evaluation["floor"], "23750.00");
  EXPECT_EQ(evaluation["drawdown_buffer"], "1250.00");
  EXPECT_EQ(evaluation["target_equity"], "27500.00");
  EXPECT_EQ(evaluation["target_remaining"], "2500.00");
  EXPECT_EQ(account["attempts"][0]["final_equity"], "99989.35");
  EXPECT_EQ(account["attempts"][0]["rules"], before_reset["rules"]);
  EXPECT_EQ(account["attempts"][0]["peak"], before_reset["evaluation"]["peak"]);
  EXPECT_EQ(account["attempts"][0]["floor"], nullptr);
  EXPECT_EQ(account["attempts"][0]["decided_at"], nullptr);
  EXPECT_EQ(account["attempts"][0]["decided_equity"], nullptr);
  EXPECT_EQ(read(*engine, "/api/account"), account);

  EXPECT_TRUE(read(*engine, "/api/trades")["trades"].empty());
  trades = read(*engine, "/api/trades?status=closed&attempt=all")["trades"];
  ASSERT_EQ(trades.size(), 1);
  EXPECT_EQ(trades[0]["closure"], "reset");
  EXPECT_EQ(trades[0]["attempt"], 1);
  EXPECT_EQ(trades[0]["gross"], "-10.00");
  EXPECT_EQ(trades[0]["fees"], "0.65");
  EXPECT_EQ(trades[0]["net"], "-10.65");
  EXPECT_EQ(trades[0]["cost"], "420.00");
  EXPECT_NEAR(trades[0]["return"].get<double>(), -10.65 / 420, 1e-12);
  const auto trading = read(*engine, "/api/status")["trading"];
  EXPECT_EQ(trading["plan"], "Intraday 25K");
  EXPECT_EQ(trading["evaluation"], "active");
  EXPECT_EQ(json::parse(server::tick_message(*engine))["trading"], trading);

  auto sell = order(market, "naked-sell", "4.00");
  sell["side"] = "sell";
  expect_error(write(*engine, "POST", "/api/orders", sell), 422, "BUY_ONLY");
  engine->stop();
}

TEST(PaperStocks, ExerciseDeliversSharesThatThePortfolioShowsAndCloses) {
  PaperProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto contract = *md::parse_osi("SPY261022C00500000");
  const auto symbol = contract.osi_symbol();
  auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto quote = [&] {
    provider.sink->publish(md::UnderlyingQuote{"SPY", time, 519.9, 520.1, 520});
    provider.sink->publish(md::OptionQuote{0, time, 21.0, 21.2, 10, 10});
    ASSERT_TRUE(wait_for([&] { const auto m = engine.metrics("SPY"); return m && m->as_of == time && !m->slices.empty(); }));
  };
  provider.sink->publish(md::ContractDefinition{0, contract});
  quote();
  const auto bought = write(engine, "POST", "/api/orders", {{"client_order_id", "calls"}, {"symbol", symbol}, {"side", "buy"},
      {"type", "limit"}, {"quantity", 2}, {"limit_price", "21.20"}, {"time_in_force", "day"}});
  ASSERT_EQ(bought.status, 201) << bought.body;
  // A batch with the calls held carries SPY's price for exercise.
  time += md::kNanosPerSecond;
  quote();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view()->snapshot->time == time; }));
  const auto exercised = write(engine, "POST", "/api/positions/exercise", {{"symbol", symbol}, {"quantity", 1}});
  ASSERT_EQ(exercised.status, 200) << exercised.body;
  auto portfolio = json::parse(exercised.body);
  ASSERT_EQ(portfolio["stocks"].size(), 1);
  EXPECT_EQ(portfolio["stocks"][0]["symbol"], "SPY");
  EXPECT_EQ(portfolio["stocks"][0]["shares"], 100);
  EXPECT_EQ(portfolio["stocks"][0]["average_price"], "520.00");
  EXPECT_EQ(portfolio["positions"][0]["quantity"], 1);
  const auto trades = read(engine, "/api/trades")["trades"];
  EXPECT_EQ(trades[0]["closed_contracts"], 1);
  const auto partly = write(engine, "POST", "/api/stocks/close", {{"symbol", "SPY"}, {"shares", 40}});
  ASSERT_EQ(partly.status, 200) << partly.body;
  EXPECT_EQ(json::parse(partly.body)["stocks"][0]["shares"], 60);
  const auto rest = write(engine, "POST", "/api/stocks/close", {{"symbol", "SPY"}});
  ASSERT_EQ(rest.status, 200) << rest.body;
  EXPECT_TRUE(json::parse(rest.body)["stocks"].empty());
  const auto none = write(engine, "POST", "/api/stocks/close", {{"symbol", "SPY"}});
  EXPECT_EQ(none.status, 422) << none.body;
  EXPECT_EQ(write(engine, "POST", "/api/stocks/close", {{"symbol", "spy"}}).status, 400);
  EXPECT_EQ(write(engine, "POST", "/api/positions/exercise", {{"symbol", symbol}, {"quantity", 0}}).status, 400);
  EXPECT_EQ(write(engine, "POST", "/api/positions/exercise", {{"symbol", symbol}, {"quantity", 5}}).status, 422);
  engine.stop();
}

TEST_F(PaperEngine, PortfolioExplainsTheDaysPnlByGreek) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open", "4.20")).status, 201);
  const auto portfolio = read(*engine, "/api/portfolio");
  ASSERT_EQ(portfolio["margin"].size(), 1U);
  EXPECT_EQ(portfolio["margin"][0]["allocation"], "exact");
  const auto a = portfolio["attribution"];
  for (const auto* key : {"delta", "gamma", "vega", "theta", "other", "costs", "total"}) EXPECT_TRUE(a[key].is_number()) << key;
  // One contract bought at the 4.20 ask against a 4.10 mark, and the fee.
  EXPECT_DOUBLE_EQ(a["costs"].get<double>(), -10.65);
  EXPECT_DOUBLE_EQ(a["total"].get<double>(), std::stod(portfolio["day_pnl"].get<std::string>()));
  EXPECT_EQ(portfolio["positions"][0]["attribution"], a);
  engine->stop();
}

TEST_F(PaperEngine, TradesTakeNotesAndTags) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open", "4.20")).status, 201);
  const auto id = read(*engine, "/api/trades")["trades"][0]["id"].get<std::string>();
  EXPECT_EQ(read(*engine, "/api/trades")["trades"][0]["note"], "");
  EXPECT_EQ(read(*engine, "/api/trades")["trades"][0]["tags"], json::array());
  const auto path = "/api/trades/" + id + "/note";
  const auto saved = write(*engine, "PUT", path, {{"note", "Bought the dip "}, {"tags", {"Dip", "spx"}}});
  ASSERT_EQ(saved.status, 200) << saved.body;
  const auto body = json::parse(saved.body);
  EXPECT_EQ(body["trade"], id);
  EXPECT_EQ(body["note"], "Bought the dip");
  EXPECT_EQ(body["tags"], (json{"dip", "spx"}));
  EXPECT_EQ(body["account_version"], read(*engine, "/api/status")["trading"]["account_version"]);
  const auto trade = read(*engine, "/api/trades")["trades"][0];
  EXPECT_EQ(trade["note"], "Bought the dip");
  EXPECT_EQ(trade["tags"], (json{"dip", "spx"}));
  expect_error(write(*engine, "PUT", "/api/trades/999/note", {{"note", "x"}}), 404, "UNKNOWN_TRADE");
  expect_error(write(*engine, "PUT", path, {{"tags", {"a,b"}}}), 422, "INVALID_NOTE");
  expect_error(write(*engine, "PUT", path, {{"note", 5}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "PUT", path, {{"memo", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "PUT", "/api/trades/x/note", {{"note", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "PUT", "/api/trades/" + id, {{"note", "x"}}), 404, "NOT_FOUND");
  const auto cleared = write(*engine, "PUT", path, json::object());
  ASSERT_EQ(cleared.status, 200) << cleared.body;
  EXPECT_EQ(json::parse(cleared.body)["note"], "");
  EXPECT_EQ(read(*engine, "/api/trades")["trades"][0]["tags"], json::array());
  engine->stop();
}

TEST_F(PaperEngine, DayNotesAreValidatedAndKeptPerAccount) {
  engine->stop();
  const auto journal = paper_path();
  auto options = paper_options(); options.paper_accounts = journal.parent_path() / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  const auto saved = write(*engine, "PUT", "/api/days/2026-09-22/note", {{"plan", "Wait"}, {"review", "Good exit"}});
  ASSERT_EQ(saved.status, 200) << saved.body;
  EXPECT_EQ(json::parse(saved.body)["note"]["plan"], "Wait");
  EXPECT_EQ(read(*engine, "/api/trades")["day_notes"]["2026-09-22"]["review"], "Good exit");
  const auto created = write(*engine, "POST", "/api/accounts", {{"name", "Other"}, {"plan", "practice"}});
  ASSERT_EQ(created.status, 201) << created.body;
  const auto id = json::parse(created.body)["account"]["id"].get<std::string>();
  EXPECT_TRUE(read(*engine, "/api/trades?account=" + id)["day_notes"].empty());
  expect_error(write(*engine, "PUT", "/api/days/2026-02-30/note", {{"plan", ""}, {"review", ""}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "PUT", "/api/days/2026-09-22/note", {{"plan", std::string(2001, 'x')}, {"review", ""}}), 422, "INVALID_NOTE");
  expect_error(write(*engine, "PUT", "/api/days/2026-09-22/note", {{"plan", 1}, {"review", ""}}), 400, "INVALID_REQUEST");
  ASSERT_EQ(write(*engine, "PUT", "/api/days/2026-09-22/note", {{"plan", ""}, {"review", ""}}).status, 200);
  EXPECT_TRUE(read(*engine, "/api/trades")["day_notes"].empty());
  engine->stop();
  std::filesystem::remove_all(journal.parent_path());
}

TEST_F(PaperEngine, OptionalExecutionRulesAreValidatedAndPublished) {
  const auto presets = read(*engine, "/api/plans")["plans"];
  for (const auto& preset : presets) {
    EXPECT_EQ(preset["rules"]["slippage_ticks"], 0);
    EXPECT_EQ(preset["rules"]["margin"], "strategy");
  }
  json rules{{"profit_target", nullptr}, {"max_drawdown", nullptr}, {"drawdown_mode", "intraday"},
             {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0}};
  auto reset = [&](const json& r) {
    return write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", r}, {"reason", "rules test"}});
  };
  ASSERT_EQ(reset(rules).status, 200);
  EXPECT_EQ(read(*engine, "/api/account")["rules"]["margin"], "strategy");
  EXPECT_EQ(read(*engine, "/api/account")["rules"]["slippage_ticks"], 0);
  for (const auto& value : {json(-1), json(11)}) {
    rules["slippage_ticks"] = value;
    expect_error(reset(rules), 400, "INVALID_RULES");
  }
  for (const auto& value : {json(1.5), json("2"), json(true), json(nullptr)}) {
    rules["slippage_ticks"] = value;
    expect_error(reset(rules), 400, "INVALID_REQUEST");
  }
  rules["slippage_ticks"] = 10;
  // B59: an out-of-range cutoff is a rule the API understood, not a malformed request.
  for (const auto& value : {json(86'400), json(-1), json(std::numeric_limits<std::int64_t>::max()), json(std::numeric_limits<std::int64_t>::min())}) {
    rules["expiry_cutoff_seconds"] = value;
    expect_error(reset(rules), 400, "INVALID_RULES");
  }
  rules["expiry_cutoff_seconds"] = 1.5;
  expect_error(reset(rules), 400, "INVALID_REQUEST");
  rules["expiry_cutoff_seconds"] = 86'399;
  ASSERT_EQ(reset(rules).status, 200);
  rules["expiry_cutoff_seconds"] = 0;
  for (const auto& value : {json("other"), json(1), json(nullptr)}) {
    rules["margin"] = value;
    expect_error(reset(rules), 400, "INVALID_RULES");
  }
  rules["margin"] = "portfolio";
  const auto response = reset(rules);
  ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["rules"]["slippage_ticks"], 10);
  EXPECT_EQ(json::parse(response.body)["rules"]["margin"], "portfolio");
  EXPECT_EQ(read(*engine, "/api/account")["rules"]["margin"], "portfolio");
  engine->stop();
}

TEST_F(PaperEngine, MarginSettingsStandBesidePlansAndPortfolioShowsTheBreakdown) {
  engine->stop();
  const auto journal = paper_path();
  auto options = paper_options();
  options.paper_accounts = journal.parent_path() / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  // An evaluation plan keeps its rules, and still counts as the plan's own, on the trader's IRA.
  const auto reset = write(*engine, "POST", "/api/account/reset",
      {{"plan", "eod-50k"}, {"reason", "ira"}, {"account_type", "ira"}, {"house_margin_percent", 25}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto rules = json::parse(reset.body)["rules"];
  EXPECT_EQ(rules["plan"], "End-of-day 50K");
  EXPECT_EQ(rules["account_type"], "ira");
  EXPECT_EQ(rules["house_margin_percent"], 25);
  EXPECT_EQ(rules["pm_vol_shock"], 0);
  EXPECT_EQ(rules["margin"], "strategy");
  const auto portfolio = read(*engine, "/api/portfolio");
  ASSERT_TRUE(portfolio.contains("margin"));
  EXPECT_EQ(portfolio["margin"], json::array());
  // A cash account cannot take portfolio margin; nor can an unknown type be named.
  expect_error(write(*engine, "POST", "/api/account/reset",
      {{"plan", "practice"}, {"reason", "bad"}, {"account_type", "cash"}, {"margin", "portfolio"}}), 400, "INVALID_RULES");
  expect_error(write(*engine, "POST", "/api/account/reset",
      {{"plan", "practice"}, {"reason", "bad"}, {"account_type", "roth"}}), 400, "INVALID_RULES");
  expect_error(write(*engine, "POST", "/api/account/reset",
      {{"plan", "practice"}, {"reason", "bad"}, {"pm_vol_shock", 51}}), 400, "INVALID_RULES");
  for (const auto& [key, values] : std::vector<std::pair<std::string, std::vector<json>>>{
      {"account_type", {"roth", 1, nullptr}}, {"house_margin_percent", {-1, 401, 1.5, true, "25", nullptr}},
      {"pm_vol_shock", {-1, 51, 1.5, true, "5", nullptr}}, {"margin", {"unknown", 1, nullptr}}}) {
    for (const auto& value : values) {
      expect_error(write(*engine, "POST", "/api/account/reset",
          {{"plan", "practice"}, {"reason", "bad margin"}, {key, value}}), 400, "INVALID_RULES");
      expect_error(write(*engine, "POST", "/api/accounts",
          {{"name", "Bad margin"}, {"plan", "practice"}, {key, value}}), 400, "INVALID_RULES");
      auto custom = rules;
      custom["plan"] = "Custom";
      custom[key] = value;
      expect_error(write(*engine, "POST", "/api/account/reset",
          {{"initial_cash", "50000"}, {"rules", custom}, {"reason", "bad margin"}}), 400, "INVALID_RULES");
    }
  }
  const auto created = write(*engine, "POST", "/api/accounts",
      {{"name", "Portfolio"}, {"plan", "practice"}, {"margin", "portfolio"}, {"pm_vol_shock", 5}});
  ASSERT_EQ(created.status, 201) << created.body;
  const auto account = read(*engine, "/api/account?account=portfolio")["rules"];
  EXPECT_EQ(account["margin"], "portfolio");
  EXPECT_EQ(account["pm_vol_shock"], 5);
  EXPECT_EQ(account["account_type"], "margin");
  // The bounds are inclusive, and cash/IRA require buying power even in custom rules.
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "bounds"},
      {"margin", "portfolio"}, {"house_margin_percent", 400}, {"pm_vol_shock", 50}}).status, 200);
  for (const auto* type : {"cash", "ira"}) {
    auto custom = rules;
    custom["plan"] = "Custom";
    custom["account_type"] = type;
    custom["buying_power"] = false;
    expect_error(write(*engine, "POST", "/api/account/reset",
        {{"initial_cash", "50000"}, {"rules", custom}, {"reason", "buying power required"}}), 400, "INVALID_RULES");
  }
  auto malformed = rules;
  malformed["plan"] = "Custom";
  malformed["slippage_ticks"] = 1.5;
  expect_error(write(*engine, "POST", "/api/account/reset",
      {{"initial_cash", "50000"}, {"rules", malformed}, {"reason", "legacy type error"}}), 400, "INVALID_REQUEST");
  engine->stop();
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view("portfolio") != nullptr; }));
  EXPECT_EQ(read(*engine, "/api/account?account=portfolio")["rules"], account);
}

TEST_F(PaperEngine, FillPresetsPreservePlanRulesAndValidateCustomSettings) {
  engine->stop();
  const auto journal = paper_path();
  auto options = paper_options();
  options.paper_accounts = journal.parent_path() / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  for (const auto* preset : {"conservative", "as_displayed"}) {
    const auto response = write(*engine, "POST", "/api/account/reset",
        {{"plan", "intraday-50k"}, {"reason", "fill model"}, {"fill_model", preset}});
    ASSERT_EQ(response.status, 200) << response.body;
    const auto rules = json::parse(response.body)["rules"];
    EXPECT_EQ(rules["buy_only"], true);
    EXPECT_EQ(rules["profit_target"], "5000.00");
    if (std::string_view(preset) == "conservative") {
      EXPECT_EQ(rules["fill_latency_ms"], 1000);
      EXPECT_EQ(rules["impact_ticks"], 1);
      EXPECT_EQ(rules["slippage_ticks"], 1);
    } else {
      EXPECT_FALSE(rules.contains("fill_latency_ms"));
      EXPECT_FALSE(rules.contains("impact_ticks"));
      EXPECT_EQ(rules["slippage_ticks"], 0);
    }
  }
  expect_error(write(*engine, "POST", "/api/account/reset",
      {{"plan", "practice"}, {"reason", "bad"}, {"fill_model", "unknown"}}), 400, "INVALID_REQUEST");
  const auto created = write(*engine, "POST", "/api/accounts",
      {{"name", "Conservative"}, {"plan", "practice"}, {"fill_model", "conservative"}});
  ASSERT_EQ(created.status, 201) << created.body;
  EXPECT_EQ(read(*engine, "/api/account?account=conservative")["rules"]["fill_latency_ms"], 1000);
  EXPECT_EQ(read(*engine, "/api/account?account=conservative")["rules"]["impact_ticks"], 1);
  EXPECT_FALSE(read(*engine, "/api/account")["rules"].contains("impact_ticks"));
  json rules{{"profit_target", nullptr}, {"max_drawdown", nullptr}, {"drawdown_mode", "intraday"},
             {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0}};
  for (const auto* key : {"fill_latency_ms", "impact_ticks"}) {
    for (const auto& value : {json(-1), json(60'001), json(1.5), json("2"), json(true), json(nullptr)}) {
      auto custom = rules;
      custom[key] = value;
      const auto response = write(*engine, "POST", "/api/account/reset",
          {{"initial_cash", "100000"}, {"rules", custom}, {"reason", "invalid fill setting"}});
      expect_error(response, 400,
                   value.is_number_integer() ? "INVALID_RULES" : "INVALID_REQUEST");
    }
  }
  // B36: a new account's custom rules cannot borrow a preset's name either.
  auto named = rules;
  named["plan"] = "Practice";
  expect_error(write(*engine, "POST", "/api/accounts", {{"name", "Borrowed"}, {"initial_cash", "10000"}, {"rules", named}}), 400, "INVALID_RULES");
  const auto same = write(*engine, "POST", "/api/accounts", {{"name", "Same rules"}, {"initial_cash", "100000"}, {"rules", named}});
  EXPECT_EQ(same.status, 201) << same.body;  // the preset's own balance and rules may keep its name
  rules["fill_latency_ms"] = 60'000;
  rules["impact_ticks"] = 10;
  const auto custom = write(*engine, "POST", "/api/account/reset",
      {{"initial_cash", "100000"}, {"rules", rules}, {"reason", "custom fills"}});
  ASSERT_EQ(custom.status, 200) << custom.body;
  EXPECT_EQ(json::parse(custom.body)["rules"]["fill_latency_ms"], 60'000);
  EXPECT_EQ(json::parse(custom.body)["rules"]["impact_ticks"], 10);
  engine->stop();
  std::filesystem::remove_all(journal.parent_path());
}

TEST_F(PaperEngine, ResetRequestsAreStrict) {
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "platinum"}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"initial_cash", "1"}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "  "}}), 422, "INVALID_REASON");
  const json rules{{"plan", "Custom"}, {"profit_target", nullptr}, {"max_drawdown", "10.00"}, {"drawdown_mode", "sideways"},
                   {"buy_only", false}, {"buying_power", false}, {"expiry_cutoff_seconds", 0}};
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", rules}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "-5"}, {"rules", rules}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  const auto bad_query = server::handle_api({"GET", "/api/trades?status=open&status=all"}, *engine);
  EXPECT_EQ(bad_query.status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/trades?attempt=previous"}, *engine).status, 400);
  engine->stop();
}

TEST_F(PaperEngine, CustomDrawdownBreachLiquidatesWithSystemOrders) {
  engine->stop(); engine.reset();
  const auto path = paper_path();
  auto options = paper_options(); options.paper_journal = path;
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  seed();
  const json rules{{"plan", "Tight"}, {"profit_target", nullptr}, {"max_drawdown", "10.00"}, {"drawdown_mode", "intraday"},
                   {"buy_only", false}, {"buying_power", false}, {"expiry_cutoff_seconds", 0}};
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", rules}, {"reason", "tight"}}).status, 200);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "breach", "4.20")).status, 201);
  const auto account = read(*engine, "/api/account");
  EXPECT_EQ(account["evaluation"]["status"], "failed");
  EXPECT_EQ(account["evaluation"]["decided_equity"], "9989.35");
  EXPECT_NE(account["evaluation"]["decision"].get<std::string>().find("drawdown floor $9990.00"), std::string::npos);
  const auto orders = read(*engine, "/api/orders?status=all")["orders"];
  EXPECT_EQ(orders[0]["origin"], "system");
  EXPECT_TRUE(orders[0]["client_order_id"].get<std::string>().starts_with("system:drawdown:"));
  EXPECT_EQ(orders[0]["status"], "filled");
  EXPECT_TRUE(read(*engine, "/api/portfolio")["positions"].empty());
  EXPECT_EQ(read(*engine, "/api/portfolio")["cash"], "9978.70");
  EXPECT_EQ(json::parse(server::tick_message(*engine))["trading"]["evaluation"], "failed");
  expect_error(write(*engine, "POST", "/api/orders", order(market, "after", "4.20")), 422, "EVALUATION_CLOSED");
  const auto samples = read(*engine, "/api/account/equity")["samples"];
  const auto point = std::find_if(samples.begin(), samples.end(), [&](const json& sample) {
    return sample["time"] == account["evaluation"]["decided_at"] && sample["equity"] == account["evaluation"]["decided_equity"] && sample["fill"].is_null();
  });
  ASSERT_NE(point, samples.end());
  const auto liquidation = std::find_if(samples.begin(), samples.end(), [](const json& sample) { return sample["fill"] == "2"; });
  ASSERT_NE(liquidation, samples.end());
  EXPECT_LT(point, liquidation);
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "archive decision"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto archived = read(*engine, "/api/account")["attempts"].back();
  EXPECT_EQ(archived["rules"], account["rules"]);
  for (const auto* key : {"decided_at", "decided_equity", "peak", "floor", "decision_code"})
    EXPECT_EQ(archived[key], account["evaluation"][key]) << key;
  engine->stop(); engine.reset();
  std::filesystem::remove_all(path.parent_path());
}

TEST_F(PaperEngine, FundedPlansUnlockAfterAPassAndPayoutsFollowTheirRules) {
  seed();
  EXPECT_EQ(read(*engine, "/api/account")["payout"], nullptr);
  expect_error(write(*engine, "POST", "/api/account/payout", {{"amount", "10.00"}}), 422, "PAYOUT_UNAVAILABLE");
  expect_error(write(*engine, "POST", "/api/account/payout", {{"amount", "ten"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/payout", {{"amount", "1.001"}}), 422, "INVALID_PAYOUT");
  expect_error(write(*engine, "POST", "/api/account/payout", {{"amount", "10.00"}, {"to", "bank"}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "funded-intraday-25k"}, {"reason", "skip"}}), 422, "PLAN_LOCKED");

  // Custom rules cannot borrow a preset's name (B36): a pass under them is not a pass of it.
  const json rules{{"plan", "Custom"}, {"profit_target", "5.00"}, {"max_drawdown", nullptr}, {"drawdown_mode", "intraday"},
                   {"buy_only", false}, {"buying_power", false}, {"expiry_cutoff_seconds", 0}};
  json named = rules;
  named["plan"] = "Intraday 25K";
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", named}, {"reason", "eval"}}), 400, "INVALID_RULES");
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", rules}, {"reason", "eval"}}).status, 200);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "custom", "4.20")).status, 201);
  quote("4.40", "4.60");
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/account")["evaluation"]["status"] == "passed"; }));
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "funded-intraday-25k"}, {"reason", "custom"}}), 422, "PLAN_LOCKED");

  // Pass the preset itself, then the funded plan unlocks.
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"plan", "intraday-25k"}, {"reason", "eval"}}).status, 200);
  quote("4.00", "4.20");
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open", "4.20")).status, 201);
  quote("29.15", "29.35");
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/account")["evaluation"]["status"] == "passed"; }));
  // B37: the pass at 27,504.35 liquidated at the bid, below the target; nothing remains to go.
  const auto passing_account = read(*engine, "/api/account");
  const auto passed = passing_account["evaluation"];
  EXPECT_EQ(passed["decided_equity"], "27504.35");
  EXPECT_EQ(passed["equity"], "27493.70");
  EXPECT_EQ(passed["target_remaining"], "0.00");
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "funded-intraday-25k"}, {"reason", "funded"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto account = json::parse(reset.body);
  EXPECT_EQ(account["rules"]["phase"], "funded");
  EXPECT_EQ(account["rules"]["lock_balance"], "25000.00");
  EXPECT_EQ(account["evaluation"]["target_equity"], nullptr);
  EXPECT_EQ(account["evaluation"]["floor"], "23750.00");
  EXPECT_EQ(account["evaluation"]["floor_locked"], false);
  EXPECT_EQ(account["evaluation"]["qualifying_days"], 0);
  EXPECT_TRUE(account["evaluation"]["payouts"].empty());
  EXPECT_EQ(account["attempts"][2]["status"], "passed");
  EXPECT_EQ(account["attempts"][2]["plan"], "Intraday 25K");
  EXPECT_EQ(account["attempts"][2]["rules"], passing_account["rules"]);
  for (const auto* key : {"decided_at", "decided_equity", "peak", "floor"})
    EXPECT_EQ(account["attempts"][2][key], passed[key]) << key;
  const auto payout = account["payout"];
  EXPECT_EQ(payout["eligible"], false);
  EXPECT_EQ(payout["blocked"]["code"], "PAYOUT_NOT_ELIGIBLE");
  EXPECT_EQ(payout["blocked"]["actual"], 0);
  EXPECT_EQ(payout["blocked"]["limit"], 8);
  EXPECT_EQ(payout["number"], 1);
  EXPECT_EQ(payout["flat"], true);
  EXPECT_EQ(payout["required_days"], 8);
  EXPECT_EQ(payout["qualifying_profit"], "100.00");
  EXPECT_EQ(payout["profit"], "0.00");
  EXPECT_EQ(payout["maximum"], "0.00");
  EXPECT_EQ(payout["minimum"], "250.00");
  EXPECT_EQ(payout["cap"], "500.00");
  EXPECT_EQ(payout["split_percent"], 80);
  EXPECT_EQ(payout["buffer_balance"], nullptr);
  EXPECT_EQ(payout["consistency_percent"], nullptr);
  EXPECT_EQ(payout["cycle_profit"], "0.00");
  EXPECT_EQ(payout["best_day"], nullptr);
  EXPECT_EQ(payout["consistency_needed"], "0.00");
  const auto refused = write(*engine, "POST", "/api/account/payout", {{"amount", "250.00"}});
  expect_error(refused, 422, "PAYOUT_NOT_ELIGIBLE");
  EXPECT_EQ(json::parse(refused.body)["error"]["limit"], 8);
  // Once entered, the same step can restart under its reset terms.
  EXPECT_EQ(write(*engine, "POST", "/api/account/reset", {{"plan", "funded-intraday-25k"}, {"reason", "again"}}).status, 200);

  json custom = rules;
  custom["phase"] = "funded";
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", custom}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  custom["payouts"] = {{"qualifying_profit", "50"}, {"qualifying_days", 0}, {"withdrawal_percent", 50}, {"split_percent", 80},
                       {"minimum", "10"}, {"caps", json::array()}};
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", custom}, {"reason", "x"}}), 400, "INVALID_REQUEST");
  custom["payouts"]["qualifying_days"] = 3;
  custom["lock_balance"] = "10000";
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", custom}, {"reason", "x"}}), 400, "INVALID_RULES");
  custom["profit_target"] = nullptr;
  custom["payouts"]["consistency_percents"] = {20, 25, 30};
  custom["payouts"]["buffer"] = "2100";
  custom["payouts"]["buffer_payouts"] = 3;
  const auto custom_reset = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", custom}, {"reason", "x"}});
  ASSERT_EQ(custom_reset.status, 200) << custom_reset.body;
  EXPECT_EQ(json::parse(custom_reset.body)["rules"]["payouts"]["qualifying_days"], 3);
  EXPECT_EQ(json::parse(custom_reset.body)["payout"]["cap"], nullptr);
  EXPECT_EQ(json::parse(custom_reset.body)["payout"]["consistency_percent"], 20);
  EXPECT_EQ(json::parse(custom_reset.body)["payout"]["buffer_balance"], "12100.00");
  EXPECT_EQ(json::parse(custom_reset.body)["rules"]["payouts"]["buffer"], "2100.00");
  EXPECT_EQ(json::parse(custom_reset.body)["rules"]["payouts"]["buffer_payouts"], 3);
  for (const auto* key : {"caps", "consistency_percents"}) {
    auto malformed = custom;
    malformed["payouts"][key] = key == std::string_view("caps")
        ? json(std::vector<std::string>(65, "100")) : json(std::vector<int>(65, 25));
    expect_error(write(*engine, "POST", "/api/account/reset",
        {{"initial_cash", "10000"}, {"rules", malformed}, {"reason", "legacy array limit"}}), 400, "INVALID_REQUEST");
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{{"buffer", "-1"}, {"buffer_payouts", -1}, {"buffer_payouts", 101}}) {
    auto bad = custom; bad["payouts"][key] = value;
    expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", bad}, {"reason", "invalid"}}), 400, "INVALID_RULES");
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{{"buffer", 100}, {"buffer", nullptr}, {"buffer_payouts", 1.5}, {"buffer_payouts", "1"}}) {
    auto bad = custom; bad["payouts"][key] = value;
    expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", bad}, {"reason", "invalid"}}), 400, "INVALID_REQUEST");
  }
  EXPECT_EQ(json::parse(custom_reset.body)["rules"]["payouts"]["consistency_percents"], json::array({20, 25, 30}));
  for (const auto& invalid : {json::array({0}), json::array({101}), json::array({-1})}) {
    custom["payouts"]["consistency_percents"] = invalid;
    expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", custom}, {"reason", "invalid"}}), 400, "INVALID_RULES");
  }
  for (const auto& invalid : {json::array({40.5}), json::array({"40"}), json(nullptr), json(40), json::array({18446744073709551615ULL})}) {
    custom["payouts"]["consistency_percents"] = invalid;
    expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", custom}, {"reason", "invalid"}}), 400, "INVALID_REQUEST");
  }
  engine->stop();
}

TEST_F(PaperEngine, PayoutQuoteReportsTheBestRealisedDayAndBuffer) {
  seed();
  const json payouts{{"qualifying_profit", "10"}, {"qualifying_days", 1}, {"withdrawal_percent", 100}, {"split_percent", 80},
      {"minimum", "10"}, {"caps", json::array()}, {"consistency_percents", {40}}, {"buffer", "100"}, {"buffer_payouts", 1}};
  const json rules{{"plan", "Payout standing"}, {"phase", "funded"}, {"profit_target", nullptr}, {"max_drawdown", nullptr},
      {"drawdown_mode", "intraday"}, {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0}, {"payouts", payouts}};
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "10000"}, {"rules", rules}, {"reason", "standing"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "best-open", "4.20")).status, 201);
  market.next();
  quote("5.40", "5.60");
  auto close = order(market, "best-close", "5.40"); close["side"] = "sell";
  ASSERT_EQ(write(*engine, "POST", "/api/orders", close).status, 201);
  market.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  quote();
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/account")["payout"]["qualifying_days"] == 1; }));
  const auto q = read(*engine, "/api/account")["payout"];
  EXPECT_EQ(q["consistency_percent"], 40);
  EXPECT_EQ(q["cycle_profit"], "118.70");
  EXPECT_EQ(q["best_day"], (json{{"day", "2026-09-22"}, {"profit", "118.70"}}));
  EXPECT_EQ(q["consistency_needed"], "178.05");
  EXPECT_EQ(q["buffer_balance"], "10100.00");
  EXPECT_EQ(q["maximum"], "18.70");
  EXPECT_EQ(q["blocked"]["code"], "PAYOUT_NOT_ELIGIBLE");
  EXPECT_EQ(q["blocked"]["actual"], 100);
  EXPECT_EQ(q["blocked"]["limit"], 40);
  const auto refused = write(*engine, "POST", "/api/account/payout", {{"amount", "10.00"}});
  expect_error(refused, 422, "PAYOUT_NOT_ELIGIBLE");
  EXPECT_EQ(json::parse(refused.body)["error"]["message"], q["blocked"]["message"]);
  engine->stop();
}

TEST_F(PaperEngine, IdleQuoteBatchesAreNotRecordedButRolloverIs) {
  const auto processed = [&] {
    ASSERT_TRUE(wait_for([&] {
      const auto view = engine->trading_view();
      const auto time = view->market_times.find("SPX");
      return time != view->market_times.end() && time->second >= market.time;
    }));
  };
  const auto version = [&] { return read(*engine, "/api/status")["trading"]["account_version"].get<std::string>(); };
  seed();
  processed();
  const auto idle = version();
  for (int i = 0; i < 5; ++i) {
    market.time += md::kNanosPerSecond;
    quote();
    processed();
  }
  EXPECT_EQ(version(), idle);
  // A new trading day is still rolled over, once.
  market.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  quote();
  processed();
  EXPECT_EQ(engine->trading_view()->snapshot->evaluation.day, (md::Date{2026, 9, 23}));
  const auto rolled = version();
  EXPECT_NE(rolled, idle);
  market.time += md::kNanosPerSecond;
  quote();
  processed();
  EXPECT_EQ(version(), rolled);
  // A working order makes every batch count again.
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "rest", "4.00")).status, 201);
  const auto working = version();
  market.time += md::kNanosPerSecond;
  quote();
  processed();
  EXPECT_NE(version(), working);
  engine->stop();
}

TEST_F(PaperEngine, WholeTradesGroupOverHttpAndListWithTheHeldStrategies) {
  seed();
  const auto upper = *md::parse_osi("SPXW261022C05010000");
  provider.sink->publish(md::ContractDefinition{1, upper});
  provider.sink->publish(md::OptionQuote{1, market.time, 3.00, 3.20, 10, 10});
  ASSERT_TRUE(wait_for([&] {
    const auto metrics = engine->metrics("SPX");
    if (!metrics || metrics->slices.empty()) return false;
    const auto& strikes = metrics->slices[0].strikes;
    return std::any_of(strikes.begin(), strikes.end(), [](const auto& s) { return s.strike == 5010 && s.call.ask == 3.20; });
  }));
  // Legged in: a long 5000 call, then a short 5010 call joining its trade by name.
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "long", "4.20")).status, 201);
  auto hedge = order(market, "hedge", "3.00");
  hedge["symbol"] = upper.osi_symbol();
  hedge["side"] = "sell";
  hedge["group"] = "9";
  expect_error(write(*engine, "POST", "/api/orders", hedge), 422, "INVALID_GROUP");
  hedge["client_order_id"] = "hedge 2";
  hedge["group"] = "x";
  expect_error(write(*engine, "POST", "/api/orders", hedge), 400, "INVALID_REQUEST");
  hedge["client_order_id"] = "hedge 3";
  hedge["group"] = "1";
  const auto placed = write(*engine, "POST", "/api/orders", hedge);
  ASSERT_EQ(placed.status, 201) << placed.body;
  EXPECT_EQ(json::parse(placed.body)["order"]["group"], "1");
  auto trades = read(*engine, "/api/trades");
  ASSERT_EQ(trades["trades"].size(), 2);
  for (const auto& t : trades["trades"]) EXPECT_EQ(t["group"], "1");
  ASSERT_EQ(trades["groups"].size(), 1);
  const auto whole = trades["groups"][0];
  EXPECT_EQ(whole["id"], "1");
  EXPECT_EQ(whole["round_trips"], json::array({"1", "2"}));
  EXPECT_EQ(whole["entries"], 2);
  EXPECT_EQ(whole["status"], "open");
  EXPECT_TRUE(whole["review"].is_object());
  const auto portfolio = read(*engine, "/api/portfolio");
  ASSERT_EQ(portfolio["strategies"].size(), 1);
  EXPECT_EQ(portfolio["strategies"][0]["id"], "1");
  EXPECT_EQ(portfolio["strategies"][0]["legs"].size(), 2);
  // The short leaves the trade, and joins it again.
  const auto out = write(*engine, "POST", "/api/trades/ungroup", {{"trades", {"2"}}});
  ASSERT_EQ(out.status, 200) << out.body;
  EXPECT_EQ(json::parse(out.body)["groups"], (json{{"2", "2"}}));
  EXPECT_TRUE(read(*engine, "/api/trades")["groups"].empty());
  EXPECT_EQ(read(*engine, "/api/portfolio")["strategies"].size(), 2);
  const auto in = write(*engine, "POST", "/api/trades/group", {{"trades", {"1", "2"}}});
  ASSERT_EQ(in.status, 200) << in.body;
  EXPECT_EQ(json::parse(in.body)["groups"], (json{{"1", "1"}, {"2", "1"}}));
  EXPECT_FALSE(read(*engine, "/api/trades")["groups"][0]["review_since"].is_null());
  expect_error(write(*engine, "POST", "/api/trades/group", {{"trades", {"1", "7"}}}), 404, "UNKNOWN_TRADE");
  expect_error(write(*engine, "POST", "/api/trades/group", {{"trades", {"1"}}}), 422, "INVALID_GROUP");
  expect_error(write(*engine, "POST", "/api/trades/group", {{"trades", json::array()}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/trades/group", {{"trades", {1}}}), 400, "INVALID_REQUEST");
  engine->stop();
}

TEST_F(PaperEngine, MultiLegOrdersOverHttp) {
  seed();
  // A second strike, the 5010 call, beside the fixture's 5000 call.
  const auto upper = *md::parse_osi("SPXW261022C05010000");
  provider.sink->publish(md::ContractDefinition{1, upper});
  provider.sink->publish(md::OptionQuote{1, market.time, 3.00, 3.20, 10, 10});
  ASSERT_TRUE(wait_for([&] {
    const auto metrics = engine->metrics("SPX");
    if (!metrics || metrics->slices.empty()) return false;
    const auto& strikes = metrics->slices[0].strikes;
    return std::any_of(strikes.begin(), strikes.end(), [](const auto& s) { return s.strike == 5010 && s.call.ask == 3.20; });
  }));
  const json legs = json::array({{{"symbol", market.symbol()}, {"side", "buy"}},
                                 {{"symbol", upper.osi_symbol()}, {"side", "sell"}, {"ratio", 1}}});
  // Buy the 5000 call at 4.20 and sell the 5010 call at 3.00: a 1.20 debit.
  const json spread{{"client_order_id", "vertical"}, {"legs", legs}, {"type", "limit"}, {"quantity", 2},
                    {"limit_price", "1.20"}, {"time_in_force", "day"}};
  const auto response = write(*engine, "POST", "/api/orders", spread);
  ASSERT_EQ(response.status, 201) << response.body;
  const auto body = json::parse(response.body);
  const auto order = body["order"];
  EXPECT_EQ(order["symbol"], nullptr);
  EXPECT_EQ(order["side"], nullptr);
  EXPECT_EQ(order["underlying"], "SPX");
  EXPECT_EQ(order["legs"], json::array({{{"symbol", market.symbol()}, {"side", "buy"}, {"ratio", 1}},
                                        {{"symbol", upper.osi_symbol()}, {"side", "sell"}, {"ratio", 1}}}));
  EXPECT_EQ(order["status"], "filled");
  EXPECT_EQ(order["limit_price"], "1.20");
  EXPECT_EQ(order["average_fill_price"], "1.20");
  ASSERT_EQ(body["fills"].size(), 2);
  EXPECT_EQ(body["fills"][0]["price"], "4.20");
  EXPECT_EQ(body["fills"][1]["side"], "sell");
  EXPECT_EQ(body["fills"][1]["price"], "3.00");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["legs"].size(), 2);
  EXPECT_EQ(read(*engine, "/api/portfolio")["positions"].size(), 2);

  // Preview the held vertical's close without changing the positions or allocating IDs.
  const auto dry = write(*engine, "POST", "/api/positions/close/preview", json::object());
  ASSERT_EQ(dry.status, 200) << dry.body;
  const auto preview = json::parse(dry.body);
  ASSERT_EQ(preview["orders"].size(), 1);
  const auto& close = preview["orders"][0];
  EXPECT_FALSE(close.contains("id"));
  EXPECT_EQ(close["symbol"], nullptr);
  EXPECT_EQ(close["side"], nullptr);
  EXPECT_EQ(close["underlying"], "SPX");
  EXPECT_EQ(close["quantity"], 2);
  EXPECT_EQ(close["legs"], json::array({{{"symbol", upper.osi_symbol()}, {"side", "buy"}, {"ratio", 1}},
                                        {{"symbol", market.symbol()}, {"side", "sell"}, {"ratio", 1}}}));
  EXPECT_EQ(read(*engine, "/api/portfolio")["positions"].size(), 2);

  // A credit is a negative net limit; strict shapes otherwise.
  auto credit = spread;
  credit["client_order_id"] = "credit";
  credit["legs"][0]["side"] = "sell";
  credit["legs"][1]["side"] = "buy";
  credit["limit_price"] = "-1.00";
  const auto resting = write(*engine, "POST", "/api/orders", credit);
  ASSERT_EQ(resting.status, 201) << resting.body;
  EXPECT_EQ(json::parse(resting.body)["order"]["status"], "working");
  EXPECT_EQ(json::parse(resting.body)["order"]["limit_price"], "-1.00");
  auto bad = spread;
  bad["client_order_id"] = "bad";
  bad["symbol"] = market.symbol();
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad.erase("symbol");
  bad["legs"] = json::array({legs[0]});
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["legs"] = legs;
  bad["legs"][1]["side"] = "hold";
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["legs"] = legs;
  bad["legs"][1]["ratio"] = 1.5;
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  // A shape no market could make valid is malformed, whichever field breaks it:
  // 400, nothing recorded, and the client ID stays free.
  const auto recorded = read(*engine, "/api/orders")["orders"].size();
  bad["legs"] = legs;
  bad["legs"][1]["ratio"] = 11;
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["legs"] = legs;
  bad["legs"][1]["symbol"] = legs[0]["symbol"];
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["legs"] = json::array({legs[0], legs[1], legs[0], legs[1], legs[0]});
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["legs"] = legs;
  bad.erase("limit_price");
  bad["type"] = "market";
  bad["time_in_force"] = "day";
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["time_in_force"] = "ioc";
  bad["quantity"] = 0;
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  EXPECT_EQ(read(*engine, "/api/orders")["orders"].size(), recorded);
  engine->stop();
}

TEST_F(PaperEngine, BracketAndConditionalOrdersOverHttp) {
  seed();
  const json stop{{"source", "option"}, {"direction", "at_or_below"}, {"level", "3.50"}};
  auto entry = order(market, "entry", "4.20");
  entry["bracket"] = {{"stop_loss", {{"trigger", stop}}}, {"take_profit", {{"limit_price", "5.00"}}}};
  const auto response = write(*engine, "POST", "/api/orders", entry);
  ASSERT_EQ(response.status, 201) << response.body;
  const auto body = json::parse(response.body)["order"];
  EXPECT_EQ(body["status"], "filled");
  EXPECT_EQ(body["stop_loss_order"], "2");
  EXPECT_EQ(body["take_profit_order"], "3");
  EXPECT_EQ(body["bracket"]["take_profit"]["limit_price"], "5.00");
  EXPECT_EQ(body["role"], nullptr);
  const auto open = read(*engine, "/api/orders?status=open")["orders"];
  ASSERT_EQ(open.size(), 2);
  EXPECT_EQ(open[0]["role"], "take_profit");
  EXPECT_EQ(open[0]["status"], "working");
  EXPECT_EQ(open[0]["oco"], "2");
  EXPECT_EQ(open[1]["role"], "stop_loss");
  EXPECT_EQ(open[1]["status"], "armed");
  EXPECT_EQ(open[1]["parent"], "1");
  // Orders report the trigger's reference and trail too: the executable side, no trail.
  auto reported = stop;
  reported["reference"] = "bid_ask";
  reported["trail"] = nullptr;
  EXPECT_EQ(open[1]["trigger"], reported);
  EXPECT_EQ(open[1]["day_end"], md::format_timestamp(market.contract.expiry_time()));

  auto bad = order(market, "empty-bracket", "4.20");
  bad["bracket"] = json::object();
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  bad["bracket"] = {{"stop_loss", json::object()}};
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  // A stop with a limit price is a stop-limit: armed, then a GTC limit once reached.
  auto stop_limit = order(market, "stop-limit", "4.20");
  stop_limit["bracket"] = {{"stop_loss", {{"trigger", stop}, {"limit_price", "3.40"}}}};
  const auto limited = write(*engine, "POST", "/api/orders", stop_limit);
  ASSERT_EQ(limited.status, 201) << limited.body;
  const auto exit_id = json::parse(limited.body)["order"]["stop_loss_order"];
  const auto open_orders = read(*engine, "/api/orders?status=open");
  for (const auto& item : open_orders["orders"]) {
    if (item["id"] != exit_id) continue;
    EXPECT_EQ(item["type"], "limit");
    EXPECT_EQ(item["time_in_force"], "gtc");
    EXPECT_EQ(item["status"], "armed");
    EXPECT_EQ(item["limit_price"], "3.40");
    EXPECT_EQ(item["trigger"], reported);
  }
  bad["bracket"] = {{"stop_loss", {{"trigger", {{"source", "spot"}, {"direction", "at_or_below"}, {"level", "1"}}}}}};
  expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");

  auto armed = order(market, "armed", "4.20");
  armed["trigger"] = {{"source", "underlying"}, {"direction", "at_or_above"}, {"level", "5100"}};
  const auto conditional = write(*engine, "POST", "/api/orders", armed);
  ASSERT_EQ(conditional.status, 201) << conditional.body;
  EXPECT_EQ(json::parse(conditional.body)["order"]["status"], "armed");
  EXPECT_EQ(json::parse(conditional.body)["order"]["triggered_at"], nullptr);
  engine->stop();
}

TEST_F(PaperEngine, OrdersConditionalOnAnotherUnderlyingAStudyOrTheTimeOverHttp) {
  // F43: the desk supplies the watched underlying's price from its analytics.
  seed();
  auto watched = order(market, "watched", "4.20");
  watched["trigger"] = {{"source", "underlying"}, {"symbol", "SPX"}, {"direction", "at_or_above"}, {"level", "5000"}};
  const auto placed = write(*engine, "POST", "/api/orders", watched);
  ASSERT_EQ(placed.status, 201) << placed.body;
  EXPECT_EQ(json::parse(placed.body)["order"]["trigger"]["symbol"], "SPX");
  quote();
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["status"], "filled");

  auto timed = order(market, "timed", "4.10");
  timed["time_in_force"] = "gtc";
  timed["trigger"] = {{"source", "time"}, {"at", "15:30"}};
  const auto armed = write(*engine, "POST", "/api/orders", timed);
  ASSERT_EQ(armed.status, 201) << armed.body;
  const auto body = json::parse(armed.body)["order"];
  EXPECT_EQ(body["status"], "armed");
  EXPECT_EQ(body["trigger"]["source"], "time");
  EXPECT_EQ(body["trigger"]["at"], "15:30");
  EXPECT_EQ(body["trigger"]["direction"], "at_or_above");
  EXPECT_FALSE(body["trigger"].contains("symbol"));

  auto study = order(market, "study", "4.10");
  study["trigger"] = {{"source", "study"}, {"study", "iv30"}, {"direction", "at_or_above"}, {"level", "99"}};
  const auto waiting = write(*engine, "POST", "/api/orders", study);
  ASSERT_EQ(waiting.status, 201) << waiting.body;
  EXPECT_EQ(json::parse(waiting.body)["order"]["trigger"]["study"], "iv30");

  for (const auto& trigger : {json{{"source", "study"}, {"study", "iv_rank"}, {"direction", "at_or_above"}, {"level", "1"}},
                              json{{"source", "study"}, {"direction", "at_or_above"}, {"level", "1"}},
                              json{{"source", "time"}, {"at", "24:00"}},
                              json{{"source", "time"}, {"at", "15:30"}, {"level", "1"}},
                              json{{"source", "underlying"}, {"symbol", "vix"}, {"direction", "at_or_above"}, {"level", "20"}},
                              json{{"source", "option"}, {"symbol", "VIX"}, {"direction", "at_or_above"}, {"level", "20"}}}) {
    auto bad = order(market, "bad-condition", "4.10");
    bad["trigger"] = trigger;
    expect_error(write(*engine, "POST", "/api/orders", bad), 400, "INVALID_REQUEST");
  }
  engine->stop();
}

TEST_F(PaperEngine, GtcMetadataAndHeldComboExitsOverHttp) {
  seed();
  auto tagged = order(market, "tagged", "4.10");
  tagged["time_in_force"] = "gtc";
  tagged["tags"] = {" Template ", "0DTE"};
  tagged["note"] = " Entry note ";
  const auto response = write(*engine, "POST", "/api/orders", tagged);
  ASSERT_EQ(response.status, 201) << response.body;
  EXPECT_EQ(json::parse(response.body)["order"]["time_in_force"], "gtc");
  ASSERT_EQ(write(*engine, "PUT", "/api/orders/1", {{"limit_price", "4.20"}}).status, 200);
  const auto trades = read(*engine, "/api/trades")["trades"];
  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0]["tags"], json::array({"template", "0dte"}));
  EXPECT_EQ(trades[0]["note"], "Entry note");
  auto invalid = tagged;
  invalid["client_order_id"] = "bad-tags";
  invalid["tags"] = {std::string(33, 'x')};
  expect_error(write(*engine, "POST", "/api/orders", invalid), 422, "INVALID_NOTE");
  invalid["tags"] = {1};
  expect_error(write(*engine, "POST", "/api/orders", invalid), 400, "INVALID_REQUEST");
  const auto upper = *md::parse_osi("SPXW261022C05010000");
  provider.sink->publish(md::ContractDefinition{1, upper});
  provider.sink->publish(md::OptionQuote{1, market.time, 3.00, 3.20, 10, 10});
  ASSERT_TRUE(wait_for([&] {
    const auto metrics = engine->metrics("SPX");
    if (!metrics || metrics->slices.empty()) return false;
    const auto& strikes = metrics->slices[0].strikes;
    return std::any_of(strikes.begin(), strikes.end(), [](const auto& strike) { return strike.strike == 5010 && strike.call.ask == 3.20; });
  }));
  const json legs = json::array({{{"symbol", market.symbol()}, {"side", "buy"}},
                                 {{"symbol", upper.osi_symbol()}, {"side", "sell"}}});
  json entry{{"client_order_id", "spread"}, {"legs", legs}, {"type", "limit"}, {"quantity", 1},
             {"limit_price", "1.20"}, {"time_in_force", "gtc"}, {"tags", {"call-debit"}}, {"note", "Spread plan"}};
  ASSERT_EQ(write(*engine, "POST", "/api/orders", entry).status, 201);
  entry["client_order_id"] = "held-exits";
  entry["legs"][0]["side"] = "sell";
  entry["legs"][1]["side"] = "buy";
  entry["limit_price"] = "-1.80";
  entry["exits_only"] = true;
  entry["bracket"] = {{"take_profit", {{"limit_price", "-1.80"}}},
                        {"stop_loss", {{"trigger", {{"source", "combo"}, {"direction", "at_or_above"}, {"level", "-0.40"}}}}}};
  const auto exits = write(*engine, "POST", "/api/orders", entry);
  ASSERT_EQ(exits.status, 201) << exits.body;
  const auto primary = json::parse(exits.body)["order"];
  EXPECT_EQ(primary["role"], "take_profit");
  EXPECT_FALSE(primary["oco"].is_null());
  EXPECT_EQ(primary["filled_quantity"], 0);
  EXPECT_EQ(write(*engine, "POST", "/api/orders", entry).status, 200);
  const auto open = read(*engine, "/api/orders?status=open")["orders"];
  ASSERT_EQ(open.size(), 2U);
  EXPECT_EQ(open[0]["trigger"]["source"], "combo");
  engine->stop();
}

TEST_F(PaperEngine, TrailingTriggersAndTheirReferenceOverHttp) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "long", "4.20")).status, 201);
  json stop{{"source", "option"}, {"direction", "at_or_below"}, {"level", "1.00"}, {"reference", "mid"},
            {"trail", {{"unit", "amount"}, {"value", "0.50"}}}};
  json exits{{"client_order_id", "trail"}, {"symbol", market.symbol()}, {"side", "sell"}, {"type", "market"}, {"quantity", 1},
             {"time_in_force", "ioc"}, {"trigger", stop}, {"exits_only", true}, {"bracket", {{"stop_loss", {{"trigger", stop}}}}}};
  const auto placed = write(*engine, "POST", "/api/orders", exits);
  ASSERT_EQ(placed.status, 201) << placed.body;
  const auto trigger = json::parse(placed.body)["order"]["trigger"];
  EXPECT_EQ(trigger["level"], "3.60");  // the mid 4.10 less 0.50
  EXPECT_EQ(trigger["reference"], "mid");
  EXPECT_EQ(trigger["trail"], json({{"unit", "amount"}, {"value", "0.50"}}));
  EXPECT_EQ(write(*engine, "POST", "/api/orders", exits).status, 200);  // a retry still finds it
  // Plain triggers report the executable side and no trail.
  auto plain = order(market, "plain", "4.00");
  plain["trigger"] = {{"source", "option"}, {"direction", "at_or_below"}, {"level", "3.80"}};
  const auto plain_placed = write(*engine, "POST", "/api/orders", plain);
  ASSERT_EQ(plain_placed.status, 201) << plain_placed.body;
  const auto armed = json::parse(plain_placed.body)["order"]["trigger"];
  EXPECT_EQ(armed["reference"], "bid_ask");
  EXPECT_TRUE(armed["trail"].is_null());
  // Malformed trails are 400 and record nothing.
  const auto refused = [&](json t) {
    auto body = order(market, "bad", "4.00");
    body["trigger"] = std::move(t);
    expect_error(write(*engine, "POST", "/api/orders", body), 400, "INVALID_REQUEST");
  };
  const auto with = [](json base, const json& patch) { base.update(patch); return base; };
  const json spot{{"source", "underlying"}, {"direction", "at_or_below"}, {"level", "4990.00"}};
  refused(with(spot, {{"reference", "mid"}}));
  refused(with(spot, {{"trail", {{"unit", "ticks"}, {"value", 2}}}}));
  const json trail{{"unit", "amount"}, {"value", "0.50"}};
  refused(with(spot, {{"symbol", "VIX"}, {"level", "20.00"}, {"trail", trail}}));
  refused(with(spot, {{"symbol", "VIX"}, {"level", "20.00"}, {"reference", "bid_ask"}}));
  refused({{"source", "study"}, {"study", "iv30"}, {"direction", "at_or_above"}, {"level", "20.00"}, {"trail", trail}});
  refused({{"source", "time"}, {"at", "15:30"}, {"trail", trail}});
  refused(with(stop, {{"trail", {{"unit", "percent"}, {"value", "100.00"}}}}));
  refused(with(stop, {{"trail", {{"unit", "ticks"}, {"value", 0}}}}));
  refused(with(stop, {{"trail", {{"unit", "feet"}, {"value", "1.00"}}}}));
  refused(with(stop, {{"reference", "last"}}));
  engine->stop();
}

TEST_F(PaperEngine, OrderChainsOverHttp) {
  seed();
  // A dip buy or a breakout buy, whichever fills first; the dip then places its target.
  auto dip = order(market, "dip", "3.90");
  auto target = order(market, "", "4.30");
  target.erase("client_order_id");
  target["side"] = "sell";
  target["time_in_force"] = "gtc";
  dip["then"] = target;
  json breakout{{"symbol", market.symbol()}, {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"},
                {"trigger", {{"source", "underlying"}, {"direction", "at_or_above"}, {"level", "5010.00"}}}};
  dip["oco"] = breakout;
  const auto placed = write(*engine, "POST", "/api/orders", dip);
  ASSERT_EQ(placed.status, 201) << placed.body;
  const auto first = json::parse(placed.body)["order"];
  EXPECT_EQ(first["then"]["side"], "sell");
  EXPECT_EQ(first["then"]["limit_price"], "4.30");
  EXPECT_TRUE(first["then"].at("good_till").is_null());
  EXPECT_TRUE(first["then"].at("walk").is_null());
  EXPECT_TRUE(first["then"].at("group").is_null());
  EXPECT_TRUE(first["then"]["then"].is_null());
  EXPECT_TRUE(first["chained_order"].is_null());
  ASSERT_TRUE(first["oco"].is_string());
  const auto open = read(*engine, "/api/orders?status=open")["orders"];
  ASSERT_EQ(open.size(), 2);
  EXPECT_EQ(open[0]["client_order_id"], "dip:oco");
  EXPECT_EQ(open[0]["status"], "armed");
  EXPECT_EQ(write(*engine, "POST", "/api/orders", dip).status, 200);  // a retry answers once
  quote("3.80", "3.90");
  const auto all = read(*engine, "/api/orders?status=all")["orders"];
  ASSERT_EQ(all.size(), 3);
  EXPECT_EQ(all[0]["client_order_id"], "dip:then");
  EXPECT_EQ(all[0]["chained_from"], first["id"]);
  EXPECT_EQ(all[0]["status"], "working");
  EXPECT_EQ(all[1]["reason"]["code"], "OCO_FILLED");
  EXPECT_EQ(all[2]["chained_order"], all[0]["id"]);
  // Chained orders take their ID from the order they hang on, are not held exits, and chain four orders at most.
  const auto refused = [&](json body) { expect_error(write(*engine, "POST", "/api/orders", body), 400, "INVALID_REQUEST"); };
  auto named = order(market, "named", "3.90");
  named["then"] = order(market, "inner", "3.80");
  refused(named);
  auto held = order(market, "held", "3.90");
  held["then"] = target;
  held["then"]["exits_only"] = true;
  refused(held);
  auto nested = order(market, "nested", "3.90");
  nested["oco"] = target;
  nested["oco"]["oco"] = target;
  refused(nested);
  auto deep = order(market, "deep", "3.90");
  json* tail = &deep;
  for (int i = 0; i < 4; ++i) { (*tail)["then"] = target; tail = &(*tail)["then"]; }
  refused(deep);
  engine->stop();
}

TEST_F(PaperEngine, PendingChainsReportDeadlinesWalksAndGroups) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "entry", "4.20")).status, 201);
  auto dip = order(market, "dip", "3.90");
  auto next = order(market, "", "4.00");
  next.erase("client_order_id");
  next["time_in_force"] = "gtd";
  next["good_till"] = md::format_timestamp(market.time + md::kNanosPerDay);
  next["walk"] = {{"step", "0.05"}, {"seconds", 30}, {"limit", "4.20"}};
  next["group"] = "1";
  auto other = order(market, "", "3.80");
  other.erase("client_order_id");
  next["oco"] = other;
  dip["then"] = next;
  const auto placed = write(*engine, "POST", "/api/orders", dip);
  ASSERT_EQ(placed.status, 201) << placed.body;
  const auto first = json::parse(placed.body)["order"];
  const auto& pending = first.at("then");
  for (const auto* key : {"time_in_force", "good_till", "walk", "group"}) EXPECT_EQ(pending.at(key), next.at(key));
  for (const auto* key : {"good_till", "walk", "group"}) EXPECT_TRUE(pending.at("oco").at(key).is_null());
  EXPECT_EQ(read(*engine, "/api/orders/" + first["id"].get<std::string>())["order"]["then"], pending);
}

TEST_F(PaperEngine, OrderRequestBodiesRoundTripBracketAndTimeTriggers) {
  seed();
  for (const bool timed : {false, true}) {
    auto request = market.limit(timed ? "timed" : "bracket", 1, "3.90");
    if (timed) {
      request.trigger = trading::Trigger{.source = trading::TriggerSource::Time,
          .direction = trading::TriggerDirection::AtOrAbove, .level = {}, .minute = 15 * 60 + 30};
    } else {
      const trading::Trigger stop{.source = trading::TriggerSource::Option,
          .direction = trading::TriggerDirection::AtOrBelow, .level = Money::parse("3.50")};
      request.bracket = trading::Bracket{trading::ExitSpec{stop, {}}, trading::ExitSpec{{}, Money::parse("5.00")}};
    }
    const auto body = server::order_request_json(request);
    const auto& trigger = timed ? body.at("trigger") : body.at("bracket").at("stop_loss").at("trigger");
    EXPECT_FALSE(trigger.contains("reference"));
    EXPECT_FALSE(trigger.contains("trail"));
    if (timed) {
      EXPECT_FALSE(trigger.contains("level"));
      EXPECT_EQ(trigger.at("at"), "15:30");
    }
    const auto placed = write(*engine, "POST", "/api/orders", body);
    ASSERT_EQ(placed.status, 201) << placed.body;
    const auto id = std::stoull(json::parse(placed.body)["order"]["id"].get<std::string>());
    const auto view = engine->trading_view();
    ASSERT_GE(view->snapshot->recent_orders.size(), id);
    const auto& parsed = view->snapshot->recent_orders.at(id - 1).request;
    EXPECT_EQ(parsed, request);
    EXPECT_EQ(server::order_request_json(parsed), body);
  }
}

TEST_F(PaperEngine, HeldContractExitsAndAPairCancelOverHttp) {
  seed();
  auto entry = order(market, "long", "4.20");
  entry["quantity"] = 2;
  ASSERT_EQ(write(*engine, "POST", "/api/orders", entry).status, 201);
  const json stop{{"source", "option"}, {"direction", "at_or_below"}, {"level", "3.50"}};
  json exits{{"client_order_id", "held"}, {"symbol", market.symbol()}, {"side", "sell"}, {"type", "limit"}, {"quantity", 2},
             {"limit_price", "5.00"}, {"time_in_force", "gtc"}, {"exits_only", true},
             {"bracket", {{"take_profit", {{"limit_price", "5.00"}}}, {"stop_loss", {{"trigger", stop}}}}}};
  const auto placed = write(*engine, "POST", "/api/orders", exits);
  ASSERT_EQ(placed.status, 201) << placed.body;
  const auto target = json::parse(placed.body)["order"];
  EXPECT_EQ(target["role"], "take_profit");
  EXPECT_EQ(target["exits_only"], true);
  EXPECT_EQ(target["filled_quantity"], 0);
  const auto stop_id = target["oco"];
  ASSERT_TRUE(stop_id.is_string());
  EXPECT_EQ(target["stop_loss_order"], stop_id);
  EXPECT_EQ(write(*engine, "POST", "/api/orders", exits).status, 200);  // a retry answers once
  // Both exits cancel in one call; the list is all or nothing.
  expect_error(write(*engine, "POST", "/api/orders/cancel", {{"orders", {target["id"], "999"}}}), 404, "UNKNOWN_ORDER");
  expect_error(write(*engine, "POST", "/api/orders/cancel", {{"orders", json::array()}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/orders/cancel", {{"orders", {target["id"], target["id"]}}}), 400, "INVALID_REQUEST");
  expect_error(write(*engine, "POST", "/api/orders/cancel", {{"orders", {target["id"]}}, {"underlying", "SPX"}}), 400, "INVALID_REQUEST");
  const auto cancelled = write(*engine, "POST", "/api/orders/cancel", {{"orders", {target["id"], stop_id}}});
  ASSERT_EQ(cancelled.status, 200) << cancelled.body;
  EXPECT_EQ(json::parse(cancelled.body)["cancelled_orders"], json::array({target["id"], stop_id}));
  EXPECT_TRUE(read(*engine, "/api/orders?status=open")["orders"].empty());
  expect_error(write(*engine, "POST", "/api/orders/cancel", {{"orders", {stop_id}}}), 409, "ORDER_TERMINAL");
  engine->stop();
}

}  // namespace

// The breach estimate's horizon is the rest of today's session. A same-day expiry's
// implied variance is exactly that; a later expiry's is shared out in trading time,
// not calendar time, so a quiet weekend cannot dilute the hours left today.
TEST(PaperBreach, ImpliedVarianceToTheCloseSharesTheFrontExpiryInTradingTime) {
  const auto now = md::new_york_to_utc({2026, 9, 23}, 10, 30);  // a Wednesday
  const auto slice_at = [&](md::Timestamp expiry) {
    analytics::SliceMetrics slice;
    slice.expiry_time = expiry;
    slice.years = md::years_between(now, expiry);
    analytics::StrikeMetrics strike;
    strike.strike = 100;
    strike.iv = 0.2;
    slice.strikes.push_back(strike);
    return slice;
  };
  analytics::UnderlyingMetrics metrics;
  metrics.spot = 100;
  metrics.as_of = now;
  const auto today = md::new_york_to_utc({2026, 9, 23}, 16, 0);
  metrics.slices = {slice_at(today)};
  using openport::server::implied_variance_to_close;
  ASSERT_TRUE(implied_variance_to_close(metrics));
  EXPECT_NEAR(*implied_variance_to_close(metrics), 0.04 * md::years_between(now, today), 1e-15);
  const auto friday = md::new_york_to_utc({2026, 9, 25}, 16, 0);
  metrics.slices = {slice_at(friday)};
  // 5.5 of the 5.5 + 6.5 + 6.5 regular-session hours to Friday's close are today's.
  EXPECT_NEAR(*implied_variance_to_close(metrics), 0.04 * md::years_between(now, friday) * 5.5 / 18.5, 1e-15);
  metrics.as_of = md::new_york_to_utc({2026, 9, 23}, 16, 30);  // after the close
  metrics.slices = {slice_at(friday)};
  EXPECT_EQ(*implied_variance_to_close(metrics), 0.0);
  metrics.slices.clear();
  EXPECT_FALSE(implied_variance_to_close(metrics));
}

TEST(PaperBreach, ImpliedVarianceToClosePrefersAtmIvAndFallsBackToNearestStrike) {
  analytics::UnderlyingMetrics metrics;
  metrics.spot = 100;
  metrics.as_of = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  const auto close = md::new_york_to_utc({2026, 9, 23}, 16, 0);
  analytics::SliceMetrics slice;
  slice.expiry_time = close;
  slice.years = md::years_between(metrics.as_of, close);
  slice.atm_iv = 0.3;
  for (const auto& [price, iv] : {std::pair{110.0, 0.4}, std::pair{101.0, 0.2},
                                std::pair{100.0, analytics::kNaN}}) {
    analytics::StrikeMetrics strike;
    strike.strike = price;
    strike.iv = iv;
    slice.strikes.push_back(strike);
  }
  auto later = slice;
  later.expiry_time += md::kNanosPerDay;
  later.years = md::years_between(metrics.as_of, later.expiry_time);
  later.atm_iv = 0.5;
  metrics.slices = {later, slice};
  using openport::server::implied_variance_to_close;
  ASSERT_TRUE(implied_variance_to_close(metrics));
  EXPECT_NEAR(*implied_variance_to_close(metrics), 0.09 * slice.years, 1e-15);
  for (const double missing : {analytics::kNaN, 0.0, -0.1, std::numeric_limits<double>::infinity()}) {
    metrics.slices[1].atm_iv = missing;
    ASSERT_TRUE(implied_variance_to_close(metrics));
    EXPECT_NEAR(*implied_variance_to_close(metrics), 0.04 * slice.years, 1e-15);
  }
}

// Probability horizons past today's close read the at-the-money term structure in
// calendar time; before it they share today's variance in trading time.
TEST(PaperProbability, ImpliedVarianceUntilADateInterpolatesTheTermStructure) {
  using openport::server::implied_variance_until;
  const auto now = md::new_york_to_utc({2026, 9, 23}, 10, 30);  // a Wednesday
  const auto slice_at = [&](md::Timestamp expiry, double iv) {
    analytics::SliceMetrics slice;
    slice.expiry_time = expiry;
    slice.years = md::years_between(now, expiry);
    slice.atm_iv = iv;
    analytics::StrikeMetrics strike;
    strike.strike = 100;
    strike.iv = iv;
    slice.strikes.push_back(strike);
    return slice;
  };
  analytics::UnderlyingMetrics metrics;
  metrics.spot = 100;
  metrics.as_of = now;
  const auto friday = md::new_york_to_utc({2026, 9, 25}, 16, 0);
  const auto month = md::new_york_to_utc({2026, 10, 23}, 16, 0);
  metrics.slices = {slice_at(friday, 0.2), slice_at(month, 0.3)};
  const auto close = md::new_york_to_utc({2026, 9, 23}, 16, 0);
  const auto to_close = *server::implied_variance_to_close(metrics);
  EXPECT_EQ(*implied_variance_until(metrics, close), to_close);
  // Half of today's remaining session is half of today's variance.
  EXPECT_NEAR(*implied_variance_until(metrics, md::new_york_to_utc({2026, 9, 23}, 13, 15)), to_close / 2, 1e-15);
  // Halfway in time between the expiries, total variance is halfway between theirs.
  const auto middle = friday + (month - friday) / 2;
  const double front = 0.04 * md::years_between(now, friday), back = 0.09 * md::years_between(now, month);
  EXPECT_NEAR(*implied_variance_until(metrics, middle), (front + back) / 2, 1e-12);
  // Past the last expiry its volatility holds.
  const auto later = month + 30 * md::kNanosPerDay;
  EXPECT_NEAR(*implied_variance_until(metrics, later), 0.09 * md::years_between(now, later), 1e-12);
  EXPECT_EQ(*implied_variance_until(metrics, now - 1), 0.0);
  EXPECT_EQ(server::probability_horizon(now, 0), close);
  EXPECT_EQ(server::probability_horizon(now, 1.5), now + 36 * 60 * md::kNanosPerMinute);
  metrics.slices.clear();
  EXPECT_FALSE(implied_variance_until(metrics, middle));
}

TEST(PaperProbability, PriceOddsAddUpAndTouchingIsLikelierThanFinishingBeyond) {
  const auto odds = server::price_odds(100, 110, 0.01);
  EXPECT_NEAR(odds.above + odds.below, 1, 1e-15);
  // ln(100/110) = -0.0953; with v = 0.01, Φ((-0.0953 - 0.005) / 0.1) = Φ(-1.003).
  EXPECT_NEAR(odds.above, 0.15793, 1e-4);
  EXPECT_NEAR(odds.touch, std::erfc(std::log(1.1) / (0.1 * std::sqrt(2.0))), 1e-15);
  EXPECT_GT(odds.touch, odds.above);
  const auto now = server::price_odds(100, 90, 0);
  EXPECT_EQ(now.above, 1);
  EXPECT_EQ(now.touch, 0);
  EXPECT_EQ(server::price_odds(100, 100, 0).touch, 1);
}

TEST(PaperBeta, BetaIsTheCovarianceOverTheBenchmarksVarianceOfPairedReturns) {
  std::vector<md::Bar> asset, benchmark;
  double a = 100, b = 50;
  for (int i = 0; i < 40; ++i) {
    const md::Timestamp start = i * md::kNanosPerDay;
    const double move = (i % 3 == 0 ? 0.01 : i % 3 == 1 ? -0.006 : 0.002);
    b *= std::exp(move);
    a *= std::exp(2 * move);
    benchmark.push_back({start, b, b, b, b});
    if (i != 10) asset.push_back({start, a, a, a, a});  // a missing day pairs no return across it
  }
  const auto beta = server::estimate_beta(asset, benchmark, 252);
  EXPECT_NEAR(beta.beta, 2, 1e-9);
  EXPECT_NEAR(beta.correlation, 1, 1e-9);
  EXPECT_EQ(beta.observations, 37U);  // 39 returns less the two across the gap
  EXPECT_EQ(server::estimate_beta(asset, benchmark, 10).observations, 10U);
  // Bars further apart than the gap allowed pair no return.
  EXPECT_EQ(server::estimate_beta(asset, benchmark, 252, md::kNanosPerDay).observations, 37U);
  EXPECT_EQ(server::estimate_beta(asset, benchmark, 252, md::kNanosPerMinute).observations, 0U);
  EXPECT_TRUE(server::estimate_beta({}, benchmark, 252).source.empty());
  EXPECT_TRUE(server::same_index("SPY", "SPX"));
  EXPECT_TRUE(server::same_index("QQQ", "NDX"));
  EXPECT_FALSE(server::same_index("QQQ", "SPY"));
}

namespace {
TEST_F(PaperEngine, RiskProfileCurvesTheHeldBookOverDatesAndBetaWeightsIt) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset", {{"plan", "eod-25k"}, {"reason", "evaluation"}}).status, 200);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "entry", "4.20")).status, 201);
  engine->synchronize().get();
  auto profile = read(*engine, "/api/risk/profile?underlying=SPX&days=0,1,expiry&range=10&steps=5&iv=2");
  EXPECT_EQ(profile["reference"], "SPX");
  EXPECT_TRUE(profile["benchmark"].is_null());
  EXPECT_EQ(profile["spot"], 5000);
  EXPECT_EQ(profile["percent"], json::array({-10, -5, 0, 5, 10}));
  EXPECT_NEAR(profile["prices"][1].get<double>(), 4750, 1e-9);
  EXPECT_TRUE(profile["complete"]);
  EXPECT_TRUE(profile["room"].is_string());
  ASSERT_EQ(profile["curves"].size(), 3U);
  EXPECT_EQ(profile["curves"][2]["label"], "expiry");
  EXPECT_TRUE(profile["curves"][0]["label"].is_null());
  EXPECT_GT(profile["curves"][0]["pnl"][4].get<double>(), profile["curves"][0]["pnl"][2].get<double>());
  EXPECT_TRUE(profile["curves"][0]["horizon"]["sigma"].is_number());
  EXPECT_LT(profile["curves"][0]["horizon"]["one_sd"]["low"].get<double>(), 5000);
  EXPECT_EQ(profile["betas"][0]["source"], "reference");
  // The whole book, weighted to SPY: SPX follows the same index.
  profile = read(*engine, "/api/risk/profile");
  EXPECT_EQ(profile["benchmark"], "SPY");
  EXPECT_EQ(profile["percent"].size(), 41U);
  ASSERT_EQ(profile["curves"].size(), 2U);
  EXPECT_EQ(profile["curves"][0]["pnl"][20], 0);
  ASSERT_EQ(profile["betas"].size(), 1U);
  EXPECT_EQ(profile["betas"][0]["source"], "index");
  EXPECT_EQ(profile["betas"][0]["beta"], 1);
  EXPECT_EQ(profile["weighted_dollar_delta"], profile["betas"][0]["dollar_delta"]);
  profile = read(*engine, "/api/risk/profile?benchmark=SPX&betas=SPX:1.5");
  EXPECT_EQ(profile["betas"][0]["source"], "reference");
  for (const auto* bad : {"?benchmark=QQQ", "?days=-1", "?days=0,1,2,3,4,5,6,7,8", "?steps=4", "?range=0", "?iv=500",
                          "?betas=QQQ", "?betas=QQQ:x", "?other=1", "?underlying=spx"})
    EXPECT_EQ(server::handle_api({"GET", std::string("/api/risk/profile") + bad}, *engine).status, 400) << bad;
  EXPECT_EQ(server::handle_api({"GET", "/api/risk/profile?account=nobody"}, *engine).status, 404);
  // Probability cones and price odds for any underlying.
  const auto odds = read(*engine, "/api/underlyings/SPX/probability?days=0,7&prices=4900,5100");
  EXPECT_EQ(odds["symbol"], "SPX");
  ASSERT_EQ(odds["horizons"].size(), 2U);
  EXPECT_EQ(odds["horizons"][1]["days"], 7);
  const auto& week = odds["horizons"][1]["prices"];
  ASSERT_EQ(week.size(), 2U);
  EXPECT_NEAR(week[0]["above"].get<double>() + week[0]["below"].get<double>(), 1, 1e-6);
  EXPECT_GT(week[0]["touch"].get<double>(), week[0]["below"].get<double>());
  EXPECT_GT(odds["horizons"][1]["sigma"].get<double>(), odds["horizons"][0]["sigma"].get<double>());
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings/SPX/probability?days=400"}, *engine).status, 400);
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings/SPX/probability?prices=0"}, *engine).status, 400);
}
}  // namespace

namespace {
TEST_F(PaperEngine, AuthenticatedActorPassesThroughQueueToOrdersFillsAndCsv) {
  seed();
  auto forged = order(market);
  forged["actor"] = "admin";
  EXPECT_EQ(write(*engine, "POST", "/api/orders", forged).status, 400);
  server::ApiRequest request{"POST", "/api/orders", order(market).dump()};
  request.actor = "research-agent";
  auto promise = std::make_shared<std::promise<server::ApiResponse>>();
  auto future = promise->get_future();
  server::handle_api_async(request, *engine, [promise](auto response) { promise->set_value(std::move(response)); });
  ASSERT_EQ(future.wait_for(std::chrono::minutes(5)), std::future_status::ready);
  const auto response = future.get();
  ASSERT_EQ(response.status, 201) << response.body;
  EXPECT_EQ(json::parse(response.body)["order"]["actor"], "research-agent");
  market.next();
  quote("3.80", "4.00");
  ASSERT_TRUE(wait_for([&] { return !engine->trading_view()->snapshot->recent_fills.empty(); }));
  EXPECT_EQ(read(*engine, "/api/orders")["orders"][0]["actor"], "research-agent");
  EXPECT_EQ(read(*engine, "/api/fills")["fills"][0]["actor"], "research-agent");
  const auto csv = server::handle_api({"GET", "/api/fills.csv"}, *engine);
  EXPECT_NE(csv.body.find("order_id,actor,symbol"), std::string::npos);
  EXPECT_NE(csv.body.find("research-agent"), std::string::npos);
}
}  // namespace

namespace {
TEST_F(PaperEngine, ContractFixture) {
  seed();
  const auto capture = [&](std::string method, std::string path, json body = nullptr) {
    auto response = method == "GET" ? server::handle_api({method, path}, *engine) : write(*engine, method, path, body);
    EXPECT_GE(response.status, 200);
    EXPECT_LT(response.status, 300) << path << ": " << response.body;
    test::capture_contract("paper", method, path, response);
    return json::parse(response.body);
  };
  capture("POST", "/api/orders/preview", order(market));
  const auto placed = capture("POST", "/api/orders", order(market));
  const auto id = placed.at("order").at("id").get<std::string>();
  capture("POST", "/api/orders/" + id + "/preview", {{"quantity", 2}});
  capture("POST", "/api/orders/what-if", {{"candidates", json::array({{{"name", "Add one"}, {"orders", json::array({order(market, "what-if")})}}})}});
  capture("PUT", "/api/orders/" + id, {{"limit_price", "4.20"}});
  market.next();
  quote();
  for (const auto* path : {"/api/account", "/api/portfolio", "/api/orders", "/api/fills", "/api/trades", "/api/settlements", "/api/risk",
                           "/api/plans", "/api/accounts", "/api/account/equity"}) capture("GET", path);
  for (const auto* path : {"/api/fills.csv", "/api/trades.csv"})
    test::capture_contract("paper", "GET", path, server::handle_api({"GET", path}, *engine));
  capture("PUT", "/api/trades/1/note", {{"note", "synthetic trade"}, {"tags", {"test"}}});
  capture("PUT", "/api/days/2026-09-22/note", {{"plan", "test"}, {"review", "test"}});
  const auto created = capture("POST", "/api/alerts", {{"label", "test"}, {"scope", "contract"}, {"metric", "bid"},
                                                       {"symbol", market.symbol()}, {"direction", "at_or_above"}, {"level", "9.00"}});
  capture("GET", "/api/alerts");
  capture("DELETE", "/api/alerts/" + created.at("alert").at("id").get<std::string>());
  capture("POST", "/api/positions/close/preview", json::object());
  capture("POST", "/api/positions/close", json::object());
  capture("POST", "/api/orders/cancel", json::object());
  capture("POST", "/api/risk/kill", {{"action", "trip"}, {"reason", "test"}});
  auto risk = capture("GET", "/api/risk");
  auto guardrails = risk["guardrails"];
  guardrails["soft_floor_percent"] = 100;
  const auto invalid = write(*engine, "PUT", "/api/risk/guardrails",
      {{"expected_revision", risk["limits_revision"]}, {"guardrails", guardrails}});
  expect_error(invalid, 422, "INVALID_LIMITS");
  test::capture_contract("paper", "PUT", "/api/risk/guardrails", invalid);
  guardrails["soft_floor_percent"] = 99;
  guardrails["max_opening_trades"] = 1;
  risk = capture("PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", guardrails}});
  EXPECT_EQ(risk["warnings"].back()["code"], "SOFT_FLOOR_UNUSED");
  capture("POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "retain discipline"}});
  risk = capture("GET", "/api/risk");
  EXPECT_EQ(risk["kill"]["reset_blocked"]["code"], "TRADE_LIMIT");
  const auto blocked = write(*engine, "POST", "/api/risk/kill", {{"action", "reset"}, {"reason", "cannot bypass"}});
  expect_error(blocked, 422, "TRADE_LIMIT");
  test::capture_contract("paper", "POST", "/api/risk/kill", blocked);
  guardrails["soft_floor"] = "90000";
  capture("PUT", "/api/risk/guardrails", {{"expected_revision", risk["limits_revision"]}, {"guardrails", guardrails}});
  capture("POST", "/api/account/reset", {{"plan", "intraday-25k"}, {"reason", "carried floor"}});
  capture("GET", "/api/risk");
}

TEST(PaperStocks, PositionDisposalContractFixture) {
  PaperProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto contract = *md::parse_osi("SPY261022C00500000");
  const auto symbol = contract.osi_symbol();
  auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto quote = [&](double bid, double bid_size) {
    provider.sink->publish(md::UnderlyingQuote{"SPY", time, 519.9, 520.1, 520});
    provider.sink->publish(md::OptionQuote{0, time, bid, 21.2, bid_size, 10});
    ASSERT_TRUE(wait_for([&] { const auto m = engine.metrics("SPY"); return m && m->as_of == time && !m->slices.empty(); }));
  };
  provider.sink->publish(md::ContractDefinition{0, contract});
  quote(21.0, 10);
  const auto bought = write(engine, "POST", "/api/orders", {{"client_order_id", "calls"}, {"symbol", symbol}, {"side", "buy"},
      {"type", "limit"}, {"quantity", 2}, {"limit_price", "21.20"}, {"time_in_force", "day"}});
  ASSERT_EQ(bought.status, 201) << bought.body;
  const auto instructed = write(engine, "POST", "/api/positions/instruction", {{"symbol", symbol}, {"do_not_exercise", true}});
  ASSERT_EQ(instructed.status, 200) << instructed.body;
  test::capture_contract("disposal", "POST", "/api/positions/instruction", instructed);
  auto position = json::parse(instructed.body)["positions"][0];
  EXPECT_EQ(position["do_not_exercise"], true);
  EXPECT_EQ(position["no_bid"], false);
  EXPECT_EQ(write(engine, "POST", "/api/positions/instruction", {{"symbol", symbol}}).status, 400);
  for (const auto& body : {json{{"symbol", symbol}}, json{{"symbol", symbol}, {"do_not_exercise", "true"}},
                           json{{"symbol", symbol}, {"do_not_exercise", true}, {"extra", 1}}}) {
    const auto invalid = write(engine, "POST", "/api/positions/instruction", body);
    EXPECT_EQ(invalid.status, 400) << invalid.body;
  }
  const auto withdrawn = write(engine, "POST", "/api/positions/instruction", {{"symbol", symbol}, {"do_not_exercise", false}});
  ASSERT_EQ(withdrawn.status, 200) << withdrawn.body;
  EXPECT_EQ(json::parse(withdrawn.body)["positions"][0]["do_not_exercise"], false);
  const auto unknown = write(engine, "POST", "/api/positions/abandon", {{"symbol", "SPY   261022C00600000"}});
  EXPECT_EQ(unknown.status, 404) << unknown.body;
  EXPECT_EQ(json::parse(unknown.body)["error"]["code"], "UNKNOWN_CONTRACT");
  // Someone bids, so it cannot be abandoned yet.
  const auto refused = write(engine, "POST", "/api/positions/abandon", {{"symbol", symbol}});
  EXPECT_EQ(refused.status, 422) << refused.body;
  EXPECT_EQ(json::parse(refused.body)["error"]["code"], "INVALID_ORDER");
  // Nobody bids now: 0.00 x 21.20.
  time += md::kNanosPerSecond;
  quote(0, 0);
  // The underlying can advance the clock before the option quote is published.
  ASSERT_TRUE(wait_for([&] {
    const auto snapshot = engine.trading_view()->snapshot;
    return snapshot->time == time && !snapshot->positions.empty() && snapshot->positions[0].no_bid;
  }));
  EXPECT_EQ(read(engine, "/api/portfolio")["positions"][0]["no_bid"], true);
  const auto abandoned = write(engine, "POST", "/api/positions/abandon", {{"symbol", symbol}});
  ASSERT_EQ(abandoned.status, 200) << abandoned.body;
  test::capture_contract("disposal", "POST", "/api/positions/abandon", abandoned);
  test::capture_contract("disposal", "GET", "/api/trades", server::handle_api({"GET", "/api/trades"}, engine));
  EXPECT_TRUE(json::parse(abandoned.body)["positions"].empty());
  const auto trades = read(engine, "/api/trades")["trades"];
  ASSERT_EQ(trades.size(), 1);
  EXPECT_EQ(trades[0]["closure"], "abandon");
  EXPECT_EQ(trades[0]["average_close"], "0.00");
  engine.stop();
}

TEST_F(PaperEngine, ExtendedTimeInForceAndGtdRoundTripAndValidate) {
  seed();
  for (const auto tif : {"exto", "gtc_exto", "gtd"}) {
    auto body = order(market, tif);
    body["time_in_force"] = tif;
    if (std::string_view(tif) == "gtd") body["good_till"] = "2026-09-23T10:15:00-04:00";
    const auto response = write(*engine, "POST", "/api/orders", body);
    ASSERT_EQ(response.status, 201) << response.body;
    const auto saved = json::parse(response.body)["order"];
    EXPECT_EQ(saved["time_in_force"], tif);
    if (std::string_view(tif) == "gtd") { EXPECT_EQ(saved["good_till"], "2026-09-23T14:15:00.000Z"); }
    else { EXPECT_TRUE(saved["good_till"].is_null()); }
  }
  auto body = order(market, "bad-gtd"); body["time_in_force"] = "gtd";
  expect_error(write(*engine, "POST", "/api/orders", body), 400, "INVALID_REQUEST");
  body["good_till"] = "2026-09-23T10:15:00";
  expect_error(write(*engine, "POST", "/api/orders", body), 400, "INVALID_REQUEST");
  body["good_till"] = "2026-09-21T14:15:00Z";
  expect_error(write(*engine, "POST", "/api/orders", body), 422, "INVALID_ORDER");
  body["client_order_id"] = "stop"; body.erase("good_till"); body.erase("limit_price");
  body["time_in_force"] = "gtc_exto"; body["type"] = "market";
  body["trigger"] = {{"source", "option"}, {"direction", "at_or_below"}, {"level", "3.00"}};
  ASSERT_EQ(write(*engine, "POST", "/api/orders/cancel", json::object()).status, 200);
  const auto stop = write(*engine, "POST", "/api/orders", body);
  ASSERT_EQ(stop.status, 201) << stop.body;
  EXPECT_EQ(json::parse(stop.body)["order"]["status"], "armed");
}

TEST_F(PaperEngine, LimitFlattenPreviewAndCloseAcceptBoundedPricing) {
  seed();
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "held", "4.20")).status, 201);
  for (const auto path : {"/api/positions/close/preview", "/api/positions/close"}) {
    expect_error(write(*engine, "POST", path, {{"type", "market"}, {"limit_ticks", 1}}), 400, "INVALID_REQUEST");
    expect_error(write(*engine, "POST", path, {{"type", "limit"}, {"limit_ticks", 11}}), 400, "INVALID_REQUEST");
    expect_error(write(*engine, "POST", path, {{"type", "limit"}, {"limit_ticks", -1}}), 400, "INVALID_REQUEST");
    const auto response = write(*engine, "POST", path, {{"type", "limit"}, {"limit_ticks", 1}});
    ASSERT_EQ(response.status, 200) << response.body;
  }
  const auto orders = read(*engine, "/api/orders?status=all")["orders"];
  ASSERT_EQ(orders.size(), 2);
  const auto close = std::find_if(orders.begin(), orders.end(), [](const json& o) { return o["reduce_only"] == true; });
  ASSERT_NE(close, orders.end());
  EXPECT_EQ((*close)["type"], "limit");
  EXPECT_EQ((*close)["time_in_force"], "exto");
  EXPECT_EQ((*close)["limit_ticks"], 1);
}

TEST_F(PaperEngine, ItemizedFeesComeFromTheAttemptAndShowOnPreviewsAndFills) {
  const auto reset = write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "fees"}, {"fee_model", "itemized"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  const auto fees = json::parse(reset.body)["rules"]["fees"];
  EXPECT_EQ(fees, (json{{"open", "1.00"}, {"close", "0.00"}, {"leg_cap", "10.00"}, {"clearing", "0.10"}, {"regulatory", "0.02"},
                        {"index", {{"SPX", "0.60"}, {"SPXW", "0.60"}}}, {"exercise", "5.00"}}));
  EXPECT_EQ(read(*engine, "/api/account")["rules"]["fees"], fees);
  seed("4.00", "4.20", 10);
  auto buy = order(market, "fees", "4.20");
  buy["quantity"] = 2;
  const auto preview = json::parse(write(*engine, "POST", "/api/orders/preview", buy).body);
  EXPECT_EQ(preview["fee"], "3.44");
  EXPECT_EQ(preview["fees"], (json{{"commission", "2.00"}, {"clearing", "0.20"}, {"regulatory", "0.04"}, {"index", "1.20"}}));
  ASSERT_EQ(write(*engine, "POST", "/api/orders", buy).status, 201);
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/fills")["fills"].size() == 1; }));
  const auto fill = read(*engine, "/api/fills")["fills"][0];
  EXPECT_EQ(fill["fee"], "3.44");
  EXPECT_EQ(fill["fees"], preview["fees"]);
  // Custom rules take their own schedule; the flat model keeps the flat fee.
  json rules{{"profit_target", nullptr}, {"max_drawdown", nullptr}, {"drawdown_mode", "intraday"},
             {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0},
             {"fees", {{"open", "0.50"}, {"index", {{"SPXW", "0.25"}}}}}};
  auto custom = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules}, {"reason", "custom fees"}});
  ASSERT_EQ(custom.status, 200) << custom.body;
  EXPECT_EQ(json::parse(custom.body)["rules"]["fees"]["open"], "0.50");
  EXPECT_EQ(json::parse(custom.body)["rules"]["fees"]["close"], "0.00");
  EXPECT_EQ(json::parse(custom.body)["rules"]["fees"]["index"], (json{{"SPXW", "0.25"}}));
  rules["fees"] = {{"open", "-1"}};
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules}, {"reason", "bad"}}),
               400, "INVALID_RULES");
  rules["fees"] = {{"opening", "1"}};
  expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules}, {"reason", "bad"}}),
               400, "INVALID_REQUEST");
  const auto flat = write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "flat"}, {"fee_model", "flat"}});
  ASSERT_EQ(flat.status, 200) << flat.body;
  EXPECT_FALSE(json::parse(flat.body)["rules"].contains("fees"));
  expect_error(write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "bad"}, {"fee_model", "broker"}}),
               400, "INVALID_REQUEST");
  rules["fees"] = nullptr;
  const auto no_schedule = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "50000"}, {"rules", rules}, {"reason", "flat"}});
  ASSERT_EQ(no_schedule.status, 200) << no_schedule.body;
  EXPECT_FALSE(json::parse(no_schedule.body)["rules"].contains("fees"));
}

TEST(PaperAccounts, CreationSelectsAndValidatesFeeSchedules) {
  const auto directory = paper_path().parent_path();
  auto options = paper_options();
  options.paper_journal = directory / "paper.jsonl";
  options.paper_accounts = directory / "accounts";
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    const auto created = write(engine, "POST", "/api/accounts", {{"name", "Fee account"}, {"plan", "practice"}, {"fee_model", "itemized"}});
    ASSERT_EQ(created.status, 201) << created.body;
    const auto id = json::parse(created.body)["account"]["id"].get<std::string>();
    const auto fees = read(engine, "/api/account?account=" + id)["rules"]["fees"];
    EXPECT_EQ(fees["open"], "1.00");
    EXPECT_EQ(fees["index"]["SPXW"], "0.60");
    EXPECT_EQ(fees["exercise"], "5.00");
    json rules{{"profit_target", nullptr}, {"max_drawdown", nullptr}, {"drawdown_mode", "intraday"},
               {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0},
               {"fees", {{"open", "0.50"}}}};
    rules["no_hedging"] = true; rules["no_counter_positions"] = true; rules["max_volume_percent"] = 25;
    const auto custom = write(engine, "POST", "/api/accounts", {{"name", "Custom"}, {"initial_cash", "50000"}, {"rules", rules}});
    ASSERT_EQ(custom.status, 201) << custom.body;
    const auto custom_id = json::parse(custom.body)["account"]["id"].get<std::string>();
    const auto direction_rules = read(engine, "/api/account?account=" + custom_id)["rules"];
    EXPECT_EQ(direction_rules["max_volume_percent"], 25);
    EXPECT_EQ(direction_rules["no_hedging"], true);
    EXPECT_EQ(direction_rules["no_counter_positions"], true);
    EXPECT_EQ(read(engine, "/api/account?account=" + custom_id)["rules"]["fees"]["open"], "0.50");
    const auto expect_invalid = [&](int status, const char* code) {
      const auto response = write(engine, "POST", "/api/accounts", {{"name", "Bad"}, {"initial_cash", "50000"}, {"rules", rules}});
      EXPECT_EQ(response.status, status) << response.body;
      EXPECT_EQ(json::parse(response.body)["error"]["code"], code);
    };
    for (const auto& malformed : {json(1), json::array(), json{{"open", 1}}, json{{"open", nullptr}},
                                 json{{"index", json::array()}}, json{{"index", {{"SPXW", 0.6}}}}}) {
      rules["fees"] = malformed;
      expect_invalid(400, "INVALID_REQUEST");
    }
    for (const auto& invalid : {json{{"open", "1000.000001"}}, json{{"index", {{"spx", "0.60"}}}},
                               json{{"index", {{"SPXW", "-0.01"}}}}}) {
      rules["fees"] = invalid;
      expect_invalid(400, "INVALID_RULES");
    }
  }
  std::filesystem::remove_all(directory);
}

TEST(PaperStocks, SharesOpenThroughTheApiAtTheUnderlyingsPrice) {
  PaperProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  provider.sink->publish(md::UnderlyingQuote{"SPY", time, 519.9, 520.1, 520});
  ASSERT_TRUE(wait_for([&] { const auto view = engine.trading_view(); return view && view->market_times.contains("SPY") && view->market_times.at("SPY") == time; }));
  EXPECT_EQ(write(engine, "POST", "/api/stocks/trade", {{"symbol", "SPY"}, {"side", "buy"}}).status, 400);
  EXPECT_EQ(write(engine, "POST", "/api/stocks/trade", {{"symbol", "SPY"}, {"side", "hold"}, {"shares", 1}}).status, 400);
  EXPECT_EQ(write(engine, "POST", "/api/stocks/trade", {{"symbol", "SPY"}, {"side", "buy"}, {"shares", 0}}).status, 400);
  EXPECT_EQ(write(engine, "POST", "/api/stocks/trade", {{"symbol", "spy"}, {"side", "buy"}, {"shares", 1}}).status, 400);
  const auto before = read(engine, "/api/portfolio");
  const auto preview = write(engine, "POST", "/api/stocks/trade/preview", {{"symbol", "SPY"}, {"side", "buy"}, {"shares", 100}});
  ASSERT_EQ(preview.status, 200) << preview.body;
  test::capture_contract("stocks", "POST", "/api/stocks/trade/preview", preview);
  EXPECT_EQ(json::parse(preview.body)["cost"], "52000.00");
  EXPECT_EQ(json::parse(preview.body)["after"]["exposure"]["dollar_delta"], 52000);
  EXPECT_EQ(read(engine, "/api/portfolio"), before);
  const auto bought = write(engine, "POST", "/api/stocks/trade", {{"symbol", "SPY"}, {"side", "buy"}, {"shares", 100}});
  ASSERT_EQ(bought.status, 200) << bought.body;
  test::capture_contract("stocks", "POST", "/api/stocks/trade", bought);
  auto portfolio = json::parse(bought.body);
  ASSERT_EQ(portfolio["stocks"].size(), 1);
  EXPECT_EQ(portfolio["stocks"][0]["shares"], 100);
  EXPECT_EQ(portfolio["stocks"][0]["average_price"], "520.00");
  // The close endpoint must never turn an oversized close into an opening trade.
  EXPECT_EQ(write(engine, "POST", "/api/stocks/close", {{"symbol", "SPY"}, {"shares", 101}}).status, 422);
  EXPECT_EQ(read(engine, "/api/portfolio")["stocks"][0]["shares"], 100);
  const auto sold = write(engine, "POST", "/api/stocks/trade", {{"symbol", "SPY"}, {"side", "sell"}, {"shares", 150}});
  ASSERT_EQ(sold.status, 200) << sold.body;
  EXPECT_EQ(json::parse(sold.body)["stocks"][0]["shares"], -50);
  EXPECT_EQ(read(engine, "/api/trades")["share_trades"].size(), 2);
  engine.stop();
}

TEST(PaperAvailability, DamagedMainAndNamedAccountsExposeOnlyTheVerifiedPrefix) {
  const auto expect_error = [](const server::ApiResponse& response, int status, const std::string& code) {
    EXPECT_EQ(response.status, status) << response.body;
    EXPECT_EQ(json::parse(response.body)["error"]["code"], code);
  };
  for (const bool main : {false, true}) {
    for (const bool torn : {false, true}) {
      const auto path = paper_path();
      const auto directory = path.parent_path();
      struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{directory};
      const auto journal = main ? path : directory / "accounts" / "broken.jsonl";
      std::filesystem::create_directories(journal.parent_path());
      const auto contents = [&] { std::ifstream in(journal); std::ostringstream out; out << in.rdbuf(); return out.str(); };
      test::ScriptedMarket market;
      std::string good;
      std::uint64_t seq = 0;
      {
        auto sink = trading::FileJournal::create(journal.string());
        trading::TradingSession session({}, market.time, sink);
        market.seed(session);
        good = contents();
        seq = sink->sequence();
        market.next();
        session.on_quotes({market.quote("4.10", "4.30", 2)}, {market.valuation()}, market.time);
      }
      const auto later = contents().substr(good.size());
      const auto damaged = torn ? good + "torn" : good + "bad hash chain\n" + later;
      { std::ofstream out(journal); out << damaged; }
      auto options = paper_options();
      options.paper_journal = path;
      options.paper_accounts = directory / "accounts";
      PaperProvider provider;
      server::Engine engine(provider, {{"SPX"}}, options);
      ASSERT_NO_THROW(engine.start());
      const std::string query = main ? "" : "?account=broken";
      const auto account = read(engine, "/api/account" + query);
      ASSERT_TRUE(account.at("damaged").is_object());
      EXPECT_EQ(account["damaged"]["last_good_seq"], seq);
      EXPECT_NE(account["damaged"]["reason"].get<std::string>().find("--repair-journals"), std::string::npos);
      EXPECT_EQ(account["journal_size"]["bytes"], damaged.size());
      for (const auto* route : {"/api/portfolio", "/api/trades", "/api/fills", "/api/account/equity"})
        EXPECT_EQ(server::handle_api({"GET", std::string(route) + query}, engine).status, 200);
      const auto list = read(engine, "/api/accounts")["accounts"];
      const auto id = main ? "main" : "broken";
      const auto found = std::find_if(list.begin(), list.end(), [&](const auto& a) { return a.at("id") == id; });
      ASSERT_NE(found, list.end());
      EXPECT_EQ(found->at("damaged"), account["damaged"]);
      EXPECT_FALSE(found->at("trading").at("enabled").template get<bool>());
      expect_error(write(engine, "POST", "/api/risk/kill" + query, {{"action", "trip"}, {"reason", "test"}}), 409, "ACCOUNT_DAMAGED");
      if (!main) {
        expect_error(write(engine, "PATCH", "/api/accounts/broken", {{"name", "rename"}}), 409, "ACCOUNT_DAMAGED");
        expect_error(write(engine, "DELETE", "/api/accounts/broken", nullptr), 409, "ACCOUNT_DAMAGED");
      }
      provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
      EXPECT_EQ(read(engine, "/api/account" + query)["time"], account["time"]);
      engine.stop();
      EXPECT_EQ(contents(), damaged);
    }
  }
}
TEST(PaperAvailability, EquityPagesKeepEqualTimeSamplesAndUnpagedReads) {
  const auto path = paper_path();
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{path.parent_path()};
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  {
    server::EquityStore store(path.string() + ".equity.csv");
    for (std::uint64_t fill = 1; fill <= 5; ++fill)
      store.append({time, 1, Money::parse("100000"), {}, Money::parse("100000"), {}, {}, fill});
    store.append({time + md::kNanosPerMinute, 1, Money::parse("100001"), {}, Money::parse("100001"), {}, {}, 0});
  }
  auto options = paper_options(); options.paper_journal = path;
  PaperProvider provider; server::Engine engine(provider, {{"SPX"}}, options); engine.start();
  const auto all = read(engine, "/api/account/equity");
  ASSERT_EQ(all["samples"].size(), 6u);
  EXPECT_EQ(all["next"], nullptr);
  json collected = json::array();
  std::string cursor;
  for (int page = 0; page < 3; ++page) {
    const auto response = read(engine, "/api/account/equity?limit=2" + (cursor.empty() ? "" : "&cursor=" + cursor));
    ASSERT_EQ(response["samples"].size(), 2u);
    for (const auto& sample : response["samples"]) collected.push_back(sample);
    cursor = response["next"].is_null() ? "" : response["next"].get<std::string>();
  }
  EXPECT_TRUE(cursor.empty());
  EXPECT_EQ(collected, all["samples"]);
  EXPECT_EQ(read(engine, "/api/account/equity?limit=2&from=" + md::format_timestamp(time + md::kNanosPerMinute))["samples"].size(), 1u);
  for (const auto* query : {"limit=0", "limit=2001", "limit=-1", "cursor=bad&limit=2", "cursor=1:1", "limit=2&cursor=1:0"})
    EXPECT_EQ(server::handle_api({"GET", std::string("/api/account/equity?") + query}, engine).status, 400);
  engine.stop();
}
}  // namespace

TEST_F(PaperEngine, VolumeRuleRoundTripsAndExplainsUnknownVolumeInPreview) {
  for (const auto phase : {"evaluation", "verification"}) {
    SCOPED_TRACE(phase);
    seed();
    auto rules = read(*engine, "/api/account")["rules"];
    rules["phase"] = phase;
    rules["plan"] = "Volume share"; rules["max_volume_percent"] = 25;
    const auto reset = [&](const json& r) {
      return write(*engine, "POST", "/api/account/reset", {{"reason", "volume"}, {"initial_cash", "100000"}, {"rules", r}});
    };
    ASSERT_EQ(reset(rules).status, 200);
    EXPECT_EQ(read(*engine, "/api/account")["rules"]["max_volume_percent"], 25);
    const auto before = read(*engine, "/api/orders")["orders"];
    const auto request = order(market, "unknown-volume", "4.20");
    const auto preview = write(*engine, "POST", "/api/orders/preview", request);
    ASSERT_EQ(preview.status, 200) << preview.body;
    const auto reason = json::parse(preview.body)["reason"];
    EXPECT_EQ(reason["code"], "MAX_VOLUME_SHARE");
    EXPECT_EQ(reason["evidence"], (json{{"contract", market.symbol()}, {"contracts", 1}, {"volume", nullptr}, {"percent", 25}}));
    const auto refused = write(*engine, "POST", "/api/orders", request);
    ASSERT_EQ(refused.status, 422) << refused.body;
    EXPECT_EQ(json::parse(refused.body)["error"]["evidence"], reason["evidence"]);
    EXPECT_EQ(read(*engine, "/api/orders")["orders"], before);
    test::capture_contract("volume-rule", "POST", "/api/orders/preview", preview);
    test::capture_contract("volume-rule", "POST", "/api/orders", refused);
    for (const auto& value : {json(-1), json(101), json(1.5), json(true), json(nullptr)}) {
      auto bad = rules; bad["max_volume_percent"] = value;
      EXPECT_EQ(reset(bad).status, 400);
    }
  }
}

TEST_F(PaperEngine, VolumeFillCancellationReportsRuleEvidenceAndPreservesEarlierFills) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules["max_volume_percent"] = 10;
  ASSERT_EQ(write(*engine, "POST", "/api/account/reset",
      {{"reason", "volume fills"}, {"initial_cash", "100000"}, {"rules", rules}}).status, 200);
  provider.sink->publish(md::OptionVolume{0, market.time, 20});
  engine->synchronize().get();
  auto request = order(market, "partial-volume", "4.00"); request["quantity"] = 2;
  const auto accepted = write(*engine, "POST", "/api/orders", request);
  ASSERT_EQ(accepted.status, 201) << accepted.body;
  market.next(); quote("3.80", "4.00", 1);
  auto orders = read(*engine, "/api/orders")["orders"];
  ASSERT_EQ(orders.size(), 1U);
  EXPECT_EQ(orders[0]["filled_quantity"], 1);
  market.next();
  provider.sink->publish(md::OptionVolume{0, market.time, 10});
  quote("3.80", "4.00");
  const auto response = server::handle_api({"GET", "/api/orders"}, *engine);
  orders = json::parse(response.body)["orders"];
  EXPECT_EQ(orders[0]["status"], "cancelled");
  EXPECT_EQ(orders[0]["filled_quantity"], 1);
  EXPECT_EQ(orders[0]["reason"]["code"], "MAX_VOLUME_SHARE");
  EXPECT_EQ(orders[0]["reason"]["actual"], 2);
  EXPECT_EQ(orders[0]["reason"]["limit"], 1);
  EXPECT_EQ(orders[0]["reason"]["evidence"], (json{{"contract", market.symbol()}, {"contracts", 2}, {"volume", 10}, {"percent", 10}}));
  test::capture_contract("volume-rule", "GET", "/api/orders", response);
}

TEST_F(PaperEngine, DirectionRulesRoundTripAndHedgingPreviewEvidence) {
  for (const auto phase : {"evaluation", "verification"}) {
    SCOPED_TRACE(phase);
    seed();
    auto rules = read(*engine, "/api/account")["rules"];
    rules["phase"] = phase;
    rules["plan"] = "Directions"; rules["no_hedging"] = true; rules["no_counter_positions"] = true;
    const auto reset = [&](const json& r) {
      return write(*engine, "POST", "/api/account/reset", {{"reason", "directions"}, {"initial_cash", "100000"}, {"rules", r}});
    };
    ASSERT_EQ(reset(rules).status, 200);
    auto account = read(*engine, "/api/account");
    EXPECT_EQ(account["rules"]["no_hedging"], true);
    EXPECT_EQ(account["rules"]["no_counter_positions"], true);
    ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "long", "4.20")).status, 201);
    auto reverse = order(market, "reverse", "4.00");
    reverse["side"] = "sell"; reverse["quantity"] = 2;
    const auto preview = write(*engine, "POST", "/api/orders/preview", reverse);
    ASSERT_EQ(preview.status, 200) << preview.body;
    const auto reason = json::parse(preview.body)["reason"];
    EXPECT_EQ(reason["code"], "HEDGING");
    EXPECT_EQ(reason["evidence"]["underlying"], "SPX");
    EXPECT_LT(reason["evidence"]["order_dollar_delta"].get<double>(), 0);
    EXPECT_GT(reason["evidence"]["held_dollar_delta"].get<double>(), 0);
    const auto refused = write(*engine, "POST", "/api/orders", reverse);
    ASSERT_EQ(refused.status, 422) << refused.body;
    EXPECT_EQ(json::parse(refused.body)["error"]["evidence"], reason["evidence"]);
    test::capture_contract("direction-rules", "POST", "/api/orders/preview", preview);
    test::capture_contract("direction-rules", "POST", "/api/orders", refused);
    for (const auto* key : {"no_hedging", "no_counter_positions"}) {
      auto bad = rules; bad[key] = 1;
      expect_error(reset(bad), 400, "INVALID_REQUEST");
    }
  }
}

TEST_F(PaperEngine, HeldContractRulesAndRefusalEvidence) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules["plan"] = "Contract cap";
  rules["max_contracts_held"] = 2;
  auto reset = [&](const json& r) {
    return write(*engine, "POST", "/api/account/reset", {{"reason", "cap test"}, {"initial_cash", "100000"}, {"rules", r}});
  };
  ASSERT_EQ(reset(rules).status, 200);
  EXPECT_EQ(read(*engine, "/api/account")["rules"]["max_contracts_held"], 2);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "held", "4.20")).status, 201);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "working", "4.00")).status, 201);
  const auto refused = write(*engine, "POST", "/api/orders", order(market, "excess", "4.00"));
  expect_error(refused, 422, "MAX_CONTRACTS_HELD");
  const auto error = json::parse(refused.body)["error"];
  EXPECT_EQ(error["actual"], 3);
  EXPECT_EQ(error["limit"], 2);
  EXPECT_EQ(error["scope"], "account");
  for (const json& value : {json(-1), json(100001)}) {
    rules["max_contracts_held"] = value;
    expect_error(reset(rules), 400, "INVALID_RULES");
  }
  rules["max_contracts_held"] = 1.5;
  expect_error(reset(rules), 400, "INVALID_REQUEST");
}

TEST_F(PaperEngine, StopAndRiskRulesRoundTripWithPreviewAndRefusalEvidence) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules["plan"] = "Protected trades";
  rules["require_stop_loss"] = true;
  rules["max_trade_risk"] = "100.00";
  rules["max_trade_risk_percent"] = 25;
  auto reset = [&](const json& r) {
    return write(*engine, "POST", "/api/account/reset", {{"reason", "risk test"}, {"initial_cash", "100000"}, {"rules", r}});
  };
  ASSERT_EQ(reset(rules).status, 200);
  const auto wire = read(*engine, "/api/account")["rules"];
  EXPECT_EQ(wire["require_stop_loss"], true);
  EXPECT_EQ(wire["max_trade_risk"], "100.00");
  EXPECT_EQ(wire["max_trade_risk_percent"], 25);
  expect_error(write(*engine, "POST", "/api/orders", order(market, "bare", "4.20")), 422, "STOP_REQUIRED");
  auto entry = order(market, "risky", "4.20");
  entry["bracket"] = {{"stop_loss", {{"trigger", {{"source", "option"}, {"direction", "at_or_below"}, {"level", "3.00"}}}}}};
  const auto preview = write(*engine, "POST", "/api/orders/preview", entry);
  ASSERT_EQ(preview.status, 200) << preview.body;
  const auto p = json::parse(preview.body);
  EXPECT_EQ(p["trade_risk"], "120.00");
  EXPECT_EQ(p["trade_risk_limit"], "100.00");
  EXPECT_EQ(p["trade_risk_basis"], "stop_loss");
  EXPECT_EQ(p["reason"]["actual"], 120);
  EXPECT_EQ(p["reason"]["limit"], 100);
  const auto refused = write(*engine, "POST", "/api/orders", entry);
  expect_error(refused, 422, "MAX_TRADE_RISK");
  EXPECT_EQ(json::parse(refused.body)["error"]["scope"], "trade");
  entry["client_order_id"] = "protected";
  entry["bracket"]["stop_loss"]["trigger"]["level"] = "3.20";
  ASSERT_EQ(write(*engine, "POST", "/api/orders", entry).status, 201);
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"max_trade_risk", "-1"}, {"max_trade_risk_percent", -1}, {"max_trade_risk_percent", 101}}) {
    auto invalid = rules; invalid[key] = value;
    expect_error(reset(invalid), 400, "INVALID_RULES");
  }
  for (const auto& [key, value] : std::vector<std::pair<std::string, json>>{
      {"max_trade_risk", 100}, {"max_trade_risk_percent", 1.5}, {"require_stop_loss", "true"}}) {
    auto invalid = rules; invalid[key] = value;
    expect_error(reset(invalid), 400, "INVALID_REQUEST");
  }
}

namespace {
TEST_F(PaperEngine, TimeAndTradeRulesSurviveCreateResetAndPresetMatching) {
  engine->stop();
  const auto directory = paper_path().parent_path();
  auto options = paper_options();
  options.paper_journal = directory / "paper.jsonl";
  options.paper_accounts = directory / "accounts";
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules.update({{"plan", "Combined rules"}, {"plan_id", nullptr}, {"phase", "verification"},
                {"evaluation_fee", "100.000001"}, {"reset_fee", "25.000002"}, {"activation_fee", "50.000003"}, {"max_resets", 2},
                {"events", {{{"kind", "news"}, {"time", "2026-09-23T14:00:00Z"}},
                            {{"kind", "split"}, {"time", "2026-09-24"}, {"symbol", "SPX"}}}},
                {"news_before_minutes", 5}, {"news_after_minutes", 10}, {"news_action", "flatten"},
                {"hold_restrictions", {"earnings", "ex_dividend", "split", "weekend"}}, {"hold_cutoff", "15:40"}, {"flat_time", "15:45"}, {"no_overnight", true}, {"time_limit_days", 30}, {"inactivity_days", 14},
                {"underlyings", {"SPX"}}, {"trading_start", "09:30"}, {"trading_end", "16:00"},
                {"trade_consistency_percent", 40}, {"min_trades", 12}, {"min_hold_seconds", 60},
                {"microscalp_seconds", 30}, {"microscalp_percent", 25}, {"no_hedging", true}, {"no_counter_positions", true}, {"max_volume_percent", 25},
                {"max_contracts_held", 5}, {"require_stop_loss", true}, {"max_trade_risk", "123.456789"},
                {"max_trade_risk_percent", 25}, {"scaling", {{{"profit", "0.00"}, {"contracts", 2}}}}});
  auto response = write(*engine, "POST", "/api/accounts", {{"name", "Combined rules"}, {"initial_cash", "100000"}, {"rules", rules}});
  ASSERT_EQ(response.status, 201) << response.body;
  test::capture_contract("time-rules", "POST", "/api/accounts", response);
  EXPECT_EQ(read(*engine, "/api/account?account=combined-rules")["rules"], rules);
  response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", rules}, {"reason", "combined rules"}});
  ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["rules"], rules);
  response = write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "archive combined rules"}});
  ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["attempts"].back()["rules"], rules);
  const auto practice = read(*engine, "/api/account")["rules"];
  for (const auto* field : {"events", "news_before_minutes", "news_after_minutes", "news_action", "hold_restrictions", "hold_cutoff", "flat_time", "no_overnight", "time_limit_days", "inactivity_days", "underlyings", "trading_start",
                           "max_contracts_held", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent", "scaling"}) {
    auto borrowed = practice;
    borrowed[field] = rules[field];
    if (std::string_view(field) == "trading_start") borrowed["trading_end"] = rules["trading_end"];
    expect_error(write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", borrowed},
        {"reason", "cannot borrow a preset name"}}), 400, "INVALID_RULES");
  }
  rules.update({{"plan", "Funded combined rules"}, {"phase", "funded"}, {"profit_target", nullptr},
                {"time_limit_days", 0}, {"payouts", {{"qualifying_profit", "0.00"}, {"qualifying_days", 1},
                    {"withdrawal_percent", 50}, {"split_percent", 80}, {"minimum", "0.00"}, {"caps", json::array()},
                    {"consistency_percents", json::array()}, {"buffer", "0.00"}, {"buffer_payouts", 0}}},
                {"size_scaling", {{"profit_percent", 10}, {"payouts", 2}, {"days", 80},
                    {"increase_percent", 25}, {"max_balance", "200000.00"}}}});
  response = write(*engine, "POST", "/api/accounts", {{"name", "Funded combined"}, {"initial_cash", "100000"}, {"rules", rules}});
  // Creating funded accounts directly stays gated; custom funded rules enter through reset.
  expect_error(response, 400, "INVALID_REQUEST");
  response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", rules}, {"reason", "funded combined"}});
  ASSERT_EQ(response.status, 200) << response.body;
  test::capture_contract("time-rules", "POST", "/api/account/reset", response);
  EXPECT_EQ(json::parse(response.body)["rules"], rules);
  const auto funded_evaluation = json::parse(response.body)["evaluation"];
  engine->stop();
  engine = std::make_unique<server::Engine>(provider, md::Subscription{{"SPX"}}, options);
  engine->start();
  ASSERT_TRUE(wait_for([&] { return engine->trading_view() != nullptr; }));
  const auto restored = read(*engine, "/api/account");
  EXPECT_EQ(restored["rules"], rules);
  EXPECT_EQ(restored["evaluation"], funded_evaluation);
  const auto created_rules = read(*engine, "/api/account?account=combined-rules")["rules"];
  EXPECT_EQ(created_rules["phase"], "verification");
  for (const auto* field : {"events", "news_before_minutes", "news_after_minutes", "news_action", "hold_restrictions",
                           "hold_cutoff", "flat_time", "no_overnight", "scaling"}) {
    EXPECT_EQ(created_rules[field], rules[field]) << field;
  }
  response = write(*engine, "POST", "/api/account/reset", {{"plan", "practice"}, {"reason", "archive funded rules"}});
  ASSERT_EQ(response.status, 200) << response.body;
  test::capture_contract("time-rules", "POST", "/api/account/reset", response);
  EXPECT_EQ(json::parse(response.body)["attempts"].back()["rules"], rules);
  engine->stop();
  std::filesystem::remove_all(directory);
}
TEST_F(PaperEngine, TimeRulesRoundTripProgressRefusalsAndValidation) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules.update({{"plan", "Time rules"}, {"time_limit_days", 30}, {"inactivity_days", 14},
                {"underlyings", {"SPX"}}, {"trading_start", "09:30"}, {"trading_end", "11:00"}});
  const auto reset = [&](const json& r) {
    const auto response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", r}, {"reason", "test time rules"}});
    test::capture_contract("time-rules", "POST", "/api/account/reset", response);
    return response;
  };
  auto response = reset(rules);
  ASSERT_EQ(response.status, 200) << response.body;
  auto account = json::parse(response.body);
  EXPECT_EQ(account["rules"], rules);
  EXPECT_EQ(account["evaluation"]["time_limit_days"], 30);
  EXPECT_EQ(account["evaluation"]["deadline"], "2026-10-22");
  EXPECT_EQ(account["evaluation"]["days_left"], 30);
  EXPECT_EQ(account["evaluation"]["inactive_days"], 0);
  EXPECT_EQ(account["evaluation"]["inactivity_deadline"], "2026-10-06");
  EXPECT_EQ(account["evaluation"]["last_activity"], account["evaluation"]["started"]);
  ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "allowed", "4.20")).status, 201);
  rules["underlyings"] = {"SPY"};
  ASSERT_EQ(reset(rules).status, 200);
  response = write(*engine, "POST", "/api/orders", order(market, "disallowed"));
  ASSERT_EQ(response.status, 422) << response.body;
  auto error = json::parse(response.body)["error"];
  EXPECT_EQ(error["code"], "INSTRUMENT_NOT_ALLOWED");
  EXPECT_EQ(error["scope"], "SPX");
  rules["underlyings"] = {"SPX"}; rules["trading_start"] = "10:30";
  ASSERT_EQ(reset(rules).status, 200);
  response = write(*engine, "POST", "/api/orders", order(market, "outside"));
  ASSERT_EQ(response.status, 422) << response.body;
  error = json::parse(response.body)["error"];
  EXPECT_EQ(error["code"], "OUTSIDE_PLAN_HOURS");
  EXPECT_EQ(error["actual"], "10:00"); EXPECT_EQ(error["limit"], "10:30"); EXPECT_EQ(error["scope"], "SPX");
  for (const auto& patch : std::vector<json>{{{"time_limit_days", 367}}, {{"time_limit_days", nullptr}}, {{"inactivity_days", -1}},
      {{"underlyings", {"SPX", "SPX"}}}, {{"underlyings", {"spx"}}}, {{"trading_end", nullptr}},
      {{"trading_start", "11:00"}}, {{"trading_start", "09:60"}}, {{"trading_start", 570}},
      {{"underlyings", "SPX"}}, {{"underlyings", {1}}}, {{"underlyings", nullptr}}, {{"inactivity_days", 1.5}}}) {
    auto bad = rules; bad.update(patch);
    const auto invalid = reset(bad);
    expect_error(invalid, 422, "INVALID_RULES");
    EXPECT_NE(json::parse(invalid.body)["error"]["message"].get<std::string>().find(patch.begin().key()), std::string::npos);
    const auto created = write(*engine, "POST", "/api/accounts", {{"name", "Invalid restrictions"}, {"initial_cash", "100000"}, {"rules", bad}});
    expect_error(created, 422, "INVALID_RULES");
    test::capture_contract("time-rules", "POST", "/api/accounts", created);
  }
  auto funded = rules;
  funded.update({{"phase", "funded"}, {"profit_target", nullptr}, {"payouts", {{"qualifying_profit", "0"}, {"qualifying_days", 1},
      {"withdrawal_percent", 50}, {"split_percent", 80}, {"minimum", "0"}, {"caps", json::array()}}}});
  expect_error(reset(funded), 422, "INVALID_RULES");
  funded["time_limit_days"] = 0;
  ASSERT_EQ(reset(funded).status, 200);
  rules.update({{"time_limit_days", 0}, {"inactivity_days", 0}, {"underlyings", json::array()},
                {"trading_start", nullptr}, {"trading_end", nullptr}, {"flat_time", nullptr}, {"no_overnight", false}});
  response = reset(rules); ASSERT_EQ(response.status, 200) << response.body;
  account = json::parse(response.body);
  for (const auto* field : {"time_limit_days", "deadline", "days_left", "last_activity", "inactive_days", "inactivity_deadline"})
    EXPECT_TRUE(account["evaluation"][field].is_null()) << field;
}
TEST_F(PaperEngine, FlatTimeMarketTransactionClosesBeforeTheApiCanOpenAgain) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules.update({{"plan", "Flat clock"}, {"flat_time", "10:01"}});
  auto response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", rules}, {"reason", "F6"}});
  ASSERT_EQ(response.status, 200) << response.body;
  response = write(*engine, "POST", "/api/orders", order(market, "held", "4.20"));
  ASSERT_EQ(response.status, 201) << response.body;
  market.time += md::kNanosPerMinute;
  quote("3.90", "4.10");
  ASSERT_TRUE(wait_for([&] { return read(*engine, "/api/account")["evaluation"]["flat_now"] == true &&
      read(*engine, "/api/portfolio")["positions"].empty(); }));
  EXPECT_TRUE(read(*engine, "/api/portfolio")["positions"].empty());
  expect_error(write(*engine, "POST", "/api/orders", order(market, "too-late", "4.10")), 422, "FLAT_TIME");
}
TEST_F(PaperEngine, FlatRulesRoundTripValidationAndAccountProgress) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules.update({{"plan", "Mandatory flat"}, {"flat_time", "10:01"}, {"no_overnight", true}});
  const auto reset = [&](const json& r) {
    const auto response = write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", r}, {"reason", "F6"}});
    test::capture_contract("flat-rules", "POST", "/api/account/reset", response);
    return response;
  };
  auto response = reset(rules); ASSERT_EQ(response.status, 200) << response.body;
  auto account = json::parse(response.body);
  EXPECT_EQ(account["rules"], rules);
  EXPECT_EQ(account["evaluation"]["flat_time"], "10:01");
  EXPECT_EQ(account["evaluation"]["flat_now"], false);
  for (const auto& patch : std::vector<json>{{{"flat_time", "17:00"}}, {{"flat_time", "24:00"}},
      {{"flat_time", "09:60"}}, {{"flat_time", "9:30"}}, {{"flat_time", 600}},
      {{"flat_time", false}}, {{"no_overnight", nullptr}}, {{"no_overnight", 1}}, {{"no_overnight", "true"}},
      {{"flat_time", "18:00"}, {"day_end", "18:00"}}}) {
    auto bad = rules; bad.update(patch);
    response = reset(bad); expect_error(response, 422, "INVALID_RULES");
    EXPECT_EQ(json::parse(response.body)["error"]["message"], patch.contains("flat_time")
        ? "flat_time must be HH:MM New York time from 00:00 to 23:59, before day_end"
        : "no_overnight must be a boolean");
    const auto created = write(*engine, "POST", "/api/accounts", {{"name", "Invalid flat rule"}, {"initial_cash", "100000"}, {"rules", bad}});
    expect_error(created, 422, "INVALID_RULES");
    test::capture_contract("flat-rules", "POST", "/api/accounts", created);
  }
  rules["flat_time"] = "10:00";
  response = reset(rules); ASSERT_EQ(response.status, 200) << response.body;
  account = json::parse(response.body);
  EXPECT_EQ(account["evaluation"]["flat_now"], true);
  response = write(*engine, "POST", "/api/orders", order(market, "flat-time-refused"));
  expect_error(response, 422, "FLAT_TIME");
  const auto error = json::parse(response.body)["error"];
  EXPECT_EQ(error["actual"], "10:00"); EXPECT_EQ(error["limit"], "10:00"); EXPECT_EQ(error["scope"], "account");
  test::capture_contract("flat-rules", "POST", "/api/orders", response);
  response = write(*engine, "POST", "/api/orders/preview", order(market, "flat-preview"));
  expect_error(response, 422, "FLAT_TIME");
  EXPECT_EQ(json::parse(response.body)["error"]["actual"], "10:00");
  EXPECT_EQ(json::parse(response.body)["error"]["limit"], "10:00");
  EXPECT_EQ(json::parse(response.body)["error"]["scope"], "account");
  test::capture_contract("flat-rules", "POST", "/api/orders/preview", response);
  rules["flat_time"] = nullptr; rules["no_overnight"] = false;
  response = reset(rules); ASSERT_EQ(response.status, 200) << response.body;
  EXPECT_EQ(json::parse(response.body)["evaluation"]["flat_time"], nullptr);
  EXPECT_EQ(json::parse(response.body)["evaluation"]["flat_now"], false);
}
TEST(PaperStocks, FlatTimeRejectsOpeningPreviewsWith422) {
  PaperProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, paper_options());
  engine.start();
  ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  provider.sink->publish(md::UnderlyingQuote{"SPY", time, 519.9, 520.1, 520});
  ASSERT_TRUE(wait_for([&] { const auto view = engine.trading_view(); return view && view->market_times.contains("SPY"); }));
  auto rules = read(engine, "/api/account")["rules"];
  rules.update({{"plan", "Flat shares"}, {"flat_time", "10:00"}});
  const auto reset = write(engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", rules}, {"reason", "F6"}});
  ASSERT_EQ(reset.status, 200) << reset.body;
  for (const auto* path : {"/api/stocks/trade/preview", "/api/stocks/trade"}) {
    const auto response = write(engine, "POST", path, {{"symbol", "SPY"}, {"side", "buy"}, {"shares", 1}});
    ASSERT_EQ(response.status, 422) << response.body;
    const auto error = json::parse(response.body)["error"];
    EXPECT_EQ(error["code"], "FLAT_TIME"); EXPECT_EQ(error["actual"], "10:00");
    EXPECT_EQ(error["limit"], "10:00"); EXPECT_EQ(error["scope"], "account");
    test::capture_contract("flat-rules", "POST", path, response);
  }
  engine.stop();
}

TEST_F(PaperEngine, EventCalendarRulesRoundTripAndValidation) {
  seed();
  auto rules = read(*engine, "/api/account")["rules"];
  rules.update({{"plan", "Event plan"}, {"news_before_minutes", 5}, {"news_after_minutes", 5}, {"news_action", "flatten"},
      {"hold_restrictions", {"weekend", "split", "weekend"}}, {"hold_cutoff", "15:40"},
      {"events", {{{"kind", "earnings"}, {"time", "2026-10-20"}, {"symbol", "SPY"}},
                  {{"kind", "news"}, {"time", "2026-09-22T14:00:00Z"}, {"label", "CPI"}}}}});
  const auto reset = [&](const json& r) {
    return write(*engine, "POST", "/api/account/reset", {{"initial_cash", "100000"}, {"rules", r}, {"reason", "F17 F59 calendar"}});
  };
  const auto response = reset(rules); ASSERT_EQ(response.status, 200) << response.body;
  const auto account = json::parse(response.body);
  EXPECT_EQ(account["rules"]["hold_restrictions"], json({"split", "weekend"}));
  EXPECT_EQ(account["rules"]["events"][0]["kind"], "news");
  EXPECT_EQ(account["rules"]["events"][0]["time"], "2026-09-22T14:00:00Z");
  EXPECT_EQ(account["rules"]["events"][1]["session"], "before_open");
  EXPECT_EQ(account["evaluation"]["next_event"]["kind"], "news");
  EXPECT_TRUE(account["evaluation"]["next_event"]["active"]);
  expect_error(write(*engine, "POST", "/api/orders/preview", order(market, "news-preview")), 422, "NEWS_BLACKOUT");
  const auto blocked = write(*engine, "POST", "/api/orders", order(market, "news-denied"));
  expect_error(blocked, 422, "NEWS_BLACKOUT");
  const auto evidence = json::parse(blocked.body)["error"];
  EXPECT_EQ(evidence["scope"], "account"); EXPECT_TRUE(evidence["actual"].is_string());
  EXPECT_EQ(evidence["limit"], "2026-09-22T14:05:00.000Z");
  const auto again = reset(account["rules"]); ASSERT_EQ(again.status, 200) << again.body;
  EXPECT_EQ(json::parse(again.body)["rules"], account["rules"]);
  for (const auto& patch : std::vector<json>{
      {{"news_before_minutes", -1}}, {{"news_after_minutes", 241}}, {{"news_before_minutes", 1.5}},
      {{"news_action", "ignore"}}, {{"hold_restrictions", {"overnight"}}}, {{"hold_cutoff", "17:00"}},
      {{"hold_cutoff", "15:99"}}, {{"events", "bad"}},
      {{"events", {{{"kind", "news"}, {"time", "2026-09-22"}}}}},
      {{"events", {{{"kind", "split"}, {"time", "2026-09-22"}}}}},
      {{"events", {{{"kind", "earnings"}, {"time", "2026-02-30"}, {"symbol", "SPY"}}}}},
      {{"events", {{{"kind", "news"}, {"time", "2026-09-22T14:00:00Z"}, {"symbol", "spy"}}}}},
      {{"events", {{{"kind", "split"}, {"time", "2026-09-22"}, {"symbol", "SPY"}, {"session", "before_open"}}}}},
      {{"events", {{{"kind", "news"}, {"time", "2026-09-22T14:00:00Z"}, {"label", std::string(65, 'x')}}}}}}) {
    auto invalid = rules; invalid.update(patch);
    expect_error(reset(invalid), 422, "INVALID_RULES");
    expect_error(write(*engine, "POST", "/api/accounts", {{"name", "Bad calendar"}, {"initial_cash", "100000"}, {"rules", invalid}}), 422, "INVALID_RULES");
  }
}
TEST(EventCalendarApi, CsvAndFilteredReadRoute) {
  std::istringstream input("# supplied events\nkind,time,symbol,session,label\nnews,2026-09-22T14:00:00Z,,,CPI\nearnings,2026-09-23,SPY,,Earnings\nnews,2026-09-22T14:00:00Z,,,CPI\n");
  const auto events = trading::parse_event_calendar(input);
  ASSERT_EQ(events.size(), 2U); EXPECT_EQ(events[1].session, "before_open");
  const auto all = server::event_calendar_read({"GET", "/api/calendar/events"}, events);
  ASSERT_TRUE(all); EXPECT_EQ(all->status, 200); EXPECT_EQ(json::parse(all->body)["events"].size(), 2U);
  const auto range = server::event_calendar_read({"GET", "/api/calendar/events?from=2026-09-23&to=2026-09-24"}, events);
  ASSERT_TRUE(range); ASSERT_EQ(range->status, 200); EXPECT_EQ(json::parse(range->body)["events"].size(), 1U);
  for (const auto* query : {"from=bad", "from=2026-09-24&to=2026-09-23", "x=1", "from=2026-09-23&from=2026-09-24"}) {
    const auto response = server::event_calendar_read({"GET", std::string("/api/calendar/events?") + query}, events);
    ASSERT_TRUE(response); EXPECT_EQ(response->status, 400);
  }
  std::istringstream bad("# comment\nkind,time,symbol,session,label\nsplit,2026-02-30,SPY,,bad\n");
  try { (void)trading::parse_event_calendar(bad); FAIL() << "expected line error"; }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("line 3"), std::string::npos); }
}
}  // namespace

TEST_F(PaperEngine, TradeConsistencyCustomRulesRoundTrip) {
  for (const auto phase : {"evaluation", "verification"}) {
    SCOPED_TRACE(phase);
    seed();
    auto rules = read(*engine, "/api/account")["rules"];
    rules["phase"] = phase;
    rules["plan"] = "Trade consistency"; rules["trade_consistency_percent"] = 40; rules["min_trades"] = 2;
    rules["microscalp_seconds"] = 60; rules["microscalp_percent"] = 25;
    const auto reset = [&](const json& r) { return write(*engine, "POST", "/api/account/reset",
        {{"reason", "F29"}, {"initial_cash", "100000"}, {"rules", r}}); };
    ASSERT_EQ(reset(rules).status, 200);
    const auto account = read(*engine, "/api/account");
    EXPECT_EQ(account["rules"]["min_trades"], 2);
    EXPECT_EQ(account["evaluation"]["closed_trades"], 0);
    EXPECT_EQ(account["evaluation"]["short_profit"], "0.00");
    EXPECT_EQ(account["evaluation"]["objectives"].size(), 3U);
    EXPECT_EQ(account["rules"]["trade_consistency_percent"], 40);
    EXPECT_EQ(account["evaluation"]["best_trade"], nullptr);
    EXPECT_EQ(account["evaluation"]["objectives"].back()["code"], "TRADE_CONSISTENCY");
    rules["trade_consistency_percent"] = 101;
    expect_error(reset(rules), 400, "INVALID_RULES");
    rules["trade_consistency_percent"] = 1.5;
    expect_error(reset(rules), 400, "INVALID_REQUEST");
  }
}

TEST_F(PaperEngine, HoldingRulesRoundTripAndRefuseWithEvidence) {
  for (const auto phase : {"evaluation", "verification"}) {
    SCOPED_TRACE(phase);
    seed();
    auto rules = read(*engine, "/api/account")["rules"];
    rules["phase"] = phase;
    rules["plan"] = "Holding rules"; rules["min_hold_seconds"] = 60;
    rules["microscalp_seconds"] = 30; rules["microscalp_percent"] = 25;
    const auto reset = [&](const json& r) { return write(*engine, "POST", "/api/account/reset",
        {{"reason", "F61"}, {"initial_cash", "100000"}, {"rules", r}}); };
    ASSERT_EQ(reset(rules).status, 200);
    const auto account = read(*engine, "/api/account");
    EXPECT_EQ(account["rules"]["min_hold_seconds"], 60);
    EXPECT_EQ(account["rules"]["microscalp_seconds"], 30);
    EXPECT_EQ(account["rules"]["microscalp_percent"], 25);
    EXPECT_EQ(account["evaluation"]["short_profit"], "0.00");
    ASSERT_EQ(write(*engine, "POST", "/api/orders", order(market, "open-held", "4.20")).status, 201);
    auto close = order(market, "young-close", "4.00"); close["side"] = "sell";
    const auto preview = write(*engine, "POST", "/api/orders/preview", close);
    ASSERT_EQ(preview.status, 200) << preview.body;
    const auto p = json::parse(preview.body);
    EXPECT_EQ(p["reason"]["code"], "MIN_HOLD");
    EXPECT_EQ(p["reason"]["message"], "Held 0 seconds; this plan requires 60 seconds before a user reduction");
    EXPECT_EQ(p["reason"]["actual"], 0);
    EXPECT_EQ(p["reason"]["limit"], 60);
    const auto refused = write(*engine, "POST", "/api/orders", close);
    expect_error(refused, 422, "MIN_HOLD");
    EXPECT_EQ(json::parse(refused.body)["error"]["scope"], market.contract.underlying);
    for (const auto* key : {"min_hold_seconds", "microscalp_seconds", "microscalp_percent", "min_trades", "trade_consistency_percent"}) {
      auto invalid = rules; invalid[key] = nullptr;
      expect_error(reset(invalid), 400, "INVALID_REQUEST");
    }
    auto unpaired = rules; unpaired.erase("microscalp_percent");
    expect_error(reset(unpaired), 400, "INVALID_RULES");
    auto disabled = rules; disabled["microscalp_percent"] = 0;
    ASSERT_EQ(reset(disabled).status, 200);
    rules["microscalp_seconds"] = 0;
    expect_error(reset(rules), 400, "INVALID_RULES");
    rules["microscalp_seconds"] = 30; rules["min_hold_seconds"] = 1.5;
    expect_error(reset(rules), 400, "INVALID_REQUEST");
  }
}

TEST(PaperAccounts, CreationAcceptsTradeAndHoldingObjectives) {
  const auto directory = paper_path().parent_path();
  auto options = paper_options();
  options.paper_journal = directory / "paper.jsonl";
  options.paper_accounts = directory / "accounts";
  {
    PaperProvider provider;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_TRUE(wait_for([&] { return engine.trading_view() != nullptr; }));
    json rules{{"profit_target", "100"}, {"max_drawdown", nullptr}, {"drawdown_mode", "intraday"},
               {"buy_only", false}, {"buying_power", true}, {"expiry_cutoff_seconds", 0},
               {"trade_consistency_percent", 40}, {"min_trades", 10}, {"min_hold_seconds", 60},
               {"microscalp_seconds", 30}, {"microscalp_percent", 25}};
    const auto created = write(engine, "POST", "/api/accounts", {{"name", "Holding rules"}, {"initial_cash", "100000"}, {"rules", rules}});
    ASSERT_EQ(created.status, 201) << created.body;
    const auto id = json::parse(created.body)["account"]["id"].get<std::string>();
    const auto wire = read(engine, "/api/account?account=" + id)["rules"];
    for (const auto* key : {"trade_consistency_percent", "min_trades", "min_hold_seconds", "microscalp_seconds", "microscalp_percent"})
      EXPECT_EQ(wire[key], rules[key]);
    engine.stop();
  }
  std::filesystem::remove_all(directory);
}

TEST(PaperPlans, EventTimesTrimOnlyInsignificantZerosAndPreserveJournalTerms) {
  for (const auto& fraction : {std::string(""), std::string(".1"), std::string(".123456789")}) {
    const auto text = "2026-09-23T14:00:00" + fraction + "Z";
    const auto parsed = server::parse_calendar_events(json::array({{{"kind", "news"}, {"time", text}}}));
    ASSERT_EQ(parsed.size(), 1U);
    // The same nine-digit canonical form is retained in old and new journals.
    EXPECT_EQ(parsed.front().time.size(), 30U);
    EXPECT_EQ(server::calendar_event_json(parsed.front())["time"], text);
    EXPECT_EQ(server::parse_calendar_events(server::calendar_events_json(parsed)), parsed);
  }
  const trading::PlanEvent old{"news", "2026-09-23T14:00:00.120000000Z", {}, {}, {}};
  EXPECT_EQ(server::calendar_event_json(old)["time"], "2026-09-23T14:00:00.12Z");
  const trading::PlanEvent date{"earnings", "2026-09-23", "SPY", "before_open", {}};
  EXPECT_EQ(server::calendar_event_json(date)["time"], "2026-09-23");
}
