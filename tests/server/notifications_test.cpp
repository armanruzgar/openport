#include <gtest/gtest.h>

#include <fstream>
#include <future>
#include <sys/stat.h>

#include "openport/server/api.hpp"
#include "openport/server/engine.hpp"
#include "openport/server/notifications.hpp"
#include "openport/server/sandboxes.hpp"
#include "openport/server/web_policy.hpp"
#include "support/contract_capture.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
using nlohmann::json;
using trading::Money;

class NotificationHttp final : public net::HttpClient {
 public:
  struct Call { std::string url, body; net::Headers headers; };
  std::vector<Call> calls;
  std::deque<net::HttpResponse> responses;
  bool fail = false;
  std::function<void(const std::atomic<bool>*)> wait;
  net::HttpResponse post(std::string_view url, std::string_view body, const net::Headers& headers,
      std::chrono::seconds, const std::atomic<bool>* cancel) override {
    calls.push_back({std::string(url), std::string(body), headers});
    if (wait) wait(cancel);
    if (fail) throw std::runtime_error("private-token https://private.example/secret");
    if (responses.empty()) return {200, R"({"ok":true})"};
    auto result = responses.front();
    responses.pop_front();
    return result;
  }
};
json channel(std::string type = "webhook", std::string id = "phone") {
  json result{{"id", id}, {"type", type}};
  if (type == "telegram") { result["token"] = "123:private-token"; result["chat_id"] = "private-chat"; }
  else result["url"] = type == "discord" ? "https://discord.com/api/webhooks/123/private-token" : "https://private.example/private-token";
  return result;
}
struct Harness {
  std::chrono::steady_clock::time_point now{};
  NotificationHttp* http;
  std::shared_ptr<server::Notifications> notifications;
  explicit Harness(json channels = json::array({channel()}), std::size_t capacity = 256, bool include_simulated = false) {
    auto client = std::make_unique<NotificationHttp>();
    http = client.get();
    server::Notifications::Options options;
    options.worker = false;
    options.warn = [](std::uint64_t) {};
    options.clock = [&] { return now; };
    options.wall_clock = [] { return md::kNanosPerSecond; };
    notifications = std::make_shared<server::Notifications>(server::parse_notification_config(
        json{{"channels", channels}, {"queue_capacity", capacity}, {"include_simulated", include_simulated}}.dump()), std::move(client), options);
  }
  void emit(std::string kind = "fill") { notifications->publish({std::move(kind), "main", md::kNanosPerSecond, "Bought 1 SPY at $4.20", {}}); }
  void advance(std::chrono::milliseconds duration = 10s) { now += duration; }
  void drain() { for (int i = 0; i < 50; ++i) { notifications->deliver_one(); advance(); } }
  json state() { return notifications->status(); }
};
TEST(Notifications, FormatsAllFourPayloadsWithoutCredentialsOrMentions) {
  for (const std::string type : {"webhook", "discord", "telegram", "ntfy"}) {
    Harness h(json::array({channel(type)}));
    h.emit();
    ASSERT_TRUE(h.notifications->deliver_one());
    ASSERT_EQ(h.http->calls.size(), 1U);
    const auto& call = h.http->calls.front();
    EXPECT_EQ(call.body.find("private-token"), std::string::npos);
    EXPECT_NE(call.body.find("paper / simulated"), std::string::npos);
    if (type == "ntfy") {
      EXPECT_EQ(call.headers.front().second, "text/plain; charset=utf-8");
      EXPECT_NE(call.body.find("Bought 1 SPY"), std::string::npos);
    } else {
      const auto body = json::parse(call.body);
      if (type == "webhook") {
        EXPECT_EQ(body.at("event"), "fill");
        EXPECT_EQ(body.at("account"), "main");
        EXPECT_EQ(body.at("market_time"), "1970-01-01T00:00:01.000Z");
        EXPECT_EQ(body.at("simulated"), true);
      } else if (type == "discord") {
        EXPECT_TRUE(body.at("allowed_mentions").at("parse").empty());
        EXPECT_TRUE(body.contains("content"));
      } else {
        EXPECT_EQ(call.url, "https://api.telegram.org/bot123:private-token/sendMessage");
        EXPECT_EQ(body.at("chat_id"), "private-chat");
        EXPECT_FALSE(body.contains("parse_mode"));
        EXPECT_TRUE(body.at("link_preview_options").at("is_disabled"));
      }
    }
    EXPECT_EQ(h.state()["channels"][0]["delivered"], 1);
    EXPECT_EQ(h.state()["channels"][0]["last_delivery"], "1970-01-01T00:00:01.000Z");
  }
}
TEST(Notifications, DeletingAnAccountDropsItsQueuedNotificationsOnly) {
  Harness h;
  h.notifications->publish({"fill", "deleted", md::kNanosPerSecond, "old fill", {}});
  h.notifications->publish({"fill", "remaining", md::kNanosPerSecond, "other fill", {}});
  h.notifications->remove_account("deleted");
  h.drain();
  ASSERT_EQ(h.http->calls.size(), 1U);
  EXPECT_EQ(json::parse(h.http->calls.front().body).at("account"), "remaining");
}

