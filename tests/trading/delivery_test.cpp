#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "openport/trading/history.hpp"
#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy(AccountRules rules = {}, std::string_view cash = "100000") {
  SessionConfig c;
  c.initial_cash = m(cash);
  c.limits.aggregate = {1e12, 1e12};
  c.limits.per_underlying = {1e12, 1e12};
  c.limits.max_daily_loss = m("1000000");
  c.rules = std::move(rules);
  return c;
}
/// SPY options, with SPY's own price in every batch.
struct Spy {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  double spot = 510;
  void define(TradingSession& s, const md::OptionContract& c) { ASSERT_TRUE(s.define(c, time).decision.ok()); }
  void quote(TradingSession& s, const md::OptionContract& c, std::string_view bid, std::string_view ask) {
    ++observation;
    s.on_quotes({{c.osi_symbol(), observation, time, m(bid), m(ask), 10, 10}},
                {{c.osi_symbol(), time, c.type == pricing::OptionType::Call ? 0.6 : -0.4, 0.02, 0.5, -0.05, spot, spot, 0.99,
                  md::years_between(time, c.expiry_time()), 0.2, true}},
                time, {{"SPY", time, Money::from_double(spot)}});
  }
  void price(TradingSession& s) { s.on_quotes({}, {}, time, {{"SPY", time, Money::from_double(spot)}}); }
  OrderRequest market(std::string client, const md::OptionContract& c, Quantity quantity, Side side = Side::Buy) const {
    return {std::move(client), c.osi_symbol(), side, OrderType::Market, TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  }
};
const MarkedStock* stock(const TradingSession& s, std::string_view symbol) {
  for (const auto& p : s.snapshot()->stocks) if (p.position.symbol == symbol) return &p;
  return nullptr;
}
double day_pnl(const TradingSession& s) { return (s.snapshot()->equity - s.snapshot()->start_of_day_equity).dollars(); }


TEST(TradingDelivery, ExpiryShortfallRefusesOpeningsAllowsClosesAndRecovers) {
  for (const bool assignment : {false, true}) {
    SCOPED_TRACE(assignment);
    const auto expiring = *md::parse_osi(assignment ? "SPY260922P00520000" : "SPY260922C00500000");
    const auto later = *md::parse_osi("SPY261022C00500000");
    Spy f;
    auto c = roomy();
    c.rules.buying_power = true;
    if (assignment) c.rules.max_drawdown = m("10000");  // Evaluation, alongside practice exercise.
    c.rules.fees = FeeSchedule{};
    c.rules.fees->exercise = m("0.000001");
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-shortfall-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    const auto path = (std::filesystem::path(pattern) / "session.jsonl").string();
    auto journal = FileJournal::create(path);
    TradingSession s(c, f.time, journal);
    f.define(s, expiring); f.define(s, later);
    f.quote(s, expiring, "10", "10.20");
    ASSERT_TRUE(s.submit(f.market("expiry", expiring, 3, assignment ? Side::Sell : Side::Buy), f.time).decision.ok());
    f.quote(s, later, "14", "14.20");
    ASSERT_TRUE(s.submit(f.market("later", later, 1), f.time).decision.ok());
    const auto warnings = s.warnings();
    const auto warning = std::find_if(warnings.begin(), warnings.end(), [](const auto& w) { return w.code == "EXPIRY_DELIVERY"; });
    ASSERT_NE(warning, warnings.end());
    EXPECT_EQ(warning->severity, "warning");
    EXPECT_NE(warning->message.find(assignment ? "$156000.00" : "$150000.00"), std::string::npos);
    f.time = expiring.expiry_time();
    ASSERT_TRUE(s.settle(expiring.osi_symbol(), m("510"), f.time).decision.ok());
    ASSERT_EQ(s.snapshot()->stocks.size(), 1U);
    EXPECT_EQ(s.snapshot()->stocks[0].position.shares, 300);
    EXPECT_LT(s.snapshot()->account.cash, Money{});
    const auto power = s.snapshot()->buying_power.available;
    EXPECT_EQ(power, m(assignment ? "-54420.000003" : "-54480.000003"));
    EXPECT_EQ(warning->actual, power.dollars());
    EXPECT_EQ(s.snapshot()->stock_fills.back().buying_power_after, power);
    f.quote(s, later, "14", "14.20");  // Rollover requires a fresh closing mark on the remaining option.
    auto recovered = TradingSession::recover(FileJournal::read(path, journal->head()));
    EXPECT_EQ(recovered.snapshot_json(), s.snapshot_json());
    EXPECT_EQ(recovered.snapshot()->stock_fills.back().buying_power_after, power);
    // Delivery after the close is resolved when the share market next opens.
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    ASSERT_TRUE(recovered.roll_day(f.time).decision.ok());
    f.quote(recovered, later, "14", "14.20");
    EXPECT_EQ(recovered.submit(f.market("blocked", later, 1), f.time).decision.code, Reason::BUYING_POWER);
    EXPECT_EQ(recovered.trade_stock("SPY", 1, f.time).decision.code, Reason::BUYING_POWER);
    ASSERT_TRUE(recovered.submit(f.market("close-option", later, 1, Side::Sell), f.time).decision.ok());
    ASSERT_TRUE(recovered.trade_stock("SPY", -1, f.time).decision.ok());
    EXPECT_LT(recovered.snapshot()->buying_power.available, Money{});
    ASSERT_TRUE(recovered.trade_stock("SPY", -299, f.time).decision.ok());
    EXPECT_GT(recovered.snapshot()->buying_power.available, Money{});
    ASSERT_TRUE(recovered.submit(f.market("resolved", later, 1), f.time).decision.ok());
    std::filesystem::remove_all(pattern);
  }
}

TEST(TradingDelivery, BuyOnlyPutExerciseRequiresLongSharesButExpiryStillDelivers) {
  const auto put = *md::parse_osi("SPY260922P00520000");
  for (const auto shares : {0, 99, 100, 150}) {
    SCOPED_TRACE(shares);
    Spy f; AccountRules rules; rules.buy_only = true;
    TradingSession s(roomy(rules), f.time);
    f.define(s, put); f.quote(s, put, "10", "10.20");
    ASSERT_TRUE(s.submit(f.market("put", put, 1), f.time).decision.ok());
    if (shares > 0) { ASSERT_TRUE(s.trade_stock("SPY", shares, f.time).decision.ok()); }
    const auto result = s.exercise(put.osi_symbol(), 1, f.time);
    if (shares < 100) {
      EXPECT_EQ(result.decision.code, Reason::BUY_ONLY);
      EXPECT_NE(result.decision.message.find("exercise would sell shares short"), std::string::npos);
      EXPECT_EQ(s.snapshot()->positions[0].position.quantity, 1);
      f.time = put.expiry_time();
      ASSERT_TRUE(s.settle(put.osi_symbol(), m("510"), f.time).decision.ok());
      ASSERT_NE(stock(s, "SPY"), nullptr);
      EXPECT_EQ(stock(s, "SPY")->position.shares, shares - 100);
    } else {
      ASSERT_TRUE(result.decision.ok()) << result.decision.message;
      EXPECT_TRUE(s.snapshot()->positions.empty());
      if (shares == 100) { EXPECT_TRUE(s.snapshot()->stocks.empty()); }
      else { ASSERT_NE(stock(s, "SPY"), nullptr); EXPECT_EQ(stock(s, "SPY")->position.shares, shares - 100); }
    }
  }
}

TEST(TradingDelivery, BuyOnlyPutExerciseRechecksSharesAfterASale) {
  const auto put = *md::parse_osi("SPY261022P00520000");
  Spy f; AccountRules rules; rules.buy_only = true;
  TradingSession s(roomy(rules), f.time);
  f.define(s, put); f.quote(s, put, "10", "10.20");
  ASSERT_TRUE(s.submit(f.market("put", put, 1), f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", -1, f.time).decision.ok());
  EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::BUY_ONLY);
}

TEST(TradingDelivery, KillAllowsShareClosesAndFlattenButRejectsExercise) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  for (const bool daily : {false, true}) {
    Spy f;
    auto c = roomy();
    c.limits.max_daily_loss = m("100");
    TradingSession s(c, f.time);
    f.define(s, call);
    f.quote(s, call, "10.00", "10.20");
    ASSERT_TRUE(s.submit(f.market("open", call, 2), f.time).decision.ok());
    ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
    if (daily) {
      f.spot = 500;
      f.quote(s, call, "1", "1.20");
    } else {
      s.trip_kill("manual", f.time);
    }
    ASSERT_TRUE(s.snapshot()->risk.kill_latched);
    EXPECT_EQ(s.exercise(call.osi_symbol(), 1, f.time).decision.code, Reason::KILL_SWITCH);
    ASSERT_TRUE(s.trade_stock("SPY", -50, f.time).decision.ok());
    ASSERT_NE(stock(s, "SPY"), nullptr);
    EXPECT_EQ(stock(s, "SPY")->position.shares, 50);
    ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->stocks.empty());
    EXPECT_TRUE(s.snapshot()->positions.empty());
  }
}

