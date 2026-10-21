#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config(AccountRules rules = {}) {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-trade-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};

OrderRequest stopped(OrderRequest request, std::string_view level = "3.00", TriggerSource source = TriggerSource::Option) {
  const auto direction = multi_leg(request) || request.side == Side::Sell ? TriggerDirection::AtOrAbove : TriggerDirection::AtOrBelow;
  request.bracket = Bracket{ExitSpec{Trigger{source, direction, m(level)}, {}}, {}};
  return request;
}

class TradePhases : public ::testing::TestWithParam<Phase> {};
INSTANTIATE_TEST_SUITE_P(NonFunded, TradePhases,
    ::testing::Values(Phase::Evaluation, Phase::Verification),
    [](const ::testing::TestParamInfo<Phase>& param) {
      return param.param == Phase::Verification ? "Verification" : "Evaluation";
    });

TEST_P(TradePhases, TradeConsistencyUsesExactProfitAndHoldsWithoutAProfitableTrade) {
  AccountRules rules; rules.phase = GetParam(); rules.trade_consistency_percent = 50;
  Evaluation e; e.starting_balance = m("10000");
  PlanInputs now{m("10200"), m("10200"), m("200"), true};
  EXPECT_TRUE(evaluation_objectives(e, rules, now).back().met);
  e.best_trade = BestTrade{"17", m("100")};
  auto objective = evaluation_objectives(e, rules, now).back();
  EXPECT_TRUE(objective.met);
  EXPECT_EQ(objective.actual, 50);
  EXPECT_NE(objective.message.find("17"), std::string::npos);
  now.equity = m("10199.999999");
  EXPECT_FALSE(evaluation_objectives(e, rules, now).back().met);
  now.equity = m("9999");
  EXPECT_FALSE(evaluation_objectives(e, rules, now).back().actual);
  rules.profit_basis = ProfitBasis::Balance;
  EXPECT_TRUE(evaluation_objectives(e, rules, now).back().met);
  rules.phase = Phase::Funded;
  EXPECT_TRUE(evaluation_objectives(e, rules, now).empty());
  rules.trade_consistency_percent = 101;
  EXPECT_THROW(validate_rules(rules), TradingError);
}

TEST_P(TradePhases, BestWholeTradeWaitsForEveryLegAndRecovers) {
  ScriptedMarket f, g; g.contract.strike += 5;
  AccountRules rules; rules.phase = GetParam(); rules.trade_consistency_percent = 50; rules.min_trades = 2; rules.profit_target = m("10000");
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s); g.seed(s);
    auto combo = f.market("combo");
    combo.symbol.clear(); combo.legs = {{f.symbol(), Side::Buy, 1}, {g.symbol(), Side::Buy, 1}};
    ASSERT_TRUE(s.submit(combo, f.time).decision.ok());
    f.next(); g.next(); f.seed(s, "5.00", "5.20"); g.seed(s, "5.00", "5.20");
    ASSERT_TRUE(s.submit(f.market("first-close", 1, Side::Sell), f.time).decision.ok());
    EXPECT_FALSE(s.snapshot()->evaluation.best_trade);
    EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 0);
    ASSERT_TRUE(s.submit(g.market("last-close", 1, Side::Sell), g.time).decision.ok());
    ASSERT_TRUE(s.snapshot()->evaluation.best_trade);
    EXPECT_EQ(s.snapshot()->evaluation.best_trade->pnl, m("157.40"));
  }
  const auto recovery = FileJournal::read(file.path);
  auto s = TradingSession::recover(recovery, FileJournal::resume(file.path));
  ASSERT_TRUE(s.snapshot()->evaluation.best_trade);
  EXPECT_EQ(s.snapshot()->evaluation.best_trade->id, "1");
  EXPECT_EQ(s.snapshot()->evaluation.best_trade->pnl, m("157.40"));
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 1);
  f.next(); f.seed(s);
  EXPECT_EQ(s.snapshot()->evaluation.best_trade->pnl, m("157.40"));
}

TEST_P(TradePhases, MinimumTradesCountsLossesAndFlattenOnce) {
  ScriptedMarket f;
  AccountRules rules; rules.phase = GetParam(); rules.min_trades = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("partial", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 0);
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 1);
  EXPECT_FALSE(s.snapshot()->evaluation.best_trade);
  auto objectives = evaluation_objectives(s.snapshot()->evaluation, rules, {});
  EXPECT_FALSE(objectives.back().met);
  ASSERT_TRUE(s.submit(f.market("again"), f.time).decision.ok());
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  objectives = evaluation_objectives(s.snapshot()->evaluation, rules, {});
  EXPECT_TRUE(objectives.back().met);
  EXPECT_EQ(objectives.back().actual, 2);
  EXPECT_EQ(objectives.back().required, 2);
  rules.min_trades = 10001;
  EXPECT_THROW(validate_rules(rules), TradingError);
}

TEST_P(TradePhases, CachedObjectivesMatchColdRecoveryAfterTradesAndMarketTicks) {
  ScriptedMarket f;
  AccountRules rules; rules.phase = GetParam();
  rules.trade_consistency_percent = 50;
  rules.min_trades = 10;
  rules.microscalp_seconds = 60;
  rules.microscalp_percent = 50;
  rules.profit_target = m("10000");
  JournalFile file;
  TradingSession cached(config(rules), f.time, FileJournal::create(file.path));
  // Recovery deliberately drops all derived caches. Applying the same command
  // to it is an uncached oracle, including every intermediate observation.
  const auto check = [&](auto action) {
    auto cold = TradingSession::recover(FileJournal::read(file.path));
    action(cold);
    action(cached);
    EXPECT_EQ(cached.snapshot_json(), cold.snapshot_json());
  };
  check([&](auto& s) { f.seed(s); });
  check([&](auto& s) { s.submit(f.market("open", 2), f.time); });
  for (int tick = 0; tick < 3; ++tick) {
    f.next();
    check([&](auto& s) { f.seed(s, "5.00", "5.20"); });
  }
  check([&](auto& s) { s.submit(f.market("partial", 1, Side::Sell), f.time); });
  check([&](auto& s) { s.close_positions({}, f.time); });
  EXPECT_EQ(cached.snapshot()->evaluation.closed_trades, 1);
  EXPECT_GT(cached.snapshot()->evaluation.short_profit, Money{});
  check([&](auto& s) { s.trade_stock("SPY", 2, f.time, StockPrice{"SPY", f.time, m("500")}); });
  f.next();
  check([&](auto& s) { s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("501")}}); });
  check([&](auto& s) { s.trade_stock("SPY", -2, f.time); });
  check([&](auto& s) { s.reset_account(m("10000"), rules, "next attempt", f.time); });
  EXPECT_EQ(cached.snapshot()->evaluation.closed_trades, 0);
  EXPECT_EQ(cached.snapshot()->evaluation.short_profit, Money{});
  for (const auto& record : FileJournal::read(file.path).records) {
    EXPECT_EQ(record.payload.find("trade_objectives"), std::string::npos);
  }
}

