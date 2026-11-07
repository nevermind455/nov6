// Hot-path benchmarks. TickToDecision runs the real decision code (a Coinbase
// frame -> sniper::Sniper -> a sized order plan) against a live book;
// TickToIntent adds what live mode does before the socket write: EIP-712
// signing and the durable intent (with and without fdatasync). Nothing here
// opens a network connection or can submit an order.
#include "sniper/math.hpp"
#include "sniper/sniper.hpp"
#include "sniper/storage.hpp"
#include <benchmark/benchmark.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
using namespace sniper;

static void Maths(benchmark::State &state) {
  for (auto _ : state) {
    double mid = .5, move = 1, sigma = .0001, expiry = 120;
    benchmark::DoNotOptimize(mid);
    benchmark::DoNotOptimize(move);
    benchmark::DoNotOptimize(sigma);
    benchmark::DoNotOptimize(expiry);
    auto fair = anchored_fair(mid, move, sigma, expiry);
    auto cap = fair_limit(fair, .01, .01, .07);
    auto n = kelly_shares(fair, .49, .07, .25, 1000, 200);
    benchmark::DoNotOptimize(cap);
    benchmark::DoNotOptimize(n);
  }
}
static void Signing(benchmark::State &state) {
  Signer signer(
      "0x0000000000000000000000000000000000000000000000000000000000000001");
  auto d = keccak("offline native signer benchmark; authorizes no order");
  for (auto _ : state) {
    auto sig = signer.sign(d);
    benchmark::DoNotOptimize(sig);
  }
}

