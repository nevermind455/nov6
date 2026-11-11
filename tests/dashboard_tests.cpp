// The terminal dashboard's layout: exact geometry at every size, missing values
// drawn as "--", and text from outside never reaching the screen raw.
#include "sniper/dashboard.hpp"
#include "sniper/book.hpp"
#include "sniper/storage.hpp"
#include <gtest/gtest.h>
#include <string>
using namespace sniper;
using namespace sniper::dash;

namespace {
Json full_snapshot() {
  return Json::parse(R"json({
    "mode":"PAPER","clock":"14:03:27 ET",
    "trigger":{"feed":"coinbase","status":"LIVE","mid":102345.67,"move_bp":0.62,"k_bp":0.5,"window_ms":250.0,
      "signals":1475,"signals_per_min":3.7,"last_signal_bp":0.62,"last_signal_age_s":2.0,"orders":21,"filled":6,
      "refused":15,"fill_rate":0.286,"order_type":"FAK","slip":0.02,"ack_p50_ms":489.0,"ack_simulated":true,
      "errors":0,"in_flight":"UP",
      "fair":{"vol_bp":1.42,"last":{"side":"UP","market_mid":0.53,"fair":0.57,"limit":0.55,"shares":12}}},
    "market":{"slug":"btc-updown-5m-1793963400","seconds_left":153.2,"trade_until_s":10.6,
      "up":{"bid":0.52,"ask":0.54,"status":"LIVE","age_ms":120.0},
      "down":{"bid":0.45,"ask":0.47,"status":"STALE","age_ms":9120.0},
      "round_fills":3,"max_fills":20,"round_up_sh":24.0,"round_down_sh":0.0,"max_side_shares":200.0},
    "orders":[
      {"time":"14:01:02","side":"UP","bp":0.71,"ask":0.51,"cap":0.53,"result":"filled","shares":10.0,"price":0.512,"detail":""},
      {"time":"14:02:10","side":"DOWN","bp":-0.55,"ask":0.47,"cap":0.48,"result":"refused","detail":"unknown book state at the modelled arrival"},
      {"time":"14:03:20","side":"UP","bp":0.62,"ask":0.54,"cap":0.55,"result":"missed","detail":"FAK no-fill: nothing at or below 0.55"}],
    "account":{"kind":"PAPER","cash":941.64,"position_value":45.1,"equity":986.74,"realized":-3.61,"unrealized":1.2,
      "total":-13.26,"fees":0.42,"wins":1,"losses":2},
    "positions":{"rows":[{"side":"UP","shares":24.0,"avg":0.512,"cost":12.29,"bid":0.52,"value":12.48,"pnl":0.19},
      {"side":"DOWN","shares":0.0,"avg":null,"cost":0.0,"bid":0.45,"value":0.0,"pnl":0.0}],
      "pending":{"markets":1,"shares":10.5,"cost":5.2}},
    "settlement":{"active":{"label":"14:00-14:05","state":"ACTIVE","condition_id":"a"},
      "subject":{"label":"13:55-14:00","state":"PENDING SETTLEMENT","age_s":207.0,"detail":"waiting for the on-chain result","condition_id":"b"},
      "pending":[{"label":"13:55-14:00","state":"PENDING SETTLEMENT","age_s":207.0,"condition_id":"b"}],
      "previous":[{"label":"13:50-13:55","state":"SETTLED","winner":"UP","realized":2.14,"has_position":true,"condition_id":"c"}],
      "last_settled":{"label":"13:50-13:55","state":"SETTLED","winner":"UP","realized":2.14,"has_position":true,"condition_id":"c"},
      "pending_count":1},
    "net":{"coinbase":48.2,"clob":24.0},
    "health":[["COINBASE","LIVE","sockets 2/2 behind 12ms backlog max 3"],["POLY BOOK","LIVE","resync waiting 0"],
      ["USER WS","ABSENT","paper"],["SETTLE","OK","pending 1"],["LEDGER","OK","entries 118"]],
    "skips":[["ask not below fair value",402],["book stale",31]],
    "exec":{"paper":true,"latency":{"trigger_to_decision":{"n":21,"last":0.12,"avg":0.2,"p50":0.14,"p95":0.5,"p99":0.9,"max":1.1},
      "trigger_to_ack":{"n":21,"last":489.0,"avg":489.0,"p50":489.0,"p95":489.0,"p99":489.0,"max":489.0,"simulated":true}},
      "quality":{"attempted":21,"sent":21,"acked":21,"pending":0,"filled":5,"partial":1,"zero_fill":2,"rejected":13,"failed":0,
        "fill_rate":0.286,"reject_rate":0.619,"avg_slip_c":0.4,"slip_n":6,
        "last":{"signal_mid":102345.67,"ask_seen":0.54,"cap":0.55,"price":0.541,"slip_c":0.1}}},
    "events":[[1793963000.0,"bot","started","info"],[1793963300.0,"sniper","FILLED UP 10.0 @ 0.512","good"]]
  })json");
}
Json live_snapshot() {
  Json s = full_snapshot();
  s["mode"] = "LIVE";
  s["account"] = Json{{"kind", "LIVE"},   {"balance", 1250.5},   {"redeemable", nullptr},
                      {"equity", 1262.5}, {"position_value", 12}, {"realized", 4.2},
                      {"unrealized", -1}, {"total", 3.2}};
  s["canary"] = Json{{"halted", Json::array({"STOP_LIVE present: delete the stop file to allow new orders once every open order has been checked on the venue"})},
                     {"orders", 3}, {"max_orders", 40}, {"fills", 2}, {"max_fills", 20},
                     {"spend", 12.5}, {"max_spend", 60}, {"rounds", 1}, {"max_rounds", 12},
                     {"loss", 4}, {"max_loss", 20}, {"guard", "OK"}, {"awaiting", 0}};
  return s;
}
// A live bot with SNIPER_REST_LIVE: a missed UP order is on the book.
Json resting_snapshot(const char *state = "resting") {
  Json s = live_snapshot();
  s["market"]["resting"] =
      Json{{"on", true},
           {"order",
            {{"side", "UP"}, {"shares", 5.0}, {"matched", 0.0}, {"price", 0.62},
             {"state", state}, {"left_s", 5.3}, {"life_s", 8.0}, {"rested_s", 2.7},
             {"why", ""}}}};
  s["orders"].push_back(Json{{"time", "14:03:25"}, {"side", "UP"}, {"bp", 1.2},
                             {"ask", 0.66}, {"cap", 0.62}, {"result", "resting"},
                             {"detail", "on the book for up to 8s"}});
  return s;
}
std::string screen(const std::vector<Row> &frame) {
  std::string out;
  for (auto &row : frame) {
    for (auto &run : row)
      out += run.text;
    out += '\n';
  }
  return out;
}
void expect_geometry(const Json &snap, int cols, int rows) {
  auto frame = build(snap, cols, rows);
  ASSERT_EQ(static_cast<int>(frame.size()), rows) << cols << "x" << rows;
  for (auto &row : frame) {
    EXPECT_EQ(static_cast<int>(width(row)), cols) << cols << "x" << rows;
    for (auto &run : row)
      for (unsigned char c : run.text)
        EXPECT_FALSE(c < 0x20 || c == 0x7F) << cols << "x" << rows;
  }
}
} // namespace

