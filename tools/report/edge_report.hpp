// Does the bot have an edge? Calibration and profit from a run's journal.
//
// A settlement row names the winning token and a fill row names only the side,
// so each order's token comes from paper_audit.jsonl and ledger.json. Fills that
// cannot be matched to a settled result are counted and left out, never guessed.
//
// Confidence intervals resample whole rounds, because fills in one round share
// one result and do not count as independent evidence.
#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <fmt/format.h>
#include <functional>
#include <istream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace sniper::edge {
using Json = nlohmann::json;
using Interval = std::optional<std::pair<double, double>>;

inline constexpr double UNIT = 1'000'000.0; // a payout of 1000000 is $1 a share
inline constexpr int RESAMPLES = 4000;
inline constexpr int DEFAULT_MIN_ROUNDS = 300;

struct Order {
  std::string id, condition, token;
  std::optional<double> fair;
  double shares = 0, spent = 0, fee = 0, wall = 0;
  double payout = 0, net = 0;
  bool won = false, clean = false; // clean: resolved fully one way, not 50/50
  std::size_t settlement_order = 0;
  double paid() const { return spent / shares; }
};

struct Bin {
  std::string label;
  int fills = 0, rounds = 0;
  double predicted = 0, actual = 0, low = 0, high = 0, paid = 0;
  bool inside = false;
  bool has_interval = false;
};

struct Report {
  int fills_settled = 0, rounds = 0, scored = 0, calibration_rounds = 0;
  int unsettled = 0, untraceable = 0, voided = 0;
  int errors = 0, refused = 0, skipped = 0, bad_lines = 0, invalid_rows = 0;
  bool has_profit = false, has_calibration = false;
  double net = 0, spent = 0, fees = 0, return_on_spend = 0, per_round = 0;
  double max_drawdown = 0, first_half = 0, second_half = 0;
  Interval return_ci, per_round_ci, gap_ci;
  std::vector<Bin> bins;
  double predicted = 0, actual = 0, paid = 0, brier_model = 0, brier_price = 0;
  std::string verdict, why;
};

inline std::optional<double> number(const Json &row, const char *name) {
  auto it = row.find(name);
  if (it == row.end() || !it->is_number())
    return std::nullopt;
  double value = it->get<double>();
  return std::isfinite(value) ? std::optional(value) : std::nullopt;
}
inline std::string text(const Json &row, const char *name) {
  auto it = row.find(name);
  return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}

inline int parse_min_rounds(std::string_view value) {
  int result = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc() || end != value.data() + value.size() || result <= 0)
    throw std::invalid_argument("--min-rounds must be a positive integer");
  return result;
}

template <class F> int read_json_lines(std::istream &in, F use) {
  if (!in)
    throw std::runtime_error("cannot read report input");
  int bad = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    Json row = Json::parse(line, nullptr, false);
    if (row.is_discarded() || !row.is_object()) {
      ++bad;
      continue;
    }
    use(row);
  }
  if (in.bad())
    throw std::runtime_error("error reading report input");
  return bad;
}

inline std::pair<double, double> wilson(double wins, double n, double z = 1.96) {
  if (n <= 0)
    return {0.0, 1.0};
  double p = wins / n, d = 1 + z * z / n;
  double mid = (p + z * z / (2 * n)) / d;
  double half = z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d;
  return {std::max(0.0, mid - half), std::min(1.0, mid + half)};
}

using Groups = std::vector<std::vector<const Order *>>;
inline Groups by_round(const std::vector<const Order *> &items) {
  std::map<std::string, std::vector<const Order *>> grouped;
  for (auto *o : items)
    grouped[o->condition].push_back(o);
  Groups out;
  for (auto &[name, group] : grouped)
    out.push_back(std::move(group));
  return out;
}

// 95% interval of stat over resampled rounds; empty with under 2 rounds.
inline Interval
bootstrap(const Groups &groups,
          const std::function<std::optional<double>(const std::vector<const Order *> &, int)> &stat) {
  if (groups.size() < 2)
    return std::nullopt;
  std::mt19937_64 rng(7);
  std::uniform_int_distribution<std::size_t> pick(0, groups.size() - 1);
  std::vector<double> values;
  std::vector<const Order *> sample;
  for (int i = 0; i < RESAMPLES; ++i) {
    sample.clear();
    for (std::size_t g = 0; g < groups.size(); ++g) {
      auto &group = groups[pick(rng)];
      sample.insert(sample.end(), group.begin(), group.end());
    }
    if (auto v = stat(sample, static_cast<int>(groups.size())); v && std::isfinite(*v))
      values.push_back(*v);
  }
  if (values.empty())
    return std::nullopt;
  std::sort(values.begin(), values.end());
  auto at = [&](double q) { return values[static_cast<std::size_t>(q * (values.size() - 1))]; };
  return std::make_pair(at(0.025), at(0.975));
}

