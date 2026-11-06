// Exact CPython results for float repr/round/sum, and decimal arithmetic
// following CPython's Lib/_pydecimal.py (same rounding, same digits), so money
// amounts match the reference bot. See third_party/CPYTHON_LICENSE.txt.
#include "sniper/pyrt.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sniper::py {
namespace {
BigInt pow10(int n) {
  BigInt r = 1;
  static const BigInt ten = 10;
  if (n <= 0)
    return r;
  BigInt base = ten;
  while (n) {
    if (n & 1)
      r *= base;
    n >>= 1;
    if (n)
      base *= base;
  }
  return r;
}
int ndigits(const BigInt &v) {
  if (v == 0)
    return 1;
  // fast path for values that fit in 64 bits
  if (v < BigInt(std::numeric_limits<unsigned long long>::max())) {
    auto u = v.convert_to<unsigned long long>();
    int n = 0;
    while (u) {
      ++n;
      u /= 10;
    }
    return n;
  }
  return static_cast<int>(v.str().size());
}
// Round coef / divisor (both >= 0, divisor > 0) to an integer.
BigInt round_div(const BigInt &coef, const BigInt &divisor, Rounding mode,
                 bool neg) {
  BigInt q, r;
  boost::multiprecision::divide_qr(coef, divisor, q, r);
  if (r == 0)
    return q;
  bool up = false;
  switch (mode) {
  case Rounding::Down:
    break;
  case Rounding::Up:
    up = true;
    break;
  case Rounding::Floor:
    up = neg;
    break;
  case Rounding::Ceiling:
    up = !neg;
    break;
  case Rounding::HalfUp:
    up = 2 * r >= divisor;
    break;
  case Rounding::HalfDown:
    up = 2 * r > divisor;
    break;
  case Rounding::HalfEven: {
    BigInt twice = 2 * r;
    up = twice > divisor || (twice == divisor && (q & 1) != 0);
    break;
  }
  }
  return up ? q + 1 : q;
}
std::string strip(std::string_view s) {
  std::size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    --b;
  return std::string(s.substr(a, b - a));
}
} // namespace

std::string repr(double x) {
  if (std::isnan(x))
    return "nan";
  if (std::isinf(x))
    return x > 0 ? "inf" : "-inf";
  if (x == 0)
    return std::signbit(x) ? "-0.0" : "0.0";
  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof buf, x,
                           std::chars_format::scientific);
  std::string s(buf, res.ptr);
  bool neg = false;
  std::size_t i = 0;
  if (s[0] == '-') {
    neg = true;
    i = 1;
  }
  auto e = s.find('e');
  std::string mant = s.substr(i, e - i), digits;
  for (char ch : mant)
    if (ch != '.')
      digits += ch;
  int sci = std::stoi(s.substr(e + 1));
  int decpt = sci + 1;
  std::string out = neg ? "-" : "";
  int n = static_cast<int>(digits.size());
  if (decpt <= -4 || decpt > 16) {
    out += digits[0];
    if (n > 1)
      out += "." + digits.substr(1);
    int ex = decpt - 1;
    char eb[16];
    std::snprintf(eb, sizeof eb, "e%c%02d", ex < 0 ? '-' : '+', std::abs(ex));
    out += eb;
  } else if (decpt <= 0) {
    out += "0." + std::string(-decpt, '0') + digits;
  } else if (decpt >= n) {
    out += digits + std::string(decpt - n, '0') + ".0";
  } else {
    out += digits.substr(0, decpt) + "." + digits.substr(decpt);
  }
  return out;
}

double round(double x, int ndigits) {
  if (!std::isfinite(x) || x == 0)
    return x;
  if (ndigits > 323)
    return x;
  if (ndigits < -308)
    return 0.0 * x;
  char buf[512];
  if (ndigits >= 0) {
    auto res = std::to_chars(buf, buf + sizeof buf, x,
                             std::chars_format::fixed, ndigits);
    *res.ptr = 0;
    return std::strtod(buf, nullptr);
  }
  // negative ndigits: round to tens/hundreds with ties to even
  Dec d = Dec::from_double(x);
  Dec r = rescale(d, -ndigits, Rounding::HalfEven);
  double v = r.to_double();
  return v == 0 ? std::copysign(0.0, x) : v;
}

