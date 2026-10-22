#include "openport/server/analysis.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <nlohmann/json.hpp>
#include <set>

#include "openport/trading/risk.hpp"
#include "openport/trading/stress.hpp"
#include "paper_json.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
using trading::Money;

constexpr std::string_view kNote =
    "Imported for analysis only: OpenPort never places orders for it, and it is not a paper account. "
    "Values use OpenPort's analytics of your configured feed.";

json number(double value) { return std::isfinite(value) ? json(std::round(value * 1e4) / 1e4) : json(nullptr); }
json dollars(double value) { return std::isfinite(value) ? json(std::round(value * 100) / 100) : json(nullptr); }

const analytics::OptionMetrics* find_option(const analytics::UnderlyingMetrics& m, const md::OptionContract& contract) {
  const auto symbol = contract.osi_symbol();
  for (const auto& slice : m.slices) {
    if (slice.expiry_time != contract.expiry_time() || slice.style != contract.style) continue;
    for (const auto& strike : slice.strikes) {
      const auto& option = contract.type == pricing::OptionType::Call ? strike.call : strike.put;
      if (option.id != analytics::kNoInstrument && option.contract.osi_symbol() == symbol) return &option;
    }
  }
  return nullptr;
}

std::vector<std::string> split(std::string_view text) {
  std::vector<std::string> out;
  for (std::size_t start = 0;;) {
    const auto comma = text.find(',', start);
    out.emplace_back(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    if (comma == std::string_view::npos) return out;
    start = comma + 1;
  }
}

std::optional<std::vector<double>> numbers(const std::string& text, std::size_t most, double limit) {
  std::vector<double> values;
  for (const auto& item : split(text)) {
    double value = 0;
    if (!query_decimal(item, value) || std::abs(value) > limit || values.size() == most) return std::nullopt;
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
  }
  std::sort(values.begin(), values.end());
  return values;
}

std::set<std::string> held_underlyings(const AnalysisBook& book) {
  std::set<std::string> held;
  for (const auto& [symbol, p] : book.ledger.positions()) held.insert(p.contract.underlying);
  for (const auto& [symbol, s] : book.ledger.stocks()) held.insert(symbol);
  return held;
}

json base(const AnalysisBook& book) {
  return {{"portfolio", book.portfolio->id}, {"name", book.portfolio->name}, {"analysis_only", true},
          {"read_only", true}, {"note", kNote},
          {"as_of", book.now > 0 ? json(md::format_timestamp(book.now)) : json(nullptr)},
          {"unpriced", book.unpriced}, {"complete", book.unpriced.empty()}};
}

/// The held book's value across moves of one underlying, or of a benchmark with
/// each underlying beta-weighted to it, as the paper account's risk profile.
ApiResponse profile_view(const AnalysisBook& book, const std::map<std::string, std::string>& query) {
  std::string underlying, benchmark = "SPY";
  std::vector<double> days{0, 1};
  double vol_points = 0, range = 10;
  int steps = 41;
  std::map<std::string, double> given;
  for (const auto& [key, value] : query) {
    bool valid = true;
    if (key == "underlying") underlying = value, valid = !value.empty() && value.size() <= 12;
    else if (key == "benchmark") benchmark = value, valid = value == "SPY" || value == "SPX";
    else if (key == "days") {
      const auto parsed = numbers(value, 4, 366);
      valid = parsed && std::all_of(parsed->begin(), parsed->end(), [](double d) { return d >= 0; });
      if (valid) days = *parsed;
    } else if (key == "iv") valid = query_decimal(value, vol_points) && std::abs(vol_points) <= 100;
    else if (key == "range") valid = query_decimal(value, range) && range > 0 && range <= 99;
    else if (key == "steps") {
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), steps);
      valid = ec == std::errc{} && end == value.data() + value.size() && steps >= 3 && steps <= 201 && steps % 2 == 1;
    } else if (key == "betas") {
      for (const auto& item : split(value)) {
        const auto colon = item.find(':');
        double beta = 0;
        valid = valid && colon != std::string::npos && query_decimal(item.substr(colon + 1), beta) && std::abs(beta) <= 10;
        if (valid) given[item.substr(0, colon)] = beta;
      }
    } else valid = false;
    if (!valid) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter " + key);
  }
  const auto& reference = underlying.empty() ? benchmark : underlying;
  trading::ProfileConfig config;
  config.vol_points = vol_points;
  for (int i = 0; i < steps; ++i) {
    const int offset = i - steps / 2;
    config.percent.push_back(offset == 0 ? 0.0 : range * offset / (steps / 2));
  }
  config.days = days;
  const auto metrics = book.source->metrics(reference);
  const double spot = metrics && metrics->spot > 0 && std::isfinite(metrics->spot) ? metrics->spot : 0.0;
  json betas = json::array();
  bool complete = book.unpriced.empty();
  std::set<std::string> weighted;
  for (const auto& symbol : held_underlyings(book)) {
    if (!underlying.empty() && symbol != underlying) continue;
    const auto beta = underlying.empty() ? profile_beta(*book.source, symbol, reference, given) : BetaEstimate{1, analytics::kNaN, 0, "reference"};
    // An underlying without a beta stays out of the curves; its beta reads null.
    if (beta.source.empty()) complete = false;
    else {
      config.betas[symbol] = beta.beta;
      weighted.insert(symbol);
    }
    betas.push_back({{"underlying", symbol}, {"beta", beta.source.empty() ? json(nullptr) : number(beta.beta)},
                     {"source", beta.source.empty() ? json(nullptr) : json(beta.source)}, {"observations", beta.observations}});
  }
  std::map<std::string, trading::Position> positions;
  std::map<std::string, trading::StockPosition> stocks;
  for (const auto& [symbol, p] : book.ledger.positions())
    if (weighted.contains(p.contract.underlying)) positions.emplace(symbol, p);
  for (const auto& [symbol, s] : book.ledger.stocks())
    if (weighted.contains(symbol)) stocks.emplace(symbol, s);
  if (positions.empty() && stocks.empty())
    return api_error(404, "NOT_FOUND", "Nothing valued in this portfolio moves with " + reference);
  const auto profile = trading::risk_profile(trading::Ledger::restore({}, std::move(positions), std::move(stocks)),
      book.valuations, config, spot, Money{}, std::nullopt, std::nullopt, book.now, book.max_age, book.stock_prices);
  complete = complete && profile.complete;
  json curves = json::array();
  for (const auto& curve : profile.curves) {
    json pnl = json::array();
    for (const double value : curve.pnl) pnl.push_back(profile.complete ? dollars(value) : json(nullptr));
    curves.push_back({{"days", curve.days}, {"time", md::format_timestamp(curve.time)}, {"pnl", pnl}, {"clamped", curve.clamped}});
  }
  json prices = json::array();
  for (const double percent : config.percent) prices.push_back(spot > 0 ? number(spot * (1 + percent / 100)) : json(nullptr));
  auto body = base(book);
  body.update({{"view", "profile"}, {"underlying", underlying.empty() ? json(nullptr) : json(underlying)},
               {"benchmark", underlying.empty() ? json(benchmark) : json(nullptr)}, {"reference", reference},
               {"spot", spot > 0 ? number(spot) : json(nullptr)}, {"vol_points", vol_points}, {"percent", config.percent},
               {"prices", prices}, {"curves", curves}, {"betas", betas}, {"complete", complete},
               {"model", "Black-76 on each contract's smile IV plus the offset, sticky strike; remaining life and carry shrink by "
                         "the days; each underlying moves its beta times the reference's percent move. P&L is from the book's "
                         "value now, not from its cost. Unpriced holdings and underlyings without a beta are left out."}});
  return {200, body.dump()};
}

