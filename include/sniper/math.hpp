#pragma once
#include "normal.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace sniper {
inline double norm_cdf(double z) {
  // Same expression as statistics.NormalDist.cdf; do not replace with a tail
  // approximation or compile with -ffast-math.
  return 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
}
inline double taker_fee(double price, double rate, int exponent = 1) {
  return rate * std::pow(price * (1.0 - price), exponent);
}
inline double signed_anchored_fair(double market_mid, double move_bp,
                                   double sigma, double tau) {
  if (!(sigma > 0.0) || !(tau > 0.0))
    throw std::domain_error("sigma and tau must be positive");
  const double m = std::min(std::max(market_mid, 0.005), 0.995);
  const double dz = (move_bp / 1e4) / (sigma * std::sqrt(tau));
  return norm_cdf(normal_inv_cdf(m) + dz);
}
inline double anchored_fair(double market_mid, double move_bp, double sigma,
                            double tau) {
  return signed_anchored_fair(market_mid, std::abs(move_bp), sigma, tau);
}
inline double fair_limit(double fair, double margin, double tick,
                         double fee_rate, int fee_exponent = 1) {
  if (!(tick > 0.0) || !std::isfinite(tick) || !std::isfinite(fair) ||
      !std::isfinite(margin) || !std::isfinite(fee_rate) || fee_rate < 0.0 ||
      fee_exponent < 0)
    throw std::domain_error("invalid fair limit input");
  double p = std::floor((fair - margin) / tick + 1e-9) * tick;
  while (p > 0.0 &&
         p + taker_fee(p, fee_rate, fee_exponent) + margin > fair + 1e-12)
    p -= tick;
  return std::nearbyint(std::max(p, 0.0) * 1e4) / 1e4;
}
inline std::pair<double, double>
kelly_shares(double fair, double ask, double fee_rate, double fraction,
             double bankroll, double max_shares, double held_shares = 0.0,
             double opposite_shares = 0.0, int fee_exponent = 1) {
  const double p = ask + taker_fee(ask, fee_rate, fee_exponent);
  if (!(p > 0.0 && p < 1.0) || bankroll <= 0.0 || bankroll == 0.0)
    return {0.0, 0.0};
  const double f = (fair - p) / (1.0 - p);
  if (f <= 0.0)
    return {0.0, f};
  for (double v : {held_shares, opposite_shares, max_shares})
    if (!std::isfinite(v) || v < 0.0)
      return {0.0, f};
  const double wealth =
      bankroll + p * held_shares + (1.0 - p) * opposite_shares;
  const double wanted = fraction * (fair - p) * wealth / (p * (1.0 - p)) -
                        (held_shares - opposite_shares);
  return {
      std::max(0.0, std::min({std::floor(wanted + 1e-9),
                              std::floor(bankroll / p + 1e-9), max_shares})),
      f};
}
} // namespace sniper
