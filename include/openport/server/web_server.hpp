#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <type_traits>

#include "openport/server/api.hpp"
#include "openport/server/web_policy.hpp"

namespace openport::server {

using AsyncApiHandler = std::function<void(const ApiRequest&, ApiCompletion)>;
using ApiHandler = std::function<ApiResponse(const ApiRequest&)>;

/// HTTP and WebSocket server for the web terminal.
///
/// - `/api/...` goes to the API handler.
/// - `/ws` upgrades to a WebSocket that receives every `broadcast()` message.
///   A client that falls behind gets the latest message rather than a backlog.
/// - Anything else is a file under `web_root`; unknown paths without an extension
///   fall back to index.html so the single-page app can route them.
class WebServer {
 public:
  /// `allowed_hosts` names the server may be addressed by besides IP addresses,
  /// localhost and the allowed origins' hosts (see host_allowed).
  /// `read_timeout` bounds each HTTP request read, including keep-alive idle time.
  /// API completion waits have no deadline; response writes get a fresh 60 seconds.
  WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
            AsyncApiHandler api, std::vector<std::string> allowed_origins = {}, std::string write_token = {},
            std::vector<std::string> allowed_hosts = {}, std::vector<NamedToken> tokens = {}, bool require_token = false,
            std::shared_ptr<Sandboxes> sandboxes = {}, std::string client_ip_header = {}, std::shared_ptr<TokenFile> token_file = {},
            std::chrono::steady_clock::duration read_timeout = std::chrono::seconds(60));
  template <class Handler> requires std::is_invocable_r_v<ApiResponse, Handler, const ApiRequest&>
  WebServer(std::string address, unsigned short port, std::filesystem::path web_root,
            Handler api, std::vector<std::string> allowed_origins = {}, std::string write_token = {},
            std::vector<std::string> allowed_hosts = {}, std::vector<NamedToken> tokens = {}, bool require_token = false,
            std::shared_ptr<Sandboxes> sandboxes = {}, std::string client_ip_header = {}, std::shared_ptr<TokenFile> token_file = {},
            std::chrono::steady_clock::duration read_timeout = std::chrono::seconds(60))
      : WebServer(std::move(address), port, std::move(web_root),
                  AsyncApiHandler([api = std::move(api)](const ApiRequest& request, ApiCompletion complete) {
                    complete(api(request));
                  }), std::move(allowed_origins), std::move(write_token), std::move(allowed_hosts), std::move(tokens), require_token, std::move(sandboxes), std::move(client_ip_header), std::move(token_file), read_timeout) {}

  ~WebServer();
  WebServer(const WebServer&) = delete;
  WebServer& operator=(const WebServer&) = delete;

  /// Binds once per instance. A second start throws, even after stop or failure.
  /// Throws if the port is taken.
  void start(int threads = 1);
  void stop();

  /// Sends `message` to every connected WebSocket client. Thread-safe.
  void broadcast(std::string message);

  /// Attach the local event publisher before start; /ws/events is opt-in and account-scoped.
  void set_event_stream(std::shared_ptr<Notifications> events);

  /// Replace the default memory-only signal secret store before start.
  void set_signal_secrets(std::shared_ptr<SignalSecrets> secrets);

  /// The bound port (useful when constructed with port 0).
  [[nodiscard]] unsigned short port() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace openport::server
