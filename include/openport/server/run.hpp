#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <nlohmann/json.hpp>

#include "openport/providers/scenario.hpp"
#include "openport/server/desk.hpp"

namespace openport::server {

[[nodiscard]] std::string recording_input(const std::filesystem::path& file);
[[nodiscard]] std::string scenario_input(const providers::Scenario& scenario, md::Date date, std::uint64_t seed);
/// The run a recording_input or scenario_input describes, under the run's history id.
[[nodiscard]] RunIdentity run_identity(std::string_view input, std::string id);

/// Every input a run recorded, in order, as its JSON text: its start first.
[[nodiscard]] std::vector<std::string> run_inputs(const trading::JournalRecovery& journal);
/// The journal of a saved run that is re-executed to continue it (POST /api/replay
/// {"resume"}): the transactions `expected` already holds are only compared, through
/// their hashes, and the first that differs throws without writing anything; the ones
/// after them append to `file`, which has resumed that same journal.
[[nodiscard]] std::shared_ptr<trading::Journal> resuming_journal(const trading::JournalRecovery& expected,
                                                                 std::shared_ptr<trading::Journal> file);

struct VerificationInput {
  md::Timestamp first = 0, last = 0;
  /// Empty for recordings (continuous played span); scenario sessions otherwise.
  std::vector<providers::ScenarioWindow> sessions;
  std::size_t underlyings = 3;
};
/// Slower of journal throughput and replay input work, rounded up to seconds.
[[nodiscard]] nlohmann::json verification_cost(std::uint64_t bytes, std::uint64_t records = 0,
    const std::optional<VerificationInput>& input = std::nullopt);
/// Reads run metadata without generating input or verifying the journal.
/// Missing/unusable input metadata falls back to journal throughput alone.
[[nodiscard]] nlohmann::json verification_cost(const std::filesystem::path& journal);

struct RunVerification {
  nlohmann::json run;  ///< Shareable identity and inputs; contains no absolute paths.
  md::Timestamp time = 0;  ///< Final recorded market time.
  bool matched = false;
  std::uint64_t transactions = 0;
  std::string head;
  trading::Money equity;
  std::string message;
  bool cut = false;  ///< The journal ends part way through its last operation, as a crash leaves it.
};
/// Opens input read-only and regenerates the complete recorded prefix in memory,
/// including one a crash cut off part way through an operation.
/// Uses a thread-local recorded calendar; opens no provider connection.
/// Missing/changed input is reported separately from a transaction mismatch.
[[nodiscard]] RunVerification verify_run(const std::filesystem::path& journal,
    const std::function<bool()>& cancelled = {},
    const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});

}  // namespace openport::server
