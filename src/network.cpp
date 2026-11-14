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
#include <cctype>
#include <openssl/ssl.h>
namespace sniper {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ws = beast::websocket;
using tcp = net::ip::tcp;
boost::system::error_code transport::configure_tcp_socket(tcp::socket &socket) {
  beast::error_code ec;
  socket.set_option(tcp::no_delay(true), ec);
  if (ec)
    return ec;
  socket.set_option(net::socket_base::keep_alive(true), ec);
  if (ec)
    return ec;
  tcp::no_delay no_delay;
  net::socket_base::keep_alive keep_alive;
  socket.get_option(no_delay, ec);
  if (ec)
    return ec;
  socket.get_option(keep_alive, ec);
  if (ec)
    return ec;
  if (!no_delay.value() || !keep_alive.value())
    return net::error::operation_not_supported;
  return {};
}
Url Url::parse(std::string_view value) {
  Url u;
  auto p = value.find("://");
  if (p == value.npos)
    throw std::invalid_argument("URL must include a scheme");
  u.scheme = value.substr(0, p);
  std::transform(u.scheme.begin(), u.scheme.end(), u.scheme.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  value.remove_prefix(p + 3);
  for (unsigned char c : value)
    if (c <= 32 || c == 127)
      throw std::invalid_argument("URL contains whitespace or control bytes");
  auto end = value.find_first_of("/?#");
  auto auth = value.substr(0, end);
  auto target = end == value.npos ? std::string_view{} : value.substr(end);
  target = target.substr(0, target.find('#'));
  u.target = target.empty() ? "/" : std::string(target);
  if (u.target.front() == '?')
    u.target.insert(u.target.begin(), '/');
  if (auth.find('@') != auth.npos)
    throw std::invalid_argument("URL userinfo is unsupported");
  bool explicit_port = false;
  if (!auth.empty() && auth.front() == '[') {
    auto close = auth.find(']');
    if (close == auth.npos)
      throw std::invalid_argument("invalid bracketed URL host");
    u.host = auth.substr(1, close - 1);
    beast::error_code ec;
    net::ip::make_address_v6(u.host, ec);
    if (ec)
      throw std::invalid_argument("invalid IPv6 URL host");
    if (close + 1 < auth.size()) {
      if (auth[close + 1] != ':')
        throw std::invalid_argument("invalid URL authority");
      explicit_port = true;
      u.port = auth.substr(close + 2);
    }
  } else {
    auto colon = auth.find(':');
    u.host = auth.substr(0, colon);
    for (unsigned char c : u.host)
      if (!std::isalnum(c) && c != '.' && c != '-' && c != '_')
        throw std::invalid_argument("invalid URL host");
    if (colon != auth.npos) {
      explicit_port = true;
      u.port = auth.substr(colon + 1);
    }
  }
  if (!explicit_port)
    u.port = (u.scheme == "http" || u.scheme == "ws") ? "80" : "443";
  if (u.host.empty() || u.port.empty() || u.target.empty() ||
      (u.scheme != "http" && u.scheme != "https" && u.scheme != "ws" &&
       u.scheme != "wss"))
    throw std::invalid_argument("unsupported URL");
  unsigned port = 0;
  for (unsigned char c : u.port) {
    if (!std::isdigit(c) || port > 6553)
      throw std::invalid_argument("invalid URL port");
    port = port * 10 + c - '0';
  }
  if (port == 0 || port > 65535)
    throw std::invalid_argument("invalid URL port");
  return u;
}
std::string Url::authority(bool include_default_port) const {
  auto name = host.find(':') == host.npos ? host : "[" + host + "]";
  auto default_port = scheme == "http" || scheme == "ws" ? "80" : "443";
  return name + (include_default_port || port != default_port ? ":" + port : "");
}
std::optional<Url> connect_proxy_for(const Url &u) {
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
static bool order_path(std::string_view target) {
  target = target.substr(0, target.find('?'));
  std::string decoded;
  auto hex_digit = [](unsigned char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t n = 0; n < target.size(); ++n) {
    unsigned char c = target[n];
    if (c == '%') {
      if (n + 2 >= target.size() || hex_digit(target[n + 1]) < 0 ||
          hex_digit(target[n + 2]) < 0)
        throw std::invalid_argument("invalid URL path escape");
      c = hex_digit(target[n + 1]) * 16 + hex_digit(target[n + 2]);
      n += 2;
    }
    decoded += static_cast<char>(std::tolower(c));
  }
  std::vector<std::string_view> segments;
  std::string_view path(decoded);
  while (!path.empty()) {
    auto slash = path.find('/');
    auto part = path.substr(0, slash);
    if (part == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (!part.empty() && part != ".")
      segments.push_back(part);
    if (slash == path.npos) break;
    path.remove_prefix(slash + 1);
  }
  return !segments.empty() &&
         (segments.front() == "order" || segments.front() == "orders");
}
HttpResult RestClient::request(std::string_view method,
                               std::string_view address, const Headers &headers,
                               std::string_view body, int timeout) const {
  auto u = Url::parse(address);
  if (u.scheme != "https" && u.scheme != "http")
    throw std::invalid_argument("REST needs HTTP(S)");
  if (method != "GET" && order_path(u.target)) {
    if (blocked_)
      ++*blocked_;
    throw std::runtime_error("order-capable REST path blocked: only the "
                             "guarded native HTTP/2 sender may submit orders");
  }
  if (timeout <= 0)
    throw std::invalid_argument("REST timeout must be positive");
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
  auto proxy = connect_proxy_for(u);
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
  req.set(http::field::host, u.authority());
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
              if (auto options = transport::configure_tcp_socket(
                      beast::get_lowest_layer(stream).socket()))
                return finish(options);
              if (!proxy) {
                tls();
                return;
              }
              tunnel.method(http::verb::connect);
              tunnel.version(11);
              tunnel.target(u.authority(true));
              tunnel.set(http::field::host, u.authority(true));
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
  double silence, last_rx = 0, last_ping = 0, up_since = 0;
  bool app_ping, up = false, stopped = false;
  unsigned retry = 0;
  std::string failure; // why the connection last went down
  std::uint64_t drops = 0;
  tcp::resolver resolver;
  net::steady_timer retry_timer, watch;
  std::shared_ptr<Pipe> pipe;
  Impl(net::io_context &i, net::ssl::context &c, std::string address, Message m,
       State s, double timeout, bool ping)
      : io(i), ctx(c), url(Url::parse(address)), message(std::move(m)),
        state(std::move(s)), silence(timeout), app_ping(ping), resolver(i),
        retry_timer(i), watch(i) {
    if (url.scheme != "wss")
      throw std::invalid_argument("WebSocket needs WSS");
    if (!std::isfinite(silence) || silence <= 0)
      throw std::invalid_argument("WebSocket silence timeout must be positive");
  }
  bool current(const std::shared_ptr<Pipe> &p) const {
    return !stopped && pipe == p && !p->failed;
  }
  void fail(const std::shared_ptr<Pipe> &p, std::string why) {
    if (!current(p))
      return;
    p->failed = true;
    bool was_up = up;
    up = p->up = false;
    failure = std::move(why);
    if (was_up)
      ++drops;
    watch.cancel();
    resolver.cancel();
    beast::error_code ignored;
    beast::get_lowest_layer(p->socket).socket().close(ignored);
    p->queue.clear();
    auto self = shared_from_this();
    // A connection that was up and dropped is reopened at once: the venue
    // closing a socket is no reason to stay away. Attempts that fail back
    // off (0.25 s, 0.5 s ... 8 s) so a venue that is down is not hammered.
    double delay = was_up && retry == 0
                       ? 0.0
                       : std::min(8., .25 * std::pow(2., std::min(retry, 5u)));
    ++retry;
    retry_timer.expires_after(std::chrono::milliseconds(
        static_cast<int>(delay * 1000 + retry % 7 * 13)));
    retry_timer.async_wait([self, p](beast::error_code ec) {
      if (!ec && !self->stopped)
        self->connect();
    });
    if (was_up && state)
      state(false);
  }
  void fail(const std::shared_ptr<Pipe> &p, const char *what,
            const beast::error_code &ec) {
    fail(p, std::string(what) + ": " + ec.message());
  }
  void fail() {
    if (pipe)
      fail(pipe, "reconnect asked by the bot");
  }
  void connect() {
    if (stopped || (pipe && !pipe->failed))
      return;
    auto p = std::make_shared<Pipe>(io, ctx);
    pipe = p;
    p->socket.read_message_max(1024 * 1024);
    p->socket.next_layer().set_verify_mode(net::ssl::verify_peer);
    p->socket.next_layer().set_verify_callback(
        net::ssl::host_name_verification(url.host));
    if (!SSL_set_tlsext_host_name(p->socket.next_layer().native_handle(),
                                  url.host.c_str()))
      return fail(p, "TLS SNI failed");
    std::optional<Url> proxy;
    try {
      proxy = connect_proxy_for(url);
    } catch (...) {
      return fail(p, "invalid HTTP CONNECT proxy");
    }
    auto dest = proxy ? *proxy : url;
    auto self = shared_from_this();
    watch.expires_after(std::chrono::seconds(10));
    watch.async_wait([self, p](beast::error_code ec) {
      if (!ec && self->current(p) && !p->up)
        self->fail(p, "connection setup timed out");
    });
    resolver.async_resolve(
        dest.host, dest.port,
        [self, p, proxy](beast::error_code ec,
                         tcp::resolver::results_type endpoints) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p, "DNS", ec);
          beast::get_lowest_layer(p->socket).expires_after(
              std::chrono::seconds(10));
          beast::get_lowest_layer(p->socket).async_connect(
              endpoints, [self, p, proxy](beast::error_code ec, const auto &) {
                if (!self->current(p))
                  return;
                if (ec)
                  return self->fail(p, "connect", ec);
                if (transport::configure_tcp_socket(
                        beast::get_lowest_layer(p->socket).socket()))
                  return self->fail(p, "TCP options failed");
                if (!proxy)
                  return self->tls(p);
                p->tunnel.method(http::verb::connect);
                p->tunnel.version(11);
                p->tunnel.target(self->url.authority(true));
                p->tunnel.set(http::field::host,
                              self->url.authority(true));
                http::async_write(
                    beast::get_lowest_layer(p->socket), p->tunnel,
                    [self, p](beast::error_code ec, std::size_t) {
                      if (!self->current(p))
                        return;
                      if (ec)
                        return self->fail(p, "proxy write", ec);
                      http::async_read(
                          beast::get_lowest_layer(p->socket), p->buffer,
                          p->tunnel_reply,
                          [self, p](beast::error_code ec, std::size_t) {
                            if (!self->current(p))
                              return;
                            if (ec || p->tunnel_reply.get().result_int() != 200)
                              return self->fail(p, "proxy refused the tunnel");
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
            return self->fail(p, "TLS handshake", ec);
          beast::get_lowest_layer(p->socket).expires_never();
          auto timeouts =
              ws::stream_base::timeout::suggested(beast::role_type::client);
          if (self->app_ping) {
            // Hold the link: when nothing has arrived for half the silence
            // limit the socket itself sends a WebSocket ping, and the pong
            // counts as life. A link is only given up when even that goes
            // unanswered. (Feeds without an application ping keep the plain
            // rule: no data within the limit means the feed is stale.)
            timeouts.idle_timeout = std::chrono::milliseconds(
                static_cast<long long>(self->silence * 1000.0));
            timeouts.keep_alive_pings = true;
            p->socket.control_callback(
                [w = std::weak_ptr<Impl>(self)](ws::frame_type kind,
                                                beast::string_view) {
                  if (auto s = w.lock(); s && kind != ws::frame_type::close)
                    s->last_rx = mono_s();
                });
          }
          p->socket.set_option(timeouts);
          p->socket.async_handshake(self->url.authority(), self->url.target,
                                    [self, p](beast::error_code ec) {
                                      if (!self->current(p))
                                        return;
                                      if (ec)
                                        return self->fail(p, "WebSocket handshake", ec);
                                      self->watch.cancel();
                                      p->up = self->up = true;
                                      // (retry is cleared once the link has
                                      // held for a while: see tick)
                                      self->last_rx = self->up_since = mono_s();
                                      self->last_ping = 0;
                                      try {
                                        if (self->state) self->state(true);
                                      } catch (...) {
                                        return self->fail(p, "fault in the bot's connect handler");
                                      }
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
            return self->fail(p, ec == beast::error::timeout
                                     ? "no answer to keep-alive pings"
                                     : ec == ws::error::closed
                                           ? "closed by the venue"
                                           : "read",
                              ec);
          self->last_rx = mono_s();
          auto text = beast::buffers_to_string(p->buffer.data());
          p->buffer.consume(p->buffer.size());
          beast::error_code ignored;
          auto pending =
              beast::get_lowest_layer(p->socket).socket().available(ignored);
          try {
            self->message(text, static_cast<int>(pending / 4096));
          } catch (...) {
            return self->fail(p, "fault in the bot's message handler");
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
      return fail(p, "send queue overflow");
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
                              return self->fail(p, "write", ec);
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
      // Only a link that has held counts as a good connection. One that the
      // venue drops straight after accepting it keeps backing off, so a
      // flapping venue is never met with a storm of reconnects.
      if (self->retry && mono_s() - self->up_since >= 5.0)
        self->retry = 0;
      if (mono_s() - self->last_rx > self->silence)
        return self->fail(p, "silent for " +
                                 std::to_string(static_cast<int>(self->silence)) +
                                 " s");
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
std::string WebSocket::last_failure() const { return p_->failure; }
std::uint64_t WebSocket::drops() const { return p_->drops; }
} // namespace sniper
