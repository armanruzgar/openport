#include "openport/server/desk.hpp"
#include "openport/server/playbooks.hpp"
#include "openport/server/series.hpp"
#include "openport/analytics/volatility_history.hpp"
#include "metric_cache.hpp"

#include <algorithm>
#include <cmath>

namespace openport::server {
namespace {
using nlohmann::json;
struct Running {
  bool& flag;
  explicit Running(bool& value) : flag(value) { flag = true; }
  ~Running() { flag = false; }
};
}
void Desk::evaluate_playbooks(md::Timestamp driver_time, std::string_view only_account, std::string_view only_playbook) {
  if (!playbooks_ || playbook_running_) return;
  Running running(playbook_running_);
  bool evaluated = false;
  const auto cancel_reason = inputs_first() && options_.playbook_cancel_labels
      ? trading::Reason::PLAYBOOK_TIME_STOP : trading::Reason::USER_CANCEL;
  for (auto& account : accounts_) {
    if (!only_account.empty() && account.id != only_account) continue;
    if (account.archived || !account.session || !account.failure.empty() || !playbooks_->enabled(account.id)) continue;
    evaluated = true;
    const auto view = trading_view(account.id);
    const auto inputs = [&](const std::string& symbol, const json& conditions) {
      PlaybookInputs result;
      const auto snapshot = metrics(symbol);
      if (!snapshot) return result;
      result.spot = snapshot->spot;
      // Only the inputs a condition names are read: the candle scans are not free.
      const auto reference = conditions.contains("price") ? conditions.at("price").at("reference").get<std::string>() : std::string();
      const auto today = md::new_york_time(snapshot->as_of).date;
      const auto previous = md::previous_business_day(today);
      if (reference == "prior_close" || conditions.contains("gap")) {
        const auto official = official_closes_.find({symbol, previous});
        if (official != official_closes_.end()) result.prior_close = official->second.price;
      }
      std::vector<md::Bar> days;
      if (options_.candles && (reference == "prior_close" || conditions.contains("vrp_min") || conditions.contains("gap") || conditions.contains("technical")))
        days = options_.candles->daily_history(symbol);
      for (const auto& day : days) if (md::new_york_time(day.start).date == previous && !std::isfinite(result.prior_close)) result.prior_close = day.close;
      if (options_.candles && (reference == "day_open" || conditions.contains("gap"))) {
        // Require the opening minute itself; a partial day's first print is not its open.
        for (const auto& bar : options_.candles->bars(symbol, BarInterval::Minute, 10000))
          if (bar.start == md::new_york_to_utc(today, 9, 30)) result.day_open = bar.open;
      }
      if (conditions.contains("vix")) {
        const auto vix = book_.underlyings().find("VIX");
        // An option-implied forward is not the VIX index level.
        if (vix != book_.underlyings().end() && vix->second.spot_ts > 0 && vix->second.spot_ts <= snapshot->as_of &&
            snapshot->as_of - vix->second.spot_ts <= 5 * md::kNanosPerMinute && vix->second.spot > 0)
          result.vix = vix->second.spot;
      }
      if (options_.candles && conditions.contains("technical")) {
        for (const auto& bar : days)
          if (md::new_york_time(bar.start).date < today) result.daily_closes.push_back(bar.close);
        for (const auto& bar : options_.candles->bars(symbol, BarInterval::Minute, 10000))
          if (bar.start + md::kNanosPerMinute <= snapshot->as_of) result.minute_closes.push_back(bar.close);
        for (auto* closes : {&result.daily_closes, &result.minute_closes})
          if (closes->size() > 1000) closes->erase(closes->begin(), closes->end() - 1000);
      }
      if (conditions.contains("iv_rank") || conditions.contains("vrp_min") || conditions.contains("term_inverted")) {
        const auto volatility = cached_volatility(snapshot);
        result.term_ratio = volatility.ratio9_30;
        if (conditions.contains("vrp_min")) {
          const auto realized = analytics::realized_metrics(days, {}, snapshot->as_of);
          result.vrp = analytics::variance_risk_premium(volatility.mfiv[1], realized.windows[2].close_to_close.vol).spread;
        }
        if (conditions.contains("iv_rank")) {
          std::vector<analytics::VolatilityDay> own;
          if (options_.series) for (const auto& row : options_.series->rows(symbol, 0, snapshot->as_of, true))
            own.push_back({md::new_york_time(row.time).date, row.values[3], row.values[8]});
          const auto proxy = analytics::iv_proxy(symbol);
          const auto bars = options_.candles ? options_.candles->daily_history(proxy) : std::vector<md::Bar>{};
          const auto history = analytics::historical_iv(own, bars, proxy, snapshot->as_of);
          const double current = std::isfinite(volatility.mfiv[1].vol) ? volatility.mfiv[1].vol : volatility.atm[1].vol;
          result.iv_rank = analytics::iv_rank(history, current, snapshot->as_of).rank;
        }
      }
      return result;
    };
    const auto project = [&](const trading::OrderRequest& order, double share) {
      TradingReply reply;
      TradingCommand command;
      command.kind = TradingCommand::Kind::Preview;
      command.account = account.id;
      command.order = order;
      command.floor_share = share;
      PendingCommand pending{0, command, [&](TradingReply result) { reply = std::move(result); }};
      apply_command(pending, market_time_, driver_time);
      return reply;
    };
    const auto preview = [&](const trading::OrderRequest& order, double share) {
      auto reply = project(order, share);
      if (!reply.preview) throw std::invalid_argument(reply.decision.message.empty() ? "Preview unavailable" : reply.decision.message);
      return *reply.preview;
    };
    const auto send = [&](const trading::OrderRequest& order) {
      TradingReply reply;
      TradingCommand command;
      command.kind = TradingCommand::Kind::Submit;
      command.account = account.id;
      command.order = order;
      // Automatic submissions are the system's, and carry the playbook's tag.
      // Order::system is reserved for reducer liquidation and must not bypass risk.
      command.actor = "system";
      const bool time_stop = order.note.starts_with("Playbook automatic ");
      const auto close_reason = order.note.starts_with("Playbook automatic trailing stop") ? trading::Reason::PLAYBOOK_TRAILING_STOP
          : order.note.starts_with("Playbook automatic DTE stop") ? trading::Reason::PLAYBOOK_DTE_STOP
          : order.note.starts_with("Playbook automatic days in trade stop") ? trading::Reason::PLAYBOOK_DAYS_IN_TRADE_STOP : cancel_reason;
      if (time_stop && !options_.replay) {
        // Check feed/session freshness before cancelling working protection. A mode
        // command can evaluate while the feed is stopped, without a new snapshot.
        for (const auto& symbol : trading::chain_symbols(order)) {
          const auto contract = view->contracts.find(symbol);
          if (contract == view->contracts.end()) continue;
          const auto& underlying = contract->second.underlying;
          const auto time = view->market_times.find(underlying);
          reply.decision = paper_acceptance(underlying, time == view->market_times.end() ? 0 : time->second,
              driver_time, capabilities_.delay, account.session->config().limits.max_quote_age, breaker_.halts);
          if (!reply.decision.ok()) return reply;
        }
      }
      if (time_stop && close_reason != cancel_reason) {
        // An account that cannot preview (failed, or stopping) waits like an unpriced close.
        const auto check = project(order, 1.0);
        if (!check.preview || !check.preview->decision.ok()) {
          reply.decision = check.preview ? check.preview->decision : check.decision;
          reply.error_code = check.error_code;
          return reply;
        }
      }
      if (time_stop) {
        if (!options_.replay) account.session->set_actor("system");
        const auto symbols = trading::order_symbols(order);
        const auto working = account.session->snapshot()->open_orders;
        for (const auto& candidate : working) {
          const auto contracts = trading::order_symbols(candidate.request);
          if (std::any_of(contracts.begin(), contracts.end(), [&](const auto& symbol) { return std::find(symbols.begin(), symbols.end(), symbol) != symbols.end(); }))
            account.session->cancel(candidate.id, market_time_, close_reason);
        }
      } else {
        const auto symbols = trading::order_symbols(order);
        for (const auto& position : account.session->snapshot()->positions)
          if (position.position.quantity && std::find(symbols.begin(), symbols.end(), position.position.contract.osi_symbol()) != symbols.end()) {
            reply.error_code = "PLAYBOOK_OVERLAP";
            reply.decision.message = "Another position now holds a selected contract";
            return reply;
          }
      }
      std::deque<PendingCommand> pending{{0, command, [&](TradingReply result) { reply = std::move(result); }}};
      update_trading({}, pending, driver_time);
      if (time_stop) {
        // A close the market cannot take yet (a wing nobody bids for, stale quotes, a
        // halt) records nothing, and the next update tries again, as the reducer's own
        // closes do; submitting it would journal a rejected order at every update.
        TradingCommand check = command;
        check.kind = TradingCommand::Kind::Preview;
        TradingReply projection;
        PendingCommand checking{0, check, [&](TradingReply result) { projection = std::move(result); }};
        apply_command(checking, market_time_, driver_time);
        if (!projection.preview || !projection.preview->decision.ok()) {
          reply.decision = projection.preview ? projection.preview->decision : projection.decision;
          reply.error_code = projection.error_code.empty() ? "PLAYBOOK_TIME_STOP_WAITING" : projection.error_code;
          return reply;
        }
      }
      apply_command(pending.front(), market_time_, driver_time);
      return reply;
    };
    // A failure here (a journal write behind an automatic cancel or close) fails this
    // account only, as in update_trading; it must not reach the engine thread.
    try {
      playbooks_->evaluate(account.id, options_.replay, market_time_, metrics_, *view, inputs, preview, send,
          [&](trading::OrderId id, trading::Reason reason) {
            if (!options_.replay) account.session->set_actor("system");
            account.session->cancel(id, market_time_, reason == trading::Reason::PLAYBOOK_TIME_STOP ? cancel_reason : reason);
          }, only_playbook);
    } catch (const trading::TradingError& error) {
      fail_trading(account, std::string(to_string(error.code())) + ": " + error.what());
    } catch (const std::exception& error) {
      fail_trading(account, std::string("TRADING_UNAVAILABLE: ") + error.what());
    }
  }
  if (evaluated) publish_trading();
}
void Desk::playbook_command(const TradingCommand& command, TradingReply& reply, md::Timestamp driver_time) {
  if (!playbooks_) throw trading::TradingError(trading::Reason::INVALID_ORDER, "Playbooks unavailable");
  try {
    const auto change = json::parse(command.note);
    const auto account = command.account.empty() ? std::string(kMainAccount) : command.account;
    if (change.at("action") == "signal") {
      const auto id = change.at("id").get<std::string>();
      const auto publication = playbooks_->publication(account, options_.replay);
      if (!publication.at("definitions").contains(id) || publication.at("definitions").at(id).at("deleted") == true)
        throw std::invalid_argument("Unknown playbook");
      if (publication.at("modes").value(id, "off") == "off") throw std::invalid_argument("Playbook is off");
      evaluate_playbooks(driver_time, account, id);
      reply.playbook_result = playbooks_->publication(account, options_.replay).dump();
      return;
    }
    if (change.at("action") == "send") {
      evaluate_playbooks(driver_time);
      const auto order = playbooks_->take(account, change.at("staged").get<std::string>());
      Running running(playbook_running_);
      TradingCommand submit;
      submit.account = account;
      submit.actor = command.actor;
      submit.order = order;
      std::deque<PendingCommand> pending{{0, submit, [&](TradingReply result) { reply = std::move(result); }}};
      update_trading({}, pending, driver_time);
      apply_command(pending.front(), market_time_, driver_time);
      if (!reply.decision.ok() || !reply.error_code.empty()) throw std::invalid_argument(reply.decision.message);
    } else {
      std::map<std::string, trading::OrderId> next_orders;
      for (const auto& owner : accounts_) if (owner.session)
        next_orders[owner.id] = owner.session->snapshot()->recent_orders.size() + 1;
      playbooks_->change(change, account, options_.replay, market_time_, command.actor, next_orders);
    }
    evaluate_playbooks(driver_time);
    reply.playbook_result = playbooks_->publication(account, options_.replay).dump();
  } catch (const std::exception& error) {
    throw trading::TradingError(trading::Reason::INVALID_ORDER, error.what());
  }
}
}  // namespace openport::server
