#!/usr/bin/env python3
"""Replay one recording through the PYTHON reference bot's own decision code.

The reference modules (sniper_mode.Sniper, feeds.book.BookState) run
unmodified on a replay clock, as the reference's own tests/tests_latency.py
replay does. The broker is scripted (the n-th order fills a fixed fraction)
and so is the account, so both bots see the same fills and cash; the C++
driver (sniper-replay) scripts them identically. Orders resolve at their
modelled arrival time, bankroll and hybrid refreshes run every 5 s of replay
time, exactly as in tools/replay/sniper_replay.cpp.

Usage: python -I py_replay.py REPO RECORDING.jsonl OUT.jsonl [CONFIG.json]
"""
import asyncio
import concurrent.futures
import json
import math
import os
import pathlib
import sys
import tempfile
import time as _time
import types
from decimal import Decimal
from unittest.mock import patch

BASE_ENV = {
    "SNIPER_ONLY": "1", "SNIPER_PAPER_LIVE_PARITY": "0", "SNIPER_DELAY_SOURCE": "model",
    "SNIPER_FILL_ODDS": "off", "SNIPER_SHADOW_MS": "", "SNIPER_BOOK_TRACE_MS": "",
    "SNIPER_GUARD_DIAG": "0", "SNIPER_PAPER_CASH_FROM": "off", "SNIPER_LATENCY_MS": "",
    "MIN_BUY_PRICE": "0.05", "MAX_BUY_PRICE": "0.95", "TRADE_LAST_SECONDS": "300",
    "MIN_SECONDS_TO_EXPIRY": "10",
}
FRACTIONS = (1.0, 0.6, 0.0, 0.3, 1.0, 0.0)
START_CASH = 1000.0
REFRESH_S = 5.0


class Clock:
    def __init__(self):
        self.mono, self.wall = 0.0, 0.0

    def __getattr__(self, name):
        return getattr(_time, name)

    def monotonic(self):
        return self.mono

    def time(self):
        return self.wall


class Ledger:
    def __init__(self):
        self.positions = {}
        self.realized = 0.0
        self._state = None
        self.journal_seq = 0

    def book(self, cond, token, shares, cost):
        p = self.positions.setdefault(token, {"cond": cond, "shares": 0.0, "cost": 0.0})
        p["shares"] += shares
        p["cost"] += cost
        self.journal_seq += 1

    def open_leg_shares(self, cond, token):
        p = self.positions.get(str(token))
        if p is None or p["cond"] != str(cond) or p["shares"] <= 0:
            return 0.0
        return float(p["shares"])

    @property
    def hybrid_risk(self):
        return None if self._state is None else dict(self._state)

    def risk_view(self, since_wall, orders):
        return 0.0, sum(p["cost"] for p in self.positions.values() if p["shares"] > 0), {}

    def hybrid_risk_view(self, reference_equity=None, *, expected_journal_seq=None, rebase=False):
        realized = self.realized
        state = self._state
        if state is None:
            if reference_equity is None:
                raise ValueError("first hybrid risk reading needs positive reference equity")
            if reference_equity <= 0:
                raise ValueError("hybrid reference equity must be positive")
            state = {"reference_equity": float(reference_equity), "pnl_baseline": realized,
                     "high_water": float(reference_equity)}
        equity = state["reference_equity"] + realized - state["pnl_baseline"]
        if rebase:
            state = {**state, "reference_equity": equity, "pnl_baseline": realized, "high_water": equity}
        state = dict(state)
        state["high_water"] = max(state["high_water"], equity)
        self._state = state
        return {**state, "equity": equity, "realized_total": realized}


class Broker:
    trade_window_seconds = 300.0
    min_seconds_to_expiry = 10.0
    min_buy_price = 0.05
    max_buy_price = 0.95
    own_fill_ttl_s = 0
    supports_share_sized = True
    supports_hybrid_risk_guard = True
    supports_venue_min_override = True
    supports_size_at_limit = True

    def __init__(self, ledger):
        self.ledger = ledger
        self.n = 0
        self.last_fill = None
        self.last_error = None
        self._rules_cache = {}
        self._rules_cache_ttl = 300.0

    def cash_balance(self):
        return START_CASH - sum(p["cost"] for p in self.ledger.positions.values()) + self.ledger.realized

    def place_trade(self, side, amount, up, down, cond, end, cap, _x, latency_ms=None, tag=None, **kw):
        f = FRACTIONS[self.n % len(FRACTIONS)]
        self.n += 1
        if f == 0:
            self.last_fill, self.last_error = None, "FAK no-fill: nothing at or below the cap"
            return False
        price = round(cap - 0.03, 4)
        shares = round(amount * f / price, 4)
        cost = round(shares * price, 4)
        token = up if str(side).upper() == "UP" else down
        self.ledger.book(cond, str(token), shares, cost)
        self.last_error = None
        self.last_fill = {"tag": tag, "order_id": f"r{self.n}", "shares": shares, "average_price": price,
                          "total_cost": cost, "fee": 0.0, "partial": f < 1.0}
        return True


