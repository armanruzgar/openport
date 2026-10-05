#include "support/sanitizer.hpp"
#include "support/contract_capture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <functional>
#include <future>
#include <sstream>
#include <gtest/gtest.h>

#include "openport/providers/replay.hpp"
#include "openport/server/desk.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "openport/server/plans.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"
#include "server/run_json.hpp"

namespace {
using namespace openport;
using nlohmann::json;
using trading::Money;
using namespace std::chrono_literals;

std::string read_file(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}
std::vector<md::Event> market_batch(const test::ScriptedMarket& market, double ask = 4.2) {
  return {md::ContractDefinition{0, market.contract},
          md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000},
          md::OptionQuote{0, market.time, ask - .2, ask, 20, 20},
          md::SnapshotComplete{"SPX", market.time}};
}
std::vector<md::Event> market_batch(const test::ScriptedMarket& market,
                                    const test::ScriptedMarket& other,
                                    double ask = 4.2, double other_bid = 1.0,
                                    double other_ask = 1.2) {
  return {md::ContractDefinition{0, market.contract},
          md::ContractDefinition{1, other.contract},
          md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000},
          md::OptionQuote{0, market.time, ask - .2, ask, 20, 20},
          md::OptionQuote{1, market.time, other_bid, other_ask, 20, 20},
          md::SnapshotComplete{"SPX", market.time}};
}
server::TradingReply command(server::Desk& desk, server::TradingCommand request, md::Timestamp time, md::Timestamp driver) {
  std::optional<server::TradingReply> result;
  desk.command(std::move(request), [&](server::TradingReply reply) { result = std::move(reply); }, time, driver);
  if (!result) throw std::runtime_error("Desk did not complete command");
  return *result;
}
class DeskPhases : public ::testing::TestWithParam<trading::Phase> {};
INSTANTIATE_TEST_SUITE_P(NonFunded, DeskPhases,
    ::testing::Values(trading::Phase::Evaluation, trading::Phase::Verification),
    [](const ::testing::TestParamInfo<trading::Phase>& param) {
      return param.param == trading::Phase::Verification ? "Verification" : "Evaluation";
    });
TEST_P(DeskPhases, VolumeShareGateChecksKnownUnknownStaleVolumeAndOpeningContracts) {
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.paper.rules.phase = GetParam();
  options.analytics.fallback_rate = 0;
  options.paper.rules.max_volume_percent = 10;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", {}, {{"SPX", "SPY"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  const auto volume = [&](double contracts, md::Timestamp time) {
    desk.replay_batch({md::OptionVolume{0, time, contracts}}, market.time);
  };
  server::TradingCommand entry; entry.order = market.limit("one", 1, "4.00");
  for (const auto phase : {0, 1, 2, 3}) {
    if (phase == 1) volume(100, market.time - 24 * 60 * md::kNanosPerMinute);
    if (phase == 2) volume(0, market.time);
    if (phase == 3) volume(10.5, market.time);
    const auto count = desk.trading_view()->snapshot->recent_orders.size();
    const auto refused = command(desk, entry, market.time, market.time);
    EXPECT_EQ(refused.decision.code, trading::Reason::MAX_VOLUME_SHARE);
    ASSERT_TRUE(refused.decision.evidence);
    EXPECT_EQ(refused.decision.evidence->contract, market.symbol());
    EXPECT_EQ(refused.decision.evidence->contracts, 1);
    EXPECT_EQ(refused.decision.evidence->percent, 10);
    if (phase == 2) { EXPECT_EQ(refused.decision.evidence->volume, 0); }
    else { EXPECT_FALSE(refused.decision.evidence->volume); }
    EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.size(), count);
    entry.kind = server::TradingCommand::Kind::Preview;
    const auto preview = command(desk, entry, market.time, market.time);
    ASSERT_TRUE(preview.preview);
    EXPECT_EQ(preview.preview->decision.code, trading::Reason::MAX_VOLUME_SHARE);
    entry.kind = server::TradingCommand::Kind::Submit;
  }
  volume(10, market.time);
  server::TradingCommand what_if;
  what_if.kind = server::TradingCommand::Kind::WhatIf;
  what_if.candidates = {{market.market("buy"), market.market("close", 1, trading::Side::Sell)},
                        {market.market("buy"), market.market("add")}};
  const auto projection = command(desk, what_if, market.time, market.time);
  ASSERT_TRUE(projection.what_if);
  ASSERT_EQ(projection.what_if->candidates.size(), 2U);
  EXPECT_TRUE(projection.what_if->candidates[0].decision.ok());
  EXPECT_EQ(projection.what_if->candidates[1].decision.code, trading::Reason::MAX_VOLUME_SHARE);
  const auto resting = command(desk, entry, market.time, market.time);
  ASSERT_TRUE(resting.decision.ok()) << resting.decision.message;
  server::TradingCommand change;
  change.kind = server::TradingCommand::Kind::Modify;
  change.order_id = *resting.order_id; change.change.quantity = 2;
  EXPECT_EQ(command(desk, change, market.time, market.time).decision.code, trading::Reason::MAX_VOLUME_SHARE);
  change.kind = server::TradingCommand::Kind::PreviewChange;
  const auto preview = command(desk, change, market.time, market.time);
  ASSERT_TRUE(preview.preview);
  EXPECT_EQ(preview.preview->decision.code, trading::Reason::MAX_VOLUME_SHARE);
  volume(20, market.time);
  change.kind = server::TradingCommand::Kind::Modify;
  ASSERT_TRUE(command(desk, change, market.time, market.time).decision.ok());
  change.kind = server::TradingCommand::Kind::Cancel;
  ASSERT_TRUE(command(desk, change, market.time, market.time).decision.ok());
  entry.order = market.market("held");
  ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  entry.order = market.market("held-plus-opening", 2);
  const auto refused = command(desk, entry, market.time, market.time);
  EXPECT_EQ(refused.decision.code, trading::Reason::MAX_VOLUME_SHARE);
  ASSERT_TRUE(refused.decision.evidence);
  EXPECT_EQ(refused.decision.evidence->contracts, 3);
  volume(0, market.time);
  entry.order = market.market("reduce", 1, trading::Side::Sell);
  EXPECT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  // Shares never enter the volume cap.
  desk.replay_batch({md::UnderlyingQuote{"SPY", market.time, 500, 500, 500}, md::SnapshotComplete{"SPY", market.time}}, market.time);
  server::TradingCommand stock;
  stock.kind = server::TradingCommand::Kind::TradeStock; stock.symbol = "SPY"; stock.quantity = 1;
  EXPECT_TRUE(command(desk, stock, market.time, market.time).decision.ok());
  desk.stop();
}

TEST(Desk, RestingVolumeOrdersCancelOnZeroOrStaleVolumeOnlyWhenTheyCanFill) {
  for (const bool stale : {false, true}) {
    test::RecordingFile file;
    test::ScriptedMarket market;
    server::Desk::Options options;
    options.analytics.fallback_rate = 0;
    options.paper.rules.max_volume_percent = 10;
    options.paper_journal = file.directory / "volume.jsonl";
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    auto batch = market_batch(market);
    batch.push_back(md::OptionVolume{0, market.time, 10});
    desk.replay_batch(batch, market.time);
    server::TradingCommand entry;
    entry.order = market.limit("resting", 1, "4.00", trading::Side::Buy, trading::TimeInForce::Gtc);
    const auto accepted = command(desk, entry, market.time, market.time);
    ASSERT_TRUE(accepted.decision.ok()) << accepted.decision.message;
    if (stale) market.time = md::new_york_to_utc({2026, 9, 23}, 10, 0);
    else market.next();
    batch = market_batch(market);
    if (!stale) batch.push_back(md::OptionVolume{0, market.time, 0});
    desk.replay_batch(batch, market.time);
    ASSERT_EQ(desk.trading_view()->snapshot->open_orders.size(), 1U);
    market.next();
    desk.replay_batch(market_batch(market, 4.0), market.time);
    const auto snapshot = desk.trading_view()->snapshot;
    ASSERT_EQ(snapshot->recent_orders.back().status, trading::OrderStatus::Cancelled);
    EXPECT_EQ(snapshot->recent_orders.back().reason.code, trading::Reason::MAX_VOLUME_SHARE);
    ASSERT_TRUE(snapshot->recent_orders.back().reason.evidence);
    if (stale) { EXPECT_FALSE(snapshot->recent_orders.back().reason.evidence->volume); }
    else { EXPECT_EQ(snapshot->recent_orders.back().reason.evidence->volume, 0); }
    EXPECT_TRUE(snapshot->recent_fills.empty());
    desk.stop();
    const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(options.paper_journal));
    EXPECT_EQ(recovered.snapshot()->recent_orders.back().reason.code, trading::Reason::MAX_VOLUME_SHARE);
  }
}

TEST(Desk, CounterPositionsCountWorkingOpeningsAndCancelAtFillAfterAnotherAccountChanges) {
  for (const bool modify : {false, true}) {
    test::RecordingFile file;
    test::ScriptedMarket market;
    server::Desk::Options options;
    options.analytics.fallback_rate = 0;
    options.paper.rules.no_counter_positions = true;
    options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
    options.paper_journal = file.directory / "paper.jsonl";
    options.paper_accounts = file.directory / "accounts";
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    server::TradingCommand create;
    create.kind = server::TradingCommand::Kind::CreateAccount;
    create.name = "Other"; create.initial_cash = Money::parse("100000");
    ASSERT_EQ(command(desk, create, market.time, market.time).account, "other");
    desk.replay_batch(market_batch(market), market.time);
    server::TradingCommand entry;
    entry.account = "other"; entry.order = market.limit("working-long", 1, "4.00");
    const auto other = command(desk, entry, market.time, market.time);
    ASSERT_TRUE(other.decision.ok()) << other.decision.message;
    EXPECT_TRUE(desk.trading_view("other")->snapshot->positions.empty());
    entry.order = market.limit("working-short", 1, "4.50", trading::Side::Sell);
    ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
    // The two working orders have zero net delta; compare each independently.
    entry.account.clear(); entry.order = market.market("counter", 1, trading::Side::Sell);
    const auto rejected = command(desk, entry, market.time, market.time);
    ASSERT_EQ(rejected.decision.code, trading::Reason::COUNTER_POSITION);
    ASSERT_TRUE(rejected.decision.evidence);
    EXPECT_EQ(rejected.decision.evidence->other_account, "other");
    entry.kind = server::TradingCommand::Kind::Preview;
    const auto preview = command(desk, entry, market.time, market.time);
    ASSERT_TRUE(preview.preview);
    EXPECT_EQ(preview.preview->decision.code, trading::Reason::COUNTER_POSITION);
    server::TradingCommand cancel;
    cancel.account = "other"; cancel.kind = server::TradingCommand::Kind::Cancel; cancel.order_id = *other.order_id;
    ASSERT_TRUE(command(desk, cancel, market.time, market.time).decision.ok());
    entry.kind = server::TradingCommand::Kind::Submit;
    entry.order = market.limit("resting-short", 1, "4.50", trading::Side::Sell);
    ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
    entry.account = "other"; entry.order = market.market("long");
    ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
    // Its working short is now a pure reduction and must not reserve counter direction.
    entry.account.clear(); entry.kind = server::TradingCommand::Kind::Preview; entry.order = market.market("permitted-long");
    const auto allowed = command(desk, entry, market.time, market.time);
    ASSERT_TRUE(allowed.preview);
    EXPECT_TRUE(allowed.preview->decision.ok()) << allowed.preview->decision.message;
    if (modify) {
      // A price-only modification can execute, even without increasing the quantity.
      server::TradingCommand change;
      change.kind = server::TradingCommand::Kind::Modify;
      change.order_id = desk.trading_view()->snapshot->open_orders.front().id;
      change.change.limit_price = Money::parse("4.00");
      ASSERT_TRUE(command(desk, change, market.time, market.time).decision.ok());
    } else {
      market.next();
      desk.replay_batch(market_batch(market, 4.7), market.time);
    }
    const auto snapshot = desk.trading_view()->snapshot;
    EXPECT_EQ(snapshot->recent_orders.back().status, trading::OrderStatus::Cancelled);
    EXPECT_EQ(snapshot->recent_orders.back().reason.code, trading::Reason::COUNTER_POSITION);
    EXPECT_TRUE(snapshot->recent_fills.empty());
    desk.stop();
    const auto recovered = trading::TradingSession::recover(trading::FileJournal::read(options.paper_journal));
    EXPECT_EQ(recovered.snapshot()->recent_orders.back().reason.code, trading::Reason::COUNTER_POSITION);
  }
}

TEST(Desk, VolumeFillRechecksVerifyAndOlderRecordedRunsKeepAcceptanceOnlyFills) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time; header.capabilities.delay = 0s;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return market.time; };
  test::DiscardEvents discard;
  {
    md::RecordingSink sink(file.path, header, discard, recording);
    for (const auto volume : {10, 0}) {
      auto batch = market_batch(market, volume == 10 ? 4.2 : 4.0);
      batch.insert(batch.end() - 1, md::OptionVolume{0, market.time, static_cast<double>(volume)});
      for (const auto& event : batch) sink.publish(event);
      market.next();
    }
    sink.close();
  }
  for (const bool enabled : {false, true}) {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.analytics.fallback_rate = 0;
    options.replay = true; options.opening_rule_checks = enabled;
    options.paper.rules.max_volume_percent = 10;
    options.run_input = server::recording_input(file.path);
    options.paper_journal = file.directory / (enabled ? "new.jsonl" : "old.jsonl");
    server::Desk desk("replay", header.capabilities, header.subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, header.subscription);
    bool submitted = false;
    while (const auto batch = batches.next()) {
      desk.replay_batch(batch->events, batch->received, batch->time);
      if (!submitted) {
        server::TradingCommand entry; entry.order = market.limit("resting", 1, "4.00");
        const auto accepted = command(desk, entry, desk.market_time(), batch->received);
        ASSERT_TRUE(accepted.decision.ok()) << accepted.decision.message;
        submitted = true;
      }
    }
    EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.back().status,
        enabled ? trading::OrderStatus::Cancelled : trading::OrderStatus::Filled);
    desk.stop();
    const auto recovery = trading::FileJournal::read(options.paper_journal);
    const auto inputs = server::run_inputs(recovery);
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(json::parse(inputs.front()).contains("opening_rule_checks"), enabled);
    const auto verified = server::verify_run(options.paper_journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

TEST(Desk, CompactAndFullOpeningInputRunsRecoverVerifyAndResumeByteIdentically) {
  test::RecordingFile file;
  test::ScriptedMarket market, unused;
  unused.contract.strike += 5;
  auto header = test::recording_header();
  header.started = market.time; header.capabilities.delay = 0s;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return market.time; };
  test::DiscardEvents discard;
  {
    md::RecordingSink sink(file.path, header, discard, recording);
    for (const double ask : {4.2, 4.0}) {
      auto batch = market_batch(market, unused, ask);
      batch.insert(batch.end() - 1, md::OptionVolume{0, market.time, 100});
      batch.insert(batch.end() - 1, md::OptionVolume{1, market.time, 100});
      for (const auto& event : batch) sink.publish(event);
      market.next();
    }
    sink.close();
  }
  for (const bool compact : {false, true}) {
    server::Desk::Options options;
    options.analytics.fallback_rate = 0;
    options.replay = true; options.compact_opening_rule_inputs = compact;
    options.exact_margin = compact;
    options.paper.rules.max_volume_percent = 10;
    options.run_input = server::recording_input(file.path);
    options.paper_journal = file.directory / (compact ? "compact.jsonl" : "full.jsonl");
    std::string snapshot;
    {
      server::Desk desk("replay", header.capabilities, header.subscription, options);
      desk.start_trading();
      md::RecordingReader reader(file.path);
      providers::ReplayBatches batches(reader, header.subscription);
      bool submitted = false;
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (submitted) continue;
        server::TradingCommand entry; entry.order = unused.limit("unused", 1, "0.90");
        const auto resting = command(desk, entry, desk.market_time(), batch->received);
        ASSERT_TRUE(resting.decision.ok()) << resting.decision.message;
        server::TradingCommand cancel; cancel.kind = server::TradingCommand::Kind::Cancel;
        cancel.order_id = *resting.order_id;
        ASSERT_TRUE(command(desk, cancel, desk.market_time(), batch->received).decision.ok());
        entry.order = market.limit("fill", 1, "4.00");
        ASSERT_TRUE(command(desk, entry, desk.market_time(), batch->received).decision.ok());
        submitted = true;
      }
      snapshot = json(*desk.trading_view()->snapshot).dump();
      desk.stop();
    }
    const auto recovery = trading::FileJournal::read(options.paper_journal);
    EXPECT_EQ(trading::TradingSession::recover(recovery).snapshot_json(), snapshot);
    const auto start = json::parse(server::run_inputs(recovery).front());
    EXPECT_EQ(start.contains("compact_opening_rule_inputs"), compact);
    EXPECT_EQ(start.contains("exact_margin"), compact);
    std::size_t checks = 0;
    for (const auto& record : recovery.records) {
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events")) {
        if (event.at("type") != "opening_rule_inputs") continue;
        ++checks;
        const auto& volumes = event.at("payload").at("inputs").at("volumes");
        EXPECT_EQ(volumes.size(), compact ? 1U : 2U);
        EXPECT_EQ(volumes.contains(unused.symbol()), !compact);
      }
    }
    EXPECT_EQ(checks, 1U);
    const auto verified = server::verify_run(options.paper_journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    const auto original = read_file(options.paper_journal);
    options.resume = std::make_shared<trading::JournalRecovery>(recovery);
    options.compact_opening_rule_inputs = !compact; // Resume must use the recorded choice.
    options.exact_margin = !compact;
    {
      server::Desk desk("replay", header.capabilities, header.subscription, options);
      desk.start_trading();
      md::RecordingReader reader(file.path);
      providers::ReplayBatches batches(reader, header.subscription);
      while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
      EXPECT_EQ(json(*desk.trading_view()->snapshot).dump(), snapshot);
      desk.stop();
    }
    EXPECT_EQ(read_file(options.paper_journal), original);
  }
}

TEST(Desk, OrderChainsDefineUnreferencedContractsBeforeAcceptanceAndFill) {
  test::ScriptedMarket market;
  test::ScriptedMarket other;
  other.contract = *md::parse_osi("SPXW261022P05000000");
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market, other), market.time);
  server::TradingCommand entry;
  entry.order = market.limit("oco-unreferenced", 1, "3.90");
  entry.order.oco = {other.limit("", 1, "1.00")};
  const auto oco = command(desk, entry, market.time, market.time);
  ASSERT_TRUE(oco.decision.ok()) << oco.decision.message;
  const auto first = *oco.order_id;
  const auto snapshot = desk.trading_view()->snapshot;
  ASSERT_EQ(snapshot->open_orders.size(), 2U);
  const auto first_order = std::find_if(snapshot->open_orders.begin(), snapshot->open_orders.end(),
      [&](const auto& order) { return order.id == first; });
  const auto other_order = std::find_if(snapshot->open_orders.begin(), snapshot->open_orders.end(),
      [&](const auto& order) { return order.request.client_order_id == "oco-unreferenced:oco"; });
  ASSERT_NE(first_order, snapshot->open_orders.end());
  ASSERT_NE(other_order, snapshot->open_orders.end());
  EXPECT_EQ(other_order->request.symbol, other.symbol());

  test::ScriptedMarket later = market;
  later.next();
  test::ScriptedMarket next = other;
  next.time = later.time;
  next.observation = later.observation;
  desk.replay_batch({md::UnderlyingQuote{"SPX", later.time, 5000, 5000, 5000},
                     md::OptionQuote{0, later.time, 3.70, 3.90, 20, 20},
                     md::SnapshotComplete{"SPX", later.time}}, later.time);
  entry.order = later.limit("then-unreferenced", 1, "3.80");
  entry.order.then = {next.limit("", 1, "1.00")};
  const auto chained = command(desk, entry, later.time, later.time);
  ASSERT_TRUE(chained.decision.ok()) << chained.decision.message;
  later.next();
  next.time = later.time;
  next.observation = later.observation;
  desk.replay_batch({md::UnderlyingQuote{"SPX", later.time, 5000, 5000, 5000},
                     md::OptionQuote{0, later.time, 3.60, 3.80, 20, 20},
                     md::OptionQuote{1, later.time, 0.80, 1.00, 20, 20},
                     md::SnapshotComplete{"SPX", later.time}}, later.time);
  const auto orders = desk.trading_view()->snapshot->recent_orders;
  ASSERT_GE(orders.size(), 4U);
  const auto& parent = orders.at(*chained.order_id - 1);
  ASSERT_NE(parent.chained, 0U);
  const auto& child = orders.at(parent.chained - 1);
  EXPECT_EQ(child.request.client_order_id, "then-unreferenced:then");
  EXPECT_EQ(child.request.symbol, other.symbol());
  EXPECT_EQ(child.chained_from, parent.id);
  EXPECT_EQ(child.status, trading::OrderStatus::Filled);
  desk.stop();
}

