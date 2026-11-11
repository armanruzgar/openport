#include "openport/net/http.hpp"
#include "openport/net/websocket.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
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

/// What a call threw. A DNS or socket failure is marked as such, so a test of a check
/// that must fail before any network access cannot pass on the lookup's failure.
template <typename Call>
std::string error_of(Call call) {
  try {
    call();
  } catch (const boost::system::system_error& error) {
    return std::string("network: ") + error.what();
  } catch (const std::exception& error) {
    return error.what();
  }
  return "(nothing thrown)";
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

// The limit's edge, checked at 1 MiB: at the 256 MiB every response takes, each of
// these held about 1 GB, twice over when CTest ran them side by side.
TEST(Http, GunzipAcceptsExactlyItsLimitAndRejectsOneByteMore) {
  constexpr std::size_t kLimit = 1024 * 1024;
  EXPECT_EQ(gunzip(gzip(std::string(kLimit, 'x')), kLimit).size(), kLimit);
  try {
    (void)gunzip(gzip(std::string(kLimit + 1, 'x')), kLimit);
    ADD_FAILURE() << "one byte past the limit was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "gzip: decompressed output exceeds 1 MiB");
  }
}

TEST(Http, GunzipTakesUpTo256MiBByDefault) {
  EXPECT_EQ(openport::net::kMaxDecompressedBytes, 256u * 1024 * 1024);
  const std::string text(2 * 1024 * 1024, 'x');
  EXPECT_EQ(gunzip(gzip(text)), text);
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

  // An IPv6 literal's brackets are the URL's, not the address's: resolving, SNI and the
  // certificate check take it bare. Without a port the address must not be split.
  const auto six = parse_url("https://[::1]:5000/v1/api");
  ASSERT_TRUE(six);
  EXPECT_EQ(six->host, "::1");
  EXPECT_EQ(six->port, "5000");
  EXPECT_EQ(six->target, "/v1/api");
  EXPECT_EQ(openport::net::url_host(*six), "[::1]");
  const auto bare = parse_url("http://[2001:db8::7]?q=1");
  ASSERT_TRUE(bare);
  EXPECT_EQ(bare->host, "2001:db8::7");
  EXPECT_EQ(bare->port, "80");
  EXPECT_EQ(bare->target, "/?q=1");
  EXPECT_EQ(openport::net::url_host(*local), "127.0.0.1");
  EXPECT_EQ(parse_url("http://host:65535/")->port, "65535");
  EXPECT_EQ(parse_url("http://host:08080/")->port, "8080");  // one port, however written
  for (const auto* bad : {"http://user:pw@host/x", "http://user@host/", "http://host:abc/x", "http://host:0/",
                          "http://host:65536/", "http://host:123456/", "http://host:-1/", "http://host:+80/",
                          "http://host:/x", "http://[::1/x", "http://[::1]x/", "http://[::1]:/x", "http://[]/",
                          "http://[host]/", "http://::1/x", "http://ho st/", "http://[::1]:80:80/"}) {
    EXPECT_FALSE(parse_url(bad)) << bad;
  }
}

TEST(Http, BrokerCallsCancelBeforeDnsAndExposeCaseInsensitiveHeaders) {
  openport::net::HttpClient client;
  const std::atomic<bool> cancelled{true};
  // Without the checks these would look the host up and fail there instead.
  EXPECT_EQ(error_of([&] {
              (void)client.post("https://does-not-exist.invalid/oauth/token", "secret", {}, std::chrono::seconds(1),
                                &cancelled);
            }),
            "HTTP request cancelled");
  EXPECT_EQ(error_of([&] {
              (void)client.get_direct("https://does-not-exist.invalid/data", {}, std::chrono::seconds(1), &cancelled);
            }),
            "HTTP request cancelled");
  openport::net::HttpResponse response;
  response.headers = {{"x-ratelimit-available", "7"}, {"Retry-After", "60"}};
  EXPECT_EQ(response.header("X-Ratelimit-Available"), "7");
  EXPECT_EQ(response.header("retry-after"), "60");
  EXPECT_TRUE(response.header("absent").empty());
  EXPECT_TRUE(openport::net::user_agent().starts_with("openport/"));
}

TEST(Http, WebSocketRejectsPlaintextAndCancelsBeforeDns) {
  auto socket = openport::net::make_websocket();
  EXPECT_EQ(error_of([&] { socket->connect("ws://does-not-exist.invalid/", nullptr); }),
            "DXLink requires a TLS WebSocket URL");
  const std::atomic<bool> cancelled{true};
  EXPECT_EQ(error_of([&] { socket->connect("wss://does-not-exist.invalid/", &cancelled); }), "WebSocket cancelled");
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
  // A path on an IPv6 host stays on that host.
  const auto moved_six = redirect_target(*parse_url("http://[::1]:9833/old"), "/new?x=1");
  ASSERT_TRUE(moved_six);
  EXPECT_EQ(moved_six->host, "::1");
  EXPECT_EQ(moved_six->port, "9833");
  EXPECT_EQ(moved_six->target, "/new?x=1");
}

}  // namespace

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <future>

