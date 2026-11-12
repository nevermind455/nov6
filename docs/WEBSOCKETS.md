# Holding the venue's websockets

The bot never closes a venue websocket on purpose while it runs. Rounds change
by subscribing and unsubscribing on the same connection. What could still take
a connection down is the venue, the network, or the bot deciding a link is dead.
This is what holds it, and what happens when one goes anyway.

## What holds a connection

* **Keep-alive pings.** Besides the text `PING` every 10 seconds, the socket
  itself sends a WebSocket ping when nothing has arrived for half the silence
  limit (25 s), and a pong counts as life. A quiet link that still answers is
  kept. Before this, 25 seconds without a text reply closed the link even if it
  was healthy. Only a link that answers nothing at all is given up.
* **A fault of the bot's own is not a reason to disconnect.** An error while
  handling a market message is reported (and pauses new live orders); the
  socket stays up.
* **Reconnecting.** A connection that was up and dropped is reopened at once.
  A link that does not hold (dropped within 5 seconds of connecting) backs off
  0.5 s, 1 s, 2 s, 4 s, 8 s, so a venue in trouble is not hammered.

## The standby (`SNIPER_MARKET_STANDBY`, default `1`)

The market feed has two connections. One is live: its messages build the
books. The other is a warm standby: connected, subscribed to the same tokens,
read and otherwise ignored. When the live one drops, the standby is live in
the same instant, asks the venue for fresh book snapshots, and the lost
connection reconnects behind it as the new standby. The books are rebuilt from
those snapshots (the standby's earlier messages were not applied, so nothing
can be applied twice or out of order).

Measured on `tools/fake-venue` with one market socket cut every 10 seconds:
feed down 0 ms per cut with the standby, about 267 ms per cut without (on the
real network a reconnect takes longer than that).

If both connections go in the same moment (a venue restart, the network), the
feed is down until the first one is back; that cannot be prevented from this
side. `SNIPER_MARKET_STANDBY=0` goes back to a single connection.

The private channel and the Coinbase sockets keep one connection each (Coinbase
already runs two redundant ones). A resting order is cancelled when the private
channel drops (see RESTING_ORDERS.md).

## Seeing what happened

Every socket that comes up or goes down writes a journal row:

```json
{"result":"ws","socket":"market","n":1,"up":false,
 "role":"live: the standby took over","reason":"closed by the venue: ...",
 "left_s":6.8,"wall":1794400000.1}
```

* `socket`: `market`, `user` (the private channel) or `coinbase`; `n` is which
  connection.
* `role`: `live`, `standby`, `live: the standby took over`,
  `live: no standby to take over`, `private`, `trigger`.
* `reason` (when it went down): `closed by the venue`, `read: ...`,
  `write: ...`, `no answer to keep-alive pings`, `silent for 25 s`,
  `connection setup timed out`, and so on.
* `left_s`: seconds left in the round at that moment.

```bash
grep '"result":"ws"' state/live/sniper.jsonl | grep '"up":false' | tail -20
```

`sniper-miss-report` has a `sockets` section (counts and reasons), and a live
socket going down counts as a disruption for the misses right after it. With
`--dash`, HEALTH shows `POLY WS`: connections up, how long the feed has held
without a gap, drops, switches to the standby, and outages (both down).
`POLY BOOK` beside it is a different thing: how fresh the books of the current
round are. A book can read `STALE` in a round's last seconds, when the venue
stops changing it, with every connection up.
