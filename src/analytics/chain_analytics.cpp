#include "openport/analytics/chain_analytics.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <string_view>

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

/// One strike's signed positions, with common smile IV and fitted carry. Rates are
/// over the floored time `years`, and the spot-independent constants are cached for
/// the flip search's spot grid.
struct ExposureTerm {
  double strike;
  double vol;
  double years;
  double discount;
  double carry;
  double carry_rate;
  double yield;
  double calls;
  double puts;
  double sd;
  double root_years;
  double log_ratio;  ///< log(carry / strike); d1 adds log(spot)
  double scale;      ///< discount * carry
  double weight;

  ExposureTerm(double strike, double vol, double years, double discount, double carry,
               double calls, double puts)
      : strike(strike), vol(vol), years(years), discount(discount), carry(carry),
        // Using true time here would turn a small forward/spot gap near expiry into a
        // huge carry rate against the floored charm horizon.
        carry_rate(std::log(carry) / years), yield(-std::log(discount * carry) / years),
        calls(calls), puts(puts), sd(vol * std::sqrt(years)), root_years(std::sqrt(years)),
        log_ratio(std::log(carry / strike)), scale(discount * carry), weight(calls + puts) {}
};

struct ExposurePoint {
  double d1;
  double d2;
  double density;
};

ExposurePoint exposure_point(const ExposureTerm& t, double log_spot) {
  const double d1 = (log_spot + t.log_ratio) / t.sd + .5 * t.sd;
  return {d1, d1 - t.sd, pricing::norm_pdf(d1)};
}

double exposure_delta(const ExposureTerm& t, const ExposurePoint& p) {
  // Use the put CDF directly to retain small deltas in the tails.
  return t.scale * (t.calls * pricing::norm_cdf(p.d1) - t.puts * pricing::norm_cdf(-p.d1));
}

/// One metric, in kExposureFields order, without evaluating the others.
double exposure_metric(const ExposureTerm& t, double spot, double log_spot, std::size_t metric) {
  const ExposurePoint p = exposure_point(t, log_spot);
  const double gamma = t.weight * t.scale * p.density;
  switch (metric) {
    case 0: return gamma / t.sd * spot * .01;
    case 1: return -gamma * p.d2 / t.vol / 100 * spot;
    case 2: return exposure_delta(t, p) * spot;
    case 3:
      return (t.yield * exposure_delta(t, p) -
              gamma * (2 * t.carry_rate * t.years - p.d2 * t.sd) / (2 * t.years * t.sd)) /
             365.0 * spot;
    default: return gamma * t.root_years * p.d1 * p.d2 / t.vol / 10000 * spot;
  }
}

ExposureValues exposure_at(const ExposureTerm& t, double spot, double log_spot) {
  const ExposurePoint p = exposure_point(t, log_spot);
  const double gamma = t.weight * t.scale * p.density;
  const double delta = exposure_delta(t, p);
  const double charm = (t.yield * delta - gamma * (2 * t.carry_rate * t.years - p.d2 * t.sd) /
                                              (2 * t.years * t.sd)) / 365.0;
  return {gamma / t.sd * spot * .01,
          -gamma * p.d2 / t.vol / 100 * spot,
          delta * spot,
          charm * spot,
          gamma * t.root_years * p.d1 * p.d2 / t.vol / 10000 * spot};
}

double total_at(const std::vector<ExposureTerm>& terms, double spot, std::size_t metric) {
  const double log_spot = std::log(spot);
  double total = 0;
  for (const auto& term : terms) total += exposure_metric(term, spot, log_spot, metric);
  return total;
}