TEST(TradingDelivery, SharesCoverCallsWrittenAgainstThem) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto higher = *md::parse_osi("SPY261022C00520000");
  AccountRules rules;
  rules.buying_power = true;
  Spy f;
  TradingSession s(roomy(rules, "52000"), f.time);
  f.define(s, call);
  f.define(s, higher);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, higher, "3.00", "3.20");
  ASSERT_TRUE(s.submit(f.market("open", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  ASSERT_EQ(stock(s, "SPY")->position.shares, 100);
  EXPECT_EQ(s.snapshot()->account.cash, m("979.35"));
  // The shares cover a call written against them: it holds nothing, and its premium is free.
  ASSERT_TRUE(s.submit(f.market("covered", higher, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
  EXPECT_EQ(s.snapshot()->buying_power.available, m("1278.70"));
  // A second call has no shares behind it: naked, it needs about 9,200.
  EXPECT_EQ(s.submit(f.market("naked", higher, 1, Side::Sell), f.time).decision.code, Reason::BUYING_POWER);
  // Selling the shares leaves the call naked, but frees far more cash than it holds.
  ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, s.snapshot()->positions.front().market_value.value_or(Money{}) * -1 +
            naked_requirement(higher, f.spot));
}

TEST(TradingDelivery, CashAndIraKeepSharesBehindHeldAndWorkingCalls) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto higher = *md::parse_osi("SPY261022C00520000");
  const auto put = *md::parse_osi("SPY261022P00520000");
  for (const auto type : {AccountType::Cash, AccountType::Ira}) {
    AccountRules rules;
    rules.account_type = type;
    rules.buying_power = true;
    Spy f;
    TradingSession s(roomy(rules), f.time);
    for (const auto& c : {call, higher, put}) f.define(s, c);
    f.quote(s, call, "10.00", "10.20");
    f.quote(s, higher, "3.00", "3.20");
    f.quote(s, put, "10.00", "10.20");
    EXPECT_EQ(s.trade_stock("SPY", -1, f.time).decision.code, Reason::ACCOUNT_TYPE);
    EXPECT_EQ(s.submit(f.market("naked", higher, 1, Side::Sell), f.time).decision.code, Reason::ACCOUNT_TYPE);
    ASSERT_TRUE(s.submit(f.market("open", call, 1), f.time).decision.ok());
    ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("put", put, 1), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("covered", higher, 1, Side::Sell), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
    EXPECT_EQ(s.trade_stock("SPY", -1, f.time).decision.code, Reason::ACCOUNT_TYPE);
    EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::ACCOUNT_TYPE);
    ASSERT_TRUE(s.submit(f.market("cover", higher, 1), f.time).decision.ok());
    auto resting = f.market("resting", higher, 1, Side::Sell);
    resting.type = OrderType::Limit;
    resting.tif = TimeInForce::Day;
    resting.limit_price = m("3.50");
    const auto order = s.submit(resting, f.time);
    ASSERT_TRUE(order.decision.ok()) << order.decision.message;
    EXPECT_EQ(s.trade_stock("SPY", -100, f.time).decision.code, Reason::ACCOUNT_TYPE);
    EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::ACCOUNT_TYPE);
    ASSERT_TRUE(s.cancel(*order.order_id, f.time).decision.ok());
    ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
    EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::ACCOUNT_TYPE);
  }
}

