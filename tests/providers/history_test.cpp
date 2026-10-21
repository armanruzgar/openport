#include "openport/providers/history.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <type_traits>

#include "openport/providers/replay_batches.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "support/http_stub.hpp"
#include "support/recording.hpp"

namespace {
using namespace openport;
using nlohmann::json;
const md::Date kToday{2026, 9, 27};
std::string sample(const std::string& name) {
  std::ifstream input(std::filesystem::path(OPENPORT_TEST_DATA_DIR) / "history" / name);
  if (!input) throw std::runtime_error("missing history fixture");
  return {std::istreambuf_iterator<char>(input), {}};
}
providers::ImportDay request(const std::filesystem::path& directory, std::string provider = "thetadata") {
  return {std::move(provider), {2026, 9, 22}, {{"SPY"}, 1, .05}, directory};
}
void theta_responses(test::HttpStub& http) {
  http.respond = [](std::string_view url) -> net::HttpResponse {
    if (url.find("/list/contracts/quote?") != std::string_view::npos) return {200, sample("contracts.ndjson")};
    if (url.find("/list/expirations?") != std::string_view::npos) return {200, sample("expirations.ndjson")};
    if (url.find("/list/strikes?") != std::string_view::npos) return {200, sample("strikes.ndjson")};
    if (url.find("start_time=09:30:00") == std::string_view::npos && url.find("/open_interest?") == std::string_view::npos) return {472, "No data"};
    if (url.find("/stock/history/quote?") != std::string_view::npos) return {200, sample("stock_quotes.ndjson")};
    if (url.find("/option/history/quote?") != std::string_view::npos) return {200, sample("quotes.ndjson")};
    if (url.find("/option/history/trade?") != std::string_view::npos) return {200, sample("trades.ndjson")};
    if (url.find("/option/history/open_interest?") != std::string_view::npos) return {200, sample("open_interest.ndjson")};
    throw std::runtime_error("unexpected history route");
  };
}
std::vector<md::RecordedEvent> read(const std::filesystem::path& path) {
  md::RecordingReader reader(path);
  std::vector<md::RecordedEvent> result;
  while (auto event = reader.next()) result.push_back(std::move(*event));
  EXPECT_TRUE(reader.diagnostic().empty());
  return result;
}
template <typename T>
std::vector<T> events_of(const std::vector<md::RecordedEvent>& records) {
  std::vector<T> result;
  for (const auto& record : records)
    if (const auto* value = std::get_if<T>(&record.event)) result.push_back(*value);
  return result;
}

TEST(HistoryImport, ThetaSavedResponsesRoundTripWithMarketClocksDefinitionsAndFilters) {
  test::RecordingFile file;
  test::HttpStub http;
  theta_responses(http);
  std::vector<std::chrono::milliseconds> waits;
  auto source = providers::theta_history(http, [&](auto delay) { waits.push_back(delay); });
  std::size_t completed = 0;
  const auto path = providers::import_day(request(file.directory), *source, kToday,
      [&](std::size_t count, std::size_t total, std::uint64_t) { EXPECT_LE(count, total); completed = count; });
  const md::RecordingReader reader(path);
  EXPECT_EQ(reader.header().provider, "thetadata");
  EXPECT_TRUE(reader.header().imported);
  EXPECT_EQ(reader.header().started, md::new_york_to_utc({2026, 9, 22}, 9, 30));
  EXPECT_EQ(reader.header().capabilities.poll_interval.count(), 0);
  const auto records = read(path);
  std::set<md::InstrumentId> defined;
  md::Timestamp receipt = 0;
  for (const auto& record : records) {
    EXPECT_GE(record.received, receipt);
    EXPECT_LT(record.received, md::new_york_to_utc({2026, 9, 22}, 17, 0));
    receipt = record.received;
    std::visit([&](const auto& event) {
      if constexpr (std::is_same_v<std::decay_t<decltype(event)>, md::ContractDefinition>) defined.insert(event.id);
      else if constexpr (requires { event.id; }) { EXPECT_TRUE(defined.contains(event.id)); }
    }, record.event);
  }
  EXPECT_EQ(defined.size(), 2U);
  const auto quotes = events_of<md::OptionQuote>(records);
  ASSERT_EQ(quotes.size(), 3U);
  EXPECT_EQ(quotes[0].ts, md::new_york_to_utc({2026, 9, 22}, 9, 30));
  EXPECT_EQ(quotes.back().bid, 5.2);
  const auto trades = events_of<md::OptionTrade>(records);
  ASSERT_EQ(trades.size(), 2U);
  EXPECT_EQ(trades[0].ts, quotes[0].ts + 1'125'000'000);
  EXPECT_EQ(trades[0].venue, 5);  // the row's exchange code
  EXPECT_EQ(trades[1].venue, 0);  // none given
  const auto volumes = events_of<md::OptionVolume>(records);
  ASSERT_EQ(volumes.size(), 2U);
  EXPECT_EQ(volumes[0].contracts, 3);
  EXPECT_EQ(volumes[1].contracts, 5);
  const auto interest = events_of<md::OpenInterest>(records);
  ASSERT_EQ(interest.size(), 1U);
  EXPECT_EQ(interest[0].ts, md::new_york_to_utc({2026, 9, 22}, 6, 30));
  EXPECT_EQ(interest[0].contracts, 1200);
  EXPECT_EQ(events_of<md::UnderlyingQuote>(records).size(), 2U);
  EXPECT_GT(completed, 1000U);
  EXPECT_EQ(waits.size(), http.urls.size());
  for (const auto& url : http.urls) {
    EXPECT_EQ(url.find("expiration=2026-10-16"), std::string::npos);
    EXPECT_NE(url.find("format=ndjson"), std::string::npos);
  }
  for (const auto& header : http.sent_headers) { EXPECT_TRUE(header.empty()); }
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(file.directory), std::filesystem::directory_iterator{}), 1);
}

