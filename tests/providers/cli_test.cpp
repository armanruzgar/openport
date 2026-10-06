#include <gtest/gtest.h>
#include <sys/wait.h>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>

#include "support/recording.hpp"
#include "support/scripted_market.hpp"
#include "openport/trading/journal.hpp"
#include "openport/pricing/black.hpp"
#include "openport/server/plans.hpp"

namespace {
/// A private HOME for launched binaries, so a daemon that gets far enough to start never
/// touches the user's real ~/.openport paper journal.
std::string isolated_home() {
  static const auto home = [] {
    std::string pattern = (std::filesystem::temp_directory_path() / "openport-cli-home-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
    return pattern;
  }();
  return "HOME='" + home + "' ";
}

void rejects(const std::string& application, const std::string& args, const std::string& reason,
             const std::string& environment = "") {
  const auto command = environment + isolated_home() + "\"" + OPENPORT_APPS_DIR + "/" + application + "\" " +
                       args + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 2) << output;
  EXPECT_NE(output.find(reason), std::string::npos) << output;
}

TEST(Cli, ImportValidatesDatesSymbolsCredentialsAndFlagsWithoutStartingTheDaemon) {
  rejects("openportd", "--import-day databento --date 2026-09-22 --symbols SPX", "DATABENTO_API_KEY", "DATABENTO_API_KEY='' ");
  rejects("openportd", "--import-day thetadata --symbols SPX", "requires --date");
  rejects("openportd", "--import-day thetadata --date 2026-09-22", "select 1..32 symbols");
  rejects("openportd", "--import-day thetadata --date 2026-02-30 --symbols SPX", "invalid --date");
  rejects("openportd", "--import-day thetadata --date 2099-01-02 --symbols SPX", "future");
  rejects("openportd", "--import-day thetadata --date 2026-07-03 --symbols SPX", "holiday");
  rejects("openportd", "--import-day cboe --date 2026-09-22 --symbols SPX", "provider must be");
  rejects("openportd", "--import-day databento --date 2026-09-22 --symbols SPX --api-key fake", "unknown import option");
  rejects("openportd", "--import-day databento --date 2026-09-22 --symbols SPX --option key=fake", "unknown import option");
}

TEST(Cli, DaemonRejectsMalformedRangesUnknownFlagsAndStartupFailures) {
  for (const auto* port : {"0", "65536", "-1", "80junk", "1.2", "99999999999999999999"})
    rejects("openportd", std::string("--port ") + port, "--port");
  for (const auto* expiry : {"-1", "1x", "1.2"})
    rejects("openportd", std::string("--expiries ") + expiry, "--expiries");
  for (const auto* window : {"nan", "inf", "1.1", "0.1x", "-0.1"})
    rejects("openportd", std::string("--window ") + window, "--window");
  for (const auto* rate : {"nan", "inf", "0.251", "-0.051", "0.04x"})
    rejects("openportd", std::string("--rate ") + rate, "--rate");
  for (const auto* rate : {"-0.05", "0.25", "0.045"})
    rejects("openportd", std::string("--rate ") + rate + " --provider missing", "unknown provider");
  rejects("openportd", "--poll-seconds 0", "poll_seconds");
  rejects("openportd", "--poll-seconds 1x", "poll_seconds");
  rejects("openportd", "--option unknown=1", "unknown provider option");
  rejects("openportd", "--unknown value", "unknown option");
  rejects("openportd", "--address invalid-address", "openportd:");
  rejects("openportd", "--provider missing", "unknown provider");
  rejects("openportd", "--allowed-origin https://host/path", "invalid allowed origin");
  rejects("openportd", "--provider databento --window 0.1", "whole chain upstream");
}

TEST(Cli, PaperFillFlagsValidateModelsAndExecutionRanges) {
  rejects("openportd", "--paper-fill-model unknown", "--paper-fill-model");
  for (const auto* flag : {"--paper-slippage-ticks", "--paper-impact-ticks", "--paper-fill-latency-ms"}) {
    for (const auto* value : {"-1", "1.5", "junk", "60001"})
      rejects("openportd", std::string(flag) + " " + value, flag);
  }
  rejects("openportd", "--paper-impact-ticks 11", "--paper-impact-ticks");
  rejects("openportd", "--paper-slippage-ticks 11", "--paper-slippage-ticks");
  for (const auto* model : {"as_displayed", "conservative", "midpoint"})
    rejects("openportd", std::string("--paper-fill-model ") + model +
        " --paper-fill-latency-ms 60000 --paper-impact-ticks 10 --paper-slippage-ticks 0 --provider missing", "unknown provider");
}

// F14/F27: help follows the catalogue; prerequisites apply to HTTP progression.
TEST(Cli, DaemonHelpListsEveryPlanAndPrerequisite) {
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" --help 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 0) << output;
  EXPECT_NE(output.find("plan presets (--plan seeds any of them in a new journal; over HTTP, steps marked 'after X' need a pass of X first):\n"),
            std::string::npos) << output;
  for (const auto& plan : openport::server::plan_presets()) {
    const auto line = "  " + plan.id + (plan.unlocked_by.empty() ? "" : " (after " + plan.unlocked_by + ")") + "\n";
    EXPECT_NE(output.find(line), std::string::npos) << line;
  }
}

TEST(Cli, EveryPlanReachesProviderValidationWithNewAndEmptyJournals) {
  openport::test::RecordingFile file;
  for (const auto& plan : openport::server::plan_presets()) {
    const auto journal = file.directory / (plan.id + ".jsonl");
    // A missing provider makes a regression fail promptly without starting a server.
    const auto args = "--provider missing --port 9355 --paper-journal '" + journal.string() + "' --plan " + plan.id;
    rejects("openportd", args, "unknown provider");
    EXPECT_FALSE(std::filesystem::exists(journal));
    { std::ofstream empty(journal); }
    rejects("openportd", args, "unknown provider");
    EXPECT_EQ(std::filesystem::file_size(journal), 0U);
  }
  // Analytics-only startup also reaches provider validation with a later step.
  rejects("openportd", "--provider missing --plan two-step-verify-25k --no-paper", "unknown provider");
  // --plan is ignored when recovering an account, even if it names a locked step.
  const auto existing = file.directory / "existing.jsonl";
  { openport::trading::TradingSession account({}, 0, openport::trading::FileJournal::create(existing.string())); }
  rejects("openportd", "--provider missing --paper-journal '" + existing.string() + "' --plan two-step-verify-25k", "unknown provider");
}

TEST(Cli, DemoRejectsNetworkOptionsUnknownDaysAndUnsupportedSymbols) {
  rejects("openportd", "--provider demo --option speed=max", "demo speed");
  rejects("openportd", "--provider demo --option days=overnight", "not a regular session");
  rejects("openportd", "--provider demo --symbols IWM", "unsupported symbol");
  rejects("openportd", "--provider demo --dividends massive", "needs network access");
  rejects("openportd", "--provider demo --window .1", "must be zero");
}

TEST(Cli, DemoDefaultJournalIsSeparateAndExplicitPathWins) {
  const auto invoke = [&](const std::string& args) {
    const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" " + args + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) throw std::runtime_error("popen failed");
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
    const auto result = pclose(pipe);
    EXPECT_TRUE(WIFEXITED(result));
    EXPECT_EQ(WEXITSTATUS(result), 0) << output;
    return output;
  };
  // Maintenance resolves the same default paths without starting a server or feed.
  const auto demo = invoke("--provider demo --compact-journals");
  EXPECT_NE(demo.find("/.openport/demo/paper-journal.jsonl"), std::string::npos);
  const auto live = invoke("--compact-journals");
  EXPECT_EQ(live.find("/.openport/demo/"), std::string::npos);
  openport::test::RecordingFile file;
  const auto explicit_path = file.directory / "custom.jsonl";
  EXPECT_NE(invoke("--provider demo --paper-journal '" + explicit_path.string() + "' --compact-journals")
      .find(explicit_path.string()), std::string::npos);
}