class Pool(concurrent.futures.Executor):
    """run_in_executor target: orders wait here until their arrival time."""

    def __init__(self):
        self.pending = []

    def submit(self, fn, *args, **kw):
        fut = concurrent.futures.Future()
        plan = args[0]
        release = float(plan["sent_mono"]) + float(plan["lat"]) / 1000.0
        self.pending.append((release, len(self.pending) + 1_000_000 * 0, fn, args, kw, fut))
        return fut


async def replay(repo, rec_path, out_path, cfg):
    sys.path.insert(0, repo)
    env = dict(BASE_ENV, **cfg)
    clock = Clock()
    with patch.dict(os.environ, env):
        import timer as TM
        import feeds.book as FB
        import sniper_mode as S
        from paper_trade import MarketRules
        settings = S.Settings()
    events = [json.loads(x) for x in pathlib.Path(rec_path).read_text().splitlines() if x.strip()]
    clock.mono, clock.wall = events[0]["t"], events[0]["w"]
    offset = events[0]["w"] - events[0]["t"]
    journal = pathlib.Path(tempfile.mkdtemp(prefix="pyreplay_")) / "j.jsonl"
    ledger = Ledger()
    broker = Broker(ledger)
    with patch.object(S, "time", clock), patch.object(FB, "time", clock), patch.object(TM, "time", clock):
        book = FB.BookState(history_s=30)
        hub = types.SimpleNamespace(up_token=None, down_token=None, condition_id=None, window_end=None,
                                    book=book, prepared_round=None)
        sn = S.Sniper(hub, broker, journal_path=journal, settings=settings, trade_rows=[], own_rounds=True)
        pool = Pool()
        sn._match_pool = pool
        sn.cb_fresh.enabled = settings.coinbase_freshness

        async def no_markout(*a, **k):
            return None
        sn._markout = no_markout
        finished = {"n": 0}
        original_finish = sn._finish

        async def finish(*a, **k):
            try:
                await original_finish(*a, **k)
            finally:
                finished["n"] += 1
        sn._finish = finish
        cb_up = False
        prepared = None
        current = None
        rest_views = {}
        fetched = set()
        next_bankroll = events[0]["t"]
        next_hybrid = events[0]["t"] + REFRESH_S

        def set_clock(mono):
            clock.mono, clock.wall = mono, mono + offset

        def tokens():
            out = []
            for r in (current, prepared):
                if r:
                    out += [r["up"], r["down"]]
            return out

        def cache_rules(r, wall):
            if r["cond"] in fetched:
                return
            fetched.add(r["cond"])
            x = r["rules"]
            rules = MarketRules(condition_id=x["condition_id"], fee_rate=Decimal(x["fee_rate"]),
                                fee_exponent=int(x["fee_exponent"]),
                                min_order_size=Decimal(x["min_order_size"]),
                                tick_size=Decimal(x["tick_size"]), up_token_id=x["up_token_id"],
                                down_token_id=x["down_token_id"], taker_delay_ms=x["taker_delay_ms"],
                                taker_delay_flagged=x["taker_delay_flagged"])
            broker._rules_cache[r["cond"]] = (rules, wall)

        async def settle_orders(until):
            nonlocal next_bankroll, next_hybrid
            while True:
                due = [p for p in pool.pending if p[0] <= until]
                timers = [x for x in (next_bankroll, next_hybrid) if x <= until]
                if not due and not timers:
                    return
                t_order = min((p[0] for p in due), default=math.inf)
                t_timer = min(timers, default=math.inf)
                if t_order <= t_timer:
                    item = min(due, key=lambda p: p[0])
                    pool.pending.remove(item)
                    release, _seq, fn, args, kw, fut = item
                    set_clock(release)
                    before = finished["n"]
                    try:
                        fut.set_result(fn(*args, **kw))
                    except BaseException as exc:  # noqa: BLE001
                        fut.set_exception(exc)
                    for _ in range(2000):
                        if finished["n"] > before:
                            break
                        await asyncio.sleep(0)
                else:
                    set_clock(t_timer)
                    if next_bankroll <= t_timer:
                        sn._set_bankroll(sn._read_bankroll(), clock.mono)
                        next_bankroll += REFRESH_S
                    if next_hybrid <= t_timer:
                        sn._refresh_hybrid_risk()
                        next_hybrid += REFRESH_S

        for e in events:
            await settle_orders(e["t"])
            set_clock(e["t"])
            k = e["k"]
            if k == "cb":
                sn.cb_fresh.note_message(clock.mono, e.get("backlog"))
                try:
                    sn.on_coinbase(e["raw"])
                except Exception:  # noqa: BLE001 - a refused ack reconnects that socket
                    pass
                for _ in range(3):
                    await asyncio.sleep(0)
            elif k == "cbconn":
                if e["up"] and not cb_up:
                    sn.trigger = S.Trigger(settings.k_bp, settings.window_ms)
                    sn.triggers["coinbase"] = sn.trigger
                    sn.last_mid = None
                    sn._coinbase_seq = sn._coinbase_prev_t = None
                    sn._cb_first.clear()
                    sn.cb_fresh.on_connect()
                elif not e["up"] and cb_up:
                    sn.cb_fresh.on_disconnect()
                cb_up = bool(e["up"])
            elif k == "mconn":
                book.connected = bool(e["up"])
                book.desync_all("connect" if e["up"] else "disconnect")
            elif k == "mkt":
                text = e["raw"]
                msg = json.loads(text)
                for item in (msg if isinstance(msg, list) else [msg]):
                    if isinstance(item, dict):
                        _dispatch(book, item)
            elif k == "round":
                r = {x: e[x] for x in ("up", "down", "cond", "start", "end", "rules")}
                if e["current"]:
                    current = r
                    if prepared and prepared["cond"] == r["cond"]:
                        prepared = None
                    hub.up_token, hub.down_token = r["up"], r["down"]
                    hub.condition_id, hub.window_end = r["cond"], r["end"]
                else:
                    prepared = r
                book.set_active(tokens())
                cache_rules(r, clock.wall)
            elif k == "rest_req":
                v = book.view(e["token"])
                age = v.book_age_ms()
                if v.status == "LIVE" and age is not None and age < 1500.0:
                    rest_views.pop(e["token"], None)
                else:
                    rest_views[e["token"]] = v
            elif k == "rest_resp":
                v = rest_views.pop(e["token"], None)
                if v is not None and e["token"] in book.active:
                    book.apply_snapshot(e["token"], e["bids"], e["asks"], only_if_unsynced=True,
                                        expected_generation=v.generation, expected_updates=v.updates,
                                        expected_receive_revision=v.receive_revision,
                                        refresh_if_older_than=1.5, source="rest", ts_ms=e["timestamp"],
                                        received_mono=clock.mono, tick=e["tick"], hash_=e["hash"],
                                        require_exchange_ts=True)
            elif k == "ws_gap":
                book.subscription_gap(e["token"], int(e["generation"]))
            elif k == "account":
                ledger.realized = float(e["realized"])
        await settle_orders(events[-1]["t"] + 30.0)
        for task in list(sn._tasks):
            task.cancel()
        await asyncio.gather(*sn._tasks, return_exceptions=True)
    rows = journal.read_text(encoding="utf-8").splitlines() if journal.exists() else []
    pathlib.Path(out_path).write_text("\n".join(rows) + ("\n" if rows else ""))
    return len(rows)


