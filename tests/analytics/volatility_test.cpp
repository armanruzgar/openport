#include "openport/analytics/volatility.hpp"
#include "openport/pricing/black.hpp"

#include <gtest/gtest.h>
#include <cmath>
#include <sstream>

namespace {
using namespace openport;
using namespace openport::analytics;

SliceMetrics flat_chain(double sigma = 0.2) {
  SliceMetrics slice;
  slice.years = 30.0 / 365;
  slice.forward.forward = 100;
  slice.forward.discount = 0.99;
  for (int i = 200; i <= 1800; ++i) {
    StrikeMetrics row;
    row.strike = i / 10.0;
    row.iv = sigma;
    for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
      auto& side = type == pricing::OptionType::Call ? row.call : row.put;
      const double value = pricing::black_price(type, 100, row.strike, slice.years, sigma, .99);
      side.bid = side.ask = side.mid = value;
    }
    slice.strikes.push_back(row);
  }
  return slice;
}

TEST(Mfiv, DenseFlatBlackStripRecoversVarianceAndDiscount) {
  const auto m = model_free_iv(flat_chain());
  EXPECT_NEAR(m.variance, .04, 3e-6);
  EXPECT_NEAR(m.vol, 20, .001);
  EXPECT_EQ(m.k0, 100);
}

TEST(Mfiv, SkewAndSmileConvexityRaiseVarianceAboveAtm) {
  auto chain = flat_chain();
  for (auto& row : chain.strikes) {
    const double k = std::log(row.strike / 100);
    row.iv = .2 - .2 * k + .7 * k * k;
    for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
      auto& side = type == pricing::OptionType::Call ? row.call : row.put;
      side.bid = side.ask = pricing::black_price(type, 100, row.strike, chain.years, row.iv, .99);
    }
  }
  EXPECT_GT(model_free_iv(chain).variance, .04);
}

SliceMetrics selection_chain() {
  SliceMetrics chain;
  chain.forward.forward = 101;
  chain.forward.discount = .95;
  chain.years = .25;
  for (double strike : {60, 70, 80, 90, 100, 110, 125, 140, 155}) {
    StrikeMetrics row;
    row.strike = strike;
    row.call.bid = row.put.bid = 1;
    row.call.ask = row.put.ask = 3;
    chain.strikes.push_back(row);
  }
  chain.strikes[4].call.ask = 7;  // K0 call mid 4, put mid 2: use 3.
  return chain;
}

TEST(Mfiv, K0AverageExclusionsConsecutiveStopAndSelectedNeighbourWeights) {
  auto chain = selection_chain();
  chain.strikes[3].put.bid = 0;  // 90 is skipped, 80 restarts the zero run.
  chain.strikes[1].put.bid = 0;
  chain.strikes[0].put.bid = 0;
  chain.strikes[7].call.ask = kNaN;  // No ask counts as excluded too.
  chain.strikes[8].call.bid = 0;
  const auto m = model_free_iv(chain);
  EXPECT_EQ(m.k0, 100);
  EXPECT_EQ(m.low, 80);
  EXPECT_EQ(m.high, 125);
  EXPECT_EQ(m.strikes, 4u);
  EXPECT_EQ(m.lower_stop, "zero_bids");
  EXPECT_EQ(m.upper_stop, "zero_bids");
  EXPECT_FALSE(m.truncated);
  // Selected K = 80,100,110,125; dK = 20,15,12.5,15.
  const double sum = 20 * 2 / 6400.0 + 15 * 3 / 10000.0 + 12.5 * 2 / 12100.0 + 15 * 2 / 15625.0;
  EXPECT_NEAR(m.variance, (2 * sum / .95 - .01 * .01) / .25, 1e-14);
}

