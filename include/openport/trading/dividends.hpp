#pragma once

#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "openport/md/time.hpp"
#include "openport/trading/money.hpp"

namespace openport::trading {

/// A cash dividend on an underlying's shares, paid on its ex-date to the shares
/// held into it; short shares pay it.
struct Dividend {
  std::string symbol;  ///< The underlying, e.g. "SPY".
  md::Date ex_date;
  Money per_share;
};

/// Reads "SYMBOL,YYYY-MM-DD,AMOUNT" lines, the amount in dollars a share. Blank
/// lines, "#" comments and a header starting "symbol," are skipped. Throws
/// std::invalid_argument naming the line for anything else, and for a symbol
/// listed twice on one date.
[[nodiscard]] std::vector<Dividend> parse_dividends(std::istream& in);

/// The dividends that go ex after `after` and on or before `through`: those a day
/// rollover from `after` to `through` pays, including any the server was down for.
[[nodiscard]] std::vector<Dividend> dividends_due(const std::vector<Dividend>& all, md::Date after, md::Date through);

/// A stock split on its ex-date: every `denominator` shares become `numerator`, so a
/// 2-for-1 split is 2:1 and a 1-for-10 reverse split 1:10. Held shares split, a
/// fraction left over is paid in cash, and the options on the underlying keep their
/// symbol, strike and contract count while their deliverable becomes the split
/// shares (OCC's adjustment of a series).
struct CorporateAction {
  std::string symbol;
  md::Date ex_date;
  std::int64_t numerator = 1;
  std::int64_t denominator = 1;
  bool operator==(const CorporateAction&) const = default;
};
/// Throws std::invalid_argument unless the symbol is 1 to 6 capital letters or
/// digits and both terms are 1 to 1000 and differ.
void validate_corporate_action(const CorporateAction& action);
/// Reads "SYMBOL,YYYY-MM-DD,split,NUMERATOR:DENOMINATOR" lines. Blank lines, "#"
/// comments and a header starting "symbol," are skipped; anything else, and a symbol
/// listed twice on one date, throws std::invalid_argument naming the line.
[[nodiscard]] std::vector<CorporateAction> parse_corporate_actions(std::istream& in);
/// The actions that go ex after `after` and on or before `through`.
[[nodiscard]] std::vector<CorporateAction> corporate_actions_due(const std::vector<CorporateAction>& all, md::Date after,
                                                                 md::Date through);

}  // namespace openport::trading
