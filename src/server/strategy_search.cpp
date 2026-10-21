#include "openport/server/strategy_search.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "openport/pricing/black.hpp"
#include "openport/server/paper.hpp"
#include "paper_json.hpp"
#include "openport/trading/types.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
using analytics::OptionMetrics;
using analytics::SliceMetrics;
using pricing::OptionType;

[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument(reason); }
bool finite(double value) { return std::isfinite(value); }
json nullable(double value) { return std::isfinite(value) ? json(value) : json(nullptr); }
/// A strike or price as the terminal writes it: 5000, 4987.5.
std::string text(double value) {
  std::ostringstream out;
  out.precision(10);
  out << value;
  return out.str();
}
double decimal(const std::string& value, const char* name, double low, double high) {
  double result = 0;
  if (!query_decimal(value, result) || !(result >= low && result <= high)) invalid(std::string(name) + " is outside its allowed range");
  return result;
}
std::size_t count(const std::string& value, const char* name, std::size_t low, std::size_t high) {
  std::size_t result = 0;
  const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (ec != std::errc{} || end != value.data() + value.size() || result < low || result > high)
    invalid(std::string(name) + " must be a whole number from " + std::to_string(low) + " to " + std::to_string(high));
  return result;
}
std::vector<std::string> items(const std::string& value) {
  std::vector<std::string> result;
  std::string item;
  std::istringstream input(value);
  while (std::getline(input, item, ',')) result.push_back(item);
  if (value.empty() || value.back() == ',') result.push_back({});
  return result;
}
std::string expiry_id(const SliceMetrics& slice) {
  return md::format_date(slice.expiry) + (slice.expiry_time == md::new_york_to_utc(slice.expiry, 9, 30) ? "AM" : "PM") +
         (slice.root.empty() ? "" : "-" + slice.root);
}
std::string short_date(md::Date date) {
  static constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  return std::string(months[(date.month - 1) % 12]) + " " + std::to_string(date.day);
}

/// One listed option a candidate can trade: its quote and the volatility it is valued at.
struct Quote {
  const OptionMetrics* option = nullptr;
  double iv = 0;
};
struct Expiry {
  const SliceMetrics* slice = nullptr;
  std::string id;
  std::vector<double> strikes;  ///< Ascending; each has a usable call, put or both.
  std::map<double, Quote> calls, puts;
  const Quote* find(OptionType type, double strike) const {
    const auto& side = type == OptionType::Call ? calls : puts;
    const auto it = side.find(strike);
    return it == side.end() ? nullptr : &it->second;
  }
};
struct Leg {
  const Expiry* expiry = nullptr;
  const Quote* quote = nullptr;
  OptionType type = OptionType::Call;
  double strike = 0;
  int ratio = 1;
  bool buy = true;
};
struct Candidate {
  std::string structure, name;
  std::vector<Leg> legs;
  int shares = 0;  ///< Shares bought with each unit (covered calls).
};
struct Scored {
  Candidate candidate;
  md::Timestamp at = 0;
  double net = 0;  ///< Per unit, debit positive.
  double max_loss = 0;
  std::optional<double> max_profit;
  std::vector<double> breakevens;
  double pop_neutral = 0, pop_user = 0, expected_neutral = 0, expected_user = 0, return_on_risk = 0;
  double sigma_neutral = 0, sigma_user = 0;
  long long quantity = 0;
};

/// The candidate's value at `at` as a function of the underlying's price then, per
/// unit and less what it cost: risk_profile's model, legs that have expired at
/// intrinsic value.
struct Valuer {
  const Candidate& candidate;
  double spot = 0, net = 0, shift = 0;
  md::Timestamp at = 0;
  double operator()(double price) const {
    double total = candidate.shares * (price - spot) - net * 100;
    for (const auto& leg : candidate.legs) {
      const auto& slice = *leg.expiry->slice;
      const double life = std::max(0.0, md::years_between(at, slice.expiry_time));
      double value = 0;
      if (life <= 0) value = std::max(0.0, leg.type == OptionType::Call ? price - leg.strike : leg.strike - price);
      else {
        const double share = slice.years > 0 ? std::min(1.0, life / slice.years) : 0;
        const double forward = (spot + (slice.forward.forward - spot) * share) * price / spot;
        const double discount = std::pow(slice.forward.discount, share);
        value = pricing::black_price(leg.type, forward, leg.strike, life, std::max(0.0001, leg.quote->iv + shift / 100), discount);
      }
      total += (leg.buy ? 1 : -1) * leg.ratio * 100 * value;
    }
    return total;
  }
};

