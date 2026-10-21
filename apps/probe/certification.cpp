#include "certification.hpp"

#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>
#include <type_traits>

#include "openport/pricing/black.hpp"
#include "openport/providers/factory.hpp"

namespace openport::probe {
namespace {
using Json = nlohmann::json;
constexpr std::array<const char*, std::variant_size_v<md::Event>> names{
    "ContractDefinition", "OptionQuote", "OptionTrade", "OpenInterest", "VendorGreeks",
    "UnderlyingQuote", "ProviderStatus", "UnderlyingClose", "SnapshotComplete",
    "OptionVolume", "SnapshotHeartbeat", "TradingHalt"};
constexpr const char* limitations =
    "Observation report, not proof of live entitlement or unit correctness. Unknown clocks stay unknown. "
    "Receipt-time matches are suspicious, not proof of a wall-clock fallback; event provenance cannot "
    "be established from timestamps alone. Scale comparisons use BSM with zero rate/dividend, vendor IV "
    "and a spot within 60 seconds; model differences require review. Missing samples are not passes.";
std::string error_kind(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (text.find("token") != std::string::npos || text.find("credential") != std::string::npos ||
      text.find("api_key") != std::string::npos || text.find("401") != std::string::npos ||
      text.find("403") != std::string::npos) return "authentication_or_entitlement";
  if (text.find("rate limit") != std::string::npos || text.find("429") != std::string::npos) return "rate_limit";
  if (text.find("timestamp") != std::string::npos) return "timestamp";
  if (text.find("timeout") != std::string::npos || text.find("connect") != std::string::npos) return "transport";
  if (text.find("json") != std::string::npos || text.find("parse") != std::string::npos) return "payload";
  return "other";
}

}

Conformance::Conformance(const md::Subscription& subscription, bool streaming, std::size_t minimum_quotes)
    : readiness_(subscription, streaming, minimum_quotes), underlyings_(subscription.underlyings) {
  for (const auto* field : {"iv", "vega", "theta", "rho"}) fields_.try_emplace(field);
}

void Conformance::FieldCheck::sample(double value) {
  if (std::isnan(value)) { ++missing; return; }
  if (!std::isfinite(value)) { ++invalid; return; }
  if (present++ == 0) minimum = maximum = value;
  minimum = std::min(minimum, value);
  maximum = std::max(maximum, value);
}

void Conformance::FieldCheck::compare(double value, double reference) {
  if (!std::isfinite(value) || !std::isfinite(reference) || std::abs(reference) < 1e-6) return;
  ++scale_samples;
  const double ratio = std::abs(value / reference);
  // Flag orders-of-magnitude mistakes (e.g. raw derivative vs per point/day).
  // These are diagnostic hints, not mathematical bounds on vendor models.
  if (ratio > 20 || (ratio > 0 && ratio < .05)) ++scale_suspect;
}

void Conformance::clock(std::size_t type, const std::string& key, md::Timestamp ts, md::Timestamp received) {
  auto& check = clocks_[type];
  if (ts <= 0) { ++check.unknown; return; }
  if (check.known++ == 0) check.first = ts;
  check.first = std::min(check.first, ts);
  check.last = std::max(check.last, ts);
  if (ts > received) ++check.future;
  if (ts == received) ++check.receipt_matches;
  auto& previous = previous_[{type, key}];
  if (previous > ts) ++check.regressions;
  previous = std::max(previous, ts);
}

void Conformance::greeks(const md::VendorGreeks& value) {
  fields_["iv"].sample(value.iv);
  fields_["vega"].sample(value.vega);
  fields_["theta"].sample(value.theta);
  fields_["rho"].sample(value.rho);
  if (std::isfinite(value.iv) && value.iv <= 0) ++fields_["iv"].invalid;
  if (std::isfinite(value.iv) && value.iv > 5) ++fields_["iv"].scale_suspect;
  if (std::isfinite(value.vega) && value.vega < 0) ++fields_["vega"].scale_suspect;
  const auto found = contracts_.find(value.id);
  if (found == contracts_.end() || !(value.iv > 0 && value.iv <= 5) || value.ts <= 0) return;
  const auto& contract = found->second.definition;
  const auto spot = spots_.find(contract.underlying);
  if (spot == spots_.end() || spot->second.ts <= 0 ||
      std::abs(static_cast<double>(value.ts) - static_cast<double>(spot->second.ts)) > 60e9) return;
  const auto& quote = spot->second;
  const double price = quote.last > 0 ? quote.last :
                       quote.bid > 0 && quote.ask > 0 ? (quote.bid + quote.ask) / 2 : 0;
  const double years = md::years_between(value.ts, contract.expiry_time());
  if (!(price > 0 && years > 0 && contract.strike > 0) || !contract.standard) return;
  const auto reference = pricing::bsm_greeks({contract.type, price, contract.strike, years, 0, 0, value.iv});
  fields_["vega"].compare(value.vega, reference.vega / 100);
  fields_["theta"].compare(value.theta, reference.theta / 365);
  fields_["rho"].compare(value.rho, reference.rho / 100);
}

void Conformance::publish(md::Event event) { observe(event, md::now()); }

void Conformance::observe(const md::Event& event, md::Timestamp received) {
  const std::lock_guard lock(mutex_);
  ++counts_[event.index()];
  readiness_.apply(event);
  std::visit([&](const auto& value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, md::ContractDefinition>) {
      contracts_.try_emplace(value.id, Contract{value.contract});
    } else if constexpr (std::is_same_v<T, md::ProviderStatus>) {
      // Status times are operational, and diagnostics may contain arbitrary secrets.
      ++states_[std::string(md::to_string(value.state))];
      if (value.state == md::FeedState::Error) {
        ++errors_;
        ++error_kinds_[error_kind(value.message)];
      }
      if (value.message.find("degraded") != std::string::npos) ++degraded_;
      if (value.state == md::FeedState::Stopped && value.underlying.empty()) ended_ = true;
    } else {
      std::string key;
      if constexpr (requires { value.id; }) {
        key = std::to_string(value.id);
        const auto found = contracts_.find(value.id);
        if (found == contracts_.end()) ++undefined_;
        else {
          ++found->second.events;
          if constexpr (std::is_same_v<T, md::OptionVolume>) found->second.volume = true;
          if constexpr (std::is_same_v<T, md::OpenInterest>) found->second.open_interest = true;
        }
      } else if constexpr (requires { value.symbol; }) key = value.symbol;
      else if constexpr (requires { value.underlying; }) key = value.underlying;
      clock(event.index(), key, value.ts, received);
      if constexpr (std::is_same_v<T, md::VendorGreeks>) greeks(value);
      if constexpr (std::is_same_v<T, md::UnderlyingQuote>) spots_[value.symbol] = value;
      if constexpr (std::is_same_v<T, md::OpenInterest> || std::is_same_v<T, md::OptionVolume>)
        if (!std::isfinite(value.contracts) || value.contracts < 0 || value.contracts != std::floor(value.contracts))
          ++bad_counts_;
    }
  }, event);
  changed_.notify_all();
}