/// P&L across spot and volatility shocks, as the paper account's scenario grid.
ApiResponse grid_view(const AnalysisBook& book, const std::map<std::string, std::string>& query) {
  trading::ScenarioConfig config;
  for (const auto& [key, value] : query) {
    std::optional<std::vector<double>> parsed;
    if (key == "spot") parsed = numbers(value, 15, 50);
    else if (key == "vol") parsed = numbers(value, 9, 100);
    if (!parsed || parsed->empty()) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter " + key);
    (key == "spot" ? config.spot_percent : config.vol_points) = *parsed;
  }
  const auto grid = trading::scenario_grid(book.ledger, book.valuations, config, book.now, book.max_age, book.stock_prices);
  const bool complete = grid.complete && book.unpriced.empty();
  json pnl = json::array(), clamped = json::array();
  std::size_t index = 0;
  for ([[maybe_unused]] double spot : config.spot_percent) {
    json row = json::array(), floors = json::array();
    for ([[maybe_unused]] double vol : config.vol_points) {
      const auto& cell = grid.cells.at(index++);
      row.push_back(grid.complete ? dollars(cell.pnl) : json(nullptr));
      floors.push_back(cell.clamped);
    }
    pnl.push_back(std::move(row));
    clamped.push_back(std::move(floors));
  }
  auto body = base(book);
  body.update({{"view", "grid"}, {"spot_percent", config.spot_percent}, {"vol_points", config.vol_points}, {"pnl", pnl},
               {"clamped", clamped}, {"complete", complete},
               {"model", "Every underlying moves by the same percent and every smile IV by the same points; Black-76 at "
                         "today's remaining life. P&L is from the book's value now; unpriced holdings are left out."}});
  return {200, body.dump()};
}