namespace {
struct BenchEnv : SniperEnv {
  double now = 1000, offset = 1793966400.0 + 100 - 1000;
  BookState books{8.0, 30.0, [this] { return now; }, [this] { return now + offset; },
                  [this] { return now + offset; }};
  MarketRules rules;
  std::size_t rows = 0;
  BenchEnv() {
    rules.condition_id = "0xcond";
    rules.fee_rate = py::D("0.07");
    rules.tick_size = py::D("0.01");
    rules.min_order_size = py::D("5");
    rules.up_token_id = "111";
    rules.down_token_id = "222";
    rules.fetched_wall = wall();
    books.connected = true;
    books.set_active({"111", "222"});
  }
  double mono() override { return now; }
  double wall() override { return now + offset; }
  std::string up_token() override { return "111"; }
  std::string down_token() override { return "222"; }
  std::string condition_id() override { return "0xcond"; }
  // two minutes left in the round at every tick, however long the run
  std::optional<double> window_end() override { return wall() + 120.0; }
  BookState &book() override { return books; }
  bool live() override { return false; }
  std::optional<double> read_bankroll() override { return 1000.0; }
  const MarketRules *cached_rules(const std::string &, bool) override {
    return &rules;
  }
  double open_leg_shares(const std::string &, const std::string &) override {
    return 0;
  }
  double trade_window_seconds() override { return 300; }
  double min_seconds_to_expiry() override { return 10; }
  double min_buy_price() override { return .05; }
  double max_buy_price() override { return .95; }
  double own_fill_ttl_s() override { return 0; }
  bool has_recent_own_fill(const std::string &) override { return false; }
  std::optional<PxLevels> net_asks(const BookView &) override { return {}; }
  bool hybrid_initialized() override { return true; }
  long long journal_seq() override { return 0; }
  double open_basis() override { return 0; }
  Json hybrid_risk_view(std::optional<double>, std::optional<long long>,
                        bool) override {
    return Json{{"equity", 1000.0}, {"high_water", 1000.0}};
  }
  std::map<std::string, int> reasons;
  void journal(Json row) override {
    ++rows;
    reasons[row.value("result", std::string()) + ": " +
            row.value("reason", std::string())]++;
  }
  void quote(const char *token, const char *bid, const char *ask) {
    SnapshotOptions o;
    o.ts_ms = std::to_string(static_cast<long long>(wall() * 1000));
    o.require_exchange_ts = true;
    books.apply_snapshot(token, Json::array({{{"price", bid}, {"size", "500"}}}),
                         Json::array({{{"price", ask}, {"size", "500"}}}), o);
  }
};
std::string ticker(long long seq, double mid, double wall) {
  char buf[512];
  std::snprintf(buf, sizeof buf,
                R"({"type":"ticker","sequence":%lld,"product_id":"BTC-USD","price":"%.2f","best_bid":"%.2f","best_ask":"%.2f","time":"2026-11-06T00:00:00.000000Z","side":"buy","trade_id":%lld,"last_size":"0.001"})",
                seq, mid, mid - 0.005, mid + 0.005, seq);
  (void)wall;
  return buf;
}
// A warmed Sniper whose next tick is a +1 bp move that becomes an order.
struct Rig {
  BenchEnv env;
  SniperSettings s;
  std::unique_ptr<Sniper> sn;
  long long seq = 1;
  double mid = 60000;
  Rig() {
    s.cooldown_ms = 0;
    s.coinbase_conns = 1;
    s.max_fills_round = 1 << 30;
    s.max_side_shares = 1e12;
    sn = std::make_unique<Sniper>(s, env);
    sn->cb_fresh.enabled = false;
    // 70 s of one-second samples warm the volatility estimate
    for (int i = 0; i < 70; ++i) {
      env.now += 1;
      mid *= (i % 2 ? 1.00004 : 0.99996);
      env.quote("111", ".48", ".50");
      env.quote("222", ".49", ".51");
      sn->on_coinbase(ticker(seq++, mid, env.wall()), env.now * 1000);
    }
    sn->refresh_bankroll(env.now);
    sn->refresh_hybrid_risk();
  }
  // Four quiet one-second ticks (volatility stays near 0.5 bp/sqrt(s)),
  // then the timed frame: a 1.2 bp move, alternately up and down.
  bool up = true;
  std::string prepare() {
    for (int k = 0; k < 4; ++k) {
      env.now += 1.0;
      env.quote("111", ".48", ".50");
      env.quote("222", ".49", ".51");
      sn->on_coinbase(ticker(seq++, mid, env.wall()), env.now * 1000);
    }
    env.now += 0.3;
    env.quote("111", ".48", ".50");
    env.quote("222", ".49", ".51");
    sn->on_coinbase(ticker(seq++, mid, env.wall()), env.now * 1000);
    sn->refresh_bankroll(env.now);
    sn->refresh_hybrid_risk();
    env.now += 0.01;
    mid *= up ? 1.00012 : 1 / 1.00012;
    up = !up;
    return ticker(seq++, mid, env.wall());
  }
};
} // namespace

static void TickToDecision(benchmark::State &state) {
  Rig rig;
  std::size_t orders = 0;
  for (auto _ : state) {
    state.PauseTiming();
    auto frame = rig.prepare();
    state.ResumeTiming();
    rig.sn->on_coinbase(frame, rig.env.now * 1000);
    auto plans = rig.sn->take_dispatched();
    state.PauseTiming();
    orders += plans.size();
    for (auto &p : plans) {
      PaperResult r;
      r.error = "bench";
      rig.sn->finish_paper(p, r);
    }
    state.ResumeTiming();
  }
  if (std::getenv("BENCH_REASONS"))
    for (auto &[k, v] : rig.env.reasons)
      std::fprintf(stderr, "%8d %s\n", v, k.c_str());
  state.counters["orders_per_tick"] =
      benchmark::Counter(static_cast<double>(orders) /
                         static_cast<double>(std::max<std::int64_t>(1, state.iterations())));
}

