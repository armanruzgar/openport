#include "openport/server/backtest.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include "openport/providers/replay_batches.hpp"
#include "openport/server/plans.hpp"
#include "openport/trading/history.hpp"
#include "openport/server/web_policy.hpp"
#include "run_json.hpp"
#include "event_calendar.hpp"
#include "strategy_template.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
using trading::Money;
void keys(const json& object, std::initializer_list<std::string_view> allowed) {
  if (!object.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (const auto& [key, value] : object.items()) {
    (void)value;
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
      throw std::invalid_argument("Unknown backtest field: " + key);
  }
}
unsigned bounded(const json& value, unsigned maximum, std::string_view name) {
  if (!value.is_number_integer() || value < 1 || value > maximum)
    throw std::invalid_argument(std::string(name) + " must be 1–" + std::to_string(maximum));
  return value.get<unsigned>();
}
std::uint64_t seed_value(const json& value) {
  const auto text = value.is_string() ? value.get<std::string>() : value.dump();
  std::uint64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("seed must be an unsigned 64-bit integer");
  return result;
}
/// A dollar amount, which the API writes as a decimal string.
Money decimal(const json& value, const std::string& field) {
  if (!value.is_string()) throw std::invalid_argument(field + " must be a decimal string");
  return Money::parse(value.get<std::string>());
}
/// Funded custom plans use decimal payout money, just like the account API.
trading::PayoutRules payout_rules(const json& object) {
  keys(object, {"qualifying_profit", "qualifying_days", "withdrawal_percent", "split_percent", "minimum", "caps",
                "consistency_percents", "buffer", "buffer_payouts"});
  trading::PayoutRules p;
  const auto integer = [](const json& value, const std::string& name) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::uint64_t(INT64_MAX)))
      throw std::invalid_argument("payouts " + name + " must be a signed 64-bit integer");
    return value.get<std::int64_t>();
  };
  for (const auto& [key, value] : object.items()) {
    if (key == "qualifying_profit") p.qualifying_profit = decimal(value, "payouts " + key);
    else if (key == "minimum") p.minimum = decimal(value, "payouts " + key);
    else if (key == "buffer") p.buffer = decimal(value, "payouts " + key);
    else if (key == "caps" || key == "consistency_percents") {
      if (!value.is_array() || value.size() > 64) throw std::invalid_argument("payouts " + key + " must be an array of at most 64 entries");
      for (const auto& entry : value) {
        if (key == "caps") p.caps.push_back(decimal(entry, "payouts caps"));
        else p.consistency_percents.push_back(integer(entry, key));
      }
    } else if (key == "qualifying_days") p.qualifying_days = integer(value, key);
    else if (key == "withdrawal_percent") p.withdrawal_percent = integer(value, key);
    else if (key == "split_percent") p.split_percent = integer(value, key);
    else if (key == "buffer_payouts") p.buffer_payouts = integer(value, key);
  }
  // Validate before journal decoding, so invalid rules retain INVALID_RULES.
  trading::AccountRules rules;
  rules.phase = trading::Phase::Funded;
  rules.payouts = p;
  trading::validate_rules(rules);
  return p;
}
md::Date date_value(const json& value) {
  if (!value.is_string()) throw std::invalid_argument("date must be YYYY-MM-DD");
  const auto text = value.get<std::string>();
  const auto parsed = md::parse_datetime(text + "T12:00:00", md::Zone::NewYork);
  if (text.size() != 10 || !parsed) throw std::invalid_argument("date must be YYYY-MM-DD");
  const auto date = md::new_york_time(*parsed).date;
  if (!(md::trading_date(md::new_york_to_utc(date, 12, 0)) == date)) throw std::invalid_argument("date must be a trading day");
  return date;
}
std::string numbered(std::size_t index) {
  std::ostringstream result;
  result << std::setfill('0') << std::setw(6) << index + 1;
  return result.str();
}
std::string outcome(trading::EvaluationStatus status) {
  return status == trading::EvaluationStatus::Passed ? "passed" : status == trading::EvaluationStatus::Failed ? "failed" : "open";
}
struct Cancelled {};
struct Workers {
  std::vector<std::thread> threads;
  ~Workers() { for (auto& worker : threads) if (worker.joinable()) worker.join(); }
};
void check_cancel(const std::atomic_bool& cancel) { if (cancel.load()) throw Cancelled{}; }
void check_desk(const Desk& desk) {
  if (!desk.trading_status().enabled) throw std::runtime_error(desk.trading_status().reason);
}
struct Measurements {
  Money peak, drawdown;
  std::optional<Money> distance;
  void sample(const EquitySample& value) {
    peak = std::max(peak, value.equity);
    drawdown = std::max(drawdown, peak - value.equity);
    if (value.floor) {
      const auto room = value.equity - *value.floor;
      if (!distance || room < *distance) distance = room;
    }
  }
};
/// What blocked each playbook and underlying's entries: the last reason an evaluation
/// gave inside the entry window, or outside it when the window never opened. The
/// reasons at the end of the input only say that the window has closed.
struct EntryReasons {
  json reasons = json::object();
  std::string published;
  std::set<std::string> inside;
  void observe(const TradingView& view) {
    if (view.playbooks_json.empty() || view.playbooks_json == published) return;
    published = view.playbooks_json;
    const auto publication = json::parse(published);
    for (const auto& [key, reason] : publication.at("reasons").items()) {
      const bool open = reason != kOutsideEntryWindow;
      if (open) inside.insert(key);
      if (open || !inside.contains(key)) reasons[key] = reason;
    }
  }
};
struct TemporaryRecording {
  std::filesystem::path file;
  ~TemporaryRecording() {
    if (file.empty()) return;
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    std::filesystem::remove(file.string() + ".end", ignored);
    std::filesystem::remove(file.string() + ".end.tmp", ignored);
  }
};
struct Prepared {
  std::filesystem::path file;
  std::string identity;
};
Prepared prepare(const BacktestDay& day, const std::filesystem::path& directory, std::size_t index) {
  if (!day.scenario) return {day.file, recording_input(day.file)};
  const auto file = directory / "inputs" / (numbered(index) + ".oprec");
  providers::write_scenario_recording(file, *day.scenario, day.date, day.seed);
  return {file, scenario_input(*day.scenario, day.date, day.seed)};
}
Desk::Options options_for(const BacktestRequest& request, const Prepared& input,
    const std::filesystem::path& journal, Measurements& measurements) {
  Desk::Options options;
  options.replay = true;
  options.paper = request.config;
  options.analytics = request.analytics;
  options.dividends = request.dividends;
  options.initial_actor = request.actor;
  options.initial_playbooks = request.playbooks.dump();
  options.paper_journal = journal;
  options.run_input = input.identity;
  options.candles = std::make_shared<CandleStore>();
  options.equity_sample = [&](std::string_view, const EquitySample& sample) { measurements.sample(sample); };
  return options;
}
// All calculations retain integer micro-dollars. These summaries cover the option
// lifecycles the existing playbook report can attribute, not delivered shares.
json attributed_stats(const json& trades, Money realised, std::optional<Money> marked,
    const json& reasons, const json& trips) {
  Money closed_net;
  std::size_t closed = 0, open = 0, wins = 0, losses = 0, passed = 0, measured = 0, bp_count = 0;
  double bp_total = 0;
  for (const auto& trade : trades) {
    passed += trade.at("passed_rules").get<std::size_t>();
    measured += trade.at("measured_rules").get<std::size_t>();
    if (trade.at("closed").is_null()) { ++open; continue; }
    ++closed;
    const auto net = Money::parse(trade.at("net").get<std::string>());
    closed_net = closed_net + net;
    if (net > Money{}) ++wins;
    if (net < Money{}) ++losses;
    if (trade.contains("return_on_buying_power") && trade.at("return_on_buying_power").is_number()) {
      ++bp_count; bp_total += trade.at("return_on_buying_power").get<double>();
    }
  }
  return {{"trades", closed}, {"open_trades", open},
      {"expectancy", closed ? json(closed_net.prorate(1, static_cast<std::int64_t>(closed)).str()) : json(nullptr)},
      {"win_rate", wins + losses ? json(static_cast<double>(wins) / static_cast<double>(wins + losses)) : json(nullptr)},
      {"adherence", measured ? json(static_cast<double>(passed) / static_cast<double>(measured)) : json(nullptr)},
      {"average_return_on_buying_power", bp_count ? json(bp_total / static_cast<double>(bp_count)) : json(nullptr)},
      {"realised_pnl", realised.str()}, {"marked_pnl", marked ? json(marked->str()) : json(nullptr)},
      {"entry_reasons", reasons}, {"rule_trips", trips}};
}
json joint_breakdown(const json& reports, const TradingView& view, const EntryReasons& entries, const json& trips) {
  json result = json::object();
  const auto history = trading::lifecycles(view.snapshot->recent_fills, view.snapshot->closures, view.contracts);
  for (const auto& [id, report] : reports.items()) {
    Money realised, unrealised;
    bool complete = true;
    std::set<trading::OrderId> orders;
    for (const auto& trade : report.at("trades")) {
      realised = realised + Money::parse(trade.at("net").get<std::string>());
      orders.insert(std::stoull(trade.at("order").get<std::string>()));
    }
    for (const auto& life : history) {
      if (!life.quantity || !orders.contains(life.entry_order)) continue;
      const auto position = std::find_if(view.snapshot->positions.begin(), view.snapshot->positions.end(),
          [&](const auto& item) { return item.position.contract.osi_symbol() == life.symbol; });
      if (position == view.snapshot->positions.end() || !position->fresh || !position->unrealised) complete = false;
      else unrealised = unrealised + *position->unrealised;
    }
    json reasons = json::object(), own_trips = json::array();
    for (const auto& [key, reason] : entries.reasons.items())
      if (key.starts_with(id + ":")) reasons[key] = reason;
    for (const auto& trip : trips)
      if (trip.value("playbook", "") == id) own_trips.push_back(trip);
    result[id] = attributed_stats(report.at("trades"), realised,
        complete ? std::optional<Money>(realised + unrealised) : std::nullopt, reasons, own_trips);
  }
  return result;
}
// A compact identity used by compare and saved listings. Single-run callers keep
// their historical id/version-only representation.
json joint_identities(const json& definitions) {
  json result = json::array();
  for (const auto& definition : definitions)
    result.push_back({{"id", definition.at("id")}, {"version", definition.at("version")}, {"name", definition.at("name")}});
  return result;
}
json result_for(Desk& desk, const BacktestRequest& request, const std::filesystem::path& directory,
    const std::string& journal, const Measurements& measurements, const EntryReasons& entries) {
  desk.flush_journals();
  check_desk(desk);
  const auto view = desk.trading_view();
  const auto& snapshot = *view->snapshot;
  const bool joint = request.playbook.empty();
  const auto reports = playbook_report(request.playbooks, *view);
  json stats;
  if (!joint) stats = reports.at(request.playbook);
  else {
    stats["trades"] = json::array();
    for (const auto& report : reports)
      for (const auto& trade : report.at("trades")) stats["trades"].push_back(trade);
    std::sort(stats["trades"].begin(), stats["trades"].end(), [](const json& a, const json& b) {
      return std::stoull(a.at("order").get<std::string>()) < std::stoull(b.at("order").get<std::string>());
    });
    stats["all"] = attributed_stats(stats.at("trades"), {}, {}, json::object(), json::array());
  }
  json fills = json::array();
  for (const auto& fill : snapshot.recent_fills) fills.push_back({
      {"id", std::to_string(fill.id)}, {"order", std::to_string(fill.order_id)}, {"symbol", fill.symbol},
      {"side", fill.side == trading::Side::Buy ? "buy" : "sell"}, {"quantity", fill.quantity},
      {"price", fill.price.str()}, {"fee", fill.fee.str()}, {"time", md::format_timestamp(fill.time)}});
  json stock_fills = json::array(), stock_trades = json::array();
  for (const auto& fill : snapshot.stock_fills) stock_fills.push_back({
      {"id", std::to_string(fill.id)}, {"symbol", fill.symbol}, {"shares", fill.shares},
      {"price", fill.price.str()}, {"time", md::format_timestamp(fill.time)}, {"option", fill.option}});
  for (const auto& trade : trading::share_lifecycles(snapshot.stock_fills, snapshot.dividends)) stock_trades.push_back({
      {"symbol", trade.symbol}, {"opened", md::format_timestamp(trade.opened)},
      {"closed", trade.closed ? json(md::format_timestamp(*trade.closed)) : json(nullptr)},
      {"net", (trade.gross + trade.dividends).str()}, {"shares", trade.shares}});
  json trips = json::array();
  const auto recovery = trading::FileJournal::read((directory / journal).string());
  for (const auto& record : recovery.records) {
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events")) {
      const auto type = event.at("type").get<std::string>();
      if (type == "evaluation_failed" || type == "day_locked" || type == "kill_trip" || type == "order_rejected") {
        json trip{{"time", md::format_timestamp(record.time)}, {"type", type}, {"detail", event.at("payload")}};
        if (joint && type == "order_rejected") {
          const auto order = event.at("payload").get<trading::Order>();
          for (const auto& [id, definition] : request.playbooks.at("definitions").items()) {
            const auto tag = playbook_tag(definition.at("versions").back());
            if (std::find(order.request.tags.begin(), order.request.tags.end(), tag) != order.request.tags.end()) trip["playbook"] = id;
          }
        }
        trips.push_back(std::move(trip));
      }
    }
  }
  auto distance = measurements.distance;
  if (snapshot.evaluation.closest_floor && (!distance || *snapshot.evaluation.closest_floor < *distance)) distance = snapshot.evaluation.closest_floor;
  json flags = json::array();
  for (const auto flag : snapshot.quality_flags) flags.push_back(trading::to_string(flag));
  json result{{"started", md::format_timestamp(snapshot.evaluation.started)}, {"ended", md::format_timestamp(snapshot.time)},
      {"pnl", snapshot.valuation_complete ? json((snapshot.equity - snapshot.evaluation.starting_balance).str()) : json(nullptr)},
      {"last_mark_pnl", (snapshot.equity - snapshot.evaluation.starting_balance).str()}, {"valuation_complete", snapshot.valuation_complete},
      {"quality_flags", flags}, {"max_drawdown", measurements.drawdown.str()},
      {"min_floor_distance", distance ? json(distance->str()) : json(nullptr)}, {"outcome", outcome(snapshot.evaluation.status)},
      {"decision", snapshot.evaluation.decision}, {"rule_trips", trips}, {"trades", stats.at("trades")},
      {"fills", fills}, {"stock_fills", stock_fills}, {"stock_trades", stock_trades}, {"adherence", stats.at("all").at("adherence")},
      {"entry_reasons", entries.reasons},
      {"open_positions", snapshot.positions.size() + snapshot.stocks.size()}, {"journal", journal}, {"journal_head", recovery.head}};
  if (joint) result["per_playbook"] = joint_breakdown(reports, *view, entries, trips);
  return result;
}
json evaluation_day(const trading::TradingSnapshot& start, const trading::TradingSnapshot& end,
    const trading::AccountRules& rules, const BacktestDay& day, std::size_t index) {
  const auto before = trading::plan_inputs(start), now = trading::plan_inputs(end);
  const auto& evaluation = end.evaluation;
  const bool floor = rules.max_drawdown > Money{};
  return {{"date", md::format_date(day.date)}, {"day_index", index}, {"ended", md::format_timestamp(end.time)},
      {"start_balance", before.balance.str()}, {"end_balance", now.balance.str()},
      {"start_equity", start.equity.str()}, {"end_equity", end.equity.str()},
      {"pnl", start.valuation_complete && end.valuation_complete ? json((end.equity - start.equity - (end.evaluation.starting_balance - start.evaluation.starting_balance)).str()) : json(nullptr)},
      {"valuation_complete", end.valuation_complete}, {"floor", floor ? json(evaluation.floor.str()) : json(nullptr)},
      {"floor_distance", floor && end.valuation_complete ? json((end.equity - evaluation.floor).str()) : json(nullptr)},
      {"target", rules.profit_target.str()}, {"target_progress", trading::attempt_profit(evaluation, rules, now).str()},
      {"peak", evaluation.peak.str()}, {"day_lock", trading::to_string(evaluation.day_lock)},
      {"outcome", outcome(evaluation.status)}, {"decision", evaluation.decision},
      {"decision_code", trading::to_string(evaluation.decision_code)}, {"rule_trips", json::array()},
      {"trades_opened", 0}, {"trades_closed", 0}};
}
json distribution(std::vector<Money> values) {
  std::sort(values.begin(), values.end());
  json sorted = json::array();
  Money total;
  for (const auto value : values) { sorted.push_back(value.str()); total = total + value; }
  if (values.empty()) return {{"values", sorted}, {"min", nullptr}, {"p25", nullptr}, {"median", nullptr}, {"p75", nullptr}, {"max", nullptr}, {"mean", nullptr}};
  const auto quantile = [&](std::size_t numerator) { return values[(values.size() - 1) * numerator / 4].str(); };
  return {{"values", sorted}, {"min", values.front().str()}, {"p25", quantile(1)}, {"median", quantile(2)},
      {"p75", quantile(3)}, {"max", values.back().str()}, {"mean", total.prorate(1, static_cast<std::int64_t>(values.size())).str()}};
}
json aggregate(const json& days, const json& attempts) {
  std::vector<Money> pnls, drawdowns;
  std::vector<std::size_t> worst;
  Money net;
  std::size_t trades = 0, wins = 0, losses = 0, day_wins = 0, day_losses = 0, passed = 0, failed = 0, bp_trades = 0;
  double bp_total = 0;
  for (std::size_t index = 0; index < days.size(); ++index) {
    const auto& day = days[index];
    if (day.is_null()) continue;
    if (!day.at("pnl").is_null()) {
      const auto pnl = Money::parse(day.at("pnl").get<std::string>());
      pnls.push_back(pnl);
      if (pnl > Money{}) ++day_wins;
      if (pnl < Money{}) ++day_losses;
      worst.push_back(index);
    }
    drawdowns.push_back(Money::parse(day.at("max_drawdown").get<std::string>()));
    for (const auto& trade : day.at("trades")) if (!trade.at("closed").is_null()) {
      const auto profit = Money::parse(trade.at("net").get<std::string>());
      net = net + profit; ++trades;
      if (profit > Money{}) ++wins;
      if (profit < Money{}) ++losses;
      if (const auto it = trade.find("return_on_buying_power"); it != trade.end() && it->is_number()) {
        ++bp_trades;
        bp_total += it->get<double>();
      }
    }
  }
  std::stable_sort(worst.begin(), worst.end(), [&](std::size_t left, std::size_t right) {
    return Money::parse(days[left].at("pnl").get<std::string>()) < Money::parse(days[right].at("pnl").get<std::string>());
  });
  if (worst.size() > 10) worst.resize(10);
  for (const auto& attempt : attempts) {
    if (attempt.at("outcome") == "passed") ++passed;
    if (attempt.at("outcome") == "failed") ++failed;
  }
  const auto decided = passed + failed;
  return {{"daily_pnl", distribution(pnls)}, {"daily_drawdown", distribution(drawdowns)}, {"worst_days", worst},
      {"completed_days", drawdowns.size()}, {"marked_days", pnls.size()}, {"trades", trades},
      {"expectancy", trades ? json(net.prorate(1, static_cast<std::int64_t>(trades)).str()) : json(nullptr)},
      {"average_return_on_buying_power", bp_trades ? json(bp_total / static_cast<double>(bp_trades)) : json(nullptr)},
      // Win rates count decided trades and days: breakeven ones are neither wins nor losses.
      {"win_rate", wins + losses ? json(static_cast<double>(wins) / static_cast<double>(wins + losses)) : json(nullptr)},
      {"day_win_rate", day_wins + day_losses ? json(static_cast<double>(day_wins) / static_cast<double>(day_wins + day_losses)) : json(nullptr)},
      {"attempts", attempts.size()}, {"passed", passed}, {"failed", failed}, {"open", attempts.size() - decided},
      {"pass_rate", decided ? json(static_cast<double>(passed) / static_cast<double>(decided)) : json(nullptr)}};
}
}  // namespace

