#include "openport/net/http.hpp"

#include <openssl/ssl.h>
#include <zlib.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace openport::net {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using TlsStream = beast::ssl_stream<beast::tcp_stream>;

constexpr std::size_t kMaxBody = 512u * 1024 * 1024;
constexpr std::string_view kUserAgent =
    "OpenPort/" OPENPORT_VERSION " (+https://github.com/armanruzgar/openport)";
/// A POST is never retried, so it goes out on a kept-alive connection only when that
/// was in use moments ago: a server may be closing a longer-idle one as the request
/// is written. Common keep-alive idle timeouts are five seconds or more.
constexpr auto kPostReuseIdle = std::chrono::seconds(1);

/// A URL's port: one to five digits naming 1-65535, kept without leading zeros so
/// that one port compares equal however it was written.
std::optional<std::string> port_of(std::string_view text) {
  unsigned value = 0;
  const char* end = text.data() + text.size();
  const auto [parsed, error] = std::from_chars(text.data(), end, value);
  if (text.empty() || text.size() > 5 || error != std::errc{} || parsed != end || value == 0 || value > 65535)
    return std::nullopt;
  return std::to_string(value);
}

/// A registered name or IPv4 address: printable ASCII, without what would end it or
/// bracket an IPv6 literal.
bool plain_host(std::string_view host) {
  return !host.empty() && std::all_of(host.begin(), host.end(), [](unsigned char c) {
    return c > 0x20 && c < 0x7f && c != '[' && c != ']' && c != '@' && c != '#' && c != ':';
  });
}

/// What an IPv6 literal's brackets hold: hex digits and colons, with an IPv4 tail.
bool ipv6_host(std::string_view host) {
  return host.find(':') != std::string_view::npos && std::all_of(host.begin(), host.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
  });
}

}  // namespace

std::string_view user_agent() noexcept { return "openport/" OPENPORT_VERSION; }

std::string_view HttpResponse::header(std::string_view name) const noexcept {
  for (const auto& [key, value] : headers)
    if (beast::iequals(key, name)) return value;
  return {};
}

std::string url_host(const Url& url) {
  return url.host.find(':') == std::string::npos ? url.host : '[' + url.host + ']';
}

std::optional<Url> parse_url(std::string_view url) {
  Url out;
  if (url.starts_with("https://")) {
    url.remove_prefix(8);
  } else if (url.starts_with("http://")) {
    url.remove_prefix(7);
    out.tls = false;
  } else {
    return std::nullopt;
  }

  const std::size_t path_start = url.find_first_of("/?");
  const std::string_view authority = url.substr(0, path_start);
  if (path_start == std::string_view::npos) {
    out.target = "/";
  } else {
    out.target = url.substr(path_start);
    if (out.target.front() == '?') out.target.insert(0, "/");
  }
  // "user:password@host" is no host and port, and credentials never travel in a URL here.
  if (authority.find('@') != std::string_view::npos) return std::nullopt;
  std::string_view host = authority;
  std::optional<std::string_view> port;
  if (authority.starts_with('[')) {
    // An IPv6 literal. Resolving, SNI and the certificate check take the address
    // without its brackets; only a port may follow them.
    const std::size_t close = authority.find(']');
    if (close == std::string_view::npos) return std::nullopt;
    host = authority.substr(1, close - 1);
    const auto rest = authority.substr(close + 1);
    if (!rest.empty() && !rest.starts_with(':')) return std::nullopt;
    if (!rest.empty()) port = rest.substr(1);
    if (!ipv6_host(host)) return std::nullopt;
  } else {
    const std::size_t colon = authority.find(':');
    if (colon != std::string_view::npos) {
      host = authority.substr(0, colon);
      port = authority.substr(colon + 1);
    }
    if (!plain_host(host)) return std::nullopt;
  }
  out.host = host;
  if (port) {
    auto number = port_of(*port);
    if (!number) return std::nullopt;
    out.port = std::move(*number);
  } else {
    out.port = out.tls ? "443" : "80";
  }
  return out;
}

