#include "openport/providers/history.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <variant>

#include "openport/providers/options.hpp"

namespace openport::providers {
namespace {
using nlohmann::json;
// Theta's per-response limits, after the HTTP client's own bounded download.
constexpr std::size_t kMaxRows = 250'000;
constexpr std::size_t kMaxBytes = 32 * 1024 * 1024;
// Rows one block request aims at (about 20 MB of quotes) and rows all streams may
// buffer together. strike=* quotes at interval=1s give at most one row per contract
// and second, so a chain's block length follows from its contract count.
constexpr std::size_t kBlockRows = 100'000;
constexpr std::size_t kBufferedRows = 1'000'000;
constexpr md::Timestamp kWindow = 15 * md::kNanosPerSecond;  // import_day's windows
constexpr md::Timestamp kLongestBlock = 60 * md::kNanosPerMinute;

std::size_t row_count(std::string_view body) {
  std::size_t count = 0;
  for (std::size_t start = 0; start < body.size();) {
    auto stop = body.find('\n', start);
    if (stop == std::string_view::npos) stop = body.size();
    if (body.substr(start, stop - start).find_first_not_of(" \r\t") != std::string_view::npos) ++count;
    start = stop + 1;
  }
  return count;
}
// Calls `use` with each NDJSON object, in response order.
template <typename Use>
void each_row(std::string_view body, const Use& use) {
  for (std::size_t start = 0; start < body.size();) {
    auto stop = body.find('\n', start);
    if (stop == std::string_view::npos) stop = body.size();
    const auto line = body.substr(start, stop - start);
    start = stop + 1;
    if (line.find_first_not_of(" \r\t") == std::string_view::npos) continue;
    auto row = json::parse(std::string(line), nullptr, false);
    if (row.is_discarded()) throw std::runtime_error("ThetaData history: invalid NDJSON response");
    if (!row.is_object()) throw std::runtime_error("ThetaData history: expected NDJSON objects");
    use(row);
  }
}
std::vector<json> rows(std::string_view body) {
  if (body.size() > kMaxBytes) throw std::runtime_error("ThetaData history: response exceeds 32 MiB");
  if (row_count(body) > kMaxRows) throw std::runtime_error("ThetaData history: response row limit exceeded");
  std::vector<json> result;
  each_row(body, [&](json& row) { result.push_back(std::move(row)); });
  return result;
}

double number(const json& row, const char* field) {
  if (!row.contains(field) || row[field].is_null()) return 0;
  if (!row[field].is_number()) throw std::runtime_error("ThetaData history: invalid numeric field");
  const auto value = row[field].get<double>();
  if (!std::isfinite(value) || value < 0) throw std::runtime_error("ThetaData history: invalid numeric value");
  return value;
}
md::Timestamp timestamp(const json& row) {
  const auto time = md::parse_datetime(row.value("timestamp", ""), md::Zone::NewYork);
  if (!time) throw std::runtime_error("ThetaData history: missing or invalid market timestamp");
  return *time;
}
md::Date expiration(const json& row) {
  auto text = row.at("expiration").get<std::string>();
  if (text.size() == 8) text = text.substr(0, 4) + "-" + text.substr(4, 2) + "-" + text.substr(6, 2);
  return import_date(text);
}
std::string clock_text(md::Timestamp time) {
  const auto seconds = md::new_york_time(time).seconds;
  char buffer[64];  // GCC sizes each %02d for any int
  const auto milliseconds = static_cast<int>((time % md::kNanosPerSecond) / 1'000'000);
  std::snprintf(buffer, sizeof buffer, "%02d:%02d:%02d.%03d", seconds / 3600, seconds / 60 % 60, seconds % 60, milliseconds);
  return buffer;
}

// A buffered row, kept in its event form until its window publishes it.
using Row = std::variant<md::UnderlyingQuote, md::OptionQuote, md::OptionTrade>;
md::Timestamp time_of(const Row& row) {
  return std::visit([](const auto& event) { return event.ts; }, row);
}

class ThetaHistory final : public HistorySource {
 public:
  ThetaHistory(net::HttpClient& http, HistoryWait wait) : http_(http), wait_(std::move(wait)) {}
  /// Its trade rows can name the exchange.
  bool trade_venues() const noexcept override { return true; }

