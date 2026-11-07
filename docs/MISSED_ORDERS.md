# Missed-order diagnostics

`bin/sniper-miss-report` explains every signal the bot saw: why it was
skipped, why an order was refused, and what happened to each order it sent.
It reads the journal only. It sends nothing, needs no keys and never touches
the trading path.

```bash
bin/sniper-miss-report state/paper/sniper.jsonl                 # paper
bin/sniper-miss-report state/live/sniper.jsonl                  # live
bin/sniper-miss-report state/paper/sniper.jsonl --record feed.jsonl
```

`--record` adds the market and Coinbase websocket disconnects from a
`--record` recording, for the suspected-cause check below. The output is
JSON. Exit status 1 means the journal had malformed lines or could not be
read.

## What the bot already records

Every trigger that becomes a signal is journalled with its result and reason:

| `result` | Meaning |
| --- | --- |
| `skipped` | an entry filter, risk check or data gate stopped it; `reason` says which (`detail` for `live refused` / `live halted`) |
| `refused` | paper: the broker refused the order (`reason`, `outcome`); live: the venue replied with no match or a refusal (`http_status`, `venue_reply`) |
| `filled` | paper fill (`shares`, `order_shares`, `cost`, `fee`) |
| `pending` | live: the dispatch row (order sent), a "success, awaiting fill size" reply, or an unknown outcome (`never_retry`) |
| `live_fill_update` / `live_fill` | live: matched size from the reply, then confirmed private-channel trades |

Decision rows carry the BTC move (`bp`, `signal_mid`), book state at the
decision (`dec_ask`, `dec_bid`, `dec_quote_age_ms`, `dec_venue_age_ms`,
`dec_depth_sh`), fair value (`fair`, `fair_limit`, `cap`), sizing
(`order_shares`, `target_shares`, `kelly_shares`), the round (`condition_id`,
`round_end`, `left_s`) and price-to-beat (`ptb`, `ptb_why`). Live reply rows
carry monotonic stamps: `T0_ns` trigger receipt, `T2_ns` dispatch start,
`T3_ns` signed, `T4_ns` durable intent, `T5_ns` request written, `T6_ns`
reply received. Credentials never appear in the journal; reply text is
truncated to 300 characters.

Skip rows are rate-limited to one per second per reason, round and side. The
exact totals are the `skip: <reason>` counters in each `summary` row, so the
report counts skips from the counters (summed over restarts) and uses rows
only as evidence.

## How each reason is classified

Every reason text maps to a stable code and one of these kinds. The test
`MissReport.EveryEngineReasonHasACode` reads the engine source and fails if a
new reason has no code.

| Kind | Verdict | Examples |
| --- | --- | --- |
| `strategy_filter` | correct filter, not a bug | `FILTER_ASK_NOT_BELOW_FAIR`, `FILTER_PRICE_BAND`, `FILTER_TOO_LATE`, `FILTER_COOLDOWN` |
| `risk_control` | correct risk control, not a bug | `RISK_KELLY_BELOW_MINIMUM`, `RISK_ORDER_IN_FLIGHT`, `RISK_ROUND_CAP`, `RISK_INSUFFICIENT_BALANCE`, `RISK_LIVE_HALTED` |
| `data_not_ready` | missed opportunity: data or state not ready | `DATA_VOL_WARMUP`, `DATA_BOOK_STALE`, `DATA_BOOK_NOT_LIVE`, `DATA_PRIVATE_CHANNEL`, `DATA_SENDER_NOT_READY` |
| `late_or_stale` | missed opportunity: aged before dispatch | `LATE_TRIGGER_STALE`, `LATE_BOOK_STALE`, `LATE_ROUND_CHANGED` |
| `price_moved` | unfilled: price moved past the limit | `PAPER_PRICE_CAP_MISS`, `PAPER_LIVE_ODDS_MISS` |
| `no_liquidity` | unfilled: nothing to match at the limit | `LIQ_FAK_NO_MATCH`, `LIQ_FAK_NO_FILL`, `PAPER_INSUFFICIENT_LIQUIDITY` |
| `venue_rejected` | rejected by the exchange | `VENUE_REFUSED` (with `http_status`, `venue_reply`) |
| `paper_model` | paper simulator could not decide (paper only) | `PAPER_ARRIVAL_UNKNOWN` |
| `software_failure` | software failure | `SW_HANDLER_FAULT`, `SW_AUDIT_FAILED`, `SW_SIGNING_FAILED` |
| `unknown_outcome` | unknown outcome: reconcile with the account | `UNKNOWN_ORDER_OUTCOME` |
| `unclassified` | inspect | anything the table does not know, listed verbatim |

Each code in the output has a count, its source (`counter` or `rows`), a
short recommended action, and up to three evidence rows (time, round, side,
book age, detail, venue reply).

For paper orders the broker's `outcome` code decides, because it separates a
price that moved (`PRICE_CAP_MISS`) from missing size
(`INSUFFICIENT_LIQUIDITY`); its catch-all `TECHNICAL_ERROR` defers to the
reason text.

## Orders and latency

Live orders are followed by `order_id` from the dispatch row: submitted,
acknowledged, then filled, partially filled (confirmed below
`order_shares`), matched but not yet confirmed, unfilled (FAK no match),
rejected, unknown outcome, or no reply yet. Paper orders are counted
separately and never as live orders.

`latency_ms` reports n, p50, p95, p99 and max for:

| Stage | From | Measured |
| --- | --- | --- |
| `trigger_to_dispatch` | T0 to T2 | locally |
| `signing` | T2 to T3 | locally |
| `durable_intent` | T3 to T4 (fsync) | locally |
| `request_write` | T4 to T5 | locally |
| `network_and_ack` | T5 to T6 | network and exchange |
| `trigger_to_ack` | T0 to T6 | both |
| `send_to_first_fill_ms` | dispatch row to first confirmed fill | network, exchange and private channel |
| `book_age_at_decision` | `dec_quote_age_ms` | market data freshness |
| `paper_modelled_latency` | paper `latency_ms` | **modelled, not measured** |

## Confirmed and suspected causes

The code is the confirmed cause: it is the check that stopped the signal.
Why the data was missing is a separate question. `suspected_causes` counts
the data and timing misses that had a market or Coinbase websocket
disconnect in the 30 seconds before them (`--record` needed). That is a
correlation, not proof.

## Reference results (simulated, 7 November 2026)

Fake venue (`tools/fake-venue`), synthetic data, 4 vCPU container.

| Run | Signals | Filters | Risk | Data not ready | Paper model | Price moved | Fills | Unclassified |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 11 min | 2,519 | 2,174 | 78 | 198 | 56 | 7 | 5 + 1 partial | 0 |
| 15 min | 3,511 | 3,239 | 47 | 181 | 31 | 10 | 3 | 0 |
| 10 min, every connection cut every 45 s | 2,230 | 1,961 | 26 | 222 | 15 | 3 | 3 | 0 |

Every `data_not_ready` miss in these runs was `DATA_VOL_WARMUP`: the
volatility window filling during the first ~60 s after start. It did not
recur after the 13 forced disconnects. Each market websocket reconnect took
0.27 s.