/// The risk branch's historical stress scenarios, applied to the valued holdings at
/// once. There is no account here, so no floor and no paper margin are reported.
ApiResponse stress_view(const AnalysisBook& book, const std::map<std::string, std::string>& query) {
  if (!query.empty()) return api_error(400, "INVALID_REQUEST", "This view takes no query parameters");
  const auto held = held_underlyings(book);
  json stress = json::array();
  for (const auto& scenario : trading::historical_stresses()) {
    const auto value = trading::stress_book(book.ledger, book.valuations, scenario.factors, 0, Money{}, std::nullopt,
        book.now, book.max_age, book.stock_prices);
    const bool complete = value.complete && book.unpriced.empty();
    json missing = json::array();
    for (const auto& symbol : held) if (!scenario.factors.contains(symbol)) missing.push_back(symbol);
    stress.push_back({{"id", scenario.id}, {"name", scenario.name}, {"from", scenario.from}, {"to", scenario.to},
        {"label", scenario.label}, {"source_note", scenario.source_note}, {"sources", scenario.sources},
        {"complete", complete}, {"missing_underlyings", missing},
        {"pnl", value.complete ? dollars(value.pnl) : json(nullptr)}, {"clamped", value.clamped}});
  }
  auto body = base(book);
  body.update({{"view", "stress"}, {"stress", stress}, {"complete", book.unpriced.empty()},
               {"model", "Each historical episode's recorded underlying and IV moves, applied at once with Black-76 sticky "
                         "strike as the paper account's stress tests. A held underlying the episode has no factor for "
                         "leaves that scenario without a P&L. P&L is from the book's value now; unpriced holdings are left out."}});
  return {200, body.dump()};
}