TEST(Notifications, FiltersPerChannelAndTestBypassesEventFilters) {
  auto first = channel(); first["events"] = {"rule_trip"};
  auto second = channel("ntfy", "off"); second["enabled"] = false;
  Harness h(json::array({first, second}));
  h.emit();
  EXPECT_FALSE(h.notifications->deliver_one());
  h.emit("rule_trip");
  EXPECT_TRUE(h.notifications->deliver_one());
  EXPECT_EQ(h.notifications->test("phone"), 202);
  EXPECT_EQ(h.notifications->test("off"), 409);
  EXPECT_EQ(h.notifications->test("missing"), 404);
  h.advance(); EXPECT_TRUE(h.notifications->deliver_one());
  EXPECT_EQ(json::parse(h.http->calls.back().body).at("event"), "test");
}
TEST(Notifications, RetriesWithExponentialBackoffAndStopsAfterFourAttempts) {
  Harness h;
  h.http->fail = true;
  h.emit();
  EXPECT_TRUE(h.notifications->deliver_one());
  h.advance(999ms); EXPECT_FALSE(h.notifications->deliver_one());
  h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
  h.advance(1999ms); EXPECT_FALSE(h.notifications->deliver_one());
  h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
  h.advance(3999ms); EXPECT_FALSE(h.notifications->deliver_one());
  h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
  h.advance(); EXPECT_FALSE(h.notifications->deliver_one());
  EXPECT_EQ(h.state()["channels"][0]["failures"], 4);
  EXPECT_EQ(h.state()["channels"][0]["last_error"], "NETWORK_ERROR");
  EXPECT_TRUE(h.state()["channels"][0]["last_delivery"].is_null());
}
TEST(Notifications, RetriesServerErrorsButNotAuthenticationOrRedirects) {
  for (const int status : {301, 401, 503}) {
    Harness h;
    h.http->responses.push_back({status, "private-token"});
    h.emit(); EXPECT_TRUE(h.notifications->deliver_one());
    h.advance();
    EXPECT_EQ(h.notifications->deliver_one(), status == 503);
    EXPECT_EQ(h.state()["channels"][0]["failures"], 1);
  }
}
TEST(Notifications, EnforcesProviderRatesAcrossDuplicateDestinations) {
  for (const std::string type : {"discord", "telegram", "webhook"}) {
    Harness h(json::array({channel(type), channel(type, "same")}));
    h.emit(); EXPECT_TRUE(h.notifications->deliver_one());
    const auto period = type == "telegram" ? 3100ms : type == "discord" ? 2000ms : 1000ms;
    h.advance(period - 1ms); EXPECT_FALSE(h.notifications->deliver_one());
    h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
  }
}
TEST(Notifications, TelegramBotBudgetIsSharedAcrossChats) {
  auto other = channel("telegram", "other"); other["chat_id"] = "different";
  Harness h(json::array({channel("telegram"), other}));
  h.emit(); EXPECT_TRUE(h.notifications->deliver_one());
  h.advance(34ms); EXPECT_FALSE(h.notifications->deliver_one());
  h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
}
TEST(Notifications, HonorsRetryAfterHeadersAndProviderBodies) {
  for (const std::string type : {"discord", "telegram", "webhook"}) {
    Harness h(json::array({channel(type)}));
    net::HttpResponse limited{429, type == "telegram" ? R"({"ok":false,"parameters":{"retry_after":12}})" : R"({"retry_after":12.0})"};
    limited.headers = {{"Retry-After", "10"}};
    h.http->responses.push_back(limited);
    h.emit(); EXPECT_TRUE(h.notifications->deliver_one());
    const auto wait = type == "webhook" ? 10s : 12s;
    h.advance(wait - 1ms); EXPECT_FALSE(h.notifications->deliver_one());
    h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
    EXPECT_EQ(h.state()["channels"][0]["failures"], 1);
    EXPECT_EQ(h.state()["channels"][0]["delivered"], 1);
  }
}
TEST(Notifications, DiscordEmptyBucketDelaysEvenAfterSuccessfulDelivery) {
  Harness h(json::array({channel("discord")}));
  net::HttpResponse response{204, ""};
  response.headers = {{"X-RateLimit-Remaining", "0"}, {"X-RateLimit-Reset-After", "9.5"}};
  h.http->responses.push_back(response);
  h.emit(); h.emit(); EXPECT_TRUE(h.notifications->deliver_one());
  h.advance(9499ms); EXPECT_FALSE(h.notifications->deliver_one());
  h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
}
TEST(Notifications, DiscordGlobalAndTelegram429BlockOtherChannelsForTheSameService) {
  for (const std::string type : {"discord", "telegram"}) {
    auto other = channel(type, "other");
    if (type == "discord") other["url"] = "https://discord.com/api/webhooks/456/other";
    else other["chat_id"] = "different";
    Harness h(json::array({channel(type), other}));
    h.http->responses.push_back({429, type == "discord" ? R"({"retry_after":10,"global":true})" : R"({"ok":false,"parameters":{"retry_after":10}})"});
    h.emit(); EXPECT_TRUE(h.notifications->deliver_one());
    h.advance(9999ms); EXPECT_FALSE(h.notifications->deliver_one());
    h.advance(1ms); EXPECT_TRUE(h.notifications->deliver_one());
  }
}
TEST(Notifications, OneServicesBackoffDoesNotDelayOtherDestinations) {
  Harness h(json::array({channel("discord"), channel("webhook", "other")}));
  h.http->responses.push_back({429, R"({"retry_after":10,"global":true})"});
  h.emit();
  EXPECT_TRUE(h.notifications->deliver_one());
  EXPECT_TRUE(h.notifications->deliver_one());
  ASSERT_EQ(h.http->calls.size(), 2U);
  EXPECT_EQ(json::parse(h.http->calls.back().body)["event"], "fill");
}

