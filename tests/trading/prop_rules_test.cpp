#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "openport/md/time.hpp"
#include "support/scripted_market.hpp"
#include "trading/state.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig config(AccountRules rules, Guardrails guardrails = {}) {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  c.guardrails = guardrails;
  return c;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation()}, f.time);
}
void next_day(TradingSession& s, ScriptedMarket& f, md::Date day) {
  f.time = md::new_york_to_utc(day, 10, 0);
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.roll_day(f.time).decision.ok());
}
/// One closed round trip of one contract with exactly `profit` net of both fees.
void profit_day(TradingSession& s, ScriptedMarket& f, std::string_view profit) {
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("open-" + std::to_string(f.observation)), f.time).decision.ok());
  const auto bid = m("4.20") + (m(profit) + s.config().fee_per_contract * 2).prorate(1, 100);
  quote(s, f, bid.str(), (bid + m("0.20")).str());
  const auto closed = s.submit(f.market("close-" + std::to_string(f.observation), 1, Side::Sell), f.time);
  ASSERT_TRUE(closed.decision.ok()) << closed.decision.message;
}
struct JournalFile {
  std::filesystem::path directory;
  std::string path;
  JournalFile() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-prop-rules-XXXXXX").string();
    if (::mkdtemp(pattern.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    directory = pattern;
    path = (directory / "account.jsonl").string();
  }
  ~JournalFile() { std::filesystem::remove_all(directory); }
};
std::vector<nlohmann::json> events_of(const std::string& path, std::string_view type) {
  std::vector<nlohmann::json> found;
  for (const auto& record : FileJournal::read(path).records) {
    const auto payload = nlohmann::json::parse(record.payload);
    for (const auto& event : payload.at("events"))
      if (event.at("type") == std::string(type)) found.push_back(event.at("payload"));
  }
  return found;
}
AccountRules open_loss(OpenLossAction action, std::string_view amount = "300") {
  AccountRules r;
  r.plan = "Open loss test";
  r.open_loss_cap = OpenLossCap{OpenLossBasis::Amount, m(amount), 0, action};
  return r;
}
const Objective* objective(const std::vector<Objective>& list, Reason code) {
  for (const auto& o : list) if (o.code == code) return &o;
  return nullptr;
}

TEST(PropRules, NewRulesAreOffAndKeepEveryRecordedByte) {
  const Json rules = AccountRules{};
  for (const auto* key : {"drawdown_basis", "open_loss_cap", "minute_volume_percent", "best_position_percent"})
    EXPECT_FALSE(rules.contains(key)) << key;
  const Json payouts = PayoutRules{};
  for (const auto* key : {"review", "after_payout", "reset_percent"}) EXPECT_FALSE(payouts.contains(key)) << key;
  const Json guardrails = Guardrails{};
  EXPECT_EQ(guardrails.size(), 6U);
  const Json state = GuardrailState{};
  EXPECT_EQ(state.size(), 4U);
  const Json evaluation = Evaluation{};
  for (const auto* key : {"day_close_balance", "open_loss_symbols", "open_loss_trades", "position_excess", "best_position", "cycle_trades"})
    EXPECT_FALSE(evaluation.contains(key)) << key;
  const Json payout = Payout{};
  EXPECT_FALSE(payout.contains("review"));
  EXPECT_FALSE(payout.contains("removed"));
  EXPECT_FALSE(AccountRules{}.evaluation());
  EXPECT_TRUE(AccountRules{}.practice());
}

TEST(PropRules, NewRuleFieldsAreProgramRulesAndRoundTrip) {
  AccountRules base;
  base.plan = "Base";
  base.profit_target = m("1000");
  base.max_drawdown = m("500");
  auto changed = base;
  changed.open_loss_cap = OpenLossCap{OpenLossBasis::DrawdownRoom, {}, 30, OpenLossAction::BlockAdds};
  EXPECT_FALSE(same_program_rules(changed, base));
  for (auto edit : std::vector<std::function<void(AccountRules&)>>{
           [](AccountRules& r) { r.drawdown_basis = DrawdownBasis::Balance; },
           [](AccountRules& r) { r.minute_volume_percent = 10; },
           [](AccountRules& r) { r.best_position_percent = 40; }}) {
    auto other = base;
    edit(other);
    EXPECT_FALSE(same_program_rules(other, base));
    validate_rules(other);
    const Json encoded = other;
    EXPECT_EQ(encoded.get<AccountRules>(), other);
  }
  validate_rules(changed);
  const Json encoded = changed;
  EXPECT_EQ(encoded.get<AccountRules>(), changed);
  auto funded = base;
  funded.profit_target = {};
  funded.phase = Phase::Funded;
  funded.payouts.qualifying_days = 1;
  funded.payouts.review = PayoutReview{60, 40, false, {ReviewStep::Warning, ReviewStep::Cap}, m("250")};
  funded.payouts.after_payout = PayoutFloor::Reset;
  funded.payouts.reset_percent = 90;
  validate_rules(funded);
  const Json payouts = funded;
  EXPECT_EQ(payouts.get<AccountRules>(), funded);
  EXPECT_FALSE(same_program_rules(funded, [&] { auto r = funded; r.payouts.review.reset(); return r; }()));
}

TEST(PropRules, ValidationRefusesIncoherentPropRules) {
  const auto invalid = [](const std::function<void(AccountRules&)>& edit) {
    AccountRules r;
    r.profit_target = m("1000");
    edit(r);
    EXPECT_THROW(validate_rules(r), TradingError);
  };
  invalid([](AccountRules& r) { r.open_loss_cap = OpenLossCap{OpenLossBasis::Amount, {}, 0, OpenLossAction::Flatten}; });
  invalid([](AccountRules& r) { r.open_loss_cap = OpenLossCap{OpenLossBasis::DayProfit, {}, 101, OpenLossAction::Flatten}; });
  invalid([](AccountRules& r) { r.open_loss_cap = OpenLossCap{OpenLossBasis::DrawdownRoom, {}, 30, OpenLossAction::Fail}; });
  invalid([](AccountRules& r) { r.minute_volume_percent = 1001; });
  invalid([](AccountRules& r) { r.best_position_percent = 50; r.profit_target = {}; r.max_drawdown = m("100"); });
  invalid([](AccountRules& r) { r.payouts.review = PayoutReview{60, 0, false, {ReviewStep::Warning}, {}}; });
  invalid([](AccountRules& r) {
    r.profit_target = {}; r.max_drawdown = m("500"); r.phase = Phase::Funded; r.payouts.qualifying_days = 1;
    r.payouts.review = PayoutReview{60, 0, false, {ReviewStep::Cap}, {}};
  });
  invalid([](AccountRules& r) { r.payouts.after_payout = PayoutFloor::StartingBalance; });
  invalid([](AccountRules& r) {
    r.profit_target = {}; r.max_drawdown = m("500"); r.phase = Phase::Funded; r.payouts.qualifying_days = 1;
    r.payouts.reset_percent = 50;
  });
  Guardrails g;
  g.loss_streak = 3;
  EXPECT_THROW(validate_guardrails(g), TradingError);
  g.cooldown_minutes = 5;
  EXPECT_NO_THROW(validate_guardrails(g));
  g.size_creep_percent = 100;
  EXPECT_THROW(validate_guardrails(g), TradingError);
  g.size_creep_percent = 200;
  g.size_creep_window = 2;
  EXPECT_THROW(validate_guardrails(g), TradingError);
  g.size_creep_window = 10;
  g.size_creep_block = true;
  EXPECT_NO_THROW(validate_guardrails(g));
}

TEST(PropRules, AnOpenLossCapFlattensThePositionAndJournalsTheBreach) {
  ScriptedMarket f;
  JournalFile file;
  TradingSession s(config(open_loss(OpenLossAction::Flatten)), f.time, FileJournal::create(file.path));
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  // Marked at the 3.70 mid the open loss is $250, inside the $300 cap.
  quote(s, f, "3.60", "3.80");
  EXPECT_TRUE(s.snapshot()->evaluation.open_loss_symbols.empty());
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  // At the 3.50 mid it is $350: the account closes it at the bid.
  quote(s, f, "3.40", "3.60");
  const auto& snapshot = *s.snapshot();
  EXPECT_TRUE(snapshot.positions.empty());
  EXPECT_EQ(snapshot.evaluation.status, EvaluationStatus::Active);
  ASSERT_EQ(snapshot.evaluation.open_loss_trades.size(), 1U);
  const auto close = snapshot.recent_orders.back();
  EXPECT_TRUE(close.system);
  EXPECT_TRUE(close.request.client_order_id.starts_with("system:open_loss:"));
  const auto breaches = events_of(file.path, "open_loss_cap");
  ASSERT_EQ(breaches.size(), 1U);
  EXPECT_EQ(breaches[0].at("loss"), m("350").micros());
  EXPECT_EQ(breaches[0].at("limit"), m("300").micros());
  EXPECT_EQ(breaches[0].at("action"), "flatten");
  quote(s, f, "4.00", "4.20");
  EXPECT_TRUE(s.snapshot()->evaluation.open_loss_symbols.empty());
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}

/// A 5000 call and a 5010 call on one expiry, quoted together.
struct Vertical {
  ScriptedMarket a, b;
  Vertical() { b.contract.strike += 10; }
  void seed(TradingSession& s) { a.seed(s); b.seed(s, "2.00", "2.20"); }
  void quote(TradingSession& s, std::string_view a_bid, std::string_view b_bid) {
    a.next();
    b.next();
    b.time = a.time;
    const auto ask = [](std::string_view bid) { return (Money::parse(bid) + Money::parse("0.20")).str(); };
    s.on_quotes({a.quote(a_bid, ask(a_bid)), b.quote(b_bid, ask(b_bid))}, {a.valuation(), b.valuation()}, a.time);
  }
  /// Buys five 5000 calls and sells five 5010 calls as one debit vertical.
  OrderRequest open() const {
    OrderRequest r;
    r.client_order_id = "vertical";
    r.type = OrderType::Market;
    r.tif = TimeInForce::Ioc;
    r.quantity = 5;
    r.legs = {{a.symbol(), Side::Buy, 1}, {b.symbol(), Side::Sell, 1}};
    return r;
  }
};

TEST(PropRules, AnOpenLossCapMeasuresASpreadNetAndClosesItWhole) {
  Vertical v;
  JournalFile file;
  TradingSession s(config(open_loss(OpenLossAction::Flatten)), v.a.time, FileJournal::create(file.path));
  v.seed(s);
  const auto opened = s.submit(v.open(), v.a.time);
  ASSERT_TRUE(opened.decision.ok()) << opened.decision.message;
  // The long leg is $350 under its 4.20 cost at the 3.50 mid, but the short leg is
  // $250 over its 2.00 credit at 1.50: the trade is down $100, inside the cap.
  v.quote(s, "3.40", "1.40");
  EXPECT_TRUE(s.snapshot()->evaluation.open_loss_symbols.empty());
  EXPECT_EQ(s.snapshot()->positions.size(), 2U);
  // Down $550 and up $100: $450 net reaches the cap, and the whole vertical closes
  // as one order, the short bought back with the long sold.
  v.quote(s, "3.00", "1.70");
  const auto& snapshot = *s.snapshot();
  EXPECT_TRUE(snapshot.positions.empty());
  const auto close = snapshot.recent_orders.back();
  EXPECT_TRUE(close.system);
  EXPECT_TRUE(close.request.client_order_id.starts_with("system:open_loss:"));
  ASSERT_EQ(close.request.legs.size(), 2U);
  EXPECT_EQ(close.request.legs[0].symbol, v.b.symbol());
  EXPECT_EQ(close.request.legs[0].side, Side::Buy);
  EXPECT_EQ(snapshot.evaluation.open_loss_trades.size(), 2U);
  const auto breaches = events_of(file.path, "open_loss_cap");
  ASSERT_EQ(breaches.size(), 1U);
  EXPECT_EQ(breaches[0].at("trade"), "1");
  EXPECT_EQ(breaches[0].at("symbols").size(), 2U);
  EXPECT_EQ(breaches[0].at("loss"), m("450").micros());
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}

TEST(PropRules, ABuyOnlyTradeIsMeasuredAcrossItsLegs) {
  Vertical v;
  auto rules = open_loss(OpenLossAction::Flatten);
  rules.buy_only = true;
  TradingSession s(config(rules), v.a.time);
  v.seed(s);
  ASSERT_TRUE(s.submit(v.a.market("long", 5), v.a.time).decision.ok());
  auto second = v.b.market("joined", 5);
  second.group = "1";
  const auto joined = s.submit(second, v.a.time);
  ASSERT_TRUE(joined.decision.ok()) << joined.decision.message;
  // $350 down on the 5000 calls, $150 up on the 5010 calls bought at 2.20.
  v.quote(s, "3.40", "2.40");
  EXPECT_TRUE(s.snapshot()->evaluation.open_loss_symbols.empty());
  EXPECT_EQ(s.snapshot()->positions.size(), 2U);
  // $350 and $50 down: $400 net, so both legs are sold.
  v.quote(s, "3.40", "2.00");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->evaluation.open_loss_trades.size(), 2U);
}

