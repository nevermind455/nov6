# Going live

This bot has never sent a real order. Its live path has only been tested against
a local stand-in for the venue, with dummy keys. The first real orders are the
test, so start small, stay at the screen, and use money you can afford to lose.

Run every command below on the VPS, from `~/sniper`.

## 1. Before anything else

- **Check you are allowed to trade.** Polymarket restricts some countries, and
  the rules depend on where you are, not only on where the server is. A server
  abroad does not make trading allowed if it is restricted for you, and this
  bot must not be used to get around that.
- **Use a separate wallet** with only what this test needs. Never use a wallet
  that holds anything else. Its key must never go in a file, a chat or a repo.

## 2. Finish the 24-hour paper run

```bash
bash scripts/run-paper.sh --dash --run-for 86400
```

Read the results before moving on: `errors` should be 0 and the numbers should
look sensible. By default, the preflight refuses real orders until the journal
covers 24 hours. To deliberately skip only that requirement, add
`--skip-paper-check` to the live command:

```bash
bash scripts/run-live.sh --skip-paper-check --new-canary-session --dash
```

The other preflight checks and live trading limits still apply. The flag is
handled by the shell launcher, so it does not require rebuilding the executable.

## 3. Prepare the first-run settings

```bash
bash scripts/prepare-live.sh
```

This writes `.env.live`, leaving `.env` alone: quarter-Kelly orders sized from
the wallet (up to 200 shares per outcome), a $5 first-session loss budget
($100 when a previous canary state already exists), and a limit of 10000 orders, 10000 fills, 10000 rounds and
$100 spent for the session. When a budget is reached, new orders stop until you deliberately
start a new session. Your limit of ask + 0.02 carries over from `.env`.

## 4. Put your key in this session only

This hides what you type. The key lasts until you close this terminal.

```bash
read -rsp 'Wallet private key: ' POLY_PRIVATE_KEY; echo; export POLY_PRIVATE_KEY
```

```bash
export SNIPER_LIVE_CANARY=1 POLY_SIGNATURE_TYPE=0
```

Signature type 0 is a wallet that holds the funds itself. For a Polymarket account
wallet, use type 1 (proxy) or 2 (Safe), and also `export POLY_FUNDER=<its address>`.

## 5. Dry run: every live check, no orders

```bash
bash scripts/run-live.sh --dry-run --new-canary-session --run-for 300
```

It logs in, checks your account, signs orders and sends none. It should print a
`dry_run_ready` line showing your wallet, its collateral and its allowances, and
`dry_run_orders` above 0 in the final line.

- If an allowance is 0 or missing, the bot will refuse every order. Enable trading
  for that wallet on polymarket.com, then run this step again.
- If it stops with `FAIL` or a refusal, fix that first.

## 6. The first real orders

Start this inside tmux so it survives a dropped connection, and watch it:

```bash
bash scripts/run-live.sh --new-canary-session --dash
```

To stop new orders at once, from another SSH window:

```bash
touch STOP_LIVE
```

- **Ctrl+C or `touch STOP_LIVE` never closes a position.** Anything open is held
  until the round resolves.
- **Winnings are not automatic.** Winning shares stay in the wallet until you claim
  them on polymarket.com, so cash does not come back on its own.
- **Never restart to get around a pause.** An unknown order outcome pauses new
  orders while the bot looks it up. It is never resent.

## 7. Check it before you go further

Compare the bot with your account on polymarket.com: orders, fills, fees and cash
should all agree. Read `state/live/sniper.jsonl` for anything unexpected. Only then
consider a larger session, by changing `.env.live` and starting again with
`--new-canary-session`. That starts a new budget; it does not reset positions.

## Trades you make yourself

The bot halts with "fill the native bot did not authorize" when the wallet has
a trade it did not place. If it was your own trade, find its `trade_id` in the
error row and add it, one per line, to `state/live/manual_trades.txt`:

```bash
grep '"fill the native bot did not authorize"' state/live/sniper.jsonl | grep -o '"trade_id":"[^"]*"' | cut -d'"' -f4 | sort -u
```

The bot then skips just those trades. Any other unknown trade still halts it,
so do not list one you do not recognise: move the funds to a new wallet.
