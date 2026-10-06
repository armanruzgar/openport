#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "openport/md/time.hpp"
#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig config(AccountRules rules) {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
AccountRules plan(std::string_view target = "0", std::string_view max_drawdown = "0") {
  AccountRules r;
  r.plan = "Plan rules test";
  r.profit_target = m(target);
  r.max_drawdown = m(max_drawdown);
  return r;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
/// Rolls over to `day` at 10:00 on the marks the old day closed on, then quotes the new day.
void next_day(TradingSession& s, ScriptedMarket& f, md::Date day, std::string_view bid = "4.00", std::string_view ask = "4.20") {
  f.time = md::new_york_to_utc(day, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  quote(s, f, bid, ask);
}
class CapturingJournal final : public Journal {
 public:
  std::vector<std::string> payloads;
  void append(Timestamp, std::string_view, std::string_view payload) override { payloads.emplace_back(payload); }
  std::uint64_t sequence() const override { return payloads.size(); }
  std::string head() const override { return std::string(64, '0'); }
};
/// A journal file in a fresh temporary directory, removed afterwards.
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-plan-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};
const Objective* objective(const std::vector<Objective>& list, Reason code) {
  for (const auto& o : list) if (o.code == code) return &o;
  return nullptr;
}

AccountRules scaling_plan() {
  auto r = plan();
  r.phase = Phase::Funded;
  r.payouts.qualifying_days = 1;
  r.scaling = {{m("0"), 2}, {m("100"), 3}, {m("200"), 5}};
  return r;
}

TEST(PlanRules, VerificationContractScalingStillPassesAndNeverEnablesFundedGrowth) {
  ScriptedMarket f;
  auto rules = plan("300", "1000");
  rules.phase = Phase::Verification;
  rules.profit_basis = ProfitBasis::Balance;
  rules.scaling = {{m("0"), 1}, {m("100"), 2}};
  rules.time_limit_days = 30;
  JournalFile file;
  TradingSession s(config(rules), f.time, FileJournal::create(file.path));
  f.seed(s);
  EXPECT_EQ(s.submit(f.market("too-big", 2), f.time).decision.code, Reason::SCALING_LIMIT);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  quote(s, f, "6.20", "6.40");
  ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 23});
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 2);
  EXPECT_FALSE(s.snapshot()->evaluation.size_scaling);
  EXPECT_EQ(s.request_payout(m("10"), f.time).decision.code, Reason::PAYOUT_UNAVAILABLE);
  ASSERT_TRUE(s.submit(f.market("second-open", 2), f.time).decision.ok());
  quote(s, f, "5.20", "5.40");
  ASSERT_TRUE(s.submit(f.market("second-close", 2, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}

TEST(TradingPlanRules, BothContractCapsUseTheSameWorkingOpeningCount) {
  for (const auto fixed : {1, 3}) {
    ScriptedMarket f;
    auto rules = scaling_plan();
    rules.max_contracts_held = fixed;
    TradingSession s(config(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.limit("reserved", 1, "4.00"), f.time).decision.ok());
    const auto result = s.submit(f.limit("too-many", 2, "4.00"), f.time);
    EXPECT_EQ(result.decision.code, fixed == 1 ? Reason::MAX_CONTRACTS_HELD : Reason::SCALING_LIMIT);
    EXPECT_EQ(result.decision.actual, 3);
    EXPECT_EQ(result.decision.limit, fixed == 1 ? 1 : 2);
  }
}

TEST(TradingPlanRules, ScalingReductionAllowanceDoesNotBypassTheFixedCap) {
  ScriptedMarket f;
  auto rules = scaling_plan();
  rules.max_contracts_held = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
  // Reversing two longs into one short does not increase gross holdings, so F39
  // allows it. F15 still counts the opening short alongside the held contracts.
  const auto reverse = s.submit(f.market("reverse", 3, Side::Sell), f.time);
  EXPECT_EQ(reverse.decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(reverse.decision.actual, 3);
  EXPECT_EQ(reverse.decision.limit, 2);
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 2);
}

TEST(PlanRules, ScalingCountsHeldContractsWorkingOpeningsAndComboLegs) {
  ScriptedMarket f, g;
  g.contract = *md::parse_osi("SPXW261022C05010000");
  TradingSession s(config(scaling_plan()), f.time);
  f.seed(s); g.seed(s);
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 2);
  const auto refused = s.submit(f.market("too-big", 3), f.time);
  EXPECT_EQ(refused.decision.code, Reason::SCALING_LIMIT);
  EXPECT_EQ(refused.decision.actual, 3); EXPECT_EQ(refused.decision.limit, 2);
  EXPECT_EQ(s.preview(f.market("preview", 3), f.time).decision.code, Reason::SCALING_LIMIT);
  EXPECT_EQ(s.what_if({{f.market("what-if", 3)}}, f.time).candidates.front().decision.code, Reason::SCALING_LIMIT);
  auto combo = f.limit("combo", 2, "0.20");
  combo.symbol.clear(); combo.legs = {{f.symbol(), Side::Buy, 1}, {g.symbol(), Side::Sell, 1}};
  auto result = s.submit(combo, f.time);
  EXPECT_EQ(result.decision.code, Reason::SCALING_LIMIT); EXPECT_EQ(result.decision.actual, 4);
  const auto working = s.submit(f.limit("working", 1, "4.00"), f.time);
  ASSERT_TRUE(working.decision.ok());
  EXPECT_EQ(s.submit(g.market("reserved", 2), f.time).decision.code, Reason::SCALING_LIMIT);
  ASSERT_TRUE(s.cancel(*working.order_id, f.time).decision.ok());
  auto armed = f.market("armed", 2);
  armed.trigger = Trigger{TriggerSource::Option, TriggerDirection::AtOrAbove, m("5")};
  ASSERT_TRUE(s.submit(armed, f.time).decision.ok());
  EXPECT_EQ(s.submit(g.market("armed-reserved"), f.time).decision.code, Reason::SCALING_LIMIT);
}

TEST(PlanRules, ScalingCountsPartiallyFilledOrdersAndOtherClosingReservations) {
  ScriptedMarket f;
  TradingSession s(config(scaling_plan()), f.time);
  f.seed(s, "4.00", "4.20", 1);
  const auto partial = s.submit(f.limit("partial", 2), f.time);
  ASSERT_TRUE(partial.decision.ok());
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 1);
  EXPECT_EQ(s.submit(f.market("extra"), f.time).decision.code, Reason::SCALING_LIMIT);
  ASSERT_TRUE(s.cancel(*partial.order_id, f.time).decision.ok());
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("second"), f.time).decision.ok());
  const auto close = s.submit(f.limit("close", 2, "4.50", Side::Sell), f.time);
  ASSERT_TRUE(close.decision.ok());
  // Pending closes do not free capacity for a new opening.
  EXPECT_EQ(s.submit(f.market("after-close"), f.time).decision.code, Reason::SCALING_LIMIT);
  ASSERT_TRUE(s.cancel(*close.order_id, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("reduce", 2, Side::Sell), f.time).decision.ok());
}

