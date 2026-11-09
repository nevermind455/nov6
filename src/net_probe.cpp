// Read-only /time probe. No wallet, ledger, signing or order submission path.
#include "sniper/network.hpp"
#include <atomic>
#include <boost/asio/steady_timer.hpp>
#include <charconv>
#include <iostream>

namespace {
using namespace sniper;
namespace net = boost::asio;
struct Options {
  Url endpoint = Url::parse("https://clob.polymarket.com");
  std::string label;
  int samples = 30, interval_ms = 200, timeout_s = 10;
};
int integer(std::string_view text, int low, int high) {
  int value = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value < low || value > high)
    throw std::invalid_argument("integer argument is outside its supported range");
  return value;
}
void help() {
  std::cout << "sniper-net-probe [--endpoint https://clob.polymarket.com] "
               "[--samples 30] [--interval-ms 200] [--timeout-s 10] [--label ams3]\n"
               "Read-only GET /time over one TLS-verified HTTP/2 connection.\n"
               "Samples include local queuing, network and server processing; "
               "they do not isolate exchange-imposed execution delay.\n";
}
Options options(int argc, char **argv) {
  Options out;
  for (int i = 1; i < argc; ++i) {
    const std::string_view key = argv[i];
    if (i + 1 == argc)
      throw std::invalid_argument("option requires a value");
    const std::string_view value = argv[++i];
    if (key == "--endpoint") {
      out.endpoint = Url::parse(value);
      if (out.endpoint.scheme != "https" ||
          (out.endpoint.target != "/" && out.endpoint.target != "/time"))
        throw std::invalid_argument("endpoint must be HTTPS with no path or /time");
    } else if (key == "--samples")
      out.samples = integer(value, 1, 10000);
    else if (key == "--interval-ms")
      out.interval_ms = integer(value, 0, 60000);
    else if (key == "--timeout-s")
      out.timeout_s = integer(value, 1, 60);
    else if (key == "--label") {
      if (value.size() > 128)
        throw std::invalid_argument("location label is too long");
      out.label = value;
    } else
      throw std::invalid_argument("unknown option");
  }
  return out;
}
Json summary(std::vector<double> values) {
  Json out{{"count", values.size()}, {"percentile_method", "nearest_rank"},
           {"min_ms", nullptr}, {"p50_ms", nullptr},
           {"p95_ms", nullptr}, {"max_ms", nullptr}};
  if (values.empty())
    return out;
  std::sort(values.begin(), values.end());
  auto quantile = [&](double q) {
    return values[static_cast<std::size_t>(std::ceil(q * values.size())) - 1];
  };
  out["min_ms"] = values.front();
  out["p50_ms"] = quantile(.5);
  out["p95_ms"] = quantile(.95);
  out["max_ms"] = values.back();
  return out;
}
class Probe {
  Options options_;
  net::io_context io_;
  net::ssl::context tls_{net::ssl::context::tls_client};
  net::steady_timer next_{io_}, watchdog_{io_};
  std::atomic<std::uint64_t> blocked_{0};
  std::unique_ptr<H2Client> client_;
  std::optional<H2ConnectionInfo> connection_;
  std::vector<double> round_trips_;
  Json samples_ = Json::array();
  std::int64_t started_ns_ = 0, sent_ns_ = 0, finished_ns_ = 0;
  std::uint64_t watchdog_generation_ = 0;
  double started_wall_ = 0;
  bool proxy_ = false, done_ = false, success_ = false;
  std::string error_;
  void finish(bool success, std::string error = {}) {
    if (done_)
      return;
    done_ = true;
    success_ = success;
    finished_ns_ = mono_ns();
    error_ = std::move(error);
    next_.cancel();
    ++watchdog_generation_;
    watchdog_.cancel();
    if (client_)
      client_->stop();
    io_.stop();
  }
  void deadline(const char *reason) {
    const auto generation = ++watchdog_generation_;
    watchdog_.expires_after(std::chrono::seconds(options_.timeout_s));
    watchdog_.async_wait([this, reason, generation](auto ec) {
      if (!ec && generation == watchdog_generation_)
        finish(false, reason);
    });
  }
  void wait_ready() {
    if (done_)
      return;
    auto info = client_->connection_info();
    if (info) {
      if (info->ready_ns < started_ns_ || !info->tcp_no_delay || !info->tcp_keep_alive) {
        finish(false, "invalid connection setup evidence");
        return;
      }
      connection_ = info;
      ++watchdog_generation_;
      watchdog_.cancel();
      send();
      return;
    }
    next_.expires_after(std::chrono::milliseconds(1));
    next_.async_wait([this](auto ec) {
      if (!ec)
        wait_ready();
    });
  }
  void send() {
    if (done_)
      return;
    const auto info = client_->connection_info();
    if (!info || !connection_ || info->generation != connection_->generation) {
      finish(false, "connection unavailable or replaced; samples are not retried");
      return;
    }
    deadline("GET /time response timeout");
    sent_ns_ = mono_ns();
    if (!client_->get_time())
      finish(false, "GET /time could not be submitted");
  }
  void reply(std::string_view id, const HttpResult &response) {
    const auto received_ns = mono_ns();
    if (done_)
      return;
    try {
      const auto info = client_->connection_info();
      if (id != "__clock" || sent_ns_ <= 0 || received_ns < sent_ns_ ||
          !info || !connection_ || info->generation != connection_->generation)
        throw std::runtime_error("invalid or replaced probe response");
      if (response.status != 200)
        throw std::runtime_error("GET /time returned HTTP " + std::to_string(response.status));
      double server_seconds;
      try {
        server_seconds = number(response.json());
      } catch (...) {
        throw std::runtime_error("invalid GET /time timestamp response");
      }
      if (server_seconds <= 0 || server_seconds >= 0x1p63)
        throw std::runtime_error("GET /time timestamp is outside its valid range");
      const double ms = static_cast<double>(received_ns - sent_ns_) / 1e6;
      samples_.push_back({{"sequence", samples_.size() + 1},
                          {"sent_ns", sent_ns_}, {"received_ns", received_ns},
                          {"request_round_trip_ms", ms}, {"http_status", response.status},
                          {"server_time_seconds", server_seconds},
                          {"connection_generation", info->generation}});
      round_trips_.push_back(ms);
      sent_ns_ = 0;
      ++watchdog_generation_;
      watchdog_.cancel();
      if (round_trips_.size() == static_cast<std::size_t>(options_.samples)) {
        finish(true);
        return;
      }
      // Queue the next sample after nghttp2 has closed this response stream.
      next_.expires_after(std::chrono::milliseconds(options_.interval_ms));
      next_.async_wait([this](auto ec) {
        if (!ec)
          send();
      });
    } catch (const std::exception &e) {
      finish(false, e.what());
    }
  }
public:
  explicit Probe(Options options) : options_(std::move(options)) {
    proxy_ = connect_proxy_for(options_.endpoint).has_value();
    tls_.set_default_verify_paths();
    H2Callbacks callbacks;
    callbacks.reply = [this](auto id, const HttpResult &response) { reply(id, response); };
    callbacks.unknown = [this](auto, auto) {
      finish(false, "HTTP/2 request failed; samples are not retried");
    };
    client_ = std::make_unique<H2Client>(io_, tls_, false, blocked_, std::move(callbacks),
        options_.timeout_s, options_.endpoint.host, options_.endpoint.port);
  }
  Json run() {
    started_wall_ = wall_s();
    started_ns_ = mono_ns();
    deadline("HTTP/2 connection setup timeout");
    client_->start();
    wait_ready();
    try {
      io_.run();
    } catch (...) {
      finish(false, "native HTTP/2 probe handler failed");
    }
    if (!done_)
      finish(false, "probe stopped before completion");
    Json connection = nullptr, setup = nullptr;
    if (connection_) {
      setup = static_cast<double>(connection_->ready_ns - started_ns_) / 1e6;
      connection = {{"generation", connection_->generation},
                    {"ready_ns", connection_->ready_ns},
                    {"tcp_no_delay", connection_->tcp_no_delay},
                    {"tcp_keep_alive", connection_->tcp_keep_alive}};
    }
    auto warm = round_trips_;
    if (!warm.empty())
      warm.erase(warm.begin());
    return {{"native_cpp", true}, {"success", success_},
            {"endpoint", "https://" + options_.endpoint.authority() + "/time"},
            {"location_label", options_.label}, {"location_verified", false},
            {"configured_http_connect_proxy", proxy_},
            {"started_wall_seconds", started_wall_}, {"clock", "monotonic_nanoseconds"},
            {"connection_started_ns", started_ns_}, {"probe_finished_ns", finished_ns_},
            {"pending_request_sent_ns", sent_ns_ > 0 ? Json(sent_ns_) : Json(nullptr)},
            {"connection_setup_ms", setup},
            {"connection", std::move(connection)}, {"samples_requested", options_.samples},
            {"samples_completed", samples_.size()}, {"order_capable_calls", blocked_.load()},
            {"first_request_round_trip_ms", round_trips_.empty() ? Json(nullptr) : Json(round_trips_.front())},
            {"warm_request_round_trip_ms", summary(std::move(warm))},
            {"measurement", "GET /time RTT includes client queuing, network and server processing; it is not pure WAN latency or exchange-imposed execution delay"},
            {"timing_boundaries", {{"sent_ns", "before GET /time enqueue, not a socket-write timestamp"},
                                   {"received_ns", "entry into the complete HTTP/2 response callback"},
                                   {"ready_ns", "TLS, ALPN and local HTTP/2 initialization; not peer SETTINGS acknowledgement"}}},
            {"error", error_.empty() ? Json(nullptr) : Json(error_)},
            {"samples", std::move(samples_)}};
  }
};
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      help();
      return 0;
    }
    auto parsed = options(argc, argv);
    try {
      Probe probe(std::move(parsed));
      auto result = probe.run();
      const bool success = result.at("success").get<bool>();
      std::cout << result.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
      return success ? 0 : 1;
    } catch (const std::exception &) {
      std::cout << Json{{"native_cpp", true}, {"success", false},
                       {"samples_completed", 0}, {"order_capable_calls", 0},
                       {"error", "probe setup failed"}}.dump() << '\n';
      return 1;
    }
  } catch (const std::exception &e) {
    std::cerr << "sniper-net-probe: " << e.what() << '\n';
    return 2;
  }
}
