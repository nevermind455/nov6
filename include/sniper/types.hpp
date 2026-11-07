#pragma once
#include <algorithm>
#include <array>
#include <boost/multiprecision/cpp_int.hpp>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
namespace sniper {
using Json = nlohmann::ordered_json;
using Units = std::int64_t;
using Big = boost::multiprecision::cpp_int;
inline constexpr Units U = 1000000;
inline constexpr std::string_view STANDARD_EXCHANGE =
    "0xE111180000d2663C0091e4f400237545B87B996B";
inline constexpr std::string_view NEG_EXCHANGE =
    "0xe2222d279d744050d28e00520010520000310F59";
inline constexpr std::string_view CTF =
    "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045";
inline constexpr std::string_view COLLATERAL =
    "0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB";
inline constexpr std::string_view ZERO32 =
    "0x0000000000000000000000000000000000000000000000000000000000000000";
inline double mono_s() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline std::int64_t mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline double wall_s() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
inline double dollars(Units n) { return static_cast<double>(n) / U; }
inline std::string decimal(Units n) {
  bool neg = n < 0;
  auto v = static_cast<std::uint64_t>(neg ? -(n + 1) : n) + (neg ? 1 : 0);
  auto s = fmt::format("{}{}.{:06}", neg ? "-" : "", v / U, v % U);
  while (s.back() == '0')
    s.pop_back();
  if (s.back() == '.')
    s.pop_back();
  return s;
}
inline Units units(std::string_view s) {
  if (s.empty())
    throw std::invalid_argument("empty decimal");
  bool neg = false;
  std::size_t pos = 0;
  if (s[0] == '-' || s[0] == '+') {
    neg = s[0] == '-';
    ++pos;
  }
  Big whole = 0, frac = 0;
  int places = 0;
  bool dot = false, any = false;
  for (; pos < s.size(); ++pos) {
    char c = s[pos];
    if (c == '.' && !dot) {
      dot = true;
      continue;
    }
    if (c < '0' || c > '9')
      throw std::invalid_argument("invalid decimal");
    any = true;
    if (!dot)
      whole = whole * 10 + (c - '0');
    else {
      if (places >= 6) {
        if (c != '0')
          throw std::invalid_argument("decimal exceeds six places");
      } else {
        frac = frac * 10 + (c - '0');
        ++places;
      }
    }
  }
  if (!any)
    throw std::invalid_argument("invalid decimal");
  while (places++ < 6)
    frac *= 10;
  Big n = whole * U + frac;
  if (neg)
    n = -n;
  if (n > std::numeric_limits<Units>::max() ||
      n < std::numeric_limits<Units>::min())
    throw std::overflow_error("decimal overflow");
  return n.convert_to<Units>();
}
// Six-place fixed text of a double, correctly rounded like printf("%.6f").
// std::to_chars avoids fmt 9's precision path, which trips a GCC 13
// -Warray-bounds false positive in header-only builds.
inline std::string fixed6(double x) {
  if (!std::isfinite(x))
    throw std::invalid_argument("nonfinite decimal");
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, 6);
  if (r.ec != std::errc{})
    throw std::overflow_error("decimal overflow");
  return std::string(buf, r.ptr);
}
// A quantity the venue reports about a trade that already happened. The venue
// can send more than six places (an average price, say), and refusing it would
// leave a real fill unbooked, so this rounds to the nearest micro-unit. Settings
// and signed amounts keep the strict units().
inline Units venue_units(const Json &j) {
  if (!j.is_string())
    return units(fixed6(j.get<double>()));
  std::string s = j.get<std::string>();
  auto dot = s.find('.');
  if (dot == std::string::npos || s.size() - dot - 1 <= 6)
    return units(s);
  for (std::size_t i = dot + 7; i < s.size(); ++i)
    if (s[i] < '0' || s[i] > '9')
      throw std::invalid_argument("invalid decimal");
  bool up = s[dot + 7] >= '5';
  Units kept = units(s.substr(0, dot + 7));
  if (!up)
    return kept;
  if (s[0] == '-') {
    if (kept == std::numeric_limits<Units>::min())
      throw std::overflow_error("rounded venue quantity overflow");
    return kept - 1;
  }
  if (kept == std::numeric_limits<Units>::max())
    throw std::overflow_error("rounded venue quantity overflow");
  return kept + 1;
}
inline Units as_units(const Json &j) {
  if (j.is_string())
    return units(j.get<std::string>());
  if (j.is_boolean() || !j.is_number())
    throw std::invalid_argument("expected decimal");
  return units(fixed6(j.get<double>()));
}
inline double number(const Json &j) {
  if (j.is_boolean())
    throw std::invalid_argument("expected number");
  double n;
  if (j.is_string()) {
    auto text = j.get<std::string>();
    std::size_t consumed = 0;
    n = std::stod(text, &consumed);
    if (text.find_first_not_of(" \t\r\n\f\v", consumed) != std::string::npos)
      throw std::invalid_argument("trailing data in number");
  } else
    n = j.get<double>();
  if (!std::isfinite(n))
    throw std::invalid_argument("nonfinite number");
  return n;
}
inline Units ceil_div128(__int128 n, __int128 d) {
  if (n < 0 || d <= 0)
    throw std::invalid_argument("invalid division");
  auto q = n / d + (n % d != 0);
  if (q > std::numeric_limits<Units>::max())
    throw std::overflow_error("integer overflow");
  return static_cast<Units>(q);
}
inline Units multiply(Units a, Units b, bool up = false) {
  if (a < 0 || b < 0)
    throw std::invalid_argument("negative monetary input");
  auto product = static_cast<__int128>(a) * b;
  if (up)
    return ceil_div128(product, U);
  auto result = product / U;
  if (result > std::numeric_limits<Units>::max())
    throw std::overflow_error("monetary multiplication overflow");
  return static_cast<Units>(result);
}
inline Units reservation(Units spend, Units rate, Units quoted = 0) {
  auto fee = std::max(multiply(spend, rate, true), quoted);
  if (spend > std::numeric_limits<Units>::max() - fee)
    throw std::overflow_error("reservation overflow");
  return spend + fee;
}
inline Units shares_within_budget(Units bank, Units cap, Units rate) {
  if (bank < 0 || cap <= 0 || rate < 0)
    throw std::invalid_argument("invalid budget");
  return static_cast<Units>((static_cast<__int128>(bank) * U) /
                            (static_cast<__int128>(cap) * (static_cast<__int128>(U) + rate)));
}
inline Units share_multiple(Units price) {
  if (price <= 0 || price >= U)
    throw std::invalid_argument("price out of range");
  return 10000 / std::gcd(price, Units(10000));
}
inline Units minimum_entry_shares(Units price, Units venue = 5 * U,
                                  Units min_usd = U) {
  auto m = std::max(ceil_div128(venue, U), ceil_div128(min_usd, price));
  auto step = share_multiple(price);
  auto result = static_cast<__int128>(ceil_div128(m, step)) * step;
  if (result > std::numeric_limits<Units>::max())
    throw std::overflow_error("minimum share count overflow");
  return static_cast<Units>(result);
}
inline Units cent_compatible_shares(Units whole, Units price) {
  auto step = share_multiple(price);
  return whole / step * step;
}
struct Amounts {
  Units maker = 0, taker = 0;
};
inline Amounts market_amounts(Units stake, Units price, Units tick) {
  int dp = 0;
  switch (tick) {
  case 100000:
    dp = 3;
    break;
  case 10000:
    dp = 4;
    break;
  case 5000:
  case 1000:
    dp = 5;
    break;
  case 2500:
  case 100:
    dp = 6;
    break;
  default:
    throw std::invalid_argument("unsupported market tick");
  }
  if (price <= 0 || price >= U || price % tick || stake <= 0)
    throw std::invalid_argument("invalid order amounts");
  Units maker = stake / 10000 * 10000, scale = 1;
  for (int i = 0; i < dp; ++i)
    scale *= 10;
  __int128 numerator = static_cast<__int128>(maker) * scale;
  // The SDK first rounds UP at amount+4, then DOWN at amount precision.
  __int128 coarse = ((numerator * 10000 + price - 1) / price) / 10000;
  Units mul = 1;
  for (int i = dp; i < 6; ++i)
    mul *= 10;
  if (coarse > std::numeric_limits<Units>::max() / mul)
    throw std::overflow_error("taker amount overflow");
  return {maker, static_cast<Units>(coarse) * mul};
}
enum class Side { Up = 0, Down = 1 };
inline const char *side_name(Side s) { return s == Side::Up ? "UP" : "DOWN"; }
struct Rules {
  Units tick = 0, min_shares = 0, rate = 0;
  int exponent = 1;
  bool neg = false, loaded = false;
  std::uint64_t generation = 0;
};
struct Round {
  std::string condition, up, down, slug, market_id;
  std::int64_t start = 0, end = 0;
  Rules rules[2];
  std::optional<double> ptb;
};
struct Level {
  Units price = 0, size = 0;
};
inline std::string token(const Round &r, Side side) {
  return side == Side::Up ? r.up : r.down;
}
inline bool valid_hex(std::string_view s, std::size_t bytes) {
  if (s.size() != 2 + bytes * 2 || s.substr(0, 2) != "0x")
    return false;
  for (char c : s.substr(2))
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return false;
  return true;
}
inline bool valid_token(std::string_view s) {
  if (s.empty() || s.size() > 78)
    return false;
  Big n = 0;
  for (char c : s) {
    if (c < '0' || c > '9')
      return false;
    n = n * 10 + c - '0';
  }
  return n > 0 && n < (Big(1) << 256);
}
} // namespace sniper
