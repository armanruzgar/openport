#include "openport/providers/history.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
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
  EXPECT_TRUE(reader.header().trade_venues);  // a v5 import, so older builds refuse it at the header
  EXPECT_FALSE(reader.header().market_controls);
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
  // Three lists and the day's OI, then hour-long blocks from 09:30 to 16:15 for SPY's
  // quotes, the chain's quotes and its trades: 7 each, not one per 15-second window.
  EXPECT_EQ(http.urls.size(), 25U);
  EXPECT_TRUE(waits.empty());  // Theta Terminal queues requests itself; only a 429 waits
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
  const auto late = md::new_york_to_utc({2026, 9, 21}, 23, 45);
  source->read({late, late + 15 * md::kNanosPerSecond}, sink);
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
  // Blocks stop at New York midnight, as Theta's times belong to one date.
  for (const auto* times : {"date=2026-09-21&start_time=20:15:00.000&end_time=21:14:59.999",
                            "date=2026-09-21&start_time=23:45:00.000&end_time=23:59:59.999",
                            "date=2026-09-22&start_time=00:00:00.000&end_time=00:59:59.999"}) {
    EXPECT_TRUE(std::any_of(http.urls.begin(), http.urls.end(), [&](const auto& url) {
      return url.find(times) != std::string::npos;
    })) << times;
  }
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

TEST(HistoryImport, ThetaRetryAfterOutsideItsRangeBacksOffInsteadOfFailing) {
  for (const auto* retry : {"120", "0", "Wed, 21 Oct 2026 07:28:00 GMT"}) {
    SCOPED_TRACE(retry);
    test::HttpStub http;
    std::vector<std::chrono::milliseconds> waits;
    http.respond = [&](std::string_view) -> net::HttpResponse { net::HttpResponse response{429, "private"}; response.headers = {{"Retry-After", retry}}; return response; };
    auto source = providers::theta_history(http, [&](auto delay) { waits.push_back(delay); });
    test::DiscardEvents sink;
    try { source->prepare(request("unused"), {}, sink); FAIL(); }
    catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("HTTP 429"), std::string::npos) << error.what(); }
    EXPECT_EQ(http.urls.size(), 4U);
    const auto expected = std::string_view(retry) == "120" ? 60000 : std::string_view(retry) == "0" ? 1000 : 1000;
    EXPECT_EQ(std::count(waits.begin(), waits.end(), std::chrono::milliseconds(expected)), std::string_view(retry).size() > 3 ? 1 : 3);
  }
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

