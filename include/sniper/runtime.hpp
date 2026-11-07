#pragma once
#include "config.hpp"
#include <memory>
namespace sniper {
class Ledger;
namespace execution {
// A REST balance can precede fills already confirmed on the event loop.
Units spendable_collateral(Units sampled_balance, Units sampled_outflow,
                           Units current_outflow);
std::set<std::string> account_markets(const Ledger &,
                                     std::set<std::string> active);
bool pending_trade_status(std::string_view);
bool refresh_round_rules(Round &current, const Round &fresh);
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