void Conformance::wait(std::chrono::steady_clock::time_point deadline, bool snapshot) {
  std::unique_lock lock(mutex_);
  changed_.wait_until(lock, deadline, [&] { return errors_ > 0 || ended_ || (snapshot && readiness_.all_ready()); });
}

Json Conformance::report(bool startup_failed) const {
  const std::lock_guard lock(mutex_);
  Json result{{"schema_version", 1}, {"limitations", limitations},
              {"redaction", "Configuration, payloads, diagnostic text, account identifiers and symbol strings omitted. Contracts use per-feed numeric IDs."},
              {"event_counts", Json::object()}, {"contracts", Json::array()},
              {"timestamps", Json::object()}, {"greeks", Json::object()},
              {"readiness", Json::array()}, {"provider_states", states_},
              {"errors", {{"provider", errors_}, {"startup", startup_failed ? 1 : 0},
                          {"undefined_instrument", undefined_}, {"invalid_activity_count", bad_counts_}}}};
  bool failed = startup_failed || errors_ > 0 || undefined_ > 0 || bad_counts_ > 0;
  result["error_categories"] = error_kinds_;
  result["degraded_statuses"] = degraded_;
  bool review = degraded_ > 0;
  for (std::size_t i = 0; i < names.size(); ++i) result["event_counts"][names[i]] = counts_[i];
  for (const auto& [type, check] : clocks_) {
    result["timestamps"][names[type]] = {{"known", check.known}, {"unknown", check.unknown},
        {"future", check.future}, {"regressions", check.regressions}, {"receipt_time_matches", check.receipt_matches},
        {"first_market_ns", check.first > 0 ? Json(check.first) : Json()},
        {"last_market_ns", check.last > 0 ? Json(check.last) : Json()}};
    failed = failed || check.future > 0 || check.regressions > 0;
    review = review || check.unknown > 0 || check.receipt_matches > 0;
  }
  result["timestamp_scope"] = "Nondecreasing per event type and instrument/underlying; equal timestamps allowed. ProviderStatus excluded (operational clock). No future tolerance.";
  result["wall_clock_provenance"] = "unverified; inspect adapter and source payloads locally";
  std::size_t volume = 0, interest = 0;
  for (const auto& [id, contract] : contracts_) {
    const auto& definition = contract.definition;
    const auto underlying = std::find(underlyings_.begin(), underlyings_.end(), definition.underlying);
    result["contracts"].push_back({{"instrument_id", id}, {"events", contract.events},
        {"underlying_id", underlying == underlyings_.end() ? Json() : Json(underlying - underlyings_.begin())},
        {"expiry", md::valid_date(definition.expiry) ? Json(md::format_date(definition.expiry)) : Json()},
        {"strike", std::isfinite(definition.strike) ? Json(definition.strike) : Json()},
        {"right", definition.type == pricing::OptionType::Call ? "call" : "put"},
        {"settlement", definition.settlement == md::Settlement::AM ? "AM" : "PM"},
        {"volume_seen", contract.volume}, {"open_interest_seen", contract.open_interest}});
    volume += contract.volume;
    interest += contract.open_interest;
  }
  result["contracts_seen"] = contracts_.size();
  result["presence"] = {{"volume_contracts", volume}, {"open_interest_contracts", interest}};
  review = review || volume < contracts_.size() || interest < contracts_.size();
  const std::map<std::string, std::string> units{{"iv", "decimal"}, {"vega", "price per volatility percentage point"},
      {"theta", "price per calendar day"}, {"rho", "price per rate percentage point"}};
  for (const auto& [name, field] : fields_) {
    result["greeks"][name] = {{"unit", units.at(name)}, {"present", field.present}, {"missing", field.missing},
        {"invalid", field.invalid}, {"minimum", field.present ? Json(field.minimum) : Json()},
        {"maximum", field.present ? Json(field.maximum) : Json()}, {"scale_samples", field.scale_samples},
        {"scale_suspect", field.scale_suspect}, {"unit_validation", "unverified"}};
    failed = failed || field.invalid > 0;
    review = review || field.present == 0 || field.missing > 0 || field.scale_suspect > 0 ||
             (name != "iv" && field.scale_samples == 0);
  }
  result["scale_check"] = "IV > 5 (500%) or negative vega flagged; vega/theta/rho magnitude ratios outside [0.05,20] against the stated BSM reference flagged for review, not rejected.";
  for (std::size_t i = 0; i < underlyings_.size(); ++i)
    result["readiness"].push_back({{"underlying_id", i}, {"ready", readiness_.ready(underlyings_[i])}});
  const bool ready = readiness_.all_ready() && !contracts_.empty() && counts_[1] > 0;
  result["ready"] = ready;
  result["outcome"] = failed ? "fail" : !ready ? "incomplete" : review ? "review" : "observed_checks_passed";
  result["exit_code"] = failed || !ready ? 1 : 0;
  return result;
}

