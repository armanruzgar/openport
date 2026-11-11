#pragma once

#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace openport::test {

/// The keys an older build wrote for each type it journaled, with the type of each
/// key's value: a name in the same table, "[T]" for an array of T, "{T}" for an
/// object holding a T under each symbol or name, or "" for a value written whole.
using JournalShapes = std::map<std::string, std::map<std::string, std::string>>;

/// What one older build wrote in each journal record: its schema number, the shapes
/// of its types ("Record" is the record's own payload), and the payload type of each
/// event it wrote.
struct LegacyJournal {
  int schema = 0;
  JournalShapes shapes;
  std::map<std::string, std::string> events;
};

/// Remove from `j`, a value of `type`, every key `shapes` does not list, at every depth.
inline void cut_to_shape(nlohmann::json& j, const std::string& type, const JournalShapes& shapes) {
  if (type.empty() || j.is_null()) return;
  if (type.front() == '[' || type.front() == '{') {
    const auto inner = type.substr(1, type.size() - 2);
    for (auto& item : j) cut_to_shape(item, inner, shapes);
    return;
  }
  if (!j.is_object()) return;
  const auto& keys = shapes.at(type);
  for (auto it = j.begin(); it != j.end();) {
    const auto kept = keys.find(it.key());
    if (kept == keys.end()) {
      it = j.erase(it);
    } else {
      cut_to_shape(it.value(), kept->second, shapes);
      ++it;
    }
  }
}

/// A record today's writer made (with its state whole, as TradingSession::expand
/// writes it), as `old` wrote it: only the keys and events that build wrote, with its
/// schema number and tick policy. Keys today's writer leaves out stay out, so this is
/// at most what that build wrote; a reader that needs a key added since then fails on
/// it as it would on that build's journals.
inline std::string as_written_by(const LegacyJournal& old, std::string_view payload) {
  auto j = nlohmann::json::parse(payload);
  auto events = nlohmann::json::array();
  for (const auto& event : j.at("events")) {
    const auto type = old.events.find(event.at("type").get<std::string>());
    if (type == old.events.end()) continue;
    nlohmann::json kept{{"type", event.at("type")}, {"payload", event.at("payload")}};
    cut_to_shape(kept["payload"], type->second, old.shapes);
    events.push_back(std::move(kept));
  }
  cut_to_shape(j, "Record", old.shapes);
  j["schema"] = old.schema;
  j["tick_policy"] = "index-v1";
  j["events"] = std::move(events);
  return j.dump();
}

