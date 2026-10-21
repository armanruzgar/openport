#include "playbook_api.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/playbooks.hpp"
#include "openport/trading/history.hpp"
#include "strategy_template.hpp"

#include <algorithm>
#include <charconv>
#include <set>

namespace openport::server {
namespace {
using nlohmann::json;
// The shared parser, so the access check reads the same account as these routes;
// strategy templates arrive form-encoded, with "+" for a space.
json query(const ApiRequest& request) {
  const auto question = request.target.find('?');
  const auto parameters = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1), true);
  if (!parameters) throw std::invalid_argument("Invalid query encoding or duplicate query parameter");
  json result = json::object();
  for (const auto& [key, value] : *parameters) result[key] = value;
  return result;
}
std::uint64_t uint_parameter(const json& parameters, const char* key, std::uint64_t fallback) {
  if (!parameters.contains(key)) return fallback;
  const auto text = parameters.at(key).get<std::string>();
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::invalid_argument(std::string("Invalid ") + key);
  return value;
}
/// A fresh attempt at `rules` from `balance`, for the odds of a program step not yet started.
trading::Evaluation fresh_attempt(const trading::Evaluation& current, const trading::AccountRules& rules, trading::Money balance) {
  trading::Evaluation e;
  e.started = current.started;
  e.day = current.day;
  e.starting_balance = e.peak = e.day_open_equity = e.day_close_equity = balance;
  e.floor = trading::evaluation_floor(rules, balance, e.floor_locked, balance);
  return e;
}
/// The profit a funded account needs to withdraw `gross`: gross × 100 / withdrawal percent, rounded up to a cent.
trading::Money payout_profit(const trading::PayoutRules& p, trading::Money gross) {
  const auto cents = (gross.micros() / 10'000 * 100 + p.withdrawal_percent - 1) / p.withdrawal_percent;
  return trading::Money::from_micros(cents * 10'000);
}
/// Challenge economics (trading::challenge_economics) for the account's program:
/// its preset and the steps its passes unlock, each from a fresh attempt on the
/// same history, through the funded step whose first payout's odds the history
/// gives too; query payout_odds and payout replace those two. Null with a reason
/// when the account follows no program and gives neither.
json economics_json(const TradingView& view, const std::vector<trading::EvaluationDay>& history, int days, int samples,
                    std::uint64_t seed, std::optional<double> payout_odds, std::optional<trading::Money> payout) {
  using namespace trading;
  const auto& snapshot = *view.snapshot;
  const auto& e = snapshot.evaluation;
  const auto& rules = view.config.rules;
  const auto id = preset_id(e.starting_balance, rules);
  struct Step { std::string plan; AccountRules rules; Money balance; bool first; };
  std::vector<Step> program;
  if (!id.empty()) {
    for (const auto* plan = find_plan(id); plan;) {
      program.push_back({plan->id, plan->rules, plan->initial_cash, program.empty()});
      if (plan->rules.phase == Phase::Funded) break;
      const PlanPreset* next = nullptr;
      for (const auto& candidate : plan_presets()) if (candidate.unlocked_by == plan->id) { next = &candidate; break; }
      plan = next;
    }
  } else {
    program.push_back({rules.plan.empty() ? "custom" : rules.plan, rules, e.starting_balance, true});
  }
  const bool funded = !program.empty() && program.back().rules.phase == Phase::Funded;
  std::string assumption = "Each step from a fresh attempt on this account's recorded days; resets cost the step's reset fee";
  std::vector<EconomicsStep> steps;
  std::optional<double> odds = payout_odds;
  std::optional<Money> value = payout;
  for (const auto& step : program) {
    if (step.rules.phase == Phase::Funded) {
      const auto& p = step.rules.payouts;
      // The funded step's activation is its cost; its first payout is what it is worth.
      steps.push_back({step.plan, 1, step.rules.activation_fee, Money{}, 1});
      const auto gross = !p.caps.empty() ? p.caps.front() : p.minimum;
      if (!value && gross > Money{}) value = gross.prorate(p.split_percent, 100);
      if (!odds && gross > Money{} && p.withdrawal_percent > 0) {
        auto target = step.rules;
        target.phase = Phase::Evaluation;
        target.payouts = {};
        target.size_scaling.reset();
        target.profit_target = payout_profit(p, gross);
        target.min_profitable_days = std::min<std::int64_t>(p.qualifying_days, 366);
        target.profitable_day_profit = p.qualifying_profit;
        odds = pass_odds(fresh_attempt(e, target, step.balance), target, step.balance, history, days, samples, seed).pass;
        assumption += "; payout odds: reaching the first payout's profit with its qualifying days before the floor";
      }
      continue;
    }
    if (!step.rules.evaluation()) continue;
    const auto pass = pass_odds(fresh_attempt(e, step.rules, step.balance), step.rules, step.balance, history, days, samples, seed).pass;
    steps.push_back({step.plan, pass, step.first ? step.rules.evaluation_fee : step.rules.activation_fee, step.rules.reset_fee,
                     step.rules.max_resets});
  }
  if (steps.empty() || !odds || !value)
    return {{"available", false}, {"reason", funded || payout_odds || payout ? "Give payout_odds and payout for this program"
        : "This account follows no preset program with a funded step; give payout_odds and payout"}};
  const auto result = challenge_economics(steps, *odds, *value);
  json rows = json::array();
  for (const auto& step : result.steps)
    rows.push_back({{"plan", step.plan}, {"pass", step.pass}, {"eventual_pass", step.eventual_pass},
                    {"expected_cost", step.expected_cost}, {"expected_attempts", step.expected_attempts}});
  return {{"available", true}, {"steps", rows}, {"payout_odds", result.payout_odds}, {"payout", result.payout},
          {"payout_odds_source", payout_odds ? "query" : "simulated"}, {"payout_source", payout ? "query" : "first payout cap, trader share"},
          {"expected_cost", result.expected_cost}, {"expected_payout", result.expected_payout},
          {"expected_value", result.expected_value},
          {"break_even_pass", result.break_even_pass ? json(*result.break_even_pass) : json(nullptr)},
          {"assumption", assumption}, {"label", "Illustrative estimate from past results, not a prediction"}};
}
std::vector<trading::EvaluationDay> playbook_days(const TradingView& view, const std::string& id) {
  if (id.empty()) return view.snapshot->evaluation.days;
  const auto history = trading::lifecycles(view.snapshot->recent_fills, view.snapshot->closures, view.contracts);
  std::vector<trading::EvaluationDay> result;
  for (const auto& day : view.snapshot->evaluation.days) {
    bool own = false, mixed = false;
    for (const auto& life : history) {
      const auto opened = md::new_york_time(life.opened).date;
      if (opened > day.day || (life.closed && md::new_york_time(*life.closed).date < day.day)) continue;
      const auto first = std::find_if(view.snapshot->recent_fills.begin(), view.snapshot->recent_fills.end(), [&](const auto& fill) { return fill.id == life.first_fill; });
      if (first == view.snapshot->recent_fills.end() || first->order_id == 0 || first->order_id > view.snapshot->recent_orders.size()) { mixed = true; continue; }
      const auto& order = view.snapshot->recent_orders[first->order_id - 1];
      const bool matches = std::any_of(order.request.tags.begin(), order.request.tags.end(), [&](const auto& tag) { return tag.starts_with("playbook:" + id + "@v"); });
      own = own || matches;
      mixed = mixed || !matches;
      // A later opening fill can add another setup to the same contract lifecycle.
      for (const auto fill_id : life.fills) {
        const auto added = std::find_if(view.snapshot->recent_fills.begin(), view.snapshot->recent_fills.end(), [&](const auto& fill) { return fill.id == fill_id; });
        if (added == view.snapshot->recent_fills.end() || (added->side == trading::Side::Buy ? 1 : -1) != life.direction ||
            added->order_id == 0 || added->order_id > view.snapshot->recent_orders.size()) continue;
        const auto& tags = view.snapshot->recent_orders[added->order_id - 1].request.tags;
        if (std::none_of(tags.begin(), tags.end(), [&](const auto& tag) { return tag.starts_with("playbook:" + id + "@v"); })) mixed = true;
      }
    }
    // Delivered shares and withdrawals prevent attribution to option playbooks.
    for (const auto& stock : trading::share_lifecycles(view.snapshot->stock_fills, view.snapshot->dividends))
      if (md::new_york_time(stock.opened).date <= day.day && (!stock.closed || md::new_york_time(*stock.closed).date >= day.day)) mixed = true;
    for (const auto& payout : view.snapshot->evaluation.payouts) if (payout.day == day.day) mixed = true;
    if (own && !mixed) result.push_back(day);
  }
  return result;
}
}
std::optional<ApiResponse> playbook_read(const ApiRequest& request, const MetricsSource& source) {
  const auto path = request.target.substr(0, request.target.find('?'));
  if (path != "/api/playbooks" && !path.starts_with("/api/playbooks/") && path != "/api/account/pass-odds" && path != "/api/strategy-template") return {};
  json parameters;
  try { parameters = query(request); }
  catch (const std::exception& error) { return api_error(400, "INVALID_REQUEST", error.what()); }
  try {
    if (path == "/api/strategy-template") {
      strict_keys(parameters, {"symbol", "expiry", "template", "min_strike", "max_strike"});
      const auto symbol = required(parameters, "symbol", "symbol").get<std::string>();
      const auto metrics = source.metrics(symbol);
      if (!metrics) return api_error(404, "UNKNOWN_UNDERLYING", "No chain for this underlying");
      const auto value = json::parse(required(parameters, "template", "template").get<std::string>(), nullptr, false);
      if (value.is_discarded()) throw std::invalid_argument("template must be URL-encoded JSON");
      validate_template(value);
      const auto& expiry = required(parameters, "expiry", "expiry");
      json near = nullptr, far = nullptr;
      for (const auto& slice : metrics->slices) {
        auto chain = template_chain(*metrics, slice);
        if (chain.at("expiry").at("id") == expiry) near = chain;
        if (value.contains("farExpiry") && chain.at("expiry").at("id") == value.at("farExpiry")) far = chain;
      }
      if (near.is_null()) return api_error(404, "UNKNOWN_EXPIRY", "Selected expiry unavailable");
      if (parameters.contains("min_strike") || parameters.contains("max_strike")) {
        if (!parameters.contains("min_strike") || !parameters.contains("max_strike")) throw std::invalid_argument("min_strike and max_strike go together");
        const auto low = json::parse(parameters.at("min_strike").get<std::string>(), nullptr, false);
        const auto high = json::parse(parameters.at("max_strike").get<std::string>(), nullptr, false);
        const auto minimum = bounded_number(low, 0, 10000000, "min_strike");
        const auto maximum = bounded_number(high, minimum, 10000000, "max_strike");
        std::erase_if(near["strikes"].get_ref<json::array_t&>(), [&](const auto& row) { return row.at("strike").template get<double>() < minimum || row.at("strike").template get<double>() > maximum; });
      }
      return ApiResponse{200, build_template(value, near, far).dump()};
    }
    const bool odds = path == "/api/account/pass-odds";
    // The query is the request's: a malformed one is 400 INVALID_REQUEST, as on every route.
    std::uint64_t days = 0, samples = 0, seed = 0;
    bool economics = false;
    std::optional<double> payout_odds;
    std::optional<trading::Money> payout;
    try {
      strict_keys(parameters, odds ? std::initializer_list<std::string_view>{"account", "days", "samples", "seed", "playbook", "economics", "payout_odds", "payout"} : std::initializer_list<std::string_view>{"account", "version"});
      if (path == "/api/playbooks" && parameters.contains("version")) throw std::invalid_argument("version requires a playbook ID");
      if (odds) {
        days = uint_parameter(parameters, "days", 20);
        samples = uint_parameter(parameters, "samples", 1000);
        seed = uint_parameter(parameters, "seed", 81723);
        if (days < 1 || days > 252 || samples < 1 || samples > 10000) throw std::invalid_argument("days must be 1–252 and samples 1–10000");
        if (parameters.contains("economics") && parameters.at("economics") != "true" && parameters.at("economics") != "false")
          throw std::invalid_argument("economics must be true or false");
        economics = parameters.value("economics", "false") == "true";
        if (parameters.contains("payout_odds")) {
          const auto text = parameters.at("payout_odds").get<std::string>();
          double value = -1;
          const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
          if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !(value >= 0 && value <= 1))
            throw std::invalid_argument("payout_odds must be a number from 0 to 1");
          payout_odds = value;
        }
        if (parameters.contains("payout")) {
          try { payout = trading::Money::parse(parameters.at("payout").get<std::string>()); }
          catch (const std::exception&) { throw std::invalid_argument("payout must be a decimal dollar amount"); }
          if (*payout < trading::Money{}) throw std::invalid_argument("payout must be nonnegative");
        }
        if ((payout_odds || payout) && !economics) throw std::invalid_argument("payout_odds and payout need economics=true");
      }
    } catch (const std::exception& error) { return api_error(400, "INVALID_REQUEST", error.what()); }
    const auto account = query_account(request.target, request.access.sandbox);
    if (!account) return api_error(400, "INVALID_REQUEST", "account must be an account ID");
    const auto view = source.trading_view(*account);
    if (!view || !view->snapshot) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + *account);
    const auto catalogue = view->playbooks_json.empty() ? json{{"definitions", json::object()}, {"modes", json::object()}, {"staged", json::array()}, {"reasons", json::object()}, {"auto_allowed", false}} : json::parse(view->playbooks_json);
    if (odds) {
      // The account cannot be estimated yet: no evaluation rule, incomplete marks or too little history.
      if (!view->snapshot->valuation_complete)
        return api_error(422, "PASS_ODDS_UNAVAILABLE", "Pass odds require complete current equity marks");
      const auto id = parameters.value("playbook", "");
      if (!id.empty() && !catalogue.at("definitions").contains(id)) return api_error(404, "UNKNOWN_PLAYBOOK", "Unknown playbook");
      trading::PassOdds result;
      json costs = nullptr;
      try {
        const auto history = playbook_days(*view, id);
        result = trading::pass_odds(view->snapshot->evaluation, view->config.rules, view->snapshot->equity,
            history, static_cast<int>(days), static_cast<int>(samples), seed);
        if (economics) costs = economics_json(*view, history, static_cast<int>(days), static_cast<int>(samples), seed, payout_odds, payout);
      } catch (const std::invalid_argument& error) { return api_error(422, "PASS_ODDS_UNAVAILABLE", error.what()); }
      return ApiResponse{200, json{{"economics", costs}, {"pass", result.pass}, {"fail", result.fail}, {"neither", result.neither},
          {"median_days_to_pass", result.median_days_to_pass ? json(*result.median_days_to_pass) : json(nullptr)},
          {"historical_days", result.historical_days}, {"seed", std::to_string(result.seed)}, {"days", days}, {"samples", samples},
          {"block_days", 3}, {"label", "Estimate from past results, not a prediction"},
          {"history_basis", id.empty() ? "Account equity days with recorded intraday extremes" : "Days attributable exclusively to this playbook; mixed days excluded"},
          {"path_assumption", "Open, recorded low/high in timestamp order, close; moves between these observations are unknown"}, {"simulated", true}}.dump()};
    }
    if (path != "/api/playbooks") {
      const auto id = path.substr(std::string("/api/playbooks/").size());
      if (!catalogue.at("definitions").contains(id)) return api_error(404, "UNKNOWN_PLAYBOOK", "Unknown playbook");
      const auto& record = catalogue.at("definitions").at(id);
      const auto version = uint_parameter(parameters, "version", record.at("versions").back().at("version").get<std::uint64_t>());
      for (const auto& definition : record.at("versions")) if (definition.at("version") == version) return ApiResponse{200, definition.dump()};
      return api_error(404, "UNKNOWN_VERSION", "Unknown playbook version");
    }
    auto result = catalogue;
    result["reports"] = playbook_report(catalogue, *view);
    result["forward_tests"] = playbook_forward_report(catalogue, *view);
    return ApiResponse{200, result.dump()};
  } catch (const std::exception& error) { return api_error(400, "INVALID_PLAYBOOK", error.what()); }
}
bool playbook_write(const ApiRequest& request, MetricsSource& source, ApiCompletion complete) {
  const auto path = request.target.substr(0, request.target.find('?'));
  const bool signal = path.starts_with("/api/signals/");
  if (path != "/api/playbooks" && !path.starts_with("/api/playbooks/") && !signal) return false;
  if (signal && !request.signal_verified) { complete(api_error(403, "INVALID_SIGNAL_SIGNATURE", "Signal secret or signature required")); return true; }
  try {
    const auto parameters = query(request);
    strict_keys(parameters, {"account", "version"});
    if (request.method != "DELETE" && parameters.contains("version")) throw std::invalid_argument("version query is only valid when archiving; updates carry version in the definition");
    json command;
    const auto body = request.body.empty() ? json::object() : json::parse(request.body, nullptr, false);
    if (body.is_discarded()) throw std::invalid_argument("Request body must be JSON");
    if (signal) {
      strict_keys(parameters, {"account"});
      strict_keys(body, {"action", "nonce", "secret"});
      if (request.method != "POST" || body.at("action") != "evaluate") throw std::invalid_argument("Signal action must be evaluate");
      command = {{"action", "signal"}, {"id", path.substr(13)}};
    } else if (request.method == "POST" && path == "/api/playbooks") command = {{"action", "create"}, {"definition", body}};
    else if (path.starts_with("/api/playbooks/staged/") && request.method == "POST") {
      const auto rest = path.substr(std::string("/api/playbooks/staged/").size());
      const auto slash = rest.find('/');
      if (slash == std::string::npos || (rest.substr(slash + 1) != "send" && rest.substr(slash + 1) != "dismiss")) throw std::invalid_argument("Unknown staged-order action");
      strict_keys(body, {});
      command = {{"action", rest.substr(slash + 1)}, {"staged", rest.substr(0, slash)}};
    } else {
      const auto rest = path.substr(std::min(path.size(), std::string("/api/playbooks/").size()));
      if (request.method == "POST" && rest.ends_with("/evaluate")) {
        // The bearer-authenticated sibling of /api/signals: trade scope, no signal secret.
        strict_keys(parameters, {"account"});
        strict_keys(body, {});
        command = {{"action", "signal"}, {"id", rest.substr(0, rest.size() - 9)}};
      } else if (request.method == "PUT" && rest.ends_with("/mode")) {
        strict_keys(body, {"mode"});
        command = {{"action", "mode"}, {"id", rest.substr(0, rest.size() - 5)}, {"mode", required(body, "mode", "mode")}};
      } else if (request.method == "PUT") {
        if (required(body, "id", "id") != rest) throw std::invalid_argument("Path and definition IDs must match");
        command = {{"action", "update"}, {"definition", body}};
      } else if (request.method == "DELETE") {
        strict_keys(body, {});
        command = {{"action", "delete"}, {"id", rest}, {"version", uint_parameter(parameters, "version", 0)}};
      } else throw std::invalid_argument("Unknown playbook route or method");
    }
    TradingCommand request_command;
    request_command.kind = TradingCommand::Kind::Playbook;
    request_command.actor = request.actor;
    const auto account = query_account(request.target, request.access.sandbox);
    if (!account) { complete(api_error(400, "INVALID_REQUEST", "account must be an account ID")); return true; }
    request_command.account = *account;
    request_command.note = command.dump();
    if (!source.post_trading(request_command, [complete](TradingReply reply) {
      // As on every account route: an unknown account is 404, an unavailable one 503.
      if (reply.error_code == "UNKNOWN_ACCOUNT") complete(api_error(404, "UNKNOWN_ACCOUNT", reply.decision.message));
      else if (!reply.error_code.empty()) complete(api_error(503, reply.error_code, reply.decision.message));
      else if (!reply.decision.ok()) complete(api_error(400, "INVALID_PLAYBOOK", reply.decision.message));
      else complete(ApiResponse{200, reply.playbook_result});
    })) {
      auto busy = api_error(503, "TRADING_UNAVAILABLE", "Command inbox full or trading unavailable");
      busy.retry_after = kInboxRetrySeconds;
      complete(std::move(busy));
    }
  } catch (const std::exception& error) { complete(api_error(400, "INVALID_PLAYBOOK", error.what())); }
  return true;
}
}  // namespace openport::server