std::string Conformance::markdown(const Json& report) {
  std::ostringstream out;
  out << "# OpenPort provider conformance report\n\n"
      << "Outcome: **" << report.at("outcome").get<std::string>() << "**\n\n"
      << limitations << "\n\n" << report.at("redaction").get<std::string>() << "\n\n"
      << "Contracts seen: " << report.at("contracts_seen") << ". With volume: "
      << report.at("presence").at("volume_contracts") << "; with open interest: "
      << report.at("presence").at("open_interest_contracts") << ".\n\n"
      << "| Event | Count |\n| --- | ---: |\n";
  for (const auto& [name, count] : report.at("event_counts").items()) out << "| " << name << " | " << count << " |\n";
  out << "\n" << report.at("timestamp_scope").get<std::string>() << "\n\n"
      << "| Event | Known | Unknown | Future | Regressions | Receipt matches |\n| --- | ---: | ---: | ---: | ---: | ---: |\n";
  for (const auto& [name, check] : report.at("timestamps").items())
    out << "| " << name << " | " << check.at("known") << " | " << check.at("unknown") << " | "
        << check.at("future") << " | " << check.at("regressions") << " | " << check.at("receipt_time_matches") << " |\n";
  out << "\n| Field | Expected unit (per underlying unit) | Present | Missing | Invalid | Scale flags |\n"
      << "| --- | --- | ---: | ---: | ---: | ---: |\n";
  for (const auto& [name, check] : report.at("greeks").items())
    out << "| " << name << " | " << check.at("unit").get<std::string>() << " | " << check.at("present")
        << " | " << check.at("missing") << " | " << check.at("invalid") << " | " << check.at("scale_suspect") << " |\n";
  out << "\nErrors: `" << report.at("errors").dump() << "`\n\n"
      << "Full measurements and run metadata (identical to conformance.json):\n\n"
      << "```json\n" << report.dump(2) << "\n```\n";
  return out.str();
}

