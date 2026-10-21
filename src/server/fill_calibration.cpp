#include "fill_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "openport/md/recording.hpp"

namespace openport::server {
namespace {
struct Recorded {
  md::Timestamp time = 0;
  trading::CalibrationQuote quote;
};
std::optional<trading::Money> price_of(double value) {
  if (!std::isfinite(value) || value <= 0) return std::nullopt;
  try { return trading::Money::from_double(value); } catch (const std::exception&) { return std::nullopt; }
}
trading::Quantity size_of(double value) {
  if (!std::isfinite(value) || value <= 0 || value >= 9e18) return 0;
  return static_cast<trading::Quantity>(std::floor(value));
}
}  // namespace

std::map<std::size_t, trading::CalibrationQuote> recorded_quotes(
    const std::filesystem::path& directory, const std::vector<trading::BrokerFill>& fills, md::Timestamp max_age) {
  std::map<std::size_t, trading::CalibrationQuote> found;
  std::map<std::string, std::vector<const trading::BrokerFill*>> wanted;
  std::set<md::Date> dates;
  md::Timestamp first = 0, last = 0;
  for (const auto& fill : fills) {
    if (fill.time <= 0) continue;
    wanted[fill.symbol].push_back(&fill);
    dates.insert(md::trading_date(fill.time));
    first = first == 0 ? fill.time : std::min(first, fill.time);
    last = std::max(last, fill.time);
  }
  std::error_code ec;
  if (wanted.empty() || directory.empty() || !std::filesystem::is_directory(directory, ec)) return found;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    const auto name = entry.path().filename().string();
    if (std::filesystem::is_regular_file(entry.symlink_status(ec)) && !name.starts_with('.') && entry.path().extension() == ".oprec")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  std::map<std::size_t, md::Timestamp> best;  // the found quote's time, by row
  for (const auto& file : files) {
    try {
      md::RecordingReader reader(file);
      const auto started = reader.header().started;
      if (started <= 0 || started > last ||
          (!dates.contains(md::trading_date(started)) && started < first - md::kNanosPerDay))
        continue;
      std::map<md::InstrumentId, std::string> ids;
      std::map<std::string, std::vector<Recorded>> quotes;
      while (const auto record = reader.next()) {
        if (const auto* definition = std::get_if<md::ContractDefinition>(&record->event)) {
          auto symbol = definition->contract.osi_symbol();
          if (wanted.contains(symbol)) ids[definition->id] = std::move(symbol);
          else ids.erase(definition->id);
        } else if (const auto* quote = std::get_if<md::OptionQuote>(&record->event)) {
          const auto id = ids.find(quote->id);
          if (id == ids.end() || quote->ts <= 0 || quote->ts > last) continue;
          const auto bid = price_of(quote->bid), ask = price_of(quote->ask);
          quotes[id->second].push_back({quote->ts, {bid.value_or(trading::Money{}), ask.value_or(trading::Money{}),
                                                    size_of(quote->bid_size), size_of(quote->ask_size), quote->ts, "recording"}});
        }
      }
      for (auto& [symbol, list] : quotes) {
        std::stable_sort(list.begin(), list.end(), [](const Recorded& a, const Recorded& b) { return a.time < b.time; });
        for (const auto* fill : wanted.at(symbol)) {
          const auto after = std::upper_bound(list.begin(), list.end(), fill->time,
                                              [](md::Timestamp time, const Recorded& r) { return time < r.time; });
          if (after == list.begin()) continue;
          const auto& at = *(after - 1);
          if (fill->time - at.time > max_age) continue;
          if (const auto previous = best.find(fill->row); previous != best.end() && previous->second >= at.time) continue;
          best[fill->row] = at.time;
          found[fill->row] = at.quote;
        }
      }
    } catch (const std::exception&) {
      continue;
    }
  }
  return found;
}
}  // namespace openport::server