static void TickToIntent(benchmark::State &state) {
  DurableLog::benchmark_without_sync = state.range(0) == 0;
  auto dir = std::filesystem::temp_directory_path() /
             ("sniper-bench-" + std::to_string(getpid()) + "-" +
              std::to_string(state.range(0)));
  std::filesystem::create_directories(dir);
  {
    Ledger ledger(dir / "ledger.json", 1000 * U, false);
    Signer signer(
        "0x0000000000000000000000000000000000000000000000000000000000000001");
    Credentials creds{"k", "c2VjcmV0", "p"};
    Rules rules{10000, 5 * U, 70000, 1, false, true, 1};
    Rig rig;
    long long n = 0;
    for (auto _ : state) {
      state.PauseTiming();
      auto frame = rig.prepare();
      state.ResumeTiming();
      rig.sn->on_coinbase(frame, rig.env.now * 1000);
      auto plans = rig.sn->take_dispatched();
      for (auto &p : plans) {
        auto order = build_order(signer, signer.address(), 0,
                                 "71321045679252212594626385532706912750332728"
                                 "571942532289631379312455583992563",
                                 units(py::D(p->amount).fixed(6)),
                                 units(py::fixed(p->cap, 6)), rules, creds);
        Intent intent;
        intent.id = order.id + std::to_string(++n);
        intent.condition = "0x" + std::string(64, '1');
        intent.token = "111";
        intent.up = "111";
        intent.down = "222";
        intent.side = p->side;
        intent.slug = "bench";
        intent.status = "sent";
        intent.stake = units(py::D(p->amount).fixed(6));
        intent.reserve = intent.stake * 2;
        intent.target = *p->shares * U;
        intent.limit = units(py::fixed(p->cap, 6));
        intent.fee_rate = 70000;
        ledger.register_intent(intent, &order, ledger.canary);
        benchmark::DoNotOptimize(order);
      }
      state.PauseTiming();
      for (auto &p : plans) {
        PaperResult r;
        r.error = "bench";
        rig.sn->finish_paper(p, r);
      }
      state.ResumeTiming();
    }
  }
  std::filesystem::remove_all(dir);
  DurableLog::benchmark_without_sync = false;
}