TEST(Dashboard, EveryFrameIsExactlyTheTerminalSize) {
  const std::vector<Json> snaps{full_snapshot(), live_snapshot(), Json::object(),
                                Json::array({1, 2})};
  for (auto &snap : snaps)
    for (int cols = 1; cols <= 260; cols += 7)
      for (int rows = 1; rows <= 90; rows += 3)
        expect_geometry(snap, cols, rows);
  expect_geometry(full_snapshot(), 170, 50);
  expect_geometry(full_snapshot(), 80, 24);
}

TEST(Dashboard, ARestingOrderIsShownOnTheBookWithItsTimeLeft) {
  auto text = screen(build(resting_snapshot(), 170, 50));
  for (const char *part : {"resting", "UP 5@0.620", "5.3s", "RESTING",
                           "on the book for up to 8s"})
    EXPECT_NE(text.find(part), std::string::npos) << part;
  // the next frame has less time left
  auto later = resting_snapshot();
  later["market"]["resting"]["order"]["left_s"] = 1.4;
  text = screen(build(later, 170, 50));
  EXPECT_NE(text.find("1.4s"), std::string::npos);
  EXPECT_EQ(text.find("5.3s"), std::string::npos);
  // part of it matched while it rests
  later["market"]["resting"]["order"]["matched"] = 2.0;
  EXPECT_NE(screen(build(later, 170, 50)).find("UP 2/5@0.620"), std::string::npos);
  // a time that ran out is never drawn negative
  later["market"]["resting"]["order"]["left_s"] = -3.0;
  text = screen(build(later, 170, 50));
  EXPECT_NE(text.find("0.0s"), std::string::npos);
  EXPECT_EQ(text.find("-3"), std::string::npos);
  // its other states
  for (auto [state, shown] : std::vector<std::pair<const char *, const char *>>{
           {"placing", "placing"}, {"cancelling", "cancelling"},
           {"cancelled", "off the book"}})
    EXPECT_NE(screen(build(resting_snapshot(state), 170, 50)).find(shown),
              std::string::npos)
        << state;
  // on, with nothing on the book; and switched off by its loss stop
  auto idle = live_snapshot();
  idle["market"]["resting"] = Json{{"on", true}};
  EXPECT_NE(screen(build(idle, 170, 50)).find("no order on the book"), std::string::npos);
  idle["market"]["resting"] = Json{{"on", false}, {"off", "off: the loss stop was reached"}};
  EXPECT_NE(screen(build(idle, 170, 50)).find("off: the loss stop"), std::string::npos);
  // a phone-sized terminal still shows the order and its time
  text = screen(build(resting_snapshot(), 90, 30));
  EXPECT_NE(text.find("UP 5@0.620"), std::string::npos);
  EXPECT_NE(text.find("5.3s"), std::string::npos);
  // without the feature the panel is as before
  EXPECT_EQ(screen(build(live_snapshot(), 170, 50)).find("resting"), std::string::npos);
  // and the frame keeps its exact size everywhere
  for (int cols = 1; cols <= 200; cols += 9)
    for (int rows = 1; rows <= 60; rows += 4)
      for (const char *state : {"resting", "cancelling", "odd state from somewhere"})
        expect_geometry(resting_snapshot(state), cols, rows);
}

