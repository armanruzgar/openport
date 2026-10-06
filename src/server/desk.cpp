#include "openport/server/desk.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/playbooks.hpp"
#include "openport/server/plans.hpp"

#include "openport/pricing/black.hpp"
#include "openport/trading/format.hpp"
#include "openport/server/run.hpp"
#include "metric_cache.hpp"
#include "run_json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <nlohmann/json.hpp>
#include <fcntl.h>
#include <unistd.h>

namespace openport::server {
namespace {
using namespace trading;

// Hypothetical fills used by chained-entry gates and sequential what-if previews.
Decision project_contracts(const OrderRequest& order, std::map<std::string, Quantity>& changes) {
  if (order.exits_only) return {};
  auto legs = order.legs;
  if (legs.empty()) legs.push_back({order.symbol, order.side, 1});
  for (const auto& leg : legs) {
    if (order.quantity <= 0 || leg.ratio < 1 || leg.ratio > kMaxRatio || (leg.side != Side::Buy && leg.side != Side::Sell))
      return {Reason::INVALID_ORDER, "Invalid projected contract count or side", {}, {}, leg.symbol};
    Quantity quantity = 0;
    if (__builtin_mul_overflow(order.quantity, leg.ratio, &quantity))
      return {Reason::ARITHMETIC_OVERFLOW, "Projected contract count overflow", {}, {}, leg.symbol};
    if (leg.side == Side::Sell) quantity = -quantity;
    if (__builtin_add_overflow(changes[leg.symbol], quantity, &changes[leg.symbol]))
      return {Reason::ARITHMETIC_OVERFLOW, "Projected holdings overflow", {}, {}, leg.symbol};
  }
  return {};
}

void sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) throw TradingError(Reason::JOURNAL_IO, "Cannot open journal directory for durability");
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) throw TradingError(Reason::JOURNAL_IO, "Cannot sync journal directory");
}

void save_account_file(const std::filesystem::path& path, const std::string& text) {
  check_storage_space(path, text.size());
  const auto temporary = path.string() + ".tmp";
  const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) throw TradingError(Reason::JOURNAL_IO, "Cannot write account metadata");
  const bool written = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
  const bool synced = written && ::fsync(fd) == 0;
  ::close(fd);
  if (!synced) throw TradingError(Reason::JOURNAL_IO, "Cannot sync account metadata");
  std::filesystem::rename(temporary, path);
  sync_directory(path.parent_path());
}

// A durable destination directory is the deletion tombstone. Startup finishes
// any moves interrupted by a crash; its existence permanently reserves the ID.
void retain_deleted_files(const std::filesystem::path& directory, const std::string& id) {
  const auto destination = directory / "deleted" / id;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (entry.is_regular_file() && name.starts_with(id + "."))
      std::filesystem::rename(entry.path(), destination / entry.path().filename());
  }
  sync_directory(destination);
  sync_directory(directory);
}

/// How close to the close the last print before it must be to settle on instead of
/// a closing print that has not come within kLastPrintWait.
constexpr md::Timestamp kLastPrintAge = 5 * md::kNanosPerMinute;
md::Date new_york_date(md::Timestamp time) {
  auto date = md::date_from_days(time / md::kNanosPerDay);
  // New York is behind UTC; compare its actual midnight, including DST dates.
  if (time < md::new_york_to_utc(date, 0, 0))
    date = md::date_from_days(md::days_since_epoch(date) - 1);
  return date;
}
Quantity whole_size(double size) {
  if (!std::isfinite(size) || size <= 0 || size >= 9223372036854775808.0) return 0;
  return static_cast<Quantity>(std::floor(size));
}
std::optional<Money> quote_price(double price) {
  if (!std::isfinite(price) || price <= 0) return {};
  try { return Money::from_double(price); }
  catch (const TradingError&) { return {}; }
}
Valuation valuation_for(const std::string& symbol, const md::OptionContract& contract,
                         const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  Valuation result;
  result.symbol = symbol;
  result.valid = false;
  if (!metrics) return result;
  result.time = metrics->as_of;
  for (const auto& slice : metrics->slices) {
    if (slice.expiry_time != contract.expiry_time() || slice.style != contract.style) continue;
    for (const auto& strike : slice.strikes) {
      const auto& option = contract.type == pricing::OptionType::Call ? strike.call : strike.put;
      if (option.id == analytics::kNoInstrument || option.contract.osi_symbol() != symbol) continue;
      result = {symbol, metrics->as_of, option.delta, option.gamma, option.vega, option.theta,
                metrics->spot, slice.forward.forward, slice.forward.discount, slice.years,
                strike.iv, true};
      result.valid = valid_valuation(result);
      // A far wing without a bid can leave its strike with no smile IV. Its own ask's
      // (or bid's) IV, at the expiry's forward, still measures holding it.
      const double vol = std::isfinite(option.ask_iv) ? option.ask_iv : option.bid_iv;
      const double spot = metrics->spot, forward = slice.forward.forward;
      if (!result.valid && vol > 0 && std::isfinite(vol) && spot > 0 && forward > 0 && slice.years > 0) {
        const double carry = forward / spot;
        const auto g = pricing::black_greeks(contract.type, forward, contract.strike, slice.years, vol,
                                             slice.forward.discount);
        result = {symbol, metrics->as_of, g.delta * carry, g.gamma * carry * carry, g.vega / 100.0,
                  g.theta / 365.0, spot, forward, slice.forward.discount, slice.years, vol, true};
        result.valid = valid_valuation(result);
      }
      result.spot_source = metrics->spot_source;
      return result;
    }
  }
  return result;
}
/// Opens a journal for writing, making each directory created on the way durable:
/// an existing file is locked, then read for recovery; otherwise a new one is created.
/// An empty journal, as a create that failed before its first record leaves, holds
/// no transaction, so it opens as a new one too.
std::pair<std::shared_ptr<Journal>, std::optional<JournalRecovery>> open_journal(
    const std::filesystem::path& file, const FileJournal::Options& options) {
  const auto parent = std::filesystem::absolute(file).parent_path();
  auto existing = parent;
  while (!std::filesystem::exists(existing)) existing = existing.parent_path();
  std::filesystem::create_directories(parent);
  for (auto path = parent; path != existing; path = path.parent_path()) sync_directory(path);
  sync_directory(existing);
  if (std::filesystem::exists(file)) {
    // Lock before reading, then recover the same verified head held by the writer.
    std::shared_ptr<Journal> journal = FileJournal::resume(file.string(), options);
    auto recovery = FileJournal::read(file.string());
    if (recovery.records.empty()) return {journal, std::nullopt};
    return {journal, std::move(recovery)};
  }
  std::shared_ptr<Journal> journal = FileJournal::create(file.string(), options);
  sync_directory(parent);
  return {journal, std::nullopt};
}
FileJournal::Options journal_options(const Desk::Options& desk) {
  FileJournal::Options options;
  // Live callers cannot inherit a configurable batching policy from another desk.
  if (desk.replay) options.sync_policy = FileJournal::SyncPolicy::Batched;
  options.hooks = desk.journal_io;
  return options;
}

/// Account IDs name journal files: lowercase letters, digits and single hyphens.
bool account_id(std::string_view id) {
  return !id.empty() && id.size() <= 40 && id.front() != '-' && id.back() != '-' &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }) &&
         id.find("--") == std::string_view::npos;
}
/// Named accounts list in ID order: "swing-50k" before "swing-50k-2", although the
/// latter's file name sorts first ('-' before '.').
bool by_account_id(const std::filesystem::path& a, const std::filesystem::path& b) {
  return a.stem().string() < b.stem().string();
}
}  // namespace

bool valid_account_name(std::string_view name) {
  std::size_t characters = 0;
  for (std::size_t i = 0; i < name.size(); ++characters) {
    const auto lead = static_cast<unsigned char>(name[i]);
    const std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
    if (!length || i + length > name.size()) return false;
    std::uint32_t code = length == 1 ? lead : lead & (0x7Fu >> length);
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(name[i + k]);
      if ((next & 0xC0) != 0x80) return false;
      code = code << 6 | (next & 0x3Fu);
    }
    // Overlong forms, surrogates and values past U+10FFFF are not UTF-8.
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && (code < 0x10000 || code > 0x10FFFF)) ||
        (code >= 0xD800 && code <= 0xDFFF) || code < 0x20 || (code >= 0x7F && code <= 0x9F))
      return false;
    i += length;
  }
  return characters >= 1 && characters <= 64;
}

namespace {
std::string slug(std::string_view name) {
  std::string id;
  for (const char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) id += c;
    else if (c >= 'A' && c <= 'Z') id += static_cast<char>(c - 'A' + 'a');
    else if (!id.empty() && id.back() != '-') id += '-';
    if (id.size() >= 32) break;
  }
  while (!id.empty() && id.back() == '-') id.pop_back();
  return id;
}
/// Regular-session time between two instants, over each trading day's 09:30 open to
/// its close (13:00 on early closes): business time, not calendar time.
md::Timestamp session_time(md::Timestamp from, md::Timestamp to) {
  md::Timestamp total = 0;
  if (to <= from) return total;
  auto day = md::days_since_epoch(md::new_york_time(from).date);
  const auto last = md::days_since_epoch(md::new_york_time(to).date);
  for (; day <= last; ++day) {
    const auto date = md::date_from_days(day);
    const auto open = md::new_york_to_utc(date, 9, 30);
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    if (!md::market_session(open).open) continue;
    total += std::max<md::Timestamp>(0, std::min(close, to) - std::max(open, from));
  }
  return total;
}
double slice_atm_iv(const analytics::SliceMetrics& slice, double spot) {
  if (slice.atm_iv > 0 && std::isfinite(slice.atm_iv)) return slice.atm_iv;
  double iv = analytics::kNaN, distance = std::numeric_limits<double>::max();
  for (const auto& strike : slice.strikes) {
    if (!(strike.iv > 0) || !std::isfinite(strike.iv)) continue;
    const auto away = std::abs(strike.strike - spot);
    if (away < distance) { iv = strike.iv; distance = away; }
  }
  return iv;
}
std::optional<double> close_variance(const std::shared_ptr<const analytics::UnderlyingMetrics>& metrics) {
  return metrics ? implied_variance_to_close(*metrics) : std::nullopt;
}
}  // namespace

/// The nearest expiry's at-the-money IV was solved on calendar time, so IV² × its
/// years is the variance to its settlement; today's session takes its share of that in
/// business time. Calendar time alone would spread a week's variance over nights and
/// weekends and understate a few trading hours several times over.
std::optional<double> implied_variance_to_close(const analytics::UnderlyingMetrics& metrics) {
  if (!(metrics.spot > 0)) return {};
  const analytics::SliceMetrics* front = nullptr;
  double distance = std::numeric_limits<double>::max();
  for (const auto& slice : metrics.slices) {
    if (!(slice.years > 0) || slice.expiry_time <= metrics.as_of || (front && slice.years > front->years)) continue;
    for (const auto& strike : slice.strikes) {
      if (!(strike.iv > 0) || !std::isfinite(strike.iv)) continue;
      const auto away = std::abs(strike.strike - metrics.spot);
      if (!front || slice.years < front->years || away < distance) { front = &slice; distance = away; }
    }
  }
  if (!front) return {};
  const double iv = slice_atm_iv(*front, metrics.spot);
  const auto date = md::trading_date(metrics.as_of);
  const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
  const auto today = session_time(metrics.as_of, close);
  const auto until_expiry = session_time(metrics.as_of, front->expiry_time);
  if (today <= 0) return 0.0;
  if (until_expiry <= 0) return {};
  return iv * iv * front->years * std::min(1.0, static_cast<double>(today) / static_cast<double>(until_expiry));
}

md::Timestamp probability_horizon(md::Timestamp now, double days) {
  if (days > 0) return now + std::llround(days * static_cast<double>(md::kNanosPerDay));
  const auto date = md::trading_date(now);
  return md::new_york_to_utc(date, md::regular_close_hour(date), 0);
}

/// Past today's close, each expiry's at-the-money total variance (IV² × years, the
/// IV's own calendar time) is interpolated linearly in time, with flat volatility
/// before the first expiry and after the last.
std::optional<double> implied_variance_until(const analytics::UnderlyingMetrics& metrics, md::Timestamp target) {
  if (!(metrics.spot > 0)) return {};
  if (target <= metrics.as_of) return 0.0;
  const auto to_close = implied_variance_to_close(metrics);
  const auto date = md::trading_date(metrics.as_of);
  const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
  if (target <= close) {
    if (!to_close) return {};
    const auto today = session_time(metrics.as_of, close);
    return today <= 0 ? 0.0 : *to_close * static_cast<double>(session_time(metrics.as_of, target)) / static_cast<double>(today);
  }
  std::vector<std::pair<double, double>> term;  // years, total variance
  for (const auto& slice : metrics.slices) {
    if (!(slice.years > 0) || slice.expiry_time <= metrics.as_of) continue;
    const double iv = slice_atm_iv(slice, metrics.spot);
    if (iv > 0 && std::isfinite(iv)) term.emplace_back(slice.years, iv * iv * slice.years);
  }
  if (term.empty()) return {};
  std::sort(term.begin(), term.end());
  const double years = md::years_between(metrics.as_of, target);
  double variance = 0;
  if (years <= term.front().first) variance = term.front().second / term.front().first * years;
  else if (years >= term.back().first) variance = term.back().second / term.back().first * years;
  else {
    const auto upper = std::lower_bound(term.begin(), term.end(), std::pair{years, -1.0});
    const auto lower = std::prev(upper);
    variance = lower->second + (upper->second - lower->second) * (years - lower->first) / (upper->first - lower->first);
  }
  return std::max(variance, to_close.value_or(0.0));
}

PriceOdds price_odds(double spot, double price, double variance) {
  PriceOdds odds{price, 0, 0, 0};
  if (!(spot > 0) || !(price > 0) || !(variance >= 0)) return odds;
  const double distance = std::log(spot / price);
  if (variance == 0) {
    odds.above = distance > 0 ? 1 : 0;
    odds.below = 1 - odds.above;
    odds.touch = distance == 0 ? 1 : 0;
    return odds;
  }
  const double sd = std::sqrt(variance);
  odds.above = std::clamp(0.5 * std::erfc(-(distance - variance / 2) / (sd * std::sqrt(2.0))), 0.0, 1.0);
  odds.below = 1 - odds.above;
  odds.touch = std::clamp(std::erfc(std::abs(distance) / (sd * std::sqrt(2.0))), 0.0, 1.0);
  return odds;
}

