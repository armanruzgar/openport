#include "openport/server/web_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace openport::server {
namespace {
using nlohmann::json;
constexpr std::int64_t kWindow = 300;               // seconds either side of the server clock
constexpr std::int64_t kStaticRetention = 24 * 60 * 60;
constexpr std::size_t kRate = 60;                   // accepted signals per rolling minute per secret
constexpr std::size_t kNonces = 2048;               // retained per secret
constexpr std::size_t kSecrets = 256;
constexpr std::size_t kFileMax = 1024 * 1024;
// 70 bytes, longer than SHA-256's 64-byte block, so standard HMAC first replaces the
// key with its SHA-256 (RFC 2104). The stored digest therefore verifies signatures
// made with the secret itself, and the secret is never kept.
constexpr std::string_view kPrefix = "opsig_";

[[noreturn]] void invalid_file() { throw std::runtime_error("Invalid signal-secrets.json"); }
bool playbook_id(std::string_view id) {
  return !id.empty() && id.size() <= 15 && std::all_of(id.begin(), id.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}
bool nonce_text(std::string_view nonce) {
  return !nonce.empty() && nonce.size() <= 128 && nonce.find_first_not_of(
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:") == std::string_view::npos;
}
std::string hex(const unsigned char* data, std::size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (std::size_t i = 0; i < size; ++i) { text += digits[data[i] >> 4]; text += digits[data[i] & 15]; }
  return text;
}
std::optional<std::array<unsigned char, 32>> sha256(std::string_view text) {
  std::array<unsigned char, 32> digest{};
  unsigned int size = 0;
  if (EVP_Digest(text.data(), text.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 || size != digest.size()) return std::nullopt;
  return digest;
}
ApiResponse mismatch() {
  // Unknown playbook/account pairs and wrong secrets are indistinguishable.
  return api_error(403, "INVALID_SIGNAL_SIGNATURE", "Signal secret or signature does not match");
}
}  // namespace

SignalSecrets::SignalSecrets(std::filesystem::path file, std::function<md::Timestamp()> clock)
    : file_(std::move(file)), clock_(std::move(clock)) {
  const auto started = clock_();
  started_ = started / md::kNanosPerSecond + (started % md::kNanosPerSecond != 0);
  if (file_.empty()) return;
  const int fd = ::open(file_.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error("Cannot open signal-secrets.json");
  }
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::size_t>(info.st_size) > kFileMax) invalid_file();
  std::string contents;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof buffer);
    if (count < 0) throw std::runtime_error("Cannot read signal-secrets.json");
    if (count == 0) break;
    contents.append(buffer, static_cast<std::size_t>(count));
    if (contents.size() > kFileMax) invalid_file();
  }
  try {
    const auto root = json::parse(contents);
    if (!root.is_object() || root.size() != 2 || root.at("schema") != 1 || !root.at("secrets").is_array() ||
        root.at("secrets").size() > kSecrets) invalid_file();
    for (const auto& item : root.at("secrets")) {
      if (!item.is_object() || item.size() != 4) invalid_file();
      const auto playbook = item.at("playbook").get<std::string>(), account = item.at("account").get<std::string>();
      const auto digest = item.at("sha256").get<std::string>();
      if (!playbook_id(playbook) || !query_account("/?account=" + account) || digest.size() != 64) invalid_file();
      Secret secret;
      for (std::size_t i = 0; i < secret.digest.size(); ++i) {
        const auto [end, error] = std::from_chars(digest.data() + 2 * i, digest.data() + 2 * i + 2, secret.digest[i], 16);
        if (error != std::errc{} || end != digest.data() + 2 * i + 2) invalid_file();
      }
      secret.created_at = item.at("created_at").get<std::string>();
      if (!secrets_.emplace(Key{playbook, account}, std::move(secret)).second) invalid_file();
    }
  } catch (const std::runtime_error&) { throw; }
  catch (...) { invalid_file(); }
}

