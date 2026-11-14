#pragma once
// Port of feeds/book.py, the market-event dispatch of feeds/poly_market.py and
// sniper_mode.BinanceFreshness (nevermind455/oct10@772f2eb). Prices and sizes
// are Python floats (doubles) here, exactly as the reference holds them.
#include "types.hpp"
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace sniper {
using PxLevels = std::vector<std::pair<double, double>>;
using LevelMap = std::map<double, double>;

inline constexpr const char *LIVE = "LIVE";
inline constexpr const char *STALE = "STALE";
inline constexpr const char *UNSYNCED = "UNSYNCED";
inline constexpr const char *DISCONNECTED = "DISCONNECTED";

struct BookView {
  std::string token;
  std::uint64_t generation = 0;
  std::string status = DISCONNECTED;
  PxLevels bids, asks; // bids best (highest) first, asks best (lowest) first
  std::optional<double> updated_mono;
  std::optional<long long> exchange_ts_ms;
  std::optional<double> tick_size;
  std::optional<std::string> hash;
  long long updates = 0, receive_revision = 0;
  std::string source = "ws";
  std::optional<double> best_bid() const {
    return bids.empty() ? std::nullopt : std::optional(bids.front().first);
  }
  std::optional<double> best_ask() const {
    return asks.empty() ? std::nullopt : std::optional(asks.front().first);
  }
  std::optional<double> mid() const {
    if (bids.empty() || asks.empty())
      return std::nullopt;
    return (asks.front().first + bids.front().first) / 2.0;
  }
};

struct ViewAtInfo {
  std::optional<long long> hwm_ms;
  std::string why, evidence;
  std::optional<long long> book_ts;
  std::optional<double> applied_wall;
};

struct SnapshotOptions {
  Json ts_ms;                       // raw timestamp value (any JSON)
  std::optional<std::string> hash;
  std::optional<double> tick;
  bool only_if_unsynced = false;
  std::optional<std::uint64_t> expected_generation;
  std::optional<long long> expected_updates, expected_receive_revision;
  std::optional<double> refresh_if_older_than;
  bool require_exchange_ts = false;
  std::string source = "ws";
  std::optional<double> received_mono;
};

// Python int(v) for a JSON value as feeds/book._int does; nullopt on failure.
std::optional<long long> py_int(const Json &v);
// Python float(v) for a JSON value; nullopt on TypeError/ValueError or bool.
std::optional<double> py_float(const Json &v);
// timer.parse_exchange_ts: unix seconds of a timestamp in s/ms/us/ns.
std::optional<double> parse_exchange_ts(double raw);

class BookState {
public:
  using Clock = std::function<double()>;
  BookState(double stale_after = 8.0, double history_s = 30.0,
            Clock mono = nullptr, Clock wall = nullptr,
            Clock venue_wall = nullptr);

  bool connected = false;
  double stale_after, history_s, future_tolerance = 5.0, max_quiet = 900.0;
  std::optional<long long> ts_hwm;

  std::pair<std::vector<std::string>, std::vector<std::string>>
  set_active(const std::vector<std::string> &tokens);
  std::vector<std::string> active() const;
  bool is_active(const std::string &t) const { return active_.count(t) > 0; }
  void desync_all(const std::string &reason = "reconnect");
  std::optional<std::uint64_t> subscription_gap(const std::string &token,
                                                std::uint64_t expected);
  bool apply_snapshot(const std::string &token, const Json &bids,
                      const Json &asks, const SnapshotOptions &o);
  bool apply_price_changes(const Json &changes, const Json &ts_ms,
                           const Json &hash, bool require_exchange_ts = true);
  bool set_tick_size(const std::string &token, const Json &tick,
                     const Json &ts_ms, bool require_exchange_ts);
  void note_exchange_ts(const Json &ts);

  BookView view(const std::string &token) const;
  std::pair<std::optional<BookView>, ViewAtInfo>
  view_at(const std::string &token, double venue_ms,
          bool allow_quiet = false) const;
  std::optional<BookView> view_at_mono(const std::string &token,
                                       double mono) const;
  std::string status(const std::string &token) const;
  std::vector<std::string> needs_resync(bool include_stale = false) const;
  bool fresh_exchange_ts(std::optional<long long> ts) const;
  std::size_t history_entries() const;
  double mono() const { return mono_(); }
  double wall() const { return wall_(); }

private:
  struct Meta {
    bool synced = false;
    std::optional<double> updated;
    std::optional<long long> ts, seen_ts, tick_ts;
    std::optional<double> tick;
    std::optional<std::string> hash;
    long long updates = 0, receive_revision = 0;
    std::string source = "ws", desync_reason;
  };
  struct Entry {
    long long key = 0;
    double mono = 0, wall = 0;
    std::optional<long long> ts;
    std::shared_ptr<const LevelMap> bids, asks; // null for a gap
    std::optional<double> tick;
    std::string source;
    std::optional<double> receipt;
  };
  Clock mono_, wall_, venue_wall_;
  std::map<std::string, std::shared_ptr<const LevelMap>> bids_, asks_;
  std::map<std::string, Meta> meta_;
  std::set<std::string> active_;
  std::map<std::string, std::uint64_t> generation_;
  std::map<std::string, std::deque<Entry>> hist_;
  void note_ts(std::optional<long long> ts);
  void record(const std::string &token, std::optional<long long> ts,
              bool gap = false, std::optional<double> mono = {},
              std::optional<double> wall = {});
  BookView entry_view(const std::string &token, const Entry &e) const;
  void desync(const std::vector<std::string> &tokens,
              const std::string &reason);
  void observe(const std::vector<std::string> &tokens,
               std::optional<long long> ts = {});
  std::string status_locked(const std::string &token) const;
};

// poly_market._dispatch for one market-socket event. Returns true when the
// event was refused and the reference would request a book resync.
bool dispatch_market_event(BookState &, const Json &event);
// poly_market._handle: one raw frame (a JSON object or a list of them).
// Returns the number of refused events that request a resync.
int handle_market_frame(BookState &, std::string_view raw);

// sniper_mode.BinanceFreshness, created with name "coinbase".
struct Freshness {
  double stale_ms = 1000, unknown_ms = 5000, baseline_s = 300;
  int backlog_max = 20;
  std::string name = "coinbase";
  bool enabled = false, connected = false;
  std::deque<std::pair<double, double>> min_q; // (mono, lag), increasing lag
  std::optional<double> last_trade_mono, last_msg_mono, last_excess;
  int backlog = 0, backlog_max_seen = 0;
  std::string mode = "count";
  double fresh_ms = 100;
  bool fresh_burst() const {
    return backlog > backlog_max && last_excess && *last_excess <= fresh_ms;
  }
  void on_connect() {
    connected = true;
    last_trade_mono.reset();
    last_excess.reset();
  }
  void on_disconnect() { connected = false; }
  void note_message(double mono, std::optional<int> b) {
    last_msg_mono = mono;
    if (b) {
      backlog = *b;
      backlog_max_seen = std::max(backlog_max_seen, *b);
    }
  }
  void note_trade(std::optional<double> event_ms, double recv_wall_ms,
                  double mono);
  // empty string: the signal may be acted on
  std::string why_not(double now_mono) const;
};

std::optional<double> exchange_ms(const Json &value);
std::optional<Round> parse_round(const Json &event, std::int64_t start);
Rules parse_rules(const Json &market, const Json &book,
                  std::string_view token_id);
} // namespace sniper
