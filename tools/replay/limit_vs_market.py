"""The reference signs a share-sized live FAK as a limit order (size, price);
this program signs it as an amount order (shares x price, price). Check with
the pinned SDK's own functions that both give the same maker/taker amounts,
for every on-grid price of four tick sizes and 1-300 shares at whole cents.
Usage: REF_PYTHON -I limit_vs_market.py REPO   (REF_PYTHON has the SDK)"""
import sys
sys.path.insert(0, sys.argv[1])
from decimal import Decimal
from polymarket._internal.actions.orders.limit import _compute_limit_order_amounts
from polymarket._internal.actions.orders.market import _compute_market_order_amounts
from polymarket._internal.actions.orders.context import resolve_rounding_config
from polymarket._internal.actions.orders.math import round_normal
from sniper_mode import signing_price, signing_amount

total = diff = 0
examples = []
for tick in ("0.1", "0.01", "0.001", "0.0001"):
    rc = resolve_rounding_config(Decimal(tick))
    t = Decimal(tick)
    steps = int(1 / t)
    for k in range(1, steps):
        p = k * t
        for shares in range(1, 301):
            spend = shares * p
            if spend != spend.quantize(Decimal("0.01")):
                continue   # the reference refuses: not a whole-cent amount
            total += 1
            sp = signing_price(float(p), tick)
            lim = _compute_limit_order_amounts(price=round_normal(Decimal(str(sp)), rc.price),
                                               size=Decimal(str(float(shares))), side="BUY", tick_size=t)
            mkt = _compute_market_order_amounts(amount=Decimal(str(signing_amount(float(spend)))),
                                                price=Decimal(str(sp)), side="BUY", tick_size=t)
            if tuple(map(int, lim)) != tuple(map(int, mkt)):
                diff += 1
                if len(examples) < 5:
                    examples.append((tick, str(p), shares, lim, mkt))
print("cases", total, "different", diff)
for e in examples:
    print(e)
sys.exit(1 if diff else 0)
