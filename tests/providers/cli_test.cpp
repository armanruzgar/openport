#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/recording.hpp"
#include "support/scripted_market.hpp"
#include "openport/net/http.hpp"
#include "openport/trading/journal.hpp"
#include "openport/pricing/black.hpp"
#include "openport/server/plans.hpp"
#include "../../apps/openportd/startup.hpp"

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
  rejects("openportd", "--import-day massive --date 2026-09-22 --symbols SPY", "MASSIVE_API_KEY", "MASSIVE_API_KEY='' ");
  rejects("openportd", "--import-day massive --import-days 2026-09-22..2026-09-21 --symbols SPY", "ascending");
  rejects("openportd", "--import-day massive --date 2026-09-22 --date 2099-01-02 --symbols SPY", "future");
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
  for (const auto* model : {"crr", "BAW", "", "bs2002x"})
    rejects("openportd", std::string("--american-model '") + model + "'", "--american-model");
  for (const auto* model : {"lr", "bs2002", "baw", "barone_adesi_whaley"})
    rejects("openportd", std::string("--american-model ") + model + " --provider missing", "unknown provider");
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
  rejects("openportd", "--paper-queue-seed -1", "--paper-queue-seed");
  rejects("openportd", "--paper-stop-stress-ticks 11", "--paper-stop-stress-ticks");
  for (const auto* value : {"100", "1001"})
    rejects("openportd", std::string("--paper-stop-stress-percent ") + value, "--paper-stop-stress-percent");
  rejects("openportd", "--paper-fill-model queue --paper-queue-seed 7 --paper-stop-stress-ticks 2 "
          "--paper-stop-stress-percent 300 --provider missing", "unknown provider");
  for (const auto* model : {"as_displayed", "conservative", "midpoint", "queue"})
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

