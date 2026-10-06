#include "paper_json.hpp"
#include "event_calendar.hpp"
#include "openport/server/sandboxes.hpp"
#include "playbook_api.hpp"
#include "paper_csv.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "openport/server/candles.hpp"
#include "openport/server/plans.hpp"
#include "openport/trading/contracts.hpp"
#include "openport/trading/history.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
using namespace trading;

json number(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
constexpr std::size_t kMaxWhatIfCandidates = 6;
constexpr std::size_t kMaxWhatIfOrders = 4;
json nullable(const std::string& value) { return value.empty() ? json(nullptr) : json(value); }
json money(const std::optional<Money>& value) { return value ? json(value->str()) : json(nullptr); }
json positive(Money value) { return value > Money{} ? json(value.str()) : json(nullptr); }
const char* status_name(EvaluationStatus status) {
  constexpr const char* names[] = {"active", "passed", "failed"};
  return names[static_cast<int>(status)];
}
json payout_rules_json(const PayoutRules& p) {
  json caps = json::array();
  for (const auto cap : p.caps) caps.push_back(cap.str());
  return {{"qualifying_profit", p.qualifying_profit.str()}, {"qualifying_days", p.qualifying_days},
          {"withdrawal_percent", p.withdrawal_percent}, {"split_percent", p.split_percent},
          {"minimum", p.minimum.str()}, {"caps", caps}, {"consistency_percents", p.consistency_percents},
          {"buffer", p.buffer.str()}, {"buffer_payouts", p.buffer_payouts}};
}
constexpr const char* kDrawdownModes[] = {"intraday", "end_of_day", "static"};
constexpr const char* kProfitBases[] = {"equity", "balance"};
constexpr const char* kDailyLossBases[] = {"equity", "balance", "higher", "peak"};
constexpr const char* kBreachActions[] = {"lock", "fail"};
constexpr const char* kConsistencyBases[] = {"total", "positive_days"};
/// "HH:MM" New York time for minutes after midnight; 1440 is "24:00".
std::string clock_text(std::int64_t minutes) {
  char text[8];
  std::snprintf(text, sizeof text, "%02d:%02d", static_cast<int>(minutes / 60), static_cast<int>(minutes % 60));
  return text;
}
json fee_schedule_json(const FeeSchedule& f) {
  json index = json::object();
  for (const auto& [root, fee] : f.index) index[root] = fee.str();
  return {{"open", f.open.str()}, {"close", f.close.str()}, {"leg_cap", f.leg_cap.str()}, {"clearing", f.clearing.str()},
          {"regulatory", f.regulatory.str()}, {"index", index}, {"exercise", f.exercise.str()}};
}
json fill_fees_json(const std::optional<FillFees>& f) {
  if (!f) return nullptr;
  return {{"commission", f->commission.str()}, {"clearing", f->clearing.str()}, {"regulatory", f->regulatory.str()},
          {"index", f->index.str()}};
}
json rules_json(const AccountRules& r, Money initial_cash, const std::optional<SizeScalingProgress>& scaling = {}) {
  const bool funded = r.phase == Phase::Funded;
  const auto id = preset_id(scaling ? scaling->original : initial_cash, original_program_rules(r, initial_cash, scaling));
  json result = {{"plan", nullable(r.plan)}, {"plan_id", nullable(id)}, {"phase", funded ? "funded" : r.phase == Phase::Verification ? "verification" : "evaluation"},
          {"profit_target", positive(r.profit_target)}, {"max_drawdown", positive(r.max_drawdown)},
          {"drawdown_mode", kDrawdownModes[static_cast<int>(r.drawdown_mode)]},
          {"lock_balance", positive(r.lock_balance)}, {"lock_at_start", r.lock_at_start},
          {"profit_basis", kProfitBases[static_cast<int>(r.profit_basis)]},
          {"daily_loss_limit", positive(r.daily_loss_limit)},
          {"daily_loss_basis", kDailyLossBases[static_cast<int>(r.daily_loss_basis)]},
          {"daily_loss_action", kBreachActions[static_cast<int>(r.daily_loss_action)]},
          {"min_hold_seconds", r.min_hold_seconds}, {"microscalp_seconds", r.microscalp_seconds},
          {"microscalp_percent", r.microscalp_percent}, {"min_trades", r.min_trades}, {"trade_consistency_percent", r.trade_consistency_percent}, {"consistency_percent", r.consistency_percent},
          {"consistency_basis", kConsistencyBases[static_cast<int>(r.consistency_basis)]},
          {"min_trading_days", r.min_trading_days}, {"min_profitable_days", r.min_profitable_days},
          {"profitable_day_profit", positive(r.profitable_day_profit)}, {"day_end", clock_text(r.day_end_minutes)},
          {"time_limit_days", r.time_limit_days}, {"inactivity_days", r.inactivity_days}, {"underlyings", r.underlyings},
          {"trading_start", r.trading_start ? json(clock_text(*r.trading_start)) : json(nullptr)},
          {"trading_end", r.trading_end ? json(clock_text(*r.trading_end)) : json(nullptr)},
          {"flat_time", r.flat_time ? json(clock_text(*r.flat_time)) : json(nullptr)}, {"no_overnight", r.no_overnight},
          {"buy_only", r.buy_only}, {"defined_risk", r.defined_risk}, {"buying_power", r.buying_power},
          {"slippage_ticks", r.slippage_ticks}, {"margin", r.margin == MarginMode::Portfolio ? "portfolio" : "strategy"},
          {"account_type", r.account_type == AccountType::Cash ? "cash" : r.account_type == AccountType::Ira ? "ira" : "margin"},
          {"house_margin_percent", r.house_margin_percent}, {"pm_vol_shock", r.pm_vol_shock},
          {"max_contracts_held", r.max_contracts_held},
          {"max_volume_percent", r.max_volume_percent}, {"no_hedging", r.no_hedging}, {"no_counter_positions", r.no_counter_positions},
          {"require_stop_loss", r.require_stop_loss}, {"max_trade_risk", positive(r.max_trade_risk)},
          {"max_trade_risk_percent", r.max_trade_risk_percent},
          {"evaluation_fee", r.evaluation_fee.str()}, {"reset_fee", r.reset_fee.str()},
          {"activation_fee", r.activation_fee.str()}, {"max_resets", r.max_resets},
          {"expiry_cutoff_seconds", r.expiry_cutoff / md::kNanosPerSecond},
          {"payouts", funded ? payout_rules_json(r.payouts) : json(nullptr)}};
  result["size_scaling"] = nullptr;
  if (r.size_scaling) {
    const auto& v = *r.size_scaling;
    result["size_scaling"] = {{"profit_percent", v.profit_percent}, {"payouts", v.payouts}, {"days", v.days},
        {"increase_percent", v.increase_percent}, {"max_balance", v.max_balance.str()}};
  }
  result["scaling"] = json::array();
  for (const auto& step : r.scaling)
    result["scaling"].push_back({{"profit", step.profit.str()}, {"contracts", step.contracts}});
  if (r.fill_latency_ms != 0) result["fill_latency_ms"] = r.fill_latency_ms;
  if (r.impact_ticks != 0) result["impact_ticks"] = r.impact_ticks;
  if (r.inside_fill_percent != 0) result["inside_fill_percent"] = r.inside_fill_percent;
  if (r.fees) result["fees"] = fee_schedule_json(*r.fees);
  result["events"] = calendar_events_json(r.events);
  result["news_before_minutes"] = r.news_before_minutes; result["news_after_minutes"] = r.news_after_minutes;
  result["news_action"] = r.news_action; result["hold_restrictions"] = r.hold_restrictions;
  result["hold_cutoff"] = clock_text(r.hold_cutoff);
  return result;
}
/// A check one contract failed reports that contract's underlying as its scope, as
/// HTTP errors do; the journaled decision keeps the OSI, as the order itself does.
json scope_json(const std::string& scope) {
  const auto contract = md::parse_osi(scope);
  return nullable(contract ? contract->underlying : scope);
}
json evidence_value(const Decision& d, bool limit) {
  const auto value = limit ? d.limit : d.actual;
  if (!value) return nullptr;
  if (d.code == Reason::NEWS_BLACKOUT) {
    const auto maximum = std::numeric_limits<Timestamp>::max();
    return md::format_timestamp(*value >= static_cast<double>(maximum) ? maximum : static_cast<Timestamp>(*value));
  }
  if (d.code == Reason::FLAT_TIME || d.code == Reason::OUTSIDE_PLAN_HOURS || d.code == Reason::HOLD_RESTRICTED)
    return clock_text(static_cast<std::int64_t>(*value));
  return number(*value);
}
/// A reason with its numeric evidence: how far a check was exceeded, and where.
json rule_evidence_json(const RuleEvidence& e) {
  if (!e.contract.empty())
    return {{"contract", e.contract}, {"contracts", e.contracts},
            {"volume", e.volume ? json(*e.volume) : json(nullptr)}, {"percent", e.percent}};
  json result{{"underlying", e.underlying}, {"order_dollar_delta", e.order_dollar_delta}, {"held_dollar_delta", e.held_dollar_delta}};
  if (!e.other_account.empty()) result["other_account"] = e.other_account;
  return result;
}
json decision_json(const Decision& d) {
  if (d.ok()) return nullptr;
  json result{{"code", to_string(d.code)}, {"message", d.message},
          {"actual", evidence_value(d, false)}, {"limit", evidence_value(d, true)},
          {"scope", scope_json(d.scope)}};
  if (d.evidence) result["evidence"] = rule_evidence_json(*d.evidence);
  return result;
}
json time_or_null(Timestamp time) { return time > 0 ? json(md::format_timestamp(time)) : json(nullptr); }
json guardrails_json(const Guardrails& g) {
  return {{"soft_floor", g.soft_floor.str()}, {"soft_floor_percent", g.soft_floor_percent},
          {"max_opening_trades", g.max_opening_trades}, {"cooldown_loss", g.cooldown_loss.str()},
          {"cooldown_minutes", g.cooldown_minutes}, {"profit_lock", g.profit_lock.str()}};
}
json guardrail_state_json(const TradingSnapshot& s) {
  json latched = json::array();
  for (auto reason : s.guardrails.latched) latched.push_back(to_string(reason));
  if (s.time < s.guardrails.cooldown_until) latched.push_back("COOLDOWN");
  return {{"opening_trades", s.guardrails.opening_trades}, {"latched", latched},
          {"cooldown_until", time_or_null(s.guardrails.cooldown_until)},
          {"cooldown_seconds", std::max<Timestamp>(0, s.guardrails.cooldown_until - s.time) / md::kNanosPerSecond},
          {"soft_floor", money(s.soft_floor)}};
}
json breach_json(const BreachRisk& b) {
  const auto level = [](const std::optional<BreachLevel>& value) -> json {
    if (!value) return nullptr;
    return {{"points", number(value->points)}, {"percent", number(value->percent)},
            {"touch_probability", value->touch_probability ? number(*value->touch_probability) : json(nullptr)}};
  };
  json underlyings = json::array();
  for (const auto& item : b.underlyings)
    underlyings.push_back({{"underlying", item.underlying}, {"spot", number(item.spot)}, {"complete", item.complete},
        {"close_sigma", item.close_sigma ? number(*item.close_sigma) : json(nullptr)}, {"down", level(item.down)}, {"up", level(item.up)},
        {"soft_down", level(item.soft_down)}, {"soft_up", level(item.soft_up)}});
  return {{"room", money(b.room)}, {"soft_room", money(b.soft_room)}, {"complete", b.complete},
          {"underlyings", underlyings}, {"model", "Driftless log-return reflection estimate; unchanged volatility, one underlying at a time"},
          {"scan_down_percent", -99.75}, {"scan_up_percent", 1000}};
}
json units(const std::optional<Quantity>& value) { return value ? json(*value) : json(nullptr); }
json warnings_json(const std::vector<RiskWarning>& warnings) {
  json out = json::array();
  for (const auto& w : warnings)
    out.push_back({{"code", w.code}, {"severity", w.severity}, {"scope", w.scope},
                   {"symbol", w.symbol.empty() ? json(nullptr) : json(w.symbol)}, {"message", w.message},
                   {"actual", w.actual ? number(*w.actual) : json(nullptr)}, {"limit", w.limit ? number(*w.limit) : json(nullptr)}});
  return out;
}
json preview_fills(const std::vector<PreviewFill>& fills) {
  json out = json::array();
  for (const auto& f : fills)
    out.push_back({{"symbol", f.symbol}, {"side", f.side == Side::Buy ? "buy" : "sell"}, {"quantity", f.quantity}, {"price", f.price.str()}});
  return out;
}
json execution_json(const PreviewExecution& e) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected", "armed"};
  return {{"status", statuses[static_cast<int>(e.status)]}, {"filled_quantity", e.filled_quantity},
          {"remaining_quantity", e.remaining_quantity},
          {"reason", e.reason.ok() ? json(nullptr) : json{{"code", to_string(e.reason.code)}, {"message", e.reason.message}}},
          {"fills", preview_fills(e.fills)}, {"average_fill_price", money(e.average_fill_price)},
          {"schedule", preview_fills(e.schedule)}, {"average_price", money(e.average_price)}};
}
json exposure_json(const Exposure& e) {
  return {{"dollar_delta", number(e.dollar_delta)}, {"dollar_gamma_1pct", number(e.dollar_gamma_1pct)},
          {"vega", number(e.vega)}, {"theta", number(e.theta)}};
}
json what_if_account_json(const WhatIfAccount& a, const ScenarioConfig& grid) {
  if (!a.projected) return nullptr;
  json pnl = json::array();
  std::size_t index = 0;
  for ([[maybe_unused]] double spot : grid.spot_percent) {
    json row = json::array();
    for ([[maybe_unused]] double vol : grid.vol_points) {
      const auto* cell = index < a.scenarios.cells.size() ? &a.scenarios.cells[index] : nullptr;
      ++index;
      row.push_back(cell && a.scenarios.complete ? number(cell->pnl) : json(nullptr));
    }
    pnl.push_back(std::move(row));
  }
  return {{"equity", a.equity.str()}, {"buying_power", a.buying_power.str()},
          {"exposure", a.exposure ? exposure_json(*a.exposure) : json(nullptr)},
          {"max_loss", money(a.max_loss)}, {"equity_at_max_loss", money(a.equity_at_max_loss)},
          {"breaches_floor", a.breaches_floor ? json(*a.breaches_floor) : json(nullptr)},
          {"breaches_soft_floor", a.breaches_soft_floor ? json(*a.breaches_soft_floor) : json(nullptr)},
          {"scenarios", {{"spot_percent", grid.spot_percent}, {"vol_points", grid.vol_points}, {"pnl", pnl},
                         {"complete", a.scenarios.complete}}},
          {"breach", breach_json(a.breach)}};
}
json what_if_json(const WhatIf& w, const std::vector<std::string>& names, const ScenarioConfig& grid) {
  json candidates = json::array();
  for (std::size_t i = 0; i < w.candidates.size(); ++i) {
    const auto& c = w.candidates[i];
    json orders = json::array();
    for (const auto& d : c.orders) orders.push_back({{"decision", d.ok() ? "ok" : to_string(d.code)}, {"reason", decision_json(d)}});
    candidates.push_back({{"name", i < names.size() ? names[i] : std::string()}, {"decision", c.decision.ok() ? "ok" : to_string(c.decision.code)},
                          {"reason", decision_json(c.decision)}, {"orders", orders}, {"after", what_if_account_json(c.after, grid)}});
  }
  return {{"current", what_if_account_json(w.current, grid)}, {"candidates", candidates}, {"simulated", true}};
}
/// Each leg's quote in a preview: whether it can fill now, and the displayed size
/// on its side with what this account's orders have left of it.
json leg_liquidity_json(const OrderPreview& p) {
  json legs = json::array();
  for (const auto& leg : p.liquidity)
    legs.push_back({{"symbol", leg.symbol}, {"side", leg.side == Side::Buy ? "buy" : "sell"}, {"contracts", leg.contracts},
                    {"executable", leg.quote.ok()}, {"reason", decision_json(leg.quote)},
                    {"displayed", leg.displayed}, {"size_left", leg.left}});
  return legs;
}
json walk_step_json(const std::optional<WalkStep>& step) {
  return step ? json{{"time", md::format_timestamp(step->time)}, {"limit_price", step->price.str()}} : json(nullptr);
}
json walk_json(const std::optional<Walk>& walk) {
  return walk ? json{{"step", walk->step.str()}, {"seconds", walk->seconds}, {"limit", walk->limit.str()}} : json(nullptr);
}
json preview_json(const OrderPreview& p) {
  json warnings = json::array();
  for (const auto& warning : p.warnings) warnings.push_back({{"code", warning.code}, {"message", warning.message}});
  json change = nullptr;
  if (p.exposure_change) change = {{"dollar_delta", number(p.exposure_change->dollar_delta)},
      {"dollar_gamma_1pct", number(p.exposure_change->dollar_gamma_1pct)},
      {"vega", number(p.exposure_change->vega)}, {"theta", number(p.exposure_change->theta)}};
  return {{"decision", p.decision.ok() ? "ok" : to_string(p.decision.code)}, {"reason", decision_json(p.decision)},
      {"buying_power", {{"required", p.buying_power_required.str()}, {"before", p.buying_power_before.str()},
                        {"working", money(p.buying_power_working)}, {"after", money(p.buying_power_after)}}},
      {"exposure_change", change}, {"max_loss", money(p.max_loss)}, {"max_loss_basis", nullable(p.max_loss_basis)},
      {"trade_risk", money(p.trade_risk)}, {"trade_risk_limit", money(p.trade_risk_limit)},
      {"trade_risk_basis", nullable(p.trade_risk_basis)},
      {"equity_at_max_loss", money(p.equity_at_max_loss)},
      {"breaches_floor", p.breaches_floor ? json(*p.breaches_floor) : json(nullptr)},
      {"breaches_soft_floor", p.breaches_soft_floor ? json(*p.breaches_soft_floor) : json(nullptr)},
      {"max_units", units(p.max_units)}, {"max_units_basis", nullable(p.max_units_basis)}, {"max_units_buying_power", units(p.max_units_buying_power)},
      {"max_units_floor", units(p.max_units_floor)}, {"breach", breach_json(p.breach)},
      {"fee", money(p.fee)}, {"fees", fill_fees_json(p.fees)},
      {"next_walk", walk_step_json(p.next_walk)},
      {"execution", execution_json(p.execution)}, {"liquidity", leg_liquidity_json(p)}, {"warnings", warnings}, {"simulated", true}};
}