std::string fixed(double x, int places) {
  if (std::isnan(x))
    return "nan";
  if (std::isinf(x))
    return x > 0 ? "inf" : "-inf";
  char buf[512];
  auto res = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed,
                           places);
  return std::string(buf, res.ptr);
}

std::string fmt_g(double x) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%g", x);
  return buf;
}

double parse_float(std::string_view text) {
  std::string s = strip(text);
  std::string t;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char ch = s[i];
    if (ch == '_') {
      // Python accepts a single underscore between digits only
      if (i == 0 || i + 1 >= s.size() ||
          !std::isdigit(static_cast<unsigned char>(s[i - 1])) ||
          !std::isdigit(static_cast<unsigned char>(s[i + 1])))
        throw ValueError("could not convert string to float");
      continue;
    }
    if (ch == 'x' || ch == 'X' || ch == 'p' || ch == 'P')
      throw ValueError("could not convert string to float");
    t += ch;
  }
  if (t.empty())
    throw ValueError("could not convert string to float");
  std::string low = t;
  std::transform(low.begin(), low.end(), low.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  std::string body = low;
  if (!body.empty() && (body[0] == '+' || body[0] == '-'))
    body = body.substr(1);
  if (body == "inf" || body == "infinity")
    return low[0] == '-' ? -INFINITY : INFINITY;
  if (body == "nan")
    return NAN;
  for (char ch : body)
    if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' ||
          ch == 'e' || ch == '+' || ch == '-'))
      throw ValueError("could not convert string to float");
  char *end = nullptr;
  errno = 0;
  double v = std::strtod(t.c_str(), &end);
  if (end != t.c_str() + t.size() || end == t.c_str())
    throw ValueError("could not convert string to float");
  return v;
}

// ------------------------------------------------------------------ Decimal
int Dec::digits() const { return ndigits(coef); }

Dec Dec::from_int(long long v) {
  Dec d;
  d.neg = v < 0;
  d.coef = v < 0 ? BigInt(-(v + 1)) + 1 : BigInt(v);
  d.exp = 0;
  return d;
}

Dec Dec::from_string(std::string_view text) {
  std::string s = strip(text);
  Dec d;
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
    d.neg = s[i] == '-';
    ++i;
  }
  std::string digits;
  int frac = 0;
  bool dot = false, any = false;
  for (; i < s.size(); ++i) {
    char ch = s[i];
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      digits += ch;
      any = true;
      if (dot)
        ++frac;
    } else if (ch == '.' && !dot) {
      dot = true;
    } else if (ch == '_' && any && i + 1 < s.size() &&
               std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
      continue;
    } else
      break;
  }
  if (!any)
    throw ValueError("Invalid literal for Decimal");
  int ex = 0;
  if (i < s.size()) {
    if (s[i] != 'e' && s[i] != 'E')
      throw ValueError("Invalid literal for Decimal");
    ++i;
    std::size_t start = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-'))
      ++i;
    if (i >= s.size())
      throw ValueError("Invalid literal for Decimal");
    for (std::size_t k = i; k < s.size(); ++k)
      if (!std::isdigit(static_cast<unsigned char>(s[k])))
        throw ValueError("Invalid literal for Decimal");
    ex = std::stoi(s.substr(start));
  }
  // cpp_int reads a leading 0 as an octal prefix: strip leading zeros
  auto nz = digits.find_first_not_of('0');
  d.coef = nz == std::string::npos ? BigInt(0) : BigInt(digits.substr(nz));
  d.exp = ex - frac;
  return d;
}

Dec Dec::from_double(double x) {
  if (!std::isfinite(x))
    throw ValueError("non-finite float has no finite Decimal");
  return from_string(repr(x));
}