#include "openport/md/event_queue.hpp"
#include "openport/providers/massive.hpp"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include "support/recording.hpp"

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

/// Accepts the next connection within five seconds.
bool accept_within(boost::asio::ip::tcp::acceptor& listener, boost::asio::ip::tcp::socket& peer) {
  listener.non_blocking(true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    boost::system::error_code ec;
    listener.accept(peer, ec);
    if (!ec) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

/// Reads one whole request (its header and any Content-Length body) within five
/// seconds: its text, or nothing. A socket closed with a request left unread would be
/// reset, not closed.
std::optional<std::string> request_text(boost::asio::ip::tcp::socket& peer) {
  peer.non_blocking(true);
  std::string text;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (const auto end = text.find("\r\n\r\n"); end != std::string::npos) {
      const auto at = text.find("Content-Length: ");
      const std::size_t length = at == std::string::npos || at > end ? 0 : std::stoul(text.substr(at + 16));
      if (text.size() >= end + 4 + length) return std::optional<std::string>(std::move(text));
    }
    char bytes[4096];
    boost::system::error_code ec;
    const auto count = peer.read_some(boost::asio::buffer(bytes), ec);
    if (ec == boost::asio::error::would_block) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } else if (ec) {
      return std::nullopt;
    } else {
      text.append(bytes, count);
    }
  }
  return std::nullopt;
}

bool read_request(boost::asio::ip::tcp::socket& peer) { return request_text(peer).has_value(); }

