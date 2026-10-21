#include <gtest/gtest.h>

#include "openport/trading/session.hpp"
#include "support/recording.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view value) { return Money::parse(value); }
Timestamp at(md::Date day) { return md::new_york_to_utc(day, 10, 0); }
SessionConfig config() {
  SessionConfig c;
  c.limits.aggregate = {1e12, 1e12}; c.limits.per_underlying = {1e12, 1e12};
  return c;
}
TEST(TradingCashFlows, TransfersArePracticeOnlyRetryableAndNeverProfit) {
  const auto time = at({2026, 9, 22});
  test::RecordingFile file;
  auto sink = FileJournal::create((file.directory / "cash.jsonl").string());
  TradingSession s(config(), time, sink);
  ASSERT_TRUE(s.transfer_cash("deposit", m("500"), time).decision.ok());
  const auto version = s.snapshot()->account_version;
  ASSERT_TRUE(s.transfer_cash("deposit", m("500"), time).decision.ok());
  EXPECT_EQ(s.snapshot()->account_version, version);
  EXPECT_EQ(s.transfer_cash("deposit", m("501"), time).decision.code, Reason::DUPLICATE_CLIENT_ID);
  ASSERT_TRUE(s.transfer_cash("withdraw", m("-200"), time).decision.ok());
  const auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, m("100300"));
  EXPECT_EQ(snap->account.realised, Money{});
  EXPECT_EQ(snap->equity, snap->start_of_day_equity);
  EXPECT_EQ(snap->equity, snap->evaluation.starting_balance);
  EXPECT_DOUBLE_EQ(snap->attribution.total(), 0);
  EXPECT_EQ(snap->cash_flows.size(), 2U);
  EXPECT_EQ(s.transfer_cash("too-much", m("-100301"), time).decision.code, Reason::BUYING_POWER);
  EXPECT_THROW(s.transfer_cash("fraction", m("0.001"), time), TradingError);
  EXPECT_EQ(TradingSession::recover(FileJournal::read((file.directory / "cash.jsonl").string())).snapshot_json(), s.snapshot_json());
  auto evaluation = config(); evaluation.rules.max_drawdown = m("1000");
  TradingSession locked(evaluation, time);
  EXPECT_EQ(locked.transfer_cash("no", m("500"), time).decision.code, Reason::PLAN_LOCKED);
}
TEST(TradingCashFlows, CreditInterestIncludesCalendarDaysAndPreservesTheLossBase) {
  auto c = config(); c.rules.cash_credit_rate_bps = 365; c.rules.daily_loss_limit = m("1000");
  const auto friday = at({2026, 9, 25}), monday = at({2026, 9, 28});
  test::RecordingFile file;
  TradingSession s(c, friday, FileJournal::create((file.directory / "interest.jsonl").string()));
  ASSERT_TRUE(s.roll_day(monday).decision.ok());
  EXPECT_EQ(s.snapshot()->account.cash, m("100030"));
  EXPECT_EQ(s.snapshot()->account.realised, m("30"));
  EXPECT_EQ(s.snapshot()->start_of_day_equity, m("100000"));
  EXPECT_EQ(s.snapshot()->evaluation.day_open_equity, m("100000"));
  EXPECT_DOUBLE_EQ(s.snapshot()->attribution.other, 30);
  ASSERT_EQ(s.snapshot()->cash_flows.size(), 1U);
  EXPECT_EQ(s.snapshot()->cash_flows[0].days, 3);
  EXPECT_FALSE(s.roll_day(monday).decision.ok());
  EXPECT_EQ(s.snapshot()->cash_flows.size(), 1U);
  auto recovered = TradingSession::recover(FileJournal::read((file.directory / "interest.jsonl").string()));
  EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
  EXPECT_FALSE(recovered.roll_day(monday).decision.ok());
  EXPECT_EQ(recovered.snapshot()->account.cash, m("100030"));
}
TEST(TradingCashFlows, PortfolioDebitAndShortBorrowPostAsOtherProfitAndLoss) {
  const auto time = at({2026, 9, 22}), next = at({2026, 9, 23});
  auto c = config(); c.rules.margin = MarginMode::Portfolio; c.rules.buying_power = true;
  c.rules.margin_debit_rate_bps = 3650;
  TradingSession debit(c, time);
  ASSERT_TRUE(debit.trade_stock("SPY", 300, time, StockPrice{"SPY", time, m("500")}).decision.ok());
  ASSERT_EQ(debit.snapshot()->account.cash, m("-50000"));
  ASSERT_TRUE(debit.roll_day(next).decision.ok());
  EXPECT_EQ(debit.snapshot()->account.cash, m("-50050"));
  EXPECT_DOUBLE_EQ(debit.snapshot()->attribution.other, -50);
  EXPECT_EQ(debit.snapshot()->cash_flows[0].kind, "debit_interest");
  c.rules.stock_borrow_rate_bps = 3650;
  TradingSession borrow(c, time);
  ASSERT_TRUE(borrow.trade_stock("SPY", -100, time, StockPrice{"SPY", time, m("500")}).decision.ok());
  ASSERT_TRUE(borrow.roll_day(next).decision.ok());
  EXPECT_EQ(borrow.snapshot()->account.cash, m("149950"));
  EXPECT_DOUBLE_EQ(borrow.snapshot()->attribution.other, -50);
  EXPECT_EQ(borrow.snapshot()->cash_flows[0].kind, "borrow_fee");
  EXPECT_EQ(borrow.snapshot()->cash_flows[0].symbol, "SPY");
  c.rules.margin = MarginMode::Strategy; c.rules.buying_power = false;
  TradingSession strategy(c, time);
  ASSERT_TRUE(strategy.trade_stock("SPY", 300, time, StockPrice{"SPY", time, m("500")}).decision.ok());
  ASSERT_TRUE(strategy.roll_day(next).decision.ok());
  EXPECT_TRUE(strategy.snapshot()->cash_flows.empty());
}
}  // namespace
}  // namespace openport::trading
