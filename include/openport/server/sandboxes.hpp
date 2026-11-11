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
    /// Creations per client address per creation_window, and sandboxes it may hold at once.
    std::size_t per_client = 3;
    std::size_t global = 30;
    std::chrono::seconds creation_window{60 * 60};
    std::size_t orders = 60;
    std::chrono::seconds order_window{60};
    /// Cancels, closes, exercise and the other exits, per order_window.
    std::size_t exits = 60;
    std::function<Clock::time_point()> clock = Clock::now;
  };
  explicit Sandboxes(Options options);
  [[nodiscard]] std::chrono::seconds idle() const { return options_.idle; }
  void create(const ApiRequest& request, MetricsSource& source, ApiCompletion complete);
  [[nodiscard]] std::optional<std::string> authenticate(std::string_view secret);
  [[nodiscard]] bool active(const std::string& account, bool touch = false);
  /// Whether `account` is a sandbox not yet removed: being created, active or expired.
  [[nodiscard]] bool contains(const std::string& account);
  /// Counts an order request or returns its rate-limit response.
  [[nodiscard]] std::optional<ApiResponse> check_order(const std::string& account);
  /// Counts a cancel, close, exercise or other exit, or returns its rate-limit
  /// response. Exits have a budget of their own, so a spent order budget never traps
  /// a visitor in a position; the engine journals an exit it refuses (an unknown
  /// order, say) like any other command, so they cannot go unlimited either.
  [[nodiscard]] std::optional<ApiResponse> check_exit(const std::string& account);
  /// Expired tokens stop working immediately; capacity returns after file removal.
  [[nodiscard]] std::vector<std::string> expired();
  void removed(const std::string& account);
  /// A creation that failed: its sandbox expires at once and its budget is given back.
  void failed(const std::string& account);

 private:
  struct Entry {
    std::string digest;
    Clock::time_point used;
    bool ready = false;
    std::deque<Clock::time_point> orders;
    std::deque<Clock::time_point> exits = {};
    std::string client = {};  ///< the address that created it, which may hold per_client at once
    Clock::time_point created = {};  ///< when it was reserved, in creations_ and its client's bucket
  };
  [[nodiscard]] std::optional<ApiResponse> charge(const std::string& account, bool exit_budget);
  /// Gives back the creation budget a reservation took when no sandbox came of it.
  /// Caller holds mutex_.
  void refund(const Entry& entry);
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
