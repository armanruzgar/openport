// openportd: runs one market-data provider, the analytics engine and the web
// terminal's HTTP/WebSocket server.
//
//   openportd                                   # Cboe delayed SPX, SPY, QQQ, IWM and DIA, no key needed
//   openportd --provider databento --symbols SPX,QQQ
//   openportd --provider massive --symbols SPY --poll-seconds 5
//
// API keys come from the environment (DATABENTO_API_KEY, MASSIVE_API_KEY) and never
// leave this machine except to authenticate with that provider.

#include <algorithm>
#include <cstring>
#include <cerrno>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <openssl/rand.h>
#include <unistd.h>

#include "openport/analytics/volatility_history.hpp"
#include "openport/providers/cboe.hpp"
#include "openport/providers/demo_feed.hpp"
#include "openport/providers/factory.hpp"
#include "openport/providers/history.hpp"
#include "openport/providers/massive.hpp"
#include "openport/providers/demo.hpp"
#include "openport/providers/options.hpp"
#include "openport/server/api.hpp"
#include "openport/server/backtest.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "openport/server/web_server.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/web_policy.hpp"
#include "openport/trading/journal.hpp"
#include "openport/server/event_calendar.hpp"

namespace {

using namespace openport;

std::atomic<bool> g_stop{false};
std::atomic<bool> g_reload_tokens{false};

void on_signal(int signal) {
  if (signal == SIGHUP) g_reload_tokens = true;
  else g_stop = true;
}

struct Settings {
  md::ProviderConfig provider{"cboe", "", {}};
  bool explicit_symbols = false;
  md::Subscription subscription{{"SPX", "SPY", "QQQ", "IWM", "DIA"}, 0, 0.0};
  std::string address = "127.0.0.1";
  unsigned short port = 8080;
  std::filesystem::path web_root;
  std::filesystem::path scenario_dir;
  unsigned backtest_keep = 20;
  std::filesystem::path record_file;
  std::filesystem::path record_dir;
  std::optional<std::filesystem::path> candle_dir;
  bool history = true;
  bool series = true;
  bool force = false;
  std::optional<std::filesystem::path> series_dir;
  std::vector<std::filesystem::path> backfill_series;
  bool cboe_holidays = true;
  bool paper_enabled = true;
  bool compact_journals = false;
  bool repair_journals = false;
  bool repair_dry_run = false;
  std::filesystem::path repair_file;
  std::filesystem::path paper_journal;
  trading::SessionConfig paper;
  std::vector<trading::Dividend> dividends;
  std::vector<trading::PlanEvent> event_calendar;
  bool massive_dividends = false;
  bool dividends_source = false;
  std::vector<analytics::EventLabel> events;
  const server::PlanPreset* plan = server::find_plan("practice");
  std::optional<trading::Money> paper_cash;
  std::string paper_fill_model = "as_displayed";
  std::optional<int> paper_slippage_ticks, paper_fill_latency_ms, paper_impact_ticks;
  std::string write_token;
  std::filesystem::path write_token_file;
  std::filesystem::path notify_config;
  std::vector<server::NamedToken> tokens;
  std::filesystem::path token_file;
  bool require_token = false;
  server::Sandboxes::Options sandboxes;
  std::string client_ip_header;
  int threads = 2;
  double rate = 0.04;
  std::vector<std::string> allowed_origins;
  std::vector<std::string> allowed_hosts;
};

/// Without an error this is --help's answer: it goes to stdout and exits 0.
int usage(const char* error = nullptr) {
  auto* out = error ? stderr : stdout;
  if (error) std::fprintf(stderr, "openportd: %s\n\n", error);
  std::fprintf(
      out,
      "usage: openportd [--provider NAME] [--symbols SPX,SPY,QQQ,IWM,DIA] [--address ADDR] [--port N]\n"
      "                 [--scenario-dir DIR] [--web-root DIR] [--expiries N] [--window F] [--poll-seconds N]\n"
      "                 [--record FILE] [--record-dir DIR] [--rate R] [--option KEY=VALUE]... [--allowed-origin ORIGIN]...\n"
      "                 [--allowed-host NAME]... [--token-file FILE] [--require-token]\n"
      "                 [--notify-config FILE] [--backtest-keep N] (default 20, 0 unlimited)\n"
      "                 [--paper-journal PATH] [--plan ID] [--paper-cash DECIMAL] [--paper-fee DECIMAL]\n"
      "                 [--paper-fill-model as_displayed|conservative|midpoint]\n"
      "                 [--paper-slippage-ticks N] [--paper-fill-latency-ms N] [--paper-impact-ticks N]\n"
      "                 [--sandboxes N] [--sandbox-idle-seconds N] [--client-ip-header NAME]\n"
      "                 [--no-paper] [--write-token TOKEN] [--write-token-file PATH] [--candle-dir DIR] [--no-history]\n"
      "                 [--dividends FILE|massive] [--event-calendar FILE] [--events FILE] [--no-cboe-holidays]\n"
      "                 [--series-dir DIR] [--no-series]\n"
      "       openportd --backfill-series FILE... [--force] [--series-dir DIR]\n"
      "       openportd --verify-run JOURNAL\n"
      "       openportd --backtest PLAYBOOK[@VERSION][,PLAYBOOK[@VERSION]...] --plan PLAN (--days FILE | --recordings DIR | --scenarios N --seed S) --out REPORT.json\n"
      "                 [--playbooks FILE] [--paper-journal PATH] [--scenario-dir DIR] [--workers 1..16]\n"
      "       openportd --import-day databento|thetadata --date YYYY-MM-DD --symbols SPX,SPY\n"
      "                 [--expiries N] [--window F] [--out DIR] (default ./recordings)\n"
      "       openportd --compact-journals [--paper-journal PATH]\n"
      "       openportd --repair-journals [--dry-run] [--file PATH] [--paper-journal PATH]\n"
      "       openportd --version\n\n"
      "paper: durable paper trading on index, equity and ETF options; cash 100000, fee\n"
      "       0.65; more named accounts live in an accounts directory beside the journal\n"
      "compact: rewrite journals from builds before the compact format, keeping each\n"
      "         original as FILE.bak, then exit; stop openportd first\n"
      "repair: cut a torn last line, as a full disk leaves, off each journal, keeping the\n"
      "        original as FILE.torn-TIME, then exit; stop openportd first\n"
      "        --dry-run reports bytes cut and records kept without changes; --file selects one journal\n"
      "        exit 0: inspected/repaired; 1: a journal failed; 2: invalid options\n"
      "plan: rules for a new journal (presets below); default practice;\n"
      "      --paper-cash then overrides its starting balance\n"
      "fill model: seeds only a new main journal; numeric execution flags override the model\n"
      "            slippage/impact: 0..10 ticks, latency: 0..60000 ms; saved rules win on restart\n"
      "write token: --write-token overrides OPENPORT_WRITE_TOKEN; required for remote writes\n"
      "write token file: without either, the token kept in PATH, created at random if missing,\n"
      "                  with a link that saves it in a browser tab printed at startup\n"
      "dividends: SYMBOL,YYYY-MM-DD,AMOUNT lines (ex-date, dollars a share); on each ex-date\n"
      "           held shares receive the dividend and short shares pay it. \"massive\" reads\n"
      "           them for the stock and ETF symbols from Massive's API instead, every six\n"
      "           hours, with MASSIVE_API_KEY (any stocks plan), whatever the provider\n"
      "series: --series-dir DIR (default series beside candle-dir), --no-series\n"
      "        --backfill-series FILE... [--force] rebuilds recording minutes offline and exits\n"
      "events: YYYY-MM-DD,Label lines for volatility session moves; labels are 1-160 bytes\n"
      "rate: assumed flat zero rate in [-0.05, 0.25], default 0.04 (4%%)\n"
      "allowed origins: exact http[s]://host[:port], in addition to same-origin\n"
      "allowed hosts: names the server answers to besides IP addresses, localhost and the\n"
      "               allowed origins' hosts, such as a reverse proxy's upstream name\n"
      "databento: --expiries and --window must be 0 (whole-chain upstream subscription)\n"
      "tradier: --option sandbox=true for delayed data; poll interval follows request budget\n"
      "         --option option_size_unit=contracts only after confirming option size units\n"
      "tastytrade: production OAuth only; DXLink time units unknown unless confirmed with\n"
      "            --option dxlink_time_unit=milliseconds\n"
      "record: create a new compressed event file (existing files are never overwritten);\n"
      "        --record-dir names one per run by provider and start time there, and the\n"
      "        web terminal replays recordings from it (default ~/.openport/recordings)\n"
      "charts: one-minute bars persist in --candle-dir (default ~/.openport/candles; replay/demo\n"
      "        keeps them in memory); Cboe's free delayed chart history backfills them\n"
      "        unless --no-history (always off for replay/demo)\n"
      "holidays: Cboe's published holiday schedule is read daily, so a closure it announces\n"
      "          applies without a new build, unless --no-cboe-holidays (always off for replay/demo)\n"
      "replay: --option file=PATH [--option speed=1|10|60|max] [--option loop=on|off]\n"
      "demo: simulated regular sessions, rotating forever; --option days=ID,ID,...\n"
      "      --option speed=1|2|5|10|30|60|120|300 (default 1); no network services\n"
      "      --option revision=1|2|3|4|5|6|7 (default 7); older revisions keep prior symbols, calendar, timing, volume and quotes\n"
      "      symbols default to the selected days; journal defaults to ~/.openport/demo/paper-journal.jsonl\n"
      "providers:");
  for (auto name : providers::provider_names()) {
    std::fprintf(out, " %.*s", static_cast<int>(name.size()), name.data());
  }
  std::fprintf(out,
               "\nkeys are read from the environment: DATABENTO_API_KEY, MASSIVE_API_KEY,\n"
               "TRADIER_ACCESS_TOKEN, TASTYTRADE_CLIENT_SECRET, TASTYTRADE_REFRESH_TOKEN,\n"
               "and optionally TASTYTRADE_CLIENT_ID. No broker credential flags.\n");
  std::fprintf(out, "\nplan presets (--plan seeds any of them in a new journal; over HTTP, steps marked 'after X' need a pass of X first):\n");
  for (const auto& plan : server::plan_presets()) {
    std::fprintf(out, "  %s", plan.id.c_str());
    if (!plan.unlocked_by.empty()) std::fprintf(out, " (after %s)", plan.unlocked_by.c_str());
    std::fprintf(out, "\n");
  }
  return error ? 2 : 0;
}

std::vector<std::string> split(const std::string& list) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= list.size()) {
    const std::size_t comma = list.find(',', start);
    std::string item = list.substr(start, comma - start);
    for (char& c : item) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (!item.empty()) out.push_back(item);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

std::string env_key_for(std::string name) {
  for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const char* value = std::getenv((name + "_API_KEY").c_str());
  return value ? value : "";
}

/// The built web terminal: $OPENPORT_WEB_ROOT, next to the binary, or ./web/dist.
std::filesystem::path find_web_root(const char* argv0) {
  if (const char* env = std::getenv("OPENPORT_WEB_ROOT")) return env;
  std::error_code ec;
  const auto exe_dir = std::filesystem::weakly_canonical(argv0, ec).parent_path();
  for (const auto& candidate :
       {exe_dir / "web", exe_dir / "../../web/dist", exe_dir / "../share/openport/web",
        std::filesystem::current_path() / "web/dist"}) {
    if (std::filesystem::exists(candidate / "index.html", ec)) {
      return std::filesystem::weakly_canonical(candidate, ec);
    }
  }
  return std::filesystem::current_path() / "web/dist";
}

std::string size_text(std::uintmax_t bytes) {
  char text[32];
  if (bytes >= 1'000'000) std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / 1e6);
  else std::snprintf(text, sizeof text, "%.1f KB", static_cast<double>(bytes) / 1e3);
  return text;
}