TEST(PropRules, AFlattenPendingForAnOpenLossBlocksAddingToTheTrade) {
  ScriptedMarket f;
  auto rules = open_loss(OpenLossAction::Flatten);
  rules.fill_latency_ms = 500;
  TradingSession s(config(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "4.00", "4.20");
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  // The delayed close is pending, so the position is still held but cannot grow.
  quote(s, f, "3.40", "3.60");
  ASSERT_EQ(s.snapshot()->positions.size(), 1U);
  ASSERT_EQ(s.snapshot()->evaluation.open_loss_symbols, std::vector<std::string>{f.symbol()});
  EXPECT_EQ(s.submit(f.market("add"), f.time).decision.code, Reason::OPEN_LOSS_CAP);
}

TEST(PropRules, BlockAddsRefusesAddingUntilThePositionRecovers) {
  ScriptedMarket f;
  TradingSession s(config(open_loss(OpenLossAction::BlockAdds)), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "3.40", "3.60");
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  ASSERT_EQ(s.snapshot()->evaluation.open_loss_symbols, std::vector<std::string>{f.symbol()});
  const auto refused = s.submit(f.market("add"), f.time);
  EXPECT_EQ(refused.decision.code, Reason::OPEN_LOSS_CAP);
  EXPECT_EQ(s.preview(f.market("add-preview"), f.time).decision.code, Reason::OPEN_LOSS_CAP);
  EXPECT_TRUE(s.submit(f.market("reduce", 1, Side::Sell), f.time).decision.ok());
  // Four contracts at the 4.10 mid are $40 under their cost: back within the cap.
  quote(s, f, "4.00", "4.20");
  EXPECT_TRUE(s.snapshot()->evaluation.open_loss_symbols.empty());
  EXPECT_TRUE(s.submit(f.market("add-again"), f.time).decision.ok());
}

TEST(PropRules, AFailActionFailsTheAttemptWithItsCode) {
  ScriptedMarket f;
  TradingSession s(config(open_loss(OpenLossAction::Fail)), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
  quote(s, f, "3.40", "3.60");
  const auto& e = s.snapshot()->evaluation;
  EXPECT_EQ(e.status, EvaluationStatus::Failed);
  EXPECT_EQ(e.decision_code, Reason::OPEN_LOSS_CAP);
  EXPECT_NE(e.decision.find("open-loss cap of $300.00"), std::string::npos) << e.decision;
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_TRUE(s.snapshot()->recent_orders.back().request.client_order_id.starts_with("system:open_loss:"));
}

TEST(PropRules, PercentageCapsReadTheDayProfitOrTheDrawdownRoom) {
  ScriptedMarket f;
  AccountRules r;
  r.plan = "Open loss percent";
  r.max_drawdown = m("1000");
  r.open_loss_cap = OpenLossCap{OpenLossBasis::DrawdownRoom, {}, 30, OpenLossAction::Flatten};
  TradingSession room(config(r), f.time);
  f.seed(room);
  const auto& e = room.snapshot()->evaluation;
  EXPECT_EQ(open_loss_limit(e, room.config().rules, plan_inputs(*room.snapshot())), m("300"));
  // Without profit at the day's open, the day-profit basis takes the drawdown room.
  r.open_loss_cap->basis = OpenLossBasis::DayProfit;
  r.open_loss_cap->percent = 50;
  TradingSession profit(config(r), f.time);
  f.seed(profit);
  EXPECT_EQ(open_loss_limit(profit.snapshot()->evaluation, r, plan_inputs(*profit.snapshot())), m("500"));
  profit_day(profit, f, "600");
  next_day(profit, f, {2026, 9, 23});
  // $600 of start-of-day profit balance, half of it.
  EXPECT_EQ(open_loss_limit(profit.snapshot()->evaluation, r, plan_inputs(*profit.snapshot())), m("300"));
  r.max_drawdown = {};
  r.open_loss_cap->basis = OpenLossBasis::DayProfit;
  ScriptedMarket g;
  TradingSession none(config(r), g.time);
  g.seed(none);
  EXPECT_FALSE(open_loss_limit(none.snapshot()->evaluation, r, plan_inputs(*none.snapshot())));
}

TEST(PropRules, ProfitAboveTheBestPositionCapDoesNotCountTowardTheTarget) {
  ScriptedMarket f;
  AccountRules r;
  r.plan = "Best position";
  r.profit_target = m("1000");
  r.best_position_percent = 50;
  TradingSession s(config(r), f.time);
  f.seed(s);
  profit_day(s, f, "600");
  auto e = s.snapshot()->evaluation;
  EXPECT_EQ(e.position_excess, m("100"));
  ASSERT_TRUE(e.best_position);
  EXPECT_EQ(e.best_position->pnl, m("600"));
  profit_day(s, f, "450");
  e = s.snapshot()->evaluation;
  // $1,050 of profit, but only $950 counts.
  EXPECT_EQ(e.status, EvaluationStatus::Active);
  const auto objectives = evaluation_objectives(e, r, plan_inputs(*s.snapshot()));
  const auto* best = objective(objectives, Reason::BEST_POSITION);
  ASSERT_TRUE(best);
  EXPECT_FALSE(best->met);
  EXPECT_NE(best->message.find("$950.00 of the $1000.00 target counted"), std::string::npos) << best->message;
  EXPECT_TRUE(objective(objectives, Reason::PROFIT_TARGET)->met);
  // Marked profit on an open third trade counts in full, while it is within the cap.
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("third"), f.time).decision.ok());
  quote(s, f, "5.20", "5.40");
  EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Passed);
}

