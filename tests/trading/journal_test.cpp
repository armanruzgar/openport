#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>
#include "trading/state.hpp"
#include <nlohmann/json.hpp>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
class TemporaryJournal {
 public:
  TemporaryJournal() {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-trading-XXXXXX").string();
    auto* directory = ::mkdtemp(pattern.data());
    if (!directory) throw std::runtime_error("mkdtemp failed");
    directory_ = directory;
    path = (directory_ / "session.jsonl").string();
  }
  ~TemporaryJournal() { std::filesystem::remove_all(directory_); }
  std::string read() const {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text; text << file.rdbuf(); return text.str();
  }
  void write(std::string_view text) const { std::ofstream file(path, std::ios::binary); file << text; }
  std::string path;
 private:
  std::filesystem::path directory_;
};
TEST(TradingJournal, OlderStockFillsKeepTheirBytesWithoutDeliveryBuyingPower) {
  const StockFill original{1, "SPY", 100, Money::parse("510"), 1, StockSource::Delivery, "call"};
  const Json legacy = original;
  EXPECT_FALSE(legacy.contains("buying_power_after"));
  const auto restored = legacy.get<StockFill>();
  EXPECT_FALSE(restored.buying_power_after);
  EXPECT_EQ(Json(restored), legacy);
  auto recorded = original;
  recorded.buying_power_after = Money::parse("-0.000001");
  const auto current = Json(recorded).get<StockFill>();
  EXPECT_EQ(current.buying_power_after, recorded.buying_power_after);
}

TEST(TradingJournal, AmRetirementKeepsClosingDataThroughRecoveryAndSettlement) {
  TemporaryJournal file;
  auto sink = FileJournal::create(file.path);
  test::ScriptedMarket f;
  f.contract = *md::parse_osi("SPX260918C06020000");
  f.time = f.contract.last_trade_time() - 15 * md::kNanosPerSecond;
  TradingSession session({}, f.time, sink);
  f.seed(session);
  ASSERT_TRUE(session.submit(f.market("entry"), f.time).decision.ok());
  const auto close = f.time;
  for (const auto time : {f.contract.last_trade_time(),
                         md::new_york_to_utc({2026, 9, 17}, 21, 0)}) {
    f.time = time;
    ++f.observation;
    auto value = f.valuation();
    value.valid = false;
    ASSERT_TRUE(session.on_quotes({f.quote("0.00", "0.00")}, {value}, time).decision.ok());
    auto recovered = TradingSession::recover(FileJournal::read(file.path));
    EXPECT_EQ(recovered.snapshot_json(), session.snapshot_json());
    EXPECT_EQ(recovered.valuations().at(f.symbol()).time, close);
    EXPECT_TRUE(recovered.snapshot()->valuation_complete);
    EXPECT_TRUE(recovered.snapshot()->risk.complete);
  }
  ASSERT_TRUE(session.settle(f.symbol(), Money::parse("6210"), f.contract.expiry_time()).decision.ok());
  EXPECT_TRUE(session.snapshot()->positions.empty());
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path)).snapshot_json(), session.snapshot_json());
}

