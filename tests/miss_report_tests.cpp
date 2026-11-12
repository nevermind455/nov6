#include "miss_report.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
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
Json dispatch_row(const char *id, std::int64_t base = 0) {
  return Json{{"result", "pending"}, {"order_id", id}, {"side", "UP"},
              {"order_shares", 10}, {"wall", 100.0},
              {"T0_ns", base + 1000000}, {"T1_ns", base + 1020000},
              {"T2_ns", base + 1060000}};
}
Json acknowledgement_row(const char *id, std::int64_t base = 0) {
  auto row = dispatch_row(id, base);
  row["acknowledged"] = true;
  row["T3_ns"] = base + 1090000;
  row["T4_ns"] = base + 1500000;
  row["T5_ns"] = base + 1600000;
  row["T6_ns"] = base + 31600000;
  return row;
}
Json confirmation_row(const char *id, Json receipt = 21600000,
                      const char *source_name = "websocket", double shares = 2,
                      const char *status = "CONFIRMED") {
  return Json{{"result", "live_fill"}, {"order_id", id}, {"shares", shares},
              {"status", status}, {"wall", 100.4},
              {"evidence_received_ns", std::move(receipt)},
              {"evidence_source", source_name}};
}
std::size_t samples(const Json &report, const char *metric) {
  const auto &latency = report.at("latency_ms");
  auto found = latency.find(metric);
  return found == latency.end() ? 0 : found->value("n", std::size_t(0));
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

// Every venue or trigger socket that comes up or goes down is journalled. A
// live socket going down is a disruption; a standby going is not, because
// the feed never stopped.
TEST(MissReport, SocketDropsAreCountedAndOnlyALostFeedIsADisruption) {
  Report r;
  r.add(Json{{"result", "ws"}, {"socket", "market"}, {"n", 1}, {"up", true}, {"role", "live"}, {"wall", 50.0}});
  r.add(Json{{"result", "ws"}, {"socket", "market"}, {"n", 2}, {"up", true}, {"role", "standby"}, {"wall", 50.1}});
  // the standby drops: no data was lost
  r.add(Json{{"result", "ws"}, {"socket", "market"}, {"n", 2}, {"up", false}, {"role", "standby"},
             {"reason", "closed by the venue"}, {"wall", 90.0}});
  r.add(Json{{"result", "skipped"}, {"reason", "book not live"}, {"wall", 95.0}});
  // the live one drops and the standby takes over
  r.add(Json{{"result", "ws"}, {"socket", "market"}, {"n", 1}, {"up", false},
             {"role", "live: the standby took over"}, {"reason", "silent for 25 s"},
             {"left_s", 6.8}, {"wall", 300.0}});
  r.add(Json{{"result", "skipped"}, {"reason", "book not live"}, {"wall", 301.0}});
  r.add(Json{{"result", "ws"}, {"socket", "user"}, {"n", 1}, {"up", false}, {"role", "private"},
             {"reason", "read: stream truncated"}, {"wall", 400.0}});
  auto out = r.json();
  EXPECT_EQ(out["sockets"]["events"]["market up"], 2);
  EXPECT_EQ(out["sockets"]["events"]["market standby down"], 1);
  EXPECT_EQ(out["sockets"]["events"]["market down"], 1);
  EXPECT_EQ(out["sockets"]["events"]["user down"], 1);
  EXPECT_EQ(out["sockets"]["down_reasons"]["market: silent for 25 s"], 1);
  EXPECT_EQ(out["sockets"]["down_reasons"]["user: read: stream truncated"], 1);
  EXPECT_FALSE(out["sockets"]["down_reasons"].contains("market: closed by the venue"));
  // the miss after the standby's drop has no disruption near it; the one
  // after the live socket's drop has
  EXPECT_EQ(out["suspected_causes"]["missed_with_disruption_within_30s"], 1);
  EXPECT_EQ(out["suspected_causes"]["missed_without_disruption"], 1);
  // socket rows are not orders or decisions
  EXPECT_EQ(out["orders"]["submitted"], 0);
  // a journal without socket rows has no such section
  EXPECT_FALSE(run(R"({"result":"start","wall":1})" "\n").contains("sockets"));
}

TEST(MissReport, MalformedLinesAreCounted) {
  Report r;
  std::istringstream in("{\"result\":\"start\"}\nnot json\n\n[1,2]\n");
  auto bad = read_lines(in, [&](const Json &j) { r.add(j); });
  EXPECT_EQ(bad, 1u);
  EXPECT_EQ(r.json()["malformed_rows"], 1);
}

TEST(MissReport, DispatchEntrySeparatesSignalSchedulingFromPresendChecks) {
  Report current;
  current.add(dispatch_row("new"));
  current.add(acknowledgement_row("new"));
  auto now = current.json();
  const auto &lat = now["latency_ms"];
  EXPECT_DOUBLE_EQ(lat["trigger_to_dispatch"]["p50"].get<double>(), 0.02);
  EXPECT_DOUBLE_EQ(lat["pre_send_checks"]["p50"].get<double>(), 0.04);
  EXPECT_DOUBLE_EQ(lat["signing"]["p50"].get<double>(), 0.03);
  EXPECT_DOUBLE_EQ(lat["durable_intent"]["p50"].get<double>(), 0.41);
  EXPECT_DOUBLE_EQ(lat["request_write"]["p50"].get<double>(), 0.1);
  EXPECT_DOUBLE_EQ(lat["network_and_ack"]["p50"].get<double>(), 30.0);
  EXPECT_DOUBLE_EQ(lat["trigger_to_ack"]["p50"].get<double>(), 30.6);
  EXPECT_EQ(now["latency_evidence"]["legacy_trigger_to_dispatch_includes_checks"], 0);
  EXPECT_EQ(now["latency_evidence"]["pre_send_checks_missing_boundary"], 0);

  Report legacy;
  auto send = dispatch_row("old"), ack = acknowledgement_row("old");
  send.erase("T1_ns");
  ack.erase("T1_ns");
  legacy.add(send);
  legacy.add(ack);
  auto old = legacy.json();
  EXPECT_DOUBLE_EQ(old["latency_ms"]["trigger_to_dispatch"]["p50"].get<double>(), 0.06);
  EXPECT_EQ(samples(old, "pre_send_checks"), 0u);
  EXPECT_EQ(old["latency_evidence"]["legacy_trigger_to_dispatch_includes_checks"], 1);
  EXPECT_EQ(old["latency_evidence"]["pre_send_checks_missing_boundary"], 1);
}

TEST(MissReport, InvalidOrReversedDispatchBoundariesDoNotInventCheckLatency) {
  for (const auto &invalid : std::vector<Json>{nullptr, 0, -1, true, 1020000.5,
           std::numeric_limits<std::uint64_t>::max(), 1e100}) {
    Report report;
    report.add(dispatch_row("A"));
    auto ack = acknowledgement_row("A");
    ack["T1_ns"] = invalid;
    report.add(ack);
    auto out = report.json();
    EXPECT_EQ(samples(out, "trigger_to_dispatch"), 0u) << invalid;
    EXPECT_EQ(samples(out, "pre_send_checks"), 0u) << invalid;
    EXPECT_EQ(out["latency_evidence"]["legacy_trigger_to_dispatch_includes_checks"], 0);
    EXPECT_EQ(out["latency_evidence"]["pre_send_checks_missing_boundary"], 1);
  }
  Report reversed;
  reversed.add(dispatch_row("A"));
  auto ack = acknowledgement_row("A");
  ack["T1_ns"] = 1070000; // dispatch entry after the recorded check completion
  reversed.add(ack);
  auto out = reversed.json();
  EXPECT_EQ(samples(out, "pre_send_checks"), 0u);
  EXPECT_EQ(out["latency_evidence"]["pre_send_checks_missing_boundary"], 1);
}

TEST(MissReport, ConfirmationEvidenceJoinsBeforeOrAfterAckAndUsesItsEarliestSource) {
  Report report;
  report.add(dispatch_row("websocket-first"));
  report.add(confirmation_row("websocket-first", 11600000, "websocket", 0));
  report.add(confirmation_row("websocket-first", 15600000, "websocket", 2, "MINED"));
  auto update = confirmation_row("websocket-first", 17600000);
  update["result"] = "live_fill_update";
  report.add(update);
  auto first = confirmation_row("websocket-first", 21600000);
  report.add(first); // first positive CONFIRMED evidence precedes HTTP ACK
  report.add(acknowledgement_row("websocket-first"));
  report.add(first); // repeated evidence must not add a latency observation
  report.add(confirmation_row("websocket-first", 51600000, "rest_reconciliation"));

  report.add(dispatch_row("reconciliation-first"));
  report.add(acknowledgement_row("reconciliation-first"));
  report.add(confirmation_row("reconciliation-first", 61600000));
  // Posted later, but actually received before the websocket event above.
  report.add(confirmation_row("reconciliation-first", 51600000, "rest_reconciliation"));
  auto out = report.json();
  const auto &lat = out["latency_ms"];
  EXPECT_EQ(lat["send_to_websocket_confirmation_evidence_ms"]["n"], 1);
  EXPECT_DOUBLE_EQ(lat["send_to_websocket_confirmation_evidence_ms"]["p50"].get<double>(), 20.0);
  EXPECT_EQ(lat["send_to_reconciliation_confirmation_evidence_ms"]["n"], 1);
  EXPECT_DOUBLE_EQ(lat["send_to_reconciliation_confirmation_evidence_ms"]["p50"].get<double>(), 50.0);
  EXPECT_EQ(out["orders"]["submitted"], 2);
  EXPECT_EQ(out["orders"]["acknowledged"], 2);
  EXPECT_EQ(lat["send_to_first_fill_ms"]["n"], 2); // legacy wall metric survives
  EXPECT_DOUBLE_EQ(lat["send_to_first_fill_ms"]["p50"].get<double>(), 400.0);
}

TEST(MissReport, IncompleteOrConflictingConfirmationEvidenceCannotClaimLatency) {
  const auto metric = "send_to_websocket_confirmation_evidence_ms";
  std::vector<Json> invalid;
  for (const auto &receipt : std::vector<Json>{nullptr, 0, -1, true, 21600000.5,
           std::numeric_limits<std::uint64_t>::max(), 1e100}) {
    invalid.push_back(confirmation_row("A", receipt));
  }
  auto missing = confirmation_row("A");
  missing.erase("evidence_received_ns");
  invalid.push_back(missing);
  missing = confirmation_row("A");
  missing.erase("evidence_source");
  invalid.push_back(missing);
  invalid.push_back(confirmation_row("A", 31600000, "unknown"));
  invalid.push_back(confirmation_row("A", 21600000, "rest_reconciliation"));
  for (const auto &bad : invalid) {
    Report report;
    report.add(dispatch_row("A"));
    report.add(acknowledgement_row("A"));
    report.add(confirmation_row("A"));
    report.add(bad);
    auto out = report.json();
    EXPECT_EQ(samples(out, metric), 0u) << bad;
    EXPECT_EQ(samples(out, "send_to_reconciliation_confirmation_evidence_ms"), 0u) << bad;
    EXPECT_EQ(out["orders"]["submitted"], 1);
    EXPECT_EQ(out["orders"]["acknowledged"], 1);
  }
  Report reversed;
  reversed.add(dispatch_row("A"));
  reversed.add(acknowledgement_row("A"));
  reversed.add(confirmation_row("A", 1500000)); // before completed full write
  EXPECT_EQ(samples(reversed.json(), metric), 0u);
  Report conflicting_write;
  conflicting_write.add(dispatch_row("A"));
  conflicting_write.add(acknowledgement_row("A"));
  auto conflicting_ack = acknowledgement_row("A");
  conflicting_ack["T5_ns"] = 1700000;
  conflicting_write.add(conflicting_ack);
  conflicting_write.add(confirmation_row("A"));
  EXPECT_EQ(samples(conflicting_write.json(), metric), 0u);
  Report missing_status;
  missing_status.add(dispatch_row("A"));
  missing_status.add(acknowledgement_row("A"));
  auto no_status = confirmation_row("A");
  no_status.erase("status");
  missing_status.add(no_status);
  EXPECT_EQ(samples(missing_status.json(), metric), 0u);
}

TEST(MissReport, ConfirmationTimesNeverJoinMonotonicClocksAcrossRestarts) {
  Report report;
  report.add(Json{{"result", "start"}, {"wall", 1}});
  report.add(dispatch_row("old-order"));
  report.add(acknowledgement_row("old-order"));
  report.add(Json{{"result", "start"}, {"wall", 2}});
  report.add(acknowledgement_row("old-order"));
  report.add(confirmation_row("old-order", 21600000));
  EXPECT_EQ(samples(report.json(), "send_to_websocket_confirmation_evidence_ms"), 0u);
  // A current-run dispatch and receipt still produce their own observation.
  report.add(dispatch_row("new-order"));
  report.add(confirmation_row("new-order", 21600000));
  report.add(acknowledgement_row("new-order"));
  auto out = report.json();
  EXPECT_EQ(samples(out, "send_to_websocket_confirmation_evidence_ms"), 1u);
  EXPECT_DOUBLE_EQ(out["latency_ms"]["send_to_websocket_confirmation_evidence_ms"]["p50"].get<double>(), 20.0);
}

TEST(MissReport, MonotonicDurationsKeepIntegerPrecisionNearInt64Limit) {
  const auto base = std::numeric_limits<std::int64_t>::max() - 100000000;
  Report report;
  report.add(dispatch_row("A", base));
  report.add(acknowledgement_row("A", base));
  report.add(confirmation_row("A", base + 21600000));
  auto out = report.json();
  const auto &lat = out["latency_ms"];
  EXPECT_DOUBLE_EQ(lat["trigger_to_dispatch"]["p50"].get<double>(), 0.02);
  EXPECT_DOUBLE_EQ(lat["pre_send_checks"]["p50"].get<double>(), 0.04);
  EXPECT_DOUBLE_EQ(lat["durable_intent"]["p50"].get<double>(), 0.41);
  EXPECT_DOUBLE_EQ(lat["request_write"]["p50"].get<double>(), 0.1);
  EXPECT_DOUBLE_EQ(lat["network_and_ack"]["p50"].get<double>(), 30.0);
  EXPECT_DOUBLE_EQ(lat["send_to_websocket_confirmation_evidence_ms"]["p50"].get<double>(), 20.0);
}
