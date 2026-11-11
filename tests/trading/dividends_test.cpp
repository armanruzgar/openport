#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "openport/trading/dividends.hpp"
#include "openport/trading/history.hpp"
#include "openport/trading/session.hpp"

namespace openport::trading {
namespace {
Money m(std::string_view s) { return Money::parse(s); }
std::vector<Dividend> parse(const std::string& text) {
  std::istringstream in(text);
  return parse_dividends(in);
}

TEST(TradingDividends, ReadsAFileOfExDatesAndAmounts) {
  const auto all = parse("symbol,ex_date,amount\n# quarterly\n\nQQQ,2026-12-21,0.70\n SPY , 2026-12-18 , 1.90 \n");
  ASSERT_EQ(all.size(), 2U);
  EXPECT_EQ(all[0].symbol, "SPY");  // sorted by ex-date
  EXPECT_EQ(all[0].ex_date, (md::Date{2026, 12, 18}));
  EXPECT_EQ(all[0].per_share, m("1.90"));
  EXPECT_EQ(all[1].symbol, "QQQ");
  for (const auto* bad : {"SPY,2026-12-18", "SPY,2026-13-01,1.90", "spy,2026-12-18,1.90", "SPY,2026-12-18,-1",
                          "SPY,2026-12-18,abc", "SPY,2026-12-18,1.90,x", "SPY,2026-12-18,1.90\nSPY,2026-12-18,2"}) {
    EXPECT_THROW(parse(bad), std::invalid_argument) << bad;
  }
  try {
    parse("SPY,2026-12-18,1.90\nQQQ,soon,1\n");
    FAIL();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("line 2"), std::string::npos);
  }
  // A rollover pays what went ex after the last trading date, through the new one.
  const auto due = dividends_due(all, {2026, 12, 17}, {2026, 12, 21});
  EXPECT_EQ(due.size(), 2U);
  EXPECT_TRUE(dividends_due(all, {2026, 12, 18}, {2026, 12, 18}).empty());
}

TEST(TradingDividends, AByteOrderMarkBeforeTheFirstLineIsNotPartOfIt) {
  // Excel's "CSV UTF-8" and Windows editors start the file with one.
  const auto headed = parse("\xEF\xBB\xBFsymbol,ex_date,amount\r\nSPY,2026-12-18,1.90\r\n");
  ASSERT_EQ(headed.size(), 1U);
  EXPECT_EQ(headed[0].symbol, "SPY");
  EXPECT_EQ(headed[0].per_share, m("1.90"));
  const auto bare = parse("\xEF\xBB\xBFQQQ,2026-12-21,0.70\n");
  ASSERT_EQ(bare.size(), 1U);
  EXPECT_EQ(bare[0].symbol, "QQQ");
  // Only before the file's first line: one inside a symbol later is still refused.
  EXPECT_THROW(parse("SPY,2026-12-18,1.90\n\xEF\xBB\xBFQQQ,2026-12-21,0.70\n"), std::invalid_argument);
}

/// SPY shares through an exercise, then SPY's own price in every batch. The call
/// expires after the 2026-12-18 ex-date, so it can be assigned the night before.
struct Held {
  md::OptionContract call = *md::parse_osi("SPY270115C00500000");
  md::Timestamp time = md::new_york_to_utc({2026, 12, 17}, 10, 0);
  std::uint64_t observation = 0;
  void quote(TradingSession& s, double spot) {
    ++observation;
    s.on_quotes({{call.osi_symbol(), observation, time, m("20.00"), m("20.20"), 10, 10}},
                {{call.osi_symbol(), time, 0.9, 0.01, 0.2, -0.05, spot, spot, 0.99, md::years_between(time, call.expiry_time()), 0.2, true}},
                time, {{"SPY", time, Money::from_double(spot)}});
  }
};
SessionConfig roomy() {
  SessionConfig c;
  c.limits.aggregate = {1e12, 1e12};
  c.limits.per_underlying = {1e12, 1e12};
  return c;
}

TEST(TradingDividends, SharesHeldIntoTheExDateReceiveItAndItJoinsTheirRoundTrip) {
  Held f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
  f.quote(s, 520);
  ASSERT_TRUE(s.submit({"calls", f.call.osi_symbol(), Side::Buy, OrderType::Market, TimeInForce::Ioc, 2, {}, {}, {}, {}}, f.time).decision.ok());
  ASSERT_TRUE(s.exercise(f.call.osi_symbol(), 2, f.time).decision.ok());  // 200 SPY at 520
  const auto cash = s.snapshot()->account.cash;
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 0);
  f.quote(s, 521);
  const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
  s.on_quotes({}, {}, night);
  const std::vector<Dividend> due{{"SPY", {2026, 12, 18}, m("1.90")}, {"QQQ", {2026, 12, 18}, m("0.70")}};
  ASSERT_TRUE(s.roll_day(night, due).decision.ok());
  auto snap = s.snapshot();
  EXPECT_EQ(snap->account.cash, cash + m("380"));  // 200 shares at 1.90; no QQQ held
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].shares, 200);
  EXPECT_EQ(snap->dividends[0].amount, m("380"));
  EXPECT_NEAR(snap->attribution.other, 380, 1e-9);
  // Once per ex-date.
  f.time = md::new_york_to_utc({2026, 12, 18}, 10, 0);
  f.quote(s, 519.10);
  ASSERT_TRUE(s.trade_stock("SPY", -200, f.time).decision.ok());
  snap = s.snapshot();
  const auto trips = share_lifecycles(snap->stock_fills, snap->dividends);
  ASSERT_EQ(trips.size(), 1U);
  EXPECT_EQ(trips[0].dividends, m("380"));
  EXPECT_EQ(trips[0].gross, m("-180"));  // bought at 520, sold at 519.10
}

