# Changelog

## 12 November 2026 — holding the venue's websockets

The bot never closed a venue websocket on purpose, but it could give one up
too easily, took a quarter of a second or more to get it back, and wrote
nowhere why it went.

* **Keep-alive.** The venue sockets send WebSocket pings when nothing has
  arrived for half the silence limit, and a pong counts as life. Before, 25
  seconds without a text reply closed a link that was still healthy.
* **A warm standby for the market feed** (`SNIPER_MARKET_STANDBY`, default
  on): a second connection, subscribed to the same tokens, takes over in the
  same instant when the live one drops; the books restart from fresh
  snapshots. On the fake venue with one market socket cut every 10 s the feed
  was down 0 ms per cut, against about 267 ms with one connection.
* **Reconnect** at once after a drop; a link that does not hold backs off
  (24 connections in a minute against a venue that drops every socket, where
  the old code made 159).
* A fault in the bot's own market handler no longer costs the connection.
* **Every socket up/down is journalled** (`"result":"ws"`) with its reason
  and the seconds left in the round; `sniper-miss-report` has a `sockets`
  section; the dashboard has a `POLY WS` line.
* `tools/fake-venue`: `VENUE_MARKET_KILL_EVERY_S`, `VENUE_NO_PONG`,
  `VENUE_MARKET_FLAP`.

See [docs/WEBSOCKETS.md](docs/WEBSOCKETS.md). No strategy rule changed.

## 11 November 2026 — rest a missed order on the book (`SNIPER_REST_LIVE`)

Opt-in, live only, off by default. No change to any decision, limit or size,
and none at all unless `SNIPER_REST_LIVE=1`.

* After a FAK the venue confirms matched nothing, the same BUY (its shares at
  its limit) can rest on the book as a post-only order for at most
  `SNIPER_REST_S` seconds. A fill is a maker fill: booked at the order's own
  price with no fee.
* `SNIPER_REST_DIR=1` (default) rests only the side that beats the price to
  beat by more than the estimate's own error.
* It is cancelled early when the trigger fires the other way, when BTC gives
  the move back, when the side falls behind the price to beat, when the feed
  goes stale, on any pause, and at shutdown. A cancel is repeated until the
  venue shows the order gone.
* While an order may be on the book its market takes no other order of ours.
* `SNIPER_REST_LIVE_STOP_USD` turns it off for good once the resting orders'
  own settled loss reaches it (`state/live/rest_live_test.json`).
* The order sender gained a cancel (`DELETE /order`) on the same HTTP/2
  connection; the ledger a `rest` flag per order and `rest_realized`; trade
  handling accepts a trade whose maker leg is our resting order (it was an
  "unauthorized fill" before); open-order reconciliation expects our own
  resting order; an inherited resting order is cancelled at start.
* `sniper-miss-report` prints a `resting_orders` section and knows the new
  reasons (`RISK_RESTING_ORDER`, `LATE_REST`).
* `tools/fake-venue` can play the account side (`VENUE_LIVE=1`): orders,
  cancels, the private channel, trades and balance, for end-to-end runs of
  `--live` with no real venue.

Details, rules and limits: [docs/RESTING_ORDERS.md](docs/RESTING_ORDERS.md).
Wire bytes are checked against the real SDK; the feature has not traded on
the real venue.

Found and fixed in a review of the above, before any live use:

* A fault while watching a resting order was caught as a Coinbase error (the
  feed reconnected) or, from the timer, stopped the bot. It is now reported,
  pauses new orders and never reaches the caller.
* A fault while handling the placement or cancel reply could reset the order
  connection. The placement reply now fails closed (unknown outcome, cancel).
* A cancel waited for the placement's answer however late it was. After one
  second it is sent anyway.
* An order that could not be cancelled because the order connection was down
  was never reported. Any order not shown gone five seconds after its cancel
  was wanted is now an error and pauses new orders.
* A placement reply arriving after the private channel had already shown the
  order's end put the order back to pending; a late "unknown" could too.
* The end of an order was written to the ledger again for every repeat of the
  same evidence, and its `rest_done` row could report fewer shares than had
  matched.
* The loss stop counted settled results only, so resting fills still open
  could take the test past it. One more order must now fit under the stop
  even if every open resting fill, and itself, lost all it paid.
* An order the venue confirmed cancelled but still lists is cancelled again.
* A missed order is not rested while another order of ours can still trade in
  the market.

Dashboard: the MARKET panel shows the resting order under the book (side,
shares, price, time left with a bar, or its state), recomputed every frame;
the ORDERS table has a line for each order that rested.

