#include "sniper/book.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace sniper;

TEST(Discovery, ExchangeTimestampHonorsUtcOffsets) {
  constexpr double expected = 1793963100000.0;
  for (const char *timestamp : {"2026-11-06T11:05:00Z",
                               "2026-11-06T11:05:00+00:00",
                               "2026-11-06T06:05:00-05:00",
                               "2026-11-06T16:35:00+05:30",
                               "2026-11-06T06:05:00-0500",
                               "2026-11-07T01:05:00+14:00",
                               "2026-11-05T23:05:00-12:00"}) {
    SCOPED_TRACE(timestamp);
    auto parsed = exchange_ms(Json(timestamp));
    ASSERT_TRUE(parsed);
    EXPECT_DOUBLE_EQ(*parsed, expected);
  }
  auto fractional = exchange_ms(Json("2026-11-06T06:05:00.123-05:00"));
  ASSERT_TRUE(fractional);
  EXPECT_DOUBLE_EQ(*fractional, expected + 123.0);
  EXPECT_EQ(exchange_ms(Json(expected)), expected);
  EXPECT_EQ(exchange_ms(Json("1793963100000")), expected);
  EXPECT_EQ(exchange_ms(Json("2026-11-06T11:05:00")), expected);
}

TEST(Discovery, ExchangeTimestampRefusesInvalidUtcOffsets) {
  for (const char *timestamp : {"2026-11-06T11:05:00+",
                               "2026-11-06T11:05:00+05:",
                               "2026-11-06T11:05:00+24:00",
                               "2026-11-06T11:05:00+05:60",
                               "2026-11-06T11:05:00+05:00trailing",
                               "2026-11-06T11:05:00Ztrailing"}) {
    SCOPED_TRACE(timestamp);
    EXPECT_FALSE(exchange_ms(Json(timestamp)));
  }
}

TEST(Discovery, RoundAcceptsEquivalentOffsetTimestamps) {
  auto base = std::filesystem::read_symlink("/proc/self/exe")
                  .parent_path()
                  .parent_path() /
              "tests" / "fixtures";
  if (!std::filesystem::exists(base / "public_event.json"))
    base = FIXTURE_DIR;
  std::ifstream input(base / "public_event.json");
  ASSERT_TRUE(input);
  Json event;
  input >> event;
  ASSERT_TRUE(parse_round(event, 1793963100));

  event["markets"][0]["eventStartTime"] = "2026-11-06T06:05:00-05:00";
  event["markets"][0]["endDate"] = "2026-11-06T16:40:00+05:30";
  EXPECT_TRUE(parse_round(event, 1793963100));

  // Matching wall-clock digits with a non-UTC offset describe another round.
  event["markets"][0]["eventStartTime"] = "2026-11-06T11:05:00-05:00";
  event["markets"][0]["endDate"] = "2026-11-06T11:10:00-05:00";
  EXPECT_FALSE(parse_round(event, 1793963100));
}
