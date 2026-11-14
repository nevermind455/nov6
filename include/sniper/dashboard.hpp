#pragma once
// Port of dashboard/ (nevermind455/oct10@772f2eb): the terminal dashboard as
// one pure function from a snapshot to a frame, plus a renderer that runs on
// its own thread.
//
//     build(snap, cols, rows) -> exactly `rows` rows, each `cols` cells wide
//
// The runtime builds `snap` from plain values copied on the event loop, so
// nothing here touches the bot and the layout can be tested without a terminal.
// Every key is optional: a missing value is drawn as "--", never invented.
//
//     mode        "PAPER" | "LIVE"
//     clock       "HH:MM:SS ET"
//     trigger     feed, status, mid, move_bp, k_bp, window_ms, last_signal_bp,
//                 last_signal_age_s, signals, signals_per_min, orders, filled,
//                 refused, fill_rate, order_type, slip, send_p50_ms, ack_p50_ms,
//                 ack_simulated, errors, in_flight, fair {vol_bp, why, last}
//     exec        latency {metric: {n, last, avg, p50, p95, p99, max, simulated}},
//                 quality {attempted, sent, acked, pending, filled, partial,
//                 zero_fill, rejected, failed, fill_rate, reject_rate, last,
//                 avg_slip_c, slip_n}, paper
//     market      slug, seconds_left, trade_until_s, up / down ({bid, ask,
//                 status, age_ms}), round_fills, max_fills, round_up_sh,
//                 round_down_sh, max_side_shares
//     orders      [{time, side, bp, ask, cap, result, shares, price, detail}],
//                 newest last
//     account     kind, cash, equity, pnl, realized, fees, wins, losses, balance,
//                 balance_status, position_value, unrealized, total, redeemable
//     positions   rows [{side, shares, avg, cost, bid, value, pnl}], pending
//                 {markets, shares, cost}
//     settlement  active, subject, pending [...], previous [...], last_settled,
//                 pending_count; each market {label, state, age_s, winner,
//                 settlement_price, realized, settlement_s, has_position,
//                 redeem, detail, condition_id, window_end}
//     canary      null in paper; halted [reasons], orders/max_orders,
//                 fills/max_fills, spend/max_spend, rounds/max_rounds,
//                 loss/max_loss, guard, guard_reason, awaiting
//     net         coinbase, clob (ms)
//     health      [[name, state, detail]]
//     skips       [[reason, count]], most frequent first
//     events      [[wall, source, text, level]], newest last
//
// Left out because this build has no such feature: the POSITION MANAGER,
// POLYMARKET BTC, LATENCY BUDGET and P/L BY SOURCE panels, markouts, and the
// Binance round trip on the NET line.
#include <memory>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace sniper {
using Json = nlohmann::ordered_json;
struct Intent;
struct Position;
struct BookView;
}

namespace sniper::dash {
// Pure evidence adapters. Confirmed ledger bookings drive live fills; HTTP
// acknowledgements and decision asks are independent of the bounded order table.
// Resting orders (SNIPER_REST_LIVE) are not FAK orders and are left out.
// An order filled in part: fewer shares booked than it asked for. One that got
// every share below its limit spent less than its stake and is filled in full.
bool partial_fill(const Intent &);
Json live_execution_quality(const std::map<std::string, Intent> &orders,
                            const std::set<std::string> &acknowledged,
                            const std::map<std::string, double> &decision_asks,
                            double started_wall,
                            const std::string &last_fill_order = {});
Json execution_latencies(const Json &row);
// Settled rounds in the ledger, all time: won (the round's positions made
// money together), lost, even; unknown counts rounds with a settled position
// whose payout is not on record.
struct RoundResults {
  long long won = 0, lost = 0, even = 0, unknown = 0;
};
RoundResults settled_rounds(const std::map<std::string, Position> &positions);
// A settled position that paid something: worth its payout until redeemed.
bool redeem_candidate(const Position &);
// Settled winnings not redeemed yet, in dollars: each winning position's
// payout on the shares the wallet still holds (`held`: token -> micro-shares
// read on chain). Empty when a winning position's balance is not known.
std::optional<double> unredeemed_value(const std::map<std::string, Position> &positions,
                                       const std::map<std::string, std::int64_t> &held);
Json mark_position(double shares, double notional, double fees,
                    const BookView &, double now_mono, double max_age_ms);
Json live_balance_view(std::optional<double> balance, double age_s,
                        double max_age_s = 120);

enum class Color : unsigned char {
  cream, paper2, ink, dim, faint, rule, green, green2, red, red2, amber, blue,
  purple, cyan, white, count_
};
struct Style {
  Color fg = Color::ink;
  Color bg = Color::cream;
  bool bold = false;
  bool operator==(const Style &) const = default;
};
struct Run {
  std::string text;
  Style style;
};
using Row = std::vector<Run>;

// ---------------------------------------------------------------- safety
// Text that crosses the terminal boundary is untrusted: control sequences can
// forge status rows, and error strings can echo credentials.
std::string redact(std::string_view value, std::size_t max_chars = 4096);
// Redacted, then reduced to printable ASCII, which also keeps every character
// exactly one terminal cell wide.
std::string terminal_text(std::string_view value, std::size_t max_chars = 4096);

// ---------------------------------------------------------------- widgets
std::size_t cells(std::string_view utf8);
std::size_t width(const Row &);
std::string fit(std::string_view text, int n, char align = '<');
Row pad(Row row, int cols, Style style = {});
std::vector<Row> panel(const std::string &title, const std::vector<Row> &body,
                       int cols, int rows, const std::string &right_note = "");
std::vector<int> hsplit(int total, const std::vector<double> &weights,
                        const std::vector<int> &mins);
std::vector<Row> join(const std::vector<std::vector<Row>> &parts,
                      const std::vector<int> &widths, int rows);
Row bar(double frac, int width, Style fill);

// ---------------------------------------------------------------- clock
// US Eastern time, which is how the venue names its rounds. Computed from the
// US daylight-saving rule, so it needs no time zone files on the server.
int et_offset_s(double unix_wall);
std::string et_clock(double unix_wall); // "HH:MM:SS ET"
std::string et_time(double unix_wall);  // "HH:MM:SS"
std::string et_label(double start, double end); // "14:00-14:05"

// ---------------------------------------------------------------- layout
// Never throws: a layout fault is drawn, not thrown.
std::vector<Row> build(const Json &snap, int cols, int rows);
// One status line for a non-terminal --dash (a log file, systemd).
std::string plain_line(const Json &snap);

// ---------------------------------------------------------------- output
enum class ColorMode { truecolor, xterm256, mono };
std::string sgr(Style style, ColorMode mode);
std::string render_row(const Row &row, ColorMode mode);

// The terminal side. The newest snapshot is handed to a separate thread, which
// builds and writes the frame, so a slow terminal can never delay the event
// loop: while it is busy, a newer snapshot simply replaces the waiting one.
class Dashboard {
public:
  explicit Dashboard(int fd);
  ~Dashboard();
  Dashboard(const Dashboard &) = delete;
  Dashboard &operator=(const Dashboard &) = delete;
  void submit(Json snapshot);
  // Restores the terminal. Safe to call more than once.
  void stop();

private:
  struct State;
  std::shared_ptr<State> s_;
  std::thread thread_;
};
} // namespace sniper::dash
