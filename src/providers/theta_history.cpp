#include "openport/providers/history.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "openport/providers/options.hpp"

namespace openport::providers {
namespace {
using nlohmann::json;

std::vector<json> rows(std::string_view body) {
  std::vector<json> result;
  std::istringstream input{std::string(body)};
  std::string line;
  while (std::getline(input, line)) {
    if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
    auto row = json::parse(line, nullptr, false);
    if (row.is_discarded()) throw std::runtime_error("ThetaData history: invalid NDJSON response");
    if (!row.is_object()) throw std::runtime_error("ThetaData history: expected NDJSON objects");
    result.push_back(std::move(row));
    if (result.size() > 250'000) throw std::runtime_error("ThetaData history: response row limit exceeded");
  }
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

class ThetaHistory final : public HistorySource {
 public:
  ThetaHistory(net::HttpClient& http, HistoryWait wait) : http_(http), wait_(std::move(wait)) {}
  /// Its trade rows can name the exchange.
  bool trade_venues() const noexcept override { return true; }

  void prepare(const ImportDay& request, HistoryWindow day, md::EventSink& sink) override {
    request_ = request;
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
  }

  void read(HistoryWindow window, md::EventSink& sink) override {
    const auto local = md::new_york_time(window.begin);
    const auto date = md::format_date(local.date);
    const auto times = "&date=" + date + "&start_time=" + clock_text(window.begin) + "&end_time=" + clock_text(window.end - 1'000'000);
    for (const auto& symbol : request_.subscription.underlyings) {
      if (!md::trading_session(symbol, window.begin).open) continue;
      const bool index = md::conventions_for_root(symbol).style == pricing::ExerciseStyle::European;
      const auto endpoint = index ? "/index/history/price" : "/stock/history/quote";
      for (const auto& row : get(std::string(endpoint) + "?symbol=" + symbol + times + "&interval=1s")) {
        const auto time = timestamp(row);
        if (time < window.begin || time >= window.end) continue;
        const auto price = index ? number(row, "price") : 0;
        const auto bid = index ? 0 : number(row, "bid");
        const auto ask = index ? 0 : number(row, "ask");
        if (price > 0 || bid > 0 || ask > 0) sink.publish(md::UnderlyingQuote{symbol, time, bid, ask, price});
      }
    }
    for (const auto& chain : chains_) {
      if (!md::trading_session(chain.root, window.begin).open) continue;
      const auto contract = "?symbol=" + chain.root + "&expiration=" + md::format_date(chain.expiry) + "&strike=*&right=both";
      const auto interest_key = std::make_tuple(chain.root, chain.expiry, local.date);
      if (interest_days_.insert(interest_key).second) {
        for (auto& row : get("/option/history/open_interest" + contract + "&date=" + date)) {
          if (!row.contains("open_interest") || row["open_interest"].is_null()) continue;
          const auto id = row_id(chain, row, sink);
          interest_.push_back(md::OpenInterest{id, timestamp(row), number(row, "open_interest")});
        }
      }
      for (const auto& row : get("/option/history/quote" + contract + times + "&interval=1s")) {
        const auto time = timestamp(row);
        if (time < window.begin || time >= window.end) continue;
        const auto id = row_id(chain, row, sink);
        sink.publish(md::OptionQuote{id, time, number(row, "bid"), number(row, "ask"), number(row, "bid_size"), number(row, "ask_size")});
      }
      for (const auto& row : get("/option/history/trade" + contract + times)) {
        const auto time = timestamp(row);
        if (time < window.begin || time >= window.end) continue;
        const auto price = number(row, "price");
        const auto size = number(row, "size");
        if (price <= 0 || size <= 0) throw std::runtime_error("ThetaData history: incomplete trade");
        const auto id = row_id(chain, row, sink);
        // ThetaData's exchange code, when a row carries one; otherwise the venue is unknown.
        const auto exchange = row.contains("exchange") && row["exchange"].is_number_unsigned() ? row["exchange"].get<std::uint64_t>() : 0;
        sink.publish(md::OptionTrade{id, time, price, size, static_cast<std::uint16_t>(exchange <= 0xffff ? exchange : 0)});
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
  std::vector<json> get(const std::string& target) {
    for (int attempt = 0; attempt < 4; ++attempt) {
      wait_(std::chrono::milliseconds(100));
      net::HttpResponse response;
      try { response = http_.get_direct("http://127.0.0.1:25503/v3" + target + "&format=ndjson", {}, std::chrono::seconds(60)); }
      catch (const std::exception&) { throw std::runtime_error("ThetaData history: local request failed; check Theta Terminal and its login"); }
      if (response.status == 472 || response.status == 204) return {};  // documented no-data response
      if (response.status == 429 && attempt < 3) {
        int seconds = 1 << attempt;
        const auto retry = response.header("Retry-After");
        if (!retry.empty()) seconds = parse_integer(retry, "ThetaData Retry-After", 1, 60);
        wait_(std::chrono::seconds(seconds));
        continue;
      }
      if (response.status != 200) throw std::runtime_error("ThetaData history: HTTP " + std::to_string(response.status) + " (check historical entitlements and Theta Terminal login)");
      if (response.body.size() > 32 * 1024 * 1024) throw std::runtime_error("ThetaData history: response exceeds 32 MiB");
      return rows(response.body);
    }
    throw std::runtime_error("ThetaData history: request limit exhausted");
  }
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
  ImportDay request_;
  std::vector<Chain> chains_;
  std::map<std::string, md::InstrumentId> ids_;
  std::set<std::tuple<std::string, md::Date, md::Date>> interest_days_;
  std::vector<md::OpenInterest> interest_;
};
}
std::unique_ptr<HistorySource> theta_history(net::HttpClient& http, HistoryWait wait) {
  return std::make_unique<ThetaHistory>(http, std::move(wait));
}
}  // namespace openport::providers
