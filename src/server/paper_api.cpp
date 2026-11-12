#include "paper_internal.hpp"

namespace openport::server {
std::string_view settle_by(const md::OptionContract& c, const trading::TradingSnapshot& s, const TradingView& view) {
  // A replay records the first opening print at or after 9:30 however late it comes.
  if (c.settlement == md::Settlement::AM && view.opening_settlement && (view.run || s.time <= c.expiry_time() + kOpeningPrintWait))
    return "opening_print";
  // A published special opening quotation is still fetched while its date is within
  // the fetcher's lookback, however late Cboe posts the row.
  if (c.settlement == md::Settlement::AM &&
      std::find(view.published_opening_quotations.begin(), view.published_opening_quotations.end(), c.underlying) !=
          view.published_opening_quotations.end() &&
      md::days_since_epoch(md::trading_date(s.time)) - md::days_since_epoch(c.expiry) <= providers::kCboeSoqLookbackDays)
    return "soq";
  if (c.settlement == md::Settlement::PM &&
      (s.closing_prints.contains(c.underlying + " " + md::format_date(c.expiry)) ||
       s.time < md::new_york_to_utc(c.expiry, md::regular_close_hour(c.expiry), 0) + kLastPrintWait))
    return "closing_print";
  return "manual";
}
namespace paper_internal {
namespace {

/// SSVI surfaces for the underlyings a smile-dynamics read moves, from each one's
/// cached surface fit; the first such read for a snapshot fits it on this worker.
/// An underlying without analytics or a successful fit is left out (sticky strike).
SmileSurfaces smile_surfaces(const MetricsSource& source, const std::set<std::string>& underlyings) {
  SmileSurfaces result;
  for (const auto& symbol : underlyings) {
    const auto metrics = source.metrics(symbol);
    if (!metrics) continue;
    const auto cache = surface_cache(metrics);
    analytics::SsviFit fit;
    {
      const std::lock_guard lock(cache->mutex);
      if (!cache->ssvi) cache->ssvi = analytics::fit_ssvi(metrics->slices);
      fit = *cache->ssvi;
    }
    if (fit.status != analytics::SviStatus::Ok) continue;
    SmileSurface surface{fit.parameters, {}};
    for (std::size_t i = 0; i < fit.expiries.size() && i < metrics->slices.size(); ++i)
      if (std::isfinite(fit.expiries[i].theta) && metrics->slices[i].years > 0)
        surface.thetas.emplace_back(metrics->slices[i].years, fit.expiries[i].theta);
    // Repaired thetas are nondecreasing in T, so sorting by T keeps them so.
    std::sort(surface.thetas.begin(), surface.thetas.end());
    if (!surface.thetas.empty()) result.emplace(symbol, std::move(surface));
  }
  return result;
}

int reason_status(Reason reason) {
  if (reason == Reason::INVALID_RULES) return 400;
  if (reason == Reason::UNKNOWN_ORDER || reason == Reason::UNKNOWN_CONTRACT || reason == Reason::UNKNOWN_TRADE ||
      reason == Reason::UNKNOWN_ALERT) return 404;
  if (reason == Reason::ORDER_TERMINAL || reason == Reason::DUPLICATE_CLIENT_ID || reason == Reason::RESET_LIMIT) return 409;
  if (reason == Reason::JOURNAL_IO || reason == Reason::JOURNAL_CORRUPT ||
      reason == Reason::JOURNAL_LOCKED) return 503;
  return 422;
}
ApiResponse command_body(const TradingCommand& command, const TradingReply& reply);
/// A command's HTTP answer. It runs inside the engine's completion, where nothing
/// may throw: an exception there would leave the request unanswered. A share
/// order's cost past the money range answers 422 ARITHMETIC_OVERFLOW, as the
/// reducer answers its own overflows.
ApiResponse command_response(const TradingCommand& command, const TradingReply& reply) noexcept {
  try {
    return command_body(command, reply);
  } catch (const TradingError& error) {
    return api_error(reason_status(error.code()), std::string(to_string(error.code())), error.what());
  } catch (...) {
    return api_error(500, "INTERNAL_ERROR", "Internal error");
  }
}
ApiResponse command_body(const TradingCommand& command, const TradingReply& reply) {
  if (!reply.error_code.empty())
    return api_error(reply.error_code == "INVALID_REQUEST" ? 400 : reply.error_code == "ACCOUNT_PROTECTED" ? 403 :
                     reply.error_code == "ACCOUNT_DAMAGED" || reply.error_code == "ACCOUNT_ARCHIVED" || reply.error_code == "ACCOUNT_NOT_EMPTY" ||
                     reply.error_code == "LIMITS_REVISION" || reply.error_code == "ACCOUNTS_UNSUPPORTED" ? 409
                     : reply.error_code == "UNKNOWN_ACCOUNT" ? 404 : 503,
                     reply.error_code, reply.decision.message);
  if (!reply.decision.ok()) return api_error(reason_status(reply.decision.code),
      std::string(to_string(reply.decision.code)), reply.decision.message, reply.decision);
  const auto* preview_decision = reply.preview ? &reply.preview->decision : reply.stock_preview ? &reply.stock_preview->decision : nullptr;
  if (preview_decision && (preview_decision->code == Reason::FLAT_TIME || preview_decision->code == Reason::NEWS_BLACKOUT || preview_decision->code == Reason::HOLD_RESTRICTED))
    return api_error(422, std::string(to_string(preview_decision->code)), preview_decision->message, *preview_decision);
  if (!reply.account_result.empty()) return {200, reply.account_result};
  if (!reply.view || !reply.view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "No trading publication");
  const auto& view = *reply.view;
  const auto& s = *view.snapshot;
  json body{{"account_version", std::to_string(s.account_version)}};
  int status = 200;
  switch (command.kind) {
    case TradingCommand::Kind::UpdateAccount:
    case TradingCommand::Kind::DeleteAccount: break;  // handled above
    case TradingCommand::Kind::Submit:
    case TradingCommand::Kind::Cancel:
    case TradingCommand::Kind::Modify: {
      const auto it = std::find_if(s.recent_orders.begin(), s.recent_orders.end(), [&](const auto& o) { return o.id == reply.order_id; });
      if (it == s.recent_orders.end()) return api_error(503, "TRADING_UNAVAILABLE", "Order publication missing");
      body["order"] = order_json(*it, view);
      if (command.kind != TradingCommand::Kind::Cancel) {
        if (command.kind == TradingCommand::Kind::Submit && !reply.replayed) status = 201;
        body["fills"] = json::array();
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == it->id) body["fills"].push_back(fill_json(fill, view));
      }
      break;
    }
    case TradingCommand::Kind::CancelAll:
    case TradingCommand::Kind::ClosePositions: {
      body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      if (command.kind == TradingCommand::Kind::CancelAll) {
        // Share orders have their own IDs (GET /api/stocks/orders).
        body["cancelled_stock_orders"] = json::array();
        for (auto id : reply.cancelled_stock_orders) body["cancelled_stock_orders"].push_back(std::to_string(id));
        break;
      }
      // Each closing order with its outcome: a rejection carries its reason.
      body["orders"] = json::array();
      body["fills"] = json::array();
      for (auto id : reply.created_orders) {
        const auto& order = s.recent_orders.at(static_cast<std::size_t>(id - 1));
        body["orders"].push_back(order_json(order, view));
        for (const auto& fill : s.recent_fills)
          if (fill.order_id == id) body["fills"].push_back(fill_json(fill, view));
      }
      // Delivered shares: those it closed, and those still held that it could not.
      body["stock_fills"] = json::array();
      for (auto id : reply.stock_fills) body["stock_fills"].push_back(stock_fill_json(s.stock_fills.at(static_cast<std::size_t>(id - 1))));
      body["kept_stocks"] = json::array();
      for (const auto& stock : s.stocks)
        if (const auto kept = reply.kept_stocks.find(stock.position.symbol); kept != reply.kept_stocks.end())
          body["kept_stocks"].push_back({{"symbol", stock.position.symbol}, {"shares", stock.position.shares},
              {"reason", decision_json(kept->second)}});
      // Positions still open: the contracts its closes are still working, and why the rest are not.
      body["residuals"] = json::array();
      for (const auto& residual : reply.residuals)
        body["residuals"].push_back({{"symbol", residual.symbol}, {"underlying", underlying(view, residual.symbol)},
            {"quantity", residual.quantity}, {"working", residual.working}, {"reason", decision_json(residual.reason)}});
      break;
    }
    case TradingCommand::Kind::Playbook: body = json::parse(reply.playbook_result); break;
    case TradingCommand::Kind::SubmitStockOrder:
    case TradingCommand::Kind::CancelStockOrder: {
      if (!reply.order_id || *reply.order_id == 0 || *reply.order_id > s.stock_orders.size())
        return api_error(503, "TRADING_UNAVAILABLE", "Share order publication missing");
      const auto& o = s.stock_orders.at(static_cast<std::size_t>(*reply.order_id - 1));
      if (command.kind == TradingCommand::Kind::SubmitStockOrder && !reply.replayed) status = 201;
      body["stock_order"] = stock_order_json(o);
      // The orders it placed with it or by filling: its OCO partner and bracket exits.
      body["related_orders"] = json::array();
      for (const auto other : {o.oco, o.stop_loss, o.take_profit})
        if (other) body["related_orders"].push_back(stock_order_json(s.stock_orders.at(static_cast<std::size_t>(other - 1))));
      body["stock_fills"] = json::array();
      for (const auto& fill : s.stock_fills)
        if (fill.stock_order == o.id || (o.oco && fill.stock_order == o.oco)) body["stock_fills"].push_back(stock_fill_json(fill));
      break;
    }
    case TradingCommand::Kind::PreviewStockOrder:
    case TradingCommand::Kind::PreviewStock: {
      if (!reply.stock_preview) return api_error(503, "TRADING_UNAVAILABLE", "No share preview available");
      const auto& p = *reply.stock_preview;
      const auto shares = command.stock_order ? (command.stock_order->side == Side::Buy ? command.stock_order->shares : -command.stock_order->shares)
                                              : command.quantity;
      body = {{"account_version", std::to_string(s.account_version)}, {"simulated", true},
              {"decision", p.decision.ok() ? "ok" : to_string(p.decision.code)}, {"reason", decision_json(p.decision)},
              {"price", money(p.price)}, {"cost", [&]() -> json {
                 // A share order costs its limit, or a stop's level, at most; a market order the price.
                 const auto& o = command.stock_order;
                 const auto bound = o && o->limit_price ? o->limit_price : o && o->stop_price ? o->stop_price : p.price;
                 return bound ? json((*bound * shares).str()) : json(nullptr);
               }()},
              {"current", what_if_account_json(p.current, view.config.scenarios)},
              {"after", what_if_account_json(p.after, view.config.scenarios)}};
      break;
    }
    case TradingCommand::Kind::PreviewClose:
      if (!reply.flatten) return api_error(503, "TRADING_UNAVAILABLE", "No flatten preview available");
      body = flatten_preview_json(*reply.flatten, view);
      body["account_version"] = std::to_string(s.account_version);
      break;
    case TradingCommand::Kind::WhatIf: {
      if (!reply.what_if) return api_error(503, "TRADING_UNAVAILABLE", "No what-if available");
      body = what_if_json(*reply.what_if, command.candidate_names, view.config.scenarios);
      body["account_version"] = std::to_string(s.account_version);
      break;
    }
    case TradingCommand::Kind::Preview:
    case TradingCommand::Kind::PreviewChange:
      if (!reply.preview) return api_error(503, "TRADING_UNAVAILABLE", "No preview available");
      body = preview_json(*reply.preview);
      body["account_version"] = std::to_string(s.account_version);
      break;
    case TradingCommand::Kind::Guardrails:
    case TradingCommand::Kind::Limits: body = risk_json(view); break;
    case TradingCommand::Kind::Trip:
    case TradingCommand::Kind::Reset:
      body["kill"] = kill_json(s); body["cancelled_orders"] = json::array();
      for (auto id : reply.cancelled_orders) body["cancelled_orders"].push_back(std::to_string(id));
      break;
    case TradingCommand::Kind::Settle: body["position_closed"] = true; break;
    case TradingCommand::Kind::ResetAccount:
    case TradingCommand::Kind::Halt:
    case TradingCommand::Kind::CashTransfer:
    case TradingCommand::Kind::Payout: body = account_json(view); break;
    case TradingCommand::Kind::Exercise:
    case TradingCommand::Kind::CloseStock:
    case TradingCommand::Kind::TradeStock:
    case TradingCommand::Kind::Abandon:
    case TradingCommand::Kind::ExerciseInstruction: body = portfolio_json(view); break;
    case TradingCommand::Kind::DayNote: {
      const auto key = md::format_date(command.day);
      body["day"] = key;
      body["note"] = day_notes_json(s).value(key, json{{"plan", ""}, {"review", ""}, {"time", nullptr}});
      break;
    }
    case TradingCommand::Kind::Group:
    case TradingCommand::Kind::Ungroup: {
      // The trades the round trips are in now.
      json trades = json::object();
      for (const auto& t : lifecycles(s.recent_fills, s.closures, view.contracts))
        if (std::find(command.trades.begin(), command.trades.end(), t.first_fill) != command.trades.end())
          trades[std::to_string(t.first_fill)] = trade_group(t, s.groups);
      body["groups"] = trades;
      break;
    }
    case TradingCommand::Kind::Annotate: {
      const auto key = (command.shares ? "s" : "") + std::to_string(command.trade);
      const auto a = s.annotations.find(key);
      body["trade"] = key;
      body["note"] = a == s.annotations.end() ? std::string{} : a->second.note;
      body["tags"] = a == s.annotations.end() ? json::array() : json(a->second.tags);
      body["mistakes"] = a == s.annotations.end() ? json::array() : json(a->second.mistakes);
      break;
    }
    case TradingCommand::Kind::CreateAlert: {
      const auto it = std::find_if(s.alerts.begin(), s.alerts.end(), [&](const Alert& a) { return a.id == reply.alert_id; });
      if (it == s.alerts.end()) return api_error(503, "TRADING_UNAVAILABLE", "Alert publication missing");
      status = 201;
      body["alert"] = alert_json(*it);
      break;
    }
    case TradingCommand::Kind::DeleteAlert: body["deleted"] = std::to_string(command.alert_id); break;
    case TradingCommand::Kind::CreateSandbox:
    case TradingCommand::Kind::CreateAccount:
      status = 201;
      body = {{"account", {{"id", reply.account}, {"name", command.name},
                           {"account_version", std::to_string(s.account_version)},
                           {"plan", nullable(view.config.rules.plan)}, {"plan_id", nullable(preset_id(view.config.initial_cash, view.config.rules))},
                           {"equity", s.equity.str()}}}};
      break;
  }
  return {status, body.dump()};
}
}  // namespace
}  // namespace paper_internal