TEST(Cli, ProbeRejectsBadValuesUnknownFlagsAndDatabentoFilters) {
  rejects("openport-probe", "cboe SPY --window nan", "--window");
  rejects("openport-probe", "cboe SPY --window 1x", "--window");
  rejects("openport-probe", "cboe SPY --expiries -1", "--expiries");
  rejects("openport-probe", "cboe SPY --seconds 0", "--seconds");
  rejects("openport-probe", "cboe SPY --quotes 0", "--quotes");
  rejects("openport-probe", "cboe SPY --unknown", "unknown option");
  rejects("openport-probe", "cboe SPY -x", "unknown option");
  rejects("openport-probe", "cboe SPY --window", "missing value");
  rejects("openport-probe", "missing SPY", "unknown provider");
  rejects("openport-probe", "databento SPX --expiries 1", "whole chain upstream");
}

TEST(Cli, BrokerNamesHelpSandboxAndCredentialFlagsAreValidatedOffline) {
  rejects("openport-probe", "--help", "tradier tastytrade");
  rejects("openportd", "--provider tastytrade --option sandbox=true", "funded production");
  rejects("openport-probe", "tastytrade SPX --option sandbox=true", "funded production");
  rejects("openportd", "--provider tradier --option access_token=fake", "unknown provider option");
  rejects("openport-probe", "tastytrade SPX --option refresh_token=fake", "unknown provider option");
}
TEST(Cli, ReplayAndRecordValidationWorksInBothApplications) {
  openport::test::RecordingFile file;
  openport::test::record_events(file.path, {});
  const auto path = " --option file='" + file.path.string() + "'";
  rejects("openportd", "--provider replay", "file=PATH is required");
  rejects("openport-probe", "replay SPX", "file=PATH is required");
  for (const auto& option : {"speed=2", "speed=nan", "loop=yes", "unknown=1"}) {
    const auto reason = std::string(option).substr(0, std::string(option).find('='));
    rejects("openportd", "--provider replay" + path + " --option " + option, reason);
    rejects("openport-probe", "replay SPX" + path + " --option " + option, reason);
  }
  rejects("openportd", "--provider replay --symbols QQQ" + path, "file contains: SPX, SPY");
  rejects("openport-probe", "replay QQQ" + path, "file contains: SPX, SPY");
  rejects("openportd", "--provider replay" + path + " --record '" + file.path.string() + "'",
          "File exists");
  rejects("openport-probe", "replay SPX" + path + " --record '" + file.path.string() + "'",
          "File exists");
  rejects("openportd", "--record ''", "nonempty path");
  rejects("openport-probe", "cboe SPX --record ''", "nonempty path");
  rejects("openport-probe", "replay SPX --option missing-equals", "KEY=VALUE");
}

