// Tests for tools/report/edge_report.hpp on made-up journal rows.
#include "edge_report.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <sstream>

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

TEST(EdgeReport, CalibrationBinsResampleRoundsInsteadOfIndependentFills) {
  Collector one, many;
  for (int i = 0; i < 20; ++i) {
    paper_round(one, i, i % 2 == 0);
    for (int k = 0; k < 20; ++k)
      paper_round(many, i, i % 2 == 0, 0.7, 0.6, "-" + std::to_string(k));
  }
  auto a = one.report(300), b = many.report(300);
  ASSERT_EQ(a.bins.size(), 1u);
  ASSERT_EQ(b.bins.size(), 1u);
  EXPECT_TRUE(a.bins[0].has_interval);
  EXPECT_NEAR(a.bins[0].low, b.bins[0].low, 1e-12);
  EXPECT_NEAR(a.bins[0].high, b.bins[0].high, 1e-12);
  Collector single;
  for (int k = 0; k < 30; ++k)
    paper_round(single, 1, true, 0.7, 0.6, "-" + std::to_string(k));
  auto r = single.report(1);
  ASSERT_EQ(r.bins.size(), 1u);
  EXPECT_FALSE(r.bins[0].has_interval);
  EXPECT_NE(render(r, "x").find("needs 2+ rounds"), std::string::npos);
}

TEST(EdgeReport, ProfitWithoutScoredRoundsDoesNotClaimModelCalibration) {
  Collector c;
  for (int i = 0; i < 4; ++i) {
    auto id = "o" + std::to_string(i), condition = "c" + std::to_string(i);
    c.add({{"result", "filled"}, {"order_id", id}, {"condition_id", condition},
           {"shares", 10}, {"cost", 6}, {"fee", 0}, {"wall", 1000 + i}});
    c.add_audit({{"order_id", id}, {"token_id", id}});
    c.add({{"result", "settlement"}, {"condition_id", condition}, {"payouts", {{id, 1000000}}}});
  }
  auto r = c.report(2);
  EXPECT_GT(r.net, 0);
  EXPECT_FALSE(r.has_calibration);
  EXPECT_EQ(r.verdict, "UNPROVEN");
}

TEST(EdgeReport, InvalidSettlementAndConflictingTokenEvidenceCannotProduceProfit) {
  for (double payout : {-1.0, 1000001.0, std::numeric_limits<double>::infinity()}) {
    Collector c;
    paper_round(c, 1, true);
    c.add({{"result", "settlement"}, {"condition_id", "c1"},
           {"payouts", {{"c1-up", payout}, {"c1-down", 0}}}});
    auto r = c.report(1);
    EXPECT_EQ(r.fills_settled, 0);
    EXPECT_GT(r.invalid_rows, 0);
    EXPECT_EQ(r.verdict, "INCOMPLETE DATA");
  }
  Collector conflicting;
  paper_round(conflicting, 1, true);
  conflicting.add_ledger({{"orders", Json::array({{{"id", "paper-1"}, {"token", "c1-down"}}})}});
  auto r = conflicting.report(1);
  EXPECT_EQ(r.fills_settled, 0);
  EXPECT_EQ(r.verdict, "INCOMPLETE DATA");
}

TEST(EdgeReport, InvalidNumericEvidenceIsExcludedBeforeSortingAndScoring) {
  for (const auto &[field, value] : std::vector<std::pair<std::string, double>>{
           {"fair", 1.5}, {"fee", -1.0}, {"shares", -10.0},
           {"wall", std::numeric_limits<double>::quiet_NaN()},
           {"cost", std::numeric_limits<double>::infinity()}}) {
    Collector c;
    Json row{{"result", "filled"}, {"order_id", "o"}, {"condition_id", "c"},
             {"shares", 10}, {"cost", 6}, {"fee", .1}, {"fair", .7}, {"wall", 1000}};
    row[field] = value;
    c.add(row);
    auto r = c.report(1);
    EXPECT_EQ(r.fills_settled, 0) << field;
    EXPECT_GT(r.invalid_rows, 0) << field;
    EXPECT_EQ(r.verdict, "INCOMPLETE DATA") << field;
  }
}

TEST(EdgeReport, OrphanAndConflictingConfirmedTradesRemainVisibleAsIncompleteEvidence) {
  Collector c;
  c.add({{"result", "live_fill"}, {"order_id", "missing"}, {"trade_id", "t"},
         {"shares", 10}, {"amount", 6}, {"fee", .1}});
  auto r = c.report(1);
  EXPECT_EQ(r.untraceable, 1);
  EXPECT_EQ(r.verdict, "INCOMPLETE DATA");
  c.add({{"result", "live_fill"}, {"order_id", "missing"}, {"trade_id", "t"},
         {"shares", 100}, {"amount", 60}, {"fee", 1}});
  EXPECT_GT(c.report(1).invalid_rows, 0);
  Collector missing_id;
  missing_id.add({{"result", "live_fill"}, {"order_id", "o"}, {"trade_id", ""},
                  {"shares", 10}, {"amount", 6}, {"fee", .1}});
  EXPECT_GT(missing_id.report(1).invalid_rows, 0);
}