/// The next payout's requirements, or null outside the funded phase.
json payout_json(const TradingView& view) {
  const auto& r = view.config.rules;
  const auto q = payout_quote(*view.snapshot, r);
  if (!q.funded) return nullptr;
  return {{"eligible", q.blocked.ok()}, {"blocked", decision_json(q.blocked)}, {"number", q.number},
          {"active", q.active}, {"flat", q.flat}, {"qualifying_days", q.qualifying_days},
          {"required_days", q.required_days}, {"qualifying_profit", r.payouts.qualifying_profit.str()},
          {"profit", q.profit.str()},
          {"consistency_percent", q.consistency_percent ? json(*q.consistency_percent) : json(nullptr)},
          {"cycle_profit", q.cycle_profit.str()}, {"consistency_needed", q.consistency_needed.str()},
          {"buffer_balance", money(q.buffer_balance)},
          {"best_day", q.best_day ? json{{"day", md::format_date(q.best_day_date)}, {"profit", q.best_day->str()}} : json(nullptr)},
          {"withdrawable", q.withdrawable.str()}, {"cap", money(q.cap)}, {"maximum", q.maximum.str()},
          {"minimum", q.minimum.str()}, {"trader_share", q.trader_share.str()},
          {"withdrawal_percent", r.payouts.withdrawal_percent}, {"split_percent", r.payouts.split_percent}};
}
json buying_power_json(const BuyingPower& power) {
  // short_requirement keeps its historical name; requirement is the same amount
  // under a name that also fits portfolio margin, where it covers the whole book.
  return {{"available", power.available.str()}, {"reserved", power.reserved.str()},
          {"short_requirement", power.short_requirement.str()}, {"requirement", power.short_requirement.str()}};
}
/// Per-unit average of a notional over whole contracts, nearest micro-dollar.
json average(Money notional, Quantity contracts) {
  return contracts > 0 ? json(notional.prorate(1, contracts).str()) : json(nullptr);
}
std::string underlying(const TradingView& view, const std::string& symbol) {
  const auto it = view.contracts.find(symbol);
  if (it != view.contracts.end()) return it->second.underlying;
  const auto parsed = md::parse_osi(symbol);
  return parsed ? parsed->underlying : "";
}
json trigger_json(const std::optional<Trigger>& t) {
  if (!t) return nullptr;
  constexpr const char* sources[] = {"option", "underlying", "combo", "study", "time"};
  json result{{"source", sources[static_cast<int>(t->source)]},
              {"direction", t->direction == TriggerDirection::AtOrBelow ? "at_or_below" : "at_or_above"},
              {"level", t->level.str()}};
  // Conditional terms appear only on the triggers that have them.
  if (!t->symbol.empty()) result["symbol"] = t->symbol;
  if (t->source == TriggerSource::Study) result["study"] = t->study;
  // Triggers on the order's own market report what they read and how they trail.
  if (!conditional(*t)) {
    result["reference"] = t->reference == TriggerReference::Mid ? "mid" : t->reference == TriggerReference::Mark ? "mark" : "bid_ask";
    result["trail"] = nullptr;
    if (t->trail) {
      const auto unit = t->trail->unit;
      result["trail"] = {{"unit", unit == TrailUnit::Amount ? "amount" : unit == TrailUnit::Percent ? "percent" : "ticks"},
                         {"value", unit == TrailUnit::Ticks ? json(t->trail->value.micros() / 1'000'000) : json(t->trail->value.str())}};
    }
  }
  if (t->source == TriggerSource::Time) {
    const auto two = [](std::int64_t n) { return std::string{static_cast<char>('0' + n / 10), static_cast<char>('0' + n % 10)}; };
    result["at"] = two(t->minute / 60 % 24) + ":" + two(t->minute % 60);
  }
  return result;
}
json exit_json(const std::optional<ExitSpec>& e) {
  if (!e) return nullptr;
  return {{"trigger", trigger_json(e->trigger)}, {"limit_price", money(e->limit_price)}};
}
json id_or_null(OrderId id) { return id == 0 ? json(nullptr) : json(std::to_string(id)); }
json alert_json(const Alert& a) {
  constexpr const char* scopes[] = {"contract", "spread", "underlying", "account"};
  const auto& c = a.spec.condition;
  json legs = nullptr;
  if (c.scope == AlertScope::Spread) {
    legs = json::array();
    for (const auto& leg : c.legs) legs.push_back({{"symbol", leg.symbol}, {"side", leg.side == Side::Buy ? "buy" : "sell"}, {"ratio", leg.ratio}});
  }
  return {{"id", std::to_string(a.id)}, {"label", a.spec.label}, {"scope", scopes[static_cast<int>(c.scope)]},
          {"metric", c.metric}, {"symbol", c.symbol.empty() ? json(nullptr) : json(c.symbol)}, {"legs", legs},
          {"direction", c.direction == TriggerDirection::AtOrBelow ? "at_or_below" : "at_or_above"},
          {"level", c.level.str()}, {"repeat", a.spec.repeat}, {"created_at", md::format_timestamp(a.created)},
          {"actor", a.actor}, {"armed", a.armed}, {"fired", a.fired},
          {"fired_at", a.fired > 0 ? json(md::format_timestamp(a.fired_at)) : json(nullptr)},
          {"value", a.value ? json(a.value->str()) : json(nullptr)}};
}
const char* tif_name(TimeInForce tif) {
  switch (tif) {
    case TimeInForce::Day: return "day";
    case TimeInForce::Ioc: return "ioc";
    case TimeInForce::Gtc: return "gtc";
    case TimeInForce::Exto: return "exto";
    case TimeInForce::GtcExto: return "gtc_exto";
    case TimeInForce::Gtd: return "gtd";
  }
  return "day";
}
json tif_or_null(const std::optional<TimeInForce>& tif) { return !tif ? json(nullptr) : json(tif_name(*tif)); }
/// Each change asked of an order: the terms requested (null where kept), the terms
/// before it, and whether it was applied or refused, and why.
json order_changes_json(const Order& o) {
  json changes = json::array();
  for (const auto& c : o.changes) {
    json change{{"time", md::format_timestamp(c.time)}, {"actor", c.actor},
        {"quantity", c.quantity ? json(*c.quantity) : json(nullptr)}, {"limit_price", money(c.limit_price)},
        {"trigger_level", money(c.trigger_level)},
        {"previous", {{"quantity", c.previous_quantity}, {"limit_price", money(c.previous_limit_price)},
                      {"trigger_level", money(c.previous_trigger_level)}}},
        {"applied", c.decision.ok()}, {"reason", decision_json(c.decision)}};
    // A change of time in force says so, beside the one before it.
    if (c.time_in_force) {
      change["time_in_force"] = tif_or_null(c.time_in_force);
      change["previous"]["time_in_force"] = tif_or_null(c.previous_time_in_force);
    }
    if (c.walk) {
      change["walk"] = walk_json(*c.walk);
      change["previous"]["walk"] = walk_json(c.previous_walk);
    }
    changes.push_back(std::move(change));
  }
  return changes;
}
/// When the order's terms last changed, or null.
json modified_at(const Order& o) {
  for (auto it = o.changes.rbegin(); it != o.changes.rend(); ++it)
    if (it->decision.ok()) return md::format_timestamp(it->time);
  return nullptr;
}
/// The evaluation attempt an order belongs to: the last one started at or before it.
std::uint64_t order_attempt(const TradingSnapshot& s, OrderId id) {
  if (id >= s.evaluation.first_order) return s.evaluation.attempt;
  for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
    if (id >= it->first_order) return it->attempt;
  return s.attempts.empty() ? s.evaluation.attempt : s.attempts.front().attempt;
}
const char* side_name(Side side) { return side == Side::Buy ? "buy" : "sell"; }
/// A chained order not yet placed, in the order body's shape.
json chained_json(const OrderRequest& r) {
  const bool multi = multi_leg(r);
  json legs = nullptr;
  if (multi) {
    legs = json::array();
    for (const auto& leg : r.legs) legs.push_back({{"symbol", leg.symbol}, {"side", side_name(leg.side)}, {"ratio", leg.ratio}});
  }
  return {{"symbol", multi ? json(nullptr) : json(r.symbol)}, {"side", multi ? json(nullptr) : json(side_name(r.side))},
          {"legs", legs}, {"type", r.type == OrderType::Limit ? "limit" : "market"},
          {"time_in_force", tif_name(r.tif)},
          {"good_till", r.good_till ? time_or_null(*r.good_till) : json(nullptr)},
          {"walk", walk_json(r.walk)}, {"group", nullable(r.group)},
          {"quantity", r.quantity}, {"limit_price", money(r.limit_price)}, {"trigger", trigger_json(r.trigger)},
          {"bracket", r.bracket ? json{{"stop_loss", exit_json(r.bracket->stop_loss)}, {"take_profit", exit_json(r.bracket->take_profit)}}
                                : json(nullptr)},
          {"then", r.then.empty() ? json(nullptr) : chained_json(r.then.front())},
          {"oco", r.oco.empty() ? json(nullptr) : chained_json(r.oco.front())}};
}
json order_json(const Order& o, const TradingView& view) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected", "armed"};
  constexpr const char* roles[] = {"", "stop_loss", "take_profit"};
  const bool multi = multi_leg(o.request);
  json legs = nullptr;
  if (multi) {
    legs = json::array();
    for (const auto& leg : o.request.legs)
      legs.push_back({{"symbol", leg.symbol}, {"side", side_name(leg.side)}, {"ratio", leg.ratio}});
  }
  return {{"id", std::to_string(o.id)}, {"client_order_id", o.request.client_order_id}, {"actor", o.actor},
          {"symbol", multi ? json(nullptr) : json(o.request.symbol)},
          {"underlying", underlying(view, order_symbols(o.request).front())},
          {"side", multi ? json(nullptr) : json(side_name(o.request.side))}, {"legs", legs},
          {"type", o.request.type == OrderType::Limit ? "limit" : "market"},
          {"time_in_force", tif_name(o.request.tif)},
          {"good_till", o.request.good_till ? time_or_null(*o.request.good_till) : json(nullptr)},
          {"limit_ticks", o.limit_ticks ? json(*o.limit_ticks) : json(nullptr)},
          {"walk", walk_json(o.request.walk)}, {"next_walk", walk_step_json(next_walk(o))},
          {"tags", o.request.tags}, {"note", o.request.note}, {"exits_only", o.request.exits_only},
          {"group", nullable(o.request.group)},
          {"quantity", o.request.quantity}, {"filled_quantity", o.filled_quantity},
          {"remaining_quantity", o.remaining()}, {"limit_price", money(o.request.limit_price)},
          {"average_fill_price", o.filled_quantity > 0
              ? json(o.filled_notional.prorate(1, o.filled_quantity).str()) : json(nullptr)},
          {"status", statuses[static_cast<int>(o.status)]},
          {"reason", decision_json(o.reason)},
          {"accepted_at", md::format_timestamp(o.accepted_at)},
          {"day_end", o.day_end > 0 ? json(md::format_timestamp(o.day_end)) : json(nullptr)},
          {"waiting", [&]() -> json {
             const auto wait = view.snapshot->waiting.find(o.id);
             if (!o.open() || wait == view.snapshot->waiting.end()) return nullptr;
             return {{"code", wait->second.code}, {"message", wait->second.message}};
           }()},
          {"ended_at", time_or_null(o.ended_at)}, {"modified_at", modified_at(o)}, {"changes", order_changes_json(o)},
          {"attempt", order_attempt(*view.snapshot, o.id)},
          {"origin", o.system ? "system" : "user"}, {"reduce_only", o.reduce_only},
          {"trigger", trigger_json(o.request.trigger)},
          {"triggered_at", o.triggered_at > 0 ? json(md::format_timestamp(o.triggered_at)) : json(nullptr)},
          {"bracket", o.request.bracket ? json{{"stop_loss", exit_json(o.request.bracket->stop_loss)},
                                               {"take_profit", exit_json(o.request.bracket->take_profit)}} : json(nullptr)},
          {"role", nullable(roles[static_cast<int>(o.role)])}, {"parent", id_or_null(o.parent)}, {"oco", id_or_null(o.oco)},
          {"stop_loss_order", id_or_null(o.stop_loss)}, {"take_profit_order", id_or_null(o.take_profit)},
          {"then", o.request.then.empty() ? json(nullptr) : chained_json(o.request.then.front())},
          {"chained_order", id_or_null(o.chained)}, {"chained_from", id_or_null(o.chained_from)}};
}
json flatten_preview_json(const FlattenPreview& f, const TradingView& view) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected", "armed"};
  json cancelled = json::array();
  for (const auto id : f.cancelled) cancelled.push_back(std::to_string(id));
  // The closing orders, without the IDs a flatten would give them.
  json orders = json::array();
  for (const auto& o : f.orders) {
    const bool multi = multi_leg(o.request);
    json legs = nullptr;
    if (multi) {
      legs = json::array();
      for (const auto& leg : o.request.legs)
        legs.push_back({{"symbol", leg.symbol}, {"side", side_name(leg.side)}, {"ratio", leg.ratio}});
    }
    orders.push_back({{"symbol", multi ? json(nullptr) : json(o.request.symbol)},
                      {"underlying", underlying(view, order_symbols(o.request).front())},
                      {"side", multi ? json(nullptr) : json(side_name(o.request.side))}, {"legs", legs}, {"quantity", o.request.quantity}, {"filled_quantity", o.filled_quantity},
                      {"average_fill_price", o.filled_quantity > 0 ? json(o.filled_notional.prorate(1, o.filled_quantity).str()) : json(nullptr)},
                      {"status", statuses[static_cast<int>(o.status)]}, {"reason", decision_json(o.reason)}});
  }
  json fills = json::array();
  for (const auto& fill : f.fills)
    fills.push_back({{"symbol", fill.symbol}, {"side", side_name(fill.side)}, {"quantity", fill.quantity},
                     {"price", fill.price.str()}, {"fee", fill.fee.str()}});
  json stock_fills = json::array();
  for (const auto& fill : f.stock_fills) stock_fills.push_back({{"symbol", fill.symbol}, {"shares", fill.shares}, {"price", fill.price.str()}});
  json kept = json::array();
  for (const auto& [symbol, decision] : f.kept_stocks) {
    const auto held = f.remaining_shares.find(symbol);
    kept.push_back({{"symbol", symbol}, {"shares", held == f.remaining_shares.end() ? 0 : held->second}, {"reason", decision_json(decision)}});
  }
  json remaining = json::array();
  for (const auto& [symbol, quantity] : f.remaining)
    remaining.push_back({{"symbol", symbol}, {"underlying", underlying(view, symbol)}, {"quantity", quantity}});
  json shares = json::array();
  for (const auto& [symbol, held] : f.remaining_shares) shares.push_back({{"symbol", symbol}, {"shares", held}});
  const auto& grid = view.config.scenarios;
  return {{"decision", f.decision.ok() ? "ok" : to_string(f.decision.code)}, {"reason", decision_json(f.decision)},
          {"cancelled_orders", cancelled}, {"orders", orders}, {"fills", fills}, {"stock_fills", stock_fills},
          {"kept_stocks", kept}, {"remaining", remaining}, {"remaining_shares", shares},
          {"current", what_if_account_json(f.current, grid)}, {"after", what_if_account_json(f.after, grid)}, {"simulated", true}};
}
json context_json(const std::optional<FillContext>& context) {
  if (!context) return nullptr;
  const auto& c = *context;
  const auto optional_number = [](const std::optional<double>& n) { return n ? number(*n) : json(nullptr); };
  return {{"spot", optional_number(c.spot)}, {"spot_source", nullable(c.spot_source)},
          {"iv", optional_number(c.iv)}, {"delta", optional_number(c.delta)}, {"years", optional_number(c.years)},
          {"equity", money(c.equity)}, {"floor_room", money(c.floor_room)}, {"buying_power", money(c.buying_power)}};
}
json review_json(const TradeReview* r, std::optional<Money> net) {
  const auto extreme = [](const std::optional<Excursion>& e) -> json {
    if (!e) return nullptr;
    return {{"pnl", e->pnl.str()}, {"time", md::format_timestamp(e->time)}, {"spot", e->spot ? number(*e->spot) : json(nullptr)}};
  };
  const auto mae = r && r->worst ? std::optional(std::max(Money{}, -r->worst->pnl)) : std::nullopt;
  const auto mfe = r && r->best ? std::optional(std::max(Money{}, r->best->pnl)) : std::nullopt;
  const auto risk = r ? r->planned_risk : std::nullopt;
  return {{"mae", money(mae)}, {"mfe", money(mfe)},
          {"worst", r ? extreme(r->worst) : json(nullptr)}, {"best", r ? extreme(r->best) : json(nullptr)},
          {"planned_risk", money(risk)},
          {"give_back", mfe && net ? json(std::max(Money{}, *mfe - *net).str()) : json(nullptr)},
          {"heat", mae && risk && *risk > Money{} ? number(mae->dollars() / risk->dollars()) : json(nullptr)},
          {"r_multiple", net && risk && *risk > Money{} ? number(net->dollars() / risk->dollars()) : json(nullptr)}};
}
json day_notes_json(const TradingSnapshot& s) {
  json notes = json::object();
  for (const auto& [day, note] : s.day_notes)
    notes[day] = {{"plan", note.plan}, {"review", note.review}, {"time", md::format_timestamp(note.time)}};
  return notes;
}
/// The attempt an option fill belongs to: each attempt's fills start at its first_fill.
std::uint64_t fill_attempt(const TradingSnapshot& s, std::uint64_t fill) {
  if (fill >= s.evaluation.first_fill) return s.evaluation.attempt;
  for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
    if (fill >= it->first_fill) return it->attempt;
  return 1;
}
/// The attempt a share round trip belongs to, by the time it opened.
std::uint64_t time_attempt(const TradingSnapshot& s, Timestamp time) {
  if (time >= s.evaluation.started) return s.evaluation.attempt;
  for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
    if (time >= it->started) return it->attempt;
  return 1;
}
/// A round trip's P&L by Greek over its life; null for one open from before they were kept.
json trip_attribution_json(const TradingSnapshot& s, const std::string& trade) {
  const auto it = s.trip_attributions.find(trade);
  return it == s.trip_attributions.end() ? json(nullptr) : attribution_json(it->second);
}
/// The trading date a trade closed in, holidays included; null while it is open.
json trading_day(const std::optional<Timestamp>& closed) {
  return closed ? json(md::format_date(md::trading_date(*closed))) : json(nullptr);
}
/// The book a fill traded against; null for fills recorded before it was kept.
json fill_quote_json(const Fill& f) {
  if (!f.quote) return nullptr;
  const auto& q = *f.quote;
  return {{"observation", std::to_string(f.observation)}, {"bid", money(q.bid)}, {"ask", money(q.ask)},
          {"bid_size", q.bid_size}, {"ask_size", q.ask_size}, {"size_left", std::max<Quantity>(q.left, 0)},
          {"quoted_at", md::format_timestamp(q.quoted)},
          {"age_seconds", static_cast<double>(f.time - q.quoted) / md::kNanosPerSecond}};
}
json fill_json(const Fill& f, const TradingView& view) {
  return {{"id", std::to_string(f.id)}, {"order_id", std::to_string(f.order_id)}, {"actor", f.actor},
          {"attempt", fill_attempt(*view.snapshot, f.id)},
          {"symbol", f.symbol}, {"underlying", underlying(view, f.symbol)},
          {"side", f.side == Side::Buy ? "buy" : "sell"}, {"quantity", f.quantity},
          {"price", f.price.str()}, {"fee", f.fee.str()}, {"fees", fill_fees_json(f.fees)}, {"context", context_json(f.context)},
          {"quote", fill_quote_json(f)},
          {"quote_time", md::format_timestamp(f.quote_time)}, {"time", md::format_timestamp(f.time)}};
}
json position_greeks(const MarkedPosition& p, const TradingView& view) {
  json out = {{"delta", nullptr}, {"gamma", nullptr}, {"vega", nullptr}, {"theta", nullptr},
              {"dollar_delta", nullptr}, {"dollar_gamma_1pct", nullptr},
              {"vega_dollars", nullptr}, {"theta_dollars", nullptr}};
  const auto it = view.valuations.find(p.position.contract.osi_symbol());
  if (it == view.valuations.end() || p.awaiting_settlement) return out;
  const auto& v = it->second;
  const auto time = view.snapshot->time;
  if (!valid_valuation(v) || v.time > time || time - v.time > view.config.limits.max_valuation_age) return out;
  const auto units = static_cast<double>(p.position.quantity) * 100;
  out["delta"] = number(v.delta); out["gamma"] = number(v.gamma);
  out["vega"] = number(v.vega); out["theta"] = number(v.theta);
  out["dollar_delta"] = number(units * v.delta * v.spot);
  out["dollar_gamma_1pct"] = number(units * v.gamma * v.spot * v.spot * .01);
  out["vega_dollars"] = number(units * v.vega);
  out["theta_dollars"] = number(units * v.theta);
  return out;
}
Money average_price(const Position& position) {
  // Divide basis by signed contracts * multiplier in one rounding step; two
  // successive Money divisions can double-round a micro-dollar average.
  __extension__ using Wide = __int128;
  Wide numerator = position.basis.micros();
  Wide denominator = static_cast<Wide>(position.quantity) * 100;
  if (denominator < 0) { numerator = -numerator; denominator = -denominator; }
  const bool negative = numerator < 0;
  if (negative) numerator = -numerator;
  const auto rounded = static_cast<std::int64_t>((numerator + denominator / 2) / denominator);
  return Money::from_micros(negative ? -rounded : rounded);
}
/// The requirement by underlying: strategy margin's parts, each naming the
/// positions it takes, or the portfolio-margin scan's worst point.
json margin_json(const std::vector<MarginUnderlying>& margin) {
  constexpr const char* kinds[] = {"naked", "vertical", "covered", "straddle", "short_shares", "protected_shares", "worst_loss", "long",
                                   "cash_secured"};
  static_assert(std::size(kinds) == static_cast<std::size_t>(MarginPartKind::CashSecured) + 1);
  json result = json::array();
  for (const auto& item : margin) {
    json parts = json::array();
    for (const auto& part : item.parts) {
      json legs = json::array();
      for (const auto& [symbol, quantity] : part.legs) legs.push_back({{"symbol", symbol}, {"quantity", quantity}});
      parts.push_back({{"kind", kinds[static_cast<int>(part.kind)]}, {"legs", legs}, {"requirement", part.requirement.str()}});
    }
    json scan = nullptr;
    if (item.scan) scan = {{"loss", item.scan->loss.str()}, {"spot_percent", item.scan->spot_percent},
                           {"vol_points", item.scan->vol_points}, {"minimum", item.scan->minimum.str()}};
    result.push_back({{"underlying", item.underlying}, {"requirement", item.requirement.str()}, {"parts", parts}, {"scan", scan},
                      {"allocation", item.allocation.empty() ? json(nullptr) : json(item.allocation)}});
  }
  return result;
}
/// The current quotes whose displayed size this account's orders have taken some of.
json liquidity_used_json(const TradingView& view) {
  json used = json::array();
  for (const auto& [symbol, left] : view.sizes_left)
    used.push_back({{"symbol", symbol}, {"bid_size", left.bid_size}, {"ask_size", left.ask_size},
                    {"bid_left", std::max<Quantity>(0, left.bid)}, {"ask_left", std::max<Quantity>(0, left.ask)}});
  return used;
}
/// A holding's round trip in progress, whose numbers its row shows, and every round
/// trip in it this attempt: how many, and their realised P&L and fees together.
struct Lifetime {
  std::string trade;
  Timestamp opened = 0;
  std::size_t round_trips = 0;
  Money realised, fees;
};
json lifetime_json(const Lifetime& l) {
  return {{"round_trips", l.round_trips}, {"realised", l.realised.str()}, {"fees", l.fees.str()}, {"net", (l.realised - l.fees).str()}};
}
json portfolio_json(const TradingView& view) {
  const auto& s = *view.snapshot;
  std::map<std::string, Lifetime> lifetimes;
  const auto trips = lifecycles(s.recent_fills, s.closures, view.contracts);
  for (const auto& t : trips) {
    if (fill_attempt(s, t.first_fill) != s.evaluation.attempt) continue;
    auto& l = lifetimes[t.symbol];
    ++l.round_trips;
    l.realised = l.realised + t.gross;
    l.fees = l.fees + t.fees;
    if (!t.closed) { l.trade = std::to_string(t.first_fill); l.opened = t.opened; }
  }
  for (const auto& t : share_lifecycles(s.stock_fills, s.dividends)) {
    if (time_attempt(s, t.opened) != s.evaluation.attempt) continue;
    auto& l = lifetimes[t.symbol];
    ++l.round_trips;
    l.realised = l.realised + t.gross + t.dividends;
    if (!t.closed) { l.trade = "s" + std::to_string(t.fills.front()); l.opened = t.opened; }
  }
  const auto trade_of = [&](const std::string& symbol) {
    const auto it = lifetimes.find(symbol);
    return it == lifetimes.end() || it->second.trade.empty() ? json(nullptr) : json(it->second.trade);
  };
  const auto lifetime_of = [&](const std::string& symbol) {
    const auto it = lifetimes.find(symbol);
    return it == lifetimes.end() ? json(nullptr) : lifetime_json(it->second);
  };
  json positions = json::array();
  for (const auto& p : s.positions) {
    const auto& position = p.position;
    const auto& c = position.contract;
    const auto q = position.quantity;
    const auto average = average_price(position);
    positions.push_back({{"symbol", c.osi_symbol()}, {"underlying", c.underlying},
        {"expiry", md::format_date(c.expiry)}, {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"},
        {"expiry_time", md::format_timestamp(c.expiry_time())}, {"last_trade_time", md::format_timestamp(c.last_trade_time())},
        {"strike", c.strike}, {"type", c.type == pricing::OptionType::Call ? "call" : "put"},
        {"quantity", q}, {"average_price", average.str()}, {"basis", position.basis.str()},
        {"mark", money(p.mark)}, {"mark_age_seconds", p.mark ? json(static_cast<double>(p.mark_age) / md::kNanosPerSecond) : json(nullptr)},
        {"market_value", money(p.market_value)}, {"unrealised", money(p.unrealised)},
        {"realised", position.realised.str()}, {"fees", position.fees.str()}, {"fresh", p.fresh},
        {"awaiting_settlement", p.awaiting_settlement}, {"no_bid", p.no_bid}, {"do_not_exercise", p.do_not_exercise},
        // How an expired position settles: on the closing print, recorded or still awaited
        // (with the last print before the close as the fallback) for half an hour after the
        // close, or by a value entered by hand once no automatic source remains.
        {"settle_by", !p.awaiting_settlement ? json(nullptr)
            : c.settlement == md::Settlement::AM && view.opening_settlement &&
              s.time <= c.expiry_time() + kOpeningPrintWait ? json("opening_print")
            : c.settlement == md::Settlement::PM &&
              (s.closing_prints.contains(c.underlying + " " + md::format_date(c.expiry)) ||
               s.time < md::new_york_to_utc(c.expiry, md::regular_close_hour(c.expiry), 0) + kLastPrintWait)
                ? json("closing_print") : json("manual")},
        {"greeks", position_greeks(p, view)},
        {"attribution", s.attributions.contains(c.osi_symbol()) ? attribution_json(s.attributions.at(c.osi_symbol())) : json(nullptr)},
        {"trade", trade_of(c.osi_symbol())}, {"lifetime", lifetime_of(c.osi_symbol())}});
  }
  json stocks = json::array();
  for (const auto& held : s.stocks) {
    const auto& p = held.position;
    const auto magnitude = p.shares < 0 ? -p.shares : p.shares;
    stocks.push_back({{"symbol", p.symbol}, {"shares", p.shares},
        {"average_price", (p.shares < 0 ? -p.basis : p.basis).prorate(1, magnitude).str()}, {"basis", p.basis.str()},
        {"mark", money(held.mark)}, {"mark_time", held.mark ? json(md::format_timestamp(held.mark_time)) : json(nullptr)},
        {"market_value", money(held.market_value)}, {"unrealised", money(held.unrealised)},
        {"realised", p.realised.str()}, {"fees", p.fees.str()}, {"fresh", held.fresh},
        {"attribution", s.attributions.contains(p.symbol) ? attribution_json(s.attributions.at(p.symbol)) : json(nullptr)},
        {"trade", trade_of(p.symbol)}, {"lifetime", lifetime_of(p.symbol)}});
  }
  // Held strategies: the open positions by the whole trade each is in, with every
  // round trip of that trade, closed ones included, in its realised P&L and fees.
  std::map<std::string, std::vector<const Lifecycle*>> whole;
  for (const auto& t : trips) whole[trade_group(t, s.groups)].push_back(&t);
  json strategies = json::array();
  for (const auto& [id, members] : whole) {
    json legs = json::array();
    Money realised, fees;
    std::optional<Money> unrealised = Money{};
    std::set<OrderId> entries;
    for (const auto* t : members) {
      realised = realised + t->gross;
      fees = fees + t->fees;
      entries.insert(t->entry_order);
      if (t->closed) continue;
      std::optional<Money> part;
      for (const auto& p : s.positions)
        if (p.position.contract.osi_symbol() == t->symbol) part = p.unrealised;
      if (unrealised) unrealised = part ? std::optional(*unrealised + *part) : std::nullopt;
      legs.push_back({{"symbol", t->symbol}, {"quantity", t->quantity}, {"trade", std::to_string(t->first_fill)}});
    }
    if (legs.empty()) continue;
    strategies.push_back({{"id", id}, {"underlying", members.front()->contract.underlying},
        {"opened", md::format_timestamp(members.front()->opened)}, {"legs", legs},
        {"round_trips", members.size()}, {"entries", entries.size()},
        {"realised", realised.str()}, {"fees", fees.str()}, {"unrealised", money(unrealised)},
        {"net", unrealised ? json((realised - fees + *unrealised).str()) : json(nullptr)}});
  }
  // Contracts and shares traded today and no longer held keep their P&L by Greek.
  std::set<std::string> held;
  for (const auto& p : s.positions) held.insert(p.position.contract.osi_symbol());
  for (const auto& p : s.stocks) held.insert(p.position.symbol);
  json closed = json::array();
  for (const auto& [symbol, attribution] : s.attributions) {
    if (held.contains(symbol)) continue;
    const auto contract = view.contracts.find(symbol);
    closed.push_back({{"symbol", symbol}, {"kind", contract == view.contracts.end() ? "shares" : "option"},
        {"underlying", contract == view.contracts.end() ? symbol : contract->second.underlying},
        {"attribution", attribution_json(attribution)}});
  }
  json flags = json::array();
  for (auto code : s.quality_flags) flags.push_back(to_string(code));
  return {{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
          {"cash", s.account.cash.str()}, {"equity", s.equity.str()},
          {"start_of_day_equity", s.start_of_day_equity.str()}, {"day_pnl", (s.equity - s.start_of_day_equity).str()},
          {"realised", s.account.realised.str()}, {"unrealised", s.unrealised.str()}, {"fees", s.account.fees.str()},
          {"valuation_complete", s.valuation_complete}, {"quality_flags", flags}, {"positions", positions}, {"stocks", stocks},
          {"buying_power", buying_power_json(s.buying_power)}, {"margin", margin_json(s.margin)},
          {"attribution", attribution_json(s.attribution)},
          {"liquidity_used", liquidity_used_json(view)}, {"closed", closed}, {"strategies", strategies}};
}
json damage_json(const std::optional<AccountDamage>& damage) {
  if (!damage) return nullptr;
  return {{"reason", damage->reason}, {"last_good_seq", damage->last_good_seq},
          {"last_good_time", time_or_null(damage->last_good_time)}};
}
json journal_json(std::uint64_t bytes, std::uint64_t records, bool replay) {
  return {{"bytes", bytes}, {"records", records}, {"warning", nullable(journal_warning(bytes, records, replay))}};
}
json account_json(const TradingView& view) {
  const auto& s = *view.snapshot;
  const auto& r = view.config.rules;
  const auto& e = s.evaluation;
  const bool marked = std::all_of(s.positions.begin(), s.positions.end(), [](const auto& p) { return p.market_value.has_value(); });
  const bool floor = r.max_drawdown > Money{};
  const bool target = r.profit_target > Money{};
  const bool decided = e.status != EvaluationStatus::Active;
  const Money target_equity = e.starting_balance + r.profit_target;
  const auto in = plan_inputs(s);
  const bool executions = counts_executions(r);
  json days = json::array();
  for (const auto& d : e.days) {
    const auto profit = day_profit(d, r);
    days.push_back({{"day", md::format_date(d.day)}, {"open_equity", d.open_equity.str()},
                    {"close_equity", d.close_equity.str()}, {"peak", d.peak.str()},
                    {"floor", floor ? json(d.floor.str()) : json(nullptr)},
                    {"realised", d.realised.str()}, {"qualifying", d.qualifying},
                    {"attribution", attribution_json(d.attribution)},
                    {"low_equity", money(d.low_equity)}, {"high_equity", money(d.high_equity)},
                    {"low_at", time_or_null(d.low_at)}, {"high_at", time_or_null(d.high_at)},
                    {"profit", profit.str()},
                    {"profitable", profit > Money{} && profit >= r.profitable_day_profit},
                    {"executions", executions ? json(d.executions) : json(nullptr)},
                    {"locked", d.locked == Reason::NONE ? json(nullptr) : json(to_string(d.locked))}});
  }
  json objectives = json::array();
  for (const auto& o : evaluation_objectives(e, r, in))
    objectives.push_back({{"code", to_string(o.code)}, {"met", o.met}, {"actual", o.actual ? number(*o.actual) : json(nullptr)},
                          {"required", number(o.required)}, {"message", o.message}});
  const auto stats = day_stats(e, r, in);
  json daily_loss = nullptr;
  if (const auto level = daily_loss_level(e, r, in))
    daily_loss = {{"limit", r.daily_loss_limit.str()}, {"basis", kDailyLossBases[static_cast<int>(r.daily_loss_basis)]},
                  {"action", kBreachActions[static_cast<int>(r.daily_loss_action)]}, {"reference", level->reference.str()},
                  {"level", level->level.str()}, {"room", (s.equity - level->level).str()}};
  const auto timed = time_rule_progress(e, r, s.time);
  json scaling = nullptr;
  if (!r.scaling.empty()) {
    json next = nullptr;
    for (const auto& step : r.scaling)
      if (step.contracts > e.scaling_limit) {
        next = {{"profit", step.profit.str()}, {"contracts", step.contracts}};
        break;
      }
    scaling = {{"limit", e.scaling_limit},
               {"held", contracts_held(s.positions, [](const auto& p) { return p.position.quantity; })},
               {"profit", (in.balance - e.starting_balance).str()}, {"next", next}};
  }
  json size_scaling = nullptr;
  if (r.size_scaling && e.size_scaling) {
    const auto& rule = *r.size_scaling;
    const auto& period = *e.size_scaling;
    json history = json::array();
    for (const auto& h : period.history)
      history.push_back({{"day", md::format_date(h.day)}, {"old", h.old.str()}, {"size", h.size.str()}});
    size_scaling = {{"size", e.starting_balance.str()}, {"original", period.original.str()},
        {"original_max_drawdown", period.original_max_drawdown.str()},
        {"original_daily_loss_limit", period.original_daily_loss_limit.str()},
        {"max_balance", rule.max_balance.str()}, {"period_started", md::format_date(period.period_started)},
        {"period_days", period.period_days}, {"days_required", rule.days},
        {"period_profit", size_scaling_profit(e, in.balance).str()}, {"profit_required", size_scaling_required(e, rule).str()},
        {"period_payouts", e.payouts.size() - period.payouts_at_start}, {"payouts_required", rule.payouts},
        {"next_size", (e.starting_balance + size_scaling_increase(e, rule)).str()}, {"history", history}};
  }
  const bool liquidated = decided && in.flat;
  json payouts = json::array();
  for (const auto& p : e.payouts)
    payouts.push_back({{"number", p.number}, {"time", md::format_timestamp(p.time)}, {"day", md::format_date(p.day)}, {"amount", p.amount.str()},
                       {"trader_share", p.trader_share.str()}, {"balance", p.balance.str()}});
  json next_plans = json::array();
  if (e.status == EvaluationStatus::Passed) {
    const auto id = preset_id(e.size_scaling ? e.size_scaling->original : e.starting_balance,
        original_program_rules(r, e.starting_balance, e.size_scaling));
    for (const auto& p : plan_presets())
      if (!id.empty() && p.unlocked_by == id) next_plans.push_back(p.id);
  }
  const auto costs = program_costs(s, r);
  json attempts = json::array();
  for (const auto& a : s.attempts)
    attempts.push_back({{"attempt", a.attempt}, {"plan", nullable(a.plan)}, {"plan_id", nullptr}, {"started", md::format_timestamp(a.started)},
                        {"ended", md::format_timestamp(a.ended)}, {"starting_balance", a.starting_balance.str()},
                        {"final_equity", a.final_equity.str()}, {"status", status_name(a.status)},
                        {"fee_charged", a.fee_charged.amount.str()}, {"fee_kind", nullable(a.fee_charged.kind)},
                        {"decision", nullable(a.decision)}, {"rules", a.rules ? rules_json(*a.rules, a.starting_balance, a.size_scaling) : json(nullptr)},
                        {"decided_at", time_or_null(a.decided_at)}, {"decided_equity", money(a.decided_equity)},
                        {"peak", money(a.peak)}, {"floor", money(a.floor)},
                        {"decision_code", a.status == EvaluationStatus::Active ? json(nullptr) : json(to_string(a.decision_code))}});
  return {{"damaged", damage_json(view.damaged)}, {"journal_size", journal_json(view.journal_bytes, view.journal_transactions, view.run.has_value())},
          {"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
          {"rules", rules_json(r, e.starting_balance, e.size_scaling)}, {"breach", breach_json(view.breach)}, {"warnings", warnings_json(view.warnings)},
          {"guardrails", guardrails_json(view.config.guardrails)}, {"guardrail_state", guardrail_state_json(s)},
          {"evaluation", {
              {"enabled", r.evaluation()}, {"attempt", e.attempt}, {"status", status_name(e.status)},
              {"started", md::format_timestamp(e.started)}, {"starting_balance", e.starting_balance.str()},
              {"equity", s.equity.str()}, {"marked", marked}, {"valuation_complete", s.valuation_complete},
              {"profit", (s.equity - e.starting_balance).str()}, {"peak", e.peak.str()},
              {"floor", floor ? json(e.floor.str()) : json(nullptr)},
              {"drawdown_buffer", floor ? json((s.equity - e.floor).str()) : json(nullptr)},
              {"target_equity", target ? json(target_equity.str()) : json(nullptr)},
              // Liquidating a pass at the bid can leave equity just under the target it reached.
              // A target on the closed balance counts the balance.
              {"target_remaining", target ? json((e.status == EvaluationStatus::Passed ? Money{}
                  : std::max(Money{}, target_equity - (r.profit_basis == ProfitBasis::Balance ? in.balance : s.equity))).str())
                  : json(nullptr)},
              {"decided_at", decided ? json(md::format_timestamp(e.decided_at)) : json(nullptr)},
              {"decided_equity", decided ? json(e.decided_equity.str()) : json(nullptr)},
              {"decision", nullable(e.decision)},
              {"day", md::format_date(e.day)}, {"day_open_equity", e.day_open_equity.str()},
              {"day_close_equity", e.day_close_equity.str()}, {"days", days},
              {"day_low_equity", money(e.day_low_equity)}, {"day_high_equity", money(e.day_high_equity)},
              {"day_low_at", time_or_null(e.day_low_at)}, {"day_high_at", time_or_null(e.day_high_at)},
              {"closest_floor", money(e.closest_floor)}, {"closest_floor_at", time_or_null(e.closest_floor_at)},
              {"floor_locked", floor && e.floor_locked}, {"qualifying_days", e.qualifying_days},
              {"cycle_started", md::format_timestamp(e.cycle_started)}, {"payouts", payouts},
              {"decision_code", decided ? json(to_string(e.decision_code)) : json(nullptr)},
              {"balance", in.balance.str()}, {"profit_basis", kProfitBases[static_cast<int>(r.profit_basis)]},
              {"objectives", objectives},
              {"trading_days", executions ? json(stats.trading_days) : json(nullptr)},
              {"profitable_days", stats.profitable_days},
              {"best_day", stats.best_day ? json{{"day", md::format_date(stats.best_day_date)}, {"profit", stats.best_day->str()}}
                                          : json(nullptr)},
              {"short_profit", r.phase != Phase::Funded && r.microscalp_percent > 0 ? json(e.short_profit.str()) : json(nullptr)},
              {"closed_trades", r.phase != Phase::Funded && r.min_trades > 0 ? json(e.closed_trades) : json(nullptr)},
              {"best_trade", e.best_trade ? json{{"id", e.best_trade->id}, {"pnl", e.best_trade->pnl.str()}} : json(nullptr)},
              {"consistency_target", money(consistency_target(e, r, in))},
              {"daily_loss", daily_loss},
              {"next_event", [&] { const auto upcoming = restricting_events_json(r, s.time, false);
                  return upcoming.empty() ? json(nullptr) : upcoming.front(); }()},
              {"active_events", restricting_events_json(r, s.time, true)},
              {"time_limit_days", r.time_limit_days > 0 ? json(r.time_limit_days) : json(nullptr)},
              {"deadline", timed.deadline ? json(md::format_date(*timed.deadline)) : json(nullptr)},
              {"days_left", timed.days_left ? json(*timed.days_left) : json(nullptr)},
              {"last_activity", time_or_null(timed.last_activity)},
              {"inactive_days", timed.inactive_days ? json(*timed.inactive_days) : json(nullptr)},
              {"inactivity_deadline", timed.inactivity_deadline ? json(md::format_date(*timed.inactivity_deadline)) : json(nullptr)},
              {"flat_time", r.flat_time ? json(clock_text(*r.flat_time)) : json(nullptr)},
              {"flat_now", plan_flat_now(r, s.time)},
              {"scaling", scaling},
              {"size_scaling", size_scaling},
              {"day_lock", e.day_lock == Reason::NONE ? json(nullptr) : json(to_string(e.day_lock))},
              {"day_locked_at", time_or_null(e.day_locked_at)},
              {"exit_equity", s.exit_equity.str()}, {"exit_cost", (s.equity - s.exit_equity).str()},
              {"liquidated_equity", liquidated ? json(s.equity.str()) : json(nullptr)},
              {"liquidation_cost", liquidated ? json((e.decided_equity - s.equity).str()) : json(nullptr)}}},
          {"buying_power", buying_power_json(s.buying_power)},
          {"payout", payout_json(view)},
          {"next_plans", next_plans},
          {"costs", {{"evaluation", costs.evaluation.str()}, {"reset", costs.reset.str()}, {"activation", costs.activation.str()},
                     {"total", costs.total().str()}, {"resets_used", costs.resets_used},
                     {"resets_left", r.max_resets == 0 ? json(nullptr) : json(std::max<std::int64_t>(0, r.max_resets - costs.resets_used))},
                     {"payouts_received", costs.payouts_received.str()}, {"net", (costs.payouts_received - costs.total()).str()},
                     {"fee_charged", s.fee_charged.amount.str()}, {"fee_kind", nullable(s.fee_charged.kind)}}},
          {"attempts", attempts}};
}
/// How a change in shares came about.
json share_source(const StockFill& fill) {
  switch (fill.source) {
    case StockSource::Exercise: return "early_exercise";
    case StockSource::Delivery: {
      // Long calls and short puts buy at expiry; the short side is assigned.
      const auto option = md::parse_osi(fill.option);
      const bool call = option && option->type == pricing::OptionType::Call;
      return (call ? fill.shares < 0 : fill.shares > 0) ? "assignment" : "expiry_exercise";
    }
    case StockSource::Assignment: return "assignment";
    case StockSource::Trade: return "trade";
    case StockSource::Rule: return "rule";
    case StockSource::Reset: return "reset";
  }
  return nullptr;
}
json stock_fill_json(const StockFill& fill) {
  return {{"id", std::to_string(fill.id)}, {"symbol", fill.symbol}, {"shares", fill.shares},
          {"price", fill.price.str()}, {"time", md::format_timestamp(fill.time)}, {"source", share_source(fill)},
          {"option", nullable(fill.option)}, {"buying_power_after", money(fill.buying_power_after)}};
}
const char* closure_name(ClosureKind kind) {
  return kind == ClosureKind::Settlement ? "settlement" : kind == ClosureKind::Exercise ? "exercise"
       : kind == ClosureKind::Assignment ? "assignment" : kind == ClosureKind::Abandon ? "abandon" : "reset";
}
/// What closed a round trip, and for a reducer liquidation why: the closure that ended
/// it, or its last reducing fill's order. A system order's client ID is system:<why>:<id>.
std::pair<json, json> closed_by(const TradingSnapshot& s, const Lifecycle& t) {
  if (!t.closed) return {nullptr, nullptr};
  if (t.closure) return {closure_name(*t.closure), nullptr};
  if (t.exit_order == 0 || t.exit_order > s.recent_orders.size()) return {nullptr, nullptr};
  const auto& order = s.recent_orders[t.exit_order - 1];
  const auto& client = order.request.client_order_id;
  if (order.system) {
    const auto first = client.find(':'), last = client.rfind(':');
    return {"system", first != std::string::npos && last > first ? json(client.substr(first + 1, last - first - 1)) : json(nullptr)};
  }
  if (order.role == OrderRole::StopLoss) return {"stop_loss", nullptr};
  if (order.role == OrderRole::TakeProfit) return {"take_profit", nullptr};
  if (client.starts_with("openport-close-")) return {"flatten", nullptr};
  if (client.starts_with("pb-close:")) return {"playbook", nullptr};
  return {"order", nullptr};
}
/// The replay run behind an account's trades, or null for a live account.
/// Net P&L over the buying power an entry needed: closed trades with a positive need only.
json return_on(Money net, Money buying_power, bool closed) {
  return closed && buying_power > Money{} ? number(net.dollars() / buying_power.dollars()) : json(nullptr);
}
json run_json(const std::optional<RunIdentity>& run) {
  if (!run) return nullptr;
  return {{"id", nullable(run->id)}, {"scenario", nullable(run->scenario)}, {"seed", nullable(run->seed)},
          {"recording", nullable(run->recording)}, {"date", nullable(run->date)}};
}
json trades_json(const TradingView& view, std::string_view status, bool current_only) {
  const auto& s = *view.snapshot;
  const auto& e = s.evaluation;
  std::map<std::pair<std::string, Timestamp>, const SettlementRecord*> settlements;
  for (const auto& record : s.settlements) settlements[{record.contract.osi_symbol(), record.time}] = &record;
  const auto all = lifecycles(s.recent_fills, s.closures, view.contracts);
  json trades = json::array();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    const auto& t = *it;
    const bool open = !t.closed;
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    const auto attempt = fill_attempt(s, t.first_fill);
    if (current_only && attempt != e.attempt) continue;
    const auto& c = t.contract;
    const Money cost = t.open_notional * 100;
    const Money net = t.gross - t.fees;
    json mark = nullptr, unrealised = nullptr;
    if (open) {
      for (const auto& p : s.positions) {
        if (p.position.contract.osi_symbol() != t.symbol) continue;
        mark = money(p.mark);
        unrealised = money(p.unrealised);
      }
    }
    json fills = json::array();
    for (const auto id : t.fills) fills.push_back(std::to_string(id));
    const auto a = s.annotations.find(std::to_string(t.first_fill));
    const auto exit = closed_by(s, t);
    const auto review = s.trade_reviews.find(std::to_string(t.first_fill));
    const auto order_id = s.recent_fills.at(t.first_fill - 1).order_id;
    const auto strategy = s.strategy_reviews.find(std::to_string(order_id));
    std::optional<Money> strategy_net;
    std::vector<const Lifecycle*> strategy_legs;
    if (strategy != s.strategy_reviews.end()) {
      for (const auto& leg : all)
        if (s.recent_fills.at(leg.first_fill - 1).order_id == order_id) strategy_legs.push_back(&leg);
      if (strategy->second.finished) {
        strategy_net = Money{};
        for (const auto* leg : strategy_legs) *strategy_net = *strategy_net + leg->gross - leg->fees;
      }
    }
    const auto settled = t.closure == ClosureKind::Settlement && t.closed
        ? settlements.find({t.symbol, *t.closed}) : settlements.end();
    const auto* settlement = settled == settlements.end() ? nullptr : settled->second;
    const auto buying_power = entry_buying_power({&t});
    const auto strategy_power = strategy_legs.empty() ? std::optional<Money>{} : std::optional(entry_buying_power(strategy_legs));
    trades.push_back({{"entry_context", context_json(t.entry_context)}, {"exit_context", context_json(t.exit_context)},
        {"review", review_json(review == s.trade_reviews.end() ? nullptr : &review->second, open ? std::nullopt : std::optional(net))},
        {"strategy_id", strategy == s.strategy_reviews.end() ? json(nullptr) : json(std::to_string(order_id))},
        {"strategy_review", strategy == s.strategy_reviews.end() ? json(nullptr) : review_json(&strategy->second, strategy_net)},
        {"buying_power", buying_power.str()}, {"return_on_buying_power", return_on(net, buying_power, !open)},
        {"strategy_buying_power", money(strategy_power)},
        {"strategy_return_on_buying_power", strategy_power && strategy_net ? return_on(*strategy_net, *strategy_power, true) : json(nullptr)},
        {"id", std::to_string(t.first_fill)}, {"attempt", attempt}, {"symbol", t.symbol},
        {"underlying", c.underlying}, {"expiry", md::format_date(c.expiry)},
        {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"}, {"strike", c.strike},
        {"type", c.type == pricing::OptionType::Call ? "call" : "put"},
        {"direction", t.direction > 0 ? "long" : "short"}, {"status", open ? "open" : "closed"},
        {"opened", md::format_timestamp(t.opened)},
        {"closed", open ? json(nullptr) : json(md::format_timestamp(*t.closed))}, {"trading_day", trading_day(t.closed)},
        {"duration_seconds", open ? json(nullptr) : json((*t.closed - t.opened) / md::kNanosPerSecond)},
        {"quantity", t.quantity}, {"max_quantity", t.max_quantity},
        {"opened_contracts", t.opened_contracts}, {"closed_contracts", t.closed_contracts},
        {"average_open", average(t.open_notional, t.opened_contracts)},
        {"average_close", average(t.close_notional, t.closed_contracts)},
        {"cost", cost.str()}, {"gross", t.gross.str()}, {"fees", t.fees.str()}, {"net", net.str()},
        {"return", open || cost == Money{} ? json(nullptr) : number(net.dollars() / cost.dollars())},
        {"mark", mark}, {"unrealised", unrealised},
        {"closure", !t.closure ? json(nullptr) : json(closure_name(*t.closure))},
        {"settlement_value", settlement ? json(settlement->value.str()) : json(nullptr)},
        {"settlement_source", settlement && settlement->source && settlement->source->contains("kind")
            ? json(settlement->source->at("kind")) : json(nullptr)},
        {"closed_by", exit.first}, {"system_reason", exit.second},
        {"attribution", trip_attribution_json(s, std::to_string(t.first_fill))},
        {"group", trade_group(t, s.groups)},
        {"fills", fills},
        {"note", a == s.annotations.end() ? std::string{} : a->second.note},
        {"tags", a == s.annotations.end() ? json::array() : json(a->second.tags)}});
  }
  // Whole trades: the round trips of a trade with more than one entry (a roll, an
  // adjustment or legs the account grouped), newest first.
  struct Whole {
    std::vector<const Lifecycle*> members;
    std::set<OrderId> entries;
  };
  std::map<std::string, Whole> wholes;
  for (const auto& t : all) {
    auto& whole = wholes[trade_group(t, s.groups)];
    whole.members.push_back(&t);
    whole.entries.insert(t.entry_order);
  }
  std::vector<std::pair<std::string, const Whole*>> listed;
  for (const auto& [id, whole] : wholes)
    if (whole.entries.size() > 1 || s.group_reviews.contains(id)) listed.emplace_back(id, &whole);
  std::sort(listed.begin(), listed.end(), [](const auto& a, const auto& b) {
    return a.second->members.front()->first_fill > b.second->members.front()->first_fill;
  });
  json groups = json::array();
  for (const auto& [id, whole] : listed) {
    const auto& first = *whole->members.front();
    const bool open = std::any_of(whole->members.begin(), whole->members.end(), [](const Lifecycle* t) { return !t->closed; });
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    const auto attempt = fill_attempt(s, first.first_fill);
    if (current_only && attempt != e.attempt) continue;
    Money gross, fees;
    std::optional<Money> unrealised = Money{};
    Timestamp closed = 0;
    json members = json::array();
    for (const auto* t : whole->members) {
      gross = gross + t->gross;
      fees = fees + t->fees;
      if (t->closed) closed = std::max(closed, *t->closed);
      members.push_back(std::to_string(t->first_fill));
      if (t->closed || !unrealised) continue;
      std::optional<Money> part;
      for (const auto& p : s.positions)
        if (p.position.contract.osi_symbol() == t->symbol) part = p.unrealised;
      unrealised = part ? std::optional(*unrealised + *part) : std::nullopt;
    }
    const Money net = gross - fees;
    const auto review = s.group_reviews.find(id);
    const auto peak = peak_buying_power(whole->members);
    groups.push_back({{"id", id}, {"attempt", attempt}, {"underlying", first.contract.underlying},
        {"status", open ? "open" : "closed"}, {"opened", md::format_timestamp(first.opened)},
        {"closed", open ? json(nullptr) : json(md::format_timestamp(closed))},
        {"trading_day", open ? json(nullptr) : trading_day(closed)},
        {"round_trips", members}, {"entries", whole->entries.size()},
        {"gross", gross.str()}, {"fees", fees.str()}, {"net", net.str()},
        {"unrealised", open && unrealised ? json(unrealised->str()) : json(nullptr)},
        {"buying_power", peak.str()}, {"return_on_buying_power", return_on(net, peak, !open)},
        {"review", review == s.group_reviews.end() ? json(nullptr)
                   : review_json(&review->second, open ? std::nullopt : std::optional(net))},
        {"review_since", review == s.group_reviews.end() || !review->second.since ? json(nullptr)
                         : json(md::format_timestamp(*review->second.since))}});
  }
  // Shares from exercise and assignment, round trip by round trip, newest first.
  const auto attempt_at = [&](Timestamp time) { return time_attempt(s, time); };
  const auto source = [&](std::uint64_t id) -> json {
    if (id == 0 || id > s.stock_fills.size()) return nullptr;
    return share_source(s.stock_fills[id - 1]);
  };
  const auto option_of = [&](std::uint64_t id) -> json {
    if (id == 0 || id > s.stock_fills.size() || s.stock_fills[id - 1].option.empty()) return nullptr;
    return s.stock_fills[id - 1].option;
  };
  const auto shares = share_lifecycles(s.stock_fills, s.dividends);
  json share_trades = json::array();
  for (auto it = shares.rbegin(); it != shares.rend(); ++it) {
    const auto& t = *it;
    const bool open = !t.closed;
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    const auto attempt = attempt_at(t.opened);
    if (current_only && attempt != e.attempt) continue;
    json mark = nullptr, unrealised = nullptr;
    if (open) {
      for (const auto& held : s.stocks) {
        if (held.position.symbol != t.symbol) continue;
        mark = money(held.mark);
        unrealised = money(held.unrealised);
      }
    }
    // Stock fill IDs carry the "s" of the share trade IDs, apart from option fills'.
    json fills = json::array();
    for (const auto id : t.fills) fills.push_back("s" + std::to_string(id));
    const auto first = t.fills.front();
    const auto last = open ? std::uint64_t{0} : t.fills.back();
    const auto note = s.annotations.find("s" + std::to_string(first));
    share_trades.push_back({{"kind", "shares"}, {"id", "s" + std::to_string(first)}, {"attempt", attempt},
        {"symbol", t.symbol}, {"direction", t.direction > 0 ? "long" : "short"}, {"status", open ? "open" : "closed"},
        {"opened", md::format_timestamp(t.opened)},
        {"closed", open ? json(nullptr) : json(md::format_timestamp(*t.closed))}, {"trading_day", trading_day(t.closed)},
        {"duration_seconds", open ? json(nullptr) : json((*t.closed - t.opened) / md::kNanosPerSecond)},
        {"shares", t.shares}, {"max_shares", t.max_shares},
        {"opened_shares", t.opened_shares}, {"closed_shares", t.closed_shares},
        {"average_open", average(t.open_notional, t.opened_shares)},
        {"average_close", average(t.close_notional, t.closed_shares)},
        {"cost", t.open_notional.str()}, {"gross", t.gross.str()}, {"dividends", t.dividends.str()},
        {"fees", Money{}.str()}, {"net", (t.gross + t.dividends).str()},
        {"return", open || t.open_notional == Money{} ? json(nullptr) : number((t.gross + t.dividends).dollars() / t.open_notional.dollars())},
        {"mark", mark}, {"unrealised", unrealised},
        {"opened_by", source(first)}, {"option", option_of(first)},
        {"closed_by", source(last)}, {"closing_option", option_of(last)},
        {"attribution", trip_attribution_json(s, "s" + std::to_string(first))}, {"fills", fills},
        {"note", note == s.annotations.end() ? std::string{} : note->second.note},
        {"tags", note == s.annotations.end() ? json::array() : json(note->second.tags)}});
  }
  // Every change in shares and every dividend, oldest first, for the terminal's alerts.
  json stock_fills = json::array();
  for (const auto& fill : s.stock_fills) stock_fills.push_back(stock_fill_json(fill));
  json dividends = json::array();
  for (const auto& d : s.dividends)
    dividends.push_back({{"symbol", d.symbol}, {"ex_date", md::format_date(d.ex_date)}, {"per_share", d.per_share.str()},
        {"shares", d.shares}, {"amount", d.amount.str()}, {"time", md::format_timestamp(d.time)}});
  return {{"account_version", std::to_string(s.account_version)}, {"attempt", e.attempt}, {"trades", trades},
          {"groups", groups}, {"share_trades", share_trades}, {"stock_fills", stock_fills}, {"dividends", dividends}, {"day_notes", day_notes_json(s)},
          {"run", run_json(view.run)}};
}
json unlocks_json(const PlanPreset& plan) {
  json out = json::array();
  for (const auto& candidate : plan_presets())
    if (candidate.unlocked_by == plan.id) out.push_back(candidate.id);
  return out;
}
json plans_json() {
  json plans = json::array();
  for (const auto& p : plan_presets())
    plans.push_back({{"id", p.id}, {"name", p.name}, {"summary", p.summary},
                     {"initial_cash", p.initial_cash.str()}, {"rules", rules_json(p.rules, p.initial_cash)},
                     {"phase", p.rules.phase == Phase::Funded ? "funded" : p.rules.phase == Phase::Verification ? "verification" : "evaluation"},
                     {"unlocked_by", nullable(p.unlocked_by)}, {"unlocks", unlocks_json(p)}});
  return {{"plans", plans}};
}
json exposure_limits(const ExposureLimits& limits) {
  return {{"dollar_delta", limits.dollar_delta}, {"vega", limits.vega}};
}
json limits_json(const Limits& limits) {
  json overrides = json::object();
  for (const auto& [symbol, caps] : limits.underlying_overrides) overrides[symbol] = exposure_limits(caps);
  return {{"max_order_contracts", limits.max_order_contracts}, {"price_band_absolute", limits.price_band_absolute.str()},
          {"price_band_relative", limits.price_band_relative}, {"aggregate", exposure_limits(limits.aggregate)},
          {"per_underlying", exposure_limits(limits.per_underlying)}, {"underlying_overrides", overrides},
          {"max_daily_loss", limits.max_daily_loss.str()},
          {"max_quote_age_seconds", limits.max_quote_age / md::kNanosPerSecond},
          {"max_valuation_age_seconds", limits.max_valuation_age / md::kNanosPerSecond}};
}
json bucket_json(const RiskBucket& b) {
  return {{"dollar_delta", b.position.dollar_delta}, {"dollar_gamma_1pct", b.position.dollar_gamma_1pct},
          {"vega", b.position.vega}, {"theta", b.position.theta},
          {"reachable", {{"delta_low", b.reachable.delta_low}, {"delta_high", b.reachable.delta_high},
                         {"vega_low", b.reachable.vega_low}, {"vega_high", b.reachable.vega_high}}},
          {"limits", exposure_limits(b.limits)}, {"delta_utilisation", b.delta_utilisation}, {"vega_utilisation", b.vega_utilisation}};
}
/// The latch, why the kill-switch reset could not clear it now (null when it could), and its
/// last 50 trips, resets and releases, oldest first.
json kill_json(const TradingSnapshot& s) {
  constexpr std::size_t kHistory = 50;
  json history = json::array();
  const auto& all = s.kill_history;
  for (auto i = all.size() > kHistory ? all.size() - kHistory : 0; i < all.size(); ++i)
    history.push_back({{"time", md::format_timestamp(all[i].time)}, {"action", all[i].action}, {"reason", nullable(all[i].reason)},
                       {"previous", nullable(all[i].previous)}, {"actor", all[i].actor}});
  return {{"latched", s.risk.kill_latched}, {"reason", nullable(s.risk.kill_reason)},
          {"reset_blocked", decision_json(s.kill_reset)}, {"history", std::move(history)}};
}
json risk_json(const TradingView& view) {
  const auto& s = *view.snapshot;
  json underlyings = json::object();
  for (const auto& [symbol, bucket] : s.risk.underlyings) underlyings[symbol] = bucket_json(bucket);
  json pnl = json::array(), clamped = json::array();
  std::size_t index = 0;
  for ([[maybe_unused]] double spot : view.config.scenarios.spot_percent) {
    json row = json::array(), clamps = json::array();
    for ([[maybe_unused]] double vol : view.config.scenarios.vol_points) {
      const auto& cell = s.scenarios.cells.at(index++);
      row.push_back(s.scenarios.complete ? number(cell.pnl) : json(nullptr));
      clamps.push_back(cell.clamped);
    }
    pnl.push_back(std::move(row)); clamped.push_back(std::move(clamps));
  }
  return {{"account_version", std::to_string(s.account_version)}, {"limits_revision", std::to_string(s.risk.limits_revision)},
          {"limits", limits_json(view.config.limits)},
          {"pending_limits", s.pending_limits ? limits_json(*s.pending_limits) : json(nullptr)},
          {"guardrails", guardrails_json(view.config.guardrails)},
          {"pending_guardrails", s.pending_guardrails ? guardrails_json(*s.pending_guardrails) : json(nullptr)},
          {"guardrail_state", guardrail_state_json(s)}, {"pending_applied_at", time_or_null(s.pending_applied_at)},
          {"pending_applied_day", s.pending_applied_at > 0 ? json(md::format_date(plan_trading_date(view.config.rules, s.pending_applied_at))) : json(nullptr)},
          {"pending_effective", s.pending_limits || s.pending_guardrails ? json("next_trading_day") : json(nullptr)},
          {"pending_requires_reset", (s.pending_limits || s.pending_guardrails) && view.replay_end > 0 &&
              plan_trading_date(view.config.rules, view.replay_end) <=
              plan_trading_date(view.config.rules, std::max(s.time, view.replay_start))},
          {"time", md::format_timestamp(s.time)}, {"breach", breach_json(view.breach)}, {"warnings", warnings_json(view.warnings)},
          {"complete", s.risk.complete}, {"daily_loss", s.risk.daily_loss.str()},
          {"kill", kill_json(s)}, {"aggregate", bucket_json(s.risk.aggregate)}, {"underlyings", underlyings},
          {"scenarios", {{"spot_percent", view.config.scenarios.spot_percent}, {"vol_points", view.config.scenarios.vol_points},
                         {"pnl", pnl}, {"clamped", clamped}, {"complete", s.scenarios.complete}}}};
}

int reason_status(Reason reason) {
  if (reason == Reason::INVALID_RULES) return 400;
  if (reason == Reason::UNKNOWN_ORDER || reason == Reason::UNKNOWN_CONTRACT || reason == Reason::UNKNOWN_TRADE ||
      reason == Reason::UNKNOWN_ALERT) return 404;
  if (reason == Reason::ORDER_TERMINAL || reason == Reason::DUPLICATE_CLIENT_ID || reason == Reason::RESET_LIMIT) return 409;
  if (reason == Reason::JOURNAL_IO || reason == Reason::JOURNAL_CORRUPT ||
      reason == Reason::JOURNAL_LOCKED) return 503;
  return 422;
}
ApiResponse command_response(const TradingCommand& command, const TradingReply& reply) {
  if (!reply.error_code.empty())
    return api_error(reply.error_code == "INVALID_REQUEST" ? 400 : reply.error_code == "ACCOUNT_PROTECTED" ? 403 :
                     reply.error_code == "ACCOUNT_DAMAGED" || reply.error_code == "ACCOUNT_ARCHIVED" || reply.error_code == "ACCOUNT_NOT_EMPTY" ||
                     reply.error_code == "LIMITS_REVISION" || reply.error_code == "ACCOUNTS_UNSUPPORTED" ? 409
                     : reply.error_code == "UNKNOWN_ACCOUNT" ? 404 : 503,
                     reply.error_code, reply.decision.message);
  if (!reply.decision.ok()) return api_error(reason_status(reply.decision.code),
      std::string(to_string(reply.decision.code)), reply.decision.message, reply.decision);
  const auto* preview_decision = reply.preview ? &reply.preview->decision : reply.stock_preview ? &reply.stock_preview->decision : nullptr;
  if (preview_decision && (preview_decision->code == Reason::FLAT_TIME || preview_decision->code == Reason::NEWS_BLACKOUT || preview_decision->code == Reason::HOLD_RESTRICTED))
    return api_error(422, std::string(to_string(preview_decision->code)), preview_decision->message, *preview_decision);
  if (!reply.account_result.empty()) return {200, reply.account_result};
  if (!reply.view || !reply.view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "No trading publication");
  const auto& view = *reply.view;
  const auto& s = *view.snapshot;
  json body{{"account_version", std::to_string(s.account_version)}};
  int status = 200;
  switch (command.kind) {
    case TradingCommand::Kind::UpdateAccount:
    case TradingCommand::Kind::DeleteAccount: break;  // handled above
    case TradingCommand::Kind::Submit:
    case TradingCommand::Kind::Cancel:
    case TradingCommand::Kind::Modify: {
      const auto it = std::find_if(s.recent_orders.begin(), s.recent_orders.end(), [&](const auto& o) { return o.id == reply.order_id; });
      if (it == s.recent_orders.end()) return api_error(503, "TRADING_UNAVAILABLE", "Order publication missing");
      body["order"] = order_json(*it, view);
      if (command.kind != TradingCommand::Kind::Cancel) {
        if (command.kind == TradingCommand::Kind::Submit && !reply.replayed) status = 201;
        body["fills"] = json::array();
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == it->id) body["fills"].push_back(fill_json(fill, view));
      }
      break;
    }
    case TradingCommand::Kind::CancelAll:
    case TradingCommand::Kind::ClosePositions: {
      body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      if (command.kind == TradingCommand::Kind::CancelAll) break;
      // Each closing order with its outcome: a rejection carries its reason.
      body["orders"] = json::array();
      body["fills"] = json::array();
      for (auto id : reply.created_orders) {
        const auto& order = s.recent_orders.at(static_cast<std::size_t>(id - 1));
        body["orders"].push_back(order_json(order, view));
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == id) body["fills"].push_back(fill_json(fill, view));
      }
      // Delivered shares: those it closed, and those still held that it could not.
      body["stock_fills"] = json::array();
      for (auto id : reply.stock_fills) body["stock_fills"].push_back(stock_fill_json(s.stock_fills.at(static_cast<std::size_t>(id - 1))));
      body["kept_stocks"] = json::array();
      for (const auto& stock : s.stocks)
        if (const auto kept = reply.kept_stocks.find(stock.position.symbol); kept != reply.kept_stocks.end())
          body["kept_stocks"].push_back({{"symbol", stock.position.symbol}, {"shares", stock.position.shares},
              {"reason", decision_json(kept->second)}});
      // Positions still open: the contracts its closes are still working, and why the rest are not.
      body["residuals"] = json::array();
      for (const auto& residual : reply.residuals)
        body["residuals"].push_back({{"symbol", residual.symbol}, {"underlying", underlying(view, residual.symbol)},
            {"quantity", residual.quantity}, {"working", residual.working}, {"reason", decision_json(residual.reason)}});
      break;
    }
    case TradingCommand::Kind::Playbook: body = json::parse(reply.playbook_result); break;
    case TradingCommand::Kind::PreviewStock: {
      if (!reply.stock_preview) return api_error(503, "TRADING_UNAVAILABLE", "No share preview available");
      const auto& p = *reply.stock_preview;
      body = {{"account_version", std::to_string(s.account_version)}, {"simulated", true},
              {"decision", p.decision.ok() ? "ok" : to_string(p.decision.code)}, {"reason", decision_json(p.decision)},
              {"price", money(p.price)}, {"cost", p.price ? json((*p.price * command.quantity).str()) : json(nullptr)},
              {"current", what_if_account_json(p.current, view.config.scenarios)},
              {"after", what_if_account_json(p.after, view.config.scenarios)}};
      break;
    }
    case TradingCommand::Kind::PreviewClose:
      if (!reply.flatten) return api_error(503, "TRADING_UNAVAILABLE", "No flatten preview available");
      body = flatten_preview_json(*reply.flatten, view);
      body["account_version"] = std::to_string(s.account_version);
      break;
    case TradingCommand::Kind::WhatIf: {
      if (!reply.what_if) return api_error(503, "TRADING_UNAVAILABLE", "No what-if available");
      body = what_if_json(*reply.what_if, command.candidate_names, view.config.scenarios);
      body["account_version"] = std::to_string(s.account_version);
      break;
    }
    case TradingCommand::Kind::Preview:
    case TradingCommand::Kind::PreviewChange:
      if (!reply.preview) return api_error(503, "TRADING_UNAVAILABLE", "No preview available");
      body = preview_json(*reply.preview);
      body["account_version"] = std::to_string(s.account_version);
      break;
    case TradingCommand::Kind::Guardrails:
    case TradingCommand::Kind::Limits: body = risk_json(view); break;
    case TradingCommand::Kind::Trip:
    case TradingCommand::Kind::Reset:
      body["kill"] = kill_json(s); body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      break;
    case TradingCommand::Kind::Settle: body["position_closed"] = true; break;
    case TradingCommand::Kind::ResetAccount:
    case TradingCommand::Kind::Payout: body = account_json(view); break;
    case TradingCommand::Kind::Exercise:
    case TradingCommand::Kind::CloseStock:
    case TradingCommand::Kind::TradeStock:
    case TradingCommand::Kind::Abandon:
    case TradingCommand::Kind::ExerciseInstruction: body = portfolio_json(view); break;
    case TradingCommand::Kind::DayNote: {
      const auto key = md::format_date(command.day);
      body["day"] = key;
      body["note"] = day_notes_json(s).value(key, json{{"plan", ""}, {"review", ""}, {"time", nullptr}});
      break;
    }
    case TradingCommand::Kind::Group:
    case TradingCommand::Kind::Ungroup: {
      // The trades the round trips are in now.
      json trades = json::object();
      for (const auto& t : lifecycles(s.recent_fills, s.closures, view.contracts))
        if (std::find(command.trades.begin(), command.trades.end(), t.first_fill) != command.trades.end())
          trades[std::to_string(t.first_fill)] = trade_group(t, s.groups);
      body["groups"] = trades;
      break;
    }
    case TradingCommand::Kind::Annotate: {
      const auto key = (command.shares ? "s" : "") + std::to_string(command.trade);
      const auto a = s.annotations.find(key);
      body["trade"] = key;
      body["note"] = a == s.annotations.end() ? std::string{} : a->second.note;
      body["tags"] = a == s.annotations.end() ? json::array() : json(a->second.tags);
      break;
    }
    case TradingCommand::Kind::CreateAlert: {
      const auto it = std::find_if(s.alerts.begin(), s.alerts.end(), [&](const Alert& a) { return a.id == reply.alert_id; });
      if (it == s.alerts.end()) return api_error(503, "TRADING_UNAVAILABLE", "Alert publication missing");
      status = 201;
      body["alert"] = alert_json(*it);
      break;
    }
    case TradingCommand::Kind::DeleteAlert: body["deleted"] = std::to_string(command.alert_id); break;
    case TradingCommand::Kind::CreateSandbox:
    case TradingCommand::Kind::CreateAccount:
      status = 201;
      body = {{"account", {{"id", reply.account}, {"name", command.name},
                           {"account_version", std::to_string(s.account_version)},
                           {"plan", nullable(view.config.rules.plan)}, {"plan_id", nullable(preset_id(view.config.initial_cash, view.config.rules))},
                           {"equity", s.equity.str()}}}};
      break;
  }
  return {status, body.dump()};
}

void fields(const json& object, std::initializer_list<std::string_view> required,
             std::initializer_list<std::string_view> optional = {}) {
  if (!object.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (auto field : required) if (!object.contains(field)) throw std::invalid_argument("Missing field: " + std::string(field));
  for (auto it = object.begin(); it != object.end(); ++it)
    if (std::find(required.begin(), required.end(), it.key()) == required.end() &&
        std::find(optional.begin(), optional.end(), it.key()) == optional.end())
      throw std::invalid_argument("Unknown field: " + it.key());
}
std::string string_field(const json& j, const char* key) {
  if (!j.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
  return j.at(key).get<std::string>();
}
std::int64_t integer_field(const json& j, const char* key) {
  const auto& value = j.at(key);
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::uint64_t(INT64_MAX)))
    throw std::invalid_argument(std::string(key) + " must be a signed 64-bit integer");
  return value.get<std::int64_t>();
}
double number_field(const json& j, const char* key) {
  if (!j.at(key).is_number()) throw std::invalid_argument(std::string(key) + " must be a number");
  const double value = j.at(key).get<double>();
  if (!std::isfinite(value)) throw std::invalid_argument(std::string(key) + " must be finite");
  return value;
}
std::uint64_t identifier(std::string_view text) {
  std::uint64_t id = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("Expected an unsigned decimal ID string");
  return id;
}
Money decimal_field(const json& j, const char* key) {
  try { return Money::parse(string_field(j, key)); }
  catch (const TradingError& e) { throw std::invalid_argument(std::string(key) + ": " + e.what()); }
}
std::string symbol_field(const json& j) {
  const auto symbol = string_field(j, "symbol");
  const auto contract = md::parse_osi(symbol);
  if (!contract || contract->osi_symbol() != symbol) throw std::invalid_argument("symbol must be a canonical padded OSI");
  return symbol;
}
ExposureLimits parse_exposure(const json& j) {
  fields(j, {"dollar_delta", "vega"});
  return {number_field(j, "dollar_delta"), number_field(j, "vega")};
}
Limits parse_limits(const json& j) {
  fields(j, {"max_order_contracts", "price_band_absolute", "price_band_relative", "aggregate", "per_underlying",
             "max_daily_loss", "max_quote_age_seconds", "max_valuation_age_seconds"}, {"underlying_overrides"});
  Limits limits;
  limits.max_order_contracts = integer_field(j, "max_order_contracts");
  limits.price_band_absolute = decimal_field(j, "price_band_absolute");
  limits.price_band_relative = number_field(j, "price_band_relative");
  limits.aggregate = parse_exposure(j.at("aggregate"));
  limits.per_underlying = parse_exposure(j.at("per_underlying"));
  if (j.contains("underlying_overrides")) {
    const auto& overrides = j.at("underlying_overrides");
    if (!overrides.is_object()) throw std::invalid_argument("underlying_overrides must be an object");
    for (const auto& [symbol, caps] : overrides.items()) {
      if (symbol.empty() || symbol.size() > 16 || !std::all_of(symbol.begin(), symbol.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.';
          })) throw std::invalid_argument("Override underlying must be 1-16 uppercase letters, digits or dots");
      limits.underlying_overrides[symbol] = parse_exposure(caps);
    }
  }
  limits.max_daily_loss = decimal_field(j, "max_daily_loss");
  auto age = [&](const char* key) {
    const auto seconds = integer_field(j, key);
    if (seconds > INT64_MAX / md::kNanosPerSecond || seconds < 0)
      throw TradingError(Reason::INVALID_LIMITS, "Age must be nonnegative and representable in nanoseconds");
    return seconds * md::kNanosPerSecond;
  };
  limits.max_quote_age = age("max_quote_age_seconds");
  limits.max_valuation_age = age("max_valuation_age_seconds");
  return limits;
}
Trigger parse_trigger(const json& j) {
  const auto source = j.is_object() && j.contains("source") && j.at("source").is_string() ? j.at("source").get<std::string>() : "";
  Trigger t;
  if (source == "time") {
    // A time of day: from that New York minute on, or up to it with at_or_below.
    fields(j, {"source", "at"}, {"direction"});
    const auto at = string_field(j, "at");
    const auto digit = [&](std::size_t i, char high) { return at[i] >= '0' && at[i] <= high; };
    if (at.size() != 5 || at[2] != ':' || !digit(0, '2') || !digit(1, '9') || !digit(3, '5') || !digit(4, '9') ||
        (at[0] - '0') * 10 + (at[1] - '0') > 23)
      throw std::invalid_argument("trigger at must be HH:MM New York time");
    t.source = TriggerSource::Time;
    t.minute = ((at[0] - '0') * 10 + (at[1] - '0')) * 60 + (at[3] - '0') * 10 + (at[4] - '0');
    t.direction = TriggerDirection::AtOrAbove;
  } else {
    fields(j, {"source", "direction", "level"}, source == "study" ? std::initializer_list<std::string_view>{"symbol", "study"}
                                               : source == "underlying" ? std::initializer_list<std::string_view>{"symbol", "reference", "trail"}
                                                                        : std::initializer_list<std::string_view>{"reference", "trail"});
    if (source != "option" && source != "underlying" && source != "combo" && source != "study")
      throw std::invalid_argument("trigger source must be option, combo, underlying, study or time");
    t.source = source == "option" ? TriggerSource::Option : source == "combo" ? TriggerSource::Combo
             : source == "study" ? TriggerSource::Study : TriggerSource::Underlying;
    t.level = decimal_field(j, "level");
    if (j.contains("symbol")) t.symbol = string_field(j, "symbol");
    if (source == "study") {
      if (!j.contains("study")) throw std::invalid_argument("Missing field: study");
      t.study = string_field(j, "study");
    }
  }
  if (j.contains("direction")) {
    const auto direction = string_field(j, "direction");
    if (direction != "at_or_below" && direction != "at_or_above")
      throw std::invalid_argument("trigger direction must be at_or_below or at_or_above");
    t.direction = direction == "at_or_below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove;
  }
  if (j.contains("symbol") && (t.symbol.empty() || t.symbol.size() > 12 || !std::all_of(t.symbol.begin(), t.symbol.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
      })))
    throw std::invalid_argument("trigger symbol must be an underlying of 1-12 uppercase letters, digits, dots or underscores");
  if (t.source == TriggerSource::Study &&
      std::find(std::begin(kTriggerStudies), std::end(kTriggerStudies), t.study) == std::end(kTriggerStudies))
    throw std::invalid_argument("trigger study must be iv30, iv7 or term_ratio");
  // A reference and a trail read the order's own market, not another underlying.
  if ((j.contains("reference") || j.contains("trail")) && conditional(t))
    throw std::invalid_argument("A trigger on another underlying takes no reference or trail");
  if (j.contains("reference")) {
    const auto reference = string_field(j, "reference");
    if (reference != "bid_ask" && reference != "mid" && reference != "mark")
      throw std::invalid_argument("trigger reference must be bid_ask, mid or mark");
    if (reference != "bid_ask" && t.source == TriggerSource::Underlying)
      throw std::invalid_argument("An underlying trigger reads spot; only option and combo triggers take mid or mark");
    t.reference = reference == "mid" ? TriggerReference::Mid : reference == "mark" ? TriggerReference::Mark : TriggerReference::BidAsk;
  }
  if (j.contains("trail")) {
    const auto& spec = j.at("trail");
    fields(spec, {"unit", "value"});
    const auto unit = string_field(spec, "unit");
    Trail trail;
    if (unit == "ticks") {
      if (t.source == TriggerSource::Underlying) throw std::invalid_argument("An underlying trigger trails by amount or percent");
      const auto ticks = integer_field(spec, "value");
      if (ticks < 1 || ticks > 1000) throw std::invalid_argument("A trail is 1 to 1,000 ticks");
      trail = {TrailUnit::Ticks, Money::from_micros(ticks * 1'000'000)};
    } else if (unit == "amount" || unit == "percent") {
      trail = {unit == "amount" ? TrailUnit::Amount : TrailUnit::Percent, decimal_field(spec, "value")};
      if (trail.value <= Money{} || (trail.unit == TrailUnit::Percent && trail.value >= Money::from_micros(100'000'000)))
        throw std::invalid_argument("A trail amount is positive, and a percentage above 0 and below 100");
    } else {
      throw std::invalid_argument("trail unit must be amount, percent or ticks");
    }
    t.trail = trail;
  }
  return t;
}
ExitSpec parse_exit(const json& j) {
  fields(j, {}, {"trigger", "limit_price"});
  ExitSpec exit;
  if (j.contains("trigger")) exit.trigger = parse_trigger(j.at("trigger"));
  if (j.contains("limit_price")) exit.limit_price = decimal_field(j, "limit_price");
  if (!exit.trigger && !exit.limit_price)
    throw std::invalid_argument("A bracket exit takes a trigger, a limit_price, or both for a stop-limit");
  return exit;
}
bool boolean_field(const json& j, const char* key) {
  if (!j.at(key).is_boolean()) throw std::invalid_argument(std::string(key) + " must be a boolean");
  return j.at(key).get<bool>();
}
PayoutRules parse_payout_rules(const json& j) {
  fields(j, {"qualifying_profit", "qualifying_days", "withdrawal_percent", "split_percent", "minimum", "caps"},
         {"consistency_percents", "buffer", "buffer_payouts"});
  PayoutRules p;
  p.qualifying_profit = decimal_field(j, "qualifying_profit");
  p.qualifying_days = integer_field(j, "qualifying_days");
  if (p.qualifying_days < 1) throw std::invalid_argument("qualifying_days must be at least 1");
  p.withdrawal_percent = integer_field(j, "withdrawal_percent");
  p.split_percent = integer_field(j, "split_percent");
  p.minimum = decimal_field(j, "minimum");
  if (j.contains("buffer")) p.buffer = decimal_field(j, "buffer");
  if (j.contains("buffer_payouts")) p.buffer_payouts = integer_field(j, "buffer_payouts");
  if (j.contains("consistency_percents")) {
    const auto& percents = j.at("consistency_percents");
    if (!percents.is_array() || percents.size() > 64)
      throw std::invalid_argument("consistency_percents must be an array of at most 64 whole percentages");
    for (const auto& percent : percents) p.consistency_percents.push_back(integer_field(json{{"consistency_percents", percent}}, "consistency_percents"));
  }
  const auto& caps = j.at("caps");
  if (!caps.is_array() || caps.size() > 64) throw std::invalid_argument("caps must be an array of at most 64 amounts");
  for (const auto& cap : caps) {
    if (!cap.is_string()) throw std::invalid_argument("caps must be decimal strings");
    try { p.caps.push_back(Money::parse(cap.get<std::string>())); }
    catch (const TradingError& e) { throw std::invalid_argument(std::string("caps: ") + e.what()); }
  }
  return p;
}
/// One of a rule's named choices; anything else is a malformed request.
template <class E, std::size_t N>
E choice_field(const json& j, const char* key, const char* const (&names)[N]) {
  const auto value = string_field(j, key);
  for (std::size_t i = 0; i < N; ++i)
    if (value == names[i]) return static_cast<E>(i);
  std::string list;
  for (std::size_t i = 0; i < N; ++i) list += (i == 0 ? "" : i + 1 == N ? " or " : ", ") + std::string(names[i]);
  throw std::invalid_argument(std::string(key) + " must be " + list);
}
/// "HH:MM", 00:00 to 24:00, as minutes after midnight.
std::int64_t clock_field(const json& j, const char* key) {
  const auto text = string_field(j, key);
  const auto digit = [&](std::size_t i) { return text[i] >= '0' && text[i] <= '9'; };
  if (text.size() != 5 || text[2] != ':' || !digit(0) || !digit(1) || !digit(3) || !digit(4))
    throw std::invalid_argument(std::string(key) + " must be HH:MM New York time");
  const auto hours = (text[0] - '0') * 10 + (text[1] - '0'), minutes = (text[3] - '0') * 10 + (text[4] - '0');
  if (minutes > 59 || hours > 24 || (hours == 24 && minutes != 0))
    throw std::invalid_argument(std::string(key) + " must be HH:MM New York time");
  return hours * 60 + minutes;
}
/// Each omitted fee amount is zero. Null on the enclosing rules selects flat fees.
FeeSchedule parse_fee_schedule(const json& j) {
  fields(j, {}, {"open", "close", "leg_cap", "clearing", "regulatory", "index", "exercise"});
  FeeSchedule f;
  const auto amount = [&](const char* key, Money& value) { if (j.contains(key)) value = decimal_field(j, key); };
  amount("open", f.open); amount("close", f.close); amount("leg_cap", f.leg_cap); amount("clearing", f.clearing);
  amount("regulatory", f.regulatory); amount("exercise", f.exercise);
  if (j.contains("index")) {
    const auto& index = j.at("index");
    if (!index.is_object()) throw std::invalid_argument("index must map option roots to decimal strings");
    for (auto it = index.begin(); it != index.end(); ++it) f.index[it.key()] = decimal_field(index, it.key().c_str());
  }
  return f;
}
/// The margin settings present in `j`, in rules or beside a plan: the margin
/// mode, account type, house margin and portfolio margin's vol shock.
void margin_fields(const json& j, AccountRules& rules) try {
  if (j.contains("margin")) {
    const auto margin = string_field(j, "margin");
    if (margin != "strategy" && margin != "portfolio") throw std::invalid_argument("margin must be strategy or portfolio");
    rules.margin = margin == "portfolio" ? MarginMode::Portfolio : MarginMode::Strategy;
  }
  if (j.contains("account_type")) {
    const auto type = string_field(j, "account_type");
    if (type != "margin" && type != "cash" && type != "ira") throw std::invalid_argument("account_type must be margin, cash or ira");
    rules.account_type = type == "cash" ? AccountType::Cash : type == "ira" ? AccountType::Ira : AccountType::Margin;
  }
  if (j.contains("house_margin_percent")) rules.house_margin_percent = integer_field(j, "house_margin_percent");
  if (j.contains("pm_vol_shock")) rules.pm_vol_shock = integer_field(j, "pm_vol_shock");
} catch (const std::exception& error) {
  throw TradingError(Reason::INVALID_RULES, error.what());
}
/// New restriction values use 422 without changing legacy rule error statuses.
struct PlanRestrictionError : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};
void parse_time_rules(const json& j, AccountRules& rules) try {
  if (j.contains("time_limit_days")) rules.time_limit_days = integer_field(j, "time_limit_days");
  if (j.contains("inactivity_days")) rules.inactivity_days = integer_field(j, "inactivity_days");
  if (j.contains("underlyings")) {
    const auto& symbols = j.at("underlyings");
    if (!symbols.is_array() || !std::all_of(symbols.begin(), symbols.end(), [](const json& symbol) { return symbol.is_string(); }))
      throw std::invalid_argument("underlyings must be an array of uppercase symbols");
    rules.underlyings = symbols.get<std::vector<std::string>>();
  }
  if (j.contains("trading_start") && !j.at("trading_start").is_null()) rules.trading_start = clock_field(j, "trading_start");
  if (j.contains("trading_end") && !j.at("trading_end").is_null()) rules.trading_end = clock_field(j, "trading_end");
  if (j.contains("flat_time") && !j.at("flat_time").is_null()) {
    try { rules.flat_time = clock_field(j, "flat_time"); }
    catch (const std::exception&) {
      throw std::invalid_argument("flat_time must be HH:MM New York time from 00:00 to 23:59, before day_end");
    }
  }
  if (j.contains("no_overnight")) rules.no_overnight = boolean_field(j, "no_overnight");
  if (j.contains("events")) rules.events = parse_calendar_events(j.at("events"));
  if (j.contains("news_before_minutes")) rules.news_before_minutes = integer_field(j, "news_before_minutes");
  if (j.contains("news_after_minutes")) rules.news_after_minutes = integer_field(j, "news_after_minutes");
  if (j.contains("news_action")) rules.news_action = string_field(j, "news_action");
  if (j.contains("hold_cutoff")) rules.hold_cutoff = clock_field(j, "hold_cutoff");
  if (j.contains("hold_restrictions")) {
    const auto& list = j.at("hold_restrictions");
    if (!list.is_array() || !std::all_of(list.begin(), list.end(), [](const json& v) { return v.is_string(); }))
      throw std::invalid_argument("hold_restrictions must be an array of restriction names");
    rules.hold_restrictions = list.get<std::vector<std::string>>();
  }
  normalize_event_rules(rules);
  validate_time_rules(rules);
} catch (const std::exception& error) {
  throw PlanRestrictionError(error.what());
}
/// Cost overrides are bookkeeping terms, independent of preset identity.
void program_cost_fields(const json& j, AccountRules& rules) {
  if (j.contains("evaluation_fee")) rules.evaluation_fee = decimal_field(j, "evaluation_fee");
  if (j.contains("reset_fee")) rules.reset_fee = decimal_field(j, "reset_fee");
  if (j.contains("activation_fee")) rules.activation_fee = decimal_field(j, "activation_fee");
  if (j.contains("max_resets")) rules.max_resets = integer_field(j, "max_resets");
}
/// Custom rules: nullable money for an absent target/drawdown, like rules_json.
/// The phase defaults to evaluation; a funded phase requires payout rules.
AccountRules parse_rules(const json& j) {
  fields(j, {"profit_target", "max_drawdown", "drawdown_mode", "buy_only", "buying_power", "expiry_cutoff_seconds"},
         {"plan", "plan_id", "phase", "lock_balance", "payouts", "defined_risk", "slippage_ticks", "margin", "fill_latency_ms", "impact_ticks",
          "lock_at_start", "profit_basis", "daily_loss_limit", "daily_loss_basis", "daily_loss_action", "consistency_percent",
          "consistency_basis", "min_trading_days", "min_profitable_days", "profitable_day_profit", "day_end", "fees",
          "account_type", "house_margin_percent", "pm_vol_shock", "inside_fill_percent",
          "min_hold_seconds", "microscalp_seconds", "microscalp_percent", "min_trades", "trade_consistency_percent", "max_volume_percent", "max_contracts_held", "no_hedging", "no_counter_positions", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent",
          "time_limit_days", "inactivity_days", "underlyings", "trading_start", "trading_end", "flat_time", "no_overnight", "scaling", "size_scaling",
          "events", "news_before_minutes", "news_after_minutes", "news_action", "hold_restrictions", "hold_cutoff",
          "evaluation_fee", "reset_fee", "activation_fee", "max_resets"});

  if (j.contains("microscalp_seconds") != j.contains("microscalp_percent"))
    throw TradingError(Reason::INVALID_RULES, "Set microscalp_seconds and microscalp_percent together");
  AccountRules rules;
  // Accept read-back rules in a custom request, but always derive the identity.
  if (j.contains("plan_id") && !j.at("plan_id").is_null() && !j.at("plan_id").is_string())
    throw std::invalid_argument("plan_id must be a string or null");
  if (j.contains("phase")) {
    const auto phase = string_field(j, "phase");
    if (phase != "evaluation" && phase != "funded" && phase != "verification") throw std::invalid_argument("phase must be evaluation, verification or funded");
    rules.phase = phase == "funded" ? Phase::Funded : phase == "verification" ? Phase::Verification : Phase::Evaluation;
  }
  const bool payouts = j.contains("payouts") && !j.at("payouts").is_null();
  if (payouts != (rules.phase == Phase::Funded))
    throw std::invalid_argument("payouts are required for the funded phase and forbidden otherwise");
  if (payouts) rules.payouts = parse_payout_rules(j.at("payouts"));
  if (j.contains("lock_balance") && !j.at("lock_balance").is_null()) rules.lock_balance = decimal_field(j, "lock_balance");
  if (j.contains("plan") && !j.at("plan").is_null()) rules.plan = string_field(j, "plan");
  auto optional_money = [&](const char* key) { return j.at(key).is_null() ? Money{} : decimal_field(j, key); };
  rules.profit_target = optional_money("profit_target");
  rules.max_drawdown = optional_money("max_drawdown");
  rules.drawdown_mode = choice_field<DrawdownMode>(j, "drawdown_mode", kDrawdownModes);
  // The evaluation rules added later are optional, with their defaults off.
  const auto has = [&](const char* key) { return j.contains(key) && !j.at(key).is_null(); };
  if (has("lock_at_start")) rules.lock_at_start = boolean_field(j, "lock_at_start");
  if (has("profit_basis")) rules.profit_basis = choice_field<ProfitBasis>(j, "profit_basis", kProfitBases);
  if (has("daily_loss_limit")) rules.daily_loss_limit = decimal_field(j, "daily_loss_limit");
  if (has("daily_loss_basis")) rules.daily_loss_basis = choice_field<DailyLossBasis>(j, "daily_loss_basis", kDailyLossBases);
  if (has("daily_loss_action")) rules.daily_loss_action = choice_field<BreachAction>(j, "daily_loss_action", kBreachActions);
  if (has("consistency_percent")) rules.consistency_percent = integer_field(j, "consistency_percent");
  if (has("consistency_basis")) rules.consistency_basis = choice_field<ConsistencyBasis>(j, "consistency_basis", kConsistencyBases);
  if (j.contains("min_hold_seconds")) rules.min_hold_seconds = integer_field(j, "min_hold_seconds");
  if (j.contains("microscalp_seconds")) rules.microscalp_seconds = integer_field(j, "microscalp_seconds");
  if (j.contains("microscalp_percent")) rules.microscalp_percent = integer_field(j, "microscalp_percent");
  if (j.contains("min_trades")) rules.min_trades = integer_field(j, "min_trades");
  if (j.contains("trade_consistency_percent")) rules.trade_consistency_percent = integer_field(j, "trade_consistency_percent");
  if (has("min_trading_days")) rules.min_trading_days = integer_field(j, "min_trading_days");
  if (has("min_profitable_days")) rules.min_profitable_days = integer_field(j, "min_profitable_days");
  if (has("profitable_day_profit")) rules.profitable_day_profit = decimal_field(j, "profitable_day_profit");
  if (has("day_end")) rules.day_end_minutes = clock_field(j, "day_end");
  if (j.contains("max_contracts_held")) rules.max_contracts_held = integer_field(j, "max_contracts_held");
  if (j.contains("max_volume_percent")) rules.max_volume_percent = integer_field(j, "max_volume_percent");
  if (j.contains("no_hedging")) rules.no_hedging = boolean_field(j, "no_hedging");
  if (j.contains("no_counter_positions")) rules.no_counter_positions = boolean_field(j, "no_counter_positions");
  if (j.contains("require_stop_loss")) rules.require_stop_loss = boolean_field(j, "require_stop_loss");
  if (has("max_trade_risk")) rules.max_trade_risk = decimal_field(j, "max_trade_risk");
  if (j.contains("max_trade_risk_percent")) rules.max_trade_risk_percent = integer_field(j, "max_trade_risk_percent");
  parse_time_rules(j, rules);
  if (j.contains("size_scaling") && !j.at("size_scaling").is_null()) {
    const auto& v = j.at("size_scaling");
    fields(v, {"profit_percent", "payouts", "days", "increase_percent", "max_balance"});
    rules.size_scaling = SizeScaling{integer_field(v, "profit_percent"), integer_field(v, "payouts"),
        integer_field(v, "days"), integer_field(v, "increase_percent"), decimal_field(v, "max_balance")};
  }
  if (j.contains("scaling")) {
    if (!j.at("scaling").is_array()) throw std::invalid_argument("scaling must be an array");
    for (const auto& step : j.at("scaling")) {
      fields(step, {"profit", "contracts"}, {});
      rules.scaling.push_back({decimal_field(step, "profit"), integer_field(step, "contracts")});
    }
  }
  rules.buy_only = boolean_field(j, "buy_only");
  if (j.contains("defined_risk")) rules.defined_risk = boolean_field(j, "defined_risk");
  if (j.contains("slippage_ticks")) rules.slippage_ticks = integer_field(j, "slippage_ticks");
  if (j.contains("fill_latency_ms")) rules.fill_latency_ms = integer_field(j, "fill_latency_ms");
  if (j.contains("impact_ticks")) rules.impact_ticks = integer_field(j, "impact_ticks");
  if (j.contains("inside_fill_percent")) rules.inside_fill_percent = integer_field(j, "inside_fill_percent");
  if (j.contains("fees") && !j.at("fees").is_null()) rules.fees = parse_fee_schedule(j.at("fees"));
  margin_fields(j, rules);
  program_cost_fields(j, rules);
  rules.buying_power = boolean_field(j, "buying_power");
  // Out of range is a rule error (INVALID_RULES from validate_rules), not a malformed
  // request; saturate first so the conversion to nanoseconds cannot overflow.
  rules.expiry_cutoff = std::clamp<std::int64_t>(integer_field(j, "expiry_cutoff_seconds"), -1, 86'400) * md::kNanosPerSecond;
  validate_rules(rules);
  return rules;
}
/// A preset's name belongs to its own balance and rules, so an attempt recorded under
/// it (and the funded plan it unlocks) is that preset's; the fill model may differ.
void check_plan_name(Money initial_cash, const AccountRules& rules) {
  if (const auto* preset = find_plan_named(rules.plan); preset && !follows_plan(*preset, initial_cash, rules))
    throw TradingError(Reason::INVALID_RULES, "\"" + rules.plan + "\" is a preset's name: reset with plan " + preset->id +
                       ", or give these rules another name");
}
/// The account's margin, as a broker sets it, can stand beside a plan or custom
/// rules, as the fill model does; the evaluation rules stay intact.
void margin_model(const json& body, AccountRules& rules) {
  margin_fields(body, rules);
  validate_rules(rules);
}
/// Presets override only execution settings; all evaluation rules stay intact.
void fill_model(const json& body, AccountRules& rules) {
  if (!body.contains("fill_model")) return;
  const auto model = string_field(body, "fill_model");
  if (model != "as_displayed" && model != "conservative" && model != "midpoint")
    throw std::invalid_argument("fill_model must be as_displayed, conservative or midpoint");
  rules.fill_latency_ms = model == "conservative" ? 1000 : 0;
  rules.impact_ticks = model == "conservative" ? 1 : 0;
  rules.slippage_ticks = model == "conservative" ? 1 : 0;
  rules.inside_fill_percent = model == "midpoint" ? 50 : 0;
}
/// An illustrative broker schedule; custom rules can supply each amount instead.
void fee_model(const json& body, AccountRules& rules) {
  if (!body.contains("fee_model")) return;
  const auto model = string_field(body, "fee_model");
  if (model != "flat" && model != "itemized") throw std::invalid_argument("fee_model must be flat or itemized");
  rules.fees.reset();
  if (model == "itemized")
    rules.fees = FeeSchedule{Money::parse("1.00"), {}, Money::parse("10.00"), Money::parse("0.10"), Money::parse("0.02"),
                            {{"SPX", Money::parse("0.60")}, {"SPXW", Money::parse("0.60")}}, Money::parse("5.00")};
}
json strict_json(const std::string& body) {
  // JSON parsers normally keep the last duplicate key; that is ambiguous for orders.
  std::vector<std::set<std::string>> keys;
  return json::parse(body, [&](int, json::parse_event_t event, json& parsed) {
    if (event == json::parse_event_t::object_start) keys.emplace_back();
    if (event == json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
      throw std::invalid_argument("Duplicate JSON field");
    if (event == json::parse_event_t::object_end) keys.pop_back();
    return true;
  });
}
/// The optional scope of a bulk command: one underlying, or every one.
std::string scope_field(const json& body) {
  fields(body, {}, {"underlying"});
  if (!body.contains("underlying")) return {};
  auto underlying = string_field(body, "underlying");
  if (underlying.empty() || underlying.size() > 16 ||
      !std::all_of(underlying.begin(), underlying.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
    throw std::invalid_argument("underlying must be an uppercase symbol such as SPX");
  return underlying;
}
/// Client order IDs: 1 to 128 bytes of text, none of them a control character.
bool valid_client_order_id(std::string_view id) {
  return !id.empty() && id.size() <= 128 &&
         std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
/// Account IDs are lowercase letters, digits and single hyphens, as their journals are named.
bool valid_account(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' && id.find("--") == std::string_view::npos &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
std::optional<Walk> parse_walk(const json& j) {
  if (j.is_null()) return std::nullopt;
  fields(j, {"step", "seconds", "limit"});
  return Walk{decimal_field(j, "step"), integer_field(j, "seconds"), decimal_field(j, "limit")};
}
/// Orders in a request and its chained orders.
std::size_t chain_length(const OrderRequest& r) {
  std::size_t size = 1;
  for (const auto& next : r.then) size += chain_length(next);
  for (const auto& other : r.oco) size += chain_length(other);
  return size;
}
/// An order body: a single contract (symbol and side) or legs. A body no market
/// could make a valid order is malformed. `depth` counts the orders a chained one
/// (an order's then or oco) hangs on; it takes no client_order_id or exits_only.
OrderRequest parse_order(const json& body, std::size_t depth = 0) {
  if (!body.is_object()) throw std::invalid_argument("An order is an object");
  if (depth >= kMaxChain) throw std::invalid_argument("A chain is at most four orders");
  const bool chained = depth > 0;
  // A single contract (symbol and side), or legs for a multi-leg order.
  const bool legs = body.contains("legs");
  // A chained order takes its ID from the order it hangs on, and is never a held exit.
  if (chained && legs) fields(body, {"legs", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "group", "good_till", "walk", "then", "oco"});
  else if (chained) fields(body, {"symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "group", "good_till", "walk", "then", "oco"});
  else if (legs) fields(body, {"client_order_id", "legs", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "exits_only", "group", "good_till", "walk", "then", "oco"});
  else fields(body, {"client_order_id", "symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "exits_only", "group", "good_till", "walk", "then", "oco"});
  // A body no market could make a valid order is malformed: 400, and nothing is
  // recorded, so its client_order_id stays free. The reducer's own checks (422,
  // recorded) are those that depend on the account and the market.
  OrderRequest order;
  if (!chained) {
    order.client_order_id = string_field(body, "client_order_id");
    if (!valid_client_order_id(order.client_order_id))
      throw std::invalid_argument("client_order_id must be 1 to 128 bytes of text without control characters");
  }
  const auto type = string_field(body, "type"), tif = string_field(body, "time_in_force");
  constexpr std::pair<std::string_view, TimeInForce> tifs[] = {{"day", TimeInForce::Day}, {"ioc", TimeInForce::Ioc},
      {"gtc", TimeInForce::Gtc}, {"exto", TimeInForce::Exto}, {"gtc_exto", TimeInForce::GtcExto}, {"gtd", TimeInForce::Gtd}};
  const auto named = std::find_if(std::begin(tifs), std::end(tifs), [&](const auto& t) { return t.first == tif; });
  if ((type != "limit" && type != "market") || named == std::end(tifs))
    throw std::invalid_argument("Invalid type or time_in_force");
  order.type = type == "limit" ? OrderType::Limit : OrderType::Market;
  order.tif = named->second;
  if (order.type == OrderType::Market && order.tif != TimeInForce::Ioc &&
      !(body.contains("trigger") && (order.tif == TimeInForce::Exto || order.tif == TimeInForce::GtcExto || order.tif == TimeInForce::Gtd)))
    throw std::invalid_argument("Market orders need ioc; triggered markets also take exto, gtc_exto or gtd");
  if ((order.tif == TimeInForce::Gtd) != body.contains("good_till"))
    throw std::invalid_argument("good_till is required for gtd and forbidden otherwise");
  if (body.contains("good_till")) {
    const auto value = string_field(body, "good_till");
    const bool zoned = value.ends_with("Z") || (value.size() >= 6 && (value[value.size() - 6] == '+' || value[value.size() - 6] == '-'));
    const auto parsed = md::parse_datetime(value, md::Zone::Utc);
    if (!zoned || !parsed) throw std::invalid_argument("good_till must be an ISO date-time with Z or a UTC offset");
    order.good_till = *parsed;
  }
  order.quantity = integer_field(body, "quantity");
  if (order.quantity < 1) throw std::invalid_argument("quantity must be a positive whole number of contracts or units");
  if ((order.type == OrderType::Limit) != body.contains("limit_price"))
    throw std::invalid_argument("limit_price is required for limit orders and forbidden for market orders");
  if (body.contains("limit_price")) order.limit_price = decimal_field(body, "limit_price");
  if (legs) {
    const auto& list = body.at("legs");
    if (!list.is_array() || list.size() < 2 || list.size() > kMaxRollLegs)
      throw std::invalid_argument("legs must be an array of two to four legs, or up to eight for a roll");
    std::set<std::string> symbols;
    for (const auto& item : list) {
      fields(item, {"symbol", "side"}, {"ratio"});
      Leg leg;
      leg.symbol = symbol_field(item);
      const auto side = string_field(item, "side");
      if (side != "buy" && side != "sell") throw std::invalid_argument("Leg side must be buy or sell");
      leg.side = side == "buy" ? Side::Buy : Side::Sell;
      if (item.contains("ratio")) leg.ratio = integer_field(item, "ratio");
      if (leg.ratio < 1 || leg.ratio > kMaxRatio) throw std::invalid_argument("A leg's ratio must be 1 to 10");
      if (!symbols.insert(leg.symbol).second) throw std::invalid_argument("Each leg must name a different contract");
      order.legs.push_back(std::move(leg));
    }
  } else {
    if (order.limit_price && *order.limit_price <= Money{})
      throw std::invalid_argument("A single contract's limit_price must be positive");
    order.symbol = symbol_field(body);
    const auto side = string_field(body, "side");
    if (side != "buy" && side != "sell") throw std::invalid_argument("Invalid side, type or time_in_force");
    order.side = side == "buy" ? Side::Buy : Side::Sell;
  }
  if (body.contains("exits_only")) order.exits_only = boolean_field(body, "exits_only");
  if (body.contains("walk")) order.walk = parse_walk(body.at("walk"));
  if (body.contains("group")) {
    // A trade ID: the opening fill of one of the account's round trips.
    order.group = string_field(body, "group");
    (void)identifier(order.group);
  }
  if (body.contains("note")) order.note = string_field(body, "note");
  if (body.contains("tags")) {
    const auto& tags = body.at("tags");
    if (!tags.is_array() || tags.size() > 16) throw std::invalid_argument("tags must be an array of at most 16 strings");
    for (const auto& tag : tags) {
      if (!tag.is_string()) throw std::invalid_argument("tags must be strings");
      order.tags.push_back(tag.get<std::string>());
    }
  }
  if (body.contains("trigger")) order.trigger = parse_trigger(body.at("trigger"));
  if (body.contains("bracket")) {
    const auto& bracket = body.at("bracket");
    fields(bracket, {}, {"stop_loss", "take_profit"});
    order.bracket = Bracket{};
    if (bracket.contains("stop_loss")) order.bracket->stop_loss = parse_exit(bracket.at("stop_loss"));
    if (bracket.contains("take_profit")) order.bracket->take_profit = parse_exit(bracket.at("take_profit"));
    if (!order.bracket->stop_loss && !order.bracket->take_profit)
      throw std::invalid_argument("A bracket needs a stop_loss, a take_profit or both");
  }
  // One order placed when this one fills completely, and one that cancels it.
  for (const char* key : {"then", "oco"}) {
    if (!body.contains(key)) continue;
    auto next = parse_order(body.at(key), depth + 1);
    if (std::string_view(key) == "oco" && !next.oco.empty()) throw std::invalid_argument("An oco order takes no oco of its own");
    (std::string_view(key) == "then" ? order.then : order.oco).push_back(std::move(next));
  }
  if (order.exits_only && (!order.then.empty() || !order.oco.empty())) throw std::invalid_argument("Held exits take no then or oco");
  if (!chained && chain_length(order) > kMaxChain) throw std::invalid_argument("A chain is at most four orders");
  return order;
}
TradingCommand parse_command(const ApiRequest& request, std::string_view path) {
  TradingCommand command;
  if (path.starts_with("/api/accounts/")) {
    command.account = path.substr(std::string_view("/api/accounts/").size());
    if (!valid_account(command.account)) throw std::invalid_argument("Invalid account id");
    if (request.method == "DELETE") {
      if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
      command.kind = TradingCommand::Kind::DeleteAccount;
    } else {
      command.kind = TradingCommand::Kind::UpdateAccount;
      const auto body = strict_json(request.body);
      fields(body, {}, {"name", "archived"});
      if (body.empty()) throw std::invalid_argument("Provide name or archived");
      if (body.contains("name")) {
        command.name = string_field(body, "name");
        if (!valid_account_name(command.name)) throw std::invalid_argument("name must be 1 to 64 characters, none of them a control character");
      }
      if (body.contains("archived")) {
        if (!body.at("archived").is_boolean()) throw std::invalid_argument("archived must be a boolean");
        command.archived = body.at("archived").get<bool>();
      }
    }
    return command;
  }
  if (request.method == "DELETE" && path.starts_with("/api/alerts/")) {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::DeleteAlert;
    command.alert_id = identifier(path.substr(std::string_view("/api/alerts/").size()));
    return command;
  }
  if (request.method == "DELETE") {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::Cancel;
    command.order_id = identifier(path.substr(std::string_view("/api/orders/").size()));
    return command;
  }
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  auto body = strict_json(request.body);
  // POST /api/orders/what-if: candidates, each a list of orders without chains.
  if (path == "/api/orders/what-if") {
    fields(body, {"candidates"});
    command.kind = TradingCommand::Kind::WhatIf;
    const auto& list = body.at("candidates");
    if (!list.is_array() || list.empty() || list.size() > kMaxWhatIfCandidates)
      throw std::invalid_argument("candidates must be an array of one to six candidates");
    for (std::size_t c = 0; c < list.size(); ++c) {
      const auto where = "candidates[" + std::to_string(c) + "]";
      fields(list[c], {"orders"}, {"name"});
      auto name = list[c].contains("name") ? string_field(list[c], "name") : "Candidate " + std::to_string(c + 1);
      if (name.empty() || name.size() > 64) throw std::invalid_argument(where + ".name must be 1 to 64 bytes");
      command.candidate_names.push_back(std::move(name));
      const auto& orders = list[c].at("orders");
      if (!orders.is_array() || orders.empty() || orders.size() > kMaxWhatIfOrders)
        throw std::invalid_argument(where + ".orders must be an array of one to four orders");
      auto& requests = command.candidates.emplace_back();
      for (std::size_t i = 0; i < orders.size(); ++i) {
        // Each order is parsed as a submission parses it; a client ID is optional here.
        auto order = orders[i];
        if (order.is_object() && !order.contains("client_order_id"))
          order["client_order_id"] = "what-if-" + std::to_string(c + 1) + "-" + std::to_string(i + 1);
        ApiRequest single;
        single.method = "POST";
        single.body = order.dump();
        try {
          if (order.contains("then") || order.contains("oco"))
            throw std::invalid_argument("What-if does not model order chains; then and oco are not supported");
          requests.push_back(parse_command(single, "/api/orders").order);
        } catch (const std::invalid_argument& error) {
          throw std::invalid_argument(where + ".orders[" + std::to_string(i) + "]: " + error.what());
        }
      }
    }
    return command;
  }
  // POST /api/orders/{id}/preview: a change's terms, as PUT takes them, and floor_share.
  const bool change_preview = request.method == "POST" && path != "/api/orders/preview" &&
      path.starts_with("/api/orders/") && path.ends_with("/preview");
  if ((request.method == "PUT" && path.starts_with("/api/orders/")) || change_preview) {
    if (change_preview) fields(body, {}, {"quantity", "limit_price", "trigger_level", "time_in_force", "walk", "floor_share"});
    else fields(body, {}, {"quantity", "limit_price", "trigger_level", "time_in_force", "walk"});
    command.kind = change_preview ? TradingCommand::Kind::PreviewChange : TradingCommand::Kind::Modify;
    auto id = path.substr(std::string_view("/api/orders/").size());
    if (change_preview) id = id.substr(0, id.size() - std::string_view("/preview").size());
    command.order_id = identifier(id);
    if (change_preview && body.contains("floor_share")) {
      command.floor_share = number_field(body, "floor_share");
      if (command.floor_share <= 0 || command.floor_share > 1) throw std::invalid_argument("floor_share must be in (0, 1]");
    }
    if (body.contains("quantity")) {
      command.change.quantity = integer_field(body, "quantity");
      if (*command.change.quantity < 1)
        throw std::invalid_argument("quantity must be a positive whole number of contracts or units");
    }
    if (body.contains("limit_price")) command.change.limit_price = decimal_field(body, "limit_price");
    if (body.contains("trigger_level")) command.change.trigger_level = decimal_field(body, "trigger_level");
    if (body.contains("walk")) command.change.walk.emplace(parse_walk(body.at("walk")));
    if (body.contains("time_in_force")) {
      const auto tif = string_field(body, "time_in_force");
      if (tif != "day" && tif != "gtc") throw std::invalid_argument("time_in_force changes to day or gtc");
      command.change.tif = tif == "day" ? TimeInForce::Day : TimeInForce::Gtc;
    }
    if (command.change.empty()) throw std::invalid_argument("Give quantity, limit_price, trigger_level, time_in_force or walk");
    return command;
  }
  if (request.method == "PUT" && path.starts_with("/api/days/") && path.ends_with("/note")) {
    fields(body, {"plan", "review"});
    const auto date = csv_date(path.substr(10, path.size() - 15));
    if (!date) throw std::invalid_argument("Expected a valid YYYY-MM-DD date");
    command.kind = TradingCommand::Kind::DayNote;
    command.day = *date;
    command.plan = string_field(body, "plan");
    command.review = string_field(body, "review");
    return command;
  }
  if (request.method == "POST" && (path == "/api/trades/group" || path == "/api/trades/ungroup")) {
    // The round trips, by trade ID, whose trades join or which leave theirs.
    fields(body, {"trades"});
    command.kind = path == "/api/trades/group" ? TradingCommand::Kind::Group : TradingCommand::Kind::Ungroup;
    const auto& trades = body.at("trades");
    if (!trades.is_array() || trades.empty() || trades.size() > 16)
      throw std::invalid_argument("trades must be an array of 1 to 16 trade IDs");
    for (const auto& id : trades) {
      if (!id.is_string()) throw std::invalid_argument("trades must be trade ID strings");
      command.trades.push_back(identifier(id.get<std::string>()));
    }
    return command;
  }
  if (request.method == "PUT" && path.starts_with("/api/trades/")) {
    // PUT /api/trades/{id}/note: the note and tags replace the trade's.
    fields(body, {}, {"note", "tags"});
    command.kind = TradingCommand::Kind::Annotate;
    auto id = path.substr(std::string_view("/api/trades/").size());
    id = id.substr(0, id.size() - std::string_view("/note").size());
    // Share round trips are named "s" and their opening stock fill.
    command.shares = id.starts_with("s");
    command.trade = identifier(command.shares ? id.substr(1) : id);
    if (body.contains("note")) command.note = string_field(body, "note");
    if (body.contains("tags")) {
      const auto& tags = body.at("tags");
      if (!tags.is_array() || tags.size() > 16) throw std::invalid_argument("tags must be an array of at most 16 strings");
      for (const auto& tag : tags) {
        if (!tag.is_string()) throw std::invalid_argument("tags must be strings");
        command.tags.push_back(tag.get<std::string>());
      }
    }
    return command;
  }
  if (path == "/api/alerts") {
    // POST /api/alerts: what to watch, a direction and an inclusive level.
    fields(body, {"scope", "metric", "direction", "level"}, {"label", "symbol", "legs", "repeat"});
    command.kind = TradingCommand::Kind::CreateAlert;
    auto& c = command.alert.condition;
    const auto scope = string_field(body, "scope");
    constexpr const char* scopes[] = {"contract", "spread", "underlying", "account"};
    const auto found = std::find(std::begin(scopes), std::end(scopes), scope);
    if (found == std::end(scopes)) throw std::invalid_argument("scope must be contract, spread, underlying or account");
    c.scope = static_cast<AlertScope>(found - std::begin(scopes));
    c.metric = string_field(body, "metric");
    const auto direction = string_field(body, "direction");
    if (direction != "at_or_below" && direction != "at_or_above") throw std::invalid_argument("direction must be at_or_below or at_or_above");
    c.direction = direction == "at_or_below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove;
    c.level = decimal_field(body, "level");
    if (body.contains("symbol") && !body.at("symbol").is_null()) c.symbol = string_field(body, "symbol");
    if (body.contains("legs") && !body.at("legs").is_null()) {
      if (!body.at("legs").is_array()) throw std::invalid_argument("legs must be an array");
      for (const auto& item : body.at("legs")) {
        fields(item, {"symbol", "side", "ratio"});
        const auto side = string_field(item, "side");
        if (side != "buy" && side != "sell") throw std::invalid_argument("A leg's side must be buy or sell");
        c.legs.push_back({symbol_field(item), side == "buy" ? Side::Buy : Side::Sell, integer_field(item, "ratio")});
      }
    }
    if (body.contains("label")) command.alert.label = string_field(body, "label");
    if (body.contains("repeat")) command.alert.repeat = boolean_field(body, "repeat");
    // The reducer's own check, so malformed terms are a 400 like any malformed request.
    try { validate_alert(command.alert); } catch (const TradingError& e) { throw std::invalid_argument(e.what()); }
    return command;
  }
  if (path == "/api/positions/exercise") {
    fields(body, {"symbol", "quantity"});
    command.kind = TradingCommand::Kind::Exercise;
    command.symbol = symbol_field(body);
    command.quantity = integer_field(body, "quantity");
    if (command.quantity <= 0) throw std::invalid_argument("quantity must be a positive number of contracts");
    return command;
  }
  if (path == "/api/positions/abandon") {
    fields(body, {"symbol"});
    command.kind = TradingCommand::Kind::Abandon;
    command.symbol = symbol_field(body);
    return command;
  }
  if (path == "/api/positions/instruction") {
    fields(body, {"symbol", "do_not_exercise"});
    command.kind = TradingCommand::Kind::ExerciseInstruction;
    command.symbol = symbol_field(body);
    command.do_not_exercise = boolean_field(body, "do_not_exercise");
    return command;
  }
  if (path == "/api/stocks/trade" || path == "/api/stocks/trade/preview") {
    fields(body, {"symbol", "side", "shares"});
    command.kind = path.ends_with("/preview") ? TradingCommand::Kind::PreviewStock : TradingCommand::Kind::TradeStock;
    command.symbol = string_field(body, "symbol");
    if (command.symbol.empty() || command.symbol.size() > 16 ||
        !std::all_of(command.symbol.begin(), command.symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw std::invalid_argument("symbol must be an uppercase stock symbol such as SPY");
    const auto side = string_field(body, "side");
    if (side != "buy" && side != "sell") throw std::invalid_argument("side must be buy or sell");
    command.quantity = integer_field(body, "shares");
    if (command.quantity <= 0 || command.quantity > 10'000'000)
      throw std::invalid_argument("shares must be a positive whole number, at most 10,000,000");
    if (side == "sell") command.quantity = -command.quantity;
    return command;
  }
  if (path == "/api/stocks/close") {
    fields(body, {"symbol"}, {"shares"});
    command.kind = TradingCommand::Kind::CloseStock;
    command.symbol = string_field(body, "symbol");
    if (command.symbol.empty() || command.symbol.size() > 16 ||
        !std::all_of(command.symbol.begin(), command.symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw std::invalid_argument("symbol must be an uppercase stock symbol such as SPY");
    if (body.contains("shares")) {
      command.quantity = integer_field(body, "shares");
      if (command.quantity <= 0) throw std::invalid_argument("shares must be positive; leave it out to close them all");
    }
    return command;
  }
  if (path == "/api/orders/cancel") {
    command.kind = TradingCommand::Kind::CancelAll;
    // Named orders, such as both exits of a pair, cancel together in one transaction.
    if (body.is_object() && body.contains("orders")) {
      fields(body, {"orders"});
      const auto& list = body.at("orders");
      if (!list.is_array() || list.empty() || list.size() > 16)
        throw std::invalid_argument("orders must be an array of one to sixteen order IDs");
      for (const auto& item : list) {
        if (!item.is_string()) throw std::invalid_argument("orders must be order ID strings");
        const auto id = identifier(item.get<std::string>());
        if (std::find(command.order_ids.begin(), command.order_ids.end(), id) != command.order_ids.end())
          throw std::invalid_argument("orders must not repeat an order ID");
        command.order_ids.push_back(id);
      }
      return command;
    }
    command.underlying = scope_field(body);
    return command;
  }
  if (path == "/api/positions/close" || path == "/api/positions/close/preview") {
    command.kind = path.ends_with("/preview") ? TradingCommand::Kind::PreviewClose : TradingCommand::Kind::ClosePositions;
    fields(body, {}, {"underlying", "type", "limit_ticks"});
    auto scope = body;
    scope.erase("type"); scope.erase("limit_ticks");
    command.underlying = scope_field(scope);
    const auto type = body.contains("type") ? string_field(body, "type") : std::string("market");
    if (type != "market" && type != "limit") throw std::invalid_argument("type must be market or limit");
    command.close_pricing.limit = type == "limit";
    if (body.contains("limit_ticks")) {
      if (!command.close_pricing.limit) throw std::invalid_argument("limit_ticks needs type limit");
      command.close_pricing.limit_ticks = integer_field(body, "limit_ticks");
      if (command.close_pricing.limit_ticks < 0 || command.close_pricing.limit_ticks > 10)
        throw std::invalid_argument("limit_ticks must be 0 to 10");
    }
    return command;
  }
  if (path == "/api/accounts") {
    // A name, and either a preset plan or a starting balance and complete rules.
    fields(body, {"name"}, {"plan", "initial_cash", "rules", "fill_model", "fee_model", "margin", "account_type",
                            "house_margin_percent", "pm_vol_shock", "copy_settings_from", "evaluation_fee", "reset_fee", "activation_fee", "max_resets"});
    command.kind = TradingCommand::Kind::CreateAccount;
    command.name = string_field(body, "name");
    if (body.contains("copy_settings_from")) {
      command.copy_settings_from = string_field(body, "copy_settings_from");
      if (!valid_account(command.copy_settings_from)) throw std::invalid_argument("copy_settings_from must be an account id");
    }
    if (!valid_account_name(command.name))
      throw std::invalid_argument("name must be 1 to 64 characters, none of them a control character");
    if (body.contains("plan")) {
      if (body.contains("initial_cash") || body.contains("rules")) throw std::invalid_argument("plan excludes initial_cash and rules");
      const auto* plan = find_plan(string_field(body, "plan"));
      if (!plan) throw std::invalid_argument("Unknown plan; see GET /api/plans");
      if (!plan->unlocked_by.empty())
        throw std::invalid_argument("A locked plan starts from an account that passed its prerequisite; reset that account instead");
      command.initial_cash = plan->initial_cash;
      command.rules = plan->rules;
    } else {
      if (!body.contains("initial_cash") || !body.contains("rules"))
        throw std::invalid_argument("Provide a plan, or both initial_cash and rules");
      command.initial_cash = decimal_field(body, "initial_cash");
      if (command.initial_cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
      command.rules = parse_rules(body.at("rules"));
      if (command.rules.phase == Phase::Funded)
        throw std::invalid_argument("A funded account starts from an account that passed its evaluation");
      check_plan_name(command.initial_cash, command.rules);
      if (const auto* preset = find_plan_named(command.rules.plan); preset && !preset->unlocked_by.empty())
        throw std::invalid_argument("A locked plan requires a pass; reset the passing account instead");
    }
    program_cost_fields(body, command.rules);
    fill_model(body, command.rules);
    fee_model(body, command.rules);
    margin_model(body, command.rules);
    return command;
  }
  if (path == "/api/orders" || path == "/api/orders/preview") {
    if (path == "/api/orders/preview") {
      command.kind = TradingCommand::Kind::Preview;
      if (body.contains("floor_share")) {
        command.floor_share = number_field(body, "floor_share");
        if (command.floor_share <= 0 || command.floor_share > 1) throw std::invalid_argument("floor_share must be in (0, 1]");
        body.erase("floor_share");
      }
    }
    command.order = parse_order(body);
  } else if (path == "/api/risk/guardrails") {
    fields(body, {"expected_revision", "guardrails"});
    command.kind = TradingCommand::Kind::Guardrails;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    const auto& g = body.at("guardrails");
    fields(g, {"soft_floor", "soft_floor_percent", "max_opening_trades", "cooldown_loss", "cooldown_minutes", "profit_lock"});
    command.guardrails = {decimal_field(g, "soft_floor"), integer_field(g, "soft_floor_percent"), integer_field(g, "max_opening_trades"),
        decimal_field(g, "cooldown_loss"), integer_field(g, "cooldown_minutes"), decimal_field(g, "profit_lock")};
    validate_guardrails(command.guardrails);
  } else if (path == "/api/risk/limits") {
    fields(body, {"expected_revision", "limits"});
    command.kind = TradingCommand::Kind::Limits;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    command.limits = parse_limits(body.at("limits"));
  } else if (path == "/api/risk/kill") {
    fields(body, {"action", "reason"});
    const auto action = string_field(body, "action");
    if (action != "trip" && action != "reset") throw std::invalid_argument("action must be trip or reset");
    command.kind = action == "trip" ? TradingCommand::Kind::Trip : TradingCommand::Kind::Reset;
    command.reason = string_field(body, "reason");
  } else if (path == "/api/account/reset") {
    // Either a preset ID, or a custom starting balance and complete rules.
    fields(body, {"reason"}, {"plan", "initial_cash", "rules", "fill_model", "fee_model", "margin", "account_type",
                              "house_margin_percent", "pm_vol_shock", "evaluation_fee", "reset_fee", "activation_fee", "max_resets"});
    command.kind = TradingCommand::Kind::ResetAccount;
    command.program_costs = true;
    command.restore_scaled_rules = true;
    std::uint64_t cost_bit = 1;
    for (const auto* field : {"evaluation_fee", "reset_fee", "activation_fee", "max_resets"}) {
      if (body.contains(field) || (body.contains("rules") && body.at("rules").contains(field)))
        command.program_cost_overrides |= cost_bit;
      cost_bit <<= 1;
    }
    command.reason = string_field(body, "reason");
    if (body.contains("plan")) {
      if (body.contains("initial_cash") || body.contains("rules"))
        throw std::invalid_argument("plan excludes initial_cash and rules");
      const auto* plan = find_plan(string_field(body, "plan"));
      if (!plan) throw std::invalid_argument("Unknown plan; see GET /api/plans");
      command.initial_cash = plan->initial_cash;
      command.rules = plan->rules;
      if (!plan->unlocked_by.empty()) command.required_pass = find_plan(plan->unlocked_by)->name;
    } else {
      if (!body.contains("initial_cash") || !body.contains("rules"))
        throw std::invalid_argument("Provide a plan, or both initial_cash and rules");
      command.initial_cash = decimal_field(body, "initial_cash");
      if (command.initial_cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
      command.rules = parse_rules(body.at("rules"));
      check_plan_name(command.initial_cash, command.rules);
      if (const auto* preset = find_plan_named(command.rules.plan); preset && !preset->unlocked_by.empty())
        command.required_pass = find_plan(preset->unlocked_by)->name;
    }
    program_cost_fields(body, command.rules);
    fill_model(body, command.rules);
    fee_model(body, command.rules);
    margin_model(body, command.rules);
  } else if (path == "/api/account/payout") {
    fields(body, {"amount"});
    command.kind = TradingCommand::Kind::Payout;
    command.amount = decimal_field(body, "amount");
  } else {
    fields(body, {"symbol", "value"});
    command.kind = TradingCommand::Kind::Settle;
    command.symbol = symbol_field(body);
    command.settlement = decimal_field(body, "value");
  }
  return command;
}
}  // namespace

/// P&L by Greek in dollars, to the micro-dollar as the account's money is. The
/// parts add up to the total exactly: each is its value rounded down or up, the
/// micro-dollars rounding down leaves go to the parts it cut most (largest
/// remainder, the earlier part on a tie).
json attribution_json(const trading::Attribution& a) {
  constexpr double scale = 1'000'000;
  const std::array<double, 6> parts{a.delta, a.gamma, a.vega, a.theta, a.other, a.costs};
  std::array<double, 6> dollars{};
  const double total = std::round(a.total() * scale);
  if (std::all_of(parts.begin(), parts.end(), [](double x) { return std::isfinite(x); }) && std::isfinite(total)) {
    std::array<double, 6> remainders{};
    double left = total;
    for (std::size_t i = 0; i < parts.size(); ++i) {
      dollars[i] = std::floor(parts[i] * scale);
      remainders[i] = parts[i] * scale - dollars[i];
      left -= dollars[i];
    }
    std::array<std::size_t, 6> order{0, 1, 2, 3, 4, 5};
    std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return remainders[x] > remainders[y]; });
    for (std::size_t k = 0; left >= 1; ++k, --left) ++dollars[order[k % order.size()]];
    for (std::size_t k = 0; left <= -1; ++k, ++left) --dollars[order[order.size() - 1 - k % order.size()]];
    for (auto& part : dollars) part /= scale;
  } else {
    for (std::size_t i = 0; i < parts.size(); ++i) dollars[i] = std::round(parts[i] * scale) / scale;
  }
  return {{"delta", number(dollars[0])}, {"gamma", number(dollars[1])}, {"vega", number(dollars[2])},
          {"theta", number(dollars[3])}, {"other", number(dollars[4])}, {"costs", number(dollars[5])},
          {"total", number(total / scale)}, {"fallback", a.fallback}};
}

ApiResponse api_error(int status, std::string code, std::string message, const Decision& evidence) {
  // Some reducer checks identify a single contract. The HTTP contract exposes
  // risk scope as the underlying, while the order itself carries the OSI.
  json error{{"code", code}, {"message", message},
      {"actual", evidence_value(evidence, false)},
      {"limit", evidence_value(evidence, true)}, {"scope", scope_json(evidence.scope)}};
  if (evidence.evidence) error["evidence"] = rule_evidence_json(*evidence.evidence);
  return {status, json{{"error", std::move(error)}}.dump()};
}
json order_request_json(const OrderRequest& order) {
  json body{{"client_order_id", order.client_order_id}, {"type", order.type == OrderType::Limit ? "limit" : "market"},
            {"time_in_force", tif_name(order.tif)}, {"quantity", order.quantity}, {"tags", order.tags}, {"note", order.note}};
  if (multi_leg(order)) {
    body["legs"] = json::array();
    for (const auto& leg : order.legs)
      body["legs"].push_back({{"symbol", leg.symbol}, {"side", side_name(leg.side)}, {"ratio", leg.ratio}});
    if (order.exits_only) body["exits_only"] = true;
  } else {
    body["symbol"] = order.symbol;
    body["side"] = side_name(order.side);
  }
  if (order.limit_price) body["limit_price"] = order.limit_price->str();
  if (order.good_till) body["good_till"] = md::format_timestamp(*order.good_till);
  if (!order.group.empty()) body["group"] = order.group;
  const auto trigger_body = [](const Trigger& trigger) {
    auto result = trigger_json(trigger);
    if (trigger.source == TriggerSource::Time) result.erase("level");
    // A request names only what differs from the defaults.
    if (result.contains("trail") && result["trail"].is_null()) result.erase("trail");
    if (result.value("reference", "") == "bid_ask") result.erase("reference");
    return result;
  };
  if (order.trigger) body["trigger"] = trigger_body(*order.trigger);
  if (order.bracket) {
    body["bracket"] = json::object();
    const auto add_exit = [&](const char* key, const std::optional<ExitSpec>& exit) {
      if (!exit) return;
      json terms = json::object();
      if (exit->trigger) terms["trigger"] = trigger_body(*exit->trigger);
      if (exit->limit_price) terms["limit_price"] = exit->limit_price->str();
      body["bracket"][key] = std::move(terms);
    };
    add_exit("stop_loss", order.bracket->stop_loss);
    add_exit("take_profit", order.bracket->take_profit);
  }
  return body;
}
json trading_status_json(const TradingStatus& status) {
  return {{"enabled", status.enabled}, {"reason", nullable(status.reason)},
          {"account_version", std::to_string(status.account_version)},
          {"kill_latched", status.kill_latched}, {"write", status.write},
          {"fee_per_contract", status.fee_per_contract.str()},
          {"initial_cash", status.initial_cash.str()},
          {"plan", nullable(status.plan)}, {"plan_id", nullable(status.plan_id)}, {"evaluation", nullable(status.evaluation)}};
}
json accounts_json(const MetricsSource& source, const ApiAccess& access, bool archived = false) {
  json list = json::array();
  for (const auto& account : source.status().accounts) {
    if (!sandbox_visible(account, access) || (account.archived && !archived)) continue;
    const auto view = source.trading_view(account.id);
    list.push_back({{"id", account.id}, {"name", account.name}, {"archived", account.archived}, {"trading", trading_status_json(account.trading)},
                    {"equity", view && view->snapshot ? json(view->snapshot->equity.str()) : json(nullptr)}});
    list.back()["damaged"] = damage_json(account.damaged);
    list.back()["journal_size"] = journal_json(account.journal_bytes, account.journal_records, account.replay);
    if (account.sandbox_idle_seconds) list.back()["sandbox_idle_seconds"] = account.sandbox_idle_seconds;
  }
  return list;
}
json account_ticks_json(const EngineStatus& status) {
  json list = json::array();
  for (const auto& account : status.accounts) {
    if (account.archived) continue;
    list.push_back({{"id", account.id}, {"name", account.name}, {"trading", trading_status_json(account.trading)}});
    list.back()["damaged"] = damage_json(account.damaged);
    list.back()["journal_size"] = journal_json(account.journal_bytes, account.journal_records, account.replay);
    if (account.sandbox_idle_seconds) list.back()["sandbox_idle_seconds"] = account.sandbox_idle_seconds;
  }
  return list;
}

std::optional<std::map<std::string, std::string>> query_parameters(std::string_view query, bool plus_is_space) {
  const auto decode = [plus_is_space](std::string_view text) -> std::optional<std::string> {
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '%') { decoded += plus_is_space && text[i] == '+' ? ' ' : text[i]; continue; }
      if (i + 2 >= text.size()) return std::nullopt;
      unsigned int byte = 0;
      const auto* digits = text.data() + i + 1;
      const auto [end, error] = std::from_chars(digits, digits + 2, byte, 16);
      if (error != std::errc{} || end != digits + 2) return std::nullopt;
      decoded += static_cast<char>(byte);
      i += 2;
    }
    return decoded;
  };
  std::map<std::string, std::string> pairs;
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto pair = query.substr(0, amp);
    const auto equals = pair.find('=');
    auto key = decode(pair.substr(0, equals));
    auto value = decode(equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1));
    if (!key || !value || !pairs.emplace(std::move(*key), std::move(*value)).second) return std::nullopt;
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return pairs;
}

std::optional<std::string> query_account(std::string_view target, std::string_view default_account) {
  const auto question = target.find('?');
  const auto pairs = query_parameters(question == std::string_view::npos ? std::string_view{} : target.substr(question + 1));
  if (!pairs) return std::nullopt;
  const auto account = pairs->find("account");
  if (account == pairs->end()) return std::string(default_account.empty() ? kMainAccount : default_account);
  if (!valid_account(account->second)) return std::nullopt;
  return account->second;
}

/// Whether `account` names an account the source publishes (the main one when empty).
bool known_account(const MetricsSource& source, const std::string& account) {
  if (account.empty() || account == kMainAccount) return true;
  const auto accounts = source.status().accounts;
  return std::any_of(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == account; });
}

namespace {
constexpr std::size_t kMaxProfileCurves = 8;
bool underlying_symbol(std::string_view symbol) {
  return !symbol.empty() && symbol.size() <= 16 &&
         std::all_of(symbol.begin(), symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; });
}
std::vector<std::string_view> split_list(std::string_view text) {
  std::vector<std::string_view> items;
  for (std::size_t start = 0;;) {
    const auto comma = text.find(',', start);
    items.push_back(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return items;
}
/// The beta of `underlying` to `reference`, by the first source that has one.
BetaEstimate profile_beta(const MetricsSource& source, const std::string& underlying, const std::string& reference,
                          const std::map<std::string, double>& given) {
  BetaEstimate beta;
  if (underlying == reference) { beta.beta = 1; beta.source = "reference"; return beta; }
  if (const auto it = given.find(underlying); it != given.end()) { beta.beta = it->second; beta.source = "given"; return beta; }
  if (same_index(underlying, reference)) { beta.beta = 1; beta.source = "index"; return beta; }
  const auto* candles = source.candles();
  if (!candles) return beta;
  beta = estimate_beta(candles->daily_history(underlying), candles->daily_history(reference), 252);
  if (!beta.source.empty() && beta.observations >= 20) { beta.source = "daily"; return beta; }
  beta = estimate_beta(candles->bars(underlying, BarInterval::FiveMinutes, 2000),
                       candles->bars(reference, BarInterval::FiveMinutes, 2000), 2000, 5 * md::kNanosPerMinute);
  if (!beta.source.empty() && beta.observations >= 30) { beta.source = "intraday"; return beta; }
  return BetaEstimate{0, beta.correlation, beta.observations, ""};
}
json level_json(const std::optional<BreachLevel>& value, double spot) {
  if (!value) return nullptr;
  return {{"percent", number(value->percent)}, {"points", spot > 0 ? number(value->points) : json(nullptr)},
          {"price", spot > 0 ? number(spot + value->points) : json(nullptr)},
          {"touch_probability", value->touch_probability ? number(*value->touch_probability) : json(nullptr)}};
}

/// GET /api/risk/profile: the held book's value across moves of one underlying, or
/// of a benchmark with every underlying beta-weighted to it, on several dates.
ApiResponse risk_profile_response(const std::map<std::string, std::string>& query, const MetricsSource& source, std::string account) {
  std::string underlying, benchmark = "SPY";
  std::vector<std::string> days_text{"0", "1"};
  double vol_points = 0, range = 10;
  int steps = 41;
  std::map<std::string, double> given;
  bool valid = true;
  for (const auto& [key, value] : query) {
    if (key == "account" && valid_account(value)) account = value;
    else if (key == "underlying" && underlying_symbol(value)) underlying = value;
    else if (key == "benchmark" && (value == "SPY" || value == "SPX")) benchmark = value;
    else if (key == "days") {
      days_text.clear();
      for (const auto item : split_list(value)) days_text.emplace_back(item);
      valid &= days_text.size() <= kMaxProfileCurves;
    } else if (key == "iv") valid &= query_decimal(value, vol_points) && std::abs(vol_points) <= 100;
    else if (key == "range") valid &= query_decimal(value, range) && range > 0 && range <= 99;
    else if (key == "steps") {
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), steps);
      valid &= ec == std::errc{} && end == value.data() + value.size() && steps >= 3 && steps <= 201 && steps % 2 == 1;
    } else if (key == "betas") {
      for (const auto item : split_list(value)) {
        const auto colon = item.find(':');
        double beta = 0;
        const std::string symbol(item.substr(0, colon));
        valid &= colon != std::string_view::npos && underlying_symbol(symbol) && !given.contains(symbol) &&
                 query_decimal(std::string(item.substr(colon + 1)), beta) && std::abs(beta) <= 10;
        if (valid) given[symbol] = beta;
      }
    } else valid = false;
    if (!valid) break;
  }
  if (!valid) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  if (!known_account(source, account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + account);
  const auto view = source.trading_view(account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  const auto& reference = underlying.empty() ? benchmark : underlying;
  // The underlyings in scope: each held contract's and share's.
  std::set<std::string> held;
  Timestamp first_expiry = std::numeric_limits<Timestamp>::max();
  for (const auto& p : s.positions) {
    if (!underlying.empty() && p.position.contract.underlying != underlying) continue;
    held.insert(p.position.contract.underlying);
    if (p.position.contract.expiry_time() > s.time) first_expiry = std::min(first_expiry, p.position.contract.expiry_time());
  }
  for (const auto& stock : s.stocks)
    if (underlying.empty() || stock.position.symbol == underlying) held.insert(stock.position.symbol);
  ProfileConfig config;
  config.vol_points = vol_points;
  config.vol_floor = view->config.scenarios.vol_floor;
  for (int i = 0; i < steps; ++i) {
    const int offset = i - steps / 2;
    config.percent.push_back(offset == 0 ? 0.0 : range * offset / (steps / 2));
  }
  config.days.clear();
  std::vector<std::string> labels;
  for (const auto& text : days_text) {
    double days = 0;
    if (text == "expiry") {
      if (first_expiry == std::numeric_limits<Timestamp>::max()) continue;  // nothing expires in scope
      days = std::min(366.0, static_cast<double>(first_expiry - s.time) / static_cast<double>(md::kNanosPerDay));
    } else if (!query_decimal(text, days) || days < 0 || days > 366) {
      return api_error(400, "INVALID_REQUEST", "days must be numbers of days from 0 to 366, or expiry");
    }
    if (std::find(config.days.begin(), config.days.end(), days) != config.days.end()) continue;
    config.days.push_back(days);
    labels.push_back(text == "expiry" ? "expiry" : "");
  }
  if (config.days.empty()) {
    config.days.push_back(0);
    labels.emplace_back();
  }
  // The reference's spot and implied variances come from its analytics; a held
  // underlying's valuation stands in for a spot it has none of.
  const auto metrics = source.metrics(reference);
  double spot = metrics && metrics->spot > 0 && std::isfinite(metrics->spot) ? metrics->spot : 0.0;
  if (!(spot > 0)) {
    Timestamp latest = -1;
    for (const auto& p : s.positions) {
      const auto v = view->valuations.find(p.position.contract.osi_symbol());
      if (p.position.contract.underlying == reference && v != view->valuations.end() && v->second.time > latest && v->second.spot > 0) {
        spot = v->second.spot; latest = v->second.time;
      }
    }
    for (const auto& stock : s.stocks)
      if (stock.position.symbol == reference && stock.fresh && stock.mark) spot = stock.mark->dollars();
  }
  json horizons = json::array();
  for (const double days : config.days) {
    const auto target = probability_horizon(s.time, days);
    const auto variance = metrics ? implied_variance_until(*metrics, target) : std::nullopt;
    config.variances.push_back(variance);
    const auto cone = [&](double k) -> json {
      if (!variance || !(spot > 0)) return nullptr;
      return {{"low", number(spot * std::exp(-k * std::sqrt(*variance)))}, {"high", number(spot * std::exp(k * std::sqrt(*variance)))}};
    };
    horizons.push_back({{"until", md::format_timestamp(target)}, {"sigma", variance ? number(std::sqrt(*variance)) : json(nullptr)},
                        {"one_sd", cone(1)}, {"two_sd", cone(2)}});
  }
  json betas = json::array();
  double weighted = 0;
  bool weighted_complete = s.risk.complete;
  for (const auto& symbol : held) {
    const auto beta = underlying.empty() ? profile_beta(source, symbol, reference, given) : BetaEstimate{1, analytics::kNaN, 0, "reference"};
    const auto bucket = s.risk.underlyings.find(symbol);
    const std::optional<double> dollar_delta = bucket != s.risk.underlyings.end() ? std::optional(bucket->second.position.dollar_delta) : std::nullopt;
    if (!beta.source.empty()) config.betas[symbol] = beta.beta;
    if (beta.source.empty() || !dollar_delta) weighted_complete = false;
    else weighted += *dollar_delta * beta.beta;
    betas.push_back({{"underlying", symbol}, {"beta", beta.source.empty() ? json(nullptr) : number(beta.beta)},
                     {"source", nullable(beta.source)}, {"observations", beta.observations},
                     {"correlation", number(beta.correlation)},
                     {"dollar_delta", dollar_delta ? number(*dollar_delta) : json(nullptr)},
                     {"weighted_dollar_delta", dollar_delta && !beta.source.empty() ? number(*dollar_delta * beta.beta) : json(nullptr)}});
  }
  const auto profile = snapshot_profile(s, view->config, view->valuations, config, spot, underlying);
  json prices = json::array();
  for (const double percent : config.percent) prices.push_back(spot > 0 ? number(spot * (1 + percent / 100)) : json(nullptr));
  json curves = json::array();
  for (std::size_t i = 0; i < profile.curves.size(); ++i) {
    const auto& curve = profile.curves[i];
    json pnl = json::array();
    for (const double value : curve.pnl) pnl.push_back(profile.complete ? number(value) : json(nullptr));
    curves.push_back({{"days", curve.days}, {"label", nullable(labels[i])}, {"time", md::format_timestamp(curve.time)},
                      {"pnl", pnl}, {"clamped", curve.clamped}, {"horizon", horizons[i]},
                      {"down", level_json(curve.down, spot)}, {"up", level_json(curve.up, spot)},
                      {"soft_down", level_json(curve.soft_down, spot)}, {"soft_up", level_json(curve.soft_up, spot)}});
  }
  return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
      {"underlying", nullable(underlying)}, {"benchmark", underlying.empty() ? json(benchmark) : json(nullptr)},
      {"reference", reference}, {"spot", spot > 0 ? number(spot) : json(nullptr)}, {"vol_points", vol_points},
      {"equity", s.equity.str()}, {"room", money(profile.room)}, {"soft_room", money(profile.soft_room)},
      {"percent", config.percent}, {"prices", prices}, {"curves", curves}, {"betas", betas},
      {"weighted_dollar_delta", weighted_complete ? number(weighted) : json(nullptr)},
      {"weighted_delta", weighted_complete && spot > 0 ? number(weighted / spot) : json(nullptr)},
      {"complete", profile.complete},
      {"model", "Black-76 on each contract's smile IV plus the offset, sticky strike; remaining life and carry shrink by the days; "
                "each underlying moves its beta times the reference's percent move"}}.dump()};
}
}  // namespace

std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source) {
  const auto question = request.target.find('?');
  const auto path = request.target.substr(0, question);
  const bool order_detail = path.starts_with("/api/orders/") && path.find('/', 12) == std::string::npos;
  if (!order_detail && path != "/api/portfolio" && path != "/api/orders" && path != "/api/fills" && path != "/api/risk" &&
      path != "/api/trades.csv" && path != "/api/fills.csv" && path != "/api/account" && path != "/api/account/equity" &&
      path != "/api/trades" && path != "/api/settlements" && path != "/api/plans" && path != "/api/accounts" && path != "/api/risk/profile" &&
      path != "/api/alerts") return {};
  if (path == "/api/risk/profile") {
    const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
    if (!pairs) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
    return risk_profile_response(*pairs, source, request.access.sandbox);
  }
  const auto query = question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1);
  // Every route takes account=ID; orders take status=open|all and client_order_id;
  // trades take status=open|closed|all and attempt=current|all. Each key at most once.
  const auto pairs = query_parameters(query);
  const bool csv = path == "/api/trades.csv" || path == "/api/fills.csv";
  // CSV exports filter by New York date; the equity history by instant.
  std::string account = request.access.sandbox, status = "all", attempt = csv ? "all" : "current", from, to;
  std::optional<std::string> client_order_id;
  std::optional<Timestamp> since, until;
  std::optional<std::uint64_t> limit;
  Timestamp cursor_time = 0;
  std::uint64_t cursor_ordinal = 0;
  bool valid_query = pairs.has_value(), archived = false;
  for (const auto& [key, value] : pairs.value_or(std::map<std::string, std::string>{})) {
    if ((key == "limit" || key == "cursor") && path == "/api/account/equity") {
      const auto positive = [](std::string_view text, auto& number) {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        return ec == std::errc{} && end == text.data() + text.size() && number > 0;
      };
      std::uint64_t count = 0;
      if (key == "limit") {
        if (!positive(value, count) || count > 2000) valid_query = false;
        else limit = count;
      } else {
        const auto colon = value.find(':');
        if (colon == std::string::npos || !positive(std::string_view(value).substr(0, colon), cursor_time) ||
            !positive(std::string_view(value).substr(colon + 1), cursor_ordinal)) valid_query = false;
      }
    } else if ((key == "from" || key == "to") && path == "/api/account/equity") {
      const auto parsed = md::parse_datetime(value, md::Zone::Utc);
      if (!parsed || *parsed < 0) valid_query = false;
      else if (key == "from") since = parsed; else until = parsed;
    } else if (key == "archived" && path == "/api/accounts" && (value == "true" || value == "false")) archived = value == "true";
    else if (key == "account" && path != "/api/accounts" && valid_account(value)) account = value;
    else if (key == "status" && path == "/api/orders" && (value == "open" || value == "all")) status = value;
    else if (key == "client_order_id" && path == "/api/orders" && valid_client_order_id(value)) client_order_id = value;
    else if (key == "status" && (path == "/api/trades" || path == "/api/trades.csv") && (value == "open" || value == "closed" || value == "all")) status = value;
    else if (key == "attempt" && (path == "/api/trades" || path == "/api/trades.csv") && (value == "current" || value == "all")) attempt = value;
    else if (csv && key == "from" && csv_date(value)) from = value;
    else if (csv && key == "to" && csv_date(value)) to = value;
    else valid_query = false;
  }
  if (!from.empty() && !to.empty() && from > to) valid_query = false;
  if (since && until && *since > *until) valid_query = false;
  if (cursor_time && !limit) valid_query = false;
  if (!valid_query) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter");
  if (path == "/api/plans") return ApiResponse{200, plans_json().dump()};
  if (path == "/api/accounts") return ApiResponse{200, json{{"accounts", accounts_json(source, request.access, archived)}}.dump()};
  if (!known_account(source, account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + account);
  const auto view = source.trading_view(account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  if (order_detail) {
    OrderId id;
    try { id = identifier(std::string_view(path).substr(12)); }
    catch (const std::invalid_argument& error) { return api_error(400, "INVALID_REQUEST", error.what()); }
    if (!id || id > s.recent_orders.size()) return api_error(404, "UNKNOWN_ORDER", "Unknown order ID");
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)},
        {"order", order_json(s.recent_orders[id - 1], *view)}}.dump()};
  }
  if (path == "/api/account/equity") {
    json samples = json::array(), next = nullptr;
    Timestamp previous = 0;
    std::uint64_t ordinal = 0;
    std::string last;
    const auto first = std::lower_bound(view->equity_samples.begin(), view->equity_samples.end(), std::max(cursor_time, since.value_or(0)),
        [](const EquitySample& sample, Timestamp time) { return sample.time < time; });
    for (auto it = first; it != view->equity_samples.end(); ++it) {
      const auto& sample = *it;
      if (until && sample.time > *until) break;
      ordinal = sample.time == previous ? ordinal + 1 : 1;
      previous = sample.time;
      if ((since && sample.time < *since) || (until && sample.time > *until) ||
          sample.time < cursor_time || (sample.time == cursor_time && ordinal <= cursor_ordinal)) continue;
      if (limit && samples.size() == *limit) { next = last; break; }
      last = std::to_string(sample.time) + ":" + std::to_string(ordinal);
      samples.push_back({{"time", md::format_timestamp(sample.time)}, {"day", md::format_date(plan_trading_date(view->config.rules, sample.time))},
          {"attempt", sample.attempt}, {"equity", sample.equity.str()}, {"floor", money(sample.floor)}, {"peak", sample.peak.str()},
          {"target", money(sample.target)}, {"tomorrow_floor", money(sample.tomorrow_floor)},
          {"fill", sample.stock_fill ? json("s" + std::to_string(sample.stock_fill)) : sample.fill ? json(std::to_string(sample.fill)) : json(nullptr)}});
    }
    return ApiResponse{200, json{{"samples", samples}, {"next", next}, {"error", nullable(view->equity_error)},
        {"error_time", time_or_null(view->equity_error_time)}, {"error_market_time", time_or_null(view->equity_error_market_time)},
        {"error_recovered", view->equity_error_recovered}}.dump()};
  }
  if (path == "/api/settlements") {
    json records = json::array();
    for (auto it = s.settlements.rbegin(); it != s.settlements.rend(); ++it) {
      const auto& c = it->contract;
      records.push_back({{"symbol", c.osi_symbol()}, {"underlying", c.underlying}, {"expiry", md::format_date(c.expiry)},
          {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"}, {"value", it->value.str()},
          {"time", md::format_timestamp(it->time)}, {"quantity", it->quantity}, {"cash", it->cash.str()},
          {"realised", it->realised.str()}, {"fee", it->fee.str()}, {"source", it->source ? json(*it->source) : json(nullptr)}});
    }
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"settlements", records}}.dump()};
  }
  if (path == "/api/portfolio") return ApiResponse{200, portfolio_json(*view).dump()};
  if (path == "/api/risk") return ApiResponse{200, risk_json(*view).dump()};
  if (path == "/api/account") return ApiResponse{200, account_json(*view).dump()};
  if (path == "/api/trades") return ApiResponse{200, trades_json(*view, status, attempt == "current").dump()};
  if (path == "/api/alerts") {
    json alerts = json::array();
    for (const auto& alert : s.alerts) alerts.push_back(alert_json(alert));
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"alerts", alerts}}.dump()};
  }
  if (csv) {
    json rows = json::array();
    const bool fills = path == "/api/fills.csv";
    if (fills) {
      for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it) rows.push_back(fill_json(*it, *view));
    } else {
      const auto trades = trades_json(*view, status, attempt == "current");
      std::map<std::string, OrderId> fill_orders;
      for (const auto& fill : s.recent_fills) fill_orders[std::to_string(fill.id)] = fill.order_id;
      std::map<OrderId, json> time_stops;
      for (const auto& order : s.recent_orders) {
        if (order.reason.code != Reason::PLAYBOOK_TIME_STOP && order.reason.code != Reason::PLAYBOOK_TRAILING_STOP &&
            order.reason.code != Reason::PLAYBOOK_DTE_STOP && order.reason.code != Reason::PLAYBOOK_DAYS_IN_TRADE_STOP &&
            !order.request.note.starts_with("Playbook automatic ")) continue;
        const auto related = order.parent ? order.parent : order.id;
        if (!time_stops.contains(related)) time_stops[related] = json::array();
        time_stops[related].push_back({{"order_id", std::to_string(order.id)}, {"reason", decision_json(order.reason)}, {"note", order.request.note}});
      }
      for (auto row : trades.at("trades")) {
        row["kind"] = "option";
        row["time_stop_orders"] = json::array();
        std::set<OrderId> related;
        for (const auto& fill : row.at("fills"))
          if (const auto found = fill_orders.find(fill.get<std::string>()); found != fill_orders.end()) related.insert(found->second);
        for (const auto id : related)
          if (const auto found = time_stops.find(id); found != time_stops.end())
            for (const auto& event : found->second) row["time_stop_orders"].push_back(event);
        row["time_stop_orders"] = row["time_stop_orders"].empty() ? "" : row["time_stop_orders"].dump();
        rows.push_back(std::move(row));
      }
      for (const auto& row : trades.at("share_trades")) rows.push_back(row);
    }
    return ApiResponse{200, paper_csv(rows, fills, account.empty() ? std::string(kMainAccount) : account,
                                    source.status().provider, s.account_version, from, to, view->run),
                       "text/csv; charset=utf-8", fills ? "fills.csv" : "trades.csv"};
  }
  json body{{"account_version", std::to_string(s.account_version)}};
  if (path == "/api/orders") {
    body["orders"] = json::array();
    for (auto it = s.recent_orders.rbegin(); it != s.recent_orders.rend(); ++it)
      if ((status != "open" || it->open()) && (!client_order_id || it->request.client_order_id == *client_order_id))
        body["orders"].push_back(order_json(*it, *view));
  } else {
    body["fills"] = json::array();
    for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it) body["fills"].push_back(fill_json(*it, *view));
  }
  return ApiResponse{200, body.dump()};
}