TEST(Cli, ProbeCanReplayAndRecordANewFileWithoutNetwork) {
  using namespace openport;
  test::RecordingFile source, recorded;
  test::record_events(source.path,
                      {md::ContractDefinition{0, *md::parse_osi("SPXW261022C05000000")},
                       md::OptionQuote{0, 100, 100, 101, 10, 10},
                       md::UnderlyingQuote{"SPX", 100, 5000, 5001, 5000.5},
                       md::ProviderStatus{100, md::FeedState::Live, "snapshot complete", "SPX"}});
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR +
                       "/openport-probe\" replay SPX --option file='" + source.path.string() +
                       "' --option speed=max --option loop=off --record '" +
                       recorded.path.string() + "' 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 0) << output;
  EXPECT_NE(output.find("replay (synthetic)"), std::string::npos) << output;
  EXPECT_NE(output.find("quotes=1"), std::string::npos) << output;
  md::RecordingReader reader(recorded.path);
  EXPECT_EQ(reader.header().provider, "replay (synthetic)");
  EXPECT_EQ(reader.header().subscription.underlyings, std::vector<std::string>{"SPX"});
  std::size_t events = 0;
  while (reader.next()) ++events;
  EXPECT_GE(events, 4u);
  EXPECT_TRUE(reader.diagnostic().empty());
}