TEST(TradingDividends, APaymentIsWholeCents) {
  Held f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
  f.quote(s, 520);
  ASSERT_TRUE(s.submit({"calls", f.call.osi_symbol(), Side::Buy, OrderType::Market, TimeInForce::Ioc, 2, {}, {}, {}, {}}, f.time).decision.ok());
  ASSERT_TRUE(s.exercise(f.call.osi_symbol(), 2, f.time).decision.ok());  // 200 SPY
  ASSERT_TRUE(s.trade_stock("SPY", -137, f.time).decision.ok());          // 63 left
  const auto cash = s.snapshot()->account.cash;
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 0);
  f.quote(s, 521);
  const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
  s.on_quotes({}, {}, night);
  // 0.123457 x 63 = 7.777791 is paid as 7.78.
  ASSERT_TRUE(s.roll_day(night, {{"SPY", {2026, 12, 18}, m("0.123457")}}).decision.ok());
  const auto snap = s.snapshot();
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].amount, m("7.78"));
  EXPECT_EQ(snap->account.cash, cash + m("7.78"));
}

TEST(TradingDividends, SharesSoldAtTheRolloversOwnMarketTimeKeepItInTheirRoundTrip) {
  // B42: the round trip took the dividend only after the fills before its time, so a
  // sale at the rollover's own market time closed the trip first and lost it.
  Held f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
  f.quote(s, 520);
  ASSERT_TRUE(s.submit({"calls", f.call.osi_symbol(), Side::Buy, OrderType::Market, TimeInForce::Ioc, 2, {}, {}, {}, {}}, f.time).decision.ok());
  ASSERT_TRUE(s.exercise(f.call.osi_symbol(), 2, f.time).decision.ok());  // 200 SPY at 520
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 0);
  f.quote(s, 521);
  // The ex-date's first batch rolls over at 09:30, and the shares are sold at 09:30.
  f.time = md::new_york_to_utc({2026, 12, 18}, 9, 30);
  ASSERT_TRUE(s.roll_day(f.time, {{"SPY", {2026, 12, 18}, m("1.90")}}).decision.ok());
  f.quote(s, 519.10);
  ASSERT_TRUE(s.trade_stock("SPY", -200, f.time).decision.ok());
  const auto snap = s.snapshot();
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].time, snap->stock_fills.back().time);
  const auto trips = share_lifecycles(snap->stock_fills, snap->dividends);
  ASSERT_EQ(trips.size(), 1U);
  EXPECT_TRUE(trips[0].closed);
  EXPECT_EQ(trips[0].dividends, m("380"));
  EXPECT_EQ(trips[0].gross, m("-180"));
  // A payment recorded before the count was kept comes before a trade at its time.
  auto legacy = snap->dividends;
  legacy.mut(0).after_stock_fill.reset();
  EXPECT_EQ(share_lifecycles(snap->stock_fills, legacy)[0].dividends, m("380"));
}

