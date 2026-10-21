#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/server/api.hpp"
#include "openport/server/candles.hpp"
#include "openport/trading/types.hpp"

namespace openport::server {

/// One earnings event's move as the options priced it and as the stock made it.
///
/// Implied: the front at-the-money straddle's mid over spot, from the last analytics
/// before the event, using the first expiry that settles after the reaction session
/// (the event day for before_open, the next business day for after_close).
/// Realized: the reaction session's close over the last close before the event.
struct EarningsMove {
  std::string symbol;
  md::Date date;             ///< the event's date, as the calendar gives it
  std::string session;       ///< before_open or after_close
  std::string label;
  std::string source;        ///< "recorded" here, or "imported" from a user's CSV
  std::optional<double> implied;   ///< fraction of spot, 0.052 for 5.2%
  md::Timestamp implied_at = 0;    ///< market time of the analytics it came from
  std::string expiry;              ///< the straddle's expiry id
  double strike = analytics::kNaN;
  double straddle = analytics::kNaN;
  double spot = analytics::kNaN;
  std::optional<double> realized;  ///< signed fraction, -0.031 for a 3.1% fall
  md::Date before;                 ///< date of the close before the event
  md::Date after;                  ///< date of the reaction session's close
  double close_before = analytics::kNaN;
  double close_after = analytics::kNaN;
  /// Whether the straddle priced more movement than came: "over", "under", or
  /// nullopt until both moves are known.
  [[nodiscard]] std::optional<std::string> priced() const;
};

/// The last regular session before the event's news, and the first that trades on it.
[[nodiscard]] md::Date earnings_last_session(md::Date date, std::string_view session);
[[nodiscard]] md::Date earnings_reaction_session(md::Date date, std::string_view session);

/// The straddle that prices `event`'s move in `metrics`: the first expiry whose
/// settlement is at or after the reaction session's close, and its strike nearest the
/// forward with a two-sided call and put. nullopt without one or once the event began.
[[nodiscard]] std::optional<EarningsMove> implied_move(const trading::PlanEvent& event,
                                                       const analytics::UnderlyingMetrics& metrics);

/// Earnings history, one JSON file (memory only when the path is empty). Recorded and
/// imported moves never share a symbol and date: the first to arrive keeps it.
/// Thread-safe.
class EarningsStore {
 public:
  explicit EarningsStore(std::filesystem::path file = {});
  /// Updates each calendar earnings event of `metrics`' symbol with its implied move,
  /// while the event has not begun; later analytics replace earlier ones.
  void capture(const std::vector<trading::PlanEvent>& calendar, const analytics::UnderlyingMetrics& metrics);
  /// Completes moves whose closes `candles` has, once the reaction session closed
  /// before `now` (wall-clock time).
  void settle(const CandleStore& candles, md::Timestamp now);
  /// Capture for every symbol the source publishes, then settle. Never call it for
  /// simulated markets: their moves are not market history.
  void observe(const MetricsSource& source, const std::vector<trading::PlanEvent>& calendar);
  struct Import {
    std::size_t added = 0;
    std::vector<std::string> skipped;  ///< lines with why
  };
  /// symbol,date,session,implied_move,realized_move[,label] with moves in percent.
  /// Throws std::invalid_argument for a malformed file; nothing is added then.
  Import import_csv(std::string_view text);
  std::size_t remove_imported();
  [[nodiscard]] std::vector<EarningsMove> moves(std::string_view symbol = {}) const;
  [[nodiscard]] std::string error() const;

 private:
  void save();
  std::filesystem::path file_;
  mutable std::mutex mutex_;
  std::map<std::tuple<std::string, md::Date>, EarningsMove> moves_;
  std::string error_;
};

/// Routes:
///   GET    /api/earnings[?symbol=SPY]   history, newest first, with the hit rate
///   POST   /api/earnings/import  {csv}  imported moves, labelled as such
///   DELETE /api/earnings/imported       removes every imported move
/// Returns no response for other routes.
[[nodiscard]] std::optional<ApiResponse> earnings_request(const ApiRequest& request, EarningsStore& store);

}  // namespace openport::server