TEST(TradingJournal, EveryReasonRoundTripsIncludingScalingAndCalendarCodes) {
  EXPECT_EQ(static_cast<int>(Reason::NEWS_BLACKOUT), static_cast<int>(Reason::MAX_VOLUME_SHARE) + 1);
  EXPECT_EQ(static_cast<int>(Reason::HOLD_RESTRICTED), static_cast<int>(Reason::NEWS_BLACKOUT) + 1);
  EXPECT_EQ(static_cast<int>(Reason::RESET_LIMIT), static_cast<int>(Reason::HOLD_RESTRICTED) + 1);
  EXPECT_EQ(kLastReason, Reason::RESET_LIMIT);
  for (int i = 0; i <= static_cast<int>(kLastReason); ++i) {
    const auto reason = static_cast<Reason>(i);
    const nlohmann::json encoded = reason;
    ASSERT_TRUE(encoded.is_string());
    EXPECT_NE(encoded.get<std::string>(), std::string("UNKNOWN"));
    EXPECT_EQ(nlohmann::json::parse(encoded.dump()).get<Reason>(), reason);
  }
}
TEST(TradingJournal, SettlementRecordsRecoverExactReferencesCashAndOptionalProvenance) {
  for (const auto side : {Side::Buy, Side::Sell}) {
    for (const bool provenance : {false, true}) {
      TemporaryJournal file;
      auto sink = FileJournal::create(file.path);
      test::ScriptedMarket f;
      f.contract = *md::parse_osi("SPX260923C05000000");
      TradingSession session({}, f.time, sink);
      f.seed(session);
      ASSERT_TRUE(session.submit(f.market("entry", 2, side), f.time).decision.ok());
      const auto before = session.snapshot();
      const auto position = before->positions.front().position;
      std::optional<SettlementSource> source;
      if (provenance) source = SettlementSource{{"kind", "manual_am_import"}, {"symbol", f.symbol()}};
      const auto time = f.contract.expiry_time();
      ASSERT_TRUE(session.settle(f.symbol(), Money::parse("5010.123456"), time, source).decision.ok());
      const auto expected_cash = Money::parse("1012.3456") * position.quantity;
      const auto check = [&](const TradingSnapshot& snapshot) {
        ASSERT_EQ(snapshot.settlements.size(), 1);
        const auto& record = snapshot.settlements.front();
        EXPECT_EQ(record.contract.osi_symbol(), f.symbol());
        EXPECT_EQ(record.contract.settlement, md::Settlement::AM);
        EXPECT_EQ(record.value, Money::parse("5010.123456"));
        EXPECT_EQ(record.time, time);
        EXPECT_EQ(record.quantity, position.quantity);
        EXPECT_EQ(record.cash, expected_cash);
        EXPECT_EQ(record.realised, expected_cash - position.basis);
        EXPECT_EQ(record.fee, Money{});
        EXPECT_EQ(record.source, source);
      };
      check(*session.snapshot());
      EXPECT_EQ(session.snapshot()->account.cash - before->account.cash, expected_cash);
      // A later delta must not lose a record derived from a previous transaction.
      session.trip_kill("after settlement", time + md::kNanosPerSecond);
      const auto recovery = FileJournal::read(file.path);
      auto recovered = TradingSession::recover(recovery, sink);
      check(*recovered.snapshot());
      EXPECT_EQ(recovered.snapshot_json(), session.snapshot_json());
      EXPECT_EQ(recovered.settle(f.symbol(), Money::parse("5011"), time + md::kNanosPerSecond).decision.code, Reason::ALREADY_SETTLED);
      check(*recovered.snapshot());
      for (const auto& record : recovery.records) {
        if (record.type == "settlement") {
          EXPECT_EQ(nlohmann::json::parse(record.payload).contains("settlement_source"), provenance);
        }
      }
      // A recovery checkpoint still derives all earlier settlements from events.
      check(*TradingSession::recover(FileJournal::read(file.path)).snapshot());
    }
  }
}

TEST(TradingJournal, BatchedWritesAreImmediatelyReadableAndFlushOnlyWhenPending) {
  TemporaryJournal file;
  int syncs = 0;
  FileJournal::Options options;
  options.sync_policy = FileJournal::SyncPolicy::Batched;
  options.sync_interval = std::chrono::hours(1);
  options.hooks.sync = [&](int) { ++syncs; return true; };
  options.hooks.clock = [] { return std::chrono::steady_clock::time_point{}; };
  auto journal = FileJournal::create(file.path, options);
  journal->flush();
  EXPECT_EQ(syncs, 0);
  for (std::uint64_t record = 1; record <= 10; ++record) {
    journal->append(static_cast<Timestamp>(record), "test", "{}");
    const auto recovered = FileJournal::read(file.path, journal->head());
    EXPECT_EQ(recovered.records.size(), record);
    EXPECT_FALSE(recovered.truncated_final_line);
    EXPECT_EQ(syncs, 1);  // The first append always syncs.
  }
  journal->flush();
  EXPECT_EQ(syncs, 2);
  journal->flush();
  journal.reset();
  EXPECT_EQ(syncs, 2);
}

TEST(TradingJournal, BatchedIntervalUsesSteadyTimeAndFlushRestartsIt) {
  TemporaryJournal file;
  int syncs = 0;
  auto now = std::chrono::steady_clock::time_point{};
  FileJournal::Options options;
  options.sync_policy = FileJournal::SyncPolicy::Batched;
  options.hooks.sync = [&](int) { ++syncs; return true; };
  options.hooks.clock = [&] { return now; };
  auto journal = FileJournal::create(file.path, options);
  journal->append(0, "first", "{}");
  now += std::chrono::milliseconds(249);
  journal->append(md::kNanosPerDay, "market_time_does_not_sync", "{}");
  EXPECT_EQ(syncs, 1);
  now += std::chrono::milliseconds(1);
  journal->append(md::kNanosPerDay, "interval", "{}");
  EXPECT_EQ(syncs, 2);
  now += std::chrono::milliseconds(100);
  journal->append(md::kNanosPerDay, "pending", "{}");
  journal->flush();
  EXPECT_EQ(syncs, 3);
  now += std::chrono::milliseconds(249);
  journal->append(md::kNanosPerDay, "pending", "{}");
  EXPECT_EQ(syncs, 3);
  now += std::chrono::milliseconds(1);
  journal->append(md::kNanosPerDay, "interval", "{}");
  EXPECT_EQ(syncs, 4);
}

