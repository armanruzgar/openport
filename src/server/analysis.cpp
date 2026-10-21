#include "openport/server/analysis.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <locale>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>

namespace openport::server {
namespace {
using nlohmann::json;
using Row = std::vector<std::string>;

/// RFC 4180 rows: quoted fields may hold commas, quotes ("") and line breaks.
std::vector<Row> csv_rows(std::string_view text) {
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  std::vector<Row> rows;
  Row row;
  std::string field;
  bool quoted = false, any = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quoted) {
      if (c == '"' && i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
      else if (c == '"') quoted = false;
      else field += c;
    } else if (c == '"') {
      quoted = true;
      any = true;
    } else if (c == ',') {
      row.push_back(std::move(field));
      field.clear();
      any = true;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
      if (any || !field.empty()) row.push_back(std::move(field));
      rows.push_back(std::move(row));
      row.clear();
      field.clear();
      any = false;
    } else {
      field += c;
      any = true;
    }
  }
  if (quoted) throw std::invalid_argument("the file ends inside a quoted field");
  if (any || !field.empty()) row.push_back(std::move(field));
  if (!row.empty()) rows.push_back(std::move(row));
  return rows;
}

std::string trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return std::string(s);
}
/// "Qty (Quantity)" -> "qtyquantity", "Call/Put" -> "callput".
std::string key(std::string_view header) {
  std::string out;
  for (const char c : header)
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}
std::string upper(std::string text) {
  for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return text;
}

/// "$1,234.50", "(12)", "-3", "+2" -> numbers; nullopt for blanks, "--" and text.
std::optional<double> amount(std::string_view raw) {
  auto text = trim(raw);
  bool negative = false;
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')') { negative = true; text = text.substr(1, text.size() - 2); }
  std::string clean;
  for (const char c : text) if (c != '$' && c != ',' && c != ' ') clean += c;
  if (clean.empty() || clean == "--" || clean == "N/A") return std::nullopt;
  std::istringstream in(clean);
  in.imbue(std::locale::classic());
  double value = 0;
  in >> std::noskipws >> value;
  if (!in || in.peek() != std::char_traits<char>::eof() || !std::isfinite(value)) return std::nullopt;
  return negative ? -value : value;
}

std::optional<md::OptionContract> option_of(std::string root, md::Date expiry, double strike, char right) {
  while (!root.empty() && (root.front() == '$' || root.front() == '.' || root.front() == '/')) root.erase(root.begin());
  if (root.empty() || root.size() > 6 || !(strike > 0) || strike >= 100000 || (right != 'C' && right != 'P')) return std::nullopt;
  const auto milli = static_cast<long long>(std::llround(strike * 1000));
  if (std::abs(static_cast<double>(milli) - strike * 1000) > 1e-6) return std::nullopt;
  char osi[32];
  std::snprintf(osi, sizeof osi, "%-6s%02d%02d%02d%c%08lld", root.c_str(), expiry.year % 100, expiry.month, expiry.day, right, milli);
  auto contract = md::parse_osi(osi);
  if (contract && contract->expiry != expiry) return std::nullopt;
  return contract;
}

char right_of(std::string_view text) {
  const auto t = upper(trim(text));
  return t == "C" || t == "CALL" ? 'C' : t == "P" || t == "PUT" ? 'P' : '\0';
}

