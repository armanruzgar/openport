#include "openport/server/web_server.hpp"
#include "openport/server/sandboxes.hpp"

#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <algorithm>
#include <charconv>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <sstream>
#include <set>
#include <future>
#include <fstream>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "openport/server/web_policy.hpp"

namespace openport::server {

StaticFile resolve_static_file(const std::filesystem::path& root, std::string_view target) {
  target = target.substr(0, target.find('?'));
  if (target.empty() || target.front() != '/') return {400, {}, "path must start with one slash"};
  target.remove_prefix(1);
  std::filesystem::path relative;
  while (!target.empty()) {
    const auto slash = target.find('/');
    const auto encoded = target.substr(0, slash);
    if (encoded.empty()) return {400, {}, "absolute or empty path component"};
    std::string component;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
      char c = encoded[i];
      if (c == '%') {
        if (i + 2 >= encoded.size()) return {400, {}, "malformed path escape"};
        unsigned int byte = 0;
        const auto [end, ec] =
            std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, byte, 16);
        if (ec != std::errc{} || end != encoded.data() + i + 3) {
          return {400, {}, "malformed path escape"};
        }
        c = static_cast<char>(byte);
        i += 2;
      }
      if (c == '/' || c == '\\' || c == '\0') return {400, {}, "forbidden path separator or NUL"};
      component += c;
    }
    const std::filesystem::path part(component);
    if (component == ".." || part.has_root_path()) return {400, {}, "forbidden path component"};
    relative /= part;
    if (slash == std::string_view::npos) break;
    target.remove_prefix(slash + 1);
  }

  std::error_code ec;
  const auto canonical_root = std::filesystem::canonical(root, ec);
  if (ec || !std::filesystem::is_directory(canonical_root, ec)) {
    return {404, {}, "web terminal not built: run `npm run build` in web/, or pass --web-root"};
  }
  auto resolve = [&](const std::filesystem::path& path) -> StaticFile {
    const auto file = std::filesystem::weakly_canonical(canonical_root / path, ec);
    if (ec) return {403, {}, "cannot resolve path safely"};
    auto component = file.begin();
    for (const auto& root_component : canonical_root) {
      if (component == file.end() || *component++ != root_component) {
        return {403, {}, "path escapes web root"};
      }
    }
    if (!std::filesystem::is_regular_file(file, ec)) return {404, {}, "not a regular file"};
    return {200, file, {}};
  };
  if (relative.empty()) relative = "index.html";
  StaticFile file = resolve(relative);
  if (file.status != 404 || relative.has_extension()) return file;
  // Only missing client-side routes get the shell; directories and special files do not.
  const bool exists = std::filesystem::exists(canonical_root / relative, ec);
  if (ec || exists) return file;
  return resolve("index.html");
}

std::optional<std::string> normalize_origin(std::string_view origin) {
  const auto scheme = origin.find("://");
  if (scheme == std::string_view::npos) return std::nullopt;
  const auto protocol = origin.substr(0, scheme);
  if (protocol != "http" && protocol != "https") return std::nullopt;
  const auto authority = origin.substr(scheme + 3);
  auto normalize = [protocol](std::string_view value) -> std::optional<std::string> {
    if (value.empty() || value.find_first_of("/@\\?#% \t\r\n") != std::string_view::npos ||
        value.find('\0') != std::string_view::npos)
      return std::nullopt;
    const auto colon = value.rfind(':');
    const bool has_port =
        colon != std::string_view::npos && (value.front() != '[' || colon > value.find(']'));
    unsigned int port = protocol == "https" ? 443 : 80;
    auto name = value;
    if (has_port) {
      name = value.substr(0, colon);
      const auto text = value.substr(colon + 1);
      const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), port);
      if (ec != std::errc{} || end != text.data() + text.size() || port == 0 || port > 65535) {
        return std::nullopt;
      }
    }
    if (name.empty()) return std::nullopt;
    if (name.front() == '[') {
      boost::system::error_code ec;
      if (name.back() != ']') return std::nullopt;
      boost::asio::ip::make_address_v6(std::string(name.substr(1, name.size() - 2)), ec);
      if (ec) return std::nullopt;
    } else {
      for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.'))
          return std::nullopt;
      }
    }
    std::string normalized(name);
    for (char& c : normalized)
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return normalized + ":" + std::to_string(port);
  };
  const auto source = normalize(authority);
  if (!source) return std::nullopt;
  return std::string(protocol) + "://" + *source;
}

namespace {
/// A Host value's name, lowercased, without its port; empty when malformed.
std::string host_name(std::string_view host) {
  const auto authority = normalize_origin("http://" + std::string(host));
  if (!authority) return {};
  const std::string_view hostport = std::string_view(*authority).substr(7);
  return std::string(hostport.substr(0, hostport.rfind(':')));
}
}  // namespace

bool host_allowed(std::string_view host, const std::vector<std::string>& allowed_origins,
                  const std::vector<std::string>& allowed_hosts) {
  const auto name = host_name(host);
  if (name.empty()) return false;
  // Rebinding needs a name: IP literals and localhost cannot be pointed elsewhere.
  if (name.front() == '[') return true;  // an IPv6 literal normalize_origin validated
  boost::system::error_code ec;
  boost::asio::ip::make_address_v4(name, ec);
  if (!ec || name == "localhost" || name.ends_with(".localhost")) return true;
  for (const auto& origin : allowed_origins) {
    const auto normalized = normalize_origin(origin);
    if (!normalized) continue;
    const auto authority = std::string_view(*normalized).substr(normalized->find("://") + 3);
    if (host_name(authority) == name) return true;
  }
  return std::any_of(allowed_hosts.begin(), allowed_hosts.end(),
                     [&](const std::string& allowed) { return host_name(allowed) == name; });
}