TEST_P(TradePhases, MinimumHoldUsesFirstFillExactMarketAgeAndRecovers) {
  ScriptedMarket f;
  AccountRules rules; rules.phase = GetParam(); rules.min_hold_seconds = 60;
  JournalFile file;
  const auto opened = f.time;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
    for (const auto& [age, text] : {std::pair{0LL, "0 seconds"}, {1'000'000'000LL, "1 second"},
                                  {12'500'000'000LL, "12.5 seconds"}, {12'540'000'000LL, "12.5 seconds"},
                                  {12'560'000'000LL, "12.6 seconds"}}) {
      f.time = opened + age; ++f.observation; f.seed(s);
      const auto preview = s.preview(f.market("early", 1, Side::Sell), f.time);
      EXPECT_EQ(preview.decision.code, Reason::MIN_HOLD);
      EXPECT_EQ(preview.decision.message, std::string("Held ") + text +
          "; this plan requires 60 seconds before a user reduction");
      EXPECT_EQ(preview.decision.actual, static_cast<double>(age) / md::kNanosPerSecond);
      EXPECT_EQ(preview.decision.limit, 60);
    }
    f.time = opened + 30 * md::kNanosPerSecond; ++f.observation; f.seed(s);
    ASSERT_TRUE(s.submit(f.market("add"), f.time).decision.ok());
    const auto preview = s.preview(f.market("early", 1, Side::Sell), f.time);
    EXPECT_EQ(preview.decision.code, Reason::MIN_HOLD);
    EXPECT_EQ(preview.decision.actual, 30);
    EXPECT_EQ(preview.decision.limit, 60);
    EXPECT_EQ(preview.decision.scope, f.symbol());
    EXPECT_EQ(s.submit(f.market("early", 1, Side::Sell), f.time).decision.code, Reason::MIN_HOLD);
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  f.time = opened + 60 * md::kNanosPerSecond - 1; ++f.observation; f.seed(s);
  EXPECT_EQ(s.preview(f.market("boundary", 1, Side::Sell), f.time).decision.code, Reason::MIN_HOLD);
  ++f.time; ++f.observation; f.seed(s);
  EXPECT_TRUE(s.submit(f.market("boundary", 1, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("final", 1, Side::Sell), f.time).decision.ok());
}

TEST(TradeRules, MinimumHoldRejectsAnyYoungComboLegAndShareReversal) {
  ScriptedMarket f, g; g.contract.strike += 5;
  AccountRules rules; rules.min_hold_seconds = 60;
  TradingSession s(config(rules), f.time);
  f.seed(s); g.seed(s);
  ASSERT_TRUE(s.submit(f.market("old"), f.time).decision.ok());
  f.time += 60 * md::kNanosPerSecond; ++f.observation; g.time = f.time; ++g.observation;
  f.seed(s); g.seed(s);
  ASSERT_TRUE(s.submit(g.market("young"), f.time).decision.ok());
  auto combo = f.market("close"); combo.symbol.clear();
  combo.legs = {{f.symbol(), Side::Sell, 1}, {g.symbol(), Side::Sell, 1}};
  EXPECT_EQ(s.preview(combo, f.time).decision.code, Reason::MIN_HOLD);
  EXPECT_EQ(s.submit(combo, f.time).decision.code, Reason::MIN_HOLD);
  EXPECT_EQ(s.snapshot()->positions.size(), 2);
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
  ASSERT_TRUE(s.trade_stock("SPY", 2, f.time).decision.ok());
  EXPECT_EQ(s.preview_trade_stock("SPY", -3, f.time).decision.code, Reason::MIN_HOLD);
  EXPECT_EQ(s.trade_stock("SPY", -1, f.time).decision.code, Reason::MIN_HOLD);
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
}

TEST(TradeRules, MinimumHoldAllowsBracketsOcoFlattenAndDailyLossLiquidation) {
  ScriptedMarket f;
  AccountRules rules; rules.min_hold_seconds = 3600; rules.min_trades = 10;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto entry = stopped(f.market("protected"), "4.00");
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 1);
  ASSERT_TRUE(s.submit(f.market("oco-entry"), f.time).decision.ok());
  auto oco = f.limit("oco-close", 1, "4.50", Side::Sell);
  oco.oco = {f.limit("sibling", 1, "4.60", Side::Sell)};
  const auto pair = s.submit(oco, f.time);
  ASSERT_TRUE(pair.decision.ok()) << to_string(pair.decision.code) << ": " << pair.decision.message;
  f.next(); f.seed(s, "4.50", "4.70");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 2);
  ASSERT_TRUE(s.submit(f.market("flatten-entry"), f.time).decision.ok());
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
  rules.daily_loss_limit = m("15");
  TradingSession t(config(rules), f.time);
  f.seed(t);
  ASSERT_TRUE(t.submit(f.market("loss", 2), f.time).decision.ok());
  EXPECT_EQ(t.snapshot()->evaluation.day_lock, Reason::DAILY_LOSS_LIMIT);
  EXPECT_TRUE(t.snapshot()->positions.empty());
  EXPECT_EQ(t.snapshot()->evaluation.closed_trades, 1);
}

TEST_P(TradePhases, MicroscalpingUsesExactPositiveNetProfitAndFinalCloseAge) {
  AccountRules rules; rules.phase = GetParam(); rules.microscalp_seconds = 60; rules.microscalp_percent = 50;
  Evaluation e; e.starting_balance = m("10000");
  PlanInputs now{m("10200"), m("10200"), m("200"), true};
  EXPECT_TRUE(evaluation_objectives(e, rules, now).front().met);
  e.short_profit = m("100");
  EXPECT_TRUE(evaluation_objectives(e, rules, now).front().met);
  now.equity = m("10199.999999");
  EXPECT_FALSE(evaluation_objectives(e, rules, now).front().met);
  now.equity = m("10000");
  EXPECT_FALSE(evaluation_objectives(e, rules, now).front().actual);
  rules.profit_basis = ProfitBasis::Balance;
  EXPECT_TRUE(evaluation_objectives(e, rules, now).front().met);
  rules.phase = Phase::Funded;
  EXPECT_TRUE(evaluation_objectives(e, rules, now).empty());
}

TEST_P(TradePhases, MicroscalpProfitSurvivesRecoveryAndExcludesLossesAndBoundary) {
  ScriptedMarket f;
  AccountRules rules; rules.phase = GetParam(); rules.microscalp_seconds = 60; rules.microscalp_percent = 50;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("win"), f.time).decision.ok());
    f.next(); f.seed(s, "5.00", "5.20");
    ASSERT_TRUE(s.submit(f.market("win-close", 1, Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("78.70"));
    ASSERT_TRUE(s.submit(f.market("loss"), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("loss-close", 1, Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("78.70"));
    f.next(); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("longer", 2), f.time).decision.ok());
    const auto opened = f.time;
    f.next(); f.seed(s, "5.00", "5.20");
    ASSERT_TRUE(s.submit(f.market("partial", 1, Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("78.70"));
    f.time = opened + 60 * md::kNanosPerSecond; ++f.observation; f.seed(s, "5.00", "5.20");
    ASSERT_TRUE(s.submit(f.market("boundary", 1, Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("78.70"));
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("78.70"));
  f.next(); f.seed(s);
  EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("78.70"));
}

TEST_P(TradePhases, SettlementClosesWholeTradesDespiteMinimumHold) {
  ScriptedMarket f;
  f.time = f.contract.last_trade_time() - 30 * md::kNanosPerSecond;
  AccountRules rules; rules.phase = GetParam(); rules.min_hold_seconds = 60; rules.min_trades = 1;
  rules.trade_consistency_percent = 100; rules.microscalp_seconds = 60; rules.microscalp_percent = 100;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("expiry"), f.time).decision.ok());
  ASSERT_TRUE(s.settle(f.symbol(), m("5005"), f.contract.expiry_time()).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 1);
  ASSERT_TRUE(s.snapshot()->evaluation.best_trade);
  EXPECT_EQ(s.snapshot()->evaluation.best_trade->pnl, m("79.35"));
  EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("79.35"));
}

TEST_P(TradePhases, MicroscalpingSharesResetsAtSameTimestampAndRecoversBoundary) {
  ScriptedMarket f;
  AccountRules rules; rules.phase = GetParam(); rules.microscalp_seconds = 60; rules.microscalp_percent = 50; rules.min_trades = 2;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
    ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
    s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("510")}});
    ASSERT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("10"));
    EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 0);  // Whole trades group options only.
    ASSERT_TRUE(s.reset_account(m("10000"), rules, "new attempt", f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.short_profit, Money{});
    EXPECT_EQ(s.snapshot()->evaluation.first_stock_fill, 3);
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.snapshot()->evaluation.first_stock_fill, 3);
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
  EXPECT_EQ(s.snapshot()->evaluation.short_profit, Money{});
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("505")}});
  ASSERT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.short_profit, m("5"));
}

