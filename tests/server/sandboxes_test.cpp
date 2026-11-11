#include <gtest/gtest.h>
#include <fstream>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "openport/server/notifications.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/web_server.hpp"
#include "openport/server/web_policy.hpp"
#include "openport/server/plans.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using namespace std::chrono_literals;

class SandboxSource final : public server::MetricsSource {
 public:
  explicit SandboxSource(server::Desk::Options options) : options_(std::move(options)) { restart(); }
  void restart() {
    desk.reset();
    desk = std::make_unique<server::Desk>("demo", md::Capabilities{}, md::Subscription{{"SPX"}}, options_);
    desk->start_trading();
  }
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override {
    server::EngineStatus result;
    result.provider = "demo";
    result.accounts = desk->accounts();
    result.trading = desk->trading_status();
    return result;
  }
  server::Sandboxes* sandboxes() const override { return options_.sandboxes.get(); }
  using MetricsSource::trading_view;
  std::shared_ptr<const server::TradingView> trading_view(std::string_view account) const override {
    return desk->trading_view(account);
  }
  bool post_trading(server::TradingCommand command, server::TradingCompletion complete) override {
    if (inbox_full) return false;
    if (hold) { queued = std::move(command); pending = std::move(complete); return true; }
    desk->command(std::move(command), std::move(complete), market.time, market.time);
    return true;
  }
  test::ScriptedMarket market;
  std::unique_ptr<server::Desk> desk;
  bool hold = false;
  bool inbox_full = false;
  server::TradingCommand queued;
  server::TradingCompletion pending;
 private:
  server::Desk::Options options_;
};
class Sandboxes : public testing::Test {
 protected:
  test::RecordingFile files;
  server::Sandboxes::Clock::time_point now{};
  server::Sandboxes::Options limits;
  server::Desk::Options desk_options;
  std::shared_ptr<server::Sandboxes> manager;
  std::unique_ptr<SandboxSource> source;
  server::WritePolicy policy;
  bool fail_sync = false;
  void SetUp() override {
    limits.capacity = 10;
    limits.clock = [&] { return now; };
    desk_options.analytics.fallback_rate = 0;
    desk_options.paper_journal = files.directory / "demo.jsonl";
    desk_options.paper_accounts = files.directory / "accounts";
    desk_options.write_mode = "token";
    desk_options.journal_io.sync = [&](int) { return !fail_sync; };
    configure();
  }
  void configure() {
    source.reset();
    manager = std::make_shared<server::Sandboxes>(limits);
    desk_options.sandboxes = manager;
    source = std::make_unique<SandboxSource>(desk_options);
    policy = {"0.0.0.0", "operator", {}, {}, false, manager, {}};
  }
  server::ApiRequest request(std::string method, std::string target, const std::string& token = "", std::string body = "{}") {
    server::ApiRequest result{std::move(method), std::move(target), std::move(body)};
    result.host = "localhost";
    result.content_type = "application/json";
    if (!token.empty()) result.authorization = "Bearer " + token;
    result.client_ip = "192.0.2.1";
    if (result.method == "GET" || result.method == "DELETE") result.body.clear();
    return result;
  }
  server::ApiResponse send(server::ApiRequest input) {
    if (auto rejected = server::check_api_write(input, policy, &input.actor, &input.access)) return *rejected;
    std::optional<server::ApiResponse> response;
    server::handle_api_async(input, *source, [&](auto value) { response = std::move(value); });
    if (!response) throw std::runtime_error("Missing sandbox response");
    return *response;
  }
  json create(std::string client = "192.0.2.1") {
    auto input = request("POST", "/api/sandboxes");
    input.client_ip = std::move(client);
    const auto response = send(input);
    EXPECT_EQ(response.status, 201) << response.body;
    return json::parse(response.body);
  }
  static std::string token(const json& created) { return created.at("token").get<std::string>(); }
  static std::string id(const json& created) { return created.at("account").get<std::string>(); }
};

TEST_F(Sandboxes, CreatesPracticeAccountAndReturnsSecretOnlyOnce) {
  const auto first = create(), second = create();
  EXPECT_NE(id(first), id(second));
  EXPECT_NE(token(first), token(second));
  EXPECT_EQ(token(first).size(), 8U + 32U);
  EXPECT_TRUE(first.at("simulated"));
  EXPECT_EQ(first.at("idle_seconds"), 86400);
  const auto view = source->trading_view(id(first));
  ASSERT_TRUE(view);
  EXPECT_EQ(view->config.initial_cash, server::find_plan("practice")->initial_cash);
  EXPECT_EQ(view->config.rules.plan, server::find_plan("practice")->rules.plan);
  const auto journal = files.directory / "sandboxes" / id(first) / (id(first) + ".jsonl");
  EXPECT_TRUE(std::filesystem::exists(journal));
  const auto records = trading::FileJournal::read(journal.string());
  EXPECT_FALSE(records.records.empty());
  EXPECT_NO_THROW(trading::TradingSession::recover(records));
  for (const auto& record : records.records) EXPECT_EQ(record.payload.find(token(first)), std::string::npos);
  for (const auto& path : std::vector<std::string>{"/api/status", "/api/accounts", "/api/account?account=" + id(first)}) {
    const auto response = send(request("GET", path, token(first)));
    EXPECT_EQ(response.status, 200) << response.body;
    EXPECT_EQ(response.body.find(token(first)), std::string::npos);
  }
}
TEST_F(Sandboxes, OperatorCannotRenameArchiveOrDeleteSandboxAccounts) {
  const auto created = create();
  const auto account_id = id(created);
  for (const auto* body : {R"({"name":"renamed"})", R"({"archived":true})"}) {
    const auto result = send(request("PATCH", "/api/accounts/" + account_id, "operator", body));
    EXPECT_EQ(result.status, 403) << result.body;
    EXPECT_EQ(json::parse(result.body).at("error").at("code"), "ACCOUNT_PROTECTED");
  }
  const auto removed = send(request("DELETE", "/api/accounts/" + account_id, "operator"));
  EXPECT_EQ(removed.status, 403);
  EXPECT_EQ(json::parse(removed.body).at("error").at("code"), "ACCOUNT_PROTECTED");
}