void SignalSecrets::save(const std::map<Key, Secret>& secrets) const {
  if (file_.empty()) return;
  json list = json::array();
  for (const auto& [key, secret] : secrets)
    list.push_back({{"playbook", key.first}, {"account", key.second}, {"sha256", hex(secret.digest.data(), secret.digest.size())},
                    {"created_at", secret.created_at}});
  const auto text = json{{"schema", 1}, {"secrets", list}}.dump(2) + "\n";
  const auto temporary = file_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("Cannot write signal secrets");
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  const bool synced = written && ::fchmod(fd, 0600) == 0 && ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) throw std::runtime_error("Cannot sync signal secrets");
  std::filesystem::rename(temporary, file_);
  const int directory = ::open(file_.parent_path().empty() ? "." : file_.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (directory >= 0) { (void)::fsync(directory); ::close(directory); }
}

ApiResponse SignalSecrets::issue(const std::string& playbook, const std::string& account) {
  if (!playbook_id(playbook)) return api_error(400, "INVALID_REQUEST", "Playbook IDs use 1-15 lowercase letters, digits or hyphens");
  std::array<unsigned char, 32> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
    return api_error(503, "SIGNAL_AUTH_UNAVAILABLE", "Cannot generate a signal secret");
  const auto secret = std::string(kPrefix) + hex(random.data(), random.size());
  const auto digest = sha256(secret);
  if (!digest) return api_error(503, "SIGNAL_AUTH_UNAVAILABLE", "Cannot generate a signal secret");
  const std::lock_guard lock(mutex_);
  const Key key{playbook, account};
  const auto existing = secrets_.find(key);
  if (existing == secrets_.end() && secrets_.size() >= kSecrets)
    return api_error(409, "SIGNAL_SECRET_LIMIT", "At most 256 signal secrets; revoke one first");
  auto next = secrets_;
  next[key] = {*digest, md::format_timestamp(clock_())};
  try { save(next); } catch (const std::exception&) { return api_error(503, "SIGNAL_SECRET_STORAGE", "Cannot persist the signal secret"); }
  if (existing != secrets_.end()) replay_.erase(existing->second.digest);
  const bool rotated = existing != secrets_.end();
  secrets_ = std::move(next);
  return {201, json{{"playbook", playbook}, {"account", account}, {"secret", secret}, {"created_at", secrets_.at(key).created_at},
                    {"rotated", rotated}}.dump()};
}

ApiResponse SignalSecrets::revoke(const std::string& playbook, const std::string& account) {
  const std::lock_guard lock(mutex_);
  const auto existing = secrets_.find({playbook, account});
  if (existing == secrets_.end()) return api_error(404, "NOT_FOUND", "No signal secret for this playbook and account");
  auto next = secrets_;
  next.erase(existing->first);
  try { save(next); } catch (const std::exception&) { return api_error(503, "SIGNAL_SECRET_STORAGE", "Cannot persist the revocation"); }
  replay_.erase(existing->second.digest);
  secrets_ = std::move(next);
  return {200, json{{"playbook", playbook}, {"account", account}, {"revoked", true}}.dump()};
}

ApiResponse SignalSecrets::describe(const std::string& playbook, const std::string& account) {
  const std::lock_guard lock(mutex_);
  const auto existing = secrets_.find({playbook, account});
  return {200, json{{"playbook", playbook}, {"account", account}, {"configured", existing != secrets_.end()},
                    {"created_at", existing == secrets_.end() ? json(nullptr) : json(existing->second.created_at)}}.dump()};
}