TEST(Dashboard, DrawsThePanelsAtACommonSize) {
  auto text = screen(build(full_snapshot(), 170, 50));
  for (const char *part : {"BTC SNIPER", "PAPER", "TRIGGER", "MARKET", "ACCOUNT", "ORDERS",
                           "HEALTH", "POSITION", "SETTLEMENT", "MARKETS", "EVENTS",
                           "EXECUTION QUALITY", "FAK +0.02", "102,345.67", "T-153s",
                           "$941.64", "10.0@0.512", "14:03:27 ET", "PENDING SETTLEMENT"})
    EXPECT_NE(text.find(part), std::string::npos) << part;
}

TEST(Dashboard, MissingValuesAreNotInvented) {
  auto text = screen(build(Json::object(), 120, 40));
  EXPECT_NE(text.find("no round yet"), std::string::npos);
  EXPECT_NE(text.find("--"), std::string::npos);
  EXPECT_EQ(text.find("$0.00"), std::string::npos); // no cash figure from nowhere
  EXPECT_EQ(text.find("nan"), std::string::npos);
}

TEST(Dashboard, WrongTypesAreDrawnNotThrown) {
  Json odd = full_snapshot();
  odd["trigger"]["mid"] = "not a number";
  odd["market"]["seconds_left"] = true;
  odd["orders"] = 5;
  odd["health"] = Json::array({1, Json::array({1, 2}), Json::array({Json::object(), Json::array(), nullptr})});
  odd["events"] = Json::array({"x", Json::array({"a", 1, 2, 3})});
  odd["settlement"]["previous"] = Json::array({1});
  odd["settlement"]["pending"] = "soon";
  odd["skips"] = Json::array({Json::array({nullptr})});
  odd["positions"]["rows"] = Json::array({1, 2, 3});
  expect_geometry(odd, 170, 50);
  EXPECT_EQ(screen(build(odd, 170, 50)).find("layout error"), std::string::npos);
  // a resting order with nonsense in it
  odd["market"]["resting"] =
      Json{{"on", "maybe"},
           {"order",
            {{"side", 7}, {"shares", "many"}, {"matched", Json::array()}, {"price", nullptr},
             {"state", 3}, {"left_s", "soon"}, {"life_s", 0}}}};
  expect_geometry(odd, 170, 50);
  EXPECT_EQ(screen(build(odd, 170, 50)).find("layout error"), std::string::npos);
  odd["market"]["resting"]["order"]["state"] = "resting";
  odd["market"]["resting"]["order"]["life_s"] = 0;
  odd["market"]["resting"]["order"]["left_s"] = 1e308;
  expect_geometry(odd, 170, 50);
  odd["market"]["resting"]["order"] = 7;
  expect_geometry(odd, 170, 50);
  odd["market"]["resting"] = "x";
  expect_geometry(odd, 170, 50);
  EXPECT_EQ(screen(build(odd, 170, 50)).find("layout error"), std::string::npos);
}

