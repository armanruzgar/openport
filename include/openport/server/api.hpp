#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "openport/server/engine.hpp"

namespace openport::server {

struct ApiAccess {
  bool admin = false;
  std::string sandbox;
};

struct ApiRequest {
  ApiRequest(std::string method_value = "GET", std::string target_value = {}, std::string body_value = {})
      : method(std::move(method_value)), target(std::move(target_value)), body(std::move(body_value)) {}
  std::string method = "GET";
  std::string target;  ///< Path and query.
  std::string body;
  std::string content_type;
  std::optional<std::string> origin;
  std::string host;
  std::string authorization;
  std::string actor = "unknown";  ///< Set by transport authentication, never request JSON.
  ApiAccess access;
  std::string client_ip;  ///< Connection peer, or the explicitly configured proxy header.
  bool ambiguous_headers = false;
};

struct ApiResponse {
  int status = 200;
  std::string body;
  std::string content_type = "application/json";
  std::string download = {};
  /// Seconds a client should wait before retrying (Retry-After); zero sends none.
  int retry_after = 0;
};
/// A full command inbox frees up as the engine applies commands, so its 503
/// asks the client to retry after this many seconds.
inline constexpr int kInboxRetrySeconds = 1;

using ApiCompletion = std::function<void(ApiResponse)>;

[[nodiscard]] ApiResponse api_error(int status, std::string code, std::string message,
                                    const trading::Decision& evidence = {});
/// Splits a query ("a=1&b=2", without the "?") into percent-decoded keys and values;
/// "+" stays "+" unless `plus_is_space` (form encoding). A pair without "=" has an
/// empty value. nullopt when an escape is malformed or a decoded key repeats. Every
/// route and access check parses queries here, so each reads the same keys however
/// a client spells them: %61ccount is account (RFC 3986 equivalence).
[[nodiscard]] std::optional<std::map<std::string, std::string>> query_parameters(std::string_view query, bool plus_is_space = false);
/// The account a request addresses: its decoded account=ID query parameter, or the
/// supplied default (main when empty) without one. nullopt when the query does not parse or the ID is not
/// an account ID (lowercase letters, digits and single hyphens); refuse the request.
[[nodiscard]] std::optional<std::string> query_account(std::string_view target, std::string_view default_account = {});
/// Writes enqueue on the owner thread. Never wait for completion on an I/O thread.
void handle_api_async(const ApiRequest& request, MetricsSource& source, ApiCompletion complete);

/// Routes:
///   GET /api/status
///   GET /api/underlyings/{symbol}/summary
///   GET /api/underlyings/{symbol}/chain?expiry={id}[&window=0.1]
///   GET /api/underlyings/{symbol}/exposure[?expiries=8][&window=0.08]
///   GET /api/underlyings/{symbol}/series[?fields=mfiv30,atm30][&interval=1m|1d][&from=][&to=]
///   GET /api/underlyings/{symbol}/volatility
///   GET /api/underlyings/{symbol}/surface[?expiries=12][&window=0.2]
///   GET /api/underlyings/{symbol}/candles[?interval=5m][&limit=500]
///   GET /api/underlyings/{symbol}/flow[?limit=200][&type=call|put][&side=bid,ask,mid,outside,unquoted]
///       [&expiry=ID][&min_size=N][&min_premium=USD][&only=sweep|block|multi_leg]
/// Flow: the option tape (FlowTape), newest first, with net premium by expiry and
/// strike over the filtered prints; source is feed, simulated (the demo market's
/// labelled prints) or none, with a message when there is nothing to show.
/// Candles: {symbol, interval, bars: [{t, o, h, l, c}]}, oldest first, where t is
/// the bar's start in Unix seconds of market-data time; interval is 1m, 5m, 15m,
/// 30m, 1h (starting at half past) or 1d (starting at the 09:30 ET open), and
/// limit (1-5000) keeps the most recent bars. See CandleStore for their sources.
/// Expiry ids are the date plus settlement, e.g. "2026-10-16AM", because SPX
/// (morning settlement) and SPXW (afternoon) can expire on the same day.
/// OEX and XEO additionally carry -OEX or -XEO to distinguish exercise styles.
/// Status and ticks include market {open, note, next_open}; this uses wall-clock
/// regular-session hours, while as_of remains the market data's timestamp.
/// next_open uses the same UTC timestamp format as as_of; null while open.
/// Each status/tick underlyings[] entry also includes session {name, open, note}
/// at wall-clock time, where name is regular/curb/global/closed for that product.
/// paper {accepting, reason, message} uses the new-order session/feed gate with
/// the data clock, provider delay and current max_quote_age. Success has null
/// reason/message; contract, risk and write-access checks still apply separately.
/// has_tradable_contracts is true when unexpired slices contain a contract eligible
/// for paper trading (standard European index or American equity/ETF options).
/// Portfolio session notices use this flag, or held positions/open orders.
/// The top-level market remains the regular-session calendar for compatibility.
/// All four underlying views include spot_source ("quote", "parity", or null).
/// Summary includes american_approximation (any priced American expiry without
/// de-Americanisation) and
/// coverage {options, quoted, priced, open_interest} over live standard contracts.
/// summary.expiries[] and chain.expiry include style ("european"/"american") and
/// the same coverage counts. Quoted/OI counts mean received, including real zeros;
/// priced means the option has its own valid IV, not just its strike's smile IV.
/// Each expiry includes rate_source: "parity" (this expiry's fitted rate), "term"
/// (borrowed from this underlying's longer European expiries), "curve" (another
/// underlying's European parity curve), or "assumed" (flat fallback). rate_fitted
/// remains true only for "parity". rate_curve_symbol names the curve (e.g. "SPX")
/// for "curve", otherwise null. deamericanized reports EEP removal with an LR
/// tree. Chain bid/ask/mid remain the raw market quotes; iv/bid_iv/ask_iv use
/// premium-adjusted prices. Each option includes eep, the per-unit price premium
/// used for those solves (null when not computed, zero when computed as zero).
/// Greeks remain European Black-76 Greeks at the final smile IV, accurate OTM.
/// Chain bid/ask/mid and oi are null when never received; invalid values are null.
/// Exposure summaries include oi_coverage: received OI / options at finite-smile-IV strikes,
/// or null for an empty set. This covers the full exposure, regardless of display
/// window/expiry filters. Every side at those strikes with usable OI contributes risk, even without
/// its own IV.
/// Surface preserves the market points and adds points[].svi_iv (decimal IV/null).
/// Each expiry adds svi: {a,b,rho,m,sigma,rmse_vol_points,points,status,reason,
/// fit_ms,butterfly_min_g,butterfly_k,butterfly_ok}, or null when fitting fails.
/// svi_status (ok/too_few_points/failed), svi_reason, svi_points and svi_fit_ms
/// remain available even when svi is null. svi_years and svi_min_k/svi_max_k give
/// the high-precision tenor and calibration range for dense client-side curve sampling.
/// Parameters describe TOTAL variance at k=ln(K/F); rmse_vol_points is in percent.
/// Butterfly fields report a 2,001-point density check over the calibration range,
/// not a global no-arbitrage certificate. Undefined density fails the check.
/// calendar_violations: [{earlier: expiry id, later: expiry id, k, vol_points,
/// tolerance_vol_points}] compares consecutive successful fits ordered by T over
/// their calibration-range intersection. vol_points is the later-expiry IV increase
/// needed to remove the crossing; tolerance is max(0.1 vp, both fit RMSEs, both
/// locally interpolated quoted IV half-spreads in vp). Reports the largest flagged
/// increase and its local tolerance. Diagnostics never repair raw SVI fits.
/// Top-level ssvi: {rho,eta,gamma,rmse_vol_points,status,reason,monotone_adjusted,
/// fit_ms} describes one modified-power-law SSVI surface for all usable expiries.
/// status is ok/too_few_points/failed; parameters/RMSE are null on failure. Each
/// expiry adds ssvi_theta (ATM total variance), ssvi_rmse_vol_points, ssvi_reason,
/// ssvi_min_k/ssvi_max_k; points[].ssvi_iv is decimal IV/null. Missing ATM brackets
/// or unusable quotes skip that expiry (null theta/RMSE/IV, explicit reason).
/// A failed surface has null estimates and a top-level reason. Theta and parameters
/// retain full precision. See analytics/ssvi.hpp and docs/svi.md for constraints.
/// Fitting is lazy, outside the engine. Raw SVI fits only the requested expiry
/// prefix; SSVI fits the entire snapshot once, independent of window/prefix.
/// Successes and failures are cached for the immutable (symbol, metrics version)
/// snapshot, isolated by snapshot owner. Concurrent requests share fits; expired
/// snapshots release their cache entries. Window only filters displayed points.
[[nodiscard]] ApiResponse handle_api(const ApiRequest& request, const MetricsSource& source);

/// The small message pushed to every WebSocket client each second, so the UI knows
/// when data changed without polling everything.
[[nodiscard]] std::string tick_message(const MetricsSource& source, const ApiAccess& access = {});

}  // namespace openport::server
