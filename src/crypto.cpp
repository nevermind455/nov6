#include "sniper/crypto.hpp"
extern "C" {
#include "sha3.h"
}
#include <algorithm>
#include <boost/multiprecision/cpp_dec_float.hpp>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <type_traits>
namespace sniper {
Bytes unhex(std::string_view s) {
  if (s.substr(0, 2) == "0x" || s.substr(0, 2) == "0X")
    s.remove_prefix(2);
  if (s.size() % 2)
    throw std::invalid_argument("invalid hex length");
  Bytes out(s.size() / 2);
  auto digit = [](char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    throw std::invalid_argument("invalid hex digit");
  };
  for (std::size_t i = 0; i < out.size(); ++i)
    out[i] = digit(s[2 * i]) * 16 + digit(s[2 * i + 1]);
  return out;
}
std::string hex(std::span<const unsigned char> b, bool prefix) {
  static constexpr char h[] = "0123456789abcdef";
  std::string out = prefix ? "0x" : "";
  out.reserve(out.size() + b.size() * 2);
  for (auto c : b) {
    out += h[c >> 4];
    out += h[c & 15];
  }
  return out;
}
Hash keccak(std::span<const unsigned char> b) {
  sha3_ctx_t c;
  sha3_init(&c, 32);
  sha3_update(&c, b.data(), b.size());
  c.st.b[c.pt] ^= 0x01;
  c.st.b[c.rsiz - 1] ^= 0x80;
  sha3_keccakf(c.st.q);
  Hash out{};
  std::memcpy(out.data(), c.st.b, 32);
  return out;
}
Hash keccak(std::string_view s) {
  return keccak(
      std::span(reinterpret_cast<const unsigned char *>(s.data()), s.size()));
}
std::string sha256(std::string_view s) {
  Hash h{};
  SHA256(reinterpret_cast<const unsigned char *>(s.data()), s.size(), h.data());
  return hex(h, false);
}
Hash word(const Big &n) {
  if (n < 0 || n >= (Big(1) << 256))
    throw std::invalid_argument("uint256 overflow");
  Hash out{};
  Big v = n;
  for (int i = 31; i >= 0; --i) {
    out[i] = static_cast<unsigned char>((v & 255).convert_to<unsigned>());
    v >>= 8;
  }
  return out;
}
Hash address_word(std::string_view a) {
  if (!valid_hex(a, 20))
    throw std::invalid_argument("invalid EVM address");
  Hash out{};
  auto b = unhex(a);
  std::copy(b.begin(), b.end(), out.begin() + 12);
  return out;
}
std::string checksum_address(std::string_view address) {
  auto b = unhex(address);
  if (b.size() != 20)
    throw std::invalid_argument("invalid address");
  auto lower = hex(b, false);
  const auto h = keccak(lower);
  for (std::size_t i = 0; i < lower.size(); ++i) {
    const auto nibble = i % 2 == 0 ? h[i / 2] >> 4 : h[i / 2] & 15;
    if (lower[i] >= 'a' && lower[i] <= 'f' &&
        nibble >= 8)
      lower[i] = std::toupper(lower[i]);
  }
  return "0x" + lower;
}
static Hash hash_words(std::initializer_list<Hash> words) {
  // initializer_list owns contiguous, fixed-size word values for this call.
  // Hash their object bytes directly rather than copying them to a heap vector.
  static_assert(sizeof(Hash) == 32);
  const auto bytes = std::as_bytes(std::span(words.begin(), words.size()));
  return keccak(std::span(
      reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size()));
}
template <class Integer> static Hash native_word(Integer value) {
  static_assert(std::is_integral_v<Integer> && sizeof(Integer) <= 8);
  if constexpr (std::is_signed_v<Integer>)
    if (value < 0)
      throw std::invalid_argument("uint256 overflow");
  auto n = static_cast<std::uint64_t>(value);
  Hash out{};
  for (int i = 31; i >= 24; --i) {
    out[i] = static_cast<unsigned char>(n & 255);
    n >>= 8;
  }
  return out;
}
static Hash raw32(std::string_view s) {
  if (s == ZERO32)
    return {};
  auto b = unhex(s);
  if (b.size() != 32)
    throw std::invalid_argument("bytes32 length");
  Hash h;
  std::copy(b.begin(), b.end(), h.begin());
  return h;
}
static Big decimal_token(std::string_view token) {
  if (token.empty() || token.size() > 78)
    throw std::invalid_argument("invalid decimal token id");
  Big value = 0;
  for (char digit : token) {
    if (digit < '0' || digit > '9')
      throw std::invalid_argument("invalid decimal token id");
    value = value * 10 + (digit - '0');
  }
  if (value <= 0 || value >= (Big(1) << 256))
    throw std::invalid_argument("invalid decimal token id");
  return value;
}
static const Hash &order_type_hash() {
  static const auto type = keccak(
      "Order(uint256 salt,address maker,address signer,uint256 tokenId,uint256 "
      "makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,uint256 "
      "timestamp,bytes32 metadata,bytes32 builder)");
  return type;
}
static Hash order_domain(std::string_view version, std::string_view exchange) {
  static const auto type = keccak("EIP712Domain(string name,string version,uint256 "
                                 "chainId,address verifyingContract)");
  static const auto name = keccak("Polymarket CTF Exchange");
  static const auto chain = native_word(137);
  return hash_words({type, name, keccak(version), chain, address_word(exchange)});
}
static const std::array<Hash, 2> &common_order_domains() {
  static const std::array<Hash, 2> domains{
      order_domain("2", STANDARD_EXCHANGE), order_domain("2", NEG_EXCHANGE)};
  return domains;
}
static Hash typed_digest(const Hash &domain, const Hash &structure) {
  std::array<unsigned char, 66> message{};
  message[0] = 0x19;
  message[1] = 0x01;
  std::copy(domain.begin(), domain.end(), message.begin() + 2);
  std::copy(structure.begin(), structure.end(), message.begin() + 34);
  return keccak(message);
}
static Hash order_digest_impl(const UnsignedOrder &o, const Big *parsed_token) {
  if (o.signature_type < 0 || o.signature_type > 2)
    throw std::invalid_argument("signature type 3 is unsupported");
  // Only immutable known domains are cached. Arbitrary public API domains
  // retain their normal validated encoding and cannot reuse a wrong separator.
  const auto domain =
      o.version == "2" && o.exchange == STANDARD_EXCHANGE
          ? common_order_domains()[0]
      : o.version == "2" && o.exchange == NEG_EXCHANGE
          ? common_order_domains()[1]
          : order_domain(o.version, o.exchange);
  auto structure = hash_words(
      {order_type_hash(), native_word(o.salt), address_word(o.maker), address_word(o.signer),
       parsed_token ? word(*parsed_token) : word(decimal_token(o.token)),
       native_word(o.maker_amount), native_word(o.taker_amount),
       native_word(o.side), native_word(o.signature_type), native_word(o.timestamp),
       raw32(o.metadata), raw32(o.builder)});
  return typed_digest(domain, structure);
}
Hash order_digest(const UnsignedOrder &o) {
  return order_digest_impl(o, nullptr);
}
Hash auth_digest(std::string_view address, std::int64_t timestamp,
                 std::uint64_t nonce) {
  static const auto domain = hash_words(
      {keccak("EIP712Domain(string name,string version,uint256 chainId)"),
       keccak("ClobAuthDomain"), keccak("1"), native_word(137)});
  static const auto type = keccak("ClobAuth(address address,string timestamp,uint256 nonce,string "
                                  "message)");
  static const auto attestation = keccak("This message attests that I control the given wallet");
  auto content = hash_words(
      {type, address_word(address), keccak(std::to_string(timestamp)), native_word(nonce),
       attestation});
  return typed_digest(domain, content);
}
Signer::Signer(std::string_view key) {
  std::array<unsigned char, 32> seed{};
  try {
  auto b = unhex(key);
  if (b.size() != 32)
    throw std::invalid_argument("POLY_PRIVATE_KEY must be 32 bytes");
  std::copy(b.begin(), b.end(), key_.begin());
  OPENSSL_cleanse(b.data(), b.size());
  ctx_ = secp256k1_context_create(SECP256K1_CONTEXT_SIGN |
                                  SECP256K1_CONTEXT_VERIFY);
  if (!ctx_ || !secp256k1_ec_seckey_verify(ctx_, key_.data()))
    throw std::invalid_argument("invalid POLY_PRIVATE_KEY");
  if (RAND_bytes(seed.data(), 32) != 1 || !secp256k1_context_randomize(ctx_, seed.data()))
    throw std::runtime_error("signing context randomization failed");
  OPENSSL_cleanse(seed.data(), seed.size());
  secp256k1_pubkey pub{};
  if (!secp256k1_ec_pubkey_create(ctx_, &pub, key_.data()))
    throw std::runtime_error("public key derivation failed");
  unsigned char encoded[65];
  std::size_t len = 65;
  secp256k1_ec_pubkey_serialize(ctx_, encoded, &len, &pub,
                                SECP256K1_EC_UNCOMPRESSED);
  auto h = keccak(std::span(encoded + 1, 64));
  address_ = checksum_address(hex(std::span(h.data() + 12, 20)));
  // Warm immutable typed-data constants before any signal can request a sign.
  (void)common_order_domains();
  (void)order_type_hash();
  } catch (...) {
    OPENSSL_cleanse(seed.data(), seed.size());
    OPENSSL_cleanse(key_.data(), key_.size());
    if (ctx_) secp256k1_context_destroy(ctx_);
    ctx_ = nullptr;
    throw;
  }
}

Signer::~Signer() {
  OPENSSL_cleanse(key_.data(), key_.size());
  if (ctx_)
    secp256k1_context_destroy(ctx_);
}
std::string Signer::sign(const Hash &digest) const {
  secp256k1_ecdsa_recoverable_signature sig{};
  if (!secp256k1_ecdsa_sign_recoverable(ctx_, &sig, digest.data(), key_.data(),
                                        secp256k1_nonce_function_rfc6979,
                                        nullptr))
    throw std::runtime_error("ECDSA signing failed");
  unsigned char out[65];
  int recid = 0;
  secp256k1_ecdsa_recoverable_signature_serialize_compact(ctx_, out, &recid,
                                                          &sig);
  out[64] = 27 + recid;
  return hex(out);
}
std::string Signer::recover(const Hash &digest,
                            std::string_view signature) const {
  auto b = unhex(signature);
  if (b.size() != 65 || b[64] < 27 || b[64] > 30)
    throw std::invalid_argument("invalid recoverable signature");
  secp256k1_ecdsa_recoverable_signature sig{};
  secp256k1_pubkey pub{};
  if (!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx_, &sig, b.data(),
                                                           b[64] - 27) ||
      !secp256k1_ecdsa_recover(ctx_, &pub, &sig, digest.data()))
    throw std::runtime_error("signature recovery failed");
  unsigned char out[65];
  std::size_t n = 65;
  secp256k1_ec_pubkey_serialize(ctx_, out, &n, &pub, SECP256K1_EC_UNCOMPRESSED);
  auto h = keccak(std::span(out + 1, 64));
  return checksum_address(hex(std::span(h.data() + 12, 20)));
}
bool Signer::self_test() const {
  auto h = keccak("sniper-cpp signing self-test; authorizes no order");
  return recover(h, sign(h)) == address_;
}
static std::string b64decode(std::string s) {
  std::replace(s.begin(), s.end(), '-', '+');
  std::replace(s.begin(), s.end(), '_', '/');
  while (s.size() % 4)
    s += '=';
  std::string out(s.size(), 0);
  int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                          reinterpret_cast<const unsigned char *>(s.data()),
                          s.size());
  if (n < 0)
    throw std::invalid_argument("invalid API secret base64");
  if (!s.empty() && s.back() == '=')
    --n;
  if (s.size() > 1 && s[s.size() - 2] == '=')
    --n;
  out.resize(n);
  return out;
}
std::string hmac_signature(std::string_view secret, std::int64_t timestamp,
                           std::string_view method, std::string_view path,
                           std::string_view body) {
  auto key = b64decode(std::string(secret));
  auto data = std::to_string(timestamp) + std::string(method) +
              std::string(path) + std::string(body);
  unsigned char md[32];
  unsigned int n = 32;
  if (!HMAC(EVP_sha256(), key.data(), key.size(),
            reinterpret_cast<const unsigned char *>(data.data()), data.size(),
            md, &n))
    throw std::runtime_error("HMAC failed");
  OPENSSL_cleanse(key.data(), key.size());
  std::string out(44, 0);
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()), md, 32);
  std::replace(out.begin(), out.end(), '+', '-');
  std::replace(out.begin(), out.end(), '/', '_');
  return out;
}
Headers l2_headers(const Signer &s, const Credentials &c,
                   std::string_view method, std::string_view path,
                   std::string_view body, std::int64_t timestamp) {
  if (!c.valid())
    throw std::invalid_argument("L2 credentials are unavailable");
  if (!timestamp)
    timestamp = static_cast<std::int64_t>(wall_s());
  return {{"POLY_ADDRESS", s.address()},
          {"POLY_API_KEY", c.key},
          {"POLY_PASSPHRASE", c.passphrase},
          {"POLY_SIGNATURE",
           hmac_signature(c.secret, timestamp, method, path, body)},
          {"POLY_TIMESTAMP", std::to_string(timestamp)}};
}
std::string wallet_for(std::string_view signer, int type) {
  if (type == 0)
    return checksum_address(signer);
  std::string factory;
  Hash salt, code;
  if (type == 1) {
    factory = "0xaB45c5A4B0c941a2F231C04C3f49182e1A254052";
    std::string f = hex(unhex(factory), false),
                impl = "44e999d5c2f66ef0861317f9a4805ac2e90aeb4f";
    auto bytes =
        unhex("3d3d606380380380913d393d73" + f +
              "5af4602a57600080fd5b602d8060366000396000f3363d3d373d3d3d363d73" +
              impl +
              "5af43d82803e903d91602b57fd5bf352e831dd00000000000000000000000000"
              "0000000000000000000000000000000000002000000000000000000000000000"
              "00000000000000000000000000000000000000");
    code = keccak(bytes);
    salt = keccak(unhex(signer));
  } else if (type == 2) {
    factory = "0xaacFeEa03eb1561C4e67d661e40682Bd20E3541b";
    auto w = address_word(signer);
    salt = keccak(w);
    code = raw32(
        "0x2bce2127ff07fb632d16c8347c4ebf501f4841168bed00d9e6ef715ddb6fcecf");
  } else
    throw std::invalid_argument("POLY_SIGNATURE_TYPE must be 0, 1 or 2");
  Bytes b{255};
  auto f = unhex(factory);
  b.insert(b.end(), f.begin(), f.end());
  b.insert(b.end(), salt.begin(), salt.end());
  b.insert(b.end(), code.begin(), code.end());
  auto h = keccak(b);
  return checksum_address(hex(std::span(h.data() + 12, 20)));
}
std::string serialize_order(const UnsignedOrder &o, std::string_view signature,
                            std::string_view owner, std::string_view type) {
  // SDK 0.13.0 build_post_order_request key order, serialized without spaces.
  auto quoted = [](std::string_view s) { return Json(std::string(s)).dump(); };
  return fmt::format(
      "{{\"deferExec\":false,\"order\":{{\"builder\":{},\"expiration\":\"0\","
      "\"maker\":{},\"makerAmount\":\"{}\",\"metadata\":{},\"salt\":{},"
      "\"side\":\"BUY\",\"signature\":{},\"signatureType\":{},\"signer\":{},"
      "\"takerAmount\":\"{}\",\"timestamp\":\"{}\",\"tokenId\":{}}},"
      "\"orderType\":{},\"owner\":{}}}",
      quoted(o.builder), quoted(o.maker), o.maker_amount, quoted(o.metadata),
      o.salt, quoted(signature), o.signature_type, quoted(o.signer),
      o.taker_amount, o.timestamp, quoted(o.token), quoted(type),
      quoted(owner));
}
SignedOrder build_order(const Signer &signer, std::string_view maker,
                        int signature_type, std::string_view token_id,
                        Units stake, Units cap, const Rules &rules,
                        const Credentials &creds, std::string_view type) {
  if (!rules.loaded)
    throw std::invalid_argument("order rules or token unavailable");
  Big token;
  try {
    token = decimal_token(token_id);
  } catch (const std::invalid_argument &) {
    throw std::invalid_argument("order rules or token unavailable");
  }
  if ((token & (((Big(1) << 64) - 1) << 40)) == 0)
    throw std::invalid_argument(
        "protocol-v2 position IDs require exchange-v3; native baseline trades "
        "legacy CTF tokens only");
  auto a = market_amounts(stake, cap, rules.tick);
  if (a.maker <= 0 || a.taker <= 0 ||
      static_cast<__int128>(a.maker) * U >=
          static_cast<__int128>(a.taker) * (cap + 100))
    throw std::invalid_argument("cannot preserve protected BUY price");
  SignedOrder s;
  s.order = {};
  auto &o = s.order;
  if (RAND_bytes(reinterpret_cast<unsigned char *>(&o.salt), sizeof(o.salt)) !=
      1)
    throw std::runtime_error("salt generation failed");
  o.salt &= ((1ull << 53) - 1);
  o.maker = checksum_address(maker);
  o.signer = signer.address();
  o.token = token_id;
  o.exchange = rules.neg ? NEG_EXCHANGE : STANDARD_EXCHANGE;
  o.maker_amount = a.maker;
  o.taker_amount = a.taker;
  o.signature_type = signature_type;
  o.timestamp = static_cast<std::int64_t>(wall_s() * 1000);
  s.digest = order_digest_impl(o, &token);
  s.signature = signer.sign(s.digest);
  s.id = hex(s.digest);
  s.body = serialize_order(o, s.signature, creds.key, type);
  return s;
}
Units fee_total(Units shares, Units price, Units rate, int exponent) {
  using D = boost::multiprecision::cpp_dec_float_50;
  if (shares < 0 || price < 0 || price > U || rate < 0 || exponent < 1 ||
      exponent > 8)
    throw std::invalid_argument("invalid fee input");
  D p = D(price) / U;
  D fee = D(shares) / U * D(rate) / U * pow(p * (1 - p), exponent);
  D rounded = floor(fee * 100000 + D("0.5"));
  if (rounded > D(std::numeric_limits<Units>::max() / 10))
    throw std::overflow_error("fee total overflow");
  return rounded.convert_to<Units>() * 10;
}
} // namespace sniper