void answer(boost::asio::ip::tcp::socket& peer, const std::string& body) {
  boost::system::error_code ignored;
  boost::asio::write(peer, boost::asio::buffer("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                                               "\r\n\r\n" + body), ignored);
}

/// A response's body, or the error a request threw.
template <typename Request>
std::string body_of(Request request) {
  try {
    return request().body;
  } catch (const std::exception& error) {
    return std::string("error: ") + error.what();
  }
}

/// Reads one bodiless request's head within five seconds; empty if none arrives.
std::string request_head(boost::asio::ip::tcp::socket& peer) {
  peer.non_blocking(true);
  std::string text;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (text.find("\r\n\r\n") == std::string::npos && std::chrono::steady_clock::now() < deadline) {
    char bytes[4096];
    boost::system::error_code ec;
    const auto count = peer.read_some(boost::asio::buffer(bytes), ec);
    if (ec == boost::asio::error::would_block) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } else if (ec) {
      return {};
    } else {
      text.append(bytes, count);
    }
  }
  return text;
}

/// A header's value in a request head, or empty.
std::string header_of(const std::string& head, const std::string& name) {
  const auto at = head.find("\r\n" + name + ": ");
  if (at == std::string::npos) return {};
  const auto start = at + name.size() + 4;
  return head.substr(start, head.find("\r\n", start) - start);
}

// Every test that opens a socket, even on loopback, is in the WebServer suite, so
// --gtest_filter=-WebServer.* runs the rest where sockets are not allowed. Their fake
// servers give up on a connection that does not come within five seconds: a client
// that opens fewer than expected fails the test instead of hanging it in join().
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
  // Each server answers up to `count` requests, one per connection, and notes what they carried.
  const auto serve = [&io](tcp::acceptor& listener, int count, std::string next, std::vector<std::string>& seen) {
    return std::thread([&io, &listener, count, next, &seen] {
      for (int i = 0; i < count; ++i) {
        tcp::socket peer(io);
        if (!accept_within(listener, peer)) return;
        const auto request = request_head(peer);
        if (!request.starts_with("GET ")) return;
        seen.push_back(header_of(request, "Authorization"));
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
        boost::system::error_code ignored;
        asio::write(peer, asio::buffer(reply), ignored);
      }
    });
  };
  std::vector<std::string> at_first, at_second;
  auto a = serve(first, 2, elsewhere, at_first);
  auto b = serve(second, 1, "", at_second);
  openport::net::HttpClient client;
  const auto body = body_of([&] {
    return client.get("http://127.0.0.1:" + std::to_string(first.local_endpoint().port()) + "/same",
                      {{"Authorization", "Bearer secret"}}, std::chrono::seconds(10));
  });
  a.join();
  b.join();
  EXPECT_EQ(body, "done");
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
  // A retry would show as a second connection; the wait is bounded only to rule out a
  // hang, loosely, so a loaded runner cannot fail it.
  EXPECT_EQ(accepted.load(), 1);
  EXPECT_LT(waited, std::chrono::seconds(30));
}

TEST(WebServer, AFailedRetryDoesNotLeaveItsConnectionForTheNextRequest) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto url = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port()) + "/x";
  std::atomic<int> served{0};
  std::thread server([&] {
    std::vector<std::unique_ptr<tcp::socket>> held;
    const auto serve = [&](std::string_view reply, bool close) {
      auto peer = std::make_unique<tcp::socket>(io);
      if (!accept_within(listener, *peer) || !read_request(*peer)) return false;
      boost::system::error_code ignored;
      asio::write(*peer, asio::buffer(reply), ignored);
      if (close) peer->close(ignored);
      held.push_back(std::move(peer));
      ++served;
      return true;
    };
    // A kept-alive answer, then the server drops the connection; then a malformed
    // answer with a stale one behind it, the connection left open; then a fresh one.
    if (serve("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", true) &&
        serve("HTTP/1.1 200 OK\r\nBroken header\r\n\r\n"
              "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstale",
              false))
      serve("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfresh", true);
  });
  openport::net::HttpClient client;
  EXPECT_EQ(body_of([&] { return client.get(url, {}, std::chrono::seconds(5)); }), "ok");
  const auto failed = body_of([&] { return client.get(url, {}, std::chrono::seconds(5)); });
  EXPECT_TRUE(failed.starts_with("error: ")) << failed;
  EXPECT_EQ(body_of([&] { return client.get(url, {}, std::chrono::seconds(5)); }), "fresh");
  server.join();
  EXPECT_EQ(served.load(), 3);
}