constexpr int kNodes = 161;
constexpr double kSpan = 6;
/// Normal quadrature nodes and weights on [-6, 6], weights summing to one.
const std::pair<std::array<double, kNodes>, std::array<double, kNodes>>& nodes() {
  static const auto value = [] {
    std::array<double, kNodes> z{}, w{};
    double total = 0;
    for (int i = 0; i < kNodes; ++i) {
      z[i] = -kSpan + 2 * kSpan * i / (kNodes - 1);
      w[i] = std::exp(-z[i] * z[i] / 2);
      total += w[i];
    }
    for (auto& weight : w) weight /= total;
    return std::pair{z, w};
  }();
  return value;
}

/// Lognormal with mean `center` and log variance `variance`, as price_odds has it.
struct Distribution {
  double center = 0, variance = 0;
  double above(double level) const {
    if (level <= 0) return 1;
    if (!std::isfinite(level)) return 0;
    return price_odds(center, level, variance).above;
  }
  double price(double z) const { return center * std::exp(-variance / 2 + std::sqrt(variance) * z); }
};

double expected(const Valuer& value, const Distribution& d) {
  const auto& [z, w] = nodes();
  double total = 0;
  for (int i = 0; i < kNodes; ++i) total += w[i] * value(d.price(z[i]));
  return total;
}
/// Where the payoff changes sign between the sampled prices, bisected in log price.
std::vector<double> breakevens(const Valuer& value, const std::vector<double>& grid, const std::vector<double>& values) {
  std::vector<double> result;
  for (std::size_t i = 1; i < grid.size(); ++i) {
    if ((values[i - 1] > 0) == (values[i] > 0)) continue;
    double low = grid[i - 1], high = grid[i];
    for (int step = 0; step < 40; ++step) {
      const double middle = std::sqrt(low * high);
      ((value(middle) > 0) == (values[i - 1] > 0) ? low : high) = middle;
    }
    result.push_back(std::sqrt(low * high));
  }
  return result;
}
/// Probability mass where the payoff is positive, split at its breakevens.
double profit_probability(const Valuer& value, const std::vector<double>& breakevens, const std::vector<double>& grid,
                          const Distribution& d) {
  std::vector<double> edges{0};
  edges.insert(edges.end(), breakevens.begin(), breakevens.end());
  edges.push_back(std::numeric_limits<double>::infinity());
  double total = 0;
  for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
    const double low = edges[i], high = edges[i + 1];
    const double probe = i == 0 ? std::min(grid.front(), high * 0.999) : !std::isfinite(high) ? std::max(grid.back(), low * 1.001) : std::sqrt(low * high);
    if (value(probe) > 0) total += std::max(0.0, d.above(low) - d.above(high));
  }
  return std::clamp(total, 0.0, 1.0);
}

void add_quote(std::map<double, Quote>& side, double strike, double smile, const OptionMetrics& option) {
  if (option.id == analytics::kNoInstrument || !trading::eligible(option.contract).ok()) return;
  if (!finite(option.bid) || !finite(option.ask) || !finite(option.mid) || option.bid < 0 || !(option.ask > 0) || option.ask < option.bid) return;
  const double iv = smile > 0 && finite(smile) ? smile : option.iv;
  if (!(iv > 0) || !finite(iv)) return;
  side[strike] = {&option, iv};
}
}  // namespace

