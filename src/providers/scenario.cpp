#include "openport/providers/scenario.hpp"

#include <algorithm>
#include <map>
#include <cmath>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <tuple>

namespace openport::providers {
namespace {
using nlohmann::json;
void require(bool condition, const std::string& field, const std::string& message) {
  if (!condition) throw std::invalid_argument(field + ": " + message);
}
void keys(const json& value, std::initializer_list<std::string_view> allowed, const std::string& field) {
  require(value.is_object(), field, "expected an object");
  for (auto it = value.begin(); it != value.end(); ++it)
    require(std::find(allowed.begin(), allowed.end(), it.key()) != allowed.end(), field + "." + it.key(), "unknown field");
}
std::string text(const json& value, const std::string& field, std::size_t limit = 500) {
  require(value.is_string(), field, "expected text");
  auto out = value.get<std::string>();
  require(!out.empty() && out.size() <= limit && out.find_first_of("\n\r") == std::string::npos, field, "expected one nonempty line");
  return out;
}
double number(const json& value, const std::string& field, double low, double high) {
  require(value.is_number(), field, "expected a number");
  const auto n = value.get<double>();
  require(std::isfinite(n) && n >= low && n <= high, field, "outside [" + std::to_string(low) + ", " + std::to_string(high) + "]");
  return n;
}
std::vector<std::filesystem::path> files(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> out;
  if (directory.empty()) return out;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() == ".json") out.push_back(entry.path());
  std::sort(out.begin(), out.end());
  return out;
}
}  // namespace

md::Timestamp scenario_time(std::string_view time, md::Date date, bool overnight) {
  require(time.size() == 5 && time[2] == ':' && time[0] >= '0' && time[0] <= '2' &&
          time[1] >= '0' && time[1] <= '9' && time[3] >= '0' && time[3] <= '5' && time[4] >= '0' && time[4] <= '9',
          "time", "expected HH:MM New York time");
  const int hour = (time[0] - '0') * 10 + time[1] - '0';
  const int minute = (time[3] - '0') * 10 + time[4] - '0';
  require(hour < 24, "time", "hour must be below 24");
  if (overnight && hour * 60 + minute >= 20 * 60 + 15) date = md::date_from_days(md::days_since_epoch(date) - 1);
  const auto result = md::new_york_to_utc(date, hour, minute);
  require(result != md::kInvalidTimestamp, "time", "invalid date or DST time");
  return result;
}
md::Timestamp scenario_open(const Scenario& scenario, md::Date date) {
  return scenario_time(scenario.overnight ? "20:15" : "09:30", date, scenario.overnight);
}
md::Timestamp scenario_close(const Scenario& scenario, md::Date date) {
  return scenario.overnight ? scenario_time("09:25", date, true) : md::new_york_to_utc(date, md::regular_close_hour(date), 0);
}

md::Timestamp scenario_event_time(std::string_view time, const ScenarioWindow& window, int minutes) {
  auto at = scenario_time(time, window.date, window.session == "overnight");
  at += static_cast<md::Timestamp>(minutes) * md::kNanosPerMinute;
  if (window.session == "regular") {
    const auto offset = (at - window.first) / md::kNanosPerMinute;
    require(offset >= 0 && offset <= 390 && (minutes > 0 || offset < 390),
            "at", std::string(time) + ": must be inside 09:30–16:00 ET");
    const auto duration = (window.close - window.first) / md::kNanosPerMinute;
    const auto scaled = window.first + (offset * duration / 390) * md::kNanosPerMinute;
    return minutes > 0 ? std::max(scaled, scenario_event_time(time, window) + md::kNanosPerMinute) : scaled;
  }
  const auto opens = window.session == "curb" ? window.first - window.step : window.first;
  require(at >= opens && (minutes > 0 ? at <= window.close : at < window.close),
          "at", std::string(time) + ": must be inside the session before its close");
  return at;
}