TEST(TradingJournal, DestructionFlushesPendingRecordsAndNeverThrows) {
  for (const bool fail : {false, true}) {
    TemporaryJournal file;
    int syncs = 0;
    FileJournal::Options options;
    options.sync_policy = FileJournal::SyncPolicy::Batched;
    options.hooks.clock = [] { return std::chrono::steady_clock::time_point{}; };
    options.hooks.sync = [&](int) { return ++syncs == 1 || !fail; };
    {
      const auto journal = FileJournal::create(file.path, options);
      journal->append(0, "first", "{}");
      journal->append(1, "pending", "{}");
      EXPECT_EQ(syncs, 1);
    }
    EXPECT_EQ(syncs, 2);
    EXPECT_EQ(FileJournal::read(file.path).records.size(), 2U);
    EXPECT_NO_THROW(FileJournal::resume(file.path));  // Destruction also releases the lock.
  }
}

TEST(TradingJournal, DeferredSyncFailureIsIndeterminateAndLatchesAppendFailure) {
  for (const bool explicit_flush : {false, true}) {
    TemporaryJournal file;
    int syncs = 0;
    auto now = std::chrono::steady_clock::time_point{};
    FileJournal::Options options;
    options.sync_policy = FileJournal::SyncPolicy::Batched;
    options.hooks.clock = [&] { return now; };
    options.hooks.sync = [&](int) { return ++syncs == 1; };
    const auto journal = FileJournal::create(file.path, options);
    journal->append(0, "first", "{}");
    journal->append(1, "pending", "{}");
    const auto head = journal->head();
    now += options.sync_interval;
    try {
      if (explicit_flush) journal->flush();
      else journal->append(2, "indeterminate", "{}");
      FAIL() << "Deferred sync must fail";
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::JOURNAL_IO); }
    EXPECT_EQ(journal->sequence(), 2U);
    EXPECT_EQ(journal->head(), head);
    const auto written = file.read();
    EXPECT_EQ(FileJournal::read(file.path).records.size(), explicit_flush ? 2U : 3U);
    try {
      journal->append(3, "must_not_write", "{}");
      FAIL() << "Failed journal must stay failed";
    } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::JOURNAL_IO); }
    EXPECT_THROW(journal->flush(), TradingError);
    EXPECT_EQ(syncs, 2);
    EXPECT_EQ(file.read(), written);
  }
}

TEST(TradingJournal, ResumeSyncsItsFirstAppendAndDefaultsToPerRecord) {
  TemporaryJournal file;
  { const auto journal = FileJournal::create(file.path); journal->append(0, "older", "{}"); }
  int syncs = 0;
  FileJournal::Options options;
  options.hooks.sync = [&](int) { ++syncs; return true; };
  options.hooks.clock = [] { return std::chrono::steady_clock::time_point{}; };
  {
    const auto journal = FileJournal::resume(file.path, options);
    journal->append(1, "default", "{}");
    journal->append(2, "default", "{}");
    EXPECT_EQ(syncs, 2);
  }
  options.sync_policy = FileJournal::SyncPolicy::Batched;
  {
    const auto journal = FileJournal::resume(file.path, options);
    journal->append(3, "first_after_resume", "{}");
    EXPECT_EQ(syncs, 3);
    journal->append(4, "pending", "{}");
    EXPECT_EQ(syncs, 3);
  }
  EXPECT_EQ(syncs, 4);
  EXPECT_EQ(FileJournal::read(file.path).records.size(), 5U);
}

