# VPS paper validation (Ubuntu 24.04)

Paper mode needs no wallet or API credentials and never submits orders: the
summary line reports `paper_order_calls` and `dry_run_orders`, and both must
stay 0. Do not add `--live`; live mode also refuses to start without
`SNIPER_LIVE_CANARY=1` and the live preflight.

## Run

```bash
bash scripts/setup.sh            # once: creates .env from .env.example
bash scripts/build.sh            # optional: rebuild bin/ and run the tests
mkdir -p ~/sniper-validation
nohup bash scripts/run-paper.sh --run-for 86400 \
  --record ~/sniper-validation/feed.jsonl > ~/sniper-validation/bot.log 2>&1 &
echo $! > ~/sniper-validation/pid
# memory and CPU, one sample a minute
while kill -0 "$(cat ~/sniper-validation/pid)" 2>/dev/null; do
  printf '%s %s\n' "$(date -u +%FT%TZ)" "$(ps -o rss=,pcpu= -p "$(cat ~/sniper-validation/pid)")"
  sleep 60
done >> ~/sniper-validation/resources.txt &
```

Stop early with `kill -INT "$(cat ~/sniper-validation/pid)"`; the bot writes
its final summary and ledger snapshot before exiting.

## Where each item is logged

| Item | Where |
| --- | --- |
| Round start/end, current and next market IDs and tokens | `feed.jsonl` `k=round` (`current` true/false, `cond`, `up`, `down`, `start`, `end`); journal rows carry `condition_id` and `round_end` |
| Rule refreshes | `feed.jsonl` `k=rules` |
| Market WebSocket connect/disconnect | `feed.jsonl` `k=mconn`; dashboard events |
| Book gaps and resubscription | `feed.jsonl` `k=ws_gap` |
| Book freshness at each decision | journal `dec_quote_age_ms`, `dec_venue_age_ms` |
| Price-to-beat value and source | journal `ptb`, `ptb_why` |
| Signal acceptance/rejection reason | journal `result` (`skipped`, `refused`, `filled`) and `reason` |
| Intended limit and shares | journal `cap`, `fair_limit`, `shares`, `target_shares`, `order_shares` |
| Simulated order events | `state/paper/paper_audit.jsonl` |
| Memory and CPU | `resources.txt` (above); summary `peak_rss_kib` |

Round-switch duration is not logged as its own field. Measure it as the time
from each `round_end - 300` to the first journal row for that round.

## Acceptance checks after the run

1. The process ran the full 86,400 s and exited 0; `errors` is 0.
2. `paper_order_calls` and `dry_run_orders` are 0.
3. Every journal order row's `wall` lies inside `[round_end - 300, round_end)`,
   and every `condition_id` maps to exactly one `round_end`.
4. A `k=round` with `current=false` appears for each round about 55-60 s
   before its start (default `SNIPER_PREPARE_LEAD_S=60`; discovery runs every 5 s).
5. `resources.txt` RSS is flat after the first hour (no steady growth).
6. Every `k=mconn` `up=false` is followed by `up=true`, and the decisions
   after it carry fresh `dec_quote_age_ms`.
7. `bin/sniper-miss-report state/paper/sniper.jsonl --record
   ~/sniper-validation/feed.jsonl` lists no `unclassified_reasons` and no
   `software_failure` codes; `data_not_ready` misses outside the first minute
   after start are investigated (see `docs/MISSED_ORDERS.md`).
8. File descriptors stay flat: add `$(ls /proc/$PID/fd | wc -l)` to the
   resource sampler. In the simulated runs it stayed at 17 through 13 forced
   disconnects.

## Reference: 11-minute simulated run (7 November 2026)

Run against the development fake venue (`tools/fake-venue`), synthetic data,
three rounds, two boundaries: exit 0, 0 errors, 2,519 signals, 69 paper
orders (6 fills), 0 order calls, 0 orders outside their own round, one
`round_end` per condition. RSS was 50.0 MB one minute in and 50.6 MB at the
end; average CPU 3.4% on 4 vCPUs. After each boundary the first decision came
0.6-0.8 s into the new round. 38 of 63 refusals were the strict paper
arrival-book policy (`unknown book state at the modelled arrival`), which is
expected with the fake venue's one-second requotes. This does not replace
the 24-hour VPS run against the real venue.
