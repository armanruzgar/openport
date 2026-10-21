#pragma once
#include <map>
#include <string>
#include "openport/server/api.hpp"
namespace openport::server {
/// GET /api/underlyings/{symbol}/flow: the retained tape, newest first, with net
/// premium by expiry and by strike over the same filtered prints.
ApiResponse flow_response(const MetricsSource& source, const std::string& symbol,
                          const std::map<std::string, std::string>& query);
}
