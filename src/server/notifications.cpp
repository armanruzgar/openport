#include "openport/server/notifications.hpp"

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace openport::server {
namespace {
using nlohmann::json;
using trading::Money;
using namespace std::chrono_literals;
const std::set<std::string> kEvents = {"fill", "order_rejected", "floor", "rule_trip",
    "assignment", "exercise", "playbook_ready", "feed_stalled", "alert"};
[[noreturn]] void invalid() { throw std::runtime_error("Invalid notification configuration"); }
void keys(const json& value, const std::set<std::string>& allowed) {
  if (!value.is_object()) invalid();
  for (const auto& [key, item] : value.items()) if (!allowed.contains(key)) invalid();
}
std::set<std::string> filters(const json& value) {
  if (!value.is_array()) invalid();
  std::set<std::string> result;
  for (const auto& item : value) {
    const auto event = item.get<std::string>();
    if (!kEvents.contains(event) || !result.insert(event).second) invalid();
  }
  return result;
}
void settings(NotificationChannel& channel, const json& value) {
  if (value.contains("enabled")) channel.enabled = value.at("enabled").get<bool>();
  if (value.contains("events")) channel.events = filters(value.at("events"));
  if (value.contains("floor_distance")) channel.floor_distance = Money::parse(value.at("floor_distance").get<std::string>());
  if (channel.floor_distance < Money{}) invalid();
}
bool safe_id(const std::string& value) {
  return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}
json stamp(md::Timestamp time) { return time > 0 ? json(md::format_timestamp(time)) : json(nullptr); }
struct Request { std::string url, body; net::Headers headers{{"Content-Type", "application/json"}}; };
Request payload(const NotificationChannel& channel, const NotificationEvent& event) {
  Request out;
  out.url = channel.url;
  const auto text = "OpenPort paper / simulated · " + event.kind +
      (event.account.empty() ? "" : " · " + event.account) + "\n" + event.message +
      (event.time > 0 ? "\nMarket time: " + md::format_timestamp(event.time) : "");
  if (channel.type == "discord") {
    out.body = json{{"content", text}, {"allowed_mentions", {{"parse", json::array()}}}}.dump();
  } else if (channel.type == "telegram") {
    out.url = "https://api.telegram.org/bot" + channel.token + "/sendMessage";
    out.body = json{{"chat_id", channel.chat_id}, {"text", text}, {"link_preview_options", {{"is_disabled", true}}}}.dump();
  } else if (channel.type == "ntfy") {
    out.headers = {{"Content-Type", "text/plain; charset=utf-8"}};
    out.body = text;
  } else {
    out.body = json{{"event", event.kind}, {"account", event.account}, {"market_time", stamp(event.time)},
        {"message", text}, {"simulated", true}, {"details", event.details}}.dump();
  }
  return out;
}
std::chrono::milliseconds seconds(std::string_view text) {
  try {
    std::size_t used = 0;
    const double value = std::stod(std::string(text), &used);
    if (used == text.size() && std::isfinite(value) && value > 0)
      return std::chrono::milliseconds(static_cast<std::int64_t>(std::ceil(std::min(value, 86400.0) * 1000)));
  } catch (...) {}
  return 0ms;
}
std::string destination(const NotificationChannel& channel) {
  return channel.type == "telegram" ? "chat:" + channel.token + ":" + channel.chat_id : channel.url;
}
std::chrono::milliseconds interval(const NotificationChannel& channel) {
  // Telegram: <=1/s per chat, <=20/min in groups. Apply the group budget to all chats.
  // Discord limits are dynamic; space requests and also follow bucket headers.
  return channel.type == "telegram" ? 3100ms : channel.type == "discord" ? 2000ms : 1000ms;
}
}  // namespace

NotificationConfig parse_notification_config(std::string_view text) {
  try {
    const auto root = json::parse(text);
    keys(root, {"queue_capacity", "channels", "include_simulated"});
    NotificationConfig config;
    config.include_simulated = root.value("include_simulated", false);
    if (root.contains("queue_capacity") && !root.at("queue_capacity").is_number_integer()) invalid();
    config.capacity = root.value("queue_capacity", std::size_t{256});
    if (config.capacity < 1 || config.capacity > 10000) invalid();
    const auto& list = root.at("channels");
    if (!list.is_array() || list.size() > 32) invalid();
    std::set<std::string> ids;
    for (const auto& item : list) {
      keys(item, {"id", "type", "url", "token", "chat_id", "events", "floor_distance", "enabled"});
      NotificationChannel channel;
      channel.id = item.at("id").get<std::string>();
      channel.type = item.at("type").get<std::string>();
      channel.events = kEvents;
      settings(channel, item);
      if (!safe_id(channel.id) || !ids.insert(channel.id).second) invalid();
      if (channel.type == "telegram") {
        if (item.contains("url")) invalid();
        channel.token = item.at("token").get<std::string>();
        channel.chat_id = item.at("chat_id").get<std::string>();
        if (channel.token.empty() || channel.token.size() > 256 || channel.chat_id.empty() || channel.chat_id.size() > 128 ||
            channel.token.find_first_not_of("0123456789:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-") != std::string::npos) invalid();
      } else {
        if (channel.type != "webhook" && channel.type != "discord" && channel.type != "ntfy") invalid();
        if (item.contains("token") || item.contains("chat_id")) invalid();
        channel.url = item.at("url").get<std::string>();
        const auto url = net::parse_url(channel.url);
        if (!url || channel.url.size() > 2048 || channel.url.find_first_of("\r\n\t #@") != std::string::npos) invalid();
        if (channel.type == "discord" && (!url->tls || url->host != "discord.com" || url->port != "443" || !url->target.starts_with("/api/webhooks/"))) invalid();
      }
      config.channels.push_back(std::move(channel));
    }
    return config;
  } catch (...) { invalid(); }
}

NotificationConfig load_notification_config(const std::filesystem::path& file,
    const std::function<std::string(const char*)>& environment) {
  if (!file.empty()) {
    const int fd = ::open(file.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("Cannot open notification config file");
    struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 077) != 0)
      throw std::runtime_error("Notification config must be an owner-only regular file (chmod 600)");
    if (info.st_size > 65536) invalid();
    std::string contents;
    char buffer[4096];
    for (;;) {
      const auto count = ::read(fd, buffer, sizeof buffer);
      if (count < 0) throw std::runtime_error("Cannot read notification config file");
      if (count == 0) break;
      contents.append(buffer, static_cast<std::size_t>(count));
      if (contents.size() > 65536) invalid();
    }
    return parse_notification_config(contents);
  }
  const auto configured = environment("OPENPORT_NOTIFY_JSON");
  if (!configured.empty()) return parse_notification_config(configured);
  json list = json::array();
  for (const auto& [type, variable] : {std::pair{"webhook", "OPENPORT_NOTIFY_WEBHOOK_URL"},
      {"discord", "OPENPORT_NOTIFY_DISCORD_URL"}, {"ntfy", "OPENPORT_NOTIFY_NTFY_URL"}}) {
    const auto url = environment(variable);
    if (!url.empty()) list.push_back({{"id", type}, {"type", type}, {"url", url}});
  }
  const auto token = environment("OPENPORT_NOTIFY_TELEGRAM_TOKEN");
  const auto chat = environment("OPENPORT_NOTIFY_TELEGRAM_CHAT_ID");
  if (!token.empty() || !chat.empty()) list.push_back({{"id", "telegram"}, {"type", "telegram"}, {"token", token}, {"chat_id", chat}});
  return parse_notification_config(json{{"channels", list}}.dump());
}