TEST(TradingDividends, ShortSharesPayIt) {
  Held f;
  TradingSession s(roomy(), f.time);
  ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
  f.quote(s, 520);
  ASSERT_TRUE(s.submit({"sold", f.call.osi_symbol(), Side::Sell, OrderType::Market, TimeInForce::Ioc, 10, {}, {}, {}, {}}, f.time).decision.ok());
  // Partly assigned overnight as the market prices the call under its exercise value, then short into the ex-date.
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 14, 30);
  ++f.observation;
  s.on_quotes({{f.call.osi_symbol(), f.observation, f.time, m("19.80"), m("19.90"), 10, 10}},
              {{f.call.osi_symbol(), f.time, 0.9, 0.01, 0.2, -0.05, 520, 520, 0.99, md::years_between(f.time, f.call.expiry_time()), 0.2, true}},
              f.time, {{"SPY", f.time, m("520")}});
  const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
  s.on_quotes({}, {}, night);
  const auto cash = s.snapshot()->account.cash;
  ASSERT_TRUE(s.roll_day(night, {{"SPY", {2026, 12, 18}, m("1.90")}}).decision.ok());
  const auto snap = s.snapshot();
  ASSERT_EQ(snap->closures.size(), 1U);
  const Quantity assigned = -snap->closures[0].quantity;
  ASSERT_GT(assigned, 0);
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].shares, -100 * assigned);
  EXPECT_EQ(snap->dividends[0].amount, m("-190") * assigned);
  // The buy-back at intrinsic and the short sale at 520 net to the strike; then the dividend is paid.
  EXPECT_EQ(snap->account.cash, cash + (m("-20") * 100 + m("520") * 100 - m("190")) * assigned);
}

TEST(TradingDividends, CallsWithLessTimeValueThanTheDividendAreAssignedBeforeIt) {
  const auto roll = [](std::vector<Dividend> due) {
    Held f;
    auto s = std::make_unique<TradingSession>(roomy(), f.time);
    EXPECT_TRUE(s->define(f.call, f.time).decision.ok());
    f.quote(*s, 520);
    EXPECT_TRUE(s->submit({"sold", f.call.osi_symbol(), Side::Sell, OrderType::Market, TimeInForce::Ioc, 10, {}, {}, {}, {}}, f.time).decision.ok());
    // At 20.10 against 20 of exercise value, the call keeps 0.10 of time value.
    f.time = md::new_york_to_utc({2026, 12, 17}, 16, 14, 30);
    f.quote(*s, 520);
    const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
    s->on_quotes({}, {}, night);
    EXPECT_TRUE(s->roll_day(night, due).decision.ok());
    return s;
  };
  // Without a dividend a holder keeps it.
  EXPECT_TRUE(roll({})->snapshot()->closures.empty());
  // Exercising takes the 1.90 dividend for 0.10 of time value, so holders do.
  const auto s = roll({{"SPY", {2026, 12, 18}, m("1.90")}});
  const auto snap = s->snapshot();
  ASSERT_EQ(snap->closures.size(), 1U);
  const Quantity assigned = -snap->closures[0].quantity;
  EXPECT_GT(assigned, 0);
  EXPECT_LT(assigned, 10);
  ASSERT_EQ(snap->dividends.size(), 1U);
  EXPECT_EQ(snap->dividends[0].shares, -100 * assigned);
}

