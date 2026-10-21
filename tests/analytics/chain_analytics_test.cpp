#include "openport/analytics/chain_analytics.hpp"

#include <gtest/gtest.h>

#include <cmath>

#include "openport/analytics/chain_book.hpp"
#include "openport/pricing/black.hpp"
#include "support/synthetic_chain.hpp"

namespace {

using namespace openport;
using analytics::ChainBook;
using pricing::OptionType;

using test::SyntheticChain;
constexpr double kSpot = SyntheticChain::kSpot;

double smile(double strike, double forward) {
  return SyntheticChain::smile(strike, forward);
}

TEST(ChainAnalytics, RecoversTheForwardAndTheSmile) {
  SyntheticChain chain;
  const auto& underlying = chain.book.underlyings().at("SPX");
  const auto metrics = analytics::analyze(underlying, chain.book, chain.as_of);
  ASSERT_EQ(metrics.slices.size(), 1u);
  const auto& slice = metrics.slices[0];

  EXPECT_NEAR(slice.forward.forward, chain.forward, 1e-6 * chain.forward);
  EXPECT_NEAR(slice.forward.discount, chain.discount, 1e-8);
  EXPECT_EQ(metrics.options_priced, 2 * 41);

  for (const auto& row : slice.strikes) {
    // Quotes are symmetric around fair value, so the mid is exact and so is its IV.
    EXPECT_NEAR(row.iv, smile(row.strike, chain.forward), 1e-6) << row.strike;
  }
  EXPECT_NEAR(slice.atm_iv, smile(chain.forward, chain.forward), 2e-3);
}

TEST(ChainAnalytics, GreeksAreConsistentAcrossCallAndPut) {
  SyntheticChain chain;
  const auto metrics =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  for (const auto& row : metrics.slices[0].strikes) {
    // Same smile vol on both sides: put-call parity for delta, identical gamma and vega.
    const double carry = chain.forward / kSpot;
    EXPECT_NEAR(row.call.delta - row.put.delta, chain.discount * carry, 1e-9) << row.strike;
    EXPECT_NEAR(row.call.gamma, row.put.gamma, 1e-12);
    EXPECT_NEAR(row.call.vega, row.put.vega, 1e-9);
    EXPECT_GT(row.call.gamma, 0.0);
    EXPECT_LT(row.call.theta, 0.0);
  }
}

TEST(ChainAnalytics, ExposureFollowsTheOpenInterestConvention) {
  SyntheticChain chain;
  const auto metrics =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  const auto& exposure = metrics.exposure;

  // Calls dominate above spot (positive GEX), puts below (negative GEX).
  for (const auto& row : metrics.slices[0].strikes) {
    if (row.strike > kSpot + 1) {
      EXPECT_GT(row.gex, 0.0) << row.strike;
    }
    if (row.strike < kSpot - 1) {
      EXPECT_LT(row.gex, 0.0) << row.strike;
    }
  }
  EXPECT_GT(exposure.call_wall, kSpot);
  EXPECT_LE(exposure.put_wall, kSpot);  // the at-the-money strike carries heavy put OI here
  ASSERT_TRUE(std::isfinite(exposure.gamma_flip));
  EXPECT_NEAR(exposure.gamma_flip, kSpot, 0.05 * kSpot);

  // One strike by hand: gamma * OI * 100 * S^2 * 1%.
  const auto& row = metrics.slices[0].strikes[30];
  const double expected =
      (row.call.gamma * row.call.open_interest - row.put.gamma * row.put.open_interest) * 100.0 *
      kSpot * kSpot * 0.01;
  EXPECT_NEAR(row.gex, expected, 1e-6 * std::abs(expected));
}

TEST(ChainAnalytics, SkipsExpiredSlices) {
  SyntheticChain chain;
  const md::Timestamp after_expiry = md::new_york_to_utc(md::Date{2026, 10, 23}, 10, 0);
  const auto metrics =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, after_expiry);
  EXPECT_TRUE(metrics.slices.empty());
}

