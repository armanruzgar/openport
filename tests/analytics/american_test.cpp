#include <gtest/gtest.h>

#include <algorithm>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/pricing/american.hpp"
#include "openport/pricing/implied_vol.hpp"
#include "support/american_chain.hpp"

namespace {
using namespace openport;
using pricing::ExerciseStyle;
using pricing::OptionType;

TEST(AmericanAnalytics, CurveAndOneCorrectionRecoverForwardAndOtmVol) {
  for (double years : {.25, 1.0}) {
    analytics::ChainBook book;
    md::InstrumentId id = 0;
    const md::Date expiry{2027, 9, 22};
    // SPY options trade, and expire, at 16:15.
    const auto as_of =
        md::new_york_to_utc(expiry, 16, 15) - static_cast<md::Timestamp>(years * md::kNanosPerYear);
    test::add_american_expiry(book, as_of, expiry, id);
    const auto& underlying = book.underlyings().at("SPY");
    std::vector<analytics::ParityPoint> points;
    for (const auto& [k, pair] : underlying.expiries.begin()->second.strikes)
      points.push_back({k, book.option(pair.call)->mid(), book.option(pair.put)->mid(), 1});
    const auto contaminated = analytics::implied_forward(points, years, .045);
    ASSERT_TRUE(contaminated.fitted_discount);
    EXPECT_GT(std::abs(-std::log(contaminated.discount) / years - .045), .005);

    analytics::AnalyticsOptions options;
    options.discount_curve = analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
    const auto m = analytics::analyze(underlying, book, as_of, options);
    const auto& slice = m.slices.at(0);
    const double forward = 100 * std::exp(.035 * years);
    EXPECT_NEAR(slice.forward.forward, forward, forward * .0005);
    EXPECT_NEAR(slice.forward.discount, std::exp(-.045 * years), 1e-12);
    EXPECT_EQ(slice.rate_source, "curve");
    EXPECT_EQ(slice.rate_curve_symbol, "SPX");
    EXPECT_FALSE(slice.forward.fitted_discount);
    EXPECT_TRUE(slice.deamericanized);
    EXPECT_FALSE(m.american_approximation);
    for (const auto& row : slice.strikes) {
      const auto& otm = row.strike >= forward ? row.call : row.put;
      EXPECT_NEAR(otm.iv, .20, .001) << years << " / " << row.strike;
      EXPECT_NEAR(row.iv, .20, .001);
      EXPECT_NEAR(row.call.gamma, row.put.gamma, 1e-12);
    }

    options.deamericanize = false;
    const auto raw = analytics::analyze(underlying, book, as_of, options);
    EXPECT_TRUE(raw.american_approximation);
    EXPECT_FALSE(raw.slices[0].deamericanized);
    // Reproduce the old D-fixed weighted-mean path on the same near-spot strikes.
    std::sort(points.begin(), points.end(),
              [](auto a, auto b) { return std::abs(a.strike - 100) < std::abs(b.strike - 100); });
    points.resize(options.parity_strikes);
    for (auto& p : points) {
      const auto pair = underlying.expiries.begin()->second.strikes.at(p.strike);
      const auto* call = book.option(pair.call);
      const auto* put = book.option(pair.put);
      const double spread = std::max(call->ask - call->bid + put->ask - put->bid,
                                     std::max(.01, .001 * (call->mid() + put->mid())));
      p.weight = 1 / (spread * spread);
    }
    const auto fixed = analytics::implied_forward_given_discount(points, std::exp(-.045 * years));
    EXPECT_NEAR(raw.slices[0].forward.forward, fixed.forward, 1e-12);
    EXPECT_EQ(raw.slices[0].rate_source, "curve");
    EXPECT_GT(std::abs(raw.slices[0].forward.forward / forward - 1), .0005);
  }
}

TEST(AmericanAnalytics, NeverFitsItsOwnRateOrBorrowsAmericanLongRates) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  for (auto expiry : {md::Date{2026, 9, 23}, md::Date{2026, 12, 22}, md::Date{2027, 9, 22}})
    test::add_american_expiry(book, as_of, expiry, id, 100, 80, 2.5, 17, 101);
  analytics::AnalyticsOptions options;
  options.fallback_rate = .037;
  const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  for (const auto& s : m.slices) {
    EXPECT_EQ(s.rate_source, "assumed");
    EXPECT_TRUE(s.rate_curve_symbol.empty());
    EXPECT_FALSE(s.forward.fitted_discount);
    EXPECT_NEAR(-std::log(s.forward.discount) / s.years, .037, 1e-12);
  }
  EXPECT_EQ(analytics::make_discount_curve(m), nullptr);
}

