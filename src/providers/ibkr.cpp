#include "openport/providers/ibkr.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

#include "broker_json.hpp"

namespace openport::providers {
namespace {
using namespace broker;
// Last, bid, ask size, ask, bid size, delta, gamma, option open interest, the day's
// volume, availability.
constexpr std::string_view kFields = "31,84,85,86,88,7308,7309,7638,7762,6509";
constexpr std::size_t kSnapshotBatch = 50;
constexpr const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

std::int64_t conid(const Json& value) {
  if (value.is_number_integer()) return value.get<std::int64_t>();
  const auto text = str(value);
  std::int64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  return error == std::errc{} && end == text.data() + text.size() && result > 0 ? result : 0;
}

double value(const Json& item, std::string_view key, bool price) {
  const auto& raw = field(item, key);
  if (raw.is_number()) return number(raw);
  return raw.is_string() ? parse_ibkr_number(raw.get<std::string>(), price) : missing;
}

/// Field 7762, the day's volume at full precision, as a whole count, or -1. A rounded
/// display value such as 87's "1.2K" is not a count.
double day_volume(const Json& item) {
  const auto& raw = field(item, "7762");
  if (raw.is_string() && raw.get<std::string>().find_first_of("KM") != std::string::npos) return -1;
  const double count = value(item, "7762", false);
  return std::isfinite(count) && count >= 0 && count == std::floor(count) ? count : -1;
}

/// "OCT26" -> its year and month, to order months and drop past ones.
std::optional<std::pair<int, int>> month_of(std::string_view month) {
  if (month.size() != 5) return std::nullopt;
  for (int i = 0; i < 12; ++i) {
    if (month.substr(0, 3) != kMonths[i]) continue;
    int year = 0;
    const auto [end, error] = std::from_chars(month.data() + 3, month.data() + 5, year);
    if (error != std::errc{} || end != month.data() + 5) return std::nullopt;
    return std::pair{2000 + year, i + 1};
  }
  return std::nullopt;
}

std::string strike_text(double strike) {
  char text[32];
  const auto [end, error] = std::to_chars(text, text + sizeof text, strike);
  if (error != std::errc{}) throw std::runtime_error("IBKR: invalid strike");
  return std::string(text, end);
}

bool loopback(const std::string& url) {
  const auto parsed = net::parse_url(url);
  return parsed && (parsed->host == "localhost" || parsed->host == "127.0.0.1" || parsed->host == "[::1]" ||
                    parsed->host == "::1");
}

bool allowed_path(std::string_view path, bool post) {
  // Read-only market data: lookups, snapshots and the session calls they need.
  // No order, portfolio or account-detail endpoint, and no other POST.
  const auto endpoint = path.substr(0, path.find('?'));
  if (post) return endpoint == "/tickle";
  return endpoint == "/iserver/accounts" || endpoint == "/iserver/secdef/search" ||
         endpoint == "/iserver/secdef/strikes" || endpoint == "/iserver/secdef/info" ||
         endpoint == "/iserver/marketdata/snapshot" || endpoint == "/iserver/marketdata/unsubscribeall";
}
}  // namespace

double parse_ibkr_number(std::string_view text, bool price) {
  // A closing or halted price is not a current one.
  if (price && !text.empty() && (text.front() == 'C' || text.front() == 'H')) return 0;
  std::string digits;
  double scale = 1;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == ',') continue;
    if (i + 1 == text.size() && (c == 'K' || c == 'M')) scale = c == 'K' ? 1e3 : 1e6;
    else digits += c;
  }
  // strtod, not from_chars: older libc++ (macOS 15) has no floating-point from_chars.
  // Plain decimals only, so no whitespace, sign prefix, hex, inf or nan gets through.
  if (digits.empty() || digits.find_first_not_of("0123456789.-eE") != std::string::npos) return missing;
  char* end = nullptr;
  const double result = std::strtod(digits.c_str(), &end);
  if (end != digits.c_str() + digits.size() || !std::isfinite(result)) return missing;
  return result * scale;
}