TEST(Cli, DaemonReportsJournalLockedByAnotherProcessBeforeWebStartupFails) {
  using namespace openport;
  test::RecordingFile source, journal_file;
  test::record_events(source.path, {});
  const auto journal = trading::FileJournal::create(journal_file.path.string());
  journal->append(0, "test", "{}");
  const auto head = journal->head();
  const auto command = "--provider replay --symbols SPX,SPY --option file='" + source.path.string() +
      "' --option speed=max --paper-journal '" + journal_file.path.string() +
      "' --address invalid-address";
  rejects("openportd", command, "JOURNAL_LOCKED: paper journal is in use by another openportd; use --paper-journal to choose another file or --no-paper");
  const auto recovery = trading::FileJournal::read(journal_file.path.string(), head);
  EXPECT_EQ(recovery.records.size(), 1u);
  EXPECT_FALSE(recovery.truncated_final_line);
  journal->append(1, "still-owned", "{}");
  EXPECT_EQ(journal->sequence(), 2u);
}

struct Finished { int status = -1; std::string output; };
Finished run_daemon(const std::string& args) {
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" " + args + " 2>&1";
  Finished finished;
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) return finished;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) finished.output += buffer;
  const auto status = pclose(pipe);
  if (WIFEXITED(status)) finished.status = WEXITSTATUS(status);
  return finished;
}

TEST(Cli, DaemonPrintsItsVersion) {
  const auto result = run_daemon("--version");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output, std::string("openportd ") + OPENPORT_VERSION + "\n");
  EXPECT_TRUE(std::regex_match(result.output, std::regex(R"(openportd \d+\.\d+\.\d+\n)"))) << result.output;
}

TEST(Cli, DaemonDefaultsToTheIndexAndFourEtfsAndAllowsAnOverride) {
  using namespace openport;
  const std::vector<std::string> symbols{"SPX", "SPY", "QQQ", "IWM", "DIA"};
  test::RecordingFile source;
  auto header = test::recording_header();
  header.subscription.underlyings = symbols;
  test::record_events(source.path, {}, header);
  for (const std::string flags : {"", " --symbols IWM,DIA"}) {
    test::RecordingFile recorded;
    // Record the subscription before an invalid address ends startup, without a listener or network feed.
    const auto result = run_daemon("--provider replay --option file='" + source.path.string() +
        "' --option speed=max --no-paper --record '" + recorded.path.string() +
        "' --address invalid-address" + flags);
    EXPECT_EQ(result.status, 2) << result.output;
    EXPECT_NE(result.output.find("openportd:"), std::string::npos) << result.output;
    EXPECT_EQ(result.output.find("replay: unknown symbol"), std::string::npos) << result.output;
    md::RecordingReader reader(recorded.path);
    EXPECT_EQ(reader.header().subscription.underlyings,
              (flags.empty() ? symbols : std::vector<std::string>{"IWM", "DIA"}));
    EXPECT_EQ(reader.header().subscription.max_expiries, 0);
    EXPECT_DOUBLE_EQ(reader.header().subscription.strike_window, 0);
  }
}

// U2: help is an answer, not an error, and its usage lines name every flag.
TEST(Cli, DaemonHelpGoesToStdoutAndExitsZero) {
  for (const auto* flag : {"--help", "-h"}) {
    const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" " + flag + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
    const auto status = pclose(pipe);
    ASSERT_TRUE(WIFEXITED(status)) << output;
    EXPECT_EQ(WEXITSTATUS(status), 0) << output;
    const auto usage = output.substr(0, output.find("\n\n"));
    for (const auto* expected : {"[--symbols SPX,SPY,QQQ,IWM,DIA]", "[--series-dir DIR] [--no-series]",
                                 "--backfill-series FILE... [--force]", "demo: simulated regular sessions",
                                 "--option revision=1|2|3|4|5|6|7|8 (default 8)", "tradier tastytrade"})
      EXPECT_NE(output.find(expected), std::string::npos) << expected << '\n' << output;
    EXPECT_NE(usage.find("--no-series"), std::string::npos) << usage;
  }
  // An error still goes to stderr with status 2.
  rejects("openportd", "--no-such-flag", "usage: openportd");
}

