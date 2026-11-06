# Native verification results (1.1.0)

Release 1.1.0 prepared 6 November 2026. No funded wallet was used and no real
order was sent. This build machine cannot reach Polymarket or Coinbase, so the
end-to-end runs below use the bundled fake venue (`tools/fake-venue/`).

## Same decisions as the reference bot

Details and per-run tables: `PARITY.md`.

* **Decision parity: 100.000%.** 80 replays (5 generated sessions x 16
  settings, 98,269 journal rows) through the reference's own Python decision
  code and `bin/sniper-replay`: every decision and every compared field
  identical.
* **Sessions the bot recorded itself:** four paper runs with `--record`
  against the fake venue (38 minutes, 54,455 recorded inputs) replayed through
  both: 2,572 rows, 100.000%, every field identical.
* **Differential test:** 20,000 random cases through the reference's functions
  and `bin/sniper-diff`: 0 mismatches, 4,098 refusal texts compared verbatim.
* **Live pre-send check:** 12 books through the reference's own
  `LiveExecution.prepare`; the C++ check gives the same refusal texts and the
  same spend, cap, reserve and fill estimate.
* **Live signed amounts:** a share-sized live order has the same maker and
  taker amounts as the reference's SDK limit order in all 268,800 grid cases.

## Build and native tests

* Clean release build from scratch (`scripts/build.sh`, GCC 13.3, `-Wall
  -Wextra`): no warnings (`build.log`).
* **35 native tests in 20 suites pass** (`native_tests.log`, `native_tests.xml`),
  including SDK signing vectors, the 57,400-case amount grid, ledger crash and
  restart cases, the local TLS/HTTP/2 exchange (matched, HTTP 400, uncertain
  write never retried), book state, the paper Decimal walk, CPython numerics,
  settings refusals, the live pre-send check and live refusal journalling.
* The startup self-test passes. No Python library is linked by any shipped
  binary (`linkage.txt`, `python_source_scan.txt`).
* `--check` on the shipped `.env.example`: paper, no notices
  (`config_check.json`). `--live --check` with `SNIPER_ORDER_SIZING=dollars`
  refuses at startup, as intended.

## Paper runs against the fake venue

Each run used the shipped `.env.example`, real sockets, TLS and HTTP/2 to the
local venue, and its own state directory. Counts are from the bot's own
summary, journal and ledger.

| | Run 3 (12 min) | Run 4, shipped binary (11 min) |
| --- | --- | --- |
| Signals | 2,627 | 2,431 |
| Orders | 23 (one still in flight at shutdown) | 24 |
| `FILLED` | 4 | 2 |
| `UNKNOWN_BOOK_STATE` | 16 | 17 |
| `PRICE_CAP_MISS` | 2 | 5 |
| Rounds settled | 1 (lost: realized -$3.61, payout $0) | 0 (its one held round resolved after the run stopped) |
| Handler errors | 0 | 0 |
| Order-capable calls in paper | 0 | 0 |
| Exit code / peak RSS | 0 / 49.5 MB | 0 / 49.1 MB |

Run 3 used the 1.1.0 build from before the live pre-send change, whose paper,
ledger and settlement code is identical to the shipped one. In run 3 the
ledger reconciles exactly: cash $916.39 = $1,000 - $81.60 cost - $2.01 fees
+ $0 payout.

**Why most paper orders end `UNKNOWN_BOOK_STATE`.** As in the reference, a
paper order is matched against the book at its modelled arrival (send + 14 ms
network + 250 ms hold + 25 ms server + 200 ms paper extra), and only once the
market feed has passed that instant, waiting at most `SNIPER_BOOK_WAIT_MS`
(500 ms). The fake venue's market maker requotes about once a second, so the
feed often has not passed the arrival in time; the reference labels these
orders the same way. A busier real book should leave fewer orders unknown.
`SNIPER_BOOK_QUIET=1` is the reference's opt-in that accepts a quiet book.
1.0.1 accepted quiet books by default; that diverged from the reference and is
reverted. The 1.0.1 side-by-side runs are therefore superseded by these.

The simulated market maker is a model: these runs exercise mechanics, not live
fill rates or profitability.

## Latency (`sniper-bench`, release build, this machine)

Medians of five runs on a 2-vCPU container (`benchmark.log`, `benchmark.json`).
The benchmark library itself is a debug build (its warning is in the log).

| Path | p50 | p99 |
| --- | --- | --- |
| Coinbase frame -> sized order plan | 53 us | 120 us |
| Frame -> plan -> signed order -> durable intent, no fdatasync | 167 us | 320 us |
| The same with the intent fdatasync | 426 us | 817 us |

The PDF's targets, on the VPS: p50 under 0.3 ms without the fsynced intent and
under 2 ms with it. The HTTP/2 write and the network are not included. Maths
alone: 96 ns; one recoverable signature: 28.8 us.

## Earlier evidence kept

`public_paper_smoke.log` and `public_paper_audit.jsonl` are the 1.0.0
package's 100-second smoke run on the real public feeds. They predate 1.0.1
and 1.1.0 and show connectivity only.

## Outstanding acceptance

The 24-hour paper run on the VPS, replays of real-venue recordings (run
`--record` there, then `tools/replay`), the full fault matrix, a week-long
memory test and the staged live canary with a $5 loss budget remain. The hot
path is not allocation-free. Details: `docs/ACCEPTANCE.md` and
`docs/SPEC_COMPLIANCE.md`.
