#pragma once
#include "config.hpp"
#include <memory>
namespace sniper {
class Ledger;
namespace execution {
// Local application observations in steady-clock nanoseconds. Missing stages
// remain absent; these stamps are not venue or hardware-wire timestamps.
Json order_timing_fields(const std::array<std::int64_t, 8> &);
Json trade_evidence_fields(const Json &, std::string_view source,
                           std::int64_t received_ns);
// A REST balance can precede fills already confirmed on the event loop.
Units spendable_collateral(Units sampled_balance, Units sampled_outflow,
                           Units current_outflow);
std::set<std::string> account_markets(const Ledger &,
                                     std::set<std::string> active);
bool pending_trade_status(std::string_view);
bool refresh_round_rules(Round &current, const Round &fresh);
// Whether the round after the one containing `now_wall` should be prepared:
// true anywhere in its last `lead` seconds, so a late tick cannot miss it.
bool prepare_due(double now_wall, double lead);
// Book tokens to keep subscribed: both outcomes of the current and the
// prepared round, and nothing else.
std::set<std::string> round_tokens(const std::optional<Round> &current,
                                   const std::optional<Round> &prepared);
// Start (unix seconds) of the five-minute round containing `now_wall`.
std::int64_t round_window(double now_wall);
// Whether the round state, last switched at `current_window`, is behind the
// clock: checked on every trigger message so no trigger after a boundary is
// decided against the previous round.
bool round_switch_due(double now_wall, std::int64_t current_window);
// Debounces a fault pause per reason: the same reason cannot re-pause while
// its own pause still runs, and pauses again once it has expired.
class PauseLatch {
public:
  bool admit(const std::string &why, double now) const;
  void hold(const std::string &why, double until);
  std::size_t size() const { return until_.size(); }

private:
  std::map<std::string, double> until_;
};
bool cache_round_rules(std::map<std::string, MarketRules> &, const Round &,
                       double fetched_wall);
} // namespace execution
class Runtime {
  struct Impl;
  std::unique_ptr<Impl> p_;

public:
  explicit Runtime(Config);
  ~Runtime();
  int run();
};
bool native_self_test();
int compare_decisions(const std::filesystem::path &,
                      const std::filesystem::path &);
} // namespace sniper
