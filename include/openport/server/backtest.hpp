#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include "openport/server/api.hpp"
#include "openport/server/playbooks.hpp"
#include "openport/server/run.hpp"

namespace openport::server {
inline constexpr std::string_view kBacktestLabel =
    "Simulated trading on recorded or generated days. Not a prediction. Not investment advice.";

struct BacktestDay {
  std::filesystem::path file;
  std::optional<providers::Scenario> scenario;
  md::Date date;
  std::uint64_t seed = 0;
};
struct BacktestRequest {
  nlohmann::json playbooks;
  std::string playbook;  ///< Single selected ID; empty for a joint run.
  trading::SessionConfig config;
  analytics::AnalyticsOptions analytics;
  std::vector<trading::Dividend> dividends;
  std::vector<BacktestDay> days;
  unsigned workers = 4;
  std::string actor = "system";
};
/// Pin one definition or 2–8 distinct IDs (lexical evaluation order), and a plan. API file names stay under recordings;
/// CLI manifests may supply paths relative to their directory.
[[nodiscard]] BacktestRequest parse_backtest(const nlohmann::json& body, const nlohmann::json& catalogue,
    const std::vector<providers::Scenario>& scenarios, const std::filesystem::path& recordings, bool confined = true);
/// The directory must be new. Journal paths in the report are relative to it.
/// Progress counts finished independent days, then days consumed by attempts.
[[nodiscard]] nlohmann::json run_backtest(const BacktestRequest& request, const std::filesystem::path& directory,
    const std::atomic_bool& cancel, const std::function<void(std::size_t, std::string_view)>& progress = {});
[[nodiscard]] nlohmann::json compare_backtests(const nlohmann::json& runs);
/// Single-playbook JSON-pointer grid; 1-4 fields and 2-8 variants, validated before launch.
[[nodiscard]] std::vector<std::pair<nlohmann::json, BacktestRequest>> expand_backtest_grid(
    const BacktestRequest& request, const nlohmann::json& grid);
[[nodiscard]] nlohmann::json run_backtest_sweep(const BacktestRequest& request, const nlohmann::json& grid,
    const std::filesystem::path& directory, const std::atomic_bool& cancel,
    const std::function<void(std::size_t, std::string_view)>& progress = {});
void write_backtest_report(const std::filesystem::path& file, const nlohmann::json& report);

/// One background job per host. Completed reports are read from disk after restart.
class BacktestHost {
 public:
  struct Options {
    std::filesystem::path directory, recordings, scenario_dir;
    analytics::AnalyticsOptions analytics;
    std::vector<trading::Dividend> dividends;
    bool enabled = true;
    unsigned keep = 20;  ///< Finished unpinned runs retained on start; zero is unlimited.
  };
  explicit BacktestHost(Options options);
  ~BacktestHost();
  bool handle(const ApiRequest& request, const MetricsSource& source, const ApiCompletion& complete);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace openport::server