TEST(Desk, VolumeShareChecksEveryComboRatioWithAnExactPercentBoundaryInReplay) {
  test::ScriptedMarket market, other;
  other.contract.strike += 5;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.replay = true; options.paper.rules.max_volume_percent = 33;
  options.paper.limits.aggregate = {1e9, 1e9}; options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("replay", {}, {{"SPX"}}, options);
  desk.start_trading();
  auto batch = market_batch(market);
  batch.insert(batch.begin(), {md::ContractDefinition{1, other.contract}, md::OptionQuote{1, market.time, 4.0, 4.2, 20, 20}});
  batch.push_back(md::OptionVolume{0, market.time, 100});
  batch.push_back(md::OptionVolume{1, market.time, 3});
  desk.replay_batch(batch, market.time);
  server::TradingCommand entry;
  entry.order = market.market("ratio"); entry.order.symbol.clear();
  entry.order.legs = {{market.symbol(), trading::Side::Buy, 1}, {other.symbol(), trading::Side::Sell, 2}};
  auto refused = command(desk, entry, market.time, market.time);
  EXPECT_EQ(refused.decision.code, trading::Reason::MAX_VOLUME_SHARE);
  ASSERT_TRUE(refused.decision.evidence);
  EXPECT_EQ(refused.decision.evidence->contract, other.symbol());
  EXPECT_EQ(refused.decision.evidence->contracts, 2);
  desk.replay_batch({md::OptionVolume{1, market.time, 6}}, market.time);
  EXPECT_EQ(command(desk, entry, market.time, market.time).decision.code, trading::Reason::MAX_VOLUME_SHARE); // 200 > 198.
  desk.replay_batch({md::OptionVolume{1, market.time, 7}}, market.time);
  EXPECT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  desk.stop();
}

TEST_P(DeskPhases, CounterPositionsGatePreviewsChangesChainsAndIgnoresArchivedAccounts) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.paper.rules.phase = GetParam();
  options.analytics.fallback_rate = 0;
  options.paper_journal = file.directory / "paper.jsonl";
  options.paper_accounts = file.directory / "accounts";
  options.paper.rules.no_counter_positions = true;
  options.paper.limits.aggregate = {1e9, 1e9};
  options.paper.limits.per_underlying = {1e9, 1e9};
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  server::TradingCommand create;
  create.kind = server::TradingCommand::Kind::CreateAccount;
  create.name = "Other"; create.initial_cash = Money::parse("100000");
  ASSERT_EQ(command(desk, create, market.time, market.time).account, "other");
  desk.replay_batch(market_batch(market), market.time);
  server::TradingCommand entry;
  entry.order = market.limit("resting", 1, "4.50", trading::Side::Sell);
  const auto resting = command(desk, entry, market.time, market.time);
  ASSERT_TRUE(resting.decision.ok()) << resting.decision.message;
  entry.account = "other"; entry.order = market.market("long");
  ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  entry.account.clear(); entry.order = market.market("opposite", 1, trading::Side::Sell);
  const auto count = desk.trading_view()->snapshot->recent_orders.size();
  const auto refused = command(desk, entry, market.time, market.time);
  EXPECT_EQ(refused.decision.code, trading::Reason::COUNTER_POSITION);
  ASSERT_TRUE(refused.decision.evidence);
  EXPECT_EQ(refused.decision.evidence->other_account, "other");
  EXPECT_LT(refused.decision.evidence->order_dollar_delta, 0);
  EXPECT_GT(refused.decision.evidence->held_dollar_delta, 0);
  EXPECT_EQ(desk.trading_view()->snapshot->recent_orders.size(), count);
  entry.kind = server::TradingCommand::Kind::Preview;
  auto preview = command(desk, entry, market.time, market.time);
  ASSERT_TRUE(preview.preview);
  EXPECT_EQ(preview.preview->decision.code, trading::Reason::COUNTER_POSITION);
  server::TradingCommand change;
  change.kind = server::TradingCommand::Kind::Modify;
  change.order_id = *resting.order_id; change.change.quantity = 2;
  EXPECT_EQ(command(desk, change, market.time, market.time).decision.code, trading::Reason::COUNTER_POSITION);
  change.kind = server::TradingCommand::Kind::PreviewChange;
  preview = command(desk, change, market.time, market.time);
  ASSERT_TRUE(preview.preview);
  EXPECT_EQ(preview.preview->decision.code, trading::Reason::COUNTER_POSITION);
  entry.kind = server::TradingCommand::Kind::Submit;
  entry.order = market.market("chain");
  entry.order.then = {market.market("child", 2, trading::Side::Sell)};
  EXPECT_EQ(command(desk, entry, market.time, market.time).decision.code, trading::Reason::COUNTER_POSITION);
  // An opposite order that only reduces is exempt.
  change.kind = server::TradingCommand::Kind::Cancel;
  ASSERT_TRUE(command(desk, change, market.time, market.time).decision.ok());
  server::TradingCommand what_if;
  what_if.kind = server::TradingCommand::Kind::WhatIf;
  what_if.candidates = {{market.market("buy"), market.market("close", 1, trading::Side::Sell)},
                        {market.market("buy"), market.market("reverse", 2, trading::Side::Sell)}};
  const auto projection = command(desk, what_if, market.time, market.time);
  ASSERT_TRUE(projection.what_if);
  ASSERT_EQ(projection.what_if->candidates.size(), 2U);
  EXPECT_TRUE(projection.what_if->candidates[0].decision.ok());
  EXPECT_EQ(projection.what_if->candidates[1].decision.code, trading::Reason::COUNTER_POSITION);
  entry.order = market.market("same-direction");
  ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  entry.order = market.market("reduce", 1, trading::Side::Sell);
  ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  server::TradingCommand archive;
  archive.kind = server::TradingCommand::Kind::UpdateAccount;
  archive.account = "other"; archive.archived = true;
  ASSERT_TRUE(command(desk, archive, market.time, market.time).error_code.empty());
  entry.order = market.market("archived-ignored", 1, trading::Side::Sell);
  ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  archive.archived = false;
  ASSERT_TRUE(command(desk, archive, market.time, market.time).error_code.empty());
  // Only the ordering account's setting counts.
  entry.account = "other"; entry.order = market.market("unguarded");
  ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
  desk.stop();
}

TEST(Desk, CounterPositionShareGateUsesRecoveredPricesBeforeFeedArrives) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPY261022C00500000");
  server::Desk::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  options.paper_accounts = file.directory / "accounts";
  options.paper.rules.no_counter_positions = true;
  std::filesystem::create_directory(options.paper_accounts);
  for (const auto& path : {options.paper_journal, options.paper_accounts / "other.jsonl"}) {
    trading::TradingSession session(options.paper, market.time, trading::FileJournal::create(path.string()));
    market.seed(session);
    ASSERT_TRUE(session.trade_stock("SPY", 10, market.time,
        trading::StockPrice{"SPY", market.time, Money::parse("500")}).decision.ok());
  }
  server::Desk desk("test", {}, {{"SPY"}}, options);
  desk.start_trading();
  ASSERT_EQ(desk.accounts().size(), 2U);
  server::TradingCommand stock;
  stock.kind = server::TradingCommand::Kind::TradeStock; stock.symbol = "SPY"; stock.quantity = -11;
  const auto refused = command(desk, stock, market.time, market.time);
  EXPECT_EQ(refused.decision.code, trading::Reason::COUNTER_POSITION) << refused.decision.message;
  ASSERT_TRUE(refused.decision.evidence);
  EXPECT_EQ(refused.decision.evidence->order_dollar_delta, -5500);
  EXPECT_EQ(refused.decision.evidence->other_account, "other");
  desk.stop();
}

TEST(Desk, CounterPositionsIncludeShareEntriesAndAllowShareReductions) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.paper_accounts = file.directory / "accounts";
  options.paper.rules.no_counter_positions = true;
  server::Desk desk("test", {}, {{"SPY"}}, options);
  desk.start_trading();
  server::TradingCommand create;
  create.kind = server::TradingCommand::Kind::CreateAccount;
  create.name = "Other"; create.initial_cash = Money::parse("100000");
  ASSERT_EQ(command(desk, create, market.time, market.time).account, "other");
  desk.replay_batch({md::UnderlyingQuote{"SPY", market.time, 500.123456, 500.123456, 500.123456},
                    md::SnapshotComplete{"SPY", market.time}}, market.time);
  server::TradingCommand stock;
  stock.kind = server::TradingCommand::Kind::TradeStock;
  stock.symbol = "SPY"; stock.quantity = 10;
  ASSERT_TRUE(command(desk, stock, market.time, market.time).decision.ok());
  stock.account = "other";
  ASSERT_TRUE(command(desk, stock, market.time, market.time).decision.ok());
  stock.account.clear(); stock.quantity = -11;
  const auto refused = command(desk, stock, market.time, market.time);
  EXPECT_EQ(refused.decision.code, trading::Reason::COUNTER_POSITION);
  EXPECT_EQ(refused.decision.message, "Opening SPY dollar delta -$5,501 opposes account other held or working dollar delta +$5,001");
  ASSERT_TRUE(refused.decision.actual);
  ASSERT_TRUE(refused.decision.limit);
  EXPECT_DOUBLE_EQ(*refused.decision.actual, -5501.358016);
  EXPECT_DOUBLE_EQ(*refused.decision.limit, 5001.23456);
  ASSERT_TRUE(refused.decision.evidence);
  EXPECT_EQ(refused.decision.evidence->order_dollar_delta, refused.decision.actual);
  EXPECT_EQ(refused.decision.evidence->held_dollar_delta, refused.decision.limit);
  stock.kind = server::TradingCommand::Kind::PreviewStock;
  const auto preview = command(desk, stock, market.time, market.time);
  ASSERT_TRUE(preview.stock_preview);
  EXPECT_EQ(preview.stock_preview->decision.code, trading::Reason::COUNTER_POSITION);
  stock.kind = server::TradingCommand::Kind::TradeStock; stock.quantity = -10;
  EXPECT_TRUE(command(desk, stock, market.time, market.time).decision.ok());
  desk.stop();
}

TEST(Desk, DemoAmSettlementUsesFirstExpiryOpeningPrintAndLiveKeepsManualImport) {
  for (const auto* provider : {"demo", "live"}) {
    for (const bool manual : {false, true}) {
      test::ScriptedMarket market;
      market.contract = *md::parse_osi("SPX260923C05000000");
      server::Desk::Options options;
      server::Desk desk(provider, {}, {{"SPX"}}, options);
      desk.start_trading();
      desk.replay_batch(market_batch(market), market.time);
      server::TradingCommand entry;
      entry.order = market.market("am-entry");
      ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
      const auto open = market.contract.expiry_time();
      // Neither another date's open nor today's premarket price settles the position.
      desk.replay_batch({md::UnderlyingQuote{"SPX", open - md::kNanosPerMinute, 0, 0, 5020}}, open - md::kNanosPerMinute);
      ASSERT_EQ(desk.trading_view()->snapshot->positions.size(), 1U);
      desk.replay_batch({md::UnderlyingQuote{"SPY", open, 0, 0, 500}}, open);
      ASSERT_EQ(desk.trading_view()->snapshot->positions.size(), 1U);
      EXPECT_TRUE(desk.trading_view()->snapshot->positions[0].awaiting_settlement);
      EXPECT_EQ(desk.trading_view()->opening_settlement, std::string_view(provider) == "demo");
      if (manual) {
        server::TradingCommand settle;
        settle.kind = server::TradingCommand::Kind::Settle;
        settle.symbol = market.symbol();
        settle.settlement = Money::parse("5007.125");
        ASSERT_TRUE(command(desk, settle, open, open).decision.ok());
      }
      // Arrival after 09:30 still counts, but a later print in the batch cannot replace it.
      const auto print_time = open + md::kNanosPerSecond;
      desk.replay_batch({md::UnderlyingQuote{"SPX", print_time, 0, 0, 5011.125},
                         md::UnderlyingQuote{"SPX", print_time + md::kNanosPerSecond, 0, 0, 5099}}, print_time + md::kNanosPerSecond);
      const auto snapshot = desk.trading_view()->snapshot;
      if (!manual && std::string_view(provider) == "live") {
        EXPECT_EQ(snapshot->positions.size(), 1U);
        EXPECT_TRUE(snapshot->settlements.empty());
      } else {
        EXPECT_TRUE(snapshot->positions.empty());
        ASSERT_EQ(snapshot->settlements.size(), 1U);
        const auto& record = snapshot->settlements.front();
        EXPECT_EQ(record.value, Money::parse(manual ? "5007.125" : "5011.125"));
        ASSERT_TRUE(record.source);
        EXPECT_EQ(record.source->at("kind"), manual ? "manual_am_import" : "demo_opening_print");
        if (!manual) {
          EXPECT_EQ(record.source->at("quote_time"), md::format_timestamp(print_time));
          EXPECT_EQ(record.source->at("symbol"), "SPX");
          EXPECT_EQ(record.source->at("provider"), "demo");
        }
      }
    }
  }
}

TEST(Desk, DemoLateRestartDoesNotInventAnAmOpeningPrint) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPX260923C05000000");
  server::Desk::Options options;
  options.paper_journal = file.directory / "am.jsonl";
  {
    server::Desk desk("demo", {}, {{"SPX"}}, options);
    desk.start_trading();
    desk.replay_batch(market_batch(market), market.time);
    server::TradingCommand entry;
    entry.order = market.market("am-entry");
    ASSERT_TRUE(command(desk, entry, market.time, market.time).decision.ok());
    desk.stop();
  }
  server::Desk restarted("demo", {}, {{"SPX"}}, options);
  restarted.start_trading();
  const auto late = market.contract.expiry_time() + 90 * md::kNanosPerMinute;
  restarted.replay_batch({md::UnderlyingQuote{"SPX", late, 0, 0, 5011}}, late);
  const auto view = restarted.trading_view();
  ASSERT_EQ(view->snapshot->positions.size(), 1U);
  EXPECT_TRUE(view->snapshot->positions.front().awaiting_settlement);
  EXPECT_TRUE(view->snapshot->settlements.empty());
  restarted.stop();
}

TEST(ReproducibleRun, AmOpeningSettlementIsGatedByDriverAndNamesScenarioOrRecording) {
  const test::SanitizerScenarioScale scale;
  test::RecordingFile file;
  const auto source = file.directory / "am-overnight.json";
  {
    std::ofstream out(source);
    out << R"({"id":"am-overnight","title":"AM expiry","description":"Hold an AM monthly into its opening print","symbols":["SPX"],"date":"2026-09-17","seed":81723,"generator":1,"volatility":0.12,"iv_shift":0,"spot_vol":-2,"sessions":[{"session":"regular","drift":[[1,0.001]]},{"session":"regular","drift":[[1,0.001]]}]})";
  }
  const auto scenario = providers::read_scenario(source);
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  const auto contract = *md::parse_osi("SPX260918C06000000");
  for (const auto* kind : {"scenario", "recording"}) {
    for (const int driver : {4, 5}) {
      SCOPED_TRACE(std::string(kind) + " driver " + std::to_string(driver));
      const auto journal = file.directory / (std::string(kind) + std::to_string(driver) + ".jsonl");
      Money reference;
      {
        md::RecordingReader reader(file.path);
        server::Desk::Options options;
        options.replay = true;
        options.opening_settlement = driver >= 5;
        options.run_input = std::string_view(kind) == "scenario"
            ? server::scenario_input(scenario, scenario.date, scenario.seed) : server::recording_input(file.path);
        options.paper_journal = journal;
        server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
        desk.start_trading();
        providers::ReplayBatches batches(reader, reader.header().subscription);
        bool bought = false;
        while (const auto batch = batches.next()) {
          if (batch->time > contract.expiry_time()) break;
          for (const auto& event : batch->events) {
            if (const auto* quote = std::get_if<md::UnderlyingQuote>(&event);
                quote && quote->symbol == "SPX" && quote->ts == contract.expiry_time()) reference = Money::from_double(quote->last);
          }
          desk.replay_batch(batch->events, batch->received, batch->time);
          if (!bought) {
            server::TradingCommand entry;
            entry.order.client_order_id = "am-entry";
            entry.order.symbol = contract.osi_symbol();
            entry.order.quantity = 1;
            entry.order.type = trading::OrderType::Market;
            entry.order.tif = trading::TimeInForce::Ioc;
            const auto result = command(desk, entry, desk.market_time(), batch->received);
            ASSERT_TRUE(result.decision.ok()) << result.decision.message;
            ASSERT_EQ(result.view->snapshot->positions.size(), 1U);
            bought = true;
          }
        }
        ASSERT_TRUE(bought);
        ASSERT_GT(reference, Money{});
        const auto snapshot = desk.trading_view()->snapshot;
        if (driver == 4) {
          EXPECT_EQ(snapshot->positions.size(), 1U);
          EXPECT_TRUE(snapshot->settlements.empty());
        } else {
          EXPECT_TRUE(snapshot->positions.empty());
          ASSERT_EQ(snapshot->settlements.size(), 1U);
          const auto& record = snapshot->settlements.front();
          EXPECT_EQ(record.value, reference);
          EXPECT_EQ(record.time, contract.expiry_time());
          ASSERT_TRUE(record.source);
          EXPECT_EQ(record.source->at("kind"), std::string_view(kind) == "scenario" ? "scenario_opening_print" : "recorded_opening_print");
          EXPECT_EQ(record.source->at("provider"), "replay (demo)");
          EXPECT_EQ(record.source->at("quote_time"), md::format_timestamp(contract.expiry_time()));
        }
        desk.stop();
      }
      const auto verified = server::verify_run(journal);
      EXPECT_TRUE(verified.matched) << verified.message;
      auto recovered = trading::TradingSession::recover(trading::FileJournal::read(journal.string()));
      EXPECT_EQ(recovered.snapshot()->settlements.size(), driver == 5 ? 1U : 0U);
    }
  }
}

TEST(Desk, NamedAccountsListInIdOrderBeforeAndAfterARestart) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.paper_journal = file.directory / "paper-journal.jsonl";
  options.paper_accounts = file.directory / "accounts";
  const auto ids = [](const server::Desk& desk) {
    std::vector<std::string> result;
    for (const auto& account : desk.accounts()) result.push_back(account.id);
    return result;
  };
  // "swing-50k-2.jsonl" sorts before "swing-50k.jsonl"; an account created later can sort earlier.
  const std::vector<std::string> expected{"main", "alpha", "swing-50k", "swing-50k-2"};
  {
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    for (const auto* name : {"Swing 50k", "Swing 50k", "Alpha"}) {
      server::TradingCommand create;
      create.kind = server::TradingCommand::Kind::CreateAccount;
      create.name = name;
      create.rules = server::find_plan("practice")->rules;
      create.initial_cash = server::find_plan("practice")->initial_cash;
      const auto reply = command(desk, create, market.time, market.time);
      ASSERT_FALSE(reply.account.empty()) << reply.decision.message;
    }
    EXPECT_EQ(ids(desk), expected);
    desk.stop();
  }
  server::Desk restarted("test", {}, {{"SPX"}}, options);
  restarted.start_trading();
  EXPECT_EQ(ids(restarted), expected);
  restarted.stop();
}

TEST(Desk, AccountLifecycleSurvivesRestartAndReservesDeletedIds) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.paper_journal = file.directory / "paper.jsonl";
  options.paper_accounts = file.directory / "accounts";
  server::TradingCommand create;
  create.kind = server::TradingCommand::Kind::CreateAccount;
  create.name = "Evaluation";
  create.rules = server::find_plan("eod-50k")->rules;
  create.initial_cash = server::find_plan("eod-50k")->initial_cash;
  server::TradingCommand change;
  change.kind = server::TradingCommand::Kind::UpdateAccount;
  change.account = "evaluation";
  change.name = "Renamed 東京";
  change.archived = true;
  {
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    ASSERT_EQ(command(desk, create, market.time, market.time).account, "evaluation");
    ASSERT_TRUE(command(desk, change, market.time, market.time).error_code.empty());
    const auto version = desk.trading_view("evaluation")->snapshot->account_version;
    desk.replay_batch(market_batch(market), market.time);
    EXPECT_EQ(desk.trading_view("evaluation")->snapshot->account_version, version);
    server::TradingCommand order;
    order.account = "evaluation";
    order.order = market.market("archived-order");
    EXPECT_EQ(command(desk, order, market.time, market.time).error_code, "ACCOUNT_ARCHIVED");
    change.account = "main";
    EXPECT_EQ(command(desk, change, market.time, market.time).error_code, "ACCOUNT_PROTECTED");
    change.account = "missing";
    EXPECT_EQ(command(desk, change, market.time, market.time).error_code, "UNKNOWN_ACCOUNT");
    desk.stop();
  }
  {
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    ASSERT_EQ(desk.accounts().size(), 2U);
    EXPECT_EQ(desk.accounts()[1].name, "Renamed 東京");
    EXPECT_TRUE(desk.accounts()[1].archived);
    EXPECT_EQ(desk.accounts()[1].trading.plan_id, "eod-50k");
    change.account = "evaluation";
    change.name.clear();
    change.archived = false;
    ASSERT_TRUE(command(desk, change, market.time, market.time).error_code.empty());
    desk.replay_batch(market_batch(market), market.time);
    server::TradingCommand order;
    order.account = "evaluation";
    order.order = market.market("working");
    order.order.type = trading::OrderType::Limit;
    order.order.tif = trading::TimeInForce::Day;
    order.order.limit_price = Money::from_double(4);
    const auto working = command(desk, order, market.time, market.time);
    ASSERT_TRUE(working.decision.ok()) << working.decision.message;
    ASSERT_EQ(working.view->snapshot->open_orders.size(), 1U);
    server::TradingCommand remove;
    remove.kind = server::TradingCommand::Kind::DeleteAccount;
    remove.account = "evaluation";
    EXPECT_EQ(command(desk, remove, market.time, market.time).error_code, "ACCOUNT_NOT_EMPTY");
    change.archived = true;
    ASSERT_TRUE(command(desk, change, market.time, market.time).error_code.empty());
    ASSERT_TRUE(command(desk, remove, market.time, market.time).error_code.empty());
    EXPECT_FALSE(desk.trading_view("evaluation"));
    EXPECT_TRUE(std::filesystem::exists(options.paper_accounts / "deleted/evaluation/evaluation.jsonl"));
    ASSERT_EQ(command(desk, create, market.time, market.time).account, "evaluation-2");
    desk.stop();
  }
  // Simulate a crash after committing the deletion tombstone, before moving files.
  std::filesystem::create_directories(options.paper_accounts / "deleted/evaluation-2");
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  EXPECT_EQ(desk.accounts().size(), 1U);
  EXPECT_TRUE(std::filesystem::exists(options.paper_accounts / "deleted/evaluation-2/evaluation-2.jsonl"));
  EXPECT_EQ(command(desk, create, market.time, market.time).account, "evaluation-3");
  create.name = "Sandbox trial";  // A display name does not make this a visitor sandbox.
  EXPECT_EQ(command(desk, create, market.time, market.time).account, "sandbox-trial");
  change.account = "sandbox-trial";
  change.archived = true;
  EXPECT_TRUE(command(desk, change, market.time, market.time).error_code.empty());
  desk.stop();
}

