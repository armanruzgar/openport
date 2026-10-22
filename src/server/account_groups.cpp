#include "openport/server/account_groups.hpp"

#include <algorithm>
#include <cerrno>
#include <numeric>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace openport::server {
namespace {
using json = nlohmann::json;
using trading::Money;
using trading::Quantity;
using Wide = __int128;

constexpr std::size_t kFileMax = 1 << 20;
const Money kMinRatio = Money::from_micros(100);
const Money kMaxRatio = Money::from_micros(100'000'000);

const char* allocation_name(Allocation a) { return a == Allocation::Equity ? "equity" : "ratio"; }

json group_json(const AccountGroup& g) {
  json members = json::array();
  for (const auto& m : g.members) members.push_back({{"account", m.account}, {"ratio", m.ratio.str()}});
  return {{"name", g.name}, {"allocation", allocation_name(g.allocation)}, {"members", members}};
}

AccountGroup group_from(const json& j) {
  AccountGroup g;
  g.name = j.at("name").get<std::string>();
  const auto allocation = j.at("allocation").get<std::string>();
  if (allocation != "ratio" && allocation != "equity") throw std::invalid_argument("Unknown allocation");
  g.allocation = allocation == "equity" ? Allocation::Equity : Allocation::Ratio;
  for (const auto& m : j.at("members")) g.members.push_back({m.at("account").get<std::string>(), Money::parse(m.at("ratio").get<std::string>())});
  validate_group(g);
  return g;
}
}  // namespace

bool valid_group_name(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' && id.find("--") == std::string_view::npos &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

void validate_group(const AccountGroup& group) {
  if (!valid_group_name(group.name))
    throw std::invalid_argument("A group name is 1-40 lowercase letters, digits or single hyphens");
  if (group.members.empty() || group.members.size() > kMaxGroupMembers)
    throw std::invalid_argument("A group has one to sixteen member accounts");
  std::set<std::string> seen;
  for (const auto& m : group.members) {
    if (!valid_group_name(m.account)) throw std::invalid_argument("Members must be account IDs");
    if (!seen.insert(m.account).second) throw std::invalid_argument("A group names each account once");
    if (group.allocation == Allocation::Ratio && (m.ratio < kMinRatio || m.ratio > kMaxRatio))
      throw std::invalid_argument("Ratios are from 0.0001 to 100");
  }
}

std::vector<Quantity> allocate(const AccountGroup& group, Quantity quantity, const std::vector<std::optional<Money>>& equity) {
  std::vector<Quantity> sizes(group.members.size(), 0);
  if (quantity <= 0) return sizes;
  if (group.allocation == Allocation::Ratio) {
    for (std::size_t i = 0; i < sizes.size(); ++i)
      sizes[i] = static_cast<Quantity>(static_cast<Wide>(quantity) * group.members[i].ratio.micros() / 1'000'000);
    return sizes;
  }
  // Equity: the floors of each member's share, then the largest remainders (earlier
  // members first among equal ones) take what rounding left.
  std::vector<Wide> weights(sizes.size(), 0);
  Wide total = 0;
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    if (i < equity.size() && equity[i] && *equity[i] > Money{}) weights[i] = equity[i]->micros();
    total += weights[i];
  }
  if (total == 0) return sizes;
  std::vector<Wide> remainders(sizes.size(), 0);
  Quantity given = 0;
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    const Wide share = static_cast<Wide>(quantity) * weights[i];
    sizes[i] = static_cast<Quantity>(share / total);
    remainders[i] = share % total;
    given += sizes[i];
  }
  std::vector<std::size_t> order(sizes.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return remainders[a] > remainders[b]; });
  for (std::size_t k = 0; given < quantity && k < order.size(); ++k) {
    if (weights[order[k]] == 0) continue;
    ++sizes[order[k]];
    ++given;
  }
  return sizes;
}

AccountGroups::AccountGroups(std::filesystem::path file) : file_(std::move(file)) {
  if (file_.empty()) return;
  const int fd = ::open(file_.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error("Cannot open " + file_.filename().string());
  }
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || static_cast<std::size_t>(info.st_size) > kFileMax)
    throw std::runtime_error(file_.filename().string() + " is not a regular file of at most 1 MiB");
  std::string contents;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof buffer);
    if (count < 0) throw std::runtime_error("Cannot read " + file_.filename().string());
    if (count == 0) break;
    contents.append(buffer, static_cast<std::size_t>(count));
    if (contents.size() > kFileMax) throw std::runtime_error(file_.filename().string() + " exceeds 1 MiB");
  }
  try {
    const auto doc = json::parse(contents);
    if (doc.at("schema") != 1) throw std::invalid_argument("Unknown schema");
    for (const auto& item : doc.at("groups")) {
      auto group = group_from(item);
      groups_[group.name] = std::move(group);
    }
  } catch (const std::exception& e) {
    throw std::runtime_error(file_.filename().string() + " is invalid: " + e.what());
  }
}

std::vector<AccountGroup> AccountGroups::list() const {
  const std::lock_guard lock(mutex_);
  std::vector<AccountGroup> all;
  for (const auto& [name, group] : groups_) all.push_back(group);
  return all;
}

std::optional<AccountGroup> AccountGroups::find(std::string_view name) const {
  const std::lock_guard lock(mutex_);
  const auto it = groups_.find(std::string(name));
  if (it == groups_.end()) return std::nullopt;
  return it->second;
}

void AccountGroups::put(AccountGroup group) {
  validate_group(group);
  const std::lock_guard lock(mutex_);
  auto next = groups_;
  if (!next.contains(group.name) && next.size() >= kMaxGroups) throw std::invalid_argument("At most 32 account groups");
  next[group.name] = std::move(group);
  save(next);
  groups_ = std::move(next);
}

bool AccountGroups::remove(std::string_view name) {
  const std::lock_guard lock(mutex_);
  auto next = groups_;
  if (next.erase(std::string(name)) == 0) return false;
  save(next);
  groups_ = std::move(next);
  return true;
}

void AccountGroups::save(const std::map<std::string, AccountGroup>& groups) const {
  if (file_.empty()) return;
  json list = json::array();
  for (const auto& [name, group] : groups) list.push_back(group_json(group));
  const auto text = json{{"schema", 1}, {"groups", list}}.dump(2) + "\n";
  const auto temporary = file_.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("Cannot write account groups");
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  const bool synced = written && ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) throw std::runtime_error("Cannot sync account groups");
  std::filesystem::rename(temporary, file_);
  const int directory = ::open(file_.parent_path().empty() ? "." : file_.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (directory >= 0) { (void)::fsync(directory); ::close(directory); }
}

}  // namespace openport::server
