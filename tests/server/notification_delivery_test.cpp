#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <future>
#include <mutex>
#include <thread>

#include "openport/server/notifications.hpp"
#include "openport/server/replay_host.hpp"
#include "openport/server/run.hpp"
#include "support/recording.hpp"
#include "support/scripted_market.hpp"

namespace {
using namespace openport;
using namespace std::chrono_literals;
using nlohmann::json;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using tcp = asio::ip::tcp;

// A real loopback receiver: no credentials or external service. All I/O is
// cancellable so a failing assertion cannot leave a blocked accept/read thread.
class NotificationReceiver {
 public:
  using Request = http::request<http::string_body>;
  NotificationReceiver() : listener_(io_, {asio::ip::make_address("127.0.0.1"), port()}) {
    accept();
    thread_ = std::thread([this] { io_.run(); });
  }
  ~NotificationReceiver() { io_.stop(); thread_.join(); }
  std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(listener_.local_endpoint().port()) + std::string(path);
  }
  std::vector<Request> requests() const { const std::lock_guard lock(mutex_); return requests_; }
 private:
  static unsigned short port() {
    const auto* selected = std::getenv("OPENPORT_TEST_NOTIFY_PORT");
    return selected ? static_cast<unsigned short>(std::stoi(selected)) : 0;
  }
  struct Connection {
    explicit Connection(asio::io_context& io) : socket(io) {}
    tcp::socket socket;
    boost::beast::flat_buffer buffer;
    Request request;
    http::response<http::string_body> response{http::status::ok, 11};
  };
  void accept() {
    auto connection = std::make_shared<Connection>(io_);
    listener_.async_accept(connection->socket, [this, connection](auto error) {
      if (error) return;
      accept();
      http::async_read(connection->socket, connection->buffer, connection->request,
          [this, connection](auto read_error, auto) {
        if (read_error) return;
        { const std::lock_guard lock(mutex_); requests_.push_back(connection->request); }
        connection->response.keep_alive(false);
        connection->response.body() = "ok";
        connection->response.prepare_payload();
        http::async_write(connection->socket, connection->response, [connection](auto, auto) {
          boost::system::error_code ignored;
          connection->socket.shutdown(tcp::socket::shutdown_both, ignored);
        });
      });
    });
  }
  asio::io_context io_;
  tcp::acceptor listener_;
  mutable std::mutex mutex_;
  std::vector<Request> requests_;
  std::thread thread_;
};

server::ApiResponse call(server::ReplayHost& host, std::string method, std::string target, json body = json::object()) {
  auto done = std::make_shared<std::promise<server::ApiResponse>>();
  auto result = done->get_future();
  server::ApiRequest request{std::move(method), std::move(target), body.dump()};
  request.content_type = "application/json";
  request.actor = "test";
  if (!host.handle(request, [done](auto response) { done->set_value(std::move(response)); }))
    throw std::runtime_error("Unhandled replay request");
  if (result.wait_for(30s) != std::future_status::ready) throw std::runtime_error("Replay request timed out");
  return result.get();
}

