#pragma once

#include "openport/trading/evaluation.hpp"
#include "openport/trading/risk.hpp"

namespace openport::trading {
struct FactorShock {
  double spot_percent = 0;
  std::map<double, double> vol_points;  ///< Calendar-day tenor to additive vol points; linear interpolation, flat ends.
};
struct HistoricalStress {
  std::string id, name, from, to, label, source_note;
  std::vector<std::string> sources;
  std::map<std::string, FactorShock> factors;
};
[[nodiscard]] const std::vector<HistoricalStress>& historical_stresses();
struct ShockResult {
  bool complete = true;
  double pnl = 0;
  bool clamped = false;
  std::optional<bool> floor_breach;
  std::optional<Money> margin;
};
/// Reprice the held book with the profile's sticky-strike model. Missing factors
/// invalidate the whole result. Margin is recomputed for instantaneous shocks only.
[[nodiscard]] ShockResult stress_book(const Ledger& ledger, const Valuations& valuations,
    const std::map<std::string, FactorShock>& factors, double days, Money equity,
    std::optional<Money> floor, Timestamp now, Timestamp max_age,
    const std::map<std::string, double>& stock_prices, const AccountRules& rules = {});
struct HistoricalClose {
  md::Date date;
  double close = 0;
  std::optional<double> atm_iv;  ///< Decimal 30-day ATM IV, observed on this date.
};
struct TailRisk {
  std::size_t observations = 0;
  bool complete = false;
  std::optional<double> var95, es95, var99, es99;
  std::optional<md::Date> first, last;
};
/// Empirical inverse-CDF VaR and fractional upper-tail mean ES, losses floored at zero.
[[nodiscard]] TailRisk loss_quantiles(std::vector<double> losses);
struct HistoricalRisk { TailRisk sticky_strike, atm_vol_change; };
/// At most 252 closes per held factor, matching both endpoints of each return.
/// Requires 20 paired daily returns. No resampling, forward filling or beta proxies.
[[nodiscard]] HistoricalRisk historical_risk(const Ledger& ledger, const Valuations& valuations,
    const std::map<std::string, std::vector<HistoricalClose>>& history, Timestamp now,
    Timestamp max_age, const std::map<std::string, double>& stock_prices);
}  // namespace openport::trading
