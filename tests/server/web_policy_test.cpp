#include "openport/server/web_policy.hpp"

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <fstream>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include "openport/server/sandboxes.hpp"
#include "openport/server/web_server.hpp"

namespace {

using namespace openport;
namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class StaticFiles : public testing::Test {
 protected:
  void SetUp() override {
    parent = fs::temp_directory_path() /
             ("openport-static-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    root = parent / "root";
    fs::create_directories(root / "assets");
    fs::create_directories(parent / "root-outside");
    std::ofstream(root / "index.html") << "shell";
    std::ofstream(root / "assets" / "app.js") << "asset";
    std::ofstream(parent / "root-outside" / "secret") << "secret";
  }
  void TearDown() override { fs::remove_all(parent); }
  fs::path parent;
  fs::path root;
};

TEST_F(StaticFiles, RejectsAbsoluteTraversalBackslashAndNulComponents) {
  for (const std::string target :
       {"//etc/passwd", "/%2Fetc/passwd", "/a/%2fetc/passwd", "/../secret", "/%2e%2e/secret",
        "/a\\b", "/a%5cb", "/a%00b", "/bad%", "/bad%zz"}) {
    const auto result = server::resolve_static_file(root, target);
    EXPECT_EQ(result.status, 400) << target;
    EXPECT_FALSE(result.reason.empty());
  }
  EXPECT_EQ(server::resolve_static_file(root, std::string("/a") + '\0' + "b").status, 400);
}

TEST_F(StaticFiles, RejectsSymlinksOutsideTheCanonicalRootIncludingTheShell) {
  fs::create_symlink(parent / "root-outside" / "secret", root / "leak.js");
  fs::create_directory_symlink(parent / "root-outside", root / "escape");
  EXPECT_EQ(server::resolve_static_file(root, "/leak.js").status, 403);
  EXPECT_EQ(server::resolve_static_file(root, "/escape/secret").status, 403);
  EXPECT_EQ(server::resolve_static_file(root, "/escape/missing-route").status, 403);
  fs::remove(root / "index.html");
  fs::create_symlink(parent / "root-outside" / "secret", root / "index.html");
  EXPECT_EQ(server::resolve_static_file(root, "/chain/SPX").status, 403);
}

TEST_F(StaticFiles, ServesAssetsAndSpaFallbackButOnlyRegularFiles) {
  EXPECT_EQ(server::resolve_static_file(root, "/assets/app.js?v=1").path,
            fs::canonical(root / "assets" / "app.js"));
  EXPECT_EQ(server::resolve_static_file(root, "/assets/%61pp.js").status, 200);
  EXPECT_EQ(server::resolve_static_file(root, "/chain/SPX").path,
            fs::canonical(root / "index.html"));
  EXPECT_EQ(server::resolve_static_file(root, "/").status, 200);
  EXPECT_EQ(server::resolve_static_file(root, "/assets/missing.js").status, 404);
  EXPECT_EQ(server::resolve_static_file(root, "/assets").status, 404);
  fs::create_symlink(root / "assets" / "app.js", root / "local.js");
  EXPECT_EQ(server::resolve_static_file(root, "/local.js").status, 200);
}

TEST(WebPolicy, WebSocketOriginsMustMatchHost) {
  EXPECT_TRUE(server::websocket_origin_allowed(std::nullopt, "localhost:8080"));
  EXPECT_TRUE(server::websocket_origin_allowed("http://localhost:8080", "localhost:8080"));
  EXPECT_TRUE(server::websocket_origin_allowed("http://LOCALHOST:80", "localhost"));
  EXPECT_TRUE(server::websocket_origin_allowed("http://[::1]:8080", "[::1]:8080"));
  for (const std::string origin :
       {"", "null", "https://evil.test", "http://localhost:8081", "http://localhost:8080@evil.test",
        "http://localhost:8080/", "http://localhost:8080,https://evil.test",
        "http://localhost:8080?x"}) {
    EXPECT_FALSE(server::websocket_origin_allowed(origin, "localhost:8080")) << origin;
  }
}

TEST(WebPolicy, HostsMustBeAddressesLocalhostOrNamesAllowedOnPurpose) {
  for (const std::string host : {"127.0.0.1", "127.0.0.1:8080", "10.0.0.7:8080", "[::1]:8080", "[fe80::1]",
                                 "localhost", "LOCALHOST:8080", "app.localhost:5173"}) {
    EXPECT_TRUE(server::host_allowed(host)) << host;
  }
  // DNS rebinding arrives under the attacker's name, and malformed values fail closed.
  for (const std::string host : {"evil.test", "evil.test:8080", "localhost.evil.test", "127.0.0.1.evil.test",
                                 "", "localhost:8080@evil.test", "localhost:99999", "[::1", "local host"}) {
    EXPECT_FALSE(server::host_allowed(host)) << host;
  }
  EXPECT_TRUE(server::host_allowed("Terminal.Example", {"https://terminal.example"}));
  EXPECT_TRUE(server::host_allowed("internal:8080", {}, {"internal"}));
  EXPECT_TRUE(server::host_allowed("internal", {}, {"INTERNAL:9000"}));
  EXPECT_FALSE(server::host_allowed("terminal.example.evil", {"https://terminal.example"}, {"internal"}));
}

TEST(WebPolicy, SessionSlotsAreBoundedAndReleasedAfterFailedOrClosedSessions) {
  server::WebSocketSlots slots;
  std::vector<std::unique_ptr<server::WebSocketSlots::Lease>> sessions;
  for (std::size_t i = 0; i < 256; ++i) {
    auto session = slots.acquire();
    ASSERT_TRUE(session);
    sessions.push_back(std::move(session));
  }
  EXPECT_FALSE(slots.acquire());
  sessions.pop_back();
  EXPECT_TRUE(slots.acquire());
  sessions.clear();
  EXPECT_TRUE(slots.acquire());
}

TEST(WebServer, RejectsForeignOriginsAndOmitsCorsHeaders) {
  server::WebServer web("127.0.0.1", 0, {},
                        [](const auto&) { return server::ApiResponse{200, "{}"}; });
  web.start();
  asio::io_context io;
  const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), web.port());
  const auto host = "127.0.0.1:" + std::to_string(web.port());
  tcp::socket socket(io);
  socket.connect(endpoint);
  http::request<http::empty_body> request{http::verb::get, "/api/status", 11};
  request.set(http::field::host, host);
  http::write(socket, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(socket, buffer, response);
  EXPECT_EQ(response.count(http::field::access_control_allow_origin), 0u);
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect(endpoint);
  ws.set_option(websocket::stream_base::decorator(
      [](websocket::request_type& req) { req.set(http::field::origin, "https://evil.test"); }));
  beast::error_code ec;
  websocket::response_type upgrade;
  ws.handshake(upgrade, host, "/ws", ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(upgrade.result(), http::status::forbidden);
  web.stop();
}

TEST(WebServer, RefusesRequestsAddressedByARebindingName) {
  server::WebServer web("127.0.0.1", 0, {},
                        [](const auto&) { return server::ApiResponse{200, "{}"}; });
  web.start();
  asio::io_context io;
  const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), web.port());
  const auto port = std::to_string(web.port());
  const auto get = [&](const std::string& host, const std::string& target) {
    tcp::socket socket(io);
    socket.connect(endpoint);
    http::request<http::empty_body> request{http::verb::get, target, 11};
    request.set(http::field::host, host);
    http::write(socket, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
  };
  EXPECT_EQ(get("127.0.0.1:" + port, "/api/status").result(), http::status::ok);
  EXPECT_EQ(get("localhost:" + port, "/api/status").result(), http::status::ok);
  const auto api = get("evil.test:" + port, "/api/status");
  EXPECT_EQ(api.result(), http::status::forbidden);
  EXPECT_NE(api.body().find("HOST_REJECTED"), std::string::npos) << api.body();
  EXPECT_EQ(get("evil.test:" + port, "/").result(), http::status::forbidden);
  // A rebinding page is same-origin with its own name, so its Origin matches Host.
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect(endpoint);
  ws.set_option(websocket::stream_base::decorator(
      [&](websocket::request_type& req) { req.set(http::field::origin, "http://evil.test:" + port); }));
  beast::error_code ec;
  websocket::response_type upgrade;
  ws.handshake(upgrade, "evil.test:" + port, "/ws", ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(upgrade.result(), http::status::forbidden);
  web.stop();
}

TEST(WebServer, ClosesWebSocketsWithMessagesLargerThanFourKiB) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; });
  web.start();
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
  ws.handshake("127.0.0.1:" + std::to_string(web.port()), "/ws");
  ws.write(asio::buffer(std::string(4097, 'x')));
  beast::flat_buffer buffer;
  beast::error_code ec;
  bool completed = false;
  ws.async_read(buffer, [&](beast::error_code error, std::size_t) {
    ec = error;
    completed = true;
  });
  io.run_for(std::chrono::minutes(5));
  EXPECT_TRUE(completed);
  EXPECT_EQ(ec, websocket::error::closed);
  EXPECT_EQ(ws.reason().code, websocket::close_code::too_big);
  web.stop();
}

}  // namespace