TEST(Notifications, QueueOverflowCountsDroppedDeliveries) {
  Harness h(json::array({channel()}), 2);
  h.emit(); h.emit(); h.emit();
  EXPECT_EQ(h.state()["queue_depth"], 2);
  EXPECT_EQ(h.state()["dropped"], 1);
  EXPECT_EQ(h.state()["channels"][0]["dropped"], 1);
  EXPECT_EQ(h.notifications->test("phone"), 429);
  EXPECT_EQ(h.state()["dropped"], 2);
}
TEST(Notifications, SettingsAreAtomicAndNeverReturnSecrets) {
  Harness h(json::array({channel("telegram")}));
  EXPECT_THROW(h.notifications->configure("phone", {{"enabled", false}, {"events", {"bogus"}}}), std::runtime_error);
  EXPECT_TRUE(h.state()["channels"][0]["enabled"]);
  EXPECT_THROW(h.notifications->configure("phone", {{"token", "new-secret"}}), std::runtime_error);
  EXPECT_TRUE(h.notifications->configure("phone", {{"events", {"exercise"}}, {"floor_distance", "123.45"}}));
  EXPECT_EQ(h.state()["channels"][0]["events"], json::array({"exercise"}));
  EXPECT_EQ(h.state()["channels"][0]["floor_distance"], "123.45");
  h.http->fail = true;
  EXPECT_EQ(h.notifications->test("phone"), 202); h.notifications->deliver_one();
  const auto status = h.state().dump();
  for (const std::string secret : {"private-token", "private-chat", "api.telegram.org", "https://"})
    EXPECT_EQ(status.find(secret), std::string::npos);
}
TEST(Notifications, DisablingOrFilteringDropsPendingRequests) {
  Harness h;
  h.emit();
  ASSERT_TRUE(h.notifications->configure("phone", {{"events", json::array()}}));
  EXPECT_EQ(h.state()["queue_depth"], 0);
  EXPECT_EQ(h.notifications->test("phone"), 202);
  ASSERT_TRUE(h.notifications->configure("phone", {{"enabled", false}}));
  EXPECT_FALSE(h.notifications->deliver_one());
}
TEST(Notifications, FilePermissionsAndErrorsNeverExposeInput) {
  test::RecordingFile file;
  const auto path = file.directory / "notifications.json";
  const auto no_env = [](const char*) { return std::string{}; };
  { std::ofstream output(path); output << json{{"channels", {channel("telegram")}}}; }
  ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
  EXPECT_THROW((void)server::load_notification_config(path, no_env), std::runtime_error);
  ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
  EXPECT_EQ(server::load_notification_config(path, no_env).channels.size(), 1U);
  const auto link = file.directory / "link";
  std::filesystem::create_symlink(path, link);
  EXPECT_THROW((void)server::load_notification_config(link, no_env), std::runtime_error);
  for (const std::string input : {"private-token", R"({"channels":[{"id":"private-token"}]})"}) {
    try { (void)server::parse_notification_config(input); FAIL() << "Expected rejection"; }
    catch (const std::exception& error) { EXPECT_EQ(std::string(error.what()).find("private-token"), std::string::npos); }
  }
}
TEST(Notifications, EnvironmentSupportsSimpleChannelsAndFullConfiguration) {
  std::map<std::string, std::string> env{{"OPENPORT_NOTIFY_DISCORD_URL", channel("discord")["url"]},
      {"OPENPORT_NOTIFY_TELEGRAM_TOKEN", "123:private-token"}, {"OPENPORT_NOTIFY_TELEGRAM_CHAT_ID", "private-chat"},
      {"OPENPORT_NOTIFY_NTFY_URL", "https://ntfy.example/topic"}, {"OPENPORT_NOTIFY_WEBHOOK_URL", "https://hook.example/hook"}};
  const auto read = [&](const char* key) { return env[key]; };
  EXPECT_EQ(server::load_notification_config({}, read).channels.size(), 4U);
  EXPECT_FALSE(server::load_notification_config({}, read).include_simulated);
  env["OPENPORT_NOTIFY_JSON"] = json{{"channels", {channel()}}, {"include_simulated", true}}.dump();
  EXPECT_EQ(server::load_notification_config({}, read).channels.size(), 1U);
  EXPECT_TRUE(server::load_notification_config({}, read).include_simulated);
  env["OPENPORT_NOTIFY_JSON"].clear(); env["OPENPORT_NOTIFY_TELEGRAM_CHAT_ID"].clear();
  EXPECT_THROW((void)server::load_notification_config({}, read), std::runtime_error);
}
TEST(Notifications, RejectsUnsafeOrAmbiguousConfiguration) {
  for (auto entry : {channel(), channel("discord"), channel("telegram")}) {
    entry["events"] = {"unknown"};
    EXPECT_THROW((void)server::parse_notification_config(json{{"channels", {entry}}}.dump()), std::runtime_error);
  }
  for (const std::string url : {"file:///topic", "https://user:secret@example.org/", "https://example.org/\r\nX:secret"}) {
    auto entry = channel(); entry["url"] = url;
    EXPECT_THROW((void)server::parse_notification_config(json{{"channels", {entry}}}.dump()), std::runtime_error);
  }
  EXPECT_THROW((void)server::parse_notification_config(json{{"channels", {channel(), channel()}}}.dump()), std::runtime_error);
  EXPECT_THROW((void)server::parse_notification_config(R"({"channels":[],"queue_capacity":0})"), std::runtime_error);
  for (const auto& value : {json(1), json("true"), json(nullptr)}) {
    EXPECT_THROW((void)server::parse_notification_config(json{{"channels", json::array()}, {"include_simulated", value}}.dump()), std::runtime_error);
  }
  for (const std::string type : {"webhook", "ntfy", "discord"}) {
    auto entry = channel(type); entry["url"] = "http://discord.com/api/webhooks/123/token";
    const auto config = json{{"channels", {entry}}}.dump();
    if (type == "discord") { EXPECT_THROW((void)server::parse_notification_config(config), std::runtime_error); }
    else { EXPECT_NO_THROW((void)server::parse_notification_config(config)); }
  }
}