// Tick quotes, each on its own clock, and no underlying price, as Massive sends them:
// the strike window's center comes from put-call parity alone.
class ParityHistory final : public providers::HistorySource {
 public:
  void prepare(const providers::ImportDay& input, providers::HistoryWindow, md::EventSink& sink) override {
    first = md::new_york_to_utc(input.date, 9, 30) + md::kNanosPerSecond / 1000;
    sink.publish(md::ContractDefinition{0, *md::parse_osi("SPY260923C00500000")});
    sink.publish(md::ContractDefinition{1, *md::parse_osi("SPY260923P00500000")});
    sink.publish(md::ContractDefinition{2, *md::parse_osi("SPY260923C00560000")});
  }
  void read(providers::HistoryWindow window, md::EventSink& sink) override {
    if (first < window.begin || first >= window.end) return;
    const auto quote = [&](md::Timestamp after, md::InstrumentId id, double mid) {
      sink.publish(md::OptionQuote{id, first + after, mid - 0.1, mid + 0.1, 10, 10});
    };
    quote(0, 0, 5);      // a call alone gives no forward yet
    quote(1'000, 1, 5);  // with its put, parity says 500
    // A jump this large within a second is artificial; it shows when the center moves.
    quote(2'000, 0, 65);  // parity now says 560, ...
    quote(3'000, 1, 5);
    quote(4'000, 2, 5);  // ... but the center stays 500 until the next second
    quote(md::kNanosPerSecond + 5'000, 2, 5);
  }
  md::Timestamp first = 0;
};

TEST(HistoryImport, ParityCentersRefreshOncePerMarketSecondOnceKnown) {
  test::RecordingFile file;
  ParityHistory source;
  const auto records = read(providers::import_day(request(file.directory), source, kToday));
  const auto definitions = events_of<md::ContractDefinition>(records);
  ASSERT_EQ(definitions.size(), 3U);
  // Admitted on the quote that first gave its underlying a center, not a second later.
  EXPECT_EQ(definitions[0].contract.type, pricing::OptionType::Put);
  EXPECT_EQ(definitions[1].contract.strike, 500);
  EXPECT_EQ(definitions[2].contract.strike, 560);
  const auto quotes = events_of<md::OptionQuote>(records);
  ASSERT_EQ(quotes.size(), 4U);
  EXPECT_EQ(quotes.front().ts, source.first + 1'000);
  EXPECT_EQ(quotes[1].id, definitions[1].id);
  EXPECT_DOUBLE_EQ(quotes[1].bid, 64.9);
  // Quotes alone refresh a known center once per market second.
  EXPECT_EQ(quotes.back().id, definitions[2].id);
  EXPECT_EQ(quotes.back().ts, source.first + md::kNanosPerSecond + 5'000);
}

TEST(HistoryImport, AFailureAfterTheFirstWindowRemovesThePartialRecording) {
  test::RecordingFile file;
  test::HttpStub http;
  theta_responses(http);
  const auto saved = http.respond;
  http.respond = [saved](std::string_view url) -> net::HttpResponse {
    if (url.find("start_time=10:30:00") != std::string_view::npos) return {500, "private provider error"};  // the second block
    return saved(url);
  };
  auto source = providers::theta_history(http, [](auto) {});
  std::uint64_t written = 0;
  EXPECT_THROW((void)providers::import_day(request(file.directory), *source, kToday,
      [&](std::size_t, std::size_t, std::uint64_t count) { written = count; }), std::runtime_error);
  EXPECT_GT(written, 0U);
  EXPECT_TRUE(std::filesystem::is_empty(file.directory));
}

TEST(HistoryImport, ThetaHalvesABlockOverTheResponseLimitsDownToOneWindow) {
  // One row past Theta's 250,000-row limit; rows are counted before any is parsed.
  std::string oversized;
  for (int line = 0; line <= 250'000; ++line) oversized += "{}\n";
  const auto quote_times = [](const test::HttpStub& stub) {
    std::vector<std::string> result;
    for (const auto& url : stub.urls)
      if (url.find("/option/history/quote?") != std::string::npos) result.push_back(url.substr(url.find("&start_time=")));
    return result;
  };
  {
    test::RecordingFile file;
    test::HttpStub http;
    theta_responses(http);
    const auto saved = http.respond;
    http.respond = [&](std::string_view url) -> net::HttpResponse {
      if (url.find("/option/history/quote?") != std::string_view::npos &&
          url.find("&start_time=09:30:00.000&end_time=10:29:59.999") != std::string_view::npos) return {200, oversized};
      return saved(url);
    };
    auto source = providers::theta_history(http, [](auto) {});
    const auto path = providers::import_day(request(file.directory), *source, kToday);
    EXPECT_EQ(events_of<md::OptionQuote>(read(path)).size(), 3U);
    const auto times = quote_times(http);
    ASSERT_GE(times.size(), 3U);
    EXPECT_EQ(times[0], "&start_time=09:30:00.000&end_time=10:29:59.999&interval=1s&format=ndjson");
    EXPECT_EQ(times[1], "&start_time=09:30:00.000&end_time=09:59:59.999&interval=1s&format=ndjson");
    EXPECT_EQ(times[2], "&start_time=10:00:00.000&end_time=10:29:59.999&interval=1s&format=ndjson");
  }
  test::RecordingFile file;
  test::HttpStub http;
  theta_responses(http);
  const auto saved = http.respond;
  http.respond = [&](std::string_view url) -> net::HttpResponse {
    if (url.find("/option/history/quote?") != std::string_view::npos) return {200, oversized};
    return saved(url);
  };
  auto source = providers::theta_history(http, [](auto) {});
  try {
    (void)providers::import_day(request(file.directory), *source, kToday);
    FAIL() << "accepted one window over the row limit";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("row limit"), std::string::npos) << error.what();
  }
  const auto times = quote_times(http);
  EXPECT_EQ(times.size(), 8U);  // 60, 30 and 15 minutes, then 450, 225, 105, 45 and 15 seconds
  ASSERT_FALSE(times.empty());
  EXPECT_NE(times.back().find("&start_time=09:30:00.000&end_time=09:30:14.999"), std::string::npos) << times.back();
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

namespace {
void massive_responses(test::HttpStub& http) {
  http.respond = [](std::string_view url) -> net::HttpResponse {
    if (url.find("/reference/options/contracts?") != std::string_view::npos) return {200, sample("massive_contracts.json")};
    if (url.find("/quotes/") != std::string_view::npos) return {200, sample("massive_quotes.json")};
    if (url.find("/trades/") != std::string_view::npos) return {200, sample("massive_trades.json")};
    throw std::runtime_error("unexpected Massive route");
  };
}
TEST(HistoryImport, MassiveSavedQuotesAndTradesKeepSipAndExchangeClocks) {
  test::RecordingFile file;
  test::HttpStub http;
  massive_responses(http);
  auto input = request(file.directory, "massive");
  input.subscription.strike_window = 0;
  auto source = providers::massive_history(http, "test-key", [](auto) {});
  const auto path = providers::import_day(input, *source, kToday);
  const auto records = read(path);
  const md::RecordingReader reader(path);
  EXPECT_EQ(reader.header().provider, "massive");
  EXPECT_TRUE(reader.header().imported);
  EXPECT_FALSE(reader.header().capabilities.open_interest);
  EXPECT_EQ(events_of<md::ContractDefinition>(records).size(), 2U);
  EXPECT_EQ(events_of<md::OptionQuote>(records).size(), 4U);
  EXPECT_TRUE(events_of<md::UnderlyingQuote>(records).empty());
  EXPECT_TRUE(events_of<md::OpenInterest>(records).empty());
  const auto open = md::new_york_to_utc(input.date, 9, 30);
  for (const auto& record : records) if (const auto* trade = std::get_if<md::OptionTrade>(&record.event)) {
    EXPECT_EQ(trade->ts, open + 14 * md::kNanosPerSecond + 456);
    EXPECT_EQ(record.received, open + 16 * md::kNanosPerSecond + 789);
  }
  const auto volume = events_of<md::OptionVolume>(records);
  ASSERT_EQ(volume.size(), 2U);
  EXPECT_EQ(volume[0].contracts, 3);
  EXPECT_EQ(http.urls.size(), 5U) << "retain page lookahead rather than fetching every empty window";
  EXPECT_NE(http.urls[0].find("as_of=2026-09-22"), std::string::npos);
  for (std::size_t index = 1; index < http.urls.size(); ++index)
    EXPECT_NE(http.urls[index].find("&limit=50000"), std::string::npos) << http.urls[index];
  for (const auto& headers : http.sent_headers) EXPECT_EQ(headers, (net::Headers{{"Authorization", "Bearer test-key"}}));
}
TEST(HistoryImport, MassivePaginationRetryAndFailuresAreBoundedAndPrivate) {
  for (const std::string mode : {"pages", "foreign", "cycle", "missing-time", "correction", "denied"}) {
    SCOPED_TRACE(mode);
    test::RecordingFile file;
    test::HttpStub http;
    int attempts = 0;
    http.respond = [&](std::string_view url) -> net::HttpResponse {
      if (url.find("/reference/") != std::string_view::npos) {
        if (attempts++ == 0) return {429, "secret", 0, {}, "", {{"Retry-After", "2"}}};
        return {200, sample("massive_contracts.json")};
      }
      if (mode == "denied") return {403, "test-key must not leak"};
      auto body = json::parse(sample(url.find("/quotes/") != std::string_view::npos ? "massive_quotes.json" : "massive_trades.json"));
      if (mode == "foreign") body["next_url"] = "https://example.com/v3/quotes/O:SPY260925C00600000?cursor=secret";
      if (mode == "cycle") body["next_url"] = url;
      if (mode == "missing-time") body["results"][0].erase("sip_timestamp");
      if (mode == "correction" && url.find("/trades/") != std::string_view::npos) body["results"][0]["correction"] = 1;
      if (mode == "pages" && url.find("/quotes/") != std::string_view::npos) {
        if (url.find("cursor=next") == std::string_view::npos) {
          body["results"].erase(1);
          body["next_url"] = std::string(url.substr(0, url.find('?'))) + "?cursor=next";
        } else body["results"].erase(0);
      }
      return {200, body.dump()};
    };
    std::vector<std::chrono::milliseconds> waits;
    auto source = providers::massive_history(http, "test-key", [&](auto wait) { waits.push_back(wait); });
    auto input = request(file.directory, "massive");
    input.subscription.strike_window = 0;
    if (mode == "pages") {
      const auto path = providers::import_day(input, *source, kToday);
      EXPECT_EQ(events_of<md::OptionQuote>(read(path)).size(), 4U);
    } else {
      // Each failure for its own reason: a foreign next_url and a cycle are both refused
      // as pagination, which only the requests below tell apart.
      const auto* reason = mode == "denied"         ? "Massive history: HTTP 403"
                           : mode == "missing-time" ? "Massive history: missing or invalid market timestamp"
                           : mode == "correction"   ? "Massive history: corrected trades are unsupported"
                                                    : "Massive history: invalid pagination URL";
      try { (void)providers::import_day(input, *source, kToday); FAIL() << "expected failure"; }
      catch (const std::exception& error) {
        EXPECT_EQ(std::string(error.what()).find("test-key"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
      }
      EXPECT_TRUE(std::filesystem::is_empty(file.directory));
    }
    // The key goes to Massive's API alone, never to a next_url on another host.
    for (const auto& url : http.urls) {
      EXPECT_TRUE(url.starts_with("https://api.massive.com/")) << url;
      EXPECT_EQ(url.find("example.com"), std::string::npos) << url;
    }
    EXPECT_NE(std::find(waits.begin(), waits.end(), std::chrono::seconds(2)), waits.end());
  }
}

TEST(HistoryImport, MassivePagesShareTheBufferCapAcrossStreams) {
  EXPECT_EQ(providers::massive_history_page_size(0), 50'000U);
  EXPECT_EQ(providers::massive_history_page_size(4), 50'000U);
  EXPECT_EQ(providers::massive_history_page_size(20), 50'000U);
  EXPECT_EQ(providers::massive_history_page_size(400), 2'500U);
  EXPECT_EQ(providers::massive_history_page_size(10'000), 100U);
  EXPECT_EQ(providers::massive_history_page_size(1'000'000), 100U);
  for (const std::size_t streams : {1U, 21U, 400U, 9'999U})
    EXPECT_LE(streams * providers::massive_history_page_size(streams), 1'000'000U) << streams;
}

class DailyHistory final : public providers::HistorySource {
 public:
  void prepare(const providers::ImportDay& input, providers::HistoryWindow day, md::EventSink& sink) override {
    first_ = day.begin;
    sink.publish(md::ContractDefinition{0, *md::parse_osi(input.subscription.underlyings[0] + "261218C00600000")});
  }
  void read(providers::HistoryWindow window, md::EventSink& sink) override {
    if (window.begin == first_) sink.publish(md::OptionQuote{0, first_, 5, 6, 10, 10});
  }
 private:
  md::Timestamp first_ = 0;
};
TEST(HistoryImport, DateRangesDeduplicateSkipClosuresAndResumeMatchingCompletedDays) {
  const auto dates = providers::import_dates({"2026-09-04..2026-09-08", "2026-09-08", "2026-09-04"});
  ASSERT_EQ(dates.size(), 5U);
  EXPECT_THROW((void)providers::import_dates({"2026-09-08..2026-09-04"}), std::invalid_argument);
  EXPECT_THROW((void)providers::import_dates({"2026-09-31"}), std::invalid_argument);
  EXPECT_THROW((void)providers::import_dates({"2022-01-01..2040-01-01"}), std::invalid_argument);
  for (const auto& provider : {"massive", "thetadata", "databento"}) {
    test::RecordingFile file;
    auto input = request(file.directory, provider);
    input.subscription.strike_window = 0;
    int sources = 0;
    const auto factory = [&]() { ++sources; return std::make_unique<DailyHistory>(); };
    std::vector<std::string> states;
    const auto progress = [&](const providers::ImportDayProgress& update) {
      EXPECT_LE(update.day, update.days);
      if (update.state != "importing") states.push_back(update.state);
    };
    providers::import_days(input, dates, kToday, factory, progress);
    EXPECT_EQ(sources, 2);
    EXPECT_EQ(states, (std::vector<std::string>{"imported", "closed", "closed", "closed", "imported"}));
    states.clear();
    providers::import_days(input, dates, kToday, factory, progress);
    EXPECT_EQ(sources, 2);
    EXPECT_EQ(states.front(), "existing");
    EXPECT_EQ(states.back(), "existing");
    input.subscription.max_expiries = 2;
    EXPECT_THROW(providers::import_days(input, dates, kToday, factory), std::runtime_error);
    EXPECT_EQ(sources, 2);
  }
}
TEST(HistoryImport, RangeValidatesBeforeTransportAndRejectsTornExistingDays) {
  test::RecordingFile file;
  auto input = request(file.directory);
  input.subscription.strike_window = 0;
  int sources = 0;
  const auto factory = [&]() { ++sources; return std::make_unique<DailyHistory>(); };
  EXPECT_THROW(providers::import_days(input, {{2026, 9, 22}, kToday}, kToday, factory), std::invalid_argument);
  EXPECT_EQ(sources, 0);
  providers::import_days(input, {{2026, 9, 22}}, kToday, factory);
  const auto path = file.directory / "thetadata-2026-09-22-imported.oprec";
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
  EXPECT_THROW(providers::import_days(input, {{2026, 9, 22}}, kToday, factory), std::runtime_error);
  EXPECT_EQ(sources, 1);
}
}  // namespace

namespace {
TEST(HistoryImport, MassiveIndexReferenceUsesRootAndPreservesAmPmConventions) {
  test::HttpStub http;
  http.respond = [](std::string_view url) -> net::HttpResponse {
    EXPECT_NE(url.find("underlying_ticker=SPX&"), std::string_view::npos);
    auto body = json::parse(sample("massive_contracts.json"));
    for (auto& row : body["results"]) {
      row["ticker"] = row["contract_type"] == "call" ? "O:SPX261218C05000000" : "O:SPXW261218P05000000";
      row["underlying_ticker"] = "SPX";
      row["exercise_style"] = "european";
      row["expiration_date"] = "2026-12-18";
      row["strike_price"] = 5000;
    }
    auto adjusted = body["results"][0];
    adjusted["ticker"] = "O:SPX1260923C05000000";
    adjusted["expiration_date"] = "2026-09-23";
    body["results"].push_back(adjusted);
    return {200, body.dump()};
  };
  auto source = providers::massive_history(http, "test", [](auto) {});
  auto input = request("unused", "massive");
  input.subscription.underlyings = {"SPX"};
  test::EventCollector sink;
  const auto windows = providers::history_windows(input, kToday);
  source->prepare(input, {windows.front().begin, windows.back().end}, sink);
  const auto definitions = sink.snapshot();
  ASSERT_EQ(definitions.size(), 2U);
  EXPECT_EQ(std::get<md::ContractDefinition>(definitions[0]).contract.settlement, md::Settlement::AM);
  EXPECT_EQ(std::get<md::ContractDefinition>(definitions[1]).contract.settlement, md::Settlement::PM);
}
}  // namespace

namespace {
TEST(HistoryImport, FailedRangeKeepsEarlierDaysAndRetryCreatesOnlyMissingDays) {
  test::RecordingFile file;
  auto input = request(file.directory);
  input.subscription.strike_window = 0;
  const auto dates = providers::import_dates({"2026-09-21..2026-09-23"});
  int sources = 0;
  const auto fail_second = [&]() -> std::unique_ptr<providers::HistorySource> {
    if (++sources == 2) throw std::runtime_error("fixture unavailable");
    return std::make_unique<DailyHistory>();
  };
  EXPECT_THROW(providers::import_days(input, dates, kToday, fail_second), std::runtime_error);
  EXPECT_TRUE(std::filesystem::exists(file.directory / "thetadata-2026-09-21-imported.oprec"));
  EXPECT_FALSE(std::filesystem::exists(file.directory / "thetadata-2026-09-22-imported.oprec"));
  sources = 0;
  providers::import_days(input, dates, kToday, [&]() { ++sources; return std::make_unique<DailyHistory>(); });
  EXPECT_EQ(sources, 2);
}
}  // namespace

namespace {
// `openportd --import-day` reads through make_history_source's wrapper, which must
// forward every HistorySource member. It once dropped trade_venues(), so a ThetaData
// trade row with an exchange code stopped the import at a v4 header.
TEST(HistoryImport, CliSourcesForwardTradeVenuesAndReceiptClocks) {
  {
    test::RecordingFile file;
    auto http = std::make_unique<test::HttpStub>();
    theta_responses(*http);
    const auto* stub = http.get();
    auto source = providers::make_history_source("thetadata", std::move(http), [](auto) {});
    EXPECT_TRUE(source->trade_venues());
    const auto path = providers::import_day(request(file.directory), *source, kToday);
    const md::RecordingReader reader(path);
    EXPECT_TRUE(reader.header().trade_venues);
    const auto trades = events_of<md::OptionTrade>(read(path));
    ASSERT_EQ(trades.size(), 2U);
    EXPECT_EQ(trades[0].venue, 5);  // tests/data/history/trades.ndjson's exchange code
    EXPECT_EQ(trades[1].venue, 0);
    EXPECT_FALSE(stub->urls.empty());
  }
  // Massive's SIP receipt clock reaches the import through the same wrapper.
  struct MassiveKey {
    std::optional<std::string> previous;
    MassiveKey() {
      if (const auto* value = std::getenv("MASSIVE_API_KEY")) previous = value;
      setenv("MASSIVE_API_KEY", "test-key", 1);
    }
    ~MassiveKey() {
      if (previous) setenv("MASSIVE_API_KEY", previous->c_str(), 1);
      else unsetenv("MASSIVE_API_KEY");
    }
  } key;
  test::RecordingFile file;
  auto http = std::make_unique<test::HttpStub>();
  massive_responses(*http);
  auto source = providers::make_history_source("massive", std::move(http), [](auto) {});
  EXPECT_FALSE(source->trade_venues());
  auto input = request(file.directory, "massive");
  input.subscription.strike_window = 0;
  const auto records = read(providers::import_day(input, *source, kToday));
  const auto open = md::new_york_to_utc(input.date, 9, 30);
  std::size_t trades = 0;
  for (const auto& record : records) {
    if (!std::holds_alternative<md::OptionTrade>(record.event)) continue;
    ++trades;
    EXPECT_EQ(record.received, open + 16 * md::kNanosPerSecond + 789);
  }
  EXPECT_GT(trades, 0U);
  EXPECT_THROW((void)providers::make_history_source("databento", std::make_unique<test::HttpStub>(), [](auto) {}),
               std::invalid_argument);
}
}  // namespace
