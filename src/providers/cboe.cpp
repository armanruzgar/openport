#include "openport/providers/cboe.hpp"

// simdjson 4.6 names std::ranges::input_range under C++20 without including
// <ranges>; libc++ does not pull it in transitively.
// clang-format off
#include <ranges>
#include <simdjson.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "openport/md/contract.hpp"
#include "openport/net/http.hpp"

namespace openport::providers {
namespace {

/// Reads a numeric field that Cboe sometimes sends as null or omits.
double number_or_zero(simdjson::ondemand::object& object, std::string_view key) {
  double value = 0.0;
  if (object[key].get_double().get(value) != simdjson::SUCCESS || !std::isfinite(value)) return 0.0;
  return value;
}

std::string text_or_empty(simdjson::ondemand::object& object, std::string_view key) {
  std::string_view value;
  if (object[key].get_string().get(value) != simdjson::SUCCESS) return {};
  return std::string(value);
}

/// A number Cboe may send as a JSON number or as a string ("7770.810000").
double loose_number(simdjson::ondemand::object& object, std::string_view key) {
  simdjson::ondemand::value value;
  if (object[key].get(value) != simdjson::SUCCESS) return 0.0;
  simdjson::ondemand::json_type type;
  if (value.type().get(type) != simdjson::SUCCESS) return 0.0;
  double number = 0.0;
  const auto status = type == simdjson::ondemand::json_type::string
                          ? value.get_double_in_string().get(number)
                          : value.get_double().get(number);
  return status == simdjson::SUCCESS && std::isfinite(number) ? number : 0.0;
}

/// Oldest first, one bar per start; a later row for the same start wins.
std::vector<md::Bar> ordered(std::vector<md::Bar> bars) {
  std::stable_sort(bars.begin(), bars.end(),
                   [](const md::Bar& a, const md::Bar& b) { return a.start < b.start; });
  std::vector<md::Bar> out;
  out.reserve(bars.size());
  for (const auto& bar : bars) {
    if (!out.empty() && out.back().start == bar.start)
      out.back() = bar;
    else
      out.push_back(bar);
  }
  return out;
}

simdjson::ondemand::array chart_rows(simdjson::ondemand::document& doc, const char* what) {
  simdjson::ondemand::array rows;
  if (doc["data"].get_array().get(rows) != simdjson::SUCCESS)
    throw std::runtime_error(std::string("Cboe ") + what + " chart: missing data array");
  return rows;
}

}  // namespace

std::string cboe_chart_url(std::string_view underlying, CboeChart chart) {
  std::string url = "https://cdn-api.cboe.com/api/global/delayed_quotes/charts/";
  url += chart == CboeChart::Intraday ? "intraday/" : "historical/";
  if (md::is_index_underlying(underlying)) url += '_';
  url += underlying;
  url += ".json";
  return url;
}

std::vector<md::Bar> parse_cboe_intraday(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);
  std::vector<md::Bar> bars;
  for (simdjson::ondemand::object row : chart_rows(doc, "intraday")) {
    const auto closes = md::parse_datetime(text_or_empty(row, "datetime"), md::Zone::NewYork);
    simdjson::ondemand::object prices;
    if (!closes || row["price"].get_object().get(prices) != simdjson::SUCCESS) continue;
    const md::Bar bar{*closes - md::kNanosPerMinute, loose_number(prices, "open"),
                      loose_number(prices, "high"), loose_number(prices, "low"),
                      loose_number(prices, "close")};
    if (md::valid_bar(bar)) bars.push_back(bar);
  }
  return ordered(std::move(bars));
}

std::vector<md::Bar> parse_cboe_daily(std::string_view json) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);
  std::vector<md::Bar> bars;
  for (simdjson::ondemand::object row : chart_rows(doc, "daily")) {
    const std::string text = text_or_empty(row, "date");
    md::Date day;
    char tail = 0;
    if (std::sscanf(text.c_str(), "%4d-%2d-%2d%c", &day.year, &day.month, &day.day, &tail) != 3 ||
        !md::valid_date(day))
      continue;
    const md::Timestamp open = md::new_york_to_utc(day, 9, 30);
    const md::Bar bar{open, loose_number(row, "open"), loose_number(row, "high"),
                      loose_number(row, "low"), loose_number(row, "close")};
    if (open != md::kInvalidTimestamp && std::isfinite(bar.close) && bar.close > 0) bars.push_back(bar);
  }
  return ordered(std::move(bars));
}