TEST(ChainBook, KeepsAmAndPmSettledExpiriesApart) {
  ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("SPX261016C05000000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("SPXW261016C05000000")});
  book.apply(md::OptionQuote{7, 1, 1.0, 2.0, 1, 1});  // undefined id: ignored
  const auto& spx = book.underlyings().at("SPX");
  EXPECT_EQ(spx.expiries.size(), 2u);
  EXPECT_EQ(book.contracts(), 2u);
  EXPECT_EQ(book.option(7), nullptr);
}

TEST(ChainBook, AdjustedContractsCannotReplaceStandardStrikePairs) {
  ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("SPY261218C00500000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("SPY1261218C00500000")});
  book.apply(md::OptionQuote{1, 10, 1.0, 2.0, 1, 1});
  const auto& spy = book.underlyings().at("SPY");
  ASSERT_EQ(spy.expiries.size(), 1u);
  EXPECT_EQ(spy.expiries.begin()->second.strikes.at(500).call, 0u);
  EXPECT_EQ(spy.data_time, 0);
  EXPECT_EQ(book.nonstandard_contracts(), 1u);
  book.apply(md::ContractDefinition{1, *md::parse_osi("SPY1261218C00500000")});
  EXPECT_EQ(book.nonstandard_contracts(), 1u);
}

TEST(ChainBook, OexAndXeoDoNotOverwriteOrPairAcrossExerciseStyles) {
  ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("OEX261016C03000000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("XEO261016P03000000")});
  book.apply(md::ContractDefinition{2, *md::parse_osi("XEO261016C03000000")});
  const auto& slices = book.underlyings().at("OEX").expiries;
  ASSERT_EQ(slices.size(), 2u);
  for (const auto& [key, slice] : slices) {
    const auto pair = slice.strikes.at(3000);
    if (slice.root == "OEX") {
      EXPECT_EQ(pair.call, 0u);
      EXPECT_EQ(pair.put, analytics::kNoInstrument);
    } else {
      EXPECT_EQ(pair.call, 2u);
      EXPECT_EQ(pair.put, 1u);
    }
  }
}

// Build quotes from exact forwards, with optional OI, to isolate exposure regressions.
void add_option(ChainBook& book, md::InstrumentId id, md::Date date, OptionType type, double strike,
                md::Timestamp as_of, double forward, double vol, double oi = -1,
                double discount = 1) {
  md::OptionContract c;
  c.root = "SPXW";
  c.underlying = "SPX";
  c.expiry = date;
  c.strike = strike;
  c.type = type;
  c.style = pricing::ExerciseStyle::European;
  c.settlement = md::Settlement::PM;
  book.apply(md::ContractDefinition{id, c});
  const double price = pricing::black_price(
      type, forward, strike, md::years_between(as_of, c.expiry_time()), vol, discount);
  book.apply(md::OptionQuote{id, as_of, price * 0.999, price * 1.001, 1, 1});
  if (oi >= 0) book.apply(md::OpenInterest{id, as_of, oi});
}

TEST(ChainAnalytics, DeltaCharmAndVommaMatchFiniteDifferences) {
  SyntheticChain chain;
  const auto m = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  double dex = 0, cex = 0, vomex = 0;
  for (const auto& row : m.slices[0].strikes) {
    double expected_delta = 0, expected_charm = 0, expected_vomma = 0;
    for (const auto* side : {&row.call, &row.put}) {
      const double weight = (side->contract.type == OptionType::Call ? 1 : -1) *
                            side->open_interest * side->contract.multiplier;
      pricing::BsmInputs in{side->contract.type, kSpot, row.strike, chain.years,
          SyntheticChain::kRate, SyntheticChain::kDividend, row.iv};
      expected_delta += weight * pricing::bsm_greeks(in).delta * kSpot;
      constexpr double dt = 1e-6, dv = 1e-5;
      auto before = in, after = in;
      before.expiry += dt;
      after.expiry -= dt;
      expected_charm += weight * kSpot *
          (pricing::bsm_greeks(after).delta - pricing::bsm_greeks(before).delta) / (2 * dt * 365);
      before = after = in;
      before.vol -= dv;
      after.vol += dv;
      expected_vomma += weight *
          (pricing::bsm_greeks(after).vega - pricing::bsm_greeks(before).vega) / (2 * dv * 10000);
    }
    EXPECT_NEAR(row.dex, expected_delta, 1e-5);
    EXPECT_NEAR(row.cex, expected_charm, .01);
    EXPECT_NEAR(row.vomex, expected_vomma, .001);
    dex += row.dex; cex += row.cex; vomex += row.vomex;
  }
  EXPECT_DOUBLE_EQ(m.exposure.dex, dex);
  EXPECT_DOUBLE_EQ(m.exposure.cex, cex);
  EXPECT_DOUBLE_EQ(m.exposure.vomex, vomex);
  // Short puts have positive delta: do not net call and put OI before computing DEX.
  EXPECT_GT(m.exposure.dex, 0);
  EXPECT_TRUE(std::isnan(m.exposure.levels[2].flip));
  EXPECT_TRUE(std::isnan(m.exposure.levels[2].negative_wall));
}

TEST(ChainAnalytics, ExposureConventionsKeepDefaultAndMissingFlowHonest) {
  SyntheticChain chain;
  const auto m = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  const auto alias = analytics::exposure_view(m, analytics::ExposureConvention::LongCallsShortPuts);
  const auto short_all = analytics::exposure_view(m, analytics::ExposureConvention::ShortAll);
  for (const auto field : analytics::kExposureFields)
    EXPECT_DOUBLE_EQ(m.exposure.*field, alias.exposure.*field);
  for (const auto& row : short_all.slices[0].strikes) {
    EXPECT_LT(row.gex, 0);
    EXPECT_NEAR(row.dex, -(row.call.delta * row.call.open_interest +
                          row.put.delta * row.put.open_interest) * 100 * kSpot, 1e-5);
  }
  EXPECT_TRUE(std::isnan(short_all.exposure.gamma_flip));
  EXPECT_TRUE(std::isnan(short_all.exposure.call_wall));
  // Unsigned volume is not customer direction, even when every side has it.
  auto with_volume = m;
  for (auto& row : with_volume.slices[0].strikes) row.call.volume = row.put.volume = 100;
  const auto flow = analytics::exposure_view(with_volume, analytics::ExposureConvention::CustomerFlow);
  EXPECT_FALSE(flow.exposure.available);
  EXPECT_NE(flow.exposure.unavailable_reason.find("aggressor"), std::string::npos);
  for (const auto field : analytics::kExposureFields) {
    EXPECT_TRUE(std::isnan(flow.exposure.*field));
    EXPECT_TRUE(std::isnan(flow.slices[0].strikes[0].*field));
  }
  EXPECT_TRUE(m.exposure.available);
}

TEST(ChainAnalytics, EqualOiStillCarriesDeltaAndShortAllHasADeltaFlip) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  for (const auto type : {OptionType::Call, OptionType::Put})
    add_option(book, type == OptionType::Put, {2026, 10, 22}, type, 100, as_of, 100, .3, 10);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  EXPECT_DOUBLE_EQ(m.exposure.gex, 0);
  EXPECT_DOUBLE_EQ(m.exposure.vex, 0);
  EXPECT_NEAR(m.exposure.dex, 100000, 1e-8);
  const auto short_all = analytics::exposure_view(m, analytics::ExposureConvention::ShortAll);
  EXPECT_NEAR(short_all.exposure.levels[2].flip,
              100 * std::exp(-.5 * .3 * .3 * m.slices[0].years), 1e-6);
}

TEST(ChainAnalytics, NewExposureMetricsUseTheFloorWhileDeltaQuotesUseTrueTime) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 10, 22}, 15, 59);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Call, 100, as_of, 100, .3, 10);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  const auto& row = m.slices[0].strikes[0];
  const auto floored = pricing::black_greeks(OptionType::Call, 100, 100, .5 / 365, row.iv);
  EXPECT_NEAR(row.dex, floored.delta * 100000, 1e-8);
  EXPECT_NEAR(row.vomex, floored.volga * 1000 / 10000, 1e-8);
  constexpr double dt = 1e-8;
  const double before = pricing::black_greeks(OptionType::Call, 100, 100, .5 / 365 + dt, row.iv).delta;
  const double after = pricing::black_greeks(OptionType::Call, 100, 100, .5 / 365 - dt, row.iv).delta;
  EXPECT_NEAR(row.cex, (after - before) * 100000 / (2 * dt * 365), 1e-4);
  EXPECT_NE(row.call.delta, floored.delta);
}

