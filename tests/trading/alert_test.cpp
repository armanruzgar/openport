#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"
#include "trading/state.hpp"

// F44: alerts on a contract, a spread, an underlying or the account, kept with it.
namespace openport::trading {
namespace {
using test::ScriptedMarket;
Money m(std::string_view s) { return Money::parse(s); }
SessionConfig roomy(AccountRules rules = {}) {
  SessionConfig c;
  c.limits.aggregate = {1e9, 1e9};
  c.limits.per_underlying = {1e9, 1e9};
  c.rules = std::move(rules);
  return c;
}
AlertSpec alert(AlertScope scope, std::string metric, std::string symbol, TriggerDirection direction, std::string_view level,
                bool repeat = false) {
  AlertSpec spec;
  spec.label = metric + " alert";
  spec.condition.scope = scope;
  spec.condition.metric = std::move(metric);
  spec.condition.symbol = std::move(symbol);
  spec.condition.direction = direction;
  spec.condition.level = m(level);
  spec.repeat = repeat;
  return spec;
}
void quote(TradingSession& s, ScriptedMarket& f, std::string_view bid, std::string_view ask, double delta = 0.5,
           const std::vector<Indicator>& indicators = {}) {
  f.next();
  s.on_quotes({f.quote(bid, ask)}, {f.valuation(delta)}, f.time, {}, indicators);
}
Alert find(const TradingSession& s, std::uint64_t id) {
  for (const auto& a : s.snapshot()->alerts) if (a.id == id) return a;
  throw std::runtime_error("no such alert");
}

TEST(TradingAlerts, AContractAlertFiresOnceOnItsBidAndAnnouncesIt) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto created = s.create_alert(alert(AlertScope::Contract, "bid", f.symbol(), TriggerDirection::AtOrAbove, "4.50"), f.time);
  ASSERT_TRUE(created.decision.ok()) << created.decision.message;
  ASSERT_TRUE(created.alert_id);
  EXPECT_EQ(*created.alert_id, 1U);
  EXPECT_TRUE(find(s, 1).armed);
  quote(s, f, "4.40", "4.60");
  EXPECT_EQ(find(s, 1).fired, 0U);
  quote(s, f, "4.50", "4.70");
  const auto& fired = find(s, 1);
  EXPECT_EQ(fired.fired, 1U);
  EXPECT_EQ(fired.fired_at, f.time);
  EXPECT_EQ(fired.value, m("4.50"));
  EXPECT_FALSE(fired.armed);
  quote(s, f, "4.40", "4.60");
  quote(s, f, "4.60", "4.80");
  EXPECT_EQ(find(s, 1).fired, 1U);  // one-shot
  EXPECT_TRUE(s.delete_alert(1, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->alerts.empty());
  EXPECT_EQ(s.delete_alert(1, f.time).decision.code, Reason::UNKNOWN_ALERT);
  // IDs are never reused.
  EXPECT_EQ(*s.create_alert(alert(AlertScope::Contract, "ask", f.symbol(), TriggerDirection::AtOrBelow, "1"), f.time).alert_id, 2U);
}

TEST(TradingAlerts, ARepeatingAlertRearmsWhenItsConditionLapsesAndGreeksAndIvAreWatched) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.create_alert(alert(AlertScope::Contract, "delta", f.symbol(), TriggerDirection::AtOrAbove, "0.6", true), f.time).decision.ok());
  ASSERT_TRUE(s.create_alert(alert(AlertScope::Contract, "iv", f.symbol(), TriggerDirection::AtOrAbove, "20"), f.time).decision.ok());
  // The valuation's smile IV of 0.20 is 20 vol points: already reached, so it fires at once.
  EXPECT_EQ(find(s, 2).fired, 1U);
  quote(s, f, "4.00", "4.20", 0.65);
  EXPECT_EQ(find(s, 1).fired, 1U);
  quote(s, f, "4.00", "4.20", 0.7);
  EXPECT_EQ(find(s, 1).fired, 1U);  // still inside: no repeat
  quote(s, f, "4.00", "4.20", 0.5);
  EXPECT_TRUE(find(s, 1).armed);
  quote(s, f, "4.00", "4.20", 0.61);
  EXPECT_EQ(find(s, 1).fired, 2U);
}

