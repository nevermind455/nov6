#!/usr/bin/env python3
"""Compare the Python reference's and the C++ bot's journal rows, row by row.

Every field either side wrote is compared exactly (JSON value equality:
17 == 17.0), except per-process diagnostics that measure this machine's
own time (signing, thread hand-off, stage timings) and fields only one
side computes as described in docs/SPEC_COMPLIANCE.md. A decision row
matches when result, reason, side, cap, fair limit and shares all agree
(the PDF's parity criterion); field-level differences are listed too.

Strict field parity is required by default. --decision-only selects only the
decision criterion and still refuses journals containing no decisions.
Usage: compare.py PY.jsonl CPP.jsonl [--show N] [--decision-only]
"""
import json
import sys
from collections import Counter

# measured on the host, not decided: never comparable across processes
DIAGNOSTIC = {
    "sign_ms", "sign_cpu_ms", "sign_gc", "t_loop_ms", "t_decide_ms", "t_sign_ms", "t_request_ms",
    "t_wire_ms", "t_inflight_handoff_ms", "t_checks_ms", "t_total_ms", "ts_us", "lat", "lat_ack_source",
    "lat_fill_source", "t_signal_to_send_ms", "fill_confirmed_ms", "t_send_to_fill_confirmed_ms",
    "decision_book", "book_trace", "late_recovered", "old_rule",
}
DECISION = ("result", "reason", "side", "cap", "fair_limit", "shares")
MISSING = object()


def load(path):
    with open(path, encoding="utf-8") as fh:
        return [json.loads(x) for x in fh if x.strip()]


def same(a, b):
    if isinstance(a, bool) or isinstance(b, bool):
        return a is b or a == b and type(a) is type(b)
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


def main():
    py, cpp = load(sys.argv[1]), load(sys.argv[2])
    show = int(sys.argv[sys.argv.index("--show") + 1]) if "--show" in sys.argv else 5
    def is_decision(row):
        return row.get("result") in ("filled", "refused", "skipped", "error", "pending")
    n = max(len(py), len(cpp))
    field_diff = Counter()
    decision_mismatch = 0
    shown = 0
    for i in range(n):
        a = py[i] if i < len(py) else {}
        b = cpp[i] if i < len(cpp) else {}
        keys = (set(a) | set(b)) - DIAGNOSTIC
        bad = sorted(k for k in keys if not same(a.get(k, MISSING), b.get(k, MISSING)))
        for k in bad:
            field_diff[k] += 1
        if is_decision(a) or is_decision(b):
            if any(not same(a.get(k), b.get(k)) for k in DECISION):
                decision_mismatch += 1
        if bad and shown < show:
            shown += 1
            print(f"row {i}: {a.get('result')}/{a.get('reason')} vs {b.get('result')}/{b.get('reason')}")
            for k in bad[:12]:
                print(f"   {k}: py={a.get(k, '<absent>')!r} cpp={b.get(k, '<absent>')!r}")
    total = sum(is_decision(py[i] if i < len(py) else {}) or
                is_decision(cpp[i] if i < len(cpp) else {}) for i in range(n))
    print(f"rows: python {len(py)}, c++ {len(cpp)}; decision rows {total}")
    print(f"decision mismatches (result/reason/side/cap/fair_limit/shares): {decision_mismatch}")
    rate = 100.0 * (total - decision_mismatch) / total if total else 0.0
    print(f"decision parity: {rate:.3f}%")
    if field_diff:
        print("fields that differ (rows):", dict(field_diff.most_common()))
    else:
        print("every compared field identical in every row")
    # Successful strict parity requires actual decisions and every comparable
    # field. A successful empty replay is not evidence of decision parity.
    strict = "--decision-only" not in sys.argv
    sys.exit(0 if total > 0 and decision_mismatch == 0 and len(py) == len(cpp)
             and (not strict or not field_diff) else 1)


if __name__ == "__main__":
    main()
