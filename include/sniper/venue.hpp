#pragma once
#include "book.hpp"
#include "network.hpp"
#include "storage.hpp"
#include <deque>
#include <istream>
#include <set>
namespace sniper {
struct ServerClock {
  std::deque<double> offsets, rtts;
  double at = -1, server_at = 0, offset = 0;
  bool sample(double server, double sent, double received, double rtt,
              double mono);
  std::int64_t now(double mono) const;
  double median_rtt() const;
};
Credentials derive_credentials(const RestClient &, const Signer &);
std::map<std::string, Units> resolve_payouts(const RestClient &, const Round &,
                                             std::string_view rpc,
                                             bool onchain);
struct ParsedTrade {
  std::string id, order, token, condition, status;
  Units shares = 0, cost = 0, fee = 0;
};
ParsedTrade parse_trade(const Json &, const std::map<std::string, Intent> &);
// Trade ids the owner has marked as their own manual trades, one per line;
// blank lines and text after '#' are ignored.
std::set<std::string> read_manual_trades(std::istream &);
// A FAK the venue has no record of can only still fill during its brief taker
// hold, so a minute after sending with nothing matched it did not fill.
inline bool missing_fak_is_unfilled(const Intent &i, double wall,
                                    double grace_s = 60) {
  return i.matched_shares == 0 && i.booked_shares == 0 && i.sent_wall > 0 &&
         wall - i.sent_wall >= grace_s;
}
// A trade on this wallet whose taker order this bot never placed.
bool is_foreign_trade(const Json &, const std::map<std::string, Intent> &);
} // namespace sniper