TEST(PropRules, ABalanceDrawdownBasisIgnoresOpenLossesUntilTheyClose) {
  for (const auto basis : {DrawdownBasis::Equity, DrawdownBasis::Balance}) {
    ScriptedMarket f;
    AccountRules r;
    r.plan = "Balance floor";
    r.max_drawdown = m("300");
    r.drawdown_basis = basis;
    TradingSession s(config(r), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open", 5), f.time).decision.ok());
    quote(s, f, "3.40", "3.60");
    if (basis == DrawdownBasis::Equity) {
      EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Failed);
      continue;
    }
    EXPECT_EQ(s.snapshot()->evaluation.status, EvaluationStatus::Active);
    ASSERT_TRUE(s.snapshot()->evaluation.day_close_balance);
    ASSERT_TRUE(s.submit(f.market("close", 5, Side::Sell), f.time).decision.ok());
    const auto& e = s.snapshot()->evaluation;
    EXPECT_EQ(e.status, EvaluationStatus::Failed);
    EXPECT_EQ(e.decision_code, Reason::DRAWDOWN_FLOOR);
    EXPECT_TRUE(e.decision.starts_with("The closed balance")) << e.decision;
  }
}

AccountRules reviewed() {
  AccountRules r;
  r.plan = "Funded review";
  r.phase = Phase::Funded;
  r.max_drawdown = m("1000");
  r.lock_balance = m("10000");
  r.payouts.qualifying_profit = m("50");
  r.payouts.qualifying_days = 1;
  r.payouts.withdrawal_percent = 50;
  r.payouts.split_percent = 80;
  r.payouts.minimum = m("10");
  r.payouts.caps = {m("60"), m("100")};
  r.payouts.review = PayoutReview{60, 0, false, {ReviewStep::Warning, ReviewStep::Forfeit, ReviewStep::Cap}, m("15")};
  return r;
}
/// Buy five at 4.20 and sell them a second later at 4.40: $93.50 net, held one second.
void quick_win(TradingSession& s, ScriptedMarket& f) {
  quote(s, f, "4.00", "4.20");
  ASSERT_TRUE(s.submit(f.market("quick-open-" + std::to_string(f.observation), 5), f.time).decision.ok());
  quote(s, f, "4.40", "4.60");
  ASSERT_TRUE(s.submit(f.market("quick-close-" + std::to_string(f.observation), 5, Side::Sell), f.time).decision.ok());
}