TEST(WebServer, HttpClientNeverSendsOnAKeptAliveConnectionTheServerHasClosed) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
  // Each connection answers one request, kept alive, and then the server closes it,
  // as a server does once a connection has been idle for its keep-alive timeout.
  std::atomic<int> closed{0};
  std::thread server([&] {
    for (int i = 0; i < 3; ++i) {
      tcp::socket peer(io);
      if (!accept_within(listener, peer) || !read_request(peer)) return;
      answer(peer, "ok");
      boost::system::error_code ignored;
      peer.close(ignored);
      ++closed;
    }
  });
  const auto after_close = [&](int count) {
    for (int waited = 0; waited < 5000 && closed.load() < count; ++waited)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // the FIN reaches the client
  };
  openport::net::HttpClient client;
  const auto timeout = std::chrono::seconds(5);
  EXPECT_EQ(body_of([&] { return client.post(base + "/oauth/token", "first", {}, timeout); }), "ok");
  after_close(1);
  // Written to the closed connection, a POST would fail though the server is up, and a
  // POST is never retried.
  EXPECT_EQ(body_of([&] { return client.post(base + "/oauth/token", "second", {}, timeout); }), "ok");
  after_close(2);
  EXPECT_EQ(body_of([&] { return client.get_direct(base + "/option-chains/SPX/nested", {}, timeout); }), "ok");
  server.join();
  EXPECT_EQ(closed.load(), 3);
}

TEST(WebServer, HttpClientRetriesAGetButNotAPostThatAKeptAliveConnectionDropped) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
  // Each connection answers one request, kept alive, then reads the next and closes
  // unanswered: the server timed the connection out just as the request arrived.
  std::atomic<int> accepted{0};
  std::atomic<bool> stop{false};
  std::thread server([&] {
    for (const auto* reply : {"a", "b"}) {
      tcp::socket peer(io);
      if (!accept_within(listener, peer)) return;
      ++accepted;
      if (!read_request(peer)) return;
      answer(peer, reply);
      if (!read_request(peer)) return;
      boost::system::error_code ignored;
      peer.close(ignored);
    }
    // A retried POST would open another connection.
    while (!stop) {
      tcp::socket extra(io);
      boost::system::error_code ec;
      listener.accept(extra, ec);
      if (!ec) ++accepted;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  openport::net::HttpClient client;
  const auto timeout = std::chrono::seconds(5);
  EXPECT_EQ(body_of([&] { return client.get_direct(base + "/a", {}, timeout); }), "a");
  EXPECT_EQ(body_of([&] { return client.get_direct(base + "/b", {}, timeout); }), "b");  // sent again, on a new connection
  const auto posted = body_of([&] { return client.post(base + "/c", "c", {}, timeout); });
  EXPECT_TRUE(posted.starts_with("error: ")) << posted;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  stop = true;
  server.join();
  EXPECT_EQ(accepted.load(), 2);
}

TEST(WebServer, HttpClientSendsAPostOnANewConnectionOnceTheKeptAliveOneHasIdled) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
  // The first connection stays open and quiet; the server could time it out at any
  // moment, so a POST after a second's idling must not go out on it.
  std::atomic<bool> second{false};
  std::thread server([&] {
    tcp::socket first(io), next(io);
    if (!accept_within(listener, first) || !read_request(first)) return;
    answer(first, "first");
    if (!accept_within(listener, next) || !read_request(next)) return;
    second = true;
    answer(next, "second");
  });
  openport::net::HttpClient client;
  const auto timeout = std::chrono::seconds(2);
  EXPECT_EQ(body_of([&] { return client.post(base + "/hook", "1", {}, timeout); }), "first");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_EQ(body_of([&] { return client.post(base + "/hook", "2", {}, timeout); }), "second");
  server.join();
  EXPECT_TRUE(second.load());
}

TEST(WebServer, HttpClientSendsAPortThatIsNotTheSchemesDefaultInHost) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto port = std::to_string(listener.local_endpoint().port());
  // RFC 9110 wants host:port here: a server that routes or builds links from Host
  // took a bare host for port 80.
  std::string host;
  std::thread server([&] {
    tcp::socket peer(io);
    if (!accept_within(listener, peer)) return;
    host = header_of(request_head(peer), "Host");
    boost::system::error_code ignored;
    asio::write(peer, asio::buffer(std::string("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")),
                ignored);
  });
  openport::net::HttpClient client;
  EXPECT_EQ(body_of([&] { return client.get("http://127.0.0.1:" + port + "/a", {}, std::chrono::seconds(5)); }), "ok");
  server.join();
  EXPECT_EQ(host, "127.0.0.1:" + port);
}