TEST(Dashboard, TextFromOutsideCannotControlTheTerminal) {
  auto safe = terminal_text("a\x1b[2Jb\n\tcaf\xc3\xa9\x7f");
  EXPECT_EQ(safe.find('\x1b'), std::string::npos);
  EXPECT_EQ(safe, "a?[2Jb  caf??");
  Json s = full_snapshot();
  s["events"] = Json::array({Json::array({1793963300.0, "error", "\x1b[2Jhacked", "bad"})});
  auto frame = build(s, 170, 50);
  EXPECT_EQ(screen(frame).find('\x1b'), std::string::npos);
  EXPECT_NE(screen(frame).find("?[2Jhacked"), std::string::npos);
}

TEST(Dashboard, SecretsAreHidden) {
  auto hex = "0x" + std::string(64, 'f');
  EXPECT_EQ(terminal_text("key " + hex).find(hex), std::string::npos);
  EXPECT_NE(terminal_text(hex).find("<redacted-32-byte-value>"), std::string::npos);
  EXPECT_EQ(terminal_text("Authorization: abc123").find("abc123"), std::string::npos);
  EXPECT_EQ(terminal_text("secret=hunter2").find("hunter2"), std::string::npos);
  EXPECT_EQ(terminal_text("api-key: zzz9").find("zzz9"), std::string::npos);
  EXPECT_EQ(terminal_text("https://user:pw9@host/x").find("pw9"), std::string::npos);
  EXPECT_EQ(terminal_text("GET /x?api_key=zzz9&a=1").find("zzz9"), std::string::npos);
  // ordinary text is left alone
  EXPECT_EQ(terminal_text("ask not below fair value"), "ask not below fair value");
  EXPECT_EQ(terminal_text("btc-updown-5m-1793963400"), "btc-updown-5m-1793963400");
  EXPECT_EQ(terminal_text("https://localhost:443"), "https://localhost:443");
}

