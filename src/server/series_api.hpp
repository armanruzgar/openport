#pragma once
#include <map>
#include <optional>
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"
namespace openport::server {
// The 404 for a symbol a metric route cannot serve, in the shared error shape, or nullopt when it can.
std::optional<ApiResponse> unknown_symbol_error(const MetricsSource& source, const std::string& symbol);
ApiResponse series_response(const MetricsSource& source, const std::string& symbol,
                            const std::map<std::string, std::string>& query);
nlohmann::json series_status_json(const MetricsSource& source);
ApiResponse volatility_scanner_response(const MetricsSource& source);
nlohmann::json surface_snapshot_json(const SurfaceSnapshot& snapshot);
ApiResponse surface_history_response(const MetricsSource& source, const std::string& symbol,
                                     const std::map<std::string, std::string>& query);
void add_volatility_history(nlohmann::json& response, const MetricsSource& source,
                            const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics);
}
