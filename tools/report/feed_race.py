#!/usr/bin/env python3
"""Which BTC feed fires the bot's trigger first ON THIS SERVER: Coinbase (the
bot's own feed), Binance spot or Binance USD-M futures.

Read-only: public market data only, no keys, no orders, no file written.
Standard Python 3 only (nothing to install). Runs at the lowest CPU priority.

    python3 feed_race.py [minutes]        # default 60; Ctrl+C prints the report so far

Each feed's mid price (best bid + best ask) / 2 goes through the bot's own
trigger rule: a move of at least SNIPER_K_BP (0.5) bp from the price
SNIPER_WINDOW_MS (250) ms earlier. A feed's move starts at its first signal
after a second with no signal that way. Moves the same way on different feeds
within a second of each other are one move; the report says which feed fired
first and how many milliseconds the others came after. Times are when each
message reached this server, so they include each exchange's distance from it.
"""
import asyncio, base64, json, os, signal, ssl, struct, sys, time
from collections import Counter, deque
from datetime import datetime
from urllib.parse import urlparse

K_BP = float(os.environ.get("SNIPER_K_BP", "0.5"))
WINDOW_MS = float(os.environ.get("SNIPER_WINDOW_MS", "250"))
MAX_GAP_MS = max(2000.0, WINDOW_MS)  # as the bot: a longer gap restarts the rule
QUIET_MS = 1000.0                    # a new move on a feed: no signal that way for 1 s
SAME_MOVE_MS = 1000.0                # the same move on another feed: started within 1 s

FEEDS = {
    "coinbase": ("wss://ws-feed.exchange.coinbase.com",
                 {"type": "subscribe", "product_ids": ["BTC-USD"], "channels": ["ticker"]}),
    "binance spot": ("wss://stream.binance.com:9443/ws/btcusdt@bookTicker", None),
    "binance futures": ("wss://fstream.binance.com/ws/btcusdt@bookTicker", None),
}
for name, url in json.loads(os.environ.get("FEED_RACE_URLS", "{}")).items():  # tests only
    FEEDS[name] = (url, FEEDS[name][1])
NAMES = list(FEEDS)


def now_ms():
    return time.monotonic() * 1000.0


def iso_ms(text):
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp() * 1000.0
    except (AttributeError, ValueError):
        return None


def parse(name, raw):
    """(mid, exchange time in ms or None) from one message, or None."""
    d = json.loads(raw)
    if not isinstance(d, dict):
        return None
    if name == "coinbase":
        if d.get("type") != "ticker" or d.get("product_id") != "BTC-USD":
            return None
        bid, ask, ts = d.get("best_bid"), d.get("best_ask"), iso_ms(d.get("time"))
    else:
        bid, ask, ts = d.get("b"), d.get("a"), d.get("E")  # spot carries no event time
    try:
        bid, ask = float(bid), float(ask)
    except (TypeError, ValueError):
        return None
    if not 0 < bid <= ask:
        return None
    return (bid + ask) / 2, ts


class Trigger:
    """The bot's rule, as include/sniper/trigger.hpp: +1 / -1 / 0."""

    def __init__(self):
        self.q = deque()

    def update(self, t, mid):
        q = self.q
        if q and (t <= q[-1][0] or t - q[-1][0] > MAX_GAP_MS):
            q.clear()
        q.append((t, mid))
        cut = t - WINDOW_MS
        while len(q) >= 2 and q[1][0] <= cut:
            q.popleft()
        if q[0][0] > cut or t - q[0][0] > MAX_GAP_MS:
            return 0
        r = (mid / q[0][1] - 1.0) * 1e4
        return 1 if r >= K_BP - 1e-9 else -1 if r <= -K_BP + 1e-9 else 0


