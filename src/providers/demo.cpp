#include "openport/providers/demo.hpp"
#include "openport/providers/scenario.hpp"

#ifdef OPENPORT_SANITIZER_TEST_SUPPORT
#include "scenario_sampling.hpp"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "openport/md/contract.hpp"
#include "openport/md/recording.hpp"
#include "openport/pricing/black.hpp"
#include "openport/pricing/binomial.hpp"
#include "openport/providers/snapshot.hpp"
#include "openport/trading/types.hpp"

namespace openport::providers {
namespace {

constexpr md::Timestamp kStep = 15 * md::kNanosPerSecond;
constexpr double kRate = 0.04;
constexpr double kDividend = 0.013;
constexpr double kOpen = 6000.0;     // SPX at the open
constexpr double kSpyRatio = 10.02;  // index points per SPY dollar
constexpr double kQqqOpen = 480.0;
constexpr double kQqqBeta = 1.25;  // QQQ's moves against SPX's
constexpr double kNdxOpen = 21000.0;
constexpr double kRutOpen = 2300.0;
constexpr double kRutBeta = 1.15;
constexpr double kVixCenter = 17.0;
constexpr double kVixMean = 19.5;  // the long-run VIX forward

/// splitmix64 with Box-Muller: the same draws on every standard library, which
/// <random>'s distributions do not promise.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  /// Uniform in (0, 1).
  double uniform() { return (static_cast<double>(next() >> 11) + 0.5) * 0x1.0p-53; }
  double normal() {
    const double radius = std::sqrt(-2 * std::log(uniform()));
    return radius * std::cos(2 * std::numbers::pi * uniform());
  }

