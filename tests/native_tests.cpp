#include "sniper/math.hpp"
#include "sniper/paper.hpp"
#include "sniper/strategy.hpp"
#include "sniper/venue.hpp"
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace sniper;
static Json fixture(const char *name) {
  auto base = std::filesystem::read_symlink("/proc/self/exe")
                  .parent_path()
                  .parent_path() /
              "tests" / "fixtures";
  if (!std::filesystem::exists(base / name))
    base = FIXTURE_DIR;
  std::ifstream f(base / name);
  if (!f)
    throw std::runtime_error("missing test fixture");
  Json j;
  f >> j;
  return j;
}
static std::filesystem::path tempdir() {
  auto p = std::filesystem::temp_directory_path() /
           ("sniper-native-test-" + std::to_string(getpid()) + "-" +
            std::to_string(mono_ns()));
  std::filesystem::create_directories(p);
  return p;
}
static Intent intent() {
  Intent i;
  i.id = "native-test-order";
  i.condition = "0x" + std::string(64, '1');
  i.token = "123";
  i.up = "123";
  i.down = "456";
  i.side = "UP";
  i.slug = "test";
  i.status = "sent";
  i.stake = 5 * U;
  i.reserve = 5350000;
  i.target = 10 * U;
  i.limit = 500000;
  i.fee_rate = 70000;
  return i;
}
TEST(Crypto, KeccakKnownAnswers) {
  EXPECT_EQ(
      hex(keccak(std::string_view{})),
      "0xc5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
  EXPECT_EQ(
      hex(keccak("abc")),
      "0x4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}
TEST(Crypto, ActualSdkSignaturesAndWireBytes) {
  auto fixtures = fixture("sdk_vectors.json");
  Signer signer(
      "0x0000000000000000000000000000000000000000000000000000000000000001");
  ASSERT_TRUE(signer.self_test());
  for (auto &r : fixtures.at("signing_vectors")) {
    UnsignedOrder o;
    o.salt = r.at("salt");
    o.maker = o.signer = r.at("signer");
    o.token = r.at("token_id");
    o.exchange = r.at("exchange");
    o.maker_amount = r.at("maker_amount");
    o.taker_amount = r.at("taker_amount");
    o.signature_type = r.at("signature_type");
    o.timestamp = r.at("timestamp");
    auto d = order_digest(o);
    EXPECT_EQ(hex(d), r.at("digest").get<std::string>());
    auto sig = signer.sign(d);
    EXPECT_EQ(sig, r.at("signature").get<std::string>());
    EXPECT_EQ(signer.recover(d, sig), signer.address());
    EXPECT_EQ(serialize_order(o, sig, "offline-key"),
              r.at("body").get<std::string>());
  }
}
TEST(Crypto, NativeAuthHmacAndWalletDerivation) {
  auto r = fixture("auth_vectors.json");
  Signer s(
      "0x0000000000000000000000000000000000000000000000000000000000000001");
  auto digest = auth_digest(s.address(), r.at("timestamp"));
  EXPECT_EQ(hex(digest), r.at("digest").get<std::string>());
  EXPECT_EQ(s.sign(digest), r.at("signature").get<std::string>());
  EXPECT_EQ(wallet_for(s.address(), 1), r.at("proxy").get<std::string>());
  EXPECT_EQ(wallet_for(s.address(), 2), r.at("safe").get<std::string>());
  EXPECT_EQ(hmac_signature(r.at("hmac_secret").get<std::string>(), 1900000000,
                           "POST", "/order",
                           r.at("hmac_body").get<std::string>()),
            r.at("hmac_signature").get<std::string>());
}
TEST(Amounts, EverySdkGridVector) {
  auto f = fixture("amount_vectors.json");
  for (auto &r : f.at("cases")) {
    auto result = market_amounts(units(r[2].get<std::string>()),
                                 units(r[1].get<std::string>()),
                                 units(r[0].get<std::string>()));
    ASSERT_EQ(result.maker, r[3].get<Units>()) << r;
    ASSERT_EQ(result.taker, r[4].get<Units>()) << r;
  }
}
TEST(Money, MinimumsAndExactBoundaries) {
  for (auto [p, minimum, n9, n13] :
       std::vector<std::tuple<Units, Units, Units, Units>>{
           {500000, 5, 9, 13},
           {501000, 10, 0, 10},
           {150000, 7, 9, 13},
           {70000, 15, 9, 13},
           {330000, 5, 9, 13},
           {955000, 6, 8, 12}}) {
    EXPECT_EQ(minimum_entry_shares(p), minimum);
    EXPECT_EQ(cent_compatible_shares(9, p), n9);
    EXPECT_EQ(cent_compatible_shares(13, p), n13);
  }
  EXPECT_EQ(reservation(4860000, 70000), 5200200);
  EXPECT_EQ(shares_within_budget(10 * U, 540000, 70000), 17);
  EXPECT_EQ(shares_within_budget(100 * U, 540000, 70000), 173);
  EXPECT_EQ(shares_within_budget(3 * U, 510000, 70000), 5);
  EXPECT_EQ(units(".5010000"), 501000);
  EXPECT_THROW(units("nan"), std::exception);
  EXPECT_THROW(units("1.0000001"), std::exception);
}
TEST(Math, IndependentReferenceVectors) {
  auto f = fixture("math_vectors.json");
  for (auto &r : f.at("cases")) {
    auto fair = anchored_fair(r[0], r[1], r[2], r[3]);
    ASSERT_NEAR(fair, r[4].get<double>(), 1e-12);
    ASSERT_EQ(fair_limit(fair, .01, r[5], .07), r[6].get<double>());
    auto k = kelly_shares(fair, r[7], .07, r[8], r[9], r[12], r[10], r[11]);
    ASSERT_EQ(k.first, r[13].get<double>());
    ASSERT_NEAR(k.second, r[14].get<double>(), 1e-12);
  }
}
TEST(Math, HybridExactDrawdown) {
  hybrid::Policy p;
  for (auto [sigma, equity, peak, want] :
       std::vector<std::tuple<double, double, double, double>>{
           {.0001, 100, 100, .25},
           {.0002, 100, 100, .125},
           {.00005, 100, 100, .25},
           {.0001, 90, 100, .125},
           {.0001, 80, 100, 0},
           {.0002, 95, 100, .09375},
           {.0001, 98.76, 123.45, 0}})
    EXPECT_NEAR(hybrid::sizing_factors(p, sigma, equity, peak, .25)
                    .effective_fraction,
                want, 1e-12);
  EXPECT_TRUE(hybrid::at_drawdown_stop(p, 98.76, 123.45));
}
TEST(Trigger, GoldenTicksGapsAndThreshold) {
  Trigger t(.5, 250);
  for (auto tick : std::vector<Tick>{{0, 100000}, {100, 100000}, {200, 100002}})
    EXPECT_FALSE(t.update(tick.first, tick.second).first);
  auto up = t.update(260, 100005);
  EXPECT_EQ(up.first, 1);
  EXPECT_NEAR(up.second, .5, 1e-9);
  EXPECT_EQ(t.update(400, 100006).first, 1);
  EXPECT_FALSE(t.update(3000, 100020).first);
  EXPECT_FALSE(t.update(3100, 100020).first);
  auto down = t.update(3300, 99990);
  EXPECT_EQ(down.first, -1);
  EXPECT_NEAR(down.second, -2.9994, 1e-6);
  t.update(NAN, 1);
  EXPECT_TRUE(t.q.values.empty());
}
TEST(Volatility, WarmupFloorAndBoundedHistory) {
  Volatility v;
  for (int i = 0; i < 60; ++i)
    v.observe(i, 100000);
  EXPECT_FALSE(v.vol());
  v.observe(60, 100000);
  ASSERT_TRUE(v.vol());
  EXPECT_EQ(*v.vol(), .00002);
  for (int i = 61; i < 100000; ++i)
    v.observe(i, 100000 * std::exp((i % 2) * .0001));
  EXPECT_LE(v.incs.size(), 301);
  EXPECT_NEAR(*v.vol(), .0001, 1e-12);
}
TEST(Money, FixedSixMatchesPrintfRounding) {
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> wide(-1e6, 1e6), unit(0, 1);
  std::vector<double> cases{0.0, 0.5, 0.07, 0.1, 1e-7, 4.9999995e-7, 0.0000005,
                            123.4567895, 0.9999995, 1000.0, -0.0000004};
  for (int i = 0; i < 20000; ++i)
    cases.push_back(i % 2 ? wide(rng) : unit(rng));
  for (double x : cases) {
    char want[64];
    std::snprintf(want, sizeof want, "%.6f", x);
    ASSERT_EQ(fixed6(x), want) << x;
  }
  EXPECT_EQ(as_units(Json(0.07)), 70000);
  EXPECT_THROW(fixed6(NAN), std::exception);
}
TEST(Ledger, DurableIntentConfirmedFillIdempotenceAndSettlement) {
  auto path = tempdir() / "ledger.json";
  {
    Ledger l(path, 100 * U, false);
    auto i = intent();
    l.register_intent(i, nullptr, {});
    EXPECT_EQ(l.reserved(), 5350000);
    l.status(i.id, "matched", 6 * U, 3 * U);
    ASSERT_TRUE(l.fill("trade-1", i.id, 6 * U, 3 * U, 105000));
    EXPECT_FALSE(l.fill("trade-1", i.id, 6 * U, 3 * U, 105000));
    EXPECT_EQ(l.holding("123"), 6 * U);
    EXPECT_EQ(l.risk_shares("123"), 6 * U);
    EXPECT_EQ(l.reserved(), 0);
    l.snapshot();
  }
  {
    Ledger l(path, 100 * U, false);
    EXPECT_EQ(l.holding("123"), 6 * U);
    EXPECT_FALSE(l.fill("trade-1", "native-test-order", 6 * U, 3 * U, 105000));
    ASSERT_TRUE(l.settle("0x" + std::string(64, '1'), {{"123", U}}));
    EXPECT_FALSE(l.settle("0x" + std::string(64, '1'), {{"123", U}}));
    EXPECT_EQ(l.realized, 2895000);
    EXPECT_EQ(l.cash, 102895000);
    l.snapshot();
  }
  std::filesystem::remove_all(path.parent_path());
}
TEST(Ledger, UnauthorizedAndOverCapFillRefused) {
  auto p = tempdir() / "ledger.json";
  {
    Ledger l(p, 100 * U, false);
    EXPECT_THROW(l.fill("t", "unknown", U, U, 0), std::exception);
    auto i = intent();
    l.register_intent(i, nullptr, {});
    EXPECT_THROW(l.fill("t", i.id, U, 600000, 0), std::exception);
    EXPECT_EQ(l.cash, 100 * U);
  }
  std::filesystem::remove_all(p.parent_path());
}
TEST(Ledger, ProcessCrashAfterIntentNeverLosesAuthorization) {
  auto p = tempdir() / "ledger.json";
  auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    Ledger ledger(p, 100 * U, false);
    ledger.register_intent(intent(), nullptr, {});
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  ASSERT_EQ(WEXITSTATUS(status), 0);
  {
    Ledger ledger(p, 100 * U, false);
    ASSERT_EQ(ledger.orders.size(), 1);
    EXPECT_TRUE(ledger.awaiting("123"));
    EXPECT_EQ(ledger.reserved(), 5350000);
  }
  std::filesystem::remove_all(p.parent_path());
}
TEST(Ledger, TornOrCorruptedJournalRefusesRestart) {
  auto p = tempdir() / "ledger.json";
  {
    Ledger ledger(p, 100 * U, false);
    ledger.register_intent(intent(), nullptr, {});
  }
  std::ofstream f(p.string() + ".journal", std::ios::app);
  f << "{broken";
  f.close();
  EXPECT_THROW((Ledger(p, 100 * U, false)), std::exception);
  std::filesystem::remove_all(p.parent_path());
}
TEST(Risk, CanaryPausesThenIncrementsOnlyOnce) {
  Config c;
  CanaryState state;
  state.orders = 40;
  EXPECT_EQ(canary_refusal(c, state, 10, 5 * U, 5 * U, "m", 100),
            "canary budget: orders");
  EXPECT_EQ(state.pause_until, 400);
  EXPECT_FALSE(canary_refusal(c, state, 10, 5 * U, 5 * U, "m", 200).empty());
  EXPECT_EQ(state.increments["orders"], 0);
  EXPECT_TRUE(canary_refusal(c, state, 10, 5 * U, 5 * U, "m", 400).empty());
  EXPECT_EQ(state.increments["orders"], 1);
  EXPECT_TRUE(canary_refusal(c, state, 10, 5 * U, 5 * U, "m", 401).empty());
  EXPECT_EQ(state.increments["orders"], 1);
}
TEST(Safety, PaperHasOneNativeSubmissionFirewall) {
  boost::asio::io_context io;
  boost::asio::ssl::context tls(boost::asio::ssl::context::tls_client);
  std::atomic<std::uint64_t> blocked{0};
  H2Client h(io, tls, false, blocked, {});
  EXPECT_THROW(h.post("fake", "{}", {}), std::exception);
  EXPECT_EQ(blocked, 1);
  RestClient rest(false, &blocked);
  EXPECT_THROW(
      rest.request("POST", "https://clob.polymarket.com/order", {}, "{}"),
      std::exception);
  EXPECT_EQ(blocked, 2);
}
TEST(Clock, RejectsSlowSamplesAndAdvancesOnMonotonicTime) {
  ServerClock c;
  EXPECT_THROW(c.now(0), std::exception);
  EXPECT_FALSE(c.sample(1000, 1000, 1001, 1000, 1));
  ASSERT_TRUE(c.sample(1000, 1000, 1000.1, 100, 1));
  EXPECT_EQ(c.now(2), 1001);
  EXPECT_THROW(c.now(62), std::exception);
}
TEST(Discovery, PublicVenueResponsesAcceptMissingNegativeRiskFlag) {
  auto event = fixture("public_event.json");
  auto round = parse_round(event, 1793963100);
  ASSERT_TRUE(round);
  auto rules = parse_rules(fixture("public_rules.json"),
                           fixture("public_book.json"), round->up);
  EXPECT_TRUE(rules.loaded);
  EXPECT_FALSE(rules.neg);
  EXPECT_EQ(rules.rate, 70000);
  EXPECT_EQ(rules.exponent, 1);
  EXPECT_EQ(rules.tick, 10000);
  EXPECT_EQ(rules.min_shares, 5 * U);
  event["markets"][0]["cryptoMarketConfig"]["twapLookbackSeconds"] = 30;
  EXPECT_FALSE(parse_round(event, 1793963100));
}
TEST(Ledger, PendingPartialFillBlocksSettlementAcrossRestart) {
  auto p = tempdir() / "ledger.json";
  auto i = intent();
  {
    Ledger l(p, 100 * U, false);
    l.register_intent(i, nullptr, {});
    l.status(i.id, "matched", 6 * U, 3 * U);
    l.fill("partial-1", i.id, 2 * U, U, 35000);
    EXPECT_TRUE(l.awaiting(i.token));
    EXPECT_EQ(l.reserved(), 4315000);
    EXPECT_FALSE(l.settle(i.condition, {{i.token, U}}));
    l.snapshot();
  }
  {
    Ledger l(p, 100 * U, false);
    EXPECT_FALSE(l.settle(i.condition, {{i.token, U}}));
    l.fill("partial-2", i.id, 4 * U, 2 * U, 70000);
    EXPECT_FALSE(l.awaiting(i.token));
    EXPECT_EQ(l.risk_shares(i.token), 6 * U);
    EXPECT_EQ(l.reserved(), 0);
    EXPECT_TRUE(l.settle(i.condition, {{i.token, U}}));
    EXPECT_THROW(l.fill("late", i.id, U, 500000, 17500), std::exception);
  }
  std::filesystem::remove_all(p.parent_path());
}
TEST(Ledger, DurableDrawdownPauseDoesNotRebaseCapital) {
  auto p = tempdir() / "ledger.json";
  {
    Ledger l(p, 100 * U, false);
    l.drawdown_pause(12345);
    EXPECT_EQ(l.reference_cash, 100 * U);
    EXPECT_EQ(l.high_water, 100 * U);
    l.snapshot();
  }
  {
    Ledger l(p, 100 * U, false);
    EXPECT_EQ(l.dd_pause_until, 12345);
    EXPECT_EQ(l.reference_cash, 100 * U);
    l.risk_rebase(80 * U);
    EXPECT_EQ(l.dd_pause_until, 0);
    EXPECT_EQ(l.high_water, 80 * U);
  }
  std::filesystem::remove_all(p.parent_path());
}
TEST(Ledger, EmptyFillSlotReleaseIsIdempotentAndDurable) {
  auto p = tempdir() / "ledger.json";
  auto i = intent();
  i.fill_slot = true;
  CanaryState c;
  c.fills = 1;
  {
    Ledger l(p, 100 * U, false);
    l.register_intent(i, nullptr, c);
    l.release_fill_slot(i.id);
    EXPECT_EQ(l.canary.fills, 0);
    auto entries = l.entries;
    l.release_fill_slot(i.id);
    EXPECT_EQ(l.entries, entries);
  }
  {
    Ledger l(p, 100 * U, false);
    EXPECT_EQ(l.canary.fills, 0);
    EXPECT_FALSE(l.orders.at(i.id).fill_slot);
  }
  std::filesystem::remove_all(p.parent_path());
}
TEST(Config, RejectsAmbiguousModesAndInvalidDuration) {
  auto load = [](std::vector<std::string> a) {
    std::vector<char *> args;
    for (auto &s : a)
      args.push_back(s.data());
    return Config::load(static_cast<int>(args.size()), args.data());
  };
  EXPECT_THROW(load({"sniper-cpp", "--live", "--paper"}), std::exception);
  EXPECT_THROW(load({"sniper-cpp", "--run-for", "1junk"}), std::exception);
  EXPECT_THROW(load({"sniper-cpp", "--run-for", "nan"}), std::exception);
  EXPECT_THROW(load({"sniper-cpp", "--config", "/nonexistent/sniper-test/.env"}),
               std::exception);
}
TEST(Config, RuntimeSettingsCountAsUsedAndAreChecked) {
  auto dir = tempdir();
  auto path = dir / ".env";
  auto load = [&](const std::string &text) {
    {
      std::ofstream out(path);
      out << text << '\n';
    }
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                           std::filesystem::perms::owner_write);
    std::vector<std::string> a{"sniper-cpp", "--config", path.string(),
                               "--check"};
    std::vector<char *> args;
    for (auto &s : a)
      args.push_back(s.data());
    return Config::load(static_cast<int>(args.size()), args.data());
  };
  auto notices = load("MAX_ALLOWED_SPREAD=0.1").notices;
  EXPECT_EQ(std::count(notices.begin(), notices.end(),
                       std::string("unused setting: MAX_ALLOWED_SPREAD")),
            0);
  EXPECT_THROW(load("MAX_ALLOWED_SPREAD=abc"), std::exception);
  std::filesystem::remove_all(dir);
}
TEST(DurableLog, RefusesMoreRowsAfterAFailedWrite) {
  // Every write to /dev/full fails with "no space left on device".
  DurableLog durable("/dev/full");
  Json row{{"type", "test"}};
  EXPECT_THROW(durable.append(row), std::runtime_error);
  std::string second;
  try {
    durable.append(row);
  } catch (const std::runtime_error &e) {
    second = e.what();
  }
  EXPECT_NE(second.find("an earlier write failed"), std::string::npos);
  EXPECT_EQ(durable.sequence(), 0u);
}
TEST(Journal, OversizedRecordFailsClosed) {
  auto p = tempdir() / "audit.jsonl";
  {
    Journal j(p);
    EXPECT_TRUE(j.write({{"result", "start"}}));
    EXPECT_FALSE(j.write({{"payload", std::string(9000, 'x')}}));
    EXPECT_TRUE(j.failed());
    EXPECT_FALSE(j.write({{"result", "later"}}));
    j.close();
  }
  std::filesystem::remove_all(p.parent_path());
}
TEST(Ledger, DurableSignedProofExcludesApiCredentialEnvelope) {
  auto p = tempdir() / "ledger.json";
  SignedOrder proof;
  proof.body = Json{
      {"order", {{"tokenId", "123"}, {"signature", "dummy-signature"}}},
      {"owner", "DUMMY_API_CREDENTIAL_MUST_NOT_BE_LOGGED"},
      {"orderType",
       "FAK"}}.dump();
  {
    Ledger l(p, 100 * U, false);
    l.register_intent(intent(), &proof, {});
  }
  std::ifstream in(p.string() + ".journal");
  std::string raw((std::istreambuf_iterator<char>(in)), {});
  EXPECT_EQ(raw.find("DUMMY_API_CREDENTIAL_MUST_NOT_BE_LOGGED"),
            std::string::npos);
  auto event = Json::parse(raw).at("event");
  EXPECT_EQ(event.at("signed_order").at("signature"), "dummy-signature");
  EXPECT_EQ(event.at("wire_sha256"), sha256(proof.body));
  std::filesystem::remove_all(p.parent_path());
}

// ---------------------------------------------------------------- reference
// ports (sniper_mode / feeds.book / paper_trade @ 772f2eb). The replay and
// differential suites in tools/replay compare these against the Python code
// itself; these tests pin the behaviours the audit found the old engine got
// wrong.
TEST(Freshness, ReferenceReasonsAndOrder) {
  Freshness f;
  f.enabled = true;
  EXPECT_EQ(f.why_not(0), "coinbase disconnected");
  f.on_connect();
  EXPECT_EQ(f.why_not(0), "coinbase age unknown");
  f.note_trade(100000, 100050, 1);
  EXPECT_TRUE(f.why_not(1).empty());
  f.note_trade(100100, 101500, 2);
  EXPECT_EQ(f.why_not(2), "coinbase feed behind");
  f.note_message(2, 21);
  EXPECT_EQ(f.why_not(2), "coinbase receive backlog");
  f.mode = "age";
  f.note_trade(103000, 103010, 3); // fresh again: the burst is let through
  EXPECT_TRUE(f.why_not(3).empty());
}
static Json lv(const char *p, const char *q) { return {{"price", p}, {"size", q}}; }
struct Clocked {
  double mono = 100, wall = 1793966400;
  BookState book{8.0, 30.0, [this] { return mono; }, [this] { return wall; },
                 [this] { return wall; }};
  Clocked() {
    book.connected = true;
    book.set_active({"123", "456"});
  }
  std::string ts(double dt = 0) {
    return std::to_string(static_cast<long long>((wall + dt) * 1000));
  }
  bool snap(const char *bid, const char *ask, double dt = 0) {
    SnapshotOptions o;
    o.ts_ms = ts(dt);
    o.require_exchange_ts = true;
    return book.apply_snapshot("123", Json::array({lv(bid, "20")}),
                               Json::array({lv(ask, "20")}), o);
  }
};
TEST(Book, AtomicDeltasLockedBookAndZeroSizeSnapshot) {
  Clocked c;
  ASSERT_TRUE(c.snap(".49", ".51"));
  auto g = c.book.view("123").generation;
  auto change = [](const char *p, const char *q, const char *side) {
    return Json{{"asset_id", "123"}, {"price", p}, {"size", q}, {"side", side}};
  };
  // moving up a level crosses only mid-message: applied atomically
  Json up{{"event_type", "price_change"},
          {"timestamp", c.ts(0.1)},
          {"price_changes", Json::array({change(".52", "30", "BUY"),
                                         change(".53", "30", "SELL"),
                                         change(".49", "0", "BUY"),
                                         change(".51", "0", "SELL")})}};
  c.mono += .1;
  c.wall += .1;
  EXPECT_FALSE(dispatch_market_event(c.book, up));
  auto v = c.book.view("123");
  EXPECT_EQ(v.status, LIVE);
  EXPECT_EQ(v.generation, g);
  EXPECT_DOUBLE_EQ(*v.best_bid(), .52);
  EXPECT_DOUBLE_EQ(*v.best_ask(), .53);
  // a locked book (bid == ask) is crossed for the reference: desync
  Json lock{{"event_type", "price_change"},
            {"timestamp", c.ts(0.1)},
            {"price_changes", Json::array({change(".53", "5", "BUY")})}};
  EXPECT_TRUE(dispatch_market_event(c.book, lock));
  EXPECT_EQ(c.book.view("123").status, UNSYNCED);
  EXPECT_EQ(c.book.view("123").generation, g + 1);
  // a zero-size level is an invalid snapshot, not an empty level
  SnapshotOptions o;
  o.ts_ms = c.ts(0.2);
  EXPECT_FALSE(c.book.apply_snapshot("123", Json::array({lv(".4", "0")}),
                                     Json::array({lv(".6", "1")}), o));
}
TEST(Book, ArrivalBookIsStrictUnlessQuietIsOptedIn) {
  Clocked c;
  ASSERT_TRUE(c.snap(".49", ".51"));
  double venue = (c.wall + .5) * 1000; // the order meets the venue later
  auto [none, why] = c.book.view_at("123", venue);
  EXPECT_FALSE(none);
  EXPECT_EQ(why.why, "the feed has not yet passed that time");
  auto [quiet, info] = c.book.view_at("123", venue, true);
  ASSERT_TRUE(quiet);
  EXPECT_EQ(info.evidence, "quiet");
  // any later message, even another token's, proves the arrival state
  c.book.note_exchange_ts(Json(c.ts(0.8)));
  auto [proven, pinfo] = c.book.view_at("123", venue);
  ASSERT_TRUE(proven);
  EXPECT_DOUBLE_EQ(*proven->best_ask(), .51);
  // a REST refresh is not stream-ordered history
  SnapshotOptions r;
  r.ts_ms = c.ts(0.9);
  r.source = "rest";
  r.require_exchange_ts = true;
  c.mono += 2;
  ASSERT_TRUE(c.book.apply_snapshot("123", Json::array({lv(".48", "9")}),
                                    Json::array({lv(".50", "9")}), r));
  c.book.note_exchange_ts(Json(c.ts(1.0)));
  EXPECT_EQ(c.book.view_at("123", (c.wall + .95) * 1000).second.why,
            "REST snapshot is not stream-ordered history");
}
TEST(Paper, DecimalFakFokAndShareSizedReference) {
  BookSnapshot b;
  b.token_id = "t";
  b.asks = {{py::D(".5"), py::D("20")}};
  MarketRules rules;
  rules.fee_rate = py::D("0.07");
  rules.tick_size = py::D("0.01");
  rules.min_order_size = py::D("5");
  auto q = estimate_fok(b, py::D("4.86"), py::D(.54), rules, py::D(.05));
  EXPECT_EQ(q.shares.str(), "9.72000"); // Python: Decimal("9.72000")
  EXPECT_EQ(q.fee.str(), "0.17010");
  b.asks = {{py::D(".5"), py::D("2")}};
  EXPECT_THROW(estimate_fok(b, py::D(5), py::D(.54), rules, py::D(.05)),
               PaperRejected);
  auto partial = estimate_fak(b, py::D(5), py::D(.54), rules, py::D(.05));
  EXPECT_EQ(partial.shares.str(), "2");
  try {
    estimate_fak_shares(b, py::D(9), py::D(.49), rules, py::D(.05));
    FAIL();
  } catch (const PaperRejected &e) {
    EXPECT_STREQ(e.what(), "FAK no-fill: nothing at or below 0.49");
  }
  EXPECT_EQ(paper_outcome(false, "FAK no-fill: nothing at or below 0.49",
                          Json{{"cap", .49}, {"arrival", {{"best_ask", .5}}}}),
            "PRICE_CAP_MISS");
}
// Expected values come from the reference's own LiveExecution.prepare
// (sniper_live.py at 772f2eb) run on the same books with stub state.
TEST(Live, PresendCheckMatchesReferencePrepare) {
  struct Case {
    const char *name;
    PxLevels asks, bids;
    double cap;
    long long shares;
    const char *tick, *order_tick;
    std::optional<long long> venue_min;
    const char *refused = nullptr; // nullptr: sized
    double spend = 0, price = 0, reserve = 0, est_shares = 0, est_price = 0,
           est_fee = 0;
  };
  std::vector<Case> cases = {
      {"ok", {{.47, 100}, {.48, 50}, {.6, 10}}, {{.45, 30}, {.4, 5}}, .5, 10,
       "0.01", "0.01", std::nullopt, nullptr, 5.0, .5, 5.35,
       10.638297872340425, .47, .1855},
      {"clamped", {{.9, 40}}, {{.88, 10}}, .98, 10, "0.01", "0.01",
       std::nullopt, nullptr, 9.5, .95, 10.165, 10.555555555555555, .9, .0665},
      {"spread", {{.47, 100}}, {{.2, 30}}, .5, 10, "0.01", "0.01",
       std::nullopt, "spread exceeds the limit"},
      {"one-sided", {{.47, 100}}, {}, .5, 10, "0.01", "0.01", std::nullopt,
       "one-sided book"},
      {"rules-changed", {{.47, 100}}, {{.45, 30}}, .5, 10, "0.01", "0.001",
       std::nullopt, "market rules changed; background refresh requested"},
      {"below-min", {{.47, 100}}, {{.45, 30}}, .5, 3, "0.01", "0.01",
       std::nullopt, "order size 3 shares is below venue minimum 5"},
      {"override-min", {{.47, 100}}, {{.45, 30}}, .5, 3, "0.01", "0.01", 3,
       nullptr, 1.5, .5, 1.605, 3.1914893617021276, .47, .05565},
      {"not-cent", {{.471, 100}}, {{.45, 30}}, .4759, 7, "0.001", "0.001",
       std::nullopt, "7 shares at 0.475 is not a whole-cent amount"},
      {"under-1usd", {{.09, 100}}, {{.08, 30}}, .1, 5, "0.01", "0.01",
       std::nullopt,
       "invalid amount for a marketable BUY order ($0.50), min size: $1"},
      {"nothing-under-cap", {{.6, 100}}, {{.45, 30}}, .5, 10, "0.01", "0.01",
       std::nullopt, "FAK no-fill: nothing at or below 0.5"},
      {"thin", {{.47, 3}, {.49, 2}, {.7, 100}}, {{.45, 30}}, .5, 10, "0.01",
       "0.01", std::nullopt, nullptr, 5.0, .5, 5.35, 5.0, .478, .0873},
      {"crossed", {{.47, 100}}, {{.47, 30}}, .5, 10, "0.01", "0.01",
       std::nullopt, "public order book is crossed or locked"},
  };
  double now = 1000.0, wall = 1793985300.0;
  for (auto &c : cases) {
    SCOPED_TRACE(c.name);
    MarketRules rules;
    rules.condition_id = "c";
    rules.fee_rate = py::D("0.07");
    rules.min_order_size = py::D("5");
    rules.tick_size = py::D(std::string_view(c.tick));
    Plan p;
    p.token = "u";
    p.cap = c.cap;
    p.shares = c.shares;
    if (c.venue_min)
      p.venue_min_shares = c.venue_min;
    p.view.token = "u";
    p.view.status = LIVE;
    p.view.asks = c.asks;
    p.view.bids = c.bids;
    p.view.updated_mono = now;
    p.view.exchange_ts_ms = static_cast<long long>(wall * 1000);
    p.view.tick_size = py::parse_float(c.tick);
    try {
      auto chk = live_presend_check(p, rules, py::D(std::string_view(c.order_tick)),
                                    py::D("5"), .95, .05, .25, now, wall);
      ASSERT_EQ(c.refused, nullptr) << "sized, the reference refused";
      EXPECT_EQ(chk.spend.to_double(), c.spend);
      EXPECT_EQ(chk.price.to_double(), c.price);
      EXPECT_EQ(hybrid::risk_total(chk.spend, rules.fee_rate, chk.quote.fee)
                    .to_double(),
                c.reserve);
      EXPECT_EQ(chk.quote.shares.to_double(), c.est_shares);
      EXPECT_EQ(chk.quote.average_price.to_double(), c.est_price);
      EXPECT_EQ(chk.quote.fee.to_double(), c.est_fee);
    } catch (const PaperRejected &e) {
      ASSERT_NE(c.refused, nullptr) << "refused: " << e.what();
      EXPECT_STREQ(e.what(), c.refused);
    }
  }
}
namespace {
// A warmed engine in live mode whose next tick is a +1.2 bp move; the live
// dispatcher is a stub that refuses or accepts.
struct LiveStubEnv : SniperEnv {
  double now = 1000, offset = 1793966400.0 + 100 - 1000;
  BookState books{8.0, 30.0, [this] { return now; }, [this] { return now + offset; },
                  [this] { return now + offset; }};
  MarketRules rules;
  std::vector<Json> rows;
  std::string refusal;
  bool halted = false;
  int dispatched = 0;
  LiveStubEnv() {
    rules.condition_id = "0xcond";
    rules.fee_rate = py::D("0.07");
    rules.tick_size = py::D("0.01");
    rules.min_order_size = py::D("5");
    rules.up_token_id = "111";
    rules.down_token_id = "222";
    rules.fetched_wall = wall();
    books.connected = true;
    books.set_active({"111", "222"});
  }
  double mono() override { return now; }
  double wall() override { return now + offset; }
  std::string up_token() override { return "111"; }
  std::string down_token() override { return "222"; }
  std::string condition_id() override { return "0xcond"; }
  std::optional<double> window_end() override { return wall() + 120.0; }
  BookState &book() override { return books; }
  bool live() override { return true; }
  std::optional<double> read_bankroll() override { return 1000.0; }
  const MarketRules *cached_rules(const std::string &, bool) override {
    return &rules;
  }
  double open_leg_shares(const std::string &, const std::string &) override {
    return 0;
  }
  double trade_window_seconds() override { return 300; }
  double min_seconds_to_expiry() override { return 10; }
  double min_buy_price() override { return .05; }
  double max_buy_price() override { return .95; }
  double own_fill_ttl_s() override { return 0; }
  bool has_recent_own_fill(const std::string &) override { return false; }
  std::optional<PxLevels> net_asks(const BookView &) override { return {}; }
  bool hybrid_initialized() override { return true; }
  long long journal_seq() override { return 0; }
  double open_basis() override { return 0; }
  Json hybrid_risk_view(std::optional<double>, std::optional<long long>,
                        bool) override {
    return Json{{"equity", 1000.0}, {"high_water", 1000.0}};
  }
  void journal(Json row) override { rows.push_back(std::move(row)); }
  bool dispatch_live(const std::shared_ptr<Plan> &p) override {
    ++dispatched;
    if (refusal.empty())
      return true;
    p->live_refusal = refusal;
    p->live_halted = halted;
    return false;
  }
  void quote(const char *token, const char *bid, const char *ask) {
    SnapshotOptions o;
    o.ts_ms = std::to_string(static_cast<long long>(wall() * 1000));
    o.require_exchange_ts = true;
    books.apply_snapshot(token, Json::array({{{"price", bid}, {"size", "500"}}}),
                         Json::array({{{"price", ask}, {"size", "500"}}}), o);
  }
};
std::string live_ticker(long long seq, double mid) {
  char buf[512];
  std::snprintf(buf, sizeof buf,
                R"({"type":"ticker","sequence":%lld,"product_id":"BTC-USD","price":"%.2f","best_bid":"%.2f","best_ask":"%.2f","time":"2026-11-06T00:00:00.000000Z","side":"buy","trade_id":%lld,"last_size":"0.001"})",
                seq, mid, mid - 0.005, mid + 0.005, seq);
  return buf;
}
} // namespace
TEST(Live, RefusedDispatchIsJournalledAsReferenceSkip) {
  LiveStubEnv env;
  SniperSettings s;
  s.cooldown_ms = 0;
  s.coinbase_conns = 1;
  Sniper sn(s, env);
  sn.cb_fresh.enabled = false;
  long long seq = 1;
  double mid = 60000;
  for (int i = 0; i < 70; ++i) {
    env.now += 1;
    mid *= (i % 2 ? 1.00004 : 0.99996);
    env.quote("111", ".48", ".50");
    env.quote("222", ".49", ".51");
    sn.on_coinbase(live_ticker(seq++, mid), env.now * 1000);
  }
  sn.refresh_bankroll(env.now);
  sn.refresh_hybrid_risk();
  auto fire = [&](bool up) {
    for (int k = 0; k < 4; ++k) {
      env.now += 1.0;
      env.quote("111", ".48", ".50");
      env.quote("222", ".49", ".51");
      sn.on_coinbase(live_ticker(seq++, mid), env.now * 1000);
    }
    env.now += 0.3;
    env.quote("111", ".48", ".50");
    env.quote("222", ".49", ".51");
    sn.on_coinbase(live_ticker(seq++, mid), env.now * 1000);
    sn.refresh_bankroll(env.now);
    sn.refresh_hybrid_risk();
    env.now += 0.01;
    mid *= up ? 1.00012 : 1 / 1.00012;
    sn.on_coinbase(live_ticker(seq++, mid), env.now * 1000);
  };
  env.refusal = "pre-send check: spread exceeds the limit";
  fire(true);
  ASSERT_EQ(env.dispatched, 1);
  EXPECT_EQ(sn.n["orders"], 0u); // decided, but not sent
  const Json *skip = nullptr;
  for (auto &r : env.rows)
    if (r.value("result", "") == "skipped" && r.value("reason", "") == "live refused")
      skip = &r;
  ASSERT_NE(skip, nullptr);
  EXPECT_EQ(skip->value("detail", ""), "pre-send check: spread exceeds the limit");
  EXPECT_FALSE(skip->contains("dec_cap")); // no decision-book fields: not sent
  // the side was released: the next signal reaches the dispatcher again
  env.refusal = "canary budget: spend";
  env.halted = true;
  env.now += 2.0; // past the skip coalescing second
  fire(true);
  ASSERT_EQ(env.dispatched, 2);
  bool halted_row = false;
  for (auto &r : env.rows)
    halted_row |= r.value("reason", "") == "live halted" &&
                  r.value("detail", "") == "canary budget: spend";
  EXPECT_TRUE(halted_row);
  env.refusal.clear();
  env.now += 2.0;
  fire(true);
  ASSERT_EQ(env.dispatched, 3);
  EXPECT_EQ(sn.n["orders"], 1u);
}
TEST(Settings, ReferenceDefaultsAndRefusals) {
  std::vector<std::string> notes;
  auto s = sniper_settings(std::map<std::string, std::string>{}, &notes);
  EXPECT_EQ(s.order_type, "FAK");
  EXPECT_EQ(s.order_sizing, "shares");
  EXPECT_FALSE(s.book_quiet);
  EXPECT_DOUBLE_EQ(s.net_default_ms, 14);
  EXPECT_FALSE(notes.empty()); // the paper/live comparison default is named
  try {
    sniper_settings(std::map<std::string, std::string>{{"SNIPER_K_BP", "0"},
                                                       {"SNIPER_SHARES", "4"}});
    FAIL();
  } catch (const std::invalid_argument &e) {
    EXPECT_STREQ(e.what(), "invalid SNIPER_* settings: SNIPER_K_BP=0 (must be > "
                           "0); SNIPER_SHARES=4 (venue minimum is 5)");
  }
  EXPECT_THROW(sniper_settings(std::map<std::string, std::string>{
                   {"SNIPER_FAIR_MARGIN", "nan"}}),
               std::invalid_argument);
  EXPECT_THROW(sniper_settings(std::map<std::string, std::string>{
                   {"SNIPER_DIR_ADD", "0.5"}}),
               std::invalid_argument);
}
TEST(Python, NumericsMatchCPython) {
  EXPECT_EQ(py::repr(1e-05), "1e-05");
  EXPECT_EQ(py::repr(1e16), "1e+16");
  EXPECT_EQ(py::repr(0.1 + 0.2), "0.30000000000000004");
  EXPECT_EQ(py::round(0.125, 2), 0.12);
  EXPECT_EQ(py::round(2.675, 2), 2.67);
  EXPECT_EQ(py::D(.55).str(), "0.55"); // no octal misread of "055"
  EXPECT_EQ((py::D(1) / py::D(3)).str(), "0.3333333333333333333333333333");
  EXPECT_EQ(py::quantize(py::D("2.675"), py::D("0.01"), py::Rounding::HalfUp).str(),
            "2.68");
  EXPECT_EQ(hybrid::minimum_entry_shares(py::D(.501)), 10);
  EXPECT_EQ(hybrid::cent_compatible_shares(py::D(13.0), py::D(.15)), 13);
}