std::optional<md::Date> date_parts(std::string_view raw);
std::optional<md::Date> date_of(std::string_view raw) {
  const auto date = date_parts(raw);
  if (!date || date->year < 2000 || date->year > 2099 || date->month < 1 || date->month > 12 || date->day < 1 || date->day > 31)
    return std::nullopt;
  return date;
}
std::optional<md::Date> date_parts(std::string_view raw) {
  const auto text = trim(raw);
  int a = 0, b = 0, c = 0;
  char tail = 0;
  if (std::sscanf(text.c_str(), "%4d-%2d-%2d%c", &a, &b, &c, &tail) == 3 && text.size() == 10) return md::Date{a, b, c};
  if (std::sscanf(text.c_str(), "%2d/%2d/%4d%c", &a, &b, &c, &tail) == 3) return md::Date{c, a, b};
  if (text.size() == 8 && std::all_of(text.begin(), text.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
    return md::Date{std::stoi(text.substr(0, 4)), std::stoi(text.substr(4, 2)), std::stoi(text.substr(6, 2))};
  // IBKR's 18DEC26
  static constexpr std::string_view months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  const auto t = upper(text);
  if (t.size() == 7 && std::isdigit(static_cast<unsigned char>(t[0])) && std::isdigit(static_cast<unsigned char>(t[1])))
    for (int m = 0; m < 12; ++m)
      if (t.substr(2, 3) == months[m] && std::isdigit(static_cast<unsigned char>(t[5])) && std::isdigit(static_cast<unsigned char>(t[6])))
        return md::Date{2000 + std::stoi(t.substr(5, 2)), m + 1, std::stoi(t.substr(0, 2))};
  return std::nullopt;
}

/// OSI, or "ROOT EXPIRY STRIKE C|P" with a Schwab or IBKR date.
std::optional<md::OptionContract> option_symbol(std::string_view raw) {
  const auto text = trim(raw);
  if (auto osi = md::parse_osi(text)) return osi;
  std::istringstream in(text);
  std::vector<std::string> parts;
  for (std::string part; in >> part;) parts.push_back(part);
  if (parts.size() != 4) return std::nullopt;
  const auto expiry = date_of(parts[1]);
  const auto strike = amount(parts[2]);
  if (!expiry || !strike) return std::nullopt;
  return option_of(upper(parts[0]), *expiry, *strike, right_of(parts[3]));
}

bool ticker(std::string_view text) {
  return !text.empty() && text.size() <= 12 && std::all_of(text.begin(), text.end(), [](char c) {
    return std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '/';
  });
}

/// One layout: the header row's index and its columns by key.
struct Table {
  std::size_t header = 0;
  std::map<std::string, std::size_t> columns;
  [[nodiscard]] std::optional<std::size_t> column(std::initializer_list<std::string_view> names) const {
    for (const auto name : names)
      if (const auto it = columns.find(std::string(name)); it != columns.end()) return it->second;
    return std::nullopt;
  }
};

Table table_at(const std::vector<Row>& rows, std::size_t index, std::size_t first = 0) {
  Table table;
  table.header = index;
  for (std::size_t i = first; i < rows[index].size(); ++i) table.columns.emplace(key(rows[index][i]), i);
  return table;
}

/// The first row that holds every one of `required` (as keys).
std::optional<Table> find_header(const std::vector<Row>& rows, std::initializer_list<std::initializer_list<std::string_view>> required) {
  for (std::size_t r = 0; r < rows.size() && r < 20; ++r) {
    const auto table = table_at(rows, r);
    if (std::all_of(required.begin(), required.end(), [&](const auto& names) { return table.column(names).has_value(); }))
      return table;
  }
  return std::nullopt;
}

const std::initializer_list<std::string_view> kSymbol{"symbol"};
const std::initializer_list<std::string_view> kQuantity{"quantity", "qtyquantity", "qty", "position"};
const std::initializer_list<std::string_view> kCost{"cost", "costprice", "averageprice", "avgprice", "averageopenprice", "tradeprice", "costbasisprice", "price"};

struct Sink {
  AnalysisPortfolio& out;
  std::map<std::string, std::size_t> index;
  void option(const md::OptionContract& contract, double quantity, std::optional<double> cost, const std::string& source) {
    add(contract.osi_symbol(), contract, quantity, cost, source);
  }
  void shares(const std::string& symbol, double quantity, std::optional<double> cost, const std::string& source) {
    add(symbol, std::nullopt, quantity, cost, source);
  }
  void add(const std::string& symbol, std::optional<md::OptionContract> contract, double quantity,
           std::optional<double> cost, const std::string& source) {
    if (!std::isfinite(quantity) || std::abs(quantity - std::round(quantity)) > 1e-9 || std::abs(quantity) > 1e9)
      throw std::invalid_argument(source + ": quantity must be a whole number");
    const auto whole = static_cast<trading::Quantity>(std::llround(quantity));
    if (cost && (!std::isfinite(*cost) || *cost < 0)) cost.reset();
    const auto [it, inserted] = index.emplace(symbol, out.holdings.size());
    if (inserted) {
      if (out.holdings.size() == kAnalysisMaxHoldings) throw std::invalid_argument("more than 2000 holdings");
      out.holdings.push_back({symbol, std::move(contract), whole, cost, source});
      return;
    }
    auto& held = out.holdings[it->second];
    // Several lots of one symbol: their quantity-weighted cost, when every lot has one.
    if (held.cost && cost && held.quantity + whole != 0)
      held.cost = (*held.cost * static_cast<double>(held.quantity) + *cost * static_cast<double>(whole)) /
                  static_cast<double>(held.quantity + whole);
    else held.cost.reset();
    held.quantity += whole;
  }
};

std::string cell(const Row& row, std::optional<std::size_t> column) {
  return column && *column < row.size() ? trim(row[*column]) : std::string{};
}

void parse_generic(const std::vector<Row>& rows, Sink& sink) {
  auto table = find_header(rows, {kSymbol, kQuantity});
  const bool split = !table;
  if (!table) table = find_header(rows, {{"underlying"}, {"expiry", "expiration"}, {"strike"}, {"type", "right", "callput"}, kQuantity});
  if (!table) throw std::invalid_argument("generic CSV needs symbol and quantity columns, or underlying, expiry, strike, type and quantity");
  for (std::size_t r = table->header + 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    if (std::all_of(row.begin(), row.end(), [](const std::string& c) { return trim(c).empty(); })) continue;
    const auto where = "line " + std::to_string(r + 1);
    const auto quantity = amount(cell(row, table->column(kQuantity)));
    if (!quantity) throw std::invalid_argument(where + ": quantity is not a number");
    const auto cost = amount(cell(row, table->column(kCost)));
    if (split) {
      const auto expiry = date_of(cell(row, table->column({"expiry", "expiration"})));
      const auto strike = amount(cell(row, table->column({"strike"})));
      const auto contract = expiry && strike ? option_of(upper(cell(row, table->column({"underlying"}))), *expiry, *strike,
          right_of(cell(row, table->column({"type", "right", "callput"})))) : std::nullopt;
      if (!contract) throw std::invalid_argument(where + ": not a listed option (underlying, YYYY-MM-DD expiry, strike, call or put)");
      sink.option(*contract, *quantity, cost, where);
      continue;
    }
    const auto symbol = cell(row, table->column(kSymbol));
    if (const auto contract = option_symbol(symbol)) sink.option(*contract, *quantity, cost, where + " " + symbol);
    else if (ticker(upper(symbol))) sink.shares(upper(symbol), *quantity, cost, where + " " + symbol);
    else throw std::invalid_argument(where + ": " + symbol + " is neither an OSI option symbol nor a ticker");
  }
}

void parse_tastytrade(const std::vector<Row>& rows, Sink& sink) {
  const auto table = find_header(rows, {kSymbol, {"type", "instrumenttype"}, kQuantity});
  if (!table) throw std::invalid_argument("tastytrade CSV needs Symbol, Type and Quantity columns");
  for (std::size_t r = table->header + 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    const auto symbol = cell(row, table->column(kSymbol));
    if (symbol.empty()) continue;
    const auto where = "line " + std::to_string(r + 1) + " " + symbol;
    const auto type = key(cell(row, table->column({"type", "instrumenttype"})));
    auto quantity = amount(cell(row, table->column(kQuantity)));
    if (!quantity) throw std::invalid_argument(where + ": quantity is not a number");
    // Some exports give an unsigned quantity and the side apart.
    const auto direction = key(cell(row, table->column({"direction", "side", "longshort"})));
    if (*quantity > 0 && (direction == "short" || direction == "sell")) quantity = -*quantity;
    const auto cost = amount(cell(row, table->column({"averageopenprice", "tradeprice", "costprice", "averageprice"})));
    if (type.find("option") != std::string::npos) {
      auto contract = option_symbol(symbol);
      if (!contract) {
        const auto expiry = date_of(cell(row, table->column({"expdate", "expirationdate", "expiration"})));
        const auto strike = amount(cell(row, table->column({"strikeprice", "strike"})));
        if (expiry && strike)
          contract = option_of(upper(cell(row, table->column({"underlyingsymbol", "underlying"}))), *expiry, *strike,
                               right_of(cell(row, table->column({"callput", "putcall"}))));
      }
      if (!contract) throw std::invalid_argument(where + ": cannot read the option");
      sink.option(*contract, *quantity, cost, where);
    } else if (type == "equity" || type == "stock") {
      sink.shares(upper(symbol), *quantity, cost, where);
    } else {
      sink.out.skipped.push_back(where + ": " + cell(row, table->column({"type", "instrumenttype"})) + " is not supported");
    }
  }
}

void parse_schwab(const std::vector<Row>& rows, Sink& sink) {
  const auto table = find_header(rows, {kSymbol, {"qtyquantity", "quantity", "qty"}});
  if (!table) throw std::invalid_argument("Schwab CSV needs Symbol and Qty (Quantity) columns");
  for (std::size_t r = table->header + 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    const auto symbol = cell(row, table->column(kSymbol));
    const auto where = "line " + std::to_string(r + 1) + " " + symbol;
    const auto type = key(cell(row, table->column({"securitytype", "assettype"})));
    const auto quantity = amount(cell(row, table->column({"qtyquantity", "quantity", "qty"})));
    if (symbol.empty()) continue;
    if (!quantity) {
      sink.out.skipped.push_back(where + ": no quantity (cash or a total)");
      continue;
    }
    // Cost Basis is the lot's total: per share it is divided by shares or by contracts x 100.
    const auto basis = amount(cell(row, table->column({"costbasis"})));
    if (const auto contract = option_symbol(symbol)) {
      const auto cost = basis && *quantity != 0 ? std::optional(std::abs(*basis / (*quantity * contract->multiplier))) : std::nullopt;
      sink.option(*contract, *quantity, cost, where);
    } else if (type.empty() || type.find("equity") != std::string::npos || type.find("etf") != std::string::npos ||
               type.find("stock") != std::string::npos) {
      if (!ticker(upper(symbol))) {
        sink.out.skipped.push_back(where + ": not a ticker");
        continue;
      }
      const auto cost = basis && *quantity != 0 ? std::optional(std::abs(*basis / *quantity)) : std::nullopt;
      sink.shares(upper(symbol), *quantity, cost, where);
    } else {
      sink.out.skipped.push_back(where + ": " + cell(row, table->column({"securitytype", "assettype"})) + " is not supported");
    }
  }
}

void parse_ibkr(const std::vector<Row>& rows, Sink& sink) {
  // An activity statement repeats a section name in its first column; a Flex query is one table.
  std::optional<Table> table;
  bool statement = false;
  for (std::size_t r = 0; r < rows.size(); ++r)
    if (rows[r].size() > 2 && trim(rows[r][0]) == "Open Positions" && trim(rows[r][1]) == "Header") {
      table = table_at(rows, r, 2);
      statement = true;
      break;
    }
  if (!table) table = find_header(rows, {kSymbol, kQuantity, {"assetclass", "assetcategory"}});
  if (!table) throw std::invalid_argument("IBKR CSV needs an Open Positions section, or Symbol, Quantity and AssetClass columns");
  for (std::size_t r = table->header + 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    if (statement) {
      if (row.size() < 2 || trim(row[0]) != "Open Positions") continue;
      if (trim(row[1]) != "Data") continue;
      const auto discriminator = cell(row, table->column({"datadiscriminator"}));
      if (!discriminator.empty() && discriminator != "Summary") continue;  // lots repeat the summary
    }
    const auto symbol = cell(row, table->column(kSymbol));
    if (symbol.empty()) continue;
    const auto where = "line " + std::to_string(r + 1) + " " + symbol;
    const auto asset = key(cell(row, table->column({"assetclass", "assetcategory"})));
    const auto quantity = amount(cell(row, table->column(kQuantity)));
    if (!quantity) {
      sink.out.skipped.push_back(where + ": no quantity (a total)");
      continue;
    }
    const auto cost = amount(cell(row, table->column({"costprice", "costbasisprice", "averagecost"})));
    if (asset == "opt" || asset.find("option") != std::string::npos) {
      auto contract = option_symbol(symbol);
      if (!contract) {
        const auto expiry = date_of(cell(row, table->column({"expiry", "expiration"})));
        const auto strike = amount(cell(row, table->column({"strike"})));
        if (expiry && strike)
          contract = option_of(upper(cell(row, table->column({"underlyingsymbol"}))), *expiry, *strike,
                               right_of(cell(row, table->column({"putcall", "right"}))));
      }
      if (!contract) throw std::invalid_argument(where + ": cannot read the option");
      // IBKR's cost price is per contract share, as the other layouts' are.
      sink.option(*contract, *quantity, cost, where);
    } else if (asset == "stk" || asset.find("stock") != std::string::npos) {
      sink.shares(upper(symbol), *quantity, cost, where);
    } else {
      sink.out.skipped.push_back(where + ": " + cell(row, table->column({"assetclass", "assetcategory"})) + " is not supported");
    }
  }
}

std::string detect(const std::vector<Row>& rows) {
  for (const auto& row : rows)
    if (row.size() > 1 && trim(row[0]) == "Open Positions") return "ibkr";
  if (find_header(rows, {kSymbol, kQuantity, {"assetclass", "assetcategory"}})) return "ibkr";
  if (find_header(rows, {kSymbol, {"qtyquantity"}}) || find_header(rows, {kSymbol, kQuantity, {"securitytype"}})) return "schwab";
  if (find_header(rows, {kSymbol, {"type", "instrumenttype"}, kQuantity, {"callput", "expdate", "underlyingsymbol"}})) return "tastytrade";
  return "generic";
}

json holding_json(const AnalysisHolding& h) {
  return {{"symbol", h.symbol}, {"quantity", h.quantity}, {"cost", h.cost ? json(*h.cost) : json(nullptr)}, {"source", h.source}};
}

bool valid_id(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' && id.find("--") == std::string_view::npos &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
}  // namespace

AnalysisPortfolio parse_analysis_csv(std::string_view text, std::string_view format) {
  const auto rows = csv_rows(text);
  if (rows.empty()) throw std::invalid_argument("the file is empty");
  AnalysisPortfolio out;
  out.format = format == "auto" || format.empty() ? detect(rows) : std::string(format);
  Sink sink{out, {}};
  if (out.format == "generic") parse_generic(rows, sink);
  else if (out.format == "tastytrade") parse_tastytrade(rows, sink);
  else if (out.format == "schwab") parse_schwab(rows, sink);
  else if (out.format == "ibkr") parse_ibkr(rows, sink);
  else throw std::invalid_argument("format must be auto, generic, tastytrade, schwab or ibkr");
  std::erase_if(out.holdings, [](const AnalysisHolding& h) { return h.quantity == 0; });
  if (out.holdings.empty()) throw std::invalid_argument("no open positions found");
  return out;
}

AnalysisPortfolios::AnalysisPortfolios(std::filesystem::path directory) : directory_(std::move(directory)) {
  if (directory_.empty()) return;
  try {
    if (!std::filesystem::exists(directory_)) return;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      if (entry.path().extension() != ".json") continue;
      try {
        std::ifstream in(entry.path());
        const auto j = json::parse(in);
        AnalysisPortfolio p;
        p.id = j.at("id").get<std::string>();
        if (!valid_id(p.id) || p.id != entry.path().stem().string()) throw std::runtime_error("id does not match its file");
        p.name = j.at("name").get<std::string>();
        p.format = j.at("format").get<std::string>();
        p.imported = j.at("imported").get<md::Timestamp>();
        p.skipped = j.value("skipped", std::vector<std::string>{});
        for (const auto& h : j.at("holdings")) {
          AnalysisHolding holding;
          holding.symbol = h.at("symbol").get<std::string>();
          holding.contract = md::parse_osi(holding.symbol);
          holding.quantity = h.at("quantity").get<trading::Quantity>();
          if (!h.at("cost").is_null()) holding.cost = h.at("cost").get<double>();
          holding.source = h.value("source", "");
          p.holdings.push_back(std::move(holding));
        }
        portfolios_.emplace(p.id, std::move(p));
      } catch (const std::exception& e) {
        error_ = entry.path().string() + ": " + e.what();
      }
    }
  } catch (const std::exception& e) {
    error_ = "analysis: " + std::string(e.what());
  }
}

std::vector<AnalysisPortfolio> AnalysisPortfolios::list() const {
  const std::lock_guard lock(mutex_);
  std::vector<AnalysisPortfolio> out;
  for (const auto& [id, p] : portfolios_) out.push_back(p);
  return out;
}

std::optional<AnalysisPortfolio> AnalysisPortfolios::find(std::string_view id) const {
  const std::lock_guard lock(mutex_);
  const auto it = portfolios_.find(id);
  return it == portfolios_.end() ? std::nullopt : std::optional(it->second);
}

void AnalysisPortfolios::save(const AnalysisPortfolio& p) const {
  if (directory_.empty()) return;
  json holdings = json::array();
  for (const auto& h : p.holdings) holdings.push_back(holding_json(h));
  const json body{{"id", p.id}, {"name", p.name}, {"format", p.format}, {"imported", p.imported},
                  {"analysis_only", true}, {"holdings", holdings}, {"skipped", p.skipped}};
  std::filesystem::create_directories(directory_);
  const auto file = directory_ / (p.id + ".json");
  const auto temporary = directory_ / (p.id + ".json.tmp");
  {
    std::ofstream out(temporary, std::ios::trunc);
    out << body.dump(2) << '\n';
    if (!out.flush()) throw std::runtime_error("cannot write " + temporary.string());
  }
  std::filesystem::rename(temporary, file);
}

AnalysisPortfolio AnalysisPortfolios::add(AnalysisPortfolio portfolio, std::string_view name, md::Timestamp now) {
  const auto title = trim(name);
  if (title.empty() || title.size() > 64 || std::any_of(title.begin(), title.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    throw std::invalid_argument("name must be 1 to 64 characters without control characters");
  std::string base;
  for (const char c : title) {
    const auto lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) base += lower;
    else if (!base.empty() && base.back() != '-') base += '-';
    if (base.size() == 32) break;
  }
  while (!base.empty() && base.back() == '-') base.pop_back();
  if (base.empty()) base = "portfolio";
  const std::lock_guard lock(mutex_);
  if (portfolios_.size() >= kAnalysisMaxPortfolios) throw std::invalid_argument("at most 32 analysis portfolios; delete one first");
  auto id = base;
  for (int n = 2; portfolios_.contains(id); ++n) id = base + "-" + std::to_string(n);
  portfolio.id = id;
  portfolio.name = title;
  portfolio.imported = now;
  save(portfolio);
  portfolios_.emplace(id, portfolio);
  return portfolio;
}

bool AnalysisPortfolios::remove(std::string_view id) {
  const std::lock_guard lock(mutex_);
  const auto it = portfolios_.find(id);
  if (it == portfolios_.end()) return false;
  if (!directory_.empty()) std::filesystem::remove(directory_ / (it->first + ".json"));
  portfolios_.erase(it);
  return true;
}

std::string AnalysisPortfolios::error() const {
  const std::lock_guard lock(mutex_);
  return error_;
}

}  // namespace openport::server