std::optional<Url> redirect_target(const Url& from, std::string_view location) {
  std::string absolute(location);
  if (location.starts_with("//")) {
    absolute.insert(0, from.tls ? "https:" : "http:");
  } else if (location.starts_with('/')) {
    absolute.insert(0, (from.tls ? "https://" : "http://") + url_host(from) + ':' + from.port);
  }
  auto target = parse_url(absolute);
  if (target && from.tls && !target->tls) return std::nullopt;
  return target;
}

struct HttpClient::Impl {
  asio::io_context io;
  const std::atomic<bool>* cancellation = nullptr;
  ssl::context tls_context{ssl::context::tls_client};
  // Exactly one of these is open at a time.
  std::unique_ptr<TlsStream> tls;
  std::unique_ptr<beast::tcp_stream> plain;
  Url connected;
  beast::flat_buffer buffer;
  std::chrono::steady_clock::time_point last_used{};  ///< when the open connection last finished a request

  Impl() {
    tls_context.set_verify_mode(ssl::verify_peer);
    tls_context.set_default_verify_paths();
    // OpenSSL's compiled-in trust store is not always populated; add the platform bundle.
    for (const char* bundle : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"}) {
      if (std::filesystem::exists(bundle)) {
        boost::system::error_code ignored;
        tls_context.load_verify_file(bundle, ignored);
        break;
      }
    }
  }

  [[nodiscard]] bool open() const noexcept { return tls || plain; }

  [[nodiscard]] bool connected_to(const Url& url) const noexcept {
    return open() && connected.tls == url.tls && connected.host == url.host &&
           connected.port == url.port;
  }

  [[nodiscard]] beast::tcp_stream& tcp_layer() {
    return tls ? beast::get_lowest_layer(*tls) : *plain;
  }

  /// Whether the open connection to `url` can carry another request: nothing has
  /// arrived on it since the last response. A server that dropped the idle connection
  /// has sent its FIN (or a TLS close_notify, or a 408) by now, and a request written
  /// after that would fail.
  [[nodiscard]] bool quiet(const Url& url) {
    if (!connected_to(url) || buffer.size() > 0) return false;
    auto& socket = tcp_layer().socket();
    boost::system::error_code ec;
    socket.non_blocking(true, ec);
    if (ec) return false;
    char peeked = 0;
    (void)socket.receive(asio::buffer(&peeked, 1), tcp::socket::message_peek, ec);
    boost::system::error_code ignored;
    socket.non_blocking(false, ignored);
    return ec == asio::error::would_block;
  }

  // Beast only enforces tcp_stream timeouts on asynchronous operations, so every
  // step is started asynchronously and the io_context is run until it finishes.
  void check_cancelled() const {
    if (cancellation && cancellation->load()) throw std::runtime_error("HTTP request cancelled");
  }

  template <typename Start>
  void run(Start&& start) {
    check_cancelled();
    boost::system::error_code result = asio::error::would_block;
    std::forward<Start>(start)([&result](boost::system::error_code ec) { result = ec; });
    io.restart();
    while (!io.stopped()) {
      io.run_for(std::chrono::milliseconds(25));
      if (cancellation && cancellation->load() && open()) {
        // Complete the cancelled handlers before their captured stack data dies.
        boost::system::error_code ignored;
        tcp_layer().socket().cancel(ignored);
        tcp_layer().socket().close(ignored);
      }
    }
    check_cancelled();
    if (result) throw boost::system::system_error(result);
  }

  void close() noexcept {
    if (!open()) return;
    boost::system::error_code ignored;
    tcp_layer().socket().shutdown(tcp::socket::shutdown_both, ignored);
    tcp_layer().socket().close(ignored);
    tls.reset();
    plain.reset();
    buffer.clear();
  }

