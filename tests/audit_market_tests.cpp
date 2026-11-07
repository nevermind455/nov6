#include "sniper/venue.hpp"
#include "sniper/storage.hpp"
#include <fstream>
#include <gtest/gtest.h>
using namespace sniper;
namespace {
Json market_fixture(const char *name) {
  auto base = std::filesystem::read_symlink("/proc/self/exe").parent_path().parent_path() / "tests" / "fixtures";
  if (!std::filesystem::exists(base / name)) base = FIXTURE_DIR;
  std::ifstream file(base / name);
  Json value;
  file >> value;
  return value;
}
}
TEST(AuditMarket, RejectsUnsupportedOrUnspecifiedProtocolVersions) {
  auto event = market_fixture("public_event.json");
  ASSERT_TRUE(parse_round(event, 1793963100));
  event["markets"][0]["version"] = "v2";
  EXPECT_FALSE(parse_round(event, 1793963100));
  event["markets"][0].erase("version");
  EXPECT_FALSE(parse_round(event, 1793963100));
}
TEST(AuditMarket, RulesMustDescribeTheRequestedBookAndCondition) {
  auto rules = market_fixture("public_rules.json"), book = market_fixture("public_book.json");
  auto token = book["asset_id"].get<std::string>();
  EXPECT_TRUE(parse_rules(rules, book, token).loaded);
  for (const auto *field : {"asset_id", "market"}) {
    auto bad = book;
    bad[field] = "wrong";
    EXPECT_THROW(parse_rules(rules, bad, token), std::runtime_error);
  }
  for (const auto *field : {"tick_size", "min_order_size"}) {
    auto bad = book;
    bad[field] = "0.123";
    EXPECT_THROW(parse_rules(rules, bad, token), std::runtime_error);
  }
  rules["v"] = "v2";
  EXPECT_THROW(parse_rules(rules, book, token), std::runtime_error);
}
TEST(AuditMarket, FeeExponentIsValidatedBeforeNativeNarrowing) {
  auto rules = market_fixture("public_rules.json"), book = market_fixture("public_book.json");
  auto token = book["asset_id"].get<std::string>();
  for (double invalid : {1.5, 1e100, -1e100, 0.0, 9.0}) {
    rules["fd"]["e"] = invalid;
    EXPECT_THROW(parse_rules(rules, book, token), std::runtime_error);
  }
}
TEST(AuditMarket, ClockRejectsInvalidSamplesWithoutDamagingTheGoodClock) {
  ServerClock clock;
  ASSERT_TRUE(clock.sample(1900000000, 1900000000, 1900000000.1, 100, 100));
  auto expected = clock.now(101);
  EXPECT_FALSE(clock.sample(1900000000, 1900000000, 1900000000.1, 100, NAN));
  EXPECT_FALSE(clock.sample(1e100, 1900000000, 1900000000.1, 100, 101));
  EXPECT_EQ(clock.now(101), expected);
  EXPECT_THROW(clock.now(NAN), std::runtime_error);
}
TEST(AuditMarket, PythonIntegerBoundaryParsingIsExact) {
  EXPECT_EQ(py_int(Json("-9223372036854775808")), std::numeric_limits<long long>::min());
  EXPECT_EQ(py_int(Json("9223372036854775807")), std::numeric_limits<long long>::max());
  EXPECT_FALSE(py_int(Json("9223372036854775808")));
  EXPECT_FALSE(py_int(Json("18446744073709551616")));
}
TEST(AuditMarket, UnsupportedTinyTicksCannotEnterALiveBook) {
  double now = 100;
  BookState book(8, 30, [&] { return now; }, [&] { return 1900000000.0; });
  book.connected = true;
  book.set_active({"123"});
  SnapshotOptions options;
  options.tick = 0.01;
  options.ts_ms = "1900000000000";
  options.require_exchange_ts = true;
  auto bids = Json::array({{{"price", "0.4"}, {"size", "10"}}});
  auto asks = Json::array({{{"price", "0.5"}, {"size", "10"}}});
  ASSERT_TRUE(book.apply_snapshot("123", bids, asks, options));
  EXPECT_FALSE(book.set_tick_size("123", "1e-10", Json(), false));
  EXPECT_EQ(book.status("123"), UNSYNCED);
  options.tick = 1e-10;
  EXPECT_FALSE(book.apply_snapshot("123", bids, asks, options));
}
TEST(AuditMarket, PrivateTradeIdentityAndLegInputsCannotBeEmptyOrMalformed) {
  Intent intent;
  intent.id = "order";
  intent.condition = "condition";
  intent.token = intent.up = "123";
  intent.down = "456";
  intent.limit = U / 2;
  intent.stake = 5 * U;
  intent.fee_rate = 70000;
  std::map<std::string, Intent> intents{{intent.id, intent}};
  Json trade{{"id", "trade"}, {"taker_order_id", "order"}, {"asset_id", "123"},
             {"market", "condition"}, {"side", "BUY"}, {"size", "2"},
             {"price", "0.5"}, {"status", "CONFIRMED"}};
  EXPECT_EQ(parse_trade(trade, intents).shares, 2 * U);
  auto invalid = trade;
  invalid["id"] = "";
  EXPECT_THROW(parse_trade(invalid, intents), std::runtime_error);
  invalid = trade;
  invalid["maker_orders"] = "invalid";
  EXPECT_THROW(parse_trade(invalid, intents), std::runtime_error);
  invalid = trade;
  invalid["maker_orders"] = Json::array({{{"price", "-0.1"}, {"matched_amount", "2"}, {"asset_id", "456"}}});
  EXPECT_THROW(parse_trade(invalid, intents), std::runtime_error);
}