TEST(HistoryImport, CalendarIncludesOvernightDstHolidayAndEarlyCloseBoundaries) {
  auto input = request("unused");
  input.subscription.underlyings = {"SPX", "SPY"};
  auto windows = providers::history_windows(input, kToday);
  EXPECT_EQ(windows.front().begin, md::new_york_to_utc({2026, 9, 21}, 20, 15));
  EXPECT_EQ(windows.back().end, md::new_york_to_utc(input.date, 17, 0));
  for (const auto window : windows) {
    EXPECT_EQ(md::trading_date(window.begin), input.date);
    EXPECT_TRUE(md::trading_session("SPX", window.begin).open);
    EXPECT_EQ(window.end - window.begin, 15 * md::kNanosPerSecond);
  }
  input.date = {2026, 11, 27};
  windows = providers::history_windows(input, {2027, 1, 1});
  EXPECT_EQ(windows.back().end, md::new_york_to_utc(input.date, 13, 15));
  // Thanksgiving's morning GTH belongs to Friday as well as Thursday evening.
  EXPECT_EQ(md::new_york_time(windows.front().begin).date, (md::Date{2026, 11, 25}));
  input.date = {2026, 3, 9};
  windows = providers::history_windows(input, kToday);
  EXPECT_EQ(windows.front().begin, *md::parse_datetime("2026-03-09T00:15:00Z", md::Zone::Utc));
  input.subscription.underlyings = {"AAPL"};
  windows = providers::history_windows(input, kToday);
  EXPECT_EQ(windows.front().begin, md::new_york_to_utc(input.date, 9, 30));
  EXPECT_EQ(windows.back().end, md::new_york_to_utc(input.date, 16, 0));
}

TEST(HistoryImport, ThetaIndexPricesKeepOvernightClocksAndDistinctAmPmRoots) {
  test::HttpStub http;
  theta_responses(http);
  const auto saved = http.respond;
  http.respond = [saved](std::string_view url) -> net::HttpResponse {
    if (url.find("/index/history/price?") != std::string_view::npos) return {200, sample("index_prices.ndjson")};
    if (url.find("/option/history/trade?") != std::string_view::npos) return {472, "No data"};
    auto response = url.find("/option/history/quote?") != std::string_view::npos
        ? net::HttpResponse{200, sample("quotes.ndjson")} : saved(url);
    if (response.status != 200) return response;
    std::istringstream input(response.body);
    response.body.clear();
    std::string line;
    while (std::getline(input, line)) {
      auto row = json::parse(line);
      if (row.contains("symbol")) row["symbol"] = url.find("symbol=SPXW") != std::string_view::npos ? "SPXW" : "SPX";
      if (row.contains("strike")) row["strike"] = row["strike"].get<double>() * 10;
      if (row.contains("timestamp") && url.find("/quote?") != std::string_view::npos)
        row["timestamp"] = "2026-09-21T20:15" + row["timestamp"].get<std::string>().substr(16);
      response.body += row.dump() + '\n';
    }
    return response;
  };
  auto source = providers::theta_history(http, [](auto) {});
  auto input = request("unused");
  input.subscription.underlyings = {"SPX"};
  const auto open = md::new_york_to_utc({2026, 9, 21}, 20, 15);
  test::EventCollector sink;
  source->prepare(input, {open, md::new_york_to_utc(input.date, 17, 0)}, sink);
  source->read({open, open + 15 * md::kNanosPerSecond}, sink);
  const auto midnight = md::new_york_to_utc(input.date, 0, 0);
  source->read({midnight, midnight + 15 * md::kNanosPerSecond}, sink);
  std::set<md::Settlement> settlements;
  std::size_t spots = 0;
  for (const auto& event : sink.snapshot()) {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) settlements.insert(definition->contract.settlement);
    if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
      ++spots;
      EXPECT_EQ(spot->symbol, "SPX");
      EXPECT_EQ(spot->ts, open);
      EXPECT_DOUBLE_EQ(spot->last, 5000.2);
    }
    EXPECT_FALSE(std::holds_alternative<md::OpenInterest>(event));  // morning observations are still in the future
  }
  EXPECT_EQ(spots, 1U);
  EXPECT_EQ(settlements, (std::set<md::Settlement>{md::Settlement::AM, md::Settlement::PM}));
  EXPECT_TRUE(std::any_of(http.urls.begin(), http.urls.end(), [](const auto& url) {
    return url.find("date=2026-09-22&start_time=00:00:00.000&end_time=00:00:14.999") != std::string::npos;
  }));
}