class FailingJournal final : public Journal {
 public:
  bool fail = false;
  std::uint64_t count = 0;
  void append(Timestamp, std::string_view, std::string_view) override {
    if (fail) throw TradingError(Reason::JOURNAL_IO, "injected disk failure");
    ++count;
  }
  std::uint64_t sequence() const override { return count; }
  std::string head() const override { return std::string(64, '0'); }
};
// The first field of `expected` that `actual` lacks or holds another value in, or ""
// when it has them all. Later builds add state fields, so an older snapshot is a subset.
std::string mismatch(const nlohmann::json& actual, const nlohmann::json& expected, const std::string& path = "") {
  if (expected.is_object()) {
    if (!actual.is_object()) return path + " (not an object)";
    for (const auto& [key, value] : expected.items()) {
      if (!actual.contains(key)) return path + "/" + key + " (missing)";
      if (auto where = mismatch(actual.at(key), value, path + "/" + key); !where.empty()) return where;
    }
    return "";
  }
  if (expected.is_array()) {
    if (!actual.is_array() || actual.size() != expected.size()) return path + " (array size)";
    for (std::size_t i = 0; i < expected.size(); ++i)
      if (auto where = mismatch(actual[i], expected[i], path + "/" + std::to_string(i)); !where.empty()) return where;
    return "";
  }
  // Derived analytics (scenario P&L, Greeks) go through libm, whose last bits differ
  // between platforms; recorded money is integer and compares exactly.
  if (expected.is_number_float() && actual.is_number()) {
    const double a = actual.get<double>(), e = expected.get<double>();
    if (std::abs(a - e) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(e)})) return "";
  }
  return actual == expected ? "" : path + ": " + actual.dump() + " != " + expected.dump();
}
// Captured with the original f31e910 binary: daily loss cancelled both bracket
// exits and a manual close, then rejected another close. Recovery preserves it.
TEST(TradingJournal, PreReduceOnlyKillJournalRecoversItsOriginalState) {
  const auto directory = std::filesystem::path(OPENPORT_TEST_DATA_DIR);
  const auto recovery = FileJournal::read((directory / "kill-before-reduce-only.jsonl").string());
  auto restored = TradingSession::recover(recovery);
  std::ifstream expected(directory / "kill-before-reduce-only.snapshot.json");
  std::string snapshot;
  std::getline(expected, snapshot);
  ASSERT_FALSE(snapshot.empty());
  EXPECT_EQ(mismatch(nlohmann::json::parse(restored.snapshot_json()), nlohmann::json(nlohmann::json::parse(snapshot).get<TradingSnapshot>())), "");
  EXPECT_TRUE(restored.snapshot()->risk.kill_latched);
  EXPECT_TRUE(restored.snapshot()->open_orders.empty());
  ASSERT_EQ(restored.snapshot()->positions.size(), 1U);
  EXPECT_EQ(restored.snapshot()->recent_orders.back().reason.code, Reason::KILL_SWITCH);
}

TEST(TradingJournal, IdleEmptyBatchesAreNotRecordedButTimeRulesStillRun) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, 0, journal);
  EXPECT_EQ(journal->count, 1);  // session_start
  // The first real market time starts the attempt, so it is recorded.
  s.on_quotes({}, {}, f.time);
  EXPECT_EQ(journal->count, 2);
  EXPECT_EQ(s.snapshot()->evaluation.started, f.time);
  // Flat with no open orders, later empty batches change nothing but the clock.
  const auto version = s.snapshot()->account_version;
  for (int i = 1; i <= 50; ++i) EXPECT_EQ(s.on_quotes({}, {}, f.time + i * md::kNanosPerSecond).account_version, version);
  EXPECT_EQ(journal->count, 2);
  EXPECT_EQ(s.snapshot()->time, f.time);
  // Time must still move forward.
  EXPECT_THROW(s.on_quotes({}, {}, f.time - 1), TradingError);
  // A working order makes time matter again: empty batches are recorded, and
  // a DAY order still ends with the session.
  f.next();
  f.seed(s);
  ASSERT_TRUE(s.submit(f.limit("rest", 1, "4.00"), f.time).decision.ok());
  const auto working = journal->count;
  s.on_quotes({}, {}, f.time + md::kNanosPerSecond);
  EXPECT_EQ(journal->count, working + 1);
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 16, 20));
  EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::DAY_END);
  // Flat again, so idle again.
  const auto ended = journal->count;
  s.on_quotes({}, {}, md::new_york_to_utc({2026, 9, 22}, 17, 0));
  EXPECT_EQ(journal->count, ended);
}

TEST(TradingJournal, EmptyBatchesAreRecordedWhileAPositionIsHeld) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, f.time, journal);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 1), f.time).decision.ok());
  const auto held = journal->count;
  s.on_quotes({}, {}, f.time + 2 * md::kNanosPerMinute);
  EXPECT_EQ(journal->count, held + 1);
  // Its mark ages, and the snapshot says so.
  EXPECT_FALSE(s.snapshot()->valuation_complete);
}

