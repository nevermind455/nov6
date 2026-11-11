# Resting a missed order (`SNIPER_REST_LIVE`)

Live only, off by default. When a FAK matches nothing, the bot can leave the
**same order** on the book for a few seconds instead of giving up: the same
shares at the same price limit, as a post-only order. If somebody sells into it
during that time it fills, with no fee (a resting order is a maker). Otherwise
it is cancelled.

Nothing about the signal, the fair value, the limit or the size changes. This
only adds a second chance after a miss.

## Settings

| Setting | Default | Meaning |
| --- | --- | --- |
| `SNIPER_REST_LIVE` | `0` | `1` turns it on. Needs `SNIPER_ORDER_TYPE=FAK`, `SNIPER_ORDER_SIZING=shares`, and `SNIPER_MISS_RETRY=0` (after a miss the order either rests or is sent again, not both) |
| `SNIPER_REST_S` | `5` | Longest time on the book, in seconds (1 to 30) |
| `SNIPER_REST_DIR` | `1` | `1`: rest only on the side that is beating the price to beat. `0`: rest any missed order |
| `SNIPER_REST_LIVE_STOP_USD` | `10` | Turns the feature off for good once the resting orders' own settled loss reaches this |

Example for `.env.live`:

```bash
SNIPER_REST_LIVE=1
SNIPER_REST_S=8
SNIPER_REST_DIR=1
SNIPER_REST_LIVE_STOP_USD=10
```

## When a missed order rests

All of these must hold when the miss is confirmed. The first one that fails is
written in the `rest_order` row as its `reason`.

1. The order is at least the venue's published minimum (5 shares). A FAK can
   be smaller; a resting order cannot.
2. The signal is at most 2 seconds old.
3. There is time left: the order must be off the book
   `max(SNIPER_MIN_LEFT_S, MIN_SECONDS_TO_EXPIRY)` seconds before the round
   ends, and must be able to rest at least 1 second.
4. The trigger feed is fresh, and BTC still holds some of the move that
   caused the signal.
5. With `SNIPER_REST_DIR=1`: the side is ahead of the price to beat by more
   than the estimate's own error, and the estimate has been checked against
   Polymarket's published value on `SNIPER_PTB_MIN_CHECKS` rounds. This needs
   about an hour of running after a start (see below).
6. The best offer is above the limit (otherwise a post-only order would be
   refused by the venue).
7. Every check a normal live order passes: connections ready, fresh balance,
   collateral, allowance, round caps and the canary budgets. A resting order
   counts as an order, takes a fill slot and reserves its stake, exactly as a
   FAK does; an unfilled one gives its slot and spend back.

## When it is cancelled

Whichever comes first:

* it rested `SNIPER_REST_S` seconds, or the round's end is near;
* **the trigger fired the other way** (BTC moved against the order by the
  trigger size within the trigger window);
* **the move was given back** (BTC is back at the price before the signal);
* with `SNIPER_REST_DIR=1`, the side is no longer ahead of the price to beat;
* the trigger feed stopped being fresh;
* new orders are paused, the stop file exists, or the bot is shutting down.

Price checks run on every Coinbase tick, so a cancel leaves in the same
moment the tick is read. A cancel is asked again until the venue shows the
order gone: three quick tries, then every 5 seconds with new orders paused and
an `error` row. A shutdown waits (up to 20 seconds) for the venue to confirm.

## While it rests

The market takes no other order from the bot (skip reason `our resting order
is in this market`, code `RISK_RESTING_ORDER`). This lasts at most
`SNIPER_REST_S` seconds per miss and keeps the bot from trading against its
own order.

## Accounting

* A fill arrives as a trade in which the bot's order is a maker leg. It is
  booked at the order's own price with a zero fee, under the trade id
  `<trade id>:<order id>`. Journal rows `live_fill_update` and `live_fill`
  carry `"rest": true`.
* The final size comes from the venue: the private channel's cancellation,
  the order's own record, or its trades. Until then the order stays pending,
  reserves its stake and blocks settlement of its round, like a FAK whose size
  is unknown.
* The ledger keeps `rest_realized`: the settled profit and loss of resting
  orders only. The loss stop reads its change since the test began, from
  `state/live/rest_live_test.json`. Delete that file to begin a new test.
* Resting orders are not counted among the FAK orders in the summary's
  `orders`, or in the fill rate and latency figures of `sniper-miss-report`.
  That report has its own `resting_orders` section.

## Journal rows

| `result` | Meaning |
| --- | --- |
| `rest_live` | At start: whether the feature is on, and why not |
| `rest_order` | One per confirmed miss. `outcome`: `resting`, `skipped` (a rule above), `refused` (a live gate), `not resting` (the venue refused it), `unknown` |
| `rest_cancel` | A cancel was sent: `why`, `try`, `rested_ms` |
| `rest_cancel_reply` | The venue's answer to it |
| `rest_done` | The end: `filled`, `shares`, `cancel_why`, `source` of the final size |
| `rest_live_off` | The loss stop was reached, or its record is unreadable |

Counters in the summary row: `rest orders sent`, `rest orders resting`,
`rest orders filled`, `rest orders unfilled`, `rest skipped`, `rest refused`,
`rest not resting`.

## What to know before turning it on

* **The price-to-beat estimate must be working.** With `SNIPER_REST_DIR=1`
  nothing rests until the estimate has been checked on
  `SNIPER_PTB_MIN_CHECKS` (6) published rounds, after 3 rounds that only set
  its baseline: about an hour after each start. If Polymarket's event data
  does not carry `eventMetadata.priceToBeat`, it never is, and every
  `rest_order` row will say `estimate checked ... on 0 of 6 rounds`. Check:

  ```bash
  grep -c '"result":"ptb_official"' state/live/sniper.jsonl
  ```

  A count that grows by one each round means it works.
* **A crash leaves the order on the book.** If the process dies while an order
  rests, nothing cancels it until the bot starts again (it cancels inherited
  resting orders first thing) or the round ends. The exposure is one order's
  stake.
* **Fills are selected against the bot.** A resting buy is filled by someone
  who wants to sell at that price at that moment. The early cancel is what
  protects it; the loss stop bounds the test if that is not enough. Judge it
  on its own settled result (`rest_realized`), not on the fill rate.
* Not ported from the reference: `SNIPER_MAKER_ONLY`, `SNIPER_MAKER_SPLIT`.
  The direction rule and the early cancel are additions to the reference's
  `SNIPER_REST_LIVE`, which rests any miss for the full time.

## Tested with

Unit tests (`tests/rest_tests.cpp`): the post-only body, the cancel request
and its signature against the real SDK's bytes (`tests/fixtures/rest_vectors.json`);
the rest and cancel rules; the venue's replies; maker fills; ledger accounting
across a restart; the loss stop. End to end against `tools/fake-venue` with
`VENUE_LIVE=1`: see that folder's README. Not tested against the real venue.
