#include "openport/server/run.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <openssl/evp.h>
#include <stdexcept>
#include <unistd.h>

#include "openport/providers/replay_batches.hpp"
#include "run_json.hpp"

namespace openport::server {
namespace {
using nlohmann::json;
std::string hex_digest(const unsigned char* bytes, unsigned size) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (unsigned i = 0; i < size; ++i) {
    result.push_back(hex[bytes[i] >> 4]);
    result.push_back(hex[bytes[i] & 15]);
  }
  return result;
}
std::string hash_text(std::string_view text) {
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (EVP_Digest(text.data(), text.size(), bytes, &size, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 failed");
  return hex_digest(bytes, size);
}
std::string hash_file(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) throw std::runtime_error("Missing input: " + file.filename().string());
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 failed");
  std::array<char, 65536> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    if (EVP_DigestUpdate(context.get(), buffer.data(), static_cast<std::size_t>(input.gcount())) != 1)
      throw std::runtime_error("SHA-256 failed");
  }
  if (!input.eof()) throw std::runtime_error("Cannot read input: " + file.filename().string());
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (EVP_DigestFinal_ex(context.get(), bytes, &size) != 1) throw std::runtime_error("SHA-256 failed");
  return hex_digest(bytes, size);
}
json shareable_input(json input) {
  input.erase("file");
  if (input.contains("date")) input["date"] = md::format_date(input.at("date").get<md::Date>());
  else if (input.contains("started"))
    input["date"] = md::format_date(md::new_york_time(input.at("started").get<md::Timestamp>()).date);
  if (input.contains("seed")) input["seed"] = std::to_string(input.at("seed").get<std::uint64_t>());
  if (input.value("kind", "") == "scenario" && !input.contains("revision")) input["revision"] = 1;
  return input;
}
json describe_run(const std::filesystem::path& journal) {
  json run{{"id", journal.stem().string()}, {"file", journal.filename().string()},
           {"inputs", json::array()}, {"plan", "unknown"}};
  std::ifstream metadata(std::filesystem::path(journal).replace_extension(".json"));
  if (metadata) {
    const auto saved = json::parse(metadata, nullptr, false);
    if (saved.is_object()) {
      run["plan"] = saved.value("plan", "unknown");
      if (saved.contains("journal")) run["recorded_journal"] = saved.at("journal");
    }
  }
  // The initial state and start input are the first two records. Read them even
  // when a later record is damaged, so failure output still identifies the run.
  std::ifstream file(journal);
  std::string line;
  for (int index = 0; index < 2 && std::getline(file, line); ++index) {
    const auto record = json::parse(line, nullptr, false);
    if (!record.is_object() || !record.contains("payload")) continue;
    const auto& payload = record.at("payload");
    if (index == 0 && run.at("plan") == "unknown" && payload.contains("state"))
      run["plan"] = payload.at("state").at("config").at("rules").value("plan", "unknown");
    if (record.value("type", "") != "run_input") continue;
    for (const auto& event : payload.at("events")) {
      if (event.at("type") == "run_input" && event.at("payload").at("kind") == "start")
        run["inputs"].push_back(shareable_input(event.at("payload").at("input")));
    }
  }
  return run;
}
std::string run_label(const json& run) {
  std::string text = "Run " + run.at("id").get<std::string>() + " (" + run.at("file").get<std::string>() + ")";
  for (const auto& input : run.at("inputs")) {
    if (input.at("kind") == "scenario")
      text += "; scenario " + input.at("id").get<std::string>() + "; date " + input.at("date").get<std::string>() +
          "; seed " + input.at("seed").get<std::string>() + "; revision " + input.at("revision").dump();
    else text += "; recording " + input.at("name").get<std::string>() + "; date " + input.at("date").get<std::string>();
  }
  text += "; plan " + run.at("plan").get<std::string>();
  if (run.contains("recorded_journal")) {
    const auto& final = run.at("recorded_journal");
    text += "; recorded final " + final.at("transactions").dump() + " transactions; head " + final.at("head").get<std::string>();
  }
  return text + "\n";
}
struct TemporaryInput {
  std::filesystem::path directory;
  ~TemporaryInput() { std::error_code error; if (!directory.empty()) std::filesystem::remove_all(directory, error); }
  std::filesystem::path file() {
    auto pattern = (std::filesystem::temp_directory_path() / "openport-verify-XXXXXX").string();
    const auto* created = ::mkdtemp(pattern.data());
    if (!created) throw std::runtime_error("Cannot create verification directory");
    directory = created;
    return directory / "input.oprec";
  }
};
void omit_actors(json& value) {
  if (value.is_object()) {
    value.erase("actor");
    for (auto& child : value) omit_actors(child);
  } else if (value.is_array()) {
    for (auto& child : value) omit_actors(child);
  }
}
class ComparisonJournal final : public trading::Journal {
 public:
  /// `attributed` is false only for a run recorded before actors, whose records carry none.
  ComparisonJournal(const trading::JournalRecovery& expected, bool attributed)
      : expected_(expected), attributed_(attributed) {}
  void append(md::Timestamp time, std::string_view type, std::string_view payload) override {
    auto recorded = json::parse(payload);
    // Pre-actor runs used the same reducer and delta format. Reproduce their wire
    // representation for hashing. The choice is the whole run's: every record of an
    // attributed run stays byte-exact, so one whose actor was removed differs.
    if (!attributed_) omit_actors(recorded);
    if (sequence_ == expected_.records.size()) {
      // The journal ends here, part way through an operation: a crash cut off the rest.
      // Throwing stops the account, which keeps the journal's last state.
      cut = true;
      throw std::runtime_error("The journal ends part way through an operation");
    }
    json line{{"seq", sequence_ + 1}, {"time", time}, {"type", type}, {"payload", std::move(recorded)}, {"prev_hash", head_}};
    const auto hash = hash_text(line.dump());
    if (expected_.records[sequence_].hash != hash) {
      error = "First differing transaction " + std::to_string(sequence_ + 1) + " (" + std::string(type) + ")";
      throw std::runtime_error(error);
    }
    head_ = hash;
    ++sequence_;
  }
  std::uint64_t sequence() const override { return sequence_; }
  std::string head() const override { return head_; }
  std::string error;
  bool cut = false;
 private:
  const trading::JournalRecovery& expected_;
  bool attributed_;
  std::uint64_t sequence_ = 0;
  std::string head_ = std::string(64, '0');
};
class ResumingJournal final : public trading::Journal {
 public:
  ResumingJournal(const trading::JournalRecovery& expected, std::shared_ptr<trading::Journal> file)
      : file_(std::move(file)) {
    for (const auto& record : expected.records) hashes_.push_back(record.hash);
  }
  void append(md::Timestamp time, std::string_view type, std::string_view payload) override {
    if (sequence_ >= hashes_.size()) {
      file_->append(time, type, payload);
      sequence_ = file_->sequence();
      head_ = file_->head();
      return;
    }
    json line{{"seq", sequence_ + 1}, {"time", time}, {"type", type}, {"payload", json::parse(payload)}, {"prev_hash", head_}};
    const auto hash = hash_text(line.dump());
    if (hashes_[sequence_] != hash)
      throw std::runtime_error("The resumed run differs from its journal at transaction " + std::to_string(sequence_ + 1) +
                               " (" + std::string(type) + "); nothing was written");
    head_ = hash;
    ++sequence_;
  }
  void flush() override { file_->flush(); }
  std::uint64_t bytes() const override { return file_->bytes(); }
  std::uint64_t sequence() const override { return sequence_; }
  std::string head() const override { return head_; }
 private:
  std::shared_ptr<trading::Journal> file_;
  std::vector<std::string> hashes_;
  std::uint64_t sequence_ = 0;
  std::string head_ = std::string(64, '0');
};
}