TEST(TradingDividends, OptionsExpiringOnTheNewDaySettleInsteadOfBeingAssigned) {
  // Ten each of a call worth less than the dividend's time value and a put below its
  // exercise value, expiring on the ex-date, and the same call expiring after it.
  const auto expiring_call = *md::parse_osi("SPY261218C00500000");
  const auto expiring_put = *md::parse_osi("SPY261218P00540000");
  Held f;
  TradingSession s(roomy(), f.time);
  for (const auto& c : {f.call, expiring_call, expiring_put}) ASSERT_TRUE(s.define(c, f.time).decision.ok());
  const auto quote = [&](md::Timestamp time) {
    std::vector<QuoteObservation> quotes;
    std::vector<Valuation> valuations;
    const std::vector<std::tuple<md::OptionContract, const char*, const char*>> books{
        {f.call, "20.00", "20.20"}, {expiring_call, "20.00", "20.20"}, {expiring_put, "19.80", "19.90"}};
    for (const auto& [c, bid, ask] : books) {
      quotes.push_back({c.osi_symbol(), ++f.observation, time, m(bid), m(ask), 10, 10});
      valuations.push_back({c.osi_symbol(), time, 0.9, 0.01, 0.2, -0.05, 520, 520, 0.99,
                            md::years_between(time, c.expiry_time()), 0.2, true});
    }
    s.on_quotes(quotes, valuations, time, {{"SPY", time, m("520")}});
  };
  quote(f.time);
  for (const auto& c : {f.call, expiring_call, expiring_put})
    ASSERT_TRUE(s.submit({"sold " + c.osi_symbol(), c.osi_symbol(), Side::Sell, OrderType::Market, TimeInForce::Ioc, 10,
                          {}, {}, {}, {}}, f.time).decision.ok());
  f.time = md::new_york_to_utc({2026, 12, 17}, 16, 14, 30);
  quote(f.time);
  const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
  s.on_quotes({}, {}, night);
  ASSERT_TRUE(s.roll_day(night, {{"SPY", {2026, 12, 18}, m("1.90")}}).decision.ok());
  const auto snap = s.snapshot();
  // Only the later call is assigned; the options expiring on 2026-12-18 settle at its close.
  ASSERT_FALSE(snap->closures.empty());
  for (const auto& closure : snap->closures) EXPECT_EQ(closure.symbol, f.call.osi_symbol());
  for (const auto& p : snap->positions)
    if (p.position.contract.expiry == expiring_call.expiry) { EXPECT_EQ(p.position.quantity, -10); }
}

TEST(TradingDividends, ADividendThatRoundsToNothingPaysNothingAndTheDayRollsOn) {
  // $0.004 on one share is under half a cent: replay drivers 1-8 asked the ledger to pay
  // $0.00, which it refuses, so the rollover failed and with it the account.
  const std::vector<Dividend> due{{"SPY", {2026, 12, 18}, m("0.004")}};
  for (const bool revised : {false, true}) {
    SCOPED_TRACE(revised ? "driver 9" : "drivers 1-8");
    Held f;
    TradingSession s(roomy(), f.time);
    s.set_revised_corporate_actions(revised);
    ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
    f.quote(s, 520);
    ASSERT_TRUE(s.trade_stock("SPY", 1, f.time).decision.ok());
    f.time = md::new_york_to_utc({2026, 12, 17}, 16, 0);
    f.quote(s, 521);
    const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
    s.on_quotes({}, {}, night);
    if (!revised) {
      EXPECT_THROW((void)s.roll_day(night, due), TradingError);
      continue;
    }
    // The warning says what the rollover will pay: nothing.
    const auto warnings = s.warnings({}, due);
    const auto ex = std::find_if(warnings.begin(), warnings.end(), [](const RiskWarning& w) { return w.code == "EX_DIVIDEND"; });
    ASSERT_NE(ex, warnings.end());
    EXPECT_NE(ex->message.find("less than half a cent, so nothing is paid"), std::string::npos) << ex->message;
    const auto cash = s.snapshot()->account.cash;
    ASSERT_TRUE(s.roll_day(night, due).decision.ok());
    EXPECT_TRUE(s.snapshot()->dividends.empty());
    EXPECT_EQ(s.snapshot()->account.cash, cash);
    EXPECT_EQ(s.snapshot()->evaluation.day, (md::Date{2026, 12, 18}));
  }
}

TEST(TradingDividends, ACatchUpRolloverPaysADividendOnTheSharesBeforeALaterSplit) {
  // Down from Thursday's close to Monday: $1.50 went ex on Friday, on 100 shares, and a
  // 2-for-1 on Monday. Replay drivers 1-8 split first and paid it on 200.
  for (const bool revised : {false, true}) {
    SCOPED_TRACE(revised ? "driver 9" : "drivers 1-8");
    Held f;
    TradingSession s(roomy(), f.time);
    s.set_revised_corporate_actions(revised);
    ASSERT_TRUE(s.define(f.call, f.time).decision.ok());
    f.quote(s, 520);
    ASSERT_TRUE(s.trade_stock("SPY", 100, f.time).decision.ok());
    f.time = md::new_york_to_utc({2026, 12, 17}, 16, 0);
    f.quote(s, 521);
    const auto cash = s.snapshot()->account.cash;
    const auto monday = md::new_york_to_utc({2026, 12, 21}, 9, 0);
    ASSERT_TRUE(s.roll_day(monday, {{"SPY", {2026, 12, 18}, m("1.50")}}, {{"SPY", {2026, 12, 21}, 2, 1}}).decision.ok());
    const auto snap = s.snapshot();
    ASSERT_EQ(snap->dividends.size(), 1U);
    EXPECT_EQ(snap->dividends[0].shares, revised ? 100 : 200);
    EXPECT_EQ(snap->dividends[0].amount, m(revised ? "150" : "300"));
    EXPECT_EQ(snap->account.cash, cash + m(revised ? "150" : "300"));
    ASSERT_EQ(snap->stocks.size(), 1U);
    EXPECT_EQ(snap->stocks[0].position.shares, 200);
  }
}

