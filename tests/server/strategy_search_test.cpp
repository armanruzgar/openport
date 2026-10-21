#include "openport/server/strategy_search.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <future>
#include <thread>

#include "openport/server/api.hpp"
#include "openport/server/paper.hpp"
#include "support/contract_capture.hpp"
#include "support/synthetic_chain.hpp"

namespace {
using namespace openport;
using nlohmann::json;

/// SPY (American ETF options, so shares and covered calls) on two expiries, priced
/// flat at 20% from a forward at spot, a dime wide.
struct SharesChain {
  analytics::ChainBook book;
  md::Timestamp as_of = md::new_york_to_utc(md::Date{2026, 9, 22}, 15, 0);
  SharesChain() {
    book.apply(md::UnderlyingQuote{"SPY", as_of, 499.99, 500.01, 500});
    md::InstrumentId id = 0;
    for (const char* expiry : {"261023", "261120"}) {
      for (int k = 450; k <= 550; k += 5) {
        for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) {
          char symbol[32];
          std::snprintf(symbol, sizeof symbol, "SPY%s%c%08d", expiry, type == pricing::OptionType::Call ? 'C' : 'P', k * 1000);
          const auto contract = *md::parse_osi(symbol);
          book.apply(md::ContractDefinition{id, contract});
          const double years = md::years_between(as_of, contract.expiry_time());
          const double fair = pricing::black_price(type, 500, k, years, 0.2, 1);
          book.apply(md::OptionQuote{id, as_of, std::max(0.01, fair - 0.05), fair + 0.05, 10, 10});
          ++id;
        }
      }
    }
  }
  analytics::UnderlyingMetrics metrics() const {
    analytics::AnalyticsOptions options;
    options.fallback_rate = 0;
    return analytics::analyze(book.underlyings().at("SPY"), book, as_of, options);
  }
};
analytics::UnderlyingMetrics spx() {
  test::SyntheticChain chain;
  return analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
}
server::StrategySearchRequest parse(std::map<std::string, std::string> query) { return server::parse_strategy_search(query); }

TEST(StrategySearch, QueriesNeedATargetAndABudgetAndStayInBounds) {
  EXPECT_THROW(parse({{"budget", "1000"}}), std::invalid_argument);
  EXPECT_THROW(parse({{"target", "5000"}}), std::invalid_argument);
  const auto request = parse({{"target", "5050"}, {"budget", "2500"}, {"date", "2026-10-22"}, {"rank", "pop"}, {"price", "mid"},
                              {"structures", "vertical,condor"}, {"expiry_ids", "2026-10-22PM"}, {"limit", "5"}, {"vol_points", "-2"}});
  EXPECT_EQ(request.target_price, 5050);
  EXPECT_TRUE(request.dated);
  EXPECT_EQ(request.target_date, (md::Date{2026, 10, 22}));
  EXPECT_EQ(request.structures, (std::vector<std::string>{"vertical", "condor"}));
  EXPECT_TRUE(request.mid);
  EXPECT_EQ(request.limit, 5U);
  for (const auto& bad : std::vector<std::map<std::string, std::string>>{
           {{"rank", "best"}}, {{"price", "last"}}, {{"structures", "strangle"}}, {{"structures", ""}}, {{"limit", "0"}},
           {{"limit", "101"}}, {{"max_candidates", "25001"}}, {{"date", "2026-13-01"}}, {{"date", "tomorrow"}}, {{"vol_points", "500"}},
           {{"expiry_ids", "2026-10-22PM,2026-10-23PM,2026-10-30PM,2026-11-20PM,2026-12-18PM"}}, {{"budget", "0"}}, {{"other", "1"}}}) {
    auto query = std::map<std::string, std::string>{{"target", "5000"}, {"budget", "1000"}};
    for (const auto& [key, value] : bad) query[key] = value;
    EXPECT_THROW(parse(query), std::invalid_argument) << bad.begin()->first << "=" << bad.begin()->second;
  }
}

