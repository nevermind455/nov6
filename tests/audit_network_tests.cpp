#include "sniper/network.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <cstdlib>
#include <gtest/gtest.h>
#include <limits>

using namespace sniper;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {
struct ScopedEnvironment {
  std::string name;
  std::optional<std::string> previous;
  explicit ScopedEnvironment(std::string key, const char *value)
      : name(std::move(key)) {
    if (auto old = std::getenv(name.c_str()))
      previous = old;
    if (value)
      setenv(name.c_str(), value, 1);
    else
      unsetenv(name.c_str());
  }
  ~ScopedEnvironment() {
    if (previous)
      setenv(name.c_str(), previous->c_str(), 1);
    else
      unsetenv(name.c_str());
  }
};
} // namespace

TEST(AuditNetwork, QueryAndNormalizedOrderPathsAreBlockedBeforeTransport) {
  for (bool live : {false, true}) {
    std::atomic<std::uint64_t> blocked{0};
    RestClient client(live, &blocked);
    std::uint64_t attempts = 0;
    for (const auto *method : {"POST", "PUT", "PATCH", "DELETE"})
      for (const auto *path : {"/order?trace=offline", "/orders?trace=offline",
                               "/orders/cancel", "/ORDER", "/%6frder",
                               "/safe/../order", "//orders/", "/order#fragment"}) {
        ++attempts;
        try {
          // Even invalid transport settings must never precede this firewall.
          client.request(method, std::string("http://127.0.0.1:1") + path,
                          {}, "{}", 0);
          FAIL() << method << " " << path;
        } catch (const std::runtime_error &e) {
          EXPECT_NE(std::string(e.what()).find("order-capable REST path blocked"),
                    std::string::npos)
              << method << " " << path << ": " << e.what();
        }
        EXPECT_EQ(blocked, attempts);
      }
  }
}

TEST(AuditNetwork, ReadOnlyAndUnrelatedPathsDoNotTripOrderFirewall) {
  std::atomic<std::uint64_t> blocked{0};
  RestClient client(false, &blocked);
  EXPECT_THROW(client.request("GET", "http://127.0.0.1:1/order?read=1", {}, {}, 0),
               std::invalid_argument);
  EXPECT_THROW(client.request("POST", "http://127.0.0.1:1/health?next=/order",
                                {}, {}, 0),
               std::invalid_argument);
  EXPECT_THROW(client.request("POST", "http://127.0.0.1:1/%zz", {}, {}, 0),
               std::invalid_argument);
  EXPECT_EQ(blocked, 0);
}

TEST(AuditNetwork, UrlSeparatesQueryFragmentsPortsAndIpv6Authority) {
  auto q = Url::parse("HTTPS://example.test?market=btc#client-fragment");
  EXPECT_EQ(q.scheme, "https");
  EXPECT_EQ(q.host, "example.test");
  EXPECT_EQ(q.port, "443");
  EXPECT_EQ(q.target, "/?market=btc");
  EXPECT_EQ(q.authority(), "example.test");
  EXPECT_EQ(q.authority(true), "example.test:443");
  auto v6 = Url::parse("https://[::1]:8443/book?asset=123#fragment");
  EXPECT_EQ(v6.host, "::1");
  EXPECT_EQ(v6.port, "8443");
  EXPECT_EQ(v6.target, "/book?asset=123");
  EXPECT_EQ(v6.authority(), "[::1]:8443");
  EXPECT_EQ(Url::parse("wss://[::1]/stream").authority(), "[::1]");
  EXPECT_EQ(Url::parse("http://example.test:8080/").authority(),
            "example.test:8080");
  EXPECT_EQ(Url::parse("http://example.test#fragment").target, "/");
}

TEST(AuditNetwork, UrlRefusesAmbiguousAuthorityAndControlBytes) {
  for (const auto *address : {"https://user:secret@example.test/",
                              "https://example.test:/", "https://example.test:0/",
                              "https://example.test:65536/",
                              "https://example.test:not-a-port/",
                              "https://::1/", "https://[::1/",
                              "https://[not-ipv6]/", "https://[::1]suffix/",
                              "https:///book", "ftp://example.test/",
                              "https://example.test/line\r\ninjection"})
    EXPECT_THROW(Url::parse(address), std::invalid_argument) << address;
}

