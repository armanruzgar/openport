#include "openport/server/earnings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

#include "openport/server/engine.hpp"

namespace openport::server {
namespace {
using nlohmann::json;

std::optional<md::Date> parse_date(std::string_view text) {
  int y = 0, m = 0, d = 0;
  char tail = 0;
  const std::string s(text);
  if (s.size() != 10 || std::sscanf(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &tail) != 3 || y < 1970 || m < 1 || m > 12 ||
      d < 1 || d > 31) return std::nullopt;
  const md::Date date{y, m, d};
  if (md::format_date(md::date_from_days(md::days_since_epoch(date))) != s) return std::nullopt;
  return date;
}

md::Timestamp close_of(md::Date date) { return md::new_york_to_utc(date, md::regular_close_hour(date), 0); }

/// When the news can first move the price: the event day's open, or its close.
md::Timestamp event_start(md::Date date, std::string_view session) {
  return session == "after_close" ? close_of(date) : md::new_york_to_utc(date, 9, 30);
}

std::string expiry_id(const analytics::SliceMetrics& slice) {
  return md::format_date(slice.expiry) + (slice.expiry_time == md::new_york_to_utc(slice.expiry, 9, 30) ? "AM" : "PM") +
         (slice.root.empty() ? "" : "-" + slice.root);
}

bool two_sided(const analytics::OptionMetrics& o) {
  return std::isfinite(o.bid) && std::isfinite(o.ask) && o.bid > 0 && o.ask >= o.bid;
}

std::optional<double> close_on(const std::vector<md::Bar>& days, md::Date date) {
  for (const auto& bar : days)
    if (md::new_york_time(bar.start).date == date && bar.close > 0 && std::isfinite(bar.close)) return bar.close;
  return std::nullopt;
}

std::optional<double> percent(std::string_view raw) {
  std::string text(raw);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  if (!text.empty() && text.back() == '%') text.pop_back();
  std::istringstream in(text);
  in.imbue(std::locale::classic());
  double value = 0;
  in >> std::noskipws >> value;
  if (text.empty() || !in || in.peek() != std::char_traits<char>::eof() || !std::isfinite(value)) return std::nullopt;
  return value / 100;
}

json move_json(const EarningsMove& m) {
  const auto finite = [](double v) { return std::isfinite(v) ? json(v) : json(nullptr); };
  const auto date = [](md::Date d) { return d.year ? json(md::format_date(d)) : json(nullptr); };
  return {{"symbol", m.symbol}, {"date", md::format_date(m.date)}, {"session", m.session},
          {"label", m.label.empty() ? json(nullptr) : json(m.label)}, {"source", m.source},
          {"implied", m.implied ? json(*m.implied) : json(nullptr)},
          {"implied_at", m.implied_at > 0 ? json(md::format_timestamp(m.implied_at)) : json(nullptr)},
          {"expiry", m.expiry.empty() ? json(nullptr) : json(m.expiry)}, {"strike", finite(m.strike)},
          {"straddle", finite(m.straddle)}, {"spot", finite(m.spot)},
          {"realized", m.realized ? json(*m.realized) : json(nullptr)}, {"before", date(m.before)}, {"after", date(m.after)},
          {"close_before", finite(m.close_before)}, {"close_after", finite(m.close_after)},
          {"priced", m.priced() ? json(*m.priced()) : json(nullptr)}};
}

EarningsMove move_from(const json& j) {
  const auto number = [&](const char* key) { return j.contains(key) && j.at(key).is_number() ? j.at(key).get<double>() : analytics::kNaN; };
  const auto day = [&](const char* key) -> md::Date {
    if (!j.contains(key) || j.at(key).is_null()) return {};
    const auto date = parse_date(j.at(key).get<std::string>());
    if (!date) throw std::runtime_error(std::string("invalid ") + key);
    return *date;
  };
  EarningsMove m;
  m.symbol = j.at("symbol").get<std::string>();
  const auto date = parse_date(j.at("date").get<std::string>());
  if (!date) throw std::runtime_error("invalid date");
  m.date = *date;
  m.session = j.at("session").get<std::string>();
  m.label = j.value("label", json(nullptr)).is_string() ? j.at("label").get<std::string>() : "";
  m.source = j.at("source").get<std::string>();
  if (j.contains("implied") && j.at("implied").is_number()) m.implied = j.at("implied").get<double>();
  if (j.contains("implied_at") && j.at("implied_at").is_string())
    m.implied_at = md::parse_datetime(j.at("implied_at").get<std::string>(), md::Zone::Utc).value_or(0);
  if (j.contains("expiry") && j.at("expiry").is_string()) m.expiry = j.at("expiry").get<std::string>();
  m.strike = number("strike");
  m.straddle = number("straddle");
  m.spot = number("spot");
  if (j.contains("realized") && j.at("realized").is_number()) m.realized = j.at("realized").get<double>();
  m.before = day("before");
  m.after = day("after");
  m.close_before = number("close_before");
  m.close_after = number("close_after");
  return m;
}
}  // namespace

std::optional<std::string> EarningsMove::priced() const {
  if (!implied || !realized) return std::nullopt;
  return *implied > std::abs(*realized) ? "over" : "under";
}

md::Date earnings_last_session(md::Date date, std::string_view session) {
  return session == "after_close" ? date : md::previous_business_day(date);
}

md::Date earnings_reaction_session(md::Date date, std::string_view session) {
  // After 17:00 ET a moment belongs to the next business day.
  return session == "after_close" ? md::trading_date(md::new_york_to_utc(date, 18, 0)) : date;
}

std::optional<EarningsMove> implied_move(const trading::PlanEvent& event, const analytics::UnderlyingMetrics& m) {
  const auto date = parse_date(event.time);
  if (event.kind != "earnings" || event.symbol != m.symbol || !date) return std::nullopt;
  const auto session = event.session.empty() ? std::string("before_open") : event.session;
  // Only the last session before the news: an older snapshot measures more than the event.
  const auto last = earnings_last_session(*date, session);
  if (m.as_of >= event_start(*date, session) || m.as_of < md::new_york_to_utc(last, 9, 30)) return std::nullopt;
  if (!(m.spot > 0) || !std::isfinite(m.spot)) return std::nullopt;
  const auto reaction_close = close_of(earnings_reaction_session(*date, session));
  const analytics::SliceMetrics* front = nullptr;
  for (const auto& slice : m.slices)
    if (slice.expiry_time >= reaction_close && (!front || slice.expiry_time < front->expiry_time)) front = &slice;
  if (!front) return std::nullopt;
  const double forward = std::isfinite(front->forward.forward) && front->forward.forward > 0 ? front->forward.forward : m.spot;
  const analytics::StrikeMetrics* atm = nullptr;
  for (const auto& strike : front->strikes)
    if (two_sided(strike.call) && two_sided(strike.put) &&
        (!atm || std::abs(strike.strike - forward) < std::abs(atm->strike - forward))) atm = &strike;
  if (!atm) return std::nullopt;
  EarningsMove move;
  move.symbol = m.symbol;
  move.date = *date;
  move.session = session;
  move.label = event.label;
  move.source = "recorded";
  move.straddle = atm->call.mid + atm->put.mid;
  move.spot = m.spot;
  move.strike = atm->strike;
  move.expiry = expiry_id(*front);
  move.implied = move.straddle / m.spot;
  move.implied_at = m.as_of;
  move.before = last;
  move.after = earnings_reaction_session(*date, session);
  return move;
}

EarningsStore::EarningsStore(std::filesystem::path file) : file_(std::move(file)) {
  if (file_.empty()) return;
  try {
    if (!std::filesystem::exists(file_)) return;
    std::ifstream in(file_);
    const auto data = json::parse(in);
    for (const auto& item : data.at("moves")) {
      auto move = move_from(item);
      moves_[{move.symbol, move.date}] = std::move(move);
    }
  } catch (const std::exception& e) {
    error_ = file_.string() + ": " + e.what();
  }
}

void EarningsStore::save() {
  if (file_.empty()) return;
  try {
    json moves = json::array();
    for (const auto& [key, move] : moves_) moves.push_back(move_json(move));
    if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path());
    const auto temporary = file_.string() + ".tmp";
    {
      std::ofstream out(temporary, std::ios::trunc);
      out << json{{"version", 1}, {"moves", moves}}.dump(1) << '\n';
      if (!out.flush()) throw std::runtime_error("cannot write " + temporary);
    }
    std::filesystem::rename(temporary, file_);
  } catch (const std::exception& e) {
    error_ = "earnings: " + std::string(e.what());
  }
}

