// Reproducible timing/convergence probe; see docs/american-analytics.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "../tests/support/american_chain.hpp"
#include "openport/analytics/chain_analytics.hpp"
#include "openport/pricing/american.hpp"
#include "openport/pricing/implied_vol.hpp"

using namespace openport;

void accuracy() {
  for (int n : {15, 21, 31, 51, 71, 101}) {
    double largest = 0, otm = 0;
    for (double t : {.25, 1.0})
      for (double k = 80; k <= 120; k += 2.5)
        for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
          const pricing::BsmInputs in{type, 100, k, t, .045, .01, .2};
          const double error = std::abs(pricing::binomial_early_exercise_premium(in, n) -
                                        pricing::binomial_early_exercise_premium(in, 2001));
          largest = std::max(largest, error);
          if ((type == pricing::OptionType::Call) == (k >= 100 * std::exp(.035 * t)))
            otm = std::max(otm, error);
        }
    std::printf("steps=%d max_EEP_error=%.8f max_OTM_EEP_error=%.8f\n", n, largest, otm);
  }
  for (auto model : {pricing::AmericanModel::LeisenReimer, pricing::AmericanModel::BjerksundStensland2002,
                     pricing::AmericanModel::BaroneAdesiWhaley}) {
    for (double t : {.25, 1.0}) {
      analytics::ChainBook book;
      md::InstrumentId id = 0;
      const md::Date expiry{2027, 9, 22};
      const auto as_of =
          md::new_york_to_utc(expiry, 16, 0) - static_cast<md::Timestamp>(t * md::kNanosPerYear);
      test::add_american_expiry(book, as_of, expiry, id);
      analytics::AnalyticsOptions options;
      options.discount_curve = analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
      options.american_model = model;
      for (bool corrected : {false, true}) {
        if (!corrected && model != pricing::AmericanModel::LeisenReimer) continue;
        options.deamericanize = corrected;
        const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
        const auto& s = m.slices[0];
        double vol_error = 0;
        for (const auto& row : s.strikes) vol_error = std::max(vol_error, std::abs(row.iv - .2));
        std::printf("model=%s T=%.2f corrected=%d forward_error_pct=%.6f max_OTM_IV_error_vp=%.6f\n",
                    std::string(pricing::to_string(model)).c_str(), t, corrected,
                    100 * (s.forward.forward / (100 * std::exp(.035 * t)) - 1), 100 * vol_error);
      }
    }
  }
}

/// Each model's EEP against a converged 2001-step LR EEP over a wide grid, in
/// dollars per $100 spot, and end to end: the BSM IV solved from the converged
/// American price minus the model's EEP, against the vol that generated it.
void models() {
  struct Worst {
    double dollars = 0, otm_dollars = 0, vol_points = 0, otm_vol_points = 0;
    int count = 0, failed = 0;
  };
  for (auto model : {pricing::AmericanModel::LeisenReimer, pricing::AmericanModel::BjerksundStensland2002,
                     pricing::AmericanModel::BaroneAdesiWhaley}) {
    Worst worst;
    double seconds = 0;
    for (double t : {7 / 365.0, 30 / 365.0, .25, .5, 1.0, 2.0})
      for (double vol : {.1, .2, .4})
        for (double r : {.01, .045})
          for (double q : {0.0, .01, .03})
            for (double k = 70; k <= 130; k += 2.5)
              for (auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
                const pricing::BsmInputs in{type, 100, k, t, r, q, vol};
                const double reference = pricing::binomial_early_exercise_premium(in, 2001);
                const double american = pricing::bsm_price(in) + reference;
                const auto start = std::chrono::steady_clock::now();
                const double premium = pricing::early_exercise_premium(in, model, analytics::kDeamericanizationSteps);
                seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                const double error = std::abs(premium - reference);
                const double forward = 100 * std::exp((r - q) * t);
                const bool otm = (type == pricing::OptionType::Call) == (k >= forward);
                ++worst.count;
                worst.dollars = std::max(worst.dollars, error);
                if (otm) worst.otm_dollars = std::max(worst.otm_dollars, error);
                const auto iv = pricing::implied_vol_bsm(american - premium, type, 100, k, t, r, q);
                if (!iv.ok()) {
                  // Deep ITM time value can be below the tick; analytics uses OTM IVs.
                  worst.failed += otm;
                  continue;
                }
                const double vol_points = 100 * std::abs(iv.vol - vol);
                worst.vol_points = std::max(worst.vol_points, vol_points);
                if (otm) worst.otm_vol_points = std::max(worst.otm_vol_points, vol_points);
              }
    std::printf("model=%s contracts=%d max_EEP_error=%.6f max_OTM_EEP_error=%.6f max_IV_error_vp=%.4f "
                "max_OTM_IV_error_vp=%.4f otm_iv_failures=%d mean_us=%.3f\n",
                std::string(pricing::to_string(model)).c_str(), worst.count, worst.dollars, worst.otm_dollars,
                worst.vol_points, worst.otm_vol_points, worst.failed, 1e6 * seconds / worst.count);
  }
}

int main(int argc, char** argv) {
  bool cash_dividends = false, deamericanize = true;
  auto model = pricing::AmericanModel::LeisenReimer;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--accuracy") { accuracy(); return 0; }
    if (arg == "--models") { models(); return 0; }
    if (arg == "--cash-dividends") cash_dividends = true;
    else if (arg == "--no-deamericanize") deamericanize = false;
    else if (arg == "--model" && i + 1 < argc && pricing::parse_american_model(argv[i + 1]))
      model = *pricing::parse_american_model(argv[++i]);
    else {
      std::fprintf(stderr, "usage: %s [--accuracy | --models | [--model lr|bs2002|baw] "
                           "[--cash-dividends] [--no-deamericanize]]\n", argv[0]);
      return 2;
    }
  }
  analytics::ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  analytics::AnalyticsOptions options;
  std::vector<pricing::CashDividend> cash;
  if (cash_dividends) {
    // Synthetic quarterly amounts for timing only, not a fund's dividend forecast.
    for (int days = 60; days < 901; days += 90) {
      const auto date = md::date_from_days(md::days_since_epoch({2026, 9, 22}) + days);
      options.dividends.push_back({date, 1.5});
      cash.push_back({md::years_between(as_of, md::new_york_to_utc(date, 0, 0)), 1.5});
    }
  }
  md::InstrumentId id = 0;
  for (int i = 0; i < 31; ++i) {
    const auto date = md::date_from_days(md::days_since_epoch({2026, 9, 22}) + 1 + i * i);
    test::add_american_expiry(book, as_of, date, id, 600, 120, 5, 194, 101, "SPY", cash);
  }
  options.discount_curve = analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
  options.deamericanize = deamericanize;
  options.american_model = model;
  std::vector<double> timings;
  int priced = 0;
  for (int i = 0; i < 55; ++i) {
    const auto m = analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
    priced = m.options_priced;
    if (i >= 5) timings.push_back(m.compute_ms);
  }
  std::sort(timings.begin(), timings.end());
  std::printf("options=%u expiries=31 priced=%d corrected=%d model=%s cash=%zu steps=%d median_ms=%.3f p90_ms=%.3f\n",
              id, priced, options.deamericanize, std::string(pricing::to_string(model)).c_str(), cash.size(),
              analytics::kDeamericanizationSteps, timings[25], timings[45]);
}
