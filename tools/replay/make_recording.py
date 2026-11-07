#!/usr/bin/env python3
"""Deterministic synthetic session in the replay recording format.

Each JSONL line is one input both bots consume, in receive order:
  {"k":"cbconn","t":mono,"w":wall,"up":true}           Coinbase socket(s) up/down
  {"k":"cb","t":..,"w":..,"backlog":n,"raw":"..."}     one Coinbase frame
  {"k":"mconn","t":..,"w":..,"up":true}                market socket up/down
  {"k":"mkt","t":..,"w":..,"raw":"..."}                one market-socket frame
  {"k":"round","t":..,"w":..,"up":..,"down":..,"cond":..,"start":..,"end":..,
   "current":true|false,"rules":{...}}                 round discovery / promotion
  {"k":"rules","t":..,"w":..,"cond":..,"rules":{...}} refreshed cached market rules
  {"k":"rest_req","t":..,"w":..,"token":..}            REST /book request sent
  {"k":"rest_resp","t":..,"w":..,"token":..,"bids":[..],"asks":[..],
   "timestamp":"..","tick":0.01,"hash":".."}           its response, applied now
  {"k":"account","t":..,"w":..,"realized":x}           scripted realized P/L
Usage: make_recording.py OUT.jsonl --seed N --rounds R [--drawdown]
"""
import argparse
import hashlib
import json
import math
import random
from datetime import datetime, timezone


def iso(wall):
    return datetime.fromtimestamp(wall, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def phi(z):
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))