void handle_api_async(const ApiRequest& request, MetricsSource& source, ApiCompletion complete) {
  if (request.method == "POST" && request.target.substr(0, request.target.find('?')) == "/api/sandboxes") {
    if (source.sandboxes()) source.sandboxes()->create(request, source, std::move(complete));
    else complete(api_error(404, "NOT_FOUND", "Sandboxes are not offered"));
    return;
  }
  if (auto rejection = sandbox_visibility(request, source)) { complete(std::move(*rejection)); return; }
  if (request.method == "GET") { complete(handle_api(request, source)); return; }
  if (request.target.starts_with("/api/notifications/")) {
    if (!source.notifications()) { complete(api_error(503, "NOTIFICATIONS_UNAVAILABLE", "Notifications are unavailable on this source")); return; }
    try {
      const auto body = json::parse(request.body);
      if (request.method == "POST" && request.target == "/api/notifications/test") {
        if (!body.is_object() || body.size() != 1 || !body.at("channel").is_string()) throw std::invalid_argument("request");
        const int status = source.notifications()->test(body.at("channel").get<std::string>());
        if (status == 202) complete({202, json{{"queued", true}}.dump()});
        else complete(api_error(status, "NOTIFICATION_TEST_FAILED", "Channel is unknown, disabled, stopped or its queue is full"));
      } else if (request.method == "PUT" && request.target.starts_with("/api/notifications/channels/") && request.target.find('?') == std::string::npos) {
        if (!source.notifications()->configure(request.target.substr(28), body)) complete(api_error(404, "NOT_FOUND", "Unknown notification channel"));
        else complete({200, source.notifications()->status().dump()});
      } else complete(api_error(404, "NOT_FOUND", "Unknown notification endpoint or method"));
    } catch (...) { complete(api_error(400, "INVALID_REQUEST", "Invalid notification settings or test request")); }
    return;
  }
  if (playbook_write(request, source, complete)) return;
  // Writes take one query parameter, account=ID; the route is the path alone.
  const auto question = request.target.find('?');
  const std::string path = request.target.substr(0, question);
  const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
  const bool route = (request.method == "POST" && (path == "/api/orders" || path == "/api/orders/preview" || path == "/api/orders/what-if" ||
      (path.starts_with("/api/orders/") && path.ends_with("/preview")) ||
      path == "/api/orders/cancel" || path == "/api/positions/close" || path == "/api/positions/close/preview" || path == "/api/accounts" ||
      path == "/api/positions/exercise" || path == "/api/stocks/close" || path == "/api/stocks/trade" || path == "/api/stocks/trade/preview" ||
      path == "/api/positions/abandon" || path == "/api/positions/instruction" ||
      path == "/api/risk/kill" || path == "/api/settlements" ||
      path == "/api/account/reset" || path == "/api/account/payout" ||
      path == "/api/trades/group" || path == "/api/trades/ungroup" || path == "/api/alerts")) ||
      (request.method == "PUT" && (path == "/api/risk/limits" || path == "/api/risk/guardrails" || path.starts_with("/api/orders/") ||
                                   ((path.starts_with("/api/trades/") || path.starts_with("/api/days/")) && path.ends_with("/note")))) ||
      (request.method == "DELETE" && (path.starts_with("/api/orders/") || path.starts_with("/api/alerts/"))) ||
      ((request.method == "PATCH" || request.method == "DELETE") && path.starts_with("/api/accounts/"));
  if (!route) { complete(api_error(404, "NOT_FOUND", "Unknown endpoint or method")); return; }
  std::string account = request.access.sandbox;
  if (!pairs || pairs->size() > 1 || (pairs->size() == 1 && (!pairs->contains("account") || !valid_account(pairs->at("account"))) ) ||
      (pairs->size() == 1 && (path == "/api/accounts" || path.starts_with("/api/accounts/")))) {
    complete(api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter"));
    return;
  }
  if (pairs->size() == 1) account = pairs->at("account");
  // An account whose journal another process holds cannot take writes.
  const auto status = source.status();
  for (const auto& a : status.accounts)
    if ((a.id == (account.empty() ? std::string(kMainAccount) : account)) && a.trading.reason.starts_with("JOURNAL_LOCKED:")) {
      complete(api_error(503, "TRADING_UNAVAILABLE", a.trading.reason));
      return;
    }
  if (status.accounts.empty() && status.trading.reason.starts_with("JOURNAL_LOCKED:")) {
    complete(api_error(503, "TRADING_UNAVAILABLE", status.trading.reason));
    return;
  }
  try {
    auto command = parse_command(request, path);
    if (!path.starts_with("/api/accounts/")) command.account = account;
    command.actor = request.actor;
    if (!source.post_trading(command, [command, complete](TradingReply reply) {
          complete(command_response(command, reply));
        })) {
      // Safe to retry: an order resent with the same client_order_id and terms is answered once.
      auto busy = api_error(503, "TRADING_UNAVAILABLE", "Command inbox full or trading unavailable");
      busy.retry_after = kInboxRetrySeconds;
      complete(std::move(busy));
    }
  } catch (const PlanRestrictionError& error) {
    complete(api_error(422, "INVALID_RULES", error.what()));
  } catch (const TradingError& error) {
    complete(api_error(reason_status(error.code()), std::string(to_string(error.code())), error.what()));
  } catch (const std::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  }
}
}  // namespace openport::server