TEST(WebServer, HttpClientReachesAnIpv6Literal) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io);
  boost::system::error_code ec;
  listener.open(tcp::v6(), ec);
  if (!ec) listener.bind({asio::ip::make_address("::1"), 0}, ec);
  if (!ec) listener.listen(asio::socket_base::max_listen_connections, ec);
  if (ec) {
    GTEST_SKIP() << "no IPv6 loopback here: " << ec.message();
  }
  const auto port = std::to_string(listener.local_endpoint().port());
  // The bracketed host used to reach the resolver as "[::1]", which never resolves.
  // /old moves by a path alone, which stays on the same host.
  std::vector<std::string> hosts;
  std::thread server([&] {
    for (int i = 0; i < 2; ++i) {
      tcp::socket peer(io);
      if (!accept_within(listener, peer)) return;
      const auto head = request_head(peer);
      hosts.push_back(header_of(head, "Host"));
      const std::string reply = head.starts_with("GET /old ")
          ? "HTTP/1.1 302 Found\r\nLocation: /new\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
          : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
      boost::system::error_code ignored;
      asio::write(peer, asio::buffer(reply), ignored);
    }
  });
  openport::net::HttpClient client;
  EXPECT_EQ(body_of([&] { return client.get("http://[::1]:" + port + "/old", {}, std::chrono::seconds(5)); }), "ok");
  server.join();
  EXPECT_EQ(hosts, (std::vector<std::string>{"[::1]:" + port, "[::1]:" + port}));
}

TEST(WebServer, HttpClientTakesABodilessAnswerThatNamesGzip) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
  // Compressing proxies keep Content-Encoding on answers without a body. Inflating the
  // empty body threw, so the redirect was never followed.
  std::vector<std::string> paths;
  std::thread server([&] {
    for (int i = 0; i < 3; ++i) {
      tcp::socket peer(io);
      if (!accept_within(listener, peer)) return;
      const auto head = request_head(peer);
      if (!head.starts_with("GET ")) return;
      const auto path = head.substr(4, head.find(' ', 4) - 4);
      paths.push_back(path);
      std::string reply;
      if (path == "/a") {
        reply = "HTTP/1.1 301 Moved Permanently\r\nLocation: /b\r\nContent-Encoding: gzip\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
      } else if (path == "/empty") {
        reply = "HTTP/1.1 204 No Content\r\nContent-Encoding: gzip\r\nConnection: close\r\n\r\n";
      } else {
        reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
      }
      boost::system::error_code ignored;
      asio::write(peer, asio::buffer(reply), ignored);
    }
  });
  openport::net::HttpClient client;
  EXPECT_EQ(body_of([&] { return client.get(base + "/a", {}, std::chrono::seconds(5)); }), "ok");
  try {
    const auto empty = client.get(base + "/empty", {}, std::chrono::seconds(5));
    EXPECT_EQ(empty.status, 204);
    EXPECT_TRUE(empty.body.empty());
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  server.join();
  EXPECT_EQ(paths, (std::vector<std::string>{"/a", "/b", "/empty"}));
}

