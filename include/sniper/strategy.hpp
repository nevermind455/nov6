#pragma once
// Live canary budgets (sniper_live.CanaryLimits). The decision engine itself
// is sniper.hpp (a port of sniper_mode.Sniper); the paper fill model is
// paper.hpp (a port of paper_trade.py).
#include "config.hpp"
#include "storage.hpp"
namespace sniper {
// An order the venue confirmed matched nothing gives its spend back.
inline void refund_missed_spend(CanaryState &state, Units stake) {
  state.spend = state.spend > stake ? state.spend - stake : 0;
}
std::string canary_refusal(const Config &, CanaryState &, Units shares,
                           Units stake, Units loss, std::string_view condition,
                           double wall);
} // namespace sniper
