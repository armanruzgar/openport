#include "support/contract_capture.hpp"
#include "openport/server/api.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <atomic>
#include <future>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>

#include "openport/analytics/chain_analytics.hpp"
#include "openport/net/http.hpp"
#include "openport/server/web_server.hpp"
#include "support/american_chain.hpp"
#include "support/synthetic_chain.hpp"

namespace {

using namespace openport;
using nlohmann::json;

/// Serves analytics computed once from the synthetic chain.
class StubSource final : public server::MetricsSource {
 public:
  explicit StubSource(analytics::UnderlyingMetrics metrics)
      : metrics_(std::make_shared<const analytics::UnderlyingMetrics>(std::move(metrics))) {}

  StubSource() {
    test::SyntheticChain chain;
    metrics_ = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of));
    status_.provider = "cboe";
    status_.capabilities.delay = std::chrono::seconds(900);
    status_.feed_state = md::FeedState::Delayed;
    status_.feed_message = "cboe SPX: 82 options";
  }

  std::vector<std::string> symbols() const override { return {metrics_->symbol}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(
      const std::string& symbol) const override {
    return symbol == metrics_->symbol ? metrics_ : nullptr;
  }
  server::EngineStatus status() const override { return status_; }

 private:
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics_;
  server::EngineStatus status_;
};

json get(const server::MetricsSource& source, const std::string& target, int expected_status = 200) {
  const server::ApiResponse response = server::handle_api({"GET", target}, source);
  EXPECT_EQ(response.status, expected_status) << target << " -> " << response.body;
  return json::parse(response.body);
}

/// Price history only, as before the first analysis has run.
class CandleSource final : public server::MetricsSource {
 public:
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override {
    return nullptr;
  }
  server::EngineStatus status() const override { return {}; }
  const server::CandleStore* candles() const override { return &store; }
  server::CandleStore store;
};

TEST(Api, CandlesServeStoredBarsBeforeTheFirstAnalysis) {
  CandleSource source;
  const auto open = md::new_york_to_utc({2026, 9, 22}, 9, 30);
  source.store.merge_minutes("SPX", {{open, 7770.81, 7773.58, 7769.6, 7772.65},
                                     {open + md::kNanosPerMinute, 7772.94, 7779.88, 7772.9, 7779.15}});
  auto body = get(source, "/api/underlyings/SPX/candles?interval=1m");
  EXPECT_EQ(body["symbol"], "SPX");
  EXPECT_EQ(body["interval"], "1m");
  ASSERT_EQ(body["bars"].size(), 2u);
  EXPECT_EQ(body["bars"][0], (json{{"t", open / md::kNanosPerSecond}, {"o", 7770.81}, {"h", 7773.58},
                                   {"l", 7769.6}, {"c", 7772.65}}));
  body = get(source, "/api/underlyings/SPX/candles?limit=1");
  EXPECT_EQ(body["interval"], "5m");
  ASSERT_EQ(body["bars"].size(), 1u);
  EXPECT_EQ(body["bars"][0]["o"], 7770.81);
  EXPECT_EQ(body["bars"][0]["h"], 7779.88);
  EXPECT_EQ(body["bars"][0]["c"], 7779.15);
  get(source, "/api/underlyings/SPX/candles?interval=2m", 400);
  get(source, "/api/underlyings/SPX/candles?limit=0", 400);
  get(source, "/api/underlyings/SPX/candles?limit=5001", 400);
  get(source, "/api/underlyings/QQQ/candles", 404);
  get(source, "/api/underlyings/SPX/summary", 404);
  // A source without a store has no bars.
  StubSource plain;
  EXPECT_TRUE(get(plain, "/api/underlyings/SPX/candles?interval=1d")["bars"].empty());
}

TEST(Api, StatusDescribesProviderFeedAndUnderlyings) {
  StubSource source;
  const json status = get(source, "/api/status");
  EXPECT_EQ(status["provider"]["name"], "cboe");
  EXPECT_EQ(status["provider"]["delay_seconds"], 900);
  EXPECT_EQ(status["feed"]["state"], "delayed");
  ASSERT_EQ(status["underlyings"].size(), 1u);
  EXPECT_EQ(status["underlyings"][0]["symbol"], "SPX");
  EXPECT_DOUBLE_EQ(status["underlyings"][0]["spot"].get<double>(), 5000.0);
}