TEST(Dashboard, HttpAuthorizationAndJsonCredentialsAreHidden) {
  for (const auto &sample : {
           std::string("Authorization: Bearer audit-token-123"),
           std::string("Authorization: Basic audit-token-123"),
           std::string(R"({"authorization":"Bearer audit-token-123"})"),
           std::string(R"({"apiKey":"audit-token-123"})"),
           std::string(R"({"secret":"audit-token-123"})"),
           std::string("POLY_API_SECRET=audit-token-123"),
           std::string("POLY_API_PASSPHRASE=audit-token-123")}) {
    SCOPED_TRACE(sample);
    EXPECT_EQ(terminal_text(sample).find("audit-token-123"), std::string::npos);
    EXPECT_NE(terminal_text(sample).find("<redacted>"), std::string::npos);
  }
}

TEST(Dashboard, TruncationDoesNotExposePrivateKeyPrefixes) {
  const auto secret = "0x" + std::string(64, 'f');
  EXPECT_EQ(terminal_text(secret, 32).find(std::string(16, 'f')),
            std::string::npos);
  EXPECT_EQ(terminal_text("prefix " + secret + std::string(300, 'z'), 32)
                .find(std::string(16, 'f')),
            std::string::npos);
  EXPECT_EQ(terminal_text("Authorization: Bearer " + std::string(1000, 's'), 32)
                .find(std::string(8, 's')),
            std::string::npos);
}

TEST(Dashboard, FitCountsCharactersNotBytes) {
  EXPECT_EQ(cells("\xe2\x94\x80\xe2\x94\x80x"), 3u);
  EXPECT_EQ(cells(fit("abcdefgh", 5)), 5u);
  EXPECT_EQ(fit("abc", 5), "abc  ");
  EXPECT_EQ(fit("abc", 5, '>'), "  abc");
  EXPECT_EQ(fit("abc", 0), "");
  Row row = pad({{"\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80", {}}, {"abc", {}}}, 4);
  EXPECT_EQ(width(row), 4u);
}

TEST(Dashboard, HsplitUsesTheWholeWidth) {
  auto w = hsplit(170, {1.0, 1.1, 0.9}, {26, 30, 24});
  ASSERT_EQ(w.size(), 3u);
  EXPECT_EQ(w[0] + w[1] + w[2], 170);
  EXPECT_GE(w[1], w[2]);
}

TEST(Dashboard, ClockFollowsUsEasternTime) {
  // 2026: daylight time begins Mar 8 07:00 UTC and ends Nov 1 06:00 UTC.
  EXPECT_EQ(et_clock(1772953199), "01:59:59 ET");
  EXPECT_EQ(et_clock(1772953200), "03:00:00 ET");
  EXPECT_EQ(et_clock(1793512799), "01:59:59 ET");
  EXPECT_EQ(et_clock(1793512800), "01:00:00 ET");
  EXPECT_EQ(et_label(1793512800, 1793513100), "01:00-01:05");
}

TEST(Dashboard, PlainLineForLogFiles) {
  auto line = plain_line(full_snapshot());
  EXPECT_NE(line.find("[DASH] PAPER btc-updown-5m-1793963400 T-153s"), std::string::npos);
  EXPECT_NE(line.find("orders=21"), std::string::npos);
  EXPECT_EQ(line.find('\x1b'), std::string::npos);
}