int compact_journals(const std::filesystem::path& journal, const std::filesystem::path& accounts) {
  const auto results = server::compact_paper_journals(journal, accounts);
  if (results.empty()) std::printf("no paper journals at %s\n", journal.c_str());
  bool failed = false;
  for (const auto& r : results) {
    if (!r.error.empty()) {
      failed = true;
      std::fprintf(stderr, "%s: left as it was: %s\n", r.file.c_str(), r.error.c_str());
    } else if (r.backup.empty()) {
      std::printf("%s: already compact (%s)\n", r.file.c_str(), size_text(r.bytes_before).c_str());
    } else {
      std::printf("%s: %s -> %s; the original is %s\n", r.file.c_str(), size_text(r.bytes_before).c_str(),
                  size_text(r.bytes_after).c_str(), r.backup.filename().c_str());
    }
  }
  return failed ? 1 : 0;
}

/// --repair-journals: cuts torn last lines off the main journal and the accounts' ones.
int repair_journals(const std::filesystem::path& journal, const std::filesystem::path& accounts,
                    bool dry_run, const std::filesystem::path& single) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!single.empty()) files = {single};
  else {
    if (std::filesystem::is_regular_file(journal, ec)) files.push_back(journal);
    std::vector<std::filesystem::path> named;
    for (const auto& directory : {accounts, std::filesystem::absolute(journal).parent_path() / "replays"}) {
      if (!std::filesystem::is_directory(directory, ec)) continue;
      for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        if (entry.path().extension() == ".jsonl") named.push_back(entry.path());
    }
    std::sort(named.begin(), named.end());
    files.insert(files.end(), named.begin(), named.end());
  }
  if (files.empty()) std::printf("no paper journals at %s\n", journal.c_str());
  bool failed = false;
  for (const auto& file : files) {
    try {
      const auto repaired = trading::FileJournal::repair(file.string(), dry_run);
      if (dry_run) {
        std::printf("%s: would cut %zu bytes; %zu records kept\n", file.c_str(), repaired.bytes_cut, repaired.records_kept);
        continue;
      }
      // An empty journal held no transaction: its account starts afresh.
      if (repaired.bytes_cut == 0) {
        std::printf("%s: %s\n", file.c_str(), repaired.empty ? "empty, holds no transaction" : "whole");
      } else {
        std::printf("%s: cut a torn last line (%zu bytes)%s; the original is %s\n", file.c_str(),
                    repaired.bytes_cut, repaired.empty ? ", leaving no transaction" : "",
                    std::filesystem::path(repaired.backup).filename().c_str());
      }
    } catch (const std::exception& error) {
      failed = true;
      std::fprintf(stderr, "%s: left as it was: %s\n", file.c_str(), error.what());
    }
  }
  return failed ? 1 : 0;
}

