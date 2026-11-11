#include "openport/server/earnings.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

#include "support/synthetic_chain.hpp"

namespace {

using namespace openport;
using nlohmann::json;

// The synthetic SPX chain stands in for a stock with earnings; its prices are
// generated test inputs, and so are the closes and imported moves below.
analytics::UnderlyingMetrics metrics_at(md::Timestamp as_of) {
  test::SyntheticChain chain;
  auto m = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  m.as_of = as_of;
  return m;
}

trading::PlanEvent earnings(const char* date, const char* session) {
  return {"earnings", date, "SPX", session, "Q3"};
}

TEST(Earnings, SessionsAroundTheNews) {
  // Tuesday 22 September 2026.
  EXPECT_EQ(server::earnings_last_session({2026, 9, 22}, "after_close"), (md::Date{2026, 9, 22}));
  EXPECT_EQ(server::earnings_reaction_session({2026, 9, 22}, "after_close"), (md::Date{2026, 9, 23}));
  EXPECT_EQ(server::earnings_last_session({2026, 9, 22}, "before_open"), (md::Date{2026, 9, 21}));
  EXPECT_EQ(server::earnings_reaction_session({2026, 9, 22}, "before_open"), (md::Date{2026, 9, 22}));
  // Friday's after-close news trades on Monday.
  EXPECT_EQ(server::earnings_reaction_session({2026, 9, 25}, "after_close"), (md::Date{2026, 9, 28}));
}

TEST(Earnings, ImpliedMoveIsTheFrontStraddleOverSpotInTheLastSessionBeforeTheNews) {
  const auto m = metrics_at(md::new_york_to_utc({2026, 9, 22}, 15, 0));
  const auto move = server::implied_move(earnings("2026-09-22", "after_close"), m);
  ASSERT_TRUE(move);
  EXPECT_EQ(move->expiry, "2026-10-22PM");
  EXPECT_EQ(move->source, "recorded");
  // The strike nearest the forward with both sides quoted.
  const auto& slice = m.slices.front();
  double nearest = 0;
  for (const auto& s : slice.strikes)
    if (!nearest || std::abs(s.strike - slice.forward.forward) < std::abs(nearest - slice.forward.forward)) nearest = s.strike;
  EXPECT_EQ(move->strike, nearest);
  EXPECT_NEAR(*move->implied, move->straddle / test::SyntheticChain::kSpot, 1e-12);
  EXPECT_GT(*move->implied, 0.02);
  EXPECT_LT(*move->implied, 0.08);
  EXPECT_EQ(move->implied_at, m.as_of);

  // After the news began, a day too early, another symbol or kind: nothing.
  EXPECT_FALSE(server::implied_move(earnings("2026-09-22", "before_open"), m));
  EXPECT_FALSE(server::implied_move(earnings("2026-09-24", "before_open"), m));
  EXPECT_TRUE(server::implied_move(earnings("2026-09-23", "before_open"), m));
  EXPECT_FALSE(server::implied_move({"earnings", "2026-09-22", "SPY", "after_close", ""}, m));
  EXPECT_FALSE(server::implied_move({"ex_dividend", "2026-09-22", "SPX", "", ""}, m));
  // An expiry that settles before the reaction close does not price the event.
  EXPECT_FALSE(server::implied_move(earnings("2026-10-22", "after_close"), metrics_at(md::new_york_to_utc({2026, 10, 22}, 11, 0))));
}

TEST(Earnings, RecordsSettlesImportsAndSurvivesARestart) {
  const auto file = std::filesystem::temp_directory_path() / ("openport-earnings-" + std::to_string(::getpid()) + ".json");
  std::filesystem::remove(file);
  const std::vector<trading::PlanEvent> calendar{earnings("2026-09-22", "after_close"), {"news", "2026-09-22T18:00:00Z", "", "", "FOMC"}};
  {
    server::EarningsStore store(file);
    store.capture(calendar, metrics_at(md::new_york_to_utc({2026, 9, 22}, 14, 0)));
    store.capture(calendar, metrics_at(md::new_york_to_utc({2026, 9, 22}, 15, 59)));
    store.capture(calendar, metrics_at(md::new_york_to_utc({2026, 9, 22}, 16, 5)));  // after the news: ignored
    auto moves = store.moves();
    ASSERT_EQ(moves.size(), 1u);
    EXPECT_EQ(moves[0].implied_at, md::new_york_to_utc({2026, 9, 22}, 15, 59));
    EXPECT_FALSE(moves[0].realized);

    server::CandleStore candles;
    candles.merge_days("SPX", {{md::new_york_to_utc({2026, 9, 22}, 9, 30), 4990, 5010, 4980, 5000},
                               {md::new_york_to_utc({2026, 9, 23}, 9, 30), 5000, 5030, 4950, 4960}});
    store.settle(candles, md::new_york_to_utc({2026, 9, 23}, 15, 0));  // the reaction session still trades
    EXPECT_FALSE(store.moves()[0].realized);
    store.settle(candles, md::new_york_to_utc({2026, 9, 23}, 16, 30));
    moves = store.moves();
    ASSERT_TRUE(moves[0].realized);
    EXPECT_DOUBLE_EQ(*moves[0].realized, 4960.0 / 5000.0 - 1);
    // The straddle priced a move of more than 2%, as the test above finds; the reaction was a 0.8% drop.
    EXPECT_EQ(*moves[0].priced(), "over");

    const auto result = store.import_csv(
        "# past quarters, from my notes\n"
        "symbol,date,session,implied_move,realized_move,label\r\n"
        "SPX,2026-09-22,after_close,4.0,-1.0,dup\n"
        "AAPL,2026-07-30,after_close,4.1%,-2.3,\"Q3, FY26\"\n"  // quoted, as analysis imports read them
        "AAPL,2026-05-01,before_open,3.5,5.25,\n"
        "AAPL,2026-01-29,after_close,3,-5\n");  // a drop larger than the move priced
    EXPECT_EQ(result.added, 3u);
    ASSERT_EQ(result.skipped.size(), 1u);  // recorded here first
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move\nAAPL,2026-13-01,after_close,4,1\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date\nAAPL,2026-07-30\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move,label\nAAPL,2026-07-30,after_close,4,1,\"Q3\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move\nAAPL,2026-07-30,midday,4,1\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move\nAAPL,2026-07-30,after_close,0,1\n"), std::invalid_argument);
  }
  server::EarningsStore store(file);
  EXPECT_TRUE(store.error().empty()) << store.error();
  ASSERT_EQ(store.moves().size(), 4u);
  const auto aapl = store.moves("AAPL");
  ASSERT_EQ(aapl.size(), 3u);
  EXPECT_EQ(aapl[0].date, (md::Date{2026, 7, 30}));  // newest first
  EXPECT_EQ(aapl[0].source, "imported");
  EXPECT_EQ(aapl[0].label, "Q3, FY26");
  EXPECT_DOUBLE_EQ(*aapl[0].implied, 0.041);
  EXPECT_EQ(*aapl[0].priced(), "over");
  EXPECT_EQ(*aapl[1].priced(), "under");
  // A move is compared by its size: a 5% drop came in above the 3% priced.
  EXPECT_EQ(aapl[2].date, (md::Date{2026, 1, 29}));
  EXPECT_EQ(*aapl[2].priced(), "under");

  auto response = server::earnings_request(server::ApiRequest("GET", "/api/earnings?symbol=AAPL"), store);
  ASSERT_TRUE(response);
  auto body = json::parse(response->body);
  EXPECT_EQ(body["summary"]["events"], 3);
  EXPECT_EQ(body["summary"]["over"], 1);
  EXPECT_EQ(body["summary"]["under"], 2);
  EXPECT_EQ(body["summary"]["imported"], 3);
  EXPECT_DOUBLE_EQ(body["summary"]["over_rate"].get<double>(), 1.0 / 3);
  // The means of 4.1%, 3.5% and 3% priced, and of the sizes of -2.3%, 5.25% and -5%.
  EXPECT_NEAR(body["summary"]["mean_implied"].get<double>(), 0.106 / 3, 1e-12);
  EXPECT_NEAR(body["summary"]["mean_realized"].get<double>(), 0.1255 / 3, 1e-12);
  EXPECT_EQ(body["moves"][0]["source"], "imported");
  EXPECT_EQ(body["moves"][0]["priced"], "over");

  response = server::earnings_request(server::ApiRequest("POST", "/api/earnings/import",
      json{{"csv", "symbol,date,session,implied_move,realized_move\nMSFT,2026-07-29,after_close,5,3\n"}}.dump()), store);
  ASSERT_TRUE(response);
  EXPECT_EQ(json::parse(response->body)["added"], 1);
  // An import gives no session dates: unknown, never 1970-01-01 (B-99).
  response = server::earnings_request(server::ApiRequest("GET", "/api/earnings?symbol=MSFT"), store);
  ASSERT_TRUE(response);
  body = json::parse(response->body);
  ASSERT_EQ(body["moves"].size(), 1u);
  EXPECT_TRUE(body["moves"][0]["before"].is_null()) << body["moves"][0];
  EXPECT_TRUE(body["moves"][0]["after"].is_null()) << body["moves"][0];
  EXPECT_FALSE(store.moves("MSFT")[0].before);
  EXPECT_EQ(server::earnings_request(server::ApiRequest("POST", "/api/earnings/import", "{\"csv\":\"bad\"}"), store)->status, 400);
  EXPECT_EQ(server::earnings_request(server::ApiRequest("GET", "/api/earnings?from=1"), store)->status, 400);
  response = server::earnings_request(server::ApiRequest("DELETE", "/api/earnings/imported"), store);
  EXPECT_EQ(json::parse(response->body)["removed"], 4);
  EXPECT_EQ(store.moves().size(), 1u);
  EXPECT_FALSE(server::earnings_request(server::ApiRequest("GET", "/api/calendar/events"), store));
  // A method a route does not take is 405 with the methods it does take.
  const auto posted = server::earnings_request(server::ApiRequest("POST", "/api/earnings"), store);
  ASSERT_TRUE(posted);
  EXPECT_EQ(posted->status, 405);
  EXPECT_EQ(posted->allow, "GET, HEAD");
  const auto fetched = server::earnings_request(server::ApiRequest("GET", "/api/earnings/import"), store);
  ASSERT_TRUE(fetched);
  EXPECT_EQ(fetched->allow, "POST");
  const auto removed = server::earnings_request(server::ApiRequest("POST", "/api/earnings/imported"), store);
  ASSERT_TRUE(removed);
  EXPECT_EQ(removed->allow, "DELETE");
  std::filesystem::remove(file);
}

}  // namespace

