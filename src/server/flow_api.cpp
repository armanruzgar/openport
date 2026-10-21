#include "flow_api.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>
#include <tuple>

#include "openport/providers/demo.hpp"
#include "openport/server/flow.hpp"
#include "paper_json.hpp"

namespace openport::server {
namespace {
using nlohmann::json;

json number(double value, double scale = 1e4) {
  return std::isfinite(value) ? json(std::round(value * scale) / scale) : json(nullptr);
}
json dollars(double value) { return number(value, 100); }


/// Premium by direction: what aggressive buyers paid, what aggressive sellers took
/// in, and what printed between the quotes or without one.
struct Premium {
  double call_net = 0, put_net = 0, bought = 0, sold = 0, unclassified = 0, contracts = 0;
  std::size_t prints = 0;
  void add(const FlowPrint& p) {
    const double premium = p.premium();
    (p.type == pricing::OptionType::Call ? call_net : put_net) += p.direction * premium;
    if (p.direction > 0) bought += premium;
    else if (p.direction < 0) sold += premium;
    else unclassified += premium;
    contracts += p.size;
    ++prints;
  }
  [[nodiscard]] json body() const {
    return {{"call_net", dollars(call_net)}, {"put_net", dollars(put_net)}, {"net", dollars(call_net + put_net)},
            {"bought", dollars(bought)}, {"sold", dollars(sold)}, {"unclassified", dollars(unclassified)},
            {"contracts", number(contracts)}, {"prints", prints}};
  }
};

json print_json(const FlowPrint& p) {
  return {{"sequence", p.sequence}, {"time", md::format_timestamp(p.time)}, {"symbol", p.symbol},
          {"expiry", p.expiry}, {"strike", number(p.strike)},
          {"type", p.type == pricing::OptionType::Call ? "call" : "put"},
          {"price", number(p.price)}, {"size", number(p.size)}, {"premium", dollars(p.premium())},
          {"bid", number(p.bid)}, {"ask", number(p.ask)},
          {"quote_time", p.quote_time > 0 ? json(md::format_timestamp(p.quote_time)) : json(nullptr)},
          {"side", std::string(to_string(p.side))},
          {"aggressor", p.direction > 0 ? json("buy") : p.direction < 0 ? json("sell") : json(nullptr)},
          {"venue", p.venue ? json(p.venue) : json(nullptr)}, {"sweep", p.sweep}, {"block", p.block},
          {"group", p.group ? json(p.group) : json(nullptr)}, {"simulated", p.simulated}};
}
}  // namespace

ApiResponse flow_response(const MetricsSource& source, const std::string& symbol,
                          const std::map<std::string, std::string>& query) {
  const auto bad = [](const std::string& message) { return api_error(400, "INVALID_REQUEST", message); };
  const auto symbols = source.symbols();
  if (std::find(symbols.begin(), symbols.end(), symbol) == symbols.end())
    return api_error(404, "NOT_FOUND", "no data for " + symbol + " yet");
  std::size_t limit = 200;
  std::optional<pricing::OptionType> type;
  std::set<PrintSide> sides;
  std::string expiry, only;
  double min_size = 0, min_premium = 0;
  for (const auto& [key, value] : query) {
    if (key == "limit") {
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), limit);
      if (ec != std::errc{} || end != value.data() + value.size() || limit < 1 || limit > 1000)
        return bad("limit must be an integer in [1, 1000]");
    } else if (key == "type") {
      if (value != "call" && value != "put") return bad("type must be call or put");
      type = value == "call" ? pricing::OptionType::Call : pricing::OptionType::Put;
    } else if (key == "side") {
      for (std::size_t start = 0; start <= value.size();) {
        const auto comma = value.find(',', start);
        const auto side = parse_print_side(std::string_view(value).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!side) return bad("side must list bid, ask, mid, outside or unquoted");
        sides.insert(*side);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    } else if (key == "expiry") {
      expiry = value;
    } else if (key == "min_size") {
      if (!query_decimal(value, min_size) || min_size < 0) return bad("min_size must be a number of contracts, at least 0");
    } else if (key == "min_premium") {
      if (!query_decimal(value, min_premium) || min_premium < 0) return bad("min_premium must be dollars, at least 0");
    } else if (key == "only") {
      if (value != "sweep" && value != "block" && value != "multi_leg") return bad("only must be sweep, block or multi_leg");
      only = value;
    } else if (key != "window" && key != "expiries") {
      return bad("unknown query parameter " + key);
    }
  }
  const auto status = source.status();
  const bool simulated_feed = providers::simulated_provider(status.provider) && !status.capabilities.trades;
  const auto* tape = source.flow();
  const auto snapshot = tape ? tape->snapshot(symbol) : FlowTape::Snapshot{};
  std::vector<const FlowPrint*> kept;
  for (const auto& p : snapshot.prints) {
    if ((type && p.type != *type) || (!sides.empty() && !sides.contains(p.side)) || (!expiry.empty() && p.expiry != expiry) ||
        p.size < min_size || p.premium() < min_premium || (only == "sweep" && !p.sweep) || (only == "block" && !p.block) ||
        (only == "multi_leg" && !p.group)) continue;
    kept.push_back(&p);
  }
  Premium totals;
  std::map<std::string, Premium> by_expiry;
  std::map<std::pair<std::string, double>, Premium> by_strike;
  for (const auto* p : kept) {
    totals.add(*p);
    by_expiry[p->expiry].add(*p);
    by_strike[{p->expiry, p->strike}].add(*p);
  }
  json expiries = json::array(), strikes = json::array(), prints = json::array();
  for (const auto& [id, premium] : by_expiry) {
    auto row = premium.body();
    row["expiry"] = id;
    expiries.push_back(std::move(row));
  }
  for (const auto& [key, premium] : by_strike) {
    auto row = premium.body();
    row["expiry"] = key.first;
    row["strike"] = number(key.second);
    strikes.push_back(std::move(row));
  }
  for (auto it = kept.rbegin(); it != kept.rend() && prints.size() < limit; ++it) prints.push_back(print_json(**it));
  const std::string source_kind = !snapshot.prints.empty() ? (snapshot.simulated ? "simulated" : "feed")
      : status.capabilities.trades ? "feed" : simulated_feed ? "simulated" : "none";
  json message = nullptr;
  if (source_kind == "none")
    message = status.provider + " does not deliver option trades, so there is no time and sales or flow for " + symbol + ".";
  else if (source_kind == "simulated")
    message = "Simulated prints: the demo market's volume placed at its bid, ask or midpoint. They are not market trades.";
  else if (snapshot.prints.empty())
    message = "No option trades for " + symbol + " have arrived from " + status.provider + " yet.";
  md::Timestamp latest = 0;
  for (const auto& p : snapshot.prints) latest = std::max(latest, p.time);
  const auto options = tape ? tape->options() : FlowTape::Options{};
  return {200, json{{"symbol", symbol}, {"provider", status.provider}, {"trades", status.capabilities.trades},
      {"source", source_kind}, {"message", message},
      {"as_of", latest > 0 ? json(md::format_timestamp(latest)) : json(nullptr)},
      {"block_size", options.block_size}, {"capacity", options.capacity}, {"retained", snapshot.prints.size()},
      {"total", snapshot.total}, {"evicted", snapshot.evicted},
      // The queue drops trades by instrument id, before their underlying is known: feed-wide.
      {"dropped_trades", status.dropped_events}, {"matched", kept.size()},
      {"totals", totals.body()}, {"expiries", expiries}, {"strikes", strikes}, {"prints", prints},
      {"model", "Each print is classified against the option's quote when it was applied: within half a cent of the ask "
                "is a buy, of the bid a sell, above the ask a buy and below the bid a sell (outside); between them, "
                "without a two-sided quote, or when the quote is stamped after the print, it is unclassified. Net premium is bought minus sold premium. Sweeps, "
                "blocks and multi-leg groups are inferred from venue, time and size, not reported by the feed."}}.dump()};
}

}  // namespace openport::server
