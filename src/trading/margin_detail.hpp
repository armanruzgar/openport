#pragma once

#include "margin_optimizer.hpp"
#include "openport/trading/evaluation.hpp"

namespace openport::trading::detail {
/// The pre-improvement allocator, retained as an incumbent and a benchmark/test baseline.
Money pairing_margin_requirement(const std::vector<MarginLeg>& legs, const std::vector<MarginStock>& stocks = {},
                                 const MarginPolicy& policy = {});
}  // namespace openport::trading::detail
