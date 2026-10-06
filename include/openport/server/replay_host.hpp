#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "openport/server/api.hpp"
#include "openport/providers/scenario.hpp"
#include "openport/server/engine.hpp"

namespace openport::server {

/// Plays recordings back beside the live feed, one at a time: each replay has its
/// own engine, isolated paper account and chart history, all on the replay's
/// clock (the recorded receipt times), so sessions, delays and feed checks behave
/// as they did that day. Its API mirrors the live one under /api/replay/..., so the
/// terminal trades a recorded day exactly as it trades today.
///
///   GET    /api/replay   recordings, scenarios, history and the replay running
///   POST   /api/replay   {file | scenario, speed?, plan?, seed?, start_at?, paused?}
///                        or {resume: a saved run's id, speed?, paused?}: continue it
///   PUT    /api/replay   {speed?, paused?, skip? | until}: control it
///   DELETE /api/replay   stop it
///   *      /api/replay/X the live route /api/X, on the replay
///
/// Starts, controls and history deletes run one at a time on the host's own thread
/// and complete from there, so a lockstep step or a scenario's generation never
/// holds an HTTP thread; listings, reads and the mirrored routes answer at once.
class ReplayHost {
 public:
  struct Options {
    /// Recordings are listed from and read in this directory only.
    std::filesystem::path recordings;
    /// Analytics, paper and write settings. Replay journals live beside the main
    /// journal in replays/; generated recordings and candles remain temporary.
    Engine::Options engine;
    /// Offer simulated scenarios, cached by date and seed in a temporary directory.
    /// Listing prepares the default scenario with its own seed in the background.
    bool demo = true;
    std::filesystem::path scenario_dir = {};   ///< User additions and overrides.
    /// Immutable live account publications, including main and archived accounts.
    std::function<std::shared_ptr<const TradingView>(std::string_view)> settings_source = {};
    std::function<void(std::uint64_t, std::uint64_t)> verification_progress = {};  ///< Optional worker progress observer.
  };

  explicit ReplayHost(Options options);
  ~ReplayHost();
  ReplayHost(const ReplayHost&) = delete;
  ReplayHost& operator=(const ReplayHost&) = delete;

  /// Handles /api/replay and every /api/replay/... route; false for any other target.
  /// Changes may complete after this returns, on the host's control thread.
  bool handle(const ApiRequest& request, const ApiCompletion& complete);
  /// The replay's WebSocket message, a tick typed "replay_tick" with the replay's
  /// state, or empty while none runs.
  [[nodiscard]] std::string tick() const;
  /// Terminal shutdown: closes the job queue. DELETE /api/replay stops only the session.
  void stop();
  /// The dividends replays started from now on pay (Options::engine.dividends at first).
  void set_dividends(std::vector<trading::Dividend> dividends);
  /// Journal recovery attempts for saved-run summaries and full archives.
  [[nodiscard]] std::uint64_t history_recoveries() const;

 private:
  struct Session;
  class DemoRecordings;
  class History;
  void stop_session(bool shutdown = false);  // control_mutex_ held
  void control(const ApiRequest& request, const ApiCompletion& complete);
  void resume(const std::string& id, int speed, bool paused, const ApiCompletion& complete,
              bool restart = false, const std::string& at = {});
  void history(const ApiRequest& request, const ApiCompletion& complete);
  /// Queues a change for the control thread, which runs each under control_mutex_.
  void enqueue(const ApiRequest& request, const ApiCompletion& complete);
  void work();
  void stop_verifier();  // control_mutex_ held
  [[nodiscard]] std::shared_ptr<Session> current() const;

  Options options_;
  // Sessions play the demo's files, so they are declared after it and stop first.
  std::unique_ptr<DemoRecordings> demos_;
  std::vector<providers::Scenario> scenarios_;
  std::unique_ptr<History> history_;
  std::mutex control_mutex_;  // serializes controls and history handoffs
  /// Held while a journal passes between running and history (a start, a stop, a
  /// delete), so listings and archive reads never see one half written.
  std::mutex handoff_mutex_;
  mutable std::mutex mutex_;
  std::shared_ptr<Session> session_;
  struct Job {
    ApiRequest request;
    ApiCompletion complete;
  };
  std::mutex jobs_mutex_;
  std::condition_variable jobs_ready_;
  std::deque<Job> jobs_;  // jobs_mutex_
  bool closing_ = false;  // jobs_mutex_
  std::thread worker_;
  std::atomic_bool verifier_stop_{false};
  std::thread verifier_;  // launches under control_mutex_; never uses the live engine
};

}  // namespace openport::server