namespace {
/// A session's snapshots on its trading date: overnight from 20:15 the evening
/// before to 09:25 each minute; regular from 09:30 to the 16:15 last trade (13:15 on
/// an early close) every 15 seconds; and curb after the regular session's last
/// snapshot until before 17:00, when the trading date turns, every 15 seconds.
ScenarioWindow session_window(const std::string& session, md::Date date) {
  ScenarioWindow w{session, date};
  constexpr md::Timestamp quarter = 15 * md::kNanosPerSecond;
  if (session == "overnight") {
    const auto evening = md::date_from_days(md::days_since_epoch(date) - 1);
    w.first = md::new_york_to_utc(evening, 20, 15);
    w.close = w.last = md::new_york_to_utc(date, 9, 25);
    w.step = 60 * md::kNanosPerSecond;
  } else if (session == "curb") {
    w.first = md::new_york_to_utc(date, 16, 15) + quarter;
    w.close = w.last = md::new_york_to_utc(date, 17, 0) - quarter;
    w.step = quarter;
  } else {
    const int hour = md::regular_close_hour(date);
    w.first = md::new_york_to_utc(date, 9, 30);
    w.close = md::new_york_to_utc(date, hour, 0);
    w.last = md::new_york_to_utc(date, hour, 15);
    w.step = quarter;
  }
  return w;
}
std::vector<std::pair<double, double>> drift_of(const json& value, const std::string& field) {
  require(value.is_array() && !value.empty() && value.size() <= 100, field, "expected 1 to 100 waypoints");
  std::vector<std::pair<double, double>> drift;
  double previous = 0;
  for (const auto& point : value) {
    require(point.is_array() && point.size() == 2, field, "expected [fraction, cumulative log return]");
    const double until = number(point[0], field + ".fraction", 0, 1);
    require(until > previous, field + ".fraction", "must be strictly increasing, starting above zero");
    drift.emplace_back(until, number(point[1], field + ".move", -0.2, 0.2));
    previous = until;
  }
  require(previous == 1, field, "last fraction must be 1");
  return drift;
}
std::vector<ScenarioEvent> events_of(const json& value, const std::string& parent, const ScenarioWindow& window,
                                     const std::vector<std::string>& symbols) {
  require(value.is_array() && value.size() <= 32, parent, "expected at most 32 events");
  std::vector<ScenarioEvent> events;
  std::set<std::string> unique;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto& e = value[index];
    auto field = parent + "[" + std::to_string(index) + "]";
    require(e.is_object() && e.contains("type"), field + ".type", "required field is missing");
    ScenarioEvent event;
    event.type = text(e.at("type"), field + ".type");
    field += " (" + event.type + ")";
    if (event.type == "gap") keys(e, {"type", "move"}, field);
    else if (event.type == "crush") keys(e, {"type", "at", "iv"}, field);
    else if (event.type == "spike") keys(e, {"type", "at", "move", "iv"}, field);
    else if (event.type == "pin") keys(e, {"type", "at", "strike"}, field);
    else if (event.type == "book") keys(e, {"type", "at", "minutes", "state", "symbols", "expiry"}, field);
    else if (event.type == "stall") keys(e, {"type", "at", "minutes", "symbols"}, field);
    else if (event.type == "halt") keys(e, {"type", "at", "minutes"}, field);
    else require(false, field + ".type", "expected gap, crush, spike, pin, book, stall or halt");
    if (event.type != "gap") {
      require(e.contains("at"), field + ".at", "required field is missing");
      event.at = text(e.at("at"), field + ".at");
      try { (void)scenario_event_time(event.at, window); }
      catch (const std::invalid_argument& error) { throw std::invalid_argument(field + ".at " + event.at + ": " + error.what()); }
    }
    require(unique.insert(event.type + event.at).second, field, "duplicate event type and time");
    if (event.type == "gap" || event.type == "spike") {
      require(e.contains("move"), field + ".move", "required field is missing");
      event.move = number(e.at("move"), field + ".move", -0.3, 0.3);
    }
    if (event.type == "crush" || event.type == "spike") {
      require(e.contains("iv"), field + ".iv", "required field is missing");
      event.iv = number(e.at("iv"), field + ".iv", event.type == "crush" ? -0.3 : 0, event.type == "crush" ? -0.0001 : 0.3);
    }
    if (event.type == "pin") {
      require(window.session == "regular", field + ".pin", "pin requires a regular session");
      require(e.contains("strike"), field + ".strike", "required field is missing");
      event.strike = number(e.at("strike"), field + ".strike", 5400, 6600);
    }
    if (event.type == "book" || event.type == "stall" || event.type == "halt") {
      require(e.contains("minutes") && e.at("minutes").is_number_integer(), field + ".minutes", "expected integer minutes");
      event.minutes = static_cast<int>(number(e.at("minutes"), field + ".minutes", 1, 790));
      try { (void)scenario_event_time(event.at, window, event.minutes); }
      catch (const std::invalid_argument& error) { throw std::invalid_argument(field + ".minutes: " + error.what()); }
      if (e.contains("symbols")) {
        require(e.at("symbols").is_array() && !e.at("symbols").empty(), field + ".symbols", "expected nonempty subset of scenario symbols");
        std::set<std::string> selected;
        for (const auto& item : e.at("symbols")) {
          const auto symbol = text(item, field + ".symbols");
          require(std::find(symbols.begin(), symbols.end(), symbol) != symbols.end() && selected.insert(symbol).second,
                  field + ".symbols", "expected unique scenario symbols");
          event.symbols.push_back(symbol);
        }
      }
      if (event.type == "halt") require(window.session == "regular", field, "halt requires a regular session");
      if (event.type == "book") {
        require(e.contains("state"), field + ".state", "required field is missing");
        event.state = text(e.at("state"), field + ".state");
        require(event.state == "crossed" || event.state == "locked" || event.state == "one_sided" ||
                event.state == "zero_size" || event.state == "wide", field + ".state", "unknown book state");
        if (e.contains("expiry")) {
          const auto expiry = text(e.at("expiry"), field + ".expiry");
          const auto parsed = md::parse_datetime(expiry + "T12:00:00", md::Zone::NewYork);
          require(expiry.size() == 10 && parsed.has_value(), field + ".expiry", "expected YYYY-MM-DD");
          event.expiry = md::new_york_time(*parsed).date;
        }
      }
    }
    events.push_back(event);
  }
  return events;
}
}  // namespace

