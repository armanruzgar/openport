#include "openport/server/replay_host.hpp"

#include <algorithm>
#include <atomic>
#include <future>
#include <fstream>
#include <charconv>
#include <cstdio>
#include <random>
#include <map>
#include <limits>
#include <nlohmann/json.hpp>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include "openport/providers/demo.hpp"
#include "openport/providers/replay.hpp"
#include "openport/server/plans.hpp"
#include "openport/server/practice.hpp"
#include "openport/server/run.hpp"
#include "openport/server/playbooks.hpp"
#include "run_json.hpp"

namespace openport::server {
namespace {
using nlohmann::json;

/// A plain file name in the recordings directory: no directories, no hidden files.
bool plain_name(std::string_view name) {
  return !name.empty() && name.size() <= 255 && name.front() != '.' &&
         name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos;
}

json header_json(const md::RecordingHeader& header) {
  return {{"provider", header.provider}, {"imported", header.imported}, {"simulated", providers::simulated_provider(header.provider)}, {"symbols", header.subscription.underlyings},
          {"started", header.started > 0 ? json(md::format_timestamp(header.started)) : json(nullptr)},
          {"delay_seconds", header.capabilities.delay.count()}};
}

json recordings_json(const std::filesystem::path& directory) {
  json list = json::array();
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec)) return list;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
    if (std::filesystem::is_regular_file(entry.symlink_status(ec)) && plain_name(entry.path().filename().string()) &&
        entry.path().extension() != ".end" && !entry.path().string().ends_with(".end.tmp")) files.push_back(entry.path());
  for (const auto& file : files) {
    json item{{"file", file.filename().string()}, {"bytes", std::filesystem::file_size(file, ec)}};
    try {
      const md::RecordingReader reader(file);
      item.update(header_json(reader.header()));
      item["date"] = reader.header().started > 0 ? json(md::format_date(md::trading_date(reader.header().started))) : json(nullptr);
    } catch (const std::exception& error) {
      item["error"] = error.what();
    }
    list.push_back(std::move(item));
  }
  // Provider prefixes and arbitrary filenames must not group dates by source.
  std::sort(list.begin(), list.end(), [](const json& a, const json& b) {
    const auto start = [](const json& item) { return item.contains("started") && item["started"].is_string() ? item["started"].get<std::string>() : std::string{}; };
    const auto left = start(a), right = start(b);
    return left != right ? left > right : a["file"].get<std::string>() > b["file"].get<std::string>();
  });
  return list;
}

ApiResponse ok(const json& body, int status = 200) { return {status, body.dump()}; }

json parse_body(const ApiRequest& request, std::initializer_list<std::string_view> allowed) {
  if (request.body.size() > 64 * 1024) throw std::invalid_argument("Body exceeds 64 KiB");
  auto body = request.body.empty() ? json::object() : json::parse(request.body);
  if (!body.is_object()) throw std::invalid_argument("Expected a JSON object");
  for (auto it = body.begin(); it != body.end(); ++it)
    if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
      throw std::invalid_argument("Unknown field: " + it.key());
  return body;
}

/// Each session a scenario run plays: its kind, trading date, first and last snapshot.
json sessions_json(const std::vector<providers::ScenarioWindow>& windows) {
  json out = json::array();
  for (const auto& w : windows)
    out.push_back({{"session", w.session}, {"date", md::format_date(w.date)},
                   {"open", md::format_timestamp(w.first)}, {"end", md::format_timestamp(w.last)}});
  return out;
}
json demo_json(const providers::Scenario& d) {
  const auto windows = providers::scenario_windows(d, d.date);
  return {{"id", d.id}, {"title", d.title}, {"description", d.description}, {"goal", d.goal},
          {"session", d.overnight ? "overnight" : "regular"}, {"date", md::format_date(d.date)},
          {"seed", std::to_string(d.seed)}, {"generator", d.generator}, {"provider", providers::kDemoProvider},
          {"events", scenario_marks(d, d.date)}, {"objectives", drill_objectives(d, d.date)},
          {"symbols", d.symbols}, {"started", md::format_timestamp(providers::scenario_open(d, d.date))},
          {"sessions", sessions_json(windows)}, {"end", md::format_timestamp(windows.back().last)}};
}
const providers::Scenario& default_scenario(const std::vector<providers::Scenario>& scenarios) {
  const auto found = std::find_if(scenarios.begin(), scenarios.end(), [](const auto& s) { return s.id == "reversal"; });
  return found == scenarios.end() ? scenarios.front() : *found;
}
json demos_json(const std::vector<providers::Scenario>& scenarios) {
  json list = json::array();
  for (const auto& d : scenarios) list.push_back(demo_json(d));
  return list;
}
bool builtin_id(const std::string& id) {
  const auto& builtins = providers::builtin_scenarios();
  return std::any_of(builtins.begin(), builtins.end(), [&](const auto& s) { return s.id == id; });
}
std::uint64_t seed_field(const json& value, const providers::Scenario& scenario) {
  if (value == "scenario") return scenario.seed;
  if (value.is_string()) {
    const auto text = value.get<std::string>();
    std::uint64_t seed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
    if (error == std::errc{} && end == text.data() + text.size()) return seed;
  }
  if (value.is_number_unsigned() || (value.is_number_integer() && value.get<std::int64_t>() >= 0))
    return value.get<std::uint64_t>();
  throw std::invalid_argument("seed must be a uint64, its decimal string, or scenario");
}
std::uint64_t fresh_seed() {
  std::random_device random;
  return (static_cast<std::uint64_t>(random()) << 32) ^ random();
}
std::string slug(std::string_view source) {
  std::string out;
  for (const char c : source) {
    if (out.size() == 40) break;
    out += (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
  }
  return out.empty() ? "recording" : out;
}

/// A date and time, "YYYY-MM-DDTHH:MM[:SS]" (or with a space) in New York unless it
/// carries Z or a UTC offset; empty for a bare "HH:MM[:SS]".
std::optional<md::Timestamp> dated_time(std::string value, const char* field) {
  if (value.size() <= 8) return std::nullopt;
  // Seconds are optional: 2026-09-17T10:30 is 10:30:00.
  if (value.size() >= 16 && value[13] == ':' && (value.size() == 16 || value[16] != ':')) value.insert(16, ":00");
  const auto parsed = md::parse_datetime(value, md::Zone::NewYork);
  if (!parsed) throw std::invalid_argument(std::string(field) + " must be New York HH:MM, or a date and time such as 2026-09-17T10:30");
  return parsed;
}
/// A bare New York "HH:MM[:SS]" in a multi-session run: its first occurrence at or after
/// `from` inside one of the run's sessions.
md::Timestamp session_time(const std::string& value, const std::vector<providers::ScenarioWindow>& windows,
                           md::Timestamp from, const char* field) {
  const auto parsed = md::parse_datetime("2000-01-03T" + value + (value.size() == 5 ? ":00" : ""), md::Zone::Utc);
  if ((value.size() != 5 && value.size() != 8) || !parsed)
    throw std::invalid_argument(std::string(field) + " must be New York HH:MM, or a date and time such as 2026-09-17T10:30");
  const auto seconds = static_cast<int>(*parsed % md::kNanosPerDay / md::kNanosPerSecond);
  for (const auto& w : windows) {
    // A session spans at most two New York dates.
    for (const auto day : {md::new_york_time(w.first).date, md::new_york_time(w.last).date}) {
      const auto candidate = md::new_york_to_utc(day, seconds / 3600, seconds / 60 % 60, seconds % 60);
      if (candidate != md::kInvalidTimestamp && candidate >= from && candidate >= w.first && candidate <= w.last) return candidate;
    }
  }
  throw std::invalid_argument(std::string(field) + " " + value + " falls in none of this run's sessions at or after " +
                              md::format_timestamp(from) + "; give a date and time");
}

md::Timestamp control_time(std::string value, const std::vector<providers::ScenarioWindow>& windows,
                           const providers::ReplayProvider& provider, const char* field) {
  const auto current = std::max(provider.settled_through(), provider.market_time());
  if (value == "next") {
    const auto next = provider.next_time();
    if (next <= current) throw std::invalid_argument("No next snapshot; the recording has ended");
    return next;
  }
  if (value.starts_with('+')) {
    md::Timestamp seconds = 0;
    if (value.size() < 3) throw std::invalid_argument("Relative time must be +Ns, +Nm or +Nh (whole positive seconds, minutes or hours)");
    const auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size() - 1, seconds);
    const auto unit = value.back() == 's' ? 1 : value.back() == 'm' ? 60 : value.back() == 'h' ? 3600 : 0;
    if (error != std::errc{} || end != value.data() + value.size() - 1 || seconds <= 0 || unit == 0 ||
        seconds > (std::numeric_limits<md::Timestamp>::max() - current) / md::kNanosPerSecond / unit)
      throw std::invalid_argument("Relative time must be +Ns, +Nm or +Nh (whole positive seconds, minutes or hours)");
    return current + seconds * unit * md::kNanosPerSecond;
  }
  if (const auto dated = dated_time(value, field)) return *dated;
  // A bare time is its next occurrence at or after the replay's time.
  if (windows.size() > 1) return session_time(value, windows, current, field);
  const auto date = md::trading_date(provider.header().started);
  auto day = date;
  if (md::new_york_time(provider.header().started).date < date && value.substr(0, 5) >= "20:15")
    day = md::date_from_days(md::days_since_epoch(date) - 1);
  if (value.size() == 5) value += ":00";
  if (const auto parsed = md::parse_datetime(md::format_date(day) + "T" + value, md::Zone::NewYork)) return *parsed;
  throw std::invalid_argument(std::string(field) + " must be HH:MM[:SS], a dated time, +Ns/+Nm/+Nh or next");
}

md::Timestamp start_time(const std::string& value, const std::vector<providers::ScenarioWindow>& windows,
                         md::Timestamp first, md::Date date, bool overnight) {
  if (const auto dated = dated_time(value, "start_at")) return *dated;
  if (windows.size() > 1) return session_time(value, windows, first, "start_at");
  if (overnight && value.substr(0, 5) >= "20:15") date = md::date_from_days(md::days_since_epoch(date) - 1);
  if (value.size() == 5 || value.size() == 8) {
    if (const auto parsed = md::parse_datetime(md::format_date(date) + "T" + value + (value.size() == 5 ? ":00" : ""), md::Zone::NewYork))
      return *parsed;
  }
  throw std::invalid_argument("start_at must be New York HH:MM[:SS], or a dated time");
}

void replay_gate(json& message, const providers::ReplayProvider& provider) {
  const bool preparing = provider.fast_forwarding(), stepping = provider.stepping(), finished = provider.finished();
  if (!preparing && !stepping && !finished) return;
  const auto* reason = preparing ? "REPLAY_FAST_FORWARD" : stepping ? "REPLAY_STEPPING" : "REPLAY_READ_ONLY";
  const auto gate = [&](json& trading) {
    if (trading.is_object() && trading.value("enabled", false)) {
      trading["write"] = "disabled";
      trading["reason"] = reason;
    }
  };
  if (message.contains("trading")) gate(message["trading"]);
  if (message.contains("accounts")) for (auto& account : message["accounts"]) gate(account["trading"]);
  if (message.contains("underlyings")) for (auto& underlying : message["underlyings"]) {
    if (!underlying.contains("paper") || !underlying["paper"].is_object()) continue;
    underlying["paper"]["accepting"] = false;
    underlying["paper"]["reason"] = reason;
    underlying["paper"]["message"] = preparing ? "Preparing replay start state" :
        stepping ? "Advancing to the until time" : "Finished replay is read-only";
  }
}

/// A history id that names no saved run: answered 404, never with a file path.
struct UnknownRun : std::runtime_error {
  explicit UnknownRun(const std::string& id) : std::runtime_error("No saved replay run " + id) {}
};