TEST(HistoryImport, RejectsInvalidFutureCurrentWeekendAndHolidayDatesBeforeFetching) {
  auto input = request("unused");
  for (const auto date : {md::Date{2026, 12, 1}, kToday, md::Date{2026, 9, 26}, md::Date{2026, 7, 3}, md::Date{2025, 1, 9}, md::Date{2026, 2, 30}}) {
    input.date = date;
    EXPECT_THROW((void)providers::history_windows(input, kToday), std::invalid_argument);
  }
  EXPECT_THROW((void)providers::import_date("2026-2-01"), std::invalid_argument);
  EXPECT_THROW((void)providers::import_date("2026-02-30"), std::invalid_argument);
}

TEST(HistoryImport, ThetaContractListsIncludeFridayHolidayMorningSessions) {
  test::HttpStub http;
  theta_responses(http);
  auto input = request("unused");
  input.date = {2026, 6, 22};
  input.subscription.underlyings = {"SPX"};
  const auto windows = providers::history_windows(input, kToday);
  auto source = providers::theta_history(http, [](auto) {});
  test::DiscardEvents sink;
  source->prepare(input, {windows.front().begin, windows.back().end}, sink);
  EXPECT_TRUE(std::any_of(http.urls.begin(), http.urls.end(), [](const auto& url) {
    return url.find("/option/list/contracts/quote?symbol=SPX&date=2026-06-19") != std::string::npos;
  }));
}

TEST(HistoryImport, NoDataAndTransportFailuresLeaveNoRecordingOrTemporaryFiles) {
  for (const int status : {472, 401, 500}) {
    test::RecordingFile file;
    test::HttpStub http;
    http.respond = [status](std::string_view) -> net::HttpResponse { return {status, "secret response must never be printed"}; };
    auto source = providers::theta_history(http, [](auto) {});
    try {
      (void)providers::import_day(request(file.directory), *source, kToday);
      FAIL() << "accepted missing data";
    } catch (const std::runtime_error& error) {
      EXPECT_EQ(std::string(error.what()).find("secret"), std::string::npos);
      if (status == 472) { EXPECT_NE(std::string(error.what()).find("no option quote data"), std::string::npos); }
    }
    EXPECT_TRUE(std::filesystem::is_empty(file.directory));
  }
}

TEST(HistoryImport, ThetaRequestBudgetHonoursRetryAfterAndHasAFiniteRetryLimit) {
  test::HttpStub http;
  std::vector<std::chrono::milliseconds> waits;
  http.respond = [](std::string_view) -> net::HttpResponse { net::HttpResponse response{429, "private"}; response.headers = {{"Retry-After", "2"}}; return response; };
  auto source = providers::theta_history(http, [&](auto delay) { waits.push_back(delay); });
  test::DiscardEvents sink;
  EXPECT_THROW(source->prepare(request("unused"), {}, sink), std::runtime_error);
  EXPECT_EQ(http.urls.size(), 4U);
  EXPECT_EQ(std::count(waits.begin(), waits.end(), std::chrono::milliseconds(2000)), 3);
}

