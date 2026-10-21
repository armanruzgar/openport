#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

#include "support/scripted_market.hpp"

namespace openport::trading {
namespace {
using test::ScriptedMarket;

void expect_invalid(TradingSession& s, std::string note, std::vector<std::string> tags, Timestamp time) {
  const auto version = s.snapshot()->account_version;
  try {
    (void)s.annotate(1, std::move(note), std::move(tags), time);
    ADD_FAILURE() << "expected INVALID_NOTE";
  } catch (const TradingError& error) { EXPECT_EQ(error.code(), Reason::INVALID_NOTE) << error.what(); }
  EXPECT_EQ(s.snapshot()->account_version, version);  // nothing recorded
}

TEST(TradingNotes, TradesTakeANoteAndTagsThatSurviveRecovery) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-notes-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = (directory / "journal.jsonl").string();
  ScriptedMarket f;
  std::string expected;
  {
    TradingSession s({}, f.time, FileJournal::create(path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
    ASSERT_TRUE(s.submit(f.market("close", 1, Side::Sell), f.time).decision.ok());
    const auto trade = s.snapshot()->recent_fills.front().id;
    ASSERT_TRUE(s.annotate(trade, "  Faded the open; stop was too tight.\n", {" Breakout ", "0DTE", "breakout"}, f.time).decision.ok());
    const auto& note = s.snapshot()->annotations.at(std::to_string(trade));
    EXPECT_EQ(note.note, "Faded the open; stop was too tight.");
    EXPECT_EQ(note.tags, (std::vector<std::string>{"breakout", "0dte"}));
    EXPECT_EQ(note.time, f.time);
    // The closing fill does not open a trade.
    EXPECT_EQ(s.annotate(s.snapshot()->recent_fills.back().id, "no", {}, f.time).decision.code, Reason::UNKNOWN_TRADE);
    EXPECT_EQ(s.annotate(99, "no", {}, f.time).decision.code, Reason::UNKNOWN_TRADE);
    // Notes are kept whatever the account's state.
    s.trip_kill("review", f.time);
    ASSERT_TRUE(s.annotate(trade, "Reviewed.", {"breakout"}, f.time).decision.ok());
    expected = s.snapshot_json();
  }
  auto s = TradingSession::recover(FileJournal::read(path), FileJournal::resume(path));
  EXPECT_EQ(s.snapshot_json(), expected);
  EXPECT_EQ(s.snapshot()->annotations.at("1").note, "Reviewed.");
  // An empty note without tags clears it.
  ASSERT_TRUE(s.annotate(1, " ", {}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->annotations.empty());
  std::filesystem::remove_all(directory);
}

TEST(TradingNotes, NotesAndTagsMustBeShortPlainText) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
  ASSERT_TRUE(s.annotate(1, std::string(2000, 'x'), {std::string(32, 't')}, f.time).decision.ok());
  expect_invalid(s, std::string(2001, 'x'), {}, f.time);
  expect_invalid(s, "bell\a", {}, f.time);
  expect_invalid(s, "bad \xff byte", {}, f.time);
  expect_invalid(s, "", {std::string(33, 't')}, f.time);
  expect_invalid(s, "", {"a,b"}, f.time);
  expect_invalid(s, "", {" "}, f.time);
  expect_invalid(s, "", {"line\nbreak"}, f.time);
  expect_invalid(s, "", {"1", "2", "3", "4", "5", "6", "7", "8", "9"}, f.time);
  // Tags repeated after lowercasing count once.
  EXPECT_TRUE(s.annotate(1, "", {"a", "A", "b", "c", "d", "e", "f", "g", "h"}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.at("1").tags.size(), 8U);
}


TEST(TradingNotes, OrderMetadataUsesTradeValidationAndClosingFillsDoNotAnnotate) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  auto r = f.market("invalid");
  r.tags = {std::string(33, 'x')};
  EXPECT_EQ(s.submit(r, f.time).decision.code, Reason::INVALID_NOTE);
  r.client_order_id = "open"; r.tags = {" PLAN ", "plan"}; r.note = " first ";
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  r = f.market("close", 1, Side::Sell); r.tags = {"close"}; r.note = "ignored";
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.size(), 1U);
  EXPECT_EQ(s.snapshot()->annotations.at("1").tags, (std::vector<std::string>{"plan"}));
  EXPECT_EQ(s.snapshot()->annotations.at("1").note, "first");
}

TEST(TradingNotes, ACrlfLineBreakIsANewlineAndAControlCharacterIsNamedAsOne) {
  ScriptedMarket f;
  TradingSession s({}, f.time);
  f.seed(s);
  auto r = f.market("open");
  r.note = "setup A\r\nrisk 1R";
  ASSERT_TRUE(s.submit(r, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.at("1").note, "setup A\nrisk 1R");
  ASSERT_TRUE(s.annotate(1, "Entry plan\r\nexit at 6.00\rdone\r\n", {}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.at("1").note, "Entry plan\nexit at 6.00\ndone");
  ASSERT_TRUE(s.annotate_day({2026, 9, 22}, "Plan\r\nwait for 10:00", "Review\r\n", f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->day_notes.at("2026-09-22").plan, "Plan\nwait for 10:00");
  EXPECT_EQ(s.snapshot()->day_notes.at("2026-09-22").review, "Review");
  // 1,000 CRLF breaks are 1,000 bytes of newlines.
  std::string lines;
  for (int i = 0; i < 1000; ++i) lines += "x\r\n";
  ASSERT_TRUE(s.annotate(1, lines, {}, f.time).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.at("1").note.size(), 1999U);
  try {
    (void)s.annotate(1, "bell\a", {}, f.time);
    ADD_FAILURE() << "expected INVALID_NOTE";
  } catch (const TradingError& error) {
    EXPECT_EQ(error.code(), Reason::INVALID_NOTE);
    EXPECT_NE(std::string(error.what()).find("control characters"), std::string::npos) << error.what();
  }
  expect_invalid(s, "", {"line\rbreak"}, f.time);
}

}  // namespace
}  // namespace openport::trading

namespace openport::trading {
TEST(TradingNotes, FixedMistakesAreSeparateValidatedAndRecovered) {
  test::ScriptedMarket f;
  const auto path = (std::filesystem::temp_directory_path() / ("openport-mistakes-" + std::to_string(::getpid()) + ".jsonl")).string();
  std::filesystem::remove(path);
  {
    TradingSession s({}, f.time, FileJournal::create(path));
    f.seed(s);
    ASSERT_TRUE(s.submit(f.market("open"), f.time).decision.ok());
    ASSERT_TRUE(s.annotate(1, "review", {"chased"}, f.time, {"chased", "A+ setup", "chased"}).decision.ok());
    const auto& a = s.snapshot()->annotations.at("1");
    EXPECT_EQ(a.tags, std::vector<std::string>{"chased"});
    EXPECT_EQ(a.mistakes, (std::vector<std::string>{"chased", "A+ setup"}));
    const auto version = s.snapshot()->account_version;
    EXPECT_THROW((void)s.annotate(1, "", {}, f.time, {"unknown"}), TradingError);
    EXPECT_EQ(s.snapshot()->account_version, version);
  }
  auto s = TradingSession::recover(FileJournal::read(path));
  EXPECT_EQ(s.snapshot()->annotations.at("1").mistakes.size(), 2U);
  ASSERT_TRUE(s.annotate(1, "", {}, f.time, {"revenge"}).decision.ok());
  EXPECT_EQ(s.snapshot()->annotations.at("1").mistakes, std::vector<std::string>{"revenge"});
  ASSERT_TRUE(s.annotate(1, "", {}, f.time).decision.ok());
  EXPECT_TRUE(s.snapshot()->annotations.empty());
  std::filesystem::remove(path);
}
}
