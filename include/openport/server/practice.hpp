#pragma once

#include <nlohmann/json.hpp>
#include "openport/providers/scenario.hpp"
#include "openport/trading/journal.hpp"

namespace openport::server {
/// Authored events resolved against the run's actual date and session windows.
nlohmann::json scenario_marks(const providers::Scenario& scenario, md::Date date);
nlohmann::json drill_objectives(const providers::Scenario& scenario, md::Date date);
/// Derived from every committed journal state, never from UI polling. Null pass
/// means insufficient observations; incomplete objectives cannot earn a score.
nlohmann::json grade_drill(const trading::JournalRecovery& journal, const nlohmann::json& objectives);
}  // namespace openport::server