def _dispatch(book, m):
    """feeds/poly_market.PolyMarketFeed._dispatch's book effects."""
    et = m.get("event_type")
    if et == "book":
        book.apply_snapshot(str(m.get("asset_id")), m.get("bids"), m.get("asks"),
                            ts_ms=m.get("timestamp"), hash_=m.get("hash"), require_exchange_ts=True)
    elif et == "price_change":
        book.apply_price_changes(m.get("price_changes"), ts_ms=m.get("timestamp"), hash_=m.get("hash"),
                                 require_exchange_ts=True)
    elif et == "tick_size_change":
        book.set_tick_size(str(m.get("asset_id")), m.get("new_tick_size"), ts_ms=m.get("timestamp"),
                           require_exchange_ts=m.get("timestamp") is not None)
    elif et == "last_trade_price":
        token = str(m.get("asset_id") or "")
        if token not in book.active:
            book.note_inactive_drop()
            return
        try:
            if isinstance(m.get("price"), bool) or isinstance(m.get("timestamp"), bool):
                raise ValueError
            price = float(m.get("price"))
            ts = int(m.get("timestamp"))
        except (TypeError, ValueError, OverflowError):
            return
        if not math.isfinite(price) or not 0 < price < 1 or not book._fresh_exchange_ts(ts):
            return
        book.note_exchange_ts(ts)
    elif et == "best_bid_ask":
        if str(m.get("asset_id") or "") in book.active:
            book.note_exchange_ts(m.get("timestamp"))


def main():
    repo, rec, out = sys.argv[1:4]
    cfg = json.loads(pathlib.Path(sys.argv[4]).read_text()) if len(sys.argv) > 4 else {}
    n = asyncio.run(replay(repo, rec, out, cfg))
    print(f"{n} journal rows")


if __name__ == "__main__":
    main()