StrategySearchRequest parse_strategy_search(const std::map<std::string, std::string>& query) {
  StrategySearchRequest request;
  bool target = false, budget = false;
  for (const auto& [key, value] : query) {
    if (key == "target") { request.target_price = decimal(value, "target", 1e-6, 1e9); target = true; }
    else if (key == "budget") { request.budget = decimal(value, "budget", 0.01, 1e9); budget = true; }
    else if (key == "vol_points") request.vol_points = decimal(value, "vol_points", -50, 100);
    else if (key == "date") {
      md::Date date;
      const auto parse = [&](std::size_t from, std::size_t size, int& out) {
        const auto [end, ec] = std::from_chars(value.data() + from, value.data() + from + size, out);
        return ec == std::errc{} && end == value.data() + from + size;
      };
      if (value.size() != 10 || value[4] != '-' || value[7] != '-' || !parse(0, 4, date.year) || !parse(5, 2, date.month) ||
          !parse(8, 2, date.day) || !md::valid_date(date)) invalid("date must be YYYY-MM-DD");
      request.target_date = date;
      request.dated = true;
    } else if (key == "expiry_ids") {
      request.expiries.clear();
      for (auto& id : items(value)) {
        if (id.size() < 12 || id.size() > 24) invalid("expiry_ids must list expiry IDs");
        if (std::find(request.expiries.begin(), request.expiries.end(), id) == request.expiries.end()) request.expiries.push_back(id);
      }
      if (request.expiries.size() > kMaxSearchExpiries) invalid("expiry_ids lists at most four expiries");
    } else if (key == "structures") {
      static const std::set<std::string> known{"vertical", "condor", "butterfly", "calendar", "single", "covered"};
      request.structures.clear();
      for (auto& name : items(value)) {
        if (!known.contains(name)) invalid("structures must be vertical, condor, butterfly, calendar, single or covered");
        if (std::find(request.structures.begin(), request.structures.end(), name) == request.structures.end()) request.structures.push_back(name);
      }
    } else if (key == "rank") {
      if (value != "expected" && value != "risk_neutral" && value != "pop" && value != "return_on_risk")
        invalid("rank must be expected, risk_neutral, pop or return_on_risk");
      request.rank = value;
    } else if (key == "price") {
      if (value != "natural" && value != "mid") invalid("price must be natural or mid");
      request.mid = value == "mid";
    } else if (key == "limit") request.limit = count(value, "limit", 1, 100);
    else if (key == "max_candidates") request.max_candidates = count(value, "max_candidates", 1, kMaxSearchCandidates);
    else invalid("Unknown query parameter " + key);
  }
  if (!target) invalid("target is required: the expected price at the target date");
  if (!budget) invalid("budget is required: the most one candidate may lose, in dollars");
  return request;
}

