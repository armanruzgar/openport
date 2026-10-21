#include "openport/analytics/volatility.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace openport::analytics {
namespace {
bool positive(double x) { return std::isfinite(x) && x > 0.0; }
double vol_of(double variance) {
  return std::isfinite(variance) && variance >= 0 ? 100 * std::sqrt(variance) : kNaN;
}
double variance_of(double vol) { return std::isfinite(vol) ? vol * vol / 10000 : kNaN; }

std::vector<std::pair<md::Date, double>> session_weights(md::Timestamp from, md::Timestamp to) {
  std::vector<std::pair<md::Date, double>> weights;
  const auto begin_day = md::days_since_epoch(md::new_york_time(from).date);
  const auto end_day = md::days_since_epoch(md::new_york_time(to).date);
  for (auto day = begin_day; day <= end_day; ++day) {
    const auto date = md::date_from_days(day);
    const auto open = md::new_york_to_utc(date, 9, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    if (!md::market_session(open).open || from >= close || to < open) continue;
    const auto overlap = std::min(close, to) - std::max(open, from);
    // An AM settlement assigns the overnight increment to that coming session.
    const double weight = overlap > 0 ? static_cast<double>(overlap) / static_cast<double>(close - open)
        : to == open && from < open ? 1.0 : 0.0;
    if (weight > 0) weights.emplace_back(date, weight);
  }
  return weights;
}

class Smile {
 public:
  Smile(const SliceMetrics& slice, const SviFit* fit, bool vendor) : years_(slice.years) {
    for (const auto& row : slice.strikes) {
      const double iv = vendor ? (row.strike >= slice.forward.forward ? row.call.vendor_iv : row.put.vendor_iv) : row.iv;
      if (positive(row.strike) && positive(slice.forward.forward) && positive(iv))
        points_.emplace_back(std::log(row.strike / slice.forward.forward), iv);
    }
    std::sort(points_.begin(), points_.end());
    points_.erase(std::unique(points_.begin(), points_.end(), [](const auto& a, const auto& b) {
      return a.first == b.first;
    }), points_.end());
    if (!vendor && fit && fit->status == SviStatus::Ok && fit->butterfly.ok &&
        svi_admissible(fit->parameters, years_) && fit->min_k <= 0 && fit->max_k >= 0)
      fit_ = fit;
  }
  double low() const { return fit_ ? fit_->min_k : points_.empty() ? kNaN : points_.front().first; }
  double high() const { return fit_ ? fit_->max_k : points_.empty() ? kNaN : points_.back().first; }
  const SviFit* fit() const { return fit_; }
  double iv(double k) const {
    if (!positive(years_) || !(k >= low() && k <= high())) return kNaN;
    if (fit_) return svi_iv(fit_->parameters, k, years_);
    const auto next = std::lower_bound(points_.begin(), points_.end(), k,
                                     [](const auto& p, double x) { return p.first < x; });
    if (next == points_.end()) return kNaN;
    if (next->first == k) return next->second;
    if (next == points_.begin()) return kNaN;
    const auto& prev = *std::prev(next);
    return std::lerp(prev.second, next->second, (k - prev.first) / (next->first - prev.first));
  }
  DeltaPoint delta(double target, bool call, double forward) const {
    DeltaPoint out;
    auto residual = [&](double k) {
      const double sigma = iv(k);
      const double st = sigma * std::sqrt(years_);
      if (!positive(st)) return kNaN;
      const double d1 = -k / st + st / 2;
      return 0.5 * std::erfc(-d1 / std::sqrt(2.0)) - (call ? 0.0 : 1.0) - target;
    };
    double left = low();
    double fl = residual(left);
    // A scan also handles non-monotone interpolated smiles without wing extension.
    for (int i = 1; i <= 512; ++i) {
      double right = std::lerp(low(), high(), static_cast<double>(i) / 512);
      const double fr = residual(right);
      if (std::isfinite(fl) && std::isfinite(fr) && fl * fr <= 0) {
        for (int step = 0; step < 70; ++step) {
          const double mid = std::midpoint(left, right);
          const double fm = residual(mid);
          if (fl * fm <= 0) right = mid;
          else { left = mid; fl = fm; }
        }
        const double k = std::midpoint(left, right);
        out.strike = forward * std::exp(k);
        out.vol = 100 * iv(k);
        return out;
      }
      left = right;
      fl = fr;
    }
    out.reason = "delta_outside_smile";
    return out;
  }
 private:
  double years_;
  const SviFit* fit_ = nullptr;
  std::vector<std::pair<double, double>> points_;
};

void set_skew(DeltaSkew& skew, double atm) {
  skew.rr = skew.call.vol - skew.put.vol;
  skew.bf = (skew.call.vol + skew.put.vol) / 2 - atm;
}
}  // namespace

Mfiv model_free_iv(const SliceMetrics& slice) {
  Mfiv out;
  const bool american = slice.style == pricing::ExerciseStyle::American;
  if (american) { out.eep = "unavailable"; out.proxy = true; }
  const double forward = slice.forward.forward;
  if (!positive(slice.years) || !positive(forward) || !positive(slice.forward.discount)) {
    out.reason = "invalid_forward_or_time";
    return out;
  }
  std::vector<const StrikeMetrics*> rows;
  for (const auto& row : slice.strikes) if (positive(row.strike)) rows.push_back(&row);
  std::sort(rows.begin(), rows.end(), [](auto a, auto b) { return a->strike < b->strike; });
  rows.erase(std::unique(rows.begin(), rows.end(), [](auto a, auto b) {
    return a->strike == b->strike;
  }), rows.end());
  const auto above = std::upper_bound(rows.begin(), rows.end(), forward,
                                      [](double f, auto row) { return f < row->strike; });
  if (above == rows.begin()) { out.reason = "no_strike_at_or_below_forward"; return out; }
  const auto center = static_cast<std::ptrdiff_t>(std::distance(rows.begin(), above) - 1);
  out.k0 = rows[static_cast<std::size_t>(center)]->strike;
  std::size_t corrected = 0, raw = 0;
  auto mid = [&](const OptionMetrics& option) {
    if (!positive(option.bid) || !positive(option.ask) || option.ask < option.bid) return kNaN;
    double value = std::midpoint(option.bid, option.ask);
    if (american && std::isfinite(option.eep) && option.eep >= 0) value -= option.eep;
    if (!std::isfinite(value) || value < 0) return kNaN;
    return value;
  };
  auto count_eep = [&](const OptionMetrics& option) {
    if (american) {
      if (std::isfinite(option.eep) && option.eep >= 0) ++corrected;
      else ++raw;
    }
  };
  const auto& k0 = *rows[static_cast<std::size_t>(center)];
  const double call = mid(k0.call), put = mid(k0.put);
  if (!std::isfinite(call) || !std::isfinite(put)) {
    out.reason = "missing_k0_quotes";
    return out;
  }
  std::vector<std::pair<double, double>> strip{{out.k0, (call + put) / 2}};
  count_eep(k0.call); count_eep(k0.put);
  auto wing = [&](int direction, std::string& stop) {
    int excluded = 0;
    for (auto i = center + direction; i >= 0 && i < static_cast<std::ptrdiff_t>(rows.size()); i += direction) {
      const auto& row = *rows[static_cast<std::size_t>(i)];
      const auto& option = direction < 0 ? row.put : row.call;
      const double q = mid(option);
      if (!std::isfinite(q)) {
        if (++excluded == 2) { stop = "zero_bids"; break; }
      } else {
        excluded = 0;
        strip.emplace_back(row.strike, q);
        count_eep(option);
      }
    }
  };
  wing(-1, out.lower_stop); wing(1, out.upper_stop);
  out.truncated = out.lower_stop == "window" || out.upper_stop == "window";
  if (american) {
    out.eep = corrected == 0 ? "unavailable" : raw == 0 ? "removed" : "partial";
    out.proxy = raw > 0;
  }
  std::sort(strip.begin(), strip.end());
  out.strikes = strip.size();
  out.low = strip.front().first;
  out.high = strip.back().first;
  if (strip.size() < 2) { out.reason = "too_few_strikes"; return out; }
  double sum = 0;
  for (std::size_t i = 0; i < strip.size(); ++i) {
    const double delta = i == 0 ? strip[1].first - strip[0].first
        : i + 1 == strip.size() ? strip[i].first - strip[i - 1].first
        : (strip[i + 1].first - strip[i - 1].first) / 2;
    sum += delta / (strip[i].first * strip[i].first) * strip[i].second;
  }
  const double adjustment = forward / out.k0 - 1;
  const double variance = (2 * sum / slice.forward.discount - adjustment * adjustment) / slice.years;
  if (!std::isfinite(variance) || variance < 0) out.reason = "negative_or_invalid_variance";
  else { out.variance = variance; out.vol = vol_of(variance); }
  return out;
}

SmileMetrics smile_metrics(const SliceMetrics& slice, const SviFit* fit, bool vendor) {
  const Smile smile(slice, fit, vendor);
  SmileMetrics out;
  out.source = vendor ? "vendor_iv_interpolation" : smile.fit() ? "svi" : "smile_interpolation";
  out.atm = 100 * smile.iv(0);
  if (!std::isfinite(out.atm)) out.reason = "forward_outside_smile";
  if (smile.fit() && positive(out.atm)) {
    const auto& p = smile.fit()->parameters;
    const double x = -p.m, h = std::hypot(x, p.sigma);
    const double w = svi_variance(p, 0), first = p.b * (p.rho + x / h);
    const double second = p.b * p.sigma * p.sigma / (h * h * h);
    out.slope = out.atm * first / (2 * w);
    out.curvature = out.atm * (second / (2 * w) - first * first / (4 * w * w));
  }
  for (const double delta : {0.25, 0.10}) {
    auto& skew = delta == 0.25 ? out.delta25 : out.delta10;
    skew.call = smile.delta(delta, true, slice.forward.forward);
    skew.put = smile.delta(-delta, false, slice.forward.forward);
    set_skew(skew, out.atm);
  }
  return out;
}

ConstantVol constant_vol(std::span<const VarianceKnot> knots, double days, double minimum_minutes) {
  ConstantVol out;
  out.days = days;
  const double target = days * 1440.0;
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < knots.size(); ++i)
    if (positive(knots[i].minutes) && knots[i].minutes >= minimum_minutes) order.push_back(i);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return knots[a].minutes < knots[b].minutes; });
  const auto next = std::lower_bound(order.begin(), order.end(), target,
                                    [&](auto i, double n) { return knots[i].minutes < n; });
  if (days <= 0 || next == order.end() || (next == order.begin() && knots[*next].minutes != target)) {
    out.reason = "target_not_bracketed";
    return out;
  }
  out.next = *next;
  out.near = knots[*next].minutes == target ? *next : *std::prev(next);
  const auto& a = knots[out.near];
  const auto& b = knots[out.next];
  out.truncated = a.truncated || b.truncated;
  out.proxy = a.proxy || b.proxy;
  if (!std::isfinite(a.variance) || !std::isfinite(b.variance) || a.variance < 0 || b.variance < 0) {
    out.reason = "unusable_bracketing_expiry";
    return out;
  }
  out.variance = out.near == out.next ? a.variance
      : std::lerp(a.minutes * a.variance, b.minutes * b.variance,
                  (target - a.minutes) / (b.minutes - a.minutes)) / target;
  out.vol = vol_of(out.variance);
  return out;
}

