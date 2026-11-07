# Audit verification artifacts

These artifacts belong to the 7 November 2026 audit, rather than the historical
release measurements in the parent directory. They contain synthetic test
evidence and public, unauthenticated market data. No funded order was sent.

* `native-tests.log`: complete normal CTest output (146 GoogleTests plus CLI
  and process I/O regressions).
* `sanitizer-tests.log`: the same suite with ASan, UBSan, float-cast-overflow,
  `detect_leaks=1`, `abort_on_error=1` and `halt_on_error=1`.
* `parity.log`: final 80-scenario strict replay comparison, after the recorded
  wall-clock correction, against `oct10@772f2ebb5aa907c6cc438299aa2469d9cb2051cd`.
* `differential.log`: final audited native executable versus the reference in
  20,000 seeded sizing, math and refusal cases.
* `signed-amounts.log`: 268,800 SDK limit-versus-market amount comparisons.
  `reference-presend.log` records the reference's 12 preparation cases; native
  preparation cases are asserted in the main native suite.
* `linkage.log`: packaged bot's native dynamic-library dependencies.
* `integration-summary.json`, `paper-output.log`, `paper-journal.jsonl` and
  `dashboard-*.txt`: a 360-second local paper/recording run crossing a round
  boundary, and a 270-second dashboard run at two terminal sizes. There were
  1,397 signals, five paper orders, one synthetic fill, no runtime errors and
  zero real submission calls. These are fixture results, not live performance.
  The accompanying `paper-ledger.json` and `paper-execution-audit.jsonl` allow
  the native edge report to trace the synthetic fill; `synthetic-edge-report.txt`
  correctly refuses an edge conclusion for one round. `public-edge-report.txt`
  reports NO DATA for the checked-in public-feed journal with zero fills.
* `public-event.json`, `public-market.json`, `public-book.json`: read-only
  responses observed on 7 November around 10:36–10:37 UTC, from:
  `https://gamma-api.polymarket.com/events/slug/btc-updown-5m-1794047700`,
  `https://clob.polymarket.com/clob-markets/0x40fa5572fb8a4b795216a56d7089e6be1f5d87589b609a65a1420750a3ea54a8`,
  and the CLOB `/book?token_id=` endpoint for the event's UP token. They are
  point-in-time examples, not current quotes.

Build/check reproduction on Ubuntu 24.04 with the documented dependencies:

```sh
bash scripts/build.sh
bash tests/live_scripts_tests.sh
node --test tools/fake-venue/oracle.test.js
cmake -S . -B build-asan -G Ninja -DSNIPER_SANITIZE=ON
cmake --build build-asan --parallel 1
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure
```

The local paper and terminal workflow is reproducible from
`.github/workflows/build.yml`. It uses development-only Node dependencies and
throwaway certificate files, not the real exchange. Optional Python reference
comparisons require the separately pinned reference environment; Python is not
needed to build, test or run the native bot.

An initial sanitizer/replay attempt exhausted this workspace's temporary disk
while Docker copied its filesystem and the linker wrote archives. Those runs
were discarded; the final artifacts above are successful reruns after freeing
reproducible build files. This was a verification-host resource failure, not a
suppressed sanitizer finding. Compiler warnings in pinned fmt/libstdc++ headers
were not converted into claims of a warning-free build.