TEST(TradingAlerts, SpreadUnderlyingAndAccountAlertsReadTheirOwnValues) {
  ScriptedMarket f;
  AccountRules rules;
  rules.profit_target = m("5000");
  rules.max_drawdown = m("3000");
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  auto put = *md::parse_osi("SPXW261022P05000000");
  s.define(put, f.time);
  const auto put_quote = [&](std::string_view bid, std::string_view ask) {
    return QuoteObservation{put.osi_symbol(), f.observation, f.time, m(bid), m(ask), 10, 10};
  };
  s.on_quotes({put_quote("3.00", "3.20")}, {}, f.time);
  // Long the call, short the put: a net mark of 4.10 - 3.10 = 1.00.
  auto spread = alert(AlertScope::Spread, "mark", "", TriggerDirection::AtOrAbove, "1.50");
  spread.condition.legs = {{f.symbol(), Side::Buy, 1}, {put.osi_symbol(), Side::Sell, 1}};
  ASSERT_TRUE(s.create_alert(spread, f.time).decision.ok());
  ASSERT_TRUE(s.create_alert(alert(AlertScope::Underlying, "price", "VIX", TriggerDirection::AtOrAbove, "25"), f.time).decision.ok());
  ASSERT_TRUE(s.create_alert(alert(AlertScope::Account, "floor_room", "", TriggerDirection::AtOrBelow, "2900"), f.time).decision.ok());
  ASSERT_TRUE(s.create_alert(alert(AlertScope::Account, "day_pnl", "", TriggerDirection::AtOrBelow, "-50"), f.time).decision.ok());
  for (std::uint64_t id = 1; id <= 4; ++id) { EXPECT_EQ(find(s, id).fired, 0U) << id; }
  ASSERT_TRUE(s.submit(f.market("open", 1), f.time).decision.ok());  // buys at 4.20, marked at 4.10
  f.next();
  s.on_quotes({f.quote("3.50", "3.70"), put_quote("2.00", "2.20")}, {f.valuation()}, f.time, {},
              {{"VIX", "", f.time, m("26")}});
  // Spread: 3.60 - 2.10 = 1.50. Day P&L: (3.60 - 4.20) * 100 - 0.65 = -60.65.
  EXPECT_EQ(find(s, 1).fired, 1U);
  EXPECT_EQ(find(s, 1).value, m("1.50"));
  EXPECT_EQ(find(s, 2).fired, 1U);
  EXPECT_EQ(find(s, 3).fired, 0U);  // floor room is still 2,939.35
  EXPECT_EQ(find(s, 4).fired, 1U);
  EXPECT_EQ(find(s, 4).value, m("-60.65"));
}

