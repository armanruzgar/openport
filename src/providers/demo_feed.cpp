#include "openport/providers/demo_feed.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

#include "openport/md/recording.hpp"
#include "openport/providers/factory.hpp"

namespace openport::providers {
namespace {
class DayStatus final : public md::EventSink {
 public:
  explicit DayStatus(md::EventSink& sink) : sink_(sink) {}
  void publish(md::Event event) override {
    const auto* status = std::get_if<md::ProviderStatus>(&event);
    if (status && status->state == md::FeedState::Stopped) return;
    if (status && status->state == md::FeedState::Error) failed = true;
    sink_.publish(std::move(event));
  }
  std::atomic<bool> failed{false};
 private:
  md::EventSink& sink_;
};

/// What the batches a restart's accounts already saw left in the book: every
/// definition, the official closes the day opened with, and each contract's and
/// underlying's latest values. The first batch after them carries these, so the book
/// is whole again from it on. Halts are not repeated: the desk keeps them across a
/// restart (market-halts.json), and a second copy would list one twice.
class CatchUp {
 public:
  void absorb(std::vector<md::Event>& events) {
    for (auto& event : events) {
      std::visit([&](auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, md::ContractDefinition>) definitions_.emplace_back(std::move(value));
        else if constexpr (std::is_same_v<T, md::OpenInterest>) interest_[value.id] = value;
        else if constexpr (std::is_same_v<T, md::OptionQuote>) quotes_[value.id] = value;
        else if constexpr (std::is_same_v<T, md::OptionVolume>) volumes_[value.id] = value;
        else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) spots_[value.symbol] = std::move(value);
        else if constexpr (std::is_same_v<T, md::UnderlyingClose>) {
          // A day that authors its previous close opens with it; nothing else sends it.
          const auto key = std::pair{value.symbol, value.date};
          closes_[key] = std::move(value);
        }
      }, event);
    }
  }
  /// Leads `events`, a batch at `time`, with what they do not themselves bring up to date.
  void lead(std::vector<md::Event>& events, md::Timestamp time) {
    if (definitions_.empty() && closes_.empty()) return;
    for (const auto& event : events) {
      std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, md::OpenInterest>) interest_.erase(value.id);
        else if constexpr (std::is_same_v<T, md::OptionQuote>) quotes_.erase(value.id);
        else if constexpr (std::is_same_v<T, md::OptionVolume>) volumes_.erase(value.id);
        else if constexpr (std::is_same_v<T, md::UnderlyingQuote>) spots_.erase(value.symbol);
        else if constexpr (std::is_same_v<T, md::UnderlyingClose>) closes_.erase(std::pair{value.symbol, value.date});
      }, event);
    }
    std::vector<md::Event> lead(std::make_move_iterator(definitions_.begin()), std::make_move_iterator(definitions_.end()));
    // Stamped with the batch, as a fresh start's first day gets its previous closes.
    for (auto& [key, value] : closes_) {
      value.ts = time;
      lead.emplace_back(std::move(value));
    }
    for (auto& [id, value] : interest_) lead.emplace_back(std::move(value));
    for (auto& [symbol, value] : spots_) lead.emplace_back(std::move(value));
    for (auto& [id, value] : quotes_) lead.emplace_back(std::move(value));
    for (auto& [id, value] : volumes_) lead.emplace_back(std::move(value));
    events.insert(events.begin(), std::make_move_iterator(lead.begin()), std::make_move_iterator(lead.end()));
    *this = {};
  }

 private:
  std::vector<md::Event> definitions_;
  std::map<md::InstrumentId, md::OpenInterest> interest_;
  std::map<md::InstrumentId, md::OptionQuote> quotes_;
  std::map<md::InstrumentId, md::OptionVolume> volumes_;
  std::map<std::string, md::UnderlyingQuote> spots_;
  std::map<std::pair<std::string, md::Date>, md::UnderlyingClose> closes_;
};

