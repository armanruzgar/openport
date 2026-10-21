#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

#include "openport/net/http.hpp"
#include "openport/server/paper.hpp"

namespace openport::server {

struct NotificationChannel {
  std::string id;
  std::string type;
  std::string url;
  std::string token;
  std::string chat_id;
  bool enabled = true;
  std::set<std::string> events;
  trading::Money floor_distance = trading::Money::from_micros(500'000'000);
};
struct NotificationConfig {
  std::size_t capacity = 256;
  std::vector<NotificationChannel> channels;
  bool include_simulated = false;
};
/// Parsing and file errors deliberately omit input, paths and JSON diagnostics.
[[nodiscard]] NotificationConfig parse_notification_config(std::string_view text);
[[nodiscard]] NotificationConfig load_notification_config(const std::filesystem::path& file,
    const std::function<std::string(const char*)>& environment);

struct NotificationEvent {
  std::string kind;
  std::string account;
  md::Timestamp time = 0;
  std::string message;
  nlohmann::json details = nlohmann::json::object();
};

/// One bounded, memory-only delivery queue. No network or logging on producers.
/// Observation, publication, settings and status are thread-safe.
class Notifications {
 public:
  struct Options {
    std::function<std::chrono::steady_clock::time_point()> clock = std::chrono::steady_clock::now;
    std::function<md::Timestamp()> wall_clock = md::now;
    std::function<void(std::uint64_t)> warn = [](std::uint64_t dropped) {
      std::fprintf(stderr, "notifications: dropped %llu deliveries (queue full or shutdown)\n",
                   static_cast<unsigned long long>(dropped));
    };
    bool worker = true;  ///< False allows deterministic pumping with deliver_one in tests.
  };
  Notifications(NotificationConfig config, std::unique_ptr<net::HttpClient> http, Options options);
  ~Notifications();
  Notifications(const Notifications&) = delete;
  Notifications& operator=(const Notifications&) = delete;
  void stop();
  void publish(const NotificationEvent& event);
  using Subscriber = std::function<void(const nlohmann::json&)>;
  /// Receives only `account`'s events from `source` ("live" or "replay"); JSON is built
  /// only when such a subscriber exists. Callbacks must only enqueue bounded work; never
  /// block on I/O. No history is replayed. Floor events arrive only on entering or
  /// leaving the default warning band, not on every observation.
  std::uint64_t subscribe(std::string account, std::string source, Subscriber subscriber);
  void unsubscribe(std::uint64_t subscription);
  /// History primes observation without delivering recovered or fast-forwarded events.
  void observe(std::string_view account, const TradingView& view, bool history = false);
  void remove_account(const std::string& account, bool discard_pending = true);
  [[nodiscard]] bool includes_simulated() const { return include_simulated_; }
  /// 202 queued, 404 unknown channel, 409 disabled, 429 full, 503 stopped.
  int test(std::string_view channel);
  /// Only nonsecret settings can be changed; invalid input throws a redacted error.
  bool configure(std::string_view channel, const nlohmann::json& settings);
  [[nodiscard]] nlohmann::json status() const;
  /// Runs at most one due request, outside the queue mutex. Only the worker calls
  /// this in production; tests call it with Options::worker=false.
  bool deliver_one();

 private:
  using Time = std::chrono::steady_clock::time_point;
  struct Channel {
    NotificationChannel config;
    Time next{};
    std::uint64_t delivered = 0, failures = 0, dropped = 0;
    md::Timestamp last_attempt = 0, last_delivery = 0;
    std::string last_error;
    std::map<std::string, bool> near_floor;
  };
  struct Pending {
    std::size_t channel;
    NotificationEvent event;
    int attempts = 0;
    Time due{};
  };
  struct Seen {
    std::shared_ptr<const trading::TradingSnapshot> snapshot;
    std::string playbooks;
    std::set<std::string> stages;
  };
  bool enqueue(std::size_t channel, const NotificationEvent& event);
  void run();
  std::size_t capacity_;
  bool include_simulated_;
  std::unique_ptr<net::HttpClient> http_;
  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<Channel> channels_;
  std::deque<Pending> queue_;
  std::size_t in_flight_ = 0;
  std::uint64_t dropped_ = 0, warned_ = 0;
  std::map<std::string, Time> limits_;
  // Keyed by stream account and source, so a publication touches only its own listeners.
  std::map<std::pair<std::string, std::string>, std::map<std::uint64_t, Subscriber>> subscribers_;
  std::map<std::string, bool> stream_near_floor_;  // by publisher account, like Channel::near_floor
  std::uint64_t subscription_ = 0, sequence_ = 0;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::mutex observation_mutex_;
  std::map<std::string, Seen> seen_;  // observation_mutex_; account names include the replay run
};

}  // namespace openport::server
