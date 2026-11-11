#pragma once

#include "openport/providers/demo.hpp"
#include "openport/providers/replay.hpp"
#include "openport/providers/scenario.hpp"

namespace openport::providers {

/// An endless rotation of simulated regular sessions, on successive trading dates.
/// A restart resumes the date its accounts stopped on, after their latest market time.
class DemoProvider final : public md::Provider {
 public:
  struct Options {
    std::vector<std::string> days;  ///< Empty selects all built-in regular sessions.
    int speed = 1;
    md::Timestamp started = 0;  ///< Zero selects the server's start time.
    std::shared_ptr<ReplayClock> clock;
    int revision = kScenarioRevision;  ///< Select an older feed's output, calendar, timing and rotation.
  };
  explicit DemoProvider(Options options);
  ~DemoProvider() override;
  [[nodiscard]] std::string_view name() const noexcept override { return kDemoProvider; }
  [[nodiscard]] md::Capabilities capabilities() const noexcept override;
  [[nodiscard]] bool market_controls() const noexcept override { return true; }
  void start(const md::Subscription& subscription, md::EventSink& sink) override;
  void stop() override;
  void set_driver(ReplayProvider::Driver driver);
  /// Resume after recovered market time: the rest of its trading date, or the next
  /// trading date once that date has played to its last snapshot; zero keeps the
  /// default. May only be called before start().
  void start_after(md::Timestamp recovered_time);
  /// The market time a restart resumes after, or zero to start at the first date's open.
  [[nodiscard]] md::Timestamp resumed_after() const noexcept { return resume_after_; }
  /// The day `date` plays, as an index into days(): successive trading dates play
  /// successive days, counted from a fixed date, so a date plays the same day whenever
  /// the feed starts.
  [[nodiscard]] std::size_t day_on(md::Date date) const;
  [[nodiscard]] md::Timestamp time() const noexcept { return time_.load(); }
  [[nodiscard]] const std::vector<std::string>& symbols() const { return symbols_; }
  [[nodiscard]] const std::vector<Scenario>& days() const { return days_; }
  [[nodiscard]] int revision() const noexcept { return options_.revision; }
  [[nodiscard]] md::Date first_date() const { return first_date_; }
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
  void validate(const md::Subscription& subscription) const;

  /// Fit events with the shared generator clock; keeps older live revisions unchanged.
  [[nodiscard]] static Scenario on_date(Scenario scenario, md::Date date);
  [[nodiscard]] static md::Date next_date(md::Date date);
  [[nodiscard]] static std::uint64_t seed(std::string_view id, md::Date date);

 private:
  void run(md::Subscription subscription, md::EventSink& sink);
  Options options_;
  std::vector<Scenario> days_;
  std::vector<std::string> symbols_;
  md::Date first_date_;
  md::Timestamp resume_after_ = 0;
  std::filesystem::path directory_;
  ReplayProvider::Driver driver_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::atomic<bool> stopping_{false};
  std::atomic<md::Timestamp> time_{0};
  bool started_ = false;
};

/// Whose the directories named for this process's own PID are.
enum class OwnDirectories : bool {
  Live,      ///< this process's own: kept
  Orphaned,  ///< an earlier run's: this process has made none yet, as at openportd's startup
};
/// Removes the openport-feed-PID-* and openport-demo-PID-* directories that a process
/// no longer running left in the temporary directory, as a SIGKILL does; a running
/// process's directories and other users' are never touched. Returns how many went.
/// A container's openportd is PID 1 at every start, so a hard-killed run's directories
/// carry the PID of the process that starts after it: at startup, pass Orphaned.
std::size_t remove_orphaned_demo_directories(OwnDirectories own = OwnDirectories::Live);
/// The same sweep confined to a supplied temporary directory.
std::size_t remove_orphaned_demo_directories(const std::filesystem::path& temporary,
                                             OwnDirectories own = OwnDirectories::Live);

}  // namespace openport::providers