TEST(Mfiv, WindowEndsAndMissingK0AreExplicit) {
  auto chain = selection_chain();
  auto m = model_free_iv(chain);
  EXPECT_TRUE(m.truncated);
  EXPECT_EQ(m.lower_stop, "window");
  EXPECT_EQ(m.upper_stop, "window");
  chain.strikes[4].put.bid = 0;
  m = model_free_iv(chain);
  EXPECT_TRUE(std::isnan(m.vol));
  EXPECT_EQ(m.reason, "missing_k0_quotes");
  chain.forward.forward = 10;
  EXPECT_EQ(model_free_iv(chain).reason, "no_strike_at_or_below_forward");
}

TEST(Mfiv, AmericanEepIsRemovedAndPartialOrMissingCorrectionsAreFlagged) {
  auto chain = selection_chain();
  const double expected = model_free_iv(chain).variance;
  chain.style = pricing::ExerciseStyle::American;
  for (auto& row : chain.strikes) for (auto* side : {&row.call, &row.put}) {
    side->bid += .5; side->ask += .5; side->eep = .5;
  }
  auto m = model_free_iv(chain);
  EXPECT_NEAR(m.variance, expected, 1e-14);
  EXPECT_EQ(m.eep, "removed"); EXPECT_FALSE(m.proxy);
  chain.strikes[0].put.eep = kNaN;
  m = model_free_iv(chain);
  EXPECT_EQ(m.eep, "partial"); EXPECT_TRUE(m.proxy);
  for (auto& row : chain.strikes) row.call.eep = row.put.eep = kNaN;
  m = model_free_iv(chain);
  EXPECT_EQ(m.eep, "unavailable"); EXPECT_TRUE(m.proxy);
  EXPECT_GT(m.variance, expected);
}

TEST(Mfiv, ConstantMaturityUsesTotalVarianceInMinutesAndNeverExtrapolates) {
  const std::vector<VarianceKnot> knots{{20 * 1440, .04, true, false}, {40 * 1440, .09, false, true}};
  const auto value = constant_vol(knots, 30, 1440);
  EXPECT_NEAR(value.variance, (.04 * 20 + .09 * 40) / 60, 1e-14);
  EXPECT_TRUE(value.truncated); EXPECT_TRUE(value.proxy);
  EXPECT_EQ(value.near, 0u); EXPECT_EQ(value.next, 1u);
  EXPECT_DOUBLE_EQ(constant_vol(knots, 20).variance, .04);
  EXPECT_EQ(constant_vol(knots, 9).reason, "target_not_bracketed");
  EXPECT_TRUE(std::isnan(constant_vol(knots, 50).vol));
  const std::vector<VarianceKnot> short_knots{{700, .04}, {3 * 1440, .04}};
  EXPECT_TRUE(std::isnan(constant_vol(short_knots, 1, 1440).vol));
  const std::vector<VarianceKnot> missing{{20 * 1440, kNaN}, {40 * 1440, .04}};
  EXPECT_EQ(constant_vol(missing, 30).reason, "unusable_bracketing_expiry");
}

SviFit known_fit() {
  SviFit fit;
  fit.parameters = {.01, .03, -.5, 0, .2};
  fit.status = SviStatus::Ok;
  fit.min_k = -.8; fit.max_k = .8; fit.years = .5;
  fit.butterfly = svi_butterfly(fit.parameters, fit.min_k, fit.max_k);
  return fit;
}