TEST(Api, StatusIncludesRunningVersionBeforeMarketDataArrives) {
  CandleSource source;
  EXPECT_EQ(get(source, "/api/status")["version"], OPENPORT_VERSION);
  const auto build = get(source, "/api/status")["build"];
  ASSERT_EQ(build.size(), 3u);
  for (const auto* key : {"revision", "compiler", "platform"}) {
    ASSERT_TRUE(build[key].is_string());
    EXPECT_FALSE(build[key].get<std::string>().empty());
  }
}

TEST(Api, TradableUnderlyingFlagUsesContractsAndIsIncludedInTicks) {
  test::SyntheticChain chain;
  auto m = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  StubSource european(m);
  EXPECT_TRUE(get(european, "/api/status")["underlyings"][0]["has_tradable_contracts"].get<bool>());
  EXPECT_TRUE(json::parse(server::tick_message(european))["underlyings"][0]["has_tradable_contracts"].get<bool>());
  for (auto& slice : m.slices) {
    slice.style = pricing::ExerciseStyle::American;
    for (auto& row : slice.strikes) {
      row.call.contract.style = pricing::ExerciseStyle::American;
      row.put.contract.style = pricing::ExerciseStyle::American;
    }
  }
  StubSource american(m);
  EXPECT_FALSE(get(american, "/api/status")["underlyings"][0]["has_tradable_contracts"].get<bool>());
  EXPECT_FALSE(json::parse(server::tick_message(american))["underlyings"][0]["has_tradable_contracts"].get<bool>());
  m.slices.clear();
  StubSource empty(m);
  EXPECT_FALSE(get(empty, "/api/status")["underlyings"][0]["has_tradable_contracts"].get<bool>());
}

TEST(Api, SummaryListsExpiriesWithSettlementInTheirIds) {
  StubSource source;
  const json summary = get(source, "/api/underlyings/SPX/summary");
  ASSERT_EQ(summary["expiries"].size(), 1u);
  const json& expiry = summary["expiries"][0];
  EXPECT_EQ(expiry["id"], "2026-10-22PM");
  EXPECT_EQ(expiry["settlement"], "PM");
  EXPECT_EQ(expiry["strikes"], 41);
  EXPECT_EQ(expiry["dividends"], json::array());
  EXPECT_NEAR(expiry["rate"].get<double>(), test::SyntheticChain::kRate, 1e-3);
  EXPECT_TRUE(summary["exposure"]["gamma_flip"].is_number());
}

TEST(Api, SummaryAndChainReportTheCashAmountsUsedWithoutRounding) {
  analytics::ChainBook book;
  md::InstrumentId id = 0;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  test::add_american_expiry(book, as_of, {2027, 9, 22}, id, 100, 90, 2.5, 9, 101);
  analytics::AnalyticsOptions options;
  options.dividends = {{{2027, 2, 2}, .5}, {{2026, 11, 2}, 1.234567}};
  const auto& underlying = book.underlyings().at("SPY");
  StubSource source(analytics::analyze(underlying, book, as_of, options));
  const auto summary = get(source, "/api/underlyings/SPY/summary");
  const auto expiry = summary["expiries"][0];
  const auto chain = get(source, "/api/underlyings/SPY/chain?expiry=" + expiry["id"].get<std::string>());
  const json expected = json::array({{{"ex_date", "2026-11-02"}, {"amount", 1.234567}},
                                      {{"ex_date", "2027-02-02"}, {"amount", .5}}});
  EXPECT_EQ(expiry["dividends"], expected);
  EXPECT_EQ(chain["expiry"]["dividends"], expected);
  options.deamericanize = false;
  StubSource raw(analytics::analyze(underlying, book, as_of, options));
  EXPECT_EQ(get(raw, "/api/underlyings/SPY/summary")["expiries"][0]["dividends"], json::array());
}

