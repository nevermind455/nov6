// SNIPER_REST_LIVE: a missed FAK resting on the book as a post-only order.
#include "sniper/rest.hpp"
#include "sniper/sniper.hpp"
#include "sniper/venue.hpp"
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>
using namespace sniper;
namespace {
Json fixture(const char *name) {
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
std::filesystem::path tempdir() {
  auto p = std::filesystem::temp_directory_path() /
           ("sniper-rest-test-" + std::to_string(getpid()) + "-" +
            std::to_string(mono_ns()));
  std::filesystem::create_directories(p);
  return p;
}
const std::string COND = "0x" + std::string(64, '1');
// 5 shares resting at 0.60: a stake of $3 and no fee to reserve.
Intent rest_intent(std::string id = "rest-order") {
  Intent i;
  i.id = std::move(id);
  i.condition = COND;
  i.token = "123";
  i.up = "123";
  i.down = "456";
  i.side = "UP";
  i.slug = "test";
  i.status = "sent";
  i.stake = 3 * U;
  i.reserve = 3 * U;
  i.target = 5 * U;
  i.limit = 600000;
  i.fee_rate = 70000;
  i.fill_slot = true;
  i.rest = true;
  i.sent_wall = 1000;
  return i;
}
// An UP order missed 400 ms after its signal; BTC is still up, the side is
// well ahead of the price to beat and the ask has moved away.
struct Scene {
  rest::Missed missed;
  rest::Now now;
  rest::Policy policy;
  Scene() {
    missed.side = "UP";
    missed.end = 1300;
    missed.limit = 600000;
    missed.shares = 5 * U;
    missed.signal_ms = 50000;
    missed.ref_mid = 100000.0;
    now.wall = 1100;
    now.mono_ms = 50400;
    now.mid = 100012.0;
    now.feed_fresh = true;
    now.best_ask = 640000;
    now.min_shares = 5 * U;
    now.direction = Json{{"ptb", 99950.0},
                         {"ptb_dist_bp", 6.2},
                         {"ptb_margin_bp", 1.0},
                         {"ptb_checks", 8}};
    policy.rest_s = 8;
  }
  std::string refusal() const { return rest::refusal(missed, now, policy); }
};
rest::Order resting(double placed = 100, double life = 8) {
  rest::Order o;
  o.id = "rest-order";
  o.side = "UP";
  o.token = "123";
  o.condition = COND;
  o.end = 1300;
  o.ref_mid = 100000.0;
  o.placed_mono = placed;
  o.deadline_mono = placed + life;
  o.posted = true;
  return o;
}
} // namespace

TEST(RestWire, PostOnlyBodyAndCancelRequestAreTheSdksBytes) {
  auto f = fixture("rest_vectors.json");
  auto v = fixture("sdk_vectors.json").at("signing_vectors").at(
      f.at("signing_vector").get<std::size_t>());
  UnsignedOrder o;
  o.salt = v.at("salt");
  o.maker = o.signer = v.at("signer");
  o.token = v.at("token_id");
  o.exchange = v.at("exchange");
  o.maker_amount = v.at("maker_amount");
  o.taker_amount = v.at("taker_amount");
  o.signature_type = v.at("signature_type");
  o.timestamp = v.at("timestamp");
  const std::string sig = v.at("signature");
  // the order itself is signed exactly as a FAK is: only the envelope differs
  EXPECT_EQ(serialize_order(o, sig, "offline-key"), v.at("body").get<std::string>());
  EXPECT_EQ(serialize_order(o, sig, "offline-key", "GTC", true),
            f.at("gtc_post_only_body").get<std::string>());
  EXPECT_EQ(serialize_order(o, sig, "offline-key", "GTC"),
            f.at("gtc_body").get<std::string>());
  EXPECT_EQ(f.at("post_path"), "/order");
  // a taker order can never be marked post-only
  EXPECT_THROW(serialize_order(o, sig, "offline-key", "FAK", true),
               std::invalid_argument);
  EXPECT_THROW(serialize_order(o, sig, "offline-key", "FOK", true),
               std::invalid_argument);

  auto &c = f.at("cancel");
  EXPECT_EQ(c.at("method"), "DELETE");
  EXPECT_EQ(c.at("path"), "/order");
  auto body = cancel_order_body(c.at("order_id").get<std::string>());
  EXPECT_EQ(body, c.at("body").get<std::string>());
  EXPECT_EQ(hmac_signature(c.at("hmac_secret").get<std::string>(),
                           c.at("timestamp").get<std::int64_t>(), "DELETE",
                           "/order", body),
            c.at("hmac_signature").get<std::string>());
  EXPECT_THROW(cancel_order_body(""), std::invalid_argument);
}

TEST(RestWire, WholeSharesAtALimitSignTheSdksLimitOrderAmounts) {
  // The resting order reuses the missed FAK's stake and limit. A FAK's stake
  // is always whole cents, and for those the signed amounts must be exactly
  // the SDK's limit order of the same shares. Any other stake cannot sign for
  // the shares, which the runtime checks before it sends.
  auto f = fixture("rest_vectors.json");
  std::size_t same = 0, refused = 0;
  for (auto &r : f.at("limit_amounts")) {
    Units tick = units(r.at("tick").get<std::string>());
    Units price = units(r.at("price").get<std::string>());
    Units shares = r.at("shares").get<Units>() * U;
    Units stake = multiply(shares, price);
    auto a = market_amounts(stake, price, tick);
    if (stake % 10000 == 0) {
      ASSERT_EQ(a.maker, r.at("maker").get<Units>()) << r.dump();
      ASSERT_EQ(a.taker, r.at("taker").get<Units>()) << r.dump();
      ASSERT_EQ(a.taker, shares);
      ++same;
    } else {
      ASSERT_NE(a.taker, shares) << r.dump();
      ++refused;
    }
  }
  EXPECT_GT(same, 1000u);
  EXPECT_GT(refused, 100u);
}

TEST(RestRules, AMissedOrderRestsOnlyWhenEverythingHolds) {
  Scene ok;
  EXPECT_EQ(ok.refusal(), "");
  EXPECT_DOUBLE_EQ(rest::lifetime_s(1300, 1100, ok.policy), 8);

  Scene small;
  small.missed.shares = 3 * U; // Kelly sized the FAK under the venue minimum
  EXPECT_EQ(small.refusal(),
            "3 shares is below the venue minimum 5 for a resting order");

  Scene old;
  old.now.mono_ms = old.missed.signal_ms + 2500; // a very slow reply
  EXPECT_EQ(old.refusal(), "the signal is too old to rest on");

  Scene late;
  late.now.wall = 1289.5; // 10.5 s left, 10 of them kept clear
  EXPECT_EQ(late.refusal(), "too late in the round to rest");
  late.now.wall = 1285; // 5 s of resting fit: allowed, and shortened
  EXPECT_EQ(late.refusal(), "");
  EXPECT_DOUBLE_EQ(rest::lifetime_s(1300, 1285, late.policy), 5);

  Scene blind;
  blind.now.feed_fresh = false;
  EXPECT_EQ(blind.refusal(), "no fresh trigger price");

  Scene no_ref;
  no_ref.missed.ref_mid.reset();
  EXPECT_EQ(no_ref.refusal(), "the signal's reference price is unknown");

  Scene faded;
  faded.now.mid = 99999.0; // BTC is back under where the move began
  EXPECT_EQ(faded.refusal(), "the move was given back");

  Scene crossed;
  crossed.now.best_ask = 600000; // an offer at our limit is back
  EXPECT_EQ(crossed.refusal(),
            "the ask is back inside the limit (a post-only order would cross)");
  crossed.now.best_ask = 610000;
  EXPECT_EQ(crossed.refusal(), "");
  crossed.now.best_ask.reset(); // no usable book: the venue decides
  EXPECT_EQ(crossed.refusal(), "");

  Scene down;
  down.missed.side = "DOWN";
  down.now.mid = 99990.0;
  EXPECT_EQ(down.refusal(), "");
  down.now.mid = 100001.0;
  EXPECT_EQ(down.refusal(), "the move was given back");
}

TEST(RestRules, OnlyTheSideBeatingThePriceToBeatRests) {
  Scene behind;
  behind.now.direction["ptb_dist_bp"] = -2.5;
  EXPECT_EQ(behind.refusal(), "not beating the price to beat");

  Scene close;
  close.now.direction["ptb_dist_bp"] = 0.6;
  close.now.direction["ptb_why"] = "within the estimate's error (+-1.00bp)";
  EXPECT_EQ(close.refusal(), "beating the price to beat is not certain: within "
                             "the estimate's error (+-1.00bp)");

  Scene unchecked;
  unchecked.now.direction["ptb_why"] =
      "estimate checked against Polymarket's published price to beat on 2 of 6 rounds";
  EXPECT_NE(unchecked.refusal().find("2 of 6 rounds"), std::string::npos);

  Scene unknown;
  unknown.now.direction = Json{{"ptb", nullptr},
                               {"ptb_why", "this process did not see the round start"}};
  EXPECT_EQ(unknown.refusal(),
            "price to beat unknown: this process did not see the round start");

  // SNIPER_REST_DIR=0: any missed order may rest
  for (Scene *s : {&behind, &close, &unchecked, &unknown}) {
    s->policy.direction = false;
    EXPECT_EQ(s->refusal(), "");
  }
}

TEST(RestRules, ARestingOrderIsCancelledWhenTheTriggerPriceTurns) {
  rest::Policy policy;
  policy.rest_s = 8;
  auto o = resting();
  rest::Now now;
  now.wall = 1100;
  now.mono_ms = 101000; // one second in
  now.mid = 100010.0;
  now.feed_fresh = true;
  now.direction = Json{{"ptb_dist_bp", 5.0}};
  EXPECT_EQ(rest::cancel_reason(o, now, policy, false), "");

  EXPECT_EQ(rest::cancel_reason(o, now, policy, true),
            "the trigger fired the other way");

  auto back = now;
  back.mid = 100000.0; // all of the move given back
  EXPECT_EQ(rest::cancel_reason(o, back, policy, false),
            "the move was given back");

  auto behind = now;
  behind.direction["ptb_dist_bp"] = -0.1;
  EXPECT_EQ(rest::cancel_reason(o, behind, policy, false),
            "no longer beating the price to beat");
  // a lead inside the estimate's error keeps a placed order; only falling
  // behind takes it off
  auto close = now;
  close.direction = Json{{"ptb_dist_bp", 0.4}, {"ptb_why", "within the estimate's error"}};
  EXPECT_EQ(rest::cancel_reason(o, close, policy, false), "");
  auto unknown = now;
  unknown.direction = Json{{"ptb_dist_bp", nullptr}};
  EXPECT_EQ(rest::cancel_reason(o, unknown, policy, false),
            "price to beat unknown");

  auto silent = now;
  silent.feed_fresh = false;
  EXPECT_EQ(rest::cancel_reason(o, silent, policy, false),
            "no fresh trigger price");

  auto done = now;
  done.mono_ms = 108000;
  EXPECT_EQ(rest::cancel_reason(o, done, policy, false), "it rested its time");
  done.mono_ms = 107999;
  EXPECT_EQ(rest::cancel_reason(o, done, policy, false), "");

  auto ending = now;
  ending.wall = 1290;
  EXPECT_EQ(rest::cancel_reason(o, ending, policy, false),
            "the round's end is near");

  // the bot's own reasons come first
  EXPECT_EQ(rest::cancel_reason(o, back, policy, true, "shutdown"), "shutdown");

  // between ticks the timer judges time and silence only
  auto timer = policy;
  timer.direction = false;
  auto no_view = now;
  no_view.direction = Json::object();
  EXPECT_EQ(rest::cancel_reason(o, no_view, timer, false), "");
  EXPECT_EQ(rest::cancel_reason(o, no_view, policy, false),
            "price to beat unknown");

  auto down = resting();
  down.side = "DOWN";
  auto fell = now;
  fell.mid = 99990.0;
  EXPECT_EQ(rest::cancel_reason(down, fell, policy, false), "");
  fell.mid = 100000.5;
  EXPECT_EQ(rest::cancel_reason(down, fell, policy, false),
            "the move was given back");
}

TEST(RestReplies, PlacementAnswers) {
  using rest::Posted;
  auto live = rest::parse_post_reply(
      200,
      R"({"errorMsg":"","orderID":"0xabc","takingAmount":"","makingAmount":"","status":"live","success":true})",
      "0xabc");
  EXPECT_EQ(live.kind, Posted::Resting);
  EXPECT_EQ(live.venue_status, "live");

  auto crossed = rest::parse_post_reply(
      400, R"({"error":"invalid post-only order: order crosses book"})", "0xabc");
  EXPECT_EQ(crossed.kind, Posted::Crossed);
  EXPECT_EQ(crossed.detail, "invalid post-only order: order crosses book");
  crossed = rest::parse_post_reply(
      200,
      R"({"success":false,"errorMsg":"invalid post-only order: order crosses book","orderID":""})",
      "0xabc");
  EXPECT_EQ(crossed.kind, Posted::Crossed);

  auto small = rest::parse_post_reply(
      400, R"({"error":"order size 3 is lower than the minimum: 5"})", "0xabc");
  EXPECT_EQ(small.kind, Posted::Refused);
  // "post-only mode" is the venue's state, not our order crossing
  auto mode = rest::parse_post_reply(
      200,
      R"({"success":false,"errorMsg":"not enough balance / allowance"})", "0xabc");
  EXPECT_EQ(mode.kind, Posted::Refused);

  auto matched = rest::parse_post_reply(
      200,
      R"({"success":true,"status":"matched","orderID":"0xabc","makingAmount":"3","takingAmount":"5"})",
      "0xabc");
  EXPECT_EQ(matched.kind, Posted::Matched);
  EXPECT_EQ(matched.making, 3 * U);
  EXPECT_EQ(matched.taking, 5 * U);

  // anything else may be on the book: unknown, so it is cancelled
  for (auto [status, body] : std::vector<std::pair<int, const char *>>{
           {200, R"({"success":true,"status":"delayed","orderID":"0xabc"})"},
           {200, R"({"success":true,"status":"live","orderID":"0xother"})"},
           {500, R"({"error":"internal"})"},
           {502, "<html>bad gateway</html>"},
           {200, "[]"},
           {200, R"({"success":true,"status":"live","makingAmount":"x"})"}}) {
    EXPECT_EQ(rest::parse_post_reply(status, body, "0xabc").kind, Posted::Unknown)
        << body;
  }
}

TEST(RestReplies, CancelAnswersAndOrderRecords) {
  using rest::Cancelled;
  EXPECT_EQ(rest::parse_cancel_reply(
                200, R"({"canceled":["0xabc"],"not_canceled":{}})", "0xabc")
                .kind,
            Cancelled::Yes);
  auto gone = rest::parse_cancel_reply(
      200,
      R"({"canceled":[],"not_canceled":{"0xabc":"order can't be found - already canceled or matched"}})",
      "0xabc");
  EXPECT_EQ(gone.kind, Cancelled::NotOpen);
  EXPECT_EQ(gone.detail, "order can't be found - already canceled or matched");
  // another order's cancel says nothing about ours
  EXPECT_EQ(rest::parse_cancel_reply(200, R"({"canceled":["0xother"]})", "0xabc").kind,
            Cancelled::Failed);
  EXPECT_EQ(rest::parse_cancel_reply(401, R"({"error":"Unauthorized"})", "0xabc").kind,
            Cancelled::Failed);
  EXPECT_EQ(rest::parse_cancel_reply(400, R"({"canceled":["0xabc"]})", "0xabc").kind,
            Cancelled::Failed);
  EXPECT_EQ(rest::parse_cancel_reply(200, "not json", "0xabc").kind,
            Cancelled::Failed);

  for (auto s : {"MATCHED", "CANCELED", "CANCELLED", "canceled",
                 "CANCELED_MARKET_RESOLVED", "UNMATCHED", "EXPIRED", "KILLED"})
    EXPECT_TRUE(rest::record_terminal(s)) << s;
  for (auto s : {"LIVE", "live", "DELAYED", "", "RETRYING"})
    EXPECT_FALSE(rest::record_terminal(s)) << s;
}

TEST(RestBook, AMarketIsBusyUntilItsOrderIsShownOffTheBook) {
  rest::Book book;
  EXPECT_TRUE(book.empty());
  EXPECT_FALSE(book.busy());
  auto o = resting();
  o.posted = false;
  book.add(o);
  EXPECT_THROW(book.add(o), std::runtime_error);
  EXPECT_TRUE(book.busy());
  EXPECT_TRUE(book.in_market(COND));
  EXPECT_FALSE(book.in_market("0xanother"));

  // a cancel waits for the placement's answer: the venue has to know the order
  EXPECT_TRUE(book.cancel("rest-order", "the move was given back"));
  EXPECT_TRUE(book.due(100.2).empty());
  book.posted("rest-order");
  EXPECT_EQ(book.due(100.3), std::vector<std::string>{"rest-order"});
  // one request at a time, and the first reason is kept
  EXPECT_TRUE(book.due(100.4).empty());
  book.cancel("rest-order", "it rested its time");
  EXPECT_EQ(book.find("rest-order")->why, "the move was given back");
  EXPECT_FALSE(book.cancel("no-such-order", "x"));

  // the venue confirmed the cancel: the market is free, the final size is due
  book.proven("rest-order");
  EXPECT_FALSE(book.busy());
  EXPECT_FALSE(book.in_market(COND));
  EXPECT_FALSE(book.empty());
  EXPECT_TRUE(book.due(200).empty());
  EXPECT_TRUE(book.off("rest-order"));
  EXPECT_TRUE(book.empty());
  EXPECT_FALSE(book.off("rest-order"));
}

TEST(RestBook, ACancelIsAskedAgainUntilTheVenueShowsTheOrderGone) {
  rest::Book book;
  book.add(resting());
  book.cancel("rest-order", "it rested its time");
  double t = 108.0;
  ASSERT_EQ(book.due(t).size(), 1u);
  EXPECT_TRUE(book.unproven_to_report().empty());
  // an unanswered cancel is sent again after two seconds
  EXPECT_TRUE(book.due(t + 1.9).empty());
  ASSERT_EQ(book.due(t + 2.0).size(), 1u);
  // a failed one after a short, growing wait
  t += 2.1;
  book.cancel_unproven("rest-order", t);
  EXPECT_TRUE(book.due(t + 0.49).empty()); // 0.25 s x 2 tries
  ASSERT_EQ(book.due(t + 0.5).size(), 1u);
  EXPECT_EQ(book.find("rest-order")->cancels, 3);
  t += 0.6;
  book.cancel_unproven("rest-order", t);
  // three tries without proof: the operator is told once, and it keeps
  // asking every five seconds
  EXPECT_EQ(book.unproven_to_report(), std::vector<std::string>{"rest-order"});
  EXPECT_TRUE(book.unproven_to_report().empty());
  EXPECT_TRUE(book.due(t + 4.9).empty());
  ASSERT_EQ(book.due(t + 5.0).size(), 1u);
  EXPECT_TRUE(book.busy());

  // its market closed two minutes ago: nothing of it can match any more
  EXPECT_TRUE(book.give_up(1300 + 119).empty());
  auto left = book.give_up(1300 + 121);
  ASSERT_EQ(left.size(), 1u);
  EXPECT_FALSE(left[0].off_book);
  EXPECT_TRUE(book.empty());
}

TEST(RestTest, TheLossStopTurnsTheTestOffForGood) {
  rest::Test t;
  t.started_wall = 1000;
  t.baseline = -2 * U; // what resting orders had lost before this test
  EXPECT_TRUE(t.look(-2 * U, 10 * U, 1100));
  EXPECT_TRUE(t.look(3 * U, 10 * U, 1200));
  EXPECT_EQ(t.realized, 5 * U);
  EXPECT_TRUE(t.look(-11999999, 10 * U, 1300)); // $9.999999 lost: still on
  EXPECT_FALSE(t.look(-12 * U, 10 * U, 1400));  // $10 lost: off
  EXPECT_TRUE(t.off);
  EXPECT_EQ(t.realized, -10 * U);
  EXPECT_DOUBLE_EQ(t.off_wall, 1400);
  EXPECT_EQ(t.reason, "the loss stop was reached");
  // a later profit does not turn it back on
  EXPECT_FALSE(t.look(50 * U, 10 * U, 1500));
  EXPECT_DOUBLE_EQ(t.off_wall, 1400);

  auto again = rest::Test::load(Json::parse(t.json().dump()));
  EXPECT_TRUE(again.off);
  EXPECT_EQ(again.baseline, -2 * U);
  EXPECT_EQ(again.realized, -10 * U);
  EXPECT_DOUBLE_EQ(again.started_wall, 1000);
  EXPECT_EQ(again.reason, t.reason);
  EXPECT_THROW(rest::Test::load(Json{{"off", false}}), std::runtime_error);
  EXPECT_THROW(rest::Test::load(Json::array()), std::runtime_error);
  auto bad = t.json();
  bad["started_wall"] = 0;
  EXPECT_THROW(rest::Test::load(bad), std::runtime_error);
  bad = t.json();
  bad.erase("baseline");
  EXPECT_THROW(rest::Test::load(bad), std::exception);

  rest::Test no_stop;
  no_stop.started_wall = 1;
  EXPECT_FALSE(no_stop.look(0, 0, 2)); // a stop of zero bounds nothing: off
}

TEST(RestTrades, AMakerLegOfOurRestingOrderIsOurFill) {
  std::map<std::string, Intent> intents{{"rest-order", rest_intent()}};
  // someone sold 5 UP into our resting bid at 0.60
  Json sold{{"id", "trade-1"},
            {"taker_order_id", "0xsomeone"},
            {"market", COND},
            {"asset_id", "123"},
            {"side", "SELL"},
            {"size", "8"},
            {"price", "0.60"},
            {"status", "MATCHED"},
            {"maker_orders",
             Json::array({{{"order_id", "0xother-maker"},
                           {"asset_id", "123"},
                           {"matched_amount", "3"},
                           {"price", "0.61"}},
                          {{"order_id", "rest-order"},
                           {"asset_id", "123"},
                           {"matched_amount", "5"},
                           {"price", "0.60"},
                           {"side", "BUY"}}})}};
  ASSERT_TRUE(is_foreign_trade(sold, intents));
  auto fills = parse_maker_fills(sold, intents);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].id, "trade-1:rest-order");
  EXPECT_EQ(fills[0].order, "rest-order");
  EXPECT_EQ(fills[0].token, "123");
  EXPECT_EQ(fills[0].condition, COND);
  EXPECT_EQ(fills[0].status, "MATCHED");
  EXPECT_EQ(fills[0].shares, 5 * U);
  EXPECT_EQ(fills[0].cost, 3 * U);
  EXPECT_EQ(fills[0].fee, 0); // a maker pays no fee

  // a taker buying DOWN at 0.40 is matched with our UP bid: the trade is in
  // the other token, our leg is still UP at our price
  Json minted = sold;
  minted["asset_id"] = "456";
  minted["side"] = "BUY";
  minted["price"] = "0.40";
  minted["maker_orders"] = Json::array({{{"order_id", "rest-order"},
                                         {"asset_id", "123"},
                                         {"matched_amount", "2.5"},
                                         {"price", "0.60"}}});
  fills = parse_maker_fills(minted, intents);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].shares, 2500000);
  EXPECT_EQ(fills[0].cost, 1500000);

  // two legs of the same order in one trade are one fill
  Json twice = sold;
  twice["maker_orders"] = Json::array(
      {{{"order_id", "rest-order"}, {"asset_id", "123"}, {"matched_amount", "2"}, {"price", "0.60"}},
       {{"order_id", "rest-order"}, {"asset_id", "123"}, {"matched_amount", "3"}, {"price", "0.59"}}});
  fills = parse_maker_fills(twice, intents);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_EQ(fills[0].shares, 5 * U);
  EXPECT_EQ(fills[0].cost, 1200000 + 1770000);

  // none of the makers is ours: still a stranger's trade
  Json strangers = sold;
  strangers["maker_orders"] = Json::array(
      {{{"order_id", "0xother-maker"}, {"asset_id", "123"}, {"matched_amount", "3"}, {"price", "0.61"}}});
  EXPECT_TRUE(parse_maker_fills(strangers, intents).empty());
  strangers.erase("maker_orders");
  EXPECT_TRUE(parse_maker_fills(strangers, intents).empty());
}

