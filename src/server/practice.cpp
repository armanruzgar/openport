#include "openport/server/practice.hpp"

#include <cmath>
#include "../trading/state.hpp"
#include "../trading/state_delta.hpp"

namespace openport::server {
using nlohmann::json;
json scenario_marks(const providers::Scenario& scenario, md::Date date) {
  json out = json::array();
  const auto windows = providers::scenario_windows(scenario, date);
  for (std::size_t i = 0; i < windows.size(); ++i) {
    const auto& w = windows[i];
    const auto& events = scenario.sessions.empty() ? scenario.events : scenario.sessions[i].events;
    for (const auto& e : events) {
      const auto time = e.type == "gap" ? w.first : providers::scenario_event_time(e.at, w);
      json mark{{"type", e.type}, {"time", md::format_timestamp(time)}, {"symbols", e.symbols.empty() ? scenario.symbols : e.symbols}};
      if (e.minutes > 0) mark["end"] = md::format_timestamp(providers::scenario_event_time(e.at, w, e.minutes));
      out.push_back(std::move(mark));
    }
  }
  return out;
}
json drill_objectives(const providers::Scenario& scenario, md::Date date) {
  json out = json::array();
  const auto windows = providers::scenario_windows(scenario, date);
  for (const auto& o : scenario.objectives) {
    json value{{"id", o.id}, {"type", o.type}, {"limit", o.limit}};
    if (!o.at.empty()) {
      const auto& w = windows.at(o.session - 1);
      const auto at = providers::scenario_event_time(o.at, w);
      value["at"] = md::format_timestamp(at);
    }
    out.push_back(std::move(value));
  }
  return out;
}
json grade_drill(const trading::JournalRecovery& journal, const json& objectives) {
  json results = json::array();
  for (const auto& o : objectives) results.push_back({{"id", o.at("id")}, {"type", o.at("type")}, {"passed", nullptr}, {"observed", nullptr}});
  json state, original_plan;
  bool unchanged_plan = true;
  std::vector<bool> missing(objectives.size(), false), sampled(objectives.size(), false), failed(objectives.size(), false);
  for (const auto& record : journal.records) {
    const auto payload = json::parse(record.payload);
    if (payload.contains("state")) state = payload.at("state");
    else trading::detail::apply_state_delta(state, payload.at("delta"));
    const json plan{{"rules", state.at("config").at("rules")}, {"cash", state.at("config").at("initial_cash")}, {"attempt", state.at("evaluation").at("attempt")}};
    if (original_plan.is_null()) original_plan = plan;
    else unchanged_plan = unchanged_plan && plan == original_plan;
    const auto& ledger = state.at("ledger");
    const auto positions = ledger.at("positions").get<std::map<std::string, trading::Position>>();
    const auto stocks = ledger.value("stocks", json::object()).get<std::map<std::string, trading::StockPosition>>();
    auto equity = ledger.at("account").at("cash").get<trading::Money>();
    double vega = 0;
    bool marked = true, valued = true, flat = true;
    for (const auto& [symbol, p] : positions) if (p.quantity != 0) {
      flat = false;
      const auto mark = state.at("marks").find(symbol), valuation = state.at("valuations").find(symbol);
      if (mark == state.at("marks").end()) marked = false;
      else equity = equity + mark->at("price").get<trading::Money>() * p.quantity * 100;
      if (valuation == state.at("valuations").end() || !valuation->value("valid", true)) valued = false;
      else vega += valuation->at("vega").get<double>() * static_cast<double>(p.quantity) * 100;
    }
    for (const auto& [symbol, p] : stocks) if (p.shares != 0) {
      flat = false;
      const auto mark = state.at("stock_marks").find(symbol);
      if (mark == state.at("stock_marks").end()) marked = false;
      else equity = equity + mark->at("price").get<trading::Money>() * p.shares;
    }
    const auto config = state.at("config").get<trading::SessionConfig>();
    for (std::size_t i = 0; i < objectives.size(); ++i) {
      const auto& o = objectives[i];
      const auto type = o.at("type").get<std::string>();
      if (o.contains("at") && record.time < *md::parse_datetime(o.at("at").get<std::string>(), md::Zone::Utc)) continue;
      double observed = 0;
      bool available = marked;
      if (type == "max_net_vega") { observed = std::abs(vega); available = valued; }
      else if (type == "flat_by") { observed = flat ? 0 : 1; available = true; }
      else if (type == "max_loss") observed = std::max(trading::Money{}, config.initial_cash - equity).dollars();
      else {
        const auto floor = state.at("evaluation").at("floor").get<trading::Money>();
        const auto room = config.rules.max_drawdown.dollars();
        available = marked && room > 0;
        observed = room > 0 ? 100 * (equity - floor).dollars() / room : 0;
      }
      if (!available) { missing[i] = true; continue; }
      sampled[i] = true;
      const auto limit = o.at("limit").get<double>();
      const bool pass = type == "min_floor_room" ? observed >= limit : observed <= limit;
      failed[i] = failed[i] || !pass;
      const auto prior = results[i].at("observed");
      if (prior.is_null() || (type == "min_floor_room" ? observed < prior.get<double>() : observed > prior.get<double>())) results[i]["observed"] = observed;
    }
  }
  int passed = 0;
  bool complete = !objectives.empty() && unchanged_plan;
  for (std::size_t i = 0; i < objectives.size(); ++i) {
    const bool known = failed[i] || (sampled[i] && !missing[i]);
    results[i]["passed"] = known ? json(!failed[i]) : json(nullptr);
    if (!known) complete = false;
    if (known && !failed[i]) ++passed;
  }
  return {{"score", complete ? json(100.0 * passed / static_cast<double>(objectives.size())) : json(nullptr)},
          {"passed", passed}, {"total", objectives.size()}, {"objectives", results}, {"basis", unchanged_plan ? "committed journal states; derived practice score" : "ungraded: plan changed or account reset"}};
}
}  // namespace openport::server