VolatilityMetrics volatility_metrics(const UnderlyingMetrics& metrics, std::span<const SviFit> fits,
                                     bool vendor) {
  VolatilityMetrics out;
  std::set<std::size_t> bad_calendar;
  for (const auto& violation : svi_calendar(fits)) {
    bad_calendar.insert(violation.earlier); bad_calendar.insert(violation.later);
  }
  std::vector<VarianceKnot> mfiv, atm, call25, put25, call10, put10;
  for (std::size_t i = 0; i < metrics.slices.size(); ++i) {
    const auto& slice = metrics.slices[i];
    const SviFit* fit = i < fits.size() && !bad_calendar.contains(i) ? &fits[i] : nullptr;
    ExpiryVolatility expiry;
    expiry.minutes = md::years_between(metrics.as_of, slice.expiry_time) * kMinutesPerYear;
    // Use settlement time even for hand-built snapshots whose cached years differ.
    auto timed = slice;
    timed.years = expiry.minutes / kMinutesPerYear;
    expiry.mfiv = model_free_iv(timed);
    expiry.smile = smile_metrics(timed, fit, vendor);
    mfiv.push_back({expiry.minutes, expiry.mfiv.variance, expiry.mfiv.truncated, expiry.mfiv.proxy});
    const bool proxy = expiry.smile.source != "svi";
    atm.push_back({expiry.minutes, variance_of(expiry.smile.atm), false, proxy});
    call25.push_back({expiry.minutes, variance_of(expiry.smile.delta25.call.vol), false, proxy});
    put25.push_back({expiry.minutes, variance_of(expiry.smile.delta25.put.vol), false, proxy});
    call10.push_back({expiry.minutes, variance_of(expiry.smile.delta10.call.vol), false, proxy});
    put10.push_back({expiry.minutes, variance_of(expiry.smile.delta10.put.vol), false, proxy});
    out.expiries.push_back(std::move(expiry));
  }
  for (int days : kMfivDays) out.mfiv.push_back(constant_vol(mfiv, days, 1440));
  for (int days : kAtmDays) out.atm.push_back(constant_vol(atm, days));
  const auto atm30 = constant_vol(atm, 30);
  const auto c25 = constant_vol(call25, 30), p25 = constant_vol(put25, 30);
  const auto c10 = constant_vol(call10, 30), p10 = constant_vol(put10, 30);
  out.skew25.call.vol = c25.vol; out.skew25.put.vol = p25.vol;
  out.skew10.call.vol = c10.vol; out.skew10.put.vol = p10.vol;
  out.skew25.call.reason = c25.reason; out.skew25.put.reason = p25.reason;
  out.skew10.call.reason = c10.reason; out.skew10.put.reason = p10.reason;
  set_skew(out.skew25, atm30.vol); set_skew(out.skew10, atm30.vol);
  out.skew_proxy = atm30.proxy || c25.proxy || p25.proxy || c10.proxy || p10.proxy;
  for (const auto& value : {atm30, c25, p25, c10, p10})
    if (!value.reason.empty()) out.skew_reason = value.reason;
  if (positive(out.mfiv[1].vol)) out.ratio9_30 = out.mfiv[0].vol / out.mfiv[1].vol;
  if (positive(out.mfiv[2].vol)) out.ratio30_93 = out.mfiv[1].vol / out.mfiv[2].vol;
  out.atm30_7 = out.atm[1].vol - out.atm[0].vol;
  return out;
}

