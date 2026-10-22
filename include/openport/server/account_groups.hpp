#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "openport/trading/money.hpp"
#include "openport/trading/types.hpp"

namespace openport::server {

/// How a copied order's size reaches each account of a group: `Ratio` trades the
/// order's quantity times each member's ratio, rounded down; `Equity` splits the
/// order's quantity across the members in proportion to their marked equity, the
/// largest remainders taking the units rounding leaves, so the members' sizes add up
/// to the order's.
enum class Allocation { Ratio, Equity };

struct GroupMember {
  std::string account;  ///< An account ID; "main" names the main account.
  trading::Money ratio = trading::Money::from_micros(1'000'000);  ///< Ratio allocation: 0.0001 to 100.
  bool operator==(const GroupMember&) const = default;
};

/// A named set of accounts one order can be mirrored to.
struct AccountGroup {
  std::string name;
  Allocation allocation = Allocation::Ratio;
  std::vector<GroupMember> members;
  bool operator==(const AccountGroup&) const = default;
};

inline constexpr std::size_t kMaxGroups = 32;
inline constexpr std::size_t kMaxGroupMembers = 16;

/// Group names use the account ID rules: 1-40 lowercase letters, digits or single hyphens.
[[nodiscard]] bool valid_group_name(std::string_view name);
/// Throws std::invalid_argument unless the group has a valid name, one to sixteen
/// distinct members with valid account IDs and, for ratio allocation, ratios from
/// 0.0001 to 100.
void validate_group(const AccountGroup& group);

/// Each member's quantity of an order of `quantity` units. `equity` gives the members'
/// marked equity, in member order, for equity allocation; a member without positive
/// equity takes none. A member may get zero units.
[[nodiscard]] std::vector<trading::Quantity> allocate(const AccountGroup& group, trading::Quantity quantity,
                                                      const std::vector<std::optional<trading::Money>>& equity = {});

/// The server's account groups, kept in a JSON file beside the main journal (an empty
/// path keeps them in memory). Groups are configuration, not trading: each copied
/// order is an independent submission journaled by its own account. Thread-safe.
class AccountGroups {
 public:
  /// Throws std::runtime_error when an existing file is unreadable or invalid.
  explicit AccountGroups(std::filesystem::path file = {});
  [[nodiscard]] std::vector<AccountGroup> list() const;
  [[nodiscard]] std::optional<AccountGroup> find(std::string_view name) const;
  /// Creates or replaces a validated group; throws std::invalid_argument past kMaxGroups
  /// and std::runtime_error when it cannot be saved (nothing changes then).
  void put(AccountGroup group);
  /// Whether a group by that name existed.
  bool remove(std::string_view name);

 private:
  void save(const std::map<std::string, AccountGroup>& groups) const;
  std::filesystem::path file_;
  mutable std::mutex mutex_;
  std::map<std::string, AccountGroup> groups_;
};

}  // namespace openport::server
