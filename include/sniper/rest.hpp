#pragma once
#include "storage.hpp"
#include <optional>
// SNIPER_REST_LIVE: after a FAK that matched nothing, the same BUY - its shares
// at its limit - rests on the book as a post-only order for a few seconds and
// is then cancelled. This file is the part with no I/O: when an order may rest,
// when a resting one is cancelled, what the venue's replies mean, and the
// record of orders that are not proven off the book yet. The runtime does the
// sending.
namespace sniper::rest {

// The FAK the venue just missed.
struct Missed {
  std::string side;              // UP or DOWN
  double end = 0;                // its round's end, unix seconds
  Units limit = 0, shares = 0;   // its signed price limit and whole shares
  double signal_ms = 0;          // monotonic ms of its trigger signal
  std::optional<double> ref_mid; // the trigger price before the move
};

// What is known at the moment of deciding.
struct Now {
  double wall = 0, mono_ms = 0;
  std::optional<double> mid;     // latest trigger price
  bool feed_fresh = false;       // the trigger feed is connected and current
  std::optional<Units> best_ask; // the token's best ask, on a live fresh book
  Units min_shares = 0;          // the venue's published minimum order size
  Json direction;                // Sniper::direction_view for the side
};

struct Policy {
  double rest_s = 5;         // SNIPER_REST_S
  bool direction = true;     // SNIPER_REST_DIR
  double end_margin_s = 10;  // off the book this long before the round ends
  double max_signal_age_ms = 2000;
  double min_rest_s = 1;
};

// How much of the trigger move is still there, in bp of the signal's reference
// price; positive is the order's way. Empty when either price is unknown.
std::optional<double> move_bp(const std::string &side,
                              std::optional<double> ref_mid,
                              std::optional<double> mid);
// Empty when the side beats the price to beat by more than the estimate's own
// error, on an estimate checked against enough published rounds; else why not.
std::string direction_refusal(const Json &view);
// Seconds an order placed at `wall` may rest: SNIPER_REST_S, less whatever the
// round's end takes off.
double lifetime_s(double end, double wall, const Policy &);
// Why the missed order is not rested; empty = rest it.
std::string refusal(const Missed &, const Now &, const Policy &);

// The venue's answer to placing the post-only order.
enum class Posted {
  Resting, // on the book
  Crossed, // refused: it would have taken an offer (nothing rests, no trade)
  Refused, // refused for another reason
  Matched, // traded at once (a post-only order should not)
  Accepted, // taken, but neither resting nor matched (e.g. "delayed"): it
            // may reach the book, so it is cancelled and followed
  Unknown, // anything else: assume it may be on the book
};
struct PostReply {
  Posted kind = Posted::Unknown;
  std::string detail, venue_status;
  Units making = 0, taking = 0;
};
PostReply parse_post_reply(int http_status, std::string_view body,
                           std::string_view order_id);

// The venue's answer to cancelling it.
enum class Cancelled {
  Yes,    // the venue took it off the book
  NotOpen, // the venue has no open order to cancel: read its record
  Failed, // no answer that says either
};
struct CancelReply {
  Cancelled kind = Cancelled::Failed;
  std::string detail;
};
CancelReply parse_cancel_reply(int http_status, std::string_view body,
                               std::string_view order_id);
// A venue order record's status shows the order can match no more.
bool record_terminal(std::string_view status);

// One order that may be on the book.
struct Order {
  std::string id, side, token, condition;
  double end = 0;
  std::optional<double> ref_mid;
  double placed_mono = 0;   // seconds
  double deadline_mono = 0; // cancel no later than this
  bool posted = false;      // the venue answered its placement
  bool wanted = false;      // a cancel is wanted
  std::string why;          // ... for this reason (the first one given)
  double wanted_at = 0;     // ... since this monotonic time
  int cancels = 0;          // cancel requests sent
  bool in_flight = false;
  double flight_since = 0, next_cancel = 0;
  bool warned = false;      // the operator was told it is not proven off
  bool off_book = false;    // proven off the book; only its final size is due
  double proven_at = 0;     // ... since this monotonic time
  Json row;                 // its rest_order journal row, written once
  std::uint64_t signals_other = 0; // opposite-side signals seen when placed
  std::string feed = "coinbase";   // the trigger feed of its signal
};

// Why a resting order is cancelled now; empty = it keeps resting. `hold` is a
// reason of the bot's own (shutdown, a pause, the kill switch) and wins.
std::string cancel_reason(const Order &, const Now &, const Policy &,
                          bool opposite_signal, const std::string &hold = {});

// The resting orders being followed. While one is not proven off the book its
// market takes no other order of ours; after that it stays here only until
// the venue gives its final size.
class Book {
  std::map<std::string, Order> orders_;

public:
  static constexpr int QuickTries = 3;         // then a slow retry
  static constexpr double Backoff = 0.25;      // seconds x the attempt number
  static constexpr double SlowRetry = 5.0;     // seconds
  static constexpr double FlightTimeout = 2.0; // a cancel unanswered this long
  static constexpr double GiveUpAfterEnd = 120.0;
  // A placement normally answers in tens of milliseconds. Past this a wanted
  // cancel no longer waits for the answer: the order may be on the book.
  static constexpr double PostGrace = 1.0;
  // A cancel wanted this long with the order still not shown gone is
  // reported, whatever the reason (no connection, no answer, refusals).
  static constexpr double StuckAfter = 5.0;