// Reads rows one at a time, so a long journal is not held in memory.
class Collector {
public:
  void add_ledger(const Json &ledger) {
    auto it = ledger.find("orders");
    if (it == ledger.end() || !it->is_array()) {
      ++invalid_rows_;
      return;
    }
    for (auto &o : *it)
      if (!text(o, "id").empty() && !text(o, "token").empty())
        add_token(text(o, "id"), text(o, "token"));
      else
        ++invalid_rows_;
  }
  void add_audit(const Json &row) {
    if (!text(row, "order_id").empty() && !text(row, "token_id").empty())
      add_token(text(row, "order_id"), text(row, "token_id"));
  }
  void add(const Json &row) {
    auto result = text(row, "result");
    if (result == "error")
      ++errors_;
    else if (result == "refused")
      ++refused_;
    else if (result == "skipped")
      ++skipped_;
    else if (result == "settlement") {
      auto it = row.find("payouts");
      auto condition = text(row, "condition_id");
      std::map<std::string, double> pay;
      bool valid = !condition.empty() && it != row.end() && it->is_object() && !it->empty();
      if (valid)
        for (auto p = it->begin(); p != it->end(); ++p) {
          auto amount = number(*it, p.key().c_str());
          if (p.key().empty() || !amount || *amount < 0 || *amount > UNIT) {
            valid = false;
            break;
          }
          pay[p.key()] = *amount;
        }
      auto previous = payouts_.find(condition);
      if (!valid || (previous != payouts_.end() && previous->second != pay)) {
        ++invalid_rows_;
        invalid_conditions_.insert(condition);
        return;
      }
      if (previous == payouts_.end())
        settlement_order_[condition] = settlement_order_.size();
      payouts_[condition] = std::move(pay);
    } else if (result == "filled" || result == "pending") {
      const auto id = text(row, "order_id");
      Decision d;
      d.condition = text(row, "condition_id");
      d.fair = number(row, "fair");
      d.wall = number(row, "wall").value_or(0);
      d.paper_filled = result == "filled";
      d.shares = number(row, "shares").value_or(0);
      d.cost = number(row, "cost");
      d.fee = number(row, "fee").value_or(0);
      const bool invalid_optional =
          (row.contains("fair") && !row.at("fair").is_null() && !d.fair) ||
          (row.contains("wall") && !number(row, "wall")) ||
          (row.contains("fee") && !number(row, "fee"));
      if (id.empty() || d.condition.empty() || invalid_optional ||
          (d.fair && (*d.fair < 0 || *d.fair > 1)) || d.wall < 0 || d.fee < 0 ||
          (d.paper_filled && (d.shares <= 0 || !d.cost || *d.cost <= 0 || d.fee > *d.cost))) {
        ++invalid_rows_;
        invalid_orders_.insert(id);
        return;
      }
      auto old = decisions_.find(id);
      if (old != decisions_.end() &&
          (old->second.condition != d.condition || old->second.fair != d.fair ||
           old->second.paper_filled != d.paper_filled || old->second.shares != d.shares ||
           old->second.cost != d.cost || old->second.fee != d.fee)) {
        ++invalid_rows_;
        invalid_orders_.insert(id);
        return;
      }
      decisions_[id] = d;
    } else if (result == "live_fill") {
      auto trade = text(row, "trade_id"), id = text(row, "order_id");
      auto shares = number(row, "shares"), amount = number(row, "amount"), fee = number(row, "fee");
      if (trade.empty() || id.empty() || !shares || *shares <= 0 ||
          !amount || *amount <= 0 || !fee || *fee < 0) {
        ++invalid_rows_;
        invalid_orders_.insert(id);
        return;
      }
      Json evidence{{"order_id", id}, {"shares", *shares}, {"amount", *amount}, {"fee", *fee}};
      auto old = trades_.find(trade);
      if (old != trades_.end()) {
        if (old->second != evidence) {
          ++invalid_rows_;
          invalid_orders_.insert(id);
          invalid_orders_.insert(text(old->second, "order_id"));
        }
        return; // a repeated trade counts once
      }
      trades_[trade] = std::move(evidence);
      auto &got = live_[id];
      std::array<double, 3> next{got[0] + *shares, got[1] + *amount, got[2] + *fee};
      if (!std::all_of(next.begin(), next.end(), [](double n) { return std::isfinite(n); })) {
        ++invalid_rows_;
        invalid_orders_.insert(id);
        return;
      }
      got = next;
    }
  }
  Report report(int min_rounds, int bad_lines = 0) const;

private:
  void add_token(const std::string &id, const std::string &token) {
    auto [it, added] = tokens_.emplace(id, token);
    if (!added && it->second != token) {
      ++invalid_rows_;
      invalid_orders_.insert(id);
    }
  }
  struct Decision {
    std::string condition;
    std::optional<double> fair, cost;
    double wall = 0, shares = 0, fee = 0;
    bool paper_filled = false;
  };
  std::map<std::string, std::string> tokens_;
  std::map<std::string, std::map<std::string, double>> payouts_;
  std::map<std::string, Decision> decisions_;
  std::map<std::string, std::array<double, 3>> live_;
  std::map<std::string, Json> trades_;
  std::map<std::string, std::size_t> settlement_order_;
  std::set<std::string> invalid_orders_, invalid_conditions_;
  int errors_ = 0, refused_ = 0, skipped_ = 0, invalid_rows_ = 0;
};

