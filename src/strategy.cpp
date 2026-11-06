#include "sniper/strategy.hpp"
namespace sniper {
std::string canary_refusal(const Config &c, CanaryState &state, Units shares,
                           Units stake, Units loss, std::string_view condition,
                           double wall) {
  if (shares > c.canary_shares)
    return "canary order shares";
  if (state.pause_until > wall)
    return "canary paused: " + state.pause_reason;
  if (state.pause_until > 0) {
    // The reference raises a reached budget by its original size after each
    // pause, so "$5" becomes $10, then $15. With the hard stop a budget stays
    // what was configured until a deliberate --new-canary-session.
    if (!c.canary_hard_stop)
      state.increments[state.pause_reason]++;
    state.pause_until = 0;
    state.pause_reason.clear();
  }
  auto limit = [&](std::string_view name, Units base) {
    return base * (1 + state.increments[std::string(name)]);
  };
  std::string why;
  if (state.orders + 1 > limit("orders", c.canary_orders))
    why = "orders";
  else if (state.fills + 1 > limit("fills", c.canary_fills))
    why = "fills";
  else if (state.rounds.size() +
               (state.rounds.count(std::string(condition)) ? 0 : 1) >
           static_cast<std::size_t>(limit("rounds", c.canary_rounds)))
    why = "rounds";
  else if (state.spend + stake > limit("spend", c.canary_spend))
    why = "spend";
  else if (loss > limit("loss", c.canary_loss))
    why = "loss";
  if (!why.empty()) {
    state.pause_reason = why;
    state.pause_until = wall + 300;
    return "canary budget: " + why;
  }
  return {};
}
} // namespace sniper