void compute_exposure(UnderlyingMetrics& out) {
  out.exposure = {};
  auto& summary = out.exposure;
  const auto& settings = out.exposure_settings;
  if (out.exposure_convention == ExposureConvention::CustomerFlow) {
    summary.available = false;
    summary.unavailable_reason = "Customer flow unavailable: the feed has no aggressor-side or signed volume data";
  } else if (!(out.spot > 0) || !std::isfinite(out.spot)) {
    summary.available = false;
    summary.unavailable_reason = "Exposure unavailable: no finite positive spot";
  }
  int exposure_options = 0, exposure_oi = 0;
  std::map<double, ExposureValues> by_strike;
  std::vector<ExposureTerm> terms;
  for (auto& slice : out.slices) {
    static_cast<ExposureValues&>(slice) = {};
    for (auto& row : slice.strikes) {
      static_cast<ExposureValues&>(row) = {};
      auto weight = [&](const OptionMetrics& option, double sign) {
        if (option.id == kNoInstrument || !std::isfinite(row.iv)) return 0.0;
        ++exposure_options;
        exposure_oi += option.has_open_interest;
        return option.has_open_interest && std::isfinite(option.open_interest) &&
                       option.open_interest >= 0
                   ? sign * option.open_interest * option.contract.multiplier : 0.0;
      };
      const double calls = weight(row.call,
          out.exposure_convention == ExposureConvention::ShortAll ? -1 : 1);
      const double puts = weight(row.put, -1);
      if (summary.available && row.iv > 0 && std::isfinite(row.iv) &&
          slice.years > 0 && slice.forward.forward > 0 && slice.forward.discount > 0 &&
          std::isfinite(slice.forward.forward) && std::isfinite(slice.forward.discount) &&
          (calls != 0 || puts != 0)) {
        const ExposureTerm term{row.strike, row.iv,
            std::max(slice.years, settings.min_days / 365.0), slice.forward.discount,
            slice.forward.forward / out.spot, calls, puts};
        static_cast<ExposureValues&>(row) = exposure_at(term, out.spot, std::log(out.spot));
        terms.push_back(term);
      }
      for (const auto field : kExposureFields) {
        if (!summary.available) row.*field = kNaN;
        slice.*field += row.*field;
        by_strike[row.strike].*field += row.*field;
      }
    }
    for (const auto field : kExposureFields) summary.*field += slice.*field;
  }
  if (exposure_options > 0)
    summary.oi_coverage = static_cast<double>(exposure_oi) / exposure_options;
  if (!summary.available) {
    for (const auto field : kExposureFields) summary.*field = kNaN;
    return;
  }
  // Share the spot grid across metrics: each strike's density/CDF is evaluated once.
  std::vector<std::pair<double, ExposureValues>> grid;
  ExposureValues largest;
  if (!terms.empty() && settings.flip_steps > 1) {
    for (int i = 0; i < settings.flip_steps; ++i) {
      const double x = -settings.flip_range + 2.0 * settings.flip_range * i /
                                                  (settings.flip_steps - 1);
      const double spot = out.spot * (1 + x);
      ExposureValues totals;
      if (spot > 0) {
        const double log_spot = std::log(spot);
        for (const auto& term : terms) {
          const auto values = exposure_at(term, spot, log_spot);
          for (const auto field : kExposureFields) totals.*field += values.*field;
        }
      } else {
        for (const auto field : kExposureFields) totals.*field = kNaN;
      }
      grid.emplace_back(spot, totals);
      for (const auto field : kExposureFields)
        if (std::isfinite(totals.*field))
          largest.*field = std::max(largest.*field, std::abs(totals.*field));
    }
  }
  for (std::size_t metric = 0; metric < kExposureFields.size(); ++metric) {
    const auto field = kExposureFields[metric];
    auto& levels = summary.levels[metric];
    double positive = 0, negative = 0;
    for (const auto& [strike, values] : by_strike) {
      if (values.*field > positive) {
        positive = values.*field;
        levels.positive_wall = strike;
      }
      if (values.*field < negative) {
        negative = values.*field;
        levels.negative_wall = strike;
      }
    }
    // Neither underflowed tails nor an identically zero profile prove a flip.
    const double threshold = (largest.*field) * 1e-10;
    struct Bracket {
      double lo, flo, hi, distance;
    };
    std::vector<Bracket> brackets;
    std::size_t previous = grid.size();
    for (std::size_t i = 0; i < grid.size(); ++i) {
      const auto& [spot, values] = grid[i];
      const double value = values.*field;
      if (!std::isfinite(value)) {
        previous = grid.size();
        continue;
      }
      if (!(std::abs(value) > threshold)) continue;
      const auto prior = previous;
      previous = i;
      if (prior == grid.size()) continue;
      const double lo = grid[prior].first;
      const double flo = grid[prior].second.*field;
      if (std::signbit(flo) == std::signbit(value)) continue;
      const double distance = lo <= out.spot && out.spot <= spot
          ? 0 : std::min(std::abs(lo - out.spot), std::abs(spot - out.spot));
      brackets.push_back({lo, flo, spot, distance});
    }
    // Reprice only the brackets that can still hold the crossing nearest spot.
    std::sort(brackets.begin(), brackets.end(),
              [](const auto& a, const auto& b) { return a.distance < b.distance; });
    double best_distance = std::numeric_limits<double>::infinity();
    for (auto [lo, flo, hi, distance] : brackets) {
      if (distance >= best_distance) break;
      bool finite = true;
      for (int iteration = 0; iteration < 60 && hi - lo > out.spot * 1e-10; ++iteration) {
        const double mid = .5 * (lo + hi);
        const double v = total_at(terms, mid, metric);
        if (!std::isfinite(v)) {
          finite = false;
          break;
        }
        if (v == 0) {
          lo = hi = mid;
          break;
        }
        if (std::signbit(v) == std::signbit(flo)) {
          lo = mid;
          flo = v;
        } else
          hi = mid;
      }
      if (!finite) continue;
      const double crossing = .5 * (lo + hi);
      if (std::abs(crossing - out.spot) < best_distance) {
        best_distance = std::abs(crossing - out.spot);
        levels.flip = crossing;
      }
    }
  }
  summary.gamma_flip = summary.levels[0].flip;
  summary.call_wall = summary.levels[0].positive_wall;
  summary.put_wall = summary.levels[0].negative_wall;
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
      auto include = [&](const OptionState* state, const OptionMetrics& m) {
        if (!state) return;
        ++sm.coverage.options;
        sm.coverage.quoted += state->has_quote;
        sm.coverage.open_interest += state->has_open_interest;
        sm.coverage.volume += std::isfinite(m.volume);
        sm.coverage.priced += std::isfinite(m.iv);
      };
      include(call, row.call);
      include(put, row.put);
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
    out.slices.push_back(std::move(sm));
  }

  out.options_priced = out.coverage.priced;
  out.exposure_settings = {options.exposure_min_days, options.flip_range, options.flip_steps};
  compute_exposure(out);

  out.compute_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  return out;
}

