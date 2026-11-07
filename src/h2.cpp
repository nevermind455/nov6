#include "sniper/network.hpp"
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>
namespace sniper {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;
struct H2Client::Impl : std::enable_shared_from_this<H2Client::Impl> {
  struct Request {
    std::string id, body, response;
    std::size_t offset = 0;
    int status = 0;
    double started = 0;
    bool replied = false, timed_out = false;
  };
  struct Write {
    std::shared_ptr<std::string> bytes;
    std::vector<std::string> ids;
  };
  struct Pipe {
    beast::ssl_stream<beast::tcp_stream> stream;
    beast::flat_buffer proxy_buffer;
    http::request<http::empty_body> proxy_request;
    http::response_parser<http::empty_body> proxy_parser;
    std::array<unsigned char, 65536> rx{};
    nghttp2_session *session = nullptr;
    std::map<int32_t, std::shared_ptr<Request>> requests;
    std::deque<Write> writes;
    std::vector<std::string> serialized_ids;
    std::size_t queued_bytes = 0;
    bool up = false, writing = false, goaway = false, failed = false;
    Impl *owner;
    Pipe(net::io_context &i, net::ssl::context &c, Impl *o)
        : stream(i, c), owner(o) {
      proxy_parser.skip(true);
    }
    ~Pipe() {
      if (session)
        nghttp2_session_del(session);
    }
  };
  net::io_context &io;
  net::ssl::context &ctx;
  bool live, stopped = false;
  std::atomic<std::uint64_t> &blocked;
  H2Callbacks callbacks;
  int timeout;
  std::string host, port;
  tcp::resolver resolver;
  net::steady_timer timer, retry;
  std::shared_ptr<Pipe> pipe;
  double last_ping = 0;
  unsigned failures = 0;
  Impl(net::io_context &i, net::ssl::context &c, bool l,
       std::atomic<std::uint64_t> &b, H2Callbacks cb, int t, std::string h,
       std::string po)
      : io(i), ctx(c), live(l), blocked(b), callbacks(std::move(cb)),
        timeout(t), host(std::move(h)), port(std::move(po)), resolver(i),
        timer(i), retry(i) {
    if (timeout <= 0)
      throw std::invalid_argument("HTTP/2 timeout must be positive");
  }
  bool current(const std::shared_ptr<Pipe> &p) {
    return !stopped && p == pipe && !p->failed;
  }
  static ssize_t data_read(nghttp2_session *, int32_t, uint8_t *buffer,
                           size_t length, uint32_t *flags,
                           nghttp2_data_source *source, void *) {
    auto *r = static_cast<Request *>(source->ptr);
    auto n = std::min(length, r->body.size() - r->offset);
    std::memcpy(buffer, r->body.data() + r->offset, n);
    r->offset += n;
    if (r->offset == r->body.size())
      *flags |= NGHTTP2_DATA_FLAG_EOF;
    return n;
  }
  static int header(nghttp2_session *, const nghttp2_frame *frame,
                    const uint8_t *name, size_t namelen, const uint8_t *value,
                    size_t len, uint8_t, void *user) {
    auto *p = static_cast<Pipe *>(user);
    auto i = p->requests.find(frame->hd.stream_id);
    if (i != p->requests.end() &&
        std::string_view(reinterpret_cast<const char *>(name), namelen) ==
            ":status") {
      try {
        i->second->status =
            std::stoi(std::string(reinterpret_cast<const char *>(value), len));
      } catch (...) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
    }
    return 0;
  }
  static int data(nghttp2_session *, uint8_t, int32_t sid, const uint8_t *bytes,
                  size_t len, void *user) {
    auto *p = static_cast<Pipe *>(user);
    auto i = p->requests.find(sid);
    if (i != p->requests.end()) {
      if (i->second->response.size() + len > 256 * 1024)
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      try {
        i->second->response.append(reinterpret_cast<const char *>(bytes), len);
      } catch (...) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
      }
    }
    return 0;
  }
  static int frame(nghttp2_session *, const nghttp2_frame *frame, void *user) {
    auto *p = static_cast<Pipe *>(user);
    if (frame->hd.type == NGHTTP2_GOAWAY)
      p->goaway = true;
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
      auto i = p->requests.find(frame->hd.stream_id);
      if (i != p->requests.end()) {
        auto r = i->second;
        if (r->replied || r->timed_out)
          return 0;
        if (p->owner->callbacks.reply)
          try {
            p->owner->callbacks.reply(r->id, {r->status, r->response});
          } catch (...) {
            return NGHTTP2_ERR_CALLBACK_FAILURE;
          }
        // A final response can precede the end of the upload. nghttp2 still
        // holds the data provider's raw Request pointer until stream close.
        r->replied = true;
      }
    }
    return 0;
  }
  static int closed(nghttp2_session *, int32_t sid, uint32_t, void *user) {
    auto *p = static_cast<Pipe *>(user);
    auto i = p->requests.find(sid);
    if (i != p->requests.end()) {
      auto r = i->second;
      p->requests.erase(i);
      if (!r->replied && !r->timed_out && p->owner->callbacks.unknown)
        try {
          p->owner->callbacks.unknown(
              r->id, "HTTP/2 stream closed before a complete reply");
        } catch (...) {
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
    }
    return 0;
  }
  static int frame_sent(nghttp2_session *, const nghttp2_frame *frame,
                        void *user) {
    auto *p = static_cast<Pipe *>(user);
    if ((frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
      auto i = p->requests.find(frame->hd.stream_id);
      if (i != p->requests.end() && i->second->id != "__clock") {
        try {
          p->serialized_ids.push_back(i->second->id);
        } catch (...) {
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
      }
    }
    return 0;
  }
  void fail(const std::shared_ptr<Pipe> &p, std::string_view reason) {
    if (!current(p))
      return;
    p->failed = true;
    p->up = false;
    beast::error_code ec;
    beast::get_lowest_layer(p->stream).socket().close(ec);
    resolver.cancel();
    auto pending = std::move(p->requests);
    p->writes.clear();
    p->serialized_ids.clear();
    p->queued_bytes = 0;
    p->writing = false;
    timer.cancel();
    if (stopped)
      return;
    auto self = shared_from_this();
    retry.expires_after(std::chrono::milliseconds(static_cast<int>(
        std::min(8000., 250. * std::pow(2., std::min(failures++, 5u))))));
    retry.async_wait([self, p](beast::error_code ec) {
      if (!ec)
        self->connect();
    });
    // Finish transport cleanup before application callbacks, which may throw.
    std::exception_ptr failure;
    for (auto &[sid, r] : pending)
      if (!r->replied && !r->timed_out && callbacks.unknown)
        try {
          callbacks.unknown(r->id, reason);
        } catch (...) {
          if (!failure) failure = std::current_exception();
        }
    if (failure)
      std::rethrow_exception(failure);
  }
  void connect() {
    if (stopped || (pipe && !pipe->failed))
      return;
    auto p = std::make_shared<Pipe>(io, ctx, this);
    pipe = p;
    p->stream.set_verify_mode(net::ssl::verify_peer);
    p->stream.set_verify_callback(net::ssl::host_name_verification(host));
    if (!SSL_set_tlsext_host_name(p->stream.native_handle(), host.c_str()))
      return fail(p, "TLS SNI failed");
    static const unsigned char alpn[]{2, 'h', '2'};
    if (SSL_set_alpn_protos(p->stream.native_handle(), alpn, sizeof(alpn)))
      return fail(p, "HTTP/2 ALPN setup failed");
    auto self = shared_from_this();
    std::optional<Url> proxy;
    Url endpoint{"https", host, port, "/"};
    try {
      proxy = connect_proxy_for(endpoint);
    } catch (...) {
      return fail(p, "invalid HTTP CONNECT proxy");
    }
    // This deadline includes DNS, CONNECT and TLS, rather than starting only
    // after DNS has already completed.
    timer.expires_after(std::chrono::seconds(timeout));
    timer.async_wait([self, p](beast::error_code ec) {
      if (!ec && self->current(p) && !p->up)
        self->fail(p, "HTTP/2 connection setup timed out");
    });
    resolver.async_resolve(
        proxy ? proxy->host : host, proxy ? proxy->port : port,
        [self, p, proxy](beast::error_code ec,
                         tcp::resolver::results_type addresses) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p, "HTTP/2 DNS failed");
          beast::get_lowest_layer(p->stream).expires_after(
              std::chrono::seconds(self->timeout));
          beast::get_lowest_layer(p->stream).async_connect(
              addresses, [self, p, proxy](beast::error_code ec, const auto &) {
                if (!self->current(p))
                  return;
                if (ec)
                  return self->fail(p, "HTTP/2 connect failed");
                if (!proxy) {
                  self->tls(p);
                  return;
                }
                p->proxy_request.method(http::verb::connect);
                p->proxy_request.version(11);
                Url endpoint{"https", self->host, self->port, "/"};
                p->proxy_request.target(endpoint.authority(true));
                p->proxy_request.set(http::field::host,
                                     endpoint.authority(true));
                http::async_write(
                    beast::get_lowest_layer(p->stream), p->proxy_request,
                    [self, p](beast::error_code ec, std::size_t) {
                      if (!self->current(p))
                        return;
                      if (ec)
                        return self->fail(p, "HTTP/2 proxy write failed");
                      http::async_read(
                          beast::get_lowest_layer(p->stream), p->proxy_buffer,
                          p->proxy_parser,
                          [self, p](beast::error_code ec, std::size_t) {
                            if (!self->current(p))
                              return;
                            if (ec || p->proxy_parser.get().result_int() != 200)
                              return self->fail(p, "HTTP/2 proxy refused");
                            self->tls(p);
                          });
                    });
              });
        });
  }
  void tls(const std::shared_ptr<Pipe> &p) {
    auto self = shared_from_this();
    p->stream.async_handshake(
        net::ssl::stream_base::client, [self, p](beast::error_code ec) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p, "HTTP/2 TLS failed");
          const unsigned char *protocol = nullptr;
          unsigned length = 0;
          SSL_get0_alpn_selected(p->stream.native_handle(), &protocol, &length);
          if (length != 2 || std::memcmp(protocol, "h2", 2))
            return self->fail(p, "CLOB did not negotiate HTTP/2");
          nghttp2_session_callbacks *cb = nullptr;
          nghttp2_session_callbacks_new(&cb);
          nghttp2_session_callbacks_set_on_header_callback(cb, header);
          nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cb, data);
          nghttp2_session_callbacks_set_on_frame_recv_callback(cb, frame);
          nghttp2_session_callbacks_set_on_stream_close_callback(cb, closed);
          nghttp2_session_callbacks_set_on_frame_send_callback(cb, frame_sent);
          int rc = nghttp2_session_client_new(&p->session, cb, p.get());
          nghttp2_session_callbacks_del(cb);
          if (rc)
            return self->fail(p, "HTTP/2 session initialization failed");
          nghttp2_settings_entry settings[] = {
              {NGHTTP2_SETTINGS_ENABLE_PUSH, 0},
              {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 20}};
          nghttp2_submit_settings(p->session, NGHTTP2_FLAG_NONE, settings, 2);
          p->up = true;
          self->failures = 0;
          beast::get_lowest_layer(p->stream).expires_never();
          self->flush(p);
          self->read(p);
          self->tick(p);
        });
  }
  void flush(const std::shared_ptr<Pipe> &p) {
    if (!current(p) || !p->up)
      return;
    auto out = std::make_shared<std::string>();
    const uint8_t *bytes;
    ssize_t n;
    while ((n = nghttp2_session_mem_send(p->session, &bytes)) > 0)
      out->append(reinterpret_cast<const char *>(bytes), n);
    if (n < 0)
      return fail(p, "HTTP/2 frame encoding failed");
    if (!out->empty()) {
      if (p->writes.size() >= 256 || p->queued_bytes + out->size() > 1024 * 1024)
        return fail(p, "HTTP/2 write queue exceeded its bound");
      p->queued_bytes += out->size();
      p->writes.push_back({out, std::move(p->serialized_ids)});
      p->serialized_ids.clear();
      if (!p->writing)
        write(p);
    }
  }
  void write(const std::shared_ptr<Pipe> &p) {
    if (!current(p) || !p->up || p->writes.empty()) {
      p->writing = false;
      return;
    }
    p->writing = true;
    auto self = shared_from_this();
    auto payload = p->writes.front();
    net::async_write(p->stream, net::buffer(*payload.bytes),
                     [self, p, payload](beast::error_code ec, std::size_t) {
                       if (!self->current(p))
                         return;
                       if (ec)
                         return self->fail(
                             p, "transport failure after possible order write");
                       for (auto &id : payload.ids)
                         if (self->callbacks.written)
                           self->callbacks.written(id, mono_ns());
                       if (!p->writes.empty()) {
                         p->queued_bytes -= p->writes.front().bytes->size();
                         p->writes.pop_front();
                       }
                       self->write(p);
                     });
  }
  void read(const std::shared_ptr<Pipe> &p) {
    auto self = shared_from_this();
    p->stream.async_read_some(
        net::buffer(p->rx), [self, p](beast::error_code ec, std::size_t n) {
          if (!self->current(p))
            return;
          if (ec)
            return self->fail(p, "HTTP/2 connection lost after possible write");
          auto used = nghttp2_session_mem_recv(p->session, p->rx.data(), n);
          if (used < 0)
            return self->fail(p, "invalid HTTP/2 reply");
          // A GOAWAY connection cannot accept new streams even while TCP stays
          // open. Reconnect instead of advertising it as ready. Requests whose
          // replies have not completed remain unknown and are never resent,
          // including streams the peer says it may already have processed.
          if (p->goaway)
            return self->fail(p, "HTTP/2 peer sent GOAWAY");
          self->flush(p);
          self->read(p);
        });
  }
  void tick(const std::shared_ptr<Pipe> &p) {
    auto self = shared_from_this();
    timer.expires_after(std::chrono::milliseconds(100));
    timer.async_wait([self, p](beast::error_code ec) {
      if (ec || !self->current(p) || !p->up)
        return;
      auto now = mono_s();
      bool clock_lost = false;
      for (auto &[sid, r] : p->requests) {
        if (!r->timed_out && now - r->started >= self->timeout) {
          r->timed_out = true;
          nghttp2_submit_rst_stream(p->session, NGHTTP2_FLAG_NONE, sid,
                                    NGHTTP2_CANCEL);
          if (r->id == "__clock" && !r->replied)
            clock_lost = true;
          if (!r->replied && self->callbacks.unknown)
            self->callbacks.unknown(r->id, "order reply timeout; never resent");
        }
      }
      // No answer to /time means the connection is dead. TCP alone can take
      // many minutes to notice, and live orders stay refused until it does.
      if (clock_lost)
        return self->fail(p, "HTTP/2 clock probe timed out");
      if (now - self->last_ping >= 15) {
        unsigned char opaque[8]{};
        nghttp2_submit_ping(p->session, NGHTTP2_FLAG_NONE, opaque);
        self->last_ping = now;
      }
      self->flush(p);
      self->tick(p);
    });
  }
  bool post(std::string id, std::string body, const Headers &headers) {
    if (!live) {
      ++blocked;
      throw std::runtime_error(
          "paper firewall: native order submission blocked");
    }
    auto p = pipe;
    if (stopped || !p || !p->up ||
        std::count_if(
            p->requests.begin(), p->requests.end(),
            [](const auto &item) { return item.second->id != "__clock"; }) >= 2)
      return false;
    if (nghttp2_session_get_remote_window_size(p->session) <
        static_cast<int>(body.size()))
      return false;
    auto r = std::make_shared<Request>();
    r->id = std::move(id);
    r->body = std::move(body);
    r->started = mono_s();
    std::vector<std::pair<std::string, std::string>> h{
        {":method", "POST"},
        {":scheme", "https"},
        {":authority", Url{"https", host, port, "/"}.authority()},
        {":path", "/order"},
        {"content-type", "application/json"},
        {"content-length", std::to_string(r->body.size())},
        {"user-agent", "sniper-native-cpp/1.0"}};
    for (auto [k, v] : headers) {
      std::transform(k.begin(), k.end(), k.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      h.emplace_back(k, v);
    }
    std::vector<nghttp2_nv> nv;
    for (auto &[k, v] : h)
      nv.push_back({reinterpret_cast<uint8_t *>(k.data()),
                    reinterpret_cast<uint8_t *>(v.data()), k.size(), v.size(),
                    NGHTTP2_NV_FLAG_NONE});
    nghttp2_data_provider provider{};
    provider.source.ptr = r.get();
    provider.read_callback = data_read;
    auto sid = nghttp2_submit_request(p->session, nullptr, nv.data(), nv.size(),
                                      &provider, nullptr);
    if (sid < 0)
      return false;
    p->requests[sid] = r;
    flush(p);
    return true;
  }
  bool get_time() {
    auto p = pipe;
    if (stopped || !p || !p->up || p->requests.size() >= 3)
      return false;
    for (auto &[sid, r] : p->requests)
      if (r->id == "__clock")
        return false;
    auto r = std::make_shared<Request>();
    r->id = "__clock";
    r->started = mono_s();
    // Same identity headers as the REST client and the order sender.
    std::vector<std::pair<std::string, std::string>> h{
        {":method", "GET"},
        {":scheme", "https"},
        {":authority", Url{"https", host, port, "/"}.authority()},
        {":path", "/time"},
        {"accept", "application/json"},
        {"user-agent", "sniper-native-cpp/1.0"}};
    std::vector<nghttp2_nv> nv;
    for (auto &[k, v] : h)
      nv.push_back({reinterpret_cast<uint8_t *>(k.data()),
                    reinterpret_cast<uint8_t *>(v.data()), k.size(), v.size(),
                    NGHTTP2_NV_FLAG_NONE});
    auto sid = nghttp2_submit_request(p->session, nullptr, nv.data(), nv.size(),
                                      nullptr, nullptr);
    if (sid < 0)
      return false;
    p->requests[sid] = r;
    flush(p);
    return true;
  }
  void stop() {
    stopped = true;
    timer.cancel();
    retry.cancel();
    resolver.cancel();
    if (pipe) {
      pipe->up = false;
      beast::error_code ec;
      beast::get_lowest_layer(pipe->stream).socket().close(ec);
    }
  }
};
H2Client::H2Client(net::io_context &io, net::ssl::context &ctx, bool live,
                   std::atomic<std::uint64_t> &blocked, H2Callbacks c,
                   int timeout, std::string host, std::string port)
    : p_(std::make_shared<Impl>(io, ctx, live, blocked, std::move(c), timeout,
                                std::move(host), std::move(port))) {}
void H2Client::start() { p_->connect(); }
void H2Client::stop() { p_->stop(); }
bool H2Client::ready() const { return p_->pipe && p_->pipe->up; }
bool H2Client::post(std::string id, std::string body, const Headers &h) {
  return p_->post(std::move(id), std::move(body), h);
}
} // namespace sniper

namespace sniper {
bool H2Client::get_time() { return p_->get_time(); }
} // namespace sniper
