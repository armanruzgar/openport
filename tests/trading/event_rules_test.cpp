#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "openport/trading/events.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig config(AccountRules rules) {
  SessionConfig c; c.initial_cash = m("10000"); c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules); return c;
}
AccountRules news(const ScriptedMarket& f, std::string action = "block", std::string symbol = "") {
  AccountRules r; r.news_before_minutes = 1; r.news_after_minutes = 1; r.news_action = std::move(action);
  r.events = {{"news", md::format_timestamp(f.time + 2 * md::kNanosPerMinute), std::move(symbol), "", "CPI"}}; return r;
}
void tick(TradingSession& s, ScriptedMarket& f, Timestamp time) {
  f.time = time; ++f.observation; s.on_quotes({f.quote()}, {f.valuation()}, time);
}
TEST(EventRules, NewsRefusesPreviewCancelsOpeningsAndAllowsReductions) {
  ScriptedMarket f; auto r = news(f); TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
  const auto open = s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  const auto close = s.submit(f.limit("exit", 1, "4.80", Side::Sell, TimeInForce::Gtc), f.time);
  ASSERT_TRUE(open.decision.ok()) << open.decision.message;
  ASSERT_TRUE(close.decision.ok()) << close.decision.message;
  tick(s, f, f.time + md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->recent_orders.at(*open.order_id - 1).reason.code, Reason::NEWS_BLACKOUT);
  EXPECT_TRUE(s.snapshot()->recent_orders.at(*close.order_id - 1).open());
  const auto preview = s.preview(f.market("blocked"), f.time);
  EXPECT_EQ(preview.decision.code, Reason::NEWS_BLACKOUT); EXPECT_EQ(preview.decision.scope, "account");
  EXPECT_EQ(s.submit(f.market("blocked"), f.time).decision.code, Reason::NEWS_BLACKOUT);
  EXPECT_TRUE(s.submit(f.market("reduce", 1, Side::Sell), f.time).decision.ok());
  tick(s, f, f.time + 2 * md::kNanosPerMinute);
  EXPECT_TRUE(s.submit(f.market("end-exclusive"), f.time).decision.ok());
}
TEST(EventRules, SymbolScopesNewsAndShares) {
  ScriptedMarket f; auto r = news(f, "block", "SPY"); TradingSession s(config(r), f.time); f.seed(s);
  tick(s, f, f.time + md::kNanosPerMinute);
  EXPECT_TRUE(s.submit(f.market("spx"), f.time).decision.ok());
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.code, Reason::NEWS_BLACKOUT);
  EXPECT_TRUE(s.trade_stock("QQQ", 1, f.time, StockPrice{"QQQ", f.time, m("450")}).decision.ok());
}
TEST(EventRules, GapOverNewsDoesNotFlattenOrCancelWorkingOpenings) {
  ScriptedMarket f; auto r = news(f, "flatten"); TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  const auto open = s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
  tick(s, f, f.time + 4 * md::kNanosPerMinute);
  EXPECT_EQ(s.snapshot()->positions.size(), 1U); EXPECT_TRUE(s.snapshot()->recent_orders.at(*open.order_id - 1).open());
  EXPECT_TRUE(s.snapshot()->evaluation.event_actions.empty());
}
struct JournalFile {
  std::filesystem::path dir;
  std::string path;
  JournalFile() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-events-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp");
    dir = pattern; path = (dir / "journal.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(dir); }
};
TEST(EventRules, FlattenOnceWithRecoveryAndIdenticalReplay) {
  ScriptedMarket f; auto r = news(f, "flatten"); JournalFile file;
  std::string before;
  {
    TradingSession s(config(r), f.time, FileJournal::create(file.path)); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    tick(s, f, f.time + md::kNanosPerMinute);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:news:"));
    before = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(file.path), FileJournal::resume(file.path));
  EXPECT_EQ(s.snapshot_json(), before);
  const auto orders = s.snapshot()->recent_orders.size();
  tick(s, f, f.time + md::kNanosPerSecond);
  EXPECT_EQ(s.snapshot()->recent_orders.size(), orders);
  EXPECT_EQ(s.snapshot()->evaluation.event_actions.size(), 1U);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}
TEST(EventRules, FundedScalingWithFlatNewsAndHoldingRulesSurvivesResetAndRecovery) {
  for (const auto* first : {"flat_time", "news", "hold"}) {
    SCOPED_TRACE(first);
    const std::string trigger(first);
    ScriptedMarket f;
    AccountRules r;
    r.phase = Phase::Funded;
    r.payouts.qualifying_days = 1;
    r.scaling = {{Money{}, 2}, {m("100"), 4}};
    r.size_scaling = SizeScaling{1, 0, 1, 25, m("14000")};
    r.max_contracts_held = 4;
    r.min_hold_seconds = 3600; // Calendar and flat-time system closes bypass the user hold minimum.
    r.flat_time = trigger == "flat_time" ? 601 : 602;
    r.no_overnight = true;
    r.news_after_minutes = 5;
    r.news_action = "flatten";
    r.events = {{"news", trigger == "news" ? "2026-09-22T14:01:00Z" : "2026-09-22T14:02:00Z"},
                {"split", "2026-09-23", "SPX"}};
    r.hold_restrictions = {"split"};
    r.hold_cutoff = trigger == "hold" ? 601 : 602;
    EXPECT_TRUE(r.evaluation()); // Holding rules can decide even without a profit target or floor.
    auto evaluation_phase = r;
    evaluation_phase.phase = Phase::Evaluation;
    EXPECT_THROW(TradingSession(config(evaluation_phase), f.time), TradingError);

    JournalFile file;
    TradingSession s(config(r), f.time, FileJournal::create(file.path));
    const auto recorded_rules = s.config().rules;
    ASSERT_TRUE(recorded_rules.hold_calendar);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    const auto working = s.submit(f.limit("reserved", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
    ASSERT_TRUE(working.decision.ok());
    // One held contract and one working entry consume the session's scaling cap.
    EXPECT_EQ(s.preview(f.market("over-cap"), f.time).decision.code, Reason::SCALING_LIMIT);
    auto recovered = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(recovered.config().rules, recorded_rules);
    EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());

    // Each close mechanism must release the reservation, close through the cap,
    // and retain its label when the other two mechanisms become due.
    for (const auto minute : {1, 2}) {
      f.time = md::new_york_to_utc({2026, 9, 22}, 10, minute);
      ++f.observation;
      for (auto* session : {&s, &recovered}) {
        session->on_quotes({f.quote("6.20", "6.40")}, {f.valuation()}, f.time);
        EXPECT_TRUE(session->snapshot()->positions.empty());
        EXPECT_TRUE(session->snapshot()->open_orders.empty());
        EXPECT_TRUE(session->snapshot()->recent_orders.back().request.client_order_id.starts_with("system:" + trigger + ":"));
        EXPECT_EQ(session->snapshot()->evaluation.scaling_limit, 2);
        EXPECT_EQ(session->snapshot()->evaluation.status, EvaluationStatus::Active);
      }
      EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
    }
    const auto closed = s.snapshot();
    EXPECT_EQ(closed->recent_orders.size(), 3U);
    EXPECT_EQ(closed->recent_orders.at(*working.order_id - 1).reason.code,
        trigger == "flat_time" ? Reason::FLAT_TIME : trigger == "news" ? Reason::NEWS_BLACKOUT : Reason::HOLD_RESTRICTED);
    EXPECT_EQ(closed->evaluation.event_actions.size(), 2U);
    EXPECT_EQ(size_scaling_profit(closed->evaluation, plan_inputs(*closed).balance), m("198.70"));
    EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());

    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    for (auto* session : {&s, &recovered}) {
      ASSERT_TRUE(session->roll_day(f.time).decision.ok());
      const auto scaled = session->snapshot();
      EXPECT_EQ(scaled->evaluation.status, EvaluationStatus::Active);
      EXPECT_EQ(scaled->evaluation.scaling_limit, 4);
      EXPECT_EQ(scaled->evaluation.starting_balance, m("12500"));
      ASSERT_TRUE(scaled->evaluation.size_scaling);
      EXPECT_EQ(scaled->evaluation.size_scaling->history.size(), 1U);
      EXPECT_TRUE(scaled->evaluation.holding_violations.empty());
      ASSERT_TRUE(session->reset_account(m("10000"), r, "combined rules", f.time).decision.ok());
      EXPECT_EQ(session->config().rules, recorded_rules);
      EXPECT_EQ(*session->snapshot()->attempts.back().rules, recorded_rules);
      EXPECT_EQ(session->snapshot()->evaluation.scaling_limit, 2);
      ASSERT_TRUE(session->snapshot()->evaluation.size_scaling);
      EXPECT_TRUE(session->snapshot()->evaluation.size_scaling->history.empty());
      EXPECT_TRUE(session->snapshot()->evaluation.event_actions.empty());
    }
    EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
    const auto reset = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(reset.config().rules, recorded_rules);
    EXPECT_EQ(reset.snapshot_json(), s.snapshot_json());
  }
}
TEST(EventRules, HoldingFailurePreventsAnOtherwiseEarnedFundedSizeIncrease) {
  for (const bool overnight : {false, true}) {
    SCOPED_TRACE(overnight);
    ScriptedMarket f;
    f.time = md::new_york_to_utc({2026, 9, 25}, 10, 0);
    auto r = news(f, "flatten");
    r.phase = Phase::Funded;
    r.payouts.qualifying_days = 1;
    r.scaling = {{Money{}, 2}, {m("100"), 4}};
    r.size_scaling = SizeScaling{1, 0, 1, 25, m("14000")};
    r.flat_time = 945;
    r.no_overnight = overnight;
    r.hold_restrictions = {"weekend"};
    EXPECT_TRUE(r.evaluation()); // F59 alone activates failure checks when overnight is off.
    JournalFile file;
    TradingSession s(config(r), f.time, FileJournal::create(file.path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("profit-open"), f.time).decision.ok());
    f.next();
    s.on_quotes({f.quote("6.20", "6.40")}, {f.valuation()}, f.time);
    ASSERT_TRUE(s.submit(f.market("profit-close", 1, Side::Sell), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("weekend-hold"), f.time).decision.ok());
    const auto before = s.snapshot();
    ASSERT_GT(size_scaling_profit(before->evaluation, plan_inputs(*before).balance), m("100"));

    // No fresh closing liquidity arrives before the boundary. The earned profit
    // cannot qualify this failed attempt for a capital increase at the review.
    const auto rollover = md::new_york_to_utc({2026, 9, 25}, 17, 0);
    ASSERT_TRUE(s.roll_day(rollover).decision.ok());
    const auto after = s.snapshot();
    EXPECT_EQ(after->evaluation.status, EvaluationStatus::Failed);
    EXPECT_EQ(after->evaluation.decision_code, overnight ? Reason::OVERNIGHT_HOLD : Reason::HOLD_RESTRICTED);
    EXPECT_EQ(after->evaluation.starting_balance, m("10000"));
    ASSERT_TRUE(after->evaluation.size_scaling);
    EXPECT_TRUE(after->evaluation.size_scaling->history.empty());
    EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
    tick(s, f, md::new_york_to_utc({2026, 9, 28}, 10, 0));
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_EQ(s.snapshot()->evaluation.decided_at, rollover);
    EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
  }
}
TEST(EventRules, EachCorporateKindAndEarningsSessionHasCorrectCutoff) {
  for (const auto& kind : {"earnings", "ex_dividend", "split"}) {
    for (const auto& session : {"before_open", "after_close"}) {
      if (std::string(kind) != "earnings" && std::string(session) == "after_close") continue;
      AccountRules r; r.hold_restrictions = {kind};
      r.events = {{kind, "2026-09-23", "SPY", std::string(kind) == "earnings" ? session : "", "event"}};
      normalize_event_rules(r, true);
      const auto day = std::string(session) == "after_close" ? md::Date{2026, 9, 23} : md::Date{2026, 9, 22};
      const auto at = md::new_york_to_utc(day, 15, 45);
      EXPECT_TRUE(event_entry_check(r, "SPY", at - 1).ok());
      const auto d = event_entry_check(r, "SPY", at);
      EXPECT_EQ(d.code, Reason::HOLD_RESTRICTED); EXPECT_EQ(d.scope, std::string(kind) + ":SPY");
      EXPECT_TRUE(event_entry_check(r, "SPX", at).ok());
      EXPECT_TRUE(event_entry_check(r, "SPY", md::new_york_to_utc(day, 17, 0)).ok());
    }
  }
}
TEST(EventRules, WeekendHolidayAndFrozenBusinessCalendar) {
  AccountRules r; r.hold_restrictions = {"weekend"}; normalize_event_rules(r, true);
  EXPECT_EQ(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 4, 2}, 15, 45)).code, Reason::HOLD_RESTRICTED); // Good Friday
  EXPECT_TRUE(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 4, 1}, 15, 45)).ok());
  EXPECT_EQ(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 9, 25}, 15, 45)).code, Reason::HOLD_RESTRICTED);
  md::ScheduledDaysScope changed({{{2026, 9, 25}, "New closure", true, 13, 0}});
  EXPECT_TRUE(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 9, 24}, 15, 45)).ok());
  EXPECT_EQ(event_entry_check(r, "SPY", md::new_york_to_utc({2026, 9, 25}, 15, 45)).code, Reason::HOLD_RESTRICTED);
}
TEST(EventRules, HoldingCancelsAndClosesOptionsAndSharesOnce) {
  for (const auto& kind : {"earnings", "ex_dividend", "split", "weekend"}) {
    ScriptedMarket f; f.contract = *md::parse_osi("SPY261022C00500000"); f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
    AccountRules r; r.hold_restrictions = {kind};
    if (std::string(kind) != "weekend") r.events = {{kind, "2026-09-28", "SPY", "", ""}};
    TradingSession s(config(r), f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
    const auto open = s.submit(f.limit("working", 1, "3.50", Side::Buy, TimeInForce::Gtc), f.time);
    f.time += md::kNanosPerMinute; ++f.observation;
    s.on_quotes({f.quote()}, {f.valuation()}, f.time, {{"SPY", f.time, m("500")}});
    EXPECT_TRUE(s.snapshot()->positions.empty()); EXPECT_TRUE(s.snapshot()->stocks.empty());
    EXPECT_EQ(s.snapshot()->recent_orders.at(*open.order_id - 1).reason.code, Reason::HOLD_RESTRICTED);
    EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:hold:"));
    const auto count = s.snapshot()->recent_orders.size(); tick(s, f, f.time + md::kNanosPerSecond);
    EXPECT_EQ(s.snapshot()->recent_orders.size(), count);
    EXPECT_EQ(s.preview(f.market("no-entry"), f.time).decision.code, Reason::HOLD_RESTRICTED);
  }
}
TEST(EventRules, GapAcrossHoldingBoundaryFailsAtRolloverEvenAfterLateClose) {
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
  AccountRules r; r.hold_restrictions = {"weekend"}; TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  tick(s, f, md::new_york_to_utc({2026, 9, 28}, 10, 0));
  EXPECT_TRUE(s.snapshot()->positions.empty()); // missed cutoff executes once with Monday liquidity
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::HOLD_RESTRICTED);
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
}
TEST(EventRules, FailedCutoffLiquidityFailsAtRollover) {
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
  AccountRules r; r.hold_restrictions = {"weekend"}; TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 25}, 15, 45); ++f.observation;
  auto no_bid = f.quote(); no_bid.bid.reset(); no_bid.bid_size = 0;
  s.on_quotes({no_bid}, {f.valuation()}, f.time);
  ASSERT_FALSE(s.snapshot()->positions.empty());
  ASSERT_TRUE(s.roll_day(md::new_york_to_utc({2026, 9, 25}, 17, 0)).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::HOLD_RESTRICTED);
}
TEST(EventRules, OverlappingLateClosesPreserveEveryCrossedHoldingBoundary) {
  ScriptedMarket f; f.time = md::new_york_to_utc({2026, 9, 25}, 14, 0);
  AccountRules r; r.hold_restrictions = {"earnings", "weekend"};
  r.events = {{"earnings", "2026-09-28", "SPX"}};
  TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  // Both Friday cutoffs were missed. The earnings close sorts first, but must
  // not erase evidence that this position also crossed the weekend boundary.
  tick(s, f, md::new_york_to_utc({2026, 9, 28}, 10, 0));
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->evaluation.holding_violations,
      (std::vector<std::string>{"earnings:SPX", "weekend:account"}));
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::HOLD_RESTRICTED);
}
TEST(EventRules, SettlementAwaitingPositionIsSkipped) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW260922C05000000");
  AccountRules r; r.news_after_minutes = 10; r.news_action = "flatten";
  r.events = {{"news", "2026-09-22T20:01:00Z", "", "", ""}};
  TradingSession s(config(r), f.time); f.seed(s); ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  tick(s, f, md::new_york_to_utc({2026, 9, 22}, 16, 1));
  EXPECT_EQ(s.snapshot()->positions.size(), 1U); EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
  EXPECT_EQ(s.snapshot()->evaluation.event_actions.size(), 1U);
}
TEST(EventRules, HoldingSkipsSettlementAwaitingPositionAndRecordsReplayIdentically) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW260922C05000000");
  AccountRules r; r.hold_restrictions = {"split"}; r.hold_cutoff = 16 * 60 + 5;
  r.events = {{"split", "2026-09-23", "SPX"}};
  JournalFile first, second;
  const auto run = [&](const std::string& path) {
    TradingSession s(config(r), f.time, FileJournal::create(path)); f.seed(s);
    EXPECT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
    const auto cutoff = md::new_york_to_utc({2026, 9, 22}, 16, 5);
    s.on_quotes({}, {}, cutoff);
    EXPECT_EQ(s.snapshot()->positions.size(), 1U); EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
    EXPECT_EQ(s.snapshot()->evaluation.event_actions.size(), 1U);
  };
  run(first.path); run(second.path);
  EXPECT_EQ(FileJournal::read(first.path).head, FileJournal::read(second.path).head);
  auto recovered = TradingSession::recover(FileJournal::read(first.path));
  EXPECT_EQ(recovered.snapshot()->evaluation.event_actions.size(), 1U);
  recovered.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 16, 6));
  EXPECT_EQ(recovered.snapshot()->recent_orders.size(), 1U);
}
TEST(EventRules, OverlappingNewsAndDuplicateHoldingScopesActDeterministically) {
  ScriptedMarket f; auto r = news(f);
  r.events.push_back({"news", md::format_timestamp(f.time + 3 * md::kNanosPerMinute), "SPY", "", "FOMC"});
  normalize_event_rules(r);
  EXPECT_EQ(event_entry_check(r, "QQQ", f.time + md::kNanosPerMinute).scope, "account");
  EXPECT_TRUE(event_entry_check(r, "QQQ", f.time + 3 * md::kNanosPerMinute).ok());
  EXPECT_EQ(event_entry_check(r, "SPY", f.time + 3 * md::kNanosPerMinute).code, Reason::NEWS_BLACKOUT);
  r.hold_restrictions = {"split"}; r.events = {{"split", "2026-09-23", "SPY", "", "First"}, {"split", "2026-09-23", "SPY", "", "Second"}};
  normalize_event_rules(r, true);
  EXPECT_EQ(event_windows(r, md::new_york_to_utc({2026, 9, 22}, 15, 45), md::new_york_to_utc({2026, 9, 22}, 15, 45)).size(), 1U);
}
TEST(EventRules, MidnightDayEndKeepsHoldingWindowsUntilTheNextDay) {
  AccountRules r; r.day_end_minutes = 24 * 60; r.hold_restrictions = {"weekend"};
  normalize_event_rules(r, true);
  const auto friday = md::new_york_to_utc({2026, 9, 25}, 15, 50);
  const auto windows = event_windows(r, friday, friday);
  ASSERT_EQ(windows.size(), 1U);
  EXPECT_EQ(windows.front().start, md::new_york_to_utc({2026, 9, 25}, 15, 45));
  EXPECT_EQ(windows.front().end, md::new_york_to_utc({2026, 9, 26}, 0, 0));
  EXPECT_EQ(event_entry_check(r, "SPX", friday).code, Reason::HOLD_RESTRICTED);
}
TEST(EventRules, CombinedRolloverPrecedenceAndRecoveryBeforeLateCloses) {
  for (const bool overnight : {false, true}) {
    for (const bool explicit_roll : {false, true}) {
      SCOPED_TRACE(overnight);
      SCOPED_TRACE(explicit_roll);
      ScriptedMarket f; f.time = md::new_york_to_utc({2026, 9, 25}, 15, 44);
      AccountRules r; r.no_overnight = overnight; r.flat_time = 15 * 60 + 45;
      r.hold_restrictions = {"weekend", "earnings"};
      r.events = {{"earnings", "2026-09-28", "SPX"}, {"news", "2026-09-28T14:00:00Z"}};
      r.news_after_minutes = 10; r.news_action = "flatten";
      JournalFile file;
      TradingSession s(config(r), f.time, FileJournal::create(file.path)); f.seed(s);
      ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
      const auto rollover = md::new_york_to_utc({2026, 9, 25}, 17, 0);
      if (explicit_roll) { ASSERT_TRUE(s.roll_day(rollover).decision.ok()); }
      else s.on_quotes({}, {}, rollover);
      const auto expected = overnight ? Reason::OVERNIGHT_HOLD : Reason::HOLD_RESTRICTED;
      EXPECT_EQ(s.snapshot()->evaluation.decision_code, expected);
      EXPECT_EQ(s.snapshot()->evaluation.holding_violations,
          (std::vector<std::string>{"earnings:SPX", "weekend:account"}));
      EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
      tick(s, f, md::new_york_to_utc({2026, 9, 28}, 10, 0));
      EXPECT_TRUE(s.snapshot()->positions.empty());
      EXPECT_EQ(s.snapshot()->recent_orders.size(), 2U);
      EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with(
          overnight ? "system:overnight:" : "system:hold:"));
      EXPECT_EQ(s.snapshot()->evaluation.decided_at, rollover);
      EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
    }
  }
}
TEST(EventRules, CombinedCutoffsKeepFirstCloseLabelWithoutDuplicates) {
  for (const auto* first : {"flat_time", "news", "hold", "tie"}) {
    for (const auto latency : {0, 1000}) {
      SCOPED_TRACE(first);
      SCOPED_TRACE(latency);
      ScriptedMarket f; f.contract = *md::parse_osi("SPY261022C00500000");
      const std::string trigger(first);
      AccountRules r; r.flat_time = trigger == "flat_time" || trigger == "tie" ? 601 : 602;
      r.no_overnight = true; r.fill_latency_ms = latency;
      r.news_after_minutes = 5; r.news_action = "flatten";
      r.events = {{"news", trigger == "news" || trigger == "tie" ? "2026-09-22T14:01:00Z" : "2026-09-22T14:02:00Z"},
          {"split", "2026-09-23", "SPY"}};
      r.hold_restrictions = {"split"}; r.hold_cutoff = trigger == "hold" || trigger == "tie" ? 601 : 602;
      JournalFile file;
      TradingSession s(config(r), f.time, FileJournal::create(file.path)); f.seed(s);
      ASSERT_TRUE(s.submit(f.market("held", 2), f.time).decision.ok());
      ASSERT_TRUE(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
      tick(s, f, f.time + md::kNanosPerSecond);
      f.time = md::new_york_to_utc({2026, 9, 22}, 10, 1); ++f.observation;
      s.on_quotes({f.quote()}, {f.valuation()}, f.time, {{"SPY", f.time, m("500")}});
      const auto label = trigger == "tie" ? "flat_time" : trigger;
      ASSERT_EQ(s.snapshot()->recent_orders.size(), 2U);
      EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:" + label + ":"));
      auto recovered = TradingSession::recover(FileJournal::read(file.path));
      EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
      // Pending latency closes retain ownership when the remaining triggers fire.
      f.time = md::new_york_to_utc({2026, 9, 22}, 10, 2); ++f.observation;
      for (auto* session : {&s, &recovered}) {
        session->on_quotes({f.quote()}, {f.valuation()}, f.time, {{"SPY", f.time, m("500")}});
        EXPECT_TRUE(session->snapshot()->stocks.empty());
        EXPECT_EQ(session->snapshot()->stock_fills.size(), 2U);
        EXPECT_EQ(session->snapshot()->stock_fills.back().source, StockSource::Rule);
        EXPECT_TRUE(session->snapshot()->positions.empty());
        EXPECT_EQ(session->snapshot()->recent_orders.size(), 2U);
        EXPECT_TRUE(session->snapshot()->recent_orders.back().request.client_order_id.starts_with("system:" + label + ":"));
        EXPECT_EQ(session->snapshot()->evaluation.event_actions.size(), 2U);
        EXPECT_EQ(session->snapshot()->evaluation.status, EvaluationStatus::Active);
      }
      EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
    }
  }
}
TEST(EventRules, UnrelatedCalendarPreservesFlatTimeEquityDecisionOrder) {
  for (const int calendar : {0, 1, 2}) {
    SCOPED_TRACE(calendar);
    ScriptedMarket f;
    AccountRules r; r.flat_time = 601; r.profit_target = m("100");
    if (calendar) {
      r.news_after_minutes = 1; r.news_action = "flatten";
      r.events = {{"news", calendar == 1 ? "2026-09-23T14:00:00Z" : "2026-09-22T14:01:00Z", "SPY"}};
      r.hold_restrictions = {"weekend"};
    }
    TradingSession s(config(r), f.time); f.seed(s);
    ASSERT_TRUE(s.submit(f.market("held", 5), f.time).decision.ok());
    f.time = md::new_york_to_utc({2026, 9, 22}, 10, 1); ++f.observation;
    s.on_quotes({f.quote("4.40", "4.60")}, {f.valuation()}, f.time);
    EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
    EXPECT_EQ(s.snapshot()->evaluation.decision_code, Reason::PROFIT_TARGET);
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:target:"));
  }
}
TEST(EventRules, BookkeepingRolloverDoesNotFailPositionsOpenedOnTheNewDate) {
  ScriptedMarket f;
  AccountRules r; r.no_overnight = true; r.hold_restrictions = {"weekend"};
  TradingSession s(config(r), f.time); f.seed(s);
  tick(s, f, md::new_york_to_utc({2026, 9, 23}, 10, 0));
  ASSERT_TRUE(s.submit(f.market("new-day-entry"), f.time).decision.ok());
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
}
TEST(EventRules, CombinedHoldingAndOvernightExcludeSettlementAtTheBoundary) {
  ScriptedMarket f; f.contract = *md::parse_osi("SPXW260922C05000000");
  AccountRules r; r.no_overnight = true; r.flat_time = 16 * 60 + 5;
  r.hold_restrictions = {"split"}; r.hold_cutoff = 16 * 60 + 5;
  r.events = {{"split", "2026-09-23", "SPX"}};
  TradingSession s(config(r), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("held"), f.time).decision.ok());
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 17, 0));
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
  EXPECT_TRUE(s.snapshot()->evaluation.holding_violations.empty());
  EXPECT_EQ(s.snapshot()->recent_orders.size(), 1U);
}
TEST(EventRules, NormalizeAndValidateCalendar) {
  AccountRules r; r.events = {{"earnings", "2026-09-23", "SPY"}, {"news", "2026-09-22T14:00:00Z"}, {"earnings", "2026-09-23", "SPY", "before_open"}};
  normalize_event_rules(r); ASSERT_EQ(r.events.size(), 2U); EXPECT_EQ(r.events[0].kind, "news"); EXPECT_EQ(r.events[1].session, "before_open");
  r.news_before_minutes = 241; EXPECT_THROW(validate_event_rules(r), TradingError); r.news_before_minutes = 0;
  r.events[1].symbol = "spy"; EXPECT_THROW(validate_event_rules(r), TradingError);
}
}  // namespace
}  // namespace openport::trading
