#include "openport/providers/history.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "openport/providers/massive.hpp"
#include "openport/providers/options.hpp"

namespace openport::providers {
namespace {
using nlohmann::json;
constexpr std::size_t kMaxPending = 1'000'000;
// Massive documents a 50,000-row maximum for /v3/quotes and /v3/trades pages.
constexpr std::size_t kMaxPage = 50'000, kMinPage = 100;

double number(const json& row, const char* field) {
  if (!row.contains(field) || !row[field].is_number())
    throw std::runtime_error("Massive history: missing numeric field");
  const auto value = row[field].get<double>();
  if (!std::isfinite(value) || value < 0) throw std::runtime_error("Massive history: invalid numeric value");
  return value;
}
md::Timestamp timestamp(const json& row, const char* field) {
  if (!row.contains(field) || !row[field].is_number_integer() || row[field] <= 0 ||
      (row[field].is_number_unsigned() && row[field].get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<md::Timestamp>::max())))
    throw std::runtime_error("Massive history: missing or invalid market timestamp");
  return row[field].get<md::Timestamp>();
}

class MassiveHistory final : public HistorySource {
 public:
  MassiveHistory(net::HttpClient& http, std::string key, HistoryWait wait)
      : http_(http), key_(std::move(key)), wait_(std::move(wait)) {
    if (key_.empty()) throw std::invalid_argument("import: set MASSIVE_API_KEY");
  }
  md::Timestamp receipt_time() const noexcept override { return received_; }
  void prepare(const ImportDay& request, HistoryWindow day, md::EventSink& sink) override {
    day_ = day;
    for (const auto& symbol : request.subscription.underlyings) {
      std::map<std::string, md::OptionContract> contracts;
      std::set<md::Date> expiries;
      Cursor cursor;
      cursor.path = "/v3/reference/options/contracts";
      cursor.url = base_ + cursor.path + "?underlying_ticker=" + symbol +
          "&as_of=" + md::format_date(request.date) + "&expiration_date.gte=" + md::format_date(request.date) +
          "&expired=false&sort=expiration_date&order=asc&limit=1000";
      while (!cursor.url.empty()) {
        for (const auto& row : page(cursor)) {
          const auto ticker = row.at("ticker").get<std::string>();
          auto contract = md::parse_osi(ticker.starts_with("O:") ? ticker.substr(2) : ticker);
          if (!ticker.starts_with("O:") || !contract || contract->underlying != symbol ||
              contract->expiry != import_date(row.at("expiration_date").get<std::string>()) ||
              contract->strike != number(row, "strike_price") ||
              row.at("contract_type") != (contract->type == pricing::OptionType::Call ? "call" : "put"))
            throw std::runtime_error("Massive history: invalid contract identity");
          if (contract->expiry < request.date) continue;
          const auto style = row.at("exercise_style").get<std::string>();
          if (style != "american" && style != "european")
            throw std::runtime_error("Massive history: unsupported exercise style");
          contract->style = style == "european" ? pricing::ExerciseStyle::European : pricing::ExerciseStyle::American;
          contract->multiplier = number(row, "shares_per_contract");
          if (contract->multiplier <= 0) throw std::runtime_error("Massive history: invalid multiplier");
          if (row.contains("additional_underlyings") && !row["additional_underlyings"].empty()) contract->standard = false;
          if (!contract->standard) continue;
          expiries.insert(contract->expiry);
          contracts.emplace(ticker, *contract);
          if (contracts.size() > 500'000) throw std::runtime_error("Massive history: contract limit exceeded; reduce symbols");
        }
      }
      // Expiry selection is known as of the requested day. Strike admission stays
      // in the common merger, using contemporaneous parity rather than today's spot.
      if (request.subscription.max_expiries > 0)
        while (expiries.size() > static_cast<std::size_t>(request.subscription.max_expiries)) expiries.erase(std::prev(expiries.end()));
      for (const auto& [ticker, contract] : contracts) {
        if (!contract.standard || !expiries.contains(contract.expiry)) continue;
        if (streams_.size() / 2 >= 500'000) throw std::runtime_error("Massive history: contract limit exceeded; reduce symbols");
        const auto id = static_cast<md::InstrumentId>(streams_.size() / 2);
        sink.publish(md::ContractDefinition{id, contract});
        for (const bool trades : {false, true}) {
          Stream stream;
          stream.id = id;
          stream.trades = trades;
          stream.cursor.path = std::string(trades ? "/v3/trades/" : "/v3/quotes/") + ticker;
          stream.cursor.url = base_ + stream.cursor.path + "?timestamp.gte=" + std::to_string(day.begin) +
              "&timestamp.lt=" + std::to_string(day.end) + "&order=asc&sort=timestamp&limit=";
          streams_.push_back(std::move(stream));
        }
      }
    }
    // Every stream may hold one page, so pages share the buffer cap: large pages
    // for narrow chains, never fewer requests than the cap allows for broad ones.
    const auto limit = std::to_string(massive_history_page_size(streams_.size()));
    for (auto& stream : streams_) stream.cursor.url += limit;
  }
  void read(HistoryWindow window, md::EventSink& sink) override {
    for (auto& stream : streams_) {
      for (;;) {
        if (stream.position == stream.pending.size()) {
          pending_ -= stream.pending.size();
          std::vector<md::RecordedEvent>{}.swap(stream.pending);
          stream.position = 0;
          if (stream.cursor.url.empty()) break;
          for (const auto& row : page(stream.cursor)) {
            const auto sip = timestamp(row, "sip_timestamp");
            if (sip < stream.last) throw std::runtime_error("Massive history: unordered history page");
            stream.last = sip;
            if (sip < day_.begin || sip >= day_.end) continue;
            md::Event event;
            if (stream.trades) {
              // The event model cannot undo corrected/cancelled prints. Refuse
              // these instead of silently double-counting traded volume.
              if (row.contains("correction") && row["correction"] != 0)
                throw std::runtime_error("Massive history: corrected trades are unsupported");
              const auto time = row.contains("participant_timestamp") ? timestamp(row, "participant_timestamp") : sip;
              if (time > sip) throw std::runtime_error("Massive history: trade timestamp follows SIP receipt");
              const auto price = number(row, "price"), size = number(row, "size");
              if (price <= 0 || size <= 0) throw std::runtime_error("Massive history: incomplete trade");
              event = md::OptionTrade{stream.id, time, price, size};
            } else {
              event = md::OptionQuote{stream.id, sip, number(row, "bid_price"), number(row, "ask_price"),
                                      number(row, "bid_size"), number(row, "ask_size")};
            }
            if (++pending_ > kMaxPending) throw std::runtime_error("Massive history: pending page limit exceeded; reduce symbols or expiries");
            stream.pending.push_back({sip, std::move(event)});
          }
          continue;
        }
        auto& record = stream.pending[stream.position];
        if (record.received >= window.end) break;
        received_ = record.received;
        if (record.received >= window.begin) sink.publish(std::move(record.event));
        ++stream.position;

      }
    }
  }
 private:
  struct Cursor {
    std::string path, url;
    std::size_t pages = 0;
  };
  struct Stream {
    md::InstrumentId id = 0;
    bool trades = false;
    Cursor cursor;
    std::vector<md::RecordedEvent> pending;
    std::size_t position = 0;
    md::Timestamp last = 0;
  };
  json page(Cursor& cursor) {
    if (++cursor.pages > 100'000) throw std::runtime_error("Massive history: pagination did not end");
    for (int attempt = 0; attempt < 4; ++attempt) {
      wait_(std::chrono::milliseconds(100));
      net::HttpResponse response;
      try { response = http_.get_direct(cursor.url, {{"Authorization", "Bearer " + key_}}, std::chrono::seconds(60)); }
      catch (const std::exception&) { throw std::runtime_error("Massive history: request failed"); }
      if (response.status == 429 && attempt < 3) {
        const auto retry = response.header("Retry-After");
        wait_(std::chrono::seconds(retry.empty() ? 1 << attempt : parse_integer(retry, "Massive Retry-After", 1, 60)));
        continue;
      }
      if (response.status != 200) throw std::runtime_error("Massive history: HTTP " + std::to_string(response.status) + " (check key, historical quotes/trades entitlements and date availability)");
      if (response.body.size() > 32 * 1024 * 1024) throw std::runtime_error("Massive history: response exceeds 32 MiB");
      auto body = json::parse(response.body, nullptr, false);
      if (!body.is_object() || body.value("status", "") != "OK") throw std::runtime_error("Massive history: invalid or unsuccessful response");
      // Empty successful responses may omit the optional results field.
      auto rows = body.value("results", json::array());
      if (!rows.is_array() || rows.size() > kMaxPage) throw std::runtime_error("Massive history: invalid results array");
      auto next = body.value("next_url", "");
      if (!next.empty()) {
        const auto url = net::parse_url(next);
        const auto origin = net::parse_url(base_);
        if (!url || !url->tls || url->host != origin->host || url->port != origin->port ||
            url->target.substr(0, url->target.find('?')) != cursor.path || next == cursor.url)
          throw std::runtime_error("Massive history: invalid pagination URL");
      }
      cursor.url = std::move(next);
      return rows;
    }
    throw std::runtime_error("Massive history: request limit exhausted");
  }
  net::HttpClient& http_;
  std::string key_;
  HistoryWait wait_;
  const std::string base_ = MassiveProvider::Options{}.base_url;
  HistoryWindow day_;
  std::vector<Stream> streams_;
  std::size_t pending_ = 0;
  md::Timestamp received_ = 0;
};
}
std::unique_ptr<HistorySource> massive_history(net::HttpClient& http, std::string key, HistoryWait wait) {
  return std::make_unique<MassiveHistory>(http, std::move(key), std::move(wait));
}
std::size_t massive_history_page_size(std::size_t streams) noexcept {
  return std::clamp(kMaxPending / std::max<std::size_t>(streams, 1), kMinPage, kMaxPage);
}
}  // namespace openport::providers
