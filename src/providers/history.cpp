#include "openport/providers/history.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unistd.h>

#include "openport/analytics/forward.hpp"
#include "openport/providers/snapshot.hpp"

namespace openport::providers {
#ifdef OPENPORT_WITH_DATABENTO
std::unique_ptr<HistorySource> databento_history(std::string key);
#endif
namespace {
constexpr std::size_t kMaxEvents = 1'000'000;
constexpr std::size_t kMaxContracts = 500'000;
md::Timestamp event_time(const md::Event& event) {
  return std::visit([](const auto& value) -> md::Timestamp {
    if constexpr (requires { value.ts; }) return value.ts;
    else return 0;
  }, event);
}
class WindowSink final : public md::EventSink {
 public:
  explicit WindowSink(const HistorySource& source) : source_(source) {}
  void publish(md::Event event) override {
    if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) {
      if (contracts.size() >= kMaxContracts && !contracts.contains(definition->id))
        throw std::runtime_error("import: more than 500000 contracts; reduce symbols");
      const auto [it, inserted] = contracts.emplace(definition->id, definition->contract);
      if (!inserted && it->second.osi_symbol() != definition->contract.osi_symbol())
        throw std::runtime_error("import: instrument definition changed identity");
      return;
    }
    if (std::holds_alternative<md::ProviderStatus>(event)) return;
    if (events.size() == kMaxEvents)
      throw std::runtime_error("import: window exceeds 1000000 events; reduce symbols");
    if (event_time(event) <= 0) throw std::runtime_error("import: missing market timestamp");
    const auto captured = source_.receipt_time();
    if (captured < 0) throw std::runtime_error("import: invalid capture timestamp");
    events.push_back({captured > 0 ? captured : event_time(event), std::move(event)});
  }
  std::map<md::InstrumentId, md::OptionContract> contracts;
  std::vector<md::RecordedEvent> events;
 private:
  const HistorySource& source_;
};
class Discard final : public md::EventSink {
 public:
  void publish(md::Event) override {}
};
struct Temporary {
  std::filesystem::path path;
  ~Temporary() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
}

md::Date import_date(std::string_view text) {
  if (text.size() != 10) throw std::invalid_argument("import: --date must be YYYY-MM-DD");
  const auto time = md::parse_datetime(std::string(text) + "T12:00:00", md::Zone::NewYork);
  if (!time) throw std::invalid_argument("import: invalid --date");
  return md::new_york_time(*time).date;
}

namespace {
void validate_request(const ImportDay& request, md::Date today) {
  if (request.provider != "databento" && request.provider != "thetadata" && request.provider != "massive")
    throw std::invalid_argument("import: provider must be databento, thetadata or massive");
  if (!md::valid_date(request.date) || request.date.year < 2022)
    throw std::invalid_argument("import: date must be valid and in 2022 or later");
  if (request.date >= today) throw std::invalid_argument("import: date must be a completed past day, not today or in the future");
  if (request.provider == "databento" && request.date < md::Date{2023, 3, 28})
    throw std::invalid_argument("import: Databento cbbo-1s requires 2023-03-28 or later");
  if (request.provider == "massive" && request.date < md::Date{2022, 3, 7})
    throw std::invalid_argument("import: Massive quotes require 2022-03-07 or later");
  if (request.subscription.underlyings.empty() || request.subscription.underlyings.size() > 32)
    throw std::invalid_argument("import: select 1..32 symbols");
  std::set<std::string> symbols;
  for (const auto& symbol : request.subscription.underlyings) {
    if (symbol.empty() || symbol.size() > 6 || symbol.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
      throw std::invalid_argument("import: invalid underlying symbol");
    if (!symbols.insert(symbol).second) throw std::invalid_argument("import: duplicate underlying symbol");
  }
  if (request.subscription.max_expiries < 0 || !std::isfinite(request.subscription.strike_window) ||
      request.subscription.strike_window < 0 || request.subscription.strike_window > 1)
    throw std::invalid_argument("import: invalid expiry or strike filter");
  if (request.output.empty()) throw std::invalid_argument("import: output directory is empty");
}
std::filesystem::path import_path(const ImportDay& request) {
  return request.output / (request.provider + "-" + md::format_date(request.date) + "-imported.oprec");
}
}

std::vector<HistoryWindow> history_windows(const ImportDay& request, md::Date today) {
  validate_request(request, today);
  const auto noon = md::new_york_to_utc(request.date, 12, 0);
  if (!md::stock_session(noon).open) throw std::invalid_argument("import: holiday or weekend has no trading day");
  const auto start = md::new_york_to_utc(md::previous_business_day(request.date), 17, 0);
  const auto end = md::new_york_to_utc(request.date, 17, 0);
  std::vector<HistoryWindow> windows;
  // Fifteen seconds bounds quote memory even for a whole OPRA chain. Scan closed
  // minutes too so weekend and holiday GTH use the calendar's actual assignment.
  for (auto time = start; time < end; time += md::kNanosPerMinute) {
    bool open = false;
    for (const auto& symbol : request.subscription.underlyings)
      open = open || md::trading_session(symbol, time).open;
    if (!open || md::trading_date(time) != request.date) continue;
    for (int part = 0; part < 4; ++part) {
      const auto begin = time + part * 15 * md::kNanosPerSecond;
      windows.push_back({begin, begin + 15 * md::kNanosPerSecond});
    }
  }
  if (windows.empty()) throw std::invalid_argument("import: no sessions for this day");
  return windows;
}

std::filesystem::path import_day(const ImportDay& request, HistorySource& source,
                                  md::Date today, ImportProgress progress) {
  const auto windows = history_windows(request, today);
  if (request.output.empty()) throw std::invalid_argument("import: output directory is empty");
  std::filesystem::create_directories(request.output);
  const auto result = import_path(request);
  if (std::filesystem::exists(result)) throw std::runtime_error("import: recording already exists");
  auto pattern = (request.output / ".import-XXXXXX").string();
  if (!::mkdtemp(pattern.data())) throw std::runtime_error("import: cannot create temporary directory");
  Temporary temporary{pattern};
  const auto partial = temporary.path / "day.oprec";
  md::RecordingHeader header;
  header.provider = request.provider;
  header.imported = true;
  header.trade_venues = source.trade_venues();
  header.subscription = request.subscription;
  header.started = windows.front().begin;
  header.capabilities.realtime = true;
  header.capabilities.quotes = true;
  header.capabilities.trades = true;
  header.capabilities.open_interest = request.provider != "massive";
  header.capabilities.history = true;
  std::atomic<md::Timestamp> receipt{header.started};
  Discard discard;
  md::RecordingSink recording(partial, header, discard, {.clock = [&] { return receipt.load(); }});
  WindowSink pending(source);
  source.prepare(request, {windows.front().begin, windows.back().end}, pending);
  if (!pending.events.empty()) throw std::runtime_error("import: prepare must emit only definitions");
  std::map<md::InstrumentId, md::InstrumentId> admitted;
  std::map<md::InstrumentId, md::OptionQuote> latest;
  std::map<std::string, md::UnderlyingQuote> spots;
  std::map<std::string, std::size_t> quote_counts;
  std::map<md::InstrumentId, double> volumes;
  std::map<md::InstrumentId, md::Timestamp> volume_times;
  std::map<md::InstrumentId, md::OpenInterest> deferred_interest;
  std::map<std::string, double> centers;
  md::Timestamp selected_at = 0;
  std::size_t completed = 0;
  for (const auto window : windows) {
    source.read(window, pending);
    receipt = std::max(receipt.load(), window.begin);
    std::stable_sort(pending.events.begin(), pending.events.end(), [](const auto& left, const auto& right) {
      return left.received < right.received;
    });
    std::map<std::string, std::set<md::Date>> expiries;
    for (const auto& [id, contract] : pending.contracts) {
      (void)id;
      expiries[contract.underlying].insert(contract.expiry);
    }
    // Equal capture-time observations form one selection boundary. Later prices in the
    // download window must not admit a contract at an earlier market time.
    for (std::size_t position = 0; position < pending.events.size();) {
      const auto selection_time = pending.events[position].received;
      auto end = position + 1;
      while (end < pending.events.size() && pending.events[end].received == selection_time) ++end;
      bool selection_changed = false;
      for (auto index = position; index < end; ++index) {
        const auto& event = pending.events[index].event;
        if (event_time(event) < window.begin || event_time(event) >= window.end) continue;
        if (const auto* quote = std::get_if<md::OptionQuote>(&event); quote && request.subscription.strike_window > 0) {
          latest[quote->id] = *quote;
          selection_changed = true;
        }
        if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
          spots[spot->symbol] = *spot;
          selection_changed = true;
        }
      }
      if (selection_changed || selection_time - selected_at >= md::kNanosPerSecond) {
        centers.clear();
        selected_at = selection_time;
        for (const auto& [symbol, spot] : spots) {
          if (selection_time - spot.ts <= 30 * md::kNanosPerMinute)
            centers[symbol] = spot.last > 0 ? spot.last : (spot.bid > 0 && spot.ask > 0 ? (spot.bid + spot.ask) / 2 : 0);
        }
        using Group = std::tuple<std::string, md::Date, std::string>;
        std::map<Group, std::map<double, std::pair<double, double>>> pairs;
        if (request.subscription.strike_window > 0) {
          for (const auto& [id, quote] : latest) {
            const auto& contract = pending.contracts.at(id);
            if (contract.expiry < request.date || quote.bid <= 0 || quote.ask < quote.bid || selection_time - quote.ts > md::kNanosPerMinute) continue;
            auto& pair = pairs[{contract.underlying, contract.expiry, contract.root}][contract.strike];
            (contract.type == pricing::OptionType::Call ? pair.first : pair.second) = (quote.bid + quote.ask) / 2;
          }
          for (const auto& [group, strikes] : pairs) {
            const auto& [symbol, expiry, root] = group;
            (void)root;
            if (centers[symbol] > 0) continue;
            std::vector<analytics::ParityPoint> points;
            for (const auto& [strike, pair] : strikes)
              if (pair.first > 0 && pair.second > 0) points.push_back({strike, pair.first, pair.second, 1});
            const auto forward = analytics::implied_forward(points, md::years_between(selection_time, md::new_york_to_utc(expiry, 16, 0)));
            if (forward.ok) centers[symbol] = forward.forward;
          }
        }
      }
      for (auto index = position; index < end; ++index) {
        auto& event = pending.events[index].event;
        const auto time = event_time(event);
        const auto captured = pending.events[index].received;
        // A daily OI observation may precede the first quote window, but never
        // introduce future information. Other endpoints use half-open windows.
        const bool interest = std::holds_alternative<md::OpenInterest>(event);
        if (captured >= window.end || (!interest && captured < window.begin)) continue;
        if (const auto* oi = std::get_if<md::OpenInterest>(&event); oi && !admitted.contains(oi->id)) {
          auto& previous = deferred_interest[oi->id];
          if (oi->ts >= previous.ts) previous = *oi;
          continue;
        }
        double volume = 0;
        md::Timestamp volume_time = time;
        if (const auto* trade = std::get_if<md::OptionTrade>(&event)) {
          const auto& contract = pending.contracts.at(trade->id);
          if (md::trading_session(contract.root, time).open && md::trading_date(time) == request.date) {
            volume = (volumes[trade->id] += trade->size);
            volume_time = (volume_times[trade->id] = std::max(volume_times[trade->id], time));
          }
        }
        std::string symbol;
        bool keep = true;
        std::visit([&](auto& value) {
          if constexpr (requires { value.id; }) {
            const auto found = pending.contracts.find(value.id);
            if (found == pending.contracts.end()) throw std::runtime_error("import: undefined instrument");
            const auto& contract = found->second;
            symbol = contract.underlying;
            if (std::find(request.subscription.underlyings.begin(), request.subscription.underlyings.end(), symbol) == request.subscription.underlyings.end()) { keep = false; return; }
            if (!interest && (!md::trading_session(contract.root, time).open || md::trading_date(time) != request.date || time >= contract.last_trade_time())) { keep = false; return; }
            if (!admitted.contains(value.id)) {
              if (request.subscription.strike_window > 0 && centers[symbol] <= 0) { keep = false; return; }
              const ChainFilter filter(request.subscription, request.date, centers[symbol], expiries[symbol]);
              if (!filter.admits(contract)) { keep = false; return; }
              const auto id = static_cast<md::InstrumentId>(admitted.size());
              admitted.emplace(value.id, id);
              receipt = std::max({receipt.load(), time, captured});
              recording.publish(md::ContractDefinition{id, contract});
              if (const auto previous = deferred_interest.find(value.id); previous != deferred_interest.end()) {
                recording.publish(md::OpenInterest{id, previous->second.ts, previous->second.contracts});
                deferred_interest.erase(previous);
              }
            }
            value.id = admitted.at(value.id);
          } else if constexpr (requires { value.symbol; }) {
            symbol = value.symbol;
            keep = std::find(request.subscription.underlyings.begin(), request.subscription.underlyings.end(), symbol) != request.subscription.underlyings.end();
          }
        }, event);
        if (!keep) continue;
        receipt = std::max({receipt.load(), time, captured});
        recording.publish(event);
        if (std::holds_alternative<md::OptionQuote>(event) && quote_counts[symbol]++ == 0)
          recording.publish(md::ProviderStatus{time, md::FeedState::Live, "Imported historical quotes", symbol});
        if (const auto* trade = std::get_if<md::OptionTrade>(&event)) {
          recording.publish(md::OptionVolume{trade->id, volume_time, volume});
        }
      }
      position = end;
    }
    pending.events.clear();
    if (!recording.error().empty()) throw std::runtime_error(recording.error());
    ++completed;
    if (progress) progress(completed, windows.size(), recording.stats().events);
  }
  for (const auto& symbol : request.subscription.underlyings)
    if (quote_counts[symbol] == 0) throw std::runtime_error("import: no option quote data for " + symbol + " on this day with these filters");
  recording.close();
  if (!recording.error().empty()) throw std::runtime_error(recording.error());
  // link is atomic and refuses an existing destination, including a symlink.
  std::filesystem::create_hard_link(partial, result);
  return result;
}

std::vector<md::Date> import_dates(const std::vector<std::string>& selections) {
  std::set<md::Date> dates;
  for (const auto& selection : selections) {
    const auto separator = selection.find("..");
    const auto first = import_date(selection.substr(0, separator));
    const auto last = separator == std::string::npos ? first : import_date(selection.substr(separator + 2));
    const auto begin = md::days_since_epoch(first), end = md::days_since_epoch(last);
    if (end < begin || end - begin >= 3660) throw std::invalid_argument("import: range must be ascending and at most 3660 days");
    for (auto day = begin; day <= end; ++day) dates.insert(md::date_from_days(day));
    if (dates.size() > 3660) throw std::invalid_argument("import: select at most 3660 days");
  }
  if (dates.empty()) throw std::invalid_argument("--import-day requires --date or --import-days YYYY-MM-DD[..YYYY-MM-DD]");
  return {dates.begin(), dates.end()};
}

void import_days(const ImportDay& request, const std::vector<md::Date>& dates, md::Date today,
                 HistoryFactory factory, std::function<void(const ImportDayProgress&)> progress) {
  const std::set<md::Date> selected(dates.begin(), dates.end());
  if (selected.empty() || selected.size() > 3660) throw std::invalid_argument("import: select 1..3660 dates");
  auto day = request;
  for (const auto date : selected) { day.date = date; validate_request(day, today); }
  std::size_t completed = 0;
  for (const auto date : selected) {
    day.date = date;
    ImportDayProgress update{date, ++completed, selected.size(), "importing"};
    const auto report = [&] { if (progress) progress(update); };
    if (!md::stock_session(md::new_york_to_utc(date, 12, 0)).open) {
      update.state = "closed";
      report();
      continue;
    }
    const auto path = import_path(day);
    if (std::filesystem::exists(std::filesystem::symlink_status(path))) {
      if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))
        throw std::runtime_error("import: existing destination is not a regular file");
      md::RecordingReader reader(path);
      const auto& header = reader.header();
      auto symbols = header.subscription.underlyings, wanted = day.subscription.underlyings;
      std::sort(symbols.begin(), symbols.end());
      std::sort(wanted.begin(), wanted.end());
      if (!header.imported || header.provider != day.provider || md::trading_date(header.started) != date ||
          symbols != wanted || header.subscription.max_expiries != day.subscription.max_expiries ||
          header.subscription.strike_window != day.subscription.strike_window)
        throw std::runtime_error("import: existing recording has different date, source or filters; choose another output directory");
      while (reader.next()) {}
      if (!reader.diagnostic().empty()) throw std::runtime_error("import: existing recording is incomplete; move it aside before retrying");
      update.state = "existing";
      report();
      continue;
    }
    report();
    auto source = factory();
    (void)import_day(day, *source, today, [&](auto windows, auto total, auto events) {
      update.windows = windows; update.total_windows = total; update.events = events;
      report();
    });
    update.state = "imported";
    report();
  }
}