json compare_backtests(const json& runs) {
  json result{{"runs", json::array()}, {"daily", json::array()}, {"label", kBacktestLabel}};
  std::map<std::string, std::map<std::string, json>> dates;
  json first_inputs, first_plan;
  bool different_inputs = false, different_plans = false, incomplete_inputs = false, includes_joint = false;
  for (const auto& run : runs) {
    const auto id = run.at("id").get<std::string>();
    const auto& report = run.at("report");
    json inputs = report.value("input_set", json::array());
    if (!report.contains("input_set")) {
      for (const auto& day : report.at("days")) {
        if (day.is_null()) { incomplete_inputs = true; continue; }
        inputs.push_back({{"date", day.at("date")}, {"input", day.at("input")}});
      }
    }
    for (const auto& input : inputs) {
      dates[input.at("date").get<std::string>()][id] = nullptr;
      if (input.at("input").is_null()) incomplete_inputs = true;
    }
    for (const auto& day : report.at("days"))
      if (!day.is_null()) dates[day.at("date").get<std::string>()][id] = day.at("pnl");
    const auto& plan = report.at("config");
    if (result["runs"].empty()) { first_inputs = inputs; first_plan = plan; }
    else { different_inputs = different_inputs || inputs != first_inputs; different_plans = different_plans || plan != first_plan; }
    json row{{"id", id}, {"status", run.at("status")}, {"plan", plan}, {"input_set", inputs}, {"summary", report.at("summary")}};
    if (report.value("mode", "") == "joint") {
      includes_joint = true;
      row["mode"] = "joint";
      row["playbooks"] = joint_identities(report.at("playbooks"));
    } else row["playbook"] = {{"id", report.at("playbook").at("id")}, {"version", report.at("playbook").at("version")}};
    result["runs"].push_back(std::move(row));
  }
  result["different_inputs"] = different_inputs;
  result["different_plans"] = different_plans;
  result["incomplete_inputs"] = incomplete_inputs;
  result["apples_to_oranges"] = different_inputs || different_plans || incomplete_inputs;
  json curve = json::array();
  std::vector<Money> pnls;
  std::vector<std::size_t> worst;
  Money cumulative, peak, drawdown;
  std::size_t wins = 0, losses = 0;
  bool curve_complete = true;
  for (const auto& [date, values] : dates) {
    json row{{"date", date}, {"pnl", json::object()}};
    Money total;
    bool complete = true;
    std::size_t contributors = 0;
    for (const auto& run : runs) {
      const auto id = run.at("id").get<std::string>();
      const auto found = values.find(id);
      row["pnl"][id] = found == values.end() ? json(nullptr) : found->second;
      if (found == values.end()) continue; // No supplied day contributes nothing.
      if (found->second.is_null()) { complete = false; continue; }
      total = total + Money::parse(found->second.get<std::string>());
      ++contributors;
    }
    result["daily"].push_back(row);
    if (complete) {
      pnls.push_back(total);
      worst.push_back(curve.size());
      if (total > Money{}) ++wins;
      if (total < Money{}) ++losses;
    } else curve_complete = false;
    if (curve_complete) {
      cumulative = cumulative + total;
      peak = std::max(peak, cumulative);
      drawdown = std::max(drawdown, peak - cumulative);
    }
    curve.push_back({{"date", date}, {"pnl", complete ? json(total.str()) : json(nullptr)},
        {"cumulative", curve_complete ? json(cumulative.str()) : json(nullptr)}, {"contributors", contributors}});
  }
  std::stable_sort(worst.begin(), worst.end(), [&](std::size_t a, std::size_t b) {
    return Money::parse(curve[a].at("pnl").get<std::string>()) < Money::parse(curve[b].at("pnl").get<std::string>());
  });
  if (worst.size() > 10) worst.resize(10);
  json worst_days = json::array();
  for (const auto index : worst) worst_days.push_back(curve[index]);
  result["combined"] = {{"label", "Sum of independent single-playbook days. No shared buying power, risk limits or plan floor; not a joint account simulation."},
      {"daily_pnl", distribution(pnls)}, {"curve", curve}, {"max_drawdown", curve_complete ? json(drawdown.str()) : json(nullptr)},
      {"worst_days", worst_days}, {"day_win_rate", wins + losses ? json(static_cast<double>(wins) / static_cast<double>(wins + losses)) : json(nullptr)}};
  if (includes_joint) {
    result["combined"]["includes_joint"] = true;
    result["combined"]["label"] = "Sum of independent run days; includes already-joint account reports. No shared buying power, risk limits or plan floor across runs.";
  }
  return result;
}

