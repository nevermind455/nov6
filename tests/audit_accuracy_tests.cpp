// Dashboard accuracy audit: each number the dashboard shows is the number its
// source holds, and what cannot be known is shown as unknown.
#include "sniper/crypto.hpp"
#include "sniper/dashboard.hpp"
#include "sniper/storage.hpp"
#include "sniper/strategy.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace sniper;

namespace {
struct Directory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("sniper-accuracy-" + std::to_string(getpid()) + "-" + std::to_string(mono_ns()));
  Directory() { std::filesystem::create_directories(path); }
  ~Directory() { std::filesystem::remove_all(path); }
};
std::size_t lines_in(const std::filesystem::path &p) {
  std::ifstream in(p);
  std::size_t n = 0;
  for (std::string line; std::getline(in, line);)
    ++n;
  return n;
}
std::string screen(const std::vector<dash::Row> &frame) {
  std::string out;
  for (auto &row : frame) {
    for (auto &run : row)
      out += run.text;
    out += '\n';
  }
  return out;
}
Intent bought(std::string id, std::string condition, std::string token, Units shares,
              Units cost) {
  Intent i;
  i.id = std::move(id);
  i.condition = std::move(condition);
  i.token = i.up = std::move(token);
  i.down = "other-" + i.token;
  i.status = "sent";
  i.target = shares;
  i.limit = U / 2;
  i.stake = multiply(shares, i.limit);
  i.reserve = i.stake;
  (void)cost;
  return i;
}
void book(Ledger &l, const Intent &i, Units shares, Units cost, Units fee) {
  l.register_intent(i, nullptr, l.canary);
  l.status(i.id, "matched", shares, cost);
  ASSERT_TRUE(l.fill(i.id + "-trade", i.id, shares, cost, fee));
}
Position position(std::string condition, Units shares, Units cost, Units fees,
                  Units payout, bool settled = true) {
  Position p;
  p.condition = std::move(condition);
  p.shares = shares;
  p.cost = cost;
  p.fees = fees;
  p.settled = settled;
  p.payout = payout;
  return p;
}
} // namespace

// A signal refused while a budget pause runs hands back the canary state it
// read. Each used to be written again: a durable row (the whole state, with
// every round of the session) synced to disk on the event loop, per signal.
TEST(AuditAccuracy, ARefusedSignalWritesNoCanaryRow) {
  Directory d;
  CanaryState kept;
  {
  Ledger l(d.path / "ledger.json", 0, true);
  Config c;
  c.canary_spend = 25 * U;
  CanaryState s;
  s.started = 100;
  s.spend = 24 * U;
  l.set_canary(s);
  const auto entries = l.entries;
  const auto rows = lines_in(d.path / "ledger.json.journal");
  // The first refusal reaches the budget and pauses: that is a change.
  auto state = l.canary;
  EXPECT_EQ(canary_refusal(c, state, 5, 2 * U, 0, "condition", 1000), "canary budget: spend");
  l.set_canary(state);
  EXPECT_EQ(l.entries, entries + 1);
  // Every later signal during the pause changes nothing and writes nothing.
  for (int n = 0; n < 200; ++n) {
    auto again = l.canary;
    EXPECT_EQ(canary_refusal(c, again, 5, 2 * U, 0, "condition", 1001 + n),
              "canary paused: spend");
    l.set_canary(again);
  }
  EXPECT_EQ(l.entries, entries + 1);
  EXPECT_EQ(lines_in(d.path / "ledger.json.journal"), rows + 1);
  // A real change is still written, and the state is what a restart reads.
  state = l.canary;
  state.pause_until = 0;
  l.set_canary(state);
  EXPECT_EQ(l.entries, entries + 2);
  kept = l.canary;
  }
  Ledger again(d.path / "ledger.json", 0, true);
  EXPECT_EQ(again.canary, kept);
}