/// Schema 1, as written by the last build before schema 2 (8901b6e2): the keys of
/// its src/trading/state.hpp and the events of its src/trading/session.cpp.
inline const LegacyJournal& schema_one_journal() {
  static const LegacyJournal journal{
      1,
      {
          {"Record", {{"schema", ""}, {"tick_policy", ""}, {"events", ""}, {"state", "State"},
                      {"snapshot", "TradingSnapshot"}, {"decision", "Decision"}}},
          {"Date", {{"year", ""}, {"month", ""}, {"day", ""}}},
          {"OptionContract", {{"root", ""}, {"underlying", ""}, {"expiry", "Date"}, {"strike", ""}, {"type", ""},
                              {"style", ""}, {"settlement", ""}, {"multiplier", ""}, {"standard", ""}}},
          {"Decision", {{"code", ""}, {"message", ""}, {"actual", ""}, {"limit", ""}, {"scope", ""}}},
          {"OrderRequest", {{"client_order_id", ""}, {"symbol", ""}, {"side", ""}, {"type", ""}, {"tif", ""},
                            {"quantity", ""}, {"limit_price", ""}}},
          {"Order", {{"id", ""}, {"request", "OrderRequest"}, {"status", ""}, {"filled_quantity", ""},
                     {"filled_notional", ""}, {"accepted_at", ""}, {"day_end", ""}, {"reason", "Decision"}}},
          {"Fill", {{"id", ""}, {"order_id", ""}, {"symbol", ""}, {"side", ""}, {"quantity", ""}, {"price", ""},
                    {"fee", ""}, {"observation", ""}, {"quote_time", ""}, {"time", ""}}},
          {"QuoteObservation", {{"symbol", ""}, {"observation", ""}, {"time", ""}, {"bid", ""}, {"ask", ""},
                                {"bid_size", ""}, {"ask_size", ""}}},
          {"Valuation", {{"symbol", ""}, {"time", ""}, {"delta", ""}, {"gamma", ""}, {"vega", ""}, {"theta", ""},
                         {"spot", ""}, {"forward", ""}, {"discount", ""}, {"years", ""}, {"smile_iv", ""}, {"valid", ""}}},
          {"Exposure", {{"dollar_delta", ""}, {"dollar_gamma_1pct", ""}, {"vega", ""}, {"theta", ""}}},
          {"ExposureLimits", {{"dollar_delta", ""}, {"vega", ""}}},
          {"Limits", {{"max_order_contracts", ""}, {"price_band_absolute", ""}, {"price_band_relative", ""},
                      {"aggregate", "ExposureLimits"}, {"per_underlying", "ExposureLimits"},
                      {"underlying_overrides", "{ExposureLimits}"}, {"max_daily_loss", ""}, {"max_quote_age", ""},
                      {"max_valuation_age", ""}}},
          {"ScenarioConfig", {{"spot_percent", ""}, {"vol_points", ""}, {"vol_floor", ""}}},
          {"SessionConfig", {{"initial_cash", ""}, {"fee_per_contract", ""}, {"limits", "Limits"},
                             {"scenarios", "ScenarioConfig"}}},
          {"Position", {{"contract", "OptionContract"}, {"quantity", ""}, {"basis", ""}, {"realised", ""}, {"fees", ""}}},
          {"Account", {{"cash", ""}, {"realised", ""}, {"fees", ""}}},
          {"Ledger", {{"account", "Account"}, {"positions", "{Position}"}}},
          {"ExposureRange", {{"delta_low", ""}, {"delta_high", ""}, {"vega_low", ""}, {"vega_high", ""}}},
          {"RiskBucket", {{"position", "Exposure"}, {"reachable", "ExposureRange"}, {"limits", "ExposureLimits"},
                          {"delta_utilisation", ""}, {"vega_utilisation", ""}}},
          {"RiskSnapshot", {{"aggregate", "RiskBucket"}, {"underlyings", "{RiskBucket}"}, {"complete", ""},
                            {"daily_loss", ""}, {"kill_latched", ""}, {"kill_reason", ""}, {"limits_revision", ""}}},
          {"ScenarioCell", {{"spot_percent", ""}, {"vol_points", ""}, {"pnl", ""}, {"clamped", ""}}},
          {"ScenarioGrid", {{"cells", "[ScenarioCell]"}, {"complete", ""}}},
          {"MarkedPosition", {{"position", "Position"}, {"mark", ""}, {"mark_time", ""}, {"mark_age", ""},
                              {"market_value", ""}, {"unrealised", ""}, {"fresh", ""}, {"awaiting_settlement", ""}}},
          {"TradingSnapshot", {{"account_version", ""}, {"time", ""}, {"account", "Account"}, {"equity", ""},
                               {"start_of_day_equity", ""}, {"unrealised", ""}, {"valuation_complete", ""},
                               {"journal_failed", ""}, {"positions", "[MarkedPosition]"}, {"open_orders", "[Order]"},
                               {"recent_orders", "[Order]"}, {"recent_fills", "[Fill]"}, {"risk", "RiskSnapshot"},
                               {"scenarios", "ScenarioGrid"}, {"quality_flags", ""}}},
          {"Book", {{"quote", "QuoteObservation"}, {"bid_left", ""}, {"ask_left", ""}}},
          {"Mark", {{"price", ""}, {"time", ""}}},
          {"State", {{"config", "SessionConfig"}, {"time", ""}, {"version", ""}, {"limits_revision", ""},
                     {"ledger", "Ledger"}, {"start_equity", ""}, {"day", "Date"}, {"contracts", "{OptionContract}"},
                     {"books", "{Book}"}, {"marks", "{Mark}"}, {"valuations", "{Valuation}"}, {"orders", "[Order]"},
                     {"fills", "[Fill]"}, {"settled", ""}, {"kill", ""}, {"kill_reason", ""}}},
          {"KillEvent", {{"reason", ""}}},
          {"SettlementEvent", {{"symbol", ""}, {"reference", ""}, {"intrinsic", ""}}},
          {"DayRolloverEvent", {{"day", "Date"}, {"equity", ""}}},
      },
      {
          {"session_start", "SessionConfig"}, {"definition", "OptionContract"}, {"order_accepted", "Order"},
          {"order_rejected", "Order"}, {"cancel", "Order"}, {"fill", "Fill"}, {"limit_change", "Limits"},
          {"kill_trip", "KillEvent"}, {"kill_reset", "KillEvent"}, {"settlement", "SettlementEvent"},
          {"day_rollover", "DayRolloverEvent"},
      },
  };
  return journal;
}

