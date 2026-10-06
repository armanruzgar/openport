#include <gtest/gtest.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view value) { return Money::parse(value); }
SessionConfig config() {
  SessionConfig c;
  c.initial_cash = m("10000");
  c.rules.max_drawdown = m("1000");
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.limits.price_band_absolute = m("20");
  return c;
}
const RiskWarning* find(const std::vector<RiskWarning>& warnings, std::string_view code) {
  for (const auto& w : warnings) if (w.code == code) return &w;
  return nullptr;
}
/// SPY options, with SPY's own price in every batch.
struct Spy {
  md::Timestamp time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  std::uint64_t observation = 0;
  double spot = 510;
  void quote(TradingSession& s, const md::OptionContract& c, std::string_view bid, std::string_view ask) {
    ++observation;
    s.on_quotes({{c.osi_symbol(), observation, time, m(bid), m(ask), 10, 10}},
                {{c.osi_symbol(), time, c.type == pricing::OptionType::Call ? 0.6 : -0.4, 0.02, 0.5, -0.05, spot, spot, 0.99,
                  md::years_between(time, c.expiry_time()), 0.2, true}},
                time, {{"SPY", time, Money::from_double(spot)}});
  }
  OrderRequest market(std::string client, const md::OptionContract& c, Quantity quantity, Side side = Side::Buy) const {
    return {std::move(client), c.osi_symbol(), side, OrderType::Market, TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  }
};

TEST(TradingWarnings, ABookOverALimitOrNearOneByGammaIsWarned) {
  ScriptedMarket f; TradingSession s(config(), f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  EXPECT_TRUE(s.warnings().empty());
  const auto book = s.snapshot()->risk.underlyings.at("SPX").position;
  // Per 1% move, dollar delta changes by dollar gamma and by 1% of itself.
  const double rate = book.dollar_gamma_1pct + 0.01 * book.dollar_delta;
  ASSERT_GT(rate, 0);
  // A plan's limits tighten at once, so from the loosest: three percent away is
  // past 1%, but within a 4% move to the close.
  auto limits = s.config().limits;
  limits.per_underlying.dollar_delta = book.dollar_delta + 3 * rate;
  ASSERT_TRUE(s.set_limits(limits, f.time).decision.ok());
  EXPECT_EQ(find(s.warnings(), "DELTA_HEADROOM"), nullptr);
  const auto wide = s.warnings({{"SPX", 0.04 * 0.04}});
  ASSERT_NE(find(wide, "DELTA_HEADROOM"), nullptr);
  EXPECT_NEAR(*find(wide, "DELTA_HEADROOM")->limit, 4, 1e-9);
  // Closer, a rise of less than 1% reaches it by gamma.
  limits.per_underlying.dollar_delta = book.dollar_delta + 0.5 * rate;
  ASSERT_TRUE(s.set_limits(limits, f.time).decision.ok());
  auto warnings = s.warnings();
  EXPECT_EQ(find(warnings, "DELTA_LIMIT"), nullptr);
  const auto* near = find(warnings, "DELTA_HEADROOM");
  ASSERT_NE(near, nullptr);
  EXPECT_EQ(near->severity, "warning");
  EXPECT_NEAR(*near->actual, 0.5, 1e-9);
  EXPECT_EQ(*near->limit, 1.0);
  EXPECT_NE(near->message.find("rise in SPX"), std::string::npos) << near->message;
  // Over the underlying's limit: orders that add are refused, closes still go.
  limits.per_underlying.dollar_delta = book.dollar_delta - 100'000;
  ASSERT_TRUE(s.set_limits(limits, f.time).decision.ok());
  warnings = s.warnings();
  const auto* over = find(warnings, "DELTA_LIMIT");
  ASSERT_NE(over, nullptr);
  EXPECT_EQ(over->scope, "SPX");
  EXPECT_EQ(over->severity, "warning");
  EXPECT_DOUBLE_EQ(*over->actual, book.dollar_delta);
  EXPECT_DOUBLE_EQ(*over->limit, book.dollar_delta - 100'000);
  EXPECT_EQ(find(warnings, "DELTA_HEADROOM"), nullptr);
}

TEST(TradingWarnings, TonightsRatchetAndASoftFloorAboveEquityAreWarned) {
  ScriptedMarket f; auto c = config(); c.rules.drawdown_mode = DrawdownMode::EndOfDay;
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long", 2), f.time).decision.ok());
  EXPECT_EQ(find(s.warnings(), "FLOOR_RATCHET"), nullptr);
  // The calls rise to 7.10: equity is above the 10,000 high-water mark.
  f.next(); s.on_quotes({f.quote("7.00", "7.20")}, {f.valuation()}, f.time);
  const auto equity = s.snapshot()->equity;
  ASSERT_GT(equity, m("10000"));
  auto warnings = s.warnings();
  const auto* ratchet = find(warnings, "FLOOR_RATCHET");
  ASSERT_NE(ratchet, nullptr);
  EXPECT_EQ(ratchet->severity, "info");
  EXPECT_DOUBLE_EQ(*ratchet->actual, (equity - m("1000")).dollars());
  EXPECT_DOUBLE_EQ(*ratchet->limit, 9000);
  EXPECT_EQ(find(warnings, "SOFT_FLOOR_ROLLOVER"), nullptr);
  // The highest allowed new percentage keeps $10 of room after tonight's ratchet.
  // Historical 100 settings still recover, but new writes cannot latch at the peak.
  Guardrails g; g.soft_floor_percent = 99;
  ASSERT_TRUE(s.set_guardrails(g, f.time).decision.ok());
  warnings = s.warnings();
  EXPECT_EQ(find(warnings, "SOFT_FLOOR"), nullptr);
  EXPECT_EQ(find(warnings, "SOFT_FLOOR_ROLLOVER"), nullptr);
  // A soft floor above equity now closes the positions; the warning stays until rollover.
  g.soft_floor = equity + m("100");
  ASSERT_TRUE(s.set_guardrails(g, f.time).decision.ok());
  warnings = s.warnings();
  const auto* soft = find(warnings, "SOFT_FLOOR");
  ASSERT_NE(soft, nullptr);
  EXPECT_DOUBLE_EQ(*soft->limit, (equity + m("100")).dollars());
  EXPECT_EQ(find(warnings, "SOFT_FLOOR_ROLLOVER"), nullptr);
}

TEST(TradingWarnings, LargeFiniteLimitsPrintWithoutIntegerOverflow) {
  ScriptedMarket f; auto c = config();
  c.limits.aggregate.dollar_delta = 1e300;
  c.limits.per_underlying.dollar_delta = 1e300;
  TradingSession s(c, f.time); f.seed(s);
  ASSERT_TRUE(s.submit(f.market("long"), f.time).decision.ok());
  EXPECT_TRUE(s.warnings().empty());
  // Large gamma brings the large limit within reach, so its text is published.
  f.next(); auto valuation = f.valuation(); valuation.gamma = 1e294;
  s.on_quotes({f.quote()}, {valuation}, f.time);
  const auto warnings = s.warnings();
  const auto* near = find(warnings, "DELTA_HEADROOM");
  ASSERT_NE(near, nullptr);
  const std::string prefix = "to its ";
  const auto start = near->message.find(prefix);
  ASSERT_NE(start, std::string::npos) << near->message;
  const auto end = near->message.find(" limit", start);
  ASSERT_NE(end, std::string::npos) << near->message;
  auto digits = near->message.substr(start + prefix.size(), end - start - prefix.size());
  EXPECT_NE(digits.find(','), std::string::npos);
  std::erase(digits, ',');
  EXPECT_EQ(digits.size(), 301);
  EXPECT_EQ(digits.find_first_not_of("0123456789"), std::string::npos);
  EXPECT_DOUBLE_EQ(std::stod(digits), 1e300);
}

TEST(TradingWarnings, InTheMoneyEquityOptionsExpiringTodaySayWhatDeliveryLeaves) {
  const auto call = *md::parse_osi("SPY260922C00500000");  // expires today
  const auto put = *md::parse_osi("SPY260922P00500000");
  const auto later = *md::parse_osi("SPY261022C00500000");
  Spy f; auto c = config(); c.initial_cash = m("100000");
  TradingSession s(c, f.time);
  for (const auto& contract : {call, put, later}) {
    ASSERT_TRUE(s.define(contract, f.time).decision.ok());
  }
  f.quote(s, call, "10.00", "10.20");
  f.quote(s, put, "0.10", "0.20");
  f.quote(s, later, "14.00", "14.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 2), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("puts", put, 1), f.time).decision.ok());
  ASSERT_TRUE(s.submit(f.market("later", later, 1), f.time).decision.ok());
  const auto warnings = s.warnings();
  // Only the calls: the put is out of the money, the later call does not expire today.
  ASSERT_EQ(std::count_if(warnings.begin(), warnings.end(), [](const auto& w) { return w.code == "EXPIRY_DELIVERY"; }), 1);
  const auto* delivery = find(warnings, "EXPIRY_DELIVERY");
  EXPECT_EQ(delivery->symbol, call.osi_symbol());
  EXPECT_EQ(delivery->scope, "SPY");
  EXPECT_NE(delivery->message.find("buying 200 SPY shares at the strike ($100000.00)"), std::string::npos) << delivery->message;
  // 200 shares at 510 cost far more than the 100,000 account holds.
  EXPECT_EQ(delivery->severity, "warning");
  EXPECT_LT(*delivery->actual, 0);
  // Index options settle in cash.
  ScriptedMarket spx; spx.contract = *md::parse_osi("SPXW260922C05000000");
  TradingSession index(config(), spx.time); spx.seed(index, "10.00", "10.20");
  ASSERT_TRUE(index.submit(spx.market("index"), spx.time).decision.ok());
  EXPECT_EQ(find(index.warnings(), "EXPIRY_DELIVERY"), nullptr);
}

TEST(TradingWarnings, FundedDeliveryIsInformationalAndDoNotExerciseSuppressesIt) {
  const auto call = *md::parse_osi("SPY260922C00500000");
  Spy f; auto c = config(); c.initial_cash = m("100000"); c.rules.buying_power = true;
  TradingSession s(c, f.time);
  ASSERT_TRUE(s.define(call, f.time).decision.ok());
  f.quote(s, call, "10", "10.20");
  ASSERT_TRUE(s.submit(f.market("call", call, 1), f.time).decision.ok());
  const auto warnings = s.warnings();
  const auto* delivery = find(warnings, "EXPIRY_DELIVERY");
  ASSERT_NE(delivery, nullptr);
  EXPECT_EQ(delivery->severity, "info");
  EXPECT_EQ(delivery->actual, 48979.35);
  EXPECT_EQ(delivery->message.find("shortfall"), std::string::npos);
  ASSERT_TRUE(s.instruct_exercise(call.osi_symbol(), true, f.time).decision.ok());
  EXPECT_EQ(find(s.warnings(), "EXPIRY_DELIVERY"), nullptr);
}

TEST(TradingWarnings, ExpiryDeliveryNamesTheCutoffUntilItPasses) {
  const auto call = *md::parse_osi("SPY260922C00500000");
  Spy f; f.time = md::new_york_to_utc({2026, 9, 22}, 16, 8);
  auto c = config(); c.initial_cash = m("100000");
  c.rules.expiry_cutoff = 5 * md::kNanosPerMinute;
  c.rules.buying_power = true;
  TradingSession s(c, f.time);
  ASSERT_TRUE(s.define(call, f.time).decision.ok());
  f.quote(s, call, "10.00", "10.20");
  ASSERT_TRUE(s.submit(f.market("calls", call, 2), f.time).decision.ok());
  auto warnings = s.warnings();
  const auto* delivery = find(warnings, "EXPIRY_DELIVERY");
  ASSERT_NE(delivery, nullptr);
  EXPECT_EQ(delivery->severity, "warning");
  EXPECT_NE(delivery->message.find("Openings that use buying power are refused"), std::string::npos);
  EXPECT_NE(delivery->message.find("pre-expiry cutoff at 16:10:00 ET at the market"), std::string::npos) << delivery->message;
  EXPECT_NE(delivery->message.find("only contracts still held into expiry because a close cannot fill are exercised"),
            std::string::npos) << delivery->message;
  EXPECT_NE(delivery->message.find("buying 200 SPY shares at the strike ($100000.00)"), std::string::npos) << delivery->message;
  ASSERT_TRUE(delivery->actual.has_value());
  const auto power = *delivery->actual;
  EXPECT_LT(power, 0);
  EXPECT_EQ(delivery->limit, 0.0);
  // With no fresh option quote the cutoff cannot close; delivery remains a risk.
  for (const auto time : {call.last_trade_time() - c.rules.expiry_cutoff, call.last_trade_time() - md::kNanosPerMinute}) {
    f.time = time;
    s.on_quotes({}, {}, f.time, {{"SPY", f.time, m("510")}});
    warnings = s.warnings();
    delivery = find(warnings, "EXPIRY_DELIVERY");
    ASSERT_NE(delivery, nullptr);
    EXPECT_EQ(delivery->severity, "warning");
    EXPECT_NE(delivery->message.find("held into expiry they are exercised"), std::string::npos) << delivery->message;
    EXPECT_EQ(delivery->message.find("pre-expiry cutoff"), std::string::npos);
    EXPECT_EQ(delivery->actual, power);
    EXPECT_EQ(delivery->limit, 0.0);
  }
  f.quote(s, call, "10.00", "10.20");
  EXPECT_TRUE(s.snapshot()->positions.empty());
  EXPECT_EQ(find(s.warnings(), "EXPIRY_DELIVERY"), nullptr);
}

TEST(TradingWarnings, EarlyAssignmentAndExDatesAreWarned) {
  const auto call = *md::parse_osi("SPY270115C00500000");
  Spy f; f.time = md::new_york_to_utc({2026, 12, 17}, 10, 0); f.spot = 520;
  auto c = config(); c.initial_cash = m("100000"); c.rules = {};
  TradingSession s(c, f.time);
  ASSERT_TRUE(s.define(call, f.time).decision.ok());
  // At 20.10 against 20 of exercise value, the call keeps 0.10 of time value.
  f.quote(s, call, "20.00", "20.20");
  ASSERT_TRUE(s.submit(f.market("sold", call, 10, Side::Sell), f.time).decision.ok());
  EXPECT_EQ(find(s.warnings(), "EARLY_ASSIGNMENT"), nullptr);
  // A 1.90 dividend going ex tomorrow: holders exercise tonight.
  auto warnings = s.warnings({}, {{"SPY", {2026, 12, 18}, m("1.90")}});
  const auto* assignment = find(warnings, "EARLY_ASSIGNMENT");
  ASSERT_NE(assignment, nullptr);
  EXPECT_EQ(assignment->severity, "warning");
  EXPECT_EQ(assignment->symbol, call.osi_symbol());
  EXPECT_DOUBLE_EQ(*assignment->actual, 0.10);
  EXPECT_DOUBLE_EQ(*assignment->limit, 1.90);
  EXPECT_NE(assignment->message.find("tonight"), std::string::npos) << assignment->message;
  const auto* ex = find(warnings, "EX_DIVIDEND");
  ASSERT_NE(ex, nullptr);
  EXPECT_EQ(ex->severity, "info");
  EXPECT_EQ(ex->scope, "SPY");
  // One going ex next week is a night further off; one below the time value is none.
  warnings = s.warnings({}, {{"SPY", {2026, 12, 22}, m("1.90")}});
  ASSERT_NE(find(warnings, "EARLY_ASSIGNMENT"), nullptr);
  EXPECT_EQ(find(warnings, "EARLY_ASSIGNMENT")->severity, "info");
  EXPECT_EQ(find(s.warnings({}, {{"SPY", {2026, 12, 18}, m("0.05")}}), "EARLY_ASSIGNMENT"), nullptr);
  // Past a week it is not mentioned.
  EXPECT_TRUE(s.warnings({}, {{"SPY", {2027, 1, 8}, m("1.90")}}).empty());
  // Marked below its exercise value, it is exercised whatever the dividend.
  f.quote(s, call, "19.00", "19.20");
  warnings = s.warnings();
  const auto* below = find(warnings, "EARLY_ASSIGNMENT");
  ASSERT_NE(below, nullptr);
  EXPECT_DOUBLE_EQ(*below->actual, 19.10);
  EXPECT_DOUBLE_EQ(*below->limit, 20);
}
}  // namespace
}  // namespace openport::trading
