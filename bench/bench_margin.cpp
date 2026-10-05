#include <benchmark/benchmark.h>

#include "../src/trading/margin_detail.hpp"

namespace {
using namespace openport;

std::vector<trading::MarginLeg> mixed_book(std::int64_t count) {
  std::vector<trading::MarginLeg> legs;
  const auto first = *md::parse_osi("SPY261022P00500000");
  const auto later = *md::parse_osi("SPY261029P00500000");
  for (std::int64_t i = 0; i < count; ++i) {
    auto contract = i % 3 == 0 ? later : first;
    contract.type = i % 2 == 0 ? pricing::OptionType::Call : pricing::OptionType::Put;
    contract.strike = 450.0 + static_cast<double>(i);
    const bool shorted = i % 4 < 2;
    legs.push_back({contract, shorted ? -2 : 2, shorted ? trading::Money::parse("600.000002") : trading::Money{}, 500.0});
  }
  return legs;
}
void BM_MarginPairing(benchmark::State& state) {
  const auto legs = mixed_book(state.range(0));
  for (auto _ : state) benchmark::DoNotOptimize(trading::detail::pairing_margin_requirement(legs));
}
void report_allocation(benchmark::State& state, const std::vector<trading::MarginLeg>& legs) {
  const auto result = trading::margin_breakdown(legs);
  state.counters["exact"] = result.front().allocation == "exact" ? 1 : 0;
}
void BM_MarginJoint(benchmark::State& state) {
  const auto legs = mixed_book(state.range(0));
  report_allocation(state, legs);
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
void BM_MarginJointLargeQuantity(benchmark::State& state) {
  auto legs = mixed_book(state.range(0));
  for (auto& leg : legs) { leg.quantity *= 10000; leg.value = leg.value * 10000; }
  report_allocation(state, legs);
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
std::vector<trading::MarginLeg> fragmented_book() {
  std::vector<trading::MarginLeg> legs;
  const auto first = *md::parse_osi("SPY261022P00500000");
  const auto second = *md::parse_osi("SPY261029P00500000");
  const auto third = *md::parse_osi("SPY261105P00500000");
  for (int i = 0; i < 40; ++i) {
    auto put = i % 3 == 0 ? first : i % 3 == 1 ? second : third;
    auto call = i % 3 == 0 ? second : i % 3 == 1 ? third : first;
    put.strike = 460.0 + i * 2;
    call.strike = put.strike + 1;
    call.type = pricing::OptionType::Call;
    legs.push_back({put, -1, trading::Money::parse("250"), 500.0});
    legs.push_back({call, -1, trading::Money::parse("250"), 500.0});
  }
  return legs;
}
// Forty small straddles across three expiries cannot coalesce into bounded
// pools. Their many cheap, unsuccessful trials exhaust the work budget through
// more neighbourhoods than a few large pools with expensive payoff curves.
void BM_MarginPairingFragmented(benchmark::State& state) {
  const auto legs = fragmented_book();
  for (auto _ : state) benchmark::DoNotOptimize(trading::detail::pairing_margin_requirement(legs));
}
void BM_MarginJointFragmented(benchmark::State& state) {
  const auto legs = fragmented_book();
  report_allocation(state, legs);
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
// The most expensive small mixed-expiry sample in the fixed-seed probe. It
// requires integer branching, unlike the fragmented all-short flow above.
void BM_MarginJointAdversarial(benchmark::State& state) {
  std::vector<trading::MarginLeg> legs;
  for (const auto& [symbol, quantity] : std::vector<std::pair<std::string, trading::Quantity>>{
      {"SPY261029C00510000", -2}, {"SPY261105C00500000", 1}, {"SPY261022P00490000", -2},
      {"SPY261105C00470000", 2}, {"SPY261029P00520000", -1}, {"SPY261022C00490000", -2},
      {"SPY261022P00480000", 2}, {"SPY261105P00530000", 1}, {"SPY261105P00500000", -1},
      {"SPY261029P00510000", -2}, {"SPY261105C00530000", -2}, {"SPY261029P00490000", -1},
      {"SPY261029P00530000", -2}})
    legs.push_back({*md::parse_osi(symbol), quantity,
                    quantity < 0 ? trading::Money::parse("250") * -quantity : trading::Money{}, 500.0});
  report_allocation(state, legs);
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
BENCHMARK(BM_MarginJointAdversarial)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginPairing)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJoint)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointLargeQuantity)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginPairingFragmented)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointFragmented)->Arg(80)->Unit(benchmark::kMicrosecond);
}  // namespace