// A preset's name belongs to its own balance, so --paper-cash sets only practice's.
TEST(Cli, PaperCashSetsOnlyAPracticeBalance) {
  rejects("openportd", "--provider missing --plan funded-intraday-50k --paper-cash 60000",
          "--paper-cash sets only a practice balance; funded-intraday-50k starts at 50000.00");
  rejects("openportd", "--provider missing --plan intraday-25k --paper-cash 100000", "intraday-25k starts at 25000.00");
  // Practice takes any balance, and a preset its own.
  for (const auto* args : {"--paper-cash 25000", "--plan practice --paper-cash 25000", "--plan eod-50k --paper-cash 50000"})
    rejects("openportd", std::string("--provider missing ") + args, "unknown provider");
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

TEST(Cli, CertificationWritesRedactedReportsFromReplayAndRejectsUnsafeCombinations) {
  using namespace openport;
  test::RecordingFile source;
  const auto time = md::parse_datetime("2026-09-22T14:30:00Z", md::Zone::Utc).value();
  test::record_events(source.path, {md::ContractDefinition{0, *md::parse_osi("SPXW991218C05000000")},
      md::OptionQuote{0, time, 100, 101, 1, 1}, md::SnapshotComplete{"SPX", time},
      md::ProviderStatus{time, md::FeedState::Live, "token=never-share account=secret-account", "SPX"}});
  const auto directory = source.directory / "certificate";
  const auto command = isolated_home() + "\"" + OPENPORT_APPS_DIR +
      "/openport-probe\" --certify replay SPX --option file='" + source.path.string() +
      "' --option speed=max --snapshot --seconds 1 --report-dir '" + directory.string() + "' 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status)) << output;
  EXPECT_EQ(WEXITSTATUS(status), 0) << output;
  EXPECT_EQ(output.find("never-share"), std::string::npos);
  EXPECT_EQ(output.find("secret-account"), std::string::npos);
  std::ifstream report_file(directory / "conformance.json");
  const auto report = nlohmann::json::parse(report_file);
  EXPECT_EQ(report["event_counts"]["OptionQuote"], 1);
  EXPECT_EQ(report["mode"], "one_snapshot_cycle");
  EXPECT_TRUE(std::filesystem::exists(directory / "conformance.md"));
  rejects("openport-probe", "--certify cboe SPX --record leak.oprec", "diagnostic redacted");
  rejects("openport-probe", "--certify cboe SPX --report-dir ''", "diagnostic redacted");
  rejects("openport-probe", "--certify cboe SPX --seconds 0", "diagnostic redacted");
}

TEST(Cli, BrokerNamesHelpSandboxAndCredentialFlagsAreValidatedOffline) {
  rejects("openport-probe", "--help", "tradier tastytrade");
  rejects("openportd", "--provider tastytrade --option sandbox=true", "funded production");
  rejects("openport-probe", "tastytrade SPX --option sandbox=true", "funded production");
  rejects("openportd", "--provider tradier --option access_token=fake", "unknown provider option");
  rejects("openport-probe", "tastytrade SPX --option refresh_token=fake", "unknown provider option");
  rejects("openportd", "--provider schwab --option refresh_token=fake", "unknown provider option");
  rejects("openportd", "--provider alpaca --symbols SPX,SPY", "no index options");
  rejects("openport-probe", "alpaca SPY --option feed=sip", "indicative or opra");
  rejects("openportd", "--fallback-seconds 10", "require --fallback-provider");
  rejects("openportd", "--fallback-option poll_seconds=20", "require --fallback-provider");
  rejects("openportd", "--provider demo --fallback-provider cboe", "needs a live --provider");
  rejects("openportd", "--provider cboe --fallback-provider cboe", "different provider");
  rejects("openportd", "--fallback-provider cboe --fallback-seconds 0", "--fallback-seconds");
  rejects("openportd", "--fallback-provider alpaca --symbols SPX", "no index options");
#ifdef OPENPORT_WITH_IBKR
  rejects("openportd", "--provider ibkr --symbols SPY", "--expiries and --window are required");
  rejects("openport-probe", "ibkr SPY --expiries 1 --window 0.05 --option base_url=https://example.com",
          "local Client Portal gateway");
#endif
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
Finished run_daemon(const std::string& args, const std::string& environment = "") {
  const auto command = environment + isolated_home() + "\"" + OPENPORT_APPS_DIR + "/openportd\" " + args + " 2>&1";
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

TEST(Cli, PrintUrlReadsExistingSettingsWithoutStartingOrCreatingAnything) {
  openport::test::RecordingFile file;
  const auto token = file.directory / "write-token";
  const auto args = "--print-url --write-token-file '" + token.string() + "' --address ::1 --port 9090";
  const auto missing = run_daemon(args, "OPENPORT_WRITE_TOKEN='' ");
  EXPECT_EQ(missing.status, 2);
  EXPECT_FALSE(std::filesystem::exists(token));
  { std::ofstream out(token); out << "test a&b/#%\n"; }
  const auto result = run_daemon(args, "OPENPORT_WRITE_TOKEN='' ");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output, "http://[::1]:9090/#token=test%20a%26b%2F%23%25\n");
  EXPECT_EQ(run_daemon(args, "OPENPORT_WRITE_TOKEN=override ").output, "http://[::1]:9090/#token=override\n");
  EXPECT_EQ(run_daemon(args + " --write-token explicit", "OPENPORT_WRITE_TOKEN=override ").output,
            "http://[::1]:9090/#token=explicit\n");
  EXPECT_EQ(run_daemon("--print-url --paper-journal '" + (file.directory / "paper.jsonl").string() + "'",
                       "OPENPORT_WRITE_TOKEN='' ").output,
            "http://127.0.0.1:8080/#token=test%20a%26b%2F%23%25\n");
  EXPECT_FALSE(std::filesystem::exists(file.directory / "paper.jsonl"));
  EXPECT_FALSE(std::filesystem::exists(file.directory / "crash.log"));
  rejects("openportd", "--print-url --repair-journals", "cannot be combined");
  rejects("openportd", "--print-url --dry-run", "cannot be combined");
  const auto named = file.directory / "named-tokens";
  { std::ofstream out(named); out << "reader read fixture-reader-token\n"; }
  const auto named_args = " --token-file '" + named.string() + "' --paper-journal '" +
                         (file.directory / "named" / "paper.jsonl").string() + "'";
  const auto named_result = run_daemon("--print-url" + named_args, "OPENPORT_WRITE_TOKEN='' ");
  EXPECT_EQ(named_result.status, 2);
  EXPECT_NE(named_result.output.find("named tokens are not selected automatically"), std::string::npos);
  EXPECT_EQ(run_daemon("--open --provider missing" + named_args, "OPENPORT_WRITE_TOKEN='' ").status, 2);
  EXPECT_FALSE(std::filesystem::exists(file.directory / "named" / "write-token"));
}

TEST(Cli, PrintUrlNamesAHostNameAddressAsGiven) {
  const auto result = run_daemon("--print-url --address localhost --write-token fixture");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output, "http://localhost:8080/#token=fixture\n");
  EXPECT_EQ(openport::startup::terminal_url("openport.internal", 9090, ""), "http://openport.internal:9090/");
  EXPECT_EQ(openport::startup::terminal_url("0.0.0.0", 8080, "t"), "http://localhost:8080/#token=t");
}

// A service's stdout may land in a log other users read, such as journald.
TEST(Cli, NoTokenLinkKeepsTheTokenOutOfTheStartupNotice) {
  const std::filesystem::path file = "/var/lib/openport/write-token";
  const auto linked = openport::startup::token_file_notice(file, "127.0.0.1", 8080, "secret-token", true);
  EXPECT_EQ(linked, "write token: kept in /var/lib/openport/write-token; to trade, open this once in a browser tab:\n"
                    "  http://127.0.0.1:8080/#token=secret-token\n");
  const auto unlinked = openport::startup::token_file_notice(file, "127.0.0.1", 8080, "secret-token", false);
  EXPECT_EQ(unlinked.find("secret-token"), std::string::npos) << unlinked;
  EXPECT_NE(unlinked.find("openportd --print-url --write-token-file /var/lib/openport/write-token "
                          "--address 127.0.0.1 --port 8080"), std::string::npos) << unlinked;
  // The flag parses; --print-url, run by a user who can read the token, still prints it.
  const auto result = run_daemon("--no-token-link --print-url --write-token fixture");
  EXPECT_EQ(result.status, 0) << result.output;
  EXPECT_EQ(result.output, "http://127.0.0.1:8080/#token=fixture\n");
}

TEST(Cli, BrowserReceivesOneLiteralUrlAndFailureLeavesItPrintable) {
  openport::test::RecordingFile file;
#ifdef __APPLE__
  const auto launcher = file.directory / "open";
#else
  const auto launcher = file.directory / "xdg-open";
#endif
  const auto output = file.directory / "arguments";
  { std::ofstream script(launcher); script << "#!/bin/sh\nprintf '%s\\n' \"$#\" \"$1\" > '" << output.string() << "'\n"; }
  std::filesystem::permissions(launcher, std::filesystem::perms::owner_all);
  const auto args = "--print-url --open --address 0.0.0.0 --write-token 'literal&$(echo bad)'";
  const auto env = "PATH='" + file.directory.string() + "' ";
  const auto result = run_daemon(args, env);
  EXPECT_EQ(result.status, 0) << result.output;
  std::ifstream in(output);
  const std::string actual(std::istreambuf_iterator<char>(in), {});
  EXPECT_EQ(actual, "1\nhttp://localhost:8080/#token=literal%26%24(echo%20bad)\n");
  { std::ofstream script(launcher); script << "#!/bin/sh\nexec /bin/sleep 5\n"; }
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(run_daemon(args, env).status, 0);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(4));
  std::filesystem::remove(launcher);
  const auto failed = run_daemon(args, env);
  EXPECT_EQ(failed.status, 1);
  EXPECT_NE(failed.output.find("http://localhost:8080/#token="), std::string::npos);
  EXPECT_NE(failed.output.find("open the printed link manually"), std::string::npos);
}

