#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "openport/md/recording.hpp"
#include "openport/net/http.hpp"

namespace openport::providers {

struct ImportDay {
  std::string provider;
  md::Date date;
  md::Subscription subscription;
  std::filesystem::path output = "recordings";
};

struct HistoryWindow {
  md::Timestamp begin = 0;
  md::Timestamp end = 0;  ///< exclusive
};

/// Session windows, including holiday GTH assigned to this trading date. Only
/// completed business dates are accepted. `today` is injected for offline tests.
[[nodiscard]] std::vector<HistoryWindow> history_windows(const ImportDay& request, md::Date today);
[[nodiscard]] md::Date import_date(std::string_view text);
/// Inclusive dates/ranges, sorted and deduplicated; at most 3660 calendar days.
[[nodiscard]] std::vector<md::Date> import_dates(const std::vector<std::string>& selections);
struct ImportDayProgress {
  md::Date date;
  std::size_t day = 0, days = 0;
  std::string state;  ///< importing, imported, existing, closed
  std::size_t windows = 0, total_windows = 0;
  std::uint64_t events = 0;
};

/// One import per instance, with sequential, bounded delivery. Definitions may
/// arrive in prepare or read, but must precede dependent events. No connection or
/// receipt wall clocks are emitted.
class HistorySource {
 public:
  virtual ~HistorySource() = default;
  virtual void prepare(const ImportDay& request, HistoryWindow day, md::EventSink& sink) = 0;
  virtual void read(HistoryWindow window, md::EventSink& sink) = 0;
  /// Original capture time during publish, or zero when only event time is available.
  [[nodiscard]] virtual md::Timestamp receipt_time() const noexcept { return 0; }
};

using ImportProgress = std::function<void(std::size_t completed, std::size_t total,
                                          std::uint64_t events)>;
/// Creates a private temporary file, publishes it exclusively only on success.
[[nodiscard]] std::filesystem::path import_day(const ImportDay& request, HistorySource& source,
                                              md::Date today, ImportProgress progress = {});
using HistoryFactory = std::function<std::unique_ptr<HistorySource>()>;
/// Validate all dates before transport; resume only matching, complete imports.
/// A failed day leaves earlier completed days available for the next invocation.
void import_days(const ImportDay& request, const std::vector<md::Date>& dates, md::Date today,
                 HistoryFactory factory, std::function<void(const ImportDayProgress&)> progress = {});
/// Reads DATABENTO_API_KEY or MASSIVE_API_KEY; Theta Terminal owns its login.
[[nodiscard]] std::unique_ptr<HistorySource> make_history_source(const std::string& provider);

/// Injectable transport and wait for saved-response tests. Production serializes
/// requests, waits at least 100 ms between them and honours bounded 429 retries.
using HistoryWait = std::function<void(std::chrono::milliseconds)>;
[[nodiscard]] std::unique_ptr<HistorySource> theta_history(net::HttpClient& http,
                                                         HistoryWait wait);

[[nodiscard]] std::unique_ptr<HistorySource> massive_history(net::HttpClient& http, std::string key,
                                                           HistoryWait wait);
/// Quote/trade page size for `streams` streams: Massive's 50,000-row maximum,
/// lowered so one page per stream fits the one-million-record buffer (minimum 100).
[[nodiscard]] std::size_t massive_history_page_size(std::size_t streams) noexcept;

}  // namespace openport::providers