TEST(PlanRules, ScalingRechecksOtoChildrenAndDoesNotCountShares) {
  ScriptedMarket f;
  TradingSession s(config(scaling_plan()), f.time);
  f.seed(s);
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("10")}).decision.ok());
  auto parent = f.market("parent", 2);
  parent.then = {f.market("", 1)};
  ASSERT_TRUE(s.submit(parent, f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders[1].status, OrderStatus::Rejected);
  EXPECT_EQ(s.snapshot()->recent_orders[1].reason.code, Reason::SCALING_LIMIT);
  ASSERT_TRUE(s.submit(f.market("reduce", 2, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->stocks.front().position.shares, 100);
}

TEST(PlanRules, ScalingUsesClosedBalanceOnlyAtRolloverAndKeepsHoldingsOnAStepDown) {
  ScriptedMarket f;
  JournalFile file;
  TradingSession s(config(scaling_plan()), f.time, FileJournal::create(file.path));
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  quote(s, f, "5.50", "5.70");
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 2); // Unrealised gains never raise it.
  ASSERT_TRUE(s.submit(f.market("profit", 2, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(plan_inputs(*s.snapshot()).balance, m("10257.40"));
  EXPECT_EQ(s.submit(f.market("same-day", 3), f.time).decision.code, Reason::SCALING_LIMIT);
  next_day(s, f, {2026, 9, 23}, "5.50", "5.70");
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 5);
  auto recovered = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(recovered.snapshot()->evaluation.scaling_limit, 5);
  EXPECT_EQ(recovered.config().rules.scaling, scaling_plan().scaling);
  ASSERT_TRUE(s.submit(f.market("next-day", 5), f.time).decision.ok());
  quote(s, f, "3.50", "3.70");
  ASSERT_TRUE(s.submit(f.market("loss", 2, Side::Sell), f.time).decision.ok());
  EXPECT_LT(plan_inputs(*s.snapshot()).balance, m("10000"));
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 5);
  // A GTC opening fits now but cannot fill after the loss lowers the next day's cap.
  auto pending = s.submit(f.limit("pending", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(pending.decision.ok());
  next_day(s, f, {2026, 9, 24}, "3.30", "3.50");
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 2);
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_EQ(s.snapshot()->positions.front().position.quantity, 3);
  const auto& cancelled = s.snapshot()->recent_orders.at(*pending.order_id - 1);
  EXPECT_EQ(cancelled.status, OrderStatus::Cancelled);
  EXPECT_EQ(cancelled.reason.code, Reason::SCALING_LIMIT);
  EXPECT_EQ(cancelled.reason.actual, 4); EXPECT_EQ(cancelled.reason.limit, 2);
  EXPECT_EQ(s.submit(f.market("increase"), f.time).decision.code, Reason::SCALING_LIMIT);
  ASSERT_TRUE(s.submit(f.market("reduce", 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("flat", 2, Side::Sell), f.time).decision.ok());
  const auto recovery = FileJournal::read(file.path);
  bool raised = false, lowered = false;
  for (const auto& record : recovery.records) {
    const auto payloads = nlohmann::json::parse(record.payload);
    for (const auto& event : payloads.at("events")) {
      if (event.at("type") != "scaling_limit") continue;
      const auto& payload = event.at("payload");
      raised = raised || (payload.at("old") == 2 && payload.at("limit") == 5 && payload.at("profit") == m("257.40").micros());
      lowered = lowered || (payload.at("old") == 5 && payload.at("limit") == 2);
    }
  }
  EXPECT_TRUE(raised); EXPECT_TRUE(lowered);
  auto restored = TradingSession::recover(recovery);
  EXPECT_EQ(restored.snapshot()->evaluation.scaling_limit, 2);
  EXPECT_TRUE(s.reset_account(m("10000"), scaling_plan(), "new attempt", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.scaling_limit, 2);
}

TEST(PlanRules, ScalingValidationAndDisabledJournalCompatibility) {
  auto r = scaling_plan();
  for (const auto& steps : std::vector<std::vector<ScalingStep>>{
      {{m("1"), 2}}, {{m("0"), 0}}, {{m("0"), 10001}}, {{m("0"), 2}, {m("0"), 3}},
      {{m("0"), 2}, {m("-1"), 3}}, {{m("0"), 3}, {m("100"), 2}}, std::vector<ScalingStep>(17, {m("0"), 2})}) {
    r.scaling = steps;
    try { TradingSession s(config(r), ScriptedMarket{}.time); FAIL() << "Invalid scaling accepted"; }
    catch (const TradingError& e) { EXPECT_EQ(e.code(), Reason::INVALID_RULES); }
  }
  r.scaling = {{m("0"), 2}, {m("100"), 2}};
  r.phase = Phase::Evaluation;
  EXPECT_NO_THROW(TradingSession(config(r), ScriptedMarket{}.time));
  JournalFile file;
  ScriptedMarket f;
  {
    TradingSession s(config(plan()), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("old"), f.time).decision.ok());
  }
  const auto recovery = FileJournal::read(file.path);
  for (const auto& record : recovery.records) EXPECT_EQ(record.payload.find("scaling"), std::string::npos);
  const auto old = TradingSession::recover(recovery);
  EXPECT_TRUE(old.config().rules.scaling.empty());
  EXPECT_EQ(old.snapshot()->evaluation.scaling_limit, 0);
}

TEST(ProgramCosts, ExactFeesLimitsPurchasesAndRecoveryLeaveTheLedgerAlone) {
  ScriptedMarket f;
  auto rules = plan();
  rules.phase = Phase::Verification;
  rules.evaluation_fee = m("100.000001"); rules.reset_fee = m("25.000002"); rules.max_resets = 1;
  JournalFile file;
  TradingSession s(config(rules), f.time, FileJournal::create(file.path));
  EXPECT_EQ(program_costs(*s.snapshot(), rules).total(), m("100.000001"));
  EXPECT_EQ(s.snapshot()->equity, m("10000"));
  auto changed = rules;
  changed.slippage_ticks = 1; changed.reset_fee = m("30.000003");
  ASSERT_TRUE(s.reset_account(m("10000"), changed, "restart", f.time).decision.ok());
  auto costs = program_costs(*s.snapshot(), changed);
  EXPECT_EQ(costs.evaluation, m("100.000001")); EXPECT_EQ(costs.reset, m("30.000003"));
  EXPECT_EQ(costs.resets_used, 1); EXPECT_EQ(s.snapshot()->equity, m("10000"));
  const auto refused = s.reset_account(m("10000"), rules, "exhausted", f.time);
  EXPECT_EQ(refused.decision.code, Reason::RESET_LIMIT);
  EXPECT_EQ(refused.decision.actual, 2); EXPECT_EQ(refused.decision.limit, 1);
  EXPECT_EQ(s.snapshot()->attempts.size(), 1U);
  EXPECT_EQ(program_costs(*s.snapshot(), changed).total(), m("130.000004"));
  auto recovered = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(recovered.reset_account(m("10000"), rules, "still exhausted", f.time).decision.code, Reason::RESET_LIMIT);
  rules.plan = "Different purchase";
  ASSERT_TRUE(s.reset_account(m("10000"), rules, "purchase", f.time).decision.ok());
  costs = program_costs(*s.snapshot(), rules);
  EXPECT_EQ(costs.resets_used, 0); EXPECT_EQ(costs.total(), m("230.000005"));
  // A zero-cost attempt removes its current fee field, preserving the earlier fees.
  rules.plan = "Free practice"; rules.evaluation_fee = {}; rules.reset_fee = {}; rules.max_resets = 0;
  ASSERT_TRUE(s.reset_account(m("10000"), rules, "free", f.time).decision.ok());
  EXPECT_FALSE(Json::parse(s.snapshot_json()).contains("fee_charged"));
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
  for (int i = 0; i < 3; ++i) { ASSERT_TRUE(s.reset_account(m("10000"), rules, "unlimited", f.time).decision.ok()); }
  EXPECT_EQ(program_costs(*s.snapshot(), rules).resets_used, 3);
  // Re-encoding uses the reference JSON delta writer and must retain every byte.
  const auto recovery = FileJournal::read(file.path);
  const auto compacted = (file.directory / "compacted.jsonl").string();
  EXPECT_EQ(TradingSession::compact(recovery, *FileJournal::create(compacted)), s.snapshot_json());
  EXPECT_EQ(FileJournal::read(compacted).head, recovery.head);
}

TEST(ProgramCosts, TradingRestrictionsStartANewPurchaseButExecutionMarginAndCostsDoNot) {
  auto rules = plan(); rules.phase = Phase::Verification; rules.buying_power = true;
  rules.evaluation_fee = m("100.000001"); rules.reset_fee = m("25.000002"); rules.max_resets = 1;
  rules.trading_start = 9 * 60; rules.trading_end = 16 * 60;
  rules.microscalp_seconds = 30;
  const Json encoded = rules;
  const Json restrictions{{"time_limit_days", 30}, {"inactivity_days", 14}, {"underlyings", {"SPX"}},
      {"trading_start", 9 * 60 + 30}, {"trading_end", 15 * 60}, {"flat_time", 15 * 60 + 45},
      {"no_overnight", true}, {"trade_consistency_percent", 40}, {"min_trades", 2},
      {"min_hold_seconds", 60}, {"microscalp_seconds", 60}, {"microscalp_percent", 25},
      {"no_hedging", true}, {"no_counter_positions", true}, {"max_volume_percent", 25}};
  for (const auto& [key, value] : restrictions.items()) {
    SCOPED_TRACE(key);
    auto altered = encoded; altered[key] = value;
    const auto different = altered.get<AccountRules>();
    EXPECT_FALSE(same_program_rules(rules, different));
    JournalFile file;
    TradingSession s(config(rules), 0, FileJournal::create(file.path));
    ASSERT_TRUE(s.reset_account(m("10000"), rules, "last reset", 0).decision.ok());
    EXPECT_EQ(s.reset_account(m("10000"), rules, "exhausted", 0).decision.code, Reason::RESET_LIMIT);
    ASSERT_TRUE(s.reset_account(m("10000"), different, "new plan rules", 0).decision.ok());
    const auto costs = program_costs(*s.snapshot(), different);
    EXPECT_EQ(costs.resets_used, 0);
    EXPECT_EQ(costs.evaluation, m("200.000002"));
    EXPECT_EQ(costs.reset, m("25.000002"));
    EXPECT_EQ(s.snapshot()->equity, m("10000"));
    const auto recovered = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(recovered.config().rules, different);
    EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
    EXPECT_EQ(program_costs(*recovered.snapshot(), different).resets_used, 0);
  }
  auto execution = rules;
  execution.slippage_ticks = 1; execution.fill_latency_ms = 100; execution.impact_ticks = 2;
  execution.inside_fill_percent = 25; execution.fees = FeeSchedule{};
  execution.margin = MarginMode::Portfolio;
  execution.house_margin_percent = 20; execution.pm_vol_shock = 10;
  execution.evaluation_fee = m("200"); execution.reset_fee = m("50");
  execution.activation_fee = m("75"); execution.max_resets = 3;
  EXPECT_TRUE(same_program_rules(rules, execution));
  execution.margin = MarginMode::Strategy; execution.account_type = AccountType::Ira;
  EXPECT_TRUE(same_program_rules(rules, execution));
}

TEST(ProgramCosts, DefaultsKeepBytesAndNegativeOrFractionalSettingsAreRejected) {
  const auto rules = plan();
  const Json encoded = rules;
  for (const auto* key : {"evaluation_fee", "reset_fee", "activation_fee", "max_resets"}) { EXPECT_FALSE(encoded.contains(key)); }
  EXPECT_EQ(Json(encoded.get<AccountRules>()).dump(), encoded.dump());
  for (const auto* key : {"evaluation_fee", "reset_fee", "activation_fee", "max_resets"}) {
    auto bad = encoded; bad[key] = -1;
    EXPECT_THROW(validate_rules(bad.get<AccountRules>()), TradingError);
  }
  for (const auto* key : {"max_resets", "time_limit_days", "inactivity_days"}) {
    auto fractional = encoded; fractional[key] = 1.5;
    EXPECT_THROW(fractional.get<AccountRules>(), TradingError) << key;
  }
  TradingSession free(config(rules), 0);
  ASSERT_TRUE(free.reset_account(m("10000"), rules, "free", 0).decision.ok());
  EXPECT_FALSE(Json(free.snapshot()->attempts.front()).contains("fee_charged"));
  EXPECT_FALSE(Json::parse(free.snapshot_json()).contains("fee_charged"));
}

TEST(ProgramCosts, ActivationAndPayoutsSurviveResets) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.phase = Phase::Verification; rules.min_trading_days = 2;
  JournalFile file;
  TradingSession s(config(rules), f.time, FileJournal::create(file.path));
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  ASSERT_TRUE(s.submit(f.market("second", 1, Side::Sell), f.time).decision.ok());
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  auto funded = plan("0", "1000"); funded.phase = Phase::Funded; funded.payouts.qualifying_days = 1;
  funded.activation_fee = m("20.000001"); funded.evaluation_fee = m("999");
  ASSERT_TRUE(s.reset_account(m("10000"), funded, "activate", f.time, true).decision.ok());
  EXPECT_EQ(program_costs(*s.snapshot(), funded).activation, m("20.000001"));
  EXPECT_EQ(program_costs(*s.snapshot(), funded).evaluation, Money{});
  EXPECT_EQ(s.snapshot()->attempts.front().rules->phase, Phase::Verification);
  // Real payouts and an account reset keep the trader share across journal recovery.
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("funded open", 5), f.time).decision.ok());
  quote(s, f, "4.60", "4.80");
  ASSERT_TRUE(s.submit(f.market("funded close", 5, Side::Sell), f.time).decision.ok());
  next_day(s, f, {2026, 9, 24});
  ASSERT_TRUE(s.request_payout(m("50"), f.time).decision.ok());
  EXPECT_EQ(program_costs(*s.snapshot(), funded).payouts_received, m("40"));
  auto free = plan();
  ASSERT_TRUE(s.reset_account(m("10000"), free, "after payout", f.time, false, false).decision.ok());
  // Legacy resets did not archive payout totals; recover them from payout events without changing bytes.
  EXPECT_FALSE(Json(s.snapshot()->attempts.back()).contains("payouts_received"));
  const auto restored = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(restored.snapshot_json(), s.snapshot_json());
  const auto costs = program_costs(*restored.snapshot(), free);
  EXPECT_EQ(costs.payouts_received, m("40"));
  EXPECT_EQ(costs.payouts_received - costs.total(), m("19.999999"));
}

TEST(PlanRules, TheTradingDayEndsAtThePlansOwnTime) {
  // 17:30 New York time on a Tuesday counts toward Wednesday under the default
  // 17:00 end, but still toward Tuesday when the plan's day ends at 18:00.
  const auto evening = md::new_york_to_utc({2026, 9, 22}, 17, 30);
  EXPECT_EQ(md::trading_date(evening), (md::Date{2026, 9, 23}));
  EXPECT_EQ(md::trading_date(evening, 17 * 60), md::trading_date(evening));
  EXPECT_EQ(md::trading_date(evening, 18 * 60), (md::Date{2026, 9, 22}));
  EXPECT_EQ(md::trading_date(md::new_york_to_utc({2026, 9, 22}, 18, 0), 18 * 60), (md::Date{2026, 9, 23}));
  // A day that ends at midnight keeps the evening; a Saturday belongs to Monday.
  EXPECT_EQ(md::trading_date(md::new_york_to_utc({2026, 9, 22}, 23, 59), 24 * 60), (md::Date{2026, 9, 22}));
  EXPECT_EQ(md::trading_date(md::new_york_to_utc({2026, 9, 26}, 12, 0), 24 * 60), (md::Date{2026, 9, 28}));

  ScriptedMarket f;
  auto rules = plan("0", "100");
  rules.day_end_minutes = 18 * 60;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.trading_date(evening), (md::Date{2026, 9, 22}));
  // At 17:30 the plan's day has not ended, so there is nothing to roll over.
  EXPECT_EQ(s.roll_day(evening).decision.code, Reason::INVALID_TIME);
  const auto later = md::new_york_to_utc({2026, 9, 22}, 18, 5);
  EXPECT_EQ(s.trading_date(later), (md::Date{2026, 9, 23}));
  ASSERT_TRUE(s.roll_day(later).decision.ok());
  EXPECT_EQ(s.trading_day(), (md::Date{2026, 9, 23}));
  ASSERT_EQ(s.snapshot()->evaluation.days.size(), 1U);
  EXPECT_EQ(s.snapshot()->evaluation.days[0].day, (md::Date{2026, 9, 22}));

  for (const std::int64_t minutes : {std::int64_t{16 * 60 + 14}, std::int64_t{24 * 60 + 1}, std::int64_t{0}}) {
    rules.day_end_minutes = minutes;
    EXPECT_THROW(validate_rules(rules), TradingError) << minutes;
  }
  rules.day_end_minutes = 16 * 60 + 15;
  EXPECT_NO_THROW(validate_rules(rules));
  rules.day_end_minutes = 24 * 60;
  EXPECT_NO_THROW(validate_rules(rules));
}