bool websocket_origin_allowed(std::optional<std::string_view> origin, std::string_view host,
                              const std::vector<std::string>& allowed_origins) {
  if (!origin) return true;
  const auto source = normalize_origin(*origin);
  if (!source) return false;
  const auto scheme = source->substr(0, source->find("://") + 3);
  const auto destination = normalize_origin(scheme + std::string(host));
  if (source == destination) return true;
  for (const auto& allowed : allowed_origins) {
    if (source == normalize_origin(allowed)) return true;
  }
  return false;
}

std::vector<NamedToken> parse_token_file(std::string_view text, bool allow_empty) {
  std::vector<NamedToken> tokens;
  std::set<std::string> names, secrets;
  std::istringstream input{std::string(text)};
  std::string line;
  std::size_t number = 0;
  const auto identifier = [](std::string_view value) {
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
  };
  const auto account_id = [](std::string_view value) {
    return !value.empty() && value.size() <= 40 && value.front() != '-' && value.back() != '-' &&
        value.find("--") == std::string_view::npos && std::all_of(value.begin(), value.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        });
  };
  while (std::getline(input, line)) {
    ++number;
    line = line.substr(0, line.find('#'));
    std::istringstream fields(line);
    NamedToken token;
    std::string scopes, extra;
    if (!(fields >> token.name)) continue;
    const auto invalid = [&] { throw std::invalid_argument("Invalid token file at line " + std::to_string(number)); };
    if (!(fields >> scopes >> token.secret) || fields >> extra || !identifier(token.name) ||
        token.name == "system" || token.name == "loopback" || token.name == "unknown" || token.name == "legacy" ||
        !names.insert(token.name).second || !secrets.insert(token.secret).second) invalid();
    if (!std::all_of(token.secret.begin(), token.secret.end(), [](unsigned char c) { return c > 32 && c < 127; })) invalid();
    std::set<std::string> seen;
    std::size_t offset = 0;
    do {
      const auto comma = scopes.find(',', offset);
      auto scope = scopes.substr(offset, comma == std::string::npos ? comma : comma - offset);
      if ((scope != "read" && scope != "replay" && scope != "admin" && scope != "trade:*" &&
           !(scope.starts_with("trade:") && account_id(std::string_view(scope).substr(6)))) ||
          !seen.insert(scope).second) invalid();
      token.scopes.push_back(std::move(scope));
      if (comma == std::string::npos) break;
      offset = comma + 1;
    } while (true);
    tokens.push_back(std::move(token));
  }
  if (tokens.empty() && !allow_empty) throw std::invalid_argument("Token file contains no tokens");
  return tokens;
}

TokenFile::TokenFile(std::filesystem::path path, std::string legacy_token)
    : path_(std::move(path)), legacy_token_(std::move(legacy_token)) {
  refresh(true);
  if (!snapshot_) throw std::invalid_argument(error_);
}
void TokenFile::refresh(bool force) {
  try {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(path_, ec);
    if (ec) throw std::invalid_argument("Cannot stat token file");
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec) throw std::invalid_argument("Cannot stat token file");
    const auto stamp = std::make_pair(time, size);
    if (!force && stamp_ == stamp) return;
    stamp_ = stamp;
    std::ifstream input(path_);
    if (!input) throw std::invalid_argument("Cannot read token file");
    const std::string text(std::istreambuf_iterator<char>(input), {});
    if (input.bad()) throw std::invalid_argument("Cannot read token file");
    auto tokens = parse_token_file(text, bool(snapshot_));
    for (const auto& token : tokens)
      if (token.secret == legacy_token_) throw std::invalid_argument("Named and legacy tokens must have different secrets");
    const auto after_time = std::filesystem::last_write_time(path_, ec);
    if (ec || after_time != time) throw std::invalid_argument("Token file changed while reading; retry reload");
    const auto after_size = std::filesystem::file_size(path_, ec);
    if (ec || after_size != size) throw std::invalid_argument("Token file changed while reading; retry reload");
    snapshot_ = std::make_shared<const Snapshot>(Snapshot{std::move(tokens), md::format_timestamp(md::now())});
    error_.clear();
  } catch (const std::invalid_argument& error) {
    if (error_ != error.what()) std::fprintf(stderr, "openportd: token reload: %s\n", error.what());
    error_ = error.what();
  }
}
std::shared_ptr<const TokenFile::Snapshot> TokenFile::snapshot() {
  const std::lock_guard lock(mutex_);
  refresh(false);
  return snapshot_;
}
ApiResponse TokenFile::reload() {
  const std::lock_guard lock(mutex_);
  refresh(true);
  if (!error_.empty()) return api_error(400, "TOKEN_FILE_INVALID", error_);
  auto names = nlohmann::json::array();
  for (const auto& token : snapshot_->tokens) names.push_back(token.name);
  return {200, nlohmann::json{{"names", names}, {"count", names.size()}, {"loaded_at", snapshot_->loaded_at}}.dump()};
}
bool TokenFile::valid(const NamedToken& token) {
  const auto current = snapshot();
  return std::any_of(current->tokens.begin(), current->tokens.end(), [&](const auto& found) {
    return found.name == token.name && found.secret == token.secret && found.scopes == token.scopes;
  });
}

