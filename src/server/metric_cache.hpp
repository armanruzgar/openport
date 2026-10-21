#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

#include "openport/analytics/heston.hpp"
#include "openport/analytics/realized.hpp"
#include "openport/analytics/sabr.hpp"
#include "openport/analytics/ssvi.hpp"
#include "openport/analytics/volatility.hpp"
#include "openport/server/api.hpp"

namespace openport::server {
class CandleStore;
class MetricsSource;

// Shared by API workers, the series worker and enabled playbook conditions.
struct SurfaceFits {
  std::mutex mutex;
  std::vector<analytics::SviFit> fits;
  std::optional<analytics::SsviFit> ssvi;
  /// Optional SABR overlays, fitted only when requested, by fixed beta or "fit".
  std::map<std::string, std::vector<analytics::SabrFit>> sabr;
  /// The research Heston fit, by the number of leading expiries it covers.
  std::map<std::size_t, analytics::HestonFit> heston;
  /// A deferred Heston or SABR fit for this snapshot is queued or running.
  bool fitting = false;
  std::optional<analytics::VolatilityMetrics> volatility;
  std::map<const CandleStore*, analytics::RealizedMetrics> realized;
  std::map<const MetricsSource*, nlohmann::json> volatility_responses;
  std::map<const MetricsSource*, nlohmann::json> scanner_rows;  ///< Metric fields only; feed state is per request.
  std::map<const MetricsSource*, std::vector<analytics::EventLabel>> events;
};
/// A read whose answer needs a Heston or SABR fit not yet cached: too slow for an
/// HTTP I/O thread. `run` fits outside the cache lock, stores the fits and builds
/// the response from the snapshot alone, so it never touches the source.
struct DeferredRead {
  std::shared_ptr<SurfaceFits> cache;
  std::function<ApiResponse()> run;
};
/// A busy fit worker asks the client to retry after this many seconds.
inline constexpr int kFitRetrySeconds = 2;
/// handle_api, except that a read needing a slow uncached fit fills `deferred`
/// and returns nullopt instead of fitting on the calling thread.
std::optional<ApiResponse> handle_api_or_defer(const ApiRequest& request, const MetricsSource& source,
                                               DeferredRead& deferred);
/// Runs `read` on the process's fit worker and completes from there. One deferred
/// fit per snapshot at a time; another, or a full queue, is 503 with Retry-After.
void run_deferred(DeferredRead read, ApiCompletion complete);

std::shared_ptr<SurfaceFits> surface_cache(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
analytics::VolatilityMetrics cached_volatility(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
}  // namespace openport::server