std::optional<ApiResponse> SignalSecrets::check(const ApiRequest& request) {
  if (request.method != "POST") return api_error(405, "METHOD_NOT_ALLOWED", "Signals use POST");
  const auto question = request.target.find('?');
  const auto playbook = request.target.substr(0, question).substr(std::string_view("/api/signals/").size());
  const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
  const auto account = query_account(request.target);
  if (!playbook_id(playbook) || !pairs || !account || std::any_of(pairs->begin(), pairs->end(), [](const auto& item) { return item.first != "account"; }))
    return api_error(400, "INVALID_SIGNAL", "Expected /api/signals/PLAYBOOK?account=ID");
  const bool signed_request = !request.signal_signature.empty() || !request.signal_timestamp.empty();
  Digest digest;
  {
    const std::lock_guard lock(mutex_);
    const auto found = secrets_.find({playbook, *account});
    if (found == secrets_.end()) return mismatch();
    digest = found->second.digest;
  }
  const auto now = clock_() / md::kNanosPerSecond;
  std::int64_t expires = now + kStaticRetention;
  if (signed_request) {
    std::int64_t timestamp = -1;
    const auto& stamp = request.signal_timestamp;
    const auto [end, error] = std::from_chars(stamp.data(), stamp.data() + stamp.size(), timestamp);
    if (stamp.empty() || stamp.size() > 12 || stamp.front() == '-' || error != std::errc{} || end != stamp.data() + stamp.size() ||
        request.signal_signature.empty())
      return api_error(400, "INVALID_SIGNAL", "Send X-OpenPort-Timestamp (Unix seconds) and X-OpenPort-Signature together");
    const auto message = stamp + "\n" + request.method + "\n" + request.target + "\n" + request.body;
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int size = 0;
    if (!HMAC(EVP_sha256(), digest.data(), static_cast<int>(digest.size()),
              reinterpret_cast<const unsigned char*>(message.data()), message.size(), mac.data(), &size))
      return api_error(503, "SIGNAL_AUTH_UNAVAILABLE", "Cannot verify signal identity");
    const auto expected = hex(mac.data(), size);
    auto supplied = std::string_view(request.signal_signature);
    if (supplied.starts_with("sha256=")) supplied.remove_prefix(7);
    std::string lower(supplied);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower.size() != expected.size() || CRYPTO_memcmp(lower.data(), expected.data(), expected.size()) != 0) return mismatch();
    if (timestamp < now - kWindow || timestamp > now + kWindow)
      return api_error(409, "SIGNAL_EXPIRED", "Signal timestamp must be within 300 seconds of the server clock");
    // Nonces live in memory; a request signed before a restart must not become valid again.
    if (timestamp < started_) return api_error(409, "SIGNAL_EXPIRED", "Signal was signed before this server started");
    expires = timestamp + kWindow + 1;
  } else if (!request.signal_secure) {
    return api_error(403, "SIGNAL_TLS_REQUIRED", "A body secret needs HTTPS through the configured proxy or a loopback connection; use signature headers otherwise");
  }
  const auto body = json::parse(request.body, nullptr, false);
  if (!body.is_object() || body.size() != (signed_request ? 2U : 3U) || body.value("action", json()) != "evaluate" ||
      !body.contains("nonce") || !body.at("nonce").is_string() || !nonce_text(body.at("nonce").get_ref<const std::string&>()) ||
      (!signed_request && (!body.contains("secret") || !body.at("secret").is_string())))
    return api_error(400, "INVALID_SIGNAL", signed_request
        ? "Expected {action: evaluate, nonce}; nonce is 1-128 letters, digits, _ - . or :"
        : "Expected {action: evaluate, nonce, secret}; nonce is 1-128 letters, digits, _ - . or :");
  if (!signed_request) {
    const auto supplied = sha256(body.at("secret").get_ref<const std::string&>());
    if (!supplied) return api_error(503, "SIGNAL_AUTH_UNAVAILABLE", "Cannot verify signal identity");
    if (CRYPTO_memcmp(supplied->data(), digest.data(), digest.size()) != 0) return mismatch();
  }
  const auto nonce = body.at("nonce").get<std::string>();
  const std::lock_guard lock(mutex_);
  const auto found = secrets_.find({playbook, *account});
  if (found == secrets_.end() || found->second.digest != digest) return mismatch();  // rotated or revoked meanwhile
  auto& replay = replay_[digest];
  std::erase_if(replay.nonces, [&](const auto& item) { return item.second <= now; });
  while (!replay.accepted.empty() && replay.accepted.front() <= now - 60) replay.accepted.pop_front();
  if (replay.nonces.contains(nonce)) return api_error(409, "SIGNAL_REPLAY", "Signal nonce was already consumed");
  if (replay.accepted.size() >= kRate || replay.nonces.size() >= kNonces) {
    auto error = api_error(429, "SIGNAL_RATE_LIMIT", "Signal budget exhausted; retry later with a new nonce");
    error.retry_after = 60;
    return error;
  }
  replay.accepted.push_back(now);
  replay.nonces[nonce] = expires;
  return {};
}

}  // namespace openport::server