  void connect(const Url& url, std::chrono::seconds timeout) {
    check_cancelled();
    close();
    tcp::resolver resolver(io);
    // Synchronous system DNS is the only step the cancellation flag cannot interrupt.
    const auto endpoints = resolver.resolve(url.host, url.port);
    check_cancelled();

    if (url.tls) {
      tls = std::make_unique<TlsStream>(io, tls_context);
      auto& stream = tls;
      // SSL_set_tlsext_host_name is a macro with a C-style cast; call what it expands to.
      if (SSL_ctrl(stream->native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                   const_cast<char*>(url.host.c_str())) != 1) {
        throw std::runtime_error("TLS: cannot set SNI host name");
      }
      stream->set_verify_callback(ssl::host_name_verification(url.host));
      auto& layer = beast::get_lowest_layer(*stream);
      layer.expires_after(timeout);
      run([&](auto done) {
        layer.async_connect(endpoints,
                            [done](boost::system::error_code ec, const tcp::endpoint&) { done(ec); });
      });
      layer.expires_after(timeout);
      run([&](auto done) { stream->async_handshake(ssl::stream_base::client, done); });
    } else {
      plain = std::make_unique<beast::tcp_stream>(io);
      auto& stream = plain;
      stream->expires_after(timeout);
      run([&](auto done) {
        stream->async_connect(endpoints,
                              [done](boost::system::error_code ec, const tcp::endpoint&) { done(ec); });
      });
    }
    connected = url;
  }

  template <typename Stream>
  HttpResponse exchange(Stream& stream, const Url& url, const Headers& headers,
                        std::chrono::seconds timeout, http::verb method, std::string_view body) {
    http::request<http::string_body> req{method, url.target, 11};
    // The URL's authority, with its port unless that is the scheme's default (RFC 9110, 7.2).
    const bool default_port = url.port == (url.tls ? "443" : "80");
    req.set(http::field::host, default_port ? url_host(url) : url_host(url) + ':' + url.port);
    req.set(http::field::user_agent, kUserAgent);
    req.set(http::field::accept_encoding, "gzip");
    for (const auto& [name, value] : headers) req.set(name, value);
    if (method == http::verb::post) {
      req.body() = body;
      req.prepare_payload();
    }

    const auto started = std::chrono::steady_clock::now();
    tcp_layer().expires_after(timeout);
    run([&](auto done) {
      http::async_write(stream, req, [done](boost::system::error_code ec, std::size_t) { done(ec); });
    });

    http::response_parser<http::string_body> parser;
    parser.body_limit(kMaxBody);
    tcp_layer().expires_after(timeout);
    run([&](auto done) {
      http::async_read(stream, buffer, parser,
                       [done](boost::system::error_code ec, std::size_t) { done(ec); });
    });
    tcp_layer().expires_never();

    auto response = parser.release();
    HttpResponse out;
    out.status = static_cast<int>(response.result_int());
    out.location = std::string(response[http::field::location]);
    for (const auto& field : response.base())
      out.headers.emplace_back(field.name_string(), field.value());
    out.wire_bytes = response.body().size();
    // A bodiless answer (a redirect, 204, 304, an empty error page) may still name its
    // encoding; there is nothing to inflate, and an empty stream is no gzip stream.
    const bool gzipped =
        !response.body().empty() && beast::iequals(response[http::field::content_encoding], "gzip");
    out.body = gzipped ? gunzip(response.body()) : std::move(response.body());
    out.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    last_used = std::chrono::steady_clock::now();
    if (!response.keep_alive()) close();
    return out;
  }

  HttpResponse request(const Url& url, const Headers& headers, std::chrono::seconds timeout,
                       http::verb method = http::verb::get, std::string_view body = {}) {
    return tls ? exchange(*tls, url, headers, timeout, method, body)
               : exchange(*plain, url, headers, timeout, method, body);
  }

  /// One request to `target`, on the kept-alive connection while it is quiet, else on
  /// a new one. A server can still drop a kept-alive connection as a request reaches
  /// it, so a GET that fails on a reused connection is sent once more on a new one.
  /// A POST is not: the server may have acted on it. Nor is a timeout, which has
  /// waited the whole timeout already.
  HttpResponse fetch(const Url& target, const Headers& headers, std::chrono::seconds timeout,
                     http::verb method = http::verb::get, std::string_view body = {}) {
    const bool posting = method == http::verb::post;
    const bool reusing =
        quiet(target) && (!posting || std::chrono::steady_clock::now() - last_used <= kPostReuseIdle);
    try {
      if (!reusing) connect(target, timeout);
      return request(target, headers, timeout, method, body);
    } catch (const std::exception& error) {
      close();
      const auto* system = dynamic_cast<const boost::system::system_error*>(&error);
      const bool timed_out = system && system->code() == beast::error::timeout;
      if (!reusing || posting || timed_out || (cancellation && cancellation->load())) throw;
    }
    // A failed retry closes too, so no half-read connection is reused.
    try {
      connect(target, timeout);
      return request(target, headers, timeout, method, body);
    } catch (...) {
      close();
      throw;
    }
  }

