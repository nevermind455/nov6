#include "sniper/strategy.hpp"
#include "sniper/dashboard.hpp"
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
Units canary_limit(const Config &c, const CanaryState &state,
                   std::string_view budget) {
  const Units base = budget == "orders"   ? c.canary_orders
                     : budget == "fills"  ? c.canary_fills
                     : budget == "rounds" ? c.canary_rounds
                     : budget == "spend"  ? c.canary_spend
                     : budget == "loss"   ? c.canary_loss
                                          : 0;
  auto raised = state.increments.find(std::string(budget));
  return base * (1 + (raised == state.increments.end() ? 0 : raised->second));
}
Json canary_panel(const Config &c, const CanaryState &state, Units open_cost,
                  std::size_t awaiting, double wall,
                  std::vector<std::string> held,
                  const std::string &runtime_reason) {
  const auto total = static_cast<__int128>(state.settled_loss) + open_cost;
  const Units loss = total > std::numeric_limits<Units>::max()
                         ? std::numeric_limits<Units>::max()
                         : static_cast<Units>(total);
  auto money = [](Units v) { return fmt::format("${:.2f}", dollars(v)); };
  std::string reason; // why the canary itself holds new orders, if it does
  if (state.pause_until > wall) {
    const std::string &why = state.pause_reason;
    if (why.rfind("fault:", 0) == 0) {
      // The runtime's own fault pauses are written here too. One that is
      // still running in this process is listed already.
      if (why.substr(6) != runtime_reason)
        reason = "paused after a fault: " + why.substr(6);
    } else {
      auto used = why == "orders"   ? std::to_string(state.orders)
                  : why == "fills"  ? std::to_string(state.fills)
                  : why == "rounds" ? std::to_string(state.rounds.size())
                  : why == "spend"  ? money(state.spend)
                  : why == "loss"   ? money(loss)
                                    : std::string("--");
      auto limit = canary_limit(c, state, why);
      reason = "canary budget " + why + " reached (" + used + " of " +
               (why == "spend" || why == "loss" ? money(limit)
                                                : std::to_string(limit)) +
               ")";
    }
    if (!reason.empty())
      held.push_back(reason + ": no new orders until " +
                     dash::et_time(state.pause_until) +
                     " ET, then checked again");
  }
  const bool paused = !runtime_reason.empty() || !reason.empty();
  return Json{{"halted", held},
              {"orders", state.orders},
              {"max_orders", canary_limit(c, state, "orders")},
              {"fills", state.fills},
              {"max_fills", canary_limit(c, state, "fills")},
              {"spend", dollars(state.spend)},
              {"max_spend", dollars(canary_limit(c, state, "spend"))},
              {"rounds", state.rounds.size()},
              {"max_rounds", canary_limit(c, state, "rounds")},
              {"loss", dollars(loss)},
              {"max_loss", dollars(canary_limit(c, state, "loss"))},
              {"guard", paused ? "PAUSED" : "OK"},
              {"guard_reason", !runtime_reason.empty() ? runtime_reason : reason},
              {"awaiting", awaiting}};
}
} // namespace sniper