/// The write token kept in `path`: read when present, else created at random with
/// owner-only permissions, so a container keeps its token in its volume.
std::string load_write_token(const std::filesystem::path& path) {
  const auto read = [&] {
    std::ifstream in(path);
    std::string token;
    std::getline(in, token);
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) token.pop_back();
    return token;
  };
  if (auto token = read(); !token.empty()) return token;
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("cannot generate a write token");
  std::string token;
  for (const unsigned char b : bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    token += kHex[b >> 4];
    token += kHex[b & 15];
  }
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    if (errno == EEXIST) if (auto existing = read(); !existing.empty()) return existing;
    throw std::runtime_error("cannot create " + path.string() + ": " + std::strerror(errno));
  }
  const std::string line = token + "\n";
  const bool written = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!written) throw std::runtime_error("cannot write " + path.string());
  return token;
}

}  // namespace

int import_command(int argc, char** argv) {
  using namespace openport;
  providers::ImportDay request;
  bool has_date = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (i + 1 == argc) return usage("missing value for import option");
    const std::string value = argv[++i];
    if (arg == "--import-day") request.provider = value;
    else if (arg == "--date") { request.date = providers::import_date(value); has_date = true; }
    else if (arg == "--symbols") request.subscription.underlyings = split(value);
    else if (arg == "--expiries") request.subscription.max_expiries = providers::parse_integer(value, "--expiries");
    else if (arg == "--window") request.subscription.strike_window = providers::parse_fraction(value, "--window");
    else if (arg == "--out") request.output = value;
    else return usage("unknown import option");
  }
  if (!has_date) return usage("--import-day requires --date YYYY-MM-DD");
  const auto today = md::new_york_time(md::now()).date;
  (void)providers::history_windows(request, today);
  auto source = providers::make_history_source(request.provider);
  std::fprintf(stderr, "Importing %s %s; data stays in %s\n", request.provider.c_str(), md::format_date(request.date).c_str(), request.output.c_str());
  try {
    const auto path = providers::import_day(request, *source, today,
        [](std::size_t completed, std::size_t total, std::uint64_t events) {
          if (completed == 1 || completed % 20 == 0 || completed == total)
            std::fprintf(stderr, "import: %zu/%zu windows, %llu events\n", completed, total, static_cast<unsigned long long>(events));
        });
    std::printf("Imported %s\n", path.c_str());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openportd: %s\n", error.what());
    return 1;
  }
}