json search_strategies(const analytics::UnderlyingMetrics& metrics, const StrategySearchRequest& request, const std::atomic_bool* cancel) {
  const double spot = metrics.spot;
  if (!(spot > 0) || !finite(spot)) throw std::invalid_argument("The underlying has no spot price to search from");
  const auto now = metrics.as_of;
  json skipped = json::array();
  // The expiries searched: the chosen ones, else the first four on or after the date.
  std::vector<const SliceMetrics*> slices;
  for (const auto& id : request.expiries) {
    const auto it = std::find_if(metrics.slices.begin(), metrics.slices.end(), [&](const auto& s) { return expiry_id(s) == id; });
    if (it == metrics.slices.end()) throw std::invalid_argument("Unknown expiry " + id);
    slices.push_back(&*it);
  }
  if (request.expiries.empty()) {
    std::vector<const SliceMetrics*> listed;
    for (const auto& slice : metrics.slices) if (slice.expiry_time > now && (!request.dated || slice.expiry >= request.target_date)) listed.push_back(&slice);
    std::sort(listed.begin(), listed.end(), [](const auto* a, const auto* b) { return a->expiry_time < b->expiry_time; });
    for (const auto* slice : listed) if (slices.size() < kMaxSearchExpiries) slices.push_back(slice);
  }
  std::sort(slices.begin(), slices.end(), [](const auto* a, const auto* b) { return a->expiry_time < b->expiry_time; });
  if (slices.empty()) throw std::invalid_argument("No listed expiry settles on or after the target date");
  const md::Date date = request.dated ? request.target_date : slices.front()->expiry;
  const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
  if (close <= now) throw std::invalid_argument("The target date's close has already passed");

  std::vector<Expiry> expiries;
  for (const auto* slice : slices) {
    const auto id = expiry_id(*slice);
    if (slice->expiry_time <= now) { skipped.push_back({{"expiry", id}, {"reason", "Expired"}}); continue; }
    if (slice->expiry < date) { skipped.push_back({{"expiry", id}, {"reason", "Settles before the target date"}}); continue; }
    if (!slice->forward.ok || !(slice->forward.forward > 0) || !(slice->years > 0)) {
      skipped.push_back({{"expiry", id}, {"reason", "No parity forward for this expiry"}}); continue;
    }
    // Strikes within 2.5 standard deviations of the spot and the target, nearest their middle first.
    const auto variance = implied_variance_until(metrics, slice->expiry_time).value_or(slice->atm_iv * slice->atm_iv * slice->years);
    const double sd = std::sqrt(std::max(variance, 0.0001));
    const double low = std::min(spot, request.target_price) * std::exp(-2.5 * sd), high = std::max(spot, request.target_price) * std::exp(2.5 * sd);
    Expiry expiry{slice, id, {}, {}, {}};
    for (const auto& row : slice->strikes) {
      if (!(row.strike >= low && row.strike <= high)) continue;
      add_quote(expiry.calls, row.strike, row.iv, row.call);
      add_quote(expiry.puts, row.strike, row.iv, row.put);
      if (expiry.calls.contains(row.strike) || expiry.puts.contains(row.strike)) expiry.strikes.push_back(row.strike);
    }
    if (expiry.strikes.size() > kMaxSearchStrikes) {
      const double middle = std::sqrt(spot * request.target_price);
      auto kept = expiry.strikes;
      std::stable_sort(kept.begin(), kept.end(), [&](double a, double b) { return std::abs(std::log(a / middle)) < std::abs(std::log(b / middle)); });
      kept.resize(kMaxSearchStrikes);
      std::sort(kept.begin(), kept.end());
      for (const double strike : expiry.strikes) if (!std::binary_search(kept.begin(), kept.end(), strike)) { expiry.calls.erase(strike); expiry.puts.erase(strike); }
      expiry.strikes = std::move(kept);
    }
    std::sort(expiry.strikes.begin(), expiry.strikes.end());
    if (expiry.strikes.empty()) { skipped.push_back({{"expiry", id}, {"reason", "No tradable two-sided quotes near the spot and target"}}); continue; }
    expiries.push_back(std::move(expiry));
  }

  const bool shares = !md::is_index_underlying(metrics.symbol);
  const auto wants = [&](const char* name) { return std::find(request.structures.begin(), request.structures.end(), name) != request.structures.end(); };
  std::map<md::Timestamp, std::pair<Distribution, Distribution>> distributions;
  std::vector<Scored> results;
  std::size_t considered = 0, evaluated = 0, unbounded = 0, over_budget = 0;
  bool truncated = false;
  const auto sign = [](const Leg& leg) { return leg.buy ? leg.ratio : -leg.ratio; };
  // Every bounded candidate is listed first; past max_candidates an even stride
  // through the list is valued, so each structure and expiry keeps its share.
  std::vector<Candidate> listed;
  const auto score = [&](Candidate candidate) {
    if (listed.size() >= kMaxListedCandidates) { truncated = true; return; }
    double calls = candidate.shares / 100.0;
    for (const auto& leg : candidate.legs) if (leg.type == OptionType::Call) calls += sign(leg);
    // A net short call position loses without bound as the price rises.
    if (calls < -1e-9) { ++unbounded; return; }
    listed.push_back(std::move(candidate));
  };
  const auto value = [&](Candidate candidate) {
    md::Timestamp at = close;
    double net = candidate.shares * spot / 100, calls = candidate.shares / 100.0;
    for (const auto& leg : candidate.legs) {
      at = std::min(at, leg.expiry->slice->expiry_time);
      const auto& q = *leg.quote->option;
      net += sign(leg) * (request.mid ? q.mid : leg.buy ? q.ask : q.bid);
      if (leg.type == OptionType::Call) calls += sign(leg);
    }
    ++evaluated;
    auto& [neutral, user] = distributions[at];
    if (neutral.center == 0) {
      const double variance = implied_variance_until(metrics, at).value_or(0);
      neutral = {spot, variance};
      const double years = std::max(md::years_between(now, at), 1e-9);
      const double vol = std::max(0.01, std::sqrt(variance / years) + request.vol_points / 100);
      user = {request.target_price, vol * vol * years};
    }
    Scored scored;
    scored.candidate = std::move(candidate);
    scored.at = at;
    scored.net = net;
    const Valuer neutral_value{scored.candidate, spot, net, 0, at}, user_value{scored.candidate, spot, net, request.vol_points, at};
    // Prices where the payoff is sampled for its extremes and breakevens.
    std::vector<double> grid{spot * 1e-4, 10 * std::max(spot, request.target_price)};
    const auto& z = nodes().first;
    for (int i = 0; i < kNodes; i += 2) { grid.push_back(neutral.price(z[i])); grid.push_back(user.price(z[i])); }
    for (const auto& leg : scored.candidate.legs) grid.push_back(leg.strike);
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());
    std::vector<double> values;
    values.reserve(grid.size());
    for (const double price : grid) values.push_back(neutral_value(price));
    scored.max_loss = std::max(0.0, -*std::min_element(values.begin(), values.end()));
    if (calls <= 1e-9) scored.max_profit = *std::max_element(values.begin(), values.end());
    if (!(scored.max_loss >= 0.01)) return;  // No loss anywhere: a stale or crossed quote.
    if (scored.max_loss > request.budget) { ++over_budget; return; }
    scored.breakevens = breakevens(neutral_value, grid, values);
    // A volatility change moves the breakevens of legs still alive at the time.
    auto shifted = scored.breakevens;
    if (request.vol_points != 0) {
      for (std::size_t i = 0; i < grid.size(); ++i) values[i] = user_value(grid[i]);
      shifted = breakevens(user_value, grid, values);
    }
    scored.pop_neutral = profit_probability(neutral_value, scored.breakevens, grid, neutral);
    scored.pop_user = profit_probability(user_value, shifted, grid, user);
    scored.expected_neutral = expected(neutral_value, neutral);
    scored.expected_user = expected(user_value, user);
    scored.return_on_risk = scored.expected_user / scored.max_loss;
    scored.sigma_neutral = std::sqrt(neutral.variance);
    scored.sigma_user = std::sqrt(user.variance);
    scored.quantity = static_cast<long long>(std::floor(request.budget / scored.max_loss + 1e-9));
    results.push_back(std::move(scored));
  };
  const auto leg = [](const Expiry& e, OptionType type, double strike, bool buy, int ratio = 1) -> std::optional<Leg> {
    const auto* quote = e.find(type, strike);
    if (!quote) return std::nullopt;
    return Leg{&e, quote, type, strike, ratio, buy};
  };
  const auto label = [](OptionType type) { return type == OptionType::Call ? std::string("call") : std::string("put"); };
  for (const auto& e : expiries) {
    const auto& k = e.strikes;
    const auto day = " " + short_date(e.slice->expiry);
    if (wants("single"))
      for (const double strike : k)
        for (const auto type : {OptionType::Call, OptionType::Put})
          for (const bool buy : {true, false})
            if (const auto l = leg(e, type, strike, buy))
              score({"single", std::string(buy ? "Long " : "Short ") + label(type) + " " + text(strike) + day, {*l}, 0});
    if (wants("covered") && shares)
      for (const double strike : k)
        if (const auto l = leg(e, OptionType::Call, strike, false)) score({"covered", "Covered call " + text(strike) + day, {*l}, 100});
    if (wants("vertical"))
      for (std::size_t i = 0; i < k.size(); ++i)
        for (std::size_t j = i + 1; j < k.size() && j <= i + 8; ++j)
          for (const auto type : {OptionType::Call, OptionType::Put})
            for (const bool bull : {true, false}) {
              // Bulls buy the lower call or sell the higher put.
              const bool buy_low = type == OptionType::Call ? bull : !bull;
              const auto low = leg(e, type, k[i], buy_low), high = leg(e, type, k[j], !buy_low);
              if (low && high) score({"vertical", std::string(bull ? "Bull " : "Bear ") + label(type) + " " + text(k[i]) + "/" + text(k[j]) + day, {*low, *high}, 0});
            }
    if (wants("butterfly"))
      for (std::size_t c = 1; c + 1 < k.size(); ++c)
        for (std::size_t w = 1; w <= 6 && w <= c && c + w < k.size(); ++w) {
          if (std::abs((k[c] - k[c - w]) - (k[c + w] - k[c])) > 1e-9) continue;
          for (const auto type : {OptionType::Call, OptionType::Put}) {
            const auto low = leg(e, type, k[c - w], true), middle = leg(e, type, k[c], false, 2), high = leg(e, type, k[c + w], true);
            if (low && middle && high)
              score({"butterfly", "Long " + label(type) + " butterfly " + text(k[c - w]) + "/" + text(k[c]) + "/" + text(k[c + w]) + day, {*low, *middle, *high}, 0});
          }
        }
    if (wants("condor"))
      for (std::size_t p = 1; p < k.size(); ++p)
        for (std::size_t c = p + 1; c + 1 < k.size() && c <= p + 16; ++c)
          for (std::size_t w = 1; w <= 4 && w <= p && c + w < k.size(); ++w) {
            const auto wing = leg(e, OptionType::Put, k[p - w], true), put = leg(e, OptionType::Put, k[p], false);
            const auto call = leg(e, OptionType::Call, k[c], false), cap = leg(e, OptionType::Call, k[c + w], true);
            if (wing && put && call && cap)
              score({"condor", "Short iron condor " + text(k[p - w]) + "/" + text(k[p]) + "/" + text(k[c]) + "/" + text(k[c + w]) + day,
                     {*wing, *put, *call, *cap}, 0});
          }
  }
  if (wants("calendar"))
    for (std::size_t n = 0; n < expiries.size(); ++n)
      for (std::size_t f = n + 1; f < expiries.size(); ++f) {
        const auto& near = expiries[n];
        const auto& far = expiries[f];
        if (far.slice->expiry_time <= near.slice->expiry_time) continue;
        for (const double strike : near.strikes)
          for (const auto type : {OptionType::Call, OptionType::Put}) {
            const auto short_leg = leg(near, type, strike, false), long_leg = leg(far, type, strike, true);
            if (short_leg && long_leg)
              score({"calendar", "Long " + label(type) + " calendar " + text(strike) + " " + short_date(near.slice->expiry) + "/" + short_date(far.slice->expiry),
                     {*short_leg, *long_leg}, 0});
          }
      }

  const std::size_t stride = std::max<std::size_t>(1, (listed.size() + request.max_candidates - 1) / request.max_candidates);
  truncated |= stride > 1;
  considered = listed.size();
  for (std::size_t i = 0; i < listed.size(); i += stride) {
    if (cancel && (evaluated % 256) == 0 && cancel->load()) { truncated = true; break; }
    value(std::move(listed[i]));
  }
  const auto key = [&](const Scored& s) {
    return request.rank == "risk_neutral" ? s.expected_neutral : request.rank == "pop" ? s.pop_user
        : request.rank == "return_on_risk" ? s.return_on_risk : s.expected_user;
  };
  std::stable_sort(results.begin(), results.end(), [&](const Scored& a, const Scored& b) {
    const double x = key(a), y = key(b);
    if (x != y) return x > y;
    return a.max_loss < b.max_loss;
  });
  json candidates = json::array();
  for (std::size_t i = 0; i < results.size() && i < request.limit; ++i) {
    const auto& s = results[i];
    json legs = json::array();
    for (const auto& l : s.candidate.legs) {
      const auto& q = *l.quote->option;
      legs.push_back({{"symbol", q.contract.osi_symbol()}, {"side", l.buy ? "buy" : "sell"}, {"ratio", l.ratio},
                      {"type", l.type == OptionType::Call ? "call" : "put"}, {"strike", l.strike}, {"expiry", l.expiry->id},
                      {"bid", nullable(q.bid)}, {"ask", nullable(q.ask)}, {"mid", nullable(q.mid)}, {"iv", l.quote->iv}});
    }
    candidates.push_back({{"name", s.candidate.name}, {"structure", s.candidate.structure}, {"legs", legs},
        {"shares", s.candidate.shares}, {"net", s.net}, {"max_loss", s.max_loss},
        {"max_profit", s.max_profit ? json(*s.max_profit) : json(nullptr)}, {"breakevens", s.breakevens},
        {"pop", {{"risk_neutral", s.pop_neutral}, {"user", s.pop_user}}},
        {"expected", {{"risk_neutral", s.expected_neutral}, {"user", s.expected_user}}},
        {"return_on_risk", s.return_on_risk}, {"quantity", s.quantity},
        {"valued_at", md::format_timestamp(s.at)}, {"sigma", {{"risk_neutral", s.sigma_neutral}, {"user", s.sigma_user}}}});
  }
  json searched = json::array();
  for (const auto& e : expiries) searched.push_back(e.id);
  return {{"symbol", metrics.symbol}, {"as_of", md::format_timestamp(now)}, {"spot", spot},
          {"target_price", request.target_price}, {"target_date", md::format_date(date)}, {"target_time", md::format_timestamp(close)},
          {"budget", request.budget}, {"vol_points", request.vol_points}, {"rank", request.rank}, {"price", request.mid ? "mid" : "natural"},
          {"structures", request.structures}, {"expiries", searched}, {"skipped", skipped},
          {"considered", considered}, {"evaluated", evaluated}, {"unbounded", unbounded}, {"over_budget", over_budget},
          {"within_budget", results.size()}, {"truncated", truncated}, {"max_candidates", request.max_candidates},
          {"candidates", candidates},
          {"label", "Derived estimates from current quotes, not a forecast or investment advice"},
          {"model", "Per unit, entered at the far sides (or mids) and valued at the target date's close or the first leg's expiry: "
                    "Black-76 on each strike's smile IV (plus vol_points for the user's distribution), remaining life, carry and discounting "
                    "shrinking as in the risk profile; expired legs at intrinsic value. Lognormal distributions on the at-the-money implied "
                    "variance to that time: risk-neutral with the forward at spot, the user's with its mean at the target price and its "
                    "volatility changed by vol_points. Fees and early exercise are not modelled; net short calls (unbounded loss) are excluded."}};
}