void EarningsStore::capture(const std::vector<trading::PlanEvent>& calendar, const analytics::UnderlyingMetrics& metrics) {
  bool changed = false;
  const std::lock_guard lock(mutex_);
  for (const auto& event : calendar) {
    auto move = implied_move(event, metrics);
    if (!move) continue;
    const auto key = std::make_tuple(move->symbol, move->date);
    const auto found = moves_.find(key);
    if (found != moves_.end() && (found->second.source != "recorded" || found->second.implied_at >= move->implied_at)) continue;
    moves_[key] = std::move(*move);
    changed = true;
  }
  if (changed) save();
}

void EarningsStore::settle(const CandleStore& candles, md::Timestamp now) {
  bool changed = false;
  const std::lock_guard lock(mutex_);
  std::map<std::string, std::vector<md::Bar>> history;
  for (auto& [key, move] : moves_) {
    // A session's daily bar is built from its minutes while it trades: wait for its close.
    if (move.source != "recorded" || move.realized || !move.implied || now < close_of(move.after) + 15 * md::kNanosPerMinute) continue;
    auto found = history.find(move.symbol);
    if (found == history.end()) found = history.emplace(move.symbol, candles.daily_history(move.symbol)).first;
    const auto before = close_on(found->second, move.before);
    const auto after = close_on(found->second, move.after);
    if (!before || !after) continue;
    move.close_before = *before;
    move.close_after = *after;
    move.realized = *after / *before - 1;
    changed = true;
  }
  if (changed) save();
}