// A vertical's numbers follow from its quotes: risk is the debit, reward the rest of
// the width, breakeven the long strike plus the debit; its odds are price_odds'.
TEST(StrategySearch, AVerticalsRiskRewardBreakevenAndOddsComeFromItsQuotes) {
  const auto metrics = spx();
  const auto result = server::search_strategies(metrics, parse({{"target", "5100"}, {"budget", "100000"},
      {"structures", "vertical"}, {"limit", "100"}, {"rank", "return_on_risk"}, {"max_candidates", "25000"}}));
  ASSERT_FALSE(result["candidates"].empty());
  EXPECT_EQ(result["target_date"], "2026-10-22");
  EXPECT_EQ(result["expiries"], json::array({"2026-10-22PM"}));
  EXPECT_FALSE(result["truncated"]);
  // Valued at expiry, so a bull call spread is its intrinsic payoff less the debit.
  for (const auto& c : result["candidates"]) {
    ASSERT_EQ(c["legs"].size(), 2U);
    EXPECT_LE(c["max_loss"].get<double>(), 100000);
    EXPECT_EQ(c["quantity"], static_cast<long long>(std::floor(100000 / c["max_loss"].get<double>() + 1e-9)));
    if (c["name"].get<std::string>().starts_with("Bull call")) {
      const double low = c["legs"][0]["strike"], high = c["legs"][1]["strike"];
      const double debit = c["legs"][0]["ask"].get<double>() - c["legs"][1]["bid"].get<double>();
      EXPECT_NEAR(c["net"].get<double>(), debit, 1e-9);
      EXPECT_NEAR(c["max_loss"].get<double>(), debit * 100, 1e-6);
      EXPECT_NEAR(c["max_profit"].get<double>(), (high - low - debit) * 100, 1e-6);
      ASSERT_EQ(c["breakevens"].size(), 1U);
      EXPECT_NEAR(c["breakevens"][0].get<double>(), low + debit, 1e-6);
      const auto variance = *server::implied_variance_until(metrics, metrics.slices[0].expiry_time);
      EXPECT_NEAR(c["pop"]["risk_neutral"].get<double>(), server::price_odds(metrics.spot, low + debit, variance).above, 1e-9);
      EXPECT_NEAR(c["pop"]["user"].get<double>(), server::price_odds(5100, low + debit, variance).above, 1e-9);
      EXPECT_GT(c["pop"]["user"].get<double>(), c["pop"]["risk_neutral"].get<double>());
    }
  }
  // Ranked best first; a target above spot favours bullish spreads.
  const auto& top = result["candidates"][0];
  EXPECT_GE(top["return_on_risk"].get<double>(), result["candidates"].back()["return_on_risk"].get<double>());
  EXPECT_TRUE(top["name"].get<std::string>().starts_with("Bull")) << top["name"];
  EXPECT_GT(top["expected"]["user"].get<double>(), 0);
}

// Fairly priced options lose their spread on average under the market's own
// distribution, so no candidate shows a risk-neutral edge beyond quadrature error.
TEST(StrategySearch, RiskNeutralExpectationsAreAboutTheSpreadPaid) {
  const SharesChain chain;
  const auto metrics = chain.metrics();
  const auto result = server::search_strategies(metrics, parse({{"target", "500"}, {"budget", "1000000"}, {"limit", "100"},
      {"date", "2026-10-23"}, {"rank", "risk_neutral"}, {"max_candidates", "25000"}}));
  ASSERT_FALSE(result["candidates"].empty());
  for (const auto& c : result["candidates"]) {
    double spread = 0;
    for (const auto& leg : c["legs"]) spread += leg["ratio"].get<double>() * (leg["ask"].get<double>() - leg["bid"].get<double>()) / 2 * 100;
    EXPECT_LT(c["expected"]["risk_neutral"].get<double>(), 0.05 * spread + 1) << c["name"];
  }
}

