#pragma once
// Port of sniper_mode.Sniper's decision path (nevermind455/oct10@772f2eb):
// on_coinbase -> _signal -> _fire_now -> _decide, the skip journal, the
// per-side cooldown and in-flight state, round counters, the cached paper
// bankroll with its spend tail, the hybrid risk snapshot and minimum entries.
// Everything the reference reads from its hub, broker, ledger and clock comes
// through SniperEnv, so the runtime, the paper broker and the replay driver
// all run this one implementation.
#include "book.hpp"
#include "hybrid.hpp"
#include "pyrt.hpp"
#include "trigger.hpp"
#include "volatility.hpp"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace sniper {

// paper_trade.MarketRules, as the broker caches it per condition.
struct MarketRules {
  std::string condition_id;
  py::Dec fee_rate = py::D(0);
  int fee_exponent = 1;
  std::optional<py::Dec> min_order_size, tick_size;
  std::optional<std::string> up_token_id, down_token_id;
  std::optional<double> taker_delay_ms = 0.0;
  bool taker_delay_flagged = false;
  std::string source = "venue";
  double fetched_wall = 0; // _rules_cache entry time
};

// The sniper_mode.Settings fields the decision path reads.
struct SniperSettings {
  double k_bp = 0.5, window_ms = 250, shares = 10, slip = 0.05;
  bool fixed_latency = false;
  double latency_ms = 200, hold_ms = 250, server_ms = 25, net_default_ms = 14,
         paper_extra_ms = 200;
  double cooldown_ms = 300, min_left_s = 10;
  int max_fills_round = 20;
  double max_side_shares = 200;
  bool fair = true, fair_no_limit = false;
  double fair_margin = 0.01, min_gap_c = 0, fair_vol_s = 300;
  bool kelly = true, flat_live = false, hybrid_sizing = true,
       kelly_min_entry = false;
  double kelly_fraction = 0.25, min_entry_max_drawdown = 0;
  hybrid::Policy hybrid_policy;
  double hybrid_dd_pause_s = 600;
  double stale_ms = 2000, book_wait_ms = 500, venue_offset_ms = 0;
  std::string book_clock = "exchange";
  bool book_quiet = false;
  std::string order_type = "FAK", order_sizing = "shares";
  double coinbase_stale_ms = 1000, coinbase_unknown_ms = 5000;
  int coinbase_backlog_max = 20, coinbase_conns = 2;
  bool coinbase_freshness = true;
  std::string backlog_gate = "count";
  double backlog_fresh_ms = 100;
  double venue_min_shares = 0;
  // One more FAK after a confirmed zero-fill miss, while the signal is fresh
  // and its move still holds. 0 = off (the reference behaviour).
  int miss_retry = 0;
  double miss_retry_ms = 1000;
  // SNIPER_REST_LIVE (live only): after a FAK that matched nothing, rest the
  // same BUY - its shares at its limit - as a post-only order for at most
  // rest_s seconds, then cancel it. rest_dir keeps it to the side that is
  // beating the price to beat. Off for good once the resting orders' own
  // settled loss reaches rest_stop_usd.
  bool rest_live = false, rest_dir = true;
  double rest_s = 5, rest_stop_usd = 10;
  bool ptb_skip_close = false;
  double ptb_margin_bp = 1.0;
  std::string primary_feed = "coinbase";
  // SNIPER_EXTRA_FEEDS: trigger feeds beside the primary, in order -
  // "binance_fut" (Binance USD-M perpetual BTCUSDT book ticker) and
  // "binance_spot" (Binance spot BTCUSDT book ticker, its trades on the same
  // socket for the exchange's clock). The same rule runs on each feed's own
  // mid with its own freshness gate; whichever signals first fires, under the
  // per-side cooldown and in-flight flag all feeds share: one move, one order.
  // Empty (the default): the primary only.
  std::vector<std::string> extra_feeds;
  bool binance_freshness = true;
  double binance_stale_ms = 1000, binance_unknown_ms = 5000, binance_silent_s = 5;
  int binance_backlog_max = 20;
  double coinbase_silent_s = 5;
  double ptb_max_gap_s = 5, ptb_min_checks = 6;
};

// price_to_beat.Calibration: published prices to beat against our TWAPs.
struct PtbCalibration {
  std::deque<double> diffs, errors; // maxlen 12 / 24
  std::set<double> seen;
  double basis() const;
  int checks() const { return static_cast<int>(errors.size()); }
  double margin_bp(double floor_bp) const;
  Json add(double start, std::optional<double> raw, std::optional<double> est,
           int basis_rounds, double official);
};