  bool empty() const { return orders_.empty(); }
  std::size_t size() const { return orders_.size(); }
  // An order may still be on the book: anywhere, or in this market.
  bool busy() const;
  bool in_market(std::string_view condition) const;
  Order *find(std::string_view id);
  const std::map<std::string, Order> &orders() const { return orders_; }
  // A resting order was sent, or one from an earlier run was found.
  void add(Order);
  // The venue answered the placement: the order is (or may be) on the book.
  // A cancel already wanted goes out at once: any sent before this could
  // not have found the order.
  void posted(std::string_view id);
  // The venue shows it is off the book (a confirmed cancel); its final size
  // is still to come.
  void proven(std::string_view id, double mono = 0);
  // The venue still lists an order it said it cancelled: it is cancelled
  // again. False when it is not here or was not proven off.
  bool reopen(std::string_view id);
  // It never rested, or its final size is known: nothing more to follow.
  bool off(std::string_view id);
  // Cancel it; the first reason (and its time) is kept. False when it is not
  // here.
  bool cancel(std::string_view id, const std::string &why, double mono = 0);
  // Orders to send a cancel for now; each is marked in flight.
  std::vector<std::string> due(double mono);
  // A cancel request ended without showing the order off the book.
  void cancel_unproven(std::string_view id, double mono);
  // Orders the operator has not been told about yet that are still not
  // proven off: after QuickTries answered cancels, or StuckAfter seconds
  // from when the cancel was first wanted.
  std::vector<std::string> unproven_to_report(double mono = 0);
  // Orders whose market closed GiveUpAfterEnd ago: nothing of them can match
  // any more. They are dropped from the book and returned (off_book says
  // whether the venue ever showed the order gone).
  std::vector<Order> give_up(double wall);
};

// Whether one more resting order of this stake keeps the test inside its loss
// stop even if every resting fill still unsettled, and this order, lost all
// they paid. `realized` is the settled result since the test began.
bool fits_stop(Units realized, Units open_cost, Units stake, Units stop);

// The test's own record, kept beside the ledger so the loss stop survives a
// restart. Delete the file (with the bot stopped) to begin a new test.
struct Test {
  double started_wall = 0, off_wall = 0;
  bool off = false;
  Units baseline = 0; // the ledger's rest_realized when the test began
  Units realized = 0; // ... and its change since, at the last look
  std::string reason;
  Json json() const;
  static Test load(const Json &); // throws on a malformed record
  // Looks at the resting orders' settled result; true while the test is on.
  bool look(Units rest_realized, Units stop, double wall);
};
} // namespace sniper::rest