TEST(PlanRules, AStaticFloorNeverMovesAndALockAtStartStopsAtTheStartingBalance) {
  ScriptedMarket f;
  auto rules = plan("0", "100");
  rules.drawdown_mode = DrawdownMode::Static;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("9900"));
  EXPECT_TRUE(s.snapshot()->evaluation.floor_locked);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.80", "5.00");  // equity 10346.75: the peak follows, the floor does not
  EXPECT_EQ(s.snapshot()->evaluation.peak, m("10346.75"));
  EXPECT_EQ(s.snapshot()->evaluation.floor, m("9900"));
  quote(s, f, "4.00", "4.20");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);  // 9946.75 > 9900
  quote(s, f, "3.90", "4.00");  // 9896.75 touches 9900
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::DRAWDOWN_FLOOR);

  ScriptedMarket g;
  auto locking = plan("0", "100");
  locking.lock_at_start = true;
  TradingSession t(config(locking), g.time);
  g.seed(t);
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("9900"));
  EXPECT_FALSE(t.snapshot()->evaluation.floor_locked);
  ASSERT_TRUE(t.submit(g.market("open", 5), g.time).decision.ok());
  quote(t, g, "4.20", "4.40");  // 10046.75: the floor trails to 9946.75
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("9946.75"));
  quote(t, g, "4.60", "4.80");  // 10246.75: peak - 100 passes the start, so it locks there
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("10000"));
  EXPECT_TRUE(t.snapshot()->evaluation.floor_locked);
  quote(t, g, "4.90", "5.10");
  EXPECT_EQ(t.snapshot()->evaluation.floor, m("10000"));

  auto invalid = plan("0", "100");
  invalid.drawdown_mode = DrawdownMode::Static;
  invalid.lock_at_start = true;
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid.lock_at_start = false;
  invalid.lock_balance = m("9950");
  EXPECT_THROW(validate_rules(invalid), TradingError);
  invalid.drawdown_mode = DrawdownMode::Intraday;
  invalid.lock_at_start = true;
  EXPECT_THROW(validate_rules(invalid), TradingError);
}