TEST(Cli, FatalSignalWritesPrivateLocalRecordAndPreservesSignalExit) {
  openport::test::RecordingFile file;
  const auto log = file.directory / "crash.log";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const rlimit core{0, 0};
    setrlimit(RLIMIT_CORE, &core);
    openport::startup::install_crash_log(log, "fixture-build");
    raise(SIGABRT);
    _exit(99);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGABRT);
  std::ifstream input(log);
  const std::string text(std::istreambuf_iterator<char>(input), {});
  EXPECT_EQ(text, "started fixture-build\nfatal SIGABRT\n");
  EXPECT_EQ(std::filesystem::status(log).permissions() & std::filesystem::perms::all,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  const auto link = file.directory / "link";
  std::filesystem::create_symlink(log, link);
  EXPECT_THROW(openport::startup::install_crash_log(link, "unused"), std::runtime_error);
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
  {
    // Without --symbols a recording of fewer symbols plays those, not the live defaults.
    test::RecordingFile spx, recorded;
    auto only = test::recording_header();
    only.subscription.underlyings = {"SPX"};
    test::record_events(spx.path, {}, only);
    const auto result = run_daemon("--provider replay --option file='" + spx.path.string() +
        "' --option speed=max --no-paper --record '" + recorded.path.string() + "' --address invalid-address");
    EXPECT_EQ(result.output.find("replay: unknown symbol"), std::string::npos) << result.output;
    EXPECT_EQ(md::RecordingReader(recorded.path).header().subscription.underlyings, std::vector<std::string>{"SPX"});
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
                                 "--option revision=1|2|3|4|5|6|7|8 (default 8)", "tradier tastytrade",
                                 "[--american-model lr|bs2002|baw]"})
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
  // Compaction rewrites every journal and has no dry run: nothing is touched.
  const auto journal = "--paper-journal '" + first.string() + "'";
  const auto files = [&] { return std::distance(std::filesystem::directory_iterator(directory.directory), std::filesystem::directory_iterator{}); };
  const auto before = files();
  const auto repaired_size = std::filesystem::file_size(first);
  rejects("openportd", "--compact-journals --dry-run " + journal, "require --repair-journals");
  rejects("openportd", "--compact-journals --file '" + second.string() + "' " + journal, "require --repair-journals");
  rejects("openportd", "--compact-journals --repair-journals --dry-run " + journal, "separate runs");
  EXPECT_EQ(files(), before);
  EXPECT_EQ(std::filesystem::file_size(first), repaired_size);
}