VarianceRiskPremium variance_risk_premium(const ConstantVol& implied, double realized) {
  VarianceRiskPremium out;
  out.truncated = implied.truncated;
  out.proxy = implied.proxy;
  if (!std::isfinite(implied.vol) || !std::isfinite(realized) || realized < 0) {
    out.reason = "missing_implied_or_realized";
    return out;
  }
  out.spread = implied.vol - realized;
  if (realized > 0) out.ratio = implied.vol / realized;
  else out.reason = "zero_realized_ratio_undefined";
  return out;
}

ConstantVol session_implied_vol(const VolatilityMetrics& volatility, md::Timestamp as_of, int sessions) {
  if (sessions < 1 || sessions > 252) {
    ConstantVol out;
    out.reason = "invalid_session_horizon";
    return out;
  }
  std::vector<VarianceKnot> knots;
  for (const auto& expiry : volatility.expiries)
    knots.push_back({expiry.minutes, variance_of(expiry.smile.atm), false, expiry.smile.source != "svi"});
  auto day = md::days_since_epoch(md::new_york_time(as_of).date);
  md::Timestamp end = as_of;
  int remaining = sessions;
  for (int offset = 0; offset < 800 && remaining > 0; ++offset, ++day) {
    const auto date = md::date_from_days(day);
    const auto open = md::new_york_to_utc(date, 9, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    if (close > as_of && md::market_session(open).open) { --remaining; end = close; }
  }
  if (remaining > 0) {
    ConstantVol out;
    out.reason = "no_session_horizon";
    return out;
  }
  return constant_vol(knots, md::years_between(as_of, end) * 365);
}

std::vector<EventLabel> parse_events(std::istream& input) {
  std::vector<EventLabel> out;
  std::set<md::Date> seen;
  std::string line;
  auto trim = [](std::string text) {
    const auto first = text.find_first_not_of(" \t\r");
    return first == std::string::npos ? std::string{} : text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
  };
  for (int number = 1; std::getline(input, line); ++number) {
    line = trim(line);
    if (line.empty() || line.front() == '#' || line == "date,label" || line == "Date,Label") continue;
    auto fail = [&](const std::string& reason) { return std::invalid_argument("events line " + std::to_string(number) + ": " + reason); };
    const auto comma = line.find(',');
    if (comma == std::string::npos) throw fail("expected YYYY-MM-DD,Label");
    const auto date = trim(line.substr(0, comma));
    const auto label = trim(line.substr(comma + 1));
    const auto parsed = md::parse_datetime(date + " 12:00:00", md::Zone::NewYork);
    if (date.size() != 10 || !parsed) throw fail("date must be YYYY-MM-DD within the timestamp range");
    if (label.empty() || label.size() > 160 ||
        std::any_of(label.begin(), label.end(), [](unsigned char c) { return c < 32 || c == 127; }))
      throw fail("label must contain 1 to 160 bytes without control characters");
    const auto day = md::new_york_time(*parsed).date;
    if (!seen.insert(day).second) throw fail("date listed twice: " + date);
    out.push_back({day, label});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.date < b.date; });
  return out;
}