int run(int argc, char** argv) {
  Settings settings;
  settings.web_root = find_web_root(argv[0]);
  if (const auto* token = std::getenv("OPENPORT_WRITE_TOKEN")) settings.write_token = token;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--help" || arg == "-h") return usage();
    if (arg == "--version") { std::printf("openportd %s\n", OPENPORT_VERSION); return 0; }
    if (arg == "--require-token") { settings.require_token = true; continue; }
    if (arg == "--no-paper") { settings.paper_enabled = false; continue; }
    if (arg == "--no-series") { settings.series = false; continue; }
    if (arg == "--force") { settings.force = true; continue; }
    if (arg == "--backfill-series") {
      const auto before = settings.backfill_series.size();
      while (i + 1 < argc && !std::string_view(argv[i + 1]).starts_with("--"))
        settings.backfill_series.emplace_back(argv[++i]);
      if (settings.backfill_series.size() == before) return usage("--backfill-series requires recording files");
      continue;
    }
    if (arg == "--no-history") { settings.history = false; continue; }
    if (arg == "--no-cboe-holidays") { settings.cboe_holidays = false; continue; }
    if (arg == "--compact-journals") { settings.compact_journals = true; continue; }
    if (arg == "--dry-run") { settings.repair_dry_run = true; continue; }
    if (arg == "--repair-journals") { settings.repair_journals = true; continue; }
    if (!has_value) return usage(("missing value for " + arg).c_str());
    const std::string value = argv[++i];
    if (arg == "--file") { settings.repair_file = value; }
    else if (arg == "--provider") {
      settings.provider.name = value;
    } else if (arg == "--sandboxes") {
      settings.sandboxes.capacity = static_cast<std::size_t>(providers::parse_integer(value, "--sandboxes"));
    } else if (arg == "--sandbox-idle-seconds") {
      settings.sandboxes.idle = std::chrono::seconds(providers::parse_integer(value, "--sandbox-idle-seconds", 1));
    } else if (arg == "--client-ip-header") {
      if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
          })) return usage("--client-ip-header takes a header name");
      settings.client_ip_header = value;
    } else if (arg == "--symbols") {
      settings.subscription.underlyings = split(value);
      settings.explicit_symbols = true;
    } else if (arg == "--address") {
      settings.address = value;
    } else if (arg == "--port") {
      settings.port =
          static_cast<unsigned short>(providers::parse_integer(value, "--port", 1, 65535));
    } else if (arg == "--allowed-origin") {
      settings.allowed_origins.push_back(value);
    } else if (arg == "--allowed-host") {
      if (!server::host_allowed(value, {}, {value})) return usage("--allowed-host takes a host name");
      settings.allowed_hosts.push_back(value);
    } else if (arg == "--paper-journal") {
      if (value.empty()) return usage("--paper-journal requires a nonempty path");
      settings.paper_journal = value;
    } else if (arg == "--plan") {
      const auto* plan = server::find_plan(value);
      if (!plan) return usage("--plan must be practice or an evaluation or funded plan ID (see --help)");
      settings.plan = plan;
    } else if (arg == "--paper-cash") {
      settings.paper_cash = trading::Money::parse(value);
    } else if (arg == "--paper-fee") {
      settings.paper.fee_per_contract = trading::Money::parse(value);
      if (settings.paper.fee_per_contract < trading::Money{}) return usage("--paper-fee must be nonnegative");
    } else if (arg == "--paper-fill-model") {
      if (value != "as_displayed" && value != "conservative" && value != "midpoint")
        return usage("--paper-fill-model must be as_displayed, conservative or midpoint");
      settings.paper_fill_model = value;
    } else if (arg == "--paper-slippage-ticks") {
      settings.paper_slippage_ticks = providers::parse_integer(value, "--paper-slippage-ticks", 0, 10);
    } else if (arg == "--paper-fill-latency-ms") {
      settings.paper_fill_latency_ms = providers::parse_integer(value, "--paper-fill-latency-ms", 0, 60000);
    } else if (arg == "--paper-impact-ticks") {
      settings.paper_impact_ticks = providers::parse_integer(value, "--paper-impact-ticks", 0, 10);
    } else if (arg == "--write-token") {
      if (value.empty()) return usage("--write-token requires a nonempty token");
      settings.write_token = value;
    } else if (arg == "--notify-config") {
      if (value.empty()) return usage("--notify-config requires a nonempty path");
      settings.notify_config = value;
    } else if (arg == "--token-file") {
      settings.token_file = value;
      std::ifstream input(value);
      if (!input) return usage("Cannot read token file");
      settings.tokens = server::parse_token_file(std::string(std::istreambuf_iterator<char>(input), {}));
    } else if (arg == "--write-token-file") {
      if (value.empty()) return usage("--write-token-file requires a nonempty path");
      settings.write_token_file = value;
    } else if (arg == "--record") {
      if (value.empty()) return usage("--record requires a nonempty path");
      settings.record_file = value;
    } else if (arg == "--record-dir") {
      if (value.empty()) return usage("--record-dir requires a nonempty path");
      settings.record_dir = value;
    } else if (arg == "--series-dir") {
      if (value.empty()) return usage("--series-dir requires a nonempty path");
      settings.series_dir = value;
    } else if (arg == "--candle-dir") {
      if (value.empty()) return usage("--candle-dir requires a nonempty path");
      settings.candle_dir = value;
    } else if (arg == "--events") {
      std::ifstream file(value);
      if (!file) return usage(("--events: cannot read " + value).c_str());
      try { settings.events = analytics::parse_events(file); }
      catch (const std::exception& e) { return usage(e.what()); }
    } else if (arg == "--event-calendar") {
      std::ifstream file(value);
      if (!file) return usage(("--event-calendar: cannot read " + value).c_str());
      try { settings.event_calendar = trading::parse_event_calendar(file); }
      catch (const std::exception& error) { return usage(error.what()); }
    } else if (arg == "--dividends" && value == "massive") {
      settings.massive_dividends = true;
      settings.dividends_source = true;
    } else if (arg == "--dividends") {
      settings.dividends_source = true;
      std::ifstream file(value);
      if (!file) return usage(("--dividends: cannot read " + value).c_str());
      try {
        settings.dividends = trading::parse_dividends(file);
      } catch (const std::invalid_argument& error) {
        return usage(error.what());
      }
    } else if (arg == "--backtest-keep") {
      settings.backtest_keep = providers::parse_integer(value, "--backtest-keep");
    } else if (arg == "--scenario-dir") {
      settings.scenario_dir = value;
    } else if (arg == "--web-root") {
      settings.web_root = value;
    } else if (arg == "--expiries") {
      settings.subscription.max_expiries = providers::parse_integer(value, "--expiries");
    } else if (arg == "--window") {
      settings.subscription.strike_window = providers::parse_fraction(value, "--window");
    } else if (arg == "--rate") {
      std::istringstream input(value);
      input.imbue(std::locale::classic());
      input >> std::noskipws >> settings.rate;
      if (!input || input.peek() != std::char_traits<char>::eof() ||
          !std::isfinite(settings.rate) || settings.rate < -0.05 || settings.rate > 0.25)
        return usage("--rate must be a finite number in [-0.05, 0.25]");
    } else if (arg == "--poll-seconds") {
      settings.provider.options["poll_seconds"] = value;
    } else if (arg == "--option") {
      const std::size_t eq = value.find('=');
      if (eq == std::string::npos) return usage("--option takes KEY=VALUE");
      settings.provider.options[value.substr(0, eq)] = value.substr(eq + 1);
    } else {
      return usage(("unknown option " + arg).c_str());
    }
  }
  const bool demo = settings.provider.name == providers::kDemoProvider;
  if (settings.sandboxes.capacity && !demo) return usage("--sandboxes requires --provider demo");
  if (settings.sandboxes.capacity && !settings.paper_enabled) return usage("--sandboxes requires paper trading");
  const bool offline = demo || settings.provider.name == "replay";
  if (settings.paper_journal.empty()) {
    if (const auto* home = std::getenv("HOME"))
      settings.paper_journal = std::filesystem::path(home) /
          (demo ? ".openport/demo/paper-journal.jsonl" : ".openport/paper-journal.jsonl");
  }
  if (demo && settings.massive_dividends) return usage("demo: --dividends massive needs network access; use a file");
  std::filesystem::path candle_directory;
  if (settings.candle_dir) candle_directory = *settings.candle_dir;
  else if (const auto* home = std::getenv("HOME")) candle_directory = std::filesystem::path(home) / ".openport/candles";
  const auto series_directory = settings.series_dir.value_or(candle_directory.empty()
      ? std::filesystem::path{} : std::filesystem::absolute(candle_directory).parent_path() / "series");
  if (settings.force && settings.backfill_series.empty()) return usage("--force requires --backfill-series");
  if (!settings.backfill_series.empty()) {
    if (!settings.series) return usage("--backfill-series conflicts with --no-series");
    if (series_directory.empty()) return usage("specify --series-dir when HOME is unavailable");
    // Reject any simulated input before writing any of a multi-file invocation.
    for (const auto& file : settings.backfill_series) {
      md::RecordingReader reader(file);
      if (server::simulated_series_recording(reader.header().provider)) return usage("demo recordings are simulated and cannot be backfilled");
    }
    server::SeriesStore store(series_directory);
    analytics::AnalyticsOptions options;
    options.fallback_rate = settings.rate;
    std::size_t written = 0;
    for (const auto& file : settings.backfill_series)
      written += server::backfill_series(file, store, settings.force, options);
    const auto state = store.status(0);
    if (!state.last_error.empty()) { std::fprintf(stderr, "%s\n", state.last_error.c_str()); return 1; }
    std::printf("series: %zu minutes written to %s\n", written, series_directory.c_str());
    return 0;
  }
  // More named accounts live beside the main journal, one journal each.
  const auto paper_accounts = settings.paper_journal.empty()
      ? std::filesystem::path{} : std::filesystem::absolute(settings.paper_journal).parent_path() / "accounts";
  if (settings.compact_journals) {
    if (settings.paper_journal.empty()) return usage("HOME is unavailable; specify --paper-journal");
    return compact_journals(settings.paper_journal, paper_accounts);
  }
  if ((settings.repair_dry_run || !settings.repair_file.empty()) && !settings.repair_journals)
    throw std::invalid_argument("--dry-run and --file require --repair-journals");
  if (settings.repair_journals) {
    if (settings.paper_journal.empty()) return usage("HOME is unavailable; specify --paper-journal");
    return repair_journals(settings.paper_journal, paper_accounts, settings.repair_dry_run, settings.repair_file);
  }
  if (settings.require_token && settings.tokens.empty() && settings.write_token.empty() && settings.write_token_file.empty())
    return usage("--require-token needs a configured token");
  const bool token_from_file = settings.write_token.empty() && !settings.write_token_file.empty();
  if (token_from_file) settings.write_token = load_write_token(settings.write_token_file);
  for (const auto& token : settings.tokens)
    if (token.secret == settings.write_token) return usage("Named and legacy tokens must have different secrets");
  if (settings.subscription.underlyings.empty()) return usage("no symbols");
  settings.provider.api_key = env_key_for(settings.provider.name);
  if (settings.massive_dividends && env_key_for("massive").empty())
    return usage("--dividends massive needs MASSIVE_API_KEY");

  if (settings.paper_enabled && settings.paper_journal.empty())
    return usage("HOME is unavailable; specify --paper-journal or --no-paper");
  if (!demo) providers::validate_subscription(settings.provider.name, settings.subscription);
  // A SIGKILLed run leaves its generated days behind; nothing else would remove them.
  providers::remove_orphaned_demo_directories();
  auto provider = providers::make_provider(settings.provider);
  if (const auto* feed = dynamic_cast<providers::DemoProvider*>(provider.get())) {
    if (!settings.explicit_symbols) settings.subscription.underlyings = feed->symbols();
    feed->validate(settings.subscription);
  }

  // Each run records to its own file, named by provider and UTC start time.
  if (!settings.record_dir.empty() && settings.record_file.empty()) {
    std::filesystem::create_directories(settings.record_dir);
    auto stamp = md::format_timestamp(md::now()).substr(0, 19);  // 2026-09-23T20:15:30
    stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
    settings.record_file = settings.record_dir / (settings.provider.name + "-" + stamp + "Z.oprec");
  }
  server::CandleStore::Options candle_options;
  if (settings.candle_dir)
    candle_options.directory = *settings.candle_dir;
  else if (const auto* home = std::getenv("HOME"); home && !offline)
    candle_options.directory = std::filesystem::path(home) / ".openport/candles";
  const auto candles = std::make_shared<server::CandleStore>(candle_options);
  if (const auto error = candles->error(); !error.empty())
    std::fprintf(stderr, "openportd: %s\n", error.c_str());

  server::Engine::Options engine_options;
  const auto notification_config = server::load_notification_config(settings.notify_config, [](const char* name) {
    const auto* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
  });
  // Keep public channel settings visible; each engine applies its forwarding policy.
  if (!settings.sandboxes.capacity && !notification_config.channels.empty())
    engine_options.notifications = std::make_shared<server::Notifications>(notification_config,
        std::make_unique<net::HttpClient>(), server::Notifications::Options{});
  engine_options.candles = candles;
  if (settings.series && !offline)
    engine_options.series = std::make_shared<server::SeriesStore>(series_directory);
  engine_options.analytics.fallback_rate = settings.rate;
  engine_options.record_file = settings.record_file;
  engine_options.paper_enabled = settings.paper_enabled;
  engine_options.paper_journal = settings.paper_journal;
  engine_options.paper_accounts = paper_accounts;
  if (settings.sandboxes.capacity) engine_options.sandboxes = std::make_shared<server::Sandboxes>(settings.sandboxes);
  // Rules and cash seed new journals only; recovery restores the recorded configuration.
  settings.paper.rules = settings.plan->rules;
  const bool conservative = settings.paper_fill_model == "conservative";
  settings.paper.rules.slippage_ticks = settings.paper_slippage_ticks.value_or(conservative ? 1 : 0);
  settings.paper.rules.fill_latency_ms = settings.paper_fill_latency_ms.value_or(conservative ? 1000 : 0);
  settings.paper.rules.impact_ticks = settings.paper_impact_ticks.value_or(conservative ? 1 : 0);
  settings.paper.rules.inside_fill_percent = settings.paper_fill_model == "midpoint" ? 50 : 0;
  trading::validate_rules(settings.paper.rules);
  settings.paper.initial_cash = settings.paper_cash.value_or(settings.plan->initial_cash);
  if (settings.paper.initial_cash <= trading::Money{}) return usage("--paper-cash must be positive");
  engine_options.paper = settings.paper;
  engine_options.dividends = settings.dividends;
  engine_options.demo_dividends = !settings.dividends_source;
  engine_options.events = settings.events;
  engine_options.write_mode = server::write_mode({settings.address, settings.write_token, settings.allowed_origins, settings.tokens, settings.require_token, engine_options.sandboxes, settings.client_ip_header});
  server::Engine engine(*provider, settings.subscription, engine_options);
  engine.start();
  std::unique_ptr<providers::CboeChartHistory> history;
  if (settings.history && !offline) {
    history = std::make_unique<providers::CboeChartHistory>(
        settings.subscription.underlyings,
        [candles](const std::string& symbol, providers::CboeChart chart, std::vector<md::Bar> bars) {
          if (chart == providers::CboeChart::Intraday)
            candles->merge_minutes(symbol, bars);
          else
            candles->merge_days(symbol, bars);
        });
    history->start();
  }
  std::unique_ptr<providers::CboeChartHistory> proxy_history;
  if (settings.history && settings.series && !offline) {
    std::vector<std::string> indices;
    for (const auto& symbol : settings.subscription.underlyings) {
      const auto index = analytics::iv_proxy(symbol);
      if (!index.empty() && std::find(indices.begin(), indices.end(), "_" + index) == indices.end())
        indices.push_back("_" + index);
    }
    // SPY's ex-post estimate uses SPX closes and is labelled as a proxy.
    if (std::find(settings.subscription.underlyings.begin(), settings.subscription.underlyings.end(), "SPY") != settings.subscription.underlyings.end() &&
        std::find(settings.subscription.underlyings.begin(), settings.subscription.underlyings.end(), "SPX") == settings.subscription.underlyings.end())
      indices.push_back("SPX");
    providers::CboeChartHistory::Options options;
    options.daily_only = true;
    proxy_history = std::make_unique<providers::CboeChartHistory>(indices,
        [candles](const std::string& symbol, providers::CboeChart, std::vector<md::Bar> bars) {
          candles->merge_days(symbol.starts_with('_') ? symbol.substr(1) : symbol, bars);
        }, options);
    proxy_history->start();
  }
  // Closures the exchange announces reach the calendar without a new build.
  std::unique_ptr<providers::CboeHolidaySchedule> holidays;
  if (settings.cboe_holidays && !offline) {
    holidays = std::make_unique<providers::CboeHolidaySchedule>(md::set_scheduled_days);
    holidays->start();
  }
  const auto trading_status = engine.status().trading;
  if (trading_status.reason.starts_with("JOURNAL_LOCKED:"))
    std::fprintf(stderr, "openportd: %s\n", trading_status.reason.c_str());
  // Recorded days replay beside the live feed, each with its own paper account.
  server::ReplayHost::Options replay_options;
  replay_options.engine = engine_options;
  replay_options.settings_source = [&engine](std::string_view id) { return engine.trading_view(id); };
  replay_options.scenario_dir = settings.scenario_dir;
  if (!settings.record_dir.empty())
    replay_options.recordings = settings.record_dir;
  else if (const auto* home = std::getenv("HOME"))
    replay_options.recordings = std::filesystem::path(home) / ".openport/recordings";
  server::ReplayHost replays(replay_options);
  server::BacktestHost backtests({settings.paper_journal.parent_path() / "backtests", replay_options.recordings,
      settings.scenario_dir, engine_options.analytics, engine_options.dividends,
      settings.paper_enabled && engine_options.write_mode != "disabled", settings.backtest_keep});
  // Dividends from Massive reach the live engine and replays started after them.
  std::unique_ptr<providers::MassiveDividends> dividends;
  if (settings.massive_dividends) {
    std::vector<std::string> tickers;
    for (const auto& symbol : settings.subscription.underlyings)
      if (!md::is_index_underlying(symbol)) tickers.push_back(symbol);
    providers::MassiveDividends::Options options;
    options.api_key = env_key_for("massive");
    dividends = std::make_unique<providers::MassiveDividends>(
        tickers,
        [&engine, &replays](std::vector<providers::MassiveDividend> found) {
          // Two distributions going ex together are paid as one.
          std::map<std::pair<std::string, md::Date>, trading::Money> amounts;
          for (const auto& d : found) {
            try {
              auto& amount = amounts[{d.ticker, d.ex_date}];
              amount = amount + trading::Money::from_double(d.cash_amount);
            } catch (const std::exception&) {
            }
          }
          std::vector<trading::Dividend> list;
          for (const auto& [key, amount] : amounts)
            if (amount > trading::Money{}) list.push_back({key.first, key.second, amount});
          engine.set_dividends(list);
          replays.set_dividends(std::move(list));
        },
        options);
    dividends->start();
  }
  const auto token_file = settings.token_file.empty() ? nullptr : std::make_shared<server::TokenFile>(settings.token_file, settings.write_token);
  server::WebServer web(
      settings.address, settings.port, settings.web_root,
      [&engine, &replays, &backtests, &settings](const server::ApiRequest& request, server::ApiCompletion complete) {
        if (auto response = server::event_calendar_read(request, settings.event_calendar)) { complete(std::move(*response)); return; }
        if (backtests.handle(request, engine, complete)) return;
        if (replays.handle(request, complete)) return;
        server::handle_api_async(request, engine, std::move(complete));
      }, settings.allowed_origins, settings.write_token, settings.allowed_hosts, settings.tokens, settings.require_token, engine_options.sandboxes, settings.client_ip_header, token_file);
  web.start(settings.threads);

  std::string symbols;
  for (const auto& symbol : settings.subscription.underlyings) {
    symbols += (symbols.empty() ? "" : ", ") + symbol;
  }
  std::printf("OpenPort on http://%s:%u  (provider %s: %s)\n", settings.address.c_str(), web.port(),
              settings.provider.name.c_str(), symbols.c_str());
  std::printf("web terminal: %s\n", settings.web_root.string().c_str());
  if (token_from_file) {
    std::printf("write token: kept in %s; to trade, open this once in a browser tab:\n"
                "  http://localhost:%u/#token=%s\n",
                settings.write_token_file.string().c_str(), web.port(), settings.write_token.c_str());
  }
  std::fflush(stdout);

  std::signal(SIGHUP, on_signal);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  std::string recording_error;
  std::string history_error, proxy_error, series_error;
  std::string holidays_error;
  std::string dividends_error;
  std::string candle_error;
  std::string breaker_error;
  auto report = [](const std::string& error, std::string& reported) {
    if (!error.empty() && error != reported) std::fprintf(stderr, "openportd: %s\n", error.c_str());
    reported = error;
  };
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (token_file && g_reload_tokens.exchange(false)) (void)token_file->reload();
    if (token_file) (void)token_file->snapshot();
    web.broadcast(server::tick_message(engine, {true, {}}));
    if (auto tick = replays.tick(); !tick.empty()) web.broadcast(tick);
    report(engine.recording_error(), recording_error);
    report(engine.status().circuit_breaker.error, breaker_error);
    if (history) report(history->error(), history_error);
    if (proxy_history) report(proxy_history->error(), proxy_error);
    if (engine_options.series) report(engine_options.series->status(md::now()).last_error, series_error);
    if (holidays) report(holidays->error(), holidays_error);
    if (dividends) report(dividends->error(), dividends_error);
    report(candles->error(), candle_error);
  }
  std::printf("\nshutting down\n");
  web.stop();
  replays.stop();
  if (history) history->stop();
  if (proxy_history) proxy_history->stop();
  if (holidays) holidays->stop();
  if (dividends) dividends->stop();
  engine.stop();
  candles->flush();
  if (!settings.record_file.empty()) {
    const auto stats = engine.recording_stats();
    const double count = static_cast<double>(stats.events);
    std::printf("recording: %llu events, %llu bytes, %.1f ns/event publish, %.2f bytes/event\n",
                static_cast<unsigned long long>(stats.events),
                static_cast<unsigned long long>(stats.bytes),
                count > 0 ? static_cast<double>(stats.publish_nanoseconds) / count : 0,
                count > 0 ? static_cast<double>(stats.bytes) / count : 0);
    if (const auto error = engine.recording_error(); !error.empty()) {
      std::fprintf(stderr, "openportd: %s\n", error.c_str());
      return 1;
    }
  }
  return 0;
}