namespace {
TEST(WebPolicy, ExplicitProxyOriginsMatchExactlyAfterNormalization) {
  const std::vector<std::string> allowed{"https://Terminal.Example:443", "http://localhost:3000"};
  EXPECT_TRUE(
      server::websocket_origin_allowed("https://terminal.example", "internal:8080", allowed));
  EXPECT_TRUE(server::websocket_origin_allowed("http://localhost:3000", "internal:8080", allowed));
  EXPECT_TRUE(server::websocket_origin_allowed("http://internal:8080", "internal:8080", allowed));
  for (const auto* origin :
       {"http://terminal.example", "https://terminal.example:444", "https://terminal.example.evil",
        "https://terminal.example/", "https://terminal.example@evil", "null"})
    EXPECT_FALSE(server::websocket_origin_allowed(origin, "internal:8080", allowed)) << origin;
  EXPECT_THROW((server::WebServer("127.0.0.1", 0, {}, {}, {"https://host/path"})),
               std::invalid_argument);
}

TEST(WebPolicy, SlowClientKeepsActiveWriteAndOnlyNewestPendingTick) {
  server::TickQueue queue;
  const auto active = std::make_shared<const std::string>("active");
  queue.push(active);
  for (int i = 0; i < 10000; ++i) {
    queue.push(std::make_shared<const std::string>(std::to_string(i)));
    EXPECT_EQ(queue.size(), 2u);
    EXPECT_EQ(queue.front(), active);
  }
  queue.pop();
  ASSERT_EQ(queue.size(), 1u);
  EXPECT_EQ(*queue.front(), "9999");
  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST(WebPolicy, TheStartupLinkEncodesItsTokenAsTheTerminalDecodesIt) {
  EXPECT_EQ(server::url_component("0123abcdef"), "0123abcdef");
  // decodeURIComponent restores each: & and # no longer end the fragment, % stays literal.
  EXPECT_EQ(server::url_component("p&ss#1"), "p%26ss%231");
  EXPECT_EQ(server::url_component("50%off"), "50%25off");
  EXPECT_EQ(server::url_component("%41 caf\xc3\xa9"), "%2541%20caf%C3%A9");
}

TEST(WebPolicy, SlowClientKeepsTheNewestTickOfEachStream) {
  server::TickQueue queue;
  queue.push(std::make_shared<const std::string>("active"), "tick");
  // Every second the engine's tick and then the replay's arrive during a write.
  for (int i = 0; i < 3; ++i) {
    queue.push(std::make_shared<const std::string>("tick " + std::to_string(i)), "tick");
    queue.push(std::make_shared<const std::string>("replay " + std::to_string(i)), "replay_tick");
  }
  ASSERT_EQ(queue.size(), 3u);
  queue.pop();
  EXPECT_EQ(*queue.front(), "tick 2");
  queue.pop();
  EXPECT_EQ(*queue.front(), "replay 2");
  queue.pop();
  EXPECT_TRUE(queue.empty());
}

TEST_F(StaticFiles, MissingWebRootExplainsHowToBuildOrSelectTheTerminal) {
  const auto result = server::resolve_static_file(parent / "missing", "/");
  EXPECT_EQ(result.status, 404);
  EXPECT_EQ(result.reason,
            "web terminal not built: run `npm run build` in web/, or pass --web-root");
}

TEST(WebServer, ConnectionsBeyondTheCapAreClosedAtOnceAndFreedSlotsServeAgain) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; });
  web.set_http_session_limit(2);
  web.start(2);
  asio::io_context io;
  const tcp::endpoint endpoint{asio::ip::make_address("127.0.0.1"), web.port()};
  const auto get = [&](tcp::socket& socket) {
    http::request<http::empty_body> request{http::verb::get, "/api/status", 11};
    request.set(http::field::host, "localhost");
    beast::error_code ec;
    http::write(socket, request, ec);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    if (!ec) http::read(socket, buffer, response, ec);
    return !ec;
  };
  tcp::socket first(io), second(io), third(io);
  first.connect(endpoint);
  second.connect(endpoint);
  ASSERT_TRUE(get(first));
  ASSERT_TRUE(get(second));
  third.connect(endpoint);
  const auto started = std::chrono::steady_clock::now();
  char byte;
  beast::error_code closed;
  third.read_some(asio::buffer(&byte, 1), closed);
  EXPECT_EQ(closed, asio::error::eof);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
  first.close();
  bool served = false;
  for (int attempt = 0; attempt < 500 && !served; ++attempt) {
    tcp::socket next(io);
    next.connect(endpoint);
    served = get(next);
    if (!served) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(served);
  web.stop();
}

TEST(WebServer, StopClosesHttpAndWebSocketSessionsAndReleasesThePort) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; });
  web.start(2);
  const auto port = web.port();
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), port});
  ws.handshake("127.0.0.1:" + std::to_string(port), "/ws");
  tcp::socket http_socket(io);
  http_socket.connect({asio::ip::make_address("127.0.0.1"), port});
  // Round-trip one request so the server is known to own this keep-alive session.
  http::request<http::empty_body> request{http::verb::get, "/api/status", 11};
  request.set(http::field::host, "localhost");
  http::write(http_socket, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(http_socket, buffer, response);
  web.broadcast(std::string(1024 * 1024, 'x'));
  const auto started = std::chrono::steady_clock::now();
  web.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  EXPECT_THROW(web.start(), std::logic_error);
  bool http_closed = false, ws_closed = false;
  char byte;
  http_socket.async_read_some(asio::buffer(&byte, 1),
                              [&](auto ec, auto) { http_closed = bool(ec); });
  beast::flat_buffer ws_buffer;
  std::function<void()> read_ws;
  read_ws = [&] {
    ws.async_read(ws_buffer, [&](auto ec, auto) {
      if (ec)
        ws_closed = true;
      else {
        ws_buffer.consume(ws_buffer.size());
        read_ws();
      }
    });
  };
  read_ws();
  io.run_for(std::chrono::minutes(5));
  EXPECT_TRUE(http_closed);
  EXPECT_TRUE(ws_closed);
  server::WebServer replacement("127.0.0.1", port, {},
                                [](const auto&) { return server::ApiResponse{}; });
  EXPECT_NO_THROW(replacement.start());
  replacement.stop();
}

TEST(WebServer, RejectsSecondStartWhileRunningAndAcceptsConfiguredProxyOrigin) {
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; },
                        {"https://terminal.example"}, {}, {"internal"});
  web.start();
  EXPECT_THROW(web.start(), std::logic_error);
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
  ws.set_option(websocket::stream_base::decorator(
      [](auto& request) { request.set(http::field::origin, "https://terminal.example:443"); }));
  EXPECT_NO_THROW(ws.handshake("internal:8080", "/ws"));
  web.stop();
}
}  // namespace

TEST(WebPolicy, FailedWebServerStartStillConsumesTheSingleStartAttempt) {
  openport::server::WebServer web("invalid-address", 0, {}, {});
  EXPECT_THROW(web.start(), std::exception);
  EXPECT_THROW(web.start(), std::logic_error);
  EXPECT_NO_THROW(web.stop());
}

