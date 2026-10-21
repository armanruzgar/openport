#include "openport/trading/fill_quality.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <variant>

namespace openport::trading {
namespace {
constexpr std::size_t kMaxBrokerFills = 5000;

std::string lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  return text;
}
/// Contract-weighted median of (value, weight) pairs.
double weighted_median(std::vector<std::pair<double, Quantity>> values) {
  std::sort(values.begin(), values.end());
  Quantity total = 0;
  for (const auto& [value, weight] : values) total += weight;
  Quantity seen = 0;
  for (const auto& [value, weight] : values) {
    seen += weight;
    if (2 * seen >= total) return value;
  }
  return values.empty() ? 0 : values.back().first;
}

// ---- CSV ----
struct Row {
  std::size_t line = 0;
  std::vector<std::string> cells;
  [[nodiscard]] bool blank() const {
    return std::all_of(cells.begin(), cells.end(), [](const std::string& cell) { return trim(cell).empty(); });
  }
};
/// RFC 4180 rows: quoted fields may hold commas, doubled quotes and line breaks.
std::vector<Row> csv_rows(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  std::vector<Row> rows;
  Row row{1, {}};
  std::string cell;
  bool quoted = false, any = false;
  std::size_t line = 1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') { cell += '"'; ++i; }
      else if (c == '"') quoted = false;
      else { if (c == '\n') ++line; cell += c; }
      continue;
    }
    if (c == '"') { quoted = true; any = true; }
    else if (c == ',') { row.cells.push_back(std::move(cell)); cell.clear(); any = true; }
    else if (c == '\r') {}
    else if (c == '\n') {
      if (any || !cell.empty()) row.cells.push_back(std::move(cell));
      rows.push_back(std::move(row));
      cell.clear();
      any = false;
      row = Row{++line, {}};
    } else { cell += c; any = true; }
  }
  if (any || !cell.empty()) row.cells.push_back(std::move(cell));
  if (!row.cells.empty()) rows.push_back(std::move(row));
  return rows;
}
/// Columns by lower-case, trimmed header name.
struct Header {
  std::map<std::string, std::size_t> columns;
  explicit Header(const Row& row) {
    for (std::size_t i = 0; i < row.cells.size(); ++i) columns.emplace(lower(trim(row.cells[i])), i);
  }
  [[nodiscard]] std::optional<std::size_t> find(std::initializer_list<std::string_view> names) const {
    for (const auto name : names)
      if (const auto it = columns.find(std::string(name)); it != columns.end()) return it->second;
    return std::nullopt;
  }
  [[nodiscard]] bool has(std::initializer_list<std::string_view> names) const { return find(names).has_value(); }
};
std::string_view cell(const Row& row, std::optional<std::size_t> column) {
  return column && *column < row.cells.size() ? trim(row.cells[*column]) : std::string_view{};
}