IbkrUnderlying parse_ibkr_search(std::string_view json, std::string_view symbol) {
  // The search answers with a top-level array; wrap it so parse() still applies.
  const auto doc = parse("{\"items\":" + std::string(json) + "}");
  for (const auto& item : list(field(doc, "items"))) {
    if (str(field(item, "symbol")) != symbol) continue;
    IbkrUnderlying result;
    result.conid = conid(field(item, "conid"));
    for (const auto& section : list(field(item, "sections"))) {
      if (str(field(section, "secType")) != "OPT") continue;
      const auto months = str(field(section, "months"));
      std::size_t begin = 0;
      while (begin < months.size()) {
        const auto end = std::min(months.find(';', begin), months.size());
        if (const auto month = months.substr(begin, end - begin); month_of(month)) result.months.push_back(month);
        begin = end + 1;
      }
    }
    std::sort(result.months.begin(), result.months.end(), [](const auto& a, const auto& b) { return month_of(a) < month_of(b); });
    if (result.conid > 0 && !result.months.empty()) return result;
  }
  throw std::runtime_error("IBKR: no optionable contract for the symbol");
}

std::vector<double> parse_ibkr_strikes(std::string_view json) {
  const auto doc = parse(json);
  if (!doc.contains("call") && !doc.contains("put")) throw std::runtime_error("IBKR: invalid strikes");
  std::set<double> strikes;
  for (const auto* side : {"call", "put"})
    for (const auto& strike : list(field(doc, side)))
      if (const double n = number(strike); n > 0) strikes.insert(n);
  return {strikes.begin(), strikes.end()};
}