std::unique_ptr<HistorySource> make_history_source(const std::string& provider) {
  if (provider == "databento") {
    const char* key = std::getenv("DATABENTO_API_KEY");
    if (!key || !*key) throw std::invalid_argument("import: set DATABENTO_API_KEY");
#ifdef OPENPORT_WITH_DATABENTO
    return databento_history(key);
#else
    throw std::invalid_argument("import: Databento support is not built");
#endif
  }
  if (provider == "thetadata" || provider == "massive") {
    // The wrapper owns the HTTP client for the lifetime of its injectable source.
    class OwnedHistory final : public HistorySource {
     public:
      explicit OwnedHistory(const std::string& name) {
        const auto wait = [](auto delay) { std::this_thread::sleep_for(delay); };
        const auto* key = std::getenv("MASSIVE_API_KEY");
        source = name == "massive" ? massive_history(http, key ? key : "", wait) : theta_history(http, wait);
      }
      md::Timestamp receipt_time() const noexcept override { return source->receipt_time(); }
      void prepare(const ImportDay& request, HistoryWindow day, md::EventSink& sink) override { source->prepare(request, day, sink); }
      void read(HistoryWindow window, md::EventSink& sink) override { source->read(window, sink); }
     private:
      net::HttpClient http;
      std::unique_ptr<HistorySource> source;
    };
    return std::make_unique<OwnedHistory>(provider);
  }
  throw std::invalid_argument("import: provider must be databento, thetadata or massive");
}
}  // namespace openport::providers
