#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <utility>
namespace sniper {
using Tick = std::pair<double, double>;
struct TickQueue {
  std::deque<Tick> values;
};
struct Trigger {
  double k_bp, window_ms, max_gap_ms;
  TickQueue q;
  std::optional<Tick> ref;
  Trigger(double k, double w, double gap = 2000.0)
      : k_bp(k), window_ms(w), max_gap_ms(std::max(gap, w)) {}
  void trim(double t) {
    const double cut = t - window_ms;
    while (q.values.size() >= 2 && q.values[1].first <= cut)
      q.values.pop_front();
  }
  void hold(double t) {
    auto &v = q.values;
    if (v.empty() || !std::isfinite(t) || t <= v.back().first ||
        t - v.back().first > max_gap_ms)
      return;
    v.emplace_back(t, v.back().second);
    trim(t);
  }
  bool observe(double t, double mid) {
    auto &v = q.values;
    if (!std::isfinite(t) || !std::isfinite(mid) || mid <= 0.0) {
      v.clear();
      return false;
    }
    if (!v.empty() && (t <= v.back().first || t - v.back().first > max_gap_ms))
      v.clear();
    v.emplace_back(t, mid);
    trim(t);
    return true;
  }
  std::pair<std::optional<int>, double> update(double t, double mid) {
    if (!observe(t, mid))
      return {std::nullopt, 0.0};
    auto &v = q.values;
    if (v.front().first > t - window_ms || t - v.front().first > max_gap_ms)
      return {std::nullopt, 0.0};
    ref = v.front();
    const double r = (mid / v.front().second - 1.0) * 1e4;
    if (r >= k_bp - 1e-9)
      return {1, r};
    if (r <= -k_bp + 1e-9)
      return {-1, r};
    return {std::nullopt, r};
  }
};
} // namespace sniper