std::optional<std::string> websocket_authorization(std::string_view bearer, std::string_view protocols) {
  if (protocols.empty()) return std::string(bearer);
  constexpr std::string_view prefix = "openport, openport.token.";
  if (!bearer.empty() || !protocols.starts_with(prefix)) return {};
  protocols.remove_prefix(prefix.size());
  if (protocols.empty() || protocols.size() % 2 != 0) return {};
  std::string secret;
  for (std::size_t offset = 0; offset < protocols.size(); offset += 2) {
    unsigned int byte = 0;
    const auto* begin = protocols.data() + offset;
    const auto [end, error] = std::from_chars(begin, begin + 2, byte, 16);
    if (error != std::errc{} || end != begin + 2 || byte <= 32 || byte >= 127) return {};
    secret += static_cast<char>(byte);
  }
  return "Bearer " + secret;
}

namespace {
bool loopback_bind(const WritePolicy& policy) {
  boost::system::error_code error;
  const auto address = boost::asio::ip::make_address(policy.address, error);
  return !error && address.is_loopback();
}
/// Writes without credentials: loopback binds, unless a legacy token, --require-token
/// or visitor sandboxes (a public demo, often behind a local proxy) close them.
/// Named tokens alone leave them open.
bool open_writes(const WritePolicy& policy) {
  return loopback_bind(policy) && policy.token.empty() && !policy.require_token && !policy.sandboxes;
}
}  // namespace

std::string write_mode(const WritePolicy& policy) {
  if (open_writes(policy)) return "open";
  if (!policy.token.empty() || !policy.tokens.empty() || policy.token_file || policy.require_token || policy.sandboxes) return "token";
  return "disabled";
}