/// A plan's id from its display name, for runs whose metadata sidecar is missing.
std::string plan_id(const std::string& name) {
  for (const auto& plan : plan_presets()) if (plan.name == name) return plan.id;
  return name;
}

/// A saved run plays no further, so an order it left working or armed can never fill or
/// cancel: its archive shows it cancelled RUN_ENDED. The journal keeps the order as it was.
std::shared_ptr<const trading::TradingSnapshot> end_open_orders(std::shared_ptr<const trading::TradingSnapshot> snapshot) {
  if (snapshot->open_orders.empty()) return snapshot;
  auto ended = std::make_shared<trading::TradingSnapshot>(*snapshot);
  const trading::Decision reason{trading::Reason::RUN_ENDED, "The replay run ended with this order still open", {}, {}, {}};
  for (std::size_t i = 0; i < ended->recent_orders.size(); ++i) {
    if (!ended->recent_orders[i].open()) continue;
    auto& order = ended->recent_orders.mut(i);
    order.status = trading::OrderStatus::Cancelled;
    order.reason = reason;
  }
  ended->open_orders.clear();
  return ended;
}

}  // namespace

/// A small cache of generated (scenario, date, seed) recordings.
class ReplayHost::DemoRecordings {
 public:
  DemoRecordings() = default;
  DemoRecordings(const DemoRecordings&) = delete;
  DemoRecordings& operator=(const DemoRecordings&) = delete;
  ~DemoRecordings() {
    std::map<std::string, Attempt> days;
    {
      const std::lock_guard lock(mutex_);
      days.swap(days_);
    }
    for (auto& [day, attempt] : days) attempt.file.wait();
    std::error_code ec;
    if (!directory_.empty()) std::filesystem::remove_all(directory_, ec);
  }
  /// Starts generating the day in the background unless it is ready or under way.
  void prepare(const providers::Scenario& day) {
    { const std::lock_guard lock(mutex_); if (!days_.empty()) return; }
    (void)start(day, day.date, day.seed);
  }
  /// The day's recording: ready, awaited, or generated now. A failure is retried next time.
  std::filesystem::path get(const providers::Scenario& day, md::Date date, std::uint64_t seed, int revision = providers::kScenarioRevision) {
    const auto key = day.id + "|" + md::format_date(date) + "|" + std::to_string(seed) + "|" + std::to_string(revision);
    const auto attempt = start(day, date, seed, revision);
    try {
      return attempt.file.get();
    } catch (...) {
      const std::lock_guard lock(mutex_);
      if (const auto it = days_.find(key); it != days_.end() && it->second.number == attempt.number) days_.erase(it);
      throw;
    }
  }

 private:
  struct Attempt {
    unsigned number = 0;
    std::shared_future<std::filesystem::path> file;
  };
  Attempt start(const providers::Scenario& day, md::Date date, std::uint64_t seed, int revision = providers::kScenarioRevision) {
    const auto key = day.id + "|" + md::format_date(date) + "|" + std::to_string(seed) + "|" + std::to_string(revision);
    const std::lock_guard lock(mutex_);
    if (const auto it = days_.find(key); it != days_.end()) return it->second;
    if (directory_.empty()) {
      static std::atomic<unsigned> count{0};
      auto directory = std::filesystem::temp_directory_path() /
                       ("openport-demo-" + std::to_string(::getpid()) + "-" + std::to_string(++count));
      std::filesystem::remove_all(directory);
      std::filesystem::create_directory(directory);
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
      directory_ = std::move(directory);
    }
    // Each attempt writes its own file, so a retry never meets a failed one's.
    const unsigned number = ++attempts_;
    const auto path = directory_ / (std::to_string(number) + ".oprec");
    Attempt attempt{number, std::async(std::launch::async, [path, day, date, seed, revision] {
                              providers::write_scenario_recording(path, day, date, seed, revision);
                              return path;
                            }).share()};
    days_.emplace(key, attempt);
    // Open replay readers retain their file descriptor after eviction (POSIX).
    // Pending generators are never removed while they are writing.
    while (days_.size() > 4) {
      auto oldest = days_.end();
      for (auto it = days_.begin(); it != days_.end(); ++it)
        if (it->first != key && it->second.file.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
            (oldest == days_.end() || it->second.number < oldest->second.number)) oldest = it;
      if (oldest == days_.end()) break;
      try {
        const auto expired = oldest->second.file.get();
        std::filesystem::remove(expired);
        std::filesystem::remove(expired.string() + ".end");
      } catch (const std::exception&) {}
      days_.erase(oldest);
    }
    return attempt;
  }

  std::mutex mutex_;
  std::filesystem::path directory_;
  unsigned attempts_ = 0;
  std::map<std::string, Attempt> days_;
};

namespace {
int speed_field(const json& body) {
  const auto& value = body.at("speed");
  if (!value.is_number_integer() || value < 0 || value > 300 || !providers::ReplayProvider::valid_speed(value.get<int>()))
    throw std::invalid_argument("speed must be 0 (as fast as possible), 1, 2, 5, 10, 30, 60, 120 or 300");
  return value.get<int>();
}
}  // namespace

struct ReplayHost::Session {
  std::string file;
  bool demo = false;
  std::string id, scenario, seed, date, start_at, plan;
  json events = json::array(), objectives = json::array(), challenge = nullptr;
  int generator = 0;
  md::Timestamp target = 0;
  bool durable = false;
  mutable bool finalized = false;  // handoff_mutex_
  std::atomic<bool> restarting{false};
  json restarted_from = nullptr;
  /// A scenario run's sessions; empty for a recording.
  std::vector<providers::ScenarioWindow> windows;
  // The engine reads the provider, so it is declared after it and stops first.
  std::unique_ptr<providers::ReplayProvider> provider;
  std::unique_ptr<Engine> engine;

  [[nodiscard]] json state() const {
    const auto time = provider->time();
    json out = header_json(provider->header());
    out["file"] = file;
    out["demo"] = demo;
    out["id"] = id;
    out["scenario"] = scenario.empty() ? json(nullptr) : json(scenario);
    out["seed"] = seed.empty() ? json(nullptr) : json(seed);
    out["generator"] = generator == 0 ? json(nullptr) : json(generator);
    out["date"] = date;
    out["start_at"] = start_at;
    out["plan"] = plan;
    out["events"] = events;
    out["objectives"] = objectives;
    if (!challenge.is_null()) out["challenge"] = challenge;
    out["durable"] = durable;
    out["fast_forwarding"] = provider->fast_forwarding();
    out["stepping"] = provider->stepping() || restarting.load();
    if (!restarted_from.is_null()) out["restarted_from"] = restarted_from;
    out["skip_pending"] = provider->skip_pending();
    const auto pause_at = provider->pause_at();
    out["pause_at"] = pause_at > 0 ? json(md::format_timestamp(pause_at)) : json(nullptr);
    out["progress"] = target > provider->header().started ? std::clamp(
        static_cast<double>(time - provider->header().started) / static_cast<double>(target - provider->header().started), 0.0, 1.0) : 1.0;
    out["speed"] = provider->speed();
    out["paused"] = provider->paused();
    out["finished"] = provider->finished();
    const auto settled = provider->settled_through();
    out["settled_through"] = settled > 0 ? json(md::format_timestamp(settled)) : json(nullptr);
    out["time"] = time > 0 ? json(md::format_timestamp(time)) : json(nullptr);
    out["sessions"] = sessions_json(windows);
    out["end"] = windows.empty() ? json(nullptr) : json(md::format_timestamp(windows.back().last));
    return out;
  }
};

/// Read-only recovered accounts reuse all of the existing account and trade routes.
class ArchivedReplay final : public MetricsSource {
 public:
  explicit ArchivedReplay(const std::filesystem::path& file, json* diagnostic = nullptr) {
    const auto recovery = trading::FileJournal::read_prefix(file.string());
    integrity_ = {{"journal_found", {{"transactions", recovery.records.size()}, {"head", recovery.head},
                                      {"bytes", std::filesystem::file_size(file)}}}};
    if (recovery.truncated_final_line) {
      integrity_["torn"] = true;
      integrity_["bytes_cut"] = recovery.bytes_cut;
      integrity_["integrity_message"] = file.filename().string() + ": torn final line; --repair-journals would cut " +
          std::to_string(recovery.bytes_cut) + " bytes (stop the server first)";
    }
    const auto bytes = std::filesystem::file_size(file);
    const auto warning = journal_warning(bytes, recovery.records.size(), true);
    integrity_["journal_size"] = {{"bytes", bytes}, {"records", recovery.records.size()}, {"warning", warning.empty() ? json(nullptr) : json(warning)}};
    integrity_["verification_cost"] = verification_cost(file);
    if (diagnostic) *diagnostic = integrity_;
    if (!recovery.damage.empty() && !recovery.truncated_final_line)
      throw trading::TradingError(trading::Reason::JOURNAL_CORRUPT, recovery.damage);
    const auto session = trading::TradingSession::recover(recovery);
    view_ = std::make_shared<TradingView>();
    view_->snapshot = end_open_orders(session.snapshot());
    view_->config = session.config();
    view_->replay_start = view_->replay_end = view_->snapshot->time;
    view_->contracts = session.contracts();
    view_->valuations = session.valuations();
    // The equity history beside the journal, read as it is: an archive never compacts it.
    view_->equity_samples = read_equity_history(file.string() + ".equity.csv", view_->equity_error);
    if (!view_->equity_error.empty()) view_->equity_error_time = md::now();
    view_->journal_transactions = recovery.records.size();
    view_->journal_bytes = std::filesystem::file_size(file);
    view_->journal_head = recovery.head;
    // Replay definitions are journaled as inputs. Archives do not depend on the
    // current live catalogue or a mutable sidecar to explain historical trades.
    std::unique_ptr<Playbooks> playbooks;
    for (const auto& record : recovery.records) {
      if (record.type != "run_input") continue;
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events")) {
        if (event.at("type") != "run_input") continue;
        const auto& input = event.at("payload");
        if (input.at("kind") == "start") {
          playbooks = std::make_unique<Playbooks>(std::filesystem::path{}, input.value("playbooks", json(nullptr)));
          view_->run = run_identity(input.at("input").dump(), file.stem().string());
          view_->opening_settlement = input.value("driver", 1) >= 5;
        } else if (input.at("kind") == "command" && playbooks) {
          const auto& request = input.at("command");
          if (request.at("kind").get<int>() != static_cast<int>(TradingCommand::Kind::Playbook)) continue;
          const auto change = json::parse(request.at("note").get<std::string>());
          if (change.at("action") == "send" || change.at("action") == "dismiss") continue;
          // Invalid user commands are also recorded; leave the last valid catalogue.
          try { playbooks->change(change, "main", true); } catch (const std::exception&) {}
        }
      }
    }
    if (playbooks) view_->playbooks_json = playbooks->publication("main", false).dump();
    std::ifstream metadata(std::filesystem::path(file).replace_extension(".json"));
    // Only the simulated flag comes from the sidecar: a damaged one leaves the archive,
    // which rests on its journal, readable.
    if (metadata) {
      const auto parsed = json::parse(metadata, nullptr, false);
      simulated_ = parsed.is_object() && parsed.value("demo", false);
    }
  }
  const json& integrity() const { return integrity_; }
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  using MetricsSource::trading_view;  // the account-id overload answers for the main account
  std::shared_ptr<const TradingView> trading_view() const override { return view_; }
  md::Timestamp wall_time() const override { return view_->snapshot->time; }
  EngineStatus status() const override {
    EngineStatus out;
    out.provider = simulated_ ? "replay (demo)" : "replay archive";
    out.trading.enabled = true;
    out.trading.reason = "REPLAY_READ_ONLY";
    out.trading.write = "disabled";
    out.trading.account_version = view_->snapshot->account_version;
    out.trading.initial_cash = view_->config.initial_cash;
    out.trading.fee_per_contract = view_->config.fee_per_contract;
    out.trading.plan = view_->config.rules.plan;
    const auto& e = view_->snapshot->evaluation;
    out.trading.plan_id = preset_id(view_->config.initial_cash,
        trading::original_program_rules(view_->config.rules, e.starting_balance, e.size_scaling));
    out.accounts.push_back({"main", "Replay (read-only)", out.trading, 0, false, {}, view_->journal_bytes, view_->journal_transactions, true});
    return out;
  }
 private:
  std::shared_ptr<TradingView> view_;
  bool simulated_ = false;
  json integrity_;
};

