#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/server/api.hpp"
#include "openport/trading/ledger.hpp"
#include "openport/trading/types.hpp"

/// Analysis-only portfolios: positions held somewhere else, imported from a CSV so
/// OpenPort's risk views can run on them. They are not paper accounts: no order,
/// fill or journal can reach them, and every response labels them as such.
namespace openport::server {

/// One imported line: an option (OSI symbol) or shares of an underlying.
struct AnalysisHolding {
  std::string symbol;  ///< padded OSI for an option, the ticker for shares
  std::optional<md::OptionContract> contract;  ///< set for options
  trading::Quantity quantity = 0;  ///< signed contracts or shares; short is negative
  std::optional<double> cost;      ///< price paid per share or per option share, when the file gives one
  std::string source;              ///< the symbol text as the file wrote it
};

struct AnalysisPortfolio {
  std::string id;
  std::string name;
  std::string format;  ///< generic, tastytrade, schwab or ibkr
  md::Timestamp imported = 0;  ///< wall-clock time of the import
  std::vector<AnalysisHolding> holdings;
  /// Lines the import passed over, with why: cash, totals, unsupported instruments.
  std::vector<std::string> skipped;
};

/// The CSV layouts an import understands, by their header names (case and spacing
/// ignored). "auto" picks the first layout whose required columns are present.
inline constexpr std::string_view kAnalysisFormats[] = {"generic", "tastytrade", "schwab", "ibkr"};
inline constexpr std::size_t kAnalysisMaxHoldings = 2000;
inline constexpr std::size_t kAnalysisMaxPortfolios = 32;

/// Parses a positions file. Throws std::invalid_argument naming the line and the
/// problem, or the columns a layout is missing. Holdings of one symbol are summed;
/// ones that net to zero are dropped. Option symbols are read as OSI (padded or
/// compact), Schwab's "SPY 12/18/2026 600.00 C" and IBKR's "SPY 18DEC26 600 C".
[[nodiscard]] AnalysisPortfolio parse_analysis_csv(std::string_view text, std::string_view format);

/// Imported portfolios, one JSON file each in `directory` (memory only when empty).
/// Thread-safe.
class AnalysisPortfolios {
 public:
  explicit AnalysisPortfolios(std::filesystem::path directory = {});
  [[nodiscard]] std::vector<AnalysisPortfolio> list() const;
  [[nodiscard]] std::optional<AnalysisPortfolio> find(std::string_view id) const;
  /// Names it, gives it an id from the name and stores it. Throws std::invalid_argument
  /// for a bad name or a full store, std::runtime_error when the file cannot be written.
  AnalysisPortfolio add(AnalysisPortfolio portfolio, std::string_view name, md::Timestamp now);
  bool remove(std::string_view id);
  /// The latest storage failure while loading; storage never throws after construction.
  [[nodiscard]] std::string error() const;

 private:
  void save(const AnalysisPortfolio& portfolio) const;
  std::filesystem::path directory_;
  mutable std::mutex mutex_;
  std::map<std::string, AnalysisPortfolio, std::less<>> portfolios_;
  std::string error_;
};

/// The book a risk view runs on: the imported holdings as a ledger, valued from
/// the engine's current analytics as the desk values paper positions.
struct AnalysisBook {
  const AnalysisPortfolio* portfolio = nullptr;
  const MetricsSource* source = nullptr;
  trading::Ledger ledger;
  trading::Contracts contracts;
  trading::Valuations valuations;
  std::map<std::string, double> stock_prices;  ///< by underlying, from its analytics' spot
  md::Timestamp now = 0;  ///< the newest analytics time of the held underlyings
  /// How old a valuation may be against `now`: analytics of different underlyings
  /// finish at slightly different times, and a closed market's stay as they closed.
  md::Timestamp max_age = 15 * md::kNanosPerMinute;
  std::vector<std::string> unpriced;  ///< holdings without a usable valuation or price
};
[[nodiscard]] AnalysisBook analysis_book(const AnalysisPortfolio& portfolio, const MetricsSource& source);
/// A risk view, GET /api/analysis/portfolios/{id}/risk/{name}. Views attach by
/// adding an entry to analysis_views(); each returns its own JSON body.
using AnalysisView = std::function<ApiResponse(const AnalysisBook&, const std::map<std::string, std::string>& query)>;
[[nodiscard]] const std::map<std::string, AnalysisView, std::less<>>& analysis_views();

/// Routes:
///   GET    /api/analysis/portfolios
///   POST   /api/analysis/portfolios  {name, format: auto|generic|tastytrade|schwab|ibkr, csv}
///   GET    /api/analysis/portfolios/{id}   holdings valued now, with Greeks by underlying
///   DELETE /api/analysis/portfolios/{id}
///   GET    /api/analysis/portfolios/{id}/risk/{view}   profile, grid, and any view added later
/// Returns no response for other routes.
[[nodiscard]] std::optional<ApiResponse> analysis_request(const ApiRequest& request,
    AnalysisPortfolios& portfolios, const MetricsSource& source);

}  // namespace openport::server
