#pragma once

#include <chrono>
#include <memory>
#include <string_view>
#include <vector>

#include "openport/md/provider.hpp"

namespace openport::providers {

/// Names of the providers this build supports, e.g. {"cboe"}.
[[nodiscard]] std::vector<std::string_view> provider_names();

/// Rejects filters that cannot be honored by an upstream feed or recorded session.
void validate_subscription(std::string_view provider, const md::Subscription& subscription);

/// Creates a provider by name. Throws std::invalid_argument for an unknown name
/// or a missing API key/recording option. Replay opens and validates its header here.
[[nodiscard]] std::unique_ptr<md::Provider> make_provider(const md::ProviderConfig& config);

/// A FailoverProvider: `backup` takes over an underlying once `primary` has been
/// in Error, Stopped or silent for `after`. Neither may be replay or demo, and the
/// two must differ.
[[nodiscard]] std::unique_ptr<md::Provider> make_failover_provider(const md::ProviderConfig& primary,
                                                                   const md::ProviderConfig& backup,
                                                                   std::chrono::seconds after);

}  // namespace openport::providers