TEST(HistoryImport, ImportedReplayTradesDeterministicallyAndVerifiesOffline) {
  test::RecordingFile file;
  test::HttpStub http;
  theta_responses(http);
  auto source = providers::theta_history(http, [](auto) {});
  const auto path = providers::import_day(request(file.directory), *source, kToday);
  std::string expected;
  for (int attempt = 0; attempt < 2; ++attempt) {
    md::RecordingReader reader(path);
    providers::ReplayBatches batches(reader, reader.header().subscription);
    EXPECT_FALSE(batches.snapshot_feed());
    server::Desk::Options options;
    options.replay = true;
    options.analytics.fallback_rate = 0;
    options.run_input = server::recording_input(path);
    options.paper_journal = file.directory / ("run-" + std::to_string(attempt) + ".jsonl");
    server::Desk desk("replay (thetadata)", reader.header().capabilities, reader.header().subscription, options);
    desk.start_trading();
    bool ordered = false;
    while (const auto batch = batches.next()) {
      desk.replay_batch(batch->events, batch->received, batch->time);
      if (!ordered) {
        server::TradingCommand command;
        command.order.symbol = md::parse_osi("SPY260923C00500000")->osi_symbol();
        command.order.quantity = 1;
        command.order.type = trading::OrderType::Market;
        command.order.tif = trading::TimeInForce::Ioc;
        command.order.client_order_id = "historical-test";
        std::optional<server::TradingReply> reply;
        desk.command(command, [&](auto value) { reply = std::move(value); }, batch->time, batch->received);
        ASSERT_TRUE(reply);
        ASSERT_TRUE(reply->decision.ok()) << reply->decision.message << " " << reply->error_code;
        ordered = true;
      }
    }
    ASSERT_TRUE(ordered);
    ASSERT_EQ(desk.trading_view()->snapshot->recent_fills.size(), 1U);
    EXPECT_EQ(desk.trading_view()->snapshot->recent_fills[0].price, trading::Money::parse("5.2"));
    desk.stop();
    const auto verified = server::verify_run(options.paper_journal);
    EXPECT_TRUE(verified.matched) << verified.message;
    std::ifstream input(options.paper_journal);
    const std::string contents{std::istreambuf_iterator<char>(input), {}};
    if (attempt == 0) expected = contents;
    else { EXPECT_EQ(contents, expected); }
    const auto cli = std::string(OPENPORT_APPS_DIR) + "/openportd --verify-run '" + options.paper_journal.string() + "' > '" + (file.directory / "verify.txt").string() + "' 2>&1";
    EXPECT_EQ(std::system(cli.c_str()), 0);
  }
}

TEST(HistoryImport, ExistingOutputIsNeverOverwritten) {
  test::RecordingFile file;
  const auto target = file.directory / "thetadata-2026-09-22-imported.oprec";
  { std::ofstream output(target); output << "keep"; }
  test::HttpStub http;
  http.respond = [](std::string_view) -> net::HttpResponse { throw std::runtime_error("must not fetch"); };
  auto source = providers::theta_history(http, [](auto) {});
  EXPECT_THROW((void)providers::import_day(request(file.directory), *source, kToday), std::runtime_error);
  EXPECT_TRUE(http.urls.empty());
  EXPECT_EQ(std::filesystem::file_size(target), 4U);
}

TEST(HistoryImport, HeaderDefaultsKeepOlderRecordingsReadable) {
  test::RecordingFile file;
  auto header = test::recording_header();
  for (const bool imported : {false, true}) {
    const auto path = file.directory / (imported ? "import.oprec" : "v2.oprec");
    header.imported = imported;
    test::record_events(path, {}, header);
    md::RecordingReader reader(path);
    EXPECT_EQ(reader.header().imported, imported);
    EXPECT_EQ(reader.header().provider, header.provider);
    EXPECT_FALSE(reader.next());
  }
}
class DriftingHistory final : public providers::HistorySource {
 public:
  void prepare(const providers::ImportDay& request, providers::HistoryWindow, md::EventSink& sink) override {
    open = md::new_york_to_utc(request.date, 9, 30);
    sink.publish(md::ContractDefinition{10, *md::parse_osi("SPY260923C00500000")});
    sink.publish(md::ContractDefinition{20, *md::parse_osi("SPY260923C00600000")});
  }
  void read(providers::HistoryWindow window, md::EventSink& sink) override {
    for (const bool first : {true, false}) {
      const auto time = open + (first ? 0 : gap);
      if (time < window.begin || time >= window.end) continue;
      sink.publish(md::UnderlyingQuote{"SPY", time, 0, 0, first ? 500.0 : 600.0});
      sink.publish(md::OptionQuote{10, time, first ? 5.0 : 4.0, 6, 10, 10});
      sink.publish(md::OptionQuote{20, time, 5, 6, 10, 10});
      sink.publish(md::OptionTrade{20, time, 5.5, first ? 3.0 : 2.0});
    }
  }
  md::Timestamp gap = 15 * md::kNanosPerSecond;
  md::Timestamp open = 0;
};

