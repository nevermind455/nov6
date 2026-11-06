#!/usr/bin/env python3
"""Differential test: random cases through the Python reference functions
(paper_trade.estimate_fok/_fak/_fak_shares, hybrid_sizing.*, sniper_mode's
anchored_fair/fair_limit/kelly_shares/taker_fee, round, repr) and through
the C++ ports (sniper-diff); every result must be identical, including the
text of every refusal.

Usage: python -I diff_cases.py REPO SNIPER_DIFF_BIN [N]
"""
import json
import random
import subprocess
import sys
from decimal import Decimal


def cases(n, r):
    out = []
    for i in range(n):
        k = r.choice(("fok", "fak", "fak_shares", "fok", "fak", "min_entry", "cent", "budget", "risk_total",
                      "factors", "dd_stop", "fair", "fair_limit", "kelly", "fee", "round", "repr"))
        if k in ("fok", "fak", "fak_shares"):
            tick = r.choice(("0.01", "0.01", "0.001", None, "0.1"))
            t = float(tick or "0.01")
            levels, p = [], r.uniform(0.02, 0.9)
            for _ in range(r.randint(0, 6)):
                p += t * r.randint(0, 4)
                if p >= 0.995:
                    break
                px = round(round(p / t) * t, 4)
                levels.append([repr(px) if r.random() < 0.5 else f"{px}",
                               r.choice((f"{r.uniform(0.01, 400):.2f}", f"{r.uniform(1, 50):.6f}", "5", "0.5"))])
            levels.sort(key=lambda x: float(x[0]))
            amount = (r.choice((r.randint(1, 40), round(r.uniform(0.3, 60), 2), round(r.uniform(0.3, 60), 6)))
                      if k != "fak_shares" else r.choice((5, 7, 10, 13, 25, 0, 4.5)))
            c = {"asks": levels, "book_tick": r.choice((None, tick)), "rate": r.choice(("0.07", "0", "0.02")),
                 "exp": r.choice((1, 1, 2)), "tick": tick, "min_size": r.choice(("5", None, "15")),
                 "min_shares": r.choice((None, None, "3")), "amount": str(amount),
                 "max": repr(r.choice((0.95, round(r.uniform(0.05, 0.99), 4), 0.5700000000000001))),
                 "min": repr(r.choice((0.05, 0.0, 0.2)))}
        elif k == "min_entry":
            c = {"price": repr(r.choice((0.5, 0.501, 0.15, 0.07, 0.33, 0.955, round(r.uniform(0.01, 0.99), 3)))),
                 "vmin": r.choice(("5", "3", "5.5")), "usd": "1.0"}
        elif k == "cent":
            c = {"target": repr(r.choice((9.0, 13.0, r.uniform(0, 200), 0.0))),
                 "price": repr(r.choice((0.5, 0.501, 0.15, 0.955, round(r.uniform(0.01, 0.99), 3))))}
        elif k == "budget":
            c = {"budget": repr(r.choice((10.0, 100.0, 3.0, r.uniform(0, 2000), 987.6543210000001))),
                 "cap": repr(r.choice((0.54, 0.51, round(r.uniform(0.01, 0.99), 4)))),
                 "rate": repr(r.choice((0.07, 0.0, 0.02)))}
        elif k == "risk_total":
            c = {"spend": repr(r.choice((4.86, r.uniform(0, 100), 5.1))), "rate": repr(r.choice((0.07, 0.0))),
                 "quoted": repr(r.choice((0, 0.3402, r.uniform(0, 1))))}
        elif k == "factors":
            hw = r.uniform(50, 2000)
            c = {"base": r.choice((1.0, 0.7)), "stop": r.choice((0.2, 1.0, 0.5)), "rf": r.choice((0.0, 0.01)),
                 "mo": r.choice((0.0, 5.0)), "sigma": r.uniform(0.00002, 0.0005),
                 "equity": r.choice((hw, hw * r.uniform(0.5, 1), 98.76)), "hw": r.choice((hw, 123.45)),
                 "frac": r.choice((0.25, 0.5))}
            if c["equity"] > c["hw"]:
                c["equity"] = c["hw"]
        elif k == "dd_stop":
            c = {"stop": r.choice((0.2, 1.0)), "equity": r.choice((98.76, 80.0, r.uniform(0, 100))),
                 "hw": r.choice((123.45, 100.0))}
        elif k == "fair":
            c = {"mid": r.choice((0.5, 0.7, 0.3, 0.95, 0.002, r.uniform(0, 1))), "bp": r.uniform(0.3, 5),
                 "sigma": r.uniform(0.00002, 0.0004), "tau": r.uniform(1, 300)}
        elif k == "fair_limit":
            c = {"fair": r.uniform(0, 1), "margin": r.choice((0.01, 0.0, 0.03)), "tick": r.choice((0.01, 0.001)),
                 "rate": r.choice((0.07, 0.0)), "exp": r.choice((1, 2))}
        elif k == "kelly":
            c = {"fair": r.uniform(0.3, 0.99), "ask": r.uniform(0.05, 0.95), "rate": r.choice((0.07, 0.0)),
                 "frac": r.uniform(0.01, 0.5), "bank": r.choice((r.uniform(0, 3000), 0.0)),
                 "room": float(r.randint(0, 200)), "held": r.choice((0.0, r.uniform(0, 50))),
                 "opp": r.choice((0.0, r.uniform(0, 50))), "exp": r.choice((1, 2))}
        elif k == "fee":
            c = {"p": r.uniform(0, 1), "rate": r.choice((0.07, 0.02)), "exp": r.choice((1, 2))}
        elif k == "round":
            c = {"x": r.choice((r.uniform(-1e4, 1e4), r.uniform(0, 1), 0.125, 2.675, 1e-5)), "n": r.randint(0, 6)}
        else:
            c = {"x": r.choice((r.uniform(-1e6, 1e6), r.uniform(0, 1e-5), 1e16 * r.random(), 1e22, 0.1 + 0.2))}
        c["kind"] = k
        out.append(c)
    return out


