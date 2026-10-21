#include "openport/analytics/chain_analytics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

#include "openport/pricing/black.hpp"
#include "openport/pricing/implied_vol.hpp"
#include "openport/pricing/normal.hpp"

namespace openport::analytics {
std::shared_ptr<const DiscountCurve> DiscountCurve::from_points(
    std::string symbol, std::vector<std::pair<double, double>> points) {
  std::erase_if(points, [](auto p) {
    return !(p.first > 0) || !std::isfinite(p.first) || !std::isfinite(p.second);
  });
  std::sort(points.begin(), points.end());
  points.erase(
      std::unique(points.begin(), points.end(), [](auto a, auto b) { return a.first == b.first; }),
      points.end());
  if (points.size() < 2) return {};
  auto curve = std::shared_ptr<DiscountCurve>(new DiscountCurve);
  curve->symbol_ = std::move(symbol);
  curve->points_ = std::move(points);
  return curve;
}

double DiscountCurve::rate(double years) const {
  const auto upper = std::lower_bound(points_.begin(), points_.end(), years,
                                      [](auto p, double t) { return p.first < t; });
  if (upper == points_.begin()) return upper->second;
  if (upper == points_.end()) return points_.back().second;
  const auto lower = upper - 1;
  const double w = (years - lower->first) / (upper->first - lower->first);
  return lower->second + w * (upper->second - lower->second);
}

std::shared_ptr<const DiscountCurve> make_discount_curve(const UnderlyingMetrics& m) {
  std::vector<std::pair<double, double>> points;
  for (const auto& s : m.slices)
    if (s.style == pricing::ExerciseStyle::European && s.forward.ok && s.forward.fitted_discount)
      points.emplace_back(s.years, -std::log(s.forward.discount) / s.years);
  return DiscountCurve::from_points(m.symbol, std::move(points));
}

namespace {

using pricing::OptionType;

double implied(double price, OptionType type, double forward, double strike, double years,
               double discount) {
  if (!(price > 0.0)) return kNaN;
  const pricing::IvResult iv =
      pricing::implied_vol_black(price, type, forward, strike, years, discount);
  return iv.ok() ? iv.vol : kNaN;
}

/// Quotes and implied volatilities for one side; Greeks are filled in later, once
/// the strike's smile volatility is known.
OptionMetrics quote_metrics(const OptionState& s, md::InstrumentId id, double forward,
                            double discount, double years, double premium = kNaN) {
  OptionMetrics m;
  m.id = id;
  m.contract = s.contract;
  if (s.has_quote) {
    m.bid_size = s.bid_size;
    m.ask_size = s.ask_size;
  }
  m.eep = premium;
  const double adjustment = std::isfinite(premium) ? premium : 0;
  if (s.has_quote) {
    m.bid = s.bid;
    m.ask = s.ask;
    if (std::isfinite(s.bid) && std::isfinite(s.ask) && s.bid >= 0 && s.ask >= s.bid)
      m.mid = s.mid();
  }
  m.has_open_interest = s.has_open_interest;
  if (s.has_open_interest) m.open_interest = s.open_interest;
  if (s.vendor.iv > 0.0) m.vendor_iv = s.vendor.iv;
  const OptionType type = s.contract.type;
  const double strike = s.contract.strike;
  if (s.two_sided()) {
    m.mid = s.mid();
    m.iv = implied(m.mid - adjustment, type, forward, strike, years, discount);
  }
  if (s.bid > 0.0) m.bid_iv = implied(m.bid - adjustment, type, forward, strike, years, discount);
  if (s.ask > 0.0) m.ask_iv = implied(m.ask - adjustment, type, forward, strike, years, discount);
  return m;
}

struct GreeksContext {
  double forward;
  double discount;
  double years;
  double spot;
  double carry;  ///< dF/dS = F / S
};

void fill_greeks(OptionMetrics& m, OptionType type, double strike, double vol,
                 const GreeksContext& ctx) {
  if (!(vol > 0.0) || !std::isfinite(ctx.carry)) return;
  const pricing::Greeks g =
      pricing::black_greeks(type, ctx.forward, strike, ctx.years, vol, ctx.discount);
  m.delta = g.delta * ctx.carry;
  m.gamma = g.gamma * ctx.carry * ctx.carry;
  m.vega = g.vega / 100.0;
  m.theta = g.theta / 365.0;
  m.vanna = g.vanna * ctx.carry / 100.0;
}

/// Linear interpolation of the smile at the forward.
double atm_vol(const std::vector<StrikeMetrics>& strikes, double forward) {
  const StrikeMetrics* below = nullptr;
  const StrikeMetrics* above = nullptr;
  for (const StrikeMetrics& s : strikes) {
    if (!std::isfinite(s.iv)) continue;
    if (s.strike <= forward) below = &s;
    if (s.strike >= forward && above == nullptr) above = &s;
  }
  if (below && above) {
    if (above->strike == below->strike) return below->iv;
    const double w = (forward - below->strike) / (above->strike - below->strike);
    return below->iv + w * (above->iv - below->iv);
  }
  return below ? below->iv : (above ? above->iv : kNaN);
}

/// Dollar gamma per 1% move of one position term, with gamma at a hypothetical spot.
struct GammaTerm {
  double amplitude;  ///< weight * discount * forward_per_spot * .01 / (sd * sqrt(2pi))
  double center;     ///< log(strike / forward_per_spot) - sd^2/2
  double inverse_sd;
};

double total_gex_at(const std::vector<GammaTerm>& terms, double spot) {
  const double log_spot = std::log(spot);
  double total = 0.0;
  for (const GammaTerm& t : terms) {
    const double d1 = (log_spot - t.center) * t.inverse_sd;
    total += t.amplitude * std::exp(-0.5 * d1 * d1);
  }
  return total * spot;
}

}  // namespace

UnderlyingMetrics analyze(const UnderlyingBook& book, const ChainBook& chain, md::Timestamp as_of,
                          const AnalyticsOptions& options) {
  const auto started = std::chrono::steady_clock::now();
  UnderlyingMetrics out;
  out.symbol = book.symbol;
  out.as_of = as_of;
  out.version = book.version;
  const bool fresh_spot = md::years_between(book.spot_ts, book.data_time) <=
                          options.max_spot_age_minutes * 60 / (365.0 * 86400);
  if (fresh_spot && book.spot > 0.0 && std::isfinite(book.spot)) {
    out.spot = book.spot;
    out.spot_source = "quote";
  }

  // Pass 1: fit each live expiry's forward (and discount factor) from put-call parity.
  struct Fitted {
    const ExpirySlice* slice;
    pricing::ExerciseStyle style;
    double years;
    std::vector<ParityPoint> points;
    ForwardEstimate forward;
    std::string rate_source;
    std::map<md::InstrumentId, double> premiums;
    double dividend = kNaN;
    bool deamericanized = false;
    std::vector<Dividend> dividends;
    pricing::AmericanModel eep_model;
  };
  std::vector<Fitted> fitted;
  std::vector<double> long_rates;
  for (const auto& [key, slice] : book.expiries) {
    const double years = md::years_between(as_of, slice.expiry_time);
    if (!(years > 0.0)) continue;

    std::vector<ParityPoint> points;
    for (const auto& [strike, pair] : slice.strikes) {
      const OptionState* call = chain.option(pair.call);
      const OptionState* put = chain.option(pair.put);
      if (!call || !put || !call->two_sided() || !put->two_sided()) continue;
      const double floor = std::max(0.01, 0.001 * (call->mid() + put->mid()));
      const double spread = std::max(call->ask - call->bid + put->ask - put->bid, floor);
      points.push_back({strike, call->mid(), put->mid(), 1.0 / (spread * spread)});
    }
    double reference = out.spot;
    if (!(reference > 0.0) && !points.empty()) {
      // No spot from the feed: the strike where calls and puts cost the same is near the money.
      reference = std::min_element(points.begin(), points.end(), [](const auto& a, const auto& b) {
                    return std::abs(a.call_mid - a.put_mid) < std::abs(b.call_mid - b.put_mid);
                  })->strike;
    }
    std::sort(points.begin(), points.end(), [reference](const auto& a, const auto& b) {
      return std::abs(a.strike - reference) < std::abs(b.strike - reference);
    });
    if (points.size() > static_cast<std::size_t>(options.parity_strikes)) {
      points.resize(static_cast<std::size_t>(options.parity_strikes));
    }
    const bool american = key.second == pricing::ExerciseStyle::American;
    const double rate = american && options.discount_curve ? options.discount_curve->rate(years)
                                                           : options.fallback_rate;
    ForwardEstimate forward = american ? implied_forward_given_discount(
                                             points, std::exp(-rate * years), options.deamericanize)
                                       : implied_forward(points, years, options.fallback_rate);
    if (forward.ok && forward.fitted_discount && years * 365.0 >= options.min_days_for_rate) {
      long_rates.push_back(-std::log(forward.discount) / years);
    }
    fitted.push_back({&slice,
                      key.second,
                      years,
                      std::move(points),
                      forward,
                      american ? (options.discount_curve ? "curve" : "assumed") : "parity",
                      {},
                      kNaN,
                      false,
                      {},
                      options.american_model});
  }

  // Short expiries borrow the rate term structure's level from the long ones.
  double term_rate = options.fallback_rate;
  if (!long_rates.empty()) {
    std::nth_element(long_rates.begin(), long_rates.begin() + long_rates.size() / 2,
                     long_rates.end());
    term_rate = long_rates[long_rates.size() / 2];
  }

  // Finish rate adjustments before choosing one reference spot for all expiries.
  for (auto& f : fitted) {
    if (f.style == pricing::ExerciseStyle::European &&
        (f.years * 365.0 < options.min_days_for_rate || !f.forward.fitted_discount)) {
      f.forward = implied_forward_given_discount(f.points, std::exp(-term_rate * f.years));
      f.rate_source = long_rates.empty() ? "assumed" : "term";
    }
    if (!(out.spot > 0.0) && f.forward.ok) {
      out.spot = f.forward.forward * f.forward.discount;
      out.spot_source = "parity";
    }
  }
  // Short-tenor carry is dominated by small spot/option timing mismatches.
  // Borrow the median continuous yield from this underlying's longer expiries.
  std::vector<double> long_dividends;
  for (auto& f : fitted) {
    if (f.forward.ok && out.spot > 0) {
      f.dividend = -std::log(f.forward.discount * f.forward.forward / out.spot) / f.years;
      if (f.years * 365 >= options.min_days_for_rate && std::isfinite(f.dividend))
        long_dividends.push_back(f.dividend);
    }
  }
  double term_dividend = kNaN;
  if (!long_dividends.empty()) {
    const auto middle = long_dividends.begin() + long_dividends.size() / 2;
    std::nth_element(long_dividends.begin(), middle, long_dividends.end());
    term_dividend = *middle;
  }
  // One EEP correction at the first-pass Black IV and carry. Both valuations
  // share the LR lattice, including its terminal payoffs (not analytic BSM).
  // Measured convergence and the one-iteration residual are documented in
  // docs/american-analytics.md; Greeks remain European at the final smile IV.
  for (auto& f : fitted) {
    if (f.style != pricing::ExerciseStyle::American || !options.deamericanize || !(out.spot > 0))
      continue;
    const double discount = f.forward.discount;
    const double forward = f.forward.ok ? f.forward.forward : out.spot / discount;
    const double rate = -std::log(discount) / f.years;
    const double own_dividend = rate - std::log(forward / out.spot) / f.years;
    double dividend = std::clamp(
        f.years * 365 < options.min_days_for_rate && std::isfinite(term_dividend) ? term_dividend
                                                                                  : own_dividend,
        kMinTreeDividend, kMaxTreeDividend);
    std::vector<Dividend> used_dividends;
    std::vector<pricing::CashDividend> cash;
    double reserve = 0;
    for (const auto& d : options.dividends) {
      if (!md::valid_date(d.ex_date) || !(d.amount > 0) || !std::isfinite(d.amount)) continue;
      const auto ex_time = md::new_york_to_utc(d.ex_date, 0, 0);
      if (ex_time <= as_of || ex_time >= f.slice->expiry_time) continue;
      const double time = md::years_between(as_of, ex_time);
      used_dividends.push_back(d);
      cash.push_back({time, d.amount});
      reserve += d.amount * std::exp(-rate * time);
    }
    if (!cash.empty()) {
      // Residual carry acts only on the escrowed spot. Borrowing or clamping it
      // would break F0 = (S - PV(cash)) * exp((r - q) * T).
      const double escrow = out.spot - reserve;
      if (!(escrow > 0) || !std::isfinite(escrow)) continue;
      dividend = rate - std::log(forward / escrow) / f.years;
    }
    if (!std::isfinite(dividend)) continue;
    for (const auto& [strike, pair] : f.slice->strikes) {
      const auto* call = chain.option(pair.call);
      const auto* put = chain.option(pair.put);
      const double cv = call && call->two_sided() ? implied(call->mid(), OptionType::Call, forward,
                                                            strike, f.years, discount)
                                                  : kNaN;
      const double pv = put && put->two_sided() ? implied(put->mid(), OptionType::Put, forward,
                                                          strike, f.years, discount)
                                                : kNaN;
      auto correct = [&](const OptionState* state, md::InstrumentId id, double vol, double other) {
        if (!state || !state->two_sided()) return;
        if (!std::isfinite(vol)) vol = other;
        if (!(vol > 0) || !std::isfinite(vol)) return;
        auto used = options.american_model;
        const double premium = pricing::early_exercise_premium(
            {state->contract.type, out.spot, strike, f.years, rate, dividend, vol},
            options.american_model, kDeamericanizationSteps, cash, &used);
        if (used != options.american_model) f.eep_model = used;
        f.premiums[id] = premium;
      };
      correct(call, pair.call, cv, pv);
      correct(put, pair.put, pv, cv);
    }
    for (auto& p : f.points) {
      const auto& pair = f.slice->strikes.at(p.strike);
      if (const auto it = f.premiums.find(pair.call); it != f.premiums.end())
        p.call_mid -= it->second;
      if (const auto it = f.premiums.find(pair.put); it != f.premiums.end())
        p.put_mid -= it->second;
    }
    if (f.forward.ok) f.forward = implied_forward_given_discount(f.points, discount);
    f.deamericanized = true;
    if (!f.premiums.empty()) {
      std::sort(used_dividends.begin(), used_dividends.end(),
                [](const auto& a, const auto& b) { return a.ex_date < b.ex_date; });
      f.dividends = std::move(used_dividends);
    }
  }
  int exposure_options = 0, exposure_oi = 0;
  std::map<double, double> gex_by_strike;
  std::vector<GammaTerm> gamma_terms;

  // Pass 2: price every option against its expiry's forward.
  for (Fitted& f : fitted) {
    const ExpirySlice& slice = *f.slice;
    const double years = f.years;
    SliceMetrics sm;
    sm.expiry = slice.expiry;
    sm.expiry_time = slice.expiry_time;
    sm.root = slice.root;
    sm.style = f.style;
    sm.years = years;
    sm.forward = f.forward;
    sm.rate_source = f.rate_source;
    if (f.rate_source == "curve") sm.rate_curve_symbol = options.discount_curve->symbol();
    sm.deamericanized = f.deamericanized;
    sm.eep_model = f.eep_model;
    sm.dividends = std::move(f.dividends);
    if (!sm.forward.ok) {
      // Keep unanchored slices visible with missing analytics and receipt coverage.
      sm.forward.forward = out.spot / sm.forward.discount;
    }
    const double forward = sm.forward.forward;
    const double discount = sm.forward.discount;
    const double spot = out.spot;
    const GreeksContext ctx{forward, discount, years, spot, forward / spot};
    const double exposure_years = std::max(years, options.exposure_min_days / 365.0);
    const GreeksContext exposure_ctx{forward, discount, exposure_years, spot, forward / spot};

    sm.strikes.reserve(slice.strikes.size());
    for (const auto& [strike, pair] : slice.strikes) {
      StrikeMetrics row;
      row.strike = strike;
      const OptionState* call = chain.option(pair.call);
      const OptionState* put = chain.option(pair.put);
      auto quotes = [&](const OptionState* state, md::InstrumentId id) {
        if (!state) return OptionMetrics{};
        const auto it = f.premiums.find(id);
        const double premium = it == f.premiums.end() ? kNaN : it->second;
        auto metrics = quote_metrics(*state, id, forward, discount, years, premium);
        if (state->volume_ts > 0 && md::trading_date(state->volume_ts) == md::trading_date(as_of))
          metrics.volume = state->volume;
        return metrics;
      };
      row.call = quotes(call, pair.call);
      row.put = quotes(put, pair.put);

      const double otm = strike >= forward ? row.call.iv : row.put.iv;
      const double itm = strike >= forward ? row.put.iv : row.call.iv;
      row.iv = std::isfinite(otm) ? otm : itm;
      if (call) fill_greeks(row.call, OptionType::Call, strike, row.iv, ctx);
      if (put) fill_greeks(row.put, OptionType::Put, strike, row.iv, ctx);
      double net_oi = 0;
      auto include = [&](const OptionState* state, const OptionMetrics& m, double sign) {
        if (!state) return;
        ++sm.coverage.options;
        sm.coverage.quoted += state->has_quote;
        sm.coverage.open_interest += state->has_open_interest;
        sm.coverage.volume += std::isfinite(m.volume);
        sm.coverage.priced += std::isfinite(m.iv);
        if (!std::isfinite(row.iv)) return;
        ++exposure_options;
        exposure_oi += state->has_open_interest;
        if (state->has_open_interest && std::isfinite(m.open_interest) && m.open_interest >= 0)
          net_oi += sign * m.open_interest * state->contract.multiplier;
      };
      include(call, row.call, 1);
      include(put, row.put, -1);
      if (std::isfinite(row.iv) && net_oi != 0 && spot > 0) {
        // Both sides share the smile IV; received OI contributes even when
        // that side's own quote could not produce an IV.
        OptionMetrics exposure;
        fill_greeks(exposure, OptionType::Call, strike, row.iv, exposure_ctx);
        row.gex = exposure.gamma * net_oi * spot * spot * 0.01;
        row.vex = exposure.vanna * net_oi * spot;
        const double sd = row.iv * std::sqrt(exposure_years);
        const double carry = forward / spot;
        gamma_terms.push_back({net_oi * discount * carry * .01 * pricing::norm_pdf(0) / sd,
                               std::log(strike / carry) - .5 * sd * sd, 1 / sd});
      }
      sm.gex += row.gex;
      sm.vex += row.vex;
      gex_by_strike[strike] += row.gex;
      sm.strikes.push_back(std::move(row));
    }
    out.coverage.options += sm.coverage.options;
    out.coverage.quoted += sm.coverage.quoted;
    out.coverage.priced += sm.coverage.priced;
    out.coverage.open_interest += sm.coverage.open_interest;
    out.coverage.volume += sm.coverage.volume;
    if (sm.coverage.priced > 0 && sm.style == pricing::ExerciseStyle::American &&
        !sm.deamericanized)
      out.american_approximation = true;
    sm.atm_iv = atm_vol(sm.strikes, forward);
    out.exposure.gex += sm.gex;
    out.exposure.vex += sm.vex;
    out.slices.push_back(std::move(sm));
  }

  out.options_priced = out.coverage.priced;
  if (exposure_options > 0)
    out.exposure.oi_coverage = static_cast<double>(exposure_oi) / exposure_options;

  // Walls: the strikes carrying the most positive and most negative GEX.
  double best_call = 0.0;
  double best_put = 0.0;
  for (const auto& [strike, gex] : gex_by_strike) {
    if (gex > best_call) {
      best_call = gex;
      out.exposure.call_wall = strike;
    }
    if (gex < best_put) {
      best_put = gex;
      out.exposure.put_wall = strike;
    }
  }

  // Gamma flip: re-evaluate total GEX at hypothetical spots (sticky-strike vols)
  // and find the sign change nearest the current spot.
  if (out.spot > 0.0 && !gamma_terms.empty() && options.flip_steps > 1) {
    std::vector<std::pair<double, double>> grid;
    double largest = 0;
    for (int i = 0; i < options.flip_steps; ++i) {
      const double x =
          -options.flip_range + 2.0 * options.flip_range * i / (options.flip_steps - 1);
      const double s = out.spot * (1.0 + x);
      const double gex = s > 0 ? total_gex_at(gamma_terms, s) : kNaN;
      grid.emplace_back(s, gex);
      if (std::isfinite(gex)) largest = std::max(largest, std::abs(gex));
    }
    // Underflowed tails carry no evidence of a sign. Require both endpoints
    // to have meaningful magnitude relative to the entire search grid.
    const double threshold = largest * 1e-10;
    double best_distance = std::numeric_limits<double>::infinity();
    std::size_t previous = grid.size();
    for (std::size_t i = 0; i < grid.size(); ++i) {
      const auto [s, gex] = grid[i];
      if (!std::isfinite(gex)) {
        previous = grid.size();
        continue;
      }
      if (!(std::abs(gex) > threshold)) continue;
      const auto prior = previous;
      previous = i;
      if (prior == grid.size()) continue;
      // A root can lie exactly on a grid point; bracket it using the nearest
      // meaningful values on either side, never the tiny value itself.
      auto [lo, flo] = grid[prior];
      double hi = s;
      if (std::signbit(flo) == std::signbit(gex)) continue;
      bool finite = true;
      for (int iteration = 0; iteration < 60 && hi - lo > out.spot * 1e-10; ++iteration) {
        const double mid = .5 * (lo + hi);
        const double value = total_gex_at(gamma_terms, mid);
        if (!std::isfinite(value)) {
          finite = false;
          break;
        }
        if (value == 0) {
          lo = hi = mid;
          break;
        }
        if (std::signbit(value) == std::signbit(flo)) {
          lo = mid;
          flo = value;
        } else
          hi = mid;
      }
      if (!finite) continue;
      const double crossing = .5 * (lo + hi);
      if (std::abs(crossing - out.spot) < best_distance) {
        best_distance = std::abs(crossing - out.spot);
        out.exposure.gamma_flip = crossing;
      }
    }
  }

  out.compute_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return out;
}

}  // namespace openport::analytics