A second pass (sanitizer builds, connection-drop stress, static analysis):

* A resting order is cancelled when the private channel closes or goes
  silent; before, it stayed on the book with its fills unseen until the next
  reconciliation.
* A placement answered with any 2xx status and `live` is resting; null
  `errorMsg` and amount fields are read as absent; an order the venue takes
  without showing it on the book (`delayed`) is followed and cancelled at
  once instead of raising the unknown-outcome pause.
* Cancel replies match the order id whatever its letter case.
* When a late placement answer arrives for an order whose cancel is already
  wanted, the cancel goes out at once.
* A placement answered as resting for an order no longer followed is followed
  again and cancelled.
* The same fault in the resting-order watch is reported once in ten seconds.
* Should one of our orders ever take our own resting order, both sides of
  the trade are booked.
* `tools/fake-venue` gained `VENUE_SIGMA`, and its account side was used for
  the stress runs: with every connection cut every 6 s for ten minutes the
  ledger equalled the venue's record and no order was left open.

## 9 November 2026 — execution latency and measurement

* Precompute immutable EIP-712 signing data, avoid duplicate token parsing and
  temporary hashing allocations, preserving fresh order fields and signatures.
* Verify TCP_NODELAY and SO_KEEPALIVE on native connections and expose the
  warmed order session's generation and readiness in execution evidence.
* Separate dispatch from pre-send checks, capture private evidence receipt and
  source, and report same-run monotonic confirmation observations separately
  from REST polling and the legacy wall-clock metric.
* Add `sniper-net-probe`, a native read-only `/time` probe for AMS3 and candidate
  hosts, with setup/first-request/warm-request measurements and strict failures.

See [the measured review](reports/LATENCY_2026-11-09.md) and
[the execution timing guide](docs/EXECUTION_LATENCY.md). No strategy gate,
durability check, production deployment or VPS location was changed by this work.

## Unreleased — missed-order diagnostics, fault pauses and round switch timing

* **Fix: a repeated fault only paused new orders the first time.** `pause()`
  latched each "once" reason for the life of the process, so after the first
  unknown order outcome (or private trade failure, accounting disagreement,
  unauthorized wallet order, invalid private event, handler fault) a later one
  hours afterwards paused nothing, contrary to the documented 5-minute pause.
  The latch now holds only while that reason's own pause runs: a repeat inside
  it neither extends it nor logs again; after it expires the reason pauses
  again. `execution::PauseLatch`, regression
  `AuditRuntime.ARepeatedFaultPausesAgainOnceItsPauseHasExpired`.
* **Fix: triggers in the first 50 ms of a round were judged against the
  previous round.** The round switched only on the 50 ms timer, so a Coinbase
  trigger just after a boundary was skipped as too late in the old round. The
  Coinbase handler now switches first (before recording the frame, so replays
  keep the order). Measured cost 35 ns per message. Regression
  `AuditRuntime.TriggersAfterABoundarySwitchTheRoundFirst`.
* Add `sniper-miss-report` (native, read-only): reason codes and verdicts for
  every skip and refusal, the live order lifecycle, per-stage latency
  percentiles, evidence rows and suspected causes. `docs/MISSED_ORDERS.md`;
  8 `MissReport` regressions, including one that fails when the engine gains
  a reason without a code.
* `tools/fake-venue`: `VENUE_PROXY_DROP_EVERY_S` cuts every tunnelled
  connection for reconnection stress (development only).
* Add a round-switch check micro-benchmark.

## Unreleased — subscription cleanup and paper validation

* Retire book subscriptions when a discovery refresh drops the current or
  prepared round (identity change) without another round replacing it.
  `update_hub` used to return early in that case, leaving the old tokens
  subscribed and active until the next round change. Trading was already
  blocked there; this fixes the leak. Regression:
  `AuditRuntime.SubscriptionsFollowOnlyTheCurrentAndPreparedRounds`.
* Add `docs/PAPER_VALIDATION.md`: VPS paper procedure, where each item is
  logged and the acceptance checks, with an 11-minute simulated reference run.

## Unreleased — configurable next-round preparation

* Prepare the next round (subscribe both books, cache its rules) anywhere in
  the last `SNIPER_PREPARE_LEAD_S` seconds of the current round (default 60,
  allowed 30-240), instead of only in a fixed 25-30 s slot that a late
  discovery pass could miss. Strategy, sizing and risk are unchanged.
* Move the preparation timing into `execution::prepare_due` and cover the
  whole lead window, the old slot, a late tick and the boundary with a
  regression test.
* Add p95 to the tick-to-wire benchmark and a websocket book-update latency
  benchmark (`BookUpdatePercentiles`).