const char* to_string(ExposureConvention convention) {
  switch (convention) {
    case ExposureConvention::LongCallsShortPuts: return "long_calls_short_puts";
    case ExposureConvention::ShortAll: return "short_all";
    case ExposureConvention::CustomerFlow: return "customer_flow";
  }
  return "long_calls_short_puts";
}

const char* to_string(ExposureSlice slice) {
  switch (slice) {
    case ExposureSlice::All: return "all";
    case ExposureSlice::ZeroDte: return "0dte";
    case ExposureSlice::ExZeroDte: return "ex_0dte";
  }
  return "all";
}

std::optional<ExposureConvention> parse_exposure_convention(std::string_view name) {
  if (name == "long_calls_short_puts" || name == "open_interest")
    return ExposureConvention::LongCallsShortPuts;
  if (name == "short_all") return ExposureConvention::ShortAll;
  if (name == "customer_flow") return ExposureConvention::CustomerFlow;
  return std::nullopt;
}

UnderlyingMetrics exposure_view(const UnderlyingMetrics& metrics, ExposureConvention convention,
                                ExposureSlice slice) {
  // Copy only the selected expiries; the rest of a large chain is never touched.
  UnderlyingMetrics out;
  out.symbol = metrics.symbol;
  out.spot = metrics.spot;
  out.spot_source = metrics.spot_source;
  out.american_approximation = metrics.american_approximation;
  out.coverage = metrics.coverage;
  out.as_of = metrics.as_of;
  out.version = metrics.version;
  out.exposure_settings = metrics.exposure_settings;
  out.exposure_convention = convention;
  out.exposure_slice = slice;
  out.options_priced = metrics.options_priced;
  out.compute_ms = metrics.compute_ms;
  const auto date = md::new_york_time(metrics.as_of).date;
  for (const auto& expiry : metrics.slices) {
    const bool today = expiry.expiry == date;
    if (slice == ExposureSlice::All || (slice == ExposureSlice::ZeroDte) == today)
      out.slices.push_back(expiry);
  }
  compute_exposure(out);
  return out;
}

}  // namespace openport::analytics