CboeChartHistory::CboeChartHistory(std::vector<std::string> underlyings, Sink sink, Options options)
    : underlyings_(std::move(underlyings)), sink_(std::move(sink)), options_(std::move(options)) {}

void CboeChartHistory::start() {
  stop();
  stopping_ = false;
  thread_ = std::thread(&CboeChartHistory::run, this);
}

void CboeChartHistory::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CboeChartHistory::run() {
  net::HttpClient http;
  while (!stopping_) {
    const md::Timestamp next = poll_once(http);
    const md::Timestamp wait =
        std::clamp<md::Timestamp>(next - options_.clock(), md::kNanosPerSecond, md::kNanosPerMinute);
    std::unique_lock lock(wake_mutex_);
    if (wake_.wait_for(lock, std::chrono::nanoseconds(wait), [this] { return stopping_.load(); }))
      break;
  }
}

md::Timestamp CboeChartHistory::poll_once(net::HttpClient& http) {
  md::Timestamp next = std::numeric_limits<md::Timestamp>::max();
  for (const auto& underlying : underlyings_) {
    for (const auto chart : {CboeChart::Intraday, CboeChart::Daily}) {
      if (options_.daily_only && chart == CboeChart::Intraday) continue;
      const auto key = std::pair{underlying, chart};
      md::Timestamp& due = due_[key];
      const md::Timestamp now = options_.clock();
      if (stopping_) return now;
      if (due <= now) {
        const auto seconds = [](std::chrono::seconds s) { return s.count() * md::kNanosPerSecond; };
        const bool active = md::market_session(now).open ||
                            md::market_session(now - 30 * md::kNanosPerMinute).open;
        due = now + seconds(chart == CboeChart::Daily ? options_.daily_interval
                            : active                   ? options_.session_interval
                                                       : options_.idle_interval);
        std::string failure;
        try {
          const auto response =
              http.get(cboe_chart_url(underlying, chart), {}, options_.timeout, &stopping_);
          if (response.status == 403 || response.status == 404) {
            due = now + seconds(options_.missing_interval);
            failure = "Cboe publishes no chart (HTTP " + std::to_string(response.status) + ")";
          } else if (response.status != 200) {
            failure = "HTTP " + std::to_string(response.status);
          } else {
            auto bars = chart == CboeChart::Intraday ? parse_cboe_intraday(response.body)
                                                     : parse_cboe_daily(response.body);
            sink_(underlying, chart, std::move(bars));
          }
        } catch (const std::exception& error) {
          if (stopping_) return now;
          failure = error.what();
          due = now + seconds(std::min(options_.session_interval, options_.daily_interval));
        }
        const std::lock_guard lock(error_mutex_);
        if (failure.empty())
          errors_.erase(key);
        else
          errors_[key] = "cboe " + underlying +
                         (chart == CboeChart::Intraday ? " minute bars: " : " daily bars: ") + failure;
      }
      next = std::min(next, due);
    }
  }
  return next;
}

std::string CboeChartHistory::error() const {
  const std::lock_guard lock(error_mutex_);
  std::string out;
  for (const auto& [key, message] : errors_) out += (out.empty() ? "" : "\n") + message;
  return out;
}