server::TradingView view(std::shared_ptr<trading::TradingSnapshot> snapshot) {
  server::TradingView result; result.snapshot = std::move(snapshot); return result;
}
TEST(Notifications, ObservesNewAccountEventsOnceAndSkipsRecoveredHistory) {
  Harness h;
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  snapshot->time = md::kNanosPerSecond;
  trading::Fill old; old.id = 1; old.symbol = "SPY";
  snapshot->recent_fills.push_back(old);
  auto current = view(snapshot);
  h.notifications->observe("main", current);
  EXPECT_FALSE(h.notifications->deliver_one());
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  auto fill = old; fill.id = 2; fill.time = snapshot->time; fill.quantity = 1; fill.price = Money::parse("4.20");
  snapshot->recent_fills.push_back(fill);
  trading::Order order; order.id = 1; order.status = trading::OrderStatus::Rejected;
  order.reason.code = trading::Reason::KILL_SWITCH; snapshot->recent_orders.push_back(order);
  snapshot->risk.kill_latched = true;
  snapshot->risk.daily_loss = Money::parse("10001");
  snapshot->closures.push_back({"SPY", -1, Money::parse("500"), snapshot->time, trading::ClosureKind::Assignment});
  snapshot->closures.push_back({"QQQ", 1, Money::parse("400"), snapshot->time, trading::ClosureKind::Exercise});
  current = view(snapshot);
  current.playbooks_json = R"({"staged":[{"id":"one","name":"A setup"}]})";
  h.notifications->observe("main", current);
  h.notifications->observe("main", current);
  h.drain();
  std::multiset<std::string> kinds;
  for (const auto& call : h.http->calls) kinds.insert(json::parse(call.body).at("event").get<std::string>());
  EXPECT_EQ(kinds, (std::multiset<std::string>{"fill", "order_rejected", "rule_trip", "rule_trip", "assignment", "exercise", "playbook_ready"}));
}
TEST(Notifications, PlanDailyLossTripsNotifyOnceForLocksAndFailures) {
  for (const auto action : {trading::BreachAction::Lock, trading::BreachAction::Fail}) {
    Harness h;
    test::ScriptedMarket market;
    trading::SessionConfig config;
    config.initial_cash = Money::parse("10000");
    config.limits.aggregate = {1e9, 1e9};
    config.limits.per_underlying = {1e9, 1e9};
    config.rules.daily_loss_limit = Money::parse("200");
    config.rules.daily_loss_action = action;
    trading::TradingSession session(config, market.time);
    market.seed(session);
    const auto entry = session.submit(market.market("open", 5), market.time);
    ASSERT_TRUE(entry.decision.ok()) << entry.decision.message;
    server::TradingView current;
    current.config = config;
    current.snapshot = session.snapshot();
    h.notifications->observe("main", current);
    market.next();
    session.on_quotes({market.quote("3.70", "3.90")}, {market.valuation()}, market.time);
    current.snapshot = session.snapshot();
    const bool locked = action == trading::BreachAction::Lock;
    ASSERT_EQ(current.snapshot->evaluation.status, locked ? trading::EvaluationStatus::Active : trading::EvaluationStatus::Failed);
    ASSERT_EQ(locked ? current.snapshot->evaluation.day_lock : current.snapshot->evaluation.decision_code,
        trading::Reason::DAILY_LOSS_LIMIT);
    h.notifications->observe("main", current);
    h.notifications->observe("main", current);
    current.snapshot = std::make_shared<trading::TradingSnapshot>(*current.snapshot);
    h.notifications->observe("main", current);
    h.drain();
    std::vector<json> trips;
    for (const auto& call : h.http->calls) {
      auto body = json::parse(call.body);
      if (body.at("event") == "rule_trip") trips.push_back(std::move(body));
    }
    ASSERT_EQ(trips.size(), 1U);
    EXPECT_EQ(trips[0].at("account"), "main");
    EXPECT_EQ(trips[0].at("market_time"), md::format_timestamp(market.time));
    EXPECT_EQ(trips[0].at("details"), (json{{"code", "DAILY_LOSS_LIMIT"}, {"level", "9800.00"},
        {"action", locked ? "lock" : "fail"}}));
    const auto message = locked
        ? "Plan daily loss limit reached; positions are closed and opening orders refused until the next trading day."
        : "Plan daily loss limit reached; the attempt failed.";
    EXPECT_NE(trips[0].at("message").get<std::string>().find(message), std::string::npos);
    // Starting observation on a recovered lock or failure does not replay it.
    Harness recovered;
    recovered.notifications->observe("main", current);
    current.snapshot = std::make_shared<trading::TradingSnapshot>(*current.snapshot);
    recovered.notifications->observe("main", current);
    recovered.drain();
    EXPECT_TRUE(recovered.http->calls.empty());
  }
}
TEST(Notifications, PlanDailyLossLocksRearmOnANewDayAndOtherFailuresStayQuiet) {
  Harness h;
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  snapshot->evaluation.day = {2026, 9, 22};
  snapshot->evaluation.day_lock = trading::Reason::DAILY_LOSS_LIMIT;
  h.notifications->observe("main", view(snapshot));
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->evaluation.day = {2026, 9, 23};
  snapshot->evaluation.day_lock = trading::Reason::NONE;
  h.notifications->observe("main", view(snapshot));
  EXPECT_FALSE(h.notifications->deliver_one());
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->evaluation.day_lock = trading::Reason::DAILY_LOSS_LIMIT;
  h.notifications->observe("main", view(snapshot));
  h.drain();
  ASSERT_EQ(h.http->calls.size(), 1U);
  const auto body = json::parse(h.http->calls[0].body);
  EXPECT_EQ(body.at("event"), "rule_trip");
  EXPECT_EQ(body.at("details").at("action"), "lock");
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->evaluation.status = trading::EvaluationStatus::Failed;
  snapshot->evaluation.decision_code = trading::Reason::DRAWDOWN_FLOOR;
  h.notifications->observe("main", view(snapshot));
  h.drain();
  EXPECT_EQ(h.http->calls.size(), 1U);
}
TEST(Notifications, ForwardsEachAlertFiringOnce) {
  // F44: alerts the server keeps fire off-screen through the channels.
  Harness h;
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  trading::Alert alert;
  alert.id = 7;
  alert.spec.label = "Vol spike";
  alert.spec.condition = {trading::AlertScope::Underlying, "iv30", "SPX", {}, trading::TriggerDirection::AtOrAbove, Money::parse("25")};
  alert.fired = 1;  // fired before observation began: history
  snapshot->alerts.push_back(alert);
  h.notifications->observe("main", view(snapshot));
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->alerts[0].fired = 2;
  snapshot->alerts[0].value = Money::parse("26.5");
  auto added = alert;
  added.id = 8;
  added.fired = 0;
  snapshot->alerts.push_back(added);
  h.notifications->observe("main", view(snapshot));
  h.notifications->observe("main", view(snapshot));
  h.drain();
  ASSERT_EQ(h.http->calls.size(), 1U);
  const auto body = json::parse(h.http->calls[0].body);
  EXPECT_EQ(body["event"], "alert");
  EXPECT_EQ(body["details"]["alert_id"], "7");
  EXPECT_EQ(body["details"]["value"], "26.50");
  EXPECT_NE(body["message"].get<std::string>().find("Vol spike: SPX iv30 at or above 25.00 (now 26.50)"), std::string::npos);
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->alerts[1].fired = 1;  // a newly created alert can fire immediately
  h.notifications->observe("main", view(snapshot));
  h.drain();
  ASSERT_EQ(h.http->calls.size(), 2U);
  EXPECT_EQ(json::parse(h.http->calls[1].body)["details"]["alert_id"], "8");
  EXPECT_EQ(h.notifications->test("phone"), 202);
  h.drain();
  ASSERT_EQ(h.http->calls.size(), 3U);
  EXPECT_EQ(json::parse(h.http->calls[2].body)["event"], "test");
}
TEST(Notifications, AlertFilterUsesEveryExistingChannelWithoutNetwork) {
  for (const std::string type : {"webhook", "discord", "telegram", "ntfy"}) {
    auto config = channel(type);
    config["events"] = {"alert"};
    Harness h(json::array({config}));
    h.emit("fill");
    EXPECT_FALSE(h.notifications->deliver_one());
    h.emit("alert");
    EXPECT_TRUE(h.notifications->deliver_one());
    ASSERT_EQ(h.http->calls.size(), 1U);
    EXPECT_NE(h.http->calls[0].body.find("alert"), std::string::npos);
    EXPECT_EQ(h.notifications->test("phone"), 202);
    h.drain();
    ASSERT_EQ(h.http->calls.size(), 2U);
    EXPECT_NE(h.http->calls[1].body.find("test"), std::string::npos);
  }
}
TEST(Notifications, ExpiryDeliveriesDistinguishShortAssignmentFromLongExercise) {
  Harness h;
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  h.notifications->observe("main", view(snapshot));
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->closures.push_back({"put", -1, {}, 1, trading::ClosureKind::Settlement});
  snapshot->closures.push_back({"call", 1, {}, 1, trading::ClosureKind::Settlement});
  snapshot->stock_fills.push_back({1, "SPY", 100, {}, 1, trading::StockSource::Delivery, "put"});
  snapshot->stock_fills.push_back({2, "QQQ", 100, {}, 1, trading::StockSource::Delivery, "call"});
  snapshot->stock_fills.push_back({3, "IWM", 100, {}, 1, trading::StockSource::Delivery, "unknown"});
  h.notifications->observe("main", view(snapshot)); h.drain();
  ASSERT_EQ(h.http->calls.size(), 2U);
  EXPECT_EQ(json::parse(h.http->calls[0].body)["event"], "assignment");
  EXPECT_EQ(json::parse(h.http->calls[1].body)["event"], "exercise");
}
TEST(Notifications, FloorUsesBreachRoomPerChannelAndRearmsAfterRecovery) {
  auto narrow = channel("ntfy", "narrow"); narrow["floor_distance"] = "100";
  Harness h(json::array({channel(), narrow}));
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  auto current = view(snapshot);
  h.notifications->observe("main", current);
  current.breach.room = Money::parse("300");
  h.notifications->observe("main", current); h.notifications->observe("main", current);
  EXPECT_EQ(h.state()["queue_depth"], 1);
  current.breach.room = Money::parse("50");
  h.notifications->observe("main", current);
  EXPECT_EQ(h.state()["queue_depth"], 2);
  current.breach.room = Money::parse("600"); h.notifications->observe("main", current);
  current.breach.room = Money::parse("50"); h.notifications->observe("main", current);
  EXPECT_EQ(h.state()["queue_depth"], 4);
  current.breach.room.reset(); h.notifications->observe("main", current);
  EXPECT_EQ(h.state()["queue_depth"], 4);
}

