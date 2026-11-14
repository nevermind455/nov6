// sniper-replay: one recording through the C++ decision code (sniper::Sniper,
// sniper::BookState) on a replay clock, with the same scripted broker and
// account as the Python reference's replay did (that tool is in git history at
// commit d5221b5; this repository is C++ only).
// Usage: sniper-replay RECORDING.jsonl OUT.jsonl [CONFIG.json]
#include "sniper/config.hpp"
#include "sniper/sniper.hpp"
#include <cmath>
#include <fstream>
#include <iostream>

using namespace sniper;
using py::D;

namespace {
constexpr double FRACTIONS[] = {1.0, 0.6, 0.0, 0.3, 1.0, 0.0};
constexpr double START_CASH = 1000.0, REFRESH_S = 5.0;

struct Position {
  std::string cond;
  double shares = 0, cost = 0;
};

struct ReplayEnv : SniperEnv {
  double now_mono = 0, offset = 0;
  BookState books{8.0, 30.0, [this] { return now_mono; },
                  [this] { return now_mono + offset; },
                  [this] { return now_mono + offset; }};
  std::string up, down, cond;
  std::optional<double> end;
  std::map<std::string, MarketRules> rules;
  std::vector<std::string> pos_order;
  std::map<std::string, Position> positions;
  double realized = 0;
  std::optional<Json> state;
  long long seq = 0;
  std::ofstream out;
  double mono() override { return now_mono; }
  double wall() override { return now_mono + offset; }
  std::string up_token() override { return up; }
  std::string down_token() override { return down; }
  std::string condition_id() override { return cond; }
  std::optional<double> window_end() override { return end; }
  BookState &book() override { return books; }
  bool live() override { return false; }
  std::optional<double> read_bankroll() override {
    std::vector<double> costs;
    for (auto &t : pos_order)
      costs.push_back(positions[t].cost);
    return START_CASH - py::builtin_sum(costs) + realized;
  }
  const MarketRules *cached_rules(const std::string &c, bool ttl) override {
    auto it = rules.find(c);
    if (it == rules.end())
      return nullptr;
    if (ttl) {
      double age = wall() - it->second.fetched_wall;
      if (!(0 <= age && age <= 300.0))
        return nullptr;
    }
    return &it->second;
  }
  double open_leg_shares(const std::string &c, const std::string &t) override {
    auto it = positions.find(t);
    if (it == positions.end() || it->second.cond != c || it->second.shares <= 0)
      return 0.0;
    return it->second.shares;
  }
  double trade_window_seconds() override { return 300.0; }
  double min_seconds_to_expiry() override { return 10.0; }
  double min_buy_price() override { return 0.05; }
  double max_buy_price() override { return 0.95; }
  double own_fill_ttl_s() override { return 0; }
  bool has_recent_own_fill(const std::string &) override { return false; }
  std::optional<PxLevels> net_asks(const BookView &) override {
    return std::nullopt;
  }
  bool hybrid_initialized() override { return state.has_value(); }
  long long journal_seq() override { return seq; }
  double open_basis() override {
    std::vector<double> costs;
    for (auto &t : pos_order)
      if (positions[t].shares > 0)
        costs.push_back(positions[t].cost);
    return py::builtin_sum(costs);
  }
  Json hybrid_risk_view(std::optional<double> reference,
                        std::optional<long long>, bool rebase) override {
    Json st;
    if (!state) {
      if (!reference)
        throw py::ValueError(
            "first hybrid risk reading needs positive reference equity");
      if (*reference <= 0)
        throw py::ValueError("hybrid reference equity must be positive");
      st = Json{{"reference_equity", *reference},
                {"pnl_baseline", realized},
                {"high_water", *reference}};
    } else
      st = *state;
    double equity = st["reference_equity"].get<double>() + realized -
                    st["pnl_baseline"].get<double>();
    if (rebase) {
      st["reference_equity"] = equity;
      st["pnl_baseline"] = realized;
      st["high_water"] = equity;
    }
    st["high_water"] = std::max(st["high_water"].get<double>(), equity);
    state = st;
    Json out = st;
    out["equity"] = equity;
    out["realized_total"] = realized;
    return out;
  }
  void journal(Json row) override { out << row.dump() << "\n"; }
  void book_fill(const std::string &c, const std::string &t, double shares,
                 double cost) {
    if (!positions.count(t)) {
      pos_order.push_back(t);
      positions[t].cond = c;
    }
    positions[t].shares += shares;
    positions[t].cost += cost;
    ++seq;
  }
};

SniperSettings settings_from(const Json &cfg) {
  // The same SNIPER_* names and defaults the runtime reads (config.cpp).
  std::map<std::string, std::string> env;
  for (auto &[k, v] : cfg.items())
    env[k] = v.is_string() ? v.get<std::string>() : v.dump();
  return sniper_settings(env);
}
} // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: sniper-replay RECORDING.jsonl OUT.jsonl [CONFIG.json]\n";
    return 2;
  }
  Json cfg = Json::object();
  if (argc > 3) {
    std::ifstream c(argv[3]);
    c >> cfg;
  }
  std::vector<Json> events;
  {
    std::ifstream in(argv[1]);
    std::string line;
    while (std::getline(in, line))
      if (!line.empty())
        events.push_back(Json::parse(line));
  }
  if (events.empty())
    return 1;
  ReplayEnv env;
  env.out.open(argv[2]);
  if (!env.out) {
    std::cerr << "could not open replay output\n";
    return 1;
  }
  env.now_mono = events[0]["t"].get<double>();
  env.offset = events[0]["w"].get<double>() - env.now_mono;
  SniperSettings s;
  try {
    s = settings_from(cfg);
  } catch (const std::exception &e) {
    std::cerr << "settings refused: " << e.what() << "\n";
    return 1;
  }
  Sniper sn(s, env);
  sn.cb_fresh.enabled = s.coinbase_freshness;
  struct Pending {
    double release;
    long long order;
    std::shared_ptr<Plan> plan;
  };
  std::vector<Pending> pending;
  long long submitted = 0, broker_n = 0;
  double next_bankroll = env.now_mono, next_hybrid = env.now_mono + REFRESH_S;
  bool cb_up = false;
  std::optional<Json> current, prepared;
  std::map<std::string, BookView> rest_views;
  std::set<std::string> fetched;
  auto cache_rules = [&](const Json &r, bool refresh) {
    auto c = r["cond"].get<std::string>();
    if (!refresh && fetched.count(c))
      return;
    fetched.insert(c);
    const Json &x = r["rules"];
    MarketRules m;
    m.condition_id = x["condition_id"].get<std::string>();
    m.fee_rate = D(std::string_view(x["fee_rate"].get<std::string>()));
    m.fee_exponent = x["fee_exponent"].get<int>();
    m.min_order_size = D(std::string_view(x["min_order_size"].get<std::string>()));
    m.tick_size = D(std::string_view(x["tick_size"].get<std::string>()));
    m.up_token_id = x["up_token_id"].get<std::string>();
    m.down_token_id = x["down_token_id"].get<std::string>();
    m.taker_delay_ms = x["taker_delay_ms"].get<double>();
    m.taker_delay_flagged = x["taker_delay_flagged"].get<bool>();
    m.fetched_wall = env.wall();
    env.rules[c] = std::move(m);
  };

  auto collect = [&] {
    for (auto &p : sn.take_dispatched())
      pending.push_back({p->sent_mono + p->lat / 1000.0, submitted++, p});
  };
  auto resolve = [&](const std::shared_ptr<Plan> &plan) {
    PaperResult res;
    if (!plan->sign_error.empty()) {
      sn.finish_paper(plan, res);
      return;
    }
    double f = FRACTIONS[broker_n % 6];
    ++broker_n;
    if (f == 0) {
      res.error = "FAK no-fill: nothing at or below the cap";
    } else {
      double price = py::round(plan->cap - 0.03, 4);
      double shares = py::round(plan->amount * f / price, 4);
      double cost = py::round(shares * price, 4);
      env.book_fill(plan->cond, plan->token, shares, cost);
      res.filled = true;
      res.fill = Json{{"tag", "sniper"},
                      {"order_id", "r" + std::to_string(broker_n)},
                      {"shares", shares},
                      {"average_price", price},
                      {"total_cost", cost},
                      {"fee", 0.0},
                      {"partial", f < 1.0}};
    }
    sn.finish_paper(plan, res);
  };
  auto settle = [&](double until) {
    for (;;) {
      std::optional<std::size_t> pick;
      for (std::size_t i = 0; i < pending.size(); ++i)
        if (pending[i].release <= until &&
            (!pick || pending[i].release < pending[*pick].release))
          pick = i;
      double t_order = pick ? pending[*pick].release : INFINITY;
      double t_timer = INFINITY;
      for (double x : {next_bankroll, next_hybrid})
        if (x <= until)
          t_timer = std::min(t_timer, x);
      if (!pick && !std::isfinite(t_timer))
        return;
      if (t_order <= t_timer) {
        auto item = pending[*pick];
        pending.erase(pending.begin() + static_cast<long>(*pick));
        env.now_mono = item.release;
        resolve(item.plan);
      } else {
        env.now_mono = t_timer;
        if (next_bankroll <= t_timer) {
          sn.refresh_bankroll(env.now_mono);
          next_bankroll += REFRESH_S;
        }
        if (next_hybrid <= t_timer) {
          sn.refresh_hybrid_risk();
          next_hybrid += REFRESH_S;
        }
      }
    }
  };
  auto tokens = [&] {
    std::vector<std::string> out;
    for (auto *r : {&current, &prepared})
      if (*r) {
        out.push_back((**r)["up"].get<std::string>());
        out.push_back((**r)["down"].get<std::string>());
      }
    return out;
  };
  for (auto &e : events) {
    double t = e["t"].get<double>();
    settle(t);
    // Timers before this event use the preceding clock sample. Apply the
    // recorded wall clock at receipt, including NTP corrections.
    env.offset = e["w"].get<double>() - t;
    env.now_mono = t;
    std::string k = e["k"].get<std::string>();
    if (k == "cb") {
      const Json &b = get(e, "backlog");
      sn.cb_fresh.note_message(t, b.is_number() ? std::optional<int>(b.get<int>())
                                                : std::nullopt);
      try {
        sn.on_coinbase(e["raw"].get<std::string>(), t * 1000.0);
      } catch (const std::exception &) {
      }
      collect();
    } else if (k == "xf") {
      // SNIPER_EXTRA_FEEDS frames: replayed when the settings name the feed
      const auto feed = get(e, "feed").is_string() ? e["feed"].get<std::string>() : "";
      for (std::size_t i = 0; i < sn.extra.size(); ++i)
        if (sn.extra[i].name == feed) {
          const Json &b = get(e, "backlog");
          sn.extra[i].fresh.note_message(t, b.is_number() ? std::optional<int>(b.get<int>())
                                                          : std::nullopt);
          try {
            sn.on_extra(i, e["raw"].get<std::string>(), t * 1000.0);
          } catch (const std::exception &) {
          }
          collect();
        }
    } else if (k == "xconn") {
      const auto feed = get(e, "feed").is_string() ? e["feed"].get<std::string>() : "";
      for (std::size_t i = 0; i < sn.extra.size(); ++i)
        if (sn.extra[i].name == feed) {
          if (get(e, "up") == Json(true))
            sn.on_extra_connect(i);
          else
            sn.on_extra_disconnect(i);
        }
    } else if (k == "cbconn") {
      bool up = e["up"].get<bool>();
      if (up && !cb_up)
        sn.on_coinbase_connect();
      else if (!up && cb_up)
        sn.on_coinbase_disconnect();
      cb_up = up;
    } else if (k == "mconn") {
      bool up = e["up"].get<bool>();
      env.books.connected = up;
      env.books.desync_all(up ? "connect" : "disconnect");
    } else if (k == "mkt") {
      handle_market_frame(env.books, e["raw"].get<std::string>());
    } else if (k == "round") {
      Json r = e;
      if (e["current"].get<bool>()) {
        current = r;
        if (prepared && (*prepared)["cond"] == r["cond"])
          prepared.reset();
        env.up = r["up"].get<std::string>();
        env.down = r["down"].get<std::string>();
        env.cond = r["cond"].get<std::string>();
        env.end = r["end"].get<double>();
      } else
        prepared = r;
      env.books.set_active(tokens());
      cache_rules(r, false);
    } else if (k == "rules") {
      cache_rules(e, true);
    } else if (k == "rest_req") {
      std::string token = e["token"].get<std::string>();
      BookView v = env.books.view(token);
      bool fresh = v.status == LIVE && v.updated_mono &&
                   (env.now_mono - *v.updated_mono) * 1000.0 < 1500.0;
      if (fresh)
        rest_views.erase(token);
      else
        rest_views[token] = v;
    } else if (k == "rest_resp") {
      std::string token = e["token"].get<std::string>();
      auto it = rest_views.find(token);
      if (it != rest_views.end()) {
        BookView v = it->second;
        rest_views.erase(it);
        if (env.books.is_active(token)) {
          SnapshotOptions o;
          o.only_if_unsynced = true;
          o.expected_generation = v.generation;
          o.expected_updates = v.updates;
          o.expected_receive_revision = v.receive_revision;
          o.refresh_if_older_than = 1.5;
          o.source = "rest";
          o.ts_ms = e["timestamp"];
          o.received_mono = env.now_mono;
          if (!e["tick"].is_null())
            o.tick = e["tick"].get<double>();
          if (!e["hash"].is_null())
            o.hash = e["hash"].get<std::string>();
          o.require_exchange_ts = true;
          env.books.apply_snapshot(token, e["bids"], e["asks"], o);
        }
      }
    } else if (k == "ws_gap") {
      env.books.subscription_gap(e["token"].get<std::string>(),
                                 e["generation"].get<std::uint64_t>());
    } else if (k == "account") {
      env.realized = e["realized"].get<double>();
    }
  }
  settle(events.back()["t"].get<double>() + 30.0);
  env.out.flush();
  if (!env.out) {
    std::cerr << "replay output write failed\n";
    return 1;
  }
  return 0;
}