namespace {
int backtest_cli(int argc, char** argv) {
  using nlohmann::json;
  std::map<std::string, std::string> args;
  for (int index = 1; index < argc; ++index) {
    const std::string key = argv[index];
    if (key != "--backtest" && key != "--plan" && key != "--days" && key != "--recordings" &&
        key != "--scenarios" && key != "--seed" && key != "--out" && key != "--playbooks" &&
        key != "--paper-journal" && key != "--scenario-dir" && key != "--workers")
      throw std::invalid_argument("Unknown backtest option: " + key);
    if (++index == argc || !args.emplace(key, argv[index]).second) throw std::invalid_argument("Missing or repeated option: " + key);
  }
  if (!args.contains("--plan") || !args.contains("--out") || args.at("--out").empty())
    throw std::invalid_argument("Backtests require --plan and --out");
  if (args.contains("--days") + args.contains("--recordings") + args.contains("--scenarios") != 1)
    throw std::invalid_argument("Choose --days, --recordings or --scenarios");
  const auto read = [](const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) throw std::invalid_argument("Cannot read " + file.string());
    return json::parse(input);
  };
  std::filesystem::path catalogue;
  if (args.contains("--playbooks")) catalogue = args.at("--playbooks");
  else if (args.contains("--paper-journal")) catalogue = std::filesystem::path(args.at("--paper-journal")).parent_path() / "playbooks.json";
  else if (const auto* home = std::getenv("HOME")) catalogue = std::filesystem::path(home) / ".openport/playbooks.json";
  else throw std::invalid_argument("Supply --playbooks FILE");
  json body{{"plan", args.at("--plan")}};
  const auto& selectors = args.at("--backtest");
  if (selectors.find(',') == std::string::npos) body["playbook"] = selectors;
  else {
    body["playbooks"] = json::array();
    std::size_t start = 0;
    do {
      const auto end = selectors.find(',', start);
      body["playbooks"].push_back(selectors.substr(start, end == std::string::npos ? end : end - start));
      if (end == std::string::npos) break;
      start = end + 1;
    } while (start <= selectors.size());
  }
  if (!server::find_plan(args.at("--plan"))) body["plan"] = read(args.at("--plan"));
  if (args.contains("--workers")) body["workers"] = json::parse(args.at("--workers"));
  std::filesystem::path base;
  if (args.contains("--days")) {
    body["days"] = read(args.at("--days"));
    base = std::filesystem::absolute(args.at("--days")).parent_path();
  } else if (args.contains("--recordings")) {
    base = std::filesystem::absolute(args.at("--recordings"));
    std::vector<std::pair<md::Timestamp, std::string>> files;
    for (const auto& entry : std::filesystem::directory_iterator(base)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".oprec") continue;
      md::RecordingReader reader(entry.path());
      files.emplace_back(reader.header().started, entry.path().filename().string());
    }
    std::sort(files.begin(), files.end());
    body["days"] = json::array();
    for (const auto& [time, file] : files) { (void)time; body["days"].push_back({{"file", file}}); }
  } else {
    body["scenarios"] = json::parse(args.at("--scenarios"));
    if (!args.contains("--seed")) throw std::invalid_argument("--scenarios requires --seed");
    body["seed"] = args.at("--seed");
  }
  if (args.contains("--seed") && !args.contains("--scenarios")) throw std::invalid_argument("Use per-day seeds in --days");
  const auto request = server::parse_backtest(body, read(catalogue),
      providers::load_scenarios(args.contains("--scenario-dir") ? std::filesystem::path(args.at("--scenario-dir")) : std::filesystem::path{}), base, false);
  const auto output = std::filesystem::absolute(args.at("--out"));
  std::signal(SIGHUP, on_signal);
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  const auto report = server::run_backtest(request, output.string() + ".d", g_stop);
  server::write_backtest_report(output, report);
  std::fprintf(stdout, "%s\nReport: %s\n", std::string(server::kBacktestLabel).c_str(), output.c_str());
  return report.at("status") == "completed" ? 0 : report.at("status") == "cancelled" ? 130 : 1;
}
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--verify-run") {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(argv[2], ec);
    if (!ec) {
      const auto estimate = openport::server::verification_cost(bytes);
      std::fprintf(stderr, "Verification estimate: %llu seconds or more (hardware and input dependent)\n",
          static_cast<unsigned long long>(estimate.at("estimated_seconds").get<std::uint64_t>()));
      if (!estimate.at("warning").is_null()) std::fprintf(stderr, "%s\n", estimate.at("warning").get<std::string>().c_str());
    }
    const auto result = openport::server::verify_run(argv[2]);
    std::fprintf(result.matched ? stdout : stderr, "%s\n", result.message.c_str());
    return result.matched ? 0 : 1;
  }
  try {
    for (int index = 1; index < argc; ++index)
      if (std::string_view(argv[index]) == "--backtest") return backtest_cli(argc, argv);
    for (int i = 1; i < argc; ++i)
      if (std::string_view(argv[i]) == "--import-day") return import_command(argc, argv);
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "openportd: %s\n", error.what());
    return 2;
  } catch (...) {
    std::fprintf(stderr, "openportd: unknown startup failure\n");
    return 2;
  }
}