std::vector<std::string> run_inputs(const trading::JournalRecovery& journal) {
  std::vector<std::string> inputs;
  for (const auto& record : journal.records) {
    if (record.type != "run_input") continue;
    const auto payload = json::parse(record.payload);
    for (const auto& event : payload.at("events"))
      if (event.at("type") == "run_input") inputs.push_back(event.at("payload").dump());
  }
  return inputs;
}
std::shared_ptr<trading::Journal> resuming_journal(const trading::JournalRecovery& expected,
                                                   std::shared_ptr<trading::Journal> file) {
  if (file->sequence() != expected.records.size() || file->head() != expected.head)
    throw std::invalid_argument("The resumed journal is not the one its run recorded");
  return std::make_shared<ResumingJournal>(expected, std::move(file));
}

std::string recording_input(const std::filesystem::path& file) {
  const auto hash = hash_file(file);
  md::RecordingReader reader(file);
  return json{{"version", 1}, {"kind", "recording"}, {"file", std::filesystem::absolute(file).string()},
      {"name", file.filename().string()}, {"size", std::filesystem::file_size(file)}, {"sha256", hash},
      {"started", reader.header().started}}.dump();
}
std::string scenario_input(const providers::Scenario& scenario, md::Date date, std::uint64_t seed) {
  return json{{"version", 1}, {"kind", "scenario"}, {"id", scenario.id}, {"file", scenario.source_file.string()},
      {"builtin", scenario.builtin}, {"sha256", hash_text(scenario.source)}, {"generator", scenario.generator},
      {"revision", providers::kScenarioRevision}, {"seed", seed}, {"date", date},
      {"started", providers::scenario_open(scenario, date)}}.dump();
}
RunIdentity run_identity(std::string_view input, std::string id) {
  RunIdentity run;
  run.id = std::move(id);
  const auto parsed = json::parse(input, nullptr, false);
  if (!parsed.is_object()) return run;
  if (parsed.value("kind", "") == "scenario") {
    run.scenario = parsed.value("id", "");
    if (const auto seed = parsed.find("seed"); seed != parsed.end() && seed->is_number_unsigned())
      run.seed = std::to_string(seed->get<std::uint64_t>());
    if (const auto date = parsed.find("date"); date != parsed.end() && date->is_object())
      run.date = md::format_date(date->get<md::Date>());
  } else {
    run.recording = parsed.value("name", "");
    if (const auto started = parsed.find("started"); started != parsed.end() && started->is_number_integer() && started->get<md::Timestamp>() > 0)
      run.date = md::format_date(md::new_york_time(started->get<md::Timestamp>()).date);
  }
  return run;
}
nlohmann::json verification_cost(std::uint64_t bytes, std::uint64_t records) {
  const auto seconds = std::max<std::uint64_t>(1, std::max(bytes ? 1 + (bytes - 1) / (10 * 1024 * 1024) : 0, records ? 1 + (records - 1) / 2000 : 0));
  return {{"estimated_seconds", seconds}, {"warning", seconds >= 30 ? nlohmann::json(
      "Long verification: roughly " + std::to_string(seconds) + " seconds or more; recording generation and hardware can take longer") : nlohmann::json(nullptr)}};
}
RunVerification verify_run(const std::filesystem::path& journal,
    const std::function<bool()>& cancelled,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
  RunVerification result;
  result.run = {{"id", journal.stem().string()}, {"file", journal.filename().string()},
                {"inputs", json::array()}, {"plan", "unknown"}};
  std::string label = run_label(result.run);
  std::vector<std::filesystem::path> private_paths{journal};
  const auto cancelled_now = [&] { return cancelled && cancelled(); };
  try {
    result.run = describe_run(journal);
    label = run_label(result.run);
    const auto expected = trading::FileJournal::read(journal.string());
    if (expected.truncated_final_line)
      throw std::runtime_error("Journal has a torn final line; --repair-journals would cut " + std::to_string(expected.bytes_cut) + " bytes");
    if (result.run.contains("recorded_journal")) {
      const auto& final = result.run.at("recorded_journal");
      if (final.at("transactions") != expected.records.size() || final.at("head") != expected.head ||
          final.at("bytes") != std::filesystem::file_size(journal))
        throw std::runtime_error("Journal disagrees with recorded final: expected " + final.at("transactions").dump() +
            " transactions, found " + std::to_string(expected.records.size()) + "; expected head " +
            final.at("head").get<std::string>() + ", found " + expected.head);
      label += "Journal agrees with recorded final head/count/bytes.\n";
    }
    std::vector<json> inputs;
    for (const auto& record : expected.records) {
      if (record.type != "run_input") continue;
      const auto payload = json::parse(record.payload);
      for (const auto& event : payload.at("events"))
        if (event.at("type") == "run_input") inputs.push_back(event.at("payload"));
    }
    if (inputs.empty() || inputs.front().at("kind") != "start")
      throw std::runtime_error("Journal has no reproducible-run metadata (older journals still load as accounts)");
    result.run["inputs"] = json::array();
    for (const auto& operation : inputs)
      if (operation.at("kind") == "start" || operation.at("kind") == "source")
        result.run["inputs"].push_back(shareable_input(operation.at("input")));
    const auto& start = inputs.front();
    const auto& input = start.at("input");
    const md::ScheduledDaysScope calendar(start.value("calendar", std::vector<md::ScheduledDay>{}));
    std::unique_ptr<TemporaryInput> generated_input;
    const auto open_input = [&](const json& identity) {
      if (identity.at("version") != 1) throw std::runtime_error("Unsupported run driver version");
      if (identity.contains("file")) private_paths.emplace_back(identity.at("file").get<std::string>());
      auto generated = std::make_unique<TemporaryInput>();
      std::filesystem::path file;
      if (identity.at("kind") == "recording") {
        file = identity.at("file").get<std::string>();
        if (hash_file(file) != identity.at("sha256").get<std::string>() ||
            std::filesystem::file_size(file) != identity.at("size").get<std::uintmax_t>())
          throw std::runtime_error("Recording input changed: " + file.filename().string());
      } else if (identity.at("kind") == "scenario") {
        providers::Scenario scenario;
        if (identity.at("builtin").get<bool>()) {
          bool found = false;
          for (const auto& candidate : providers::builtin_scenarios()) {
            if (candidate.id != identity.at("id").get<std::string>()) continue;
            scenario = candidate;
            found = true;
            break;
          }
          if (!found) throw std::runtime_error("Missing built-in scenario: " + identity.at("id").get<std::string>());
        } else {
          file = identity.at("file").get<std::string>();
          if (!std::filesystem::is_regular_file(file)) throw std::runtime_error("Missing scenario input: " + file.filename().string());
          scenario = providers::read_scenario(file);
        }
        if (hash_text(scenario.source) != identity.at("sha256").get<std::string>())
          throw std::runtime_error("Scenario input changed: " + scenario.id);
        if (scenario.generator != identity.at("generator").get<decltype(scenario.generator)>()) throw std::runtime_error("Scenario generator version changed");
        // Runs from before revisions regenerate the first revision's chain.
        const auto revision = identity.value("revision", 1);
        if (revision < 1 || revision > providers::kScenarioRevision) throw std::runtime_error("Unsupported scenario revision");
        file = generated->file();
        private_paths.push_back(file);
        providers::write_scenario_recording(file, scenario, identity.at("date").get<md::Date>(), identity.at("seed").get<std::uint64_t>(),
                                            revision);
      } else throw std::runtime_error("Unknown run input kind");
      auto next_reader = std::make_unique<md::RecordingReader>(file);
      generated_input = std::move(generated);
      return next_reader;
    };
    const auto restored = trading::TradingSession::recover(expected);
    result.time = restored.snapshot()->time;
    if (cancelled_now()) throw std::runtime_error("Verification cancelled");
    if (progress) progress(0, expected.records.size());
    auto reader = open_input(input);
    if (cancelled_now()) throw std::runtime_error("Verification cancelled");
    const md::Subscription subscription{start.at("symbols").get<std::vector<std::string>>(), 0, 0};
    // Driver 2 batches each market instant whole, driver 3 also rolls the day over on the
    // closing marks before a new date's quotes, and driver 4 also records each command's
    // input before its transactions. Driver 5 settles AM options on opening prints.
    // Driver 6 also labels playbook time-stop cancellations.
    // Older runs replay as they were recorded.
    const auto driver = start.value("driver", 1);
    if (driver < 1 || driver > 6) throw std::runtime_error("Unsupported run driver version");
    const bool instants = driver >= 2;
    auto batches = std::make_unique<providers::ReplayBatches>(*reader, subscription, instants);
    // Every build that writes a driver version also attributes every record, so only a
    // driver 1 run whose first record has no actor predates actors.
    const bool attributed = driver != 1 || json::parse(expected.records.front().payload).contains("actor");
    auto comparison = std::make_shared<ComparisonJournal>(expected, attributed);
    Desk::Options options;
    options.replay = true;
    options.instant_batches = instants;
    options.closing_rollover = driver >= 3;
    options.inputs_first = driver >= 4;
    options.opening_settlement = driver >= 5;
    options.playbook_cancel_labels = driver >= 6;
    options.exact_margin = start.value("exact_margin", false);
    options.opening_rule_checks = start.value("opening_rule_checks", false);
    options.compact_opening_rule_inputs = start.value("compact_opening_rule_inputs", false);
    options.candles = std::make_shared<CandleStore>();
    options.run_input = input.dump();
    if (start.contains("playbooks") && !start.at("playbooks").is_null()) options.initial_playbooks = start.at("playbooks").dump();
    options.paper_sink = comparison;
    options.initial_actor = json::parse(expected.records.front().payload).value("actor", std::string("system"));
    options.paper = json::parse(expected.records.front().payload).at("state").at("config").get<trading::SessionConfig>();
    options.analytics = start.at("analytics").get<analytics::AnalyticsOptions>();
    options.dividends = start.at("dividends").get<std::vector<trading::Dividend>>();
    Desk desk("replay (" + reader->header().provider + ")", reader->header().capabilities, subscription, options);
    desk.start_trading();
    for (std::size_t index = 1; index < inputs.size() && comparison->error.empty() && !comparison->cut; ++index) {
      if (cancelled_now()) throw std::runtime_error("Verification cancelled");
      if (progress && index % 64 == 0) progress(comparison->sequence(), expected.records.size());
      const auto& operation = inputs[index];
      if (operation.at("kind") == "boundary") {
        auto batch = batches->next();
        if (!batch || batch->events.size() != operation.at("events").get<std::size_t>() ||
            batch->received != operation.at("driver_time").get<md::Timestamp>())
          throw std::runtime_error("Input boundary differs at operation " + std::to_string(index));
        desk.replay_batch(batch->events, batch->received, batch->time);
      } else if (operation.at("kind") == "source") {
        batches.reset();  // It reads through the reader replaced next.
        reader = open_input(operation.at("input"));
        batches = std::make_unique<providers::ReplayBatches>(*reader, subscription, instants);
        desk.replay_source(operation.at("input").dump(), reader->header());
      } else if (operation.at("kind") == "command") {
        desk.command(operation.at("command").get<TradingCommand>(), [](TradingReply) {},
                     operation.at("time").get<md::Timestamp>(), operation.at("driver_time").get<md::Timestamp>());
      } else if (operation.at("kind") == "dividends") {
        desk.set_dividends(operation.at("dividends").get<std::vector<trading::Dividend>>());
      } else throw std::runtime_error("Unknown recorded run operation");
    }
    // A batch's transactions precede its boundary input. When a crash cut that input
    // off, the recording's next batch must reproduce the transactions after the last one.
    if (comparison->error.empty() && !comparison->cut && comparison->sequence() < expected.records.size()) {
      if (const auto batch = batches->next()) desk.replay_batch(batch->events, batch->received, batch->time);
    }
    if (!comparison->error.empty()) throw std::runtime_error(comparison->error);
    if (comparison->sequence() != expected.records.size())
      throw std::runtime_error("First differing transaction " + std::to_string(comparison->sequence() + 1) + " (" +
                               expected.records[comparison->sequence()].type + ", after the last recorded input)");
    const auto view = desk.trading_view();
    if (!view || view->snapshot->equity != restored.snapshot()->equity || comparison->head() != expected.head)
      throw std::runtime_error("Final equity or head hash differs");
    result.matched = true;
    result.cut = comparison->cut;
    result.transactions = comparison->sequence();
    result.head = comparison->head();
    result.equity = view->snapshot->equity;
    result.message = "Verified " + std::to_string(result.transactions) + " transactions; equity " +
        std::to_string(result.equity.micros()) + " micro-dollars; head " + result.head +
        (result.cut ? "; the journal ends part way through its last operation, as a crash leaves it" : "");
  } catch (const std::filesystem::filesystem_error& error) { result.message = error.code().message(); }
  catch (const std::exception& error) { result.message = error.what(); }
  for (const auto& file : private_paths) {
    if (file.empty() || !file.is_absolute()) continue;
    const auto path = file.string();
    for (auto at = result.message.find(path); at != std::string::npos; at = result.message.find(path, at + file.filename().string().size()))
      result.message.replace(at, path.size(), file.filename().string());
  }
  result.message = label + result.message;
  return result;
}
}  // namespace openport::server