// Broker calls (OAuth, accounts) never follow a redirect: it would carry a client
// secret, a refresh token or a bearer token to wherever its Location points. The 307
// comes back as it is, and the request went out once.
TEST(WebServer, HttpClientHandsARedirectBackToAPostOrADirectGet) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  tcp::acceptor elsewhere(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
  const auto steal = "http://127.0.0.1:" + std::to_string(elsewhere.local_endpoint().port()) + "/steal";
  std::vector<std::string> requests;
  std::thread server([&] {
    for (int i = 0; i < 2; ++i) {
      tcp::socket peer(io);
      if (!accept_within(listener, peer)) return;
      auto request = request_text(peer);
      if (!request) return;
      requests.push_back(std::move(*request));
      boost::system::error_code ignored;
      asio::write(peer, asio::buffer("HTTP/1.1 307 Temporary Redirect\r\nLocation: " + steal +
                                     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
                  ignored);
    }
  });
  openport::net::HttpClient client;
  const auto timeout = std::chrono::seconds(5);
  openport::net::HttpResponse posted;
  openport::net::HttpResponse fetched;
  try {
    posted = client.post(base + "/oauth/token", "refresh_token=SECRET", {}, timeout);
    fetched = client.get_direct(base + "/accounts", {{"Authorization", "Bearer SECRET"}}, timeout);
  } catch (const std::exception& error) {
    ADD_FAILURE() << error.what();
  }
  server.join();
  EXPECT_EQ(posted.status, 307);
  EXPECT_EQ(posted.location, steal);
  EXPECT_EQ(fetched.status, 307);
  EXPECT_EQ(fetched.location, steal);
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_TRUE(requests[0].starts_with("POST /oauth/token ")) << requests[0];
  EXPECT_TRUE(requests[0].ends_with("\r\n\r\nrefresh_token=SECRET")) << requests[0];
  EXPECT_TRUE(requests[1].starts_with("GET /accounts ")) << requests[1];
  // Nothing went to the address the redirects named.
  elsewhere.non_blocking(true);
  tcp::socket followed(io);
  boost::system::error_code ec;
  elsewhere.accept(followed, ec);
  EXPECT_TRUE(ec == asio::error::would_block) << ec.message();
}

// Cboe serves its chains gzipped: the body is inflated, whatever the case of the
// coding's name, and wire_bytes counts what crossed the network.
TEST(WebServer, HttpClientInflatesAGzipBodyAndCountsItsWireBytes) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
  std::string text = "{\"data\":[";
  for (int i = 0; i < 2000; ++i) text += "{\"option\":\"SPXW261005P07405000\",\"bid\":4.4},";
  text += "{}]}";
  const auto compressed = gzip(text);
  std::thread server([&] {
    for (const auto* coding : {"gzip", "GZIP"}) {
      tcp::socket peer(io);
      if (!accept_within(listener, peer) || request_head(peer).empty()) return;
      boost::system::error_code ignored;
      asio::write(peer,
                  asio::buffer("HTTP/1.1 200 OK\r\nContent-Encoding: " + std::string(coding) + "\r\nContent-Length: " +
                               std::to_string(compressed.size()) + "\r\nConnection: close\r\n\r\n" + compressed),
                  ignored);
    }
  });
  openport::net::HttpClient client;
  for (int i = 0; i < 2; ++i) {
    SCOPED_TRACE(i);
    try {
      const auto response = client.get(base + "/chain.json", {}, std::chrono::seconds(5));
      EXPECT_EQ(response.status, 200);
      EXPECT_EQ(response.body.size(), text.size());
      EXPECT_TRUE(response.body == text);
      EXPECT_EQ(response.wire_bytes, compressed.size());
    } catch (const std::exception& error) {
      ADD_FAILURE() << error.what();
    }
  }
  server.join();
}

