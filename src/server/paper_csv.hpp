#pragma once

#include <charconv>
#include <nlohmann/json.hpp>

#include "openport/md/time.hpp"
#include "openport/providers/demo.hpp"
#include "openport/server/paper.hpp"

namespace openport::server {

inline std::optional<md::Date> csv_date(std::string_view text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-') return {};
  md::Date date;
  const auto parse = [](std::string_view part, int& value) {
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
    return error == std::errc{} && end == part.data() + part.size();
  };
  if (!parse(text.substr(0, 4), date.year) || !parse(text.substr(5, 2), date.month) || !parse(text.substr(8, 2), date.day) ||
      !md::valid_date(date) || md::format_date(date) != text) return {};
  return date;
}
inline std::string csv_quote(std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) return std::string(text);
  std::string result = "\"";
  for (const auto c : text) { result += c; if (c == '"') result += '"'; }
  return result + '"';
}
/// Fixed columns, including empty columns when an account has no trades yet.
inline std::vector<std::string> paper_csv_columns(bool fills) {
  std::vector<std::string> columns = {"account", "account_version", "provider", "prices", "new_york_date",
      "run_id", "scenario", "seed", "recording"};
  const std::vector<std::string> base = fills
      ? std::vector<std::string>{"id", "attempt", "order_id", "actor", "symbol", "underlying", "side", "quantity", "price", "fee", "quote_time", "time"}
      : std::vector<std::string>{"kind", "id", "attempt", "symbol", "underlying", "expiry", "settlement", "strike", "type", "direction",
          "status", "opened", "closed", "trading_day", "duration_seconds", "quantity", "max_quantity", "opened_contracts", "closed_contracts",
          "average_open", "average_close", "cost", "gross", "fees", "net", "return", "mark", "unrealised", "closure", "fills", "note", "tags",
          "shares", "max_shares", "opened_shares", "closed_shares", "dividends", "opened_by", "option", "closed_by", "closing_option", "strategy_id", "system_reason"};
  columns.insert(columns.end(), base.begin(), base.end());
  for (const auto& prefix : fills ? std::vector<std::string>{"context"} : std::vector<std::string>{"entry_context", "exit_context"})
    for (const auto* field : {"spot", "spot_source", "iv", "delta", "years", "equity", "floor_room", "buying_power"})
      columns.push_back(prefix + "." + field);
  if (fills) {
    for (const auto* field : {"observation", "bid", "ask", "bid_size", "ask_size", "size_left", "quoted_at", "age_seconds"})
      columns.push_back(std::string("quote.") + field);
    for (const auto* field : {"commission", "clearing", "regulatory", "index"})
      columns.push_back(std::string("fees.") + field);
  }
  if (!fills) {
    for (const auto* prefix : {"review", "strategy_review"}) {
      for (const auto* field : {"mae", "mfe", "planned_risk", "give_back", "heat", "r_multiple"})
        columns.push_back(std::string(prefix) + "." + field);
      for (const auto* extreme : {"worst", "best"})
        for (const auto* field : {"pnl", "time", "spot"}) columns.push_back(std::string(prefix) + "." + extreme + "." + field);
    }
    for (const auto* field : {"delta", "gamma", "vega", "theta", "other", "costs", "total", "fallback"})
      columns.push_back(std::string("attribution.") + field);
    for (const auto* field : {"group", "buying_power", "return_on_buying_power", "strategy_buying_power", "strategy_return_on_buying_power", "settlement_value", "settlement_source", "time_stop_orders"})
      columns.push_back(field);
  }
  return columns;
}
inline std::string csv_value(const nlohmann::json& row, std::string_view path) {
  const auto* value = &row;
  for (;;) {
    const auto dot = path.find('.');
    const std::string key(path.substr(0, dot));
    if (!value->is_object() || !value->contains(key)) return {};
    value = &value->at(key);
    if (dot == std::string_view::npos) break;
    path.remove_prefix(dot + 1);
  }
  if (value->is_null()) return {};
  if (value->is_string()) return value->get<std::string>();
  if (value->is_array()) {
    std::string joined;
    for (const auto& item : *value) {
      if (!joined.empty()) joined += ';';
      joined += item.is_string() ? item.get<std::string>() : item.dump();
    }
    return joined;
  }
  return value->dump();
}
/// `run` names the replay account on every row; its metadata columns stay empty for a live account.
inline std::string paper_csv(const nlohmann::json& rows, bool fills, const std::string& account,
                             const std::string& provider, std::uint64_t version,
                             const std::string& from, const std::string& to, const std::optional<RunIdentity>& run = {}) {
  const auto columns = paper_csv_columns(fills);
  std::string out;
  const auto line = [&](const std::vector<std::string>& cells) {
    for (std::size_t i = 0; i < cells.size(); ++i) { if (i) out += ','; out += csv_quote(cells[i]); }
    out += "\r\n";
  };
  line(columns);
  for (auto row : rows) {
    const auto instant = fills ? row.at("time") : (row.at("closed").is_null() ? row.at("opened") : row.at("closed"));
    const auto time = md::parse_datetime(instant.get<std::string>(), md::Zone::Utc);
    const auto day = time ? md::format_date(md::new_york_time(*time).date) : std::string{};
    if ((!from.empty() && day < from) || (!to.empty() && day > to)) continue;
    row["account"] = run ? run->id : account;
    row["account_version"] = std::to_string(version);
    row["provider"] = provider;
    // Exported paper P&L is always simulated, regardless of the price source.
    row["prices"] = providers::simulated_provider(provider) ? "simulated" : "price provenance unrecorded; simulated trading";
    row["new_york_date"] = day;
    if (run) {
      row["run_id"] = run->id;
      row["scenario"] = run->scenario;
      row["seed"] = run->seed;
      row["recording"] = run->recording;
    }
    std::vector<std::string> cells;
    for (const auto& column : columns) cells.push_back(csv_value(row, column));
    line(cells);
  }
  return out;
}
}  // namespace openport::server