TEST(VolSmile, DeltaRootsUseTheirOwnSmileAndUndiscountedForwardDelta) {
  auto chain = flat_chain();
  chain.years = .5;
  auto fit = known_fit();
  ASSERT_TRUE(fit.butterfly.ok);
  const auto result = smile_metrics(chain, &fit);
  EXPECT_EQ(result.source, "svi");
  EXPECT_NEAR(result.atm, 100 * svi_iv(fit.parameters, 0, .5), 1e-12);
  for (const double delta : {.25, .1}) {
    const auto& skew = delta == .25 ? result.delta25 : result.delta10;
    for (const bool call : {true, false}) {
      const auto& point = call ? skew.call : skew.put;
      const double sigma = svi_iv(fit.parameters, std::log(point.strike / 100), .5);
      const auto greeks = pricing::black_greeks(call ? pricing::OptionType::Call : pricing::OptionType::Put, 100, point.strike, .5, sigma, 1);
      EXPECT_NEAR(greeks.delta, call ? delta : -delta, 1e-10);
      EXPECT_NEAR(point.vol, sigma * 100, 1e-12);
    }
    EXPECT_NEAR(skew.rr, skew.call.vol - skew.put.vol, 1e-12);
    EXPECT_NEAR(skew.bf, (skew.call.vol + skew.put.vol) / 2 - result.atm, 1e-12);
    EXPECT_LT(skew.rr, 0);
  }
  const double h = 1e-4;
  const double center = svi_iv(fit.parameters, 0, .5);
  EXPECT_NEAR(result.slope, 100 * (svi_iv(fit.parameters, h, .5) - svi_iv(fit.parameters, -h, .5)) / (2*h), 1e-5);
  EXPECT_NEAR(result.curvature, 100 * (svi_iv(fit.parameters, h, .5) + svi_iv(fit.parameters, -h, .5) - 2*center) / (h*h), 1e-4);
}

TEST(VolSmile, FailedChecksFallBackAndSparseSmilesDoNotExtrapolate) {
  auto chain = selection_chain();
  for (auto& row : chain.strikes) row.iv = .2 + .1 * std::log(row.strike / 101);
  auto fit = known_fit(); fit.butterfly.ok = false;
  auto result = smile_metrics(chain, &fit);
  EXPECT_EQ(result.source, "smile_interpolation");
  EXPECT_NEAR(result.atm, 20, 1e-12);
  EXPECT_TRUE(std::isnan(result.slope));
  chain.strikes.erase(chain.strikes.begin(), chain.strikes.begin() + 5);
  result = smile_metrics(chain);
  EXPECT_TRUE(std::isnan(result.atm));
  EXPECT_EQ(result.reason, "forward_outside_smile");
  EXPECT_EQ(result.delta25.put.reason, "delta_outside_smile");
}

TEST(VolSmile, ConstantDeltaPointsInterpolateVarianceBeforeRiskReversal) {
  UnderlyingMetrics m;
  m.as_of = md::new_york_to_utc({2026, 9, 1}, 16, 0);
  for (const int days : {20, 40}) {
    auto slice = flat_chain(days == 20 ? .2 : .4);
    slice.expiry_time = m.as_of + days * md::kNanosPerDay;
    slice.expiry = md::new_york_time(slice.expiry_time).date;
    m.slices.push_back(slice);
  }
  const auto v = volatility_metrics(m);
  const double expected = 100 * std::sqrt((20*.04 + 40*.16) / 60);
  EXPECT_NEAR(v.atm[1].vol, expected, 1e-12);
  EXPECT_NEAR(v.skew25.call.vol, expected, 1e-12);
  EXPECT_NEAR(v.skew25.rr, 0, 1e-12);
  EXPECT_NEAR(v.skew25.bf, 0, 1e-12);
  EXPECT_TRUE(v.skew_proxy);
}

void add_expiry(UnderlyingMetrics& m, VolatilityMetrics& v, md::Date date, double total, int hour = 16) {
  SliceMetrics s;
  s.expiry = date; s.expiry_time = md::new_york_to_utc(date, hour, hour == 9 ? 30 : 0);
  s.forward.forward = 100; s.years = md::years_between(m.as_of, s.expiry_time);
  m.slices.push_back(s);
  ExpiryVolatility e;
  e.minutes = s.years * kMinutesPerYear; e.mfiv.variance = total / s.years;
  e.mfiv.truncated = false;
  v.expiries.push_back(e);
}