TEST(Dashboard, LiveMetricsRequireConfirmedBookingsAndExplicitHttpAcknowledgements) {
  Intent order;
  order.id = "pending-order";
  order.side = "UP";
  order.sent_wall = 101;
  order.stake = 5 * U;
  order.limit = 500000;
  order.status = "matched";
  order.matched_shares = 6 * U;
  order.matched_cost = 3 * U;
  std::map<std::string, Intent> orders{{order.id, order}};
  std::set<std::string> acknowledged;
  const std::map<std::string, double> asks{{order.id, .49}};
  auto q = live_execution_quality(orders, acknowledged, asks, 100);
  EXPECT_EQ(q["sent"], 1);
  EXPECT_EQ(q["acked"], 0);
  EXPECT_EQ(q["filled"], 0);
  EXPECT_EQ(q["partial"], 0);
  EXPECT_EQ(q["pending"], 1);
  acknowledged.insert(order.id); // HTTP match acknowledgement is not a fill.
  q = live_execution_quality(orders, acknowledged, asks, 100);
  EXPECT_EQ(q["acked"], 1);
  EXPECT_EQ(q["partial"], 0);
  orders.at(order.id).booked_shares = 2 * U;
  orders.at(order.id).booked_cost = 900000;
  q = live_execution_quality(orders, acknowledged, asks, 100);
  EXPECT_EQ(q["partial"], 1);
  EXPECT_EQ(q["pending"], 1);
  EXPECT_DOUBLE_EQ(q["last"]["price"].get<double>(), .45);
  // A later confirmed trade updates the same order's cumulative VWAP/count.
  orders.at(order.id).booked_shares = 6 * U;
  orders.at(order.id).booked_cost = 3 * U;
  orders.at(order.id).status = "confirmed";
  q = live_execution_quality(orders, acknowledged, asks, 100);
  EXPECT_EQ(q["filled"], 0);
  EXPECT_EQ(q["partial"], 1);
  EXPECT_EQ(q["pending"], 0);
  EXPECT_DOUBLE_EQ(q["last"]["price"].get<double>(), .5);
  EXPECT_DOUBLE_EQ(q["last"]["shares"].get<double>(), 6);
  EXPECT_EQ(q["slip_n"], 1);
  EXPECT_NEAR(q["avg_slip_c"].get<double>(), 1.0, 1e-10);
  EXPECT_DOUBLE_EQ(q["fill_rate"].get<double>(), 1);
  EXPECT_EQ(q, live_execution_quality(orders, acknowledged, asks, 100));
}

TEST(Dashboard, LiveMetricsSurviveOrderTableEvictionAndExcludePreviousRuns) {
  std::map<std::string, Intent> orders;
  std::set<std::string> acknowledged;
  for (int n = 0; n < 60; ++n) {
    Intent i;
    i.id = std::to_string(n);
    i.sent_wall = 101 + n;
    i.status = n == 0 ? "unknown" : "no_match";
    i.stake = 5 * U;
    orders.emplace(i.id, i);
    if (n != 0)
      acknowledged.insert(i.id);
  }
  Intent previous;
  previous.id = "previous-run";
  previous.sent_wall = 99;
  previous.status = "confirmed";
  previous.booked_shares = 10 * U;
  previous.booked_cost = previous.stake = 5 * U;
  orders.emplace(previous.id, previous);
  auto q = live_execution_quality(orders, acknowledged, {}, 100);
  EXPECT_EQ(q["attempted"], 60);
  EXPECT_EQ(q["acked"], 59);
  EXPECT_EQ(q["pending"], 1);
  EXPECT_EQ(q["zero_fill"], 59);
  auto &late = orders.at("0"); // already older than the table's 40 rows
  late.booked_shares = 10 * U;
  late.booked_cost = 5 * U;
  late.status = "confirmed";
  q = live_execution_quality(orders, acknowledged, {{"0", .49}}, 100);
  EXPECT_EQ(q["filled"], 1);
  EXPECT_EQ(q["acked"], 59); // a confirmed private trade is not an HTTP ACK
  EXPECT_EQ(q["pending"], 0);
  EXPECT_EQ(q["last"]["order_id"], "0");
  EXPECT_EQ(q["zero_fill"], 59);
  auto &newer = orders.at("1");
  newer.booked_shares = 4 * U;
  newer.booked_cost = 2 * U;
  newer.status = "confirmed";
  // The latest confirmed event can belong to an older submitted order.
  q = live_execution_quality(orders, acknowledged, {{"0", .49}}, 100, "0");
  EXPECT_EQ(q["last"]["order_id"], "0");
  EXPECT_EQ(q["filled"], 1);
  EXPECT_EQ(q["partial"], 1);
}