TEST(StrategySearch, CoversEveryStructureButNeverNakedShortCalls) {
  const SharesChain chain;
  const auto metrics = chain.metrics();
  const auto result = server::search_strategies(metrics, parse({{"target", "510"}, {"budget", "1000000"}, {"limit", "100"},
      {"date", "2026-10-23"}, {"max_candidates", "25000"}, {"rank", "pop"}}));
  EXPECT_GT(result["unbounded"].get<int>(), 0);
  EXPECT_EQ(result["expiries"], json::array({"2026-10-23PM", "2026-11-20PM"}));
  std::set<std::string> seen;
  for (const auto& c : result["candidates"]) {
    seen.insert(c["structure"]);
    EXPECT_FALSE(c["name"].get<std::string>().starts_with("Short call")) << c["name"];
  }
  // Each structure on its own: the search finds it within the budget.
  for (const auto* structure : {"vertical", "condor", "butterfly", "calendar", "single", "covered"}) {
    const auto only = server::search_strategies(metrics, parse({{"target", "510"}, {"budget", "1000000"}, {"limit", "3"},
        {"date", "2026-10-23"}, {"structures", structure}}));
    ASSERT_FALSE(only["candidates"].empty()) << structure;
    for (const auto& c : only["candidates"]) EXPECT_EQ(c["structure"], structure);
  }
  // A covered call buys a hundred shares per unit; a calendar is valued at its near expiry.
  const auto covered = server::search_strategies(metrics, parse({{"target", "510"}, {"budget", "1000000"}, {"limit", "1"},
      {"date", "2026-10-23"}, {"structures", "covered"}}));
  EXPECT_EQ(covered["candidates"][0]["shares"], 100);
  EXPECT_GT(covered["candidates"][0]["net"].get<double>(), 400);
  const auto calendar = server::search_strategies(metrics, parse({{"target", "500"}, {"budget", "1000000"}, {"limit", "1"},
      {"date", "2026-10-23"}, {"structures", "calendar"}}));
  const auto& spread = calendar["candidates"][0];
  EXPECT_EQ(spread["valued_at"], md::format_timestamp(md::new_york_to_utc({2026, 10, 23}, 16, 0)));
  EXPECT_EQ(spread["legs"][0]["expiry"], "2026-10-23PM");
  EXPECT_EQ(spread["legs"][1]["expiry"], "2026-11-20PM");
  EXPECT_GT(spread["net"].get<double>(), 0);
  EXPECT_LE(spread["max_loss"].get<double>(), spread["net"].get<double>() * 100 + 1e-6);
  EXPECT_TRUE(spread["max_profit"].is_number());
  // An index has no shares, so it offers no covered calls.
  const auto index = server::search_strategies(spx(), parse({{"target", "5000"}, {"budget", "100000"}, {"structures", "covered"}}));
  EXPECT_TRUE(index["candidates"].empty());
}

TEST(StrategySearch, BudgetsAndCandidateCapsBoundTheSearch) {
  const auto metrics = spx();
  const auto tight = server::search_strategies(metrics, parse({{"target", "5000"}, {"budget", "300"}, {"limit", "100"}}));
  EXPECT_GT(tight["over_budget"].get<int>(), 0);
  for (const auto& c : tight["candidates"]) EXPECT_LE(c["max_loss"].get<double>(), 300);
  const auto capped = server::search_strategies(metrics, parse({{"target", "5000"}, {"budget", "100000"}, {"max_candidates", "5"}}));
  EXPECT_TRUE(capped["truncated"]);
  EXPECT_LE(capped["evaluated"].get<int>(), 5);
  EXPECT_GT(capped["evaluated"].get<int>(), 0);
  // The sample spans the structures rather than stopping inside the first.
  std::set<std::string> sampled;
  const auto wide = server::search_strategies(metrics, parse({{"target", "5000"}, {"budget", "100000"}, {"max_candidates", "40"}, {"limit", "100"}}));
  for (const auto& c : wide["candidates"]) sampled.insert(c["structure"]);
  EXPECT_GE(sampled.size(), 3U);
  EXPECT_GT(wide["considered"].get<int>(), wide["evaluated"].get<int>());
  std::atomic_bool cancel{true};
  const auto cancelled = server::search_strategies(metrics, parse({{"target", "5000"}, {"budget", "100000"}}), &cancel);
  EXPECT_TRUE(cancelled["truncated"]);
  EXPECT_THROW((void)server::search_strategies(metrics, parse({{"target", "5000"}, {"budget", "1000"}, {"expiry_ids", "2027-01-15AM"}})),
               std::invalid_argument);
  EXPECT_THROW((void)server::search_strategies(metrics, parse({{"target", "5000"}, {"budget", "1000"}, {"date", "2026-12-31"}})),
               std::invalid_argument);
}