std::vector<ScenarioWindow> scenario_windows(const Scenario& scenario, md::Date date) {
  if (scenario.sessions.empty()) return {session_window(scenario.overnight ? "overnight" : "regular", date)};
  std::vector<ScenarioWindow> windows;
  for (std::size_t i = 0; i < scenario.sessions.size(); ++i) {
    const auto& session = scenario.sessions[i].session;
    const auto field = "sessions[" + std::to_string(i) + "].session";
    auto day = date;
    if (windows.empty()) {
      require(session != "curb", field, "the first session must be regular or overnight");
    } else {
      const auto& prior = windows.back();
      if (prior.session == "overnight") {
        require(session == "regular", field, "an overnight session leads into its date's regular session");
        day = prior.date;
      } else if (session == "curb") {
        require(prior.session == "regular", field, "a curb session follows its date's regular session");
        day = prior.date;
      } else {
        // After 17:00 the trading date is the next business day.
        day = md::trading_date(md::new_york_to_utc(prior.date, 18, 0));
      }
    }
    require(session != "curb" || md::regular_close_hour(day) == 16, field,
            "an early close on " + md::format_date(day) + " has no curb session");
    windows.push_back(session_window(session, day));
  }
  return windows;
}
md::Timestamp scenario_end(const Scenario& scenario, md::Date date) { return scenario_windows(scenario, date).back().last; }

Scenario read_scenario(const std::filesystem::path& file) {
  std::string source;
  try {
    require(std::filesystem::file_size(file) <= 64 * 1024, "$", "file exceeds 64 KiB");
    std::ifstream input(file);
    require(input.good(), "$", "cannot open file");
    source.assign(std::istreambuf_iterator<char>{input}, {});
    require(!input.bad(), "$", "cannot read file");
  } catch (const std::exception& error) {
    throw std::invalid_argument(file.string() + ": " + error.what());
  }
  return parse_scenario(source, file);
}

