#include "openport/server/equity.hpp"

#include <algorithm>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace openport::server {
namespace {
using trading::Money;
std::string amount(const std::optional<Money>& value) { return value ? std::to_string(value->micros()) : "null"; }
std::string line(const EquitySample& s) {
  std::ostringstream risk;
  if (s.risk) {
    const auto& r = *s.risk;
    risk << std::setprecision(std::numeric_limits<double>::max_digits10) << "," << r.exposure.dollar_delta
         << "," << r.exposure.dollar_gamma_1pct << "," << r.exposure.vega << "," << r.exposure.theta
         << "," << r.delta_utilisation << "," << r.vega_utilisation;
    for (const auto& cap : {r.gamma_utilisation, r.theta_utilisation, r.grid_utilisation, r.short_vega_utilisation}) {
      risk << ",";
      if (cap) risk << *cap; else risk << "null";
    }
  }
  return std::to_string(s.time) + "," + std::to_string(s.attempt) + "," + std::to_string(s.equity.micros()) + "," +
      amount(s.floor) + "," + std::to_string(s.peak.micros()) + "," + amount(s.target) + "," +
      amount(s.tomorrow_floor) + "," + std::to_string(s.fill) + "," + std::to_string(s.stock_fill) + risk.str() + "\n";
}
EquitySample parse(std::string text) {
  std::replace(text.begin(), text.end(), ',', ' ');
  std::istringstream stream(text);
  EquitySample sample;
  std::int64_t equity = 0, peak = 0;
  std::string floor, target, tomorrow, extra;
  if (!(stream >> sample.time >> sample.attempt >> equity >> floor >> peak >> target >> tomorrow >> sample.fill) ||
      sample.time <= 0 || sample.attempt == 0) throw std::runtime_error("invalid equity sample");
  // The original eight-column format predates share-change samples.
  stream >> std::ws;
  if (!stream.eof() && !(stream >> sample.stock_fill)) throw std::runtime_error("invalid equity sample");
  stream >> std::ws;
  if (!stream.eof()) {
    EquityRisk risk;
    if (!(stream >> risk.exposure.dollar_delta >> risk.exposure.dollar_gamma_1pct >> risk.exposure.vega >> risk.exposure.theta
          >> risk.delta_utilisation >> risk.vega_utilisation))
      throw std::runtime_error("invalid equity risk sample");
    for (double value : {risk.exposure.dollar_delta, risk.exposure.dollar_gamma_1pct, risk.exposure.vega,
                        risk.exposure.theta, risk.delta_utilisation, risk.vega_utilisation})
      if (!std::isfinite(value)) throw std::runtime_error("nonfinite equity risk sample");
    stream >> std::ws;
    if (!stream.eof()) {
      for (auto* value : {&risk.gamma_utilisation, &risk.theta_utilisation, &risk.grid_utilisation, &risk.short_vega_utilisation}) {
        std::string field;
        if (!(stream >> field)) throw std::runtime_error("invalid equity utilisation");
        if (field == "null") continue;
        std::size_t used = 0;
        *value = std::stod(field, &used);
        if (used != field.size() || !std::isfinite(**value) || **value < 0) throw std::runtime_error("invalid equity utilisation");
      }
      if (stream >> extra) throw std::runtime_error("invalid equity risk sample");
    }
    sample.risk = risk;
  }
  sample.equity = Money::from_micros(equity);
  sample.peak = Money::from_micros(peak);
  const auto money = [](const std::string& value) -> std::optional<Money> {
    if (value == "null") return {};
    std::size_t used = 0;
    const auto number = std::stoll(value, &used);
    if (used != value.size()) throw std::runtime_error("invalid equity amount");
    return Money::from_micros(number);
  };
  sample.floor = money(floor); sample.target = money(target); sample.tomorrow_floor = money(tomorrow);
  return sample;
}
}
std::vector<EquitySample> fill_equity_samples(const trading::TradingSession& session, const trading::TradingSnapshot& before) {
  const auto after = session.snapshot();
  if (after->journal_failed || after->evaluation.attempt != before.evaluation.attempt) return {};
  const auto& rules = session.config().rules;
  const auto sample = [&](md::Timestamp time, const trading::Evaluation& e, Money value) {
    EquitySample result;
    result.time = time; result.attempt = e.attempt; result.equity = value; result.peak = e.peak;
    if (rules.max_drawdown > Money{}) {
      result.floor = e.floor;
      if (rules.drawdown_mode == trading::DrawdownMode::EndOfDay) result.tomorrow_floor = trading::evaluation_tomorrow_floor(e, rules, value);
    }
    if (rules.profit_target > Money{}) result.target = e.starting_balance + rules.profit_target;
    return result;
  };
  std::vector<EquitySample> decision;
  if (before.evaluation.status == trading::EvaluationStatus::Active && after->evaluation.status != trading::EvaluationStatus::Active) {
    const auto& e = after->evaluation;
    decision.push_back(sample(e.decided_at, e, e.decided_equity));
    decision.back().decision = true;
  }
  const auto first = before.recent_fills.size();
  if (!after->valuation_complete || (first >= after->recent_fills.size() && before.stock_fills.size() >= after->stock_fills.size()))
    return decision;
  std::vector<Money> changes;
  Money total;
  for (auto i = first; i < after->recent_fills.size(); ++i) {
    const auto& fill = after->recent_fills[i];
    const auto quote = session.quote(fill.symbol);
    if (!quote || !trading::markable_quote(*quote)) return decision;
    const auto mark = quote->bid ? *quote->bid + (*quote->ask - *quote->bid).prorate(1, 2) : quote->ask->prorate(1, 2);
    const auto change = ((mark - fill.price) * 100) * (fill.side == trading::Side::Buy ? fill.quantity : -fill.quantity) - fill.fee;
    changes.push_back(change);
    total = total + change;
  }
  auto equity = after->equity - total;
  // The rule state the fills met, observed as the reducer does. An end-of-day peak and
  // floor move only at rollover, which comes before the fills of its own transaction;
  // an intraday peak follows the marks from the state before.
  const auto& ratcheted = rules.drawdown_mode == trading::DrawdownMode::EndOfDay ? after->evaluation : before.evaluation;
  trading::Evaluation state;
  state.attempt = after->evaluation.attempt;
  state.starting_balance = after->evaluation.starting_balance;
  state.peak = ratcheted.peak;
  state.floor = ratcheted.floor;
  state.floor_locked = ratcheted.floor_locked;
  state.status = before.evaluation.status;
  // Objectives, or a target on the closed balance, can hold a pass back: only the
  // reducer's own pass stops the ratchet.
  const auto observe = [&] {
    const auto outcome = trading::evaluate_equity(state, rules, equity);
    if (outcome != trading::EvaluationStatus::Passed || after->evaluation.status == trading::EvaluationStatus::Passed)
      state.status = outcome;
  };
  observe();
  std::vector<EquitySample> result;
  // A quote can mark a new high, or decide the attempt, and execute in the same
  // transaction: keep that mark before subtracting the committed fills' spread and fees.
  if (first < after->recent_fills.size()) result.push_back(sample(after->recent_fills[first].time, state, equity));
  for (auto i = first; i < after->recent_fills.size();) {
    auto end = i + 1;
    const auto& fill = after->recent_fills[i];
    const auto& order = after->recent_orders.at(static_cast<std::size_t>(fill.order_id - 1));
    if (trading::multi_leg(order.request)) end = std::min(after->recent_fills.size(), i + order.request.legs.size());
    for (auto j = i; j < end; ++j) equity = equity + changes[j - first];
    if (order.system && !decision.empty()) state = after->evaluation;
    observe();
    for (; i < end; ++i) {
      auto leg = sample(after->recent_fills[i].time, state, equity);
      leg.fill = after->recent_fills[i].id;
      result.push_back(leg);
    }
  }
  // Deliveries can also remove an option at intrinsic value; use the committed
  // transaction mark for these atomic share changes, including manual closes.
  for (auto i = before.stock_fills.size(); i < after->stock_fills.size(); ++i) {
    const auto& fill = after->stock_fills[i];
    auto change = sample(fill.time, after->evaluation, after->equity);
    change.stock_fill = fill.id;
    result.push_back(change);
  }
  if (!decision.empty()) {
    const auto& point = decision.front();
    const auto same = std::find_if(result.begin(), result.end(), [&](const EquitySample& s) {
      return s.time == point.time && s.equity == point.equity;
    });
    if (same != result.end() && same->fill == 0 && same->stock_fill == 0) *same = point;
    else {
      // A decision can follow an entry fill, precede liquidation, or involve shares
      // only. Use the reducer's exact observation, even when interpolation cannot.
      const auto liquidation = std::find_if(result.begin(), result.end(), [&](const EquitySample& s) {
        if (s.fill != 0) return after->recent_orders.at(after->recent_fills.at(s.fill - 1).order_id - 1).system;
        if (s.stock_fill != 0) return after->stock_fills.at(s.stock_fill - 1).source == trading::StockSource::Rule;
        return false;
      });
      result.insert(same != result.end() ? same : liquidation, point);
    }
  }
  return result;
}
std::vector<EquitySample> read_equity_history(const std::filesystem::path& file, std::string& error) {
  std::vector<EquitySample> samples;
  try {
    if (!std::filesystem::exists(file)) return samples;
    std::ifstream input(file);
    if (!input) throw std::runtime_error("cannot read equity history");
    std::string text;
    while (std::getline(input, text)) {
      // A torn trailing row is ignored; earlier complete rows stay usable.
      try {
        auto sample = parse(text);
        if (samples.empty() || sample.time >= samples.back().time) samples.push_back(std::move(sample));
      } catch (const std::exception&) { error = "equity: ignored an invalid row"; }
    }
    if (input.bad()) throw std::runtime_error("cannot read equity history");
  } catch (const std::filesystem::filesystem_error& e) { error = "equity: " + e.code().message(); }
  catch (const std::exception& e) { error = std::string("equity: ") + e.what(); }
  return samples;
}
EquityStore::EquityStore(std::filesystem::path file, bool read_only) : file_(std::move(file)), read_only_(read_only) {
  samples_ = read_equity_history(file_, error_);
  if (!error_.empty()) fail(error_);
  if (read_only) return;
  try {
    if (!samples_.empty()) { compact(); if (!error_.empty()) error_recovered_ = true; }
  }
  catch (const std::exception& e) { fail(e.what()); }
}
void EquityStore::fail(std::string message, md::Timestamp market_time) {
  error_ = message.starts_with("equity: ") ? std::move(message) : "equity: " + message;
  error_time_ = md::now();
  error_market_time_ = market_time;
  error_recovered_ = false;
}
void EquityStore::compact() {
  if (samples_.empty()) return;
  const auto newest = samples_.back();
  const auto old = newest.time - 90 * md::kNanosPerDay;
  std::size_t earlier = 0;
  for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) if (it->attempt != newest.attempt) ++earlier;
  std::erase_if(samples_, [&](const EquitySample& s) {
    if (s.attempt == newest.attempt) return false;
    if (s.time < old || earlier > 100'000) { --earlier; return true; }
    return false;
  });
  const auto temporary = file_.string() + ".tmp";
  std::uint64_t bytes = 0;
  for (const auto& sample : samples_) bytes += line(sample).size();
  trading::check_storage_space(file_, bytes);
  std::ofstream out(temporary, std::ios::trunc);
  if (!out) throw std::runtime_error("cannot compact equity history");
  for (const auto& sample : samples_) out << line(sample);
  out.flush();
  if (!out) throw std::runtime_error("cannot flush equity history");
  out.close();
  std::error_code ec;
  std::filesystem::rename(temporary, file_, ec);
  if (ec) throw std::runtime_error("cannot replace equity history: " + ec.message());
  lines_ = samples_.size();
}
void EquityStore::append(const EquitySample& sample) noexcept {
  try {
    if (read_only_ || sample.time <= 0 || sample.attempt == 0) return;
    if (!samples_.empty()) {
      const auto& last = samples_.back();
      if (sample.time < last.time) return;
      // One mark per minute, but keep a floor change and the first mark at or past the
      // target or the floor. A decision is retained even when none of those change.
      const auto side = [](const EquitySample& s) {
        return std::pair{s.target && s.equity >= *s.target, s.floor && s.equity <= *s.floor};
      };
      if (!sample.decision && sample.attempt == last.attempt && sample.fill == 0 && sample.stock_fill == 0 && last.time / md::kNanosPerMinute == sample.time / md::kNanosPerMinute &&
          last.floor == sample.floor && last.target == sample.target && side(last) == side(sample) &&
          (!sample.risk || last.risk)) return;
      if (sample.fill != 0 && std::any_of(samples_.rbegin(), samples_.rend(), [&](const EquitySample& s) { return s.fill == sample.fill; })) return;
      if (sample.stock_fill != 0 && std::any_of(samples_.rbegin(), samples_.rend(), [&](const EquitySample& s) { return s.stock_fill == sample.stock_fill; })) return;
    }
    const bool reset = !samples_.empty() && samples_.back().attempt != sample.attempt;
    samples_.push_back(sample);
    if (!error_.empty() && !error_recovered_) {
      compact();
      error_recovered_ = true;
      return;
    }
    const auto row = line(sample);
    trading::check_storage_space(file_, row.size());
    std::ofstream out(file_, std::ios::app);
    out << row;
    out.flush();
    if (!out) throw std::runtime_error("cannot append equity history");
    ++lines_;
    if (reset || lines_ % 4096 == 0) compact();
  } catch (const std::exception& e) { fail(e.what(), sample.time); }
}
}  // namespace openport::server