 private:
  std::uint64_t state_;
};

/// A draw in (0, 1) that depends only on its arguments, not on the order of calls.
double draw(std::uint64_t seed, std::uint64_t a, std::uint64_t b) {
  Random random(seed ^ (a * 0xD6E8FEB86659FD93ull) ^ (b * 0xA0761D6478BD642Full));
  random.next();
  return random.uniform();
}

double cents(double value) { return std::round(value * 100) / 100; }

md::Date next_business_day(md::Date date) {
  // After 17:00 the trading date is the next business day.
  return md::trading_date(md::new_york_to_utc(date, 18, 0));
}
bool business_day(md::Date date) { return md::trading_date(md::new_york_to_utc(date, 12, 0)) == date; }
md::Date friday_from(md::Date date) {
  auto days = md::days_since_epoch(date);
  while (md::weekday(md::date_from_days(days)) != 5) ++days;
  return md::date_from_days(days);
}
/// A Friday expiry moves to the business day before when the exchange is closed.
md::Date expiry_on(md::Date friday) { return business_day(friday) ? friday : md::previous_business_day(friday); }

struct Series {
  std::string root;
  md::Date expiry;
  double step;    // strike spacing
  double window;  // strikes within this fraction of the open
  bool monthly;
};

std::vector<md::OptionContract> list(const Series& series, double center) {
  std::vector<md::OptionContract> contracts;
  const double low = std::ceil(center * (1 - series.window) / series.step) * series.step;
  const double high = std::floor(center * (1 + series.window) / series.step) * series.step;
  for (double strike = low; strike <= high + 1e-9; strike += series.step) {
    for (const char right : {'C', 'P'}) {
      char osi[32];
      std::snprintf(osi, sizeof osi, "%s%02d%02d%02d%c%08lld", series.root.c_str(), series.expiry.year % 100,
                    series.expiry.month, series.expiry.day, right, std::llround(strike * 1000));
      if (const auto contract = md::parse_osi(osi)) contracts.push_back(*contract);
    }
  }
  return contracts;
}

/// Thirty days before next month's third Friday, with holiday adjustments.
md::Date vix_expiry(int year, int month) {
  const md::Date next{month == 12 ? year + 1 : year, month == 12 ? 1 : month + 1, 1};
  const auto friday = expiry_on(md::date_from_days(md::days_since_epoch(friday_from(next)) + 14));
  return expiry_on(md::date_from_days(md::days_since_epoch(friday) - 30));
}
bool am_settled(const std::string& root) { return md::conventions_for_root(root).settlement == md::Settlement::AM; }

/// Daily and weekly PM series, next month's monthly, and revision 3's VIX expiries.
std::vector<Series> listed_on(md::Date date, int revision) {
  const auto d1 = next_business_day(date);
  const auto d2 = next_business_day(d1);
  const auto weekly = expiry_on(friday_from(md::date_from_days(md::days_since_epoch(d2) + 1)));
  const md::Date month{date.month == 12 ? date.year + 1 : date.year, date.month == 12 ? 1 : date.month + 1, 1};
  const auto monthly = expiry_on(md::date_from_days(md::days_since_epoch(friday_from(month)) + 14));
  std::vector<Series> out;
  for (const auto& expiry : {date, d1, d2}) {
    out.push_back({"SPXW", expiry, 5, 0.03, false});
    out.push_back({"SPY", expiry, 1, 0.03, false});
    out.push_back({"QQQ", expiry, 1, 0.03, false});
  }
  out.push_back({"SPXW", weekly, 5, 0.04, false});
  out.push_back({"SPY", weekly, 1, 0.04, false});
  out.push_back({"QQQ", weekly, 1, 0.04, false});
  out.push_back({"SPX", monthly, 10, 0.06, true});
  out.push_back({"SPY", monthly, 1, 0.06, true});
  out.push_back({"QQQ", monthly, 1, 0.06, true});
  if (revision < 3) return out;
  for (const auto& expiry : {date, d1, d2}) {
    out.push_back({"XSP", expiry, 1, 0.03, false});
    out.push_back({"NDXP", expiry, 25, 0.03, false});
    out.push_back({"RUTW", expiry, 5, 0.03, false});
  }
  out.push_back({"XSP", weekly, 1, 0.04, false});
  out.push_back({"NDXP", weekly, 25, 0.04, false});
  out.push_back({"RUTW", weekly, 5, 0.04, false});
  out.push_back({"XSP", monthly, 1, 0.06, true});
  out.push_back({"NDX", monthly, 50, 0.06, true});
  out.push_back({"RUT", monthly, 10, 0.06, true});
  // Two AM monthlies and two Wednesday weeklies, omitting monthly dates.
  std::vector<md::Date> months;
  for (int ahead = 0; months.size() < 2; ++ahead) {
    const int offset = date.month - 1 + ahead;
    const auto expiry = vix_expiry(date.year + offset / 12, offset % 12 + 1);
    if (expiry > date) months.push_back(expiry);
  }
  for (auto day = md::days_since_epoch(date) + 1, count = std::int64_t{0}; count < 2; ++day) {
    if (md::weekday(md::date_from_days(day)) != 3) continue;
    const auto expiry = expiry_on(md::date_from_days(day));
    if (expiry <= date || std::find(months.begin(), months.end(), expiry) != months.end()) continue;
    out.push_back({"VIXW", expiry, 1, 0.6, false});
    ++count;
  }
  for (const auto& expiry : months) out.push_back({"VIX", expiry, 1, 0.8, true});
  return out;
}

/// One contract of a day's chain, with what its open interest follows.
struct Listing {
  md::OptionContract contract;
  bool monthly = false;
  double center = 0;  ///< the underlying's open, around which strikes are listed
};
/// Where each underlying's strikes are listed around: its opening level.
struct Centers {
  double spx = kOpen;
  double spy = kOpen / kSpyRatio;
  double qqq = kQqqOpen;
  double ndx = kNdxOpen;
  double rut = kRutOpen;
  double vix = kVixCenter;
  double for_symbol(const std::string& symbol) const {
    if (symbol == "SPX") return spx;
    if (symbol == "XSP") return spx / 10;
    if (symbol == "SPY") return spy;
    if (symbol == "QQQ") return qqq;
    if (symbol == "NDX") return ndx;
    if (symbol == "RUT") return rut;
    return vix;
  }
};
std::vector<Listing> listings(const Scenario& script, md::Date date, int revision, const Centers& centers = {}) {
  if (revision < 1 || revision > kScenarioRevision) throw std::invalid_argument("Unsupported scenario revision");
  auto series = listed_on(date, revision);
  if (revision >= 2) {
    // Every series an earlier date listed that still trades, as wide as it ever was,
    // after the date's own: a position opened on an earlier day keeps its quotes.
    // AM-settled series stop the day before.
    std::map<std::pair<std::string, md::Date>, Series> earlier;
    const auto first = md::days_since_epoch(date) - 56;
    for (auto day = md::previous_business_day(date); md::days_since_epoch(day) >= first; day = md::previous_business_day(day)) {
      for (const auto& s : listed_on(day, revision)) {
        if (am_settled(s.root) ? s.expiry <= date : s.expiry < date) continue;
        const auto [it, added] = earlier.try_emplace({s.root, s.expiry}, s);
        if (added) continue;
        it->second.window = std::max(it->second.window, s.window);
        it->second.monthly = it->second.monthly || s.monthly;
      }
    }
    for (const auto& [key, s] : earlier) series.push_back(s);
  }
  std::vector<Listing> out;
  std::set<std::string> defined;
  for (auto s : series) {
    s.window = std::max(s.window, script.strike_window);
    const auto symbol = md::conventions_for_root(s.root).underlying;
    if (std::find(script.symbols.begin(), script.symbols.end(), symbol) == script.symbols.end()) continue;
    const double center = centers.for_symbol(symbol);
    for (auto& contract : list(s, center))
      if (defined.insert(contract.osi_symbol()).second) out.push_back({std::move(contract), s.monthly, center});
  }
  return out;
}

/// At-the-money implied volatility: a little higher for the shortest expiries and
/// next month, and higher as the index falls (`spot_vol` per unit of return).
double atm_vol(double days, double ratio, double spot_vol, double noise) {
  return 0.132 + 0.018 * std::exp(-days / 1.5) + 0.012 * std::min(days / 30, 1.0) + spot_vol * (ratio - 1) + noise;
}
/// The generated SPX 30-day ATM volatility, in points. Events reach it with the
/// same term weight as SPX options, so spikes and crushes move VIX with that surface.
double vix_level(double ratio, double spot_vol, double noise, double iv_shift, double event_iv) {
  return 100 * std::clamp(atm_vol(30, ratio, spot_vol, noise) + iv_shift +
      event_iv * (0.25 + 0.75 * std::exp(-30.0 / 7)), 0.02, 2.0);
}
/// The skew in standardised moneyness: puts richer, calls cheaper, wings up.
double smile(double atm, double forward, double strike, double years) {
  const double z = std::log(strike / forward) / (atm * std::sqrt(std::max(years, 1e-7)));
  return atm * std::clamp(1 - 0.18 * z + 0.03 * z * z, 0.6, 3.0);
}

struct Quote {
  double bid = 0;
  double ask = 0;
};
/// Cache the product's two tick tiers once per contract, outside the snapshot loop.
struct Ticks {
  double below = 0.01, above = 0.01;
};
Ticks ticks_for(const std::string& root) {
  return {trading::tick_size(root, trading::Money{}).dollars(),
          trading::tick_size(root, trading::Money::from_micros(3'000'000)).dollars()};
}
/// Quotes around the model price on the product's ticks. Liquid wings keep a
/// one-tick bid even when the model premium is sub-tick; remote tails have no bid.
Quote quote(Ticks ticks, double mid, bool liquid_wing) {
  const auto tick = [ticks](double price) { return price < 3 ? ticks.below : ticks.above; };
  const double width = ticks.above == 0.01 ? std::max(0.01, 0.01 * mid + 0.01)
      : ticks.below == 0.01 ? std::max(0.01, 0.015 * mid + 0.01) : std::max(0.05, 0.02 * mid + 0.05);
  const double low = mid - width / 2;
  const double high = mid + width / 2;
  Quote q;
  q.bid = low > 0 ? std::floor(low / tick(low) + 1e-9) * tick(low) : 0;
  if (liquid_wing) q.bid = std::max(q.bid, tick(0));
  q.ask = std::ceil(high / tick(high) - 1e-9) * tick(high);
  q.ask = std::max({q.ask, q.bid + tick(q.bid), tick(0)});
  // The ask's tick follows its own price: 2.95 + a nickel reaches 3.00, a dime tick.
  q.ask = std::ceil(q.ask / tick(q.ask) - 1e-9) * tick(q.ask);
  return {cents(q.bid), cents(q.ask)};
}

struct Interest {
  double base, round, rounder;
};
Interest interest_for(const std::string& symbol) {
  if (symbol == "SPX") return {2500, 25, 100};
  if (symbol == "NDX") return {600, 250, 1000};
  if (symbol == "RUT") return {900, 50, 100};
  if (symbol == "XSP") return {1500, 5, 10};
  if (symbol == "VIX") return {15000, 5, 10};
  return {6000, 5, 10};
}

double open_interest(const md::OptionContract& c, bool monthly, double center, double days, std::uint64_t seed,
                     md::InstrumentId id) {
  const auto interest = interest_for(c.underlying);
  const double x = std::log(c.strike / center);
  const double width = (c.underlying == "VIX" ? 0.2 : 0.012) + 0.01 * std::sqrt(days);
  double oi = interest.base * std::exp(-0.5 * (x / width) * (x / width)) * (monthly ? 3 : 1);
  if (std::fmod(c.strike, interest.round) == 0) oi *= 1.8;
  if (std::fmod(c.strike, interest.rounder) == 0) oi *= 1.6;
  if (c.type == pricing::OptionType::Put ? c.strike < center : c.strike > center) oi *= 1.3;
  return std::round(oi * (0.6 + 0.8 * draw(seed, id, 0x0A11)));
}

class Discard final : public md::EventSink {
 public:
  void publish(md::Event) override {}
};

struct Listed {
  md::OptionContract contract;
  md::Timestamp expiry = 0;  ///< contract.expiry_time(), computed once
  md::Timestamp last_trade = 0;
  md::InstrumentId id = 0;
  bool monthly = false;
  double center = 0;  ///< the level its strikes were listed around
  Ticks ticks;
  Quote last;
  double bid_size = 0;
  double ask_size = 0;
  double volume = 0;
};

const Scenario& script_for(DemoDay day) {
  constexpr const char* ids[] = {"reversal", "trend", "chop", "selloff", "overnight"};
  const auto index = static_cast<std::size_t>(day);
  const std::string_view id = index < 5 ? ids[index] : ids[0];
  for (const auto& scenario : builtin_scenarios())
    if (scenario.id == id) return scenario;
  throw std::invalid_argument("Missing built-in scenario " + std::string(id));
}

/// One session of a run: when it plays and its script.
struct Play {
  ScenarioWindow window;
  const std::vector<std::pair<double, double>>& drift;
  double volatility, iv_shift, spot_vol;
  const std::vector<ScenarioEvent>& events;
  std::vector<ScenarioDividend> dividends;
  std::optional<double> previous_close;
};

}  // namespace