class ReplayHost::History {
 public:
  explicit History(const Engine::Options& options)
      : directory_(options.paper_journal.empty() ? std::filesystem::path{} :
          std::filesystem::absolute(options.paper_journal).parent_path() / "replays"),
        writable_(options.paper_enabled && options.write_mode != "disabled" && !directory_.empty()) {}
  bool writable() const { return writable_; }
  std::filesystem::path create(const Session& session, const trading::SessionConfig&) {
    if (!writable_) return {};
    std::filesystem::create_directories(directory_);
    const auto path = directory_ / (session.id + ".jsonl");
    // The journal's exclusive create also prevents overwriting an earlier run.
    if (std::filesystem::exists(path)) throw std::runtime_error("Replay journal already exists");
    const auto data = session.state().dump();
    trading::check_storage_space(path, data.size() + 1);
    std::ofstream metadata(directory_ / (session.id + ".json"));
    metadata << data << '\n';
    metadata.close();
    if (!metadata) throw std::runtime_error("Cannot write replay metadata for " + session.id);
    return path;
  }
  /// Rewrites a retired run's metadata with its final playback state, which then
  /// reads finished. An interruption leaves the last start/resume state.
  void finish(const Session& session) const {
    if (!writable_ || !session.durable || session.id.empty() || session.finalized) return;
    const bool finalized = session.provider->finished();
    auto value = session.state();
    value["finished"] = finalized;
    if (finalized && !session.objectives.empty()) {
      const auto recovery = trading::FileJournal::read(journal(session.id).string());
      auto grade = grade_drill(recovery, session.objectives);
      const bool full = !session.windows.empty() && session.provider->settled_through() >= session.windows.back().last && run_from_open(recovery);
      grade["complete_run"] = full;
      if (!full) grade["score"] = nullptr;
      value["drill"] = std::move(grade);
      value["completed_at"] = md::format_timestamp(md::now());
    }
    if (finalized) {
      const auto view = session.engine->trading_view();
      if (!view || view->journal_head.empty()) return;
      value["journal"] = {{"transactions", view->journal_transactions}, {"head", view->journal_head},
                          {"bytes", std::filesystem::file_size(journal(session.id))}};
    }
    save(session.id, value);
    session.finalized = finalized;
  }
  void save(const std::string& id, const json& value) const {
    const auto file = directory_ / (id + ".json");
    const auto staged = directory_ / (id + ".json.tmp");
    const auto data = value.dump();
    trading::check_storage_space(file, data.size() + 1);
    std::error_code ec;
    {
      std::ofstream metadata(staged);
      metadata << data << '\n';
      metadata.close();
      if (!metadata) {
        std::filesystem::remove(staged, ec);
        throw std::runtime_error("Cannot write replay metadata for " + id);
      }
    }
    std::filesystem::rename(staged, file, ec);
    if (ec) {
      std::filesystem::remove(staged, ec);
      throw std::runtime_error("Cannot replace replay metadata for " + id);
    }
  }
  /// A saved run's metadata as its last start or retirement wrote it, or null.
  json metadata(const std::string& id) const {
    if (directory_.empty() || !plain_name(id)) return nullptr;
    std::ifstream file(directory_ / (id + ".json"));
    if (!file) return nullptr;
    return json::parse(file, nullptr, false);
  }
  /// Removes what a resumed run rebuilds as it re-executes: its equity history and
  /// playbook catalogue, which would otherwise hold the later state it reaches again.
  /// A resume rebuilds the run's equity history and playbook catalogue: they are set
  /// aside until it is known to work, then dropped, or put back if it does not.
  void set_aside(const std::string& id) const {
    std::error_code ignored;
    for (const auto* suffix : {".playbooks.json", ".jsonl.equity.csv"}) {
      const auto path = directory_ / (id + suffix);
      std::filesystem::remove(path.string() + ".resuming", ignored);
      std::filesystem::rename(path, path.string() + ".resuming", ignored);
    }
  }
  void restore(const std::string& id) const {
    std::error_code ignored;
    for (const auto* suffix : {".playbooks.json", ".jsonl.equity.csv"}) {
      const auto path = directory_ / (id + suffix);
      const auto aside = path.string() + ".resuming";
      if (std::filesystem::exists(aside, ignored)) std::filesystem::rename(aside, path, ignored);
      else std::filesystem::remove(path, ignored);
    }
  }
  void drop_aside(const std::string& id) const {
    std::error_code ignored;
    for (const auto* suffix : {".playbooks.json", ".jsonl.equity.csv"})
      std::filesystem::remove((directory_ / (id + suffix)).string() + ".resuming", ignored);
  }
  /// A saved run's journal, or empty when there is none by that id.
  std::filesystem::path journal(const std::string& id) const {
    if (directory_.empty() || !plain_name(id)) return {};
    auto path = directory_ / (id + ".jsonl");
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path{};
  }
  /// Removes what create and a run that failed to start left, so it lists no entry.
  void discard(const std::string& id) const {
    if (!writable_) return;
    std::error_code ignored;
    for (const auto* suffix : {".jsonl", ".json", ".playbooks.json", ".jsonl.equity.csv"})
      std::filesystem::remove(directory_ / (id + suffix), ignored);
  }
  std::shared_ptr<ArchivedReplay> open(const std::string& id) const {
    const auto path = journal(id);
    if (path.empty()) throw UnknownRun(id);
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    const auto bytes = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) throw UnknownRun(id);
    const std::lock_guard lock(cache_mutex_);
    const auto found = cache_.find(id);
    if (found != cache_.end() && found->second.modified == modified && found->second.bytes == bytes) return found->second.account;
    auto account = recover(path);
    summaries_[id] = {modified, bytes, summarize_archive(*account)};
    if (cache_.size() >= 16) cache_.erase(cache_.begin());
    cache_[id] = {modified, bytes, account};
    return account;
  }
  json list(const std::shared_ptr<Session>& active) const {
    json out = json::array();
    std::error_code ec;
    if (directory_.empty() || !std::filesystem::is_directory(directory_, ec)) return out;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec))
      if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") files.push_back(entry.path());
    std::sort(files.begin(), files.end(), std::greater<>());
    for (const auto& file : files) {
      const auto id = file.stem().string();
      const bool current = active && active->id == id;
      if (current && !active->provider->finished()) continue;
      json item{{"id", id}, {"file", id}, {"demo", false}, {"result", "open"}, {"pnl", nullptr}};
      bool finalized = current;
      bool resumable = current;  // a resume starts from the run's metadata
      try {
        if (current) finish(*active);
        {
          std::ifstream metadata(std::filesystem::path(file).replace_extension(".json"));
          if (metadata) { item.update(json::parse(metadata)); resumable = true; }
          finalized = item.value("finished", false);
        }
        item.update(summary(id, file));
        item.update(integrity(id, item));
        item["verification"] = verification(id);
      } catch (const std::filesystem::filesystem_error& error) {
        item["error"] = error.code().message();
      } catch (const std::exception& error) {
        if (!std::filesystem::exists(file, ec)) continue;  // deleted while listing
        item["error"] = error.what();
      }
      if (item.contains("drill") && (item.contains("error") || item.value("torn", false) || item.value("truncated", false) || item.value("mismatch", false))) {
        item["drill"]["score"] = nullptr;
        item["drill"]["complete_run"] = false;
      }
      item["id"] = id;  // A copied sidecar cannot redirect its journal's history routes.
      if (!item.contains("plan") && item.contains("plan_name")) item["plan"] = plan_id(item.at("plan_name").get<std::string>());
      // Interrupted metadata still holds the start state: playback stopped and
      // settled through its last journaled time.
      if (!finalized && item.contains("fast_forwarding")) {
        item["fast_forwarding"] = false;
        if (item.contains("stepping")) item["stepping"] = false;
        item["progress"] = 1.0;
        item["paused"] = true;
        item["settled_through"] = item.value("time", json(nullptr));
      }
      // A run interrupted by shutdown or a crash, with its start state intact, can
      // continue: POST /api/replay {"resume"}. A stopped or replaced run has ended.
      item["interrupted"] = !finalized && resumable && !item.contains("error");
      item["finished"] = true;
      item["read_only"] = true;
      out.push_back(std::move(item));
    }
    // Scores come from editable sidecars. A best counts as verified only when a passed
    // verification of its run, against the current journal, recomputed that same score.
    std::map<std::string, std::pair<double, bool>> best;
    for (const auto& run : out) if (run.contains("drill") && run.at("drill").at("score").is_number()) {
      const auto scenario = run.value("scenario", std::string());
      const auto score = run.at("drill").at("score").get<double>();
      const auto& verified = run.value("verification", json::object());
      const bool rechecked = verified.value("status", "") == "passed" && verified.contains("run") && verified.at("run").contains("drill") &&
          verified.at("run").at("drill").value("score", json(nullptr)) == score;
      auto [entry, added] = best.try_emplace(scenario, score, rechecked);
      if (!added && score > entry->second.first) entry->second = {score, rechecked};
      else if (!added && score == entry->second.first) entry->second.second = entry->second.second || rechecked;
    }
    for (auto& run : out) if (run.contains("scenario") && run.at("scenario").is_string() && best.contains(run.at("scenario").get<std::string>())) {
      const auto& [score, verified] = best.at(run.at("scenario").get<std::string>());
      run["personal_best"] = score;
      run["personal_best_verified"] = verified;
    }
    return out;
  }
  json stamp(const std::string& id) const {
    const auto file = journal(id);
    if (file.empty()) throw UnknownRun(id);
    return {{"bytes", std::filesystem::file_size(file)},
            {"modified", std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::filesystem::last_write_time(file).time_since_epoch()).count())}};
  }
  const std::string& verifying() const { return verifying_; }
  json verification(const std::string& id) const {
    auto saved = metadata(id);
    auto value = verifying_ == id ? verification_ : saved.is_object() ? saved.value("verification", json::object()) : json::object();
    if (value.empty()) value = {{"status", "idle"}, {"message", "This run has not been verified"}};
    if (value.at("status") == "idle") { value["cost"] = cost(id); return value; }
    if (!value.contains("stamp") || value.at("stamp") != stamp(id))
      return {{"status", "idle"}, {"message", "Journal changed since verification; verify it again"}, {"cost", cost(id)}};
    if (value.value("status", "") == "running" && verifying_ != id)
      return {{"status", "idle"}, {"message", "Verification was interrupted; verify it again"}, {"cost", cost(id)}};
    value.erase("stamp");
    value["cost"] = cost(id);
    return value;
  }
  json cost(const std::string& id) const {
    return verification_cost(journal(id));
  }
  void begin_verification(const std::string& id, const json& fingerprint) {
    auto value = metadata(id);
    if (!value.is_object()) value = json::object();
    const json running{{"status", "running"}, {"message", "Verifying " + id}, {"stamp", fingerprint}, {"build", OPENPORT_VERSION}};
    value["verification"] = running;
    save(id, value);
    verifying_ = id;
    verification_ = running;
  }
  void verification_progress(std::uint64_t done, std::uint64_t total) {
    verification_["transactions"] = done;
    verification_["progress"] = total ? static_cast<double>(done) / static_cast<double>(total) : 0.0;
  }
  /// Leaves the saved "running" status, which reads back as interrupted.
  void abandon_verification() {
    verifying_.clear();
    verification_ = json::object();
  }
  void end_verification(const std::string& id, json result, const json& fingerprint) {
    result["stamp"] = fingerprint;
    auto value = metadata(id);
    if (!value.is_object()) value = json::object();
    value["verification"] = result;
    verifying_.clear();
    verification_ = json::object();
    save(id, value);
  }
  json integrity(const std::string& id, const json& found) const {
    json out = json::object();
    for (const auto* key : {"journal_found", "torn", "bytes_cut", "integrity_message"})
      if (found.contains(key)) out[key] = found.at(key);
    const auto saved = metadata(id);
    if (saved.is_object() && saved.contains("journal")) {
      const auto& expected = saved.at("journal");
      out["journal"] = expected;
      if (found.contains("journal_found")) {
        const auto& actual = found.at("journal_found");
        if (expected.at("transactions") != actual.at("transactions") || expected.at("head") != actual.at("head") ||
            expected.at("bytes") != actual.at("bytes")) {
          out["mismatch"] = true;
          out["truncated"] = actual.at("transactions") < expected.at("transactions") || actual.at("bytes") < expected.at("bytes");
          std::string message = id + ": journal differs";
          bool described = false;
          const auto append = [&](std::string detail) {
            message += described ? "; " : "; ";
            message += std::move(detail);
            described = true;
          };
          if (expected.at("transactions") != actual.at("transactions"))
            append("expected " + expected.at("transactions").dump() + " transactions, found " + actual.at("transactions").dump());
          if (expected.at("head") != actual.at("head"))
            append("expected head " + expected.at("head").get<std::string>() + ", found " + actual.at("head").get<std::string>());
          if (expected.at("bytes") != actual.at("bytes"))
            append("expected " + expected.at("bytes").dump() + " bytes, found " + actual.at("bytes").dump());
          out["integrity_message"] = out.value("integrity_message", std::string()) +
              (out.contains("integrity_message") ? "; " : "") + message;
        }
      }
    }
    return out;
  }
  json details(const std::string& id) const {
    const auto value = summary(id, journal(id));
    auto out = integrity(id, value);
    if (value.contains("journal_size")) out["journal_size"] = value.at("journal_size");
    if (value.contains("verification_cost")) out["verification_cost"] = value.at("verification_cost");
    out["verification"] = verification(id);
    if (value.contains("error")) out["error"] = value.at("error");
    return out;
  }
  void remove(const std::string& id) const {
    if (!writable_) throw std::invalid_argument("Replay history is read-only");
    const auto file = journal(id);
    if (file.empty()) throw UnknownRun(id);
    // The writer's lock keeps another process's running replay; nothing is verified,
    // so an edited or torn journal is deleted too.
    trading::FileJournal::remove(file.string());
    std::error_code ec;
    for (const char* sidecar : {".json", ".playbooks.json", ".jsonl.equity.csv"})
      std::filesystem::remove(directory_ / (id + sidecar), ec);
    const std::lock_guard lock(cache_mutex_);
    cache_.erase(id);
    summaries_.erase(id);
  }
  std::uint64_t recoveries() const {
    const std::lock_guard lock(cache_mutex_);
    return recoveries_;
  }
 private:
  static json summarize(const std::shared_ptr<const TradingView>& view) {
    if (!view) throw std::runtime_error("Replay account unavailable");
    const auto result = view->snapshot->evaluation.status;
    const auto& e = view->snapshot->evaluation;
    const auto id = preset_id(view->config.initial_cash,
        trading::original_program_rules(view->config.rules, e.starting_balance, e.size_scaling));
    return {{"result", result == trading::EvaluationStatus::Passed ? "pass" : result == trading::EvaluationStatus::Failed ? "fail" : "open"},
            {"pnl", (view->snapshot->equity - view->config.initial_cash).str()},
            {"valuation_complete", view->snapshot->valuation_complete}, {"plan_name", view->config.rules.plan},
            {"plan_id", id.empty() ? json(nullptr) : json(id)},
            {"time", md::format_timestamp(view->snapshot->time)}};
  }
  static json summarize_archive(const ArchivedReplay& account) {
    auto value = summarize(account.trading_view());
    value.update(account.integrity());
    return value;
  }
  std::shared_ptr<ArchivedReplay> recover(const std::filesystem::path& path, json* diagnostic = nullptr) const {
    ++recoveries_;  // Caller holds cache_mutex_. Count attempts, including damaged journals.
    return std::make_shared<ArchivedReplay>(path, diagnostic);
  }
  json summary(const std::string& id, const std::filesystem::path& path) const {
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    const auto bytes = ec ? 0 : std::filesystem::file_size(path, ec);
    if (ec) throw UnknownRun(id);
    const std::lock_guard lock(cache_mutex_);
    const auto found = summaries_.find(id);
    if (found != summaries_.end() && found->second.modified == modified && found->second.bytes == bytes) return found->second.value;
    json value;
    try {
      const auto cached = cache_.find(id);
      const auto account = cached != cache_.end() && cached->second.modified == modified && cached->second.bytes == bytes
          ? cached->second.account : recover(path, &value);
      value = summarize_archive(*account);
    } catch (const std::filesystem::filesystem_error& error) { value["error"] = error.code().message(); }
    catch (const std::exception& error) { value["error"] = error.what(); }
    summaries_[id] = {modified, bytes, value};
    return value;
  }
  struct Cached {
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes;
    std::shared_ptr<ArchivedReplay> account;
  };
  struct Summary {
    std::filesystem::file_time_type modified;
    std::uintmax_t bytes;
    json value;
  };
  mutable std::mutex cache_mutex_;
  mutable std::map<std::string, Cached> cache_;
  mutable std::map<std::string, Summary> summaries_;
  mutable std::uint64_t recoveries_ = 0;
  std::filesystem::path directory_;
  bool writable_;
  std::string verifying_;  // handoff_mutex_ protects verification and sidecar writes
  json verification_;
};

