#include "openport/providers/factory.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <stdexcept>
#include <string>

#include "openport/providers/cboe.hpp"
#include "openport/providers/massive.hpp"
#include "openport/providers/options.hpp"
#include "openport/providers/replay.hpp"
#include "openport/providers/schwab.hpp"
#include "openport/providers/demo_feed.hpp"
#include "openport/providers/thetadata.hpp"
#include "openport/providers/tradier.hpp"
#include "openport/providers/tastytrade.hpp"
#ifdef OPENPORT_WITH_DATABENTO
#include "openport/providers/databento.hpp"
#endif

namespace openport::providers {
namespace {

std::string option_or(const md::ProviderConfig& config, const std::string& key,
                      const std::string& fallback) {
  const auto it = config.options.find(key);
  return it == config.options.end() ? fallback : it->second;
}

std::chrono::seconds seconds_option(const md::ProviderConfig& config, const std::string& key,
                                    std::chrono::seconds fallback) {
  const auto it = config.options.find(key);
  return it == config.options.end() ? fallback
                                    : std::chrono::seconds(parse_integer(it->second, key, 1));
}

void validate_keys(const md::ProviderConfig& config,
                   std::initializer_list<std::string_view> allowed) {
  for (const auto& [key, value] : config.options) {
    if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
      throw std::invalid_argument(config.name + ": unknown provider option " + key);
  }
}

std::string environment(const char* name) {
  const auto* value = std::getenv(name);
  return value ? value : "";
}

bool sandbox_option(const md::ProviderConfig& config) {
  const auto value = option_or(config, "sandbox", "false");
  if (value != "true" && value != "false") throw std::invalid_argument("sandbox must be true or false");
  return value == "true";
}

}  // namespace

std::vector<std::string_view> provider_names() {
  return {
      "cboe",
#ifdef OPENPORT_WITH_DATABENTO
      "databento",
#endif
      "massive",
      "thetadata",
      "tradier",
      "tastytrade",
      "schwab",
      "replay",
      "demo",
  };
}

void validate_subscription(std::string_view provider, const md::Subscription& subscription) {
  if (subscription.max_expiries < 0 || !std::isfinite(subscription.strike_window) ||
      subscription.strike_window < 0 || subscription.strike_window > 1)
    throw std::invalid_argument("expiries must be >= 0 and window must be in [0, 1]");
  if (provider == "demo") {
    if (subscription.max_expiries != 0 || subscription.strike_window != 0)
      throw std::invalid_argument("demo: --expiries and --window must be zero");
    for (const auto& symbol : subscription.underlyings)
      if (symbol != "SPX" && symbol != "SPY" && symbol != "QQQ" && symbol != "XSP" &&
          symbol != "NDX" && symbol != "RUT" && symbol != "VIX")
        throw std::invalid_argument("demo: unsupported symbol " + symbol);
  }
  if (provider == "replay" &&
      (subscription.max_expiries != 0 || subscription.strike_window != 0))
    throw std::invalid_argument("replay: --expiries and --window must be zero; "
                                "the recording already contains the original subscription filters");
  if (provider == "databento" &&
      (subscription.max_expiries != 0 || subscription.strike_window != 0))
    throw std::invalid_argument(
        "databento: --expiries and --window must be zero; parent subscriptions "
        "stream the whole chain upstream and cannot reduce traffic with these filters");
}

std::unique_ptr<md::Provider> make_provider(const md::ProviderConfig& config) {
  if (config.name == "tradier") {
    validate_keys(config, {"sandbox", "poll_seconds", "option_size_unit"});
    TradierProvider::Options options;
    options.access_token = environment("TRADIER_ACCESS_TOKEN");
    options.sandbox = sandbox_option(config);
    options.poll_seconds = seconds_option(config, "poll_seconds", std::chrono::seconds(0));
    const auto unit = option_or(config, "option_size_unit", "contracts");
    if (unit != "contracts" && unit != "hundreds" && unit != "unknown")
      throw std::invalid_argument("tradier: option_size_unit must be contracts, hundreds or unknown");
    options.size_multiplier = unit == "contracts" ? 1 : unit == "hundreds" ? 100 : 0;
    return std::make_unique<TradierProvider>(std::move(options));
  }
  if (config.name == "tastytrade") {
    validate_keys(config, {"sandbox", "dxlink_time_unit"});
    TastytradeProvider::Options options;
    options.sandbox = sandbox_option(config);
    options.credentials = {environment("TASTYTRADE_CLIENT_SECRET"), environment("TASTYTRADE_REFRESH_TOKEN"),
                           environment("TASTYTRADE_CLIENT_ID")};
    const auto unit = option_or(config, "dxlink_time_unit", "milliseconds");
    if (unit != "unknown" && unit != "milliseconds")
      throw std::invalid_argument("tastytrade: dxlink_time_unit must be unknown or milliseconds");
    options.timestamps_in_milliseconds = unit == "milliseconds";
    return std::make_unique<TastytradeProvider>(std::move(options));
  }
  if (config.name == "schwab") {
    validate_keys(config, {"poll_seconds"});
    SchwabProvider::Options options;
    options.credentials = {environment("SCHWAB_CLIENT_ID"), environment("SCHWAB_CLIENT_SECRET"),
                           environment("SCHWAB_REFRESH_TOKEN")};
    options.poll_seconds = seconds_option(config, "poll_seconds", std::chrono::seconds(0));
    return std::make_unique<SchwabProvider>(std::move(options));
  }
  if (config.name == "demo") {
    validate_keys(config, {"days", "speed", "revision"});
    DemoProvider::Options options;
    options.speed = parse_integer(option_or(config, "speed", "1"), "demo speed", 1, 300);
    options.revision = parse_integer(option_or(config, "revision", std::to_string(kScenarioRevision)),
                                     "demo revision", 1, kScenarioRevision);
    if (const auto found = config.options.find("days"); found != config.options.end()) {
      std::size_t begin = 0;
      do {
        const auto end = found->second.find(',', begin);
        const auto id = found->second.substr(begin, end - begin);
        if (id.empty()) throw std::invalid_argument("demo: days must contain nonempty scenario ids");
        options.days.push_back(id);
        if (end == std::string::npos) break;
        begin = end + 1;
      } while (true);
    }
    return std::make_unique<DemoProvider>(std::move(options));
  }
  if (config.name == "replay") {
    validate_keys(config, {"file", "speed", "loop"});
    ReplayProvider::Options options;
    options.file = option_or(config, "file", "");
    if (options.file.empty()) throw std::invalid_argument("replay: --option file=PATH is required");
    const auto speed = option_or(config, "speed", "1");
    if (speed != "1" && speed != "10" && speed != "60" && speed != "max")
      throw std::invalid_argument("replay: speed must be 1, 10, 60 or max");
    options.speed = speed == "max" ? 0 : parse_integer(speed, "speed", 1);
    const auto loop = option_or(config, "loop", "off");
    if (loop != "on" && loop != "off")
      throw std::invalid_argument("replay: loop must be on or off");
    options.loop = loop == "on";
    return std::make_unique<ReplayProvider>(std::move(options));
  }
  if (config.name == "cboe") {
    validate_keys(config, {"poll_seconds"});
    CboeDelayedProvider::Options options;
    options.poll_interval = seconds_option(config, "poll_seconds", options.poll_interval);
    return std::make_unique<CboeDelayedProvider>(options);
  }
#ifdef OPENPORT_WITH_DATABENTO
  if (config.name == "databento") {
    validate_keys(config, {"quotes", "trades"});
    DatabentoProvider::Options options;
    options.api_key = config.api_key;
    const std::string quotes = option_or(config, "quotes", "cbbo-1s");
    if (quotes == "cbbo-1s") {
      options.quotes = DatabentoProvider::QuoteSchema::Cbbo1s;
    } else if (quotes == "cmbp-1") {
      options.quotes = DatabentoProvider::QuoteSchema::Cmbp1;
    } else {
      throw std::invalid_argument("databento: quotes must be cbbo-1s or cmbp-1, not " + quotes);
    }
    const auto trades = option_or(config, "trades", "on");
    if (trades != "on" && trades != "off")
      throw std::invalid_argument("databento: trades must be on or off");
    options.trades = trades == "on";
    return std::make_unique<DatabentoProvider>(std::move(options));
  }
#endif
  if (config.name == "massive") {
    validate_keys(config, {"base_url", "poll_seconds"});
    MassiveProvider::Options options;
    options.api_key = config.api_key;
    options.base_url = option_or(config, "base_url", options.base_url);
    options.poll_interval = seconds_option(config, "poll_seconds", options.poll_interval);
    return std::make_unique<MassiveProvider>(std::move(options));
  }
  if (config.name == "thetadata") {
    validate_keys(config, {"base_url", "poll_seconds"});
    ThetaDataProvider::Options options;
    options.base_url = option_or(config, "base_url", options.base_url);
    options.poll_interval = seconds_option(config, "poll_seconds", options.poll_interval);
    return std::make_unique<ThetaDataProvider>(std::move(options));
  }
  throw std::invalid_argument("unknown provider: " + config.name);
}

}  // namespace openport::providers
