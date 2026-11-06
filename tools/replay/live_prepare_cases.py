"""Run the reference's own LiveExecution.prepare (sniper_live.py) on stub
state for a few share-sized FAK cases; print the refusal text or the sized
order (spend, cap, shares, estimate) as JSON lines.
Usage: REF_PYTHON -I live_prepare_cases.py REPO
The expected values in tests/native_tests.cpp (Live.PresendCheck...) come
from this output."""
import json
import sys
import time
import types
from collections import Counter

sys.path.insert(0, sys.argv[1])
from decimal import Decimal  # noqa: E402

import sniper_live  # noqa: E402
from feeds.book import BookView  # noqa: E402
from paper_trade import MarketRules  # noqa: E402

CASES = [
    # name, asks, bids, cap, shares, tick, order_tick, order_min, venue_min
    ("ok", [(0.47, 100), (0.48, 50), (0.6, 10)], [(0.45, 30), (0.4, 5)], 0.5, 10, "0.01", "0.01", "5", None),
    ("clamped", [(0.9, 40)], [(0.88, 10)], 0.98, 10, "0.01", "0.01", "5", None),
    ("spread", [(0.47, 100)], [(0.2, 30)], 0.5, 10, "0.01", "0.01", "5", None),
    ("one-sided", [(0.47, 100)], [], 0.5, 10, "0.01", "0.01", "5", None),
    ("rules-changed", [(0.47, 100)], [(0.45, 30)], 0.5, 10, "0.01", "0.001", "5", None),
    ("below-min", [(0.47, 100)], [(0.45, 30)], 0.5, 3, "0.01", "0.01", "5", None),
    ("override-min", [(0.47, 100)], [(0.45, 30)], 0.5, 3, "0.01", "0.01", "5", 3),
    ("not-cent", [(0.471, 100)], [(0.45, 30)], 0.4759, 7, "0.001", "0.001", "5", None),
    ("under-1usd", [(0.09, 100)], [(0.08, 30)], 0.1, 5, "0.01", "0.01", "5", None),
    ("nothing-under-cap", [(0.6, 100)], [(0.45, 30)], 0.5, 10, "0.01", "0.01", "5", None),
    ("thin", [(0.47, 3), (0.49, 2), (0.7, 100)], [(0.45, 30)], 0.5, 10, "0.01", "0.01", "5", None),
    ("crossed", [(0.47, 100)], [(0.47, 30)], 0.5, 10, "0.01", "0.01", "5", None),
]


def stub(rules, order_rules, captured):
    s = types.SimpleNamespace()
    s.ROUND_S = 300.0
    import threading
    s._risk_lock = threading.RLock()
    s._risk_orders = {}
    s._balance_revision = 0
    s.kill = types.SimpleNamespace(check=lambda: None)
    s.fak_orders = {}
    s._faks_on_token = lambda token: []
    s.balance_at = time.monotonic()
    s.balance_max_age_s = 120.0
    s._cash_read_for = lambda start: True
    s.user_ready = None
    s.assumed_delay_s = 0.0
    s.n = Counter()
    s.rules_ready = None
    s.balance, s.reserved = 1000.0, 0.0
    s._check_loss_budget = lambda *a, **k: None
    s.shim = types.SimpleNamespace(cached_rules=lambda cond: rules, max_buy_price=0.95,
                                   max_spread=0.25, min_buy_price=0.05)

    def prepare(**kw):
        captured.update(kw)
        return types.SimpleNamespace(client_order_id='x', order_type=kw.get('order_type'))
    s.sender = types.SimpleNamespace(guard=types.SimpleNamespace(check=lambda: None),
                                     rules=types.SimpleNamespace(get=lambda c, u, d: order_rules,
                                                                 invalidate=lambda c: None),
                                     prepare=prepare)
    return s


for name, asks, bids, cap, shares, tick, otick, omin, vmin in CASES:
    rules = MarketRules(condition_id="c", fee_rate=Decimal("0.07"), fee_exponent=1,
                        min_order_size=Decimal("5"), tick_size=Decimal(tick),
                        up_token_id="u", down_token_id="d")
    order_rules = {"tick": Decimal(otick), "minimum": Decimal(omin), "fee_rate": Decimal("0.07"),
                   "fee_exponent": 1, "neg_risk": False}
    view = BookView(token="u", generation=1, status="live", bids=tuple(bids), asks=tuple(asks),
                    updated_mono=time.monotonic(), exchange_ts_ms=int(time.time() * 1000),
                    tick_size=float(tick))
    plan = {"order_type": "FAK", "minimum_entry": False, "token": "u", "cond": "c",
            "end": time.time() + 120, "send": ("UP", 5.0, "u", "d"), "cap": cap, "shares": shares,
            "view": view}
    if vmin is not None:
        plan["venue_min_shares"] = vmin
    captured = {}
    s = stub(rules, order_rules, captured)
    out = {"case": name}
    try:
        p = sniper_live.LiveExecution.prepare(s, plan)
        out.update(spend=repr(captured["amount"]), cap=repr(captured["cap"]),
                   shares=repr(captured["shares"]), reserve=repr(p.reserve),
                   est_shares=repr(p.estimate["shares"]), est_price=repr(p.estimate["price"]),
                   est_fee=repr(p.estimate["fee"]))
    except Exception as exc:  # noqa: BLE001
        out["refused"] = f"{type(exc).__name__}: {exc}"
    print(json.dumps(out))
