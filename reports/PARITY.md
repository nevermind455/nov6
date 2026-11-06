# Decision parity report (1.1.0)

Reference: `nevermind455/oct10@772f2ebb5aa907c6cc438299aa2469d9cb2051cd`, run by
`tools/replay/py_replay.py` (Python 3.13, polymarket-client 0.13.0). C++: the shipped
`bin/sniper-replay`. Each input goes through both; `tools/replay/compare.py` compares
every journal row and field. Both sides use the same scripted fills, so the comparison
is of decisions, not of fill luck.

## Sessions the bot recorded itself

Paper runs of `bin/sniper-cpp --record` against the bundled fake venue (real sockets,
TLS, HTTP/2 and timing; shuffled `price_change` entries), with the shipped
`.env.example` settings for both replays.

| recording | minutes | recorded inputs | python rows | c++ rows | decision parity | fields |
|---|---|---|---|---|---|---|
| 1 | 8 | 11,550 | 555 | 555 | 100.000% | identical |
| 2 | 7 | 9,688 | 452 | 452 | 100.000% | identical |
| 3 | 12 | 17,285 | 820 | 820 | 100.000% | identical |
| 4 (final build) | 11 | 15,932 | 745 | 745 | 100.000% | identical |

**4 recordings, 2,572 rows.**

## Generated sessions

`tools/replay/make_recording.py` sessions (4 rounds each) with feed stalls, duplicate
tickers, missing match times, socket drops, stale and invalid deltas and REST refreshes;
`drawdown` adds scripted realized losses that trigger the hybrid stop, pause and reset.
Settings per run: `tools/replay/run_parity.sh`.