int certify(const md::ProviderConfig& config, const md::Subscription& subscription,
            int seconds, bool duration, bool snapshot, std::size_t minimum_quotes,
            const std::filesystem::path& directory) {
  if (!std::filesystem::create_directory(directory))
    throw std::invalid_argument("certification requires a new report directory");
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
  const auto started = std::chrono::steady_clock::now();
  const auto operational_start = md::now();
  std::unique_ptr<md::Provider> provider;
  bool startup_failed = false;
  std::string startup_category;
  try { provider = providers::make_provider(config); }
  catch (const std::exception& error) { startup_failed = true; startup_category = error_kind(error.what()); }
  catch (...) { startup_failed = true; startup_category = "other"; }
  const bool streaming = provider && provider->capabilities().poll_interval.count() == 0;
  Conformance monitor(subscription, streaming, minimum_quotes);
  if (snapshot && streaming) { startup_failed = true; startup_category = "snapshot_requires_polling"; }
  if (!startup_failed) {
    try {
      provider->start(subscription, monitor);
      monitor.wait(started + std::chrono::seconds(seconds), snapshot || (!duration && !streaming));
    } catch (const std::exception& error) { startup_failed = true; startup_category = error_kind(error.what()); }
    catch (...) { startup_failed = true; startup_category = "other"; }
  }
  // Join before reading the final counters, including stop/error callbacks.
  if (provider) provider->stop();
  auto report = monitor.report(startup_failed);
  report["startup_error_category"] = startup_category.empty() ? Json() : Json(startup_category);
  const auto provider_names = providers::provider_names();
  report["provider"] = std::find(provider_names.begin(), provider_names.end(), config.name) != provider_names.end() ? config.name : "unknown";
  report["source"] = config.name == "demo" ? "simulated" : config.name == "replay" ? "replayed; original provenance unverified" : "provider connection; entitlement unverified";
  report["mode"] = snapshot || (!duration && !streaming) ? "one_snapshot_cycle" : "duration";
  report["requested_seconds"] = seconds;
  report["openport_version"] = OPENPORT_VERSION;
  report["operational_start_ns"] = operational_start;
  report["elapsed_seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  for (const auto* name : {"conformance.json", "conformance.md"}) {
    std::ofstream file(directory / name);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file << (std::string_view(name) == "conformance.json" ? report.dump(2) + "\n" : Conformance::markdown(report));
    file.close();
  }
  return report.at("exit_code").get<int>();
}

}  // namespace openport::probe