std::optional<ApiResponse> check_api_write(const ApiRequest& request, const WritePolicy& policy, std::string* actor, ApiAccess* access, std::function<bool()>* authorization_valid) {
  if (access) *access = {};
  if (authorization_valid) *authorization_valid = {};
  if (!request.target.starts_with("/api/") && request.target != "/ws") return {};
  const bool read = request.method == "GET";
  if (!read && request.method != "POST" && request.method != "PUT" && request.method != "PATCH" && request.method != "DELETE") return {};
  if (request.ambiguous_headers || (!read && !websocket_origin_allowed(
          request.origin ? std::optional<std::string_view>(*request.origin) : std::nullopt,
          request.host, policy.allowed_origins)))
    return api_error(403, "ORIGIN_REJECTED", "Origin or security headers are ambiguous or not allowed");
  const bool create_sandbox = request.method == "POST" && request.target.substr(0, request.target.find('?')) == "/api/sandboxes";
  if (create_sandbox && !policy.sandboxes) return api_error(404, "NOT_FOUND", "Sandboxes are not offered");
  std::vector<std::string> scopes;
  std::string name, sandbox;
  if (!request.authorization.empty()) {
    constexpr std::string_view prefix = "Bearer ";
    const auto matches = [&](const std::string& secret) {
      if (secret.empty() || !request.authorization.starts_with(prefix)) return false;
      const auto supplied = std::string_view(request.authorization).substr(prefix.size());
      std::array<unsigned char, 32> expected{}, actual{};
      unsigned int length = 0;
      const bool hashed = EVP_Digest(secret.data(), secret.size(), expected.data(), &length, EVP_sha256(), nullptr) == 1 &&
          EVP_Digest(supplied.data(), supplied.size(), actual.data(), &length, EVP_sha256(), nullptr) == 1;
      return hashed && CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) == 0;
    };
    if (matches(policy.token)) { name = "legacy"; scopes = {"admin"}; }
    const auto loaded = policy.token_file ? policy.token_file->snapshot() : nullptr;
    for (const auto& token : loaded ? loaded->tokens : policy.tokens) {
      if (matches(token.secret)) {
        name = token.name; scopes = token.scopes;
        if (authorization_valid && policy.token_file)
          *authorization_valid = [file = policy.token_file, token] { return file->valid(token); };
      }
    }
    if (name.empty() && policy.sandboxes && request.authorization.starts_with(prefix)) {
      if (const auto account = policy.sandboxes->authenticate(std::string_view(request.authorization).substr(prefix.size()))) {
        sandbox = *account;
        const NamedToken token{sandbox, {"read", "trade:" + sandbox}, {}};
        name = token.name;
        scopes = token.scopes;
      } else if (request.authorization.starts_with("Bearer sandbox_")) {
        return api_error(403, "SANDBOX_EXPIRED", "This sandbox has expired. Create a new sandbox account.");
      }
    }
    // Reads are public unless --require-token, so a stale token saved in a browser tab
    // cannot hide the market data; writes still reject it.
    if (name.empty() && (!read || policy.require_token))
      return api_error(403, "WRITE_TOKEN_REQUIRED", "A valid bearer token is required");
  } else if (create_sandbox || (!policy.require_token && read) || open_writes(policy)) {
    name = "loopback";
    scopes = {"admin"};
  } else {
    if (!read && write_mode(policy) == "disabled")
      return api_error(403, "WRITE_DISABLED", "A non-loopback bind requires a write token");
    return api_error(403, "WRITE_TOKEN_REQUIRED", "A valid bearer token is required");
  }
  const auto has = [&](std::string_view scope) { return std::find(scopes.begin(), scopes.end(), scope) != scopes.end(); };
  std::string_view path(request.target);
  path = path.substr(0, path.find('?'));
  // The routes read the account through the same parser, so no spelling of the
  // key or value can address an account other than the one checked here.
  const auto addressed = query_account(request.target, sandbox);
  if (!addressed) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  const auto& account = *addressed;
  if (!sandbox.empty()) {
    const bool market_read = read && (path == "/ws" || path == "/api/status" || path == "/api/accounts" ||
        path == "/api/plans" || path == "/api/calendar/events" || path == "/api/strategy-template" || path.starts_with("/api/underlyings/"));
    const bool account_read = read && (path == "/api/account" || path == "/api/account/equity" ||
        path == "/api/account/pass-odds" || path == "/api/portfolio" || path == "/api/orders" || path.starts_with("/api/orders/") ||
        path == "/api/fills" || path == "/api/settlements" || path == "/api/risk" || path == "/api/risk/profile" || path == "/api/trades" ||
        path == "/api/trades.csv" || path == "/api/fills.csv" || path == "/api/playbooks" || path == "/api/alerts");
    if ((!market_read && account != sandbox) || (read && !market_read && !account_read) ||
        (!read && path.starts_with("/api/playbooks")))
      return api_error(403, "SCOPE_REQUIRED", "Sandbox tokens may access only their own account and the demo market");
  }
  const bool replay = path.starts_with("/api/replay/");
  if (replay) path.remove_prefix(11); // leaves /orders, /risk, ...
  else if (path.starts_with("/api/")) path.remove_prefix(4);
  const bool trade = path == "/orders" || path == "/orders/preview" || path == "/orders/cancel" ||
      path.starts_with("/orders/") || path == "/positions/close" || path == "/positions/close/preview" || path == "/positions/exercise" || path == "/stocks/close" || path == "/stocks/trade" || path == "/stocks/trade/preview" ||
      path == "/positions/abandon" || path == "/positions/instruction" ||
      path.starts_with("/playbooks/staged/") || path == "/alerts" || path.starts_with("/alerts/") ||
      path == "/trades/group" || path == "/trades/ungroup" ||
      ((path.starts_with("/trades/") || path.starts_with("/days/")) && path.ends_with("/note"));
  const bool history_control = replay && path.starts_with("/history/") &&
      ((request.method == "POST" && path.ends_with("/verify")) ||
       (request.method == "DELETE" && path.substr(9).find('/') == std::string_view::npos));
  const bool replay_settings = replay &&
      ((request.method == "POST" && (path == "/account/reset" || path == "/risk/kill")) ||
       (request.method == "PUT" && (path == "/risk/limits" || path == "/risk/guardrails")));
  const bool permitted = (create_sandbox && sandbox.empty()) || has("admin") || (read ? has("read") || !policy.require_token :
      ((path == "/replay" || history_control || path == "/backtests" || path.starts_with("/backtests/")) ? has("replay") :
       (replay_settings && has("replay")) || (trade && (replay ? has("replay") : has("trade:*") || has("trade:" + account)))));
  if (!permitted) return api_error(403, "SCOPE_REQUIRED", "Token does not permit this operation");
  if (access) *access = {has("admin") && !request.authorization.empty(), sandbox};
  if (actor) *actor = name;
  const bool sandbox_account = !sandbox.empty() || (has("admin") && !request.authorization.empty() &&
      policy.sandboxes && policy.sandboxes->active(account, true));
  if (read) return {};
  if (sandbox_account && (path == "/orders" || (path.starts_with("/orders/") && path != "/orders/cancel")) &&
      request.method != "DELETE") {
    if (auto rejected = policy.sandboxes->check_order(account)) return rejected;
  }
  if (request.method == "DELETE") {
    if (!request.body.empty()) return api_error(400, "INVALID_REQUEST", "DELETE must have no body");
  } else {
    auto type = request.content_type.substr(0, request.content_type.find(';'));
    while (!type.empty() && type.back() == ' ') type.pop_back();
    for (auto& c : type) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (type != "application/json") return api_error(400, "INVALID_REQUEST", "Content-Type must be application/json");
  }
  return {};
}

std::unique_ptr<WebSocketSlots::Lease> WebSocketSlots::acquire() {
  auto count = active_.load();
  do {
    if (count >= kWebSocketSessionMax) return nullptr;
  } while (!active_.compare_exchange_weak(count, count + 1));
  try {
    return std::unique_ptr<Lease>(new Lease(*this));
  } catch (...) {
    --active_;
    throw;
  }
}

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

