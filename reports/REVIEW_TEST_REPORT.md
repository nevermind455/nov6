Code review completed on 7 November 2026 against commit `35a4c30`.
The review focused on order accounting and recovery, the HTTP/2 sender, and
market discovery. Three correctness issues were fixed:

* **P1: stale order updates released known execution exposure.** A matched
  order followed by an older zero-fill cancellation/refusal lost its collateral
  reservation and could settle before its remaining fills were booked. Late
  acknowledgements could also reopen fully confirmed orders. The ledger now
  preserves execution evidence, reservations and canary fill slots, and reopens
  confirmed orders when cumulative matched shares exceed booked shares.
  Changes: `src/storage.cpp`; three regressions in `tests/native_tests.cpp`.
* **P2: HTTP/2 GOAWAY left the sender reporting ready.** A server draining a
  connection while keeping TCP open prevented new streams without triggering
  reconnection. GOAWAY now closes the connection and schedules reconnection
  after nghttp2 finishes processing the received frames. Unfinished requests
  remain unknown for reconciliation and are never resubmitted.
  Changes: `src/h2.cpp`; two local TLS regressions in `tests/transport_tests.cpp`.
* **P2: discovery ignored ISO timestamp timezone offsets.** Equivalent UTC and
  offset timestamps could select different instants, rejecting valid rounds or
  accepting incorrectly timed metadata. Discovery now applies the offset and
  rejects malformed timezone suffixes.
  Changes: `src/book.cpp`; three regressions in `tests/discovery_tests.cpp`.

Validation used Ubuntu 24.04 x86-64, GCC 13, Release builds, and the exact
dependency versions required by CMake. A clean worktree preserved the original
sources while the regression tests were added: all eight new tests failed.
After applying the fixes, all 58 tests in 22 suites passed through CTest.
The source files and tests in the validation worktree match this checkout.

All four programs in `bin/` were rebuilt and installed from the patched source.
The installed `bin/sniper-tests` also passed all 58 tests, confirming fixture
lookup and linkage after installation. These additional checks passed:

```sh
./bin/sniper-cpp --self-test
./bin/sniper-cpp --check
bash tests/live_scripts_tests.sh
git diff --check
sha256sum --check --quiet reports/SHA256SUMS
```

The launcher suite covers ten cases. Runtime validation ran in a container with
external networking disabled; the HTTP/2 tests use local TLS sockets. Funded
wallets, live venue acceptance, long paper runs and strategy parity replays were
outside this review. Existing release reports remain historical evidence for
release 1.1.0.
