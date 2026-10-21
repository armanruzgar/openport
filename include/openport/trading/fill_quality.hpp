#pragma once

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "openport/trading/types.hpp"

/// Fill quality: where fills sit on the book they traded against, and how a
/// trader's real broker fills compare with what the paper fill models would
/// have done on the same quotes. Read-only analysis; nothing here trades.
namespace openport::trading {

/// Where a price sits on a valid book, measured from the mid toward the far
/// side (the natural): at the mid or better, within one tick past it, further
/// inside the spread, at the natural, or beyond it.
enum class FillPlace { MidOrBetter, MidPlusOneTick, Inside, Natural, BeyondNatural };
inline constexpr std::size_t kFillPlaces = 5;
[[nodiscard]] std::string_view to_string(FillPlace place) noexcept;

/// A price against a book. Ticks are the far side's tick tier (tick_size), signed
/// so that positive is worse for the side: paid above the mid for a buy, received
/// below it for a sale. `fraction` runs from 0 at the own side to 1 at the natural.
struct FillPosition {
  FillPlace place = FillPlace::Natural;
  Money mid;
  Money natural;
  Money tick;
  double from_mid_ticks = 0;
  double vs_natural_ticks = 0;
  double fraction = 1;
  std::int64_t spread_ticks = 0;  ///< The spread's width, rounded to whole ticks.
};
/// Nothing without a positive bid below a positive ask.
[[nodiscard]] std::optional<FillPosition> fill_position(std::string_view root, Side side, Money price,
                                                        std::optional<Money> bid, std::optional<Money> ask);

/// Fills of one kind: their contracts by place, and contract-weighted means.
struct FillQualityGroup {
  std::string key;
  std::string label;
  std::int64_t fills = 0;
  Quantity contracts = 0;
  std::array<Quantity, kFillPlaces> places{};
  std::optional<double> from_mid_ticks;
  std::optional<double> vs_natural_ticks;
  Money cost_vs_mid;  ///< Dollars paid beyond the mid, multiplier applied; negative is better.
  void add(const FillPosition& position, Quantity quantity, double multiplier);
};

/// Suggested fill-model settings, with why. `basis` is "heuristic" for the
/// journal's own fills and "real_fills" for a broker import.
struct FillCalibration {
  std::string basis;
  std::string fill_model;  ///< as_displayed, conservative, midpoint or custom.
  std::optional<std::int64_t> fill_latency_ms;  ///< Empty: nothing measured it.
  std::int64_t slippage_ticks = 0;
  std::int64_t impact_ticks = 0;
  std::int64_t inside_fill_percent = 0;
  std::vector<std::string> reasons;
};

/// The journal's fills by place, overall and by New York time of day, the
/// spread's width in ticks and calendar days to expiry, with a suggested
/// Conservative calibration. Fills recorded without their book (older builds),
/// on a book without both sides, or given away for nothing are `unmeasured`.
struct FillQualityReport {
  std::int64_t fills = 0;
  std::int64_t unmeasured = 0;
  FillQualityGroup overall;
  std::vector<FillQualityGroup> time_of_day;
  std::vector<FillQualityGroup> spread;
  std::vector<FillQualityGroup> dte;
  FillCalibration suggestion;
};
/// Fills from `first_fill` (an attempt's first fill ID) on; contracts unknown to
/// `contracts` are skipped as unmeasured.
[[nodiscard]] FillQualityReport fill_quality(const SharedVector<Fill>& fills, const Contracts& contracts,
                                             std::uint64_t first_fill = 0);

/// One execution from a broker's export, normalised. `time` is zero when the
/// export gives only a date; `bid` and `ask` only come from generic imports
/// that carry the quote at the fill.
struct BrokerFill {
  std::size_t row = 0;  ///< 1-based line in the file.
  Timestamp time = 0;
  std::string symbol;   ///< Padded OSI.
  Side side = Side::Buy;
  Quantity quantity = 0;
  Money price;
  std::optional<Money> bid = {};
  std::optional<Money> ask = {};
};
struct BrokerImport {
  std::string format;  ///< The format read, once detected.
  std::vector<BrokerFill> fills;
  std::vector<std::pair<std::size_t, std::string>> skipped;  ///< Rows that are not option fills, and why.
};
/// Parses `format`: generic, tastytrade, schwab, ibkr, or auto to detect it from
/// the header. Throws std::invalid_argument for an unknown format, or a file
/// with no recognisable header. At most 5,000 fills.
[[nodiscard]] BrokerImport parse_broker_fills(std::string_view csv, std::string_view format = "auto");

/// The book at a real fill's time, and where it came from: "import" (the file's
/// own bid and ask) or "recording" (the server's recorded market data).
struct CalibrationQuote {
  Money bid;
  Money ask;
  Quantity bid_size = 0;
  Quantity ask_size = 0;
  Timestamp time = 0;
  std::string source;
};
/// What a paper model would have done with a limit at the real fill's price on
/// that book: whether it fills at once, and at what average price.
struct PaperOutcome {
  bool fills = false;
  std::optional<Money> price;
};
struct CalibrationRow {
  BrokerFill fill;
  std::optional<CalibrationQuote> quote;
  std::optional<FillPosition> real;
  PaperOutcome as_displayed, conservative, midpoint;
  std::string note;  ///< Why it could not be compared, when it could not.
};
struct CalibrationReport {
  std::string format;
  std::vector<CalibrationRow> rows;
  std::int64_t matched = 0;
  std::int64_t unmatched = 0;
  FillQualityGroup real;  ///< The matched real fills by place.
  std::array<std::int64_t, 3> paper_fills{};  ///< Matched fills each model fills at once: as displayed, conservative, midpoint.
  FillCalibration suggestion;
  std::vector<std::pair<std::size_t, std::string>> skipped;
};
/// Compares each real fill with its own bid and ask when the file has them, else
/// the book in `quotes` (by BrokerFill::row), and suggests the settings that bring
/// the paper fills closest to the real ones.
[[nodiscard]] CalibrationReport calibrate(const BrokerImport& import, const std::map<std::size_t, CalibrationQuote>& quotes);

}  // namespace openport::trading