namespace {
// One row this build could not read (a hand edit, another version's file) stopped the
// load: the rows after it were dropped, the next save rewrote the file without them,
// and the load error stayed after the rewrite succeeded.
TEST(Earnings, AnUnreadableRowCostsOnlyItselfAndTheFileIsKeptBeforeItIsRewritten) {
  const auto directory = std::filesystem::temp_directory_path() / ("openport-earnings-rows-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto file = directory / "earnings.json";
  const auto row = [](const char* symbol) {
    return json{{"symbol", symbol}, {"date", "2026-07-30"}, {"session", "after_close"}, {"source", "imported"}};
  };
  auto unreadable = row("MSFT");
  unreadable["before"] = "2026-1-28";
  {
    std::ofstream out(file);
    out << json{{"version", 1}, {"moves", json::array({row("AAPL"), unreadable, row("NVDA"), row("TSLA")})}}.dump();
  }
  {
    server::EarningsStore store(file);
    EXPECT_EQ(store.moves().size(), 3u);
    EXPECT_NE(store.error().find("skipped 1 unreadable move (invalid before)"), std::string::npos) << store.error();
    EXPECT_EQ(store.import_csv("symbol,date,session,implied_move,realized_move\nAMZN,2026-07-30,after_close,4,1\n").added, 1u);
    EXPECT_NE(store.error().find("kept as it was in earnings.json.unread-"), std::string::npos) << store.error();
  }
  std::vector<std::filesystem::path> kept;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().filename().string().starts_with("earnings.json.unread-")) kept.push_back(entry.path());
  ASSERT_EQ(kept.size(), 1u);
  std::ifstream copy(kept[0]);
  EXPECT_EQ(json::parse(copy)["moves"].size(), 4u);  // MSFT's row is still there
  {
    server::EarningsStore reopened(file);
    EXPECT_EQ(reopened.moves().size(), 4u);
    EXPECT_TRUE(reopened.error().empty()) << reopened.error();
  }
  // A file that is not JSON at all is kept the same way, and a later save clears the error.
  { std::ofstream out(file); out << "{\"version\": 1, \"moves\": [{\"symbol\""; }
  {
    server::EarningsStore store(file);
    EXPECT_TRUE(store.moves().empty());
    EXPECT_FALSE(store.error().empty());
    EXPECT_EQ(store.import_csv("symbol,date,session,implied_move,realized_move\nAMZN,2026-07-30,after_close,4,1\n").added, 1u);
  }
  std::size_t copies = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory))
    if (entry.path().filename().string().starts_with("earnings.json.unread-")) ++copies;
  EXPECT_EQ(copies, 2u);
  server::EarningsStore reopened(file);
  EXPECT_EQ(reopened.moves().size(), 1u);
  EXPECT_TRUE(reopened.error().empty()) << reopened.error();
  std::filesystem::remove_all(directory);
}