ApiResponse api_error(int status, std::string code, std::string message, const Decision& evidence) noexcept {
  // A message may quote a parser or a file; printing it must not throw on a byte
  // that is not UTF-8, since a throw here escapes the error path itself.
  try {
    // Some reducer checks identify a single contract. The HTTP contract exposes
    // risk scope as the underlying, while the order itself carries the OSI.
    json error{{"code", code}, {"message", message},
        {"actual", evidence_value(evidence, false)},
        {"limit", evidence_value(evidence, true)}, {"scope", scope_json(evidence.scope)}};
    if (evidence.evidence) error["evidence"] = rule_evidence_json(*evidence.evidence);
    return {status, json{{"error", std::move(error)}}.dump(-1, ' ', false, json::error_handler_t::replace)};
  } catch (...) {
    return {status, R"({"error":{"code":"INTERNAL_ERROR","message":"Internal error","actual":null,"limit":null,"scope":null}})"};
  }
}

/// Whether `account` names an account the source publishes (the main one when empty).
bool known_account(const MetricsSource& source, const std::string& account) {
  if (account.empty() || account == kMainAccount) return true;
  const auto accounts = source.status().accounts;
  return std::any_of(accounts.begin(), accounts.end(), [&](const auto& a) { return a.id == account; });
}

ApiResponse command_refused(const MetricsSource& source) {
  const auto status = source.status();
  if (status.accounts.empty() && status.trading.reason == "PAPER_DISABLED")
    return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is turned off on this server (--no-paper)");
  // Safe to retry: an order resent with the same client_order_id and terms is answered once.
  auto busy = api_error(503, "TRADING_UNAVAILABLE", "Command inbox full or trading unavailable");
  busy.retry_after = kInboxRetrySeconds;
  return busy;
}
namespace paper_internal {
namespace {
/// Every API route with the methods it takes, HEAD wherever GET is; "{}" matches one
/// path segment, and the first match wins, so names come before patterns. Replay
/// routes are their live routes' (/api/replay/orders is /api/orders).
constexpr std::pair<std::string_view, std::string_view> kRouteMethods[] = {
    {"/api/status", "GET, HEAD"}, {"/api/volatility/scanner", "GET, HEAD"}, {"/api/calendar/events", "GET, HEAD"},
    {"/api/strategy-template", "GET, HEAD"}, {"/api/underlyings", "GET, HEAD, POST"},
    {"/api/underlyings/{}", "GET, HEAD, DELETE"}, {"/api/underlyings/{}/{}", "GET, HEAD"},
    {"/api/account", "GET, HEAD"}, {"/api/account/equity", "GET, HEAD"}, {"/api/account/pass-odds", "GET, HEAD"},
    {"/api/account/reset", "POST"}, {"/api/account/payout", "POST"}, {"/api/account/cash-flows", "POST"},
    {"/api/account/rules-compare", "POST"}, {"/api/portfolio", "GET, HEAD"}, {"/api/plans", "GET, HEAD"},
    {"/api/fills", "GET, HEAD"}, {"/api/fills.csv", "GET, HEAD"}, {"/api/fills/quality", "GET, HEAD"},
    {"/api/fills/calibration", "POST"}, {"/api/trades", "GET, HEAD"}, {"/api/trades.csv", "GET, HEAD"},
    {"/api/trades/group", "POST"}, {"/api/trades/ungroup", "POST"}, {"/api/trades/{}/note", "PUT"}, {"/api/days/{}/note", "PUT"},
    {"/api/risk", "GET, HEAD"}, {"/api/risk/profile", "GET, HEAD"}, {"/api/risk/scenarios", "GET, HEAD"},
    {"/api/risk/history", "GET, HEAD"}, {"/api/risk/hedges", "GET, HEAD"}, {"/api/risk/limits", "PUT"},
    {"/api/risk/guardrails", "PUT"}, {"/api/risk/kill", "POST"}, {"/api/settlements", "GET, HEAD, POST"},
    {"/api/orders", "GET, HEAD, POST"}, {"/api/orders/preview", "POST"}, {"/api/orders/what-if", "POST"},
    {"/api/orders/cancel", "POST"}, {"/api/orders/copy", "POST"}, {"/api/orders/copy/preview", "POST"},
    {"/api/orders/{}", "GET, HEAD, PUT, DELETE"}, {"/api/orders/{}/preview", "POST"},
    {"/api/positions/close", "POST"}, {"/api/positions/close/preview", "POST"}, {"/api/positions/exercise", "POST"},
    {"/api/positions/abandon", "POST"}, {"/api/positions/instruction", "POST"}, {"/api/stocks/close", "POST"},
    {"/api/stocks/trade", "POST"}, {"/api/stocks/trade/preview", "POST"}, {"/api/stocks/orders", "GET, HEAD, POST"},
    {"/api/stocks/orders/preview", "POST"}, {"/api/stocks/orders/{}", "DELETE"}, {"/api/market/halt", "POST"},
    {"/api/alerts", "GET, HEAD, POST"}, {"/api/alerts/{}", "DELETE"}, {"/api/accounts", "GET, HEAD, POST"},
    {"/api/accounts/{}", "PATCH, DELETE"}, {"/api/account-groups", "GET, HEAD"}, {"/api/account-groups/{}", "PUT, DELETE"},
    {"/api/playbooks", "GET, HEAD, POST"}, {"/api/playbooks/staged/{}/send", "POST"},
    {"/api/playbooks/staged/{}/dismiss", "POST"}, {"/api/playbooks/{}", "GET, HEAD, PUT, DELETE"},
    {"/api/playbooks/{}/mode", "PUT"}, {"/api/playbooks/{}/evaluate", "POST"},
    {"/api/playbooks/{}/signal-secret", "GET, HEAD, POST, DELETE"}, {"/api/signals/{}", "POST"},
    {"/api/rule-packs/validate", "POST"}, {"/api/sandboxes", "POST"}, {"/api/tokens/reload", "POST"},
    {"/api/notifications/test", "POST"}, {"/api/notifications/channels/{}", "PUT"},
    {"/api/notifications/history", "GET, HEAD"}, {"/api/earnings", "GET, HEAD"},
    {"/api/earnings/import", "POST"}, {"/api/earnings/imported", "DELETE"},
    {"/api/analysis/portfolios", "GET, HEAD, POST"}, {"/api/analysis/portfolios/{}", "GET, HEAD, DELETE"},
    {"/api/analysis/portfolios/{}/risk/{}", "GET, HEAD"}, {"/api/backtests", "GET, HEAD, POST, DELETE"},
    {"/api/backtests/compare", "GET, HEAD"}, {"/api/backtests/sweeps", "GET, HEAD, POST"},
    {"/api/backtests/sweeps/{}", "GET, HEAD, DELETE"}, {"/api/backtests/{}", "GET, HEAD, PUT, DELETE"},
    {"/api/replay", "GET, HEAD, POST, PUT, DELETE"}, {"/api/replay/packs", "GET, HEAD, POST"},
    {"/api/replay/packs/{}", "GET, HEAD"}, {"/api/replay/history", "GET, HEAD"},
    {"/api/replay/history/{}", "GET, HEAD, DELETE"}, {"/api/replay/history/{}/verify", "GET, HEAD, POST"}};

bool route_matches(std::string_view pattern, std::string_view path) {
  for (;;) {
    const auto wildcard = pattern.find("{}");
    if (wildcard == std::string_view::npos) return pattern == path;
    if (path.substr(0, wildcard) != pattern.substr(0, wildcard)) return false;
    pattern.remove_prefix(wildcard + 2);
    path.remove_prefix(wildcard);
    const auto segment = std::min(path.find('/'), path.size());
    if (segment == 0) return false;
    path.remove_prefix(segment);
  }
}
}  // namespace
}  // namespace paper_internal

std::string allowed_methods(std::string_view path) {
  std::string live;
  if (path.starts_with("/api/replay/") && !path.starts_with("/api/replay/packs") && !path.starts_with("/api/replay/history")) {
    live = "/api" + std::string(path.substr(std::string_view("/api/replay").size()));
    path = live;
  }
  for (const auto& [pattern, methods] : kRouteMethods)
    if (route_matches(pattern, path)) return std::string(methods);
  return {};
}

ApiResponse method_not_allowed(std::string_view path, std::string_view method) {
  auto allow = allowed_methods(path);
  // A route that takes the method but was not answered here is another host's.
  bool listed = false;
  for (std::string_view rest = allow; !rest.empty();) {
    const auto comma = rest.find(", ");
    listed = listed || rest.substr(0, comma) == method;
    rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 2);
  }
  if (allow.empty() || listed) return api_error(404, "NOT_FOUND", "Unknown endpoint");
  auto refused = api_error(405, "METHOD_NOT_ALLOWED", "This route takes " + allow);
  refused.allow = std::move(allow);
  return refused;
}

