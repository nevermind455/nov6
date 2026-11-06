#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <tuple>
#include <utility>
namespace sniper {
struct Volatility {
  using Step = std::tuple<double, double, double>;
  double window_s, min_vol, sq = 0.0, dt = 0.0;
  int min_samples;
  unsigned long long steps = 0;
  std::optional<double> last_mid;
  std::optional<std::pair<double, double>> last;
  std::deque<Step> incs;
  Volatility(double window = 300.0, int samples = 60, double floor_bp = 0.2)
      : window_s(window), min_vol(floor_bp / 1e4), min_samples(samples) {}
  void observe(double mono, double mid) {
    if (!std::isfinite(mid) || mid <= 0.0)
      return;
    last_mid = mid;
    if (last && mono - last->first < 1.0)
      return;
    const double x = std::log(mid);
    if (last) {
      const double dx = x - last->second, a = dx * dx, b = mono - last->first;
      incs.emplace_back(mono, a, b);
      sq += a;
      dt += b;
      ++steps;
      const double cut = mono - window_s;
      while (!incs.empty() && std::get<0>(incs.front()) < cut) {
        sq -= std::get<1>(incs.front());
        dt -= std::get<2>(incs.front());
        incs.pop_front();
      }
      if (steps % 600 == 0) {
        // Compensated sum, as in Python >=3.12's float sum().
        double sa = 0.0, ca = 0.0, sb = 0.0, cb = 0.0;
        for (const auto &row : incs) {
          add(sa, ca, std::get<1>(row));
          add(sb, cb, std::get<2>(row));
        }
        sq = sa + ca;
        dt = sb + cb;
      }
    }
    last = std::pair{mono, x};
  }
  static void add(double &s, double &c, double x) {
    const double t = s + x;
    if (std::abs(s) >= std::abs(x))
      c += (s - t) + x;
    else
      c += (x - t) + s;
    s = t;
  }
  std::optional<double> vol() const {
    if (incs.size() < static_cast<std::size_t>(min_samples) || dt <= 0.0)
      return std::nullopt;
    return std::max(std::sqrt(std::max(sq, 0.0) / dt), min_vol);
  }
};
} // namespace sniper
