// The paper-trading API's request parsers: strict JSON bodies and query strings,
// orders and share orders, limits, triggers, plan rules and commands.

#include "paper_internal.hpp"

namespace openport::server {
namespace paper_internal {
namespace {
constexpr std::size_t kMaxWhatIfCandidates = 6;
constexpr std::size_t kMaxWhatIfOrders = 4;
}  // namespace

void fields(const json& object, std::initializer_list<std::string_view> required,
             std::initializer_list<std::string_view> optional) {
  if (!object.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (auto field : required) if (!object.contains(field)) throw std::invalid_argument("Missing field: " + std::string(field));
  for (auto it = object.begin(); it != object.end(); ++it)
    if (std::find(required.begin(), required.end(), it.key()) == required.end() &&
        std::find(optional.begin(), optional.end(), it.key()) == optional.end())
      throw std::invalid_argument("Unknown field: " + it.key());
}
std::string string_field(const json& j, const char* key) {
  if (!j.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
  return j.at(key).get<std::string>();
}
std::int64_t integer_field(const json& j, const char* key) {
  const auto& value = j.at(key);
  if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > std::uint64_t(INT64_MAX)))
    throw std::invalid_argument(std::string(key) + " must be a signed 64-bit integer");
  return value.get<std::int64_t>();
}
namespace {
double number_field(const json& j, const char* key) {
  if (!j.at(key).is_number()) throw std::invalid_argument(std::string(key) + " must be a number");
  const double value = j.at(key).get<double>();
  if (!std::isfinite(value)) throw std::invalid_argument(std::string(key) + " must be finite");
  return value;
}
}  // namespace
std::uint64_t identifier(std::string_view text) {
  std::uint64_t id = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
  if (text.empty() || error != std::errc{} || end != text.data() + text.size())
    throw std::invalid_argument("Expected an unsigned decimal ID string");
  return id;
}
Money decimal_field(const json& j, const char* key) {
  try { return Money::parse(string_field(j, key)); }
  catch (const TradingError& e) { throw std::invalid_argument(std::string(key) + ": " + e.what()); }
}
namespace {
std::string symbol_field(const json& j) {
  const auto symbol = string_field(j, "symbol");
  const auto contract = md::parse_osi(symbol);
  if (!contract || contract->osi_symbol() != symbol) throw std::invalid_argument("symbol must be a canonical padded OSI");
  return symbol;
}
ExposureLimits parse_exposure(const json& j) {
  fields(j, {"dollar_delta", "vega"}, {"dollar_gamma_1pct", "theta", "worst_grid_loss", "short_vega"});
  ExposureLimits result{number_field(j, "dollar_delta"), number_field(j, "vega")};
  const auto cap = [&](const char* name) -> std::optional<double> {
    return !j.contains(name) || j.at(name).is_null() ? std::nullopt : std::optional(number_field(j, name));
  };
  result.dollar_gamma_1pct = cap("dollar_gamma_1pct"); result.theta = cap("theta");
  result.worst_grid_loss = cap("worst_grid_loss"); result.short_vega = cap("short_vega");
  return result;
}
Limits parse_limits(const json& j) {
  fields(j, {"max_order_contracts", "price_band_absolute", "price_band_relative", "aggregate", "per_underlying",
             "max_daily_loss", "max_quote_age_seconds", "max_valuation_age_seconds"}, {"underlying_overrides", "tag_overrides", "playbook_overrides"});
  Limits limits;
  limits.max_order_contracts = integer_field(j, "max_order_contracts");
  limits.price_band_absolute = decimal_field(j, "price_band_absolute");
  limits.price_band_relative = number_field(j, "price_band_relative");
  limits.aggregate = parse_exposure(j.at("aggregate"));
  limits.per_underlying = parse_exposure(j.at("per_underlying"));
  if (j.contains("underlying_overrides")) {
    const auto& overrides = j.at("underlying_overrides");
    if (!overrides.is_object()) throw std::invalid_argument("underlying_overrides must be an object");
    for (const auto& [symbol, caps] : overrides.items()) {
      if (symbol.empty() || symbol.size() > 16 || !std::all_of(symbol.begin(), symbol.end(), [](char c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.';
          })) throw std::invalid_argument("Override underlying must be 1-16 uppercase letters, digits or dots");
      limits.underlying_overrides[symbol] = parse_exposure(caps);
    }
  }
  for (const auto& key : {"tag_overrides", "playbook_overrides"}) {
    if (!j.contains(key)) continue;
    if (!j.at(key).is_object() || j.at(key).size() > 100) throw std::invalid_argument("Scope overrides must be an object with at most 100 entries");
    auto& target = std::string_view(key) == "tag_overrides" ? limits.tag_overrides : limits.playbook_overrides;
    for (const auto& [name, caps] : j.at(key).items()) {
      if (name.empty() || name.size() > 100 || std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw std::invalid_argument("Scope names must be 1-100 characters without controls");
      target[name] = parse_exposure(caps);
    }
  }
  limits.max_daily_loss = decimal_field(j, "max_daily_loss");
  auto age = [&](const char* key) {
    const auto seconds = integer_field(j, key);
    if (seconds > INT64_MAX / md::kNanosPerSecond || seconds < 0)
      throw TradingError(Reason::INVALID_LIMITS, "Age must be nonnegative and representable in nanoseconds");
    return seconds * md::kNanosPerSecond;
  };
  limits.max_quote_age = age("max_quote_age_seconds");
  limits.max_valuation_age = age("max_valuation_age_seconds");
  return limits;
}
Trigger parse_trigger(const json& j) {
  const auto source = j.is_object() && j.contains("source") && j.at("source").is_string() ? j.at("source").get<std::string>() : "";
  Trigger t;
  if (source == "time") {
    // A time of day: from that New York minute on, or up to it with at_or_below.
    fields(j, {"source", "at"}, {"direction"});
    const auto at = string_field(j, "at");
    const auto digit = [&](std::size_t i, char high) { return at[i] >= '0' && at[i] <= high; };
    if (at.size() != 5 || at[2] != ':' || !digit(0, '2') || !digit(1, '9') || !digit(3, '5') || !digit(4, '9') ||
        (at[0] - '0') * 10 + (at[1] - '0') > 23)
      throw std::invalid_argument("trigger at must be HH:MM New York time");
    t.source = TriggerSource::Time;
    t.minute = ((at[0] - '0') * 10 + (at[1] - '0')) * 60 + (at[3] - '0') * 10 + (at[4] - '0');
    t.direction = TriggerDirection::AtOrAbove;
  } else {
    fields(j, {"source", "direction", "level"}, source == "study" ? std::initializer_list<std::string_view>{"symbol", "study"}
                                               : source == "underlying" ? std::initializer_list<std::string_view>{"symbol", "reference", "trail"}
                                                                        : std::initializer_list<std::string_view>{"reference", "trail"});
    if (source != "option" && source != "underlying" && source != "combo" && source != "study")
      throw std::invalid_argument("trigger source must be option, combo, underlying, study or time");
    t.source = source == "option" ? TriggerSource::Option : source == "combo" ? TriggerSource::Combo
             : source == "study" ? TriggerSource::Study : TriggerSource::Underlying;
    t.level = decimal_field(j, "level");
    if (j.contains("symbol")) t.symbol = string_field(j, "symbol");
    if (source == "study") {
      if (!j.contains("study")) throw std::invalid_argument("Missing field: study");
      t.study = string_field(j, "study");
    }
  }
  if (j.contains("direction")) {
    const auto direction = string_field(j, "direction");
    if (direction != "at_or_below" && direction != "at_or_above")
      throw std::invalid_argument("trigger direction must be at_or_below or at_or_above");
    t.direction = direction == "at_or_below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove;
  }
  if (j.contains("symbol") && (t.symbol.empty() || t.symbol.size() > 12 || !std::all_of(t.symbol.begin(), t.symbol.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
      })))
    throw std::invalid_argument("trigger symbol must be an underlying of 1-12 uppercase letters, digits, dots or underscores");
  if (t.source == TriggerSource::Study &&
      std::find(std::begin(kTriggerStudies), std::end(kTriggerStudies), t.study) == std::end(kTriggerStudies))
    throw std::invalid_argument("trigger study must be iv30, iv7 or term_ratio");
  // A reference and a trail read the order's own market, not another underlying.
  if ((j.contains("reference") || j.contains("trail")) && conditional(t))
    throw std::invalid_argument("A trigger on another underlying takes no reference or trail");
  if (j.contains("reference")) {
    const auto reference = string_field(j, "reference");
    if (reference != "bid_ask" && reference != "mid" && reference != "mark")
      throw std::invalid_argument("trigger reference must be bid_ask, mid or mark");
    if (reference != "bid_ask" && t.source == TriggerSource::Underlying)
      throw std::invalid_argument("An underlying trigger reads spot; only option and combo triggers take mid or mark");
    t.reference = reference == "mid" ? TriggerReference::Mid : reference == "mark" ? TriggerReference::Mark : TriggerReference::BidAsk;
  }
  if (j.contains("trail")) {
    const auto& spec = j.at("trail");
    fields(spec, {"unit", "value"});
    const auto unit = string_field(spec, "unit");
    Trail trail;
    if (unit == "ticks") {
      if (t.source == TriggerSource::Underlying) throw std::invalid_argument("An underlying trigger trails by amount or percent");
      const auto ticks = integer_field(spec, "value");
      if (ticks < 1 || ticks > 1000) throw std::invalid_argument("A trail is 1 to 1,000 ticks");
      trail = {TrailUnit::Ticks, Money::from_micros(ticks * 1'000'000)};
    } else if (unit == "amount" || unit == "percent") {
      trail = {unit == "amount" ? TrailUnit::Amount : TrailUnit::Percent, decimal_field(spec, "value")};
      if (trail.value <= Money{} || (trail.unit == TrailUnit::Percent && trail.value >= Money::from_micros(100'000'000)))
        throw std::invalid_argument("A trail amount is positive, and a percentage above 0 and below 100");
    } else {
      throw std::invalid_argument("trail unit must be amount, percent or ticks");
    }
    t.trail = trail;
  }
  return t;
}
ExitSpec parse_exit(const json& j) {
  fields(j, {}, {"trigger", "limit_price"});
  ExitSpec exit;
  if (j.contains("trigger")) exit.trigger = parse_trigger(j.at("trigger"));
  if (j.contains("limit_price")) exit.limit_price = decimal_field(j, "limit_price");
  if (!exit.trigger && !exit.limit_price)
    throw std::invalid_argument("A bracket exit takes a trigger, a limit_price, or both for a stop-limit");
  return exit;
}
}  // namespace
bool boolean_field(const json& j, const char* key) {
  if (!j.at(key).is_boolean()) throw std::invalid_argument(std::string(key) + " must be a boolean");
  return j.at(key).get<bool>();
}
namespace {
/// One of a rule's named choices; anything else is a malformed request.
template <class E, std::size_t N>
E choice_field(const json& j, const char* key, const char* const (&names)[N]) {
  const auto value = string_field(j, key);
  for (std::size_t i = 0; i < N; ++i)
    if (value == names[i]) return static_cast<E>(i);
  std::string list;
  for (std::size_t i = 0; i < N; ++i) list += (i == 0 ? "" : i + 1 == N ? " or " : ", ") + std::string(names[i]);
  throw std::invalid_argument(std::string(key) + " must be " + list);
}
PayoutRules parse_payout_rules(const json& j) {
  fields(j, {"qualifying_profit", "qualifying_days", "withdrawal_percent", "split_percent", "minimum", "caps"},
         {"consistency_percents", "buffer", "buffer_payouts", "review", "after_payout", "reset_percent"});
  PayoutRules p;
  p.qualifying_profit = decimal_field(j, "qualifying_profit");
  p.qualifying_days = integer_field(j, "qualifying_days");
  if (p.qualifying_days < 1) throw std::invalid_argument("qualifying_days must be at least 1");
  p.withdrawal_percent = integer_field(j, "withdrawal_percent");
  p.split_percent = integer_field(j, "split_percent");
  p.minimum = decimal_field(j, "minimum");
  if (j.contains("buffer")) p.buffer = decimal_field(j, "buffer");
  if (j.contains("buffer_payouts")) p.buffer_payouts = integer_field(j, "buffer_payouts");
  if (j.contains("consistency_percents")) {
    const auto& percents = j.at("consistency_percents");
    if (!percents.is_array() || percents.size() > 64)
      throw std::invalid_argument("consistency_percents must be an array of at most 64 whole percentages");
    for (const auto& percent : percents) p.consistency_percents.push_back(integer_field(json{{"consistency_percents", percent}}, "consistency_percents"));
  }
  if (j.contains("review") && !j.at("review").is_null()) {
    const auto& v = j.at("review");
    fields(v, {}, {"min_hold_seconds", "max_trade_percent", "open_loss", "ladder", "cap"});
    PayoutReview review;
    if (v.contains("min_hold_seconds")) review.min_hold_seconds = integer_field(v, "min_hold_seconds");
    if (v.contains("max_trade_percent")) review.max_trade_percent = integer_field(v, "max_trade_percent");
    if (v.contains("open_loss")) review.open_loss = boolean_field(v, "open_loss");
    if (v.contains("cap") && !v.at("cap").is_null()) review.cap = decimal_field(v, "cap");
    if (v.contains("ladder")) {
      const auto& ladder = v.at("ladder");
      if (!ladder.is_array() || ladder.size() > 16) throw std::invalid_argument("ladder must be an array of at most 16 steps");
      review.ladder.clear();
      for (const auto& step : ladder) review.ladder.push_back(choice_field<ReviewStep>(json{{"ladder", step}}, "ladder", kReviewSteps));
    }
    p.review = review;
  }
  if (j.contains("after_payout")) p.after_payout = choice_field<PayoutFloor>(j, "after_payout", kPayoutFloors);
  if (j.contains("reset_percent")) p.reset_percent = integer_field(j, "reset_percent");
  const auto& caps = j.at("caps");
  if (!caps.is_array() || caps.size() > 64) throw std::invalid_argument("caps must be an array of at most 64 amounts");
  for (const auto& cap : caps) {
    if (!cap.is_string()) throw std::invalid_argument("caps must be decimal strings");
    try { p.caps.push_back(Money::parse(cap.get<std::string>())); }
    catch (const TradingError& e) { throw std::invalid_argument(std::string("caps: ") + e.what()); }
  }
  return p;
}
/// "HH:MM", 00:00 to 24:00, as minutes after midnight.
std::int64_t clock_field(const json& j, const char* key) {
  const auto text = string_field(j, key);
  const auto digit = [&](std::size_t i) { return text[i] >= '0' && text[i] <= '9'; };
  if (text.size() != 5 || text[2] != ':' || !digit(0) || !digit(1) || !digit(3) || !digit(4))
    throw std::invalid_argument(std::string(key) + " must be HH:MM New York time");
  const auto hours = (text[0] - '0') * 10 + (text[1] - '0'), minutes = (text[3] - '0') * 10 + (text[4] - '0');
  if (minutes > 59 || hours > 24 || (hours == 24 && minutes != 0))
    throw std::invalid_argument(std::string(key) + " must be HH:MM New York time");
  return hours * 60 + minutes;
}
/// Each omitted fee amount is zero. Null on the enclosing rules selects flat fees.
FeeSchedule parse_fee_schedule(const json& j) {
  fields(j, {}, {"open", "close", "leg_cap", "clearing", "regulatory", "index", "exercise"});
  FeeSchedule f;
  const auto amount = [&](const char* key, Money& value) { if (j.contains(key)) value = decimal_field(j, key); };
  amount("open", f.open); amount("close", f.close); amount("leg_cap", f.leg_cap); amount("clearing", f.clearing);
  amount("regulatory", f.regulatory); amount("exercise", f.exercise);
  if (j.contains("index")) {
    const auto& index = j.at("index");
    if (!index.is_object()) throw std::invalid_argument("index must map option roots to decimal strings");
    for (auto it = index.begin(); it != index.end(); ++it) f.index[it.key()] = decimal_field(index, it.key().c_str());
  }
  return f;
}
/// The margin settings present in `j`, in rules or beside a plan: the margin
/// mode, account type, house margin and portfolio margin's vol shock.
void margin_fields(const json& j, AccountRules& rules) try {
  if (j.contains("stock_borrow_rate_bps")) rules.stock_borrow_rate_bps = integer_field(j, "stock_borrow_rate_bps");
  if (j.contains("margin_debit_rate_bps")) rules.margin_debit_rate_bps = integer_field(j, "margin_debit_rate_bps");
  if (j.contains("cash_credit_rate_bps")) rules.cash_credit_rate_bps = integer_field(j, "cash_credit_rate_bps");
  if (j.contains("pin_risk_seed")) rules.pin_risk_seed = integer_field(j, "pin_risk_seed");
  if (j.contains("pin_risk")) rules.pin_risk = boolean_field(j, "pin_risk");
  if (j.contains("margin")) {
    const auto margin = string_field(j, "margin");
    if (margin != "strategy" && margin != "portfolio") throw std::invalid_argument("margin must be strategy or portfolio");
    rules.margin = margin == "portfolio" ? MarginMode::Portfolio : MarginMode::Strategy;
  }
  if (j.contains("account_type")) {
    const auto type = string_field(j, "account_type");
    if (type != "margin" && type != "cash" && type != "ira") throw std::invalid_argument("account_type must be margin, cash or ira");
    rules.account_type = type == "cash" ? AccountType::Cash : type == "ira" ? AccountType::Ira : AccountType::Margin;
  }
  if (j.contains("house_margin_percent")) rules.house_margin_percent = integer_field(j, "house_margin_percent");
  if (j.contains("pm_vol_shock")) rules.pm_vol_shock = integer_field(j, "pm_vol_shock");
} catch (const std::exception& error) {
  throw TradingError(Reason::INVALID_RULES, error.what());
}
/// The queue model's seed and stop stress, in custom rules or beside a plan.
void fill_model_fields(const json& j, AccountRules& rules) {
  if (j.contains("queue_seed")) rules.queue_seed = integer_field(j, "queue_seed");
  if (j.contains("stop_stress_ticks")) rules.stop_stress_ticks = integer_field(j, "stop_stress_ticks");
  if (j.contains("stop_stress_percent")) rules.stop_stress_percent = integer_field(j, "stop_stress_percent");
}
void parse_time_rules(const json& j, AccountRules& rules) try {
  if (j.contains("time_limit_days")) rules.time_limit_days = integer_field(j, "time_limit_days");
  if (j.contains("inactivity_days")) rules.inactivity_days = integer_field(j, "inactivity_days");
  if (j.contains("underlyings")) {
    const auto& symbols = j.at("underlyings");
    if (!symbols.is_array() || !std::all_of(symbols.begin(), symbols.end(), [](const json& symbol) { return symbol.is_string(); }))
      throw std::invalid_argument("underlyings must be an array of uppercase symbols");
    rules.underlyings = symbols.get<std::vector<std::string>>();
  }
  if (j.contains("trading_start") && !j.at("trading_start").is_null()) rules.trading_start = clock_field(j, "trading_start");
  if (j.contains("trading_end") && !j.at("trading_end").is_null()) rules.trading_end = clock_field(j, "trading_end");
  if (j.contains("flat_time") && !j.at("flat_time").is_null()) {
    try { rules.flat_time = clock_field(j, "flat_time"); }
    catch (const std::exception&) {
      throw std::invalid_argument("flat_time must be HH:MM New York time from 00:00 to 23:59, before day_end");
    }
  }
  if (j.contains("no_overnight")) rules.no_overnight = boolean_field(j, "no_overnight");
  if (j.contains("events")) rules.events = parse_calendar_events(j.at("events"));
  if (j.contains("news_before_minutes")) rules.news_before_minutes = integer_field(j, "news_before_minutes");
  if (j.contains("news_after_minutes")) rules.news_after_minutes = integer_field(j, "news_after_minutes");
  if (j.contains("news_action")) rules.news_action = string_field(j, "news_action");
  if (j.contains("hold_cutoff")) rules.hold_cutoff = clock_field(j, "hold_cutoff");
  if (j.contains("hold_restrictions")) {
    const auto& list = j.at("hold_restrictions");
    if (!list.is_array() || !std::all_of(list.begin(), list.end(), [](const json& v) { return v.is_string(); }))
      throw std::invalid_argument("hold_restrictions must be an array of restriction names");
    rules.hold_restrictions = list.get<std::vector<std::string>>();
  }
  normalize_event_rules(rules);
  validate_time_rules(rules);
} catch (const std::exception& error) {
  throw PlanRestrictionError(error.what());
}
/// Cost overrides are bookkeeping terms, independent of preset identity.
void program_cost_fields(const json& j, AccountRules& rules) {
  if (j.contains("evaluation_fee")) rules.evaluation_fee = decimal_field(j, "evaluation_fee");
  if (j.contains("reset_fee")) rules.reset_fee = decimal_field(j, "reset_fee");
  if (j.contains("activation_fee")) rules.activation_fee = decimal_field(j, "activation_fee");
  if (j.contains("max_resets")) rules.max_resets = integer_field(j, "max_resets");
}
}  // namespace
/// Custom rules: nullable money for an absent target/drawdown, like rules_json.
/// The phase defaults to evaluation; a funded phase requires payout rules.
AccountRules parse_rules(const json& j) {
  fields(j, {"profit_target", "max_drawdown", "drawdown_mode", "buy_only", "buying_power", "expiry_cutoff_seconds"},
         {"plan", "plan_id", "phase", "lock_balance", "payouts", "defined_risk", "slippage_ticks", "margin", "fill_latency_ms", "impact_ticks",
          "lock_at_start", "profit_basis", "daily_loss_limit", "daily_loss_basis", "daily_loss_action", "consistency_percent",
          "consistency_basis", "min_trading_days", "min_profitable_days", "profitable_day_profit", "day_end", "fees",
          "account_type", "house_margin_percent", "pm_vol_shock", "cash_credit_rate_bps", "margin_debit_rate_bps", "stock_borrow_rate_bps", "inside_fill_percent", "pin_risk", "pin_risk_seed",
          "queue_fills", "queue_seed", "stop_stress_ticks", "stop_stress_percent",
          "min_hold_seconds", "microscalp_seconds", "microscalp_percent", "min_trades", "trade_consistency_percent", "max_volume_percent", "max_contracts_held", "no_hedging", "no_counter_positions", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent",
          "time_limit_days", "inactivity_days", "underlyings", "trading_start", "trading_end", "flat_time", "no_overnight", "scaling", "size_scaling",
          "events", "news_before_minutes", "news_after_minutes", "news_action", "hold_restrictions", "hold_cutoff",
          "evaluation_fee", "reset_fee", "activation_fee", "max_resets",
          "drawdown_basis", "open_loss_cap", "minute_volume_percent", "best_position_percent"});

  if (j.contains("microscalp_seconds") != j.contains("microscalp_percent"))
    throw TradingError(Reason::INVALID_RULES, "Set microscalp_seconds and microscalp_percent together");
  AccountRules rules;
  // Accept read-back rules in a custom request, but always derive the identity.
  if (j.contains("plan_id") && !j.at("plan_id").is_null() && !j.at("plan_id").is_string())
    throw std::invalid_argument("plan_id must be a string or null");
  if (j.contains("phase")) {
    const auto phase = string_field(j, "phase");
    if (phase != "evaluation" && phase != "funded" && phase != "verification") throw std::invalid_argument("phase must be evaluation, verification or funded");
    rules.phase = phase == "funded" ? Phase::Funded : phase == "verification" ? Phase::Verification : Phase::Evaluation;
  }
  const bool payouts = j.contains("payouts") && !j.at("payouts").is_null();
  if (payouts != (rules.phase == Phase::Funded))
    throw std::invalid_argument("payouts are required for the funded phase and forbidden otherwise");
  if (payouts) rules.payouts = parse_payout_rules(j.at("payouts"));
  if (j.contains("lock_balance") && !j.at("lock_balance").is_null()) rules.lock_balance = decimal_field(j, "lock_balance");
  if (j.contains("plan") && !j.at("plan").is_null()) rules.plan = string_field(j, "plan");
  auto optional_money = [&](const char* key) { return j.at(key).is_null() ? Money{} : decimal_field(j, key); };
  rules.profit_target = optional_money("profit_target");
  rules.max_drawdown = optional_money("max_drawdown");
  rules.drawdown_mode = choice_field<DrawdownMode>(j, "drawdown_mode", kDrawdownModes);
  // The evaluation rules added later are optional, with their defaults off.
  const auto has = [&](const char* key) { return j.contains(key) && !j.at(key).is_null(); };
  if (has("lock_at_start")) rules.lock_at_start = boolean_field(j, "lock_at_start");
  if (has("profit_basis")) rules.profit_basis = choice_field<ProfitBasis>(j, "profit_basis", kProfitBases);
  if (has("daily_loss_limit")) rules.daily_loss_limit = decimal_field(j, "daily_loss_limit");
  if (has("daily_loss_basis")) rules.daily_loss_basis = choice_field<DailyLossBasis>(j, "daily_loss_basis", kDailyLossBases);
  if (has("daily_loss_action")) rules.daily_loss_action = choice_field<BreachAction>(j, "daily_loss_action", kBreachActions);
  if (has("consistency_percent")) rules.consistency_percent = integer_field(j, "consistency_percent");
  if (has("consistency_basis")) rules.consistency_basis = choice_field<ConsistencyBasis>(j, "consistency_basis", kConsistencyBases);
  if (j.contains("min_hold_seconds")) rules.min_hold_seconds = integer_field(j, "min_hold_seconds");
  if (j.contains("microscalp_seconds")) rules.microscalp_seconds = integer_field(j, "microscalp_seconds");
  if (j.contains("microscalp_percent")) rules.microscalp_percent = integer_field(j, "microscalp_percent");
  if (j.contains("min_trades")) rules.min_trades = integer_field(j, "min_trades");
  if (j.contains("trade_consistency_percent")) rules.trade_consistency_percent = integer_field(j, "trade_consistency_percent");
  if (has("min_trading_days")) rules.min_trading_days = integer_field(j, "min_trading_days");
  if (has("min_profitable_days")) rules.min_profitable_days = integer_field(j, "min_profitable_days");
  if (has("profitable_day_profit")) rules.profitable_day_profit = decimal_field(j, "profitable_day_profit");
  if (has("day_end")) rules.day_end_minutes = clock_field(j, "day_end");
  if (j.contains("max_contracts_held")) rules.max_contracts_held = integer_field(j, "max_contracts_held");
  if (j.contains("max_volume_percent")) rules.max_volume_percent = integer_field(j, "max_volume_percent");
  if (j.contains("no_hedging")) rules.no_hedging = boolean_field(j, "no_hedging");
  if (j.contains("no_counter_positions")) rules.no_counter_positions = boolean_field(j, "no_counter_positions");
  if (j.contains("require_stop_loss")) rules.require_stop_loss = boolean_field(j, "require_stop_loss");
  if (has("max_trade_risk")) rules.max_trade_risk = decimal_field(j, "max_trade_risk");
  if (j.contains("max_trade_risk_percent")) rules.max_trade_risk_percent = integer_field(j, "max_trade_risk_percent");
  if (has("drawdown_basis")) rules.drawdown_basis = choice_field<DrawdownBasis>(j, "drawdown_basis", kDrawdownBases);
  if (has("open_loss_cap")) {
    const auto& v = j.at("open_loss_cap");
    fields(v, {"basis", "action"}, {"amount", "percent"});
    OpenLossCap cap;
    cap.basis = choice_field<OpenLossBasis>(v, "basis", kOpenLossBases);
    cap.action = choice_field<OpenLossAction>(v, "action", kOpenLossActions);
    if (v.contains("amount") && !v.at("amount").is_null()) cap.amount = decimal_field(v, "amount");
    if (v.contains("percent")) cap.percent = integer_field(v, "percent");
    rules.open_loss_cap = cap;
  }
  if (j.contains("minute_volume_percent")) rules.minute_volume_percent = integer_field(j, "minute_volume_percent");
  if (j.contains("best_position_percent")) rules.best_position_percent = integer_field(j, "best_position_percent");
  parse_time_rules(j, rules);
  if (j.contains("size_scaling") && !j.at("size_scaling").is_null()) {
    const auto& v = j.at("size_scaling");
    fields(v, {"profit_percent", "payouts", "days", "increase_percent", "max_balance"});
    rules.size_scaling = SizeScaling{integer_field(v, "profit_percent"), integer_field(v, "payouts"),
        integer_field(v, "days"), integer_field(v, "increase_percent"), decimal_field(v, "max_balance")};
  }
  if (j.contains("scaling")) {
    if (!j.at("scaling").is_array()) throw std::invalid_argument("scaling must be an array");
    for (const auto& step : j.at("scaling")) {
      fields(step, {"profit", "contracts"}, {});
      rules.scaling.push_back({decimal_field(step, "profit"), integer_field(step, "contracts")});
    }
  }
  rules.buy_only = boolean_field(j, "buy_only");
  if (j.contains("defined_risk")) rules.defined_risk = boolean_field(j, "defined_risk");
  if (j.contains("slippage_ticks")) rules.slippage_ticks = integer_field(j, "slippage_ticks");
  if (j.contains("fill_latency_ms")) rules.fill_latency_ms = integer_field(j, "fill_latency_ms");
  if (j.contains("impact_ticks")) rules.impact_ticks = integer_field(j, "impact_ticks");
  if (j.contains("inside_fill_percent")) rules.inside_fill_percent = integer_field(j, "inside_fill_percent");
  if (j.contains("queue_fills")) rules.queue_fills = boolean_field(j, "queue_fills");
  fill_model_fields(j, rules);
  if (j.contains("fees") && !j.at("fees").is_null()) rules.fees = parse_fee_schedule(j.at("fees"));
  margin_fields(j, rules);
  program_cost_fields(j, rules);
  rules.buying_power = boolean_field(j, "buying_power");
  // Out of range is a rule error (INVALID_RULES from validate_rules), not a malformed
  // request; saturate first so the conversion to nanoseconds cannot overflow.
  rules.expiry_cutoff = std::clamp<std::int64_t>(integer_field(j, "expiry_cutoff_seconds"), -1, 86'400) * md::kNanosPerSecond;
  validate_new_rules(rules);
  return rules;
}
namespace {
/// A preset's name belongs to its own balance and rules, so an attempt recorded under
/// it (and the funded plan it unlocks) is that preset's; the fill model may differ.
void check_plan_name(Money initial_cash, const AccountRules& rules) {
  if (const auto* preset = find_plan_named(rules.plan); preset && !follows_plan(*preset, initial_cash, rules))
    throw TradingError(Reason::INVALID_RULES, "\"" + rules.plan + "\" is a preset's name: reset with plan " + preset->id +
                       ", or give these rules another name");
}
/// The account's margin, as a broker sets it, can stand beside a plan or custom
/// rules, as the fill model does; the evaluation rules stay intact.
/// Financing posts to realised P&L, so only a practice account may change the
/// plan's rates; pin risk stays a simulation choice on any plan.
void margin_model(const json& body, AccountRules& rules) {
  const auto plan = rules;
  margin_fields(body, rules);
  if (!plan.practice() && (rules.cash_credit_rate_bps != plan.cash_credit_rate_bps ||
                           rules.margin_debit_rate_bps != plan.margin_debit_rate_bps ||
                           rules.stock_borrow_rate_bps != plan.stock_borrow_rate_bps))
    throw TradingError(Reason::PLAN_LOCKED, "Financing rates can be overridden only on practice accounts; "
                       "evaluation, verification and funded plans use their own rates");
  // The last step of a create or reset request: the rules it sets, its fee fields too.
  validate_new_rules(rules);
}
/// Presets override only execution settings; all evaluation rules stay intact.
/// Queue fills, its seed and stop stress are program rules: an attempt that
/// changes them no longer follows its preset plan.
void fill_model(const json& body, AccountRules& rules) {
  fill_model_fields(body, rules);
  if (body.contains("fill_model")) {
    const auto model = string_field(body, "fill_model");
    if (model != "as_displayed" && model != "conservative" && model != "midpoint" && model != "queue")
      throw std::invalid_argument("fill_model must be as_displayed, conservative, midpoint or queue");
    rules.fill_latency_ms = model == "conservative" ? 1000 : 0;
    rules.impact_ticks = model == "conservative" ? 1 : 0;
    rules.slippage_ticks = model == "conservative" ? 1 : 0;
    rules.inside_fill_percent = model == "midpoint" ? 50 : 0;
    rules.queue_fills = model == "queue";
  }
  validate_rules(rules);
}
/// An illustrative broker schedule; custom rules can supply each amount instead.
void fee_model(const json& body, AccountRules& rules) {
  if (!body.contains("fee_model")) return;
  const auto model = string_field(body, "fee_model");
  if (model != "flat" && model != "itemized") throw std::invalid_argument("fee_model must be flat or itemized");
  rules.fees.reset();
  if (model == "itemized")
    rules.fees = FeeSchedule{Money::parse("1.00"), {}, Money::parse("10.00"), Money::parse("0.10"), Money::parse("0.02"),
                            {{"SPX", Money::parse("0.60")}, {"SPXW", Money::parse("0.60")}}, Money::parse("5.00")};
}
}  // namespace
json strict_json(const std::string& body) {
  // JSON parsers normally keep the last duplicate key; that is ambiguous for orders.
  std::vector<std::set<std::string>> keys;
  try {
    return json::parse(body, [&](int depth, json::parse_event_t event, json& parsed) {
      // The transport checks depth first (check_request_json); this keeps direct callers bounded too.
      if (depth >= kRequestJsonDepthMax && (event == json::parse_event_t::object_start || event == json::parse_event_t::array_start))
        throw std::invalid_argument("Request body nests deeper than " + std::to_string(kRequestJsonDepthMax) + " levels");
      if (event == json::parse_event_t::object_start) keys.emplace_back();
      if (event == json::parse_event_t::key && !keys.back().insert(parsed.get<std::string>()).second)
        throw std::invalid_argument("Duplicate JSON field");
      if (event == json::parse_event_t::object_end) keys.pop_back();
      return true;
    });
  } catch (const json::parse_error&) {
    // An empty or malformed body is the client's mistake on every route, a 400.
    throw std::invalid_argument("Request body must be valid JSON");
  }
}
namespace {
/// The optional scope of a bulk command: one underlying, or every one.
std::string scope_field(const json& body) {
  fields(body, {}, {"underlying"});
  if (!body.contains("underlying")) return {};
  auto underlying = string_field(body, "underlying");
  if (underlying.empty() || underlying.size() > 16 ||
      !std::all_of(underlying.begin(), underlying.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }))
    throw std::invalid_argument("underlying must be an uppercase symbol such as SPX");
  return underlying;
}
}  // namespace
/// Client order IDs: 1 to 128 bytes of text, none of them a control character.
bool valid_client_order_id(std::string_view id) {
  return !id.empty() && id.size() <= 128 &&
         std::none_of(id.begin(), id.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}
namespace {
/// A kill-switch or account-reset reason: the journal keeps it with every change of the
/// latch, refusals repeat it and /api/risk lists it, so at most 200 bytes of text without
/// control characters. The reducer refuses a blank one (INVALID_REASON).
std::string reason_field(const json& body) {
  auto reason = string_field(body, "reason");
  if (reason.size() > 200) throw std::invalid_argument("reason must be at most 200 bytes");
  if (std::any_of(reason.begin(), reason.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; }))
    throw std::invalid_argument("reason must be text without control characters");
  return reason;
}
}  // namespace
/// Account IDs are lowercase letters, digits and single hyphens, as their journals are named.
bool valid_account(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' && id.find("--") == std::string_view::npos &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
/// /api/orders/{id}: one segment after /api/orders/ that is not a sibling route's name.
bool order_id_path(std::string_view path) {
  constexpr std::string_view prefix = "/api/orders/";
  if (!path.starts_with(prefix)) return false;
  const auto id = path.substr(prefix.size());
  return !id.empty() && id.find('/') == std::string_view::npos && id != "preview" && id != "what-if" && id != "cancel" && id != "copy";
}
/// A display name without the spaces around it.
std::string trimmed_name(std::string_view name) {
  const auto first = name.find_first_not_of(' ');
  if (first == std::string_view::npos) return {};
  return std::string(name.substr(first, name.find_last_not_of(' ') - first + 1));
}
namespace {
/// Whether a name shows a character: not only spaces, other Unicode spaces or
/// invisible format characters (zero-width spaces and joiners, bidi controls, fillers,
/// variation selectors, tags, the BOM), which an account switcher would show blank.
bool visible_name(std::string_view name) {
  for (std::size_t at = 0; at < name.size();) {
    const auto length = md::utf8_sequence(name, at);
    if (length == 0) return false;
    const auto lead = static_cast<unsigned char>(name[at]);
    std::uint32_t code = length == 1 ? lead : lead & (0x7Fu >> length);
    for (std::size_t k = 1; k < length; ++k) code = code << 6 | (static_cast<unsigned char>(name[at + k]) & 0x3Fu);
    at += length;
    const bool blank = code == 0x20 || code == 0xA0 || code == 0xAD || code == 0x34F || code == 0x115F || code == 0x1160 ||
        code == 0x1680 || code == 0x180E || (code >= 0x2000 && code <= 0x200F) || (code >= 0x2028 && code <= 0x202F) ||
        (code >= 0x205F && code <= 0x206F) || code == 0x3000 || code == 0x3164 || (code >= 0xFE00 && code <= 0xFE0F) ||
        code == 0xFEFF || code == 0xFFA0 || (code >= 0xE0000 && code <= 0xE0FFF);
    if (!blank) return true;
  }
  return false;
}
/// A new or changed account name, trimmed of surrounding spaces; 1 to 64 characters,
/// at least one of them visible and none a control character.
std::string account_name_field(const json& body) {
  auto name = trimmed_name(string_field(body, "name"));
  if (!valid_account_name(name) || !visible_name(name))
    throw std::invalid_argument("name must be 1 to 64 characters, at least one of them visible and none a control character");
  return name;
}
std::optional<Walk> parse_walk(const json& j) {
  if (j.is_null()) return std::nullopt;
  fields(j, {"step", "seconds", "limit"});
  return Walk{decimal_field(j, "step"), integer_field(j, "seconds"), decimal_field(j, "limit")};
}
/// Orders in a request and its chained orders.
std::size_t chain_length(const OrderRequest& r) {
  std::size_t size = 1;
  for (const auto& next : r.then) size += chain_length(next);
  for (const auto& other : r.oco) size += chain_length(other);
  return size;
}
}  // namespace
/// An order body: a single contract (symbol and side) or legs. A body no market
/// could make a valid order is malformed. `depth` counts the orders a chained one
/// (an order's then or oco) hangs on; it takes no client_order_id or exits_only.
OrderRequest parse_order(const json& body, std::size_t depth) {
  if (!body.is_object()) throw std::invalid_argument("An order is an object");
  if (depth >= kMaxChain) throw std::invalid_argument("A chain is at most four orders");
  const bool chained = depth > 0;
  // A single contract (symbol and side), or legs for a multi-leg order.
  const bool legs = body.contains("legs");
  // A chained order takes its ID from the order it hangs on, and is never a held exit.
  if (chained && legs) fields(body, {"legs", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "group", "good_till", "walk", "then", "oco"});
  else if (chained) fields(body, {"symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "group", "good_till", "walk", "then", "oco"});
  else if (legs) fields(body, {"client_order_id", "legs", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "exits_only", "group", "good_till", "walk", "then", "oco"});
  else fields(body, {"client_order_id", "symbol", "side", "type", "quantity", "time_in_force"}, {"limit_price", "trigger", "bracket", "tags", "note", "exits_only", "group", "good_till", "walk", "then", "oco"});
  // A body no market could make a valid order is malformed: 400, and nothing is
  // recorded, so its client_order_id stays free. The reducer's own checks (422,
  // recorded) are those that depend on the account and the market.
  OrderRequest order;
  if (!chained) {
    order.client_order_id = string_field(body, "client_order_id");
    if (!valid_client_order_id(order.client_order_id))
      throw std::invalid_argument("client_order_id must be 1 to 128 bytes of text without control characters");
  }
  const auto type = string_field(body, "type"), tif = string_field(body, "time_in_force");
  constexpr std::pair<std::string_view, TimeInForce> tifs[] = {{"day", TimeInForce::Day}, {"ioc", TimeInForce::Ioc},
      {"gtc", TimeInForce::Gtc}, {"exto", TimeInForce::Exto}, {"gtc_exto", TimeInForce::GtcExto}, {"gtd", TimeInForce::Gtd}};
  const auto named = std::find_if(std::begin(tifs), std::end(tifs), [&](const auto& t) { return t.first == tif; });
  if ((type != "limit" && type != "market") || named == std::end(tifs))
    throw std::invalid_argument("Invalid type or time_in_force");
  order.type = type == "limit" ? OrderType::Limit : OrderType::Market;
  order.tif = named->second;
  if (order.type == OrderType::Market && order.tif != TimeInForce::Ioc &&
      !(body.contains("trigger") && (order.tif == TimeInForce::Exto || order.tif == TimeInForce::GtcExto || order.tif == TimeInForce::Gtd)))
    throw std::invalid_argument("Market orders need ioc; triggered markets also take exto, gtc_exto or gtd");
  if ((order.tif == TimeInForce::Gtd) != body.contains("good_till"))
    throw std::invalid_argument("good_till is required for gtd and forbidden otherwise");
  if (body.contains("good_till")) {
    const auto value = string_field(body, "good_till");
    const bool zoned = value.ends_with("Z") || (value.size() >= 6 && (value[value.size() - 6] == '+' || value[value.size() - 6] == '-'));
    const auto parsed = md::parse_datetime(value, md::Zone::Utc);
    if (!zoned || !parsed) throw std::invalid_argument("good_till must be an ISO date-time with Z or a UTC offset");
    order.good_till = *parsed;
  }
  order.quantity = integer_field(body, "quantity");
  if (order.quantity < 1) throw std::invalid_argument("quantity must be a positive whole number of contracts or units");
  if ((order.type == OrderType::Limit) != body.contains("limit_price"))
    throw std::invalid_argument("limit_price is required for limit orders and forbidden for market orders");
  if (body.contains("limit_price")) order.limit_price = decimal_field(body, "limit_price");
  if (legs) {
    const auto& list = body.at("legs");
    if (!list.is_array() || list.size() < 2 || list.size() > kMaxRollLegs)
      throw std::invalid_argument("legs must be an array of two to four legs, or up to eight for a roll");
    std::set<std::string> symbols;
    for (const auto& item : list) {
      fields(item, {"symbol", "side"}, {"ratio"});
      Leg leg;
      leg.symbol = symbol_field(item);
      const auto side = string_field(item, "side");
      if (side != "buy" && side != "sell") throw std::invalid_argument("Leg side must be buy or sell");
      leg.side = side == "buy" ? Side::Buy : Side::Sell;
      if (item.contains("ratio")) leg.ratio = integer_field(item, "ratio");
      if (leg.ratio < 1 || leg.ratio > kMaxRatio) throw std::invalid_argument("A leg's ratio must be 1 to 10");
      if (!symbols.insert(leg.symbol).second) throw std::invalid_argument("Each leg must name a different contract");
      order.legs.push_back(std::move(leg));
    }
  } else {
    if (order.limit_price && *order.limit_price <= Money{})
      throw std::invalid_argument("A single contract's limit_price must be positive");
    order.symbol = symbol_field(body);
    const auto side = string_field(body, "side");
    if (side != "buy" && side != "sell") throw std::invalid_argument("Invalid side, type or time_in_force");
    order.side = side == "buy" ? Side::Buy : Side::Sell;
  }
  if (body.contains("exits_only")) order.exits_only = boolean_field(body, "exits_only");
  if (body.contains("walk")) order.walk = parse_walk(body.at("walk"));
  if (body.contains("group")) {
    // A trade ID: the opening fill of one of the account's round trips.
    order.group = string_field(body, "group");
    (void)identifier(order.group);
  }
  if (body.contains("note")) order.note = string_field(body, "note");
  if (body.contains("tags")) {
    const auto& tags = body.at("tags");
    if (!tags.is_array() || tags.size() > 16) throw std::invalid_argument("tags must be an array of at most 16 strings");
    for (const auto& tag : tags) {
      if (!tag.is_string()) throw std::invalid_argument("tags must be strings");
      order.tags.push_back(tag.get<std::string>());
    }
  }
  if (body.contains("trigger")) order.trigger = parse_trigger(body.at("trigger"));
  if (body.contains("bracket")) {
    const auto& bracket = body.at("bracket");
    fields(bracket, {}, {"stop_loss", "take_profit"});
    order.bracket = Bracket{};
    if (bracket.contains("stop_loss")) order.bracket->stop_loss = parse_exit(bracket.at("stop_loss"));
    if (bracket.contains("take_profit")) order.bracket->take_profit = parse_exit(bracket.at("take_profit"));
    if (!order.bracket->stop_loss && !order.bracket->take_profit)
      throw std::invalid_argument("A bracket needs a stop_loss, a take_profit or both");
  }
  // One order placed when this one fills completely, and one that cancels it.
  for (const char* key : {"then", "oco"}) {
    if (!body.contains(key)) continue;
    auto next = parse_order(body.at(key), depth + 1);
    if (std::string_view(key) == "oco" && !next.oco.empty()) throw std::invalid_argument("An oco order takes no oco of its own");
    (std::string_view(key) == "then" ? order.then : order.oco).push_back(std::move(next));
  }
  if (order.exits_only && (!order.then.empty() || !order.oco.empty())) throw std::invalid_argument("Held exits take no then or oco");
  if (!chained && chain_length(order) > kMaxChain) throw std::invalid_argument("A chain is at most four orders");
  return order;
}
namespace {
/// A share order's body: POST /api/stocks/orders, its preview and its OCO partner.
StockOrderRequest parse_stock_order(const json& body, bool partner = false) {
  if (partner) fields(body, {"side", "type", "shares"}, {"time_in_force", "limit_price", "stop_price", "extended_hours"});
  else fields(body, {"client_order_id", "symbol", "side", "type", "shares"},
              {"time_in_force", "limit_price", "stop_price", "extended_hours", "bracket", "oco"});
  StockOrderRequest r;
  if (!partner) {
    r.client_order_id = string_field(body, "client_order_id");
    if (!valid_client_order_id(r.client_order_id)) throw std::invalid_argument("client_order_id must be 1-64 printable bytes");
    r.symbol = string_field(body, "symbol");
  }
  const auto side = string_field(body, "side");
  if (side != "buy" && side != "sell") throw std::invalid_argument("side must be buy or sell");
  r.side = side == "buy" ? Side::Buy : Side::Sell;
  const auto type = string_field(body, "type");
  if (type == "market") r.type = StockOrderType::Market;
  else if (type == "limit") r.type = StockOrderType::Limit;
  else if (type == "stop") r.type = StockOrderType::Stop;
  else if (type == "stop_limit") r.type = StockOrderType::StopLimit;
  else throw std::invalid_argument("type must be market, limit, stop or stop_limit");
  r.shares = integer_field(body, "shares");
  const auto tif = body.contains("time_in_force") ? string_field(body, "time_in_force") : std::string("day");
  if (tif != "day" && tif != "gtc") throw std::invalid_argument("time_in_force must be day or gtc for shares");
  r.tif = tif == "gtc" ? TimeInForce::Gtc : TimeInForce::Day;
  if (body.contains("limit_price")) r.limit_price = decimal_field(body, "limit_price");
  if (body.contains("stop_price")) r.stop_price = decimal_field(body, "stop_price");
  if (body.contains("extended_hours")) r.extended_hours = boolean_field(body, "extended_hours");
  if (body.contains("bracket")) {
    const auto& bracket = body.at("bracket");
    fields(bracket, {}, {"stop_loss", "take_profit"});
    if (bracket.contains("stop_loss")) r.stop_loss = decimal_field(bracket, "stop_loss");
    if (bracket.contains("take_profit")) r.take_profit = decimal_field(bracket, "take_profit");
    if (!r.stop_loss && !r.take_profit) throw std::invalid_argument("A bracket needs a stop_loss, a take_profit or both");
  }
  if (body.contains("oco")) {
    auto other = parse_stock_order(body.at("oco"), true);
    other.symbol = r.symbol;
    r.oco.push_back(std::move(other));
  }
  return r;
}
}  // namespace
TradingCommand parse_command(const ApiRequest& request, std::string_view path) {
  TradingCommand command;
  if (path.starts_with("/api/accounts/")) {
    command.account = path.substr(std::string_view("/api/accounts/").size());
    if (!valid_account(command.account)) throw std::invalid_argument("Invalid account id");
    if (request.method == "DELETE") {
      if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
      command.kind = TradingCommand::Kind::DeleteAccount;
    } else {
      command.kind = TradingCommand::Kind::UpdateAccount;
      const auto body = strict_json(request.body);
      fields(body, {}, {"name", "archived"});
      if (body.empty()) throw std::invalid_argument("Provide name or archived");
      if (body.contains("name")) command.name = account_name_field(body);
      if (body.contains("archived")) {
        if (!body.at("archived").is_boolean()) throw std::invalid_argument("archived must be a boolean");
        command.archived = body.at("archived").get<bool>();
      }
    }
    return command;
  }
  if (request.method == "DELETE" && path.starts_with("/api/alerts/")) {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::DeleteAlert;
    command.alert_id = identifier(path.substr(std::string_view("/api/alerts/").size()));
    return command;
  }
  if (request.method == "DELETE" && path.starts_with("/api/stocks/orders/")) {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::CancelStockOrder;
    command.order_id = identifier(path.substr(std::string_view("/api/stocks/orders/").size()));
    return command;
  }
  if (request.method == "DELETE") {
    if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
    command.kind = TradingCommand::Kind::Cancel;
    command.order_id = identifier(path.substr(std::string_view("/api/orders/").size()));
    return command;
  }
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  auto body = strict_json(request.body);
  // POST /api/orders/what-if: candidates, each a list of orders without chains.
  if (path == "/api/orders/what-if") {
    fields(body, {"candidates"});
    command.kind = TradingCommand::Kind::WhatIf;
    const auto& list = body.at("candidates");
    if (!list.is_array() || list.empty() || list.size() > kMaxWhatIfCandidates)
      throw std::invalid_argument("candidates must be an array of one to six candidates");
    for (std::size_t c = 0; c < list.size(); ++c) {
      const auto where = "candidates[" + std::to_string(c) + "]";
      fields(list[c], {"orders"}, {"name"});
      auto name = list[c].contains("name") ? string_field(list[c], "name") : "Candidate " + std::to_string(c + 1);
      if (name.empty() || name.size() > 64) throw std::invalid_argument(where + ".name must be 1 to 64 bytes");
      command.candidate_names.push_back(std::move(name));
      const auto& orders = list[c].at("orders");
      if (!orders.is_array() || orders.empty() || orders.size() > kMaxWhatIfOrders)
        throw std::invalid_argument(where + ".orders must be an array of one to four orders");
      auto& requests = command.candidates.emplace_back();
      for (std::size_t i = 0; i < orders.size(); ++i) {
        // Each order is parsed as a submission parses it; a client ID is optional here.
        // Checked before the copy: copying a value recurses once per nesting level.
        if (!orders[i].is_object())
          throw std::invalid_argument(where + ".orders[" + std::to_string(i) + "]: An order is an object");
        auto order = orders[i];
        if (!order.contains("client_order_id"))
          order["client_order_id"] = "what-if-" + std::to_string(c + 1) + "-" + std::to_string(i + 1);
        ApiRequest single;
        single.method = "POST";
        single.body = order.dump();
        try {
          requests.push_back(parse_command(single, "/api/orders").order);
        } catch (const std::invalid_argument& error) {
          throw std::invalid_argument(where + ".orders[" + std::to_string(i) + "]: " + error.what());
        }
      }
    }
    return command;
  }
  // POST /api/orders/{id}/preview: a change's terms, as PUT takes them, and floor_share.
  const bool change_preview = request.method == "POST" && path != "/api/orders/preview" &&
      path.starts_with("/api/orders/") && path.ends_with("/preview");
  if ((request.method == "PUT" && path.starts_with("/api/orders/")) || change_preview) {
    if (change_preview) fields(body, {}, {"quantity", "limit_price", "trigger_level", "time_in_force", "walk", "floor_share"});
    else fields(body, {}, {"quantity", "limit_price", "trigger_level", "time_in_force", "walk"});
    command.kind = change_preview ? TradingCommand::Kind::PreviewChange : TradingCommand::Kind::Modify;
    auto id = path.substr(std::string_view("/api/orders/").size());
    if (change_preview) id = id.substr(0, id.size() - std::string_view("/preview").size());
    command.order_id = identifier(id);
    if (change_preview && body.contains("floor_share")) {
      command.floor_share = number_field(body, "floor_share");
      if (command.floor_share <= 0 || command.floor_share > 1) throw std::invalid_argument("floor_share must be in (0, 1]");
    }
    if (body.contains("quantity")) {
      command.change.quantity = integer_field(body, "quantity");
      if (*command.change.quantity < 1)
        throw std::invalid_argument("quantity must be a positive whole number of contracts or units");
    }
    if (body.contains("limit_price")) command.change.limit_price = decimal_field(body, "limit_price");
    if (body.contains("trigger_level")) command.change.trigger_level = decimal_field(body, "trigger_level");
    if (body.contains("walk")) command.change.walk.emplace(parse_walk(body.at("walk")));
    if (body.contains("time_in_force")) {
      const auto tif = string_field(body, "time_in_force");
      if (tif != "day" && tif != "gtc") throw std::invalid_argument("time_in_force changes to day or gtc");
      command.change.tif = tif == "day" ? TimeInForce::Day : TimeInForce::Gtc;
    }
    if (command.change.empty()) throw std::invalid_argument("Give quantity, limit_price, trigger_level, time_in_force or walk");
    return command;
  }
  if (request.method == "PUT" && path.starts_with("/api/days/") && path.ends_with("/note")) {
    fields(body, {"plan", "review"});
    const auto date = csv_date(path.substr(10, path.size() - 15));
    if (!date) throw std::invalid_argument("Expected a valid YYYY-MM-DD date");
    command.kind = TradingCommand::Kind::DayNote;
    command.day = *date;
    command.plan = string_field(body, "plan");
    command.review = string_field(body, "review");
    return command;
  }
  if (request.method == "POST" && (path == "/api/trades/group" || path == "/api/trades/ungroup")) {
    // The round trips, by trade ID, whose trades join or which leave theirs.
    fields(body, {"trades"});
    command.kind = path == "/api/trades/group" ? TradingCommand::Kind::Group : TradingCommand::Kind::Ungroup;
    const auto& trades = body.at("trades");
    if (!trades.is_array() || trades.empty() || trades.size() > 16)
      throw std::invalid_argument("trades must be an array of 1 to 16 trade IDs");
    for (const auto& id : trades) {
      if (!id.is_string()) throw std::invalid_argument("trades must be trade ID strings");
      command.trades.push_back(identifier(id.get<std::string>()));
    }
    return command;
  }
  if (request.method == "PUT" && path.starts_with("/api/trades/")) {
    // PUT /api/trades/{id}/note: the note and tags replace the trade's.
    fields(body, {}, {"note", "tags", "mistakes"});
    if (body.contains("mistakes")) {
      const auto& picks = body.at("mistakes");
      if (!picks.is_array() || picks.size() > 7) throw std::invalid_argument("mistakes must be an array of at most seven fixed picks");
      for (const auto& pick : picks) {
        if (!pick.is_string()) throw std::invalid_argument("mistakes must be strings");
        command.mistakes.push_back(pick.get<std::string>());
      }
    }
    command.kind = TradingCommand::Kind::Annotate;
    auto id = path.substr(std::string_view("/api/trades/").size());
    id = id.substr(0, id.size() - std::string_view("/note").size());
    // Share round trips are named "s" and their opening stock fill.
    command.shares = id.starts_with("s");
    command.trade = identifier(command.shares ? id.substr(1) : id);
    if (body.contains("note")) command.note = string_field(body, "note");
    if (body.contains("tags")) {
      const auto& tags = body.at("tags");
      if (!tags.is_array() || tags.size() > 16) throw std::invalid_argument("tags must be an array of at most 16 strings");
      for (const auto& tag : tags) {
        if (!tag.is_string()) throw std::invalid_argument("tags must be strings");
        command.tags.push_back(tag.get<std::string>());
      }
    }
    return command;
  }
  if (path == "/api/alerts") {
    // POST /api/alerts: what to watch, a direction and an inclusive level.
    fields(body, {"scope", "metric", "direction", "level"}, {"label", "symbol", "legs", "repeat"});
    command.kind = TradingCommand::Kind::CreateAlert;
    auto& c = command.alert.condition;
    const auto scope = string_field(body, "scope");
    constexpr const char* scopes[] = {"contract", "spread", "underlying", "account"};
    const auto found = std::find(std::begin(scopes), std::end(scopes), scope);
    if (found == std::end(scopes)) throw std::invalid_argument("scope must be contract, spread, underlying or account");
    c.scope = static_cast<AlertScope>(found - std::begin(scopes));
    c.metric = string_field(body, "metric");
    const auto direction = string_field(body, "direction");
    if (direction != "at_or_below" && direction != "at_or_above") throw std::invalid_argument("direction must be at_or_below or at_or_above");
    c.direction = direction == "at_or_below" ? TriggerDirection::AtOrBelow : TriggerDirection::AtOrAbove;
    c.level = decimal_field(body, "level");
    if (body.contains("symbol") && !body.at("symbol").is_null()) c.symbol = string_field(body, "symbol");
    if (body.contains("legs") && !body.at("legs").is_null()) {
      if (!body.at("legs").is_array()) throw std::invalid_argument("legs must be an array");
      for (const auto& item : body.at("legs")) {
        fields(item, {"symbol", "side", "ratio"});
        const auto side = string_field(item, "side");
        if (side != "buy" && side != "sell") throw std::invalid_argument("A leg's side must be buy or sell");
        c.legs.push_back({symbol_field(item), side == "buy" ? Side::Buy : Side::Sell, integer_field(item, "ratio")});
      }
    }
    if (body.contains("label")) command.alert.label = string_field(body, "label");
    if (body.contains("repeat")) command.alert.repeat = boolean_field(body, "repeat");
    // The reducer's own check, so malformed terms are a 400 like any malformed request.
    try { validate_alert(command.alert); } catch (const TradingError& e) { throw std::invalid_argument(e.what()); }
    return command;
  }
  if (path == "/api/positions/exercise") {
    fields(body, {"symbol", "quantity"});
    command.kind = TradingCommand::Kind::Exercise;
    command.symbol = symbol_field(body);
    command.quantity = integer_field(body, "quantity");
    if (command.quantity <= 0) throw std::invalid_argument("quantity must be a positive number of contracts");
    return command;
  }
  if (path == "/api/positions/abandon") {
    fields(body, {"symbol"});
    command.kind = TradingCommand::Kind::Abandon;
    command.symbol = symbol_field(body);
    return command;
  }
  if (path == "/api/positions/instruction") {
    fields(body, {"symbol", "do_not_exercise"});
    command.kind = TradingCommand::Kind::ExerciseInstruction;
    command.symbol = symbol_field(body);
    command.do_not_exercise = boolean_field(body, "do_not_exercise");
    return command;
  }
  if (path == "/api/stocks/trade" || path == "/api/stocks/trade/preview") {
    fields(body, {"symbol", "side", "shares"});
    command.kind = path.ends_with("/preview") ? TradingCommand::Kind::PreviewStock : TradingCommand::Kind::TradeStock;
    command.symbol = string_field(body, "symbol");
    if (command.symbol.empty() || command.symbol.size() > 16 ||
        !std::all_of(command.symbol.begin(), command.symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw std::invalid_argument("symbol must be an uppercase stock symbol such as SPY");
    const auto side = string_field(body, "side");
    if (side != "buy" && side != "sell") throw std::invalid_argument("side must be buy or sell");
    command.quantity = integer_field(body, "shares");
    if (command.quantity <= 0 || command.quantity > 10'000'000)
      throw std::invalid_argument("shares must be a positive whole number, at most 10,000,000");
    if (side == "sell") command.quantity = -command.quantity;
    return command;
  }
  if (path == "/api/stocks/orders" || path == "/api/stocks/orders/preview") {
    command.kind = path.ends_with("/preview") ? TradingCommand::Kind::PreviewStockOrder : TradingCommand::Kind::SubmitStockOrder;
    command.stock_order = parse_stock_order(body);
    command.symbol = command.stock_order->symbol;
    validate_stock_order(*command.stock_order);
    return command;
  }
  if (path == "/api/stocks/close") {
    fields(body, {"symbol"}, {"shares"});
    command.kind = TradingCommand::Kind::CloseStock;
    command.symbol = string_field(body, "symbol");
    if (command.symbol.empty() || command.symbol.size() > 16 ||
        !std::all_of(command.symbol.begin(), command.symbol.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; }))
      throw std::invalid_argument("symbol must be an uppercase stock symbol such as SPY");
    if (body.contains("shares")) {
      command.quantity = integer_field(body, "shares");
      if (command.quantity <= 0) throw std::invalid_argument("shares must be positive; leave it out to close them all");
    }
    return command;
  }
  if (path == "/api/orders/cancel") {
    command.kind = TradingCommand::Kind::CancelAll;
    // Named orders, such as both exits of a pair, cancel together in one transaction.
    if (body.is_object() && body.contains("orders")) {
      fields(body, {"orders"});
      const auto& list = body.at("orders");
      if (!list.is_array() || list.empty() || list.size() > 16)
        throw std::invalid_argument("orders must be an array of one to sixteen order IDs");
      for (const auto& item : list) {
        if (!item.is_string()) throw std::invalid_argument("orders must be order ID strings");
        const auto id = identifier(item.get<std::string>());
        if (std::find(command.order_ids.begin(), command.order_ids.end(), id) != command.order_ids.end())
          throw std::invalid_argument("orders must not repeat an order ID");
        command.order_ids.push_back(id);
      }
      return command;
    }
    command.underlying = scope_field(body);
    return command;
  }
  if (path == "/api/positions/close" || path == "/api/positions/close/preview") {
    command.kind = path.ends_with("/preview") ? TradingCommand::Kind::PreviewClose : TradingCommand::Kind::ClosePositions;
    fields(body, {}, {"underlying", "type", "limit_ticks"});
    // Only the scope is copied, and only once it is known to be a string.
    if (body.contains("underlying") && !body.at("underlying").is_string())
      throw std::invalid_argument("underlying must be a string");
    json scope = json::object();
    if (body.contains("underlying")) scope["underlying"] = body.at("underlying");
    command.underlying = scope_field(scope);
    const auto type = body.contains("type") ? string_field(body, "type") : std::string("market");
    if (type != "market" && type != "limit") throw std::invalid_argument("type must be market or limit");
    command.close_pricing.limit = type == "limit";
    if (body.contains("limit_ticks")) {
      if (!command.close_pricing.limit) throw std::invalid_argument("limit_ticks needs type limit");
      command.close_pricing.limit_ticks = integer_field(body, "limit_ticks");
      if (command.close_pricing.limit_ticks < 0 || command.close_pricing.limit_ticks > 10)
        throw std::invalid_argument("limit_ticks must be 0 to 10");
    }
    return command;
  }
  if (path == "/api/accounts") {
    // A name, and either a preset plan or a starting balance and complete rules.
    fields(body, {"name"}, {"plan", "initial_cash", "rules", "fill_model", "fee_model", "margin", "account_type",
                            "house_margin_percent", "pm_vol_shock", "pin_risk", "pin_risk_seed", "cash_credit_rate_bps", "margin_debit_rate_bps", "stock_borrow_rate_bps", "copy_settings_from",
                            "queue_seed", "stop_stress_ticks", "stop_stress_percent", "evaluation_fee", "reset_fee", "activation_fee", "max_resets"});
    command.kind = TradingCommand::Kind::CreateAccount;
    command.name = account_name_field(body);
    if (body.contains("copy_settings_from")) {
      command.copy_settings_from = string_field(body, "copy_settings_from");
      if (!valid_account(command.copy_settings_from)) throw std::invalid_argument("copy_settings_from must be an account id");
    }
    if (body.contains("plan")) {
      if (body.contains("initial_cash") || body.contains("rules")) throw std::invalid_argument("plan excludes initial_cash and rules");
      const auto* plan = find_plan(string_field(body, "plan"));
      if (!plan) throw std::invalid_argument("Unknown plan; see GET /api/plans");
      if (!plan->unlocked_by.empty())
        throw std::invalid_argument("A locked plan starts from an account that passed its prerequisite; reset that account instead");
      command.initial_cash = plan->initial_cash;
      command.rules = plan->rules;
    } else {
      if (!body.contains("initial_cash") || !body.contains("rules"))
        throw std::invalid_argument("Provide a plan, or both initial_cash and rules");
      command.initial_cash = decimal_field(body, "initial_cash");
      if (command.initial_cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
      command.rules = parse_rules(body.at("rules"));
      if (command.rules.phase == Phase::Funded)
        throw std::invalid_argument("A funded account starts from an account that passed its evaluation");
      check_plan_name(command.initial_cash, command.rules);
      if (const auto* preset = find_plan_named(command.rules.plan); preset && !preset->unlocked_by.empty())
        throw std::invalid_argument("A locked plan requires a pass; reset the passing account instead");
    }
    program_cost_fields(body, command.rules);
    fill_model(body, command.rules);
    fee_model(body, command.rules);
    margin_model(body, command.rules);
    return command;
  }
  if (path == "/api/orders" || path == "/api/orders/preview") {
    if (path == "/api/orders/preview") {
      command.kind = TradingCommand::Kind::Preview;
      if (body.contains("floor_share")) {
        command.floor_share = number_field(body, "floor_share");
        if (command.floor_share <= 0 || command.floor_share > 1) throw std::invalid_argument("floor_share must be in (0, 1]");
        body.erase("floor_share");
      }
    }
    command.order = parse_order(body);
  } else if (path == "/api/risk/guardrails") {
    fields(body, {"expected_revision", "guardrails"});
    command.kind = TradingCommand::Kind::Guardrails;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    const auto& g = body.at("guardrails");
    fields(g, {"soft_floor", "soft_floor_percent", "max_opening_trades", "cooldown_loss", "cooldown_minutes", "profit_lock"},
           {"loss_streak", "size_creep_percent", "size_creep_window", "size_creep_block"});
    command.guardrails = {decimal_field(g, "soft_floor"), integer_field(g, "soft_floor_percent"), integer_field(g, "max_opening_trades"),
        decimal_field(g, "cooldown_loss"), integer_field(g, "cooldown_minutes"), decimal_field(g, "profit_lock")};
    if (g.contains("loss_streak")) command.guardrails.loss_streak = integer_field(g, "loss_streak");
    if (g.contains("size_creep_percent")) command.guardrails.size_creep_percent = integer_field(g, "size_creep_percent");
    if (g.contains("size_creep_window")) command.guardrails.size_creep_window = integer_field(g, "size_creep_window");
    if (g.contains("size_creep_block")) command.guardrails.size_creep_block = boolean_field(g, "size_creep_block");
    validate_guardrails(command.guardrails);
  } else if (path == "/api/risk/limits") {
    fields(body, {"expected_revision", "limits"});
    command.kind = TradingCommand::Kind::Limits;
    command.expected_revision = identifier(string_field(body, "expected_revision"));
    command.limits = parse_limits(body.at("limits"));
  } else if (path == "/api/risk/kill") {
    fields(body, {"action", "reason"});
    const auto action = string_field(body, "action");
    if (action != "trip" && action != "reset") throw std::invalid_argument("action must be trip or reset");
    command.kind = action == "trip" ? TradingCommand::Kind::Trip : TradingCommand::Kind::Reset;
    command.reason = reason_field(body);
  } else if (path == "/api/account/reset") {
    // Either a preset ID, or a custom starting balance and complete rules.
    fields(body, {"reason"}, {"plan", "initial_cash", "rules", "fill_model", "fee_model", "margin", "account_type",
                              "house_margin_percent", "pm_vol_shock", "pin_risk", "pin_risk_seed", "cash_credit_rate_bps", "margin_debit_rate_bps", "stock_borrow_rate_bps", "evaluation_fee", "reset_fee", "activation_fee", "max_resets",
                              "queue_seed", "stop_stress_ticks", "stop_stress_percent"});
    command.kind = TradingCommand::Kind::ResetAccount;
    command.program_costs = true;
    command.restore_scaled_rules = true;
    std::uint64_t cost_bit = 1;
    for (const auto* field : {"evaluation_fee", "reset_fee", "activation_fee", "max_resets"}) {
      if (body.contains(field) || (body.contains("rules") && body.at("rules").contains(field)))
        command.program_cost_overrides |= cost_bit;
      cost_bit <<= 1;
    }
    command.reason = reason_field(body);
    if (body.contains("plan")) {
      if (body.contains("initial_cash") || body.contains("rules"))
        throw std::invalid_argument("plan excludes initial_cash and rules");
      const auto* plan = find_plan(string_field(body, "plan"));
      if (!plan) throw std::invalid_argument("Unknown plan; see GET /api/plans");
      command.initial_cash = plan->initial_cash;
      command.rules = plan->rules;
      if (!plan->unlocked_by.empty()) command.required_pass = find_plan(plan->unlocked_by)->name;
    } else {
      if (!body.contains("initial_cash") || !body.contains("rules"))
        throw std::invalid_argument("Provide a plan, or both initial_cash and rules");
      command.initial_cash = decimal_field(body, "initial_cash");
      if (command.initial_cash <= Money{}) throw std::invalid_argument("initial_cash must be positive");
      command.rules = parse_rules(body.at("rules"));
      check_plan_name(command.initial_cash, command.rules);
      if (const auto* preset = find_plan_named(command.rules.plan); preset && !preset->unlocked_by.empty())
        command.required_pass = find_plan(preset->unlocked_by)->name;
    }
    program_cost_fields(body, command.rules);
    fill_model(body, command.rules);
    fee_model(body, command.rules);
    margin_model(body, command.rules);
  } else if (path == "/api/market/halt") {
    fields(body, {"symbol", "minutes"});
    command.kind = TradingCommand::Kind::Halt;
    command.symbol = string_field(body, "symbol");
    command.quantity = integer_field(body, "minutes");
    if (command.symbol.empty() || command.quantity < 1 || command.quantity > 390)
      throw std::invalid_argument("A halt needs a symbol and 1-390 minutes");
  } else if (path == "/api/account/cash-flows") {
    fields(body, {"client_id", "amount"});
    command.kind = TradingCommand::Kind::CashTransfer;
    command.name = string_field(body, "client_id");
    command.amount = decimal_field(body, "amount");
  } else if (path == "/api/account/payout") {
    fields(body, {"amount"});
    command.kind = TradingCommand::Kind::Payout;
    command.amount = decimal_field(body, "amount");
  } else {
    fields(body, {"symbol", "value"});
    command.kind = TradingCommand::Kind::Settle;
    command.symbol = symbol_field(body);
    command.settlement = decimal_field(body, "value");
  }
  return command;
}
namespace {
/// Accepts the whole body through nlohmann's own lexer (which also checks UTF-8)
/// without building a value, and stops at the first level past the limit.
struct RequestJsonCheck final : json::json_sax_t {
  int depth = 0;
  bool too_deep = false;
  bool open() { too_deep = ++depth > kRequestJsonDepthMax; return !too_deep; }
  bool null() override { return true; }
  bool boolean(bool) override { return true; }
  bool number_integer(number_integer_t) override { return true; }
  bool number_unsigned(number_unsigned_t) override { return true; }
  bool number_float(number_float_t, const string_t&) override { return true; }
  bool string(string_t&) override { return true; }
  bool binary(binary_t&) override { return true; }
  bool start_object(std::size_t) override { return open(); }
  bool key(string_t&) override { return true; }
  bool end_object() override { --depth; return true; }
  bool start_array(std::size_t) override { return open(); }
  bool end_array() override { --depth; return true; }
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override { return false; }
};
}  // namespace
}  // namespace paper_internal

std::optional<ApiResponse> check_request_json(std::string_view body) {
  if (body.empty()) return std::nullopt;
  RequestJsonCheck check;
  if (json::sax_parse(body, &check)) return std::nullopt;
  if (check.too_deep)
    return api_error(400, "INVALID_REQUEST", "Request body nests deeper than " + std::to_string(kRequestJsonDepthMax) + " levels");
  return api_error(400, "INVALID_REQUEST", "Request body must be valid JSON in UTF-8");
}

std::optional<std::map<std::string, std::string>> query_parameters(std::string_view query, bool plus_is_space) {
  const auto decode = [plus_is_space](std::string_view text) -> std::optional<std::string> {
    std::string decoded;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != '%') { decoded += plus_is_space && text[i] == '+' ? ' ' : text[i]; continue; }
      if (i + 2 >= text.size()) return std::nullopt;
      unsigned int byte = 0;
      const auto* digits = text.data() + i + 1;
      const auto [end, error] = std::from_chars(digits, digits + 2, byte, 16);
      if (error != std::errc{} || end != digits + 2) return std::nullopt;
      decoded += static_cast<char>(byte);
      i += 2;
    }
    // A route may echo a value in its JSON answer, which must be UTF-8: a value that
    // is not is malformed here, a 400, rather than a 500 when the answer is printed.
    if (!md::valid_utf8(decoded)) return std::nullopt;
    return decoded;
  };
  std::map<std::string, std::string> pairs;
  while (!query.empty()) {
    const auto amp = query.find('&');
    const auto pair = query.substr(0, amp);
    const auto equals = pair.find('=');
    auto key = decode(pair.substr(0, equals));
    auto value = decode(equals == std::string_view::npos ? std::string_view{} : pair.substr(equals + 1));
    if (!key || !value || !pairs.emplace(std::move(*key), std::move(*value)).second) return std::nullopt;
    if (amp == std::string_view::npos) break;
    query.remove_prefix(amp + 1);
  }
  return pairs;
}

std::optional<std::string> query_account(std::string_view target, std::string_view default_account) {
  const auto question = target.find('?');
  const auto pairs = query_parameters(question == std::string_view::npos ? std::string_view{} : target.substr(question + 1));
  if (!pairs) return std::nullopt;
  const auto account = pairs->find("account");
  if (account == pairs->end()) return std::string(default_account.empty() ? kMainAccount : default_account);
  if (!valid_account(account->second)) return std::nullopt;
  return account->second;
}
}  // namespace openport::server