// sniper_mode.Settings: parse and validate the SNIPER_* environment exactly
// as the reference does (names, defaults, refusal texts). `get` returns a
// variable's raw value or nullopt when unset. Throws std::invalid_argument.
SniperSettings sniper_settings(
    const std::function<std::optional<std::string>(const std::string &)> &get,
    std::vector<std::string> *notices = nullptr);
SniperSettings sniper_settings(const std::map<std::string, std::string> &env,
                               std::vector<std::string> *notices = nullptr);

struct Plan;
struct HybridSnapshot {
  double equity = 0, high_water = 0, observed_mono = 0;
  Json raw; // the ledger's view, journalled with the decision
};

// What the reference reads from its hub, broker, ledger, live sender and
// clock. Paper implementations come from the runtime; the replay driver
// scripts them.
struct SniperEnv {
  virtual ~SniperEnv() = default;
  virtual double mono() = 0; // time.monotonic()
  virtual double wall() = 0; // time.time()
  virtual std::string up_token() = 0;
  virtual std::string down_token() = 0;
  virtual std::string condition_id() = 0;
  virtual std::optional<double> window_end() = 0;
  virtual BookState &book() = 0;
  virtual bool live() = 0;
  // _read_bankroll: paper broker cash; live: collateral less reservations
  virtual std::optional<double> read_bankroll() = 0;
  // broker.cached_rules / _rules_cache (with its TTL for fees)
  virtual const MarketRules *cached_rules(const std::string &cond,
                                          bool fee_ttl) = 0;
  virtual double open_leg_shares(const std::string &cond,
                                 const std::string &token) = 0;
  // live only (None in paper)
  virtual std::optional<double> own_risk_shares(const std::string &,
                                                const std::string &) {
    return std::nullopt;
  }
  virtual std::optional<double> order_share_cap() { return std::nullopt; }
  virtual bool awaiting_fak(const std::string & /*side*/,
                            const std::string & /*cond*/,
                            const std::string & /*token*/) {
    return false;
  }
  // live SNIPER_REST_LIVE: an order of ours may be resting in this market;
  // it takes no other order of ours until that one is off the book.
  virtual bool resting_in(const std::string & /*cond*/) { return false; }
  // broker attributes
  virtual double trade_window_seconds() = 0;
  virtual double min_seconds_to_expiry() = 0;
  virtual double min_buy_price() = 0;
  virtual double max_buy_price() = 0;
  virtual double own_fill_ttl_s() = 0;
  virtual bool has_recent_own_fill(const std::string &token) = 0;
  // _net_of_own_fills(snapshot_from_book_view(view)).asks as floats, or
  // nullopt when the reference falls back to the raw view (PaperRejected)
  virtual std::optional<PxLevels> net_asks(const BookView &view) = 0;
  // the ledger's hybrid_risk_view / risk_view; throw on failure
  virtual bool hybrid_initialized() = 0;
  virtual long long journal_seq() = 0;
  virtual double open_basis() = 0;
  virtual Json hybrid_risk_view(std::optional<double> reference,
                                std::optional<long long> expected_seq,
                                bool rebase) = 0;
  virtual void journal(Json row) = 0;
  // _ptb_view's journal fields (price to beat), {} when not tracked
  virtual Json ptb_view(const std::string & /*side*/, double /*end*/) {
    return Json::object();
  }
  // live: half the median CLOB round trip, when measured
  virtual std::vector<double> net_rtt() { return {}; }
  // live: prepare, sign, write the intent and send; false when refused
  // before the wire (the row is written by the sender then)
  virtual bool dispatch_live(const std::shared_ptr<Plan> &) { return false; }
};

struct InFlight {
  std::optional<double> end;
  double amount = 0, risk_shares = 0;
};

struct RoundCounts {
  double up = 0, down = 0;
  long long fills = 0;
  double &side(const std::string &s) { return s == "UP" ? up : down; }
};

struct Plan {
  std::string side, token, other, cond, up, down, order_type, tick;
  std::string feed = "coinbase"; // the trigger feed whose signal this is
  double end = 0, amount = 0, cap = 0, lat = 0, risk_shares = 0;
  double dispatch_deadline = 0, t0 = 0, t_ms = 0, decided_mono = 0;
  double sent_wall = 0, sent_mono = 0;
  std::optional<double> fair_fee_rate, risk_budget_usd, target;
  std::optional<int> fair_fee_exponent;
  std::optional<long long> shares, venue_min_shares;
  std::optional<hybrid::Factors> hybrid;
  bool minimum_entry = false;
  std::tuple<std::string, std::string, double> minimum_entry_key;
  std::optional<long long> paper_spend_token;
  std::string sign_error; // set when dispatch refused before the broker
  // live: why the order was not sent (journalled as "live refused", or
  // "live halted" when new orders are paused)
  std::string live_refusal;
  bool live_halted = false;
  Json row;      // the decision row, journalled when the outcome is known
  BookView view; // the book the decision saw
  double r_bp = 0;
  int retry = 0; // 0 for a signal's order, 1 for its re-try after a miss
  std::int64_t t_ns = 0;
};

