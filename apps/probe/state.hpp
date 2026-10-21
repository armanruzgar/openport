#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "openport/md/provider.hpp"

namespace openport::probe {

using ExpiryKey = std::pair<md::Timestamp, md::Settlement>;
inline ExpiryKey expiry_key(const md::OptionContract& contract) {
  return {contract.expiry_time(), contract.settlement};
}
inline const char* settlement_label(md::Settlement settlement) {
  return settlement == md::Settlement::AM ? "AM" : "PM";
}

/// A successful status is a complete poll only for snapshot providers. Streaming
/// readiness needs actual definitions and quotes for every requested underlying.
class Readiness {
 public:
  Readiness(const md::Subscription& subscription, bool streaming, std::size_t minimum_quotes)
      : streaming_(streaming), minimum_quotes_(minimum_quotes) {
    for (const auto& symbol : subscription.underlyings) progress_.try_emplace(symbol);
  }

  void apply(const md::Event& event) {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) {
      definitions_[definition->id] = definition->contract.underlying;
    } else if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
      const auto found = definitions_.find(quote->id);
      if (found == definitions_.end()) return;
      const auto progress = progress_.find(found->second);
      if (progress != progress_.end()) ++progress->second.quotes;
    } else if (const auto* complete = std::get_if<md::SnapshotComplete>(&event)) {
      const auto progress = progress_.find(complete->underlying);
      if (!streaming_ && progress != progress_.end()) progress->second.snapshot = true;
    } else if (const auto* status = std::get_if<md::ProviderStatus>(&event)) {
      const auto progress = progress_.find(status->underlying);
      if (!streaming_ && progress != progress_.end() &&
          (status->state == md::FeedState::Live || status->state == md::FeedState::Delayed))
        progress->second.snapshot = true;
    }
  }

  [[nodiscard]] bool ready(const std::string& symbol) const {
    const auto& progress = progress_.at(symbol);
    return streaming_ ? progress.quotes >= minimum_quotes_ : progress.snapshot;
  }
  [[nodiscard]] bool all_ready() const {
    return !progress_.empty() && std::all_of(progress_.begin(), progress_.end(),
                                             [&](const auto& entry) { return ready(entry.first); });
  }

 private:
  struct Progress {
    std::size_t quotes = 0;
    bool snapshot = false;
  };
  bool streaming_;
  std::size_t minimum_quotes_;
  std::map<std::string, Progress> progress_;
  std::map<md::InstrumentId, std::string> definitions_;
};

}  // namespace openport::probe