namespace {
/// One CSV line's fields; a quoted field may hold commas and doubled quotes.
std::vector<std::string> csv_fields(std::string_view line) {
  std::vector<std::string> fields(1);
  bool quoted = false;
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (quoted && c == '"' && i + 1 < line.size() && line[i + 1] == '"') { fields.back() += '"'; ++i; }
    else if (c == '"') quoted = !quoted;
    else if (c == ',' && !quoted) fields.emplace_back();
    else fields.back() += c;
  }
  for (auto& f : fields) {
    const auto first = f.find_first_not_of(" \t");
    f = first == std::string::npos ? std::string() : f.substr(first, f.find_last_not_of(" \t") - first + 1);
  }
  return fields;
}
/// "HH:MM:SS - HH:MM:SS": the end, in minutes after midnight.
std::optional<int> session_end(std::string_view hours) {
  int h1 = 0, m1 = 0, s1 = 0, h2 = 0, m2 = 0, s2 = 0;
  if (std::sscanf(std::string(hours).c_str(), "%d:%d:%d - %d:%d:%d", &h1, &m1, &s1, &h2, &m2, &s2) != 6) return std::nullopt;
  return h2 * 60 + m2;
}
/// When an overnight session "to 11:30 AM (Mon)" ends on `date` itself, in minutes after midnight.
int overnight_into(std::string_view hours, md::Date date) {
  static constexpr std::array<std::string_view, 7> names{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  const std::string day = "(" + std::string(names[static_cast<std::size_t>(md::weekday(date))]) + ")";
  for (auto at = hours.find("to "); at != std::string_view::npos; at = hours.find("to ", at + 3)) {
    int h = 0, m = 0;
    char half[3] = {};
    char weekday[8] = {};
    if (std::sscanf(std::string(hours.substr(at + 3)).c_str(), "%d:%d %2s %7s", &h, &m, half, weekday) != 4) continue;
    if (day != weekday) continue;
    if (std::string_view(half) == "PM" && h != 12) h += 12;
    if (std::string_view(half) == "AM" && h == 12) h = 0;
    return h * 60 + m;
  }
  return 0;
}
}  // namespace

std::vector<md::ScheduledDay> parse_cboe_holidays(std::string_view csv) {
  std::vector<md::ScheduledDay> days;
  bool header = false;
  for (std::size_t start = 0; start < csv.size();) {
    auto end = csv.find('\n', start);
    if (end == std::string_view::npos) end = csv.size();
    auto line = csv.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line.front() == '#') continue;
    const auto fields = csv_fields(line);
    if (!header) {
      header = fields.size() >= 4 && fields[0] == "Holiday Name" && fields[1] == "Date";
      continue;
    }
    if (fields.size() < 4) continue;
    const auto midnight = md::parse_datetime(fields[1] + " 00:00:00", md::Zone::Utc);
    if (!midnight) continue;
    md::ScheduledDay day;
    day.date = md::date_from_days(*midnight / md::kNanosPerDay);
    day.name = fields[0];
    if (fields[2] == "None") {
      day.overnight_until = overnight_into(fields[3], day.date);
    } else if (const auto close = session_end(fields[2]); close && *close < 16 * 60) {
      day.closed = false;
      day.close_hour = *close / 60;
    } else {
      continue;  // a full day
    }
    days.push_back(std::move(day));
  }
  if (!header) throw std::runtime_error("Cboe holiday schedule: no Holiday Name,Date header");
  return days;
}

CboeHolidaySchedule::CboeHolidaySchedule(Sink sink, Options options)
    : sink_(std::move(sink)), options_(std::move(options)) {}

void CboeHolidaySchedule::start() {
  stop();
  stopping_ = false;
  thread_ = std::thread(&CboeHolidaySchedule::run, this);
}

void CboeHolidaySchedule::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CboeHolidaySchedule::run() {
  net::HttpClient http;
  while (!stopping_) {
    const md::Timestamp next = poll_once(http);
    const md::Timestamp wait = std::clamp<md::Timestamp>(next - options_.clock(), md::kNanosPerSecond, 3600 * md::kNanosPerSecond);
    std::unique_lock lock(wake_mutex_);
    if (wake_.wait_for(lock, std::chrono::nanoseconds(wait), [this] { return stopping_.load(); })) break;
  }
}

md::Timestamp CboeHolidaySchedule::poll_once(net::HttpClient& http) {
  const md::Timestamp now = options_.clock();
  if (due_ > now || stopping_) return due_;
  std::string failure;
  try {
    const auto response = http.get(kCboeHolidaysUrl, {}, options_.timeout, &stopping_);
    if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));
    // The latest listing wins: an upcoming day of a year it lists, but no longer this
    // day (rescinded, or back to a full day), is dropped. Past days, and the rest of a
    // year once Cboe lists only the next, are kept as seen.
    auto listed = parse_cboe_holidays(response.body);
    const auto today = md::new_york_time(now).date;
    std::set<int> years;
    for (const auto& day : listed) years.insert(day.date.year);
    std::erase_if(days_, [&](const auto& entry) {
      return !(entry.first < today) && years.contains(entry.first.year) &&
          std::none_of(listed.begin(), listed.end(), [&](const auto& day) { return day.date == entry.first; });
    });
    for (auto& day : listed) days_[day.date] = std::move(day);
    std::vector<md::ScheduledDay> all;
    for (const auto& [date, day] : days_) all.push_back(day);
    sink_(std::move(all));
    due_ = now + options_.interval.count() * md::kNanosPerSecond;
  } catch (const std::exception& e) {
    if (stopping_) return now;
    failure = std::string("cboe holiday schedule: ") + e.what();
    due_ = now + options_.retry.count() * md::kNanosPerSecond;
  }
  const std::lock_guard lock(error_mutex_);
  error_ = std::move(failure);
  return due_;
}