TEST(Api, ExpiriesReportContractLastTradeAndFiveMinuteAutoClose) {
  for (const auto date : {md::Date{2026, 9, 22}, md::Date{2026, 11, 27}}) {
    for (const auto root : {"SPXW", "SPX", "SPY", "QQQ", "IWM", "DIA"}) {
      SCOPED_TRACE(root);
      auto contract = *md::parse_osi(std::string(root) + "260922C05000000");
      contract.expiry = date;
      analytics::UnderlyingMetrics metrics;
      metrics.symbol = contract.underlying;
      analytics::SliceMetrics slice;
      // As the chain book builds them: only OEX and XEO slices name their root.
      slice.expiry = date;
      slice.expiry_time = contract.expiry_time();
      slice.style = contract.style;
      metrics.slices.push_back(slice);
      StubSource source(metrics);
      const auto summary = get(source, "/api/underlyings/" + metrics.symbol + "/summary");
      const auto& expiry = summary["expiries"][0];
      const auto last_date = contract.settlement == md::Settlement::AM ? md::previous_business_day(date) : date;
      const auto minute = std::string_view(root) == "SPXW" ? 0 : 15;
      const auto last = md::new_york_to_utc(last_date, md::regular_close_hour(last_date), minute);
      EXPECT_EQ(expiry["last_trade"], md::format_timestamp(last));
      EXPECT_EQ(expiry["auto_close"], md::format_timestamp(last - 5 * md::kNanosPerMinute));
      const auto chain = get(source, "/api/underlyings/" + metrics.symbol + "/chain?expiry=" + expiry["id"].get<std::string>());
      EXPECT_EQ(chain["expiry"]["last_trade"], expiry["last_trade"]);
      EXPECT_EQ(chain["expiry"]["auto_close"], expiry["auto_close"]);
    }
  }
}

TEST(Api, ChainReturnsBothSidesOfEveryStrike) {
  StubSource source;
  const json chain = get(source, "/api/underlyings/SPX/chain?expiry=2026-10-22PM");
  ASSERT_EQ(chain["strikes"].size(), 41u);
  const json& atm = chain["strikes"][20];
  EXPECT_DOUBLE_EQ(atm["strike"].get<double>(), 5000.0);
  EXPECT_TRUE(atm["call"]["iv"].is_number());
  EXPECT_TRUE(atm["put"]["delta"].is_number());
  EXPECT_TRUE(atm["call"]["vendor_iv"].is_null());  // the synthetic feed sends none

  const json narrow = get(source, "/api/underlyings/SPX/chain?expiry=2026-10-22PM&window=0.02");
  EXPECT_EQ(narrow["strikes"].size(), 9u);  // 4900..5100
}

TEST(Api, ExposureMatrixIsAlignedByStrike) {
  StubSource source;
  const json exposure = get(source, "/api/underlyings/SPX/exposure?window=0.05");
  const auto strikes = exposure["strikes"].get<std::vector<double>>();
  ASSERT_EQ(exposure["expiries"].size(), 1u);
  EXPECT_EQ(exposure["expiries"][0]["gex"].size(), strikes.size());
  EXPECT_EQ(exposure["total_gex"].size(), strikes.size());
}

TEST(Api, SurfaceHasLogMoneynessPoints) {
  StubSource source;
  const json surface = get(source, "/api/underlyings/SPX/surface");
  const json& points = surface["expiries"][0]["points"];
  ASSERT_FALSE(points.empty());
  EXPECT_TRUE(points[0]["k"].is_number());
}

TEST(Api, ErrorsAreJson) {
  StubSource source;
  EXPECT_TRUE(get(source, "/api/underlyings/QQQ/summary", 404).contains("error"));
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?expiry=2030-01-01PM", 404).contains("error"));
  EXPECT_TRUE(get(source, "/api/nope", 404).contains("error"));
  const auto post = server::handle_api({"POST", "/api/status"}, source);
  EXPECT_EQ(post.status, 405);
}