TEST(HistoryImport, FilterDriftKeepsKnownContractsAndFullObservedSessionVolume) {
  test::RecordingFile file;
  DriftingHistory source;
  const auto path = providers::import_day(request(file.directory), source, kToday);
  const auto records = read(path);
  const auto definitions = events_of<md::ContractDefinition>(records);
  ASSERT_EQ(definitions.size(), 2U);
  EXPECT_EQ(definitions[0].id, 0U);
  EXPECT_EQ(definitions[1].id, 1U);
  const auto quotes = events_of<md::OptionQuote>(records);
  ASSERT_EQ(quotes.size(), 3U);
  EXPECT_EQ(quotes[1].id, 0U);
  EXPECT_EQ(quotes[1].bid, 4);
  const auto volumes = events_of<md::OptionVolume>(records);
  ASSERT_EQ(volumes.size(), 1U);
  EXPECT_EQ(volumes[0].contracts, 5);
}

TEST(HistoryImport, MalformedResponsesDoNotExposeBodyText) {
  test::RecordingFile file;
  test::HttpStub http;
  http.respond = [](std::string_view) -> net::HttpResponse { return {200, "{private-credential"}; };
  auto source = providers::theta_history(http, [](auto) {});
  try {
    (void)providers::import_day(request(file.directory), *source, kToday);
    FAIL() << "accepted malformed NDJSON";
  } catch (const std::exception& error) {
    EXPECT_EQ(std::string(error.what()).find("private-credential"), std::string::npos);
  }
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}

TEST(HistoryImport, StrikeSelectionCannotSeeLaterPricesInTheSameDownloadWindow) {
  test::RecordingFile file;
  DriftingHistory source;
  source.gap = 2 * md::kNanosPerSecond;
  const auto path = providers::import_day(request(file.directory), source, kToday);
  const auto records = read(path);
  const auto definitions = events_of<md::ContractDefinition>(records);
  ASSERT_EQ(definitions.size(), 2U);
  EXPECT_EQ(definitions[0].contract.strike, 500);
  EXPECT_EQ(definitions[1].contract.strike, 600);
  const auto quotes = events_of<md::OptionQuote>(records);
  ASSERT_EQ(quotes.size(), 3U);
  EXPECT_EQ(quotes.front().ts, source.open);
  EXPECT_EQ(quotes.back().ts, source.open + source.gap);
  EXPECT_EQ(quotes.back().id, definitions[1].id);
}

TEST(HistoryImport, AFailureAfterTheFirstWindowRemovesThePartialRecording) {
  test::RecordingFile file;
  test::HttpStub http;
  theta_responses(http);
  const auto saved = http.respond;
  http.respond = [saved](std::string_view url) -> net::HttpResponse {
    if (url.find("start_time=09:30:15") != std::string_view::npos) return {500, "private provider error"};
    return saved(url);
  };
  auto source = providers::theta_history(http, [](auto) {});
  std::uint64_t written = 0;
  EXPECT_THROW((void)providers::import_day(request(file.directory), *source, kToday,
      [&](std::size_t, std::size_t, std::uint64_t count) { written = count; }), std::runtime_error);
  EXPECT_GT(written, 0U);
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}

TEST(HistoryImport, EveryRequestedSymbolMustHaveQuotes) {
  test::RecordingFile file;
  DriftingHistory source;
  auto input = request(file.directory);
  input.subscription.underlyings.push_back("QQQ");
  EXPECT_THROW((void)providers::import_day(input, source, kToday), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}

TEST(HistoryImport, MissingTradePricesAreNotInvented) {
  test::RecordingFile file;
  test::HttpStub http;
  theta_responses(http);
  const auto saved = http.respond;
  http.respond = [saved](std::string_view url) -> net::HttpResponse {
    if (url.find("/option/history/trade?") != std::string_view::npos)
      return {200, R"({"timestamp":"2026-09-22T09:30:01.125","symbol":"SPY","expiration":"2026-09-23","strike":500,"right":"call","price":null,"size":3})"};
    return saved(url);
  };
  auto source = providers::theta_history(http, [](auto) {});
  EXPECT_THROW((void)providers::import_day(request(file.directory), *source, kToday), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}

}  // namespace
