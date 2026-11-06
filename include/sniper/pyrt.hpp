#pragma once
// Exact emulation of the Python numerics the reference bot decides with:
// repr(float), round(float, n), f"{x:.Nf}" and the subset of decimal.Decimal
// (context precision and rounding, quantize, to_integral_value, str) used by
// sniper_mode.py, hybrid_sizing.py and paper_trade.py. Decisions that compare
// a Decimal(str(float)) against a budget must see the same digits Python sees,
// so values are never squeezed through a fixed micro-unit grid here.
#include <boost/multiprecision/cpp_int.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sniper::py {
using BigInt = boost::multiprecision::cpp_int;

struct ValueError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Python repr(float): shortest round-trip digits, Python's layout.
std::string repr(double x);
// Python round(x, ndigits) for a float (correctly rounded, ties to even).
double round(double x, int ndigits);
// Python format(x, ".Nf").
std::string fixed(double x, int places);
// Python format(x, "g").
std::string fmt_g(double x);
// Python's builtin sum() of floats (3.12+: Neumaier-compensated).
template <class C> double builtin_sum(const C &values) {
  double f = 0.0, c = 0.0;
  for (double x : values) {
    double t = f + x;
    if ((f < 0 ? -f : f) >= (x < 0 ? -x : x))
      c += (f - t) + x;
    else
      c += (x - t) + f;
    f = t;
  }
  if (c != 0 && c - c == 0)
    f += c;
  return f;
}
// Python float(text) for a decimal string; throws ValueError.
double parse_float(std::string_view text);

enum class Rounding { HalfEven, HalfUp, HalfDown, Floor, Ceiling, Down, Up };
struct Context {
  int prec = 28;
  Rounding rounding = Rounding::HalfEven;
};
inline constexpr Context DEFAULT{};

struct Dec {
  bool neg = false;
  BigInt coef = 0; // magnitude
  int exp = 0;     // value = (neg ? -1 : 1) * coef * 10^exp

  static Dec from_string(std::string_view);
  static Dec from_double(double); // Decimal(str(x))
  static Dec from_int(long long v);
  bool zero() const { return coef == 0; }
  int digits() const;
  int adjusted() const { return exp + digits() - 1; }
  std::string str() const;           // Decimal.__str__
  std::string fixed(int places) const; // format(d, ".Nf")
  double to_double() const;          // float(d)
  long long to_ll() const;           // int(d), truncating
  bool integral() const;             // d == d.to_integral_value()
};

Dec fix(Dec d, const Context &c);
Dec add(const Dec &a, const Dec &b, const Context &c = DEFAULT);
Dec sub(const Dec &a, const Dec &b, const Context &c = DEFAULT);
Dec mul(const Dec &a, const Dec &b, const Context &c = DEFAULT);
Dec div(const Dec &a, const Dec &b, const Context &c = DEFAULT);
Dec pow(const Dec &a, int n, const Context &c = DEFAULT);
Dec negate(const Dec &a);
int cmp(const Dec &a, const Dec &b);
Dec rescale(const Dec &a, int exp, Rounding r);
Dec quantize(const Dec &a, const Dec &like, Rounding r = Rounding::HalfEven);
Dec to_integral(const Dec &a, Rounding r = Rounding::HalfEven);
// (numerator, denominator) reduced, denominator > 0.
std::pair<BigInt, BigInt> as_integer_ratio(const Dec &a);
// Python's max(a, b): a unless b > a.
inline const Dec &max(const Dec &a, const Dec &b) {
  return cmp(b, a) > 0 ? b : a;
}
inline const Dec &min(const Dec &a, const Dec &b) {
  return cmp(b, a) < 0 ? b : a;
}

inline Dec operator+(const Dec &a, const Dec &b) { return add(a, b); }
inline Dec operator-(const Dec &a, const Dec &b) { return sub(a, b); }
inline Dec operator*(const Dec &a, const Dec &b) { return mul(a, b); }
inline Dec operator/(const Dec &a, const Dec &b) { return div(a, b); }
inline bool operator<(const Dec &a, const Dec &b) { return cmp(a, b) < 0; }
inline bool operator<=(const Dec &a, const Dec &b) { return cmp(a, b) <= 0; }
inline bool operator>(const Dec &a, const Dec &b) { return cmp(a, b) > 0; }
inline bool operator>=(const Dec &a, const Dec &b) { return cmp(a, b) >= 0; }
inline bool operator==(const Dec &a, const Dec &b) { return cmp(a, b) == 0; }
inline bool operator!=(const Dec &a, const Dec &b) { return cmp(a, b) != 0; }
inline Dec D(std::string_view s) { return Dec::from_string(s); }
inline Dec D(double x) { return Dec::from_double(x); }
inline Dec D(int v) { return Dec::from_int(v); }
inline Dec D(long long v) { return Dec::from_int(v); }
} // namespace sniper::py