namespace {
md::Timestamp event_boundary(const EventLabel& event) {
  if (event.timing == "exact") return event.time;
  if (event.timing == "after_close") return md::new_york_to_utc(event.date, md::regular_close_hour(event.date), 0) + 1;
  return md::new_york_to_utc(event.date, 9, 30);
}
double business_weight(md::Timestamp from, md::Timestamp to) {
  double result = 0;
  for (const auto& [date, weight] : session_weights(from, to)) { (void)date; result += weight; }
  return result;
}
}
md::Date event_session(const EventLabel& event) {
  auto date = event.timing == "exact" ? md::new_york_time(event.time).date : event.date;
  if (event.timing == "after_close" || (event.timing == "exact" && event.time >= md::new_york_to_utc(date, md::regular_close_hour(date), 0)))
    date = md::date_from_days(md::days_since_epoch(date) + 1);
  while (!md::market_session(md::new_york_to_utc(date, 9, 30)).open)
    date = md::date_from_days(md::days_since_epoch(date) + 1);
  return date;
}
std::vector<EventVolatility> event_volatility(const UnderlyingMetrics& metrics,
    const VolatilityMetrics& volatility, std::span<const EventLabel> events) {
  std::vector<EventVolatility> out;
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < metrics.slices.size() && i < volatility.expiries.size(); ++i)
    if (metrics.slices[i].expiry_time > metrics.as_of) order.push_back(i);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return metrics.slices[a].expiry_time < metrics.slices[b].expiry_time; });
  for (const auto& event : events) {
    const auto boundary = event_boundary(event);
    if (boundary <= metrics.as_of) continue;
    EventVolatility result;
    result.event = event; result.session = event_session(event);
    const auto next = std::lower_bound(order.begin(), order.end(), boundary,
        [&](auto i, auto time) { return metrics.slices[i].expiry_time < time; });
    if (next == order.begin() || next == order.end()) {
      result.reason = "event_not_bracketed"; out.push_back(result); continue;
    }
    result.bracketed = true; result.near = *std::prev(next); result.next = *next;
    const auto& near = metrics.slices[result.near];
    const auto& far = metrics.slices[result.next];
    const auto& a = volatility.expiries[result.near];
    const auto& b = volatility.expiries[result.next];
    // Use one variance basis on both sides; mixed MFIV/ATM levels cannot isolate a jump.
    const bool fallback = !std::isfinite(a.mfiv.variance) || !std::isfinite(b.mfiv.variance);
    const double wa = (fallback ? variance_of(a.smile.atm) : a.mfiv.variance) * md::years_between(metrics.as_of, near.expiry_time);
    const double wb = (fallback ? variance_of(b.smile.atm) : b.mfiv.variance) * md::years_between(metrics.as_of, far.expiry_time);
    result.proxy = fallback || a.mfiv.proxy || b.mfiv.proxy;
    result.truncated = !fallback && (a.mfiv.truncated || b.mfiv.truncated);
    result.forward = far.forward.forward;
    const auto na = business_weight(metrics.as_of, near.expiry_time);
    const auto nb = na + business_weight(near.expiry_time, far.expiry_time);
    const auto competing = std::count_if(events.begin(), events.end(), [&](const auto& other) {
      const auto time = event_boundary(other);
      return time > metrics.as_of && time <= far.expiry_time;
    });
    if (competing > 1) result.reason = "multiple_events_in_solve";
    else if (!std::isfinite(wa) || !std::isfinite(wb) || wa < 0 || wb < 0) result.reason = "unusable_variance";
    else if (na <= 0 || nb <= na) result.reason = "no_baseline_sessions";
    else {
      result.baseline_variance = wa / na;
      result.variance_premium = wb - result.baseline_variance * nb;
      result.calendar_arbitrage = wb < wa - 1e-12;
      if (result.calendar_arbitrage) result.reason = "negative_forward_variance";
      else if (result.variance_premium < -1e-12) result.reason = "negative_event_premium";
      else {
        result.variance_premium = std::max(0.0, result.variance_premium);
        result.percent = vol_of(result.variance_premium);
        result.points = positive(result.forward) ? result.forward * result.percent / 100 : kNaN;
      }
    }
    out.push_back(std::move(result));
  }
  return out;
}