TEST(Desk, PresetIdKeepsBrokerOverridesAndRejectsCustomObjectives) {
  for (const auto& plan : server::plan_presets()) {
    auto rules = plan.rules;
    EXPECT_EQ(server::preset_id(plan.initial_cash, rules), plan.id);
    rules.slippage_ticks = 1;
    rules.fill_latency_ms = 1000;
    rules.margin = trading::MarginMode::Portfolio;
    rules.house_margin_percent = 20;
    EXPECT_EQ(server::preset_id(plan.initial_cash, rules), plan.id);
    rules.profit_target = rules.profit_target + Money::from_double(1);
    EXPECT_TRUE(server::preset_id(plan.initial_cash, rules).empty());
  }
}

TEST(Desk, CopiesActiveSettingsAsInitialStateAndRecoversThem) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.paper_accounts = file.directory / "accounts";
  options.paper.rules = server::find_plan("eod-50k")->rules;
  options.paper.initial_cash = server::find_plan("eod-50k")->initial_cash;
  options.paper.limits.aggregate.dollar_delta = 400'000;
  options.paper.limits.underlying_overrides["SPX"] = {100'000, 1000};
  options.paper.guardrails.max_opening_trades = 3;
  {
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    desk.replay_batch(market_batch(market), market.time);
    server::TradingCommand limits;
    limits.kind = server::TradingCommand::Kind::Limits;
    limits.expected_revision = desk.trading_view()->snapshot->risk.limits_revision;
    limits.limits = options.paper.limits;
    limits.limits.aggregate.dollar_delta = 800'000;
    ASSERT_TRUE(command(desk, limits, market.time, market.time).decision.ok());
    ASSERT_TRUE(desk.trading_view()->snapshot->pending_limits);
    server::TradingCommand guardrails;
    guardrails.kind = server::TradingCommand::Kind::Guardrails;
    guardrails.expected_revision = desk.trading_view()->snapshot->risk.limits_revision;
    guardrails.guardrails.max_opening_trades = 6;
    ASSERT_TRUE(command(desk, guardrails, market.time, market.time).decision.ok());
    ASSERT_TRUE(desk.trading_view()->snapshot->pending_guardrails);
    server::TradingCommand create;
    create.kind = server::TradingCommand::Kind::CreateAccount;
    create.name = "Copy";
    create.rules = options.paper.rules;
    create.initial_cash = options.paper.initial_cash;
    create.copy_settings_from = "missing";
    EXPECT_EQ(command(desk, create, market.time, market.time).error_code, "UNKNOWN_ACCOUNT");
    create.copy_settings_from = "main";
    const auto result = command(desk, create, market.time, market.time);
    ASSERT_EQ(result.account, "copy") << result.decision.message;
    EXPECT_EQ(result.view->config.limits.aggregate.dollar_delta, 400'000);
    EXPECT_EQ(result.view->config.limits.underlying_overrides.at("SPX").dollar_delta, 100'000);
    EXPECT_EQ(result.view->config.guardrails.max_opening_trades, 3);
    EXPECT_FALSE(result.view->snapshot->pending_limits);
    EXPECT_FALSE(result.view->snapshot->pending_guardrails);
    desk.stop();
  }
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  const auto copied = desk.trading_view("copy");
  ASSERT_TRUE(copied);
  EXPECT_EQ(copied->config.limits.aggregate.dollar_delta, 400'000);
  EXPECT_EQ(copied->config.guardrails.max_opening_trades, 3);
  EXPECT_FALSE(copied->snapshot->pending_limits);
  desk.stop();
}

TEST(Desk, OnlyExplicitReplayJournalsBatchSyncs) {
  for (const bool replay : {false, true}) {
    test::RecordingFile file;
    test::ScriptedMarket market;
    int syncs = 0;
    server::Desk::Options options;
    options.replay = replay;
    options.analytics.fallback_rate = 0;
    options.paper_journal = file.directory / "account.jsonl";
    options.journal_io.sync = [&](int) { ++syncs; return true; };
    options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
    server::Desk desk("test", {}, {{"SPX"}}, options);
    desk.start_trading();
    desk.replay_batch(market_batch(market), market.time);
    server::TradingCommand order;
    order.order = market.market("sync-policy");
    const auto reply = command(desk, order, market.time, market.time);
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    const auto records = trading::FileJournal::read(options.paper_journal.string()).records.size();
    ASSERT_GT(records, 1U);
    EXPECT_EQ(static_cast<std::size_t>(syncs), replay ? 1U : records);
    desk.stop();
    EXPECT_EQ(static_cast<std::size_t>(syncs), replay ? 2U : records);
    EXPECT_NO_THROW(trading::FileJournal::resume(options.paper_journal.string()));
  }
}

TEST(Desk, FailedBoundarySyncDisablesTradingAndPreservesThePublishedAccount) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  bool fail = false;
  server::Desk::Options options;
  options.replay = true;
  options.paper_journal = file.directory / "account.jsonl";
  options.journal_io.sync = [&](int) { return !fail; };
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  const auto before = desk.trading_view()->snapshot;
  fail = true;
  desk.flush_journals();
  EXPECT_NE(desk.trading_status().reason.find("JOURNAL_IO"), std::string::npos);
  EXPECT_EQ(desk.trading_status().write, "disabled");
  server::TradingCommand order;
  order.order = market.market("failed-sync");
  const auto reply = command(desk, order, market.time, market.time);
  EXPECT_EQ(reply.error_code, "TRADING_UNAVAILABLE");
  EXPECT_EQ(desk.trading_view()->snapshot->account_version, before->account_version);
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_fills.empty());
}

TEST(Desk, ExplicitClockControlsFeedStallWithoutThreads) {
  test::ScriptedMarket market;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  server::TradingCommand order;
  order.order = market.market("stale");
  auto result = command(desk, order, market.time, market.time + 10 * md::kNanosPerMinute);
  EXPECT_EQ(result.decision.code, trading::Reason::FEED_STALLED);
  order.order.client_order_id = "fresh";
  result = command(desk, order, market.time, market.time);
  EXPECT_TRUE(result.decision.ok()) << result.decision.message;
  ASSERT_EQ(result.view->snapshot->recent_fills.size(), 1U);
  EXPECT_EQ(result.view->snapshot->recent_fills.front().price, Money::parse("4.20"));
}
TEST(Desk, SnapshotsDoNotAnalyseOtherUnderlyingsBeforeTheirCompleteBoundary) {
  test::ScriptedMarket market;
  auto spy = *md::parse_osi("SPY261022C00500000");
  server::Desk::Options options;
  options.replay = true;
  server::Desk desk("test", {}, {{"SPX", "SPY"}}, options);
  desk.start_trading();
  auto batch = market_batch(market);
  batch.insert(batch.begin(), md::ContractDefinition{1, spy});
  batch.insert(batch.begin(), md::UnderlyingQuote{"SPY", market.time, 500, 500, 500});
  desk.replay_batch(batch, market.time);
  EXPECT_FALSE(desk.metrics("SPY"));
  desk.replay_batch({md::OptionQuote{1, market.time, 4, 4.2, 20, 20}, md::SnapshotComplete{"SPY", market.time}}, market.time);
  ASSERT_TRUE(desk.metrics("SPY"));
  EXPECT_EQ(desk.metrics("SPY")->options_priced, 1);
}

TEST(Desk, AnalyticsInputsAndEquityHooksUseExplicitMarketTime) {
  test::ScriptedMarket market;
  std::vector<server::EquitySample> samples;
  server::Desk::Options options;
  options.analytics.fallback_rate = 0;
  options.equity_sample = [&](std::string_view account, const server::EquitySample& sample) {
    EXPECT_EQ(account, "main");
    samples.push_back(sample);
  };
  server::Desk desk("test", {}, {{"SPX"}}, options);
  desk.start_trading();
  desk.replay_batch(market_batch(market), market.time);
  const auto analytics = desk.metrics("SPX");
  ASSERT_TRUE(analytics);
  EXPECT_THROW(desk.apply_analytics(analytics, market.time + 1), std::invalid_argument);
  desk.apply_analytics(analytics, market.time);
  server::TradingCommand order;
  order.order = market.market("sample");
  const auto reply = command(desk, order, market.time, market.time);
  ASSERT_TRUE(reply.decision.ok());
  ASSERT_FALSE(samples.empty());
  EXPECT_EQ(samples.back().time, market.time);
  EXPECT_EQ(samples.back().equity, reply.view->snapshot->equity);
}

/// A desk whose main account keeps its equity history beside a journal, fed one
/// SPX call's quotes at explicit market times.
struct EquityDesk {
  test::RecordingFile file;
  test::ScriptedMarket market;
  std::unique_ptr<server::Desk> desk;
  explicit EquityDesk(const trading::SessionConfig& paper) {
    server::Desk::Options options;
    options.analytics.fallback_rate = 0;
    options.paper = paper;
    options.paper.limits.aggregate = {1e9, 1e9};
    options.paper.limits.per_underlying = {1e9, 1e9};
    options.paper_journal = file.directory / "main.jsonl";
    desk = std::make_unique<server::Desk>("test", md::Capabilities{}, md::Subscription{{"SPX"}}, options);
    desk->start_trading();
  }
  void quotes(md::Timestamp time, double bid, double ask) {
    market.time = time;
    desk->replay_batch({md::ContractDefinition{0, market.contract}, md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000},
                        md::OptionQuote{0, time, bid, ask, 20, 20}, md::SnapshotComplete{"SPX", time}}, time);
  }
  void buy(trading::Quantity quantity) {
    server::TradingCommand request;
    request.order = market.market("buy-" + std::to_string(market.time), quantity);
    const auto reply = command(*desk, request, market.time, market.time);
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  }
  std::shared_ptr<const trading::TradingSnapshot> snapshot() const { return desk->trading_view()->snapshot; }
  const std::vector<server::EquitySample>& samples() const { return desk->trading_view()->equity_samples; }
};

TEST(DeskEquity, APassedAttemptsHistoryHoldsThePassingEquityAndTomorrowsFloorStopsRising) {
  // B25: the transaction that passed also liquidated at the bid, and only the fill's
  // lower equity was stored, so the chart of a passed day never reached its target.
  // B27: once decided, an end-of-day attempt's floor no longer ratchets at rollover.
  for (const auto mode : {trading::DrawdownMode::Intraday, trading::DrawdownMode::EndOfDay}) {
    trading::SessionConfig paper;
    paper.initial_cash = Money::parse("10000");
    paper.rules.profit_target = Money::parse("100");
    paper.rules.max_drawdown = Money::parse("100");
    paper.rules.drawdown_mode = mode;
    EquityDesk f(paper);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
    f.buy(1);
    const auto pass = md::new_york_to_utc({2026, 9, 22}, 10, 0) + 15 * md::kNanosPerSecond;
    f.quotes(pass, 5.20, 5.40);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 1), 5.20, 5.40);
    const auto e = f.snapshot()->evaluation;
    ASSERT_EQ(e.status, trading::EvaluationStatus::Passed);
    ASSERT_EQ(e.decided_equity, Money::parse("10109.35"));
    EXPECT_EQ(f.snapshot()->equity, Money::parse("10098.70"));  // sold at the 5.20 bid
    const auto& samples = f.samples();
    const auto passing = std::find_if(samples.begin(), samples.end(), [&](const server::EquitySample& s) {
      return s.time == pass && s.fill == 0 && s.equity == e.decided_equity;
    });
    ASSERT_NE(passing, samples.end());
    EXPECT_EQ(passing->target, Money::parse("10100"));
    std::size_t later = 0;
    for (const auto& sample : samples) {
      if (sample.time < pass) continue;
      ++later;
      if (mode == trading::DrawdownMode::EndOfDay) { EXPECT_EQ(sample.tomorrow_floor, sample.floor); }
      else { EXPECT_FALSE(sample.tomorrow_floor); }
    }
    EXPECT_GE(later, 3U);  // the passing mark, the liquidation fill and 10:01
  }
}

TEST(DeskEquity, ARolloverLiquidationIsStoredUnderTheRatchetedFloor) {
  // B26: the end-of-day ratchet and the liquidation it caused share the rollover
  // transaction, and the fill's sample carried the previous day's peak and floor.
  trading::SessionConfig paper;
  paper.initial_cash = Money::parse("10000");
  paper.rules.max_drawdown = Money::parse("100");
  paper.rules.drawdown_mode = trading::DrawdownMode::EndOfDay;
  EquityDesk f(paper);
  f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
  f.buy(5);
  f.quotes(md::new_york_to_utc({2026, 9, 22}, 15, 59), 4.40, 4.60);  // closes at 10,146.75
  f.quotes(md::new_york_to_utc({2026, 9, 23}, 9, 30), 4.00, 4.20);
  const auto snapshot = f.snapshot();
  ASSERT_EQ(snapshot->evaluation.status, trading::EvaluationStatus::Failed);
  ASSERT_EQ(snapshot->recent_fills.size(), 2U);
  const auto& samples = f.samples();
  const auto liquidation = std::find_if(samples.begin(), samples.end(), [](const server::EquitySample& s) { return s.fill == 2; });
  ASSERT_NE(liquidation, samples.end());
  EXPECT_EQ(liquidation->peak, Money::parse("10146.75"));
  EXPECT_EQ(liquidation->floor, Money::parse("10046.75"));
  EXPECT_EQ(liquidation->tomorrow_floor, liquidation->floor);
}

TEST(DeskPlans, RecordedLegacyFundedResetKeepsItsLock) {
  const auto* funded = server::find_plan("funded-intraday-25k"); ASSERT_NE(funded, nullptr);
  trading::SessionConfig paper; paper.initial_cash = funded->initial_cash; paper.rules = funded->rules;
  EquityDesk f(paper);
  f.quotes(f.market.time, 4.00, 4.20);
  server::TradingCommand reset;
  reset.kind = server::TradingCommand::Kind::ResetAccount; reset.initial_cash = funded->initial_cash;
  reset.rules = funded->rules; reset.required_pass = "Intraday 25K"; reset.reason = "restart";
  auto legacy = command(*f.desk, reset, f.market.time, f.market.time);
  EXPECT_EQ(legacy.decision.code, trading::Reason::PLAN_LOCKED);
  EXPECT_EQ(legacy.decision.message, "Pass the Intraday 25K evaluation to start this funded account");
  EXPECT_TRUE(f.snapshot()->attempts.empty());
  reset.program_costs = true;
  ASSERT_TRUE(command(*f.desk, reset, f.market.time, f.market.time).decision.ok());
  EXPECT_EQ(f.snapshot()->attempts.size(), 1U);
}

TEST(DeskPlans, ProgramCostCommandDefaultsRetainOldBytes) {
  server::TradingCommand command;
  command.kind = server::TradingCommand::Kind::ResetAccount;
  const nlohmann::json old = command;
  EXPECT_FALSE(old.contains("program_costs"));
  EXPECT_FALSE(old.contains("restore_scaled_rules"));
  EXPECT_EQ(nlohmann::json(old.get<server::TradingCommand>()).dump(), old.dump());
  command.program_costs = true;
  command.restore_scaled_rules = true;
  const nlohmann::json current = command;
  EXPECT_TRUE(current.at("program_costs"));
  EXPECT_TRUE(current.at("restore_scaled_rules"));
  EXPECT_EQ(nlohmann::json(current.get<server::TradingCommand>()).dump(), current.dump());
  const auto* plan = server::find_plan("two-step-25k"); ASSERT_NE(plan, nullptr);
  auto rules = plan->rules;
  rules.evaluation_fee = Money::parse("1.000001"); rules.reset_fee = {}; rules.activation_fee = Money::parse("15"); rules.max_resets = 7;
  EXPECT_TRUE(server::follows_plan(*plan, plan->initial_cash, rules));
  rules.profit_target = Money::parse("1");
  EXPECT_FALSE(server::follows_plan(*plan, plan->initial_cash, rules));
}

TEST(DeskPlans, EveryPresetIsValidAndTheObjectivePresetsNameTheirFloors) {
  for (const auto& plan : server::plan_presets()) {
    EXPECT_NO_THROW(trading::validate_rules(plan.rules)) << plan.id;
    EXPECT_TRUE(server::follows_plan(plan, plan.initial_cash, plan.rules)) << plan.id;
  }
  const auto* fixed = server::find_plan("static-25k");
  const auto* locking = server::find_plan("locking-25k");
  ASSERT_TRUE(fixed && locking);
  EXPECT_EQ(fixed->rules.drawdown_mode, trading::DrawdownMode::Static);
  EXPECT_EQ(fixed->rules.max_drawdown, Money::parse("2000"));
  EXPECT_EQ(fixed->rules.day_end_minutes, 18 * 60);
  EXPECT_TRUE(locking->rules.lock_at_start);
  EXPECT_EQ(locking->rules.lock_balance, Money{});
  EXPECT_EQ(locking->rules.daily_loss_action, trading::BreachAction::Lock);
  // A static floor sits below the start from the first moment; a locking one trails to it.
  bool locked = false;
  EXPECT_EQ(trading::evaluation_floor(fixed->rules, Money::parse("30000"), locked, fixed->initial_cash), Money::parse("23000"));
  EXPECT_TRUE(locked);
  locked = false;
  EXPECT_EQ(trading::evaluation_floor(locking->rules, Money::parse("26500"), locked, locking->initial_cash), Money::parse("25000"));
  EXPECT_TRUE(locked);
}

TEST(DeskPlans, EachAccountRollsOverAtItsPlansOwnDayEnd) {
  // At 17:30 New York time the default trading day has ended; a plan whose day
  // ends at 18:00 still counts the evening toward the day it follows.
  for (const std::int64_t day_end : {std::int64_t{17 * 60}, std::int64_t{18 * 60}}) {
    trading::SessionConfig paper;
    paper.initial_cash = Money::parse("10000");
    paper.rules.max_drawdown = Money::parse("100");
    paper.rules.day_end_minutes = day_end;
    EquityDesk f(paper);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 17, 30), 4.00, 4.20);
    EXPECT_EQ(f.snapshot()->evaluation.days.size(), day_end == 17 * 60 ? 1U : 0U) << day_end;
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 18, 5), 4.00, 4.20);
    const auto& e = f.snapshot()->evaluation;
    ASSERT_EQ(e.days.size(), 1U) << day_end;
    EXPECT_EQ(e.days[0].day, (md::Date{2026, 9, 22}));
    EXPECT_EQ(e.day, (md::Date{2026, 9, 23}));
  }
}

TEST(DeskEquity, OnlyAPassOfThePresetItselfUnlocksItsFundedPlan) {
  // B36: custom rules that only borrowed the name "Intraday 25K" unlocked funded-intraday-25k.
  const auto* evaluation = server::find_plan("intraday-25k");
  const auto* funded = server::find_plan("funded-intraday-25k");
  ASSERT_TRUE(evaluation && funded);
  for (const bool preset : {false, true}) {
    trading::SessionConfig paper;
    paper.initial_cash = preset ? evaluation->initial_cash : Money::parse("1000000");
    paper.rules = evaluation->rules;
    if (!preset) paper.rules.profit_target = Money::parse("1");
    EquityDesk f(paper);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0), 4.00, 4.20);
    f.buy(1);
    f.quotes(md::new_york_to_utc({2026, 9, 22}, 10, 0) + 15 * md::kNanosPerSecond, preset ? 29.40 : 5.20, preset ? 29.60 : 5.40);
    ASSERT_EQ(f.snapshot()->evaluation.status, trading::EvaluationStatus::Passed);
    server::TradingCommand reset;
    reset.kind = server::TradingCommand::Kind::ResetAccount;
    reset.initial_cash = funded->initial_cash;
    reset.rules = funded->rules;
    reset.reason = "funded";
    reset.required_pass = evaluation->name;
    const auto reply = command(*f.desk, reset, f.market.time, f.market.time);
    EXPECT_EQ(reply.decision.ok(), preset) << reply.decision.message;
    if (!preset) { EXPECT_EQ(reply.decision.code, trading::Reason::PLAN_LOCKED); }
  }
  // The fill model's execution settings are not part of the plan.
  auto conservative = evaluation->rules;
  conservative.fill_latency_ms = 1000; conservative.slippage_ticks = 1; conservative.impact_ticks = 1;
  EXPECT_TRUE(server::follows_plan(*evaluation, evaluation->initial_cash, conservative));
  conservative.margin = trading::MarginMode::Portfolio;
  conservative.house_margin_percent = 25;
  conservative.pm_vol_shock = 5;
  EXPECT_TRUE(server::follows_plan(*evaluation, evaluation->initial_cash, conservative));
  conservative.margin = trading::MarginMode::Strategy;
  conservative.account_type = trading::AccountType::Ira;
  EXPECT_TRUE(server::follows_plan(*evaluation, evaluation->initial_cash, conservative));
  conservative.buying_power = false;
  EXPECT_FALSE(server::follows_plan(*evaluation, evaluation->initial_cash, conservative));
  EXPECT_FALSE(server::follows_plan(*evaluation, Money::parse("1000000"), evaluation->rules));
  auto looser = evaluation->rules;
  looser.max_drawdown = Money::parse("5000");
  EXPECT_FALSE(server::follows_plan(*evaluation, evaluation->initial_cash, looser));
}