ApiResponse method_refused(std::string_view path, std::string message) {
  auto refused = api_error(405, "METHOD_NOT_ALLOWED", std::move(message));
  refused.allow = allowed_methods(path);
  return refused;
}
namespace paper_internal {
namespace {
constexpr std::size_t kMaxProfileCurves = 8;
constexpr std::size_t kMaxProfileLegs = 8;
bool underlying_symbol(std::string_view symbol) {
  return !symbol.empty() && symbol.size() <= 16 &&
         std::all_of(symbol.begin(), symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; });
}
std::vector<std::string_view> split_list(std::string_view text) {
  std::vector<std::string_view> items;
  for (std::size_t start = 0;;) {
    const auto comma = text.find(',', start);
    items.push_back(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return items;
}
}  // namespace
}  // namespace paper_internal

std::map<std::string, std::vector<HistoricalClose>> historical_closes(const MetricsSource& source,
    const std::set<std::string>& underlyings, Timestamp now) {
  std::map<std::string, std::vector<HistoricalClose>> history;
  for (const auto& symbol : underlyings) {
    std::map<md::Date, double> iv;
    if (const auto* series = source.series())
      for (const auto& row : series->rows(symbol, 0, now, true))
        if (std::isfinite(row.values[8]) && row.values[8] > 0) iv[md::new_york_time(row.time).date] = row.values[8] / 100;
    if (const auto* candles = source.candles())
      for (const auto& row : candles->daily_history(symbol)) {
        const auto date = md::new_york_time(row.start).date;
        history[symbol].push_back({date, row.close, iv.contains(date) ? std::optional(iv.at(date)) : std::nullopt});
      }
  }
  return history;
}

/// The beta of `underlying` to `reference`, by the first source that has one.
BetaEstimate profile_beta(const MetricsSource& source, const std::string& underlying, const std::string& reference,
                          const std::map<std::string, double>& given) {
  BetaEstimate beta;
  if (underlying == reference) { beta.beta = 1; beta.source = "reference"; return beta; }
  if (const auto it = given.find(underlying); it != given.end()) { beta.beta = it->second; beta.source = "given"; return beta; }
  if (same_index(underlying, reference)) { beta.beta = 1; beta.source = "index"; return beta; }
  const auto* candles = source.candles();
  if (!candles) return beta;
  beta = estimate_beta(candles->daily_history(underlying), candles->daily_history(reference), 252);
  if (!beta.source.empty() && beta.observations >= 20) { beta.source = "daily"; return beta; }
  beta = estimate_beta(candles->bars(underlying, BarInterval::FiveMinutes, 2000),
                       candles->bars(reference, BarInterval::FiveMinutes, 2000), 2000, 5 * md::kNanosPerMinute);
  if (!beta.source.empty() && beta.observations >= 30) { beta.source = "intraday"; return beta; }
  return BetaEstimate{0, beta.correlation, beta.observations, ""};
}
namespace paper_internal {
namespace {
json level_json(const std::optional<BreachLevel>& value, double spot) {
  if (!value) return nullptr;
  return {{"percent", number(value->percent)}, {"points", spot > 0 ? number(value->points) : json(nullptr)},
          {"price", spot > 0 ? number(spot + value->points) : json(nullptr)},
          {"touch_probability", value->touch_probability ? number(*value->touch_probability) : json(nullptr)}};
}

json historical_risk_json(const TradingView& view, const MetricsSource& source) {
  const auto& s = *view.snapshot;
  std::map<std::string, Position> positions;
  std::map<std::string, StockPosition> stocks;
  std::map<std::string, double> prices;
  std::set<std::string> held;
  for (const auto& p : s.positions) {
    positions.emplace(p.position.contract.osi_symbol(), p.position);
    held.insert(p.position.contract.underlying);
  }
  for (const auto& stock : s.stocks) {
    stocks.emplace(stock.position.symbol, stock.position); held.insert(stock.position.symbol);
    if (stock.fresh && stock.mark) prices[stock.position.symbol] = stock.mark->dollars();
  }
  const auto ledger = Ledger::restore({}, positions, stocks);
  // Adjusted series reprice as the standard series that pay the same.
  const DeliverablesScope adjusted(&s.deliverables);
  std::optional<Money> floor = view.config.rules.max_drawdown > Money{} ? std::optional(s.evaluation.floor) : std::nullopt;
  if (s.soft_floor) floor = floor ? std::max(*floor, *s.soft_floor) : s.soft_floor;
  json stress = json::array();
  for (const auto& scenario : historical_stresses()) {
    const auto value = stress_book(ledger, view.valuations, scenario.factors, 0, s.equity, floor,
        s.time, view.config.limits.max_valuation_age, prices, view.config.rules);
    const bool complete = value.complete && s.valuation_complete;
    json missing = json::array();
    for (const auto& symbol : held) if (!scenario.factors.contains(symbol)) missing.push_back(symbol);
    stress.push_back({{"id", scenario.id}, {"name", scenario.name}, {"from", scenario.from}, {"to", scenario.to},
        {"label", scenario.label}, {"source_note", scenario.source_note}, {"sources", scenario.sources},
        {"complete", complete}, {"missing_underlyings", missing}, {"pnl", complete ? number(value.pnl) : json(nullptr)},
        {"floor_breach", complete && value.floor_breach ? json(*value.floor_breach) : json(nullptr)},
        {"margin", complete ? money(value.margin) : json(nullptr)}, {"clamped", value.clamped}});
  }
  const auto history = historical_closes(source, held, s.time);
  const auto risk = s.valuation_complete ? historical_risk(ledger, view.valuations, history,
      s.time, view.config.limits.max_valuation_age, prices) : HistoricalRisk{};
  return {{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
    {"stress", stress}, {"sticky_strike", tail_risk_json(risk.sticky_strike)}, {"atm_vol_change", tail_risk_json(risk.atm_vol_change)},
    {"model", "Derived 1-day historical simulation, up to 252 completed closes per factor; at least 20 paired returns for 95% and 100 for 99%. Black-76 sticky strike with one calendar day of decay; ATM variant adds observed 30d ATM IV changes uniformly by tenor. Empirical inverse-CDF VaR and fractional upper-tail ES in dollars, floored at zero. No liquidity, path or settlement simulation. Stress margin is the held-book requirement after the instantaneous shock, excluding working orders."}};
}

/// Prose for a model string: "sticky strike", "sticky moneyness" or "sticky delta".
std::string dynamics_label(SmileDynamics dynamics) {
  auto text = std::string(to_string(dynamics));
  std::replace(text.begin(), text.end(), '_', ' ');
  return text;
}

/// GET /api/risk/profile: the held book's value across moves of one underlying, or
/// of a benchmark with every underlying beta-weighted to it, on several dates.
ApiResponse risk_profile_response(const std::map<std::string, std::string>& query, const MetricsSource& source, std::string account, bool grid = false) {
  std::string underlying, benchmark = "SPY";
  std::vector<std::string> days_text = grid ? std::vector<std::string>{"0"} : std::vector<std::string>{"0", "1"};
  std::vector<double> vols{-5, 0, 5, 10};
  bool term_scaled = false, beta_weighted = !grid;
  double vol_points = 0, range = 10;
  int steps = 41;
  std::map<std::string, double> given;
  SmileDynamics dynamics = SmileDynamics::StickyStrike;
  // Hypothetical legs, by padded OSI symbol, valued with (or without) the held book.
  std::vector<std::pair<std::string, Quantity>> legs;
  bool held_book = true;
  bool valid = true;
  std::string invalid;  // the parameter at fault
  for (const auto& [key, value] : query) {
    if (key == "account" && valid_account(value)) account = value;
    else if (!grid && key == "legs") {
      for (const auto item : split_list(value)) {
        const auto colon = item.rfind(':');
        Quantity quantity = 0;
        const auto text = colon == std::string_view::npos ? std::string_view{} : item.substr(colon + 1);
        // One optional sign: from_chars takes a minus itself, so "+-5" must be refused here.
        const auto digits = text.substr(text.starts_with('+') ? 1 : 0);
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), quantity);
        const std::string symbol(item.substr(0, colon == std::string_view::npos ? 0 : colon));
        // Bounds compared directly: std::abs of the most negative quantity overflows.
        valid &= colon != std::string_view::npos && !(text.starts_with('+') && digits.starts_with('-')) && ec == std::errc{} &&
                 end == digits.data() + digits.size() && quantity != 0 && quantity >= -10000 && quantity <= 10000 &&
                 symbol.size() == 21 && md::parse_osi(symbol).has_value() &&
                 std::none_of(legs.begin(), legs.end(), [&](const auto& leg) { return leg.first == symbol; });
        if (!valid) break;
        legs.emplace_back(symbol, quantity);
      }
      valid &= legs.size() <= kMaxProfileLegs;
    } else if (!grid && key == "held") {
      valid &= value == "true" || value == "false";
      held_book = value == "true";
    }
    else if (key == "underlying" && underlying_symbol(value)) underlying = value;
    else if (key == "benchmark" && (value == "SPY" || value == "SPX")) benchmark = value;
    else if (key == "days") {
      days_text.clear();
      for (const auto item : split_list(value)) days_text.emplace_back(item);
      valid &= days_text.size() <= kMaxProfileCurves;
    } else if (key == "term_scaled") {
      valid &= value == "true" || value == "false";
      term_scaled = value == "true";
    } else if (grid && key == "mode") {
      valid &= value == "parallel" || value == "beta";
      beta_weighted = value == "beta";
    } else if (grid && key == "vols") {
      vols.clear();
      for (const auto item : split_list(value)) {
        double vol = 0;
        valid &= query_decimal(std::string(item), vol) && std::abs(vol) <= 100;
        vols.push_back(vol);
      }
      valid &= vols.size() <= 21;
    } else if (key == "dynamics") {
      const auto parsed = parse_smile_dynamics(value);
      valid &= parsed.has_value();
      if (parsed) dynamics = *parsed;
    } else if (key == "iv") valid &= query_decimal(value, vol_points) && std::abs(vol_points) <= 100;
    else if (key == "range") valid &= query_decimal(value, range) && range > 0 && range <= 99;
    else if (key == "steps") {
      const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), steps);
      valid &= ec == std::errc{} && end == value.data() + value.size() && steps >= 3 && steps <= 201 && steps % 2 == 1;
    } else if (key == "betas") {
      for (const auto item : split_list(value)) {
        const auto colon = item.find(':');
        double beta = 0;
        const std::string symbol(item.substr(0, colon));
        valid &= colon != std::string_view::npos && underlying_symbol(symbol) && !given.contains(symbol) &&
                 query_decimal(std::string(item.substr(colon + 1)), beta) && std::abs(beta) <= 10;
        if (valid) given[symbol] = beta;
      }
    } else valid = false;
    if (!valid) {
      invalid = key;
      break;
    }
  }
  if (!valid) return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter " + invalid);
  if (!legs.empty() && underlying.empty()) return api_error(400, "INVALID_REQUEST", "legs require an underlying");
  if (legs.empty() && !held_book) return api_error(400, "INVALID_REQUEST", "held=false requires legs");
  if (!known_account(source, account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + account);
  const auto view = source.trading_view(account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  const auto& reference = underlying.empty() ? benchmark : underlying;
  // The underlyings in scope: each held contract's and share's.
  std::set<std::string> held;
  Timestamp first_expiry = std::numeric_limits<Timestamp>::max();
  for (const auto& p : s.positions) {
    if (!held_book || (!underlying.empty() && p.position.contract.underlying != underlying)) continue;
    held.insert(p.position.contract.underlying);
    if (p.position.contract.expiry_time() > s.time) first_expiry = std::min(first_expiry, p.position.contract.expiry_time());
  }
  for (const auto& stock : s.stocks)
    if (held_book && (underlying.empty() || stock.position.symbol == underlying)) held.insert(stock.position.symbol);
  // Each leg must be quoted in the underlying's analytics, which also value it.
  const auto leg_metrics = legs.empty() ? nullptr : source.metrics(underlying);
  std::vector<const analytics::OptionMetrics*> leg_options;
  for (const auto& [symbol, quantity] : legs) {
    const auto* option = leg_metrics ? find_option(*leg_metrics, symbol) : nullptr;
    if (!option || option->contract.underlying != underlying)
      return api_error(404, "UNKNOWN_CONTRACT", "No current " + underlying + " quote for leg " + symbol);
    leg_options.push_back(option);
    held.insert(underlying);
    if (option->contract.expiry_time() > s.time) first_expiry = std::min(first_expiry, option->contract.expiry_time());
  }
  ProfileConfig config;
  config.vol_points = vol_points;
  config.term_scaled = term_scaled;
  config.vol_floor = view->config.scenarios.vol_floor;
  for (int i = 0; i < steps; ++i) {
    const int offset = i - steps / 2;
    config.percent.push_back(offset == 0 ? 0.0 : range * offset / (steps / 2));
  }
  config.days.clear();
  std::vector<std::string> labels;
  for (const auto& text : days_text) {
    double days = 0;
    if (text == "expiry") {
      if (first_expiry == std::numeric_limits<Timestamp>::max()) continue;  // nothing expires in scope
      days = std::min(366.0, static_cast<double>(first_expiry - s.time) / static_cast<double>(md::kNanosPerDay));
    } else if (!query_decimal(text, days) || days < 0 || days > 366) {
      return api_error(400, "INVALID_REQUEST", "days must be numbers of days from 0 to 366, or expiry");
    }
    if (std::find(config.days.begin(), config.days.end(), days) != config.days.end()) continue;
    config.days.push_back(days);
    labels.push_back(text == "expiry" ? "expiry" : "");
  }
  if (config.days.empty()) {
    config.days.push_back(0);
    labels.emplace_back();
  }
  // The reference's spot and implied variances come from its analytics; a held
  // underlying's valuation stands in for a spot it has none of.
  const auto metrics = source.metrics(reference);
  double spot = metrics && metrics->spot > 0 && std::isfinite(metrics->spot) ? metrics->spot : 0.0;
  if (!(spot > 0)) {
    Timestamp latest = -1;
    for (const auto& p : s.positions) {
      const auto v = view->valuations.find(p.position.contract.osi_symbol());
      if (p.position.contract.underlying == reference && v != view->valuations.end() && v->second.time > latest && v->second.spot > 0) {
        spot = v->second.spot; latest = v->second.time;
      }
    }
    for (const auto& stock : s.stocks)
      if (stock.position.symbol == reference && stock.fresh && stock.mark) spot = stock.mark->dollars();
  }
  json horizons = json::array();
  for (const double days : config.days) {
    const auto target = probability_horizon(s.time, days);
    const auto variance = metrics ? implied_variance_until(*metrics, target) : std::nullopt;
    config.variances.push_back(variance);
    const auto cone = [&](double k) -> json {
      if (!variance || !(spot > 0)) return nullptr;
      return {{"low", number(spot * std::exp(-k * std::sqrt(*variance)))}, {"high", number(spot * std::exp(k * std::sqrt(*variance)))}};
    };
    horizons.push_back({{"until", md::format_timestamp(target)}, {"sigma", variance ? number(std::sqrt(*variance)) : json(nullptr)},
                        {"one_sd", cone(1)}, {"two_sd", cone(2)}});
  }
  // With legs, the profile is the held book in scope (none with held=false) and the
  // legs together, each leg valued from the analytics that quote it unless the
  // account already values that contract.
  std::map<std::string, Position> leg_positions;
  std::map<std::string, StockPosition> leg_stocks;
  std::map<std::string, double> share_prices;
  auto valuations = view->valuations;
  json leg_list = json::array();
  if (!legs.empty()) {
    if (held_book) {
      for (const auto& marked : s.positions)
        if (marked.position.contract.underlying == underlying) leg_positions.emplace(marked.position.contract.osi_symbol(), marked.position);
      for (const auto& marked : s.stocks) {
        if (marked.position.symbol != underlying) continue;
        leg_stocks.emplace(marked.position.symbol, marked.position);
        if (marked.fresh && marked.mark) share_prices[marked.position.symbol] = marked.mark->dollars();
      }
    }
    for (std::size_t i = 0; i < legs.size(); ++i) {
      const auto& [symbol, quantity] = legs[i];
      auto& position = leg_positions.try_emplace(symbol, Position{leg_options[i]->contract, 0, {}, {}, {}}).first->second;
      const bool was_long = position.quantity > 0;
      position.quantity += quantity;
      if (position.quantity == 0) leg_positions.erase(symbol);
      // A leg past the held size flips its side; the profile values marks, not
      // cost, so the flipped hypothetical position carries no basis.
      else if ((position.quantity > 0) != was_long) position.basis = position.fees = Money{};
      if (!valuations.contains(symbol)) valuations[symbol] = valuation_for(symbol, leg_options[i]->contract, leg_metrics);
      leg_list.push_back({{"symbol", symbol}, {"quantity", quantity}});
    }
  }
  // Analytics can publish ahead of the account's view of the same quotes.
  const auto now = legs.empty() ? s.time : std::max(s.time, leg_metrics->as_of);
  const auto leg_book = Ledger::restore({}, std::move(leg_positions), std::move(leg_stocks));
  // The rows' dollar deltas are the profiled book's, as its curves are: the account's
  // own risk without legs, and with them the ledger above (the legs alone when the
  // held book is left out), never the held book's beside curves that are not.
  const auto leg_risk = legs.empty() ? RiskSnapshot{}
      : portfolio_risk(leg_book, {}, view->contracts, valuations, view->config.limits, now, share_prices);
  const auto& book_risk = legs.empty() ? s.risk : leg_risk;
  json betas = json::array();
  double weighted = 0;
  bool weighted_complete = book_risk.complete;
  for (const auto& symbol : held) {
    const auto beta = underlying.empty() && beta_weighted ? profile_beta(source, symbol, reference, given) : BetaEstimate{1, analytics::kNaN, 0, "reference"};
    const auto bucket = book_risk.underlyings.find(symbol);
    // Legs that leave the profiled book flat in an underlying leave it no bucket: no delta.
    const std::optional<double> dollar_delta = bucket != book_risk.underlyings.end() ? std::optional(bucket->second.position.dollar_delta)
        : legs.empty() ? std::nullopt : std::optional(0.0);
    if (!beta.source.empty()) config.betas[symbol] = beta.beta;
    if (beta.source.empty() || !dollar_delta) weighted_complete = false;
    else weighted += *dollar_delta * beta.beta;
    betas.push_back({{"underlying", symbol}, {"beta", beta.source.empty() ? json(nullptr) : number(beta.beta)},
                     {"source", nullable(beta.source)}, {"observations", beta.observations},
                     {"correlation", number(beta.correlation)},
                     {"dollar_delta", dollar_delta ? number(*dollar_delta) : json(nullptr)},
                     {"weighted_dollar_delta", dollar_delta && !beta.source.empty() ? number(*dollar_delta * beta.beta) : json(nullptr)}});
  }
  // Other dynamics move each smile along its underlying's fitted SSVI surface.
  SmileSurfaces surfaces;
  if (dynamics != SmileDynamics::StickyStrike) surfaces = smile_surfaces(source, held);
  config.smile = {dynamics, &surfaces};
  if (grid) {
    if (!query.contains("range") && !query.contains("steps")) config.percent = view->config.scenarios.spot_percent;
    if (config.percent.size() * config.days.size() * vols.size() > 20000)
      return api_error(400, "INVALID_REQUEST", "Scenario grid is limited to 20000 cells");
    config.variances.clear();
    std::vector<RiskProfile> columns;
    bool complete = true;
    for (const double vol : vols) {
      config.vol_points = vol;
      // No floor root searches are needed for this grid.
      auto settings = view->config;
      settings.rules.max_drawdown = {};
      auto snapshot = s;
      snapshot.soft_floor.reset();
      columns.push_back(snapshot_profile(snapshot, settings, view->valuations, config, spot, underlying));
      complete &= columns.back().complete;
    }
    json slices = json::array();
    for (std::size_t day = 0; day < config.days.size(); ++day) {
      json pnl = json::array(), clamped = json::array();
      for (std::size_t move = 0; move < config.percent.size(); ++move) {
        json row = json::array(), clamps = json::array();
        for (const auto& column : columns) {
          row.push_back(complete ? number(column.curves[day].pnl[move]) : json(nullptr));
          clamps.push_back(column.curves[day].clamped);
        }
        pnl.push_back(row); clamped.push_back(clamps);
      }
      slices.push_back({{"days", config.days[day]}, {"spot_percent", config.percent}, {"vol_points", vols},
                        {"pnl", pnl}, {"clamped", clamped}, {"complete", complete}});
    }
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
        {"mode", beta_weighted ? "beta" : "parallel"}, {"benchmark", reference}, {"betas", betas},
        {"term_scaled", term_scaled}, {"slices", slices}, {"complete", complete},
        {"dynamics", to_string(dynamics)}, {"surface_fallback", columns.empty() ? json::array() : json(columns.front().surface_fallback)},
        {"model", std::string("Derived Black-76 ") + dynamics_label(dynamics) + " scenarios; time reduces carry and remaining life. "
                  "Term scaling uses sqrt(30/max(1,remaining days)). Trading rules retain their configured parallel, zero-day, sticky-strike grid."}}.dump()};
  }
  RiskProfile profile;
  if (legs.empty()) profile = snapshot_profile(s, view->config, view->valuations, config, spot, underlying);
  else {
    const bool floors = held_book;
    profile = risk_profile(leg_book, valuations, config, spot, s.equity,
        floors && view->config.rules.max_drawdown > Money{} ? std::optional(s.evaluation.floor) : std::nullopt,
        floors ? s.soft_floor : std::nullopt, now, view->config.limits.max_valuation_age, share_prices);
    if (held_book && !s.valuation_complete) {
      profile.complete = false;
      profile.room.reset(); profile.soft_room.reset();
      for (auto& curve : profile.curves) {
        std::fill(curve.pnl.begin(), curve.pnl.end(), 0.0);
        curve.down.reset(); curve.up.reset(); curve.soft_down.reset(); curve.soft_up.reset();
      }
    }
  }
  json prices = json::array();
  for (const double percent : config.percent) prices.push_back(spot > 0 ? number(spot * (1 + percent / 100)) : json(nullptr));
  json curves = json::array();
  for (std::size_t i = 0; i < profile.curves.size(); ++i) {
    const auto& curve = profile.curves[i];
    json pnl = json::array();
    for (const double value : curve.pnl) pnl.push_back(profile.complete ? number(value) : json(nullptr));
    curves.push_back({{"days", curve.days}, {"label", nullable(labels[i])}, {"time", md::format_timestamp(curve.time)},
                      {"pnl", pnl}, {"clamped", curve.clamped}, {"horizon", horizons[i]},
                      {"down", level_json(curve.down, spot)}, {"up", level_json(curve.up, spot)},
                      {"soft_down", level_json(curve.soft_down, spot)}, {"soft_up", level_json(curve.soft_up, spot)}});
  }
  return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
      {"underlying", nullable(underlying)}, {"benchmark", underlying.empty() ? json(benchmark) : json(nullptr)},
      {"reference", reference}, {"spot", spot > 0 ? number(spot) : json(nullptr)}, {"vol_points", vol_points},
      {"equity", s.equity.str()}, {"room", money(profile.room)}, {"soft_room", money(profile.soft_room)},
      {"percent", config.percent}, {"prices", prices}, {"curves", curves}, {"betas", betas},
      {"weighted_dollar_delta", weighted_complete ? number(weighted) : json(nullptr)},
      {"weighted_delta", weighted_complete && spot > 0 ? number(weighted / spot) : json(nullptr)},
      {"complete", profile.complete}, {"term_scaled", term_scaled}, {"held", held_book}, {"legs", leg_list},
      {"dynamics", to_string(dynamics)}, {"surface_fallback", profile.surface_fallback},
      {"model", std::string("Black-76 on each contract's smile IV ") +
          (dynamics == SmileDynamics::StickyStrike ? "plus the offset, sticky strike"
              : std::string("moved along its underlying's fitted SSVI surface (") + dynamics_label(dynamics) + ") plus the offset") +
          "; remaining life and carry shrink by the days; each underlying moves its beta times the reference's percent move; "
          "when term_scaled is true the IV offset is multiplied by sqrt(30/max(1,remaining days))"}}.dump()};
}

