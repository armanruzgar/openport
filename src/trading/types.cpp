#include "openport/trading/types.hpp"
#include "openport/trading/events.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace openport::trading {
std::string_view to_string(Reason reason) noexcept {
#define CASE(name) case Reason::name: return #name
  switch (reason) {
    CASE(NONE); CASE(INVALID_MONEY); CASE(ARITHMETIC_OVERFLOW); CASE(INVALID_CONTRACT);
    CASE(AMERICAN_UNSUPPORTED); CASE(NONSTANDARD_UNSUPPORTED); CASE(ROOT_UNSUPPORTED);
    CASE(UNKNOWN_CONTRACT); CASE(INVALID_ORDER); CASE(DUPLICATE_CLIENT_ID);
    CASE(INVALID_TICK); CASE(INVALID_QUOTE); CASE(STALE_QUOTE); CASE(MISSING_VALUATION);
    CASE(MAX_ORDER_CONTRACTS); CASE(PRICE_BAND); CASE(DELTA_LIMIT); CASE(VEGA_LIMIT);
    CASE(DAILY_LOSS); CASE(KILL_SWITCH); CASE(RISK_CHANGED); CASE(IOC_REMAINDER);
    CASE(SCALING_LIMIT);
    CASE(USER_CANCEL); CASE(PLAYBOOK_TIME_STOP); CASE(PLAYBOOK_TRAILING_STOP); CASE(PLAYBOOK_DTE_STOP); CASE(PLAYBOOK_DAYS_IN_TRADE_STOP); CASE(SESSION_CLOSED); CASE(FEED_STALLED); CASE(DAY_END); CASE(EXPIRED);
    CASE(AWAITING_SETTLEMENT); CASE(INVALID_SETTLEMENT); CASE(ALREADY_SETTLED);
    CASE(UNKNOWN_ORDER); CASE(ORDER_TERMINAL); CASE(INVALID_LIMITS); CASE(INVALID_TIME);
    CASE(INVALID_SCENARIO); CASE(INVALID_REASON); CASE(JOURNAL_IO); CASE(JOURNAL_CORRUPT); CASE(JOURNAL_LOCKED);
    CASE(EVALUATION_CLOSED); CASE(BUYING_POWER); CASE(BUY_ONLY); CASE(EXPIRY_CUTOFF);
    CASE(ACCOUNT_RESET); CASE(INVALID_RULES); CASE(OCO_FILLED); CASE(POSITION_CLOSED);
    CASE(PAYOUT_UNAVAILABLE); CASE(PAYOUT_NOT_ELIGIBLE); CASE(INVALID_PAYOUT); CASE(PLAN_LOCKED);
    CASE(LIMIT_ONLY); CASE(INVALID_NOTE); CASE(UNKNOWN_TRADE); CASE(DEFINED_RISK); CASE(MARKET_HALTED);
    CASE(SOFT_FLOOR); CASE(TRADE_LIMIT); CASE(COOLDOWN); CASE(PROFIT_LOCK);
    CASE(RUN_ENDED); CASE(INVALID_GROUP); CASE(GTD_END);
    CASE(PROFIT_TARGET); CASE(DRAWDOWN_FLOOR); CASE(DAILY_LOSS_LIMIT); CASE(MIN_TRADING_DAYS);
    CASE(MIN_PROFITABLE_DAYS); CASE(CONSISTENCY); CASE(ACCOUNT_TYPE); CASE(INVALID_ALERT); CASE(UNKNOWN_ALERT);
    CASE(MAX_CONTRACTS_HELD); CASE(STOP_REQUIRED); CASE(MAX_TRADE_RISK);
    CASE(TIME_LIMIT); CASE(INACTIVITY); CASE(INSTRUMENT_NOT_ALLOWED); CASE(OUTSIDE_PLAN_HOURS); CASE(FLAT_TIME); CASE(OVERNIGHT_HOLD);
    CASE(TRADE_CONSISTENCY); CASE(MIN_TRADES); CASE(MIN_HOLD); CASE(MICROSCALPING);
    CASE(HEDGING); CASE(COUNTER_POSITION); CASE(MAX_VOLUME_SHARE);
    CASE(NEWS_BLACKOUT); CASE(HOLD_RESTRICTED); CASE(RESET_LIMIT);
    CASE(GAMMA_LIMIT); CASE(THETA_LIMIT); CASE(GRID_LOSS_LIMIT); CASE(SHORT_VEGA_LIMIT); CASE(CORPORATE_ACTION);
  }
#undef CASE
  return "UNKNOWN";
}
namespace {
void append_order_symbols(const OrderRequest& request, std::vector<std::string>& symbols) {
  if (!multi_leg(request)) {
    symbols.push_back(request.symbol);
    return;
  }
  for (const auto& leg : request.legs) symbols.push_back(leg.symbol);
}
void append_chain_symbols(const OrderRequest& request, std::vector<std::string>& symbols) {
  append_order_symbols(request, symbols);
  for (const auto& next : request.then) append_chain_symbols(next, symbols);
  for (const auto& other : request.oco) append_chain_symbols(other, symbols);
}
}  // namespace
std::vector<std::string> order_symbols(const OrderRequest& request) {
  std::vector<std::string> symbols;
  append_order_symbols(request, symbols);
  return symbols;
}
std::vector<std::string> chain_symbols(const OrderRequest& request) {
  std::vector<std::string> symbols;
  append_chain_symbols(request, symbols);
  return symbols;
}
void validate_stock_order(const StockOrderRequest& r) {
  const auto refuse = [](const std::string& message) { throw TradingError(Reason::INVALID_ORDER, message); };
  if (r.client_order_id.empty() || r.client_order_id.size() > 64) refuse("A share order needs a client_order_id of 1-64 bytes");
  if (r.symbol.empty() || r.symbol.size() > 16 ||
      !std::all_of(r.symbol.begin(), r.symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
    refuse("Use an uppercase stock or ETF symbol");
  if (r.shares <= 0 || r.shares > 10'000'000) refuse("Trade a positive number of shares, at most 10,000,000");
  if (r.tif != TimeInForce::Day && r.tif != TimeInForce::Gtc) refuse("Share orders are DAY or GTC");
  const bool limit = r.type == StockOrderType::Limit || r.type == StockOrderType::StopLimit;
  const bool stop = r.type == StockOrderType::Stop || r.type == StockOrderType::StopLimit;
  const auto price = [&](const std::optional<Money>& p, const char* name) {
    if (p && (*p <= Money{} || p->micros() % 10'000 != 0)) refuse(std::string(name) + " must be a positive price in whole cents");
  };
  if (limit != r.limit_price.has_value()) refuse(limit ? "This order type needs a limit_price" : "Only limit and stop-limit orders take a limit_price");
  if (stop != r.stop_price.has_value()) refuse(stop ? "This order type needs a stop_price" : "Only stop and stop-limit orders take a stop_price");
  price(r.limit_price, "limit_price"); price(r.stop_price, "stop_price");
  price(r.stop_loss, "stop_loss"); price(r.take_profit, "take_profit");
  if (r.extended_hours && r.type != StockOrderType::Limit) refuse("Extended hours take limit orders only");
  // A bracket's stop is on the losing side of the entry and its target on the winning side.
  const bool buy = r.side == Side::Buy;
  if (const auto entry = r.limit_price ? r.limit_price : r.stop_price) {
    if (r.stop_loss && (buy ? *r.stop_loss >= *entry : *r.stop_loss <= *entry))
      refuse(buy ? "A buy's stop_loss is below its entry price" : "A sell's stop_loss is above its entry price");
    if (r.take_profit && (buy ? *r.take_profit <= *entry : *r.take_profit >= *entry))
      refuse(buy ? "A buy's take_profit is above its entry price" : "A sell's take_profit is below its entry price");
  }
  if (r.stop_loss && r.take_profit && (buy ? *r.stop_loss >= *r.take_profit : *r.stop_loss <= *r.take_profit))
    refuse("A bracket's stop_loss and take_profit are on either side of the entry");
  if (r.oco.size() > 1) refuse("A share order takes at most one oco partner");
  for (const auto& other : r.oco) {
    if (!other.oco.empty() || other.stop_loss || other.take_profit) refuse("An oco partner takes no oco or bracket of its own");
    if (other.symbol != r.symbol) refuse("An oco partner trades the same symbol");
    auto named = other;
    named.client_order_id = r.client_order_id + ":oco";
    validate_stock_order(named);
  }
}
Decision eligible(const md::OptionContract& c, const Deliverable* adjusted) {
  const auto conventions = md::conventions_for_root(c.root);
  if (c.style == pricing::ExerciseStyle::American && conventions.style != pricing::ExerciseStyle::American)
    return {Reason::AMERICAN_UNSUPPORTED, "American exercise is not listed on this European-style root", {}, {}, {}};
  // An adjusted series trades once its deliverable is known: shares, and any cash in
  // lieu, of an underlying that delivers shares.
  if (!c.standard && !adjusted)
    return {Reason::NONSTANDARD_UNSUPPORTED, "An adjusted series needs its deliverable (shares and cash) to trade", {}, {}, {}};
  if (adjusted && (adjusted->shares < 1 || adjusted->shares > 100'000 || adjusted->cash < Money{} ||
                   md::is_index_underlying(c.underlying) || c.style != pricing::ExerciseStyle::American))
    return {Reason::NONSTANDARD_UNSUPPORTED, "Adjusted deliverables are modelled for share-settled American options only", {}, {}, {}};
  // American equity, ETF and OEX options trade too. Equity and ETF options deliver
  // shares when exercised or assigned; OEX settles in cash.
  constexpr std::string_view roots[] = {"SPX", "SPXW", "XSP", "NDX", "NDXP", "RUT", "RUTW", "XND", "MRUT", "DJX", "VIX", "VIXW"};
  if (c.style == pricing::ExerciseStyle::European && std::find(std::begin(roots), std::end(roots), c.root) == std::end(roots))
    return {Reason::ROOT_UNSUPPORTED, "Root is outside the cash-settled European index allowlist", {}, {}, {}};
  if (c.style != conventions.style || c.multiplier != 100 ||
      c.underlying != conventions.underlying ||
      (c.settlement != md::Settlement::AM && c.settlement != md::Settlement::PM) ||
      (c.type != pricing::OptionType::Call && c.type != pricing::OptionType::Put) ||
      !md::valid_date(c.expiry) || c.expiry.year < 2000 || c.expiry.year > 2099 ||
      !std::isfinite(c.strike) || c.strike <= 0 || c.strike > 99999.999 ||
      std::abs(c.strike * 1000 - std::round(c.strike * 1000)) > 1e-6 ||
      c.expiry_time() == md::kInvalidTimestamp)
    return {Reason::INVALID_CONTRACT, "Invalid standard contract terms or noncanonical OSI strike", {}, {}, {}};
  return {};
}
Money tick_size(std::string_view root, Money price) {
  const bool below = price < Money::from_micros(3'000'000);
  if (root == "SPX" || root == "SPXW" || root == "NDX" || root == "NDXP" || root == "RUT" || root == "RUTW" || root == "OEX")
    return Money::from_micros(below ? 50'000 : 100'000);
  if (root == "XSP" || root == "MRUT") return Money::from_micros(below ? 10'000 : 50'000);
  // Equity and ETF classes: SPY, QQQ and IWM quote in pennies at every price;
  // the others follow the penny-pilot tiers.
  if (!md::is_index_underlying(md::conventions_for_root(root).underlying) &&
      root != "SPY" && root != "QQQ" && root != "IWM")
    return Money::from_micros(below ? 10'000 : 50'000);
  return Money::from_micros(10'000);
}
bool alert_metric(AlertScope scope, std::string_view metric) {
  static const std::map<AlertScope, std::vector<std::string_view>> metrics{
      {AlertScope::Contract, {"bid", "ask", "mark", "iv", "delta", "gamma", "theta", "vega"}},
      {AlertScope::Spread, {"mark"}},
      {AlertScope::Underlying, {"price", "iv30", "iv7", "term_ratio"}},
      {AlertScope::Account, {"equity", "day_pnl", "unrealised", "floor_room", "buying_power", "dollar_delta", "vega", "theta", "dollar_gamma_1pct",
          "delta_utilisation", "vega_utilisation", "grid_loss", "floor_touch_probability"}}};
  const auto it = metrics.find(scope);
  return it != metrics.end() && std::find(it->second.begin(), it->second.end(), metric) != it->second.end();
}
void validate_alert(const AlertSpec& spec) {
  const auto invalid = [](std::string message) { throw TradingError(Reason::INVALID_ALERT, std::move(message)); };
  const auto& c = spec.condition;
  if (spec.label.size() > 100 || std::any_of(spec.label.begin(), spec.label.end(), [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
    invalid("An alert's label has at most 100 bytes and no control characters");
  if (!alert_metric(c.scope, c.metric)) invalid("Unknown metric for this alert's scope");
  if (c.direction != TriggerDirection::AtOrAbove && c.direction != TriggerDirection::AtOrBelow) invalid("Unknown alert direction");
  const auto osi = [](const std::string& symbol) {
    const auto contract = md::parse_osi(symbol);
    return contract && contract->osi_symbol() == symbol;
  };
  switch (c.scope) {
    case AlertScope::Contract:
      if (!osi(c.symbol) || !c.legs.empty()) invalid("A contract alert names one canonical padded OSI and no legs");
      break;
    case AlertScope::Spread: {
      std::set<std::string> seen;
      if (!c.symbol.empty() || c.legs.size() < 2 || c.legs.size() > kMaxLegs) invalid("A spread alert takes two to four legs and no symbol");
      for (const auto& leg : c.legs)
        if (!osi(leg.symbol) || !seen.insert(leg.symbol).second || leg.ratio < 1 || leg.ratio > kMaxRatio ||
            (leg.side != Side::Buy && leg.side != Side::Sell))
          invalid("Each spread leg needs its own canonical OSI, a side and a ratio from 1 to 10");
      break;
    }
    case AlertScope::Underlying: {
      Trigger shape{TriggerSource::Underlying, c.direction, Money::from_micros(1)};
      shape.symbol = c.symbol;
      if (c.symbol.empty() || !valid_trigger(shape) || !c.legs.empty()) invalid("An underlying alert names its underlying and no legs");
      break;
    }
    case AlertScope::Account:
      if (!c.symbol.empty() || !c.legs.empty()) invalid("An account alert names no symbol or legs");
      break;
  }
}
bool valid_trigger(const Trigger& t) {
  if (t.direction != TriggerDirection::AtOrBelow && t.direction != TriggerDirection::AtOrAbove) return false;
  // An underlying symbol: 1-12 uppercase letters, digits, dots or underscores.
  const auto symbol_ok = [](std::string_view symbol) {
    return symbol.size() <= 12 && std::all_of(symbol.begin(), symbol.end(), [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
    });
  };
  switch (t.source) {
    case TriggerSource::Option:
      return t.level > Money{} && t.symbol.empty() && t.study.empty() && t.minute == 0;
    case TriggerSource::Combo:
      return t.symbol.empty() && t.study.empty() && t.minute == 0;
    case TriggerSource::Underlying:
      return t.level > Money{} && symbol_ok(t.symbol) && t.study.empty() && t.minute == 0;
    case TriggerSource::Study:
      return t.level > Money{} && symbol_ok(t.symbol) && t.minute == 0 &&
             std::find(std::begin(kTriggerStudies), std::end(kTriggerStudies), t.study) != std::end(kTriggerStudies);
    case TriggerSource::Time:
      return t.level == Money{} && t.symbol.empty() && t.study.empty() && t.minute >= 0 && t.minute < 24 * 60;
  }
  return false;
}
std::optional<WalkStep> next_walk(const Order& o) {
  if (!o.open() || !o.request.walk || !o.request.limit_price) return std::nullopt;
  const auto& w = *o.request.walk;
  const auto current = *o.request.limit_price;
  const bool up = multi_leg(o.request) || o.request.side == Side::Buy;
  if (w.seconds < 1 || w.seconds > 3600 || w.step <= Money{} || (up ? current >= w.limit : current <= w.limit))
    return std::nullopt;
  const auto interval = w.seconds * md::kNanosPerSecond;
  const auto since = std::max(o.accepted_at, o.walked_at);
  if (since > std::numeric_limits<Timestamp>::max() - interval) return std::nullopt;
  if (o.day_end > 0 && since + interval >= o.day_end) return std::nullopt;
  auto price = w.limit;
  if (w.step < (up ? w.limit - current : current - w.limit)) price = up ? current + w.step : current - w.step;
  if (!multi_leg(o.request)) {
    const auto contract = md::parse_osi(o.request.symbol);
    if (!contract) return std::nullopt;
    const auto tick = tick_size(contract->root, price).micros();
    if (const auto rest = price.micros() % tick; rest != 0)
      price = Money::from_micros(price.micros() - rest + (up ? tick : 0));
    price = up ? std::min(price, w.limit) : std::max(price, w.limit);
  }
  return WalkStep{since + interval, price};
}
bool valid_quote(const QuoteObservation& q) {
  return q.observation > 0 && q.bid && q.ask && *q.bid > Money{} && *q.ask >= *q.bid &&
         q.bid_size > 0 && q.ask_size > 0;
}
bool markable_quote(const QuoteObservation& q) {
  return valid_quote(q) ||
         (q.observation > 0 && !q.bid && q.bid_size == 0 && q.ask && *q.ask > Money{} && q.ask_size > 0);
}
bool valid_valuation(const Valuation& v) {
  return v.valid && std::isfinite(v.delta) && std::isfinite(v.gamma) &&
         std::isfinite(v.vega) && std::isfinite(v.theta) && std::isfinite(v.spot) &&
         std::isfinite(v.forward) && std::isfinite(v.discount) && std::isfinite(v.years) &&
         std::isfinite(v.smile_iv) && v.spot > 0 && v.forward > 0 && v.discount > 0 &&
         v.years >= 0 && v.smile_iv > 0;
}
void validate_limits(const Limits& l) {
  auto exposure_ok = [](const ExposureLimits& e) {
    bool valid = std::isfinite(e.dollar_delta) && e.dollar_delta >= 0 && std::isfinite(e.vega) && e.vega >= 0;
    for (const auto& cap : {e.dollar_gamma_1pct, e.theta, e.worst_grid_loss, e.short_vega})
      valid &= !cap || (std::isfinite(*cap) && *cap >= 0);
    return valid;
  };
  bool valid = l.max_order_contracts > 0 && l.price_band_absolute >= Money{} &&
               std::isfinite(l.price_band_relative) && l.price_band_relative >= 0 &&
               l.price_band_relative <= 100 && l.max_daily_loss >= Money{} &&
               l.max_quote_age >= 0 && l.max_valuation_age >= 0 &&
               exposure_ok(l.aggregate) && exposure_ok(l.per_underlying);
  for (const auto& [name, limit] : l.underlying_overrides) valid &= !name.empty() && exposure_ok(limit);
  for (const auto& [name, limit] : l.tag_overrides)
    valid &= name.size() <= 32 && name.find(',') == std::string::npos &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127 || (c >= 'A' && c <= 'Z'); });
  for (const auto* scopes : {&l.tag_overrides, &l.playbook_overrides}) {
    valid &= scopes->size() <= 100;
    for (const auto& [name, limit] : *scopes) valid &= !name.empty() && name.size() <= 100 && exposure_ok(limit);
  }
  if (!valid) throw TradingError(Reason::INVALID_LIMITS, "Limits must be finite and nonnegative; order size must be positive");
}
void validate_time_rules(const AccountRules& r) {
  if (r.flat_time && (*r.flat_time < 0 || *r.flat_time >= 1440 || *r.flat_time >= r.day_end_minutes))
    throw TradingError(Reason::INVALID_RULES, "flat_time must be HH:MM New York time from 00:00 to 23:59, before day_end");
  if (r.time_limit_days < 0 || r.time_limit_days > 366 || r.inactivity_days < 0 || r.inactivity_days > 366 ||
      (r.phase == Phase::Funded && r.time_limit_days != 0))
    throw TradingError(Reason::INVALID_RULES, "time_limit_days and inactivity_days must be 0-366 calendar days; funded plans require time_limit_days = 0");
  std::set<std::string> symbols;
  for (const auto& symbol : r.underlyings)
    if (r.underlyings.size() > 32 || symbol.empty() || symbol.size() > 12 || !symbols.insert(symbol).second ||
        !std::all_of(symbol.begin(), symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw TradingError(Reason::INVALID_RULES, "underlyings must contain up to 32 distinct symbols of 1-12 uppercase letters, digits or dots");
  if (r.trading_start.has_value() != r.trading_end.has_value() ||
      (r.trading_start && (*r.trading_start < 0 || *r.trading_end > 24 * 60 || *r.trading_start >= *r.trading_end)))
    throw TradingError(Reason::INVALID_RULES, "Set both trading_start and trading_end, between 00:00 and 24:00 New York, with start before end, or neither");
}
bool same_program_rules(AccountRules left, const AccountRules& right) {
  auto execution = std::move(left);
  execution.slippage_ticks = right.slippage_ticks;
  execution.fill_latency_ms = right.fill_latency_ms;
  execution.impact_ticks = right.impact_ticks;
  execution.inside_fill_percent = right.inside_fill_percent;
  // Financing and pin risk change P&L without a trade, so they are program
  // rules: an account that overrides them no longer follows the preset.
  execution.fees = right.fees;
  // The account's margin is the trader's broker's, not the plan's.
  execution.margin = right.margin;
  execution.account_type = right.account_type;
  execution.house_margin_percent = right.house_margin_percent;
  execution.pm_vol_shock = right.pm_vol_shock;
  execution.evaluation_fee = right.evaluation_fee;
  execution.reset_fee = right.reset_fee;
  execution.activation_fee = right.activation_fee;
  execution.max_resets = right.max_resets;
  return execution == right;
}
void validate_rules(const AccountRules& r) {
  if (r.pin_risk_seed < 0 || r.pin_risk_seed > 2147483647)
    throw TradingError(Reason::INVALID_RULES, "pin_risk_seed must be 0-2147483647");
  if (r.cash_credit_rate_bps < 0 || r.cash_credit_rate_bps > 100000 ||
      r.margin_debit_rate_bps < 0 || r.margin_debit_rate_bps > 100000 ||
      r.stock_borrow_rate_bps < 0 || r.stock_borrow_rate_bps > 100000)
    throw TradingError(Reason::INVALID_RULES, "Annual financing rates must be 0-100000 basis points");
  if (r.size_scaling) {
    const auto& v = *r.size_scaling;
    if (r.phase != Phase::Funded || v.profit_percent < 1 || v.profit_percent > 100 ||
        v.payouts < 0 || v.payouts > 100 || v.days < 1 || v.days > 366 ||
        v.increase_percent < 1 || v.increase_percent > 100 || v.max_balance <= Money{})
      throw TradingError(Reason::INVALID_RULES, "Account size scaling requires funded rules, profit and increase 1-100%, "
          "0-100 payouts, 1-366 days and a positive maximum balance at least the starting balance");
  }
  if (r.scaling.size() > 16 || (!r.scaling.empty() && r.scaling.front().profit != Money{}))
    throw TradingError(Reason::INVALID_RULES, "Scaling takes at most 16 steps, starting at zero profit");
  for (std::size_t i = 0; i < r.scaling.size(); ++i) {
    const auto& step = r.scaling[i];
    if (step.contracts < 1 || step.contracts > 10'000 ||
        (i > 0 && (step.profit <= r.scaling[i - 1].profit || step.contracts < r.scaling[i - 1].contracts)))
      throw TradingError(Reason::INVALID_RULES, "Scaling profits must strictly increase and contract limits must be non-decreasing, from 1 to 10000");
  }
  if (r.evaluation_fee < Money{} || r.reset_fee < Money{} || r.activation_fee < Money{} ||
      r.max_resets < 0 || r.max_resets > 1'000'000)
    throw TradingError(Reason::INVALID_RULES, "Program fees must be nonnegative and max_resets from 0 to 1000000 (0 is unlimited)");
  const auto& p = r.payouts;
  if (p.buffer < Money{} || p.buffer_payouts < 0 || p.buffer_payouts > 100)
    throw TradingError(Reason::INVALID_RULES, "The payout buffer must be nonnegative and buffer_payouts from 0 to 100");
  if (p.consistency_percents.size() > 64 ||
      !std::all_of(p.consistency_percents.begin(), p.consistency_percents.end(), [](auto percent) { return percent >= 1 && percent <= 100; }))
    throw TradingError(Reason::INVALID_RULES, "Payout consistency takes at most 64 percentages, each from 1 to 100");
  const bool payouts_ok = p.qualifying_profit >= Money{} && p.qualifying_days >= 0 && p.qualifying_days <= 366 &&
      p.withdrawal_percent >= 0 && p.withdrawal_percent <= 100 && p.split_percent >= 0 && p.split_percent <= 100 &&
      p.minimum >= Money{} && p.caps.size() <= 64 &&
      std::all_of(p.caps.begin(), p.caps.end(), [](Money cap) { return cap > Money{}; });
  if (r.profit_target < Money{} || r.max_drawdown < Money{} || r.expiry_cutoff < 0 ||
      r.expiry_cutoff >= md::kNanosPerDay || r.plan.size() > 64 || r.lock_balance < Money{} || !payouts_ok ||
      (r.drawdown_mode != DrawdownMode::Intraday && r.drawdown_mode != DrawdownMode::EndOfDay &&
       r.drawdown_mode != DrawdownMode::Static) ||
      (r.phase != Phase::Evaluation && r.phase != Phase::Funded && r.phase != Phase::Verification) ||
      r.slippage_ticks < 0 || r.slippage_ticks > 10 ||
      r.fill_latency_ms < 0 || r.fill_latency_ms > 60'000 || r.impact_ticks < 0 || r.impact_ticks > 10 ||
      r.inside_fill_percent < 0 || r.inside_fill_percent > 100 ||
      (r.margin != MarginMode::Strategy && r.margin != MarginMode::Portfolio) ||
      (r.account_type != AccountType::Margin && r.account_type != AccountType::Cash && r.account_type != AccountType::Ira) ||
      r.house_margin_percent < 0 || r.house_margin_percent > 400 || r.pm_vol_shock < 0 || r.pm_vol_shock > 50 ||
      (r.account_type != AccountType::Margin && (r.margin != MarginMode::Strategy || !r.buying_power)) ||
      (r.phase == Phase::Funded && (r.profit_target > Money{} || p.qualifying_days < 1)))
    throw TradingError(Reason::INVALID_RULES,
        "Rule amounts must be nonnegative, percentages 0-100, caps positive, the expiry cutoff under one day and the plan name "
        "at most 64 bytes; slippage and impact are 0-10 ticks, inside fills 0-100%, fill latency is 0-60000 ms and margin is strategy or portfolio; "
        "house margin is 0-400%, the portfolio vol shock 0-50 points, and a cash or IRA account uses strategy margin and "
        "enforces buying power; a funded phase has no profit target and at least one qualifying day");
  if (r.drawdown_mode == DrawdownMode::Static && (r.lock_at_start || r.lock_balance > Money{}))
    throw TradingError(Reason::INVALID_RULES, "A static floor never trails, so it takes no lock");
  if (r.lock_at_start && r.lock_balance > Money{})
    throw TradingError(Reason::INVALID_RULES, "The floor locks at the starting balance or at lock_balance, not both");
  if (r.daily_loss_limit < Money{} || r.profitable_day_profit < Money{} ||
      (r.profit_basis != ProfitBasis::Equity && r.profit_basis != ProfitBasis::Balance) ||
      (r.daily_loss_basis != DailyLossBasis::Equity && r.daily_loss_basis != DailyLossBasis::Balance &&
       r.daily_loss_basis != DailyLossBasis::Higher && r.daily_loss_basis != DailyLossBasis::Peak) ||
      (r.daily_loss_action != BreachAction::Lock && r.daily_loss_action != BreachAction::Fail) ||
      (r.consistency_basis != ConsistencyBasis::Total && r.consistency_basis != ConsistencyBasis::PositiveDays) ||
      r.consistency_percent < 0 || r.consistency_percent > 100 || r.min_trading_days < 0 || r.min_trading_days > 366 ||
      r.min_profitable_days < 0 || r.min_profitable_days > 366)
    throw TradingError(Reason::INVALID_RULES,
        "The daily loss limit and profitable-day profit must be nonnegative, the consistency percentage 0-100 and minimum "
        "days 0-366");
  if (r.min_hold_seconds < 0 || r.min_hold_seconds > 3600 ||
      r.microscalp_seconds < 0 || r.microscalp_seconds > 3600 ||
      r.microscalp_percent < 0 || r.microscalp_percent > 100 ||
      (r.microscalp_percent > 0 && r.microscalp_seconds == 0))
    throw TradingError(Reason::INVALID_RULES, "Hold seconds must be 0-3600; microscalping needs seconds 1-3600 with percent 1-100; percent zero disables");
  if (r.min_trades < 0 || r.min_trades > 10000)
    throw TradingError(Reason::INVALID_RULES, "Minimum trades must be from 0 to 10000");
  if (r.trade_consistency_percent < 0 || r.trade_consistency_percent > 100)
    throw TradingError(Reason::INVALID_RULES, "Trade consistency must be a whole percent from 0 to 100");
  if (r.day_end_minutes < 16 * 60 + 15 || r.day_end_minutes > 24 * 60)
    throw TradingError(Reason::INVALID_RULES, "The trading day ends between 16:15 and 24:00 New York time");
  if (r.max_volume_percent < 0 || r.max_volume_percent > 100)
    throw TradingError(Reason::INVALID_RULES, "Maximum volume percent must be an integer from 0 to 100");
  if (r.max_contracts_held < 0 || r.max_contracts_held > 100000)
    throw TradingError(Reason::INVALID_RULES, "The contracts held cap must be 0 to 100000; zero disables it");
  if (r.max_trade_risk < Money{} || r.max_trade_risk_percent < 0 || r.max_trade_risk_percent > 100)
    throw TradingError(Reason::INVALID_RULES, "Maximum trade risk must be nonnegative and its percentage 0 to 100");
  validate_time_rules(r);
  validate_event_rules(r);
  if (r.fees) {
    const auto& f = *r.fees;
    const auto amount = [](Money value) { return value >= Money{} && value <= Money::from_micros(1'000'000'000); };
    const bool roots = f.index.size() <= 16 && std::all_of(f.index.begin(), f.index.end(), [&](const auto& item) {
      return !item.first.empty() && item.first.size() <= 6 && amount(item.second) &&
             std::all_of(item.first.begin(), item.first.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); });
    });
    if (!roots || !amount(f.open) || !amount(f.close) || !amount(f.leg_cap) || !amount(f.clearing) ||
        !amount(f.regulatory) || !amount(f.exercise))
      throw TradingError(Reason::INVALID_RULES,
          "Fee schedule amounts must be $0 to $1,000, with at most 16 index roots of 1 to 6 uppercase letters or digits");
  }
}
}  // namespace openport::trading