TEST(PropRules, PayoutReviewsWarnThenForfeitThenCapOffendingTrades) {
  ScriptedMarket f;
  JournalFile file;
  TradingSession s(config(reviewed()), f.time, FileJournal::create(file.path));
  f.seed(s);
  quick_win(s, f);
  next_day(s, f, {2026, 9, 23});
  auto q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.blocked.ok()) << q.blocked.message;
  ASSERT_TRUE(q.review);
  EXPECT_EQ(q.review->step, ReviewStep::Warning);
  ASSERT_EQ(q.review->trades.size(), 1U);
  EXPECT_EQ(q.review->trades[0].offences, std::vector<std::string>{"short_hold"});
  EXPECT_EQ(q.forfeit, Money{});
  EXPECT_EQ(q.maximum, m("46.75"));
  ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
  ASSERT_TRUE(s.snapshot()->evaluation.payouts.back().review);
  EXPECT_EQ(s.snapshot()->evaluation.payouts.back().review->step, ReviewStep::Warning);
  // The next review that finds an offending trade forfeits its profit with the payout.
  quick_win(s, f);
  next_day(s, f, {2026, 9, 24});
  q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.blocked.ok()) << q.blocked.message;
  ASSERT_TRUE(q.review);
  EXPECT_EQ(q.review->step, ReviewStep::Forfeit);
  EXPECT_EQ(q.forfeit, m("93.50"));
  EXPECT_EQ(q.profit, m("147"));
  EXPECT_EQ(q.withdrawable, m("26.75"));
  const auto cash = s.snapshot()->account.cash;
  ASSERT_TRUE(s.request_payout(m("20"), f.time).decision.ok());
  EXPECT_EQ(cash - s.snapshot()->account.cash, m("113.50"));
  EXPECT_EQ(s.snapshot()->evaluation.payouts.back().review->forfeited, m("93.50"));
  EXPECT_EQ(s.snapshot()->evaluation.payouts.back().trader_share, m("16"));
  // The third caps the payout as well.
  quick_win(s, f);
  next_day(s, f, {2026, 9, 25});
  q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.review);
  EXPECT_EQ(q.review->step, ReviewStep::Cap);
  EXPECT_EQ(q.maximum, m("15"));
  const auto payouts = events_of(file.path, "payout");
  ASSERT_EQ(payouts.size(), 2U);
  EXPECT_EQ(payouts[1].at("review").at("step"), "forfeit");
  EXPECT_EQ(payouts[1].at("review").at("forfeited"), m("93.50").micros());
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}

