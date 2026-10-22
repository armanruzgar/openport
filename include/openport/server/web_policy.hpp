#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "openport/server/api.hpp"

namespace openport::server {

inline constexpr std::size_t kWebSocketMessageMax = 4 * 1024;
/// Open /ws sessions, handshakes included; the next upgrade gets 503 with Retry-After.
inline constexpr std::size_t kWebSocketSessionMax = 256;
/// Open HTTP connections; more are closed on accept, so idle ones cannot exhaust descriptors.
inline constexpr std::size_t kHttpSessionMax = 512;
inline constexpr int kWebSocketRetrySeconds = 5;

struct NamedToken {
  std::string name;
  std::vector<std::string> scopes;
  std::string secret;
};
/// Strict NAME SCOPES SECRET lines; diagnostics never include line contents.
[[nodiscard]] std::vector<NamedToken> parse_token_file(std::string_view text, bool allow_empty = false);

/// Immutable credential snapshots, replaced only after a whole file validates.
class TokenFile {
 public:
  struct Snapshot {
    std::vector<NamedToken> tokens;
    std::string loaded_at;
  };
  explicit TokenFile(std::filesystem::path path, std::string legacy_token = {});
  [[nodiscard]] std::shared_ptr<const Snapshot> snapshot();
  [[nodiscard]] ApiResponse reload();
  [[nodiscard]] bool valid(const NamedToken& token);
 private:
  void refresh(bool force);  // mutex_ held
  std::filesystem::path path_;
  std::string legacy_token_;
  std::mutex mutex_;
  std::shared_ptr<const Snapshot> snapshot_;
  std::optional<std::pair<std::filesystem::file_time_type, std::uintmax_t>> stamp_;
  std::string error_;
};

struct WritePolicy {
  std::string address = "127.0.0.1";
  std::string token;
  std::vector<std::string> allowed_origins;
  std::vector<NamedToken> tokens = {};
  bool require_token = false;
  std::shared_ptr<Sandboxes> sandboxes = {};
  std::string client_ip_header = {};
  std::shared_ptr<TokenFile> token_file = {};
};
[[nodiscard]] std::string write_mode(const WritePolicy& policy);
/// API reads and writes share authentication; optional actor is trusted transport context.
[[nodiscard]] std::optional<ApiResponse> check_api_write(const ApiRequest& request,
                                                        const WritePolicy& policy, std::string* actor = nullptr,
                                                        ApiAccess* access = nullptr,
                                                        std::function<bool()>* authorization_valid = nullptr);

/// Browser sockets cannot set Authorization; use openport plus a hex token subprotocol.
[[nodiscard]] std::optional<std::string> websocket_authorization(std::string_view bearer, std::string_view protocols);

/// Webhook secrets for POST /api/signals/{playbook}?account=ID, one per playbook and
/// account. Each authorises only evaluating that playbook on that account; bearer
/// tokens are not accepted there. Only SHA-256 digests are kept, in an owner-only
/// file when one is given. Nonces and rate budgets are per secret and in memory.
class SignalSecrets {
 public:
  /// Throws when an existing file is unreadable or invalid. An empty path keeps memory only.
  explicit SignalSecrets(std::filesystem::path file = {}, std::function<md::Timestamp()> clock = md::now);
  /// 201 with the secret, shown only here. Issuing again rotates; the old secret stops working.
  [[nodiscard]] ApiResponse issue(const std::string& playbook, const std::string& account);
  [[nodiscard]] ApiResponse revoke(const std::string& playbook, const std::string& account);
  /// Whether a secret exists and when it was issued; never the secret or digest.
  [[nodiscard]] ApiResponse describe(const std::string& playbook, const std::string& account);
  /// HMAC headers, or the static body secret over a secure transport; then the nonce and rate.
  [[nodiscard]] std::optional<ApiResponse> check(const ApiRequest& request);
 private:
  using Digest = std::array<unsigned char, 32>;
  using Key = std::pair<std::string, std::string>;  // playbook, account
  struct Secret { Digest digest{}; std::string created_at; };
  struct Replay { std::map<std::string, std::int64_t> nonces; std::deque<std::int64_t> accepted; };
  void save(const std::map<Key, Secret>& secrets) const;
  std::filesystem::path file_;
  std::function<md::Timestamp()> clock_;
  std::int64_t started_;  // whole seconds, rounded up
  std::mutex mutex_;
  std::map<Key, Secret> secrets_;
  std::map<Digest, Replay> replay_;
};

struct StaticFile {
  int status = 200;
  std::filesystem::path path;
  std::string reason;
};

/// Resolves only relative URL components, then checks canonical containment so
/// neither absolute paths nor symlinks can turn the server into a host-file reader.
[[nodiscard]] StaticFile resolve_static_file(const std::filesystem::path& root,
                                             std::string_view target);

/// Non-browser clients may omit Origin. Present origins must name the same
/// authority as Host; opaque origins and malformed authorities fail closed.
[[nodiscard]] bool websocket_origin_allowed(std::optional<std::string_view> origin,
                                            std::string_view host,
                                            const std::vector<std::string>& allowed_origins = {});

/// Canonical scheme and authority, without paths, credentials, or wildcards.
[[nodiscard]] std::optional<std::string> normalize_origin(std::string_view origin);

/// DNS rebinding points a name an attacker controls at this server, making a page
/// served under that name same-origin with it. The name arrives in Host, so a request
/// must address the server by an IP address, localhost (or a name under .localhost),
/// the host of an allowed origin, or an allowed host name such as a reverse proxy's
/// upstream. Names compare case-insensitively; ports are ignored.
[[nodiscard]] bool host_allowed(std::string_view host,
                                const std::vector<std::string>& allowed_origins = {},
                                const std::vector<std::string>& allowed_hosts = {});

/// `value` percent-encoded as encodeURIComponent does, for a URL fragment the
/// terminal decodes (the startup link's #token=).
[[nodiscard]] inline std::string url_component(std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

/// Ticks are complete snapshots of their stream: preserve the active write buffer
/// and only the latest pending snapshot of each stream (the engine's tick and the
/// replay's), so one stream never displaces the other. All methods run on the
/// session executor.
class TickQueue {
 public:
  void push(std::shared_ptr<const std::string> tick, std::string_view stream = {}) {
    if (!writing_) { writing_ = std::move(tick); return; }
    for (auto& [key, value] : pending_)
      if (key == stream) { value = std::move(tick); return; }
    pending_.emplace_back(std::string(stream), std::move(tick));
  }
  void pop() {
    writing_.reset();
    if (pending_.empty()) return;
    writing_ = std::move(pending_.front().second);
    pending_.erase(pending_.begin());
  }
  void clear() {
    writing_.reset();
    pending_.clear();
  }
  [[nodiscard]] bool empty() const { return !writing_; }
  [[nodiscard]] std::size_t size() const { return bool(writing_) + pending_.size(); }
  [[nodiscard]] const auto& front() const { return writing_; }

 private:
  std::shared_ptr<const std::string> writing_;
  std::vector<std::pair<std::string, std::shared_ptr<const std::string>>> pending_;
};

/// A slot covers the handshake as well as the open session and is returned on
/// every destruction path, including failed upgrades.
class WebSocketSlots {
 public:
  class Lease {
   public:
    ~Lease() { --owner_.active_; }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;

   private:
    friend class WebSocketSlots;
    explicit Lease(WebSocketSlots& owner) : owner_(owner) {}
    WebSocketSlots& owner_;
  };

  [[nodiscard]] std::unique_ptr<Lease> acquire();

 private:
  std::atomic<std::size_t> active_{0};
};

}  // namespace openport::server