void EarningsStore::observe(const MetricsSource& source, const std::vector<trading::PlanEvent>& calendar) {
  if (std::none_of(calendar.begin(), calendar.end(), [](const auto& e) { return e.kind == "earnings"; })) return;
  for (const auto& symbol : source.symbols())
    if (const auto metrics = source.metrics(symbol)) capture(calendar, *metrics);
  if (const auto* candles = source.candles()) settle(*candles, source.wall_time());
}

EarningsStore::Import EarningsStore::import_csv(std::string_view text) {
  Import result;
  std::vector<EarningsMove> rows;
  std::istringstream in{std::string(text)};
  std::string line;
  bool header = false;
  for (int n = 1; std::getline(in, line); ++n) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> fields;
    std::stringstream cells(line);
    for (std::string cell; std::getline(cells, cell, ',');) {
      while (!cell.empty() && std::isspace(static_cast<unsigned char>(cell.front()))) cell.erase(cell.begin());
      while (!cell.empty() && std::isspace(static_cast<unsigned char>(cell.back()))) cell.pop_back();
      fields.push_back(cell);
    }
    if (!line.empty() && line.back() == ',') fields.emplace_back();
    const auto where = "line " + std::to_string(n);
    if (!header) {
      if (fields.size() < 5 || fields[0] != "symbol" || fields[1] != "date" || fields[2] != "session" ||
          fields[3] != "implied_move" || fields[4] != "realized_move" || (fields.size() == 6 && fields[5] != "label") || fields.size() > 6)
        throw std::invalid_argument(where + ": the header must be symbol,date,session,implied_move,realized_move[,label]");
      header = true;
      continue;
    }
    if (fields.size() < 5 || fields.size() > 6) throw std::invalid_argument(where + ": expected 5 or 6 columns");
    EarningsMove move;
    move.symbol = fields[0];
    if (move.symbol.empty() || move.symbol.size() > 12 || !std::all_of(move.symbol.begin(), move.symbol.end(), [](char c) {
          return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw std::invalid_argument(where + ": symbol must be 1-12 uppercase letters, digits or dots");
    const auto date = parse_date(fields[1]);
    if (!date) throw std::invalid_argument(where + ": date must be YYYY-MM-DD");
    move.date = *date;
    move.session = fields[2].empty() ? "before_open" : fields[2];
    if (move.session != "before_open" && move.session != "after_close")
      throw std::invalid_argument(where + ": session must be before_open or after_close");
    move.implied = percent(fields[3]);
    move.realized = percent(fields[4]);
    if (!move.implied || *move.implied <= 0 || *move.implied > 5 || !move.realized || std::abs(*move.realized) > 5)
      throw std::invalid_argument(where + ": moves are percents: implied above 0, both at most 500");
    if (fields.size() == 6) move.label = fields[5].substr(0, 64);
    move.source = "imported";
    rows.push_back(std::move(move));
  }
  if (!header) throw std::invalid_argument("the file is empty");
  const std::lock_guard lock(mutex_);
  for (auto& move : rows) {
    const auto key = std::make_tuple(move.symbol, move.date);
    if (const auto found = moves_.find(key); found != moves_.end() && found->second.source == "recorded") {
      result.skipped.push_back(move.symbol + " " + md::format_date(move.date) + ": already recorded here");
      continue;
    }
    moves_[key] = std::move(move);
    ++result.added;
  }
  if (result.added) save();
  return result;
}

std::size_t EarningsStore::remove_imported() {
  const std::lock_guard lock(mutex_);
  const auto removed = std::erase_if(moves_, [](const auto& item) { return item.second.source == "imported"; });
  if (removed) save();
  return removed;
}

std::vector<EarningsMove> EarningsStore::moves(std::string_view symbol) const {
  const std::lock_guard lock(mutex_);
  std::vector<EarningsMove> out;
  for (const auto& [key, move] : moves_)
    if (symbol.empty() || move.symbol == symbol) out.push_back(move);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return std::tie(b.date, a.symbol) < std::tie(a.date, b.symbol); });
  return out;
}