TEST(RestTrades, AMakerFillThatDisagreesWithTheOrderIsRefused) {
  std::map<std::string, Intent> intents{{"rest-order", rest_intent()}};
  auto trade = [](Json leg) {
    leg["order_id"] = "rest-order";
    return Json{{"id", "trade-1"},
                {"taker_order_id", "0xsomeone"},
                {"market", COND},
                {"asset_id", "123"},
                {"side", "SELL"},
                {"size", "5"},
                {"price", "0.60"},
                {"status", "CONFIRMED"},
                {"maker_orders", Json::array({leg})}};
  };
  Json good{{"asset_id", "123"}, {"matched_amount", "5"}, {"price", "0.60"}};
  EXPECT_EQ(parse_maker_fills(trade(good), intents).size(), 1u);
  auto bad = good;
  bad["price"] = "0.61"; // above the price the order rests at
  EXPECT_THROW(parse_maker_fills(trade(bad), intents), std::runtime_error);
  bad = good;
  bad["asset_id"] = "456"; // another token
  EXPECT_THROW(parse_maker_fills(trade(bad), intents), std::runtime_error);
  bad = good;
  bad["side"] = "SELL";
  EXPECT_THROW(parse_maker_fills(trade(bad), intents), std::runtime_error);
  bad = good;
  bad["matched_amount"] = "0";
  EXPECT_THROW(parse_maker_fills(trade(bad), intents), std::runtime_error);
  auto other_market = trade(good);
  other_market["market"] = "0xanother";
  EXPECT_THROW(parse_maker_fills(other_market, intents), std::runtime_error);
  auto unnamed = trade(good);
  unnamed["id"] = "";
  EXPECT_THROW(parse_maker_fills(unnamed, intents), std::runtime_error);
  // an order that was never placed to rest cannot have a maker fill
  auto fak = rest_intent();
  fak.rest = false;
  std::map<std::string, Intent> taker_only{{"rest-order", fak}};
  EXPECT_THROW(parse_maker_fills(trade(good), taker_only), std::runtime_error);
}