void write_stream(const std::filesystem::path& file, bool snapshots, md::Timestamp delay = 0) {
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time + delay;
  header.capabilities.delay = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::nanoseconds(delay));
  header.capabilities.poll_interval = snapshots ? 1s : 0s;
  md::Timestamp receipt = market.time + delay;
  md::RecordingSink::Options options;
  options.clock = [&] { return receipt; };
  test::DiscardEvents discard;
  md::RecordingSink sink(file, header, discard, options);
  sink.publish(md::ContractDefinition{0, market.contract});
  for (int second = 0; second <= 12; ++second) {
    const auto time = market.time + second * md::kNanosPerSecond;
    receipt = time + delay;
    sink.publish(md::UnderlyingQuote{"SPX", time, 5000, 5000, 5000});
    sink.publish(md::OptionQuote{0, time, 100 + .1 * second, 100.2 + .1 * second, 20, 20});
    if (snapshots) sink.publish(md::SnapshotComplete{"SPX", time});
  }
  sink.close();
}
TEST(ReplayBatches, SnapshotAndStreamingBoundariesPreserveEveryQuote) {
  for (const bool snapshots : {false, true}) {
    test::RecordingFile file;
    write_stream(file.path, snapshots);
    md::RecordingReader reader(file.path);
    providers::ReplayBatches batches(reader, reader.header().subscription);
    std::size_t quotes = 0, count = 0;
    md::Timestamp previous = 0;
    while (const auto batch = batches.next()) {
      EXPECT_GT(batch->time, previous);
      previous = batch->time;
      for (const auto& event : batch->events) if (std::holds_alternative<md::OptionQuote>(event)) ++quotes;
      if (snapshots) { EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(batch->events.back())); }
      ++count;
    }
    EXPECT_EQ(quotes, 13U);
    EXPECT_EQ(count, 13U);
  }
}

TEST(ReplayBatches, SubsecondStreamQuotesSettleOnTheFixedMarketSecond) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  auto header = test::recording_header();
  header.started = market.time;
  header.capabilities.poll_interval = 0s;
  header.capabilities.delay = 0s;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return market.time; };
  test::DiscardEvents discard;
  {
    md::RecordingSink sink(file.path, header, discard, recording);
    sink.publish(md::ContractDefinition{0, market.contract});
    sink.publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
    market.time += md::kNanosPerSecond / 2;
    sink.publish(md::OptionQuote{0, market.time, 100, 100.2, 20, 20});
    sink.close();
  }
  server::Desk::Options options;
  options.replay = true;
  server::Desk desk("test", header.capabilities, header.subscription, options);
  desk.start_trading();
  md::RecordingReader reader(file.path);
  providers::ReplayBatches batches(reader, header.subscription);
  while (const auto batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(desk.metrics("SPX"));
  EXPECT_EQ(desk.metrics("SPX")->as_of, market.time + md::kNanosPerSecond / 2);
  EXPECT_EQ(desk.market_time(), desk.metrics("SPX")->as_of);
  providers::ReplayProvider replay({file.path, 0, false, {}, 0, true});
  replay.set_driver([](providers::ReplayBatch) {
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  });
  replay.start({header.subscription.underlyings, 0, 0}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  EXPECT_THROW(replay.until(market.time), std::invalid_argument);
  EXPECT_NO_THROW(replay.until(market.time + md::kNanosPerSecond / 2));
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond / 2);
  replay.stop();
}

/// A snapshot feed that polls QQQ, SPX and SPY in turn at each 15-second market time,
/// in the order the demo market writes them: every underlying's price, then each
/// underlying's one option quote and SnapshotComplete. SPX's option is 0.20 wide, the
/// ETFs' 0.02.
struct PolledInstants {
  test::RecordingFile file;
  std::array<md::OptionContract, 3> contracts{*md::parse_osi("QQQ261022C00450000"),
      *md::parse_osi("SPXW261022C05000000"), *md::parse_osi("SPY261022P00500000")};
  md::Subscription subscription{{"QQQ", "SPX", "SPY"}, 0, 0};
  md::Timestamp open = md::new_york_to_utc({2026, 9, 22}, 10, 0);
  md::Timestamp at(std::size_t instant) const {
    return open + static_cast<md::Timestamp>(instant) * 15 * md::kNanosPerSecond;
  }
  std::string symbol(std::size_t underlying) const { return contracts[underlying].osi_symbol(); }
  /// Each row is one market time's QQQ, SPX and SPY asks.
  void write(const std::vector<std::array<double, 3>>& asks) const {
    auto header = test::recording_header();
    header.started = open;
    header.subscription = subscription;
    header.capabilities.delay = 0s;
    md::Timestamp receipt = open;
    md::RecordingSink::Options options;
    options.clock = [&] { return receipt; };
    test::DiscardEvents discard;
    md::RecordingSink sink(file.path, header, discard, options);
    for (std::size_t u = 0; u < 3; ++u) sink.publish(md::ContractDefinition{static_cast<md::InstrumentId>(u), contracts[u]});
    constexpr std::array<double, 3> spot{450, 5000, 500};
    for (std::size_t i = 0; i < asks.size(); ++i) {
      receipt = at(i);
      for (std::size_t u = 0; u < 3; ++u) sink.publish(md::UnderlyingQuote{subscription.underlyings[u], at(i), spot[u], spot[u], spot[u]});
      for (std::size_t u = 0; u < 3; ++u) {
        receipt = at(i) + static_cast<md::Timestamp>(u) * md::kNanosPerSecond / 1000;
        const double spread = u == 1 ? .2 : .02;
        sink.publish(md::OptionQuote{static_cast<md::InstrumentId>(u), at(i), asks[i][u] - spread, asks[i][u], 20, 20});
        sink.publish(md::SnapshotComplete{subscription.underlyings[u], at(i)});
      }
    }
    sink.close();
  }
  server::Desk desk(server::Desk::Options options) const {
    options.analytics.fallback_rate = 0;  // one call per chain has no put to imply a rate from
    md::RecordingReader reader(file.path);
    return server::Desk("replay (synthetic)", reader.header().capabilities, subscription, std::move(options));
  }
};
server::TradingReply order(server::Desk& desk, const std::string& symbol, trading::Side side, trading::Quantity quantity,
                           md::Timestamp time) {
  server::TradingCommand request;
  request.order = {symbol + (side == trading::Side::Buy ? ":buy" : ":sell"), symbol, side, trading::OrderType::Market,
                   trading::TimeInForce::Ioc, quantity, {}, {}, {}, {}};
  return command(desk, request, time, time);
}

TEST(ReplayBatches, EveryUnderlyingsSnapshotOfOneMarketTimeIsOneBatch) {
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}, {4.22, 12.20, 2.22}});
  for (const bool instants : {true, false}) {
    md::RecordingReader reader(feed.file.path);
    providers::ReplayBatches batches(reader, feed.subscription, instants);
    std::vector<providers::ReplayBatch> all;
    while (auto batch = batches.next()) all.push_back(std::move(*batch));
    // Runs recorded before driver 2 batched each underlying's snapshot alone.
    ASSERT_EQ(all.size(), instants ? 3U : 9U);
    for (std::size_t i = 0; i < all.size(); ++i) {
      const auto instant = instants ? i : i / 3;
      EXPECT_EQ(all[i].time, feed.at(instant));
      const auto completes = std::count_if(all[i].events.begin(), all[i].events.end(),
          [](const md::Event& event) { return std::holds_alternative<md::SnapshotComplete>(event); });
      EXPECT_EQ(completes, instants ? 3 : 1);
      EXPECT_TRUE(std::holds_alternative<md::SnapshotComplete>(all[i].events.back()));
    }
    // The batch settles at the last snapshot's receipt, as it did before.
    EXPECT_EQ(all.back().received, feed.at(2) + 2 * md::kNanosPerSecond / 1000);
  }
  // A snapshot that changed nothing moves no clock: its SnapshotComplete's time places it,
  // with the others of its time and never with an earlier time's.
  auto header = test::recording_header();
  header.subscription = feed.subscription;
  const auto unchanged = feed.file.directory / "unchanged.oprec";
  test::record_events(unchanged, {md::ContractDefinition{0, feed.contracts[1]}, md::ContractDefinition{1, feed.contracts[2]},
      md::UnderlyingQuote{"SPX", feed.at(0), 5000, 5000, 5000}, md::OptionQuote{0, feed.at(0), 10, 10.2, 20, 20},
      md::SnapshotComplete{"SPX", feed.at(0)}, md::SnapshotComplete{"SPY", feed.at(0)},
      md::SnapshotComplete{"SPX", feed.at(1)}, md::OptionQuote{1, feed.at(1), 2, 2.02, 20, 20},
      md::SnapshotComplete{"SPY", feed.at(1)}, md::SnapshotComplete{"SPX", feed.at(2)}}, header);
  md::RecordingReader reader(unchanged);
  providers::ReplayBatches batches(reader, feed.subscription);
  std::vector<std::pair<std::size_t, md::Timestamp>> shapes;
  while (const auto batch = batches.next()) shapes.emplace_back(batch->events.size(), batch->time);
  EXPECT_EQ(shapes, (std::vector<std::pair<std::size_t, md::Timestamp>>{{6, feed.at(0)}, {3, feed.at(1)}, {1, feed.at(1)}}));
}

TEST(ReplayBatches, ATruncatedRecordingDeliversItsLastCompleteInstantBeforeFailing) {
  // A recorder killed before its clean end leaves every complete record readable. Looking
  // ahead for the rest of an instant meets the truncation, but the last whole instant is
  // still delivered, and only the next call fails, as without instants.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  std::filesystem::resize_file(feed.file.path, std::filesystem::file_size(feed.file.path) - 1);
  for (const bool instants : {true, false}) {
    md::RecordingReader reader(feed.file.path);
    providers::ReplayBatches batches(reader, feed.subscription, instants);
    std::vector<md::Timestamp> times;
    const auto drain = [&] { while (const auto batch = batches.next()) times.push_back(batch->time); };
    EXPECT_THROW(drain(), std::runtime_error);
    ASSERT_EQ(times.size(), instants ? 2U : 6U);
    EXPECT_EQ(times.back(), feed.at(1));
    EXPECT_THROW((void)batches.next(), std::runtime_error);
  }
}

TEST(MarketInstants, LatencyFillsOnTheNextInstantsQuoteNotOneAnotherUnderlyingReoffered) {
  // B01: QQQ's snapshot of 10:00:15 comes first. Applied alone, it re-offered SPX's
  // 10:00 quote at 10:00:15, which released the delayed order at the price it was sent at.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  server::Desk::Options options;
  options.replay = true;
  options.paper.rules.fill_latency_ms = 1000;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  const auto reply = order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0));
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  EXPECT_TRUE(reply.view->snapshot->recent_fills.empty());
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto& fills = desk.trading_view()->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills.front().price, Money::parse("11.20"));
  EXPECT_EQ(fills.front().quote_time, feed.at(1));
}

TEST(MarketInstants, AnotherUnderlyingsLivePollKeepsAQuoteCurrentButDoesNotMakeItNew) {
  // A live polling feed delivers each underlying's snapshot on its own, as ReplayBatches
  // without instants cuts the recording. QQQ's newer poll moves the market clock, but
  // SPX's quote was supplied before the order, so the order's latency waits for SPX's.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  server::Desk::Options options;
  options.paper.rules.fill_latency_ms = 1000;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches polls(reader, feed.subscription, false);
  const auto poll = [&] {
    const auto batch = polls.next();
    ASSERT_TRUE(batch);
    desk.replay_batch(batch->events, batch->received, batch->time);
  };
  for (int underlying = 0; underlying < 3; ++underlying) poll();
  const auto reply = order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0));
  ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
  poll();  // QQQ at 10:00:15
  EXPECT_EQ(desk.market_time(), feed.at(1));
  EXPECT_TRUE(desk.trading_view()->snapshot->recent_fills.empty());
  EXPECT_TRUE(desk.trading_view()->snapshot->valuation_complete);
  poll();  // SPX at 10:00:15
  const auto& fills = desk.trading_view()->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills.front().price, Money::parse("11.20"));
  EXPECT_EQ(fills.front().quote_time, feed.at(1));
}

TEST(MarketInstants, TheExpiryCutoffClosesOnTheCutoffInstantsBook) {
  // B02: the 15:55:00 auto-close ran on QQQ's 15:55:00 snapshot and sold into SPX's
  // 15:54:45 bid.
  PolledInstants feed;
  feed.contracts[1] = *md::parse_osi("SPXW260922C05000000");
  feed.open = md::new_york_to_utc({2026, 9, 22}, 15, 54) + 45 * md::kNanosPerSecond;
  feed.write({{4.02, 3.20, 2.02}, {4.12, 2.60, 2.12}});
  server::Desk::Options options;
  options.replay = true;
  options.paper.rules.expiry_cutoff = 300 * md::kNanosPerSecond;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 2, feed.at(0)).decision.ok());
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto view = desk.trading_view();
  EXPECT_TRUE(view->snapshot->positions.empty());
  const auto& fills = view->snapshot->recent_fills;
  ASSERT_EQ(fills.size(), 2U);
  EXPECT_EQ(fills.back().side, trading::Side::Sell);
  EXPECT_EQ(fills.back().price, Money::parse("2.40"));
  EXPECT_EQ(fills.back().time, feed.at(1));
  EXPECT_EQ(fills.back().quote_time, feed.at(1));
}

TEST(MarketInstants, TrailingDrawdownIsDecidedOnWholeInstants) {
  // B03: long an SPX call and SPY puts. At 10:00:15 SPX gains 100.00 and the puts lose
  // 120.00: equity falls. SPX's snapshot applied before SPY's showed a gain that never
  // existed, and the intraday peak and floor ratcheted on it.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 1.90}});
  server::Desk::Options options;
  options.replay = true;
  options.paper.initial_cash = Money::parse("25000");
  options.paper.rules.max_drawdown = Money::parse("1250");
  options.paper.rules.drawdown_mode = trading::DrawdownMode::Intraday;
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0)).decision.ok());
  ASSERT_TRUE(order(desk, feed.symbol(2), trading::Side::Buy, 10, feed.at(0)).decision.ok());
  const auto before = desk.trading_view()->snapshot->equity;
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto snapshot = desk.trading_view()->snapshot;
  EXPECT_EQ(snapshot->equity, before - Money::parse("20"));
  EXPECT_EQ(snapshot->evaluation.peak, Money::parse("25000"));
  EXPECT_EQ(snapshot->evaluation.floor, Money::parse("23750"));
}

TEST(MarketInstants, AMinutesFirstEquitySampleHoldsThatInstantsEquity) {
  // B21: QQQ's 10:01:00 snapshot sampled the account before SPX's arrived, and the
  // minute's first sample is the one the equity history keeps.
  PolledInstants feed;
  feed.open = md::new_york_to_utc({2026, 9, 22}, 10, 0) + 45 * md::kNanosPerSecond;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 12.20, 2.12}});
  std::vector<server::EquitySample> samples;
  server::Desk::Options options;
  options.replay = true;
  options.equity_sample = [&](std::string_view, const server::EquitySample& sample) { samples.push_back(sample); };
  auto desk = feed.desk(options);
  desk.start_trading();
  md::RecordingReader reader(feed.file.path);
  providers::ReplayBatches batches(reader, feed.subscription);
  auto batch = batches.next();
  for (; batch && batch->time == feed.at(0); batch = batches.next())
    desk.replay_batch(batch->events, batch->received, batch->time);
  ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0)).decision.ok());
  for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
  const auto equity = desk.trading_view()->snapshot->equity;
  std::size_t minute = 0;
  for (const auto& sample : samples) {
    if (sample.time != feed.at(1) || sample.fill || sample.stock_fill) continue;
    EXPECT_EQ(sample.equity, equity);
    ++minute;
  }
  EXPECT_GT(minute, 0U);
}