bool simulated_provider(std::string_view name) noexcept {
  return name == kDemoProvider || name == "replay (demo)";
}

const std::vector<DemoInfo>& demo_days() {
  static const std::vector<DemoInfo> days = [] {
    std::vector<DemoInfo> out;
    for (const auto day : {DemoDay::Reversal, DemoDay::Trend, DemoDay::Chop, DemoDay::Selloff, DemoDay::Overnight}) {
      const auto& s = script_for(day);
      out.push_back({day, s.id, s.title, s.description, s.date, s.symbols, scenario_open(s, s.date)});
    }
    return out;
  }();
  return days;
}

const DemoInfo* find_demo_day(std::string_view id) {
  for (const auto& d : demo_days())
    if (d.id == id) return &d;
  return nullptr;
}

void write_demo_recording(const std::filesystem::path& path, const DemoOptions& options) {
  const auto& script = script_for(options.day);
  write_scenario_recording(path, script, options.date.value_or(script.date), options.seed != 0 ? options.seed : script.seed);
}

std::vector<trading::Dividend> demo_dividends(md::Date first, md::Date last) {
  std::vector<trading::Dividend> out;
  for (int year = first.year; year <= last.year; ++year) {
    for (const int month : {3, 6, 9, 12}) {
      const auto friday = md::date_from_days(md::days_since_epoch(friday_from({year, month, 1})) + 14);
      for (const auto& symbol : {"SPY", "QQQ"}) {
        const bool spy = std::string_view(symbol) == "SPY";
        auto ex = spy ? friday : md::date_from_days(md::days_since_epoch(friday) + 3);
        if (!business_day(ex)) ex = next_business_day(ex);
        if (ex < first || ex > last) continue;
        // Fixed opening levels and date-keyed variation: a seed or scenario never
        // changes a payment. Round once to cents before prices or accounts see it.
        const double base = spy ? kOpen / kSpyRatio * 0.003 : kQqqOpen * 0.0015;
        const double amount = cents(base * (0.95 + 0.1 * draw(0x4449564944454E44ULL,
            static_cast<std::uint64_t>(md::days_since_epoch(ex)), spy ? 0 : 1)));
        out.push_back({symbol, ex, trading::Money::from_double(amount)});
      }
    }
  }
  return out;
}