BetaEstimate estimate_beta(const std::vector<md::Bar>& asset, const std::vector<md::Bar>& benchmark,
                           std::size_t max_returns, md::Timestamp max_gap) {
  std::map<md::Timestamp, double> closes;
  for (const auto& bar : benchmark) if (bar.close > 0 && std::isfinite(bar.close)) closes[bar.start] = bar.close;
  std::vector<std::pair<double, double>> pairs;
  for (std::size_t i = 1; i < asset.size(); ++i) {
    const auto& before = asset[i - 1];
    const auto& after = asset[i];
    if (!(before.close > 0) || !(after.close > 0) || !std::isfinite(before.close) || !std::isfinite(after.close) ||
        after.start <= before.start || (max_gap > 0 && after.start - before.start > max_gap)) continue;
    const auto from = closes.find(before.start);
    const auto to = closes.find(after.start);
    if (from == closes.end() || to == closes.end() || std::next(from) != to) continue;
    pairs.emplace_back(std::log(after.close / before.close), std::log(to->second / from->second));
  }
  if (pairs.size() > max_returns) pairs.erase(pairs.begin(), pairs.end() - static_cast<std::ptrdiff_t>(max_returns));
  BetaEstimate estimate;
  estimate.observations = pairs.size();
  if (pairs.size() < 2) return estimate;
  double mean_a = 0, mean_b = 0;
  for (const auto& [a, b] : pairs) { mean_a += a; mean_b += b; }
  mean_a /= static_cast<double>(pairs.size());
  mean_b /= static_cast<double>(pairs.size());
  double covariance = 0, variance_a = 0, variance_b = 0;
  for (const auto& [a, b] : pairs) {
    covariance += (a - mean_a) * (b - mean_b);
    variance_a += (a - mean_a) * (a - mean_a);
    variance_b += (b - mean_b) * (b - mean_b);
  }
  if (!(variance_b > 0)) return estimate;
  estimate.beta = covariance / variance_b;
  if (variance_a > 0) estimate.correlation = covariance / std::sqrt(variance_a * variance_b);
  if (!std::isfinite(estimate.beta)) estimate.beta = 0;
  else estimate.source = "estimated";
  return estimate;
}

bool same_index(std::string_view a, std::string_view b) {
  const auto family = [](std::string_view symbol) {
    if (symbol == "SPX" || symbol == "SPXW" || symbol == "XSP" || symbol == "SPY") return 1;
    if (symbol == "NDX" || symbol == "NDXP" || symbol == "XND" || symbol == "QQQ") return 2;
    if (symbol == "RUT" || symbol == "RUTW" || symbol == "IWM") return 3;
    return 0;
  };
  return a == b || (family(a) != 0 && family(a) == family(b));
}

std::optional<MarketHalt> circuit_breaker(double reference, double price, md::Timestamp time, int tripped) {
  if (!(reference > 0) || !(price > 0) || !std::isfinite(reference) || !std::isfinite(price)) return std::nullopt;
  if (!md::market_session(time).open) return std::nullopt;  // the regular session only
  const auto date = md::new_york_time(time).date;
  const double fall = 1 - price / reference;
  if (fall >= 0.20) {
    if (tripped >= 3) return std::nullopt;
    return MarketHalt{3, time, md::new_york_to_utc(date, 17, 0), reference, price};
  }
  const int level = fall >= 0.13 ? 2 : fall >= 0.07 ? 1 : 0;
  const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
  if (level <= tripped || time >= close - 35 * md::kNanosPerMinute) return std::nullopt;
  return MarketHalt{level, time, time + 15 * md::kNanosPerMinute, reference, price};
}

trading::Decision paper_acceptance(std::string_view underlying, md::Timestamp market_time,
    md::Timestamp wall_time, std::chrono::seconds delay, md::Timestamp max_quote_age,
    const std::vector<MarketHalt>& halts) {
  // Authored halts use the driver clock: their frozen quotes must not hide either
  // the halt or its resume behind the feed-stalled gate.
  const auto delay_seconds = std::max<std::int64_t>(0, delay.count());
  const auto shown = delay_seconds >= wall_time / md::kNanosPerSecond ? 0 : wall_time - delay_seconds * md::kNanosPerSecond;
  bool authored = false;
  md::Timestamp resume = 0;
  for (const auto& halt : halts) {
    if (shown < halt.start || shown >= halt.end) continue;
    authored = authored || halt.level == 0;
    resume = std::max(resume, halt.end);
  }
  if (authored) {
    std::string message = "Trading is halted market-wide by the scenario";
    const auto date = md::new_york_time(shown).date;
    const auto close = md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    if (resume >= close) message += " for the rest of the day";
    else {
      const auto minutes = md::new_york_time(resume).seconds / 60;
      char text[64];
      std::snprintf(text, sizeof text, "; it resumes at %02d:%02d ET", minutes / 60, minutes % 60);
      message += text;
    }
    return {Reason::MARKET_HALTED, std::move(message), {}, {}, {}};
  }
  if (market_time <= 0)
    return {Reason::INVALID_QUOTE, std::string(underlying) + " is waiting for market data", {}, {}, {}};
  // A healthy feed shows the market as it was `delay` ago, and stops at the end
  // of a session. Data more than max_quote_age behind that is a stalled feed;
  // a feed that rightly shows a closed market (as in a session's first minutes
  // on a delayed feed) is not.
  const auto session = md::trading_session(underlying, market_time);
  auto expected = market_time;
  if (md::trading_session(underlying, shown).open) expected = shown;
  else if (session.open && session.end <= shown) expected = session.end;
  // A delayed feed's snapshots can trail its stated delay by a minute or two (Cboe
  // refreshes its quote pages about once a minute), so it stalls only past three.
  const auto tolerance = delay_seconds > 0 ? std::max<md::Timestamp>(max_quote_age, 3 * md::kNanosPerMinute) : max_quote_age;
  if (expected > market_time && expected - market_time > tolerance) {
    const auto lag = wall_time - market_time;
    const auto minutes = lag / md::kNanosPerMinute;
    const auto duration = minutes >= 60
        ? std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m"
        : minutes > 0 ? std::to_string(minutes) + "m"
                      : std::to_string(lag / md::kNanosPerSecond) + "s";
    return {Reason::FEED_STALLED, std::string(underlying) + " quotes are " + duration +
        " behind the market; the feed appears to have stalled", {}, {}, {}};
  }
  for (const auto& halt : halts) {
    if (halt.level == 0 || market_time < halt.start || market_time >= halt.end) continue;
    const auto resumes = md::new_york_time(halt.end).seconds / 60;
    char text[240];
    std::snprintf(text, sizeof text,
                  "Trading is halted market-wide: the S&P 500 fell %.1f%% from its previous close of %.2f "
                  "(a level %d circuit breaker)%s",
                  100 * (1 - halt.price / halt.reference), halt.reference, halt.level,
                  halt.level == 3 ? " for the rest of the day" : "");
    std::string message = text;
    if (halt.level < 3) {
      std::snprintf(text, sizeof text, "; it resumes at %02d:%02d ET", resumes / 60, resumes % 60);
      message += text;
    }
    return {Reason::MARKET_HALTED, std::move(message), {}, {}, {}};
  }
  if (!session.open) {
    // Say why the market is closed now (a weekend, say), not at the feed's last close.
    const auto now = md::trading_session(underlying, shown);
    return {Reason::SESSION_CLOSED, std::string(underlying) + " options are " + (now.open ? session.note : now.note), {}, {}, {}};
  }
  return {};
}

namespace {
/// Rewrite one journal (see compact_paper_journals).
JournalCompaction compact_journal(const std::filesystem::path& file) {
  JournalCompaction result{file, 0, 0, {}, {}};
  const auto temporary = std::filesystem::path(file.string() + ".compacting");
  try {
    // The writer's lock, held until the rewrite is in place.
    const auto lock = FileJournal::resume(file.string());
    const auto recovery = FileJournal::read(file.string(), lock->head());
    result.bytes_before = result.bytes_after = std::filesystem::file_size(file);
    // Only records with whole states shrink; a final schema 1 record stays.
    bool whole = false;
    for (const auto& r : recovery.records) {
      const auto schema = nlohmann::json::parse(r.payload).at("schema");
      if (schema == 2 || (schema == 1 && r.seq < recovery.records.size())) { whole = true; break; }
    }
    if (!whole) return result;
    std::filesystem::remove(temporary);  // Left by a run that stopped part way.
    std::string expected;
    {
      const auto out = FileJournal::create(temporary.string());
      expected = TradingSession::compact(recovery, *out);
    }
    if (TradingSession::recover(FileJournal::read(temporary.string())).snapshot_json() != expected)
      throw TradingError(Reason::JOURNAL_CORRUPT, "the rewrite does not recover to the same account");
    auto backup = std::filesystem::path(file.string() + ".bak");
    for (int n = 2; std::filesystem::exists(backup); ++n) backup = file.string() + ".bak" + std::to_string(n);
    std::filesystem::create_hard_link(file, backup);
    std::filesystem::rename(temporary, file);
    sync_directory(std::filesystem::absolute(file).parent_path());
    result.backup = backup;
    result.bytes_after = std::filesystem::file_size(file);
  } catch (const TradingError& error) {
    result.error = std::string(to_string(error.code())) + ": " + error.what();
  } catch (const std::exception& error) {
    result.error = error.what();
  }
  if (!result.error.empty()) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  }
  return result;
}
}  // namespace

std::vector<JournalCompaction> compact_paper_journals(const std::filesystem::path& journal,
                                                      const std::filesystem::path& accounts) {
  std::vector<JournalCompaction> results;
  std::error_code ec;
  if (!journal.empty() && std::filesystem::exists(journal, ec)) results.push_back(compact_journal(journal));
  if (!accounts.empty() && std::filesystem::is_directory(accounts, ec)) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(accounts, ec))
      if (entry.path().extension() == ".jsonl" && account_id(entry.path().stem().string())) files.push_back(entry.path());
    std::sort(files.begin(), files.end(), by_account_id);
    for (const auto& file : files) results.push_back(compact_journal(file));
  }
  if (!journal.empty()) {
    const auto replays = std::filesystem::absolute(journal).parent_path() / "replays";
    if (std::filesystem::is_directory(replays, ec)) {
      for (const auto& entry : std::filesystem::directory_iterator(replays, ec))
        if (entry.is_regular_file(ec) && entry.path().extension() == ".jsonl") results.push_back(compact_journal(entry.path()));
    }
  }
  return results;
}

Desk::PaperAccount* Desk::find_account(std::string_view id) {
  if (id.empty()) id = kMainAccount;
  for (auto& account : accounts_)
    if (account.id == id) return &account;
  return nullptr;
}