TEST(TradingDelivery, IraExerciseReplacesALongCallsCoverWithShares) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto higher = *md::parse_osi("SPY261022C00520000");
  AccountRules rules;
  rules.account_type = AccountType::Ira;
  rules.buying_power = true;
  Spy f;
  TradingSession s(roomy(rules), f.time);
  for (const auto& c : {call, higher}) f.define(s, c);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, higher, "3.00", "3.20");
  ASSERT_TRUE(s.submit(f.market("long", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short", higher, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  ASSERT_NE(stock(s, "SPY"), nullptr);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 100);
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
}

TEST(TradingDelivery, CashAndIraCannotIncreaseAnOddLotShortLeftByDelivery) {
  const auto put = *md::parse_osi("SPY260922P00520000");
  for (const auto type : {AccountType::Cash, AccountType::Ira}) {
    Spy f;
    AccountRules rules;
    rules.account_type = type;
    rules.buying_power = true;
    TradingSession s(roomy(rules, "1000000"), f.time);
    f.define(s, put);
    f.quote(s, put, "10.00", "10.20");
    ASSERT_TRUE(s.submit(f.market("long put", put, 1), f.time).decision.ok());
    f.time = put.expiry_time();
    ASSERT_TRUE(s.settle(put.osi_symbol(), m("510"), f.time).decision.ok());
    ASSERT_EQ(stock(s, "SPY")->position.shares, -100);
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    f.price(s);
    ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
    ASSERT_EQ(stock(s, "SPY")->position.shares, -99);
    // A whole-lot count alone cannot distinguish 99 from 100 short shares.
    EXPECT_EQ(s.trade_stock("SPY", -1, f.time).decision.code, Reason::ACCOUNT_TYPE);
    EXPECT_EQ(stock(s, "SPY")->position.shares, -99);
    ASSERT_TRUE(s.trade_stock("SPY", 99, f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->stocks.empty());
  }
}

TEST(TradingDelivery, AccountTypeRechecksSingleAndComboCallsAfterExpiryRemovesTheirShares) {
  const auto put = *md::parse_osi("SPY260922P00520000");
  const auto call = *md::parse_osi("SPY261022C00520000");
  const auto hedge = *md::parse_osi("SPY261022P00520000");
  for (const auto type : {AccountType::Cash, AccountType::Ira}) {
    for (const bool combo : {false, true}) {
      Spy f;
      AccountRules rules;
      rules.account_type = type;
      rules.buying_power = true;
      TradingSession s(roomy(rules, "1000000"), f.time);
      for (const auto& c : {put, call, hedge}) { f.define(s, c); f.quote(s, c, "10.00", "10.20"); }
      ASSERT_TRUE(s.trade_stock("SPY", 100, f.time).decision.ok());
      ASSERT_TRUE(s.submit(f.market("delivery", put, 1), f.time).decision.ok());
      auto sale = f.market("covered call", call, 1, Side::Sell);
      sale.type = OrderType::Limit;
      sale.tif = TimeInForce::Gtc;
      sale.limit_price = m("11.00");
      if (combo) {
        sale.symbol.clear();
        sale.side = Side::Buy;
        sale.legs = {{call.osi_symbol(), Side::Sell, 1}, {hedge.osi_symbol(), Side::Buy, 1}};
        sale.limit_price = m("-0.80");
      }
      const auto resting = s.submit(sale, f.time);
      ASSERT_TRUE(resting.decision.ok()) << resting.decision.message;
      ASSERT_EQ(s.snapshot()->open_orders.size(), 1U);
      f.time = put.expiry_time();
      ASSERT_TRUE(s.settle(put.osi_symbol(), m("510"), f.time).decision.ok());
      EXPECT_TRUE(s.snapshot()->stocks.empty());
      f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
      f.quote(s, hedge, "10.00", "10.20");
      f.quote(s, call, "11.00", "11.20");
      EXPECT_TRUE(s.snapshot()->open_orders.empty());
      const auto& cancelled = s.snapshot()->recent_orders.at(*resting.order_id - 1);
      EXPECT_EQ(cancelled.reason.code, Reason::RISK_CHANGED);
      EXPECT_NE(cancelled.reason.message.find("ACCOUNT_TYPE"), std::string::npos);
      EXPECT_EQ(cancelled.filled_quantity, 0);
    }
  }
}

TEST(TradingDelivery, SellingSharesThatCoverACallNeedsWhatTheNakedCallHolds) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto deep = *md::parse_osi("SPY261022C00100000");
  const auto spend = *md::parse_osi("SPY261022P00550000");  // A put, which covers no call.
  AccountRules rules;
  rules.buying_power = true;
  Spy f;
  TradingSession s(roomy(rules, "52000"), f.time);
  for (const auto& c : {call, deep, spend}) f.define(s, c);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, deep, "409.90", "410.10");
  f.quote(s, spend, "41.70", "41.90");
  ASSERT_TRUE(s.submit(f.market("open", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("covered", deep, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("spend", spend, 10), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.available, m("62.20"));
  // Naked, the deep call would hold 41,000 of value and 10,200 of requirement:
  // more than the 51,000 the shares bring.
  EXPECT_EQ(s.trade_stock("SPY", -100, f.time).decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 100);
  ASSERT_TRUE(s.submit(f.market("raise", spend, 10, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
}

TEST(TradingDelivery, PortfolioMarginScansDeliveredSharesAndReleasesItOnClose) {
  const auto call = *md::parse_osi("SPY260922C00500000");
  for (const auto side : {Side::Buy, Side::Sell}) {
    AccountRules rules;
    rules.margin = MarginMode::Portfolio;
    rules.buying_power = true;
    Spy f;
    TradingSession s(roomy(rules), f.time);
    f.define(s, call);
    f.quote(s, call, "10.00", "10.20");
    ASSERT_TRUE(s.submit(f.market("open", call, 1, side), f.time).decision.ok());
    f.time = call.expiry_time();
    f.price(s);
    ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("7650"));  // 15% of 100 shares at 510
    // The shares' value counts: long shares are collateral, short ones are owed.
    EXPECT_EQ(s.snapshot()->buying_power.available, s.snapshot()->equity - m("7650"));
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    f.price(s);
    ASSERT_TRUE(s.trade_stock("SPY", side == Side::Buy ? -100 : 100, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
  }
}

TEST(TradingDelivery, ExpiryExercisesAndAssignsEquityOptionsACentInTheMoney) {
  const auto call = *md::parse_osi("SPY260922C00500000");  // expires today at 16:15
  const auto put = *md::parse_osi("SPY260922P00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, put, "0.10", "0.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("puts", put, 1), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->account.cash, m("97938.05"));  // 2040 + 20 of premium, 1.95 of fees
  f.time = call.expiry_time();
  s.on_quotes({}, {}, f.time);
  // The calls are exercised: settled at 10.00, they buy 200 shares at 510.00.
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  const auto* shares = stock(s, "SPY");
  ASSERT_NE(shares, nullptr);
  EXPECT_EQ(shares->position.shares, 200);
  EXPECT_EQ(shares->position.basis, m("102000"));
  EXPECT_EQ(s.snapshot()->account.cash, m("97938.05") + m("2000") - m("102000"));
  // Together they paid the strike: 500.00 a share.
  EXPECT_EQ(m("2000") - m("102000"), m("-100000"));
  // The put, out of the money, expires worthless.
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("510"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, 200);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_NEAR(s.snapshot()->attribution.total(), day_pnl(s), 1e-6);
  // The option trades close at their settlement; the shares stay out of the trade history.
  const auto trades = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  ASSERT_EQ(trades.size(), 2U);
  for (const auto& t : trades) EXPECT_EQ(t.closure, ClosureKind::Settlement);
}

TEST(TradingDelivery, ShortsAreAssignedAndShortSharesHoldBuyingPower) {
  const auto call = *md::parse_osi("SPY260922C00500000");
  const auto put = *md::parse_osi("SPY260922P00500000");
  AccountRules rules;
  rules.buying_power = true;
  Spy f;
  TradingSession s(roomy(rules), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, put, "0.10", "0.20");
  ASSERT_TRUE(s.submit(f.market("short-call", call, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short-put", put, 2, Side::Sell), f.time).decision.ok());
  f.time = call.expiry_time();
  s.on_quotes({}, {}, f.time);
  // Short a call at 510: assigned, it sells 100 shares at 510.
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -100);
  // Half a cent in the money is not exercised.
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("499.995"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -100);
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, m("76500"));  // 150% of 51,000
}

TEST(TradingDelivery, ShortPutsAssignedBuySharesAndIndexOptionsStayCash) {
  const auto put = *md::parse_osi("SPY260922P00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  f.define(s, put);
  f.quote(s, put, "0.10", "0.20");
  ASSERT_TRUE(s.submit(f.market("short-put", put, 3, Side::Sell), f.time).decision.ok());
  test::ScriptedMarket index;  // SPXW, European and cash-settled
  index.contract.expiry = {2026, 9, 22};
  index.seed(s);
  ASSERT_TRUE(s.submit(index.market("spx"), index.time).decision.ok());
  f.time = put.expiry_time();
  s.on_quotes({}, {}, f.time);
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("490"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, 300);
  EXPECT_EQ(stock(s, "SPY")->position.basis, m("147000"));
  ASSERT_TRUE(s.settle(index.symbol(), m("5100"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPX"), nullptr);
  EXPECT_EQ(s.snapshot()->stocks.size(), 1U);
}

TEST(TradingDelivery, AClosingPrintTakesRevisions) {
  Spy f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.record_close("SPY", {2026, 9, 22}, m("500.40"), f.time, f.time).decision.ok());
  ASSERT_TRUE(s.record_close("SPY", {2026, 9, 22}, m("499.95"), f.time + 1, f.time).decision.ok());
  EXPECT_EQ(s.closing_print("SPY", {2026, 9, 22})->price, m("499.95"));
  EXPECT_EQ(s.closing_print("SPY", {2026, 9, 22})->time, f.time + 1);
  EXPECT_FALSE(s.closing_print("SPY", {2026, 9, 23}));
}

TEST(TradingDelivery, ADefinedRiskPlanKeepsTheLongThatCoversAShort) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto later = *md::parse_osi("SPY261120C00500000");
  AccountRules rules;
  rules.defined_risk = true;
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(rules), f.time);
  f.define(s, call);
  f.define(s, later);
  f.quote(s, call, "21.00", "21.20");
  f.quote(s, later, "23.00", "23.20");
  ASSERT_TRUE(s.submit(f.market("long", later, 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short", call, 1, Side::Sell), f.time).decision.ok());
  const auto refused = s.exercise(later.osi_symbol(), 1, f.time).decision;
  EXPECT_EQ(refused.code, Reason::DEFINED_RISK);
  EXPECT_NE(refused.message.find("Exercising"), std::string::npos) << refused.message;
  ASSERT_TRUE(s.submit(f.market("close short", call, 1), f.time).decision.ok());
  EXPECT_TRUE(s.exercise(later.osi_symbol(), 1, f.time).decision.ok());
}

TEST(TradingDelivery, EarlyExerciseGivesUpTimeValueAndKeepsTheRestOpen) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 3), f.time).decision.ok());
  const auto cash = s.snapshot()->account.cash;
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 2, f.time).decision.ok());
  // Two contracts close at 20.00 intrinsic and buy 200 shares at 520.00.
  EXPECT_EQ(s.snapshot()->account.cash, cash + m("4000") - m("104000"));
  EXPECT_EQ(s.snapshot()->positions.at(0).position.quantity, 1);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 200);
  // Exercising gave up 1.10 of time value a share against the 21.10 mark.
  const auto a = s.snapshot()->attribution;
  EXPECT_NEAR(a.costs, 3 * 100 * -0.10 - 1.95 + 2 * 100 * (20.00 - 21.10), 1e-6);
  EXPECT_NEAR(a.total(), day_pnl(s), 1e-6);
  const auto trades = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  ASSERT_EQ(trades.size(), 1U);
  EXPECT_FALSE(trades[0].closed);
  EXPECT_EQ(trades[0].quantity, 1);
  EXPECT_EQ(trades[0].closed_contracts, 2);
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  const auto closed = lifecycles(s.snapshot()->recent_fills, s.snapshot()->closures, s.contracts());
  EXPECT_TRUE(closed[0].closed);
  EXPECT_EQ(closed[0].closure, ClosureKind::Exercise);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 300);
}

TEST(TradingDelivery, ExerciseNeedsALongInTheMoneyEquityOptionAndItsPrice) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  const auto put = *md::parse_osi("SPY261022P00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "21.00", "21.20");
  f.quote(s, put, "1.00", "1.20");
  ASSERT_TRUE(s.submit(f.market("put", put, 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("short-call", call, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::INVALID_ORDER);   // out of the money
  EXPECT_EQ(s.exercise(put.osi_symbol(), 2, f.time).decision.code, Reason::INVALID_ORDER);   // more than held
  EXPECT_EQ(s.exercise(call.osi_symbol(), 1, f.time).decision.code, Reason::INVALID_ORDER);  // short
  EXPECT_THROW((void)s.exercise(put.osi_symbol(), 0, f.time), TradingError);
  test::ScriptedMarket index;
  index.seed(s);
  ASSERT_TRUE(s.submit(index.market("spx"), index.time).decision.ok());
  EXPECT_EQ(s.exercise(index.symbol(), 1, f.time).decision.code, Reason::INVALID_ORDER);    // cash-settled
  // Without a fresh price for SPY there is nothing to exercise against.
  f.time += 5 * md::kNanosPerMinute;
  f.spot = 480;
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(s.exercise(put.osi_symbol(), 1, f.time).decision.code, Reason::STALE_QUOTE);
  f.price(s);
  EXPECT_TRUE(s.exercise(put.osi_symbol(), 1, f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -100);
}

TEST(TradingDelivery, ExerciseWithinBuyingPower) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  AccountRules rules;
  rules.buying_power = true;
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(rules, "10000"), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("call", call, 1), f.time).decision.ok());
  EXPECT_EQ(s.exercise(call.osi_symbol(), 1, f.time).decision.code, Reason::BUYING_POWER);
  EXPECT_EQ(s.snapshot()->positions.size(), 1U);
  EXPECT_TRUE(s.snapshot()->stocks.empty());
}

TEST(TradingDelivery, SharesAreMarkedRiskedAndClosedAtTheUnderlyingsPrice) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-delivery-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  std::string expected;
  {
    TradingSession s(roomy(), f.time, FileJournal::create(path));
    f.define(s, call);
    f.quote(s, call, "21.00", "21.20");
    ASSERT_TRUE(s.submit(f.market("calls", call, 3), f.time).decision.ok());
    ASSERT_TRUE(s.exercise(call.osi_symbol(), 3, f.time).decision.ok());
    f.time += md::kNanosPerSecond;
    f.spot = 525;
    f.price(s);
    const auto* shares = stock(s, "SPY");
    ASSERT_NE(shares, nullptr);
    EXPECT_TRUE(shares->fresh);
    EXPECT_EQ(shares->mark, m("525"));
    EXPECT_EQ(shares->market_value, m("157500"));
    EXPECT_EQ(shares->unrealised, m("1500"));
    EXPECT_TRUE(s.snapshot()->valuation_complete);
    EXPECT_NEAR(s.snapshot()->attributions.at("SPY").delta, 300 * 5.0, 1e-9);
    EXPECT_DOUBLE_EQ(s.snapshot()->risk.underlyings.at("SPY").position.dollar_delta, 300 * 525.0);
    for (const auto& cell : s.snapshot()->scenarios.cells) {
      if (cell.spot_percent == 1) { EXPECT_NEAR(cell.pnl, 300 * 525.0 * 0.01, 1e-6); }
    }
    // The legacy close path stays reduce-only.
    EXPECT_EQ(s.trade_stock("SPY", 100, f.time, {}, true).decision.code, Reason::INVALID_ORDER);
    EXPECT_EQ(s.trade_stock("SPY", -400, f.time, {}, true).decision.code, Reason::INVALID_ORDER);
    EXPECT_EQ(s.trade_stock("QQQ", -1, f.time, {}, true).decision.code, Reason::INVALID_ORDER);
    ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
    EXPECT_EQ(stock(s, "SPY")->position.shares, 200);
    EXPECT_EQ(stock(s, "SPY")->position.realised, m("500"));
    // Flattening closes the rest at the price.
    ASSERT_TRUE(s.close_positions(std::nullopt, f.time).decision.ok());
    EXPECT_TRUE(s.snapshot()->stocks.empty());
    EXPECT_NEAR(s.snapshot()->attribution.total(), day_pnl(s), 1e-6);
    // The history has one round trip in the shares: opened by the exercise, closed by the two sales.
    const auto& fills = s.snapshot()->stock_fills;
    ASSERT_EQ(fills.size(), 3U);
    EXPECT_EQ(fills[0].source, StockSource::Exercise);
    EXPECT_EQ(fills[0].option, call.osi_symbol());
    EXPECT_EQ(fills[0].shares, 300);
    EXPECT_EQ(fills[1].source, StockSource::Trade);
    EXPECT_EQ(fills[2].source, StockSource::Trade);
    EXPECT_EQ(fills[2].shares, -200);
    const auto trips = share_lifecycles(fills);
    ASSERT_EQ(trips.size(), 1U);
    EXPECT_EQ(trips[0].symbol, "SPY");
    EXPECT_EQ(trips[0].direction, 1);
    EXPECT_EQ(trips[0].closed, f.time);
    EXPECT_EQ(trips[0].max_shares, 300);
    EXPECT_EQ(trips[0].gross, m("1500"));  // 300 bought at 520 by the exercise, sold at 525
    EXPECT_EQ(trips[0].fills, (std::vector<std::uint64_t>{1, 2, 3}));
    expected = s.snapshot_json();
  }
  EXPECT_EQ(TradingSession::recover(FileJournal::read(path)).snapshot_json(), expected);
  std::filesystem::remove_all(directory);
}

TEST(TradingDelivery, ShareRoundTripsReverseAndCloseAtAResetsMark) {
  const auto t = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const SharedVector<StockFill> fills{
      {1, "SPY", 100, m("500"), t, StockSource::Delivery, "SPY   260922P00500000"},
      {2, "SPY", -300, m("503"), t + 1, StockSource::Delivery, "SPY   260922C00503000"},
      {3, "QQQ", -100, m("400"), t + 2, StockSource::Delivery, "QQQ   260922C00400000"},
      {4, "SPY", 200, m("501.50"), t + 3, StockSource::Trade, ""}};
  const auto trips = share_lifecycles(fills);
  ASSERT_EQ(trips.size(), 3U);
  // Long 100, then short 300 against them: the round trip closes at 503 and a short of 200 opens there.
  EXPECT_EQ(trips[0].direction, 1);
  EXPECT_EQ(trips[0].closed, t + 1);
  EXPECT_EQ(trips[0].gross, m("300"));
  EXPECT_EQ(trips[0].fills, (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(trips[1].symbol, "SPY");
  EXPECT_EQ(trips[1].direction, -1);
  EXPECT_EQ(trips[1].opened, t + 1);
  EXPECT_EQ(trips[1].opened_shares, 200);
  EXPECT_EQ(trips[1].closed, t + 3);
  EXPECT_EQ(trips[1].gross, m("300"));  // 200 short at 503, bought back at 501.50
  EXPECT_EQ(trips[1].fills, (std::vector<std::uint64_t>{2, 4}));
  EXPECT_EQ(trips[2].symbol, "QQQ");
  EXPECT_FALSE(trips[2].closed);
  EXPECT_EQ(trips[2].shares, -100);

  // A reset drops held shares from the ledger at their mark, which closes the round trip.
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  f.time += md::kNanosPerSecond;
  f.spot = 522;
  f.price(s);
  ASSERT_TRUE(s.reset_account(m("100000"), {}, "fresh start", f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->stocks.empty());
  const auto& after = s.snapshot()->stock_fills;
  ASSERT_EQ(after.size(), 2U);
  EXPECT_EQ(after[1].source, StockSource::Reset);
  EXPECT_EQ(after[1].shares, -100);
  EXPECT_EQ(after[1].price, m("522"));
  const auto reset = share_lifecycles(after);
  ASSERT_EQ(reset.size(), 1U);
  EXPECT_EQ(reset[0].closed, f.time);
  EXPECT_EQ(reset[0].gross, m("200"));
}

/// Ten each of a deep put and a call the market values below their exercise, a put
/// with time value left and a long, rolled into the next day.
std::unique_ptr<TradingSession> assigned_overnight(AccountRules rules = {}) {
  const auto deep = *md::parse_osi("SPY261022P00600000");  // 90 in the money at 510
  const auto near = *md::parse_osi("SPY261022P00515000");  // 5 in the money, with time value left
  const auto call = *md::parse_osi("SPY261022C00450000");  // 60 in the money
  const auto bought = *md::parse_osi("SPY261022P00590000");  // longs are never assigned
  Spy f;
  auto s = std::make_unique<TradingSession>(roomy(rules), f.time);
  for (const auto& c : {deep, near, call, bought}) f.define(*s, c);
  f.quote(*s, deep, "89.80", "90.20");
  f.quote(*s, near, "7.00", "7.20");
  f.quote(*s, call, "60.00", "60.40");
  f.quote(*s, bought, "79.80", "80.20");
  EXPECT_TRUE(s->submit(f.market("deep", deep, 10, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s->submit(f.market("near", near, 10, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s->submit(f.market("call", call, 10, Side::Sell), f.time).decision.ok());
  EXPECT_TRUE(s->submit(f.market("bought", bought, 1), f.time).decision.ok());
  // Into the close the deep put and the call trade below their exercise value, and so does the long put.
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 14, 30);
  f.quote(*s, deep, "89.60", "89.90");
  f.quote(*s, near, "7.00", "7.20");
  f.quote(*s, call, "59.70", "59.90");
  f.quote(*s, bought, "79.60", "79.90");
  EXPECT_EQ(s->snapshot()->positions.size(), 4U);
  const auto night = md::new_york_to_utc({2026, 9, 22}, 17, 30);
  s->on_quotes({}, {}, night);
  EXPECT_TRUE(s->roll_day(night).decision.ok());
  return s;
}

TEST(TradingDelivery, ShortsTheMarketValuesBelowTheirExerciseArePartlyAssignedOvernight) {
  const auto deep = md::parse_osi("SPY261022P00600000")->osi_symbol();
  const auto call = md::parse_osi("SPY261022C00450000")->osi_symbol();
  const auto s = assigned_overnight();
  const auto snap = s->snapshot();
  // Each contract a holder would exercise is assigned with even odds: some of each.
  std::map<std::string, Quantity> assigned;
  for (const auto& c : snap->closures) {
    EXPECT_EQ(c.kind, ClosureKind::Assignment);
    assigned[c.symbol] -= c.quantity;
  }
  ASSERT_EQ(assigned.size(), 2U);
  const auto puts = assigned[deep], calls = assigned[call];
  EXPECT_GT(puts, 0);
  EXPECT_LT(puts, 10);
  EXPECT_GT(calls, 0);
  EXPECT_LT(calls, 10);
  // The rest stay open, beside the put with time value and the long.
  std::map<std::string, Quantity> held;
  for (const auto& p : snap->positions) held[p.position.contract.osi_symbol()] = p.position.quantity;
  EXPECT_EQ(held[deep], puts - 10);
  EXPECT_EQ(held[call], calls - 10);
  EXPECT_EQ(held[md::parse_osi("SPY261022P00515000")->osi_symbol()], -10);
  EXPECT_EQ(held[md::parse_osi("SPY261022P00590000")->osi_symbol()], 1);
  // Assigned puts buy 100 shares each at 510 and calls sell 100: each pair costs its strike.
  EXPECT_EQ(stock(*s, "SPY")->position.shares, 100 * (puts - calls));
  for (const auto& fill : snap->stock_fills) {
    EXPECT_EQ(fill.source, StockSource::Assignment);
    EXPECT_EQ(fill.price, m("510"));
    EXPECT_EQ(fill.shares, fill.option == deep ? 100 * puts : -100 * calls);
  }
  // The new day takes the buy-backs above their marks: 0.25 a share on the puts, 0.20 on the calls.
  EXPECT_NEAR(day_pnl(*s), -25.0 * static_cast<double>(puts) - 20.0 * static_cast<double>(calls), 1e-9);
  EXPECT_NEAR(snap->attribution.total(), day_pnl(*s), 1e-6);
  const auto trades = lifecycles(snap->recent_fills, snap->closures, s->contracts());
  for (const auto& t : trades) {
    if (t.symbol != deep && t.symbol != call) continue;
    EXPECT_FALSE(t.closed);  // A partial assignment leaves this round trip open.
    EXPECT_FALSE(t.closure);
    EXPECT_EQ(t.quantity, held.at(t.symbol));
    EXPECT_EQ(t.closed_contracts, t.symbol == deep ? puts : calls);
  }
  // The draw is the account's, the contract's and the day's, so a replay assigns the same.
  const auto again = assigned_overnight()->snapshot();
  ASSERT_EQ(again->closures.size(), snap->closures.size());
  for (std::size_t i = 0; i < snap->closures.size(); ++i) EXPECT_EQ(again->closures[i].quantity, snap->closures[i].quantity);
}

TEST(FeeSchedule, OvernightAssignmentsChargeEachContractAndItsOptionTrade) {
  AccountRules rules;
  rules.fees = FeeSchedule{};
  rules.fees->exercise = m("5");
  const auto session = assigned_overnight(rules);
  const auto snapshot = session->snapshot();
  ASSERT_FALSE(snapshot->closures.empty());
  Money total;
  for (const auto& closure : snapshot->closures) {
    EXPECT_EQ(closure.kind, ClosureKind::Assignment);
    EXPECT_EQ(closure.fee, m("5") * -closure.quantity);
    total = total + closure.fee;
  }
  EXPECT_EQ(snapshot->account.fees, total);
  Money trades;
  for (const auto& trade : lifecycles(snapshot->recent_fills, snapshot->closures, session->contracts()))
    trades = trades + trade.fees;
  EXPECT_EQ(trades, total);
  EXPECT_NEAR(snapshot->attribution.total(), day_pnl(*session), 1e-6);
  const auto again = assigned_overnight(rules);
  EXPECT_EQ(again->snapshot_json(), session->snapshot_json());
}

TEST(TradingDelivery, TheNightsAssignmentsAreDecidedOnTheClosingMarksAndPrice) {
  // D11: the rollover ran on the new day's opening quotes, so the night's assignments
  // followed the next morning's prices. Rolled over before those quotes arrive, it decides
  // on the close: the put under its exercise value then is assigned at the stock's close.
  const auto deep = *md::parse_osi("SPY261022P00600000");  // 90 in the money at 510
  const auto near = *md::parse_osi("SPY261022P00515000");  // 5 in the money, with time value left
  Spy f;
  TradingSession s(roomy(), f.time);
  for (const auto& c : {deep, near}) f.define(s, c);
  f.quote(s, deep, "89.80", "90.20");
  f.quote(s, near, "7.00", "7.20");
  ASSERT_TRUE(s.submit(f.market("deep", deep, 10, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("near", near, 10, Side::Sell), f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 14, 30);
  f.quote(s, deep, "89.60", "89.90");
  f.quote(s, near, "7.00", "7.20");
  ASSERT_TRUE(s.roll_day(md::new_york_to_utc({2026, 9, 23}, 9, 30)).decision.ok());
  const auto snap = s.snapshot();
  ASSERT_FALSE(snap->closures.empty());
  for (const auto& c : snap->closures) EXPECT_EQ(c.symbol, deep.osi_symbol());
  ASSERT_FALSE(snap->stock_fills.empty());
  for (const auto& fill : snap->stock_fills) EXPECT_EQ(fill.price, m("510"));
}

TEST(TradingDelivery, TheStocksCloseMarksSharesWhileTheOptionsTradeOnAndOvernight) {
  // Cboe prints SPY's close at 16:00 while its options quote until 16:15.
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  const auto close = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  s.on_quotes({}, {}, close, {{"SPY", close, m("521")}});
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 16, 10));
  EXPECT_TRUE(stock(s, "SPY")->fresh);
  EXPECT_TRUE(s.snapshot()->valuation_complete);
  const auto night = md::new_york_to_utc({2026, 9, 22}, 18, 0);
  s.on_quotes({}, {}, night);
  EXPECT_TRUE(stock(s, "SPY")->fresh);
  ASSERT_TRUE(s.roll_day(night).decision.ok());
}

TEST(TradingDelivery, ShareRoundTripsTakeNotesAndTags) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  ASSERT_TRUE(s.annotate_shares(1, " held for the dividend ", {"Income", "income"}, f.time).decision.ok());
  const auto& note = s.snapshot()->annotations.at("s1");
  EXPECT_EQ(note.note, "held for the dividend");
  EXPECT_EQ(note.tags, (std::vector<std::string>{"income"}));
  // Share trades and option trades are named apart: fill 1 opened the option trade too.
  EXPECT_FALSE(s.snapshot()->annotations.contains("1"));
  EXPECT_EQ(s.annotate_shares(2, "no such", {}, f.time).decision.code, Reason::UNKNOWN_TRADE);
  ASSERT_TRUE(s.annotate_shares(1, "", {}, f.time).decision.ok());
  EXPECT_FALSE(s.snapshot()->annotations.contains("s1"));
}

TEST(TradingDelivery, SharesTradeInTheRegularSessionAndKeepTheirClose) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  Spy f;
  f.spot = 520;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "21.00", "21.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 1), f.time).decision.ok());
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 14);
  f.price(s);
  f.time = md::new_york_to_utc({2026, 9, 22}, 18, 0);
  s.on_quotes({}, {}, f.time);
  // The close stays current overnight, but shares trade in the regular session.
  EXPECT_TRUE(stock(s, "SPY")->fresh);
  EXPECT_EQ(s.trade_stock("SPY", -100, f.time).decision.code, Reason::SESSION_CLOSED);
}

TEST(TradingDelivery, AFlattenAfterTheStockCloseClosesTheOptionsAndSaysWhichSharesStay) {
  const auto call = *md::parse_osi("SPY261022C00500000");
  for (const Quantity calls : {1, 2}) {
    Spy f;
    f.spot = 520;
    TradingSession s(roomy(), f.time);
    f.define(s, call);
    f.quote(s, call, "21.00", "21.20");
    ASSERT_TRUE(s.submit(f.market("calls", call, calls), f.time).decision.ok());
    ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
    // At 16:05 SPY's options trade on to 16:15, but its shares closed at 16:00.
    f.time = md::new_york_to_utc({2026, 9, 22}, 16, 5);
    f.quote(s, call, "21.00", "21.20");
    const auto fills = s.snapshot()->stock_fills.size();
    const auto result = s.close_positions(std::nullopt, f.time);
    if (calls == 1) {
      // Only shares in scope, and none can trade: the flatten is refused.
      EXPECT_EQ(result.decision.code, Reason::SESSION_CLOSED);
      EXPECT_EQ(result.decision.scope, "SPY");
      EXPECT_TRUE(result.kept_stocks.empty());
    } else {
      ASSERT_TRUE(result.decision.ok()) << result.decision.message;
      EXPECT_TRUE(s.snapshot()->positions.empty());
      ASSERT_EQ(result.kept_stocks.size(), 1U);
      EXPECT_EQ(result.kept_stocks.at("SPY").code, Reason::SESSION_CLOSED);
    }
    ASSERT_NE(stock(s, "SPY"), nullptr);
    EXPECT_EQ(stock(s, "SPY")->position.shares, 100);
    EXPECT_EQ(s.snapshot()->stock_fills.size(), fills);
  }
}

const MarkedPosition* position(const TradingSession& s, const md::OptionContract& c) {
  for (const auto& p : s.snapshot()->positions) if (p.position.contract.osi_symbol() == c.osi_symbol()) return &p;
  return nullptr;
}

TEST(TradingDelivery, ADoNotExerciseInstructionLetsAnInTheMoneyLongExpireWorthless) {
  const auto call = *md::parse_osi("SPY260922C00500000");  // expires today at 16:15
  const auto put = *md::parse_osi("SPY260922P00520000");
  const auto index = *md::parse_osi("SPXW260922C05000000");
  Spy f;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.define(s, put);
  ASSERT_TRUE(s.define(index, f.time).decision.ok());
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, put, "10.00", "10.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("put", put, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.instruct_exercise(put.osi_symbol(), true, f.time).decision.code, Reason::INVALID_ORDER) << "a short";
  EXPECT_EQ(s.instruct_exercise(index.osi_symbol(), true, f.time).decision.code, Reason::INVALID_ORDER) << "no long position";
  EXPECT_EQ(s.instruct_exercise("SPY   260922C00510000", true, f.time).decision.code, Reason::UNKNOWN_CONTRACT);
  ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
  ASSERT_NE(position(s, call), nullptr);
  EXPECT_TRUE(position(s, call)->do_not_exercise);
  EXPECT_FALSE(position(s, put)->do_not_exercise);
  f.time = call.expiry_time();
  s.on_quotes({}, {}, f.time);
  const auto cash = s.snapshot()->account.cash;
  // Ten dollars in the money, the calls expire worthless as instructed: no shares.
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY"), nullptr);
  EXPECT_EQ(s.snapshot()->account.cash, cash);
  EXPECT_EQ(s.snapshot()->closures.back().kind, ClosureKind::Settlement);
  EXPECT_EQ(s.snapshot()->closures.back().price, Money{});
  // The short put is assigned as usual: 100 shares bought at 510.
  ASSERT_TRUE(s.settle(put.osi_symbol(), m("510"), f.time).decision.ok());
  ASSERT_NE(stock(s, "SPY"), nullptr);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 100);
}

TEST(TradingDelivery, AnExerciseInstructionCanBeWithdrawnEndsWithThePositionAndRecovers) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-delivery-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "session.jsonl").string();
  const auto call = *md::parse_osi("SPY260922C00500000");
  Spy f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.define(s, call);
    f.quote(s, call, "10.00", "10.20");
    ASSERT_TRUE(s.submit(f.market("calls", call, 2), f.time).decision.ok());
    ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
    ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), false, f.time).decision.ok());
    EXPECT_FALSE(position(s, call)->do_not_exercise);
    ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
    // Selling the position ends the instruction; a new long is exercised as usual.
    ASSERT_TRUE(s.submit(f.market("sell", call, 2, Side::Sell), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("again", call, 1), f.time).decision.ok());
    EXPECT_FALSE(position(s, call)->do_not_exercise);
    ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  EXPECT_TRUE(position(recovered, call)->do_not_exercise);
  f.time = call.expiry_time();
  recovered.on_quotes({}, {}, f.time);
  ASSERT_TRUE(recovered.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  EXPECT_EQ(stock(recovered, "SPY"), nullptr);
  std::filesystem::remove_all(directory);
}

TEST(TradingDelivery, ExerciseInstructionsForfeitCashSettlementAndWithdrawalRestoresIt) {
  for (const bool withdraw : {false, true}) {
    test::ScriptedMarket f;
    f.contract = *md::parse_osi("SPXW260922C05000000");
    TradingSession s(roomy(), f.time);
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("long", 1), f.time).decision.ok());
    ASSERT_TRUE(s.trip_kill("pause", f.time).decision.ok());
    ASSERT_TRUE(s.instruct_exercise(f.symbol(), true, f.time).decision.ok());
    if (withdraw) { ASSERT_TRUE(s.instruct_exercise(f.symbol(), false, f.time).decision.ok()); }
    const auto cash = s.snapshot()->account.cash;
    f.time = f.contract.expiry_time();
    ASSERT_TRUE(s.settle(f.symbol(), m("5010"), f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->account.cash, cash + (withdraw ? m("1000") : Money{}));
    EXPECT_EQ(s.snapshot()->account.realised, withdraw ? m("580") : m("-420"));
    EXPECT_EQ(s.snapshot()->closures.back().price, withdraw ? m("10") : Money{});
    EXPECT_TRUE(s.snapshot()->positions.empty());
    EXPECT_TRUE(s.snapshot()->stocks.empty());
    EXPECT_EQ(s.snapshot()->evaluation.day_close_equity, s.snapshot()->equity);
    EXPECT_NEAR(s.snapshot()->attribution.total(), (s.snapshot()->equity - s.snapshot()->start_of_day_equity).dollars(), 1e-6);
  }
}

TEST(TradingDelivery, PartialExerciseKeepsTheInstructionAndFullExerciseClearsIt) {
  const auto call = *md::parse_osi("SPY260922C00500000");
  Spy f;
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "10.00", "10.20");
  ASSERT_TRUE(s.submit(f.market("long", call, 2), f.time).decision.ok());
  ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
  for (const auto& warning : s.warnings()) EXPECT_NE(warning.code, "EXPIRY_DELIVERY");
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  EXPECT_TRUE(position(s, call)->do_not_exercise);
  ASSERT_TRUE(s.exercise(call.osi_symbol(), 1, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("again", call, 1), f.time).decision.ok());
  EXPECT_FALSE(position(s, call)->do_not_exercise);
  ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
  ASSERT_TRUE(s.reset_account(m("100000"), {}, "new attempt", f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("reset long", call, 1), f.time).decision.ok());
  EXPECT_FALSE(position(s, call)->do_not_exercise);
}

TEST(TradingDelivery, SharesOpenForAHedgeOrACoveredCall) {
  const auto call = *md::parse_osi("SPY261022C00520000");
  Spy f;
  TradingSession s(roomy(), f.time);
  // Nothing held in SPY, so no batch has priced it yet.
  EXPECT_EQ(s.trade_stock("SPY", 100, f.time).decision.code, Reason::STALE_QUOTE);
  EXPECT_THROW((void)s.trade_stock("SPY", 100, f.time, StockPrice{"QQQ", f.time, m("400")}), TradingError);
  EXPECT_THROW((void)s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time + 1, m("510")}), TradingError);
  // The engine passes the feed's price with the trade.
  const auto bought = s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")});
  ASSERT_TRUE(bought.decision.ok()) << bought.decision.message;
  ASSERT_NE(stock(s, "SPY"), nullptr);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 100);
  EXPECT_EQ(s.snapshot()->account.cash, m("49000"));
  EXPECT_EQ(s.snapshot()->stock_fills.back().source, StockSource::Trade);
  EXPECT_DOUBLE_EQ(s.snapshot()->risk.underlyings.at("SPY").position.dollar_delta, 100 * 510.0);
  // A call written against them, more shares, then a sale through zero into a short.
  f.define(s, call);
  f.quote(s, call, "3.00", "3.20");
  ASSERT_TRUE(s.submit(f.market("covered", call, 1, Side::Sell), f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 50, f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, 150);
  ASSERT_TRUE(s.trade_stock("SPY", -200, f.time).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -50);
  EXPECT_EQ(share_lifecycles(s.snapshot()->stock_fills).size(), 2U) << "the long round trip, and the short it reversed into";
  // An index has no shares.
  EXPECT_EQ(s.trade_stock("SPX", 1, f.time, StockPrice{"SPX", f.time, m("6500")}).decision.code, Reason::INVALID_ORDER);
  // The stock market's regular session only.
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 5);
  f.price(s);
  EXPECT_EQ(s.trade_stock("SPY", 100, f.time).decision.code, Reason::SESSION_CLOSED);
}