* Rebuild `bin/` with `scripts/build.sh` on Ubuntu 24.04 / GCC 13 and refresh
  `reports/SHA256SUMS`.

## 7 November 2026 — complete-system audit fixes

See [the full audit](reports/AUDIT_2026-11-07.md) for findings, source locations,
regression coverage and validation. No funded order or production deployment
was performed for this audit.

* Retain HTTP/2 upload providers through stream closure after early responses;
  record full-wire send timing only after the complete upload is written.
  Bound connection setup and keep uncertain orders reserved and never retried.
* Close REST order-firewall path variants, validate URL/proxy/TLS inputs, and
  repair stale active-market rules and accumulating account subscriptions.
* Prevent confirmed fills from reusing stale sampled collateral. Restore
  released fill slots on late positive evidence without charging a different
  canary session. Validate settlements and check monetary overflow before
  durable commits and during recovery.
* Pin initial capital before the first intent and verify checksummed v2
  snapshots. Read legacy v1 snapshots; older binaries cannot read v2. Back up
  the complete stopped state before upgrading and read the recovery notes.
* Reject unsupported market protocols, inconsistent rule/book identities,
  unsupported tiny ticks and malformed fee, timestamp and private-fill data.
  Remove spot-price substitution for official Chainlink TWAP calibration.
* Fix depleted-own-liquidity and absent-tick paper crashes, exact decimal and
  float boundaries, large formatting, cent-sizing overflow, decimal token-ID
  parsing and signing-context cleanup on constructor errors.
* Report confirmed fills, actual wire/ACK timing, fee-inclusive marks and
  collateral freshness accurately; strengthen terminal secret redaction.
* Read effective live configuration in preflight, honor selected paths, reject
  launcher configuration overrides and count uninterrupted paper history.
  Fail closed on audit/recording failure and incompatible state-file paths.
* Strengthen replay comparison and clock fidelity, fail replay output errors,
  pin development simulator dependencies and use a synthetic 60-second TWAP
  in the local fixture. Add native, CLI, launcher and simulator regressions.
* Review and incorporate concurrent main-branch additions: the native edge
  report, long-decimal venue observations, inherited-order reconciliation and
  confirmed-miss refunds. Make refunds durable, session-scoped and reversible
  on late fills; check venue rounding boundaries. Correct the analysis tool's
  clustered intervals, calibration verdict, evidence validation, round drawdown
  and read/argument error handling.

Strategy thresholds, five-share baseline, cent alignment, the $1 marketable
minimum, strict arrival-book policy and configured risk protections are kept.
Offline parity verifies the tested normal-domain decisions; it does not prove
future profitability or live exchange acceptance.

## Unreleased — fixes, live safety checks and a terminal dashboard

The programs in `bin/` were rebuilt from this source on Ubuntu 24.04 after
the accounting, HTTP/2 and timestamp fixes below. Existing release reports
still describe release 1.1.0; `reports/REVIEW_TEST_REPORT.md` records this review.

* Out-of-order empty order results no longer release known fills, collateral
  reservations or canary fill slots, and late acknowledgements cannot reopen
  fully confirmed orders without evidence of additional matched shares.
* An HTTP/2 `GOAWAY` now reconnects the sender even if TCP stays open. Outstanding
  orders remain unknown for reconciliation and are never resubmitted.
* Market discovery now honors ISO timestamp timezone offsets when validating
  round boundaries and rejects malformed timezone suffixes.

* Live safety, ahead of a first live run (`docs/GO_LIVE.md`):
  * Canary budgets are now hard stops by default (`SNIPER_CANARY_HARD_STOP=1`).
    The reference's policy, where a budget grows after each pause, is `0`.
  * Live mode now enforces `SNIPER_MAX_FILLS_ROUND` and `SNIPER_MAX_SIDE_SHARES`.
    They had no effect, because the engine's round counters only follow paper fills.
  * Live mode accepts `SNIPER_SLIP=0.02` as well as the baseline 0.05.
  * Removed the server location check from live startup and the preflight script.
  * New launcher flag `--skip-paper-check` explicitly skips the 24-hour paper-run
    requirement while keeping the remaining preflight checks and live limits.
  * New `--dry-run`: every live check and a signed order, but nothing is sent.
  * New `scripts/prepare-live.sh` and `scripts/live-preflight.sh`.
    `scripts/run-live.sh` now uses `.env.live` and requires the preflight.
  * A live start that fails now says which check did not pass.
* New `--dash` terminal dashboard, ported to C++ from the reference bot's
  `dashboard/` package. It draws on its own thread, so a slow terminal cannot
  delay trading. See "Terminal dashboard" in the README.