namespace {
TEST(WebServer, AsyncResponsesOutliveReadTimeoutAndKeepAlive) {
  for (const auto& [method, target] : {std::pair{http::verb::get, "/api/replay"},
                                      std::pair{http::verb::get, "/api/replay/history"},
                                      std::pair{http::verb::put, "/api/replay"}}) {
    SCOPED_TRACE(std::string(http::to_string(method)) + " " + target);
    std::promise<server::ApiCompletion> pending;
    auto completion = pending.get_future();
    server::WebServer web("127.0.0.1", 0, {},
        [&](const server::ApiRequest& request, server::ApiCompletion complete) {
          if (request.target == "/api/status") complete({200, "{}"});
          else pending.set_value(std::move(complete));
        }, {}, {}, {}, {}, false, {}, {}, {}, std::chrono::seconds(1));
    web.start(1);
    asio::io_context io;
    beast::tcp_stream connection(io);
    connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "localhost");
    if (method == http::verb::put) {
      request.set(http::field::content_type, "application/json");
      request.body() = R"({"until":"next"})";
    }
    request.prepare_payload();
    http::write(connection, request);
    ASSERT_EQ(completion.wait_for(std::chrono::seconds(10)), std::future_status::ready);

    // Delay only the completion, leaving the server's single I/O thread free.
    // Start after dispatch so the wait certainly exceeds the request read budget.
    const auto body = nlohmann::json{{"data", std::string(128 * 1024, 'x')}}.dump();
    asio::steady_timer delay(io, std::chrono::seconds(2));
    delay.async_wait([complete = completion.get(), body](beast::error_code ec) {
      if (!ec) complete({200, body});
    });
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    beast::error_code read_error;
    connection.expires_after(std::chrono::seconds(10));
    http::async_read(connection, buffer, response,
                     [&](beast::error_code ec, std::size_t) { read_error = ec; });
    io.run();
    ASSERT_FALSE(read_error) << read_error.message();
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response.body(), body);
    EXPECT_TRUE(response.keep_alive());

    // The same connection remains usable after the slow response, including
    // honoring a subsequent Connection: close request.
    http::request<http::empty_body> next{http::verb::get, "/api/status", 11};
    next.set(http::field::host, "localhost");
    next.keep_alive(false);
    http::write(connection, next);
    response = {};
    connection.expires_after(std::chrono::seconds(10));
    http::async_read(connection, buffer, response,
                     [&](beast::error_code ec, std::size_t) { read_error = ec; });
    io.restart();
    io.run();
    ASSERT_FALSE(read_error) << read_error.message();
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response.body(), "{}");
    EXPECT_FALSE(response.keep_alive());
  }
}