TEST(Notifications, MissingValuationDoesNotInventFloorRoomOrRearmAnAlert) {
  Harness h;
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  auto current = view(snapshot);
  h.notifications->observe("main", current);
  current.breach.room = Money::parse("100");
  h.notifications->observe("main", current);
  EXPECT_EQ(h.state()["queue_depth"], 1);
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->valuation_complete = false; current.snapshot = snapshot;
  h.notifications->observe("main", current);
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->valuation_complete = true; current.snapshot = snapshot;
  h.notifications->observe("main", current);
  EXPECT_EQ(h.state()["queue_depth"], 1);
}
TEST(Notifications, ZeroDailyLossLimitStillTripsAndStagePriceUpdatesStayQuiet) {
  Harness h;
  auto snapshot = std::make_shared<trading::TradingSnapshot>();
  auto current = view(snapshot);
  current.config.limits.max_daily_loss = Money{};
  current.playbooks_json = R"({"staged":[{"id":"one","name":"A setup","net":"4.00"}]})";
  h.notifications->observe("main", current);
  snapshot = std::make_shared<trading::TradingSnapshot>(*snapshot);
  snapshot->risk.daily_loss = Money::parse("0.01"); current.snapshot = snapshot;
  current.playbooks_json = R"({"staged":[{"id":"one","name":"A setup","net":"4.20"}]})";
  h.notifications->observe("main", current); h.drain();
  ASSERT_EQ(h.http->calls.size(), 1U);
  EXPECT_EQ(json::parse(h.http->calls.front().body)["event"], "rule_trip");
}

