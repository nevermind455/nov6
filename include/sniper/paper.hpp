#pragma once
// Port of paper_trade.py (nevermind455/oct10@772f2eb): the Decimal FOK/FAK
// fill walk, the decision-book pre-send check, the modelled arrival book
// (Sniper._arrival_book), own-fill netting and the booking checks. The
// reference runs one blocking worker thread per order; here an order is a
// state machine advanced by the event loop: begin() at dispatch, then
// poll() until its arrival book is known.
#include "sniper.hpp"
#include <functional>

namespace sniper {
struct PaperRejected : std::runtime_error {
  using std::runtime_error::runtime_error;
};

using DecLevels = std::vector<std::pair<py::Dec, py::Dec>>;
struct BookSnapshot {
  std::string token_id;
  DecLevels asks, bids;
  std::optional<py::Dec> min_order_size, tick_size, best_bid;
  std::optional<std::string> timestamp, book_hash;
  double received_wall = 0;
};
// snapshot_from_book_view; throws PaperRejected
BookSnapshot snapshot_from_book_view(const BookView &,
                                     const std::optional<std::string> &expected,
                                     double now_mono, double venue_wall_now);

struct FillQuote {
  py::Dec notional, shares, average_price, worst_price, fee;
  std::vector<std::tuple<py::Dec, py::Dec, py::Dec>> levels;
  py::Dec total_cost() const { return notional + fee; }
};
py::Dec curve_fee(const py::Dec &shares, const py::Dec &price,
                  const MarketRules &);
FillQuote estimate_fok(const BookSnapshot &, const py::Dec &amount,
                       const py::Dec &max_price, const MarketRules &,
                       const py::Dec &min_price,
                       const std::optional<py::Dec> &min_shares = {});
FillQuote estimate_fak(const BookSnapshot &, const py::Dec &amount,
                       const py::Dec &max_price, const MarketRules &,
                       const py::Dec &min_price,
                       const std::optional<py::Dec> &min_shares = {});
FillQuote estimate_fak_shares(const BookSnapshot &, const py::Dec &shares,
                              const py::Dec &max_price, const MarketRules &,
                              const py::Dec &min_price,
                              const std::optional<py::Dec> &min_shares = {});

// sniper_live.LiveExecution.prepare: the pre-send check of a share-sized FAK
// on the book its decision used (lean_view), with the same functions paper
// uses. Throws PaperRejected with the reference's texts; the caller prefixes
// "pre-send check: ". order_tick/order_minimum are the order sender's rules.
struct LiveOrderCheck {
  py::Dec cap, price, size, spend;
  std::optional<py::Dec> min_override;
  FillQuote quote;
};
LiveOrderCheck live_presend_check(const Plan &, const MarketRules &rules,
                                  const std::optional<py::Dec> &order_tick,
                                  const std::optional<py::Dec> &order_minimum,
                                  double max_buy_price, double min_buy_price,
                                  double max_spread, double now_mono,
                                  double venue_wall_now);

struct PaperConfig {
  double max_buy_price = 0.95, min_buy_price = 0.05, max_book_age_s = 8,
         max_quiet_s = 900, future_tol_s = 5, max_spread = 0.25,
         min_seconds_to_expiry = 10, trade_window_seconds = 300,
         own_fill_ttl_s = 2;
};

struct PaperHooks {
  std::function<double()> mono, unix_wall, venue_wall;
  // (condition, up, down) of the current round
  std::function<std::tuple<std::string, std::string, std::string>()> context;
  std::function<const MarketRules *(const std::string &)> rules;
  std::function<double()> cash_balance;
  // book the fill durably; returns the order id; throws to stop the engine
  std::function<std::string(const Plan &, const FillQuote &)> record_fill;
  std::function<void(Json)> audit;
};

class PaperBroker {
public:
  PaperBroker(PaperConfig, PaperHooks);
  PaperConfig cfg;

  bool has_recent_own_fill(const std::string &token);
  BookSnapshot net_of_own_fills(const BookSnapshot &);
  std::optional<PxLevels> net_asks(const BookView &view);

  // Start one sniper order (place_trade up to its modelled delay). A
  // refusal before the flight completes the order at once.
  void begin(const std::shared_ptr<Plan> &, const SniperSettings &,
             BookState &);
  // Advance pending orders; each finished one is handed to `done`.
  void poll(BookState &, const SniperSettings &,
            const std::function<void(const std::shared_ptr<Plan> &,
                                     const PaperResult &)> &done);
  std::size_t pending() const { return orders_.size(); }
  // the earliest monotonic time poll() has work to do
  std::optional<double> next_wake() const;

private:
  PaperHooks h_;
  struct Order {
    std::shared_ptr<Plan> plan;
    std::string side, token, cond, up, down;
    double end = 0, cutoff = 0, wake_mono = 0, arrival_ms = 0,
           arrival_mono = 0, wait_started = -1, latency_ms = 0;
    py::Dec spend, cap, floor;
    std::optional<py::Dec> size, min_override;
    MarketRules rules;
    bool fak = true;
    PaperResult result;
    bool finished = false, owns_slot = false;
  };
  std::vector<Order> orders_;
  std::map<std::string, std::vector<std::tuple<double, py::Dec, py::Dec>>>
      own_;
  std::set<std::string> busy_; // one sniper order in flight per token
  std::pair<double, double> check_book(const BookSnapshot &,
                                       const std::string &side,
                                       const std::optional<std::string> &token,
                                       bool require_asks = true);
  FillQuote quote(const BookSnapshot &, const Order &);
  void reject(Order &, const std::string &why);
  void arrive(Order &, BookState &, const SniperSettings &);
};
} // namespace sniper