* A missing `--config` file now refuses startup instead of using defaults.
  `scripts/run-live.sh` without a `.env` refuses for the same reason.
* After a failed write or sync the ledger log takes no more rows, so a duplicate
  sequence number cannot damage it.
* A round passed during a long stall no longer blocks later round preparation.
* A `/time` probe with no reply now reconnects the HTTP/2 connection.
* A clock probe still in flight keeps its own send time.
* Closed rounds now calibrate the price-to-beat estimate, so
  `SNIPER_PTB_SKIP_CLOSE=1` can take effect.
* Reconciliation no longer journals already-booked trades every 5 seconds.
* Live summaries count sent orders.
* `--check` accepts the settings the runtime reads itself (such as
  `MAX_ALLOWED_SPREAD`) as used, and refuses invalid values for them.

## 1.1.0 — matched to the reference bot (6 November 2026)

The PDF names `nevermind455/oct10@772f2eb` as the source of truth. An audit of
1.0.1 against that code found its decision engine, book state and paper broker
were independent re-implementations that decided differently. They are
replaced by ports of the reference:

* `src/sniper.cpp` — `sniper_mode.Sniper`: `on_coinbase`, `_signal`,
  `_fire_now`, `_decide`, `_skip`, `_finish`, cached bankroll with its 2 s
  spend tail, hybrid snapshot with drawdown pause/reset, minimum entries,
  round counters, decision-book and price-to-beat journal fields.
* `src/book.cpp` — `feeds/book.py` and `poly_market._dispatch`; the runtime
  now runs the hub's REST recovery (`_resync_loop`) and websocket snapshot
  requests, and prepares the next round 25-30 s before it starts.
* `src/paper.cpp` — `paper_trade.py`: Decimal FOK/FAK/share-sized walks,
  decision-book check, arrival book, own-fill netting, booking checks.
* `src/settings.cpp` — `sniper_mode.Settings` names, defaults, refusals.
* `src/pyrt.cpp` — exact CPython `repr`, `round`, `sum` and `decimal.Decimal`.
* Live dispatch (`src/runtime.cpp`, `live_presend_check` in `src/paper.cpp`)
  — `_fire_live` and `LiveExecution.prepare`: the pre-send check on the
  decision book (spread, one-sided, venue minimum, whole cent, $1, liquidity
  under the cap), fee-rule change, hybrid budget with the real reserve, free
  cash, and the send guard. Refusals are `live refused`/`live halted` skips,
  as in the reference. `SNIPER_ORDER_SIZING=dollars` now refuses `--live` at
  startup instead of refusing every order.

**Reverted from 1.0.1:** the quiet-book default in the paper broker. It made
paper fill more often than the reference ever would; quiet inference is again
the reference's opt-in `SNIPER_BOOK_QUIET=1`.

**Proof:** 80 replays (5 generated sessions x 16 settings, 98,269 rows) plus
four sessions the bot recorded against the fake venue (2,572 rows), through
the Python reference and this code: 100% identical decisions and fields.
20,000 differential cases: 0 mismatches. 12 live pre-send cases run through the
reference's own `LiveExecution.prepare`: identical texts and amounts.

**Measured (this build machine, in process):** decision p50 53 us / p99 120 us;
frame to signed order to durable intent p50 167 us / p99 320 us without
fdatasync, p50 426 us / p99 817 us with it (PDF targets: p50 under 0.3 ms and
under 2 ms).

**Behaviour you will notice:** more `UNKNOWN_BOOK_STATE` in paper (as the
reference), skips in the last ~20 s of each round for expired fee rules (as
the reference), one journal row per order.

The ledger file format is unchanged; existing `state/` directories load.

## 1.0.1 — fixes (6 November 2026)

No strategy parameter, trigger, sizing, signing or ledger format changed.
Existing `state/` directories load unchanged.

### Bugs

1. **Paper broker refused most orders as `UNKNOWN_BOOK_STATE`**
   (`src/runtime.cpp` `paper_tick`, new `book_at_arrival` in `src/book.cpp`).
   A paper order is matched against the book at its modelled venue arrival.
   The old code required a book update stamped *after* that arrival. When the
   book stayed quiet through `SNIPER_BOOK_WAIT_MS` it refused the order, even
   though a live, same-generation, fresh book with no updates had, by
   definition, not changed. That is precisely the stale-quote case this
   strategy trades, so paper results were biased toward "no fill".
   Now: an update stamped at or after arrival proves the arrival view (as
   before); otherwise, at the deadline, a book that is still live, in the same
   generation and no older than `SNIPER_STALE_MS` is used as-is. A resync,
   disconnect or stale book still refuses with `UNKNOWN_BOOK_STATE`. Fill and
   refusal rows now carry `"book"` (`venue time` or
   `unchanged through arrival`) and `"book_age_ms"`.