void Desk::start_trading() {
  if (!options_.paper_enabled) return;
  // Each account opens on its own: one that fails reports why and the rest trade.
  // False: a named account's journal was empty, so it held no account, and is removed.
  const auto open = [&](PaperAccount& account, const std::filesystem::path& file, bool seed) {
    try {
      std::shared_ptr<Journal> journal = file.empty() ? options_.paper_sink : nullptr;
      std::optional<JournalRecovery> recovery;
      if (!file.empty() && options_.resume && file == options_.paper_journal)
        journal = resuming_journal(*options_.resume, FileJournal::resume(file.string(), journal_options(options_)));
      else if (!file.empty()) {
        try { std::tie(journal, recovery) = open_journal(file, journal_options(options_)); }
        catch (const TradingError& error) {
          if (!options_.run_input.empty() || (error.code() != Reason::JOURNAL_CORRUPT &&
              !std::string_view(error.what()).starts_with("Torn journal suffix"))) throw;
          std::tie(journal, recovery) = FileJournal::inspect(file.string());
          if (recovery->damage.empty()) throw;
          const auto hint = recovery->truncated_final_line
              ? "; stop openportd and run --repair-journals --dry-run, then --repair-journals"
              : "; stop openportd and restore a verified backup; --repair-journals --dry-run diagnoses damage but cannot cut mid-file damage";
          account.damaged = AccountDamage{recovery->damage + hint, recovery->records.size(),
              recovery->records.empty() ? 0 : recovery->records.back().time};
          account.failure = "ACCOUNT_DAMAGED: " + account.damaged->reason;
        }
      }
      account.journal = journal;
      if (recovery && !options_.run_input.empty())
        throw TradingError(Reason::JOURNAL_CORRUPT, "A reproducible run needs a new journal; select an unused --paper-journal path");
      if (account.damaged && recovery->records.empty()) account.session = std::make_unique<TradingSession>(options_.paper, 0);
      else if (recovery) account.session = std::make_unique<TradingSession>(TradingSession::recover(*recovery, account.damaged ? nullptr : journal));
      else if (seed) account.session = std::make_unique<TradingSession>(options_.paper, 0, journal, options_.initial_actor);
      else {
        // Its create stopped before the first record. Removing the file while its
        // lock is held leaves nothing another writer could be using.
        std::filesystem::remove(file);
        std::error_code ignored;
        const auto named = std::filesystem::path(file).replace_extension(".name");
        if (std::filesystem::is_regular_file(named, ignored)) std::filesystem::remove(named, ignored);
        return false;
      }
      if (!file.empty()) account.equity = std::make_unique<EquityStore>(file.string() + ".equity.csv", account.damaged.has_value());
      account.session->set_exact_margin(options_.exact_margin);
      account.session->set_actor("system");
      account.sampled_snapshot = account.session->snapshot();
    } catch (const TradingError& error) {
      account.failure = std::string(to_string(error.code())) + ": " + error.what();
    } catch (const std::filesystem::filesystem_error& error) {
      account.failure = "JOURNAL_IO: " + error.code().message();
    } catch (const std::exception& error) {
      account.failure = std::string("JOURNAL_IO: ") + error.what();
    }
    return true;
  };
  accounts_.push_back({std::string(kMainAccount), "Main", nullptr, {}, nullptr, {}, {}});
  open(accounts_.back(), options_.paper_journal, true);
  if (options_.sandboxes) {
    if (!accounts_.back().session) throw std::runtime_error("Sandbox startup requires the main journal writer lock");
    if (!accounts_.back().damaged) std::filesystem::remove_all(options_.paper_journal.parent_path() / "sandboxes");
  }
  std::error_code ec;
  if (!options_.paper_accounts.empty() && std::filesystem::is_directory(options_.paper_accounts, ec)) {
    const auto deleted = options_.paper_accounts / "deleted";
    if (accounts_.front().session && !accounts_.front().damaged && std::filesystem::is_directory(deleted))
      for (const auto& entry : std::filesystem::directory_iterator(deleted))
        if (entry.is_directory() && account_id(entry.path().filename().string()))
          retain_deleted_files(options_.paper_accounts, entry.path().filename().string());
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(options_.paper_accounts, ec))
      if (entry.path().extension() == ".jsonl" && account_id(entry.path().stem().string()) &&
          !std::filesystem::exists(deleted / entry.path().stem())) files.push_back(entry.path());
    std::sort(files.begin(), files.end(), by_account_id);
    for (const auto& file : files) {
      PaperAccount account{file.stem().string(), file.stem().string(), nullptr, {}, nullptr, {}, {}};
      std::ifstream named(std::filesystem::path(file).replace_extension(".name"));
      if (std::string name; named && std::getline(named, name) && valid_account_name(name)) account.name = name;
      std::ifstream archived(std::filesystem::path(file).replace_extension(".archived"));
      std::string flag;
      if (archived && std::getline(archived, flag)) account.archived = flag != "false";
      if (open(account, file, false)) accounts_.push_back(std::move(account));
    }
  }
  for (const auto& account : accounts_) {
    if (!account.session) continue;
    market_time_ = std::max(market_time_, account.session->snapshot()->time);
    for (const auto& [symbol, contract] : account.session->contracts())
      if (const auto quote = account.session->quote(symbol))
        observations_[symbol] = std::max(observations_[symbol], quote->observation);
  }
  breaker_storage_ = !options_.replay && !options_.paper_journal.empty() && accounts_.front().session != nullptr && !accounts_.front().damaged;
  load_circuit_breaker();
  publish_circuit_breaker();
  if (accounts_.front().session && !accounts_.front().damaged) {
    try {
      const auto file = options_.paper_journal.empty() ? std::filesystem::path{} : options_.paper_journal.parent_path() / (options_.replay ? options_.paper_journal.stem().string() + ".playbooks.json" : "playbooks.json");
      playbooks_ = std::make_shared<Playbooks>(file, options_.initial_playbooks.empty() ? nlohmann::json(nullptr) : nlohmann::json::parse(options_.initial_playbooks));
      if (!options_.paper_accounts.empty()) {
        const auto modes = playbooks_->catalogue().at("modes");
        for (const auto& [id, bindings] : modes.items())
          if (std::filesystem::exists(options_.paper_accounts / "deleted" / id)) playbooks_->remove_account(id);
      }
    } catch (const std::exception& error) {
      fail_trading(accounts_.front(), std::string("PLAYBOOK_STORAGE: ") + error.what());
    }
  }
  if (!options_.run_input.empty()) {
    nlohmann::json start{{"kind", "start"}, {"input", nlohmann::json::parse(options_.run_input)},
        {"calendar", md::scheduled_days()}, {"analytics", options_.analytics}, {"dividends", dividends_}, {"symbols", subscription_.underlyings}};
    if (options_.instant_batches) start["driver"] = !options_.closing_rollover ? 2 : inputs_first() ? (options_.opening_settlement ? (options_.playbook_cancel_labels ? 6 : 5) : 4) : 3;
    if (options_.exact_margin) start["exact_margin"] = true;
    if (options_.opening_rule_checks) start["opening_rule_checks"] = true;
    if (options_.opening_rule_checks && options_.compact_opening_rule_inputs) start["compact_opening_rule_inputs"] = true;
    if (playbooks_ && (!options_.initial_playbooks.empty() || !playbooks_->catalogue().at("definitions").empty())) start["playbooks"] = playbooks_->catalogue();
    record_input(start.dump(), options_.initial_actor);
    if (options_.resume) {
      const auto inputs = run_inputs(*options_.resume);
      resume_inputs_.assign(inputs.begin() + (inputs.empty() ? 0 : 1), inputs.end());
      replay_recorded();
    }
  }
  publish_trading();
}

void Desk::expire_sandboxes(const std::vector<std::string>& expired) {
  if (!options_.sandboxes) return;
  if (expired.empty()) return;
  for (const auto& id : expired) {
    if (sandbox_ids_.erase(id)) {
      std::erase_if(accounts_, [&](const auto& account) { return account.id == id; });
      trading_views_.erase(id);
      playbook_publications_.erase(id);
    }
    std::error_code error;
    std::filesystem::remove_all(options_.paper_journal.parent_path() / "sandboxes" / id, error);
    if (!error) options_.sandboxes->removed(id);
  }
  publish_trading();
}

void Desk::create_account(const TradingCommand& c, TradingReply& reply) {
  const bool sandbox = c.kind == TradingCommand::Kind::CreateSandbox;
  if (!options_.paper_enabled || stopping_) {
    reply.error_code = "TRADING_UNAVAILABLE";
    reply.decision.message = "Paper trading is disabled or the engine is stopping";
    return;
  }
  // Lasting, unlike a full inbox: a replay or a server without an accounts directory
  // keeps one account, so a retry cannot succeed.
  if (sandbox ? !options_.sandboxes : options_.paper_accounts.empty()) {
    reply.error_code = "ACCOUNTS_UNSUPPORTED";
    reply.decision.message = "This server keeps a single paper account";
    return;
  }
  const auto* source = c.copy_settings_from.empty() ? nullptr : find_account(c.copy_settings_from);
  if (!c.copy_settings_from.empty() && !source) {
    reply.error_code = "UNKNOWN_ACCOUNT";
    reply.decision.message = "No settings source account " + c.copy_settings_from;
    return;
  }
  if (source && !source->session) {
    reply.error_code = "TRADING_UNAVAILABLE";
    reply.decision.message = "The settings source account is unavailable";
    return;
  }
  auto base = slug(c.name);
  if (base.empty() || base == kMainAccount) base = "account";
  auto id = sandbox ? c.account : base;
  if (sandbox && (!account_id(id) || find_account(id))) {
    reply.error_code = "SANDBOX_UNAVAILABLE";
    reply.decision.message = "Sandbox account id is unavailable";
    return;
  }
  for (int n = 2; find_account(id) || (!sandbox &&
      (std::filesystem::exists(options_.paper_accounts / "deleted" / id) ||
       std::filesystem::exists(options_.paper_accounts / (id + ".jsonl")))); ++n) id = base + "-" + std::to_string(n);
  PaperAccount account{id, sandbox ? "Sandbox" : c.name, nullptr, {}, nullptr, {}, {}};
  const auto directory = sandbox ? options_.paper_journal.parent_path() / "sandboxes" / id : options_.paper_accounts;
  const auto file = directory / (id + ".jsonl");
  const auto named = directory / (id + ".name");
  std::shared_ptr<Journal> created;  // the new journal, locked until a failed create removes it
  // A failed create leaves nothing behind: no transaction of the account committed.
  const auto discard = [&] {
    std::error_code ignored;
    if (sandbox) std::filesystem::remove_all(directory, ignored);
    if (sandbox || !created) return;
    std::filesystem::remove(file, ignored);
    if (std::filesystem::is_regular_file(named, ignored)) std::filesystem::remove(named, ignored);
  };
  try {
    // Every new journal takes the operator's fee (--paper-fee); a sandbox always uses the practice plan.
    auto config = options_.paper;
    config.rules = sandbox ? find_plan("practice")->rules : c.rules;
    config.initial_cash = sandbox ? find_plan("practice")->initial_cash : c.initial_cash;
    if (source) {
      config.limits = source->session->config().limits;
      config.guardrails = source->session->config().guardrails;
      validate_limits(config.limits);
      validate_guardrails(config.guardrails);
    }
    auto [journal, recovery] = open_journal(file, journal_options(options_));
    if (recovery) throw TradingError(Reason::JOURNAL_CORRUPT, "An account journal already exists");
    created = journal;
    save_account_file(named, account.name + '\n');
    account.journal = journal;
    account.session = std::make_unique<TradingSession>(config, market_time_, account.journal, c.actor);
    account.session->set_exact_margin(options_.exact_margin);
    account.session->set_actor("system");
    account.equity = std::make_unique<EquityStore>(file.string() + ".equity.csv");
  } catch (const TradingError& error) {
    discard();
    reply.decision = {error.code(), error.what(), {}, {}, {}};
    return;
  } catch (const std::filesystem::filesystem_error& error) {
    discard();
    reply.decision = {Reason::JOURNAL_IO, error.code().message(), {}, {}, {}};
    return;
  } catch (const std::exception& error) {
    discard();
    reply.decision = {Reason::JOURNAL_IO, error.what(), {}, {}, {}};
    return;
  }
  if (sandbox) sandbox_ids_.insert(id);
  // After main, in ID order, as a restart recovers them.
  const auto position = std::upper_bound(accounts_.empty() ? accounts_.end() : accounts_.begin() + 1, accounts_.end(), id,
      [](const std::string& value, const PaperAccount& other) { return value < other.id; });
  accounts_.insert(position, std::move(account));
  publish_trading();
  reply.account = id;
}

void Desk::manage_account(const TradingCommand& c, TradingReply& reply) {
  const auto id = c.account.empty() ? std::string(kMainAccount) : c.account;
  const auto refuse = [&](std::string code, std::string message) {
    reply.error_code = std::move(code);
    reply.decision.message = std::move(message);
  };
  if (id == kMainAccount || options_.replay || sandbox_ids_.contains(id)) {
    refuse("ACCOUNT_PROTECTED", "Only named live accounts can be renamed, archived or deleted");
    return;
  }
  auto* account = find_account(id);
  if (!account) { refuse("UNKNOWN_ACCOUNT", "No paper account " + id); return; }
  if (stopping_ || account->failure.starts_with("JOURNAL_LOCKED:")) {
    refuse("TRADING_UNAVAILABLE", "The account is stopping or its journal is locked"); return;
  }
  try {
    if (c.kind == TradingCommand::Kind::DeleteAccount) {
      if (!account->archived && (!account->session || !account->session->snapshot()->positions.empty() ||
          !account->session->snapshot()->stocks.empty() || !account->session->snapshot()->open_orders.empty())) {
        refuse("ACCOUNT_NOT_EMPTY", "Archive the account before deleting it with positions, working orders or an unreadable journal");
        return;
      }
      const auto deleted = options_.paper_accounts / "deleted";
      std::filesystem::create_directories(deleted / id);
      sync_directory(deleted / id);
      sync_directory(deleted);
      sync_directory(options_.paper_accounts);
      // The tombstone committed: release every owner even if a file move fails.
      std::erase_if(accounts_, [&](const auto& a) { return a.id == id; });
      trading_views_.erase(id);
      playbook_publications_.erase(id);
      if (removal_sink_) removal_sink_(id);
      publish_trading();
      if (playbooks_) playbooks_->remove_account(id);
      retain_deleted_files(options_.paper_accounts, id);
      reply.account_result = nlohmann::json{{"deleted", id}}.dump();
    } else {
      if (!c.name.empty()) {
        if (!valid_account_name(c.name)) { refuse("INVALID_REQUEST", "Invalid account name"); return; }
        save_account_file(options_.paper_accounts / (id + ".name"), c.name + '\n');
        account->name = c.name;
      }
      if (c.archived) {
        save_account_file(options_.paper_accounts / (id + ".archived"), *c.archived ? "true\n" : "false\n");
        account->archived = *c.archived;
      }
      reply.account_result = nlohmann::json{{"account", {{"id", id}, {"name", account->name}, {"archived", account->archived}}}}.dump();
      publish_trading();
    }
    reply.account = id;
  } catch (const std::filesystem::filesystem_error& error) {
    refuse("TRADING_UNAVAILABLE", error.code().message());
  } catch (const std::exception& error) {
    refuse("TRADING_UNAVAILABLE", error.what());
  }
}

void Desk::flush_journals() {
  for (auto& account : accounts_) {
    if (!account.journal || !account.failure.empty()) continue;
    try { account.journal->flush(); }
    catch (const std::exception& error) {
      fail_trading(account, std::string("JOURNAL_IO: ") + error.what());
    }
  }
}

void Desk::sample_equity(PaperAccount& account) {
  if ((!account.equity && !options_.equity_sample) || !account.session) return;
  const auto append = [&](const EquitySample& sample) {
    if (account.equity) account.equity->append(sample);
    if (options_.equity_sample) options_.equity_sample(account.id, sample);
  };
  const auto& session = *account.session;
  const auto current = session.snapshot();
  const auto& snapshot = *current;
  const auto& previous = account.sampled_snapshot;
  // Until equity is fully marked again, as when a second expiry still awaits its
  // settlement, keep the earlier snapshot: the fills and deliveries since then are
  // sampled at the first complete mark instead of being skipped.
  if (previous && !snapshot.valuation_complete && !snapshot.journal_failed &&
      snapshot.evaluation.attempt == previous->evaluation.attempt) return;
  if (previous && current != previous)
    for (const auto& sample : fill_equity_samples(session, *previous)) append(sample);
  const auto& e = snapshot.evaluation;
  const auto& rules = session.config().rules;
  if (snapshot.valuation_complete && market_time_ > 0 && !snapshot.journal_failed) {
    EquitySample sample;
    sample.time = market_time_;
    sample.attempt = e.attempt;
    sample.equity = snapshot.equity;
    sample.peak = e.peak;
    if (rules.max_drawdown > Money{}) {
      sample.floor = e.floor;
      if (rules.drawdown_mode == DrawdownMode::EndOfDay) sample.tomorrow_floor = trading::evaluation_tomorrow_floor(e, rules, snapshot.equity);
    }
    if (rules.profit_target > Money{}) sample.target = e.starting_balance + rules.profit_target;
    append(sample);
  }
  account.sampled_snapshot = current;
}

