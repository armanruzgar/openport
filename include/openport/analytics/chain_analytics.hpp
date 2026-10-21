#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "openport/analytics/chain_book.hpp"
#include "openport/analytics/forward.hpp"
#include "openport/pricing/american.hpp"

namespace openport::analytics {

inline constexpr int kDeamericanizationSteps = 31;
// Continuous dividend/borrow yield safeguards; not a cash-dividend forecast.
inline constexpr double kMinTreeDividend = -0.05;
inline constexpr double kMaxTreeDividend = 0.20;
inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

/// OpenPort's own numbers for one option: the same for every provider.
/// Greeks are per unit of underlying (multiply by the contract multiplier for a
/// position), with delta and gamma taken with respect to spot.
struct OptionMetrics {
  md::OptionContract contract;
  double bid_size = kNaN;
  double ask_size = kNaN;
  md::InstrumentId id = kNoInstrument;
  double bid = kNaN;
  double ask = kNaN;
  double mid = kNaN;
  double eep = kNaN;  ///< EEP removed for IV solves only; displayed quotes remain raw
  double iv = kNaN;   ///< implied from mid minus eep when available
  double bid_iv = kNaN;
  double ask_iv = kNaN;
  double delta = kNaN;
  double gamma = kNaN;  ///< per $1 of spot
  double vega = kNaN;   ///< per vol point
  double theta = kNaN;  ///< per calendar day
  double vanna = kNaN;  ///< change in delta per vol point
  double volume = kNaN;  ///< cumulative contracts for the current trading date
  double open_interest = kNaN;
  bool has_open_interest = false;
  double vendor_iv = kNaN;
};

/// Signed OI-weighted dollar sensitivities; see docs/methods.md for units.
struct ExposureValues {
  double gex = 0.0;
  double vex = 0.0;
  double dex = 0.0;
  double cex = 0.0;
  double vomex = 0.0;
};
inline constexpr std::array kExposureNames = {"gex", "vex", "dex", "cex", "vomex"};
inline constexpr std::array kExposureFields = {
    &ExposureValues::gex, &ExposureValues::vex, &ExposureValues::dex,
    &ExposureValues::cex, &ExposureValues::vomex};

/// LongCallsShortPuts is the default; "open_interest" is its deprecated spelling.
enum class ExposureConvention { LongCallsShortPuts, ShortAll, CustomerFlow };
enum class ExposureSlice { All, ZeroDte, ExZeroDte };
[[nodiscard]] const char* to_string(ExposureConvention convention);
[[nodiscard]] const char* to_string(ExposureSlice slice);
/// Accepts the deprecated "open_interest" as LongCallsShortPuts; nullopt if unknown.
[[nodiscard]] std::optional<ExposureConvention> parse_exposure_convention(std::string_view name);

struct ExposureSettings {
  double min_days = 0.5;
  double flip_range = 0.10;
  int flip_steps = 81;
};

struct StrikeMetrics : ExposureValues {
  double strike = 0.0;
  double iv = kNaN;  ///< smile IV, taken from the out-of-the-money side
  OptionMetrics call;
  OptionMetrics put;
};

/// Counts over standard contracts in live expiries; quoted/OI count receipt,
/// priced counts a successful IV solve, independently of the other side's IV.
struct Coverage {
  int options = 0;
  int quoted = 0;
  int priced = 0;
  int open_interest = 0;
  int volume = 0;
};

/// Known cash per share for the underlying being analysed. The ex-date starts
/// at midnight New York time, so ex-date quotes no longer include the payment.
struct Dividend {
  md::Date ex_date;
  double amount;
};

struct SliceMetrics : ExposureValues {
  md::Date expiry;
  md::Timestamp expiry_time = 0;
  std::string root;
  pricing::ExerciseStyle style = pricing::ExerciseStyle::European;
  Coverage coverage;
  double years = 0.0;
  ForwardEstimate forward;
  std::string rate_source = "assumed";  ///< parity, term, curve, or assumed
  std::string rate_curve_symbol;
  bool deamericanized = false;
  /// The EEP model the correction used: the configured one, or Leisen-Reimer
  /// when cash dividends or any contract's inputs sent the expiry to the lattice.
  pricing::AmericanModel eep_model = pricing::AmericanModel::LeisenReimer;
  std::vector<Dividend> dividends;  ///< cash payments used in the EEP correction
  double atm_iv = kNaN;
  std::vector<StrikeMetrics> strikes;
};

/// Sticky-strike spot crossing and signed strike extrema, absent when not found.
struct ExposureLevels {
  double flip = kNaN;
  double positive_wall = kNaN;
  double negative_wall = kNaN;
};

/// Default positioning is long calls / short puts; these are model assumptions,
/// not knowledge of anyone's actual positions. Levels follow kExposureNames order.
struct ExposureSummary : ExposureValues {
  /// Fraction of options at finite-smile-IV strikes with received OI; NaN if none.
  double oi_coverage = kNaN;
  bool available = true;
  std::string unavailable_reason;
  std::array<ExposureLevels, kExposureNames.size()> levels;
  double gamma_flip = kNaN;  ///< spot level where total GEX changes sign, nearest to spot
  double call_wall = kNaN;   ///< strike with the largest positive GEX, all expiries combined
  double put_wall = kNaN;    ///< strike with the most negative GEX, all expiries combined
};

struct UnderlyingMetrics {
  std::string symbol;
  double spot = kNaN;
  std::string spot_source;  ///< "quote", "parity", or empty when unavailable
  bool american_approximation = false;
  Coverage coverage;
  md::Timestamp as_of = 0;
  std::uint64_t version = 0;  ///< version of the book this was computed from
  std::vector<SliceMetrics> slices;
  ExposureSummary exposure;
  ExposureSettings exposure_settings;  ///< snapshot's floor and flip search, reused by API views
  ExposureConvention exposure_convention = ExposureConvention::LongCallsShortPuts;
  ExposureSlice exposure_slice = ExposureSlice::All;
  int options_priced = 0;
  double compute_ms = 0.0;
};

/// Selects expiries and recomputes exposure from an immutable priced snapshot.
/// Does not refit IVs, change market time, or alter the source snapshot.
[[nodiscard]] UnderlyingMetrics exposure_view(
    const UnderlyingMetrics& metrics,
    ExposureConvention convention = ExposureConvention::LongCallsShortPuts,
    ExposureSlice slice = ExposureSlice::All);

/// Continuously compounded zero rates, linear in T with flat end extrapolation.
/// Construction rejects invalid points and requires two distinct positive tenors.
class DiscountCurve {
 public:
  static std::shared_ptr<const DiscountCurve> from_points(
      std::string symbol, std::vector<std::pair<double, double>> points);
  [[nodiscard]] double rate(double years) const;
  [[nodiscard]] const std::string& symbol() const { return symbol_; }