// Socket tests share the WebServer suite for the sandbox-safe test filter.
TEST(WebServer, ReplayAlertsReachWebhookAndNtfyOnlyWithOptInAndDoNotResendHistory) {
  NotificationReceiver receiver;
  for (const bool enabled : {false, true}) {
    SCOPED_TRACE(enabled);
    test::RecordingFile file;
    test::ScriptedMarket market;
    auto header = test::recording_header();
    header.provider = "demo";
    header.started = market.time;
    header.subscription = {{"SPX"}};
    header.capabilities = {};
    test::DiscardEvents discard;
    md::Timestamp receipt = market.time;
    md::RecordingSink::Options recording;
    recording.clock = [&] { return receipt; };
    {
      md::RecordingSink sink(file.path, header, discard, recording);
      sink.publish(md::ContractDefinition{0, market.contract});
      for (int tick = 0; tick < 5; ++tick) {
        receipt = market.time + tick * 15 * md::kNanosPerSecond;
        const double spot = tick % 2 == 0 ? 5000 : 5002;
        sink.publish(md::UnderlyingQuote{"SPX", receipt, spot, spot, spot});
        sink.publish(md::OptionQuote{0, receipt, 4, 4.2, 20, 20});
        sink.publish(md::SnapshotComplete{"SPX", receipt});
      }
      sink.close();
      ASSERT_TRUE(sink.error().empty()) << sink.error();
    }
    const auto channels = json::array({
        {{"id", "hook"}, {"type", "webhook"}, {"url", receiver.url("/hook")}, {"events", {"alert"}}},
        {{"id", "phone"}, {"type", "ntfy"}, {"url", receiver.url("/topic")}, {"events", {"alert"}}}});
    const auto config = server::parse_notification_config(
        json{{"include_simulated", enabled}, {"channels", channels}}.dump());
    auto service = std::make_shared<server::Notifications>(config, std::make_unique<net::HttpClient>(), server::Notifications::Options{});
    server::Engine::Options base;
    base.notifications = service;
    base.paper_journal = file.directory / "paper.jsonl";
    server::ReplayHost host({file.directory, base, true});
    const auto start = call(host, "POST", "/api/replay", {{"file", file.path.filename().string()}, {"paused", true}});
    ASSERT_EQ(start.status, 201) << start.body;
    const auto id = json::parse(start.body).at("replay").at("id").get<std::string>();
    const auto ready = [&] { return !json::parse(host.tick()).at("replay").at("fast_forwarding").get<bool>(); };
    ASSERT_TRUE(test::recording_eventually(ready));
    const auto status = json::parse(call(host, "GET", "/api/replay/status").body);
    EXPECT_EQ(status.at("notifications").at("enabled"), enabled);
    EXPECT_EQ(status.at("notifications").at("include_simulated"), enabled);
    const auto& configured = status.at("notifications").at("channels");
    ASSERT_EQ(configured.size(), 2U);
    EXPECT_EQ(configured[0].at("id"), "hook");
    EXPECT_EQ(configured[1].at("id"), "phone");
    EXPECT_TRUE(configured[0].at("enabled").get<bool>());
    EXPECT_TRUE(configured[1].at("enabled").get<bool>());
    const auto alert = call(host, "POST", "/api/replay/alerts", {{"scope", "underlying"}, {"symbol", "SPX"},
        {"metric", "price"}, {"direction", "at_or_above"}, {"level", "5001"}, {"label", "Local receiver"}, {"repeat", true}});
    ASSERT_EQ(alert.status, 201) << alert.body;
    ASSERT_EQ(call(host, "PUT", "/api/replay", {{"until", "10:00:15"}}).status, 200);
    EXPECT_EQ(json::parse(call(host, "GET", "/api/replay/alerts").body).at("alerts")[0].at("fired"), 1);
    const auto delivered = [&](std::size_t count) {
      return test::recording_eventually([&] {
        const auto state = service->status();
        return state.at("queue_depth") == 0 && state.at("channels")[0].at("delivered") == count &&
            state.at("channels")[1].at("delivered") == count;
      });
    };
    ASSERT_TRUE(delivered(enabled ? 1 : 0));
    ASSERT_EQ(receiver.requests().size(), enabled ? 2U : 0U);
    if (enabled) {
      const auto requests = receiver.requests();
      for (const auto& request : requests) {
        EXPECT_EQ(request.method(), http::verb::post);
        EXPECT_NE(request.body().find("paper / simulated"), std::string::npos);
        EXPECT_NE(request.body().find("Local receiver"), std::string::npos);
        EXPECT_NE(request.body().find("replay/" + id + "/main"), std::string::npos);
        if (request.target() == "/hook") {
          EXPECT_EQ(request[http::field::content_type], "application/json");
          const auto payload = json::parse(request.body());
          EXPECT_EQ(payload.at("event"), "alert");
          EXPECT_EQ(payload.at("market_time"), md::format_timestamp(market.time + 15 * md::kNanosPerSecond));
          EXPECT_EQ(payload.at("simulated"), true);
          EXPECT_EQ(payload.at("details").at("alert_id"), "1");
          EXPECT_EQ(payload.at("details").at("value"), "5002.00");
        } else {
          EXPECT_EQ(request.target(), "/topic");
          EXPECT_EQ(request[http::field::content_type], "text/plain; charset=utf-8");
        }
      }
    }
    // Re-executing the saved firing primes the observer without delivering it.
    const auto restart = call(host, "POST", "/api/replay", {{"restart", id}, {"at", "10:00:15"}});
    ASSERT_EQ(restart.status, 201) << restart.body;
    ASSERT_TRUE(test::recording_eventually(ready));
    EXPECT_EQ(service->status().at("queue_depth"), 0);
    ASSERT_EQ(receiver.requests().size(), enabled ? 2U : 0U);
    ASSERT_EQ(call(host, "PUT", "/api/replay", {{"until", "10:00:45"}}).status, 200);
    EXPECT_EQ(json::parse(call(host, "GET", "/api/replay/alerts").body).at("alerts")[0].at("fired"), 2);
    ASSERT_TRUE(delivered(enabled ? 2 : 0));
    EXPECT_EQ(receiver.requests().size(), enabled ? 4U : 0U);
    const auto restarted_id = json::parse(restart.body).at("replay").at("id").get<std::string>();
    host.stop();
    for (const auto& run : {id, restarted_id}) {
      const auto verified = server::verify_run(file.directory / "replays" / (run + ".jsonl"));
      EXPECT_TRUE(verified.matched) << verified.message;
    }
    service->stop();
  }
}
}  // namespace
