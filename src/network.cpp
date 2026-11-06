#include "sniper/network.hpp"
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdlib>
#include <deque>
#include <openssl/ssl.h>
namespace sniper {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
Url Url::parse(std::string_view value) {
  Url u;
  auto p = value.find("://");
  if (p == value.npos)
    throw std::invalid_argument("URL must include a scheme");
  u.scheme = value.substr(0, p);
  value.remove_prefix(p + 3);
  auto slash = value.find('/');
  auto auth = value.substr(0, slash);
  u.target = slash == value.npos ? "/" : std::string(value.substr(slash));
  if (auth.find('@') != auth.npos)
    throw std::invalid_argument("URL userinfo is unsupported");
  auto colon = auth.rfind(':');
  u.host = auth.substr(0, colon);
  if (colon != auth.npos)
    u.port = auth.substr(colon + 1);
  else
    u.port = (u.scheme == "http" || u.scheme == "ws") ? "80" : "443";
  if (u.host.empty() || u.port.empty() || u.target.empty() ||
      (u.scheme != "http" && u.scheme != "https" && u.scheme != "ws" &&
       u.scheme != "wss"))
    throw std::invalid_argument("unsupported URL");
  return u;
}
static std::optional<Url> proxy_for(const Url &u) {
  if (u.host == "localhost" || u.host == "127.0.0.1" || u.host == "::1")
    return {};
  auto p = std::getenv("HTTPS_PROXY");
  if (!p || !*p)
    p = std::getenv("https_proxy");
  if (!p || !*p)
    return {};
  auto proxy = Url::parse(p);
  if (proxy.scheme != "http")
    throw std::invalid_argument("HTTPS_PROXY must be an HTTP CONNECT proxy");
  return proxy;
}
HttpResult RestClient::request(std::string_view method,
                               std::string_view address, const Headers &headers,
                               std::string_view body, int timeout) const {
  auto u = Url::parse(address);
  if (u.scheme != "https" && u.scheme != "http")
    throw std::invalid_argument("REST needs HTTP(S)");
  if ((u.target == "/order" || u.target == "/orders") && method != "GET") {
    if (blocked_)
      ++*blocked_;
    throw std::runtime_error("order-capable REST path blocked: only the "
                             "guarded native HTTP/2 sender may submit orders");
  }
  net::io_context io;
  net::ssl::context ctx(net::ssl::context::tls_client);
  ctx.set_default_verify_paths();
  beast::ssl_stream<beast::tcp_stream> stream(io, ctx);
  stream.set_verify_mode(net::ssl::verify_peer);
  stream.set_verify_callback(net::ssl::host_name_verification(u.host));
  if (!SSL_set_tlsext_host_name(stream.native_handle(), u.host.c_str()))
    throw std::runtime_error("TLS SNI failed");
  tcp::resolver resolver(io);
  net::steady_timer timer(io);
  beast::flat_buffer buffer;
  http::request<http::string_body> req;
  http::response<http::string_body> response;
  http::request<http::empty_body> tunnel;
  http::response_parser<http::empty_body> tunnel_reply;
  tunnel_reply.skip(true);
  bool complete = false;
  beast::error_code failure;
  auto proxy = proxy_for(u);
  auto dest = proxy ? *proxy : u;
  auto finish = [&](beast::error_code ec) {
    if (complete)
      return;
    complete = true;
    failure = ec;
    timer.cancel();
    resolver.cancel();
    beast::error_code ignored;
    beast::get_lowest_layer(stream).socket().close(ignored);
  };
  req.version(11);
  req.method_string(method);
  req.target(u.target);
  req.set(http::field::host, u.host);
  req.set(http::field::user_agent, "sniper-native-cpp/1.0");
  req.set(http::field::accept, "application/json");
  for (auto &[k, v] : headers)
    req.set(k, v);
  req.body() = body;
  if (!body.empty())
    req.set(http::field::content_type, "application/json");
  req.prepare_payload();
  std::function<void()> exchange = [&] {
    if (u.scheme == "https")
      http::async_write(stream, req, [&](beast::error_code ec, std::size_t) {
        if (ec)
          return finish(ec);
        http::async_read(
            stream, buffer, response,
            [&](beast::error_code ec, std::size_t) { finish(ec); });
      });
    else
      http::async_write(beast::get_lowest_layer(stream), req,
                        [&](beast::error_code ec, std::size_t) {
                          if (ec)
                            return finish(ec);
                          http::async_read(beast::get_lowest_layer(stream),
                                           buffer, response,
                                           [&](beast::error_code ec,
                                               std::size_t) { finish(ec); });
                        });
  };
  std::function<void()> tls = [&] {
    if (u.scheme == "http") {
      exchange();
      return;
    }
    stream.async_handshake(net::ssl::stream_base::client,
                           [&](beast::error_code ec) {
                             if (ec)
                               return finish(ec);
                             exchange();
                           });
  };
  timer.expires_after(std::chrono::seconds(timeout));
  timer.async_wait([&](beast::error_code ec) {
    if (!ec)
      finish(net::error::timed_out);
  });
  resolver.async_resolve(
      dest.host, dest.port,
      [&](beast::error_code ec, tcp::resolver::results_type addresses) {
        if (ec)
          return finish(ec);
        beast::get_lowest_layer(stream).async_connect(
            addresses, [&](beast::error_code ec, const auto &) {
              if (ec)
                return finish(ec);
              if (!proxy) {
                tls();
                return;
              }
              tunnel.method(http::verb::connect);
              tunnel.version(11);
              tunnel.target(u.host + ":" + u.port);
              tunnel.set(http::field::host, u.host + ":" + u.port);
              http::async_write(
                  beast::get_lowest_layer(stream), tunnel,
                  [&](beast::error_code ec, std::size_t) {
                    if (ec)
                      return finish(ec);
                    http::async_read(
                        beast::get_lowest_layer(stream), buffer, tunnel_reply,
                        [&](beast::error_code ec, std::size_t) {
                          if (ec)
                            return finish(ec);
                          if (tunnel_reply.get().result_int() != 200)
                            return finish(net::error::connection_refused);
                          buffer.consume(buffer.size());
                          tls();
                        });
                  });
            });
      });
  io.run();
  if (failure)
    throw std::runtime_error("REST transport failed: " + failure.message());
  if (!complete)
    throw std::runtime_error("REST incomplete");
  return {static_cast<int>(response.result_int()), std::move(response.body())};
}
Json RestClient::get(std::string_view url, const Headers &headers,
                     int timeout) const {
  auto r = request("GET", url, headers, {}, timeout);
  if (r.status < 200 || r.status >= 300)
    throw std::runtime_error("REST status " + std::to_string(r.status));
  return r.json();
}
struct WebSocket::Impl : std::enable_shared_from_this<WebSocket::Impl> {
  using Socket = ws::stream<beast::ssl_stream<beast::tcp_stream>>;
  struct Pipe {
    Socket socket;
    beast::flat_buffer buffer;
    std::deque<std::shared_ptr<std::string>> queue;
    http::request<http::empty_body> tunnel;
    http::response_parser<http::empty_body> tunnel_reply;
    bool up = false, failed = false, writing = false;
    Pipe(net::io_context &io, net::ssl::context &ctx) : socket(io, ctx) {
      tunnel_reply.skip(true);
    }
  };
  net::io_context &io;
  net::ssl::context &ctx;
  Url url;
  Message message;
  State state;
  double silence, last_rx = 0, last_ping = 0;
  bool app_ping, up = false, stopped = false;
  unsigned retry = 0;
  tcp::resolver resolver;
  net::steady_timer retry_timer, watch;
  std::shared_ptr<Pipe> pipe;
  Impl(net::io_context &i, net::ssl::context &c, std::string address, Message m,
       State s, double timeout, bool ping)
      : io(i), ctx(c), url(Url::parse(address)), message(std::move(m)),
        state(std::move(s)), silence(timeout), app_ping(ping), resolver(i),
        retry_timer(i), watch(i) {}
  bool current(const std::shared_ptr<Pipe> &p) const {
    return !stopped && pipe == p && !p->failed;
  }
  void fail(const std::shared_ptr<Pipe> &p) {
    if (!current(p))
      return;
    p->failed = true;
    bool was_up = up;
    up = p->up = false;
    if (was_up)
      state(false);
    watch.cancel();
    resolver.cancel();
    beast::error_code ignored;
    beast::get_lowest_layer(p->socket).socket().close(ignored);
    p->queue.clear();
    auto self = shared_from_this();
    double delay = std::min(8., .25 * std::pow(2., std::min(retry++, 5u)));
    retry_timer.expires_after(std::chrono::milliseconds(
        static_cast<int>(delay * 1000 + retry % 7 * 13)));
    retry_timer.async_wait([self, p](beast::error_code ec) {
      if (!ec && !self->stopped)
        self->connect();
    });
  }
  void fail() {
    if (pipe)
      fail(pipe);
  }
  void connect() {
    if (stopped)
      return;
    auto p = std::make_shared<Pipe>(io, ctx);
    pipe = p;
    p->socket.read_message_max(1024 * 1024);
    p->socket.next_layer().set_verify_mode(net::ssl::verify_peer);
    p->socket.next_layer().set_verify_callback(
        net::ssl::host_name_verification(url.host));
    if (!SSL_set_tlsext_host_name(p->socket.next_layer().native_handle(),
                                  url.host.c_str()))
      return fail(p);
    std::optional<Url> proxy;
    try {
      proxy = proxy_for(url);
    } catch (...) {
      return fail(p);
    }
    auto dest = proxy ? *proxy : url;
    auto self = shared_from_this();
    watch.expires_after(std::chrono::seconds(10));
    watch.async_wait([self, p](beast::error_code ec) {
      if (!ec && self->current(p) && !p->up)
        self->fail(p);
    });
    resolver.async_resolve(
        dest.host, dest.port,
        [self, p, proxy](beast::error_code ec,
                         tcp::resolver::results_type endpoints) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p);
          beast::get_lowest_layer(p->socket).expires_after(
              std::chrono::seconds(10));
          beast::get_lowest_layer(p->socket).async_connect(
              endpoints, [self, p, proxy](beast::error_code ec, const auto &) {
                if (!self->current(p))
                  return;
                if (ec)
                  return self->fail(p);
                if (!proxy)
                  return self->tls(p);
                p->tunnel.method(http::verb::connect);
                p->tunnel.version(11);
                p->tunnel.target(self->url.host + ":" + self->url.port);
                p->tunnel.set(http::field::host,
                              self->url.host + ":" + self->url.port);
                http::async_write(
                    beast::get_lowest_layer(p->socket), p->tunnel,
                    [self, p](beast::error_code ec, std::size_t) {
                      if (!self->current(p))
                        return;
                      if (ec)
                        return self->fail(p);
                      http::async_read(
                          beast::get_lowest_layer(p->socket), p->buffer,
                          p->tunnel_reply,
                          [self, p](beast::error_code ec, std::size_t) {
                            if (!self->current(p))
                              return;
                            if (ec || p->tunnel_reply.get().result_int() != 200)
                              return self->fail(p);
                            p->buffer.consume(p->buffer.size());
                            self->tls(p);
                          });
                    });
              });
        });
  }
  void tls(const std::shared_ptr<Pipe> &p) {
    auto self = shared_from_this();
    p->socket.next_layer().async_handshake(
        net::ssl::stream_base::client, [self, p](beast::error_code ec) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p);
          beast::get_lowest_layer(p->socket).expires_never();
          p->socket.set_option(
              ws::stream_base::timeout::suggested(beast::role_type::client));
          p->socket.async_handshake(self->url.host, self->url.target,
                                    [self, p](beast::error_code ec) {
                                      if (!self->current(p))
                                        return;
                                      if (ec)
                                        return self->fail(p);
                                      self->watch.cancel();
                                      p->up = self->up = true;
                                      self->retry = 0;
                                      self->last_rx = mono_s();
                                      self->last_ping = 0;
                                      self->state(true);
                                      self->read(p);
                                      self->tick(p);
                                    });
        });
  }
  void read(const std::shared_ptr<Pipe> &p) {
    if (!current(p) || !p->up)
      return;
    auto self = shared_from_this();
    p->socket.async_read(
        p->buffer, [self, p](beast::error_code ec, std::size_t) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p);
          self->last_rx = mono_s();
          auto text = beast::buffers_to_string(p->buffer.data());
          p->buffer.consume(p->buffer.size());
          beast::error_code ignored;
          auto pending =
              beast::get_lowest_layer(p->socket).socket().available(ignored);
          try {
            self->message(text, static_cast<int>(pending / 4096));
          } catch (...) {
            return self->fail(p);
          }
          self->read(p);
        });
  }
  void send(std::string value) {
    auto p = pipe;
    if (!p || !current(p) || !p->up)
      return;
    p->queue.push_back(std::make_shared<std::string>(std::move(value)));
    if (p->queue.size() > 256)
      return fail(p);
    if (!p->writing)
      write(p);
  }
  void write(const std::shared_ptr<Pipe> &p) {
    if (!current(p) || !p->up || p->queue.empty()) {
      p->writing = false;
      return;
    }
    p->writing = true;
    p->socket.text(true);
    auto data = p->queue.front();
    auto self = shared_from_this();
    p->socket.async_write(net::buffer(*data),
                          [self, p, data](beast::error_code ec, std::size_t) {
                            if (!self->current(p))
                              return;
                            if (ec)
                              return self->fail(p);
                            if (!p->queue.empty())
                              p->queue.pop_front();
                            self->write(p);
                          });
  }
  void tick(const std::shared_ptr<Pipe> &p) {
    if (!current(p))
      return;
    watch.expires_after(std::chrono::seconds(1));
    auto self = shared_from_this();
    watch.async_wait([self, p](beast::error_code ec) {
      if (ec || !self->current(p))
        return;
      if (mono_s() - self->last_rx > self->silence)
        return self->fail(p);
      if (self->app_ping && mono_s() - self->last_ping >= 10) {
        self->last_ping = mono_s();
        self->send("PING");
      }
      self->tick(p);
    });
  }
  void stop() {
    stopped = true;
    up = false;
    retry_timer.cancel();
    watch.cancel();
    resolver.cancel();
    if (pipe) {
      pipe->up = false;
      beast::error_code ignored;
      beast::get_lowest_layer(pipe->socket).socket().close(ignored);
    }
  }
};
WebSocket::WebSocket(net::io_context &io, net::ssl::context &ctx,
                     std::string url, Message m, State s, double silence,
                     bool ping)
    : p_(std::make_shared<Impl>(io, ctx, std::move(url), std::move(m),
                                std::move(s), silence, ping)) {}
void WebSocket::start() { p_->connect(); }
void WebSocket::stop() { p_->stop(); }
void WebSocket::send(std::string s) { p_->send(std::move(s)); }
bool WebSocket::connected() const { return p_->up; }
void WebSocket::reconnect() { p_->fail(); }
} // namespace sniper