/// GET /api/risk/hedges: the smallest trade of each hedge that brings one risk
/// bucket's dollar delta or vega within its limit, or its worst scenario-grid cell
/// within the grid limit and above the floor, ranked by spread cost and residual risk.
ApiResponse hedge_response(const std::map<std::string, std::string>& query, const MetricsSource& source, std::string account) {
  std::string scope = "aggregate", target;
  double share = 1;
  std::map<std::string, double> given;
  for (const auto& [key, value] : query) {
    if (key == "account" && valid_account(value)) account = value;
    else if (key == "scope" && (value == "aggregate" || underlying_symbol(value))) scope = value;
    else if (key == "target" && (value == "delta" || value == "vega" || value == "grid")) target = value;
    else if (key == "utilisation" && query_decimal(value, share) && share > 0 && share <= 1) {}
    else if (key == "betas") {
      // As the risk profile's: SYMBOL:BETA overrides, to SPY.
      for (const auto item : split_list(value)) {
        const auto colon = item.find(':');
        double beta = 0;
        const std::string symbol(item.substr(0, colon));
        if (colon == std::string_view::npos || !underlying_symbol(symbol) || given.contains(symbol) ||
            !query_decimal(std::string(item.substr(colon + 1)), beta) || std::abs(beta) > 10)
          return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter " + key);
        given[symbol] = beta;
      }
    } else return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter " + key);
  }
  if (target.empty()) return api_error(400, "INVALID_REQUEST", "target must be delta, vega or grid");
  if (!known_account(source, account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + account);
  const auto view = source.trading_view(account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  const bool aggregate = scope == "aggregate";
  const auto found = s.risk.underlyings.find(scope);
  if (!aggregate && found == s.risk.underlyings.end()) return api_error(404, "UNKNOWN_SCOPE", "No exposure in " + scope);
  const auto& bucket = aggregate ? s.risk.aggregate : found->second;
  if (!s.risk.complete || !s.valuation_complete)
    return api_error(422, "HEDGE_UNAVAILABLE", "Hedges need fresh valuations of every position and open order");
  HedgeGoal goal;
  std::optional<Money> floor = view->config.rules.max_drawdown > Money{} ? std::optional(s.evaluation.floor) : std::nullopt;
  if (s.soft_floor) floor = floor ? std::max(*floor, *s.soft_floor) : s.soft_floor;
  if (target == "delta") goal.delta_limit = bucket.limits.dollar_delta * share;
  else if (target == "vega") goal.vega_limit = bucket.limits.vega * share;
  else {
    if (bucket.limits.worst_grid_loss) goal.grid_floor = -*bucket.limits.worst_grid_loss * share;
    if (floor) {
      const double room = -(s.equity - *floor).dollars();
      goal.grid_floor = goal.grid_floor ? std::max(*goal.grid_floor, room) : room;
    }
    if (!goal.grid_floor) return api_error(422, "HEDGE_UNAVAILABLE", "No grid loss limit or floor to hedge the grid to");
  }
  // The held book in scope on the rules' scenario grid.
  std::map<std::string, Position> positions;
  std::map<std::string, StockPosition> stocks;
  std::map<std::string, double> prices;
  for (const auto& p : s.positions)
    if (aggregate || p.position.contract.underlying == scope) positions.emplace(p.position.contract.osi_symbol(), p.position);
  for (const auto& stock : s.stocks) {
    if (stock.fresh && stock.mark) prices[stock.position.symbol] = stock.mark->dollars();
    if (aggregate || stock.position.symbol == scope) stocks.emplace(stock.position.symbol, stock.position);
  }
  const auto& grid_config = view->config.scenarios;
  const auto max_age = view->config.limits.max_valuation_age;
  const auto book_grid = scenario_grid(Ledger::restore({}, positions, stocks), view->valuations, grid_config, s.time, max_age, prices);
  if (!book_grid.complete) return api_error(422, "HEDGE_UNAVAILABLE", "The scenario grid cannot value every held position");
  HedgeBook book{bucket.reachable.delta_low, bucket.reachable.delta_high, bucket.reachable.vega_low, bucket.reachable.vega_high, {}};
  for (const auto& cell : book_grid.cells) book.cells.push_back(cell.pnl);
  const auto worst = [](const std::vector<double>& cells) { return cells.empty() ? json(nullptr) : number(*std::min_element(cells.begin(), cells.end())); };
  // Each underlying in scope's beta to SPY, by the risk profile's sources; the
  // beta-weighted dollar delta, held and reachable, needs every one of them.
  json betas = json::array();
  std::vector<std::string> missing;
  std::vector<BetaExposure> exposures;
  std::optional<double> weighted = 0.0;
  for (const auto& [symbol, held] : s.risk.underlyings) {
    if (!aggregate && symbol != scope) continue;
    const auto beta = profile_beta(source, symbol, "SPY", given);
    const bool known = !beta.source.empty();
    betas.push_back({{"underlying", symbol}, {"beta", known ? number(beta.beta) : json(nullptr)}, {"source", nullable(beta.source)},
                     {"observations", beta.observations}});
    exposures.push_back({held.reachable.delta_low, held.reachable.delta_high, known ? std::optional(beta.beta) : std::nullopt});
    if (!known) { missing.push_back(symbol); weighted.reset(); }
    else if (weighted) *weighted += held.position.dollar_delta * beta.beta;
  }
  const auto weighted_book = beta_weighted(book, exposures);
  std::string beta_note;
  if (!missing.empty()) {
    beta_note = "No beta to SPY for";
    for (std::size_t i = 0; i < missing.size(); ++i) beta_note += (i ? ", " : " ") + missing[i];
    // No betas= value, index family, or 20 daily or 30 intraday paired returns.
    beta_note += aggregate && target == "delta" ? ": beta hedges are sized on dollar delta one for one, with no beta-weighted delta"
                                                : ": no beta-weighted delta";
  }
  struct Choice {
    std::string name, kind, instrument, underlying, reason;
    bool beta = false;
    std::string expiry = {};
    HedgeUnit unit;
    const analytics::OptionMetrics* option = nullptr;
    double price = 0, mid = 0;
    std::optional<double> instrument_beta = {};  ///< To SPY, by the same sources as the book's.
  };
  std::vector<Choice> choices;
  const auto option_choice = [&](const std::string& underlying, pricing::OptionType type, bool beta) {
    Choice choice{std::string(beta ? "Beta hedge: " : "") + underlying + " ATM " + (type == pricing::OptionType::Call ? "call" : "put"),
                  "option", "", underlying, "", beta, {}, {}, nullptr};
    const auto metrics = source.metrics(underlying);
    const auto* option = metrics ? atm_option(*metrics, type) : nullptr;
    if (!option) { choice.reason = "No tradable two-sided at-the-money quote"; choices.push_back(choice); return; }
    const auto symbol = option->contract.osi_symbol();
    auto valuations = view->valuations;
    if (!valuations.contains(symbol)) valuations[symbol] = valuation_for(symbol, option->contract, metrics);
    const auto& valuation = valuations.at(symbol);
    choice.instrument = symbol;
    choice.option = option;
    // The expiry ID the chain lists the contract under, for the ticket.
    for (const auto& slice : metrics->slices)
      for (const auto& row : slice.strikes)
        if (&row.call == option || &row.put == option)
          choice.expiry = md::format_date(slice.expiry) + (option->contract.settlement == md::Settlement::AM ? "AM" : "PM") +
                          (slice.root.empty() ? "" : "-" + slice.root);
    choice.mid = option->mid;
    const auto unit_grid = scenario_grid(Ledger::restore({}, {{symbol, Position{option->contract, 1, {}, {}, {}}}}), valuations, grid_config,
        std::max(s.time, metrics->as_of), max_age, prices);
    if (!valid_valuation(valuation) || !unit_grid.complete) { choice.reason = "No fresh valuation of " + symbol; choices.push_back(choice); return; }
    choice.unit = {100 * valuation.delta * valuation.spot, 100 * valuation.vega, {},
                   100 * std::max(0.0, option->ask - option->mid), 100 * std::max(0.0, option->mid - option->bid)};
    for (const auto& cell : unit_grid.cells) choice.unit.cells.push_back(cell.pnl);
    choices.push_back(choice);
  };
  const auto share_choice = [&](const std::string& underlying, bool beta) {
    Choice choice{std::string(beta ? "Beta hedge: " : "") + underlying + " shares", "shares", underlying, underlying, "", beta, {}, {}, nullptr};
    const auto metrics = source.metrics(underlying);
    double price = prices.contains(underlying) ? prices.at(underlying) : metrics && metrics->spot > 0 ? metrics->spot : 0.0;
    if (md::is_index_underlying(underlying)) choice.reason = underlying + " is an index: it has no shares to trade";
    else if (!(price > 0) || !std::isfinite(price)) choice.reason = "No current price for " + underlying;
    else {
      auto share_prices = prices;
      share_prices[underlying] = price;
      const auto unit_grid = scenario_grid(Ledger::restore({}, {}, {{underlying, StockPosition{underlying, 1, {}, {}, {}}}}), view->valuations,
          grid_config, s.time, max_age, share_prices);
      choice.unit = {price, 0, {}, 0, 0};
      for (const auto& cell : unit_grid.cells) choice.unit.cells.push_back(cell.pnl);
      choice.price = choice.mid = price;
      if (!unit_grid.complete) choice.reason = "The scenario grid cannot value " + underlying + " shares";
    }
    choices.push_back(choice);
  };
  if (aggregate) {
    share_choice("SPY", true);
    for (const auto* index : {"SPY", "SPX"})
      for (const auto type : {pricing::OptionType::Put, pricing::OptionType::Call}) option_choice(index, type, true);
  } else {
    share_choice(scope, false);
    for (const auto type : {pricing::OptionType::Put, pricing::OptionType::Call}) option_choice(scope, type, false);
  }
  for (auto& choice : choices)
    if (const auto beta = profile_beta(source, choice.underlying, "SPY", given); !beta.source.empty()) choice.instrument_beta = beta.beta;
  const auto ratio = [](double value, std::optional<double> limit) -> json {
    if (!limit) return nullptr;
    return *limit == 0 ? (value == 0 ? json(0) : json(nullptr)) : number(value / *limit);
  };
  const std::optional<double> delta_limit = bucket.limits.dollar_delta, vega_limit = bucket.limits.vega;
  const std::optional<double> grid_limit = bucket.limits.worst_grid_loss;
  struct Ranked { json body; bool feasible; double cost, residual; };
  std::vector<Ranked> ranked;
  for (auto& choice : choices) {
    json body{{"name", choice.name}, {"kind", choice.kind}, {"underlying", choice.underlying}, {"instrument", nullable(choice.instrument)},
              {"beta_hedge", choice.beta}, {"expiry", nullable(choice.expiry)},
              {"strike", choice.option ? json(choice.option->contract.strike) : json(nullptr)},
              {"type", choice.option ? json(choice.option->contract.type == pricing::OptionType::Call ? "call" : "put") : json(nullptr)},
              {"sizing", target != "delta" ? target : choice.beta && weighted_book && choice.instrument_beta ? "beta_weighted_dollar_delta" : "dollar_delta"},
              {"instrument_beta", choice.instrument_beta ? number(*choice.instrument_beta) : json(nullptr)}};
    if (!choice.reason.empty()) {
      body.update(json{{"feasible", false}, {"reason", choice.reason}, {"side", nullptr}, {"quantity", nullptr}, {"price", nullptr},
                       {"premium", nullptr}, {"spread_cost", nullptr}, {"residual", nullptr}, {"order", nullptr}, {"stock_trade", nullptr},
                       {"exceeds_order_limit", false}});
      ranked.push_back({body, false, 0, 0});
      continue;
    }
    // A beta hedge brings the beta-weighted dollar delta within the limit, when
    // every underlying and the hedge have a beta; otherwise the one-for-one delta.
    const bool beta_sized = target == "delta" && choice.beta && weighted_book && choice.instrument_beta;
    auto sized_unit = choice.unit;
    if (beta_sized) sized_unit.delta *= *choice.instrument_beta;
    const long long most = choice.kind == "shares" ? 10'000'000 : 100'000;
    const auto units = smallest_hedge(hedge_constraints(beta_sized ? *weighted_book : book, sized_unit, goal), most);
    if (!units) {
      body.update(json{{"feasible", false}, {"reason", "No size of this hedge meets the target"}, {"side", nullptr}, {"quantity", nullptr},
                       {"price", nullptr}, {"premium", nullptr}, {"spread_cost", nullptr}, {"residual", nullptr}, {"order", nullptr},
                       {"stock_trade", nullptr}, {"exceeds_order_limit", false}});
      ranked.push_back({body, false, 0, 0});
      continue;
    }
    const auto q = *units;
    const bool buy = q >= 0;
    const double size = static_cast<double>(std::llabs(q));
    if (choice.option) choice.price = buy ? choice.option->ask : choice.option->bid;
    const double multiplier = choice.kind == "shares" ? 1 : 100;
    const double premium = (buy ? 1 : -1) * size * multiplier * choice.price;
    const double cost = size * (buy ? choice.unit.buy_cost : choice.unit.sell_cost);
    const auto after = hedged(book, choice.unit, q);
    const double delta = std::max(std::abs(after.delta_low), std::abs(after.delta_high));
    const double vega = std::max(std::abs(after.vega_low), std::abs(after.vega_high));
    const double loss = after.worst_cell ? std::max(0.0, -*after.worst_cell) : 0;
    // The beta-weighted residual, where the book and the hedge both have betas.
    std::optional<HedgeResidual> weighted_after;
    if (weighted_book && choice.instrument_beta) {
      auto unit = choice.unit;
      unit.delta *= *choice.instrument_beta;
      weighted_after = hedged(*weighted_book, unit, q);
    }
    const double weighted_delta = weighted_after ? std::max(std::abs(weighted_after->delta_low), std::abs(weighted_after->delta_high)) : 0;
    double residual = target == "delta" ? (beta_sized ? weighted_delta : delta) / std::max(1e-9, *delta_limit)
        : target == "vega" ? vega / std::max(1e-9, *vega_limit) : grid_limit ? loss / std::max(1e-9, *grid_limit) : loss;
    json order = nullptr, stock_trade = nullptr;
    const auto quantity = static_cast<long long>(size);
    if (q != 0 && choice.option)
      order = {{"symbol", choice.instrument}, {"side", buy ? "buy" : "sell"}, {"type", "limit"}, {"time_in_force", "day"},
               {"quantity", quantity}, {"limit_price", Money::from_double(choice.price).str()}};
    if (q != 0 && choice.kind == "shares") stock_trade = {{"symbol", choice.instrument}, {"side", buy ? "buy" : "sell"}, {"shares", quantity}};
    body.update(json{{"feasible", true}, {"reason", q == 0 ? json("Already within the target") : json(nullptr)},
        {"side", q == 0 ? json(nullptr) : json(buy ? "buy" : "sell")}, {"quantity", quantity},
        {"price", number(choice.price)}, {"premium", number(premium)}, {"spread_cost", number(cost)},
        {"residual", {{"dollar_delta_low", number(after.delta_low)}, {"dollar_delta_high", number(after.delta_high)},
                      {"vega_low", number(after.vega_low)}, {"vega_high", number(after.vega_high)},
                      {"worst_cell", after.worst_cell ? number(*after.worst_cell) : json(nullptr)},
                      {"delta_utilisation", ratio(delta, delta_limit)}, {"vega_utilisation", ratio(vega, vega_limit)},
                      {"grid_utilisation", ratio(loss, grid_limit)},
                      {"beta_weighted_dollar_delta", weighted && choice.instrument_beta
                           ? number(*weighted + static_cast<double>(q) * choice.unit.delta * *choice.instrument_beta) : json(nullptr)},
                      {"beta_weighted_dollar_delta_low", weighted_after ? number(weighted_after->delta_low) : json(nullptr)},
                      {"beta_weighted_dollar_delta_high", weighted_after ? number(weighted_after->delta_high) : json(nullptr)},
                      {"beta_weighted_delta_utilisation", weighted_after ? ratio(weighted_delta, delta_limit) : json(nullptr)}}},
        {"order", order}, {"stock_trade", stock_trade},
        {"exceeds_order_limit", choice.option && quantity > view->config.limits.max_order_contracts}});
    ranked.push_back({body, true, cost, residual});
  }
  std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
    if (a.feasible != b.feasible) return a.feasible;
    if (a.cost != b.cost) return a.cost < b.cost;
    return a.residual < b.residual;
  });
  json hedges = json::array();
  for (auto& item : ranked) hedges.push_back(std::move(item.body));
  return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"time", md::format_timestamp(s.time)},
      {"scope", scope}, {"target", target}, {"utilisation", share},
      {"current", {{"dollar_delta_low", number(book.delta_low)}, {"dollar_delta_high", number(book.delta_high)},
                   {"vega_low", number(book.vega_low)}, {"vega_high", number(book.vega_high)}, {"worst_cell", worst(book.cells)},
                   {"beta_weighted_dollar_delta", weighted ? number(*weighted) : json(nullptr)},
                   {"beta_weighted_dollar_delta_low", weighted_book ? number(weighted_book->delta_low) : json(nullptr)},
                   {"beta_weighted_dollar_delta_high", weighted_book ? number(weighted_book->delta_high) : json(nullptr)},
                   {"delta_limit", number(bucket.limits.dollar_delta)}, {"vega_limit", number(bucket.limits.vega)},
                   {"grid_floor", goal.grid_floor ? number(*goal.grid_floor) : json(nullptr)}}},
      {"betas", betas}, {"betas_missing", missing}, {"beta_note", nullable(beta_note)},
      {"hedges", hedges}, {"simulated", true},
      {"model", "Linear in the hedge's size: dollar delta and vega across what open orders can reach, and each cell of the rules' "
                "spot x volatility grid for the held book (pending orders' grid reservations are not included), with the hedge "
                "entered at the far side. The smallest whole size meeting the target is shown; ranked by spread cost, then residual "
                "risk. Fees are not included; the what-if and the ticket preview price them. Beta hedges trade SPY or SPX against "
                "the aggregate bucket: for a delta target they are sized so the beta-weighted dollar delta to SPY (each underlying's "
                "reachable dollar delta times its beta, from betas=, the index family, or daily then intraday history as in the risk "
                "profile) is within the limit, or one for one on dollar delta when an underlying has no beta (beta_note). The "
                "risk limit itself counts dollar delta one for one, so its utilisation is shown beside the beta-weighted one."}}.dump()};
}
}  // namespace
}  // namespace paper_internal

