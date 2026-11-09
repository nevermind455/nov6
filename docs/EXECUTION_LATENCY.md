# Execution latency

The order sender already maintains a persistent HTTP/2 TLS connection. The
runtime probes `/time` about every five seconds and the transport sends HTTP/2
PINGs about every 15 seconds. Coinbase and market WebSocket messages already
feed the decision engine directly. REST discovery, account reconciliation and
book recovery run in background work; there is no REST price request inserted
between a live decision and its order submission.

The November latency changes keep those paths and add verified TCP_NODELAY and
SO_KEEPALIVE after REST, WebSocket and HTTP/2 TCP connections. These options
are set and read back once during setup. They do not disable application
timeouts, heartbeat checks or the rule that uncertain orders are never resent.
TCP_NODELAY controls this client's outgoing traffic; it cannot change the
remote market-data server's socket policy.

## Signing without changing the decision

Static EIP-712 type/domain data is prepared once, hashing uses fixed buffers,
and the token is parsed once per order. Full signed orders are not cached in
advance: Kelly size and the final price cap depend on the signal, book and
current account risk. Salt and timestamp remain fresh for every order. A
speculative grid of signatures is not substituted for the actual decision.

All minimum-size, cash, allowance, exposure, canary, freshness and expiry
checks remain. The durable authorization and its `fdatasync` remain before
submission. Local component benchmarks are in the
[latency report](../reports/LATENCY_2026-11-09.md); they do not measure WAN
latency or establish improved fills or profit.

## Exact timestamp boundaries

`T*_ns` values use the process's monotonic clock, in nanoseconds. They are not
Unix timestamps and must not be compared directly with a venue timestamp or
joined across process runs.

| Stamp | Boundary |
| --- | --- |
| `T0_ns` | Trigger message receipt, carried into the order plan. |
| `T1_ns` | Entry into live dispatch, before live pre-send checks. |
| `T2_ns` | Immediately before building/signing the exact order. |
| `T3_ns` | Signed order construction completed. |
| `T4_ns` | Canary authorization and durable intent completed. |
| `T5_ns` | Asynchronous complete-request TLS/socket write completed. This is not a physical NIC transmission or remote-receipt timestamp. |
| `T6_ns` | Entry into the complete HTTP response callback. |

`T0→T1` covers local decision work; `T1→T2` covers pre-send checks;
`T2→T3` covers order construction/signing; `T3→T4` includes authorization and
durable I/O. `T4→T5` also includes final guards, L2 headers, enqueueing and
the complete socket write. `T5→T6` combines network and venue response time.
An acknowledgement is not a confirmed fill. Missing or reversed boundaries
are unavailable evidence, rather than zero-duration samples.

When an order reply supplies a status string, `venue_order_status` preserves
it and `venue_reported_delay` identifies the literal `delayed` response.
That flag is evidence of what the API reported, not a measured hold duration.

Order rows also record `connection_generation`, `connection_ready_ns`,
`tcp_no_delay`, `tcp_keep_alive`, and, when available,
`recent_time_probe_median_rtt_ms` plus `time_probe_age_ms`. The connection
stamp means TLS/ALPN and local HTTP/2 initialization completed; it does not
mean the peer's SETTINGS acknowledgement arrived. Generation changes reveal
reconnections. The recent `/time` RTT is context, not a value that can be
subtracted to prove an exact exchange-imposed hold.

Private trade rows carry `evidence_source` and `evidence_received_ns`:

* `websocket`: captured at entry to the private WebSocket message handler.
* `rest_reconciliation`: captured after the REST page was decoded, before
  posting it to the event loop. This includes polling and response-decoding
  effects; it is not a socket-arrival or venue-execution timestamp.

`venue_event_times_raw` preserves supplied `match_time`, `last_update`,
`timestamp` and `created_at` fields without guessing their units or clock.
The missed-order report joins confirmed receipt evidence to the same run's
full-write stamp, and reports WebSocket and reconciliation receipts separately.
See [missed-order diagnostics](MISSED_ORDERS.md) for the output field names.
Neither client receipt time nor an HTTP ACK alone identifies the venue's
matching delay.

## Measure AMS3 and candidate VPS hosts

AMS3 is the user's current Amsterdam location. Run this on that VPS, from the
repository directory, after installing the updated native binaries:

```bash
./bin/sniper-net-probe --label ams3 --samples 100 --interval-ms 200 > ams3-latency.json
```

The probe sends only public `GET /time` requests. It uses no wallet, signing,
ledger or trading runtime. It measures connection setup separately, records
the first request separately from later requests, and reports subsequent RTT
count, minimum, p50, p95 and maximum using nearest-rank percentiles. Each
successful request has local enqueue/response timestamps and the connection
generation. The interval is the wait after a response before the next request;
total duration also includes setup and each response time.

`sent_ns` in probe samples means immediately before HTTP/2 enqueue, not the
order sender's full-write `T5_ns`. Probe RTT therefore includes client queuing,
network and server processing. The JSON states that limitation explicitly.
The first failed request, timeout or replacement connection fails the run;
partial successful samples are not silently turned into a successful result.
Exit status is 0 for success, 1 for an incomplete/failed probe and 2 for invalid
arguments. `--help` lists options. TLS verification stays enabled and existing
proxy/CA settings are honored.

Run the same command on each available, eligible candidate host with its own
label and output file. Keep endpoint, sample count and interval identical;
repeat at comparable times and workload. Compare failure rates, warm p50/p95,
variability and setup time. Inspect `configured_http_connect_proxy`: results
through different proxies or routes are not a controlled region comparison.
A label is user-supplied and is not independent proof of location.

A localhost fake-venue result tests software behavior, not a VPS route. A
managed-cloud result through a proxy is not an AMS3 measurement. No AMS3 SSH
connection or competing VPS host was available to this development task, so
no regional ranking or VPS relocation is claimed. Relocation should follow
actual host measurements and the venue's account/location requirements.

As checked on 9 November 2026, [Polymarket's infrastructure documentation](https://docs.polymarket.com/api-reference/geoblock)
lists primary servers in `eu-west-2` and `eu-west-1` as the closest
non-georestricted region. It lists the Netherlands as frontend-close-only
with the API itself unrestricted; UK API access is close-only except for
approved co-location access. These identify deployment constraints and a
candidate to measure, not proof that moving from AMS3 will reduce latency.
