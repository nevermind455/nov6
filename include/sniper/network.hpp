#pragma once
#include "crypto.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <functional>
#include <memory>
namespace sniper {
struct Url {
  std::string scheme, host, port, target;
  static Url parse(std::string_view);
  std::string authority(bool include_default_port = false) const;
};
std::optional<Url> connect_proxy_for(const Url &);
namespace transport {
// Apply and verify once after TCP connect, before CONNECT/TLS. Application
// watchdogs and heartbeats still decide readiness; kernel keepalive is extra.
boost::system::error_code
configure_tcp_socket(boost::asio::ip::tcp::socket &);
} // namespace transport
struct HttpResult {
  int status = 0;
  std::string body;
  Json json() const { return Json::parse(body); }
};
class RestClient {
  bool live_;
  std::atomic<std::uint64_t> *blocked_;

public:
  explicit RestClient(bool live = false,
                      std::atomic<std::uint64_t> *blocked = nullptr)
      : live_(live), blocked_(blocked) {}
  HttpResult request(std::string_view method, std::string_view url,
                     const Headers &headers = {}, std::string_view body = {},
                     int timeout = 10) const;
  Json get(std::string_view url, const Headers &headers = {},
           int timeout = 10) const;
};
class WebSocket {
  struct Impl;
  std::shared_ptr<Impl> p_;

public:
  // All methods and callbacks run on the supplied io_context's single thread.
  // WSS is required; the TLS context must outlive the client and its handlers.
  using Message = std::function<void(std::string_view, int)>;
  using State = std::function<void(bool)>;
  WebSocket(boost::asio::io_context &, boost::asio::ssl::context &,
            std::string url, Message, State, double silence = 25,
            bool app_ping = true);
  void start();
  void stop();
  void send(std::string);
  bool connected() const;
  void reconnect();
  // Why the connection last went down ("" while it never has), how many
  // times it has, and when it last came up (monotonic seconds, 0 if never).
  std::string last_failure() const;
  std::uint64_t drops() const;
  double up_since() const;
};
struct H2Callbacks {
  std::function<void(std::string_view, std::int64_t)> written;
  std::function<void(std::string_view, const HttpResult &)> reply;
  std::function<void(std::string_view, std::string_view)> unknown;
};
struct H2ConnectionInfo {
  std::uint64_t generation = 0;
  std::int64_t ready_ns = 0;
  bool tcp_no_delay = false, tcp_keep_alive = false;
};
class H2Client {
  struct Impl;
  std::shared_ptr<Impl> p_;

public:
  // As with WebSocket, use the supplied io_context's single thread. An uncertain
  // request is reported through unknown and is never retried by this client.
  H2Client(boost::asio::io_context &, boost::asio::ssl::context &, bool live,
           std::atomic<std::uint64_t> &blocked, H2Callbacks, int timeout_s = 10,
           std::string host = "clob.polymarket.com", std::string port = "443");
  void start();
  void stop();
  bool ready() const;
  // A snapshot of the currently negotiated session, not a new socket query.
  // Absence means no ready connection; generations change after reconnects.
  std::optional<H2ConnectionInfo> connection_info() const;
  bool post(std::string id, std::string body, const Headers &headers);
  // DELETE /order with this body: cancel one resting order. The reply (or an
  // unknown outcome) arrives through the same callbacks under `id`. A cancel
  // whose outcome is unknown is safe to send again.
  bool cancel(std::string id, std::string body, const Headers &headers);
  bool get_time();
};
} // namespace sniper