TEST(Earnings, ALongLabelIsCutOnACharacterBoundaryAndOnlyUtf8IsImported) {
  const auto file = std::filesystem::temp_directory_path() / ("openport-earnings-labels-" + std::to_string(::getpid()) + ".json");
  std::filesystem::remove(file);
  const std::string header = "symbol,date,session,implied_move,realized_move,label\n";
  {
    server::EarningsStore store(file);
    // An accented letter straddles byte 64.
    EXPECT_EQ(store.import_csv(header + "AAPL,2026-07-30,after_close,4.1,-2.3," + std::string(63, 'x') + "\xc3\xa9 and more\n").added, 1u);
    EXPECT_THROW(store.import_csv(header + "NVDA,2026-08-27,after_close,6,3,Q2 \xe9\n"), std::invalid_argument);
    ASSERT_EQ(store.moves().size(), 1u);
    EXPECT_EQ(store.moves()[0].label, std::string(63, 'x'));
    EXPECT_TRUE(store.error().empty()) << store.error();
    const auto response = server::earnings_request({"GET", "/api/earnings"}, store);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200) << response->body;
  }
  server::EarningsStore reopened(file);
  ASSERT_EQ(reopened.moves().size(), 1u);
  EXPECT_EQ(reopened.moves()[0].label, std::string(63, 'x'));
  std::filesystem::remove(file);
}
}  // namespace