TEST_F(Sandboxes, NewSandboxesChargeTheOperatorsFee) {
  desk_options.paper.fee_per_contract = trading::Money::parse("1.00");
  configure();
  const auto own = create();
  const auto view = source->trading_view(id(own));
  ASSERT_TRUE(view);
  EXPECT_EQ(view->config.fee_per_contract, trading::Money::parse("1.00"));
  EXPECT_EQ(view->config.initial_cash, server::find_plan("practice")->initial_cash);
  EXPECT_EQ(view->config.rules.plan, server::find_plan("practice")->rules.plan);
  // The visitor's own status reports the fee their orders pay.
  const auto status = json::parse(send(request("GET", "/api/status", token(own))).body);
  EXPECT_EQ(status["trading"]["fee_per_contract"], "1.00");
  // Its trading is its own account's, as the operator's listing shows it. The operator's
  // notification settings (channels, their errors) stay in the operator's status only.
  const auto operator_status = json::parse(send(request("GET", "/api/status", "operator")).body);
  json own_trading;
  for (const auto& account : operator_status.at("accounts")) {
    if (account.at("id") == id(own)) own_trading = account.at("trading");
  }
  EXPECT_EQ(status.at("trading"), own_trading);
  EXPECT_FALSE(status.contains("notifications"));
  EXPECT_TRUE(operator_status.contains("notifications"));
}
TEST_F(Sandboxes, TradesOnSharedSimulatedFeedWithoutChangingTheMainAccount) {
  const auto& market = source->market;
  source->desk->replay_batch({md::ContractDefinition{0, market.contract},
      md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000},
      md::OptionQuote{0, market.time, 4.0, 4.2, 20, 20},
      md::SnapshotComplete{"SPX", market.time}}, market.time);
  const auto own = create();
  const auto main_before = source->trading_view("main")->snapshot;
  const json order{{"client_order_id", "sandbox-order"}, {"symbol", market.symbol()},
      {"side", "buy"}, {"type", "limit"}, {"quantity", 1}, {"limit_price", "4.20"}, {"time_in_force", "day"}};
  const auto response = send(request("POST", "/api/orders?account=" + id(own), token(own), order.dump()));
  ASSERT_EQ(response.status, 201) << response.body;
  const auto body = json::parse(response.body);
  EXPECT_EQ(body["order"]["actor"], id(own));
  EXPECT_EQ(body["order"]["filled_quantity"], 1);
  EXPECT_EQ(source->trading_view("main")->snapshot->account.cash, main_before->account.cash);
  EXPECT_TRUE(source->trading_view("main")->snapshot->recent_orders.empty());
  EXPECT_EQ(source->trading_view(id(own))->snapshot->positions.size(), 1U);
}
TEST_F(Sandboxes, CapacityIncludesPendingCreations) {
  limits.capacity = 1;
  configure();
  source->hold = true;
  auto input = request("POST", "/api/sandboxes");
  std::optional<server::ApiResponse> response;
  manager->create(input, *source, [&](auto value) { response = std::move(value); });
  EXPECT_FALSE(response);
  const auto refused = send(input);
  EXPECT_EQ(refused.status, 429);
  EXPECT_EQ(json::parse(refused.body)["error"]["code"], "SANDBOX_CAPACITY");
  EXPECT_EQ(refused.retry_after, 0);
  source->hold = false;
  source->post_trading(std::move(source->queued), std::move(source->pending));
  ASSERT_TRUE(response);
  EXPECT_EQ(response->status, 201);
}
TEST_F(Sandboxes, CreationRateRetryAfterUsesOldestRequestInEachBucket) {
  for (const bool global : {false, true}) {
    SCOPED_TRACE(global ? "global" : "client");
    limits.global = global ? 2 : 30;
    limits.per_client = global ? 3 : 2;
    // Left unused past the hour, the client's sandboxes stop counting as held.
    limits.idle = 30min;
    now = {};
    configure();
    if (!global) {
      create("192.0.2.2");
      now += 20s;
    }
    const auto start = now;
    const auto market_time = source->desk->market_time();
    create();
    now += 10s;
    create(global ? "192.0.2.2" : "192.0.2.1");
    auto input = request("POST", "/api/sandboxes");
    if (global) input.client_ip = "192.0.2.3";
    for (const auto elapsed : {10250ms, 3599500ms}) {
      now = start + elapsed;
      const auto response = send(input);
      EXPECT_EQ(response.status, 429);
      EXPECT_EQ(json::parse(response.body)["error"]["code"],
                global ? "SANDBOX_GLOBAL_RATE" : "SANDBOX_CLIENT_RATE");
      EXPECT_EQ(response.retry_after, elapsed == 10250ms ? 3590 : 1);
    }
    now = start + limits.creation_window;
    EXPECT_EQ(send(input).status, 201);
    const auto response = send(input);
    EXPECT_EQ(response.status, 429);
    EXPECT_EQ(response.retry_after, 10);
    if (!global) { create("192.0.2.2"); }
    EXPECT_EQ(source->desk->market_time(), market_time);
  }
}
// The per-client limit counted creations in the last hour, not sandboxes held: one
// address creating 3 an hour and using each within the idle period took all 20 slots
// of the public demo after seven hours and kept them from every other visitor.
TEST_F(Sandboxes, AClientHoldsAtMostItsLimitOfLiveSandboxes) {
  limits.capacity = 20;
  limits.idle = 1h;
  configure();
  std::vector<std::string> tokens;
  for (int hour = 0; hour < 7; ++hour) {
    for (int i = 0; i < 3; ++i) {
      auto input = request("POST", "/api/sandboxes");
      input.client_ip = "203.0.113.5";
      const auto response = send(input);
      if (response.status == 201) tokens.push_back(token(json::parse(response.body)));
      else {
        EXPECT_EQ(response.status, 429);
        EXPECT_EQ(json::parse(response.body)["error"]["code"], "SANDBOX_CLIENT_RATE");
      }
    }
    for (int half = 0; half < 2; ++half) {
      now += 30min;
      for (const auto& secret : tokens) EXPECT_TRUE(manager->authenticate(secret));
    }
  }
  EXPECT_EQ(tokens.size(), 3U);
  const auto other = create("198.51.100.7");
  EXPECT_EQ(other.at("simulated"), true);
  // A sandbox left idle until it expires frees one of the client's places.
  now += 2h;
  for (const auto& secret : tokens) EXPECT_FALSE(manager->authenticate(secret));
  create("203.0.113.5");
}
TEST_F(Sandboxes, ClientHeaderOnlySeparatesVisitorsWhenConfigured) {
  limits.per_client = 1;
  configure();
  const auto proxy = "10.0.0.1";
  create(server::sandbox_client_ip(proxy, "", "192.0.2.1"));
  auto input = request("POST", "/api/sandboxes");
  input.client_ip = server::sandbox_client_ip(proxy, "", "192.0.2.2");
  EXPECT_EQ(send(input).status, 429);
  create(server::sandbox_client_ip(proxy, "X-Real-IP", "192.0.2.1"));
  create(server::sandbox_client_ip(proxy, "X-Real-IP", "192.0.2.2"));
  EXPECT_EQ(server::sandbox_client_ip(proxy, "X-Real-IP", "192.0.2.4, 10.0.0.2"), proxy);
  EXPECT_EQ(server::sandbox_client_ip(proxy, "X-Real-IP", "bad"), proxy);
  EXPECT_EQ(server::sandbox_client_ip(proxy, "X-Real-IP", "192.0.2.4", true), proxy);
  EXPECT_EQ(server::sandbox_client_ip(proxy, "X-Real-IP", "2001:0db8::1"), "2001:db8::1");
}
TEST_F(Sandboxes, ScopeCannotReadOrWriteMainOrAnotherSandbox) {
  const auto& market = source->market;
  source->desk->replay_batch({md::ContractDefinition{0, market.contract},
      md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000},
      md::OptionQuote{0, market.time, 4.0, 4.2, 20, 20},
      md::SnapshotComplete{"SPX", market.time}}, market.time);
  const auto own = create(), other = create();
  for (const auto& account : {std::string("main"), id(other)}) {
    for (const auto& method : {"GET", "POST", "PUT", "DELETE"}) {
      auto input = request(method, "/api/orders?account=" + account, token(own));
      const auto rejected = server::check_api_write(input, policy);
      ASSERT_TRUE(rejected) << method << account;
      EXPECT_EQ(rejected->status, 403);
    }
  }
  EXPECT_EQ(send(request("GET", "/api/account", token(own))).status, 200);
  EXPECT_EQ(send(request("GET", "/api/account?account=" + id(own), token(own))).status, 200);
  // An order any of the accounts would take. With the account key given twice, the
  // scope check and the route could each read a different one: it is refused before
  // either does, whichever comes first, and no account gets the order.
  const json order{{"client_order_id", "scoped"}, {"symbol", market.symbol()}, {"side", "buy"}, {"type", "limit"},
      {"quantity", 1}, {"limit_price", "4.20"}, {"time_in_force", "day"}};
  const auto version = [&](const std::string& account) { return source->trading_view(account)->snapshot->account_version; };
  const auto orders = [&](const std::string& account) { return source->trading_view(account)->snapshot->recent_orders.size(); };
  const std::vector<std::string> accounts{"main", id(own), id(other)};
  std::vector<std::uint64_t> versions;
  for (const auto& account : accounts) versions.push_back(version(account));
  for (const auto& query : {"?account=main&account=" + id(own), "?account=" + id(own) + "&account=main"}) {
    const auto refused = send(request("POST", "/api/orders" + query, token(own), order.dump()));
    EXPECT_EQ(refused.status, 400) << query << refused.body;
    EXPECT_EQ(json::parse(refused.body)["error"]["code"], "INVALID_REQUEST") << query;
  }
  for (std::size_t i = 0; i < accounts.size(); ++i) {
    EXPECT_EQ(version(accounts[i]), versions[i]) << accounts[i];
    EXPECT_EQ(orders(accounts[i]), 0U) << accounts[i];
  }
  // %61ccount is the account key spelt another way: it names the sandbox itself.
  const auto placed = send(request("POST", "/api/orders?%61ccount=" + id(own), token(own), order.dump()));
  ASSERT_EQ(placed.status, 201) << placed.body;
  EXPECT_EQ(orders(id(own)), 1U);
  EXPECT_EQ(orders("main"), 0U);
  EXPECT_EQ(orders(id(other)), 0U);
  EXPECT_EQ(version("main"), versions[0]);
  EXPECT_EQ(version(id(other)), versions[2]);
}
TEST_F(Sandboxes, OmittedAccountUsesOwnSandboxOnReadsAndWrites) {
  const auto own = create();
  for (const auto* path : {"/api/account", "/api/account/equity", "/api/portfolio", "/api/orders", "/api/fills",
                          "/api/settlements", "/api/risk", "/api/risk/profile", "/api/trades", "/api/trades.csv",
                          "/api/fills.csv", "/api/playbooks", "/api/account/pass-odds", "/api/alerts"}) {
    const auto implicit = send(request("GET", path, token(own)));
    const auto explicit_account = send(request("GET", std::string(path) + "?account=" + id(own), token(own)));
    EXPECT_EQ(implicit.status, explicit_account.status) << path;
    EXPECT_EQ(implicit.body, explicit_account.body) << path;
    EXPECT_NE(implicit.status, 403) << path;
  }
  const auto own_version = source->desk->trading_view(id(own))->snapshot->account_version;
  const auto main_version = source->desk->trading_view("main")->snapshot->account_version;
  const auto result = send(request("PUT", "/api/days/2026-09-22/note", token(own), R"({"plan":"Practice","review":"Own account"})"));
  EXPECT_EQ(result.status, 200) << result.body;
  EXPECT_EQ(source->desk->trading_view(id(own))->snapshot->account_version, own_version + 1);
  EXPECT_EQ(source->desk->trading_view("main")->snapshot->account_version, main_version);
  for (const auto& [query, expected] : std::vector<std::pair<std::string, int>>{
      {"?status=open", 200}, {"?status=open&%61ccount=main", 403},
      {"?account=", 400}, {"?account=main&%61ccount=main", 400}}) {
    const auto response = send(request("GET", "/api/orders" + query, token(own)));
    EXPECT_EQ(response.status, expected) << query;
  }
}

