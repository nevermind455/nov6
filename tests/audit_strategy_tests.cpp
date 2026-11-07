#include "sniper/paper.hpp"
#include "sniper/math.hpp"
#include "sniper/strategy.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <unistd.h>

using namespace sniper;

TEST(AuditStrategy, RejectsImpossibleExchangeDatesAndOffsets) {
  for (const auto *value : {"2026-02-30T00:00:00Z", "2025-02-29T00:00:00Z",
                            "2026-04-31T00:00:00Z", "0000-01-01T00:00:00Z",
                            "2026-11-07T00:00:00+24:00",
                            "2026-11-07T00:00:00+01:60"})
    EXPECT_FALSE(iso_ms(Json(value))) << value;
  EXPECT_TRUE(iso_ms(Json("2024-02-29T12:34:56.123456Z")));
  EXPECT_EQ(iso_ms(Json("2026-11-07T01:00:00+01:00")),
            iso_ms(Json("2026-11-07T00:00:00Z")));
}

TEST(AuditStrategy, DecimalIntegerConversionIsExactOrRefusesOverflow) {
  const auto maximum = std::numeric_limits<long long>::max();
  const auto minimum = std::numeric_limits<long long>::min();
  EXPECT_EQ(py::D("9223372036854775807").to_ll(), maximum);
  EXPECT_EQ(py::D("-9223372036854775808").to_ll(), minimum);
  EXPECT_EQ(py::D("-123.999").to_ll(), -123);
  EXPECT_THROW(py::D("9223372036854775808").to_ll(), py::ValueError);
  EXPECT_THROW(py::D("-9223372036854775809").to_ll(), py::ValueError);
  EXPECT_THROW(py::D("1e100").to_ll(), py::ValueError);
}

TEST(AuditStrategy, FloatRoundingUsesExactBinaryValue) {
  // CPython: the binary float is just below the apparent decimal midpoint.
  EXPECT_DOUBLE_EQ(py::round(3.95e22, -21), 3.9e22);
  EXPECT_DOUBLE_EQ(py::round(-3.95e22, -21), -3.9e22);
  EXPECT_DOUBLE_EQ(py::round(25.0, -1), 20.0);
  EXPECT_DOUBLE_EQ(py::round(35.0, -1), 40.0);
  EXPECT_THROW(py::round(std::numeric_limits<double>::max(), -308),
               std::overflow_error);
}