TEST(TradingAlerts, TermsAreCheckedAndAlertsAreJournaledOnlyOnceUsedAndRecover) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-alerts-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string head;
  {
    auto journal = FileJournal::create(path);
    TradingSession s(roomy(), f.time, journal);
    f.seed(s);
    {
      std::ifstream in(path);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(text.find("alerts"), std::string::npos);
    }
    const auto invalid = [&](AlertSpec spec) {
      try { (void)s.create_alert(std::move(spec), f.time); } catch (const TradingError& e) { return e.code() == Reason::INVALID_ALERT; }
      return false;
    };
    EXPECT_TRUE(invalid(alert(AlertScope::Contract, "spot", f.symbol(), TriggerDirection::AtOrAbove, "1")));
    EXPECT_TRUE(invalid(alert(AlertScope::Account, "equity", "SPX", TriggerDirection::AtOrAbove, "1")));
    EXPECT_TRUE(invalid(alert(AlertScope::Underlying, "iv30", "", TriggerDirection::AtOrAbove, "1")));
    EXPECT_TRUE(invalid(alert(AlertScope::Spread, "mark", "", TriggerDirection::AtOrAbove, "1")));  // no legs
    auto long_label = alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrAbove, "1");
    long_label.label = std::string(101, 'x');
    EXPECT_TRUE(invalid(long_label));
    EXPECT_EQ(s.create_alert(alert(AlertScope::Contract, "bid", "SPXW  261022C09000000", TriggerDirection::AtOrAbove, "1"), f.time).decision.code,
              Reason::UNKNOWN_CONTRACT);
    ASSERT_TRUE(s.create_alert(alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrAbove, "200000"), f.time).decision.ok());
    ASSERT_TRUE(s.create_alert(alert(AlertScope::Contract, "mark", f.symbol(), TriggerDirection::AtOrAbove, "4.50"), f.time).decision.ok());
    quote(s, f, "4.40", "4.60");
    EXPECT_EQ(find(s, 2).fired, 1U);
    head = journal->head();
  }
  auto recovered = TradingSession::recover(FileJournal::read(path, head), FileJournal::resume(path));
  ASSERT_EQ(recovered.snapshot()->alerts.size(), 2U);
  EXPECT_EQ(find(recovered, 2).fired, 1U);
  EXPECT_FALSE(find(recovered, 2).armed);
  EXPECT_EQ(find(recovered, 2).fired_at, f.time);
  EXPECT_EQ(find(recovered, 2).value, m("4.50"));
  EXPECT_TRUE(find(recovered, 1).armed);
  EXPECT_EQ(find(recovered, 1).spec.label, "equity alert");
  EXPECT_EQ(*recovered.create_alert(alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrBelow, "1"), f.time).alert_id, 3U);
  std::filesystem::remove_all(directory);
}