void Desk::publish_trading() {
  if (!options_.paper_enabled) return;
  std::map<std::string, std::shared_ptr<const TradingView>, std::less<>> views;
  std::vector<AccountStatus> statuses;
  for (auto& account : accounts_) {
    std::shared_ptr<TradingView> view;
    if (account.session) {
      const auto& session = *account.session;
      view = std::make_shared<TradingView>();
      view->replay_start = options_.replay_start;
      view->replay_end = options_.replay_end;
      view->snapshot = session.snapshot();
      view->damaged = account.damaged;
      if (account.journal) {
        view->journal_transactions = account.journal->sequence();
        view->journal_bytes = account.journal->bytes();
        view->journal_head = account.journal->head();
      }
      if (playbooks_) {
        // Quote batches publish far more often than playbooks change.
        auto& published = playbook_publications_[account.id];
        if (published.second.empty() || published.first != playbooks_->revision())
          published = {playbooks_->revision(), playbooks_->publication(account.id, options_.replay).dump()};
        view->playbooks_json = published.second;
      }
      // An idle account's publication is as of the feed's market time, not its last
      // transaction: batches that change nothing are not transactions.
      if (!account.archived && !account.damaged && market_time_ > view->snapshot->time) {
        auto clocked = std::make_shared<trading::TradingSnapshot>(*view->snapshot);
        clocked->time = market_time_;
        view->snapshot = std::move(clocked);
      }
      view->config = session.config();
      view->contracts = session.contracts();
      view->valuations = session.valuations();
      view->run = run_;
      view->opening_settlement = !opening_source().empty();
      // Each underlying's own data time tells whether its feed has stalled. Until this
      // run has seen any (just after a restart), the account's last quotes stand in.
      for (const auto& [symbol, contract] : view->contracts) {
        if (const auto quote = session.quote(symbol)) {
          auto& time = view->market_times[contract.underlying];
          time = std::max(time, quote->time);
        }
      }
      for (const auto& [symbol, book] : book_.underlyings())
        if (book.data_time > 0) view->market_times[symbol] = book.data_time;
      // A newer observation than the account's refreshes the displayed size it took.
      for (const auto& [symbol, left] : session.sizes_left())
        if (const auto current = observations_.find(symbol); current != observations_.end() && current->second == left.observation)
          view->sizes_left.emplace(symbol, left);
      for (const auto& [symbol, time] : snapshots_) {
        auto& latest = view->market_times[symbol];
        latest = std::max(latest, time);
      }
      view->halts = breaker_.halts;
      std::map<std::string, double> vols;
      for (const auto& [underlying, bucket] : view->snapshot->risk.underlyings)
        if (const auto variance = close_variance(metrics(underlying))) vols[underlying] = *variance;
      view->breach = session.breach(vols);
      view->warnings = session.warnings(vols, dividends_);
      if (!account.archived && account.failure.empty()) sample_equity(account);
      if (account.equity) {
        view->equity_samples = account.equity->samples();
        view->equity_error = account.equity->error();
        view->equity_error_time = account.equity->error_time();
        view->equity_error_market_time = account.equity->error_market_time();
        view->equity_error_recovered = account.equity->error_recovered();
        if (account.damaged) {
          const auto& snapshot = *view->snapshot;
          std::erase_if(view->equity_samples, [&](const auto& sample) { return sample.time >= snapshot.time; });
          if (snapshot.time > 0 && snapshot.valuation_complete) {
            const auto& rules = view->config.rules;
            EquitySample last{snapshot.time, snapshot.evaluation.attempt, snapshot.equity, {}, snapshot.evaluation.peak, {}, {}, 0};
            if (rules.max_drawdown > Money{}) last.floor = snapshot.evaluation.floor;
            if (rules.profit_target > Money{}) last.target = snapshot.evaluation.starting_balance + rules.profit_target;
            if (rules.max_drawdown > Money{} && rules.drawdown_mode == DrawdownMode::EndOfDay)
              last.tomorrow_floor = evaluation_tomorrow_floor(snapshot.evaluation, rules, snapshot.equity);
            view->equity_samples.push_back(last);
          }
        }
      }
    }
    TradingStatus status;
    status.enabled = !account.archived && account.failure.empty() && view != nullptr;
    status.reason = account.archived ? "ACCOUNT_ARCHIVED" : account.failure;
    status.write = status.enabled ? options_.write_mode : "disabled";
    const auto& config = view ? view->config : options_.paper;
    status.fee_per_contract = config.fee_per_contract;
    status.initial_cash = config.initial_cash;
    status.plan = config.rules.plan;
    status.plan_id = preset_id(config.initial_cash, config.rules);
    if (view) {
      const auto& e = view->snapshot->evaluation;
      status.plan_id = preset_id(config.initial_cash,
          trading::original_program_rules(config.rules, e.starting_balance, e.size_scaling));
      status.account_version = view->snapshot->account_version;
      status.kill_latched = view->snapshot->risk.kill_latched;
      if (config.rules.evaluation()) {
        constexpr const char* names[] = {"active", "passed", "failed"};
        status.evaluation = names[static_cast<int>(view->snapshot->evaluation.status)];
      }
    }
    statuses.push_back({account.id, account.name, std::move(status),
        sandbox_ids_.contains(account.id) ? options_.sandboxes->idle().count() : 0, account.archived, account.damaged, view ? view->journal_bytes : 0, view ? view->journal_transactions : 0, options_.replay});
    if (view && !account.archived && !account.damaged && publication_sink_) {
      // Delivery observers cannot invalidate an already committed transaction.
      try { publication_sink_(account.id, *view); } catch (...) {}
    }
    if (view) views.emplace(account.id, std::move(view));
  }
  trading_views_ = std::move(views);
  if (!statuses.empty()) trading_status_ = statuses.front().trading;
  account_statuses_ = std::move(statuses);
}

void Desk::fail_trading(PaperAccount& account, std::string reason) {
  account.failure = std::move(reason);
  publish_trading();
}

void Desk::observe_trading(const md::Event& event) {
  if (const auto* definition = std::get_if<md::ContractDefinition>(&event)) {
    instruments_[definition->contract.osi_symbol()] = definition->id;
  } else if (const auto* quote = std::get_if<md::OptionQuote>(&event)) {
    market_time_ = std::max(market_time_, quote->ts);
    if (const auto* option = book_.option(quote->id)) {
      auto& number = observations_[option->contract.osi_symbol()];
      if (number == std::numeric_limits<std::uint64_t>::max()) {
        for (auto& account : accounts_) fail_trading(account, "ARITHMETIC_OVERFLOW: quote observation exhausted");
      } else {
        ++number;
      }
    }
  } else if (const auto* spot = std::get_if<md::UnderlyingQuote>(&event)) {
    market_time_ = std::max(market_time_, spot->ts);
  } else if (const auto* heartbeat = std::get_if<md::SnapshotHeartbeat>(&event)) {
    market_time_ = std::max(market_time_, heartbeat->ts);
  } else if (const auto* halt = std::get_if<md::TradingHalt>(&event)) {
    market_time_ = std::max(market_time_, halt->ts);
  } else if (const auto* snapshot = std::get_if<md::SnapshotComplete>(&event)) {
    // It vouches for quotes but does not move the market clock: a provider may stamp
    // it with the wall clock (ThetaData), which can run ahead of its data.
    auto& time = snapshots_[snapshot->underlying];
    time = std::max(time, snapshot->ts);
  } else if (const auto* trade = std::get_if<md::OptionTrade>(&event)) {
    market_time_ = std::max(market_time_, trade->ts);
  }
}

void Desk::advance_circuit_breaker(md::Timestamp time) {
  if (time <= 0) return;
  const auto day = md::trading_date(time);
  if (day < breaker_.day) return;
  if (day != breaker_.day) {
    breaker_.day = day;
    breaker_.level = 0;
    breaker_.previous_close.reset();
    breaker_dirty_ = true;
  }
  const auto previous = md::previous_business_day(day);
  auto reference = breaker_.previous_close;
  if (const auto close = official_closes_.find({breaker_.symbol, previous}); close != official_closes_.end())
    reference = CircuitBreakerStatus::Close{previous, close->second.price};
  else if (const auto print = closing_prints_.find({breaker_.symbol, previous}); print != closing_prints_.end())
    reference = CircuitBreakerStatus::Close{previous, print->second.last};
  if (reference != breaker_.previous_close) {
    breaker_.previous_close = reference;
    breaker_dirty_ = true;
  }
}

void Desk::publish_circuit_breaker() {
  advance_circuit_breaker(market_time_);
  breaker_.market_time = market_time_;
  const bool active = std::any_of(breaker_.halts.begin(), breaker_.halts.end(), [&](const MarketHalt& halt) {
    return halt.start <= market_time_ && market_time_ < halt.end;
  });
  if (active != breaker_.active) breaker_dirty_ = true;
  breaker_.active = active;
  if (breaker_dirty_) save_circuit_breaker();
  breaker_dirty_ = false;
  
}

void Desk::load_circuit_breaker() {
  if (!breaker_storage_) return;
  const auto file = options_.paper_journal.parent_path() / "market-halts.json";
  try {
    if (!std::filesystem::exists(file)) return;
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot read file");
    const auto data = nlohmann::json::parse(in);
    const auto date = [](const nlohmann::json& value) {
      const auto text = value.get<std::string>();
      const auto time = md::parse_datetime(text + "T00:00:00Z", md::Zone::Utc);
      if (text.size() != 10 || !time) throw std::runtime_error("invalid date");
      return md::date_from_days(*time / md::kNanosPerDay);
    };
    CircuitBreakerStatus recovered;
    recovered.symbol = data.at("symbol").get<std::string>();
    recovered.day = date(data.at("day"));
    recovered.level = data.at("level").get<int>();
    recovered.market_time = data.at("market_time").get<md::Timestamp>();
    if (data.at("schema") != 1 || !data.at("level").is_number_integer() ||
        !data.at("market_time").is_number_integer() || data.at("level") < 0 || data.at("level") > 3 ||
        (recovered.symbol != "SPX" && recovered.symbol != "SPY") || recovered.market_time <= 0 ||
        md::trading_date(recovered.market_time) != recovered.day || !data.at("halts").is_array() ||
        data.at("halts").size() > 771)
      throw std::runtime_error("invalid breaker state");
    const auto& close = data.at("previous_close");
    if (!close.is_null()) {
      recovered.previous_close = CircuitBreakerStatus::Close{date(close.at("date")), close.at("price").get<double>()};
      if (recovered.previous_close->date != md::previous_business_day(recovered.day) ||
          !(recovered.previous_close->price > 0) || !std::isfinite(recovered.previous_close->price))
        throw std::runtime_error("invalid previous close");
    }
    int level = 0;
    for (const auto& value : data.at("halts")) {
      MarketHalt halt{value.at("level").get<int>(), value.at("start").get<md::Timestamp>(),
                      value.at("end").get<md::Timestamp>(), value.at("reference").get<double>(),
                      value.at("price").get<double>()};
      if (halt.level == 0) {
        if (!value.at("level").is_number_integer() || !value.at("start").is_number_integer() ||
            !value.at("end").is_number_integer() || halt.start <= 0 || halt.start > recovered.market_time ||
            halt.end <= halt.start || halt.end - halt.start > md::kNanosPerDay || halt.reference != 0 || halt.price != 0)
          throw std::runtime_error("invalid authored halt");
        recovered.halts.push_back(halt);
        continue;
      }
      const auto expected = circuit_breaker(halt.reference, halt.price, halt.start, level);
      if (!value.at("level").is_number_integer() || value.at("level") < 1 || value.at("level") > 3 ||
          !value.at("start").is_number_integer() ||
          !value.at("end").is_number_integer() || !expected || expected->level != halt.level ||
          expected->end != halt.end || halt.start > recovered.market_time ||
          (!recovered.halts.empty() && (halt.start < recovered.halts.back().start ||
           new_york_date(halt.start) != new_york_date(recovered.halts.back().start))))
        throw std::runtime_error("invalid halt");
      level = halt.level;
      recovered.halts.push_back(halt);
    }
    const int today = !recovered.halts.empty() && new_york_date(recovered.halts.back().start) == recovered.day ? level : 0;
    if (recovered.level != today) throw std::runtime_error("invalid tripped level");
    // A subscription change can change the proxy, but cannot undo a market-wide halt.
    if (recovered.symbol != breaker_.symbol) recovered.previous_close.reset();
    recovered.symbol = breaker_.symbol;
    breaker_ = std::move(recovered);
    market_time_ = std::max(market_time_, breaker_.market_time);
  } catch (const std::exception& error) {
    breaker_.error = "market halts: cannot read " + file.string() + ": " + error.what();
  }
}

void Desk::save_circuit_breaker() {
  if (!breaker_storage_) return;
  const auto file = options_.paper_journal.parent_path() / "market-halts.json";
  const auto temporary = std::filesystem::path(file.string() + ".tmp");
  try {
    using nlohmann::json;
    json halts = json::array();
    for (const auto& halt : breaker_.halts)
      halts.push_back({{"level", halt.level}, {"start", halt.start}, {"end", halt.end},
                       {"reference", halt.reference}, {"price", halt.price}});
    const auto& close = breaker_.previous_close;
    const json data{{"schema", 1}, {"symbol", breaker_.symbol}, {"day", md::format_date(breaker_.day)},
                     {"level", breaker_.level}, {"market_time", breaker_.market_time}, {"halts", halts},
                     {"previous_close", close ? json{{"date", md::format_date(close->date)}, {"price", close->price}} : json(nullptr)}};
    {
      std::ofstream out(temporary, std::ios::trunc | std::ios::binary);
      out << data.dump() << '\n';
      out.close();
      if (!out) throw std::runtime_error("cannot write temporary file");
    }
    std::filesystem::rename(temporary, file);
  } catch (const std::exception& error) {
    breaker_.error = "market halts: cannot write " + file.string() + ": " + error.what();
  }
}

