#include "openport/net/http.hpp"
#include "openport/net/websocket.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <gtest/gtest.h>
#include <zlib.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using openport::net::gunzip;
using openport::net::parse_url;

std::string gzip(const std::string& text) {
  z_stream zs{};
  EXPECT_EQ(deflateInit2(&zs, Z_BEST_SPEED, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY),
            Z_OK);
  std::string out(deflateBound(&zs, static_cast<uLong>(text.size())) + 32, '\0');
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(text.data()));
  zs.avail_in = static_cast<uInt>(text.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

TEST(Http, GunzipRoundTripsLargeRepetitiveText) {
  std::string text;
  for (int i = 0; i < 200000; ++i) text += "{\"option\":\"SPXW261005P07405000\",\"bid\":4.4},";
  EXPECT_EQ(gunzip(gzip(text)), text);
}

TEST(Http, GunzipRejectsCorruptInput) {
  std::string compressed = gzip("hello, options");
  compressed.resize(compressed.size() / 2);
  EXPECT_THROW((void)gunzip(compressed), std::runtime_error);
  EXPECT_THROW((void)gunzip("not gzip at all"), std::runtime_error);
}

TEST(Http, GunzipRejectsExpansionBeyond256MiB) {
  const auto compressed = gzip(std::string(256 * 1024 * 1024 + 1, 'x'));
  EXPECT_THROW((void)gunzip(compressed), std::runtime_error);
}

TEST(Http, GunzipAcceptsExactlyTheDecompressionLimit) {
  EXPECT_EQ(openport::net::kMaxDecompressedBytes, 256u * 1024 * 1024);
  const auto compressed = gzip(std::string(openport::net::kMaxDecompressedBytes, 'x'));
  EXPECT_EQ(gunzip(compressed).size(), openport::net::kMaxDecompressedBytes);
}

TEST(Http, AFailedRetryDoesNotLeaveItsConnectionForTheNextRequest) {
  namespace asio = boost::asio;
  using asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto port = acceptor.local_endpoint().port();
  std::thread server([&] {
    const auto serve = [&](std::string_view reply, bool close) {
      auto socket = std::make_shared<tcp::socket>(acceptor.accept());
      asio::streambuf request;
      asio::read_until(*socket, request, "\r\n\r\n");
      asio::write(*socket, asio::buffer(reply));
      if (close) socket->close();
      return socket;
    };
    // A kept-alive answer, then the server drops the connection.
    serve("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", true);
    // The retry: a malformed answer with a stale one behind it, connection left open.
    const auto held = serve("HTTP/1.1 200 OK\r\nBroken header\r\n\r\n"
                            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstale", false);
    serve("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfresh", true);
  });
  openport::net::HttpClient client;
  const auto url = "http://127.0.0.1:" + std::to_string(port) + "/x";
  EXPECT_EQ(client.get(url, {}, std::chrono::seconds(5)).body, "ok");
  EXPECT_THROW((void)client.get(url, {}, std::chrono::seconds(5)), std::exception);
  EXPECT_EQ(client.get(url, {}, std::chrono::seconds(5)).body, "fresh");
  server.join();
}

TEST(Http, ParsesUrls) {
  const auto url = parse_url("https://cdn.cboe.com/api/global/delayed_quotes/options/_SPX.json");
  ASSERT_TRUE(url);
  EXPECT_TRUE(url->tls);
  EXPECT_EQ(url->host, "cdn.cboe.com");
  EXPECT_EQ(url->port, "443");
  EXPECT_EQ(url->target, "/api/global/delayed_quotes/options/_SPX.json");

  const auto with_port = parse_url("https://localhost:8443?a=1");
  ASSERT_TRUE(with_port);
  EXPECT_EQ(with_port->host, "localhost");
  EXPECT_EQ(with_port->port, "8443");
  EXPECT_EQ(with_port->target, "/?a=1");

  const auto local = parse_url("http://127.0.0.1:25503/v3/option/snapshot/quote?symbol=SPY");
  ASSERT_TRUE(local);
  EXPECT_FALSE(local->tls);
  EXPECT_EQ(local->port, "25503");
  EXPECT_EQ(local->target, "/v3/option/snapshot/quote?symbol=SPY");
  EXPECT_EQ(parse_url("http://example.com")->port, "80");

  EXPECT_FALSE(parse_url("ftp://example.com/"));
  EXPECT_FALSE(parse_url("https://"));
}

TEST(Http, BrokerCallsCancelBeforeDnsAndExposeCaseInsensitiveHeaders) {
  openport::net::HttpClient client;
  const std::atomic<bool> cancelled{true};
  EXPECT_THROW((void)client.post("https://does-not-exist.invalid/oauth/token", "secret", {},
                                 std::chrono::seconds(1), &cancelled), std::runtime_error);
  EXPECT_THROW((void)client.get_direct("https://does-not-exist.invalid/data", {},
                                       std::chrono::seconds(1), &cancelled), std::runtime_error);
  openport::net::HttpResponse response;
  response.headers = {{"x-ratelimit-available", "7"}, {"Retry-After", "60"}};
  EXPECT_EQ(response.header("X-Ratelimit-Available"), "7");
  EXPECT_EQ(response.header("retry-after"), "60");
  EXPECT_TRUE(response.header("absent").empty());
  EXPECT_TRUE(openport::net::user_agent().starts_with("openport/"));
}

TEST(Http, WebSocketRejectsPlaintextAndCancelsBeforeDns) {
  auto socket = openport::net::make_websocket();
  EXPECT_THROW(socket->connect("ws://does-not-exist.invalid/", nullptr), std::runtime_error);
  const std::atomic<bool> cancelled{true};
  EXPECT_THROW(socket->connect("wss://does-not-exist.invalid/", &cancelled), std::runtime_error);
  socket->close();
  socket->close();
}

TEST(Http, ResolvesRedirectTargets) {
  using openport::net::redirect_target;
  const auto from = *parse_url("https://cdn.cboe.com/api/global/delayed_quotes/options/_SPX.json");
  const auto moved = redirect_target(
      from, "https://cdn-api.cboe.com/api/global/delayed_quotes/options/_SPX.json");
  ASSERT_TRUE(moved);
  EXPECT_EQ(moved->host, "cdn-api.cboe.com");
  EXPECT_EQ(moved->target, "/api/global/delayed_quotes/options/_SPX.json");

  const auto same_host = redirect_target(from, "/moved?x=1");
  ASSERT_TRUE(same_host);
  EXPECT_TRUE(same_host->tls);
  EXPECT_EQ(same_host->host, "cdn.cboe.com");
  EXPECT_EQ(same_host->port, "443");
  EXPECT_EQ(same_host->target, "/moved?x=1");

  const auto same_scheme = redirect_target(from, "//cdn-api.cboe.com/x.json");
  ASSERT_TRUE(same_scheme);
  EXPECT_TRUE(same_scheme->tls);
  EXPECT_EQ(same_scheme->host, "cdn-api.cboe.com");

  EXPECT_FALSE(redirect_target(from, "http://cdn.cboe.com/x.json"));  // never down to http
  EXPECT_FALSE(redirect_target(from, "moved.json"));
  EXPECT_FALSE(redirect_target(from, "ftp://cdn.cboe.com/x.json"));
  const auto plain = *parse_url("http://127.0.0.1:8080/old");
  const auto up = redirect_target(plain, "https://127.0.0.1:8443/new");
  ASSERT_TRUE(up);
  EXPECT_TRUE(up->tls);
  EXPECT_EQ(redirect_target(plain, "/new")->port, "8080");
}

}  // namespace

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <future>

#include "openport/md/event_queue.hpp"
#include "openport/providers/massive.hpp"

namespace {
TEST(Http, PreCancelledRequestsFailBeforeDnsOrNetworkAccess) {
  openport::net::HttpClient client;
  std::atomic<bool> cancel{true};
  const auto started = std::chrono::steady_clock::now();
  try {
    client.get("https://does-not-exist.invalid/", {}, std::chrono::seconds(30), &cancel);
    FAIL() << "cancelled request succeeded";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string(error.what()).find("cancelled"), std::string::npos);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

// Socket tests use the WebServer suite so the sandbox-safe filter excludes them.
TEST(WebServer, HttpCancellationInterruptsReadAndTlsHandshake) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  for (const auto* scheme : {"http://", "https://"}) {
    asio::io_context io;
    tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
    tcp::socket peer(io);
    std::atomic<bool> cancel{false};
    openport::net::HttpClient client;
    const auto url =
        std::string(scheme) + "127.0.0.1:" + std::to_string(listener.local_endpoint().port());
    auto request = std::async(std::launch::async, [&] {
      try {
        client.get(url, {}, std::chrono::minutes(10), &cancel);
        return std::string("succeeded");
      } catch (const std::exception& error) {
        return std::string(error.what());
      }
    });
    bool received = false;
    char bytes[4096];
    listener.async_accept(peer, [&](auto ec) {
      ASSERT_FALSE(ec);
      peer.async_read_some(asio::buffer(bytes), [&](auto error, auto) {
        EXPECT_FALSE(error);
        received = true;
      });
    });
    io.run_for(std::chrono::minutes(5));
    EXPECT_TRUE(received);  // HTTP request or TLS ClientHello, deliberately unanswered
    const auto started = std::chrono::steady_clock::now();
    cancel = true;
    EXPECT_EQ(request.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_NE(request.get().find("cancelled"), std::string::npos);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  }
}

TEST(WebServer, HttpClientFollowsRedirectsButNotForever) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto port = std::to_string(listener.local_endpoint().port());
  // Answers each request on its own connection: /old moves to /moved on the same host,
  // which moves to an absolute URL, which serves the data; /loop redirects forever.
  std::atomic<bool> stop{false};
  std::thread server([&] {
    while (!stop) {
      tcp::socket peer(io);
      boost::system::error_code ec;
      listener.accept(peer, ec);
      if (ec || stop) break;
      std::string request(4096, '\0');
      request.resize(peer.read_some(asio::buffer(request), ec));
      const auto path = request.substr(4, request.find(' ', 4) - 4);
      std::string reply;
      if (path == "/old") {
        reply = "HTTP/1.1 307 Temporary Redirect\r\nLocation: /moved\r\n";
      } else if (path == "/moved") {
        reply = "HTTP/1.1 301 Moved Permanently\r\nLocation: http://127.0.0.1:" + port + "/new\r\n";
      } else if (path == "/loop") {
        reply = "HTTP/1.1 302 Found\r\nLocation: /loop\r\n";
      } else {
        reply = "HTTP/1.1 200 OK\r\n";
      }
      const std::string body = path == "/new" ? "fresh" : "";
      reply += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
      reply += body;
      asio::write(peer, asio::buffer(reply), ec);
    }
  });
  openport::net::HttpClient client;
  const auto base = "http://127.0.0.1:" + port;
  const auto response = client.get(base + "/old");
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, "fresh");
  try {
    (void)client.get(base + "/loop");
    ADD_FAILURE() << "a redirect loop must fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("more than 5 redirects"), std::string::npos)
        << error.what();
  }
  stop = true;
  boost::system::error_code ignored;
  tcp::socket(io).connect(listener.local_endpoint(), ignored);  // wakes the accept
  server.join();
}

TEST(WebServer, HttpClientSendsAuthorizationOnlyToTheOriginItWasGivenFor) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor first(io, {asio::ip::make_address("127.0.0.1"), 0});
  tcp::acceptor second(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto elsewhere = "http://127.0.0.1:" + std::to_string(second.local_endpoint().port());
  // Each server answers `count` requests, one per connection, and notes what they carried.
  const auto serve = [&io](tcp::acceptor& listener, int count, std::string next, std::vector<std::string>& seen) {
    return std::thread([&io, &listener, count, next, &seen] {
      for (int i = 0; i < count; ++i) {
        tcp::socket peer(io);
        boost::system::error_code ec;
        listener.accept(peer, ec);
        if (ec) return;
        std::string request(4096, '\0');
        request.resize(peer.read_some(asio::buffer(request), ec));
        const auto at = request.find("Authorization: ");
        seen.push_back(at == std::string::npos ? "" : request.substr(at + 15, request.find("\r\n", at) - at - 15));
        const auto path = request.substr(4, request.find(' ', 4) - 4);
        std::string reply;
        if (path == "/same") {
          reply = "HTTP/1.1 307 Temporary Redirect\r\nLocation: /start\r\n";
        } else if (path == "/start") {
          reply = "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + next + "/end\r\n";
        } else {
          reply = "HTTP/1.1 200 OK\r\n";
        }
        const std::string body = path == "/end" ? "done" : "";
        reply += "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
        reply += body;
        asio::write(peer, asio::buffer(reply), ec);
      }
    });
  };
  std::vector<std::string> at_first, at_second;
  auto a = serve(first, 2, elsewhere, at_first);
  auto b = serve(second, 1, "", at_second);
  openport::net::HttpClient client;
  const auto response = client.get("http://127.0.0.1:" + std::to_string(first.local_endpoint().port()) + "/same",
                                   {{"Authorization", "Bearer secret"}});
  a.join();
  b.join();
  EXPECT_EQ(response.body, "done");
  EXPECT_EQ(at_first, (std::vector<std::string>{"Bearer secret", "Bearer secret"}));  // same origin
  EXPECT_EQ(at_second, (std::vector<std::string>{""}));  // another port is another origin
}

TEST(WebServer, HttpClientDoesNotRetryARequestThatTimedOutOnAKeptAliveConnection) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto url = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port()) + "/";
  // The first request is answered and the connection kept alive; the second never
  // is. A retry would open a second connection and wait the timeout again.
  std::atomic<int> accepted{0};
  std::atomic<bool> stop{false};
  std::thread server([&] {
    tcp::socket peer(io);
    boost::system::error_code ec;
    listener.accept(peer, ec);
    ++accepted;
    std::string request(4096, '\0');
    peer.read_some(asio::buffer(request), ec);
    asio::write(peer, asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")), ec);
    peer.read_some(asio::buffer(request), ec);  // the second request, left unanswered
    listener.non_blocking(true);
    while (!stop) {
      tcp::socket retry(io);
      listener.accept(retry, ec);
      if (!ec) ++accepted;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  openport::net::HttpClient client;
  EXPECT_EQ(client.get(url, {}, std::chrono::seconds(1)).body, "ok");
  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW((void)client.get(url, {}, std::chrono::seconds(1)), std::exception);
  const auto waited = std::chrono::steady_clock::now() - started;
  stop = true;
  server.join();
  EXPECT_EQ(accepted.load(), 1);
  EXPECT_LT(waited, std::chrono::milliseconds(1900));
}

TEST(WebServer, PollingProviderStopInterruptsAnInFlightHttpRead) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  tcp::socket peer(io);
  openport::providers::MassiveProvider provider(
      {.api_key = "test",
       .base_url = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port())});
  openport::md::EventQueue queue;
  bool received = false;
  char bytes[4096];
  listener.async_accept(peer, [&](auto ec) {
    ASSERT_FALSE(ec);
    peer.async_read_some(asio::buffer(bytes), [&](auto error, auto) {
      EXPECT_FALSE(error);
      received = true;
    });
  });
  provider.start({{"SPY"}}, queue);
  io.run_for(std::chrono::minutes(5));
  EXPECT_TRUE(received);
  const auto started = std::chrono::steady_clock::now();
  provider.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}
}  // namespace