TEST(TradingJournal, BatchesThatChangeNothingAreNotRecorded) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, f.time, journal);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open", 1), f.time).decision.ok());
  const auto held = journal->count;
  const auto version = s.snapshot()->account_version;
  // The same market time and the same quote again, as batches of other news bring.
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(s.on_quotes({}, {}, f.time).account_version, version);
    EXPECT_EQ(s.on_quotes({f.quote()}, {f.valuation()}, f.time).account_version, version);
  }
  EXPECT_EQ(journal->count, held);
  // A later market time is a change.
  s.on_quotes({}, {}, f.time + md::kNanosPerSecond);
  EXPECT_EQ(journal->count, held + 1);
  EXPECT_EQ(s.snapshot()->account_version, version + 1);
}

TEST(TradingJournal, SkippedIdleBatchesRecoverToTheSameState) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string expected, head;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    for (int i = 1; i <= 20; ++i) s.on_quotes({}, {}, f.time + i * md::kNanosPerSecond);
    expected = s.snapshot_json();
    head = journal->head();
  }
  EXPECT_EQ(FileJournal::read(file.path).records.size(), 3);  // session_start, definition, market
  auto s = TradingSession::recover(FileJournal::read(file.path, head), FileJournal::resume(file.path));
  EXPECT_EQ(s.snapshot_json(), expected);
  // Commands carry on from the recorded clock.
  ASSERT_TRUE(s.submit(f.market("after", 1), f.time + 30 * md::kNanosPerSecond).decision.ok());
  EXPECT_EQ(s.snapshot()->recent_orders.back().status, OrderStatus::Filled);
}