TEST(Api, RejectsMalformedAndOutOfRangeNumbers) {
  StubSource source;
  for (const std::string value : {"", "0", "501", "-1", "1.5", "1e2", "nan", "inf",
                                  "999999999999999999999999", "8junk", " 8"}) {
    EXPECT_TRUE(
        get(source, "/api/underlyings/SPX/exposure?expiries=" + value, 400).contains("error"));
  }
  for (const std::string value :
       {"", "-0.1", "1.01", "nan", "inf", "1e999", "0.1junk", " 0.1", "0.1 ", "0x1p-4", "0x0", ".",
        "+", "1e", "1e+", "1e-", "1e-999", "1e-310"}) {
    EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?window=" + value, 400).contains("error"));
  }
  for (const auto value : {"0", "-0", "+.5", ".5", "1.", "1e-2", "+1.0E-2", "0e-999"})
    EXPECT_FALSE(
        get(source, std::string("/api/underlyings/SPX/chain?window=") + value).contains("error"));
  EXPECT_FALSE(get(source, "/api/underlyings/SPX/surface?expiries=500&window=1").contains("error"));
  EXPECT_FALSE(get(source, "/api/underlyings/SPX/chain?expiries=1&window=0").contains("error"));
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?window=2&window=0.5", 400).contains("error"));
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/chain?window=1e-999", 400).contains("error"));
}

TEST(Api, TickMessageCarriesVersions) {
  StubSource source;
  const json tick = json::parse(server::tick_message(source));
  EXPECT_EQ(tick["type"], "tick");
  EXPECT_EQ(tick["underlyings"][0]["symbol"], "SPX");
  EXPECT_TRUE(tick["underlyings"][0]["version"].is_number());
}

TEST(Api, OexAndXeoHaveDistinctPmExpiryIds) {
  analytics::ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("OEX261016C03000000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("XEO261016C03000000")});
  book.apply(md::UnderlyingQuote{"OEX", 1, 0, 0, 3000});
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 12, 0);
  StubSource source(analytics::analyze(book.underlyings().at("OEX"), book, as_of));
  const auto summary = get(source, "/api/underlyings/OEX/summary");
  ASSERT_EQ(summary["expiries"].size(), 2u);
  std::set<std::string> ids;
  for (const auto& expiry : summary["expiries"]) ids.insert(expiry["id"].get<std::string>());
  EXPECT_EQ(ids, (std::set<std::string>{"2026-10-16PM-OEX", "2026-10-16PM-XEO"}));
  for (const auto& id : ids)
    EXPECT_EQ(get(source, "/api/underlyings/OEX/chain?expiry=" + id)["expiry"]["id"], id);
}

TEST(WebServer, ServesTheApiAndTheAppShellOverHttp) {
  const auto root = std::filesystem::temp_directory_path() / "openport-web-test";
  std::filesystem::create_directories(root / "assets");
  std::ofstream(root / "index.html") << "<!doctype html><title>OpenPort</title>";
  std::ofstream(root / "assets" / "app-1234.js") << "console.log('hi')";

  StubSource source;
  server::WebServer web("127.0.0.1", 0, root, [&source](const server::ApiRequest& r) {
    return server::handle_api(r, source);
  });
  web.start(1);
  const std::string base = "http://127.0.0.1:" + std::to_string(web.port());

  net::HttpClient http;
  const auto status = http.get(base + "/api/status");
  EXPECT_EQ(status.status, 200);
  EXPECT_EQ(json::parse(status.body)["provider"]["name"], "cboe");

  const auto shell = http.get(base + "/");
  EXPECT_EQ(shell.status, 200);
  EXPECT_NE(shell.body.find("OpenPort"), std::string::npos);

  const auto route = http.get(base + "/chain/SPX");  // client-side route -> app shell
  EXPECT_EQ(route.status, 200);
  EXPECT_NE(route.body.find("OpenPort"), std::string::npos);

  EXPECT_EQ(http.get(base + "/assets/app-1234.js").status, 200);
  EXPECT_EQ(http.get(base + "/assets/missing.js").status, 404);
  EXPECT_EQ(http.get(base + "/../etc/passwd").status, 400);

  web.stop();
  std::filesystem::remove_all(root);
}