TEST(Cli, AnEmptyWriteTokenFileIsReplacedWithANewToken) {
  openport::test::RecordingFile directory;
  const auto token = directory.directory / "write-token";
  { std::ofstream empty(token); empty << "  \n"; }
  rejects("openportd", "--write-token-file '" + token.string() + "' --provider missing", "unknown provider");
  std::ifstream in(token);
  std::string line;
  std::getline(in, line);
  EXPECT_EQ(line.size(), 32U) << line;
  // No temporary file is left beside it.
  EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory.directory), std::filesystem::directory_iterator{}), 1);
}

TEST(Cli, RecordingRetentionRequiresDirectoryAndValidLimits) {
  for (const auto* value : {"-1", "nan", "inf", "1x", "1000001"})
    rejects("openportd", std::string("--record-max-gb ") + value, "--record-max-gb");
  for (const auto* value : {"-1", "1.5", "36501"})
    rejects("openportd", std::string("--record-keep-days ") + value, "--record-keep-days");
  rejects("openportd", "--record-max-gb 1", "requires --record-dir");
  rejects("openportd", "--record-keep-days 5 --record-dir recordings --record explicit.oprec", "without --record");
}

TEST(Cli, ClosedDateRangesExitSuccessfullyWithoutCredentialsOrDaemonStartup) {
  const std::string command = "MASSIVE_API_KEY='' " + isolated_home() + "\"" + OPENPORT_APPS_DIR +
      "/openportd\" --import-day massive --import-days 2026-09-05..2026-09-07 --date 2026-09-05 --symbols SPY 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[512];
  while (fgets(buffer, sizeof buffer, pipe)) output += buffer;
  const auto status = pclose(pipe);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0) << output;
  EXPECT_NE(output.find("day 3/3 2026-09-07 closed"), std::string::npos) << output;
  EXPECT_EQ(output.find("OpenPort on http"), std::string::npos);
}