TEST(TradingAlerts, EveryMetricUsesItsDocumentedUnits) {
  ScriptedMarket f;
  AccountRules rules;
  rules.max_drawdown = m("3000");
  TradingSession s(roomy(rules), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  const auto fires = [&](AlertScope scope, const std::string& metric, const std::string& symbol, Money value) {
    auto spec = alert(scope, metric, symbol, TriggerDirection::AtOrAbove, "0");
    spec.condition.level = value;
    const auto created = s.create_alert(spec, f.time);
    ASSERT_TRUE(created.alert_id) << metric;
    EXPECT_EQ(find(s, *created.alert_id).fired, 1U) << metric;
    EXPECT_EQ(find(s, *created.alert_id).value, value) << metric;
  };
  for (const auto& [metric, value] : std::map<std::string, std::string>{
      {"bid", "4"}, {"ask", "4.2"}, {"mark", "4.1"}, {"iv", "20"},
      {"delta", "0.5"}, {"gamma", "0.001"}, {"theta", "-0.1"}, {"vega", "2"}})
    fires(AlertScope::Contract, metric, f.symbol(), m(value));
  const auto snapshot = s.snapshot();
  for (const auto& [metric, value] : std::map<std::string, Money>{
      {"equity", snapshot->equity}, {"day_pnl", snapshot->equity - snapshot->start_of_day_equity},
      {"unrealised", snapshot->unrealised}, {"floor_room", snapshot->equity - snapshot->evaluation.floor},
      {"buying_power", snapshot->buying_power.available}, {"dollar_delta", m("250000")}, {"vega", m("200")}, {"theta", m("-10")}})
    fires(AlertScope::Account, metric, "", value);
  f.next();
  s.on_quotes({}, {}, f.time, {}, {{"SPX", "", f.time, m("5000")}, {"SPX", "iv7", f.time, m("22")},
                                 {"SPX", "iv30", f.time, m("20")}, {"SPX", "term_ratio", f.time, m("1.1")}});
  for (const auto& [metric, value] : std::map<std::string, std::string>{
      {"price", "5000"}, {"iv7", "22"}, {"iv30", "20"}, {"term_ratio", "1.1"}})
    fires(AlertScope::Underlying, metric, "SPX", m(value));
}

TEST(TradingAlerts, StaleOrMissingValuesNeitherFireNorRearm) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  ASSERT_TRUE(s.create_alert(alert(AlertScope::Contract, "delta", f.symbol(), TriggerDirection::AtOrAbove, "0.4", true), f.time).alert_id);
  EXPECT_EQ(find(s, 1).fired, 1U);
  f.time += 3 * md::kNanosPerMinute;
  auto stale = f.valuation(0.3);
  stale.time -= 2 * md::kNanosPerMinute;
  s.on_quotes({}, {stale}, f.time);
  EXPECT_FALSE(find(s, 1).armed);
  // All of these would hold using the old values, but none is fresh now.
  for (const auto& spec : {
      alert(AlertScope::Contract, "mark", f.symbol(), TriggerDirection::AtOrAbove, "1"),
      alert(AlertScope::Contract, "ask", f.symbol(), TriggerDirection::AtOrAbove, "1"),
      alert(AlertScope::Contract, "iv", f.symbol(), TriggerDirection::AtOrAbove, "1"),
      alert(AlertScope::Underlying, "price", "SPX", TriggerDirection::AtOrAbove, "1"),
      alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrAbove, "1"),
      alert(AlertScope::Account, "buying_power", "", TriggerDirection::AtOrAbove, "1"),
      alert(AlertScope::Account, "vega", "", TriggerDirection::AtOrAbove, "1")}) {
    const auto id = s.create_alert(spec, f.time).alert_id;
    ASSERT_TRUE(id);
    EXPECT_EQ(find(s, *id).fired, 0U) << spec.condition.metric;
  }
  auto put = *md::parse_osi("SPXW261022P05000000");
  s.define(put, f.time);
  auto spread = alert(AlertScope::Spread, "mark", "", TriggerDirection::AtOrAbove, "-100");
  spread.condition.legs = {{f.symbol(), Side::Buy, 1}, {put.osi_symbol(), Side::Sell, 1}};
  const auto spread_id = s.create_alert(spread, f.time).alert_id;
  ASSERT_TRUE(spread_id);
  // A fresh value still inside the condition must not fire a repeating alert twice.
  quote(s, f, "4", "4.2", 0.5);
  EXPECT_EQ(find(s, 1).fired, 1U);
  EXPECT_EQ(find(s, *spread_id).fired, 0U);  // missing put mark
  quote(s, f, "4", "4.2", 0.3);
  EXPECT_TRUE(find(s, 1).armed);
  auto invalid = f.valuation();
  invalid.valid = false;
  f.next();
  invalid.time = f.time;
  s.on_quotes({}, {invalid}, f.time);
  EXPECT_EQ(find(s, 1).fired, 1U);
  quote(s, f, "4", "4.2", 0.5);
  EXPECT_EQ(find(s, 1).fired, 2U);
}

TEST(TradingAlerts, OneHundredAlertsAreAllowedAndDeletingFreesSpaceWithoutReusingIds) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  const auto spec = alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrBelow, "0");
  for (std::uint64_t id = 1; id <= 100; ++id) { EXPECT_EQ(s.create_alert(spec, f.time).alert_id, id); }
  EXPECT_EQ(s.create_alert(spec, f.time).decision.code, Reason::INVALID_ALERT);
  ASSERT_TRUE(s.delete_alert(50, f.time).decision.ok());
  EXPECT_EQ(s.create_alert(spec, f.time).alert_id, 101U);
  EXPECT_EQ(s.snapshot()->alerts.size(), 100U);
}