def python_eval(c):
    import hybrid_sizing as H
    import paper_trade as P
    import sniper_mode as S
    k = c["kind"]
    fj = repr
    try:
        if k in ("fok", "fak", "fak_shares"):
            book = P.BookSnapshot(token_id="t", asks=tuple((Decimal(p), Decimal(s)) for p, s in c["asks"]),
                                  tick_size=None if c["book_tick"] is None else Decimal(c["book_tick"]))
            rules = P.MarketRules(condition_id="c", fee_rate=Decimal(c["rate"]), fee_exponent=c["exp"],
                                  min_order_size=None if c["min_size"] is None else Decimal(c["min_size"]),
                                  tick_size=None if c["tick"] is None else Decimal(c["tick"]))
            ms = None if c["min_shares"] is None else Decimal(c["min_shares"])
            amt, mx, mn = Decimal(c["amount"]), Decimal(c["max"]), Decimal(c["min"])
            if k == "fok":
                q = P.estimate_fok(book, amt, mx, rules, min_price=mn, min_shares=ms)
            elif k == "fak":
                q = P.estimate_fak(book, amt, mx, rules, min_price=mn, min_shares=ms)
            else:
                q = P.estimate_fak_shares(book, amt, mx, rules, min_price=mn, min_shares=ms)
            return {"notional": str(q.notional), "shares": str(q.shares), "avg": str(q.average_price),
                    "worst": str(q.worst_price), "fee": str(q.fee),
                    "levels": [[str(a), str(b), str(n)] for a, b, n in q.levels]}
        if k == "min_entry":
            return H.minimum_entry_shares(Decimal(c["price"]), Decimal(c["vmin"]), Decimal(c["usd"]))
        if k == "cent":
            return H.cent_compatible_shares(Decimal(c["target"]), Decimal(c["price"]))
        if k == "budget":
            return H.shares_within_budget(Decimal(c["budget"]), Decimal(c["cap"]), Decimal(c["rate"]))
        if k == "risk_total":
            return str(H.risk_total(Decimal(c["spend"]), Decimal(c["rate"]), Decimal(c["quoted"])))
        if k == "factors":
            p = H.HybridPolicy(vol_baseline_bp=c["base"], drawdown_stop=c["stop"], risk_fraction=c["rf"],
                               max_order_usd=c["mo"])
            f = H.sizing_factors(p, sigma=c["sigma"], equity=c["equity"], high_water=c["hw"],
                                 base_fraction=c["frac"])
            return {"vol": fj(f["volatility_scale"]), "dd": fj(f["drawdown"]), "dds": fj(f["drawdown_scale"]),
                    "eff": fj(f["effective_fraction"]),
                    "budget": None if f["risk_budget_usd"] is None else fj(f["risk_budget_usd"])}
        if k == "dd_stop":
            return H.at_drawdown_stop(H.HybridPolicy(drawdown_stop=c["stop"]), equity=c["equity"], high_water=c["hw"])
        if k == "fair":
            return fj(S.anchored_fair(c["mid"], c["bp"], c["sigma"], c["tau"]))
        if k == "fair_limit":
            return fj(S.fair_limit(c["fair"], c["margin"], c["tick"], c["rate"], c["exp"]))
        if k == "kelly":
            s, f = S.kelly_shares(c["fair"], c["ask"], c["rate"], c["frac"], c["bank"], c["room"],
                                  held_shares=c["held"], opposite_shares=c["opp"], fee_exponent=c["exp"])
            return [fj(float(s)), fj(float(f))]
        if k == "fee":
            return fj(S.taker_fee(c["p"], c["rate"], c["exp"]))
        if k == "round":
            return fj(round(c["x"], c["n"]))
        return repr(c["x"])
    except Exception as exc:  # noqa: BLE001 - the refusal text is compared too
        return {"err": str(exc)}


def main():
    repo, tool = sys.argv[1], sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 20000
    sys.path.insert(0, repo)
    r = random.Random(20261106)
    cs = cases(n, r)
    want = [python_eval(c) for c in cs]
    proc = subprocess.run([tool], input="\n".join(json.dumps(c) for c in cs) + "\n",
                          capture_output=True, text=True, check=True)
    got = [json.loads(x) for x in proc.stdout.splitlines()]
    assert len(got) == len(cs), (len(got), len(cs))
    bad = [(c, w, g) for c, w, g in zip(cs, want, got) if w != g]
    from collections import Counter
    kinds = Counter(c["kind"] for c in cs)
    errs = Counter(c["kind"] for c, w in zip(cs, want) if isinstance(w, dict) and "err" in w)
    print(f"{len(cs)} cases {dict(kinds)}; refusals compared {dict(errs)}")
    for c, w, g in bad[:8]:
        print("MISMATCH", json.dumps(c)[:300], "\n   python:", json.dumps(w)[:300], "\n   c++:   ", json.dumps(g)[:300])
    print(f"mismatches: {len(bad)}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
