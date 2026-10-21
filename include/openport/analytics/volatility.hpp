#pragma once

#include <array>
#include <istream>
#include <span>

#include "openport/analytics/svi.hpp"

namespace openport::analytics {

inline constexpr double kMinutesPerYear = 365.0 * 1440.0;
inline constexpr std::array<int, 5> kMfivDays{9, 30, 93, 182, 365};
inline constexpr std::array<int, 5> kAtmDays{7, 30, 60, 90, 180};
inline constexpr const char* kDeltaConvention = "Black-76 forward, premium-unadjusted, undiscounted";

struct Mfiv {
  double variance = kNaN;
  double vol = kNaN;  ///< All reported vols, RR and BF are in vol points.
  double k0 = kNaN;
  double low = kNaN;
  double high = kNaN;
  std::size_t strikes = 0;
  std::string lower_stop = "window";
  std::string upper_stop = "window";
  bool truncated = true;
  bool proxy = false;
  std::string eep = "not_applicable";  ///< removed, partial, unavailable
  std::string reason;
};
[[nodiscard]] Mfiv model_free_iv(const SliceMetrics& slice);

struct DeltaPoint {
  double strike = kNaN;
  double vol = kNaN;
  std::string reason;
};
struct DeltaSkew {
  DeltaPoint call;
  DeltaPoint put;
  double rr = kNaN;
  double bf = kNaN;
};
struct SmileMetrics {
  double atm = kNaN;
  double slope = kNaN;      ///< Vol points per unit log-moneyness.
  double curvature = kNaN;
  std::string source = "smile_interpolation";
  std::string reason;
  DeltaSkew delta25;
  DeltaSkew delta10;
};
/// SVI is used only inside its checked quote range. Otherwise interpolate IV in k;
/// never extend the observed smile. vendor=true uses OTM vendor_iv, without a fit.
[[nodiscard]] SmileMetrics smile_metrics(const SliceMetrics& slice, const SviFit* fit = nullptr,
                                         bool vendor = false);

struct VarianceKnot {
  double minutes = 0.0;
  double variance = kNaN;
  bool truncated = false;
  bool proxy = false;
};
struct ConstantVol {
  double days = 0;
  double variance = kNaN;
  double vol = kNaN;
  std::size_t near = 0;
  std::size_t next = 0;
  bool truncated = false;
  bool proxy = false;
  std::string reason;
};
/// Exact tenors are allowed; otherwise require two listed tenors, both usable.
[[nodiscard]] ConstantVol constant_vol(std::span<const VarianceKnot> knots, double days,
                                       double minimum_minutes = 0.0);

struct ExpiryVolatility {
  Mfiv mfiv;
  SmileMetrics smile;
  double minutes = 0.0;
};
struct VolatilityMetrics {
  std::vector<ExpiryVolatility> expiries;  ///< Same order as the input slices.
  std::vector<ConstantVol> mfiv;
  std::vector<ConstantVol> atm;
  DeltaSkew skew25;
  DeltaSkew skew10;
  std::string skew_reason;
  bool skew_proxy = false;
  double ratio9_30 = kNaN;
  double ratio30_93 = kNaN;
  double atm30_7 = kNaN;
};
/// Fits are supplied by the caller's lazy cache. Empty fits mean interpolate quotes.
[[nodiscard]] VolatilityMetrics volatility_metrics(const UnderlyingMetrics& metrics,
                                                    std::span<const SviFit> fits = {},
                                                    bool vendor = false);

struct VarianceRiskPremium {
  double spread = kNaN;
  double ratio = kNaN;
  bool truncated = false;
  bool proxy = false;
  std::string reason;
};
[[nodiscard]] VarianceRiskPremium variance_risk_premium(const ConstantVol& implied, double realized);
/// ATM IV to the close of the Nth coming business session, including today if open later.
[[nodiscard]] ConstantVol session_implied_vol(const VolatilityMetrics& volatility,
                                              md::Timestamp as_of, int sessions);

struct EventLabel {
  md::Date date;
  std::string label;
  std::string timing = "session";  ///< session, before_open, after_close, or exact.
  md::Timestamp time = 0;  ///< Only for an explicitly timed event; never an inferred print.
};
[[nodiscard]] std::vector<EventLabel> parse_events(std::istream& input);
[[nodiscard]] md::Date event_session(const EventLabel& event);
struct EventVolatility {
  EventLabel event;
  md::Date session;
  std::size_t near = 0, next = 0;
  bool bracketed = false;
  double baseline_variance = kNaN;  ///< Per full business session.
  double variance_premium = kNaN;
  double percent = kNaN, points = kNaN, forward = kNaN;
  bool proxy = false, truncated = false, calendar_arbitrage = false;
  std::string reason;
};
/// Wnear = baseline * sessionsNear; Wnext = baseline * sessionsNext + event premium.
[[nodiscard]] std::vector<EventVolatility> event_volatility(const UnderlyingMetrics& metrics,
    const VolatilityMetrics& volatility, std::span<const EventLabel> events);

struct ImpliedMove {
  md::Date date;
  double points = kNaN;
  double percent = kNaN;
  double forward = kNaN;
  bool shared = false;
  bool calendar_arbitrage = false;
  bool proxy = false;
  bool truncated = false;
  std::string label;
  std::string reason;
};
struct MoveInterval {
  md::Timestamp from = 0;
  md::Timestamp to = 0;
  double forward_variance = kNaN;
  std::size_t sessions = 0;
  bool calendar_arbitrage = false;
  bool proxy = false;
  bool truncated = false;
  std::string reason;
};
struct ImpliedMoves {
  std::vector<MoveInterval> intervals;
  std::vector<ImpliedMove> sessions;
  double today_points = kNaN;
  double today_percent = kNaN;
  bool today_proxy = false;
  bool today_truncated = false;
  bool today_calendar_arbitrage = false;
  std::string today_reason = "no_same_day_expiry";
};
[[nodiscard]] ImpliedMoves implied_moves(const UnderlyingMetrics& metrics,
                                         const VolatilityMetrics& volatility,
                                         std::span<const EventLabel> events = {});

}  // namespace openport::analytics
