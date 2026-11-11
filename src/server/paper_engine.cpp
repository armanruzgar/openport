#include "openport/server/engine.hpp"

#include <limits>

namespace openport::server {
std::string journal_warning(std::uint64_t bytes, std::uint64_t records, bool replay) {
  if (bytes < kLargeJournalBytes && records < kLargeJournalRecords) return {};
  return replay ? "Large replay journal: exact verification may be slow; compaction breaks exact run verification" :
      "Large journal: stop openportd and consider --compact-journals after backing up";
}

std::shared_ptr<const TradingView> Engine::trading_view() const { return trading_view(kMainAccount); }

std::shared_ptr<const TradingView> Engine::trading_view(std::string_view account) const {
  const std::lock_guard lock(mutex_);
  const auto it = trading_views_.find(account.empty() ? kMainAccount : account);
  return it == trading_views_.end() ? nullptr : it->second;
}

bool Engine::post_trading(TradingCommand command, TradingCompletion completion) {
  {
    const std::lock_guard lock(command_mutex_);
    if (!accepting_commands_ || stopping_ || commands_.size() >= options_.command_capacity ||
        next_command_ == std::numeric_limits<std::uint64_t>::max()) return false;
    const bool submit = command.kind == TradingCommand::Kind::Submit;
    commands_.push_back({next_command_++, std::move(command), [this, submit, complete = std::move(completion)](TradingReply reply) {
      publish_desk();
      if (submit && options_.notifications && !reply.replayed && !reply.order_id && !reply.decision.ok())
        options_.notifications->publish({"order_rejected", notification_account(reply.account), desk_.market_time(),
            "Order rejected: " + std::string(trading::to_string(reply.decision.code)), {}});
      complete(std::move(reply));
    }});
  }
  // The engine thread may be waiting out its drain timeout on a quiet feed; wake it,
  // as synchronize does, so the command is applied now rather than up to 50 ms later.
  queue_.wake();
  return true;
}

}  // namespace openport::server