TEST(WebServer, IdleKeepAliveConnectionClosesAfterReadTimeout) {
  server::WebServer web("127.0.0.1", 0, {},
      [](const server::ApiRequest&) { return server::ApiResponse{200, "{}"}; },
      {}, {}, {}, {}, false, {}, {}, {}, std::chrono::seconds(1));
  web.start(1);
  asio::io_context io;
  beast::tcp_stream connection(io);
  connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
  http::request<http::empty_body> request{http::verb::get, "/api/status", 11};
  request.set(http::field::host, "localhost");
  const auto started = std::chrono::steady_clock::now();
  http::write(connection, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  beast::error_code read_error;
  connection.expires_after(std::chrono::seconds(10));
  http::async_read(connection, buffer, response,
                   [&](beast::error_code ec, std::size_t) { read_error = ec; });
  io.run();
  ASSERT_FALSE(read_error) << read_error.message();
  ASSERT_EQ(response.result_int(), 200);
  ASSERT_TRUE(response.keep_alive());

  // Wait for the server to close the idle connection. A separate client deadline
  // bounds failures; a client-side timeout must not count as a server close.
  char byte;
  connection.expires_after(std::chrono::seconds(10));
  connection.async_read_some(asio::buffer(&byte, 1),
      [&](beast::error_code ec, std::size_t bytes) {
        read_error = ec;
        EXPECT_EQ(bytes, 0u);
      });
  io.restart();
  io.run();
  EXPECT_EQ(read_error, asio::error::eof) << read_error.message();
  EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

TEST(WebServer, AsyncPostDeleteRoundTripsAndSecurityHeaders) {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<server::ApiCompletion> pending;
  std::optional<server::ApiRequest> received;
  server::WebServer web("127.0.0.1", 0, {},
      [&](const server::ApiRequest& request, server::ApiCompletion complete) {
        if (request.method == "GET") { complete({200, "{}"}); return; }
        const std::lock_guard lock(mutex);
        received = request; pending = std::move(complete); ready.notify_one();
      }, {}, "secret");
  web.start(1);
  asio::io_context io;
  beast::tcp_stream connection(io);
  connection.expires_after(std::chrono::minutes(5));
  connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
  const auto host = "127.0.0.1:" + std::to_string(web.port());
  auto request = [&](http::verb method, const std::string& target, const std::string& body) {
    http::request<http::string_body> out{method, target, 11};
    out.set(http::field::host, host); out.set(http::field::origin, "http://" + host);
    out.set(http::field::authorization, "Bearer secret");
    if (method != http::verb::delete_) out.set(http::field::content_type, "application/json");
    out.body() = body; out.prepare_payload(); return out;
  };
  for (const auto method : {http::verb::post, http::verb::delete_}) {
    const auto body = method == http::verb::post ? "{\"client_order_id\":\"socket\"}" : "";
    auto req = request(method, method == http::verb::post ? "/api/orders" : "/api/orders/1", body);
    http::write(connection, req);
    server::ApiCompletion complete;
    {
      std::unique_lock lock(mutex);
      ASSERT_TRUE(ready.wait_for(lock, std::chrono::minutes(5), [&] { return pending.has_value(); }));
      EXPECT_EQ(received->body, body);
      EXPECT_EQ(received->authorization, "Bearer secret");
      complete = std::move(*pending); pending.reset();
    }
    // A second request completes on the single I/O thread while the first is
    // waiting for its owner-thread callback. A blocking handler would deadlock.
    beast::tcp_stream other(io);
    other.expires_after(std::chrono::minutes(5));
    other.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    auto get = request(http::verb::get, "/api/status", "");
    http::write(other, get);
    beast::flat_buffer other_buffer;
    http::response<http::string_body> other_response;
    http::read(other, other_buffer, other_response);
    EXPECT_EQ(other_response.result_int(), 200);
    std::thread engine([complete = std::move(complete), method]() mutable {
      complete({method == http::verb::post ? 201 : 200, "{\"account_version\":\"3\"}"});
    });
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(connection, buffer, response);
    engine.join();
    EXPECT_EQ(response.result_int(), method == http::verb::post ? 201 : 200);
    EXPECT_EQ(response.body(), "{\"account_version\":\"3\"}");
  }
  auto rejected = request(http::verb::post, "/api/orders", "{}");
  rejected.erase(http::field::authorization);
  http::write(connection, rejected);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(connection, buffer, response);
  EXPECT_EQ(response.result_int(), 403);
  EXPECT_NE(response.body().find("WRITE_TOKEN_REQUIRED"), std::string::npos);
  web.stop();
}

TEST(WebServer, OnlyThePortfolioImportTakesABodyOverSixtyFourKiB) {
  std::mutex mutex;
  std::vector<std::size_t> received;
  server::WebServer web("127.0.0.1", 0, {},
      [&](const server::ApiRequest& request, server::ApiCompletion complete) {
        { const std::lock_guard lock(mutex); received.push_back(request.body.size()); }
        complete({200, "{}"});
      });
  web.start(1);
  asio::io_context io;
  const auto host = "127.0.0.1:" + std::to_string(web.port());
  const auto send = [&](const std::string& target, std::size_t size) {
    beast::tcp_stream connection(io);
    connection.expires_after(std::chrono::minutes(5));
    connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, host);
    request.set(http::field::content_type, "application/json");
    request.body() = "{\"csv\":\"" + std::string(size, 'x') + "\"}";
    request.prepare_payload();
    // A rejected body is answered before it is all read, so a write may see a broken pipe.
    beast::error_code ec;
    http::write(connection, request, ec);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(connection, buffer, response, ec);
    return response;
  };
  const auto imported = send("/api/analysis/portfolios", 1024 * 1024);
  EXPECT_EQ(imported.result_int(), 200);
  const auto order = send("/api/orders", 80 * 1024);
  EXPECT_EQ(order.result_int(), 400);
  EXPECT_NE(order.body().find("64 KiB"), std::string::npos);
  const auto other = send("/api/analysis/portfolios/ira", 80 * 1024);
  EXPECT_EQ(other.result_int(), 400);
  const std::lock_guard lock(mutex);
  ASSERT_EQ(received.size(), 1u);
  EXPECT_GT(received[0], 1024u * 1024);
  web.stop();
}

// A long read learns that nobody will take its response: the client hung up, or
// the server is stopping. Answering it still lets the connection carry on.
TEST(WebServer, AwaitedRequestsAreCancelledWhenTheClientLeavesOrTheServerStops) {
  std::mutex mutex;
  std::condition_variable ready;
  std::vector<std::pair<std::shared_ptr<const std::atomic_bool>, server::ApiCompletion>> pending;
  server::WebServer web("127.0.0.1", 0, {},
      [&](const server::ApiRequest& request, server::ApiCompletion complete) {
        const std::lock_guard lock(mutex);
        pending.emplace_back(request.cancelled, std::move(complete));
        ready.notify_all();
      });
  web.start(1);
  asio::io_context io;
  const auto send = [&](beast::tcp_stream& connection, bool connect = true) {
    if (connect) connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::empty_body> request{http::verb::get, "/api/underlyings/SPX/optimizer?target=1&budget=1", 11};
    request.set(http::field::host, "localhost");
    http::write(connection, request);
  };
  const auto await = [&](std::size_t count) {
    std::unique_lock lock(mutex);
    return ready.wait_for(lock, std::chrono::seconds(10), [&] { return pending.size() >= count; });
  };
  const auto turns_true = [](const std::shared_ptr<const std::atomic_bool>& flag) {
    for (int i = 0; i < 1000 && !flag->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return flag->load();
  };
  // A client that waits keeps its request alive, and gets its answer.
  beast::tcp_stream staying(io);
  send(staying);
  ASSERT_TRUE(await(1));
  ASSERT_TRUE(pending[0].first);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(pending[0].first->load());
  pending[0].second({200, "{}"});
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(staying, buffer, response);
  EXPECT_EQ(response.result_int(), 200);
  EXPECT_FALSE(pending[0].first->load());
  // One that hangs up cancels it.
  {
    beast::tcp_stream leaving(io);
    send(leaving);
    ASSERT_TRUE(await(2));
    beast::error_code ignored;
    leaving.socket().close(ignored);
  }
  EXPECT_TRUE(turns_true(pending[1].first));
  // Stopping the server cancels what is still awaited.
  send(staying, false);
  ASSERT_TRUE(await(3));
  EXPECT_FALSE(pending[2].first->load());
  web.stop();
  EXPECT_TRUE(pending[2].first->load());
  for (auto& [flag, complete] : pending) EXPECT_NO_THROW(complete({200, "{}"}));
}

TEST(WebServer, StopSeversLateCommandCompletionBeforeDestroyingExecutor) {
  server::ApiCompletion complete;
  std::promise<void> received;
  asio::io_context io;
  beast::tcp_stream connection(io);
  {
    server::WebServer web("127.0.0.1", 0, {},
        [&](const server::ApiRequest&, server::ApiCompletion callback) {
          complete = std::move(callback); received.set_value();
        });
    web.start();
    connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::string_body> request{http::verb::post, "/api/risk/kill", 11};
    request.set(http::field::host, "localhost");
    request.set(http::field::content_type, "application/json");
    request.body() = "{}"; request.prepare_payload();
    http::write(connection, request);
    ASSERT_EQ(received.get_future().wait_for(std::chrono::minutes(5)), std::future_status::ready);
    web.stop();
  }
  EXPECT_NO_THROW(complete({200, "{}"}));
}
}  // namespace

namespace {
TEST(WebPolicy, NamedTokenFilesAreStrictAndDiagnosticsHideSecrets) {
  const auto tokens = server::parse_token_file("# agents\nreader read read-secret\nagent read,trade:practice,replay agent-secret # local\nowner admin admin-secret\n");
  ASSERT_EQ(tokens.size(), 3U);
  EXPECT_EQ(tokens[1].name, "agent");
  EXPECT_EQ(tokens[1].scopes, (std::vector<std::string>{"read", "trade:practice", "replay"}));
  // A '#' inside a secret is part of it; a comment starts a field.
  const auto hashed = server::parse_token_file("ops admin x#Gk2-long-secret #comment\n");
  ASSERT_EQ(hashed.size(), 1U);
  EXPECT_EQ(hashed[0].secret, "x#Gk2-long-secret");
  for (const std::string line : {"", "# empty", "a read", "a read secret extra", "a trade: secret", "a read, secret",
      "a unknown secret", "a read,read secret", "system admin secret", "loopback admin secret", "a read secret\na admin other",
      "a read secret\nb admin secret", "a trade:../main secret", "a trade:MAIN secret", "a trade:bad_id secret", "a trade:-main secret",
      "a trade:main--two secret", "a read secret\nb ,read other"}) {
    try { (void)server::parse_token_file(line); FAIL() << "Accepted malformed token file"; }
    catch (const std::invalid_argument& error) {
      EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
    }
  }
}

TEST_F(StaticFiles, TokenReloadKeepsGoodCredentialsAndAtomicallyRevokesRemovedOnes) {
  const auto path = parent / "tokens";
  const auto write = [&](std::string_view text) { std::ofstream(path) << text; };
  write("owner admin owner-secret\nscript read,trade:main script-secret\n");
  const auto file = std::make_shared<server::TokenFile>(path, "legacy-secret");
  server::WritePolicy policy{"0.0.0.0", "legacy-secret", {}, {}, true, {}, {}, file};
  const auto check = [&](const char* secret) {
    server::ApiRequest request{"POST", "/api/orders"};
    request.content_type = "application/json";
    request.authorization = std::string("Bearer ") + secret;
    return server::check_api_write(request, policy);
  };
  EXPECT_FALSE(check("script-secret"));
  const auto initial = file->snapshot();
  write("owner admin owner-secret\nscript invalid leaked-secret\n");
  const auto bad = file->reload();
  EXPECT_EQ(bad.status, 400);
  EXPECT_NE(bad.body.find("line 2"), std::string::npos);
  EXPECT_EQ(bad.body.find("leaked-secret"), std::string::npos);
  EXPECT_EQ(file->snapshot(), initial);
  EXPECT_FALSE(check("script-secret"));
  fs::remove(path);
  EXPECT_EQ(file->reload().status, 400);
  EXPECT_FALSE(check("script-secret"));
  write("owner admin owner-secret\n");
  EXPECT_TRUE(check("script-secret"));  // stat detects replacement before this request
  EXPECT_FALSE(check("owner-secret"));
  EXPECT_FALSE(file->valid(initial->tokens.back()));
  EXPECT_EQ(file->reload().body.find("owner-secret"), std::string::npos);
  write("collision admin legacy-secret\n");
  EXPECT_EQ(file->reload().status, 400);
  EXPECT_FALSE(check("owner-secret"));
  write("# revoke every named token\n");
  EXPECT_EQ(file->reload().status, 200);
  EXPECT_TRUE(check("owner-secret"));
  EXPECT_FALSE(check("legacy-secret"));
  EXPECT_EQ(server::write_mode(policy), "token");
  EXPECT_TRUE(file->snapshot()->tokens.empty());
  write("new read replacement-secret\n");
  EXPECT_EQ(file->snapshot()->tokens.front().name, "new");
}

TEST_F(StaticFiles, TokenReloadRouteChecksAdminAndRevokesExistingWebSockets) {
  const auto path = parent / "tokens";
  std::ofstream(path) << "owner admin owner-secret\nreader read reader-secret\n";
  const auto file = std::make_shared<server::TokenFile>(path);
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{200, "{}"}; },
      {}, "legacy-secret", {}, {}, true, {}, {}, file);
  web.start(2);
  asio::io_context io;
  const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), web.port());
  const auto call = [&](http::verb method, const std::string& target, const std::string& secret, std::string body = "{}") {
    tcp::socket socket(io); socket.connect(endpoint);
    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, "localhost");
    request.set(http::field::authorization, "Bearer " + secret);
    request.set(http::field::content_type, "application/json");
    request.body() = std::move(body); request.prepare_payload();
    http::write(socket, request);
    beast::flat_buffer buffer; http::response<http::string_body> response;
    http::read(socket, buffer, response);
    return response;
  };
  EXPECT_EQ(call(http::verb::post, "/api/tokens/reload", "reader-secret").result_int(), 403U);
  EXPECT_EQ(call(http::verb::post, "/api/tokens/reload", "owner-secret", R"({"bad":1})").result_int(), 400U);
  const auto good = call(http::verb::post, "/api/tokens/reload", "owner-secret");
  EXPECT_EQ(good.result_int(), 200U);
  EXPECT_EQ(nlohmann::json::parse(good.body())["names"], (nlohmann::json{"owner", "reader"}));
  EXPECT_EQ(good.body().find("secret"), std::string::npos);
  const auto status = nlohmann::json::parse(call(http::verb::get, "/api/status", "owner-secret").body());
  EXPECT_EQ(status["tokens"]["count"], 2);
  EXPECT_FALSE(status["tokens"].contains("names"));
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect(endpoint);
  ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
    req.set(http::field::authorization, "Bearer reader-secret");
  }));
  ws.handshake("localhost", "/ws");
  // Wait for the pong: sending a ping alone can race the server's accept handler
  // and drop the broadcast before the socket has been registered in the hub.
  ws.control_callback([&](websocket::frame_type kind, beast::string_view) {
    if (kind == websocket::frame_type::pong) web.broadcast("before");
  });
  ws.ping({});
  beast::flat_buffer received;
  ws.read(received);
  ws.control_callback({});
  EXPECT_EQ(beast::buffers_to_string(received.data()), "before");
  std::ofstream(path) << "owner admin owner-secret\n";
  EXPECT_EQ(call(http::verb::post, "/api/tokens/reload", "owner-secret").result_int(), 200U);
  EXPECT_EQ(call(http::verb::get, "/api/status", "reader-secret").result_int(), 403U);
  web.broadcast("must-not-arrive");
  received.consume(received.size());
  beast::error_code error;
  ws.read(received, error);
  EXPECT_TRUE(error);
  EXPECT_EQ(received.size(), 0U);
  EXPECT_EQ(call(http::verb::get, "/api/status", "legacy-secret").result_int(), 200U);
  web.stop();
}