std::vector<trading::Dividend> scenario_dividends(const Scenario& script, md::Date date, int revision) {
  const auto windows = scenario_windows(script, date);
  // ETF listings reach at most next month's third Friday. Include the known
  // payments beyond the run too, for its option prices and American analytics.
  auto out = revision >= 4 ? demo_dividends(date,
      md::date_from_days(md::days_since_epoch(windows.back().date) + 62)) : std::vector<trading::Dividend>{};
  std::erase_if(out, [&](const auto& d) {
    return std::find(script.symbols.begin(), script.symbols.end(), d.symbol) == script.symbols.end();
  });
  for (std::size_t i = 0; i < script.sessions.size(); ++i) {
    for (const auto& d : script.sessions[i].dividends) {
      const auto ex = windows[i].date;
      std::erase_if(out, [&](const auto& generated) { return generated.symbol == d.symbol && generated.ex_date == ex; });
      out.push_back({d.symbol, ex, trading::Money::from_double(d.per_share)});
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return std::tie(a.ex_date, a.symbol) < std::tie(b.ex_date, b.symbol);
  });
  return out;
}

std::vector<md::OptionContract> scenario_chain(const Scenario& script, md::Date date, int revision) {
  std::vector<md::OptionContract> out;
  for (auto& listing : listings(script, date, revision)) out.push_back(std::move(listing.contract));
  return out;
}