TEST(PlanRules, ADailyLossLimitFlattensAndLocksTheDayWithoutFailing) {
  ScriptedMarket f;
  auto rules = plan("1000", "1000");
  rules.daily_loss_limit = m("200");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("resting", 1, "3.50"), f.time).decision.ok());
  quote(s, f, "3.70", "3.90");  // 7896.75 + 5 * 100 * 3.80 = 9796.75, at or below 9800
  auto snap = s.snapshot();
  auto e = snap->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Active);
  EXPECT_EQ(e.day_lock, Reason::DAILY_LOSS_LIMIT);
  EXPECT_EQ(e.day_locked_at, f.time);
  EXPECT_EQ(snap->recent_orders[1].status, OrderStatus::Cancelled);
  EXPECT_EQ(snap->recent_orders[1].reason.code, Reason::DAILY_LOSS_LIMIT);
  const auto& close = snap->recent_orders.back();
  EXPECT_TRUE(close.system);
  EXPECT_TRUE(close.request.client_order_id.starts_with("system:daily_loss:")) << close.request.client_order_id;
  EXPECT_TRUE(snap->positions.empty());
  // Opening orders wait for the next trading day; the attempt goes on.
  EXPECT_EQ(s.submit(f.market("again"), f.time).decision.code, Reason::DAILY_LOSS_LIMIT);
  next_day(s, f, {2026, 9, 23});
  e = s.snapshot()->evaluation;
  EXPECT_EQ(e.day_lock, Reason::NONE);
  ASSERT_EQ(e.days.size(), 1U);
  EXPECT_EQ(e.days[0].locked, Reason::DAILY_LOSS_LIMIT);
  EXPECT_TRUE(s.submit(f.market("next-day"), f.time).decision.ok());
}

