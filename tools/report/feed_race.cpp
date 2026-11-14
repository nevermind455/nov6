// sniper-feed-race: which BTC feed fires the bot's trigger first ON THIS
// SERVER - Coinbase (the bot's primary feed), Binance spot or Binance USD-M
// futures (SNIPER_EXTRA_FEEDS).
//
//   sniper-feed-race [minutes]        default 60; Ctrl+C prints the report so far
//
// Read-only: public market data only, no key, no order, no file written. It
// runs at the lowest CPU priority and uses the bot's own websocket client,
// trigger rule (include/sniper/trigger.hpp) and Binance message parsers.
//
// Each feed's mid (best bid + best ask) / 2 goes through the rule: a move of
// at least SNIPER_K_BP (0.5) bp from the price SNIPER_WINDOW_MS (250) ms
// earlier. A feed's move starts at its first signal after a second with no
// signal that way. Moves the same way on different feeds within a second of
// each other are one move; the report says which feed fired first and how many
// milliseconds the others came after. Times are when each message reached this
// server, so they include each exchange's distance from it.
#include "sniper/book.hpp"
#include "sniper/feeds.hpp"
#include "sniper/network.hpp"
#include "sniper/sniper.hpp"
#include "sniper/trigger.hpp"
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <unistd.h>

using namespace sniper;
namespace net = boost::asio;

namespace {
constexpr double QUIET_MS = 1000;     // a new move on a feed: no signal that way for 1 s
constexpr double SAME_MOVE_MS = 1000; // the same move on another feed: within 1 s
const std::vector<std::string> NAMES{"coinbase", "binance spot", "binance futures"};
const std::map<std::string, std::string> FEED{{"binance spot", "binance_spot"},
                                              {"binance futures", "binance_fut"}};

std::string sf(const char *f, auto... a) {
  char b[512];
  int n = std::snprintf(b, sizeof b, f, a...);
  return n < 0 ? std::string() : std::string(b, std::min<std::size_t>(n, sizeof b - 1));
}
double env_or(const char *name, double fallback) {
  const char *v = std::getenv(name);
  if (!v || !*v)
    return fallback;
  try {
    return std::stod(v);
  } catch (...) {
    return fallback;
  }
}
std::optional<double> pct(std::vector<double> v, double q) {
  if (v.empty())
    return std::nullopt;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))];
}

struct Move {
  int way;
  double first;
  std::map<std::string, double> at;
};
struct Race {
  std::map<std::pair<std::string, int>, double> last;
  std::vector<Move> open, done;
  std::map<std::string, long long> moves;
  void close(double t) {
    std::vector<Move> still;
    for (auto &m : open)
      (t - m.first > SAME_MOVE_MS ? done : still).push_back(m);
    open = std::move(still);
  }
  void signal(const std::string &name, int way, double t) {
    auto key = std::pair{name, way};
    auto it = last.find(key);
    bool quiet = it == last.end() || t - it->second > QUIET_MS;
    last[key] = t;
    if (!quiet)
      return; // the move this feed already reported is still going
    ++moves[name];
    close(t);
    for (auto &m : open)
      if (m.way == way && !m.at.count(name) && t - m.first <= SAME_MOVE_MS) {
        m.at[name] = t;
        return;
      }
    open.push_back(Move{way, t, {{name, t}}});
  }
};
struct Feed {
  std::string name;
  std::unique_ptr<WebSocket> ws;
  Trigger trigger{0.5, 250};
  long long msgs = 0, reconnects = 0;
  std::deque<double> age; // arrival wall clock less the exchange's time, ms
  std::string error;
};
} // namespace