TEST(TradingDividends, EarlyAssignmentWeighsTheDividendOnWhatAUnitDelivers) {
  // A holder exercises a call before a dividend when its time value is less than what the
  // dividend pays on the shares one unit delivers: twice a share's on a 2-for-1-adjusted
  // series, a tenth on a 1-for-10, and twice on a standard series splitting 2-for-1 the same
  // night, the dividend being on the split shares. Replay drivers 1-8 weighed a share's.
  struct Case {
    const char* name;
    std::optional<Deliverable> deliverable;
    double spot;
    const char* bid;
    const char* ask;
    const char* dividend;
    std::vector<CorporateAction> splits;
    bool assigned;
  };
  const md::Date ex{2026, 12, 18};
  const std::vector<Case> cases{
      // 200 shares at 255 are 10.00 a unit in the money; 0.60 of time value, 1.00 a unit of dividend.
      {"2:1-adjusted", Deliverable{200, {}}, 255, "10.50", "10.70", "0.50", {}, true},
      // 10 shares at 5,100 are 10.00 a unit; 0.20 of time value, 0.10 a unit of dividend.
      {"1:10-adjusted", Deliverable{10, {}}, 5100, "10.10", "10.30", "1.00", {}, false},
      // A standard call at 510 splitting tonight; 0.60 of time value, 1.00 a unit of dividend.
      {"splitting", std::nullopt, 510, "10.50", "10.70", "0.50", {{"SPY", ex, 2, 1}}, true},
  };
  for (const auto& c : cases) {
    for (const bool revised : {false, true}) {
      SCOPED_TRACE(std::string(c.name) + (revised ? ", driver 9" : ", drivers 1-8"));
      auto call = *md::parse_osi("SPY270115C00500000");
      if (c.deliverable) {
        call.root = "SPY1";
        call.standard = false;
      }
      auto time = md::new_york_to_utc({2026, 12, 17}, 10, 0);
      std::uint64_t observation = 0;
      TradingSession s(roomy(), time);
      s.set_revised_corporate_actions(revised);
      ASSERT_TRUE(s.define(call, time, c.deliverable).decision.ok());
      const auto quote = [&] {
        s.on_quotes({{call.osi_symbol(), ++observation, time, m(c.bid), m(c.ask), 100, 100}},
                    {{call.osi_symbol(), time, 0.9, 0.01, 0.2, -0.05, c.spot, c.spot, 0.99, md::years_between(time, call.expiry_time()), 0.2, true}},
                    time, {{"SPY", time, Money::from_double(c.spot)}});
      };
      quote();
      ASSERT_TRUE(s.submit({"sold", call.osi_symbol(), Side::Sell, OrderType::Market, TimeInForce::Ioc, 40, {}, {}, {}, {}}, time).decision.ok());
      time = md::new_york_to_utc({2026, 12, 17}, 16, 14, 30);
      quote();
      const auto night = md::new_york_to_utc({2026, 12, 17}, 18, 0);
      s.on_quotes({}, {}, night);
      const std::vector<Dividend> due{{"SPY", ex, m(c.dividend)}};
      const bool expected = revised ? c.assigned : !c.assigned;
      if (revised) {
        // The warning beforehand weighs it the same way.
        const auto warnings = s.warnings({}, due, c.splits);
        const bool warned = std::any_of(warnings.begin(), warnings.end(), [](const RiskWarning& w) { return w.code == "EARLY_ASSIGNMENT"; });
        EXPECT_EQ(warned, expected);
      }
      ASSERT_TRUE(s.roll_day(night, due, c.splits).decision.ok());
      EXPECT_EQ(!s.snapshot()->closures.empty(), expected);
    }
  }
}

}  // namespace
}  // namespace openport::trading
