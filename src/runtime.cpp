#include "sniper/runtime.hpp"
#include "sniper/dashboard.hpp"
#include "sniper/paper.hpp"
#include "sniper/sniper.hpp"
#include <mutex>
#include "sniper/strategy.hpp"
#include "sniper/venue.hpp"
#include <atomic>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <iostream>
#include <openssl/crypto.h>
#include <simdjson.h>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
namespace sniper {
namespace net = boost::asio;
namespace execution {
Units spendable_collateral(Units sampled_balance, Units sampled_outflow,
                           Units current_outflow) {
  if (sampled_balance < 0 || sampled_outflow < 0 || current_outflow < sampled_outflow)
    throw std::invalid_argument("invalid collateral snapshot watermark");
  auto available = static_cast<__int128>(sampled_balance) -
                   (static_cast<__int128>(current_outflow) - sampled_outflow);
  return available > 0 ? static_cast<Units>(available) : 0;
}
std::set<std::string> account_markets(const Ledger &ledger,
                                     std::set<std::string> active) {
  for (const auto &[id, intent] : ledger.orders) {
    (void)id;
    if (!ledger.settled_conditions.count(intent.condition) &&
        (intent.unresolved() || intent.status == "matched" || intent.booked_shares > 0))
      active.insert(intent.condition);
  }
  return active;
}
bool pending_trade_status(std::string_view status) {
  return status == "MATCHED" || status == "MATCHED_NOT_BROADCASTED" ||
         status == "MINED" || status == "RETRYING";
}
bool refresh_round_rules(Round &current, const Round &fresh) {
  if (current.condition != fresh.condition || current.up != fresh.up ||
      current.down != fresh.down || current.start != fresh.start || current.end != fresh.end)
    return false;
  current.rules[0] = fresh.rules[0];
  current.rules[1] = fresh.rules[1];
  if (fresh.ptb) current.ptb = fresh.ptb;
  return true;
}
bool cache_round_rules(std::map<std::string, MarketRules> &cache,
                       const Round &round, double fetched_wall) {
  if (!round.rules[0].loaded || !round.rules[1].loaded)
    return false;
  MarketRules rules;
  rules.condition_id = round.condition;
  rules.fee_rate = py::D(dollars(round.rules[0].rate));
  rules.fee_exponent = round.rules[0].exponent;
  rules.min_order_size = py::D(dollars(round.rules[0].min_shares));
  rules.tick_size = py::D(dollars(round.rules[0].tick));
  rules.up_token_id = round.up;
  rules.down_token_id = round.down;
  rules.taker_delay_ms = 0.0;
  rules.taker_delay_flagged = true;
  rules.fetched_wall = fetched_wall;
  cache[round.condition] = std::move(rules);
  while (cache.size() > 64)
    cache.erase(std::min_element(
        cache.begin(), cache.end(), [](auto &a, auto &b) {
          return a.second.fetched_wall < b.second.fetched_wall;
        }));
  return true;
}
} // namespace execution
struct Runtime::Impl {
  Config c;
  net::io_context io;
  net::ssl::context tls{net::ssl::context::tls_client};
  net::signal_set signals{io, SIGINT, SIGTERM};
  net::steady_timer timer{io};
  Ledger ledger;
  Journal journal;
  std::unique_ptr<Journal> recorder;
  std::unique_ptr<Signer> signer;
  Credentials creds;
  std::unique_ptr<FileLock> wallet_lock;
  std::unique_ptr<H2Client> sender;
  std::vector<std::unique_ptr<WebSocket>> coinbase;
  std::unique_ptr<WebSocket> market, user;
  BookState book{8.0, 30.0, nullptr, nullptr, [this] { return venue_wall(); }};
  ServerClock clock;
  // the reference's decision engine, paper broker and hub state
  struct Env;
  std::unique_ptr<Env> env;
  std::unique_ptr<Sniper> sniper;
  std::unique_ptr<PaperBroker> paper;
  std::map<std::string, MarketRules> market_rules; // broker _rules_cache
  double live_max_spread = 0.25;                  // MAX_ALLOWED_SPREAD
  std::optional<Round> hub, prepared;              // hub current / prepared
  std::int64_t hub_window = -1;
  std::set<std::string> resync_wanted;
  std::map<std::string, std::pair<std::uint64_t, double>> next_fetch,
      next_ws_snapshot;
  struct BookFetch {
    std::string token;
    std::uint64_t generation = 0;
    long long updates = 0, revision = 0;
  };
  std::mutex fetch_mutex;
  std::deque<BookFetch> fetch_queue;
  std::set<std::string> fetching;
  std::condition_variable fetch_wake;
  std::ofstream rec_out;
  double next_bankroll = 0, next_hybrid = 0, ptb_asked = -1e30;
  net::steady_timer paper_timer{io};
  bool paper_timer_armed = false;
  double venue_wall() const {
    double o = clock.at >= 0 ? clock.offset : 0.0;
    return wall_s() + (std::abs(o) > 2.0 ? o : 0.0);
  }
  std::atomic<std::uint64_t> blocked{0};
  simdjson::dom::parser parser;
  std::map<std::int64_t, Round> rounds;
  std::set<std::string> subscribed, user_markets;
  // Orders still open when this run started. New orders wait until the
  // reconciliation has resolved them; a timed pause would outlast startup.
  std::set<std::string> inherited_orders;
  std::vector<bool> cb_open, cb_ack;
  std::vector<double> cb_last_tick;
  std::optional<std::uint64_t> sequence;
  std::string inflight_id[2];
  double start_mono = mono_s(), last_report = 0, last_snapshot = 0,
         last_probe = -1e30, probe_sent = 0, probe_wall = 0;
  double balance_at = -1, risk_at = 0, user_pong = -1, last_sub = 0,
         pause_until = 0, stop_started = 0, dd_started = 0;
  std::optional<Units> balance;
  Units booked_outflow = 0, balance_outflow = 0;
  std::optional<Units> effective_balance() const {
    return balance ? std::optional(execution::spendable_collateral(*balance, balance_outflow, booked_outflow))
                   : std::nullopt;
  }
  std::map<std::string, Big> allowances;
  std::string pause_reason;
  std::set<std::string> pause_latches;
  std::deque<double> slow_replies;
  std::map<std::string, double> sent_at, skip_last;
  std::map<std::string, std::array<std::int64_t, 8>> stamps;
  std::map<std::string, Units> provisional;
  std::map<std::string, std::map<std::string, Units>> private_sizes;
  std::uint64_t signals_seen = 0, orders_sent = 0, paper_fills = 0, errors = 0,
                raw_id = 0;
  std::uint64_t snapshot_entries = 0;
  std::map<std::string, std::uint64_t> skips;
  double last_recovery = -1e30;
  bool stopping = false, startup_ok = false;
  int exit_code = 0;

  struct Work {
    std::vector<Round> unsettled;
    std::vector<std::string> conditions;
    std::vector<std::string> unresolved;
    double since = 0;
    std::int64_t auth_time = 0;
    double auth_mono = 0;
    Units booked_outflow = 0;
  };
  std::atomic<std::shared_ptr<const Work>> work;
  std::atomic<bool> workers_stop{false}, worker_done{false},
      refresher_done{false}, settler_done{false};
  std::thread worker, refresher, refresher2, settler;
  std::atomic<int> refreshers_live{2};
  std::mutex wake_mutex;
  std::condition_variable wake;

  // The terminal dashboard (--dash). All of this is read and written on the
  // event loop only; what reaches the dashboard's thread is a finished copy.
  struct DashSeries {
    std::deque<double> values;
    bool simulated = false;
  };
  struct DashQuality {
    long long attempted = 0, sent = 0, acked = 0, pending = 0, filled = 0,
              partial = 0, zero_fill = 0, rejected = 0, failed = 0, slip_n = 0;
    double slip_sum = 0;
    Json last;
  };
  std::unique_ptr<dash::Dashboard> dashboard;
  double next_dash = 0, dash_every = 0.5, dash_last_tick = 0, dash_tick_gap = 0;
  std::deque<Json> dash_orders, dash_events, dash_settled;
  std::deque<std::pair<double, std::uint64_t>> dash_signals;
  std::optional<std::pair<double, double>> dash_last_signal;
  Json dash_last_fair;
  std::map<std::string, DashSeries> dash_latency;
  Json dash_latency_json = Json::object();
  bool dash_latency_dirty = false;
  DashQuality dash_quality;
  double dash_started_wall = wall_s();
  std::set<std::string> dash_live_acknowledged;
  std::map<std::string, double> dash_live_decision_asks;
  std::string dash_live_last_fill_order;
  std::set<std::pair<std::string, std::string>> dash_live_latency_recorded;
  long long dash_wins = 0, dash_losses = 0;
  std::vector<std::string> dash_faults;
  std::uint64_t dry_orders = 0;

  explicit Impl(Config cfg)
      : c(std::move(cfg)), ledger(c.ledger, c.live ? 0 : c.start_cash, c.live),
        journal(c.journal) {
    tls.set_default_verify_paths();
    if (parser.allocate(8192) != simdjson::SUCCESS)
      throw std::runtime_error("simdjson parser allocation failed");
    cb_open.resize(c.conns);
    cb_ack.resize(c.conns);
    cb_last_tick.resize(c.conns);
    if (!c.record.empty()) {
      rec_out.open(c.record, std::ios::app);
      if (!rec_out)
        throw std::runtime_error("cannot open --record file");
    }
    if (c.live) {
      signer = std::make_unique<Signer>(c.private_key);
      if (!signer->self_test())
        throw std::runtime_error("native signer recovery self-test failed");
      auto expected = wallet_for(signer->address(), c.signature_type);
      if (c.funder.empty())
        c.funder = expected;
      else if (checksum_address(c.funder) != expected)
        throw std::runtime_error("POLY_FUNDER does not match SDK wallet "
                                 "derivation for this signer/type");
      c.funder = expected;
      wallet_lock = std::make_unique<FileLock>(
          std::filesystem::temp_directory_path() /
          ("sniper-native-wallet-" +
           hex(keccak(c.funder), false).substr(0, 24) + ".lock"));
      creds = {c.api_key, c.api_secret, c.passphrase};
      if (!creds.valid())
        creds = derive_credentials(RestClient(true, &blocked), *signer);
      OPENSSL_cleanse(c.private_key.data(), c.private_key.size());
      c.private_key.clear();
      auto canary_file = c.state / "live_canary_state.json";
      if (c.new_session) {
        if (std::filesystem::exists(canary_file))
          std::filesystem::copy_file(
              canary_file, canary_file.string() + ".bak",
              std::filesystem::copy_options::overwrite_existing);
        CanaryState state;
        state.started = wall_s();
        ledger.set_canary(state);
        atomic_file(canary_file, Json{{"format", "sniper-native-canary-1"},
                                      {"canary", ledger.canary.json()}}
                                     .dump());
      } else {
        std::ifstream file(canary_file);
        Json state;
        if (!file || !(file >> state) ||
            state.value("format", "") != "sniper-native-canary-1")
          throw std::runtime_error(
              "native live canary state missing or unreadable; deliberately "
              "create one with --new-canary-session");
        if (std::abs(state.at("canary").at("started").get<double>() -
                     ledger.canary.started) > 1e-6)
          throw std::runtime_error(
              "canary session and durable ledger disagree");
      }
      auto closed =
          RestClient(true, &blocked)
              .get("https://clob.polymarket.com/auth/ban-status/closed-only",
                   l2_headers(*signer, creds, "GET",
                              "/auth/ban-status/closed-only"));
      if (!closed.contains("closed_only") ||
          closed.at("closed_only").get<bool>())
        throw std::runtime_error(
            "CLOB account is closed-only or could not be verified");
    }
    for (auto &[id, i] : ledger.orders) {
      auto total = static_cast<__int128>(booked_outflow) + i.booked_cost + i.booked_fee;
      if (total < 0 || total > std::numeric_limits<Units>::max())
        throw std::overflow_error("historical collateral outflow overflow");
      booked_outflow = static_cast<Units>(total);
      if (!ledger.settled_conditions.count(i.condition)) {
        Round r;
        r.condition = i.condition;
        r.up = i.up;
        r.down = i.down;
        r.slug = i.slug;
        r.start = i.start;
        r.end = i.end;
        rounds[r.start] = r;
        if (i.unresolved() || i.status == "matched") {
          if (c.live)
            inherited_orders.insert(id);
          else
            ledger.status(id, "cancelled");
        }
      }
    }
    H2Callbacks cb;
    cb.written = [this](auto id, auto t) {
      if (auto it = stamps.find(std::string(id)); it != stamps.end()) {
        it->second[5] = t;
        if (dashboard)
          dash_live_timings(std::string(id), timing_fields(id));
      }
    };
    cb.reply = [this](auto id, const HttpResult &result) {
      on_reply(id, result);
    };
    cb.unknown = [this](auto id, auto why) { on_unknown(id, why); };
    sender = std::make_unique<H2Client>(io, tls, c.live, blocked, std::move(cb),
                                        c.send_timeout_s);
    market = std::make_unique<WebSocket>(
        io, tls, "wss://ws-subscriptions-clob.polymarket.com/ws/market",
        [this](auto raw, int) {
          if (raw == "PONG")
            return;
          rec_frame("mkt", raw);
          int refused = 0;
          try {
            refused = handle_market_frame(book, raw);
          } catch (...) {
            ++errors;
          }
          if (refused) // poly_market._request_resync
            for (auto &t : book.needs_resync())
              resync_wanted.insert(t);
          paper_poll();
        },
        [this](bool up) {
          rec(Json{{"k", "mconn"}, {"up", up}});
          dash_event("market", up ? "connected" : "disconnected", up ? "good" : "warn");
          book.connected = up;
          book.desync_all(up ? "connect" : "disconnect");
          if (up) {
            for (auto &t : book.needs_resync())
              resync_wanted.insert(t);
            if (!subscribed.empty()) {
              Json sub{{"assets_ids", std::vector<std::string>(
                                          subscribed.begin(), subscribed.end())},
                       {"type", "market"},
                       {"custom_feature_enabled", true}};
              market->send(sub.dump());
            }
          }
        },
        25, true);
    for (int n = 0; n < c.conns; ++n)
      coinbase.push_back(std::make_unique<WebSocket>(
          io, tls, "wss://ws-feed.exchange.coinbase.com",
          [this, n](auto raw, int backlog) { on_coinbase(n, raw, backlog); },
          [this, n](bool up) {
            bool any = std::any_of(cb_open.begin(), cb_open.end(),
                                   [](bool v) { return v; });
            cb_open[n] = up;
            cb_ack[n] = false;
            cb_last_tick[n] = mono_s();
            dash_event("coinbase",
                       "socket " + std::to_string(n + 1) +
                           (up ? " connected" : " disconnected"),
                       up ? "good" : "warn");
            if (up) {
              if (!any) { // the first socket up restarts the rule
                rec(Json{{"k", "cbconn"}, {"up", true}});
                sniper->on_coinbase_connect();
              }
              coinbase[n]->send(
                  R"({"type":"subscribe","product_ids":["BTC-USD"],"channels":["ticker","heartbeat"]})");
            } else if (std::none_of(cb_open.begin(), cb_open.end(),
                                    [](bool v) { return v; })) {
              rec(Json{{"k", "cbconn"}, {"up", false}});
              sniper->on_coinbase_disconnect();
            }
          },
          c.silent, false));
    if (c.live)
      user = std::make_unique<WebSocket>(
          io, tls, "wss://ws-subscriptions-clob.polymarket.com/ws/user",
          [this](auto raw, int) { on_user(raw); },
          [this](bool up) {
            user_pong = -1;
            if (up) {
              Json sub{
                  {"auth",
                   {{"apiKey", creds.key},
                    {"secret", creds.secret},
                    {"passphrase", creds.passphrase}}},
                  {"type", "user"},
                  {"markets", std::vector<std::string>(user_markets.begin(),
                                                       user_markets.end())}};
              last_sub = mono_s();
              user->send(sub.dump());
              user->send("PING");
            }
          },
          25, true);
    work.store(std::make_shared<Work>());
    build_engine();
    refresh_work();
  }
  ~Impl() {
    if (dashboard)
      dashboard->stop();
    workers_stop = true;
    wake.notify_all();
    fetch_wake.notify_all();
    if (worker.joinable())
      worker.join();
    if (refresher.joinable())
      refresher.join();
    if (refresher2.joinable())
      refresher2.join();
    if (settler.joinable())
      settler.join();
    if (sender)
      sender->stop();
    for (auto &ws : coinbase)
      ws->stop();
    if (market)
      market->stop();
    if (user)
      user->stop();
  }
  void audit(Json row) {
    if (dashboard)
      dash_note(row);
    if (!journal.write(std::move(row))) {
      pause_reason = "journal disk failure or queue overflow";
      pause_until = std::numeric_limits<double>::infinity();
    }
  }