TEST(ChainAnalytics, ZeroDtePartitionsEveryMetricUsingSnapshotNewYorkDate) {
  ChainBook book;
  // UTC is already the next day; tomorrow's expiry must remain ex-0DTE.
  const auto as_of = md::new_york_to_utc({2026, 10, 21}, 21, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  for (int day = 22; day <= 23; ++day)
    for (const auto type : {OptionType::Call, OptionType::Put})
      add_option(book, 2 * (day - 22) + (type == OptionType::Put), {2026, 10, day},
                 type, 100, as_of, 100, .3, type == OptionType::Call ? 20 : 10);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  options.exposure_min_days = 1;
  auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  EXPECT_TRUE(analytics::exposure_view(m, analytics::ExposureConvention::OpenInterest,
                                     analytics::ExposureSlice::ZeroDte).slices.empty());
  const auto later = md::new_york_to_utc({2026, 10, 22}, 15, 59);
  m = analytics::analyze(book.underlyings().at("SPX"), book, later, options);
  const auto today = analytics::exposure_view(m, analytics::ExposureConvention::OpenInterest,
                                             analytics::ExposureSlice::ZeroDte);
  const auto ex = analytics::exposure_view(m, analytics::ExposureConvention::OpenInterest,
                                          analytics::ExposureSlice::ExZeroDte);
  ASSERT_EQ(today.slices.size(), 1u);
  ASSERT_EQ(ex.slices.size(), 1u);
  EXPECT_EQ(today.as_of, later);
  EXPECT_EQ(today.exposure_settings.min_days, 1);
  for (const auto field : analytics::kExposureFields)
    EXPECT_NEAR(m.exposure.*field, today.exposure.*field + ex.exposure.*field, 1e-8);
  // Once settlement has passed, the same-day slice is absent, not a stale 0DTE position.
  const auto settled = analytics::analyze(book.underlyings().at("SPX"), book,
      md::new_york_to_utc({2026, 10, 22}, 16, 1), options);
  EXPECT_TRUE(analytics::exposure_view(settled, analytics::ExposureConvention::OpenInterest,
                                     analytics::ExposureSlice::ZeroDte).slices.empty());
}

TEST(ChainAnalytics, HigherOrderFlipsAreRepricedRootsAndWallsUseSignedExtrema) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Call, 100, as_of, 100, .3, 10);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  const double half_variance = .5 * .3 * .3 * m.slices[0].years;
  // Vanna/charm vanish at d2=0. Vomma vanishes at d1=0 and d2=0;
  // its d1 root is slightly nearer to spot in price space.
  EXPECT_NEAR(m.exposure.levels[1].flip, 100 * std::exp(half_variance), 1e-6);
  EXPECT_NEAR(m.exposure.levels[3].flip, 100 * std::exp(half_variance), 1e-6);
  EXPECT_NEAR(m.exposure.levels[4].flip, 100 * std::exp(-half_variance), 1e-6);
  EXPECT_EQ(m.exposure.levels[2].positive_wall, 100);
  EXPECT_EQ(m.exposure.levels[3].negative_wall, 100);
  EXPECT_EQ(m.exposure.levels[4].negative_wall, 100);
  book.apply(md::OpenInterest{0, as_of, 0});
  const auto zero = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  for (const auto& levels : zero.exposure.levels) {
    EXPECT_TRUE(std::isnan(levels.flip));
    EXPECT_TRUE(std::isnan(levels.positive_wall));
    EXPECT_TRUE(std::isnan(levels.negative_wall));
  }
}