inline void judge(Report &r, int min_rounds) {
  if (r.bad_lines || r.invalid_rows || r.untraceable) {
    r.verdict = "INCOMPLETE DATA";
    r.why = "unreadable, invalid or untraceable evidence prevents an edge conclusion";
    return;
  }
  if (!r.has_profit) {
    r.verdict = "NO DATA";
    r.why = "no settled fills to judge";
    return;
  }
  bool overconfident = r.gap_ci && r.gap_ci->second < 0;
  if (r.return_ci && r.return_ci->second < 0) {
    r.verdict = "NO EDGE";
    r.why = "the whole profit interval is below zero: it is losing money";
  } else if (r.rounds < min_rounds) {
    r.verdict = "TOO EARLY";
    r.why = fmt::format("{} rounds settled, {} needed.", r.rounds, min_rounds);
    if (overconfident)
      r.why += " The model is already overconfident, so Kelly would oversize.";
  } else if (!r.has_calibration || !r.gap_ci) {
    r.verdict = "UNPROVEN";
    r.why = "settled profit is available, but model calibration needs 2+ scored rounds";
  } else if (r.calibration_rounds < min_rounds) {
    r.verdict = "TOO EARLY";
    r.why = fmt::format("{} scored rounds, {} needed to judge calibration", r.calibration_rounds, min_rounds);
  } else if (overconfident) {
    r.verdict = "OVERCONFIDENT";
    r.why = "it wins less often than predicted, so Kelly oversizes orders";
  } else if (r.return_ci && r.return_ci->first > 0) {
    r.verdict = "EDGE LIKELY";
    r.why = "the whole profit interval is above zero and the model is not overconfident";
  } else {
    r.verdict = "UNPROVEN";
    r.why = "the profit interval still includes zero: collect more rounds";
  }
}

