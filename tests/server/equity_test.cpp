#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include "openport/server/equity.hpp"
#include "support/scripted_market.hpp"

namespace openport::server {
namespace {
using trading::Money;
Money m(std::string_view value) { return Money::parse(value); }
// Made by mkdtemp: a name from the clock alone was shared by test processes that
// CTest started in the same microsecond, which then read or deleted each other's rows.
struct Directory {
  std::filesystem::path path = make();
  ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
  static std::filesystem::path make() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-equity-XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("cannot create an equity test directory");
    return pattern;
  }
};
std::string bytes_of(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
std::size_t lines_of(const std::filesystem::path& file) {
  std::ifstream input(file);
  std::size_t count = 0;
  for (std::string row; std::getline(input, row);) ++count;
  return count;
}
TEST(EquityStore, PersistsMinuteAndFillSamplesReloadsAndLeavesGaps) {
  Directory dir;
  const auto file = dir.path / "account.jsonl.equity.csv";
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  EquitySample sample{time, 1, m("10000"), m("9000"), m("10000"), m("11000"), {}, 0};
  {
    EquityStore store(file);
    store.append(sample);
    sample.time += md::kNanosPerSecond; store.append(sample);
    EXPECT_EQ(store.samples().size(), 1U);
    sample.fill = 1; sample.equity = m("9989.35"); store.append(sample); store.append(sample);
    sample.fill = 2; store.append(sample);
    ASSERT_EQ(store.samples().size(), 3U);
    EXPECT_TRUE(store.error().empty());
  }
  EquityStore recovered(file);
  ASSERT_EQ(recovered.samples().size(), 3U);
  EXPECT_EQ(recovered.samples()[1].equity, m("9989.35"));
  // I-29: a fill already kept, here the reloaded fill 1, is known without a scan.
  sample.fill = 1; recovered.append(sample);
  ASSERT_EQ(recovered.samples().size(), 3U);
  // A publication's copy shares the history and keeps what it saw.
  const auto published = recovered.samples();
  EXPECT_TRUE(published.same(recovered.samples()));
  sample.time += 10 * md::kNanosPerMinute; sample.fill = 0; recovered.append(sample);
  ASSERT_EQ(recovered.samples().size(), 4U);
  EXPECT_EQ(published.size(), 3U);
  EXPECT_EQ(recovered.samples().back().time, sample.time);
  EXPECT_FALSE(recovered.samples().back().tomorrow_floor);
}
TEST(EquityStore, CompactsOldAttemptsButKeepsCurrentAndDoesNotThrowOnStorageFailure) {
  Directory dir;
  const auto file = dir.path / "history.csv";
  const auto time = md::new_york_to_utc({2026, 1, 2}, 10, 0);
  EquityStore store(file);
  EquitySample sample{time, 1, m("10000"), {}, m("10000"), {}, {}, 0};
  store.append(sample);
  sample.time += 100 * md::kNanosPerDay; store.append(sample);
  EXPECT_EQ(store.samples().size(), 2U);
  sample.attempt = 2; sample.time += md::kNanosPerMinute; store.append(sample);
  EXPECT_EQ(store.samples().size(), 2U);
  EquityStore failed(dir.path); // A directory cannot be an appendable CSV.
  EXPECT_NO_THROW(failed.append(sample));
  EXPECT_FALSE(failed.error().empty());
  EXPECT_EQ(EquityStore(file).samples().size(), 2U);
}
// Compaction keeps the current attempt however old it is: removing that rule wiped a
// long-running account's whole curve at its next restart, which compacts on open.
TEST(EquityStore, CompactionKeepsTheCurrentAttemptHoweverOld) {
  Directory dir;
  const auto file = dir.path / "history.csv";
  const auto time = md::new_york_to_utc({2026, 1, 2}, 10, 0);
  EquitySample sample{time, 1, m("10000"), {}, m("10000"), {}, {}, 0};
  {
    EquityStore store(file);
    store.append(sample);
    sample.time += 100 * md::kNanosPerDay;
    store.append(sample);
  }
  EquityStore reopened(file);
  ASSERT_EQ(reopened.samples().size(), 2U);
  EXPECT_EQ(reopened.samples()[0].time, time);
  // A new attempt drops only the earlier one's rows from more than 90 days before it.
  sample.attempt = 2;
  sample.time += md::kNanosPerMinute;
  reopened.append(sample);
  ASSERT_EQ(reopened.samples().size(), 2U);
  EXPECT_EQ(reopened.samples()[0].time, time + 100 * md::kNanosPerDay);
  EXPECT_EQ(reopened.samples()[0].attempt, 1U);
  EXPECT_EQ(reopened.samples()[1].attempt, 2U);
  EXPECT_EQ(EquityStore(file).samples().size(), 2U);
}
// At most 100,000 samples of earlier attempts stay, the newest of them, even inside 90 days.
TEST(EquityStore, KeepsTheNewestHundredThousandSamplesOfEarlierAttempts) {
  Directory dir;
  const auto file = dir.path / "history.csv";
  const auto start = md::new_york_to_utc({2026, 1, 2}, 10, 0);
  constexpr int kEarlier = 100'010;  // minutes: about 69 days
  {
    std::ofstream out(file);
    for (int i = 0; i < kEarlier; ++i)
      out << start + i * md::kNanosPerMinute << ",1,100000000000,null,100000000000,null,null,0,0\n";
  }
  EquityStore store(file);
  ASSERT_EQ(store.samples().size(), static_cast<std::size_t>(kEarlier));  // all the current attempt
  store.append({start + kEarlier * md::kNanosPerMinute, 2, m("10000"), {}, m("10000"), {}, {}, 0});
  ASSERT_EQ(store.samples().size(), 100'001U);
  EXPECT_EQ(store.samples().front().time, start + 10 * md::kNanosPerMinute);
  EXPECT_EQ(store.samples()[99'999].attempt, 1U);
  EXPECT_EQ(store.samples().back().attempt, 2U);
  EXPECT_TRUE(store.error().empty()) << store.error();
  EXPECT_EQ(lines_of(file), 100'001U);
  EXPECT_EQ(EquityStore(file).samples().size(), 100'001U);
}
// Between resets, every 4096th line written compacts the file too, so a long attempt
// does not carry an earlier attempt's old rows until the next restart or reset.
TEST(EquityStore, EveryFourThousandNinetySixthLineCompactsTheHistory) {
  Directory dir;
  const auto file = dir.path / "history.csv";
  const auto start = md::new_york_to_utc({2026, 1, 2}, 10, 0);
  EquityStore store(file);
  store.append({start, 1, m("10000"), {}, m("10000"), {}, {}, 0});
  // The reset compacts and keeps attempt 1's recent row: two lines.
  store.append({start + md::kNanosPerMinute, 2, m("10000"), {}, m("10000"), {}, {}, 0});
  ASSERT_EQ(store.samples().size(), 2U);
  // 91 days on, that row is old, but only a compaction drops it.
  const auto later = start + 91 * md::kNanosPerDay;
  for (int i = 0; i < 4093; ++i)
    store.append({later + i * md::kNanosPerMinute, 2, m("10000"), {}, m("10000"), {}, {}, 0});
  ASSERT_EQ(store.samples().size(), 4095U);
  EXPECT_EQ(store.samples().front().attempt, 1U);
  EXPECT_EQ(lines_of(file), 4095U);
  store.append({later + 4093 * md::kNanosPerMinute, 2, m("10000"), {}, m("10000"), {}, {}, 0});  // line 4096
  ASSERT_EQ(store.samples().size(), 4095U);
  EXPECT_EQ(store.samples().front().attempt, 2U);
  EXPECT_EQ(lines_of(file), 4095U);
  EXPECT_TRUE(store.error().empty()) << store.error();
  EXPECT_EQ(EquityStore(file).samples().size(), 4095U);
}
// A damaged account's history is opened read-only (Desk::open_journal), and openportd
// leaves a damaged account's files alone until --repair-journals: no compaction of
// its torn row, no appended samples. Read-write, the same open rewrites the file.
TEST(EquityStore, AReadOnlyHistoryIsNeverRewrittenOrAppended) {
  Directory dir;
  const auto file = dir.path / "damaged.jsonl.equity.csv";
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  {
    EquityStore store(file);
    store.append({time, 1, m("10000"), {}, m("10000"), {}, {}, 0});
    store.append({time + md::kNanosPerMinute, 1, m("10010"), {}, m("10010"), {}, {}, 0});
  }
  { std::ofstream out(file, std::ios::app); out << "torn"; }
  const auto before = bytes_of(file);
  EquityStore store(file, true);
  EXPECT_EQ(store.samples().size(), 2U);
  EXPECT_EQ(store.error(), "equity: ignored an invalid row");
  store.append({time + 2 * md::kNanosPerMinute, 1, m("10020"), {}, m("10020"), {}, {}, 0});
  store.append({time + 3 * md::kNanosPerMinute, 2, m("10000"), {}, m("10000"), {}, {}, 0});  // a reset
  EXPECT_EQ(store.samples().size(), 2U);
  EXPECT_EQ(bytes_of(file), before);
  const EquityStore writable(file);
  EXPECT_EQ(writable.samples().size(), 2U);
  EXPECT_NE(bytes_of(file), before);
}
// An account's equity, floor and risk curve, beside its owner-only journal. Under a
// shell's umask 022 appends used to create it 0644.
TEST(EquityStore, HistoryIsOwnerOnlyAndAnOlderReadableOneIsTightened) {
  Directory dir;
  const auto previous = ::umask(022);
  const auto mode_of = [](const std::filesystem::path& path) {
    struct stat info {};
    EXPECT_EQ(::stat(path.c_str(), &info), 0) << path;
    return static_cast<unsigned>(info.st_mode) & 0777u;
  };
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto file = dir.path / "journal.jsonl.equity.csv";
  { EquityStore store(file); store.append({time, 1, m("10000"), {}, m("10000"), {}, {}, 0}); }
  EXPECT_EQ(mode_of(file), 0600u);
  // A history with rows is compacted on open; an empty one is only tightened.
  for (const auto& older : {file, dir.path / "empty.equity.csv"}) {
    { std::ofstream touch(older, std::ios::app); }
    ASSERT_EQ(::chmod(older.c_str(), 0644), 0);
    EquityStore reopened(older);
    EXPECT_TRUE(reopened.error().empty()) << reopened.error();
    EXPECT_EQ(mode_of(older), 0600u) << older;
  }
  ::umask(previous);
}
TEST(EquityStore, TornRowsAreIgnoredWithoutLosingGoodSamples) {
  Directory dir; const auto file = dir.path / "history.csv";
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  { EquityStore store(file); store.append({time, 1, m("10000"), {}, m("10000"), {}, {}, 0}); }
  { std::ofstream out(file, std::ios::app); out << "torn,partial"; }
  EquityStore recovered(file);
  EXPECT_EQ(recovered.samples().size(), 1U);
  EXPECT_FALSE(recovered.error().empty());
}
TEST(EquityStore, KeepsTheFirstMarkAtTheTargetOrFloorWithinAMinute) {
  // B25: the mark that decides an attempt is stored even when its minute has one.
  Directory dir;
  EquityStore store(dir.path / "history.csv");
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto at = [&](int seconds, std::string_view equity) {
    store.append({time + seconds * md::kNanosPerSecond, 1, m(equity), m("9900"), m("10000"), m("10100"), {}, 0});
  };
  at(0, "10050");
  at(15, "10060");     // the same minute and levels: dropped
  at(30, "10109.35");  // first at or past the target: kept
  at(45, "10110");
  at(50, "9900");      // touches the floor: kept
  ASSERT_EQ(store.samples().size(), 3U);
  EXPECT_EQ(store.samples()[1].equity, m("10109.35"));
  EXPECT_EQ(store.samples()[2].equity, m("9900"));
}
TEST(EquityStore, EachFillGetsItsExactMarkedEquityAndAtomicLegsShareAMark) {
  test::ScriptedMarket f;
  trading::SessionConfig c;
  c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
  c.rules.max_drawdown = m("1000");
  trading::TradingSession s(c, f.time); f.seed(s);
  s.submit(f.limit("first", 1, "4"), f.time);
  s.submit(f.limit("second", 1, "4"), f.time);
  const auto before = s.snapshot();
  f.next(); s.on_quotes({f.quote("3.80", "4", 10)}, {f.valuation()}, f.time);
  // The mark before the executions leads, then each fill's own.
  const auto samples = fill_equity_samples(s, *before);
  ASSERT_EQ(samples.size(), 3U);
  EXPECT_EQ(samples[0].equity, m("100000"));
  EXPECT_EQ(samples[0].fill, 0U);
  EXPECT_EQ(samples[1].equity, m("99989.35"));
  EXPECT_EQ(samples[2].equity, m("99978.70"));
  EXPECT_EQ(samples[2].equity, s.snapshot()->equity);
  EXPECT_EQ(samples[1].floor, m("99000"));
  test::ScriptedMarket wing; wing.contract.strike = 5010; wing.time = f.time; wing.seed(s, "2", "2.20");
  auto combo = f.market("combo"); combo.symbol.clear(); combo.legs = {{f.symbol(), trading::Side::Buy, 1}, {wing.symbol(), trading::Side::Sell, 1}};
  const auto prior = s.snapshot(); s.submit(combo, f.time);
  const auto legs = fill_equity_samples(s, *prior);
  ASSERT_EQ(legs.size(), 3U);
  EXPECT_EQ(legs[1].equity, legs[2].equity);
  EXPECT_EQ(legs[2].equity, s.snapshot()->equity);
}
TEST(EquityStore, PreFillObservationPreservesAHighBeforeTheSameQuoteClosesThePosition) {
  test::ScriptedMarket market;
  trading::SessionConfig config;
  config.limits.aggregate = {1e9, 1e9}; config.limits.per_underlying = {1e9, 1e9};
  config.limits.price_band_absolute = m("10");
  trading::TradingSession session(config, market.time);
  market.seed(session);
  ASSERT_TRUE(session.submit(market.market("entry"), market.time).decision.ok());
  auto exit = market.limit("exit", 1, "7"); exit.side = trading::Side::Sell;
  ASSERT_TRUE(session.submit(exit, market.time).decision.ok());
  const auto before = session.snapshot();
  market.next(); session.on_quotes({market.quote("7", "7.20", 10)}, {market.valuation()}, market.time);
  const auto samples = fill_equity_samples(session, *before);
  ASSERT_EQ(samples.size(), 2U);
  EXPECT_EQ(samples[0].equity, m("100289.35"));
  EXPECT_EQ(samples[1].equity, m("100278.70"));
  EXPECT_EQ(samples[0].fill, 0U);
  EXPECT_NE(samples[1].fill, 0U);
}
TEST(EquityStore, ShareChangesAreSampledAndEightColumnHistoryStillLoads) {
  Directory dir; const auto file = dir.path / "history.csv";
  test::ScriptedMarket f; f.contract = *md::parse_osi("SPY261022C00500000");
  trading::SessionConfig c; c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
  trading::TradingSession s(c, f.time);
  auto valuation = f.valuation(); valuation.spot = 505; valuation.forward = 505;
  s.define(f.contract, f.time);
  s.on_quotes({f.quote("6", "6.20")}, {valuation}, f.time, {{"SPY", f.time, m("505")}});
  ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
  auto before = s.snapshot();
  ASSERT_TRUE(s.exercise(f.symbol(), 1, f.time).decision.ok());
  auto samples = fill_equity_samples(s, *before);
  ASSERT_EQ(samples.size(), 1U);
  EXPECT_EQ(samples.front().stock_fill, 1U);
  EXPECT_EQ(samples.front().equity, s.snapshot()->equity);
  { std::ofstream out(file); out << f.time << ",1,100000000000,null,100000000000,null,null,0\n"; }
  EquityStore store(file);
  store.append(samples.front()); store.append(samples.front());
  EXPECT_EQ(store.samples().size(), 2U);
  before = s.snapshot();
  ASSERT_TRUE(s.trade_stock("SPY", -100, f.time).decision.ok());
  samples = fill_equity_samples(s, *before);
  ASSERT_EQ(samples.size(), 1U);
  store.append(samples.front());
  const EquityStore recovered(file);
  ASSERT_EQ(recovered.samples().size(), 3U);
  EXPECT_EQ(recovered.samples().back().stock_fill, 2U);
  EXPECT_TRUE(recovered.error().empty());
}
TEST(EquityStore, EveryDecisionIsRetainedBeforeLiquidationWithOrWithoutFills) {
  for (const auto mode : {trading::DrawdownMode::Intraday, trading::DrawdownMode::EndOfDay}) {
    for (const auto reason : {trading::Reason::PROFIT_TARGET, trading::Reason::DRAWDOWN_FLOOR, trading::Reason::DAILY_LOSS_LIMIT}) {
      for (const bool liquidate : {false, true}) {
        SCOPED_TRACE(::testing::Message() << static_cast<int>(mode) << " " << trading::to_string(reason) << " fills " << liquidate);
        Directory dir;
        const auto file = dir.path / "equity.csv";
        EquityStore store(file);
        test::ScriptedMarket f;
        trading::SessionConfig c;
        c.initial_cash = m("10000");
        c.limits.aggregate = {1e9, 1e9}; c.limits.per_underlying = {1e9, 1e9};
        c.rules.profit_target = m("100");
        c.rules.max_drawdown = m(reason == trading::Reason::DAILY_LOSS_LIMIT ? "1000" : "100");
        c.rules.drawdown_mode = mode;
        if (reason == trading::Reason::DAILY_LOSS_LIMIT) {
          c.rules.daily_loss_limit = m("100");
          c.rules.daily_loss_action = trading::BreachAction::Fail;
        }
        trading::TradingSession s(c, f.time);
        f.seed(s);
        ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
        const auto before = s.snapshot();
        store.append({f.time, 1, before->equity, before->evaluation.floor, before->evaluation.peak, m("10100"), {}, 0});
        f.next();
        const bool pass = reason == trading::Reason::PROFIT_TARGET;
        auto quote = f.quote(pass ? "6" : "2", pass ? "6.20" : "2.20");
        if (!liquidate) { quote.bid.reset(); quote.bid_size = 0; quote.ask = m(pass ? "12.20" : "0.05"); }
        s.on_quotes({quote}, {f.valuation()}, f.time);
        const auto after = s.snapshot();
        ASSERT_EQ(after->evaluation.decision_code, reason);
        ASSERT_EQ(after->evaluation.status, pass ? trading::EvaluationStatus::Passed : trading::EvaluationStatus::Failed);
        EXPECT_EQ(after->recent_fills.size(), liquidate ? 2U : 1U);
        const auto samples = fill_equity_samples(s, *before);
        for (const auto& sample : samples) store.append(sample);
        const auto& points = store.samples();
        const auto deciding = std::find_if(points.begin(), points.end(), [&](const EquitySample& sample) {
          return sample.time == after->evaluation.decided_at && sample.equity == after->evaluation.decided_equity && sample.fill == 0;
        });
        ASSERT_NE(deciding, points.end());
        EXPECT_EQ(deciding->floor, after->evaluation.floor);
        if (mode == trading::DrawdownMode::EndOfDay) { EXPECT_EQ(deciding->tomorrow_floor, deciding->floor); }
        if (liquidate) {
          const auto fill = std::find_if(points.begin(), points.end(), [](const EquitySample& sample) { return sample.fill == 2; });
          ASSERT_NE(fill, points.end());
          EXPECT_LT(deciding, fill);
          EXPECT_EQ(fill->time, deciding->time);
          EXPECT_EQ(fill->equity, after->equity);
        }
        const EquityStore loaded(file);
        EXPECT_EQ(loaded.samples().size(), points.size());
        EXPECT_EQ(loaded.samples().at(static_cast<std::size_t>(deciding - points.begin())).equity, after->evaluation.decided_equity);
      }
    }
  }
}

TEST(EquityStore, ADecisionCausedByAnEntryFillKeepsItsOwnEquityBeforeTheLiquidation) {
  test::ScriptedMarket f;
  trading::SessionConfig c;
  c.rules.max_drawdown = m("10");
  trading::TradingSession s(c, f.time);
  f.seed(s);
  const auto before = s.snapshot();
  ASSERT_TRUE(s.submit(f.market("entry"), f.time).decision.ok());
  const auto after = s.snapshot();
  ASSERT_EQ(after->evaluation.status, trading::EvaluationStatus::Failed);
  const auto samples = fill_equity_samples(s, *before);
  const auto deciding = std::find_if(samples.begin(), samples.end(), [](const EquitySample& sample) { return sample.decision; });
  ASSERT_NE(deciding, samples.end());
  EXPECT_EQ(deciding->equity, after->evaluation.decided_equity);
  EXPECT_EQ(deciding->time, after->evaluation.decided_at);
  const auto liquidation = std::find_if(samples.begin(), samples.end(), [](const EquitySample& sample) { return sample.fill == 2; });
  ASSERT_NE(liquidation, samples.end());
  EXPECT_LT(deciding, liquidation);
}

TEST(EquityStore, StorageErrorKeepsFailureTimesAndRecoversRetainedSamples) {
  Directory directory;
  const auto file = directory.path / "blocked";
  std::filesystem::create_directory(file);
  EquityStore store(file);
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  const auto before = md::now();
  store.append({time, 1, m("10000"), {}, m("10000"), {}, {}, 0});
  ASSERT_FALSE(store.error().empty());
  const auto failed_at = store.error_time();
  EXPECT_GE(failed_at, before);
  EXPECT_LE(failed_at, md::now());
  EXPECT_EQ(store.error_market_time(), time);
  EXPECT_FALSE(store.error_recovered());
  std::filesystem::remove(file);
  store.append({time + md::kNanosPerMinute, 1, m("10001"), {}, m("10001"), {}, {}, 0});
  EXPECT_TRUE(store.error_recovered());
  EXPECT_EQ(store.error_time(), failed_at);
  EXPECT_EQ(store.error_market_time(), time);
  EXPECT_FALSE(store.error().empty());
  EXPECT_EQ(EquityStore(file).samples().size(), 2u);
}

TEST(EquityStore, RiskSamplesRoundTripWithoutInventingValuesForLegacyRows) {
  Directory dir;
  const auto file = dir.path / "risk.csv";
  const auto time = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  EquitySample sample{time, 1, m("10000"), {}, m("10000"), {}, {}, 0};
  {
    EquityStore store(file); store.append(sample);
    sample.time += md::kNanosPerMinute;
    sample.risk = EquityRisk{{123456.789012345, -120, -40, 15}, 0.25, 0.4, 0.12, {}, 0.8, 0.75};
    store.append(sample);
  }
  EquityStore restored(file);
  ASSERT_EQ(restored.samples().size(), 2U);
  EXPECT_FALSE(restored.samples()[0].risk);
  ASSERT_TRUE(restored.samples()[1].risk);
  EXPECT_DOUBLE_EQ(restored.samples()[1].risk->exposure.dollar_delta, sample.risk->exposure.dollar_delta);
  EXPECT_DOUBLE_EQ(restored.samples()[1].risk->vega_utilisation, 0.4);
  EXPECT_EQ(restored.samples()[1].risk->gamma_utilisation, 0.12);
  EXPECT_FALSE(restored.samples()[1].risk->theta_utilisation);
  EXPECT_EQ(restored.samples()[1].risk->grid_utilisation, 0.8);
  EXPECT_EQ(restored.samples()[1].risk->short_vega_utilisation, 0.75);
}
}  // namespace
}  // namespace openport::server