/// The closes a generated day prints at its regular close, as that date's official ones.
/// Its prices alone print them: no option is priced, and nothing is recorded.
std::vector<md::Event> closes_of(const Scenario& day, md::Date date, std::uint64_t day_seed, int revision,
                                 const std::vector<std::string>& symbols) {
  class Closes final : public md::EventSink {
   public:
    Closes(md::Date on, const std::vector<std::string>& wanted)
        : on_(on), at_(md::new_york_to_utc(on, md::regular_close_hour(on), 0)), wanted_(wanted) {}
    void publish(md::Event event) override {
      const auto* spot = std::get_if<md::UnderlyingQuote>(&event);
      if (spot && spot->ts == at_ && std::find(wanted_.begin(), wanted_.end(), spot->symbol) != wanted_.end())
        closes.emplace_back(md::UnderlyingClose{spot->symbol, at_, on_, spot->last});
    }
    std::vector<md::Event> closes;

   private:
    md::Date on_;
    md::Timestamp at_;
    const std::vector<std::string>& wanted_;
  };
  Closes sink(date, symbols);
  play_scenario_prices(day, date, day_seed, sink, revision);
  return std::move(sink.closes);
}
}

DemoProvider::DemoProvider(Options options) : options_(std::move(options)) {
  if (options_.speed == 0 || !ReplayProvider::valid_speed(options_.speed))
    throw std::invalid_argument("demo: speed must be 1, 2, 5, 10, 30, 60, 120 or 300");
  if (options_.revision < 1 || options_.revision > kScenarioRevision)
    throw std::invalid_argument("demo: unsupported scenario revision");
  const auto supported = [&](const Scenario& day) {
    return options_.revision >= 3 || std::all_of(day.symbols.begin(), day.symbols.end(), [](const auto& symbol) {
      return symbol == "SPX" || symbol == "SPY" || symbol == "QQQ";
    });
  };
  const auto& builtins = builtin_scenarios();
  if (options_.days.empty()) {
    for (const auto& day : builtins)
      if (!day.overnight && day.sessions.empty() && supported(day) &&
          !day.previous_close && day.strike_window == 0) days_.push_back(day);
  } else {
    for (const auto& id : options_.days) {
      const auto it = std::find_if(builtins.begin(), builtins.end(), [&](const auto& day) { return day.id == id; });
      if (it == builtins.end()) throw std::invalid_argument("demo: unknown day " + id);
      if (it->overnight) throw std::invalid_argument("demo: day " + id + " is not a regular session");
      if (!it->sessions.empty()) throw std::invalid_argument("demo: day " + id + " spans several sessions; play it in Replay");
      if (!supported(*it)) throw std::invalid_argument("demo: day " + id + " needs revision 3");
      days_.push_back(*it);
    }
  }
  if (options_.revision >= 3)
    for (auto& day : days_)
      for (const auto* symbol : {"XSP", "NDX", "RUT", "VIX"})
        if (std::find(day.symbols.begin(), day.symbols.end(), symbol) == day.symbols.end()) day.symbols.emplace_back(symbol);
  for (const auto& day : days_)
    for (const auto& symbol : day.symbols)
      if (std::find(symbols_.begin(), symbols_.end(), symbol) == symbols_.end()) symbols_.push_back(symbol);
  const auto started = options_.started != 0 ? options_.started : md::now();
  first_date_ = md::previous_business_day(md::new_york_time(started).date);
  time_ = scenario_open(days_[day_on(first_date_)], first_date_);
}
DemoProvider::~DemoProvider() { stop(); }
md::Capabilities DemoProvider::capabilities() const noexcept {
  md::Capabilities out;
  out.poll_interval = std::chrono::seconds(15);
  out.open_interest = true;
  return out;
}
md::Date DemoProvider::next_date(md::Date date) {
  return md::trading_date(md::new_york_to_utc(date, 18, 0));
}
std::size_t DemoProvider::day_on(md::Date date) const {
  // Trading dates from the first of 2026, signed: a short count either way.
  constexpr md::Date origin{2026, 1, 2};
  std::int64_t count = 0;
  for (auto day = origin; day < date; day = next_date(day)) ++count;
  for (auto day = origin; date < day; day = md::previous_business_day(day)) --count;
  const auto size = static_cast<std::int64_t>(days_.size());
  return static_cast<std::size_t>((count % size + size) % size);
}
std::uint64_t DemoProvider::seed(std::string_view id, md::Date date) {
  // FNV-1a has specified bytes and arithmetic; std::hash is not stable across builds.
  std::uint64_t value = 14695981039346656037ULL;
  for (const unsigned char byte : std::string(id) + "|" + md::format_date(date)) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}