// p50/p99 of the in-process path a live order takes before the socket
// write: frame -> decision -> signed order -> durable intent.
static void TickToWirePercentiles(benchmark::State &state) {
  bool sync = state.range(0) != 0;
  DurableLog::benchmark_without_sync = !sync;
  auto dir = std::filesystem::temp_directory_path() /
             ("sniper-pct-" + std::to_string(getpid()) + "-" +
              std::to_string(state.range(0)));
  std::filesystem::create_directories(dir);
  std::vector<double> decide_us, wire_us;
  {
    Ledger ledger(dir / "ledger.json", 1000 * U, false);
    Signer signer(
        "0x0000000000000000000000000000000000000000000000000000000000000001");
    Credentials creds{"k", "c2VjcmV0", "p"};
    Rules rules{10000, 5 * U, 70000, 1, false, true, 1};
    Rig rig;
    long long n = 0;
    for (auto _ : state) {
      for (int i = 0; i < 2000; ++i) {
        auto frame = rig.prepare();
        auto t0 = std::chrono::steady_clock::now();
        rig.sn->on_coinbase(frame, rig.env.now * 1000);
        auto plans = rig.sn->take_dispatched();
        auto t1 = std::chrono::steady_clock::now();
        for (auto &p : plans) {
          auto order = build_order(
              signer, signer.address(), 0,
              "71321045679252212594626385532706912750332728"
              "571942532289631379312455583992563",
              units(py::D(p->amount).fixed(6)), units(py::fixed(p->cap, 6)),
              rules, creds);
          Intent intent;
          intent.id = order.id + std::to_string(++n);
          intent.condition = "0x" + std::string(64, '1');
          intent.token = "111";
          intent.up = "111";
          intent.down = "222";
          intent.side = p->side;
          intent.slug = "bench";
          intent.status = "sent";
          intent.stake = units(py::D(p->amount).fixed(6));
          intent.reserve = intent.stake * 2;
          intent.target = *p->shares * U;
          intent.limit = units(py::fixed(p->cap, 6));
          intent.fee_rate = 70000;
          ledger.register_intent(intent, &order, ledger.canary);
        }
        auto t2 = std::chrono::steady_clock::now();
        if (!plans.empty()) {
          decide_us.push_back(
              std::chrono::duration<double, std::micro>(t1 - t0).count());
          wire_us.push_back(
              std::chrono::duration<double, std::micro>(t2 - t0).count());
        }
        for (auto &p : plans) {
          PaperResult r;
          r.error = "bench";
          rig.sn->finish_paper(p, r);
        }
      }
    }
  }
  std::filesystem::remove_all(dir);
  DurableLog::benchmark_without_sync = false;
  auto pct = [](std::vector<double> v, double q) {
    if (v.empty())
      return 0.0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(q * static_cast<double>(v.size() - 1))];
  };
  state.counters["orders"] = static_cast<double>(wire_us.size());
  state.counters["decide_p50_us"] = pct(decide_us, .5);
  state.counters["decide_p95_us"] = pct(decide_us, .95);
  state.counters["decide_p99_us"] = pct(decide_us, .99);
  state.counters["wire_p50_us"] = pct(wire_us, .5);
  state.counters["wire_p95_us"] = pct(wire_us, .95);
  state.counters["wire_p99_us"] = pct(wire_us, .99);
}
// One websocket price-change delta applied to an in-memory book, from the
// parsed message to the updated level map (local processing only).
static void BookUpdatePercentiles(benchmark::State &state) {
  double now = 1'762'500'000.0;
  BookState books{8.0, 30.0, [&] { return now; }, [&] { return now; }};
  books.connected = true;
  books.set_active({"111"});
  SnapshotOptions o;
  o.ts_ms = std::to_string(static_cast<long long>(now * 1000));
  o.require_exchange_ts = true;
  Json bids = Json::array(), asks = Json::array();
  for (int i = 1; i <= 20; ++i) {
    bids.push_back({{"price", py::fixed(0.50 - 0.01 * i, 2)}, {"size", "500"}});
    asks.push_back({{"price", py::fixed(0.50 + 0.01 * i, 2)}, {"size", "500"}});
  }
  books.apply_snapshot("111", bids, asks, o);
  const int n = 20000;
  std::vector<Json> changes, stamps;
  for (int i = 0; i < n; ++i) {
    bool bid = i % 2 == 0;
    changes.push_back(Json::array({{{"asset_id", "111"},
                                    {"price", py::fixed(bid ? 0.45 : 0.55, 2)},
                                    {"size", std::to_string(100 + i % 400)},
                                    {"side", bid ? "BUY" : "SELL"}}}));
    stamps.push_back(std::to_string(static_cast<long long>(now * 1000) + i + 1));
  }
  std::vector<double> us;
  us.reserve(n);
  for (auto _ : state) {
    for (int i = 0; i < n; ++i) {
      now += 0.001;
      auto t0 = std::chrono::steady_clock::now();
      bool ok = books.apply_price_changes(changes[i], stamps[i], Json());
      auto t1 = std::chrono::steady_clock::now();
      if (!ok)
        state.SkipWithError("book update refused");
      us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
  }
  std::sort(us.begin(), us.end());
  auto at = [&](double q) { return us[static_cast<std::size_t>(q * (us.size() - 1))]; };
  state.counters["updates"] = static_cast<double>(us.size());
  state.counters["p50_us"] = at(.5);
  state.counters["p95_us"] = at(.95);
  state.counters["p99_us"] = at(.99);
}
BENCHMARK(BookUpdatePercentiles)->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(TickToWirePercentiles)->Arg(0)->Arg(1)->Iterations(1)->Unit(benchmark::kMillisecond);
BENCHMARK(Maths);
BENCHMARK(Signing);
BENCHMARK(TickToDecision)->Repetitions(1);
BENCHMARK(TickToIntent)->Arg(0)->Arg(1);