// ---- Fields ----
/// "$1,234.50", "(4.20)" and "-4.2" alike; nothing for an empty or malformed cell.
std::optional<Money> money_cell(std::string_view text) {
  std::string clean;
  bool negative = false;
  for (const char c : trim(text)) {
    if (c == '$' || c == ',' || c == ' ' || c == '+') continue;
    if (c == '(' || c == ')') { negative = negative || c == '('; continue; }
    clean += c;
  }
  if (clean.empty()) return std::nullopt;
  try {
    // Exports round to cents, but averages can carry more digits than micro-dollars.
    if (const auto dot = clean.find('.'); dot != std::string::npos && clean.size() - dot - 1 > 6) clean.resize(dot + 7);
    const auto value = Money::parse(clean);
    return negative ? -value : value;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}
std::optional<std::int64_t> integer_cell(std::string_view text) {
  std::string clean;
  for (const char c : trim(text)) if (c != ',' && c != '+' && c != ' ') clean += c;
  if (const auto dot = clean.find('.'); dot != std::string::npos &&
      clean.find_first_not_of('0', dot + 1) == std::string::npos) clean.resize(dot);
  std::int64_t value = 0;
  const auto [end, error] = std::from_chars(clean.data(), clean.data() + clean.size(), value);
  if (clean.empty() || error != std::errc{} || end != clean.data() + clean.size()) return std::nullopt;
  return value;
}
std::optional<Side> side_cell(std::string_view text) {
  const auto value = lower(trim(text));
  if (value.starts_with("buy") || value == "b" || value == "bot" || value == "bto" || value == "btc") return Side::Buy;
  if (value.starts_with("sell") || value == "s" || value == "sld" || value == "sto" || value == "stc") return Side::Sell;
  return std::nullopt;
}
int month_number(std::string_view name) {
  static constexpr std::array<std::string_view, 12> months{"jan", "feb", "mar", "apr", "may", "jun",
                                                          "jul", "aug", "sep", "oct", "nov", "dec"};
  const auto key = lower(name.substr(0, 3));
  for (std::size_t i = 0; i < months.size(); ++i) if (months[i] == key) return static_cast<int>(i) + 1;
  return 0;
}
/// New York wall-clock time unless the text names its offset, written
/// "-04:00" or "-0400". Commas and semicolons separating date and time count as spaces.
std::optional<Timestamp> time_cell(std::string_view text) {
  std::string clean(trim(text));
  for (auto& c : clean) if (c == ',' || c == ';') c = ' ';
  std::string squeezed;
  for (const char c : clean) if (!(c == ' ' && (squeezed.empty() || squeezed.back() == ' '))) squeezed += c;
  clean = std::string(trim(squeezed));
  // 20261021 101532 (IBKR's yyyyMMdd;HHmmss)
  if (clean.size() == 15 && clean[8] == ' ' && std::all_of(clean.begin(), clean.begin() + 8, ::isdigit) &&
      std::all_of(clean.begin() + 9, clean.end(), ::isdigit))
    clean = clean.substr(0, 4) + "-" + clean.substr(4, 2) + "-" + clean.substr(6, 2) + " " +
            clean.substr(9, 2) + ":" + clean.substr(11, 2) + ":" + clean.substr(13, 2);
  // 10/21/26 10:15:32 or 10/21/2026 10:15:32 (thinkorswim, Schwab)
  if (const auto first = clean.find('/'); first != std::string::npos && first <= 2) {
    const auto second = clean.find('/', first + 1);
    const auto space = clean.find(' ');
    if (second == std::string::npos || space == std::string::npos || space < second) return std::nullopt;
    auto year = clean.substr(second + 1, space - second - 1);
    if (year.size() == 2) year = "20" + year;
    const auto two = [](std::string value) { return value.size() == 1 ? "0" + value : value; };
    clean = year + "-" + two(clean.substr(0, first)) + "-" + two(clean.substr(first + 1, second - first - 1)) + clean.substr(space);
  }
  if (clean.size() > 5) {
    const auto tail = clean.substr(clean.size() - 5);
    if ((tail[0] == '+' || tail[0] == '-') && std::all_of(tail.begin() + 1, tail.end(), ::isdigit))
      clean = clean.substr(0, clean.size() - 5) + tail.substr(0, 3) + ":" + tail.substr(3);
  }
  return md::parse_datetime(clean, md::Zone::NewYork);
}
/// The padded OSI for a root, expiry, right (C or P) and strike.
std::optional<std::string> osi_of(std::string_view root, md::Date expiry, char right, double strike) {
  if (root.empty() || root.size() > 6 || !md::valid_date(expiry) || expiry.year < 2000 || expiry.year > 2099 ||
      !(strike > 0) || strike >= 100000)
    return std::nullopt;
  const auto two = [](int value) { return std::string(value < 10 ? "0" : "") + std::to_string(value); };
  auto thousandths = std::to_string(std::llround(strike * 1000));
  thousandths.insert(0, 8 - std::min<std::size_t>(8, thousandths.size()), '0');
  const auto compact = std::string(root) + two(expiry.year % 100) + two(expiry.month) + two(expiry.day) + right + thousandths;
  const auto contract = md::parse_osi(compact);
  return contract ? std::optional(contract->osi_symbol()) : std::nullopt;
}
std::vector<std::string> words(std::string_view text) {
  std::vector<std::string> out;
  std::string word;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c))) { if (!word.empty()) out.push_back(std::move(word)); word.clear(); }
    else word += c;
  }
  if (!word.empty()) out.push_back(std::move(word));
  return out;
}
std::optional<double> strike_cell(std::string_view text) {
  std::string clean;
  for (const char c : trim(text)) if (c != ',' && c != '$') clean += c;
  if (clean.empty()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(clean.c_str(), &end);
  if (end != clean.c_str() + clean.size() || !std::isfinite(value)) return std::nullopt;
  return value;
}
std::optional<char> right_cell(std::string_view text) {
  const auto value = lower(trim(text));
  if (value == "c" || value == "call") return 'C';
  if (value == "p" || value == "put") return 'P';
  return std::nullopt;
}
/// An OSI symbol padded or compact, or a broker's "ROOT DDMMMYY STRIKE C" (IBKR)
/// or "ROOT MM/DD/YYYY STRIKE C" (Schwab) description.
std::optional<std::string> symbol_cell(std::string_view text) {
  const auto value = trim(text);
  if (const auto contract = md::parse_osi(value)) return contract->osi_symbol();
  const auto parts = words(value);
  if (parts.size() != 4) return std::nullopt;
  const auto strike = strike_cell(parts[2]);
  const auto right = right_cell(parts[3]);
  if (!strike || !right) return std::nullopt;
  md::Date expiry;
  const auto& date = parts[1];
  if (date.size() == 7 && month_number(date.substr(2, 3)) > 0) {
    expiry = {2000 + std::atoi(date.substr(5, 2).c_str()), month_number(date.substr(2, 3)), std::atoi(date.substr(0, 2).c_str())};
  } else if (const auto first = date.find('/'), last = date.rfind('/'); first != std::string::npos && last != first) {
    auto year = std::atoi(date.substr(last + 1).c_str());
    if (year < 100) year += 2000;
    expiry = {year, std::atoi(date.substr(0, first).c_str()), std::atoi(date.substr(first + 1, last - first - 1).c_str())};
  } else {
    return std::nullopt;
  }
  return osi_of(parts[0], expiry, *right, *strike);
}

// ---- Formats ----
/// What a row parser makes of one row: a fill, nothing (a row of another kind,
/// with why), or an error.
using RowResult = std::variant<BrokerFill, std::string>;
void add_row(BrokerImport& out, std::size_t line, RowResult result) {
  if (auto* fill = std::get_if<BrokerFill>(&result)) {
    if (out.fills.size() >= kMaxBrokerFills) {
      if (out.skipped.empty() || out.skipped.back().second.find("5,000") == std::string::npos)
        out.skipped.emplace_back(line, "Only the first 5,000 fills are read");
      return;
    }
    fill->row = line;
    out.fills.push_back(std::move(*fill));
  } else {
    out.skipped.emplace_back(line, std::get<std::string>(result));
  }
}
/// Quantity's sign stands in for a missing side.
RowResult finish(std::optional<std::string> symbol, std::optional<Side> side, std::optional<std::int64_t> quantity,
                 std::optional<Money> price, Timestamp time) {
  if (!symbol) return std::string("Not an option contract this import reads");
  if (!quantity || *quantity == 0) return std::string("No quantity");
  if (!side) side = *quantity > 0 ? Side::Buy : Side::Sell;
  if (!price || *price < Money{}) return std::string("No price");
  BrokerFill fill;
  fill.time = time;
  fill.symbol = *symbol;
  fill.side = *side;
  fill.quantity = *quantity < 0 ? -*quantity : *quantity;
  fill.price = *price < Money{} ? -*price : *price;
  return fill;
}

void parse_generic(const std::vector<Row>& rows, BrokerImport& out) {
  const Header header(rows.front());
  const auto time = header.find({"time", "timestamp", "datetime", "date_time", "executed_at", "date"});
  const auto symbol = header.find({"symbol", "osi", "contract", "occ_symbol"});
  const auto side = header.find({"side", "action", "buy/sell"});
  const auto quantity = header.find({"quantity", "qty", "contracts"});
  const auto price = header.find({"price", "fill_price", "trade_price"});
  if (!time || !symbol || !quantity || !price)
    throw std::invalid_argument("A generic import needs time, symbol, side, quantity and price columns");
  const auto bid = header.find({"bid"});
  const auto ask = header.find({"ask"});
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    if (row.blank()) continue;
    const auto when = time_cell(cell(row, time));
    if (!when) { out.skipped.emplace_back(row.line, "Unreadable time"); continue; }
    auto result = finish(symbol_cell(cell(row, symbol)), side_cell(cell(row, side)), integer_cell(cell(row, quantity)),
                         money_cell(cell(row, price)), *when);
    if (auto* fill = std::get_if<BrokerFill>(&result)) {
      fill->bid = money_cell(cell(row, bid));
      fill->ask = money_cell(cell(row, ask));
    }
    add_row(out, row.line, std::move(result));
  }
}
/// tastytrade's transaction history: trades only, priced from Value per
/// contract (Value / (Quantity * Multiplier)), which excludes commissions and fees.
void parse_tastytrade(const std::vector<Row>& rows, BrokerImport& out) {
  const Header header(rows.front());
  const auto date = header.find({"date"});
  const auto type = header.find({"type"});
  const auto action = header.find({"action"});
  const auto symbol = header.find({"symbol"});
  const auto instrument = header.find({"instrument type"});
  const auto value = header.find({"value"});
  const auto quantity = header.find({"quantity"});
  const auto multiplier = header.find({"multiplier"});
  if (!date || !action || !symbol || !value || !quantity)
    throw std::invalid_argument("A tastytrade import needs its Date, Action, Symbol, Value and Quantity columns");
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    if (row.blank()) continue;
    if (type && lower(cell(row, type)) != "trade") { out.skipped.emplace_back(row.line, "Not a trade"); continue; }
    if (instrument && lower(cell(row, instrument)).find("option") == std::string::npos) {
      out.skipped.emplace_back(row.line, "Not an option");
      continue;
    }
    const auto when = time_cell(cell(row, date));
    if (!when) { out.skipped.emplace_back(row.line, "Unreadable time"); continue; }
    const auto contracts = integer_cell(cell(row, quantity));
    const auto per = multiplier ? integer_cell(cell(row, multiplier)) : std::optional<std::int64_t>(100);
    const auto total = money_cell(cell(row, value));
    std::optional<Money> price;
    if (contracts && *contracts != 0 && per && *per > 0 && total)
      price = (*total < Money{} ? -*total : *total).prorate(1, (*contracts < 0 ? -*contracts : *contracts) * *per);
    add_row(out, row.line, finish(symbol_cell(cell(row, symbol)), side_cell(cell(row, action)), contracts, price, *when));
  }
}
/// thinkorswim's account statement, Account Trade History section: a spread's
/// later legs leave Exec Time empty and share the first leg's.
void parse_thinkorswim(const std::vector<Row>& rows, std::size_t start, BrokerImport& out) {
  const Header header(rows[start]);
  const auto time = header.find({"exec time"});
  const auto side = header.find({"side"});
  const auto quantity = header.find({"qty"});
  const auto symbol = header.find({"symbol"});
  const auto expiry = header.find({"exp"});
  const auto strike = header.find({"strike"});
  const auto type = header.find({"type"});
  const auto price = header.find({"price"});
  if (!time || !side || !quantity || !symbol || !expiry || !strike || !type || !price)
    throw std::invalid_argument("A thinkorswim Account Trade History needs Exec Time, Side, Qty, Symbol, Exp, Strike, Type and Price");
  std::optional<Timestamp> last;
  for (std::size_t i = start + 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    if (row.blank()) break;
    if (!cell(row, time).empty()) last = time_cell(cell(row, time));
    if (!last) { out.skipped.emplace_back(row.line, "Unreadable time"); continue; }
    const auto parts = words(cell(row, expiry));
    std::optional<std::string> osi;
    if (parts.size() == 3 && month_number(parts[1]) > 0) {
      const auto right = right_cell(cell(row, type));
      const auto at = strike_cell(cell(row, strike));
      if (right && at)
        osi = osi_of(cell(row, symbol), {2000 + std::atoi(parts[2].c_str()), month_number(parts[1]), std::atoi(parts[0].c_str())}, *right, *at);
    }
    add_row(out, row.line, finish(osi, side_cell(cell(row, side)), integer_cell(cell(row, quantity)), money_cell(cell(row, price)), *last));
  }
}
/// Schwab's transaction export: dates without times, so its rows load but cannot
/// be compared with a quote.
void parse_schwab_transactions(const std::vector<Row>& rows, BrokerImport& out) {
  const Header header(rows.front());
  const auto date = header.find({"date"});
  const auto action = header.find({"action"});
  const auto symbol = header.find({"symbol"});
  const auto quantity = header.find({"quantity"});
  const auto price = header.find({"price"});
  if (!date || !action || !symbol || !quantity || !price)
    throw std::invalid_argument("A Schwab transaction export needs Date, Action, Symbol, Quantity and Price columns");
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    if (row.blank()) continue;
    const auto side = side_cell(cell(row, action));
    if (!side) { out.skipped.emplace_back(row.line, "Not a buy or a sale"); continue; }
    add_row(out, row.line, finish(symbol_cell(cell(row, symbol)), side, integer_cell(cell(row, quantity)),
                                  money_cell(cell(row, price)), 0));
  }
}
/// IBKR Flex Query trades (Symbol, DateTime or TradeDate and TradeTime, Buy/Sell,
/// Quantity, TradePrice), options only when AssetClass is given.
void parse_ibkr_flex(const std::vector<Row>& rows, BrokerImport& out) {
  const Header header(rows.front());
  const auto symbol = header.find({"symbol"});
  const auto description = header.find({"description"});
  const auto when = header.find({"datetime", "date/time"});
  const auto trade_date = header.find({"tradedate"});
  const auto trade_time = header.find({"tradetime"});
  const auto side = header.find({"buy/sell"});
  const auto quantity = header.find({"quantity"});
  const auto price = header.find({"tradeprice", "t. price"});
  const auto asset = header.find({"assetclass"});
  const auto underlying = header.find({"underlyingsymbol"});
  const auto expiry = header.find({"expiry"});
  const auto strike = header.find({"strike"});
  const auto right = header.find({"put/call"});
  if (!symbol || (!when && !trade_date) || !quantity || !price)
    throw std::invalid_argument("An IBKR Flex import needs Symbol, DateTime (or TradeDate), Quantity and TradePrice columns");
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const auto& row = rows[i];
    if (row.blank()) continue;
    if (asset && cell(row, asset) != "OPT") { out.skipped.emplace_back(row.line, "Not an option"); continue; }
    const auto time = when ? time_cell(cell(row, when))
                           : time_cell(std::string(cell(row, trade_date)) + " " + std::string(cell(row, trade_time)));
    if (!time) { out.skipped.emplace_back(row.line, "Unreadable time"); continue; }
    auto osi = symbol_cell(cell(row, symbol));
    if (!osi) osi = symbol_cell(cell(row, description));
    if (!osi && underlying && expiry && strike && right) {
      const auto date = cell(row, expiry);
      const auto at = strike_cell(cell(row, strike));
      const auto r = right_cell(cell(row, right));
      if (date.size() == 8 && at && r)
        osi = osi_of(cell(row, symbol).substr(0, cell(row, symbol).find(' ')),
                     {std::atoi(std::string(date.substr(0, 4)).c_str()), std::atoi(std::string(date.substr(4, 2)).c_str()),
                      std::atoi(std::string(date.substr(6, 2)).c_str())}, *r, *at);
    }
    add_row(out, row.line, finish(osi, side ? side_cell(cell(row, side)) : std::nullopt, integer_cell(cell(row, quantity)),
                                  money_cell(cell(row, price)), *time));
  }
}
/// IBKR's activity statement: "Trades,Header,..." then "Trades,Data,Order,..." rows,
/// options under the "Equity and Index Options" asset category.
void parse_ibkr_statement(const std::vector<Row>& rows, BrokerImport& out) {
  std::optional<Header> header;
  for (const auto& row : rows) {
    if (row.cells.size() < 2 || trim(row.cells[0]) != "Trades") continue;
    if (trim(row.cells[1]) == "Header") { header.emplace(row); continue; }
    if (!header || trim(row.cells[1]) != "Data") continue;
    if (const auto kind = header->find({"datadiscriminator"}); kind && cell(row, kind) != "Order") continue;
    const auto category = cell(row, header->find({"asset category"}));
    if (lower(category).find("option") == std::string::npos) { out.skipped.emplace_back(row.line, "Not an option"); continue; }
    const auto time = time_cell(cell(row, header->find({"date/time"})));
    if (!time) { out.skipped.emplace_back(row.line, "Unreadable time"); continue; }
    add_row(out, row.line, finish(symbol_cell(cell(row, header->find({"symbol"}))), std::nullopt,
                                  integer_cell(cell(row, header->find({"quantity"}))),
                                  money_cell(cell(row, header->find({"t. price", "tradeprice"}))), *time));
  }
  if (!header) throw std::invalid_argument("An IBKR activity statement needs its Trades section");
}
}  // namespace