TEST(AuditAccuracy, SettledPositionsKeepTheirPayoutAcrossRestarts) {
  Directory d;
  const auto path = d.path / "ledger.json";
  {
    Ledger l(path, 0, true);
    book(l, bought("up", "round-a", "111", 10 * U, 4 * U), 10 * U, 4 * U, 100000);
    book(l, bought("down", "round-a", "222", 5 * U, 2 * U), 5 * U, 2 * U, 50000);
    ASSERT_TRUE(l.settle("round-a", {{"111", U}, {"222", 0}}));
    EXPECT_EQ(l.positions.at("111").payout, U);
    EXPECT_EQ(l.positions.at("222").payout, 0);
    l.snapshot();
  }
  {
    Ledger l(path, 0, true);
    EXPECT_EQ(l.positions.at("111").payout, U);
    EXPECT_EQ(l.positions.at("222").payout, 0);
  }
  // A snapshot written before positions kept their payout: the settlement
  // event retained in the journal gives it back.
  {
    std::ifstream in(path);
    Json j;
    in >> j;
    for (auto &p : j.at("positions"))
      p.erase("payout");
    j.erase("sha256");
    j["sha256"] = sha256(j.dump());
    std::ofstream(path) << j.dump();
  }
  Ledger l(path, 0, true);
  EXPECT_EQ(l.positions.at("111").payout, U);
  EXPECT_EQ(l.positions.at("222").payout, 0);
  // The realized result is unchanged by any of this.
  EXPECT_EQ(l.realized, (10 * U - 4 * U - 100000) + (0 - 2 * U - 50000));
}

TEST(AuditAccuracy, TheCanaryPanelShowsABudgetPauseAndTheLimitsThatApply) {
  Config c;
  c.canary_spend = 25 * U;
  c.canary_loss = 100 * U;
  CanaryState s;
  s.started = 1;
  s.orders = 9;
  s.fills = 7;
  s.spend = 23500000;
  s.rounds = {"a", "b"};
  const double wall = 1794638700;
  // The budget check paused new orders: the panel says so, and why.
  auto state = s;
  ASSERT_EQ(canary_refusal(c, state, 5, 2500000, 0, "b", wall), "canary budget: spend");
  auto p = canary_panel(c, state, 3 * U, 0, wall + 10, {}, "");
  ASSERT_EQ(p["halted"].size(), 1u);
  const auto why = p["halted"][0].get<std::string>();
  EXPECT_NE(why.find("canary budget spend reached ($23.50 of $25.00)"), std::string::npos) << why;
  EXPECT_NE(why.find("no new orders until"), std::string::npos) << why;
  EXPECT_EQ(p["guard"], "PAUSED");
  EXPECT_DOUBLE_EQ(p["spend"].get<double>(), 23.5);
  EXPECT_DOUBLE_EQ(p["max_spend"].get<double>(), 25.0);
  // The header and the side panel show it as a halt.
  auto text = screen(dash::build(Json{{"mode", "LIVE"}, {"canary", p}}, 160, 40));
  EXPECT_NE(text.find(" HALTED "), std::string::npos);
  EXPECT_NE(text.find("canary budget spend"), std::string::npos);
  EXPECT_EQ(text.find("CANARY ARMED"), std::string::npos);
  // Once the pause is over nothing is held: the next order is checked again.
  p = canary_panel(c, state, 3 * U, 0, wall + 301, {}, "");
  EXPECT_TRUE(p["halted"].empty());
  EXPECT_EQ(p["guard"], "OK");
  // Without the hard stop a reached budget is raised by its own size; the
  // panel shows the limit that applies, not the configured one.
  state.increments["spend"] = 1;
  p = canary_panel(c, state, 0, 0, wall + 301, {}, "");
  EXPECT_DOUBLE_EQ(p["max_spend"].get<double>(), 50.0);
  EXPECT_EQ(p["max_orders"], c.canary_orders);
  EXPECT_EQ(canary_limit(c, state, "spend"), 50 * U);
  // Risk is the settled loss and everything still open, as the check reads it.
  state.settled_loss = 2 * U;
  p = canary_panel(c, state, 3 * U, 0, wall + 301, {}, "");
  EXPECT_DOUBLE_EQ(p["loss"].get<double>(), 5.0);
}