std::string CboeHolidaySchedule::error() const {
  const std::lock_guard lock(error_mutex_);
  return error_;
}

std::string_view cboe_soq_index(std::string_view underlying) {
  for (const auto& entry : kCboeSoqIndices)
    if (entry.underlying == underlying) return entry.index;
  return {};
}

std::string cboe_soq_history_url(std::string_view index) {
  return "https://cdn.cboe.com/api/global/us_indices/daily_prices/" + std::string(index) + "_History.csv";
}

std::vector<SettlementQuotation> parse_cboe_soq_history(std::string_view csv, std::string_view underlying,
                                                        std::string_view index, md::Date since) {
  std::vector<SettlementQuotation> rows;
  bool header = false;
  for (std::size_t start = 0; start < csv.size();) {
    auto end = csv.find('\n', start);
    if (end == std::string_view::npos) end = csv.size();
    auto line = csv.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;
    const auto fields = csv_fields(line);
    if (!header) {
      header = fields.size() >= 2 && fields[0] == "DATE" && fields[1] == index;
      if (!header) break;
      continue;
    }
    // MM/DD/YYYY, as Cboe writes its index histories.
    if (fields.size() < 2 || fields[0].size() != 10 || fields[0][2] != '/' || fields[0][5] != '/') continue;
    const auto midnight = md::parse_datetime(fields[0].substr(6, 4) + "-" + fields[0].substr(0, 2) + "-" +
                                             fields[0].substr(3, 2) + " 00:00:00", md::Zone::Utc);
    if (!midnight) continue;
    char* rest = nullptr;
    const double value = std::strtod(fields[1].c_str(), &rest);
    if (rest == fields[1].c_str() || *rest != '\0' || !std::isfinite(value) || value <= 0) continue;
    const auto date = md::date_from_days(*midnight / md::kNanosPerDay);
    if (date < since) continue;
    rows.push_back({std::string(underlying), std::string(index), date, value, cboe_soq_history_url(index)});
  }
  if (!header) throw std::runtime_error("Cboe " + std::string(index) + " history: no DATE," + std::string(index) + " header");
  return rows;
}

CboeSettlementQuotations::CboeSettlementQuotations(std::vector<std::string> underlyings, Sink sink, Options options)
    : sink_(std::move(sink)), options_(std::move(options)) {
  for (auto& underlying : underlyings)
    if (!cboe_soq_index(underlying).empty() &&
        std::find(underlyings_.begin(), underlyings_.end(), underlying) == underlyings_.end())
      underlyings_.push_back(std::move(underlying));
}

void CboeSettlementQuotations::start() {
  stop();
  stopping_ = false;
  thread_ = std::thread(&CboeSettlementQuotations::run, this);
}