TEST_F(Sandboxes, SandboxCannotUseAnyAdministrativeRoute) {
  const auto own = create();
  const std::vector<std::pair<std::string, std::string>> routes{
    {"PUT", "/api/risk/limits"}, {"PUT", "/api/risk/guardrails"}, {"POST", "/api/risk/kill"},
    {"POST", "/api/account/reset"}, {"POST", "/api/account/payout"}, {"POST", "/api/settlements"},
    {"POST", "/api/accounts"}, {"POST", "/api/playbooks"}, {"PUT", "/api/playbooks/setup/mode"},
    {"POST", "/api/playbooks/staged/setup/send"}, {"DELETE", "/api/playbooks/setup"},
    {"POST", "/api/replay"}, {"PUT", "/api/replay"}, {"DELETE", "/api/replay/history/test"},
    {"POST", "/api/backtests"}, {"DELETE", "/api/backtests/test"}, {"POST", "/api/notifications/test"},
    {"PUT", "/api/notifications/channels/test"}, {"POST", "/api/future-admin-route"},
    {"GET", "/api/replay"}, {"GET", "/api/backtests"}, {"GET", "/api/replay/account"},
  };
  for (const auto& [method, path] : routes) {
    const auto rejected = server::check_api_write(request(method, path + "?account=" + id(own), token(own)), policy);
    ASSERT_TRUE(rejected) << path;
    EXPECT_EQ(rejected->status, 403) << path;
  }
  // Fill calibration searches the server's recorded days, which a visitor has no part in.
  const auto calibration = server::check_api_write(request("POST", "/api/fills/calibration", token(own)), policy);
  ASSERT_TRUE(calibration);
  EXPECT_EQ(calibration->status, 403);
  EXPECT_EQ(json::parse(calibration->body)["error"]["code"], "SCOPE_REQUIRED");
}
TEST_F(Sandboxes, TradeScopeAllowsOwnOrdersPreviewsExitsAndNotes) {
  const auto own = create();
  for (const auto& [method, path] : std::vector<std::pair<std::string, std::string>>{
      {"POST", "/api/orders"}, {"POST", "/api/orders/preview"}, {"PUT", "/api/orders/1"},
      {"DELETE", "/api/orders/1"}, {"POST", "/api/orders/cancel"}, {"POST", "/api/positions/close"},
      {"POST", "/api/positions/exercise"}, {"POST", "/api/stocks/close"}, {"POST", "/api/stocks/trade"}, {"POST", "/api/stocks/trade/preview"}, {"POST", "/api/positions/abandon"},
      {"POST", "/api/positions/instruction"},
      {"PUT", "/api/trades/1/note"}, {"PUT", "/api/days/2026-09-25/note"}}) {
    EXPECT_FALSE(server::check_api_write(request(method, path + "?account=" + id(own), token(own)), policy)) << path;
    EXPECT_FALSE(server::check_api_write(request(method, path, token(own)), policy)) << path;
  }
  EXPECT_FALSE(server::check_api_write(request("GET", "/api/underlyings/SPX/chain", token(own)), policy));
  EXPECT_FALSE(server::check_api_write(request("GET", "/api/volatility/scanner", token(own)), policy));
  EXPECT_FALSE(server::check_api_write(request("GET", "/api/underlyings/SPX/volatility", token(own)), policy));
  EXPECT_TRUE(server::check_api_write(request("GET", "/api/underlyings/SPX/volatility?account=main", token(own)), policy));
  EXPECT_FALSE(server::check_api_write(request("GET", "/ws", token(own)), policy));
  EXPECT_FALSE(server::check_api_write(request("GET", "/ws/events?version=1", token(own)), policy));
  const auto denied = server::check_api_write(request("GET", "/ws/events?version=1&account=main", token(own)), policy);
  ASSERT_TRUE(denied); EXPECT_EQ(denied->status, 403);
}
TEST_F(Sandboxes, ListingsStatusTicksAndGuessedIdsRespectVisibility) {
  const auto own = create(), other = create();
  policy.tokens.push_back({"reader", {"read", "trade:*"}, "reader-secret"});
  for (const auto& secret : {std::string{}, std::string("reader-secret"), token(own), std::string("operator")}) {
    for (const auto& path : {"/api/status", "/api/accounts"}) {
      const auto response = send(request("GET", path, secret));
      ASSERT_EQ(response.status, 200);
      const auto body = json::parse(response.body);
      const auto count = secret == "operator" ? 3U : 1U;
      EXPECT_EQ(body["accounts"].size(), count);
      if (secret == token(own)) {
        EXPECT_EQ(body["accounts"][0]["id"], id(own));
        EXPECT_EQ(response.body.find(id(other)), std::string::npos);
      } else if (secret != "operator") {
        EXPECT_EQ(response.body.find(id(own)), std::string::npos);
      }
    }
  }
  EXPECT_EQ(send(request("GET", "/api/account?account=" + id(own))).status, 404);
  EXPECT_EQ(send(request("GET", "/api/playbooks?account=" + id(own))).status, 404);
  EXPECT_EQ(send(request("POST", "/api/orders?account=" + id(own), "reader-secret")).status, 404);
  EXPECT_EQ(server::tick_message(*source).find(id(own)), std::string::npos);
  const auto raw = server::tick_message(*source, {true, {}});
  EXPECT_NE(raw.find(id(other)), std::string::npos);
  const auto private_tick = server::sandbox_tick(raw, {false, id(own)});
  EXPECT_NE(private_tick.find(id(own)), std::string::npos);
  EXPECT_EQ(private_tick.find(id(other)), std::string::npos);
  EXPECT_EQ(json::parse(send(request("GET", "/api/status")).body)["sandboxes"]["enabled"], true);
}
TEST_F(Sandboxes, EventStreamOpensOnlyForItsOwnTokenOrAnAdmin) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  namespace websocket = beast::websocket;
  using Socket = websocket::stream<asio::ip::tcp::socket>;
  const auto own = create(), other = create();
  server::Notifications::Options options;
  options.worker = false;
  auto events = std::make_shared<server::Notifications>(server::NotificationConfig{}, nullptr, options);
  server::WebServer web("127.0.0.1", 0, {},
      [&](const server::ApiRequest& input, server::ApiCompletion complete) {
        server::handle_api_async(input, *source, std::move(complete));
      }, {}, "operator", {}, {{"reader", {"read", "trade:*"}, "reader-secret"}}, false, manager);
  web.set_event_stream(events);
  web.start(1);
  asio::io_context io;
  const auto open = [&](Socket& ws, const std::string& query, const std::string& secret) {
    ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
    if (!secret.empty())
      ws.set_option(websocket::stream_base::decorator([secret](websocket::request_type& upgrade) {
        upgrade.set(http::field::authorization, "Bearer " + secret);
      }));
    websocket::response_type response;
    beast::error_code error;
    ws.handshake(response, "localhost", "/ws/events?version=1&" + query, error);
    return response;
  };
  const auto next = [](Socket& ws) {
    beast::flat_buffer buffer;
    ws.read(buffer);
    return json::parse(beast::buffers_to_string(buffer.data()));
  };
  // Like its REST routes, a sandbox's stream is unknown to a client without a token,
  // to a token without admin scope, and however its key or ID is spelled.
  for (const auto& [query, secret] : std::vector<std::pair<std::string, std::string>>{
           {"account=" + id(own), ""}, {"account=" + id(own), "reader-secret"},
           {"%61ccount=" + id(own), ""}, {"account=%73" + id(own).substr(1), ""},
           {"account=" + id(own) + "&source=replay", ""}}) {
    Socket refused(io);
    const auto response = open(refused, query, secret);
    EXPECT_EQ(response.result(), http::status::not_found) << query << " " << secret;
    EXPECT_NE(response.body().find("UNKNOWN_ACCOUNT"), std::string::npos) << response.body();
  }
  Socket stranger(io);
  EXPECT_EQ(open(stranger, "account=" + id(own), token(other)).result(), http::status::forbidden);
  Socket anonymous(io);
  ASSERT_EQ(open(anonymous, "account=main", "").result(), http::status::switching_protocols);
  EXPECT_EQ(next(anonymous).at("account"), "main");
  // Its own token and the operator's follow it.
  for (const auto& secret : {token(own), std::string("operator")}) {
    Socket ws(io);
    ASSERT_EQ(open(ws, "account=" + id(own), secret).result(), http::status::switching_protocols) << secret;
    EXPECT_EQ(next(ws).at("type"), "event_stream");
    events->publish({"fill", id(other), 0, "Other sandbox", {{"id", 1}}});
    events->publish({"fill", id(own), 0, "Own sandbox", {{"id", 2}}});
    const auto event = next(ws);
    EXPECT_EQ(event.at("account"), id(own));
    EXPECT_EQ(event.at("details").at("id"), 2);
    ws.close(websocket::close_code::normal);
  }
  anonymous.close(websocket::close_code::normal);
  web.stop();
}
TEST_F(Sandboxes, EncodedAccountKeysCannotReachAnotherAccountOrAHiddenSandbox) {
  const auto own = create();
  const auto created = send(request("POST", "/api/accounts", "operator", R"({"name":"Beta","plan":"practice"})"));
  ASSERT_EQ(created.status, 201) << created.body;
  policy.tokens.push_back({"mainer", {"read", "trade:main"}, "mainer-secret"});
  // The playbook routes decode %61ccount as account; the scope check must read beta too.
  for (const auto* query : {"?%61ccount=beta", "?account=%62eta"}) {
    const auto refused = send(request("POST", std::string("/api/playbooks/staged/1/send") + query, "mainer-secret"));
    EXPECT_EQ(refused.status, 403) << query << refused.body;
    EXPECT_EQ(json::parse(refused.body)["error"]["code"], "SCOPE_REQUIRED") << query;
  }
  // Without a token, a sandbox stays hidden however its ID or the key is spelled.
  const auto encoded = "%73" + id(own).substr(1);
  for (const auto& path : {"/api/playbooks?%61ccount=" + id(own), "/api/playbooks?account=" + encoded,
                           "/api/account/pass-odds?%61ccount=" + id(own), "/api/portfolio?%61ccount=" + id(own),
                           "/api/account?account=" + encoded}) {
    const auto hidden = send(request("GET", path));
    EXPECT_EQ(hidden.status, 404) << path << hidden.body;
    EXPECT_EQ(json::parse(hidden.body)["error"]["code"], "UNKNOWN_ACCOUNT") << path;
  }
  // The same spellings name the same accounts for callers entitled to them.
  EXPECT_EQ(send(request("GET", "/api/playbooks?%61ccount=" + id(own), token(own))).status, 200);
  EXPECT_EQ(send(request("GET", "/api/account?account=" + encoded, token(own))).status, 200);
  EXPECT_EQ(send(request("GET", "/api/playbooks?%61ccount=beta", "operator")).status, 200);
  EXPECT_EQ(send(request("GET", "/api/orders?%61ccount=main", token(own))).status, 403);
}
TEST_F(Sandboxes, OrderRateIncludesPreviewsAndEditsButExitsStayAvailable) {
  limits.orders = 2;
  configure();
  const auto own = create();
  const auto market_time = source->desk->market_time();
  const auto input = [&](std::string path) { return request("POST", path + "?account=" + id(own), token(own)); };
  EXPECT_FALSE(server::check_api_write(input("/api/orders"), policy));
  now += 10s;
  EXPECT_FALSE(server::check_api_write(input("/api/orders/preview"), policy));
  for (const auto elapsed : {10250ms, 59500ms}) {
    now = server::Sandboxes::Clock::time_point{} + elapsed;
    for (const auto& [method, path] : std::vector<std::pair<std::string, std::string>>{
        {"POST", "/api/orders"}, {"POST", "/api/orders/preview"}, {"POST", "/api/orders/what-if"},
        {"PUT", "/api/orders/1"}, {"POST", "/api/orders/1/preview"},
        // Notes, alerts and grouping are journaled too, and share the budget.
        {"PUT", "/api/days/2026-10-09/note"}, {"PUT", "/api/trades/1/note"}, {"POST", "/api/alerts"},
        {"POST", "/api/trades/group"}}) {
      const auto refused = server::check_api_write(request(method, path, token(own)), policy);
      ASSERT_TRUE(refused) << path;
      EXPECT_EQ(refused->status, 429);
      EXPECT_EQ(json::parse(refused->body)["error"]["code"], "SANDBOX_ORDER_RATE");
      EXPECT_EQ(refused->retry_after, elapsed == 10250ms ? 50 : 1);
    }
  }
  const auto admin_refused = server::check_api_write(request("POST", "/api/orders?account=" + id(own), "operator"), policy);
  ASSERT_TRUE(admin_refused);
  EXPECT_EQ(admin_refused->status, 429);
  EXPECT_EQ(admin_refused->retry_after, 1);
  EXPECT_FALSE(server::check_api_write(input("/api/orders/cancel"), policy));
  EXPECT_FALSE(server::check_api_write(input("/api/positions/close"), policy));
  EXPECT_FALSE(server::check_api_write(request("DELETE", "/api/orders/1", token(own)), policy));
  now = server::Sandboxes::Clock::time_point{} + limits.order_window;
  EXPECT_FALSE(server::check_api_write(input("/api/orders"), policy));
  const auto refused = server::check_api_write(input("/api/orders"), policy);
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->retry_after, 10);
  EXPECT_EQ(source->desk->market_time(), market_time);
}
TEST_F(Sandboxes, ShareTradesAndPreviewsShareOrderRateButShareClosesDoNot) {
  for (const auto* stock_path : {"/api/stocks/trade", "/api/stocks/trade/preview"}) {
    SCOPED_TRACE(stock_path);
    limits.orders = 2;
    now = {};
    configure();
    const auto own = create();
    const auto check = [&](const std::string& path) {
      return server::check_api_write(request("POST", path, token(own)), policy);
    };
    EXPECT_FALSE(check("/api/stocks/close"));
    EXPECT_FALSE(check(stock_path));
    now += 10s;
    EXPECT_FALSE(check("/api/orders"));
    now += 250ms;
    for (const auto* path : {"/api/orders", "/api/stocks/trade", "/api/stocks/trade/preview"}) {
      const auto refused = check(path);
      ASSERT_TRUE(refused) << path;
      EXPECT_EQ(refused->status, 429);
      EXPECT_EQ(json::parse(refused->body)["error"]["code"], "SANDBOX_ORDER_RATE");
      EXPECT_EQ(refused->retry_after, 50);
    }
    EXPECT_FALSE(check("/api/stocks/close"));
    now = server::Sandboxes::Clock::time_point{} + limits.order_window;
    EXPECT_FALSE(check(stock_path));
    const auto refused = check(stock_path);
    ASSERT_TRUE(refused);
    EXPECT_EQ(refused->retry_after, 10);
  }
}
TEST_F(Sandboxes, ExitsHaveABudgetOfTheirOwnSoRefusedOnesCannotGrowTheJournal) {
  limits.orders = 2;
  limits.exits = 3;
  configure();
  const auto own = create();
  const auto journal = files.directory / "sandboxes" / id(own) / (id(own) + ".jsonl");
  const auto records = [&] { return trading::FileJournal::read(journal.string()).records.size(); };
  const auto before = records();
  // Each names nothing the account has, so the engine refuses it and journals that.
  struct NoOp { std::string method, path, body; };
  const std::vector<NoOp> no_ops{{"DELETE", "/api/orders/999", ""}, {"DELETE", "/api/alerts/999", ""},
                                 {"POST", "/api/orders/cancel", R"({"orders":["999"]})"}};
  int refused = 0, limited = 0;
  for (int round = 0; round < 4; ++round) {
    for (const auto& [method, path, body] : no_ops) {
      const auto response = send(request(method, path, token(own), body));
      if (response.status == 429) {
        ++limited;
        EXPECT_EQ(json::parse(response.body)["error"]["code"], "SANDBOX_ORDER_RATE");
      } else {
        ++refused;
        EXPECT_EQ(response.status, 404) << path << response.body;
      }
    }
  }
  EXPECT_EQ(refused, 3);
  EXPECT_EQ(limited, 9);
  EXPECT_LE(records() - before, 3U);
  now += 10s;
  // Every exit shares that budget, whoever sends it to the sandbox; the main account has none.
  for (const auto& [method, path] : std::vector<std::pair<std::string, std::string>>{
           {"DELETE", "/api/orders/1"}, {"POST", "/api/orders/cancel"}, {"DELETE", "/api/stocks/orders/1"},
           {"POST", "/api/positions/close"}, {"POST", "/api/positions/close/preview"}, {"POST", "/api/stocks/close"},
           {"POST", "/api/positions/exercise"}, {"POST", "/api/positions/abandon"},
           {"POST", "/api/positions/instruction"}, {"DELETE", "/api/alerts/1"}}) {
    const auto rejected = server::check_api_write(request(method, path, token(own)), policy);
    ASSERT_TRUE(rejected) << path;
    EXPECT_EQ(rejected->status, 429) << path;
    EXPECT_EQ(rejected->retry_after, 50) << path;
  }
  const auto admin_refused = server::check_api_write(request("DELETE", "/api/orders/1?account=" + id(own), "operator"), policy);
  ASSERT_TRUE(admin_refused);
  EXPECT_EQ(admin_refused->status, 429);
  EXPECT_FALSE(server::check_api_write(request("DELETE", "/api/orders/1", "operator"), policy));
  // Spent exits leave the order budget alone, as spent orders leave exits alone.
  EXPECT_FALSE(server::check_api_write(request("POST", "/api/orders/preview", token(own)), policy));
  now = server::Sandboxes::Clock::time_point{} + limits.order_window;
  EXPECT_FALSE(server::check_api_write(request("DELETE", "/api/orders/1", token(own)), policy));
}
TEST_F(Sandboxes, HttpRateLimitsEmitRetryAfterButCapacityDoesNot) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  namespace http = beast::http;
  for (const auto* code : {"SANDBOX_CLIENT_RATE", "SANDBOX_GLOBAL_RATE", "SANDBOX_ORDER_RATE", "SANDBOX_CAPACITY"}) {
    SCOPED_TRACE(code);
    const std::string reason = code;
    limits.capacity = reason == "SANDBOX_CAPACITY" ? 1 : 10;
    limits.per_client = reason == "SANDBOX_CLIENT_RATE" ? 1 : 3;
    limits.global = reason == "SANDBOX_GLOBAL_RATE" ? 1 : 30;
    limits.orders = 1;
    now = {};
    configure();
    const auto own = create("127.0.0.1");
    EXPECT_FALSE(manager->check_order(id(own)));
    now += 10250ms;
    server::WebServer web("127.0.0.1", 0, {},
        [&](const server::ApiRequest& input, server::ApiCompletion complete) {
          server::handle_api_async(input, *source, std::move(complete));
        }, {}, "operator", {}, {}, false, manager);
    web.start(1);
    asio::io_context io;
    beast::tcp_stream connection(io);
    connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::string_body> input{http::verb::post,
        reason == "SANDBOX_ORDER_RATE" ? "/api/orders" : "/api/sandboxes", 11};
    input.set(http::field::host, "localhost");
    input.set(http::field::content_type, "application/json");
    if (reason == "SANDBOX_ORDER_RATE") input.set(http::field::authorization, "Bearer " + token(own));
    input.body() = "{}";
    input.prepare_payload();
    http::write(connection, input);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(connection, buffer, response);
    EXPECT_EQ(response.result_int(), 429);
    EXPECT_EQ(json::parse(response.body())["error"]["code"], reason);
    if (reason == "SANDBOX_CAPACITY") {
      EXPECT_EQ(response.count(http::field::retry_after), 0U);
    } else {
      EXPECT_EQ(response[http::field::retry_after], reason == "SANDBOX_ORDER_RATE" ? "50" : "3590");
    }
    web.stop();
  }
}
TEST_F(Sandboxes, IdleExpiryDeletesAccountCredentialAndAllFilesWithoutUsingMarketTime) {
  limits.capacity = 1;
  configure();
  const auto own = create();
  const auto directory = files.directory / "sandboxes" / id(own);
  std::ofstream(directory / "extra.snapshot.json") << "{}";
  const auto market_time = source->desk->market_time();
  now += 23h;
  EXPECT_EQ(send(request("GET", "/api/status", token(own))).status, 200);
  now += 23h;
  source->desk->expire_sandboxes(manager->expired());
  EXPECT_TRUE(source->trading_view(id(own)));
  now += 1h;
  EXPECT_FALSE(manager->active(id(own)));
  EXPECT_EQ(send(request("GET", "/api/status", token(own))).status, 403);
  // Like a deleted account, it is removed from notifications.
  std::vector<std::string> removed;
  source->desk->set_removal_sink([&](const std::string& account) { removed.push_back(account); });
  source->desk->expire_sandboxes(manager->expired());
  EXPECT_EQ(removed, std::vector<std::string>{id(own)});
  EXPECT_FALSE(source->trading_view(id(own)));
  EXPECT_FALSE(std::filesystem::exists(directory));
  EXPECT_TRUE(std::filesystem::exists(desk_options.paper_journal));
  EXPECT_EQ(source->desk->market_time(), market_time);
  create();
}
TEST_F(Sandboxes, RestartRemovesOnlySandboxFilesAndRefusesOldSecret) {
  const auto own = create();
  std::ofstream(files.directory / "unrelated") << "keep";
  configure();
  EXPECT_FALSE(std::filesystem::exists(files.directory / "sandboxes"));
  EXPECT_TRUE(std::filesystem::exists(files.directory / "unrelated"));
  EXPECT_TRUE(std::filesystem::exists(desk_options.paper_journal));
  EXPECT_FALSE(source->trading_view(id(own)));
  EXPECT_EQ(send(request("GET", "/api/status", token(own))).status, 403);
  create();
}
TEST_F(Sandboxes, FailedJournalRefusesCreationAndReleasesCapacity) {
  limits.capacity = 1;
  configure();
  fail_sync = true;
  const auto refused = send(request("POST", "/api/sandboxes"));
  EXPECT_EQ(refused.status, 503);
  EXPECT_EQ(refused.body.find("sandbox_"), std::string::npos);
  EXPECT_TRUE(std::filesystem::is_empty(files.directory / "sandboxes"));
  fail_sync = false;
  source->desk->expire_sandboxes(manager->expired());
  create();
}
// B-119: a creation that makes no sandbox gives its budget back, so the retry its 503
// asks for is not refused for the rest of the hour, for this client or for everyone.
TEST_F(Sandboxes, FailedCreationsSpendNoCreationBudget) {
  limits.per_client = 3;
  limits.global = 4;
  configure();
  source->inbox_full = true;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const auto busy = send(request("POST", "/api/sandboxes"));
    EXPECT_EQ(busy.status, 503) << attempt << busy.body;
    EXPECT_EQ(busy.retry_after, server::kInboxRetrySeconds) << attempt;
  }
  source->inbox_full = false;
  fail_sync = true;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const auto failed = send(request("POST", "/api/sandboxes"));
    EXPECT_EQ(failed.status, 503) << attempt << failed.body;
  }
  fail_sync = false;
  source->desk->expire_sandboxes(manager->expired());
  for (int created = 0; created < 3; ++created) create();
  EXPECT_EQ(send(request("POST", "/api/sandboxes")).status, 429);
}
TEST_F(Sandboxes, OffKeepsPublicReadsAndWritePolicyAndDoesNotCleanFiles) {
  source.reset();
  desk_options.sandboxes.reset();
  std::filesystem::create_directories(files.directory / "sandboxes");
  std::ofstream(files.directory / "sandboxes" / "untouched") << "keep";
  source = std::make_unique<SandboxSource>(desk_options);
  policy.sandboxes.reset();
  EXPECT_EQ(send(request("POST", "/api/sandboxes")).status, 404);
  EXPECT_EQ(send(request("GET", "/api/status", "stale-token")).status, 200);
  EXPECT_FALSE(json::parse(send(request("GET", "/api/status")).body).contains("sandboxes"));
  EXPECT_EQ(send(request("POST", "/api/orders")).status, 403);
  EXPECT_TRUE(std::filesystem::exists(files.directory / "sandboxes" / "untouched"));
}
TEST_F(Sandboxes, CreationStillChecksOriginAndContentType) {
  auto input = request("POST", "/api/sandboxes");
  input.origin = "https://evil.example";
  EXPECT_EQ(send(input).status, 403);
  input.origin.reset();
  input.content_type = "text/plain";
  EXPECT_EQ(send(input).status, 400);
  input.content_type = "application/json";
  input.body = R"({"plan":"funded-50k"})";
  EXPECT_EQ(send(input).status, 400);
  input.body = " { } ";
  EXPECT_EQ(send(input).status, 201);
}
}  // namespace
