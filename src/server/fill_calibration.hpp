#pragma once

#include <filesystem>
#include <map>
#include <vector>

#include "openport/trading/fill_quality.hpp"

namespace openport::server {
/// The last recorded quote at or before each timed real fill, no older than
/// `max_age`, by BrokerFill::row, from the complete recordings in `directory`
/// that start on one of the fills' trading dates (or within the day before the
/// first). Unreadable files are skipped; nothing is written.
[[nodiscard]] std::map<std::size_t, trading::CalibrationQuote> recorded_quotes(
    const std::filesystem::path& directory, const std::vector<trading::BrokerFill>& fills, md::Timestamp max_age);
}  // namespace openport::server
