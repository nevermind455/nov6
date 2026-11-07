#include "sniper/storage.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <type_traits>
#include <unistd.h>

using namespace sniper;
namespace {
struct AuditDirectory {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("sniper-ledger-audit-" + std::to_string(getpid()) + "-" +
       std::to_string(mono_ns()));
  AuditDirectory() { std::filesystem::create_directories(path); }
  ~AuditDirectory() { std::filesystem::remove_all(path); }
};
Intent audit_intent(std::string id = "audit-order") {
  Intent i;
  i.id = std::move(id);
  i.condition = "condition-a";
  i.token = i.up = "123";
  i.down = "456";
  i.status = "sent";
  i.stake = 5 * U;
  i.reserve = 5350000;
  i.target = 10 * U;
  i.limit = U / 2;
  i.fee_rate = 70000;
  return i;
}
void book_audit_fill(Ledger &l, const Intent &i) {
  l.register_intent(i, nullptr, l.canary);
  l.status(i.id, "matched", 6 * U, 3 * U);
  ASSERT_TRUE(l.fill(i.id + "-fill", i.id, 6 * U, 3 * U, 105000));
}
} // namespace

TEST(AuditStorageHandles, FileDescriptorOwnersCannotBeCopied) {
  EXPECT_FALSE(std::is_copy_constructible_v<FileLock>);
  EXPECT_FALSE(std::is_copy_assignable_v<FileLock>);
  EXPECT_FALSE(std::is_copy_constructible_v<DurableLog>);
  EXPECT_FALSE(std::is_copy_assignable_v<DurableLog>);
}
TEST(AuditDurableLog, ReplayRefusesAnUnavailableJournal) {
  AuditDirectory dir;
  auto path = dir.path / "journal";
  DurableLog log(path);
  log.append({{"type", "evidence"}});
  std::filesystem::rename(path, dir.path / "moved-journal");
  EXPECT_THROW(log.replay([](auto, const auto &) {}), std::runtime_error);
}
TEST(AuditLedger, InitialCashSurvivesRestartBeforeTheFirstPeriodicSnapshot) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  {
    Ledger l(path, 100 * U, false);
    book_audit_fill(l, audit_intent());
    // No explicit snapshot: this is also the state after a process crash.
  }
  Ledger recovered(path, 200 * U, false);
  EXPECT_EQ(recovered.cash, 96895000);
  EXPECT_EQ(recovered.reference_cash, 100 * U);
}
TEST(AuditLedger, MissingSnapshotWithExistingHistoryRefusesAmbiguousRecovery) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  {
    Ledger l(path, 100 * U, false);
    book_audit_fill(l, audit_intent());
  }
  std::filesystem::remove(path);
  EXPECT_THROW((Ledger(path, 200 * U, false)), std::runtime_error);
}
TEST(AuditLedger, ValidJsonSnapshotCorruptionCannotChangeTheRecoveredBankroll) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  {
    Ledger ledger(path, 100 * U, false);
    book_audit_fill(ledger, audit_intent());
    ledger.snapshot();
  }
  Json snapshot;
  std::ifstream(path) >> snapshot;
  ASSERT_EQ(snapshot["format"], "sniper-native-ledger-2");
  snapshot["cash"] = 200 * U;
  std::ofstream(path) << snapshot.dump();
  EXPECT_THROW((Ledger(path, 100 * U, false)), std::runtime_error);
}
TEST(AuditLedger, SettlementRejectsAnotherConditionsPositionBeforeWriting) {
  AuditDirectory dir;
  Ledger l(dir.path / "ledger.json", 100 * U, false);
  auto a = audit_intent(), b = audit_intent("other-order");
  b.condition = "condition-b";
  b.token = b.up = "789";
  b.down = "987";
  book_audit_fill(l, a);
  book_audit_fill(l, b);
  auto entries = l.entries;
  EXPECT_THROW(l.settle(a.condition, {{a.token, U}, {b.token, U}}),
               std::invalid_argument);
  EXPECT_EQ(l.entries, entries);
  EXPECT_FALSE(l.positions.at(b.token).settled);
  EXPECT_FALSE(l.settled_conditions.count(a.condition));
  EXPECT_TRUE(l.settle(a.condition, {{a.token, U}}));
  EXPECT_EQ(l.holding(b.token), 6 * U);
}
TEST(AuditLedger, InvalidPayoutDoesNotPoisonTheDurableHistory) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto i = audit_intent();
  {
    Ledger l(path, 100 * U, false);
    book_audit_fill(l, i);
    auto entries = l.entries;
    EXPECT_THROW(l.settle(i.condition, {{i.token, -1}}), std::invalid_argument);
    EXPECT_EQ(l.entries, entries);
    EXPECT_FALSE(l.settled_conditions.count(i.condition));
    EXPECT_THROW(l.settle(i.condition, {{i.token, U + 1}}),
                 std::invalid_argument);
    EXPECT_EQ(l.entries, entries);
  }
  Ledger recovered(path, 100 * U, false);
  EXPECT_TRUE(recovered.settle(i.condition, {{i.token, U}}));
}
TEST(AuditLedger, LatePositiveEvidenceRestoresOneCanarySlotAcrossRestart) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto i = audit_intent();
  i.fill_slot = true;
  CanaryState c;
  c.started = 1001;
  c.fills = 1;
  {
    Ledger l(path, 100 * U, true);
    l.register_intent(i, nullptr, c);
    l.status(i.id, "no_match");
    l.release_fill_slot(i.id);
    EXPECT_EQ(l.canary.fills, 0);
    l.status(i.id, "matched", 2 * U, U);
    EXPECT_EQ(l.canary.fills, 1);
    EXPECT_TRUE(l.orders.at(i.id).fill_slot);
    l.status(i.id, "matched", 2 * U, U);
    EXPECT_EQ(l.canary.fills, 1);
    l.snapshot();
  }
  {
    Ledger l(path, 100 * U, true);
    EXPECT_EQ(l.canary.fills, 1);
    EXPECT_TRUE(l.fill("late-fill", i.id, 2 * U, U, 35000));
    EXPECT_EQ(l.canary.fills, 1);
  }
  Ledger recovered(path, 100 * U, true);
  EXPECT_EQ(recovered.canary.fills, 1);
}
TEST(AuditLedger, RecoveryRepairsACrashBetweenMatchEvidenceAndSlotRestoration) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto i = audit_intent();
  i.fill_slot = true;
  CanaryState c;
  c.started = 1001;
  c.fills = 1;
  {
    Ledger l(path, 100 * U, true);
    l.register_intent(i, nullptr, c);
    l.status(i.id, "no_match");
    l.release_fill_slot(i.id);
    l.snapshot();
  }
  {
    DurableLog log(path.string() + ".journal");
    log.replay([](auto, const auto &) {});
    // Simulate a crash after committing the status but before slot repair.
    log.append({{"type", "status"}, {"id", i.id}, {"status", "matched"},
                {"shares", 2 * U}, {"cost", U}});
  }
  {
    Ledger l(path, 100 * U, true);
    EXPECT_EQ(l.canary.fills, 1);
    EXPECT_TRUE(l.orders.at(i.id).fill_slot);
  }
  Ledger recovered(path, 100 * U, true);
  EXPECT_EQ(recovered.canary.fills, 1);
}
TEST(AuditLedger, PreviousSessionOrdersCannotSpendTheNewSessionsFillBudget) {
  AuditDirectory dir;
  Ledger l(dir.path / "ledger.json", 100 * U, true);
  auto old = audit_intent("old-order"), next = audit_intent("new-order");
  old.fill_slot = next.fill_slot = true;
  CanaryState c;
  c.started = 1001;
  c.fills = 1;
  l.register_intent(old, nullptr, c);
  c.started = 2002;
  c.fills = 1;
  l.register_intent(next, nullptr, c);
  l.status(old.id, "no_match");
  l.release_fill_slot(old.id);
  EXPECT_EQ(l.canary.fills, 1);
  l.status(old.id, "matched", 2 * U, U);
  EXPECT_EQ(l.canary.fills, 1);
  EXPECT_TRUE(l.fill("old-late-fill", old.id, 2 * U, U, 35000));
  EXPECT_EQ(l.canary.fills, 1);
}
TEST(AuditMoney, ArithmeticRejectsOverflowAndWidensBeforeAddingTheFeeRate) {
  constexpr auto max = std::numeric_limits<Units>::max();
  EXPECT_THROW(multiply(max, max), std::overflow_error);
  EXPECT_THROW(multiply(max, max, true), std::overflow_error);
  EXPECT_EQ(shares_within_budget(max, 1, max), 999999);
  EXPECT_THROW(minimum_entry_shares(1, 5 * U, max), std::overflow_error);
  EXPECT_THROW(fee_total(max, U / 2, max, 1), std::overflow_error);
}
TEST(AuditMoney, NumericStringsRejectTrailingNonNumericData) {
  EXPECT_THROW(number(Json("100junk")), std::invalid_argument);
  EXPECT_THROW(number(Json("1.25e2suffix")), std::invalid_argument);
  EXPECT_DOUBLE_EQ(number(Json(" 1.25e2 ")), 125);
}
TEST(AuditLedger, OverflowCannotCommitAnUnrecoverableFill) {
  AuditDirectory dir;
  Ledger ledger(dir.path / "ledger.json", 100 * U, false);
  auto order = audit_intent();
  order.stake = order.reserve = std::numeric_limits<Units>::max() - 1;
  order.target = std::numeric_limits<Units>::max();
  ledger.register_intent(order, nullptr, {});
  ASSERT_TRUE(ledger.fill("first", order.id, std::numeric_limits<Units>::max() - 100, 1, 0));
  auto entries = ledger.entries;
  EXPECT_THROW(ledger.fill("overflow", order.id, 200, 1, 0), std::overflow_error);
  EXPECT_EQ(ledger.entries, entries);
  EXPECT_FALSE(ledger.trade_ids.count("overflow"));
}
TEST(AuditLedger, LegacyOverflowingHistoryRefusesRecovery) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto order = audit_intent();
  order.stake = order.reserve = std::numeric_limits<Units>::max() - 1;
  order.target = std::numeric_limits<Units>::max();
  {
    Ledger ledger(path, 100 * U, false);
    ledger.register_intent(order, nullptr, {});
    ASSERT_TRUE(ledger.fill("first", order.id, std::numeric_limits<Units>::max() - 100, 1, 0));
  }
  {
    DurableLog log(path.string() + ".journal");
    log.replay([](auto, const auto &) {});
    log.append({{"type", "fill"}, {"id", order.id}, {"trade", "legacy-overflow"},
                {"shares", 200}, {"cost", 1}, {"fee", 0}});
  }
  EXPECT_THROW((Ledger(path, 100 * U, false)), std::overflow_error);
}
TEST(AuditCrypto, DecimalTokenIdsHaveTheSameDigestWithLeadingZeros) {
  UnsignedOrder o;
  o.salt = 42;
  o.maker = o.signer = "0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf";
  o.exchange = STANDARD_EXCHANGE;
  o.maker_amount = 5100000;
  o.taker_amount = 10 * U;
  o.timestamp = 1900000000123;
  o.token = "123";
  auto want = order_digest(o);
  EXPECT_EQ(hex(want),
            "0x5523d2a8739a225fb47f9b6d0d171be92b9557567891433462bf1fc0240f17d4");
  o.token = "0123";
  EXPECT_EQ(order_digest(o), want);
  o.token = "19";
  want = order_digest(o);
  o.token = "019";
  EXPECT_EQ(order_digest(o), want);
}
TEST(AuditCrypto, InvalidPrivateKeysReleaseTheirNativeSigningContexts) {
  for (int attempt = 0; attempt < 32; ++attempt)
    EXPECT_THROW((Signer("0x" + std::string(64, '0'))), std::invalid_argument);
  // LeakSanitizer checks that rejected constructions own no leaked contexts.
}
TEST(AuditLedger, ConfirmedMissRefundIsDurableAndIdempotent) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto order = audit_intent();
  order.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = 10 * U;
  state.fills = 1;
  {
    Ledger ledger(path, 100 * U, true);
    ledger.register_intent(order, nullptr, state);
    ledger.status(order.id, "refused");
    ledger.release_missed_spend(order.id);
    EXPECT_EQ(ledger.canary.spend, 10 * U);
    ledger.status(order.id, "no_match");
    ledger.release_missed_spend(order.id);
    EXPECT_EQ(ledger.canary.spend, 5 * U);
    auto entries = ledger.entries;
    ledger.release_missed_spend(order.id);
    EXPECT_EQ(ledger.entries, entries);
    ledger.snapshot();
  }
  Ledger recovered(path, 100 * U, true);
  EXPECT_TRUE(recovered.orders.at(order.id).canary_spend_released);
  auto entries = recovered.entries;
  recovered.release_missed_spend(order.id);
  EXPECT_EQ(recovered.entries, entries);
  EXPECT_EQ(recovered.canary.spend, 5 * U);
}
TEST(AuditLedger, LatePositiveEvidenceRestoresTheRefundedStakeOnce) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto order = audit_intent();
  order.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = order.stake;
  state.fills = 1;
  {
    Ledger ledger(path, 100 * U, true);
    ledger.register_intent(order, nullptr, state);
    ledger.status(order.id, "no_match");
    ledger.release_missed_spend(order.id);
    ledger.release_fill_slot(order.id);
    EXPECT_EQ(ledger.canary.spend, 0);
    EXPECT_EQ(ledger.canary.fills, 0);
    ledger.status(order.id, "matched", 2 * U, U);
    EXPECT_EQ(ledger.canary.spend, order.stake);
    EXPECT_EQ(ledger.canary.fills, 1);
    ASSERT_TRUE(ledger.fill("late-confirmation", order.id, 2 * U, U, 35000));
    ledger.status(order.id, "no_match");
    ledger.release_missed_spend(order.id);
    EXPECT_EQ(ledger.canary.spend, order.stake);
    EXPECT_FALSE(ledger.orders.at(order.id).canary_spend_released);
  }
  Ledger recovered(path, 100 * U, true);
  EXPECT_EQ(recovered.canary.spend, order.stake);
  EXPECT_EQ(recovered.canary.fills, 1);
}
TEST(AuditLedger, RefundsAndLateFillsCannotChangeAnotherSessionsSpendBudget) {
  AuditDirectory dir;
  Ledger ledger(dir.path / "ledger.json", 100 * U, true);
  auto old = audit_intent("old"), refunded = audit_intent("old-refunded"), current = audit_intent("current");
  old.fill_slot = refunded.fill_slot = current.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = old.stake;
  state.fills = 1;
  ledger.register_intent(old, nullptr, state);
  state.spend += refunded.stake;
  ++state.fills;
  ledger.register_intent(refunded, nullptr, state);
  ledger.status(refunded.id, "no_match");
  ledger.release_missed_spend(refunded.id);
  EXPECT_EQ(ledger.canary.spend, old.stake);
  state.started = 2002;
  state.spend = 8 * U;
  state.fills = 1;
  ledger.register_intent(current, nullptr, state);
  ledger.status(old.id, "no_match");
  ledger.release_missed_spend(old.id);
  EXPECT_EQ(ledger.canary.spend, 8 * U);
  ledger.status(refunded.id, "matched", 2 * U, U);
  ASSERT_TRUE(ledger.fill("old-confirmation", refunded.id, 2 * U, U, 35000));
  EXPECT_EQ(ledger.canary.spend, 8 * U);
  EXPECT_EQ(ledger.canary.fills, 1);
}
TEST(AuditLedger, RecoveryRestoresSpendAfterACrashFollowingPositiveEvidence) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto order = audit_intent();
  order.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = order.stake;
  state.fills = 1;
  {
    Ledger ledger(path, 100 * U, true);
    ledger.register_intent(order, nullptr, state);
    ledger.status(order.id, "no_match");
    ledger.release_missed_spend(order.id);
    ledger.release_fill_slot(order.id);
    ledger.snapshot();
  }
  {
    DurableLog log(path.string() + ".journal");
    log.replay([](auto, const auto &) {});
    log.append({{"type", "status"}, {"id", order.id}, {"status", "matched"},
                {"shares", 2 * U}, {"cost", U}});
  }
  {
    Ledger recovered(path, 100 * U, true);
    EXPECT_EQ(recovered.canary.spend, order.stake);
    EXPECT_EQ(recovered.canary.fills, 1);
    EXPECT_FALSE(recovered.orders.at(order.id).canary_spend_released);
  }
  Ledger again(path, 100 * U, true);
  EXPECT_EQ(again.canary.spend, order.stake);
  EXPECT_EQ(again.canary.fills, 1);
}
TEST(AuditMoney, RoundedVenueQuantitiesRejectMicroUnitOverflow) {
  EXPECT_THROW(venue_units(Json("9223372036854.7758075")), std::overflow_error);
  EXPECT_THROW(venue_units(Json("-9223372036854.7758085")), std::overflow_error);
  EXPECT_EQ(venue_units(Json("9223372036854.7758074")), std::numeric_limits<Units>::max());
  EXPECT_EQ(venue_units(Json("-9223372036854.7758084")), std::numeric_limits<Units>::min());
  EXPECT_EQ(venue_units(Json("9223372036854.7758065")), std::numeric_limits<Units>::max());
  EXPECT_EQ(venue_units(Json("-9223372036854.7758075")), std::numeric_limits<Units>::min());
}
TEST(AuditLedger, LegacySnapshotRefundMigratesWithoutLosingLateSpendRestoration) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto order = audit_intent();
  order.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = order.stake;
  state.fills = 1;
  {
    Ledger ledger(path, 100 * U, true);
    ledger.register_intent(order, nullptr, state);
    ledger.status(order.id, "no_match");
    state.spend = 0;
    ledger.set_canary(state); // The original main-branch refund WAL shape.
    ledger.release_fill_slot(order.id);
    auto snapshot = ledger.json();
    snapshot["format"] = "sniper-native-ledger-1";
    snapshot.erase("sha256");
    for (auto &intent : snapshot["orders"])
      intent.erase("canary_spend_released");
    atomic_file(path, snapshot.dump());
  }
  {
    Ledger upgraded(path, 100 * U, true);
    EXPECT_EQ(upgraded.canary.spend, 0);
    EXPECT_TRUE(upgraded.orders.at(order.id).canary_spend_released);
    upgraded.status(order.id, "matched", 2 * U, U);
    EXPECT_EQ(upgraded.canary.spend, order.stake);
    upgraded.snapshot();
  }
  Ledger again(path, 100 * U, true);
  EXPECT_EQ(again.canary.spend, order.stake);
  EXPECT_FALSE(again.orders.at(order.id).canary_spend_released);
}
TEST(AuditLedger, LegacyPositiveEvidenceAlreadyInSnapshotRestoresSpendOnlyOnce) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto order = audit_intent();
  order.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = order.stake;
  state.fills = 1;
  {
    Ledger ledger(path, 100 * U, true);
    ledger.register_intent(order, nullptr, state);
    ledger.status(order.id, "no_match");
    state.spend = 0;
    ledger.set_canary(state);
    ledger.release_fill_slot(order.id);
    // Older code did not attribute the refund or restore spend on this update.
    ledger.status(order.id, "matched", 2 * U, U);
    EXPECT_EQ(ledger.canary.spend, 0);
    ledger.snapshot();
  }
  {
    Ledger upgraded(path, 100 * U, true);
    EXPECT_EQ(upgraded.canary.spend, order.stake);
  }
  Ledger again(path, 100 * U, true);
  EXPECT_EQ(again.canary.spend, order.stake);
}
TEST(AuditLedger, LegacyRefundOfAnotherSessionsBudgetRefusesCurrentRecovery) {
  AuditDirectory dir;
  auto path = dir.path / "ledger.json";
  auto old = audit_intent("old"), current = audit_intent("current");
  old.fill_slot = current.fill_slot = true;
  CanaryState state;
  state.started = 1001;
  state.spend = old.stake;
  state.fills = 1;
  {
    Ledger ledger(path, 100 * U, true);
    ledger.register_intent(old, nullptr, state);
    state.started = 2002;
    state.spend = 10 * U;
    ledger.register_intent(current, nullptr, state);
    ledger.status(old.id, "no_match");
    state.spend -= old.stake;
    ledger.set_canary(state);
    ledger.snapshot();
  }
  EXPECT_THROW((Ledger(path, 100 * U, true)), std::runtime_error);
}