TEST(Api, PublishesMarketAndAnalyticsProvenance) {
  StubSource source;
  auto status = get(source, "/api/status");
  auto tick = json::parse(server::tick_message(source));
  for (auto* j : {&status, &tick}) {
    const auto& session = (*j)["underlyings"][0]["session"];
    ASSERT_TRUE(session.is_object());
    EXPECT_TRUE(session["open"].is_boolean());
    EXPECT_TRUE(session["note"].is_string());
    EXPECT_TRUE(session["name"] == "regular" || session["name"] == "curb" ||
                session["name"] == "global" || session["name"] == "closed");
    EXPECT_EQ(session["open"], session["name"] != "closed");
    ASSERT_TRUE((*j)["market"].is_object());
    EXPECT_TRUE((*j)["market"]["open"].is_boolean());
    EXPECT_TRUE((*j)["market"]["note"].is_string());
    EXPECT_TRUE((*j)["market"].contains("next_open"));
  }
  for (const auto view : {"summary", "chain", "exposure", "surface"}) {
    auto j = get(source, std::string("/api/underlyings/SPX/") + view);
    EXPECT_EQ(j["spot_source"], "quote");
  }
  auto summary = get(source, "/api/underlyings/SPX/summary");
  EXPECT_EQ(summary["american_approximation"], false);
  const json coverage{{"options", 82}, {"quoted", 82}, {"priced", 82}, {"open_interest", 82}, {"volume", 0}};
  EXPECT_EQ(summary["coverage"], coverage);
  EXPECT_EQ(summary["expiries"][0]["coverage"], coverage);
  EXPECT_EQ(summary["expiries"][0]["style"], "european");
  EXPECT_EQ(summary["exposure"]["oi_coverage"], 1.0);
}

TEST(Api, MissingQuotesAndOiAreNullWhileReceivedZerosStayZero) {
  analytics::ChainBook book;
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 12, 0);
  book.apply(md::UnderlyingQuote{"SPY", as_of, 100, 100, 100});
  book.apply(md::ContractDefinition{0, *md::parse_osi("SPY261022C00100000")});
  book.apply(md::ContractDefinition{1, *md::parse_osi("SPY261022P00100000")});
  book.apply(md::ContractDefinition{2, *md::parse_osi("SPY1261022C00100000")});
  book.apply(md::OptionQuote{1, as_of, 0, 2, 0, 1});
  book.apply(md::OpenInterest{1, as_of, 0});
  StubSource source(analytics::analyze(book.underlyings().at("SPY"), book, as_of));
  auto chain = get(source, "/api/underlyings/SPY/chain");
  auto& row = chain["strikes"][0];
  for (const auto key : {"bid", "ask", "mid", "oi"}) EXPECT_TRUE(row["call"][key].is_null());
  EXPECT_EQ(row["put"]["bid"], 0);
  EXPECT_EQ(row["put"]["ask"], 2);
  EXPECT_EQ(row["put"]["mid"], 1);
  EXPECT_EQ(row["put"]["oi"], 0);
  EXPECT_EQ(chain["expiry"]["style"], "american");
  EXPECT_EQ(chain["expiry"]["coverage"]["options"], 2);
  EXPECT_EQ(chain["expiry"]["coverage"]["quoted"], 1);
  EXPECT_EQ(chain["expiry"]["coverage"]["open_interest"], 1);
}

TEST(Api, AmericanApproximationAndPartialExposureCoverageAreExplicit) {
  test::SyntheticChain original;
  analytics::ChainBook book;
  for (md::InstrumentId id = 0; id < 82; ++id) {
    const auto* s = original.book.option(id);
    auto c = s->contract;
    c.underlying = "SPY";
    c.root = "SPY";
    c.style = pricing::ExerciseStyle::American;
    book.apply(md::ContractDefinition{id, c});
    book.apply(md::OptionQuote{id, original.as_of, s->bid, s->ask, 1, 1});
    if (id < 41) book.apply(md::OpenInterest{id, original.as_of, 0});
  }
  analytics::AnalyticsOptions options;
  options.deamericanize = false;
  StubSource source(
      analytics::analyze(book.underlyings().at("SPY"), book, original.as_of, options));
  auto summary = get(source, "/api/underlyings/SPY/summary");
  EXPECT_EQ(summary["spot_source"], "parity");
  EXPECT_EQ(summary["american_approximation"], true);
  EXPECT_EQ(summary["coverage"],
            (json{{"options", 82}, {"quoted", 82}, {"priced", 82}, {"open_interest", 41}, {"volume", 0}}));
  EXPECT_EQ(summary["expiries"][0]["style"], "american");
  EXPECT_EQ(summary["exposure"]["oi_coverage"], .5);
  auto exposure = get(source, "/api/underlyings/SPY/exposure?window=0.001");
  EXPECT_EQ(exposure["exposure"]["oi_coverage"], .5);
  auto chain = get(source, "/api/underlyings/SPY/chain");
  EXPECT_EQ(chain["expiry"]["coverage"], summary["coverage"]);
  EXPECT_EQ(chain["expiry"]["style"], "american");
  EXPECT_EQ(chain["strikes"][0]["call"]["oi"], 0);
  EXPECT_TRUE(chain["strikes"][40]["call"]["oi"].is_null());
}