std::string_view to_string(FillPlace place) noexcept {
  switch (place) {
    case FillPlace::MidOrBetter: return "mid_or_better";
    case FillPlace::MidPlusOneTick: return "mid_plus_one_tick";
    case FillPlace::Inside: return "inside";
    case FillPlace::Natural: return "natural";
    case FillPlace::BeyondNatural: return "beyond_natural";
  }
  return "natural";
}

std::optional<FillPosition> fill_position(std::string_view root, Side side, Money price, std::optional<Money> bid,
                                          std::optional<Money> ask) {
  if (!bid || !ask || *bid <= Money{} || *ask < *bid) return std::nullopt;
  const bool buy = side == Side::Buy;
  FillPosition p;
  p.mid = *bid + (*ask - *bid).prorate(1, 2);
  p.natural = buy ? *ask : *bid;
  p.tick = tick_size(root, p.natural);
  const auto adverse = buy ? price - p.mid : p.mid - price;
  const auto half = buy ? p.natural - p.mid : p.mid - p.natural;
  p.place = adverse == half ? FillPlace::Natural
      : adverse > half ? FillPlace::BeyondNatural
      : adverse <= Money{} ? FillPlace::MidOrBetter
      : adverse <= p.tick ? FillPlace::MidPlusOneTick : FillPlace::Inside;
  const auto tick = static_cast<double>(p.tick.micros());
  p.from_mid_ticks = static_cast<double>(adverse.micros()) / tick;
  p.vs_natural_ticks = static_cast<double>((adverse - half).micros()) / tick;
  const auto width = *ask - *bid;
  p.fraction = width == Money{} ? 1.0
      : static_cast<double>((buy ? price - *bid : *ask - price).micros()) / static_cast<double>(width.micros());
  p.spread_ticks = std::llround(static_cast<double>(width.micros()) / tick);
  return p;
}

