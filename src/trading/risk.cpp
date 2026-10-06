#include "openport/trading/risk.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "openport/pricing/black.hpp"

namespace openport::trading {
namespace {
bool fresh(const Valuation& v, const md::OptionContract& c, Timestamp now, Timestamp max_age) {
  return valid_valuation(v) && v.time >= 0 && v.time <= now && observation_time(c, now) - v.time <= max_age;
}
Exposure exposure(Quantity q, const Valuation& v) {
  const double units = static_cast<double>(q) * 100;
  return {units * v.delta * v.spot, units * v.gamma * v.spot * v.spot * 0.01,
          units * v.vega, units * v.theta};
}
bool finite(const Exposure& e) {
  return std::isfinite(e.dollar_delta) && std::isfinite(e.dollar_gamma_1pct) &&
         std::isfinite(e.vega) && std::isfinite(e.theta);
}
bool add(RiskBucket& b, const Exposure& e, bool pending) {
  if (pending) {
    b.reachable.delta_low += std::min(0.0, e.dollar_delta);
    b.reachable.delta_high += std::max(0.0, e.dollar_delta);
    b.reachable.vega_low += std::min(0.0, e.vega);
    b.reachable.vega_high += std::max(0.0, e.vega);
  } else {
    b.position.dollar_delta += e.dollar_delta;
    b.position.dollar_gamma_1pct += e.dollar_gamma_1pct;
    b.position.vega += e.vega;
    b.position.theta += e.theta;
    b.reachable.delta_low += e.dollar_delta;
    b.reachable.delta_high += e.dollar_delta;
    b.reachable.vega_low += e.vega;
    b.reachable.vega_high += e.vega;
  }
  return finite(b.position) && std::isfinite(b.reachable.delta_low) &&
         std::isfinite(b.reachable.delta_high) && std::isfinite(b.reachable.vega_low) &&
         std::isfinite(b.reachable.vega_high);
}
double worst(double low, double high) { return std::max(std::abs(low), std::abs(high)); }
double utilisation(double value, double limit) {
  if (limit == 0) return value == 0 ? 0 : std::numeric_limits<double>::max();
  const double result = value / limit;
  return std::isfinite(result) ? result : std::numeric_limits<double>::max();
}
void finish(RiskBucket& b) {
  b.delta_utilisation = utilisation(worst(b.reachable.delta_low, b.reachable.delta_high), b.limits.dollar_delta);
  b.vega_utilisation = utilisation(worst(b.reachable.vega_low, b.reachable.vega_high), b.limits.vega);
}
/// A bucket over a limit fails, unless `before` (the bucket without the order
/// being checked) was at least as far over it on that factor.
Decision check_bucket(const RiskBucket& b, const std::string& scope, const RiskBucket* before = nullptr) {
  const double delta = worst(b.reachable.delta_low, b.reachable.delta_high);
  const double vega = worst(b.reachable.vega_low, b.reachable.vega_high);
  const bool delta_raised = !before || delta > worst(before->reachable.delta_low, before->reachable.delta_high);
  const bool vega_raised = !before || vega > worst(before->reachable.vega_low, before->reachable.vega_high);
  if (delta > b.limits.dollar_delta && delta_raised)
    return {Reason::DELTA_LIMIT, "Worst reachable absolute dollar delta exceeds limit", delta, b.limits.dollar_delta, scope};
  if (vega > b.limits.vega && vega_raised)
    return {Reason::VEGA_LIMIT, "Worst reachable absolute vega per point exceeds limit", vega, b.limits.vega, scope};
  return {};
}
/// The first move from zero, in percent, at which `pnl` loses `room`: a scan in
/// 0.25% steps (1% past 100%) to -99.75% or +1000%, then bisection of the crossing.
/// `sigma` is the log price's standard deviation to when the touch probability is
/// measured (the driftless reflection estimate); zero means no time is left.
std::optional<BreachLevel> solve_level(const std::function<std::optional<double>(double)>& pnl, bool up,
    double room, double spot, std::optional<double> sigma) {
  double prior = 0;
  double crossing = 0;
  bool found = room <= 0;
  for (double distance = 0.25; !found && distance <= (up ? 1000 : 99.75); distance += distance < 100 ? 0.25 : 1) {
    const double shock = up ? distance : -distance;
    const auto loss = pnl(shock);
    if (!loss) return {};
    if (*loss <= -room) { crossing = shock; found = true; break; }
    prior = shock;
  }
  if (!found) return {};
  for (int i = 0; i < 40 && room > 0; ++i) {
    const double middle = (prior + crossing) / 2;
    const auto value = pnl(middle);
    if (!value) return {};
    if (*value <= -room) crossing = middle; else prior = middle;
  }
  BreachLevel level{spot * crossing / 100, crossing, {}};
  if (crossing == 0) level.touch_probability = 1;
  else if (sigma && *sigma > 0)
    level.touch_probability = std::clamp(std::erfc(std::abs(std::log1p(crossing / 100)) /
        (*sigma * std::sqrt(2.0))), 0.0, 1.0);
  else if (sigma) level.touch_probability = 0;  // no session left today
  return level;
}
}  // namespace
Timestamp observation_time(const md::OptionContract& contract, Timestamp now) {
  const auto session = md::trading_session(contract.root, now);
  const auto observed = session.open || session.market_time == md::kInvalidTimestamp ? now : std::min(now, session.market_time);
  // A series can stop trading before settlement while its root keeps trading.
  return std::min(observed, contract.last_trade_time());
}
RiskSnapshot portfolio_risk(const Ledger& ledger, const std::vector<const Order*>& orders,
    const Contracts& contracts,
    const Valuations& valuations, const Limits& limits, Timestamp now,
    const std::map<std::string, double>& stock_prices) {
  RiskSnapshot result;
  result.aggregate.limits = limits.aggregate;
  // Each contract's fresh valuation, looked up once however many orders trade it.
  std::map<std::string, std::optional<Valuation>, std::less<>> usable;
  auto exposure_of = [&](const std::string& symbol, const md::OptionContract& c, Quantity q) -> std::optional<Exposure> {
    auto saved = usable.find(symbol);
    if (saved == usable.end()) {
      const auto it = valuations.find(symbol);
      std::optional<Valuation> value;
      if (now < c.expiry_time() && it != valuations.end() && fresh(it->second, c, now, limits.max_valuation_age)) value = it->second;
      saved = usable.emplace(symbol, std::move(value)).first;
    }
    if (!saved->second) return std::nullopt;
    const auto e = exposure(q, *saved->second);
    if (!finite(e)) return std::nullopt;
    return e;
  };
  auto add_exposure = [&](const std::string& underlying, const std::optional<Exposure>& e, bool pending) {
    auto& bucket = result.underlyings[underlying];
    const auto override = limits.underlying_overrides.find(underlying);
    bucket.limits = override == limits.underlying_overrides.end() ? limits.per_underlying : override->second;
    if (!e) { result.complete = false; return; }
    auto next_bucket = bucket;
    auto next_aggregate = result.aggregate;
    if (!add(next_bucket, *e, pending) || !add(next_aggregate, *e, pending)) {
      result.complete = false;
      return;
    }
    bucket = next_bucket;
    result.aggregate = next_aggregate;
  };
  for (const auto& [symbol, p] : ledger.positions()) add_exposure(p.contract.underlying, exposure_of(symbol, p.contract, p.quantity), false);
  for (const auto& [symbol, stock] : ledger.stocks()) {
    const auto price = stock_prices.find(symbol);
    add_exposure(symbol, price == stock_prices.end() ? std::nullopt
        : std::optional<Exposure>(Exposure{static_cast<double>(stock.shares) * price->second, 0, 0, 0}), false);
  }
  for (const auto* pending : orders) {
    const auto& o = *pending;
    if (!o.open()) continue;
    if (multi_leg(o.request)) {
      // The legs fill together, so a multi-leg order is one pending exposure.
      std::optional<Exposure> sum = Exposure{};
      std::string underlying;
      for (const auto& leg : o.request.legs) {
        const auto it = contracts.find(leg.symbol);
        if (it == contracts.end()) { sum.reset(); break; }
        underlying = it->second.underlying;
        const auto q = o.remaining() * leg.ratio;
        const auto e = exposure_of(leg.symbol, it->second, leg.side == Side::Buy ? q : -q);
        if (!e) { sum.reset(); break; }
        sum->dollar_delta += e->dollar_delta;
        sum->dollar_gamma_1pct += e->dollar_gamma_1pct;
        sum->vega += e->vega;
        sum->theta += e->theta;
      }
      if (underlying.empty()) { result.complete = false; continue; }
      add_exposure(underlying, sum && finite(*sum) ? sum : std::nullopt, true);
      continue;
    }
    const auto it = contracts.find(o.request.symbol);
    if (it == contracts.end()) { result.complete = false; continue; }
    const auto q = o.request.side == Side::Buy ? o.remaining() : -o.remaining();
    add_exposure(it->second.underlying, exposure_of(o.request.symbol, it->second, q), true);
  }
  finish(result.aggregate);
  for (auto& [name, bucket] : result.underlyings) finish(bucket);
  return result;
}
Decision check_exposure(const RiskSnapshot& risk) {
  if (!risk.complete) return {Reason::MISSING_VALUATION, "Fresh complete portfolio and pending-order valuations required", {}, {}, {}};
  for (const auto& [name, bucket] : risk.underlyings) {
    auto result = check_bucket(bucket, name);
    if (!result.ok()) return result;
  }
  return check_bucket(risk.aggregate, "aggregate");
}
Decision check_exposure(const RiskSnapshot& risk, const RiskSnapshot& without) {
  if (!risk.complete) return check_exposure(risk);
  // An underlying the order alone trades had no exposure without it.
  const RiskBucket empty;
  for (const auto& [name, bucket] : risk.underlyings) {
    const auto before = without.underlyings.find(name);
    auto result = check_bucket(bucket, name, before == without.underlyings.end() ? &empty : &before->second);
    if (!result.ok()) return result;
  }
  return check_bucket(risk.aggregate, "aggregate", &without.aggregate);
}
void validate_scenarios(const ScenarioConfig& c) {
  bool valid = !c.spot_percent.empty() && !c.vol_points.empty() &&
               c.spot_percent.size() <= 101 && c.vol_points.size() <= 101 &&
               std::isfinite(c.vol_floor) && c.vol_floor > 0 && c.vol_floor <= 10;
  for (double x : c.spot_percent) valid &= std::isfinite(x) && x > -100 && x <= 1000;
  for (double y : c.vol_points) valid &= std::isfinite(y) && std::abs(y) <= 1000;
  if (!valid) throw TradingError(Reason::INVALID_SCENARIO, "Invalid or oversized spot/vol grid");
}
ScenarioGrid scenario_grid(const Ledger& ledger, const Valuations& valuations,
    const ScenarioConfig& config, Timestamp now, Timestamp max_age, const std::map<std::string, double>& stock_prices) {
  validate_scenarios(config);
  ScenarioGrid result;
  struct ValuedPosition {
    const Position* position;
    const Valuation* valuation;
  };
  std::vector<ValuedPosition> positions;
  positions.reserve(ledger.positions().size());
  for (const auto& [symbol, p] : ledger.positions()) {
    const auto it = valuations.find(symbol);
    if (now >= p.contract.expiry_time() || it == valuations.end() || !fresh(it->second, p.contract, now, max_age)) {
      result.complete = false;  // Validation above guarantees at least one cell.
      continue;
    }
    positions.push_back({&p, &it->second});
  }
  for (double spot : config.spot_percent) {
    for (double vol : config.vol_points) {
      ScenarioCell cell{spot, vol, 0, false};
      for (const auto& [position, valuation] : positions) {
        const auto& p = *position;
        const auto& v = *valuation;
        const double shocked_vol = v.smile_iv + vol / 100;
        cell.clamped |= shocked_vol < config.vol_floor;
        // Avoid subtraction and floor artifacts: zero shock is exactly zero.
        if (spot == 0 && vol == 0) continue;
        const double base = pricing::black_price(p.contract.type, v.forward, p.contract.strike, v.years, v.smile_iv, v.discount);
        const double shocked = pricing::black_price(p.contract.type, v.forward * (1 + spot / 100),
            p.contract.strike, v.years, std::max(config.vol_floor, shocked_vol), v.discount);
        const double pnl = static_cast<double>(p.quantity) * 100 * (shocked - base);
        if (!std::isfinite(pnl) || !std::isfinite(cell.pnl + pnl)) result.complete = false;
        else cell.pnl += pnl;
      }
      // Shares move with the underlying; volatility does not touch them.
      for (const auto& [symbol, stock] : ledger.stocks()) {
        const auto price = stock_prices.find(symbol);
        if (price == stock_prices.end()) {
          result.complete = false;
          continue;
        }
        const double pnl = static_cast<double>(stock.shares) * price->second * spot / 100;
        if (!std::isfinite(pnl) || !std::isfinite(cell.pnl + pnl)) result.complete = false;
        else cell.pnl += pnl;
      }
      result.cells.push_back(cell);
    }
  }
  // An incomplete grid must not look like a valuation of only the covered subset.
  if (!result.complete) for (auto& cell : result.cells) cell.pnl = 0;
  return result;
}
void validate_profile(const ProfileConfig& c) {
  bool valid = !c.percent.empty() && c.percent.size() <= 201 && !c.days.empty() && c.days.size() <= 8 &&
               (c.variances.empty() || c.variances.size() == c.days.size()) &&
               std::isfinite(c.vol_points) && std::abs(c.vol_points) <= 1000 &&
               std::isfinite(c.vol_floor) && c.vol_floor > 0 && c.vol_floor <= 10;
  for (double x : c.percent) valid &= std::isfinite(x) && x > -100 && x <= 1000;
  for (double d : c.days) valid &= std::isfinite(d) && d >= 0 && d <= 366;
  for (const auto& [underlying, beta] : c.betas) valid &= std::isfinite(beta) && std::abs(beta) <= 10;
  for (const auto& variance : c.variances) valid &= !variance || (std::isfinite(*variance) && *variance >= 0);
  if (!valid) throw TradingError(Reason::INVALID_SCENARIO, "Invalid or oversized risk profile");
}
RiskProfile risk_profile(const Ledger& ledger, const Valuations& valuations, const ProfileConfig& config,
    double reference_spot, Money equity, std::optional<Money> floor, std::optional<Money> soft_floor,
    Timestamp now, Timestamp max_age, const std::map<std::string, double>& stock_prices) {
  validate_profile(config);
  RiskProfile result;
  if (floor) result.room = equity - *floor;
  if (soft_floor) result.soft_room = equity - *soft_floor;
  struct Held {
    const Position* position;
    const Valuation* valuation;
    double beta;
  };
  struct Shares {
    double shares, price, beta;
  };
  std::vector<Held> held;
  std::vector<Shares> shares;
  bool clamped = false;
  for (const auto& [symbol, p] : ledger.positions()) {
    const auto beta = config.betas.find(p.contract.underlying);
    const auto it = valuations.find(symbol);
    if (beta == config.betas.end() || now >= p.contract.expiry_time() || it == valuations.end() ||
        !fresh(it->second, p.contract, now, max_age)) {
      result.complete = false;
      continue;
    }
    clamped |= it->second.smile_iv + config.vol_points / 100 < config.vol_floor;
    held.push_back({&p, &it->second, beta->second});
  }
  for (const auto& [symbol, stock] : ledger.stocks()) {
    const auto beta = config.betas.find(symbol);
    const auto price = stock_prices.find(symbol);
    if (beta == config.betas.end() || price == stock_prices.end()) {
      result.complete = false;
      continue;
    }
    shares.push_back({static_cast<double>(stock.shares), price->second, beta->second});
  }
  const auto target = floor ? floor : soft_floor;
  for (std::size_t index = 0; index < config.days.size(); ++index) {
    const double days = config.days[index];
    ProfileCurve curve;
    curve.days = days;
    curve.time = now + std::llround(days * static_cast<double>(md::kNanosPerDay));
    curve.clamped = clamped;
    const double elapsed = days * static_cast<double>(md::kNanosPerDay) / md::kNanosPerYear;
    std::map<double, std::optional<double>> valued;
    const auto pnl = [&](double percent) -> std::optional<double> {
      const auto [it, added] = valued.try_emplace(percent);
      if (!added) return it->second;
      double total = 0;
      for (const auto& [position, valuation, beta] : held) {
        // Avoid subtraction and floor artifacts: no change at all is exactly zero.
        if (percent == 0 && days == 0 && config.vol_points == 0) continue;
        const auto& p = *position;
        const auto& v = *valuation;
        const double move = 1 + std::max(-0.9999, beta * percent / 100);
        // Today's curve prices exactly as the scenario grid does.
        double life = v.years, forward = v.forward, discount = v.discount;
        if (days > 0) {
          life = p.contract.expiry_time() <= curve.time ? 0 : std::max(0.0, v.years - elapsed);
          const double share = v.years > 0 ? life / v.years : 0;
          if (v.spot > 0) forward = v.spot + (v.forward - v.spot) * share;
          if (v.years > 0) discount = std::pow(v.discount, share);
        }
        const double base = pricing::black_price(p.contract.type, v.forward, p.contract.strike, v.years, v.smile_iv, v.discount);
        const double shocked = pricing::black_price(p.contract.type, forward * move, p.contract.strike, life,
            std::max(config.vol_floor, v.smile_iv + config.vol_points / 100), discount);
        total += static_cast<double>(p.quantity) * 100 * (shocked - base);
      }
      for (const auto& stock : shares) total += stock.shares * stock.price * std::max(-0.9999, stock.beta * percent / 100);
      if (std::isfinite(total)) it->second = total;
      return it->second;
    };
    for (double percent : config.percent) {
      const auto value = result.complete ? pnl(percent) : std::nullopt;
      if (!value) result.complete = false;
      curve.pnl.push_back(value.value_or(0));
    }
    if (result.complete && target) {
      std::optional<double> sigma;
      if (index < config.variances.size() && config.variances[index]) sigma = std::sqrt(*config.variances[index]);
      const auto solve = [&](bool up, Money reach) {
        return solve_level(pnl, up, (equity - reach).dollars(), reference_spot, sigma);
      };
      curve.down = solve(false, *target);
      curve.up = solve(true, *target);
      if (floor && soft_floor) {
        curve.soft_down = solve(false, *soft_floor);
        curve.soft_up = solve(true, *soft_floor);
      } else if (soft_floor) {
        curve.soft_down = curve.down;
        curve.soft_up = curve.up;
      }
    }
    result.curves.push_back(std::move(curve));
  }
  // An incomplete profile must not look like a valuation of only the covered subset.
  if (!result.complete) {
    for (auto& curve : result.curves) {
      std::fill(curve.pnl.begin(), curve.pnl.end(), 0.0);
      curve.down.reset(); curve.up.reset(); curve.soft_down.reset(); curve.soft_up.reset();
    }
  }
  return result;
}
BreachRisk breach_risk(const Ledger& ledger, const Valuations& valuations,
    Money equity, std::optional<Money> floor, std::optional<Money> soft_floor, Timestamp now, Timestamp max_age,
    const std::map<std::string, double>& stock_prices, const std::map<std::string, double>& close_variances) {
  BreachRisk result;
  if (floor) result.room = equity - *floor;
  if (soft_floor) result.soft_room = equity - *soft_floor;
  // Levels reach the plan floor, or the soft floor on an account without one.
  const auto target = floor ? floor : soft_floor;
  std::map<std::string, std::map<std::string, Position>> groups;
  for (const auto& [symbol, position] : ledger.positions()) groups[position.contract.underlying][symbol] = position;
  for (const auto& [symbol, stock] : ledger.stocks()) groups.try_emplace(symbol);
  for (const auto& [underlying, positions] : groups) {
    UnderlyingBreach item;
    item.underlying = underlying;
    std::map<std::string, StockPosition> stocks;
    if (const auto stock = ledger.stocks().find(underlying); stock != ledger.stocks().end()) stocks.emplace(*stock);
    const auto subset = Ledger::restore({}, positions, stocks);
    Timestamp spot_time = -1;
    for (const auto& [symbol, position] : positions) {
      const auto v = valuations.find(symbol);
      if (v != valuations.end() && fresh(v->second, position.contract, now, max_age) && v->second.time > spot_time) {
        item.spot = v->second.spot;
        spot_time = v->second.time;
      }
    }
    if (const auto price = stock_prices.find(underlying); price != stock_prices.end()) item.spot = price->second;
    if (const auto variance = close_variances.find(underlying);
        variance != close_variances.end() && std::isfinite(variance->second) && variance->second >= 0)
      item.close_sigma = std::sqrt(variance->second);
    ScenarioConfig scan;
    scan.vol_points = {0};
    // The plan and soft floors' scans visit the same moves; each is valued once.
    std::map<double, std::optional<double>> valued;
    const auto pnl = [&](double percent) -> std::optional<double> {
      const auto [it, added] = valued.try_emplace(percent);
      if (!added) return it->second;
      scan.spot_percent = {percent};
      const auto grid = scenario_grid(subset, valuations, scan, now, max_age, stock_prices);
      if (grid.complete) it->second = grid.cells.front().pnl;
      return it->second;
    };
    item.complete = item.spot > 0 && pnl(0).has_value();
    result.complete &= item.complete;
    if (item.complete && target) {
      const auto solve = [&](bool up, Money reach) {
        return solve_level(pnl, up, (equity - reach).dollars(), item.spot, item.close_sigma);
      };
      item.down = solve(false, *target);
      item.up = solve(true, *target);
      if (floor && soft_floor) {
        item.soft_down = solve(false, *soft_floor);
        item.soft_up = solve(true, *soft_floor);
      } else if (soft_floor) {
        item.soft_down = item.down;
        item.soft_up = item.up;
      }
    }
    result.underlyings.push_back(std::move(item));
  }
  return result;
}
}  // namespace openport::trading
