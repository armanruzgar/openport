#include "openport/server/backtest.hpp"

#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace openport::server {
namespace {
using nlohmann::json;
bool run_id(std::string_view id) {
  return id.size() == 6 && std::all_of(id.begin(), id.end(), [](char value) { return value >= '0' && value <= '9'; });
}
json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("Cannot read saved backtest");
  return json::parse(input);
}
struct UnsafePath : std::runtime_error { using std::runtime_error::runtime_error; };
// Never follow a saved run or any child through a symlink, including metadata.
std::uintmax_t tree_bytes(const std::filesystem::path& directory) {
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory))) throw UnsafePath("Backtest storage contains a symlink");
  if (!std::filesystem::exists(directory)) return 0;
  std::uintmax_t total = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    const auto status = entry.symlink_status();
    if (std::filesystem::is_symlink(status)) throw UnsafePath("Backtest storage contains a symlink");
    if (std::filesystem::is_regular_file(status)) {
      std::error_code error;
      const auto bytes = entry.file_size(error);
      // Generated inputs and atomic-write temporaries can disappear during a read.
      if (!error) total += bytes;
      else if (error != std::errc::no_such_file_or_directory)
        throw std::filesystem::filesystem_error("Cannot size backtest file", entry.path(), error);
    }
  }
  return total;
}
bool finished(const json& state) {
  const auto status = state.at("status").get<std::string>();
  return status == "completed" || status == "cancelled" || status == "failed" || status == "interrupted";
}
}
struct BacktestHost::Impl {
  Options options;
  std::vector<providers::Scenario> scenarios;
  std::mutex mutex;
  std::atomic_bool cancel{false};
  bool running = false;
  json current = nullptr;
  std::thread worker;
  explicit Impl(Options value) : options(std::move(value)), scenarios(providers::load_scenarios(options.scenario_dir)) {}
  ~Impl() { cancel = true; if (worker.joinable()) worker.join(); }
  json with_storage_fields(const std::filesystem::path& directory, json state, bool report, std::uintmax_t size) const {
    if (std::filesystem::is_regular_file(directory / "report.json")) {
      const auto saved_report = read_json(directory / "report.json");
      if (report) state["report"] = saved_report;
      else if (saved_report.contains("summary")) {
        if (saved_report.value("mode", "") == "joint") {
          state["mode"] = "joint";
          state["playbooks"] = json::array();
          for (const auto& definition : saved_report.at("playbooks"))
            state["playbooks"].push_back({{"id", definition.at("id")}, {"version", definition.at("version")}, {"name", definition.at("name")}});
        } else if (saved_report.contains("playbook"))
          state["playbook"] = {{"id", saved_report.at("playbook").at("id")}, {"version", saved_report.at("playbook").at("version")}};
        state["summary"] = saved_report.at("summary");
      }
    }
    state["keep"] = state.value("keep", false);
    state["bytes"] = size;
    return state;
  }
  json saved(const std::string& id, bool report) const {
    const auto directory = options.directory / id;
    const auto size = tree_bytes(directory);
    auto state = read_json(directory / "state.json");
    if (!current.is_null() && current.at("id") == id) state = current;
    else if (state.at("status") == "running" || state.at("status") == "cancelling") state["status"] = "interrupted";
    return with_storage_fields(directory, std::move(state), report, size);
  }
  std::vector<std::string> ids() const {
    std::vector<std::string> result;
    if (std::filesystem::is_directory(options.directory))
      for (const auto& entry : std::filesystem::directory_iterator(options.directory))
        if (run_id(entry.path().filename().string()) && !entry.is_symlink() && entry.is_directory() &&
            std::filesystem::is_regular_file(entry.path() / "state.json")) result.push_back(entry.path().filename().string());
    std::sort(result.begin(), result.end());
    return result;
  }
  void purge(const std::string& id) {
    (void)tree_bytes(options.directory / id);
    std::filesystem::remove_all(options.directory / id);
    if (!current.is_null() && current.at("id") == id) current = nullptr;
  }
  void retain() {
    if (!options.keep) return;
    std::vector<std::string> removable;
    for (const auto& id : ids()) {
      const auto state = saved(id, false);
      if (finished(state) && !state.at("keep").get<bool>()) removable.push_back(id);
    }
    if (removable.size() <= options.keep) return;
    for (std::size_t index = 0; index < removable.size() - options.keep; ++index) purge(removable[index]);
  }

};
BacktestHost::BacktestHost(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
BacktestHost::~BacktestHost() = default;
bool BacktestHost::handle(const ApiRequest& request, const MetricsSource& source, const ApiCompletion& complete) {
  const auto path = request.target.substr(0, request.target.find('?'));
  if (path != "/api/backtests" && !path.starts_with("/api/backtests/")) return false;
  auto& host = *impl_;
  const bool sweep = path == "/api/backtests/sweeps" || path.starts_with("/api/backtests/sweeps/");
  const auto root = sweep ? std::string("/api/backtests/sweeps") : std::string("/api/backtests");
  const auto id = path == root ? std::string{} : path.substr(root.size() + 1);
  if (!id.empty() && (id != "compare" || sweep) && !run_id(id)) { complete(api_error(404, "NOT_FOUND", "Unknown backtest")); return true; }
  ApiResponse response;
  try {
    const std::lock_guard lock(host.mutex);
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(host.options.directory))) throw UnsafePath("Backtest root is a symlink");
    const auto question = request.target.find('?');
    const auto query = query_parameters(question == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(question + 1));
    if (!query) throw std::invalid_argument("Invalid query parameters");
    if (sweep && request.method != "GET" && request.method != "POST" && request.method != "DELETE") {
      complete(api_error(405, "METHOD_NOT_ALLOWED", "Unsupported sweep method")); return true;
    }
    if (!id.empty() && id != "compare") {
      const auto state = !host.current.is_null() && host.current.at("id") == id ? host.current
          : std::filesystem::is_regular_file(host.options.directory / id / "state.json") ? host.saved(id, false) : json();
      if (!state.is_null() && state.value("kind", "backtest") != (sweep ? "sweep" : "backtest")) {
        complete(api_error(404, "NOT_FOUND", "Unknown backtest")); return true;
      }
    }
    const bool purge = request.method == "DELETE" && query->contains("purge") && query->at("purge") == "true";
    if (sweep && !query->empty() && !(purge && query->size() == 1))
      throw std::invalid_argument("Sweeps accept only purge=true when deleting a saved sweep");
    for (const auto& [key, value] : *query) {
      (void)value;
      if (!(request.method == "GET" && id == "compare" && key == "ids") &&
          !(request.method == "GET" && id.empty() && key == "playbook") && !(purge && key == "purge"))
        throw std::invalid_argument("Unknown backtest query parameter");
    }
    if (id == "compare" && request.method == "GET") {
      if (!query->contains("ids")) throw std::invalid_argument("ids must name 2–8 distinct saved runs");
      std::vector<std::string> selected;
      std::istringstream input(query->at("ids"));
      std::string item;
      while (std::getline(input, item, ',')) {
        if (!run_id(item) || std::find(selected.begin(), selected.end(), item) != selected.end())
          throw std::invalid_argument("ids must name 2–8 distinct saved runs");
        selected.push_back(item);
      }
      if (selected.size() < 2 || selected.size() > 8 || query->at("ids").back() == ',')
        throw std::invalid_argument("ids must name 2–8 distinct saved runs");
      json runs = json::array();
      for (const auto& selected_id : selected) {
        if (host.running && host.current.at("id") == selected_id) {
          response = api_error(409, "BACKTEST_RUNNING", "Wait for backtests to finish before comparing"); break;
        }
        if (!std::filesystem::is_regular_file(host.options.directory / selected_id / "state.json")) {
          response = api_error(404, "NOT_FOUND", "Unknown backtest"); break;
        }
        const auto state = host.saved(selected_id, true);
        if (!finished(state)) { response = api_error(409, "BACKTEST_RUNNING", "Wait for backtests to finish before comparing"); break; }
        if (!state.contains("report") || !state.at("report").is_object() || !state.at("report").contains("summary")) {
          response = api_error(422, "BACKTEST_REPORT_UNAVAILABLE", "No completed or partial report is available"); break;
        }
        runs.push_back(state);
      }
      if (response.status == 200) {
        try { response.body = compare_backtests(runs).dump(); }
        catch (const trading::TradingError& error) { response = api_error(422, "BACKTEST_COMPARISON_UNAVAILABLE", error.what()); }
      }
    } else if (id == "compare") response = api_error(405, "METHOD_NOT_ALLOWED", "Unsupported comparison method");
    else if (request.method == "GET") {
      if (!id.empty()) {
        if (!std::filesystem::is_regular_file(host.options.directory / id / "state.json")) response = api_error(404, "NOT_FOUND", "Unknown backtest");
        else {
          const auto state = host.saved(id, true);
          if (state.value("kind", "backtest") != (sweep ? "sweep" : "backtest")) response = api_error(404, "NOT_FOUND", "Unknown backtest");
          else response.body = state.dump();
        }
      } else {
        json runs = json::array();
        auto ids = host.ids();
        std::reverse(ids.begin(), ids.end());
        for (const auto& item : ids) {
          const auto state = host.saved(item, false);
          if (state.value("kind", "backtest") == (sweep ? "sweep" : "backtest")) runs.push_back(state);
        }
        if (!host.current.is_null() && host.current.value("kind", "backtest") == (sweep ? "sweep" : "backtest") && std::find(ids.begin(), ids.end(), host.current.at("id").get<std::string>()) == ids.end()) runs.insert(runs.begin(), host.current);
        if (query->contains("playbook")) {
          const auto& selector = query->at("playbook");
          if (selector.empty()) throw std::invalid_argument("playbook filter must be ID or ID@VERSION");
          const auto matches = [&](const json& definition) {
            const auto name = definition.at("id").get<std::string>();
            return selector == name || selector == name + "@" + std::to_string(definition.at("version").get<unsigned>());
          };
          json filtered = json::array();
          for (const auto& run : runs) {
            bool match = run.contains("playbook") && matches(run.at("playbook"));
            if (run.contains("playbooks"))
              for (const auto& definition : run.at("playbooks")) match = match || matches(definition);
            if (match) filtered.push_back(run);
          }
          runs = std::move(filtered);
        }
        response.body = json{{"runs", runs}, {"active", host.running && host.current.value("kind", "backtest") == (sweep ? "sweep" : "backtest") ? host.current.at("id") : json(nullptr)}, {"label", kBacktestLabel}}.dump();
      }
    } else if (!host.options.enabled || source.status().trading.write == "disabled") {
      response = api_error(403, "WRITE_DISABLED", "Backtest writes are disabled");
    } else if (request.method == "DELETE") {
      if (!request.body.empty()) throw std::invalid_argument("DELETE must have no body");
      if (purge) {
        if (id.empty()) throw std::invalid_argument("Purging requires a saved run id");
        if (host.running && host.current.at("id") == id) response = api_error(409, "BACKTEST_RUNNING", "Cancel the active run and wait before deleting it");
        else if (!std::filesystem::is_regular_file(host.options.directory / id / "state.json")) response = api_error(404, "NOT_FOUND", "Unknown backtest");
        else if (!finished(host.saved(id, false))) response = api_error(409, "BACKTEST_RUNNING", "Run is not finished");
        else { host.purge(id); response.body = json{{"id", id}, {"deleted", true}}.dump(); }
      } else if (!host.running || host.current.value("kind", "backtest") != (sweep ? "sweep" : "backtest") || (!id.empty() && host.current.at("id") != id)) response = api_error(409, "BACKTEST_NOT_RUNNING", "No matching backtest is running");
      else {
        host.cancel = true;
        host.current["status"] = "cancelling";
        response.body = host.current.dump();
      }
    } else if (request.method == "PUT" && !id.empty()) {
      const auto body = json::parse(request.body);
      if (!body.is_object() || body.size() != 1 || !body.contains("keep") || !body.at("keep").is_boolean())
        throw std::invalid_argument("Expected {keep: true|false}");
      if (!host.current.is_null() && host.current.at("id") == id) {
        const auto directory = host.options.directory / id;
        if (std::filesystem::exists(directory)) (void)tree_bytes(directory);
        auto state = host.current;
        state["keep"] = body.at("keep");
        if (std::filesystem::is_directory(directory)) write_backtest_report(directory / "state.json", state);
        host.current = state;
        const auto size = tree_bytes(directory);
        response.body = host.with_storage_fields(directory, std::move(state), true, size).dump();
      } else if (!std::filesystem::is_regular_file(host.options.directory / id / "state.json")) response = api_error(404, "NOT_FOUND", "Unknown backtest");
      else {
        auto state = host.saved(id, false);
        state["keep"] = body.at("keep");
        state.erase("bytes"); state.erase("summary"); state.erase("playbook"); state.erase("playbooks"); state.erase("mode");
        write_backtest_report(host.options.directory / id / "state.json", state);
        response.body = host.saved(id, true).dump();
      }
    } else if (request.method == "POST" && id.empty()) {
      if (host.running) response = api_error(409, "BACKTEST_RUNNING", "A backtest is already running");
      else {
        const auto view = source.trading_view();
        if (!view || view->playbooks_json.empty()) throw std::invalid_argument("Playbooks are unavailable");
        BacktestRequest job;
        json grid;
        std::size_t variants = 1;
        try {
          auto body = json::parse(request.body);
          if (sweep) { grid = body.at("parameter_grid"); body.erase("parameter_grid"); }
          job = parse_backtest(body, json::parse(view->playbooks_json), host.scenarios, host.options.recordings);
          if (sweep) variants = expand_backtest_grid(job, grid).size();
        }
        catch (const std::exception& error) { throw std::invalid_argument(error.what()); }
        job.analytics = host.options.analytics;
        job.dividends = host.options.dividends;
        job.actor = request.actor;
        if (host.options.directory.empty()) throw std::invalid_argument("Backtest storage is unavailable");
        std::filesystem::create_directories(host.options.directory);
        const auto sequence = host.options.directory / "sequence.json";
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(sequence)) ||
            std::filesystem::is_symlink(std::filesystem::symlink_status(sequence.string() + ".tmp"))) throw UnsafePath("Backtest sequence is a symlink");
        unsigned number = std::filesystem::is_regular_file(sequence) ? read_json(sequence).at("next").get<unsigned>() : 1;
        for (const auto& entry : std::filesystem::directory_iterator(host.options.directory)) {
          const auto name = entry.path().filename().string();
          if (run_id(name)) number = std::max(number, static_cast<unsigned>(std::stoul(name)) + 1);
        }
        if (!number || number > 999999) throw std::runtime_error("Backtest directory is full");
        write_backtest_report(sequence, {{"next", number + 1}});
        host.retain();
        std::ostringstream name;
        name << std::setfill('0') << std::setw(6) << number;
        const auto directory = host.options.directory / name.str();
        // run_backtest requires a new directory. Persist the initial state from its
        // first progress callback; GET also uses the in-memory state while starting.
        if (host.worker.joinable()) host.worker.join();
        host.cancel = false;
        host.current = {{"id", name.str()}, {"status", "running"}, {"phase", "days"}, {"completed", 0},
            {"keep", false}, {"bytes", 0}, {"total", job.days.size() * 2 * variants}, {"directory", std::filesystem::absolute(directory).string()}, {"label", kBacktestLabel}, {"report", nullptr}};
        if (sweep) host.current["kind"] = "sweep";
        host.running = true;
        try {
          host.worker = std::thread([&host, job = std::move(job), directory, grid] {
            json report;
            try {
              const auto progress = [&](std::size_t completed, std::string_view phase) {
                const std::lock_guard progress_lock(host.mutex);
                host.current["completed"] = completed;
                host.current["phase"] = phase;
                write_backtest_report(directory / "state.json", host.current);
              };
              report = grid.is_null() ? run_backtest(job, directory, host.cancel, progress)
                  : run_backtest_sweep(job, grid, directory, host.cancel, progress);
            } catch (const std::exception& error) {
              report = {{"status", "failed"}, {"message", error.what()}};
            }
            const std::lock_guard finished_lock(host.mutex);
            host.current["status"] = report.at("status");
            host.current["phase"] = "finished";
            if (report.contains("message")) host.current["error"] = report.at("message");
            try { write_backtest_report(directory / "state.json", host.current); }
            catch (const std::exception& error) { host.current["status"] = "failed"; host.current["error"] = error.what(); }
            host.running = false;
          });
        } catch (...) {
          host.running = false;
          host.current["status"] = "failed";
          throw;
        }
        response.status = 202;
        response.body = host.current.dump();
      }
    } else response = api_error(405, "METHOD_NOT_ALLOWED", "Unsupported backtest method");
    // A newly queued job may not have created its directory yet.
    if (request.method == "GET" && !id.empty() && !host.current.is_null() && host.current.at("id") == id && host.current.value("kind", "backtest") == (sweep ? "sweep" : "backtest") && response.status == 404) {
      response.status = 200;
      response.body = host.current.dump();
    }
  } catch (const UnsafePath& error) { response = api_error(409, "BACKTEST_UNSAFE_PATH", error.what()); }
  catch (const std::invalid_argument& error) { response = api_error(400, "INVALID_REQUEST", error.what()); }
  catch (const json::exception& error) { response = api_error(400, "INVALID_REQUEST", error.what()); }
  catch (const std::exception& error) { response = api_error(500, "BACKTEST_IO", error.what()); }
  complete(std::move(response));
  return true;
}
}  // namespace openport::server