void FillQualityGroup::add(const FillPosition& position, Quantity quantity, double multiplier) {
  const auto before = static_cast<double>(contracts);
  const auto weight = static_cast<double>(quantity);
  from_mid_ticks = (from_mid_ticks.value_or(0) * before + position.from_mid_ticks * weight) / (before + weight);
  vs_natural_ticks = (vs_natural_ticks.value_or(0) * before + position.vs_natural_ticks * weight) / (before + weight);
  ++fills;
  contracts += quantity;
  places[static_cast<std::size_t>(position.place)] += quantity;
  const auto adverse = Money::from_micros(static_cast<std::int64_t>(std::llround(position.from_mid_ticks * static_cast<double>(position.tick.micros()))));
  cost_vs_mid = cost_vs_mid + adverse * (quantity * std::llround(multiplier));
}

FillQualityReport fill_quality(const SharedVector<Fill>& fills, const Contracts& contracts, std::uint64_t first_fill) {
  FillQualityReport report;
  const auto group = [](std::string key, std::string label) {
    FillQualityGroup g;
    g.key = std::move(key);
    g.label = std::move(label);
    return g;
  };
  const auto groups = [&](std::initializer_list<std::pair<const char*, const char*>> items) {
    std::vector<FillQualityGroup> out;
    for (const auto& [key, label] : items) out.push_back(group(key, label));
    return out;
  };
  report.overall = group("all", "All fills");
  report.time_of_day = groups({{"before_open", "Before 09:30"}, {"open", "09:30-10:00"}, {"morning", "10:00-12:00"},
                               {"midday", "12:00-14:00"}, {"afternoon", "14:00-15:30"}, {"close", "15:30-16:15"},
                               {"after_close", "After 16:15"}});
  report.spread = groups({{"1", "1 tick"}, {"2", "2 ticks"}, {"3-4", "3-4 ticks"}, {"5-9", "5-9 ticks"}, {"10+", "10 or more ticks"}});
  report.dte = groups({{"0", "0 DTE"}, {"1", "1 DTE"}, {"2-7", "2-7 DTE"}, {"8-30", "8-30 DTE"}, {"31+", "31+ DTE"}});
  Quantity stressed = 0, deep = 0;
  for (const auto& fill : fills) {
    if (fill.id < first_fill) continue;
    ++report.fills;
    const auto contract = contracts.find(fill.symbol);
    const auto position = contract == contracts.end() || !fill.quote ? std::nullopt
        : fill_position(contract->second.root, fill.side, fill.price, fill.quote->bid, fill.quote->ask);
    if (!position) { ++report.unmeasured; continue; }
    const auto multiplier = contract->second.multiplier;
    report.overall.add(*position, fill.quantity, multiplier);
    const auto local = md::new_york_time(fill.time);
    const auto minute = local.seconds / 60;
    const std::size_t clock = minute < 570 ? 0 : minute < 600 ? 1 : minute < 720 ? 2 : minute < 840 ? 3 : minute < 930 ? 4 : minute < 975 ? 5 : 6;
    report.time_of_day[clock].add(*position, fill.quantity, multiplier);
    const auto width = position->spread_ticks;
    const std::size_t spread = width <= 1 ? 0 : width == 2 ? 1 : width <= 4 ? 2 : width <= 9 ? 3 : 4;
    report.spread[spread].add(*position, fill.quantity, multiplier);
    const auto days = md::days_since_epoch(contract->second.expiry) - md::days_since_epoch(local.date);
    const std::size_t dte = days <= 0 ? 0 : days == 1 ? 1 : days <= 7 ? 2 : days <= 30 ? 3 : 4;
    report.dte[dte].add(*position, fill.quantity, multiplier);
    if (spread >= 3 || clock == 1 || clock == 5 || dte == 0) stressed += fill.quantity;
    // It took all the displayed size the account had left on that quote.
    if (fill.quote->left <= fill.quantity) deep += fill.quantity;
  }
  auto& s = report.suggestion;
  s.basis = "heuristic";
  s.fill_latency_ms = 1000;
  s.slippage_ticks = 1;
  s.impact_ticks = 1;
  s.reasons.push_back("Conservative's 1,000 ms latency, 1 slippage tick and 1 impact tick are the starting point.");
  const auto total = report.overall.contracts;
  if (total > 0 && 2 * stressed >= total) {
    s.slippage_ticks = 2;
    s.reasons.push_back("At least half of the contracts traded on spreads of 5 ticks or more, in the first or last half hour, "
                        "or at 0 DTE, where real fills tend to slip further: 2 slippage ticks.");
  }
  if (total > 0 && 4 * deep >= total) {
    s.impact_ticks = 2;
    s.reasons.push_back("At least a quarter of the contracts used up the displayed size left on their quote: 2 impact ticks.");
  }
  if (total == 0) s.reasons.push_back("No measured fills yet; the suggestion is Conservative unchanged.");
  s.reasons.push_back("A rule of thumb from these paper fills, not a measurement of real ones: import broker fills to calibrate from data.");
  s.fill_model = s.slippage_ticks == 1 && s.impact_ticks == 1 ? "conservative" : "custom";
  return report;
}

