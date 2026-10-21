#include "openport/providers/replay_batches.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace openport::providers {
namespace {
md::Timestamp market_time(const md::Event& event) {
  return std::visit([](const auto& value) -> md::Timestamp {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::OptionQuote> || std::is_same_v<T, md::OptionTrade> ||
                  std::is_same_v<T, md::UnderlyingQuote> || std::is_same_v<T, md::SnapshotHeartbeat> ||
                  std::is_same_v<T, md::TradingHalt>) return value.ts;
    else return 0;
  }, event);
}
}
ReplayBatches::ReplayBatches(md::RecordingReader& reader, const md::Subscription& subscription, bool instants)
    : reader_(reader), symbols_(subscription.underlyings.begin(), subscription.underlyings.end()),
      snapshots_(false), instants_(instants) {
  if (const auto mode = reader_.indexed_snapshot_mode()) { snapshots_ = *mode; return; }
  // Older recordings predate SnapshotComplete. They use the streaming clock
  // even when their header describes a polling provider.
  while (const auto record = reader_.next()) {
    if (std::holds_alternative<md::SnapshotComplete>(record->event) || std::holds_alternative<md::SnapshotHeartbeat>(record->event)) { snapshots_ = true; break; }
  }
  if (!reader_.diagnostic().empty()) throw std::runtime_error("replay: " + reader_.diagnostic());
  reader_.rewind();
}
bool ReplayBatches::include(const md::Event& event) {
  return std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::ContractDefinition>) {
      return admitted_[value.id] = symbols_.contains(value.contract.underlying);
    } else if constexpr (std::is_same_v<T, md::UnderlyingQuote> || std::is_same_v<T, md::UnderlyingClose>) {
      return symbols_.contains(value.symbol);
    } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
      return value.underlying.empty() || symbols_.contains(value.underlying);
    } else if constexpr (std::is_same_v<T, md::SnapshotComplete> || std::is_same_v<T, md::SnapshotHeartbeat>) {
      return symbols_.contains(value.underlying);
    } else if constexpr (std::is_same_v<T, md::TradingHalt>) {
      return value.underlying.empty() || symbols_.contains(value.underlying);
    } else {
      const auto it = admitted_.find(value.id);
      if (it == admitted_.end()) throw std::runtime_error("replay: event references undefined contract " + std::to_string(value.id));
      return it->second;
    }
  }, event);
}
std::optional<ReplayBatches::Part> ReplayBatches::read() {
  Part part;
  auto& batch = part.batch;
  md::Timestamp end = 0;
  while (true) {
    auto record = pending_ ? std::exchange(pending_, {}) : reader_.next();
    if (!record) {
      if (!reader_.diagnostic().empty()) throw std::runtime_error("replay: " + reader_.diagnostic());
      break;
    }
    if (!include(record->event)) continue;
    const auto time = std::max(time_, market_time(record->event));
    if (!snapshots_ && end > 0 && time > end) {
      pending_ = std::move(record);
      break;
    }
    time_ = time;
    if (!snapshots_ && end == 0 && time > 0) end = ((time - 1) / md::kNanosPerSecond + 1) * md::kNanosPerSecond;
    batch.received = record->received;
    const auto* complete = std::get_if<md::SnapshotComplete>(&record->event);
    if (complete) part.complete = complete->ts;
    if (const auto* heartbeat = std::get_if<md::SnapshotHeartbeat>(&record->event)) part.complete = heartbeat->ts;
    batch.events.push_back(std::move(record->event));
    if (snapshots_ && part.complete) break;
  }
  if (batch.events.empty()) return {};
  batch.time = snapshots_ ? time_ : std::max(time_, end);
  return part;
}
std::optional<ReplayBatch> ReplayBatches::next() {
  if (!ahead_ && failure_) std::rethrow_exception(failure_);
  auto part = ahead_ ? std::exchange(ahead_, {}) : read();
  if (!part) return {};
  // A feed polls its underlyings one after another. The next snapshot belongs to this
  // market instant when its SnapshotComplete names the same time and none of its events
  // is later. A snapshot that changed nothing moves no clock, so its own time counts too.
  while (instants_ && part->complete) {
    std::optional<Part> following;
    // A truncated or damaged input still delivers every complete snapshot before the
    // damage, as it did when each snapshot was its own batch; the next call fails.
    try { following = read(); } catch (...) { failure_ = std::current_exception(); break; }
    if (!following) break;
    if (following->complete != part->complete || following->batch.time > std::max(part->batch.time, *part->complete)) {
      ahead_ = std::move(following);
      break;
    }
    auto& events = part->batch.events;
    events.insert(events.end(), std::make_move_iterator(following->batch.events.begin()),
                  std::make_move_iterator(following->batch.events.end()));
    part->batch.time = following->batch.time;
    part->batch.received = following->batch.received;
  }
  return std::move(part->batch);
}
}  // namespace openport::providers