/// A key and a self-signed certificate for 127.0.0.1, made for one test.
struct LoopbackCertificate {
  LoopbackCertificate() {
    char curve[] = "P-256";
    key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", curve);
    certificate = X509_new();
    if (!key || !certificate) throw std::runtime_error("cannot make a test certificate");
    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
    X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
    X509_set_pubkey(certificate, key);
    // OpenSSL 3.6 returns the subject name const, so the name is built apart and copied in.
    X509_NAME* name = X509_NAME_new();
    const unsigned char common_name[] = "127.0.0.1";
    const bool named = name && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, common_name, -1, -1, 0) == 1 &&
                       X509_set_subject_name(certificate, name) == 1 && X509_set_issuer_name(certificate, name) == 1;
    X509_NAME_free(name);
    if (!named) throw std::runtime_error("cannot name a test certificate");
    X509V3_CTX context{};
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* addresses = X509V3_EXT_conf_nid(nullptr, &context, NID_subject_alt_name, "IP:127.0.0.1");
    const bool signed_ok = addresses && X509_add_ext(certificate, addresses, -1) == 1 &&
                           X509_sign(certificate, key, EVP_sha256()) > 0;
    X509_EXTENSION_free(addresses);
    if (!signed_ok) throw std::runtime_error("cannot sign a test certificate");
  }
  ~LoopbackCertificate() {
    X509_free(certificate);
    EVP_PKEY_free(key);
  }
  LoopbackCertificate(const LoopbackCertificate&) = delete;
  LoopbackCertificate& operator=(const LoopbackCertificate&) = delete;

  void write(const std::filesystem::path& path) const {
    std::FILE* file = std::fopen(path.c_str(), "w");
    if (!file) throw std::runtime_error("cannot write a test certificate");
    const bool written = PEM_write_X509(file, certificate) == 1;
    std::fclose(file);
    if (!written) throw std::runtime_error("cannot write a test certificate");
  }

  EVP_PKEY* key = nullptr;
  X509* certificate = nullptr;
};

/// Clients made while this lives trust `file` as their system store, which OpenSSL
/// lets SSL_CERT_FILE name; the previous setting comes back after.
struct TrustOnly {
  std::optional<std::string> previous;
  explicit TrustOnly(const std::filesystem::path& file) {
    if (const char* value = std::getenv("SSL_CERT_FILE")) previous = value;
    setenv("SSL_CERT_FILE", file.c_str(), 1);
  }
  ~TrustOnly() {
    if (previous) setenv("SSL_CERT_FILE", previous->c_str(), 1);
    else unsetenv("SSL_CERT_FILE");
  }
  TrustOnly(const TrustOnly&) = delete;
  TrustOnly& operator=(const TrustOnly&) = delete;
};

TEST(WebServer, WebSocketCloseReturnsPromptlyAfterAReceive) {
  namespace asio = boost::asio;
  namespace beast = boost::beast;
  using tcp = asio::ip::tcp;
  const LoopbackCertificate loopback;
  openport::test::RecordingFile directory;
  const auto trusted = directory.directory / "loopback.pem";
  loopback.write(trusted);
  const TrustOnly trust(trusted);

  asio::ssl::context server_tls(asio::ssl::context::tls_server);
  ASSERT_EQ(SSL_CTX_use_certificate(server_tls.native_handle(), loopback.certificate), 1);
  ASSERT_EQ(SSL_CTX_use_PrivateKey(server_tls.native_handle(), loopback.key), 1);
  asio::io_context io;
  tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto url = "wss://127.0.0.1:" + std::to_string(listener.local_endpoint().port()) + "/feed";
  // Each connection sends one message, then reads until the client goes away.
  std::thread server([&] {
    for (int i = 0; i < 2; ++i) {
      boost::system::error_code ec;
      tcp::socket peer(io);
      listener.accept(peer, ec);
      if (ec) return;
      beast::websocket::stream<beast::ssl_stream<tcp::socket>> socket(std::move(peer), server_tls);
      socket.next_layer().handshake(asio::ssl::stream_base::server, ec);
      if (!ec) socket.accept(ec);
      if (ec) continue;
      socket.text(true);
      socket.write(asio::buffer(std::string("hello")), ec);
      beast::flat_buffer unread;
      socket.read(unread, ec);
    }
  });
  // Once a read has completed, and with a read still pending.
  for (const bool pending : {false, true}) {
    SCOPED_TRACE(pending ? "read pending" : "no read pending");
    auto client = openport::net::make_websocket();
    try {
      client->connect(url, nullptr);
      EXPECT_EQ(client->receive(std::chrono::seconds(10)).value_or("(nothing)"), "hello");
      if (pending) {
        EXPECT_FALSE(client->receive(std::chrono::milliseconds(50)));
      }
    } catch (const std::exception& error) {
      ADD_FAILURE() << error.what();
    }
    // Beast's idle timer, armed by the read, used to hold close() for a minute.
    const auto started = std::chrono::steady_clock::now();
    client->close();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  }
  server.join();
}