BrokerImport parse_broker_fills(std::string_view csv, std::string_view format) {
  static constexpr std::array<std::string_view, 5> formats{"auto", "generic", "tastytrade", "schwab", "ibkr"};
  if (std::find(formats.begin(), formats.end(), format) == formats.end())
    throw std::invalid_argument("format must be auto, generic, tastytrade, schwab or ibkr");
  auto rows = csv_rows(csv);
  // Blank lines end a thinkorswim section, so only leading ones go.
  while (!rows.empty() && rows.front().blank()) rows.erase(rows.begin());
  if (rows.empty()) throw std::invalid_argument("The file has no rows");
  BrokerImport out;
  // thinkorswim's statement holds several sections; its trades follow an Exec Time header.
  std::optional<std::size_t> history;
  for (std::size_t i = 0; i < rows.size(); ++i)
    if (Header(rows[i]).has({"exec time"})) { history = i; break; }
  const bool statement = std::any_of(rows.begin(), rows.end(), [](const Row& row) {
    return row.cells.size() >= 2 && trim(row.cells[0]) == "Trades" && trim(row.cells[1]) == "Header";
  });
  const Header first(rows.front());
  std::string detected(format);
  if (format == "auto") {
    if (history) detected = "schwab";
    else if (statement || first.has({"tradeprice", "buy/sell"})) detected = "ibkr";
    else if (first.has({"instrument type", "root symbol"})) detected = "tastytrade";
    else if (first.has({"fees & comm"})) detected = "schwab";
    else detected = "generic";
  }
  out.format = detected;
  if (detected == "generic") parse_generic(rows, out);
  else if (detected == "tastytrade") parse_tastytrade(rows, out);
  else if (detected == "schwab") {
    if (history) parse_thinkorswim(rows, *history, out);
    else parse_schwab_transactions(rows, out);
  } else if (statement) parse_ibkr_statement(rows, out);
  else parse_ibkr_flex(rows, out);
  return out;
}