TEST(TradeRules, HoldingRulesValidatePairsAndRanges) {
  AccountRules r;
  r.min_hold_seconds = 3601; EXPECT_THROW(validate_rules(r), TradingError);
  r.min_hold_seconds = 0; r.microscalp_percent = 25; EXPECT_THROW(validate_rules(r), TradingError);
  r.microscalp_seconds = 3601; EXPECT_THROW(validate_rules(r), TradingError);
  r.microscalp_seconds = 60; EXPECT_NO_THROW(validate_rules(r));
  r.microscalp_percent = 101; EXPECT_THROW(validate_rules(r), TradingError);
  r.microscalp_percent = 0; EXPECT_NO_THROW(validate_rules(r));
}

TEST_P(TradePhases, CombinedObjectivesHoldOnlyThePassAtExactBoundaries) {
  AccountRules rules; rules.phase = GetParam(); rules.profit_target = m("100"); rules.min_trades = 2;
  rules.trade_consistency_percent = 50; rules.microscalp_seconds = 60; rules.microscalp_percent = 25;
  Evaluation e; e.starting_balance = m("10000"); e.peak = e.starting_balance;
  e.best_trade = BestTrade{"1", m("100")}; e.short_profit = m("50"); e.closed_trades = 1;
  const PlanInputs now{m("10200"), m("10200"), m("200"), true};
  EXPECT_EQ(evaluate_plan(e, rules, now).status, EvaluationStatus::Active);
  e.closed_trades = 2;
  EXPECT_EQ(evaluate_plan(e, rules, now).status, EvaluationStatus::Passed);
  e.short_profit = m("50.000001");
  EXPECT_EQ(evaluate_plan(e, rules, now).status, EvaluationStatus::Active);
  e.short_profit = m("50"); e.best_trade->pnl = m("100.000001");
  EXPECT_EQ(evaluate_plan(e, rules, now).status, EvaluationStatus::Active);
}