TEST(DiscountCurve, UsesOnlyFittedEuropeanTenorsAndInterpolatesZeroRates) {
  analytics::UnderlyingMetrics m;
  m.symbol = "SPX";
  auto add = [&](double t, double r, bool fitted, ExerciseStyle style) {
    analytics::SliceMetrics s;
    s.years = t;
    s.style = style;
    s.forward = {100, std::exp(-r * t), 12, fitted, true};
    m.slices.push_back(s);
  };
  add(.01, .25, false, ExerciseStyle::European);
  add(.2, -.03, true, ExerciseStyle::American);
  add(1, .04, true, ExerciseStyle::European);
  EXPECT_EQ(analytics::make_discount_curve(m), nullptr);
  add(3, .06, true, ExerciseStyle::European);
  const auto curve = analytics::make_discount_curve(m);
  ASSERT_NE(curve, nullptr);
  EXPECT_EQ(curve->symbol(), "SPX");
  EXPECT_NEAR(curve->rate(.001), .04, 1e-12);
  EXPECT_NEAR(curve->rate(1), .04, 1e-12);
  EXPECT_NEAR(curve->rate(2), .05, 1e-12);
  EXPECT_NEAR(curve->rate(3), .06, 1e-12);
  EXPECT_NEAR(curve->rate(5), .06, 1e-12);
  EXPECT_EQ(analytics::DiscountCurve::from_points("SPX", {{1, .04}, {1, .05}}), nullptr);
}

TEST(AmericanAnalytics, PremiumUsesSameLatticeAndHasMeasuredConvergence) {
  double largest = 0;
  for (double t : {.25, 1.0})
    for (double strike = 80; strike <= 120; strike += 2.5)
      for (auto type : {OptionType::Call, OptionType::Put}) {
        const pricing::BsmInputs in{type, 100, strike, t, .045, .01, .20};
        const double eep = pricing::binomial_early_exercise_premium(in, 31);
        const double reference = pricing::binomial_early_exercise_premium(in, 2001);
        largest = std::max(largest, std::abs(eep - reference));
        EXPECT_NEAR(eep,
                    pricing::binomial_price(in, ExerciseStyle::American,
                                            pricing::TreeMethod::LeisenReimer, 31) -
                        pricing::binomial_price(in, ExerciseStyle::European,
                                                pricing::TreeMethod::LeisenReimer, 31),
                    1e-12);
      }
  EXPECT_LT(largest, .039);
  EXPECT_DOUBLE_EQ(
      pricing::binomial_early_exercise_premium({OptionType::Call, 100, 100, 1, .045, 0, .2}, 31),
      0);
}
}  // namespace

namespace {
TEST(AmericanAnalytics, DisplayedQuotesStayRawWhileAllIvSolvesRemoveTheRecordedPremium) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  test::add_american_expiry(book, as_of, {2027, 9, 22}, id);
  analytics::AnalyticsOptions options;
  options.fallback_rate = .045;
  const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  const auto& slice = m.slices[0];
  int positive_premiums = 0;
  for (const auto& row : slice.strikes) {
    for (const auto* quote : {&row.call, &row.put}) {
      const auto* raw = book.option(quote->id);
      EXPECT_DOUBLE_EQ(quote->bid, raw->bid);
      EXPECT_DOUBLE_EQ(quote->ask, raw->ask);
      EXPECT_DOUBLE_EQ(quote->mid, raw->mid());
      ASSERT_TRUE(std::isfinite(quote->eep));
      positive_premiums += quote->eep > .01;
      for (auto [price, iv] :
           {std::pair{quote->mid, quote->iv}, std::pair{quote->bid, quote->bid_iv},
            std::pair{quote->ask, quote->ask_iv}}) {
        if (!std::isfinite(iv)) continue;
        EXPECT_NEAR(pricing::black_price(raw->contract.type, slice.forward.forward, row.strike,
                                         slice.years, iv, slice.forward.discount),
                    price - quote->eep, 1e-8);
      }
    }
  }
  EXPECT_GT(positive_premiums, 0);
  options.deamericanize = false;
  const auto raw = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  for (const auto& row : raw.slices[0].strikes) {
    EXPECT_TRUE(std::isnan(row.call.eep));
    EXPECT_TRUE(std::isnan(row.put.eep));
  }
}