std::vector<std::pair<json, BacktestRequest>> expand_backtest_grid(const BacktestRequest& request, const json& grid) {
  if (request.playbook.empty()) throw std::invalid_argument("Sweeps require one playbook");
  if (!grid.is_object() || grid.empty() || grid.size() > 4) throw std::invalid_argument("Grid requires 1-4 JSON-pointer fields");
  std::vector<std::pair<json, BacktestRequest>> variants{{json::object(), request}};
  for (const auto& [path, values] : grid.items()) {
    if (path.size() > 256 || (!path.starts_with("/structure/") && !path.starts_with("/management/") && !path.starts_with("/sizing/") &&
         !path.starts_with("/conditions/") && !path.starts_with("/guardrails/") && !path.starts_with("/window/")) ||
        !values.is_array() || values.empty() || values.size() > 8 || variants.size() * values.size() > 8)
      throw std::invalid_argument("Grid permits 2-8 variants over playbook rule fields");
    json unique = json::array();
    std::vector<std::pair<json, BacktestRequest>> next;
    for (const auto& value : values) {
      if (value.is_structured() || value.is_null() || std::find(unique.begin(), unique.end(), value) != unique.end())
        throw std::invalid_argument("Grid values must be distinct non-null scalars");
      unique.push_back(value);
      for (const auto& [parameters, base] : variants) {
        auto job = base;
        auto selected = parameters;
        selected[path] = value;
        auto& definition = job.playbooks["definitions"][job.playbook]["versions"].back();
        // Optional object fields may be added, then validated. Never grow an
        // array or create intermediate containers from a caller's pointer.
        const json::json_pointer pointer(path);
        auto& parent = definition.at(pointer.parent_pointer());
        if (parent.is_object()) parent[pointer.back()] = value;
        else definition.at(pointer) = value;
        next.emplace_back(std::move(selected), std::move(job));
      }
    }
    variants = std::move(next);
  }
  if (variants.size() < 2) throw std::invalid_argument("Grid requires at least two variants");
  for (const auto& [parameters, job] : variants) {
    (void)parameters;
    validate_playbook(job.playbooks.at("definitions").at(job.playbook).at("versions").back());
  }
  return variants;
}

