#include "openport/trading/stress.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>
#include <nlohmann/json.hpp>

#include "openport/pricing/black.hpp"
#include "stress_data.hpp"

namespace openport::trading {
namespace {
double vol_at(const FactorShock& factor, double days) {
  if (factor.vol_points.empty()) return 0;
  const auto high = factor.vol_points.lower_bound(days);
  if (high == factor.vol_points.begin()) return high->second;
  if (high == factor.vol_points.end()) return factor.vol_points.rbegin()->second;
  const auto low = std::prev(high);
  return low->second + (high->second - low->second) * (days - low->first) / (high->first - low->first);
}
std::set<std::string> held_underlyings(const Ledger& ledger) {
  std::set<std::string> held;
  for (const auto& [symbol, p] : ledger.positions()) held.insert(p.contract.underlying);
  for (const auto& [symbol, p] : ledger.stocks()) held.insert(symbol);
  return held;
}
}
const std::vector<HistoricalStress>& historical_stresses() {
  static const auto scenarios = [] {
    std::vector<HistoricalStress> result;
    for (const auto& row : nlohmann::json::parse(kStressData)) {
      HistoricalStress stress;
      stress.id = row.at("id"); stress.name = row.at("name");
      stress.from = row.at("from_date"); stress.to = row.at("to_date");
      stress.label = row.at("label"); stress.source_note = row.at("source_note");
      stress.sources = row.at("sources").get<std::vector<std::string>>();
      for (const auto& [symbol, value] : row.at("factors").items()) {
        auto& factor = stress.factors[symbol];
        factor.spot_percent = value.at("spot_percent");
        for (const auto& vol : value.at("vol_points")) factor.vol_points[vol.at("days").get<double>()] = vol.at("points");
      }
      result.push_back(std::move(stress));
    }
    return result;
  }();
  return scenarios;
}
ShockResult stress_book(const Ledger& ledger, const Valuations& valuations,
    const std::map<std::string, FactorShock>& factors, double days, Money equity,
    std::optional<Money> floor, Timestamp now, Timestamp max_age,
    const std::map<std::string, double>& stock_prices, const AccountRules& rules) {
  if (!std::isfinite(days) || days < 0 || days > 366) throw TradingError(Reason::INVALID_SCENARIO, "Shock days must be 0-366");
  ShockResult result;
  try {
    std::vector<MarginLeg> legs;
    std::vector<MarginStock> stocks;
    // Series that deliver cash only owe a fixed amount: a short requires its buy-back
    // value, its discounted payoff, whatever the shock, as account margin has it.
    Money cash_only;
    Valuations shocked_values;
    std::map<std::string, double> shocked_prices;
    for (const auto& symbol : held_underlyings(ledger)) {
      const auto f = factors.find(symbol);
      if (f == factors.end() || !std::isfinite(f->second.spot_percent) || f->second.spot_percent <= -100 || f->second.spot_percent > 1000) {
        result.complete = false; break;
      }
      for (const auto& [tenor, points] : f->second.vol_points)
        if (!std::isfinite(tenor) || tenor < 0 || !std::isfinite(points) || std::abs(points) > 1000) result.complete = false;
    }
    if (!result.complete) return result;
    for (const auto& [symbol, p] : ledger.positions()) {
      const auto v = valuations.find(symbol);
      if (v == valuations.end()) { result.complete = false; break; }
      const auto& factor = factors.at(p.contract.underlying);
      ProfileConfig profile;
      profile.percent = {factor.spot_percent}; profile.days = {days};
      profile.betas[p.contract.underlying] = 1;
      profile.vol_points = vol_at(factor, std::max(0.0, v->second.years * 365 - days));
      const auto valued = risk_profile(Ledger::restore({}, {{symbol, p}}, {}), valuations,
          profile, 0, {}, {}, {}, now, max_age);
      if (!valued.complete) { result.complete = false; break; }
      result.pnl += valued.curves[0].pnl[0];
      result.clamped |= valued.curves[0].clamped;
      if (days == 0) {
        auto shocked = v->second;
        shocked.spot *= 1 + factor.spot_percent / 100;
        shocked.forward *= 1 + factor.spot_percent / 100;
        shocked.smile_iv = std::max(profile.vol_floor, shocked.smile_iv + profile.vol_points / 100);
        const auto e = equivalent(symbol, p.contract);
        if (e.contracts == 0) {
          if (p.quantity < 0)
            cash_only = cash_only + Money::from_double(shocked.discount * e.payoff * 100 * std::abs(static_cast<double>(p.quantity)));
          continue;
        }
        const double price = e.contracts * pricing::black_price(p.contract.type, shocked.forward, e.strike,
            shocked.years, shocked.smile_iv, shocked.discount);
        legs.push_back({p.contract, p.quantity, Money::from_double(price * 100 * std::abs(static_cast<double>(p.quantity))), shocked.spot});
        shocked_values[symbol] = shocked;
      }
    }
    for (const auto& [symbol, p] : ledger.stocks()) {
      const auto price = stock_prices.find(symbol);
      if (price == stock_prices.end() || !std::isfinite(price->second) || price->second <= 0) { result.complete = false; break; }
      const double move = factors.at(symbol).spot_percent / 100;
      result.pnl += static_cast<double>(p.shares) * price->second * move;
      shocked_prices[symbol] = price->second * (1 + move);
      stocks.push_back({symbol, p.shares, Money::from_double(std::abs(static_cast<double>(p.shares)) * shocked_prices[symbol])});
    }
    result.complete &= std::isfinite(result.pnl);
    if (!result.complete) { result.pnl = 0; return result; }
    if (floor) result.floor_breach = equity.dollars() + result.pnl <= floor->dollars();
    if (days == 0) {
      if (rules.margin == MarginMode::Portfolio)
        result.margin = portfolio_margin_requirement(legs, shocked_values, now, max_age, ledger.stocks(), shocked_prices, margin_policy(rules));
      else result.margin = margin_requirement(legs, stocks, margin_policy(rules));
      if (result.margin) result.margin = *result.margin + cash_only;
    }
    return result;
  } catch (const TradingError& error) {
    if (error.code() != Reason::ARITHMETIC_OVERFLOW) throw;
    ShockResult unavailable;
    unavailable.complete = false;
    return unavailable;
  }
}
TailRisk loss_quantiles(std::vector<double> losses) {
  TailRisk result;
  result.observations = losses.size();
  if (std::any_of(losses.begin(), losses.end(), [](double x) { return !std::isfinite(x); })) {
    result.tail99_reason = "A non-finite historical loss";
    return result;
  }
  if (losses.size() < 100)
    result.tail99_reason = "Needs at least 100 observations; " + std::to_string(losses.size()) + " available";
  if (losses.size() < 20) return result;
  std::sort(losses.begin(), losses.end());
  const auto tail = [&](double confidence) {
    const double n = static_cast<double>(losses.size());
    const auto index = static_cast<std::size_t>(std::ceil(confidence * n)) - 1;
    const double mass = (1 - confidence) * n;
    double sum = 0, left = mass;
    for (auto it = losses.rbegin(); it != losses.rend() && left > 1e-12; ++it) {
      const double weight = std::min(1.0, left);
      sum += weight * *it; left -= weight;
    }
    return std::pair(std::max(0.0, losses[index]), std::max(0.0, sum / mass));
  };
  const auto [v95, e95] = tail(0.95);
  result.var95 = v95; result.es95 = e95; result.complete = true;
  if (losses.size() >= 100) {
    const auto [v99, e99] = tail(0.99);
    result.var99 = v99; result.es99 = e99;
  }
  return result;
}
HistoricalRisk historical_risk(const Ledger& ledger, const Valuations& valuations,
    const std::map<std::string, std::vector<HistoricalClose>>& history, Timestamp now,
    Timestamp max_age, const std::map<std::string, double>& stock_prices) {
  using Pair = std::pair<md::Date, md::Date>;
  struct Move { double percent; std::optional<double> vol; };
  std::map<std::string, std::map<Pair, Move>> moves;
  const auto held = held_underlyings(ledger);
  if (held.empty()) return {};
  const auto today = md::new_york_time(now).date;
  for (const auto& symbol : held) {
    const auto source = history.find(symbol);
    if (source == history.end()) return {};
    std::map<md::Date, HistoricalClose> closes;
    for (const auto& c : source->second)
      if (c.date < today && std::isfinite(c.close) && c.close > 0) closes[c.date] = c;
    while (closes.size() > 252) closes.erase(closes.begin());
    auto& returns = moves[symbol];
    if (closes.size() < 2) continue;
    for (auto it = std::next(closes.begin()); it != closes.end(); ++it) {
      const auto& a = std::prev(it)->second;
      const auto& b = it->second;
      bool consecutive = md::days_since_epoch(b.date) - md::days_since_epoch(a.date) <= 7;
      for (auto day = md::days_since_epoch(a.date) + 1; consecutive && day < md::days_since_epoch(b.date); ++day)
        if (md::stock_session(md::new_york_to_utc(md::date_from_days(day), 12, 0)).open) consecutive = false;
      if (!consecutive) continue;
      Move move{100 * (b.close / a.close - 1), {}};
      if (a.atm_iv && b.atm_iv && std::isfinite(*a.atm_iv) && std::isfinite(*b.atm_iv) && *a.atm_iv > 0 && *b.atm_iv > 0)
        move.vol = 100 * (*b.atm_iv - *a.atm_iv);
      returns[{a.date, b.date}] = move;
    }
  }
  std::vector<double> sticky, vol;
  std::optional<md::Date> first, last, vol_first, vol_last;
  for (const auto& [dates, move] : moves.begin()->second) {
    std::map<std::string, FactorShock> factors;
    bool paired = true, has_vol = true;
    for (const auto& symbol : held) {
      const auto value = moves[symbol].find(dates);
      if (value == moves[symbol].end()) { paired = false; break; }
      factors[symbol].spot_percent = value->second.percent;
      has_vol &= value->second.vol.has_value();
    }
    if (!paired) continue;
    const auto base = stress_book(ledger, valuations, factors, 1, {}, {}, now, max_age, stock_prices);
    if (!base.complete) return {};
    sticky.push_back(-base.pnl); if (!first) first = dates.second; last = dates.second;
    if (has_vol) {
      for (const auto& symbol : held) factors[symbol].vol_points[30] = *moves[symbol].at(dates).vol;
      const auto variant = stress_book(ledger, valuations, factors, 1, {}, {}, now, max_age, stock_prices);
      if (!variant.complete) continue;
      vol.push_back(-variant.pnl); if (!vol_first) vol_first = dates.second; vol_last = dates.second;
    }
  }
  HistoricalRisk result{loss_quantiles(std::move(sticky)), loss_quantiles(std::move(vol))};
  result.sticky_strike.first = first; result.sticky_strike.last = last;
  result.atm_vol_change.first = vol_first; result.atm_vol_change.last = vol_last;
  return result;
}
}  // namespace openport::trading
