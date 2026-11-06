#!/usr/bin/env bash
# Decision parity: the same recordings through the Python reference
# (tools/replay/py_replay.py on nevermind455/oct10@772f2eb) and the C++
# decision code (sniper-replay), under several settings, row by row.
# Usage: run_parity.sh REPO PYTHON REPLAY_BIN OUT_DIR [SEEDS...]
set -euo pipefail
repo=$1 py=$2 bin=$3 out=$4
shift 4
seeds=${*:-"7 11 23 42 99"}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
configs=(
  'default|{}'
  'drawdown|{}'
  'dollars|{"SNIPER_ORDER_SIZING":"dollars"}'
  'fok|{"SNIPER_ORDER_TYPE":"FOK"}'
  'min-entry|{"SNIPER_KELLY_MIN_ENTRY":"1","SNIPER_MIN_ENTRY_MAX_DRAWDOWN":"0.15"}'
  'no-limit|{"SNIPER_FAIR_NO_LIMIT":"1"}'
  'gap|{"SNIPER_MIN_GAP_C":"2"}'
  'risk-cap|{"SNIPER_HYBRID_RISK_FRACTION":"0.004","SNIPER_HYBRID_MAX_ORDER_USD":"6"}'
  'fixed-latency|{"SNIPER_LATENCY_MS":"5"}'
  'venue-min|{"SNIPER_VENUE_MIN_SHARES":"3"}'
  'flat|{"SNIPER_KELLY":"0","SNIPER_SHARES":"12"}'
  'no-hybrid|{"SNIPER_HYBRID_SIZING":"0","SNIPER_KELLY_FRACTION":"0.5"}'
  'caps|{"SNIPER_MAX_FILLS_ROUND":"3","SNIPER_MAX_SIDE_SHARES":"30","SNIPER_COOLDOWN_MS":"0"}'
  'one-socket|{"SNIPER_COINBASE_CONNS":"1","SNIPER_BACKLOG_GATE":"age"}'
  'tight|{"SNIPER_STALE_MS":"600","SNIPER_PAPER_EXTRA_MS":"0","SNIPER_HOLD_MS":"150","SNIPER_SLIP":"0.02"}'
  'quiet|{"SNIPER_BOOK_QUIET":"1","SNIPER_PTB_SKIP_CLOSE":"1","SNIPER_PTB_MIN_CHECKS":"1"}'
)
total=0 bad=0 rows=0
printf '%-14s %-6s %8s %8s %10s\n' config seed rows decisions parity
for seed in $seeds; do
  for entry in "${configs[@]}"; do
    name=${entry%%|*} cfg=${entry#*|}
    rec="$out/rec-$seed${name/drawdown/-dd}.jsonl"
    flags=""
    [[ $name == drawdown ]] && flags="--drawdown"
    [[ -f $rec ]] || python3 "$here/make_recording.py" "$rec" --seed "$seed" --rounds 4 $flags >/dev/null
    echo "$cfg" > "$out/cfg-$name.json"
    "$py" -I "$here/py_replay.py" "$repo" "$rec" "$out/py-$seed-$name.jsonl" "$out/cfg-$name.json" >/dev/null
    "$bin" "$rec" "$out/cpp-$seed-$name.jsonl" "$out/cfg-$name.json"
    if python3 "$here/compare.py" "$out/py-$seed-$name.jsonl" "$out/cpp-$seed-$name.jsonl" --show 3 \
        > "$out/cmp-$seed-$name.txt"; then status=ok; else status=DIFF; bad=$((bad + 1)); fi
    n=$(grep -c . "$out/py-$seed-$name.jsonl" || true)
    d=$(sed -n 's/.*decision rows \([0-9]*\).*/\1/p' "$out/cmp-$seed-$name.txt")
    p=$(sed -n 's/decision parity: //p' "$out/cmp-$seed-$name.txt")
    total=$((total + 1)) rows=$((rows + n))
    printf '%-14s %-6s %8s %8s %10s %s\n' "$name" "$seed" "$n" "$d" "$p" "$status"
  done
done
echo "runs: $total, journal rows compared: $rows, runs with any difference: $bad"
[[ $bad -eq 0 ]]
