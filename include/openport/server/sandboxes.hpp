#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "openport/server/api.hpp"

namespace openport::server {

/// Ephemeral credentials and abuse limits. This clock never enters the reducer.
class Sandboxes {
 public:
  using Clock = std::chrono::steady_clock;
  struct Options {
    std::size_t capacity = 0;
    std::chrono::seconds idle{24 * 60 * 60};
    std::size_t per_client = 3;
    std::size_t global = 30;
    std::chrono::seconds creation_window{60 * 60};
    std::size_t orders = 60;
    std::chrono::seconds order_window{60};
    std::function<Clock::time_point()> clock = Clock::now;
  };
  explicit Sandboxes(Options options);
  [[nodiscard]] std::chrono::seconds idle() const { return options_.idle; }
  void create(const ApiRequest& request, MetricsSource& source, ApiCompletion complete);
  [[nodiscard]] std::optional<std::string> authenticate(std::string_view secret);
  [[nodiscard]] bool active(const std::string& account, bool touch = false);
  /// Counts an order request or returns its rate-limit response.
  [[nodiscard]] std::optional<ApiResponse> check_order(const std::string& account);
  /// Expired tokens stop working immediately; capacity returns after file removal.
  [[nodiscard]] std::vector<std::string> expired();
  void removed(const std::string& account);
  void failed(const std::string& account);

 private:
  struct Entry {
    std::string digest;
    Clock::time_point used;
    bool ready = false;
    std::deque<Clock::time_point> orders;
  };
  Options options_;
  std::mutex mutex_;
  std::map<std::string, Entry> entries_;
  std::map<std::string, std::deque<Clock::time_point>> clients_;
  std::deque<Clock::time_point> creations_;
};

/// Accept one canonical IP literal, never a comma-separated forwarded chain.
[[nodiscard]] std::string sandbox_client_ip(std::string_view peer, std::string_view configured_header,
                                           std::string_view supplied, bool duplicate = false);
[[nodiscard]] bool sandbox_visible(const AccountStatus& account, const ApiAccess& access);
[[nodiscard]] std::optional<ApiResponse> sandbox_visibility(const ApiRequest& request, const MetricsSource& source);
/// Applies the same account visibility to REST status and WebSocket snapshots.
[[nodiscard]] std::string sandbox_tick(std::string message, const ApiAccess& access);

}  // namespace openport::server