TEST(WebPolicy, NamedTokensEnforceEveryRouteFamilyAndAccount) {
  server::WritePolicy policy{"0.0.0.0", "legacy-secret", {},
      server::parse_token_file("reader read reader-secret\nagent trade:practice agent-secret\nall trade:* all-secret\nreplayer replay replay-secret\nowner admin owner-secret"), true};
  const auto check = [&](std::string method, std::string path, const std::string& secret) {
    server::ApiRequest request{std::move(method), std::move(path)};
    request.content_type = "application/json";
    request.authorization = "Bearer " + secret;
    return server::check_api_write(request, policy);
  };
  for (const auto* path : {"/api/status", "/api/calendar/events?from=2026-09-22", "/api/accounts", "/api/portfolio?account=practice", "/api/replay", "/ws", "/api/volatility/scanner"}) {
    EXPECT_FALSE(check("GET", path, "reader-secret"));
    EXPECT_TRUE(check("GET", path, "agent-secret"));
    EXPECT_FALSE(check("GET", path, "owner-secret"));
  }
  for (const auto* method : {"PATCH", "DELETE"}) {
    EXPECT_TRUE(check(method, "/api/accounts/practice", "agent-secret"));
    EXPECT_TRUE(check(method, "/api/accounts/practice", "replay-secret"));
    EXPECT_FALSE(check(method, "/api/accounts/practice", "owner-secret"));
  }
  const std::vector<std::pair<std::string, std::string>> trades = {
      {"POST", "/alerts"}, {"DELETE", "/alerts/1"}, {"POST", "/orders"}, {"POST", "/orders/preview"}, {"PUT", "/orders/1"}, {"DELETE", "/orders/1"},
      {"POST", "/orders/cancel"}, {"POST", "/positions/close"}, {"POST", "/positions/exercise"},
      {"POST", "/stocks/close"}, {"POST", "/stocks/trade"}, {"POST", "/stocks/trade/preview"}, {"POST", "/positions/abandon"}, {"POST", "/positions/instruction"},
      {"PUT", "/trades/1/note"}, {"PUT", "/days/2026-09-22/note"},
      {"POST", "/playbooks/staged/12/send"}, {"POST", "/playbooks/staged/12/dismiss"}};
  for (const auto& [method, path] : trades) {
    EXPECT_FALSE(check(method, "/api" + path + "?account=practice", "agent-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path, "agent-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path + "?account=other", "agent-secret")) << path;
    EXPECT_FALSE(check(method, "/api" + path + "?account=other", "all-secret")) << path;
    EXPECT_TRUE(check(method, "/api/replay" + path, "all-secret")) << path;
    EXPECT_FALSE(check(method, "/api/replay" + path, "replay-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path, "replay-secret")) << path;
    EXPECT_TRUE(check(method, "/api" + path, "reader-secret")) << path;
  }
  for (const auto& [method, path] : std::vector<std::pair<std::string, std::string>>{
      {"POST", "/api/replay/history/run/verify"}, {"DELETE", "/api/replay/history/run"}, {"POST", "/api/replay/packs"}}) {
    EXPECT_FALSE(check(method, path, "replay-secret"));
    EXPECT_TRUE(check(method, path, "reader-secret"));
    EXPECT_TRUE(check(method, path, "agent-secret"));
  }
  EXPECT_FALSE(check("GET", "/api/replay/history/run/verify?format=receipt", "reader-secret"));
  for (const std::string method : {"POST", "PUT", "DELETE"}) {
    EXPECT_FALSE(check(method, "/api/replay", "replay-secret"));
    EXPECT_TRUE(check(method, "/api/replay", "agent-secret"));
  }
  for (const auto* path : {"/api/risk/limits", "/api/risk/guardrails", "/api/risk/kill", "/api/account/reset",
       "/api/account/payout", "/api/accounts", "/api/settlements", "/api/replay/risk/limits", "/api/replay/history/run", "/api/future-write",
       "/api/playbooks", "/api/playbooks/morning/mode"}) {
    EXPECT_TRUE(check("POST", path, "agent-secret")) << path;
    EXPECT_TRUE(check("POST", path, "replay-secret")) << path;
    EXPECT_FALSE(check("POST", path, "owner-secret")) << path;
    EXPECT_FALSE(check("POST", path, "legacy-secret")) << path;
  }
}

TEST(WebPolicy, AnalysisPortfoliosRequireAdminEvenToRead) {
  const auto tokens = server::parse_token_file("reader read reader-secret\nall trade:* all-secret\nowner admin owner-secret");
  const auto check = [](const server::WritePolicy& policy, std::string method, std::string path, const std::string& secret) {
    server::ApiRequest request{std::move(method), std::move(path)};
    request.content_type = "application/json";
    if (!secret.empty()) request.authorization = "Bearer " + secret;
    return server::check_api_write(request, policy);
  };
  const std::vector<std::string> routes{"/api/analysis/portfolios", "/api/analysis/portfolios/ira",
      "/api/analysis/portfolios/ira/risk/profile?underlying=SPY", "/api/analysis/portfolios/ira/risk/grid"};
  // Reads are public on this server, and stay so for market data, but not for outside holdings.
  const server::WritePolicy open_reads{"0.0.0.0", "", {}, tokens};
  EXPECT_FALSE(check(open_reads, "GET", "/api/status", ""));
  EXPECT_FALSE(check(open_reads, "GET", "/api/status", "stale-secret"));
  for (const auto& route : routes) {
    for (const auto* secret : {"", "stale-secret", "reader-secret", "all-secret"}) {
      const auto rejected = check(open_reads, "GET", route, secret);
      ASSERT_TRUE(rejected) << route << " " << secret;
      EXPECT_EQ(rejected->status, 403);
    }
    EXPECT_FALSE(check(open_reads, "GET", route, "owner-secret")) << route;
  }
  EXPECT_TRUE(check(open_reads, "POST", "/api/analysis/portfolios", "reader-secret"));
  EXPECT_FALSE(check(open_reads, "POST", "/api/analysis/portfolios", "owner-secret"));
  EXPECT_TRUE(check(open_reads, "DELETE", "/api/analysis/portfolios/ira", "all-secret"));
  EXPECT_FALSE(check(open_reads, "DELETE", "/api/analysis/portfolios/ira", "owner-secret"));
  // A prefix that is not the route is not caught by it.
  EXPECT_FALSE(check(open_reads, "GET", "/api/analysis/portfoliosx", ""));
  // With --require-token, too.
  const server::WritePolicy required{"0.0.0.0", "", {}, tokens, true};
  EXPECT_TRUE(check(required, "GET", routes[1], "reader-secret"));
  EXPECT_FALSE(check(required, "GET", routes[1], "owner-secret"));
  // A loopback server whose writes need no credentials already gives every request admin.
  const server::WritePolicy loopback{"127.0.0.1", "", {}};
  EXPECT_EQ(server::write_mode(loopback), "open");
  for (const auto& route : routes) EXPECT_FALSE(check(loopback, "GET", route, "")) << route;
  const server::WritePolicy guarded{"127.0.0.1", "legacy-secret", {}};
  EXPECT_TRUE(check(guarded, "GET", routes[0], ""));
  EXPECT_FALSE(check(guarded, "GET", routes[0], "legacy-secret"));
}

TEST(WebPolicy, ReplayScopeConfiguresOnlyTheIsolatedAccount) {
  server::WritePolicy policy{"0.0.0.0", "", {},
      server::parse_token_file("run replay run-secret\ntrader trade:* trade-secret\nowner admin owner-secret"), true};
  for (const auto& [method, path] : std::vector<std::pair<std::string, std::string>>{
      {"POST", "/account/reset"}, {"PUT", "/risk/limits"}, {"PUT", "/risk/guardrails"}, {"POST", "/risk/kill"}}) {
    for (const auto* secret : {"run-secret", "trade-secret", "owner-secret"}) {
      for (const bool replay : {false, true}) {
        server::ApiRequest request{method, std::string(replay ? "/api/replay" : "/api") + path};
        request.content_type = "application/json";
        request.authorization = std::string("Bearer ") + secret;
        const auto rejected = server::check_api_write(request, policy);
        const bool permitted = std::string_view(secret) == "owner-secret" || (replay && std::string_view(secret) == "run-secret");
        EXPECT_EQ(!rejected, permitted) << request.target << secret;
        if (rejected) { EXPECT_EQ(rejected->status, 403); EXPECT_NE(rejected->body.find("SCOPE_REQUIRED"), std::string::npos); }
      }
    }
  }
}

TEST(WebPolicy, EverySpellingOfTheAccountIsScopedAsTheRoutesReadIt) {
  // Routes percent-decode query keys and values (%61 is "a"), so the scope check reads
  // the account through the same parser and cannot be sidestepped by encoding it.
  server::WritePolicy policy{"0.0.0.0", "", {}, server::parse_token_file("mainer trade:main mainer-secret\nbeta trade:beta beta-secret"), false};
  const auto check = [&](std::string method, std::string path, const std::string& secret) {
    server::ApiRequest request{std::move(method), std::move(path)};
    request.content_type = "application/json";
    request.authorization = "Bearer " + secret;
    return server::check_api_write(request, policy);
  };
  for (const std::string path : {"/api/playbooks/staged/1/send", "/api/orders", "/api/positions/close"}) {
    for (const auto* query : {"?account=beta", "?%61ccount=beta", "?%61%63%63ount=beta", "?account=%62eta", "?%61ccount=%62%65%74%61", "?x=1&%61ccount=beta"}) {
      const auto refused = check("POST", path + query, "mainer-secret");
      ASSERT_TRUE(refused) << path << query;
      EXPECT_EQ(refused->status, 403) << path << query;
      EXPECT_FALSE(check("POST", path + query, "beta-secret")) << path << query;
    }
    // Two spellings of one key are ambiguous whichever comes first, and a malformed
    // query or account cannot fall back to the main account.
    for (const auto* query : {"?account=main&%61ccount=beta", "?%61ccount=beta&account=main", "?account=main&account=beta",
                              "?%zz=1&account=main", "?account=%6", "?account=Beta", "?account=", "?account"}) {
      const auto refused = check("POST", path + query, "mainer-secret");
      ASSERT_TRUE(refused) << path << query;
      EXPECT_EQ(refused->status, 400) << path << query;
    }
    EXPECT_FALSE(check("POST", path + "?%61ccount=main", "mainer-secret")) << path;
  }
  EXPECT_EQ(server::query_account("/api/orders?%61ccount=%62eta"), "beta");
  EXPECT_EQ(server::query_account("/api/orders"), "main");
  EXPECT_EQ(server::query_account("/api/orders?status=open"), "main");
  EXPECT_FALSE(server::query_account("/api/orders?account=main&%61ccount=main"));
  const auto form = server::query_parameters("template=%7B%22name%22%3A%22iron+condor%22%7D&flag", true);
  ASSERT_TRUE(form);
  EXPECT_EQ(form->at("template"), R"({"name":"iron condor"})");
  EXPECT_EQ(form->at("flag"), "");
  EXPECT_EQ(server::query_parameters("from=2026-09-28T09:30:00+00:00")->at("from"), "2026-09-28T09:30:00+00:00");
  EXPECT_EQ(server::query_parameters("from=2026-09-28T13%3A30%3A00Z")->at("from"), "2026-09-28T13:30:00Z");
}

TEST(WebPolicy, WriteModeSaysWhetherAWriteWithoutCredentialsIsAccepted) {
  const auto tokens = server::parse_token_file("reader read reader-secret");
  server::Sandboxes::Options limits;
  limits.capacity = 1;
  const auto sandboxes = std::make_shared<server::Sandboxes>(limits);
  server::ApiRequest order{"POST", "/api/orders"};
  order.content_type = "application/json";
  const std::vector<std::pair<server::WritePolicy, std::string>> cases{
      {{"127.0.0.1", "", {}}, "open"},
      {{"127.0.0.1", "", {}, tokens}, "open"},  // named tokens alone leave loopback writes open
      {{"127.0.0.1", "legacy-secret", {}}, "token"},
      {{"127.0.0.1", "", {}, tokens, true}, "token"},
      {{"127.0.0.1", "", {}, {}, false, sandboxes}, "token"},
      {{"0.0.0.0", "", {}}, "disabled"},
      {{"0.0.0.0", "", {}, tokens}, "token"},
      {{"0.0.0.0", "legacy-secret", {}}, "token"}};
  for (const auto& [policy, mode] : cases) {
    EXPECT_EQ(server::write_mode(policy), mode) << policy.address << " token=" << policy.token << " named=" << policy.tokens.size();
    // A terminal reading "open" writes without a token; any other mode needs one or refuses.
    EXPECT_EQ(!server::check_api_write(order, policy), mode == "open") << policy.address << " " << mode;
  }
}

TEST(WebPolicy, RequireTokenProtectsLoopbackReadsAndWritesAndAttributesActors) {
  server::WritePolicy policy{"127.0.0.1", "", {}, server::parse_token_file("alice read,trade:main secret"), false};
  server::ApiRequest request{"POST", "/api/orders"};
  request.content_type = "application/json";
  std::string actor;
  EXPECT_FALSE(server::check_api_write(request, policy, &actor));
  EXPECT_EQ(actor, "loopback");
  policy.require_token = true;
  EXPECT_TRUE(server::check_api_write(request, policy));
  request.authorization = "Bearer secret";
  EXPECT_FALSE(server::check_api_write(request, policy, &actor));
  EXPECT_EQ(actor, "alice");
  request.method = "GET";
  request.target = "/api/status";
  EXPECT_FALSE(server::check_api_write(request, policy));
  request.authorization.clear();
  EXPECT_TRUE(server::check_api_write(request, policy));
  request.authorization = "Bearer wrong";
  EXPECT_TRUE(server::check_api_write(request, policy));
  // Without --require-token reads stay public: a stale or narrow token still reads,
  // and only its writes are refused.
  policy.require_token = false;
  EXPECT_FALSE(server::check_api_write(request, policy));
  request.authorization.clear();
  EXPECT_FALSE(server::check_api_write(request, policy));
  request.method = "POST";
  request.target = "/api/orders";
  request.authorization = "Bearer wrong";
  EXPECT_TRUE(server::check_api_write(request, policy));
  policy.tokens = server::parse_token_file("agent trade:practice agent-secret");
  request.method = "GET";
  request.target = "/api/portfolio";
  request.authorization = "Bearer agent-secret";
  EXPECT_FALSE(server::check_api_write(request, policy));
}

TEST(WebPolicy, BrowserSocketTokensUseHeadersWithoutAmbiguousCredentials) {
  EXPECT_EQ(server::websocket_authorization("", "openport, openport.token.616263"), "Bearer abc");
  EXPECT_EQ(server::websocket_authorization("Bearer abc", ""), "Bearer abc");
  EXPECT_FALSE(server::websocket_authorization("Bearer abc", "openport, openport.token.616263"));
  // Legacy tokens may contain a space or UTF-8, as the terminal hex-encodes them.
  EXPECT_EQ(server::websocket_authorization("", "openport, openport.token.636f727265637420686f727365"), "Bearer correct horse");
  EXPECT_EQ(server::websocket_authorization("", "openport, openport.token.636166c3a9"), "Bearer caf\xc3\xa9");
  for (const auto* value : {"openport, openport.token.", "openport, openport.token.0", "openport, openport.token.00", "openport, openport.token.gg",
                            "openport, openport.token.610a62", "openport, openport.token.617f", "other"}) {
    EXPECT_FALSE(server::websocket_authorization("", value));
  }
}
}  // namespace

TEST(WebServer, EventChannelIsVersionedFilteredAndDoesNotCoalesce) {
  server::Notifications::Options options;
  options.worker = false;
  auto events = std::make_shared<server::Notifications>(server::NotificationConfig{}, nullptr, options);
  server::WebServer web("127.0.0.1", 0, {}, [](const auto&) { return server::ApiResponse{}; });
  web.set_event_stream(events);
  web.start();
  asio::io_context io;
  websocket::stream<tcp::socket> ws(io);
  ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
  ws.handshake("localhost", "/ws/events?version=1&account=practice");
  const auto read = [&] {
    beast::flat_buffer buffer;
    ws.read(buffer);
    return nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
  };
  EXPECT_EQ(read().at("type"), "event_stream");
  events->publish({"fill", "main", 0, "Other account", {}});
  events->publish({"fill", "replay/run/main", 0, "Other source", {}});
  for (int i = 0; i < 20; ++i) events->publish({"fill", "demo/practice", 0, "Synthetic", {{"id", i}}});
  web.broadcast("{\"type\":\"tick\"}");
  for (int i = 0; i < 20; ++i) {
    const auto event = read();
    EXPECT_EQ(event.at("type"), "event");
    EXPECT_EQ(event.at("details").at("id"), i);
  }
  ws.close(websocket::close_code::normal);
  websocket::stream<tcp::socket> invalid(io);
  invalid.next_layer().connect({asio::ip::make_address("127.0.0.1"), web.port()});
  beast::error_code error;
  websocket::response_type upgrade;
  invalid.handshake(upgrade, "localhost", "/ws/events?version=2", error);
  EXPECT_TRUE(error);
  EXPECT_EQ(upgrade.result(), http::status::bad_request);
  web.stop();
}

namespace {
std::string hmac_hex(std::string_view key, std::string_view message) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int length = 0;
  HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(message.data()), message.size(), hash.data(), &length);
  std::string text;
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned int i = 0; i < length; ++i) { text += hex[hash[i] >> 4]; text += hex[hash[i] & 15]; }
  return text;
}
/// Signs as any sender would: standard HMAC-SHA256 keyed by the secret text itself.
server::ApiRequest signed_signal(const std::string& secret, std::string target, std::int64_t timestamp, std::string nonce) {
  server::ApiRequest request{"POST", std::move(target), nlohmann::json{{"action", "evaluate"}, {"nonce", std::move(nonce)}}.dump()};
  request.signal_timestamp = std::to_string(timestamp);
  request.signal_signature = "sha256=" + hmac_hex(secret, request.signal_timestamp + "\nPOST\n" + request.target + "\n" + request.body);
  return request;
}
server::ApiRequest static_signal(const std::string& secret, std::string target, std::string nonce, bool secure = true) {
  server::ApiRequest request{"POST", std::move(target),
      nlohmann::json{{"action", "evaluate"}, {"nonce", std::move(nonce)}, {"secret", secret}}.dump()};
  request.signal_secure = secure;
  return request;
}
std::string issued(server::SignalSecrets& secrets, const std::string& playbook, const std::string& account) {
  const auto response = secrets.issue(playbook, account);
  EXPECT_EQ(response.status, 201) << response.body;
  return nlohmann::json::parse(response.body).at("secret").get<std::string>();
}
struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("openport-signals-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TemporaryDirectory() { fs::create_directories(path); }
  ~TemporaryDirectory() { fs::remove_all(path); }
};
}  // namespace