TEST(ImpliedMoves, SessionsSkipWeekendAndHolidayAndShareVariance) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 7, 2}, 16, 0);  // July 3 observed holiday.
  add_expiry(m, v, {2026, 7, 7}, .0008);
  const std::vector<EventLabel> events{{{2026, 7, 6}, "Supplied event"}};
  const auto moves = implied_moves(m, v, events);
  ASSERT_EQ(moves.sessions.size(), 15u);
  EXPECT_EQ(moves.sessions[0].date, (md::Date{2026, 7, 6}));
  EXPECT_NEAR(moves.sessions[0].percent, 2, 1e-12);
  EXPECT_NEAR(moves.sessions[1].points, 2, 1e-12);
  EXPECT_TRUE(moves.sessions[0].shared);
  EXPECT_EQ(moves.sessions[0].label, "Supplied event");
  EXPECT_TRUE(std::isnan(moves.sessions[2].points));
}

TEST(ImpliedMoves, EarlyCloseIsOneSessionAndNegativeForwardVarianceIsFlagged) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 11, 25}, 16, 0);
  add_expiry(m, v, {2026, 11, 27}, .0004, 13);
  add_expiry(m, v, {2026, 11, 30}, .0003);
  const auto moves = implied_moves(m, v);
  EXPECT_NEAR(moves.sessions[0].points, 2, 1e-12);
  EXPECT_TRUE(moves.sessions[1].calendar_arbitrage);
  EXPECT_TRUE(std::isnan(moves.sessions[1].points));
  EXPECT_EQ(moves.sessions[1].reason, "negative_forward_variance");
}

TEST(ImpliedMoves, SameDayRemainingAndAmOvernightAllocation) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 9, 25}, 8, 0);
  add_expiry(m, v, {2026, 9, 25}, .0001, 9);
  add_expiry(m, v, {2026, 9, 25}, .0004);
  const auto moves = implied_moves(m, v);
  EXPECT_NEAR(moves.today_percent, 2, 1e-12);
  EXPECT_NEAR(moves.sessions[0].points, 2, 1e-12);
}

TEST(ImpliedMoves, AtmProxyAndUnusableIntervalsRemainVisible) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 9, 25}, 12, 0);
  add_expiry(m, v, {2026, 9, 25}, .0004);
  v.expiries[0].mfiv.variance = kNaN;
  v.expiries[0].smile.atm = 20;
  auto moves = implied_moves(m, v);
  EXPECT_TRUE(moves.sessions[0].proxy);
  EXPECT_TRUE(std::isfinite(moves.sessions[0].points));
  v.expiries[0].smile.atm = kNaN;
  moves = implied_moves(m, v);
  EXPECT_TRUE(std::isnan(moves.sessions[0].points));
  EXPECT_EQ(moves.sessions[0].reason, "unusable_variance");
}