TEST_P(TradePhases, RolledWholeTradeClosesOnlyAfterItsNewLeg) {
  ScriptedMarket f, g; g.contract.strike += 5;
  AccountRules rules; rules.phase = GetParam(); rules.min_trades = 1; rules.trade_consistency_percent = 100;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s); g.seed(s);
    ASSERT_TRUE(s.submit(f.market("first"), f.time).decision.ok());
    f.next(); g.next(); f.seed(s, "5.00", "5.20"); g.seed(s);
    auto roll = f.market("roll"); roll.symbol.clear();
    roll.legs = {{f.symbol(), Side::Sell, 1}, {g.symbol(), Side::Buy, 1}};
    ASSERT_TRUE(s.submit(roll, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 0);
    EXPECT_FALSE(s.snapshot()->evaluation.best_trade);
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  g.next(); g.seed(s, "5.00", "5.20");
  ASSERT_TRUE(s.submit(g.market("finish", 1, Side::Sell), g.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.closed_trades, 1);
  ASSERT_TRUE(s.snapshot()->evaluation.best_trade);
  EXPECT_EQ(s.snapshot()->evaluation.best_trade->id, "1");
  EXPECT_EQ(s.snapshot()->evaluation.best_trade->pnl, m("157.40"));
}

TEST_P(TradePhases, TradePercentagesCompareWideProductsWithoutFloatingRounding) {
  AccountRules rules; rules.phase = GetParam(); rules.trade_consistency_percent = 50;
  rules.microscalp_seconds = 60; rules.microscalp_percent = 50;
  Evaluation e;
  e.best_trade = BestTrade{"1", Money::from_micros(4'000'000'000'000'000'000)};
  e.short_profit = e.best_trade->pnl;
  PlanInputs now; now.equity = Money::from_micros(8'000'000'000'000'000'000);
  for (const auto& objective : evaluation_objectives(e, rules, now)) EXPECT_TRUE(objective.met);
  e.best_trade->pnl = e.best_trade->pnl + Money::from_micros(1);
  e.short_profit = e.best_trade->pnl;
  for (const auto& objective : evaluation_objectives(e, rules, now)) EXPECT_FALSE(objective.met);
}

TEST(TradeRules, RequiredStopsProtectEntriesAndCannotBeCancelledWhileHeld) {
  ScriptedMarket f;
  AccountRules rules;
  rules.require_stop_loss = true;
  TradingSession s(config(rules), f.time);
  f.seed(s, "4.00", "4.20", 1);
  EXPECT_EQ(s.preview(f.market("bare"), f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.submit(f.market("bare"), f.time).decision.code, Reason::STOP_REQUIRED);
  auto target = f.market("target-only");
  target.bracket = Bracket{{}, ExitSpec{{}, m("5.00")}};
  EXPECT_EQ(s.submit(target, f.time).decision.code, Reason::STOP_REQUIRED);
  target.client_order_id = "limit-only";
  target.bracket = Bracket{ExitSpec{{}, m("3.00")}, {}};
  EXPECT_EQ(s.submit(target, f.time).decision.code, Reason::STOP_REQUIRED);
  auto wrong = stopped(f.market("wrong-way"));
  wrong.bracket->stop_loss->trigger->direction = TriggerDirection::AtOrAbove;
  EXPECT_EQ(s.submit(wrong, f.time).decision.code, Reason::STOP_REQUIRED);
  const auto entry = s.submit(stopped(f.limit("protected", 2)), f.time);
  ASSERT_TRUE(entry.decision.ok());
  const auto stop = s.snapshot()->recent_orders.at(*entry.order_id - 1).stop_loss;
  ASSERT_NE(stop, 0U);
  EXPECT_EQ(s.cancel(stop, f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.cancel_all({}, f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.cancel_orders({*entry.order_id, stop}, f.time).decision.code, Reason::STOP_REQUIRED);
  // Cancelling the unfilled entry is safe: its filled part keeps its stop.
  EXPECT_TRUE(s.cancel(*entry.order_id, f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("manual-close", 1, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.cancel_all({}, f.time).decision.ok());
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::STOP_REQUIRED);
}

TEST(TradeRules, StopFillsRearmPartialExitsAndFlattenRemainsAvailable) {
  ScriptedMarket f;
  AccountRules rules;
  rules.require_stop_loss = true;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  const auto entry = s.submit(stopped(f.market("entry", 2)), f.time);
  ASSERT_TRUE(entry.decision.ok());
  const auto stop = s.snapshot()->recent_orders.at(*entry.order_id - 1).stop_loss;
  OrderChange shrink; shrink.quantity = 1;
  EXPECT_EQ(s.modify(stop, shrink, f.time).decision.code, Reason::STOP_REQUIRED);
  f.next();
  s.on_quotes({f.quote("2.90", "3.00", 1)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->positions.at(0).position.quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_orders.at(stop - 1).status, OrderStatus::Armed);
  EXPECT_EQ(s.cancel(stop, f.time).decision.code, Reason::STOP_REQUIRED);
  f.next();
  s.on_quotes({f.quote("3.10", "3.20")}, {f.valuation()}, f.time);
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradeRules, StopRiskUsesExactEntryTermsAndExcludesFees) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_trade_risk = m("120");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto request = stopped(f.market("at-cap"));
  const auto preview = s.preview(request, f.time);
  EXPECT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.trade_risk, m("120"));
  EXPECT_EQ(preview.trade_risk_limit, m("120"));
  EXPECT_EQ(preview.trade_risk_basis, "stop_loss");
  EXPECT_EQ(preview.max_units, 1);
  request.quantity = 2;
  const auto refused = s.submit(request, f.time);
  EXPECT_EQ(refused.decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_EQ(refused.decision.actual, 240);
  EXPECT_EQ(refused.decision.limit, 120);
  EXPECT_EQ(refused.decision.scope, "trade");
  // A resting limit uses its limit, not today's ask; a change rechecks the total quantity.
  const auto working = s.submit(stopped(f.limit("resting", 1, "4.00")), f.time);
  ASSERT_TRUE(working.decision.ok());
  OrderChange change; change.quantity = 2;
  EXPECT_EQ(s.preview_change(*working.order_id, change, f.time).trade_risk, m("200"));
  EXPECT_EQ(s.modify(*working.order_id, change, f.time).decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_EQ(s.submit(stopped(f.market("short", 1, Side::Sell), "5.20"), f.time).decision.code, Reason::NONE);
}

TEST(TradeRules, AWalkingOrderIsMeasuredAtItsWalkCap) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_trade_risk = m("120");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  // From 4.00 it can walk to 4.30 and fill there: 1.30 above the 3.00 stop.
  auto walking = stopped(f.limit("walk", 1, "4.00"));
  walking.walk = Walk{m("0.10"), 10, m("4.30")};
  EXPECT_EQ(s.preview(walking, f.time).trade_risk, m("130"));
  EXPECT_EQ(s.submit(walking, f.time).decision.code, Reason::MAX_TRADE_RISK);
  walking.client_order_id = "walk-within";
  walking.walk->limit = m("4.20");
  EXPECT_TRUE(s.submit(walking, f.time).decision.ok());
}

TEST(TradeRules, MissingAndUnderlyingStopsUseBoundedExpiryLossNeverTheScenarioGrid) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_trade_risk = m("420");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto preview = s.preview(f.market("long"), f.time);
  EXPECT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.trade_risk, m("420"));
  EXPECT_EQ(preview.trade_risk_basis, "expiry_payoff");
  preview = s.preview(f.market("short", 1, Side::Sell), f.time);
  EXPECT_EQ(preview.decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_FALSE(preview.trade_risk);
  EXPECT_EQ(preview.trade_risk_basis, "unbounded_or_unknown");
  EXPECT_EQ(s.submit(f.market("short", 1, Side::Sell), f.time).decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_EQ(s.preview(stopped(f.market("spot-stop"), "4900", TriggerSource::Underlying), f.time).trade_risk, m("420"));
}

TEST(TradeRules, ComboRiskUsesSignedNetStopAndExpiryPayoff) {
  ScriptedMarket f, g;
  g.contract.strike += 5;
  AccountRules rules;
  rules.require_stop_loss = true;
  rules.max_trade_risk = m("110");
  TradingSession s(config(rules), f.time);
  f.seed(s, "4.00", "4.20"); g.seed(s, "2.00", "2.20");
  auto combo = f.limit("credit", 1, "-1.80");
  combo.symbol.clear(); combo.legs = {{f.symbol(), Side::Sell, 1}, {g.symbol(), Side::Buy, 1}};
  EXPECT_EQ(s.submit(combo, f.time).decision.code, Reason::STOP_REQUIRED);
  combo = stopped(combo, "3.00", TriggerSource::Combo);
  combo.client_order_id = "credit-stop";
  EXPECT_EQ(s.preview(combo, f.time).trade_risk, m("120"));
  EXPECT_EQ(s.submit(combo, f.time).decision.code, Reason::MAX_TRADE_RISK);
  combo.client_order_id = "credit-safe";
  combo.bracket->stop_loss->trigger->level = m("2.90");
  ASSERT_TRUE(s.submit(combo, f.time).decision.ok());
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
  auto debit = combo;
  debit.client_order_id = "debit"; debit.limit_price = m("2.20");
  for (auto& leg : debit.legs) leg.side = leg.side == Side::Buy ? Side::Sell : Side::Buy;
  debit.bracket->stop_loss->trigger->level = m("-1.00");
  EXPECT_EQ(s.preview(debit, f.time).trade_risk, m("120"));
  rules.require_stop_loss = false; rules.max_trade_risk = m("320");
  TradingSession t(config(rules), f.time);
  f.seed(t); g.seed(t, "2.00", "2.20");
  combo.bracket.reset(); combo.client_order_id = "bounded";
  EXPECT_EQ(t.preview(combo, f.time).trade_risk, m("320"));
  EXPECT_TRUE(t.submit(combo, f.time).decision.ok());
}

TEST(TradeRules, PercentageUsesCurrentPlanFloorRoomAndTheTighterLimit) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_drawdown = m("1000"); rules.drawdown_mode = DrawdownMode::Static;
  rules.max_trade_risk_percent = 10; rules.max_trade_risk = m("120");
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto request = stopped(f.market("entry"));
  EXPECT_EQ(s.preview(request, f.time).trade_risk_limit, m("100"));
  EXPECT_EQ(s.submit(request, f.time).decision.code, Reason::MAX_TRADE_RISK);
  request.client_order_id = "fits"; request.bracket->stop_loss->trigger->level = m("3.20");
  EXPECT_TRUE(s.submit(request, f.time).decision.ok());
  EXPECT_EQ(s.preview(stopped(f.market("another")), f.time).trade_risk_limit, m("98.935"));
  rules.max_drawdown = {}; rules.max_trade_risk = {};
  TradingSession t(config(rules), f.time);
  f.seed(t);
  EXPECT_FALSE(t.preview(request, f.time).trade_risk_limit);
  EXPECT_TRUE(t.submit(f.market("no-floor", 1, Side::Sell), f.time).decision.ok());
}

TEST(TradeRules, StopAndRiskRulesSurviveJournalRecovery) {
  ScriptedMarket f;
  AccountRules rules;
  rules.require_stop_loss = true; rules.max_trade_risk = m("250"); rules.max_trade_risk_percent = 25;
  rules.max_contracts_held = 5;
  JournalFile file;
  OrderId stop = 0;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    const auto result = s.submit(stopped(f.market("entry")), f.time);
    ASSERT_TRUE(result.decision.ok());
    stop = s.snapshot()->recent_orders.at(*result.order_id - 1).stop_loss;
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.cancel(stop, f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.submit(f.market("bare"), f.time).decision.code, Reason::STOP_REQUIRED);
  EXPECT_EQ(s.submit(stopped(f.market("over-risk", 3)), f.time).decision.code, Reason::MAX_TRADE_RISK);
  EXPECT_TRUE(s.close_positions({}, f.time).decision.ok());
}

TEST(TradeRules, RiskRulesValidateRanges) {
  AccountRules rules;
  rules.max_trade_risk = m("-0.000001");
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules.max_trade_risk = {};
  for (const auto percent : {-1, 101}) {
    rules.max_trade_risk_percent = percent;
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
}

TEST(TradeRules, HeldCapCountsAveragingInWorkingOrdersAndQuantityChanges) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_contracts_held = 5;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("first", 2), f.time).decision.ok());
  const auto working = s.submit(f.limit("working", 2, "4.00"), f.time);
  ASSERT_TRUE(working.decision.ok());
  ASSERT_TRUE(s.submit(f.limit("last", 1, "4.00"), f.time).decision.ok());
  const auto preview = s.preview(f.market("too-many"), f.time);
  EXPECT_EQ(preview.decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(preview.decision.actual, 6);
  EXPECT_EQ(preview.decision.limit, 5);
  EXPECT_EQ(preview.decision.scope, "account");
  EXPECT_EQ(preview.max_units, 0);
  EXPECT_EQ(s.submit(f.market("too-many"), f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  OrderChange change;
  change.quantity = 3;
  EXPECT_EQ(s.preview_change(*working.order_id, change, f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(s.modify(*working.order_id, change, f.time).decision.actual, 6);
  ASSERT_TRUE(s.cancel(*working.order_id, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("replacement", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("close", 4, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TradeRules, HeldCapCountsComboRatiosShortsAndReversalsButNotExitsOrShares) {
  ScriptedMarket f, g;
  g.contract.strike += 5;
  AccountRules rules;
  rules.max_contracts_held = 3;
  TradingSession s(config(rules), f.time);
  f.seed(s); g.seed(s);
  auto combo = f.limit("combo", 1, "4.40");
  combo.symbol.clear();
  combo.legs = {{f.symbol(), Side::Buy, 2}, {g.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(combo, f.time).decision.ok());
  EXPECT_EQ(s.submit(f.market("fourth"), f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(s.submit(f.market("reverse", 3, Side::Sell), f.time).decision.actual, 4);
  ASSERT_TRUE(s.submit(g.market("close-short", 1, Side::Buy), f.time).decision.ok());
  auto bracket = f.market("bracket");
  bracket.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3")}, {}}, {}};
  ASSERT_TRUE(s.submit(bracket, f.time).decision.ok());
  EXPECT_EQ(s.submit(f.market("past-exits"), f.time).decision.actual, 4);
  s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("500")}});
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
}

TEST(TradeRules, WorkingOrdersShareClosingCapacityOnce) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_contracts_held = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("close", 2, "4.50", Side::Sell), f.time).decision.ok());
  const auto result = s.submit(f.limit("extra-close", 1, "4.50", Side::Sell), f.time);
  EXPECT_EQ(result.decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_EQ(result.decision.actual, 3);
}

TEST(TradeRules, ContractCapSurvivesJournalRecovery) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_contracts_held = 3;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.limit("reserved", 1, "4.00"), f.time).decision.ok());
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.submit(f.market("after-restart"), f.time).decision.code, Reason::MAX_CONTRACTS_HELD);
  EXPECT_TRUE(s.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
}

TEST(TradeRules, DisabledRulesAreAbsentFromJournalBytes) {
  ScriptedMarket f;
  JournalFile file;
  {
    TradingSession s(config(), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("entry", 2), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
  }
  const auto recovery = FileJournal::read(file.path);
  for (const auto& record : recovery.records)
    for (const auto* field : {"max_volume_percent", "no_hedging", "no_counter_positions", "min_hold_seconds", "microscalp_seconds", "microscalp_percent", "short_profit", "first_stock_fill", "min_trades", "closed_trades", "trade_consistency_percent", "best_trade", "max_contracts_held", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent"})
      EXPECT_EQ(record.payload.find(field), std::string::npos);
  auto s = TradingSession::recover(recovery);
  EXPECT_EQ(s.config().rules.max_contracts_held, 0);
  // An older journal's literal rules re-encode byte for byte, without new defaults.
  const auto legacy = FileJournal::read((std::filesystem::path(OPENPORT_TEST_DATA_DIR) / "kill-before-reduce-only.jsonl").string());
  for (const auto& record : legacy.records) {
    const auto j = nlohmann::json::parse(record.payload);
    if (!j.contains("state")) continue;
    const auto& old_rules = j.at("state").at("config").at("rules");
    const nlohmann::json encoded = old_rules.get<AccountRules>();
    EXPECT_EQ(encoded.dump(), old_rules.dump());
  }
  JournalFile repeated;
  auto defaults = config();
  defaults.rules.max_volume_percent = 0;
  defaults.rules.no_hedging = false;
  defaults.rules.no_counter_positions = false;
  defaults.rules.max_contracts_held = 0;
  defaults.rules.require_stop_loss = false;
  defaults.rules.max_trade_risk = {};
  defaults.rules.max_trade_risk_percent = 0;
  {
    TradingSession t(defaults, f.time, FileJournal::create(repeated.path));
    f.seed(t);
    ASSERT_TRUE(t.submit(f.market("entry", 2), f.time).decision.ok());
    ASSERT_TRUE(t.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
  }
  const auto again = FileJournal::read(repeated.path);
  ASSERT_EQ(again.records.size(), recovery.records.size());
  for (std::size_t i = 0; i < recovery.records.size(); ++i) {
    EXPECT_EQ(again.records[i].payload, recovery.records[i].payload);
    EXPECT_EQ(again.records[i].hash, recovery.records[i].hash);
  }
}

TEST(TradeRules, ServerVolumeRuleValidatesAndRecoversWithoutReducerVolumeInputs) {
  AccountRules rules; rules.max_volume_percent = 25;
  ScriptedMarket f;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("server-accepted"), f.time).decision.ok());
  }
  auto recovered = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(recovered.config().rules.max_volume_percent, 25);
  EXPECT_EQ(recovered.snapshot()->positions.size(), 1U);
  for (const auto percent : {-1, 101}) {
    rules.max_volume_percent = percent;
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
}

TEST(TradeRules, ContractCapValidatesItsRange) {
  AccountRules rules;
  for (const Quantity cap : {-1, 100001}) {
    rules.max_contracts_held = cap;
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
  rules.max_contracts_held = 100000;
  EXPECT_NO_THROW(validate_rules(rules));
}

TEST_P(TradePhases, HedgingChecksNetDeltaAndAllowsReductionsAndSystemExitsAcrossRecovery) {
  ScriptedMarket call, put;
  put.contract.type = pricing::OptionType::Put;
  AccountRules rules; rules.phase = GetParam(); rules.no_hedging = true;
  JournalFile file;
  TradingSession s(config(rules), call.time, FileJournal::create(file.path));
  call.seed(s); put.seed(s);
  s.on_quotes({}, {call.valuation(0.059152861928), put.valuation(-0.060631543814)}, put.time);
  ASSERT_TRUE(s.submit(call.market("long"), call.time).decision.ok());
  const auto refused = s.submit(put.market("hedge"), put.time);
  EXPECT_EQ(refused.decision.code, Reason::HEDGING);
  EXPECT_EQ(refused.decision.message, "Opening SPX dollar delta -$30,316 opposes held dollar delta +$29,576");
  ASSERT_TRUE(refused.decision.evidence);
  EXPECT_EQ(refused.decision.evidence->underlying, "SPX");
  EXPECT_DOUBLE_EQ(refused.decision.evidence->order_dollar_delta, -30315.771907);
  EXPECT_DOUBLE_EQ(refused.decision.evidence->held_dollar_delta, 29576.430964);
  EXPECT_EQ(refused.decision.actual, refused.decision.evidence->order_dollar_delta);
  EXPECT_EQ(refused.decision.limit, refused.decision.evidence->held_dollar_delta);
  EXPECT_EQ(s.preview(put.market("preview"), put.time).decision.code, Reason::HEDGING);
  auto recovered = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
  EXPECT_EQ(recovered.submit(put.market("after-recovery"), put.time).decision.code, Reason::HEDGING);
  // A reversal opens and is refused, while closing the existing call is allowed.
  EXPECT_EQ(s.submit(call.market("reverse", 2, Side::Sell), call.time).decision.code, Reason::HEDGING);
  ASSERT_TRUE(s.submit(call.market("reduce", 1, Side::Sell), call.time).decision.ok());
  ASSERT_TRUE(s.submit(put.market("new-direction"), put.time).decision.ok());
  ASSERT_TRUE(s.close_positions({}, put.time).decision.ok());
  // A delta-neutral combo has no opposite direction.
  s.on_quotes({}, {call.valuation(), put.valuation(-0.5)}, call.time);
  ASSERT_TRUE(s.submit(call.market("held-for-neutral"), call.time).decision.ok());
  auto neutral = call.market("neutral"); neutral.symbol.clear();
  neutral.legs = {{call.symbol(), Side::Buy, 1}, {put.symbol(), Side::Buy, 1}};
  ASSERT_TRUE(s.submit(neutral, call.time).decision.ok());
}

TEST_P(TradePhases, HedgingIncludesSharesAndRejectsUnknownDelta) {
  ScriptedMarket f;
  f.contract = *md::parse_osi("SPY261022P00500000");
  AccountRules rules; rules.phase = GetParam(); rules.no_hedging = true;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  s.on_quotes({}, {f.valuation(-0.5)}, f.time, {{"SPY", f.time, m("500")}});
  ASSERT_TRUE(s.submit(f.market("put"), f.time).decision.ok());
  EXPECT_EQ(s.preview_trade_stock("SPY", 1, f.time).decision.code, Reason::HEDGING);
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::HEDGING);
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
  EXPECT_EQ(s.submit(f.market("hedges-shares"), f.time).decision.code, Reason::HEDGING);
  EXPECT_EQ(s.trade_stock("SPY", -2, f.time).decision.code, Reason::HEDGING);
  EXPECT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  auto missing = f.valuation(); missing.valid = false;
  s.on_quotes({}, {missing}, f.time);
  EXPECT_EQ(s.submit(f.market("unknown"), f.time).decision.code, Reason::MISSING_VALUATION);
}
}  // namespace
TEST(TradeRules, OpeningFillInputsRecheckEachPartialFillAndRecoverDeterministically) {
  std::string expected;
  for (int repeat = 0; repeat < 2; ++repeat) {
    JournalFile file;
    ScriptedMarket f;
    AccountRules rules; rules.max_volume_percent = 10; rules.no_counter_positions = true;
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s);
    OpeningRuleInputs inputs; inputs.volumes[f.symbol()] = 20;
    s.set_opening_rule_inputs(inputs);
    const auto resting = s.submit(f.limit("resting", 2, "4.00"), f.time);
    ASSERT_TRUE(resting.decision.ok());
    f.next();
    s.on_quotes({f.quote("3.80", "4.00", 1)}, {f.valuation()}, f.time);
    ASSERT_EQ(s.snapshot()->recent_fills.size(), 1U);
    EXPECT_EQ(s.snapshot()->recent_fills.front().quantity, 1);
    EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::PartiallyFilled);
    inputs.volumes[f.symbol()] = 10;
    s.set_opening_rule_inputs(inputs);
    f.next();
    s.on_quotes({f.quote("3.80", "4.00")}, {f.valuation()}, f.time);
    const auto after = s.snapshot();
    ASSERT_EQ(after->recent_orders.back().reason.code, Reason::MAX_VOLUME_SHARE);
    EXPECT_EQ(after->recent_orders.back().status, OrderStatus::Cancelled);
    EXPECT_EQ(after->recent_orders.back().filled_quantity, 1);
    EXPECT_EQ(after->recent_orders.back().reason.actual, 2);
    EXPECT_EQ(after->recent_orders.back().reason.limit, 1);
    EXPECT_EQ(after->recent_fills.size(), 1U);
    const auto recovery = FileJournal::read(file.path);
    EXPECT_EQ(TradingSession::recover(recovery).snapshot_json(), s.snapshot_json());
    bool recorded = false;
    for (const auto& record : recovery.records)
      if (record.payload.find("opening_rule_inputs") != std::string::npos) recorded = true;
    EXPECT_TRUE(recorded);
    if (repeat == 0) expected = recovery.head;
    else { EXPECT_EQ(recovery.head, expected); }
    // Neither unknown volume nor an opposite external direction may block a reduction.
    inputs.volumes.clear(); inputs.counter_positions = {{"other", "SPX", 100}};
    s.set_opening_rule_inputs(inputs);
    ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradeRules, VolumeFillsSeeOtherOrdersFilledEarlierInTheSameBatch) {
  ScriptedMarket f;
  AccountRules rules; rules.max_volume_percent = 10;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  OpeningRuleInputs inputs; inputs.volumes[f.symbol()] = 10;
  s.set_opening_rule_inputs(inputs);
  ASSERT_TRUE(s.submit(f.limit("first", 1, "4.00"), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.limit("second", 1, "4.00"), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("3.80", "4.00")}, {f.valuation()}, f.time);
  const auto snapshot = s.snapshot();
  ASSERT_EQ(snapshot->recent_orders.size(), 2U);
  EXPECT_EQ(snapshot->recent_orders.front().status, OrderStatus::Filled);
  EXPECT_EQ(snapshot->recent_orders.back().reason.code, Reason::MAX_VOLUME_SHARE);
  EXPECT_EQ(snapshot->recent_orders.back().reason.actual, 2);
  EXPECT_EQ(snapshot->recent_fills.size(), 1U);
}

TEST(TradeRules, OpeningFillJournalContainsOnlyTheAttemptedLegsAndUnderlyings) {
  for (const bool compact : {false, true}) {
    JournalFile file;
    ScriptedMarket f, other;
    other.contract.strike += 5;
    AccountRules rules; rules.max_volume_percent = 10; rules.no_counter_positions = true;
    TradingSession s(config(rules), f.time, FileJournal::create(file.path));
    f.seed(s); other.seed(s);
    OpeningRuleInputs inputs;
    inputs.volumes = {{f.symbol(), 100}, {other.symbol(), std::nullopt}, {"unused", 9000}};
    inputs.counter_positions = {{"unrelated", "QQQ", std::nullopt}, {"first", "SPX", 100},
                               {"second", "SPY", 100}, {"third", "SPX", 200}};
    s.set_opening_rule_inputs(inputs, compact);
    auto request = f.market("combo");
    request.symbol.clear(); request.legs = {{f.symbol(), Side::Buy, 1}, {other.symbol(), Side::Buy, 1}};
    ASSERT_TRUE(s.submit(request, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::MAX_VOLUME_SHARE);
    const auto recovery = FileJournal::read(file.path);
    EXPECT_EQ(TradingSession::recover(recovery).snapshot_json(), s.snapshot_json());
    std::size_t count = 0;
    for (const auto& record : recovery.records) {
      const auto payload = Json::parse(record.payload);
      for (const auto& event : payload.at("events")) {
        if (event.at("type") != "opening_rule_inputs") continue;
        ++count;
        const auto& recorded = event.at("payload").at("inputs");
        auto expected = Json(inputs);
        if (compact) {
          expected["volumes"].erase("unused");
          expected["counter_positions"].erase(2);
          expected["counter_positions"].erase(0);
        }
        EXPECT_EQ(recorded, expected);
      }
    }
    EXPECT_EQ(count, 1U);
  }
}

TEST(TradeRules, ComboFillChecksEveryRatioAndKeepsRuleCancellationCode) {
  ScriptedMarket f, other;
  other.contract.strike += 5;
  AccountRules rules; rules.max_volume_percent = 10;
  TradingSession s(config(rules), f.time);
  f.seed(s); other.seed(s);
  auto request = f.limit("combo", 1, "12.00");
  request.symbol.clear(); request.legs = {{f.symbol(), Side::Buy, 1}, {other.symbol(), Side::Buy, 2}};
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  OpeningRuleInputs inputs; inputs.volumes = {{f.symbol(), 100}, {other.symbol(), 10}};
  s.set_opening_rule_inputs(inputs);
  f.next(); other.next();
  s.on_quotes({f.quote("3.80", "4.00"), other.quote("3.80", "4.00")}, {f.valuation(), other.valuation()}, f.time);
  const auto snapshot = s.snapshot();
  EXPECT_TRUE(snapshot->recent_fills.empty());
  ASSERT_EQ(snapshot->recent_orders.back().reason.code, Reason::MAX_VOLUME_SHARE);
  ASSERT_TRUE(snapshot->recent_orders.back().reason.evidence);
  EXPECT_EQ(snapshot->recent_orders.back().reason.evidence->contract, other.symbol());
  EXPECT_EQ(snapshot->recent_orders.back().reason.actual, 2);
}

TEST(TradeRules, CounterExposureQueryDropsExpiredWorkingOrdersWithoutChangingTheAccount) {
  ScriptedMarket f;
  TradingSession s(config(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("working", 1, "4.00"), f.time).decision.ok());
  const auto before = s.snapshot_json();
  EXPECT_EQ(s.counter_exposures("other", f.time).size(), 1U);
  f.time = s.snapshot()->open_orders.front().day_end;
  EXPECT_TRUE(s.counter_exposures("other", f.time, {f.valuation()}).empty());
  EXPECT_EQ(s.snapshot_json(), before);
}

TEST(TradeRules, ChainedOpeningFillsUseTheCurrentExternalDirection) {
  ScriptedMarket f;
  AccountRules rules; rules.no_counter_positions = true;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  auto request = f.limit("parent", 1, "4.00");
  request.then = {f.market("", 2, Side::Sell)};
  ASSERT_TRUE(s.submit(request, f.time).decision.ok());
  OpeningRuleInputs inputs; inputs.counter_positions = {{"other", "SPX", 100}};
  s.set_opening_rule_inputs(inputs);
  f.next();
  s.on_quotes({f.quote("3.80", "4.00")}, {f.valuation()}, f.time);
  const auto snapshot = s.snapshot();
  ASSERT_EQ(snapshot->recent_orders.size(), 2U);
  EXPECT_EQ(snapshot->recent_orders.front().status, OrderStatus::Filled);
  EXPECT_EQ(snapshot->recent_orders.back().status, OrderStatus::Cancelled);
  EXPECT_EQ(snapshot->recent_orders.back().reason.code, Reason::COUNTER_POSITION);
  ASSERT_EQ(snapshot->positions.size(), 1U);
  EXPECT_EQ(snapshot->positions.front().position.quantity, 1);
}

}  // namespace openport::trading
