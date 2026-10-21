#include "openport/server/flow.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace openport::server {
namespace {
/// Option prices are quoted in cents or nickels; half a cent tells "at" from "near".
constexpr double kAtQuote = 0.005;

bool whole_ratio(double a, double b) {
  const double low = std::min(a, b), high = std::max(a, b);
  if (!(low > 0)) return false;
  const double ratio = high / low;
  return ratio <= 4 + 1e-9 && std::abs(ratio - std::round(ratio)) < 1e-9;
}
}  // namespace

std::string_view to_string(PrintSide side) noexcept {
  switch (side) {
    case PrintSide::Bid: return "bid";
    case PrintSide::Ask: return "ask";
    case PrintSide::Mid: return "mid";
    case PrintSide::Outside: return "outside";
    case PrintSide::Unquoted: return "unquoted";
  }
  return "unquoted";
}

std::optional<PrintSide> parse_print_side(std::string_view text) noexcept {
  for (const auto side : {PrintSide::Bid, PrintSide::Ask, PrintSide::Mid, PrintSide::Outside, PrintSide::Unquoted})
    if (to_string(side) == text) return side;
  return std::nullopt;
}

std::string contract_expiry_id(const md::OptionContract& contract) {
  const bool named = contract.root == "OEX" || contract.root == "XEO";
  return md::format_date(contract.expiry) + (contract.settlement == md::Settlement::AM ? "AM" : "PM") +
         (named ? "-" + contract.root : "");
}

FlowTape::FlowTape(Options options) : options_(options) {
  options_.capacity = std::max<std::size_t>(options_.capacity, 1);
}

void FlowTape::add(const md::OptionTrade& trade, const analytics::OptionState& option, bool simulated) {
  if (!(trade.price > 0) || !std::isfinite(trade.price) || !(trade.size > 0) || !std::isfinite(trade.size)) return;
  FlowPrint print;
  print.time = trade.ts;
  print.symbol = option.contract.osi_symbol();
  print.underlying = option.contract.underlying;
  print.expiry = contract_expiry_id(option.contract);
  print.strike = option.contract.strike;
  print.type = option.contract.type;
  print.multiplier = option.contract.multiplier;
  print.price = trade.price;
  print.size = trade.size;
  print.venue = trade.venue;
  print.simulated = simulated;
  print.block = trade.size >= options_.block_size;
  print.bid = print.ask = std::numeric_limits<double>::quiet_NaN();
  if (option.two_sided()) {
    print.bid = option.bid;
    print.ask = option.ask;
    print.quote_time = option.quote_ts;
    if (std::abs(trade.price - option.ask) <= kAtQuote) {
      print.side = PrintSide::Ask;
      print.direction = 1;
    } else if (std::abs(trade.price - option.bid) <= kAtQuote) {
      print.side = PrintSide::Bid;
      print.direction = -1;
    } else if (trade.price > option.bid && trade.price < option.ask) {
      print.side = PrintSide::Mid;
    } else {
      print.side = PrintSide::Outside;
      print.direction = trade.price > option.ask ? 1 : -1;
    }
  }
  const std::lock_guard lock(mutex_);
  print.sequence = ++sequence_;
  auto& tape = tapes_[print.underlying];
  tape.prints.push_back(std::move(print));
  ++tape.total;
  mark_sweep(tape);
  mark_legs(tape);
  while (tape.prints.size() > options_.capacity) {
    tape.prints.pop_front();
    ++tape.evicted;
  }
}

void FlowTape::mark_sweep(Tape& tape) {
  const auto& last = tape.prints.back();
  if (last.direction == 0 || last.venue == 0) return;
  std::vector<FlowPrint*> members;
  std::set<std::uint16_t> venues;
  for (auto it = tape.prints.rbegin(); it != tape.prints.rend(); ++it) {
    if (it->time < last.time - options_.sweep_window) break;
    if (it->symbol != last.symbol || it->direction != last.direction || it->venue == 0 ||
        std::abs(it->time - last.time) > options_.sweep_window) continue;
    members.push_back(&*it);
    venues.insert(it->venue);
  }
  if (venues.size() < 2) return;
  for (auto* member : members) member->sweep = true;
}

void FlowTape::mark_legs(Tape& tape) {
  auto& last = tape.prints.back();
  for (auto it = std::next(tape.prints.rbegin()); it != tape.prints.rend(); ++it) {
    if (it->time < last.time - options_.leg_window) break;
    if (it->symbol == last.symbol || it->venue != last.venue || std::abs(it->time - last.time) > options_.leg_window ||
        !whole_ratio(it->size, last.size)) continue;
    if (!it->group) it->group = ++groups_;
    last.group = it->group;
    return;
  }
}

FlowTape::Snapshot FlowTape::snapshot(const std::string& underlying) const {
  const std::lock_guard lock(mutex_);
  Snapshot out;
  const auto found = tapes_.find(underlying);
  if (found == tapes_.end()) return out;
  out.prints.assign(found->second.prints.begin(), found->second.prints.end());
  out.total = found->second.total;
  out.evicted = found->second.evicted;
  out.simulated = std::any_of(out.prints.begin(), out.prints.end(), [](const FlowPrint& p) { return p.simulated; });
  return out;
}

std::optional<md::OptionTrade> simulated_print(const md::OptionVolume& volume, const analytics::OptionState& option,
                                               double increment) {
  if (!option.two_sided() || !(increment > 0) || !std::isfinite(increment)) return std::nullopt;
  // A fixed mix from the contract and time: four in ten at the bid, four at the ask, two between.
  auto mix = static_cast<std::uint64_t>(volume.ts) ^ (static_cast<std::uint64_t>(volume.id) * 0x9E3779B97F4A7C15ULL);
  mix ^= mix >> 33;
  mix *= 0xff51afd7ed558ccdULL;
  mix ^= mix >> 33;
  const auto bucket = mix % 10;
  const double price = bucket < 4 ? option.bid : bucket < 8 ? option.ask : std::round(option.mid() * 100) / 100;
  return md::OptionTrade{volume.id, volume.ts, price, increment, 0};
}

}  // namespace openport::server