TEST(AmericanAnalytics, OneDayCarryBorrowsLongTenorsDespiteFiveBasisPointSpotMismatch) {
  for (double mismatch : {-.0005, .0005}) {
    analytics::ChainBook book;
    md::InstrumentId id = 0;
    const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
    test::add_american_expiry(book, as_of, {2026, 9, 23}, id, 100, 97, .25, 25);
    test::add_american_expiry(book, as_of, {2027, 9, 22}, id);
    book.apply(md::UnderlyingQuote{"SPY", as_of, 0, 0, 100 * (1 + mismatch)});
    analytics::AnalyticsOptions options;
    options.fallback_rate = .045;
    auto maximum_error = [](const analytics::UnderlyingMetrics& m) {
      double error = 0;
      for (const auto& row : m.slices[0].strikes) error = std::max(error, std::abs(row.iv - .2));
      return error;
    };
    const auto borrowed = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    EXPECT_LT(maximum_error(borrowed), .0001);  // <0.01 vol points
    EXPECT_NEAR(borrowed.slices[0].forward.forward, 100 * std::exp(.035 / 365), .001);
    options.min_days_for_rate = 0;  // force own tenor carry as a comparison
    const auto own = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    EXPECT_LT(maximum_error(borrowed), .3 * maximum_error(own));
  }
}

TEST(AmericanAnalytics, NoLongCarryFallsBackToOwnYieldWithDocumentedClamp) {
  for (double spot : {99.95, 100.1}) {
    analytics::ChainBook book;
    md::InstrumentId id = 0;
    const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
    test::add_american_expiry(book, as_of, {2026, 9, 23}, id, 100, 100, 1, 1);
    book.apply(md::UnderlyingQuote{"SPY", as_of, 0, 0, spot});
    const double t = md::years_between(as_of, book.option(0)->expiry_time);
    const double d = std::exp(-.045 * t);
    const double f0 = 100 + (book.option(0)->mid() - book.option(1)->mid()) / d;
    const double own_q = .045 - std::log(f0 / spot) / t;
    EXPECT_TRUE(own_q < analytics::kMinTreeDividend || own_q > analytics::kMaxTreeDividend);
    analytics::AnalyticsOptions options;
    options.fallback_rate = .045;
    const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    for (const auto* side : {&m.slices[0].strikes[0].call, &m.slices[0].strikes[0].put}) {
      const auto* raw = book.option(side->id);
      const auto iv = pricing::implied_vol_black(raw->mid(), raw->contract.type, f0, 100, t, d);
      ASSERT_TRUE(iv.ok());
      const double expected = pricing::binomial_early_exercise_premium(
          {raw->contract.type, spot, 100, t, .045,
           std::clamp(own_q, analytics::kMinTreeDividend, analytics::kMaxTreeDividend), iv.vol},
          analytics::kDeamericanizationSteps);
      EXPECT_NEAR(side->eep, expected, 1e-10);
    }
  }
}

TEST(AmericanAnalytics, CashDividendCorrectionRecoversEscrowedForwardAndVol) {
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  for (const auto expiry : {md::Date{2026, 12, 22}, md::Date{2027, 9, 22}}) {
    analytics::ChainBook book;
    md::InstrumentId id = 0;
    analytics::AnalyticsOptions options;
    options.fallback_rate = .045;
    options.dividends = {{{2026, 11, 2}, 1.5}, {{2027, 2, 2}, 1.5}};
    std::vector<pricing::CashDividend> cash;
    for (const auto& d : options.dividends)
      cash.push_back({md::years_between(as_of, md::new_york_to_utc(d.ex_date, 0, 0)), d.amount});
    test::add_american_expiry(book, as_of, expiry, id, 100, 80, 2.5, 17, 2001, "SPY", cash);
    const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    const auto& slice = m.slices[0];
    double escrow = 100;
    for (const auto& d : cash)
      if (d.time < slice.years) escrow -= d.amount * std::exp(-.045 * d.time);
    const double forward = escrow * std::exp(.035 * slice.years);
    EXPECT_TRUE(slice.deamericanized);
    EXPECT_EQ(slice.dividends.size(), expiry.year == 2026 ? 1u : 2u);
    EXPECT_NEAR(slice.forward.forward, forward, forward * .001);
    for (const auto& row : slice.strikes) {
      ASSERT_TRUE(std::isfinite(row.iv));
      EXPECT_NEAR(row.iv, .2, .005) << md::format_date(expiry) << " / " << row.strike;
      for (const auto* side : {&row.call, &row.put}) {
        EXPECT_DOUBLE_EQ(side->mid, book.option(side->id)->mid());
        if (std::isfinite(side->iv)) {
          EXPECT_NEAR(pricing::black_price(side->contract.type, slice.forward.forward, row.strike,
                                           slice.years, side->iv, slice.forward.discount),
                      side->mid - side->eep, 1e-8);
        }
      }
    }
  }
}