std::string_view mime_type(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  const std::string_view ext = dot == std::string_view::npos ? "" : path.substr(dot);
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".js" || ext == ".mjs") return "text/javascript; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".json") return "application/json";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".png") return "image/png";
  if (ext == ".ico") return "image/x-icon";
  if (ext == ".woff2") return "font/woff2";
  if (ext == ".woff") return "font/woff";
  if (ext == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

class Session {
 public:
  virtual ~Session() = default;
  virtual std::future<void> stop() = 0;
};

class Sessions {
 public:
  bool add(const std::shared_ptr<Session>& session) {
    const std::lock_guard lock(mutex_);
    if (stopping_) return false;
    std::erase_if(sessions_, [](const auto& weak) { return weak.expired(); });
    sessions_.push_back(session);
    return true;
  }

  void stop(asio::io_context* local_io = nullptr) {
    std::vector<std::shared_ptr<Session>> active;
    {
      const std::lock_guard lock(mutex_);
      stopping_ = true;
      for (auto& weak : sessions_)
        if (auto session = weak.lock()) active.push_back(session);
    }
    std::vector<std::future<void>> closed;
    for (auto& session : active) closed.push_back(session->stop());
    for (auto& done : closed) {
      if (local_io) {
        while (done.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          local_io->poll_one();
      }
      done.get();
    }
  }

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<Session>> sessions_;
  bool stopping_ = false;
};

class WsSession;

/// Tracks live WebSocket sessions and fans messages out to them.
class Hub {
 public:
  void add(std::weak_ptr<WsSession> session) {
    const std::lock_guard lock(mutex_);
    std::erase_if(sessions_, [](const auto& weak) { return weak.expired(); });
    sessions_.push_back(std::move(session));
  }

  void broadcast(const std::shared_ptr<const std::string>& message);

 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<WsSession>> sessions_;
};

class WsSession : public Session, public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(tcp::socket&& socket, Hub& hub, std::unique_ptr<WebSocketSlots::Lease> slot, ApiAccess access, std::shared_ptr<Sandboxes> sandboxes, std::function<bool()> authorized)
      : ws_(std::move(socket)), hub_(hub), slot_(std::move(slot)), access_(std::move(access)), sandboxes_(std::move(sandboxes)), authorized_(std::move(authorized)) {
    ws_.read_message_max(kWebSocketMessageMax);
  }

  void accept(http::request<http::string_body> request) {
    ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    if (request[http::field::sec_websocket_protocol].starts_with("openport, openport.token."))
      ws_.set_option(websocket::stream_base::decorator([](websocket::response_type& response) {
        response.set(http::field::sec_websocket_protocol, "openport");
      }));
    ws_.async_accept(request, beast::bind_front_handler(&WsSession::on_accept, shared_from_this()));
  }

  std::future<void> stop() override {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    asio::post(ws_.get_executor(), [self = shared_from_this(), done] {
      self->open_ = false;
      self->stopped_ = true;
      beast::error_code ignored;
      auto& socket = beast::get_lowest_layer(self->ws_).socket();
      socket.cancel(ignored);
      socket.shutdown(tcp::socket::shutdown_both, ignored);
      socket.close(ignored);
      // The active write owns its buffer until its cancellation handler runs.
      done->set_value();
    });
    return future;
  }

  void send(std::shared_ptr<const std::string> message) {
    asio::post(ws_.get_executor(), [self = shared_from_this(), message = std::move(message)] {
      if (!self->open_) return;
      if (self->sandboxes_ && !self->access_.sandbox.empty()) {
        if (!self->sandboxes_->active(self->access_.sandbox)) { (void)self->stop(); return; }
        if (message->find("\"replay_tick\"") != std::string::npos) return;
      }
      const bool idle = self->queue_.empty();
      if (self->sandboxes_)
        self->queue_.push(std::make_shared<const std::string>(sandbox_tick(*message, self->access_)));
      else self->queue_.push(std::move(message));
      if (idle) self->write_next();
    });
  }

 private:
  void on_accept(beast::error_code ec) {
    if (ec || stopped_) return;
    open_ = true;
    hub_.add(weak_from_this());
    read();
  }

  void read() {
    ws_.async_read(buffer_, beast::bind_front_handler(&WsSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (ec || stopped_) {
      open_ = false;
      return;
    }
    buffer_.consume(buffer_.size());  // clients have nothing to say yet
    read();
  }

  void write_next() {
    if (authorized_ && !authorized_()) { (void)stop(); return; }
    ws_.text(true);
    ws_.async_write(asio::buffer(*queue_.front()),
                    beast::bind_front_handler(&WsSession::on_write, shared_from_this()));
  }

  void on_write(beast::error_code ec, std::size_t) {
    if (ec || stopped_) {
      open_ = false;
      queue_.clear();
      return;
    }
    queue_.pop();
    if (!queue_.empty()) write_next();
  }

  websocket::stream<beast::tcp_stream> ws_;
  beast::flat_buffer buffer_;
  Hub& hub_;
  std::unique_ptr<WebSocketSlots::Lease> slot_;
  TickQueue queue_;
  ApiAccess access_;
  std::shared_ptr<Sandboxes> sandboxes_;
  std::function<bool()> authorized_;
  bool stopped_ = false;
  bool open_ = false;
};

void Hub::broadcast(const std::shared_ptr<const std::string>& message) {
  const std::lock_guard lock(mutex_);
  std::erase_if(sessions_, [&](const std::weak_ptr<WsSession>& weak) {
    const auto session = weak.lock();
    if (!session) return true;
    session->send(message);
    return false;
  });
}

struct Shared {
  std::filesystem::path web_root;
  AsyncApiHandler api;
  std::chrono::steady_clock::duration read_timeout;
  WritePolicy write_policy;
  Hub hub;
  WebSocketSlots slots;
  Sessions sessions;
  std::vector<std::string> allowed_origins;
  std::vector<std::string> allowed_hosts;
};

class HttpSession : public Session, public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket&& socket, Shared& shared) : stream_(std::move(socket)), shared_(shared) {}

  void run() {
    if (!shared_.sessions.add(shared_from_this())) return;
    asio::dispatch(stream_.get_executor(),
                   beast::bind_front_handler(&HttpSession::read, shared_from_this()));
  }

  std::future<void> stop() override {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    asio::post(stream_.get_executor(), [self = shared_from_this(), done] {
      self->stopped_ = true;
      {
        const std::lock_guard lock(self->completion_gate_->mutex);
        self->completion_gate_->session.reset();
      }
      self->awaiting_.reset();
      self->close();
      done->set_value();
    });
    return future;
  }

 private:
  void read() {
    if (stopped_) return;
    parser_.emplace();
    parser_->body_limit(64 * 1024);
    stream_.expires_after(shared_.read_timeout);
    http::async_read(stream_, buffer_, *parser_,
                     beast::bind_front_handler(&HttpSession::on_read, shared_from_this()));
  }

  void on_read(beast::error_code ec, std::size_t) {
    if (stopped_ || ec == http::error::end_of_stream) return close();
    if (ec == http::error::body_limit) {
      send_api(api_error(400, "INVALID_REQUEST", "Body exceeds 64 KiB"), 11, false);
      return;
    }
    if (ec) return;
    // Reading is complete. Engine work may take longer than the read budget.
    stream_.expires_never();
    http::request<http::string_body> request = parser_->release();

    // Before anything is served: a DNS-rebinding page addresses the server by a name
    // its attacker controls. Clients without Host (HTTP/1.0) are not browsers.
    if (request.count(http::field::host) > 1 ||
        (request.count(http::field::host) == 1 &&
         !host_allowed(request[http::field::host], shared_.allowed_origins, shared_.allowed_hosts))) {
      const std::string reason =
          "Host not allowed: address this server by IP address or localhost, or start it with "
          "--allowed-host for this name";
      if (request.target().starts_with("/api/"))
        return send_api(api_error(403, "HOST_REJECTED", reason), request.version(), false);
      return reject_upgrade(request, http::status::forbidden, reason);
    }

    if (websocket::is_upgrade(request)) {
      if (request.target() == "/ws") {
        const auto origin = request.find(http::field::origin);
        const auto origin_value = origin == request.end()
                                      ? std::nullopt
                                      : std::optional<std::string_view>(origin->value());
        if (request.count(http::field::origin) > 1 || request.count(http::field::host) != 1 ||
            !websocket_origin_allowed(origin_value, request[http::field::host],
                                      shared_.allowed_origins)) {
          return reject_upgrade(request, http::status::forbidden,
                                "WebSocket Origin does not match Host");
        }
        ApiRequest auth{"GET", "/ws"};
        const auto authorization = websocket_authorization(request[http::field::authorization], request[http::field::sec_websocket_protocol]);
        auth.authorization = authorization.value_or("");
        auth.ambiguous_headers = !authorization || request.count(http::field::authorization) > 1 ||
                                 request.count(http::field::sec_websocket_protocol) > 1;
        std::function<bool()> authorized;
        if (auto rejection = check_api_write(auth, shared_.write_policy, nullptr, &auth.access, &authorized))
          return reject_upgrade(request, http::status::forbidden, rejection->body);
        auto slot = shared_.slots.acquire();
        // Sessions free up as other clients disconnect: ask this one to retry.
        if (!slot)
          return reject_upgrade(request, http::status::service_unavailable,
                                "WebSocket session limit reached (" + std::to_string(kWebSocketSessionMax) +
                                " open sessions); retry later", kWebSocketRetrySeconds);
        stream_.expires_never();
        auto session =
            std::make_shared<WsSession>(stream_.release_socket(), shared_.hub, std::move(slot), std::move(auth.access), shared_.write_policy.sandboxes, std::move(authorized));
        // During shutdown an upgrade must not escape the registry's close barrier.
        if (shared_.sessions.add(session)) session->accept(std::move(request));
      }
      return;
    }
    if (request.target().starts_with("/api/")) {
      handle_api_request(request);
      return;
    }
    respond(serve_file(request, std::string(request.target())));
  }

  void reject_upgrade(const http::request<http::string_body>& request, http::status status,
                      std::string reason, int retry_after = 0) {
    http::response<http::string_body> response{status, request.version()};
    response.set(http::field::content_type, "text/plain; charset=utf-8");
    if (retry_after > 0) response.set(http::field::retry_after, std::to_string(retry_after));
    response.keep_alive(false);
    response.body() = std::move(reason);
    response.prepare_payload();
    respond(http::message_generator(std::move(response)));
  }

  void send_api(ApiResponse api, unsigned version, bool keep_alive) {
    if (stopped_) return;
    http::response<http::string_body> response{static_cast<http::status>(api.status), version};
    response.set(http::field::content_type, api.content_type);
    if (!api.download.empty()) response.set(http::field::content_disposition, "attachment; filename=\"" + api.download + "\"");
    response.set(http::field::cache_control, "no-store");
    if (api.retry_after > 0) response.set(http::field::retry_after, std::to_string(api.retry_after));
    response.keep_alive(keep_alive);
    response.body() = std::move(api.body);
    response.prepare_payload();
    respond(http::message_generator(std::move(response)));
  }

  void handle_api_request(const http::request<http::string_body>& request) {
    ApiRequest api{std::string(request.method_string()), std::string(request.target())};
    api.body = request.body();
    api.content_type = std::string(request[http::field::content_type]);
    api.host = std::string(request[http::field::host]);
    api.authorization = std::string(request[http::field::authorization]);
    if (request.count(http::field::origin)) api.origin = std::string(request[http::field::origin]);
    api.ambiguous_headers = request.count(http::field::origin) > 1 || request.count(http::field::host) != 1 ||
                            request.count(http::field::authorization) > 1 || request.count(http::field::content_type) > 1;
    if (shared_.write_policy.sandboxes) {
      const auto& header = shared_.write_policy.client_ip_header;
      beast::error_code peer_error;
      const auto peer = stream_.socket().remote_endpoint(peer_error);
      api.client_ip = sandbox_client_ip(peer_error ? "unknown" : peer.address().to_string(), header,
          header.empty() ? std::string_view{} : std::string_view(request[header]),
          !header.empty() && request.count(header) > 1);
    }
    if (auto rejection = check_api_write(api, shared_.write_policy, &api.actor, &api.access))
      return send_api(std::move(*rejection), request.version(), request.keep_alive());
    if (api.method == "POST" && api.target.substr(0, api.target.find('?')) == "/api/tokens/reload") {
      if (api.target != "/api/tokens/reload")
        return send_api(api_error(400, "INVALID_REQUEST", "Unknown query parameter"), request.version(), request.keep_alive());
      const auto body = nlohmann::json::parse(api.body.empty() ? "{}" : api.body, nullptr, false);
      if (!body.is_object() || !body.empty())
        return send_api(api_error(400, "INVALID_REQUEST", "Expected an empty object"), request.version(), request.keep_alive());
      return send_api(shared_.write_policy.token_file ? shared_.write_policy.token_file->reload()
          : api_error(409, "TOKENS_UNCONFIGURED", "No token file configured"), request.version(), request.keep_alive());
    }
    completion_gate_ = std::make_shared<CompletionGate>();
    awaiting_ = shared_from_this();
    {
      const std::lock_guard lock(completion_gate_->mutex);
      completion_gate_->session = awaiting_;
    }
    // The gate severs late engine completions during stop. No executor or socket
    // may outlive the server's io_context, even when a command is still queued.
    auto complete = [gate = completion_gate_, version = request.version(), keep_alive = request.keep_alive(),
                     token_file = shared_.write_policy.token_file, status = api.target == "/api/status"](ApiResponse response) {
      if (status && token_file && response.status == 200) {
        const auto loaded = token_file->snapshot();
        auto body = nlohmann::json::parse(response.body);
        body["tokens"] = {{"loaded_at", loaded->loaded_at}, {"count", loaded->tokens.size()}};
        response.body = body.dump();
      }
      const std::lock_guard lock(gate->mutex);
      const auto self = gate->session.lock();
      if (!self) return;
      gate->session.reset();
      asio::post(self->stream_.get_executor(), [self, response = std::move(response), version, keep_alive]() mutable {
        self->awaiting_.reset();
        self->send_api(std::move(response), version, keep_alive);
      });
    };
    try { shared_.api(api, complete); }
    catch (const std::exception&) { complete(api_error(500, "INTERNAL_ERROR", "Internal error")); }
  }

  http::message_generator serve_file(const http::request<http::string_body>& request,
                                     std::string target) {
    auto text = [&](http::status status, std::string_view body) {
      http::response<http::string_body> response{status, request.version()};
      response.set(http::field::content_type, "text/plain; charset=utf-8");
      response.keep_alive(request.keep_alive());
      response.body() = std::string(body);
      response.prepare_payload();
      return http::message_generator(std::move(response));
    };
    if (request.method() != http::verb::get && request.method() != http::verb::head) {
      return text(http::status::method_not_allowed, "method not allowed\n");
    }
    const StaticFile resolved = resolve_static_file(shared_.web_root, target);
    if (resolved.status != 200)
      return text(static_cast<http::status>(resolved.status), resolved.reason);
    const auto& file = resolved.path;

    beast::error_code open_error;
    http::file_body::value_type body;
    body.open(file.string().c_str(), beast::file_mode::scan, open_error);
    if (open_error) return text(http::status::internal_server_error, "cannot read file\n");
    const auto size = body.size();

    const bool fingerprinted = target.starts_with("/assets/");
    if (request.method() == http::verb::head) {
      http::response<http::empty_body> response{http::status::ok, request.version()};
      response.set(http::field::content_type, mime_type(file.string()));
      response.content_length(size);
      response.keep_alive(request.keep_alive());
      return response;
    }
    http::response<http::file_body> response{std::piecewise_construct,
                                             std::make_tuple(std::move(body)),
                                             std::make_tuple(http::status::ok, request.version())};
    response.set(http::field::content_type, mime_type(file.string()));
    // Built assets carry a content hash in their name and never change; the shell must.
    response.set(http::field::cache_control,
                 fingerprinted ? "public, max-age=31536000, immutable" : "no-cache");
    response.content_length(size);
    response.keep_alive(request.keep_alive());
    return response;
  }

  void respond(http::message_generator&& message) {
    const bool keep_alive = message.keep_alive();
    // Bound a stalled client from the start of this write, not the request read.
    stream_.expires_after(std::chrono::seconds(60));
    beast::async_write(stream_, std::move(message),
                       [self = shared_from_this(), keep_alive](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         if (!keep_alive) return self->close();
                         self->read();
                       });
  }

  void close() {
    beast::error_code ignored;
    stream_.socket().cancel(ignored);
    stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
    stream_.socket().close(ignored);
  }

  struct CompletionGate {
    std::mutex mutex;
    std::weak_ptr<HttpSession> session;
  };
  std::shared_ptr<CompletionGate> completion_gate_ = std::make_shared<CompletionGate>();
  std::shared_ptr<HttpSession> awaiting_;
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;
  Shared& shared_;
  bool stopped_ = false;
};

class Listener : public std::enable_shared_from_this<Listener> {
 public:
  Listener(asio::io_context& io, const tcp::endpoint& endpoint, Shared& shared)
      : io_(io), acceptor_(asio::make_strand(io)), shared_(shared) {
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(asio::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(asio::socket_base::max_listen_connections);
    port_ = acceptor_.local_endpoint().port();
  }

  void accept() {
    acceptor_.async_accept(asio::make_strand(io_),
                           beast::bind_front_handler(&Listener::on_accept, shared_from_this()));
  }

  [[nodiscard]] unsigned short port() const noexcept { return port_; }

  std::future<void> close() {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    asio::post(acceptor_.get_executor(), [self = shared_from_this(), done] {
      beast::error_code ignored;
      self->acceptor_.close(ignored);
      done->set_value();
    });
    return future;
  }

 private:
  void on_accept(beast::error_code ec, tcp::socket socket) {
    if (!acceptor_.is_open()) return;
    if (!ec) std::make_shared<HttpSession>(std::move(socket), shared_)->run();
    accept();
  }

  asio::io_context& io_;
  tcp::acceptor acceptor_;
  Shared& shared_;
  unsigned short port_ = 0;
};

}  // namespace

struct WebServer::Impl {
  // Declared in this order so the io_context (and every pending session in it) is
  // destroyed before the state those sessions refer to.
  Shared shared;
  std::string address;
  unsigned short requested_port;
  asio::io_context io;
  std::shared_ptr<Listener> listener;
  std::vector<std::thread> threads;
  bool started = false;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work;
};

WebServer::WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
                     AsyncApiHandler api, std::vector<std::string> allowed_origins, std::string write_token,
                     std::vector<std::string> allowed_hosts, std::vector<NamedToken> tokens, bool require_token,
                     std::shared_ptr<Sandboxes> sandboxes, std::string client_ip_header, std::shared_ptr<TokenFile> token_file,
                     std::chrono::steady_clock::duration read_timeout)
    : impl_(std::make_unique<Impl>()) {
  impl_->shared.web_root = std::move(web_root);
  impl_->shared.api = std::move(api);
  impl_->shared.read_timeout = read_timeout;
  for (const auto& origin : allowed_origins) {
    if (!normalize_origin(origin)) throw std::invalid_argument("invalid allowed origin: " + origin);
  }
  for (const auto& host : allowed_hosts) {
    if (host_name(host).empty()) throw std::invalid_argument("invalid allowed host: " + host);
  }
  impl_->shared.allowed_hosts = std::move(allowed_hosts);
  impl_->shared.write_policy = {address, std::move(write_token), allowed_origins, std::move(tokens), require_token, std::move(sandboxes), std::move(client_ip_header), std::move(token_file)};
  impl_->shared.allowed_origins = std::move(allowed_origins);
  impl_->address = std::move(address);
  impl_->requested_port = port;
}

