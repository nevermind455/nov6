#!/usr/bin/env python3
"""A short health report of a live run: halts, errors, orders, fills, resting
orders, websockets, settlement and the ledger. Read-only: it opens the journal
and the ledger snapshot, sends nothing, needs no key, and prints no secret
(order and trade ids are shortened).

    python3 -I tools/report/live_check.py                 # state/live, last 24 h
    python3 -I tools/report/live_check.py --hours 6
    python3 -I tools/report/live_check.py --state state/paper
"""
import argparse
import collections
import json
import math
import os
import sys
import time

U = 1_000_000


def hm(wall):
    return time.strftime("%m-%d %H:%M", time.gmtime(wall)) if wall else "--"


def dur(seconds):
    s = int(max(0, seconds))
    if s >= 3600:
        return f"{s // 3600}h{s % 3600 // 60:02d}m"
    if s >= 60:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s}s"


def short(text, n=90):
    text = str(text or "")
    return text if len(text) <= n else text[: n - 1] + "~"


def pct(values, q):
    if not values:
        return None
    v = sorted(values)
    return v[min(len(v) - 1, max(0, int(math.ceil(q / 100 * len(v))) - 1))]


def money(units):
    return f"${units / U:+.2f}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--state", default="state/live", help="the bot's state folder")
    ap.add_argument("--hours", type=float, default=24.0, help="how far back (default 24)")
    a = ap.parse_args()
    try:
        os.nice(19)  # never compete with a bot running on the same machine
    except OSError:
        pass
    journal = os.path.join(a.state, "sniper.jsonl")
    ledger_path = os.path.join(a.state, "ledger.json")
    if not os.path.exists(journal):
        sys.exit(f"no journal at {journal}: run this from the bot's folder (cd /root/sniper)")

    # The window ends at the journal's last row: the report reads the same
    # whenever it is run.
    last_wall = 0.0
    with open(journal, "rb") as f:
        f.seek(max(0, os.path.getsize(journal) - 65536))
        for line in f.read().splitlines():
            try:
                last_wall = max(last_wall, float(json.loads(line).get("wall") or 0))
            except Exception:
                pass
    since = last_wall - a.hours * 3600

    bad = rows = 0
    runs = []          # [start, end, final row or None, exit code]
    run_counters = []  # per run: (counters before the window, last counters)
    guards = []        # (wall, reason, until)
    held = collections.Counter()
    held_periods = []  # [detail, first, last, rows]
    refused_why = collections.Counter()
    errors = collections.OrderedDict()
    order_side, order_cond, order_wall = {}, {}, {}
    fak_sent = fak_miss = fak_unknown = 0
    venue_refusals = collections.Counter()
    reply_ms, net_ms = [], []
    fills = collections.Counter()
    fill_sh = collections.Counter()
    fill_cost = collections.Counter()
    fill_fee = collections.Counter()
    rest_out = collections.Counter()
    rest_skip = collections.Counter()
    rest_done = collections.Counter()
    rest_cancel_why = collections.Counter()
    rest_max_try = 0
    rest_off = []
    rest_on_rows = []
    ws_down = collections.Counter()
    ws_reason = collections.Counter()
    ws_up = collections.Counter()
    feed_outage_left = []
    coinbase_reconnects = 0
    discovery = collections.Counter()
    ptb_rows = []
    settlements = []
    drawdown = []
    manual = 0
    not_found = 0
    first_wall = None

    def note_error(reason, wall, row):
        key = short(reason, 100)
        e = errors.get(key)
        if e is None:
            e = errors[key] = [0, wall, wall, ""]
        e[0] += 1
        e[2] = wall
        if not e[3] and row.get("trade_id"):
            e[3] = "trade " + short(row.get("trade_id"), 24)

    with open(journal, "rb") as f:
        for line in f:
            # Most rows are routine skips whose totals the summaries already
            # carry: only the ones that say why orders were held are read.
            if b'"result":"skipped"' in line and b'"reason":"live ' not in line:
                continue
            try:
                r = json.loads(line.decode("utf-8", "replace"))
            except Exception:
                bad += 1
                continue
            if not isinstance(r, dict):
                bad += 1
                continue
            wall = float(r.get("wall") or 0)
            res = r.get("result")
            # runs and their cumulative counters, also before the window
            if res == "start" and r.get("native_cpp"):
                runs.append([wall, None, None, None])
                run_counters.append([{}, {}])
            if res == "summary" and run_counters:
                c = r.get("counters") or {}
                if wall < since:
                    run_counters[-1][0] = c
                run_counters[-1][1] = c
                if runs:
                    runs[-1][1] = wall
                if r.get("final") and runs:
                    runs[-1][2] = r
                    runs[-1][3] = r.get("exit_code")
            if wall < since:
                continue
            rows += 1
            first_wall = first_wall or wall
            if runs and (runs[-1][1] is None or wall > runs[-1][1]):
                runs[-1][1] = wall
            if res == "guard":
                guards.append((wall, r.get("reason", ""), float(r.get("pause_until") or wall)))
            elif res == "skipped":
                why = r.get("reason", "")
                if why in ("live halted", "live refused"):
                    detail = r.get("detail", "")
                    (held if why == "live halted" else refused_why)[short(detail, 80)] += 1
                    if why == "live halted":
                        p = held_periods[-1] if held_periods else None
                        if p and p[0] == detail and wall - p[2] < 120:
                            p[2] = wall
                            p[3] += 1
                        else:
                            held_periods.append([detail, wall, wall, 1])
            elif res == "error":
                note_error(r.get("reason", ""), wall, r)
            elif res == "pending" and r.get("order_id") and not r.get("acknowledged"):
                if r.get("never_retry"):
                    fak_unknown += 1
                    note_error("unknown order outcome: " + str(r.get("reason", "")), wall, r)
                elif "side" in r:
                    fak_sent += 1
                    oid = r["order_id"]
                    order_side[oid] = r.get("side")
                    order_cond[oid] = r.get("condition_id")
                    order_wall[oid] = wall
            elif res == "refused" and r.get("order_id") and r.get("acknowledged"):
                if r.get("reason") == "FAK no match":
                    fak_miss += 1
                else:
                    venue_refusals[short(r.get("venue_reply") or r.get("reason"), 90)] += 1
                if r.get("T5_ns") and r.get("T6_ns"):
                    net_ms.append((r["T6_ns"] - r["T5_ns"]) / 1e6)
            elif res == "live_fill_update" and r.get("acknowledged"):
                if r.get("reply_ms") is not None:
                    reply_ms.append(float(r["reply_ms"]))
                if r.get("T5_ns") and r.get("T6_ns"):
                    net_ms.append((r["T6_ns"] - r["T5_ns"]) / 1e6)
            elif res == "live_fill":
                kind = "resting" if r.get("rest") else "FAK"
                fills[kind] += 1
                fill_sh[kind] += round(float(r.get("shares") or 0) * U)
                fill_cost[kind] += round(float(r.get("amount") or 0) * U)
                fill_fee[kind] += round(float(r.get("fee") or 0) * U)
            elif res == "rest_order":
                out = r.get("outcome", "?")
                rest_out[out] += 1
                if out not in ("resting", "matched", "accepted"):
                    rest_skip[short(r.get("reason"), 70)] += 1
            elif res == "rest_done":
                rest_done["filled" if r.get("filled") else "no fill"] += 1
                rest_cancel_why[short(r.get("cancel_why") or "filled in full", 50)] += 1
            elif res == "rest_cancel":
                rest_max_try = max(rest_max_try, int(r.get("try") or 0))
            elif res == "rest_live_off":
                rest_off.append((wall, r.get("reason", "")))
            elif res == "rest_live":
                rest_on_rows.append((wall, r.get("on"), r.get("reason", "")))
            elif res == "ws":
                key = f'{r.get("socket")} {r.get("role", "")}'
                if r.get("up"):
                    ws_up[r.get("socket")] += 1
                else:
                    ws_down[key] += 1
                    ws_reason[f'{r.get("socket")}: {short(r.get("reason"), 60)}'] += 1
                    if r.get("socket") == "market" and "no standby" in str(r.get("role")):
                        feed_outage_left.append(r.get("left_s"))
            elif res == "coinbase_reconnect":
                coinbase_reconnects += 1
            elif res == "discovery_refused":
                discovery[short(r.get("reason"), 70)] += 1
            elif res == "ptb_official":
                ptb_rows.append(r)
            elif res == "settlement":
                settlements.append(r)
            elif res in ("hybrid_drawdown_pause", "hybrid_drawdown_reset"):
                drawdown.append((wall, res))
            elif res == "manual_trade":
                manual += 1
            elif res == "order_not_found":
                not_found += 1

    now = time.time()
    out = []
    p = out.append
    p(f"LIVE CHECK  {a.state}  window {hm(since)} .. {hm(last_wall)} UTC ({a.hours:g} h)")
    age = now - last_wall
    p(f"journal: {rows} rows in the window, {bad} unreadable; last row {dur(age)} ago"
      + ("  -> the bot looks STOPPED" if age > 180 else "  (bot running)"))

    # ---- runs
    p("")
    p("RUNS")
    shown = [r for r in runs if (r[1] or r[0]) >= since]
    for start, end, final, code in shown[-8:]:
        state = (f"stopped, exit {code}" if final is not None else
                 "running" if end and now - end < 180 else "ended without a final summary (killed or crashed)")
        p(f"  {hm(start)} -> {hm(end)}  {dur((end or start) - start):>7}  {state}")
    if not shown:
        p("  no start in the window")

    # ---- counters in the window (summed over runs)
    total = collections.Counter()
    for before, last in run_counters:
        for k, v in last.items():
            if isinstance(v, (int, float)):
                total[k] += v - (before.get(k, 0) if isinstance(before.get(k, 0), (int, float)) else 0)
    p("")
    p("SIGNALS AND SKIPS (exact counters)")
    p(f"  signals {int(total.get('signals', 0))}   orders decided {int(total.get('orders', 0))}")
    skips = [(k[6:], v) for k, v in total.items() if k.startswith("skip: ") and v > 0]
    for k, v in sorted(skips, key=lambda x: -x[1])[:10]:
        p(f"  {int(v):>7}  {short(k, 70)}")

    # ---- halts
    p("")
    p("HALTS (new orders held)")
    if guards:
        p("  fault pauses (5 min each unless repeated):")
        by = collections.OrderedDict()
        for wall, why, until in guards:
            e = by.setdefault(short(why, 70), [0, wall, wall, 0.0])
            e[0] += 1
            e[2] = wall
            e[3] += max(0.0, until - wall)
        for why, (n, first, last, secs) in by.items():
            p(f"    {n:>3}x  {why}  ({hm(first)} .. {hm(last)}, up to {dur(secs)} paused)")
    if held_periods:
        p("  held stretches ('live halted', from rate-limited rows):")
        merged = collections.OrderedDict()
        for detail, first, last, n in held_periods:
            e = merged.setdefault(short(detail, 70), [0, 0.0, first, last])
            e[0] += 1
            e[1] += max(1.0, last - first)
            e[3] = last
        for why, (n, secs, first, last) in sorted(merged.items(), key=lambda x: -x[1][1])[:8]:
            p(f"    {dur(secs):>7} in {n:>3} stretches  {why}  (last {hm(last)})")
    if drawdown:
        p(f"  drawdown stop: {sum(1 for _, k in drawdown if k == 'hybrid_drawdown_pause')} pauses, last {hm(drawdown[-1][0])}")
    if refused_why:
        p("  refused before sending ('live refused'):")
        for why, n in refused_why.most_common(6):
            p(f"    {n:>5}  {why}")
    if not (guards or held_periods or drawdown or refused_why):
        p("  none")

    # ---- errors
    p("")
    p("ERRORS")
    if errors:
        for why, (n, first, last, extra) in sorted(errors.items(), key=lambda x: -x[1][0])[:10]:
            p(f"  {n:>5}x  {why}  ({hm(first)} .. {hm(last)}){'  ' + extra if extra else ''}")
    else:
        p("  none")
    if manual:
        p(f"  {manual} trades listed in manual_trades.txt were skipped")
    if not_found:
        p(f"  {not_found} orders the venue had no record of a minute after sending (taken as no fill)")

    # ---- orders and fills
    p("")
    p("ORDERS AND FILLS")
    answered = fak_miss + sum(venue_refusals.values())
    p(f"  FAK sent {fak_sent}   missed {fak_miss}   venue refused {sum(venue_refusals.values())}   unknown outcome {fak_unknown}")
    for k in ("FAK", "resting"):
        if fills[k]:
            p(f"  {k:<7} fills {fills[k]:>4}  {fill_sh[k] / U:>8.1f} sh  cost {fill_cost[k] / U:>8.2f}  fees {fill_fee[k] / U:.2f}")
    if fak_sent:
        p(f"  FAK fill rate {fills['FAK'] / fak_sent * 100:.1f}% of orders sent")
    for why, n in venue_refusals.most_common(4):
        p(f"    refused {n}x: {why}")
    if net_ms:
        p(f"  order round trip ms  p50 {pct(net_ms, 50):.0f}  p95 {pct(net_ms, 95):.0f}  max {max(net_ms):.0f}  ({len(net_ms)} replies)")

    # ---- resting
    if rest_out or rest_on_rows or rest_off:
        p("")
        p("RESTING ORDERS")
        if rest_on_rows:
            w, on, why = rest_on_rows[-1]
            p(f"  at last start: {'ON' if on else 'OFF'}  {short(why, 80)}")
        p("  after a miss: " + ", ".join(f"{k} {v}" for k, v in rest_out.most_common()))
        for why, n in rest_skip.most_common(5):
            p(f"    {n:>4}  not rested: {why}")
        if rest_done:
            p("  ended: " + ", ".join(f"{k} {v}" for k, v in rest_done.items())
              + "   cancel reasons: " + ", ".join(f"{k} {v}" for k, v in rest_cancel_why.most_common(4)))
        if rest_max_try > 3:
            p(f"  !! a cancel needed {rest_max_try} tries")
        for w, why in rest_off:
            p(f"  !! test switched OFF at {hm(w)}: {why}")

    # ---- sockets
    p("")
    p("WEBSOCKETS")
    if ws_down or ws_up:
        p("  connected: " + ", ".join(f"{k} {v}x" for k, v in ws_up.items()))
        p("  dropped:   " + (", ".join(f"{k} {v}x" for k, v in ws_down.most_common()) or "none"))
        for why, n in ws_reason.most_common(5):
            p(f"    {n:>4}x  {why}")
        if feed_outage_left:
            near_end = sum(1 for x in feed_outage_left if isinstance(x, (int, float)) and x <= 15)
            p(f"  !! market feed fully down {len(feed_outage_left)}x ({near_end} in a round's last 15 s)")
    else:
        p("  no socket rows (an older build, or nothing happened)")
    if coinbase_reconnects:
        p(f"  coinbase reconnects asked by the bot: {coinbase_reconnects}")
    for why, n in discovery.most_common(3):
        p(f"  discovery refused {n}x: {why}")

    # ---- price to beat
    p("")
    checks = ptb_rows[-1].get("checks") if ptb_rows else None
    errs = [r.get("err_bp") for r in ptb_rows if isinstance(r.get("err_bp"), (int, float))]
    p(f"PRICE TO BEAT: {len(ptb_rows)} published values read, checks now {checks}"
      + (f", estimate error p50 {pct(errs, 50):.2f} bp max {max(errs):.2f} bp" if errs else ""))

    # ---- ledger
    p("")
    p("LEDGER")
    try:
        led = json.load(open(ledger_path))
    except Exception as e:
        p(f"  ledger unreadable: {e}")
        led = None
    if led:
        orders = led.get("orders", [])
        by_id = {o["id"]: o for o in orders}
        pay = {}
        for s in settlements:
            pay[s.get("condition_id")] = s.get("payouts") or {}
        pnl = collections.Counter()
        wins = losses = 0
        for cond, payouts in pay.items():
            for kind in ("FAK", "resting"):
                v = 0
                any_fill = False
                for o in orders:
                    if o.get("condition") != cond or not o.get("booked_shares"):
                        continue
                    if bool(o.get("rest")) != (kind == "resting"):
                        continue
                    any_fill = True
                    v += o["booked_shares"] * int(payouts.get(o["token"], 0)) // U - o["booked_cost"] - o["booked_fee"]
                if any_fill:
                    pnl[kind] += v
            total_v = sum(o["booked_shares"] * int(payouts.get(o["token"], 0)) // U - o["booked_cost"] - o["booked_fee"]
                          for o in orders if o.get("condition") == cond and o.get("booked_shares"))
            if any(o.get("condition") == cond and o.get("booked_shares") for o in orders):
                wins += total_v > 0
                losses += total_v < 0
        p(f"  settled in the window: {len(pay)} rounds, {wins} won / {losses} lost, "
          f"P&L FAK {money(pnl['FAK'])}  resting {money(pnl['resting'])}  total {money(pnl['FAK'] + pnl['resting'])}")
        p(f"  all time: realized {money(led.get('realized', 0))}  of it resting {money(led.get('rest_realized', 0))}  fees ${led.get('fees', 0) / U:.2f}")
        open_pos = [x for x in led.get("positions", []) if not x.get("settled") and x.get("shares")]
        if open_pos:
            p(f"  open positions: {len(open_pos)}  {sum(x['shares'] for x in open_pos) / U:.1f} sh  cost {sum(x['cost'] + x['fees'] for x in open_pos) / U:.2f}")
        unresolved = [o for o in orders if o.get("status") in ("intent", "sent", "unknown", "pending", "matched")]
        for o in unresolved[:6]:
            p(f"  !! order {o['id'][:10]} {o.get('side')} {'resting ' if o.get('rest') else ''}status {o['status']} for {dur(now - o.get('sent_wall', now))}")
        settled = set(led.get("settled_conditions", []))
        late = {o["condition"] for o in orders
                if o.get("booked_shares") and o["condition"] not in settled and o.get("end", 0) < now - 1800}
        if late:
            p(f"  !! {len(late)} rounds with shares still unsettled 30 min after they ended")
        c = led.get("canary", {})
        paused = c.get("pause_until", 0) > now
        p(f"  canary session since {hm(c.get('started', 0))}: orders {c.get('orders')}  fills {c.get('fills')}  "
          f"spend ${c.get('spend', 0) / U:.2f}  rounds {len(c.get('rounds', []))}  settled loss ${c.get('settled_loss', 0) / U:.2f}"
          + (f"  PAUSED {dur(c['pause_until'] - now)} more: {c.get('pause_reason')}" if paused else ""))
    test = os.path.join(a.state, "rest_live_test.json")
    if os.path.exists(test):
        try:
            t = json.load(open(test))
            p(f"  resting test: {'OFF (' + str(t.get('reason')) + ')' if t.get('off') else 'on'}, result since it began {money(t.get('realized', 0))}")
        except Exception:
            p("  resting test record unreadable")
    stop = os.path.join(os.path.dirname(os.path.abspath(a.state.rstrip('/'))), "..", "STOP_LIVE")
    if os.path.exists("STOP_LIVE") or os.path.exists(os.path.normpath(stop)):
        p("  !! STOP_LIVE exists: new orders are blocked")
    print("\n".join(out))


if __name__ == "__main__":
    main()