Notifications::Notifications(NotificationConfig config, std::unique_ptr<net::HttpClient> http, Options options)
    : capacity_(config.capacity), include_simulated_(config.include_simulated), http_(std::move(http)), options_(std::move(options)) {
  if (!http_ || capacity_ == 0) invalid();
  for (auto& channel : config.channels) {
    Channel state;
    state.config = std::move(channel);
    channels_.push_back(std::move(state));
  }
  if (options_.worker) thread_ = std::thread([this] { run(); });
}
Notifications::~Notifications() { stop(); }
void Notifications::stop() {
  stopping_ = true;
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::uint64_t warning = 0;
  {
    const std::lock_guard lock(mutex_);
    for (const auto& pending : queue_) { ++channels_[pending.channel].dropped; ++dropped_; }
    queue_.clear();
    if (dropped_ != warned_) { warning = dropped_; warned_ = dropped_; }
  }
  if (warning) options_.warn(warning);
}
bool Notifications::enqueue(std::size_t channel, const NotificationEvent& event) {
  if (stopping_ || queue_.size() + in_flight_ >= capacity_) {
    ++dropped_;
    ++channels_[channel].dropped;
    return false;
  }
  queue_.push_back({channel, event, 0, {}});
  wake_.notify_one();
  return true;
}
void Notifications::publish(const NotificationEvent& event) {
  const std::lock_guard lock(mutex_);
  for (std::size_t i = 0; i < channels_.size(); ++i) {
    auto& channel = channels_[i];
    if (!channel.config.enabled || !channel.config.events.contains(event.kind)) continue;
    if (event.kind == "floor") {
      const auto& room = event.details.at("room");
      // Missing marks do not establish that the account left the warning band.
      if (room.is_null()) continue;
      const bool near = Money::parse(room.get<std::string>()) <= channel.config.floor_distance;
      auto& latched = channel.near_floor[event.account];
      const bool was = latched;
      latched = near;
      if (!near || was) continue;
    }
    enqueue(i, event);
  }
}
int Notifications::test(std::string_view channel) {
  const std::lock_guard lock(mutex_);
  for (std::size_t i = 0; i < channels_.size(); ++i) if (channels_[i].config.id == channel) {
    if (stopping_) return 503;
    if (!channels_[i].config.enabled) return 409;
    return enqueue(i, {"test", "", 0, "Test notification. No order was sent.", json::object()}) ? 202 : 429;
  }
  return 404;
}
bool Notifications::configure(std::string_view channel, const json& value) {
  const std::lock_guard lock(mutex_);
  try {
    keys(value, {"enabled", "events", "floor_distance"});
    for (std::size_t i = 0; i < channels_.size(); ++i) if (channels_[i].config.id == channel) {
      auto config = channels_[i].config;
      settings(config, value);
      channels_[i].config = std::move(config);
      channels_[i].near_floor.clear();
      // Pending messages follow the newly selected filters too. An in-flight request can finish.
      std::erase_if(queue_, [&](const auto& pending) {
        return pending.channel == i && (!channels_[i].config.enabled ||
            (pending.event.kind != "test" && !channels_[i].config.events.contains(pending.event.kind)));
      });
      return true;
    }
    return false;
  } catch (...) { invalid(); }
}
json Notifications::status() const {
  const std::lock_guard lock(mutex_);
  json list = json::array();
  for (const auto& channel : channels_) {
    const auto& config = channel.config;
    list.push_back({{"id", config.id}, {"type", config.type}, {"enabled", config.enabled}, {"events", config.events},
        {"floor_distance", config.floor_distance.str()}, {"delivered", channel.delivered}, {"failures", channel.failures},
        {"dropped", channel.dropped}, {"last_attempt", stamp(channel.last_attempt)}, {"last_delivery", stamp(channel.last_delivery)},
        {"last_error", channel.last_error.empty() ? json(nullptr) : json(channel.last_error)}});
  }
  return {{"enabled", !stopping_ && !channels_.empty()}, {"include_simulated", include_simulated_}, {"queue_depth", queue_.size() + in_flight_},
      {"queue_capacity", capacity_}, {"dropped", dropped_}, {"channels", list}};
}

