#include "openport/trading/types.hpp"

#include <algorithm>
#include <set>

namespace openport::trading {
void validate_guardrails(const Guardrails& g, bool stored) {
  if (!stored && g.soft_floor_percent == 100)
    throw TradingError(Reason::INVALID_LIMITS, "Soft floor percent must be 0-99: 100 can put the soft floor at the peak, where it latches at once");
  if (g.soft_floor < Money{} || g.soft_floor_percent < 0 || g.soft_floor_percent > 100 ||
      g.max_opening_trades < 0 || g.cooldown_loss < Money{} || g.cooldown_minutes < 0 ||
      g.cooldown_minutes > 1440 || g.profit_lock < Money{})
    throw TradingError(Reason::INVALID_LIMITS, "Guardrails must be nonnegative; floor percent is 0-99 and cooldown at most 1440 minutes");
  if (g.loss_streak < 0 || g.loss_streak > 100 || (g.loss_streak > 0 && g.cooldown_minutes == 0))
    throw TradingError(Reason::INVALID_LIMITS, "A losing streak of 1-100 closes starts the cooldown, so it needs cooldown_minutes");
  if (g.size_creep_percent < 0 || (g.size_creep_percent > 0 && (g.size_creep_percent <= 100 || g.size_creep_percent > 10000)) ||
      g.size_creep_window < 0 || (g.size_creep_window != 0 && (g.size_creep_window < 3 || g.size_creep_window > 100)) ||
      ((g.size_creep_window != 0 || g.size_creep_block) && g.size_creep_percent == 0))
    throw TradingError(Reason::INVALID_LIMITS, "Size creep is 101-10000% of the rolling median of the last 3-100 opening orders "
                       "(zero: 20); its window and block need a percentage");
}
Limits tightened_limits(const Limits& current, const Limits& requested) {
  auto result = requested;
  result.max_order_contracts = std::min(current.max_order_contracts, requested.max_order_contracts);
  result.price_band_absolute = std::min(current.price_band_absolute, requested.price_band_absolute);
  result.price_band_relative = std::min(current.price_band_relative, requested.price_band_relative);
  result.max_daily_loss = std::min(current.max_daily_loss, requested.max_daily_loss);
  result.max_quote_age = std::min(current.max_quote_age, requested.max_quote_age);
  result.max_valuation_age = std::min(current.max_valuation_age, requested.max_valuation_age);
  const auto tighter = [](const ExposureLimits& a, const ExposureLimits& b) {
    const auto cap = [](std::optional<double> x, std::optional<double> y) { return !x ? y : !y ? x : std::optional(std::min(*x, *y)); };
    return ExposureLimits{std::min(a.dollar_delta, b.dollar_delta), std::min(a.vega, b.vega),
        cap(a.dollar_gamma_1pct, b.dollar_gamma_1pct), cap(a.theta, b.theta),
        cap(a.worst_grid_loss, b.worst_grid_loss), cap(a.short_vega, b.short_vega)};
  };
  result.aggregate = tighter(current.aggregate, requested.aggregate);
  result.per_underlying = tighter(current.per_underlying, requested.per_underlying);
  std::set<std::string> names;
  for (const auto& [name, limit] : current.underlying_overrides) names.insert(name);
  for (const auto& [name, limit] : requested.underlying_overrides) names.insert(name);
  for (const auto& name : names) {
    const auto a = current.underlying_overrides.find(name), b = requested.underlying_overrides.find(name);
    result.underlying_overrides[name] = tighter(a == current.underlying_overrides.end() ? current.per_underlying : a->second,
                                               b == requested.underlying_overrides.end() ? requested.per_underlying : b->second);
  }
  const auto scopes = [&](const auto& old, auto& next) {
    for (const auto& [name, caps] : old) {
      const auto found = next.find(name);
      if (found == next.end()) next[name] = caps;
      else found->second = tighter(caps, found->second);
    }
  };
  scopes(current.tag_overrides, result.tag_overrides);
  scopes(current.playbook_overrides, result.playbook_overrides);
  return result;
}
Guardrails tightened_guardrails(const Guardrails& current, const Guardrails& requested) {
  const auto earlier = [](auto a, auto b) { return a == decltype(a){} ? b : b == decltype(b){} ? a : std::min(a, b); };
  Guardrails result;
  result.soft_floor = std::max(current.soft_floor, requested.soft_floor);
  result.soft_floor_percent = std::max(current.soft_floor_percent, requested.soft_floor_percent);
  result.max_opening_trades = earlier(current.max_opening_trades, requested.max_opening_trades);
  result.cooldown_minutes = std::max(current.cooldown_minutes, requested.cooldown_minutes);
  result.cooldown_loss = earlier(current.cooldown_loss, requested.cooldown_loss);
  result.profit_lock = earlier(current.profit_lock, requested.profit_lock);
  result.loss_streak = earlier(current.loss_streak, requested.loss_streak);
  result.size_creep_percent = earlier(current.size_creep_percent, requested.size_creep_percent);
  // A block is stricter than a warning; the window is not, so the requested one applies.
  const auto& creep = current.size_creep_percent == 0 || requested.size_creep_percent != 0 ? requested : current;
  result.size_creep_window = result.size_creep_percent == 0 ? 0 : creep.size_creep_window;
  result.size_creep_block = result.size_creep_percent != 0 &&
      ((current.size_creep_percent != 0 && current.size_creep_block) || (requested.size_creep_percent != 0 && requested.size_creep_block));
  return result;
}
}  // namespace openport::trading