/// One-day historical VaR and expected shortfall of the valued holdings, as the
/// paper account's: daily closes and recorded ATM IV changes of each held underlying.
ApiResponse var_view(const AnalysisBook& book, const std::map<std::string, std::string>& query) {
  if (!query.empty()) return api_error(400, "INVALID_REQUEST", "This view takes no query parameters");
  const auto history = historical_closes(*book.source, held_underlyings(book), book.now);
  const auto risk = trading::historical_risk(book.ledger, book.valuations, history, book.now, book.max_age, book.stock_prices);
  auto body = base(book);
  auto sticky = tail_risk_json(risk.sticky_strike), atm = tail_risk_json(risk.atm_vol_change);
  // A book with unpriced holdings has no complete tail: the valued subset is not the portfolio.
  if (!book.unpriced.empty()) sticky["complete"] = atm["complete"] = false;
  body.update({{"view", "var"}, {"sticky_strike", sticky}, {"atm_vol_change", atm},
               {"complete", book.unpriced.empty() && risk.sticky_strike.complete},
               {"model", "Derived 1-day historical simulation over up to 252 completed closes per held underlying; at least "
                         "20 paired returns for 95% and 100 for 99%. Black-76 sticky strike with one calendar day of decay; "
                         "the ATM variant adds observed 30d ATM IV changes uniformly by tenor. Losses in dollars, floored at "
                         "zero. Unpriced holdings are left out."}});
  return {200, body.dump()};
}

json summary_json(const AnalysisPortfolio& p) {
  return {{"id", p.id}, {"name", p.name}, {"format", p.format}, {"imported", md::format_timestamp(p.imported)},
          {"holdings", p.holdings.size()}, {"analysis_only", true}, {"read_only", true}};
}

json detail_json(const AnalysisPortfolio& p, const AnalysisBook& book) {
  json holdings = json::array();
  double value = 0, cost = 0;
  bool valued = true, costed = true;
  for (const auto& h : p.holdings) {
    json row{{"symbol", h.symbol}, {"quantity", h.quantity}, {"source", h.source},
             {"cost", h.cost ? number(*h.cost) : json(nullptr)}};
    const double multiplier = h.contract ? h.contract->multiplier : 1;
    double mark = analytics::kNaN;
    std::string status = "priced";
    double delta = analytics::kNaN, gamma = analytics::kNaN, vega = analytics::kNaN, theta = analytics::kNaN;
    const auto underlying = h.contract ? h.contract->underlying : h.symbol;
    const auto metrics = book.source->metrics(underlying);
    if (!metrics) {
      status = "no_market_data";
    } else if (h.contract) {
      if (book.now >= h.contract->expiry_time()) status = "expired";
      else if (const auto* option = find_option(*metrics, *h.contract); !option) status = "not_in_chain";
      else {
        if (option->bid > 0 && option->ask >= option->bid) mark = option->mid;
        const auto v = book.valuations.find(h.symbol);
        if (v != book.valuations.end() && v->second.valid) {
          const double units = static_cast<double>(h.quantity) * multiplier;
          delta = units * v->second.delta;
          gamma = units * v->second.gamma;
          vega = units * v->second.vega;
          theta = units * v->second.theta;
        }
        if (!std::isfinite(mark) || !std::isfinite(delta)) status = "unpriced";
      }
    } else {
      mark = metrics->spot;
      delta = static_cast<double>(h.quantity);
      gamma = vega = theta = 0;
      if (!std::isfinite(mark) || !(mark > 0)) status = "unpriced";
    }
    const double market_value = mark * static_cast<double>(h.quantity) * multiplier;
    if (std::isfinite(market_value)) value += market_value; else valued = false;
    if (h.cost) cost += *h.cost * static_cast<double>(h.quantity) * multiplier; else costed = false;
    row.update({{"underlying", underlying}, {"type", h.contract ? (h.contract->type == pricing::OptionType::Call ? "call" : "put") : "shares"},
                {"expiry", h.contract ? json(md::format_date(h.contract->expiry)) : json(nullptr)},
                {"strike", h.contract ? number(h.contract->strike) : json(nullptr)},
                {"multiplier", multiplier}, {"mark", number(mark)}, {"value", dollars(market_value)},
                {"unrealised", h.cost && std::isfinite(market_value) ? dollars(market_value - *h.cost * static_cast<double>(h.quantity) * multiplier) : json(nullptr)},
                {"delta", number(delta)}, {"gamma", number(gamma)}, {"vega", number(vega)}, {"theta", number(theta)},
                {"status", status}});
    holdings.push_back(std::move(row));
  }
  trading::Limits limits;
  limits.max_valuation_age = book.max_age;
  const auto risk = trading::portfolio_risk(book.ledger, {}, book.contracts, book.valuations, limits, book.now, book.stock_prices);
  const auto exposure = [](const trading::Exposure& e) {
    return json{{"dollar_delta", dollars(e.dollar_delta)}, {"dollar_gamma_1pct", dollars(e.dollar_gamma_1pct)},
                {"vega", dollars(e.vega)}, {"theta", dollars(e.theta)}};
  };
  json underlyings = json::array();
  for (const auto& [symbol, bucket] : risk.underlyings) {
    auto row = exposure(bucket.position);
    row["underlying"] = symbol;
    underlyings.push_back(std::move(row));
  }
  json views = json::array();
  for (const auto& [name, view] : analysis_views()) views.push_back(name);
  auto body = base(book);
  body.update({{"id", p.id}, {"format", p.format}, {"imported", md::format_timestamp(p.imported)}, {"holdings", holdings},
               {"skipped", p.skipped}, {"value", valued ? dollars(value) : json(nullptr)},
               {"cost", costed ? dollars(cost) : json(nullptr)},
               {"unrealised", valued && costed ? dollars(value - cost) : json(nullptr)},
               {"exposure", {{"aggregate", exposure(risk.aggregate.position)}, {"underlyings", underlyings},
                             {"complete", risk.complete && book.unpriced.empty()}}},
               {"views", views}});
  return body;
}
}  // namespace