TEST(AuditNetwork, ConnectProxyRequiresPlainHttpAndPreservesLoopbackBypass) {
  ScopedEnvironment lower("https_proxy", "http://lower-proxy.test:8081");
  ScopedEnvironment upper("HTTPS_PROXY", "http://upper-proxy.test:8080");
  auto destination = Url::parse("https://example.test/book");
  auto proxy = connect_proxy_for(destination);
  ASSERT_TRUE(proxy);
  EXPECT_EQ(proxy->host, "upper-proxy.test");
  EXPECT_EQ(proxy->port, "8080");
  for (const auto *loopback : {"https://localhost/", "https://127.0.0.1/",
                               "https://[::1]/"})
    EXPECT_FALSE(connect_proxy_for(Url::parse(loopback)));
  {
    ScopedEnvironment absent("HTTPS_PROXY", nullptr);
    ASSERT_TRUE(connect_proxy_for(destination));
    EXPECT_EQ(connect_proxy_for(destination)->host, "lower-proxy.test");
  }
  for (const auto *value : {"https://proxy.test:443", "socks5://proxy.test:1080",
                            "not-a-url"}) {
    ScopedEnvironment unsupported("HTTPS_PROXY", value);
    EXPECT_THROW(connect_proxy_for(destination), std::invalid_argument);
    EXPECT_FALSE(connect_proxy_for(Url::parse("https://127.0.0.1/")));
  }
}

TEST(AuditNetwork, WebsocketRequiresTlsAndFinitePositiveSilence) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  for (const auto *address : {"ws://127.0.0.1/stream", "https://127.0.0.1/stream"})
    EXPECT_THROW((WebSocket(io, tls, address, {}, {})), std::invalid_argument);
  for (double silence : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()})
    EXPECT_THROW((WebSocket(io, tls, "wss://127.0.0.1/stream", {}, {}, silence)),
                 std::invalid_argument);
}

TEST(AuditNetwork, Http2SetupDeadlineAndRepeatedStartHaveOneRetry) {
  net::io_context io;
  net::ssl::context tls(net::ssl::context::tls_client);
  tcp::acceptor listener(io, {net::ip::make_address("127.0.0.1"), 0});
  std::vector<std::shared_ptr<tcp::socket>> stalled;
  int accepted = 0;
  std::function<void()> accept = [&] {
    auto socket = std::make_shared<tcp::socket>(io);
    listener.async_accept(*socket, [&, socket](auto ec) {
      if (ec)
        return;
      ++accepted;
      stalled.push_back(socket); // accept TCP, then deliberately never do TLS
      accept();
    });
  };
  accept();
  std::atomic<std::uint64_t> blocked{0};
  int unknown = 0;
  H2Callbacks callbacks;
  callbacks.unknown = [&](auto, auto) { ++unknown; };
  H2Client client(io, tls, true, blocked, std::move(callbacks), 1, "127.0.0.1",
                  std::to_string(listener.local_endpoint().port()));
  net::steady_timer during_setup(io), end(io);
  during_setup.expires_after(std::chrono::milliseconds(400));
  during_setup.async_wait([&](auto ec) {
    if (!ec) {
      EXPECT_EQ(accepted, 1);
      client.start(); // repeated start during TLS must be idempotent
    }
  });
  end.expires_after(std::chrono::milliseconds(1650));
  end.async_wait([&](auto ec) {
    if (!ec) {
      EXPECT_FALSE(client.ready());
      EXPECT_EQ(accepted, 2); // one setup timeout, then one 250ms retry
      client.stop();
      boost::system::error_code ignored;
      listener.close(ignored);
      for (auto &socket : stalled)
        socket->close(ignored);
      io.stop();
    }
  });
  client.start();
  client.start();
  io.run_for(std::chrono::seconds(3));
  client.stop();
  EXPECT_EQ(unknown, 0); // setup did not authorize or submit any order
  EXPECT_EQ(blocked, 0);
}