TEST(ChainAnalytics, GammaUnderflowIsNotAFlip) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 10, 22}, 4, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Put, 100, as_of, 100, 0.05, 100);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  EXPECT_LT(m.exposure.gex, 0);
  EXPECT_TRUE(std::isnan(m.exposure.gamma_flip));
}

TEST(ChainAnalytics, GammaFlipIncludesDistantStrikesAndRefinesRoot) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2025, 10, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Put, 100, as_of, 100, 0.5, 100);
  add_option(book, 1, {2026, 10, 22}, OptionType::Call, 150, as_of, 100, 0.5, 114);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  options.flip_steps = 5;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  // Equal weighted densities: log(S/100)=log(1.5)/2 - .125 - .25*log(1.14)/log(1.5).
  const double root =
      100 * std::exp(std::log(1.5) / 2 - .125 - .25 * std::log(1.14) / std::log(1.5));
  EXPECT_NEAR(m.exposure.gamma_flip, root, 1e-6);
}

TEST(ChainAnalytics, AllExpiriesShareNearestParitySpot) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  for (int expiry = 0; expiry < 2; ++expiry) {
    const md::Date date{2026, 10 + expiry, 22};
    for (auto type : {OptionType::Call, OptionType::Put})
      add_option(book, 2 * expiry + (type == OptionType::Put), date, type, 100, as_of,
                 expiry == 0 ? 100 : 120, .5, type == OptionType::Call ? 10 : 0);
  }
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  ASSERT_EQ(m.slices.size(), 2u);
  EXPECT_NEAR(m.spot, 100, 1e-10);
  const auto& slice = m.slices[1];
  const auto g = pricing::black_greeks(OptionType::Call, 120, 100, slice.years, .5);
  EXPECT_NEAR(slice.strikes[0].call.gamma, g.gamma * 1.2 * 1.2, 1e-10);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 110, 110, 110});
  const auto quoted = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  EXPECT_DOUBLE_EQ(quoted.spot, 110);
  EXPECT_NEAR(quoted.slices[1].strikes[0].call.gamma, g.gamma * std::pow(120.0 / 110, 2), 1e-10);
}

