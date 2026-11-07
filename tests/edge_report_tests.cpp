// Tests for tools/report/edge_report.hpp on made-up journal rows.
#include "edge_report.hpp"
#include <gtest/gtest.h>

using namespace sniper::edge;

namespace {
void paper_round(Collector &c, int i, bool won, double fair = 0.7, double price = 0.6,
                 const std::string &suffix = "") {
  double shares = 10, fee = 0.1;
  std::string cond = "c" + std::to_string(i), oid = "paper-" + std::to_string(i) + suffix;
  c.add({{"result", "filled"}, {"order_id", oid}, {"condition_id", cond}, {"side", "UP"},
         {"fair", fair}, {"shares", shares}, {"cost", shares * price + fee}, {"fee", fee},
         {"wall", 1000.0 + i}});
  c.add({{"result", "settlement"}, {"condition_id", cond},
         {"payouts", {{cond + "-up", won ? 1000000 : 0}, {cond + "-down", won ? 0 : 1000000}}}});
  c.add_audit({{"order_id", oid}, {"token_id", cond + "-up"}});
}
Report sample(int n, double win_rate, double fair = 0.7, double price = 0.6, int min_rounds = DEFAULT_MIN_ROUNDS) {
  std::mt19937_64 rng(1);
  std::uniform_real_distribution<double> u(0, 1);
  Collector c;
  for (int i = 0; i < n; ++i)
    paper_round(c, i, u(rng) < win_rate, fair, price);
  return c.report(min_rounds);
}
} // namespace

TEST(EdgeReport, PaperWinAndLossCountFeesOnce) {
  Collector c;
  paper_round(c, 0, true);
  paper_round(c, 1, false);
  auto r = c.report(300);
  EXPECT_NEAR(r.net, -2.2, 1e-9); // win 10 - 6.1, loss -6.1
  EXPECT_NEAR(r.fees, 0.2, 1e-9);
}

TEST(EdgeReport, LiveAmountAddsTheFeeAndRepeatedTradesCountOnce) {
  Collector c;
  c.add_ledger({{"orders", Json::array({{{"id", "o"}, {"token", "tok"}}})}});
  c.add({{"result", "pending"}, {"order_id", "o"}, {"condition_id", "c"}, {"fair", 0.7}, {"wall", 5.0}});
  Json fill{{"result", "live_fill"}, {"order_id", "o"}, {"trade_id", "t"},
            {"shares", 10}, {"amount", 6.0}, {"fee", 0.1}};
  c.add(fill);
  c.add(fill);
  c.add({{"result", "settlement"}, {"condition_id", "c"}, {"payouts", {{"tok", 1000000}}}});
  EXPECT_NEAR(c.report(300).net, 3.9, 1e-9);
}

TEST(EdgeReport, SentButUnfilledLiveOrderIsNotATrade) {
  Collector c;
  c.add({{"result", "pending"}, {"order_id", "o"}, {"condition_id", "c"}});
  c.add({{"result", "settlement"}, {"condition_id", "c"}, {"payouts", {{"tok", 1000000}}}});
  EXPECT_EQ(c.report(300).fills_settled, 0);
}

TEST(EdgeReport, UnsettledAndUnmatchedAreCountedNotGuessed) {
  Collector c;
  Json fill{{"result", "filled"}, {"side", "UP"}, {"fair", 0.7}, {"shares", 5}, {"cost", 3.0}, {"fee", 0}};
  fill["order_id"] = "a";
  fill["condition_id"] = "open";
  c.add(fill);
  fill["order_id"] = "b";
  fill["condition_id"] = "done";
  c.add(fill);
  c.add({{"result", "settlement"}, {"condition_id", "done"}, {"payouts", {{"x", 1000000}}}});
  auto r = c.report(300);
  EXPECT_EQ(r.fills_settled, 0);
  EXPECT_EQ(r.unsettled, 1);
  EXPECT_EQ(r.untraceable, 1);
}

TEST(EdgeReport, SplitResolutionPaysHalfAndIsNotScored) {
  Collector c;
  c.add({{"result", "filled"}, {"order_id", "a"}, {"condition_id", "c"}, {"fair", 0.7},
         {"shares", 10}, {"cost", 6.0}, {"fee", 0}});
  c.add_audit({{"order_id", "a"}, {"token_id", "t"}});
  c.add({{"result", "settlement"}, {"condition_id", "c"}, {"payouts", {{"t", 500000}}}});
  auto r = c.report(300);
  EXPECT_NEAR(r.net, -1.0, 1e-9);
  EXPECT_EQ(r.voided, 1);
  EXPECT_EQ(r.scored, 0);
}

TEST(EdgeReport, Verdicts) {
  EXPECT_EQ(Collector().report(300).verdict, "NO DATA");
  EXPECT_EQ(sample(50, 0.8).verdict, "TOO EARLY");
  EXPECT_EQ(sample(800, 0.7).verdict, "EDGE LIKELY");
  EXPECT_EQ(sample(800, 0.4).verdict, "NO EDGE");
  // pays 0.5, wins 62%, claims 80%: profit today, but Kelly oversizes
  EXPECT_EQ(sample(800, 0.62, 0.8, 0.5).verdict, "OVERCONFIDENT");
  // claims what it wins, and pays about that too
  EXPECT_EQ(sample(800, 0.61, 0.61, 0.6).verdict, "UNPROVEN");
}

TEST(EdgeReport, FillsInOneRoundAreNotIndependent) {
  std::mt19937_64 rng(3);
  std::uniform_real_distribution<double> u(0, 1);
  Collector one, many;
  for (int i = 0; i < 40; ++i) {
    bool won = u(rng) < 0.6;
    paper_round(one, i, won);
    for (int k = 0; k < 20; ++k)
      paper_round(many, i, won, 0.7, 0.6, "-" + std::to_string(k));
  }
  auto width = [](const Report &r) { return r.return_ci->second - r.return_ci->first; };
  EXPECT_GT(width(many.report(300)), width(one.report(300)) * 0.8);
}

TEST(EdgeReport, WilsonInterval) {
  auto [low, high] = wilson(50, 100);
  EXPECT_NEAR(low, 0.4038, 1e-3);
  EXPECT_NEAR(high, 0.5962, 1e-3);
}

TEST(EdgeReport, TopBinIncludesCertaintyAndRenders) {
  Collector c;
  paper_round(c, 0, true, 1.0);
  auto r = c.report(300);
  ASSERT_EQ(r.bins.size(), 1u);
  EXPECT_EQ(r.bins[0].label, "90%-100%");
  EXPECT_NE(render(r, "x").find("VERDICT: TOO EARLY"), std::string::npos);
}
