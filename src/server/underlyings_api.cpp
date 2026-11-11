#include "underlyings_api.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace openport::server {
namespace {
using nlohmann::json;

json subscription_json(const UnderlyingSubscription& subscription) {
  json underlyings = subscription.configured;
  for (const auto& symbol : subscription.added) underlyings.push_back(symbol);
  return json{{"underlyings", std::move(underlyings)}, {"configured", subscription.configured},
              {"added", subscription.added}, {"live", subscription.live},
              {"reason", subscription.live ? json(nullptr) : json(subscription.reason)}, {"limit", subscription.limit}};
}
}  // namespace

std::optional<std::string> underlying_symbol_of(std::string text) {
  for (auto& letter : text)
    if (letter >= 'a' && letter <= 'z') letter = static_cast<char>(letter - 'a' + 'A');
  const bool valid = !text.empty() && text.size() <= 16 && text.front() != '.' &&
      std::all_of(text.begin(), text.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.'; });
  if (!valid) return std::nullopt;
  return text;
}

ApiResponse underlyings_response(const MetricsSource& source) {
  return {200, subscription_json(source.underlying_subscription()).dump()};
}

bool underlying_write(const ApiRequest& request, MetricsSource& source, const ApiCompletion& complete) {
  constexpr std::string_view prefix = "/api/underlyings/";
  const auto question = request.target.find('?');
  const auto path = std::string_view(request.target).substr(0, question);
  const bool add = request.method == "POST" && path == "/api/underlyings";
  const bool remove = request.method == "DELETE" && path.starts_with(prefix) && path.size() > prefix.size() &&
                      path.find('/', prefix.size()) == std::string_view::npos;
  if (!add && !remove) return false;
  if (question != std::string::npos) {
    complete(api_error(400, "INVALID_REQUEST", "Underlying changes take no query parameters"));
    return true;
  }
  std::string named;
  if (add) {
    try {
      const auto body = json::parse(request.body);
      if (!body.is_object() || body.size() != 1 || !body.contains("symbol") || !body.at("symbol").is_string())
        throw std::invalid_argument("symbol");
      named = body.at("symbol").get<std::string>();
    } catch (...) {
      complete(api_error(400, "INVALID_REQUEST", R"(The body must be {"symbol": "NVDA"} and nothing else)"));
      return true;
    }
  } else {
    named = std::string(path.substr(prefix.size()));
  }
  const auto symbol = underlying_symbol_of(std::move(named));
  if (!symbol) {
    complete(api_error(400, "INVALID_REQUEST", "symbol must be 1 to 16 letters, digits and dots, such as NVDA or BRK.B"));
    return true;
  }
  // A source whose underlyings are fixed says why at once; the engine checks again.
  if (const auto subscription = source.underlying_subscription(); !subscription.live) {
    complete(api_error(409, "SUBSCRIPTION_FIXED", subscription.reason));
    return true;
  }
  const bool posted = source.post_underlying({*symbol, add}, [complete](UnderlyingReply reply) {
    if (!reply.code.empty()) {
      complete(api_error(reply.status, std::move(reply.code), std::move(reply.message)));
      return;
    }
    auto body = subscription_json(reply.subscription);
    body["message"] = std::move(reply.message);
    complete({reply.status, body.dump()});
  });
  if (!posted) {
    auto busy = api_error(503, "ENGINE_BUSY", "The engine is not taking underlying changes now; retry shortly");
    busy.retry_after = kInboxRetrySeconds;
    complete(std::move(busy));
  }
  return true;
}

}  // namespace openport::server