 private:
  DiscountCurve() = default;
  std::string symbol_;
  std::vector<std::pair<double, double>> points_;
};

/// Only European expiries whose own parity fit supplied D enter the curve.
[[nodiscard]] std::shared_ptr<const DiscountCurve> make_discount_curve(const UnderlyingMetrics& m);

struct AnalyticsOptions {
  int parity_strikes = 12;   ///< strikes nearest the money used to fit the forward
  double flip_range = 0.10;  ///< search for exposure flips within +-10% of spot
  int flip_steps = 81;
  double fallback_rate = 0.04;  ///< flat rate when no eligible market-implied rate exists
  std::shared_ptr<const DiscountCurve> discount_curve;
  bool deamericanize = true;
  /// How the EEP is estimated. Leisen-Reimer stays the default: the closed forms
  /// are faster but not equivalent within the LR tolerance (docs/american-analytics.md).
  pricing::AmericanModel american_model = pricing::AmericanModel::LeisenReimer;
  std::vector<Dividend> dividends;  ///< this underlying's known cash schedule

  /// Expiries shorter than this take their discount rate from the longer expiries
  /// (the median of their fitted rates): over a few days D is within a basis point
  /// of 1, so bid/ask noise swamps the parity slope that would otherwise estimate it.
  /// American tree dividend/borrow yield also takes the median implied q of longer
  /// expiries below this threshold, falling back to own q when none exists. Tree
  /// q is clamped to [-5%, 20%] to limit annualisation of spot/option clock noise.
  /// With known cash before expiry, residual q instead preserves the own-tenor
  /// parity forward on the escrowed spot, without borrowing or clamping.
  double min_days_for_rate = 30.0;

  /// All exposure sensitivities and flips use at least this much time to expiry. Local
  /// gamma of an option minutes from expiry explodes but only holds over a tiny move,
  /// so it would drown out everything else. Displayed Greeks use the true time.
  double exposure_min_days = 0.5;

  /// The provider's underlying print is used as spot only while it is at most this
  /// much older than the option data; otherwise spot is inferred from parity. Stock
  /// and ETF prints stop at 16:00 ET while their options quote until 16:15, so the
  /// limit sits above that gap; an index frozen at the close during Cboe's overnight
  /// session is hours behind its options and falls back to parity.
  double max_spot_age_minutes = 30.0;
};

/// Prices one underlying's whole chain as of `as_of` (the data's own time, not the
/// wall clock, so delayed feeds get the right time to expiry).
///
/// European expiries fit parity rates; short expiries borrow their median level.
/// American expiries use the supplied European curve or fallback_rate, never a
/// parity slope. One de-Americanisation pass removes same-tree Leisen-Reimer EEP
/// (or, when configured, a closed-form EEP; see AnalyticsOptions::american_model)
/// at first-pass IV/carry, then refits F and Black-76 IVs. One iteration suffices
/// for the measured S=100, r=4.5%, q=1%, vol=20%, T={.25,1} grid: maximum forward
/// residual 0.0271%, OTM IV residual 0.0870 vol points with 31 steps. See
/// docs/american-analytics.md for EEP convergence and the performance measurement.
/// Greeks remain European Black-76 Greeks at the final smile IV, accurate out of
/// the money for American contracts. A single spot (quote no more than
/// max_spot_age_minutes behind option data, else nearest parity F*D) anchors all exposures. Every side with received,
/// finite nonnegative OI at a finite-smile-IV strike contributes exposure; priced coverage uses own
/// IV.
[[nodiscard]] UnderlyingMetrics analyze(const UnderlyingBook& book, const ChainBook& chain,
                                        md::Timestamp as_of, const AnalyticsOptions& options = {});

}  // namespace openport::analytics