std::string Dec::str() const {
  std::string ints = coef.str();
  int left = exp + static_cast<int>(ints.size());
  int dot;
  if (exp <= 0 && left > -6)
    dot = left;
  else
    dot = 1;
  std::string ip, fp;
  int n = static_cast<int>(ints.size());
  if (dot <= 0) {
    ip = "0";
    fp = "." + std::string(-dot, '0') + ints;
  } else if (dot >= n) {
    ip = ints + std::string(dot - n, '0');
  } else {
    ip = ints.substr(0, dot);
    fp = "." + ints.substr(dot);
  }
  std::string e;
  if (left != dot) {
    char b[24];
    std::snprintf(b, sizeof b, "E%+d", left - dot);
    e = b;
  }
  return (neg ? "-" : "") + ip + fp + e;
}

std::string Dec::fixed(int places) const {
  Dec r = rescale(*this, -places, Rounding::HalfEven);
  std::string ints = r.coef.str();
  if (places > 0) {
    if (static_cast<int>(ints.size()) <= places)
      ints = std::string(places + 1 - ints.size(), '0') + ints;
    ints.insert(ints.size() - places, ".");
  } else if (r.exp > 0) {
    ints += std::string(r.exp, '0');
  }
  bool negative = r.neg && r.coef != 0;
  return (negative || (r.neg && neg) ? "-" : "") + ints;
}

double Dec::to_double() const { return std::strtod(str().c_str(), nullptr); }

long long Dec::to_ll() const {
  BigInt v = coef;
  if (exp > 0)
    v *= pow10(exp);
  else if (exp < 0)
    v /= pow10(-exp);
  long long r = v.convert_to<long long>();
  return neg ? -r : r;
}

bool Dec::integral() const {
  if (exp >= 0 || coef == 0)
    return true;
  return coef % pow10(-exp) == 0;
}

Dec fix(Dec d, const Context &c) {
  if (d.coef == 0)
    return d;
  int n = ndigits(d.coef);
  if (n <= c.prec)
    return d;
  int drop = n - c.prec;
  BigInt q = round_div(d.coef, pow10(drop), c.rounding, d.neg);
  if (ndigits(q) > c.prec) {
    q /= 10;
    ++drop;
  }
  d.coef = q;
  d.exp += drop;
  return d;
}

Dec negate(const Dec &a) {
  Dec r = a;
  r.neg = !a.neg;
  return r;
}

int cmp(const Dec &a, const Dec &b) {
  bool az = a.coef == 0, bz = b.coef == 0;
  if (az && bz)
    return 0;
  int sa = az ? 0 : (a.neg ? -1 : 1), sb = bz ? 0 : (b.neg ? -1 : 1);
  if (sa != sb)
    return sa < sb ? -1 : 1;
  int e = std::min(a.exp, b.exp);
  BigInt x = a.coef * pow10(a.exp - e), y = b.coef * pow10(b.exp - e);
  int m = x < y ? -1 : (x > y ? 1 : 0);
  return sa < 0 ? -m : m;
}

Dec add(const Dec &a0, const Dec &b0, const Context &c) {
  if (a0.coef == 0 && b0.coef == 0) {
    Dec r;
    r.neg = c.rounding == Rounding::Floor ? (a0.neg || b0.neg)
                                         : (a0.neg && b0.neg);
    r.exp = std::min(a0.exp, b0.exp);
    return fix(r, c);
  }
  if (a0.coef == 0 || b0.coef == 0) {
    const Dec &zero = a0.coef == 0 ? a0 : b0;
    const Dec &other = a0.coef == 0 ? b0 : a0;
    int ex = std::min(zero.exp, other.exp);
    ex = std::max(ex, other.exp - c.prec - 1);
    return fix(rescale(other, ex, c.rounding), c);
  }
  // _normalize
  Dec tmp = a0.exp < b0.exp ? b0 : a0;   // larger exponent
  Dec other = a0.exp < b0.exp ? a0 : b0; // smaller exponent
  int tmp_len = ndigits(tmp.coef), other_len = ndigits(other.coef);
  int lim = tmp.exp + std::min(-1, tmp_len - c.prec - 2);
  if (other_len + other.exp - 1 < lim) {
    other.coef = 1;
    other.exp = lim;
  }
  tmp.coef *= pow10(tmp.exp - other.exp);
  tmp.exp = other.exp;
  Dec r;
  r.exp = other.exp;
  if (tmp.neg == other.neg) {
    r.coef = tmp.coef + other.coef;
    r.neg = tmp.neg;
  } else if (tmp.coef == other.coef) {
    r.coef = 0;
    r.neg = c.rounding == Rounding::Floor;
  } else if (tmp.coef > other.coef) {
    r.coef = tmp.coef - other.coef;
    r.neg = tmp.neg;
  } else {
    r.coef = other.coef - tmp.coef;
    r.neg = other.neg;
  }
  return fix(r, c);
}