TEST(AmericanAnalytics, CashResidualCarryPreservesOwnForwardWithoutClampOrTermBorrowing) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  const md::Date ex_date{2026, 9, 23};
  const double time = md::years_between(as_of, md::new_york_to_utc(ex_date, 0, 0));
  const std::vector<pricing::CashDividend> cash{{time, 1}};
  test::add_american_expiry(book, as_of, {2026, 9, 24}, id, 100, 100, 1, 1, 1001, "SPY", cash);
  test::add_american_expiry(book, as_of, {2027, 9, 22}, id);
  analytics::AnalyticsOptions options;
  options.fallback_rate = .045;
  options.dividends = {{ex_date, 1}};
  options.deamericanize = false;
  const auto raw = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  const auto& slice = raw.slices[0];
  const double escrow = 100 - std::exp(-.045 * time);
  const double q = .045 - std::log(slice.forward.forward / escrow) / slice.years;
  ASSERT_TRUE(q < analytics::kMinTreeDividend || q > analytics::kMaxTreeDividend);
  options.deamericanize = true;
  const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  for (const auto* side : {&m.slices[0].strikes[0].call, &m.slices[0].strikes[0].put}) {
    const auto iv = pricing::implied_vol_black(side->mid, side->contract.type,
                                               slice.forward.forward, 100, slice.years,
                                               slice.forward.discount);
    ASSERT_TRUE(iv.ok());
    const double expected = pricing::binomial_early_exercise_premium(
        {side->contract.type, 100, 100, slice.years, .045, q, iv.vol},
        analytics::kDeamericanizationSteps, cash);
    EXPECT_NEAR(side->eep, expected, 1e-10);
  }
}

TEST(AmericanAnalytics, ClosedFormModelsCorrectAndReportThemselvesWithinTheirMeasuredError) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const md::Date expiry{2027, 9, 22};
  const auto as_of = md::new_york_to_utc(expiry, 16, 15) - static_cast<md::Timestamp>(.25 * md::kNanosPerYear);
  test::add_american_expiry(book, as_of, expiry, id);
  analytics::AnalyticsOptions options;
  options.discount_curve = analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
  const auto lattice = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  EXPECT_EQ(lattice.slices[0].eep_model, pricing::AmericanModel::LeisenReimer);
  for (auto model : {pricing::AmericanModel::BjerksundStensland2002, pricing::AmericanModel::BaroneAdesiWhaley}) {
    options.american_model = model;
    const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    const auto& slice = m.slices.at(0);
    EXPECT_TRUE(slice.deamericanized);
    EXPECT_EQ(slice.eep_model, model);
    const double forward = 100 * std::exp(.035 * .25);
    EXPECT_NEAR(slice.forward.forward, forward, forward * .0005);
    for (std::size_t i = 0; i < slice.strikes.size(); ++i) {
      const auto& row = slice.strikes[i];
      // Within the 0.15 vol points measured at this tenor (docs/american-analytics.md).
      EXPECT_NEAR(row.iv, .20, .0016) << row.strike;
      for (const auto* side : {&row.call, &row.put}) {
        const auto& reference = side == &row.call ? lattice.slices[0].strikes[i].call : lattice.slices[0].strikes[i].put;
        // The same contract's EEP within six cents of the lattice's (deep ITM puts differ most).
        EXPECT_NEAR(side->eep, reference.eep, .06) << row.strike;
      }
    }
  }
}

TEST(AmericanAnalytics, ClosedFormModelsLeaveCashDividendExpiriesOnTheLattice) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  const md::Date ex_date{2026, 12, 1};
  const std::vector<pricing::CashDividend> cash{{md::years_between(as_of, md::new_york_to_utc(ex_date, 0, 0)), 1}};
  test::add_american_expiry(book, as_of, {2027, 3, 19}, id, 100, 80, 2.5, 17, 301, "SPY", cash);
  test::add_american_expiry(book, as_of, {2026, 10, 16}, id, 100, 80, 2.5, 17, 301);
  analytics::AnalyticsOptions options;
  options.fallback_rate = .045;
  options.dividends = {{ex_date, 1}};
  options.american_model = pricing::AmericanModel::BaroneAdesiWhaley;
  const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  ASSERT_EQ(m.slices.size(), 2u);
  for (const auto& slice : m.slices) {
    EXPECT_TRUE(slice.deamericanized);
    // The later expiry spans the payment; the earlier one does not.
    EXPECT_EQ(slice.eep_model, slice.dividends.empty() ? pricing::AmericanModel::BaroneAdesiWhaley
                                                        : pricing::AmericanModel::LeisenReimer);
  }
  EXPECT_NE(m.slices[0].dividends.empty(), m.slices[1].dividends.empty());
}