TEST(Dashboard, LiveAbortedRejectedAndUncertainOrdersHaveDistinctMetrics) {
  std::map<std::string, Intent> orders;
  for (const auto *status : {"aborted", "refused", "no_match", "unknown"}) {
    Intent i;
    i.id = i.status = status;
    i.sent_wall = 101;
    orders.emplace(i.id, i);
  }
  const auto q = live_execution_quality(orders, {"refused", "no_match"}, {}, 100);
  EXPECT_EQ(q["attempted"], 4);
  EXPECT_EQ(q["sent"], 3);
  EXPECT_EQ(q["acked"], 2);
  EXPECT_EQ(q["failed"], 1);
  EXPECT_EQ(q["rejected"], 1);
  EXPECT_EQ(q["zero_fill"], 1);
  EXPECT_EQ(q["pending"], 1);
}

TEST(Dashboard, LiveLatencyUsesCompleteWireWriteAndActualAcknowledgement) {
  Json row{{"T0_ns", 1000000000LL}, {"T2_ns", 1002000000LL},
            {"T5_ns", 1020000000LL}, {"T6_ns", 1050000000LL},
            {"acknowledged", true}, {"reply_ms", 999.0}};
  auto samples = execution_latencies(row);
  EXPECT_EQ(samples["trigger_to_decision"], 2);
  EXPECT_EQ(samples["trigger_to_send"], 20);
  EXPECT_EQ(samples["send_to_ack"], 30);
  EXPECT_EQ(samples["trigger_to_ack"], 50);
  EXPECT_EQ(samples["enqueue_to_ack"], 999);
  row.erase("T5_ns");
  samples = execution_latencies(row);
  EXPECT_FALSE(samples.contains("trigger_to_send"));
  EXPECT_FALSE(samples.contains("send_to_ack")); // T2 never substitutes T5
  row["T5_ns"] = 1020000000LL;
  row["acknowledged"] = false;
  samples = execution_latencies(row);
  EXPECT_FALSE(samples.contains("send_to_ack"));
  EXPECT_FALSE(samples.contains("trigger_to_ack"));
  EXPECT_FALSE(samples.contains("enqueue_to_ack"));
  row["acknowledged"] = true;
  row["T5_ns"] = 1060000000LL; // early reply before the upload ended
  EXPECT_FALSE(execution_latencies(row).contains("send_to_ack"));
}

TEST(Dashboard, PositionMarksIncludeFeesAndRequireFreshLiveBids) {
  BookView book;
  book.status = LIVE;
  book.updated_mono = 100;
  book.bids = {{.6, 20}};
  auto position = mark_position(10, 5, .1, book, 100.5, 2000);
  EXPECT_DOUBLE_EQ(position["value"].get<double>(), 6);
  EXPECT_NEAR(position["pnl"].get<double>(), .9, 1e-12);
  book.status = STALE;
  EXPECT_TRUE(mark_position(10, 5, .1, book, 100.5, 2000)["value"].is_null());
  book.status = LIVE;
  EXPECT_TRUE(mark_position(10, 5, .1, book, 103, 2000)["pnl"].is_null());
  EXPECT_TRUE(mark_position(10, 5, .1, book, 99, 2000)["value"].is_null());
  book.updated_mono.reset();
  EXPECT_TRUE(mark_position(10, 5, .1, book, 100, 2000)["value"].is_null());
  EXPECT_EQ(mark_position(0, 0, 0, book, 100, 2000)["value"], 0);
}

TEST(Dashboard, StaleLiveCollateralIsUnavailableInsteadOfCurrentEquity) {
  auto current = live_balance_view(123.45, 5);
  EXPECT_EQ(current["balance_status"], "OK");
  EXPECT_DOUBLE_EQ(current["balance"].get<double>(), 123.45);
  auto stale = live_balance_view(123.45, 121);
  EXPECT_EQ(stale["balance_status"], "STALE");
  EXPECT_TRUE(stale["balance"].is_null());
  EXPECT_EQ(live_balance_view({}, 5)["balance_status"], "UNKNOWN");
  EXPECT_TRUE(live_balance_view(123.45, -1)["balance"].is_null());
}