struct StrategySearchPool::Impl {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::function<void()>> queue;
  std::vector<std::thread> threads;
  std::size_t capacity = 0, outstanding = 0;  ///< Jobs running or waiting, at most workers + queue.
  bool stopping = false;
  void run() {
    while (true) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping && queue.empty()) return;
        job = std::move(queue.front());
        queue.pop_front();
      }
      try { job(); } catch (...) { /* A job completes its own request; nothing escapes a worker. */ }
      const std::lock_guard lock(mutex);
      --outstanding;
    }
  }
};
StrategySearchPool::StrategySearchPool(std::size_t workers, std::size_t queue) : impl_(std::make_unique<Impl>()) {
  impl_->capacity = std::max<std::size_t>(1, workers) + queue;
  for (std::size_t i = 0; i < std::max<std::size_t>(1, workers); ++i) impl_->threads.emplace_back([impl = impl_.get()] { impl->run(); });
}
StrategySearchPool::~StrategySearchPool() {
  {
    const std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
  }
  impl_->ready.notify_all();
  for (auto& thread : impl_->threads) thread.join();
}
bool StrategySearchPool::submit(std::function<void()> job) {
  {
    const std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->outstanding >= impl_->capacity) return false;
    ++impl_->outstanding;
    impl_->queue.push_back(std::move(job));
  }
  impl_->ready.notify_one();
  return true;
}
StrategySearchPool& StrategySearchPool::shared() {
  static StrategySearchPool pool(2, 4);
  return pool;
}

}  // namespace openport::server