Scenario DemoProvider::on_date(Scenario scenario, md::Date date) {
  if (scenario.events_scaled) return scenario;
  const auto window = scenario_windows(scenario, date).front();
  for (auto& event : scenario.events) {
    if (event.type == "gap") continue;
    const auto at = scenario_event_time(event.at, window);
    if (event.minutes > 0)
      event.minutes = static_cast<int>((scenario_event_time(event.at, window, event.minutes) - at) / md::kNanosPerMinute);
    const auto minute = md::new_york_time(at).seconds / 60;
    event.at = (minute / 60 < 10 ? "0" : "") + std::to_string(minute / 60) + ":" +
        (minute % 60 < 10 ? "0" : "") + std::to_string(minute % 60);
  }
  scenario.events_scaled = true;
  return scenario;
}
void DemoProvider::validate(const md::Subscription& subscription) const {
  validate_subscription(kDemoProvider, subscription);
  if (subscription.underlyings.empty()) throw std::invalid_argument("demo: no symbols subscribed");
  for (const auto& symbol : subscription.underlyings)
    for (const auto& day : days_)
      if (std::find(day.symbols.begin(), day.symbols.end(), symbol) == day.symbols.end())
        throw std::invalid_argument("demo: symbol " + symbol + " is not covered by day " + day.id);
}
void DemoProvider::set_driver(ReplayProvider::Driver driver) {
  if (started_) throw std::logic_error("demo: install driver before start");
  driver_ = std::move(driver);
}
void DemoProvider::start_after(md::Timestamp recovered_time) {
  if (started_) throw std::logic_error("demo: select first date before start");
  if (recovered_time <= 0) return;
  const auto date = md::trading_date(recovered_time);
  const auto& day = days_[day_on(date)];
  const auto open = scenario_open(day, date);
  if (recovered_time >= scenario_close(day, date) + 15 * md::kNanosPerMinute) {
    // That date played to its last snapshot: the next one starts at its open.
    first_date_ = next_date(date);
    resume_after_ = 0;
    time_ = scenario_open(days_[day_on(first_date_)], first_date_);
    return;
  }
  // The rest of the date plays on; before its open, all of it.
  first_date_ = date;
  resume_after_ = recovered_time >= open ? recovered_time : 0;
  time_ = std::max(recovered_time, open);
}
void DemoProvider::start(const md::Subscription& subscription, md::EventSink& sink) {
  if (started_) throw std::logic_error("DemoProvider::start may be called only once");
  validate(subscription);
  // The process id lets a later start find and remove this after a SIGKILL.
  auto pattern = (std::filesystem::temp_directory_path() / ("openport-feed-" + std::to_string(::getpid()) + "-XXXXXX")).string();
  const auto* created = ::mkdtemp(pattern.data());
  if (!created) throw std::runtime_error("demo: cannot create private recording directory");
  directory_ = created;
  started_ = true;
  thread_ = std::thread([this, subscription, &sink] { run(subscription, sink); });
}
void DemoProvider::stop() {
  stopping_ = true;
  wake_.notify_all();
  // Only the rotation thread joins the replay. stop() interrupts its wait here.
  if (thread_.joinable()) thread_.join();
  std::error_code error;
  if (!directory_.empty()) std::filesystem::remove_all(directory_, error);
}
void DemoProvider::run(md::Subscription subscription, md::EventSink& sink) {
  try {
    std::map<std::string, md::InstrumentId> ids;
    std::set<md::InstrumentId> prior;
    const auto prepare = [&](md::Date date) {
      const auto day = on_date(days_[day_on(date)], date);
      const auto path = directory_ / (md::format_date(date) + ".oprec");
      return std::async(std::launch::async, [day, path, date, revision = options_.revision] {
        write_scenario_recording(path, day, date, seed(day.id, date), revision);
        return path;
      });
    };
    auto date = first_date_;
    auto pending = prepare(date);
    // The trading date before the first one, as the feed would have played it: its
    // closes are the first day's previous closes, which circuit breakers measure from.
    // Its prices alone give them, without a whole day generated and recorded beside
    // the first; a first day that authors its previous close brings its own.
    std::vector<md::Event> closes;
    if (!days_[day_on(date)].previous_close) {
      try {
        const auto before = md::previous_business_day(date);
        const auto previous = on_date(days_[day_on(before)], before);
        closes = closes_of(previous, before, seed(previous.id, before), options_.revision, subscription.underlyings);
      } catch (const std::exception&) {
        // Without them the first day has no breaker reference, as before a close is seen.
        closes.clear();
      }
    }
    bool resuming = resume_after_ > 0;
    while (!stopping_.load()) {
      const auto path = pending.get();
      if (stopping_.load()) break;
      const auto following = next_date(date);
      pending = prepare(following);
      ReplayProvider::Options playback;
      playback.file = path;
      playback.speed = options_.speed;
      playback.clock = options_.clock;
      // A restart plays the batches its accounts already saw unpaced, and the first one
      // after them brings the book up to date: the day goes on where it stopped.
      if (resuming) playback.start_at = resume_after_;
      auto replay = std::make_shared<ReplayProvider>(std::move(playback));
      std::unordered_map<md::InstrumentId, md::InstrumentId> remap;
      bool first_batch = true;
      CatchUp caught;
      const auto title = "demo: " + days_[day_on(date)].title + " · simulated prices";
      const bool authored_close = days_[day_on(date)].previous_close.has_value();
      if (authored_close) closes.clear();
      replay->set_driver([&, title, authored_close](ReplayBatch batch) {
        // A day's recording opens with the previous close its scenario starts from; the
        // feed keeps the close the day before it actually printed.
        if (!authored_close)
          std::erase_if(batch.events, [](const md::Event& event) { return std::holds_alternative<md::UnderlyingClose>(event); });
        for (auto& event : batch.events) {
          std::visit([&](auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, md::ContractDefinition>) {
              const auto key = value.contract.osi_symbol();
              if (ids.size() >= std::numeric_limits<md::InstrumentId>::max())
                throw std::runtime_error("demo: too many contracts");
              const auto [it, inserted] = ids.try_emplace(key, static_cast<md::InstrumentId>(ids.size()));
              (void)inserted;
              remap[value.id] = it->second;
              value.id = it->second;
            } else if constexpr (requires { value.id; }) {
              value.id = remap.at(value.id);
            } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
              value.message = value.state == md::FeedState::Stale ? title + ": " + value.message : title;
            }
          }, event);
        }
        if (first_batch) {
          // Every definition precedes the first snapshot. Retire quotes omitted
          // by the new day before its snapshot can vouch for the whole chain.
          std::set<md::InstrumentId> current;
          for (const auto& [original, mapped] : remap) { (void)original; current.insert(mapped); }
          std::vector<md::Event> retired;
          for (const auto id : prior)
            if (!current.contains(id)) retired.emplace_back(md::OptionQuote{id, batch.time, 0, 0, 0, 0});
          batch.events.insert(batch.events.begin(), retired.begin(), retired.end());
          prior = std::move(current);
          first_batch = false;
        }
        std::promise<void> done;
        done.set_value();
        if (resuming) {
          if (batch.time <= resume_after_) {
            caught.absorb(batch.events);
            return done.get_future();
          }
          caught.lead(batch.events, batch.time);
          resuming = false;
        }
        if (!closes.empty()) {
          for (auto& close : closes) std::get<md::UnderlyingClose>(close).ts = batch.time;
          batch.events.insert(batch.events.begin(), closes.begin(), closes.end());
          closes.clear();
        }
        // SnapshotComplete precedes the generator's status; publish health in the
        // same batch so the next day's first snapshot is immediately healthy.
        if (!batch.events.empty()) {
          if (const auto* complete = std::get_if<md::SnapshotComplete>(&batch.events.back()))
            batch.events.emplace_back(md::ProviderStatus{batch.time, md::FeedState::Live, title, complete->underlying});
        }
        time_ = batch.received;
        if (driver_) return driver_(std::move(batch));
        for (auto& event : batch.events) sink.publish(std::move(event));
        return done.get_future();
      });
      DayStatus status(sink);
      replay->start(subscription, status);
      {
        std::unique_lock lock(mutex_);
        while (!stopping_.load() && !replay->finished()) wake_.wait_for(lock, std::chrono::milliseconds(10));
      }
      replay->stop();
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + ".end");
      if (status.failed) break;
      // A day played to its end before the time a restart resumed after leaves nothing.
      resuming = false;
      date = following;
    }
  } catch (const std::exception& error) {
    sink.publish(md::ProviderStatus{time(), md::FeedState::Error, "demo: " + std::string(error.what()), ""});
  }
}