inline Report Collector::report(int min_rounds, int bad_lines) const {
  Report r;
  r.errors = errors_;
  r.refused = refused_;
  r.skipped = skipped_;
  r.bad_lines = bad_lines;
  r.invalid_rows = invalid_rows_;
  for (const auto &[id, fills] : live_) {
    (void)fills;
    if (!decisions_.count(id))
      ++r.untraceable;
  }

  std::vector<Order> settled;
  for (auto &[id, d] : decisions_) {
    if (invalid_orders_.count(id) || invalid_conditions_.count(d.condition))
      continue;
    Order o;
    o.id = id;
    o.condition = d.condition;
    o.fair = d.fair;
    o.wall = d.wall;
    if (d.paper_filled) {
      if (!d.cost)
        continue;
      o.shares = d.shares;
      o.spent = *d.cost; // a paper row's cost already includes the fee
      o.fee = d.fee;
    } else {
      auto got = live_.find(id);
      if (got == live_.end())
        continue; // sent, but never confirmed filled
      o.shares = got->second[0];
      o.fee = got->second[2];
      o.spent = got->second[1] + o.fee; // a live amount leaves the fee out
    }
    if (o.shares <= 0)
      continue;
    auto pay = payouts_.find(d.condition);
    if (pay == payouts_.end()) {
      ++r.unsettled;
      continue;
    }
    auto token = tokens_.find(id);
    if (token == tokens_.end() || !pay->second.count(token->second)) {
      ++r.untraceable;
      continue;
    }
    o.token = token->second;
    double per_share = pay->second.at(o.token) / UNIT;
    o.clean = per_share == 0.0 || per_share == 1.0;
    if (!o.clean)
      ++r.voided; // a 50/50 resolution says nothing about prediction
    o.won = per_share == 1.0;
    o.payout = per_share * o.shares;
    o.net = o.payout - o.spent;
    if (!std::isfinite(o.spent) || !std::isfinite(o.payout) || !std::isfinite(o.net)) {
      ++r.invalid_rows;
      continue;
    }
    o.settlement_order = settlement_order_.at(d.condition);
    settled.push_back(std::move(o));
  }
  std::sort(settled.begin(), settled.end(),
            [](const Order &a, const Order &b) {
              return std::tie(a.settlement_order, a.wall, a.id) <
                     std::tie(b.settlement_order, b.wall, b.id);
            });

  std::vector<const Order *> all, scored;
  for (auto &o : settled) {
    all.push_back(&o);
    if (o.fair && o.clean)
      scored.push_back(&o);
  }
  r.fills_settled = static_cast<int>(all.size());
  r.scored = static_cast<int>(scored.size());
  Groups groups = by_round(all);
  r.rounds = static_cast<int>(groups.size());

  if (!all.empty()) {
    r.has_profit = true;
    for (auto *o : all) {
      r.net += o->net;
      r.spent += o->spent;
      r.fees += o->fee;
    }
    r.return_on_spend = r.spent > 0 ? r.net / r.spent : 0;
    r.per_round = r.net / r.rounds;
    r.per_round_ci = bootstrap(groups, [](const auto &s, int rounds) -> std::optional<double> {
      double net = 0;
      for (auto *o : s)
        net += o->net;
      return net / rounds;
    });
    r.return_ci = bootstrap(groups, [](const auto &s, int) -> std::optional<double> {
      double net = 0, spent = 0;
      for (auto *o : s) {
        net += o->net;
        spent += o->spent;
      }
      if (spent <= 0)
        return std::nullopt;
      return net / spent;
    });
    Groups ordered = groups;
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
      return a.front()->settlement_order < b.front()->settlement_order;
    });
    std::size_t half = ordered.size() / 2;
    double curve = 0, peak = 0;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
      double net = 0;
      for (auto *o : ordered[i])
        net += o->net;
      (i < half ? r.first_half : r.second_half) += net;
      curve += net;
      peak = std::max(peak, curve);
      r.max_drawdown = std::max(r.max_drawdown, peak - curve);
    }
  }

  if (!scored.empty()) {
    r.has_calibration = true;
    r.calibration_rounds = static_cast<int>(by_round(scored).size());
    double n = static_cast<double>(scored.size());
    auto price = [](const Order *o) { return std::min(o->paid(), 1.0); };
    for (auto *o : scored) {
      double won = o->won ? 1.0 : 0.0;
      r.predicted += *o->fair / n;
      r.actual += won / n;
      r.paid += price(o) / n;
      r.brier_model += (*o->fair - won) * (*o->fair - won) / n;
      r.brier_price += (price(o) - won) * (price(o) - won) / n;
    }
    r.gap_ci = bootstrap(by_round(scored), [](const auto &s, int) -> std::optional<double> {
      if (s.empty())
        return std::nullopt;
      double gap = 0;
      for (auto *o : s)
        gap += (o->won ? 1.0 : 0.0) - *o->fair;
      return gap / static_cast<double>(s.size());
    });
    static const std::array<std::pair<double, double>, 6> edges{
        {{0.0, 0.5}, {0.5, 0.6}, {0.6, 0.7}, {0.7, 0.8}, {0.8, 0.9}, {0.9, 1.0001}}};
    for (auto [low, high] : edges) {
      Bin b;
      std::set<std::string> rounds;
      std::vector<const Order *> bin_orders;
      double wins = 0;
      for (auto *o : scored)
        if (*o->fair >= low && *o->fair < high) {
          ++b.fills;
          rounds.insert(o->condition);
          wins += o->won ? 1 : 0;
          b.predicted += *o->fair;
          b.paid += o->paid();
          bin_orders.push_back(o);
        }
      if (!b.fills)
        continue;
      b.label = fmt::format("{:.0f}%-{:.0f}%", low * 100, std::min(high, 1.0) * 100);
      b.rounds = static_cast<int>(rounds.size());
      b.predicted /= b.fills;
      b.paid /= b.fills;
      b.actual = wins / b.fills;
      auto ci = bootstrap(by_round(bin_orders), [](const auto &s, int) -> std::optional<double> {
        double wins = 0;
        for (auto *o : s)
          wins += o->won ? 1 : 0;
        return wins / static_cast<double>(s.size());
      });
      if (ci) {
        std::tie(b.low, b.high) = *ci;
        b.has_interval = true;
        b.inside = b.low <= b.predicted && b.predicted <= b.high;
      }
      r.bins.push_back(b);
    }
  }
  if (!std::isfinite(r.net) || !std::isfinite(r.spent) || !std::isfinite(r.fees) ||
      !std::isfinite(r.max_drawdown))
    throw std::overflow_error("report totals are not representable");
  judge(r, min_rounds);
  return r;
}