Dec sub(const Dec &a, const Dec &b, const Context &c) {
  return add(a, negate(b), c);
}

Dec mul(const Dec &a, const Dec &b, const Context &c) {
  Dec r;
  r.neg = a.neg != b.neg;
  r.exp = a.exp + b.exp;
  r.coef = a.coef * b.coef;
  return fix(r, c);
}

Dec div(const Dec &a, const Dec &b, const Context &c) {
  if (b.coef == 0)
    throw ValueError("decimal division by zero");
  Dec r;
  r.neg = a.neg != b.neg;
  if (a.coef == 0) {
    r.exp = a.exp - b.exp;
    r.coef = 0;
    return fix(r, c);
  }
  int shift = ndigits(b.coef) - ndigits(a.coef) + c.prec + 1;
  int ex = a.exp - b.exp - shift;
  BigInt q, rem;
  if (shift >= 0)
    boost::multiprecision::divide_qr(BigInt(a.coef * pow10(shift)), b.coef, q,
                                     rem);
  else
    boost::multiprecision::divide_qr(a.coef, BigInt(b.coef * pow10(-shift)), q,
                                     rem);
  if (rem != 0) {
    if (q % 5 == 0)
      q += 1;
  } else {
    int ideal = a.exp - b.exp;
    while (ex < ideal && q % 10 == 0) {
      q /= 10;
      ++ex;
    }
  }
  r.coef = q;
  r.exp = ex;
  return fix(r, c);
}

Dec pow(const Dec &a, int n, const Context &c) {
  if (n < 0)
    throw ValueError("negative decimal power not supported");
  Dec r;
  r.neg = a.neg && (n % 2 == 1);
  BigInt v = 1;
  for (int i = 0; i < n; ++i)
    v *= a.coef;
  r.coef = v;
  r.exp = a.exp * n;
  return fix(r, c);
}

Dec rescale(const Dec &a, int ex, Rounding mode) {
  Dec r;
  r.neg = a.neg;
  r.exp = ex;
  if (a.coef == 0) {
    r.coef = 0;
    return r;
  }
  if (a.exp >= ex) {
    r.coef = a.coef * pow10(a.exp - ex);
    return r;
  }
  r.coef = round_div(a.coef, pow10(ex - a.exp), mode, a.neg);
  return r;
}

Dec quantize(const Dec &a, const Dec &like, Rounding mode) {
  return rescale(a, like.exp, mode);
}

Dec to_integral(const Dec &a, Rounding mode) {
  if (a.exp >= 0)
    return a;
  return rescale(a, 0, mode);
}

std::pair<BigInt, BigInt> as_integer_ratio(const Dec &a) {
  BigInt n = a.coef, d = 1;
  if (a.exp >= 0)
    n *= pow10(a.exp);
  else
    d = pow10(-a.exp);
  if (n == 0)
    return {0, 1};
  BigInt g = boost::multiprecision::gcd(n, d);
  n /= g;
  d /= g;
  return {a.neg ? BigInt(-n) : n, d};
}
} // namespace sniper::py