TEST(SignalSecrets, IssueRotateRevokeAndPersistOnlyDigests) {
  TemporaryDirectory directory;
  const auto file = directory.path / "signal-secrets.json";
  md::Timestamp now = 1'800'000'000 * md::kNanosPerSecond;
  std::string first;
  {
    server::SignalSecrets secrets(file, [&] { return now; });
    EXPECT_EQ(nlohmann::json::parse(secrets.describe("morning", "main").body).at("configured"), false);
    first = issued(secrets, "morning", "main");
    EXPECT_TRUE(first.starts_with("opsig_"));
    EXPECT_EQ(first.size(), 70U);  // 32 random bytes as hex, longer than the HMAC block
    EXPECT_EQ(secrets.issue("Bad ID", "main").status, 400);
  }
  std::string text((std::istreambuf_iterator<char>(std::ifstream(file).rdbuf())), {});
  EXPECT_EQ(text.find(first), std::string::npos);
  EXPECT_EQ(fs::status(file).permissions() & fs::perms::all, fs::perms::owner_read | fs::perms::owner_write);
  server::SignalSecrets reloaded(file, [&] { return now + md::kNanosPerSecond; });
  const auto described = nlohmann::json::parse(reloaded.describe("morning", "main").body);
  EXPECT_EQ(described.at("configured"), true);
  EXPECT_FALSE(described.contains("secret"));
  EXPECT_FALSE(reloaded.check(static_signal(first, "/api/signals/morning", "loaded")));
  const auto rotated = nlohmann::json::parse(reloaded.issue("morning", "main").body);
  EXPECT_EQ(rotated.at("rotated"), true);
  const auto second = rotated.at("secret").get<std::string>();
  EXPECT_EQ(reloaded.check(static_signal(first, "/api/signals/morning?account=main", "old"))->status, 403);
  EXPECT_FALSE(reloaded.check(static_signal(second, "/api/signals/morning?account=main", "new")));
  EXPECT_EQ(reloaded.revoke("morning", "main").status, 200);
  EXPECT_EQ(reloaded.revoke("morning", "main").status, 404);
  EXPECT_EQ(reloaded.check(static_signal(second, "/api/signals/morning", "revoked"))->status, 403);
  EXPECT_EQ(nlohmann::json::parse(server::SignalSecrets(file).describe("morning", "main").body).at("configured"), false);
  std::ofstream(file) << "{\"schema\":2}";
  EXPECT_THROW(server::SignalSecrets{file}, std::runtime_error);
}

