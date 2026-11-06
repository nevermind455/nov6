#pragma once
#include "types.hpp"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <span>
namespace sniper {
using Hash = std::array<unsigned char, 32>;
using Bytes = std::vector<unsigned char>;
using Headers = std::vector<std::pair<std::string, std::string>>;
Bytes unhex(std::string_view s);
std::string hex(std::span<const unsigned char> b, bool prefix = true);
Hash keccak(std::span<const unsigned char> bytes);
Hash keccak(std::string_view text);
std::string sha256(std::string_view s);
Hash word(const Big &n);
Hash address_word(std::string_view address);
std::string checksum_address(std::string_view address);
std::string hmac_signature(std::string_view secret, std::int64_t timestamp,
                           std::string_view method, std::string_view path,
                           std::string_view body = {});
struct UnsignedOrder {
  std::uint64_t salt = 0;
  std::string maker, signer, token, exchange;
  Units maker_amount = 0, taker_amount = 0;
  int side = 0, signature_type = 0;
  std::int64_t timestamp = 0;
  std::string version = "2", metadata = std::string(ZERO32),
              builder = std::string(ZERO32);
};
Hash order_digest(const UnsignedOrder &order);
Hash auth_digest(std::string_view address, std::int64_t timestamp,
                 std::uint64_t nonce = 0);
class Signer {
  secp256k1_context *ctx_ = nullptr;
  std::array<unsigned char, 32> key_{};
  std::string address_;

public:
  explicit Signer(std::string_view private_key);
  ~Signer();
  Signer(const Signer &) = delete;
  Signer &operator=(const Signer &) = delete;
  std::string sign(const Hash &digest) const;
  std::string recover(const Hash &digest, std::string_view signature) const;
  const std::string &address() const { return address_; }
  bool self_test() const;
};
struct Credentials {
  std::string key, secret, passphrase;
  bool valid() const {
    return !key.empty() && !secret.empty() && !passphrase.empty();
  }
};
Headers l2_headers(const Signer &, const Credentials &, std::string_view method,
                   std::string_view path, std::string_view body = {},
                   std::int64_t timestamp = 0);
std::string wallet_for(std::string_view signer, int signature_type);
std::string serialize_order(const UnsignedOrder &, std::string_view signature,
                            std::string_view owner,
                            std::string_view order_type = "FAK");
struct SignedOrder {
  UnsignedOrder order;
  Hash digest{};
  std::string signature, body, id;
};
SignedOrder build_order(const Signer &, std::string_view maker,
                        int signature_type, std::string_view token, Units stake,
                        Units cap, const Rules &, const Credentials &,
                        std::string_view type = "FAK");
Units fee_total(Units shares, Units price, Units rate, int exponent);
} // namespace sniper
