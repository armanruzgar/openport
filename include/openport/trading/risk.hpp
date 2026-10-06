#pragma once

#include "openport/trading/ledger.hpp"

namespace openport::trading {

struct ExposureRange {
  double delta_low = 0;
  double delta_high = 0;
  double vega_low = 0;
  double vega_high = 0;
};
struct RiskBucket {
  Exposure position;
  ExposureRange reachable;  ///< Any subset of open orders, independently per factor.
  ExposureLimits limits;
  double delta_utilisation = 0;
  double vega_utilisation = 0;
};
struct RiskSnapshot {
  RiskBucket aggregate;
  std::map<std::string, RiskBucket> underlyings;
  bool complete = true;
  Money daily_loss;  ///< max(0, start-of-day equity - equity).
  bool kill_latched = false;
  std::string kill_reason;
  std::uint64_t limits_revision = 1;
};

/// The market time a contract's quotes and valuations must be recent to: `now`
/// while one of its sessions is open, otherwise the end of its last session, so
/// a closed market's close stays current until it reopens (an SPY position
/// overnight, while SPX trades). Capped at the contract's last trade, even when
/// its root has later sessions before settlement.
[[nodiscard]] Timestamp observation_time(const md::OptionContract& contract, Timestamp now);

/// Shares count at their dollar delta, at `stock_prices` (fresh prices by
/// underlying); a holding without one leaves the result incomplete. `orders` are
/// the pending orders, by pointer so that a book of many need not be copied.
[[nodiscard]] RiskSnapshot portfolio_risk(
    const Ledger& ledger, const std::vector<const Order*>& orders, const Contracts& contracts,
    const Valuations& valuations, const Limits& limits, Timestamp now,
    const std::map<std::string, double>& stock_prices = {});
[[nodiscard]] Decision check_exposure(const RiskSnapshot& risk);
/// One order's check: `risk` with the order among the pending ones, `without`
/// the same book without it. A bucket over its limit refuses the order only when
/// the order raises that bucket's worst reachable exposure, so a book already
/// over a limit can still close, hedge and trade other underlyings.
[[nodiscard]] Decision check_exposure(const RiskSnapshot& risk, const RiskSnapshot& without);

struct ScenarioCell {
  double spot_percent = 0;
  double vol_points = 0;
  double pnl = 0;  ///< Analytical dollars, not a booked cash amount.
  bool clamped = false;
};
struct ScenarioGrid {
  std::vector<ScenarioCell> cells;  ///< Spot-major, vol-minor.
  bool complete = true;
};
void validate_scenarios(const ScenarioConfig& config);
[[nodiscard]] ScenarioGrid scenario_grid(const Ledger& ledger,
    const Valuations& valuations, const ScenarioConfig& config,
    Timestamp now, Timestamp max_age, const std::map<std::string, double>& stock_prices = {});

struct BreachLevel {
  double points = 0;
  double percent = 0;
  std::optional<double> touch_probability;
};
struct UnderlyingBreach {
  std::string underlying;
  double spot = 0;
  /// One standard deviation of the log price to today's regular close, from the
  /// market's implied variance (see Engine's close_variance).
  std::optional<double> close_sigma;
  std::optional<BreachLevel> down;
  std::optional<BreachLevel> up;
  /// Where equity would reach the personal soft floor, which liquidates first
  /// when it is nearer than the plan floor; the same as down/up without a plan floor.
  std::optional<BreachLevel> soft_down;
  std::optional<BreachLevel> soft_up;
  bool complete = false;
};
struct BreachRisk {
  std::optional<Money> room;
  std::optional<Money> soft_room;
  bool complete = true;
  std::vector<UnderlyingBreach> underlyings;
};
/// Something about the held book worth acting on before the market or the night
/// acts on it. `actual` and `limit` are numbers whose meaning depends on the code
/// (see TradingSession::warnings).
struct RiskWarning {
  std::string code;
  std::string severity;  ///< "warning": it costs room, buying power or a fill soon; "info": worth knowing.
  std::string scope;     ///< The underlying, or "aggregate" for the account.
  std::string symbol;    ///< The contract it is about, if one; empty otherwise.
  std::string message;
  std::optional<double> actual;
  std::optional<double> limit;
};

/// A risk profile of the held book: its value across moves of a reference price
/// on several dates. Each held underlying moves `betas[underlying]` times the
/// reference's percent move (1 for the reference itself), so a book of several
/// underlyings is weighted to one benchmark.
struct ProfileConfig {
  std::vector<double> percent;          ///< The reference's moves, in percent.
  std::vector<double> days{0};          ///< Each curve's date, in calendar days after `now`.
  double vol_points = 0;                ///< Added to every contract's smile IV.
  double vol_floor = 0.0001;
  std::map<std::string, double> betas;  ///< By underlying; one held without a beta leaves the profile incomplete.
  /// For each curve, the variance of the reference's log price to the date its levels'
  /// touch probabilities are measured to; empty or nullopt leaves them out.
  std::vector<std::optional<double>> variances;
};
struct ProfileCurve {
  double days = 0;
  Timestamp time = 0;        ///< now + days.
  std::vector<double> pnl;   ///< By move; analytical dollars from the book's value now. Zero when incomplete.
  bool clamped = false;      ///< A shocked volatility met the floor.
  /// Reference levels where equity reaches the plan floor (or the soft floor without
  /// one), and the soft floor's own, as BreachRisk gives them for today.
  std::optional<BreachLevel> down;
  std::optional<BreachLevel> up;
  std::optional<BreachLevel> soft_down;
  std::optional<BreachLevel> soft_up;
};
struct RiskProfile {
  std::optional<Money> room;
  std::optional<Money> soft_room;
  std::vector<ProfileCurve> curves;
  bool complete = true;
};
void validate_profile(const ProfileConfig& config);
/// Prices each contract with Black-76 at the curve's date: remaining life
/// T' = T - days (zero once it has expired, leaving intrinsic value), the forward's
/// carry over spot shrinking in proportion, F' = (1 + beta x)(S + (F - S) T'/T),
/// D' = D^(T'/T) and sigma' = max(vol_floor, smile IV + vol_points / 100). Shares
/// move by beta x. Today's curve with no volatility change is the scenario grid's
/// zero-volatility column, and its levels are breach_risk's. `reference_spot` turns
/// percent levels into price points.
[[nodiscard]] RiskProfile risk_profile(const Ledger& ledger, const Valuations& valuations,
    const ProfileConfig& config, double reference_spot, Money equity, std::optional<Money> floor,
    std::optional<Money> soft_floor, Timestamp now, Timestamp max_age,
    const std::map<std::string, double>& stock_prices = {});

/// Every held underlying, with levels where equity reaches the plan floor, or the
/// soft floor on an account without one, and separately the soft floor's levels;
/// with neither floor there are no levels.
/// One underlying moves at a time, today's vol and remaining option life held
/// fixed. Search to -99.75% / +1000%, then bisect the first crossing. Missing
/// ATM vol leaves the driftless log-return reflection estimate absent.
[[nodiscard]] BreachRisk breach_risk(const Ledger& ledger,
    const Valuations& valuations, Money equity,
    std::optional<Money> floor, std::optional<Money> soft_floor, Timestamp now, Timestamp max_age,
    const std::map<std::string, double>& stock_prices = {}, const std::map<std::string, double>& close_variances = {});

}  // namespace openport::trading
