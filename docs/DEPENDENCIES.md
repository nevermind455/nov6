# Build provenance and native dependencies

Target used for the included executables: Linux x86-64, Ubuntu 24.04, GCC 13.3,
glibc 2.39, system OpenSSL 3.0.13. C++20, `-O2`, no fast-math, no
`-march=native`, and `-ffp-contract=off`. The test executable uses GoogleTest.

| Dependency | Build version | Use |
| --- | --- | --- |
| Boost | 1.83.0 | Asio/Beast and multiprecision, header-only |
| fmt | 9.1.0 | Header-only formatting |
| nlohmann JSON | 3.11.3 | Structured metadata and accounting |
| simdjson | 3.6.4 | Coinbase parsing; shared library bundled |
| nghttp2 | 1.59.0 | Native HTTP/2, statically linked |
| libsecp256k1 | 0.2.0 | Recoverable deterministic signatures, statically linked |
| OpenSSL | 3.0.13 in build environment | TLS, HMAC, SHA-256 and randomness; system runtime library |
| GoogleTest | 1.14.0 | Native offline tests, statically linked |
| Google Benchmark | 1.8.3 | Development microbenchmarks; no benchmark runtime shipped |

Ubuntu package source paths and SHA-256 values are retained in
`reports/dependency_packages.json`. They identify the packages used in this
build; source builds using current Ubuntu packages may receive security updates.
CMake enforces the listed non-system library release versions.

The live shell launcher also requires `curl` and `jq` (`sudo apt-get install
curl jq`). Preflight reads the native program's effective `--check` JSON, so
exported settings and resolved state, journal and stop-file paths are checked
with the same configuration that the live process uses. It does not source
configuration as shell code.

`third_party/sha3.c` and `sha3.h` come from `mjosaarinen/tiny_sha3` at commit
`dcbb3192047c2a721f5f851db591871d428036a9`. The native wrapper uses Keccak-256
domain separation for Ethereum. Known-answer tests cover the empty and `abc`
digests. The C++ inverse-normal implementation is adapted from CPython
v3.12.10's AS241 implementation; its license is included. `src/pyrt.cpp`
reproduces CPython's float `repr`, `round` and `sum` results and the decimal
arithmetic of `Lib/_pydecimal.py` (same license), so money amounts round exactly
as in the reference bot. This is translated native code and does not link or
load Python.

The parity tools in `tools/replay/` are development-only. `compare.py` and
`make_recording.py` need only Python 3; `py_replay.py` and `diff_cases.py` run the
reference bot (`nevermind455/oct10` at 772f2eb, Python 3.13, its pinned
requirements including `polymarket-client 0.13.0`).

SDK fixtures were generated against the PDF's pinned `polymarket-client 0.13.0`
development oracle. The SDK is not a runtime or build dependency of this package.
The expected JSON fixtures contain a public dummy test key and dummy API inputs;
none authorize a real funded order.

Primary references checked during implementation:

* [Official Polymarket API](https://docs.polymarket.com/getting-started/api.md)
* [Order placement](https://docs.polymarket.com/trading/place-orders.md)
* [Fees](https://docs.polymarket.com/trading/fees.md)
* [Protocol migration](https://docs.polymarket.com/migrate/polymarket-v2/api-integrations.md)
* [Official legacy SDK market-info parsing](https://github.com/Polymarket/py-clob-client-v2/blob/main/py_clob_client_v2/client.py)

Dependency copyright notices are in `third_party/licenses/`. Those dependencies
retain their own licenses; the generated native application source is provided
for the user's requested bot implementation.
