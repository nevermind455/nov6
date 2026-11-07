#!/usr/bin/env bash
# Creates .env.live, the settings for a first small live run, from .env.
# .env stays as it is, so paper runs keep their own settings. Holds no secrets.
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$sniper_root"
umask 077
if [[ ! -f .env ]]; then
  printf '%s\n' 'Run bash scripts/setup.sh first.' >&2
  exit 1
fi
if [[ -e .env.live ]]; then
  cp .env.live .env.before-live
  printf '%s\n' 'Kept the old .env.live as .env.before-live.'
fi
cp .env .env.live
chmod 600 .env.live
set_value() {
  if grep -q "^$1=" .env.live; then
    sed -i "s/^$1=.*/$1=$2/" .env.live
  else
    printf '%s=%s\n' "$1" "$2" >> .env.live
  fi
}
# Quarter-Kelly orders, sized from the wallet. 100000 is the highest share cap
# the config accepts, so this cap no longer limits an order.
set_value SNIPER_KELLY 1
set_value SNIPER_KELLY_FRACTION 0.25
set_value SNIPER_HYBRID_SIZING 1
set_value SNIPER_FLAT_LIVE 0
set_value SNIPER_SHARES 5
set_value SNIPER_CANARY_MAX_SHARES 100000
# The whole session. The loss budget counts settled losses plus everything open
# or pending, and gives no credit for wins.
set_value SNIPER_CANARY_MAX_LOSS_USD 100
set_value SNIPER_CANARY_MAX_SPEND_USD 100
set_value SNIPER_CANARY_MAX_ORDERS 10000
set_value SNIPER_CANARY_MAX_FILLS 10000
set_value SNIPER_CANARY_MAX_ROUNDS 10000
set_value SNIPER_CANARY_HARD_STOP 1
printf '%s\n' 'Wrote .env.live for a live run: quarter-Kelly orders, $100 loss budget.'
