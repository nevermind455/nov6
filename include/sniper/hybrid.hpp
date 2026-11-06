#pragma once
// Port of hybrid_sizing.py (nevermind455/oct10@772f2eb): pure sizing maths
// with the reference's exact Decimal semantics. Inputs that Python passes as
// floats are converted with Decimal(str(x)), exactly as _decimal() does.
#include "pyrt.hpp"
#include <cmath>
#include <optional>
#include <string>

namespace sniper::hybrid {
using py::D;
using py::Dec;

// _decimal(value, name, positive)
inline Dec decimal(const Dec &v, const std::string &name,
                   bool positive = false) {
  if (v.neg && !v.zero())
    throw py::ValueError(name + " must be finite and " +
                         (positive ? "positive" : "nonnegative"));
  if (positive && v.zero())
    throw py::ValueError(name + " must be finite and positive");
  return v;
}
inline Dec decimal(double v, const std::string &name, bool positive = false) {
  if (!std::isfinite(v))
    throw py::ValueError(name + " must be finite and " +
                         (positive ? "positive" : "nonnegative"));
  return decimal(D(v), name, positive);
}
// _number(value, name, positive)
inline double number(double v, const std::string &name, bool positive = false) {
  Dec d = decimal(v, name, positive);
  double r = d.to_double();
  if (!std::isfinite(r) || (!d.zero() && !d.neg && r <= 0))
    throw py::ValueError(name + " must be a finite representable number");
  return r;
}

struct Policy {
  double vol_baseline_bp = 1.0, drawdown_stop = 0.20, risk_fraction = 0.0,
         max_order_usd = 0.0;
  void validate() {
    vol_baseline_bp = number(vol_baseline_bp, "vol_baseline_bp", true);
    drawdown_stop = number(drawdown_stop, "drawdown_stop", true);
    if (drawdown_stop > 1)
      throw py::ValueError("drawdown_stop must be at most 1");
    risk_fraction = number(risk_fraction, "risk_fraction");
    if (risk_fraction > 1)
      throw py::ValueError("risk_fraction must be at most 1");
    max_order_usd = number(max_order_usd, "max_order_usd");
  }
};

inline bool at_drawdown_stop(const Policy &p, double equity,
                             double high_water) {
  equity = number(equity, "equity");
  high_water = number(high_water, "high_water", true);
  if (p.drawdown_stop >= 1)
    return false;
  py::Context c{50, py::Rounding::HalfEven};
  Dec peak = D(high_water);
  return py::sub(peak, D(equity), c) >= py::mul(peak, D(p.drawdown_stop), c);
}

struct Factors {
  double volatility_scale = 1, drawdown = 0, drawdown_scale = 1,
         effective_fraction = 0;
  std::optional<double> risk_budget_usd;
  bool hard_cap_enabled = false;
};

inline Factors sizing_factors(const Policy &p, double sigma, double equity,
                              double high_water, double base_fraction) {
  sigma = number(sigma, "sigma", true);
  equity = number(equity, "equity");
  high_water = number(high_water, "high_water", true);
  base_fraction = number(base_fraction, "base_fraction");
  if (high_water < equity)
    throw py::ValueError("high_water must be at least equity");
  if (base_fraction > 1)
    throw py::ValueError("base_fraction must be at most 1");
  Factors f;
  f.volatility_scale = std::min(1.0, (p.vol_baseline_bp / 1e4) / sigma);
  py::Context c{50, py::Rounding::HalfEven};
  Dec peak = D(high_water);
  Dec loss = py::sub(peak, D(equity), c);
  Dec stop_loss = py::mul(peak, D(p.drawdown_stop), c);
  f.drawdown = py::div(loss, peak, c).to_double();
  if (p.drawdown_stop >= 1)
    f.drawdown_scale = 1.0;
  else if (loss >= stop_loss)
    f.drawdown_scale = 0.0;
  else
    f.drawdown_scale =
        py::sub(D(1), py::div(loss, stop_loss, c), c).to_double();
  std::optional<double> budget;
  if (p.risk_fraction > 0)
    budget = equity * p.risk_fraction;
  if (p.max_order_usd > 0)
    budget = budget ? std::min(*budget, p.max_order_usd) : p.max_order_usd;
  f.risk_budget_usd = budget;
  f.hard_cap_enabled = budget.has_value();
  f.effective_fraction = base_fraction * f.volatility_scale * f.drawdown_scale;
  return f;
}

inline Dec risk_total(const Dec &spend0, const Dec &rate0,
                      const Dec &quoted0 = D(0)) {
  Dec spend = decimal(spend0, "spend"), rate = decimal(rate0, "fee_rate"),
      quoted = decimal(quoted0, "quoted_fee");
  py::Context c{50, py::Rounding::Ceiling};
  return py::add(spend, py::max(quoted, py::mul(spend, rate, c)), c);
}

inline long long shares_within_budget(const Dec &budget0, const Dec &cap0,
                                      const Dec &rate0) {
  Dec budget = decimal(budget0, "budget"),
      cap = decimal(cap0, "price_cap", true), rate = decimal(rate0, "fee_rate");
  py::Context c{50, py::Rounding::Ceiling};
  Dec per = py::mul(cap, py::add(D(1), rate, c), c);
  c.rounding = py::Rounding::Floor;
  return py::to_integral(py::div(budget, per, c), py::Rounding::Floor).to_ll();
}

// Raises py::ValueError("order risk X exceeds hybrid budget Y").
inline Dec enforce_order_budget(const Dec &spend, const Dec &rate,
                                const Dec &budget0,
                                const Dec &quoted = D(0)) {
  Dec budget = decimal(budget0, "budget");
  Dec total = risk_total(spend, rate, quoted);
  if (total > budget)
    throw py::ValueError("order risk " + total.str() +
                         " exceeds hybrid budget " + budget.str());
  return total;
}

inline long long minimum_entry_shares(const Dec &price_cap,
                                      const Dec &venue_min = D(5),
                                      const Dec &min_usd = D(1)) {
  Dec price = decimal(price_cap, "price_cap", true);
  Dec vmin = decimal(venue_min, "venue_min_shares", true);
  Dec musd = decimal(min_usd, "min_order_usd", true);
  if (price >= D(1))
    throw py::ValueError("price_cap must be below 1");
  py::Context c{50, py::Rounding::Ceiling};
  long long a = py::to_integral(vmin, py::Rounding::Ceiling).to_ll();
  long long b =
      py::to_integral(py::div(musd, price, c), py::Rounding::Ceiling).to_ll();
  long long minimum = std::max(a, b);
  auto ratio = py::as_integer_ratio(py::mul(price, D(100), c));
  long long multiple = ratio.second.convert_to<long long>();
  return ((minimum + multiple - 1) / multiple) * multiple;
}

inline long long cent_compatible_shares(const Dec &target0,
                                        const Dec &price_cap) {
  Dec target = decimal(target0, "target");
  Dec price = decimal(price_cap, "price_cap", true);
  if (price >= D(1))
    throw py::ValueError("price_cap must be below 1");
  auto ratio = py::as_integer_ratio(py::mul(price, D(100)));
  long long multiple = ratio.second.convert_to<long long>();
  long long shares = py::to_integral(target, py::Rounding::Floor).to_ll();
  return shares / multiple * multiple;
}
} // namespace sniper::hybrid
