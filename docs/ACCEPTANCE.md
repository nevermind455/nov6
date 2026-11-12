# Acceptance report (1.1.0)

Source reference: the supplied 23-page C++ build prompt and
`nevermind455/oct10@772f2ebb5aa907c6cc438299aa2469d9cb2051cd`.
This report describes the delivered implementation; it does not certify all of
the PDF's acceptance conditions.

| PDF requirement | Current evidence/status |
| --- | --- |
| Native implementation, no Python runtime | Implemented. Native ELF executable, CMake, shell scripts and GoogleTest tests; no Python library is linked (`reports/linkage.txt`). The only Python files are the parity tools in `tools/replay/`, which run the reference bot for comparison and are never used by the bot. |
| Native signing identical to pinned SDK | Six SDK-produced signature/digest/exact request-body vectors, L1 authentication, L2 HMAC, proxy/Safe derivation and amount-grid fixtures pass. Dummy key only. |
| Pure math parity | 10,000 expected math vectors, plus 20,000 random differential cases evaluated by the reference's own Python functions (Decimal fill walks, hybrid sizing, minimums, fair value, Kelly, `round`, `repr`): 0 mismatches. |
| Discovery and public feeds | Current and upcoming BTC 5m rounds, two Coinbase ticker connections, market WebSocket and public REST rules implemented. Short public paper smoke evidence is included. |
| Paper broker | Port of `paper_trade.py`: Decimal FOK/FAK and share-sized walks, decision-book pre-check, arrival book from the feed's exchange-time history (strict; `SNIPER_BOOK_QUIET=1` opt-in), own-fill netting, cash and hybrid booking checks, reference outcome labels. Independent model delays; no measured live calibration. |
| Live broker | Native secp256k1/Keccak signing, native HMAC, one guarded warm HTTP/2 sender, private fills and REST reconciliation implemented. The reference's pre-send check (`LiveExecution.prepare`) is ported and matches it on 12 test books; refusals are journalled as in the reference. Local fake-CLOB tests cover matched, rejected and uncertain-write outcomes. No real order was sent. |
| Durability | Fsynced authorization log, checksum/sequence verification, atomic snapshots, restart/crash test, duplicate-fill/settlement guards, partial-fill reservation, persisted drawdown pause and fill-slot release tests. |
| 24h paper on user's VPS | **Not performed.** Short smoke evidence cannot replace it. |
| At least 99.9% full decision parity | **Met on recorded input: 100.000%.** 80 replays (5 sessions x 16 settings, 98,269 rows) and four sessions the bot recorded against the fake venue (38 minutes, 2,572 rows) run through the reference's own Python decision code (`tools/replay/py_replay.py`) and this program (`sniper-replay`); every decision and every compared journal field identical. Real Polymarket/Coinbase recordings: run the same tools on a `--record` file from the VPS. |
| Python accounting/journal compatibility | **Not complete.** Native ledger is separate format `sniper-native-ledger-1`. Python ledger round-trip, SELL-lot imports and all Python analysis-tool compatibility are not provided. |
| Tick-to-wire latency | **Measured in process** (`sniper-bench`, TickToWirePercentiles): frame -> decision -> signed order -> durable intent p50 167 us / p99 320 us without fdatasync, p50 426 us / p99 817 us with it (medians of five runs on the 2-vCPU build machine). The PDF's VPS targets are p50 under 0.3 ms without the fsynced intent and under 2 ms with it. The HTTP/2 write and network are excluded. |
| Allocation-free hot path | **Not achieved.** JSON, string, container and snapshot activity still allocate; ledger work can block. |
| Full fault matrix and week of bounded memory | **Not completed.** Book/history/maps have limits, but the required long-duration proof is absent. |
| Live $5 canary after paper/parity acceptance | **Not performed.** Requires completion of earlier acceptance steps and deliberate user-side setup. |
| Optional phase-3 features | Extra feeds, maker-only and split maker orders, direction add-ons, measured odds, strict paper/live matching and cash mirroring are explicitly refused when enabled. |
| Optional price-to-beat gate | Reference legacy estimate (trigger TWAP, calibrated by published values) ported and replay-verified; the Python production path instead reads Polymarket's RTDS reference, which is not ported. Disabled by default. |

## Material differences and limits

The venue's standard-market response can omit `nr`; native discovery now uses
the same false default as the official SDK. This fixes the initial no-round
failure and has a regression fixture from the public API response. Missing
fees, token identities or other required parameters still refuse trading.

Paper timing uses the reference's live-like model (half the upper-median CLOB
round trip, 14 ms until measured, + 250 ms hold + 25 ms server + 200 ms paper
extra); measured live delays and fill odds are not implemented (`auto` uses
the model). Receive backlog is estimated from available kernel bytes, not the
Python WebSocket library's queued-frame count; replays use the recorded value,
so this can only matter live. See `docs/SPEC_COMPLIANCE.md` for every remaining
difference from the reference.

Native state files use a separate format. They are not a migration of an
existing wallet's Python trade history. Live cash comes from the venue, but
positions authorized outside this native ledger are not imported automatically.
Settlement waits for all known matched fills to be booked. New fills after a
recorded settlement refuse accounting and require investigation.

Only legacy CTF outcome tokens and EOA/proxy/Safe types 0–2 are covered by the
signed wire format. Polymarket's newer protocol-V2 position namespace and
exchange-V3 signing, deposit-wallet type 3, automatic token redemption and
session-key flows are outside this baseline. The similarly named SDK wire
version 2 is distinct from the newer protocol-V2 position system.

The freeze checks and canary controls are implemented, but have not been
certified to match every Python startup/refusal path. A temporary wallet lock
coordinates copies on this Linux host only. Audit/recorder failures pause new
submissions; this does not guarantee recovery from every possible storage fault.

Decision parity is now shown on recorded input. The 24-hour VPS paper run and
the staged live canary are still required before live money.
