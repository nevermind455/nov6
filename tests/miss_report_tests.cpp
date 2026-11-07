#include "miss_report.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <sstream>

using namespace sniper::miss;

namespace {
Json run(const std::string &lines) {
  Report r;
  std::istringstream in(lines);
  read_lines(in, [&](const Json &j) { r.add(j); });
  return r.json();
}
std::string source(const char *path) {
  std::ifstream in(std::string(SOURCE_DIR) + "/" + path);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}
} // namespace

// Every skip and dispatch-gate reason the engine can write must have a code,
// so a new reason cannot hide in "unclassified".
TEST(MissReport, EveryEngineReasonHasACode) {
  std::set<std::string> reasons;
  std::smatch m;
  for (const char *file : {"src/sniper.cpp", "src/runtime.cpp", "src/paper.cpp"}) {
    auto text = source(file);
    ASSERT_FALSE(text.empty()) << file;
    for (const auto *pattern : {R"re(\bskip\("([^"]+)")re", R"re(\brefuse\("([^"]+)")re",
                                R"re(gate = "([^"]+)")re", R"re(\breturn "([a-zA-Z][^"]{8,})";)re",
                                R"re(\breject\([a-z]+, "([^"]+)")re"}) {
      std::regex re(pattern);
      for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
           it != std::sregex_iterator(); ++it)
        reasons.insert((*it)[1]);
    }
  }
  ASSERT_GT(reasons.size(), 40u);
  // Text that is not a skip or refusal reason (labels, field values).
  const std::set<std::string> not_reasons{"tick", "before move", "history unavailable",
                                          "heartbeat", "malformed", "subscribed"};
  for (auto &why : reasons) {
    if (not_reasons.count(why))
      continue;
    if (std::all_of(why.begin(), why.end(), [](char c) { return std::isupper(c) || c == '_'; })) {
      // a paper outcome code; TECHNICAL_ERROR defers to the reason text
      EXPECT_TRUE(why == "TECHNICAL_ERROR" || classify_paper_outcome(why)) << why;
      continue;
    }
    EXPECT_NE(classify_reason(why).kind, Kind::Unclassified) << why;
  }
  for (auto o : {"PRICE_CAP_MISS", "LIVE_ODDS_MISS", "FOK_MISS_AT_WAKE", "MISS_AT_DECISION",
                 "INSUFFICIENT_LIQUIDITY", "SPREAD_TOO_WIDE", "BELOW_VENUE_MINIMUM",
                 "STALE_BOOK", "UNKNOWN_BOOK_STATE"})
    EXPECT_TRUE(classify_paper_outcome(o)) << o;
}

TEST(MissReport, FiltersAreNotBugsAndDataGapsAreMissedOpportunities) {
  EXPECT_EQ(classify_reason("ask not below fair value").kind, Kind::StrategyFilter);
  EXPECT_EQ(classify_reason("kelly size under the 10-share minimum").kind, Kind::RiskControl);
  EXPECT_EQ(classify_reason("book stale").kind, Kind::DataNotReady);
  EXPECT_EQ(classify_reason("fair value unknown: volatility warming up").code, "DATA_VOL_WARMUP");
  EXPECT_EQ(classify_reason("fair value unknown: no move").kind, Kind::StrategyFilter);
  EXPECT_EQ(classify_reason("trigger signal became stale before dispatch").kind, Kind::LateOrStale);
  EXPECT_EQ(classify_reason("decision book became stale before dispatch").code, "LATE_BOOK_STALE");
  EXPECT_EQ(classify_reason("fault in native discovery").kind, Kind::SoftwareFailure);
  EXPECT_EQ(classify_reason("something new").kind, Kind::Unclassified);
}

// Skip totals are the bot's exact counters, summed over restarts; journal
// rows are rate-limited and only supply evidence.
TEST(MissReport, SkipTotalsComeFromCountersAcrossRuns) {
  auto out = run(
      R"({"result":"start","wall":1})"
      "\n"
      R"({"result":"skipped","reason":"ask not below fair value","wall":2,"skip_count":1})"
      "\n"
      R"({"result":"summary","signals":10,"counters":{"skip: ask not below fair value":7,"skip: book stale":2}})"
      "\n"
      R"({"result":"start","wall":3})"
      "\n"
      R"({"result":"summary","signals":5,"counters":{"skip: ask not below fair value":3}})"
      "\n");
  EXPECT_EQ(out["runs"], 2);
  EXPECT_EQ(out["signals"], 15.0);
  EXPECT_EQ(out["by_kind"]["strategy_filter"]["codes"]["FILTER_ASK_NOT_BELOW_FAIR"]["count"], 10);
  EXPECT_EQ(out["by_kind"]["data_not_ready"]["codes"]["DATA_BOOK_STALE"]["count"], 2);
  EXPECT_EQ(out["by_kind"]["strategy_filter"]["verdict"], "correct filter, not a bug");
  EXPECT_EQ(out["evidence"]["FILTER_ASK_NOT_BELOW_FAIR"].size(), 1u);
}

TEST(MissReport, LiveRefusalsAreClassifiedByTheirDetail) {
  auto out = run(
      R"({"result":"skipped","reason":"live refused","detail":"private channel not ready for this market","wall":5})"
      "\n"
      R"({"result":"skipped","reason":"live halted","detail":"STOP_LIVE present","wall":6})"
      "\n");
  EXPECT_EQ(out["by_kind"]["data_not_ready"]["codes"]["DATA_PRIVATE_CHANNEL"]["count"], 1);
  EXPECT_EQ(out["by_kind"]["risk_control"]["codes"]["RISK_STOP_FILE"]["count"], 1);
}

// The order lifecycle: dispatch row, acknowledged reply with stage stamps,
// confirmed fills; partial, FAK miss, venue refusal and unknown outcome.
TEST(MissReport, LiveOrdersAreFollowedFromSendToFill) {
  std::string j;
  auto send = [&](const char *id, double wall) {
    j += std::string(R"({"result":"pending","order_id":")") + id +
         R"(","side":"UP","order_shares":10,"dec_quote_age_ms":120,"wall":)" +
         std::to_string(wall) + R"(,"T0_ns":1000000,"T2_ns":1060000})" "\n";
  };
  auto stamps = std::string(R"("T0_ns":1000000,"T2_ns":1060000,"T3_ns":1090000,"T4_ns":1500000,"T5_ns":1600000,"T6_ns":31600000)");
  send("A", 100);
  j += R"({"result":"pending","order_id":"A","reason":"awaiting FAK fill size","acknowledged":true,)" + stamps + "}\n";
  j += R"({"result":"live_fill","order_id":"A","shares":10,"status":"CONFIRMED","wall":100.4})" "\n";
  send("B", 200);
  j += R"({"result":"live_fill_update","order_id":"B","shares":4,"confirmed":false,"acknowledged":true,)" + stamps + "}\n";
  j += R"({"result":"live_fill","order_id":"B","shares":4,"status":"CONFIRMED","wall":200.5})" "\n";
  send("C", 300);
  j += R"({"result":"refused","order_id":"C","reason":"FAK no match","http_status":400,"acknowledged":true,)" + stamps + "}\n";
  send("D", 400);
  j += R"({"result":"refused","order_id":"D","reason":"venue refused native order","http_status":400,"venue_reply":"{\"error\":\"invalid tick\"}","acknowledged":true,)" + stamps + "}\n";
  send("E", 500);
  j += R"({"result":"pending","order_id":"E","reason":"stream reset","never_retry":true,"acknowledged":false})" "\n";
  auto out = run(j);
  auto &o = out["orders"];
  EXPECT_EQ(o["submitted"], 5);
  EXPECT_EQ(o["acknowledged"], 4);
  EXPECT_EQ(o["filled"], 1);
  EXPECT_EQ(o["partially_filled"], 1);
  EXPECT_EQ(o["unfilled"], 1);
  EXPECT_EQ(o["rejected"], 1);
  EXPECT_EQ(o["unknown_outcome"], 1);
  EXPECT_EQ(out["by_kind"]["no_liquidity"]["codes"]["LIQ_FAK_NO_MATCH"]["count"], 1);
  EXPECT_EQ(out["by_kind"]["venue_rejected"]["codes"]["VENUE_REFUSED"]["count"], 1);
  EXPECT_EQ(out["by_kind"]["unknown_outcome"]["codes"]["UNKNOWN_ORDER_OUTCOME"]["count"], 1);
  EXPECT_EQ(out["evidence"]["VENUE_REFUSED"][0]["venue_reply"], "{\"error\":\"invalid tick\"}");
  auto &lat = out["latency_ms"];
  EXPECT_EQ(lat["signing"]["n"], 4);
  EXPECT_DOUBLE_EQ(lat["signing"]["p50"].get<double>(), 0.03);
  EXPECT_DOUBLE_EQ(lat["network_and_ack"]["p99"].get<double>(), 30.0);
  EXPECT_DOUBLE_EQ(lat["trigger_to_ack"]["p50"].get<double>(), 30.6);
  EXPECT_EQ(lat["send_to_first_fill_ms"]["n"], 2);
  EXPECT_EQ(lat["book_age_at_decision"]["n"], 5);
}