TEST(TradingAlerts, AStaleSpreadLegAndIndicatorCannotFireOrRearm) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  const auto put = *md::parse_osi("SPXW261022P05000000");
  s.define(put, f.time);
  s.on_quotes({{put.osi_symbol(), 1, f.time, m("3"), m("3.2"), 10, 10}}, {}, f.time);
  auto spread = alert(AlertScope::Spread, "mark", "", TriggerDirection::AtOrAbove, "2");
  spread.condition.legs = {{f.symbol(), Side::Buy, 1}, {put.osi_symbol(), Side::Sell, 1}};
  s.create_alert(spread, f.time);
  s.create_alert(alert(AlertScope::Underlying, "iv30", "SPX", TriggerDirection::AtOrAbove, "20", true), f.time);
  f.time += 3 * md::kNanosPerMinute;
  quote(s, f, "6", "6.2", 0.5, {{"SPX", "iv30", f.time - md::kNanosPerMinute * 2, m("25")}});
  EXPECT_EQ(find(s, 1).fired, 0U);  // the put's old mark cannot price the spread
  EXPECT_EQ(find(s, 2).fired, 0U);
  s.on_quotes({{put.osi_symbol(), 2, f.time, m("3"), m("3.2"), 10, 10}}, {}, f.time, {},
              {{"SPX", "iv30", f.time, m("25")}});
  EXPECT_EQ(find(s, 1).value, m("3"));
  EXPECT_EQ(find(s, 2).fired, 1U);
  f.time += 3 * md::kNanosPerMinute;
  s.on_quotes({}, {}, f.time, {}, {{"SPX", "iv30", f.time - 2 * md::kNanosPerMinute, m("15")}});
  EXPECT_FALSE(find(s, 2).armed);
  f.next();
  s.on_quotes({}, {}, f.time, {}, {{"SPX", "iv30", f.time, m("25")}});
  EXPECT_EQ(find(s, 2).fired, 1U);
}

TEST(TradingAlerts, InvalidQuotesDoNotFireAndAccountAlertsSurviveAResetAndKill) {
  ScriptedMarket f;
  TradingSession s(roomy(), f.time);
  f.seed(s);
  s.create_alert(alert(AlertScope::Contract, "bid", f.symbol(), TriggerDirection::AtOrAbove, "5"), f.time);
  f.next();
  s.on_quotes({f.quote("6", "4")}, {}, f.time);  // crossed
  EXPECT_EQ(find(s, 1).fired, 0U);
  s.create_alert(alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrAbove, "110000"), f.time);
  ASSERT_TRUE(s.trip_kill("testing", f.time).decision.ok());
  ASSERT_TRUE(s.reset_account(m("120000"), {}, "new attempt", f.time).decision.ok());
  EXPECT_EQ(find(s, 2).fired, 0U);
  f.next();
  s.on_quotes({}, {}, f.time);  // an otherwise idle account still checks its alerts
  EXPECT_EQ(find(s, 2).fired, 1U);
}

TEST(TradingAlerts, JournalsWithoutAlertsKeepTheirPreFeatureBytes) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-alert-bytes-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const auto path = pattern + "/journal.jsonl";
  {
    // This head was captured from F43, before alerts existed, with these inputs.
    ScriptedMarket f;
    auto journal = FileJournal::create(path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    s.submit(f.limit("resting", 1, "3.50"), f.time);
    f.next();
    s.on_quotes({f.quote("3.20", "3.40")}, {f.valuation()}, f.time);
    s.trip_kill("test", f.time);
    EXPECT_EQ(journal->head(), "aea5515892f85a6937a8a782fc9811458b0919ec8a2060dfa8e99f769c091717");
    EXPECT_FALSE(Json::parse(s.snapshot_json()).contains("alerts"));
  }
  EXPECT_TRUE(TradingSession::recover(FileJournal::read(path)).snapshot()->alerts.empty());
  std::filesystem::remove_all(pattern);
}

