# BTC 5-Minute Sniper — native C++20

This package contains a native Linux executable and its complete C++20 source.
Networking, strategy, order signing, paper fills, accounting, and settlement run
inside the C++ process. There is no Python interpreter, Python extension, Python
SDK, or Python subprocess in the bot, build scripts, or tests. The cryptographic
libraries include native C code. `SNIPER_HYBRID_SIZING` names the volatility and
drawdown sizing formula; it does not mean a mixed-language runtime. The only
Python in the package is under `tools/replay/`: it runs the reference Python bot
beside this program to prove they decide the same way. The bot never uses it.

**Release status (1.1.0): decision engine matched to the reference.** The
decision engine, market book and paper broker are now line-for-line ports of
the Python bot named in the PDF (`nevermind455/oct10@772f2eb`). Run on the same
recorded input, the Python code and this program made the same decision in
100% of 98,269 journal rows across 80 replays and of 2,572 rows from four
sessions the bot recorded itself, with every journal field identical; see
[the compliance report](docs/SPEC_COMPLIANCE.md). Still open
before live money: the 24-hour VPS paper run and the staged live canary.
No real orders were placed while preparing this package.

## What changed in 1.1.0

* **Decisions now follow the reference exactly**: gate order, skip texts,
  cooldown and in-flight handling, latency model, cached bankroll, hybrid
  drawdown snapshot and pause, Kelly/minimum/cent sizing in Python `Decimal`
  arithmetic, round caps and one journal row per order.
* **Paper fills follow `paper_trade.py`**: Decimal FOK/FAK walk, the
  decision-book pre-check, own-fill netting, booking checks and the
  reference's outcome labels.
* **Book state follows `feeds/book.py`**, including REST recovery fencing,
  websocket resubscription and the strict arrival book.
* **Settings follow `sniper_mode.Settings`**: same names, defaults and refusal
  texts. Reference features this build lacks are refused when switched on.
* **Live orders get the reference's pre-send check** (`LiveExecution.prepare`):
  spread, one-sided book, venue minimum, whole-cent and $1 stakes, visible
  liquidity under the cap, fee-rule changes, the hybrid budget and free cash,
  then the send guard. A refused order is journalled as `live refused` (or
  `live halted` while orders are paused) and nothing is sent.
* **Paper results will look worse than 1.0.1, on purpose.** 1.0.1 accepted a
  quiet book at the order's arrival by default; the reference does not. On a
  sparse feed many paper orders now end `UNKNOWN_BOOK_STATE`, exactly as the
  Python bot reports them. `SNIPER_BOOK_QUIET=1` is the reference's own opt-in.
* **Reference quirk kept for parity:** fee rules are cached once, ~30 s before
  a round starts, and trusted for 300 s, so the last ~20 s of every round skip
  as `fair value unknown: market fee rules not loaded` (in the Python bot too).
* New tools: `sniper-replay`, `tools/replay/py_replay.py`, `compare.py`,
  `run_parity.sh` (decision parity) and `sniper-diff` + `diff_cases.py`
  (20,000-case differential test). `--record FILE` writes the replay format.

## Start paper mode on a VPS

The supplied binaries target **Ubuntu 24.04, x86-64**, with OpenSSL 3, glibc 2.39
and libstdc++ from GCC 13. The simdjson shared library is included. This is not an
Android/Termux, Windows, macOS, ARM, or Ubuntu 22.04 binary. Termux can connect to
your Ubuntu VPS using SSH; run the bot on the VPS.

```bash
unzip BTC_5Minute_Sniper_Native_CPP_v1.1.0.zip
cd BTC_5Minute_Sniper_Native_CPP/sniper-native-cpp
bash scripts/setup.sh
bash scripts/run-paper.sh --run-for 300
```

The setup script creates `.env` from `.env.example` only when it does not exist,
sets its permissions to 600, and runs an offline self-test. Paper mode needs no
wallet or API credentials. For a continuous run, omit `--run-for`.

To validate settings without opening connections:

```bash
./bin/sniper-cpp --check
```

To observe public connectivity without making paper decisions:

```bash
./bin/sniper-cpp --health --run-for 90
```

The startup summary is printed immediately and then once per minute. Inspect
`state/paper/sniper.jsonl` for decisions, skip reasons and discovery refusals.
Volatility needs at least 60 seconds of valid data. Market discovery and fresh
books must be ready before an order can be considered. A signal is not a promise
of a fill. Paper fills are simulations, not evidence of live execution or profit.

## Build and test from source

For Ubuntu 24.04 with the usual main and universe package sources enabled:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build ca-certificates \
  libssl-dev libboost1.83-dev libfmt-dev nlohmann-json3-dev \
  libsimdjson-dev libnghttp2-dev libsecp256k1-dev \
  libgtest-dev libbenchmark-dev