TEST(RestLedger, AFilledRestingOrderIsBookedWithoutAFeeAndCountedInItsOwnResult) {
  auto dir = tempdir();
  auto path = dir / "ledger.json";
  auto order = rest_intent();
  auto fak = rest_intent("fak-order");
  fak.rest = false;
  fak.reserve = 3210000;
  const std::map<std::string, Units> up_wins{{"123", U}, {"456", 0}};
  {
    Ledger l(path, 100 * U, false);
    auto c = l.canary;
    ++c.orders;
    ++c.fills;
    c.spend += order.stake;
    l.register_intent(order, nullptr, c);
    l.status(order.id, "pending"); // the venue says it is on the book
    EXPECT_TRUE(l.orders.at(order.id).rest);
    EXPECT_TRUE(l.awaiting(order.token));
    EXPECT_EQ(l.reserved(), 3 * U); // its stake, and no fee on top
    EXPECT_EQ(l.risk_shares(order.token), 5 * U);
    EXPECT_FALSE(l.settle(COND, up_wins)); // not while it may still fill
    // a taker's FAK in the same round, to show whose result is whose
    ++c.orders;
    ++c.fills;
    c.spend += fak.stake;
    l.register_intent(fak, nullptr, c);
    EXPECT_TRUE(l.fill("taker-trade", fak.id, 5 * U, 3 * U, 84000));
    l.status(fak.id, "matched", 5 * U, 3 * U);
    // our resting order fills as a maker, then its cancellation gives 5 matched
    EXPECT_TRUE(l.fill("trade-1:" + order.id, order.id, 5 * U, 3 * U, 0));
    EXPECT_FALSE(l.fill("trade-1:" + order.id, order.id, 5 * U, 3 * U, 0));
    l.status(order.id, "matched", 5 * U);
    EXPECT_EQ(l.orders.at(order.id).status, "confirmed");
    EXPECT_FALSE(l.awaiting(order.token));
    EXPECT_EQ(l.orders.at(order.id).booked_fee, 0);
    // above its resting price or its stake is refused
    EXPECT_THROW(l.fill("trade-2:" + order.id, order.id, U, 610000, 0),
                 std::runtime_error);
    ASSERT_TRUE(l.settle(COND, up_wins));
    EXPECT_EQ(l.rest_realized, 2 * U); // 5 x $1 - $3, the resting order alone
    EXPECT_EQ(l.realized, 2 * U + 2 * U - 84000);
  }
  // the journal alone rebuilds it, and so does a snapshot
  {
    Ledger replayed(path, 100 * U, false);
    EXPECT_TRUE(replayed.orders.at(order.id).rest);
    EXPECT_FALSE(replayed.orders.at(fak.id).rest);
    EXPECT_EQ(replayed.rest_realized, 2 * U);
    replayed.snapshot();
  }
  Ledger restored(path, 100 * U, false);
  EXPECT_TRUE(restored.orders.at(order.id).rest);
  EXPECT_EQ(restored.rest_realized, 2 * U);
  std::filesystem::remove_all(dir);
}