TEST(AuditStrategy, LargeFloatFormattingDoesNotTruncateOrOverrun) {
  ASSERT_EXIT(
      {
        const double value = 1e308;
        if (py::round(value, 323) != value)
          _exit(31);
        const auto formatted = py::fixed(value, 323);
        // 309 integer digits, point, 323 decimals.
        if (formatted.size() != 633 || formatted.find('.') != 309 ||
            py::parse_float(formatted) != value)
          _exit(32);
        _exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(AuditStrategy, IntegerSettingsRefuseUnrepresentableValues) {
  // Run under float-cast-overflow sanitization as well: the checks must occur
  // before converting a finite but out-of-range double to a native int.
  for (const auto *name : {"SNIPER_MAX_FILLS_ROUND", "SNIPER_COINBASE_BACKLOG_MAX",
                           "SNIPER_COINBASE_CONNS"})
    for (const auto *value : {"1e100", "-1e100", "2147483648"})
      EXPECT_THROW(sniper_settings({{name, value}}), std::invalid_argument)
          << name << "=" << value;
  EXPECT_EQ(sniper_settings({{"SNIPER_MAX_FILLS_ROUND", "20.9"}}).max_fills_round,
            20);
}

TEST(AuditStrategy, CentSizingDoesNotSaturateLargeIntegerRatios) {
  EXPECT_EQ(hybrid::cent_compatible_shares(py::D("9223372036854775807"),
                                           py::D("0.0000000000000000000001")),
            0);
  EXPECT_THROW(hybrid::minimum_entry_shares(py::D("0.501"),
                                             py::D("9223372036854775807")),
               py::ValueError);
  EXPECT_THROW(hybrid::minimum_entry_shares(py::D("0.0000000000000000000001")),
               py::ValueError);
}

TEST(AuditStrategy, UnrepresentableFairLimitTickCannotHangTheEngine) {
  ASSERT_EXIT(
      {
        alarm(2);
        int refused = 0;
        for (double tick : {1e-20, 1e-320})
          try {
            fair_limit(.6, .01, tick, .07);
          } catch (const std::domain_error &) {
            ++refused;
          }
        _exit(refused == 2 ? 0 : 41);
      },
      ::testing::ExitedWithCode(0), "");
}

namespace {
struct PaperAuditHarness {
  double mono = 100, wall = 1793966500, cash = 1000;
  BookState books{8.0, 30.0, [this] { return mono; }, [this] { return wall; },
                  [this] { return wall; }};
  MarketRules rules;
  SniperSettings settings;
  PaperHooks hooks() {
    PaperHooks h;
    h.mono = [this] { return mono; };
    h.unix_wall = h.venue_wall = [this] { return wall; };
    h.context = [] { return std::make_tuple(std::string("condition"),
                                           std::string("123"),
                                           std::string("456")); };
    h.rules = [this](const std::string &) { return &rules; };
    h.cash_balance = [this] { return cash; };
    h.record_fill = [this](const Plan &, const FillQuote &q) {
      cash -= q.total_cost().to_double();
      return std::string("simulated-order");
    };
    return h;
  }
  PaperAuditHarness() {
    books.connected = true;
    books.set_active({"123", "456"});
    SnapshotOptions o;
    o.ts_ms = std::to_string(static_cast<long long>(wall * 1000));
    books.apply_snapshot("123", Json::array({{{"price", ".49"}, {"size", "10"}}}),
                         Json::array({{{"price", ".50"}, {"size", "10"}}}), o);
    rules.condition_id = "condition";
    rules.up_token_id = "123";
    rules.down_token_id = "456";
    rules.tick_size = py::D(".01");
    rules.min_order_size = py::D("5");
    rules.fee_rate = py::D(".07");
  }
  std::shared_ptr<Plan> plan() {
    auto p = std::make_shared<Plan>();
    p->side = "UP";
    p->token = p->up = "123";
    p->down = "456";
    p->cond = "condition";
    p->cap = .5;
    p->amount = 5;
    p->shares = 10;
    p->end = wall + 200;
    p->lat = 0;
    p->sent_mono = mono;
    p->sent_wall = wall;
    return p;
  }
};
} // namespace

TEST(AuditStrategy, DepletedOwnLiquidityIsRejectedWithoutInvalidAccess) {
  // An unpatched broker dereferences asks[0] after netting the last level.
  // Keep that regression in a child so it cannot terminate the whole suite.
  ASSERT_EXIT(
      {
        PaperAuditHarness h;
        PaperBroker broker({}, h.hooks());
        broker.begin(h.plan(), h.settings, h.books);
        h.books.note_exchange_ts(Json(std::to_string(
            static_cast<long long>(h.wall * 1000) + 1)));
        int completed = 0;
        broker.poll(h.books, h.settings, [&](const auto &, const auto &result) {
          if (!result.filled)
            _exit(11);
          ++completed;
        });
        if (completed != 1)
          _exit(12);
        broker.begin(h.plan(), h.settings, h.books);
        broker.poll(h.books, h.settings, [&](const auto &, const auto &result) {
          if (result.filled || result.error.find("no asks") == std::string::npos)
            _exit(13);
          ++completed;
        });
        _exit(completed == 2 ? 0 : 14);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(AuditStrategy, MissingVenueTickIsRejectedWithoutInvalidAccess) {
  ASSERT_EXIT(
      {
        PaperAuditHarness h;
        h.rules.tick_size.reset();
        PaperBroker broker({}, h.hooks());
        broker.begin(h.plan(), h.settings, h.books);
        int completed = 0;
        broker.poll(h.books, h.settings, [&](const auto &, const auto &result) {
          if (result.filled || result.error.find("tick") == std::string::npos)
            _exit(21);
          ++completed;
        });
        _exit(completed == 1 ? 0 : 22);
      },
      ::testing::ExitedWithCode(0), "");
}