void Desk::check_circuit_breaker(const md::UnderlyingQuote& spot) {
  if (spot.symbol != breaker_.symbol || md::trading_date(spot.ts) < breaker_.day) return;
  advance_circuit_breaker(spot.ts);
  const double reference = breaker_.previous_close ? breaker_.previous_close->price : 0;
  if (const auto halt = circuit_breaker(reference, spot.last, spot.ts, breaker_.level)) {
    breaker_.level = halt->level;
    if (!breaker_.halts.empty() && new_york_date(breaker_.halts.back().start) != breaker_.day)
      breaker_.halts.clear();
    breaker_.halts.push_back(*halt);
    breaker_dirty_ = true;
  }
}

std::string Desk::opening_source() const {
  if (!options_.opening_settlement) return {};
  if (options_.replay)
    return inputs_first() ? (scenario_source_ ? "scenario_opening_print" : "recorded_opening_print") : "";
  return provider_ == "demo" ? "demo_opening_print" : "";
}

void Desk::update_trading(const std::vector<md::Event>& batch,
                            std::deque<PendingCommand>& commands, md::Timestamp driver_time) {
  // Dry runs prepare their market on private copies. Without new market events,
  // answering them must not revalue or advance the live account.
  if (batch.empty() && std::all_of(commands.begin(), commands.end(), [](const auto& pending) {
        return dry_run(pending.command.kind);
      })) return;
  const auto now = driver_time;
  // Underlying prints retain ingress order. The first valid last on a date at or
  // after its regular close (16:00, 13:00 early) is our documented PM
  // closing-print approximation; the last one before it stands in when none comes.
  for (const auto& event : batch) {
    if (const auto* halt = std::get_if<md::TradingHalt>(&event)) {
      if (halt->ts > 0 && halt->end > halt->ts && halt->end - halt->ts <= md::kNanosPerDay) {
        advance_circuit_breaker(halt->ts);
        if (!breaker_.halts.empty() && new_york_date(breaker_.halts.back().start) != breaker_.day) breaker_.halts.clear();
        breaker_.halts.push_back({0, halt->ts, halt->end, 0, 0});
        breaker_dirty_ = true;
      }
      continue;
    }
    if (const auto* close = std::get_if<md::UnderlyingClose>(&event)) {
      if (!(close->price > 0) || !std::isfinite(close->price)) continue;
      official_closes_[{close->symbol, close->date}] = *close;
      const auto oldest = md::date_from_days(md::days_since_epoch(close->date) - 7);
      std::erase_if(official_closes_, [&](const auto& entry) { return entry.first.second < oldest; });
      continue;
    }
    const auto* spot = std::get_if<md::UnderlyingQuote>(&event);
    if (!spot || !std::isfinite(spot->last) || spot->last <= 0) continue;
    check_circuit_breaker(*spot);
    const auto date = new_york_date(spot->ts);
    const auto open = md::new_york_to_utc(date, 9, 30);
    if (!opening_source().empty() && spot->ts >= open &&
        (options_.replay || spot->ts <= open + kOpeningPrintWait)) {
      // Replay drivers retain their recorded behavior; live restarts cannot invent an open.
      // Ingress order matters: premarket prints and later revisions never replace it.
      opening_prints_.try_emplace(std::make_pair(spot->symbol, date), *spot);
      const auto oldest = md::date_from_days(md::days_since_epoch(date) - 7);
      std::erase_if(opening_prints_, [&](const auto& entry) { return entry.first.second < oldest; });
    }
    const bool closed = spot->ts >= md::new_york_to_utc(date, md::regular_close_hour(date), 0);
    auto& prints = closed ? closing_prints_ : before_close_;
    const auto [it, added] = prints.try_emplace({spot->symbol, date}, *spot);
    if (!closed && spot->ts >= it->second.ts) it->second = *spot;
    if (added) {
      // A week of dates covers every expiry still waiting on its print.
      const auto oldest = md::date_from_days(md::days_since_epoch(date) - 7);
      std::erase_if(prints, [&](const auto& entry) { return entry.first.second < oldest; });
    }
  }
  publish_circuit_breaker();
  for (const auto& event : batch) {
    const auto* complete = std::get_if<md::SnapshotComplete>(&event);
    if (!complete) continue;
    // An older snapshot arriving late vouches for nothing newer.
    const auto latest = snapshots_.find(complete->underlying);
    if (latest != snapshots_.end() && complete->ts >= latest->second) vouched_at_[complete->underlying] = market_time_;
  }
  for (auto& account : accounts_) {
    if (account.archived || !account.session || !account.failure.empty()) continue;
    auto& session = *account.session;
    // An overnight session belongs to the next trading date, so a day ends when the
    // last session of the one before (curb) does, or at the plan's own boundary.
    const auto day = session.trading_date(market_time_);
    try {
      session.set_opening_rule_inputs(opening_rule_inputs(account, market_time_), options_.compact_opening_rule_inputs);
      const auto roll = [&] {
        if (!batch.empty() && day > session.trading_day() &&
            md::market_session(md::new_york_to_utc(day, 12, 0)).open &&
            session.snapshot()->valuation_complete)
          session.roll_day(market_time_, trading::dividends_due(dividends_, session.trading_day(), day));
      };
      // A new trading date's first batch closes the finished day on its own marks
      // before its quotes replace them, so the overnight move and any fill at the
      // open belong to the new day. A PM position of the finished day still to
      // settle settles first, and rolls the day over after the quotes below.
      if (options_.instant_batches && options_.closing_rollover) {
        const auto& positions = session.snapshot()->positions;
        if (std::none_of(positions.begin(), positions.end(), [&](const auto& p) {
              const auto& contract = p.position.contract;
              return contract.settlement == md::Settlement::PM && market_time_ >= contract.expiry_time();
            }))
          roll();
      }
      std::set<std::string> symbols;
      for (const auto& p : session.snapshot()->positions) symbols.insert(p.position.contract.osi_symbol());
      for (const auto& order : session.snapshot()->open_orders)
        for (const auto& symbol : chain_symbols(order.request)) symbols.insert(symbol);
      for (const auto& pending : commands) {
        const auto& command = pending.command;
        if ((command.account.empty() ? kMainAccount : std::string_view(command.account)) != account.id) continue;
        if (command.kind == TradingCommand::Kind::Submit)
          for (const auto& symbol : chain_symbols(command.order)) symbols.insert(symbol);
        if (command.kind == TradingCommand::Kind::Settle) symbols.insert(command.symbol);
        if (command.kind == TradingCommand::Kind::CreateAlert) {
          if (command.alert.condition.scope == AlertScope::Contract) symbols.insert(command.alert.condition.symbol);
          for (const auto& leg : command.alert.condition.legs) symbols.insert(leg.symbol);
        }
      }
      // Alerts on contracts the account does not trade still need their quotes.
      for (const auto& alert : session.snapshot()->alerts) {
        if (!alert.armed && !alert.spec.repeat) continue;
        if (alert.spec.condition.scope == AlertScope::Contract) symbols.insert(alert.spec.condition.symbol);
        for (const auto& leg : alert.spec.condition.legs) symbols.insert(leg.symbol);
      }
      // A stalled feed cannot replenish resting-order liquidity, and nothing trades
      // while a circuit breaker halts the market: then quotes are not offered, and age.
      // The orders and reducer clock stay intact; ordinary market-time DAY/expiry rules
      // still apply.
      const auto open = [&](const std::string& underlying, md::Timestamp time) {
        const auto gate = paper_acceptance(underlying, time, now, capabilities_.delay,
                                           session.config().limits.max_quote_age, breaker_.halts).code;
        return time > 0 && gate != Reason::FEED_STALLED && gate != Reason::MARKET_HALTED;
      };
      // Snapshot feeds send only the quotes that changed, and underlyings' snapshots run
      // apart (Cboe's quote pages trail its data files by a minute or two). While an
      // underlying's last complete snapshot is within the stall tolerance, its quotes,
      // valuations and share price are current at the market time. Without snapshots (a
      // streaming feed), each quote keeps the time it last changed.
      std::map<std::string, bool> vouched;
      const auto current = [&](const std::string& underlying) {
        const auto [it, added] = vouched.try_emplace(underlying, false);
        if (added) {
          const auto snapshot = snapshots_.find(underlying);
          it->second = snapshot != snapshots_.end() && open(underlying, snapshot->second);
        }
        return it->second;
      };
      // Current is not new: fill latency needs a quote supplied after the order. A vouched
      // quote is offered at the market time its own snapshot arrived (or its own newer
      // time), so another underlying's data cannot release an order on it. Only once that
      // is older than the freshness window allows does it follow the market time, keeping
      // the quote current until its own feed stalls.
      const auto offered = [&](const std::string& underlying, md::Timestamp time) {
        if (!options_.instant_batches) return market_time_;
        if (const auto arrived = vouched_at_.find(underlying); arrived != vouched_at_.end())
          time = std::max(time, arrived->second);
        return std::clamp(time, market_time_ - session.config().limits.max_quote_age, market_time_);
      };
      std::vector<QuoteObservation> quotes;
      std::vector<Valuation> valuations;
      for (const auto& symbol : symbols) {
        const auto id = instruments_.find(symbol);
        const auto* option = id == instruments_.end() ? nullptr : book_.option(id->second);
        if (option && !session.contracts().contains(symbol)) {
          const auto result = session.define(option->contract, market_time_);
          if (!result.decision.ok()) continue;
        }
        const auto definition = session.contracts().find(symbol);
        if (definition == session.contracts().end()) continue;
        if (option) {
          const auto& a = option->contract;
          const auto& b = definition->second;
          if (a.root != b.root || a.underlying != b.underlying || a.expiry != b.expiry ||
              a.strike != b.strike || a.type != b.type || a.style != b.style ||
              a.settlement != b.settlement || a.multiplier != b.multiplier || a.standard != b.standard)
            throw TradingError(Reason::INVALID_CONTRACT, "INVALID_CONTRACT: listed terms conflict with registered definition");
        }
        const auto& underlying = definition->second.underlying;
        const bool quoted = option && option->has_quote && option->quote_ts >= 0;
        const bool fresh = current(underlying);
        if (quoted && (fresh || open(underlying, option->quote_ts))) {
          // A vouched quote offered later than the provider gave it keeps that first time.
          const auto time = fresh ? offered(underlying, option->quote_ts) : option->quote_ts;
          quotes.push_back({symbol, observations_[symbol], time, quote_price(option->bid), quote_price(option->ask),
                            whole_size(option->bid_size), whole_size(option->ask_size), std::min(option->quote_ts, time)});
        }
        auto valuation = valuation_for(symbol, definition->second, metrics(underlying));
        if (fresh && valuation.time > 0) valuation.time = market_time_;
        // Missing live analytics after recovery must not overwrite a recorded frame, and
        // a valuation ahead of the market time the account runs on would fail it.
        if (valuation.time > 0 && valuation.time <= market_time_) valuations.push_back(std::move(valuation));
      }
      // Underlyings price the shares that equity options deliver, and exercise.
      std::set<std::string> deliverable;
      for (const auto& p : session.snapshot()->positions) {
        const auto& c = p.position.contract;
        if (c.style == pricing::ExerciseStyle::American && !md::is_index_underlying(c.underlying)) deliverable.insert(c.underlying);
      }
      for (const auto& stock : session.snapshot()->stocks) deliverable.insert(stock.position.symbol);
      std::vector<StockPrice> stocks;
      for (const auto& symbol : deliverable) {
        const auto book = book_.underlyings().find(symbol);
        if (book == book_.underlyings().end() || book->second.spot_ts <= 0 || book->second.spot_ts > market_time_) continue;
        const auto time = current(symbol) ? market_time_ : book->second.spot_ts;
        if (const auto price = quote_price(book->second.spot)) stocks.push_back({symbol, time, *price});
      }
      // Conditional triggers watch other underlyings' prices and studies, read from
      // the same analytics that value the contracts, so a replay supplies them alike.
      std::set<std::pair<std::string, std::string>> watched;
      const auto watch = [&](const OrderRequest& request) {
        if (!request.trigger || !conditional(*request.trigger) || request.trigger->source == TriggerSource::Time) return;
        auto symbol = request.trigger->symbol;
        if (symbol.empty()) {
          const auto definition = session.contracts().find(order_symbols(request).front());
          if (definition == session.contracts().end()) return;
          symbol = definition->second.underlying;
        }
        watched.emplace(symbol, request.trigger->study);
      };
      for (const auto& order : session.snapshot()->open_orders)
        if (order.status == OrderStatus::Armed) watch(order.request);
      // Alerts on an underlying read the same values.
      const auto watch_alert = [&](const AlertSpec& spec) {
        if (spec.condition.scope == AlertScope::Underlying)
          watched.emplace(spec.condition.symbol, spec.condition.metric == "price" ? "" : spec.condition.metric);
      };
      for (const auto& alert : session.snapshot()->alerts)
        if (alert.armed || alert.spec.repeat) watch_alert(alert.spec);
      for (const auto& pending : commands)
        if ((pending.command.account.empty() ? kMainAccount : std::string_view(pending.command.account)) == account.id &&
            (pending.command.kind == TradingCommand::Kind::Submit || pending.command.kind == TradingCommand::Kind::CreateAlert)) {
          if (pending.command.kind == TradingCommand::Kind::Submit) watch(pending.command.order);
          else watch_alert(pending.command.alert);
        }
      std::vector<Indicator> indicators;
      for (const auto& [symbol, study] : watched) {
        const auto m = metrics(symbol);
        if (!m || m->as_of <= 0 || m->as_of > market_time_) continue;
        double value = m->spot;
        if (!study.empty()) {
          // As the Volatility page reports them: model-free 30-day volatility (at the money
          // when the strip cannot give it), 7-day at the money, and the 9/30-day ratio.
          const auto volatility = cached_volatility(m);
          value = study == "term_ratio" ? volatility.ratio9_30
              : study == "iv7" ? (!volatility.atm.empty() ? volatility.atm[0].vol : std::nan(""))
              : volatility.mfiv.size() > 1 && std::isfinite(volatility.mfiv[1].vol) ? volatility.mfiv[1].vol
              : volatility.atm.size() > 1 ? volatility.atm[1].vol : std::nan("");
        }
        const auto price = quote_price(value);
        if (price) indicators.push_back({symbol, study, current(symbol) ? market_time_ : m->as_of, *price});
      }
      session.set_opening_rule_inputs(opening_rule_inputs(account, market_time_), options_.compact_opening_rule_inputs);
      session.on_quotes(quotes, valuations, market_time_, stocks, indicators);
      sample_equity(account);
      // Each account keeps the closing print its PM positions will settle on, so
      // a restart before they expire (16:15 for ETF options) still uses it. When
      // none has come half an hour into the market's evening, the last print
      // before the close stands in, if it came in the close's last five minutes.
      // Hold the snapshot: record_close publishes a new one and frees this one.
      const auto marked = session.snapshot();
      for (const auto& p : marked->positions) {
        const auto& contract = p.position.contract;
        if (contract.settlement != md::Settlement::PM) continue;
        // The provider's official close wins over any print, revisions included,
        // until the positions settle.
        const auto recorded = session.closing_print(contract.underlying, contract.expiry);
        if (const auto official = official_closes_.find({contract.underlying, contract.expiry});
            official != official_closes_.end()) {
          const auto price = Money::from_double(official->second.price);
          if (!recorded || recorded->price != price)
            session.record_close(contract.underlying, contract.expiry, price, official->second.ts, market_time_);
          continue;
        }
        if (recorded) continue;
        const auto close = md::new_york_to_utc(contract.expiry, md::regular_close_hour(contract.expiry), 0);
        auto print = closing_prints_.find({contract.underlying, contract.expiry});
        if (print == closing_prints_.end() && market_time_ >= close + kLastPrintWait) {
          print = before_close_.find({contract.underlying, contract.expiry});
          if (print != before_close_.end() && close - print->second.ts > kLastPrintAge) print = before_close_.end();
          if (print == before_close_.end()) continue;
        } else if (print == closing_prints_.end()) {
          continue;
        }
        session.record_close(contract.underlying, contract.expiry, Money::from_double(print->second.last), print->second.ts, market_time_);
      }
      // A PM contract settles on its expiry date's closing print once it expires:
      // at the close, or a quarter hour later for ETF options that trade until 16:15.
      // Demo and driver-5 replay AM contracts use their date’s opening print.
      // Hold the snapshot: settle publishes a new one and frees this one.
      const auto expiring = session.snapshot();
      for (const auto& p : expiring->positions) {
        const auto& contract = p.position.contract;
        if (market_time_ < contract.expiry_time()) continue;
        if (contract.settlement == md::Settlement::AM) {
          const auto kind = opening_source();
          const auto opening = opening_prints_.find({contract.underlying, contract.expiry});
          if (kind.empty() || opening == opening_prints_.end() || opening->second.ts > market_time_) continue;
          const auto& print = opening->second;
          const SettlementSource source{{"kind", kind}, {"provider", provider_},
              {"symbol", contract.underlying}, {"quote_time", md::format_timestamp(print.ts)}};
          session.settle(contract.osi_symbol(), Money::from_double(print.last), market_time_, source);
          sample_equity(account);
          continue;
        }
        const auto print = session.closing_print(contract.underlying, contract.expiry);
        if (!print) continue;
        const bool before = print->time < md::new_york_to_utc(contract.expiry, md::regular_close_hour(contract.expiry), 0);
        const auto official = official_closes_.find({contract.underlying, contract.expiry});
        const bool is_official = official != official_closes_.end() && Money::from_double(official->second.price) == print->price;
        const SettlementSource source{{"kind", is_official ? "provider_official_close"
                                       : before  ? "provider_last_print_before_close"
                                                 : "provider_closing_print"},
                              {"provider", provider_},
                              {"symbol", contract.underlying}, {"quote_time", md::format_timestamp(print->time)}};
        session.settle(contract.osi_symbol(), print->price, market_time_, source);
        sample_equity(account);
      }
      // Otherwise the day rolls over once the marks are complete: after a settlement,
      // or on the new quotes when the finished day's marks were incomplete.
      roll();
    } catch (const TradingError& error) {
      account.failure = std::string(to_string(error.code())) + ": " + error.what();
    } catch (const std::exception& error) {
      account.failure = std::string("TRADING_UNAVAILABLE: ") + error.what();
    }
  }
  publish_trading();
  // Playbooks read the analytics, so they are evaluated when those change, not on
  // every batch of quotes.
  if (!playbook_running_ && playbook_generation_ != analytics_generation_) {
    playbook_generation_ = analytics_generation_;
    evaluate_playbooks(driver_time);
  }
}

