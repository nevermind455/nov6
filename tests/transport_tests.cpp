#include "sniper/network.hpp"
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>
#include <nghttp2/nghttp2.h>
#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <thread>
#include <unistd.h>
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
  bool flow_controlled = false, early_response = false;
  int http_status = 200, expected_requests = 1;
  std::atomic<int> requests{0}, connections{0};
  std::atomic<std::size_t> received_bytes{0};
  std::atomic<bool> settings_acknowledged{false}, window_exhausted{false};
  std::atomic<bool> upload_released{false};
  std::atomic<bool> fixture_finished{false};
  std::mutex upload_mutex;
  std::condition_variable upload_wake;
  std::string method, path; // of the last request
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
    bool responded = false, window_released = false;
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
  static int request_header(nghttp2_session *, const nghttp2_frame *,
                            const uint8_t *name, size_t namelen,
                            const uint8_t *value, size_t valuelen, uint8_t,
                            void *user) {
    auto *state = static_cast<Session *>(user);
    std::string_view key(reinterpret_cast<const char *>(name), namelen),
        text(reinterpret_cast<const char *>(value), valuelen);
    if (key == ":method")
      state->server->method = text;
    else if (key == ":path")
      state->server->path = text;
    return 0;
  }
  static int data(nghttp2_session *, uint8_t, int32_t, const uint8_t *bytes,
                  size_t len, void *user) {
    auto *state = static_cast<Session *>(user);
    state->server->received.append(reinterpret_cast<const char *>(bytes), len);
    state->server->received_bytes += len;
    return 0;
  }
  static void respond(nghttp2_session *session, Session *s, int32_t stream) {
    s->stream = stream;
    std::string key = ":status",
                value = std::to_string(s->server->http_status);
    nghttp2_nv header{reinterpret_cast<uint8_t *>(key.data()),
                      reinterpret_cast<uint8_t *>(value.data()), key.size(),
                      value.size(), NGHTTP2_NV_FLAG_NONE};
    nghttp2_data_provider provider{};
    provider.source.ptr = s;
    provider.read_callback = response_data;
    nghttp2_submit_response(session, s->stream, &header, 1, &provider);
    s->responded = true;
  }
  static int frame(nghttp2_session *session, const nghttp2_frame *frame,
                   void *user) {
    auto *s = static_cast<Session *>(user);
    if (frame->hd.type == NGHTTP2_SETTINGS &&
        (frame->hd.flags & NGHTTP2_FLAG_ACK))
      s->server->settings_acknowledged = true;
    if (frame->hd.type == NGHTTP2_HEADERS) {
      s->stream = frame->hd.stream_id;
      if (s->server->early_response && !s->responded)
        respond(session, s, frame->hd.stream_id);
    }
    if (s->server->flow_controlled && frame->hd.type == NGHTTP2_DATA &&
        !(frame->hd.flags & NGHTTP2_FLAG_END_STREAM) &&
        s->server->received_bytes >= 1024)
      s->server->window_exhausted = true;
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
      if (!s->responded)
        respond(session, s, frame->hd.stream_id);
      s->complete = true;
    }
    return 0;
  }
  void start() {
    thread = std::thread([this] {
      try {
        net::ssl::stream<tcp::socket> socket(io, tls);
        acceptor.accept(socket.next_layer());
        ++connections;
        socket.handshake(net::ssl::stream_base::server);
        Session state{this};
        nghttp2_session_callbacks *callbacks = nullptr;
        nghttp2_session_callbacks_new(&callbacks);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, frame);
        nghttp2_session_callbacks_set_on_header_callback(callbacks,
                                                         request_header);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                                  data);
        nghttp2_session *session = nullptr;
        nghttp2_option *options = nullptr;
        nghttp2_option_new(&options);
        if (flow_controlled)
          nghttp2_option_set_no_auto_window_update(options, 1);
        nghttp2_session_server_new2(&session, callbacks, &state, options);
        nghttp2_option_del(options);
        nghttp2_session_callbacks_del(callbacks);
        nghttp2_settings_entry window{NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1024};
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE,
                               flow_controlled ? &window : nullptr,
                               flow_controlled ? 1 : 0);
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
          if (flow_controlled && window_exhausted && !state.window_released) {
            std::unique_lock lock(upload_mutex);
            if (!upload_wake.wait_for(lock, std::chrono::seconds(5),
                                      [this] { return upload_released.load(); }))
              break;
            state.window_released = true;
            nghttp2_submit_window_update(session, NGHTTP2_FLAG_NONE,
                                         state.stream, 65535);
            flush();
          }
          if (state.complete) {
            if (requests < expected_requests) {
              // flush() serialized the complete response to the completed
              // request. Reuse this TLS/nghttp2 session for the next stream.
              state = Session{this};
              continue;
            }
            if (flow_controlled) {
              std::unique_lock lock(upload_mutex);
              upload_wake.wait_for(lock, std::chrono::seconds(5),
                                   [this] { return fixture_finished.load(); });
            }
            break;
          }
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
    fixture_finished = true;
    release_upload();
    if (thread.joinable())
      thread.join();
  }
  void release_upload() {
    upload_released = true;
    upload_wake.notify_all();
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
  EXPECT_EQ(fake.method, "POST");
  EXPECT_EQ(fake.path, "/order");
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
void exercise_flow_control(bool early_response) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  FakeClob fake(tls, false, early_response ? 400 : 200);
  fake.flow_controlled = true;
  fake.early_response = early_response;
  fake.start();
  std::string body(32768, 'x');
  std::atomic<std::uint64_t> blocked{0};
  int written = 0, replies = 0, unknown = 0;
  bool submitted = false, saw_blocked_upload = false, saw_early_reply = false;
  double blocked_since = 0;
  H2Callbacks callbacks;
  callbacks.written = [&](auto id, auto stamp) {
    EXPECT_EQ(id, "flow-controlled-order");
    EXPECT_GT(stamp, 0);
    EXPECT_TRUE(fake.upload_released);
    ++written;
  };
  callbacks.reply = [&](auto id, const HttpResult &response) {
    EXPECT_EQ(id, "flow-controlled-order");
    EXPECT_EQ(response.status, early_response ? 400 : 200);
    EXPECT_EQ(response.body, fake.response);
    saw_early_reply = !fake.upload_released;
    ++replies;
  };
  callbacks.unknown = [&](auto id, auto reason) {
    if (id == "flow-controlled-order") {
      ADD_FAILURE() << reason;
      ++unknown;
    }
  };
  H2Client client(io, tls, true, blocked, std::move(callbacks), 3, "127.0.0.1",
                  std::to_string(fake.acceptor.local_endpoint().port()));
  net::steady_timer poll(io);
  std::function<void()> step = [&] {
    poll.expires_after(std::chrono::milliseconds(10));
    poll.async_wait([&](auto ec) {
      if (ec)
        return;
      if (!submitted && client.ready() && fake.settings_acknowledged)
        submitted = client.post("flow-controlled-order", body, {});
      if (fake.window_exhausted && !fake.upload_released) {
        if (blocked_since == 0)
          blocked_since = mono_s();
        if ((early_response && replies == 1) ||
            (!early_response && mono_s() - blocked_since > .1)) {
          EXPECT_EQ(written, 0);
          EXPECT_LT(fake.received_bytes, body.size());
          saw_blocked_upload = true;
          fake.release_upload();
        }
      }
      if (written == 1 && replies == 1 && fake.requests == 1)
        io.stop();
      else
        step();
    });
  };
  client.start();
  step();
  io.run_for(std::chrono::seconds(5));
  client.stop();
  io.restart();
  io.poll();
  fake.join();
  EXPECT_TRUE(submitted);
  EXPECT_TRUE(saw_blocked_upload);
  EXPECT_EQ(saw_early_reply, early_response);
  EXPECT_EQ(written, 1);
  EXPECT_EQ(replies, 1);
  EXPECT_EQ(unknown, 0);
  EXPECT_EQ(fake.requests, 1);
  EXPECT_EQ(fake.received, body);
  EXPECT_EQ(blocked, 0);
}
} // namespace
// SNIPER_REST_LIVE: a cancel is a DELETE of the order path on the same warm
// connection, answered under its own id.
TEST(NativeHttp2, CancelIsADeleteOfTheOrderPath) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  FakeClob fake(tls, false, 200);
  fake.response = R"({"canceled":["0xabc"],"not_canceled":{}})";
  auto port = std::to_string(fake.acceptor.local_endpoint().port());
  fake.start();
  std::atomic<std::uint64_t> blocked{0};
  bool submitted = false, replied = false, unknown = false;
  HttpResult response;
  H2Callbacks callbacks;
  callbacks.reply = [&](auto id, const HttpResult &result) {
    EXPECT_EQ(id, "cancel:0xabc");
    response = result;
    replied = true;
    io.stop();
  };
  callbacks.unknown = [&](auto, auto) {
    unknown = true;
    io.stop();
  };
  H2Client client(io, tls, true, blocked, std::move(callbacks), 2, "127.0.0.1",
                  port);
  net::steady_timer poll(io);
  std::function<void()> ready = [&] {
    poll.expires_after(std::chrono::milliseconds(10));
    poll.async_wait([&](auto ec) {
      if (ec)
        return;
      if (client.ready())
        submitted = client.cancel("cancel:0xabc", R"({"orderID":"0xabc"})",
                                  {{"POLY_TIMESTAMP", "1900000000"}});
      else
        ready();
    });
  };
  client.start();
  ready();
  io.run_for(std::chrono::seconds(5));
  client.stop();
  io.restart();
  io.poll();
  fake.join();
  ASSERT_TRUE(submitted);
  EXPECT_TRUE(replied);
  EXPECT_FALSE(unknown);
  EXPECT_EQ(fake.method, "DELETE");
  EXPECT_EQ(fake.path, "/order");
  EXPECT_EQ(fake.received, R"({"orderID":"0xabc"})");
  EXPECT_EQ(fake.requests, 1);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, fake.response);
  EXPECT_EQ(blocked, 0);
}
TEST(NativeHttp2, PaperCanNeitherPlaceNorCancel) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  std::atomic<std::uint64_t> blocked{0};
  H2Client client(io, tls, false, blocked, {}, 2, "127.0.0.1", "1");
  EXPECT_THROW(client.cancel("cancel:0xabc", R"({"orderID":"0xabc"})", {}),
               std::runtime_error);
  EXPECT_THROW(client.post("0xabc", "{}", {}), std::runtime_error);
  EXPECT_EQ(blocked, 2u);
}
TEST(NativeHttp2, LocalTlsMatchedReply) { exercise(false, 200); }
TEST(NativeHttp2, LocalTlsHttp400Reply) { exercise(false, 400); }
TEST(NativeHttp2, UncertainWriteIsNeverRetried) { exercise(true, 200); }
TEST(NativeHttp2, GoawayAfterPossibleProcessingClosesConnection) {
  exercise(false, 200, true, true);
}
TEST(NativeHttp2, GoawayBeforeProcessingClosesConnection) {
  exercise(false, 200, true, false);
}
TEST(NativeHttp2, WrittenWaitsForFlowControlledUploadEndStream) {
  exercise_flow_control(false);
}
TEST(NativeHttp2, EarlyResponseKeepsFlowControlledUploadProviderAlive) {
  ASSERT_EXIT(
      {
        exercise_flow_control(true);
        _exit(::testing::Test::HasFailure() ? 1 : 0);
      },
      ::testing::ExitedWithCode(0), "");
}
TEST(NativeHttp2, SequentialOrdersReuseTheWarmLowLatencyTlsConnection) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  FakeClob fake(tls, false, 200);
  fake.expected_requests = 2;
  fake.start();
  std::atomic<std::uint64_t> blocked{0};
  int written = 0, replies = 0, unknown = 0;
  bool first_submitted = false, second_submitted = false;
  std::optional<H2ConnectionInfo> first_connection, second_connection;
  H2Client *client_ptr = nullptr;
  H2Callbacks callbacks;
  callbacks.written = [&](auto id, auto stamp) {
    EXPECT_TRUE(id == "offline-first" || id == "offline-second");
    EXPECT_GT(stamp, 0);
    ++written;
    if (written == 2 && replies == 2)
      io.stop();
  };
  callbacks.reply = [&](auto id, const HttpResult &response) {
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, fake.response);
    ++replies;
    if (id == "offline-first") {
      EXPECT_EQ(replies, 1);
      // Dispatch outside the nghttp2 response callback, as normal event-loop
      // decisions do, rather than recursively calling its serializer.
      net::post(io, [&] {
        second_connection = client_ptr->connection_info();
        second_submitted = client_ptr->post(
            "offline-second", R"({"offline":2})", {});
      });
    } else {
      EXPECT_EQ(id, "offline-second");
      EXPECT_EQ(replies, 2);
      if (written == 2)
        io.stop();
    }
  };
  callbacks.unknown = [&](auto id, auto why) {
    ADD_FAILURE() << id << ": " << why;
    ++unknown;
    io.stop();
  };
  H2Client client(io, tls, true, blocked, std::move(callbacks), 2,
                  "127.0.0.1",
                  std::to_string(fake.acceptor.local_endpoint().port()));
  client_ptr = &client;
  EXPECT_FALSE(client.connection_info());
  net::steady_timer poll(io);
  std::function<void()> start_when_ready = [&] {
    poll.expires_after(std::chrono::milliseconds(10));
    poll.async_wait([&](auto ec) {
      if (ec)
        return;
      if (!client.ready()) {
        start_when_ready();
        return;
      }
      first_connection = client.connection_info();
      first_submitted = client.post("offline-first", R"({"offline":1})", {});
    });
  };
  client.start();
  start_when_ready();
  io.run_for(std::chrono::seconds(5));
  client.stop();
  EXPECT_FALSE(client.connection_info());
  io.restart();
  io.poll();
  fake.join();
  ASSERT_TRUE(first_submitted);
  ASSERT_TRUE(second_submitted);
  ASSERT_TRUE(first_connection);
  ASSERT_TRUE(second_connection);
  EXPECT_GT(first_connection->generation, 0u);
  EXPECT_GT(first_connection->ready_ns, 0);
  EXPECT_LE(first_connection->ready_ns, mono_ns());
  EXPECT_EQ(second_connection->generation, first_connection->generation);
  EXPECT_EQ(second_connection->ready_ns, first_connection->ready_ns);
  EXPECT_TRUE(first_connection->tcp_no_delay);
  EXPECT_TRUE(first_connection->tcp_keep_alive);
  EXPECT_TRUE(second_connection->tcp_no_delay);
  EXPECT_TRUE(second_connection->tcp_keep_alive);
  EXPECT_EQ(fake.connections, 1);
  EXPECT_EQ(fake.requests, 2);
  EXPECT_EQ(fake.received, R"({"offline":1}{"offline":2})");
  EXPECT_EQ(written, 2);
  EXPECT_EQ(replies, 2);
  EXPECT_EQ(unknown, 0);
  EXPECT_EQ(blocked, 0);
}
