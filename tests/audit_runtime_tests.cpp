#include "sniper/runtime.hpp"
#include "sniper/storage.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace sniper;
TEST(ExecutionTiming, DispatchAndSigningBoundariesStayDistinct) {
  std::array<std::int64_t, 8> stamps{100, 200, 300, 400, 500, 0, 700, 0};
  auto fields = execution::order_timing_fields(stamps);
  EXPECT_EQ(fields["T0_ns"], 100);
  EXPECT_EQ(fields["T1_ns"], 200);
  EXPECT_EQ(fields["T2_ns"], 300);
  EXPECT_EQ(fields["T6_ns"], 700);
  EXPECT_FALSE(fields.contains("T5_ns"));
  EXPECT_FALSE(fields.contains("T7_ns"));
}
TEST(ExecutionTiming, PrivateEvidenceKeepsItsSourceAndRawVenueClock) {
  Json trade{{"match_time", "1900000000"}, {"last_update", 1900000001000LL},
             {"timestamp", nullptr}, {"created_at", Json::object()},
             {"apiKey", "never copy unrelated data"}};
  auto fields = execution::trade_evidence_fields(trade, "websocket", 123456789);
  EXPECT_EQ(fields["evidence_source"], "websocket");
  EXPECT_EQ(fields["evidence_received_ns"], 123456789);
  EXPECT_EQ(fields["venue_event_times_raw"]["match_time"], "1900000000");
  EXPECT_EQ(fields["venue_event_times_raw"]["last_update"], 1900000001000LL);
  EXPECT_EQ(fields["venue_event_times_raw"].size(), 2u);
  EXPECT_FALSE(fields.contains("apiKey"));
  auto polled = execution::trade_evidence_fields(trade, "rest_reconciliation", 200);
  EXPECT_EQ(polled["evidence_source"], "rest_reconciliation");
  EXPECT_EQ(polled["evidence_received_ns"], 200);
}
TEST(ExecutionTiming, MissingReceiptAndUnboundedVenueTextAreNotInvented) {
  Json trade{{"match_time", std::string(256, 'x')},
             {"timestamp", std::numeric_limits<double>::infinity()}};
  auto fields = execution::trade_evidence_fields(trade, "websocket", 0);
  EXPECT_FALSE(fields.contains("evidence_received_ns"));
  EXPECT_EQ(fields["venue_event_times_raw"]["match_time"].get<std::string>().size(), 128u);
  EXPECT_FALSE(fields["venue_event_times_raw"].contains("timestamp"));
}
namespace {
struct RuntimeDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("sniper-runtime-audit-" + std::to_string(getpid()) + "-" + std::to_string(mono_ns()));
  RuntimeDirectory() { std::filesystem::create_directories(path); }
  ~RuntimeDirectory() { std::filesystem::remove_all(path); }
  Config config(const std::string &contents = "") {
    auto file = path / ".env";
    std::ofstream(file) << contents;
    chmod(file.c_str(), 0600);
    std::vector<std::string> args{"sniper", "--config", file.string(), "--check"};
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    return Config::load(static_cast<int>(argv.size()), argv.data());
  }
};
}
TEST(AuditRuntime, ConfirmedDebitsCannotReuseAStaleCollateralSample) {
  EXPECT_EQ(execution::spendable_collateral(100 * U, 10 * U, 13 * U), 97 * U);
  EXPECT_EQ(execution::spendable_collateral(97 * U, 13 * U, 13 * U), 97 * U);
  // A response already reflecting the fill is conservatively debited until
  // the following sample; an in-flight response cannot free the same cash twice.
  EXPECT_EQ(execution::spendable_collateral(97 * U, 10 * U, 13 * U), 94 * U);
  EXPECT_EQ(execution::spendable_collateral(U, 0, 2 * U), 0);
  EXPECT_THROW(execution::spendable_collateral(U, 2 * U, U), std::invalid_argument);
}
TEST(AuditRuntime, AccountSubscriptionsDropSettledHistoryButRetainUncertainOrders) {
  RuntimeDirectory dir;
  Ledger ledger(dir.path / "ledger.json", 100 * U, false);
  Intent pending;
  pending.id = "pending";
  pending.condition = "unresolved";
  ledger.register_intent(pending, nullptr, {});
  auto settled = pending;
  settled.id = "settled";
  settled.condition = "old";
  settled.status = "confirmed";
  settled.booked_shares = U;
  ledger.register_intent(settled, nullptr, {});
  ledger.settled_conditions.insert("old");
  EXPECT_EQ(execution::account_markets(ledger, {"current", "prepared"}),
            (std::set<std::string>{"current", "prepared", "unresolved"}));
}
TEST(AuditRuntime, OfficialIntermediateTradeStatusRemainsPending) {
  for (auto status : {"MATCHED", "MATCHED_NOT_BROADCASTED", "MINED", "RETRYING"})
    EXPECT_TRUE(execution::pending_trade_status(status)) << status;
  EXPECT_FALSE(execution::pending_trade_status("FAILED"));
  EXPECT_FALSE(execution::pending_trade_status("CONFIRMED"));
}
TEST(AuditRuntime, DiscoveryRefreshesTheActiveLiveFeeAndMinimumSnapshot) {
  Round active;
  active.condition = "condition";
  active.up = "123";
  active.down = "456";
  active.start = 300;
  active.end = 600;
  active.rules[0].rate = 70000;
  auto fresh = active;
  fresh.rules[0].rate = 80000;
  fresh.rules[0].min_shares = 10 * U;
  EXPECT_TRUE(execution::refresh_round_rules(active, fresh));
  EXPECT_EQ(active.rules[0].rate, 80000);
  EXPECT_EQ(active.rules[0].min_shares, 10 * U);
  fresh.up = "wrong";
  EXPECT_FALSE(execution::refresh_round_rules(active, fresh));
  EXPECT_EQ(active.up, "123");
}
TEST(AuditRuntime, RefreshedDiscoveryReplacesTheSharedDecisionAndPresendRules) {
  Round round;
  round.condition = "condition";
  round.up = "123";
  round.down = "456";
  for (auto &rules : round.rules) {
    rules.loaded = true;
    rules.rate = 70000;
    rules.min_shares = 5 * U;
    rules.tick = 10000;
  }
  std::map<std::string, MarketRules> cache;
  ASSERT_TRUE(execution::cache_round_rules(cache, round, 1000));
  ASSERT_EQ(cache.size(), 1);
  EXPECT_EQ(cache.at(round.condition).fee_rate, py::D("0.07"));
  for (auto &rules : round.rules) {
    rules.rate = 80000;
    rules.exponent = 2;
    rules.min_shares = 10 * U;
    rules.tick = 1000;
  }
  ASSERT_TRUE(execution::cache_round_rules(cache, round, 1010));
  const auto &refreshed = cache.at(round.condition);
  EXPECT_EQ(cache.size(), 1);
  EXPECT_EQ(refreshed.fee_rate, py::D("0.08"));
  EXPECT_EQ(refreshed.fee_exponent, 2);
  ASSERT_TRUE(refreshed.min_order_size);
  EXPECT_EQ(*refreshed.min_order_size, py::D(10));
  ASSERT_TRUE(refreshed.tick_size);
  EXPECT_EQ(*refreshed.tick_size, py::D("0.001"));
  EXPECT_EQ(refreshed.up_token_id, round.up);
  EXPECT_EQ(refreshed.down_token_id, round.down);
  EXPECT_DOUBLE_EQ(refreshed.fetched_wall, 1010);
  EXPECT_EQ(refreshed.taker_delay_ms, 0.0);
  EXPECT_TRUE(refreshed.taker_delay_flagged);
  round.rules[1].loaded = false;
  EXPECT_FALSE(execution::cache_round_rules(cache, round, 1020));
  EXPECT_DOUBLE_EQ(cache.at(round.condition).fetched_wall, 1010);
}
TEST(AuditRuntime, ConfigRefusesNativeIntegerOverflowBeforeNarrowing) {
  RuntimeDirectory dir;
  for (const auto *setting : {"SNIPER_SHARES", "SNIPER_HOLD_MS", "SNIPER_BOOK_WAIT_MS"})
    EXPECT_THROW(dir.config(std::string(setting) + "=1e100\n"), std::invalid_argument) << setting;
}
TEST(AuditRuntime, PublicCheckReportsEffectiveRiskAndAuditPaths) {
  RuntimeDirectory dir;
  auto config = dir.config("SNIPER_CANARY_MAX_LOSS_USD=5\nSNIPER_CANARY_HARD_STOP=0\nPAPER_AUDIT_PATH=custom-paper.jsonl\nSNIPER_CANARY_STOP_FILE=custom-stop\n");
  auto view = config.public_view();
  EXPECT_EQ(view["live_canary"]["max_loss_usd"], 5.0);
  EXPECT_EQ(view["live_canary"]["hard_stop"], false);
  EXPECT_EQ(view["paths"]["paper_journal"], (dir.path / "custom-paper.jsonl").string());
  EXPECT_EQ(view["paths"]["stop_file"], (dir.path / "custom-stop").string());
}
TEST(AuditRuntime, IncompatibleStateAndAuditFilesCannotOverwriteEachOther) {
  RuntimeDirectory dir;
  EXPECT_THROW(dir.config("PAPER_LEDGER_PATH=same\nPAPER_AUDIT_PATH=same\n"), std::invalid_argument);
  EXPECT_THROW(dir.config("PAPER_LEDGER_PATH=ledger\nPAPER_AUDIT_PATH=ledger.journal\n"), std::invalid_argument);
  EXPECT_THROW(dir.config("PAPER_AUDIT_PATH=paper_audit.jsonl\n"), std::invalid_argument);
  EXPECT_NO_THROW(dir.config());
}
TEST(AuditRuntime, PrepareLeadDefaultsToSixtySecondsAndIsBounded) {
  RuntimeDirectory dir;
  EXPECT_EQ(dir.config().prepare_lead, 60.0);
  EXPECT_EQ(dir.config().public_view()["prepare_lead_s"], 60.0);
  EXPECT_EQ(dir.config("SNIPER_PREPARE_LEAD_S=120\n").prepare_lead, 120.0);
  for (const auto *bad : {"10", "241", "nan"})
    EXPECT_THROW(dir.config(std::string("SNIPER_PREPARE_LEAD_S=") + bad + "\n"),
                 std::invalid_argument) << bad;
}
TEST(AuditRuntime, NextRoundPreparationCoversTheWholeLeadWindow) {
  const double start = 1'762'500'000; // a five-minute boundary
  ASSERT_EQ(execution::round_window(start + 299.9), start);
  EXPECT_FALSE(execution::prepare_due(start, 60));          // 300 s left
  EXPECT_FALSE(execution::prepare_due(start + 239.9, 60));  // 60.1 s left
  EXPECT_TRUE(execution::prepare_due(start + 240, 60));     // exactly 60 s
  // The old fixed 25-30 s slot is inside the window, and a tick that lands
  // after it (a stalled loop) still prepares instead of leaving it cold.
  EXPECT_TRUE(execution::prepare_due(start + 272, 60));
  EXPECT_TRUE(execution::prepare_due(start + 290, 60));
  EXPECT_TRUE(execution::prepare_due(start + 299.99, 60));
  // At the boundary the clock belongs to the next round: nothing is due.
  EXPECT_EQ(execution::round_window(start + 300), start + 300);
  EXPECT_FALSE(execution::prepare_due(start + 300, 30));
  EXPECT_TRUE(execution::prepare_due(start + 60, 240));
}
TEST(AuditRuntime, SubscriptionsFollowOnlyTheCurrentAndPreparedRounds) {
  Round current, next;
  current.start = 1'762'500'000;
  current.up = "cur-up";
  current.down = "cur-down";
  next.start = current.start + 300;
  next.up = "next-up";
  next.down = "next-down";
  EXPECT_EQ(execution::round_tokens(current, next),
            (std::set<std::string>{"cur-up", "cur-down", "next-up", "next-down"}));
  // A refresh that drops the prepared round (identity change) must retire its
  // books even though no other round replaces it.
  EXPECT_EQ(execution::round_tokens(current, std::nullopt),
            (std::set<std::string>{"cur-up", "cur-down"}));
  // After the boundary the prepared round is the current one; the previous
  // round's tokens are no longer wanted.
  EXPECT_EQ(execution::round_tokens(next, std::nullopt),
            (std::set<std::string>{"next-up", "next-down"}));
  EXPECT_TRUE(execution::round_tokens(std::nullopt, std::nullopt).empty());
}
TEST(AuditRuntime, TriggersAfterABoundarySwitchTheRoundFirst) {
  const double start = 1'762'500'000;
  const auto window = execution::round_window(start);
  EXPECT_FALSE(execution::round_switch_due(start + 299.999, window));
  // 10 ms into the next round, before the 50 ms timer: the trigger handler
  // must switch instead of deciding against the old round.
  EXPECT_TRUE(execution::round_switch_due(start + 300.010, window));
  EXPECT_FALSE(execution::round_switch_due(start + 300.010, window + 300));
  // Before the first switch (startup) the state is always behind.
  EXPECT_TRUE(execution::round_switch_due(start, -1));
}
TEST(AuditRuntime, ARepeatedFaultPausesAgainOnceItsPauseHasExpired) {
  execution::PauseLatch latch;
  const std::string unknown = "unknown native order outcome";
  ASSERT_TRUE(latch.admit(unknown, 1000));
  latch.hold(unknown, 1300);
  // A repeat inside the running pause does not extend it or log again...
  EXPECT_FALSE(latch.admit(unknown, 1100));
  EXPECT_FALSE(latch.admit(unknown, 1299.9));
  // ...a different reason is independent...
  EXPECT_TRUE(latch.admit("private trade failed; reconciliation required", 1100));
  // ...and the same reason hours later pauses again (it used to latch for the
  // whole process, so a second unknown outcome paused nothing).
  EXPECT_TRUE(latch.admit(unknown, 1300));
  EXPECT_TRUE(latch.admit(unknown, 15400));
  latch.hold(unknown, 15700);
  EXPECT_FALSE(latch.admit(unknown, 15500));
  // Expired reasons are dropped, so the latch cannot grow without bound.
  for (int i = 0; i < 100; ++i)
    latch.hold("fault in native handler " + std::to_string(i), 20000.0 + 400.0 * i);
  EXPECT_LE(latch.size(), 2u);
}
