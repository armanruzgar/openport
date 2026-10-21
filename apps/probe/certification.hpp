#pragma once

#include <array>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>

#include "state.hpp"

namespace openport::probe {

/// Observes publication before queue coalescing. Reports contain structured
/// measurements only: no configuration, raw payloads or provider diagnostics.
class Conformance final : public md::EventSink {
 public:
  Conformance(const md::Subscription& subscription, bool streaming, std::size_t minimum_quotes);
  void publish(md::Event event) override;
  void observe(const md::Event& event, md::Timestamp received);
  void wait(std::chrono::steady_clock::time_point deadline, bool snapshot);
  [[nodiscard]] nlohmann::json report(bool startup_failed = false) const;
  [[nodiscard]] static std::string markdown(const nlohmann::json& report);

 private:
  struct ClockCheck {
    std::size_t known = 0, unknown = 0, future = 0, regressions = 0, receipt_matches = 0;
    md::Timestamp first = 0, last = 0;
  };
  struct FieldCheck {
    std::size_t present = 0, missing = 0, invalid = 0, scale_samples = 0, scale_suspect = 0;
    double minimum = 0, maximum = 0;
    void sample(double value);
    void compare(double value, double reference);
  };
  struct Contract {
    md::OptionContract definition;
    std::size_t events = 0;
    bool volume = false, open_interest = false;
  };
  void clock(std::size_t type, const std::string& key, md::Timestamp ts, md::Timestamp received);
  void greeks(const md::VendorGreeks& value);

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Readiness readiness_;
  std::vector<std::string> underlyings_;
  std::map<md::InstrumentId, Contract> contracts_;
  std::map<std::string, md::UnderlyingQuote> spots_;
  std::map<std::pair<std::size_t, std::string>, md::Timestamp> previous_;
  std::map<std::size_t, ClockCheck> clocks_;
  std::map<std::string, FieldCheck> fields_;
  std::map<std::string, std::size_t> states_, error_kinds_;
  std::size_t degraded_ = 0;
  std::array<std::size_t, std::variant_size_v<md::Event>> counts_{};
  std::size_t undefined_ = 0, bad_counts_ = 0, errors_ = 0;
  bool ended_ = false;
};

/// Runs a real adapter, or an explicitly labelled demo/replay. Creates a new
/// directory containing conformance.json and conformance.md, including failures.
int certify(const md::ProviderConfig& config, const md::Subscription& subscription,
            int seconds, bool duration, bool snapshot, std::size_t minimum_quotes,
            const std::filesystem::path& directory);

}  // namespace openport::probe