TEST(PlanRules, ALockedDayRecoversLockedAndRolloverReleasesIt) {
  ScriptedMarket f;
  auto rules = plan("1000", "1000");
  rules.daily_loss_limit = m("200");
  JournalFile file;
  TradingSession s(config(rules), f.time, FileJournal::create(file.path));
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "3.70", "3.90");
  ASSERT_EQ(s.snapshot()->evaluation.day_lock, Reason::DAILY_LOSS_LIMIT);
  // The lock is journaled with its code and level, and a restart keeps it.
  const auto recovery = FileJournal::read(file.path);
  bool announced = false;
  for (const auto& record : recovery.records) {
    const auto payload = nlohmann::json::parse(record.payload);
    for (const auto& event : payload.at("events"))
      if (event.at("type") == "day_locked") {
        announced = event.at("payload").at("code") == "DAILY_LOSS_LIMIT" && event.at("payload").at("level") == m("9800").micros();
        EXPECT_TRUE(announced) << event.dump();
      }
  }
  EXPECT_TRUE(announced);
  auto restored = TradingSession::recover(recovery);
  EXPECT_EQ(restored.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(restored.snapshot()->evaluation.day_lock, Reason::DAILY_LOSS_LIMIT);
  EXPECT_EQ(restored.snapshot()->evaluation.day_locked_at, s.snapshot()->evaluation.day_locked_at);
  EXPECT_EQ(restored.submit(f.market("after-restart"), f.time).decision.code, Reason::DAILY_LOSS_LIMIT);
  next_day(restored, f, {2026, 9, 23});
  EXPECT_EQ(restored.snapshot()->evaluation.day_lock, Reason::NONE);
  EXPECT_EQ(restored.snapshot()->evaluation.days.at(0).locked, Reason::DAILY_LOSS_LIMIT);
  EXPECT_TRUE(restored.submit(f.market("next-day"), f.time).decision.ok());
}

TEST(PlanRules, ADailyLossLimitCanFailTheAttemptAndRecoversWithItsCode) {
  ScriptedMarket f;
  auto rules = plan("1000", "1000");
  rules.daily_loss_limit = m("200");
  rules.daily_loss_action = BreachAction::Fail;
  JournalFile file;
  TradingSession s(config(rules), f.time, FileJournal::create(file.path));
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "3.70", "3.90");
  const auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_EQ(e.decision_code, Reason::DAILY_LOSS_LIMIT);
  EXPECT_NE(e.decision.find("daily loss limit at $9800.00 ($200.00 below the day's opening equity, $10000.00)"), std::string::npos)
      << e.decision;
  EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:daily_loss:"));
  // The decision's code is journaled with it, and recovery restores it.
  const auto recovery = FileJournal::read(file.path);
  const auto failed = nlohmann::json::parse(recovery.records.back().payload);
  bool coded = false;
  for (const auto& event : failed.at("events"))
    if (event.at("type") == "evaluation_failed") coded = event.at("payload").at("code") == "DAILY_LOSS_LIMIT";
  EXPECT_TRUE(coded) << failed.dump();
  const auto restored = TradingSession::recover(recovery);
  EXPECT_EQ(restored.snapshot()->evaluation.decision_code, Reason::DAILY_LOSS_LIMIT);
  EXPECT_EQ(restored.snapshot_json(), s.snapshot_json());
}