std::vector<IbkrContract> parse_ibkr_info(std::string_view json, std::string_view underlying) {
  const auto doc = parse("{\"items\":" + std::string(json) + "}");
  std::vector<IbkrContract> result;
  for (const auto& item : list(field(doc, "items"))) {
    const auto maturity = str(field(item, "maturityDate"));
    const auto right = str(field(item, "right"));
    const auto root = str(field(item, "tradingClass"));
    const double strike = number(field(item, "strike"));
    const auto id = conid(field(item, "conid"));
    if (str(field(item, "secType")) != "OPT" || maturity.size() != 8 || (right != "C" && right != "P") ||
        !(strike > 0) || strike >= 100'000 || id <= 0)
      throw std::runtime_error("IBKR: invalid option definition");
    // OSI from the trading class, maturity, right and strike in thousandths.
    char osi[40];
    std::snprintf(osi, sizeof osi, "%s%s%s%08lld", root.c_str(), maturity.substr(2).c_str(), right.c_str(),
                  static_cast<long long>(std::llround(strike * 1000)));
    auto contract = md::parse_osi(osi);
    if (!contract || contract->underlying != underlying || contract->strike != strike)
      throw std::runtime_error("IBKR: option does not match the underlying");
    const double multiplier = number(field(item, "multiplier"), 100);
    if (!(multiplier > 0)) throw std::runtime_error("IBKR: invalid contract size");
    contract->multiplier = multiplier;
    contract->standard = contract->standard && multiplier == 100 && str(field(item, "currency")) == "USD";
    result.push_back({id, *contract});
  }
  return result;
}

std::vector<IbkrQuote> parse_ibkr_snapshot(std::string_view json) {
  const auto doc = parse("{\"items\":" + std::string(json) + "}");
  std::vector<IbkrQuote> result;
  for (const auto& item : list(field(doc, "items"))) {
    IbkrQuote quote;
    quote.conid = conid(field(item, "conid"));
    if (quote.conid <= 0) throw std::runtime_error("IBKR: invalid snapshot row");
    for (const auto* key : {"31", "84", "85", "86", "88", "6509"}) quote.fields = quote.fields || item.contains(key);
    quote.ts = milliseconds(field(item, "_updated"));
    const auto positive = [](double n) { return std::isfinite(n) ? std::max(0.0, n) : 0.0; };
    quote.last = positive(value(item, "31", true));
    quote.bid = positive(value(item, "84", true));
    quote.ask = positive(value(item, "86", true));
    quote.bid_size = positive(value(item, "88", false));
    quote.ask_size = positive(value(item, "85", false));
    const double oi = value(item, "7638", false);
    quote.open_interest = std::isfinite(oi) && oi >= 0 ? oi : -1;
    quote.delta = value(item, "7308", false);
    quote.gamma = value(item, "7309", false);
    const auto availability = str(field(item, "6509"));
    quote.availability = availability.empty() ? 0 : availability.front();
    // Only a real-time row's count is current at _updated: a delayed row's arrives
    // 15-20 minutes late and a frozen row's is the last close's. A positive count
    // also needs a current trade beside it; beside a previous close it may be an
    // earlier session's, and OpenPort never stamps that with today's clock.
    const double volume = day_volume(item);
    if (quote.availability == 'R' && volume >= 0 && (volume == 0 || quote.last > 0))
      quote.volume = volume;
    if (quote.fields && quote.ts == 0 && (quote.bid > 0 || quote.ask > 0 || quote.last > 0))
      throw std::runtime_error("IBKR: snapshot time missing or units unconfirmed");
    result.push_back(quote);
  }
  return result;
}

IbkrSession parse_ibkr_tickle(std::string_view json) {
  const auto doc = parse(json);
  const auto& status = field(field(doc, "iserver"), "authStatus");
  return {field(status, "authenticated") == Json(true), field(status, "connected") == Json(true),
          field(status, "competing") == Json(true)};
}

IbkrProvider::IbkrProvider(Options options)
    : PollingProvider(options.poll_seconds), options_(std::move(options)),
      budget_(options_.requests_per_second, md::kNanosPerSecond) {
  while (options_.base_url.ends_with('/')) options_.base_url.pop_back();
  if (!loopback(options_.base_url))
    throw std::invalid_argument("ibkr: base_url must be the local Client Portal gateway (localhost)");
  if (options_.max_lines < 2) throw std::invalid_argument("ibkr: max_lines must be at least 2");
  if (options_.requests_per_second < 1 || options_.requests_per_second > 10)
    throw std::invalid_argument("ibkr: requests_per_second must be in [1, 10]");
  if (options_.poll_seconds.count() < 1) throw std::invalid_argument("ibkr: poll interval must be positive");
}

md::Capabilities IbkrProvider::capabilities() const noexcept {
  return {.realtime = false, .realtime_plan_dependent = true, .poll_interval = options_.poll_seconds,
          .open_interest = true, .vendor_greeks = true};
}

std::string IbkrProvider::request(net::HttpClient& http, const std::string& path, bool post) {
  if (!allowed_path(path, post)) throw std::logic_error("ibkr: forbidden endpoint");
  for (auto delay = budget_.wait(options_.pacing_clock()); delay.count() > 0;
       delay = budget_.wait(options_.pacing_clock())) {
    if (!sleep(delay)) check_cancelled();
  }
  check_cancelled();
  budget_.sent(options_.pacing_clock());
  // The gateway refuses requests without a User-Agent.
  const net::Headers headers{{"User-Agent", std::string(net::user_agent())}, {"Accept", "application/json"}};
  const auto url = options_.base_url + path;
  const auto result = post ? http.post(url, "", headers, std::chrono::seconds(15), cancellation())
                           : http.get_direct(url, headers, std::chrono::seconds(15), cancellation());
  if (result.status == 429) {
    // Pacing violations can also put the address in a penalty box for minutes.
    budget_.throttled(options_.pacing_clock(), std::chrono::seconds(60));
    throw std::runtime_error("IBKR: pacing limit exceeded; backing off");
  }
  if (result.status == 401) {
    // After the operator logs in again, keep the session and list accounts again.
    tickled_ = 0;
    accounts_read_ = false;
    throw std::runtime_error("IBKR: gateway session is not authenticated; log in through the gateway");
  }
  if (result.status != 200) throw std::runtime_error("IBKR: HTTP " + std::to_string(result.status));
  budget_.succeeded();
  return result.body;
}

void IbkrProvider::keepalive(net::HttpClient& http) {
  // The gateway drops a session after a few idle minutes; /tickle keeps it.
  if (tickled_ == 0 || options_.clock() - tickled_ >= md::kNanosPerMinute) {
    const auto session = parse_ibkr_tickle(request(http, "/tickle", true));
    if (session.competing) throw std::runtime_error("IBKR: another session is using this username");
    if (!session.authenticated || !session.connected) {
      accounts_read_ = false;
      throw std::runtime_error("IBKR: gateway session is not authenticated; log in through the gateway");
    }
    tickled_ = options_.clock();
  }
  if (!accounts_read_) {
    // Required once before market-data snapshots; the response is not kept.
    (void)request(http, "/iserver/accounts");
    accounts_read_ = true;
  }
}

void IbkrProvider::look_up(net::HttpClient& http, Chain& chain, const std::string& underlying,
                           const std::string& month, double strike, char right) {
  // Contract ids are stable, so each (month, strike, right) is looked up once a day.
  if (chain.looked_up.contains({month, strike, right})) return;
  const auto contracts = parse_ibkr_info(
      request(http, "/iserver/secdef/info?conid=" + std::to_string(chain.underlying.conid) + "&sectype=OPT&month=" +
                        month + "&exchange=SMART&strike=" + strike_text(strike) + "&right=" + right),
      underlying);
  for (const auto& found : contracts) chain.contracts.insert_or_assign(found.conid, found.contract);
  chain.looked_up.insert({month, strike, right});
}

std::vector<IbkrQuote> IbkrProvider::snapshot(net::HttpClient& http, const std::vector<std::int64_t>& conids) {
  std::vector<IbkrQuote> result;
  for (std::size_t begin = 0; begin < conids.size(); begin += kSnapshotBatch) {
    std::string list;
    for (std::size_t i = begin; i < std::min(conids.size(), begin + kSnapshotBatch); ++i)
      list += (list.empty() ? "" : ",") + std::to_string(conids[i]);
    auto rows = parse_ibkr_snapshot(request(http, "/iserver/marketdata/snapshot?conids=" + list + "&fields=" + std::string(kFields)));
    std::move(rows.begin(), rows.end(), std::back_inserter(result));
  }
  return result;
}

std::string IbkrProvider::poll(net::HttpClient& http, const std::string& underlying,
                               const md::Subscription& sub, md::EventSink& sink) {
  keepalive(http);
  const auto today = md::new_york_time(options_.clock()).date;
  auto& chain = chains_[underlying];
  if (chain.underlying.conid == 0 || chain.mapped_on != today) {
    // A new date: release yesterday's lines once, then map the chain again.
    // Contract ids are stable; expired ones go.
    if (chain.underlying.conid != 0 && unsubscribed_on_ != today) {
      (void)request(http, "/iserver/marketdata/unsubscribeall");
      unsubscribed_on_ = today;
    }
    chain.underlying = parse_ibkr_search(request(http, "/iserver/secdef/search?symbol=" + symbol_path(underlying) +
                                                       "&secType=" + (md::is_index_underlying(underlying) ? "IND" : "STK")),
                                         underlying);
    chain.strikes.clear();
    chain.looked_up.clear();
    std::erase_if(chain.contracts, [&](const auto& entry) { return entry.second.expiry < today; });
    std::erase_if(chain.defined, [&](auto id) { return !chain.contracts.contains(id); });
    chain.mapped_on = today;
  }
  md::UnderlyingQuote spot{underlying, 0, 0, 0, 0};
  char underlying_availability = 0;
  for (const auto& row : snapshot(http, {chain.underlying.conid})) {
    if (row.conid != chain.underlying.conid || !row.fields || row.ts == 0) continue;
    underlying_availability = row.availability;
    if (row.last > 0) spot = {underlying, row.ts, 0, 0, row.last};
    else if (row.bid > 0 || row.ask > 0) spot = {underlying, row.ts, row.bid, row.ask, 0};
  }
  const double price = spot.last > 0 ? spot.last : (spot.bid > 0 && spot.ask > 0 ? (spot.bid + spot.ask) / 2 : 0);
  if (price <= 0) throw std::runtime_error("no underlying price yet; the gateway is warming up its market-data line");

  // Each option month's expiries are found from its strike nearest spot; months
  // are read in order until the nearest expiries are known.
  const auto low = price * (1 - sub.strike_window), high = price * (1 + sub.strike_window);
  const auto nearest = [&] {
    std::set<md::Date> all, selected;
    for (const auto& [id, contract] : chain.contracts)
      if (contract.expiry >= today) all.insert(contract.expiry);
    for (const auto& expiry : all) {
      if (selected.size() >= static_cast<std::size_t>(sub.max_expiries)) break;
      selected.insert(expiry);
    }
    return selected;
  };
  std::vector<std::string> months;
  for (const auto& month : chain.underlying.months) {
    const auto year_month = month_of(month);
    if (!year_month || *year_month < std::pair{today.year, today.month}) continue;
    if (const auto selected = nearest(); selected.size() >= static_cast<std::size_t>(sub.max_expiries) &&
        std::pair{selected.rbegin()->year, selected.rbegin()->month} < *year_month)
      break;
    if (!chain.strikes.contains(month))
      chain.strikes[month] = parse_ibkr_strikes(request(http, "/iserver/secdef/strikes?conid=" +
          std::to_string(chain.underlying.conid) + "&sectype=OPT&month=" + month + "&exchange=SMART"));
    const auto& strikes = chain.strikes.at(month);
    if (strikes.empty()) continue;
    const auto atm = *std::min_element(strikes.begin(), strikes.end(), [&](double a, double b) {
      return std::abs(a - price) < std::abs(b - price);
    });
    look_up(http, chain, underlying, month, atm, 'C');
    months.push_back(month);
  }
  const auto selected = nearest();
  for (const auto& month : months) {
    const auto year_month = month_of(month);
    if (std::none_of(selected.begin(), selected.end(), [&](const md::Date& d) { return std::pair{d.year, d.month} == *year_month; }))
      continue;
    for (const double strike : chain.strikes.at(month))
      if (strike >= low - 1e-9 && strike <= high + 1e-9)
        for (const char right : {'C', 'P'}) look_up(http, chain, underlying, month, strike, right);
  }

  const ChainFilter filter(sub, today, price, selected);
  std::vector<std::pair<std::string, std::int64_t>> wanted;  // by OSI, for a stable order
  for (const auto& [id, contract] : chain.contracts)
    if (contract.expiry >= today && (chain.defined.contains(id) || filter.admits(contract)))
      wanted.emplace_back(contract.osi_symbol(), id);
  std::sort(wanted.begin(), wanted.end());
  // Every snapshot contract holds one of the account's market-data lines.
  lines_[underlying] = 1 + wanted.size();
  std::size_t total = 0;
  for (const auto& [name, count] : lines_) total += count;
  if (total > static_cast<std::size_t>(options_.max_lines)) {
    const auto needed = total;
    lines_[underlying] = 1;
    throw std::runtime_error("needs " + std::to_string(needed) + " market-data lines; max_lines is " +
                             std::to_string(options_.max_lines) + "; narrow --expiries or --window");
  }
  std::vector<std::int64_t> conids;
  for (const auto& [osi, id] : wanted) conids.push_back(id);
  std::map<std::int64_t, IbkrQuote> rows;
  for (auto& row : snapshot(http, conids)) rows.insert_or_assign(row.conid, row);
  md::Timestamp market_time = spot.ts;
  std::set<char> availability;
  if (underlying_availability) availability.insert(underlying_availability);
  for (const auto& [id, row] : rows) {
    if (!row.fields) continue;
    market_time = std::max(market_time, row.ts);
    if (row.availability) availability.insert(row.availability);
  }
  if (market_time <= 0) throw std::runtime_error("snapshot has no known market timestamp");
  check_cancelled();
  if (spot.ts > 0) sink.publish(spot);
  std::set<md::InstrumentId> seen;
  std::size_t warming = 0;
  for (const auto& [osi, id] : wanted) {
    const auto instrument = publisher_.define(osi, chain.contracts.at(id), sink);
    chain.defined.insert(id);
    const auto row = rows.find(id);
    // A contract still warming up has no quote yet: finish() leaves it unpriced.
    if (row == rows.end() || !row->second.fields || row->second.ts == 0) {
      ++warming;
      continue;
    }
    const auto& quote = row->second;
    seen.insert(instrument);
    publisher_.quote(instrument, quote.ts, quote.bid, quote.ask, quote.bid_size, quote.ask_size, sink);
    if (quote.open_interest >= 0) publisher_.open_interest(instrument, 0, quote.open_interest, sink);
    if (quote.volume >= 0) publisher_.volume(instrument, quote.ts, quote.volume, sink);
    if (std::isfinite(quote.delta) || std::isfinite(quote.gamma))
      publisher_.greeks(md::VendorGreeks{instrument, quote.ts, missing, quote.delta, quote.gamma, missing, missing, missing}, sink);
  }
  publisher_.finish(underlying, seen, market_time, sink);
  std::string summary;
  if (availability.contains('Z') || availability.contains('Y')) {
    state_ = md::FeedState::Stale;
    summary = "IBKR: frozen values from the last session";
  } else if (availability.contains('D')) {
    state_ = md::FeedState::Delayed;
    summary = "IBKR: delayed snapshots";
  } else if (availability.contains('R')) {
    state_ = md::FeedState::Live;
    summary = "IBKR: real-time snapshots";
  } else {
    state_ = md::FeedState::Stale;
    summary = "IBKR: market-data availability unreported";
  }
  if (warming > 0) summary += "; " + std::to_string(warming) + " contracts warming up";
  return summary;
}

}  // namespace openport::providers