/// Schema 2 as its first build wrote it (9c0f7fc3), before funded accounts and
/// multi-leg orders: schema 1 with plan rules, system orders, the evaluation, its
/// attempts and closures, and buying power.
inline const LegacyJournal& first_schema_two_journal() {
  static const LegacyJournal journal = [] {
    auto old = schema_one_journal();
    old.schema = 2;
    auto& shapes = old.shapes;
    shapes["AccountRules"] = {{"plan", ""}, {"profit_target", ""}, {"max_drawdown", ""}, {"drawdown_mode", ""},
                              {"buy_only", ""}, {"buying_power", ""}, {"expiry_cutoff", ""}};
    shapes["SessionConfig"]["rules"] = "AccountRules";
    shapes["Order"]["system"] = "";
    shapes["EvaluationDay"] = {{"day", "Date"}, {"open_equity", ""}, {"close_equity", ""}, {"peak", ""}, {"floor", ""}};
    shapes["Evaluation"] = {{"attempt", ""}, {"started", ""}, {"starting_balance", ""}, {"peak", ""}, {"floor", ""},
                            {"status", ""}, {"decided_at", ""}, {"decided_equity", ""}, {"decision", ""},
                            {"first_order", ""}, {"first_fill", ""}, {"day", "Date"}, {"day_open_equity", ""},
                            {"day_close_equity", ""}, {"days", "[EvaluationDay]"}};
    shapes["AttemptSummary"] = {{"attempt", ""}, {"plan", ""}, {"started", ""}, {"ended", ""}, {"starting_balance", ""},
                                {"final_equity", ""}, {"status", ""}, {"decision", ""}, {"first_order", ""},
                                {"first_fill", ""}};
    shapes["Closure"] = {{"symbol", ""}, {"quantity", ""}, {"price", ""}, {"time", ""}, {"kind", ""}, {"after_fill", ""}};
    shapes["BuyingPower"] = {{"available", ""}, {"reserved", ""}, {"short_requirement", ""}};
    for (const auto* type : {"State", "TradingSnapshot"}) {
      shapes[type]["evaluation"] = "Evaluation";
      shapes[type]["attempts"] = "[AttemptSummary]";
      shapes[type]["closures"] = "[Closure]";
    }
    shapes["TradingSnapshot"]["buying_power"] = "BuyingPower";
    shapes["AccountResetEvent"] = {{"reason", ""}, {"attempt", ""}, {"initial_cash", ""}, {"rules", "AccountRules"}};
    old.events["account_reset"] = "AccountResetEvent";
    old.events["evaluation_day"] = "EvaluationDay";
    return old;
  }();
  return journal;
}

}  // namespace openport::test