bash scripts/build.sh
```

The build runs native offline tests, installs the binaries in `bin/`, and copies
the needed simdjson library to `lib/`. No Python package manager or interpreter
is used. See [dependencies and licenses](docs/DEPENDENCIES.md) for pinned library
versions. Rebuild on a different supported Linux architecture rather than using
the included x86-64 executable there.

Run the packaged tests directly:

```bash
./bin/sniper-tests
./bin/sniper-cpp --self-test
```

Optional development builds:

```bash
cmake -S . -B build-asan -G Ninja -DSNIPER_SANITIZE=ON
cmake --build build-asan --parallel 1
ctest --test-dir build-asan --output-on-failure
./build/sniper-bench --benchmark_min_time=0.5s
```

Sanitizer commands are provided for additional validation; they are not a claim
that this release passed a full runtime soak under sanitizers.

## Strategy (reference behaviour)

* Two Coinbase BTC-USD ticker sockets, deduplicated by sequence. A mid move of
  at least 0.5 bp within 250 ms signals UP or DOWN. A freshness gate (feed
  behind, backlog, unknown age, disconnected) refuses stale signals.
* Fair value anchors to the lower of the pre-move and current outcome mid:
  `fair = Phi(Phi^-1(m) + |r|/(sigma*sqrt(tau)))`, with sigma from one-second
  samples over 300 s (floor 0.2 bp, 60 samples to warm up). The limit is fair
  minus the fee curve and $0.01, floored to the tick; the cap is the lower of
  that and ask + $0.05.
* Fractional Kelly (0.25) scaled by volatility and drawdown, limited by venue
  minimums, whole-cent stakes, cash, the round caps and (live) the canary.
* FAK buys held to resolution. Paper models the order's arrival at
  send + ~14 ms network + 250 ms hold + 25 ms server + 200 ms paper extra.

The code is the authority: `docs/SPEC_COMPLIANCE.md` lists where the PDF and
the reference disagree and which one this build follows.

## Settings

Edit `.env`; use `./bin/sniper-cpp --check` after changes. The process environment
overrides `.env`. Unknown file settings appear as notices; they do not create
features. Never copy the old Python bot's entire configuration without review.

| Setting | Included default | Meaning |
| --- | --- | --- |
| `SNIPER_K_BP`, `SNIPER_WINDOW_MS` | `0.5`, `250` | Trigger threshold and window |
| `SNIPER_COOLDOWN_MS` | `300` | Per-side order cooldown |
| `SNIPER_MIN_LEFT_S` | `10` | Minimum time remaining |
| `SNIPER_STALE_MS` | `2000` | Market-book freshness bound |
| `MIN_BUY_PRICE`, `MAX_BUY_PRICE` | `0.05`, `0.95` | Ask price band |
| `SNIPER_SLIP`, `SNIPER_FAIR_MARGIN` | `0.05`, `0.01` | Price protection and fair margin |
| `SNIPER_KELLY_FRACTION` | `0.25` | Base fractional Kelly |
| `SNIPER_HYBRID_SIZING` | `1` | Volatility/drawdown risk scaling |
| `SNIPER_HYBRID_DRAWDOWN_STOP` | `0.20` | Drawdown stop fraction |
| `SNIPER_HYBRID_DRAWDOWN_PAUSE_S` | `600` | Drawdown pause before rebase |
| `SNIPER_KELLY_MIN_ENTRY` | `0` | Optional minimum entry, disabled |
| `SNIPER_MAX_SIDE_SHARES` | `200` | Per-outcome risk limit |
| `SNIPER_MAX_FILLS_ROUND` | `20` | Per-round fill limit |
| `PAPER_START_BALANCE` | `1000` | Initial simulated cash for a fresh ledger |
| `SNIPER_DELAY_SOURCE`, `SNIPER_FILL_ODDS` | `model`, `off` | Independent paper model |
| `SNIPER_BOOK_QUIET` | `0` | Reference opt-in: accept a quiet book at the order's arrival |
| `SNIPER_PTB_SKIP_CLOSE` | `0` | Optional price-to-beat gate; unvalidated here |
| `SNIPER_CANARY_MAX_SHARES` | `25` | Live order share cap |
| `SNIPER_CANARY_MAX_ORDERS` | `40` | Initial canary order budget |
| `SNIPER_CANARY_MAX_FILLS` | `20` | Initial canary fill-slot budget |
| `SNIPER_CANARY_MAX_ROUNDS` | `12` | Initial canary round budget |
| `SNIPER_CANARY_MAX_SPEND_USD` | `60` | Initial gross-spend budget |
| `SNIPER_CANARY_MAX_LOSS_USD` | `20` | Initial conservative loss budget |

These are initial canary budgets: reaching a budget pauses the session and the
implemented reference-style policy can allow one bounded increment after its
pause. They are not permanent lifetime account loss limits. See `canary_refusal`
in `src/strategy.cpp` and the native tests before changing them. The PDF calls for
a $5 loss budget in its first live canary; use that lower value for that staged
acceptance after the preceding checks have passed.

Live mode freezes the baseline trigger, cooldown, fair margin, slippage, FAK
type and sizing combinations. Changing them can correctly refuse startup.
Optional extra feeds, maker features, direction add-ons, measured fill odds,
strict paper/live parity and live cash mirroring are not implemented in this
baseline; enabling them explicitly refuses startup.

## Live interface — acceptance still required

The native signer and HTTP/2 sender are implemented and tested with dummy keys
and a local TLS exchange. This release has not been validated with a funded
wallet or a live canary. Complete the outstanding acceptance work first.

Only use one bot per wallet. The native process takes a wallet lock, but cannot
coordinate with a separate Python bot or a copy on another server. Stop other
trading processes yourself before a deliberate live test.

Supported signature types are EOA `0`, Polymarket proxy `1`, and Safe `2` for
legacy CTF outcome IDs using the pinned SDK's V2 signing format. New protocol-V2
position IDs needing exchange-V3 signatures are refused. Deposit-wallet type
`3` and newer protocol integrations are not implemented.

Credentials must be exported into the process environment, not saved in `.env`:

```bash
read -rsp 'Wallet private key: ' POLY_PRIVATE_KEY
export POLY_PRIVATE_KEY
export POLY_SIGNATURE_TYPE=0
export SNIPER_LIVE_CANARY=1
# For proxy/Safe types also export the correct POLY_FUNDER address.
# Optional: export POLY_API_KEY, POLY_API_SECRET, POLY_API_PASSPHRASE.
# Without all three, the native client derives/creates credentials itself.
```

After acceptance, the explicit first-session interface is:

```bash
bash scripts/run-live.sh --new-canary-session
```

Resume the same session using `bash scripts/run-live.sh`. Do not use
`--new-canary-session` to bypass an unresolved order. It creates a deliberate
new canary budget, not a reset of exchange positions or the accounting ledger.
`STOP_LIVE` in this project directory blocks new live submissions:

```bash
touch STOP_LIVE
```

Use Ctrl+C or SIGTERM for a normal stop. Neither a stop file nor shutdown sells
positions. Unknown outcomes remain reserved and must be reconciled; they are
never resubmitted. Redemption of actual winning tokens is manual.

## Files and recovery

* `bin/sniper-cpp`: native main executable; `bin/sniper-tests`: native offline tests;
  `bin/sniper-replay`, `bin/sniper-diff`: parity tools (see below).
* `src/`, `include/`, `tests/`: full source, test code and expected JSON fixtures.
* `reports/`: build, test, parity, paper-run and benchmark evidence
  (`TEST_REPORT.md` first, then `PARITY.md`).
* `tools/replay/`: decision-parity and differential-test tools.
* `tools/fake-venue/`: optional, development-only local venue (Node.js) for
  end-to-end paper tests; the bot never uses it.
* `state/paper/` and `state/live/`: created separately at runtime.
* `ledger.json.journal`: fsynced, sequenced, checksummed authorization/accounting
  events; `ledger.json`: atomic snapshot. Preserve both together for recovery.

The native ledger format is intentionally different from the Python ledger.
Never reuse Python ledger paths or delete live state to resolve an accounting
error. A corrupted native durable log refuses startup for investigation.

`--record PATH` writes every input the decision engine sees (Coinbase and market
frames, REST book snapshots, round changes, socket connects and drops) as JSON
lines. Replay a recording through this program and through the reference, then
compare them row by row:

```bash
./bin/sniper-replay rec.jsonl cpp.jsonl [settings.json]
REF_PYTHON -I tools/replay/py_replay.py REFERENCE_CHECKOUT rec.jsonl py.jsonl [settings.json]
python3 -I tools/replay/compare.py py.jsonl cpp.jsonl
```

`REFERENCE_CHECKOUT` is `nevermind455/oct10` at commit 772f2eb and `REF_PYTHON` a
Python 3.13 environment with its requirements installed. `settings.json` is a
flat object of `SNIPER_*` settings. Both replays use the same scripted fills, so
decisions are compared, not paper fill luck.
`--compare A B` compares two existing journals in row order.

For a missing round, inspect `discovery_refused` rows. For a missing shared
library or an incompatible glibc, rebuild on the target supported Linux host.
For `.env` permission refusal, run `chmod 600 .env`. Keep the `lib/` directory
beside `bin/`; the packaged binaries locate it relative to themselves.