TEST(PropRules, ATradeHeldLongEnoughAndSmallEnoughIsNotReviewed) {
  ScriptedMarket f;
  auto rules = reviewed();
  rules.payouts.review = PayoutReview{1, 90, false, {ReviewStep::Forfeit}, {}};
  TradingSession s(config(rules), f.time);
  f.seed(s);
  quick_win(s, f);
  profit_day(s, f, "100");
  next_day(s, f, {2026, 9, 23});
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  // Held a second each, so not short; each under 90% of the $193.50 cycle.
  EXPECT_FALSE(q.review);
  EXPECT_EQ(q.forfeit, Money{});
}

TEST(PropRules, ATradeClosedAtAPayoutsTimestampIsReviewedOnce) {
  ScriptedMarket f;
  TradingSession s(config(reviewed()), f.time);
  f.seed(s);
  quick_win(s, f);
  next_day(s, f, {2026, 9, 23});
  // Closed and paid out at the same instant: this payout reviews it.
  quick_win(s, f);
  auto q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.review);
  ASSERT_EQ(q.review->trades.size(), 2U);
  ASSERT_TRUE(s.request_payout(m("40"), f.time).decision.ok());
  quick_win(s, f);
  next_day(s, f, {2026, 9, 24});
  q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.review);
  // Only the trade closed after the payout; the one closed with it is not reviewed again.
  ASSERT_EQ(q.review->trades.size(), 1U);
  EXPECT_GT(q.review->trades[0].closed, s.snapshot()->evaluation.payouts.back().time);
}