const std::map<std::string, AnalysisView, std::less<>>& analysis_views() {
  static const std::map<std::string, AnalysisView, std::less<>> views{
      {"profile", profile_view}, {"grid", grid_view}, {"stress", stress_view}, {"var", var_view}};
  return views;
}

AnalysisBook analysis_book(const AnalysisPortfolio& portfolio, const MetricsSource& source) {
  AnalysisBook book;
  book.portfolio = &portfolio;
  book.source = &source;
  std::map<std::string, std::shared_ptr<const analytics::UnderlyingMetrics>> metrics;
  const auto analytics_of = [&](const std::string& symbol) {
    auto [it, inserted] = metrics.try_emplace(symbol);
    if (inserted) {
      it->second = source.metrics(symbol);
      if (it->second) book.now = std::max(book.now, it->second->as_of);
    }
    return it->second;
  };
  std::map<std::string, trading::Position> positions;
  std::map<std::string, trading::StockPosition> stocks;
  for (const auto& h : portfolio.holdings) {
    const auto m = analytics_of(h.contract ? h.contract->underlying : h.symbol);
    if (h.contract) {
      trading::Position position{*h.contract, h.quantity, {}, {}, {}};
      if (h.cost) position.basis = Money::from_double(*h.cost * static_cast<double>(h.quantity) * h.contract->multiplier);
      const auto valuation = contract_valuation(h.symbol, *h.contract, m);
      // A holding without a valuation stays out of the views, which name it as unpriced.
      if (!valuation.valid) {
        book.unpriced.push_back(h.symbol);
        continue;
      }
      positions.emplace(h.symbol, position);
      book.contracts.emplace(h.symbol, *h.contract);
      book.valuations.emplace(h.symbol, valuation);
    } else {
      trading::StockPosition stock{h.symbol, h.quantity, {}, {}, {}};
      if (h.cost) stock.basis = Money::from_double(*h.cost * static_cast<double>(h.quantity));
      if (m && m->spot > 0 && std::isfinite(m->spot)) {
        book.stock_prices[h.symbol] = m->spot;
        stocks.emplace(h.symbol, stock);
      } else {
        book.unpriced.push_back(h.symbol);
      }
    }
  }
  book.ledger = trading::Ledger::restore({}, std::move(positions), std::move(stocks));
  return book;
}