ReplayHost::ReplayHost(Options options)
    : options_(std::move(options)), demos_(std::make_unique<DemoRecordings>()),
      scenarios_(providers::load_scenarios(options_.scenario_dir,
          [](const auto& error) { std::fprintf(stderr, "scenario skipped: %s\n", error.c_str()); })),
      history_(std::make_unique<History>(options_.engine)) {
  if (!options_.engine.paper_journal.empty()) {
    const auto directory = options_.engine.paper_journal.parent_path() / "scenario-packs";
    std::error_code ec;
    if (std::filesystem::is_directory(directory, ec)) for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.path().extension() != ".pack") continue;
      try {
        std::ifstream file(entry.path());
        const auto pack = json::parse(file);
        const auto id = pack.at("scenario").at("id").get<std::string>();
        // The id names a file; check it before building a path from it.
        if (!providers::valid_scenario_id(id)) throw std::invalid_argument(entry.path().filename().string() + ": invalid scenario id");
        if (builtin_id(id)) throw std::invalid_argument(id + ": a pack cannot replace a built-in scenario");
        auto scenario = providers::read_scenario(directory / (id + ".json"));
        if (json::parse(scenario.source) != pack.at("scenario")) throw std::invalid_argument("Pack definition disagrees with its saved source");
        const auto existing = std::find_if(scenarios_.begin(), scenarios_.end(), [&](const auto& s) { return s.id == scenario.id; });
        if (existing != scenarios_.end() && json::parse(existing->source) != json::parse(scenario.source)) throw std::invalid_argument("Scenario id conflicts with catalogue");
        packs_[scenario.id] = pack.dump();
        if (existing == scenarios_.end()) scenarios_.push_back(std::move(scenario));
      } catch (const std::exception& error) { std::fprintf(stderr, "scenario pack skipped: %s\n", error.what()); }
    }
  }
  worker_ = std::thread([this] { work(); });
}
ReplayHost::~ReplayHost() {
  std::deque<Job> left;
  {
    const std::lock_guard lock(jobs_mutex_);
    closing_ = true;
    left.swap(jobs_);
  }
  jobs_ready_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (auto& job : left) job.complete(api_error(503, "ENGINE_STOPPING", "The replay host is stopping"));
  try { stop(); }
  catch (const std::exception& error) { std::fprintf(stderr, "Replay shutdown: %s\n", error.what()); }
  try {
    const std::lock_guard control_lock(control_mutex_);
    stop_verifier();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Replay verifier shutdown: %s\n", error.what());
  }
}

void ReplayHost::enqueue(const ApiRequest& request, const ApiCompletion& complete) {
  {
    const std::lock_guard lock(jobs_mutex_);
    if (!closing_) {
      jobs_.push_back({request, complete});
      jobs_ready_.notify_one();
      return;
    }
  }
  complete(api_error(503, "ENGINE_STOPPING", "The replay host is stopping"));
}