TEST(TradingAlerts, RepeatingAlertsAndDeletedBooksRecoverAndReplayDeterministically) {
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-alert-replay-XXXXXX").string();
  ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
  const auto run = [&](const std::string& path) {
    ScriptedMarket f;
    {
      auto journal = FileJournal::create(path);
      TradingSession s(roomy(), f.time, journal);
      f.seed(s);
      s.set_actor("trader");
      s.create_alert(alert(AlertScope::Contract, "mark", f.symbol(), TriggerDirection::AtOrAbove, "4", true), f.time);
      EXPECT_EQ(find(s, 1).actor, "trader");
    }
    auto journal = FileJournal::resume(path);
    auto s = TradingSession::recover(FileJournal::read(path), journal);
    EXPECT_EQ(find(s, 1).fired, 1U);
    EXPECT_FALSE(find(s, 1).armed);
    quote(s, f, "3", "3.2");
    EXPECT_TRUE(find(s, 1).armed);
    quote(s, f, "4", "4.2");
    EXPECT_EQ(find(s, 1).fired, 2U);
    EXPECT_EQ(Json::parse(s.snapshot_json()).get<TradingSnapshot>().alerts[0].fired, 2U);
    s.delete_alert(1, f.time);
    return journal->head();
  };
  const auto first = pattern + "/first.jsonl", second = pattern + "/second.jsonl";
  EXPECT_EQ(run(first), run(second));
  auto recovered = TradingSession::recover(FileJournal::read(first));
  EXPECT_TRUE(recovered.snapshot()->alerts.empty());
  EXPECT_EQ(recovered.create_alert(alert(AlertScope::Account, "equity", "", TriggerDirection::AtOrAbove, "0"),
                                   recovered.snapshot()->time).alert_id, 2U);
  std::filesystem::remove_all(pattern);
}

TEST(TradingAlerts, RiskMetricsUseCompleteValuationsAndTheExistingRepeatLifecycle) {
  test::ScriptedMarket f;
  auto config = roomy(); config.limits.aggregate.dollar_delta = 1000000; config.limits.aggregate.vega = 10000;
  TradingSession session(config, f.time); f.seed(session);
  for (const auto* metric : {"delta_utilisation", "vega_utilisation", "dollar_gamma_1pct", "grid_loss"})
    ASSERT_TRUE(session.create_alert(alert(AlertScope::Account, metric, "", TriggerDirection::AtOrAbove, "0.001", true), f.time).decision.ok());
  ASSERT_TRUE(session.submit(f.market("entry", 1), f.time).decision.ok());
  quote(session, f, "4", "4.20");
  for (std::uint64_t id = 1; id <= 4; ++id) EXPECT_EQ(find(session, id).fired, 1U);
  EXPECT_EQ(find(session, 1).value, m("0.25"));
  ASSERT_TRUE(session.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
  quote(session, f, "4", "4.20");
  ASSERT_TRUE(session.submit(f.market("again", 1), f.time).decision.ok());
  quote(session, f, "4", "4.20");
  EXPECT_EQ(find(session, 1).fired, 2U);
  // A new valuation-dependent alert does not fire against stale analytics.
  f.time += 2 * md::kNanosPerMinute;
  ASSERT_TRUE(session.create_alert(alert(AlertScope::Account, "vega", "", TriggerDirection::AtOrAbove, "0"), f.time).decision.ok());
  EXPECT_EQ(find(session, 5).fired, 0U);
}
TEST(TradingAlerts, FloorOddsNeedObservedVarianceAndAreUnavailableWithoutAFloor) {
  test::ScriptedMarket f;
  auto config = roomy(); config.rules.max_drawdown = m("1000");
  TradingSession session(config, f.time); f.seed(session);
  ASSERT_TRUE(session.submit(f.market("entry", 2), f.time).decision.ok());
  ASSERT_TRUE(session.create_alert(alert(AlertScope::Account, "floor_touch_probability", "", TriggerDirection::AtOrAbove, "0"), f.time).decision.ok());
  EXPECT_EQ(find(session, 1).fired, 0U);
  f.next(); session.on_quotes({f.quote("4", "4.20")}, {f.valuation()}, f.time, {}, {{"SPX", "close_sigma", f.time, m("2")}});
  EXPECT_EQ(find(session, 1).fired, 1U);
  TradingSession no_floor(roomy(), f.time); f.seed(no_floor);
  ASSERT_TRUE(no_floor.create_alert(alert(AlertScope::Account, "floor_touch_probability", "", TriggerDirection::AtOrAbove, "0"), f.time).decision.ok());
  EXPECT_EQ(find(no_floor, 1).fired, 0U);
}
}  // namespace
}  // namespace openport::trading