TEST(RestLedger, ALosingRestingFillAndAnUnfilledOneAreAccountedFor) {
  auto dir = tempdir();
  Ledger l(dir / "ledger.json", 100 * U, false);
  auto lost = rest_intent("lost");
  auto c = l.canary;
  ++c.orders;
  ++c.fills;
  c.spend += lost.stake;
  l.register_intent(lost, nullptr, c);
  l.status(lost.id, "pending");
  EXPECT_TRUE(l.fill("t:lost", lost.id, 2 * U, 1200000, 0)); // 2 of 5 matched
  l.status(lost.id, "matched", 2 * U); // cancelled with 2 matched
  EXPECT_EQ(l.orders.at(lost.id).status, "confirmed");
  ASSERT_TRUE(l.settle(COND, {{"123", 0}, {"456", U}}));
  EXPECT_EQ(l.rest_realized, -1200000);

  // an order cancelled with nothing matched gives its budget back
  auto unfilled = rest_intent("unfilled");
  unfilled.condition = "0x" + std::string(64, '2');
  unfilled.token = "789";
  c = l.canary;
  ++c.orders;
  ++c.fills;
  c.spend += unfilled.stake;
  l.register_intent(unfilled, nullptr, c);
  l.status(unfilled.id, "pending");
  auto fills = l.canary.fills;
  auto spend = l.canary.spend;
  l.status(unfilled.id, "no_match", 0);
  l.release_missed_spend(unfilled.id);
  l.release_fill_slot(unfilled.id);
  EXPECT_EQ(l.canary.fills, fills - 1);
  EXPECT_EQ(l.canary.spend, spend - unfilled.stake);
  EXPECT_FALSE(l.awaiting("789"));
  EXPECT_EQ(l.reserved(), 0);
  // a fill that turns up after all takes them back
  EXPECT_TRUE(l.fill("late:unfilled", unfilled.id, U, 600000, 0));
  EXPECT_EQ(l.canary.fills, fills);
  EXPECT_EQ(l.canary.spend, spend);
  EXPECT_EQ(l.rest_realized, -1200000); // only settled results count
  std::filesystem::remove_all(dir);
}

