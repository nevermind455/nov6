# Changelog

## Unreleased — fixes, live safety checks and a terminal dashboard

The programs in `bin/` were rebuilt from this source on Ubuntu 24.04 after
removing the location check. Everything in `reports/` except `SHA256SUMS`
still describes release 1.1.0.

* Live safety, ahead of a first live run (`docs/GO_LIVE.md`):
  * Canary budgets are now hard stops by default (`SNIPER_CANARY_HARD_STOP=1`).
    The reference's policy, where a budget grows after each pause, is `0`.
  * Live mode now enforces `SNIPER_MAX_FILLS_ROUND` and `SNIPER_MAX_SIDE_SHARES`.
    They had no effect, because the engine's round counters only follow paper fills.
  * Live mode accepts `SNIPER_SLIP=0.02` as well as the baseline 0.05.
  * Removed the server location check from live startup and the preflight script.
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