namespace {
using namespace std::chrono_literals;

/// A loopback port nothing listens on now, for a daemon to take.
unsigned short free_port() {
  boost::asio::io_context io;
  const boost::asio::ip::tcp::endpoint loopback(boost::asio::ip::make_address("127.0.0.1"), 0);
  boost::asio::ip::tcp::acceptor acceptor(io, loopback);
  return acceptor.local_endpoint().port();
}

/// Arguments that serve a short replay, offline and without paper trading, on `port`.
std::string replay_server(const std::filesystem::path& recording, unsigned short port) {
  return "--provider replay --symbols SPX,SPY --option file='" + recording.string() +
         "' --option speed=max --option loop=off --no-paper --port " + std::to_string(port);
}

/// A server started through `sh -c`, which execs it, so the daemon keeps the shell's
/// pid. Its stdout and stderr share one pipe, read with deadlines so no test can hang.
class Background {
 public:
  Background(const std::string& command, const std::filesystem::path& directory) {
    int ends[2];
    if (::pipe(ends) != 0) throw std::runtime_error("pipe failed");
    pid_ = ::fork();
    if (pid_ < 0) {
      ::close(ends[0]);
      ::close(ends[1]);
      throw std::runtime_error("fork failed");
    }
    if (pid_ == 0) {
      // Started as from a terminal, whatever the test runner inherited: no signal
      // blocked, and SIGHUP with its default action.
      sigset_t none;
      sigemptyset(&none);
      ::sigprocmask(SIG_SETMASK, &none, nullptr);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"  // glibc and Darwin spell SIG_DFL as a C cast.
      std::signal(SIGHUP, SIG_DFL);
#pragma GCC diagnostic pop
      ::dup2(ends[1], STDOUT_FILENO);
      ::dup2(ends[1], STDERR_FILENO);
      ::close(ends[0]);
      ::close(ends[1]);
      if (::chdir(directory.c_str()) == 0) ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
      ::_exit(127);
    }
    ::close(ends[1]);
    output_ = ends[0];
  }
  Background(const Background&) = delete;
  Background& operator=(const Background&) = delete;
  ~Background() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      ::waitpid(pid_, nullptr, 0);
    }
    if (output_ >= 0) ::close(output_);
  }

  void send(int number) const {
    if (pid_ > 0) ::kill(pid_, number);
  }

  /// The rest of the first line starting with `prefix`, once the daemon has written it;
  /// nothing if its output ends or `limit` passes first.
  std::optional<std::string> line(std::string_view prefix, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
      const auto start = output.find(prefix);
      const auto end = start == std::string::npos ? start : output.find('\n', start);
      if (end != std::string::npos) return output.substr(start + prefix.size(), end - start - prefix.size());
      if (!take(deadline)) return std::nullopt;
    }
  }

  /// Its wait status once it has exited, after the rest of its output; nothing while it
  /// is still running after `limit`.
  std::optional<int> wait_exit(std::chrono::seconds limit) {
    if (pid_ <= 0) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (take(deadline)) {}
    do {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return status;
      }
      std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
  }

  std::string output;

 private:
  /// Appends what the daemon has written; false at the end of its output or the deadline.
  bool take(std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) return false;
    pollfd ready{};
    ready.fd = output_;
    ready.events = POLLIN;
    const int polled = ::poll(&ready, 1, static_cast<int>(left));
    if (polled <= 0) return polled < 0 && errno == EINTR;
    char buffer[4096];
    const auto count = ::read(output_, buffer, sizeof buffer);
    if (count <= 0) return count < 0 && errno == EINTR;
    output.append(buffer, static_cast<std::size_t>(count));
    return true;
  }

  pid_t pid_ = -1;
  int output_ = -1;
};