TEST(ReproducibleRun, RunsRecordedBeforeInstantBatchesStillVerify) {
  // Driver 2 changes fills, so a run records it; one without it replays with the
  // per-snapshot batches and re-offered quotes it was recorded with.
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  for (const bool instants : {false, true}) {
    const auto journal = feed.file.directory / (instants ? "driver-2.jsonl" : "driver-1.jsonl");
    {
      server::Desk::Options options;
      options.replay = true;
      options.instant_batches = instants;
      options.opening_settlement = false;
      options.run_input = server::recording_input(feed.file.path);
      options.paper_journal = journal;
      options.paper.rules.fill_latency_ms = 1000;
      auto desk = feed.desk(options);
      desk.start_trading();
      md::RecordingReader reader(feed.file.path);
      providers::ReplayBatches batches(reader, feed.subscription, instants);
      auto batch = batches.next();
      for (; batch && batch->time == feed.at(0); batch = batches.next())
        desk.replay_batch(batch->events, batch->received, batch->time);
      ASSERT_TRUE(order(desk, feed.symbol(1), trading::Side::Buy, 1, feed.at(0)).decision.ok());
      for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
      const auto& fills = desk.trading_view()->snapshot->recent_fills;
      ASSERT_EQ(fills.size(), 1U);
      EXPECT_EQ(fills.front().price, Money::parse(instants ? "11.20" : "10.20"));
      desk.stop();
    }
    // Driver 3 batches instants too.
    EXPECT_EQ(read_file(journal).find("\"driver\":4") != std::string::npos, instants);
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

/// SPY's close on 2026-09-22 and its open on 2026-09-23, 4% lower: a call and a deep
/// put a month out. At the close the put keeps 0.50 of time value; at the open it
/// trades 0.20 under its exercise value.
struct Overnight {
  test::RecordingFile file;
  md::OptionContract call = *md::parse_osi("SPY261016C00500000");
  md::OptionContract put = *md::parse_osi("SPY261016P00540000");
  md::Subscription subscription{{"SPY"}, 0, 0};
  struct Instant {
    md::Timestamp time;
    double spot, call_ask, put_ask;
  };
  std::vector<Instant> instants{{md::new_york_to_utc({2026, 9, 22}, 15, 59, 45), 500, 10.20, 40.60},
                                {md::new_york_to_utc({2026, 9, 22}, 16, 14, 45), 500, 10.20, 40.60},
                                {md::new_york_to_utc({2026, 9, 23}, 9, 30), 480, 3.20, 59.90},
                                {md::new_york_to_utc({2026, 9, 23}, 9, 30, 15), 480, 3.20, 59.90}};
  Overnight() {
    auto header = test::recording_header();
    header.started = instants.front().time;
    header.subscription = subscription;
    header.capabilities.delay = 0s;
    md::Timestamp receipt = header.started;
    md::RecordingSink::Options options;
    options.clock = [&] { return receipt; };
    test::DiscardEvents discard;
    md::RecordingSink sink(file.path, header, discard, options);
    sink.publish(md::ContractDefinition{0, call});
    sink.publish(md::ContractDefinition{1, put});
    for (const auto& instant : instants) {
      receipt = instant.time;
      sink.publish(md::UnderlyingQuote{"SPY", instant.time, instant.spot, instant.spot + .01, instant.spot});
      sink.publish(md::OptionQuote{0, instant.time, instant.call_ask - .2, instant.call_ask, 20, 20});
      sink.publish(md::OptionQuote{1, instant.time, instant.put_ask - .2, instant.put_ask, 20, 20});
      sink.publish(md::SnapshotComplete{"SPY", instant.time});
    }
    sink.close();
  }
  /// Long a call and short ten puts from the first instant on, to the end.
  std::shared_ptr<const trading::TradingSnapshot> run(server::Desk::Options options) const {
    options.replay = true;
    options.analytics.fallback_rate = 0;
    options.paper.limits.aggregate = {1e12, 1e12};
    options.paper.limits.per_underlying = {1e12, 1e12};
    options.paper.limits.max_daily_loss = Money::parse("1000000");
    md::RecordingReader reader(file.path);
    server::Desk desk("replay (synthetic)", reader.header().capabilities, subscription, std::move(options));
    desk.start_trading();
    providers::ReplayBatches batches(reader, subscription);
    auto batch = batches.next();
    desk.replay_batch(batch->events, batch->received, batch->time);
    EXPECT_TRUE(order(desk, call.osi_symbol(), trading::Side::Buy, 1, instants.front().time).decision.ok());
    EXPECT_TRUE(order(desk, put.osi_symbol(), trading::Side::Sell, 10, instants.front().time).decision.ok());
    while ((batch = batches.next())) desk.replay_batch(batch->events, batch->received, batch->time);
    auto snapshot = desk.trading_view()->snapshot;
    desk.stop();
    return snapshot;
  }
};

TEST(Rollover, TheFinishedDayClosesOnItsClosingMarksBeforeTheNextDaysQuotes) {
  // B28 and D11: the first batch of 2026-09-23 was applied before the rollover, so the
  // finished day's P&L by Greek took the overnight gap and the puts were assigned on
  // the opening marks. Runs recorded before driver 3 still roll over that way.
  Overnight feed;
  for (const bool closing : {true, false}) {
    SCOPED_TRACE(closing ? "driver 3" : "driver 2");
    const auto journal = feed.file.directory / (closing ? "driver-3.jsonl" : "driver-2.jsonl");
    server::Desk::Options options;
    options.closing_rollover = closing;
    options.inputs_first = false;  // driver 3 itself
    options.run_input = server::recording_input(feed.file.path);
    options.paper_journal = journal;
    const auto snapshot = feed.run(options);
    const auto& e = snapshot->evaluation;
    ASSERT_EQ(e.days.size(), 1U);
    const auto& finished = e.days.front();
    EXPECT_EQ(finished.day, (md::Date{2026, 9, 22}));
    const double finished_pnl = (finished.close_equity - finished.open_equity).dollars();
    const double today_pnl = (snapshot->equity - e.day_open_equity).dollars();
    if (closing) {
      // The finished day's parts add up to its close less its open; the gap is the new day's.
      EXPECT_NEAR(finished.attribution.total(), finished_pnl, 1e-6);
      EXPECT_EQ(e.day_open_equity, finished.close_equity);
      EXPECT_NEAR(snapshot->attribution.total(), today_pnl, 1e-6);
      EXPECT_LT(today_pnl, -10000);
      // At the close the puts were worth more than their exercise: nobody exercises them.
      EXPECT_TRUE(snapshot->closures.empty());
      EXPECT_TRUE(snapshot->stock_fills.empty());
    } else {
      EXPECT_LT(finished.attribution.total(), finished_pnl - 10000);
      EXPECT_LT(e.day_open_equity, finished.close_equity - Money::parse("10000"));
      EXPECT_FALSE(snapshot->closures.empty());
    }
    const auto text = read_file(journal);
    EXPECT_NE(text.find(closing ? "\"driver\":3" : "\"driver\":2"), std::string::npos);
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

class ImmediateClock final : public providers::ReplayClock {
 public:
  TimePoint now() override { return now_; }
  bool wait_until(TimePoint deadline, const std::atomic<bool>& stop) override { now_ = deadline; return !stop; }
  void interrupt() override {}
 private:
  TimePoint now_{};
};

void scripted_orders(server::Desk& desk, md::Timestamp open, md::Timestamp receipt) {
  const auto time = desk.market_time();
  server::TradingCommand request;
  if (time == open + md::kNanosPerMinute) {
    request.order.client_order_id = "vertical";
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    request.order.legs = {{md::parse_osi("SPXW261127C06000000")->osi_symbol(), trading::Side::Buy, 1}, {md::parse_osi("SPXW261127C06010000")->osi_symbol(), trading::Side::Sell, 1}};
  } else if (time == open + 2 * md::kNanosPerMinute) {
    request.order.client_order_id = "bracket";
    request.order.symbol = md::parse_osi("SPXW261127P06000000")->osi_symbol();
    request.order.quantity = 1;
    request.order.type = trading::OrderType::Market;
    request.order.tif = trading::TimeInForce::Ioc;
    request.order.bracket = trading::Bracket{
        trading::ExitSpec{trading::Trigger{trading::TriggerSource::Underlying, trading::TriggerDirection::AtOrBelow, Money::parse("5800")}, {}},
        trading::ExitSpec{{}, Money::parse("200")}};
  } else if (time == open + 10 * md::kNanosPerMinute) request.kind = server::TradingCommand::Kind::ClosePositions;
  else return;
  const auto reply = command(desk, request, time, receipt);
  EXPECT_TRUE(reply.error_code.empty()) << reply.error_code;
  EXPECT_TRUE(reply.decision.ok()) << reply.decision.message;
}

TEST(ReproducibleRun, ScenarioDayHasIdenticalBytesAcrossSpeedsFastForwardAndVerification) {
  test::RecordingFile file;
  const auto source = file.directory / "golden.json";
  {
    std::ofstream out(source);
    out << R"({"id":"golden","title":"Golden day","description":"Simulated test day","symbols":["SPX"],"session":"regular","date":"2026-11-27","seed":81723,"generator":1,"drift":[[1,0.001]],"volatility":0.12,"iv_shift":0,"spot_vol":-2})";
  }
  const auto scenario = providers::read_scenario(source);
  providers::write_scenario_recording(file.path, scenario, scenario.date, scenario.seed);
  const auto open = providers::scenario_open(scenario, scenario.date);
  std::string golden;
  std::vector<trading::Fill> fills;
  Money equity;
  for (const int speed : {-1, 1, 300, 0}) {
    const auto journal = file.directory / ("run-" + std::to_string(speed) + ".jsonl");
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.replay = true;
    options.run_input = server::scenario_input(scenario, scenario.date, scenario.seed);
    options.paper_journal = journal;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    if (speed == -1) {
      providers::ReplayBatches batches(reader, reader.header().subscription);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        scripted_orders(desk, open, batch->received);
      }
    } else {
      providers::ReplayProvider replay({file.path, speed, false, std::make_shared<ImmediateClock>()});
      replay.set_driver([&](providers::ReplayBatch batch) {
        std::promise<void> done;
        desk.replay_batch(batch.events, batch.received, batch.time);
        scripted_orders(desk, open, batch.received);
        done.set_value();
        return done.get_future();
      });
      test::DiscardEvents discard;
      replay.start(reader.header().subscription, discard);
      // Full-day replay competes with other scenario tests under parallel CTest.
      const auto deadline = std::chrono::steady_clock::now() + 5min;
      while (!replay.finished() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
      ASSERT_TRUE(replay.finished());
      replay.stop();
    }
    ASSERT_TRUE(desk.trading_status().enabled) << desk.trading_status().reason;
    const auto view = desk.trading_view();
    ASSERT_TRUE(view);
    EXPECT_TRUE(view->snapshot->positions.empty());
    EXPECT_TRUE(view->snapshot->open_orders.empty());
    ASSERT_GE(view->snapshot->recent_fills.size(), 6U);
    if (golden.empty()) {
      golden = read_file(journal);
      fills = view->snapshot->recent_fills.to_vector();
      equity = view->snapshot->equity;
      json summary = json::array();
      for (const auto& fill : fills) summary.push_back({fill.symbol, fill.quantity, fill.price.micros(), fill.fee.micros(), fill.time});
      // Pin money, quantities and fill times independently of analytic floats.
      EXPECT_EQ(summary, json::parse(R"([
        ["SPXW  261127C06000000",1,11700000,650000,1795789860000000000],
        ["SPXW  261127C06010000",1,5200000,650000,1795789860000000000],
        ["SPXW  261127P06000000",1,4100000,650000,1795789920000000000],
        ["SPXW  261127C06010000",1,4800000,650000,1795790400000000000],
        ["SPXW  261127C06000000",1,10500000,650000,1795790400000000000],
        ["SPXW  261127P06000000",1,4200000,650000,1795790400000000000]
      ])"));
      EXPECT_EQ(equity.micros(), 99926100000LL);
    } else {
      EXPECT_TRUE(read_file(journal) == golden);
      EXPECT_EQ(json(view->snapshot->recent_fills), json(fills));
      EXPECT_EQ(view->snapshot->equity, equity);
    }
    desk.stop();
  }
  const auto journal = file.directory / "run-0.jsonl";
  const auto calendar = md::scheduled_days();
  md::set_scheduled_days({{scenario.date, "Different host calendar", true, 13, 0}});
  const auto verified = server::verify_run(journal);
  EXPECT_EQ(md::scheduled_days().front().name, "Different host calendar");
  md::set_scheduled_days(calendar);
  EXPECT_TRUE(verified.matched) << verified.message;
  EXPECT_EQ(verified.equity, equity);
  EXPECT_TRUE(verified.message.starts_with("Run run-0 (run-0.jsonl)"));
  EXPECT_NE(verified.message.find("scenario " + scenario.id), std::string::npos);
  EXPECT_NE(verified.message.find("seed 81723"), std::string::npos);
  EXPECT_NE(verified.message.find("revision " + std::to_string(providers::kScenarioRevision)), std::string::npos);
  EXPECT_NE(verified.message.find("plan "), std::string::npos);
  EXPECT_EQ(verified.run.at("inputs").at(0).count("file"), 0U);
  const auto altered = file.directory / "altered.jsonl";
  std::uint64_t changed_at = 0;
  {
    auto output = trading::FileJournal::create(altered.string());
    for (const auto& record : trading::FileJournal::read(journal.string()).records) {
      auto payload = json::parse(record.payload);
      if (record.type == "market" && changed_at == 0) { payload["unexpected"] = true; changed_at = record.seq; }
      output->append(record.time, record.type, payload.dump());
    }
  }
  ASSERT_NE(changed_at, 0U);
  const auto mismatch = server::verify_run(altered);
  EXPECT_FALSE(mismatch.matched);
  EXPECT_TRUE(mismatch.message.starts_with("Run altered (altered.jsonl)"));
  EXPECT_NE(mismatch.message.find("scenario " + scenario.id), std::string::npos);
  EXPECT_NE(mismatch.message.find("First differing transaction " + std::to_string(changed_at)), std::string::npos) << mismatch.message;
  // The CLI takes the same path without starting a feed, HTTP or user directories.
  const auto cli = std::string(OPENPORT_APPS_DIR) + "/openportd --verify-run " + journal.string() + " > " + (file.directory / "verify.txt").string() + " 2>&1";
  EXPECT_EQ(std::system(cli.c_str()), 0);
  EXPECT_NE(read_file(file.directory / "verify.txt").find("Run run-0 (run-0.jsonl)"), std::string::npos);
  EXPECT_TRUE(read_file(file.directory / "verify.txt").starts_with("Verification estimate:"));
  const auto recovered = trading::FileJournal::read(journal.string());
  const auto sidecar = std::filesystem::path(journal).replace_extension(".json");
  { std::ofstream out(sidecar); out << json{{"plan", "practice"}, {"journal", {{"head", recovered.head},
      {"transactions", recovered.records.size()}, {"bytes", std::filesystem::file_size(journal)}}}}; }
  const auto checkpoint = server::verify_run(journal);
  EXPECT_TRUE(checkpoint.matched) << checkpoint.message;
  EXPECT_NE(checkpoint.message.find("recorded final " + std::to_string(recovered.records.size())), std::string::npos);
  { std::ofstream out(journal); out << golden.substr(0, golden.rfind('\n', golden.size() - 2) + 1); }
  const auto truncated = server::verify_run(journal);
  EXPECT_FALSE(truncated.matched);
  EXPECT_TRUE(truncated.message.starts_with("Run run-0"));
  EXPECT_NE(truncated.message.find("Journal disagrees with recorded final"), std::string::npos);
  auto tampered = golden;
  const auto hash = tampered.find("\"hash\":\"");
  ASSERT_NE(hash, std::string::npos);
  tampered[hash + 8] = tampered[hash + 8] == 'a' ? 'b' : 'a';
  { std::ofstream out(journal); out << tampered; }
  EXPECT_FALSE(server::verify_run(journal).matched);
  EXPECT_NE(std::system(cli.c_str()), 0);
  { std::ofstream out(journal); out << golden; }
  { std::ofstream out(source, std::ios::app); out << '\n'; }
  const auto changed = server::verify_run(journal);
  EXPECT_FALSE(changed.matched);
  EXPECT_NE(changed.message.find("Scenario input changed"), std::string::npos) << changed.message;
  { std::ofstream out(source); out << "{"; }
  const auto invalid_source = server::verify_run(journal);
  EXPECT_FALSE(invalid_source.matched);
  EXPECT_EQ(invalid_source.message.find(file.directory.string()), std::string::npos);
}

TEST(ReproducibleRun, ScenarioRunsKeepTheirSavedRevisionAndLegacyDefault) {
  test::RecordingFile file;
  const auto source = file.directory / "golden.json";
  {
    std::ofstream out(source);
    out << R"({"id":"golden","title":"Golden day","description":"Simulated test day","symbols":["SPX"],"session":"regular","date":"2026-11-27","seed":81723,"generator":1,"drift":[[1,0.001]],"volatility":0.12,"iv_shift":0,"spot_vol":-2})";
  }
  const auto scenario = providers::read_scenario(source);
  for (int revision = 1; revision <= providers::kScenarioRevision; ++revision) {
    SCOPED_TRACE(revision);
    const auto recording = file.directory / ("revision-" + std::to_string(revision) + ".oprec");
    providers::write_scenario_recording(recording, scenario, scenario.date, scenario.seed, revision);
    // A run recorded before revisions names none.
    auto identity = json::parse(server::scenario_input(scenario, scenario.date, scenario.seed));
    EXPECT_EQ(identity.at("revision"), providers::kScenarioRevision);
    if (revision == 1) identity.erase("revision");
    else identity["revision"] = revision;
    const auto journal = file.directory / ("run-" + std::to_string(revision) + ".jsonl");
    {
      md::RecordingReader reader(recording);
      server::Desk::Options options;
      options.replay = true;
      options.run_input = identity.dump();
      options.paper_journal = journal;
      server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      const auto open = providers::scenario_open(scenario, scenario.date);
      while (const auto batch = batches.next()) {
        if (batch->time > open + 5 * md::kNanosPerMinute) break;
        desk.replay_batch(batch->events, batch->received, batch->time);
        scripted_orders(desk, open, batch->received);
      }
      desk.stop();
    }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

TEST(ScenarioReplay, CircuitBreakersMeasureTheFallFromTheScenariosPreviousClose) {
  // D12: a scenario had no previous close, so a crash never halted anything.
  test::RecordingFile file;
  const auto source = file.directory / "crash.json";
  {
    std::ofstream out(source);
    out << R"({"id":"crash","title":"Crash","description":"A gap down and a slide.","symbols":["SPX"],"session":"regular","date":"2026-09-16","seed":1,"generator":1,"drift":[[1,-0.2]],"volatility":0,"iv_shift":0,"spot_vol":0,"events":[{"type":"gap","move":-0.1}]})";
  }
  const auto scenario = providers::read_scenario(source);
  for (const int revision : {1, 2}) {
    SCOPED_TRACE(revision);
    const auto recording = file.directory / ("crash-" + std::to_string(revision) + ".oprec");
    providers::write_scenario_recording(recording, scenario, scenario.date, scenario.seed, revision);
    md::RecordingReader reader(recording);
    server::Desk::Options options;
    options.replay = true;
    server::Desk desk("replay (demo)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    const auto open = providers::scenario_open(scenario, scenario.date);
    while (const auto batch = batches.next()) {
      if (batch->time > open + 10 * md::kNanosPerMinute) break;
      desk.replay_batch(batch->events, batch->received, batch->time);
    }
    const auto& breaker = desk.breaker();
    if (revision == 1) {
      EXPECT_FALSE(breaker.previous_close);
      EXPECT_TRUE(breaker.halts.empty());
      continue;
    }
    // Opening 9.5% under the 6000 close trips level 1 at the open.
    ASSERT_TRUE(breaker.previous_close);
    EXPECT_EQ(breaker.previous_close->date, (md::Date{2026, 9, 15}));
    EXPECT_DOUBLE_EQ(breaker.previous_close->price, 6000);
    ASSERT_FALSE(breaker.halts.empty());
    EXPECT_EQ(breaker.halts.front().level, 1);
    EXPECT_EQ(breaker.halts.front().start, open);
    EXPECT_TRUE(breaker.active);
  }
}

server::ApiResponse replay_call(server::ReplayHost& host, std::string method, std::string target, json body = json::object()) {
  std::promise<server::ApiResponse> done;
  auto result = done.get_future();
  server::ApiRequest request{std::move(method), std::move(target), body.dump()};
  request.content_type = "application/json";
  request.actor = "test-actor";
  if (!host.handle(request, [&](server::ApiResponse response) { done.set_value(std::move(response)); }))
    throw std::runtime_error("Replay request was not handled");
  if (result.wait_for(5min) != std::future_status::ready) throw std::runtime_error("Replay request timed out");
  return result.get();
}
TEST(ReplayRun, KeptJournalFlushesAtPauseStepFinishStopAndTeardown) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  for (const std::string ending : {"finish", "stop", "teardown"}) {
    const auto directory = file.directory / ending;
    std::filesystem::create_directory(directory);
    std::atomic<std::uintmax_t> synced_bytes{0};
    std::atomic<unsigned> syncs{0};
    server::Engine::Options options;
    options.paper_journal = directory / "main.jsonl";
    options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
    options.journal_io.sync = [&](int fd) {
      synced_bytes = static_cast<std::uintmax_t>(::lseek(fd, 0, SEEK_END));
      ++syncs;
      return true;
    };
    std::filesystem::path journal;
    {
      server::ReplayHost host({file.directory, options, false});
      const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}, {"paused", true}});
      ASSERT_EQ(started.status, 201) << started.body;
      const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
      journal = directory / "replays" / (id + ".jsonl");
      ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
      EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
      const auto initial_syncs = syncs.load();
      const auto stepped = replay_call(host, "PUT", "/api/replay", {{"until", md::format_timestamp(market.time + 4 * md::kNanosPerSecond)}});
      ASSERT_EQ(stepped.status, 200) << stepped.body;
      EXPECT_GT(syncs.load(), initial_syncs);
      EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
      // Pausing explicitly also waits for the owner-thread durability barrier.
      ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", true}}).status, 200);
      EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
      if (ending == "finish") {
        ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", false}}).status, 200);
        ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick()).at("replay").at("finished").get<bool>(); }));
        EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
        const auto listing = json::parse(replay_call(host, "GET", "/api/replay").body);
        EXPECT_EQ(listing.at("history").size(), 1U);
        EXPECT_EQ(replay_call(host, "GET", "/api/replay/history/" + id).status, 200);
      } else {
        ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"speed", 1}, {"paused", false}}).status, 200);
        if (ending == "stop") {
          EXPECT_EQ(replay_call(host, "DELETE", "/api/replay").status, 200);
          EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
        }
      }
    }
    EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(journal));
    const auto recovered = trading::FileJournal::read(journal.string());
    EXPECT_LT(syncs.load(), recovered.records.size());
    EXPECT_TRUE(server::verify_run(journal).matched);
    const auto metadata = json::parse(read_file(std::filesystem::path(journal).replace_extension(".json")));
    EXPECT_EQ(metadata.at("journal").at("transactions"), recovered.records.size());
    EXPECT_EQ(metadata.at("journal").at("head"), recovered.head);
    EXPECT_EQ(metadata.at("journal").at("bytes"), std::filesystem::file_size(journal));
  }
}

TEST(ReplayRun, HistoryFlagsTornAndCleanlyTruncatedJournalsAndCachesSummaries) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({file.directory, options, false});
  const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}});
  ASSERT_EQ(started.status, 201) << started.body;
  const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
  const auto path = file.directory / "replays" / (id + ".jsonl");
  ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick()).at("replay").at("finished").get<bool>(); }));
  // EOF persists without needing a stop or history listing.
  ASSERT_TRUE(test::recording_eventually([&] {
    return json::parse(read_file(std::filesystem::path(path).replace_extension(".json"))).contains("journal");
  }));
  ASSERT_EQ(replay_call(host, "DELETE", "/api/replay").status, 200);
  const auto original = read_file(path);
  const auto records = trading::FileJournal::read(path.string());
  const auto boundary = original.rfind('\n', original.size() - 2) + 1;
  for (const bool torn : {true, false}) {
    { std::ofstream out(path, std::ios::binary); out << original.substr(0, torn ? original.size() - 5 : boundary); }
    const auto list = [&] { return json::parse(replay_call(host, "GET", "/api/replay/history").body).at("history"); };
    const auto item = list().at(0);
    EXPECT_EQ(item.value("torn", false), torn);
    EXPECT_TRUE(item.at("truncated"));
    EXPECT_TRUE(item.at("mismatch"));
    EXPECT_EQ(item.at("journal").at("transactions"), records.records.size());
    EXPECT_EQ(item.at("journal_found").at("transactions"), records.records.size() - 1);
    if (torn) {
      EXPECT_EQ(item.at("bytes_cut"), original.size() - 5 - boundary);
      EXPECT_NE(item.at("integrity_message").get<std::string>().find("--repair-journals"), std::string::npos);
    }
    EXPECT_EQ(item.dump().find(file.directory.string()), std::string::npos);
    const auto reads = host.history_recoveries();
    EXPECT_EQ(list().at(0), item);
    EXPECT_EQ(host.history_recoveries(), reads);
    const auto account = replay_call(host, "GET", "/api/replay/history/" + id + "/account");
    ASSERT_EQ(account.status, 200) << account.body;
    EXPECT_TRUE(json::parse(account.body).at("mismatch"));
    EXPECT_EQ(json::parse(account.body).value("torn", false), torn);
  }
}