TEST(Cli, DaemonCompactsJournalsFromEarlierBuildsAndKeepsTheOriginals) {
  using namespace openport;
  std::string pattern = (std::filesystem::temp_directory_path() / "openport-compact-XXXXXX").string();
  ASSERT_NE(mkdtemp(pattern.data()), nullptr);
  const std::filesystem::path directory = pattern;
  std::filesystem::create_directories(directory / "accounts");
  const auto main = directory / "paper-journal.jsonl";
  const auto swing = directory / "accounts" / "swing.jsonl";
  const auto fresh = directory / "accounts" / "fresh.jsonl";
  // Journals as a build before the compact format wrote them, and one written since.
  test::ScriptedMarket f;
  std::string expected;
  for (const auto& file : {main, swing, fresh}) {
    const auto current = file.string() + ".current";
    {
      trading::TradingSession s({}, f.time, trading::FileJournal::create(current));
      f.seed(s);
      for (int i = 0; i < 4; ++i) {
        f.next();
        s.on_quotes({f.quote()}, {f.valuation()}, f.time);
        ASSERT_TRUE(s.submit(f.market("buy-" + std::to_string(i)), f.time).decision.ok());
      }
      if (file == main) expected = s.snapshot_json();
    }
    if (file == fresh) {
      std::filesystem::rename(current, file);
    } else {
      trading::TradingSession::expand(trading::FileJournal::read(current), *trading::FileJournal::create(file.string()));
      std::filesystem::remove(current);
    }
  }
  const auto before = std::filesystem::file_size(main);
  const auto args = "--compact-journals --paper-journal '" + main.string() + "'";
  {
    // A journal in use stays as it is; the others are still rewritten.
    const auto held = trading::FileJournal::resume(swing.string());
    const auto result = run_daemon(args);
    EXPECT_EQ(result.status, 1) << result.output;
    EXPECT_NE(result.output.find(swing.string() + ": left as it was: JOURNAL_LOCKED"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find(main.string() + ": "), std::string::npos) << result.output;
    EXPECT_NE(result.output.find("; the original is paper-journal.jsonl.bak"), std::string::npos) << result.output;
    EXPECT_NE(result.output.find(fresh.string() + ": already compact"), std::string::npos) << result.output;
  }
  EXPECT_EQ(std::filesystem::file_size(directory / "paper-journal.jsonl.bak"), before);
  EXPECT_LT(std::filesystem::file_size(main) * 2, before);
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read(main.string())).snapshot_json(), expected);
  EXPECT_EQ(trading::TradingSession::recover(trading::FileJournal::read((directory / "paper-journal.jsonl.bak").string())).snapshot_json(), expected);
  const auto again = run_daemon(args);
  EXPECT_EQ(again.status, 0) << again.output;
  EXPECT_NE(again.output.find(main.string() + ": already compact"), std::string::npos) << again.output;
  EXPECT_NE(again.output.find("; the original is swing.jsonl.bak"), std::string::npos) << again.output;
  EXPECT_FALSE(std::filesystem::exists(directory / "paper-journal.jsonl.bak2"));
  for (const auto& entry : std::filesystem::directory_iterator(directory / "accounts"))
    EXPECT_EQ(entry.path().string().find(".compacting"), std::string::npos) << entry.path();
  std::filesystem::create_directories(directory / "empty");
  const auto missing = run_daemon("--compact-journals --paper-journal '" + (directory / "empty" / "none.jsonl").string() + "'");
  EXPECT_EQ(missing.status, 0) << missing.output;
  EXPECT_NE(missing.output.find("no paper journals"), std::string::npos) << missing.output;
  std::filesystem::remove_all(directory);
}

}  // namespace