json run_backtest_sweep(const BacktestRequest& request, const json& grid, const std::filesystem::path& directory,
    const std::atomic_bool& cancel, const std::function<void(std::size_t, std::string_view)>& progress) {
  const auto variants = expand_backtest_grid(request, grid);
  if (!std::filesystem::create_directory(directory)) throw std::invalid_argument("Sweep output directory already exists");
  json report{{"schema", 1}, {"mode", "sweep"}, {"simulated", true}, {"label", kBacktestLabel}, {"status", "running"},
      {"parameter_grid", grid}, {"variants", json::array()}, {"ranking", json::array()}, {"comparison", nullptr}};
  json runs = json::array();
  std::size_t completed = 0;
  if (progress) progress(0, "variants");
  for (std::size_t index = 0; index < variants.size(); ++index) {
    if (cancel) { report["status"] = "cancelled"; break; }
    std::ostringstream name;
    name << std::setfill('0') << std::setw(6) << index + 1;
    const auto id = name.str();
    const auto& [parameters, job] = variants[index];
    json result;
    try {
      result = run_backtest(job, directory / id, cancel, [&](std::size_t count, std::string_view) {
        if (progress) progress(completed + count, "variants");
      });
    } catch (const std::exception& error) {
      result = {{"status", "failed"}, {"message", error.what()}};
    }
    completed += job.days.size() * 2;
    const auto status = result.at("status");
    report["variants"].push_back({{"id", id}, {"parameters", parameters}, {"status", status}, {"report", result}});
    if (result.contains("summary")) {
      runs.push_back({{"id", id}, {"status", status}, {"report", result}});
      const auto mean = result.at("summary").at("daily_pnl").at("mean");
      if (status == "completed" && !mean.is_null()) report["ranking"].push_back({{"id", id}, {"parameters", parameters},
          {"mean_daily_pnl", mean}, {"pass_rate", result.at("summary").at("pass_rate")}});
    }
    if (status != "completed") report["status"] = status;
    if (runs.size() >= 2) report["comparison"] = compare_backtests(runs);
    auto& ranking = report["ranking"];
    std::stable_sort(ranking.begin(), ranking.end(), [](const auto& a, const auto& b) {
      return Money::parse(a.at("mean_daily_pnl").template get<std::string>()) > Money::parse(b.at("mean_daily_pnl").template get<std::string>());
    });
    write_backtest_report(directory / "report.json", report);
  }
  if (report.at("status") == "running") report["status"] = "completed";
  report["ranking_basis"] = "Completed variants by mean daily P&L, descending; ties keep grid order. Historical simulation, not a prediction.";
  write_backtest_report(directory / "report.json", report);
  return report;
}