TEST(TradingDelivery, OpeningSharesTakesTheAccountsChecks) {
  Spy f;
  // Buy-only and defined-risk plans take no short shares, but sell those held.
  for (const bool buy_only : {true, false}) {
    AccountRules rules;
    (buy_only ? rules.buy_only : rules.defined_risk) = true;
    TradingSession s(roomy(rules), f.time);
    f.price(s);
    EXPECT_EQ(s.trade_stock("SPY", -100, f.time).decision.code, buy_only ? Reason::BUY_ONLY : Reason::DEFINED_RISK);
    ASSERT_TRUE(s.trade_stock("SPY", 100, f.time).decision.ok());
    EXPECT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
  }
  {
    // 100,000 buys 196 shares at 510, not 200.
    AccountRules rules;
    rules.buying_power = true;
    TradingSession s(roomy(rules), f.time);
    f.price(s);
    const auto refused = s.trade_stock("SPY", 200, f.time).decision;
    EXPECT_EQ(refused.code, Reason::BUYING_POWER);
    EXPECT_EQ(refused.actual, 2000.0);
    EXPECT_EQ(refused.scope, "SPY");
    ASSERT_TRUE(s.trade_stock("SPY", 196, f.time).decision.ok());
    EXPECT_EQ(s.snapshot()->buying_power.available, m("40"));
  }
  {
    // The kill switch stops opening, not closing; an opening share trade counts for the guardrails.
    auto c = roomy();
    c.guardrails.max_opening_trades = 1;
    TradingSession s(c, f.time);
    f.price(s);
    ASSERT_TRUE(s.trade_stock("SPY", 100, f.time).decision.ok());
    EXPECT_EQ(s.trade_stock("SPY", 100, f.time).decision.code, Reason::TRADE_LIMIT);
    EXPECT_TRUE(s.trade_stock("SPY", -50, f.time).decision.ok());
    EXPECT_EQ(stock(s, "SPY")->position.shares, 50);
  }
  {
    // Exposure limits count the shares' dollar delta.
    auto c = roomy();
    c.limits.per_underlying = {40000, 1e12};
    TradingSession s(c, f.time);
    f.price(s);
    EXPECT_EQ(s.trade_stock("SPY", 100, f.time).decision.code, Reason::DELTA_LIMIT);
    EXPECT_TRUE(s.trade_stock("SPY", 50, f.time).decision.ok());
  }
}