TEST(ChainAnalytics, UnpricedSideContributesOiThroughTheStrikeSmile) {
  SyntheticChain chain;
  chain.book.apply(md::OptionQuote{40, chain.as_of, 0, 0, 0, 0});
  const auto m = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  EXPECT_EQ(m.options_priced, 81);
  const auto& row = m.slices[0].strikes[20];
  EXPECT_FALSE(std::isfinite(row.call.iv));
  EXPECT_NEAR(
      row.gex,
      row.put.gamma * (row.call.open_interest - row.put.open_interest) * 100 * kSpot * kSpot * .01,
      1e-6);
}

TEST(ChainBook, ReceiptFlagsDistinguishMissingFromRealZeros) {
  ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("SPXW261022C00100000")});
  ASSERT_NE(book.option(0), nullptr);
  EXPECT_FALSE(book.option(0)->has_quote);
  EXPECT_FALSE(book.option(0)->has_open_interest);
  book.apply(md::OptionQuote{0, 0, 0, 0, 0, 0});
  book.apply(md::OpenInterest{0, 0, 0});
  EXPECT_TRUE(book.option(0)->has_quote);
  EXPECT_TRUE(book.option(0)->has_open_interest);
  EXPECT_DOUBLE_EQ(book.option(0)->bid, 0);
  EXPECT_DOUBLE_EQ(book.option(0)->open_interest, 0);
}

TEST(ChainAnalytics, LockedQuoteIsRejectedBeforePricingTheChain) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2025, 10, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  for (int i = 0; i < 12; ++i)
    for (auto type : {OptionType::Call, OptionType::Put}) {
      const md::InstrumentId id = 2 * i + (type == OptionType::Put);
      add_option(book, id, {2026, 10, 22}, type, 95 + i, as_of, 100, .5);
      const auto* state = book.option(id);
      const double mid = state->mid() + (i == 11 && type == OptionType::Call ? 50 : 0);
      const double half_spread = i == 11 ? 0 : .1;
      book.apply(md::OptionQuote{id, as_of, mid - half_spread, mid + half_spread, 1, 1});
    }
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  ASSERT_EQ(m.slices.size(), 1u);
  EXPECT_EQ(m.slices[0].forward.points, 11);
  EXPECT_NEAR(m.slices[0].forward.forward, 100, 1e-8);
}

TEST(ChainAnalytics, ExposureUsesSmileIvAndReceivedOiAcrossExpiries) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Call, 100, as_of, 100, .5, 10);
  add_option(book, 1, {2026, 10, 22}, OptionType::Put, 100, as_of, 100, .5);      // missing OI
  add_option(book, 2, {2026, 11, 23}, OptionType::Call, 110, as_of, 100, .5, 0);  // received zero
  add_option(book, 3, {2026, 11, 23}, OptionType::Put, 110, as_of, 100, .5, 1000);
  book.apply(md::OptionQuote{3, as_of, 0, 0, 0,
                             0});  // OI contributes through the strike smile despite no own IV
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  ASSERT_EQ(m.slices.size(), 2u);
  EXPECT_EQ(m.options_priced, 3);
  EXPECT_EQ(m.coverage.options, 4);
  EXPECT_EQ(m.coverage.quoted, 4);
  EXPECT_EQ(m.coverage.open_interest, 3);
  EXPECT_NEAR(m.exposure.oi_coverage, 3.0 / 4, 1e-12);
  EXPECT_LT(m.slices[1].gex, 0);
  EXPECT_NEAR(m.slices[1].gex, -m.slices[1].strikes[0].put.gamma * 1000 * 100 * 100, 1e-9);
  EXPECT_TRUE(std::isnan(m.exposure.gamma_flip));
  EXPECT_NEAR(m.exposure.gex, m.slices[0].strikes[0].call.gamma * 10 * 100 * 100 + m.slices[1].gex,
              1e-9);
  EXPECT_EQ(m.spot_source, "quote");
}

