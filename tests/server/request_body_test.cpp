// Request bodies a client controls cannot end the process: depth, encoding and
// syntax are refused with a 400 on every route, including those served off the
// HTTP threads.
#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <filesystem>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

#include "openport/md/time.hpp"
#include "openport/server/api.hpp"
#include "openport/server/earnings.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/web_server.hpp"

namespace {
using namespace openport;
using nlohmann::json;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;

class QuietProvider final : public md::Provider {
 public:
  std::string_view name() const noexcept override { return "quiet"; }
  md::Capabilities capabilities() const noexcept override { return {}; }
  void start(const md::Subscription&, md::EventSink&) override {}
  void stop() override {}
};

std::string nested(std::size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

struct Reply {
  int status = 0;
  std::string body;
};

Reply send(unsigned short port, http::verb method, const std::string& target, const std::string& body) {
  asio::io_context io;
  beast::tcp_stream connection(io);
  connection.connect({asio::ip::make_address("127.0.0.1"), port});
  http::request<http::string_body> request{method, target, 11};
  request.set(http::field::host, "localhost");
  if (method != http::verb::get) request.set(http::field::content_type, "application/json");
  request.body() = body;
  request.keep_alive(false);
  request.prepare_payload();
  connection.expires_after(std::chrono::seconds(30));
  http::write(connection, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  beast::error_code error;
  http::read(connection, buffer, response, error);
  if (error) return {0, error.message()};
  return {static_cast<int>(response.result_int()), response.body()};
}

TEST(RequestBodies, DeepUnencodableAndMalformedBodiesAre400OnEveryRouteAndTheServerStaysUp) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-bodies-" + std::to_string(md::now()));
  std::filesystem::create_directories(directory / "recordings");
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(0);
  options.paper_journal = directory / "paper.jsonl";
  QuietProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, options);
  engine.start();
  server::ReplayHost replays({directory / "recordings", options, false});
  server::EarningsStore earnings;
  // Wired as openportd wires them, so replay routes run on the replay worker thread.
  server::WebServer web("127.0.0.1", 0, {},
      [&](const server::ApiRequest& request, server::ApiCompletion complete) {
        if (auto response = server::earnings_request(request, earnings)) { complete(std::move(*response)); return; }
        if (replays.handle(request, complete)) return;
        server::handle_api_async(request, engine, std::move(complete));
      });
  web.start(1);

  const auto deep = nested(30000);
  const std::vector<std::pair<http::verb, std::string>> routes{
      {http::verb::post, "/api/orders/what-if"},
      {http::verb::post, "/api/positions/close"},
      {http::verb::post, "/api/positions/close/preview"},
      {http::verb::post, "/api/account/rules-compare"},
      {http::verb::post, "/api/rule-packs/validate"},
      {http::verb::post, "/api/orders"},
      {http::verb::post, "/api/earnings/import"},
      {http::verb::post, "/api/replay"},
      {http::verb::put, "/api/replay"},
      {http::verb::post, "/api/replay/packs"},
  };
  const std::vector<std::pair<std::string, std::string>> bodies{
      {"30,000-deep arrays", deep},
      {"what-if shaped", R"({"candidates":[{"orders":[)" + deep + "]}]}"},
      {"close shaped", R"({"underlying":)" + deep + "}"},
      {"rules-compare shaped", R"({"plans":[)" + deep + "]}"},
      {"rule-pack shaped", R"({"version":1,"plans":[{"rules":{"x":)" + deep + "}}]}"},
      {"a lone 0xFF byte", "\xff"},
      {"0xFF in a string", "{\"a\":\"\xff\"}"},
      {"0xFF in a key", "{\"\xff\":1}"},
      {"truncated UTF-8", "{\"a\":\"\xc3\"}"},
      {"not JSON", "{bad"},
  };
  for (const auto& [method, target] : routes) {
    for (const auto& [label, body] : bodies) {
      SCOPED_TRACE(std::string(http::to_string(method)) + " " + target + " with " + label);
      const auto reply = send(web.port(), method, target, body);
      ASSERT_EQ(reply.status, 400) << reply.body;
      const auto error = json::parse(reply.body).at("error");
      EXPECT_EQ(error.at("code"), "INVALID_REQUEST");
      // The refusal names the problem without quoting the body back.
      EXPECT_LT(error.at("message").get<std::string>().size(), 100U);
    }
  }
  // Still serving, and a body at the depth limit is read as usual.
  EXPECT_EQ(send(web.port(), http::verb::get, "/api/status", {}).status, 200);
  const auto at_limit = send(web.port(), http::verb::post, "/api/orders/cancel",
      R"({"underlying":)" + nested(server::kRequestJsonDepthMax - 1) + "}");
  EXPECT_EQ(at_limit.status, 400);
  EXPECT_NE(at_limit.body.find("underlying must be a string"), std::string::npos) << at_limit.body;
  EXPECT_FALSE(server::check_request_json("{\"a\":" + nested(server::kRequestJsonDepthMax - 1) + "}"));
  EXPECT_TRUE(server::check_request_json("{\"a\":" + nested(server::kRequestJsonDepthMax) + "}"));
  web.stop();
  replays.stop();
  engine.stop();
  std::filesystem::remove_all(directory);
}

TEST(RequestBodies, ApiErrorPrintsMessagesThatAreNotUtf8) {
  const auto response = server::api_error(400, "INVALID_REQUEST", "last read: '\xff'");
  EXPECT_EQ(response.status, 400);
  EXPECT_EQ(json::parse(response.body).at("error").at("message"), "last read: '\xef\xbf\xbd'");
}

TEST(RequestBodies, ReplayRoutesCalledDirectlyRefuseBodiesThatAreNotUtf8) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-replay-bodies-" + std::to_string(md::now()));
  std::filesystem::create_directories(directory / "recordings");
  server::Engine::Options options;
  options.paper_journal = directory / "paper.jsonl";
  // Demo mode, so the pack route reads its body instead of refusing first.
  server::ReplayHost host({directory / "recordings", options, true});
  for (const auto& [method, target] : {std::pair{"POST", "/api/replay"}, std::pair{"PUT", "/api/replay"},
                                        std::pair{"POST", "/api/replay/packs"}}) {
    SCOPED_TRACE(std::string(method) + " " + target);
    std::promise<server::ApiResponse> done;
    auto result = done.get_future();
    server::ApiRequest request{method, target, "{\"\xff\":1}"};
    request.content_type = "application/json";
    ASSERT_TRUE(host.handle(request, [&](server::ApiResponse response) { done.set_value(std::move(response)); }));
    ASSERT_EQ(result.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    const auto response = result.get();
    EXPECT_EQ(response.status, 400) << response.body;
  }
  host.stop();
  std::filesystem::remove_all(directory);
}

}  // namespace