TEST(TradingDelivery, OpenedSharesRecoverFromTheJournal) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-delivery-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  const auto path = (directory / "session.jsonl").string();
  Spy f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    ASSERT_TRUE(s.trade_stock("SPY", 300, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
    ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
    expected = s.snapshot_json();
    head = journal->head();
  }
  const auto recovered = TradingSession::recover(FileJournal::read(path, head));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  EXPECT_EQ(stock(recovered, "SPY")->position.shares, 200);
  std::filesystem::remove_all(directory);
}


TEST(TradingDelivery, OpeningSharesChecksFreshnessSessionsAndAccountState) {
  Spy f;
  for (const auto symbol : {"SPX", "XSP", "VIX", "RUT", "NDX"}) {
    TradingSession s(roomy(), f.time);
    EXPECT_EQ(s.trade_stock(symbol, 1, f.time).decision.code, Reason::INVALID_ORDER);
  }
  TradingSession s(roomy(), f.time);
  for (const Quantity size : {Quantity{0}, Quantity{10'000'001}, Quantity{-10'000'001}, std::numeric_limits<Quantity>::min()}) {
    EXPECT_THROW((void)s.trade_stock("SPY", size, f.time), TradingError);
  }
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time - 10 * md::kNanosPerMinute, m("510")}).decision.code,
            Reason::STALE_QUOTE);
  s.trip_kill("manual", f.time);
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time).decision.code, Reason::KILL_SWITCH);
  ASSERT_TRUE(s.reset_kill("ready", f.time).decision.ok());
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  s.trip_kill("manual", f.time);
  EXPECT_EQ(s.trade_stock("SPY", -101, f.time).decision.code, Reason::KILL_SWITCH);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 100);  // Refused reversal never partly closes.
  EXPECT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
  for (const auto time : {md::new_york_to_utc({2026, 9, 22}, 9, 29),
                          md::new_york_to_utc({2026, 9, 22}, 16, 0),
                          md::new_york_to_utc({2026, 11, 27}, 13, 0)}) {
    TradingSession closed(roomy(), time);
    EXPECT_EQ(closed.trade_stock("SPY", 1, time, StockPrice{"SPY", time, m("510")}).decision.code, Reason::SESSION_CLOSED);
  }
  const auto early = md::new_york_to_utc({2026, 11, 27}, 12, 59);
  TradingSession open(roomy(), early);
  EXPECT_TRUE(open.trade_stock("SPY", 1, early, StockPrice{"SPY", early, m("510")}).decision.ok());
  auto config = roomy();
  config.rules.profit_target = m("50");
  TradingSession evaluation(config, f.time);
  ASSERT_TRUE(evaluation.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  f.spot = 511;
  f.price(evaluation);
  ASSERT_EQ(evaluation.snapshot()->evaluation.status, EvaluationStatus::Passed);
  EXPECT_EQ(evaluation.trade_stock("SPY", 1, f.time).decision.code, Reason::EVALUATION_CLOSED);
}

