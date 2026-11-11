#include "openport/net/websocket.hpp"

#include <openssl/ssl.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <filesystem>
#include <stdexcept>

#include "openport/net/http.hpp"

namespace openport::net {
namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ws = beast::websocket;
using tcp = asio::ip::tcp;

class TlsWebSocket final : public WebSocket {
 public:
  TlsWebSocket() {
    tls_.set_verify_mode(asio::ssl::verify_peer);
    tls_.set_default_verify_paths();
    for (const char* bundle : {"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt"}) {
      if (std::filesystem::exists(bundle)) {
        boost::system::error_code ignored;
        tls_.load_verify_file(bundle, ignored);
        break;
      }
    }
  }
  ~TlsWebSocket() override { close(); }

  void connect(std::string_view url, const std::atomic<bool>* cancel) override {
    close();
    cancel_ = cancel;
    check_cancelled();
    if (!url.starts_with("wss://")) throw std::runtime_error("DXLink requires a TLS WebSocket URL");
    const auto target = parse_url("https://" + std::string(url.substr(6)));
    if (!target || target->host.find('@') != std::string::npos || target->target.find('#') != std::string::npos)
      throw std::runtime_error("invalid DXLink URL");
    tcp::resolver resolver(io_);
    const auto endpoints = resolver.resolve(target->host, target->port);
    check_cancelled();
    stream_ = std::make_unique<Stream>(io_, tls_);
    auto& tls = stream_->next_layer();
    if (SSL_ctrl(tls.native_handle(), SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name,
                 const_cast<char*>(target->host.c_str())) != 1)
      throw std::runtime_error("TLS: cannot set SNI host name");
    tls.set_verify_callback(asio::ssl::host_name_verification(target->host));
    auto& layer = beast::get_lowest_layer(*stream_);
    layer.expires_after(std::chrono::seconds(15));
    run([&](auto done) {
      layer.async_connect(endpoints, [done](auto ec, const auto&) { done(ec); });
    });
    layer.expires_after(std::chrono::seconds(15));
    run([&](auto done) { tls.async_handshake(asio::ssl::stream_base::client, done); });
    layer.expires_never();
    stream_->set_option(ws::stream_base::timeout{std::chrono::seconds(15), std::chrono::seconds(60), false});
    stream_->set_option(ws::stream_base::decorator([](ws::request_type& request) {
      request.set(beast::http::field::user_agent, user_agent());
    }));
    stream_->read_message_max(16 * 1024 * 1024);
    stream_->text(true);
    run([&](auto done) { stream_->async_handshake(url_host(*target) + ':' + target->port, target->target, done); });
  }

  void send(std::string_view message) override {
    check_cancelled();
    run([&](auto done) {
      stream_->async_write(asio::buffer(message), [done](auto ec, std::size_t) { done(ec); });
    });
  }

  std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
    check_cancelled();
    if (!reading_) {
      reading_ = true;
      complete_ = false;
      stream_->async_read(buffer_, [this](auto ec, std::size_t) {
        read_error_ = ec;
        complete_ = true;
      });
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      io_.restart();
      io_.run_for(std::chrono::milliseconds(25));
      check_cancelled();
    } while (!complete_ && std::chrono::steady_clock::now() < deadline);
    if (!complete_) return std::nullopt;
    reading_ = false;
    if (read_error_) throw boost::system::system_error(read_error_);
    if (!stream_->got_text()) throw std::runtime_error("DXLink sent a non-text frame");
    auto text = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());
    return text;
  }

  void close() noexcept override {
    if (!stream_) return;
    try {
      // Beast re-arms its idle timer on every read and leaves it armed when a read
      // fails, so the drain below would wait it out (a minute) unless it is off.
      stream_->set_option(ws::stream_base::timeout{ws::stream_base::none(), ws::stream_base::none(), false});
    } catch (...) {
    }
    boost::system::error_code ignored;
    auto& socket = beast::get_lowest_layer(*stream_).socket();
    socket.cancel(ignored);
    socket.close(ignored);
    io_.restart();
    io_.run();  // drain handlers before destroying their stream and buffers
    stream_.reset();
    buffer_.consume(buffer_.size());
    reading_ = false;
    complete_ = false;
  }

 private:
  void check_cancelled() {
    if (cancel_ && cancel_->load()) {
      close();
      throw std::runtime_error("WebSocket cancelled");
    }
  }
  template <typename Start>
  void run(Start&& start) {
    check_cancelled();
    bool done = false;
    boost::system::error_code error;
    start([&](auto ec) { error = ec; done = true; });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!done) {
      io_.restart();
      io_.run_for(std::chrono::milliseconds(25));
      if ((cancel_ && cancel_->load()) || std::chrono::steady_clock::now() >= deadline) {
        close();
        throw std::runtime_error("WebSocket cancelled or timed out");
      }
    }
    if (error) throw boost::system::system_error(error);
  }
  using Stream = ws::stream<beast::ssl_stream<beast::tcp_stream>>;
  asio::io_context io_;
  asio::ssl::context tls_{asio::ssl::context::tls_client};
  std::unique_ptr<Stream> stream_;
  beast::flat_buffer buffer_;
  const std::atomic<bool>* cancel_ = nullptr;
  bool reading_ = false, complete_ = false;
  boost::system::error_code read_error_;
};
}  // namespace
std::unique_ptr<WebSocket> make_websocket() { return std::make_unique<TlsWebSocket>(); }
}  // namespace openport::net