TEST(ReplayRun, BackgroundVerificationIsExclusivePersistsAndInvalidatesWithoutMovingPlayback) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options engine;
  engine.paper_journal = file.directory / "main.jsonl";
  std::promise<void> entered, release;
  const auto entered_future = entered.get_future();
  const auto released = release.get_future().share();
  std::atomic<bool> hold{true};
  server::ReplayHost::Options options{file.directory, engine, false};
  options.verification_progress = [&](std::uint64_t, std::uint64_t) {
    if (hold.exchange(false)) { entered.set_value(); released.wait(); }
  };
  std::string id, route;
  json receipt;
  {
    server::ReplayHost host(options);
    const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}, {"paused", true}});
    ASSERT_EQ(started.status, 201) << started.body;
    id = json::parse(started.body).at("replay").at("id").get<std::string>();
    route = "/api/replay/history/" + id + "/verify";
    EXPECT_EQ(replay_call(host, "POST", route).status, 409);
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", false}}).status, 200);
    ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick()).at("replay").at("finished").get<bool>(); }));
    EXPECT_EQ(json::parse(replay_call(host, "GET", route).body).at("status"), "idle");
    EXPECT_EQ(replay_call(host, "GET", route + "?format=receipt").status, 409);
    const auto accepted = replay_call(host, "POST", route);
    const auto waiting = entered_future.wait_for(5min);
    if (waiting != std::future_status::ready) { release.set_value(); }
    ASSERT_EQ(waiting, std::future_status::ready);
    EXPECT_EQ(accepted.status, 202) << accepted.body;
    EXPECT_EQ(json::parse(accepted.body).at("status"), "running");
    const auto refused = replay_call(host, "POST", route);
    EXPECT_EQ(refused.status, 409);
    EXPECT_EQ(json::parse(refused.body).at("error").at("code"), "VERIFICATION_RUNNING");
    EXPECT_EQ(replay_call(host, "DELETE", "/api/replay/history/" + id).status, 409);
    EXPECT_EQ(json::parse(replay_call(host, "GET", "/api/replay/history").body).at("history").at(0).at("verification").at("status"), "running");
    const auto other = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}, {"paused", true}});
    EXPECT_EQ(other.status, 201) << other.body;
    EXPECT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    const auto before = json::parse(host.tick()).at("replay");
    release.set_value();
    ASSERT_TRUE(test::recording_eventually([&] { return json::parse(replay_call(host, "GET", route).body).at("status") != "running"; }));
    const auto result = json::parse(replay_call(host, "GET", route).body);
    EXPECT_EQ(result.at("status"), "passed") << result;
    EXPECT_EQ(json::parse(host.tick()).at("replay"), before);
    const auto download = replay_call(host, "GET", route + "?format=receipt");
    EXPECT_EQ(download.status, 200);
    EXPECT_FALSE(download.download.empty());
    receipt = json::parse(download.body);
    EXPECT_EQ(receipt.at("run").at("id"), id);
    EXPECT_EQ(receipt.at("run").at("inputs").at(0).at("kind"), "recording");
    EXPECT_TRUE(receipt.at("run").at("inputs").at(0).contains("sha256"));
    EXPECT_EQ(receipt.dump().find(file.directory.string()), std::string::npos);
    EXPECT_FALSE(receipt.at("build").get<std::string>().empty());
    EXPECT_TRUE(receipt.at("equity").is_string());
    EXPECT_TRUE(receipt.at("finished_at").is_string());
  }
  server::ReplayHost restarted({file.directory, engine, false});
  EXPECT_EQ(json::parse(replay_call(restarted, "GET", route).body), receipt);
  const auto path = file.directory / "replays" / (id + ".jsonl");
  const auto original = read_file(path);
  const auto modified = std::filesystem::last_write_time(path);
  std::filesystem::last_write_time(path, modified + 1s);
  EXPECT_EQ(json::parse(replay_call(restarted, "GET", route).body).at("status"), "idle");
  EXPECT_EQ(replay_call(restarted, "GET", route + "?format=receipt").status, 409);
  std::filesystem::last_write_time(path, modified);
  { std::ofstream out(path, std::ios::app); out << '{'; }
  std::filesystem::last_write_time(path, modified);  // Size alone also invalidates the result.
  EXPECT_EQ(json::parse(replay_call(restarted, "GET", route).body).at("status"), "idle");
  ASSERT_EQ(replay_call(restarted, "POST", route).status, 202);
  ASSERT_TRUE(test::recording_eventually([&] { return json::parse(replay_call(restarted, "GET", route).body).at("status") != "running"; }));
  const auto failed = json::parse(replay_call(restarted, "GET", route).body);
  EXPECT_EQ(failed.at("status"), "failed");
  EXPECT_NE(failed.at("message").get<std::string>().find("torn final line"), std::string::npos);
  engine.write_mode = "disabled";
  server::ReplayHost readonly({file.directory, engine, false});
  EXPECT_EQ(replay_call(readonly, "POST", route).status, 403);
  EXPECT_EQ(json::parse(replay_call(readonly, "GET", route).body), failed);
  auto edited = original;
  const auto hash = edited.find("\"hash\":\"");
  ASSERT_NE(hash, std::string::npos);
  edited[hash + 8] = edited[hash + 8] == 'a' ? 'b' : 'a';
  { std::ofstream out(path); out << edited; }
  ASSERT_EQ(replay_call(restarted, "POST", route).status, 202);
  ASSERT_TRUE(test::recording_eventually([&] { return json::parse(replay_call(restarted, "GET", route).body).at("status") != "running"; }));
  const auto corrupt = json::parse(replay_call(restarted, "GET", route).body);
  EXPECT_EQ(corrupt.at("status"), "failed");
  EXPECT_NE(corrupt.at("message").get<std::string>().find("Broken journal"), std::string::npos);
}

TEST(ReplayRun, StopCancelsAndJoinsBackgroundVerification) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options engine;
  engine.paper_journal = file.directory / "main.jsonl";
  std::promise<void> entered, release;
  const auto ready = entered.get_future();
  const auto released = release.get_future().share();
  server::ReplayHost::Options options{file.directory, engine, false};
  options.verification_progress = [&](std::uint64_t, std::uint64_t) { entered.set_value(); released.wait(); };
  server::ReplayHost host(options);
  const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}});
  ASSERT_EQ(started.status, 201);
  const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
  const auto route = "/api/replay/history/" + id + "/verify";
  ASSERT_TRUE(test::recording_eventually([&] { return json::parse(host.tick()).at("replay").at("finished").get<bool>(); }));
  ASSERT_EQ(replay_call(host, "POST", route).status, 202);
  const auto waiting = ready.wait_for(5min);
  if (waiting != std::future_status::ready) { release.set_value(); }
  ASSERT_EQ(waiting, std::future_status::ready);
  auto stopping = std::async(std::launch::async, [&] { host.stop(); });
  EXPECT_EQ(stopping.wait_for(20ms), std::future_status::timeout);
  release.set_value();
  stopping.get();
  const auto status = json::parse(replay_call(host, "GET", route).body);
  EXPECT_EQ(status.at("status"), "failed");
  EXPECT_NE(status.at("message").get<std::string>().find("cancelled"), std::string::npos);
}

TEST(ReplayRun, HistoryWaitsForTheStoppingJournalsFinalSync) {
  test::RecordingFile file;
  write_stream(file.path, true);
  std::promise<void> syncing, release, complete_order;
  auto entered = syncing.get_future();
  const auto released = release.get_future().share();
  const auto order_released = complete_order.get_future().share();
  std::promise<server::ApiResponse> ordered;
  auto order = ordered.get_future();
  std::atomic<bool> hold{false};
  std::atomic<unsigned> syncs{0};
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  options.analytics.fallback_rate = 0;
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  options.journal_io.sync = [&](int) {
    ++syncs;
    if (hold.exchange(false)) { syncing.set_value(); released.wait(); }
    return true;
  };
  server::ReplayHost host({file.directory, options, false});
  const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 1}, {"paused", true}});
  ASSERT_EQ(started.status, 201) << started.body;
  const auto id = json::parse(started.body).at("replay").at("id");
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  // Drain the initial pause barrier and keep playback idle, with no batch in flight.
  ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", true}}).status, 200);
  // The owner thread invokes completion after recording the command, before its
  // paused-loop flush. Hold it here so the last record stays unsynced until stop
  // owns the history handoff; merely arming a hook before stop can catch the
  // pause flush triggered by stop's abort(), before it acquires that lock.
  const test::ScriptedMarket market;
  server::ApiRequest request{"POST", "/api/replay/orders", json{{"client_order_id", "before-stop"}, {"symbol", market.symbol()},
      {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}}.dump()};
  request.content_type = "application/json";
  request.actor = "test-actor";
  ASSERT_TRUE(host.handle(request, [&](server::ApiResponse response) {
    const bool accepted = response.status == 201;
    ordered.set_value(std::move(response));
    if (accepted) order_released.wait();
  }));
  const auto order_waiting = order.wait_for(5min);
  if (order_waiting != std::future_status::ready) complete_order.set_value();
  ASSERT_EQ(order_waiting, std::future_status::ready);
  const auto order_response = order.get();
  ASSERT_EQ(order_response.status, 201) << order_response.body;
  hold = true;
  auto stopping = std::async(std::launch::async, [&] { host.stop(); });
  // stop_session removes the active session only after acquiring handoff_mutex_,
  // which it holds through the engine's final sync and history finalization.
  const bool retiring = test::recording_eventually([&] { return host.tick().empty(); });
  complete_order.set_value();
  EXPECT_TRUE(retiring);
  if (!retiring) { release.set_value(); stopping.get(); return; }
  // Wait for the sync itself; the deadline only guards against a stuck worker.
  const auto waiting = entered.wait_for(5min);
  EXPECT_EQ(waiting, std::future_status::ready);
  if (waiting != std::future_status::ready) { release.set_value(); stopping.get(); return; }
  const auto final_syncs = syncs.load();
  auto listing = std::async(std::launch::async, [&] { return replay_call(host, "GET", "/api/replay"); });
  EXPECT_EQ(listing.wait_for(20ms), std::future_status::timeout);
  release.set_value();
  stopping.get();
  EXPECT_EQ(syncs.load(), final_syncs);
  const auto response = listing.get();
  ASSERT_EQ(response.status, 200) << response.body;
  const auto body = json::parse(response.body);
  EXPECT_TRUE(body.at("replay").is_null());
  ASSERT_EQ(body.at("history").size(), 1U);
  EXPECT_EQ(body.at("history").at(0).at("id"), id);
}

// B17 and D16: a step runs on the host's control thread, so neither its caller nor a
// listing waits for it; a write meanwhile is refused as REPLAY_STEPPING, not as a
// start state being prepared.
TEST(ReplayRun, AStepHoldsNoCallerAndRefusesWritesUntilItSettles) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  std::promise<void> syncing, release;
  auto entered = syncing.get_future();
  const auto released = release.get_future().share();
  std::atomic<bool> hold{false};
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  options.journal_io.sync = [&](int) {
    if (hold.exchange(false)) { syncing.set_value(); released.wait(); }
    return true;
  };
  server::ReplayHost host({file.directory, options, false});
  ASSERT_EQ(replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}, {"speed", 0}}).status, 201);
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  const json order{{"client_order_id", "during"}, {"symbol", market.symbol()}, {"side", "buy"}, {"type", "market"},
                   {"quantity", 1}, {"time_in_force", "ioc"}};
  // The step's settling barrier syncs the journal, which this test holds.
  hold = true;
  std::promise<server::ApiResponse> stepped;
  auto step = stepped.get_future();
  server::ApiRequest request{"PUT", "/api/replay", json{{"until", md::format_timestamp(market.time + 8 * md::kNanosPerSecond)}}.dump()};
  request.content_type = "application/json";
  auto handled = std::async(std::launch::async, [&] {
    return host.handle(request, [&](server::ApiResponse response) { stepped.set_value(std::move(response)); });
  });
  ASSERT_EQ(entered.wait_for(5min), std::future_status::ready);
  // The step stays held below, so only a caller the step blocks could time out here.
  const bool returned = handled.wait_for(10s) == std::future_status::ready;
  EXPECT_TRUE(returned) << "the step held the thread that asked for it";
  auto listing = std::async(std::launch::async, [&] { return replay_call(host, "GET", "/api/replay"); });
  const bool listed = listing.wait_for(60s) == std::future_status::ready;
  EXPECT_TRUE(listed) << "a listing waited for the step";
  if (!returned || !listed) {
    release.set_value();
    return;
  }
  const auto state = json::parse(listing.get().body).at("replay");
  EXPECT_EQ(state.at("stepping"), true);
  EXPECT_EQ(state.at("fast_forwarding"), false);
  const auto refused = replay_call(host, "POST", "/api/replay/orders", order);
  EXPECT_EQ(refused.status, 409) << refused.body;
  EXPECT_EQ(json::parse(refused.body).at("error").at("code"), "REPLAY_STEPPING");
  EXPECT_EQ(step.wait_for(0s), std::future_status::timeout);
  std::promise<server::ApiResponse> invalid_done;
  auto invalid = invalid_done.get_future();
  server::ApiRequest oversized{"PUT", "/api/replay", std::string("{\"paused\":true}") + std::string(64 * 1024, ' ')};
  oversized.content_type = "application/json";
  ASSERT_TRUE(host.handle(oversized, [&](server::ApiResponse response) { invalid_done.set_value(std::move(response)); }));
  release.set_value();
  ASSERT_EQ(step.wait_for(5min), std::future_status::ready);
  const auto settled = step.get();
  ASSERT_EQ(settled.status, 200) << settled.body;
  EXPECT_EQ(json::parse(settled.body).at("replay").at("stepping"), false);
  EXPECT_EQ(json::parse(settled.body).at("aborted"), false);
  EXPECT_EQ(invalid.get().status, 400);
  EXPECT_EQ(json::parse(settled.body).at("settled_through"), md::format_timestamp(market.time + 8 * md::kNanosPerSecond));
  EXPECT_TRUE(handled.get());
  EXPECT_EQ(replay_call(host, "POST", "/api/replay/orders", order).status, 201);
  host.stop();
}

TEST(ReplayRun, RelativeStepsSecondsAndQueuedSkipsAreExplicit) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  server::ReplayHost host({file.directory, options, false});
  const test::ScriptedMarket market;
  for (const auto& start : {std::string("10:00:02"), md::format_timestamp(market.time + 2 * md::kNanosPerSecond)}) {
    const auto response = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"start_at", start}, {"paused", true}});
    ASSERT_EQ(response.status, 201) << response.body;
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    EXPECT_EQ(json::parse(host.tick()).at("replay").at("settled_through"), md::format_timestamp(market.time + 2 * md::kNanosPerSecond));
  }
  auto queued = replay_call(host, "PUT", "/api/replay", {{"skip", true}});
  EXPECT_EQ(json::parse(queued.body).at("replay").at("skip_pending"), true);
  EXPECT_TRUE(json::parse(queued.body).contains("message"));
  EXPECT_EQ(json::parse(host.tick()).at("replay").at("skip_pending"), true);
  EXPECT_EQ(replay_call(host, "PUT", "/api/replay", {{"skip", false}}).status, 200);
  EXPECT_EQ(json::parse(host.tick()).at("replay").at("skip_pending"), false);
  const auto before = json::parse(host.tick()).at("replay");
  for (const auto* value : {"+15s", "+5m", "+1h", "+0s", "+1.5s", "+999999999999999999h", "+1d"}) {
    const auto refused = replay_call(host, "PUT", "/api/replay", {{"until", value}});
    EXPECT_EQ(refused.status, 400) << refused.body;
    EXPECT_EQ(json::parse(host.tick()).at("replay"), before);
  }
  EXPECT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "next"}}).status, 200);
  EXPECT_EQ(json::parse(host.tick()).at("replay").at("settled_through"), md::format_timestamp(market.time + 3 * md::kNanosPerSecond));
}

TEST(ReplayRun, AbortPauseAndDeleteInterruptStepsAndPreserveTheSettledJournal) {
  for (const auto* action : {"abort", "paused", "paused-speed", "delete"}) {
    test::RecordingFile file;
    write_stream(file.path, true);
    const test::ScriptedMarket market;
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    auto held = entered.get_future();
    std::atomic<bool> hold{false};
    std::atomic<int> clock{0};
    server::Engine::Options options;
    options.paper_journal = file.directory / "main.jsonl";
    options.journal_io.clock = [&] { return std::chrono::steady_clock::time_point{} + std::chrono::seconds(++clock); };
    options.journal_io.sync = [&](int) {
      if (hold.exchange(false)) { entered.set_value(); released.wait(); }
      return true;
    };
    server::ReplayHost host({file.directory, options, false});
    const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
    ASSERT_EQ(started.status, 201);
    const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    // Block the first batch's durable write, so the interrupt deterministically arrives mid-step.
    hold = true;
    auto step = std::async(std::launch::async, [&] { return replay_call(host, "PUT", "/api/replay", {{"until", "+10s"}}); });
    const auto waiting = held.wait_for(10s);
    EXPECT_EQ(waiting, std::future_status::ready);
    if (waiting != std::future_status::ready) { release.set_value(); host.stop(); return; }
    std::promise<server::ApiResponse> completed;
    auto interrupted = completed.get_future();
    server::ApiRequest request{std::string(action) == "delete" ? "DELETE" : "PUT", "/api/replay",
        std::string(action) == "delete" ? "{}" : std::string(action) == "paused-speed" ? json{{"paused", true}, {"speed", 60}}.dump() : json{{action, true}}.dump()};
    request.content_type = "application/json";
    ASSERT_TRUE(host.handle(request, [&](server::ApiResponse response) { completed.set_value(std::move(response)); }));
    EXPECT_EQ(json::parse(host.tick()).at("replay").at("stepping"), true);
    EXPECT_EQ(interrupted.wait_for(0s), std::future_status::timeout);
    EXPECT_EQ(replay_call(host, "POST", "/api/replay/orders", json::object()).status, 409);
    release.set_value();
    const auto result = step.get();
    ASSERT_EQ(result.status, 200) << result.body;
    const auto state = json::parse(result.body);
    EXPECT_EQ(state.at("aborted"), true);
    EXPECT_EQ(state.at("replay").at("paused"), true);
    EXPECT_EQ(state.at("settled_through"), md::format_timestamp(market.time + md::kNanosPerSecond));
    EXPECT_EQ(interrupted.get().status, 200);
    host.stop();
    const auto journal = file.directory / "replays" / (id + ".jsonl");
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    // The interrupted run is byte-identical to a normal step to its settled boundary.
    server::ReplayHost reference({file.directory, options, false});
    const auto fresh = replay_call(reference, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
    ASSERT_EQ(fresh.status, 201);
    const auto fresh_id = json::parse(fresh.body).at("replay").at("id").get<std::string>();
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(reference.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    ASSERT_EQ(replay_call(reference, "PUT", "/api/replay", {{"until", state.at("settled_through")}}).status, 200);
    reference.stop();
    EXPECT_EQ(read_file(journal), read_file(file.directory / "replays" / (fresh_id + ".jsonl")));
  }
}

TEST(ReplayRun, RestartPreservesTheSourceAndReexecutesItsInclusiveCommandPrefix) {
  for (const auto* ending : {"active", "stopped", "finished", "delayed"}) {
    test::RecordingFile file;
    const auto delay = std::string(ending) == "delayed" ? 15 * md::kNanosPerMinute : 0;
    write_stream(file.path, true, delay);
    const test::ScriptedMarket market;
    server::Engine::Options options;
    options.paper_journal = file.directory / "main.jsonl";
    server::ReplayHost host({file.directory, options, false});
    const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true},
        {"start_at", delay ? "10:15:02" : "10:00:02"}, {"speed", 60}});
    ASSERT_EQ(started.status, 201) << started.body;
    const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
    const auto journal = file.directory / "replays" / (id + ".jsonl");
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    EXPECT_EQ(replay_call(host, "POST", "/api/replay", {{"restart", "missing"}}).status, 404);
    for (const auto* at : {"09:59:00", "10:00:03", "next", "+1s"})
      EXPECT_EQ(replay_call(host, "POST", "/api/replay", {{"restart", id}, {"at", at}}).status, 400) << at;
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "10:00:04"}}).status, 200);
    const auto bought = replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "copied"}, {"symbol", market.symbol()},
        {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
    ASSERT_EQ(bought.status, 201) << bought.body;
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"paused", true}}).status, 200);
    const auto prefix = read_file(journal);
    const auto fills = json::parse(replay_call(host, "GET", "/api/replay/fills").body).at("fills");
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "10:00:08"}}).status, 200);
    ASSERT_EQ(replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "later"}, {"symbol", market.symbol()},
        {"side", "sell"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}}).status, 201);
    if (std::string(ending) == "finished") { ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "10:00:12"}}).status, 200); }
    if (std::string(ending) == "stopped") { ASSERT_EQ(replay_call(host, "DELETE", "/api/replay").status, 200); }
    const auto source_bytes = read_file(journal);
    const auto restarted = replay_call(host, "POST", "/api/replay", {{"restart", id}, {"at", "10:00:04"}});
    ASSERT_EQ(restarted.status, 201) << restarted.body;
    const auto state = json::parse(restarted.body).at("replay");
    EXPECT_EQ(state.at("settled_through"), md::format_timestamp(market.time + 4 * md::kNanosPerSecond));
    EXPECT_EQ(state.at("time"), md::format_timestamp(market.time + delay + 4 * md::kNanosPerSecond));
    EXPECT_EQ(state.at("paused"), true);
    EXPECT_EQ(state.at("speed"), 60);
    EXPECT_EQ(state.at("restarted_from"), (json{{"id", id}, {"at", md::format_timestamp(market.time + 4 * md::kNanosPerSecond)}}));
    const auto new_id = state.at("id").get<std::string>();
    EXPECT_NE(new_id, id);
    EXPECT_EQ(read_file(journal), source_bytes);
    const auto new_journal = file.directory / "replays" / (new_id + ".jsonl");
    EXPECT_EQ(read_file(new_journal), prefix);
    EXPECT_EQ(json::parse(replay_call(host, "GET", "/api/replay/fills").body).at("fills"), fills);
    // Continue with different commands; the source remains byte-identical.
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "+1s"}}).status, 200);
    ASSERT_EQ(replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "new-only"}, {"symbol", market.symbol()},
        {"side", "sell"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}}).status, 201);
    ASSERT_EQ(replay_call(host, "DELETE", "/api/replay").status, 200);
    const auto verified = server::verify_run(new_journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    EXPECT_EQ(read_file(journal), source_bytes);
    // A host restart keeps provenance and the original start when at is omitted.
    host.stop();
    server::ReplayHost next_host({file.directory, options, false});
    const auto again = replay_call(next_host, "POST", "/api/replay", {{"restart", id}});
    ASSERT_EQ(again.status, 201) << again.body;
    EXPECT_EQ(json::parse(again.body).at("replay").at("settled_through"), md::format_timestamp(market.time + 2 * md::kNanosPerSecond));
    EXPECT_TRUE(json::parse(replay_call(next_host, "GET", "/api/replay/history").body).dump().find("restarted_from") != std::string::npos);
  }
}