TEST(EventVolatility, SolvesTwoExpiriesAndKeepsEventPremiumSeparateFromSessionMove) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 9, 21}, 16, 0);
  add_expiry(m, v, {2026, 9, 22}, .0001);
  add_expiry(m, v, {2026, 9, 24}, .0007);
  const std::vector<EventLabel> events{{{2026, 9, 23}, "Earnings", "before_open"}};
  const auto result = event_volatility(m, v, events);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_TRUE(result[0].bracketed);
  EXPECT_EQ(result[0].near, 0u); EXPECT_EQ(result[0].next, 1u);
  EXPECT_NEAR(result[0].baseline_variance, .0001, 1e-12);
  EXPECT_NEAR(result[0].variance_premium, .0004, 1e-12);
  EXPECT_NEAR(result[0].percent, 2, 1e-12); EXPECT_NEAR(result[0].points, 2, 1e-12);
  EXPECT_TRUE(result[0].reason.empty());
  EXPECT_FALSE(result[0].proxy);
}
TEST(EventVolatility, AmcMovesToNextSessionAndDoesNotEnterSameDayPmExpiry) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 9, 24}, 16, 0);
  add_expiry(m, v, {2026, 9, 25}, .0001);
  add_expiry(m, v, {2026, 9, 28}, .0006, 9);
  const std::vector<EventLabel> events{{{2026, 9, 25}, "Earnings", "after_close"}};
  const auto result = event_volatility(m, v, events);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result[0].session, (md::Date{2026, 9, 28}));
  EXPECT_NEAR(result[0].percent, 2, 1e-12);
  EXPECT_EQ(implied_moves(m, v, events).sessions[1].label, "Earnings");
  auto bmo = events; bmo[0].timing = "before_open";
  EXPECT_EQ(event_volatility(m, v, bmo)[0].reason, "event_not_bracketed");
}
TEST(EventVolatility, EarlyCloseHolidayAndFractionalBaselineUseSessionAllocation) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 11, 25}, 12, 45);
  add_expiry(m, v, {2026, 11, 25}, .00005);
  add_expiry(m, v, {2026, 11, 27}, .00055, 13);
  const std::vector<EventLabel> events{{{2026, 11, 27}, "News", "before_open"}};
  const auto result = event_volatility(m, v, events);
  EXPECT_NEAR(result[0].baseline_variance, .0001, 1e-12);
  EXPECT_NEAR(result[0].percent, 2, 1e-12);
}
TEST(EventVolatility, ExactIntradayEventBetweenAmAndPmKeepsOrdinaryIntervalVariance) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 9, 25}, 8, 0);
  add_expiry(m, v, {2026, 9, 25}, .0001, 9);
  add_expiry(m, v, {2026, 9, 25}, .0006);
  const std::vector<EventLabel> events{{{2026, 9, 25}, "News", "exact", md::new_york_to_utc({2026, 9, 25}, 14, 0)}};
  const auto result = event_volatility(m, v, events);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_NEAR(result[0].variance_premium, .0004, 1e-12);
  EXPECT_NEAR(result[0].percent, 2, 1e-12);
}
TEST(EventVolatility, MissingBracketsCompetingEventsAndNegativePremiumsStayUnavailable) {
  UnderlyingMetrics m; VolatilityMetrics v;
  m.as_of = md::new_york_to_utc({2026, 9, 21}, 16, 0);
  add_expiry(m, v, {2026, 9, 22}, .0002);
  add_expiry(m, v, {2026, 9, 24}, .0003);
  std::vector<EventLabel> events{{{2026, 9, 23}, "Event"}};
  auto result = event_volatility(m, v, events);
  EXPECT_EQ(result[0].reason, "negative_event_premium");
  EXPECT_TRUE(std::isnan(result[0].percent));
  EXPECT_LT(result[0].variance_premium, 0);
  events.push_back({{2026, 9, 24}, "Other"});
  EXPECT_EQ(event_volatility(m, v, events)[0].reason, "multiple_events_in_solve");
  events.resize(1);
  v.expiries[1].mfiv.variance /= 2;
  EXPECT_TRUE(event_volatility(m, v, events)[0].calendar_arbitrage);
  v.expiries[0].mfiv.variance = kNaN;
  EXPECT_EQ(event_volatility(m, v, events)[0].reason, "unusable_variance");
  for (std::size_t i = 0; i < 2; ++i) v.expiries[i].smile.atm = 100 * std::sqrt((i == 0 ? .0001 : .0007) / m.slices[i].years);
  result = event_volatility(m, v, events);
  EXPECT_TRUE(result[0].proxy); EXPECT_NEAR(result[0].percent, 2, 1e-12);
  events[0].date = {2026, 9, 25};
  EXPECT_EQ(event_volatility(m, v, events)[0].reason, "event_not_bracketed");
  events[0].date = {2026, 9, 21};
  EXPECT_TRUE(event_volatility(m, v, events).empty());
}
TEST(VolEvents, ValidatesLabelsDatesAndDuplicateDates) {
  std::istringstream good("# user supplied\nDate,Label\n2026-10-02, Payrolls \n2026-09-30,Quarter end, rebalance\n");
  const auto events = parse_events(good);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_EQ(events[0].label, "Quarter end, rebalance");
  EXPECT_EQ(events[1].date, (md::Date{2026, 10, 2}));
  for (const auto* text : {"2026-02-30,Wrong", "2026-09-30,", "no comma", "9999-01-01,Far", "2026-09-30,A\n2026-09-30,B"}) {
    std::istringstream bad(text);
    EXPECT_THROW((void)parse_events(bad), std::invalid_argument) << text;
  }
}
}  // namespace