void ReplayHost::work() {
  while (true) {
    Job job;
    {
      std::unique_lock lock(jobs_mutex_);
      jobs_ready_.wait_for(lock, std::chrono::milliseconds(100), [&] { return closing_ || !jobs_.empty(); });
      if (closing_) return;
      if (jobs_.empty()) {
        lock.unlock();
        const std::lock_guard control_lock(control_mutex_);
        const std::lock_guard handoff(handoff_mutex_);
        const auto session = current();
        if (session && session->provider->finished()) {
          try { history_->finish(*session); } catch (const std::exception&) { /* Retry on the next poll or explicit read. */ }
        }
        continue;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    const std::lock_guard control_lock(control_mutex_);
    bool closing;
    { const std::lock_guard lock(jobs_mutex_); closing = closing_; }
    if (closing) {
      job.complete(api_error(503, "ENGINE_STOPPING", "The replay host is stopping"));
      continue;
    }
    if (std::string_view(job.request.target).starts_with("/api/replay/history/")) history(job.request, job.complete);
    else if (std::string_view(job.request.target).starts_with("/api/replay/packs")) packs(job.request, job.complete);
    else control(job.request, job.complete);
  }
}

std::uint64_t ReplayHost::history_recoveries() const { return history_->recoveries(); }

std::shared_ptr<ReplayHost::Session> ReplayHost::current() const {
  const std::lock_guard lock(mutex_);
  return session_;
}

void ReplayHost::set_dividends(std::vector<trading::Dividend> dividends) {
  const std::lock_guard lock(mutex_);
  options_.engine.dividends = std::move(dividends);
}

void ReplayHost::stop() {
  std::deque<Job> refused;
  {
    const std::lock_guard lock(jobs_mutex_);
    closing_ = true;
    refused.swap(jobs_);
  }
  jobs_ready_.notify_all();
  for (auto& job : refused) job.complete(api_error(503, "ENGINE_STOPPING", "The replay host is stopping"));
  if (const auto session = current()) session->provider->abort();
  const std::lock_guard control_lock(control_mutex_);
  stop_verifier();
  stop_session(true);
}

void ReplayHost::stop_verifier() {
  verifier_stop_ = true;
  if (verifier_.joinable()) verifier_.join();
  verifier_stop_ = false;
}

void ReplayHost::stop_session(bool shutdown) {
  // A retiring journal becomes history only after Engine::stop has flushed it.
  const std::lock_guard handoff(handoff_mutex_);
  std::shared_ptr<Session> old;
  {
    const std::lock_guard lock(mutex_);
    old = std::move(session_);
  }
  // Drain callbacks while this thread still owns the session; its last reference
  // must not be released by a completion running on the engine thread.
  if (old) {
    // Provider shutdown also sets finished(), so remember whether playback had
    // already ended before stopping it. Leave interrupted metadata resumable.
    const bool ended = old->provider->finished();
    old->engine->stop();
    if (!shutdown || ended) history_->finish(*old);
  }
  old.reset();
}

bool ReplayHost::handle(const ApiRequest& request, const ApiCompletion& complete) {
  constexpr std::string_view prefix = "/api/replay";
  const std::string_view target = request.target;
  if (!target.starts_with(prefix)) return false;
  const auto rest = target.substr(prefix.size());
  if (!rest.empty() && rest.front() != '?' && rest.front() != '/') return false;
  const bool controls = rest.empty() || rest.front() == '?';
  if (rest == "/packs" || rest.starts_with("/packs/")) { enqueue(request, complete); return true; }
  if (rest == "/history" && request.method == "GET") {
    const std::lock_guard handoff(handoff_mutex_);
    complete(ok({{"history", history_->list(current())}}));
    return true;
  }
  const bool saved = rest.starts_with("/history/");
  // Changes wait their turn on the control thread; a step there never holds this one.
  if ((controls && request.method != "GET") || (saved && (request.method == "DELETE" || request.method == "POST"))) {
    // Interrupt before queuing: an until occupies the host thread until its current
    // batch and consumer barrier settle. Invalid controls must never move a replay.
    if (request.target == "/api/replay" && options_.engine.write_mode != "disabled") {
      bool interrupt = request.method == "DELETE";
      if (request.method == "PUT") {
        try {
          const auto body = parse_body(request, {"speed", "paused", "skip", "until", "play_until", "abort"});
          if (body.contains("speed")) (void)speed_field(body);
          const bool valid_skip = !body.contains("skip") || body.at("skip").is_boolean();
          interrupt = (body.size() == 1 && body.value("abort", json(false)) == json(true)) ||
              (valid_skip && body.value("paused", json(false)) == json(true) &&
               !body.contains("until") && !body.contains("play_until") && !body.contains("abort"));
        } catch (const std::exception&) { /* The control thread returns the validation error. */ }
      }
      if (interrupt) if (const auto session = current(); session && (request.method == "DELETE" || !session->provider->fast_forwarding()))
        session->provider->abort();
    }
    enqueue(request, complete);
    return true;
  }
  if (controls) {
    control(request, complete);
    return true;
  }
  if (saved) {
    history(request, complete);
    return true;
  }
  const auto session = current();
  if (!session) {
    complete(api_error(404, "NO_REPLAY", "No replay is running; start one at /api/replay"));
    return true;
  }
  if (request.method != "GET" && session->provider->fast_forwarding()) {
    complete(api_error(409, "REPLAY_FAST_FORWARD", "Wait until the replay reaches start_at before trading"));
    return true;
  }
  // A command during a step would land at whatever market time the step had reached.
  if (request.method != "GET" && (session->provider->stepping() || session->restarting.load())) {
    complete(api_error(409, "REPLAY_STEPPING", "Wait for the until step to settle; commands after it use the paused market time"));
    return true;
  }
  if (request.method != "GET" && session->provider->finished()) {
    complete(api_error(403, "REPLAY_READ_ONLY", "Finished replay accounts are read-only"));
    return true;
  }
  if (request.method != "GET" && options_.engine.write_mode == "disabled") {
    complete(api_error(403, "WRITE_DISABLED", "Replay writes are disabled"));
    return true;
  }
  ApiRequest forwarded = request;
  forwarded.target = "/api" + std::string(rest);
  handle_api_async(forwarded, *session->engine, [session, complete, status = rest == "/status"](ApiResponse response) {
    if (status && response.status == 200) {
      auto message = json::parse(response.body);
      replay_gate(message, *session->provider);
      response.body = message.dump();
    }
    complete(std::move(response));
  });
  return true;
}

void ReplayHost::history(const ApiRequest& request, const ApiCompletion& complete) {
  try {
    const auto route = std::string_view(request.target).substr(std::string_view("/api/replay/history/").size());
    const auto slash = route.find('/');
    const std::string id(route.substr(0, slash));
    auto session = current();
    if (session && session->id == id && !session->provider->finished()) {
      complete(api_error(409, "REPLAY_RUNNING", "Stop the replay before opening or deleting its history"));
      return;
    }
    const auto unknown = [&] { complete(api_error(404, "NOT_FOUND", "No saved replay run " + id)); };
    if (request.method == "DELETE" && slash == std::string_view::npos) {  // on the control thread
      if (history_->journal(id).empty()) return unknown();
      if (options_.engine.write_mode == "disabled" || !history_->writable()) {
        complete(api_error(403, "WRITE_DISABLED", "Replay history is read-only"));
        return;
      }
      if (session && session->id == id) stop_session();
      session.reset();
      const std::lock_guard handoff(handoff_mutex_);
      if (history_->verifying() == id) {
        complete(api_error(409, "VERIFICATION_RUNNING", "Wait for verification before deleting this run"));
        return;
      }
      history_->remove(id);
      complete(ok({{"deleted", id}}));
      return;
    }
    const std::lock_guard handoff(handoff_mutex_);
    if (history_->journal(id).empty()) return unknown();
    if (session && session->id == id) history_->finish(*session);
    const auto suffix = slash == std::string_view::npos ? std::string_view{} : route.substr(slash);
    if (suffix == "/verify" || suffix.starts_with("/verify?")) {
      const auto question = suffix.find('?');
      const auto query = query_parameters(question == std::string_view::npos ? std::string_view{} : suffix.substr(question + 1));
      if (!query || (!query->empty() && (*query != std::map<std::string, std::string>{{"format", "receipt"}} || request.method != "GET"))) {
        complete(api_error(400, "INVALID_REQUEST", "Verification accepts only GET format=receipt"));
        return;
      }
      if (request.method == "GET") {
        const auto value = history_->verification(id);
        const bool receipt = !query->empty();
        if (receipt && value.at("status") != "passed" && value.at("status") != "failed") {
          complete(api_error(409, "VERIFICATION_UNAVAILABLE", "Verify this run before downloading its receipt"));
          return;
        }
        auto response = ok(value);
        if (receipt) response.download = slug(id) + "-verification.json";
        complete(std::move(response));
        return;
      }
      if (request.method != "POST") {
        complete(api_error(405, "METHOD_NOT_ALLOWED", "Use GET or POST for verification"));
        return;
      }
      if (!history_->writable() || options_.engine.write_mode == "disabled") {
        complete(api_error(403, "WRITE_DISABLED", "Replay history is read-only"));
        return;
      }
      if (!history_->verifying().empty()) {
        complete(api_error(409, "VERIFICATION_RUNNING", "Already verifying " + history_->verifying() + "; wait for it to finish"));
        return;
      }
      (void)parse_body(request, {});
      stop_verifier();
      const auto fingerprint = history_->stamp(id);
      const auto file = history_->journal(id);
      history_->begin_verification(id, fingerprint);
      const auto accepted = history_->verification(id);
      verifier_ = std::thread([this, id, file, fingerprint] {
        const auto verified = verify_run(file, [this] { return verifier_stop_.load(); },
            [this](std::uint64_t done, std::uint64_t total) {
          if (options_.verification_progress) options_.verification_progress(done, total);
          const std::lock_guard lock(handoff_mutex_);
          history_->verification_progress(done, total);
        });
        // Cancelled by a stop, it found nothing: it stays interrupted, not failed.
        if (!verified.matched && verifier_stop_.load()) {
          const std::lock_guard lock(handoff_mutex_);
          history_->abandon_verification();
          return;
        }
        json value{{"status", verified.matched ? "passed" : "failed"}, {"message", verified.message},
                   {"transactions", verified.transactions}, {"equity", verified.matched ? json(verified.equity.str()) : json(nullptr)},
                   {"head", verified.head.empty() ? json(nullptr) : json(verified.head)}, {"build", OPENPORT_VERSION},
                   {"time", verified.time > 0 ? json(md::format_timestamp(verified.time)) : json(nullptr)},
                   {"finished_at", md::format_timestamp(md::now())}, {"run", verified.run}, {"progress", 1.0}};
        const std::lock_guard lock(handoff_mutex_);
        try { history_->end_verification(id, std::move(value), fingerprint); }
        catch (const std::exception& error) { std::fprintf(stderr, "Replay verification: %s\n", error.what()); }
      });
      complete(ok(accepted, 202));
      return;
    }
    if (request.method != "GET") {
      complete(api_error(403, "REPLAY_READ_ONLY", "Finished replay accounts are read-only"));
      return;
    }
    const auto details = history_->details(id);
    if (details.contains("error")) {
      auto response = api_error(422, "REPLAY_HISTORY_FAILED", details.at("error").get<std::string>());
      auto value = json::parse(response.body);
      for (const auto& [key, detail] : details.items()) if (key != "error") value[key] = detail;
      response.body = value.dump();
      complete(std::move(response));
      return;
    }
    const auto archived = history_->open(id);
    ApiRequest forwarded = request;
    forwarded.target = slash == std::string_view::npos ? "/api/account" : "/api" + std::string(route.substr(slash));
    const bool account = forwarded.target.substr(0, forwarded.target.find('?')) == "/api/account";
    handle_api_async(forwarded, *archived, [archived, complete, details, account](ApiResponse response) {
      if (account && response.status == 200) {
        auto value = json::parse(response.body);
        value.update(details);
        response.body = value.dump();
      }
      complete(std::move(response));
    });
  } catch (const json::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::invalid_argument& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const UnknownRun& error) {
    complete(api_error(404, "NOT_FOUND", error.what()));
  } catch (const trading::TradingError& error) {
    if (error.code() == trading::Reason::JOURNAL_LOCKED)
      complete(api_error(409, "REPLAY_RUNNING", "Another openportd is still writing this run"));
    else complete(api_error(422, "REPLAY_HISTORY_FAILED", error.what()));
  } catch (const std::filesystem::filesystem_error& error) {
    // Its what() names absolute paths; the code's message is enough.
    complete(api_error(422, "REPLAY_HISTORY_FAILED", error.code().message()));
  } catch (const std::exception& error) { complete(api_error(422, "REPLAY_HISTORY_FAILED", error.what())); }
}

void ReplayHost::resume(const std::string& id, int speed, bool paused, const ApiCompletion& complete,
                        bool restart, const std::string& at, const std::string& trade, const std::string& actor) {
  const bool sandbox = !trade.empty();
  const auto refuse = [&](const std::string& message) { complete(api_error(409, restart ? "REPLAY_NOT_RESTARTABLE" : "REPLAY_NOT_RESUMABLE", message)); };
  std::shared_ptr<Session> source;
  bool source_paused = true;
  md::Timestamp source_pause_at = 0;
  struct Restore {
    std::function<void()> action;
    ~Restore() { action(); }
  } restore{[&] {
    if (!source) return;
    source->restarting = false;
    if (!source->provider->finished()) {
      try {
        if (source_pause_at > 0) source->provider->play_until(source_pause_at);
        else source->provider->set_paused(source_paused);
      } catch (const std::exception&) { source->provider->set_paused(true); }
    }
  }};
  std::string created;
  Restore cleanup{[&] { if (!created.empty()) history_->discard(created); }};
  try {
    if (const auto running = current(); !restart && running && running->id == id) {
      if (!running->provider->finished()) complete(api_error(409, "REPLAY_RUNNING", "This run is already playing"));
      else refuse("The run has ended: it played to its end or was stopped");
      return;
    }
    const auto file = history_->journal(id);
    if (file.empty()) {
      complete(api_error(404, "NOT_FOUND", "No saved replay run " + id));
      return;
    }
    if (!history_->writable()) {
      complete(api_error(403, "WRITE_DISABLED", "Replay history is read-only"));
      return;
    }
    {
      const std::lock_guard handoff(handoff_mutex_);
      if (history_->verifying() == id) {
        complete(api_error(409, "VERIFICATION_RUNNING", restart
            ? "Wait for verification before restarting this run"
            : "Wait for verification before resuming this run"));
        return;
      }
    }
    if (restart) {
      const auto active = current();
      if (active && active->id == id && active->provider->finished()) {
        const std::lock_guard handoff(handoff_mutex_);
        history_->finish(*active);
      }
      if (active && active->id == id && !active->provider->finished()) {
        if (active->provider->fast_forwarding()) return refuse("Wait for the source run to reach its start state");
        source = active;
        source_paused = source->provider->paused();
        source_pause_at = source->provider->pause_at();
        source->restarting = true;
        source->provider->abort();
        source->provider->wait_paused();
        source->engine->synchronize().get();
      }
    }
    const auto metadata = source ? source->state() : history_->metadata(id);
    if (!metadata.is_object()) return refuse("The run's metadata is missing or unreadable");
    if (!restart && metadata.value("finished", false)) return refuse("The run has ended: it played to its end or was stopped");
    auto recovery = trading::FileJournal::read(file.string());
    // A crash part way through a write leaves a torn last line; the repair keeps a copy.
    if (recovery.truncated_final_line) {
      if (restart) return refuse("The source journal has a torn final line; repair it before restarting");
      (void)trading::FileJournal::repair(file.string());
      recovery = trading::FileJournal::read(file.string());
    }
    if (recovery.records.empty()) return refuse("The run's journal holds no transaction");
    if (restart && metadata.contains("journal")) {
      const auto& recorded = metadata.at("journal");
      if (recorded.at("transactions") != recovery.records.size() || recorded.at("head") != recovery.head ||
          recorded.at("bytes") != std::filesystem::file_size(file))
        return refuse("The source journal disagrees with its saved final head/count/bytes");
    }
    std::vector<json> inputs;
    for (const auto& input : run_inputs(recovery)) inputs.push_back(json::parse(input));
    if (inputs.empty() || inputs.front().at("kind") != "start") return refuse("The journal has no reproducible-run metadata");
    const auto& start = inputs.front();
    // The run re-executes on this build's driver; one recorded on an older driver would differ.
    if (start.value("driver", 1) != 6) return refuse("The run was recorded by an older build's driver; verify it with --verify-run");
    md::Timestamp target = 0;
    for (const auto& input : inputs) {
      if (input.at("kind") == "source") return refuse("A run that changed its recording part way cannot resume yet");
      if (input.at("kind") == "boundary") target = input.at("driver_time").get<md::Timestamp>();
    }
    if (start.value("calendar", json::array()) != json(md::scheduled_days()))
      return refuse("The exchange calendar has changed since the run started");
    const auto& input = start.at("input");
    const providers::Scenario* day = nullptr;
    std::optional<providers::Scenario> source_scenario;
    int revision = providers::kScenarioRevision;
    std::filesystem::path recording;
    md::Date date;
    std::uint64_t seed = 0;
    if (input.at("kind") == "scenario") {
      if (!options_.demo) {
        complete(api_error(404, "NOT_FOUND", "The demo market is off on this server"));
        return;
      }
      for (const auto& scenario : scenarios_) if (scenario.id == input.at("id").get<std::string>()) day = &scenario;
      if (!day) return refuse("The run's scenario is no longer listed");
      if (restart && !day->builtin) {
        try { source_scenario = providers::read_scenario(day->source_file); }
        catch (const std::exception& error) { return refuse(error.what()); }
        day = &*source_scenario;
      }
      date = input.at("date").get<md::Date>();
      seed = input.at("seed").get<std::uint64_t>();
      auto identity = json::parse(scenario_input(*day, date, seed));
      if (restart) {
        revision = input.value("revision", 1);
        if (revision < 1 || revision > providers::kScenarioRevision) return refuse("Unsupported scenario revision");
        if (input.contains("revision")) identity["revision"] = revision;
        else identity.erase("revision");
      }
      if (input.contains("challenge")) identity["challenge"] = input.at("challenge");
      if (input.contains("scenario_source")) identity["scenario_source"] = input.at("scenario_source");
      if (identity != input) return refuse("The scenario or its generator has changed since the run started");
    } else {
      recording = input.at("file").get<std::string>();
      std::error_code ec;
      if (!std::filesystem::is_regular_file(recording, ec) || json::parse(recording_input(recording)) != input)
        return refuse("The run's recording has changed or moved");
    }
    auto session = std::make_shared<Session>();
    session->id = id;
    session->file = metadata.value("file", id);
    session->scenario = day ? day->id : "";
    if (input.contains("challenge") && !sandbox) session->challenge = input.at("challenge");
    session->seed = day ? std::to_string(seed) : "";
    session->generator = day ? day->generator : 0;
    session->plan = metadata.value("plan", std::string("practice"));
    session->date = metadata.value("date", std::string());
    session->start_at = metadata.value("start_at", std::string());
    session->target = target;
    if (metadata.contains("restarted_from")) session->restarted_from = metadata.at("restarted_from");
    if (day) {
      session->windows = providers::scenario_windows(*day, date);
      session->events = scenario_marks(*day, date);
      session->objectives = drill_objectives(*day, date);
    }
    if (restart) {
      const auto first_boundary = std::find_if(inputs.begin(), inputs.end(), [](const auto& operation) { return operation.at("kind") == "boundary"; });
      if (first_boundary == inputs.end()) return refuse("The source run has no complete market batch");
      const auto first_market = first_boundary->at("time").get<md::Timestamp>();
      const auto first_receipt = input.at("started").get<md::Timestamp>();
      const auto trading_date = md::trading_date(first_receipt);
      const bool overnight = md::new_york_time(first_receipt).date < trading_date;
      md::Timestamp run_start = first_market;
      if (metadata.contains("restarted_from")) {
        run_start = md::parse_datetime(metadata.at("restarted_from").at("at").get<std::string>(), md::Zone::Utc).value();
      } else if (!session->start_at.empty()) {
        const auto receipt_start = start_time(session->start_at, session->windows, first_receipt, trading_date, overnight);
        for (const auto& operation : inputs) {
          if (operation.at("kind") == "boundary" && operation.at("driver_time").get<md::Timestamp>() >= receipt_start) {
            run_start = operation.at("time").get<md::Timestamp>();
            break;
          }
        }
      }
      md::Timestamp run_end = first_market;
      for (const auto& operation : inputs) if (operation.contains("time")) run_end = std::max(run_end, operation.at("time").get<md::Timestamp>());
      if (metadata.contains("settled_through") && metadata.at("settled_through").is_string()) {
        if (const auto settled = md::parse_datetime(metadata.at("settled_through").get<std::string>(), md::Zone::Utc)) run_end = std::max(run_end, *settled);
      }
      target = at.empty() ? run_start : start_time(at, session->windows, first_market, trading_date, overnight);
      if (sandbox) {
        const auto account = trading::TradingSession::recover(recovery);
        std::optional<md::Timestamp> opened;
        if (trade.starts_with("s")) {
          for (const auto& trip : trading::share_lifecycles(account.snapshot()->stock_fills, account.snapshot()->dividends))
            if ("s" + std::to_string(trip.fills.front()) == trade) opened = trip.opened;
        } else {
          for (const auto& trip : trading::lifecycles(account.snapshot()->recent_fills, account.snapshot()->closures, account.contracts()))
            if (std::to_string(trip.first_fill) == trade) opened = trip.opened;
        }
        if (!opened) throw std::invalid_argument("No trade with this id in the source run");
        target = std::max(first_market, *opened - 5 * md::kNanosPerMinute);
        for (const auto& w : session->windows) if (*opened >= w.first && *opened <= w.last) target = std::max(target, w.first);
        run_start = first_market;
      }
      if (target < run_start || target > run_end)
        throw std::invalid_argument("Restart at must be between the run's start " + md::format_timestamp(run_start) + " and its end " + md::format_timestamp(run_end));
      // Commands are input-first; their record can have the preceding account's
      // timestamp. Check their explicit market time before retaining their effects.
      std::size_t count = 0;
      for (const auto& record : recovery.records) {
        if (record.time > target) break;
        bool later_command = false;
        if (record.type == "run_input") {
          const auto payload = json::parse(record.payload);
          for (const auto& event : payload.at("events")) {
            if (event.at("type") == "run_input" && event.at("payload").at("kind") == "command" &&
                event.at("payload").at("time").get<md::Timestamp>() > target) later_command = true;
          }
        }
        if (later_command) break;
        ++count;
      }
      recovery.records.resize(count);
      recovery.head = recovery.records.back().hash;
      static std::atomic<unsigned> restarts{0};
      session->id = slug(session->scenario.empty() ? session->file : session->scenario) + "-restart-" + std::to_string(md::now()) + "-" + std::to_string(++restarts);
      session->target = target;
      session->start_at = md::format_timestamp(target);
      session->restarted_from = {{"id", id}, {"at", md::format_timestamp(target)}};
    }
    providers::ReplayProvider::Options playback;
    playback.file = day ? demos_->get(*day, date, seed, revision) : recording;
    playback.speed = speed;
    if (!session->windows.empty()) playback.known_end = session->windows.back().last;
    // The recorded batches replay unpaced, and playback continues from the last one.
    playback.start_at = restart && !sandbox ? 0 : target;
    playback.start_through = restart && !sandbox ? target : 0;
    playback.paused = restart ? true : paused;
    for (const auto& window : session->windows) if (session->windows.size() > 1) playback.max_gap = std::max(playback.max_gap, window.step);
    session->provider = std::make_unique<providers::ReplayProvider>(std::move(playback));
    session->demo = providers::simulated_provider(session->provider->header().provider);
    session->durable = true;
    auto engine = [&] {
      const std::lock_guard lock(mutex_);
      return options_.engine;
    }();
    const auto first = json::parse(recovery.records.front().payload);
    // Every option the run's start recorded comes from the run, not from this server's flags.
    engine.paper = first.at("state").at("config").get<trading::SessionConfig>();
    if (!session->windows.empty()) engine.replay_end = session->windows.back().last;
    engine.initial_actor = sandbox ? actor : first.value("actor", std::string("system"));
    engine.initial_playbooks = start.contains("playbooks") && !start.at("playbooks").is_null() ? start.at("playbooks").dump() : "";
    engine.analytics = start.at("analytics").get<analytics::AnalyticsOptions>();
    engine.dividends = start.at("dividends").get<std::vector<trading::Dividend>>();
    if (!sandbox) engine.resume = std::make_shared<const trading::JournalRecovery>(std::move(recovery));
    else engine.resume.reset();
    // Until the resumed run matches, its set-aside sidecars go back on any failure.
    std::string aside;
    struct PutBack {
      const History& history;
      std::string& id;
      std::unique_ptr<Engine>& engine;
      // The failed engine first: stopping, it would rewrite the equity file put back.
      void now() {
        if (id.empty()) return;
        if (engine) { engine->stop(); engine.reset(); }
        history.restore(id);
        id.clear();
      }
      ~PutBack() { now(); }
    } put_back{*history_, aside, session->engine};
    std::unique_lock handoff(handoff_mutex_);
    if (restart) {
      if (target > session->provider->end_time()) throw std::invalid_argument("Restart at exceeds the recording's end");
      created = session->id;
      engine.paper_journal = history_->create(*session, engine.paper);
      if (!sandbox) {
        trading::FileJournal::Options journal_options;
        journal_options.sync_policy = trading::FileJournal::SyncPolicy::Batched;
        auto prefix = trading::FileJournal::create(engine.paper_journal.string(), journal_options);
        for (const auto& record : engine.resume->records) prefix->append(record.time, record.type, record.payload);
        prefix->flush();
      }
    } else {
      history_->set_aside(id);
      aside = id;
      engine.paper_journal = file;
    }
    engine.replay = true;
    auto identity = input;
    if (sandbox) { identity.erase("challenge"); identity.erase("scenario_source"); }
    engine.run_input = identity.dump();
    engine.run_id = session->id;
    // A sandbox starts its own journal at the reviewed trade. A resumed or restarted run
    // keeps its source's start input, so its start_at; a restart journals where it continues.
    engine.run_start_at = sandbox ? std::optional(target) : std::nullopt;
    engine.run_restart = restart && !sandbox ? json{{"kind", "restart"}, {"from", id}, {"at", target}}.dump() : std::string();
    engine.paper_accounts.clear();
    engine.paper_sink.reset();
    engine.record_file.clear();
    engine.record_dir.clear();
    engine.candles = std::make_shared<CandleStore>();
    auto* provider = session->provider.get();
    engine.clock = [provider] { return provider->time(); };
    const auto& header = provider->header();
    session->engine = std::make_unique<Engine>(*provider, md::Subscription{header.subscription.underlyings, 0, 0.0}, engine);
    session->engine->start();
    // The new journal already holds the verified prefix. Re-execute it completely
    // before handing over; a mismatch discards only the new run, never its source.
    if (restart) session->provider->wait_paused();
    if (!session->engine->status().trading.enabled) {
      // The journal is as it was: nothing appends until every recorded transaction matched.
      const auto reason = session->engine->status().trading.reason;
      session->engine->stop();
      // Before the reply: a caller may read the run as soon as it has it.
      put_back.now();
      return refuse(reason);
    }
    if (!aside.empty()) { history_->drop_aside(aside); aside.clear(); }
    std::shared_ptr<Session> old;
    {
      const std::lock_guard lock(mutex_);
      old = std::exchange(session_, session);
    }
    if (restart) created.clear();
    if (old) {
      old->engine->stop();
      old->restarting = false;
      history_->finish(*old);
    }
    // Its metadata now reads as a run playing again, as a crash would leave it.
    history_->finish(*session);
    if (restart && !paused) session->provider->set_paused(false);
    handoff.unlock();
    old.reset();
    complete(ok({{"replay", session->state()}}, 201));
  } catch (const std::invalid_argument& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const trading::TradingError& error) {
    if (error.code() == trading::Reason::JOURNAL_LOCKED) complete(api_error(409, "REPLAY_RUNNING", "Another openportd is still writing this run"));
    else refuse(error.what());
  } catch (const std::exception& error) { refuse(error.what()); }
}

void ReplayHost::packs(const ApiRequest& request, const ApiCompletion& complete) {
  try {
    if (request.method == "GET") {
      json packs = json::array();
      if (options_.demo) for (const auto& scenario : scenarios_) {
        if (packs_.contains(scenario.id)) packs.push_back(json::parse(packs_.at(scenario.id)));
        else packs.push_back({{"version", 1}, {"scenario", json::parse(scenario.source)}, {"seed", std::to_string(scenario.seed)}, {"plan", "practice"}});
      }
      if (request.target != "/api/replay/packs") {
        const auto id = request.target.substr(std::string("/api/replay/packs/").size());
        for (const auto& pack : packs) if (pack.at("scenario").at("id") == id) {
          auto response = ok(pack); response.download = id + "-pack.json"; complete(std::move(response)); return;
        }
        complete(api_error(404, "NOT_FOUND", "No scenario pack with this id")); return;
      }
      complete(ok({{"packs", packs}})); return;
    }
    if (request.method != "POST" || request.target != "/api/replay/packs") { complete(api_error(405, "METHOD_NOT_ALLOWED", "Use GET to export, POST /api/replay/packs to import")); return; }
    if (!options_.demo || !history_->writable()) { complete(api_error(403, "WRITE_DISABLED", "Scenario packs require demo mode and writable replay storage")); return; }
    auto pack = parse_body(request, {"version", "scenario", "seed", "plan", "challenge"});
    if (!pack.contains("version") || !pack.at("version").is_number_integer() || pack.at("version") != 1)
      throw std::invalid_argument("Scenario pack version must be 1");
    const auto directory = options_.engine.paper_journal.parent_path() / "scenario-packs";
    auto scenario = providers::parse_scenario(pack.at("scenario").dump(), directory / "import.json");
    if (builtin_id(scenario.id))
      throw std::invalid_argument("Scenario id " + scenario.id + " is a built-in scenario; packs cannot replace built-ins, so use a new id");
    pack["seed"] = std::to_string(seed_field(pack.at("seed"), scenario));
    const auto* plan = find_plan(pack.at("plan").get<std::string>());
    if (!plan || !plan->unlocked_by.empty()) throw std::invalid_argument("Pack plan must be an unlocked plan id");
    if (pack.contains("challenge")) {
      const auto& c = pack.at("challenge");
      if (!c.is_object() || c.size() != 2 || !c.contains("id") || !c.contains("title") || !c.at("id").is_string() || !c.at("title").is_string() ||
          c.at("id").get<std::string>().empty() || c.at("id").get<std::string>().size() > 80 || c.at("title").get<std::string>().empty() || c.at("title").get<std::string>().size() > 100)
        throw std::invalid_argument("challenge needs id (1–80 bytes) and title (1–100 bytes)");
    }
    const std::lock_guard handoff(handoff_mutex_);
    const auto existing = std::find_if(scenarios_.begin(), scenarios_.end(), [&](const auto& s) { return s.id == scenario.id; });
    if (existing != scenarios_.end() && json::parse(existing->source) != pack.at("scenario")) throw std::invalid_argument("Scenario id already exists with different content; use a new id");
    std::filesystem::create_directories(directory);
    const auto source = directory / (scenario.id + ".json");
    const auto target = directory / (scenario.id + ".pack");
    const auto write = [](const std::filesystem::path& path, const std::string& data) {
      trading::check_storage_space(path, data.size());
      const auto staged = path.string() + ".tmp";
      std::ofstream file(staged); file << data; file.close();
      if (!file) throw std::runtime_error("Cannot save scenario pack");
      std::filesystem::rename(staged, path);
    };
    write(source, scenario.source);
    write(target, pack.dump());
    scenario.source_file = std::filesystem::absolute(source);
    packs_[scenario.id] = pack.dump();
    if (existing == scenarios_.end()) scenarios_.push_back(std::move(scenario));
    complete(ok(pack, 201));
  } catch (const std::exception& error) { complete(api_error(400, "INVALID_REQUEST", error.what())); }
}

void ReplayHost::control(const ApiRequest& request, const ApiCompletion& complete) {
  if (request.target != "/api/replay") {
    complete(api_error(400, "INVALID_REQUEST", "/api/replay takes no query parameters"));
    return;
  }
  try {
    if (request.method == "GET") {
      // Whoever lists the demo may start it next: have the default day ready.
      if (options_.demo) demos_->prepare(default_scenario(scenarios_));
      const std::lock_guard handoff(handoff_mutex_);
      const auto session = current();
      complete(ok({{"write", options_.engine.write_mode}, {"directory", options_.recordings.string()}, {"recordings", recordings_json(options_.recordings)},
                   {"demo", options_.demo ? demo_json(default_scenario(scenarios_)) : json(nullptr)},
                   {"demos", options_.demo ? demos_json(scenarios_) : json::array()},
                   {"replay", session ? session->state() : json(nullptr)}, {"history", history_->list(session)}}));
    } else if (options_.engine.write_mode == "disabled") {
      complete(api_error(403, "WRITE_DISABLED", "Replay writes are disabled"));
    } else if (request.method == "POST") {
      auto body = parse_body(request, {"file", "demo", "scenario", "speed", "plan", "seed", "date", "start_at", "paused", "resume", "restart", "at", "copy_settings_from", "replay_from", "trade", "pack"});
      json challenge = nullptr;
      if (body.contains("pack")) {
        for (const auto& [key, value] : body.items()) if (key != "pack" && key != "paused" && key != "speed") throw std::invalid_argument("pack fixes scenario, seed, date and plan; only speed and paused may vary");
        if (!body.at("pack").is_string()) throw std::invalid_argument("pack must be a scenario id");
        const auto id = body.at("pack").get<std::string>();
        const auto found = std::find_if(scenarios_.begin(), scenarios_.end(), [&](const auto& s) { return s.id == id; });
        if (found == scenarios_.end()) throw std::invalid_argument("Unknown pack; import custom packs before starting them");
        const auto pack = packs_.contains(id) ? json::parse(packs_.at(id)) : json{{"version", 1}, {"scenario", json::parse(found->source)}, {"seed", std::to_string(found->seed)}, {"plan", "practice"}};
        body.erase("pack");
        body["scenario"] = pack.at("scenario").at("id");
        body["seed"] = pack.at("seed"); body["plan"] = pack.at("plan"); body["date"] = pack.at("scenario").at("date");
        if (pack.contains("challenge")) challenge = pack;
      }
      if (body.contains("replay_from")) {
        if (body.size() != 2 || !body.at("replay_from").is_string() || !body.contains("trade") || !body.at("trade").is_string() || body.at("trade").get<std::string>().empty())
          throw std::invalid_argument("replay_from takes a saved run id and a trade id");
        resume(body.at("replay_from").get<std::string>(), 1, true, complete, true, {}, body.at("trade").get<std::string>(), request.actor);
        return;
      }
      if (body.contains("trade")) throw std::invalid_argument("trade requires replay_from");
      if (body.contains("restart")) {
        for (const auto& [key, value] : body.items())
          if (key != "restart" && key != "at" && key != "speed" && key != "paused") throw std::invalid_argument("restart takes only at, speed and paused besides it");
        if (!body.at("restart").is_string() || (body.contains("at") && !body.at("at").is_string())) throw std::invalid_argument("restart and at must be strings");
        if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
        if (body.contains("at") && body.at("at").get<std::string>().empty()) throw std::invalid_argument("at must name a time or be omitted");
        const auto metadata = history_->metadata(body.at("restart").get<std::string>());
        const auto active = current();
        const int speed = body.contains("speed") ? speed_field(body) : active && active->id == body.at("restart").get<std::string>() ? active->provider->speed() : metadata.is_object() ? metadata.value("speed", 1) : 1;
        resume(body.at("restart").get<std::string>(), speed, body.value("paused", true), complete, true, body.value("at", std::string()));
        return;
      }
      if (body.contains("at")) throw std::invalid_argument("at requires restart");
      if (body.contains("resume")) {
        for (const auto& [key, value] : body.items())
          if (key != "resume" && key != "speed" && key != "paused") throw std::invalid_argument("resume takes only speed and paused besides it");
        if (!body.at("resume").is_string()) throw std::invalid_argument("resume must be a saved run's id");
        if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
        resume(body.at("resume").get<std::string>(), body.contains("speed") ? speed_field(body) : 1,
               body.value("paused", true), complete);
        return;
      }
      std::shared_ptr<const TradingView> settings;
      if (body.contains("copy_settings_from")) {
        if (!body.at("copy_settings_from").is_string()) throw std::invalid_argument("copy_settings_from must be an account id");
        const auto id = body.at("copy_settings_from").get<std::string>();
        if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") != std::string::npos ||
            !query_account("/api/account?account=" + id))
          throw std::invalid_argument("copy_settings_from must be an account id");
        if (options_.settings_source) settings = options_.settings_source(id);
        if (!settings) { complete(api_error(404, "UNKNOWN_ACCOUNT", "No settings source account " + id)); return; }
        trading::validate_limits(settings->config.limits);
        trading::validate_guardrails(settings->config.guardrails);
      }
      // demo: true plays the default day; a day's id plays that one.
      const providers::Scenario* day = nullptr;
      if (body.contains("demo") && body.contains("scenario")) throw std::invalid_argument("Give demo or scenario, not both");
      if (body.contains("demo") || body.contains("scenario")) {
        const auto& value = body.at(body.contains("scenario") ? "scenario" : "demo");
        if (value.is_boolean()) {
          if (value.get<bool>()) day = &default_scenario(scenarios_);
        } else if (value.is_string()) {
          for (const auto& scenario : scenarios_) if (scenario.id == value.get<std::string>()) day = &scenario;
          if (!day) throw std::invalid_argument("Unknown scenario id; see GET /api/replay");
        } else {
          throw std::invalid_argument("demo must be true or a demo day's id");
        }
      }
      const bool demo = day != nullptr;
      if (demo == body.contains("file")) throw std::invalid_argument("Give either file, a recording's name, or demo: true");
      if (demo && !options_.demo) {
        complete(api_error(404, "NOT_FOUND", "The demo market is off on this server"));
        return;
      }
      std::string name = demo ? "Demo market: " + day->title : std::string();
      std::filesystem::path path;
      if (!demo) {
        if (!body.at("file").is_string()) throw std::invalid_argument("file must be a recording's name");
        name = body.at("file").get<std::string>();
        path = options_.recordings / name;
        std::error_code ec;
        if (options_.recordings.empty() || !plain_name(name) || !std::filesystem::is_regular_file(path, ec)) {
          complete(api_error(404, "NOT_FOUND", "No recording " + name + " in the recordings directory"));
          return;
        }
      }
      const int speed = body.contains("speed") ? speed_field(body) : 1;
      if (body.contains("plan") && !body.at("plan").is_string()) throw std::invalid_argument("plan must be a plan id");
      const auto* plan = find_plan(body.contains("plan") && body.at("plan").is_string() ? body.at("plan").get<std::string>() : "practice");
      if (!plan || !plan->unlocked_by.empty()) throw std::invalid_argument("plan must be an unlocked evaluation or practice plan; see GET /api/plans");
      if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
      const bool paused = body.value("paused", false);
      if (!demo && (body.contains("seed") || body.contains("date"))) throw std::invalid_argument("seed and date apply only to scenarios");
      auto date = demo ? day->date : md::Date{};
      if (demo && body.contains("date")) {
        const auto value = body.at("date").get<std::string>();
        const auto parsed = md::parse_datetime(value + "T12:00:00", md::Zone::NewYork);
        if (value.size() != 10 || !parsed || md::trading_date(*parsed) != md::new_york_time(*parsed).date)
          throw std::invalid_argument("date must be a trading date, YYYY-MM-DD");
        date = md::new_york_time(*parsed).date;
      }
      const auto seed = demo ? (body.contains("seed") ? seed_field(body.at("seed"), *day) : fresh_seed()) : 0;
      auto session = std::make_shared<Session>();
      session->file = name;
      session->demo = demo;
      session->scenario = demo ? day->id : "";
      session->seed = demo ? std::to_string(seed) : "";
      session->generator = demo ? day->generator : 0;
      session->plan = plan->id;
      session->challenge = challenge;
      if (body.contains("start_at")) session->start_at = body.at("start_at").get<std::string>();
      if (demo) {
        session->windows = providers::scenario_windows(*day, date);
        session->events = scenario_marks(*day, date);
        session->objectives = drill_objectives(*day, date);
      }
      md::Timestamp first = demo ? session->windows.front().first : 0;
      md::Timestamp last = demo ? session->windows.back().last : 0;
      bool overnight = demo && day->overnight;
      if (!demo) {
        md::RecordingReader reader(path);
        first = reader.header().started;
        if (body.contains("start_at")) {
          bool seen = false;
          while (const auto event = reader.next()) {
            if (!seen) { first = event->received; seen = true; }
            last = std::max(last, event->received);
          }
          if (!reader.diagnostic().empty()) throw std::invalid_argument(reader.diagnostic());
        }
        date = md::trading_date(first);
        overnight = md::new_york_time(first).date < date;
      }
      session->date = md::format_date(date);
      if (body.contains("start_at")) {
        // A bare time is on the session's date, or its first occurrence in a run of several sessions.
        session->target = start_time(session->start_at, session->windows, first, date, overnight);
        if (session->target < first || session->target > last)
          throw std::invalid_argument(session->windows.size() > 1 ? "start_at must be within the run's sessions" : "start_at must be within the recording's session");
        if (session->windows.size() > 1) {
          const auto window = std::find_if(session->windows.begin(), session->windows.end(),
              [&](const auto& w) { return session->target <= w.last; });
          // The outer bounds above guarantee a window; a target before its open is in a gap.
          if (session->target < window->first) {
            const auto open = md::new_york_time(window->first);
            char time[32];
            std::snprintf(time, sizeof(time), "%02d:%02d", open.seconds / 3600, open.seconds / 60 % 60);
            throw std::invalid_argument("start_at " + session->start_at + " falls between this run's sessions; the next session opens at " +
                                        md::format_date(open.date) + "T" + time + " ET");
          }
        }
      }
      static std::atomic<unsigned> runs{0};
      session->id = slug(demo ? day->id : name) + "-" + session->date + "-" + (demo ? session->seed : "recording") +
          "-" + (session->start_at.empty() ? "open" : slug(session->start_at)) + "-" + std::to_string(md::now()) + "-" + std::to_string(++runs);
      providers::ReplayProvider::Options playback;
      playback.file = demo ? demos_->get(*day, date, seed) : path;
      playback.speed = speed;
      if (!session->windows.empty()) playback.known_end = session->windows.back().last;
      playback.start_at = session->target;
      playback.paused = paused;
      // A run of several sessions passes the closed market between them in a step.
      if (session->windows.size() > 1)
        for (const auto& window : session->windows) playback.max_gap = std::max(playback.max_gap, window.step);
      session->provider = std::make_unique<providers::ReplayProvider>(std::move(playback));
      session->demo = providers::simulated_provider(session->provider->header().provider);
      auto engine = [&] {
        const std::lock_guard lock(mutex_);
        return options_.engine;
      }();
      session->durable = history_->writable();
      // Simulated dividends replace the server's on the same symbol/date; explicit
      // session entries already replace generated ones in this shared schedule.
      if (demo) {
        for (const auto& dividend : providers::scenario_dividends(*day, date)) {
          std::erase_if(engine.dividends, [&](const auto& d) {
            return d.symbol == dividend.symbol && d.ex_date == dividend.ex_date;
          });
          engine.dividends.push_back(dividend);
        }
      }
      engine.paper.rules = plan->rules;
      engine.paper.initial_cash = plan->initial_cash;
      // Scenario windows already give the last batch. Reusing one must not scan
      // its full recording merely to explain the next-trading-day risk rule.
      if (!session->windows.empty()) engine.replay_end = session->windows.back().last;
      if (settings) {
        engine.paper.limits = settings->config.limits;
        engine.paper.guardrails = settings->config.guardrails;
      }
      // Copy definitions, never live-feed bindings, into this isolated run.
      if (!engine.paper_journal.empty()) {
        const auto definitions = engine.paper_journal.parent_path() / "playbooks.json";
        if (std::filesystem::exists(definitions)) {
          std::ifstream input(definitions);
          auto catalogue = json::parse(input);
          catalogue["modes"] = json::object();
          catalogue.erase("forward_tests");
          engine.initial_playbooks = catalogue.dump();
        }
      }
      // From its journal's creation until the run it replaces has flushed, both are
      // between running and history.
      std::unique_lock handoff(handoff_mutex_);
      engine.paper_journal = history_->create(*session, engine.paper);
      try {
        engine.replay = true;
        engine.initial_actor = request.actor;
        engine.run_input = demo ? scenario_input(*day, date, seed) : recording_input(path);
        if (!challenge.is_null()) {
          auto identity = json::parse(engine.run_input);
          identity["challenge"] = challenge;
          identity["scenario_source"] = day->source;
          engine.run_input = identity.dump();
        }
        engine.run_id = session->id;
        engine.run_start_at = session->target;
        engine.paper_accounts.clear();
        engine.paper_sink.reset();
        engine.record_file.clear();
        engine.record_dir.clear();
        engine.candles = std::make_shared<CandleStore>();
        // The replay's clock is the recording's: sessions and feed checks see that day.
        auto* provider = session->provider.get();
        engine.clock = [provider] { return provider->time(); };
        const auto& header = provider->header();
        session->engine = std::make_unique<Engine>(*provider, md::Subscription{header.subscription.underlyings, 0, 0.0}, engine);
        session->engine->start();
        if (engine.paper_enabled && !session->engine->status().trading.enabled)
          throw std::runtime_error(session->engine->status().trading.reason);
      } catch (...) {
        // A run that could not start, such as one whose first record a full disk
        // refused, leaves no history entry.
        if (session->engine) session->engine->stop();
        history_->discard(session->id);
        throw;
      }
      std::shared_ptr<Session> old;
      {
        const std::lock_guard lock(mutex_);
        old = std::exchange(session_, session);
      }
      if (old) {
        old->engine->stop();
        history_->finish(*old);
      }
      handoff.unlock();
      old.reset();
      complete(ok({{"replay", session->state()}}, 201));
    } else if (request.method == "PUT") {
      const auto body = parse_body(request, {"speed", "paused", "skip", "until", "abort", "play_until"});
      const auto session = current();
      if (!session) {
        complete(api_error(404, "NO_REPLAY", "No replay is running"));
        return;
      }
      if (session->provider->fast_forwarding()) {
        complete(api_error(409, "REPLAY_FAST_FORWARD", "Wait until the replay reaches start_at before changing playback"));
        return;
      }
      if (body.contains("paused") && !body.at("paused").is_boolean()) throw std::invalid_argument("paused must be true or false");
      if (body.contains("skip") && !body.at("skip").is_boolean()) throw std::invalid_argument("skip must be true or false");
      if (body.contains("abort")) {
        if (body.size() != 1 || body.at("abort") != json(true)) throw std::invalid_argument("abort must be true and the only control");
        session->provider->abort();
        session->provider->wait_paused();
        session->engine->synchronize().get();
        complete(ok({{"replay", session->state()}, {"aborted", true}}));
        return;
      }
      if (body.contains("play_until")) {
        for (const auto& [key, value] : body.items())
          if (key != "play_until" && key != "speed") throw std::invalid_argument("play_until takes only speed besides it");
        if (!body.at("play_until").is_string()) throw std::invalid_argument("play_until must be a time string");
        const auto target = control_time(body.at("play_until").get<std::string>(), session->windows, *session->provider, "play_until");
        session->provider->play_until(target, body.contains("speed") ? std::optional<int>(speed_field(body)) : std::nullopt);
        complete(ok({{"replay", session->state()}}));
        return;
      }
      if (body.contains("until")) {
        if (body.size() != 1 || !body.at("until").is_string())
          throw std::invalid_argument("until must be a time string and the only control");
        const auto target = control_time(body.at("until").get<std::string>(), session->windows, *session->provider, "until");
        const bool aborted = session->provider->until(target);
        complete(ok({{"replay", session->state()}, {"aborted", aborted}, {"settled_through", md::format_timestamp(session->provider->settled_through())}}));
        return;
      }
      if (body.contains("speed")) session->provider->set_speed(speed_field(body));
      if (body.contains("paused")) session->provider->set_paused(body.at("paused").get<bool>());
      if (body.contains("skip")) session->provider->skip(body.at("skip").get<bool>());
      if (body.value("paused", false)) {
        session->provider->wait_paused();
        session->engine->synchronize().get();
      }
      auto response = json{{"replay", session->state()}};
      if (session->provider->skip_pending()) response["message"] = "Skip queued for the next resume; skip:false cancels it";
      complete(ok(response));
    } else if (request.method == "DELETE") {
      stop_session();
      complete(ok({{"replay", nullptr}}));
    } else {
      complete(api_error(405, "METHOD_NOT_ALLOWED", "Use GET, POST, PUT or DELETE"));
    }
  } catch (const json::exception& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::invalid_argument& error) {
    complete(api_error(400, "INVALID_REQUEST", error.what()));
  } catch (const std::exception& error) {
    complete(api_error(422, "REPLAY_FAILED", error.what()));
  }
}

std::string ReplayHost::tick() const {
  const auto session = current();
  if (!session) return {};
  auto message = json::parse(tick_message(*session->engine));
  message["type"] = "replay_tick";
  message["replay"] = session->state();
  replay_gate(message, *session->provider);
  return message.dump();
}

}  // namespace openport::server