namespace {
/// A limit at `limit` for `quantity` contracts on `quote`, with `slippage` adverse
/// ticks and one impact tick per displayed-size block when `impact` (Conservative),
/// or inside fills at `inside` percent (Inside at midpoint).
PaperOutcome paper_outcome(std::string_view root, Side side, Quantity quantity, Money limit, const CalibrationQuote& quote,
                           std::int64_t slippage, bool impact, std::int64_t inside) {
  const bool buy = side == Side::Buy;
  const auto far = buy ? quote.ask : quote.bid;
  const auto size = buy ? quote.ask_size : quote.bid_size;
  const auto tick = tick_size(root, far);
  // Without impact a fill stays within the displayed size; an unknown size limits nothing.
  const bool within = size <= 0 || quantity <= size;
  Money total;
  for (Quantity k = 0; k < quantity; ++k) {
    const auto blocks = impact && size > 0 ? k / size : 0;
    const auto slip = tick * (slippage + blocks);
    const auto price = buy ? far + slip : std::max(Money{}, far - slip);
    if (buy ? price > limit : price < limit) {
      // Not marketable: an inside limit fills at itself when far enough across the spread.
      if (k == 0 && inside > 0 && within) {
        const auto near = buy ? quote.bid : quote.ask;
        __extension__ using Wide = __int128;
        const auto reach = buy ? (static_cast<Wide>(limit.micros()) - near.micros()) * 100 >=
                                     (static_cast<Wide>(far.micros()) - near.micros()) * inside
                               : (static_cast<Wide>(near.micros()) - limit.micros()) * 100 >=
                                     (static_cast<Wide>(near.micros()) - far.micros()) * inside;
        if ((buy ? limit > near && limit < far : limit < near && limit > far) && reach) return {true, limit};
      }
      return {};
    }
    total = total + price;
  }
  if (!impact && !within) return {};
  return {true, total.prorate(1, quantity)};
}
}  // namespace