TEST(MissReport, PaperOutcomesAndPartialFills) {
  auto out = run(
      R"({"result":"filled","outcome":"FILLED","order_id":"paper-1","shares":15.2,"order_shares":15,"latency_ms":400,"wall":1})"
      "\n"
      R"({"result":"filled","outcome":"FILLED","shares":6,"order_shares":15,"wall":2})"
      "\n"
      R"({"result":"refused","outcome":"PRICE_CAP_MISS","reason":"FAK no-fill: nothing at or below 0.3","wall":3})"
      "\n"
      R"({"result":"refused","outcome":"UNKNOWN_BOOK_STATE","reason":"unknown book state at the modelled arrival: the feed has not yet passed that time","wall":4})"
      "\n");
  EXPECT_EQ(out["orders"]["paper_orders"]["filled"], 1);
  EXPECT_EQ(out["orders"]["paper_orders"]["partially_filled"], 1);
  EXPECT_EQ(out["orders"]["paper_orders"]["refused"], 2);
  EXPECT_EQ(out["orders"]["submitted"], 0); // paper orders are never live orders
  EXPECT_EQ(out["by_kind"]["price_moved"]["codes"]["PAPER_PRICE_CAP_MISS"]["count"], 1);
  EXPECT_EQ(out["by_kind"]["paper_model"]["codes"]["PAPER_ARRIVAL_UNKNOWN"]["count"], 1);
  EXPECT_EQ(out["latency_ms"]["paper_modelled_latency"]["n"], 1);
  // TECHNICAL_ERROR is the broker's catch-all: the reason text decides.
  auto cut = run(R"({"result":"refused","outcome":"TECHNICAL_ERROR","reason":"paper latency reached the round cutoff","wall":5})" "\n");
  EXPECT_EQ(cut["by_kind"]["paper_model"]["codes"]["PAPER_CUTOFF"]["count"], 1);
}