TEST(RestSettings, OffUnlessAskedForAndOnlyWithAShareSizedFak) {
  using Env = std::map<std::string, std::string>;
  auto s = sniper_settings(Env{});
  EXPECT_FALSE(s.rest_live);
  EXPECT_TRUE(s.rest_dir);
  EXPECT_DOUBLE_EQ(s.rest_s, 5);
  EXPECT_DOUBLE_EQ(s.rest_stop_usd, 10);
  s = sniper_settings(Env{{"SNIPER_REST_LIVE", "1"},
                          {"SNIPER_REST_S", "8"},
                          {"SNIPER_REST_LIVE_STOP_USD", "4"},
                          {"SNIPER_REST_DIR", "0"}});
  EXPECT_TRUE(s.rest_live);
  EXPECT_FALSE(s.rest_dir);
  EXPECT_DOUBLE_EQ(s.rest_s, 8);
  EXPECT_DOUBLE_EQ(s.rest_stop_usd, 4);
  for (Env bad : {Env{{"SNIPER_REST_S", "0.5"}},
                  Env{{"SNIPER_REST_S", "31"}},
                  Env{{"SNIPER_REST_LIVE_STOP_USD", "0"}},
                  Env{{"SNIPER_REST_LIVE", "1"}, {"SNIPER_ORDER_TYPE", "FOK"}},
                  Env{{"SNIPER_REST_LIVE", "1"}, {"SNIPER_ORDER_SIZING", "dollars"}},
                  // after a miss the order either rests or is sent again
                  Env{{"SNIPER_REST_LIVE", "1"}, {"SNIPER_MISS_RETRY", "1"}}})
    EXPECT_THROW(sniper_settings(bad), std::invalid_argument);
}