// B-71: a shell that finds openportd on PATH passes only its name as argv[0], which
// the lookup resolved against the working directory.
TEST(Cli, TheTerminalIsFoundBesideTheRunningBinaryNotTheWorkingDirectory) {
  using openport::startup::search_path;
  using openport::startup::web_root_beside;
  openport::test::RecordingFile file;
  const auto root = std::filesystem::weakly_canonical(file.directory);
  const auto page = [](const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream out(directory / "index.html");
    out << "<!doctype html>";
  };
  // Homebrew's layout: bin/openportd is a link into the Cellar, where share/ is.
  const auto keg = root / "Cellar/openport/1.0.0";
  std::filesystem::create_directories(keg / "bin");
  { std::ofstream script(keg / "bin/openportd"); script << "#!/bin/sh\n"; }
  std::filesystem::permissions(keg / "bin/openportd", std::filesystem::perms::owner_all);
  page(keg / "share/openport/web");
  std::filesystem::create_directories(root / "bin");
  std::filesystem::create_symlink("../Cellar/openport/1.0.0/bin/openportd", root / "bin/openportd");
  // An earlier PATH entry with a file of that name that cannot run is passed over.
  std::filesystem::create_directories(root / "first");
  { std::ofstream text(root / "first/openportd"); text << "not a program\n"; }
  const auto directories = (root / "first").string() + ":" + (root / "bin").string();
  EXPECT_EQ(search_path("openportd", directories), root / "bin/openportd");
  EXPECT_EQ(search_path("openportd", (root / "first").string()), std::filesystem::path{});
  EXPECT_EQ(search_path("bin/openportd", directories), std::filesystem::path("bin/openportd"));
  EXPECT_EQ(search_path("", directories), std::filesystem::path{});
  // The working directory's own web/dist is used only when nothing is beside the binary.
  const auto elsewhere = root / "elsewhere";
  page(elsewhere / "web/dist");
  EXPECT_EQ(web_root_beside(root / "bin/openportd", elsewhere), keg / "share/openport/web");
  EXPECT_EQ(web_root_beside(keg / "bin/openportd", elsewhere), keg / "share/openport/web");
  page(root / "checkout/web/dist");
  std::filesystem::create_directories(root / "checkout/build/apps");
  EXPECT_EQ(web_root_beside(root / "checkout/build/apps/openportd", elsewhere), root / "checkout/web/dist");
  EXPECT_EQ(web_root_beside(root / "first/openportd", elsewhere), elsewhere / "web/dist");
  EXPECT_EQ(web_root_beside({}, elsewhere), elsewhere / "web/dist");
  // The running binary is found whatever argv[0] says.
  const auto self = openport::startup::executable_path("openport_tests");
  EXPECT_EQ(self.filename(), "openport_tests") << self;
  EXPECT_TRUE(std::filesystem::is_regular_file(self)) << self;
}