TEST(TradingJournal, ScriptedEndToEndOutcomeRecoveryProducesIdenticalSnapshot) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string expected;
  std::string head;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s, "4", "4.20", 1);
    s.submit(f.limit("partial", 3), f.time);
    s.submit(f.limit("rest", 1, "4.10"), f.time);
    s.submit(f.limit("rejected", 1, "4.21"), f.time);
    f.next();
    s.on_quotes({f.quote("4", "4.20", 1)}, {f.valuation()}, f.time);
    auto limits = Limits{}; limits.max_order_contracts = 20;
    s.set_limits(limits, f.time);
    s.cancel(2, f.time);
    s.trip_kill("operator review", f.time);
    s.reset_kill("review complete", f.time);
    expected = s.snapshot_json();
    head = journal->head();
  }
  auto recovery = FileJournal::read(file.path, head);
  EXPECT_FALSE(recovery.truncated_final_line);
  ASSERT_FALSE(recovery.records.empty());
  const auto text = file.read();
  for (const auto* type : {"session_start", "order_accepted", "order_rejected", "fill", "cancel", "limit_change", "kill_trip", "kill_reset"})
    EXPECT_NE(text.find(std::string("\"type\":\"") + type + "\""), std::string::npos) << type;
  {
    auto resumed = FileJournal::resume(file.path);
    auto s = TradingSession::recover(recovery, resumed);
    EXPECT_EQ(s.snapshot_json(), expected);
    s.submit(f.market("empty-budget"), f.time);
    EXPECT_EQ(s.snapshot()->recent_orders.back().reason.code, Reason::IOC_REMAINDER);
    EXPECT_EQ(s.snapshot()->recent_fills.size(), 2);
    f.next();
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    s.submit(f.market("new"), f.time);
    f.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    ++f.observation;
    s.on_quotes({f.quote()}, {f.valuation()}, f.time);
    s.roll_day(f.time);
    f.time = f.contract.expiry_time();
    s.on_quotes({}, {}, f.time);
    s.settle(f.symbol(), Money::parse("5010"), f.time);
    expected = s.snapshot_json();
  }
  auto replayed = TradingSession::recover(FileJournal::read(file.path));
  EXPECT_EQ(replayed.snapshot_json(), expected);
  EXPECT_TRUE(replayed.snapshot()->positions.empty());
}
TEST(TradingJournal, IdenticalInputsProduceIdenticalBytesAndRecoveryPreservesLiquidityAndKill) {
  TemporaryJournal a;
  TemporaryJournal b;
  test::ScriptedMarket f;
  auto script = [&](const std::string& path) {
    auto journal = FileJournal::create(path);
    TradingSession s({}, f.time, journal);
    f.seed(s, "4", "4.20", 1);
    s.submit(f.limit("partial", 2), f.time);
    return s.snapshot_json();
  };
  EXPECT_EQ(script(a.path), script(b.path));
  EXPECT_EQ(a.read(), b.read());
  auto resumed = FileJournal::resume(a.path);
  auto s = TradingSession::recover(FileJournal::read(a.path), resumed);
  s.on_quotes({f.quote("4", "4.20", 100)}, {f.valuation()}, f.time);
  EXPECT_EQ(s.snapshot()->recent_orders[0].filled_quantity, 1);
  EXPECT_EQ(s.snapshot()->recent_orders[0].status, OrderStatus::PartiallyFilled);
  s.trip_kill("latched", f.time);
  const auto replayed = TradingSession::recover(FileJournal::read(a.path));
  EXPECT_TRUE(replayed.snapshot()->risk.kill_latched);
  EXPECT_EQ(replayed.snapshot_json(), s.snapshot_json());
}
TEST(TradingJournal, HashTamperingSequenceReorderingAndTrustedHeadDetectDamage) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string head;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    head = journal->head();
  }
  const auto good = file.read();
  auto tampered = good;
  const auto cash = tampered.find("100000000000");
  ASSERT_NE(cash, std::string::npos);
  tampered[cash] = '2';
  EXPECT_THROW((void)verify_journal(tampered), TradingError);
  const auto newline = good.find('\n');
  EXPECT_THROW((void)verify_journal(good.substr(newline + 1)), TradingError);
  EXPECT_THROW((void)verify_journal(good.substr(0, newline + 1), head), TradingError);
  EXPECT_THROW((void)verify_journal("{}\n"), TradingError);
  EXPECT_THROW((void)verify_journal(good + "broken\n"), TradingError);
  auto recovery = verify_journal(good);
  recovery.records.back().payload = "{}";
  EXPECT_THROW(TradingSession::recover(recovery), TradingError);
}
TEST(TradingJournal, TruncatedFinalLineIsReportedIgnoredAndNeverSilentlyOverwritten) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string snapshot;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    snapshot = s.snapshot_json();
  }
  const auto good = file.read();
  file.write(good + "{\"seq\":4,");
  const auto recovered = FileJournal::read(file.path);
  EXPECT_TRUE(recovered.truncated_final_line);
  EXPECT_EQ(TradingSession::recover(recovered).snapshot_json(), snapshot);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  EXPECT_EQ(file.read(), good + "{\"seq\":4,");
  // A fully serialized but un-terminated record is also an uncommitted suffix.
  const auto no_newline = verify_journal(good.substr(0, good.size() - 1));
  EXPECT_TRUE(no_newline.truncated_final_line);
  EXPECT_EQ(no_newline.records.size() + 1, recovered.records.size());
}
TEST(TradingJournal, RepairCutsATornFinalLineAfterKeepingTheOriginal) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  std::string snapshot;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
    snapshot = s.snapshot_json();
  }
  const auto good = file.read();
  const std::string torn = "{\"seq\":4,\"ti";  // where the disk ran out
  file.write(good + torn);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  const auto repaired = FileJournal::repair(file.path);
  EXPECT_EQ(repaired.bytes_cut, torn.size());
  ASSERT_FALSE(repaired.backup.empty());
  std::ifstream backup(repaired.backup, std::ios::binary);
  std::ostringstream kept;
  kept << backup.rdbuf();
  EXPECT_EQ(kept.str(), good + torn);
  EXPECT_EQ(file.read(), good);
  // It resumes and recovers as it stood, and a whole journal is left alone.
  auto resumed = FileJournal::resume(file.path);
  EXPECT_EQ(TradingSession::recover(FileJournal::read(file.path), resumed).snapshot_json(), snapshot);
  resumed.reset();
  EXPECT_EQ(FileJournal::repair(file.path).bytes_cut, 0U);
  EXPECT_EQ(file.read(), good);
  std::filesystem::remove(repaired.backup);
}
TEST(TradingJournal, ATornFirstRecordRepairsToAnEmptyJournalThatStartsAfresh) {
  TemporaryJournal file;
  // A new journal whose first record the disk tore, or refused, holds no transaction.
  const std::string torn = "{\"hash\":\"0f";
  file.write(torn);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  const auto repaired = FileJournal::repair(file.path);
  EXPECT_EQ(repaired.bytes_cut, torn.size());
  EXPECT_TRUE(repaired.empty);
  ASSERT_FALSE(repaired.backup.empty());
  std::ifstream backup(repaired.backup, std::ios::binary);
  std::ostringstream kept;
  kept << backup.rdbuf();
  EXPECT_EQ(kept.str(), torn);
  EXPECT_EQ(file.read(), "");
  const auto again = FileJournal::repair(file.path);
  EXPECT_EQ(again.bytes_cut, 0U);
  EXPECT_TRUE(again.empty);
  // It resumes as a new journal: the account's first record is its first.
  test::ScriptedMarket f;
  {
    auto resumed = FileJournal::resume(file.path);
    EXPECT_EQ(resumed->sequence(), 0U);
    TradingSession s({}, f.time, resumed);
    f.seed(s);
  }
  EXPECT_FALSE(FileJournal::repair(file.path).empty);
  EXPECT_EQ(FileJournal::read(file.path).records.front().type, "session_start");
  std::filesystem::remove(repaired.backup);
}
TEST(TradingJournal, RepairLeavesDamageBeforeTheLastLineAndLiveJournalsAlone) {
  TemporaryJournal file;
  test::ScriptedMarket f;
  {
    auto journal = FileJournal::create(file.path);
    TradingSession s({}, f.time, journal);
    f.seed(s);
  }
  auto damaged = file.read();
  damaged[damaged.find("\"time\":") + 8] ^= 1;  // a changed digit breaks the hash chain
  file.write(damaged + "{\"seq\":4,");
  try {
    (void)FileJournal::repair(file.path);
    ADD_FAILURE() << "a damaged journal must not be repaired";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_CORRUPT);
  }
  EXPECT_EQ(file.read(), damaged + "{\"seq\":4,");
  // A journal a server has open is locked against repair.
  file.write("");
  auto live = FileJournal::create(file.path + ".live");
  try {
    (void)FileJournal::repair(file.path + ".live");
    ADD_FAILURE() << "a journal in use must not be repaired";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_LOCKED);
  }
  live.reset();
  std::filesystem::remove(file.path + ".live");
}
TEST(TradingJournal, WriteFailureStopsTradingWithoutPublishingUnjournalledEffects) {
  test::ScriptedMarket f;
  auto journal = std::make_shared<FailingJournal>();
  TradingSession s({}, f.time, journal);
  f.seed(s);
  const auto before = s.snapshot();
  journal->fail = true;
  EXPECT_THROW(s.submit(f.market("failure"), f.time), TradingError);
  EXPECT_EQ(s.snapshot()->account_version, before->account_version);
  EXPECT_EQ(s.snapshot()->account.cash, before->account.cash);
  EXPECT_TRUE(s.snapshot()->recent_orders.empty());
  EXPECT_TRUE(s.snapshot()->journal_failed);
  journal->fail = false;
  EXPECT_THROW(s.submit(f.market("still-stopped"), f.time), TradingError);
  EXPECT_THROW(s.trip_kill("cannot write", f.time), TradingError);
  EXPECT_FALSE(before->journal_failed);
}
TEST(TradingJournal, ExclusiveWriterAndInvalidPathsFailLoudly) {
  TemporaryJournal file;
  auto journal = FileJournal::create(file.path);
  EXPECT_THROW(FileJournal::create(file.path), TradingError);
  EXPECT_THROW(FileJournal::resume(file.path), TradingError);
  EXPECT_THROW(FileJournal::create(file.path + "/missing"), TradingError);
  EXPECT_THROW(FileJournal::read(file.path + ".missing"), TradingError);
}
TEST(TradingJournal, LockRejectsSeparateOpensAndIsReleasedOnDestruction) {
  TemporaryJournal file;
  const std::string message = "paper journal is in use by another openportd; use --paper-journal to choose another file or --no-paper";
  auto expect_locked = [&](bool create) {
    try {
      const auto second = create ? FileJournal::create(file.path) : FileJournal::resume(file.path);
      FAIL() << "A second writer acquired the journal";
    } catch (const TradingError& error) {
      EXPECT_EQ(error.code(), Reason::JOURNAL_LOCKED);
      EXPECT_EQ(error.what(), message);
    }
  };
  {
    auto journal = FileJournal::create(file.path);
    journal->append(1, "first", "{}");
    const auto before = file.read();
    expect_locked(true);
    expect_locked(false);
    EXPECT_EQ(file.read(), before);
    // Closing a separate reader must not release the writer's lock.
    EXPECT_EQ(FileJournal::read(file.path).records.size(), 1u);
    expect_locked(false);
    journal->append(2, "second", "{}");
    EXPECT_EQ(FileJournal::read(file.path).records.size(), 2u);
  }
  {
    auto resumed = FileJournal::resume(file.path);
    EXPECT_EQ(resumed->sequence(), 2u);
    expect_locked(false);
    resumed->append(3, "third", "{}");
  }
  auto later = FileJournal::resume(file.path);
  EXPECT_EQ(later->sequence(), 3u);
}
TEST(TradingJournal, LockedJournalIsRejectedBeforeReadingOrRepairingItsContents) {
  TemporaryJournal file;
  const auto journal = FileJournal::create(file.path);
  file.write("invalid journal contents\n");
  try {
    const auto second = FileJournal::resume(file.path);
    FAIL() << "A second writer acquired the journal";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_LOCKED);
  }
  EXPECT_EQ(file.read(), "invalid journal contents\n");
}
TEST(TradingJournal, LowDiskNamesFilesystemFreeBytesAndReserveWithoutAPath) {
  TemporaryJournal file;
  FileJournal::Options options;
  options.hooks.free_bytes = [](int) { return 12345; };
  const auto journal = FileJournal::create(file.path, options);
  try {
    journal->append(1, "sample", "{}");
    FAIL() << "Low disk write succeeded";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_IO);
    const std::string message = error.what();
    EXPECT_NE(message.find("filesystem device"), std::string::npos);
    EXPECT_NE(message.find(std::filesystem::path(file.path).parent_path().filename().string()), std::string::npos);
    EXPECT_NE(message.find("12345 free bytes"), std::string::npos);
    EXPECT_NE(message.find("67108864"), std::string::npos);
    EXPECT_EQ(message.find(file.path), std::string::npos);
  }
  EXPECT_TRUE(file.read().empty());
}
TEST(TradingJournal, SessionPreservesLowDiskDiagnosticWithoutPublishingTheTransaction) {
  TemporaryJournal file;
  test::ScriptedMarket market;
  bool low_disk = false;
  FileJournal::Options options;
  options.hooks.free_bytes = [&](int) { return low_disk ? 12345ULL : 1024ULL * 1024 * 1024; };
  const auto journal = FileJournal::create(file.path, options);
  TradingSession session({}, market.time, journal);
  market.seed(session);
  const auto before = session.snapshot();
  const auto contents = file.read();
  low_disk = true;
  try {
    session.submit(market.market("low-disk"), market.time);
    FAIL() << "Low disk transaction succeeded";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::JOURNAL_IO);
    const std::string message = error.what();
    EXPECT_NE(message.find("Journal commit failed: Disk nearly full: filesystem device"), std::string::npos);
    EXPECT_NE(message.find(std::filesystem::path(file.path).parent_path().filename().string()), std::string::npos);
    EXPECT_NE(message.find("12345 free bytes"), std::string::npos);
    EXPECT_NE(message.find("67108864"), std::string::npos);
    EXPECT_NE(message.find("no in-memory transition published; stop trading and recover"), std::string::npos);
    EXPECT_EQ(message.find(std::filesystem::path(file.path).parent_path().string()), std::string::npos);
  }
  EXPECT_TRUE(session.snapshot()->journal_failed);
  EXPECT_EQ(session.snapshot()->quality_flags.back(), Reason::JOURNAL_IO);
  EXPECT_EQ(session.snapshot()->account_version, before->account_version);
  EXPECT_EQ(session.snapshot()->account.cash, before->account.cash);
  EXPECT_TRUE(session.snapshot()->recent_orders.empty());
  EXPECT_EQ(file.read(), contents);
  low_disk = false;
  EXPECT_THROW(session.submit(market.market("still-stopped"), market.time), TradingError);
}
TEST(TradingJournal, DryRunAndReadOnlyInspectionLeaveDamageUntouched) {
  TemporaryJournal file;
  {
    auto journal = FileJournal::create(file.path);
    journal->append(1, "first", "{}");
    journal->append(2, "second", "{}");
  }
  const auto good = file.read();
  file.write(good + "torn");
  const auto repair = FileJournal::repair(file.path, true);
  EXPECT_EQ(repair.bytes_cut, 4u);
  EXPECT_EQ(repair.records_kept, 2u);
  EXPECT_TRUE(repair.backup.empty());
  EXPECT_EQ(file.read(), good + "torn");
  const auto middle = good.find('\n') + 1;
  const auto damaged = good.substr(0, middle) + "bad\n" + good.substr(middle);
  file.write(damaged);
  EXPECT_THROW(FileJournal::read(file.path), TradingError);
  EXPECT_THROW(FileJournal::repair(file.path, true), TradingError);
  {
    auto [journal, recovery] = FileJournal::inspect(file.path);
    EXPECT_EQ(recovery.records.size(), 1u);
    EXPECT_FALSE(recovery.damage.empty());
    EXPECT_EQ(journal->bytes(), damaged.size());
    EXPECT_THROW(journal->append(3, "refused", "{}"), TradingError);
    EXPECT_THROW(FileJournal::repair(file.path), TradingError);
  }
  EXPECT_EQ(file.read(), damaged);
}
}  // namespace
}  // namespace openport::trading
