#pragma once
// SNIPER_EXTRA_FEEDS: the trigger feeds beside Coinbase, and their messages.
//
//   binance_fut   Binance USD-M perpetual BTCUSDT book ticker. Each message
//                 carries the exchange's own event time (E).
//   binance_spot  Binance spot BTCUSDT book ticker, which carries no exchange
//                 time, with the spot trades on the same socket for that (the
//                 reference bot's combined stream).
//
// Only a feed's MOVE is used, on its own rule: Binance quotes in USDT, and the
// perpetual sits at its own level (the basis), so its price is never compared
// with Coinbase's.
#include "types.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace sniper::feeds {
bool known(std::string_view feed);
// The websocket address of a feed (throws for an unknown one).
std::string url(std::string_view feed);

struct Tick {
  enum Kind { Book, Trade, Other, Malformed } kind = Other;
  std::optional<double> mid;      // Book: (best bid + best ask) / 2
  std::optional<double> event_ms; // the exchange's time of the event, if sent
};
// One message of a feed. A subscription answer or another stream is Other; a
// message that names BTCUSDT but has no usable price is Malformed.
Tick parse(std::string_view feed, std::string_view raw);
} // namespace sniper::feeds