TEST(PlanRules, DailyLossBasesMeasureFromBalanceEquityTheHigherOrTheDaysHigh) {
  // The day's high: a gain then a give-back reaches a limit trailing it.
  for (const auto basis : {DailyLossBasis::Equity, DailyLossBasis::Peak}) {
    ScriptedMarket f;
    auto rules = plan();
    rules.daily_loss_limit = m("150");
    rules.daily_loss_basis = basis;
    TradingSession s(config(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    quote(s, f, "4.40", "4.60");  // 10146.75
    quote(s, f, "4.00", "4.20");  // 9946.75: 200 off the high, 53.25 off the open
    EXPECT_EQ(s.snapshot()->evaluation.day_lock, basis == DailyLossBasis::Peak ? Reason::DAILY_LOSS_LIMIT : Reason::NONE);
  }
  // Held overnight, a winner opens the next day with equity above the balance.
  for (const auto basis : {DailyLossBasis::Equity, DailyLossBasis::Balance, DailyLossBasis::Higher}) {
    ScriptedMarket f;
    auto rules = plan();
    rules.daily_loss_limit = m("100");
    rules.daily_loss_basis = basis;
    TradingSession s(config(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    quote(s, f, "4.40", "4.60");  // closes the day at 10146.75 with a 9996.75 balance
    next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
    const auto level = daily_loss_level(s.snapshot()->evaluation, rules, plan_inputs(*s.snapshot()));
    ASSERT_TRUE(level.has_value());
    EXPECT_EQ(level->reference, basis == DailyLossBasis::Balance ? m("9996.75") : m("10146.75"));
    quote(s, f, "4.10", "4.30");  // 9996.75
    EXPECT_EQ(s.snapshot()->evaluation.day_lock, basis == DailyLossBasis::Balance ? Reason::NONE : Reason::DAILY_LOSS_LIMIT);
  }
}

TEST(PlanRules, MinimumTradingDaysHoldAPassUntilEnoughDaysTraded) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.min_trading_days = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");  // 10146.75 reaches the target on the first day
  auto snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(snap->evaluation.day_executions, 1U);
  auto objectives = evaluation_objectives(snap->evaluation, rules, plan_inputs(*snap));
  ASSERT_NE(objective(objectives, Reason::PROFIT_TARGET), nullptr);
  EXPECT_TRUE(objective(objectives, Reason::PROFIT_TARGET)->met);
  ASSERT_NE(objective(objectives, Reason::MIN_TRADING_DAYS), nullptr);
  EXPECT_FALSE(objective(objectives, Reason::MIN_TRADING_DAYS)->met);
  EXPECT_EQ(objective(objectives, Reason::MIN_TRADING_DAYS)->actual, 1.0);
  // Holding into the next day is not trading on it.
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(s.snapshot()->evaluation.days.at(0).executions, 1U);
  // A trade on the second day passes at once.
  ASSERT_TRUE(s.submit(f.market("second", 1, Side::Sell), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(snap->evaluation.decision_code, Reason::PROFIT_TARGET);
}

TEST(PlanRules, SellingDeliveredSharesIsTradingButExercisingIsNot) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261022C00500000");
  auto rules = plan("1000", "1000");
  rules.min_trading_days = 3;
  TradingSession s(config(rules), f.time);
  auto valuation = f.valuation();
  valuation.spot = 505;
  valuation.forward = 505;
  s.define(f.contract, f.time);
  s.on_quotes({f.quote("6", "6.20")}, {valuation}, f.time, {{"SPY", f.time, m("505")}});
  ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(f.symbol(), 1, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.day_executions, 1U);
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("506")}});
  EXPECT_EQ(s.snapshot()->evaluation.day_executions, 0U);
  ASSERT_TRUE(s.trade_stock("SPY", -40, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.day_executions, 1U);
  // Flattening the rest is the trader's own trade too.
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.day_executions, 2U);
  const auto in = plan_inputs(*s.snapshot());
  EXPECT_EQ(day_stats(s.snapshot()->evaluation, rules, in).trading_days, 2U);
}

TEST(PlanRules, MinimumProfitableDaysCountDaysAboveTheThreshold) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.min_profitable_days = 2;
  rules.profitable_day_profit = m("100");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");  // +146.75 today
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  quote(s, f, "4.50", "4.70");  // +50 today: not profitable enough
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.60", "4.80");  // +100 today
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
}

TEST(PlanRules, AConsistencyRuleNeedsTheBestDayWithinItsShareOfTheTotal) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.consistency_percent = 50;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");  // one day of +146.75 is all of the profit
  auto snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  auto in = plan_inputs(*snap);
  const auto objectives = evaluation_objectives(snap->evaluation, rules, in);
  const auto* consistency = objective(objectives, Reason::CONSISTENCY);
  ASSERT_NE(consistency, nullptr);
  EXPECT_FALSE(consistency->met);
  EXPECT_EQ(consistency->actual, 100.0);
  EXPECT_EQ(consistency_target(snap->evaluation, rules, in), m("293.50"));
  next_day(s, f, {2026, 9, 23}, "4.40", "4.60");
  quote(s, f, "4.60", "4.80");  // +100: total 246.75, best day 146.75 is 59%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.70", "4.90");  // +150 today is the best day now: 150 / 296.75 is 50.5%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  quote(s, f, "4.68", "4.88");  // +140 today: 146.75 / 286.75 is 51%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 24}, "4.68", "4.88");
  quote(s, f, "4.72", "4.92");  // +20: 146.75 / 306.75 is 47.8%
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);

  // Measured against the profitable days alone, losing days do not dilute it.
  auto positive = rules;
  positive.consistency_basis = ConsistencyBasis::PositiveDays;
  Evaluation e;
  e.started = 1;
  e.starting_balance = m("10000");
  e.days.push_back({{2026, 9, 22}, m("10000"), m("10300"), {}, {}, {}, false, {}, {}, {}, 0, 0, 0, Reason::NONE});
  e.days.push_back({{2026, 9, 23}, m("10300"), m("10100"), {}, {}, {}, false, {}, {}, {}, 0, 0, 0, Reason::NONE});
  e.day_open_equity = m("10100");
  const PlanInputs now{m("10400"), m("10400"), {}, true};
  EXPECT_FALSE(objective(evaluation_objectives(e, rules, now), Reason::CONSISTENCY)->met);   // 300 of 400
  EXPECT_TRUE(objective(evaluation_objectives(e, positive, now), Reason::CONSISTENCY)->met); // 300 of 600
}

TEST(PlanRules, ConsistencyDescribesNonpositiveTotalProfitWithoutClaimingAShare) {
  auto rules = plan("3000");
  rules.consistency_percent = 40;
  Evaluation e;
  e.started = 1;
  e.starting_balance = m("10000");
  e.days.push_back({{2026, 9, 16}, m("10000"), m("10815.55"), {}, {}, {}, false, {}, {}, {}, 0, 0, 0, Reason::NONE});
  e.day_open_equity = m("10815.55");
  for (const auto total : {"-679.95", "0", "1000"}) {
    SCOPED_TRACE(total);
    const auto equity = e.starting_balance + m(total);
    const auto objectives = evaluation_objectives(e, rules, {equity, equity, {}, true});
    const auto* consistency = objective(objectives, Reason::CONSISTENCY);
    ASSERT_NE(consistency, nullptr);
    EXPECT_FALSE(consistency->met);
    const std::string prefix = "The best day, $815.55 on 2026-09-16, ";
    const std::string suffix = "; at most 40% may come from one day";
    if (m(total) > Money{}) {
      EXPECT_EQ(consistency->message, prefix + "is 82% of the total profit $1000.00" + suffix);
      ASSERT_TRUE(consistency->actual.has_value());
      EXPECT_NEAR(*consistency->actual, 81.555, 0.000001);
    } else {
      EXPECT_EQ(consistency->message, prefix + "exceeds the total profit " +
          (m(total) < Money{} ? "-$679.95" : "$0.00") + suffix);
      EXPECT_FALSE(consistency->actual.has_value());
    }
  }
}

TEST(PlanRules, ATargetOnTheClosedBalanceNeedsTheTradeClosed) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  rules.profit_basis = ProfitBasis::Balance;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  auto snap = s.snapshot();
  // Closing now sells at the 4.00 bid and pays the fees again.
  EXPECT_EQ(snap->equity, m("9946.75"));
  EXPECT_EQ(snap->exit_equity, m("9893.50"));
  quote(s, f, "4.60", "4.80");  // equity 10246.75 is above the target, the balance is not
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(plan_inputs(*snap).balance, m("9996.75"));
  ASSERT_TRUE(s.submit(f.market("close", 5, Side::Sell), f.time).decision.ok());
  snap = s.snapshot();
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(snap->evaluation.decided_equity, m("10193.50"));
  EXPECT_EQ(snap->evaluation.decision, "Balance $10193.50 reached the profit target $10100.00 with every position closed");
}

