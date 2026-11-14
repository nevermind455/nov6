# Fake venue (development only)

A local stand-in for every host the bot uses in paper mode: Gamma discovery,
CLOB REST and HTTP/2 (`/time`, `/clob-markets`, `/book`, `/markets`), the
market WebSocket, the Coinbase ticker feed and the Polygon RPC used for
settlement. A lagging market maker quotes 5-minute rounds around a random-walk
BTC price, so the sniper sees real trigger, fill, rollover and settlement
paths. The bot does not load or depend on this; it needs Node.js 18+.

It was used to find and confirm the fixes in `CHANGELOG.md` by running the old
and new binaries side by side against the same feed.

```bash
cd tools/fake-venue
bash make-certs.sh
npm ci --ignore-scripts --no-audit --no-fund
node --test oracle.test.js
node venue.js &                      # proxy on 127.0.0.1:18080

mkdir -p /tmp/sim && cp ../../.env.example /tmp/sim/.env && chmod 600 /tmp/sim/.env
HTTPS_PROXY=http://127.0.0.1:18080 SSL_CERT_FILE=$PWD/certs/ca.crt \
  ../../bin/sniper-cpp --config /tmp/sim/.env --paper --run-for 600
```

Journal: `/tmp/sim/state/paper/sniper.jsonl`. Never point a live run at it.

The fixture computes its strike and outcome from a synthetic 60-second,
last-observation-held time average. This exercises TWAP boundary handling;
it is not a Chainlink feed or evidence of real trading performance.

## Live mode (`VENUE_LIVE=1`)

With `VENUE_LIVE=1` the venue also plays one account: the ban-status check,
balance and allowances, `POST /order` and `DELETE /order` on HTTP/2, the
private (`/ws/user`) channel with order and trade events, `/data/trades`,
`/data/order/<id>` and `/data/orders`. Order ids are the order's EIP-712 hash,
so the bot's own id check applies. A post-only GTC rests until it is cancelled,
a seller hits it, or the market maker's offer comes down to it; a trade goes
MATCHED, MINED, CONFIRMED. Without the variable every order path answers as
before (`paper must not post`). The key below is a published test key.

```bash
VENUE_LIVE=1 VENUE_FAK=book node venue.js &
mkdir -p /tmp/sim-live && cat > /tmp/sim-live/.env <<'ENV'
SNIPER_LIVE_CANARY=1
SNIPER_SLIP=0.02
SNIPER_FAIR_VOL_S=90
SNIPER_KELLY=0
SNIPER_FLAT_LIVE=1
SNIPER_CANARY_MAX_ORDERS=1000
SNIPER_CANARY_MAX_FILLS=1000
SNIPER_CANARY_MAX_ROUNDS=1000
SNIPER_CANARY_MAX_SPEND_USD=10000
SNIPER_REST_LIVE=1
SNIPER_REST_S=8
SNIPER_REST_DIR=0
ENV
chmod 600 /tmp/sim-live/.env
HTTPS_PROXY=http://127.0.0.1:18080 SSL_CERT_FILE=$PWD/certs/ca.crt \
  POLY_PRIVATE_KEY=0x0000000000000000000000000000000000000000000000000000000000000001 \
  POLY_API_KEY=offline-key POLY_API_SECRET=c2VjcmV0LWZvci1vZmZsaW5lLXRlc3RzLW9ubHk= \
  POLY_API_PASSPHRASE=offline-pass \
  ../../bin/sniper-cpp --config /tmp/sim-live/.env --live --new-canary-session --run-for 600
```

`SNIPER_REST_DIR=1` needs the price-to-beat estimate checked on
`SNIPER_PTB_MIN_CHECKS` rounds first (the venue publishes it in Gamma); allow
half an hour with `SNIPER_PTB_MIN_CHECKS=1`.

| Variable | Default | Effect |
| --- | --- | --- |
| `VENUE_FAK` | `miss` | `miss`: every FAK matches nothing. `book`: it takes the offer if that is still inside its limit after the hold |
| `VENUE_ORDER_DELAY_MS` | `300` | The taker hold before a FAK is answered |
| `VENUE_REST_FILL_RATE` | `0.05` | Sellers hitting a resting bid, per second |
| `VENUE_REST_PARTIAL` | `0.3` | Share of those fills that are partial |
| `VENUE_CANCEL_FAIL` | `0` | Share of cancels answered with HTTP 500 |
| `VENUE_QUIET_USER` | `0` | `1`: no order events on the private channel (the bot must read the order's record) |
| `VENUE_BALANCE` | `100` | Account balance, dollars |
| `VENUE_PTB` | `1` | Publish `eventMetadata.priceToBeat` for started rounds |
| `VENUE_SIGMA` | `0.0001` | BTC volatility per square-root second |
| `VENUE_REDEEM_AFTER_S` | `0` | The owner redeems a resolved round this many seconds after its result: winning tokens become collateral, the round's tokens leave the wallet (`0` = never, like an owner who has not claimed yet) |

The Polygon RPC answers `balanceOf(address,uint256)` with the wallet's
outcome tokens (every confirmed fill adds its shares), which is what the
dashboard's `REDEEMABLE` reads after a win. Collateral is never credited on a
win unless the owner redeems.

Stress knobs (environment variables for `node venue.js`):

| Variable | Default | Effect |
| --- | --- | --- |
| `VENUE_SHUFFLE` | `1` | Shuffle entries inside each `price_change` message (`0` = removals first) |
| `VENUE_MM_LAG_MEAN` | `1.0` | Mean seconds between market-maker requotes |
| `VENUE_JUMP_RATE` | `0.08` | BTC jumps (2–5 bp) per second |
| `VENUE_GAMMA_STALL_AFTER_S` | `0` | After this uptime, Gamma replies take `VENUE_GAMMA_STALL_MS` (12 s) |
| `VENUE_PROXY_DROP_EVERY_S` | `0` | Cut every tunnelled connection (both websockets, HTTP/2, REST) this often; reconnection stress |
| `VENUE_REST_TS` | `lastchange` | REST `/book` timestamp: time of last change, or `now` |
| `VENUE_MARKET_KILL_EVERY_S` | `0` | Cut ONE market websocket (the oldest) this often, cleanly and abruptly in turn; a client with a standby should not lose the feed |
| `VENUE_NO_PONG` | `0` | `1`: never answer the text `PING` on the market channel (with a large `VENUE_MM_LAG_MEAN` the link is silent but alive) |
| `VENUE_MARKET_FLAP` | `0` | `1`: close every market websocket 100 ms after it connects; reconnects must back off |
| `VENUE_PROXY_PORT`, `VENUE_PORT_BASE` | `18080`, `19000` | Run several venues at once |
