#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

#include "openport/analytics/realized.hpp"
#include "openport/analytics/ssvi.hpp"
#include "openport/analytics/volatility.hpp"

namespace openport::server {
class CandleStore;
class MetricsSource;

// Shared by API workers, the series worker and enabled playbook conditions.
struct SurfaceFits {
  std::mutex mutex;
  std::vector<analytics::SviFit> fits;
  std::optional<analytics::SsviFit> ssvi;
  std::optional<analytics::VolatilityMetrics> volatility;
  std::map<const CandleStore*, analytics::RealizedMetrics> realized;
  std::map<const MetricsSource*, nlohmann::json> volatility_responses;
  std::map<const MetricsSource*, nlohmann::json> scanner_rows;  ///< Metric fields only; feed state is per request.
  std::map<const MetricsSource*, std::vector<analytics::EventLabel>> events;
};
std::shared_ptr<SurfaceFits> surface_cache(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
analytics::VolatilityMetrics cached_volatility(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
}  // namespace openport::server