TEST(EdgeReport, RoundSettlementDoesNotCreateArtificialIntraroundDrawdown) {
  Collector c;
  c.add({{"result", "filled"}, {"order_id", "loser"}, {"condition_id", "c"},
         {"shares", 10}, {"cost", 5}, {"fee", 0}, {"fair", .5}, {"wall", 1000}});
  c.add({{"result", "filled"}, {"order_id", "winner"}, {"condition_id", "c"},
         {"shares", 10}, {"cost", 5}, {"fee", 0}, {"fair", .5}, {"wall", 1001}});
  c.add_audit({{"order_id", "loser"}, {"token_id", "down"}});
  c.add_audit({{"order_id", "winner"}, {"token_id", "up"}});
  c.add({{"result", "settlement"}, {"condition_id", "c"},
         {"payouts", {{"down", 0}, {"up", 1000000}}}});
  auto r = c.report(1);
  EXPECT_DOUBLE_EQ(r.net, 0);
  EXPECT_DOUBLE_EQ(r.max_drawdown, 0);
}

TEST(EdgeReport, MalformedCompanionRowsAndInputReadErrorsCannotGiveCleanEvidence) {
  std::istringstream input("{}\n{broken\n[]\n");
  EXPECT_EQ(read_json_lines(input, [](const Json &) {}), 2);
  std::istringstream unreadable;
  unreadable.setstate(std::ios::badbit);
  EXPECT_THROW(read_json_lines(unreadable, [](const Json &) {}), std::runtime_error);
  Collector c;
  paper_round(c, 1, true);
  EXPECT_EQ(c.report(1, 1).verdict, "INCOMPLETE DATA");
}

TEST(EdgeReport, MinimumRoundArgumentRequiresACompletePositiveInteger) {
  EXPECT_EQ(parse_min_rounds("300"), 300);
  for (const auto *invalid : {"0", "-1", "1x", "", "9999999999999999999"})
    EXPECT_THROW(parse_min_rounds(invalid), std::invalid_argument) << invalid;
}

TEST(EdgeReport, MetadataConditionMustMatchTheJournalDecision) {
  for (bool ledger_metadata : {false, true}) {
    Collector c;
    if (ledger_metadata)
      c.add_ledger({{"orders", Json::array({{{"id", "o"}, {"token", "tok"},
                                            {"condition", "LOSS"}}})}});
    else
      c.add_audit({{"order_id", "o"}, {"token_id", "tok"}, {"condition_id", "LOSS"}});
    c.add({{"result", "filled"}, {"order_id", "o"}, {"condition_id", "WIN"},
           {"fair", 0.6}, {"shares", 10}, {"cost", 5}, {"fee", 0}, {"wall", 100}});
    c.add({{"result", "settlement"}, {"condition_id", "WIN"},
           {"payouts", {{"tok", 1000000}}}});
    auto report = c.report(1);
    EXPECT_GT(report.invalid_rows, 0);
    EXPECT_EQ(report.verdict, "INCOMPLETE DATA");
    EXPECT_FALSE(report.has_profit);
  }
}

TEST(EdgeReport, AnOutcomeTokenCannotSettleInTwoConditions) {
  Collector c;
  c.add({{"result", "filled"}, {"order_id", "o"}, {"condition_id", "WIN"},
         {"fair", 0.6}, {"shares", 10}, {"cost", 5}, {"fee", 0}, {"wall", 100}});
  c.add_audit({{"order_id", "o"}, {"token_id", "tok"}});
  c.add({{"result", "settlement"}, {"condition_id", "WIN"},
         {"payouts", {{"tok", 1000000}}}});
  c.add({{"result", "settlement"}, {"condition_id", "LOSS"},
         {"payouts", {{"tok", 0}}}});
  auto report = c.report(1);
  EXPECT_GT(report.invalid_rows, 0);
  EXPECT_EQ(report.verdict, "INCOMPLETE DATA");
  EXPECT_FALSE(report.has_profit);
}

TEST(EdgeReport, StatusOnlyPendingAcknowledgementsPreserveTheOriginalDecision) {
  Collector c;
  c.add_ledger({{"orders", Json::array({{{"id", "o"}, {"token", "tok"},
                                        {"condition", "c"}}})}});
  c.add({{"result", "pending"}, {"order_id", "o"}, {"condition_id", "c"},
         {"fair", 0.7}, {"wall", 100}});
  c.add({{"result", "pending"}, {"order_id", "o"}, {"acknowledged", false},
         {"reason", "unknown native order outcome"}, {"wall", 101}});
  c.add({{"result", "pending"}, {"order_id", "o"}, {"acknowledged", true},
         {"reason", "awaiting FAK fill size"}, {"T6_ns", 120}, {"wall", 102}});
  c.add({{"result", "live_fill"}, {"order_id", "o"}, {"trade_id", "t"},
         {"shares", 10}, {"amount", 6}, {"fee", 0.1}});
  c.add({{"result", "settlement"}, {"condition_id", "c"},
         {"payouts", {{"tok", 1000000}}}});
  auto report = c.report(300);
  EXPECT_EQ(report.invalid_rows, 0);
  EXPECT_EQ(report.fills_settled, 1);
  EXPECT_EQ(report.scored, 1);
  EXPECT_TRUE(report.has_profit);
  EXPECT_NEAR(report.net, 3.9, 1e-9);
  for (const auto &provided : {Json{{"condition_id", "other"}}, Json{{"fair", 0.2}}}) {
    auto conflicting = c;
    Json row{{"result", "pending"}, {"order_id", "o"}};
    row.update(provided);
    conflicting.add(row);
    EXPECT_GT(conflicting.report(300).invalid_rows, 0);
    EXPECT_EQ(conflicting.report(300).verdict, "INCOMPLETE DATA");
  }
}