class Gen:
    def __init__(self, seed, rounds, drawdown, quirks):
        self.r = random.Random(seed)
        self.rounds = rounds
        self.drawdown = drawdown
        self.quirks = quirks
        self.ev = []
        # wall clock: 40s before a round boundary, mono starts at 1000
        self.w0 = 1793966400 - 40.0
        self.m0 = 1000.0

    def at(self, w):
        return self.m0 + (w - self.w0)

    def add(self, w, kind, **kw):
        self.ev.append((w, len(self.ev), dict(k=kind, t=round(self.at(w), 6), w=round(w, 6), **kw)))

    def token(self, start, side):
        return str(int(hashlib.sha256(f"{start}-{side}".encode()).hexdigest()[:30], 16))

    def cond(self, start):
        return "0x" + hashlib.sha256(f"cond-{start}".encode()).hexdigest()

    def run(self):
        r = self.r
        end_wall = self.w0 + 40 + 300 * self.rounds + 5
        # --- BTC path, 10 ms resolution
        btc, path, w = 60000.0, [], self.w0
        sigma = 1.0e-4  # per sqrt(s)
        while w < end_wall:
            dt = 0.01
            shock = r.gauss(0, sigma * math.sqrt(dt))
            if r.random() < 0.0006:
                shock += r.choice((-1, 1)) * r.uniform(0.4, 2.5) / 1e4
            btc *= math.exp(shock)
            path.append((w, btc))
            w += dt
        self.path = path

        def btc_at(wall):
            i = min(len(path) - 1, max(0, int((wall - self.w0) / 0.01)))
            return path[i][1]
        self.btc_at = btc_at

        # --- Coinbase: tickers on matches, heartbeats every second
        self.add(self.w0 + 0.05, "cbconn", up=True)
        seq = 1_000_000
        w = self.w0 + 0.1
        last_hb = w
        while w < end_wall:
            w += r.choice((0.02, 0.04, 0.08, 0.12, 0.2, 0.26, 0.32, 0.5))
            mid = btc_at(w)
            bid = math.floor(mid * 100 - r.choice((0, 1))) / 100
            ask = bid + r.choice((0.01, 0.01, 0.02, 0.05))
            lag = r.uniform(0.02, 0.07)
            if r.random() < 0.004:
                lag += r.uniform(1.2, 3.0)  # a stalled feed: "coinbase feed behind"
            seq += 1
            msg = {"type": "ticker", "sequence": seq, "product_id": "BTC-USD",
                   "price": f"{ask:.2f}", "open_24h": "60125.01",
                   "best_bid": f"{bid:.2f}", "best_bid_size": "0.08217445",
                   "best_ask": f"{ask:.2f}", "best_ask_size": "0.31221000",
                   "side": "buy", "trade_id": seq, "last_size": "0.00012",
                   "time": iso(w - lag)}
            if self.quirks and r.random() < 0.003:
                del msg["time"]  # no match time: "coinbase age unknown"
            backlog = 0 if r.random() > 0.004 else r.randint(21, 40)
            self.add(w, "cb", backlog=backlog, raw=json.dumps(msg, separators=(",", ":")))
            if self.quirks and r.random() < 0.05:   # the same ticker on the second socket
                self.add(w + r.uniform(0.001, 0.03), "cb", backlog=0,
                         raw=json.dumps(msg, separators=(",", ":")))
            if w - last_hb >= 1.0:
                last_hb = w
                self.add(w + 0.0005, "cb", backlog=0, raw=json.dumps(
                    {"type": "heartbeat", "sequence": seq, "last_trade_id": seq,
                     "product_id": "BTC-USD", "time": iso(w)}, separators=(",", ":")))
        # a Coinbase outage: both sockets down 4s
        if self.quirks:
            mid_w = self.w0 + 40 + 300 * (self.rounds // 2) + 137.0
            self.add(mid_w, "cbconn", up=False)
            self.add(mid_w + 4.0, "cbconn", up=True)
            self.ev = [e for e in self.ev if not (e[2]["k"] == "cb" and mid_w < e[0] < mid_w + 4.0)]

        # --- rounds, books
        self.add(self.w0 + 0.2, "mconn", up=True)
        starts = [int(self.w0 + 40) - 300 + 300 * i for i in range(self.rounds + 2)]
        rules = lambda s: {"condition_id": self.cond(s), "fee_rate": "0.07", "fee_exponent": 1,
                           "min_order_size": "5", "tick_size": "0.01",
                           "up_token_id": self.token(s, "up"), "down_token_id": self.token(s, "down"),
                           "taker_delay_ms": 0.0, "taker_delay_flagged": True}
        for i, s in enumerate(starts[1:self.rounds + 1], start=1):
            prev = starts[i - 1]
            if s - 30 >= self.w0:
                self.add(s - 29.5, "round", current=False, up=self.token(s, "up"), down=self.token(s, "down"),
                         cond=self.cond(s), start=s, end=s + 300, rules=rules(s))
            self.add(s + 0.3, "round", current=True, up=self.token(s, "up"), down=self.token(s, "down"),
                     cond=self.cond(s), start=s, end=s + 300, rules=rules(s))
        # round 0 (already running at start)
        s0 = starts[0]
        self.ev.append((self.w0 + 0.15, -1, dict(k="round", t=round(self.at(self.w0 + 0.15), 6),
                                                   w=round(self.w0 + 0.15, 6), current=True,
                                                   up=self.token(s0, "up"), down=self.token(s0, "down"),
                                                   cond=self.cond(s0), start=s0, end=s0 + 300, rules=rules(s0))))
        for s in starts[:self.rounds + 1]:
            self.book_process(s)

        # scripted realized P/L (hybrid drawdown)
        if self.drawdown:
            t = self.w0 + 40 + 300 * 1 + 50
            self.add(t, "account", realized=-120.0)      # 12% drawdown: smaller sizes
            self.add(t + 200, "account", realized=-230.0)  # 23%: the stop, then the pause
            self.add(t + 900, "account", realized=-100.0)

        self.ev.sort(key=lambda e: (e[0], e[1]))
        return [e[2] for e in self.ev]

    def book_process(self, start):
        """A market maker on one round's two tokens: quotes around a lagged
        estimate of the true probability; deltas, trades, REST refreshes."""
        r = self.r
        up, dn = self.token(start, "up"), self.token(start, "down")
        cond = self.cond(start)
        s0 = self.btc_at(start)
        live_from = max(self.w0 + 0.25, start - 29.0)
        until = start + 300 + 2
        lag = r.uniform(0.3, 1.5)
        w = live_from
        est = None
        books = {up: None, dn: None}
        sizes = {}
        seq = [0]
        disconnect_at = start + 160.0 if (self.quirks and start % 600 == 0) else None

        def quotes(p, spread):
            p = min(0.97, max(0.03, p))
            bid = math.floor(p * 100 - spread / 2) / 100
            bid = min(0.97, max(0.02, round(bid, 2)))
            ask = round(bid + spread / 100.0, 2)
            bids = [(round(bid - 0.01 * k, 2), sizes.setdefault((round(bid - 0.01 * k, 2), "b"),
                                                                 round(r.uniform(20, 400), 2)))
                    for k in range(4) if bid - 0.01 * k > 0.005]
            asks = [(round(ask + 0.01 * k, 2), sizes.setdefault((round(ask + 0.01 * k, 2), "a"),
                                                                 round(r.uniform(15, 300), 2)))
                    for k in range(4) if ask + 0.01 * k < 0.995]
            return bids, asks

        def ts(wall):
            return str(int(round((wall - r.uniform(0.015, 0.06)) * 1000)))

        def lv(levels):
            return [{"price": f"{p:.2f}", "size": f"{q:.2f}"} for p, q in levels]

        def snapshot(wall, kind="mkt"):
            frames = []
            for tok, b in books.items():
                if b is None:
                    continue
                frames.append({"event_type": "book", "asset_id": tok, "market": cond,
                               "bids": lv(b[0]), "asks": lv(b[1]),
                               "timestamp": ts(wall), "hash": hashlib.md5(f"{tok}{wall}".encode()).hexdigest()})
            if frames:
                self.add(wall, "mkt", raw=json.dumps(frames, separators=(",", ":")))

        first = True
        next_rest = {up: live_from + 1.6, dn: live_from + 1.7}
        while w < until:
            w += r.choice((0.05, 0.1, 0.15, 0.25, 0.4, 0.8, 1.5, 3.0))
            tau = max(1.0, start + 300 - w)
            x = math.log(self.btc_at(w - lag) / s0) if w > start else 0.0
            p = phi(x / (1.0e-4 * math.sqrt(tau)))
            est = p if est is None else est + (p - est) * min(1.0, r.uniform(0.2, 0.9))
            spread = r.choice((1, 1, 2, 2, 3))
            ub, ua = quotes(est, spread)
            db = [(round(1 - p_, 2), q) for p_, q in ua]
            da = [(round(1 - p_, 2), q) for p_, q in ub]
            new = {up: (ub, ua), dn: (db, da)}
            if first:
                books.update(new)
                snapshot(w)
                first = False
                continue
            if disconnect_at is not None and w >= disconnect_at:
                self.add(w, "mconn", up=False)
                self.add(w + 1.2, "mconn", up=True)
                books.update(new)
                snapshot(w + 1.5)
                disconnect_at = None
                w += 1.6
                continue
            changes = []
            for tok in (up, dn):
                old_b, old_a = books[tok]
                nb, na = new[tok]
                for side, old, nl in (("BUY", old_b, nb), ("SELL", old_a, na)):
                    om, nm = dict(old), dict(nl)
                    for p_ in set(om) | set(nm):
                        if om.get(p_) != nm.get(p_):
                            changes.append({"asset_id": tok, "price": f"{p_:.2f}",
                                            "size": f"{nm.get(p_, 0):.2f}" if p_ in nm else "0",
                                            "side": side, "hash": "h"})
            books.update(new)
            if changes:
                r.shuffle(changes)
                stamp = ts(w)
                if self.quirks and r.random() < 0.01:
                    stamp = str(int(stamp) - 4000)       # an older event: skipped
                if self.quirks and r.random() < 0.004:
                    changes.append({"asset_id": up, "price": "1.5", "size": "3", "side": "SELL"})  # invalid
                self.add(w, "mkt", raw=json.dumps({"event_type": "price_change", "market": cond,
                                                   "price_changes": changes, "timestamp": stamp},
                                                  separators=(",", ":")))
            if r.random() < 0.3:
                tok = r.choice((up, dn))
                px = books[tok][1][0][0] if books[tok][1] else 0.5
                self.add(w + 0.001, "mkt", raw=json.dumps(
                    {"event_type": "last_trade_price", "asset_id": tok, "market": cond, "price": f"{px:.2f}",
                     "size": "12.5", "side": "BUY", "timestamp": ts(w)}, separators=(",", ":")))
            if r.random() < 0.2:
                self.add(w + 0.002, "mkt", raw=json.dumps(
                    {"event_type": "best_bid_ask", "asset_id": up, "market": cond, "timestamp": ts(w)},
                    separators=(",", ":")))
            for tok in (up, dn):
                if w >= next_rest[tok]:
                    rt = r.uniform(0.05, 0.4)
                    b = books[tok]
                    self.add(w + 0.0003, "rest_req", token=tok)
                    self.add(w + rt, "rest_resp", token=tok, bids=lv(b[0]), asks=lv(b[1]),
                             timestamp=ts(w + rt * 0.5), tick=0.01, hash="r")
                    next_rest[tok] = w + r.uniform(1.5, 2.2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--drawdown", action="store_true")
    ap.add_argument("--no-quirks", action="store_true")
    a = ap.parse_args()
    events = Gen(a.seed, a.rounds, a.drawdown, not a.no_quirks).run()
    with open(a.out, "w") as fh:
        for e in events:
            fh.write(json.dumps(e, separators=(",", ":")) + "\n")
    print(f"{len(events)} events")


if __name__ == "__main__":
    main()