TEST(ReplayRun, RestartRefusesVerificationAndChangedOrTruncatedSources) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options engine;
  engine.paper_journal = file.directory / "main.jsonl";
  std::promise<void> entered, release;
  auto held = entered.get_future();
  const auto released = release.get_future().share();
  std::atomic<bool> once{true};
  server::ReplayHost::Options options{file.directory, engine, false};
  options.verification_progress = [&](std::uint64_t, std::uint64_t) { if (once.exchange(false)) { entered.set_value(); released.wait(); } };
  server::ReplayHost host(options);
  const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
  ASSERT_EQ(started.status, 201);
  const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "+5s"}}).status, 200);
  ASSERT_EQ(replay_call(host, "DELETE", "/api/replay").status, 200);
  const auto verifying = replay_call(host, "POST", "/api/replay/history/" + id + "/verify");
  EXPECT_EQ(verifying.status, 202);
  const auto waiting = held.wait_for(10s);
  EXPECT_EQ(waiting, std::future_status::ready);
  if (waiting != std::future_status::ready) { release.set_value(); return; }
  const auto refused = replay_call(host, "POST", "/api/replay", {{"restart", id}});
  EXPECT_EQ(refused.status, 409);
  EXPECT_EQ(json::parse(refused.body).at("error").at("code"), "VERIFICATION_RUNNING");
  release.set_value();
  ASSERT_TRUE(test::recording_eventually([&] { return json::parse(replay_call(host, "GET", "/api/replay/history/" + id + "/verify").body).at("status") != "running"; }));
  { std::ofstream changed(file.path, std::ios::app); changed << "changed"; }
  const auto changed = replay_call(host, "POST", "/api/replay", {{"restart", id}});
  EXPECT_EQ(changed.status, 409);
  EXPECT_EQ(json::parse(changed.body).at("error").at("code"), "REPLAY_NOT_RESTARTABLE");
  const auto journal = file.directory / "replays" / (id + ".jsonl");
  auto contents = read_file(journal);
  contents.resize(contents.rfind('\n', contents.size() - 2) + 1);
  { std::ofstream truncated(journal); truncated << contents; }
  const auto truncated = replay_call(host, "POST", "/api/replay", {{"restart", id}});
  EXPECT_EQ(truncated.status, 409);
  EXPECT_NE(json::parse(truncated.body).at("error").at("message").get<std::string>().find("head/count/bytes"), std::string::npos);
}

TEST(ReplayRun, StandaloneEngineFlushesBeforePublishingFinished) {
  test::RecordingFile file;
  write_stream(file.path, true);
  std::atomic<std::uintmax_t> synced_bytes{0};
  std::atomic<unsigned> syncs{0};
  providers::ReplayProvider replay({file.path, 0, false, {}});
  server::Engine::Options options;
  options.paper_journal = file.directory / "run.jsonl";
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  options.journal_io.sync = [&](int fd) {
    synced_bytes = static_cast<std::uintmax_t>(::lseek(fd, 0, SEEK_END));
    ++syncs;
    return true;
  };
  server::Engine engine(replay, {{"SPX"}}, options);
  engine.start();
  ASSERT_TRUE(test::recording_eventually([&] { return replay.finished(); }));
  EXPECT_EQ(synced_bytes.load(), std::filesystem::file_size(options.paper_journal));
  EXPECT_EQ(syncs.load(), 2U);  // First record and EOF, regardless of the market clock.
  EXPECT_TRUE(server::verify_run(options.paper_journal).matched);
  engine.stop();
}

TEST(ReproducibleRun, LockstepSmallAndLargeStepsMatchContinuousCommandsAndVerify) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  std::string golden;
  // Continuous headless run is the reference, including identical command instants.
  for (const int step : {0, 1, 4, -1, -2, -3}) {
    const auto directory = file.directory / std::to_string(step);
    std::filesystem::create_directory(directory);
    std::filesystem::path journal;
    if (step == 0) {
      journal = directory / "run.jsonl";
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.replay = true;
      options.initial_actor = "test-actor";
      options.run_input = server::recording_input(file.path);
      options.paper_journal = journal;
      // ReplayHost's default plan is practice.
      options.paper.rules = server::find_plan("practice")->rules;
      options.paper.initial_cash = server::find_plan("practice")->initial_cash;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (desk.market_time() == market.time + 4 * md::kNanosPerSecond) {
          server::TradingCommand order;
          order.actor = "test-actor";
          order.order = market.market("lockstep");
          EXPECT_TRUE(command(desk, order, desk.market_time(), batch->received).decision.ok());
        }
      }
      desk.stop();
      golden = read_file(journal);
    } else {
      server::Engine::Options options;
      options.paper_journal = directory / "main.jsonl";
      server::ReplayHost host({file.directory, options, false});
      const auto response = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}, {"speed", 0}});
      ASSERT_EQ(response.status, 201) << response.body;
      const auto id = json::parse(response.body).at("replay").at("id").get<std::string>();
      ASSERT_TRUE(test::recording_eventually([&] {
        return !json::parse(replay_call(host, "GET", "/api/replay").body).at("replay").at("fast_forwarding").get<bool>();
      }));
      for (int second = std::max(1, step); second <= 12; second += std::max(1, step)) {
        const auto time = market.time + second * md::kNanosPerSecond;
        const auto target = step == -1 ? std::string("+1s") : step == -2 ? std::string("next") : second % 2 == 0 ? md::format_timestamp(time) : "10:00:" + (second < 10 ? std::string("0") : std::string()) + std::to_string(second);
        const auto stepped = replay_call(host, "PUT", "/api/replay", {{step == -3 ? "play_until" : "until", target}});
        ASSERT_EQ(stepped.status, 200) << stepped.body;
        if (step == -3) {
          ASSERT_TRUE(test::recording_eventually([&] {
            const auto state = json::parse(host.tick()).at("replay");
            return state.at("paused") == true && state.at("pause_at").is_null() && state.at("settled_through") == md::format_timestamp(time);
          }));
        } else {
          EXPECT_EQ(json::parse(stepped.body).at("settled_through"), md::format_timestamp(time));
        }
        if (second == 4) {
          const auto bought = replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", "lockstep"}, {"symbol", market.symbol()},
              {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
          ASSERT_EQ(bought.status, 201) << bought.body;
          if (step == 1) {
            const auto gap = replay_call(host, "PUT", "/api/replay", {{"until", md::format_timestamp(time + md::kNanosPerSecond / 2)}});
            ASSERT_EQ(gap.status, 200) << gap.body;
            const auto preview = replay_call(host, "POST", "/api/replay/orders/preview", {{"client_order_id", "preview"},
                {"symbol", market.symbol()}, {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
            EXPECT_EQ(preview.status, 200) << preview.body;
          }
        }
      }
      host.stop();
      journal = directory / "replays" / (id + ".jsonl");
      EXPECT_TRUE(read_file(journal) == golden);
    }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}
TEST(ReproducibleRun, LockstepWaitsForTheConsumerAndRejectsBackwardAndPastEndTimes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  providers::ReplayProvider replay({file.path, 0, false, {}, 0, true});
  auto held = std::make_shared<std::promise<void>>();
  std::atomic<bool> received{false};
  replay.set_driver([&](providers::ReplayBatch batch) {
    if (batch.time == market.time + md::kNanosPerSecond) {
      received = true;
      return held->get_future();
    }
    std::promise<void> done;
    done.set_value();
    return done.get_future();
  });
  test::DiscardEvents discard;
  replay.start({{"SPX"}}, discard);
  ASSERT_TRUE(test::recording_eventually([&] { return !replay.fast_forwarding(); }));
  auto advancing = std::async(std::launch::async, [&] { replay.until(market.time + md::kNanosPerSecond); });
  ASSERT_TRUE(test::recording_eventually([&] { return received.load(); }));
  EXPECT_EQ(advancing.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(replay.settled_through(), market.time);
  held->set_value();
  ASSERT_EQ(advancing.wait_for(5min), std::future_status::ready);
  EXPECT_NO_THROW(advancing.get());
  EXPECT_TRUE(replay.paused());
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond);
  EXPECT_THROW(replay.until(market.time), std::invalid_argument);
  // A target past EOF is refused before anything plays (B05).
  EXPECT_THROW(replay.until(market.time + 20 * md::kNanosPerSecond), std::invalid_argument);
  EXPECT_EQ(replay.settled_through(), market.time + md::kNanosPerSecond);
  EXPECT_FALSE(replay.finished());
  EXPECT_EQ(replay.end_time(), market.time + 12 * md::kNanosPerSecond);
  EXPECT_NO_THROW(replay.until(market.time + 12 * md::kNanosPerSecond));
  EXPECT_EQ(replay.settled_through(), market.time + 12 * md::kNanosPerSecond);
  replay.stop();
}

TEST(ReproducibleRun, WalkingChangesAndInsideFillsVerifyWithIdenticalBytes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  const json unchanged = trading::OrderChange{};
  EXPECT_FALSE(unchanged.contains("walk"));
  EXPECT_FALSE(unchanged.get<trading::OrderChange>().walk);
  std::string golden;
  for (int repeat = 0; repeat < 2; ++repeat) {
    const auto journal = file.directory / ("walking-" + std::to_string(repeat) + ".jsonl");
    {
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.run_input = server::recording_input(file.path);
      options.replay = true;
      options.paper_journal = journal;
      options.paper.rules.inside_fill_percent = 50;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      int second = 0;
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (second <= 2) {
          server::TradingCommand request;
          if (second == 0) {
            request.order = market.limit("walking", 1, "99.50");
            request.order.walk = trading::Walk{Money::parse("0.10"), 2, Money::parse("101.00")};
          } else {
            request.kind = server::TradingCommand::Kind::Modify;
            request.order_id = 1;
            request.change.walk.emplace(std::nullopt);
            if (second == 2) request.change.walk.emplace(trading::Walk{Money::parse("0.20"), 1, Money::parse("102.00")});
          }
          const auto reply = command(desk, request, batch->time, batch->received);
          ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
        }
        ++second;
      }
      const auto snapshot = desk.trading_view()->snapshot;
      ASSERT_EQ(snapshot->recent_fills.size(), 1U);
      EXPECT_EQ(snapshot->recent_fills.front().price, Money::parse("101.10"));
      EXPECT_EQ(snapshot->recent_fills.front().time, market.time + 10 * md::kNanosPerSecond);
      EXPECT_EQ(snapshot->recent_orders.front().changes.size(), 10U);
      desk.stop();
    }
    if (repeat == 0) { golden = read_file(journal); } else { EXPECT_EQ(read_file(journal), golden); }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

TEST(ReproducibleRun, OverrideRemovalVerifiesWithDriverSix) {
  test::RecordingFile file;
  write_stream(file.path, true);
  for (const bool overrides : {false, true}) {
    SCOPED_TRACE(overrides ? "with overrides" : "without overrides");
    const auto journal = file.directory / (overrides ? "overrides.jsonl" : "no-overrides.jsonl");
    {
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.run_input = server::recording_input(file.path);
      options.replay = true;
      options.paper_journal = journal;
      options.paper.rules.max_drawdown = Money::parse("1000");
      if (overrides) options.paper.limits.underlying_overrides["SPX"] = {2e6, 2e4};
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      bool changed = false;
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (!changed) {
          server::TradingCommand request;
          request.kind = server::TradingCommand::Kind::Limits;
          request.expected_revision = desk.trading_view()->snapshot->risk.limits_revision;
          request.limits = desk.trading_view()->config.limits;
          request.limits.underlying_overrides.clear();
          const auto reply = command(desk, request, batch->time, batch->received);
          ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
          EXPECT_FALSE(desk.trading_view()->snapshot->pending_limits);
          EXPECT_TRUE(desk.trading_view()->config.limits.underlying_overrides.empty());
          changed = true;
        }
      }
      desk.stop();
    }
    const auto inputs = server::run_inputs(trading::FileJournal::read(journal.string()));
    ASSERT_FALSE(inputs.empty());
    EXPECT_EQ(json::parse(inputs.front()).at("driver"), 6);
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
  }
}

TEST(ReproducibleRun, ConservativeFillsHaveIdenticalBytesAndPassCliVerification) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const test::ScriptedMarket market;
  std::string golden;
  for (int repeat = 0; repeat < 2; ++repeat) {
    const auto journal = file.directory / ("conservative-" + std::to_string(repeat) + ".jsonl");
    {
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.run_input = server::recording_input(file.path);
      options.replay = true;
      options.paper_journal = journal;
      options.paper.limits.aggregate = {1e9, 1e9};
      options.paper.limits.per_underlying = {1e9, 1e9};
      options.paper.rules.fill_latency_ms = 1000;
      options.paper.rules.impact_ticks = 1;
      options.paper.rules.slippage_ticks = 1;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription);
      bool submitted = false;
      while (const auto batch = batches.next()) {
        desk.replay_batch(batch->events, batch->received, batch->time);
        if (!submitted) {
          server::TradingCommand request;
          request.order = market.market("conservative", 25);
          const auto reply = command(desk, request, batch->time, batch->received);
          ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
          EXPECT_TRUE(desk.trading_view()->snapshot->recent_fills.empty());
          submitted = true;
        }
      }
      const auto& fills = desk.trading_view()->snapshot->recent_fills;
      ASSERT_EQ(fills.size(), 2U);
      EXPECT_EQ(fills.front().time, market.time + md::kNanosPerSecond);
      EXPECT_EQ(fills.front().quantity, 20);
      EXPECT_EQ(fills.back().quantity, 5);
      EXPECT_EQ(fills.front().price, Money::parse("100.40"));
      EXPECT_EQ(fills.back().price, Money::parse("100.50"));
      desk.stop();
    }
    if (repeat == 0) { golden = read_file(journal); } else { EXPECT_EQ(read_file(journal), golden); }
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    const auto cli = std::string(OPENPORT_APPS_DIR) + "/openportd --verify-run " + journal.string() +
                     " > " + (file.directory / "verify-fills.txt").string() + " 2>&1";
    EXPECT_EQ(std::system(cli.c_str()), 0) << read_file(file.directory / "verify-fills.txt");
  }
}

/// A run of write_stream's recording that buys, rests an order, cancels it and flattens.
void trade_the_stream(server::Desk& desk, providers::ReplayBatches& batches) {
  const test::ScriptedMarket market;
  trading::OrderId resting = 0;
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    const auto second = (desk.market_time() - market.time) / md::kNanosPerSecond;
    server::TradingCommand request;
    request.actor = "agent";
    if (second == 3) request.order = market.market("buy");
    else if (second == 5) request.order = market.limit("rest", 1, "103.00", trading::Side::Sell);
    else if (second == 8) {
      request.kind = server::TradingCommand::Kind::Cancel;
      request.order_id = resting;
    } else if (second == 10) request.kind = server::TradingCommand::Kind::ClosePositions;
    else continue;
    const auto reply = command(desk, request, desk.market_time(), batch->received);
    EXPECT_TRUE(reply.decision.ok()) << reply.decision.message;
    if (second == 5) resting = reply.order_id.value_or(0);
  }
}
/// Writes the first `count` lines of `journal` beside it, as a crash after that record leaves it.
std::filesystem::path first_records(const std::filesystem::path& journal, std::size_t count) {
  std::istringstream lines(read_file(journal));
  const auto cut = journal.parent_path() / ("cut-" + std::to_string(count) + ".jsonl");
  std::ofstream out(cut, std::ios::binary | std::ios::trunc);
  std::string line;
  for (std::size_t n = 0; n < count && std::getline(lines, line); ++n) out << line << '\n';
  return cut;
}

TEST(ReproducibleRun, EveryRecordAJournalCanEndAtVerifies) {
  // Each input and each transaction is its own append, so a crash can end the journal
  // after any record: between a batch's transactions and its boundary input, or between
  // a command's input and its transactions. Each such journal verifies through its end.
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = file.directory / "run.jsonl";
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.run_input = server::recording_input(file.path);
    options.replay = true;
    options.paper_journal = journal;
    server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    trade_the_stream(desk, batches);
    ASSERT_EQ(desk.trading_view()->snapshot->recent_orders.size(), 3U);
  }
  const auto records = trading::FileJournal::read(journal.string()).records;
  ASSERT_GT(records.size(), 20U);
  // The start input is the second record; before it the journal is no run yet.
  for (std::size_t count = 2; count <= records.size(); ++count) {
    const auto verified = server::verify_run(first_records(journal, count));
    EXPECT_TRUE(verified.matched) << count << " records ending with " << records[count - 1].type << ": " << verified.message;
    EXPECT_EQ(verified.transactions, count);
    EXPECT_EQ(verified.head, records[count - 1].hash);
  }
}

TEST(ReproducibleRun, OlderDriversVerifyACutJournalWhereTheRecordingExplainsIt) {
  // Drivers before 4 recorded a command's input after its transactions: a journal cut
  // between them cannot say what the command was. Cut between a batch's transactions
  // and its boundary input, the recording's next batch reproduces them.
  test::RecordingFile file;
  write_stream(file.path, true);
  for (const int driver : {1, 2, 3}) {
    SCOPED_TRACE("driver " + std::to_string(driver));
    const auto journal = file.directory / ("driver-" + std::to_string(driver) + ".jsonl");
    {
      md::RecordingReader reader(file.path);
      server::Desk::Options options;
      options.run_input = server::recording_input(file.path);
      options.replay = true;
      options.instant_batches = driver >= 2;
      options.closing_rollover = driver >= 3;
      options.inputs_first = false;
      options.paper_journal = journal;
      server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
      desk.start_trading();
      providers::ReplayBatches batches(reader, reader.header().subscription, driver >= 2);
      trade_the_stream(desk, batches);
    }
    const auto text = read_file(journal);
    EXPECT_EQ(text.find("\"driver\":"), driver == 1 ? std::string::npos : text.find("\"driver\":" + std::to_string(driver)));
    const auto records = trading::FileJournal::read(journal.string()).records;
    std::size_t refused = 0, cut = 0;
    for (std::size_t count = 2; count <= records.size(); ++count) {
      // The input that follows the last record kept, and what it was.
      std::string next;
      for (auto i = count; i < records.size() && next.empty(); ++i)
        if (records[i].type == "run_input")
          next = json::parse(records[i].payload).at("events").at(0).at("payload").at("kind").get<std::string>();
      std::size_t inputs = count;  // records through the last input kept
      while (records[inputs - 1].type != "run_input") --inputs;
      const bool explained = inputs == count || next != "command";
      const auto verified = server::verify_run(first_records(journal, count));
      EXPECT_EQ(verified.matched, explained) << count << ": " << verified.message;
      if (!explained) {
        ++refused;
        EXPECT_NE(verified.message.find("First differing transaction " + std::to_string(inputs + 1) + " ("), std::string::npos)
            << verified.message;
      } else if (verified.cut) {
        ++cut;
        EXPECT_NE(verified.message.find("part way through its last operation"), std::string::npos) << verified.message;
      }
    }
    EXPECT_GT(refused, 0U);
    EXPECT_GT(cut, 0U);
  }
}

TEST(ReproducibleRun, MissingRecordingAndOldJournalsHaveClearDiagnostics) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = file.directory / "run.jsonl";
  {
    md::RecordingReader reader(file.path);
    server::Desk::Options options;
    options.run_input = server::recording_input(file.path);
    options.replay = true;
    options.paper_journal = journal;
    server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    providers::ReplayBatches batches(reader, reader.header().subscription);
    desk.replay_batch(batches.next()->events, reader.header().started);
    desk.set_dividends({{"SPX", {2026, 9, 23}, Money::parse("1.00")}});
  }
  EXPECT_TRUE(server::verify_run(journal).matched);
  { std::ofstream out(file.path, std::ios::app); out << 'x'; }
  EXPECT_NE(server::verify_run(journal).message.find("Recording input changed"), std::string::npos);
  std::filesystem::remove(file.path);
  EXPECT_NE(server::verify_run(journal).message.find("Missing input"), std::string::npos);
  const auto old = file.directory / "old.jsonl";
  { trading::TradingSession session({}, 0, trading::FileJournal::create(old.string())); }
  EXPECT_NO_THROW((void)trading::TradingSession::recover(trading::FileJournal::read(old.string())));
  EXPECT_NE(server::verify_run(old).message.find("older journals still load"), std::string::npos);
}
}  // namespace