WebServer::~WebServer() { stop(); }

void WebServer::start(int threads) {
  if (impl_->started)
    throw std::logic_error("WebServer::start may be called only once; construct a new WebServer");
  impl_->started = true;
  impl_->work.emplace(impl_->io.get_executor());
  const tcp::endpoint endpoint{asio::ip::make_address(impl_->address), impl_->requested_port};
  impl_->listener = std::make_shared<Listener>(impl_->io, endpoint, impl_->shared);
  impl_->listener->accept();
  try {
    for (int i = 0; i < std::max(1, threads); ++i) {
      impl_->threads.emplace_back([this] { impl_->io.run(); });
    }
  } catch (...) {
    stop();
    throw;
  }
}

void WebServer::stop() {
  if (!impl_->listener) return;
  auto closed = impl_->listener->close();
  // A failed first thread launch still needs to execute the listener close.
  if (impl_->threads.empty()) {
    while (closed.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      impl_->io.poll_one();
  }
  closed.get();
  impl_->shared.sessions.stop(impl_->threads.empty() ? &impl_->io : nullptr);
  // Let cancellation completions release sessions before the I/O threads exit.
  impl_->work.reset();
  if (impl_->threads.empty()) impl_->io.run();
  for (std::thread& thread : impl_->threads) thread.join();
  impl_->threads.clear();
  impl_->io.stop();
  impl_->listener.reset();
}

void WebServer::broadcast(std::string message) {
  impl_->shared.hub.broadcast(std::make_shared<const std::string>(std::move(message)));
}

unsigned short WebServer::port() const noexcept {
  return impl_->listener ? impl_->listener->port() : impl_->requested_port;
}

}  // namespace openport::server