Scenario parse_scenario(std::string_view source, const std::filesystem::path& file) {
  try {
    require(source.size() <= 64 * 1024, "$", "file exceeds 64 KiB");
    const auto j = json::parse(source);
    keys(j, {"id", "title", "description", "goal", "symbols", "session", "date", "seed", "generator", "drift", "volatility", "iv_shift", "spot_vol", "events", "sessions", "previous_close", "strike_window"}, "$");
    const bool several = j.contains("sessions");
    for (const auto* field : {"id", "title", "description", "symbols", "date", "seed", "generator"})
      require(j.contains(field), field, "required field is missing");
    if (several) {
      for (const auto* field : {"session", "drift", "events", "previous_close"})
        require(!j.contains(field), field, "give it in each of the sessions instead");
    } else {
      for (const auto* field : {"session", "drift", "volatility", "iv_shift", "spot_vol"})
        require(j.contains(field), field, "required field is missing");
    }
    Scenario s;
    s.source = source;
    s.source_file = std::filesystem::absolute(file);
    if (j.contains("previous_close")) s.previous_close = number(j.at("previous_close"), "previous_close", -0.3, 0.3);
    if (j.contains("strike_window")) s.strike_window = number(j.at("strike_window"), "strike_window", 0.03, 0.5);
    s.id = text(j.at("id"), "id", 40);
    require(s.id.front() != '-' && s.id.back() != '-' && std::all_of(s.id.begin(), s.id.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    }), "id", "use lowercase letters, digits and interior hyphens");
    s.title = text(j.at("title"), "title", 100);
    s.description = text(j.at("description"), "description");
    if (j.contains("goal")) s.goal = text(j.at("goal"), "goal");
    if (several) {
      const auto& sessions = j.at("sessions");
      require(sessions.is_array() && !sessions.empty() && sessions.size() <= 24, "sessions", "expected 1 to 24 sessions");
      for (std::size_t i = 0; i < sessions.size(); ++i) {
        const auto field = "sessions[" + std::to_string(i) + "]";
        const auto& value = sessions[i];
        keys(value, {"session", "drift", "volatility", "iv_shift", "spot_vol", "events", "dividends", "previous_close"}, field);
        ScenarioSession session;
        require(value.contains("session"), field + ".session", "required field is missing");
        session.session = text(value.at("session"), field + ".session");
        require(session.session == "regular" || session.session == "curb" || session.session == "overnight",
                field + ".session", "expected regular, curb or overnight");
        require(value.contains("drift"), field + ".drift", "required field is missing");
        session.drift = drift_of(value.at("drift"), field + ".drift");
        // The file's volatility, iv_shift and spot_vol apply to each session that omits its own.
        const auto setting = [&](const char* name, double low, double high) {
          require(value.contains(name) || j.contains(name), field + "." + name, "required field is missing");
          return value.contains(name) ? number(value.at(name), field + "." + name, low, high) : number(j.at(name), name, low, high);
        };
        session.volatility = setting("volatility", 0, 1);
        session.iv_shift = setting("iv_shift", -0.1, 1);
        session.spot_vol = setting("spot_vol", -10, 0);
        s.sessions.push_back(std::move(session));
      }
      for (const auto& [name, low, high] : {std::tuple{"volatility", 0.0, 1.0}, std::tuple{"iv_shift", -0.1, 1.0}, std::tuple{"spot_vol", -10.0, 0.0}})
        if (j.contains(name)) (void)number(j.at(name), name, low, high);
      s.overnight = s.sessions.front().session == "overnight";
    } else {
      const auto session = text(j.at("session"), "session");
      require(session == "regular" || session == "overnight", "session", "expected regular or overnight");
      s.overnight = session == "overnight";
    }
    require(j.at("symbols").is_array() && !j.at("symbols").empty(), "symbols", "expected a nonempty array");
    std::set<std::string> seen;
    for (const auto& value : j.at("symbols")) {
      const auto symbol = text(value, "symbols");
      require(symbol == "SPX" || symbol == "SPY" || symbol == "QQQ" || symbol == "XSP" ||
              symbol == "NDX" || symbol == "RUT" || symbol == "VIX", "symbols", "supported: SPX, SPY, QQQ, XSP, NDX, RUT, VIX");
      require(seen.insert(symbol).second, "symbols", "duplicate symbol");
      s.symbols.push_back(symbol);
    }
    const auto date = text(j.at("date"), "date");
    const auto parsed = md::parse_datetime(date + "T12:00:00", md::Zone::NewYork);
    require(date.size() == 10 && parsed.has_value(), "date", "expected YYYY-MM-DD");
    s.date = md::new_york_time(*parsed).date;
    require(md::trading_date(*parsed) == s.date, "date", "must be a trading day");
    for (const auto& window : scenario_windows(s, s.date)) {
      if (window.session == "regular") continue;
      const auto trades = [&](const auto& symbol) { return md::trading_session(symbol, window.first).open; };
      require(std::any_of(s.symbols.begin(), s.symbols.end(), trades), "symbols",
              "curb and overnight sessions need SPX, XSP, RUT or VIX");
      require(several || std::all_of(s.symbols.begin(), s.symbols.end(), trades), "symbols",
              "single overnight sessions support SPX, XSP, RUT and VIX only");
    }
    require(j.at("seed").is_number_unsigned() || (j.at("seed").is_number_integer() && j.at("seed").get<std::int64_t>() >= 0), "seed", "expected uint64");
    s.seed = j.at("seed").get<std::uint64_t>();
    require(j.at("generator").is_number_integer() && j.at("generator") == 1, "generator", "only version 1 is supported");
    if (several) {
      const auto windows = scenario_windows(s, s.date);
      for (std::size_t i = 0; i < s.sessions.size(); ++i) {
        const auto field = "sessions[" + std::to_string(i) + "]";
        const auto& value = j.at("sessions")[i];
        auto& session = s.sessions[i];
        if (value.contains("events")) session.events = events_of(value.at("events"), field + ".events", windows[i], s.symbols);
        if (value.contains("previous_close")) {
          require(i == 0 || windows[i].date != windows[i - 1].date, field + ".previous_close", "only the first session of a trading date may set its reference");
          session.previous_close = number(value.at("previous_close"), field + ".previous_close", -0.3, 0.3);
        }
        if (value.contains("dividends")) {
          const auto& dividends = value.at("dividends");
          require(dividends.is_array() && dividends.size() <= 2, field + ".dividends", "expected at most 2 dividends");
          // The account opens on the run's first date, so nothing is held into it.
          require(dividends.empty() || windows[i].date != windows.front().date, field + ".dividends",
                  "the run's first date cannot go ex-dividend");
          std::set<std::string> paying;
          for (const auto& entry : dividends) {
            keys(entry, {"symbol", "per_share"}, field + ".dividends");
            require(entry.contains("symbol") && entry.contains("per_share"), field + ".dividends", "symbol and per_share are required");
            ScenarioDividend dividend;
            dividend.symbol = text(entry.at("symbol"), field + ".dividends.symbol");
            require((dividend.symbol == "SPY" || dividend.symbol == "QQQ") && seen.contains(dividend.symbol),
                    field + ".dividends.symbol", "expected SPY or QQQ, among the symbols");
            require(paying.insert(dividend.symbol).second, field + ".dividends.symbol", "duplicate symbol");
            dividend.per_share = number(entry.at("per_share"), field + ".dividends.per_share", 0, 100);
            require(dividend.per_share > 0, field + ".dividends.per_share", "must be positive");
            session.dividends.push_back(std::move(dividend));
          }
        }
        // One ex-date a symbol: an overnight session and the regular one after it share their date.
        for (std::size_t k = 0; k < i; ++k) {
          if (windows[k].date != windows[i].date) continue;
          for (const auto& earlier : s.sessions[k].dividends)
            for (const auto& later : session.dividends)
              require(earlier.symbol != later.symbol, field + ".dividends.symbol", "goes ex once a date");
        }
      }
      const auto& first = s.sessions.front();
      s.drift = first.drift;
      s.volatility = first.volatility;
      s.iv_shift = first.iv_shift;
      s.spot_vol = first.spot_vol;
      s.events = first.events;
      s.previous_close = first.previous_close;
      return s;
    }
    s.drift = drift_of(j.at("drift"), "drift");
    s.volatility = number(j.at("volatility"), "volatility", 0, 1);
    s.iv_shift = number(j.at("iv_shift"), "iv_shift", -0.1, 1);
    s.spot_vol = number(j.at("spot_vol"), "spot_vol", -10, 0);
    if (j.contains("events")) s.events = events_of(j.at("events"), "events", scenario_windows(s, s.date).front(), s.symbols);
    return s;
  } catch (const std::exception& error) {
    throw std::invalid_argument(file.string() + ": " + error.what());
  }
}

std::vector<Scenario> load_scenarios(const std::filesystem::path& user,
                                    const std::function<void(const std::string&)>& log) {
  auto out = builtin_scenarios();
  std::map<std::string, std::filesystem::path> loaded;  // user files by scenario id
  try {
    for (const auto& file : files(user)) {
      try {
        auto s = read_scenario(file);
        // A user file may replace a built-in; two user files with one id is a mistake.
        if (const auto [it, added] = loaded.try_emplace(s.id, file); !added) {
          if (log) log(file.string() + ": id " + s.id + " is also used by " + it->second.filename().string() + ", which it replaces");
          it->second = file;
        }
        out.erase(std::remove_if(out.begin(), out.end(), [&](const auto& prior) { return prior.id == s.id; }), out.end());
        out.push_back(std::move(s));
      } catch (const std::exception& error) { if (log) log(error.what()); }
    }
  } catch (const std::exception& error) { if (log) log(user.string() + ": " + error.what()); }
  return out;
}
}  // namespace openport::providers
