#pragma once

#include "../trading/state.hpp"
#include "openport/server/desk.hpp"

namespace openport::trading {
inline void to_json(nlohmann::json& j, const OrderChange& c) {
  j = nlohmann::json{{"quantity", c.quantity}, {"limit_price", c.limit_price}, {"trigger_level", c.trigger_level}};
  // Only a change of time in force records it, so other changes keep their bytes.
  if (c.tif) j["tif"] = *c.tif;
  if (c.walk) j["walk"] = *c.walk;
}
inline void from_json(const nlohmann::json& j, OrderChange& c) {
  j.at("quantity").get_to(c.quantity); j.at("limit_price").get_to(c.limit_price); j.at("trigger_level").get_to(c.trigger_level);
  added_field(j, "tif", c.tif);
  if (j.contains("walk")) c.walk.emplace(j.at("walk").get<std::optional<Walk>>());
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Dividend, symbol, ex_date, per_share)
}
namespace openport::analytics {
inline void to_json(nlohmann::json& j, const AnalyticsOptions& o) {
  j = nlohmann::json{{"parity_strikes", o.parity_strikes}, {"flip_range", o.flip_range}, {"flip_steps", o.flip_steps},
      {"fallback_rate", o.fallback_rate}, {"deamericanize", o.deamericanize}, {"min_days_for_rate", o.min_days_for_rate},
      {"exposure_min_days", o.exposure_min_days}, {"max_spot_age_minutes", o.max_spot_age_minutes}};
  // Recorded only when chosen, so runs on the default tree keep their bytes.
  if (o.american_model != pricing::AmericanModel::LeisenReimer) j["american_model"] = pricing::to_string(o.american_model);
}
inline void from_json(const nlohmann::json& j, AnalyticsOptions& o) {
  j.at("parity_strikes").get_to(o.parity_strikes); j.at("flip_range").get_to(o.flip_range);
  j.at("flip_steps").get_to(o.flip_steps); j.at("fallback_rate").get_to(o.fallback_rate);
  j.at("deamericanize").get_to(o.deamericanize); j.at("min_days_for_rate").get_to(o.min_days_for_rate);
  j.at("exposure_min_days").get_to(o.exposure_min_days); j.at("max_spot_age_minutes").get_to(o.max_spot_age_minutes);
  if (const auto it = j.find("american_model"); it != j.end()) {
    const auto model = pricing::parse_american_model(it->get<std::string>());
    if (!model) throw std::invalid_argument("Unknown american_model " + it->get<std::string>());
    o.american_model = *model;
  }
}
}
namespace openport::server {
inline void to_json(nlohmann::json& j, const TradingCommand& c) {
  j = nlohmann::json{{"kind", c.kind}, {"order", c.order}, {"order_id", c.order_id}, {"limits", c.limits},
      {"guardrails", c.guardrails}, {"floor_share", c.floor_share}, {"expected_revision", c.expected_revision},
      {"reason", c.reason}, {"symbol", c.symbol}, {"settlement", c.settlement}, {"initial_cash", c.initial_cash},
      {"rules", c.rules}, {"required_pass", c.required_pass}, {"amount", c.amount}, {"change", c.change},
      {"underlying", c.underlying}, {"account", c.account}, {"name", c.name}, {"trade", c.trade}, {"shares", c.shares},
      {"day", c.day}, {"plan", c.plan}, {"review", c.review}, {"note", c.note}, {"tags", c.tags},
      {"quantity", c.quantity}, {"actor", c.actor}};
  if (c.kind == TradingCommand::Kind::CreateAlert) j["alert"] = c.alert;
  if (c.kind == TradingCommand::Kind::DeleteAlert) j["alert_id"] = c.alert_id;
  if (c.archived) j["archived"] = *c.archived;
  if (!c.copy_settings_from.empty()) j["copy_settings_from"] = c.copy_settings_from;
  // Later fields are recorded only when set, so earlier commands keep their bytes.
  if (c.cancel_reason != trading::Reason::USER_CANCEL) j["cancel_reason"] = c.cancel_reason;
  if (c.stock_price) j["stock_price"] = {{"symbol", c.stock_price->symbol}, {"time", c.stock_price->time}, {"price", c.stock_price->price}};
  if (c.do_not_exercise) j["do_not_exercise"] = true;
  if (c.restore_scaled_rules) j["restore_scaled_rules"] = true;
  if (c.program_costs) j["program_costs"] = true;
  if (c.program_cost_overrides != 0) j["program_cost_overrides"] = c.program_cost_overrides;
  if (!c.trades.empty()) j["trades"] = c.trades;
  if (c.close_pricing.limit) { j["close_limit"] = true; j["limit_ticks"] = c.close_pricing.limit_ticks; }
  if (!c.order_ids.empty()) j["order_ids"] = c.order_ids;
}
inline void from_json(const nlohmann::json& j, TradingCommand& c) {
  j.at("kind").get_to(c.kind);
  j.at("order").get_to(c.order);
  j.at("order_id").get_to(c.order_id);
  j.at("limits").get_to(c.limits);
  j.at("guardrails").get_to(c.guardrails);
  j.at("floor_share").get_to(c.floor_share);
  j.at("expected_revision").get_to(c.expected_revision);
  j.at("reason").get_to(c.reason);
  j.at("symbol").get_to(c.symbol);
  j.at("settlement").get_to(c.settlement);
  j.at("initial_cash").get_to(c.initial_cash);
  j.at("rules").get_to(c.rules);
  j.at("required_pass").get_to(c.required_pass);
  trading::added_field(j, "restore_scaled_rules", c.restore_scaled_rules);
  trading::added_field(j, "program_costs", c.program_costs);
  trading::added_field(j, "program_cost_overrides", c.program_cost_overrides);
  j.at("amount").get_to(c.amount);
  j.at("change").get_to(c.change);
  j.at("underlying").get_to(c.underlying);
  j.at("account").get_to(c.account);
  j.at("name").get_to(c.name);
  j.at("trade").get_to(c.trade);
  j.at("shares").get_to(c.shares);
  j.at("day").get_to(c.day);
  j.at("plan").get_to(c.plan);
  j.at("review").get_to(c.review);
  j.at("note").get_to(c.note);
  j.at("tags").get_to(c.tags);
  j.at("quantity").get_to(c.quantity);
  if (j.contains("stock_price")) {
    const auto& p = j.at("stock_price");
    c.stock_price = trading::StockPrice{p.at("symbol").get<std::string>(), p.at("time").get<md::Timestamp>(), p.at("price").get<trading::Money>()};
  }
  trading::added_field(j, "cancel_reason", c.cancel_reason);
  trading::added_field(j, "actor", c.actor);
  trading::added_field(j, "archived", c.archived);
  trading::added_field(j, "copy_settings_from", c.copy_settings_from);
  trading::added_field(j, "alert", c.alert);
  trading::added_field(j, "alert_id", c.alert_id);
  trading::added_field(j, "do_not_exercise", c.do_not_exercise);
  trading::added_field(j, "trades", c.trades);
  trading::added_field(j, "close_limit", c.close_pricing.limit);
  trading::added_field(j, "limit_ticks", c.close_pricing.limit_ticks);
  trading::added_field(j, "order_ids", c.order_ids);
}
}