  void prepare(const ImportDay& request, HistoryWindow day, md::EventSink& sink) override {
    day_ = day;
    for (const auto& symbol : request.subscription.underlyings) {
      std::map<std::string, std::set<md::Date>> root_expiries;
      std::set<md::Date> expiries;
      std::map<std::pair<std::string, md::Date>, std::set<double>> active_strikes;
      for (const auto& root : md::option_roots(symbol)) {
        std::set<md::Date> available;
        const auto first_date = md::days_since_epoch(md::new_york_time(day.begin).date);
        const auto last_date = md::days_since_epoch(md::new_york_time(day.end - 1).date);
        for (auto date_index = first_date; date_index <= last_date; ++date_index) {
          const auto date = md::date_from_days(date_index);
          const auto midnight = md::new_york_to_utc(date, 0, 0);
          const auto noon = md::new_york_to_utc(date, 12, 0);
          const auto evening = md::new_york_to_utc(date, 20, 15);
          if (!(md::trading_session(root, midnight).open && md::trading_date(midnight) == request.date) &&
              !(md::trading_session(root, noon).open && md::trading_date(noon) == request.date) &&
              !(md::trading_session(root, evening).open && md::trading_date(evening) == request.date)) continue;
          for (const auto& row : get("/option/list/contracts/quote?symbol=" + root + "&date=" + md::format_date(date))) {
            const auto expiry = expiration(row);
            if (expiry < request.date) continue;
            available.insert(expiry);
            active_strikes[{root, expiry}].insert(number(row, "strike"));
          }
        }
        // Expiration and strike lists span all history, not just the target date.
        for (const auto& row : get("/option/list/expirations?symbol=" + root)) {
          const auto expiry = expiration(row);
          if (available.contains(expiry)) { root_expiries[root].insert(expiry); expiries.insert(expiry); }
        }
      }
      if (request.subscription.max_expiries > 0)
        while (expiries.size() > static_cast<std::size_t>(request.subscription.max_expiries)) expiries.erase(std::prev(expiries.end()));
      for (const auto& [root, dates] : root_expiries) {
        for (const auto expiry : dates) {
          if (!expiries.contains(expiry)) continue;
          Chain chain{root, expiry, {}};
          for (const auto& row : get("/option/list/strikes?symbol=" + root + "&expiration=" + md::format_date(expiry))) {
            const auto strike = number(row, "strike");
            if (strike <= 0) throw std::runtime_error("ThetaData history: invalid strike");
            if (!active_strikes[{root, expiry}].contains(strike)) continue;
            chain.strikes.insert(strike);
            for (const auto type : {pricing::OptionType::Call, pricing::OptionType::Put}) define(root, expiry, strike, type, sink);
          }
          if (!chain.strikes.empty()) chains_.push_back(std::move(chain));
        }
      }
    }
    // Each stream is requested a block of windows at a time and handed out per window,
    // in the order windows were always published: prices, then each chain's quotes and
    // trades. One block length fits the widest chain's quotes in a response and every
    // chain's in the buffer; trades are far sparser than one-second quotes.
    std::size_t contracts = 0, widest = 0;
    for (const auto& chain : chains_) {
      contracts += 2 * chain.strikes.size();
      widest = std::max(widest, 2 * chain.strikes.size());
    }
    const auto seconds = std::min(kBlockRows / std::max<std::size_t>(widest, 1), kBufferedRows / std::max<std::size_t>(contracts, 1));
    const auto block = std::clamp(static_cast<md::Timestamp>(seconds) * md::kNanosPerSecond / kWindow * kWindow, kWindow, kLongestBlock);
    for (const auto& symbol : request.subscription.underlyings) {
      // At most one price row per second.
      add_stream(Kind::Prices, symbol, 0, kLongestBlock);
      streams_.back().index = md::conventions_for_root(symbol).style == pricing::ExerciseStyle::European;
    }
    for (std::size_t index = 0; index < chains_.size(); ++index) {
      add_stream(Kind::Quotes, chains_[index].root, index, block);
      add_stream(Kind::Trades, chains_[index].root, index, block);
    }
  }

  void read(HistoryWindow window, md::EventSink& sink) override {
    const auto date = md::new_york_time(window.begin).date;
    for (auto& stream : streams_) {
      if (!md::trading_session(stream.session, window.begin).open) continue;
      if (stream.kind == Kind::Quotes) interest(chains_[stream.chain], date, sink);
      if (window.begin >= stream.end) fetch(stream, window.begin, sink);
      for (; stream.position < stream.rows.size(); ++stream.position) {
        auto& row = stream.rows[stream.position];
        const auto time = time_of(row);
        if (time >= window.end) break;
        if (time >= window.begin) std::visit([&](auto& event) { sink.publish(std::move(event)); }, row);
      }
      if (stream.position == stream.rows.size()) {
        std::vector<Row>{}.swap(stream.rows);
        stream.position = 0;
      }
    }
    std::erase_if(interest_, [&](const auto& observation) {
      if (observation.ts >= window.end) return false;
      sink.publish(observation);
      return true;
    });
  }