class NotificationSource final : public server::MetricsSource {
 public:
  server::Notifications* service = nullptr;
  std::vector<std::string> symbols() const override { return {}; }
  std::shared_ptr<const analytics::UnderlyingMetrics> metrics(const std::string&) const override { return {}; }
  server::EngineStatus status() const override { return {}; }
  server::Notifications* notifications() const override { return service; }
};
server::ApiResponse request(NotificationSource& source, std::string method, std::string path, json body) {
  std::optional<server::ApiResponse> reply;
  server::handle_api_async({std::move(method), std::move(path), body.dump()}, source,
      [&](server::ApiResponse response) { reply = std::move(response); });
  if (!reply) throw std::runtime_error("No response");
  return *reply;
}
TEST(Notifications, ApiTestAndSettingsValidateAndRedact) {
  Harness h; NotificationSource source; source.service = h.notifications.get();
  const auto response = request(source, "POST", "/api/notifications/test", {{"channel", "phone"}});
  EXPECT_EQ(response.status, 202);
  test::capture_contract("notifications", "POST", "/api/notifications/test", response);
  EXPECT_EQ(request(source, "POST", "/api/notifications/test", {{"channel", "missing"}}).status, 404);
  EXPECT_EQ(request(source, "POST", "/api/notifications/test", {{"url", "private-token"}}).status, 400);
  const auto configured = request(source, "PUT", "/api/notifications/channels/phone", {{"events", {"fill"}}});
  EXPECT_EQ(configured.status, 200);
  test::capture_contract("notifications", "PUT", "/api/notifications/channels/phone", configured);
  const auto status = server::handle_api({"GET", "/api/status"}, source);
  test::capture_contract("notifications", "GET", "/api/status", status);
  EXPECT_EQ(json::parse(status.body)["notifications"]["channels"].size(), 1U);
  EXPECT_EQ(status.body.find("private-token"), std::string::npos);
  source.service = nullptr;
  EXPECT_EQ(request(source, "POST", "/api/notifications/test", {{"channel", "phone"}}).status, 503);
}

