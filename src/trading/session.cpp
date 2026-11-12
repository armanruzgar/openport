#include "openport/trading/session.hpp"

#include "session_internal.hpp"

namespace openport::trading {
namespace session_internal {
namespace {
/// Why a quote that is not executable cannot fill paper orders, in words.
std::string quote_problem(const QuoteObservation& q) {
  if (q.modelled && q.observation > 0) return "Modelled: no listed series quotes it";
  if (q.observation == 0 || (!q.bid && !q.ask)) return "No quote";
  if (!q.bid || *q.bid <= Money{}) return "No bid: the quote is one-sided";
  if (!q.ask || *q.ask <= Money{}) return "No ask: the quote is one-sided";
  if (*q.ask < *q.bid) return "Crossed: the bid is above the ask";
  return "A side shows no displayed size";
}
/// AM series retain observations strictly before last trade. PM expiry and
/// settlement keep their existing paths; retirement data cannot refresh an AM close.
bool retired_observation(const md::OptionContract& contract, Timestamp time) {
  return contract.last_trade_time() < contract.expiry_time() && time >= contract.last_trade_time();
}
/// A share price printed before a split of its stock went ex prices the shares before
/// it: on the split shares it is that price over each such split's ratio. A split goes
/// ex at its date's New York midnight. Replay drivers 1-8 took such a price as it came,
/// so the pre-split close a feed still gave after the rollover into the ex-date valued
/// the split shares at it.
Money split_basis(const State& s, const std::string& symbol, Timestamp time, Money price) {
  if (!s.revised_corporate_actions) return price;
  for (const auto& r : s.corporate_actions)
    if (r.symbol == symbol && time < md::new_york_to_utc(r.ex_date, 0, 0)) price = price.prorate(r.denominator, r.numerator);
  return price;
}
Money average_unit_price(const Position& p) {
  const auto size = magnitude(p.quantity);
  if (size == 0) return {};
  return (p.basis < Money{} ? -p.basis : p.basis).prorate(1, size * 100);
}
Money whole_cents(Money value) {
  return value > Money{} ? Money::from_micros(value.micros() / 10'000 * 10'000) : Money{};
}
Evaluation fresh_evaluation(const State& s, std::uint64_t attempt) {
  Evaluation e;
  e.revised_rules = s.evaluation.revised_rules;
  e.attempt = attempt;
  e.started = s.time;
  e.starting_balance = s.ledger.account().cash;
  e.scaling_limit = scaling_limit(s.config.rules, Money{});
  if (s.config.rules.size_scaling) {
    if (e.starting_balance <= Money{} || s.config.rules.size_scaling->max_balance < e.starting_balance)
      throw TradingError(Reason::INVALID_RULES, "Account size scaling maximum must be at least the positive starting balance");
    e.size_scaling = SizeScalingProgress{e.starting_balance, s.config.rules.max_drawdown, s.config.rules.daily_loss_limit,
        s.day, 0, e.starting_balance, e.starting_balance, 0, {}};
  }
  e.peak = e.starting_balance;
  e.floor = evaluation_floor(s.config.rules, e.peak, e.floor_locked, e.starting_balance);
  if (e.revised_rules && s.config.rules.open_loss_cap && s.config.rules.open_loss_cap->basis != OpenLossBasis::Amount)
    e.day_open_floor = e.floor;
  e.day_open_realised = net_realised(s);
  e.cycle_started = s.time;
  e.first_order = static_cast<OrderId>(s.orders.size() + 1);
  e.first_fill = s.fills.size() + 1;
  if (s.revised_share_orders) e.first_stock_order = static_cast<OrderId>(s.stock_orders.size() + 1);
  // Where the attempt's stock fills start, so share round trips opened before a reset
  // at its own market time stay with the attempt they opened in. Replay drivers 1-11
  // kept it only for microscalping, which counts this attempt's share round trips.
  if (s.revised_share_attempts || (s.config.rules.phase != Phase::Funded && s.config.rules.microscalp_percent > 0))
    e.first_stock_fill = s.stock_fills.size() + 1;
  e.day = s.day;
  e.day_open_equity = e.starting_balance;
  e.day_close_equity = e.starting_balance;
  if (s.config.rules.drawdown_basis == DrawdownBasis::Balance) e.day_close_balance = e.starting_balance;
  return e;
}
/// Capital changes neither P&L nor the account's distance above its floor.
/// A review always restarts once its finished-day count is reached.
Money review_size_scaling(State& s, md::Date next_day, Money balance, Events& events) {
  auto& e = s.evaluation;
  auto& rules = s.config.rules;
  if (!rules.size_scaling || !e.size_scaling) return {};
  auto& period = *e.size_scaling;
  const auto& rule = *rules.size_scaling;
  if (++period.period_days < static_cast<std::uint64_t>(rule.days)) return {};
  Money increase;
  if (e.status == EvaluationStatus::Active && e.starting_balance < rule.max_balance &&
      size_scaling_profit(e, balance) >= size_scaling_required(e, rule) &&
      e.payouts.size() - period.payouts_at_start >= static_cast<std::uint64_t>(rule.payouts)) {
    increase = size_scaling_increase(e, rule);
    if (increase > Money{}) {
      const auto old = e.starting_balance;
      e.starting_balance = old + increase;
      s.ledger.add_capital(increase);
      e.peak = e.peak + increase;
      if (rules.max_drawdown > Money{}) e.floor = e.floor + increase;
      if (rules.lock_balance > Money{}) rules.lock_balance = rules.lock_balance + increase;
      const auto scale_loss = [&](Money original) {
        __extension__ using Wide = __int128;
        const Wide cents = static_cast<Wide>(original.micros()) * e.starting_balance.micros() /
            period.original.micros() / 10'000;
        if (cents > std::numeric_limits<std::int64_t>::max() / 10'000)
          throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Scaled loss limit overflow");
        return Money::from_micros(static_cast<std::int64_t>(cents) * 10'000);
      };
      rules.max_drawdown = scale_loss(period.original_max_drawdown);
      rules.daily_loss_limit = scale_loss(period.original_daily_loss_limit);
      period.history.push_back({next_day, old, e.starting_balance});
      event(events, "account_scaled", Json{{"old", old}, {"size", e.starting_balance}, {"increase", increase},
          {"period_start", period.period_started}, {"period_end", e.day}, {"day", next_day}});
    }
  }
  period.period_started = next_day;
  period.period_days = 0;
  period.period_balance = balance + increase;
  period.period_size = e.starting_balance;
  period.payouts_at_start = e.payouts.size();
  return increase;
}
/// Why shares cannot trade now: they need the stock market's regular session and
/// the underlying's fresh price.
Decision share_close_check(const State& s, const std::string& symbol) {
  if (!md::market_session(s.time).open) return failure(Reason::SESSION_CLOSED, "Stock trades in the regular session");
  if (s.time < halt_end(s, symbol)) return failure(Reason::MARKET_HALTED, "Trading is halted for " + symbol);
  if (!stock_price(s, symbol)) return failure(Reason::STALE_QUOTE, "Needs a fresh price for " + symbol);
  return {};
}
/// The room sizing shares: above the nearer of the plan and soft floors, if any.
std::optional<Money> floor_room(const State& s) {
  const auto snapshot = measure(s);
  std::optional<Money> room;
  if (s.config.rules.max_drawdown > Money{}) room = snapshot.equity - s.evaluation.floor;
  if (snapshot.soft_floor) room = room ? std::min(*room, snapshot.equity - *snapshot.soft_floor) : snapshot.equity - *snapshot.soft_floor;
  return room;
}
void check_floor_share(double floor_share) {
  if (!std::isfinite(floor_share) || floor_share <= 0 || floor_share > 1)
    throw TradingError(Reason::INVALID_ORDER, "floor_share must be greater than zero and at most one");
}
std::string underlying_of(const State& s, const Order& o) {
  const auto c = s.contracts.find(order_symbols(o.request).front());
  return c == s.contracts.end() ? std::string{} : c->second.underlying;
}
void require_reason(const std::string& reason) {
  if (reason.find_first_not_of(" \t\r\n") == std::string::npos)
    throw TradingError(Reason::INVALID_REASON, "An explicit nonblank reason is required");
}
}  // namespace
}  // namespace session_internal

PlanInputs plan_inputs(const TradingSnapshot& s) {
  PlanInputs in;
  in.time = s.time;
  in.equity = s.equity;
  in.balance = s.account.cash;
  for (const auto& p : s.positions) in.balance = in.balance + p.position.basis;
  for (const auto& p : s.stocks) in.balance = in.balance + p.position.basis;
  in.net_realised = s.account.realised - s.account.fees;
  in.flat = s.positions.empty() && s.stocks.empty();
  return in;
}
ProgramCosts program_costs(const TradingSnapshot& snapshot, const AccountRules& rules) {
  ProgramCosts out;
  const auto add = [&](const AttemptFee& fee) {
    if (fee.kind == "evaluation") out.evaluation = out.evaluation + fee.amount;
    else if (fee.kind == "reset") out.reset = out.reset + fee.amount;
    else if (fee.kind == "activation") out.activation = out.activation + fee.amount;
  };
  add(snapshot.fee_charged);
  for (const auto& a : snapshot.attempts) { add(a.fee_charged); out.payouts_received = out.payouts_received + a.payouts_received; }
  for (const auto& p : snapshot.evaluation.payouts) out.payouts_received = out.payouts_received + p.trader_share;
  if (snapshot.payouts_received) out.payouts_received = *snapshot.payouts_received;
  const auto& e = snapshot.evaluation;
  const auto original = e.size_scaling ? e.size_scaling->original : e.starting_balance;
  const auto original_rules = original_program_rules(rules, e.starting_balance, e.size_scaling);
  for (auto it = snapshot.attempts.rbegin(); it != snapshot.attempts.rend(); ++it) {
    const auto previous = it->size_scaling ? it->size_scaling->original : it->starting_balance;
    if (!it->rules || previous != original ||
        !same_program_rules(original_program_rules(*it->rules, it->starting_balance, it->size_scaling), original_rules)) break;
    ++out.resets_used;
  }
  return out;
}
std::vector<ReviewTrade> review_offences(const Evaluation& e, const AccountRules& rules, Money cycle_profit) {
  std::vector<ReviewTrade> out;
  const auto& review = rules.payouts.review;
  if (!review) return out;
  __extension__ using Wide = __int128;
  for (auto trade : e.cycle_trades) {
    if (!in_payout_cycle(e, trade.closed) || trade.pnl <= Money{}) continue;
    if (review->min_hold_seconds > 0 && trade.held < review->min_hold_seconds * md::kNanosPerSecond)
      trade.offences.push_back("short_hold");
    // Exactly: pnl × 100 > percent × cycle profit.
    if (review->max_trade_percent > 0 &&
        static_cast<Wide>(trade.pnl.micros()) * 100 > static_cast<Wide>(std::max(Money{}, cycle_profit).micros()) * review->max_trade_percent)
      trade.offences.push_back("oversized");
    if (review->open_loss && trade.open_loss) trade.offences.push_back("open_loss");
    if (!trade.offences.empty()) out.push_back(std::move(trade));
  }
  return out;
}
PayoutQuote payout_quote(const TradingSnapshot& s, const AccountRules& rules) {
  __extension__ using Wide = __int128;
  const auto& p = rules.payouts;
  const auto& e = s.evaluation;
  PayoutQuote q;
  q.number = e.payouts.size() + 1;
  q.funded = rules.phase == Phase::Funded && p.qualifying_days > 0;
  q.active = e.status == EvaluationStatus::Active;
  // Shares count as holdings: their marks are in equity, not in cash.
  q.flat = s.positions.empty() && s.stocks.empty() && s.open_orders.empty();
  q.qualifying_days = e.qualifying_days;
  q.required_days = p.qualifying_days;
  const auto add_day = [&](md::Date day, Money profit) {
    q.cycle_profit = q.cycle_profit + profit;
    if (profit > Money{} && (!q.best_day || profit > *q.best_day)) {
      q.best_day = profit;
      q.best_day_date = day;
    }
  };
  // Like qualifying_days, a finished day belongs to the cycle in progress at
  // rollover. The payout request's trading day belongs to the next cycle too.
  if (q.funded) {
    for (const auto& day : e.days)
      if (e.payouts.empty() || day.day >= e.payouts.back().day) add_day(day.day, day.realised);
    if (e.started > 0) add_day(e.day, s.account.realised - s.account.fees - e.day_open_realised);
  }
  bool consistent = true;
  if (!p.consistency_percents.empty()) {
    q.consistency_percent = p.consistency_percents.at(std::min<std::size_t>(q.number, p.consistency_percents.size()) - 1);
    const Wide best = q.best_day.value_or(Money{}).micros();
    const Wide missing = best * 100 - static_cast<Wide>(q.cycle_profit.micros()) * *q.consistency_percent;
    consistent = q.cycle_profit > Money{} && missing <= 0;
    if (missing > 0) {
      const Wide divisor = *q.consistency_percent * 10'000;
      const Wide micros = (missing + divisor - 1) / divisor * 10'000;
      if (micros > std::numeric_limits<std::int64_t>::max())
        throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Payout consistency profit needed exceeds the money range");
      q.consistency_needed = Money::from_micros(static_cast<std::int64_t>(micros));
    }
  }
  if (q.funded && p.review) {
    if (auto offending = review_offences(e, rules, q.cycle_profit); !offending.empty()) {
      // The nth review that finds offending trades takes the nth step of the ladder.
      const auto earlier = static_cast<std::size_t>(std::count_if(e.payouts.begin(), e.payouts.end(),
          [](const Payout& payout) { return payout.review && !payout.review->trades.empty(); }));
      PayoutReviewOutcome outcome;
      outcome.step = p.review->ladder.at(std::min(earlier, p.review->ladder.size() - 1));
      for (const auto& trade : offending) outcome.forfeited = outcome.forfeited + trade.pnl;
      if (outcome.step == ReviewStep::Warning) outcome.forfeited = {};
      outcome.trades = std::move(offending);
      q.forfeit = whole_cents(outcome.forfeited);
      outcome.forfeited = q.forfeit;
      q.review = std::move(outcome);
    }
  }
  // Forfeited profit leaves with the payout, so neither it nor the payout may come from it.
  const auto equity = s.equity - q.forfeit;
  q.profit = s.equity - e.starting_balance;
  const auto payable = q.profit - q.forfeit;
  q.withdrawable = whole_cents(payable > Money{} ? payable.prorate(p.withdrawal_percent, 100) : Money{});
  if (!p.caps.empty()) q.cap = p.caps.at(std::min<std::size_t>(q.number, p.caps.size()) - 1);
  q.maximum = q.cap ? std::min(q.withdrawable, *q.cap) : q.withdrawable;
  if (q.review && q.review->step == ReviewStep::Cap) q.maximum = std::min(q.maximum, p.review->cap);
  // Touching the floor fails the account; an unlocked floor moves down with the
  // withdrawal, a locked one does not, and one raised to the starting balance after it
  // must stay below the equity left.
  if (rules.max_drawdown > Money{} && e.floor_locked)
    q.maximum = std::min(q.maximum, whole_cents(equity - e.floor - Money::from_micros(10'000)));
  if (p.after_payout == PayoutFloor::StartingBalance)
    q.maximum = std::min(q.maximum, whole_cents(equity - std::max(e.floor, e.starting_balance) - Money::from_micros(10'000)));
  if (p.after_payout == PayoutFloor::Reset) {
    q.reset_balance = whole_cents(e.starting_balance.prorate(p.reset_percent, 100));
    q.maximum = std::min(q.maximum, whole_cents(equity - *q.reset_balance));
  }
  bool buffer_blocks = false;
  if (p.buffer > Money{} && (p.buffer_payouts == 0 || q.number <= static_cast<std::uint64_t>(p.buffer_payouts))) {
    q.buffer_balance = e.starting_balance + p.buffer;
    const auto room = whole_cents(equity - *q.buffer_balance);
    buffer_blocks = room <= Money{};
    q.maximum = std::min(q.maximum, room);
  }
  q.minimum = p.minimum;
  q.trader_share = q.maximum.prorate(p.split_percent, 100);
  auto block = [&](Reason code, std::string message, std::optional<double> actual = {}, std::optional<double> limit = {}) {
    if (q.blocked.ok()) q.blocked = {code, std::move(message), actual, limit, "aggregate"};
  };
  if (!q.funded) block(Reason::PAYOUT_UNAVAILABLE, "Payouts are available on funded accounts");
  if (!q.active) block(Reason::PAYOUT_UNAVAILABLE, "The funded account is closed");
  if (!q.flat) block(Reason::PAYOUT_NOT_ELIGIBLE, "Close every position, share holding and working order before requesting a payout");
  if (static_cast<std::int64_t>(q.qualifying_days) < q.required_days)
    block(Reason::PAYOUT_NOT_ELIGIBLE, "Not enough qualifying days in this payout cycle",
          static_cast<double>(q.qualifying_days), static_cast<double>(q.required_days));
  if (!consistent)
    block(Reason::PAYOUT_NOT_ELIGIBLE, "Payout consistency requires a positive cycle profit and a best day of at most " +
          std::to_string(*q.consistency_percent) + "% of its net realised profit",
          q.cycle_profit > Money{} ? std::optional<double>{q.best_day.value_or(Money{}).dollars() / q.cycle_profit.dollars() * 100} : std::nullopt,
          static_cast<double>(*q.consistency_percent));
  if (q.maximum <= Money{} || q.maximum < q.minimum)
    block(Reason::PAYOUT_NOT_ELIGIBLE, buffer_blocks ? "Equity must stay above the payout buffer of " + dollars(*q.buffer_balance)
                                                   : "The payout available is below the minimum", q.maximum.dollars(), q.minimum.dollars());
  return q;
}
TradingSession::TradingSession(SessionConfig config, Timestamp time, std::shared_ptr<Journal> journal, std::string actor,
                               bool revised_evaluation)
    : impl_(std::make_unique<Impl>()) {
  validate_limits(config.limits);
  validate_scenarios(config.scenarios);
  if (config.scenarios.days != std::vector<double>{0} || config.scenarios.beta_weighted || config.scenarios.term_scaled)
    throw TradingError(Reason::INVALID_SCENARIO, "Trading rules require a parallel zero-day grid; use read-only scenario analysis for beta, time and term shocks");
  normalize_event_rules(config.rules, true);
  validate_rules(config.rules);
  validate_guardrails(config.guardrails);
  if (time < 0) throw TradingError(Reason::INVALID_TIME, "Negative session time");
  if (config.fee_per_contract < Money{}) throw TradingError(Reason::INVALID_MONEY, "Fee cannot be negative");
  // As reset_account requires: percentage rules and payouts work from this balance.
  if (config.initial_cash <= Money{}) throw TradingError(Reason::INVALID_MONEY, "Starting balance must be positive");
  if (config.rules.size_scaling && config.rules.size_scaling->max_balance < config.initial_cash)
    throw TradingError(Reason::INVALID_RULES, "Account size scaling maximum must be at least the starting balance");
  if (journal && journal->sequence() != 0) throw TradingError(Reason::JOURNAL_CORRUPT, "Use recover for a nonempty journal");
  impl_->actor = std::move(actor);
  impl_->state.evaluation.revised_rules = revised_evaluation;
  impl_->state.config = std::move(config);
  impl_->state.time = time;
  impl_->state.day = plan_trading_date(impl_->state.config.rules, time);
  impl_->state.ledger = Ledger(impl_->state.config.initial_cash);
  impl_->state.start_equity = impl_->state.config.initial_cash;
  impl_->state.evaluation = fresh_evaluation(impl_->state, 1);
  impl_->journal = std::move(journal);
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
  impl_->transact(time, "session_start", [](State& s, Events& events) {
    s.fee_charged = {s.config.rules.evaluation_fee, s.config.rules.evaluation_fee > Money{} ? "evaluation" : ""};
    Json start = s.config;
    if (s.fee_charged.amount != Money{}) start["fee_charged"] = s.fee_charged;
    event(events, "session_start", start);
    return CommandResult{};
  });
}
TradingSession::TradingSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
TradingSession::~TradingSession() = default;
void TradingSession::set_actor(std::string actor) { impl_->actor = std::move(actor); }
void TradingSession::set_exact_margin(bool exact) {
  impl_->state.exact_margin = exact;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_evaluation(bool revised) {
  impl_->state.evaluation.revised_rules = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_fills(bool revised) {
  impl_->state.revised_fills = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_corporate_actions(bool revised) {
  impl_->state.revised_corporate_actions = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_share_orders(bool revised) {
  impl_->state.revised_share_orders = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_lifecycle(bool revised) {
  impl_->state.revised_lifecycle = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_share_attempts(bool revised) {
  impl_->state.revised_share_attempts = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_revised_margin_budget(bool revised) {
  impl_->state.revised_margin_budget = revised;
  impl_->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(impl_->state));
}
void TradingSession::set_opening_rule_inputs(std::optional<OpeningRuleInputs> inputs, bool compact) {
  impl_->opening_rule_inputs = std::move(inputs);
  impl_->compact_opening_rule_inputs = compact;
}
void TradingSession::record_input(std::string_view input, Timestamp time) {
  const auto data = Json::parse(input);
  if (!data.is_object()) throw std::invalid_argument("Run input must be an object");
  impl_->transact(time, "run_input", [&](State&, Events& events) {
    event(events, "run_input", data);
    return CommandResult{};
  });
}
TradingSession::TradingSession(TradingSession&&) noexcept = default;
TradingSession& TradingSession::operator=(TradingSession&&) noexcept = default;
CommandResult TradingSession::define(const md::OptionContract& contract, Timestamp time, std::optional<Deliverable> deliverable) {
  return impl_->transact(time, "definition", [&](State& s, Events& events) {
    const auto symbol = contract.osi_symbol();
    // A series the account has adjusted keeps the deliverable it adjusted it to.
    const auto known = s.deliverables.find(symbol);
    if (!deliverable && known != s.deliverables.end()) deliverable = known->second;
    const auto decision = eligible(contract, deliverable ? &*deliverable : nullptr);
    if (!decision.ok()) return CommandResult{decision, {}, 0};
    const auto old = s.contracts.find(symbol);
    if (old != s.contracts.end() && Json(old->second) != Json(contract))
      return CommandResult{failure(Reason::INVALID_CONTRACT, "OSI definition conflicts with registered terms"), {}, 0};
    if (known != s.deliverables.end() && deliverable && *deliverable != known->second)
      return CommandResult{failure(Reason::INVALID_CONTRACT, "The series' deliverable conflicts with the one registered"), {}, 0};
    s.contracts[symbol] = contract;
    Json defined = contract;
    if (deliverable && !standard_deliverable(*deliverable) && known == s.deliverables.end()) {
      s.deliverables[symbol] = *deliverable;
      defined["deliverable"] = *deliverable;
    }
    event(events, "definition", defined);
    return CommandResult{};
  });
}
CommandResult TradingSession::submit(OrderRequest request, Timestamp time, Decision rejection) {
  // A client retrying an order it may have lost the answer to gets that answer,
  // not a second order or a duplicate-key rejection, even after the order changed.
  // Other terms are refused without recording an order, so the ID keeps naming
  // one. Only this attempt's orders hold their IDs: after an account reset, an
  // ID an earlier attempt used names a new order.
  if (!impl_->stopped) {
    const auto& state = impl_->state;
    if (const auto named = state.clients.find(request.client_order_id);
        named != state.clients.end() && named->second >= state.evaluation.first_order) {
      const auto& first = state.orders.at(static_cast<std::size_t>(named->second - 1));
      if (first.submission() == request)
        return CommandResult{first.status == OrderStatus::Rejected ? first.reason : Decision{}, first.id, state.version, true};
      return CommandResult{failure(Reason::DUPLICATE_CLIENT_ID, "client_order_id already names order " + std::to_string(first.id) +
                                   ", submitted with other terms"), first.id, state.version, false};
    }
  }
  return impl_->transact(time, "submit", [&](State& s, Events& events) {
    monitor_loss(s, events);
    return place(s, std::move(request), time, rejection, events);
  });
}
RiskProfile snapshot_profile(const TradingSnapshot& snapshot, const SessionConfig& config,
    const Valuations& valuations, const ProfileConfig& profile, double reference_spot, const std::string& underlying) {
  const DeliverablesScope adjusted(&snapshot.deliverables);
  std::map<std::string, Position> positions;
  std::map<std::string, StockPosition> stocks;
  std::map<std::string, double> prices;
  for (const auto& marked : snapshot.positions)
    if (underlying.empty() || marked.position.contract.underlying == underlying)
      positions.emplace(marked.position.contract.osi_symbol(), marked.position);
  for (const auto& marked : snapshot.stocks) {
    if (!underlying.empty() && marked.position.symbol != underlying) continue;
    stocks.emplace(marked.position.symbol, marked.position);
    if (marked.fresh && marked.mark) prices[marked.position.symbol] = marked.mark->dollars();
  }
  auto result = risk_profile(Ledger::restore({}, std::move(positions), std::move(stocks)), valuations, profile,
      reference_spot, snapshot.equity,
      config.rules.max_drawdown > Money{} ? std::optional(snapshot.evaluation.floor) : std::nullopt,
      snapshot.soft_floor, snapshot.time, config.limits.max_valuation_age, prices);
  if (!snapshot.valuation_complete) {
    result.complete = false;
    result.room.reset(); result.soft_room.reset();
    for (auto& curve : result.curves) {
      std::fill(curve.pnl.begin(), curve.pnl.end(), 0.0);
      curve.down.reset(); curve.up.reset(); curve.soft_down.reset(); curve.soft_up.reset();
    }
  }
  return result;
}
ScenarioGrid snapshot_scenarios(const TradingSnapshot& snapshot, const SessionConfig& config,
    const Valuations& valuations, const SmileModel& smile) {
  std::map<std::string, Position> positions;
  std::map<std::string, StockPosition> stocks;
  std::map<std::string, double> prices;
  for (const auto& marked : snapshot.positions) positions.emplace(marked.position.contract.osi_symbol(), marked.position);
  for (const auto& marked : snapshot.stocks) {
    stocks.emplace(marked.position.symbol, marked.position);
    if (marked.fresh && marked.mark) prices[marked.position.symbol] = marked.mark->dollars();
  }
  return scenario_grid(Ledger::restore({}, std::move(positions), std::move(stocks)), valuations, config.scenarios,
      snapshot.time, config.limits.max_valuation_age, prices, smile);
}
BreachRisk TradingSession::breach(const std::map<std::string, double>& close_variances) const {
  return breach_of(impl_->state, close_variances);
}
std::vector<RiskWarning> TradingSession::warnings(const std::map<std::string, double>& close_variances,
                                                  const std::vector<Dividend>& dividends,
                                                  const std::vector<CorporateAction>& splits) const {
  return warnings_of(impl_->state, close_variances, dividends, splits);
}
/// What order `o` would trade at now on its executable sides: a single contract's
/// ask (buys) or bid (sells), a combo's signed net; nothing without fresh quotes.
std::optional<Money> executable_value(const State& s, const Order& o) {
  Order probe = o;
  probe.request.trigger = Trigger{multi_leg(o.request) ? TriggerSource::Combo : TriggerSource::Option, TriggerDirection::AtOrBelow, Money{}};
  return trigger_value(s, probe);
}
/// Advice on terms that are accepted but rarely meant (OrderPreview::warnings).
std::vector<OrderWarning> order_warnings(const State& s, const OrderRequest& r) {
  std::vector<OrderWarning> out;
  for (const auto& symbol : order_symbols(r))
    if (!s.contracts.contains(symbol) || !s.books.contains(symbol)) return out;
  const auto level_text = [](const Trigger& t) {
    if (t.source == TriggerSource::Time) {
      const auto clock = std::to_string(100 + t.minute / 60).substr(1) + ":" + std::to_string(100 + t.minute % 60).substr(1);
      return std::string(t.direction == TriggerDirection::AtOrBelow ? "at or before " : "at or after ") + clock + " New York time";
    }
    return std::string(t.direction == TriggerDirection::AtOrBelow ? "at or below " : "at or above ") + t.level.str();
  };
  // A time trigger reads the clock and has no value to show.
  const auto now_text = [&](const Order& o) {
    const auto value = trigger_value(s, o);
    return value ? " (now " + value->str() + ")" : std::string();
  };
  // A bracket's exits as attach_exits makes them; held exits already close.
  const auto exit_of = [&](const ExitSpec& spec, OrderRole role) {
    Order exit;
    exit.role = role;
    exit.request = r;
    exit.request.bracket.reset();
    exit.request.walk.reset();
    exit.request.exits_only = false;
    exit.request.trigger = spec.trigger;
    exit.request.limit_price = spec.limit_price;
    if (!r.exits_only) {
      exit.request.side = multi_leg(r) ? Side::Buy : r.side == Side::Buy ? Side::Sell : Side::Buy;
      for (auto& leg : exit.request.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
    }
    return exit;
  };
  const std::string fires = r.exits_only ? "as soon as it is accepted" : "as soon as the entry fills";
  // A combo's net limit is what it pays: an exit pays at most its limit.
  const auto marketable = [&](const Order& exit, Money value) {
    return exit.request.side == Side::Buy ? value <= *exit.request.limit_price : value >= *exit.request.limit_price;
  };
  if (r.bracket && r.bracket->stop_loss) {
    const auto exit = exit_of(*r.bracket->stop_loss, OrderRole::StopLoss);
    const auto value = executable_value(s, exit);
    if (!exit.request.trigger && exit.request.limit_price) {
      std::string message = "The stop_loss has a limit price and no trigger, so it rests as a limit exit at " +
          exit.request.limit_price->str() + ", not a stop";
      if (value && marketable(exit, *value)) message += "; the closing price now, " + value->str() + ", already reaches it, so it closes the position " + fires;
      out.push_back({"STOP_AS_LIMIT", message + ". Give the stop a trigger instead."});
    } else if (exit.request.trigger && reached(s, exit)) {
      out.push_back({"STOP_REACHED", "The stop's trigger, " + level_text(*exit.request.trigger) + ", is already reached" +
                     now_text(exit) + ": it fires " + fires});
    }
  }
  if (r.bracket && r.bracket->take_profit && r.bracket->take_profit->limit_price && !r.bracket->take_profit->trigger) {
    const auto exit = exit_of(*r.bracket->take_profit, OrderRole::TakeProfit);
    if (const auto value = executable_value(s, exit); value && marketable(exit, *value))
      out.push_back({"TARGET_REACHED", "The take-profit, " + exit.request.limit_price->str() + ", is already marketable (now " +
                     value->str() + "): it fills " + fires});
  }
  if (r.trigger && !r.exits_only) {
    Order armed;
    armed.request = r;
    if (reached(s, armed))
      out.push_back({"TRIGGER_REACHED", "The trigger, " + level_text(*r.trigger) + ", is already reached" +
                     now_text(armed) + ": the order activates at once"});
  }
  // Slippage that alone pushes a market order's price outside the band.
  if (!multi_leg(r) && !r.limit_price && s.config.rules.slippage_ticks > 0 && quote_check(s, r.symbol).ok()) {
    const auto& quote = s.books.at(r.symbol).quote;
    const auto displayed = r.side == Side::Buy ? *quote.ask : *quote.bid;
    const auto slipped = execution_price(s, r.symbol, r.side);
    if (price_check(s, quote, displayed).ok() && !price_check(s, quote, slipped).ok())
      out.push_back({"SLIPPAGE_BAND", "The account's " + std::to_string(s.config.rules.slippage_ticks) + " slippage ticks price this order at " +
                     slipped.str() + ", outside the price band around the mid " + mid(quote).str() +
                     ", so it is refused PRICE_BAND; widen the band or lower the slippage"});
  }
  // A size-creep guardrail that only warns: the order is accepted and flagged as tilt.
  if (!s.config.guardrails.size_creep_block) {
    Order opening;
    opening.request = r;
    if (const auto creep = size_creep(s, opening))
      out.push_back({"SIZE_CREEP", size_creep_message(*creep, s.config.guardrails.size_creep_percent) +
                     "; it is accepted, and flagged as tilt in the journal"});
  }
  return out;
}
OrderPreview TradingSession::preview(const OrderRequest& request, Timestamp time, double floor_share,
    Decision rejection, const std::map<std::string, double>& close_variances, const PreviewMarket& market) const {
  check_floor_share(floor_share);
  // Submit answers an identical retry before advancing market time. It cannot
  // create another fill or reserve more buying power, even if the feed changed.
  if (!impl_->stopped) {
    const auto& state = impl_->state;
    const auto named = state.clients.find(request.client_order_id);
    const auto* first = named == state.clients.end() || named->second < state.evaluation.first_order
        ? nullptr : &state.orders.at(static_cast<std::size_t>(named->second - 1));
    if (first && first->submission() == request) {
      const auto snapshot = snapshot_of(state);
      OrderPreview retry;
      retry.decision = first->status == OrderStatus::Rejected ? first->reason : Decision{};
      retry.buying_power_before = snapshot.buying_power.available;
      retry.buying_power_after = retry.buying_power_before;
      retry.exposure_change = Exposure{};
      retry.breach = breach_of(state, close_variances);
      retry.execution.status = first->status;
      retry.execution.reason = first->reason;
      return retry;
    }
  }
  const auto before = prepared(impl_->state, time, market);
  Events ignored;
  auto projection = project_order(before, request, rejection);
  auto result = projection.result;
  result.warnings = order_warnings(before, request);
  if (impl_->stopped) result.decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
  // What submitting it now would execute: the same acceptance and matching, on a copy.
  if (!impl_->stopped) {
    State trial = before;
    try {
      if (const auto placed = place(trial, request, time, rejection, ignored); placed.order_id) {
        result.execution = executed(before, trial, *placed.order_id, std::move(result.execution));
        result.next_walk = next_walk(trial.orders[*placed.order_id - 1]);
      }
    } catch (const TradingError&) {}
  }
  // Each leg's quote: whether it can fill now, its displayed size on the leg's
  // side and what this account's orders have left of that observation.
  auto legs = request.legs;
  if (legs.empty()) legs.push_back({request.symbol, request.side, 1});
  for (const auto& leg : legs) {
    Quantity contracts = 0;
    if (!before.contracts.contains(leg.symbol) || leg.ratio < 1 || request.quantity < 1 ||
        __builtin_mul_overflow(request.quantity, leg.ratio, &contracts))
      continue;
    LegLiquidity item{leg.symbol, leg.side, contracts, quote_check(before, leg.symbol), 0, 0};
    if (const auto book = before.books.find(leg.symbol); book != before.books.end()) {
      const bool buy = leg.side == Side::Buy;
      const auto& q = book->second.quote;
      const auto far = buy ? q.ask : q.bid;
      item.displayed = far && *far > Money{} ? (buy ? q.ask_size : q.bid_size) : 0;
      item.left = std::max<Quantity>(0, buy ? book->second.ask_left : book->second.bid_left);
      if (item.quote.code == Reason::INVALID_QUOTE) item.quote.message = quote_problem(book->second.quote);
    }
    result.liquidity.push_back(std::move(item));
  }
  result.breach = breach_of(projection.projected, close_variances);
  std::map<Quantity, OrderPreview> sized_previews;
  const auto sized_preview = [&](Quantity quantity) -> const OrderPreview& {
    const auto saved = sized_previews.find(quantity);
    if (saved != sized_previews.end()) return saved->second;
    auto sized = request;
    sized.quantity = quantity;
    OrderPreview value;
    try { value = project_order(before, sized, rejection).result; }
    catch (const TradingError& error) { value.decision = failure(error.code(), error.what()); }
    return sized_previews.emplace(quantity, std::move(value)).first->second;
  };
  // Held exits follow their position's size, so sizing them means nothing.
  if (impl_->stopped || request.exits_only) return result;
  Quantity upper = before.config.limits.max_order_contracts;
  for (const auto& leg : request.legs) {
    if (leg.ratio <= 0) return result;
    upper = std::min(upper, before.config.limits.max_order_contracts / leg.ratio);
  }
  const auto sizing = size_order(sized_preview, upper, floor_room(before), floor_share);
  result.max_units = sizing.units;
  result.max_units_basis = sizing.basis;
  result.max_units_buying_power = sizing.buying_power;
  result.max_units_floor = sizing.floor;
  return result;
}
OrderPreview TradingSession::preview_change(OrderId id, const OrderChange& change, Timestamp time, double floor_share,
    Decision rejection, const std::map<std::string, double>& close_variances, const PreviewMarket& market) const {
  check_floor_share(floor_share);
  const auto before = prepared(impl_->state, time, market);
  OrderPreview result;
  // An unknown or finished order is the change's answer, as modify gives it.
  if (id == 0 || id > before.orders.size()) {
    result.decision = failure(Reason::UNKNOWN_ORDER, "Unknown order ID");
    return result;
  }
  if (!before.orders.at(static_cast<std::size_t>(id - 1)).open()) {
    result.decision = failure(Reason::ORDER_TERMINAL, "Order is already terminal");
    return result;
  }
  // The change itself, on a copy: its decision, and what it executes at once.
  State trial = before;
  Events ignored;
  auto decision = change_order(trial, id, change, rejection, ignored).decision;
  if (impl_->stopped) decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
  // The order on its new terms, unmatched: what it reserves, and the account once it fills.
  State changed = before;
  change_terms(changed, id, change);
  auto projection = project_working(before, std::move(changed), id, decision);
  result = std::move(projection.result);
  result.breach = breach_of(projection.projected, close_variances);
  result.execution = executed(before, trial, id, std::move(result.execution));
  result.next_walk = next_walk(trial.orders[id - 1]);
  // Sizing counts the units the order could still work, its filled ones aside.
  const auto& order = before.orders.at(static_cast<std::size_t>(id - 1));
  if (impl_->stopped || kept_within(order) || order.request.exits_only) return result;
  Quantity upper = before.config.limits.max_order_contracts;
  for (const auto& leg : order.request.legs) {
    if (leg.ratio <= 0) return result;
    upper = std::min(upper, before.config.limits.max_order_contracts / leg.ratio);
  }
  std::map<Quantity, OrderPreview> sized_previews;
  const auto sized_preview = [&](Quantity units) -> const OrderPreview& {
    const auto saved = sized_previews.find(units);
    if (saved != sized_previews.end()) return saved->second;
    auto sized = change;
    sized.quantity = order.filled_quantity + units;
    State resized = before;
    change_terms(resized, id, sized);
    OrderPreview value;
    try {
      auto check = change_check(resized, resized.orders.at(static_cast<std::size_t>(id - 1)), rejection);
      value = project_working(before, std::move(resized), id, std::move(check)).result;
    } catch (const TradingError& error) { value.decision = failure(error.code(), error.what()); }
    return sized_previews.emplace(units, std::move(value)).first->second;
  };
  const auto sizing = size_order(sized_preview, upper - order.filled_quantity, floor_room(before), floor_share);
  result.max_units = sizing.units;
  result.max_units_basis = sizing.basis;
  result.max_units_buying_power = sizing.buying_power;
  result.max_units_floor = sizing.floor;
  return result;
}
WhatIf TradingSession::what_if(const std::vector<std::vector<OrderRequest>>& candidates, Timestamp time,
    const std::vector<std::vector<Decision>>& rejections, const std::map<std::string, double>& close_variances,
    const PreviewMarket& market) const {
  WhatIf result;
  const auto before = prepared(impl_->state, time, market);
  const auto base = measure(before).equity;
  result.current = what_if_account(before, base, close_variances);
  for (std::size_t c = 0; c < candidates.size(); ++c) {
    WhatIfCandidate candidate;
    State state = before;
    bool projected = true;
    const bool chained = std::any_of(candidates[c].begin(), candidates[c].end(), [](const auto& order) {
      return !order.then.empty() || !order.oco.empty();
    });
    for (std::size_t i = 0; i < candidates[c].size(); ++i) {
      const auto rejection = c < rejections.size() && i < rejections[c].size() ? rejections[c][i] : Decision{};
      const auto id = static_cast<OrderId>(state.orders.size() + 1);
      if (chained) {
        // Chains depend on actual fills: limits, partial fills and OCO cancellation
        // decide which children exist. Run the same reducer on a journal-free copy.
        auto copy = std::make_unique<Impl>();
        copy->state = state;
        copy->opening_rule_inputs = impl_->opening_rule_inputs;
        copy->compact_opening_rule_inputs = impl_->compact_opening_rule_inputs;
        copy->actor = impl_->actor;
        copy->stopped = impl_->stopped;
        copy->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(state));
        TradingSession trial(std::move(copy));
        Decision decision;
        try {
          decision = trial.submit(candidates[c][i], time, rejection).decision;
          for (auto j = static_cast<std::size_t>(id - 1); j < trial.impl_->state.orders.size(); ++j) {
            const auto& order = trial.impl_->state.orders[j];
            if (decision.ok() && order.status == OrderStatus::Rejected) decision = order.reason;
          }
        } catch (const TradingError& error) {
          decision = failure(error.code(), error.what());
        }
        if (!decision.ok() && candidate.decision.ok()) candidate.decision = decision;
        candidate.orders.push_back(decision);
        state = std::move(trial.impl_->state);
        continue;
      }
      PreviewProjection projection{{}, state};
      try {
        projection = project_order(state, candidates[c][i], rejection);
      } catch (const TradingError& error) {
        projection.result.decision = failure(error.code(), error.what());
      }
      if (impl_->stopped) projection.result.decision = failure(Reason::JOURNAL_IO, "Trading stopped after journal failure");
      // An order without a contract or a quote cannot be projected, so the account after it is unknown.
      const auto& orders = projection.projected.orders;
      projected &= orders.size() >= id && orders.at(static_cast<std::size_t>(id - 1)).status == OrderStatus::Filled;
      if (!projection.result.decision.ok() && candidate.decision.ok()) candidate.decision = projection.result.decision;
      candidate.orders.push_back(projection.result.decision);
      state = std::move(projection.projected);
    }
    if (projected) candidate.after = what_if_account(state, base, close_variances);
    candidate.after.projected = projected;
    result.candidates.push_back(std::move(candidate));
  }
  return result;
}
FlattenPreview TradingSession::preview_close_positions(std::optional<std::string> underlying, Timestamp time,
    const std::map<std::string, Decision>& rejections, const std::map<std::string, double>& close_variances,
    const PreviewMarket& market, FlattenPricing pricing) const {
  if (impl_->stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
  // The flatten itself, on a journal-less copy, so the dry run does what it would.
  auto copy = std::make_unique<Impl>();
  copy->state = prepared(impl_->state, time, market);
  copy->actor = impl_->actor;
  copy->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(copy->state));
  const State before = copy->state;
  TradingSession trial(std::move(copy));
  FlattenPreview result;
  const auto base = measure(before).equity;
  result.current = what_if_account(before, base, close_variances);
  const auto closed = trial.close_positions(underlying, time, rejections, pricing);
  const auto& after = trial.impl_->state;
  result.decision = closed.decision;
  result.kept_stocks = closed.kept_stocks;
  for (const auto id : open_ids(before))
    if (after.orders.at(static_cast<std::size_t>(id - 1)).status == OrderStatus::Cancelled) result.cancelled.push_back(id);
  for (auto i = before.orders.size(); i < after.orders.size(); ++i) result.orders.push_back(after.orders[i]);
  for (auto i = before.fills.size(); i < after.fills.size(); ++i) result.fills.push_back(after.fills[i]);
  for (auto i = before.stock_fills.size(); i < after.stock_fills.size(); ++i) result.stock_fills.push_back(after.stock_fills[i]);
  const auto in_scope = [&](const std::string& name) { return !underlying || name == *underlying; };
  for (const auto& [symbol, position] : after.ledger.positions())
    if (position.quantity != 0 && in_scope(after.contracts.at(symbol).underlying)) result.remaining[symbol] = position.quantity;
  for (const auto& [symbol, stock] : after.ledger.stocks())
    if (stock.shares != 0 && in_scope(symbol)) result.remaining_shares[symbol] = stock.shares;
  result.after = what_if_account(after, base, close_variances);
  return result;
}
CommandResult TradingSession::cancel(OrderId id, Timestamp time, Reason reason) {
  if (reason != Reason::USER_CANCEL && reason != Reason::PLAYBOOK_TIME_STOP && reason != Reason::PLAYBOOK_TRAILING_STOP &&
      reason != Reason::PLAYBOOK_DTE_STOP && reason != Reason::PLAYBOOK_DAYS_IN_TRADE_STOP)
    throw TradingError(Reason::INVALID_REASON, "Invalid cancellation reason");
  return impl_->transact(time, "cancel", [&](State& s, Events& events) {
    if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), {}, 0};
    if (!s.orders.at(static_cast<std::size_t>(id - 1)).open())
      return CommandResult{failure(Reason::ORDER_TERMINAL, "Order is already terminal"), id, 0};
    const auto& order = s.orders.at(static_cast<std::size_t>(id - 1));
    if (reason == Reason::USER_CANCEL && s.config.rules.require_stop_loss && order.role == OrderRole::StopLoss &&
        exit_capacity(s, order.request) > 0)
      return CommandResult{failure(Reason::STOP_REQUIRED, "Close the protected position before cancelling its required stop-loss"), id, 0};
    cancel_order(s, id, failure(reason, reason == Reason::PLAYBOOK_TIME_STOP ? "Playbook time stop" : reason == Reason::PLAYBOOK_TRAILING_STOP ? "Playbook trailing stop" :
        reason == Reason::PLAYBOOK_DTE_STOP ? "Playbook DTE stop" : reason == Reason::PLAYBOOK_DAYS_IN_TRADE_STOP ? "Playbook days in trade stop" : "Cancelled by caller"), events);
    return CommandResult{{}, id, 0};
  });
}
CommandResult TradingSession::modify(OrderId id, OrderChange change, Timestamp time, Decision rejection) {
  return impl_->transact(time, "modify", [&](State& s, Events& events) {
    monitor_loss(s, events);
    return change_order(s, id, change, rejection, events);
  });
}
CommandResult TradingSession::cancel_all(std::optional<std::string> underlying, Timestamp time) {
  return impl_->transact(time, "cancel_all", [&](State& s, Events& events) {
    if (s.config.rules.require_stop_loss)
      for (const auto id : open_ids(s)) {
        const auto& o = s.orders[id - 1];
        if (o.role == OrderRole::StopLoss && (!underlying || underlying_of(s, o) == *underlying) && exit_capacity(s, o.request) > 0)
          return CommandResult{failure(Reason::STOP_REQUIRED, "Close protected positions before cancelling their required stops"), id, 0};
      }
    if (s.config.rules.require_stop_loss)
      for (const auto id : open_stock_ids(s))
        if (const auto& o = s.stock_orders[id - 1];
            o.role == OrderRole::StopLoss && (!underlying || o.request.symbol == *underlying) && shares_held(s, o.request.symbol) != 0)
          return CommandResult{failure(Reason::STOP_REQUIRED, "Close protected shares before cancelling their required stops"), {}, 0};
    for (const auto id : open_ids(s))
      if (const auto& o = s.orders[id - 1]; o.open() && (!underlying || underlying_of(s, o) == *underlying))
        cancel_order(s, id, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    for (const auto id : open_stock_ids(s))
      if (const auto& o = s.stock_orders[id - 1]; o.open() && (!underlying || o.request.symbol == *underlying))
        end_stock_order(s, o.id, OrderStatus::Cancelled, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{};
  });
}
namespace session_internal {
namespace {
/// Accept or reject one of a flatten's reduce-only closes. The integration's gate,
/// the account and the session can refuse it whatever the price; missing or stale
/// quotes only make it wait. Accepted, it trades at once where it can and works
/// the rest on later quotes until its session ends.
Decision place_close(State& s, OrderRequest request, const Decision& rejection, FlattenPricing pricing, Events& events) {
  Order order;
  order.id = static_cast<OrderId>(s.orders.size() + 1);
  order.request = std::move(request);
  order.actor = s.actor;
  order.accepted_at = s.time;
  order.reduce_only = true;
  if (pricing.limit) {
    order.limit_ticks = pricing.limit_ticks;
    order.request.limit_price = close_price(s, order).value_or(Money{});
  }
  add_order(s, order);
  // Written until matching starts; nothing copies the orders before that.
  auto& stored = s.orders.mut_back();
  auto decision = rejection.ok() ? account_check(s, true) : rejection;
  for (const auto& symbol : order_symbols(stored.request)) {
    if (!decision.ok()) break;
    const auto& contract = s.contracts.at(symbol);
    decision = s.time >= contract.expiry_time() ? failure(Reason::EXPIRED, "Contract has expired")
        : pricing.limit && !md::trading_session(contract.root, s.time).open
            ? failure(Reason::SESSION_CLOSED, "Flatten requires an open trading session")
            : session_check(contract, s.time, stored.request);
    if (!decision.ok() && multi_leg(stored.request)) decision.scope = symbol;
  }
  if (!decision.ok()) {
    stored.status = OrderStatus::Rejected;
    stored.reason = decision;
    event(events, "order_rejected", stored);
    return decision;
  }
  const auto id = stored.id;
  stored.day_end = order_end(s, stored.request, s.time);
  event(events, "order_accepted", stored);
  const auto symbols = order_symbols(stored.request);
  match_symbols(s, {symbols.begin(), symbols.end()}, events, id);
  return {};
}
}  // namespace
}  // namespace session_internal
CommandResult TradingSession::cancel_orders(const std::vector<OrderId>& ids, Timestamp time) {
  return impl_->transact(time, "cancel_orders", [&](State& s, Events& events) {
    if (ids.empty()) return CommandResult{failure(Reason::INVALID_ORDER, "Name at least one order to cancel"), {}, 0};
    for (const auto id : ids)
      if (id == 0 || id > s.orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "Unknown order ID"), id, 0};
    if (std::none_of(ids.begin(), ids.end(), [&](OrderId id) { return s.orders.at(static_cast<std::size_t>(id - 1)).open(); }))
      return CommandResult{failure(Reason::ORDER_TERMINAL, "Every listed order is already terminal"), ids.front(), 0};
    if (s.config.rules.require_stop_loss)
      for (const auto id : ids) {
        const auto& o = s.orders[id - 1];
        if (o.open() && o.role == OrderRole::StopLoss && exit_capacity(s, o.request) > 0)
          return CommandResult{failure(Reason::STOP_REQUIRED, "Close protected positions before cancelling their required stops"), id, 0};
      }
    for (const auto id : ids) cancel_order(s, id, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::close_positions(std::optional<std::string> underlying, Timestamp time,
                                              const std::map<std::string, Decision>& rejections, FlattenPricing pricing) {
  if (pricing.limit_ticks < 0 || pricing.limit_ticks > 10 || (!pricing.limit && pricing.limit_ticks != 0))
    throw TradingError(Reason::INVALID_ORDER, "limit_ticks needs a limit flatten and must be 0 to 10");
  return impl_->transact(time, "close_positions", [&](State& s, Events& events) {
    monitor_loss(s, events);
    const auto in_scope = [&](const std::string& name) { return !underlying || name == *underlying; };
    // Before anything is cancelled, what in scope could close: the account, the
    // integration's gate on the underlying and the session a market order (or a
    // share trade) needs can each refuse a close whatever the price. By
    // underlying, whether anything in it can close.
    std::map<std::string, bool> closable;
    Decision refusal;
    const auto gate = [&](const std::string& name, Decision decision) {
      closable[name] |= decision.ok();
      if (!decision.ok() && refusal.ok()) {
        refusal = std::move(decision);
        if (refusal.scope.empty()) refusal.scope = name;
      }
    };
    OrderRequest market;
    market.type = pricing.limit ? OrderType::Limit : OrderType::Market;
    market.tif = pricing.limit ? TimeInForce::Exto : TimeInForce::Day;
    // Every position in scope, expired ones included: they cannot trade and close
    // at settlement, but an expired short keeps the long that covers it until then.
    std::vector<std::string> positions;
    for (const auto& [symbol, position] : s.ledger.positions()) {
      const auto& contract = s.contracts.at(symbol);
      if (position.quantity == 0 || !in_scope(contract.underlying)) continue;
      positions.push_back(symbol);
      if (s.time >= contract.expiry_time()) continue;
      const auto rejection = rejections.find(contract.underlying);
      auto decision = account_check(s, true);
      if (decision.ok() && rejection != rejections.end()) decision = rejection->second;
      if (decision.ok()) decision = pricing.limit && !md::trading_session(contract.root, s.time).open
          ? failure(Reason::SESSION_CLOSED, "Flatten requires an open trading session") : session_check(contract, s.time, market);
      gate(contract.underlying, std::move(decision));
    }
    // Shares close at the underlying's fresh price in the regular session.
    std::vector<std::pair<std::string, Quantity>> stocks;
    CommandResult result;
    for (const auto& [symbol, stock] : s.ledger.stocks()) {
      if (stock.shares == 0 || !in_scope(symbol)) continue;
      const auto rejection = rejections.find(symbol);
      auto decision = account_check(s, true);
      if (decision.ok() && rejection != rejections.end()) decision = rejection->second;
      if (decision.ok()) decision = share_close_check(s, symbol);
      if (decision.ok()) stocks.emplace_back(symbol, stock.shares);
      else result.kept_stocks.emplace(symbol, decision);
      gate(symbol, std::move(decision));
    }
    // Nothing in scope can close: refused, with nothing cancelled or recorded.
    if (!closable.empty() && std::none_of(closable.begin(), closable.end(), [](const auto& c) { return c.second; }))
      return CommandResult{refusal, {}, 0};
    // The open orders in scope are cancelled, but the bracket exits, which keep
    // protecting whatever stays open until it is flat, and the account's own
    // closes. An underlying where nothing can close keeps all its orders.
    for (const auto id : open_ids(s))
      if (const auto& o = s.orders[id - 1]; o.open() && !o.system && o.role == OrderRole::Normal && in_scope(underlying_of(s, o))) {
        const auto it = closable.find(underlying_of(s, o));
        if (it == closable.end() || it->second)
          cancel_order(s, id, failure(Reason::USER_CANCEL, "Cancelled to close positions"), events);
      }
    std::map<std::string, Quantity> held_back;
    const auto plan = plan_closes(s, positions, held_back);
    // A client ID the trader already used stays theirs: take the next free number.
    const auto prefix = "openport-close-" + std::to_string(s.version + 1) + "-";
    std::size_t count = 0;
    const auto client_id = [&] {
      auto id = prefix + std::to_string(++count);
      while (s.clients.contains(id)) id = prefix + std::to_string(++count);
      return id;
    };
    // Each close is split at the order size limit. A refused close is still
    // recorded, rejected with its reason.
    std::map<std::string, Decision> refused;
    const auto limit = s.config.limits.max_order_contracts;
    for (const auto& close : plan) {
      const auto rejection = rejections.find(s.contracts.at(close.legs.front().symbol).underlying);
      for (Quantity placed = 0; placed < close.units;) {
        OrderRequest request;
        request.client_order_id = client_id();
        if (close.legs.size() == 1) {
          request.symbol = close.legs.front().symbol;
          request.side = close.legs.front().side;
        } else {
          request.legs = close.legs;
        }
        request.type = market.type;
        request.tif = market.tif;
        request.quantity = std::min(close.units - placed, limit);
        placed += request.quantity;
        const auto decision = place_close(s, std::move(request), rejection == rejections.end() ? Decision{} : rejection->second, pricing, events);
        if (!decision.ok())
          for (const auto& leg : close.legs) refused.emplace(leg.symbol, decision);
      }
    }
    for (const auto& [symbol, shares] : stocks) close_shares(s, symbol, shares, StockSource::Trade, events);
    // What is left open: the contracts still being worked, and why the rest are not.
    for (const auto& symbol : positions) {
      const auto quantity = held(s, symbol);
      if (quantity == 0) continue;
      Quantity working = 0;
      for (const auto id : open_ids(s)) {
        const auto& o = s.orders[id - 1];
        if (!o.reduce_only) continue;
        if (!multi_leg(o.request)) { if (o.request.symbol == symbol) working += o.remaining(); continue; }
        for (const auto& leg : o.request.legs) if (leg.symbol == symbol) working += o.remaining() * leg.ratio;
      }
      Residual residual{symbol, quantity, std::min(working, magnitude(quantity)), {}};
      if (residual.working < magnitude(quantity)) {
        if (s.time >= s.contracts.at(symbol).expiry_time())
          residual.reason = failure(Reason::AWAITING_SETTLEMENT, "The contract has expired; it closes at its settlement");
        else if (held_back.contains(symbol))
          residual.reason = failure(Reason::AWAITING_SETTLEMENT, "It covers a short that has expired; it stays until that short settles");
        else if (const auto it = refused.find(symbol); it != refused.end())
          residual.reason = it->second;
      }
      result.residuals.push_back(std::move(residual));
    }
    return result;
  });
}
CommandResult TradingSession::on_quotes(const std::vector<QuoteObservation>& quotes,
    const std::vector<Valuation>& valuations, Timestamp time, const std::vector<StockPrice>& stocks,
    const std::vector<Indicator>& indicators, const MarketActivity& activity) {
  // An empty batch on an idle account only moves the clock: not a transaction,
  // so nothing is journaled. The next transaction advances the clock itself.
  if (quotes.empty() && valuations.empty() && stocks.empty() && indicators.empty() && impl_->idle(time))
    return CommandResult{{}, {}, impl_->state.version};
  return impl_->transact(time, "market", [&](State& s, Events& events) {
    std::set<std::string> seen;
    std::set<std::string> changed;
    std::set<std::string> offered_again;
    for (auto quote : quotes) {
      if (!s.contracts.contains(quote.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Quote references unregistered OSI");
      if (quote.time < 0 || quote.time > time) throw TradingError(Reason::INVALID_TIME, "Quote is future-dated or negative");
      if (quote.quoted < 0 || quote.quoted > quote.time) throw TradingError(Reason::INVALID_TIME, "Quote was first given after its time");
      if (quote.quoted == quote.time) quote.quoted = 0;
      if (!seen.insert(quote.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One observation per contract per batch is required");
      if (retired_observation(s.contracts.at(quote.symbol), quote.time)) continue;
      // From a split's rollover to its ex-date's open, a feed still quotes the series as
      // they were: such a quote neither marks nor fills an adjusted series (driver 9).
      if (before_split(s, s.contracts.at(quote.symbol).underlying, quote.first_time())) continue;
      auto& book = s.books[quote.symbol];
      if (quote.observation == book.quote.observation && quote.time >= book.quote.time) {
        // The same quote, confirmed current at a later time: it and its mark stay
        // fresh, and it keeps what is left of its displayed size and when it was
        // first given.
        if (quote.time > book.quote.time) {
          const bool keep_mark = keeps_mark(s, book.quote, book.quote, quote.time);
          book.quote.quoted = book.quote.first_time();
          book.quote.time = quote.time;
          if (markable_quote(book.quote) && !keep_mark) s.marks[quote.symbol] = {mark_of(book.quote), quote.time};
        }
        offered_again.insert(quote.symbol);
        continue;
      }
      if (quote.observation <= book.quote.observation || quote.time < book.quote.time) continue;
      const bool keep_mark = keeps_mark(s, book.quote, quote, quote.time);
      // A quote with only an ask offers it to the exits that may buy there (ask_only).
      auto measured = s.config.rules.stop_stress_ticks > 0 ? std::optional(next_activity(s, book, quote)) : std::nullopt;
      book = {quote, valid_quote(quote) ? quote.bid_size : 0, markable_quote(quote) ? quote.ask_size : 0, std::move(measured)};
      changed.insert(quote.symbol);
      if (markable_quote(quote) && time - quote.time <= s.config.limits.max_quote_age && !keep_mark)
        s.marks[quote.symbol] = {mark_of(quote), quote.time};
    }
    seen.clear();
    for (auto valuation : valuations) {
      if (!s.contracts.contains(valuation.symbol)) throw TradingError(Reason::UNKNOWN_CONTRACT, "Valuation references unregistered OSI");
      if (valuation.time < 0 || valuation.time > time) throw TradingError(Reason::INVALID_TIME, "Valuation is future-dated or negative");
      if (!seen.insert(valuation.symbol).second) throw TradingError(Reason::MISSING_VALUATION, "One valuation per contract per batch is required");
      if (retired_observation(s.contracts.at(valuation.symbol), valuation.time)) continue;
      const auto prior = s.valuations.find(valuation.symbol);
      if (prior != s.valuations.end() && valuation.time < prior->second.time) continue;
      if (!valid_valuation(valuation)) {
        // Null/nonfinite data is explicitly invalid, never implicitly zero risk.
        valuation = Valuation{valuation.symbol, valuation.time, 0, 0, 0, 0, 0, 0, 0, 0, 0, false};
      }
      s.valuations[valuation.symbol] = valuation;
    }
    seen.clear();
    for (const auto& price : stocks) {
      if (price.symbol.empty() || price.price <= Money{}) throw TradingError(Reason::INVALID_QUOTE, "A stock price needs a symbol and a positive price");
      if (price.time < 0 || price.time > time) throw TradingError(Reason::INVALID_TIME, "Stock price is future-dated or negative");
      if (!seen.insert(price.symbol).second) throw TradingError(Reason::INVALID_QUOTE, "One price per underlying per batch is required");
      const auto prior = s.stock_marks.find(price.symbol);
      if (prior != s.stock_marks.end() && price.time < prior->second.time) continue;
      // A simulated halt prints nothing: the mark stays the last pre-halt print.
      if (price.time < halt_end(s, price.symbol)) continue;
      // A price printed before a split went ex is on the shares before it (driver 9).
      s.stock_marks[price.symbol] = {split_basis(s, price.symbol, price.time, price.price), price.time};
    }
    seen.clear();
    for (const auto& indicator : indicators) {
      if (indicator.symbol.empty()) throw TradingError(Reason::INVALID_QUOTE, "An indicator needs a symbol");
      if (indicator.time < 0 || indicator.time > time) throw TradingError(Reason::INVALID_TIME, "Indicator is future-dated or negative");
      const auto key = indicator_key(indicator.symbol, indicator.study);
      if (!seen.insert(key).second) throw TradingError(Reason::INVALID_QUOTE, "One value per indicator per batch is required");
      // A split stock's price before its ex-date opens is on the old shares, while the
      // levels watching it have taken the split (driver 9): it waits for the open, as the
      // stock's series' quotes do. Studies of implied volatility do not split.
      if (indicator.study.empty() && before_split(s, indicator.symbol, indicator.time)) continue;
      const auto prior = s.indicators.find(key);
      if (prior != s.indicators.end() && indicator.time <= prior->second.time) continue;
      s.indicators[key] = {indicator.value, indicator.time};
    }
    detail::update_reviews(s);
    monitor_calendar(s, events);
    monitor_loss(s, events);
    monitor_rules(s, events);
    // Invalid quotes provide no liquidity. Keep orders until a new valid quote
    // permits the fill-time risk check (or a clock/kill command cancels them). A
    // fresh quote with only an ask still serves the closes that may take it.
    const auto usable = [&](const std::string& symbol) { return quote_check(s, symbol).ok() || asks_only(s, symbol); };
    for (auto it = changed.begin(); it != changed.end();) {
      if (!usable(*it)) it = changed.erase(it); else ++it;
    }
    for (const auto& symbol : s.walked)
      if (usable(symbol)) changed.insert(symbol);
    s.walked.clear();
    // Offered again, a quote's remaining displayed size can fill the orders that
    // a data gap held back when it was new.
    for (const auto id : open_ids(s)) {
      const auto& o = s.orders[id - 1];
      if (o.status == OrderStatus::Armed) continue;
      for (const auto& symbol : order_symbols(o.request))
        if (offered_again.contains(symbol) && usable(symbol)) changed.insert(symbol);
    }
    // The queue model reads what traded, and the quotes now in place.
    if (s.config.rules.queue_fills) {
      for (const auto& print : activity.prints)
        if (print.time > time) throw TradingError(Reason::INVALID_TIME, "Trade print is future-dated");
      std::set<std::string> earned;
      update_queues(s, activity, earned);
      for (const auto& symbol : earned) if (usable(symbol)) changed.insert(symbol);
    }
    match_symbols(s, changed, events);
    // Triggers read the batch's books and valuations after resting orders match.
    check_triggers(s, events);
    // Share orders work on the underlyings' prices.
    work_stock_orders(s, events);
    check_alerts(s, events);
    return CommandResult{};
  });
}
CommandResult TradingSession::set_limits(Limits limits, Timestamp time) {
  validate_limits(limits);
  return impl_->transact(time, "limit_change", [&](State& s, Events& events) {
    auto effective = s.config.rules.evaluation() ? tightened_limits(s.config.limits, limits) : limits;
    // A removed override needs no queue when its tightened cap is the common cap.
    // No driver gate is needed: older runs cannot contain underlying overrides,
    // so removing redundant entries changes none of their journal bytes.
    std::erase_if(effective.underlying_overrides, [&](const auto& entry) {
      return !limits.underlying_overrides.contains(entry.first) &&
          entry.second == effective.per_underlying;
    });
    s.pending_limits = Json(effective) != Json(limits) ? std::optional(limits) : std::nullopt;
    s.config.limits = effective;
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "limit_change", Json{{"effective", s.config.limits}, {"pending", s.pending_limits}});
    monitor_loss(s, events);
    for (const auto id : open_ids(s)) {
      const auto& order = s.orders[id - 1];
      if (!order.open()) continue;
      // A triggered stop-limit keeps its limit beyond the band, as it may after a gap.
      auto d = order.system || kept_within(order) ? system_check(s, order)
          : order_check(s, order, order.triggered_at > 0 ? Stage::Activate : Stage::Accept);
      if ((persistent(order) || order.reduce_only) &&
          (data_gap(d.code) || d.code == Reason::SESSION_CLOSED || d.code == Reason::LIMIT_ONLY)) continue;
      if (!d.ok()) {
        if (d.code != Reason::SCALING_LIMIT) {
          d.message = std::string(to_string(d.code)) + ": " + d.message;
          d.code = Reason::RISK_CHANGED;
        }
        cancel_order(s, id, d, events);
      }
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::set_guardrails(Guardrails guardrails, Timestamp time) {
  validate_guardrails(guardrails);
  return impl_->transact(time, "guardrail_change", [&](State& s, Events& events) {
    if ((!s.evaluation.revised_rules || guardrails.soft_floor != s.config.guardrails.soft_floor) &&
        guardrails.soft_floor > Money{} && s.config.rules.max_drawdown > Money{} && guardrails.soft_floor <= s.evaluation.floor)
      throw TradingError(Reason::INVALID_LIMITS, "Soft floor equity must be above the plan floor");
    const auto effective = tightened_guardrails(s.config.guardrails, guardrails);
    if (s.guardrails.cooldown_until > s.time && effective.cooldown_minutes > s.config.guardrails.cooldown_minutes) {
      const auto extension = (effective.cooldown_minutes - s.config.guardrails.cooldown_minutes) * md::kNanosPerMinute;
      if (s.guardrails.cooldown_until > std::numeric_limits<Timestamp>::max() - extension)
        throw TradingError(Reason::INVALID_TIME, "Cooldown time exceeds timestamp range");
      s.guardrails.cooldown_until += extension;
    }
    s.pending_guardrails = effective != guardrails ? std::optional(guardrails) : std::nullopt;
    s.config.guardrails = effective;
    if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
    ++s.limits_revision;
    event(events, "guardrail_change", Json{{"effective", effective}, {"pending", s.pending_guardrails}});
    return CommandResult{};
  });
}
CommandResult TradingSession::trip_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_trip", [&](State& s, Events& events) {
    // A manual trip over a daily-loss or earlier manual latch replaces its reason;
    // its event and the history name the reason it replaced.
    const bool replaces = s.kill && !s.guardrails.owns_kill && s.kill_reason != reason;
    const auto previous = s.kill_reason;
    trip(s, reason, events);
    if (replaces) {
      event(events, "kill_trip", Json{{"reason", reason}, {"previous", previous}});
      record_kill(s, "trip", reason, previous);
    }
    s.kill_reason = reason;
    s.guardrails.owns_kill = false;
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_kill(std::string reason, Timestamp time) {
  require_reason(reason);
  return impl_->transact(time, "kill_reset", [&](State& s, Events& events) {
    if (guardrail_reason(s) != Reason::NONE) return CommandResult{reset_check(s, measure(s)), {}, 0};
    if (s.kill) record_kill(s, "reset", reason, s.kill_reason);
    s.kill = false;
    s.kill_reason.clear();
    event(events, "kill_reset", Json{{"reason", reason}});
    monitor_loss(s, events);
    if (s.kill) return CommandResult{reset_check(s, measure(s)), {}, 0};
    return CommandResult{};
  });
}
namespace session_internal {
namespace {
/// When a settlement's reference printed, never after the settlement itself: the stock
/// market's close that the expiry falls at or just after (a quarter-hour ETF series
/// still settles on the 16:00 close), or the expiry itself for an AM series, whose
/// reference is the opening print.
Timestamp settlement_print_time(const md::OptionContract& contract, Timestamp time) {
  const auto expiry = contract.expiry_time();
  const auto session = md::stock_session(expiry);
  const auto printed = session.open || session.market_time == md::kInvalidTimestamp ? expiry : session.market_time;
  return std::min(printed, time);
}
Quantity pin_assignments(const State& s, const md::OptionContract& contract, Quantity contracts, bool in_money) {
  auto seed = mix(static_cast<std::uint64_t>(s.evaluation.started) ^
                  mix(static_cast<std::uint64_t>(s.config.rules.pin_risk_seed)));
  for (const unsigned char c : "pin " + contract.osi_symbol() + " " + md::format_date(contract.expiry)) seed = mix(seed ^ c);
  Quantity assigned = 0;
  for (Quantity i = 0; i < contracts; ++i) {
    const bool contrary = ((seed = mix(seed)) >> 62) == 0;
    assigned += static_cast<Quantity>(in_money != contrary);
  }
  return assigned;
}
}  // namespace
}  // namespace session_internal
CommandResult TradingSession::settle(const std::string& symbol, Money settlement, Timestamp time,
                                     std::optional<SettlementSource> source) {
  return impl_->transact(time, "settlement", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Unknown settlement contract"), {}, 0};
    if (s.settled.contains(symbol)) return CommandResult{failure(Reason::ALREADY_SETTLED, "Settlement is exactly once per OSI"), {}, 0};
    if (time < it->second.expiry_time() || settlement < Money{} || !s.ledger.positions().contains(symbol))
      return CommandResult{failure(Reason::INVALID_SETTLEMENT, "Settlement requires expiry, a position and nonnegative reference"), {}, 0};
    const Money strike = effective_strike(s, it->second);
    const auto quantity = held(s, symbol);
    // A long instructed not to be exercised expires worthless.
    const bool unexercised = quantity > 0 && s.do_not_exercise.contains(symbol);
    const Money exercise_value = unit_exercise_value(s, it->second, settlement);
    const bool pin = s.config.rules.pin_risk && quantity < 0 && physical(it->second) && deliverable_of(s, symbol).shares != 0 &&
        exercise_value >= Money::parse("-0.10") && exercise_value <= Money::parse("0.10");
    const auto assigned = pin ? -pin_assignments(s, it->second, -quantity, exercise_value >= Money::from_micros(10'000)) : quantity;
    // Pin assignments close the option at zero and deliver at the strike. This
    // also accounts correctly for contrary exercise of an out-of-money option.
    const Money intrinsic = unexercised || pin ? Money{} : std::max(Money{}, exercise_value);
    // The last stretch ends at intrinsic value, at expiry with the settlement
    // as the underlying's price and the volatility unchanged.
    std::optional<Valuation> at_expiry;
    if (const auto r = s.references.find(symbol); r != s.references.end() && r->second.valuation) {
      at_expiry = *r->second.valuation;
      at_expiry->spot = settlement.dollars();
      at_expiry->years = 0;
    }
    end_stretch(s, symbol, intrinsic, at_expiry ? &*at_expiry : nullptr);
    // A deliverable of cash only settles in its intrinsic value, delivering nothing.
    const bool delivers = pin ? assigned != 0
                              : physical(it->second) && deliverable_of(s, symbol).shares != 0 && intrinsic >= Money::from_micros(10'000);
    const Money fee = delivers ? exercise_fee(s, assigned) : Money{};
    s.settlements.push_back(settlement_record(s.ledger.positions().at(symbol), settlement, intrinsic, fee, time, source));
    s.ledger.settle(symbol, intrinsic, fee);
    if (fee > Money{}) s.explained[symbol].costs -= fee.dollars();
    s.trips.erase(symbol);
    s.settled.insert(symbol);
    s.settling[symbol] = settlement;
    s.closures.push_back({symbol, quantity, intrinsic, time, ClosureKind::Settlement, s.fills.size(), fee});
    if (unexercised) s.do_not_exercise.erase(symbol);
    sync_exits(s, symbol, events);
    Json settled{{"symbol", symbol}, {"reference", settlement}, {"intrinsic", intrinsic}};
    if (unexercised) settled["do_not_exercise"] = true;
    if (pin) {
      settled["pin_risk"] = true;
      settled["assigned_contracts"] = -assigned;
      settled["simulated"] = true;
      settled["seed"] = s.config.rules.pin_risk_seed;
    }
    event(events, "settlement", std::move(settled));
    // American equity and ETF options a cent or more in the money are exercised
    // or assigned: settled at intrinsic value, they deliver shares at the
    // settlement price, which together cost the strike.
    if (delivers) {
      const auto underlying = it->second.underlying;
      const auto shares = delivered(s, it->second, assigned);
      const auto delivery_price = pin ? strike : settlement;
      // The settlement is the stock's price when it printed: the close the expiry
      // falls in, or an AM series' opening print. It marks the shares only over an
      // older price, stamped then, so a newer print a late settlement finds stays
      // (driver 11; replay drivers 1-10 stamped it with the settlement's own time).
      const auto printed = s.revised_lifecycle ? settlement_print_time(it->second, time) : time;
      if (const auto mark = s.stock_marks.find(underlying); mark == s.stock_marks.end() || mark->second.time <= printed)
        s.stock_marks[underlying] = {settlement, printed};
      trade_shares(s, underlying, shares, delivery_price, StockSource::Delivery, symbol);
      // An adjusted series' effective strike is rounded to the micro-dollar, so a pin
      // assignment's shares at it can miss the strike amount by a fraction of a cent:
      // the cash takes the exact amount, 100 times the strike less the cash in lieu a
      // contract (driver 11; replay drivers 1-10 kept the rounded amount).
      if (pin && s.revised_lifecycle) {
        const Money amount = (Money::from_double(it->second.strike) * 100 - deliverable_of(s, symbol).cash) * magnitude(assigned);
        const Money exact = shares > 0 ? -amount : amount;
        const Money booked = -(delivery_price * shares);
        if (exact != booked) s.ledger.accrue(exact - booked);
      }
      if (s.config.rules.buying_power)
        s.stock_fills.mut(s.stock_fills.size() - 1).buying_power_after = buying_power(s).total.available;
      event(events, "delivery", Json{{"symbol", symbol}, {"underlying", underlying}, {"shares", shares}, {"price", delivery_price}});
    }
    return CommandResult{};
  }, source);
}
namespace session_internal {
/// Overnight, short American equity and ETF options that the market valued below
/// their exercise value at the close are assigned: a holder then does better
/// exercising than selling, as with a deep put or a call before its dividend. Each
/// assigned contract is bought back at intrinsic value and delivers shares at the
/// underlying's close, together the strike. Options that expire today settle instead.
/// splitmix64, so a replay draws the same assignments on every platform.
std::uint64_t mix(std::uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
namespace {
/// How many of `contracts` short contracts holders' exercises reach: each with even
/// odds, as the OCC allocates exercises at random, drawn from the attempt, the
/// contract and the date so a replay assigns the same ones.
Quantity assigned_contracts(const State& s, const std::string& symbol, Quantity contracts) {
  auto seed = mix(static_cast<std::uint64_t>(s.evaluation.started));
  for (const unsigned char c : symbol + " " + md::format_date(s.day)) seed = mix(seed ^ c);
  Quantity assigned = 0;
  for (Quantity i = 0; i < contracts; ++i) assigned += static_cast<Quantity>((seed = mix(seed)) >> 63);
  return assigned;
}
/// `closing` is when the finished day's marks were last published: the marks and
/// the underlying's price decide as they stood then.
void assign_early(State& s, const std::vector<Dividend>& dividends, const std::vector<CorporateAction>& splits,
                  Timestamp closing, Events& events) {
  std::vector<std::string> shorts;
  for (const auto& [symbol, position] : s.ledger.positions()) {
    const auto& c = s.contracts.at(symbol);
    if (position.quantity < 0 && physical(c) && c.expiry > s.day) shorts.push_back(symbol);
  }
  for (const auto& symbol : shorts) {
    const auto contract = s.contracts.at(symbol);
    const auto mark = s.marks.find(symbol);
    const auto price = stock_price(s, contract.underlying, closing);
    if (mark == s.marks.end() || !price) continue;
    const bool call = contract.type == pricing::OptionType::Call;
    const Money intrinsic = std::max(Money{}, unit_exercise_value(s, contract, *price));
    if (intrinsic < Money::from_micros(10'000)) continue;
    // Holders exercise an option trading below its exercise value, and a call
    // whose time value is less than what a dividend going ex on the new day pays on
    // what it delivers.
    Money dividend;
    for (const auto& d : dividends)
      if (call && d.symbol == contract.underlying) dividend = dividend + unit_dividend(s, contract, d, splits);
    const char* reason = mark->second.price < intrinsic ? "below_intrinsic"
                       : mark->second.price - intrinsic < dividend ? "dividend" : nullptr;
    if (!reason) continue;
    const auto short_contracts = -held(s, symbol);
    const auto contracts = assigned_contracts(s, symbol, short_contracts);
    if (contracts == 0) continue;
    const auto shares = delivered(s, contract, -contracts);
    const auto fee = exercise_fee(s, contracts);
    fill_position(s, symbol, contracts, intrinsic, fee);
    s.closures.push_back({symbol, -contracts, intrinsic, s.time, ClosureKind::Assignment, s.fills.size(), fee});
    if (shares != 0) trade_shares(s, contract.underlying, shares, *price, StockSource::Assignment, symbol);
    sync_exits(s, symbol, events);
    if (shares != 0 && s.config.rules.buying_power)
      s.stock_fills.mut(s.stock_fills.size() - 1).buying_power_after = buying_power(s).total.available;
    event(events, "assignment", Json{{"symbol", symbol}, {"contracts", contracts}, {"of", short_contracts}, {"reason", reason},
                                     {"intrinsic", intrinsic}, {"mark", mark->second.price},
                                     {"underlying", contract.underlying}, {"shares", shares}, {"price", *price}});
  }
}
/// Actual/365 simple accrual on the last closing balance and marks, including
/// skipped calendar days. Each posting rounds once to cents; no fabricated prices.
void accrue_financing(State& s, const State& closing, std::int64_t days, Events& events) {
  const auto& rules = s.config.rules;
  const auto cash = closing.ledger.account().cash;
  const auto post = [&](std::string kind, std::string symbol, Money basis, std::int64_t rate, bool charge) {
    if (rate == 0 || basis == Money{}) return;
    auto amount = nearest_cent(basis.prorate(rate * days, 365 * 10000));
    if (charge) amount = -amount;
    if (amount == Money{}) return;
    s.ledger.accrue(amount);
    Attribution part; part.other = amount.dollars();
    explain_part(s, symbol.empty() ? "cash" : symbol, part);
    s.cash_flows.push_back({s.cash_flows.size() + 1, s.evaluation.attempt, s.time, s.day, std::move(kind),
                            std::move(symbol), amount, basis, rate, days, {}});
    event(events, "cash_flow", s.cash_flows.back());
  };
  if (cash > Money{}) post("credit_interest", {}, cash, rules.cash_credit_rate_bps, false);
  else if (cash < Money{} && rules.margin == MarginMode::Portfolio && rules.account_type == AccountType::Margin)
    post("debit_interest", {}, -cash, rules.margin_debit_rate_bps, true);
  for (const auto& [symbol, stock] : closing.ledger.stocks()) {
    if (stock.shares >= 0) continue;
    const auto mark = closing.stock_marks.find(symbol);
    if (mark != closing.stock_marks.end()) post("borrow_fee", symbol, mark->second.price * -stock.shares, rules.stock_borrow_rate_bps, true);
  }
}
/// A level on a stock's price set before a split, on the split shares: over the
/// split's ratio, to the nearest cent and never below one.
Money split_level(Money level, const CorporateAction& a) {
  return std::max(Money::from_micros(10'000), nearest_cent(level.prorate(a.denominator, a.numerator)));
}
/// Puts an option order's terms on a split stock's shares: each trigger on its
/// price, the order's own, its bracket's and its chained orders', takes the split, and
/// so does a trail of dollars behind it. Such a trigger watches the stock as its own
/// underlying or by name. Whether any moved.
bool split_request(const State& s, OrderRequest& request, const CorporateAction& a) {
  const auto symbols = order_symbols(request);
  const auto contract = symbols.empty() ? s.contracts.end() : s.contracts.find(symbols.front());
  const auto own = contract == s.contracts.end() ? std::string{} : contract->second.underlying;
  bool moved = false;
  const auto rescale = [&](std::optional<Trigger>& t) {
    if (!t || t->source != TriggerSource::Underlying || (t->symbol.empty() ? own : t->symbol) != a.symbol) return;
    t->level = split_level(t->level, a);
    if (t->trail && t->trail->unit == TrailUnit::Amount) t->trail->value = split_level(t->trail->value, a);
    moved = true;
  };
  rescale(request.trigger);
  if (request.bracket && request.bracket->stop_loss) rescale(request.bracket->stop_loss->trigger);
  if (request.bracket && request.bracket->take_profit) rescale(request.bracket->take_profit->trigger);
  for (auto& next : request.then)
    if (split_request(s, next, a)) moved = true;
  for (auto& other : request.oco)
    if (split_request(s, other, a)) moved = true;
  return moved;
}
/// On an ex-date the shares held into it are paid the dividend, and short shares
/// pay it, in whole cents: once per symbol and date, into the new day's P&L as other.
/// Splits going ex on the new trading date (and any the server was down for), each
/// once: held shares split, the fraction of a share left over paid in cash at the
/// split mark (or the average cost without one) and the mark split with them; every
/// share-settled series on the underlying that has not expired takes the split
/// deliverable, whole shares with the fraction as cash at that price (cash only once
/// a contract delivers less than a whole share, as after a 1:200 reverse split); working
/// share orders on the stock are cancelled, their prices no longer applying; and option
/// orders keep their premium, which is per contract either way, while their levels on
/// the stock's price take the split (driver 9; drivers 1-8 left them on the old shares).
void apply_corporate_actions(State& s, const std::vector<CorporateAction>& actions, Events& events) {
  for (const auto& a : actions) {
    if (std::any_of(s.corporate_actions.begin(), s.corporate_actions.end(),
                    [&](const CorporateActionRecord& r) { return r.symbol == a.symbol && r.ex_date == a.ex_date; }))
      continue;
    if (md::is_index_underlying(a.symbol)) continue;
    CorporateActionRecord record{a.symbol, a.ex_date, a.numerator, a.denominator};
    record.time = s.time;
    // The price a split share is worth: the mark, split; without one, the average cost.
    const auto mark = s.stock_marks.find(a.symbol);
    const auto shares = shares_held(s, a.symbol);
    std::optional<Money> split_price;
    if (mark != s.stock_marks.end()) split_price = mark->second.price.prorate(a.denominator, a.numerator);
    record.shares_before = shares;
    record.shares_after = shares;
    if (shares != 0) {
      const auto& stock = s.ledger.stocks().at(a.symbol);
      const auto price = split_price ? *split_price
          : (stock.basis < Money{} ? -stock.basis : stock.basis).prorate(a.denominator, magnitude(shares) * a.numerator);
      __extension__ using Wide = __int128;
      const Wide scaled = static_cast<Wide>(shares) * a.numerator;
      const auto whole = static_cast<Quantity>(scaled / a.denominator);
      const auto fraction = static_cast<std::int64_t>(scaled - static_cast<Wide>(whole) * a.denominator);
      const Money cash = nearest_cent(price.prorate(fraction, a.denominator));
      // The stretch before the split ends on the old mark; the next starts on the split one.
      if (const auto it = s.references.find(a.symbol); it != s.references.end()) {
        if (mark != s.stock_marks.end()) explain_part(s, a.symbol, explain_stock(it->second, mark->second.price));
        s.references.erase(it);
      }
      record.shares_after = s.ledger.split_stock(a.symbol, a.numerator, a.denominator, cash);
      record.cash_in_lieu = cash;
      StockFill fill{s.stock_fills.size() + 1, a.symbol, record.shares_after - shares, Money{}, s.time, StockSource::Split, {}};
      fill.split_numerator = a.numerator;
      fill.split_denominator = a.denominator;
      fill.cash_in_lieu = cash;
      s.stock_fills.push_back(std::move(fill));
    }
    if (mark != s.stock_marks.end()) s.stock_marks[a.symbol].price = *split_price;
    start_stock_stretch(s, a.symbol);
    for (const auto& [symbol, contract] : s.contracts) {
      if (contract.underlying != a.symbol || !physical(contract) || contract.expiry < a.ex_date) continue;
      const auto before = deliverable_of(s, symbol);
      const auto scaled = before.shares * a.numerator;
      Deliverable after{scaled / a.denominator, before.cash};
      if (const auto rest = scaled % a.denominator; rest != 0)
        after.cash = after.cash + nearest_cent((split_price ? *split_price : effective_strike(s, contract).prorate(a.denominator, a.numerator))
                                                   .prorate(rest, a.denominator));
      s.deliverables[symbol] = after;
      record.series[symbol] = after;
    }
    for (const auto id : open_stock_ids(s))
      if (s.stock_orders[id - 1].open() && s.stock_orders[id - 1].request.symbol == a.symbol)
        end_stock_order(s, id, OrderStatus::Cancelled,
                        failure(Reason::CORPORATE_ACTION, "A split went ex on the stock; place the order again on the split shares"), events);
    if (s.revised_corporate_actions)
      for (const auto id : open_ids(s)) {
        auto request = s.orders[id - 1].request;
        if (!split_request(s, request, a)) continue;
        auto& order = s.orders.mut(id - 1);
        // A retry of the submission still finds the order the split moved.
        if (!order.submitted && order.parent == 0) order.submitted = order.request;
        order.request = std::move(request);
        event(events, "order_adjusted", order);
      }
    s.corporate_actions.push_back(record);
    event(events, "corporate_action", s.corporate_actions.back());
  }
}
void pay_dividends(State& s, const std::vector<Dividend>& dividends, Events& events) {
  for (const auto& d : dividends) {
    const auto shares = shares_held(s, d.symbol);
    const bool paid = std::any_of(s.dividends.begin(), s.dividends.end(),
        [&](const DividendPayment& p) { return p.symbol == d.symbol && p.ex_date == d.ex_date; });
    if (shares == 0 || paid || d.per_share <= Money{}) continue;
    const Money amount = nearest_cent(d.per_share * shares);
    // Less than half a cent on the shares pays nothing, and nothing is recorded. Replay
    // drivers 1-8 passed it to the ledger, which refuses a zero payment, so the
    // rollover failed and with it the account.
    if (amount == Money{} && s.revised_corporate_actions) continue;
    s.ledger.receive_dividend(d.symbol, amount);
    Attribution part;
    part.other = amount.dollars();
    explain_part(s, d.symbol, part);
    s.dividends.push_back({d.symbol, d.ex_date, d.per_share, shares, amount, s.time, s.stock_fills.size()});
    event(events, "dividend", Json{{"symbol", d.symbol}, {"ex_date", d.ex_date}, {"per_share", d.per_share},
                                   {"shares", shares}, {"amount", amount}});
  }
}
std::string close_key(const std::string& underlying, md::Date date) { return underlying + " " + md::format_date(date); }
}  // namespace
}  // namespace session_internal
CommandResult TradingSession::record_close(const std::string& underlying, md::Date date, Money price, Timestamp print_time,
                                           Timestamp time) {
  if (underlying.empty() || price <= Money{} || !md::valid_date(date))
    throw TradingError(Reason::INVALID_SETTLEMENT, "A closing print needs an underlying, a valid date and a positive price");
  return impl_->transact(time, "closing_print", [&](State& s, Events& events) {
    auto& print = s.closing_prints[close_key(underlying, date)];
    if (print.price != price) {
      print = ClosingPrint{price, print_time};
      event(events, "closing_print", Json{{"underlying", underlying}, {"date", date}, {"price", price}, {"time", print_time}});
    }
    return CommandResult{};
  });
}
std::optional<ClosingPrint> TradingSession::closing_print(const std::string& underlying, md::Date date) const {
  const auto& prints = impl_->state.closing_prints;
  const auto it = prints.find(close_key(underlying, date));
  return it == prints.end() ? std::nullopt : std::optional(it->second);
}
CommandResult TradingSession::roll_day(Timestamp time, const std::vector<Dividend>& dividends,
                                       const std::vector<CorporateAction>& actions) {
  for (const auto& action : actions) {
    try { validate_corporate_action(action); }
    catch (const std::invalid_argument& error) { throw TradingError(Reason::INVALID_ORDER, error.what()); }
  }
  // The finished day closes on the marks it last published, before a new day's
  // quotes replace them: its close, P&L by Greek and the night's assignments come
  // from them, and the move from them to the new day's first quotes is the new day's.
  const auto closing = impl_->snapshot;
  const auto closing_time = impl_->state.time;
  return impl_->transact(time, "day_rollover", [&](State& s, Events& events) {
    const auto day = plan_trading_date(s.config.rules, time);
    const auto days = md::days_since_epoch(day) - md::days_since_epoch(s.day);
    if (day <= s.day) return CommandResult{failure(Reason::INVALID_TIME, "Rollover requires a later trading date"), {}, 0};
    // advance already checked the actual date change. Do not classify positions
    // opened on the new date before this bookkeeping rollover as overnight holds.
    check_rollover_positions(s, events, false);
    monitor_loss(s, events);
    const auto& snapshot = *closing;
    // Shares halted through the close close on their last pre-halt print.
    const bool marked = snapshot.valuation_complete ||
        (std::all_of(snapshot.positions.begin(), snapshot.positions.end(), [](const auto& p) { return p.fresh; }) &&
         std::all_of(snapshot.stocks.begin(), snapshot.stocks.end(), [](const auto& m) { return m.fresh || m.halted; }));
    if (!marked) return CommandResult{failure(Reason::STALE_QUOTE, "Rollover requires complete marked equity"), {}, 0};
    // Close the finished day. An end-of-day floor ratchets only here, from the
    // last fully marked equity observed on that date; breaches are still checked
    // on every transaction by monitor_rules.
    auto& e = s.evaluation;
    const auto& rules = s.config.rules;
    evaluation_rollover(e, rules);
    if (!rules.scaling.empty()) {
      const auto profit = plan_inputs(snapshot).balance - e.starting_balance;
      const auto next = scaling_limit(rules, profit);
      if (next != e.scaling_limit) {
        event(events, "scaling_limit", Json{{"old", e.scaling_limit}, {"limit", next}, {"profit", profit}});
        e.scaling_limit = next;
      }
    }
    Money capital;
    // A placeholder date from before the attempt started is not a trading day.
    // On a funded account, a day with enough net realised profit counts once,
    // toward the payout cycle in progress when it closes.
    if (e.started > 0 && e.day >= plan_trading_date(rules, e.started)) {
      const auto realised = net_realised(s) - e.day_open_realised;
      const auto& payouts = rules.payouts;
      const bool qualifying = rules.phase == Phase::Funded && e.status == EvaluationStatus::Active &&
          payouts.qualifying_days > 0 && realised >= payouts.qualifying_profit && realised > Money{};
      if (qualifying) ++e.qualifying_days;
      e.days.push_back({e.day, e.day_open_equity, e.day_close_equity, e.peak, e.floor, realised, qualifying, snapshot.attribution,
                        e.day_low_equity, e.day_high_equity, e.day_low_at, e.day_high_at, e.day_executions, e.day_lock});
      event(events, "evaluation_day", e.days.back());
      capital = review_size_scaling(s, day, plan_inputs(snapshot).balance, events);
    }
    // A plan limit's lock and the day's executions end with the day.
    e.day_lock = Reason::NONE;
    e.day_locked_at = 0;
    e.day_executions = 0;
    if (s.pending_limits || s.pending_guardrails) {
      if (s.limits_revision == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Limits revision exhausted");
      if (s.pending_limits) { s.config.limits = *s.pending_limits; s.pending_limits.reset(); }
      if (s.pending_guardrails) { s.config.guardrails = *s.pending_guardrails; s.pending_guardrails.reset(); }
      ++s.limits_revision;
      s.pending_applied_at = s.time;
      event(events, "pending_limits_applied", Json{{"limits", s.config.limits}, {"guardrails", s.config.guardrails}});
    }
    const bool personal_latch = s.guardrails.owns_kill;
    const auto cooldown_until = s.guardrails.cooldown_until;
    // The size-creep median rolls across days; the day's counts and flags end with it.
    auto opening_sizes = std::move(s.guardrails.opening_sizes);
    s.guardrails = {};
    s.guardrails.owns_kill = personal_latch;
    s.guardrails.opening_sizes = std::move(opening_sizes);
    if (cooldown_until > s.time) s.guardrails.cooldown_until = cooldown_until;
    refresh_guardrail_latch(s, events);
    const auto opening_equity = snapshot.equity + capital;
    e.day_low_equity = opening_equity; e.day_high_equity = opening_equity;
    e.day_low_at = s.time; e.day_high_at = s.time;
    e.day_open_realised = net_realised(s);
    e.day = day;
    e.day_open_equity = opening_equity;
    if (e.day_open_floor) e.day_open_floor = e.floor;
    e.day_close_equity = opening_equity;
    if (rules.drawdown_basis == DrawdownBasis::Balance) e.day_close_balance = plan_inputs(snapshot).balance;
    s.start_equity = opening_equity;
    s.day = day;
    // Each round trip keeps its stretches to the close; the new day's P&L by
    // Greek runs from these marks.
    for (const auto& [symbol, reference] : s.references) {
      const auto trip = s.trips.find(symbol);
      if (trip == s.trips.end()) continue;
      if (s.contracts.contains(symbol)) {
        if (const auto mark = s.marks.find(symbol); mark != s.marks.end())
          s.trip_attribution[trip->second] += explain(s.contracts.at(symbol), reference, mark->second.price, valuation_of(s, symbol));
      } else if (const auto mark = s.stock_marks.find(symbol); mark != s.stock_marks.end()) {
        s.trip_attribution[trip->second] += explain_stock(reference, mark->second.price);
      }
    }
    s.explained.clear();
    s.references.clear();
    for (const auto& [symbol, position] : s.ledger.positions()) start_stretch(s, symbol);
    for (const auto& [symbol, stock] : s.ledger.stocks()) start_stock_stretch(s, symbol);
    event(events, "day_rollover", Json{{"day", day}, {"equity", s.start_equity}});
    // Assignments arrive overnight, so the new day takes them, and then the
    // ex-date's dividends pay the shares held into it.
    // Financing is new-day P&L. Its opening daily-loss base stays unchanged;
    // charges can consume loss room and interest changes marked equity. It runs over
    // the nights the attempt has existed: one made or reset before any market data
    // starts on a placeholder date (1970-01-01), not a trading day, and takes its first
    // real market time as its start. Replay drivers 1-8 accrued from the placeholder.
    auto financed = days;
    if (s.revised_corporate_actions)
      financed = e.started > 0 ? std::min(days, md::days_since_epoch(day) - md::days_since_epoch(plan_trading_date(rules, e.started))) : 0;
    accrue_financing(s, impl_->state, financed, events);
    assign_early(s, dividends, actions, closing_time, events);
    // Splits take effect before the day's dividends, which are quoted on the split
    // shares. A rollover that spans several ex-dates takes them date by date, so a
    // dividend going ex before a split is paid on the shares before it (replay drivers
    // 1-8 split first and paid every dividend on the split shares).
    if (!s.revised_corporate_actions) {
      apply_corporate_actions(s, actions, events);
      pay_dividends(s, dividends, events);
      return CommandResult{};
    }
    std::set<md::Date> dates;
    for (const auto& a : actions) dates.insert(a.ex_date);
    for (const auto& d : dividends) dates.insert(d.ex_date);
    for (const auto& date : dates) {
      std::vector<CorporateAction> splits;
      std::vector<Dividend> paid;
      std::copy_if(actions.begin(), actions.end(), std::back_inserter(splits), [&](const CorporateAction& a) { return a.ex_date == date; });
      std::copy_if(dividends.begin(), dividends.end(), std::back_inserter(paid), [&](const Dividend& d) { return d.ex_date == date; });
      apply_corporate_actions(s, splits, events);
      pay_dividends(s, paid, events);
    }
    return CommandResult{};
  });
}
CommandResult TradingSession::halt_underlying(const std::string& symbol, Timestamp end, Timestamp time) {
  const auto& s = impl_->state;
  if (const auto it = s.market_halts.find(symbol); it != s.market_halts.end() && it->second >= end)
    return {{}, {}, s.version};
  if (symbol.empty() || time <= 0 || end <= time || end - time > md::kNanosPerDay)
    throw TradingError(Reason::INVALID_TIME, "A market halt needs an underlying and an end within the next day");
  return impl_->transact(time, "market_halt", [&](State&, Events& events) {
    event(events, "market_halt", Json{{"underlying", symbol}, {"end", end}, {"simulated", true}});
    return CommandResult{};
  }, {}, [&](State& next) {
    // Install the pause before advancing the clock can trigger a protective close.
    next.market_halts[symbol] = end;
  });
}
CommandResult TradingSession::transfer_cash(std::string client_id, Money amount, Timestamp time) {
  if (client_id.empty() || client_id.size() > 64 ||
      std::any_of(client_id.begin(), client_id.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
      amount == Money{} || amount.micros() % 10000 != 0)
    throw TradingError(Reason::INVALID_MONEY, "A transfer needs a client_id of 1-64 printable bytes and a nonzero amount in whole cents");
  const auto& current = impl_->state;
  for (const auto& flow : current.cash_flows) {
    if (flow.attempt != current.evaluation.attempt || flow.client_id != client_id) continue;
    return {flow.amount == amount ? Decision{} : failure(Reason::DUPLICATE_CLIENT_ID, "Transfer ID already used with another amount"),
            {}, current.version};
  }
  return impl_->transact(time, "cash_transfer", [&](State& s, Events& events) {
    const auto& rules = s.config.rules;
    if (!rules.practice())
      return CommandResult{failure(Reason::PLAN_LOCKED, "Cash transfers are available only on practice accounts; evaluation and funded plans use their plan capital and payouts"), {}, 0};
    const auto snapshot = snapshot_of(s);
    if (amount < Money{} && (!snapshot.valuation_complete || -amount > s.ledger.account().cash ||
        -amount > snapshot.buying_power.available || snapshot.equity + amount <= Money{}))
      return CommandResult{failure(Reason::BUYING_POWER, "Withdrawal requires complete marks, positive remaining equity and sufficient cash and unreserved buying power"), {}, 0};
    if (amount > Money{}) s.ledger.add_capital(amount); else s.ledger.withdraw(-amount);
    s.start_equity = s.start_equity + amount;
    auto& e = s.evaluation;
    e.starting_balance = e.starting_balance + amount;
    e.day_open_equity = e.day_open_equity + amount;
    if (e.day_open_floor) e.day_open_floor = *e.day_open_floor + amount;
    e.day_close_equity = e.day_close_equity + amount;
    e.peak = e.peak + amount;
    if (e.day_low_equity) e.day_low_equity = *e.day_low_equity + amount;
    if (e.day_high_equity) e.day_high_equity = *e.day_high_equity + amount;
    s.cash_flows.push_back({s.cash_flows.size() + 1, e.attempt, time, s.day,
                            amount > Money{} ? "deposit" : "withdrawal", {}, amount, {}, 0, 0, client_id});
    event(events, "cash_flow", s.cash_flows.back());
    return CommandResult{};
  });
}
CommandResult TradingSession::request_payout(Money amount, Timestamp time) {
  if (amount <= Money{} || amount.micros() % 10'000 != 0)
    throw TradingError(Reason::INVALID_PAYOUT, "Payout amount must be a positive whole number of cents");
  return impl_->transact(time, "payout", [&](State& s, Events& events) {
    const auto& rules = s.config.rules;
    auto& e = s.evaluation;
    const auto snapshot = snapshot_of(s);
    const auto quote = payout_quote(snapshot, rules);
    auto reject = [&](std::string message, Money limit) {
      return CommandResult{{Reason::INVALID_PAYOUT, std::move(message), amount.dollars(), limit.dollars(), "aggregate"}, {}, 0};
    };
    if (!quote.blocked.ok()) return CommandResult{quote.blocked, {}, 0};
    if (amount < quote.minimum) return reject("Below the minimum payout", quote.minimum);
    if (amount > quote.maximum) return reject("Above the maximum for this payout", quote.maximum);
    const Money equity = snapshot.equity;  // Flat, so this is cash.
    // Forfeited profit leaves with the payout; a reset then takes the account down
    // to its reset balance. Neither is paid to the trader.
    const auto& p = rules.payouts;
    const auto removed = quote.reset_balance ? equity - quote.forfeit - amount - *quote.reset_balance : Money{};
    const auto out = amount + quote.forfeit + removed;
    s.ledger.withdraw(out);
    // The trading day in progress takes the withdrawal, even after its date
    // ends and before rollover, so its P&L and an end-of-day ratchet ignore it.
    s.start_equity = s.start_equity - out;
    e.day_open_equity = e.day_open_equity - out;
    if (e.day_open_floor) e.day_open_floor = *e.day_open_floor - out;
    e.day_close_equity = e.day_close_equity - out;
    if (e.day_close_balance) e.day_close_balance = *e.day_close_balance - out;
    // The day's extremes too, so a daily loss limit trailing the day's high
    // does not count the withdrawal as a loss.
    if (e.day_low_equity) e.day_low_equity = *e.day_low_equity - out;
    if (e.day_high_equity) e.day_high_equity = *e.day_high_equity - out;
    if (p.after_payout == PayoutFloor::Reset) {
      // The floor starts again below the reset balance, as at the attempt's start.
      e.peak = *quote.reset_balance;
      e.floor_locked = false;
      e.floor = evaluation_floor(rules, e.peak, e.floor_locked,
          e.revised_rules ? *quote.reset_balance : e.starting_balance);
    } else if (!e.floor_locked) {
      e.peak = e.peak - out;
      const auto floor = evaluation_floor(rules, e.peak, e.floor_locked, e.starting_balance);
      e.floor = e.size_scaling ? std::max(e.floor - out, floor) : floor;
    }
    if (p.after_payout == PayoutFloor::StartingBalance) {
      e.floor = std::max(e.floor, e.starting_balance);
      e.floor_locked = true;
    }
    e.payouts.push_back({quote.number, s.time, e.day, amount, amount.prorate(rules.payouts.split_percent, 100), equity});
    e.payouts.back().review = quote.review;
    e.payouts.back().removed = removed;
    s.payouts_received = s.payouts_received + e.payouts.back().trader_share;
    e.qualifying_days = 0;
    e.cycle_started = s.time;
    event(events, "payout", e.payouts.back());
    return CommandResult{};
  });
}
CommandResult TradingSession::reset_account(Money initial_cash, AccountRules rules, std::string reason, Timestamp time, bool activated, bool archive_payouts, bool restore_scaled_rules) {
  require_reason(reason);
  const auto& current = impl_->state;
  if (restore_scaled_rules && initial_cash == current.config.initial_cash &&
      same_program_rules(rules, current.config.rules))
    rules = original_program_rules(std::move(rules), current.evaluation.starting_balance, current.evaluation.size_scaling);
  normalize_event_rules(rules, true);
  validate_rules(rules);
  if (initial_cash <= Money{}) throw TradingError(Reason::INVALID_MONEY, "Starting balance must be positive");
  if (rules.size_scaling && rules.size_scaling->max_balance < initial_cash)
    throw TradingError(Reason::INVALID_RULES, "Account size scaling maximum must be at least the starting balance");
  return impl_->transact(time, "account_reset", [&](State& s, Events& events) {
    const auto snapshot = snapshot_of(s);
    const bool same = initial_cash == s.config.initial_cash && same_program_rules(rules,
        original_program_rules(s.config.rules, s.evaluation.starting_balance, s.evaluation.size_scaling));
    const auto costs = program_costs(snapshot, s.config.rules);
    if (same && rules.max_resets > 0 && costs.resets_used >= rules.max_resets)
      return CommandResult{{Reason::RESET_LIMIT, "This plan has no resets left; choose a different plan to start a new purchase",
                            static_cast<double>(costs.resets_used + 1), static_cast<double>(rules.max_resets), {}}, {}, 0};
    if (activated && (same || s.evaluation.status != EvaluationStatus::Passed))
      return CommandResult{failure(Reason::PLAN_LOCKED, "Activation requires a pass into a different plan"), {}, 0};
    const auto fee = same ? rules.reset_fee : activated ? rules.activation_fee : rules.evaluation_fee;
    // Check aggregate overflow before changing anything, even though the fees never touch paper cash.
    static_cast<void>(costs.total() + fee);
    for (const auto id : open_ids(s)) cancel_order(s, id, failure(Reason::ACCOUNT_RESET, reason), events);
    for (const auto id : open_stock_ids(s))
      end_stock_order(s, id, OrderStatus::Cancelled, failure(Reason::ACCOUNT_RESET, reason), events);
    for (const auto& p : snapshot.positions) {
      const auto& position = p.position;
      s.closures.push_back({position.contract.osi_symbol(), position.quantity,
                            p.mark ? *p.mark : average_unit_price(position), s.time, ClosureKind::Reset, s.fills.size()});
    }
    // Shares leave with the old ledger too, at their mark or else their cost.
    for (const auto& held : snapshot.stocks) {
      const auto& p = held.position;
      const auto magnitude = p.shares < 0 ? -p.shares : p.shares;
      const auto price = held.mark ? *held.mark : (p.shares < 0 ? -p.basis : p.basis).prorate(1, magnitude);
      s.stock_fills.push_back({s.stock_fills.size() + 1, p.symbol, -p.shares, price, s.time, StockSource::Reset, {}});
    }
    const auto& e = s.evaluation;
    s.attempts.push_back({e.attempt, s.config.rules.plan, e.started, s.time, e.starting_balance, snapshot.equity,
                          e.status, e.decision, e.first_order, e.first_fill, e.decision_code, s.config.rules, e.decided_at,
                          e.status == EvaluationStatus::Active ? std::nullopt : std::optional(e.decided_equity), e.peak,
                          s.config.rules.max_drawdown > Money{} ? std::optional(e.floor) : std::nullopt, s.fee_charged, {},
                          archive_payouts ? e.size_scaling : std::nullopt,
                          s.revised_share_attempts ? e.first_stock_fill : std::uint64_t{1}});
    if (archive_payouts)
      for (const auto& payout : e.payouts) s.attempts.back().payouts_received = s.attempts.back().payouts_received + payout.trader_share;
    s.fee_charged = {fee, fee == Money{} ? "" : same ? "reset" : activated ? "activation" : "evaluation"};
    const auto attempt = e.attempt + 1;
    s.config.initial_cash = initial_cash;
    s.config.rules = std::move(rules);
    s.ledger = Ledger(initial_cash);
    s.explained.clear();
    s.references.clear();
    s.do_not_exercise = {};
    s.trips.clear();
    s.start_equity = initial_cash;
    // A reset after the plan day ended, before its rollover ran, starts the new date as
    // the rollover would have: the finished day's personal latches, opening-trade count
    // and flags end with it, and only an unexpired cooldown stays (driver 11; replay
    // drivers 1-10 carried them through the whole of the next day).
    if (s.revised_lifecycle && plan_trading_date(s.config.rules, s.time) > s.day) {
      const bool personal_latch = s.guardrails.owns_kill;
      const auto cooldown_until = s.guardrails.cooldown_until;
      auto opening_sizes = std::move(s.guardrails.opening_sizes);
      s.guardrails = {};
      s.guardrails.owns_kill = personal_latch;
      s.guardrails.opening_sizes = std::move(opening_sizes);
      if (cooldown_until > s.time) s.guardrails.cooldown_until = cooldown_until;
    }
    // The new ledger clears only the equity-based latch. Daily discipline survives the attempt.
    std::erase(s.guardrails.latched, Reason::SOFT_FLOOR);
    if (!s.guardrails.owns_kill || guardrail_reason(s) == Reason::NONE) {
      if (s.kill) record_kill(s, "reset", reason, s.kill_reason);
      s.kill = false;
      s.kill_reason.clear();
      s.guardrails.owns_kill = false;
    }
    // The new attempt starts with any pending settings, and says when they applied.
    const bool pending = s.pending_limits || s.pending_guardrails;
    if (s.pending_limits) { s.config.limits = *s.pending_limits; s.pending_limits.reset(); ++s.limits_revision; }
    if (s.pending_guardrails) { s.config.guardrails = *s.pending_guardrails; s.pending_guardrails.reset(); ++s.limits_revision; }
    s.guardrails.trade_limit_order = 0;
    refresh_guardrail_latch(s, events);
    s.pending_applied_at = pending ? s.time : 0;
    s.day = plan_trading_date(s.config.rules, s.time);
    s.evaluation = fresh_evaluation(s, attempt);
    Json reset{{"reason", reason}, {"attempt", attempt}, {"initial_cash", initial_cash}, {"rules", s.config.rules}};
    if (s.fee_charged.amount != Money{}) reset["fee_charged"] = s.fee_charged;
    event(events, "account_reset", reset);
    return CommandResult{};
  });
}
std::shared_ptr<const TradingSnapshot> TradingSession::snapshot() const { return impl_->snapshot; }
std::string TradingSession::snapshot_json() const { return Json(*impl_->snapshot).dump(); }
const SessionConfig& TradingSession::config() const { return impl_->state.config; }
const Contracts& TradingSession::contracts() const { return impl_->state.contracts; }
const SharedMap<std::string, Deliverable>& TradingSession::deliverables() const { return impl_->state.deliverables; }
const Valuations& TradingSession::valuations() const { return impl_->state.valuations; }
OpeningOrder TradingSession::opening_order(const OrderRequest& request, Timestamp time, const PreviewMarket& market,
                                            const std::map<std::string, Quantity>& preceding) const {
  return opening_exposure(prepared(impl_->state, time, market), request, preceding);
}
std::optional<double> TradingSession::held_dollar_delta(const std::string& underlying, Timestamp time) const {
  auto state = impl_->state;
  state.time = time;
  return held_direction(state, underlying);
}

std::vector<CounterExposure> TradingSession::counter_exposures(const std::string& account, Timestamp time,
    const std::vector<Valuation>& valuations, const std::vector<StockPrice>& stocks) const {
  // Overlay current marks, then age orders on a private copy. Expired or
  // clock-cancelled entries do not reserve direction; holdings never execute.
  auto state = impl_->state;
  for (const auto& v : valuations)
    if (v.time > 0 && v.time <= time && state.contracts.contains(v.symbol)) state.valuations[v.symbol] = v;
  for (const auto& p : stocks)
    if (p.time > 0 && p.time <= time && p.price > Money{}) state.stock_marks[p.symbol] = {p.price, p.time};
  Events ignored;
  advance(state, time, ignored);
  std::set<std::string> underlyings;
  for (const auto& [symbol, p] : state.ledger.positions()) underlyings.insert(p.contract.underlying);
  for (const auto& [symbol, p] : state.ledger.stocks()) underlyings.insert(symbol);
  std::vector<CounterExposure> result;
  for (const auto& underlying : underlyings) result.push_back({account, underlying, held_direction(state, underlying)});
  for (const auto id : open_ids(state)) {
    const auto& order = state.orders[id - 1];
    if (order.system || kept_within(order)) continue;
    auto request = order.request;
    request.quantity = order.remaining();
    const auto exposure = opening_exposure(state, request);
    if (!exposure.opening) continue;
    if (exposure.decision.ok()) {
      for (const auto& [underlying, delta] : exposure.dollar_delta) result.push_back({account, underlying, delta});
    } else {
      std::set<std::string> missing;
      for (const auto& symbol : order_symbols(request))
        if (const auto c = state.contracts.find(symbol); c != state.contracts.end()) missing.insert(c->second.underlying);
      for (const auto& underlying : missing) result.push_back({account, underlying, std::nullopt});
    }
  }
  return result;
}

std::optional<QuoteObservation> TradingSession::quote(const std::string& symbol) const {
  const auto it = impl_->state.books.find(symbol);
  return it == impl_->state.books.end() ? std::nullopt : std::optional(it->second.quote);
}
std::map<std::string, SizeLeft> TradingSession::sizes_left() const {
  std::map<std::string, SizeLeft> out;
  for (const auto& [symbol, book] : impl_->state.books) {
    const auto& q = book.quote;
    // The budgets an observation starts with: an ask-only quote offers its ask to exits.
    const auto bid = valid_quote(q) ? q.bid_size : 0;
    const auto ask = markable_quote(q) ? q.ask_size : 0;
    if (book.bid_left < bid || book.ask_left < ask) out[symbol] = {q.observation, q.bid_size, q.ask_size, book.bid_left, book.ask_left};
  }
  return out;
}
md::Date TradingSession::trading_day() const { return impl_->state.day; }
md::Date TradingSession::trading_date(Timestamp time) const { return plan_trading_date(impl_->state.config.rules, time); }
namespace session_internal {
/// Records (or, empty, clears) a trade's note and tags under `key`.
void store_annotation(State& s, const std::string& key, Annotation annotation, Events& events) {
  Json payload{{"trade", key}, {"note", annotation.note}, {"tags", annotation.tags}};
  if (!annotation.mistakes.empty()) payload["mistakes"] = annotation.mistakes;
  event(events, "trade_annotated", payload);
  if (annotation.note.empty() && annotation.tags.empty() && annotation.mistakes.empty()) {
    s.annotations.erase(key);
  } else {
    annotation.time = s.time;
    s.annotations[key] = std::move(annotation);
  }
}
}  // namespace session_internal
CommandResult TradingSession::annotate(std::uint64_t trade, std::string note, std::vector<std::string> tags, Timestamp time, std::vector<std::string> mistakes) {
  auto annotation = clean_annotation(std::move(note), tags);
  for (const auto& mistake : mistakes) {
    static const std::vector<std::string> picks{"chased", "oversized", "moved stop", "revenge", "broke plan", "early exit", "A+ setup"};
    if (std::find(picks.begin(), picks.end(), mistake) == picks.end())
      throw TradingError(Reason::INVALID_NOTE, "Unknown mistake pick: " + mistake);
    if (std::find(annotation.mistakes.begin(), annotation.mistakes.end(), mistake) == annotation.mistakes.end()) annotation.mistakes.push_back(mistake);
  }
  return impl_->transact(time, "annotate", [&](State& s, Events& events) {
    const auto trades = lifecycles(s.fills, s.closures, s.contracts);
    if (std::none_of(trades.begin(), trades.end(), [&](const Lifecycle& t) { return t.first_fill == trade; }))
      return CommandResult{failure(Reason::UNKNOWN_TRADE, "No trade opens with fill " + std::to_string(trade)), {}, 0};
    store_annotation(s, std::to_string(trade), std::move(annotation), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::group_trades(std::vector<std::uint64_t> trades, bool together, Timestamp time) {
  return impl_->transact(time, together ? "group" : "ungroup", [&](State& s, Events& events) {
    const auto decision = detail::regroup(s, trades, together);
    if (!decision.ok()) return CommandResult{decision, {}, 0};
    Json ids = Json::array();
    for (const auto id : trades) ids.push_back(std::to_string(id));
    event(events, together ? "trades_grouped" : "trades_ungrouped", Json{{"trades", ids}});
    return CommandResult{};
  });
}
CommandResult TradingSession::create_alert(AlertSpec spec, Timestamp time) {
  validate_alert(spec);
  return impl_->transact(time, "alert", [&](State& s, Events& events) {
    const auto& c = spec.condition;
    std::vector<std::string> symbols;
    if (c.scope == AlertScope::Contract) symbols.push_back(c.symbol);
    for (const auto& leg : c.legs) symbols.push_back(leg.symbol);
    for (const auto& symbol : symbols)
      if (!s.contracts.contains(symbol))
        return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "An alert's contracts must be registered canonical OSI definitions"), {}, 0};
    if (s.alerts.items.size() >= kMaxAlerts)
      return CommandResult{failure(Reason::INVALID_ALERT, "An account keeps at most 100 alerts; delete one first"), {}, 0};
    if (s.alerts.created == std::numeric_limits<std::uint64_t>::max()) throw TradingError(Reason::ARITHMETIC_OVERFLOW, "Alert IDs exhausted");
    Alert alert;
    alert.id = ++s.alerts.created;
    alert.spec = std::move(spec);
    alert.created = s.time;
    alert.actor = s.actor;
    s.alerts.items.push_back(alert);
    event(events, "alert_created", Json{{"alert", alert}});
    check_alerts(s, events);
    CommandResult result;
    result.alert_id = alert.id;
    return result;
  });
}
CommandResult TradingSession::delete_alert(std::uint64_t id, Timestamp time) {
  return impl_->transact(time, "alert", [&](State& s, Events& events) {
    auto& items = s.alerts.items;
    const auto it = std::find_if(items.begin(), items.end(), [&](const Alert& a) { return a.id == id; });
    if (it == items.end()) return CommandResult{failure(Reason::UNKNOWN_ALERT, "No alert " + std::to_string(id)), {}, 0};
    event(events, "alert_deleted", Json{{"alert", *it}});
    items.erase(it);
    return CommandResult{};
  });
}
CommandResult TradingSession::annotate_day(md::Date day, std::string plan, std::string review, Timestamp time) {
  if (!md::valid_date(day))
    throw TradingError(Reason::INVALID_NOTE, "A day note needs a valid YYYY-MM-DD date");
  DayNote note{clean_annotation(std::move(plan), {}).note, clean_annotation(std::move(review), {}).note, time};
  return impl_->transact(time, "day_note", [&](State& s, Events& events) {
    const auto key = md::format_date(day);
    if (note.plan.empty() && note.review.empty()) s.day_notes.erase(key);
    else s.day_notes[key] = note;
    event(events, "day_annotated", Json{{"day", key}, {"note", note}});
    return CommandResult{};
  });
}
CommandResult TradingSession::annotate_shares(std::uint64_t first_fill, std::string note, std::vector<std::string> tags, Timestamp time, std::vector<std::string> mistakes) {
  auto annotation = clean_annotation(std::move(note), tags);
  for (const auto& mistake : mistakes) {
    static const std::vector<std::string> picks{"chased", "oversized", "moved stop", "revenge", "broke plan", "early exit", "A+ setup"};
    if (std::find(picks.begin(), picks.end(), mistake) == picks.end())
      throw TradingError(Reason::INVALID_NOTE, "Unknown mistake pick: " + mistake);
    if (std::find(annotation.mistakes.begin(), annotation.mistakes.end(), mistake) == annotation.mistakes.end()) annotation.mistakes.push_back(mistake);
  }
  return impl_->transact(time, "annotate", [&](State& s, Events& events) {
    const auto trips = share_lifecycles(s.stock_fills, s.dividends);
    if (std::none_of(trips.begin(), trips.end(), [&](const ShareLifecycle& t) { return t.fills.front() == first_fill; }))
      return CommandResult{failure(Reason::UNKNOWN_TRADE, "No share trade opens with stock fill " + std::to_string(first_fill)), {}, 0};
    store_annotation(s, "s" + std::to_string(first_fill), std::move(annotation), events);
    return CommandResult{};
  });
}
CommandResult TradingSession::exercise(const std::string& symbol, Quantity contracts, Timestamp time) {
  if (contracts <= 0) throw TradingError(Reason::INVALID_ORDER, "Exercise a positive number of contracts");
  return impl_->transact(time, "exercise", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Exercise references an unregistered OSI"), {}, 0};
    const auto contract = it->second;
    if (!physical(contract))
      return CommandResult{failure(Reason::INVALID_ORDER, "Only American equity and ETF options deliver shares; index options settle in cash"), {}, 0};
    if (s.time >= contract.expiry_time()) return CommandResult{failure(Reason::EXPIRED, "The contract has expired; settlement exercises it"), {}, 0};
    if (const auto d = account_check(s); !d.ok()) return CommandResult{d, {}, 0};
    if (held(s, symbol) < contracts) return CommandResult{failure(Reason::INVALID_ORDER, "Exercise needs that many long contracts"), {}, 0};
    // A put's sale of shares counts the working share sells before it.
    if (s.config.rules.buy_only && contract.type == pricing::OptionType::Put &&
        pending_short_shares(s, contract.underlying, 0, delivered(s, contract, contracts)) < 0)
      return CommandResult{failure(Reason::BUY_ONLY, "This plan is buy-only: put exercise would sell shares short; hold enough long shares first"), {}, 0};
    // As a share sale would be: defined risk allows no short shares.
    if (s.config.rules.defined_risk && contract.type == pricing::OptionType::Put &&
        -delivered(s, contract, contracts) > std::max<Quantity>(0, shares_held(s, contract.underlying)))
      return CommandResult{failure(Reason::DEFINED_RISK, "This plan allows defined risk only: put exercise would sell shares short, which can lose without limit"), {}, 0};
    if (const auto d = defined_risk_check(s, {{symbol, -contracts}},
                                          "Exercising this long would leave a short option uncovered; close the short first");
        !d.ok())
      return CommandResult{d, {}, 0};
    if (const auto d = account_type_check(s, {{symbol, -contracts}}, {{contract.underlying, delivered(s, contract, contracts)}}); !d.ok())
      return CommandResult{d, {}, 0};
    const auto price = stock_price(s, contract.underlying);
    if (!price) return CommandResult{failure(Reason::STALE_QUOTE, "Exercise needs a fresh price for " + contract.underlying), {}, 0};
    const Money intrinsic = std::max(Money{}, unit_exercise_value(s, contract, *price));
    if (intrinsic <= Money{}) return CommandResult{failure(Reason::INVALID_ORDER, "Exercise needs the option in the money"), {}, 0};
    const auto shares = delivered(s, contract, contracts);
    // The contracts close at intrinsic value, so any time value left is a cost,
    // and the shares change hands at the underlying's price.
    const auto apply = [&](State& t) {
      const auto fee = exercise_fee(t, contracts);
      fill_position(t, symbol, -contracts, intrinsic, fee);
      t.closures.push_back({symbol, contracts, intrinsic, t.time, ClosureKind::Exercise, t.fills.size(), fee});
      if (shares != 0) trade_shares(t, contract.underlying, shares, *price, StockSource::Exercise, symbol);
    };
    // Shares the exercise opens, or adds to, enter the plan as a share trade would:
    // its entry windows, stop requirement, hedging, trade-risk and exposure rules
    // apply. Shares that only reduce a holding, as a put against long shares, do not.
    if (shares != 0 && !reduces_shares(s, contract.underlying, shares)) {
      if (const auto d = plan_entry_check(s.config.rules, contract.underlying, s.time); !d.ok()) return CommandResult{d, {}, 0};
      ShareTerms terms;
      terms.exercise = true;
      if (const auto d = opening_share_rules_check(s, contract.underlying, shares, *price, terms, events, apply); !d.ok())
        return CommandResult{d, {}, 0};
    }
    if (s.config.rules.buying_power) {
      State projected = s;
      apply(projected);
      const auto power = buying_power(projected).total;
      if (free_power(projected) < free_power(s) && power.available < Money{})
        return CommandResult{{Reason::BUYING_POWER, "Exercise needs more buying power than the account has available",
                              (-power.available).dollars(), 0.0, contract.underlying}, {}, 0};
    }
    apply(s);
    sync_exits(s, symbol, events);
    event(events, "exercise", Json{{"symbol", symbol}, {"contracts", contracts}, {"intrinsic", intrinsic},
                                   {"underlying", contract.underlying}, {"shares", shares}, {"price", *price}});
    return CommandResult{};
  });
}
CommandResult TradingSession::abandon(const std::string& symbol, Timestamp time) {
  return impl_->transact(time, "abandon", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "Abandon references an unregistered OSI"), {}, 0};
    const auto contracts = held(s, symbol);
    if (contracts <= 0) return CommandResult{failure(Reason::INVALID_ORDER, "Abandon needs a long position; buy a short back instead"), {}, 0};
    if (const auto d = account_check(s, true); !d.ok()) return CommandResult{d, {}, 0};
    // Only what cannot be sold: nobody bids for it, or it has expired.
    if (s.time < it->second.expiry_time() && !asks_only(s, symbol)) {
      auto d = quote_check(s, symbol);
      if (d.ok()) d = failure(Reason::INVALID_ORDER, "Someone bids for it: sell it instead");
      return CommandResult{d, {}, 0};
    }
    if (const auto d = defined_risk_check(s, {{symbol, -contracts}},
                                          "Abandoning this long would leave a short option uncovered; close the short first");
        !d.ok())
      return CommandResult{d, {}, 0};
    // These orders disappear with the long. They must not reserve its cover
    // during the account-type check either (including bracket siblings).
    const auto cancel_sales = [&](State& target, Events& changes) {
      for (const auto id : open_ids(target)) {
        const auto& o = target.orders[id - 1];
        const bool sells = std::any_of(o.request.legs.begin(), o.request.legs.end(), [&](const Leg& leg) {
          return leg.symbol == symbol && leg.side == Side::Sell;
        }) || (o.request.symbol == symbol && o.request.side == Side::Sell);
        if (sells) cancel_order(target, id, failure(Reason::POSITION_CLOSED, "The position this order sold was abandoned"), changes);
      }
    };
    if (s.config.rules.account_type != AccountType::Margin) {
      State checked = s;
      Events ignored;
      cancel_sales(checked, ignored);
      if (const auto d = account_type_check(checked, {{symbol, -contracts}}); !d.ok()) return CommandResult{d, {}, 0};
    }
    // The contracts leave at zero, without a fee: the whole basis is the loss.
    const auto apply = [&](State& t) {
      fill_position(t, symbol, -contracts, Money{}, Money{});
      t.closures.push_back({symbol, contracts, Money{}, t.time, ClosureKind::Abandon, t.fills.size()});
    };
    if (s.config.rules.buying_power) {
      State projected = s;
      apply(projected);
      Events ignored;
      cancel_sales(projected, ignored);
      const auto power = buying_power(projected).total;
      if (free_power(projected) < free_power(s) && power.available < Money{})
        return CommandResult{{Reason::BUYING_POWER, "Abandoning this long leaves the short it covers needing more buying power than the account has",
                              (-power.available).dollars(), 0.0, it->second.underlying}, {}, 0};
    }
    apply(s);
    sync_exits(s, symbol, events);
    cancel_sales(s, events);
    event(events, "abandon", Json{{"symbol", symbol}, {"contracts", contracts}});
    return CommandResult{};
  });
}
CommandResult TradingSession::instruct_exercise(const std::string& symbol, bool do_not_exercise, Timestamp time) {
  return impl_->transact(time, "exercise_instruction", [&](State& s, Events& events) {
    const auto it = s.contracts.find(symbol);
    if (it == s.contracts.end()) return CommandResult{failure(Reason::UNKNOWN_CONTRACT, "The instruction references an unregistered OSI"), {}, 0};
    if (held(s, symbol) <= 0)
      return CommandResult{failure(Reason::INVALID_ORDER, "Only a long can be left unexercised; a short is assigned at its holder's choice"), {}, 0};
    if (const auto d = account_check(s, true); !d.ok()) return CommandResult{d, {}, 0};
    if (do_not_exercise) s.do_not_exercise.insert(symbol);
    else s.do_not_exercise.erase(symbol);
    event(events, "exercise_instruction", Json{{"symbol", symbol}, {"do_not_exercise", do_not_exercise}});
    return CommandResult{};
  });
}
CommandResult TradingSession::trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time,
                                          std::optional<StockPrice> feed_price, bool reduce_only) {
  if (signed_shares == 0 || (!reduce_only && (signed_shares < -10'000'000 || signed_shares > 10'000'000)))
    throw TradingError(Reason::INVALID_ORDER, "Trade a nonzero number of shares, at most 10,000,000");
  if (!reduce_only && (symbol.empty() || symbol.size() > 16 ||
      !std::all_of(symbol.begin(), symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; })))
    throw TradingError(Reason::INVALID_ORDER, "Use an uppercase stock or ETF symbol");
  if (feed_price && (feed_price->symbol != symbol || feed_price->price <= Money{} || feed_price->time < 0 || feed_price->time > time))
    throw TradingError(Reason::INVALID_QUOTE, "A stock price needs the traded symbol, a positive price and no later time");
  return impl_->transact(time, "stock_trade", [&](State& s, Events& events) {
    ShareTerms terms;
    terms.reduce_only = reduce_only;
    // The trade's own terms and the account refuse before the market does, and before
    // the feed price is kept, as recorded runs did.
    if (const auto d = share_entry_checks(s, symbol, signed_shares, terms); !d.ok()) return CommandResult{d, {}, 0};
    if (feed_price) {
      const auto prior = s.stock_marks.find(symbol);
      if (prior == s.stock_marks.end() || prior->second.time <= feed_price->time)
        s.stock_marks[symbol] = {split_basis(s, symbol, feed_price->time, feed_price->price), feed_price->time};
    }
    if (const auto d = share_close_check(s, symbol); !d.ok()) return CommandResult{d, {}, 0};
    const auto d = trade_shares_checked(s, symbol, signed_shares, *stock_price(s, symbol), terms, events);
    return CommandResult{d, {}, 0};
  });
}
CommandResult TradingSession::submit_stock_order(StockOrderRequest request, Timestamp time, std::optional<StockPrice> feed_price) {
  validate_stock_order(request);
  if (feed_price && (feed_price->symbol != request.symbol || feed_price->price <= Money{} || feed_price->time < 0 || feed_price->time > time))
    throw TradingError(Reason::INVALID_QUOTE, "A stock price needs the traded symbol, a positive price and no later time");
  const auto& current = impl_->state;
  // A retry finds the order its client ID first named in this attempt, partner
  // included, and gets its first answer: the order as it stands, or the rejection it
  // ended with. An earlier attempt's orders hold no IDs (first_stock_order).
  if (const auto first = current.stock_clients.find(request.client_order_id);
      first != current.stock_clients.end() && first->second >= current.evaluation.first_stock_order) {
    const auto& o = current.stock_orders.at(static_cast<std::size_t>(first->second - 1));
    // The terms as the order took them: its partner under the name it was given.
    auto named = request;
    for (auto& partner : named.oco) partner.client_order_id = request.client_order_id + ":oco";
    bool same = false;
    if (!o.request.oco.empty()) {
      same = o.request == named;  // a partner refused with the order: its terms stay on it
    } else {
      auto terms = named;
      terms.oco.clear();
      same = o.request == terms && (o.oco != 0) == !named.oco.empty();
      if (same && o.oco != 0) same = current.stock_orders.at(static_cast<std::size_t>(o.oco - 1)).request == named.oco.front();
    }
    CommandResult replay{!same ? failure(Reason::DUPLICATE_CLIENT_ID, "Client order ID already used with other terms")
                               : o.status == OrderStatus::Rejected ? o.reason : Decision{},
                         o.id, current.version};
    replay.replayed = same;
    return replay;
  }
  return impl_->transact(time, "stock_order", [&](State& s, Events& events) {
    if (feed_price) {
      const auto prior = s.stock_marks.find(request.symbol);
      if (prior == s.stock_marks.end() || prior->second.time <= feed_price->time)
        s.stock_marks[request.symbol] = {split_basis(s, request.symbol, feed_price->time, feed_price->price), feed_price->time};
    }
    auto [d, id] = accept_stock_order(s, std::move(request), OrderRole::Normal, 0, events);
    if (!d.ok()) return CommandResult{d, id ? std::optional<OrderId>(id) : std::nullopt, 0};
    const auto partner = s.stock_orders.at(static_cast<std::size_t>(id - 1)).oco;
    work_stock_orders(s, events, id);
    if (partner && s.stock_orders.at(static_cast<std::size_t>(partner - 1)).open()) work_stock_orders(s, events, partner);
    const auto& accepted = s.stock_orders.at(static_cast<std::size_t>(id - 1));
    return CommandResult{accepted.status == OrderStatus::Rejected ? accepted.reason : Decision{}, id, 0};
  });
}
StockPreview TradingSession::preview_stock_order(const StockOrderRequest& request, Timestamp time,
    std::optional<StockPrice> price, Decision rejection) const {
  if (impl_->stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
  auto copy = std::make_unique<Impl>();
  copy->state = prepared(impl_->state, time, {});
  copy->actor = impl_->actor;
  copy->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(copy->state));
  const auto base = measure(copy->state).equity;
  StockPreview result;
  result.current = what_if_account(copy->state, base, {});
  TradingSession trial(std::move(copy));
  result.decision = rejection.ok() ? trial.submit_stock_order(request, time, price).decision : rejection;
  result.price = stock_price(trial.impl_->state, request.symbol);
  result.after = what_if_account(trial.impl_->state, base, {});
  return result;
}
CommandResult TradingSession::cancel_stock_order(OrderId id, Timestamp time) {
  return impl_->transact(time, "stock_cancel", [&](State& s, Events& events) {
    if (id == 0 || id > s.stock_orders.size()) return CommandResult{failure(Reason::UNKNOWN_ORDER, "No such share order"), id, 0};
    const auto& o = s.stock_orders.at(static_cast<std::size_t>(id - 1));
    if (!o.open()) return CommandResult{failure(Reason::ORDER_TERMINAL, "The share order has already ended"), id, 0};
    if (s.config.rules.require_stop_loss && o.role == OrderRole::StopLoss && shares_held(s, o.request.symbol) != 0)
      return CommandResult{failure(Reason::STOP_REQUIRED, "Close the protected shares before cancelling their required stop"), id, 0};
    end_stock_order(s, id, OrderStatus::Cancelled, failure(Reason::USER_CANCEL, "Cancelled by caller"), events);
    return CommandResult{{}, id, 0};
  });
}
StockPreview TradingSession::preview_trade_stock(const std::string& symbol, Quantity signed_shares, Timestamp time,
    std::optional<StockPrice> price, Decision rejection) const {
  if (impl_->stopped) throw TradingError(Reason::JOURNAL_IO, "Trading stopped after journal failure; recover first");
  auto copy = std::make_unique<Impl>();
  copy->state = prepared(impl_->state, time, {});
  copy->actor = impl_->actor;
  copy->snapshot = std::make_shared<TradingSnapshot>(snapshot_of(copy->state));
  const auto base = measure(copy->state).equity;
  StockPreview result;
  result.current = what_if_account(copy->state, base, {});
  TradingSession trial(std::move(copy));
  result.decision = rejection.ok() ? trial.trade_stock(symbol, signed_shares, time, price).decision : rejection;
  result.price = stock_price(trial.impl_->state, symbol);
  result.after = what_if_account(trial.impl_->state, base, {});
  return result;
}
}  // namespace openport::trading