void Desk::apply_command(PendingCommand& pending, md::Timestamp market_time, md::Timestamp driver_time) {
  apply_command(pending, market_time, driver_time, false);
}
std::vector<CounterExposure> Desk::counter_exposures(const PaperAccount& account, md::Timestamp time) const {
  std::vector<CounterExposure> result;
  if (!account.session->config().rules.no_counter_positions || options_.replay || sandbox_ids_.contains(account.id)) return result;
  for (const auto& other : accounts_) {
    if (other.id == account.id || other.archived || !other.session || sandbox_ids_.contains(other.id)) continue;
    const auto current = [&](const std::string& underlying) {
      const auto snapshot = snapshots_.find(underlying);
      return snapshot != snapshots_.end() && snapshot->second <= time &&
          time - snapshot->second <= other.session->config().limits.max_quote_age;
    };
    std::set<std::string> symbols;
    for (const auto& position : other.session->snapshot()->positions) symbols.insert(position.position.contract.osi_symbol());
    for (const auto& order : other.session->snapshot()->open_orders)
      for (const auto& symbol : order_symbols(order.request)) symbols.insert(symbol);
    std::vector<Valuation> valuations;
    for (const auto& symbol : symbols) {
      const auto& contract = other.session->contracts().at(symbol);
      auto valuation = valuation_for(symbol, contract, metrics(contract.underlying));
      if (current(contract.underlying) && valuation.time > 0) valuation.time = time;
      if (valuation.time > 0 && valuation.time <= time) valuations.push_back(std::move(valuation));
    }
    std::vector<StockPrice> stocks;
    for (const auto& held : other.session->snapshot()->stocks) {
      const auto& symbol = held.position.symbol;
      const auto spot = book_.underlyings().find(symbol);
      if (spot == book_.underlyings().end() || spot->second.spot_ts <= 0 || spot->second.spot_ts > time) continue;
      if (const auto price = quote_price(spot->second.spot))
        stocks.push_back({symbol, current(symbol) ? time : spot->second.spot_ts, *price});
    }
    const auto exposures = other.session->counter_exposures(other.id, time, valuations, stocks);
    result.insert(result.end(), exposures.begin(), exposures.end());
  }
  return result;
}
Decision Desk::counter_position_gate(const PaperAccount& account, const std::map<std::string, double>& direction,
                                     md::Timestamp time) const {
  return counter_position_check(direction, counter_exposures(account, time));
}
std::optional<std::int64_t> Desk::option_volume(const std::string& symbol, md::Timestamp time) const {
  const auto id = instruments_.find(symbol);
  const auto* option = id == instruments_.end() ? nullptr : book_.option(id->second);
  // Accept only exact, whole contract counts from the current market date.
  if (option && option->volume_ts > 0 && option->volume_ts <= time &&
      md::trading_date(option->volume_ts) == md::trading_date(time) && std::isfinite(option->volume) &&
      option->volume >= 0 && option->volume <= 9'007'199'254'740'991.0 && std::trunc(option->volume) == option->volume)
    return static_cast<std::int64_t>(option->volume);
  return std::nullopt;
}
std::optional<OpeningRuleInputs> Desk::opening_rule_inputs(const PaperAccount& account, md::Timestamp time) const {
  const auto& rules = account.session->config().rules;
  if (!options_.opening_rule_checks || (!rules.no_counter_positions && rules.max_volume_percent == 0)) return std::nullopt;
  OpeningRuleInputs inputs;
  inputs.counter_positions = counter_exposures(account, time);
  if (rules.max_volume_percent != 0)
    for (const auto& [symbol, contract] : account.session->contracts()) inputs.volumes[symbol] = option_volume(symbol, time);
  return inputs;
}
Decision Desk::opening_gate(const PaperAccount& account, const OrderRequest& order, md::Timestamp time,
                            const PreviewMarket& market, const std::map<std::string, Quantity>& preceding) const {
  const auto& session = *account.session;
  const bool counter = session.config().rules.no_counter_positions && !options_.replay && !sandbox_ids_.contains(account.id) &&
      std::any_of(accounts_.begin(), accounts_.end(), [&](const auto& other) {
        return other.id != account.id && !other.archived && other.session && !sandbox_ids_.contains(other.id);
      });
  const auto percent = session.config().rules.max_volume_percent;
  if (!counter && percent == 0) return {};
  const auto exposure = session.opening_order(order, time, market, preceding);
  if (!exposure.decision.ok() && (counter || exposure.decision.code != Reason::MISSING_VALUATION)) return exposure.decision;
  if (exposure.opening) {
    if (counter)
      if (const auto d = counter_position_gate(account, exposure.dollar_delta, time); !d.ok()) return d;
    if (percent != 0) for (const auto& [symbol, opening] : exposure.contracts) {
      Quantity held = 0;
      for (const auto& position : session.snapshot()->positions)
        if (position.position.contract.osi_symbol() == symbol) held = position.position.quantity;
      if (const auto it = preceding.find(symbol); it != preceding.end() && __builtin_add_overflow(held, it->second, &held))
        return {Reason::ARITHMETIC_OVERFLOW, "Chained holdings overflow", {}, {}, symbol};
      if (const auto d = volume_share_check(symbol, held, opening, percent, option_volume(symbol, time)); !d.ok()) return d;
    }
  }
  // Chained entries are accepted with their parent, so take the same server gate
  // now. Execution checks explicit current inputs inside the deterministic reducer.
  for (const auto& child : order.oco)
    if (const auto d = opening_gate(account, child, time, market, preceding); !d.ok()) return d;
  if (!order.then.empty()) {
    auto after = preceding;
    if (const auto d = project_contracts(order, after); !d.ok()) return d;
    for (const auto& child : order.then)
      if (const auto d = opening_gate(account, child, time, market, after); !d.ok()) return d;
  }
  return {};
}
void Desk::apply_command(PendingCommand& pending, md::Timestamp market_time, md::Timestamp driver_time, bool input_recorded) {
  market_time_ = std::max(market_time_, market_time);
  TradingReply reply;
  if (!input_recorded) price_stock_command(pending.command, driver_time);
  // Freeze the calendar in the command input too, so re-execution does not use
  // the run-start calendar if the live holiday schedule changed before reset.
  if (!pending.command.rules.hold_restrictions.empty() && !pending.command.rules.hold_calendar)
    pending.command.rules.hold_calendar = md::scheduled_days();
  const auto& c = pending.command;
  // Driver 4 records the command before its first transaction.
  if (!input_recorded && inputs_first() && recorded_input(c)) record_command(c, driver_time);
  auto* account = c.kind == TradingCommand::Kind::CreateAccount ? nullptr : find_account(c.account);
  if (account && account->damaged) {
    reply.error_code = "ACCOUNT_DAMAGED";
    reply.decision.message = account->damaged->reason;
  } else if (c.kind == TradingCommand::Kind::UpdateAccount || c.kind == TradingCommand::Kind::DeleteAccount) {
    manage_account(c, reply);
    account = find_account(c.account);
  } else if (c.kind == TradingCommand::Kind::CreateAccount || c.kind == TradingCommand::Kind::CreateSandbox) {
    create_account(c, reply);
    if (!reply.account.empty()) account = find_account(reply.account);
  } else if (!account) {
    reply.error_code = "UNKNOWN_ACCOUNT";
    reply.decision.message = "No paper account " + c.account;
  } else if (account->archived) {
    reply.error_code = "ACCOUNT_ARCHIVED";
    reply.decision.message = "Unarchive this account before trading or changing its settings";
  } else if (!account->session || !account->failure.empty() || stopping_) {
    reply.error_code = "TRADING_UNAVAILABLE";
    reply.decision.message = account->failure.empty() ? "Trading is disabled or engine is stopping" : account->failure;
  } else {
    auto& session = *account->session;
    session.set_actor(c.actor);
    struct ActorReset {
      TradingSession& target;
      ~ActorReset() { target.set_actor("system"); }
    } actor_reset{session};
    const auto before = session.snapshot();
    // New orders need the underlying's feed to be current, as the ticket shows.
    const auto acceptance = [&](const std::string& underlying) {
      const auto view = trading_view(account->id);
      const auto time = view->market_times.find(underlying);
      return paper_acceptance(underlying, time == view->market_times.end() ? 0 : time->second,
                              driver_time, capabilities_.delay, session.config().limits.max_quote_age, breaker_.halts);
    };
    // A dry run's newer market: the listed contracts, their quotes and valuations now,
    // without the market transaction that would journal them. `vols` gains each one's
    // underlying's variance to the close, and every held underlying's.
    const auto preview_market = [&](const std::set<std::string>& symbols, std::map<std::string, double>& vols) {
      PreviewMarket market;
      for (const auto& symbol : symbols) {
        const auto id = instruments_.find(symbol);
        const auto* option = id == instruments_.end() ? nullptr : book_.option(id->second);
        if (!option) continue;
        market.contracts.push_back(option->contract);
        const auto& underlying = option->contract.underlying;
        const auto complete = snapshots_.find(underlying);
        const bool current = complete != snapshots_.end() && acceptance(underlying).ok();
        if (option->has_quote) market.quotes.push_back({symbol, observations_[symbol], current ? market_time_ : option->quote_ts,
            quote_price(option->bid), quote_price(option->ask), whole_size(option->bid_size), whole_size(option->ask_size),
            std::clamp<md::Timestamp>(option->quote_ts, 0, current ? market_time_ : option->quote_ts)});
        auto valuation = valuation_for(symbol, option->contract, metrics(underlying));
        if (current && valuation.time > 0) valuation.time = market_time_;
        market.valuations.push_back(valuation);
        if (const auto variance = close_variance(metrics(underlying))) vols[underlying] = *variance;
      }
      for (const auto& [underlying, bucket] : before->risk.underlyings)
        if (const auto variance = close_variance(metrics(underlying))) vols[underlying] = *variance;
      return market;
    };
    // A new order's gate: every contract it trades (each leg of a multi-leg order)
    // must be supported, and its underlying's feed current.
    const auto submission_gate = [&](const OrderRequest& order) {
      Decision rejection;
      for (const auto& symbol : chain_symbols(order)) {
        const md::OptionContract* contract = nullptr;
        const auto id = instruments_.find(symbol);
        if (id != instruments_.end()) {
          if (const auto* option = book_.option(id->second)) contract = &option->contract;
        }
        if (!contract) {
          const auto saved = session.contracts().find(symbol);
          if (saved != session.contracts().end()) contract = &saved->second;
        }
        if (!contract) continue;
        rejection = eligible(*contract);
        if (rejection.ok()) rejection = acceptance(contract->underlying);
        if (!rejection.ok()) break;
      }
      return rejection;
    };
    const auto change_gate = [&](const PreviewMarket& market) -> Decision {
      for (const auto& order : before->open_orders) {
        if (order.id != c.order_id || !c.change.quantity || *c.change.quantity <= order.request.quantity ||
            order.system || order.reduce_only || order.role != OrderRole::Normal) continue;
        auto request = order.request;
        request.quantity = *c.change.quantity - order.filled_quantity;
        return opening_gate(*account, request, market_time_, market);
      }
      return {};
    };
    try {
      session.set_opening_rule_inputs(opening_rule_inputs(*account, market_time_), options_.compact_opening_rule_inputs);
      CommandResult result;
      switch (c.kind) {
        case TradingCommand::Kind::Playbook: playbook_command(c, reply, driver_time); break;
        case TradingCommand::Kind::Preview:
        case TradingCommand::Kind::Submit: {
          auto rejection = submission_gate(c.order);
          if (c.kind == TradingCommand::Kind::Preview) {
            std::map<std::string, double> vols;
            const auto symbols = chain_symbols(c.order);
            const auto market = preview_market({symbols.begin(), symbols.end()}, vols);
            if (rejection.ok()) rejection = opening_gate(*account, c.order, market_time_, market);
            reply.preview = session.preview(c.order, market_time_, c.floor_share, rejection, vols, market);
          } else {
            // Retries keep the original response even if another account changed.
            auto gate = opening_gate(*account, c.order, market_time_);
            if (!gate.ok() && std::any_of(before->recent_orders.begin(), before->recent_orders.end(), [&](const auto& o) {
                  return o.id >= before->evaluation.first_order && o.request == c.order;
                })) gate = {};
            if (!gate.ok()) result.decision = gate;  // No order or rejection enters the account journal.
            else result = session.submit(c.order, market_time_, rejection);
          }
          break;
        }
        case TradingCommand::Kind::WhatIf: {
          // Each order takes a submission's gate, at the current quotes.
          std::vector<std::vector<Decision>> rejections;
          std::set<std::string> symbols;
          for (const auto& orders : c.candidates) {
            auto& gates = rejections.emplace_back();
            std::map<std::string, Quantity> preceding;
            for (const auto& order : orders) {
              auto gate = submission_gate(order);
              std::map<std::string, double> ignored;
              const auto names = order_symbols(order);
              const auto inputs = preview_market({names.begin(), names.end()}, ignored);
              if (gate.ok()) gate = opening_gate(*account, order, market_time_, inputs, preceding);
              if (gate.ok() && (session.config().rules.no_counter_positions || session.config().rules.max_volume_percent != 0))
                gate = project_contracts(order, preceding);
              gates.push_back(std::move(gate));
              for (const auto& symbol : order_symbols(order)) symbols.insert(symbol);
            }
          }
          std::map<std::string, double> vols;
          const auto market = preview_market(symbols, vols);
          reply.what_if = session.what_if(c.candidates, market_time_, rejections, vols, market);
          break;
        }
        case TradingCommand::Kind::PreviewChange: {
          // The change's own feed gate, as Modify takes it, at the current quotes.
          Decision rejection;
          std::set<std::string> symbols;
          for (const auto& order : before->open_orders)
            if (order.id == c.order_id) {
              for (const auto& symbol : order_symbols(order.request)) symbols.insert(symbol);
              const auto contract = session.contracts().find(order_symbols(order.request).front());
              if (contract != session.contracts().end()) rejection = acceptance(contract->second.underlying);
            }
          std::map<std::string, double> vols;
          const auto market = preview_market(symbols, vols);
          if (rejection.ok()) rejection = change_gate(market);
          reply.preview = session.preview_change(c.order_id, c.change, market_time_, c.floor_share, rejection, vols, market);
          // As for the change itself, an unknown or finished order is not found or in conflict.
          if (const auto code = reply.preview->decision.code; code == Reason::UNKNOWN_ORDER || code == Reason::ORDER_TERMINAL)
            result.decision = reply.preview->decision;
          break;
        }
        case TradingCommand::Kind::Modify: {
          // A change can trade at once, so it takes a new order's feed gate.
          Decision rejection;
          for (const auto& order : before->open_orders)
            if (order.id == c.order_id) {
              const auto contract = session.contracts().find(order_symbols(order.request).front());
              if (contract != session.contracts().end()) rejection = acceptance(contract->second.underlying);
            }
          const auto gate = change_gate({});
          if (!gate.ok()) result.decision = gate;
          else result = session.modify(c.order_id, c.change, market_time_, rejection);
          break;
        }
        case TradingCommand::Kind::CancelAll:
          result = !c.order_ids.empty() ? session.cancel_orders(c.order_ids, market_time_)
              : session.cancel_all(c.underlying.empty() ? std::nullopt : std::optional(c.underlying), market_time_);
          break;
        case TradingCommand::Kind::PreviewClose:
        case TradingCommand::Kind::ClosePositions: {
          // Each underlying in scope with options or delivered shares takes the feed gate.
          std::map<std::string, Decision> rejections;
          const auto gate = [&](const std::string& underlying) {
            if ((c.underlying.empty() || underlying == c.underlying) && !rejections.contains(underlying))
              if (auto rejection = acceptance(underlying); !rejection.ok()) rejections.emplace(underlying, std::move(rejection));
          };
          for (const auto& p : before->positions) gate(p.position.contract.underlying);
          for (const auto& stock : before->stocks) gate(stock.position.symbol);
          const auto scope = c.underlying.empty() ? std::nullopt : std::optional(c.underlying);
          if (c.kind == TradingCommand::Kind::PreviewClose) {
            // The dry run closes at the current quotes, as the flatten would.
            std::set<std::string> symbols;
            for (const auto& p : before->positions) symbols.insert(p.position.contract.osi_symbol());
            std::map<std::string, double> vols;
            const auto market = preview_market(symbols, vols);
            reply.flatten = session.preview_close_positions(scope, market_time_, rejections, vols, market, c.close_pricing);
            break;
          }
          result = session.close_positions(scope, market_time_, rejections, c.close_pricing);
          reply.kept_stocks = result.kept_stocks;
          reply.residuals = result.residuals;
          break;
        }
        case TradingCommand::Kind::Cancel: result = session.cancel(c.order_id, market_time_, c.cancel_reason); break;
        case TradingCommand::Kind::Guardrails:
        case TradingCommand::Kind::Limits:
          if (c.expected_revision != before->risk.limits_revision) {
            reply.error_code = "LIMITS_REVISION";
            reply.decision.message = "Limits changed; refetch the current revision";
          } else if (c.kind == TradingCommand::Kind::Guardrails) result = session.set_guardrails(c.guardrails, market_time_);
          else result = session.set_limits(c.limits, market_time_);
          break;
        case TradingCommand::Kind::Trip: result = session.trip_kill(c.reason, market_time_); break;
        case TradingCommand::Kind::Reset: result = session.reset_kill(c.reason, market_time_); break;
        case TradingCommand::Kind::Settle: {
          // AM imports work while waiting for a source; PM only without a recorded close.
          const auto it = session.contracts().find(c.symbol);
          const bool pm = it != session.contracts().end() && it->second.settlement != md::Settlement::AM;
          if (pm && session.closing_print(it->second.underlying, it->second.expiry))
            result.decision = {Reason::INVALID_SETTLEMENT, "PM settlement uses the recorded closing print", {}, {}, {}};
          else {
            const SettlementSource source{{"kind", pm ? "manual_pm_import" : "manual_am_import"}, {"symbol", c.symbol}};
            result = session.settle(c.symbol, c.settlement, market_time_, source);
          }
          break;
        }
        case TradingCommand::Kind::ResetAccount: {
          // Only a pass of the preset itself unlocks: its balance and rules, not its name.
          const auto* evaluation = c.required_pass.empty() ? nullptr : find_plan_named(c.required_pass);
          const auto original_rules = trading::original_program_rules(session.config().rules,
              before->evaluation.starting_balance, before->evaluation.size_scaling);
          const bool restarting = c.program_costs && c.initial_cash == session.config().initial_cash &&
              (trading::same_program_rules(c.rules, original_rules) ||
               (c.restore_scaled_rules && trading::same_program_rules(c.rules, session.config().rules)));
          if (!restarting && !c.required_pass.empty() && (before->evaluation.status != EvaluationStatus::Passed || !evaluation ||
                                           !follows_plan(*evaluation, session.config().initial_cash, original_rules)))
            result.decision = {Reason::PLAN_LOCKED, "Pass the " + c.required_pass + (c.program_costs ? " to start this step" : " evaluation to start this funded account"),
                               {}, {}, {}};
          else {
            auto rules = c.rules;
            if (restarting) {
              const auto& current = session.config().rules;
              if (!(c.program_cost_overrides & 1)) rules.evaluation_fee = current.evaluation_fee;
              if (!(c.program_cost_overrides & 2)) rules.reset_fee = current.reset_fee;
              if (!(c.program_cost_overrides & 4)) rules.activation_fee = current.activation_fee;
              if (!(c.program_cost_overrides & 8)) rules.max_resets = current.max_resets;
            }
            result = session.reset_account(c.initial_cash, rules, c.reason, market_time_, !restarting && !c.required_pass.empty(), c.program_costs, c.restore_scaled_rules);
          }
          break;
        }
        case TradingCommand::Kind::Payout: result = session.request_payout(c.amount, market_time_); break;
        case TradingCommand::Kind::DayNote:
          result = session.annotate_day(c.day, c.plan, c.review, market_time_);
          break;
        case TradingCommand::Kind::Group:
        case TradingCommand::Kind::Ungroup:
          result = session.group_trades(c.trades, c.kind == TradingCommand::Kind::Group, market_time_);
          break;
        case TradingCommand::Kind::Annotate:
          result = c.shares ? session.annotate_shares(c.trade, c.note, c.tags, market_time_)
                            : session.annotate(c.trade, c.note, c.tags, market_time_);
          break;
        case TradingCommand::Kind::Exercise: {
          const auto contract = session.contracts().find(c.symbol);
          const auto gate = contract == session.contracts().end() ? Decision{} : acceptance(contract->second.underlying);
          if (gate.ok()) result = session.exercise(c.symbol, c.quantity, market_time_);
          else result.decision = gate;
          break;
        }
        case TradingCommand::Kind::CloseStock: {
          // Close all the shares, or the given number, in the direction that reduces them.
          Quantity held = 0;
          for (const auto& stock : session.snapshot()->stocks) if (stock.position.symbol == c.symbol) held = stock.position.shares;
          const Quantity shares = c.quantity > 0 ? c.quantity : (held < 0 ? -held : held);
          const auto gate = acceptance(c.symbol);
          if (held == 0) result.decision = {Reason::INVALID_ORDER, "The account holds no " + c.symbol + " shares", {}, {}, {}};
          else if (!gate.ok()) result.decision = gate;
          else result = session.trade_stock(c.symbol, held < 0 ? shares : -shares, market_time_, {}, true);
          break;
        }
        case TradingCommand::Kind::TradeStock:
        case TradingCommand::Kind::PreviewStock: {
          auto gate = acceptance(c.symbol);
          Quantity held = 0;
          for (const auto& stock : before->stocks) if (stock.position.symbol == c.symbol) held = stock.position.shares;
          const bool valid_size = c.quantity != 0 && c.quantity >= -10'000'000 && c.quantity <= 10'000'000;
          const bool reduces = valid_size && held != 0 && (held > 0) != (c.quantity > 0) &&
              (held > 0 ? -c.quantity <= held : c.quantity <= -held);
          if (gate.ok() && valid_size && !reduces && session.config().rules.no_counter_positions) {
            // Just after recovery the session can have a fresh saved price before
            // ChainBook receives one. Use that same price, never skip the gate.
            const auto price = c.stock_price ? std::optional(c.stock_price->price)
                : session.preview_trade_stock(c.symbol, c.quantity, market_time_).price;
            if (price) gate = counter_position_gate(*account, {{c.symbol, static_cast<double>(c.quantity) * price->dollars()}}, market_time_);
          }
          if (c.kind == TradingCommand::Kind::PreviewStock)
            reply.stock_preview = session.preview_trade_stock(c.symbol, c.quantity, market_time_, c.stock_price, gate);
          else if (!gate.ok()) result.decision = gate;
          else result = session.trade_stock(c.symbol, c.quantity, market_time_, c.stock_price);
          break;
        }
        case TradingCommand::Kind::Abandon: result = session.abandon(c.symbol, market_time_); break;
        case TradingCommand::Kind::ExerciseInstruction:
          result = session.instruct_exercise(c.symbol, c.do_not_exercise, market_time_);
          break;
        case TradingCommand::Kind::CreateAlert: result = session.create_alert(c.alert, market_time_); break;
        case TradingCommand::Kind::DeleteAlert: result = session.delete_alert(c.alert_id, market_time_); break;
        case TradingCommand::Kind::CreateSandbox:
        case TradingCommand::Kind::CreateAccount: break;  // handled above
        case TradingCommand::Kind::UpdateAccount:
        case TradingCommand::Kind::DeleteAccount: break;
      }
      if (reply.error_code.empty()) reply.decision = result.decision;
      reply.order_id = result.order_id;
      reply.alert_id = result.alert_id;
      reply.replayed = result.replayed;
      if (!dry_run(c.kind)) publish_trading();
      const auto& orders = session.snapshot()->recent_orders;
      for (const auto& order : before->open_orders) {
        if (orders.at(static_cast<std::size_t>(order.id - 1)).status == OrderStatus::Cancelled)
          reply.cancelled_orders.push_back(order.id);
      }
      for (auto i = before->recent_orders.size(); i < orders.size(); ++i) reply.created_orders.push_back(orders[i].id);
      const auto& stock_fills = session.snapshot()->stock_fills;
      for (auto i = before->stock_fills.size(); i < stock_fills.size(); ++i) reply.stock_fills.push_back(stock_fills[i].id);
    } catch (const TradingError& error) {
      reply.decision = {error.code(), error.what(), {}, {}, {}};
      if (error.code() == Reason::JOURNAL_IO || error.code() == Reason::JOURNAL_CORRUPT ||
          error.code() == Reason::JOURNAL_LOCKED) {
        fail_trading(*account, std::string(to_string(error.code())) + ": " + error.what());
        reply.error_code = "TRADING_UNAVAILABLE";
      }
    } catch (const std::exception& error) {
      fail_trading(*account, std::string("TRADING_UNAVAILABLE: ") + error.what());
      reply.error_code = "TRADING_UNAVAILABLE";
      reply.decision.message = account->failure;
    }
  }
  if (account) {
    reply.account = account->id;
    reply.view = trading_view(account->id);
  }
  if (recorded_input(c)) {
    if (!inputs_first()) record_command(c, driver_time);
    publish_trading();
    if (account) {
      reply.view = trading_view(account->id);
      if (!account->failure.empty()) {
        reply.error_code = "TRADING_UNAVAILABLE";
        reply.decision.message = account->failure;
      }
    }
  }
  // A disconnected consumer cannot take down the single owner of the ledger.
  try { pending.completion(std::move(reply)); } catch (...) {}
}

Desk::Desk(std::string provider, md::Capabilities capabilities, md::Subscription subscription, Options options)
    : options_(std::move(options)), provider_(std::move(provider)), subscription_(std::move(subscription)),
      capabilities_(capabilities), dividends_(options_.dividends) {
  if (options_.resume) {
    const auto inputs = run_inputs(*options_.resume);
    if (!inputs.empty()) {
      const auto start = nlohmann::json::parse(inputs.front());
      options_.exact_margin = start.value("exact_margin", false);
      options_.opening_rule_checks = start.value("opening_rule_checks", false);
      options_.compact_opening_rule_inputs = start.value("compact_opening_rule_inputs", false);
    }
  }
  if (!options_.run_input.empty() && options_.analytics.discount_curve)
    throw std::invalid_argument("Reproducible runs require curves inferred from their input, not an external discount curve");
  breaker_.symbol = std::find(subscription_.underlyings.begin(), subscription_.underlyings.end(), "SPX") !=
      subscription_.underlyings.end() ? "SPX" : "SPY";
  trading_status_.fee_per_contract = options_.paper.fee_per_contract;
  trading_status_.initial_cash = options_.paper.initial_cash;
  if (!options_.run_input.empty()) run_ = run_identity(options_.run_input, options_.run_id);
  scenario_source_ = run_ && !run_->scenario.empty();
}
void Desk::set_dividends(std::vector<trading::Dividend> dividends) {
  dividends_ = std::move(dividends);
  ++dividends_version_;
  if (!options_.run_input.empty() && !accounts_.empty()) {
    record_input(nlohmann::json{{"kind", "dividends"}, {"dividends", dividends_}}.dump());
    publish_trading();
  }
}
std::shared_ptr<const analytics::UnderlyingMetrics> Desk::metrics(const std::string& symbol) const {
  const auto it = metrics_.find(symbol);
  return it == metrics_.end() ? nullptr : it->second;
}
std::shared_ptr<const TradingView> Desk::trading_view(std::string_view account) const {
  const auto it = trading_views_.find(account.empty() ? kMainAccount : account);
  return it == trading_views_.end() ? nullptr : it->second;
}
bool Desk::active() const {
  return std::any_of(accounts_.begin(), accounts_.end(), [](const PaperAccount& account) {
    return !account.archived && account.session && (!account.session->snapshot()->positions.empty() || !account.session->snapshot()->open_orders.empty());
  });
}
void Desk::observe(const md::Event& event) {
  book_.apply(event);
  if (const auto* quote = std::get_if<md::UnderlyingQuote>(&event); quote && options_.candles) {
    const auto& underlying = book_.underlyings().at(quote->symbol);
    options_.candles->sample(quote->symbol, underlying.spot_ts, underlying.spot);
  }
  observe_trading(event);
}
void Desk::replay_source(const std::string& input, const md::RecordingHeader& header) {
  if (!options_.replay) throw std::invalid_argument("Source changes require a replay Desk");
  // Instrument IDs are local to each recording. Keep account marks and candles,
  // but never let an old chain row refer to a new recording's reused ID.
  book_ = analytics::ChainBook{};
  metrics_.clear();
  snapshots_.clear();
  vouched_at_.clear();
  analysed_versions_.clear();
  discount_curves_.clear();
  discount_curve_.reset();
  instruments_.clear();
  capabilities_ = header.capabilities;
  provider_ = "replay (" + header.provider + ")";
  scenario_source_ = nlohmann::json::parse(input).at("kind") == "scenario";
  record_input(nlohmann::json{{"kind", "source"}, {"input", nlohmann::json::parse(input)}}.dump());
  publish_trading();
}
void Desk::replay_batch(const std::vector<md::Event>& batch, md::Timestamp driver_time, md::Timestamp boundary_time) {
  if (!resume_inputs_.empty()) {
    const auto input = nlohmann::json::parse(resume_inputs_.front());
    if (input.at("kind") != "boundary" || input.at("events").get<std::size_t>() != batch.size() ||
        input.at("driver_time").get<md::Timestamp>() != driver_time)
      resume_failed("the recording's batch at " + md::format_timestamp(driver_time) + " is not the one the run recorded");
    else resume_inputs_.pop_front();
  }
  for (const auto& event : batch) observe(event);
  market_time_ = std::max(market_time_, boundary_time);
  refresh_analytics();
  std::deque<PendingCommand> commands;
  update_trading(batch, commands, driver_time);
  record_input(nlohmann::json{{"kind", "boundary"}, {"events", batch.size()}, {"time", market_time_},
      {"driver_time", driver_time}}.dump());
  replay_recorded();
  publish_trading();
}
void Desk::replay_recorded() {
  while (!resume_inputs_.empty()) {
    const auto input = nlohmann::json::parse(resume_inputs_.front());
    const auto kind = input.at("kind").get<std::string>();
    if (kind == "boundary") return;
    resume_inputs_.pop_front();
    if (kind == "command") {
      command(input.at("command").get<TradingCommand>(), [](TradingReply) {}, input.at("time").get<md::Timestamp>(),
              input.at("driver_time").get<md::Timestamp>());
    } else if (kind == "dividends") {
      set_dividends(input.at("dividends").get<std::vector<trading::Dividend>>());
    } else {
      resume_failed("its " + kind + " input cannot be replayed");
    }
  }
}
void Desk::resume_failed(const std::string& reason) {
  resume_inputs_.clear();
  for (auto& account : accounts_)
    if (account.session && account.failure.empty()) fail_trading(account, "JOURNAL_CORRUPT: The run cannot resume: " + reason);
}
void Desk::apply_analytics(std::shared_ptr<const analytics::UnderlyingMetrics> result, md::Timestamp time) {
  if (!result || result->as_of != time) throw std::invalid_argument("Analytics time does not match result");
  const auto symbol = result->symbol;
  metrics_[symbol] = std::move(result);
  ++analytics_generation_;
}
void Desk::record_input(const std::string& input, const std::string& actor, bool at_account_time) {
  if (options_.run_input.empty()) return;
  for (auto& account : accounts_) {
    if (!account.session || !account.failure.empty()) continue;
    account.session->set_actor(actor);
    // At the account's own time the record moves no clock, so no time rule runs
    // before the transactions that follow it.
    try { account.session->record_input(input, at_account_time ? account.session->snapshot()->time : market_time_); }
    catch (const std::exception& error) { account.failure = error.what(); }
    account.session->set_actor("system");
  }
}
bool Desk::recorded_input(const TradingCommand& command) const {
  return !playbook_running_ && !options_.run_input.empty() && !dry_run(command.kind);
}
void Desk::record_command(const TradingCommand& c, md::Timestamp driver_time) {
  record_input(nlohmann::json{{"kind", "command"}, {"command", c}, {"time", market_time_}, {"driver_time", driver_time}}.dump(),
               c.actor, inputs_first());
}
void Desk::price_stock_command(TradingCommand& c, md::Timestamp driver_time) {
  if ((c.kind != TradingCommand::Kind::TradeStock && c.kind != TradingCommand::Kind::PreviewStock) || c.stock_price) return;
  const auto book = book_.underlyings().find(c.symbol);
  if (book == book_.underlyings().end() || book->second.spot_ts <= 0 || book->second.spot_ts > market_time_) return;
  const auto* account = find_account(c.account);
  if (!account || !account->session) return;
  auto time = book->second.spot_ts;
  const auto complete = snapshots_.find(c.symbol);
  if (complete != snapshots_.end() && paper_acceptance(c.symbol, complete->second, driver_time, capabilities_.delay,
      account->session->config().limits.max_quote_age, breaker_.halts).ok()) time = market_time_;
  if (const auto price = quote_price(book->second.spot)) c.stock_price = StockPrice{c.symbol, time, *price};
}
void Desk::command(TradingCommand command, TradingCompletion completion, md::Timestamp time, md::Timestamp driver_time) {
  market_time_ = std::max(market_time_, time);
  price_stock_command(command, driver_time);
  if (!command.rules.hold_restrictions.empty() && !command.rules.hold_calendar)
    command.rules.hold_calendar = md::scheduled_days();
  std::deque<PendingCommand> commands{{0, std::move(command), std::move(completion)}};
  // Driver 4: the input comes before the command's quotes and its own transaction.
  const bool recorded = inputs_first() && recorded_input(commands.front().command);
  if (recorded) record_command(commands.front().command, driver_time);
  if (!dry_run(commands.front().command.kind)) update_trading({}, commands, driver_time);
  apply_command(commands.front(), market_time_, driver_time, recorded);
}
bool Desk::refresh_analytics() {
  bool recomputed = false;
  const auto& dividends = dividends_;
  const auto dividends_version = dividends_version_;
  const bool dividends_changed = dividends_version != analysed_dividends_version_;
  const auto previous_curve = discount_curve_;
  std::vector<std::string> european_updates;
  auto has_style = [](const analytics::UnderlyingBook& book, pricing::ExerciseStyle style) {
    return std::any_of(book.expiries.begin(), book.expiries.end(),
                       [style](const auto& entry) { return entry.first.second == style; });
  };
  // An underlying is analysed from its first price on, at its market-data clock, which
  // delayed and replayed feeds keep: before any price there is no market time to value
  // it at, and the wall clock would run ahead of such a feed.
  auto analyze = [&](const std::string& symbol, const analytics::UnderlyingBook& book) {
    const md::Timestamp as_of = options_.replay && snapshots_.empty() ? market_time_ : book.data_time;
    if (options_.replay) {
      const auto previous = metrics(symbol);
      if (previous && previous->as_of / md::kNanosPerSecond == as_of / md::kNanosPerSecond) return previous;
    }
    auto options = options_.analytics;
    options.dividends.clear();
    for (const auto& d : dividends)
      if (d.symbol == symbol) options.dividends.push_back({d.ex_date, d.per_share.dollars()});
    if (discount_curve_) options.discount_curve = discount_curve_;
    auto result = std::make_shared<const analytics::UnderlyingMetrics>(
        analytics::analyze(book, book_, as_of, options));
    recomputed = true;
    analysed_versions_[symbol] = book.version;
    // Quoted prints are charted as they arrive; a spot inferred from parity is charted
    // at the option data's market time.
    if (options_.candles && result->spot_source != "quote")
      options_.candles->sample(symbol, result->as_of, result->spot);
    apply_analytics(result, as_of);
    return result;
  };
  const auto ready = [&](const std::string& symbol, const analytics::UnderlyingBook& underlying) {
    if (!options_.replay || snapshots_.empty()) return true;
    const auto complete = snapshots_.find(symbol);
    return complete != snapshots_.end() && complete->second >= underlying.data_time;
  };
  // Build all European curves first, irrespective of symbol/map ordering. Mixed
  // OEX/XEO books also enter this phase; only their European slices form a curve.
  for (const auto& [symbol, book] : book_.underlyings()) {
    if (!ready(symbol, book) || book.data_time <= 0 || !has_style(book, pricing::ExerciseStyle::European) ||
        book.version == analysed_versions_[symbol])
      continue;
    auto curve = analytics::make_discount_curve(*analyze(symbol, book));
    european_updates.push_back(symbol);
    if (curve) {
      discount_curves_[symbol] = curve;
      discount_curve_ = std::move(curve);  // latest, unless SPX is available below
    } else {
      discount_curves_.erase(symbol);
      if (discount_curve_ && discount_curve_->symbol() == symbol) discount_curve_.reset();
    }
  }
  if (const auto spx = discount_curves_.find("SPX"); spx != discount_curves_.end())
    discount_curve_ = spx->second;
  else if (!discount_curve_ && !discount_curves_.empty())
    discount_curve_ = discount_curves_.begin()->second;
  const bool curve_changed = previous_curve != discount_curve_;
  // Curve or cash-schedule updates invalidate American results even without quotes.
  for (const auto& [symbol, book] : book_.underlyings()) {
    if (!ready(symbol, book) || book.data_time <= 0 || book.expiries.empty() || !has_style(book, pricing::ExerciseStyle::American))
      continue;
    const bool mixed_updated = std::find(european_updates.begin(), european_updates.end(),
                                         symbol) != european_updates.end();
    if (book.version != analysed_versions_[symbol] || curve_changed || mixed_updated || dividends_changed)
      analyze(symbol, book);
  }
  analysed_dividends_version_ = dividends_version;
  return recomputed;
}

}  // namespace openport::server
