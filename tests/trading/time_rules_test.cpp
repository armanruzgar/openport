#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "openport/md/time.hpp"
#include "support/scripted_market.hpp"
#include "support/journal.hpp"

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
AccountRules plan() {
  AccountRules r;
  r.plan = "Time rules test";
  return r;
}
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-time-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};
TEST(TimeRules, LegacyPlanJournalBytes) {
  ScriptedMarket f;
  auto rules = plan();
  rules.profit_target = m("100");
  rules.max_drawdown = m("1000");
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, test::create_journal(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    f.next();
    s.on_quotes({f.quote("4.40", "4.60")}, {f.valuation()}, f.time);
    EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  }
  const auto recovery = FileJournal::read(file.path);
  // Recorded by the pre-time-rules reducer at 85e64e20 with this scripted market.
  EXPECT_EQ(recovery.head, "4e40378560db4e891c76cb27a686dfd2baf9c5a98dbae7ab4590dbd30d57acb0");
  // Default-off fields are absent in every checkpoint, delta and event. The
  // calendar's "events" name also names the legacy transaction envelope, so
  // its omission from rules is covered by the unchanged hash above.
  for (const auto& record : recovery.records)
    for (const auto* key : {"max_contracts_held", "require_stop_loss", "max_trade_risk", "max_trade_risk_percent", "time_limit_days", "inactivity_days", "underlyings", "trading_start", "trading_end", "last_activity", "flat_time", "no_overnight", "flat_time_day", "flat_pending", "news_before_minutes", "news_after_minutes", "news_action",
                            "hold_restrictions", "hold_cutoff", "hold_calendar", "event_actions", "holding_violations", "event_checked",
                            "scaling", "size_scaling", "scaling_limit"})
      EXPECT_EQ(record.payload.find(std::string("\"") + key + "\""), std::string::npos) << key;
  // Explicitly disabling each field takes exactly the legacy command path.
  rules.max_contracts_held = 0; rules.require_stop_loss = false; rules.max_trade_risk = {}; rules.max_trade_risk_percent = 0;
  rules.time_limit_days = 0; rules.inactivity_days = 0; rules.underlyings.clear();
  rules.trading_start.reset(); rules.trading_end.reset(); rules.flat_time.reset(); rules.no_overnight = false;
  rules.events.clear(); rules.news_before_minutes = 0; rules.news_after_minutes = 0; rules.news_action = "block";
  rules.hold_restrictions.clear(); rules.hold_cutoff = 15 * 60 + 45; rules.hold_calendar.reset();
  rules.scaling.clear(); rules.size_scaling.reset();
  ScriptedMarket g;
  JournalFile again;
  {
    TradingSession s(config(rules), g.time, test::create_journal(again.path));
    g.seed(s);
    ASSERT_TRUE(s.submit(g.market("open", 5), g.time).decision.ok());
    g.next(); s.on_quotes({g.quote("4.40", "4.60")}, {g.valuation()}, g.time);
  }
  const auto repeated = FileJournal::read(again.path);
  ASSERT_EQ(recovery.records.size(), repeated.records.size());
  for (std::size_t i = 0; i < recovery.records.size(); ++i) EXPECT_EQ(recovery.records[i].payload, repeated.records[i].payload);
  EXPECT_EQ(recovery.head, repeated.head);
  EXPECT_EQ(TradingSession::recover(recovery).snapshot_json(), TradingSession::recover(repeated).snapshot_json());
}

void next_day(TradingSession& s, ScriptedMarket& f, md::Date day) {
  f.time = md::new_york_to_utc(day, 10, 0);
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
}
// Both evaluation steps enforce the same optional plan restrictions.
class EvaluationTimeRules : public ::testing::TestWithParam<Phase> {
 protected:
  static AccountRules plan() {
    AccountRules rules;
    rules.plan = "Time rules test";
    rules.phase = GetParam();
    return rules;
  }
};
INSTANTIATE_TEST_SUITE_P(ProgramSteps, EvaluationTimeRules,
    ::testing::Values(Phase::Evaluation, Phase::Verification),
    [](const ::testing::TestParamInfo<Phase>& param) {
      return param.param == Phase::Verification ? "Verification" : "Evaluation";
    });