2. **Valid `price_change` messages desynchronised books** (`src/book.cpp`).
   The crossed-book check ran after every entry. The venue does not order the
   entries of one message, so a book that moves up a level (new bid above the
   old ask, old ask removed later in the same message) looked crossed
   half-way through. Each false desync bumped the book generation, which
   made the book "not live", erased the pre-move history used by fair value
   and refused every pending paper order on that token. The check now runs
   once per touched book after the whole message; a message that really
   leaves the book crossed still fails closed.

3. **An exception in any background result handler ended the process**
   (`src/runtime.cpp`). Discovery, book-refresh, balance, reconciliation and
   settlement results run on the event loop through `net::post`. A throw there
   (for example a malformed `size_matched`, or `.value()` on a non-object
   reply) unwound out of `io.run()` and `main`, with no final summary and no
   ledger snapshot. Handlers are now guarded: the fault is journalled as an
   `error` row and live mode pauses new orders (fail closed). If anything
   still escapes, the bot stops in order with exit code 1.

4. **One slow discovery request froze every REST book refresh**
   (`src/runtime.cpp`). Discovery (up to eight REST calls per cycle, 10 s
   timeout each) and book refreshes shared one thread, so a hung Gamma or CLOB
   call left quiet books without a refresh and the strategy skipped with
   `book stale`. Book refreshes now have their own thread and a 3 s timeout.

5. **Audit rows with invalid UTF-8 threw** (`src/storage.cpp`). Venue error
   text is now written with invalid bytes replaced instead of throwing out of
   an event handler.

### Errors and warnings

6. **Packaging:** the zip stored `bin/sniper-cpp`, `bin/sniper-tests` and the
   scripts without execute permission, so running them directly failed with
   `Permission denied` unless `setup.sh` had run first. The README said
   `cd sniper-native-cpp` but the zip unpacks to
   `BTC_5Minute_Sniper_Native_CPP/sniper-native-cpp`. Both fixed.
7. **Compiler warnings:** unchecked `nice()` (three sites), unchecked simdjson
   `parser.allocate()`, and three GCC 13 `-Warray-bounds` false positives from
   fmt 9's fixed-precision float path. The build is now warning-free;
   `fixed6()` replaces `fmt::format("{:.6f}")` and is tested against
   `printf("%.6f")` on 20,000 values.
8. **CMake 3.30+** warned about removed FindBoost (CMP0167); the policy is
   now set to use Boost's own CMake config.
9. The HTTP/2 `GET /time` clock probe sent no `user-agent`/`accept`, unlike
   every other request. The live preflight needs this clock.
10. Live: a zero collateral balance re-wrote a risk-rebase event to the
    durable log every 5 seconds. Rebase now waits for a positive balance.

### Tests

Three regression tests added (34 total, all passing):
`Book.PriceChangeIsCheckedAfterTheWholeMessage` (fails on the original code),
`Paper.QuietBookIsTheBookTheOrderMet`, `Money.FixedSixMatchesPrintfRounding`.

### End-to-end evidence (simulated venue, original vs fixed side by side)

The original code (rebuilt with diagnostic journal fields only, no logic
change) and the fixed code ran in paper mode at the same time against the same
local fake Polymarket/Coinbase venue (`tools/fake-venue/`), so they saw
identical feeds.

| Scenario | Original | Fixed |
| --- | --- | --- |
| Normal feed, 10 min | 30 orders, 9 fills, 18 `UNKNOWN_BOOK_STATE` | 29 orders, 16 fills, 0 unknown |
| Unordered `price_change` entries, 5.5 min | 16 orders, **0 fills**; 90 false desyncs; 307 `book not live` + 49 `no pre-move market history` skips | 15 orders, 10 fills; 0 desyncs, 0 of those skips |
| Gamma requests hang 12 s, 5.5 min | 29 orders, 4 fills; 92 `book stale` skips | 15 orders, 11 fills; 0 `book stale` |

The fixed bot places fewer orders once it holds positions: Kelly sizing
subtracts held shares (`kelly size under the 5-share minimum`). All six runs
exited 0 with 0 handler errors, rolled over rounds, and settled every round
with a position that closed early enough to resolve before the run ended.
These are simulations of the paper broker's mechanics. They are not evidence
of live fills or profitability.