void write_scenario_recording(const std::filesystem::path& path, const Scenario& script, md::Date date, std::uint64_t seed,
                              int revision) {
  if (script.generator != 1) throw std::invalid_argument("scenario.generator: only version 1 is supported");
  if (!md::valid_date(date) || !business_day(date)) throw std::invalid_argument("The demo day must be a trading day");
  if (revision < 1 || revision > kScenarioRevision) throw std::invalid_argument("Unsupported scenario revision");
  if (!script.sessions.empty() && revision < 2) throw std::invalid_argument("Multi-session scenarios need revision 2 or later");
  if (revision < 3 && std::any_of(script.symbols.begin(), script.symbols.end(), [](const auto& symbol) {
        return symbol != "SPX" && symbol != "SPY" && symbol != "QQQ";
      })) throw std::invalid_argument("New index underlyings need scenario revision 3 or later");
  const auto windows = [&] {
    try { return scenario_windows(script, date); }
    catch (const std::invalid_argument& error) { throw std::invalid_argument("scenario." + std::string(error.what())); }
  }();
  const auto event_time = [&](const ScenarioEvent& event, const ScenarioWindow& w, bool end = false) {
    const int minutes = end ? event.minutes : 0;
    if (revision >= 5 && !script.events_scaled) return scenario_event_time(event.at, w, minutes);
    const auto at = scenario_time(event.at, w.date, w.session == "overnight") + minutes * md::kNanosPerMinute;
    const auto opens = w.session == "curb" ? w.first - w.step : w.first;
    if (at < opens || (minutes > 0 ? at > w.close : at >= w.close))
      throw std::invalid_argument("outside this date's session");
    return at;
  };
  std::vector<Play> plays;
  if (script.sessions.empty()) {
    plays.push_back({windows.front(), script.drift, script.volatility, script.iv_shift, script.spot_vol, script.events, {}, script.previous_close});
  } else {
    for (std::size_t i = 0; i < windows.size(); ++i) {
      const auto& session = script.sessions[i];
      plays.push_back({windows[i], session.drift, session.volatility, session.iv_shift, session.spot_vol, session.events, session.dividends, session.previous_close});
    }
  }
  for (std::size_t i = 0; i < plays.size(); ++i) {
    const auto& w = plays[i].window;
    for (std::size_t index = 0; index < plays[i].events.size(); ++index) {
      const auto& event = plays[i].events[index];
      if (event.type == "gap") continue;
      try { (void)event_time(event, w); if (event.minutes > 0) (void)event_time(event, w, true); }
      catch (const std::invalid_argument& error) {
        throw std::invalid_argument("scenario.sessions[" + std::to_string(i) + "].events[" +
            std::to_string(index) + "] (" + event.type + " at " + event.at + "): " + error.what());
      }
    }
    if (plays[i].previous_close && revision < 2)
      throw std::invalid_argument("scenario.previous_close needs revision 2 or later");
  }

  const auto dividends = revision >= 4 ? scenario_dividends(script, date, revision) : std::vector<trading::Dividend>{};

  md::RecordingHeader header;
  header.provider = std::string(kDemoProvider);
  for (const auto& play : plays)
    for (const auto& event : play.events)
      if (event.type == "stall" || event.type == "halt") header.market_controls = true;
  // A run's feed polls at its slowest session's pace: an overnight session's minute.
  md::Timestamp poll = 0;
  for (const auto& window : windows) poll = std::max(poll, window.step);
  header.capabilities.poll_interval = std::chrono::seconds(poll / md::kNanosPerSecond);
  header.capabilities.open_interest = true;
  header.subscription.underlyings = script.symbols;
  header.started = windows.front().first;
  md::Timestamp now = windows.front().first;
  md::RecordingSink::Options sink_options;
  sink_options.clock = [&now] { return now; };
  sink_options.fixed_frames = revision >= 5;
  Discard discard;
  md::RecordingSink sink(path, header, discard, sink_options);

  SnapshotPublisher publisher;
  std::map<std::string, std::vector<Listed>> chains;
  // Strikes, the IV response and QQQ measure moves from the run's opening level; each
  // session starts where the one before ended, before its own gap.
  const double log_reference = std::log(kOpen);
  double base = log_reference;
  double base_level = kOpen;
  Random random(seed);
  Random small_caps(seed ^ 0x52555353454C4Cull);
  double rut_idio = 0;
  double idio = 0;   // QQQ's own wander
  double noise = 0;  // implied volatility's own wander
  double carried_iv = 0;  // IV events of earlier sessions, which last the run
  double log_level = base;
  std::map<std::string, double> paid;    // dividends each ETF has gone ex on so far, a share
  std::map<std::string, double> closes;  // each underlying's closing print on `closed`
  md::Date closed;
  std::uint64_t tick = 0;  // snapshots so far, across the run's sessions
  for (std::size_t number = 0; number < plays.size(); ++number) {
    const auto& play = plays[number];
    const auto& w = play.window;
    const bool regular = w.session == "regular";
    const bool overnight = w.session == "overnight";
    const bool first = number == 0;
    now = w.first;
    if (first || w.date != plays[number - 1].window.date) {
      // A run's later dates list their own series around the level they open at, and
      // keep every earlier one until its last trade.
      double log_center = base;
      for (const auto& event : play.events) if (event.type == "gap") log_center += event.move;
      Centers centers;
      if (!first) {
        centers = {std::exp(log_center), std::exp(log_center) / kSpyRatio,
                   kQqqOpen * std::exp(kQqqBeta * (log_center - log_reference) + idio),
                   kNdxOpen * std::exp(kQqqBeta * (log_center - log_reference) + idio),
                   kRutOpen * std::exp(kRutBeta * (log_center - log_reference) + rut_idio),
                   vix_level(std::exp(log_center) / kOpen, play.spot_vol, noise, play.iv_shift, carried_iv)};
      }
      Centers reference = centers;
      if (!first && play.previous_close) {
        const double opening = std::exp(base);
        reference = {opening, std::max(0.01, opening / kSpyRatio - paid["SPY"]),
            std::max(0.01, kQqqOpen * std::exp(kQqqBeta * (base - log_reference) + idio) - paid["QQQ"]),
            kNdxOpen * std::exp(kQqqBeta * (base - log_reference) + idio),
            kRutOpen * std::exp(kRutBeta * (base - log_reference) + rut_idio),
            vix_level(opening / kOpen, play.spot_vol, noise, play.iv_shift, carried_iv)};
      }
      for (auto& [contract, monthly, center] : listings(script, w.date, revision, centers)) {
        if (publisher.known(contract.osi_symbol())) continue;
        Listed listed;
        listed.id = publisher.define(contract.osi_symbol(), contract, sink);
        listed.contract = contract;
        listed.expiry = listed.contract.expiry_time();
        listed.last_trade = revision >= 3 ? listed.contract.last_trade_time() : listed.expiry;
        listed.ticks = ticks_for(contract.root);
        listed.monthly = monthly;
        listed.center = center;
        const double days = md::years_between(w.first, listed.expiry) * 365;
        publisher.open_interest(listed.id, w.first, open_interest(listed.contract, monthly, center, days, seed, listed.id), sink);
        chains[listed.contract.underlying].push_back(std::move(listed));
      }
      if (!first) {
        // Open interest moves on with the days to expiry; session volume starts again.
        for (auto& [underlying, chain] : chains) {
          for (auto& listed : chain) {
            listed.volume = 0;
            if (w.first >= listed.last_trade) continue;
            const double days = md::years_between(w.first, listed.expiry) * 365;
            publisher.open_interest(listed.id, w.first, open_interest(listed.contract, listed.monthly, listed.center, days, seed, listed.id), sink);
          }
        }
        // Each underlying's previous close, as the run printed it.
        for (const auto& symbol : script.symbols)
          if (const auto close = closes.find(symbol); close != closes.end())
            sink.publish(md::UnderlyingClose{symbol, w.first, closed, play.previous_close
                ? cents(reference.for_symbol(symbol) * std::exp(*play.previous_close)) : close->second});
      }
    }
    if (first) {
      Centers previous;
      previous.vix = vix_level(1, play.spot_vol, 0, play.iv_shift, 0);
      // Overnight cash indices keep their last close; options follow the latent level.
      if (overnight) {
        if (revision < 3) {
          sink.publish(md::UnderlyingQuote{"SPX", md::new_york_to_utc(md::previous_business_day(date), 16, 0), 0, 0, kOpen});
        } else {
          const auto day = md::previous_business_day(date);
          for (const auto& symbol : script.symbols)
            sink.publish(md::UnderlyingQuote{symbol, md::new_york_to_utc(day, md::regular_close_hour(day), 0),
                                             0, 0, cents(previous.for_symbol(symbol))});
        }
      }
      if (revision >= 2)
        for (const auto& symbol : script.symbols)
          sink.publish(md::UnderlyingClose{symbol, w.first, md::previous_business_day(date), cents(previous.for_symbol(symbol) * std::exp(play.previous_close.value_or(0)))});
    }
    // ETF prices drop by a dividend from its ex-date on.
    if (revision >= 4) {
      if (first || w.date != plays[number - 1].window.date)
        for (const auto& dividend : dividends)
          if (dividend.ex_date == w.date) paid[dividend.symbol] += dividend.per_share.dollars();
    } else {
      for (const auto& dividend : play.dividends) paid[dividend.symbol] += dividend.per_share;
    }

    const double session = static_cast<double>(w.close - w.first);
    // Each session spreads a trading day's variance at its volatility; a curb session
    // moves at the regular session's pace.
    const double hours = overnight ? 13.0 : 6.5;
    const double step_vol = play.volatility * std::sqrt(static_cast<double>(w.step) / (hours * 3600 * md::kNanosPerSecond) / 252);
    // The index wanders around its script and back (about a 35-minute half-life), so
    // every day keeps its shape whatever the draws.
    const double revert = std::exp(-static_cast<double>(w.step) / (50.0 * 60 * md::kNanosPerSecond));
    double deviation = 0;
    double close_level = base_level;
    double close_ndx = kNdxOpen, close_rut = kRutOpen, close_vix = kVixCenter;
    double event_iv = carried_iv;
    for (std::int64_t step = 0; w.first + step * w.step <= w.last; ++step, ++tick) {
      now = w.first + step * w.step;
      const bool after_close = regular && now > w.close;
      const double progress = std::min(1.0, static_cast<double>(now - w.first) / session);
      if (step > 0) {
        // Busier at the open and into the close; after the close only SPY and QQQ trade, quietly.
        const double pace = !regular ? 1.0
            : after_close ? 0.3 : 0.8 + 0.8 * std::exp(-progress / 0.06) + 0.5 * std::exp(-(1 - progress) / 0.08);
        deviation = revert * deviation + step_vol * pace * random.normal();
        idio = 0.995 * idio + 0.3 * step_vol * random.normal();
        noise = 0.98 * noise + 0.0008 * random.normal();
        if (chains.contains("RUT")) rut_idio = 0.995 * rut_idio + 0.4 * step_vol * small_caps.normal();
      }
      // The script: each segment's change spread evenly over its part of the session.
      double scripted = 0;
      double start = 0;
      double cumulative = 0;
      for (const auto& [until, move] : play.drift) {
        const double change = move - cumulative;
        scripted += change * std::clamp((progress - start) / (until - start), 0.0, 1.0);
        start = until;
        cumulative = move;
      }
      log_level = base + scripted + deviation;
      event_iv = carried_iv;
      for (const auto& event : play.events) {
        if (event.type == "gap") { log_level += event.move; continue; }
        const auto at = event_time(event, w);
        if (now < at) continue;
        if (event.type == "crush") event_iv += event.iv;
        if (event.type == "spike") {
          // A fast five-minute move; its IV jump is repriced across every expiry.
          const double ramp = std::clamp(static_cast<double>(now - at) / (5 * md::kNanosPerMinute), 0.0, 1.0);
          log_level += event.move * ramp;
          event_iv += event.iv;
        }
        if (event.type == "pin") {
          const double pull = std::clamp(static_cast<double>(now - at) / static_cast<double>(w.close - at), 0.0, 1.0);
          log_level += (std::log(event.strike) - log_level) * pull;
        }
      }
      std::set<std::string> stalled;
      md::Timestamp halted_until = 0;
      std::vector<const ScenarioEvent*> books;
      for (const auto& event : play.events) {
        if (event.minutes == 0) continue;
        const auto at = event_time(event, w);
        const auto end = event_time(event, w, true);
        if (now < at || now >= end) continue;
        if (event.type == "halt") {
          halted_until = std::max(halted_until, end);
          if (now == at) sink.publish(md::TradingHalt{now, end});
        } else if (event.type == "stall") {
          const auto& selected = event.symbols.empty() ? script.symbols : event.symbols;
          stalled.insert(selected.begin(), selected.end());
        } else if (event.type == "book") books.push_back(&event);
      }
      const auto frozen = [&](const std::string& symbol) { return halted_until > now || stalled.contains(symbol); };
      const double level = std::exp(log_level);
      if (!after_close) close_level = level;
      const double ratio = level / kOpen;
      double spy = level / kSpyRatio;
      double qqq = kQqqOpen * std::exp(kQqqBeta * (log_level - log_reference) + idio);
      if (const auto it = paid.find("SPY"); it != paid.end()) spy = std::max(0.01, spy - it->second);
      if (const auto it = paid.find("QQQ"); it != paid.end()) qqq = std::max(0.01, qqq - it->second);
      const double ndx = kNdxOpen * std::exp(kQqqBeta * (log_level - log_reference) + idio);
      const double rut = kRutOpen * std::exp(kRutBeta * (log_level - log_reference) + rut_idio);
      const double vix = vix_level(ratio, play.spot_vol, noise, play.iv_shift, event_iv);
      if (!after_close) { close_ndx = ndx; close_rut = rut; close_vix = vix; }
#ifdef OPENPORT_SANITIZER_TEST_SUPPORT
      // Still advance every random draw and latent price. Only the repeated
      // chain pricing/publication is sampled, so opens and closing levels keep
      // their original path. Authored event edges must also have live snapshots.
      if (!detail::keep_scenario_snapshot(now, w)) {
        const bool event_edge = std::any_of(play.events.begin(), play.events.end(), [&](const auto& event) {
          if (event.type == "gap") return false;
          const auto at = event_time(event, w), end = event_time(event, w, true);
          return std::abs(now - at) <= w.step || std::abs(now - end) <= w.step;
        });
        if (!event_edge) continue;
      }
#endif
      if (regular) {
        if (!after_close)
          for (const auto& [symbol, price] : std::array<std::pair<const char*, double>, 4>{{
              {"XSP", level / 10}, {"NDX", ndx}, {"RUT", rut}, {"VIX", vix}}})
            if (chains.contains(symbol) && !frozen(symbol)) sink.publish(md::UnderlyingQuote{symbol, now, 0, 0, cents(price)});
        // The index prints until the close; SPY and QQQ trade on after it.
        if (!after_close && chains.contains("SPX") && !frozen("SPX")) sink.publish(md::UnderlyingQuote{"SPX", now, 0, 0, cents(level)});
        for (const auto& [symbol, price] : {std::pair<const char*, double>{"SPY", spy}, {"QQQ", qqq}}) {
          if (!chains.contains(symbol) || frozen(symbol)) continue;
          const double bid = std::floor(price * 100) / 100;
          sink.publish(md::UnderlyingQuote{symbol, now, bid, cents(bid + 0.01), cents(price)});
        }
        // The closing prints: the index's last, and each ETF's first at or after the close.
        if (!after_close) {
          closes = {{"SPX", cents(level)}, {"SPY", cents(spy)}, {"QQQ", cents(qqq)}};
          if (revision >= 3) {
            closes["XSP"] = cents(level / 10);
            closes["NDX"] = cents(ndx);
            closes["RUT"] = cents(rut);
            closes["VIX"] = cents(vix);
          }
          closed = w.date;
        }
      }
      for (auto& [underlying, chain] : chains) {
        if (frozen(underlying)) {
          sink.publish(md::ProviderStatus{now, md::FeedState::Stale,
              halted_until > now ? "scenario market halt" : "scenario feed stall", underlying});
          sink.publish(md::SnapshotHeartbeat{underlying, now});
          continue;
        }
        const bool index = md::is_index_underlying(underlying);
        const bool trades = revision < 3 ? underlying == "SPX" : md::trading_session(underlying, now).open;
        if (!regular && !trades) {
          // Only products with this session in the calendar trade here.
          sink.publish(md::ProviderStatus{now, md::FeedState::Live, "demo " + underlying + ": options closed", underlying});
          continue;
        }
        // SPX options price off the index, which stops at the close (outside the regular
        // session they follow the latent level, as futures do).
        const double spx = regular ? close_level : level;
        const double price = underlying == "SPX" ? spx : underlying == "SPY" ? spy : underlying == "QQQ" ? qqq
            : underlying == "XSP" ? spx / 10 : underlying == "NDX" ? close_ndx
            : underlying == "RUT" ? (regular ? close_rut : rut) : (regular ? close_vix : vix);
        const double moved = underlying == "SPX" || underlying == "XSP" ? (regular ? close_level / kOpen : ratio)
            : underlying == "SPY" ? ratio : underlying == "QQQ" ? qqq / kQqqOpen
            : underlying == "NDX" ? price / kNdxOpen : underlying == "RUT" ? price / kRutOpen : 1;
        const double extra = underlying == "QQQ" || underlying == "NDX" ? 0.03 : underlying == "SPY" ? 0.005
            : underlying == "RUT" ? 0.04 : 0.0;
        const bool american = revision >= 4 && !index;
        std::vector<pricing::CashDividend> cash;
        if (american) {
          for (const auto& d : dividends) {
            if (d.symbol != underlying) continue;
            const double time = md::years_between(now, md::new_york_to_utc(d.ex_date, 0, 0));
            if (time > 0) cash.push_back({time, d.per_share.dollars()});
          }
        }
        std::set<md::InstrumentId> seen;
        for (auto& listed : chain) {
          const auto& c = listed.contract;
          if (now >= listed.last_trade) continue;
          const double years = md::years_between(now, listed.expiry);
          const double dividend = underlying == "NDX" ? 0.007 : underlying == "RUT" ? 0.012 : kDividend;
          double reserve = 0;
          for (const auto& d : cash) if (d.time < years) reserve += d.amount * std::exp(-kRate * d.time);
          const double forward = american ? std::max(0.01, price - reserve) * std::exp(kRate * years)
              : underlying == "VIX" ? kVixMean + (price - kVixMean) * std::exp(-4 * years)
              : price * std::exp((kRate - dividend) * years);
          const double base_atm = atm_vol(years * 365, moved, play.spot_vol, noise) + play.iv_shift + extra;
          // Short maturities react most; the far end retains 25% of an event's shock.
          const double atm = std::clamp(base_atm + event_iv * (0.25 + 0.75 * std::exp(-years * 365 / 7)), 0.02, 2.0);
          // VIX options use a mean-reverting forward and a rising call wing.
          const double vol = underlying == "VIX"
              ? std::clamp((0.8 + 0.4 * std::exp(-years * 12)) * (1 + 0.3 * std::log(c.strike / forward)), 0.3, 2.0)
              : smile(atm, forward, c.strike, years);
          double mid = pricing::black_price(c.type, forward, c.strike, years, vol,
                                                  std::exp(-kRate * years));
          if (american) {
            // Black plus a same-lattice LR premium cancels European tree error.
            // Thirteen odd steps keep snapshot generation inexpensive; only ITM puts
            // and calls ahead of cash payments can have material exercise value.
            // Extreme custom payments can exhaust spot; retain the capped escrow
            // price and intrinsic floor instead of passing an invalid tree input.
            if (reserve < price && ((c.type == pricing::OptionType::Put && c.strike > price) ||
                (c.type == pricing::OptionType::Call && reserve > 0)))
              mid += pricing::binomial_early_exercise_premium({c.type, price, c.strike, years, kRate, 0, vol}, 13, cash);
            const double intrinsic = c.type == pricing::OptionType::Call ? price - c.strike : c.strike - price;
            mid = std::max(mid, intrinsic);
          }
          // A morning delta spread's protective wing can have a sub-tick model
          // premium. Use at least the ATM move for liquidity: the cheaper call
          // smile must not squeeze out ordinary protective wings. Four moves
          // also cover the 5-delta region; remote tails can still lose their bid.
          // Gate only quote synthesis, preserving older revisions and stress books.
          const bool liquid_wing = revision >= 7 &&
              std::abs(std::log(c.strike / forward)) <= 4 * std::max(atm, vol) * std::sqrt(years);
          const auto q = quote(listed.ticks, mid, liquid_wing);
          if (q.bid != listed.last.bid || q.ask != listed.last.ask || listed.ask_size == 0) {
            listed.last = q;
            const double scale = index ? 120 : 800;
            listed.bid_size = q.bid > 0 ? std::floor((index ? 5 : 20) + scale * draw(seed, listed.id, 2 * tick)) : 0;
            listed.ask_size = std::floor((index ? 5 : 20) + scale * draw(seed, listed.id, 2 * tick + 1));
          }
          // Separate draws preserve every existing price, quote and size for this seed.
          if (step > 0 || revision >= 6) {
            const double distance = std::log(c.strike / price) / 0.01;
            const double activity = 20.0 * std::exp(-0.5 * distance * distance) / (1 + years * 365);
            const double pace = 0.8 + std::exp(-progress / 0.06) + std::exp(-(1 - progress) / 0.08);
            // The opening print includes a minute's equivalent of activity. It
            // uses the same liquidity curve, independent of snapshot spacing.
            const auto elapsed = step == 0 ? md::kNanosPerMinute : w.step;
            listed.volume += activity * pace * static_cast<double>(elapsed) / kStep *
                (0.5 + draw(seed ^ 0x564F4C554D45ull, listed.id, tick));
          }
          publisher.volume(listed.id, now, std::floor(listed.volume), sink);
          auto stressed = q;
          auto bid_size = listed.bid_size, ask_size = listed.ask_size;
          for (const auto* event : books) {
            if (!event->symbols.empty() && std::find(event->symbols.begin(), event->symbols.end(), underlying) == event->symbols.end()) continue;
            if (event->expiry && *event->expiry != c.expiry) continue;
            stressed = q;
            bid_size = std::max(1.0, listed.bid_size);
            ask_size = std::max(1.0, listed.ask_size);
            if (event->state == "crossed") stressed.bid = cents(q.ask + listed.ticks.above);
            else if (event->state == "locked") stressed.bid = q.ask;
            else if (event->state == "one_sided") { stressed.bid = 0; bid_size = 0; }
            else if (event->state == "zero_size") { stressed.bid = std::max(q.bid, listed.ticks.below); bid_size = ask_size = 0; }
            else if (event->state == "wide") {
              stressed.bid = cents(std::max(listed.ticks.below, q.bid - 10 * listed.ticks.above));
              stressed.ask = cents(q.ask + 10 * listed.ticks.above);
            }
          }
          publisher.quote(listed.id, now, stressed.bid, stressed.ask, bid_size, ask_size, sink);
          seen.insert(listed.id);
        }
        publisher.finish(underlying, seen, now, sink);
        sink.publish(md::ProviderStatus{now, md::FeedState::Live,
                                        "demo " + underlying + ": " + std::to_string(seen.size()) + " simulated options", underlying});
      }
    }
    carried_iv = event_iv;
    base = log_level;
    base_level = std::exp(log_level);
  }
  sink.close();
  if (const auto error = sink.error(); !error.empty()) throw std::runtime_error("Demo recording failed: " + error);
}

}  // namespace openport::providers