ImpliedMoves implied_moves(const UnderlyingMetrics& metrics, const VolatilityMetrics& volatility,
                           std::span<const EventLabel> events) {
  ImpliedMoves out;
  const auto today = md::new_york_time(metrics.as_of).date;
  const auto first_day = md::days_since_epoch(today);
  for (int offset = 0; offset < 60 && out.sessions.size() < 15; ++offset) {
    const auto date = md::date_from_days(first_day + offset);
    const auto open = md::new_york_to_utc(date, 9, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    if (!md::market_session(open).open || close <= metrics.as_of) continue;
    ImpliedMove move;
    move.date = date;
    move.reason = "no_covering_expiry";
    for (const auto& event : events) if (event_session(event) == date)
      move.label += (move.label.empty() ? "" : "; ") + event.label;
    out.sessions.push_back(std::move(move));
  }
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < metrics.slices.size() && i < volatility.expiries.size(); ++i)
    if (metrics.slices[i].expiry_time > metrics.as_of) order.push_back(i);
  std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) {
    return metrics.slices[a].expiry_time < metrics.slices[b].expiry_time;
  });
  md::Timestamp previous_time = metrics.as_of;
  double previous_variance = 0;
  bool previous_proxy = false, previous_truncated = false;
  std::map<md::Date, double> assigned;
  for (const auto i : order) {
    const auto& slice = metrics.slices[i];
    const auto& expiry = volatility.expiries[i];
    const bool fallback = !std::isfinite(expiry.mfiv.variance);
    const double variance = fallback ? variance_of(expiry.smile.atm) : expiry.mfiv.variance;
    const double total = variance * expiry.minutes / kMinutesPerYear;
    const double increment = total - previous_variance;
    const bool negative = std::isfinite(increment) && increment < -1e-12;
    const bool proxy = fallback || expiry.mfiv.proxy || previous_proxy;
    const bool truncated = (!fallback && expiry.mfiv.truncated) || previous_truncated;
    const auto weights = session_weights(previous_time, slice.expiry_time);
    out.intervals.push_back({previous_time, slice.expiry_time, increment, weights.size(), negative, proxy, truncated,
        negative ? "negative_forward_variance" : !std::isfinite(increment) ? "unusable_variance"
        : weights.empty() ? "no_business_sessions" : ""});
    double sum = 0;
    for (const auto& weight : weights) sum += weight.second;
    for (auto& move : out.sessions) {
      const auto weight = std::find_if(weights.begin(), weights.end(), [&](const auto& w) { return w.first == move.date; });
      if (weight == weights.end()) continue;
      move.shared = move.shared || weights.size() > 1;
      move.proxy = move.proxy || proxy;
      move.truncated = move.truncated || truncated;
      move.calendar_arbitrage = move.calendar_arbitrage || negative;
      move.forward = slice.forward.forward;
      if (negative || !std::isfinite(increment) || move.calendar_arbitrage || move.reason == "unusable_variance") {
        move.reason = move.calendar_arbitrage ? "negative_forward_variance" : "unusable_variance";
        move.percent = move.points = kNaN;
      } else {
        assigned[move.date] += std::max(0.0, increment) * weight->second / sum;
        move.percent = 100 * std::sqrt(assigned[move.date]);
        move.points = positive(move.forward) ? move.forward * move.percent / 100 : kNaN;
        move.reason.clear();
      }
    }
    if (slice.expiry == today) {
      out.today_proxy = proxy;
      out.today_truncated = truncated;
      out.today_calendar_arbitrage = out.today_calendar_arbitrage || negative;
      out.today_percent = vol_of(total);
      out.today_points = positive(slice.forward.forward) ? slice.forward.forward * out.today_percent / 100 : kNaN;
      out.today_reason = std::isfinite(out.today_percent) ? "" : "unusable_variance";
    }
    previous_time = slice.expiry_time;
    previous_variance = total;
    previous_proxy = fallback || expiry.mfiv.proxy;
    previous_truncated = !fallback && expiry.mfiv.truncated;
  }
  return out;
}

}  // namespace openport::analytics