namespace {
TEST(Cli, EventsFileIsParsedBeforeProviderStartupAndRangeChecked) {
  openport::test::RecordingFile file;
  const auto args = "--events '" + file.path.string() + "' --provider missing";
  rejects("openportd", args, "--events: cannot read");
  for (const auto* row : {"2026-02-30,Impossible", "2026-09-30,", "9999-01-01,Far", "2026-09-30,A\n2026-09-30,B"}) {
    { std::ofstream out(file.path); out << row; }
    rejects("openportd", args, "events line");
  }
  { std::ofstream out(file.path); out << "date,label\n2026-09-30,User supplied label\n"; }
  rejects("openportd", args, "unknown provider");
  rejects("openport-probe", "cboe SPY --compare-mfiv", "requires SPX");
  rejects("openport-probe", "cboe SPX --comparison-dir .", "requires --compare-mfiv");
}
}  // namespace

namespace {
TEST(Cli, MfivProbeComparesSyntheticReplayWithSavedIndexMinutesOffline) {
  using namespace openport;
  test::RecordingFile recording;
  const auto time = md::new_york_to_utc({2026, 9, 25}, 15, 59);
  const auto snapshot_time = time + md::kNanosPerMinute;
  std::vector<md::Event> events{md::UnderlyingQuote{"SPX", time, 100, 100, 100}};
  md::InstrumentId id = 0;
  for (const auto* expiry : {"261016", "261120"}) {
    for (int strike = 70; strike <= 130; ++strike) {
      for (const char type : {'C', 'P'}) {
        char symbol[32];
        std::snprintf(symbol, sizeof symbol, "SPXW%s%c%08d", expiry, type, strike * 1000);
        const auto contract = *md::parse_osi(symbol);
        const double years = md::years_between(snapshot_time, contract.expiry_time());
        const double fair = pricing::black_price(contract.type, 100, strike, years, .2, std::exp(-.04*years));
        events.push_back(md::ContractDefinition{id, contract});
        events.push_back(md::OptionQuote{id, time, fair * .999, fair * 1.001, 10, 10});
        events.push_back(md::VendorGreeks{id, time, .2});
        ++id;
      }
    }
  }
  events.push_back(md::SnapshotComplete{"SPX", snapshot_time});
  events.push_back(md::ProviderStatus{snapshot_time, md::FeedState::Delayed, "synthetic complete", "SPX"});
  test::record_events(recording.path, events);
  for (const auto* index : {"_VIX", "_VIX9D", "_VIX3M", "_VIX6M", "_VIX1Y"}) {
    std::ofstream file(recording.directory / (std::string(index) + ".json"));
    file << R"({"timestamp":"2026-09-25 20:15:21","symbol":")" << index << R"(","data":[
      {"datetime":"2026-09-25T16:00:00","price":{"open":20,"high":20,"low":20,"close":20}}]})";
  }
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openport-probe\" replay SPX --compare-mfiv --option file='" +
      recording.path.string() + "' --option speed=max --option loop=off --comparison-dir '" + recording.directory.string() + "' 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 0) << output;
  EXPECT_NE(output.find("_VIX      samples=1"), std::string::npos) << output;
  EXPECT_NE(output.find("_VIX9D    samples=0"), std::string::npos) << output;
  EXPECT_NE(output.find("RR25      samples=1"), std::string::npos) << output;
  EXPECT_NE(output.find("BF25      samples=1"), std::string::npos) << output;
}
TEST(Cli, SeriesBackfillIsOfflineUsesSiblingDirectoryAndRejectsDemoAndBadFlags) {
  using namespace openport;
  test::RecordingFile file;
  const auto time = md::new_york_to_utc({2026, 9, 22}, 16, 0);
  test::record_events(file.path, {md::UnderlyingQuote{"SPX", time, 99, 101, 100}, md::SnapshotComplete{"SPX", time}});
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" --candle-dir '" +
      (file.directory / "candles").string() + "' --backfill-series '" + file.path.string() + "' --force 2>&1";
  for (int pass = 0; pass < 2; ++pass) {
    FILE* pipe = popen(command.c_str(), "r");
    ASSERT_NE(pipe, nullptr);
    std::string output;
    char buffer[512];
    while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
    const auto status = pclose(pipe);
    ASSERT_TRUE(WIFEXITED(status)) << output;
    EXPECT_EQ(WEXITSTATUS(status), 0) << output;
    EXPECT_NE(output.find(pass == 0 ? "1 minutes written" : "0 minutes written"), std::string::npos) << output;
  }
  EXPECT_TRUE(std::filesystem::exists(file.directory / "series/SPX-2026-09.csv"));
  EXPECT_FALSE(std::filesystem::exists(file.directory / "candles"));
  rejects("openportd", "--backfill-series", "requires recording files");
  rejects("openportd", "--force", "requires --backfill-series");
  rejects("openportd", "--series-dir ''", "nonempty path");
  rejects("openportd", "--no-series --backfill-series '" + file.path.string() + "'", "conflicts");
  auto header = test::recording_header(); header.provider = "demo";
  const auto demo = file.directory / "demo.oprec";
  test::record_events(demo, {}, header);
  const auto target = file.directory / "refused";
  rejects("openportd", "--series-dir '" + target.string() + "' --backfill-series '" + file.path.string() + "' '" + demo.string() + "'", "demo recordings are simulated");
  EXPECT_FALSE(std::filesystem::exists(target));
}
}  // namespace