namespace {
TEST(VolSmile, AmAndPmExpiriesOnOneDayBracketTheSameConstantMaturity) {
  UnderlyingMetrics m;
  m.as_of = md::new_york_to_utc({2026, 9, 16}, 12, 0);
  for (const int hour : {9, 16}) {
    auto slice = flat_chain(hour == 9 ? .2 : .3);
    slice.expiry = {2026, 10, 16};
    slice.expiry_time = md::new_york_to_utc(slice.expiry, hour, hour == 9 ? 30 : 0);
    m.slices.push_back(slice);
  }
  const auto v = volatility_metrics(m);
  ASSERT_TRUE(std::isfinite(v.mfiv[1].vol));
  EXPECT_EQ(v.mfiv[1].near, 0u); EXPECT_EQ(v.mfiv[1].next, 1u);
  EXPECT_GT(v.atm[1].vol, 20); EXPECT_LT(v.atm[1].vol, 30);
}

TEST(VolSmile, CalendarViolationsRejectBothFitsForMetricUse) {
  UnderlyingMetrics m;
  m.as_of = md::new_york_to_utc({2026, 9, 1}, 12, 0);
  std::vector<SviFit> fits;
  for (const int days : {100, 200}) {
    auto slice = flat_chain();
    slice.expiry_time = m.as_of + days * md::kNanosPerDay;
    auto fit = known_fit();
    fit.years = days / 365.0;
    if (days == 200) fit.parameters.a = .001;
    fits.push_back(fit); m.slices.push_back(slice);
  }
  ASSERT_FALSE(svi_calendar(fits).empty());
  const auto v = volatility_metrics(m, fits);
  EXPECT_EQ(v.expiries[0].smile.source, "smile_interpolation");
  EXPECT_EQ(v.expiries[1].smile.source, "smile_interpolation");
}

TEST(VolDerived, VrpPreservesMissingZeroAndQualityFlags) {
  ConstantVol iv;
  iv.vol = 20; iv.truncated = true;
  auto vrp = variance_risk_premium(iv, 16);
  EXPECT_EQ(vrp.spread, 4); EXPECT_EQ(vrp.ratio, 1.25); EXPECT_TRUE(vrp.truncated);
  vrp = variance_risk_premium(iv, 0);
  EXPECT_EQ(vrp.spread, 20); EXPECT_TRUE(std::isnan(vrp.ratio));
  EXPECT_EQ(vrp.reason, "zero_realized_ratio_undefined");
  vrp = variance_risk_premium(iv, kNaN);
  EXPECT_TRUE(std::isnan(vrp.spread)); EXPECT_TRUE(std::isnan(vrp.ratio));
}

TEST(VolDerived, ConeImpliedMaturityUsesBusinessSessionCloses) {
  VolatilityMetrics v;
  for (const int days : {1, 30}) {
    ExpiryVolatility expiry;
    expiry.minutes = days * 1440; expiry.smile.atm = 20;
    v.expiries.push_back(expiry);
  }
  const auto as_of = md::new_york_to_utc({2026, 7, 2}, 16, 0);
  const auto implied = session_implied_vol(v, as_of, 2);
  EXPECT_EQ(implied.days, 5);  // Monday July 6 and Tuesday July 7, holiday/weekend skipped.
  EXPECT_NEAR(implied.vol, 20, 1e-12);
  EXPECT_TRUE(implied.proxy);
  EXPECT_TRUE(std::isnan(session_implied_vol(v, as_of, 252).vol));
}
}  // namespace
