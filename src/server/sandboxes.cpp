#include "openport/server/sandboxes.hpp"

#include <algorithm>
#include <array>
#include <boost/asio/ip/address.hpp>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdexcept>

namespace openport::server {
namespace {
std::string random_hex() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("Cannot generate sandbox credentials");
  constexpr char hex[] = "0123456789abcdef";
  std::string value;
  for (const auto byte : bytes) { value += hex[byte >> 4]; value += hex[byte & 15]; }
  return value;
}
std::string digest(std::string_view secret) {
  std::array<unsigned char, 32> bytes{};
  unsigned int size = 0;
  if (EVP_Digest(secret.data(), secret.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("Cannot authenticate sandbox credentials");
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
int retry_seconds(Sandboxes::Clock::duration remaining) {
  return static_cast<int>(std::max(std::chrono::seconds{1}, std::chrono::ceil<std::chrono::seconds>(remaining)).count());
}
void prune(std::deque<Sandboxes::Clock::time_point>& times, Sandboxes::Clock::time_point cutoff) {
  while (!times.empty() && times.front() <= cutoff) times.pop_front();
}
}

Sandboxes::Sandboxes(Options options) : options_(std::move(options)) {
  if (!options_.capacity || options_.idle.count() <= 0 || !options_.per_client || !options_.global ||
      !options_.orders || options_.creation_window.count() <= 0 || options_.order_window.count() <= 0)
    throw std::invalid_argument("Invalid sandbox limits");
}

void Sandboxes::create(const ApiRequest& request, MetricsSource& source, ApiCompletion complete) {
  const auto body = request.body.empty() ? nlohmann::json::object() : nlohmann::json::parse(request.body, nullptr, false);
  if (request.target != "/api/sandboxes" || !body.is_object() || !body.empty()) {
    complete(api_error(400, "INVALID_REQUEST", "Sandbox creation takes no options"));
    return;
  }
  std::string id, secret;
  std::optional<ApiResponse> rejection;
  {
    const std::lock_guard lock(mutex_);
    const auto now = options_.clock();
    prune(creations_, now - options_.creation_window);
    // Only successful reservations allocate a client bucket; global limits bound memory.
    for (auto& [client_ip, times] : clients_) { (void)client_ip; prune(times, now - options_.creation_window); }
    std::erase_if(clients_, [](const auto& item) { return item.second.empty(); });
    const auto client = clients_.find(request.client_ip);
    if (entries_.size() >= options_.capacity)
      rejection = api_error(429, "SANDBOX_CAPACITY", "All sandbox accounts are in use. Try again later.");
    else if (creations_.size() >= options_.global) {
      rejection = api_error(429, "SANDBOX_GLOBAL_RATE", "Sandbox creation is busy. Try again later.");
      rejection->retry_after = retry_seconds(creations_.front() + options_.creation_window - now);
    } else if (client != clients_.end() && client->second.size() >= options_.per_client) {
      rejection = api_error(429, "SANDBOX_CLIENT_RATE", "Too many sandbox accounts from this client. Try again later.");
      rejection->retry_after = retry_seconds(client->second.front() + options_.creation_window - now);
    } else {
      do { id = "sbox-" + random_hex(); } while (entries_.contains(id));
      secret = "sandbox_" + random_hex();
      entries_.emplace(id, Entry{digest(secret), now, false, {}});
      creations_.push_back(now);
      clients_[request.client_ip].push_back(now);
    }
  }
  if (rejection) { complete(std::move(*rejection)); return; }
  TradingCommand command;
  command.kind = TradingCommand::Kind::CreateSandbox;
  command.account = id;
  command.actor = id;
  if (!source.post_trading(std::move(command), [this, id, secret, complete](TradingReply reply) {
        if (!reply.decision.ok() || !reply.error_code.empty() || reply.account.empty()) {
          failed(id);
          complete(api_error(503, "SANDBOX_UNAVAILABLE", "Cannot create a sandbox account; trading storage is unavailable."));
          return;
        }
        {
          const std::lock_guard lock(mutex_);
          auto& entry = entries_.at(id);
          entry.ready = true;
          entry.used = options_.clock();
        }
        complete({201, nlohmann::json{{"account", id}, {"token", secret},
            {"idle_seconds", options_.idle.count()}, {"simulated", true}}.dump()});
      })) {
    removed(id);
    auto busy = api_error(503, "SANDBOX_UNAVAILABLE", "Sandbox command inbox is full or trading is unavailable.");
    busy.retry_after = kInboxRetrySeconds;
    complete(std::move(busy));
  }
}

std::optional<std::string> Sandboxes::authenticate(std::string_view secret) {
  const auto supplied = digest(secret);
  const std::lock_guard lock(mutex_);
  const auto now = options_.clock();
  for (auto& [id, entry] : entries_) {
    if (entry.ready && now - entry.used < options_.idle &&
        CRYPTO_memcmp(entry.digest.data(), supplied.data(), supplied.size()) == 0) {
      entry.used = now;
      return id;
    }
  }
  return {};
}
bool Sandboxes::active(const std::string& account, bool touch) {
  const std::lock_guard lock(mutex_);
  const auto found = entries_.find(account);
  const auto now = options_.clock();
  if (found == entries_.end() || !found->second.ready || now - found->second.used >= options_.idle) return false;
  if (touch) found->second.used = now;
  return true;
}
std::optional<ApiResponse> Sandboxes::check_order(const std::string& account) {
  const std::lock_guard lock(mutex_);
  const auto found = entries_.find(account);
  auto rejected = api_error(429, "SANDBOX_ORDER_RATE", "Too many sandbox order requests. Try again later.");
  if (found == entries_.end() || !found->second.ready) return rejected;
  auto& times = found->second.orders;
  const auto now = options_.clock();
  prune(times, now - options_.order_window);
  if (times.size() >= options_.orders) {
    rejected.retry_after = retry_seconds(times.front() + options_.order_window - now);
    return rejected;
  }
  times.push_back(now);
  return {};
}
std::vector<std::string> Sandboxes::expired() {
  const std::lock_guard lock(mutex_);
  const auto now = options_.clock();
  std::vector<std::string> ids;
  for (const auto& [id, entry] : entries_)
    if (entry.ready && now - entry.used >= options_.idle) ids.push_back(id);
  return ids;
}
void Sandboxes::failed(const std::string& account) {
  const std::lock_guard lock(mutex_);
  auto& entry = entries_.at(account);
  entry.ready = true;
  entry.used = options_.clock() - options_.idle;
}
void Sandboxes::removed(const std::string& account) {
  const std::lock_guard lock(mutex_);
  entries_.erase(account);
}
std::string sandbox_client_ip(std::string_view peer, std::string_view configured_header,
                              std::string_view supplied, bool duplicate) {
  if (configured_header.empty() || duplicate || supplied.empty()) return std::string(peer);
  boost::system::error_code error;
  const auto address = boost::asio::ip::make_address(std::string(supplied), error);
  return error ? std::string(peer) : address.to_string();
}

bool sandbox_visible(const AccountStatus& account, const ApiAccess& access) {
  if (!access.sandbox.empty()) return account.id == access.sandbox;
  return !account.sandbox_idle_seconds || access.admin;
}
std::optional<ApiResponse> sandbox_visibility(const ApiRequest& request, const MetricsSource& source) {
  if (request.target.find('?') == std::string::npos) return {};
  // Decoded as the routes decode it: %61ccount=sbox-... names the sandbox too.
  const auto id = query_account(request.target, request.access.sandbox);
  if (!id) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  for (const auto& account : source.status().accounts)
    if (account.id == *id && !sandbox_visible(account, request.access))
      return api_error(404, "UNKNOWN_ACCOUNT", "Unknown paper account");
  return {};
}
std::string sandbox_tick(std::string message, const ApiAccess& access) {
  auto body = nlohmann::json::parse(message);
  if (!body.contains("accounts")) return message;
  auto& accounts = body.at("accounts");
  std::erase_if(accounts.get_ref<nlohmann::json::array_t&>(), [&](const auto& account) {
    return !access.sandbox.empty() ? account.at("id") != access.sandbox
        : account.value("sandbox_idle_seconds", 0) != 0 && !access.admin;
  });
  if (!access.sandbox.empty()) {
    body["trading"] = accounts.empty() ? nlohmann::json(nullptr) : accounts.front().at("trading");
    body.erase("notifications");
  }
  return body.dump();
}
}  // namespace openport::server