std::size_t remove_orphaned_demo_directories(const std::filesystem::path& temporary, OwnDirectories own) {
  std::size_t removed = 0;
  std::error_code iteration_error;
  std::filesystem::directory_iterator entry(temporary, iteration_error), end;
  // A disappearing entry is local to that entry. An unreadable directory ends
  // the iteration without throwing out of a server's startup cleanup.
  for (; entry != end; entry.increment(iteration_error)) {
    const auto name = entry->path().filename().string();
    std::string_view rest;
    for (const std::string_view prefix : {"openport-feed-", "openport-demo-"})
      if (std::string_view(name).starts_with(prefix)) rest = std::string_view(name).substr(prefix.size());
    const auto dash = rest.find('-');
    if (dash == 0 || dash == std::string_view::npos) continue;
    pid_t pid = 0;
    const auto [parsed_end, error] = std::from_chars(rest.data(), rest.data() + dash, pid);
    if (error != std::errc{} || parsed_end != rest.data() + dash || pid <= 0) continue;
    const bool ours = pid == ::getpid();
    if (ours && own == OwnDirectories::Live) continue;
    struct stat info {};
    if (::lstat(entry->path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid()) continue;
    // Only a process that no longer exists gives its directories up; before this one
    // has made any, those named for its own PID are an earlier run's.
    if (!ours && (::kill(pid, 0) == 0 || errno != ESRCH)) continue;
    // Sweep children independently: another server can remove any of them while
    // we walk. A vanished child must not prevent removing the now-empty root.
    std::error_code contents_error;
    std::filesystem::directory_iterator child(entry->path(), contents_error), child_end;
    for (; child != child_end; child.increment(contents_error)) {
      std::error_code ignored;
      std::filesystem::remove_all(child->path(), ignored);
    }
    // Count successful root removals. A missing root or one whose children could
    // not be removed does not stop the rest of the sweep.
    std::error_code ec;
    if (std::filesystem::remove(entry->path(), ec)) ++removed;
  }
  return removed;
}

std::size_t remove_orphaned_demo_directories(OwnDirectories own) {
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  return error ? 0 : remove_orphaned_demo_directories(temporary, own);
}

}  // namespace openport::providers
