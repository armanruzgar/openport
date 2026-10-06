#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/md/time.hpp"
#include "openport/trading/dividends.hpp"

namespace openport::providers {

struct ScenarioEvent {
  std::string type;  ///< gap, crush, spike, pin, book, stall or halt
  std::string at;    ///< HH:MM ET; gap has no time
  double move = 0;   ///< log return, gap or spike
  double iv = 0;     ///< absolute IV change, crush or spike
  double strike = 0; ///< SPX strike, pin
  int minutes = 0;  ///< window duration for book, stall and halt
  std::string state = {}; ///< crossed, locked, one_sided, zero_size or wide
  std::vector<std::string> symbols = {}; ///< empty means all scenario symbols
  std::optional<md::Date> expiry = std::nullopt; ///< book filter
};

/// Shares of an ETF going ex-dividend on a session's trading date.
struct ScenarioDividend {
  std::string symbol;    ///< SPY or QQQ
  double per_share = 0;  ///< dollars
};

/// One session of a multi-session scenario.
struct ScenarioSession {
  std::string session;  ///< regular, curb or overnight
  std::vector<std::pair<double, double>> drift;  ///< fraction -> cumulative log return
  double volatility = 0, iv_shift = 0, spot_vol = 0;
  std::vector<ScenarioEvent> events;
  std::vector<ScenarioDividend> dividends;
  std::optional<double> previous_close = std::nullopt; ///< log return relative to the ungapped opening level
};

struct Scenario {
  std::string id, title, description, goal;
  std::vector<std::string> symbols;
  bool overnight = false;  ///< The (first) session is overnight.
  md::Date date;  ///< The (first) session's trading date.
  std::uint64_t seed = 0;
  int generator = 1;
  std::vector<std::pair<double, double>> drift;  ///< fraction -> cumulative log return
  double volatility = 0, iv_shift = 0, spot_vol = 0;
  std::vector<ScenarioEvent> events;
  /// A multi-session scenario's sessions, played one after another from `date` as
  /// one run. Empty for a single session, which the fields above describe; for
  /// several, they repeat the first session's.
  std::vector<ScenarioSession> sessions;
  std::string source;  ///< Exact parsed bytes, for run provenance.
  std::filesystem::path source_file;
  bool builtin = false;
  std::optional<double> previous_close = std::nullopt; ///< first date, before any opening gap
  bool events_scaled = false; ///< live demo has already fitted its regular event times
  double strike_window = 0; ///< optional minimum strike window as a fraction of open
};

/// When one session of a scenario runs.
struct ScenarioWindow {
  std::string session;      ///< regular, curb or overnight
  md::Date date;            ///< the trading date it belongs to
  md::Timestamp first = 0;  ///< its first snapshot
  md::Timestamp close = 0;  ///< where its drift ends: a regular session's index close
  md::Timestamp last = 0;   ///< its last snapshot
  md::Timestamp step = 0;   ///< between snapshots
};

/// Strict version 1 parser. Errors include the file and field.
[[nodiscard]] Scenario parse_scenario(std::string_view source, const std::filesystem::path& file);
[[nodiscard]] Scenario read_scenario(const std::filesystem::path& file);
/// Built-ins first, user files after them, replacing built-ins with the same id.
/// Invalid built-ins throw; invalid user files are reported and skipped.
[[nodiscard]] std::vector<Scenario> load_scenarios(const std::filesystem::path& user = {},
    const std::function<void(const std::string&)>& log = {});
[[nodiscard]] const std::vector<Scenario>& builtin_scenarios();
[[nodiscard]] md::Timestamp scenario_open(const Scenario& scenario, md::Date date);
/// The (first) session's close.
[[nodiscard]] md::Timestamp scenario_close(const Scenario& scenario, md::Date date);
/// Each session the scenario plays from `date`, in order. A regular session follows
/// an overnight one on its date; otherwise each session after a regular or curb one
/// is on the next trading date, except a curb, which follows its date's regular
/// session. Throws std::invalid_argument naming the session when the dates cannot
/// hold the sequence (a curb on an early-close date, say).
[[nodiscard]] std::vector<ScenarioWindow> scenario_windows(const Scenario& scenario, md::Date date);
/// The last session's last snapshot.
[[nodiscard]] md::Timestamp scenario_end(const Scenario& scenario, md::Date date);
/// Overnight HH:MM >= 20:15 belongs to the evening before the trading date.
[[nodiscard]] md::Timestamp scenario_time(std::string_view time, md::Date date, bool overnight);
/// Event times use a full regular session; early closes scale offsets down to minutes.
[[nodiscard]] md::Timestamp scenario_event_time(std::string_view time, const ScenarioWindow& window, int minutes = 0);
/// The generator's output revision. Revision 1 lists five expiries a chain: the date,
/// the next two business days, the Friday after and next month's third Friday.
/// Revision 2 also lists every series an earlier date listed until its last trade, at
/// least as wide as then, and opens with each underlying's previous close. The contracts
/// both list keep their identifiers, quotes and sizes. Revision 3 adds XSP, NDX/NDXP,
/// RUT/RUTW and VIX/VIXW, their ticks and sessions, and stops AM quotes at their last
/// regular close. Revision 4 prices American ETF options with discrete quarterly
/// dividends, also reflected in spot prices. Runs record the revision; one
/// recorded without it regenerates revision 1. Revision 5 scales regular event
/// times and window endpoints into early-close sessions. Revision 6 includes
/// deterministic opening activity in session volume. Revision 7 keeps a two-sided
/// minimum-tick market in liquid wings; authored stress books still override it.
/// Revision 8 freezes ETF prices at the stock close while options trade on.
inline constexpr int kScenarioRevision = 8;
/// Deterministic simulated quarterly ETF dividends, inclusive of both dates.
[[nodiscard]] std::vector<trading::Dividend> demo_dividends(md::Date first, md::Date last);
/// Known payments through the run's listed ETF expiries. Session entries replace
/// generated payments for the same symbol/date; revisions 1-3 have only explicit entries.
[[nodiscard]] std::vector<trading::Dividend> scenario_dividends(const Scenario& scenario, md::Date date,
                                                               int revision = kScenarioRevision);
/// The option contracts a scenario lists on `date`, in the order it defines them.
[[nodiscard]] std::vector<md::OptionContract> scenario_chain(const Scenario& scenario, md::Date date,
                                                             int revision = kScenarioRevision);
void write_scenario_recording(const std::filesystem::path& path, const Scenario& scenario,
                              md::Date date, std::uint64_t seed, int revision = kScenarioRevision);

}  // namespace openport::providers