  // ------------------------------------------------------------ dashboard
  // Port of run.py's DashView and JournalTail (nevermind455/oct10@772f2eb). A
  // fault in any of this is shown on the dashboard and never reaches trading.
  static std::string dash_str(const Json &row, const char *key) {
    auto it = row.find(key);
    return it != row.end() && it->is_string() ? it->get<std::string>()
                                              : std::string();
  }
  static double dash_number(const Json &row, const char *key, double fallback) {
    auto it = row.find(key);
    return it != row.end() && it->is_number() ? it->get<double>() : fallback;
  }
  static Json dash_field(const Json &row, const char *key) {
    auto it = row.find(key);
    return it == row.end() ? Json() : *it;
  }
  static bool dash_has(std::string text, const char *needle) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });
    return text.find(needle) != std::string::npos;
  }
  void dash_event(const std::string &source, const std::string &text,
                  const char *level = "info") {
    if (!dashboard)
      return;
    // A repeating message (a failing round, a flapping socket) keeps one line.
    if (!dash_events.empty() && dash_events.back()[1] == source &&
        dash_events.back()[2] == text) {
      dash_events.back()[0] = wall_s();
      return;
    }
    dash_events.push_back(Json::array({wall_s(), source, text, level}));
    while (dash_events.size() > 40)
      dash_events.pop_front();
  }
  void dash_sample(const char *key, double value, bool simulated) {
    if (!std::isfinite(value))
      return;
    auto &series = dash_latency[key];
    series.simulated = simulated;
    series.values.push_back(value);
    while (series.values.size() > 1000)
      series.values.pop_front();
    dash_latency_dirty = true;
  }
  void dash_quality_last(const Json &row, std::optional<double> slip) {
    Json last = Json::object();
    for (auto key : {"side", "signal_mid", "ask_seen", "cap", "price", "shares"})
      last[key] = dash_field(row, key);
    last["slip_c"] = slip ? Json(*slip) : Json();
    dash_quality.last = std::move(last);
  }
  void dash_filled(const Json &row, double ask, double price) {
    auto &q = dash_quality;
    ++(row.value("partial", false) ? q.partial : q.filled);
    std::optional<double> slip;
    if (ask > 0 && price > 0) {
      slip = (price - ask) * 100.0;
      q.slip_sum += *slip;
      ++q.slip_n;
    }
    dash_quality_last(row, slip);
  }
  void dash_live_timings(const std::string &id, const Json &row) {
    const auto samples = dash::execution_latencies(row);
    for (const auto &[metric, sample] : samples.items())
      if (dash_live_latency_recorded.emplace(id, metric).second)
        dash_sample(metric.c_str(), sample.get<double>(), false);
  }
  // A row that adds to an order already listed: live, its reply and its fills
  // arrive after the row that listed it.
  bool dash_update_order(const Json &row, const std::string &result) {
    auto id = dash_str(row, "order_id");
    if (id.empty())
      return false;
    for (auto &old : dash_orders) {
      if (dash_str(old, "order_id") != id)
        continue;
      auto reason = dash_str(row, "reason");
      auto i = ledger.orders.find(id);
      if (c.live && i != ledger.orders.end() && i->second.booked_shares > 0) {
        const auto &intent = i->second;
        const double shares = dollars(intent.booked_shares);
        const double price = dollars(intent.booked_cost) / shares;
        old["result"] = "filled";
        old["shares"] = shares;
        old["price"] = price;
        old["detail"] = intent.status == "confirmed"
                            ? (intent.booked_cost < intent.stake &&
                                       intent.stake - intent.booked_cost > 1
                                   ? "partial" : "")
                            : "confirmed trades; awaiting final total";
      } else if (result == "refused") {
        bool miss = dash_has(reason, "no match") || dash_has(reason, "no-fill");
        old["result"] = miss ? "missed" : "refused";
        old["detail"] = reason.substr(0, 120);
      } else if (c.live) {
        old["result"] = i != ledger.orders.end() && i->second.status == "unknown"
                            ? "unknown" : "pending";
        old["shares"] = nullptr;
        old["price"] = nullptr;
        old["detail"] = !reason.empty() ? reason.substr(0, 120)
                           : i != ledger.orders.end() && i->second.matched_shares > 0
                               ? "matched; awaiting confirmation" : "awaiting reply";
      }
      return true;
    }
    return false;
  }
  void dash_take(const Json &row) {
    auto result = dash_str(row, "result"), reason = dash_str(row, "reason"),
         side = dash_str(row, "side");
    double wall = dash_number(row, "wall", wall_s());
    if (c.live) {
      const auto id = dash_str(row, "order_id");
      if (!id.empty() && row.value("acknowledged", false)) {
        dash_live_acknowledged.insert(id);
        dash_live_timings(id, row);
      }
    }
    if (!side.empty() && row.contains("bp") && row["bp"].is_number())
      dash_last_signal = std::pair{wall, row["bp"].get<double>()};
    if (!side.empty() && row.contains("fair") && row["fair"].is_number())
      dash_last_fair = Json{
          {"side", side},
          {"market_mid", dash_field(row, "market_mid")},
          {"fair", row["fair"]},
          {"limit", dash_field(row, "fair_limit")},
          {"shares", row.contains("shares") ? row["shares"]
                                            : dash_field(row, "kelly_shares")}};
    if (result == "guard" || (result == "error" && side.empty()))
      dash_event(result, reason, "bad");
    else if (result == "discovery_refused")
      dash_event("discovery", reason, "warn");
    else if (result == "coinbase_reconnect")
      dash_event("coinbase", reason, "warn");
    else if (result == "start")
      dash_event("bot", reason.empty() ? "started" : reason);
    else if (result == "stop")
      dash_event("bot", "stopping: " + reason, "warn");
    else if (result == "hybrid_drawdown_pause")
      dash_event("hybrid", "drawdown stop reached: paused", "warn");
    else if (result == "hybrid_drawdown_reset")
      dash_event("hybrid", "drawdown pause over: risk rebased", "warn");
    else if (result == "refused" && side.empty() && !row.contains("order_id"))
      dash_event("bot", reason, "bad");
    if (result != "filled" && result != "refused" && result != "pending" &&
        result != "error" && result != "live_fill_update" && result != "live_fill")
      return;
    if (c.live && result == "live_fill")
      if (auto i = ledger.orders.find(dash_str(row, "order_id"));
          i != ledger.orders.end() && i->second.sent_wall >= dash_started_wall)
        dash_live_last_fill_order = i->first;
    if (c.live && result == "live_fill")
      dash_event("sniper", "CONFIRMED " + side + " " +
                              py::fixed(dash_number(row, "shares", 0), 1) + " shares",
                 "good");
    if (dash_update_order(row, result) || side.empty())
      return;
    if (c.live && (result == "live_fill" || result == "live_fill_update"))
      return; // historical fills do not become new order attempts/table rows
    // The first row of one order.
    auto &q = dash_quality;
    ++q.attempted;
    if (!c.live && row.contains("loop_lag_ms") && row["loop_lag_ms"].is_number())
      dash_sample("trigger_to_decision", row["loop_lag_ms"].get<double>(), false);
    if (c.live) {
      if (result != "error")
        ++q.sent;
      if (auto ask = dash_number(row, "ask_seen", 0); ask > 0)
        dash_live_decision_asks[dash_str(row, "order_id")] = ask;
    } else {
      if (result != "error" && !dash_has(reason, "signing failed")) {
        ++q.sent;
        ++q.acked; // paper: the simulated venue always answers
      }
      if (row.contains("latency_ms") && row["latency_ms"].is_number())
        dash_sample("trigger_to_ack", row["latency_ms"].get<double>(), true);
    }
    auto label = result;
    if (result == "pending")
      ++q.pending;
    else if (result == "filled")
      dash_filled(row, dash_number(row, "ask_seen", 0), dash_number(row, "price", 0));
    else if (result == "error")
      ++q.failed;
    else {
      bool miss = dash_has(reason, "no-fill");
      ++(miss ? q.zero_fill : q.rejected);
      if (miss)
        label = "missed";
      dash_quality_last(row, std::nullopt);
    }
    auto why = reason.empty() ? dash_str(row, "outcome") : reason;
    dash_orders.push_back(Json{
        {"time", dash::et_time(wall)},
        {"side", side},
        {"bp", dash_field(row, "bp")},
        {"ask", dash_field(row, "ask_seen")},
        {"cap", dash_field(row, "cap")},
        {"result", label},
        {"shares", dash_field(row, "shares")},
        {"price", dash_field(row, "price")},
        {"order_id", dash_str(row, "order_id")},
        {"detail", row.value("partial", false) ? std::string("partial")
                   : result == "filled"        ? std::string()
                                               : why.substr(0, 120)},
        {"round_end", dash_field(row, "round_end")}});
    while (dash_orders.size() > 40)
      dash_orders.pop_front();
    if (result == "filled")
      dash_event("sniper",
                 "FILLED " + side + " " + py::fixed(dash_number(row, "shares", 0), 1) +
                     " @ " + py::fixed(dash_number(row, "price", 0), 3),
                 "good");
  }
  void dash_note(const Json &row) {
    try {
      dash_take(row);
    } catch (const std::exception &e) {
      dash_event("dashboard", std::string("journal row skipped: ") + e.what(), "warn");
    }
  }
  void dash_note_settlement(const Round &r, const std::map<std::string, Units> &payouts) {
    if (!dashboard)
      return;
    try {
      double realized = 0;
      bool has_position = false;
      std::string winner;
      for (const auto *token : {&r.up, &r.down}) {
        auto pay = payouts.find(*token);
        if (pay == payouts.end())
          continue;
        if (pay->second == U)
          winner = token == &r.up ? "UP" : "DOWN";
        auto pos = ledger.positions.find(*token);
        if (pos == ledger.positions.end() || pos->second.shares <= 0)
          continue;
        has_position = true;
        Units pnl = multiply(pos->second.shares, pay->second) - pos->second.cost -
                    pos->second.fees;
        realized += dollars(pnl);
        if (pnl > 0)
          ++dash_wins;
        else if (pnl < 0)
          ++dash_losses;
      }
      auto label = dash::et_label(static_cast<double>(r.start), static_cast<double>(r.end));
      auto shown = winner.empty() ? std::string("SPLIT") : winner;
      dash_settled.push_front(Json{{"label", label},
                                   {"state", "SETTLED"},
                                   {"winner", shown},
                                   {"realized", realized},
                                   {"has_position", has_position},
                                   {"settlement_s", wall_s() - static_cast<double>(r.end)},
                                   {"settlement_price", winner.empty() ? 0.5 : 1.0},
                                   {"redeem", c.live ? "MANUAL" : "N/A (PAPER)"},
                                   {"condition_id", r.condition}});
      while (dash_settled.size() > 5)
        dash_settled.pop_back();
      dash_event("settle", "settled " + label + " " + shown + " " + py::fixed(realized, 2),
                 realized < 0 ? "warn" : "good");
    } catch (const std::exception &e) {
      dash_event("dashboard", std::string("settlement row skipped: ") + e.what(), "warn");
    }
  }
  void dash_refresh_latency() {
    if (!dash_latency_dirty)
      return;
    dash_latency_dirty = false;
    Json out = Json::object();
    for (auto &[key, series] : dash_latency) {
      std::vector<double> v(series.values.begin(), series.values.end());
      if (v.empty())
        continue;
      double last = v.back(), sum = 0;
      for (double x : v)
        sum += x;
      std::sort(v.begin(), v.end());
      auto at = [&](double q) {
        return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
      };
      out[key] = Json{{"n", v.size()},
                      {"last", last},
                      {"avg", sum / static_cast<double>(v.size())},
                      {"p50", at(0.50)},
                      {"p95", at(0.95)},
                      {"p99", at(0.99)},
                      {"max", v.back()},
                      {"simulated", series.simulated}};
    }
    dash_latency_json = std::move(out);
  }
  Json dash_snapshot(double now) {
    double wall = wall_s();
    Json snap = Json::object();
    snap["mode"] = c.live ? "LIVE" : "PAPER";
    snap["clock"] = dash::et_clock(wall);
    auto counter = [&](const char *key) -> std::uint64_t {
      auto it = sniper->n.find(key);
      return it == sniper->n.end() ? 0 : it->second;
    };
    auto optional_json = [](std::optional<double> v) { return v ? Json(*v) : Json(); };
    dash_refresh_latency();
    Json live_quality = c.live
                            ? dash::live_execution_quality(ledger.orders,
                                  dash_live_acknowledged, dash_live_decision_asks,
                                  dash_started_wall, dash_live_last_fill_order)
                            : Json();
    auto p50 = [&](const char *key) {
      auto it = dash_latency_json.find(key);
      return it == dash_latency_json.end() ? Json() : (*it)["p50"];
    };
    // ---- trigger
    const auto &q = sniper->trigger.q.values;
    auto mid = q.empty() ? sniper->last_mid : std::optional<double>(q.back().second);
    Json move;
    if (q.size() >= 2 && q.front().second != 0)
      move = (q.back().second / q.front().second - 1.0) * 1e4;
    auto signals = counter("signals");
    dash_signals.emplace_back(now, signals);
    while (dash_signals.size() > 2 && now - dash_signals.front().first > 300)
      dash_signals.pop_front();
    Json rate;
    if (now - dash_signals.front().first >= 10)
      rate = static_cast<double>(signals - dash_signals.front().second) /
             (now - dash_signals.front().first) * 60.0;
    const auto &fresh = sniper->cb_fresh;
    std::string status =
        !fresh.enabled     ? "OFF"
        : !fresh.connected ? "DISCONNECTED"
        : !fresh.last_msg_mono || (now - *fresh.last_msg_mono) * 1000.0 > fresh.unknown_ms
            ? "STALE"
        : fresh.why_not(now).empty() ? "LIVE"
                                     : "DEGRADED";
    std::string busy;
    if (c.live && hub ? ledger.awaiting(hub->up) : sniper->inflight[0].has_value())
      busy = "UP";
    if (c.live && hub ? ledger.awaiting(hub->down) : sniper->inflight[1].has_value())
      busy += busy.empty() ? "DOWN" : "+DOWN";
    auto filled = c.live ? live_quality["filled"].get<std::uint64_t>() +
                              live_quality["partial"].get<std::uint64_t>()
                         : counter("filled");
    auto refused = c.live ? live_quality["zero_fill"].get<std::uint64_t>() +
                               live_quality["rejected"].get<std::uint64_t>() +
                               live_quality["failed"].get<std::uint64_t>()
                          : counter("refused");
    Json trigger{
        {"feed", c.s.primary_feed},
        {"status", status},
        {"mid", optional_json(mid)},
        {"move_bp", move},
        {"k_bp", c.s.k_bp},
        {"window_ms", c.s.window_ms},
        {"signals", signals},
        {"signals_per_min", rate},
        {"orders", c.live ? live_quality["sent"] : Json(counter("orders"))},
        {"filled", filled},
        {"refused", refused},
        {"fill_rate", filled + refused
                          ? Json(static_cast<double>(filled) / static_cast<double>(filled + refused))
                          : Json()},
        {"order_type", c.s.order_type},
        {"slip", c.s.slip},
        {"send_p50_ms", p50("trigger_to_send")},
        {"ack_p50_ms", c.live ? p50("send_to_ack") : p50("trigger_to_ack")},
        {"ack_simulated", !c.live && dash_latency_json.contains("trigger_to_ack")},
        {"errors", errors + counter("errors")},
        {"in_flight", busy.empty() ? Json() : Json(busy)},
        {"last_signal_bp", dash_last_signal ? Json(dash_last_signal->second) : Json()},
        {"last_signal_age_s", dash_last_signal ? Json(wall - dash_last_signal->first) : Json()}};
    if (c.live)
      trigger["fill_rate"] = live_quality["fill_rate"];
    if (c.s.fair) {
      auto v = sniper->vol.vol();
      trigger["fair"] = Json{{"vol_bp", v ? Json(*v * 1e4) : Json()},
                             {"why", v ? Json() : Json("volatility warming up")},
                             {"last", dash_last_fair}};
    }
    snap["trigger"] = std::move(trigger);
    // ---- what was skipped, most frequent first
    std::vector<std::pair<std::string, std::uint64_t>> skips;
    for (auto &[key, value] : sniper->n)
      if (value && key.rfind("skip: ", 0) == 0)
        skips.emplace_back(key.substr(6), value);
    std::sort(skips.begin(), skips.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    Json skip_rows = Json::array();
    for (std::size_t i = 0; i < skips.size() && i < 6; ++i)
      skip_rows.push_back(Json::array({skips[i].first, skips[i].second}));
    snap["skips"] = std::move(skip_rows);
    // ---- execution
    const auto &quality = dash_quality;
    long long any_fill = quality.filled + quality.partial;
    snap["exec"] = Json{
        {"latency", dash_latency_json},
        {"paper", !c.live},
        {"quality",
         Json{{"attempted", quality.attempted},
              {"sent", quality.sent},
              {"acked", quality.acked},
              {"pending", quality.pending},
              {"filled", quality.filled},
              {"partial", quality.partial},
              {"zero_fill", quality.zero_fill},
              {"rejected", quality.rejected},
              {"failed", quality.failed},
              {"fill_rate", quality.acked ? Json(static_cast<double>(any_fill) /
                                                 static_cast<double>(quality.acked))
                                          : Json()},
              {"reject_rate", quality.sent ? Json(static_cast<double>(quality.rejected) /
                                                  static_cast<double>(quality.sent))
                                           : Json()},
              {"avg_slip_c", quality.slip_n ? Json(quality.slip_sum /
                                                   static_cast<double>(quality.slip_n))
                                            : Json()},
              {"slip_n", quality.slip_n},
              {"last", quality.last}}}};
    if (c.live)
      snap["exec"]["quality"] = live_quality;
    // ---- market
    double end = hub_window >= 0 ? static_cast<double>(hub_window + 300) : 0.0;
    Json market = Json::object();
    if (hub_window >= 0)
      market["seconds_left"] = end - wall;
    auto book_side = [&](const std::string &token) {
      auto v = book.view(token);
      Json age;
      if (v.updated_mono)
        age = (now - *v.updated_mono) * 1000.0;
      return Json{{"bid", optional_json(v.best_bid())},
                  {"ask", optional_json(v.best_ask())},
                  {"status", v.status},
                  {"age_ms", age}};
    };
    if (hub) {
      market["slug"] = "btc-updown-5m-" + std::to_string(hub_window);
      market["up"] = book_side(hub->up);
      market["down"] = book_side(hub->down);
    }
    market["trade_until_s"] = sniper->window().second;
    auto round = sniper->rounds.find(end);
    market["round_fills"] = round == sniper->rounds.end() ? 0LL : round->second.fills;
    market["round_up_sh"] = round == sniper->rounds.end() ? 0.0 : round->second.up;
    market["round_down_sh"] = round == sniper->rounds.end() ? 0.0 : round->second.down;
    if (c.live && hub) {
      market["round_fills"] = ledger.round_fills(hub->condition);
      market["round_up_sh"] = dollars(ledger.holding(hub->up));
      market["round_down_sh"] = dollars(ledger.holding(hub->down));
    }
    market["max_fills"] = c.s.max_fills_round;
    market["max_side_shares"] = c.s.max_side_shares;
    snap["market"] = std::move(market);
    // ---- orders in this market only: the table clears when a new one starts
    Json orders = Json::array();
    for (auto &o : dash_orders) {
      double order_end = dash_number(o, "round_end", -1);
      if (hub && std::abs(order_end - end) < 0.5)
        orders.push_back(o);
    }
    snap["orders"] = std::move(orders);
    // ---- positions and account. Positions in the market trading now are marked
    // to its live bid; ended markets are carried at cost until their result.
    Json up_row{{"side", "UP"}, {"shares", 0.0}, {"avg", nullptr}, {"cost", 0.0}, {"fees", 0.0}};
    Json down_row{{"side", "DOWN"}, {"shares", 0.0}, {"avg", nullptr}, {"cost", 0.0}, {"fees", 0.0}};
    std::set<std::string> pending_markets;
    double pending_shares = 0, pending_cost = 0, pending_value = 0, pending_fees = 0;
    for (auto &[token, p] : ledger.positions) {
      if (p.settled || p.shares <= 0)
        continue;
      double shares = dollars(p.shares), cost = dollars(p.cost), fees = dollars(p.fees);
      if (hub && p.condition == hub->condition && (token == hub->up || token == hub->down)) {
        Json &row = token == hub->up ? up_row : down_row;
        row["shares"] = shares;
        row["avg"] = cost / shares;
        row["cost"] = cost + fees;
        row["fees"] = fees;
        continue;
      }
      pending_markets.insert(p.condition);
      pending_shares += shares;
      pending_cost += cost + fees;
      pending_value += cost; // preserve the documented notional-cost carry
      pending_fees += fees;
    }
    double active_value = 0, unrealized = -pending_fees;
    int unmarkable = 0;
    auto mark = [&](Json &row, const std::string *token) {
      double shares = row["shares"].get<double>();
      const double fees = row["fees"].get<double>();
      row.update(dash::mark_position(shares, row["cost"].get<double>() - fees,
                                    fees, token ? book.view(*token) : BookView{},
                                    now, c.s.stale_ms));
      if (shares > 0 && row["value"].is_null())
        ++unmarkable;
      else if (shares > 0) {
        active_value += row["value"].get<double>();
        unrealized += row["pnl"].get<double>();
      }
    };
    mark(up_row, hub ? &hub->up : nullptr);
    mark(down_row, hub ? &hub->down : nullptr);
    snap["positions"] = Json{{"rows", Json::array({up_row, down_row})},
                             {"pending",
                              Json{{"markets", pending_markets.size()},
                                   {"shares", pending_shares},
                                   {"cost", pending_cost}}}};
    double realized = dollars(ledger.realized);
    Json position_value = unmarkable ? Json() : Json(active_value + pending_value);
    Json account{{"realized", realized},
                 {"unrealized", unmarkable ? Json() : Json(unrealized)},
                 {"position_value", position_value},
                 {"fees", dollars(ledger.fees)},
                 {"wins", dash_wins},
                 {"losses", dash_losses}};
    if (c.live) {
      account["kind"] = "LIVE";
      auto collateral = effective_balance();
      account.update(dash::live_balance_view(collateral ? std::optional(dollars(*collateral))
                                                       : std::nullopt,
                                              now - balance_at));
      account["equity"] = !account["balance"].is_null() && !unmarkable
                              ? Json(account["balance"].get<double>() + active_value + pending_value)
                              : Json();
      account["total"] = unmarkable ? Json() : Json(realized + unrealized);
    } else {
      double cash = dollars(ledger.cash);
      account["kind"] = "PAPER";
      account["cash"] = cash;
      account["equity"] = unmarkable ? Json() : Json(cash + active_value + pending_value);
      account["total"] = unmarkable ? Json() : Json(realized + unrealized);
    }
    snap["account"] = std::move(account);
    // ---- settlement
    Json pending = Json::array();
    for (auto &[start, r] : rounds) {
      if (static_cast<double>(r.end) >= wall || ledger.settled_conditions.count(r.condition))
        continue;
      if (!ledger.holding(r.up) && !ledger.holding(r.down) && !ledger.awaiting(r.up) &&
          !ledger.awaiting(r.down))
        continue;
      double age = wall - static_cast<double>(r.end);
      pending.push_back(Json{
          {"label", dash::et_label(static_cast<double>(r.start), static_cast<double>(r.end))},
          {"state", "PENDING SETTLEMENT"},
          {"age_s", age},
          {"detail", age < 55 ? "the result is read 55s after the round ends"
                              : "waiting for the on-chain result"},
          {"condition_id", r.condition}});
    }
    Json active;
    if (hub)
      active = Json{{"label", dash::et_label(static_cast<double>(hub->start),
                                             static_cast<double>(hub->end))},
                    {"state", "ACTIVE"},
                    {"condition_id", hub->condition}};
    Json previous = Json::array();
    for (auto &row : dash_settled)
      previous.push_back(row);
    Json settled = dash_settled.empty() ? Json() : dash_settled.front();
    std::size_t waiting = pending.size();
    Json subject = waiting ? pending.front() : !settled.is_null() ? settled : active;
    snap["settlement"] = Json{{"active", active},
                              {"subject", subject},
                              {"pending", std::move(pending)},
                              {"previous", std::move(previous)},
                              {"last_settled", settled},
                              {"pending_count", waiting}};
    // ---- the live canary
    if (c.live) {
      Json halted = Json::array();
      std::error_code ec;
      if (wall < pause_until)
        halted.push_back(pause_reason);
      if (std::filesystem::exists(c.stop_file, ec))
        halted.push_back("STOP_LIVE present: delete the stop file to allow new orders");
      if (journal.failed())
        halted.push_back("audit or recorder failed");
      const auto &state = ledger.canary;
      std::size_t unresolved = 0;
      for (auto &[id, i] : ledger.orders)
        if (i.unresolved() || i.status == "matched")
          ++unresolved;
      snap["canary"] = Json{
          {"halted", std::move(halted)},
          {"orders", state.orders},
          {"max_orders", c.canary_orders},
          {"fills", state.fills},
          {"max_fills", c.canary_fills},
          {"spend", dollars(state.spend)},
          {"max_spend", dollars(c.canary_spend)},
          {"rounds", state.rounds.size()},
          {"max_rounds", c.canary_rounds},
          {"loss", dollars(state.settled_loss + ledger.open_cost())},
          {"max_loss", dollars(c.canary_loss)},
          {"guard", wall < pause_until ? "PAUSED" : "OK"},
          {"guard_reason", wall < pause_until ? pause_reason : std::string()},
          {"awaiting", unresolved}};
    }
    // ---- network and health
    Json net_ms{{"coinbase", nullptr}, {"clob", nullptr}};
    if (fresh.last_trade_mono && now - *fresh.last_trade_mono <= 10 && !fresh.min_q.empty() &&
        fresh.last_excess)
      net_ms["coinbase"] = fresh.min_q.front().second + *fresh.last_excess;
    if (!clock.rtts.empty())
      net_ms["clob"] = clock.median_rtt();
    snap["net"] = std::move(net_ms);
    Json health = Json::array();
    auto add = [&](const char *name, const std::string &state, const std::string &detail) {
      health.push_back(Json::array({name, state, detail}));
    };
    auto sockets_up = std::count(cb_open.begin(), cb_open.end(), true);
    add("COINBASE", status,
        "sockets " + std::to_string(sockets_up) + "/" + std::to_string(cb_open.size()) +
            " behind " +
            (fresh.last_excess ? py::fixed(*fresh.last_excess, 0) + "ms" : std::string("--")) +
            " backlog max " + std::to_string(fresh.backlog_max_seen));
    std::string book_state = "WAIT";
    if (!book.connected)
      book_state = "DISCONNECTED";
    else if (hub) {
      auto a = book.status(hub->up), b = book.status(hub->down);
      book_state = a == LIVE && b == LIVE ? std::string(LIVE) : a == LIVE ? b : a;
    }
    add("POLY BOOK", book_state, "resync waiting " + std::to_string(resync_wanted.size()));
    add("CLOB H2", sender->ready() ? "OK" : "WAIT", "");
    if (c.live)
      add("USER WS", user && user->connected() ? "OK" : "DOWN", "");
    else
      add("USER WS", "ABSENT", "paper");
    add("SETTLE", "OK", "pending " + std::to_string(waiting));
    add("LEDGER", "OK", "entries " + std::to_string(ledger.entries));
    add("JOURNAL", journal.failed() ? "ERROR" : "OK", "");
    add("CLOCK", clock.at < 0 ? "WAIT" : now - clock.at > 60 ? "WARN" : "OK",
        clock.at < 0 ? std::string() : "offset " + py::fixed(clock.offset, 2) + "s");
    double lag_ms = std::max(0.0, (dash_tick_gap - 0.05) * 1000.0);
    dash_tick_gap = 0;
    add("LOOP LAG", lag_ms < 100 ? "OK" : "WARN", "max " + py::fixed(lag_ms, 0) + "ms");
    snap["health"] = std::move(health);
    Json events = Json::array();
    for (auto &e : dash_events)
      events.push_back(e);
    snap["events"] = std::move(events);
    return snap;
  }
  void dash_submit(double now) {
    try {
      dashboard->submit(dash_snapshot(now));
    } catch (const std::exception &e) {
      dash_event("dashboard", std::string("snapshot failed: ") + e.what(), "bad");
    }
  }
  // What a dry run came to find out: the wallet the venue sees and whether it
  // could pay for an order. The wallet address is public; no key is shown.
  void dry_run_report(const Round &r) {
    auto allowance = [&](std::string_view exchange) -> Json {
      for (auto &[address, amount] : allowances)
        if (checksum_address(address) == checksum_address(exchange))
          return amount.convert_to<double>() / static_cast<double>(U);
      return Json();
    };
    Json report{{"result", "dry_run_ready"},
                {"wallet", c.funder},
                {"signature_type", c.signature_type},
                {"collateral", balance ? Json(dollars(*balance)) : Json()},
                {"allowance_standard_exchange", Json()},
                {"allowance_neg_risk_exchange", Json()},
                {"this_round_uses_neg_risk", r.rules[0].neg},
                {"note", "live checks passed; a dry run sends no order"}};
    try {
      report["allowance_standard_exchange"] = allowance(STANDARD_EXCHANGE);
      report["allowance_neg_risk_exchange"] = allowance(NEG_EXCHANGE);
    } catch (const std::exception &e) {
      report["allowance_error"] = e.what();
    }
    if (dashboard)
      dash_event("dry run", "live checks passed; no order is sent", "good");
    else
      std::cout << report.dump() << std::endl;
    audit(std::move(report));
  }
  void runtime_fault(const std::string &what) {
    if (dashboard) {
      dash_event("runtime", "fault: " + what, "bad");
      dash_faults.push_back(what);
    } else
      std::cerr << "native runtime fault: " << what << '\n';
  }

  // ------------------------------------------------------------ engine
  struct Env : SniperEnv {
    Impl &r;
    explicit Env(Impl &i) : r(i) {}
    double mono() override { return mono_s(); }
    double wall() override { return wall_s(); }
    std::string up_token() override { return r.hub ? r.hub->up : ""; }
    std::string down_token() override { return r.hub ? r.hub->down : ""; }
    std::string condition_id() override { return r.hub ? r.hub->condition : ""; }
    std::optional<double> window_end() override {
      if (r.hub_window < 0)
        return std::nullopt;
      return static_cast<double>(r.hub_window + 300);
    }
    BookState &book() override { return r.book; }
    bool live() override { return r.c.live; }
    std::optional<double> read_bankroll() override {
      if (r.c.live) {
        auto balance = r.effective_balance();
        if (!balance)
          return std::nullopt;
        return dollars(*balance - r.ledger.reserved());
      }
      return dollars(r.ledger.cash);
    }
    const MarketRules *cached_rules(const std::string &cond, bool ttl) override {
      auto it = r.market_rules.find(cond);
      if (it == r.market_rules.end())
        return nullptr;
      if (ttl) {
        double age = wall_s() - it->second.fetched_wall;
        if (!(0 <= age && age <= 300.0))
          return nullptr;
      }
      return &it->second;
    }
    double open_leg_shares(const std::string &cond,
                           const std::string &token) override {
      auto it = r.ledger.positions.find(token);
      if (it == r.ledger.positions.end() || it->second.condition != cond ||
          it->second.settled || it->second.shares <= 0)
        return 0.0;
      return dollars(it->second.shares);
    }
    std::optional<double> own_risk_shares(const std::string &token,
                                          const std::string &) override {
      if (!r.c.live)
        return std::nullopt;
      return dollars(r.ledger.risk_shares(token));
    }
    std::optional<double> order_share_cap() override {
      if (!r.c.live)
        return std::nullopt;
      return static_cast<double>(r.c.canary_shares);
    }
    bool awaiting_fak(const std::string &, const std::string &,
                      const std::string &token) override {
      return r.c.live && r.ledger.awaiting(token);
    }
    double trade_window_seconds() override { return r.c.trade_last; }
    double min_seconds_to_expiry() override { return r.c.min_expiry; }
    double min_buy_price() override { return r.c.min_price; }
    double max_buy_price() override { return r.c.max_price; }
    double own_fill_ttl_s() override {
      return r.paper ? r.paper->cfg.own_fill_ttl_s : 0.0;
    }
    bool has_recent_own_fill(const std::string &token) override {
      return r.paper && r.paper->has_recent_own_fill(token);
    }
    std::optional<PxLevels> net_asks(const BookView &view) override {
      if (!r.paper)
        return std::nullopt;
      return r.paper->net_asks(view);
    }
    bool hybrid_initialized() override { return r.ledger.high_water > 0; }
    long long journal_seq() override {
      return static_cast<long long>(r.ledger.entries);
    }
    double open_basis() override { return dollars(r.ledger.open_cost()); }
    Json hybrid_risk_view(std::optional<double> reference,
                          std::optional<long long>, bool rebase) override {
      auto &L = r.ledger;
      if (L.high_water <= 0) {
        if (!reference)
          throw py::ValueError(
              "first hybrid risk reading needs positive reference equity");
        if (*reference <= 0)
          throw py::ValueError("hybrid reference equity must be positive");
        L.risk_rebase(units(fixed6(*reference)));
      }
      Units equity = L.reference_cash + L.realized;
      if (rebase) {
        if (!(equity > 0))
          throw py::ValueError("a hybrid epoch reset needs an established "
                               "epoch and positive equity");
        L.risk_rebase(equity);
      }
      L.high_water = std::max(L.high_water, equity);
      return Json{{"reference_equity", dollars(L.reference_cash)},
                  {"high_water", dollars(L.high_water)},
                  {"equity", dollars(equity)},
                  {"realized_total", dollars(L.realized)}};
    }
    void journal(Json row) override { r.audit(std::move(row)); }
    std::vector<double> net_rtt() override {
      return {r.clock.rtts.begin(), r.clock.rtts.end()};
    }
    bool dispatch_live(const std::shared_ptr<Plan> &p) override {
      return r.dispatch_live(p);
    }
  };
  std::unique_ptr<Journal> paper_audit;
  double setting(const char *name, double fallback) {
    auto v = c.raw(name);
    if (!v || v->find_first_not_of(" \t") == std::string::npos)
      return fallback;
    double x = py::parse_float(*v);
    if (!std::isfinite(x))
      throw std::invalid_argument(std::string(name) + " must be finite");
    return x;
  }
  void build_engine() {
    env = std::make_unique<Env>(*this);
    sniper = std::make_unique<Sniper>(c.s, *env);
    sniper->cb_fresh.enabled = c.s.coinbase_freshness;
    sniper->closing = c.health;
    // what the ledger already holds counts into this round's caps (seed_rounds)
    std::map<std::string, std::pair<double, long long>> base;
    for (auto &[token, pos] : ledger.positions)
      if (!pos.settled && pos.shares > 0) {
        long long fills = 0;
        for (auto &[id, i] : ledger.orders)
          if (i.token == token && i.booked_shares > 0)
            ++fills;
        base[token] = {dollars(pos.shares), fills};
      }
    sniper->seed_rounds(base);
    live_max_spread = setting("MAX_ALLOWED_SPREAD", 0.25);
    if (!c.live) {
      std::vector<Json> rows;
      std::ifstream in(c.journal);
      std::string line;
      while (std::getline(in, line)) {
        if (line.find("minimum_entry_attempted") == std::string::npos)
          continue;
        try {
          rows.push_back(Json::parse(line));
        } catch (...) {
        }
      }
      sniper->seed_minimum_entries(rows);
      PaperConfig pc;
      pc.max_buy_price = c.max_price;
      pc.min_buy_price = c.min_price;
      pc.latency_ms = setting("PAPER_LATENCY_MS", 150);
      pc.max_book_age_s = setting("ORDERBOOK_MAX_AGE_SECONDS", 8.0);
      pc.max_quiet_s = setting("ORDERBOOK_MAX_QUIET_SECONDS", 900.0);
      pc.future_tol_s = setting("ORDERBOOK_FUTURE_TOLERANCE_SECONDS", 5.0);
      pc.max_spread = setting("MAX_ALLOWED_SPREAD", 0.25);
      pc.min_seconds_to_expiry = c.min_expiry;
      pc.trade_window_seconds = c.trade_last;
      pc.flagged_delay_ms = setting("PAPER_FLAGGED_DELAY_SECONDS", 1.0) * 1000.0;
      pc.own_fill_ttl_s = std::max(0.0, setting("PAPER_OWN_FILL_TTL_S", 2.0));
      PaperHooks h;
      h.mono = [] { return mono_s(); };
      h.unix_wall = [] { return wall_s(); };
      h.venue_wall = [this] { return venue_wall(); };
      h.context = [this] {
        return hub ? std::make_tuple(hub->condition, hub->up, hub->down)
                   : std::make_tuple(std::string(), std::string(), std::string());
      };
      h.rules = [this](const std::string &cond) -> const MarketRules * {
        auto it = market_rules.find(cond);
        return it == market_rules.end() ? nullptr : &it->second;
      };
      h.cash_balance = [this] { return dollars(ledger.cash); };
      h.record_fill = [this](const Plan &p, const FillQuote &q) {
        return record_paper_fill(p, q);
      };
      auto audit_path = c.journal.parent_path() / "paper_audit.jsonl";
      paper_audit = std::make_unique<Journal>(audit_path);
      h.audit = [this](Json row) {
        if (paper_audit)
          paper_audit->write(std::move(row));
      };
      paper = std::make_unique<PaperBroker>(pc, std::move(h));
    }
    next_bankroll = mono_s();
    next_hybrid = mono_s() + 5.0;
  }
  // a Decimal amount as micro-units (half-even at the sixth decimal)
  static Units dec_units(const py::Dec &d) {
    auto r = py::rescale(d, -6, py::Rounding::HalfEven);
    Units v = r.coef.convert_to<Units>();
    return r.neg ? -v : v;
  }
  std::string record_paper_fill(const Plan &p, const FillQuote &q) {
    static const char *hexd = "0123456789abcdef";
    std::string tail;
    unsigned char rnd[6];
    RAND_bytes(rnd, sizeof rnd);
    for (unsigned char b : rnd) {
      tail += hexd[b >> 4];
      tail += hexd[b & 15];
    }
    Intent intent;
    intent.id = "paper-" +
                std::to_string(static_cast<long long>(wall_s() * 1000)) + "-" +
                tail;
    intent.condition = p.cond;
    intent.token = p.token;
    intent.side = p.side;
    intent.up = p.up;
    intent.down = p.down;
    intent.end = static_cast<std::int64_t>(p.end);
    intent.start = intent.end - 300;
    if (hub && hub->condition == p.cond)
      intent.slug = hub->slug;
    intent.stake = dec_units(q.notional);
    intent.reserve = dec_units(q.total_cost());
    intent.target = p.shares ? *p.shares * U : units(fixed6(p.risk_shares));
    intent.limit = units(fixed6(std::min(p.cap, c.max_price)));
    intent.fee_rate = p.fair_fee_rate ? units(fixed6(*p.fair_fee_rate)) : 0;
    intent.fee_exponent = p.fair_fee_exponent.value_or(1);
    intent.sent_wall = p.sent_wall;
    intent.min_entry = p.minimum_entry;
    intent.status = "pending";
    ledger.register_intent(intent, nullptr, ledger.canary);
    Units shares = dec_units(q.shares), cost = dec_units(q.notional),
          fee = dec_units(q.fee);
    ledger.status(intent.id, "matched", shares, cost);
    if (!ledger.fill(intent.id + "-fill", intent.id, shares, cost, fee))
      throw std::runtime_error("paper fill id collision");
    return intent.id;
  }
  // ------------------------------------------------------------ recorder
  void rec(Json e) {
    if (!rec_out.is_open())
      return;
    double t = mono_s();
    e["t"] = t;
    e["w"] = wall_s();
    rec_out << e.dump(-1, ' ', false, Json::error_handler_t::replace) << '\n';
    if (!rec_out) {
      rec_out.close();
      pause("recorder disk failure", true);
      begin_stop(1);
    }
  }
  void rec_frame(const char *kind, std::string_view raw, int backlog = -1) {
    if (!rec_out.is_open())
      return;
    Json e{{"k", kind}, {"raw", std::string(raw)}};
    if (backlog >= 0)
      e["backlog"] = backlog;
    rec(std::move(e));
  }
  Json rules_json(const Round &r) {
    auto it = market_rules.find(r.condition);
    if (it == market_rules.end())
      return Json();
    auto &m = it->second;
    return Json{{"condition_id", m.condition_id},
                {"fee_rate", m.fee_rate.str()},
                {"fee_exponent", m.fee_exponent},
                {"min_order_size", m.min_order_size ? m.min_order_size->str() : "5"},
                {"tick_size", m.tick_size ? m.tick_size->str() : "0.01"},
                {"up_token_id", m.up_token_id.value_or("")},
                {"down_token_id", m.down_token_id.value_or("")},
                {"taker_delay_ms", m.taker_delay_ms.value_or(0.0)},
                {"taker_delay_flagged", m.taker_delay_flagged}};
  }
  // ------------------------------------------------------------ the hub
  bool activate_rules(const Round &r) {
    return execution::cache_round_rules(market_rules, r, wall_s());
  }
  void update_hub() {
    double now_w = wall_s();
    auto window = static_cast<std::int64_t>(now_w) / 300 * 300;
    bool changed = false, hub_new = false, prepared_new = false;
    if (window != hub_window) {
      hub_window = window;
      if (prepared && prepared->start == window) {
        hub = prepared;
        prepared.reset();
        hub_new = true;
      } else
        hub.reset();
      // A stall (a paused VM, a blocked loop) can pass the prepared round.
      // Keeping it would block every later preparation.
      if (prepared && prepared->start < window)
        prepared.reset();
      changed = true;
    }
    if (!hub) {
      auto it = rounds.find(window);
      if (it != rounds.end() && it->second.rules[0].loaded) {
        hub = it->second;
        changed = hub_new = true;
      }
    }
    double remain = static_cast<double>(window + 300) - now_w;
    // Prepare the next round anywhere inside the lead window, not only in a
    // fixed 25-30 s slot: a discovery pass that lands outside that slot
    // otherwise leaves the next round cold until its boundary.
    if (!prepared && remain > 0.0 && remain <= c.prepare_lead) {
      auto it = rounds.find(window + 300);
      if (it != rounds.end() && it->second.rules[0].loaded) {
        prepared = it->second;
        changed = prepared_new = true;
      }
    }
    if (!changed)
      return;
    std::set<std::string> want;
    for (auto *r : {&hub, &prepared})
      if (*r) {
        want.insert((*r)->up);
        want.insert((*r)->down);
        activate_rules(**r);
        if (!(r == &hub ? hub_new : prepared_new))
          continue;
        resync_wanted.insert((*r)->up);
        resync_wanted.insert((*r)->down);
        Json e{{"k", "round"},
               {"current", r == &hub},
               {"up", (*r)->up},
               {"down", (*r)->down},
               {"cond", (*r)->condition},
               {"start", (*r)->start},
               {"end", (*r)->end},
               {"rules", rules_json(**r)}};
        rec(std::move(e));
      }
    if (want != subscribed) {
      std::vector<std::string> add, drop;
      for (auto &t : want)
        if (!subscribed.count(t))
          add.push_back(t);
      for (auto &t : subscribed)
        if (!want.count(t))
          drop.push_back(t);
      subscribed = want;
      book.set_active(std::vector<std::string>(want.begin(), want.end()));
      if (market->connected()) {
        if (!add.empty())
          market->send(Json{{"assets_ids", add},
                            {"operation", "subscribe"},
                            {"custom_feature_enabled", true}}
                           .dump());
        if (!drop.empty())
          market->send(
              Json{{"assets_ids", drop}, {"operation", "unsubscribe"}}.dump());
      }
    }
    sync_user_markets();
  }
  void sync_user_markets() {
    if (!user)
      return;
    std::set<std::string> active;
    if (hub) active.insert(hub->condition);
    if (prepared) active.insert(prepared->condition);
    auto markets = execution::account_markets(ledger, std::move(active));
    if (markets == user_markets)
      return;
    std::vector<std::string> add, drop;
    std::set_difference(markets.begin(), markets.end(), user_markets.begin(),
                        user_markets.end(), std::back_inserter(add));
    std::set_difference(user_markets.begin(), user_markets.end(), markets.begin(),
                        markets.end(), std::back_inserter(drop));
    user_markets = std::move(markets);
    last_sub = mono_s();
    user_pong = -1;
    if (user->connected()) {
      if (!drop.empty())
        user->send(Json{{"markets", drop}, {"operation", "unsubscribe"}}.dump());
      if (!add.empty())
        user->send(Json{{"markets", add}, {"operation", "subscribe"}}.dump());
      user->send("PING");
    }
  }
  // feeds/hub._request_ws_snapshot
  void request_ws_snapshot(const std::string &token, const BookView &view) {
    double now = mono_s();
    auto prior = next_ws_snapshot.find(token);
    if (view.status != UNSYNCED ||
        (prior != next_ws_snapshot.end() &&
         prior->second.first == view.generation && now < prior->second.second))
      return;
    if (!market->connected() || !subscribed.count(token))
      return;
    market->send(
        Json{{"assets_ids", Json::array({token})}, {"operation", "unsubscribe"}}
            .dump());
    auto g = book.subscription_gap(token, view.generation);
    rec(Json{{"k", "ws_gap"}, {"token", token}, {"generation", view.generation}});
    if (g)
      market->send(Json{{"assets_ids", Json::array({token})},
                        {"operation", "subscribe"},
                        {"custom_feature_enabled", true}}
                       .dump());
    next_ws_snapshot[token] = {view.generation, now + 1.5};
  }
  // feeds/hub._resync_loop: REST /book recovery and refresh of quiet books
  void book_recovery() {
    double now = mono_s();
    std::set<std::string> need;
    for (auto &t : book.needs_resync(true))
      need.insert(t);
    for (auto &t : resync_wanted)
      need.insert(t);
    auto active = book.active();
    std::set<std::string> act(active.begin(), active.end());
    for (auto &t : active) {
      auto v = book.view(t);
      if (v.updated_mono && (now - *v.updated_mono) * 1000.0 >= 1500.0)
        need.insert(t);
    }
    for (auto it = next_fetch.begin(); it != next_fetch.end();)
      it = act.count(it->first) ? std::next(it) : next_fetch.erase(it);
    for (auto it = next_ws_snapshot.begin(); it != next_ws_snapshot.end();)
      it = act.count(it->first) ? std::next(it) : next_ws_snapshot.erase(it);
    for (auto it = resync_wanted.begin(); it != resync_wanted.end();)
      it = act.count(*it) ? std::next(it) : resync_wanted.erase(it);
    for (auto &t : need) {
      if (!act.count(t))
        continue;
      {
        std::lock_guard lock(fetch_mutex);
        if (fetching.count(t))
          continue;
      }
      auto v = book.view(t);
      auto prior = next_fetch.find(t);
      if (prior != next_fetch.end() && prior->second.first == v.generation &&
          now < prior->second.second)
        continue;
      if (v.status == LIVE && v.updated_mono &&
          (now - *v.updated_mono) * 1000.0 < 1500.0) {
        resync_wanted.erase(t);
        continue;
      }
      next_fetch[t] = {v.generation, now + 1.5};
      {
        std::lock_guard lock(fetch_mutex);
        fetching.insert(t);
        fetch_queue.push_back({t, v.generation, v.updates, v.receive_revision});
      }
      rec(Json{{"k", "rest_req"}, {"token", t}});
      fetch_wake.notify_one();
    }
  }
  void apply_rest(const BookFetch &f, const Json *j, double received,
                  const std::string &why) {
    {
      std::lock_guard lock(fetch_mutex);
      fetching.erase(f.token);
    }
    if (!book.is_active(f.token))
      return;
    if (!j || !j->is_object() ||
        (j->contains("asset_id") && (*j)["asset_id"].is_string() &&
         (*j)["asset_id"].get<std::string>() != f.token)) {
      (void)why;
      request_ws_snapshot(f.token, book.view(f.token));
      return;
    }
    SnapshotOptions o;
    o.only_if_unsynced = true;
    o.expected_generation = f.generation;
    o.expected_updates = f.updates;
    o.expected_receive_revision = f.revision;
    o.refresh_if_older_than = 1.5;
    o.source = "rest";
    o.ts_ms = j->value("timestamp", Json());
    o.received_mono = std::min(received, mono_s());
    if (j->contains("tick_size"))
      o.tick = py_float((*j)["tick_size"]);
    if (j->contains("hash") && (*j)["hash"].is_string())
      o.hash = (*j)["hash"].get<std::string>();
    o.require_exchange_ts = true;
    Json bids = j->value("bids", Json()), asks = j->value("asks", Json());
    rec(Json{{"k", "rest_resp"},
             {"token", f.token},
             {"bids", bids},
             {"asks", asks},
             {"timestamp", o.ts_ms},
             {"tick", o.tick ? Json(*o.tick) : Json()},
             {"hash", o.hash ? Json(*o.hash) : Json()}});
    bool ok = false;
    try {
      ok = book.apply_snapshot(f.token, bids, asks, o);
    } catch (...) {
      ok = false;
    }
    auto v = book.view(f.token);
    if (ok || v.status == LIVE)
      resync_wanted.erase(f.token);
    else if (v.status == UNSYNCED && v.generation == f.generation)
      request_ws_snapshot(f.token, v);
  }
  // ------------------------------------------------------------ paper
  void paper_poll() {
    if (!paper)
      return;
    paper->poll(book, c.s,
                [this](const std::shared_ptr<Plan> &plan, const PaperResult &res) {
                  if (res.filled)
                    ++paper_fills;
                  sniper->finish_paper(plan, res);
                });
    arm_paper_timer();
  }
  void arm_paper_timer() {
    if (!paper || !paper->pending() || paper_timer_armed)
      return;
    auto wake = paper->next_wake();
    double delay = wake ? *wake - mono_s() : 0.005;
    delay = std::clamp(delay, 0.001, 0.25);
    paper_timer_armed = true;
    paper_timer.expires_after(std::chrono::microseconds(
        static_cast<long long>(delay * 1e6)));
    paper_timer.async_wait([this](auto ec) {
      paper_timer_armed = false;
      if (!ec && !stopping)
        paper_poll();
    });
  }
  // ------------------------------------------------------------ the feed
  void on_coinbase(int n, std::string_view raw, int backlog) {
    auto t0 = mono_ns();
    double tm = static_cast<double>(t0) / 1e9;
    rec_frame("cb", raw, backlog);
    try {
      sniper->cb_fresh.note_message(tm, backlog);
      auto kind = sniper->on_coinbase(raw, tm * 1000.0, t0);
      if (kind == "tick" || kind == "late")
        cb_last_tick[n] = tm;
      else if (kind == "subscribed")
        cb_ack[n] = true;
    } catch (const std::exception &e) {
      // CoinbaseSubscribeError: an error message or a wrong ack
      audit({{"result", "coinbase_reconnect"},
             {"socket", n + 1},
             {"reason", std::string(e.what()).substr(0, 160)}});
      coinbase[n]->reconnect();
      return;
    }
    for (auto &plan : sniper->take_dispatched()) {
      ++orders_sent;
      if (paper)
        paper->begin(plan, c.s, book);
    }
    if (paper && paper->pending())
      paper_poll();
  }
  // Sniper._fire_live + LiveExecution.prepare: check, sign, durable intent,
  // send (no retry). A refusal sends nothing; the engine journals it as
  // "live refused" ("live halted" while new orders are paused).
  bool dispatch_live(const std::shared_ptr<Plan> &p) {
    auto refuse = [&](const std::string &why, bool halted = false) {
      p->live_refusal = why;
      p->live_halted = halted;
      return false;
    };
    if (!hub || hub->condition != p->cond)
      return refuse("market changed before dispatch");
    Round &r = *hub;
    int n = p->side == "UP" ? 0 : 1;
    auto &rule = r.rules[n];
    Json row = p->row;
    bool halted = stopping || journal.failed() ||
                  std::filesystem::exists(c.stop_file) || wall_s() < pause_until;
    auto ready = live_readiness(r);
    if (!ready.empty())
      return refuse(ready, halted);
    if (!p->shares)
      return refuse("live orders are share-sized (SNIPER_ORDER_SIZING=shares)");
    // The engine's own round counters only follow paper fills, so the round caps
    // are enforced here for live, from the ledger.
    if (ledger.round_fills(r.condition) >= c.max_fills)
      return refuse("round cap: fills in this round");
    if (dollars(ledger.risk_shares(p->token)) + static_cast<double>(*p->shares) >
        c.max_side + 1e-9)
      return refuse("round cap: shares on this side");
    // the cash read just before this round started, or since
    if (wall_s() - (mono_s() - balance_at) <
        std::min(static_cast<double>(r.start), wall_s()) - 10.0)
      return refuse("cash not read for this round yet");
    auto mit = market_rules.find(r.condition);
    if (mit == market_rules.end())
      return refuse("market rules not loaded for this round yet");
    const MarketRules &mr = mit->second;
    if (p->fair_fee_rate &&
        (py::D(*p->fair_fee_rate) != mr.fee_rate ||
         p->fair_fee_exponent.value_or(1) != mr.fee_exponent ||
         py::D(dollars(rule.rate)) != mr.fee_rate ||
         rule.exponent != mr.fee_exponent))
      return refuse("fee rules changed after the fair-value decision");
    // the decision book, checked and sized exactly as paper checks it
    LiveOrderCheck chk;
    try {
      chk = live_presend_check(*p, mr, py::D(dollars(rule.tick)),
                               py::D(dollars(rule.min_shares)), c.max_price,
                               c.min_price, live_max_spread, mono_s(),
                               venue_wall());
    } catch (const PaperRejected &e) {
      return refuse(std::string("pre-send check: ") + e.what());
    } catch (const std::exception &e) {
      return refuse(std::string("pre-send check failed: ") + e.what());
    }
    double spend = chk.spend.to_double(), fee = chk.quote.fee.to_double();
    double fee_rate = mr.fee_rate.to_double();
    // more liquidity can arrive after the snapshot: reserve the whole signed
    // spend and an upper fee bound, not just the visible partial
    double reserve_fee = std::max(fee, spend * fee_rate);
    if (p->hybrid) {
      try {
        sniper->check_hybrid_order(*p, py::D(spend), fee_rate,
                                   py::D(reserve_fee));
      } catch (const py::ValueError &e) {
        return refuse(std::string("hybrid risk check: ") + e.what());
      }
    }
    double free = dollars(effective_balance().value_or(0) - ledger.reserved());
    if (spend + reserve_fee > free + 1e-9)
      return refuse("collateral $" + py::fixed(free, 2) + " is below the order $" +
                    py::fixed(spend + reserve_fee, 2) + " incl. fee");
    Units cap = units(chk.price.fixed(6)), stake = units(chk.spend.fixed(6));
    Units reserve = units(py::quantize(hybrid::risk_total(chk.spend, mr.fee_rate,
                                                          chk.quote.fee),
                                       py::D("0.000001"), py::Rounding::Ceiling)
                              .fixed(6));
    auto allow = allowances.find(
        std::string(rule.neg ? NEG_EXCHANGE : STANDARD_EXCHANGE));
    if (allow == allowances.end())
      for (auto i = allowances.begin(); i != allowances.end(); ++i)
        if (checksum_address(i->first) ==
            checksum_address(rule.neg ? NEG_EXCHANGE : STANDARD_EXCHANGE)) {
          allow = i;
          break;
        }
    if (allow == allowances.end() || allow->second < reserve)
      return refuse("collateral allowance insufficient");
    Intent intent;
    intent.condition = r.condition;
    intent.token = p->token;
    intent.side = p->side;
    intent.slug = r.slug;
    intent.up = r.up;
    intent.down = r.down;
    intent.start = r.start;
    intent.end = r.end;
    intent.stake = stake;
    intent.reserve = reserve;
    intent.target = *p->shares * U;
    intent.limit = cap;
    intent.fee_rate = units(fixed6(fee_rate));
    intent.fee_exponent = p->fair_fee_exponent.value_or(rule.exponent);
    intent.sent_wall = wall_s();
    intent.min_entry = p->minimum_entry;
    std::array<std::int64_t, 8> ts{};
    ts[0] = p->t_ns;
    ts[1] = p->t_ns;
    ts[2] = mono_ns();
    auto signed_order = build_order(*signer, c.funder, c.signature_type,
                                    p->token, stake, cap, rule, creds,
                                    c.order_type);
    ts[3] = mono_ns();
    if (c.dry_run) {
      // Every check above ran and the order was signed. Nothing is recorded as
      // an intent, nothing counts against a budget, and nothing is sent.
      ++dry_orders;
      audit({{"result", "dry_run_order"},
             {"side", p->side},
             {"shares", *p->shares},
             {"limit", dollars(cap)},
             {"stake", dollars(stake)},
             {"order_id", signed_order.id}});
      return refuse("dry run: passed every check and was not sent", true);
    }
    intent.id = signed_order.id;
    intent.status = "sent";
    intent.fill_slot = true;
    auto state = ledger.canary;
    auto why = canary_refusal(c, state, *p->shares, stake,
                              state.settled_loss + ledger.open_cost() + reserve,
                              r.condition, wall_s());
    if (!why.empty()) {
      ledger.set_canary(state);
      // a budget or an earlier pause holds new orders: "live halted"
      return refuse(why, why != "canary order shares");
    }
    auto unsent_state = state;
    ++state.orders;
    ++state.fills;
    state.spend += stake;
    state.rounds.insert(r.condition);
    ledger.register_intent(intent, &signed_order, state); // fsynced intent
    ts[4] = mono_ns();
    // the send guard (LiveExecution send_guard, Sniper._check_dispatch_age):
    // the durable write can take time; nothing may have gone stale meanwhile
    auto now = mono_s();
    auto gate = live_readiness(r);
    if (gate.empty() && (!effective_balance() || ledger.reserved() > *effective_balance()))
      gate = "collateral fell below outstanding reservations";
    double deadline = p->dispatch_deadline > 0
                          ? p->dispatch_deadline
                          : p->end - sniper->window().second;
    if (gate.empty() && wall_s() >= deadline)
      gate = "outside the original decision window";
    if (gate.empty() && p->view.updated_mono) {
      double age = (now - *p->view.updated_mono) * 1000.0;
      if (!std::isfinite(age) || age < 0 || age > c.s.stale_ms)
        gate = "decision book became stale";
    }
    if (gate.empty() && sniper->cb_fresh.enabled) {
      auto why_not = sniper->cb_fresh.why_not(now);
      double elapsed = now * 1000.0 - p->t_ms;
      double excess = sniper->cb_fresh.last_excess.value_or(0.0);
      if (!why_not.empty())
        gate = why_not;
      else if (!std::isfinite(elapsed) || elapsed < 0 ||
               elapsed > c.s.coinbase_unknown_ms ||
               excess + elapsed > c.s.coinbase_stale_ms)
        gate = "trigger signal became stale";
    }
    if (gate.empty() && static_cast<double>(r.end) - wall_s() <=
                            std::max(c.min_expiry, c.s.min_left_s))
      gate = "inside the matching-delay/expiry window";
    auto view = book.view(p->token);
    if (gate.empty() &&
        (view.status != LIVE || !view.updated_mono ||
         (now - *view.updated_mono) * 1000 < 0 ||
         (now - *view.updated_mono) * 1000 > c.s.stale_ms))
      gate = "book became stale";
    if (!gate.empty()) {
      ledger.status(intent.id, "aborted");
      ledger.set_canary(unsent_state);
      return refuse(gate + " before dispatch");
    }
    stamps[intent.id] = ts;
    sent_at[intent.id] = now;
    auto headers = l2_headers(*signer, creds, "POST", "/order",
                              signed_order.body, clock.now(now));
    if (!sender->post(intent.id, signed_order.body, headers)) {
      ledger.status(intent.id, "aborted");
      ledger.set_canary(unsent_state);
      release(intent.id);
      return refuse("native HTTP/2 sender not ready before dispatch");
    }
    ++orders_sent;
    inflight_id[n] = intent.id;
    row["order_id"] = intent.id;
    row["client_order_id"] = intent.id;
    row["result"] = "pending";
    row["T0_ns"] = ts[0];
    row["T2_ns"] = ts[2];
    audit(row);
    return true;
  }
  void pause(std::string why, bool once) {
    if (once && !pause_latches.insert(why).second)
      return;
    pause_reason = std::move(why);
    pause_until = wall_s() + 300;
    if (c.live) {
      auto state = ledger.canary;
      state.pause_reason = "fault:" + pause_reason;
      state.pause_until = std::max(state.pause_until, pause_until);
      ledger.set_canary(state);
    }
    audit({{"result", "guard"},
           {"reason", pause_reason},
           {"pause_until", pause_until}});
  }
  Round *current() {
    auto start = static_cast<std::int64_t>(wall_s()) / 300 * 300;
    auto i = rounds.find(start);
    return i == rounds.end() ? nullptr : &i->second;
  }
  bool user_ready(const Round &r) const {
    return user && user->connected() && user_markets.count(r.condition) &&
           user_pong >= last_sub && mono_s() - user_pong < 20;
  }
  std::string live_readiness(const Round &r) {
    if (stopping)
      return "kill switch stopped";
    if (journal.failed())
      return "audit or recorder failed";
    if (std::filesystem::exists(c.stop_file))
      return "STOP_LIVE present";
    if (wall_s() < pause_until)
      return pause_reason;
    for (auto o = inherited_orders.begin(); o != inherited_orders.end();) {
      auto &i = ledger.orders.at(*o);
      if (i.unresolved() || i.status == "matched")
        return "unresolved native order from previous run: " + *o + " (" +
               i.status + ")";
      o = inherited_orders.erase(o);
    }
    if (!sender->ready())
      return "native order connection not ready";
    try {
      clock.now(mono_s());
    } catch (const std::exception &e) {
      return e.what();
    }
    if (!balance || mono_s() - balance_at > 120)
      return "fresh collateral balance unavailable";
    if (!user_ready(r))
      return "private channel not ready for this market";
    if (!r.rules[0].loaded || !r.rules[1].loaded)
      return "market rules not loaded";
    return {};
  }
  void release(std::string_view id) {
    for (int n = 0; n < 2; ++n)
      if (!id.empty() && inflight_id[n] == id) {
        inflight_id[n].clear();
        if (sniper)
          sniper->release(n == 0 ? "UP" : "DOWN");
      }
    sent_at.erase(std::string(id));
    stamps.erase(std::string(id));
  }
  Json timing_fields(std::string_view id) const {
    Json fields = Json::object();
    auto it = stamps.find(std::string(id));
    if (it != stamps.end())
      for (int index : {0, 2, 3, 4, 5, 6})
        if (it->second[index] > 0)
          fields["T" + std::to_string(index) + "_ns"] = it->second[index];
    return fields;
  }
  void on_reply(std::string_view id, const HttpResult &reply) {
    if (id == "__clock") {
      try {
        if (reply.status == 200) {
          double now = mono_s();
          clock.sample(number(reply.json()), probe_wall, wall_s(),
                       (now - probe_sent) * 1000, now);
        }
      } catch (...) {
      }
      return;
    }
    auto it = ledger.orders.find(std::string(id));
    if (it == ledger.orders.end())
      return;
    auto &intent = it->second;
    if (auto s = stamps.find(std::string(id)); s != stamps.end())
      s->second[6] = mono_ns();
    auto timings = timing_fields(id);
    auto record_reply = [&](Json row) {
      row["acknowledged"] = true;
      row.update(timings);
      audit(std::move(row));
    };
    double rtt = sent_at.count(std::string(id))
                     ? (mono_s() - sent_at.at(std::string(id))) * 1000
                     : 0;
    if (rtt > 1500) {
      slow_replies.push_back(mono_s());
      while (!slow_replies.empty() &&
             slow_replies.front() < mono_s() - c.guard_window)
        slow_replies.pop_front();
      if (rtt > 5000 ||
          slow_replies.size() >= static_cast<std::size_t>(c.guard_strikes)) {
        pause("slow native order replies", false);
        slow_replies.clear();
      }
    }
    try {
      auto j = reply.json();
      if (j.contains("orderID") &&
          !j.at("orderID").get<std::string>().empty() && j.at("orderID") != id)
        throw std::runtime_error(
            "reply order hash disagrees with signed intent");
      bool success = j.value("success", false);
      auto making =
          j.contains("makingAmount") && !j.at("makingAmount").is_null()
              ? venue_units(j.at("makingAmount"))
              : 0;
      auto taking =
          j.contains("takingAmount") && !j.at("takingAmount").is_null()
              ? venue_units(j.at("takingAmount"))
              : 0;
      std::string error = j.value("errorMsg", "");
      // an HTTP 400 carries its text in "error" instead
      if (error.empty() && j.contains("error") && j.at("error").is_string())
        error = j.at("error").get<std::string>();
      if (taking > 0 && making > 0) {
        if (making > intent.stake + 1 ||
            making > multiply(taking, intent.limit) + 1)
          throw std::runtime_error("matched totals exceed signed limits");
        ledger.status(id, "matched", taking, making);
        release(id);
        record_reply({{"result", "live_fill_update"},
               {"order_id", id},
               {"shares", dollars(taking)},
               {"amount", dollars(making)},
               {"confirmed", false},
               {"over_target", taking > intent.target},
               {"reply_ms", rtt}});
      } else if (!success && (reply.status == 400 || reply.status == 200)) {
        std::string lower = error;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        bool miss = lower.find("no orders") != std::string::npos ||
                    lower.find("not fully filled") != std::string::npos;
        ledger.status(id, miss ? "no_match" : "refused");
        // The venue confirmed nothing traded, so no money was spent. A refusal
        // for any other reason keeps its spend.
        if (miss)
          ledger.release_missed_spend(id);
        ledger.release_fill_slot(id);
        release(id);
        record_reply({{"result", "refused"},
               {"order_id", id},
               {"reason", miss ? "FAK no match" : "venue refused native order"},
               {"http_status", reply.status},
               {"venue_reply", j.dump(-1, ' ', true).substr(0, 300)}});
      } else if (success) {
        ledger.status(id, "pending");
        release(id);
        record_reply({{"result", "pending"},
               {"order_id", id},
               {"reason", "awaiting FAK fill size"}});
      } else
        on_unknown(id, "unexpected native order reply");
    } catch (const std::exception &e) {
      on_unknown(id, e.what());
    }
  }
  void on_unknown(std::string_view id, std::string_view why) {
    if (id == "__clock")
      return;
    auto i = ledger.orders.find(std::string(id));
    if (i == ledger.orders.end() || i->second.status == "confirmed")
      return;
    ledger.status(id, "unknown");
    auto timings = timing_fields(id);
    release(id);
    pause("unknown native order outcome", true);
    Json row{{"result", "pending"},
           {"order_id", id},
           {"reason", std::string(why)},
           {"never_retry", true}, {"acknowledged", false}};
    row.update(timings);
    audit(std::move(row));
  }
  void consume_trade(const Json &j) {
    try {
      auto t = parse_trade(j, ledger.orders);
      if (t.status == "FAILED") {
        private_sizes[t.order].erase(t.id);
        pause("private trade failed; reconciliation required", true);
        return;
      }
      if (t.status != "CONFIRMED" && !execution::pending_trade_status(t.status))
        throw std::runtime_error("unknown private trade status");
      private_sizes[t.order][t.id] = t.shares;
      // Reconciliation re-reads every trade of the session every 5 s; one that
      // is already booked has nothing new to record.
      if (ledger.trade_ids.count(t.id))
        return;
      Units delivered = 0;
      for (auto &[id, sh] : private_sizes[t.order]) {
        if (sh < 0 || delivered > std::numeric_limits<Units>::max() - sh)
          throw std::overflow_error("private trade size total overflow");
        delivered += sh;
      }
      auto &intent = ledger.orders.at(t.order);
      if (t.status == "CONFIRMED") {
        auto outflow = static_cast<__int128>(booked_outflow) + t.cost + t.fee;
        if (outflow > std::numeric_limits<Units>::max())
          throw std::overflow_error("confirmed collateral outflow overflow");
        if (ledger.fill(t.id, t.order, t.shares, t.cost, t.fee)) {
          booked_outflow = static_cast<Units>(outflow);
          audit({{"result", "live_fill"},
                 {"order_id", t.order},
                 {"trade_id", t.id},
                 {"side", intent.side},
                 {"shares", dollars(t.shares)},
                 {"amount", dollars(t.cost)},
                 {"fee", dollars(t.fee)},
                 {"status", "CONFIRMED"}});
        }
      } else if (delivered > intent.matched_shares)
        ledger.status(intent.id, intent.status, delivered, intent.matched_cost);
      audit({{"result", "live_fill_update"},
             {"order_id", t.order},
             {"trade_id", t.id},
             {"status", t.status}});
    } catch (const std::exception &e) {
      ++errors;
      pause("accounting disagreement or unauthorized fill", true);
      audit({{"result", "error"}, {"reason", e.what()}});
    }
  }
  void on_user(std::string_view raw) {
    if (raw == "PONG") {
      user_pong = mono_s();
      return;
    }
    try {
      auto j = Json::parse(raw);
      if (!j.is_array())
        j = Json::array({j});
      for (auto &v : j) {
        auto type = v.value("event_type", "");
        if (type == "trade")
          consume_trade(v);
        else if (type == "order") {
          auto id = v.value("id", "");
          auto i = ledger.orders.find(id);
          if (i == ledger.orders.end()) {
            pause("wallet order not authorized by native bot", true);
            continue;
          }
          if (v.value("type", "") == "CANCELLATION") {
            auto size = venue_units(v.at("size_matched"));
            ledger.status(id, size ? "matched" : "no_match", size);
            if (!size)
              ledger.release_fill_slot(id);
            release(id);
          }
        }
      }
    } catch (...) {
      ++errors;
      pause("invalid private-channel event", true);
    }
  }
  void set_rounds(std::vector<Round> fresh) {
    for (auto &r : fresh) {
      auto old = rounds.find(r.start);
      if (old != rounds.end()) {
        for (int n = 0; n < 2; ++n)
          r.rules[n].generation =
              old->second.rules[n].generation +
              ((r.rules[n].tick != old->second.rules[n].tick ||
                r.rules[n].rate != old->second.rules[n].rate ||
                r.rules[n].exponent != old->second.rules[n].exponent ||
                r.rules[n].min_shares != old->second.rules[n].min_shares ||
                r.rules[n].neg != old->second.rules[n].neg)
                   ? 1
                   : 0);
        if (!r.ptb)
          r.ptb = old->second.ptb;
      }
      rounds[r.start] = r;
      // The engine and paper/live pre-send checks share this independent
      // cache; updating the active Round alone leaves its rules stale.
      if (market_rules.count(r.condition) && activate_rules(r))
        rec(Json{{"k", "rules"}, {"cond", r.condition}, {"rules", rules_json(r)}});
      if (hub && hub->start == r.start && !execution::refresh_round_rules(*hub, r))
        hub.reset();
      if (prepared && prepared->start == r.start && !execution::refresh_round_rules(*prepared, r))
        prepared.reset();
    }
    auto start = static_cast<std::int64_t>(wall_s()) / 300 * 300;
    // A published price to beat for a closed round calibrates the estimate.
    // Discovery only returns the current and next rounds, so closed rounds come
    // from the ones kept here; ptb_official uses each round once.
    if (sniper)
      for (auto &[round_start, r] : rounds)
        if (r.ptb && round_start < start)
          sniper->ptb_official(static_cast<double>(round_start), *r.ptb);
    update_hub();
    for (auto i = rounds.begin(); i != rounds.end();)
      if (i->first < start - 300 && !(hub && hub->start == i->first) &&
          (ledger.settled_conditions.count(i->second.condition) ||
           (!ledger.holding(i->second.up) && !ledger.holding(i->second.down) &&
            !ledger.awaiting(i->second.up) &&
            !ledger.awaiting(i->second.down))))
        i = rounds.erase(i);
      else
        ++i;
  }
  void refresh_work() {
    sync_user_markets();
    auto out = std::make_shared<Work>();
    out->booked_outflow = booked_outflow;
    for (auto &[time, r] : rounds)
      if (!ledger.settled_conditions.count(r.condition) && r.end < wall_s()) {
        bool exposure = ledger.holding(r.up) || ledger.holding(r.down) ||
                        ledger.awaiting(r.up) || ledger.awaiting(r.down);
        if (exposure)
          out->unsettled.push_back(r);
      }
    for (auto &m : user_markets)
      out->conditions.push_back(m);
    for (auto &[id, i] : ledger.orders)
      if (i.unresolved() || i.status == "matched")
        out->unresolved.push_back(id);
    out->since =
        ledger.canary.started > 0 ? ledger.canary.started - 5 : wall_s() - 5;
    // An order from an earlier session still needs its own trades read back.
    for (auto &[id, i] : ledger.orders)
      if ((i.unresolved() || i.status == "matched") && i.sent_wall > 0)
        out->since = std::min(out->since, i.sent_wall - 5);
    try {
      out->auth_mono = mono_s();
      out->auth_time = clock.now(out->auth_mono);
    } catch (...) {
      out->auth_time = static_cast<std::int64_t>(wall_s());
    }
    work.store(out);
    wake.notify_all();
  }
  Headers auth(std::string_view path) {
    auto current = work.load();
    double elapsed = mono_s() - current->auth_mono;
    if (elapsed < 0 || elapsed > 60)
      throw std::runtime_error("background authentication clock stale");
    return l2_headers(*signer, creds, "GET", path, {},
                      current->auth_time + static_cast<std::int64_t>(elapsed));
  }
  // Results from background threads run on the event loop. An exception in
  // one of them used to escape io.run() and end the process abruptly; now it
  // is recorded, and live mode pauses new orders (fail closed).
  void handler_fault(const char *what, const char *why) {
    ++errors;
    audit({{"result", "error"},
           {"reason", std::string("native ") + what + ": " + why}});
    if (c.live)
      pause(std::string("fault in native ") + what, true);
  }
  template <class F> void post(const char *what, F &&f) {
    net::post(io, [this, what, f = std::forward<F>(f)]() mutable {
      try {
        f();
      } catch (const std::exception &e) {
        handler_fault(what, e.what());
      } catch (...) {
        handler_fault(what, "unknown exception");
      }
    });
  }
  void workers() {
    worker = std::thread([this] {
      background_priority();
      RestClient rest(c.live, &blocked);
      double discovered = -1e30, balanced = -1e30, reconciled = -1e30;
      while (!workers_stop) {
        try {
          auto now = mono_s();
          if (now - discovered >= 5) {
            std::vector<Round> discovered_rounds;
            auto start = static_cast<std::int64_t>(wall_s()) / 300 * 300;
            for (auto t : {start, start + 300}) {
              if (workers_stop)
                break;
              try {
                auto event = rest.get("https://gamma-api.polymarket.com/events/"
                                      "slug/btc-updown-5m-" +
                                      std::to_string(t));
                auto r = parse_round(event, t);
                if (!r)
                  throw std::runtime_error(
                      "event does not match BTC 5m TWAP contract");
                auto info = rest.get(
                    "https://clob.polymarket.com/clob-markets/" + r->condition);
                if (info.value("c", "") != r->condition)
                  throw std::runtime_error("discovered CLOB condition disagrees with Gamma");
                for (int n = 0; n < 2; ++n) {
                  auto id = n == 0 ? r->up : r->down;
                  auto b = rest.get(
                      "https://clob.polymarket.com/book?token_id=" + id);
                  r->rules[n] = parse_rules(info, b, id);
                }
                discovered_rounds.push_back(*r);
              } catch (const std::exception &e) {
                post("discovery", [this, t, why = std::string(e.what())] {
                  if (!stopping)
                    audit({{"result", "discovery_refused"},
                           {"round_start", t},
                           {"reason", why}});
                });
              }
            }
            discovered = mono_s();
            if (!workers_stop)
              post("discovery", [this, r = std::move(discovered_rounds)]() mutable {
                if (!stopping)
                  set_rounds(std::move(r));
              });
          }
          auto w = work.load();
          if (c.live && !workers_stop && now - balanced >= 5) {
            auto path = "/balance-allowance";
            auto j = rest.get(
                "https://clob.polymarket.com/"
                "balance-allowance?asset_type=COLLATERAL&signature_type=" +
                    std::to_string(c.signature_type),
                auth(path));
            Big raw(j.at("balance").is_string()
                        ? j.at("balance").get<std::string>()
                        : j.at("balance").dump());
            if (raw < 0 || raw > std::numeric_limits<Units>::max())
              throw std::runtime_error("invalid collateral balance");
            std::map<std::string, Big> allow;
            for (auto it = j.at("allowances").begin();
                 it != j.at("allowances").end(); ++it)
              allow[it.key()] =
                  Big(it.value().is_string() ? it.value().get<std::string>()
                                             : it.value().dump());
            auto money = raw.convert_to<Units>();
            post("balance", [this, money, sampled_outflow = w->booked_outflow, allow = std::move(allow)] {
              balance = money;
              balance_outflow = sampled_outflow;
              balance_at = mono_s();
              allowances = allow;
              if (ledger.reference_cash == 0 && ledger.realized == 0 &&
                  money > 0)
                ledger.risk_rebase(money);
            });
            balanced = mono_s();
          }
          if (c.live && !workers_stop && now - reconciled >= 5) {
            std::string cursor;
            for (int pages = 0; pages < 100 && !workers_stop; ++pages) {
              auto target = "https://clob.polymarket.com/data/trades?after=" +
                            std::to_string(static_cast<std::int64_t>(w->since));
              if (!cursor.empty())
                target += "&next_cursor=" + cursor;
              auto j = rest.get(target, auth("/data/trades"));
              auto trades = j.at("data");
              post("trade reconciliation", [this, trades = std::move(trades)] {
                for (auto &t : trades)
                  consume_trade(t);
              });
              cursor = j.value("next_cursor", "");
              if (cursor.empty() || cursor == "LTE=")
                break;
              if (pages == 99)
                throw std::runtime_error("reconcile page capacity reached");
            }
            for (auto &id : w->unresolved) {
              if (workers_stop)
                break;
              try {
                auto path = "/data/order/" + id;
                auto j = rest.get("https://clob.polymarket.com" + path,
                                  auth(path));
                post("order reconciliation", [this, id, j = std::move(j)] {
                  auto i = ledger.orders.find(id);
                  if (i == ledger.orders.end() ||
                      i->second.status == "confirmed")
                    return;
                  if (!j.is_object())
                    throw std::runtime_error("order status is not an object");
                  auto size = j.contains("size_matched")
                                  ? venue_units(j.at("size_matched"))
                                  : 0;
                  auto status = j.value("status", "");
                  if (status == "MATCHED" || status == "CANCELED" ||
                      status == "CANCELLED") {
                    ledger.status(id, size ? "matched" : "no_match", size);
                    if (!size)
                      ledger.release_fill_slot(id);
                    release(id);
                  }
                });
              } catch (...) {
              }
            }
            for (auto &condition : w->conditions) {
              if (workers_stop)
                break;
              auto j = rest.get(
                  "https://clob.polymarket.com/data/orders?market=" + condition,
                  auth("/data/orders"));
              auto open = j.at("data");
              post("open-order reconciliation", [this, open = std::move(open)] {
                for (auto &o : open) {
                  auto id = o.is_object() ? o.value("id", "") : std::string();
                  if (!ledger.orders.count(id) ||
                      o.value("order_type", "") == "GTC" ||
                      o.value("order_type", "") == "GTD")
                    pause("our resting order is in this market", true);
                }
              });
            }
            reconciled = mono_s();
          }
        } catch (const std::exception &e) {
          post("background REST", [this, reason = std::string(e.what())] {
            if (!stopping)
              audit({{"result", "error"},
                     {"reason", "background native REST: " + reason}});
          });
        }
        std::unique_lock lock(wake_mutex);
        wake.wait_for(lock, std::chrono::milliseconds(500),
                      [this] { return workers_stop.load(); });
      }
      worker_done = true;
    });
    // Book refreshes have their own thread and a short timeout. They shared
    // the discovery thread, so one slow Gamma or CLOB call (10 s timeout, up
    // to eight calls per cycle) froze every REST refresh and quiet books went
    // "book stale" until discovery finished.
    // REST /book recovery: the hub (book_recovery) queues requests, two
    // threads fetch with the reference's 10 s timeout; results apply on the
    // event loop with the fencing values captured at request time.
    auto fetch_loop = [this] {
      background_priority();
      RestClient rest(c.live, &blocked);
      while (!workers_stop) {
        BookFetch f;
        {
          std::unique_lock lock(fetch_mutex);
          fetch_wake.wait_for(lock, std::chrono::milliseconds(250), [this] {
            return workers_stop.load() || !fetch_queue.empty();
          });
          if (workers_stop)
            break;
          if (fetch_queue.empty())
            continue;
          f = fetch_queue.front();
          fetch_queue.pop_front();
        }
        try {
          auto j = rest.get(
              "https://clob.polymarket.com/book?token_id=" + f.token, {}, 10);
          double got = mono_s();
          post("book refresh", [this, f, got, j = std::move(j)] {
            if (!stopping)
              apply_rest(f, &j, got, "");
          });
        } catch (const std::exception &e) {
          post("book refresh", [this, f, why = std::string(e.what())] {
            if (!stopping)
              apply_rest(f, nullptr, 0, why);
          });
        }
      }
      if (--refreshers_live == 0)
        refresher_done = true;
    };
    refresher = std::thread(fetch_loop);
    refresher2 = std::thread(fetch_loop);
    settler = std::thread([this] {
      background_priority();
      RestClient rest(c.live, &blocked);
      while (!workers_stop) {
        auto w = work.load();
        for (auto r : w->unsettled) {
          if (workers_stop)
            break;
          try {
            auto payouts = resolve_payouts(rest, r, c.rpc, c.settle_onchain);
            if (!payouts.empty())
              post("settlement", [this, r, payouts] {
                if (ledger.settle(r.condition, payouts)) {
                  dash_note_settlement(r, payouts);
                  audit({{"result", "settlement"},
                         {"condition_id", r.condition},
                         {"source", "verified venue resolution"},
                         {"payouts", payouts},
                         {"redemption",
                          c.live ? "manual" : "paper cash credited"}});
                }
              });
          } catch (...) {
          }
        }
        std::unique_lock lock(wake_mutex);
        wake.wait_for(lock, std::chrono::seconds(5),
                      [this] { return workers_stop.load(); });
      }
      settler_done = true;
    });
  }
  void tick() {
    auto now = mono_s();
    if (dashboard) {
      if (dash_last_tick > 0)
        dash_tick_gap = std::max(dash_tick_gap, now - dash_last_tick);
      dash_last_tick = now;
    }
    if (!stopping) {
      if (journal.failed() || (paper_audit && paper_audit->failed())) {
        pause_reason = "audit or recorder failed";
        pause_until = std::numeric_limits<double>::infinity();
        begin_stop(1);
      }
      update_hub();
      paper_poll();
      if (now >= next_bankroll) {
        sniper->refresh_bankroll(now);
        next_bankroll = now + 5.0;
      }
      if (now >= next_hybrid) {
        sniper->refresh_hybrid_risk();
        next_hybrid = now + 5.0;
      }
      if (now - last_recovery >= 0.25) {
        last_recovery = now;
        book_recovery();
      }
      if (rec_out.is_open()) {
        rec_out.flush();
        if (!rec_out) {
          pause("recorder disk failure", true);
          begin_stop(1);
        }
      }
      if (sender->ready() && now - last_probe >= 5) {
        // Note the send time only when a probe really goes out: one still in
        // flight keeps its own times, or its late reply would look fast.
        double sent_wall = wall_s();
        if (sender->get_time()) {
          probe_sent = now;
          probe_wall = sent_wall;
          last_probe = now;
        }
      }
      for (int n = 0; n < c.conns; ++n)
        if (cb_open[n] && now - cb_last_tick[n] > 60)
          coinbase[n]->reconnect();
      if (now - last_report >= 60) {
        last_report = now;
        Json status{{"result", "summary"},
                    {"native_cpp", true},
                    {"mode", c.live ? "LIVE" : "PAPER"},
                    {"signals", sniper->n["signals"]},
                    {"orders", orders_sent},
                    {"paper_fills", paper_fills},
                    {"errors", errors},
                    {"counters", sniper->n},
                    {"paper_order_calls", blocked.load()},
                    {"book_history_entries", book.history_entries()},
                    {"coinbase_live", sniper->cb_fresh.connected},
                    {"clob_h2_ready", sender->ready()},
                    {"cash", c.live
                                 ? (balance ? Json(dollars(*balance)) : Json{})
                                 : Json(dollars(ledger.cash))},
                    {"realized_pnl", dollars(ledger.realized)}};
        // The dashboard owns the screen while it runs; the row is still journalled.
        if (!dashboard)
          std::cout << status.dump() << std::endl;
        audit(status);
      }
      if (now - last_snapshot >= (c.live ? 60 : 120) ||
          ledger.entries - snapshot_entries >= 500) {
        ledger.snapshot();
        if (c.live)
          atomic_file(c.state / "live_canary_state.json",
                      Json{{"format", "sniper-native-canary-1"},
                           {"canary", ledger.canary.json()}}
                          .dump());
        last_snapshot = now;
        snapshot_entries = ledger.entries;
      }
      refresh_work();
      if (dashboard && now >= next_dash) {
        next_dash = now + dash_every;
        dash_submit(now);
      }
      if (c.live && !startup_ok) {
        auto r = current();
        auto why = r ? live_readiness(*r) : std::string("no current round found yet");
        if (why.empty()) {
          startup_ok = true;
          audit({{"result", "start"},
                 {"reason", "native live preflight passed"}});
          if (c.dry_run)
            dry_run_report(*r);
        }
        if (now - start_mono > 45 && !startup_ok) {
          auto reason =
              "native live startup checks did not pass within 45 seconds: " + why;
          audit({{"result", "refused"}, {"reason", reason}});
          runtime_fault(reason);
          begin_stop(1);
        }
      }
      if (c.run_for > 0 && now - start_mono >= c.run_for)
        begin_stop(0);
    } else if ((worker_done && refresher_done && settler_done &&
                sent_at.empty()) ||
               now - stop_started > 20) {
      if (user)
        user->stop();
      sender->stop();
      ledger.snapshot();
      audit({{"result", "summary"},
             {"final", true},
             {"signals", sniper->n["signals"]},
             {"orders", orders_sent},
             {"paper_fills", paper_fills},
             {"errors", errors},
             {"counters", sniper->n},
             {"order_capable_calls_in_paper", blocked.load()},
             {"exit_code", exit_code}});
      io.stop();
      return;
    }
    timer.expires_after(std::chrono::milliseconds(50));
    timer.async_wait([this](auto ec) {
      if (!ec) {
        try {
          tick();
        } catch (const std::exception &e) {
          ++errors;
          runtime_fault(e.what());
          begin_stop(1);
          tick();
        }
      }
    });
  }
  void begin_stop(int code) {
    if (stopping)
      return;
    stopping = true;
    exit_code = code;
    stop_started = mono_s();
    workers_stop = true;
    wake.notify_all();
    fetch_wake.notify_all();
    for (auto &ws : coinbase)
      ws->stop();
    market->stop();
    if (sniper)
      sniper->closing = true;
    paper_timer.cancel();
    audit({{"result", "stop"},
           {"reason", code == 143   ? "SIGTERM"
                      : code == 130 ? "SIGINT"
                                    : "shutdown"}});
  }
  int run() {
    std::cout << c.public_view().dump() << std::endl;
    if (c.dash) {
      dash_every = 1.0 / c.dash_fps;
      dashboard = std::make_unique<dash::Dashboard>(STDOUT_FILENO);
    }
    audit({{"result", "start"},
           {"native_cpp", true},
           {"mode", c.live ? "LIVE" : "PAPER"},
           {"reference", "772f2eb"}});
    signals.async_wait([this](auto ec, int sig) {
      if (!ec)
        begin_stop(sig == SIGTERM ? 143 : 130);
    });
    sender->start();
    market->start();
    for (auto &ws : coinbase)
      ws->start();
    if (user)
      user->start();
    workers();
    tick();
    // Last line of defence: an exception escaping any handler stops the bot
    // in order (final summary, ledger snapshot, joined threads) instead of
    // unwinding out of main. Re-arming tick() is safe: expires_after()
    // cancels any wait still pending on the timer.
    for (;;) {
      try {
        io.run();
        break;
      } catch (const std::exception &e) {
        ++errors;
        runtime_fault(e.what());
        try {
          begin_stop(1);
          tick();
        } catch (...) {
          io.stop();
          break;
        }
      }
    }
    if (dashboard) {
      // Back to the normal screen first, so the final report below stays visible.
      dashboard->stop();
      for (auto &what : dash_faults)
        std::cerr << "native runtime fault: " << what << '\n';
    }
    workers_stop = true;
    wake.notify_all();
    if (worker.joinable())
      worker.join();
    if (refresher.joinable())
      refresher.join();
    if (refresher2.joinable())
      refresher2.join();
    if (settler.joinable())
      settler.join();
    journal.close();
    if (paper_audit)
      paper_audit->close();
    if (rec_out.is_open()) {
      rec_out.flush();
      rec_out.close();
      if (!rec_out) {
        exit_code = 1;
        ++errors;
        std::cerr << "native runtime fault: recorder flush failed\n";
      }
    }
    if (journal.failed() || (paper_audit && paper_audit->failed())) {
      exit_code = 1;
      ++errors;
      std::cerr << "native runtime fault: audit flush failed\n";
    }
    struct rusage resource {};
    getrusage(RUSAGE_SELF, &resource);
    std::cout << Json{{"peak_rss_kib", resource.ru_maxrss},
                      {"native_cpp", true},
                      {"stopped", true},
                      {"exit_code", exit_code},
                      {"paper_order_calls", blocked.load()},
                      {"signals", sniper->n["signals"]},
                      {"orders", orders_sent},
                      {"paper_fills", paper_fills},
                      {"dry_run_orders", dry_orders},
                      {"errors", errors}}
                     .dump()
              << std::endl;
    return exit_code;
  }
};
Runtime::Runtime(Config c) : p_(std::make_unique<Impl>(std::move(c))) {}
Runtime::~Runtime() = default;
int Runtime::run() { return p_->run(); }
} // namespace sniper
