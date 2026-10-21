#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "openport/pricing/binomial.hpp"

namespace openport::pricing {

/// How an American contract's early exercise premium (EEP) is estimated.
enum class AmericanModel : std::uint8_t {
  LeisenReimer,            ///< American minus European on the same LR lattice (the reference)
  BjerksundStensland2002,  ///< two-step flat exercise boundary, closed form
  BaroneAdesiWhaley,       ///< quadratic approximation, one boundary solve
};

/// "leisen_reimer", "bjerksund_stensland_2002" or "barone_adesi_whaley".
[[nodiscard]] std::string_view to_string(AmericanModel model) noexcept;
/// Accepts the names above and the short forms lr, bs2002 and baw.
[[nodiscard]] std::optional<AmericanModel> parse_american_model(std::string_view text) noexcept;

/// Bjerksund and Stensland (2002) American price for a continuous dividend yield.
///
/// Calls use the two-step flat boundary directly; puts use the put-call
/// transformation P(S, K, r, b) = C(K, S, r - b, -b). Where early exercise can
/// never pay (calls with q <= 0, puts with r <= 0) this is the BSM price. The
/// result is floored at the BSM and intrinsic values (both lower bounds of the
/// American price; the flat-boundary strategy can be worse than holding for short,
/// volatile options). NaN for invalid inputs or where the formula is undefined.
[[nodiscard]] double bjerksund_stensland_price(const BsmInputs& in) noexcept;

/// Barone-Adesi and Whaley (1987) quadratic approximation, with the critical
/// price found by Newton's method from Haug's seed. Same conventions as above.
[[nodiscard]] double barone_adesi_whaley_price(const BsmInputs& in) noexcept;

/// The early exercise premium by `model`, never negative.
///
/// LeisenReimer is binomial_early_exercise_premium(in, steps, dividends). The
/// closed forms give American minus analytic BSM, and only where they are
/// defined: with cash dividends before expiry, nonpositive rates, zero vol or
/// time, or a non-finite approximation, this falls back to the LR premium,
/// which handles all of those. Returns the model actually used in `used`.
[[nodiscard]] double early_exercise_premium(const BsmInputs& in, AmericanModel model, int steps,
                                            std::span<const CashDividend> dividends = {},
                                            AmericanModel* used = nullptr);

}  // namespace openport::pricing