TEST(Notifications, SettingsAndTestRequireAdminScope) {
  server::WritePolicy policy;
  policy.require_token = true;
  policy.tokens = {{"trader", {"trade:*"}, "trade-token"}, {"admin", {"admin"}, "admin-token"}};
  for (const auto& [method, target] : {std::pair{"POST", "/api/notifications/test"},
       {"PUT", "/api/notifications/channels/phone"}}) {
    server::ApiRequest call{method, target, "{}"};
    call.host = "localhost"; call.content_type = "application/json";
    call.authorization = "Bearer trade-token";
    ASSERT_TRUE(server::check_api_write(call, policy));
    EXPECT_EQ(server::check_api_write(call, policy)->status, 403);
    call.authorization = "Bearer admin-token";
    EXPECT_FALSE(server::check_api_write(call, policy));
  }
}

class NotificationProvider final : public md::Provider {
 public:
  std::string provider_name = "manual";
  md::EventSink* sink = nullptr;
  std::string_view name() const noexcept override { return provider_name; }
  md::Capabilities capabilities() const noexcept override { return {}; }
  void start(const md::Subscription&, md::EventSink& out) override { sink = &out; }
  void stop() override {}
};
TEST(Notifications, LiveEngineObservesCommandsAndStalledFeedWithoutAnApiReader) {
  Harness h;
  NotificationProvider provider;
  test::ScriptedMarket market;
  std::atomic<md::Timestamp> now{market.time};
  test::RecordingFile file;
  server::Engine::Options options;
  options.paper_journal = file.directory / "paper.jsonl";
  options.analytics.fallback_rate = 0;
  options.notifications = h.notifications;
  options.clock = [&] { return now.load(); };
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  provider.sink->publish(md::ContractDefinition{0, market.contract});
  provider.sink->publish(md::UnderlyingQuote{"SPX", market.time, 5000, 5000, 5000});
  provider.sink->publish(md::OptionQuote{0, market.time, 4, 4.2, 20, 20});
  engine.synchronize().get();
  server::TradingCommand order; order.order = market.market("notify");
  std::promise<server::TradingReply> result;
  ASSERT_TRUE(engine.post_trading(order, [&](server::TradingReply reply) { result.set_value(std::move(reply)); }));
  const auto filled = result.get_future().get();
  ASSERT_TRUE(filled.decision.ok()) << filled.decision.message;
  order.order = market.market("invalid-notify", 0);
  std::promise<server::TradingReply> rejected;
  ASSERT_TRUE(engine.post_trading(order, [&](server::TradingReply reply) { rejected.set_value(std::move(reply)); }));
  EXPECT_FALSE(rejected.get_future().get().decision.ok());
  now += 61 * md::kNanosPerSecond;
  engine.synchronize().get();
  engine.synchronize().get();
  provider.sink->publish(md::ProviderStatus{now.load(), md::FeedState::Live, "recovered", "SPX"});
  engine.synchronize().get();
  now += 61 * md::kNanosPerSecond;
  engine.synchronize().get();
  engine.stop(); h.drain();
  // The same journal format recovers with delivery enabled; no channel data enters it.
  const auto journal = trading::FileJournal::read(options.paper_journal.string());
  ASSERT_FALSE(journal.records.empty());
  for (const auto& record : journal.records) {
    EXPECT_EQ(record.payload.find("private-token"), std::string::npos);
    EXPECT_EQ(record.payload.find("private.example"), std::string::npos);
    EXPECT_EQ(record.payload.find("notifications"), std::string::npos);
  }
  std::multiset<std::string> kinds;
  for (const auto& call : h.http->calls) kinds.insert(json::parse(call.body).at("event").get<std::string>());
  EXPECT_EQ(kinds, (std::multiset<std::string>{"fill", "order_rejected", "feed_stalled", "feed_stalled"}));
}
TEST(Notifications, ReplayDemoAndDrillEnginesDoNotNotifyByDefault) {
  for (const std::string name : {"manual", "replay", "demo"}) {
    SCOPED_TRACE(name);
    Harness h; NotificationProvider provider; provider.provider_name = name;
    server::Engine::Options options; options.notifications = h.notifications; options.replay = name == "manual";
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    EXPECT_EQ(engine.notifications(), nullptr);
    const auto response = server::handle_api({"GET", "/api/status"}, engine);
    ASSERT_EQ(response.status, 200);
    const auto status = json::parse(response.body).at("notifications");
    EXPECT_FALSE(status.at("enabled").get<bool>());
    EXPECT_FALSE(status.at("include_simulated").get<bool>());
    EXPECT_EQ(status.at("queue_capacity"), 256);
    ASSERT_EQ(status.at("channels").size(), 1U);
    EXPECT_EQ(status.at("channels")[0].at("id"), "phone");
    EXPECT_TRUE(status.at("channels")[0].at("enabled").get<bool>());
    EXPECT_EQ(response.body.find("private-token"), std::string::npos);
    EXPECT_EQ(response.body.find("private.example"), std::string::npos);
    std::optional<server::ApiResponse> test_response;
    server::handle_api_async({"POST", "/api/notifications/test", R"({"channel":"phone"})"}, engine,
        [&](auto reply) { test_response = std::move(reply); });
    ASSERT_TRUE(test_response);
    EXPECT_EQ(test_response->status, 503);
    server::TradingCommand trip; trip.kind = server::TradingCommand::Kind::Trip;
    trip.reason = "notification test";
    std::promise<server::TradingReply> done;
    ASSERT_TRUE(engine.post_trading(trip, [&](auto reply) { done.set_value(std::move(reply)); }));
    const auto reply = done.get_future().get();
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    engine.stop();
    EXPECT_FALSE(h.notifications->deliver_one());
    EXPECT_EQ(h.state()["queue_depth"], 0);
  }
}
TEST(Notifications, SimulatedOptInEnablesDemoAndReplayEngines) {
  for (const std::string name : {"demo", "replay"}) {
    SCOPED_TRACE(name);
    Harness h(json::array({channel()}), 256, true);
    NotificationProvider provider; provider.provider_name = name;
    server::Engine::Options options;
    options.notifications = h.notifications;
    server::Engine engine(provider, {{"SPX"}}, options);
    engine.start();
    ASSERT_EQ(engine.notifications(), h.notifications.get());
    const auto status = json::parse(server::handle_api({"GET", "/api/status"}, engine).body).at("notifications");
    EXPECT_TRUE(status.at("enabled").get<bool>());
    EXPECT_TRUE(status.at("include_simulated").get<bool>());
    ASSERT_EQ(status.at("channels").size(), 1U);
    EXPECT_EQ(status.at("channels")[0].at("id"), "phone");
    server::TradingCommand trip; trip.kind = server::TradingCommand::Kind::Trip;
    trip.reason = "notification test";
    std::promise<server::TradingReply> done;
    ASSERT_TRUE(engine.post_trading(trip, [&](auto reply) { done.set_value(std::move(reply)); }));
    const auto reply = done.get_future().get();
    ASSERT_TRUE(reply.decision.ok()) << reply.decision.message;
    engine.stop();
    h.drain();
    ASSERT_EQ(h.http->calls.size(), 1U);
    const auto body = json::parse(h.http->calls.front().body);
    EXPECT_EQ(body.at("event"), "rule_trip");
    EXPECT_EQ(body.at("account"), name == "demo" ? "demo/main" : "replay/main/main");
  }
}
TEST(Notifications, SandboxServerStaysSilentWithSimulatedOptIn) {
  Harness h(json::array({channel()}), 256, true);
  test::RecordingFile file;
  NotificationProvider provider; provider.provider_name = "demo";
  server::Engine::Options options;
  options.notifications = h.notifications;
  options.paper_journal = file.directory / "paper.jsonl";
  server::Sandboxes::Options sandboxes;
  sandboxes.capacity = 1;
  options.sandboxes = std::make_shared<server::Sandboxes>(sandboxes);
  server::Engine engine(provider, {{"SPX"}}, options);
  engine.start();
  EXPECT_EQ(engine.notifications(), nullptr);
  EXPECT_EQ(engine.configured_notifications(), nullptr);
  EXPECT_TRUE(json::parse(server::handle_api({"GET", "/api/status"}, engine).body).at("notifications").at("channels").empty());
  engine.stop();
  EXPECT_EQ(h.state().at("queue_depth"), 0);
}
TEST(Notifications, WorkerDoesNotHoldQueueMutexDuringHttpAndCancelsShutdown) {
  auto client = std::make_unique<NotificationHttp>();
  std::promise<void> entered;
  std::atomic<std::uint64_t> warning{0};
  server::Notifications::Options options;
  options.warn = [&](std::uint64_t dropped) { warning = dropped; };
  client->wait = [&](const std::atomic<bool>* cancelled) {
    entered.set_value();
    while (!cancelled->load()) std::this_thread::sleep_for(1ms);
  };
  server::Notifications service(server::parse_notification_config(json{{"channels", {channel()}}, {"queue_capacity", 1}}.dump()),
      std::move(client), options);
  EXPECT_EQ(service.test("phone"), 202);
  ASSERT_EQ(entered.get_future().wait_for(5min), std::future_status::ready);
  EXPECT_EQ(service.status()["queue_depth"], 1);
  EXPECT_EQ(service.test("phone"), 429);
  service.stop();
  EXPECT_EQ(warning.load(), 1U);
  EXPECT_EQ(service.test("phone"), 503);
}
}  // namespace