 private:
  struct Chain { std::string root; md::Date expiry; std::set<double> strikes; };
  enum class Kind : std::uint8_t { Prices, Quotes, Trades };
  struct Stream {
    Kind kind = Kind::Prices;
    std::string session;          ///< the underlying or chain root whose session gates requests
    bool index = false;           ///< an index's prices rather than a stock's quotes
    std::size_t chain = 0;        ///< chains_ entry of a quote or trade stream
    md::Timestamp length = 0;     ///< block length, whole windows; halved after an oversized response
    md::Timestamp end = 0;        ///< exclusive end of the block requested last
    std::vector<Row> rows;        ///< that block's rows in time order, equal times as Theta sent them
    std::size_t position = 0;     ///< the first row not yet handed out
  };

  void add_stream(Kind kind, const std::string& session, std::size_t chain, md::Timestamp length) {
    Stream stream;
    stream.kind = kind;
    stream.session = session;
    stream.chain = chain;
    stream.length = length;
    streams_.push_back(std::move(stream));
  }
  static std::string selector(const Chain& chain) {
    return "?symbol=" + chain.root + "&expiration=" + md::format_date(chain.expiry) + "&strike=*&right=both";
  }
  // Daily OI, once per chain and New York date the chain trades on, held until a
  // window reaches its observation time.
  void interest(const Chain& chain, md::Date date, md::EventSink& sink) {
    if (!interest_days_.insert(std::make_tuple(chain.root, chain.expiry, date)).second) return;
    for (auto& row : get("/option/history/open_interest" + selector(chain) + "&date=" + md::format_date(date))) {
      if (!row.contains("open_interest") || row["open_interest"].is_null()) continue;
      const auto id = row_id(chain, row, sink);
      interest_.push_back(md::OpenInterest{id, timestamp(row), number(row, "open_interest")});
    }
  }
  // A block runs whole windows from `begin` for the stream's length, but stays within
  // one New York date (Theta's times are per date), the import day and the session.
  md::Timestamp block_end(const Stream& stream, md::Timestamp begin) const {
    const auto next_date = md::date_from_days(md::days_since_epoch(md::new_york_time(begin).date) + 1);
    const auto limit = std::min({begin + stream.length, day_.end, md::new_york_to_utc(next_date, 0, 0)});
    auto end = begin + kWindow;
    while (end < limit && md::trading_session(stream.session, end).open) end += kWindow;
    return std::min(end, limit);
  }
  std::string block_target(const Stream& stream, md::Timestamp begin, md::Timestamp end) const {
    const auto times = "&date=" + md::format_date(md::new_york_time(begin).date) + "&start_time=" + clock_text(begin) +
        "&end_time=" + clock_text(end - 1'000'000);
    if (stream.kind == Kind::Prices)
      return std::string(stream.index ? "/index/history/price" : "/stock/history/quote") + "?symbol=" + stream.session + times + "&interval=1s";
    const auto contract = selector(chains_[stream.chain]);
    if (stream.kind == Kind::Quotes) return "/option/history/quote" + contract + times + "&interval=1s";
    return "/option/history/trade" + contract + times;
  }
  // Requests the stream's next block from `begin`. A response over Theta's row or byte
  // limit halves the stream's blocks and asks again; one window over them fails.
  void fetch(Stream& stream, md::Timestamp begin, md::EventSink& sink) {
    for (;;) {
      const auto end = block_end(stream, begin);
      const auto body = download(block_target(stream, begin, end));
      const auto* oversized = body.size() > kMaxBytes ? "ThetaData history: response exceeds 32 MiB"
          : row_count(body) > kMaxRows ? "ThetaData history: response row limit exceeded" : nullptr;
      if (oversized) {
        if (end - begin <= kWindow) throw std::runtime_error(oversized);
        stream.length = std::max(kWindow, (end - begin) / 2 / kWindow * kWindow);
        continue;
      }
      std::vector<Row> block;
      each_row(body, [&](const json& row) {
        const auto time = timestamp(row);
        if (time < begin || time >= end) return;
        if (stream.kind == Kind::Prices) {
          const auto price = stream.index ? number(row, "price") : 0;
          const auto bid = stream.index ? 0 : number(row, "bid");
          const auto ask = stream.index ? 0 : number(row, "ask");
          if (price > 0 || bid > 0 || ask > 0) block.emplace_back(md::UnderlyingQuote{stream.session, time, bid, ask, price});
          return;
        }
        const auto& chain = chains_[stream.chain];
        if (stream.kind == Kind::Quotes) {
          const auto id = row_id(chain, row, sink);
          block.emplace_back(md::OptionQuote{id, time, number(row, "bid"), number(row, "ask"), number(row, "bid_size"), number(row, "ask_size")});
          return;
        }
        const auto price = number(row, "price");
        const auto size = number(row, "size");
        if (price <= 0 || size <= 0) throw std::runtime_error("ThetaData history: incomplete trade");
        const auto id = row_id(chain, row, sink);
        // ThetaData's exchange code, when a row carries one; otherwise the venue is unknown.
        const auto exchange = row.contains("exchange") && row["exchange"].is_number_unsigned() ? row["exchange"].get<std::uint64_t>() : 0;
        block.emplace_back(md::OptionTrade{id, time, price, size, static_cast<std::uint16_t>(exchange <= 0xffff ? exchange : 0)});
      });
      std::stable_sort(block.begin(), block.end(), [](const Row& left, const Row& right) { return time_of(left) < time_of(right); });
      stream.rows = std::move(block);
      stream.position = 0;
      stream.end = end;
      return;
    }
  }
  // The response body, empty for Theta's documented no-data answers. Requests are
  // serial and Theta Terminal queues them itself, so only a 429 waits.
  std::string download(const std::string& target) {
    for (int attempt = 0; attempt < 4; ++attempt) {
      net::HttpResponse response;
      try { response = http_.get_direct("http://127.0.0.1:25503/v3" + target + "&format=ndjson", {}, std::chrono::seconds(60)); }
      catch (const std::exception&) { throw std::runtime_error("ThetaData history: local request failed; check Theta Terminal and its login"); }
      if (response.status == 472 || response.status == 204) return {};  // documented no-data response
      if (response.status == 429 && attempt < 3) {
        int seconds = 1 << attempt;
        const auto retry = response.header("Retry-After");
        // Waits as asked, within 1..60 s; a value it cannot read (an HTTP date) keeps the backoff.
        if (!retry.empty()) {
          long long asked = 0;
          const auto [end, error] = std::from_chars(retry.data(), retry.data() + retry.size(), asked);
          if (error == std::errc{} && end == retry.data() + retry.size()) seconds = static_cast<int>(std::clamp<long long>(asked, 1, 60));
        }
        wait_(std::chrono::seconds(seconds));
        continue;
      }
      if (response.status != 200) throw std::runtime_error("ThetaData history: HTTP " + std::to_string(response.status) + " (check historical entitlements and Theta Terminal login)");
      return std::move(response.body);
    }
    throw std::runtime_error("ThetaData history: request limit exhausted");
  }
  std::vector<json> get(const std::string& target) { return rows(download(target)); }
  md::InstrumentId define(const std::string& root, md::Date expiry, double strike,
                           pricing::OptionType type, md::EventSink& sink) {
    md::OptionContract contract;
    const auto conventions = md::conventions_for_root(root);
    contract.root = root;
    contract.underlying = conventions.underlying;
    contract.style = conventions.style;
    contract.settlement = conventions.settlement;
    contract.standard = conventions.standard;
    contract.expiry = expiry;
    contract.strike = strike;
    contract.type = type;
    const auto key = contract.osi_symbol();
    const auto [it, inserted] = ids_.emplace(key, static_cast<md::InstrumentId>(ids_.size()));
    if (inserted) sink.publish(md::ContractDefinition{it->second, std::move(contract)});
    return it->second;
  }
  md::InstrumentId row_id(const Chain& chain, const json& row, md::EventSink& sink) {
    const auto root = row.value("symbol", chain.root);
    const auto expiry = row.contains("expiration") ? expiration(row) : chain.expiry;
    const auto strike = number(row, "strike");
    const auto right = row.value("right", "");
    const bool call = right == "call" || right == "C" || right == "CALL";
    const bool put = right == "put" || right == "P" || right == "PUT";
    if (root != chain.root || expiry != chain.expiry || strike <= 0 ||
        (!call && !put))
      throw std::runtime_error("ThetaData history: invalid contract identity");
    return define(root, expiry, strike, call ? pricing::OptionType::Call : pricing::OptionType::Put, sink);
  }
  net::HttpClient& http_;
  HistoryWait wait_;
  HistoryWindow day_;
  std::vector<Chain> chains_;
  std::vector<Stream> streams_;
  std::map<std::string, md::InstrumentId> ids_;
  std::set<std::tuple<std::string, md::Date, md::Date>> interest_days_;
  std::vector<md::OpenInterest> interest_;
};
}
std::unique_ptr<HistorySource> theta_history(net::HttpClient& http, HistoryWait wait) {
  return std::make_unique<ThetaHistory>(http, std::move(wait));
}
}  // namespace openport::providers