// B-71: started by name from another directory, as a shell runs an installed openportd
// from PATH, it serves the terminal installed beside it.
TEST(WebServer, DaemonStartedByNameFromPathServesTheTerminalInstalledBesideIt) {
  openport::test::RecordingFile source;
  openport::test::record_events(source.path, {});
  const auto root = std::filesystem::weakly_canonical(source.directory);
  const auto prefix = root / "prefix";
  std::filesystem::create_directories(prefix / "bin");
  std::filesystem::create_directories(prefix / "share/openport/web");
  { std::ofstream page(prefix / "share/openport/web/index.html"); page << "<!doctype html><title>installed terminal</title>"; }
  // As cmake --install lays it out, with the built binary linked or copied into bin/.
  const std::filesystem::path built = std::string(OPENPORT_APPS_DIR) + "/openportd";
  const auto installed = prefix / "bin/openportd";
  std::error_code ec;
  std::filesystem::create_hard_link(built, installed, ec);
  if (ec) {
    std::filesystem::copy_file(built, installed);
    std::filesystem::permissions(installed, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
  }
  // A web/dist in the working directory, which the lookup by argv[0] served instead.
  const auto elsewhere = root / "elsewhere";
  std::filesystem::create_directories(elsewhere / "web/dist");
  { std::ofstream page(elsewhere / "web/dist/index.html"); page << "<!doctype html><title>wrong terminal</title>"; }
  const auto port = free_port();
  Background daemon("export " + isolated_home() + "PATH='" + (prefix / "bin").string() +
                    "':\"$PATH\"; unset OPENPORT_WEB_ROOT; exec openportd " + replay_server(source.path, port), elsewhere);
  const auto served = daemon.line("web terminal: ", 60s);
  ASSERT_TRUE(served.has_value()) << daemon.output;
  EXPECT_EQ(*served, (prefix / "share/openport/web").string()) << daemon.output;
  openport::net::HttpClient http;
  const auto shell = http.get("http://127.0.0.1:" + std::to_string(port) + "/");
  EXPECT_EQ(shell.status, 200) << shell.body;
  EXPECT_NE(shell.body.find("installed terminal"), std::string::npos) << shell.body;
  daemon.send(SIGTERM);
  const auto status = daemon.wait_exit(60s);
  ASSERT_TRUE(status.has_value()) << daemon.output;
  EXPECT_TRUE(WIFEXITED(*status)) << daemon.output;
}

// B-72: closing the terminal or SSH session sends SIGHUP, which stops the server as
// Ctrl-C does. With a --token-file it reloads the file instead, and under nohup, which
// ignores it, the server keeps running.
TEST(WebServer, DaemonStopsOnHangupUnlessItReloadsTokensOrRunsUnderNohup) {
  openport::test::RecordingFile source;
  openport::test::record_events(source.path, {});
  const auto tokens = source.directory / "tokens";
  { std::ofstream file(tokens); file << "reader read fixture-reader-token\n"; }
  const auto daemon_path = std::string(OPENPORT_APPS_DIR) + "/openportd";
  struct Case {
    std::string name, before, after;
    bool stops;
  };
  const std::vector<Case> cases{{"no token file", "", "", true},
                                {"token file", "", " --token-file '" + tokens.string() + "'", false},
                                {"nohup", "trap '' HUP; ", "", false}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.name);
    const auto port = free_port();
    Background daemon("export " + isolated_home() + "; " + item.before + "exec '" + daemon_path + "' " +
                      replay_server(source.path, port) + item.after, source.directory);
    ASSERT_TRUE(daemon.line("web terminal: ", 60s).has_value()) << daemon.output;
    daemon.send(SIGHUP);
    if (item.stops) {
      const auto stopped = daemon.wait_exit(60s);
      ASSERT_TRUE(stopped.has_value()) << "still running after SIGHUP\n" << daemon.output;
      ASSERT_TRUE(WIFEXITED(*stopped)) << daemon.output;
      EXPECT_EQ(WEXITSTATUS(*stopped), 0) << daemon.output;
      EXPECT_NE(daemon.output.find("shutting down"), std::string::npos) << daemon.output;
      continue;
    }
    // Still serving after several of its one-second ticks.
    EXPECT_FALSE(daemon.wait_exit(3s).has_value()) << daemon.output;
    openport::net::HttpClient http;
    EXPECT_EQ(http.get("http://127.0.0.1:" + std::to_string(port) + "/api/status").status, 200);
    daemon.send(SIGTERM);
    const auto status = daemon.wait_exit(60s);
    ASSERT_TRUE(status.has_value()) << daemon.output;
    EXPECT_TRUE(WIFEXITED(*status)) << daemon.output;
  }
}

// I-18: paper trading that cannot start is named on the console with its journal, not
// only in the terminal; startup otherwise goes on.
TEST(Cli, DaemonSaysWhyPaperTradingIsOffBeforeWebStartupFails) {
  openport::test::RecordingFile source;
  openport::test::record_events(source.path, {});
  const auto folder = source.directory / "folder.jsonl";
  std::filesystem::create_directories(folder);
  const auto damaged = source.directory / "damaged.jsonl";
  { std::ofstream out(damaged); out << "not a journal\n"; }
  const auto start = [&](const std::string& paper) {
    return run_daemon("--provider replay --symbols SPX,SPY --option file='" + source.path.string() +
                      "' --option speed=max " + paper + " --address invalid-address");
  };
  const std::vector<std::pair<std::filesystem::path, std::string>> journals{
      {folder, "JOURNAL_IO: "}, {damaged, "ACCOUNT_DAMAGED: Invalid journal JSON at record 1"}};
  for (const auto& [journal, reason] : journals) {
    const auto result = start("--paper-journal '" + journal.string() + "'");
    EXPECT_EQ(result.status, 2) << result.output;
    EXPECT_NE(result.output.find("openportd: paper trading is off (" + journal.string() + "): " + reason),
              std::string::npos) << result.output;
  }
  const auto off = start("--no-paper");
  EXPECT_EQ(off.status, 2) << off.output;
  EXPECT_EQ(off.output.find("paper trading is off"), std::string::npos) << off.output;
}
}  // namespace