TEST(PropRules, PayoutReviewsReadShareRoundTrips) {
  ScriptedMarket f;
  TradingSession s(config(reviewed()), f.time);
  f.seed(s);
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("500")}).decision.ok());
  f.time += md::kNanosPerSecond;
  ASSERT_TRUE(s.trade_stock("SPY", -100, f.time, StockPrice{"SPY", f.time, m("501")}).decision.ok());
  next_day(s, f, {2026, 9, 23});
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.review);
  ASSERT_EQ(q.review->trades.size(), 1U);
  EXPECT_EQ(q.review->trades[0].id, "s1");
  EXPECT_EQ(q.review->trades[0].pnl, m("100"));
  EXPECT_EQ(q.review->trades[0].offences, std::vector<std::string>{"short_hold"});
}

AccountRules floor_after(PayoutFloor after) {
  auto r = reviewed();
  r.payouts.review.reset();
  r.lock_balance = {};
  r.payouts.after_payout = after;
  return r;
}

TEST(PropRules, AfterAPayoutTheFloorCanMoveUpToTheStartingBalance) {
  ScriptedMarket f;
  TradingSession s(config(floor_after(PayoutFloor::StartingBalance)), f.time);
  f.seed(s);
  profit_day(s, f, "300");
  next_day(s, f, {2026, 9, 23});
  EXPECT_LT(s.snapshot()->evaluation.floor, m("9400"));
  ASSERT_TRUE(s.request_payout(m("60"), f.time).decision.ok());
  const auto& e = s.snapshot()->evaluation;
  EXPECT_EQ(e.floor, m("10000"));
  EXPECT_TRUE(e.floor_locked);
  EXPECT_EQ(s.snapshot()->equity, m("10240"));
}

TEST(PropRules, AfterAPayoutTheAccountCanResetToItsStartingBalance) {
  ScriptedMarket f;
  JournalFile file;
  TradingSession s(config(floor_after(PayoutFloor::Reset)), f.time, FileJournal::create(file.path));
  f.seed(s);
  profit_day(s, f, "300");
  next_day(s, f, {2026, 9, 23});
  const auto q = payout_quote(*s.snapshot(), s.config().rules);
  ASSERT_TRUE(q.reset_balance);
  EXPECT_EQ(*q.reset_balance, m("10000"));
  ASSERT_TRUE(s.request_payout(m("60"), f.time).decision.ok());
  const auto& e = s.snapshot()->evaluation;
  EXPECT_EQ(s.snapshot()->equity, m("10000"));
  EXPECT_EQ(e.payouts.back().removed, m("240"));
  EXPECT_EQ(e.peak, m("10000"));
  EXPECT_EQ(e.floor, m("9000"));
  EXPECT_FALSE(e.floor_locked);
  EXPECT_EQ(e.payouts.back().trader_share, m("48"));
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), s.snapshot_json());
}