TEST(WebServer, HttpClientRefusesRedirectsItMustNotFollow) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor plain(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto downgrade = "http://127.0.0.1:" + std::to_string(plain.local_endpoint().port()) + "/x";
  {
    // A relative path it cannot place, and another scheme.
    tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto base = "http://127.0.0.1:" + std::to_string(listener.local_endpoint().port());
    std::vector<std::string> paths;
    std::thread server([&] {
      for (int i = 0; i < 2; ++i) {
        tcp::socket peer(io);
        if (!accept_within(listener, peer)) return;
        const auto head = request_head(peer);
        if (!head.starts_with("GET ")) return;
        const auto path = head.substr(4, head.find(' ', 4) - 4);
        paths.push_back(path);
        const std::string location = path == "/relative" ? "moved.json" : "ftp://127.0.0.1/x";
        boost::system::error_code ignored;
        asio::write(peer,
                    asio::buffer("HTTP/1.1 302 Found\r\nLocation: " + location +
                                 "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
                    ignored);
      }
    });
    openport::net::HttpClient client;
    const auto relative = body_of([&] { return client.get(base + "/relative", {}, std::chrono::seconds(5)); });
    const auto scheme = body_of([&] { return client.get(base + "/ftp", {}, std::chrono::seconds(5)); });
    server.join();
    EXPECT_TRUE(relative.starts_with("error: refusing a redirect to moved.json from ")) << relative;
    EXPECT_TRUE(scheme.starts_with("error: refusing a redirect to ftp://127.0.0.1/x from ")) << scheme;
    EXPECT_EQ(paths, (std::vector<std::string>{"/relative", "/ftp"}));
  }
  // An https origin moving to plain http.
  const LoopbackCertificate loopback;
  openport::test::RecordingFile directory;
  const auto trusted = directory.directory / "loopback.pem";
  loopback.write(trusted);
  const TrustOnly trust(trusted);
  asio::ssl::context server_tls(asio::ssl::context::tls_server);
  ASSERT_EQ(SSL_CTX_use_certificate(server_tls.native_handle(), loopback.certificate), 1);
  ASSERT_EQ(SSL_CTX_use_PrivateKey(server_tls.native_handle(), loopback.key), 1);
  tcp::acceptor secure(io, {asio::ip::make_address("127.0.0.1"), 0});
  const auto origin = "https://127.0.0.1:" + std::to_string(secure.local_endpoint().port()) + "/data";
  bool answered = false;
  std::thread server([&] {
    tcp::socket peer(io);
    if (!accept_within(secure, peer)) return;
    asio::ssl::stream<tcp::socket> stream(std::move(peer), server_tls);
    boost::system::error_code ec;
    stream.handshake(asio::ssl::stream_base::server, ec);
    asio::streambuf request;
    if (!ec) asio::read_until(stream, request, "\r\n\r\n", ec);
    if (!ec)
      asio::write(stream,
                  asio::buffer("HTTP/1.1 302 Found\r\nLocation: " + downgrade +
                               "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
                  ec);
    answered = !ec;
  });
  openport::net::HttpClient client;
  const auto refused = body_of([&] { return client.get(origin, {}, std::chrono::seconds(5)); });
  server.join();
  EXPECT_TRUE(answered);
  EXPECT_TRUE(refused.starts_with("error: refusing a redirect to " + downgrade + " from ")) << refused;
  // Nothing went to the plain http address.
  plain.non_blocking(true);
  tcp::socket followed(io);
  boost::system::error_code ec;
  plain.accept(followed, ec);
  EXPECT_TRUE(ec == asio::error::would_block) << ec.message();
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
