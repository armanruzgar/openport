#include "openport/trading/evaluation.hpp"
#include "openport/trading/events.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

#include "openport/md/time.hpp"

namespace openport::trading {
AccountRules original_program_rules(AccountRules rules, Money current_size,
    const std::optional<SizeScalingProgress>& scaling) {
  if (scaling) {
    rules.max_drawdown = scaling->original_max_drawdown;
    rules.daily_loss_limit = scaling->original_daily_loss_limit;
    if (rules.lock_balance > Money{}) rules.lock_balance = rules.lock_balance - (current_size - scaling->original);
  }
  return rules;
}

std::int64_t scaling_limit(const AccountRules& rules, Money profit) {
  if (rules.scaling.empty()) return 0;
  auto limit = rules.scaling.front().contracts;
  for (const auto& step : rules.scaling) {
    if (step.profit > profit) break;
    limit = step.contracts;
  }
  return limit;
}

Money size_scaling_profit(const Evaluation& e, Money balance) {
  const auto& p = e.size_scaling.value();
  auto profit = balance - p.period_balance;
  for (std::size_t i = p.payouts_at_start; i < e.payouts.size(); ++i) profit = profit + e.payouts[i].amount;
  return profit;
}
Money size_scaling_required(const Evaluation& e, const SizeScaling& rule) {
  __extension__ using Wide = __int128;
  // Round up to the smallest micro-dollar that meets the exact percentage.
  return Money::from_micros(static_cast<std::int64_t>(
      (static_cast<Wide>(e.size_scaling->period_size.micros()) * rule.profit_percent + 99) / 100));
}
Money size_scaling_increase(const Evaluation& e, const SizeScaling& rule) {
  __extension__ using Wide = __int128;
  const auto step = Money::from_micros(static_cast<std::int64_t>(
      static_cast<Wide>(e.size_scaling->original.micros()) * rule.increase_percent / 100));
  return std::min(step, rule.max_balance - e.starting_balance);
}

namespace {
__extension__ using Wide = __int128;
std::string dollars(Money value) { return (value < Money{} ? "-$" + (-value).str() : "$" + value.str()); }
std::string days_text(std::uint64_t n) { return std::to_string(n) + (n == 1 ? " day" : " days"); }
/// A peak that follows every high (intraday and static plans) moves with fully
/// marked equity; an end-of-day one only at rollover.
void ratchet(Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (rules.drawdown_mode != DrawdownMode::EndOfDay && equity > evaluation.peak) {
    evaluation.peak = equity;
    const auto floor = evaluation_floor(rules, evaluation.peak, evaluation.floor_locked, evaluation.starting_balance);
    evaluation.floor = evaluation.size_scaling ? std::max(evaluation.floor, floor) : floor;
  }
}
/// Adds one day to the counts the objectives read.
void add_day(DayStats& stats, const AccountRules& rules, md::Date day, Money profit, std::uint64_t executions) {
  ++stats.days;
  if (executions > 0) ++stats.trading_days;
  if (profit <= Money{}) return;
  if (profit >= rules.profitable_day_profit) ++stats.profitable_days;
  stats.positive_total = stats.positive_total + profit;
  if (!stats.best_day || profit > *stats.best_day) {
    stats.best_day = profit;
    stats.best_day_date = day;
  }
}
DayStats finished_day_stats(const Evaluation& evaluation, const AccountRules& rules) {
  DayStats stats;
  for (const auto& day : evaluation.days) add_day(stats, rules, day.day, day_profit(day, rules), day.executions);
  return stats;
}
/// day_stats from the finished days' counts and the day in progress.
DayStats with_today(DayStats stats, const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  if (evaluation.started > 0) add_day(stats, rules, evaluation.day, today_profit(evaluation, rules, now), evaluation.day_executions);
  return stats;
}
/// The consistency rule's basis: the attempt's profit, or its profitable days' sum.
Money consistency_basis(const AccountRules& rules, const DayStats& stats, Money profit) {
  return rules.consistency_basis == ConsistencyBasis::PositiveDays ? stats.positive_total : profit;
}
/// best * 100 <= percent * basis, exactly.
bool consistent(Money best, std::int64_t percent, Money basis) {
  return static_cast<Wide>(best.micros()) * 100 <= static_cast<Wide>(basis.micros()) * percent;
}
std::string basis_name(DailyLossBasis basis) {
  switch (basis) {
    case DailyLossBasis::Equity: return "the day's opening equity";
    case DailyLossBasis::Balance: return "the day's opening balance";
    case DailyLossBasis::Higher: return "the higher of the day's opening balance and equity";
    case DailyLossBasis::Peak: return "the day's equity high";
  }
  return "the day's opening equity";
}
bool needs_days(const AccountRules& rules) {
  return rules.min_trading_days > 0 || rules.min_profitable_days > 0 || rules.consistency_percent > 0;
}
std::vector<Objective> objectives_with(const Evaluation& e, const AccountRules& rules, const PlanInputs& now, const DayStats* finished) {
  std::vector<Objective> out;
  const auto profit = attempt_profit(e, rules, now);
  const bool balance = rules.profit_basis == ProfitBasis::Balance;
  if (rules.profit_target > Money{}) {
    Objective o{Reason::PROFIT_TARGET, profit >= rules.profit_target && (!balance || now.flat),
                profit.dollars(), rules.profit_target.dollars(), {}};
    const auto target = e.starting_balance + rules.profit_target;
    const auto measured = balance ? "The closed balance " + dollars(now.balance) : "Equity " + dollars(now.equity);
    if (o.met) o.message = measured + " is at or above the target " + dollars(target);
    else if (profit >= rules.profit_target) o.message = measured + " reached the target " + dollars(target) + "; close every position to count it";
    else o.message = dollars(rules.profit_target - profit) + " to go to the target " + dollars(target) +
                     (balance ? " on the closed balance" : "");
    out.push_back(std::move(o));
  }
  if (rules.phase != Phase::Funded && rules.microscalp_percent > 0) {
    Objective o{Reason::MICROSCALPING, e.short_profit == Money{} || consistent(e.short_profit, rules.microscalp_percent, profit),
                std::nullopt, static_cast<double>(rules.microscalp_percent),
                "Net positive profit " + dollars(e.short_profit) + " from round trips held under " +
                    std::to_string(rules.microscalp_seconds) + " seconds; at most " +
                    std::to_string(rules.microscalp_percent) + "% of attempt profit " + dollars(profit)};
    if (profit > Money{}) o.actual = e.short_profit.dollars() / profit.dollars() * 100;
    out.push_back(std::move(o));
  }
  if (rules.phase != Phase::Funded && rules.min_trades > 0) {
    out.push_back({Reason::MIN_TRADES, e.closed_trades >= static_cast<std::uint64_t>(rules.min_trades),
                   static_cast<double>(e.closed_trades), static_cast<double>(rules.min_trades),
                   std::to_string(e.closed_trades) + " of " + std::to_string(rules.min_trades) + " closed whole trades"});
  }
  if (rules.phase != Phase::Funded && rules.trade_consistency_percent > 0) {
    Objective o{Reason::TRADE_CONSISTENCY, true, std::nullopt, static_cast<double>(rules.trade_consistency_percent),
                "No profitable closed whole trade yet"};
    if (profit > Money{}) o.actual = e.best_trade ? e.best_trade->pnl.dollars() / profit.dollars() * 100 : 0;
    if (e.best_trade) {
      o.met = consistent(e.best_trade->pnl, rules.trade_consistency_percent, profit);
      o.message = "Best trade " + e.best_trade->id + ", net " + dollars(e.best_trade->pnl) +
          "; at most " + std::to_string(rules.trade_consistency_percent) + "% of attempt profit " + dollars(profit);
    }
    out.push_back(std::move(o));
  }
  if (rules.phase != Phase::Funded && rules.best_position_percent > 0 && rules.profit_target > Money{}) {
    const auto cap = best_position_cap(rules);
    const auto counted = profit - e.position_excess;
    Objective o{Reason::BEST_POSITION, e.position_excess == Money{} || counted >= rules.profit_target, std::nullopt,
                static_cast<double>(rules.best_position_percent), "No profitable closed whole trade yet"};
    if (e.best_position) {
      o.actual = e.best_position->pnl.dollars() / rules.profit_target.dollars() * 100;
      o.message = "Best trade " + e.best_position->id + ", net " + dollars(e.best_position->pnl) + "; at most " + dollars(cap) +
          " (" + std::to_string(rules.best_position_percent) + "% of the target) of one trade counts" +
          (e.position_excess > Money{} ? ": " + dollars(e.position_excess) + " above it is left out, " + dollars(counted) +
                                         " of the " + dollars(rules.profit_target) + " target counted"
                                       : "");
    }
    out.push_back(std::move(o));
  }
  if (!needs_days(rules)) return out;
  const auto stats = with_today(finished ? *finished : finished_day_stats(e, rules), e, rules, now);
  if (rules.min_trading_days > 0) {
    const auto required = static_cast<std::uint64_t>(rules.min_trading_days);
    out.push_back({Reason::MIN_TRADING_DAYS, stats.trading_days >= required, static_cast<double>(stats.trading_days),
                   static_cast<double>(required), std::to_string(stats.trading_days) + " of " + days_text(required) +
                   " with a trade; a day counts once one of your own orders, or a share trade, executes on it"});
  }
  if (rules.min_profitable_days > 0) {
    const auto required = static_cast<std::uint64_t>(rules.min_profitable_days);
    out.push_back({Reason::MIN_PROFITABLE_DAYS, stats.profitable_days >= required, static_cast<double>(stats.profitable_days),
                   static_cast<double>(required), std::to_string(stats.profitable_days) + " of " + days_text(required) +
                   " with a profit" + (rules.profitable_day_profit > Money{} ? " of at least " + dollars(rules.profitable_day_profit) : "")});
  }
  if (rules.consistency_percent > 0) {
    const auto basis = consistency_basis(rules, stats, profit);
    Objective o{Reason::CONSISTENCY, true, std::nullopt, static_cast<double>(rules.consistency_percent), {}};
    const std::string of = rules.consistency_basis == ConsistencyBasis::PositiveDays ? "the profitable days' total " : "the total profit ";
    if (!stats.best_day) {
      o.message = "No profitable day yet";
    } else {
      o.met = consistent(*stats.best_day, rules.consistency_percent, basis);
      if (basis > Money{}) o.actual = stats.best_day->dollars() / basis.dollars() * 100;
      o.message = "The best day, " + dollars(*stats.best_day) + " on " + md::format_date(stats.best_day_date) + ", " +
          (basis > Money{} ? "is " + std::to_string(static_cast<int>(*o.actual + 0.5)) + "% of " + of + dollars(basis)
                           : "exceeds " + of + dollars(basis)) +
          "; at most " + std::to_string(rules.consistency_percent) + "% may come from one day";
    }
    out.push_back(std::move(o));
  }
  return out;
}
}  // namespace
Money lock_level(const AccountRules& rules, Money starting_balance) {
  return rules.lock_at_start ? starting_balance : rules.lock_balance;
}
Money evaluation_floor(const AccountRules& rules, Money peak, bool& locked, Money starting_balance) {
  if (rules.max_drawdown <= Money{}) return {};
  if (rules.drawdown_mode == DrawdownMode::Static) {
    locked = true;
    return starting_balance - rules.max_drawdown;
  }
  const auto lock = lock_level(rules, starting_balance);
  if (lock > Money{} && (locked || peak - rules.max_drawdown >= lock)) {
    locked = true;
    return lock;
  }
  return peak - rules.max_drawdown;
}
EvaluationStatus evaluate_equity(Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (evaluation.status != EvaluationStatus::Active) return evaluation.status;
  // An account without rules keeps its high-water mark too; only rules decide.
  ratchet(evaluation, rules, equity);
  if (!rules.evaluation()) return EvaluationStatus::Active;
  if (rules.max_drawdown > Money{} && equity <= evaluation.floor) return EvaluationStatus::Failed;
  if (rules.profit_target > Money{} && equity >= evaluation.starting_balance + rules.profit_target)
    return EvaluationStatus::Passed;
  return EvaluationStatus::Active;
}
void evaluation_rollover(Evaluation& evaluation, const AccountRules& rules) {
  const auto close = rules.drawdown_basis == DrawdownBasis::Balance
      ? evaluation.day_close_balance.value_or(evaluation.day_close_equity) : evaluation.day_close_equity;
  if (evaluation.status == EvaluationStatus::Active && rules.drawdown_mode == DrawdownMode::EndOfDay &&
      close > evaluation.peak) {
    evaluation.peak = close;
    const auto floor = evaluation_floor(rules, evaluation.peak, evaluation.floor_locked, evaluation.starting_balance);
    evaluation.floor = evaluation.size_scaling ? std::max(evaluation.floor, floor) : floor;
  }
}
Money evaluation_tomorrow_floor(const Evaluation& evaluation, const AccountRules& rules, Money equity) {
  if (evaluation.status != EvaluationStatus::Active || rules.drawdown_mode != DrawdownMode::EndOfDay ||
      equity <= evaluation.peak)
    return evaluation.floor;
  bool locked = evaluation.floor_locked;
  const auto floor = evaluation_floor(rules, equity, locked, evaluation.starting_balance);
  return evaluation.size_scaling ? std::max(evaluation.floor, floor) : floor;
}
md::Date plan_trading_date(const AccountRules& rules, Timestamp time) {
  return md::trading_date(time, static_cast<int>(rules.day_end_minutes));
}
bool counts_executions(const AccountRules& rules) { return rules.min_trading_days > 0; }
Money day_profit(const EvaluationDay& day, const AccountRules& rules) {
  return rules.profit_basis == ProfitBasis::Balance ? day.realised : day.close_equity - day.open_equity;
}
Money today_profit(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return rules.profit_basis == ProfitBasis::Balance ? now.net_realised - evaluation.day_open_realised
                                                    : now.equity - evaluation.day_open_equity;
}
Money attempt_profit(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  Money withdrawn;
  for (const auto& payout : evaluation.payouts) withdrawn = withdrawn + payout.amount;
  return (rules.profit_basis == ProfitBasis::Balance ? now.balance : now.equity) + withdrawn - evaluation.starting_balance;
}
Money day_open_balance(const Evaluation& evaluation, const PlanInputs& now) {
  return now.balance - (now.net_realised - evaluation.day_open_realised);
}
DayStats day_stats(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return with_today(finished_day_stats(evaluation, rules), evaluation, rules, now);
}
std::vector<Objective> evaluation_objectives(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return objectives_with(evaluation, rules, now, nullptr);
}
std::optional<Money> consistency_target(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  if (rules.consistency_percent <= 0) return std::nullopt;
  const auto stats = day_stats(evaluation, rules, now);
  if (!stats.best_day) return std::nullopt;
  // Whole cents, rounded up, so the amount always satisfies the rule.
  const Wide scaled = static_cast<Wide>(stats.best_day->micros()) * 100;
  const Wide micros = (scaled + rules.consistency_percent - 1) / rules.consistency_percent;
  const Wide cents = (micros + 9'999) / 10'000;
  return Money::from_micros(static_cast<std::int64_t>(cents * 10'000));
}
std::optional<DailyLossLevel> daily_loss_level(const Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  if (rules.daily_loss_limit <= Money{}) return std::nullopt;
  Money reference = evaluation.day_open_equity;
  switch (rules.daily_loss_basis) {
    case DailyLossBasis::Equity: break;
    case DailyLossBasis::Balance: reference = day_open_balance(evaluation, now); break;
    case DailyLossBasis::Higher: reference = std::max(reference, day_open_balance(evaluation, now)); break;
    case DailyLossBasis::Peak: reference = std::max(reference, evaluation.day_high_equity.value_or(reference)); break;
  }
  return DailyLossLevel{reference, reference - rules.daily_loss_limit};
}
namespace {
PlanVerdict evaluate_with(Evaluation& e, const AccountRules& rules, const PlanInputs& now, const DayStats* finished) {
  PlanVerdict verdict;
  if (e.status != EvaluationStatus::Active) return verdict;
  if (const auto timed = evaluate_time_rules(e, rules, now.time); timed.decided()) return timed;
  // A balance basis follows and checks the closed balance: open P&L never touches the floor.
  const bool balance_floor = rules.drawdown_basis == DrawdownBasis::Balance;
  const auto measured = balance_floor ? now.balance : now.equity;
  ratchet(e, rules, measured);
  if (!rules.evaluation()) return verdict;
  if (rules.max_drawdown > Money{} && measured <= e.floor) {
    verdict.status = EvaluationStatus::Failed;
    verdict.code = Reason::DRAWDOWN_FLOOR;
    verdict.level = e.floor;
    verdict.message = (balance_floor ? "The closed balance " : "Equity ") + dollars(measured) + " reached the drawdown floor " +
                      dollars(e.floor) + " (peak " + dollars(e.peak) + ", max drawdown " + dollars(rules.max_drawdown) + ")";
    return verdict;
  }
  if (e.day_lock == Reason::NONE)
    if (const auto daily = daily_loss_level(e, rules, now); daily && now.equity <= daily->level) {
      verdict.code = Reason::DAILY_LOSS_LIMIT;
      verdict.level = daily->level;
      verdict.message = "Equity " + dollars(now.equity) + " reached the daily loss limit at " + dollars(daily->level) + " (" +
                        dollars(rules.daily_loss_limit) + " below " + basis_name(rules.daily_loss_basis) + ", " +
                        dollars(daily->reference) + ")";
      if (rules.daily_loss_action == BreachAction::Fail) {
        verdict.status = EvaluationStatus::Failed;
      } else {
        verdict.lock = true;
        verdict.message += ": positions are closed and opening orders refused until the next trading day";
      }
      return verdict;
    }
  if (rules.profit_target <= Money{}) return verdict;
  const auto objectives = objectives_with(e, rules, now, finished);
  if (!std::all_of(objectives.begin(), objectives.end(), [](const Objective& o) { return o.met; })) return verdict;
  const auto target = e.starting_balance + rules.profit_target;
  verdict.status = EvaluationStatus::Passed;
  verdict.code = Reason::PROFIT_TARGET;
  verdict.level = target;
  verdict.message = rules.profit_basis == ProfitBasis::Balance
      ? "Balance " + dollars(now.balance) + " reached the profit target " + dollars(target) + " with every position closed"
      : "Equity " + dollars(now.equity) + " reached the profit target " + dollars(target);
  return verdict;
}
}  // namespace
PlanVerdict evaluate_plan(Evaluation& evaluation, const AccountRules& rules, const PlanInputs& now) {
  return evaluate_with(evaluation, rules, now, nullptr);
}
TimeRuleProgress time_rule_progress(const Evaluation& e, const AccountRules& rules, Timestamp time) {
  TimeRuleProgress progress;
  if (e.started <= 0 || time <= 0) return progress;
  const auto day = md::days_since_epoch(plan_trading_date(rules, time));
  if (rules.time_limit_days > 0) {
    const auto deadline = md::days_since_epoch(plan_trading_date(rules, e.started)) + rules.time_limit_days;
    progress.deadline = md::date_from_days(deadline);
    progress.days_left = std::max<std::int64_t>(0, deadline - day);
  }
  if (rules.inactivity_days > 0) {
    progress.last_activity = e.last_activity > 0 ? e.last_activity : e.started;
    const auto activity = md::days_since_epoch(plan_trading_date(rules, progress.last_activity));
    progress.inactive_days = std::max<std::int64_t>(0, day - activity);
    progress.inactivity_deadline = md::date_from_days(activity + rules.inactivity_days);
  }
  return progress;
}
PlanVerdict evaluate_time_rules(const Evaluation& e, const AccountRules& rules, Timestamp time) {
  if (e.status != EvaluationStatus::Active || (rules.time_limit_days == 0 && rules.inactivity_days == 0)) return {};
  const auto progress = time_rule_progress(e, rules, time);
  if (progress.deadline && plan_trading_date(rules, time) > *progress.deadline)
    return {EvaluationStatus::Failed, false, Reason::TIME_LIMIT, {}, "The evaluation's " +
        std::to_string(rules.time_limit_days) + "-day window ended on " + md::format_date(*progress.deadline)};
  if (progress.inactive_days && *progress.inactive_days > rules.inactivity_days)
    return {EvaluationStatus::Failed, false, Reason::INACTIVITY, {}, "No own execution for " +
        std::to_string(*progress.inactive_days) + " calendar days; the " + std::to_string(rules.inactivity_days) +
        "-day inactivity window ended on " + md::format_date(*progress.inactivity_deadline)};
  return {};
}
bool plan_flat_now(const AccountRules& rules, Timestamp time) {
  if (!rules.flat_time || time <= 0) return false;
  const auto ny = md::new_york_time(time);
  const auto minute = ny.seconds / 60;
  return minute >= *rules.flat_time && minute < rules.day_end_minutes && ny.date == plan_trading_date(rules, time);
}
Decision plan_entry_check(const AccountRules& rules, std::string_view underlying, Timestamp time) {
  if (plan_flat_now(rules, time))
    return {Reason::FLAT_TIME, "Flat time passed; openings blocked until the day ends. Closing orders still work",
        static_cast<double>(md::new_york_time(time).seconds / 60), static_cast<double>(*rules.flat_time), "account"};
  if (const auto d = event_entry_check(rules, underlying, time); !d.ok()) return d;
  const std::string scope(underlying);
  if (!rules.underlyings.empty() && std::find(rules.underlyings.begin(), rules.underlyings.end(), underlying) == rules.underlyings.end())
    return {Reason::INSTRUMENT_NOT_ALLOWED, scope + " is not an allowed underlying for this plan", {}, {}, scope};
  if (rules.trading_start && time > 0) {
    const auto minute = md::new_york_time(time).seconds / 60;
    if (minute < *rules.trading_start || minute >= *rules.trading_end) {
      const auto clock = [](std::int64_t m) {
        const auto two = [](std::int64_t v) { return (v < 10 ? "0" : "") + std::to_string(v); };
        return two(m / 60) + ":" + two(m % 60);
      };
      return {Reason::OUTSIDE_PLAN_HOURS, "Opening orders are allowed from " + clock(*rules.trading_start) + " to " +
          clock(*rules.trading_end) + " New York time", static_cast<double>(minute),
          static_cast<double>(minute < *rules.trading_start ? *rules.trading_start : *rules.trading_end), scope};
    }
  }
  return {};
}
bool in_payout_cycle(const Evaluation& e, Timestamp closed) {
  return closed >= e.cycle_started && (e.payouts.empty() || closed > e.payouts.back().time);
}
std::optional<Money> open_loss_limit(const Evaluation& e, const AccountRules& rules, const PlanInputs& now) {
  if (!rules.open_loss_cap) return std::nullopt;
  const auto& cap = *rules.open_loss_cap;
  if (cap.basis == OpenLossBasis::Amount) return cap.amount;
  const auto share = [&](Money base) {
    if (base <= Money{}) return Money{};
    // Whole cents, rounded down, of the percentage.
    const Wide cents = static_cast<Wide>(base.micros()) * cap.percent / 100 / 10'000;
    return Money::from_micros(static_cast<std::int64_t>(cents * 10'000));
  };
  const auto room = [&]() -> std::optional<Money> {
    if (rules.max_drawdown <= Money{}) return std::nullopt;
    return share(e.day_open_equity - e.floor);
  };
  if (cap.basis == OpenLossBasis::DrawdownRoom) return room();
  // The start-of-day profit balance; without profit yet, the drawdown room stands in.
  const auto profit = day_open_balance(e, now) - e.starting_balance;
  if (profit > Money{}) return share(profit);
  return room();
}
Money best_position_cap(const AccountRules& rules) {
  return Money::from_micros(static_cast<std::int64_t>(
      static_cast<Wide>(rules.profit_target.micros()) * rules.best_position_percent / 100));
}
std::string day_lock_message(Reason lock) {
  if (lock == Reason::DAILY_LOSS_LIMIT)
    return "The plan's daily loss limit locked the account until the next trading day; only closing orders are accepted";
  return "The plan locked the account until the next trading day; only closing orders are accepted";
}
PassOdds pass_odds(const Evaluation& current, const AccountRules& rules, Money equity,
    const std::vector<EvaluationDay>& history, int days, int samples, std::uint64_t seed) {
  if (days < 1 || days > 252 || samples < 1 || samples > 10000)
    throw std::invalid_argument("days must be 1–252 and samples 1–10000");
  if (!rules.evaluation()) throw std::invalid_argument("This account has no evaluation target or drawdown rule");
  // The day's open and close are points on its path too. The recorded extremes
  // start from the day's first fully marked observation, so a day that gapped and
  // kept going has a low above its open or a high below it: widen them to cover
  // the open and close rather than drop the trending days that decide attempts.
  struct Day { Money open, low, high, close; bool low_first; };
  std::vector<Day> usable;
  for (const auto& day : history) {
    if (!day.low_equity || !day.high_equity || day.low_at <= 0 || day.high_at <= 0) continue;
    usable.push_back({day.open_equity, std::min({*day.low_equity, day.open_equity, day.close_equity}),
        std::max({*day.high_equity, day.open_equity, day.close_equity}), day.close_equity, day.low_at <= day.high_at});
  }
  if (usable.size() < 10) throw std::invalid_argument("Pass odds need at least 10 completed days with recorded intraday lows and highs");
  PassOdds result;
  result.historical_days = usable.size();
  result.seed = seed;
  std::mt19937_64 random(seed);
  std::vector<int> passed;
  int failed = 0;
  // The attempt's finished days count toward its objectives; the projection then
  // treats each simulated path as both equity and closed balance, flat at every
  // observation, and assumes an execution on every simulated day. Whole-trade
  // counts, best trade and short-round-trip profit remain fixed: no new closed
  // trades are invented by a sampled equity path.
  auto initial = current;
  initial.payouts.clear();
  const auto counted = needs_days(rules);
  const auto close_day = [&](Evaluation& e, DayStats& stats, Money close) {
    e.day_close_equity = close;
    if (counted) {
      EvaluationDay day;
      day.day = e.day;
      day.open_equity = e.day_open_equity;
      day.close_equity = close;
      day.realised = close - e.day_open_equity;
      day.executions = e.day_executions;
      add_day(stats, rules, day.day, day_profit(day, rules), day.executions);
    }
    evaluation_rollover(e, rules);
  };
  const auto finished = counted ? finished_day_stats(initial, rules) : DayStats{};
  for (int sample = 0; sample < samples; ++sample) {
    auto evaluation = initial;
    auto stats = finished;
    auto balance = equity;
    // Start the first projected day after closing the current day, whose profit so
    // far counts on the equity basis.
    evaluation.day_open_realised = Money{};
    close_day(evaluation, stats, balance);
    std::size_t index = 0;
    int elapsed = 0;
    for (; elapsed < days && evaluation.status == EvaluationStatus::Active; ++elapsed) {
      if (elapsed % 3 == 0) index = static_cast<std::size_t>(random() % usable.size());
      const auto& day = usable[index];
      index = (index + 1) % usable.size();
      evaluation.day = md::date_from_days(md::days_since_epoch(evaluation.day) + 1);
      Timestamp projected_time = 0;
      if (rules.time_limit_days > 0 || rules.inactivity_days > 0) {
        evaluation.day = plan_trading_date(rules, md::new_york_to_utc(evaluation.day, 10, 0));
        projected_time = md::new_york_to_utc(evaluation.day, 10, 0);
        if (const auto timed = evaluate_time_rules(evaluation, rules, projected_time); timed.decided()) {
          evaluation.status = timed.status;
          break;
        }
        if (rules.inactivity_days > 0) evaluation.last_activity = projected_time;
      }
      evaluation.day_open_equity = balance;
      evaluation.day_high_equity = balance;
      evaluation.day_lock = Reason::NONE;
      evaluation.day_executions = 1;
      auto close = balance + day.close - day.open;
      const auto observe = [&](Money historical) {
        if (evaluation.status != EvaluationStatus::Active || evaluation.day_lock != Reason::NONE) return;
        const auto value = balance + historical - day.open;
        evaluation.day_high_equity = std::max(*evaluation.day_high_equity, value);
        const auto verdict = evaluate_with(evaluation, rules, {value, value, value - evaluation.day_open_equity, true, projected_time}, &stats);
        if (verdict.decided()) evaluation.status = verdict.status;
        // A lock closes the account at its level for the rest of the day.
        if (verdict.lock) { evaluation.day_lock = verdict.code; close = verdict.level; }
      };
      observe(day.open);
      observe(day.low_first ? day.low : day.high);
      observe(day.low_first ? day.high : day.low);
      observe(day.close);
      balance = close;
      close_day(evaluation, stats, balance);
    }
    if (evaluation.status == EvaluationStatus::Passed) passed.push_back(elapsed);
    if (evaluation.status == EvaluationStatus::Failed) ++failed;
  }
  result.pass = static_cast<double>(passed.size()) / samples;
  result.fail = static_cast<double>(failed) / samples;
  result.neither = static_cast<double>(samples - static_cast<int>(passed.size()) - failed) / samples;
  if (!passed.empty()) {
    std::sort(passed.begin(), passed.end());
    result.median_days_to_pass = (passed[(passed.size() - 1) / 2] + passed[passed.size() / 2]) / 2.0;
  }
  return result;
}
RuleReplay replay_rules(const Evaluation& recorded, const PlanInputs& now, const std::vector<ReplayTrade>& trades,
    Money starting_balance, const AccountRules& rules) {
  RuleReplay result;
  Evaluation e;
  e.attempt = recorded.attempt;
  e.started = recorded.started;
  e.starting_balance = starting_balance;
  e.peak = starting_balance;
  e.floor = evaluation_floor(rules, starting_balance, e.floor_locked, starting_balance);
  e.day_open_equity = e.day_close_equity = starting_balance;
  if (rules.drawdown_basis == DrawdownBasis::Balance) e.day_close_balance = starting_balance;
  e.cycle_started = recorded.started;
  // The rule set moves by the recorded dollar P&L, withdrawals added back, from its
  // own starting balance; a lock flattens it, so later days move from the lock's level.
  Money shift = starting_balance - recorded.starting_balance;
  Money realised_total;
  std::size_t next_trade = 0;
  std::vector<ReplayTrade> sorted = trades;
  std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.closed < b.closed; });
  const auto take_trades = [&](Timestamp until) {
    for (; next_trade < sorted.size() && sorted[next_trade].closed <= until; ++next_trade) {
      const auto& trade = sorted[next_trade];
      ++e.closed_trades;
      if (trade.pnl > Money{} && (!e.best_trade || trade.pnl > e.best_trade->pnl)) e.best_trade = BestTrade{trade.id, trade.pnl};
      if (rules.microscalp_percent > 0 && trade.held < rules.microscalp_seconds * md::kNanosPerSecond)
        e.short_profit = e.short_profit + std::max(Money{}, trade.pnl);
      if (rules.best_position_percent > 0 && trade.pnl > Money{}) {
        if (!e.best_position || trade.pnl > e.best_position->pnl) e.best_position = BestTrade{trade.id, trade.pnl};
        e.position_excess = e.position_excess + std::max(Money{}, trade.pnl - best_position_cap(rules));
      }
    }
  };
  struct Day { md::Date day; Money open, close, realised; std::optional<Money> low, high; Timestamp low_at = 0, high_at = 0;
               std::uint64_t executions = 0; bool today = false; Timestamp close_at = 0; };
  std::vector<Day> path;
  for (const auto& d : recorded.days)
    path.push_back({d.day, d.open_equity, d.close_equity, d.realised, d.low_equity, d.high_equity, d.low_at, d.high_at,
                    d.executions > 0 ? d.executions : d.realised != Money{} ? 1u : 0u, false, 0});
  if (recorded.started > 0) {
    const auto realised = now.net_realised - recorded.day_open_realised;
    path.push_back({recorded.day, recorded.day_open_equity, now.equity, realised, recorded.day_low_equity, recorded.day_high_equity,
                    recorded.day_low_at, recorded.day_high_at,
                    recorded.day_executions > 0 ? recorded.day_executions : realised != Money{} ? 1u : 0u, true, now.time});
  }
  // Everything a payout took out of the equity: the amount, the profit a review
  // forfeited with it and what a reset removed.
  const auto withdrawn = [&](md::Date day) {
    Money total;
    for (const auto& payout : recorded.payouts)
      if (payout.day <= day)
        total = total + payout.amount + (payout.review ? payout.review->forfeited : Money{}) + payout.removed;
    return total;
  };
  std::optional<md::Date> decided;
  for (std::size_t i = 0; i < path.size(); ++i) {
    const auto& day = path[i];
    e.day = day.day;
    e.day_open_realised = realised_total;
    e.day_lock = Reason::NONE;
    const auto added = withdrawn(day.day);
    const auto value = [&](Money recorded_value) { return recorded_value + shift + added; };
    const auto open_balance = starting_balance + realised_total;
    e.day_open_equity = value(day.open);
    e.day_close_equity = e.day_open_equity;
    e.day_low_equity = e.day_high_equity = e.day_open_equity;
    e.day_executions = day.executions;
    // Calendar rules read the observation's time; days without one are taken at noon New York time.
    const auto noon = md::new_york_to_utc(day.day, 12, 0);
    std::string outcome = decided ? "after" : "active";
    Money close = value(day.close);
    const auto observe = [&](Money equity, Money balance, bool flat, Timestamp time, Timestamp shown) {
      if (decided || e.day_lock != Reason::NONE) return;
      take_trades(time);
      if (equity < *e.day_low_equity) e.day_low_equity = equity;
      if (equity > *e.day_high_equity) e.day_high_equity = equity;
      e.day_close_equity = equity;
      if (rules.drawdown_basis == DrawdownBasis::Balance) e.day_close_balance = balance;
      const PlanInputs inputs{equity, balance, realised_total + (balance - open_balance), flat, time};
      const auto verdict = evaluate_plan(e, rules, inputs);
      if (rules.max_drawdown > Money{}) {
        const auto room = (rules.drawdown_basis == DrawdownBasis::Balance ? balance : equity) - e.floor;
        if (!result.closest_floor || room < *result.closest_floor) result.closest_floor = room;
      }
      if (verdict.decided()) {
        e.status = verdict.status;
        decided = day.day;
        result.status = verdict.status;
        result.code = verdict.code;
        result.message = verdict.message;
        result.decided_day = day.day;
        result.decided_at = shown;
        result.decided_equity = equity;
        outcome = verdict.status == EvaluationStatus::Passed ? "passed" : "failed";
      } else if (verdict.lock) {
        e.day_lock = verdict.code;
        result.locked_days.push_back(day.day);
        outcome = "locked";
        close = verdict.level;
      }
    };
    const auto first = day.low_at > 0 && day.high_at > 0 ? std::min(day.low_at, day.high_at) : noon;
    observe(e.day_open_equity, open_balance, false, first, 0);
    const bool low_first = day.low_at <= day.high_at;
    const auto extreme = [&](bool low) {
      const auto& level = low ? day.low : day.high;
      const auto at = low ? day.low_at : day.high_at;
      if (level && at > 0) observe(value(*level), open_balance, false, at, at);
    };
    extreme(low_first);
    extreme(!low_first);
    const auto close_balance = open_balance + day.realised;
    const auto end = day.today ? now.time : std::max({day.low_at, day.high_at, noon});
    observe(close, close_balance, day.today ? now.flat : true, end, day.today ? now.time : 0);
    if (outcome == "locked") shift = shift + (close - value(day.close));
    realised_total = realised_total + day.realised;
    result.days.push_back({day.day, outcome, close, rules.max_drawdown > Money{} ? e.floor : Money{}});
    if (!day.today && !decided) {
      EvaluationDay finished;
      finished.day = day.day;
      finished.open_equity = e.day_open_equity;
      finished.close_equity = close;
      finished.realised = day.realised;
      finished.executions = day.executions;
      finished.locked = e.day_lock;
      e.day_close_equity = close;
      evaluation_rollover(e, rules);
      finished.peak = e.peak;
      finished.floor = e.floor;
      e.days.push_back(finished);
      result.days.back().floor = rules.max_drawdown > Money{} ? e.floor : Money{};
    }
  }
  return result;
}

ChallengeEconomics challenge_economics(const std::vector<EconomicsStep>& steps, double payout_odds, Money payout) {
  if (steps.empty()) throw std::invalid_argument("Challenge economics needs at least one program step");
  if (!(payout_odds >= 0 && payout_odds <= 1)) throw std::invalid_argument("Payout odds must be from 0 to 1");
  ChallengeEconomics result;
  result.payout_odds = payout_odds;
  result.payout = payout.dollars();
  const auto run = [&](double first_pass, std::vector<EconomicsStepResult>* out) {
    // Value from the funded step backwards: each step pays its costs, then reaches
    // the next with its eventual pass odds.
    double value = payout_odds * payout.dollars();
    double cost_total = 0;
    double reach = 1;
    std::vector<EconomicsStepResult> results;
    for (std::size_t i = 0; i < steps.size(); ++i) {
      const auto& step = steps[i];
      const double p = i == 0 ? first_pass : step.pass;
      const auto attempts = step.max_resets > 0 ? step.max_resets + 1 : 100;
      double fail_all = 1, expected_resets = 0, miss = 1;
      for (std::int64_t k = 1; k < attempts; ++k) { miss *= 1 - p; expected_resets += miss; }
      for (std::int64_t k = 0; k < attempts; ++k) fail_all *= 1 - p;
      const double cost = step.first_cost.dollars() + step.reset_cost.dollars() * expected_resets;
      results.push_back({step.plan, p, 1 - fail_all, cost, 1 + expected_resets});
      cost_total += reach * cost;
      reach *= 1 - fail_all;
    }
    if (out) *out = results;
    return std::pair{reach * value - cost_total, std::pair{cost_total, reach * value}};
  };
  const auto [value, parts] = run(steps.front().pass, &result.steps);
  result.expected_value = value;
  result.expected_cost = parts.first;
  result.expected_payout = parts.second;
  // The value rises with the first step's pass odds (costs fall, the payout's odds rise).
  const auto at = [&](double p) { return run(p, nullptr).first; };
  if (at(1) > 0 && at(0) < 0) {
    double low = 0, high = 1;
    for (int i = 0; i < 60; ++i) {
      const double middle = (low + high) / 2;
      (at(middle) < 0 ? low : high) = middle;
    }
    result.break_even_pass = high;
  } else if (at(0) >= 0) {
    result.break_even_pass = 0;
  }
  return result;
}
}  // namespace openport::trading