TEST(PropRules, ALosingStreakStartsTheCooldownAndFlagsTilt) {
  ScriptedMarket f;
  Guardrails g;
  g.loss_streak = 2;
  g.cooldown_minutes = 5;
  JournalFile file;
  TradingSession s(config({}, g), f.time, FileJournal::create(file.path));
  f.seed(s);
  for (int i = 0; i < 2; ++i) {
    quote(s, f, "4.00", "4.20");
    ASSERT_TRUE(s.submit(f.market("buy-" + std::to_string(i)), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("sell-" + std::to_string(i), 1, Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->guardrails.tilt.size(), static_cast<std::size_t>(i));
  }
  const auto& state = s.snapshot()->guardrails;
  ASSERT_EQ(state.tilt.size(), 1U);
  EXPECT_EQ(state.tilt[0].code, Reason::LOSS_STREAK);
  EXPECT_EQ(state.cooldown_until, f.time + 5 * md::kNanosPerMinute);
  EXPECT_EQ(s.submit(f.market("revenge"), f.time).decision.code, Reason::COOLDOWN);
  EXPECT_EQ(events_of(file.path, "tilt_flag").size(), 1U);
  // A winning close ends a streak before it reaches the limit.
  ScriptedMarket w;
  TradingSession other(config({}, g), w.time);
  w.seed(other);
  ASSERT_TRUE(other.submit(w.market("lose"), w.time).decision.ok());
  ASSERT_TRUE(other.submit(w.market("lose-close", 1, Side::Sell), w.time).decision.ok());
  ASSERT_TRUE(other.submit(w.market("win"), w.time).decision.ok());
  quote(other, w, "4.40", "4.60");
  ASSERT_TRUE(other.submit(w.market("win-close", 1, Side::Sell), w.time).decision.ok());
  ASSERT_TRUE(other.submit(w.market("lose-again"), w.time).decision.ok());
  quote(other, w, "4.00", "4.20");
  ASSERT_TRUE(other.submit(w.market("lose-again-close", 1, Side::Sell), w.time).decision.ok());
  EXPECT_TRUE(other.snapshot()->guardrails.tilt.empty());
  EXPECT_EQ(other.snapshot()->guardrails.losing_closes, 1);
}

TEST(PropRules, SizeCreepWarnsOrBlocksAgainstTheRollingMedian) {
  for (const bool block : {false, true}) {
    ScriptedMarket f;
    Guardrails g;
    g.size_creep_percent = 200;
    g.size_creep_block = block;
    TradingSession s(config({}, g), f.time);
    f.seed(s, "4.00", "4.20", 100);
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(s.submit(f.market("small-" + std::to_string(i)), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->guardrails.opening_sizes, (std::vector<Quantity>{1, 1, 1}));
    // Two is at the limit, twice the median; three is over it.
    const auto preview = s.preview(f.market("big-preview", 3), f.time);
    const auto warned = std::any_of(preview.warnings.begin(), preview.warnings.end(), [](const auto& w) { return w.code == "SIZE_CREEP"; });
    EXPECT_EQ(warned, !block);
    EXPECT_EQ(preview.decision.code, block ? Reason::SIZE_CREEP : Reason::NONE);
    const auto big = s.submit(f.market("big", 3), f.time);
    EXPECT_EQ(big.decision.code, block ? Reason::SIZE_CREEP : Reason::NONE);
    const auto& state = s.snapshot()->guardrails;
    ASSERT_EQ(state.tilt.size(), 1U);
    EXPECT_EQ(state.tilt[0].code, Reason::SIZE_CREEP);
    EXPECT_NE(state.tilt[0].message.find("3 contracts is more than 200%"), std::string::npos) << state.tilt[0].message;
    EXPECT_EQ(state.opening_sizes.size(), block ? 3U : 4U);
    // Reductions are never size creep.
    EXPECT_TRUE(s.submit(f.market("reduce", 3, Side::Sell), f.time).decision.ok());
    // The window rolls over to the next day; the day's flags do not.
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    quote(s, f, "4.00", "4.20");
    ASSERT_TRUE(s.roll_day(f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->guardrails.tilt.empty());
    EXPECT_EQ(s.snapshot()->guardrails.opening_sizes.size(), block ? 3U : 4U);
  }
}

TEST(PropRules, TighteningDisciplineGuardrailsTakesTheStricterTerms) {
  Guardrails current, requested;
  current.loss_streak = 4; current.cooldown_minutes = 10;
  requested.loss_streak = 2; requested.cooldown_minutes = 5;
  requested.size_creep_percent = 300; requested.size_creep_window = 10;
  const auto tight = tightened_guardrails(current, requested);
  EXPECT_EQ(tight.loss_streak, 2);
  EXPECT_EQ(tight.cooldown_minutes, 10);
  EXPECT_EQ(tight.size_creep_percent, 300);
  EXPECT_EQ(tight.size_creep_window, 10);
  EXPECT_FALSE(tight.size_creep_block);
  current.size_creep_percent = 200; current.size_creep_block = true;
  const auto both = tightened_guardrails(current, requested);
  EXPECT_EQ(both.size_creep_percent, 200);
  EXPECT_TRUE(both.size_creep_block);
}

TEST(PropRules, MinuteVolumeChecksRefuseWithoutAMinuteOfVolume) {
  EXPECT_TRUE(minute_volume_check("X", 5, 10, 50).ok());
  const auto over = minute_volume_check("X", 6, 10, 50);
  EXPECT_EQ(over.code, Reason::MINUTE_VOLUME);
  ASSERT_TRUE(over.limit);
  EXPECT_DOUBLE_EQ(*over.limit, 5);
  const auto unknown = minute_volume_check("X", 1, 10, std::nullopt);
  EXPECT_EQ(unknown.code, Reason::MINUTE_VOLUME);
  EXPECT_NE(unknown.message.find("cannot be checked"), std::string::npos);
  EXPECT_TRUE(minute_volume_check("X", 0, 10, std::nullopt).ok());
}

/// A recorded attempt that started at 10,000 and moved through two finished days and one in progress.
Evaluation recorded_attempt() {
  Evaluation e;
  e.started = md::new_york_to_utc({2026, 9, 21}, 9, 30);
  e.starting_balance = m("10000");
  const auto day = [](md::Date d, std::string_view open, std::string_view low, std::string_view high, std::string_view close,
                      std::string_view realised, int low_hour, int high_hour) {
    EvaluationDay r;
    r.day = d; r.open_equity = m(open); r.close_equity = m(close); r.realised = m(realised);
    r.low_equity = m(low); r.high_equity = m(high);
    r.low_at = md::new_york_to_utc(d, low_hour, 0); r.high_at = md::new_york_to_utc(d, high_hour, 0);
    return r;
  };
  e.days = {day({2026, 9, 21}, "10000", "9650", "10200", "10100", "100", 11, 14),
            day({2026, 9, 22}, "10100", "10050", "10800", "10700", "600", 10, 15)};
  e.day = {2026, 9, 23};
  e.day_open_equity = m("10700");
  e.day_open_realised = m("700");
  e.day_low_equity = m("10600");
  e.day_high_equity = m("10750");
  e.day_low_at = md::new_york_to_utc(e.day, 10, 0);
  e.day_high_at = md::new_york_to_utc(e.day, 11, 0);
  return e;
}

TEST(PropRules, ARulesReplayShowsWhereEachRuleSetPassesFailsOrLocks) {
  const auto e = recorded_attempt();
  PlanInputs now{m("10720"), m("10720"), m("700"), true, md::new_york_to_utc(e.day, 12, 0)};
  AccountRules tight;
  tight.max_drawdown = m("300");
  auto replay = replay_rules(e, now, {}, m("10000"), tight);
  EXPECT_EQ(replay.status, EvaluationStatus::Failed);
  EXPECT_EQ(replay.code, Reason::DRAWDOWN_FLOOR);
  EXPECT_EQ(replay.decided_day, (md::Date{2026, 9, 21}));
  EXPECT_EQ(replay.decided_at, e.days[0].low_at);
  ASSERT_EQ(replay.days.size(), 3U);
  EXPECT_EQ(replay.days[0].outcome, "failed");
  EXPECT_EQ(replay.days[1].outcome, "after");
  AccountRules lock;
  lock.max_drawdown = m("1000");
  lock.daily_loss_limit = m("300");
  replay = replay_rules(e, now, {}, m("10000"), lock);
  EXPECT_EQ(replay.status, EvaluationStatus::Active);
  EXPECT_EQ(replay.locked_days, std::vector<md::Date>{(md::Date{2026, 9, 21})});
  EXPECT_EQ(replay.days[0].outcome, "locked");
  // The lock closed the day at its level, so later days move from there.
  EXPECT_EQ(replay.days[0].close, m("9700"));
  EXPECT_EQ(replay.days[1].close, m("10300"));
  AccountRules target;
  target.profit_target = m("600");
  target.max_drawdown = m("1000");
  replay = replay_rules(e, now, {}, m("10000"), target);
  EXPECT_EQ(replay.status, EvaluationStatus::Passed);
  EXPECT_EQ(replay.decided_day, (md::Date{2026, 9, 22}));
  EXPECT_EQ(replay.decided_at, e.days[1].high_at);
  // A different starting balance moves by the same dollars.
  replay = replay_rules(e, now, {}, m("50000"), target);
  EXPECT_EQ(replay.decided_equity, m("50800"));
  // A minimum-trades objective counts trades from the day they closed.
  target.min_trades = 1;
  replay = replay_rules(e, now, {{"7", md::new_york_to_utc(e.day, 11, 30), m("50"), md::kNanosPerMinute}}, m("10000"), target);
  EXPECT_EQ(replay.status, EvaluationStatus::Passed);
  EXPECT_EQ(replay.decided_day, e.day);
}

TEST(PropRules, ARulesReplayAddsBackEverythingAPayoutTookOut) {
  auto e = recorded_attempt();
  // A $100 payout today forfeited $50 of profit and a reset removed $200 more:
  // the recorded equity since is $350 lower.
  Payout payout;
  payout.number = 1;
  payout.time = md::new_york_to_utc(e.day, 10, 30);
  payout.day = e.day;
  payout.amount = m("100");
  payout.review = PayoutReviewOutcome{};
  payout.review->forfeited = m("50");
  payout.removed = m("200");
  e.payouts = {payout};
  e.day_open_equity = m("10350");
  e.day_low_equity = m("10250");
  e.day_high_equity = m("10400");
  PlanInputs now{m("10370"), m("10370"), m("700"), true, md::new_york_to_utc(e.day, 12, 0)};
  AccountRules rules;
  rules.max_drawdown = m("1000");
  const auto replay = replay_rules(e, now, {}, m("10000"), rules);
  ASSERT_EQ(replay.days.size(), 3U);
  EXPECT_EQ(replay.days.back().close, m("10720"));
  EXPECT_EQ(replay.status, EvaluationStatus::Active);
}

TEST(PropRules, ChallengeEconomicsWeighsPassOddsCostsAndThePayout) {
  const auto result = challenge_economics({{"eval", 0.5, m("100"), m("50"), 1}}, 1.0, m("1000"));
  ASSERT_EQ(result.steps.size(), 1U);
  EXPECT_DOUBLE_EQ(result.steps[0].eventual_pass, 0.75);
  EXPECT_DOUBLE_EQ(result.steps[0].expected_cost, 125);
  EXPECT_DOUBLE_EQ(result.expected_value, 625);
  ASSERT_TRUE(result.break_even_pass);
  // 1000 (1 - q^2) = 100 + 50 q with q = 1 - p.
  const double q = (-50 + std::sqrt(2500 + 4 * 1000 * 900.0)) / 2000;
  EXPECT_NEAR(*result.break_even_pass, 1 - q, 1e-9);
  // Two steps: the second's pass odds and activation multiply in.
  const auto two = challenge_economics({{"eval", 0.5, m("100"), {}, 1}, {"funded", 1, m("200"), {}, 1}}, 0.5, m("1000"));
  EXPECT_DOUBLE_EQ(two.expected_value, 0.75 * (0.5 * 1000 - 200) - 100);
  EXPECT_FALSE(challenge_economics({{"eval", 0.9, m("1000"), {}, 1}}, 0.1, m("100")).break_even_pass);
  EXPECT_THROW(static_cast<void>(challenge_economics({}, 0.5, m("1"))), std::invalid_argument);
}

}  // namespace
}  // namespace openport::trading