inline std::string money(double v) {
  return v < 0 ? fmt::format("-${:.2f}", -v) : fmt::format("${:.2f}", v);
}
inline std::string pct(double v) { return fmt::format("{:.1f}%", v * 100); }
template <class F> std::string span(const Interval &ci, F show) {
  return ci ? show(ci->first) + " to " + show(ci->second) : "needs 2+ rounds";
}

inline std::string render(const Report &r, const std::string &path) {
  std::string s = fmt::format("EDGE REPORT  {}\n\nVERDICT: {}\n  {}\n\nSAMPLE\n", path, r.verdict, r.why);
  s += fmt::format("  settled fills {} in {} rounds\n", r.fills_settled, r.rounds);
  s += fmt::format("  left out: {} not settled yet, {} with no token match\n", r.unsettled, r.untraceable);
  s += fmt::format("  50/50 resolutions {}, refused {}, skipped rows {}, error rows {}\n", r.voided,
                   r.refused, r.skipped, r.errors);
  if (r.bad_lines)
    s += fmt::format("  unreadable journal lines {}\n", r.bad_lines);
  if (r.invalid_rows)
    s += fmt::format("  invalid/conflicting evidence rows {}\n", r.invalid_rows);
  if (r.untraceable)
    s += "  ! copy paper_audit.jsonl or ledger.json beside the journal to match them\n";
  if (r.has_profit) {
    s += "\nPROFIT (after fees, at settlement)\n";
    s += fmt::format("  net              {} on {} spent (fees {})\n", money(r.net), money(r.spent), money(r.fees));
    s += fmt::format("  return on spend  {}   95% interval {}\n", pct(r.return_on_spend), span(r.return_ci, pct));
    s += fmt::format("  per round        {}   95% interval {}\n", money(r.per_round), span(r.per_round_ci, money));
    s += fmt::format("  max round drawdown {} (settlement journal order)\n", money(r.max_drawdown));
    s += fmt::format("  first/second half of rounds  {} / {}\n", money(r.first_half), money(r.second_half));
  }
  if (r.has_calibration) {
    s += "\nCALIBRATION (does the predicted win chance come true?)\n";
    s += "  predicted   fills rounds   pred.  actual  95% interval    paid\n";
    for (auto &b : r.bins) {
      const auto interval = b.has_interval ? pct(b.low) + "-" + pct(b.high) : "needs 2+ rounds";
      s += fmt::format("  {:<10} {:>6} {:>6} {:>7} {:>7} {:>15} {:>6.3f}{}\n", b.label, b.fills,
                       b.rounds, pct(b.predicted), pct(b.actual), interval, b.paid,
                       b.has_interval && !b.inside ? "  <- off" : "");
    }
    s += fmt::format("  overall: predicted {}, actual {}, average price paid {:.3f}\n", pct(r.predicted),
                     pct(r.actual), r.paid);
    s += fmt::format("  actual minus predicted, 95% interval {}\n", span(r.gap_ci, pct));
    s += fmt::format("  Brier score (lower is better): model {:.4f}, price paid {:.4f}\n", r.brier_model,
                     r.brier_price);
    if (r.brier_model >= r.brier_price)
      s += "  ! the model predicts no better than the price it pays\n";
  }
  s += "\nPaper fills are simulated. Intervals describe the supplied settled sample,\n"
       "not a guarantee of future or live profitability.\n";
  return s;
}
} // namespace sniper::edge