TEST(Api, UnanchoredUnquotedChainStillReportsMissingDataAndCoverage) {
  analytics::ChainBook book;
  book.apply(md::ContractDefinition{0, *md::parse_osi("SPY261022C00100000")});
  const auto as_of = md::new_york_to_utc({2026, 9, 22}, 12, 0);
  StubSource source(analytics::analyze(book.underlyings().at("SPY"), book, as_of));
  auto summary = get(source, "/api/underlyings/SPY/summary");
  EXPECT_TRUE(summary["spot"].is_null());
  EXPECT_TRUE(summary["spot_source"].is_null());
  EXPECT_EQ(summary["american_approximation"], false);
  EXPECT_EQ(summary["coverage"],
            (json{{"options", 1}, {"quoted", 0}, {"priced", 0}, {"open_interest", 0}, {"volume", 0}}));
  EXPECT_TRUE(summary["exposure"]["oi_coverage"].is_null());
  for (const auto view : {"chain", "exposure", "surface"}) {
    auto j = get(source, std::string("/api/underlyings/SPY/") + view);
    EXPECT_TRUE(j["spot_source"].is_null());
    EXPECT_TRUE(j["spot"].is_null());
  }
}

TEST(Api, RateSourcesAndDeamericanisationAreIncludedInBothExpiryViews) {
  for (bool curve : {false, true}) {
    analytics::ChainBook book;
    md::InstrumentId id = 0;
    const auto as_of = md::new_york_to_utc({2026, 9, 22}, 16, 0);
    test::add_american_expiry(book, as_of, {2027, 9, 22}, id, 100, 80, 2.5, 17, 101);
    analytics::AnalyticsOptions options;
    if (curve)
      options.discount_curve =
          analytics::DiscountCurve::from_points("SPX", {{.25, .045}, {1, .045}});
    StubSource source(analytics::analyze(book.underlyings().at("SPY"), book, as_of, options));
    const auto summary = get(source, "/api/underlyings/SPY/summary");
    const auto chain = get(source, "/api/underlyings/SPY/chain");
    EXPECT_EQ(summary["american_approximation"], false);
    bool positive_eep = false;
    for (const auto& row : chain["strikes"]) {
      for (const auto side : {"call", "put"}) {
        const auto& quote = row[side];
        EXPECT_TRUE(quote["eep"].is_number());
        positive_eep = positive_eep || quote["eep"].get<double>() > .01;
        const double strike = row["strike"].get<double>();
        const auto& pair = book.underlyings().at("SPY").expiries.begin()->second.strikes.at(strike);
        const auto* raw = book.option(std::string_view(side) == "call" ? pair.call : pair.put);
        EXPECT_NEAR(quote["bid"].get<double>(), raw->bid, .000051);
        EXPECT_NEAR(quote["ask"].get<double>(), raw->ask, .000051);
        EXPECT_NEAR(quote["mid"].get<double>(), raw->mid(), .000051);
      }
    }
    EXPECT_TRUE(positive_eep);
    for (const auto& expiry : {summary["expiries"][0], chain["expiry"]}) {
      EXPECT_EQ(expiry["rate_source"], curve ? "curve" : "assumed");
      EXPECT_EQ(expiry["rate_curve_symbol"], curve ? json("SPX") : json(nullptr));
      EXPECT_EQ(expiry["rate_fitted"], false);
      EXPECT_EQ(expiry["deamericanized"], true);
    }
  }
  StubSource european;
  EXPECT_TRUE(get(european, "/api/underlyings/SPX/chain")["strikes"][0]["call"]["eep"].is_null());
  const auto expiry = get(european, "/api/underlyings/SPX/chain")["expiry"];
  EXPECT_EQ(expiry["rate_source"], "parity");
  EXPECT_EQ(expiry["rate_fitted"], true);
  EXPECT_EQ(expiry["deamericanized"], false);
  EXPECT_TRUE(expiry["rate_curve_symbol"].is_null());
}

}  // namespace

