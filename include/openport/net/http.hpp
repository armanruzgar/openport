#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openport::net {

struct Url {
  bool tls = true;     ///< https (true) or http (false)
  std::string host;
  std::string port;    ///< "443" / "80" unless the URL says otherwise
  std::string target;  ///< path and query
};

/// Parses "http[s]://host[:port][/path][?query]".
[[nodiscard]] std::optional<Url> parse_url(std::string_view url);

/// Resolves a redirect's Location header against the URL that sent it: an absolute
/// http(s) URL, "//host/path" on the same scheme, or a path on the same host. Empty
/// for anything else, and for a move from https down to http.
[[nodiscard]] std::optional<Url> redirect_target(const Url& from, std::string_view location);

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpResponse {
  int status = 0;
  std::string body;            ///< decompressed if the server sent gzip
  std::size_t wire_bytes = 0;  ///< body bytes as received, before decompression
  std::chrono::microseconds elapsed{0};
  std::string location{};      ///< a redirect's Location header, if any
  Headers headers{};
  [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
};

[[nodiscard]] std::string_view user_agent() noexcept;

/// A blocking HTTP/1.1 client for http and https URLs. It keeps the connection to
/// the last host alive between requests, asks for gzip and decompresses it,
/// verifies TLS certificates against the system trust store, and enforces a
/// timeout on every step. It follows up to five redirects (301, 302, 303, 307 and
/// 308, as GETs), never from https to http, dropping Authorization once one leaves the
/// URL's scheme, host and port.
///
/// A kept-alive connection carries the next request only while nothing has arrived
/// on it since the last response, so one the server has closed is replaced first. A
/// GET that still fails on a reused connection (the server closed it as the request
/// arrived) is sent once more on a new one; a timeout is not.
///
/// Not thread-safe: give each thread its own client.
class HttpClient {
 public:
  HttpClient();
  virtual ~HttpClient();
  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  /// Throws std::runtime_error on network, TLS or timeout failures. HTTP error
  /// statuses are returned, not thrown.
  virtual HttpResponse get(std::string_view url, const Headers& headers = {},
                           std::chrono::seconds timeout = std::chrono::seconds(30));

  /// Cancellation is checked every 25 ms during connect, TLS handshake and I/O.
  /// The flag must outlive this blocking call.
  /// System DNS resolution (getaddrinfo) can still block until the OS returns.
  HttpResponse get(std::string_view url, const Headers& headers, std::chrono::seconds timeout,
                   const std::atomic<bool>* cancellation);

  /// Single-origin requests for brokers. Never follow redirects or retry a POST:
  /// a redirect must not forward an OAuth body or escape an adapter's path allowlist.
  /// So a POST goes out on a kept-alive connection only within a second of its last
  /// request; after that a server may be timing the connection out.
  virtual HttpResponse get_direct(std::string_view url, const Headers& headers,
                                  std::chrono::seconds timeout,
                                  const std::atomic<bool>* cancellation = nullptr);
  virtual HttpResponse post(std::string_view url, std::string_view body, const Headers& headers,
                            std::chrono::seconds timeout,
                            const std::atomic<bool>* cancellation = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

inline constexpr std::size_t kMaxDecompressedBytes = 256 * 1024 * 1024;

/// Decompresses a complete gzip stream. Rejects corrupt streams and expansion
/// beyond kMaxDecompressedBytes so a small response cannot exhaust memory.
[[nodiscard]] std::string gunzip(std::string_view compressed);

}  // namespace openport::net
