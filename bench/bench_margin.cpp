#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
/// One solve's result and work as counters: whether it was certified exact, and the
/// simplex dictionary cells, exact-check cells and rational operations it took
/// (detail::MarginWork).
void report_allocation(benchmark::State& state, const std::vector<trading::MarginLeg>& legs) {
  const auto before = trading::detail::margin_work();
  const auto result = trading::margin_breakdown(legs);
  const auto after = trading::detail::margin_work();
  state.counters["exact"] = result.front().allocation == "exact" ? 1 : 0;
  state.counters["pivots"] = static_cast<double>(after.pivots - before.pivots);
  state.counters["checks"] = static_cast<double>(after.checks - before.checks);
  state.counters["rational"] = static_cast<double>(after.rational - before.rational);
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
// Irregular books as accounts build them: 15 to 30 legs over four expiries, strikes
// 470-530 every 5, 1 to 20 contracts long or short each, and shorts at a random
// whole-cent value, so shorts go unpaired and quantities rarely match. A fixed LCG
// gives the same books on every standard library; each argument is one book.
std::vector<trading::MarginLeg> irregular_book(std::uint32_t seed) {
  std::uint32_t state = seed * 2654435761U + 12345U;
  const auto next = [&]() { state = state * 1664525U + 1013904223U; return state >> 8; };
  static constexpr const char* expiries[] = {"261022", "261029", "261105", "261120"};
  std::map<std::string, trading::Quantity> positions;
  const std::size_t count = 15 + next() % 16;
  while (positions.size() < count) {
    const auto expiry = expiries[next() % 4];
    const char type = next() % 2 ? 'C' : 'P';
    const auto strike = 470 + 5 * static_cast<int>(next() % 13);
    auto quantity = static_cast<trading::Quantity>(1 + next() % 20);
    if (next() % 2) quantity = -quantity;
    positions[std::string("SPY") + expiry + type + "00" + std::to_string(strike) + "000"] = quantity;
  }
  std::vector<trading::MarginLeg> legs;
  for (const auto& [symbol, quantity] : positions) {
    trading::MarginLeg leg{*md::parse_osi(symbol), quantity, {}, 500.0};
    if (quantity < 0) leg.value = trading::Money::from_micros(10000 * static_cast<std::int64_t>(100 + next() % 3000)) * 100 * -quantity;
    legs.push_back(leg);
  }
  return legs;
}
void BM_MarginJointIrregular(benchmark::State& state) {
  const auto legs = irregular_book(static_cast<std::uint32_t>(state.range(0)));
  state.counters["legs"] = static_cast<double>(legs.size());
  report_allocation(state, legs);
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
// A 15-leg book with shorts over four expiries and few longs to pair them, which took
// tens of milliseconds to certify.
void BM_MarginJointUnpaired(benchmark::State& state) {
  std::vector<trading::MarginLeg> legs;
  for (const auto& [symbol, quantity, value] : std::vector<std::tuple<std::string, trading::Quantity, std::string>>{
      {"SPY261022C00480000", -10, "1830.00"}, {"SPY261022C00510000", 3, "0"}, {"SPY261022P00500000", -6, "18162.00"},
      {"SPY261029C00470000", -6, "17052.00"}, {"SPY261029C00485000", -8, "11184.00"}, {"SPY261029C00490000", 9, "0"},
      {"SPY261029C00515000", -6, "13038.00"}, {"SPY261029P00485000", -10, "8300.00"}, {"SPY261105C00525000", -8, "5712.00"},
      {"SPY261105P00470000", 7, "0"}, {"SPY261105P00510000", 9, "0"}, {"SPY261120C00500000", -10, "24920.00"},
      {"SPY261120P00495000", 5, "0"}, {"SPY261120P00515000", -8, "21248.00"}, {"SPY261120P00525000", 9, "0"}})
    legs.push_back({*md::parse_osi(symbol), quantity, trading::Money::parse(value), 500.0});
  report_allocation(state, legs);
  for (auto _ : state) benchmark::DoNotOptimize(trading::margin_requirement(legs));
}
// One solve's work in cells, an exact rational operation weighing as much as 400
// dictionary cells, about what each costs on a GitHub runner.
std::uint64_t cells_of(const trading::detail::MarginWork& before, const trading::detail::MarginWork& after) {
  return (after.pivots - before.pivots) + (after.checks - before.checks) + 400 * (after.rational - before.rational);
}
// The work of the books tests/trading/margin_allocation_test.cpp needs certified: the
// mixed-expiry pairs (MixedExpiryBooksRetainTheirSeparateAllocations) and the random
// small books checked against the exhaustive oracle.
void BM_MarginWorkTestBooks(benchmark::State& state) {
  using Positions = std::vector<std::pair<std::string, trading::Quantity>>;
  const std::vector<std::pair<Positions, Positions>> cases{
      {{{"SPY261022C00510000", -1}, {"SPY261022P00510000", -1}, {"SPY261022C00520000", -2},
        {"SPY261029P00510000", -2}, {"SPY261105C00480000", -2}, {"SPY261105C00490000", 1}, {"SPY261105C00530000", 2}},
       {{"SPY261022P00490000", -1}, {"SPY261022P00520000", -2}, {"SPY261029C00480000", 1},
        {"SPY261029C00500000", -2}, {"SPY261105C00520000", 1}}},
      {{{"SPY261022P00530000", -1}, {"SPY261029C00530000", -1}, {"SPY261105P00500000", -2}},
       {{"SPY261029P00470000", -1}, {"SPY261029P00480000", -2}, {"SPY261029P00500000", 3},
        {"SPY261029P00530000", -1}, {"SPY261105C00490000", 2}, {"SPY261105P00490000", -2}}},
      {{{"SPY261022C00510000", -2}, {"SPY261029P00480000", -1}, {"SPY261029C00500000", 1},
        {"SPY261105C00490000", -1}, {"SPY261105C00520000", 2}},
       {{"SPY261022P00480000", -2}, {"SPY261022P00520000", -1}, {"SPY261029C00470000", -2},
        {"SPY261029C00490000", 1}, {"SPY261029C00520000", 2}, {"SPY261105P00470000", 1}}},
      {{{"SPY261022P00470000", 1}, {"SPY261022C00490000", -1}, {"SPY261022P00500000", 2},
        {"SPY261022P00530000", -1}, {"SPY261105P00490000", 1}, {"SPY261105C00530000", 1}},
       {{"SPY261022C00470000", -2}, {"SPY261022C00480000", -1}, {"SPY261029C00490000", -2},
        {"SPY261029C00500000", 2}, {"SPY261029P00510000", -2}, {"SPY261029C00520000", 1}, {"SPY261105C00480000", -1}}},
      {{{"SPY261022P00520000", 1}, {"SPY261029P00470000", 2}, {"SPY261029P00530000", -1}, {"SPY261105P00470000", -2}},
       {{"SPY261022C00470000", -1}, {"SPY261022P00520000", 1}, {"SPY261022C00530000", 1}, {"SPY261029C00510000", 2}}}};
  const auto build = [](const Positions& positions) {
    std::vector<trading::MarginLeg> legs;
    for (const auto& [symbol, q] : positions)
      legs.push_back({*md::parse_osi(symbol), q, q < 0 ? trading::Money::parse("250") * -q : trading::Money{}, 500.0});
    return legs;
  };
  std::uint64_t mixed = 0, oracle = 0, fallbacks = 0;
  for (auto _ : state) {
    for (std::size_t i = 0; i < cases.size(); ++i) {
      std::map<std::string, trading::Quantity> combined;
      for (const auto* positions : {&cases[i].first, &cases[i].second})
        for (const auto& [symbol, q] : *positions) combined[symbol] += q;
      for (const auto& book : {build(cases[i].first), build(cases[i].second), build(Positions(combined.begin(), combined.end()))}) {
        const auto before = trading::detail::margin_work();
        fallbacks += trading::margin_breakdown(book).front().allocation != "exact";
        const auto work = cells_of(before, trading::detail::margin_work());
        mixed = std::max(mixed, work);
        state.counters["case" + std::to_string(i + 1)] = std::max(static_cast<double>(work), state.counters["case" + std::to_string(i + 1)].value);
      }
    }
    std::uint32_t seed = 918237;
    const auto next = [&]() { seed = seed * 1664525U + 1013904223U; return seed >> 8; };
    std::vector<md::OptionContract> contracts;
    for (const auto* expiry : {"261022", "261029", "261105"})
      for (const auto* strike : {"00480000", "00490000", "00500000", "00510000", "00520000"})
        for (const char type : {'C', 'P'})
          contracts.push_back(*md::parse_osi(std::string("SPY") + expiry + type + strike));
    for (const auto account : {trading::AccountType::Margin, trading::AccountType::Ira, trading::AccountType::Cash}) {
      for (int trial = 0; trial < 800; ++trial) {
        const trading::MarginPolicy policy{account, trial % 3 == 0 ? 25 : 0, 0};
        std::map<std::size_t, trading::Quantity> quantities;
        const auto count = 3 + next() % 5;
        for (std::uint32_t n = 0; n < count; ++n) {
          const trading::Quantity sign = next() % 2 ? 1 : -1;  // C++17: the right operand of += first
          quantities[next() % contracts.size()] += sign;
        }
        std::vector<trading::MarginLeg> book;
        for (const auto& [i, quantity] : quantities) {
          if (!quantity) continue;
          const auto n = quantity < 0 ? -quantity : quantity;
          const auto premium = trading::Money::from_micros(300000001 + static_cast<std::int64_t>(next() % 2000000000));
          book.push_back({contracts[i], quantity, quantity < 0 ? premium * n : trading::Money{}, 500.0});
        }
        if (book.empty()) continue;
        const auto before = trading::detail::margin_work();
        fallbacks += trading::margin_breakdown(book, {}, policy).front().allocation != "exact";
        oracle = std::max(oracle, cells_of(before, trading::detail::margin_work()));
      }
    }
  }
  state.counters["max_mixed"] = static_cast<double>(mixed);
  state.counters["max_oracle"] = static_cast<double>(oracle);
  state.counters["fallbacks"] = static_cast<double>(fallbacks);
}
// The seeded sample tests/trading/margin_allocation_test.cpp checks for subadditivity
// (SeededMixedExpirySubadditivityRate), solved once: the most work any of its books
// takes, and how many take more than each candidate budget.
void BM_MarginWorkSeededSample(benchmark::State& state) {
  std::uint64_t solves = 0, fallbacks = 0, pivots = 0, checks = 0, rational = 0, most = 0;
  std::map<std::uint64_t, std::uint64_t> over{{5'000'000, 0}, {10'000'000, 0}, {15'000'000, 0}, {20'000'000, 0},
                                              {30'000'000, 0}, {50'000'000, 0}};
  for (auto _ : state) {
    std::uint32_t seed = 0xF490123U;
    const auto next = [&]() { seed = seed * 1664525U + 1013904223U; return seed >> 8; };
    std::vector<md::OptionContract> contracts;
    for (const auto* expiry : {"261022", "261029", "261105"})
      for (int strike = 470; strike <= 530; strike += 10)
        for (const char type : {'C', 'P'})
          contracts.push_back(*md::parse_osi(std::string("SPY") + expiry + type + "00" + std::to_string(strike) + "000"));
    for (int trial = 0; trial < 20736; ++trial) {
      const auto na = 2 + next() % 6, nb = 2 + next() % 6;
      auto shuffled = contracts;
      std::vector<trading::MarginLeg> a, b, ab;
      for (std::size_t i = 0; i < na + nb; ++i) {
        std::swap(shuffled[i], shuffled[i + next() % (shuffled.size() - i)]);
        const auto n = static_cast<trading::Quantity>(1 + next() % 2);
        const auto q = next() % 2 ? n : -n;
        const trading::MarginLeg leg{shuffled[i], q, q < 0 ? trading::Money::parse("250") * n : trading::Money{}, 500.0};
        (i < na ? a : b).push_back(leg);
        ab.push_back(leg);
      }
      for (const auto account : {trading::AccountType::Margin, trading::AccountType::Ira}) {
        if (account == trading::AccountType::Ira && (trading::disallowed_shorts(a, {}, account) ||
            trading::disallowed_shorts(b, {}, account) || trading::disallowed_shorts(ab, {}, account))) continue;
        const trading::MarginPolicy policy{account, 0, 0};
        for (const auto* book : {&a, &b, &ab}) {
          const auto before = trading::detail::margin_work();
          const auto result = trading::margin_breakdown(*book, {}, policy);
          const auto after = trading::detail::margin_work();
          ++solves;
          fallbacks += result.front().allocation != "exact";
          const auto p = after.pivots - before.pivots, c = after.checks - before.checks, r = after.rational - before.rational;
          pivots = std::max(pivots, p);
          checks = std::max(checks, c);
          rational = std::max(rational, r);
          const auto work = p + c + 400 * r;
          most = std::max(most, work);
          for (auto& [budget, count] : over) count += work > budget;
        }
      }
    }
  }
  state.counters["solves"] = static_cast<double>(solves);
  state.counters["fallbacks"] = static_cast<double>(fallbacks);
  state.counters["max_pivots"] = static_cast<double>(pivots);
  state.counters["max_checks"] = static_cast<double>(checks);
  state.counters["max_rational"] = static_cast<double>(rational);
  state.counters["max_cells"] = static_cast<double>(most);
  for (const auto& [budget, count] : over) state.counters["over_" + std::to_string(budget / 1'000'000) + "M"] = static_cast<double>(count);
}
BENCHMARK(BM_MarginJointAdversarial)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointIrregular)->DenseRange(1, 16)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MarginJointUnpaired)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_MarginWorkSeededSample)->Iterations(1)->Repetitions(1)->ReportAggregatesOnly(false)->Unit(benchmark::kSecond);
BENCHMARK(BM_MarginWorkTestBooks)->Iterations(1)->Repetitions(1)->ReportAggregatesOnly(false)->Unit(benchmark::kSecond);
BENCHMARK(BM_MarginPairing)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJoint)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointLargeQuantity)->Arg(14)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginPairingFragmented)->Arg(80)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_MarginJointFragmented)->Arg(80)->Unit(benchmark::kMicrosecond);
}  // namespace