bool Notifications::deliver_one() {
  Pending pending;
  NotificationChannel config;
  {
    const std::lock_guard lock(mutex_);
    if (stopping_) return false;
    const auto now = options_.clock();
    const auto ready = std::find_if(queue_.begin(), queue_.end(), [&](const Pending& item) {
      const auto& channel = channels_[item.channel];
      return item.due <= now && channel.next <= now && limits_[destination(channel.config)] <= now &&
          (channel.config.type != "telegram" || limits_["bot:" + channel.config.token] <= now) &&
          (channel.config.type != "discord" || limits_["discord-global"] <= now);
    });
    if (ready == queue_.end()) return false;
    pending = std::move(*ready);
    queue_.erase(ready);
    ++in_flight_;
    auto& channel = channels_[pending.channel];
    config = channel.config;
    channel.last_attempt = options_.wall_clock();
    channel.next = now + interval(config);
    limits_[destination(config)] = channel.next;
    if (config.type == "telegram") limits_["bot:" + config.token] = now + 35ms;
  }
  net::HttpResponse response;
  bool network_error = false;
  try {
    const auto request = payload(config, pending.event);
    response = http_->post(request.url, request.body, request.headers, 5s, &stopping_);
  } catch (...) { network_error = true; }
  auto delay = seconds(response.header("Retry-After"));
  bool success = !network_error && response.status >= 200 && response.status < 300;
  bool global = false;
  try {
    const auto body = json::parse(response.body);
    if (config.type == "telegram") {
      success = success && body.value("ok", false);
      if (body.contains("error_code")) response.status = body.at("error_code").get<int>();
      if (body.contains("parameters") && body.at("parameters").contains("retry_after"))
        delay = std::max(delay, seconds(body.at("parameters").at("retry_after").dump()));
    } else if (config.type == "discord") {
      if (body.contains("retry_after")) delay = std::max(delay, seconds(body.at("retry_after").dump()));
      global = body.value("global", false);
    }
  } catch (...) { if (config.type == "telegram") success = false; }
  if (config.type == "discord" && response.header("X-RateLimit-Remaining") == "0")
    delay = std::max(delay, seconds(response.header("X-RateLimit-Reset-After")));
  const std::lock_guard lock(mutex_);
  --in_flight_;
  auto& channel = channels_[pending.channel];
  const auto now = options_.clock();
  channel.next = std::max(channel.next, now + delay);
  limits_[destination(config)] = std::max(limits_[destination(config)], channel.next);
  if (global && config.type == "discord") limits_["discord-global"] = now + std::max(delay, 1000ms);
  if (config.type == "telegram" && response.status == 429)
    limits_["bot:" + config.token] = now + std::max(delay, 1000ms);
  if (success) {
    ++channel.delivered;
    channel.last_delivery = options_.wall_clock();
    channel.last_error.clear();
  } else {
    ++channel.failures;
    channel.last_error = network_error ? "NETWORK_ERROR" : response.status >= 200 && response.status < 300
        ? "INVALID_RESPONSE" : "HTTP_" + std::to_string(response.status);
    ++pending.attempts;
    const bool retry = network_error || response.status == 429 || response.status >= 500;
    if (retry && pending.attempts < 4 && !stopping_ && channel.config.enabled &&
        (pending.event.kind == "test" || channel.config.events.contains(pending.event.kind))) {
      pending.due = now + std::max(delay, std::chrono::milliseconds(1000 * (1 << (pending.attempts - 1))));
      queue_.push_back(std::move(pending));
    }
  }
  return true;
}
void Notifications::run() {
  while (!stopping_) {
    std::uint64_t warning = 0;
    {
      const std::lock_guard lock(mutex_);
      if (dropped_ != warned_) { warning = dropped_; warned_ = dropped_; }
    }
    if (warning) options_.warn(warning);
    if (!deliver_one()) {
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, 50ms);
    }
  }
}