TEST(SignalSecrets, HmacBindsOnePlaybookAccountTimestampAndBodyWithinFiveMinutes) {
  md::Timestamp now = 1'800'000'000 * md::kNanosPerSecond;
  const std::int64_t seconds = 1'800'000'000;
  server::SignalSecrets secrets({}, [&] { return now; });
  const auto secret = issued(secrets, "morning", "practice");
  const auto other = issued(secrets, "morning", "main");
  const std::string target = "/api/signals/morning?account=practice";
  now += 10 * md::kNanosPerSecond;
  EXPECT_FALSE(secrets.check(signed_signal(secret, target, seconds + 10, "one")));
  EXPECT_EQ(secrets.check(signed_signal(secret, target, seconds + 10, "one"))->status, 409);  // replay
  // Each secret authorises only its own playbook and account.
  EXPECT_EQ(secrets.check(signed_signal(secret, "/api/signals/morning?account=main", seconds + 10, "two"))->status, 403);
  EXPECT_EQ(secrets.check(signed_signal(secret, "/api/signals/evening?account=practice", seconds + 10, "two"))->status, 403);
  EXPECT_EQ(secrets.check(signed_signal(other, target, seconds + 10, "two"))->status, 403);
  auto tampered = signed_signal(secret, target, seconds + 10, "two");
  tampered.body += " ";
  EXPECT_EQ(tampered.signal_signature.size(), 71U);
  EXPECT_EQ(secrets.check(tampered)->status, 403);
  tampered = signed_signal(secret, target, seconds + 10, "two");
  tampered.signal_timestamp = std::to_string(seconds + 11);
  EXPECT_EQ(secrets.check(tampered)->status, 403);
  tampered = signed_signal(secret, target, seconds + 10, "two");
  tampered.method = "PUT";
  EXPECT_EQ(secrets.check(tampered)->status, 405);
  // A sender clock ahead or behind by up to 300 seconds is accepted.
  EXPECT_FALSE(secrets.check(signed_signal(secret, target, seconds + 310, "ahead")));
  EXPECT_EQ(secrets.check(signed_signal(secret, target, seconds + 311, "too-far-ahead"))->status, 409);
  now += 300 * md::kNanosPerSecond;
  EXPECT_FALSE(secrets.check(signed_signal(secret, target, seconds + 10, "behind")));
  EXPECT_EQ(secrets.check(signed_signal(secret, target, seconds + 9, "too-old"))->status, 409);
  auto bare = signed_signal(secret, target, seconds + 310, "bare-hex");
  bare.signal_signature.erase(0, 7);
  EXPECT_FALSE(secrets.check(bare));
  auto missing = signed_signal(secret, target, seconds + 310, "missing");
  missing.signal_timestamp.clear();
  EXPECT_EQ(secrets.check(missing)->status, 400);
  auto with_secret = signed_signal(secret, target, seconds + 310, "body");
  with_secret.body = nlohmann::json{{"action", "evaluate"}, {"nonce", "body"}, {"secret", secret}}.dump();
  with_secret.signal_signature = hmac_hex(secret, with_secret.signal_timestamp + "\nPOST\n" + target + "\n" + with_secret.body);
  EXPECT_EQ(secrets.check(with_secret)->status, 400);
  // Nonces are per secret, and a restart refuses timestamps from before it started.
  EXPECT_FALSE(secrets.check(signed_signal(other, "/api/signals/morning?account=main", seconds + 310, "one")));
  server::SignalSecrets restarted({}, [&] { return now; });
  const auto fresh = issued(restarted, "morning", "practice");
  EXPECT_EQ(restarted.check(signed_signal(fresh, target, seconds + 309, "before-start"))->status, 409);
  EXPECT_FALSE(restarted.check(signed_signal(fresh, target, seconds + 310, "at-start")));
}