TEST(ChainAnalytics, SpotComesFromNearestValidLiveParityIncludingDiscount) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  book.apply(
      md::ContractDefinition{0, *md::parse_osi("SPXW260923C00100000")});  // nearest has no fit
  for (int e = 0; e < 2; ++e)
    for (int k = 99; k <= 101; ++k)
      for (auto type : {OptionType::Call, OptionType::Put}) {
        const auto id = 1 + e * 6 + (k - 99) * 2 + (type == OptionType::Put);
        const md::Date date{2026, 10 + e, 23};
        const auto years = md::years_between(as_of, md::new_york_to_utc(date, 16, 0));
        add_option(book, id, date, type, k, as_of, e == 0 ? 102 : 120, .5, 10,
                   std::exp(-.05 * years));
      }
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of);
  ASSERT_EQ(m.slices.size(), 3u);
  EXPECT_EQ(m.spot_source, "parity");
  EXPECT_NEAR(m.spot, 102 * m.slices[1].forward.discount, 1e-8);
  const auto& far = m.slices[2];
  const auto gamma =
      pricing::black_greeks(OptionType::Call, 120, 100, far.years, .5, far.forward.discount).gamma;
  EXPECT_NEAR(far.strikes[1].call.gamma, gamma * std::pow(120 / m.spot, 2), 1e-10);
}

TEST(ChainAnalytics, GammaFlipBracketsANearZeroGridPointAcrossExpiries) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2025, 10, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Put, 100, as_of, 100, .5, 100);
  // Equal total vols, with OI chosen so gamma cancels exactly at spot=100.
  const double log_strike = std::log(1.5);
  const double oi = 100 * std::exp(2 * log_strike * log_strike - .5 * log_strike);
  add_option(book, 1, {2027, 10, 22}, OptionType::Call, 150, as_of, 100, .5 / std::sqrt(2.0), oi);
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  options.flip_steps = 3;
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  EXPECT_NEAR(m.exposure.gamma_flip, 100, 1e-6);
}

TEST(ChainAnalytics, EuropeanShortExpiryReportsTermOrAssumedRate) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  md::InstrumentId id = 0;
  for (auto date : {md::Date{2026, 9, 23}, md::Date{2027, 9, 22}}) {
    const double t = md::years_between(as_of, md::new_york_to_utc(date, 16, 0));
    for (double k = 90; k <= 110; k += 2.5)
      for (auto type : {OptionType::Call, OptionType::Put})
        add_option(book, id++, date, type, k, as_of, 100, .5, 1, std::exp(-.05 * t));
    const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of);
    EXPECT_EQ(m.slices[0].rate_source, m.slices.size() == 1 ? "assumed" : "term");
    EXPECT_FALSE(m.slices[0].forward.fitted_discount);
    if (m.slices.size() == 2) {
      EXPECT_NEAR(-std::log(m.slices[0].forward.discount) / m.slices[0].years, .05, 1e-10);
      EXPECT_EQ(m.slices[1].rate_source, "parity");
    }
  }
}

TEST(ChainAnalytics, UnpricedItmOiContributesToVexAndGammaFlip) {
  ChainBook book;
  const auto as_of = md::new_york_to_utc({2025, 10, 22}, 16, 0);
  book.apply(md::UnderlyingQuote{"SPX", as_of, 100, 100, 100});
  add_option(book, 0, {2026, 10, 22}, OptionType::Call, 100, as_of, 100, .5, 100);
  add_option(book, 1, {2026, 10, 22}, OptionType::Call, 150, as_of, 100, .5, 0);
  add_option(book, 2, {2026, 10, 22}, OptionType::Put, 150, as_of, 100, .5, 114);
  book.apply(md::OptionQuote{2, as_of, 1, 2, 1, 1});  // sub-intrinsic ITM quote
  analytics::AnalyticsOptions options;
  options.fallback_rate = 0;
  options.parity_strikes = 0;  // isolate positions from the deliberately bad parity quote
  const auto m = analytics::analyze(book.underlyings().at("SPX"), book, as_of, options);
  const auto& row = m.slices[0].strikes[1];
  EXPECT_FALSE(std::isfinite(row.put.iv));
  EXPECT_NEAR(row.iv, .5, 1e-9);
  EXPECT_NEAR(row.vex, -row.put.vanna * 114 * 100 * 100, 1e-9);
  const double root =
      100 * std::exp(std::log(1.5) / 2 - .125 - .25 * std::log(1.14) / std::log(1.5));
  EXPECT_NEAR(m.exposure.gamma_flip, root, 1e-6);
  EXPECT_EQ(m.coverage.priced, 2);
  EXPECT_EQ(m.exposure.oi_coverage, 1);
}

}  // namespace