class Race:
    def __init__(self):
        self.last = {(n, d): -1e18 for n in NAMES for d in (1, -1)}
        self.open, self.done = [], []
        self.moves = Counter()

    def signal(self, name, way, t):
        quiet = t - self.last[(name, way)] > QUIET_MS
        self.last[(name, way)] = t
        if not quiet:
            return  # the move this feed already reported is still going
        self.moves[name] += 1
        self.close(t)
        for m in self.open:
            if m["way"] == way and name not in m["at"] and t - m["first"] <= SAME_MOVE_MS:
                m["at"][name] = t
                return
        self.open.append({"way": way, "first": t, "at": {name: t}})

    def close(self, t):
        still = []
        for m in self.open:
            (self.done if t - m["first"] > SAME_MOVE_MS else still).append(m)
        self.open = still


class Stats:
    def __init__(self):
        self.msgs, self.reconnects, self.errors = Counter(), Counter(), {}
        self.age = {n: deque(maxlen=20000) for n in NAMES}


def pct(values, q):
    v = sorted(values)
    return v[min(len(v) - 1, int(q * len(v)))] if v else None


# ------------------------------------------------------------ a small websocket client
async def ws_open(url):
    u = urlparse(url)
    secure = u.scheme == "wss"
    port = u.port or (443 if secure else 80)
    ctx = ssl.create_default_context() if secure else None
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection(u.hostname, port, ssl=ctx,
                                server_hostname=u.hostname if secure else None), 10)
    key = base64.b64encode(os.urandom(16)).decode()
    host = u.hostname if port in (80, 443) else f"{u.hostname}:{port}"
    writer.write((f"GET {u.path or '/'}{'?' + u.query if u.query else ''} HTTP/1.1\r\n"
                  f"Host: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                  f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                  f"User-Agent: feed-race\r\n\r\n").encode())
    await writer.drain()
    status = await asyncio.wait_for(reader.readline(), 10)
    if b" 101 " not in status:
        raise ConnectionError(status.decode(errors="replace").strip() or "no answer")
    while (await reader.readline()) not in (b"\r\n", b""):
        pass
    return reader, writer


def ws_frame(opcode, payload):
    mask = os.urandom(4)
    n = len(payload)
    head = bytes([0x80 | opcode])
    head += bytes([0x80 | n]) if n < 126 else (bytes([0x80 | 126]) + struct.pack("!H", n)
                                               if n < 65536 else bytes([0x80 | 127]) + struct.pack("!Q", n))
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload))