TEST(SignalSecrets, StaticBodySecretNeedsSecureTransportDedupeIdAndRateBudget) {
  md::Timestamp now = 1'800'000'000 * md::kNanosPerSecond;
  server::SignalSecrets secrets({}, [&] { return now; });
  const auto secret = issued(secrets, "morning", "main");
  const std::string target = "/api/signals/morning?account=main";
  const auto insecure = secrets.check(static_signal(secret, target, "plain", false));
  ASSERT_TRUE(insecure); EXPECT_EQ(insecure->status, 403);
  EXPECT_NE(insecure->body.find("SIGNAL_TLS_REQUIRED"), std::string::npos);
  EXPECT_EQ(secrets.check(static_signal(secret + "0", target, "wrong"))->status, 403);
  EXPECT_EQ(secrets.check(static_signal(secret, target, ""))->status, 400);
  auto no_id = static_signal(secret, target, "x");
  no_id.body = nlohmann::json{{"action", "evaluate"}, {"secret", secret}}.dump();
  EXPECT_EQ(secrets.check(no_id)->status, 400);
  EXPECT_EQ(secrets.check(static_signal(secret, target + "&extra=1", "query"))->status, 400);
  EXPECT_FALSE(secrets.check(static_signal(secret, target, "SPY-2026-10-21T14:30:00Z")));
  EXPECT_EQ(secrets.check(static_signal(secret, target, "SPY-2026-10-21T14:30:00Z"))->status, 409);
  for (int i = 1; i < 60; ++i) EXPECT_FALSE(secrets.check(static_signal(secret, target, std::to_string(i))));
  const auto limited = secrets.check(static_signal(secret, target, "over"));
  ASSERT_TRUE(limited); EXPECT_EQ(limited->status, 429); EXPECT_EQ(limited->retry_after, 60);
  now += 61 * md::kNanosPerSecond;
  EXPECT_FALSE(secrets.check(static_signal(secret, target, "over")));
  // Static dedupe IDs outlive the HMAC window: a retried alert an hour later is refused.
  now += 3600 * md::kNanosPerSecond;
  EXPECT_EQ(secrets.check(static_signal(secret, target, "over"))->status, 409);
}

TEST(WebPolicy, SignalRoutesSeparateWebhookSecretsFromBearerEvaluation) {
  server::WritePolicy policy{"0.0.0.0", "", {},
      server::parse_token_file("agent trade:practice agent-secret\nreader read reader-secret\nowner admin owner-secret"), true};
  const auto check = [&](std::string method, std::string target, const std::string& secret, std::string* actor = nullptr) {
    server::ApiRequest request{method, std::move(target), method == "DELETE" ? "" : "{}"};
    request.content_type = "application/json";
    if (!secret.empty()) request.authorization = "Bearer " + secret;
    return server::check_api_write(request, policy, actor);
  };
  std::string actor;
  EXPECT_FALSE(check("POST", "/api/signals/morning?account=practice", "", &actor));
  EXPECT_EQ(actor, "signal");
  const auto bearer = check("POST", "/api/signals/morning?account=practice", "agent-secret");
  ASSERT_TRUE(bearer); EXPECT_NE(bearer->body.find("SIGNAL_BEARER_REJECTED"), std::string::npos);
  EXPECT_FALSE(check("POST", "/api/playbooks/morning/evaluate?account=practice", "agent-secret"));
  EXPECT_EQ(check("POST", "/api/playbooks/morning/evaluate?account=main", "agent-secret")->status, 403);
  EXPECT_EQ(check("POST", "/api/playbooks/morning/evaluate?account=practice", "")->status, 403);
  EXPECT_EQ(check("POST", "/api/replay/playbooks/morning/evaluate", "owner-secret")->status, 403);
  for (const auto* method : {"GET", "POST", "DELETE"}) {
    EXPECT_EQ(check(method, "/api/playbooks/morning/signal-secret?account=practice", "agent-secret")->status, 403);
    EXPECT_EQ(check(method, "/api/playbooks/morning/signal-secret?account=practice", "reader-secret")->status, 403);
    EXPECT_FALSE(check(method, "/api/playbooks/morning/signal-secret?account=practice", "owner-secret"));
  }
  policy.require_token = false;  // Public reads are labelled admin but cannot see secret status.
  EXPECT_EQ(check("GET", "/api/playbooks/morning/signal-secret", "")->status, 403);
}

TEST(WebServer, SignalSecretRoutesAndWebhookTransportHeaders) {
  TemporaryDirectory directory;
  std::mutex mutex;
  std::vector<server::ApiRequest> received;
  server::WebServer web("127.0.0.1", 0, {}, [&](const server::ApiRequest& request) {
    const std::lock_guard lock(mutex);
    received.push_back(request);
    return server::ApiResponse{200, "{}"};
  });
  web.set_signal_secrets(std::make_shared<server::SignalSecrets>(directory.path / "signal-secrets.json"));
  web.start();
  const auto host = "127.0.0.1:" + std::to_string(web.port());
  const auto send = [&](http::verb method, const std::string& target, const std::string& body,
                        std::vector<std::pair<std::string, std::string>> headers = {}) {
    asio::io_context io;
    beast::tcp_stream connection(io);
    connection.connect({asio::ip::make_address("127.0.0.1"), web.port()});
    http::request<http::string_body> request{method, target, 11};
    request.set(http::field::host, host);
    for (const auto& [name, value] : headers) request.set(name, value);
    if (!body.empty()) request.set(http::field::content_type, "application/json");
    request.body() = body;
    request.prepare_payload();
    http::write(connection, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(connection, buffer, response);
    return response;
  };
  auto created = send(http::verb::post, "/api/playbooks/morning/signal-secret?account=main", "{}");
  ASSERT_EQ(created.result_int(), 201) << created.body();
  const auto secret = nlohmann::json::parse(created.body()).at("secret").get<std::string>();
  EXPECT_EQ(nlohmann::json::parse(send(http::verb::get, "/api/playbooks/morning/signal-secret?account=main", "").body()).at("configured"), true);
  const auto now = md::now() / md::kNanosPerSecond + 1;  // never before this server's start
  const std::string target = "/api/signals/morning?account=main";
  const auto body = nlohmann::json{{"action", "evaluate"}, {"nonce", "http-1"}}.dump();
  const auto stamp = std::to_string(now);
  const auto signature = "sha256=" + hmac_hex(secret, stamp + "\nPOST\n" + target + "\n" + body);
  EXPECT_EQ(send(http::verb::post, target, body, {{"X-OpenPort-Timestamp", stamp}, {"X-OpenPort-Signature", signature},
                                                  {"Authorization", "Bearer " + secret}}).result_int(), 403);
  const auto accepted = send(http::verb::post, target, body, {{"X-OpenPort-Timestamp", stamp}, {"X-OpenPort-Signature", signature}});
  EXPECT_EQ(accepted.result_int(), 200) << accepted.body();
  const auto plain = nlohmann::json{{"action", "evaluate"}, {"nonce", "http-2"}, {"secret", secret}}.dump();
  const auto downgraded = send(http::verb::post, target, plain, {{"X-Forwarded-Proto", "http"}});
  EXPECT_EQ(downgraded.result_int(), 403);
  EXPECT_NE(downgraded.body().find("SIGNAL_TLS_REQUIRED"), std::string::npos);
  EXPECT_EQ(send(http::verb::post, target, plain).result_int(), 200);  // loopback sender
  {
    const std::lock_guard lock(mutex);
    ASSERT_EQ(received.size(), 2U);
    for (const auto& request : received) {
      EXPECT_TRUE(request.signal_verified);
      EXPECT_EQ(request.actor, "signal");
    }
  }
  EXPECT_EQ(send(http::verb::delete_, "/api/playbooks/morning/signal-secret?account=main", "").result_int(), 200);
  const auto revoked = nlohmann::json{{"action", "evaluate"}, {"nonce", "http-3"}, {"secret", secret}}.dump();
  EXPECT_EQ(send(http::verb::post, target, revoked).result_int(), 403);
  web.stop();
}
