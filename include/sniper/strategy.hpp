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
// A canary budget's limit as it applies now ("orders", "fills", "rounds",
// "spend", "loss"): its configured size, raised by that size after each pause
// it caused when SNIPER_CANARY_HARD_STOP=0.
Units canary_limit(const Config &, const CanaryState &, std::string_view budget);
// The dashboard's CANARY panel: each budget against the limit that applies
// now, and every reason new orders are held - `held` (the runtime's own
// reasons), then the canary's pause: a reached budget, or a fault's pause
// that outlived the process that raised it. `runtime_reason` is the runtime's
// running pause, empty when there is none.
Json canary_panel(const Config &, const CanaryState &, Units open_cost,
                  std::size_t awaiting, double wall,
                  std::vector<std::string> held,
                  const std::string &runtime_reason);
} // namespace sniper