async def ws_messages(reader, writer):
    parts = []
    while True:
        h = await reader.readexactly(2)
        op, n = h[0] & 0x0F, h[1] & 0x7F
        if n == 126:
            n = struct.unpack("!H", await reader.readexactly(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", await reader.readexactly(8))[0]
        mask = await reader.readexactly(4) if h[1] & 0x80 else None
        data = await reader.readexactly(n)
        if mask:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        if op == 0x9:  # ping: answer it, or the venue closes the link
            writer.write(ws_frame(0xA, data))
            await writer.drain()
        elif op == 0x8:
            raise ConnectionError("closed by the server")
        elif op in (0x0, 0x1, 0x2):
            parts.append(data)
            if h[0] & 0x80:
                yield now_ms(), b"".join(parts)
                parts = []


async def follow(name, race, stats, stop):
    url, sub = FEEDS[name]
    while not stop.is_set():
        trig = Trigger()
        try:
            reader, writer = await ws_open(url)
            if sub:
                writer.write(ws_frame(0x1, json.dumps(sub).encode()))
                await writer.drain()
            stats.errors.pop(name, None)
            async for t, raw in ws_messages(reader, writer):
                if stop.is_set():
                    break
                got = parse(name, raw)
                if not got:
                    continue
                stats.msgs[name] += 1
                mid, ts = got
                if ts:
                    stats.age[name].append(time.time() * 1000.0 - ts)
                way = trig.update(t, mid)
                if way:
                    race.signal(name, way, t)
            writer.close()
        except Exception as e:  # any failure: say why, reconnect
            stats.errors[name] = f"{type(e).__name__}: {e}"[:120]
            stats.reconnects[name] += 1
            try:
                await asyncio.wait_for(stop.wait(), 3)
            except asyncio.TimeoutError:
                pass


# ------------------------------------------------------------ report
def report(race, stats, started):
    race.close(now_ms() + 2 * SAME_MOVE_MS)
    mins = (now_ms() - started) / 60000.0
    moves = race.done
    out = [f"FEED RACE  {mins:.0f} min on this server  (trigger {K_BP:g} bp in {WINDOW_MS:g} ms)"]
    for n in NAMES:
        age = pct(stats.age[n], 0.5)
        line = (f"  {n:16s} {stats.msgs[n] / max(mins * 60, 1):6.1f} msg/s  {race.moves[n]:5d} moves"
                + (f"  arrives {age:.0f} ms after the exchange (median)" if age is not None else ""))
        if stats.reconnects[n]:
            line += f"  reconnects {stats.reconnects[n]}"
        if n in stats.errors and not stats.msgs[n]:
            line += f"  NO DATA: {stats.errors[n]}"
        out.append(line)
    multi = [m for m in moves if len(m["at"]) >= 2]
    out.append(f"moves seen on 2+ feeds: {len(multi)} of {len(moves)}")
    if multi:
        first = Counter(min(m["at"], key=m["at"].get) for m in multi)
        out.append("  fired first: " + "   ".join(f"{n} {first[n] * 100 / len(multi):.0f}%" for n in NAMES))
    for a, b in (("coinbase", "binance futures"), ("coinbase", "binance spot"),
                 ("binance spot", "binance futures")):
        lead = [m["at"][a] - m["at"][b] for m in moves if a in m["at"] and b in m["at"]]
        if not lead:  # > 0: b fired first
            continue
        med = pct(lead, 0.5)
        out.append(f"{a} vs {b}, {len(lead)} moves on both:")
        out.append(f"  {b} first {sum(x > 0 for x in lead) * 100 / len(lead):.0f}%, "
                   f"{a} first {sum(x < 0 for x in lead) * 100 / len(lead):.0f}%")
        out.append(f"  {b if med >= 0 else a} ahead by {abs(med):.0f} ms (median; middle half "
                   f"{pct(lead, 0.25):+.0f} to {pct(lead, 0.75):+.0f} ms, + = {b} first)")
    mine = [m for m in moves if "coinbase" in m["at"] and len(m["at"]) >= 2]
    if mine:  # the bot triggers on coinbase: how often, and how far, was it beaten
        beaten = [m["at"]["coinbase"] - min(t for n, t in m["at"].items() if n != "coinbase")
                  for m in mine]
        late = [x for x in beaten if x > 0]
        out.append(f"YOUR BOT (coinbase): another feed fired first in {len(late) * 100 / len(mine):.0f}% "
                   f"of the {len(mine)} moves coinbase also made"
                   + (f", by {pct(late, 0.5):.0f} ms (median)" if late else ""))
    alone = Counter(next(iter(m["at"])) for m in moves if len(m["at"]) == 1)
    if alone:
        out.append("  moved on one feed only: " + "   ".join(f"{n} {alone[n]}" for n in NAMES))
    return "\n".join(out)


async def main():
    minutes = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    try:
        os.nice(19)
    except OSError:
        pass
    race, stats, stop = Race(), Stats(), asyncio.Event()
    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop.set)
    started = now_ms()
    print(f"listening to {', '.join(NAMES)} for {minutes:g} min (Ctrl+C to stop early) ...", flush=True)
    tasks = [asyncio.create_task(follow(n, race, stats, stop)) for n in NAMES]
    deadline = started + minutes * 60000.0
    next_note = started + 300000.0
    while not stop.is_set() and now_ms() < deadline:
        try:
            await asyncio.wait_for(stop.wait(), 1)
        except asyncio.TimeoutError:
            pass
        if now_ms() >= next_note:
            next_note += 300000.0
            print(f"{(now_ms() - started) / 60000:.0f} min: moves "
                  + ", ".join(f"{n} {race.moves[n]}" for n in NAMES), flush=True)
    stop.set()
    for t in tasks:
        t.cancel()
    await asyncio.gather(*tasks, return_exceptions=True)
    print(report(race, stats, started), flush=True)


if __name__ == "__main__":
    asyncio.run(main())