namespace {
class VolatilitySource final : public server::MetricsSource {
 public:
  std::shared_ptr<const analytics::UnderlyingMetrics> snapshot;
  mutable server::CandleStore store;
  mutable std::atomic<int> event_reads{0};
  std::vector<std::string> symbols() const override { return {"SPX"}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string& symbol) const override {
    return symbol == "SPX" ? snapshot : nullptr;
  }
  server::EngineStatus status() const override { return {}; }
  const server::CandleStore* candles() const override { return &store; }
  std::vector<analytics::EventLabel> events() const override {
    ++event_reads;
    return {{{2026, 9, 23}, "Supplied label"}};
  }
};

TEST(Api, VolatilityShapeUsesNullsForMissingMetricsAndExplicitReasons) {
  analytics::UnderlyingMetrics metrics;
  metrics.symbol = "SPX";
  metrics.as_of = md::new_york_to_utc({2026, 9, 22}, 15, 0);
  VolatilitySource source;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(metrics);
  const auto body = get(source, "/api/underlyings/SPX/volatility");
  for (const auto* key : {"mfiv", "atm", "skew", "term", "realized", "vrp", "implied_moves", "sources"})
    EXPECT_TRUE(body.contains(key)) << key;
  EXPECT_EQ(body["as_of"], md::format_timestamp(metrics.as_of));
  EXPECT_TRUE(body["spot"].is_null()); EXPECT_TRUE(body["forward"].is_null());
  EXPECT_EQ(body["units"], "vol_points");
  ASSERT_EQ(body["mfiv"]["constant"].size(), 5u);
  EXPECT_EQ(body["mfiv"]["constant"][1]["days"], 30);
  EXPECT_TRUE(body["mfiv"]["constant"][1]["vol"].is_null());
  EXPECT_TRUE(body["mfiv"]["constant"][1]["near"].is_null());
  EXPECT_EQ(body["mfiv"]["constant"][1]["reason"], "target_not_bracketed");
  EXPECT_TRUE(body["skew"]["delta25"]["rr"].is_null());
  EXPECT_TRUE(body["realized"]["daily_as_of"].is_null());
  EXPECT_EQ(body["realized"]["windows"].size(), 6u);
  EXPECT_TRUE(body["realized"]["windows"][0]["close_to_close"]["vol"].is_null());
  EXPECT_EQ(body["realized"]["windows"][0]["close_to_close"]["reason"], "insufficient_history");
  EXPECT_TRUE(body["vrp"]["spread"].is_null());
  EXPECT_TRUE(body["vrp"]["ratio"].is_null());
  EXPECT_EQ(body["implied_moves"]["sessions"][1]["label"], "Supplied label");
  EXPECT_TRUE(body["implied_moves"]["sessions"][0]["points"].is_null());
  EXPECT_EQ(body, get(source, "/api/underlyings/SPX/volatility"));
  EXPECT_EQ(source.event_reads, 1);
  get(source, "/api/underlyings/QQQ/volatility", 404);
}

TEST(Api, VolatilityUsesSnapshotCacheAndRetainsCloseOnlyHistory) {
  test::SyntheticChain chain;
  auto metrics = analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of);
  VolatilitySource source;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(metrics);
  auto day = md::days_since_epoch({2026, 8, 1});
  std::vector<md::Bar> bars;
  for (int i = 0; i < 30; ++day) {
    const auto date = md::date_from_days(day);
    const auto time = md::new_york_to_utc(date, 9, 30);
    if (!md::market_session(time).open) continue;
    const double close = 100 + (i % 4);
    bars.push_back({time, 0, 104, 99, close});
    ++i;
  }
  source.store.merge_days("SPX", bars);
  const auto body = get(source, "/api/underlyings/SPX/volatility");
  EXPECT_EQ(body["mfiv"]["expiries"].size(), 1u);
  EXPECT_TRUE(body["mfiv"]["expiries"][0]["vol"].is_number());
  EXPECT_EQ(body["skew"]["delta_convention"], analytics::kDeltaConvention);
  const auto& rv = body["realized"]["windows"][2];
  EXPECT_TRUE(rv["close_to_close"]["vol"].is_number());
  EXPECT_EQ(rv["parkinson"]["vol"], rv["close_to_close"]["vol"]);
  EXPECT_EQ(rv["parkinson"]["fallback"], true);
  EXPECT_TRUE(source.store.bars("SPX", server::BarInterval::Day, 100).empty());
  EXPECT_EQ(body, get(source, "/api/underlyings/SPX/volatility"));
  // A different snapshot owner, even with the same symbol/version, cannot reuse it.
  metrics.slices.clear();
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(metrics);
  EXPECT_TRUE(get(source, "/api/underlyings/SPX/volatility")["mfiv"]["expiries"].empty());
}
}  // namespace