namespace {

TEST(RequestBodies, ACompletionDroppedUncalledAnswers500AndFreesTheSession) {
  server::WebServer web("127.0.0.1", 0, {},
      [](const server::ApiRequest& request, server::ApiCompletion complete) {
        if (request.target == "/api/status") complete({200, "{}"});
        // Otherwise the completion is destroyed without being called.
      });
  web.start(1);
  const auto dropped = send(web.port(), http::verb::post, "/api/orders", "{}");
  EXPECT_EQ(dropped.status, 500) << dropped.body;
  EXPECT_EQ(json::parse(dropped.body).at("error").at("code"), "INTERNAL_ERROR");
  EXPECT_EQ(send(web.port(), http::verb::get, "/api/status", {}).status, 200);
  web.stop();
}

TEST(RequestBodies, AShareOrderPreviewWhoseCostOverflowsIsAnswered) {
  server::Engine::Options options;
  options.analytics_interval = std::chrono::milliseconds(0);
  QuietProvider provider;
  server::Engine engine(provider, md::Subscription{{"SPY"}}, options);
  engine.start();
  for (const auto& order : {json{{"client_order_id", "a1"}, {"symbol", "SPY"}, {"side", "buy"}, {"type", "limit"},
                                 {"shares", 10000000}, {"limit_price", "1000000"}},
                            json{{"client_order_id", "a2"}, {"symbol", "SPY"}, {"side", "sell"}, {"type", "stop"},
                                 {"shares", 10000000}, {"stop_price", "1000000"}}}) {
    SCOPED_TRACE(order.dump());
    auto done = std::make_shared<std::promise<server::ApiResponse>>();
    auto result = done->get_future();
    server::ApiRequest request{"POST", "/api/stocks/orders/preview", order.dump()};
    request.content_type = "application/json";
    server::handle_api_async(request, engine, [done](server::ApiResponse response) { done->set_value(std::move(response)); });
    ASSERT_EQ(result.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    const auto response = result.get();
    ASSERT_EQ(response.status, 422) << response.body;
    EXPECT_EQ(json::parse(response.body).at("error").at("code"), "ARITHMETIC_OVERFLOW");
  }
  engine.stop();
}

}  // namespace