TEST(Cli, SandboxesRequireDemoAndPaperTrading) {
  rejects("openportd", "--sandboxes 1", "--sandboxes requires --provider demo");
  rejects("openportd", "--provider replay --sandboxes 1", "--sandboxes requires --provider demo");
  rejects("openportd", "--provider demo --sandboxes 1 --no-paper", "--sandboxes requires paper trading");
  rejects("openportd", "--provider demo --sandboxes -1", "--sandboxes");
  rejects("openportd", "--provider demo --sandbox-idle-seconds 0", "--sandbox-idle-seconds");
  rejects("openportd", "--provider demo --client-ip-header 'X Real IP'", "--client-ip-header");
}

TEST(Cli, RepairDryRunAndSingleFileModePreserveOtherJournals) {
  using namespace openport;
  test::RecordingFile directory;
  const auto first = directory.directory / "first.jsonl";
  const auto second = directory.directory / "second.jsonl";
  for (const auto& file : {first, second}) {
    { auto journal = trading::FileJournal::create(file.string()); journal->append(1, "test", "{}"); }
    std::ofstream(file, std::ios::app) << "torn";
  }
  const auto size = std::filesystem::file_size(first);
  const auto args = "--repair-journals --file '" + first.string() + "'";
  const auto dry = run_daemon(args + " --dry-run");
  EXPECT_EQ(dry.status, 0) << dry.output;
  EXPECT_NE(dry.output.find("would cut 4 bytes; 1 records kept"), std::string::npos);
  EXPECT_EQ(std::filesystem::file_size(first), size);
  EXPECT_EQ(std::filesystem::file_size(second), size);
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.directory), std::filesystem::directory_iterator{}), 2);
  const auto repaired = run_daemon(args);
  EXPECT_EQ(repaired.status, 0) << repaired.output;
  EXPECT_EQ(std::filesystem::file_size(first), size - 4);
  EXPECT_EQ(std::filesystem::file_size(second), size);
  EXPECT_EQ(run_daemon("--repair-journals --file '" + (directory.directory / "missing").string() + "'").status, 1);
  rejects("openportd", "--dry-run", "require --repair-journals");
}