CalibrationReport calibrate(const BrokerImport& import, const std::map<std::size_t, CalibrationQuote>& quotes) {
  CalibrationReport report;
  report.format = import.format;
  report.skipped = import.skipped;
  report.real.key = "real";
  report.real.label = "Real fills";
  std::vector<std::pair<double, Quantity>> beyond, fractions;
  Quantity contracts = 0, larger = 0;
  for (const auto& fill : import.fills) {
    CalibrationRow row{fill, {}, {}, {}, {}, {}, {}};
    const auto contract = md::parse_osi(fill.symbol);
    // The file's own quote at the fill comes first: it is what the broker saw.
    if (fill.bid && fill.ask) row.quote = CalibrationQuote{*fill.bid, *fill.ask, 0, 0, fill.time, "import"};
    else if (const auto found = quotes.find(fill.row); found != quotes.end()) row.quote = found->second;
    if (fill.time == 0) row.note = "The export gives no execution time, so no quote can be matched";
    else if (!row.quote) row.note = "No recorded quote within the age limit before this fill";
    if (row.quote && contract) row.real = fill_position(contract->root, fill.side, fill.price, row.quote->bid, row.quote->ask);
    if (row.quote && !row.real) row.note = "The quote at that time was not two-sided";
    if (row.real) {
      ++report.matched;
      const auto& q = *row.quote;
      row.as_displayed = paper_outcome(contract->root, fill.side, fill.quantity, fill.price, q, 0, false, 0);
      row.conservative = paper_outcome(contract->root, fill.side, fill.quantity, fill.price, q, 1, true, 0);
      row.midpoint = paper_outcome(contract->root, fill.side, fill.quantity, fill.price, q, 0, false, 50);
      report.paper_fills[0] += row.as_displayed.fills;
      report.paper_fills[1] += row.conservative.fills;
      report.paper_fills[2] += row.midpoint.fills;
      report.real.add(*row.real, fill.quantity, contract->multiplier);
      beyond.emplace_back(row.real->vs_natural_ticks, fill.quantity);
      if (row.real->vs_natural_ticks < 0) fractions.emplace_back(row.real->fraction, fill.quantity);
      contracts += fill.quantity;
      const auto size = fill.side == Side::Buy ? q.ask_size : q.bid_size;
      if (size > 0 && fill.quantity > size) larger += fill.quantity;
    } else {
      ++report.unmatched;
    }
    report.rows.push_back(std::move(row));
  }
  auto& s = report.suggestion;
  s.basis = "real_fills";
  if (report.matched == 0) {
    s.fill_model = "as_displayed";
    s.reasons.push_back("No real fill matched a quote, so nothing is measured yet; record market data on the days you trade, or "
                        "include bid and ask columns in a generic import.");
    return report;
  }
  const auto median = weighted_median(beyond);
  if (median >= 0.5) {
    s.slippage_ticks = std::clamp<std::int64_t>(std::llround(median), 1, 10);
    s.reasons.push_back("Half of your real contracts filled at least " + std::to_string(s.slippage_ticks) +
                        " tick(s) beyond the natural: that many slippage ticks.");
  } else if (median < 0 && 2 * fractions.size() >= beyond.size()) {
    s.inside_fill_percent = std::clamp<std::int64_t>(std::llround(weighted_median(fractions) * 100), 1, 100);
    s.reasons.push_back("Most real contracts filled inside the spread, half of those at least " + std::to_string(s.inside_fill_percent) +
                        "% of the way from your own side to the far side: inside fills at that percent.");
  } else {
    s.reasons.push_back("Real fills typically came at the natural: as displayed matches them.");
  }
  if (contracts > 0 && 4 * larger >= contracts) {
    s.impact_ticks = 1;
    s.reasons.push_back("At least a quarter of the real contracts were in fills larger than the displayed size: 1 impact tick.");
  }
  s.reasons.push_back("Broker exports carry no order-entry time, so fill latency is not measured; keep your current setting.");
  s.fill_model = s.slippage_ticks == 0 && s.impact_ticks == 0 ? (s.inside_fill_percent == 0 ? "as_displayed" : s.inside_fill_percent == 50 ? "midpoint" : "custom")
      : s.slippage_ticks == 1 && s.impact_ticks == 1 && s.inside_fill_percent == 0 ? "conservative" : "custom";
  return report;
}

}  // namespace openport::trading