BacktestRequest parse_backtest(const json& body, const json& catalogue,
    const std::vector<providers::Scenario>& scenarios, const std::filesystem::path& recordings, bool confined) {
  keys(body, {"playbook", "playbooks", "plan", "days", "scenarios", "scenario", "seed", "workers"});
  BacktestRequest result;
  result.workers = bounded(body.value("workers", json(4)), 16, "workers");
  if (body.contains("playbook") == body.contains("playbooks"))
    throw std::invalid_argument("Supply exactly one of playbook or playbooks");
  const bool joint = body.contains("playbooks");
  const auto selectors = joint ? body.at("playbooks") : json::array({body.at("playbook")});
  if (joint && (!selectors.is_array() || selectors.size() < 2 || selectors.size() > 8))
    throw std::invalid_argument("playbooks must be an array of 2–8 distinct playbook IDs");
  result.playbooks = {{"schema", 1}, {"definitions", json::object()}, {"modes", {{"main", json::object()}}}};
  for (const auto& named : selectors) {
    if (!named.is_string()) throw std::invalid_argument("playbook must be ID or ID@VERSION");
    const auto selector = named.get<std::string>();
    const auto at = selector.find('@');
    const auto id = selector.substr(0, at);
    if (result.playbooks.at("definitions").contains(id))
      throw std::invalid_argument("Duplicate playbook ID: " + id);
    if (!catalogue.at("definitions").contains(id)) throw std::invalid_argument(joint ? "Unknown playbook: " + id : "Unknown playbook");
    const auto& history = catalogue.at("definitions").at(id).at("versions");
    std::size_t version = history.size();
    if (at != std::string::npos) {
      const auto text = selector.substr(at + 1);
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), version);
      if (error != std::errc{} || end != text.data() + text.size()) throw std::invalid_argument("Invalid playbook version");
    }
    if (!version || version > history.size()) throw std::invalid_argument("Unknown playbook version");
    json versions = json::array();
    for (std::size_t index = 0; index < version; ++index) versions.push_back(history[index]);
    result.playbooks["definitions"][id] = {{"deleted", false}, {"versions", versions}};
    result.playbooks["modes"]["main"][id] = "auto";
    if (!joint) result.playbook = id;
  }
  const Playbooks validated({}, result.playbooks);
  (void)validated;
  const auto& plan = required(body, "plan", "plan");
  if (plan.is_string()) {
    const auto* preset = find_plan(plan.get<std::string>());
    if (!preset) throw std::invalid_argument("Unknown plan");
    result.config.initial_cash = preset->initial_cash;
    result.config.rules = preset->rules;
  } else {
    keys(plan, {"initial_cash", "rules", "fee_per_contract"});
    result.config.initial_cash = decimal(required(plan, "initial_cash", "plan initial_cash"), "plan initial_cash");
    json rules = result.config.rules;
    // The journal leaves out optional rules at their defaults; a plan may set them.
    // Held in a local: iterating items() of a temporary reads it after it is destroyed.
    const auto defaults = trading::optional_rule_defaults();
    for (const auto& [key, value] : defaults.items())
      if (!rules.contains(key)) rules[key] = value;
    // Program bookkeeping fields are also omitted from default journal rules.
    for (const auto* key : {"evaluation_fee", "reset_fee", "activation_fee", "max_resets"})
      if (!rules.contains(key)) rules[key] = 0;
    if (!required(plan, "rules", "plan rules").is_object()) throw std::invalid_argument("rules must be an object");
    const auto& custom = plan.at("rules");
    if (custom.contains("microscalp_seconds") != custom.contains("microscalp_percent"))
      throw std::invalid_argument("Set microscalp_seconds and microscalp_percent together");
    const auto choice = [](const json& value, const std::string& key, std::initializer_list<const char*> names) {
      for (const auto* name : names)
        if (value == name) return;
      throw std::invalid_argument("Unknown " + key);
    };
    for (const auto& [key, value] : plan.at("rules").items()) {
      if (!rules.contains(key)) throw std::invalid_argument("Unknown plan rule: " + key);
      if (key == "profit_target" || key == "max_drawdown" || key == "lock_balance" || key == "daily_loss_limit" ||
          key == "profitable_day_profit" || key == "max_trade_risk" ||
          key == "evaluation_fee" || key == "reset_fee" || key == "activation_fee")
        rules[key] = decimal(value, "plan rules " + key).micros();
      else if (key == "drawdown_mode") {
        choice(value, key, {"intraday", "end_of_day", "static"});
        rules[key] = value == "intraday" ? trading::DrawdownMode::Intraday
                   : value == "end_of_day" ? trading::DrawdownMode::EndOfDay : trading::DrawdownMode::Static;
      } else if (key == "profit_basis" || key == "daily_loss_basis" || key == "daily_loss_action" || key == "consistency_basis") {
        choice(value, key, key == "profit_basis" ? std::initializer_list<const char*>{"equity", "balance"}
                         : key == "daily_loss_basis" ? std::initializer_list<const char*>{"equity", "balance", "higher", "peak"}
                         : key == "daily_loss_action" ? std::initializer_list<const char*>{"lock", "fail"}
                                                      : std::initializer_list<const char*>{"total", "positive_days"});
        rules[key] = value;
      } else if (key == "trading_start" || key == "trading_end" || key == "flat_time" || key == "hold_cutoff") {
        if (value.is_null()) { rules[key] = nullptr; continue; }
        if (!value.is_string()) throw std::invalid_argument("plan rules " + key + " must be HH:MM New York time");
        const auto text = value.get<std::string>();
        const auto digit = [&](std::size_t i) { return text[i] >= '0' && text[i] <= '9'; };
        if (text.size() != 5 || text[2] != ':' || !digit(0) || !digit(1) || !digit(3) || !digit(4))
          throw std::invalid_argument("plan rules " + key + " must be HH:MM New York time");
        const auto h = (text[0] - '0') * 10 + text[1] - '0', m = (text[3] - '0') * 10 + text[4] - '0';
        if (h > 24 || m > 59 || (h == 24 && m != 0)) throw std::invalid_argument("Invalid plan trading hour");
        rules[key] = h * 60 + m;
      } else if (key == "events") {
        rules[key] = parse_calendar_events(value);
      } else if (key == "underlyings" || key == "hold_restrictions") {
        if (!value.is_array() || !std::all_of(value.begin(), value.end(), [](const json& symbol) { return symbol.is_string(); }))
          throw std::invalid_argument("plan rules underlyings must be an array of uppercase symbols");
        rules[key] = value;
      } else if (key == "margin") {
        if (value != "strategy" && value != "portfolio") throw std::invalid_argument("Unknown margin");
        rules[key] = value == "strategy" ? trading::MarginMode::Strategy : trading::MarginMode::Portfolio;
      } else if (key == "account_type") {
        if (value != "margin" && value != "cash" && value != "ira") throw std::invalid_argument("Unknown account_type");
        rules[key] = value;
      } else if (key == "phase") {
        choice(value, key, {"evaluation", "verification", "funded"});
        rules[key] = value == "funded" ? trading::Phase::Funded
                   : value == "verification" ? trading::Phase::Verification : trading::Phase::Evaluation;
      } else if (key == "size_scaling") {
        if (value.is_null()) { rules[key] = nullptr; continue; }
        keys(value, {"profit_percent", "payouts", "days", "increase_percent", "max_balance"});
        const auto integer = [&](const char* name) {
          const auto& v = required(value, name, name);
          if (!v.is_number_integer() || (v.is_number_unsigned() && v.get<std::uint64_t>() > INT64_MAX))
            throw std::invalid_argument(std::string("size_scaling ") + name + " must be a signed 64-bit integer");
          return v.get<std::int64_t>();
        };
        rules[key] = trading::SizeScaling{integer("profit_percent"), integer("payouts"), integer("days"),
            integer("increase_percent"), decimal(required(value, "max_balance", "max_balance"), "size_scaling max_balance")};
      } else if (key == "scaling") {
        if (!value.is_array()) throw std::invalid_argument("scaling must be an array");
        std::vector<trading::ScalingStep> steps;
        for (const auto& step : value) {
          keys(step, {"profit", "contracts"});
          const auto& contracts = required(step, "contracts", "scaling contracts");
          if (!contracts.is_number_integer() || (contracts.is_number_unsigned() && contracts.get<std::uint64_t>() > INT64_MAX))
            throw std::invalid_argument("scaling contracts must be a signed 64-bit integer");
          steps.push_back({decimal(required(step, "profit", "scaling profit"), "scaling profit"), contracts.get<std::int64_t>()});
        }
        rules[key] = steps;
      } else if (key == "payouts") rules[key] = payout_rules(value);
      else {
        if (key == "expiry_cutoff" && !value.is_number_integer()) throw std::invalid_argument("expiry_cutoff must be integer nanoseconds");
        // The rest keep their own type: text, true or false, or a whole number.
        const auto& current = rules.at(key);
        if (current.is_string() && !value.is_string()) throw std::invalid_argument("plan rules " + key + " must be text");
        if (current.is_boolean() && !value.is_boolean()) throw std::invalid_argument("plan rules " + key + " must be true or false");
        if (current.is_number() && !value.is_number_integer()) throw std::invalid_argument("plan rules " + key + " must be an integer");
        rules[key] = value;
      }
    }
    result.config.rules = rules.get<trading::AccountRules>();
    if ((result.config.rules.phase == trading::Phase::Funded) != plan.at("rules").contains("payouts"))
      throw std::invalid_argument("payouts are required for custom funded plans and forbidden otherwise");
    if (plan.contains("fee_per_contract")) result.config.fee_per_contract = decimal(plan.at("fee_per_contract"), "plan fee_per_contract");
  }
  trading::normalize_event_rules(result.config.rules);
  trading::validate_rules(result.config.rules);
  if (result.config.rules.size_scaling && result.config.rules.size_scaling->max_balance < result.config.initial_cash)
    throw trading::TradingError(trading::Reason::INVALID_RULES, "Account size scaling maximum must be at least the starting balance");
  const bool custom_funded = plan.is_object() && result.config.rules.phase == trading::Phase::Funded;
  const bool target_phase = result.config.rules.phase == trading::Phase::Evaluation ||
      (plan.is_object() && result.config.rules.phase == trading::Phase::Verification);
  if (result.config.initial_cash <= Money{} || result.config.fee_per_contract < Money{} ||
      (!custom_funded && (!target_phase || result.config.rules.profit_target <= Money{})))
    throw std::invalid_argument("Backtests require positive cash, nonnegative fees and an evaluation/verification target or custom funded rules");
  if (body.contains("days") == body.contains("scenarios")) throw std::invalid_argument("Supply days or scenarios, exclusively");
  json days;
  if (body.contains("scenarios")) {
    const auto count = bounded(body.at("scenarios"), 252, "scenarios");
    const auto seed = seed_value(required(body, "seed", "seed"));
    if (seed > UINT64_MAX - count + 1) throw std::invalid_argument("Scenario seeds overflow");
    std::vector<providers::Scenario> selected;
    for (const auto& scenario : scenarios)
      if ((!body.contains("scenario") && !scenario.overnight && scenario.sessions.empty()) ||
          (body.contains("scenario") && body.at("scenario") == scenario.id)) selected.push_back(scenario);
    if (selected.empty()) throw std::invalid_argument("No matching scenarios");
    auto date = selected.front().date;
    days = json::array();
    for (unsigned index = 0; index < count; ++index) {
      while (!(md::trading_date(md::new_york_to_utc(date, 12, 0)) == date)) date = md::date_from_days(md::days_since_epoch(date) + 1);
      days.push_back({{"scenario", selected[index % selected.size()].id}, {"date", md::format_date(date)}, {"seed", std::to_string(seed + index)}});
      date = md::date_from_days(md::days_since_epoch(date) + 1);
    }
  } else {
    if (body.contains("seed") || body.contains("scenario")) throw std::invalid_argument("Put seeds and scenario IDs in each day");
    days = body.at("days");
  }
  if (!days.is_array() || days.empty() || days.size() > 252) throw std::invalid_argument("days must contain 1–252 entries");
  std::optional<md::Date> previous;
  for (const auto& entry : days) {
    BacktestDay day;
    if (entry.contains("file")) {
      keys(entry, {"file"});
      if (!entry.at("file").is_string()) throw std::invalid_argument("day file must be a recording name");
      const auto name = entry.at("file").get<std::string>();
      if (confined) {
        if (recordings.empty()) throw std::invalid_argument("Recording directory is unavailable");
        const auto resolved = resolve_static_file(recordings, "/" + name);
        if (resolved.status != 200) throw std::invalid_argument("Recording must be a file inside the recordings directory");
        day.file = resolved.path;
      } else day.file = recordings / name;
      if (!std::filesystem::is_regular_file(day.file)) throw std::invalid_argument("Recording does not exist");
      md::RecordingReader reader(day.file);
      providers::ReplayBatches batches(reader, reader.header().subscription);
      const auto first = batches.next();
      if (!first || first->time <= 0) throw std::invalid_argument("Recording has no market time");
      day.date = md::trading_date(first->time);
      if (result.config.rules.max_volume_percent != 0) {
        const auto carries_volume = [&](const auto& batch) {
          return std::any_of(batch.events.begin(), batch.events.end(), [&](const auto& event) {
            const auto* volume = std::get_if<md::OptionVolume>(&event);
            return volume && volume->ts > 0 && md::trading_date(volume->ts) == day.date &&
                std::isfinite(volume->contracts) && volume->contracts >= 0 &&
                volume->contracts <= 9'007'199'254'740'991.0 && std::trunc(volume->contracts) == volume->contracts;
          });
        };
        bool volume = carries_volume(*first);
        while (!volume) {
          const auto batch = batches.next();
          if (!batch) break;
          volume = carries_volume(*batch);
        }
        if (!volume) throw std::invalid_argument("max_volume_percent requires current-date option volume in every backtest recording");
      }
    } else {
      // prepare() generates current-revision scenarios with current-date option volume.
      keys(entry, {"scenario", "date", "seed"});
      const auto& id = required(entry, "scenario", "day scenario");
      for (const auto& scenario : scenarios) if (id == scenario.id) day.scenario = scenario;
      if (!day.scenario) throw std::invalid_argument("Unknown scenario");
      if (!day.scenario->sessions.empty())
        throw std::invalid_argument("Scenario " + day.scenario->id + " plays several sessions, and a backtest day is one; play it in Replay");
      day.date = entry.contains("date") ? date_value(entry.at("date")) : day.scenario->date;
      day.seed = seed_value(required(entry, "seed", "day seed"));
    }
    if (previous && day.date <= *previous) throw std::invalid_argument("Days must have distinct, increasing trading dates");
    previous = day.date;
    result.days.push_back(std::move(day));
  }
  return result;
}