TEST(TradingDelivery, OpeningSharesNeedsCompleteValuationsAndHonorsLossLimits) {
  Spy f;
  const auto call = *md::parse_osi("SPY261022C00520000");
  TradingSession s(roomy(), f.time);
  f.define(s, call);
  f.quote(s, call, "3", "3.20");
  ASSERT_TRUE(s.submit(f.market("open", call, 1), f.time).decision.ok());
  f.time += 10 * md::kNanosPerMinute;
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("510")}).decision.code, Reason::STALE_QUOTE);
  auto c = roomy();
  c.limits.max_daily_loss = m("100");
  TradingSession loss(c, f.time);
  ASSERT_TRUE(loss.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  // The command's fresh mark itself creates a loss, before the monitor latches kill.
  EXPECT_EQ(loss.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("508")}).decision.code, Reason::DAILY_LOSS);
  EXPECT_TRUE(loss.snapshot()->risk.kill_latched);
  EXPECT_TRUE(loss.trade_stock("SPY", -100, f.time).decision.ok());
}

TEST(TradingDelivery, OpenedSharesCoverCallsAndCollarsAndShortSharesTakeRegT) {
  Spy f;
  const auto call = *md::parse_osi("SPY261022C00520000");
  const auto put = *md::parse_osi("SPY261022P00500000");
  AccountRules rules;
  rules.buying_power = true;
  TradingSession s(roomy(rules, "52000"), f.time);
  f.define(s, call);
  f.define(s, put);
  f.quote(s, call, "3", "3.20");
  f.quote(s, put, "2", "2.20");
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("covered", call, 1, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
  EXPECT_EQ(s.snapshot()->buying_power.available, m("1299.35"));
  ASSERT_TRUE(s.submit(f.market("collar", put, 1), f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->buying_power.short_requirement, Money{});
  EXPECT_EQ(s.snapshot()->buying_power.available, m("1078.70"));
  TradingSession shorted(roomy(rules), f.time);
  ASSERT_TRUE(shorted.trade_stock("SPY", -100, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  EXPECT_EQ(shorted.snapshot()->buying_power.short_requirement, m("76500"));
  EXPECT_EQ(shorted.snapshot()->buying_power.available, m("74500"));
  rules.defined_risk = true;
  TradingSession defined(roomy(rules), f.time);
  f.define(defined, call);
  f.quote(defined, call, "3", "3.20");
  ASSERT_TRUE(defined.trade_stock("SPY", 100, f.time).decision.ok());
  // The plan's option-coverage rule is deliberately stricter than margin coverage.
  EXPECT_EQ(defined.submit(f.market("covered", call, 1, Side::Sell), f.time).decision.code, Reason::DEFINED_RISK);
}

TEST(TradingDelivery, OpenedSharesReceiveDividendsAndNetWithAssignment) {
  Spy f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.trade_stock("SPY", 150, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  ASSERT_TRUE(s.annotate_shares(1, "covered call", {"hedge"}, f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  f.price(s);
  f.time = md::new_york_to_utc({2026, 9, 23}, 9, 30);
  ASSERT_TRUE(s.roll_day(f.time, {{"SPY", {2026, 9, 23}, m("1.25")}}).decision.ok());
  ASSERT_EQ(s.snapshot()->dividends.size(), 1U);
  EXPECT_EQ(s.snapshot()->dividends.back().amount, m("187.50"));
  const auto call = *md::parse_osi("SPY260923C00500000");
  f.define(s, call);
  f.quote(s, call, "10", "10.20");
  ASSERT_TRUE(s.submit(f.market("covered", call, 1, Side::Sell), f.time).decision.ok());
  f.time = call.expiry_time();
  f.price(s);
  ASSERT_TRUE(s.settle(call.osi_symbol(), m("510"), f.time).decision.ok());
  ASSERT_NE(stock(s, "SPY"), nullptr);
  EXPECT_EQ(stock(s, "SPY")->position.shares, 50);
  EXPECT_EQ(stock(s, "SPY")->position.basis, m("25500"));
  EXPECT_EQ(s.snapshot()->annotations.at("s1").note, "covered call");
  const auto trips = share_lifecycles(s.snapshot()->stock_fills, s.snapshot()->dividends);
  ASSERT_EQ(trips.size(), 1U);
  // At the next stock session flatten closes opened shares too.
  f.time = md::new_york_to_utc({2026, 9, 24}, 10, 0);
  f.price(s);
  ASSERT_TRUE(s.close_positions({}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->stocks.empty());
}

TEST(TradingDelivery, SharePreviewIsPureAndUsesTheSameChecksAndMargin) {
  Spy f;
  TradingSession s(roomy(), f.time);
  const auto before = s.snapshot_json();
  const auto preview = s.preview_trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")});
  EXPECT_EQ(s.snapshot_json(), before);
  ASSERT_TRUE(preview.decision.ok());
  EXPECT_EQ(preview.price, m("510"));
  EXPECT_EQ(preview.after.buying_power, m("49000"));
  ASSERT_TRUE(preview.after.exposure);
  EXPECT_DOUBLE_EQ(preview.after.exposure->dollar_delta, 51000);
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  EXPECT_EQ(preview.after.buying_power, s.snapshot()->buying_power.available);
  EXPECT_EQ(preview.after.equity, s.snapshot()->equity);
}

TEST(TradingDelivery, ShareReversalsKeepExactBasisAndDailyPnlAndSeparateRoundTrips) {
  Spy f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("510")}).decision.ok());
  ASSERT_TRUE(s.annotate_shares(1, "hedge", {"delta"}, f.time).decision.ok());
  ++f.time;
  ASSERT_TRUE(s.trade_stock("SPY", 100, f.time, StockPrice{"SPY", f.time, m("520")}).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.basis, m("103000"));
  ++f.time;
  ASSERT_TRUE(s.trade_stock("SPY", -250, f.time, StockPrice{"SPY", f.time, m("530")}).decision.ok());
  EXPECT_EQ(stock(s, "SPY")->position.shares, -50);
  EXPECT_EQ(stock(s, "SPY")->position.basis, m("-26500"));
  EXPECT_EQ(s.snapshot()->account.realised, m("3000"));
  EXPECT_EQ(s.snapshot()->equity, m("103000"));
  EXPECT_EQ(s.snapshot()->evaluation.day_close_equity, m("103000"));
  EXPECT_DOUBLE_EQ(day_pnl(s), 3000);
  EXPECT_NEAR(s.snapshot()->attribution.delta, 3000, 1e-9);
  EXPECT_EQ(s.snapshot()->guardrails.opening_trades, 3U);
  const auto trips = share_lifecycles(s.snapshot()->stock_fills);
  ASSERT_EQ(trips.size(), 2U);
  EXPECT_EQ(s.snapshot()->annotations.at("s1").note, "hedge");
  ++f.time;
  ASSERT_TRUE(s.trade_stock("SPY", 50, f.time, StockPrice{"SPY", f.time, m("520")}).decision.ok());
  EXPECT_TRUE(s.snapshot()->stocks.empty());
  EXPECT_EQ(s.snapshot()->account.realised, m("3500"));
  EXPECT_EQ(s.snapshot()->equity, m("103500"));
}

TEST(TradingDelivery, FreshOptionMarksWithoutFreshGreeksBlockOpeningShares) {
  Spy f;
  auto c = roomy();
  c.limits.max_valuation_age = md::kNanosPerSecond;
  TradingSession s(c, f.time);
  const auto call = *md::parse_osi("SPY261022C00520000");
  f.define(s, call);
  f.quote(s, call, "3", "3.20");
  ASSERT_TRUE(s.submit(f.market("open", call, 1), f.time).decision.ok());
  f.time += 2 * md::kNanosPerSecond;
  s.on_quotes({{call.osi_symbol(), ++f.observation, f.time, m("3"), m("3.20"), 10, 10}}, {}, f.time);
  EXPECT_TRUE(s.snapshot()->valuation_complete);
  EXPECT_EQ(s.trade_stock("SPY", 1, f.time, StockPrice{"SPY", f.time, m("510")}).decision.code, Reason::MISSING_VALUATION);
}

}  // namespace
}  // namespace openport::trading
