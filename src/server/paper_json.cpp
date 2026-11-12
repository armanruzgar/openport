// The paper-trading API's JSON encoders: accounts, portfolios, orders, fills, trades,
// previews, plans and risk, as its answers carry them.

#include "paper_internal.hpp"

namespace openport::server {
namespace paper_internal {

json number(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
json nullable(const std::string& value) { return value.empty() ? json(nullptr) : json(value); }
json money(const std::optional<Money>& value) { return value ? json(value->str()) : json(nullptr); }
json positive(Money value) { return value > Money{} ? json(value.str()) : json(nullptr); }
const char* status_name(EvaluationStatus status) {
  constexpr const char* names[] = {"active", "passed", "failed"};
  return names[static_cast<int>(status)];
}
namespace {
json payout_review_json(const std::optional<PayoutReview>& review) {
  if (!review) return nullptr;
  json ladder = json::array();
  for (const auto step : review->ladder) ladder.push_back(kReviewSteps[static_cast<int>(step)]);
  return {{"min_hold_seconds", review->min_hold_seconds}, {"max_trade_percent", review->max_trade_percent},
          {"open_loss", review->open_loss}, {"ladder", ladder}, {"cap", positive(review->cap)}};
}
json payout_rules_json(const PayoutRules& p) {
  json caps = json::array();
  for (const auto cap : p.caps) caps.push_back(cap.str());
  return {{"qualifying_profit", p.qualifying_profit.str()}, {"qualifying_days", p.qualifying_days},
          {"withdrawal_percent", p.withdrawal_percent}, {"split_percent", p.split_percent},
          {"minimum", p.minimum.str()}, {"caps", caps}, {"consistency_percents", p.consistency_percents},
          {"buffer", p.buffer.str()}, {"buffer_payouts", p.buffer_payouts},
          {"review", payout_review_json(p.review)}, {"after_payout", kPayoutFloors[static_cast<int>(p.after_payout)]},
          {"reset_percent", p.reset_percent}};
}
json review_outcome_json(const std::optional<PayoutReviewOutcome>& review) {
  if (!review) return nullptr;
  json trades = json::array();
  for (const auto& t : review->trades)
    trades.push_back({{"id", t.id}, {"pnl", t.pnl.str()}, {"held_seconds", t.held / md::kNanosPerSecond},
                      {"closed", md::format_timestamp(t.closed)}, {"offences", t.offences}});
  return {{"step", kReviewSteps[static_cast<int>(review->step)]}, {"trades", trades}, {"forfeited", review->forfeited.str()}};
}
json open_loss_cap_json(const std::optional<OpenLossCap>& cap) {
  if (!cap) return nullptr;
  return {{"basis", kOpenLossBases[static_cast<int>(cap->basis)]}, {"amount", positive(cap->amount)},
          {"percent", cap->percent}, {"action", kOpenLossActions[static_cast<int>(cap->action)]}};
}
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
}  // namespace
json rules_json(const AccountRules& r, Money initial_cash, const std::optional<SizeScalingProgress>& scaling) {
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
          {"drawdown_basis", kDrawdownBases[static_cast<int>(r.drawdown_basis)]}, {"open_loss_cap", open_loss_cap_json(r.open_loss_cap)},
          {"minute_volume_percent", r.minute_volume_percent}, {"best_position_percent", r.best_position_percent},
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
  if (r.queue_fills) result["queue_fills"] = true;
  if (r.queue_seed != 0) result["queue_seed"] = r.queue_seed;
  if (r.stop_stress_ticks != 0) {
    result["stop_stress_ticks"] = r.stop_stress_ticks;
    result["stop_stress_percent"] = r.stop_stress_percent;
  }
  if (r.cash_credit_rate_bps != 0) result["cash_credit_rate_bps"] = r.cash_credit_rate_bps;
  if (r.margin_debit_rate_bps != 0) result["margin_debit_rate_bps"] = r.margin_debit_rate_bps;
  if (r.stock_borrow_rate_bps != 0) result["stock_borrow_rate_bps"] = r.stock_borrow_rate_bps;
  if (r.pin_risk) result["pin_risk"] = true;
  if (r.pin_risk_seed != 0) result["pin_risk_seed"] = r.pin_risk_seed;
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
namespace {
json guardrails_json(const Guardrails& g) {
  return {{"soft_floor", g.soft_floor.str()}, {"soft_floor_percent", g.soft_floor_percent},
          {"max_opening_trades", g.max_opening_trades}, {"cooldown_loss", g.cooldown_loss.str()},
          {"cooldown_minutes", g.cooldown_minutes}, {"profit_lock", g.profit_lock.str()},
          {"loss_streak", g.loss_streak}, {"size_creep_percent", g.size_creep_percent},
          {"size_creep_window", g.size_creep_window}, {"size_creep_block", g.size_creep_block}};
}
json guardrail_state_json(const TradingSnapshot& s) {
  json latched = json::array();
  for (auto reason : s.guardrails.latched) latched.push_back(to_string(reason));
  if (s.time < s.guardrails.cooldown_until) latched.push_back("COOLDOWN");
  json tilt = json::array();
  for (const auto& flag : s.guardrails.tilt)
    tilt.push_back({{"time", md::format_timestamp(flag.time)}, {"code", to_string(flag.code)}, {"message", flag.message}});
  return {{"opening_trades", s.guardrails.opening_trades}, {"latched", latched},
          {"losing_closes", s.guardrails.losing_closes}, {"recent_opening_sizes", s.guardrails.opening_sizes}, {"tilt", tilt},
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
}  // namespace
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
namespace {
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
}  // namespace
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
namespace {

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
          {"withdrawal_percent", r.payouts.withdrawal_percent}, {"split_percent", r.payouts.split_percent},
          {"review", review_outcome_json(q.review)}, {"forfeit", q.forfeit.str()},
          {"after_payout", kPayoutFloors[static_cast<int>(r.payouts.after_payout)]}, {"reset_balance", money(q.reset_balance)}};
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
}  // namespace
std::string underlying(const TradingView& view, const std::string& symbol) {
  const auto it = view.contracts.find(symbol);
  if (it != view.contracts.end()) return it->second.underlying;
  const auto parsed = md::parse_osi(symbol);
  return parsed ? parsed->underlying : "";
}
namespace {
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
}  // namespace
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
namespace {
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
/// The queue model's estimate for a resting limit; null without queue fills.
json queue_json(const Order& o) {
  if (!o.queue) return nullptr;
  const auto& q = *o.queue;
  return {{"price", q.price.str()}, {"ahead", q.ahead}, {"earned", q.earned}, {"displayed", q.displayed},
          {"joined_at", md::format_timestamp(q.joined)}, {"updated_at", md::format_timestamp(q.updated)},
          {"source", q.source}, {"simulated", true}};
}
const char* stock_type_name(StockOrderType type) {
  constexpr const char* names[] = {"market", "limit", "stop", "stop_limit"};
  return names[static_cast<int>(type)];
}
}  // namespace
json stock_order_json(const StockOrder& o) {
  constexpr const char* statuses[] = {"working", "partially_filled", "filled", "cancelled", "rejected", "armed"};
  constexpr const char* roles[] = {"entry", "stop_loss", "take_profit"};
  const auto& r = o.request;
  const auto id = [](OrderId value) { return value ? json(std::to_string(value)) : json(nullptr); };
  return {{"id", std::to_string(o.id)}, {"client_order_id", r.client_order_id}, {"actor", o.actor}, {"symbol", r.symbol},
          {"side", side_name(r.side)}, {"type", stock_type_name(r.type)}, {"time_in_force", r.tif == TimeInForce::Gtc ? "gtc" : "day"},
          {"shares", r.shares}, {"limit_price", money(r.limit_price)}, {"stop_price", money(r.stop_price)},
          {"extended_hours", r.extended_hours},
          {"bracket", r.stop_loss || r.take_profit ? json{{"stop_loss", money(r.stop_loss)}, {"take_profit", money(r.take_profit)}} : json(nullptr)},
          {"role", roles[static_cast<int>(o.role)]}, {"parent", id(o.parent)}, {"oco", id(o.oco)},
          {"stop_loss", id(o.stop_loss)}, {"take_profit", id(o.take_profit)},
          {"status", statuses[static_cast<int>(o.status)]}, {"reason", decision_json(o.reason)},
          {"accepted_at", md::format_timestamp(o.accepted_at)},
          {"day_end", o.day_end == std::numeric_limits<Timestamp>::max() ? json(nullptr) : json(md::format_timestamp(o.day_end))},
          {"triggered_at", o.triggered_at ? json(md::format_timestamp(o.triggered_at)) : json(nullptr)},
          {"ended_at", o.ended_at ? json(md::format_timestamp(o.ended_at)) : json(nullptr)},
          {"filled_shares", o.filled_shares}, {"fill_price", o.filled_shares ? json(o.fill_price.str()) : json(nullptr)},
          {"stock_fill", id(o.stock_fill)}};
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
          {"ended_at", time_or_null(o.ended_at)}, {"modified_at", modified_at(o)}, {"changes", order_changes_json(o)}, {"changes_dropped", o.changes_dropped},
          {"attempt", order_attempt(*view.snapshot, o.id)},
          {"origin", o.system ? "system" : "user"}, {"reduce_only", o.reduce_only},
          {"trigger", trigger_json(o.request.trigger)},
          {"triggered_at", o.triggered_at > 0 ? json(md::format_timestamp(o.triggered_at)) : json(nullptr)},
          {"bracket", o.request.bracket ? json{{"stop_loss", exit_json(o.request.bracket->stop_loss)},
                                               {"take_profit", exit_json(o.request.bracket->take_profit)}} : json(nullptr)},
          {"role", nullable(roles[static_cast<int>(o.role)])}, {"parent", id_or_null(o.parent)}, {"oco", id_or_null(o.oco)},
          {"stop_loss_order", id_or_null(o.stop_loss)}, {"take_profit_order", id_or_null(o.take_profit)},
          {"then", o.request.then.empty() ? json(nullptr) : chained_json(o.request.then.front())},
          {"chained_order", id_or_null(o.chained)}, {"chained_from", id_or_null(o.chained_from)},
          {"queue", queue_json(o)}, {"stress_ticks", o.stress_ticks}};
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
namespace {
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
}  // namespace
json day_notes_json(const TradingSnapshot& s) {
  json notes = json::object();
  for (const auto& [day, note] : s.day_notes)
    notes[day] = {{"plan", note.plan}, {"review", note.review}, {"time", md::format_timestamp(note.time)}};
  return notes;
}
namespace {
/// The attempt an option fill belongs to: each attempt's fills start at its first_fill.
std::uint64_t fill_attempt(const TradingSnapshot& s, std::uint64_t fill) {
  if (fill >= s.evaluation.first_fill) return s.evaluation.attempt;
  for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
    if (fill >= it->first_fill) return it->attempt;
  return 1;
}
}  // namespace
/// Whether a stock fill belongs to an attempt or a later one: from the attempt's first
/// stock fill where it kept one (every reset from replay driver 12 on), otherwise from
/// its start, as runs before it placed them. With no stock fill before an attempt its
/// boundary is 1, where the two agree, as market time never goes back.
bool from_attempt(std::uint64_t first_stock_fill, Timestamp started, std::uint64_t fill, Timestamp time) {
  return first_stock_fill > 1 ? fill >= first_stock_fill : time >= started;
}
namespace {
/// The attempt a share round trip belongs to: the one its opening stock fill is in.
std::uint64_t share_attempt(const TradingSnapshot& s, const ShareLifecycle& trip) {
  const auto opening = trip.fills.front();
  if (from_attempt(s.evaluation.first_stock_fill, s.evaluation.started, opening, trip.opened)) return s.evaluation.attempt;
  for (auto it = s.attempts.rbegin(); it != s.attempts.rend(); ++it)
    if (from_attempt(it->first_stock_fill, it->started, opening, trip.opened)) return it->attempt;
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
}  // namespace
json fill_json(const Fill& f, const TradingView& view) {
  return {{"id", std::to_string(f.id)}, {"order_id", std::to_string(f.order_id)}, {"actor", f.actor},
          {"attempt", fill_attempt(*view.snapshot, f.id)},
          {"symbol", f.symbol}, {"underlying", underlying(view, f.symbol)},
          {"side", f.side == Side::Buy ? "buy" : "sell"}, {"quantity", f.quantity},
          {"price", f.price.str()}, {"fee", f.fee.str()}, {"fees", fill_fees_json(f.fees)}, {"context", context_json(f.context)},
          {"quote", fill_quote_json(f)}, {"feed", f.feed.empty() ? json(nullptr) : json(f.feed)},
          {"stress_ticks", f.stress_ticks}, {"queued", f.queued},
          {"quote_time", md::format_timestamp(f.quote_time)}, {"time", md::format_timestamp(f.time)}};
}
namespace {
json fill_group_json(const FillQualityGroup& g) {
  json places = json::object();
  for (std::size_t i = 0; i < kFillPlaces; ++i) places[std::string(to_string(static_cast<FillPlace>(i)))] = g.places[i];
  const auto mean = [](const std::optional<double>& value) { return value ? number(*value) : json(nullptr); };
  return {{"key", g.key}, {"label", g.label}, {"fills", g.fills}, {"contracts", g.contracts}, {"places", places},
          {"from_mid_ticks", mean(g.from_mid_ticks)}, {"vs_natural_ticks", mean(g.vs_natural_ticks)},
          {"cost_vs_mid", g.cost_vs_mid.str()}};
}
json fill_groups_json(const std::vector<FillQualityGroup>& groups) {
  json out = json::array();
  for (const auto& g : groups) out.push_back(fill_group_json(g));
  return out;
}
json fill_calibration_json(const FillCalibration& c) {
  return {{"basis", c.basis}, {"fill_model", c.fill_model},
          {"fill_latency_ms", c.fill_latency_ms ? json(*c.fill_latency_ms) : json(nullptr)},
          {"slippage_ticks", c.slippage_ticks}, {"impact_ticks", c.impact_ticks},
          {"inside_fill_percent", c.inside_fill_percent}, {"reasons", c.reasons}};
}
}  // namespace
/// The paper fills by place on their book (GET /api/fills/quality).
json fill_quality_json(const TradingView& view, bool current) {
  const auto& s = *view.snapshot;
  const auto report = fill_quality(s.recent_fills, view.contracts, current ? s.evaluation.first_fill : 0);
  return {{"attempt", current ? "current" : "all"}, {"fills", report.fills}, {"measured", report.fills - report.unmeasured},
          {"unmeasured", report.unmeasured}, {"overall", fill_group_json(report.overall)},
          {"time_of_day", fill_groups_json(report.time_of_day)}, {"spread", fill_groups_json(report.spread)},
          {"dte", fill_groups_json(report.dte)}, {"suggestion", fill_calibration_json(report.suggestion)}, {"simulated", true}};
}
namespace {
json paper_outcome_json(const PaperOutcome& outcome) {
  return {{"fills", outcome.fills}, {"price", money(outcome.price)}};
}
}  // namespace
/// Real broker fills against the same-time books and paper models
/// (POST /api/fills/calibration). Nothing is stored or traded.
json calibration_json(const CalibrationReport& report) {
  json rows = json::array();
  for (const auto& r : report.rows) {
    const auto& f = r.fill;
    json quote = nullptr;
    if (r.quote)
      quote = {{"bid", r.quote->bid.str()}, {"ask", r.quote->ask.str()}, {"bid_size", r.quote->bid_size},
               {"ask_size", r.quote->ask_size}, {"time", time_or_null(r.quote->time)}, {"source", r.quote->source}};
    rows.push_back({{"row", f.row}, {"time", time_or_null(f.time)}, {"symbol", f.symbol}, {"side", side_name(f.side)},
                    {"quantity", f.quantity}, {"price", f.price.str()}, {"quote", quote},
                    {"place", r.real ? json(std::string(to_string(r.real->place))) : json(nullptr)},
                    {"from_mid_ticks", r.real ? number(r.real->from_mid_ticks) : json(nullptr)},
                    {"vs_natural_ticks", r.real ? number(r.real->vs_natural_ticks) : json(nullptr)},
                    {"as_displayed", paper_outcome_json(r.as_displayed)}, {"conservative", paper_outcome_json(r.conservative)},
                    {"midpoint", paper_outcome_json(r.midpoint)}, {"note", nullable(r.note)}});
  }
  json skipped = json::array();
  for (const auto& [row, reason] : report.skipped) skipped.push_back({{"row", row}, {"reason", reason}});
  return {{"format", report.format}, {"matched", report.matched}, {"unmatched", report.unmatched}, {"rows", rows},
          {"real", fill_group_json(report.real)},
          {"paper_fills", {{"as_displayed", report.paper_fills[0]}, {"conservative", report.paper_fills[1]}, {"midpoint", report.paper_fills[2]}}},
          {"suggestion", fill_calibration_json(report.suggestion)}, {"skipped", skipped}, {"stored", false}};
}
namespace {
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
}  // namespace
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
    if (share_attempt(s, t) != s.evaluation.attempt) continue;
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
        {"modelled_mark", p.modelled_mark},
        // How an expired position settles: on the closing print, recorded or still awaited
        // (with the last print before the close as the fallback) for half an hour after the
        // close, or by a value entered by hand once no automatic source remains.
        {"settle_by", !p.awaiting_settlement ? json(nullptr) : json(std::string(settle_by(c, s, view)))},
        // An adjusted series: what one contract delivers now, or null for 100 shares.
        {"deliverable", [&]() -> json {
           const auto d = s.deliverables.find(c.osi_symbol());
           if (d == s.deliverables.end()) return nullptr;
           return {{"shares", d->second.shares}, {"cash", d->second.cash.str()}};
         }()},
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
        {"realised", p.realised.str()}, {"fees", p.fees.str()}, {"fresh", held.fresh}, {"halted", held.halted},
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
    closed.push_back({{"symbol", symbol}, {"kind", symbol == "cash" ? "cash" : contract == view.contracts.end() ? "shares" : "option"},
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
          {"liquidity_used", liquidity_used_json(view)}, {"closed", closed}, {"strategies", strategies},
          {"stock_orders", [&] {
             json open = json::array();
             for (const auto& o : s.stock_orders) if (o.open()) open.push_back(stock_order_json(o));
             return open;
           }()}};
}
namespace {
json damage_json(const std::optional<AccountDamage>& damage) {
  if (!damage) return nullptr;
  return {{"reason", damage->reason}, {"last_good_seq", damage->last_good_seq},
          {"last_good_time", time_or_null(damage->last_good_time)}};
}
json journal_json(std::uint64_t bytes, std::uint64_t records, bool replay) {
  return {{"bytes", bytes}, {"records", records}, {"warning", nullable(journal_warning(bytes, records, replay))}};
}
}  // namespace
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
  json cash_flows = json::array();
  for (const auto& f : s.cash_flows)
    cash_flows.push_back({{"id", std::to_string(f.id)}, {"attempt", f.attempt}, {"time", md::format_timestamp(f.time)},
        {"day", md::format_date(f.day)}, {"kind", f.kind}, {"symbol", nullable(f.symbol)}, {"amount", f.amount.str()},
        {"basis", f.basis.str()}, {"rate_bps", f.rate_bps}, {"days", f.days}, {"client_id", nullable(f.client_id)}});
  json payouts = json::array();
  for (const auto& p : e.payouts)
    payouts.push_back({{"number", p.number}, {"time", md::format_timestamp(p.time)}, {"day", md::format_date(p.day)}, {"amount", p.amount.str()},
                       {"trader_share", p.trader_share.str()}, {"balance", p.balance.str()},
                       {"review", review_outcome_json(p.review)}, {"removed", positive(p.removed)}});
  json open_loss = nullptr;
  if (r.open_loss_cap) {
    const auto limit = open_loss_limit(e, r, in);
    open_loss = {{"limit", money(limit)}, {"positions", e.open_loss_symbols}, {"trades", e.open_loss_trades}};
  }
  json best_position = nullptr;
  if (r.phase != Phase::Funded && r.best_position_percent > 0)
    best_position = {{"cap", best_position_cap(r).str()}, {"excess", e.position_excess.str()},
                     {"counted_profit", (attempt_profit(e, r, in) - e.position_excess).str()},
                     {"trade", e.best_position ? json{{"id", e.best_position->id}, {"pnl", e.best_position->pnl.str()}} : json(nullptr)}};
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
              {"open_loss", open_loss}, {"best_position", best_position},
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
              // Whether today's flat time applies: a market day by the holiday calendar.
              {"flat_today", r.flat_time.has_value() && s.time > 0 &&
                  md::market_session(md::new_york_to_utc(plan_trading_date(r, s.time), 9, 30)).open},
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
          {"cash_transfers_allowed", r.practice()}, {"cash_flows", cash_flows},
          {"corporate_actions", [&] {
             json list = json::array();
             for (const auto& a : s.corporate_actions) {
               json series = json::array();
               for (const auto& [symbol, d] : a.series) series.push_back({{"symbol", symbol}, {"shares", d.shares}, {"cash", d.cash.str()}});
               list.push_back({{"symbol", a.symbol}, {"ex_date", md::format_date(a.ex_date)}, {"kind", "split"},
                               {"numerator", a.numerator}, {"denominator", a.denominator}, {"shares_before", a.shares_before},
                               {"shares_after", a.shares_after}, {"cash_in_lieu", a.cash_in_lieu.str()},
                               {"time", md::format_timestamp(a.time)}, {"series", series}});
             }
             return list;
           }()},
          {"attempts", attempts}};
}
namespace {
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
    case StockSource::Split: return "split";
  }
  return nullptr;
}
}  // namespace
json stock_fill_json(const StockFill& fill) {
  return {{"id", std::to_string(fill.id)}, {"symbol", fill.symbol}, {"shares", fill.shares},
          {"price", fill.price.str()}, {"time", md::format_timestamp(fill.time)}, {"source", share_source(fill)},
          {"option", nullable(fill.option)}, {"buying_power_after", money(fill.buying_power_after)},
          {"stock_order", fill.stock_order ? json(std::to_string(fill.stock_order)) : json(nullptr)}};
}
namespace {
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
}  // namespace
json trades_json(const TradingView& view, std::string_view status, bool current_only,
                 std::optional<Timestamp> since, std::optional<Timestamp> until) {
  const auto& s = *view.snapshot;
  const auto& e = s.evaluation;
  // from and to: a round trip or whole trade by its close, or its open while it is
  // open, as the trades CSV keeps its rows; share fills and dividends by their time.
  const auto within = [&](Timestamp time) { return (!since || time >= *since) && (!until || time <= *until); };
  std::map<std::pair<std::string, Timestamp>, const SettlementRecord*> settlements;
  for (const auto& record : s.settlements) settlements[{record.contract.osi_symbol(), record.time}] = &record;
  const auto all = lifecycles(s.recent_fills, s.closures, view.contracts);
  // Every closure of each round trip, a partial exercise or assignment as much as the
  // settlement, reset or abandonment that ended it: it belongs to its contract's round
  // trip that opened last before it (by fills recorded) and was still open then.
  std::map<std::string, std::vector<const Lifecycle*>> trips_by_symbol;
  for (const auto& trip : all) trips_by_symbol[trip.symbol].push_back(&trip);
  std::map<std::uint64_t, json> trip_closures;
  for (const auto& closure : s.closures) {
    const auto trips = trips_by_symbol.find(closure.symbol);
    if (trips == trips_by_symbol.end()) continue;
    const Lifecycle* owner = nullptr;
    for (const auto* trip : trips->second)
      if (trip->first_fill <= closure.after_fill && (owner == nullptr || trip->first_fill > owner->first_fill)) owner = trip;
    if (owner == nullptr || (owner->closed && closure.time > *owner->closed)) continue;
    auto& owned = trip_closures[owner->first_fill];
    if (!owned.is_array()) owned = json::array();
    owned.push_back({{"fee", closure.fee.str()}, {"kind", closure_name(closure.kind)}, {"price", closure.price.str()},
                      {"quantity", closure.quantity < 0 ? -closure.quantity : closure.quantity},
                      {"time", md::format_timestamp(closure.time)}});
  }
  json trades = json::array();
  for (auto it = all.rbegin(); it != all.rend(); ++it) {
    const auto& t = *it;
    const bool open = !t.closed;
    if ((status == "open" && !open) || (status == "closed" && open)) continue;
    if (!within(t.closed.value_or(t.opened))) continue;
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
    trades.push_back({{"entry_order", std::to_string(order_id)}, {"entry_context", context_json(t.entry_context)}, {"exit_context", context_json(t.exit_context)},
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
        {"closures", trip_closures.contains(t.first_fill) ? trip_closures.at(t.first_fill) : json::array()},
        {"settlement_value", settlement ? json(settlement->value.str()) : json(nullptr)},
        {"settlement_source", settlement && settlement->source && settlement->source->contains("kind")
            ? json(settlement->source->at("kind")) : json(nullptr)},
        {"closed_by", exit.first}, {"system_reason", exit.second},
        {"attribution", trip_attribution_json(s, std::to_string(t.first_fill))},
        {"group", trade_group(t, s.groups)},
        {"fills", fills},
        {"mistakes", a == s.annotations.end() ? json::array() : json(a->second.mistakes)},
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
    if (!within(open ? first.opened : closed)) continue;
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
    if (!within(t.closed.value_or(t.opened))) continue;
    const auto attempt = share_attempt(s, t);
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
        {"mistakes", note == s.annotations.end() ? json::array() : json(note->second.mistakes)},
        {"note", note == s.annotations.end() ? std::string{} : note->second.note},
        {"tags", note == s.annotations.end() ? json::array() : json(note->second.tags)}});
  }
  // Every change in shares and every dividend, oldest first, for the terminal's alerts.
  json stock_fills = json::array();
  for (const auto& fill : s.stock_fills)
    if (within(fill.time)) stock_fills.push_back(stock_fill_json(fill));
  json dividends = json::array();
  for (const auto& d : s.dividends)
    if (within(d.time)) dividends.push_back({{"symbol", d.symbol}, {"ex_date", md::format_date(d.ex_date)}, {"per_share", d.per_share.str()},
        {"shares", d.shares}, {"amount", d.amount.str()}, {"time", md::format_timestamp(d.time)}});
  // Day notes by their New York day, as a date bound reads.
  auto day_notes = day_notes_json(s);
  const auto first_day = since ? md::format_date(md::new_york_time(*since).date) : std::string{};
  const auto last_day = until ? md::format_date(md::new_york_time(*until).date) : std::string{};
  for (auto note = day_notes.begin(); note != day_notes.end();) {
    if ((since && note.key() < first_day) || (until && note.key() > last_day)) note = day_notes.erase(note);
    else ++note;
  }
  return {{"account_version", std::to_string(s.account_version)}, {"attempt", e.attempt}, {"trades", trades},
          {"groups", groups}, {"share_trades", share_trades}, {"stock_fills", stock_fills}, {"dividends", dividends}, {"day_notes", day_notes},
          {"run", run_json(view.run)}};
}
namespace {
json unlocks_json(const PlanPreset& plan) {
  json out = json::array();
  for (const auto& candidate : plan_presets())
    if (candidate.unlocked_by == plan.id) out.push_back(candidate.id);
  return out;
}
}  // namespace
json plans_json() {
  json plans = json::array();
  for (const auto& p : plan_presets())
    plans.push_back({{"id", p.id}, {"name", p.name}, {"summary", p.summary},
                     {"initial_cash", p.initial_cash.str()}, {"rules", rules_json(p.rules, p.initial_cash)},
                     {"phase", p.rules.phase == Phase::Funded ? "funded" : p.rules.phase == Phase::Verification ? "verification" : "evaluation"},
                     {"unlocked_by", nullable(p.unlocked_by)}, {"unlocks", unlocks_json(p)},
                     // Built-in terms are this project's own examples, not any firm's.
                     {"illustrative", true}});
  return {{"plans", plans}};
}
namespace {
json exposure_limits(const ExposureLimits& limits) {
  json result{{"dollar_delta", limits.dollar_delta}, {"vega", limits.vega}};
  for (const auto& [name, cap] : std::vector<std::pair<std::string, std::optional<double>>>{
      {"dollar_gamma_1pct", limits.dollar_gamma_1pct}, {"theta", limits.theta},
      {"worst_grid_loss", limits.worst_grid_loss}, {"short_vega", limits.short_vega}})
    result[name] = cap ? number(*cap) : json(nullptr);
  return result;
}
json limits_json(const Limits& limits) {
  json overrides = json::object();
  for (const auto& [symbol, caps] : limits.underlying_overrides) overrides[symbol] = exposure_limits(caps);
  json tags = json::object(), playbooks = json::object();
  for (const auto& [name, caps] : limits.tag_overrides) tags[name] = exposure_limits(caps);
  for (const auto& [name, caps] : limits.playbook_overrides) playbooks[name] = exposure_limits(caps);
  return {{"tag_overrides", tags}, {"playbook_overrides", playbooks}, {"max_order_contracts", limits.max_order_contracts}, {"price_band_absolute", limits.price_band_absolute.str()},
          {"price_band_relative", limits.price_band_relative}, {"aggregate", exposure_limits(limits.aggregate)},
          {"per_underlying", exposure_limits(limits.per_underlying)}, {"underlying_overrides", overrides},
          {"max_daily_loss", limits.max_daily_loss.str()},
          {"max_quote_age_seconds", limits.max_quote_age / md::kNanosPerSecond},
          {"max_valuation_age_seconds", limits.max_valuation_age / md::kNanosPerSecond}};
}
json bucket_json(const RiskBucket& b) {
  const auto optional = [](std::optional<double> x) -> json { return x ? number(*x) : json(nullptr); };
  return {{"short_vega", number(b.short_vega)}, {"worst_grid_loss", optional(b.worst_grid_loss)},
          {"gamma_utilisation", optional(b.gamma_utilisation)}, {"theta_utilisation", optional(b.theta_utilisation)},
          {"grid_utilisation", optional(b.grid_utilisation)}, {"short_vega_utilisation", optional(b.short_vega_utilisation)},
          {"dollar_delta", b.position.dollar_delta}, {"dollar_gamma_1pct", b.position.dollar_gamma_1pct},
          {"vega", b.position.vega}, {"theta", b.position.theta},
          {"reachable", {{"delta_low", b.reachable.delta_low}, {"delta_high", b.reachable.delta_high},
                         {"vega_low", b.reachable.vega_low}, {"vega_high", b.reachable.vega_high},
                         {"gamma_low", b.reachable.gamma_low}, {"gamma_high", b.reachable.gamma_high},
                         {"theta_low", b.reachable.theta_low}, {"theta_high", b.reachable.theta_high}}},
          {"limits", exposure_limits(b.limits)}, {"delta_utilisation", b.delta_utilisation}, {"vega_utilisation", b.vega_utilisation}};
}
}  // namespace
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
/// `grid` replaces the snapshot's sticky-strike grid with one recomputed under `dynamics`.
json risk_json(const TradingView& view, const ScenarioGrid* grid,
               SmileDynamics dynamics) {
  const auto& s = *view.snapshot;
  const auto& scenarios = grid ? *grid : s.scenarios;
  json underlyings = json::object();
  for (const auto& [symbol, bucket] : s.risk.underlyings) underlyings[symbol] = bucket_json(bucket);
  json scopes = json::object();
  for (const auto& [name, bucket] : s.risk.scopes) scopes[name] = bucket_json(bucket);
  json pnl = json::array(), clamped = json::array();
  std::size_t index = 0;
  for ([[maybe_unused]] double spot : view.config.scenarios.spot_percent) {
    json row = json::array(), clamps = json::array();
    for ([[maybe_unused]] double vol : view.config.scenarios.vol_points) {
      const auto& cell = scenarios.cells.at(index++);
      row.push_back(scenarios.complete ? number(cell.pnl) : json(nullptr));
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
          {"kill", kill_json(s)}, {"aggregate", bucket_json(s.risk.aggregate)}, {"underlyings", underlyings}, {"scopes", scopes},
          {"scenarios", {{"spot_percent", view.config.scenarios.spot_percent}, {"vol_points", view.config.scenarios.vol_points},
                         {"pnl", pnl}, {"clamped", clamped}, {"complete", scenarios.complete},
                         {"dynamics", to_string(dynamics)}, {"surface_fallback", scenarios.surface_fallback}}}};
}
}  // namespace paper_internal

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
          {"history_version", std::to_string(status.history_version)},
          {"kill_latched", status.kill_latched}, {"write", status.write},
          {"fee_per_contract", status.fee_per_contract.str()},
          {"initial_cash", status.initial_cash.str()},
          {"plan", nullable(status.plan)}, {"plan_id", nullable(status.plan_id)}, {"evaluation", nullable(status.evaluation)}};
}
json accounts_json(const MetricsSource& source, const ApiAccess& access, bool archived) {
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

json tail_risk_json(const TailRisk& value) {
  const auto num = [](const std::optional<double>& x) -> json { return x ? number(*x) : json(nullptr); };
  return json{{"complete", value.complete}, {"observations", value.observations},
    {"var95", num(value.var95)}, {"es95", num(value.es95)}, {"var99", num(value.var99)}, {"es99", num(value.es99)},
    {"tail99_reason", value.var99 ? json(nullptr)
        : json(value.tail99_reason.value_or("Needs fresh valuations and at least 100 aligned daily moves"))},
    {"first", value.first ? json(md::format_date(*value.first)) : json(nullptr)},
    {"last", value.last ? json(md::format_date(*value.last)) : json(nullptr)}};
}
}  // namespace openport::server
