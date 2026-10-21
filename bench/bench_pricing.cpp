#include <benchmark/benchmark.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "../tests/support/synthetic_chain.hpp"
#include "openport/analytics/chain_analytics.hpp"
#include "openport/analytics/chain_book.hpp"
#include "openport/pricing/binomial.hpp"
#include "openport/pricing/black.hpp"
#include "openport/pricing/implied_vol.hpp"

namespace {

using openport::pricing::OptionType;

// Time a complete pass, including parity, all IVs/Greeks, exposure and gamma flip.
void BM_AnalyzeSyntheticChain(benchmark::State& state) {
  const openport::test::SyntheticChain chain;
  const auto& underlying = chain.book.underlyings().at("SPX");
  for (auto _ : state) {
    benchmark::DoNotOptimize(openport::analytics::analyze(underlying, chain.book, chain.as_of));
  }
  state.SetItemsProcessed(state.iterations() * 82);
}
BENCHMARK(BM_AnalyzeSyntheticChain)->Unit(benchmark::kMicrosecond);

/// A synthetic SPX-sized chain (simulated quotes, not market data): 30 expiries from
/// 0DTE to about seven months, 500 strikes each, 30k options with skewed OI.
const openport::analytics::UnderlyingMetrics& large_chain_metrics() {
  namespace analytics = openport::analytics;
  namespace md = openport::md;
  static const analytics::UnderlyingMetrics metrics = [] {
    constexpr double spot = 5000.0, rate = 0.045;
    analytics::ChainBook book;
    const md::Date today{2026, 9, 22};
    const auto as_of = md::new_york_to_utc(today, 14, 0);
    book.apply(md::UnderlyingQuote{"SPX", as_of, spot - 0.5, spot + 0.5, spot});
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<double> interest(0.0, 5000.0);
    md::InstrumentId id = 0;
    for (int expiry = 0; expiry < 30; ++expiry) {
      md::OptionContract c;
      c.root = "SPXW";
      c.underlying = "SPX";
      c.expiry = md::date_from_days(md::days_since_epoch(today) + expiry + expiry * expiry / 5);
      c.style = openport::pricing::ExerciseStyle::European;
      c.settlement = md::Settlement::PM;
      const double years = md::years_between(as_of, c.expiry_time());
      const double discount = std::exp(-rate * years);
      const double forward = spot / discount;
      for (int i = 0; i < 500; ++i) {
        c.strike = 3750.0 + 5.0 * i;
        const double k = std::log(c.strike / forward);
        const double vol = std::max(0.08, 0.16 - 0.3 * k + 1.2 * k * k);
        for (const OptionType type : {OptionType::Call, OptionType::Put}) {
          c.type = type;
          book.apply(md::ContractDefinition{id, c});
          const double fair =
              openport::pricing::black_price(type, forward, c.strike, years, vol, discount);
          book.apply(md::OptionQuote{id, as_of, std::max(0.0, fair - 0.05), fair + 0.05, 10, 10});
          book.apply(md::OpenInterest{id, as_of, std::round(interest(rng))});
          ++id;
        }
      }
    }
    return analytics::analyze(book.underlyings().at("SPX"), book, as_of);
  }();
  return metrics;
}

// Exposure for a non-default API request: convention change on every expiry.
void BM_ExposureViewLargeChain(benchmark::State& state) {
  const auto& metrics = large_chain_metrics();
  for (auto _ : state) {
    benchmark::DoNotOptimize(openport::analytics::exposure_view(
        metrics, openport::analytics::ExposureConvention::ShortAll));
  }
  state.SetItemsProcessed(state.iterations() * metrics.coverage.options);
}
BENCHMARK(BM_ExposureViewLargeChain)->Unit(benchmark::kMillisecond);

// A 0DTE request on the same chain only needs the same-day expiry.
void BM_ExposureViewLargeChainZeroDte(benchmark::State& state) {
  const auto& metrics = large_chain_metrics();
  for (auto _ : state) {
    benchmark::DoNotOptimize(openport::analytics::exposure_view(
        metrics, openport::analytics::ExposureConvention::ShortAll,
        openport::analytics::ExposureSlice::ZeroDte));
  }
}
BENCHMARK(BM_ExposureViewLargeChainZeroDte)->Unit(benchmark::kMillisecond);

struct Quote {
  OptionType type;
  double forward, strike, expiry, vol, price;
};

// A realistic chain: strikes within +-3 standard deviations, one day to one year.
std::vector<Quote> make_quotes(std::size_t n) {
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> z(-3.0, 3.0);
  std::uniform_real_distribution<double> expiry(1.0 / 365.0, 1.0);
  std::uniform_real_distribution<double> vol(0.2, 1.2);
  std::vector<Quote> quotes;
  quotes.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double t = expiry(rng);
    const double v = vol(rng);
    const double k = 100.0 * std::exp(-z(rng) * v * std::sqrt(t));
    const OptionType type = k >= 100.0 ? OptionType::Call : OptionType::Put;
    quotes.push_back({type, 100.0, k, t, v, openport::pricing::black_price(type, 100.0, k, t, v)});
  }
  return quotes;
}

void BM_BlackPrice(benchmark::State& state) {
  const auto quotes = make_quotes(4096);
  std::size_t i = 0;
  for (auto _ : state) {
    const Quote& q = quotes[i++ & 4095];
    benchmark::DoNotOptimize(
        openport::pricing::black_price(q.type, q.forward, q.strike, q.expiry, q.vol));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BlackPrice);

void BM_BlackGreeks(benchmark::State& state) {
  const auto quotes = make_quotes(4096);
  std::size_t i = 0;
  for (auto _ : state) {
    const Quote& q = quotes[i++ & 4095];
    benchmark::DoNotOptimize(
        openport::pricing::black_greeks(q.type, q.forward, q.strike, q.expiry, q.vol));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_BlackGreeks);

void BM_ImpliedVol(benchmark::State& state) {
  const auto quotes = make_quotes(4096);
  std::size_t i = 0;
  std::int64_t iterations = 0;
  for (auto _ : state) {
    const Quote& q = quotes[i++ & 4095];
    auto iv = openport::pricing::implied_vol_black(q.price, q.type, q.forward, q.strike, q.expiry);
    iterations += iv.iterations;
    benchmark::DoNotOptimize(iv);
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["newton_iters"] =
      benchmark::Counter(static_cast<double>(iterations), benchmark::Counter::kAvgIterations);
}
BENCHMARK(BM_ImpliedVol);

void BM_AmericanLeisenReimer(benchmark::State& state) {
  const openport::pricing::BsmInputs in{OptionType::Put, 100.0, 105.0, 0.5, 0.04, 0.01, 0.3};
  const int steps = static_cast<int>(state.range(0));
  for (auto _ : state) {
    benchmark::DoNotOptimize(
        openport::pricing::binomial_price(in, openport::pricing::ExerciseStyle::American,
                                          openport::pricing::TreeMethod::LeisenReimer, steps));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AmericanLeisenReimer)->Arg(101)->Arg(501)->Arg(1001)->Unit(benchmark::kMicrosecond);

}  // namespace