void write_backtest_report(const std::filesystem::path& file, const json& report) {
  const auto temporary = file.string() + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  output << report.dump(2) << '\n';
  output.close();
  if (!output) throw std::runtime_error("Cannot write backtest report");
  std::filesystem::rename(temporary, file);
}

json run_backtest(const BacktestRequest& request, const std::filesystem::path& directory,
    const std::atomic_bool& cancel, const std::function<void(std::size_t, std::string_view)>& progress) {
  if (request.days.empty() || request.days.size() > 252 || !request.workers || request.workers > 16)
    throw std::invalid_argument("Backtest bounds exceeded");
  if (!std::filesystem::create_directory(directory)) throw std::invalid_argument("Backtest output directory already exists");
  for (const auto* child : {"days", "attempts", "inputs"}) std::filesystem::create_directory(directory / child);
  if (progress) progress(0, "days");
  const bool joint = request.playbook.empty();
  json definitions = json::array();
  for (const auto& record : request.playbooks.at("definitions")) definitions.push_back(record.at("versions").back());
  std::vector<Prepared> prepared(request.days.size());
  std::vector<json> results(request.days.size());
  std::vector<std::string> errors(request.days.size());
  std::atomic_size_t next{0};
  std::size_t completed = 0;
  std::mutex progress_mutex;
  const auto advance = [&](std::string_view phase) {
    const std::lock_guard lock(progress_mutex);
    ++completed;
    if (progress) progress(completed, phase);
  };
  const auto consume = [&](Desk& desk, const Prepared& input, const BacktestDay& day, bool attempt, const md::Subscription& subscription,
      EntryReasons& entries) {
    md::RecordingReader reader(input.file);
    providers::ReplayBatches batches(reader, subscription);
    std::size_t count = 0;
    while (const auto batch = batches.next()) {
      check_cancel(cancel);
      if (md::trading_date(batch->time) != day.date) throw std::invalid_argument("Each recording must contain exactly one trading day");
      desk.replay_batch(batch->events, batch->received, batch->time);
      check_desk(desk);
      entries.observe(*desk.trading_view());
      ++count;
      if (attempt && desk.trading_view()->snapshot->evaluation.status != trading::EvaluationStatus::Active) break;
    }
    if (!reader.diagnostic().empty()) throw std::runtime_error(reader.diagnostic());
    if (!count) throw std::invalid_argument("Day has no replay events");
  };
  {
    Workers workers;
    for (std::size_t worker = 0; worker < std::min<std::size_t>(request.workers, request.days.size()); ++worker) {
      workers.threads.emplace_back([&] {
        while (!cancel.load()) {
          const auto index = next.fetch_add(1);
          if (index >= request.days.size()) break;
          try {
            prepared[index] = prepare(request.days[index], directory, index);
            const TemporaryRecording generated{request.days[index].scenario ? prepared[index].file : std::filesystem::path{}};
            check_cancel(cancel);
            const auto journal = "days/" + numbered(index) + ".jsonl";
            Measurements measurements{request.config.initial_cash, {}, {}};
            md::RecordingReader reader(prepared[index].file);
            Desk desk("replay (" + reader.header().provider + ")", reader.header().capabilities, reader.header().subscription,
                options_for(request, prepared[index], directory / journal, measurements));
            desk.start_trading();
            check_desk(desk);
            EntryReasons entries;
            consume(desk, prepared[index], request.days[index], false, reader.header().subscription, entries);
            auto result = result_for(desk, request, directory, journal, measurements, entries);
            result["date"] = md::format_date(request.days[index].date);
            result["input"] = json::parse(prepared[index].identity);
            if (result["input"].contains("seed")) result["input"]["seed"] = std::to_string(request.days[index].seed);
            results[index] = std::move(result);
            advance("days");
          } catch (const Cancelled&) { break; }
          catch (const std::exception& error) { errors[index] = error.what(); }
        }
      });
    }
  }
  json report{{"schema", 2}, {"simulated", true}, {"label", kBacktestLabel}, {"actor", request.actor},
      {"config", request.config}, {"analytics", request.analytics}, {"dividends", request.dividends},
      {"days", results}, {"attempts", json::array()}, {"errors", json::array()}, {"status", "completed"}};
  if (joint) {
    report["schema"] = 3;
    report["mode"] = "joint";
    report["playbooks"] = definitions;
  } else report["playbook"] = definitions.at(0);
  report["input_set"] = json::array();
  for (std::size_t index = 0; index < request.days.size(); ++index) {
    // Preserve supplied dates even when a day was cancelled before preparation.
    const auto& day = request.days[index];
    json input = nullptr;
    if (day.scenario) input = json::parse(scenario_input(*day.scenario, day.date, day.seed));
    else if (!prepared[index].identity.empty()) input = json::parse(prepared[index].identity);
    if (input.is_object() && input.contains("seed")) input["seed"] = std::to_string(day.seed);
    report["input_set"].push_back({{"date", md::format_date(day.date)}, {"input", input}});
  }
  for (std::size_t index = 0; index < errors.size(); ++index)
    if (!errors[index].empty()) report["errors"].push_back({{"day", index}, {"message", errors[index]}});
  if (cancel.load()) report["status"] = "cancelled";
  else if (!report["errors"].empty()) report["status"] = "failed";
  else {
    try {
      std::vector<std::string> symbols;
      for (const auto& day : request.days) {
        const auto names = day.scenario ? day.scenario->symbols : md::RecordingReader(day.file).header().subscription.underlyings;
        for (const auto& name : names)
          if (std::find(symbols.begin(), symbols.end(), name) == symbols.end()) symbols.push_back(name);
      }
      const md::Subscription subscription{symbols, 0, 0};
      std::size_t index = 0, attempt_index = 0;
      while (index < request.days.size()) {
        check_cancel(cancel);
        const auto first = index;
        const auto journal = "attempts/" + numbered(attempt_index++) + ".jsonl";
        Measurements measurements{request.config.initial_cash, {}, {}};
        EntryReasons entries;
        prepared[index] = prepare(request.days[index], directory, index);
        const TemporaryRecording generated{request.days[index].scenario ? prepared[index].file : std::filesystem::path{}};
        md::RecordingReader reader(prepared[index].file);
        Desk desk("replay (" + reader.header().provider + ")", reader.header().capabilities, subscription,
            options_for(request, prepared[index], directory / journal, measurements));
        desk.start_trading();
        check_desk(desk);
        json rows = json::array();
        while (index < request.days.size()) {
          check_cancel(cancel);
          const auto start = desk.trading_view()->snapshot;
          if (index != first) {
            prepared[index] = prepare(request.days[index], directory, index);
            md::RecordingReader next_reader(prepared[index].file);
            desk.replay_source(prepared[index].identity, next_reader.header());
          }
          const TemporaryRecording day_generated{request.days[index].scenario ? prepared[index].file : std::filesystem::path{}};
          consume(desk, prepared[index], request.days[index], true, subscription, entries);
          rows.push_back(evaluation_day(*start, *desk.trading_view()->snapshot, request.config.rules, request.days[index], index));
          ++index;
          advance("attempts");
          if (desk.trading_view()->snapshot->evaluation.status != trading::EvaluationStatus::Active) break;
        }
        auto result = result_for(desk, request, directory, journal, measurements, entries);
        for (auto& row : rows) {
          const auto date = row.at("date").get<std::string>();
          for (const auto& trip : result.at("rule_trips")) {
            const auto time = md::parse_datetime(trip.at("time").get<std::string>(), md::Zone::Utc);
            if (time && md::format_date(md::trading_date(*time)) == date) row["rule_trips"].push_back(trip);
          }
          for (const auto* collection : {"trades", "stock_trades"}) {
            for (const auto& trade : result.at(collection)) {
              for (const auto* key : {"opened", "closed"}) {
                if (trade.at(key).is_null()) continue;
                const auto time = md::parse_datetime(trade.at(key).get<std::string>(), md::Zone::Utc);
                if (time && md::format_date(md::trading_date(*time)) == date) {
                  auto& count = row[std::string(key) == "opened" ? "trades_opened" : "trades_closed"];
                  count = count.get<std::size_t>() + 1;
                }
              }
            }
          }
        }
        result["day_rows"] = std::move(rows);
        result["first_day"] = first;
        result["last_day"] = index - 1;
        result["days"] = index - first;
        report["attempts"].push_back(std::move(result));
      }
    } catch (const Cancelled&) { report["status"] = "cancelled"; }
    catch (const std::exception& error) {
      report["status"] = "failed";
      report["errors"].push_back({{"day", nullptr}, {"message", error.what()}});
    }
  }
  report["journals"] = json::array();
  for (const auto* child : {"days", "attempts"}) {
    std::vector<std::string> journals;
    for (const auto& entry : std::filesystem::directory_iterator(directory / child))
      if (entry.path().extension() == ".jsonl") journals.push_back(std::string(child) + "/" + entry.path().filename().string());
    std::sort(journals.begin(), journals.end());
    for (const auto& journal : journals) report["journals"].push_back(journal);
  }
  report["summary"] = aggregate(report.at("days"), report.at("attempts"));
  if (joint) {
    auto& breakdown = report["summary"]["per_playbook"] = json::object();
    for (const auto& definition : definitions) {
      const auto id = definition.at("id").get<std::string>();
      const auto tag = playbook_tag(definition);
      json trades = json::array(), reasons = json::object(), trips = json::array();
      Money realised, marked;
      bool complete = true;
      for (const auto& day : report.at("days")) {
        if (day.is_null()) continue;
        const auto& own = day.at("per_playbook").at(id);
        realised = realised + Money::parse(own.at("realised_pnl").get<std::string>());
        if (own.at("marked_pnl").is_null()) complete = false;
        else marked = marked + Money::parse(own.at("marked_pnl").get<std::string>());
        for (const auto& trade : day.at("trades")) if (trade.at("tag") == tag) trades.push_back(trade);
        for (const auto& trip : own.at("rule_trips")) trips.push_back(trip);
        for (const auto& [key, reason] : own.at("entry_reasons").items()) reasons[day.at("date").get<std::string>() + "/" + key] = reason;
      }
      breakdown[id] = attributed_stats(trades, realised, complete ? std::optional<Money>(marked) : std::nullopt, reasons, trips);
    }
  }
  write_backtest_report(directory / "report.json", report);
  return report;
}
}  // namespace openport::server