namespace {
TEST(Api, ConcurrentVolatilityRequestsShareOneColdComputationAndSurfaceFits) {
  test::SyntheticChain chain;
  VolatilitySource source;
  source.snapshot = std::make_shared<const analytics::UnderlyingMetrics>(
      analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of));
  const auto surface = get(source, "/api/underlyings/SPX/surface");
  std::vector<std::future<server::ApiResponse>> requests;
  for (int i = 0; i < 4; ++i) requests.push_back(std::async(std::launch::async, [&] {
    return server::handle_api({"GET", "/api/underlyings/SPX/volatility"}, source);
  }));
  std::string first;
  for (auto& request : requests) {
    const auto result = request.get();
    EXPECT_EQ(result.status, 200);
    if (first.empty()) first = result.body;
    EXPECT_EQ(result.body, first);
  }
  EXPECT_EQ(source.event_reads, 1);
  EXPECT_EQ(surface, get(source, "/api/underlyings/SPX/surface"));
}

TEST(Api, ChainVolumeDistinguishesUnknownZeroAndReportedContracts) {
  test::SyntheticChain chain;
  chain.book.apply(md::OptionVolume{0, chain.as_of, 123});
  chain.book.apply(md::OptionVolume{1, chain.as_of, 0});
  StubSource source(analytics::analyze(chain.book.underlyings().at("SPX"), chain.book, chain.as_of));
  const auto body = get(source, "/api/underlyings/SPX/chain?expiry=2026-10-22PM");
  EXPECT_EQ(body["strikes"][0]["call"]["volume"], 123);
  EXPECT_EQ(body["strikes"][0]["put"]["volume"], 0);
  EXPECT_TRUE(body["strikes"][1]["call"]["volume"].is_null());
  EXPECT_EQ(body["expiry"]["coverage"]["volume"], 2);
  EXPECT_EQ(get(source, "/api/underlyings/SPX/summary")["coverage"]["volume"], 2);
}

}  // namespace

namespace {
TEST(Api, ContractFixture) {
  const StubSource source;
  for (const auto* path : {"/api/status", "/api/underlyings/SPX", "/api/underlyings/SPX/summary", "/api/underlyings/SPX/chain",
      "/api/underlyings/SPX/exposure", "/api/underlyings/SPX/surface", "/api/underlyings/SPX/volatility", "/api/underlyings/SPX/candles"}) {
    const auto response = server::handle_api({"GET", path}, source);
    EXPECT_EQ(response.status, 200) << response.body;
    test::capture_contract("analytics", "GET", path, response);
  }
  test::capture_contract("analytics", "GET", "/api/underlyings/UNKNOWN/summary",
                         server::handle_api({"GET", "/api/underlyings/UNKNOWN/summary"}, source));
}
}  // namespace