TEST(AuditAccuracy, AFaultPauseIsListedOnceAndAlsoAfterARestart) {
  Config c;
  CanaryState s;
  s.pause_reason = "fault:unknown native order outcome";
  s.pause_until = 2000;
  // Running in this process: the runtime lists it; the canary adds nothing.
  auto p = canary_panel(c, s, 0, 0, 1900, {"unknown native order outcome"},
                        "unknown native order outcome");
  ASSERT_EQ(p["halted"].size(), 1u);
  EXPECT_EQ(p["guard"], "PAUSED");
  // After a restart only the ledger remembers it, and it still holds orders.
  p = canary_panel(c, s, 0, 0, 1900, {}, "");
  ASSERT_EQ(p["halted"].size(), 1u);
  EXPECT_NE(p["halted"][0].get<std::string>().find("paused after a fault: unknown native order outcome"),
            std::string::npos);
  EXPECT_EQ(p["guard"], "PAUSED");
  EXPECT_FALSE(p["guard_reason"].get<std::string>().empty());
}

TEST(AuditAccuracy, FakQualityLeavesOutRestingOrdersAndBetterPricesAreFullFills) {
  std::map<std::string, Intent> orders;
  auto add = [&](std::string id, Units booked, Units cost, bool rest, std::string status) {
    Intent i;
    i.id = id;
    i.sent_wall = 101;
    i.status = std::move(status);
    i.target = 5 * U;
    i.limit = 200000;            // 0.20
    i.stake = 1 * U;             // 5 shares at the limit
    i.booked_shares = booked;
    i.booked_cost = cost;
    i.rest = rest;
    orders.emplace(id, i);
  };
  add("all-shares-cheaper", 5 * U, 900000, false, "confirmed"); // 5 @ 0.18
  add("three-of-five", 3 * U, 540000, false, "confirmed");      // 3 @ 0.18
  add("rested-unfilled", 0, 0, true, "no_match");
  add("rested-filled", 5 * U, 1 * U, true, "confirmed");
  const std::set<std::string> acked{"all-shares-cheaper", "three-of-five", "rested-unfilled",
                                    "rested-filled"};
  auto q = dash::live_execution_quality(orders, acked, {}, 100);
  EXPECT_EQ(q["attempted"], 2);
  EXPECT_EQ(q["sent"], 2);
  EXPECT_EQ(q["acked"], 2);
  EXPECT_EQ(q["filled"], 1);
  EXPECT_EQ(q["partial"], 1);
  EXPECT_EQ(q["zero_fill"], 0);
  EXPECT_DOUBLE_EQ(q["fill_rate"].get<double>(), 1.0);
  EXPECT_FALSE(dash::partial_fill(orders.at("all-shares-cheaper")));
  EXPECT_TRUE(dash::partial_fill(orders.at("three-of-five")));
  // Without a share target the stake decides, as before.
  Intent dollars_sized = orders.at("three-of-five");
  dollars_sized.target = 0;
  EXPECT_TRUE(dash::partial_fill(dollars_sized));
  dollars_sized.booked_cost = dollars_sized.stake;
  EXPECT_FALSE(dash::partial_fill(dollars_sized));
}

TEST(AuditAccuracy, WonAndLostCountWholeSettledRounds) {
  std::map<std::string, Position> positions{
      // round A: the UP side won more than the DOWN side lost
      {"a-up", position("A", 10 * U, 4 * U, 100000, U)},
      {"a-down", position("A", 5 * U, 2 * U, 50000, 0)},
      // round B: lost
      {"b-up", position("B", 5 * U, 3 * U, 0, 0)},
      // round C: a split paid back exactly the cost
      {"c-up", position("C", 4 * U, 2 * U, 0, U / 2)},
      // round D: still open
      {"d-up", position("D", 5 * U, 2 * U, 0, -1, false)}};
  auto r = dash::settled_rounds(positions);
  EXPECT_EQ(r.won, 1);
  EXPECT_EQ(r.lost, 1);
  EXPECT_EQ(r.even, 1);
  EXPECT_EQ(r.unknown, 0);
  positions["e-up"] = position("E", 5 * U, 2 * U, 0, -1); // settled, payout not on record
  r = dash::settled_rounds(positions);
  EXPECT_EQ(r.unknown, 1);
  EXPECT_EQ(r.won + r.lost + r.even, 3);
}