TEST(MissReport, DisruptionsBeforeAMissAreSuspectedNotConfirmed) {
  Report r;
  r.add_record(Json{{"k", "mconn"}, {"up", false}, {"t", 5.0}, {"w", 100.0}});
  // non-boolean fields (other event kinds use "up" for other things) are ignored
  r.add_record(Json{{"k", "rest_req"}, {"up", "gamma"}, {"w", 105.0}});
  r.add_record(Json{{"k", "mconn"}, {"up", "false"}, {"w", 105.0}});
  // monotonic-only events cannot be placed on the journal's wall clock
  r.add_record(Json{{"k", "cbconn"}, {"up", false}, {"t", 195.0}});
  r.add(Json{{"result", "skipped"}, {"reason", "book not live"}, {"wall", 110.0}});
  r.add(Json{{"result", "skipped"}, {"reason", "book stale"}, {"wall", 200.0}});
  r.add(Json{{"result", "skipped"}, {"reason", "ask not below fair value"}, {"wall", 111.0}});
  auto out = r.json();
  EXPECT_EQ(out["suspected_causes"]["missed_with_disruption_within_30s"], 1);
  EXPECT_EQ(out["suspected_causes"]["missed_without_disruption"], 1);
}

TEST(MissReport, MalformedLinesAreCounted) {
  Report r;
  std::istringstream in("{\"result\":\"start\"}\nnot json\n\n[1,2]\n");
  auto bad = read_lines(in, [&](const Json &j) { r.add(j); });
  EXPECT_EQ(bad, 1u);
  EXPECT_EQ(r.json()["malformed_rows"], 1);
}
