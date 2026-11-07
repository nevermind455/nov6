# Build prompt vs this program vs the reference

The build prompt (C_Sniper_Bot___Build_Prompt.pdf) asks for a native C++ bot that
makes the same decisions as the Python bot at `nevermind455/oct10@772f2eb`, and says
the code wins where the two disagree. This file records what was checked, what
was wrong in 1.0.x, what 1.1.0 changes, and what is still not covered.

## Verdict

1.0.1 did **not** match. Its strategy code was an independent re-implementation
that departed from the reference in the gate order, journal rows, book state,
latency model, sizing arithmetic and paper fill model (list below). 1.1.0
replaces the decision engine, the book state and the paper broker with
line-for-line ports of the reference and proves the match by running the
reference's own Python code and the C++ code on the same recorded input.

| Check | Result |
|---|---|
| Decision parity, 80 replays (5 sessions x 16 settings), 98,269 journal rows | **100.000%** of decision rows identical; every compared field in every row identical |
| Decision parity on four sessions the bot recorded itself against the bundled fake venue (38 minutes, 2,572 rows) | **100.000%**, every field identical |
| Differential test, 20,000 random cases (Decimal FOK/FAK walk, share-sized FAK, hybrid sizing, minimums, fair value, fair limit, Kelly, fees, `round`, `repr`) | **0 mismatches**, including 4,098 refusal texts compared verbatim |
| Live pre-send check vs the reference's own `LiveExecution.prepare` (12 books: sized orders and every refusal kind) | identical refusal texts, signed spend, cap, reserve and fill estimate |
| Signed amounts of a share-sized live order: the reference's SDK limit order vs this build's amount order, 268,800 cases (`tools/replay/limit_vs_market.py`) | identical maker and taker amounts |
| Unit tests (`sniper-tests`) | 35/35 pass |
| Reference's own test suite (Python) | 92/92 suites pass on the pinned reference |
| Tick-to-wire, in process (frame -> decision -> signed order -> durable intent), this machine | p50 167 us / p99 320 us without fdatasync; p50 426 us / p99 817 us with it (prompt, on the VPS: p50 under 0.3 ms without the fsynced intent, under 2 ms with it) |
| Decision alone (frame -> sized plan) | p50 53 us / p99 120 us |

Reproduce: `tools/replay/run_parity.sh REPO PYTHON bin/sniper-replay OUT`,
`PYTHON -I tools/replay/diff_cases.py REPO bin/sniper-diff 20000` and
`PYTHON -I tools/replay/live_prepare_cases.py REPO` (the expected values of the
`Live.PresendCheckMatchesReferencePrepare` test). REPO = a checkout of the
reference at 772f2eb; PYTHON = a Python 3.13 environment with its requirements. Any recording the bot writes with `--record` replays the same
way: `sniper-replay rec.jsonl out.jsonl` and `py_replay.py REPO rec.jsonl out.jsonl`.

## What 1.0.1 got wrong (fixed in 1.1.0)

| Area | 1.0.1 | Reference (now ported) |
|---|---|---|
| Cooldown / in-flight skips | journal rows | counters only, no row |
| Gate order | own order, extra "market tick changed" gate | `_decide` order exactly; the book's tick is used, no extra gate |
| Fair value input | raw move | the journalled move rounded to 3 decimals |
| Latency model | fixed defaults | upper-median RTT/2 (14 ms default) + 250 hold + 25 server + 200 paper extra |
| Bankroll | live cash each decision | cash read every 5 s, stale after 30 s, minus spends inside a 2 s tail |
| Hybrid sizing | inline factors | ledger snapshot every 5 s, stale after 30 s; drawdown pause/reset rows |
| Money arithmetic | 1e-6 integer grid | Python `Decimal(str(float))` with context precision (exact port) |
| Minimum entry, cent sizing, budgets | approximations | `hybrid_sizing.py` exactly |
| Journal | pending row + separate result rows | one row per order, written with its outcome label |
| Book: crossed test | `bid > ask` | `bid >= ask` (a locked book is crossed) |
| Book: zero-size snapshot level | ignored | invalid snapshot, desync |
| Book: tick-size change | desync | tick update only |
| Book: arrival book | **quiet book accepted by default** (a 1.0.1 change) | strict: the feed must pass the arrival instant; quiet inference only with `SNIPER_BOOK_QUIET=1` |
| Book: REST refresh | used as history | never history ("REST snapshot is not stream-ordered history"); fenced by generation, update count and receive revision |
| Paper fills | integer micro-units, own outcome texts | `paper_trade.py` Decimal walk, its refusal texts and labels |
| Settings | own ranges and messages | `sniper_mode.Settings`: same names, defaults, parsing and refusal texts |

## Prompt components