std::optional<ApiResponse> analysis_request(const ApiRequest& request, AnalysisPortfolios& portfolios, const MetricsSource& source) {
  const std::string_view target = request.target;
  const auto mark = target.find('?');
  const auto path = target.substr(0, mark);
  constexpr std::string_view prefix = "/api/analysis/portfolios";
  if (path != prefix && !path.starts_with(std::string(prefix) + "/")) return std::nullopt;
  const auto query = query_parameters(mark == std::string_view::npos ? std::string_view{} : target.substr(mark + 1));
  if (!query) return api_error(400, "INVALID_REQUEST", "Duplicate or malformed query parameter");
  const auto rest = path.size() > prefix.size() ? path.substr(prefix.size() + 1) : std::string_view{};
  const auto slash = rest.find('/');
  const std::string id(rest.substr(0, slash));
  const auto view = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
  if (path == prefix) {
    if (request.method == "GET") {
      if (!query->empty()) return api_error(400, "INVALID_REQUEST", "This route takes no query parameters");
      json list = json::array();
      for (const auto& p : portfolios.list()) list.push_back(summary_json(p));
      return ApiResponse{200, json{{"portfolios", list}, {"formats", std::vector<std::string>(std::begin(kAnalysisFormats), std::end(kAnalysisFormats))}, {"note", kNote},
                                   {"error", portfolios.error().empty() ? json(nullptr) : json(portfolios.error())}}.dump()};
    }
    if (request.method != "POST") return api_error(405, "METHOD_NOT_ALLOWED", "Analysis portfolios support GET and POST");
    try {
      const auto body = json::parse(request.body);
      if (!body.is_object() || !body.contains("csv") || !body.at("csv").is_string() || !body.contains("name") ||
          !body.at("name").is_string() || (body.contains("format") && !body.at("format").is_string()))
        return api_error(400, "INVALID_REQUEST", "Send {name, csv, format?}");
      for (const auto& [key, value] : body.items())
        if (key != "name" && key != "csv" && key != "format") return api_error(400, "INVALID_REQUEST", "Unknown field " + key);
      auto parsed = parse_analysis_csv(body.at("csv").get<std::string>(), body.value("format", "auto"));
      const auto stored = portfolios.add(std::move(parsed), body.at("name").get<std::string>(), source.wall_time());
      const auto book = analysis_book(stored, source);
      return ApiResponse{201, detail_json(stored, book).dump()};
    } catch (const json::exception&) {
      return api_error(400, "INVALID_REQUEST", "The body must be JSON");
    } catch (const std::invalid_argument& error) {
      return api_error(400, "INVALID_IMPORT", error.what());
    } catch (const std::exception& error) {
      return api_error(500, "STORAGE_FAILED", error.what());
    }
  }
  const auto portfolio = portfolios.find(id);
  if (!portfolio) return api_error(404, "NOT_FOUND", "No analysis portfolio " + id);
  if (view.empty()) {
    if (request.method == "DELETE") {
      portfolios.remove(id);
      return ApiResponse{200, json{{"id", id}, {"deleted", true}}.dump()};
    }
    if (request.method != "GET") return api_error(405, "METHOD_NOT_ALLOWED", "An analysis portfolio supports GET and DELETE");
    if (!query->empty()) return api_error(400, "INVALID_REQUEST", "This route takes no query parameters");
    return ApiResponse{200, detail_json(*portfolio, analysis_book(*portfolio, source)).dump()};
  }
  if (!view.starts_with("risk/")) return api_error(404, "NOT_FOUND", "Unknown analysis route");
  if (request.method != "GET") return api_error(405, "METHOD_NOT_ALLOWED", "Risk views support GET");
  const auto& views = analysis_views();
  const auto found = views.find(view.substr(5));
  if (found == views.end()) return api_error(404, "NOT_FOUND", "No risk view " + std::string(view.substr(5)));
  return found->second(analysis_book(*portfolio, source), *query);
}

}  // namespace openport::server