namespace {
TEST(ChainAnalytics, IgnoresSpotMoreThanThirtyMinutesBehindOptionData) {
  SyntheticChain chain;
  // An ETF's 15:59:59 closing print against its options' 16:15 close stays the spot.
  chain.book.apply(
      md::UnderlyingQuote{"SPX", chain.as_of - 15 * md::kNanosPerMinute - md::kNanosPerSecond, 0,
                          0, kSpot});
  EXPECT_EQ(analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of)
                .spot_source,
            "quote");
  const auto boundary = chain.as_of - 30 * md::kNanosPerMinute;
  chain.book.apply(md::UnderlyingQuote{"SPX", boundary, 0, 0, kSpot});
  const auto current =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  EXPECT_EQ(current.spot_source, "quote");
  // A deliberately wrong old print must affect neither strike selection nor Greeks.
  chain.book.apply(md::UnderlyingQuote{"SPX", boundary - 1, 0, 0, 9000});
  const auto stale =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  chain.book.apply(md::UnderlyingQuote{"SPX", boundary - 1, 0, 0, 0});
  const auto absent =
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  EXPECT_EQ(stale.spot_source, "parity");
  EXPECT_NEAR(stale.spot, chain.forward * chain.discount, 1e-8);
  EXPECT_DOUBLE_EQ(stale.spot, absent.spot);
  EXPECT_DOUBLE_EQ(stale.slices[0].forward.forward, absent.slices[0].forward.forward);
  EXPECT_DOUBLE_EQ(stale.slices[0].strikes[20].call.gamma, absent.slices[0].strikes[20].call.gamma);
}

TEST(ChainAnalytics, VolumeIsLatestCumulativeValueAndOnlyForItsTradingDate) {
  SyntheticChain chain;
  const auto analyze = [&](md::Timestamp time) {
    return analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, time);
  };
  EXPECT_TRUE(std::isnan(chain.book.option(0)->volume));
  chain.book.apply(md::OptionVolume{9999, chain.as_of, 99});
  chain.book.apply(md::OptionVolume{0, chain.as_of, 100});
  chain.book.apply(md::OptionVolume{0, chain.as_of + 1, 90});  // provider correction
  chain.book.apply(md::OptionVolume{0, chain.as_of, 200});    // older observation
  chain.book.apply(md::OptionVolume{1, chain.as_of, 0});
  chain.book.apply(md::OptionVolume{2, chain.as_of, -1});
  chain.book.apply(md::OptionVolume{2, chain.as_of, analytics::kNaN});
  EXPECT_EQ(chain.book.option(0)->volume, 90);
  EXPECT_EQ(chain.book.option(0)->volume_ts, chain.as_of + 1);
  auto metrics = analyze(chain.as_of + 1);
  EXPECT_EQ(metrics.coverage.volume, 2);
  EXPECT_EQ(metrics.slices[0].coverage.volume, 2);
  EXPECT_EQ(metrics.slices[0].strikes[0].call.volume, 90);
  EXPECT_EQ(metrics.slices[0].strikes[0].put.volume, 0);
  EXPECT_TRUE(std::isnan(metrics.slices[0].strikes[1].call.volume));
  const auto overnight = md::new_york_to_utc({2026, 9, 22}, 20, 15);
  metrics = analyze(overnight);
  EXPECT_EQ(metrics.coverage.volume, 0);
  EXPECT_TRUE(std::isnan(metrics.slices[0].strikes[0].call.volume));
  chain.book.apply(md::OptionVolume{0, overnight, 0});
  EXPECT_EQ(analyze(overnight).slices[0].strikes[0].call.volume, 0);
  EXPECT_EQ(analyze(overnight).coverage.volume, 1);
}

}  // namespace