void Notifications::remove_account(const std::string& account, bool discard_pending) {
  const std::lock_guard observation(observation_mutex_);
  seen_.erase(account);
  const std::lock_guard lock(mutex_);
  for (auto& channel : channels_) channel.near_floor.erase(account);
  if (discard_pending) std::erase_if(queue_, [&](const auto& pending) { return pending.event.account == account; });
}

void Notifications::observe(std::string_view account, const TradingView& view, bool history) {
  if (!view.snapshot) return;
  const std::lock_guard observation(observation_mutex_);
  const std::string id(account);
  auto& seen = seen_[id];
  const auto previous = history ? nullptr : seen.snapshot;
  const auto& current = *view.snapshot;
  const auto emit = [&](std::string kind, md::Timestamp time, std::string message, json details = json::object()) {
    publish({std::move(kind), id, time, std::move(message), std::move(details)});
  };
  if (previous && previous != view.snapshot) {
    for (std::size_t i = previous->recent_fills.size(); i < current.recent_fills.size(); ++i) {
      const auto& fill = current.recent_fills[i];
      emit("fill", fill.time, std::string(fill.side == trading::Side::Buy ? "Bought " : "Sold ") +
          std::to_string(fill.quantity) + " " + fill.symbol + " at $" + fill.price.str(),
          {{"id", std::to_string(fill.id)}, {"order_id", std::to_string(fill.order_id)}, {"price", fill.price.str()}, {"quantity", fill.quantity}});
    }
    const auto reject = [&](const trading::Order& order) {
      if (order.status == trading::OrderStatus::Rejected)
        emit("order_rejected", current.time, "Order " + std::to_string(order.id) + " rejected: " + std::string(trading::to_string(order.reason.code)),
            {{"order_id", std::to_string(order.id)}, {"reason", trading::to_string(order.reason.code)}});
    };
    for (std::size_t i = previous->recent_orders.size(); i < current.recent_orders.size(); ++i) reject(current.recent_orders[i]);
    for (const auto& order : previous->open_orders)
      if (order.id > 0 && order.id <= current.recent_orders.size()) reject(current.recent_orders[static_cast<std::size_t>(order.id - 1)]);
    if (current.risk.kill_latched && !previous->risk.kill_latched)
      emit("rule_trip", current.time, "Kill switch tripped; opening orders are disabled.");
    const auto limit = view.config.limits.max_daily_loss;
    if (current.risk.daily_loss > limit && (previous->risk.daily_loss <= limit ||
        previous->evaluation.day != current.evaluation.day ||
        (current.risk.kill_reason == "DAILY_LOSS" && previous->risk.kill_reason != "DAILY_LOSS")))
      emit("rule_trip", current.time, "Daily loss limit reached.", {{"daily_loss", current.risk.daily_loss.str()}});
    const auto& evaluation = current.evaluation;
    const bool plan_locked = evaluation.day_lock == trading::Reason::DAILY_LOSS_LIMIT &&
        previous->evaluation.day_lock != trading::Reason::DAILY_LOSS_LIMIT;
    const bool plan_failed = evaluation.status == trading::EvaluationStatus::Failed &&
        evaluation.decision_code == trading::Reason::DAILY_LOSS_LIMIT &&
        previous->evaluation.status != trading::EvaluationStatus::Failed;
    if (plan_locked || plan_failed) {
      json details{{"code", "DAILY_LOSS_LIMIT"}, {"action", plan_failed ? "fail" : "lock"}};
      if (const auto daily = trading::daily_loss_level(evaluation, view.config.rules, trading::plan_inputs(current)))
        details["level"] = daily->level.str();
      emit("rule_trip", current.time, plan_failed
          ? "Plan daily loss limit reached; the attempt failed."
          : "Plan daily loss limit reached; positions are closed and opening orders refused until the next trading day.",
          std::move(details));
    }
    std::map<std::string, bool> expiry_assignments;
    for (std::size_t i = previous->closures.size(); i < current.closures.size(); ++i) {
      const auto& closure = current.closures[i];
      if (closure.kind == trading::ClosureKind::Settlement) expiry_assignments[closure.symbol] = closure.quantity < 0;
      if (closure.kind == trading::ClosureKind::Exercise || closure.kind == trading::ClosureKind::Assignment)
        emit(closure.kind == trading::ClosureKind::Assignment ? "assignment" : "exercise", closure.time,
            closure.symbol + ": closed " + std::to_string(closure.quantity) + " contracts; intrinsic value $" + closure.price.str() + " per unit.");
    }
    // Each time an alert fires, wherever the terminal is.
    for (const auto& alert : current.alerts) {
      std::uint64_t before = 0;
      for (const auto& old : previous->alerts) if (old.id == alert.id) before = old.fired;
      if (alert.fired <= before) continue;
      const auto& c = alert.spec.condition;
      const auto watched = c.scope == trading::AlertScope::Account ? std::string("account") : c.scope == trading::AlertScope::Spread
          ? "spread" : c.symbol;
      emit("alert", alert.fired_at, (alert.spec.label.empty() ? "Alert " + std::to_string(alert.id) : alert.spec.label) + ": " +
          watched + " " + c.metric + (c.direction == trading::TriggerDirection::AtOrBelow ? " at or below " : " at or above ") +
          c.level.str() + (alert.value ? " (now " + alert.value->str() + ")" : std::string()),
          {{"alert_id", std::to_string(alert.id)}, {"metric", c.metric}, {"level", c.level.str()},
           {"value", alert.value ? json(alert.value->str()) : json(nullptr)}});
    }
    // Expiry deliveries use Settlement closures; stock fills identify the option's side.
    for (std::size_t i = previous->stock_fills.size(); i < current.stock_fills.size(); ++i) {
      const auto& fill = current.stock_fills[i];
      if (fill.buying_power_after && *fill.buying_power_after < trading::Money{})
        emit("alert", fill.time, "Buying power after delivery: $" + fill.buying_power_after->str() +
            ". Openings that use buying power are refused until the shortfall is resolved; closes that free buying power remain allowed.",
            {{"symbol", fill.option}, {"buying_power_after", fill.buying_power_after->str()}});
      if (fill.source != trading::StockSource::Delivery) continue;
      const auto assigned = expiry_assignments.find(fill.option);
      if (assigned == expiry_assignments.end()) continue;
      emit(assigned->second ? "assignment" : "exercise", fill.time, "Expiry delivery: " + fill.option + "; " +
          std::to_string(fill.shares) + " " + fill.symbol + " shares at $" + fill.price.str());
    }
  }
  if (previous) {
    const auto room = current.valuation_complete ? view.breach.room : std::nullopt;
    emit("floor", current.time, room ? "Floor room is $" + room->str() + "." : "Floor room unavailable.",
        {{"room", room ? json(room->str()) : json(nullptr)}, {"complete", view.breach.complete}});
  }
  if (view.playbooks_json != seen.playbooks) {
    std::set<std::string> stages;
    const auto publication = json::parse(view.playbooks_json, nullptr, false);
    if (publication.is_object() && publication.contains("staged")) {
      for (const auto& stage : publication.at("staged")) {
        const auto stage_id = stage.at("id").get<std::string>();
        stages.insert(stage_id);
        if (previous && !seen.stages.contains(stage_id))
          emit("playbook_ready", current.time, "Staged playbook order ready for review: " + stage.at("name").get<std::string>(), {{"stage_id", stage_id}});
      }
    }
    seen.stages = std::move(stages);
    seen.playbooks = view.playbooks_json;
  }
  seen.snapshot = view.snapshot;
}

}  // namespace openport::server
