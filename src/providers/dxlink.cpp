#include "openport/providers/dxlink.hpp"

#include <algorithm>
#include <stdexcept>

#include "broker_json.hpp"

namespace openport::providers {
using broker::Json;

Json DxlinkProtocol::event_fields() {
  return {{"Quote", {"eventType", "eventSymbol", "bidPrice", "askPrice", "bidSize", "askSize", "bidTime", "askTime"}},
          {"Greeks", {"eventType", "eventSymbol", "volatility", "delta", "gamma", "theta", "rho", "vega", "time"}},
          {"Summary", {"eventType", "eventSymbol", "openInterest", "eventTime"}},
          {"Trade", {"eventType", "eventSymbol", "price", "dayVolume", "size", "time", "dayId"}}};
}

std::chrono::seconds DxlinkProtocol::backoff(unsigned attempt) {
  return std::chrono::seconds(std::min(60u, 1u << std::min(attempt, 6u)));
}

void DxlinkProtocol::send(Json message, md::Timestamp now) {
  transport_.send(message.dump());
  sent_ = now;
}

void DxlinkProtocol::begin(std::string token, std::set<DxlinkSubscription> subscriptions, md::Timestamp now) {
  if (subscriptions.size() > 25000) throw std::runtime_error("DXLink: exceeds 25000 event subscriptions; use expiry/strike filters");
  token_ = std::move(token);
  desired_ = std::move(subscriptions);
  active_.clear();
  // Keep the rolling change budget across reconnects on this protocol instance.
  fields_ = event_fields();
  auth_sent_ = authorized_ = opened_ = ready_ = false;
  heartbeat_ = 30 * md::kNanosPerSecond;
  received_ = action_ = now;
  send({{"type", "SETUP"}, {"channel", 0}, {"version", "0.1-openport/1.0"},
        {"keepaliveTimeout", 60}, {"acceptKeepaliveTimeout", 60}}, now);
}

std::vector<Json> DxlinkProtocol::on_message(std::string_view message, md::Timestamp now) {
  const auto doc = broker::parse(message);
  const auto type = broker::str(broker::field(doc, "type"));
  const double channel = broker::number(broker::field(doc, "channel"), -1);
  if (channel != 0 && channel != 3) return {};
  received_ = now;
  if (type == "ERROR" || type == "CHANNEL_CLOSED") throw std::runtime_error("DXLink: server rejected or closed the feed");
  if (type == "SETUP" && channel == 0) {
    const double timeout = broker::number(broker::field(doc, "keepaliveTimeout"), 60);
    if (timeout < 2 || timeout > 3600) throw std::runtime_error("DXLink: invalid keepalive timeout");
    heartbeat_ = static_cast<md::Timestamp>(std::min(30.0, timeout / 2) * 1e9);
  } else if (type == "AUTH_STATE" && channel == 0) {
    const auto state = broker::str(broker::field(doc, "state"));
    if (state == "UNAUTHORIZED") {
      if (auth_sent_ || authorized_) throw std::runtime_error("DXLink: quote token rejected; refresh required");
      send({{"type", "AUTH"}, {"channel", 0}, {"token", token_}}, now);
      auth_sent_ = true;
      action_ = now;
    } else if (state == "AUTHORIZED" && !authorized_) {
      authorized_ = true;
      token_.clear();
      action_ = now;
      send({{"type", "CHANNEL_REQUEST"}, {"channel", 3}, {"service", "FEED"},
            {"parameters", {{"contract", "AUTO"}}}}, now);
    }
  } else if (type == "CHANNEL_OPENED" && channel == 3 && authorized_ && !opened_) {
    opened_ = true;
    action_ = now;
    send({{"type", "FEED_SETUP"}, {"channel", 3}, {"acceptAggregationPeriod", 0.1},
          {"acceptDataFormat", "COMPACT"}, {"acceptEventFields", fields_}}, now);
  } else if (type == "FEED_CONFIG" && channel == 3 && opened_) {
    const auto format = broker::str(broker::field(doc, "dataFormat"));
    if (!format.empty() && format != "COMPACT") throw std::runtime_error("DXLink: COMPACT format required");
    const auto& fields = broker::field(doc, "eventFields");
    if (!fields.is_null()) {
      if (!fields.is_object()) throw std::runtime_error("DXLink: invalid accepted fields");
      for (const auto& [name, order] : fields.items()) {
        if (!order.is_array() || order.empty()) throw std::runtime_error("DXLink: empty accepted fields");
        std::set<std::string> unique;
        for (const auto& field : order) {
          if (!field.is_string() || !unique.insert(field.get<std::string>()).second)
            throw std::runtime_error("DXLink: invalid accepted field name");
        }
        fields_[name] = order;
      }
    }
    if (!ready_) {
      ready_ = true;
      send({{"type", "FEED_SUBSCRIPTION"}, {"channel", 3}, {"reset", true}}, now);
      flush(now);
    }
  } else if (type == "FEED_DATA" && channel == 3 && ready_) {
    const auto& data = broker::field(doc, "data");
    if (!data.is_array() || data.size() % 2 != 0) throw std::runtime_error("DXLink: invalid COMPACT pairs");
    std::vector<Json> result;
    for (std::size_t i = 0; i < data.size(); i += 2) {
      const auto event_type = broker::str(data[i]);
      const auto it = fields_.find(event_type);
      if (it == fields_.end()) throw std::runtime_error("DXLink: unknown COMPACT field order");
      const auto& values = data[i + 1];
      if (!values.is_array() || values.size() % it->size() != 0)
        throw std::runtime_error("DXLink: truncated COMPACT row");
      for (std::size_t row = 0; row < values.size(); row += it->size()) {
        Json event = Json::object();
        for (std::size_t col = 0; col < it->size(); ++col) event[(*it)[col].get<std::string>()] = values[row + col];
        if (event.contains("eventType") && broker::str(event["eventType"]) != event_type)
          throw std::runtime_error("DXLink: mismatched COMPACT event type");
        event["eventType"] = event_type;
        result.push_back(std::move(event));
      }
    }
    return result;
  }
  return {};
}

void DxlinkProtocol::replace(std::set<DxlinkSubscription> subscriptions, md::Timestamp now) {
  if (subscriptions.size() > 25000) throw std::runtime_error("DXLink: exceeds 25000 event subscriptions");
  desired_ = std::move(subscriptions);
  if (ready_) flush(now);
}

void DxlinkProtocol::flush(md::Timestamp now) {
  while (!changes_.empty() && changes_.front().first <= now - md::kNanosPerMinute) changes_.pop_front();
  std::size_t count = 0;
  for (const auto& change : changes_) count += change.second;
  const auto budget = count >= 10000 ? 0 : 10000 - count;
  std::size_t used = 0;
  Json removed = Json::array(), added = Json::array();
  for (auto it = active_.begin(); it != active_.end() && used < budget;) {
    if (!desired_.contains(*it)) {
      removed.push_back({{"type", it->type}, {"symbol", it->symbol}});
      it = active_.erase(it);
      ++used;
    } else ++it;
  }
  for (const auto& subscription : desired_) {
    if (used >= budget) break;
    if (!active_.contains(subscription)) {
      added.push_back({{"type", subscription.type}, {"symbol", subscription.symbol}});
      active_.insert(subscription);
      ++used;
    }
  }
  if (used > 0) {
    send({{"type", "FEED_SUBSCRIPTION"}, {"channel", 3}, {"remove", removed}, {"add", added}}, now);
    changes_.emplace_back(now, used);
  }
}

void DxlinkProtocol::tick(md::Timestamp now) {
  if (now - received_ >= 60 * md::kNanosPerSecond) throw std::runtime_error("DXLink: receive timeout");
  if (!ready_ && now - action_ >= 15 * md::kNanosPerSecond) throw std::runtime_error("DXLink: handshake timeout");
  if (now - sent_ >= heartbeat_) send({{"type", "KEEPALIVE"}, {"channel", 0}}, now);
  if (ready_) flush(now);
}
}  // namespace openport::providers
