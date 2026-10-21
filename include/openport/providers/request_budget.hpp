#pragma once

#include <algorithm>
#include <chrono>
#include <deque>
#include <stdexcept>

#include "openport/md/time.hpp"

namespace openport::providers {

/// A rolling request limit for a REST API: at most `limit` requests in any
/// `window`, and an exponential backoff after the server throttles. Operational
/// clock, independent of market time.
class RequestBudget {
 public:
  RequestBudget(int limit, md::Timestamp window) : limit_(limit), window_(window) {
    if (limit_ < 1 || window_ <= 0) throw std::invalid_argument("request budget must be positive");
  }

  [[nodiscard]] int limit() const noexcept { return limit_; }
  [[nodiscard]] md::Timestamp window() const noexcept { return window_; }

  /// How long to wait before the next request may be sent.
  [[nodiscard]] std::chrono::milliseconds wait(md::Timestamp now) {
    while (!sent_.empty() && sent_.front() <= now - window_) sent_.pop_front();
    auto due = blocked_until_;
    if (sent_.size() >= static_cast<std::size_t>(limit_)) due = std::max(due, sent_.front() + window_);
    return milliseconds_up(due - now);
  }

  void sent(md::Timestamp now) { sent_.push_back(now); }

  /// The server refused a request for its rate: wait Retry-After when given, and
  /// back off 2, 4, 8... seconds, never less than one window and at most five minutes.
  void throttled(md::Timestamp now, std::chrono::seconds retry_after = std::chrono::seconds(0)) {
    failures_ = std::min(failures_ + 1, 8);
    auto delay = std::max<md::Timestamp>(window_, std::min<md::Timestamp>(300 * md::kNanosPerSecond,
                                         (md::Timestamp{1} << failures_) * md::kNanosPerSecond));
    if (retry_after.count() > 0 && retry_after.count() <= 3600)
      delay = std::max<md::Timestamp>(delay, retry_after.count() * md::kNanosPerSecond);
    blocked_until_ = std::max(blocked_until_, now + delay);
  }

  void succeeded() { failures_ = 0; }

  /// The shortest whole-second poll interval that keeps a cycle of `requests` in budget.
  [[nodiscard]] std::chrono::seconds interval(std::size_t requests) const {
    const auto nanos = static_cast<md::Timestamp>(requests) * window_ / limit_;
    return std::chrono::seconds(std::max<md::Timestamp>(1, (nanos + md::kNanosPerSecond - 1) / md::kNanosPerSecond));
  }

 private:
  static std::chrono::milliseconds milliseconds_up(md::Timestamp value) {
    constexpr md::Timestamp kNanosPerMillisecond = 1'000'000;
    return std::chrono::milliseconds(value <= 0 ? 0 : 1 + (value - 1) / kNanosPerMillisecond);
  }

  int limit_;
  md::Timestamp window_;
  int failures_ = 0;
  md::Timestamp blocked_until_ = 0;
  std::deque<md::Timestamp> sent_;
};

}  // namespace openport::providers