/// The rules a replay cannot evaluate: those that decide orders, positions or the
/// clock rather than the recorded equity path.
json not_replayed(const AccountRules& r) {
  json out = json::array();
  const auto add = [&](bool on, const char* name) { if (on) out.push_back(name); };
  add(r.open_loss_cap.has_value(), "open_loss_cap"); add(r.minute_volume_percent > 0, "minute_volume_percent");
  add(r.max_volume_percent > 0, "max_volume_percent"); add(r.max_contracts_held > 0 || !r.scaling.empty(), "contract_caps");
  add(r.require_stop_loss || r.max_trade_risk > Money{} || r.max_trade_risk_percent > 0, "trade_risk");
  add(r.buy_only || r.defined_risk || r.no_hedging || r.no_counter_positions, "strategies");
  add(r.min_hold_seconds > 0, "min_hold_seconds"); add(!r.underlyings.empty() || r.trading_start.has_value(), "instruments_and_hours");
  add(r.flat_time.has_value() || r.no_overnight || !r.hold_restrictions.empty() || !r.events.empty(), "holding_and_events");
  // Payouts are replayed as the recorded withdrawals; their own rules are not.
  add(r.payouts.review.has_value(), "payout_review"); add(r.payouts.after_payout != PayoutFloor::Keep, "after_payout");
  add(r.payouts.reset_percent != 100, "reset_percent");
  return out;
}
/// POST /api/account/rules-compare: the account's recorded attempt replayed
/// against up to eight rule sets, the presets named in `plans` and custom `rules`
/// (rules_json's shape with a name and starting balance), with its own rules first
/// unless include_current is false. See trading::replay_rules.
ApiResponse rules_compare(const ApiRequest& request, const MetricsSource& source) {
  // account=ID is the only query parameter, as on the other account routes: a
  // misspelt one must not compare the main account instead.
  const auto question = request.target.find('?');
  const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
  if (!pairs || std::any_of(pairs->begin(), pairs->end(), [](const auto& pair) { return pair.first != "account"; }))
    return api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter; this route takes only account=ID");
  const auto account = query_account(request.target, request.access.sandbox);
  if (!account) return api_error(400, "INVALID_REQUEST", "account must be an account ID");
  struct Set { std::string name, plan_id; Money initial_cash; AccountRules rules; };
  std::vector<Set> sets;
  bool current = true;
  try {
    const auto body = json::parse(request.body.empty() ? "{}" : request.body);
    fields(body, {}, {"plans", "rules", "include_current"});
    if (body.contains("include_current")) current = boolean_field(body, "include_current");
    if (body.contains("plans")) {
      const auto& plans = body.at("plans");
      if (!plans.is_array()) throw std::invalid_argument("plans must be an array of preset IDs");
      for (const auto& id : plans) {
        const auto* plan = id.is_string() ? find_plan(id.get<std::string>()) : nullptr;
        if (!id.is_string()) throw std::invalid_argument("plans must be an array of preset IDs");
        if (!plan) throw std::invalid_argument("Unknown plan preset: " + id.dump(-1, ' ', false, json::error_handler_t::replace));
        sets.push_back({plan->name, plan->id, plan->initial_cash, plan->rules});
      }
    }
    if (body.contains("rules")) {
      const auto& custom = body.at("rules");
      if (!custom.is_array()) throw std::invalid_argument("rules must be an array of {name, initial_cash, rules}");
      for (const auto& item : custom) {
        fields(item, {"name", "initial_cash", "rules"});
        auto rules = parse_rules(item.at("rules"));
        normalize_event_rules(rules);
        const auto name = string_field(item, "name");
        if (name.empty() || name.size() > 64) throw std::invalid_argument("A rule set's name is 1-64 bytes");
        const auto cash = decimal_field(item, "initial_cash");
        if (cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
        sets.push_back({name, preset_id(cash, rules), cash, rules});
      }
    }
  } catch (const TradingError& error) {
    return api_error(422, to_string(error.code()).data(), error.what());
  } catch (const std::exception& error) {
    return api_error(400, "INVALID_REQUEST", error.what());
  }
  if (!known_account(source, *account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + *account);
  const auto view = source.trading_view(*account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  const auto& e = s.evaluation;
  if (current) sets.insert(sets.begin(), {view->config.rules.plan.empty() ? "Current rules" : view->config.rules.plan,
      preset_id(e.starting_balance, view->config.rules), e.starting_balance, view->config.rules});
  if (sets.empty() || sets.size() > 8) return api_error(400, "INVALID_REQUEST", "Compare one to eight rule sets");
  // The attempt's closed whole trades, for the trade objectives from when each
  // closed, and its closed round trips, which microscalping reads one by one as the
  // reducer does (refresh_trade_objectives): options net of fees, shares with their
  // dividends, each whether or not the rest of its trade has closed.
  ReplayActivity activity;
  std::map<std::string, ReplayTrade> whole;
  std::set<std::string> open;
  for (const auto& life : lifecycles(s.recent_fills, s.closures, view->contracts)) {
    if (life.first_fill < e.first_fill) continue;
    const auto id = trade_group(life, s.groups);
    auto& trade = whole[id];
    trade.id = id;
    trade.pnl = trade.pnl + life.gross - life.fees;
    if (!life.closed) {
      open.insert(id);
      continue;
    }
    trade.closed = std::max(trade.closed, *life.closed);
    activity.round_trips.push_back({std::to_string(life.first_fill), *life.closed, life.gross - life.fees, *life.closed - life.opened});
  }
  for (const auto& [id, trade] : whole) if (!open.contains(id)) activity.trades.push_back(trade);
  // The attempt's share round trips, placed as the trades view places them: by the
  // stock fill that opened them, or by when they opened where the attempt kept no
  // boundary (runs before replay driver 12, outside microscalping).
  for (const auto& life : share_lifecycles(s.stock_fills, s.dividends))
    if (life.closed && from_attempt(e.first_stock_fill, e.started, life.fills.front(), life.opened))
      activity.round_trips.push_back({"s" + std::to_string(life.fills.front()), *life.closed, life.gross + life.dividends,
                                      *life.closed - life.opened});
  // The attempt's own executions, for the inactivity rule, as the reducer counts them
  // (count_trade): fills of the trader's orders, not the account's rule closes, and
  // share trades. Fills keep them whether or not the account's own rules read them.
  for (const auto& fill : s.recent_fills)
    if (fill.id >= e.first_fill && fill.order_id > 0 && fill.order_id <= s.recent_orders.size() &&
        !s.recent_orders[fill.order_id - 1].system)
      activity.executions.push_back(fill.time);
  for (const auto& fill : s.stock_fills)
    if (fill.source == StockSource::Trade && from_attempt(e.first_stock_fill, e.started, fill.id, fill.time))
      activity.executions.push_back(fill.time);
  const auto inputs = plan_inputs(s);
  json days = json::array();
  for (const auto& day : e.days) days.push_back(md::format_date(day.day));
  if (e.started > 0) days.push_back(md::format_date(e.day));
  json rows = json::array();
  for (const auto& set : sets) {
    const auto replay = replay_rules(e, inputs, activity, set.initial_cash, set.rules);
    json path = json::array();
    for (const auto& day : replay.days)
      path.push_back({{"day", md::format_date(day.day)}, {"outcome", day.outcome}, {"close", day.close.str()},
                      {"floor", positive(day.floor)}});
    json locked = json::array();
    for (const auto& day : replay.locked_days) locked.push_back(md::format_date(day));
    rows.push_back({{"name", set.name}, {"plan_id", nullable(set.plan_id)}, {"initial_cash", set.initial_cash.str()},
                    {"rules", rules_json(set.rules, set.initial_cash)},
                    {"status", status_name(replay.status)},
                    {"code", replay.status == EvaluationStatus::Active ? json(nullptr) : json(to_string(replay.code))},
                    {"message", nullable(replay.message)},
                    {"decided_day", replay.decided_day ? json(md::format_date(*replay.decided_day)) : json(nullptr)},
                    {"decided_at", time_or_null(replay.decided_at)},
                    {"decided_equity", replay.decided_day ? json(replay.decided_equity.str()) : json(nullptr)},
                    {"locked_days", locked}, {"closest_floor", money(replay.closest_floor)}, {"days", path},
                    {"not_replayed", not_replayed(set.rules)}});
  }
  return ApiResponse{200, json{{"account", *account}, {"attempt", e.attempt}, {"days", days}, {"sets", rows},
      {"basis", "Each recorded day's open, its recorded low and high in time order, and its close, as dollar P&L from each rule set's own starting balance"},
      {"label", "Replay of recorded equity; trades are not re-executed"}, {"simulated", true}}.dump()};
}
/// POST /api/rule-packs/validate: checks a versioned rule pack (docs/paper-trading.md)
/// and returns it normalized, every plan's rules as the server reads them.
ApiResponse validate_rule_pack(const ApiRequest& request, const MetricsSource& source) {
  json pack;
  try { pack = json::parse(request.body); }
  catch (const std::exception&) { return api_error(400, "INVALID_REQUEST", "Request body must be a JSON rule pack"); }
  json errors = json::array(), warnings = json::array(), plans = json::array();
  const auto fail = [&](std::string where, std::string message) { errors.push_back({{"where", std::move(where)}, {"message", std::move(message)}}); };
  json normalized;
  try {
    fields(pack, {"format", "version", "id", "name", "effective_from", "source", "verify", "plans"}, {"notes"});
    if (pack.at("format") != "openport.rule-pack") fail("format", "format must be openport.rule-pack");
    if (pack.at("version") != 1) fail("version", "Only version 1 rule packs are understood");
    const auto id = string_field(pack, "id");
    if (id.empty() || id.size() > 64 || !std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
      fail("id", "id is 1-64 lowercase letters, digits or dashes");
    const auto name = string_field(pack, "name");
    if (name.empty() || name.size() > 100) fail("name", "name is 1-100 bytes");
    const auto effective = string_field(pack, "effective_from");
    const auto date = csv_date(effective);
    if (!date) fail("effective_from", "effective_from must be a YYYY-MM-DD date");
    else if (*date > md::new_york_time(source.wall_time()).date) warnings.push_back("This pack takes effect on " + effective + ", after today");
    const auto url = string_field(pack, "source");
    if ((!url.starts_with("https://") && !url.starts_with("http://")) || url.size() > 2000 ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= ' ' || c == 127; }))
      fail("source", "source must be an http(s) URL of at most 2000 bytes");
    const auto verify = string_field(pack, "verify");
    if (verify.empty() || verify.size() > 2000) fail("verify", "verify must say, in at most 2000 bytes, how the terms were checked");
    if (pack.contains("notes") && (!pack.at("notes").is_string() || pack.at("notes").get<std::string>().size() > 4000))
      fail("notes", "notes must be text of at most 4000 bytes");
    const auto& list = pack.at("plans");
    if (!list.is_array() || list.empty() || list.size() > 16) throw std::invalid_argument("plans must list 1 to 16 plans");
    std::set<std::string> ids, names;
    for (const auto& plan : list) if (plan.is_object() && plan.contains("id") && plan.at("id").is_string()) ids.insert(plan.at("id").get<std::string>());
    std::set<std::string> seen;
    std::map<std::string, std::string> unlocks;  // Each plan's unlocked_by, by plan ID.
    for (std::size_t i = 0; i < list.size(); ++i) {
      const auto where = "plans[" + std::to_string(i) + "]";
      try {
        const auto& plan = list[i];
        fields(plan, {"id", "name", "initial_cash", "rules"}, {"summary", "unlocked_by"});
        const auto plan_id = string_field(plan, "id");
        if (plan_id.empty() || plan_id.size() > 64 || !seen.insert(plan_id).second)
          throw std::invalid_argument("Plan IDs are 1-64 bytes and unique within the pack");
        if (find_plan(plan_id)) throw std::invalid_argument("\"" + plan_id + "\" is a built-in preset's ID; pack plans keep their own");
        const auto plan_name = string_field(plan, "name");
        if (plan_name.empty() || plan_name.size() > 64 || !names.insert(plan_name).second)
          throw std::invalid_argument("Plan names are 1-64 bytes and unique within the pack");
        if (find_plan_named(plan_name)) throw std::invalid_argument("\"" + plan_name + "\" is a built-in preset's name; pack plans keep their own");
        const auto cash = decimal_field(plan, "initial_cash");
        if (cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
        if (!plan.at("rules").is_object()) throw std::invalid_argument("rules must be an object");
        auto rules_body = plan.at("rules");
        rules_body["plan"] = plan_name;
        auto rules = parse_rules(rules_body);
        normalize_event_rules(rules);
        validate_new_rules(rules);
        json unlocked = nullptr;
        if (plan.contains("unlocked_by") && !plan.at("unlocked_by").is_null()) {
          const auto prior = string_field(plan, "unlocked_by");
          if (!ids.contains(prior) || prior == plan_id) throw std::invalid_argument("unlocked_by must name another plan in this pack");
          unlocked = prior;
          unlocks[plan_id] = prior;
        }
        const auto summary = plan.contains("summary") ? string_field(plan, "summary") : std::string();
        if (summary.size() > 500) throw std::invalid_argument("summary is at most 500 bytes");
        auto read_back = rules_json(rules, cash);
        read_back.erase("plan_id");
        plans.push_back({{"id", plan_id}, {"name", plan_name}, {"summary", summary}, {"initial_cash", cash.str()},
                         {"rules", read_back}, {"unlocked_by", unlocked}});
      } catch (const TradingError& error) { fail(where, error.what()); }
      catch (const std::exception& error) { fail(where, error.what()); }
    }
    // A plan unlocked only by a plan it unlocks could never be started.
    std::set<std::string> cyclic;
    for (const auto& entry : unlocks) {
      const auto& start = entry.first;
      std::vector<std::string> chain{start};
      for (auto at = unlocks.find(start); at != unlocks.end(); at = unlocks.find(at->second)) {
        if (at->second == start) {
          if (cyclic.insert(*std::min_element(chain.begin(), chain.end())).second) {
            std::string path;
            for (const auto& link : chain) path += link + " -> ";
            fail("plans", "unlocked_by forms a cycle: " + path + start);
          }
          break;
        }
        if (std::find(chain.begin(), chain.end(), at->second) != chain.end()) break;
        chain.push_back(at->second);
      }
    }
    normalized = {{"format", "openport.rule-pack"}, {"version", 1}, {"id", id}, {"name", name}, {"effective_from", effective},
                  {"source", url}, {"verify", verify}, {"plans", plans}};
    if (pack.contains("notes")) normalized["notes"] = pack.at("notes");
  } catch (const TradingError& error) { fail("pack", error.what()); }
  catch (const std::exception& error) { fail("pack", error.what()); }
  return ApiResponse{200, json{{"valid", errors.empty()}, {"errors", errors}, {"warnings", warnings},
      {"pack", errors.empty() ? normalized : json(nullptr)},
      {"label", "Imported terms are the pack author's; this server checks only that they are well formed"}}.dump()};
}
namespace paper_internal {
namespace {
/// A from or to bound of a paper read, read the same on every route that takes one: a
/// New York date (YYYY-MM-DD, its first instant, or its last for `to`) or a UTC
/// instant (YYYY-MM-DDTHH:MM:SS, optionally with a fraction and Z or an offset).
std::optional<Timestamp> query_bound(const std::string& text, bool end) {
  if (const auto date = csv_date(text)) {
    const auto start = md::new_york_to_utc(end ? md::date_from_days(md::days_since_epoch(*date) + 1) : *date, 0, 0);
    if (start == md::kInvalidTimestamp) return std::nullopt;
    return end ? start - 1 : start;
  }
  const auto parsed = md::parse_datetime(text, md::Zone::Utc);
  if (!parsed || *parsed < 0) return std::nullopt;
  return parsed;
}
}  // namespace
}  // namespace paper_internal

std::optional<ApiResponse> paper_read(const ApiRequest& request, const MetricsSource& source) {
  const auto question = request.target.find('?');
  const auto path = request.target.substr(0, question);
  // GET /api/orders/{id}; /api/orders/cancel and the other POST routes are not orders.
  const bool order_detail = order_id_path(path);
  if (!order_detail && path != "/api/portfolio" && path != "/api/orders" && path != "/api/fills" && path != "/api/risk" &&
      path != "/api/trades.csv" && path != "/api/fills.csv" && path != "/api/account" && path != "/api/account/equity" &&
      path != "/api/trades" && path != "/api/settlements" && path != "/api/plans" && path != "/api/accounts" && path != "/api/risk/profile" &&
      path != "/api/alerts" && path != "/api/risk/scenarios" && path != "/api/risk/history" && path != "/api/risk/hedges" &&
      path != "/api/fills/quality" && path != "/api/stocks/orders") return {};
  if (path == "/api/risk/hedges") {
    const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
    if (!pairs) return api_error(400, "INVALID_REQUEST", "Duplicate or malformed query parameter");
    return hedge_response(*pairs, source, request.access.sandbox);
  }
  if (path == "/api/risk/profile" || path == "/api/risk/scenarios") {
    const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
    if (!pairs) return api_error(400, "INVALID_REQUEST", "Duplicate or malformed query parameter");
    return risk_profile_response(*pairs, source, request.access.sandbox, path == "/api/risk/scenarios");
  }
  const auto query = question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1);
  // Every route takes account=ID; orders take status=open|all and client_order_id;
  // trades take status=open|closed|all and attempt=current|all. Each key at most once.
  const auto pairs = query_parameters(query);
  const bool csv = path == "/api/trades.csv" || path == "/api/fills.csv";
  // Fills, orders, trades, the equity history and the CSV exports take from and to
  // (query_bound): long-lived accounts need not read their whole history at once.
  const bool bounded = csv || path == "/api/account/equity" || path == "/api/fills" || path == "/api/orders" || path == "/api/trades";
  std::string account = request.access.sandbox, status = "all", attempt = csv ? "all" : "current";
  std::optional<std::string> client_order_id;
  std::optional<Timestamp> since, until;
  std::optional<std::uint64_t> limit;
  Timestamp cursor_time = 0;
  std::uint64_t cursor_ordinal = 0;
  bool archived = false;
  SmileDynamics dynamics = SmileDynamics::StickyStrike;
  // The first fault, naming the parameter and what it takes.
  std::string problem = pairs ? "" : "Duplicate or malformed query parameter";
  for (const auto& [key, value] : pairs.value_or(std::map<std::string, std::string>{})) {
    if (!problem.empty()) break;
    if ((key == "limit" || key == "cursor") && path == "/api/account/equity") {
      const auto positive = [](std::string_view text, auto& number) {
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        return ec == std::errc{} && end == text.data() + text.size() && number > 0;
      };
      std::uint64_t count = 0;
      if (key == "limit") {
        if (!positive(value, count) || count > 2000) problem = "limit must be a whole number from 1 to 2000";
        else limit = count;
      } else {
        const auto colon = value.find(':');
        if (colon == std::string::npos || !positive(std::string_view(value).substr(0, colon), cursor_time) ||
            !positive(std::string_view(value).substr(colon + 1), cursor_ordinal))
          problem = "cursor must be the next value an earlier page returned";
      }
    } else if ((key == "from" || key == "to") && bounded) {
      const auto bound = query_bound(value, key == "to");
      if (!bound) problem = key + " must be a New York date (YYYY-MM-DD) or a UTC instant (YYYY-MM-DDTHH:MM:SSZ)";
      else if (key == "from") since = bound;
      else until = bound;
    } else if (key == "archived" && path == "/api/accounts") {
      if (value == "true" || value == "false") archived = value == "true";
      else problem = "archived must be true or false";
    } else if (key == "account" && path != "/api/accounts") {
      if (valid_account(value)) account = value;
      else problem = "account must be an account ID: lowercase letters, digits and single hyphens";
    } else if (key == "dynamics" && path == "/api/risk") {
      if (const auto named = parse_smile_dynamics(value)) dynamics = *named;
      else problem = "dynamics must be sticky_strike, sticky_moneyness or sticky_delta";
    } else if (key == "status" && (path == "/api/orders" || path == "/api/stocks/orders")) {
      if (value == "open" || value == "all") status = value;
      else problem = "status must be open or all";
    } else if (key == "client_order_id" && path == "/api/orders") {
      if (valid_client_order_id(value)) client_order_id = value;
      else problem = "client_order_id must be 1 to 128 bytes, none of them a control character";
    } else if (key == "status" && (path == "/api/trades" || path == "/api/trades.csv")) {
      if (value == "open" || value == "closed" || value == "all") status = value;
      else problem = "status must be open, closed or all";
    } else if (key == "attempt" && (path == "/api/trades" || path == "/api/trades.csv" || path == "/api/fills/quality")) {
      if (value == "current" || value == "all") attempt = value;
      else problem = "attempt must be current or all";
    } else {
      problem = "Unknown query parameter " + key + " for " + path;
    }
  }
  if (problem.empty() && since && until && *since > *until) problem = "from must not be after to";
  if (problem.empty() && cursor_time && !limit) problem = "cursor needs limit";
  if (!problem.empty()) return api_error(400, "INVALID_REQUEST", problem);
  if (path == "/api/plans") return ApiResponse{200, plans_json().dump()};
  if (path == "/api/accounts") return ApiResponse{200, json{{"accounts", accounts_json(source, request.access, archived)}}.dump()};
  if (!known_account(source, account)) return api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + account);
  const auto view = source.trading_view(account);
  if (!view || !view->snapshot) return api_error(503, "TRADING_UNAVAILABLE", "Paper trading is disabled or unavailable");
  const auto& s = *view->snapshot;
  if (order_detail) {
    OrderId id;
    try { id = identifier(std::string_view(path).substr(12)); }
    catch (const std::invalid_argument& error) { return api_error(400, "INVALID_REQUEST", error.what()); }
    if (!id || id > s.recent_orders.size()) return api_error(404, "UNKNOWN_ORDER", "Unknown order ID");
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)},
        {"order", order_json(s.recent_orders[id - 1], *view)}}.dump()};
  }
  if (path == "/api/risk/history") return ApiResponse{200, historical_risk_json(*view, source).dump()};
  if (path == "/api/account/equity") {
    json samples = json::array(), next = nullptr;
    Timestamp previous = 0;
    std::uint64_t ordinal = 0;
    std::string last;
    const auto first = std::lower_bound(view->equity_samples.begin(), view->equity_samples.end(), std::max(cursor_time, since.value_or(0)),
        [](const EquitySample& sample, Timestamp time) { return sample.time < time; });
    for (auto it = first; it != view->equity_samples.end(); ++it) {
      const auto& sample = *it;
      if (until && sample.time > *until) break;
      ordinal = sample.time == previous ? ordinal + 1 : 1;
      previous = sample.time;
      if ((since && sample.time < *since) || (until && sample.time > *until) ||
          sample.time < cursor_time || (sample.time == cursor_time && ordinal <= cursor_ordinal)) continue;
      if (limit && samples.size() == *limit) { next = last; break; }
      last = std::to_string(sample.time) + ":" + std::to_string(ordinal);
      samples.push_back({{"time", md::format_timestamp(sample.time)}, {"day", md::format_date(plan_trading_date(view->config.rules, sample.time))},
          {"attempt", sample.attempt}, {"equity", sample.equity.str()}, {"floor", money(sample.floor)}, {"peak", sample.peak.str()},
          {"target", money(sample.target)}, {"tomorrow_floor", money(sample.tomorrow_floor)},
          {"risk", sample.risk ? json{{"dollar_delta", number(sample.risk->exposure.dollar_delta)},
              {"dollar_gamma_1pct", number(sample.risk->exposure.dollar_gamma_1pct)}, {"vega", number(sample.risk->exposure.vega)},
              {"theta", number(sample.risk->exposure.theta)}, {"delta_utilisation", number(sample.risk->delta_utilisation)},
              {"vega_utilisation", number(sample.risk->vega_utilisation)},
              {"gamma_utilisation", sample.risk->gamma_utilisation ? number(*sample.risk->gamma_utilisation) : json(nullptr)},
              {"theta_utilisation", sample.risk->theta_utilisation ? number(*sample.risk->theta_utilisation) : json(nullptr)},
              {"grid_utilisation", sample.risk->grid_utilisation ? number(*sample.risk->grid_utilisation) : json(nullptr)},
              {"short_vega_utilisation", sample.risk->short_vega_utilisation ? number(*sample.risk->short_vega_utilisation) : json(nullptr)}} : json(nullptr)},
          {"fill", sample.stock_fill ? json("s" + std::to_string(sample.stock_fill)) : sample.fill ? json(std::to_string(sample.fill)) : json(nullptr)}});
    }
    return ApiResponse{200, json{{"samples", samples}, {"next", next}, {"error", nullable(view->equity_error)},
        {"error_time", time_or_null(view->equity_error_time)}, {"error_market_time", time_or_null(view->equity_error_market_time)},
        {"error_recovered", view->equity_error_recovered}}.dump()};
  }
  if (path == "/api/settlements") {
    json records = json::array();
    for (auto it = s.settlements.rbegin(); it != s.settlements.rend(); ++it) {
      const auto& c = it->contract;
      records.push_back({{"symbol", c.osi_symbol()}, {"underlying", c.underlying}, {"expiry", md::format_date(c.expiry)},
          {"settlement", c.settlement == md::Settlement::AM ? "AM" : "PM"}, {"value", it->value.str()},
          {"time", md::format_timestamp(it->time)}, {"quantity", it->quantity}, {"cash", it->cash.str()},
          {"realised", it->realised.str()}, {"fee", it->fee.str()}, {"source", it->source ? json(*it->source) : json(nullptr)}});
    }
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"settlements", records}}.dump()};
  }
  if (path == "/api/portfolio") return ApiResponse{200, portfolio_json(*view).dump()};
  if (path == "/api/stocks/orders") {
    json list = json::array();
    for (auto it = view->snapshot->stock_orders.rbegin(); it != view->snapshot->stock_orders.rend(); ++it)
      if (status != "open" || it->open()) list.push_back(stock_order_json(*it));
    return ApiResponse{200, json{{"account_version", std::to_string(view->snapshot->account_version)}, {"stock_orders", list}}.dump()};
  }
  if (path == "/api/risk") {
    if (dynamics == SmileDynamics::StickyStrike) return ApiResponse{200, risk_json(*view).dump()};
    std::set<std::string> held;
    for (const auto& p : s.positions) held.insert(p.position.contract.underlying);
    const auto surfaces = smile_surfaces(source, held);
    const auto grid = snapshot_scenarios(s, view->config, view->valuations, {dynamics, &surfaces});
    return ApiResponse{200, risk_json(*view, &grid, dynamics).dump()};
  }
  if (path == "/api/account") return ApiResponse{200, account_json(*view).dump()};
  if (path == "/api/trades") return ApiResponse{200, trades_json(*view, status, attempt == "current", since, until).dump()};
  if (path == "/api/fills/quality") return ApiResponse{200, fill_quality_json(*view, attempt == "current").dump()};
  if (path == "/api/alerts") {
    json alerts = json::array();
    for (const auto& alert : s.alerts) alerts.push_back(alert_json(alert));
    return ApiResponse{200, json{{"account_version", std::to_string(s.account_version)}, {"alerts", alerts}}.dump()};
  }
  if (csv) {
    json rows = json::array();
    const bool fills = path == "/api/fills.csv";
    if (fills) {
      for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it) rows.push_back(fill_json(*it, *view));
    } else {
      const auto trades = trades_json(*view, status, attempt == "current");
      std::map<std::string, OrderId> fill_orders;
      for (const auto& fill : s.recent_fills) fill_orders[std::to_string(fill.id)] = fill.order_id;
      std::map<OrderId, json> time_stops;
      for (const auto& order : s.recent_orders) {
        if (order.reason.code != Reason::PLAYBOOK_TIME_STOP && order.reason.code != Reason::PLAYBOOK_TRAILING_STOP &&
            order.reason.code != Reason::PLAYBOOK_DTE_STOP && order.reason.code != Reason::PLAYBOOK_DAYS_IN_TRADE_STOP &&
            !order.request.note.starts_with("Playbook automatic ")) continue;
        const auto related = order.parent ? order.parent : order.id;
        if (!time_stops.contains(related)) time_stops[related] = json::array();
        time_stops[related].push_back({{"order_id", std::to_string(order.id)}, {"reason", decision_json(order.reason)}, {"note", order.request.note}});
      }
      for (auto row : trades.at("trades")) {
        row["kind"] = "option";
        row["time_stop_orders"] = json::array();
        std::set<OrderId> related;
        for (const auto& fill : row.at("fills"))
          if (const auto found = fill_orders.find(fill.get<std::string>()); found != fill_orders.end()) related.insert(found->second);
        for (const auto id : related)
          if (const auto found = time_stops.find(id); found != time_stops.end())
            for (const auto& event : found->second) row["time_stop_orders"].push_back(event);
        row["time_stop_orders"] = row["time_stop_orders"].empty() ? "" : row["time_stop_orders"].dump();
        row["closures"] = row["closures"].empty() ? "" : row["closures"].dump();
        rows.push_back(std::move(row));
      }
      for (const auto& row : trades.at("share_trades")) rows.push_back(row);
    }
    return ApiResponse{200, paper_csv(rows, fills, account.empty() ? std::string(kMainAccount) : account,
                                    source.status().provider, s.account_version, since, until, view->run),
                       "text/csv; charset=utf-8", fills ? "fills.csv" : "trades.csv"};
  }
  json body{{"account_version", std::to_string(s.account_version)}};
  if (path == "/api/orders") {
    body["orders"] = json::array();
    // from and to bound when each order was accepted.
    for (auto it = s.recent_orders.rbegin(); it != s.recent_orders.rend(); ++it)
      if ((status != "open" || it->open()) && (!client_order_id || it->request.client_order_id == *client_order_id) &&
          (!since || it->accepted_at >= *since) && (!until || it->accepted_at <= *until))
        body["orders"].push_back(order_json(*it, *view));
  } else {
    body["fills"] = json::array();
    for (auto it = s.recent_fills.rbegin(); it != s.recent_fills.rend(); ++it)
      if ((!since || it->time >= *since) && (!until || it->time <= *until)) body["fills"].push_back(fill_json(*it, *view));
  }
  return ApiResponse{200, body.dump()};
}
namespace paper_internal {
namespace {
/// GET /api/underlyings/{symbol}/optimizer: a search can value thousands of
/// candidates, so it runs on the search pool and completes from there.
bool strategy_search(const ApiRequest& request, const MetricsSource& source, ApiCompletion& complete) {
  constexpr std::string_view prefix = "/api/underlyings/", suffix = "/optimizer";
  const auto question = request.target.find('?');
  const std::string_view path = std::string_view(request.target).substr(0, question);
  if (!path.starts_with(prefix) || !path.ends_with(suffix) || path.size() <= prefix.size() + suffix.size()) return false;
  const std::string symbol(path.substr(prefix.size(), path.size() - prefix.size() - suffix.size()));
  if (symbol.find('/') != std::string::npos) return false;
  const auto metrics = source.metrics(symbol);
  if (!metrics) return false;  // handle_api answers the missing underlying
  const auto query = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
  if (!query) { complete(api_error(400, "INVALID_REQUEST", "duplicate or malformed query parameter")); return true; }
  StrategySearchRequest search;
  try { search = parse_strategy_search(*query); }
  catch (const std::invalid_argument& failure) { complete(api_error(400, "INVALID_REQUEST", failure.what())); return true; }
  // A search stops early, answering what it valued, once its client has gone or the
  // server is stopping; one still queued at shutdown is dropped and answers 503.
  auto& pool = StrategySearchPool::shared();
  const bool queued = pool.submit([metrics, search, complete, cancel = request.cancelled, &pool] {
    ApiResponse response;
    const auto cancelled = [&] { return pool.stopping() || (cancel && cancel->load()); };
    try { response.body = search_strategies(*metrics, search, cancelled).dump(); }
    catch (const std::invalid_argument& failure) { response = api_error(400, "INVALID_REQUEST", failure.what()); }
    catch (const std::exception&) { response = api_error(500, "INTERNAL_ERROR", "Strategy search failed"); }
    // A client that closed the connection, even only its sending side, stopped the
    // search: say so, rather than answer as if the market had no candidates.
    if (cancel && cancel->load() && response.status == 200)
      response = api_error(503, "REQUEST_CANCELLED", "The search stopped when the client closed the connection; keep it open, sending side included, until the answer arrives");
    complete(std::move(response));
  }, [complete] { complete(api_error(503, "ENGINE_STOPPING", "Strategy searches are stopping")); });
  if (!queued) {
    auto busy = api_error(503, "SEARCH_BUSY", "Strategy searches are all busy; retry shortly");
    busy.retry_after = kInboxRetrySeconds;
    complete(std::move(busy));
  }
  return true;
}
json account_group_json(const AccountGroup& group) {
  json members = json::array();
  for (const auto& m : group.members) {
    members.push_back({{"account", m.account}});
    if (group.allocation == Allocation::Ratio) members.back()["ratio"] = m.ratio.str();
  }
  return {{"name", group.name}, {"allocation", group.allocation == Allocation::Equity ? "equity" : "ratio"}, {"members", members}};
}
AccountGroup parse_account_group(std::string name, const json& body) {
  fields(body, {"allocation", "members"});
  AccountGroup group;
  group.name = std::move(name);
  const auto allocation = string_field(body, "allocation");
  if (allocation != "ratio" && allocation != "equity") throw std::invalid_argument("allocation must be ratio or equity");
  group.allocation = allocation == "equity" ? Allocation::Equity : Allocation::Ratio;
  const auto& members = body.at("members");
  if (!members.is_array()) throw std::invalid_argument("members must be an array");
  for (const auto& m : members) {
    if (group.allocation == Allocation::Ratio) fields(m, {"account"}, {"ratio"});
    else fields(m, {"account"});
    GroupMember member{string_field(m, "account")};
    if (m.contains("ratio")) member.ratio = decimal_field(m, "ratio");
    group.members.push_back(std::move(member));
  }
  validate_group(group);
  return group;
}

/// The quantity an account's current attempt first submitted under `client_order_id`,
/// when it holds an order under that ID, which its reducer answers a retry from.
std::optional<Quantity> submitted_quantity(const TradingSnapshot& s, const std::string& client_order_id) {
  for (auto id = std::max<OrderId>(s.evaluation.first_order, 1); id <= s.recent_orders.size(); ++id) {
    const auto& o = s.recent_orders[static_cast<std::size_t>(id - 1)];
    if (o.request.client_order_id == client_order_id) return o.submission().quantity;
  }
  return std::nullopt;
}

/// 409 ACCOUNT_NAME_TAKEN when a new or renamed account would share another live
/// account's name, ignoring case and surrounding spaces (Main included): the account
/// switcher shows names, not IDs, so the two could not be told apart.
std::optional<ApiResponse> name_taken(const EngineStatus& status, const TradingCommand& command) {
  const bool naming = command.kind == TradingCommand::Kind::CreateAccount ||
      (command.kind == TradingCommand::Kind::UpdateAccount && !command.name.empty());
  if (!naming) return std::nullopt;
  const auto folded = [](std::string_view name) {
    auto text = trimmed_name(name);
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
  };
  const auto wanted = folded(command.name);
  for (const auto& other : status.accounts) {
    // Visitors' sandboxes are not in anyone's switcher; an account may change its own name's case.
    if (other.archived || other.replay || other.sandbox_idle_seconds || other.id == command.account) continue;
    if (folded(other.name) == wanted)
      return api_error(409, "ACCOUNT_NAME_TAKEN", "Account " + other.id + " is already named " + other.name + "; choose another name");
  }
  return std::nullopt;
}

/// 422 UNKNOWN_UNDERLYING for a share trade, order or close in a symbol this server
/// does not carry: no price for it will ever come, which the paper gate's "waiting
/// for market data" did not say. Refused here, it never reaches the desk or a journal.
std::optional<ApiResponse> uncarried_shares(const MetricsSource& source, const TradingCommand& command) {
  using Kind = TradingCommand::Kind;
  const bool shares = command.kind == Kind::TradeStock || command.kind == Kind::PreviewStock || command.kind == Kind::CloseStock ||
      command.kind == Kind::SubmitStockOrder || command.kind == Kind::PreviewStockOrder;
  if (!shares) return std::nullopt;
  const auto symbols = source.symbols();
  if (std::find(symbols.begin(), symbols.end(), command.symbol) != symbols.end()) return std::nullopt;
  return api_error(422, "UNKNOWN_UNDERLYING",
                   command.symbol + " is not an underlying of this server, so it has no share price; GET /api/status lists them");
}

/// One copied order's progress: each member's command and quantity, and its answer
/// once its account has replied. The last reply completes the request.
struct CopyFanOut {
  std::mutex mutex;
  AccountGroup group;
  OrderRequest order;
  bool preview = false;
  std::vector<Quantity> quantities;
  std::vector<std::optional<json>> results;
  std::size_t pending = 0;
  ApiCompletion complete;
  void finish() {
    json list = json::array();
    bool all = true;
    for (std::size_t i = 0; i < results.size(); ++i) {
      list.push_back(*results[i]);
      if (!list.back().value("ok", false)) all = false;
    }
    json body{{"group", group.name}, {"allocation", group.allocation == Allocation::Equity ? "equity" : "ratio"},
              {"client_order_id", order.client_order_id}, {"quantity", order.quantity}, {"preview", preview}, {"results", list}};
    // 207: some members were skipped or refused; each result says which and why.
    complete({all ? 200 : 207, body.dump()});
  }
};

/// /api/account-groups and /api/orders/copy: named groups of accounts and one order
/// mirrored to each of a group's accounts as its own submission.
bool account_group_api(const ApiRequest& request, MetricsSource& source, ApiCompletion& complete) {
  const auto question = request.target.find('?');
  const std::string path = request.target.substr(0, question);
  constexpr std::string_view prefix = "/api/account-groups";
  const bool groups_path = path == prefix || path.starts_with(std::string(prefix) + "/");
  const bool copy_path = path == "/api/orders/copy" || path == "/api/orders/copy/preview";
  if (!groups_path && !copy_path) return false;
  auto* groups = source.account_groups();
  if (!groups) { complete(api_error(404, "NOT_FOUND", "Account groups are not offered by this source")); return true; }
  if (!request.access.sandbox.empty()) {
    complete(api_error(403, "ACCOUNT_PROTECTED", "Sandbox visitors trade their own account only"));
    return true;
  }
  if (question != std::string::npos) { complete(api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter")); return true; }
  try {
    if (groups_path) {
      if (path == prefix) {
        if (request.method != "GET") { complete(method_not_allowed(path, request.method)); return true; }
        json list = json::array();
        for (const auto& group : groups->list()) list.push_back(account_group_json(group));
        complete({200, json{{"groups", list}}.dump()});
        return true;
      }
      const auto name = path.substr(prefix.size() + 1);
      if (!valid_group_name(name)) throw std::invalid_argument("A group name is 1-40 lowercase letters, digits or single hyphens");
      if (request.method == "PUT") {
        if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
        auto group = parse_account_group(name, strict_json(request.body));
        for (const auto& m : group.members)
          if (!known_account(source, m.account == kMainAccount ? std::string() : m.account)) {
            complete(api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + m.account));
            return true;
          }
        groups->put(group);
        complete({200, account_group_json(group).dump()});
      } else if (request.method == "DELETE") {
        if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
        complete(groups->remove(name) ? ApiResponse{200, json{{"deleted", name}}.dump()} : api_error(404, "UNKNOWN_GROUP", "No account group " + name));
      } else {
        complete(method_not_allowed(path, request.method));
      }
      return true;
    }
    if (request.method != "POST") { complete(method_not_allowed(path, request.method)); return true; }
    if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
    const auto body = strict_json(request.body);
    fields(body, {"group", "order"});
    const auto name = string_field(body, "group");
    const auto group = groups->find(name);
    if (!group) { complete(api_error(404, "UNKNOWN_GROUP", "No account group " + name)); return true; }
    auto order = parse_order(body.at("order"));
    if (!order.then.empty() || !order.oco.empty() || order.exits_only)
      throw std::invalid_argument("A copied order takes no then, oco or exits_only: each account sizes its own");
    // Equity allocation splits among the members whose positions are all marked; one
    // that is not is skipped with its reason, and the rest take the whole order. A copy
    // sent again is not split anew from equity that has moved: a member holding an order
    // under its client_order_id is sent that order's size, so it answers as first
    // submitted, and only what those orders leave is split among the others.
    std::vector<std::optional<Money>> equity;
    std::vector<bool> unmarked(group->members.size(), false);
    std::vector<std::optional<Quantity>> sent(group->members.size());
    Quantity held = 0;
    if (group->allocation == Allocation::Equity)
      for (std::size_t i = 0; i < group->members.size(); ++i) {
        const auto& m = group->members[i];
        const auto view = source.trading_view(m.account == kMainAccount ? std::string() : m.account);
        if (!view || !view->snapshot) {
          complete(api_error(404, "UNKNOWN_ACCOUNT", "No paper account " + m.account));
          return true;
        }
        sent[i] = submitted_quantity(*view->snapshot, order.client_order_id);
        if (sent[i]) held += *sent[i];
        unmarked[i] = !sent[i] && !view->snapshot->valuation_complete;
        equity.push_back((unmarked[i] || sent[i].has_value()) ? std::nullopt : std::optional(view->snapshot->equity));
      }
    auto fan = std::make_shared<CopyFanOut>();
    fan->group = *group;
    fan->order = order;
    fan->preview = path.ends_with("/preview");
    fan->quantities = allocate(*group, std::max<Quantity>(order.quantity - held, 0), equity);
    for (std::size_t i = 0; i < sent.size(); ++i)
      if (sent[i]) fan->quantities[i] = *sent[i];
    fan->results.resize(group->members.size());
    fan->complete = std::move(complete);
    std::vector<std::pair<std::size_t, TradingCommand>> commands;
    for (std::size_t i = 0; i < group->members.size(); ++i) {
      const auto& account = group->members[i].account;
      if (unmarked[i]) {
        fan->results[i] = json{{"account", account}, {"quantity", 0}, {"ok", false}, {"skipped", true}, {"code", "STALE_QUOTE"},
                               {"message", "Equity allocation needs this account's positions all marked; they are not, so it was sent nothing"}};
        continue;
      }
      if (fan->quantities[i] <= 0) {
        fan->results[i] = json{{"account", account}, {"quantity", 0}, {"ok", false}, {"skipped", true},
                               {"message", "The allocation gives this account no units"}};
        continue;
      }
      TradingCommand command;
      command.kind = fan->preview ? TradingCommand::Kind::Preview : TradingCommand::Kind::Submit;
      command.account = account == kMainAccount ? std::string() : account;
      command.actor = request.actor;
      command.order = order;
      command.order.quantity = fan->quantities[i];
      commands.emplace_back(i, std::move(command));
    }
    fan->pending = commands.size();
    if (commands.empty()) { fan->finish(); return true; }
    for (auto& [index, command] : commands) {
      const auto i = index;
      const auto record = [fan, i](ApiResponse response) {
        json result{{"account", fan->group.members[i].account}, {"quantity", fan->quantities[i]}, {"status", response.status},
                    {"ok", response.status >= 200 && response.status < 300}};
        try { result["response"] = json::parse(response.body); } catch (const std::exception&) { result["response"] = nullptr; }
        const std::lock_guard lock(fan->mutex);
        fan->results[i] = std::move(result);
        if (--fan->pending == 0) fan->finish();
      };
      if (!source.post_trading(command, [command, record](TradingReply reply) { record(command_response(command, reply)); }))
        record(command_refused(source));
    }
  } catch (const PlanRestrictionError& error) {
    complete(api_error(422, "INVALID_RULES", error.what()));
  } catch (const TradingError& error) {
    complete(api_error(reason_status(error.code()), std::string(to_string(error.code())), error.what()));
  } catch (const std::invalid_argument& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const json::exception&) {
    // A body of the wrong shape is the client's mistake, never a groups outage.
    complete(api_error(400, "INVALID_REQUEST", "Request body does not have the documented fields and types"));
  } catch (const std::exception& error) {
    complete(api_error(503, "GROUPS_UNAVAILABLE", error.what()));
  }
  return true;
}
}  // namespace
}  // namespace paper_internal

void handle_api_async(const ApiRequest& request, MetricsSource& source, ApiCompletion complete) {
  if (request.method == "POST" && request.target.substr(0, request.target.find('?')) == "/api/sandboxes") {
    if (source.sandboxes()) source.sandboxes()->create(request, source, std::move(complete));
    else complete(api_error(404, "NOT_FOUND", "Sandboxes are not offered"));
    return;
  }
  if (auto rejection = sandbox_visibility(request, source)) { complete(std::move(*rejection)); return; }
  if (account_group_api(request, source, complete)) return;
  if (underlying_write(request, source, complete)) return;
  if (request.method == "GET" && strategy_search(request, source, complete)) return;
  if (request.method == "GET") {
    // A Heston or SABR fit takes up to seconds: it runs on the fit worker, never here.
    DeferredRead deferred;
    if (auto response = handle_api_or_defer(request, source, deferred)) complete(std::move(*response));
    else run_deferred(std::move(deferred), std::move(complete));
    return;
  }
  if (request.target.starts_with("/api/notifications/")) {
    // The history is read-only and needs no delivery channel: GET answered it above.
    if (const auto bare = std::string_view(request.target).substr(0, request.target.find('?')); bare == "/api/notifications/history") {
      complete(method_not_allowed(bare, request.method));
      return;
    }
    if (!source.notifications()) { complete(api_error(503, "NOTIFICATIONS_UNAVAILABLE", "Notifications are unavailable on this source")); return; }
    try {
      const auto body = json::parse(request.body);
      if (request.method == "POST" && request.target == "/api/notifications/test") {
        if (!body.is_object() || body.size() != 1 || !body.at("channel").is_string()) throw std::invalid_argument("request");
        const int status = source.notifications()->test(body.at("channel").get<std::string>());
        if (status == 202) complete({202, json{{"queued", true}}.dump()});
        else complete(api_error(status, "NOTIFICATION_TEST_FAILED", "Channel is unknown, disabled, stopped or its queue is full"));
      } else if (request.method == "PUT" && request.target.starts_with("/api/notifications/channels/") && request.target.find('?') == std::string::npos) {
        if (!source.notifications()->configure(request.target.substr(28), body)) complete(api_error(404, "NOT_FOUND", "Unknown notification channel"));
        else complete({200, source.notifications()->status().dump()});
      } else complete(method_not_allowed(std::string_view(request.target).substr(0, request.target.find('?')), request.method));
    } catch (...) { complete(api_error(400, "INVALID_REQUEST", "Invalid notification settings or test request")); }
    return;
  }
  if (const auto bare = request.target.substr(0, request.target.find('?')); request.method == "POST" &&
      (bare == "/api/account/rules-compare" || bare == "/api/rule-packs/validate")) {
    if (bare == "/api/account/rules-compare") complete(rules_compare(request, source));
    else if (request.target != bare) complete(api_error(400, "INVALID_REQUEST", "Unknown query parameter"));
    else complete(validate_rule_pack(request, source));
    return;
  }
  if (playbook_write(request, source, complete)) return;
  // Read-only analysis of an uploaded file: it never reaches an account. The file
  // is parsed here; the recorded days are searched on the calibration worker.
  if (request.method == "POST" && request.target == "/api/fills/calibration") {
    trading::BrokerImport parsed;
    md::Timestamp age = 0;
    try {
      const auto body = json::parse(request.body);
      fields(body, {"csv"}, {"format", "max_quote_age_seconds"});
      const auto format = body.contains("format") ? string_field(body, "format") : std::string("auto");
      const auto seconds = body.contains("max_quote_age_seconds") ? integer_field(body, "max_quote_age_seconds") : 60;
      if (seconds < 1 || seconds > 3600) throw std::invalid_argument("max_quote_age_seconds must be 1 to 3600");
      age = seconds * md::kNanosPerSecond;
      parsed = parse_broker_fills(string_field(body, "csv"), format);
      check_calibration_span(parsed.fills);
    } catch (const std::exception& error) {
      complete(api_error(400, "INVALID_REQUEST", error.what()));
      return;
    }
    static CalibrationWorker worker;
    // The job builds the answer; the worker sends it once it is free for the next one.
    auto job = [parsed = std::move(parsed), age, directory = source.recordings_directory(), complete](const std::atomic_bool& cancel) {
      ApiResponse response;
      try {
        const auto recorded = recorded_quotes(directory, parsed.fills, age, {}, &cancel);
        if (recorded.search.stopped == "cancelled") {
          response = api_error(503, "ENGINE_STOPPING", "The server is stopping");
        } else {
          auto body = calibration_json(calibrate(parsed, recorded.quotes));
          body["search"] = {{"files", recorded.search.files}, {"bytes", recorded.search.bytes}, {"stopped", recorded.search.stopped}};
          response = {200, body.dump()};
        }
      } catch (const std::exception& error) {
        response = api_error(400, "INVALID_REQUEST", error.what());
      }
      return std::function<void()>([complete, response = std::move(response)]() mutable { complete(std::move(response)); });
    };
    if (!worker.start(std::move(job))) {
      auto busy = api_error(503, "CALIBRATION_BUSY", "Another fill calibration is running; retry when it finishes");
      busy.retry_after = kInboxRetrySeconds;
      complete(std::move(busy));
    }
    return;
  }
  // Writes take one query parameter, account=ID; the route is the path alone.
  const auto question = request.target.find('?');
  const std::string path = request.target.substr(0, question);
  const auto pairs = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
  const bool route = (request.method == "POST" && (path == "/api/orders" || path == "/api/orders/preview" || path == "/api/orders/what-if" ||
      (path.starts_with("/api/orders/") && path.ends_with("/preview")) ||
      path == "/api/orders/cancel" || path == "/api/positions/close" || path == "/api/positions/close/preview" || path == "/api/accounts" ||
      path == "/api/positions/exercise" || path == "/api/stocks/close" || path == "/api/stocks/trade" || path == "/api/stocks/trade/preview" ||
      path == "/api/stocks/orders" || path == "/api/stocks/orders/preview" ||
      path == "/api/positions/abandon" || path == "/api/positions/instruction" ||
      path == "/api/risk/kill" || path == "/api/settlements" ||
      path == "/api/account/reset" || path == "/api/account/payout" || path == "/api/account/cash-flows" || path == "/api/market/halt" ||
      path == "/api/trades/group" || path == "/api/trades/ungroup" || path == "/api/alerts")) ||
      (request.method == "PUT" && (path == "/api/risk/limits" || path == "/api/risk/guardrails" || order_id_path(path) ||
                                   ((path.starts_with("/api/trades/") || path.starts_with("/api/days/")) && path.ends_with("/note")))) ||
      (request.method == "DELETE" && (order_id_path(path) || path.starts_with("/api/alerts/") || path.starts_with("/api/stocks/orders/"))) ||
      ((request.method == "PATCH" || request.method == "DELETE") && path.starts_with("/api/accounts/"));
  // A known route asked with a method it does not take is 405 with Allow, as on every route.
  if (!route) { complete(method_not_allowed(path, request.method)); return; }
  std::string account = request.access.sandbox;
  if (!pairs || pairs->size() > 1 || (pairs->size() == 1 && (!pairs->contains("account") || !valid_account(pairs->at("account"))) ) ||
      (pairs->size() == 1 && (path == "/api/accounts" || path.starts_with("/api/accounts/")))) {
    complete(api_error(400, "INVALID_REQUEST", "Unknown or invalid query parameter"));
    return;
  }
  if (pairs->size() == 1) account = pairs->at("account");
  // An account whose journal another process holds cannot take writes. A route that
  // manages an account (rename, archive, delete) addresses the one in its path.
  constexpr std::string_view managed = "/api/accounts/";
  const std::string addressed = path.starts_with(managed) ? path.substr(managed.size())
                                : account.empty() ? std::string(kMainAccount) : account;
  const auto status = source.status();
  for (const auto& a : status.accounts)
    if (a.id == addressed && a.trading.reason.starts_with("JOURNAL_LOCKED:")) {
      complete(api_error(503, "TRADING_UNAVAILABLE", a.trading.reason));
      return;
    }
  if (status.accounts.empty() && status.trading.reason.starts_with("JOURNAL_LOCKED:")) {
    complete(api_error(503, "TRADING_UNAVAILABLE", status.trading.reason));
    return;
  }
  try {
    auto command = parse_command(request, path);
    if (auto taken = name_taken(status, command)) {
      complete(std::move(*taken));
      return;
    }
    if (auto uncarried = uncarried_shares(source, command)) {
      complete(std::move(*uncarried));
      return;
    }
    if (!path.starts_with(managed)) command.account = account;
    command.actor = request.actor;
    if (!source.post_trading(command, [command, complete](TradingReply reply) {
          complete(command_response(command, reply));
        })) complete(command_refused(source));
  } catch (const PlanRestrictionError& error) {
    complete(api_error(422, "INVALID_RULES", error.what()));
  } catch (const TradingError& error) {
    complete(api_error(reason_status(error.code()), std::string(to_string(error.code())), error.what()));
  } catch (const std::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  }
}
}  // namespace openport::server