| seed | settings | python rows | c++ rows | decision rows | decision parity | fields |
|---|---|---|---|---|---|---|
| 7 | caps | 1,223 | 1,223 | 1,223 | 100.000% | identical |
| 7 | default | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 7 | dollars | 1,219 | 1,219 | 1,219 | 100.000% | identical |
| 7 | drawdown | 1,223 | 1,223 | 1,221 | 100.000% | identical |
| 7 | fixed-latency | 1,222 | 1,222 | 1,222 | 100.000% | identical |
| 7 | flat | 1,215 | 1,215 | 1,215 | 100.000% | identical |
| 7 | fok | 1,215 | 1,215 | 1,215 | 100.000% | identical |
| 7 | gap | 1,224 | 1,224 | 1,224 | 100.000% | identical |
| 7 | min-entry | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 7 | no-hybrid | 1,218 | 1,218 | 1,218 | 100.000% | identical |
| 7 | no-limit | 1,313 | 1,313 | 1,313 | 100.000% | identical |
| 7 | one-socket | 1,229 | 1,229 | 1,229 | 100.000% | identical |
| 7 | quiet | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 7 | risk-cap | 1,218 | 1,218 | 1,218 | 100.000% | identical |
| 7 | tight | 1,274 | 1,274 | 1,274 | 100.000% | identical |
| 7 | venue-min | 1,222 | 1,222 | 1,222 | 100.000% | identical |
| 11 | caps | 1,213 | 1,213 | 1,213 | 100.000% | identical |
| 11 | default | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 11 | dollars | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 11 | drawdown | 1,213 | 1,213 | 1,211 | 100.000% | identical |
| 11 | fixed-latency | 1,212 | 1,212 | 1,212 | 100.000% | identical |
| 11 | flat | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 11 | fok | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 11 | gap | 1,212 | 1,212 | 1,212 | 100.000% | identical |
| 11 | min-entry | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 11 | no-hybrid | 1,208 | 1,208 | 1,208 | 100.000% | identical |
| 11 | no-limit | 1,266 | 1,266 | 1,266 | 100.000% | identical |
| 11 | one-socket | 1,202 | 1,202 | 1,202 | 100.000% | identical |
| 11 | quiet | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 11 | risk-cap | 1,208 | 1,208 | 1,208 | 100.000% | identical |
| 11 | tight | 1,282 | 1,282 | 1,282 | 100.000% | identical |
| 11 | venue-min | 1,208 | 1,208 | 1,208 | 100.000% | identical |
| 23 | caps | 1,227 | 1,227 | 1,227 | 100.000% | identical |
| 23 | default | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 23 | dollars | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 23 | drawdown | 1,227 | 1,227 | 1,225 | 100.000% | identical |
| 23 | fixed-latency | 1,229 | 1,229 | 1,229 | 100.000% | identical |
| 23 | flat | 1,221 | 1,221 | 1,221 | 100.000% | identical |
| 23 | fok | 1,221 | 1,221 | 1,221 | 100.000% | identical |
| 23 | gap | 1,237 | 1,237 | 1,237 | 100.000% | identical |
| 23 | min-entry | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 23 | no-hybrid | 1,217 | 1,217 | 1,217 | 100.000% | identical |
| 23 | no-limit | 1,286 | 1,286 | 1,286 | 100.000% | identical |
| 23 | one-socket | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 23 | quiet | 1,220 | 1,220 | 1,220 | 100.000% | identical |
| 23 | risk-cap | 1,228 | 1,228 | 1,228 | 100.000% | identical |
| 23 | tight | 1,294 | 1,294 | 1,294 | 100.000% | identical |
| 23 | venue-min | 1,221 | 1,221 | 1,221 | 100.000% | identical |
| 42 | caps | 1,227 | 1,227 | 1,227 | 100.000% | identical |
| 42 | default | 1,226 | 1,226 | 1,226 | 100.000% | identical |
| 42 | dollars | 1,225 | 1,225 | 1,225 | 100.000% | identical |
| 42 | drawdown | 1,230 | 1,230 | 1,228 | 100.000% | identical |
| 42 | fixed-latency | 1,231 | 1,231 | 1,231 | 100.000% | identical |
| 42 | flat | 1,224 | 1,224 | 1,224 | 100.000% | identical |
| 42 | fok | 1,224 | 1,224 | 1,224 | 100.000% | identical |
| 42 | gap | 1,231 | 1,231 | 1,231 | 100.000% | identical |
| 42 | min-entry | 1,226 | 1,226 | 1,226 | 100.000% | identical |
| 42 | no-hybrid | 1,228 | 1,228 | 1,228 | 100.000% | identical |
| 42 | no-limit | 1,351 | 1,351 | 1,351 | 100.000% | identical |
| 42 | one-socket | 1,231 | 1,231 | 1,231 | 100.000% | identical |
| 42 | quiet | 1,226 | 1,226 | 1,226 | 100.000% | identical |
| 42 | risk-cap | 1,231 | 1,231 | 1,231 | 100.000% | identical |
| 42 | tight | 1,309 | 1,309 | 1,309 | 100.000% | identical |
| 42 | venue-min | 1,226 | 1,226 | 1,226 | 100.000% | identical |
| 99 | caps | 1,217 | 1,217 | 1,217 | 100.000% | identical |
| 99 | default | 1,212 | 1,212 | 1,212 | 100.000% | identical |
| 99 | dollars | 1,211 | 1,211 | 1,211 | 100.000% | identical |
| 99 | drawdown | 1,217 | 1,217 | 1,215 | 100.000% | identical |
| 99 | fixed-latency | 1,215 | 1,215 | 1,215 | 100.000% | identical |
| 99 | flat | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 99 | fok | 1,206 | 1,206 | 1,206 | 100.000% | identical |
| 99 | gap | 1,223 | 1,223 | 1,223 | 100.000% | identical |
| 99 | min-entry | 1,212 | 1,212 | 1,212 | 100.000% | identical |
| 99 | no-hybrid | 1,211 | 1,211 | 1,211 | 100.000% | identical |
| 99 | no-limit | 1,320 | 1,320 | 1,320 | 100.000% | identical |
| 99 | one-socket | 1,212 | 1,212 | 1,212 | 100.000% | identical |
| 99 | quiet | 1,212 | 1,212 | 1,212 | 100.000% | identical |
| 99 | risk-cap | 1,209 | 1,209 | 1,209 | 100.000% | identical |
| 99 | tight | 1,290 | 1,290 | 1,290 | 100.000% | identical |
| 99 | venue-min | 1,207 | 1,207 | 1,207 | 100.000% | identical |

**80 runs, 98,269 journal rows, runs with any difference: 0.**

## Function-level checks

* `tools/replay/diff_cases.py` with `bin/sniper-diff`: 20,000 random cases through the
  reference's own functions and the C++ ports (Decimal FOK/FAK/share-sized walks, hybrid
  factors, budgets and drawdown stop, minimum entries, cent sizing, fair value, fair
  limit, Kelly, fees, `round`, `repr`): **0 mismatches**; 4,098 refusal texts compared
  verbatim.
* `tools/replay/live_prepare_cases.py`: 12 books through the reference's own
  `LiveExecution.prepare` (sized orders, a venue-minimum override and eight refusal kinds); the
  `Live.PresendCheckMatchesReferencePrepare` test checks the C++ pre-send check against
  that output: identical texts, spend, cap, reserve and fill estimate.
* `tools/replay/limit_vs_market.py`: the reference signs a share-sized live FAK as an
  SDK limit order, this program as an amount order of shares x price. With the pinned
  SDK's own functions, both give identical maker and taker amounts in all 268,800
  cases (four tick sizes, every on-grid price, 1-300 shares at whole cents).

Every replay and comparison here ran on 6 November 2026 with the shipped binaries.
Recordings 1-3 were made by earlier 1.1.0 builds (same recorder format) and
recording 4 by the shipped one.