namespace {
TEST(ReplayRun, ContractFixture) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.write_mode = "open";
  server::ReplayHost host({file.directory, options, false});
  const auto capture = [&](std::string method, std::string path, json body = json::object()) {
    const auto response = replay_call(host, method, path, body);
    EXPECT_GE(response.status, 200);
    EXPECT_LT(response.status, 300) << response.body;
    test::capture_contract("replay", method, path, response);
    return json::parse(response.body);
  };
  capture("GET", "/api/replay");
  const auto started = capture("POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
  const auto id = started.at("replay").at("id").get<std::string>();
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
  capture("PUT", "/api/replay", {{"until", "10:00:01"}});
  capture("GET", "/api/replay/status");
  capture("GET", "/api/replay/account");
  capture("GET", "/api/replay/settlements");
  capture("DELETE", "/api/replay");
  capture("GET", "/api/replay");
  capture("GET", "/api/replay/history/" + id);
  capture("GET", "/api/replay/history/" + id + "/trades");
  capture("GET", "/api/replay/history/" + id + "/settlements");
}
}  // namespace

namespace {
TEST(ReplayRun, ARunWhoseFirstRecordFailsLeavesNoHistoryEntry) {
  test::RecordingFile file;
  write_stream(file.path, true);
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.write_mode = "open";
  std::atomic<bool> fail = true;
  options.journal_io.sync = [&](int) { return !fail; };
  server::ReplayHost host({file.directory, options, false});
  auto response = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
  EXPECT_EQ(response.status, 422) << response.body;
  EXPECT_NE(response.body.find("JOURNAL_IO"), std::string::npos) << response.body;
  EXPECT_TRUE(json::parse(replay_call(host, "GET", "/api/replay").body).at("history").empty());
  EXPECT_TRUE(std::filesystem::is_empty(file.directory / "replays"));
  fail = false;
  response = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}});
  ASSERT_EQ(response.status, 201) << response.body;
  replay_call(host, "DELETE", "/api/replay");
  EXPECT_EQ(json::parse(replay_call(host, "GET", "/api/replay").body).at("history").size(), 1U);
}

TEST(ReplayRun, CommandActorDefaultsForOlderInputs) {
  server::TradingCommand command;
  command.actor = "agent";
  auto recorded = json(command);
  EXPECT_EQ(recorded.get<server::TradingCommand>().actor, "agent");
  recorded.erase("actor");
  EXPECT_EQ(recorded.get<server::TradingCommand>().actor, "unknown");
}

/// A run with one order an agent placed, recorded with or without driver 2's instants.
std::filesystem::path attributed_run(const test::RecordingFile& file, bool instants) {
  const auto journal = file.directory / (instants ? "run.jsonl" : "run-driver-1.jsonl");
  const test::ScriptedMarket market;
  md::RecordingReader reader(file.path);
  server::Desk::Options options;
  options.run_input = server::recording_input(file.path);
  options.replay = true;
  options.instant_batches = instants;
  options.paper_journal = journal;
  server::Desk desk("replay (synthetic)", reader.header().capabilities, reader.header().subscription, options);
  desk.start_trading();
  providers::ReplayBatches batches(reader, reader.header().subscription, instants);
  while (const auto batch = batches.next()) {
    desk.replay_batch(batch->events, batch->received, batch->time);
    if (desk.market_time() == market.time + 4 * md::kNanosPerSecond) {
      server::TradingCommand order;
      order.actor = "agent";
      order.order = market.market("older-order");
      EXPECT_TRUE(command(desk, order, desk.market_time(), batch->received).decision.ok());
    }
  }
  return journal;
}
/// Copies `journal` with a new hash chain, removing every actor from the records `strip` picks.
std::filesystem::path without_actors(const std::filesystem::path& journal, const std::string& name,
                                     const std::function<bool(const trading::JournalRecord&)>& strip) {
  const std::function<void(json&)> erase = [&](json& value) {
    if (value.is_object()) value.erase("actor");
    if (value.is_structured()) {
      for (auto& child : value) erase(child);
    }
  };
  const auto copy = journal.parent_path() / name;
  auto output = trading::FileJournal::create(copy.string());
  for (const auto& record : trading::FileJournal::read(journal.string()).records) {
    auto payload = json::parse(record.payload);
    if (strip(record)) erase(payload);
    output->append(record.time, record.type, payload.dump());
  }
  return copy;
}

TEST(ReplayRun, JournalsWithoutActorsStillVerifyTheirOriginalHashes) {
  test::RecordingFile file;
  write_stream(file.path, true);
  // Builds before actors wrote no driver version.
  const auto older = without_actors(attributed_run(file, false), "older.jsonl", [](const auto&) { return true; });
  const auto recovery = trading::FileJournal::read(older.string());
  const auto restored = trading::TradingSession::recover(recovery);
  ASSERT_FALSE(restored.snapshot()->recent_fills.empty());
  EXPECT_EQ(restored.snapshot()->recent_fills.front().actor, "unknown");
  const auto verified = server::verify_run(older);
  EXPECT_TRUE(verified.matched) << verified.message;
  EXPECT_EQ(verified.head, recovery.head);
}

TEST(ReplayRun, AnActorRemovedFromOneRecordIsADifference) {
  test::RecordingFile file;
  write_stream(file.path, true);
  const auto journal = attributed_run(file, true);
  ASSERT_TRUE(server::verify_run(journal).matched);
  const auto records = trading::FileJournal::read(journal.string()).records;
  const auto submit = std::find_if(records.begin(), records.end(), [](const auto& r) { return r.type == "submit"; });
  ASSERT_NE(submit, records.end());
  ASSERT_EQ(submit->actor, "agent");
  // Who placed the order cannot be erased from the order's record, nor from the
  // command's run input, the other record that names it.
  const auto order = without_actors(journal, "order.jsonl", [&](const auto& r) { return r.seq == submit->seq; });
  auto verified = server::verify_run(order);
  EXPECT_FALSE(verified.matched);
  EXPECT_NE(verified.message.find("First differing transaction " + std::to_string(submit->seq) + " (submit)"),
            std::string::npos) << verified.message;
  const auto input = std::find_if(records.begin(), records.end(), [](const auto& r) {
    return r.type == "run_input" && r.actor == "agent";
  });
  ASSERT_NE(input, records.end());
  const auto command = without_actors(journal, "command.jsonl", [&](const auto& r) { return r.seq == input->seq; });
  verified = server::verify_run(command);
  EXPECT_FALSE(verified.matched);
  EXPECT_NE(verified.message.find("First differing transaction " + std::to_string(std::min(input->seq, submit->seq))),
            std::string::npos) << verified.message;
  // Nor from every record of a run whose driver version postdates actors.
  verified = server::verify_run(without_actors(journal, "all.jsonl", [](const auto&) { return true; }));
  EXPECT_FALSE(verified.matched);
  EXPECT_NE(verified.message.find("First differing transaction 1 (session_start)"), std::string::npos) << verified.message;
}

TEST(ReproducibleRun, PositionDisposalReplaysRecoversAndKeepsSparseCommandFields) {
  test::RecordingFile file;
  test::ScriptedMarket market;
  market.contract = *md::parse_osi("SPXW260922C05000000");
  const auto other = *md::parse_osi("SPXW260922C05005000");
  auto header = test::recording_header();
  header.started = market.time;
  header.capabilities.delay = 0s;
  header.capabilities.poll_interval = 1s;
  md::Timestamp receipt = market.time;
  md::RecordingSink::Options recording;
  recording.clock = [&] { return receipt; };
  test::DiscardEvents discard;
  md::RecordingSink sink(file.path, header, discard, recording);
  sink.publish(md::ContractDefinition{0, market.contract});
  sink.publish(md::ContractDefinition{1, other});
  for (const auto time : {market.time, market.time + md::kNanosPerSecond, market.contract.expiry_time()}) {
    receipt = time;
    sink.publish(md::UnderlyingQuote{"SPX", time, 5010, 5010, 5010});
    sink.publish(md::OptionQuote{0, time, 10, 10.2, 10, 10});
    sink.publish(md::OptionQuote{1, time, time == market.time ? 5.0 : 0.0, 5.2, time == market.time ? 10.0 : 0.0, 10});
    sink.publish(md::SnapshotComplete{"SPX", time});
  }
  sink.close();
  server::TradingCommand command;
  json serialized = command;
  EXPECT_FALSE(serialized.contains("do_not_exercise"));
  EXPECT_FALSE(serialized.get<server::TradingCommand>().do_not_exercise) << "older command inputs have no field";
  std::string golden;
  for (const auto* name : {"first", "second"}) {
    const auto directory = file.directory / name;
    std::filesystem::create_directory(directory);
    server::Engine::Options options;
    options.paper_journal = directory / "main.jsonl";
    server::ReplayHost host({file.directory, options, false});
    const auto started = replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"speed", 0}, {"paused", true}});
    ASSERT_EQ(started.status, 201) << started.body;
    const auto id = json::parse(started.body).at("replay").at("id").get<std::string>();
    ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); }));
    for (const auto& c : {market.contract, other}) {
      const auto bought = replay_call(host, "POST", "/api/replay/orders", {{"client_order_id", c.osi_symbol()}, {"symbol", c.osi_symbol()},
          {"side", "buy"}, {"type", "market"}, {"quantity", 1}, {"time_in_force", "ioc"}});
      ASSERT_EQ(bought.status, 201) << bought.body;
    }
    for (const bool instructed : {true, false, true}) {
      const auto result = replay_call(host, "POST", "/api/replay/positions/instruction", {{"symbol", market.symbol()}, {"do_not_exercise", instructed}});
      ASSERT_EQ(result.status, 200) << result.body;
      EXPECT_EQ(json::parse(result.body).at("positions").at(0).at("do_not_exercise"), instructed);
    }
    auto journal = directory / "replays" / (id + ".jsonl");
    auto recovered = trading::TradingSession::recover(trading::FileJournal::read(journal.string()));
    ASSERT_EQ(recovered.snapshot()->positions.size(), 2u);
    EXPECT_TRUE(recovered.snapshot()->positions.front().do_not_exercise);
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "10:00:01"}}).status, 200);
    const auto abandoned = replay_call(host, "POST", "/api/replay/positions/abandon", {{"symbol", other.osi_symbol()}});
    ASSERT_EQ(abandoned.status, 200) << abandoned.body;
    const auto cash = json::parse(abandoned.body).at("cash");
    ASSERT_EQ(replay_call(host, "PUT", "/api/replay", {{"until", "16:00"}}).status, 200);
    const auto portfolio = json::parse(replay_call(host, "GET", "/api/replay/portfolio").body);
    EXPECT_TRUE(portfolio.at("positions").empty());
    EXPECT_EQ(portfolio.at("cash"), cash);
    EXPECT_EQ(json::parse(replay_call(host, "GET", "/api/replay/trades").body).at("trades").size(), 2u);
    host.stop();
    recovered = trading::TradingSession::recover(trading::FileJournal::read(journal.string()));
    EXPECT_TRUE(recovered.snapshot()->positions.empty());
    ASSERT_EQ(recovered.snapshot()->closures.size(), 2u);
    EXPECT_EQ(recovered.snapshot()->closures.front().kind, trading::ClosureKind::Abandon);
    EXPECT_EQ(recovered.snapshot()->closures.back().price, Money{});
    const auto verified = server::verify_run(journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    if (golden.empty()) golden = read_file(journal);
    else { EXPECT_EQ(read_file(journal), golden); }
  }
}

TEST(ReproducibleRun, CalendarRulesCaptureResetCalendarAndVerifyWithoutDriverChange) {
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.02, 10.20, 2.02}, {4.02, 10.20, 2.02},
              {4.02, 10.20, 2.02}, {4.02, 10.20, 2.02}, {4.02, 10.20, 2.02}});
  const auto journal = feed.file.directory / "calendar.jsonl";
  trading::AccountRules expected;
  {
    server::Desk::Options options;
    options.replay = true; options.run_input = server::recording_input(feed.file.path); options.paper_journal = journal;
    auto desk = feed.desk(options); desk.start_trading();
    md::RecordingReader reader(feed.file.path); providers::ReplayBatches batches(reader, feed.subscription);
    auto batch = batches.next();
    for (; batch && batch->time == feed.at(0); batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
    server::TradingCommand reset;
    reset.kind = server::TradingCommand::Kind::ResetAccount; reset.initial_cash = Money::parse("100000"); reset.reason = "F6 F17 F59 F31 F58 F62 F15 F60";
    reset.rules.flat_time = 601; reset.rules.no_overnight = true;
    reset.rules.profit_target = Money::parse("1000"); reset.rules.max_drawdown = Money::parse("5000");
    reset.rules.time_limit_days = 30; reset.rules.inactivity_days = 14;
    reset.rules.underlyings = {"SPX", "SPY"}; reset.rules.trading_start = 570; reset.rules.trading_end = 960;
    reset.rules.max_contracts_held = 5; reset.rules.require_stop_loss = true;
    reset.rules.max_trade_risk = Money::parse("2000.123456"); reset.rules.max_trade_risk_percent = 100;
    reset.rules.events = {{"news", md::format_timestamp(feed.at(1)), "SPX", "", "CPI"},
        {"earnings", "2026-09-22", "SPY", "after_close", "Results"}};
    reset.rules.news_after_minutes = 1; reset.rules.news_action = "flatten";
    reset.rules.hold_restrictions = {"earnings"}; reset.rules.hold_cutoff = 10 * 60 + 1;
    {
      md::ScheduledDaysScope changed({{{2030, 1, 2}, "Calendar changed after run start", true, 13, 0}});
      const auto reply = command(desk, reset, feed.at(0), feed.at(0));
      ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    }
    expected = desk.trading_view()->config.rules;
    for (const auto index : {1U, 2U}) {
      server::TradingCommand entry;
      entry.order = {"combined-" + std::to_string(index), feed.symbol(index), trading::Side::Buy,
          trading::OrderType::Market, trading::TimeInForce::Ioc, 1, {}, {}, {}, {}};
      entry.order.bracket = trading::Bracket{trading::ExitSpec{
          trading::Trigger{trading::TriggerSource::Option, trading::TriggerDirection::AtOrBelow, Money::parse("1")}, {}}, {}};
      const auto reply = command(desk, entry, feed.at(0), feed.at(0));
      ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    }
    for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
    EXPECT_TRUE(desk.trading_view()->snapshot->positions.empty());
    EXPECT_EQ(desk.trading_view()->snapshot->evaluation.event_actions.size(), 2U);
    desk.stop();
  }
  const auto recovery = trading::FileJournal::read(journal.string());
  const auto inputs = server::run_inputs(recovery);
  ASSERT_FALSE(inputs.empty()); EXPECT_EQ(json::parse(inputs.front()).at("driver"), 6);
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
  auto recovered = trading::TradingSession::recover(recovery);
  EXPECT_TRUE(recovered.snapshot()->positions.empty());
  EXPECT_EQ(recovered.config().rules, expected);
  ASSERT_TRUE(recovered.config().rules.hold_calendar);
  ASSERT_EQ(recovered.config().rules.hold_calendar->size(), 1U);
  EXPECT_EQ(recovered.config().rules.hold_calendar->front().name, "Calendar changed after run start");
}

TEST(ReproducibleRun, OpeningShareCommandsRecordTheirPriceAndRecoverCoveredCalls) {
  PolledInstants feed;
  feed.write({{4.02, 10.20, 2.02}, {4.12, 11.20, 2.12}});
  const auto journal = feed.file.directory / "shares.jsonl";
  std::string expected;
  {
    server::Desk::Options options;
    options.replay = true;
    options.run_input = server::recording_input(feed.file.path);
    options.paper_journal = journal;
    options.paper.rules.buying_power = true;
    auto desk = feed.desk(options);
    desk.start_trading();
    md::RecordingReader reader(feed.file.path);
    providers::ReplayBatches batches(reader, feed.subscription);
    auto batch = batches.next();
    for (; batch && batch->time == feed.at(0); batch = batches.next())
      desk.replay_batch(batch->events, batch->received, batch->time);
    server::TradingCommand shares;
    shares.kind = server::TradingCommand::Kind::TradeStock;
    shares.symbol = "QQQ";
    shares.quantity = 100;
    const auto bought = command(desk, shares, feed.at(0), feed.at(0));
    ASSERT_TRUE(bought.decision.ok()) << bought.decision.message;
    ASSERT_TRUE(order(desk, feed.symbol(0), trading::Side::Sell, 1, feed.at(0)).decision.ok());
    EXPECT_EQ(desk.trading_view()->snapshot->buying_power.short_requirement, Money{});
    for (; batch; batch = batches.next()) desk.replay_batch(batch->events, batch->received, batch->time);
    shares.kind = server::TradingCommand::Kind::CloseStock;
    shares.quantity = 1;
    ASSERT_TRUE(command(desk, shares, desk.market_time(), desk.market_time()).decision.ok());
    expected = trading::TradingSession::recover(trading::FileJournal::read(journal.string())).snapshot_json();
    desk.stop();
  }
  const auto verified = server::verify_run(journal);
  EXPECT_TRUE(verified.matched) << verified.message;
  auto recovered = trading::TradingSession::recover(trading::FileJournal::read(journal.string()));
  EXPECT_EQ(recovered.snapshot_json(), expected);
  ASSERT_EQ(recovered.snapshot()->stocks.size(), 1U);
  EXPECT_EQ(recovered.snapshot()->stocks.front().position.shares, 99);
  EXPECT_TRUE(recovered.snapshot()->stocks.front().mark.has_value());
  bool opening = false, closing = false;
  for (const auto& record : trading::FileJournal::read(journal.string()).records) {
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events")) {
      if (event.at("type") != "run_input") continue;
      const auto& input = event.at("payload");
      if (input.value("kind", "") != "command") continue;
      const auto& c = input.at("command");
      if (c.at("kind") == static_cast<int>(server::TradingCommand::Kind::TradeStock)) {
        opening = true;
        ASSERT_TRUE(c.contains("stock_price"));
        EXPECT_EQ(c["stock_price"]["symbol"], "QQQ");
        EXPECT_EQ(json(c.get<server::TradingCommand>()), c);
      } else {
        EXPECT_FALSE(c.contains("stock_price"));
        if (c.at("kind") == static_cast<int>(server::TradingCommand::Kind::CloseStock)) closing = true;
      }
    }
  }
  EXPECT_TRUE(opening);
  EXPECT_TRUE(closing);
}

TEST(ReplayRun, StopRefusesStartsQueuedBehindAStep) {
  test::RecordingFile file;
  write_stream(file.path, true);
  std::promise<void> entered, release;
  auto waiting = entered.get_future();
  const auto released = release.get_future().share();
  std::atomic<bool> hold{false};
  server::Engine::Options options;
  options.paper_journal = file.directory / "main.jsonl";
  options.journal_io.clock = [] { return std::chrono::steady_clock::time_point{}; };
  options.journal_io.sync = [&](int) { if (hold.exchange(false)) { entered.set_value(); released.wait(); } return true; };
  server::ReplayHost host({file.directory, options, false});
  ASSERT_EQ(replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}, {"paused", true}}).status, 201);
  ASSERT_TRUE(test::recording_eventually([&] { return !json::parse(host.tick())["replay"]["fast_forwarding"].get<bool>(); }));
  hold = true;
  const test::ScriptedMarket market;
  auto step = std::async(std::launch::async, [&] { return replay_call(host, "PUT", "/api/replay", {{"until", md::format_timestamp(market.time + 4 * md::kNanosPerSecond)}}); });
  const auto ready = waiting.wait_for(5min);
  EXPECT_EQ(ready, std::future_status::ready);
  if (ready != std::future_status::ready) { release.set_value(); (void)step.get(); return; }
  std::promise<server::ApiResponse> queued;
  auto queued_response = queued.get_future();
  server::ApiRequest start{"POST", "/api/replay", json{{"file", "session.oprec"}, {"paused", true}}.dump()};
  start.content_type = "application/json";
  ASSERT_TRUE(host.handle(start, [&](auto result) { queued.set_value(std::move(result)); }));
  auto stopping = std::async(std::launch::async, [&] { host.stop(); });
  const auto refused = queued_response.wait_for(5min);
  EXPECT_EQ(refused, std::future_status::ready);
  release.set_value();
  (void)step.get(); stopping.get();
  if (refused == std::future_status::ready) {
    const auto response = queued_response.get();
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(json::parse(response.body)["error"]["code"], "ENGINE_STOPPING");
  }
  EXPECT_EQ(replay_call(host, "POST", "/api/replay", {{"file", "session.oprec"}}).status, 503);
  EXPECT_EQ(json::parse(replay_call(host, "GET", "/api/replay").body)["replay"], nullptr);
}
TEST(ReplayRun, JournalAndVerificationWarningsHaveDocumentedThresholds) {
  EXPECT_TRUE(server::journal_warning(1, 1, false).empty());
  EXPECT_NE(server::journal_warning(server::kLargeJournalBytes, 1, false).find("--compact-journals"), std::string::npos);
  EXPECT_NE(server::journal_warning(1, server::kLargeJournalRecords, true).find("breaks exact"), std::string::npos);
  EXPECT_EQ(server::verification_cost(1).at("warning"), nullptr);
  EXPECT_FALSE(server::verification_cost(300ull * 1024 * 1024).at("warning").is_null());
}
}  // namespace
