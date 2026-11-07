#pragma once
#include "crypto.hpp"
#include <boost/asio/io_context.hpp>
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
};
struct H2Callbacks {
  std::function<void(std::string_view, std::int64_t)> written;
  std::function<void(std::string_view, const HttpResult &)> reply;
  std::function<void(std::string_view, std::string_view)> unknown;
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
  bool post(std::string id, std::string body, const Headers &headers);
  bool get_time();
};
} // namespace sniper