TEST(AmericanAnalytics, UsesOnlyFutureCashBeforeExpiryAndReportsItInDateOrder) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  for (const auto expiry : {md::Date{2026, 9, 23}, md::Date{2026, 9, 24}})
    test::add_american_expiry(book, as_of, expiry, id, 100, 99, 1, 3, 101);
  analytics::AnalyticsOptions options;
  options.dividends = {{{2026, 9, 24}, .2}, {{2026, 9, 22}, .3}, {{2026, 9, 23}, .1},
                       {{2026, 9, 25}, .4}, {{2026, 9, 21}, .5}};
  const auto& underlying = book.underlyings().at("SPY");
  const auto m = analytics::analyze(underlying, book, as_of, options);
  ASSERT_EQ(m.slices[0].dividends.size(), 1u);
  ASSERT_EQ(m.slices[1].dividends.size(), 2u);
  EXPECT_EQ(m.slices[0].dividends[0].ex_date, (md::Date{2026, 9, 23}));
  EXPECT_DOUBLE_EQ(m.slices[0].dividends[0].amount, .1);
  EXPECT_EQ(m.slices[1].dividends[0].ex_date, (md::Date{2026, 9, 23}));
  EXPECT_EQ(m.slices[1].dividends[1].ex_date, (md::Date{2026, 9, 24}));
  const auto ex_time = md::new_york_to_utc({2026, 9, 23}, 0, 0);
  EXPECT_EQ(analytics::analyze(underlying, book, ex_time - 1, options).slices[0].dividends.size(), 1u);
  EXPECT_TRUE(analytics::analyze(underlying, book, ex_time, options).slices[0].dividends.empty());
  options.deamericanize = false;
  for (const auto& s : analytics::analyze(underlying, book, as_of, options).slices) {
    EXPECT_TRUE(s.dividends.empty());
    EXPECT_FALSE(s.deamericanized);
  }
}

TEST(AmericanAnalytics, IrrelevantOrInvalidCashLeavesExistingNumbersExactlyUnchanged) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  test::add_american_expiry(book, as_of, {2026, 12, 22}, id, 100, 90, 2.5, 9, 101);
  const auto& underlying = book.underlyings().at("SPY");
  const auto original = analytics::analyze(underlying, book, as_of);
  analytics::AnalyticsOptions options;
  options.dividends = {{{2026, 9, 22}, 1}, {{2026, 12, 23}, 1}, {{2026, 11, 1}, -1},
                       {{2026, 11, 1}, analytics::kNaN}, {{2026, 13, 1}, 1}};
  const auto m = analytics::analyze(underlying, book, as_of, options);
  EXPECT_TRUE(m.slices[0].dividends.empty());
  EXPECT_DOUBLE_EQ(m.slices[0].forward.forward, original.slices[0].forward.forward);
  EXPECT_DOUBLE_EQ(m.exposure.gex, original.exposure.gex);
  EXPECT_DOUBLE_EQ(m.exposure.vex, original.exposure.vex);
  for (std::size_t i = 0; i < m.slices[0].strikes.size(); ++i) {
    const auto& actual = m.slices[0].strikes[i];
    const auto& expected = original.slices[0].strikes[i];
    EXPECT_DOUBLE_EQ(actual.iv, expected.iv);
    EXPECT_DOUBLE_EQ(actual.call.eep, expected.call.eep);
    EXPECT_DOUBLE_EQ(actual.put.eep, expected.put.eep);
    EXPECT_DOUBLE_EQ(actual.call.delta, expected.call.delta);
  }
  options.dividends = {{{2026, 11, 1}, 200}};
  const auto invalid = analytics::analyze(underlying, book, as_of, options);
  EXPECT_TRUE(invalid.american_approximation);
  EXPECT_FALSE(invalid.slices[0].deamericanized);
  EXPECT_TRUE(invalid.slices[0].dividends.empty());
  EXPECT_TRUE(std::isnan(invalid.slices[0].strikes[0].call.eep));
}
}  // namespace