TEST_P(EvaluationTimeRules, TimeLimitFailsAfterTheDeadlineOnAnEmptyBatch) {
  ScriptedMarket f;
  auto rules = plan();
  rules.time_limit_days = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  next_day(s, f, {2026, 9, 24});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  const auto progress = time_rule_progress(s.snapshot()->evaluation, rules, f.time);
  EXPECT_EQ(progress.deadline, (md::Date{2026, 9, 24}));
  EXPECT_EQ(progress.days_left, 0);
  // 17:00 belongs to Friday under this plan; even a flat empty batch decides.
  f.time = md::new_york_to_utc({2026, 9, 24}, 17, 0);
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::TIME_LIMIT);
  EXPECT_EQ(s.snapshot()->evaluation.decided_at, f.time);
  EXPECT_EQ(s.snapshot()->evaluation.decision, "The evaluation's 2-day window ended on 2026-09-24");
  EXPECT_EQ(s.submit(f.market("late"), f.time).decision.code, Reason::EVALUATION_CLOSED);
}
TEST_P(EvaluationTimeRules, PassBeforeDeadlineStandsAndZeroStartWaitsForMarketTime) {
  ScriptedMarket f;
  auto rules = plan(); rules.time_limit_days = 1; rules.profit_target = m("100");
  TradingSession s(config(rules), 0);
  EXPECT_FALSE(time_rule_progress(s.snapshot()->evaluation, rules, 0).deadline);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote("4.40", "4.60")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::PROFIT_TARGET);
}
TEST_P(EvaluationTimeRules, OwnExecutionsResetInactivityButOrdersDoNot) {
  ScriptedMarket f;
  auto rules = plan(); rules.inactivity_days = 2;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  EXPECT_EQ(time_rule_progress(s.snapshot()->evaluation, rules, f.time).last_activity, f.time);
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.submit(f.market("own"), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, f.time);
  const auto activity = f.time;
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  ASSERT_TRUE(s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, activity);
  // A late execution cannot revive it; the time check runs before matching.
  f.time = md::new_york_to_utc({2026, 9, 28}, 10, 0);
  EXPECT_EQ(s.submit(f.market("too-late"), f.time).decision.code, Reason::EVALUATION_CLOSED);
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::INACTIVITY);
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, activity);
}
TEST(TimeRules, SystemLiquidationIsNotActivityAndFundedAccountsCanExpire) {
  ScriptedMarket f;
  auto rules = plan(); rules.inactivity_days = 2; rules.daily_loss_limit = m("100");
  rules.phase = Phase::Funded; rules.payouts.qualifying_days = 1;
  TradingSession s(config(rules), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("own", 5), f.time).decision.ok());
  const auto activity = f.time;
  next_day(s, f, {2026, 9, 23});
  f.next(); s.on_quotes({f.quote("3.50", "3.70")}, {f.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->evaluation.day_lock, Reason::DAILY_LOSS_LIMIT);
  ASSERT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, activity);
  next_day(s, f, {2026, 9, 24});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::INACTIVITY);
}
TEST_P(EvaluationTimeRules, ShareExecutionsCountAsOwnActivity) {
  ScriptedMarket f;
  auto rules = plan(); rules.inactivity_days = 2;
  TradingSession s(config(rules), f.time); f.seed(s);
  next_day(s, f, {2026, 9, 23});
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, f.time);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
}
TEST_P(EvaluationTimeRules, WhitelistUsesUnderlyingForOptionsAndShares) {
  ScriptedMarket f;
  auto rules = plan(); rules.underlyings = {"SPX", "SPY"};
  TradingSession s(config(rules), f.time); f.seed(s);
  // SPXW is admitted under SPX, not its option root.
  ASSERT_TRUE(s.submit(f.market("spxw"), f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  const auto shares = s.trade_stock("QQQ", 1, f.time, StockPrice{"QQQ", f.time, m("450")}).decision;
  EXPECT_EQ(shares.code, Reason::INSTRUMENT_NOT_ALLOWED); EXPECT_EQ(shares.scope, "QQQ");
  ScriptedMarket g; g.contract = *md::parse_osi("XSP261022C00500000"); g.seed(s);
  const auto denied = s.submit(g.market("xsp"), f.time).decision;
  EXPECT_EQ(denied.code, Reason::INSTRUMENT_NOT_ALLOWED); EXPECT_EQ(denied.scope, "XSP");
}
TEST_P(EvaluationTimeRules, TradingHoursCancelOpeningsAtEndButKeepReducingOrdersAndExits) {
  ScriptedMarket f;
  auto rules = plan(); rules.trading_start = 10 * 60; rules.trading_end = 11 * 60;
  TradingSession s(config(rules), f.time); f.seed(s);
  auto entry = f.market("position", 2);
  entry.bracket = Bracket{{}, ExitSpec{{}, m("8")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  const auto opening = s.submit(f.limit("opening", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  const auto reducing = s.submit(f.limit("reducing", 1, "4.80", Side::Sell, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(opening.decision.ok()); ASSERT_TRUE(reducing.decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 10, 59);
  s.on_quotes({}, {}, f.time);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(*opening.order_id - 1).open());
  f.time += md::kNanosPerMinute;
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.code, Reason::OUTSIDE_PLAN_HOURS);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.actual, 660); EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.limit, 660);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(*reducing.order_id - 1).open());
  EXPECT_TRUE(s.snapshot()->recent_orders.at(1).open()); // managed take-profit
  ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_EQ(s.submit(f.market("outside"), f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
  ASSERT_TRUE(s.cancel(*reducing.order_id, f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("close", 2, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}
TEST(TimeRules, HoursApplyBeforeStartInCurbAndAfterDaylightSaving) {
  for (const md::Date date : {md::Date{2026, 9, 22}, md::Date{2026, 11, 3}}) {
    ScriptedMarket f; f.contract = *md::parse_osi("SPXW261119C05000000");
    f.time = md::new_york_to_utc(date, 9, 30);
    auto rules = plan(); rules.trading_start = 9 * 60 + 31; rules.trading_end = 16 * 60;
    TradingSession s(config(rules), f.time); f.seed(s);
    const auto early = s.submit(f.market("early"), f.time).decision;
    EXPECT_EQ(early.code, Reason::OUTSIDE_PLAN_HOURS); EXPECT_EQ(early.actual, 570); EXPECT_EQ(early.limit, 571);
    f.time += md::kNanosPerMinute; ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    EXPECT_TRUE(s.submit(f.market("at-start"), f.time).decision.ok());
    f.time = md::new_york_to_utc(date, 16, 30);
    auto curb = f.limit("curb"); curb.tif = TimeInForce::GtcExto;
    EXPECT_EQ(s.submit(curb, f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
    f.time = md::new_york_to_utc(date, 20, 30);
    curb.client_order_id = "global";
    EXPECT_EQ(s.submit(curb, f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
  }
}
TEST_P(EvaluationTimeRules, RulesAndActivityRecoverAndTheRecoveredDeadlineStillFails) {
  ScriptedMarket f;
  auto rules = plan(); rules.time_limit_days = 10; rules.inactivity_days = 2;
  rules.underlyings = {"SPX"}; rules.trading_start = 9 * 60 + 30; rules.trading_end = 16 * 60;
  rules.max_contracts_held = 5; rules.require_stop_loss = true;
  rules.max_drawdown = m("1000"); rules.max_trade_risk = m("123.456789"); rules.max_trade_risk_percent = 25;
  JournalFile file;
  std::string snapshot;
  {
    TradingSession s(config(rules), f.time, test::create_journal(file.path)); f.seed(s);
    auto entry = f.market("own");
    entry.bracket = Bracket{ExitSpec{Trigger{TriggerSource::Option, TriggerDirection::AtOrBelow, m("3.50")}, {}}, {}};
    ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
    snapshot = s.snapshot_json();
  }
  const auto recovery = FileJournal::read(file.path);
  auto s = TradingSession::recover(recovery, test::resume_journal(file.path));
  EXPECT_EQ(s.config().rules, rules); EXPECT_EQ(s.snapshot_json(), snapshot);
  EXPECT_EQ(s.snapshot()->evaluation.last_activity, f.time);
  next_day(s, f, {2026, 9, 25});
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::INACTIVITY);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}
TEST(TimeRules, ValidationAndCalendarDaysFollowThePlanBoundary) {
  for (const auto n : {-1, 367}) {
    auto rules = plan(); rules.time_limit_days = n; EXPECT_THROW(validate_rules(rules), TradingError);
    rules.time_limit_days = 0; rules.inactivity_days = n; EXPECT_THROW(validate_rules(rules), TradingError);
  }
  auto rules = plan(); rules.phase = Phase::Funded; rules.payouts.qualifying_days = 1; rules.time_limit_days = 1;
  EXPECT_THROW(validate_rules(rules), TradingError);
  rules = plan(); rules.trading_start = 0; EXPECT_THROW(validate_rules(rules), TradingError);
  rules.trading_end = 1440; EXPECT_NO_THROW(validate_rules(rules));
  rules.trading_start = 1440; EXPECT_THROW(validate_rules(rules), TradingError);
  for (const auto& symbols : {std::vector<std::string>{"SPX", "SPX"}, std::vector<std::string>{"spx"}, std::vector<std::string>{""}, std::vector<std::string>(33, "SPX")}) {
    rules = plan(); rules.underlyings = symbols; EXPECT_THROW(validate_rules(rules), TradingError);
  }
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 10, 30}, 17, 30);
  rules = plan(); rules.time_limit_days = 3; rules.day_end_minutes = 18 * 60;
  TradingSession s(config(rules), f.time);
  auto p = time_rule_progress(s.snapshot()->evaluation, rules, md::new_york_to_utc({2026, 11, 2}, 10, 0));
  EXPECT_EQ(p.deadline, (md::Date{2026, 11, 2})); EXPECT_EQ(p.days_left, 0);
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 11, 2}, 17, 59));
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 11, 2}, 18, 0));
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::TIME_LIMIT);
}

TEST(TimeRules, RecoveredPositionsOutsideWhitelistCanStillBeReduced) {
  ScriptedMarket f;
  JournalFile source, restricted;
  {
    TradingSession s(config(plan()), f.time, test::create_journal(source.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
    ASSERT_TRUE(s.trade_stock("SPY", 2, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  }
  // Import held positions under a restricted plan. Recovery preserves outcomes;
  // its next own orders must still be able to reduce every inherited holding.
  {
    const auto journal = test::create_journal(restricted.path);
    for (const auto& record : FileJournal::read(source.path).records) {
      auto payload = nlohmann::json::parse(record.payload);
      if (payload.contains("state")) payload["state"]["config"]["rules"]["underlyings"] = {"XSP"};
      journal->append(record.time, record.type, payload.dump());
    }
  }
  auto s = TradingSession::recover(FileJournal::read(restricted.path));
  EXPECT_EQ(s.submit(f.market("add"), f.time).decision.code, Reason::INSTRUMENT_NOT_ALLOWED);
  EXPECT_TRUE(s.submit(f.market("reduce", 1, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::INSTRUMENT_NOT_ALLOWED);
  EXPECT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  EXPECT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty()); EXPECT_TRUE(s.snapshot()->stocks.empty());
}

TEST(TimeRules, AGapOverTheEndCancelsBeforeTheNextWindowsFirstFill) {
  ScriptedMarket f;
  auto rules = plan(); rules.trading_start = 570; rules.trading_end = 960;
  TradingSession s(config(rules), f.time); f.seed(s);
  const auto opening = s.submit(f.limit("overnight", 1, "4.00", Side::Buy, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(opening.decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0); ++f.observation;
  s.on_quotes({f.quote("3.80", "4.00")}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.code, Reason::OUTSIDE_PLAN_HOURS);
  EXPECT_TRUE(s.snapshot()->positions.empty());
}

TEST(TimeRules, ComboEntriesRespectHoursAndClosingLegsRemainAllowed) {
  ScriptedMarket f, g; g.contract = *md::parse_osi("SPXW261022C05010000");
  auto rules = plan(); rules.underlyings = {"SPX"}; rules.trading_start = 570; rules.trading_end = 660;
  TradingSession s(config(rules), f.time); f.seed(s); g.seed(s);
  auto entry = f.market("spread"); entry.symbol.clear();
  entry.legs = {{f.symbol(), Side::Buy, 1}, {g.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  f.time = g.time = md::new_york_to_utc({2026, 9, 22}, 11, 0);
  ++f.observation; ++g.observation;
  s.on_quotes({f.quote(), g.quote()}, {f.valuation(), g.valuation()}, f.time);
  entry.client_order_id = "late-spread";
  EXPECT_EQ(s.submit(entry, f.time).decision.code, Reason::OUTSIDE_PLAN_HOURS);
  entry.client_order_id = "close-spread";
  entry.legs[0].side = Side::Sell; entry.legs[1].side = Side::Buy;
  EXPECT_TRUE(s.submit(entry, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->positions.empty());
}
TEST(TimeRules, ProjectedOddsRespectCalendarDeadlines) {
  std::vector<EvaluationDay> history;
  for (int i = 0; i < 12; ++i) {
    EvaluationDay day;
    day.day = md::date_from_days(md::days_since_epoch({2026, 8, 1}) + i);
    day.open_equity = m("10000"); day.close_equity = m("10060");
    day.low_equity = m("9990"); day.high_equity = m("10070");
    day.low_at = 1; day.high_at = 2;
    history.push_back(day);
  }
  const auto project = [&](std::int64_t limit, std::int64_t inactivity, md::Date date) {
    ScriptedMarket f; f.time = md::new_york_to_utc(date, 10, 0);
    auto rules = plan(); rules.profit_target = m("100");
    rules.time_limit_days = limit; rules.inactivity_days = inactivity;
    TradingSession s(config(rules), f.time); f.seed(s);
    return pass_odds(s.snapshot()->evaluation, rules, s.snapshot()->equity, history, 10, 100, 7);
  };
  // The sampled path needs two days to pass; the deadline day still counts.
  EXPECT_EQ(project(1, 0, {2026, 9, 22}).fail, 1.0);
  EXPECT_EQ(project(2, 0, {2026, 9, 22}).pass, 1.0);
  // The next trading date after Friday is Monday, three calendar days later.
  EXPECT_EQ(project(0, 1, {2026, 9, 25}).fail, 1.0);
  EXPECT_EQ(project(0, 3, {2026, 9, 25}).pass, 1.0);
}
TEST_P(EvaluationTimeRules, FlatTimeCancelsOpeningsAndClosesOptionsAndSharesBeforeMatching) {
  ScriptedMarket f;
  auto rules = plan(); rules.flat_time = 601;
  auto c = config(rules); c.limits.max_quote_age = 30 * md::kNanosPerSecond;
  TradingSession s(c, f.time); f.seed(s);
  auto entry = f.market("held", 2);
  entry.bracket = Bracket{{}, ExitSpec{{}, m("8")}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 3, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  const auto opening = s.submit(f.limit("opening", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  const auto reducing = s.submit(f.limit("reducing", 1, "4.80", Side::Sell, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(opening.decision.ok()); ASSERT_TRUE(reducing.decision.ok());
  f.time += md::kNanosPerMinute; ++f.observation;
  // Empty batches trigger the rule, with no fabricated execution on stale data.
  s.on_quotes({}, {}, f.time);
  auto snap = s.snapshot();
  EXPECT_EQ(snap->recent_orders.at(*opening.order_id - 1).reason.code, Reason::FLAT_TIME);
  EXPECT_TRUE(snap->recent_orders.at(*reducing.order_id - 1).open());
  EXPECT_TRUE(snap->recent_orders.at(1).open());
  EXPECT_TRUE(snap->evaluation.flat_pending);
  const auto denied = s.submit(f.market("late"), f.time).decision;
  EXPECT_EQ(denied.code, Reason::FLAT_TIME); EXPECT_EQ(denied.actual, 601);
  EXPECT_EQ(denied.limit, 601); EXPECT_EQ(denied.scope, "account");
  EXPECT_EQ(s.preview(f.market("preview"), f.time).decision.code, Reason::FLAT_TIME);
  EXPECT_EQ(s.preview_trade_stock("SPY", 1, f.time).decision.code, Reason::FLAT_TIME);
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::FLAT_TIME);
  // Reductions remain allowed when mandatory closes are waiting for fresh data.
  EXPECT_EQ(s.submit(f.limit("reduce-more", 1, "4.90", Side::Sell, TimeInForce::Gtc), f.time).decision.code, Reason::STALE_QUOTE);
  EXPECT_EQ(s.preview(f.market("reduce-preview", 1, Side::Sell), f.time).decision.code, Reason::STALE_QUOTE);
  EXPECT_TRUE(s.trade_stock("SPY", -1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  s.on_quotes({f.quote("3.00", "3.20")}, {f.valuation()}, f.time, {{"SPY", f.time, m("500")}});
  snap = s.snapshot();
  EXPECT_TRUE(snap->positions.empty()); EXPECT_TRUE(snap->stocks.empty());
  EXPECT_FALSE(snap->evaluation.flat_pending);
  EXPECT_EQ(snap->recent_orders.at(1).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(snap->recent_orders.at(*reducing.order_id - 1).reason.code, Reason::POSITION_CLOSED);
  EXPECT_EQ(snap->evaluation.status, EvaluationStatus::Active);
  EXPECT_TRUE(snap->recent_orders.back().request.client_order_id.starts_with("system:flat_time:"));
  EXPECT_EQ(snap->stock_fills.back().source, StockSource::Rule);
}

TEST(TimeRules, FlatTimeComboClosesSplitAtOrderLimitAndRetryLiquidity) {
  ScriptedMarket f, g; g.contract = *md::parse_osi("SPXW261022C05010000");
  auto rules = plan(); rules.flat_time = 601; rules.defined_risk = true;
  auto c = config(rules); c.limits.max_order_contracts = 2;
  TradingSession s(c, f.time); f.seed(s); g.seed(s);
  auto entry = f.market("spread", 2); entry.symbol.clear();
  entry.legs = {{f.symbol(), Side::Buy, 1}, {g.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  entry.client_order_id = "spread-again"; ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  f.time = g.time = f.time + md::kNanosPerMinute; ++f.observation; ++g.observation;
  s.on_quotes({f.quote("4", "4.20", 1), g.quote("4", "4.20", 1)}, {f.valuation(), g.valuation()}, f.time);
  ASSERT_EQ(s.snapshot()->positions.size(), 2U);
  EXPECT_TRUE(s.snapshot()->evaluation.flat_pending);
  f.next(); g.next();
  s.on_quotes({f.quote(), g.quote()}, {f.valuation(), g.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  unsigned closes = 0;
  for (const auto& o : s.snapshot()->recent_orders) {
    if (!o.system) continue;
    ++closes; EXPECT_EQ(o.request.legs.size(), 2U); EXPECT_LE(o.request.quantity, 2);
    EXPECT_EQ(o.request.tif, TimeInForce::Ioc);
    EXPECT_TRUE(o.request.client_order_id.starts_with("system:flat_time:"));
  }
  EXPECT_GE(closes, 3U);
}

TEST(TimeRules, FlatTimeComboWaitsForRegularSessionAndRetriesAfterDayEnd) {
  ScriptedMarket f, g; g.contract = *md::parse_osi("SPXW261022C05010000");
  auto rules = plan(); rules.flat_time = 16 * 60 + 30;
  TradingSession s(config(rules), f.time); f.seed(s); g.seed(s);
  auto entry = f.market("spread"); entry.symbol.clear();
  entry.legs = {{f.symbol(), Side::Buy, 1}, {g.symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.submit(entry, f.time).decision.ok());
  f.time = g.time = md::new_york_to_utc({2026, 9, 22}, 16, 30); ++f.observation; ++g.observation;
  s.on_quotes({f.quote(), g.quote()}, {f.valuation(), g.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->positions.size(), 2U);
  EXPECT_TRUE(s.snapshot()->evaluation.flat_pending);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
  f.time = g.time = md::new_york_to_utc({2026, 9, 23}, 10, 0); ++f.observation; ++g.observation;
  s.on_quotes({f.quote(), g.quote()}, {f.valuation(), g.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_FALSE(s.snapshot()->evaluation.flat_pending);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
}

TEST(TimeRules, FlatTimeIocsRespectLatencyAndResumeOnNewQuotes) {
  ScriptedMarket f; auto rules = plan(); rules.flat_time = 601; rules.fill_latency_ms = 1000;
  auto c = config(rules); c.limits.max_order_contracts = 2;
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  ASSERT_FALSE(s.snapshot()->positions.empty());
  f.time = md::new_york_to_utc({2026, 9, 22}, 10, 1); ++f.observation;
  s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  const auto first = s.snapshot();
  ASSERT_TRUE(first->recent_orders.back().system);
  EXPECT_TRUE(first->recent_orders.back().open());
  EXPECT_FALSE(first->positions.empty());
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), first->recent_orders.size());
  EXPECT_TRUE(s.preview(f.limit("manual-preview", 1, "4.80", Side::Sell, TimeInForce::Gtc), f.time).decision.ok());
  EXPECT_TRUE(s.submit(f.limit("manual-close", 1, "4.80", Side::Sell, TimeInForce::Gtc), f.time).decision.ok());
  f.next(); s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_FALSE(s.snapshot()->evaluation.flat_pending);
}

TEST_P(EvaluationTimeRules, FlatTimeRunsOncePerDateRecoversAndCatchesGaps) {
  ScriptedMarket f; auto rules = plan(); rules.flat_time = 601;
  JournalFile file;
  {
    TradingSession s(config(rules), f.time, test::create_journal(file.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    f.time += 5 * md::kNanosPerMinute;
    s.on_quotes({}, {}, f.time); // first transaction is already past flat time
    EXPECT_TRUE(s.snapshot()->evaluation.flat_pending);
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), test::resume_journal(file.path));
  EXPECT_EQ(s.config().rules, rules);
  EXPECT_EQ(s.snapshot()->evaluation.flat_time_day, (md::Date{2026, 9, 22}));
  ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_FALSE(s.snapshot()->evaluation.flat_pending);
  s.on_quotes({}, {}, f.time + md::kNanosPerMinute);
  f.time = md::new_york_to_utc({2026, 9, 22}, 17, 0);
  s.on_quotes({}, {}, f.time);
  EXPECT_FALSE(plan_flat_now(rules, f.time));
  EXPECT_EQ(s.snapshot()->evaluation.flat_time_day, (md::Date{2026, 9, 22}));
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 5);
  s.on_quotes({}, {}, f.time); // even an idle flat account records the next date
  EXPECT_EQ(s.snapshot()->evaluation.flat_time_day, (md::Date{2026, 9, 23}));
  unsigned triggers = 0;
  for (const auto& record : FileJournal::read(file.path).records) {
    const auto j = nlohmann::json::parse(record.payload);
    for (const auto& event : j.value("events", nlohmann::json::array())) {
      if (event.at("type") == "flat_time") ++triggers;
    }
  }
  EXPECT_EQ(triggers, 2U);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}

TEST(TimeRules, NoOvernightFailsWithoutMarksInAllPhasesAndDecisionsAreSticky) {
  for (const auto phase : {Phase::Evaluation, Phase::Verification, Phase::Funded}) {
    ScriptedMarket f; auto rules = plan(); rules.no_overnight = true; rules.phase = phase;
    if (phase == Phase::Funded) rules.payouts.qualifying_days = 1;
    JournalFile file;
    TradingSession s(config(rules), f.time, test::create_journal(file.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    // Advancing beyond quote freshness does not defer the position-only verdict.
    f.time = md::new_york_to_utc({2026, 9, 22}, 17, 0);
    s.on_quotes({}, {}, f.time);
    EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
    EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::OVERNIGHT_HOLD);
    EXPECT_EQ(s.snapshot()->evaluation.decided_at, f.time);
    const auto decided = f.time;
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0); ++f.observation;
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:overnight:"));
    EXPECT_EQ(s.snapshot()->evaluation.decided_at, decided);
    EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
  }
}

TEST_P(EvaluationTimeRules, NoOvernightChecksSharesOnExplicitRolloverButExcludesSettlement) {
  ScriptedMarket f; auto rules = plan(); rules.no_overnight = true; rules.flat_time = 16 * 60 + 10;
  TradingSession shares(config(rules), f.time);
  ASSERT_TRUE(shares.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  const auto next = md::new_york_to_utc({2026, 9, 23}, 10, 0);
  (void)shares.roll_day(next);
  EXPECT_EQ(shares.snapshot()->evaluation.decision_code, Reason::OVERNIGHT_HOLD);
  f.contract = *md::parse_osi("SPXW260922C05000000");
  TradingSession options(config(rules), f.time); f.seed(options);
  ASSERT_TRUE(options.submit(f.market("expiring"), f.time).decision.ok());
  options.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 16, 10));
  ASSERT_EQ(options.snapshot()->positions.size(), 1U);
  EXPECT_TRUE(options.snapshot()->positions.front().awaiting_settlement);
  EXPECT_FALSE(options.snapshot()->evaluation.flat_pending);
  (void)options.roll_day(next);
  EXPECT_EQ(options.snapshot()->evaluation.status, EvaluationStatus::Active);
}

TEST(TimeRules, OvernightPositionFailureDoesNotWaitForMissingMarks) {
  ScriptedMarket f; auto rules = plan(); rules.no_overnight = true;
  JournalFile source, imported;
  {
    TradingSession s(config(rules), f.time, test::create_journal(source.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  }
  {
    auto s = TradingSession::recover(FileJournal::read(source.path), test::resume_journal(source.path));
    // The first transaction after recovery writes a complete checkpoint.
    f.next(); s.on_quotes({}, {}, f.time);
  }
  const auto recovery = FileJournal::read(source.path);
  auto payload = nlohmann::json::parse(recovery.records.back().payload);
  ASSERT_TRUE(payload.contains("state"));
  payload["state"]["marks"] = nlohmann::json::object();
  {
    const auto journal = test::create_journal(imported.path);
    for (std::size_t i = 0; i + 1 < recovery.records.size(); ++i) {
      const auto& record = recovery.records[i];
      journal->append(record.time, record.type, record.payload);
    }
    journal->append(f.time, "market", payload.dump());
  }
  auto s = TradingSession::recover(FileJournal::read(imported.path));
  ASSERT_FALSE(s.snapshot()->valuation_complete);
  const auto result = s.roll_day(md::new_york_to_utc({2026, 9, 23}, 10, 0));
  EXPECT_EQ(result.decision.code, Reason::STALE_QUOTE); // day accounting still needs marks
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::OVERNIGHT_HOLD);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
}

TEST(TimeRules, GapIntoLaterDatesFlatTimeClosesBeforeOpeningCanMatch) {
  ScriptedMarket f; auto rules = plan(); rules.flat_time = 601;
  TradingSession s(config(rules), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  const auto opening = s.submit(f.limit("working", 1, "3.80", Side::Buy, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(opening.decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 23}, 10, 5); ++f.observation;
  s.on_quotes({f.quote("3.60", "3.80")}, {f.valuation()}, f.time);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->recent_orders.at(*opening.order_id - 1).reason.code, Reason::FLAT_TIME);
  EXPECT_EQ(s.snapshot()->evaluation.flat_time_day, (md::Date{2026, 9, 23}));
}

TEST(TimeRules, FlatTimeValidationAndDayBoundary) {
  for (const auto minute : {-1, 1020, 1440}) {
    auto rules = plan(); rules.flat_time = minute;
    EXPECT_THROW(validate_rules(rules), TradingError);
  }
  auto rules = plan(); rules.flat_time = 0; EXPECT_NO_THROW(validate_rules(rules));
  rules.day_end_minutes = 1440; rules.flat_time = 1439; EXPECT_NO_THROW(validate_rules(rules));
  rules.flat_time = 945;
  for (const auto date : {md::Date{2026, 9, 22}, md::Date{2026, 11, 3}}) {
    EXPECT_FALSE(plan_flat_now(rules, md::new_york_to_utc(date, 15, 44)));
    EXPECT_TRUE(plan_flat_now(rules, md::new_york_to_utc(date, 15, 45)));
  }
  EXPECT_FALSE(plan_flat_now(rules, 0));
}

}  // namespace
}  // namespace openport::trading
