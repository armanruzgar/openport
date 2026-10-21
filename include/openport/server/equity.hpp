#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "openport/trading/session.hpp"

namespace openport::server {
struct EquityRisk {
  trading::Exposure exposure;
  double delta_utilisation = 0, vega_utilisation = 0;
  std::optional<double> gamma_utilisation = {}, theta_utilisation = {}, grid_utilisation = {}, short_vega_utilisation = {};
};
struct EquitySample {
  md::Timestamp time = 0;
  std::uint64_t attempt = 0;
  trading::Money equity;
  std::optional<trading::Money> floor;
  trading::Money peak;
  std::optional<trading::Money> target;
  std::optional<trading::Money> tomorrow_floor;
  std::uint64_t fill = 0;  ///< Zero for a minute sample; otherwise the fill ID.
  std::uint64_t stock_fill = 0;  ///< A share change, at its committed transaction mark.
  bool decision = false;  ///< Append-time retention hint; persisted as an ordinary mark.
  std::optional<EquityRisk> risk = {};  ///< Complete contemporaneous valuations only; old rows and interim fill marks have none.
};
/// Exact marked-equity changes from committed fills, led by the mark before the first
/// option execution (fill zero), which can be a new high or the equity that decided the
/// attempt. Atomic combo legs share one mark. Each sample carries the peak, floor and
/// tomorrow's floor the reducer held at that point. The engine calls this after each
/// reducer transaction, including decisions without fills.
[[nodiscard]] std::vector<EquitySample> fill_equity_samples(const trading::TradingSession& session,
    const trading::TradingSnapshot& before);
/// Reads a history file as it is, for an archive: nothing is compacted or rewritten.
/// Invalid rows are skipped and reported in `error`.
[[nodiscard]] std::vector<EquitySample> read_equity_history(const std::filesystem::path& file, std::string& error);
/// Engine-thread owned. Append-only CSV beside the journal, monetary values in
/// micro-dollars, timestamps in nanoseconds. Keeps one plain mark per minute, every
/// fill, decision, floor changes and the first mark at or past the target or floor; the current
/// attempt and at most 90 days / 100,000 samples of earlier attempts. Storage failures never throw.
class EquityStore {
 public:
  explicit EquityStore(std::filesystem::path file, bool read_only = false);
  void append(const EquitySample& sample) noexcept;
  [[nodiscard]] const std::vector<EquitySample>& samples() const { return samples_; }
  [[nodiscard]] const std::string& error() const { return error_; }
  [[nodiscard]] md::Timestamp error_time() const { return error_time_; }
  [[nodiscard]] md::Timestamp error_market_time() const { return error_market_time_; }
  [[nodiscard]] bool error_recovered() const { return error_recovered_; }
 private:
  void compact();
  void fail(std::string message, md::Timestamp market_time = 0);
  std::filesystem::path file_;
  std::vector<EquitySample> samples_;
  std::size_t lines_ = 0;
  std::string error_;
  md::Timestamp error_time_ = 0, error_market_time_ = 0;
  bool error_recovered_ = false;
  bool read_only_ = false;
};
}  // namespace openport::server