| Component | Status |
|---|---|
| Config | Port of `Settings` (src/settings.cpp). Settings this build lacks are refused when switched on, never silently ignored. |
| Coinbase feed (2 sockets, dedup by sequence, freshness gate) | Port of `on_coinbase`, `_coinbase_conn` and `BinanceFreshness` |
| Polymarket market feed, book state | Port of `feeds/book.py` and `poly_market._dispatch`; hub REST recovery (`_resync_loop`) and WS resubscription |
| Rounds | Current round at the boundary, next round prepared up to `SNIPER_PREPARE_LEAD_S` (default 60 s, 30-240) before it; the reference prepares in a fixed 25-30 s slot. Decisions only use the current round, so parity is unchanged |
| Decision engine | Port of `Sniper._signal/_fire_now/_decide/_skip/_finish` (src/sniper.cpp) |
| Paper broker | Port of `paper_trade.py` (src/paper.cpp) |
| Journal | Reference row shapes for decisions; broker audit rows in `paper_audit.jsonl` |
| Recorder | `--record` writes the replay format both replay drivers read |
| Ledger, settlement | Native durable ledger (own file format); verified-resolution settlement |
| Live dispatch | `_fire_live` and `LiveExecution.prepare`: book, venue-minimum, whole-cent, $1, liquidity, fee-rule and free-cash checks on the decision book, hybrid recheck, send guard; refusals journalled as `live refused` / `live halted` |
| Signing, sender, canary, kill switch | From 1.0.x (SDK byte-identical signing vectors, local HTTP/2 tests) |

## Reference behaviours reproduced on purpose (worth knowing)

* **Fee rules expire inside the round when metadata is not refreshed.** The reference fetches a market's
  rules once, when the round is prepared (~30 s before it starts), and its
  fair-value gate only trusts them for 300 s. From ~30 s to 10 s before each
  round ends every signal is skipped as `fair value unknown: market fee rules
  not loaded` (seen in the reference's own replays: 46 skips, all at 10.5-29.4 s
  left). The decision engine retains that 300-second gate and reproduces it
  on the same replay metadata. The audited runtime now propagates newly
  discovered rules to its active and prepared round copies, so stale copies
  do not defeat successful metadata refreshes.
* **Strict arrival book.** A paper order whose arrival instant the market feed
  has not passed within 500 ms is `UNKNOWN_BOOK_STATE`, and an order whose last
  book state is a REST refresh is unknown too. `SNIPER_BOOK_QUIET=1` is the
  reference's opt-in for quiet books.
* **`SNIPER_PAPER_LIVE_PARITY` defaults to 1** in the reference, which makes
  default paper skip everything without live data. The prompt treats it as an
  optional phase-2 feature; this build runs the independent paper model and
  says so at start when the variable is unset (the shipped .env sets it to 0).

## Not implemented (refused when enabled)

Binance/Kraken triggers, extra feeds, the direction add-on, maker/resting
orders, wide caps, measured live fill odds (`auto` acts as `off`), the paper cash
mirror (`auto` acts as `off`), strict paper/live parity, the fresh market
socket, live-sampled delays (`auto` uses the model), `SNIPER_BOOK_CLOCK=wake`.
Diagnostics that never change a decision are also absent: shadow fills, book
traces, markout rows, guard diagnostics.

## Remaining differences from the reference

* The optional price-to-beat estimate uses the reference's legacy trigger-TWAP
  model, calibrated only with Gamma-published `priceToBeat` values. A first
  RTDS spot tick is not an official 60-second Chainlink TWAP; the audited build
  removes that misleading fallback. An authenticated PolyBolt TWAP client is
  not implemented. The `ptb*` fields therefore differ from production paths
  with an official streaming oracle (they only gate trades with
  `SNIPER_PTB_SKIP_CLOSE=1`, disabled by default).
* The paper broker's decision book is the live websocket view; the reference
  falls back to a REST fetch when that view has no asks (rare; refused here).
* A tick that disagrees with cached rules is taken from the book; the reference
  refetches the rules.
* Ledger and live journal files use this program's own formats, not
  `accounting/ledger.py`'s; the Python analysis tools read decision rows but not
  the native ledger. A live order is journalled as a `pending` row when sent,
  then the venue's reply and fills as separate rows.
* Live orders are share-sized FAKs only (`SNIPER_ORDER_SIZING=dollars` refuses
  `--live` at startup). `ASSUMED_MATCH_DELAY_SECONDS` is not read (the
  reference's default, 0, makes that check a no-op). Unknown send outcomes,
  reconciliation and canary budgets follow the 1.0.x native ledger rules, not
  `sniper_live.py`'s: an unknown outcome keeps its reservation, blocks its
  token until reconciled and pauses all new orders for 5 minutes, where the
  reference halts all new orders until the unknown is reconciled.
* Live collateral uses a confirmed-outflow watermark against the sampled
  venue balance. A confirmed fill cannot make its just-spent cash available
  again while the next balance request is in flight. The correction can
  temporarily understate available cash until the following refresh.
* Market discovery explicitly requires CTF protocol v1 and validates book,
  token, condition, tick, minimum and fee metadata together. Protocol v2,
  exchange-v3 signing and deposit-wallet signature type 3 remain unsupported.
* Execution reports distinguish durable intent, full HTTP/2 upload completion,
  HTTP acknowledgement and confirmed private fills. Historical benchmark
  numbers below exclude the HTTP write and network and are not new audit
  measurements. The audit's ledger snapshots use native format v2 with
  integrity checks; legacy v1 snapshots remain readable.

## Not verifiable here

* 24 h paper run on the VPS (`scripts/run-paper.sh` there).
* Live canary (no funded wallet; nothing in this work can send an order).
* Real Polymarket/Coinbase recordings (this sandbox cannot reach the venues;
  the replays use generated sessions and recordings made against the bundled
  fake venue).