// A chain without an at-the-money term structure still has a spread of outcomes:
// the variance falls back to the expiry's ATM IV over the time to the valuation.
TEST(StrategySearch, WithoutATermStructureTheDistributionUsesTheExpirysAtmIv) {
  auto metrics = spx();
  for (auto& slice : metrics.slices)
    for (auto& row : slice.strikes) row.iv = analytics::kNaN;  // quotes keep their own IVs
  const auto date = md::trading_date(metrics.as_of);
  const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
  ASSERT_FALSE(server::implied_variance_until(metrics, close).has_value());
  const auto query = std::map<std::string, std::string>{{"target", "5000"}, {"budget", "100000"}, {"structures", "vertical"},
                                                        {"limit", "5"}, {"date", md::format_date(date)}};
  const auto result = server::search_strategies(metrics, parse(query));
  EXPECT_TRUE(result["variance_fallback"]);
  ASSERT_FALSE(result["candidates"].empty());
  const double atm = metrics.slices.front().atm_iv;
  ASSERT_GT(atm, 0);
  const double sigma = atm * std::sqrt(md::years_between(metrics.as_of, close));
  for (const auto& candidate : result["candidates"]) {
    EXPECT_NEAR(candidate["sigma"]["risk_neutral"].get<double>(), sigma, 1e-9);
    EXPECT_GT(candidate["pop"]["risk_neutral"].get<double>(), 0);
    EXPECT_LT(candidate["pop"]["risk_neutral"].get<double>(), 1);
  }
  // With the smile in place the term structure answers, and nothing falls back.
  EXPECT_FALSE(server::search_strategies(spx(), parse(query))["variance_fallback"]);
}

TEST(StrategySearch, ThePoolRefusesWorkBeyondItsQueue) {
  server::StrategySearchPool pool(1, 1);
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic_int done{0};
  ASSERT_TRUE(pool.submit([gate, &done] { gate.wait(); ++done; }));
  // The worker holds the first job and one more may wait; the next is refused.
  ASSERT_TRUE(pool.submit([&done] { ++done; }));
  EXPECT_FALSE(pool.submit([&done] { ++done; }));
  release.set_value();
  while (done.load() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(done.load(), 2);
}

class SearchSource final : public server::MetricsSource {
 public:
  SearchSource() : metrics_(std::make_shared<const analytics::UnderlyingMetrics>(spx())) {}
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override {
    return symbol == "SPX" ? metrics_ : nullptr;
  }
  server::EngineStatus status() const override { return {}; }
 private:
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics_;
};

// The network path completes from the search pool, not the calling thread.
TEST(StrategySearch, NetworkRequestsCompleteFromThePool) {
  SearchSource source;
  const std::string path = "/api/underlyings/SPX/optimizer?target=5050&budget=2000&limit=3&structures=vertical,butterfly";
  std::promise<std::pair<server::ApiResponse, std::thread::id>> promise;
  auto future = promise.get_future();
  server::handle_api_async({"GET", path}, source, [&](server::ApiResponse response) {
    promise.set_value({std::move(response), std::this_thread::get_id()});
  });
  ASSERT_EQ(future.wait_for(std::chrono::minutes(2)), std::future_status::ready);
  const auto [response, thread] = future.get();
  EXPECT_NE(thread, std::this_thread::get_id());
  ASSERT_EQ(response.status, 200) << response.body;
  const auto body = json::parse(response.body);
  EXPECT_EQ(body["candidates"].size(), 3U);
  EXPECT_EQ(body, json::parse(server::handle_api({"GET", path}, source).body));
  test::capture_contract("analytics", "GET", path, response);
  // Invalid queries and unknown underlyings answer at once.
  std::promise<server::ApiResponse> invalid;
  server::handle_api_async({"GET", "/api/underlyings/SPX/optimizer?target=5050"}, source,
                           [&](server::ApiResponse value) { invalid.set_value(std::move(value)); });
  const auto rejected = invalid.get_future().get();
  EXPECT_EQ(rejected.status, 400);
  test::capture_contract("analytics", "GET", "/api/underlyings/SPX/optimizer?target=5050", rejected);
  EXPECT_EQ(server::handle_api({"GET", "/api/underlyings/QQQ/optimizer?target=1&budget=1"}, source).status, 404);
}
}  // namespace