// The paper broker's answer for one order (paper_trade last_fill/last_error).
struct PaperResult {
  bool filled = false;
  std::string error;
  Json fill;    // the reference's last_fill fields
  Json arrival; // last_timing["arrival"] (info of the arrival book)
};

class Sniper {
public:
  Sniper(const SniperSettings &, SniperEnv &);
  SniperSettings s;
  SniperEnv &env;
  Trigger trigger;
  Volatility vol;
  Freshness cb_fresh;
  std::map<std::string, std::uint64_t> n;
  std::optional<long long> coinbase_seq;
  std::optional<double> last_mid;
  std::optional<InFlight> inflight[2];
  double last_fire_by_side[2]{-1e18, -1e18};
  std::map<double, RoundCounts> rounds;
  std::map<std::string, std::pair<double, long long>> round_baseline;
  std::set<std::tuple<std::string, std::string, double>> minimum_entry_keys;
  std::string minimum_entry_seed_error;
  std::optional<HybridSnapshot> hybrid_risk;
  std::string hybrid_risk_error = "hybrid accounting not ready";
  std::optional<double> dd_stop_since;
  bool closing = false;

  // A Coinbase frame received at monotonic t_ms (and wall time from env).
  // Returns the reference's kind: tick, heartbeat, subscribed, late, other,
  // malformed. Throws CoinbaseSubscribeError text as std::runtime_error.
  std::string on_coinbase(std::string_view raw, double t_ms,
                          std::int64_t t_ns = 0);
  // _coinbase_conn: the first socket up restarts the rule from scratch; the
  // last socket down disconnects the freshness gate.
  void on_coinbase_connect() {
    trigger = Trigger(s.k_bp, s.window_ms);
    last_mid.reset();
    coinbase_seq.reset();
    cb_fresh.on_connect();
  }
  void on_coinbase_disconnect() { cb_fresh.on_disconnect(); }
  // SNIPER_EXTRA_FEEDS, in order: each with its own rule, freshness gate and
  // latest mid (sniper/feeds.hpp has their messages).
  struct Extra {
    std::string name;
    Trigger trigger;
    Freshness fresh;
    std::optional<double> last_mid;
  };
  std::vector<Extra> extra;
  // A frame of extra feed i received at monotonic t_ms. Returns tick, trade,
  // other or malformed.
  std::string on_extra(std::size_t i, std::string_view raw, double t_ms,
                       std::int64_t t_ns = 0);
  // Its socket came up (the rule starts afresh) or went down.
  void on_extra_connect(std::size_t i);
  void on_extra_disconnect(std::size_t i) { extra.at(i).fresh.on_disconnect(); }
  // A feed by name: its freshness gate, rule and latest mid (the primary's
  // for the primary or an unknown name).
  const Freshness &freshness(const std::string &feed) const;
  std::optional<double> mid_of(const std::string &feed) const;
  // _ptb_official_loop: Polymarket published this round's price to beat
  void ptb_official(double start, double official);
  // legacy price-to-beat estimate state (_note_primary_mid)
  PtbCalibration ptb_calib;
  std::optional<std::pair<double, double>> primary_mid;
  // Orders decided by the last on_coinbase call, in order.
  std::vector<std::shared_ptr<Plan>> take_dispatched();
  // _finish for a paper order (or a dispatch refusal).
  void finish_paper(const std::shared_ptr<Plan> &, const PaperResult &);
  // Release a side without an outcome row (live path does its own rows).
  void release(const std::string &side) { inflight[idx(side)].reset(); }
  // After a confirmed zero-fill miss on this side (released already): with
  // SNIPER_MISS_RETRY=1, decide once more for the same signal, through every
  // normal gate, if it is younger than SNIPER_MISS_RETRY_MS and the latest
  // Coinbase mid still holds the trigger move. `missed`, when given, must be
  // the side's last order.
  void retry_after_miss(const std::string &side, const Plan *missed = nullptr);
  // The paper outcomes that are a zero-fill miss of the order's own price.
  static bool retryable_outcome(const std::string &outcome);
  // SNIPER_REST_LIVE: the price-to-beat view the decisions use.
  Json direction_view(const std::string &side, double end) {
    return ptb_view(side, end);
  }
  // Trigger signals seen for each side (UP, DOWN), whatever became of them.
  std::uint64_t signals_by_side[2]{0, 0};
  // _set_bankroll(_read_bankroll(), read_at)
  void refresh_bankroll(double read_at);
  // _refresh_hybrid_risk: worker-only accounting refresh
  void refresh_hybrid_risk();
  std::optional<double> bankroll_now();
  // (ms, parts) of sniper_mode.latency_plan
  std::pair<double, Json> latency_plan();
  // _window: (first, last) seconds left at which an order can still fill
  std::pair<double, double> window();
  void seed_rounds(std::map<std::string, std::pair<double, long long>> base) {
    round_baseline = std::move(base);
  }
  void seed_minimum_entries(const std::vector<Json> &journal_rows);
  // hybrid factors from the cached snapshot (raises py::ValueError)
  hybrid::Factors hybrid_factors(Json *out = nullptr);
  void check_hybrid_order(Plan &, const py::Dec &spend, double fee_rate,
                          const py::Dec &quoted = py::D(0));
  static int idx(const std::string &side) { return side == "UP" ? 0 : 1; }

private:
  std::optional<double> bankroll_;
  double bankroll_at_ = -1e18;
  struct Spend {
    double t, amount;
    long long id;
  };
  std::vector<Spend> spends_;
  std::map<long long, Spend> pending_spends_;
  long long spend_seq_ = 0, spend_ids_ = 0;
  std::vector<std::string> skip_order_;
  std::map<std::string, double> skip_logged_;
  std::vector<std::shared_ptr<Plan>> dispatched_;
  std::optional<double> t1_mid_;
  std::shared_ptr<Plan> last_plan_[2]; // each side's last order sent
  std::optional<Json> retry_fields_;   // joined to a re-try's decision row
  int retry_next_ = 0;

