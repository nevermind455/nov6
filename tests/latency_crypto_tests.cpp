#include "sniper/crypto.hpp"
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <limits>

using namespace sniper;
namespace {
Json vectors() {
  auto path = std::filesystem::read_symlink("/proc/self/exe")
                  .parent_path().parent_path() / "tests" / "fixtures" /
              "latency_order_digest_vectors.json";
  if (!std::filesystem::exists(path))
    path = std::filesystem::path(FIXTURE_DIR) / "latency_order_digest_vectors.json";
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot read independent typed-data vectors");
  return Json::parse(input);
}
UnsignedOrder unsigned_order(const Json &row) {
  UnsignedOrder order;
  order.salt = row.at("salt").get<std::uint64_t>();
  order.maker = row.at("maker");
  order.signer = row.at("signer");
  order.token = row.at("token");
  order.exchange = row.at("exchange");
  order.maker_amount = row.at("maker_amount").get<Units>();
  order.taker_amount = row.at("taker_amount").get<Units>();
  order.side = row.at("side");
  order.signature_type = row.at("signature_type");
  order.timestamp = row.at("timestamp").get<std::int64_t>();
  order.version = row.at("version");
  order.metadata = row.at("metadata");
  order.builder = row.at("builder");
  return order;
}
}

TEST(LatencyCrypto, DigestsMatchIndependentSdkTypedDataAcrossDomainsAndBoundaries) {
  const auto fixtures = vectors();
  for (const auto &row : fixtures.at("cases")) {
    SCOPED_TRACE(row.at("name").get<std::string>());
    EXPECT_EQ(hex(order_digest(unsigned_order(row))), row.at("digest").get<std::string>());
  }
}

TEST(LatencyCrypto, DynamicFieldsRemainBoundToEachAuthorization) {
  auto order = unsigned_order(vectors().at("cases").at(0));
  const auto original = order_digest(order);
  const std::vector<std::function<void(UnsignedOrder &)>> changes{
      [](auto &o) { ++o.salt; },
      [](auto &o) { o.maker = "0x2222222222222222222222222222222222222222"; },
      [](auto &o) { o.signer = "0x3333333333333333333333333333333333333333"; },
      [](auto &o) { o.token = "124"; },
      [](auto &o) { ++o.maker_amount; },
      [](auto &o) { ++o.taker_amount; },
      [](auto &o) { o.side = 1; },
      [](auto &o) { o.signature_type = 1; },
      [](auto &o) { ++o.timestamp; },
      [](auto &o) { o.metadata.back() = '1'; },
      [](auto &o) { o.builder.back() = '1'; },
      [](auto &o) { o.exchange = NEG_EXCHANGE; },
      [](auto &o) { o.version = "1"; }};
  for (std::size_t i = 0; i < changes.size(); ++i) {
    auto changed = order;
    changes[i](changed);
    EXPECT_NE(order_digest(changed), original) << "mutation " << i;
  }
}

TEST(LatencyCrypto, BuiltOrdersReuseOnlyStaticPreparation) {
  Signer signer("0x0000000000000000000000000000000000000000000000000000000000000001");
  Credentials credentials{"offline-key", "c2VjcmV0", "offline-passphrase"};
  const std::string token =
      "71321045679252212594626385532706912750332728571942532289631379312455583992563";
  for (int signature_type : {0, 1, 2}) {
    for (bool negative_risk : {false, true}) {
      Rules rules{10000, 5 * U, 70000, 1, negative_risk, true, 1};
      auto signed_order = build_order(signer, signer.address(), signature_type,
                                      "0" + token, 5 * U, U / 2, rules,
                                      credentials, "FAK");
      EXPECT_EQ(signed_order.digest, order_digest(signed_order.order));
      EXPECT_EQ(signed_order.id, hex(signed_order.digest));
      EXPECT_EQ(signer.recover(signed_order.digest, signed_order.signature), signer.address());
      EXPECT_EQ(signed_order.order.token, "0" + token);
      EXPECT_EQ(signed_order.order.exchange, negative_risk ? NEG_EXCHANGE : STANDARD_EXCHANGE);
      EXPECT_EQ(signed_order.order.maker_amount, 5 * U);
      EXPECT_EQ(signed_order.order.taker_amount, 10 * U);
      EXPECT_LT(signed_order.order.salt, std::uint64_t{1} << 53);
      EXPECT_EQ(signed_order.body,
                serialize_order(signed_order.order, signed_order.signature,
                                credentials.key, "FAK"));
    }
  }
}

TEST(LatencyCrypto, CachedDomainsDoNotBypassInputValidation) {
  auto order = unsigned_order(vectors().at("cases").at(0));
  for (const auto &mutate : std::vector<std::function<void(UnsignedOrder &)>>{
           [](auto &o) { o.signature_type = 3; },
           [](auto &o) { o.maker_amount = -1; },
           [](auto &o) { o.taker_amount = -1; },
           [](auto &o) { o.side = -1; },
           [](auto &o) { o.timestamp = -1; },
           [](auto &o) { o.token = "0"; },
           [](auto &o) { o.token = "1x"; },
           [](auto &o) { o.maker = "0x00"; },
           [](auto &o) { o.signer = "0x00"; },
           [](auto &o) { o.exchange = "0x00"; },
           [](auto &o) { o.metadata = "0x00"; },
           [](auto &o) { o.builder = "0x00"; }}) {
    auto invalid = order;
    mutate(invalid);
    EXPECT_THROW(order_digest(invalid), std::invalid_argument);
  }
}
