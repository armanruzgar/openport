#include "openport/trading/events.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <sstream>
#include <tuple>

namespace openport::trading {
namespace {
void invalid(const std::string& message) { throw TradingError(Reason::INVALID_RULES, message); }
bool symbol_ok(const std::string& s) {
  return !s.empty() && s.size() <= 12 && std::all_of(s.begin(), s.end(), [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.';
  });
}
bool has(const AccountRules& r, const std::string& kind) {
  return std::find(r.hold_restrictions.begin(), r.hold_restrictions.end(), kind) != r.hold_restrictions.end();
}
md::Date event_date(const PlanEvent& e) { return md::date_from_days(event_time(e) / md::kNanosPerDay); }
Timestamp at(md::Date day, std::int64_t minute) {
  // day_end may be 24:00, which is midnight starting the next day.
  if (minute >= 24 * 60) { day = md::date_from_days(md::days_since_epoch(day) + minute / (24 * 60)); minute %= 24 * 60; }
  return md::new_york_to_utc(day, static_cast<int>(minute / 60), static_cast<int>(minute % 60));
}
// Canonical UTC, retaining all nanoseconds (the ordinary display formatter uses milliseconds).
std::string exact_time(Timestamp t) {
  auto value = md::format_timestamp(t);
  const auto fraction = std::to_string(md::kNanosPerSecond + t % md::kNanosPerSecond).substr(1);
  return value.substr(0, 19) + "." + fraction + "Z";
}
}  // namespace
Timestamp event_time(const PlanEvent& e) {
  return md::parse_datetime(e.kind == "news" ? e.time : e.time + "T00:00:00Z", md::Zone::Utc).value_or(0);
}
void validate_event_rules(const AccountRules& r) {
  if (r.events.size() > 256) invalid("events must contain at most 256 entries");
  if (r.news_before_minutes < 0 || r.news_before_minutes > 240 || r.news_after_minutes < 0 || r.news_after_minutes > 240)
    invalid("news_before_minutes and news_after_minutes must be integers from 0 to 240");
  if (r.news_action != "block" && r.news_action != "flatten") invalid("news_action must be block or flatten");
  for (const auto& kind : r.hold_restrictions)
    if (kind != "weekend" && kind != "earnings" && kind != "ex_dividend" && kind != "split")
      invalid("hold_restrictions accepts weekend, earnings, ex_dividend and split");
  if (r.hold_cutoff < 0 || r.hold_cutoff >= r.day_end_minutes)
    invalid("hold_cutoff must be HH:MM New York time before day_end");
  for (const auto& e : r.events) {
    if (e.kind != "news" && e.kind != "earnings" && e.kind != "ex_dividend" && e.kind != "split")
      invalid("event kind must be news, earnings, ex_dividend or split");
    if (std::count_if(e.label.begin(), e.label.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; }) > 64) invalid("event label must be at most 64 characters");
    if ((!e.symbol.empty() || e.kind != "news") && !symbol_ok(e.symbol))
      invalid("event symbol must be 1-12 uppercase letters, digits or dots; only news may omit it");
    if (e.kind == "earnings") {
      if (!e.session.empty() && e.session != "before_open" && e.session != "after_close")
        invalid("earnings session must be before_open or after_close");
    } else if (!e.session.empty()) invalid("event session is allowed only for earnings");
    const auto time = event_time(e);
    if (e.kind == "news") {
      if (e.time.size() < 20 || e.time.back() != 'Z' || e.time[10] != 'T' || time <= 0 ||
          time > std::numeric_limits<Timestamp>::max() - 240 * md::kNanosPerMinute || time < 240 * md::kNanosPerMinute)
        invalid("news time must be an exact ISO-8601 UTC timestamp ending in Z within the supported timestamp range");
    } else if (e.time.size() != 10 || time <= 0 || md::format_date(event_date(e)) != e.time)
      invalid("corporate event time must be a valid YYYY-MM-DD date after 1970-01-01");
  }
}
void normalize_event_rules(AccountRules& r, bool capture_calendar) {
  validate_event_rules(r);
  for (auto& e : r.events) {
    if (e.kind == "earnings" && e.session.empty()) e.session = "before_open";
    if (e.kind == "news") e.time = exact_time(event_time(e));
  }
  std::sort(r.events.begin(), r.events.end(), [](const auto& a, const auto& b) {
    return std::tie(a.time, a.kind, a.symbol, a.session, a.label) < std::tie(b.time, b.kind, b.symbol, b.session, b.label);
  });
  r.events.erase(std::unique(r.events.begin(), r.events.end()), r.events.end());
  std::sort(r.hold_restrictions.begin(), r.hold_restrictions.end());
  r.hold_restrictions.erase(std::unique(r.hold_restrictions.begin(), r.hold_restrictions.end()), r.hold_restrictions.end());
  if (capture_calendar && !r.hold_restrictions.empty() && !r.hold_calendar) r.hold_calendar = md::scheduled_days();
}
std::vector<PlanEvent> parse_event_calendar(std::istream& input) {
  AccountRules rules;
  std::string line;
  const auto trim = [](std::string s) {
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
    return s;
  };
  for (int n = 1; std::getline(input, line); ++n) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line == "kind,time,symbol,session,label") continue;
    try {
      std::array<std::string, 5> fields;
      std::size_t begin = 0;
      for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto end = line.find(',', begin);
        if ((i < 4 && end == std::string::npos) || (i == 4 && end != std::string::npos))
          invalid("expected kind,time,symbol,session,label (five columns)");
        fields[i] = trim(line.substr(begin, end == std::string::npos ? end : end - begin));
        begin = end == std::string::npos ? line.size() : end + 1;
      }
      rules.events.push_back({fields[0], fields[1], fields[2], fields[3], fields[4]});
      validate_event_rules(rules);
    } catch (const std::exception& error) {
      throw std::invalid_argument("event calendar line " + std::to_string(n) + ": " + error.what());
    }
  }
  normalize_event_rules(rules);
  return rules.events;
}
std::vector<EventWindow> event_windows(const AccountRules& r, Timestamp from, Timestamp to) {
  std::vector<EventWindow> out;
  if (to <= 0 || from > to) return out;
  // A frozen empty calendar is distinct from the ambient live schedule.
  md::ScheduledDaysScope calendar(r.hold_calendar.value_or(md::scheduled_days()));
  const auto append = [&](EventWindow w) { if (w.start <= to && w.end > from) out.push_back(std::move(w)); };
  const auto holding = [&](const std::string& kind, const std::string& symbol,
                           const std::string& label, md::Date day) {
    append({kind + ":" + md::format_date(day) + ":" + symbol, kind, symbol, label, at(day, r.hold_cutoff), at(day, r.day_end_minutes)});
  };
  for (std::size_t i = 0; i < r.events.size(); ++i) {
    const auto& e = r.events[i];
    const auto key = e.kind + ":" + std::to_string(i);
    if (e.kind == "news") {
      if (r.news_before_minutes || r.news_after_minutes)
        append({key, e.kind, e.symbol, e.label, event_time(e) - r.news_before_minutes * md::kNanosPerMinute,
                event_time(e) + r.news_after_minutes * md::kNanosPerMinute});
    } else if (has(r, e.kind)) {
      const auto date = event_date(e);
      const auto day = e.kind == "earnings" && e.session == "after_close"
          ? date : md::previous_business_day(date);
      holding(e.kind, e.symbol, e.label, day);
    }
  }
  if (has(r, "weekend")) {
    const auto first = md::days_since_epoch(md::new_york_time(from).date) - 7;
    const auto last = md::days_since_epoch(md::new_york_time(to).date) + 7;
    // Each Saturday names the last business date before it, including Friday holidays.
    for (auto d = first; d <= last; ++d) {
      if ((d + 4) % 7 != 6) continue;
      const auto day = md::previous_business_day(md::date_from_days(d));
      holding("weekend", "", "Weekend", day);
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return std::tie(a.start, a.key, a.label) < std::tie(b.start, b.key, b.label); });
  out.erase(std::unique(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.key == b.key; }), out.end());
  return out;
}
Decision event_decision(const AccountRules& r, const EventWindow& w, Timestamp time) {
  const auto scope = w.symbol.empty() ? "account" : w.symbol;
  if (w.kind == "news")
    return {Reason::NEWS_BLACKOUT, "News blackout" + (w.label.empty() ? std::string{} : ": " + w.label) +
        " until " + md::format_timestamp(w.end) + "; closing orders still work",
        static_cast<double>(time), static_cast<double>(w.end), scope};
  return {Reason::HOLD_RESTRICTED, "Holding restriction: " + w.kind + "; close by the plan cutoff; openings resume after day_end",
      {}, static_cast<double>(r.hold_cutoff), w.kind + ":" + scope};
}
Decision event_entry_check(const AccountRules& r, std::string_view underlying, Timestamp time) {
  if (r.events.empty() && r.hold_restrictions.empty()) return {};
  for (const auto& w : event_windows(r, time, time))
    if (w.symbol.empty() || w.symbol == underlying) return event_decision(r, w, time);
  return {};
}
}  // namespace openport::trading