TEST(AuditAccuracy, UnredeemedWinningsAreReadFromTheWalletsTokens) {
  std::map<std::string, Position> positions{
      {"win", position("A", 10 * U, 4 * U, 0, U)},
      {"lose", position("A", 5 * U, 2 * U, 0, 0)},
      {"split", position("B", 4 * U, 2 * U, 0, U / 2)},
      {"open", position("C", 5 * U, 2 * U, 0, -1, false)}};
  EXPECT_TRUE(dash::redeem_candidate(positions.at("win")));
  EXPECT_FALSE(dash::redeem_candidate(positions.at("lose")));
  EXPECT_FALSE(dash::redeem_candidate(positions.at("open")));
  // Not read yet: unknown, never zero.
  EXPECT_FALSE(dash::unredeemed_value(positions, {}).has_value());
  EXPECT_FALSE(dash::unredeemed_value(positions, {{"win", 10 * U}}).has_value());
  // Read: each winner's payout on the shares still held.
  EXPECT_DOUBLE_EQ(dash::unredeemed_value(positions, {{"win", 10 * U}, {"split", 4 * U}}).value(), 12.0);
  // Redeemed in part, and the split redeemed.
  EXPECT_DOUBLE_EQ(dash::unredeemed_value(positions, {{"win", 6 * U}, {"split", 0}}).value(), 6.0);
  // Tokens of the same outcome bought by hand are not this bot's winnings.
  EXPECT_DOUBLE_EQ(dash::unredeemed_value(positions, {{"win", 25 * U}, {"split", 0}}).value(), 10.0);
  // No winning position at all: nothing to redeem, and that is known.
  positions.erase("win");
  positions.erase("split");
  EXPECT_DOUBLE_EQ(dash::unredeemed_value(positions, {}).value(), 0.0);
}

// The wallet token read calls ERC-1155 balanceOf(address,uint256).
TEST(AuditAccuracy, TheTokenBalanceCallUsesTheErc1155Selector) {
  auto h = keccak(std::string_view("balanceOf(address,uint256)"));
  EXPECT_EQ(hex(std::span<const unsigned char>(h.data(), 4)), "0x00fdd58e");
}

TEST(AuditAccuracy, TheHeaderNamesADryRunAndShowsUptime) {
  Json s{{"mode", "LIVE DRY RUN"}, {"clock", "01:02:03 ET"}, {"uptime_s", 3 * 3600 + 12 * 60 + 5}};
  auto frame = dash::build(s, 160, 12);
  auto text = screen(frame);
  EXPECT_NE(text.find(" LIVE DRY RUN "), std::string::npos);
  EXPECT_NE(text.find("up 3h12m  01:02:03 ET"), std::string::npos);
  // a dry run is not drawn as live money (white on red)
  for (auto &run : frame[0]) {
    if (run.text == " LIVE DRY RUN ") {
      EXPECT_NE(run.style.bg, dash::Color::red);
    }
  }
  s["uptime_s"] = 2 * 86400 + 3 * 3600;
  EXPECT_NE(screen(dash::build(s, 160, 12)).find("up 2d03h"), std::string::npos);
  s["uptime_s"] = 125;
  EXPECT_NE(screen(dash::build(s, 160, 12)).find("up 2m05s"), std::string::npos);
}

TEST(AuditAccuracy, TheRestingLineCountsThisRunsRestingOrders) {
  Json s{{"mode", "LIVE"},
         {"market", {{"slug", "btc-updown-5m-1"},
                     {"resting", {{"on", true}, {"rested", 3}, {"filled", 1}}}}}};
  EXPECT_NE(screen(dash::build(s, 160, 14)).find("no order on the book  3 rested 1 filled"),
            std::string::npos);
  s["market"]["resting"]["rested"] = 0;
  auto text = screen(dash::build(s, 160, 14));
  EXPECT_NE(text.find("no order on the book"), std::string::npos);
  EXPECT_EQ(text.find("rested"), std::string::npos);
}