  void signal(double mid, double t_ms, std::int64_t t_ns);
  // The rule on one feed's mid, then the gates every feed shares: the side's
  // cooldown and one order in flight, so a move seen by two feeds the same
  // way is one order - fired by whichever saw it first. The primary's
  // counters keep their names; another feed's are suffixed with its name.
  void signal_on(const std::string &feed, Trigger &, Freshness &, double mid,
                 double t_ms, std::int64_t t_ns);
  const Trigger &trigger_of(const std::string &feed) const;
  std::string fire_feed_; // the feed whose signal fire_now decides
  std::shared_ptr<Plan> fire_now(const std::string &side, double r_bp,
                                 double t_ms, std::int64_t t_ns);
  std::shared_ptr<Plan> decide(const std::string &side, Json &row,
                               double t_ms);
  std::nullptr_t skip(const std::string &why, Json &row);
  Json window_fields(const Trigger &, double t_ms) const;
  double side_held(double end, const std::string &side,
                   const std::string &token);
  std::pair<double, double> kelly_inventory(const std::string &cond,
                                            double end, const std::string &side,
                                            const std::string &token,
                                            const std::string &opposite);
  std::optional<std::pair<double, int>>
  fair_fee_parameters(const std::string &cond, const std::string &up,
                      const std::string &down);
  long long share_order_floor(const std::string &cond, const py::Dec &cap);
  PxLevels visible_levels(const std::string &token, const BookView &view);
  std::optional<double> visible_ask(const std::string &token,
                                    const BookView &view);
  std::pair<std::optional<double>, std::string>
  fair_anchor(const std::string &token, const BookView &view, Json &row,
              double t_ms);
  void set_bankroll(std::optional<double> v, double read_at);
  void bankroll_spent(double amount, Plan *plan);
  void bankroll_completed(Plan &plan);
  void hybrid_drawdown_pause(Json &snapshot);
  void write(Json row) { env.journal(std::move(row)); }
  void note_primary_mid(double mid, double wall, double mono);
  Json ptb_view(const std::string &side, double end);
  void decision_book(Json &row, const Plan &plan);
  struct PtbRound {
    std::optional<double> est, raw;
    int basis_rounds = 0;
    std::string why;
  };
  std::deque<std::pair<double, double>> ptb_hist_;
  std::optional<double> ptb_start_;
  std::map<double, PtbRound> ptb_rounds_, ptb_pending_;
};

double ioc_spend(const PxLevels &levels, double target, double ask, double cap);
// The reference's id field: f"{time.time():.3f}-{side}"
std::string decision_id(double wall, const std::string &side);
// sniper_mode._exchange_ms for an RFC 3339 string.
std::optional<double> iso_ms(const Json &v);
// sniper_mode._outcome
std::string paper_outcome(bool filled, const std::string &reason,
                          const Json &row, bool history = true);
} // namespace sniper