void CboeSettlementQuotations::stop() {
  {
    const std::lock_guard lock(wake_mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void CboeSettlementQuotations::run() {
  net::HttpClient http;
  while (!stopping_) {
    const md::Timestamp next = poll_once(http);
    const md::Timestamp wait = std::clamp<md::Timestamp>(next - options_.clock(), md::kNanosPerSecond, 3600 * md::kNanosPerSecond);
    std::unique_lock lock(wake_mutex_);
    if (wake_.wait_for(lock, std::chrono::nanoseconds(wait), [this] { return stopping_.load(); })) break;
  }
}

md::Timestamp CboeSettlementQuotations::poll_once(net::HttpClient& http) {
  const md::Timestamp now = options_.clock();
  if (due_ > now || stopping_) return due_;
  const auto since = md::date_from_days(md::days_since_epoch(md::trading_date(now)) - options_.lookback_days);
  std::string failures;
  bool changed = false;
  for (const auto& underlying : underlyings_) {
    const auto index = cboe_soq_index(underlying);
    try {
      const auto response = http.get(cboe_soq_history_url(index), {}, options_.timeout, &stopping_);
      if (response.status != 200) throw std::runtime_error("HTTP " + std::to_string(response.status));
      for (auto& row : parse_cboe_soq_history(response.body, underlying, index, since)) {
        auto& held = known_[{row.underlying, row.date}];
        if (held == row) continue;
        held = std::move(row);
        changed = true;
      }
    } catch (const std::exception& e) {
      if (stopping_) return now;
      failures += (failures.empty() ? "" : "\n") + std::string("cboe ") + std::string(index) + " history: " + e.what();
    }
  }
  std::erase_if(known_, [&](const auto& entry) { return entry.first.second < since; });
  if (changed) {
    std::vector<SettlementQuotation> all;
    for (const auto& [key, quote] : known_) all.push_back(quote);
    sink_(std::move(all));
  }
  const auto interval = (failures.empty() ? options_.interval : std::min(options_.interval, options_.retry)).count() * md::kNanosPerSecond;
  due_ = now + interval;
  const std::lock_guard lock(error_mutex_);
  error_ = std::move(failures);
  return due_;
}

std::string CboeSettlementQuotations::error() const {
  const std::lock_guard lock(error_mutex_);
  return error_;
}

std::string cboe_chain_url(std::string_view underlying) {
  std::string url = "https://cdn-api.cboe.com/api/global/delayed_quotes/options/";
  if (md::is_index_underlying(underlying)) url += '_';
  url += underlying;
  url += ".json";
  return url;
}

std::string cboe_page_url(std::string_view underlying) {
  std::string url = "https://www.cboe.com/delayed_quotes/";
  for (const char c : underlying) url += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  url += "/quote_table";
  return url;
}

std::optional<std::string_view> cboe_page_chain(std::string_view html) {
  constexpr std::string_view kMarker = "CTX.contextOptionsData";
  const auto marker = html.find(kMarker);
  if (marker == std::string_view::npos) return std::nullopt;
  const auto open = html.find('{', marker + kMarker.size());
  if (open == std::string_view::npos) return std::nullopt;
  // The object ends where its braces balance, braces inside strings aside.
  int depth = 0;
  bool in_string = false;
  for (auto i = open; i < html.size(); ++i) {
    const char c = html[i];
    if (in_string) {
      if (c == '\\') ++i;
      else if (c == '"') in_string = false;
    } else if (c == '"') {
      in_string = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}' && --depth == 0) {
      return html.substr(open, i - open + 1);
    }
  }
  return std::nullopt;
}

CboeChain parse_cboe_chain(std::string_view json, md::Timestamp now) {
  simdjson::ondemand::parser parser;
  const simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);

  CboeChain chain;
  std::string_view timestamp;
  if (doc["timestamp"].get_string().get(timestamp) == simdjson::SUCCESS) {
    if (timestamp.size() == 8) {
      // A quote page stamps only the UTC time of day.
      const auto date = md::date_from_days(now / md::kNanosPerDay);
      if (const auto at = md::parse_datetime(md::format_date(date) + " " + std::string(timestamp), md::Zone::Utc)) {
        chain.as_of = *at > now + 3600 * md::kNanosPerSecond ? *at - md::kNanosPerDay : *at;
      }
    } else {
      chain.as_of = md::parse_datetime(timestamp, md::Zone::Utc).value_or(0);
    }
  }

  simdjson::ondemand::object data;
  if (doc["data"].get_object().get(data) != simdjson::SUCCESS) {
    throw std::runtime_error("Cboe chain: missing data object");
  }
  // On-demand parsing is fastest when fields are read in document order, so take
  // them in the order Cboe writes them: the options array first, then the underlying.
  simdjson::ondemand::array options;
  if (data["options"].get_array().get(options) != simdjson::SUCCESS) {
    throw std::runtime_error("Cboe chain: missing options array");
  }
  for (simdjson::ondemand::object row : options) {
    CboeOption option;
    option.symbol = text_or_empty(row, "option");
    option.bid = number_or_zero(row, "bid");
    option.bid_size = number_or_zero(row, "bid_size");
    option.ask = number_or_zero(row, "ask");
    option.ask_size = number_or_zero(row, "ask_size");
    option.iv = number_or_zero(row, "iv");
    option.open_interest = number_or_zero(row, "open_interest");
    double volume = 0.0;
    if (row["volume"].get_double().get(volume) == simdjson::SUCCESS &&
        std::isfinite(volume) && volume >= 0.0) option.volume = volume;
    option.delta = number_or_zero(row, "delta");
    option.gamma = number_or_zero(row, "gamma");
    option.vega = number_or_zero(row, "vega");
    option.theta = number_or_zero(row, "theta");
    option.rho = number_or_zero(row, "rho");
    option.theo = number_or_zero(row, "theo");
    if (!option.symbol.empty()) chain.options.push_back(std::move(option));
  }
  chain.symbol = text_or_empty(data, "symbol");
  chain.price = number_or_zero(data, "current_price");
  chain.bid = number_or_zero(data, "bid");
  chain.ask = number_or_zero(data, "ask");
  chain.last_trade_time =
      md::parse_datetime(text_or_empty(data, "last_trade_time"), md::Zone::NewYork).value_or(0);
  chain.prev_close = number_or_zero(data, "prev_day_close");
  chain.close = number_or_zero(data, "close");
  return chain;
}

md::Capabilities CboeDelayedProvider::capabilities() const noexcept {
  md::Capabilities caps;
  caps.poll_interval = options_.poll_interval;
  caps.realtime = false;
  caps.delay = kDelay;
  caps.quotes = true;
  caps.trades = false;
  caps.open_interest = true;
  caps.vendor_greeks = true;
  caps.history = false;
  // Outside the regular session the underlying keeps its close (see publish_chain).
  caps.extended_hours_prints = false;
  return caps;
}

std::string CboeDelayedProvider::poll(net::HttpClient& http, const std::string& underlying,
                                      const md::Subscription& subscription, md::EventSink& sink) {
  // The CDN file is lighter and usually current. When it falls behind (as on
  // 2026-09-23, when it stopped updating), Cboe's own quote page embeds the same
  // chain, fresher; read that for a while, then try the file again.
  const auto now = options_.clock();
  const auto stale = std::chrono::duration_cast<std::chrono::nanoseconds>(options_.stale_after).count();
  net::HttpResponse response;
  CboeChain chain;
  bool have = false;
  std::string source = "file";
  double parse_ms = 0;
  const auto parse = [&](std::string_view json) {
    const auto started = std::chrono::steady_clock::now();
    auto parsed = parse_cboe_chain(json, now);
    parse_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    return parsed;
  };
  const bool holding = page_until_[underlying] > now;
  bool read_page = holding;
  if (read_page && now - page_fetched_[underlying] < std::chrono::duration_cast<std::chrono::nanoseconds>(options_.page_interval).count())
    return "cboe " + underlying + " (page): waiting for Cboe's next page, about once a minute";
  if (!read_page) {
    response = http.get(cboe_chain_url(underlying), {}, options_.timeout, cancellation());
    if (response.status == 200) {
      chain = parse(response.body);
      have = true;
    }
    read_page = (!have || chain.as_of == 0 || now - chain.as_of > stale) && skip_page_until_[underlying] <= now;
  }
  if (read_page) {
    check_cancelled();
    auto page = http.get(cboe_page_url(underlying), {}, options_.timeout, cancellation());
    page_fetched_[underlying] = now;
    const auto embedded = page.status == 200 ? cboe_page_chain(page.body) : std::nullopt;
    if (embedded) {
      auto from_page = parse(*embedded);
      if (!have || from_page.as_of > chain.as_of) {
        chain = std::move(from_page);
        response = std::move(page);
        have = true;
        source = "page";
        // The hold starts when the page beats the file, and is not extended by
        // reading the page during it: the file is tried again when it ends.
        if (!holding) page_until_[underlying] = now + std::chrono::duration_cast<std::chrono::nanoseconds>(options_.page_hold).count();
      }
    }
    // A page no fresher than the file (both idle overnight, say) is left alone for a while.
    if (source != "page") {
      page_until_.erase(underlying);
      skip_page_until_[underlying] = now + std::chrono::duration_cast<std::chrono::nanoseconds>(options_.page_hold).count();
    }
  }
  if (!have) throw std::runtime_error("HTTP " + std::to_string(response.status));
  check_cancelled();
  publish_chain(chain, subscription, sink);

  char summary[208];
  std::snprintf(summary, sizeof summary, "cboe %s (%s): %zu options, %.2f MB in %.0f ms, parsed in %.1f ms",
                underlying.c_str(), source.c_str(), chain.options.size(),
                static_cast<double>(response.wire_bytes) / 1e6,
                static_cast<double>(response.elapsed.count()) / 1e3, parse_ms);
  return summary;
}

void CboeDelayedProvider::publish_chain(const CboeChain& chain,
                                        const md::Subscription& subscription,
                                        md::EventSink& sink) {
  // Quotes describe the market 15 minutes before the snapshot was generated.
  const md::Timestamp delayed =
      chain.as_of - std::chrono::duration_cast<std::chrono::nanoseconds>(kDelay).count();
  std::string underlying = chain.symbol;
  if (!underlying.empty() && underlying.front() == '^') underlying.erase(0, 1);
  const md::Timestamp ts = md::trading_session(underlying, delayed).market_time;
  const auto today = md::new_york_time(delayed).date;
  const bool after_close = md::trading_date(md::new_york_to_utc(today, 12, 0)) == today &&
                           delayed >= md::new_york_to_utc(today, md::regular_close_hour(today), 0);
  // Outside the regular session Cboe's price goes on with after- and pre-market trades,
  // still stamped with the last regular trade, so the underlying keeps its last regular
  // close instead: today's after the close, and before the open (or on a day without a
  // session) the previous close, which Cboe rolls over in the evening.
  const double regular_close = md::market_session(delayed).open ? 0.0 : after_close ? chain.close : chain.prev_close;
  const bool use_close = regular_close > 0 && std::isfinite(regular_close);
  // Stock/index prints have their own clock; they can be hours behind GTH options.
  sink.publish(md::UnderlyingQuote{underlying, chain.last_trade_time,
                                   use_close ? 0 : chain.bid, use_close ? 0 : chain.ask,
                                   use_close ? regular_close : chain.price});
  // After the close Cboe's close field holds the day's closing price, which it may
  // revise within minutes, while the price goes on with after-hours trades. In the
  // regular session the previous close is certainly the last business day's; in the
  // evening Cboe may not have rolled it over yet.
  const auto official = [&](md::Date date, double price) {
    if (!(price > 0) || !std::isfinite(price)) return;
    auto& last = closes_[underlying];
    if (last == std::pair{date, price}) return;
    last = {date, price};
    sink.publish(md::UnderlyingClose{underlying, delayed, date, price});
  };
  if (md::market_session(delayed).open)
    official(md::previous_business_day(today), chain.prev_close);
  else if (after_close)
    official(today, chain.close);
  std::map<std::string, md::Timestamp> market_times;

  std::vector<std::pair<const CboeOption*, md::OptionContract>> contracts;
  contracts.reserve(chain.options.size());
  std::set<md::Date> expiries;
  for (const CboeOption& option : chain.options) {
    if (std::optional<md::OptionContract> contract = md::parse_osi(option.symbol)) {
      expiries.insert(contract->expiry);
      contracts.emplace_back(&option, std::move(*contract));
    }
  }
  const ChainFilter filter(subscription, md::date_from_days(chain.as_of / md::kNanosPerDay),
                           chain.price, expiries);

  std::set<md::InstrumentId> seen;
  for (auto& [option, contract] : contracts) {
    if (!publisher_.known(option->symbol) && !filter.admits(contract)) continue;
    check_cancelled();
    const auto [clock, inserted] = market_times.try_emplace(contract.root);
    if (inserted) clock->second = md::trading_session(contract.root, delayed).market_time;
    const auto option_time = clock->second;
    const md::InstrumentId id = publisher_.define(option->symbol, std::move(contract), sink);
    seen.insert(id);
    publisher_.quote(id, option_time, option->bid, option->ask, option->bid_size, option->ask_size,
                     sink);
    publisher_.open_interest(id, option_time, option->open_interest, sink);
    publisher_.volume(id, option_time, option->volume, sink);
    publisher_.greeks(md::VendorGreeks{id, option_time, option->iv, option->delta, option->gamma,
                                       option->vega, option->theta, option->rho},
                      sink);
  }
  publisher_.finish(underlying, seen, ts, sink);
}

}  // namespace openport::providers
