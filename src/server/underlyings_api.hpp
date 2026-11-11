#pragma once

#include <optional>
#include <string>

#include "openport/server/api.hpp"

namespace openport::server {

/// An underlying's symbol as a request names it, in capitals: 1 to 16 letters, digits
/// and dots, not starting with a dot (SPX, NVDA, BRK.B); nullopt for anything else.
[[nodiscard]] std::optional<std::string> underlying_symbol_of(std::string text);
/// GET /api/underlyings: what the server carries and whether that can change while it runs.
[[nodiscard]] ApiResponse underlyings_response(const MetricsSource& source);
/// POST /api/underlyings and DELETE /api/underlyings/{symbol}, queued to the engine
/// thread; false for any other request, which is left for the other routes.
bool underlying_write(const ApiRequest& request, MetricsSource& source, const ApiCompletion& complete);

}  // namespace openport::server