TEST(PlanRules, RulesThatAreOffKeepTheJournalUnchanged) {
  ScriptedMarket f;
  auto journal = std::make_shared<CapturingJournal>();
  TradingSession s(config(plan("100", "1000")), f.time, journal);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::PROFIT_TARGET);
  for (const auto& payload : journal->payloads) {
    for (const auto* key : {"decision_code", "day_executions", "day_lock", "daily_loss_limit", "consistency_percent",
                            "day_end_minutes", "profit_basis", "lock_at_start", "executions"})
      EXPECT_EQ(payload.find(key), std::string::npos) << key;
    const auto record = nlohmann::json::parse(payload);
    for (const auto& event : record.at("events"))
      if (event.at("type") == "evaluation_passed") { EXPECT_FALSE(event.at("payload").contains("code")); }
  }
  // The rules round-trip through the journal, each recorded only when set.
  auto rules = plan("100", "1000");
  rules.lock_at_start = true;
  rules.profit_basis = ProfitBasis::Balance;
  rules.daily_loss_limit = m("50");
  rules.daily_loss_basis = DailyLossBasis::Higher;
  rules.daily_loss_action = BreachAction::Fail;
  rules.consistency_percent = 40;
  rules.consistency_basis = ConsistencyBasis::PositiveDays;
  rules.min_trading_days = 3;
  rules.min_profitable_days = 2;
  rules.profitable_day_profit = m("25");
  rules.day_end_minutes = 18 * 60;
  JournalFile file;
  { TradingSession t(config(rules), f.time, FileJournal::create(file.path)); }
  const auto recovery = FileJournal::read(file.path);
  const auto recorded = nlohmann::json::parse(recovery.records.at(0).payload).at("state").at("config").at("rules");
  EXPECT_EQ(recorded.at("daily_loss_basis"), "higher");
  EXPECT_EQ(recorded.at("day_end_minutes"), 18 * 60);
  EXPECT_EQ(TradingSession::recover(recovery).config().rules, rules);
}

TEST(PlanRules, ADecidedAttemptAllowsUserClosesAndReservesTheirWorkingContracts) {
  ScriptedMarket f;
  TradingSession s(config(plan("0", "100")), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 3), f.time).decision.ok());
  f.next();
  s.on_quotes({{f.symbol(), f.observation, f.time, {}, m("0.05"), 0, 10}}, {f.valuation()}, f.time);
  const auto decided = s.snapshot()->evaluation;
  ASSERT_EQ(decided.status, EvaluationStatus::Failed);
  ASSERT_EQ(s.snapshot()->positions.at(0).position.quantity, 3);
  EXPECT_EQ(decided.closest_floor, decided.decided_equity - decided.floor);
  EXPECT_EQ(decided.closest_floor_at, decided.decided_at);
  EXPECT_EQ(s.submit(f.market("opening"), f.time).decision.code, Reason::EVALUATION_CLOSED);

  // A thin fresh bid lets the system sell one. The trader works the remaining
  // two at a higher limit after this update has exhausted its closing liquidity.
  f.next();
  s.on_quotes({f.quote("1.00", "1.20", 1)}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->positions.at(0).position.quantity, 2);
  const auto close = s.submit(f.limit("close", 2, "1.10", Side::Sell), f.time);
  ASSERT_TRUE(close.decision.ok()) << close.decision.message;
  EXPECT_EQ(s.submit(f.limit("duplicate-close", 1, "1.10", Side::Sell), f.time).decision.code, Reason::EVALUATION_CLOSED);
  quote(s, f, "1.00", "1.20");
  EXPECT_EQ(s.snapshot()->positions.at(0).position.quantity, 2);  // reserved for the trader
  for (int i = 0; i < 2; ++i) {
    f.next();
    s.on_quotes({f.quote("1.10", "1.30", 1)}, {f.valuation()}, f.time);
  }
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders.at(*close.order_id - 1).status, OrderStatus::Filled);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*close.order_id - 1).filled_quantity, 2);
  const auto& after = s.snapshot()->evaluation;
  EXPECT_EQ(after.status, decided.status);
  EXPECT_EQ(after.decided_at, decided.decided_at);
  EXPECT_EQ(after.decided_equity, decided.decided_equity);
  EXPECT_EQ(after.decision_code, decided.decision_code);
  EXPECT_EQ(after.closest_floor, decided.closest_floor);
  EXPECT_EQ(after.closest_floor_at, decided.closest_floor_at);
}

TEST(PlanRules, FlattenAndDisposalWorkAfterPassOrFailureWithoutABid) {
  for (const bool pass : {false, true}) {
    for (const bool flatten : {false, true}) {
      ScriptedMarket f;
      TradingSession s(config(plan("100", "100")), f.time);
      f.seed(s);
      ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
      f.next();
      s.on_quotes({{f.symbol(), f.observation, f.time, {}, m(pass ? "11" : "0.05"), 0, 10}}, {f.valuation()}, f.time);
      const auto decided = s.snapshot()->evaluation;
      ASSERT_EQ(decided.status, pass ? EvaluationStatus::Passed : EvaluationStatus::Failed);
      ASSERT_EQ(s.snapshot()->positions.size(), 1U);
      const auto closed = flatten ? s.close_positions({}, f.time) : s.abandon(f.symbol(), f.time);
      ASSERT_TRUE(closed.decision.ok()) << closed.decision.message;
      EXPECT_TRUE(s.snapshot()->positions.empty());
      const auto& after = s.snapshot()->evaluation;
      EXPECT_EQ(after.status, decided.status);
      EXPECT_EQ(after.decided_at, decided.decided_at);
      EXPECT_EQ(after.decided_equity, decided.decided_equity);
      EXPECT_EQ(after.decision_code, decided.decision_code);
      EXPECT_EQ(after.closest_floor, decided.closest_floor);
      EXPECT_EQ(after.closest_floor_at, decided.closest_floor_at);
      EXPECT_EQ(s.submit(f.market("opening"), f.time).decision.code, Reason::EVALUATION_CLOSED);
    }
  }
}

TEST(PlanRules, APendingSystemCloseLeavesWorkingUserClosesTheirContracts) {
  ScriptedMarket f;
  auto rules = plan("0", "100");
  rules.fill_latency_ms = 1000;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  quote(s, f, "4.00", "4.20");
  ASSERT_EQ(s.snapshot()->positions.at(0).position.quantity, 2);
  quote(s, f, "2.00", "2.20");
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
  ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);  // the delayed system IOC
  const auto close = s.submit(f.limit("user-close", 1, "2.10", Side::Sell), f.time);
  ASSERT_TRUE(close.decision.ok()) << close.decision.message;
  quote(s, f, "2.00", "2.20");
  ASSERT_EQ(s.snapshot()->positions.at(0).position.quantity, 1);
  quote(s, f, "2.10", "2.30");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders.at(*close.order_id - 1).filled_quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_fills.size(), 3U);
}

