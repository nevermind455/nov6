#include "sniper/network.hpp"
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <thread>
using namespace sniper;
namespace net = boost::asio;
using tcp = net::ip::tcp;
namespace {
struct FakeClob {
  net::io_context io;
  net::ssl::context tls{net::ssl::context::tls_server};
  tcp::acceptor acceptor{io, {net::ip::make_address("127.0.0.1"), 0}};
  std::thread thread;
  bool drop = false, goaway = false, goaway_accepts_request = false;
  int http_status = 200;
  std::atomic<int> requests{0};
  std::string received,
      response =
          R"({"success":true,"makingAmount":"1","takingAmount":"2","orderID":"offline-order"})";
  explicit FakeClob(net::ssl::context &client, bool close_after_request,
                    int status)
      : drop(close_after_request), http_status(status) {
    EVP_PKEY *key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (!key)
      throw std::runtime_error("test TLS key generation failed");
    X509 *cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    auto name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    X509V3_CTX extctx{};
    X509V3_set_ctx_nodb(&extctx);
    X509V3_set_ctx(&extctx, cert, cert, nullptr, nullptr, 0);
    auto san =
        X509V3_EXT_conf_nid(nullptr, &extctx, NID_subject_alt_name,
                            const_cast<char *>("DNS:localhost,IP:127.0.0.1"));
    X509_add_ext(cert, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(cert, key, EVP_sha256());
    SSL_CTX_use_certificate(tls.native_handle(), cert);
    SSL_CTX_use_PrivateKey(tls.native_handle(), key);
    X509_STORE_add_cert(SSL_CTX_get_cert_store(client.native_handle()), cert);
    X509_free(cert);
    EVP_PKEY_free(key);
    SSL_CTX_set_alpn_select_cb(
        tls.native_handle(),
        [](SSL *, const unsigned char **out, unsigned char *len,
           const unsigned char *, unsigned int, void *) {
          static const unsigned char protocol[]{'h', '2'};
          *out = protocol;
          *len = 2;
          return SSL_TLSEXT_ERR_OK;
        },
        nullptr);
  }
  struct Session {
    FakeClob *server;
    int32_t stream = 0;
    bool complete = false, close = false;
    std::size_t offset = 0;
  };
  static ssize_t response_data(nghttp2_session *, int32_t, uint8_t *out,
                               size_t length, uint32_t *flags,
                               nghttp2_data_source *source, void *) {
    auto *state = static_cast<Session *>(source->ptr);
    auto &body = state->server->response;
    auto n = std::min(length, body.size() - state->offset);
    std::memcpy(out, body.data() + state->offset, n);
    state->offset += n;
    if (state->offset == body.size())
      *flags |= NGHTTP2_DATA_FLAG_EOF;
    return n;
  }
  static int data(nghttp2_session *, uint8_t, int32_t, const uint8_t *bytes,
                  size_t len, void *user) {
    auto *state = static_cast<Session *>(user);
    state->server->received.append(reinterpret_cast<const char *>(bytes), len);
    return 0;
  }
  static int frame(nghttp2_session *session, const nghttp2_frame *frame,
                   void *user) {
    auto *s = static_cast<Session *>(user);
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
      ++s->server->requests;
      if (s->server->drop) {
        s->close = true;
        return 0;
      }
      if (s->server->goaway) {
        // Keep TCP open after GOAWAY, as a draining HTTP/2 peer may do.
        nghttp2_submit_goaway(
            session, NGHTTP2_FLAG_NONE,
            s->server->goaway_accepts_request ? frame->hd.stream_id : 0,
            NGHTTP2_NO_ERROR, nullptr, 0);
        return 0;
      }
      s->stream = frame->hd.stream_id;
      std::string key = ":status",
                  value = std::to_string(s->server->http_status);
      nghttp2_nv header{reinterpret_cast<uint8_t *>(key.data()),
                        reinterpret_cast<uint8_t *>(value.data()), key.size(),
                        value.size(), NGHTTP2_NV_FLAG_NONE};
      nghttp2_data_provider provider{};
      provider.source.ptr = s;
      provider.read_callback = response_data;
      nghttp2_submit_response(session, s->stream, &header, 1, &provider);
      s->complete = true;
    }
    return 0;
  }
  void start() {
    thread = std::thread([this] {
      try {
        net::ssl::stream<tcp::socket> socket(io, tls);
        acceptor.accept(socket.next_layer());
        socket.handshake(net::ssl::stream_base::server);
        Session state{this};
        nghttp2_session_callbacks *callbacks = nullptr;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frame);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                                  data);
        nghttp2_session *session = nullptr;
        nghttp2_session_server_new(&session, callbacks, &state);
        nghttp2_session_callbacks_del(callbacks);
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
        auto flush = [&] {
          const uint8_t *bytes;
          ssize_t n;
          while ((n = nghttp2_session_mem_send(session, &bytes)) > 0)
            net::write(socket, net::buffer(bytes, n));
        };
        flush();
        std::array<uint8_t, 65536> rx;
        while (!state.close) {
          boost::system::error_code ec;
          auto n = socket.read_some(net::buffer(rx), ec);
          if (ec)
            break;
          if (nghttp2_session_mem_recv(session, rx.data(), n) < 0)
            break;
          if (state.close)
            break;
          flush();
          if (state.complete)
            break;
        }
        nghttp2_session_del(session);
        boost::system::error_code ec;
        socket.next_layer().shutdown(tcp::socket::shutdown_both, ec);
        socket.next_layer().close(ec);
      } catch (...) {
      }
    });
  }
  void join() {
    if (thread.joinable())
      thread.join();
  }
  ~FakeClob() { join(); }
};
void exercise(bool drop, int status, bool goaway = false,
              bool goaway_accepts_request = false) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  FakeClob fake(tls, drop, status);
  fake.goaway = goaway;
  fake.goaway_accepts_request = goaway_accepts_request;
  auto port = std::to_string(fake.acceptor.local_endpoint().port());
  fake.start();
  std::atomic<std::uint64_t> blocked{0};
  bool submitted = false, written = false, replied = false, unknown = false;
  HttpResult response;
  H2Callbacks callbacks;
  callbacks.written = [&](auto id, auto) {
    EXPECT_EQ(id, "offline-order");
    written = true;
  };
  callbacks.reply = [&](auto id, const HttpResult &result) {
    EXPECT_EQ(id, "offline-order");
    response = result;
    replied = true;
    io.stop();
  };
  callbacks.unknown = [&](auto id, auto) {
    if (id == "offline-order") {
      unknown = true;
      io.stop();
    }
  };
  H2Client client(io, tls, true, blocked, std::move(callbacks), 2, "127.0.0.1",
                  port);
  net::steady_timer poll(io);
  std::function<void()> ready = [&] {
    poll.expires_after(std::chrono::milliseconds(10));
    poll.async_wait([&](auto ec) {
      if (ec)
        return;
      if (client.ready()) {
        submitted = client.post("offline-order", R"({"offline":true})",
                                {{"x-offline", "1"}});
      } else
        ready();
    });
  };
  client.start();
  ready();
  io.run_for(std::chrono::seconds(5));
  if (goaway) {
    EXPECT_FALSE(client.ready());
  }
  client.stop();
  io.restart();
  io.poll();
  fake.join();
  ASSERT_TRUE(submitted);
  EXPECT_TRUE(written);
  EXPECT_EQ(fake.received, R"({"offline":true})");
  EXPECT_EQ(fake.requests, 1);
  EXPECT_EQ(blocked, 0);
  if (drop || goaway) {
    EXPECT_TRUE(unknown);
    EXPECT_FALSE(replied);
  } else {
    EXPECT_TRUE(replied);
    EXPECT_EQ(response.status, status);
    EXPECT_EQ(response.body, fake.response);
  }
}
} // namespace
TEST(NativeHttp2, LocalTlsMatchedReply) { exercise(false, 200); }
TEST(NativeHttp2, LocalTlsHttp400Reply) { exercise(false, 400); }
TEST(NativeHttp2, UncertainWriteIsNeverRetried) { exercise(true, 200); }
TEST(NativeHttp2, GoawayAfterPossibleProcessingClosesConnection) {
  exercise(false, 200, true, true);
}
TEST(NativeHttp2, GoawayBeforeProcessingClosesConnection) {
  exercise(false, 200, true, false);
}
