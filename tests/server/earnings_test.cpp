#include "openport/server/earnings.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <nlohmann/json.hpp>

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
    EXPECT_EQ(*moves[0].priced(), *moves[0].implied > 0.008 ? "over" : "under");

    const auto result = store.import_csv(
        "# past quarters, from my notes\n"
        "symbol,date,session,implied_move,realized_move,label\r\n"
        "SPX,2026-09-22,after_close,4.0,-1.0,dup\n"
        "AAPL,2026-07-30,after_close,4.1%,-2.3,\"Q3, FY26\"\n"  // quoted, as analysis imports read them
        "AAPL,2026-05-01,before_open,3.5,5.25,\n");
    EXPECT_EQ(result.added, 2u);
    ASSERT_EQ(result.skipped.size(), 1u);  // recorded here first
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move\nAAPL,2026-13-01,after_close,4,1\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date\nAAPL,2026-07-30\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move,label\nAAPL,2026-07-30,after_close,4,1,\"Q3\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move\nAAPL,2026-07-30,midday,4,1\n"), std::invalid_argument);
    EXPECT_THROW(store.import_csv("symbol,date,session,implied_move,realized_move\nAAPL,2026-07-30,after_close,0,1\n"), std::invalid_argument);
  }
  server::EarningsStore store(file);
  EXPECT_TRUE(store.error().empty()) << store.error();
  ASSERT_EQ(store.moves().size(), 3u);
  const auto aapl = store.moves("AAPL");
  ASSERT_EQ(aapl.size(), 2u);
  EXPECT_EQ(aapl[0].date, (md::Date{2026, 7, 30}));  // newest first
  EXPECT_EQ(aapl[0].source, "imported");
  EXPECT_EQ(aapl[0].label, "Q3, FY26");
  EXPECT_DOUBLE_EQ(*aapl[0].implied, 0.041);
  EXPECT_EQ(*aapl[0].priced(), "over");
  EXPECT_EQ(*aapl[1].priced(), "under");

  auto response = server::earnings_request(server::ApiRequest("GET", "/api/earnings?symbol=AAPL"), store);
  ASSERT_TRUE(response);
  auto body = json::parse(response->body);
  EXPECT_EQ(body["summary"]["events"], 2);
  EXPECT_EQ(body["summary"]["over"], 1);
  EXPECT_EQ(body["summary"]["imported"], 2);
  EXPECT_DOUBLE_EQ(body["summary"]["over_rate"].get<double>(), 0.5);
  EXPECT_EQ(body["moves"][0]["source"], "imported");
  EXPECT_EQ(body["moves"][0]["priced"], "over");

  response = server::earnings_request(server::ApiRequest("POST", "/api/earnings/import",
      json{{"csv", "symbol,date,session,implied_move,realized_move\nMSFT,2026-07-29,after_close,5,3\n"}}.dump()), store);
  ASSERT_TRUE(response);
  EXPECT_EQ(json::parse(response->body)["added"], 1);
  EXPECT_EQ(server::earnings_request(server::ApiRequest("POST", "/api/earnings/import", "{\"csv\":\"bad\"}"), store)->status, 400);
  EXPECT_EQ(server::earnings_request(server::ApiRequest("GET", "/api/earnings?from=1"), store)->status, 400);
  response = server::earnings_request(server::ApiRequest("DELETE", "/api/earnings/imported"), store);
  EXPECT_EQ(json::parse(response->body)["removed"], 3);
  EXPECT_EQ(store.moves().size(), 1u);
  EXPECT_FALSE(server::earnings_request(server::ApiRequest("GET", "/api/calendar/events"), store));
  std::filesystem::remove(file);
}

}  // namespace
