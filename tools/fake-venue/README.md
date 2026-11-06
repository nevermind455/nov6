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
npm install
node venue.js &                      # proxy on 127.0.0.1:18080

mkdir -p /tmp/sim && cp ../../.env.example /tmp/sim/.env && chmod 600 /tmp/sim/.env
HTTPS_PROXY=http://127.0.0.1:18080 SSL_CERT_FILE=$PWD/certs/ca.crt \
  ../../bin/sniper-cpp --config /tmp/sim/.env --paper --run-for 600
```

Journal: `/tmp/sim/state/paper/sniper.jsonl`. Never point a live run at it.

Stress knobs (environment variables for `node venue.js`):

| Variable | Default | Effect |
| --- | --- | --- |
| `VENUE_SHUFFLE` | `1` | Shuffle entries inside each `price_change` message (`0` = removals first) |
| `VENUE_MM_LAG_MEAN` | `1.0` | Mean seconds between market-maker requotes |
| `VENUE_JUMP_RATE` | `0.08` | BTC jumps (2–5 bp) per second |
| `VENUE_GAMMA_STALL_AFTER_S` | `0` | After this uptime, Gamma replies take `VENUE_GAMMA_STALL_MS` (12 s) |
| `VENUE_REST_TS` | `lastchange` | REST `/book` timestamp: time of last change, or `now` |
| `VENUE_PROXY_PORT`, `VENUE_PORT_BASE` | `18080`, `19000` | Run several venues at once |