TEST(PlanRules, ClosestFloorIncludesTheDecisionButNotLiquidationOrLaterMarks) {
  for (const auto mode : {DrawdownMode::Intraday, DrawdownMode::EndOfDay}) {
    ScriptedMarket f;
    auto rules = plan("0", "100");
    rules.drawdown_mode = mode;
    TradingSession s(config(rules), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
    quote(s, f, "3.00", "3.20");
    const auto decided = s.snapshot()->evaluation;
    ASSERT_EQ(decided.status, EvaluationStatus::Failed);
    EXPECT_EQ(decided.closest_floor, decided.decided_equity - decided.floor);
    EXPECT_EQ(decided.closest_floor_at, decided.decided_at);
    EXPECT_LT(s.snapshot()->equity, decided.decided_equity);
    next_day(s, f, {2026, 9, 23});
    EXPECT_EQ(s.snapshot()->evaluation.closest_floor, decided.closest_floor);
    EXPECT_EQ(s.snapshot()->evaluation.closest_floor_at, decided.closest_floor_at);
  }
}

TEST(PlanRules, AttemptRulesAndDecisionRoundTripAndOldSummariesStayAbsent) {
  for (const auto status : {EvaluationStatus::Active, EvaluationStatus::Passed, EvaluationStatus::Failed}) {
    ScriptedMarket f;
    auto rules = plan("100", "1000");
    rules.daily_loss_limit = m("100");
    rules.daily_loss_action = BreachAction::Fail;
    rules.slippage_ticks = 1;
    rules.inside_fill_percent = 25;
    JournalFile file;
    Evaluation ended;
    std::string expected;
    {
      TradingSession s(config(rules), f.time, FileJournal::create(file.path));
      f.seed(s);
      ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
      if (status != EvaluationStatus::Active)
        quote(s, f, status == EvaluationStatus::Passed ? "5.50" : "2.00", status == EvaluationStatus::Passed ? "5.70" : "2.20");
      ended = s.snapshot()->evaluation;
      ASSERT_EQ(ended.status, status);
      f.next();  // Archiving later must not substitute the reset time for the decision.
      ASSERT_TRUE(s.reset_account(m("25000"), plan(), "next attempt", f.time).decision.ok());
      expected = s.snapshot_json();
    }
    auto restored = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(restored.snapshot_json(), expected);
    const auto& a = restored.snapshot()->attempts.at(0);
    EXPECT_EQ(a.rules, rules);
    EXPECT_EQ(a.decided_at, ended.decided_at);
    EXPECT_GT(a.ended, a.decided_at);
    EXPECT_EQ(a.decided_equity, status == EvaluationStatus::Active ? std::nullopt : std::optional(ended.decided_equity));
    EXPECT_EQ(a.peak, ended.peak);
    EXPECT_EQ(a.floor, ended.floor);
    EXPECT_EQ(a.decision_code, ended.decision_code);

    // Missing provenance in older summaries stays absent on re-encoding.
    auto old = nlohmann::json(a);
    for (const auto* field : {"rules", "decided_at", "decided_equity", "peak", "floor"}) old.erase(field);
    const auto summary = old.get<AttemptSummary>();
    EXPECT_EQ(nlohmann::json(summary).dump(), old.dump());
    EXPECT_FALSE(summary.rules);
    EXPECT_EQ(summary.decided_at, 0);
    EXPECT_FALSE(summary.decided_equity);
    EXPECT_FALSE(summary.peak);
    EXPECT_FALSE(summary.floor);

    // Load an old-format summary from an actual schema-2 journal checkpoint.
    CapturingJournal expanded;
    const auto records = FileJournal::read(file.path);
    TradingSession::expand(records, expanded);
    JournalFile older;
    {
      auto journal = FileJournal::create(older.path);
      for (std::size_t i = 0; i < expanded.payloads.size(); ++i) {
        auto payload = nlohmann::json::parse(expanded.payloads[i]);
        if (i + 1 == expanded.payloads.size()) {
          payload["state"]["attempts"][0] = old;
          payload["snapshot"]["attempts"][0] = old;
        }
        journal->append(records.records[i].time, records.records[i].type, payload.dump());
      }
    }
    auto recovered = TradingSession::recover(FileJournal::read(older.path));
    EXPECT_EQ(nlohmann::json(recovered.snapshot()->attempts.at(0)).dump(), old.dump());
    ASSERT_TRUE(recovered.reset_account(m("50000"), plan(), "continue old history", f.time).decision.ok());
    EXPECT_EQ(nlohmann::json(recovered.snapshot()->attempts.at(0)).dump(), old.dump());
  }
}

TEST(PlanRules, PassOddsHonourTheMinimumDays) {
  ScriptedMarket f;
  auto rules = plan("100", "1000");
  Evaluation e;
  e.started = f.time;
  e.starting_balance = m("10000");
  e.peak = e.starting_balance;
  e.floor = m("9000");
  e.day = {2026, 9, 22};
  e.day_open_equity = e.starting_balance;
  std::vector<EvaluationDay> history;
  for (int i = 0; i < 12; ++i) {
    EvaluationDay day;
    day.day = md::date_from_days(md::days_since_epoch({2026, 8, 1}) + i);
    day.open_equity = m("10000");
    day.close_equity = m("10060");
    day.low_equity = m("9990");
    day.high_equity = m("10070");
    day.low_at = 1;
    day.high_at = 2;
    history.push_back(day);
  }
  const auto free = pass_odds(e, rules, m("10000"), history, 10, 100, 7);
  EXPECT_EQ(free.pass, 1.0);
  EXPECT_EQ(free.median_days_to_pass, 2.0);
  rules.min_trading_days = 5;
  const auto held = pass_odds(e, rules, m("10000"), history, 10, 100, 7);
  EXPECT_EQ(held.pass, 1.0);
  EXPECT_EQ(held.median_days_to_pass, 5.0);
  rules.min_trading_days = 20;
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 0.0);
  rules.min_trading_days = 0; rules.min_trades = 1;
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 0.0);
  e.closed_trades = 1;
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 1.0);
  rules.trade_consistency_percent = 50; e.best_trade = BestTrade{"1", m("400")};
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 0.0);
  e.best_trade->pnl = m("100");
  rules.microscalp_seconds = 60; rules.microscalp_percent = 25; e.short_profit = m("400");
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 0.0);
  e.short_profit = m("25");
  EXPECT_EQ(pass_odds(e, rules, m("10000"), history, 10, 100, 7).pass, 1.0);
}

}  // namespace
}  // namespace openport::trading