int main(int argc, char **argv) {
  const double minutes = argc > 1 ? std::atof(argv[1]) : 60.0;
  if (!(minutes > 0)) {
    std::cerr << "usage: sniper-feed-race [minutes]\n";
    return 2;
  }
  if (::nice(19) == -1) {
  }
  const double k_bp = env_or("SNIPER_K_BP", 0.5), window_ms = env_or("SNIPER_WINDOW_MS", 250);
  net::io_context io;
  net::ssl::context tls{net::ssl::context::tls_client};
  tls.set_default_verify_paths();
  Race race;
  std::map<std::string, Feed> feeds;
  auto now_ms = [] { return mono_s() * 1000.0; };
  const double started = now_ms();

  for (const auto &name : NAMES) {
    auto &f = feeds[name];
    f.name = name;
    f.trigger = Trigger(k_bp, window_ms);
    const bool coinbase = name == "coinbase";
    const std::string url = coinbase ? "wss://ws-feed.exchange.coinbase.com" : feeds::url(FEED.at(name));
    f.ws = std::make_unique<WebSocket>(
        io, tls, url,
        [&, name, coinbase](std::string_view raw, int) {
          const double t = now_ms();
          auto &g = feeds[name];
          std::optional<double> mid, event_ms;
          if (coinbase) {
            auto d = Json::parse(raw, nullptr, false);
            if (!d.is_object() || d.value("type", "") != "ticker" || d.value("product_id", "") != "BTC-USD")
              return;
            double bid = 0, ask = 0;
            try {
              bid = std::stod(d.value("best_bid", "0"));
              ask = std::stod(d.value("best_ask", "0"));
            } catch (...) {
              return;
            }
            if (!(bid > 0 && ask >= bid))
              return;
            mid = (bid + ask) / 2;
            event_ms = iso_ms(d.contains("time") ? d["time"] : Json());
          } else {
            auto tick = feeds::parse(FEED.at(name), raw);
            event_ms = tick.event_ms;
            if (tick.kind == feeds::Tick::Book)
              mid = tick.mid;
            else if (tick.kind != feeds::Tick::Trade)
              return;
          }
          ++g.msgs;
          if (event_ms) {
            g.age.push_back(wall_s() * 1000.0 - *event_ms);
            if (g.age.size() > 20000)
              g.age.pop_front();
          }
          if (!mid)
            return;
          auto [way, r] = g.trigger.update(t, *mid);
          (void)r;
          if (way)
            race.signal(name, *way, t);
        },
        [&, name, coinbase](bool up) {
          auto &g = feeds[name];
          g.trigger = Trigger(k_bp, window_ms);
          if (up) {
            g.error.clear();
            if (coinbase)
              g.ws->send(R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker","heartbeat"]})");
          } else {
            ++g.reconnects;
            g.error = g.ws->last_failure();
          }
        },
        5.0, false);
  }

  bool stop = false;
  net::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](auto ec, int) {
    if (!ec)
      stop = true;
  });
  std::printf("listening to coinbase, binance spot, binance futures for %g min (Ctrl+C to stop early) ...\n",
              minutes);
  std::fflush(stdout);
  for (auto &[n, f] : feeds)
    f.ws->start();
  net::steady_timer timer(io);
  double next_note = started + 300000;
  std::function<void()> tick = [&] {
    const double t = now_ms();
    if (stop || t - started >= minutes * 60000) {
      for (auto &[n, f] : feeds)
        f.ws->stop();
      signals.cancel();
      io.stop();
      return;
    }
    if (t >= next_note) {
      next_note += 300000;
      std::printf("%.0f min: moves coinbase %lld, binance spot %lld, binance futures %lld\n",
                  (t - started) / 60000, race.moves["coinbase"], race.moves["binance spot"],
                  race.moves["binance futures"]);
      std::fflush(stdout);
    }
    timer.expires_after(std::chrono::milliseconds(200));
    timer.async_wait([&](auto ec) {
      if (!ec)
        tick();
    });
  };
  tick();
  io.run();

  // ---- the report
  race.close(now_ms() + 2 * SAME_MOVE_MS);
  const double mins = (now_ms() - started) / 60000.0;
  const auto &moves = race.done;
  std::printf("FEED RACE  %.0f min on this server  (trigger %g bp in %g ms)\n", mins, k_bp, window_ms);
  for (const auto &name : NAMES) {
    auto &f = feeds[name];
    auto age = pct(std::vector<double>(f.age.begin(), f.age.end()), 0.5);
    std::string line = sf("  %-16s %6.1f msg/s  %5lld moves", name.c_str(),
                          static_cast<double>(f.msgs) / std::max(mins * 60, 1.0), race.moves[name]);
    if (age)
      line += sf("  arrives %.0f ms after the exchange (median)", *age);
    if (f.reconnects)
      line += sf("  reconnects %lld", f.reconnects);
    if (!f.msgs && !f.error.empty())
      line += "  NO DATA: " + f.error.substr(0, 120);
    std::puts(line.c_str());
  }
  std::vector<const Move *> multi;
  for (auto &m : moves)
    if (m.at.size() >= 2)
      multi.push_back(&m);
  std::printf("moves seen on 2+ feeds: %zu of %zu\n", multi.size(), moves.size());
  if (!multi.empty()) {
    std::map<std::string, long long> first;
    for (auto *m : multi)
      ++first[std::min_element(m->at.begin(), m->at.end(), [](auto &a, auto &b) { return a.second < b.second; })->first];
    std::string line = "  fired first:";
    for (const auto &name : NAMES)
      line += sf("   %s %.0f%%", name.c_str(), static_cast<double>(first[name]) * 100 / static_cast<double>(multi.size()));
    std::puts(line.c_str());
  }
  for (auto [a, b] : std::vector<std::pair<std::string, std::string>>{
           {"coinbase", "binance futures"}, {"coinbase", "binance spot"}, {"binance spot", "binance futures"}}) {
    std::vector<double> lead; // > 0: b fired first
    for (auto &m : moves)
      if (m.at.count(a) && m.at.count(b))
        lead.push_back(m.at.at(a) - m.at.at(b));
    if (lead.empty())
      continue;
    const double n = static_cast<double>(lead.size());
    const double med = *pct(lead, 0.5);
    std::printf("%s vs %s, %zu moves on both:\n", a.c_str(), b.c_str(), lead.size());
    std::printf("  %s first %.0f%%, %s first %.0f%%\n", b.c_str(),
                std::count_if(lead.begin(), lead.end(), [](double x) { return x > 0; }) * 100 / n, a.c_str(),
                std::count_if(lead.begin(), lead.end(), [](double x) { return x < 0; }) * 100 / n);
    std::printf("  %s ahead by %.0f ms (median; middle half %+.0f to %+.0f ms, + = %s first)\n",
                (med >= 0 ? b : a).c_str(), std::abs(med), *pct(lead, 0.25), *pct(lead, 0.75), b.c_str());
  }
  std::vector<double> late;
  long long mine = 0;
  for (auto &m : moves) {
    if (!m.at.count("coinbase") || m.at.size() < 2)
      continue;
    ++mine;
    double other = INFINITY;
    for (auto &[n, t] : m.at)
      if (n != "coinbase")
        other = std::min(other, t);
    if (m.at.at("coinbase") - other > 0)
      late.push_back(m.at.at("coinbase") - other);
  }
  if (mine)
    std::printf("YOUR BOT (coinbase): another feed fired first in %.0f%% of the %lld moves coinbase also made%s\n",
                static_cast<double>(late.size()) * 100 / static_cast<double>(mine), mine,
                late.empty() ? "" : sf(", by %.0f ms (median)", *pct(late, 0.5)).c_str());
  std::map<std::string, long long> alone;
  for (auto &m : moves)
    if (m.at.size() == 1)
      ++alone[m.at.begin()->first];
  if (!alone.empty()) {
    std::string line = "  moved on one feed only:";
    for (const auto &name : NAMES)
      line += sf("   %s %lld", name.c_str(), alone[name]);
    std::puts(line.c_str());
  }
  return 0;
}
