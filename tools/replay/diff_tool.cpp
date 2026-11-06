// sniper-diff: evaluate differential cases (JSONL on stdin) with the C++
// ports and print one JSON result per case. tools/replay/diff_cases.py
// generates the cases, evaluates them with the Python reference and
// compares. Kinds: fok, fak, fak_shares, min_entry, cent, budget,
// risk_total, factors, dd_stop, fair, fair_limit, kelly, fee, round, repr.
#include "sniper/math.hpp"
#include "sniper/paper.hpp"
#include <iostream>

using namespace sniper;
using py::D;
using py::Dec;

static Dec dec(const Json &v) {
  if (v.is_string())
    return D(std::string_view(v.get<std::string>()));
  if (v.is_number_integer())
    return D(v.get<long long>());
  return D(v.get<double>());
}
static Json fj(double v) { return Json(py::repr(v)); }

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;
    Json c = Json::parse(line), out;
    std::string k = c["kind"].get<std::string>();
    try {
      if (k == "fok" || k == "fak" || k == "fak_shares") {
        BookSnapshot b;
        b.token_id = "t";
        for (auto &l : c["asks"])
          b.asks.emplace_back(dec(l[0]), dec(l[1]));
        if (!c["book_tick"].is_null())
          b.tick_size = dec(c["book_tick"]);
        MarketRules r;
        r.fee_rate = dec(c["rate"]);
        r.fee_exponent = c["exp"].get<int>();
        if (!c["tick"].is_null())
          r.tick_size = dec(c["tick"]);
        if (!c["min_size"].is_null())
          r.min_order_size = dec(c["min_size"]);
        std::optional<Dec> ms;
        if (!c["min_shares"].is_null())
          ms = dec(c["min_shares"]);
        FillQuote q;
        if (k == "fok")
          q = estimate_fok(b, dec(c["amount"]), dec(c["max"]), r, dec(c["min"]), ms);
        else if (k == "fak")
          q = estimate_fak(b, dec(c["amount"]), dec(c["max"]), r, dec(c["min"]), ms);
        else
          q = estimate_fak_shares(b, dec(c["amount"]), dec(c["max"]), r,
                                  dec(c["min"]), ms);
        Json lv = Json::array();
        for (auto &[p, s, n] : q.levels)
          lv.push_back({p.str(), s.str(), n.str()});
        out = {{"notional", q.notional.str()}, {"shares", q.shares.str()},
               {"avg", q.average_price.str()}, {"worst", q.worst_price.str()},
               {"fee", q.fee.str()}, {"levels", lv}};
      } else if (k == "min_entry") {
        out = hybrid::minimum_entry_shares(dec(c["price"]), dec(c["vmin"]),
                                           dec(c["usd"]));
      } else if (k == "cent") {
        out = hybrid::cent_compatible_shares(dec(c["target"]), dec(c["price"]));
      } else if (k == "budget") {
        out = hybrid::shares_within_budget(dec(c["budget"]), dec(c["cap"]),
                                           dec(c["rate"]));
      } else if (k == "risk_total") {
        out = hybrid::risk_total(dec(c["spend"]), dec(c["rate"]), dec(c["quoted"]))
                  .str();
      } else if (k == "factors") {
        hybrid::Policy p;
        p.vol_baseline_bp = c["base"].get<double>();
        p.drawdown_stop = c["stop"].get<double>();
        p.risk_fraction = c["rf"].get<double>();
        p.max_order_usd = c["mo"].get<double>();
        p.validate();
        auto f = hybrid::sizing_factors(p, c["sigma"].get<double>(),
                                        c["equity"].get<double>(),
                                        c["hw"].get<double>(), c["frac"].get<double>());
        out = {{"vol", fj(f.volatility_scale)}, {"dd", fj(f.drawdown)},
               {"dds", fj(f.drawdown_scale)}, {"eff", fj(f.effective_fraction)},
               {"budget", f.risk_budget_usd ? fj(*f.risk_budget_usd) : Json()}};
      } else if (k == "dd_stop") {
        hybrid::Policy p;
        p.drawdown_stop = c["stop"].get<double>();
        out = hybrid::at_drawdown_stop(p, c["equity"].get<double>(),
                                       c["hw"].get<double>());
      } else if (k == "fair") {
        out = fj(anchored_fair(c["mid"].get<double>(), c["bp"].get<double>(),
                               c["sigma"].get<double>(), c["tau"].get<double>()));
      } else if (k == "fair_limit") {
        out = fj(fair_limit(c["fair"].get<double>(), c["margin"].get<double>(),
                            c["tick"].get<double>(), c["rate"].get<double>(),
                            c["exp"].get<int>()));
      } else if (k == "kelly") {
        auto [s, f] = kelly_shares(
            c["fair"].get<double>(), c["ask"].get<double>(), c["rate"].get<double>(),
            c["frac"].get<double>(), c["bank"].get<double>(), c["room"].get<double>(),
            c["held"].get<double>(), c["opp"].get<double>(), c["exp"].get<int>());
        out = {fj(s), fj(f)};
      } else if (k == "fee") {
        out = fj(taker_fee(c["p"].get<double>(), c["rate"].get<double>(),
                           c["exp"].get<int>()));
      } else if (k == "round") {
        out = fj(py::round(c["x"].get<double>(), c["n"].get<int>()));
      } else if (k == "repr") {
        out = py::repr(c["x"].get<double>());
      }
    } catch (const std::exception &e) {
      out = {{"err", e.what()}};
    }
    std::cout << out.dump() << "\n";
  }
}