std::string EarningsStore::error() const {
  const std::lock_guard lock(mutex_);
  return error_;
}

std::optional<ApiResponse> earnings_request(const ApiRequest& request, EarningsStore& store) {
  const std::string_view target = request.target;
  const auto mark = target.find('?');
  const auto path = target.substr(0, mark);
  if (path != "/api/earnings" && path != "/api/earnings/import" && path != "/api/earnings/imported") return std::nullopt;
  const auto query = query_parameters(mark == std::string_view::npos ? std::string_view{} : target.substr(mark + 1));
  if (!query) return api_error(400, "INVALID_REQUEST", "Duplicate or malformed query parameter");
  if (path == "/api/earnings") {
    if (request.method != "GET") return api_error(405, "METHOD_NOT_ALLOWED", "Earnings history supports GET");
    std::string symbol;
    for (const auto& [key, value] : *query) {
      if (key != "symbol") return api_error(400, "INVALID_REQUEST", "Earnings accepts only symbol");
      symbol = value;
    }
    json moves = json::array();
    std::size_t over = 0, under = 0, imported = 0;
    double implied = 0, realized = 0;
    for (const auto& move : store.moves(symbol)) {
      moves.push_back(move_json(move));
      if (move.source == "imported") ++imported;
      if (const auto priced = move.priced()) {
        (*priced == "over" ? over : under)++;
        implied += *move.implied;
        realized += std::abs(*move.realized);
      }
    }
    const auto settled = over + under;
    const auto mean = [&](double total) { return settled ? json(total / static_cast<double>(settled)) : json(nullptr); };
    return ApiResponse{200, json{{"symbol", symbol.empty() ? json(nullptr) : json(symbol)}, {"moves", moves},
        {"summary", {{"events", settled}, {"over", over}, {"under", under}, {"imported", imported},
                     {"over_rate", settled ? json(static_cast<double>(over) / static_cast<double>(settled)) : json(nullptr)},
                     {"mean_implied", mean(implied)}, {"mean_realized", mean(realized)}}},
        {"error", store.error().empty() ? json(nullptr) : json(store.error())},
        {"model", "Implied: the front straddle's mid over spot from the last analytics before the event, in the first expiry "
                  "settling after the reaction session. Realized: the reaction session's close over the last close before it. "
                  "Over means the straddle priced a larger move than came."}}.dump()};
  }
  if (path == "/api/earnings/imported") {
    if (request.method != "DELETE") return api_error(405, "METHOD_NOT_ALLOWED", "Imported earnings support DELETE");
    return ApiResponse{200, json{{"removed", store.remove_imported()}}.dump()};
  }
  if (request.method != "POST") return api_error(405, "METHOD_NOT_ALLOWED", "Earnings import supports POST");
  try {
    const auto body = json::parse(request.body);
    if (!body.is_object() || body.size() != 1 || !body.contains("csv") || !body.at("csv").is_string())
      return api_error(400, "INVALID_REQUEST", "Send {csv}");
    const auto result = store.import_csv(body.at("csv").get<std::string>());
    return ApiResponse{200, json{{"added", result.added}, {"skipped", result.skipped}}.dump()};
  } catch (const json::exception&) {
    return api_error(400, "INVALID_REQUEST", "The body must be JSON");
  } catch (const std::invalid_argument& error) {
    return api_error(400, "INVALID_IMPORT", error.what());
  }
}

}  // namespace openport::server