  HttpResponse direct(std::string_view url, std::string_view body, const Headers& headers,
                      std::chrono::seconds timeout, const std::atomic<bool>* cancel,
                      http::verb method) {
    const auto previous = cancellation;
    cancellation = cancel;
    try {
      check_cancelled();
      const auto target = parse_url(url);
      if (!target) throw std::runtime_error("invalid HTTP URL");
      auto response = fetch(*target, headers, timeout, method, body);
      check_cancelled();
      cancellation = previous;
      return response;
    } catch (...) {
      close();
      cancellation = previous;
      throw;
    }
  }
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() { impl_->close(); }

HttpResponse HttpClient::get_direct(std::string_view url, const Headers& headers,
                                    std::chrono::seconds timeout, const std::atomic<bool>* cancel) {
  return impl_->direct(url, {}, headers, timeout, cancel, http::verb::get);
}

HttpResponse HttpClient::post(std::string_view url, std::string_view body, const Headers& headers,
                              std::chrono::seconds timeout, const std::atomic<bool>* cancel) {
  return impl_->direct(url, body, headers, timeout, cancel, http::verb::post);
}

HttpResponse HttpClient::get(std::string_view url, const Headers& headers,
                             std::chrono::seconds timeout) {
  Headers sent = headers;
  // Follows data a host has moved, as Cboe moved its delayed quotes in September 2026.
  constexpr int kMaxRedirects = 5;
  std::optional<Url> target = parse_url(url);
  if (!target) throw std::runtime_error("not an http(s) URL: " + std::string(url));
  const Url origin = *target;
  for (int redirects = 0;; ++redirects) {
    impl_->check_cancelled();
    auto response = impl_->fetch(*target, sent, timeout);
    const int s = response.status;
    const bool moved = s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
    if (!moved || response.location.empty()) return response;
    if (redirects == kMaxRedirects) {
      throw std::runtime_error("more than 5 redirects from " + std::string(url));
    }
    target = redirect_target(*target, response.location);
    if (!target) {
      throw std::runtime_error("refusing a redirect to " + response.location + " from " +
                               std::string(url));
    }
    // Credentials stay with the origin they were given for, as in browsers and curl.
    if (target->tls != origin.tls || !beast::iequals(target->host, origin.host) ||
        target->port != origin.port) {
      std::erase_if(sent, [](const auto& header) { return beast::iequals(header.first, "authorization"); });
    }
  }
}

HttpResponse HttpClient::get(std::string_view url, const Headers& headers,
                             std::chrono::seconds timeout, const std::atomic<bool>* cancellation) {
  const auto previous = impl_->cancellation;
  impl_->cancellation = cancellation;
  try {
    impl_->check_cancelled();
    auto response = get(url, headers, timeout);
    impl_->check_cancelled();
    impl_->cancellation = previous;
    return response;
  } catch (...) {
    impl_->cancellation = previous;
    throw;
  }
}

std::string gunzip(std::string_view compressed) {
  if (compressed.size() > std::numeric_limits<uInt>::max()) {
    throw std::runtime_error("gzip: compressed input too large");
  }
  z_stream zs{};
  if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw std::runtime_error("gzip: init failed");
  struct EndInflate {
    z_stream& stream;
    ~EndInflate() { inflateEnd(&stream); }
  } end{zs};
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
  zs.avail_in = static_cast<uInt>(compressed.size());

  std::string out;
  char chunk[64 * 1024];
  int status = Z_OK;
  while (status == Z_OK) {
    zs.next_out = reinterpret_cast<Bytef*>(chunk);
    zs.avail_out = sizeof chunk;
    status = inflate(&zs, Z_NO_FLUSH);
    const std::size_t produced = sizeof chunk - zs.avail_out;
    if (produced > kMaxDecompressedBytes - out.size()) {
      throw std::runtime_error("gzip: decompressed output exceeds 256 MiB");
    }
    out.append(chunk, produced);
  }
  if (status != Z_STREAM_END) throw std::runtime_error("gzip: corrupt or truncated stream");
  return out;
}

}  // namespace openport::net
