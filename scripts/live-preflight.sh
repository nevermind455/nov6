#!/usr/bin/env bash
# Read-only checks before a live run. Sends no order and prints no secret.
# With --dry-run, a missing 24-hour paper run is a warning, because a dry run
# sends nothing. --skip-paper-check explicitly waives that requirement.
set -u
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$sniper_root" || exit 1
dry=0 skip_paper=0
for argument in "$@"; do
  case "$argument" in
    --dry-run) dry=1 ;;
    --skip-paper-check) skip_paper=1 ;;
    *) printf 'Unknown preflight option: %s\n' "$argument" >&2; exit 2 ;;
  esac
done
fails=0 warns=0
ok() { printf 'ok    %s\n' "$*"; }
warn() {
  printf 'WARN  %s\n' "$*"
  warns=$((warns + 1))
}
fail() {
  printf 'FAIL  %s\n' "$*"
  fails=$((fails + 1))
}
setting() { sed -n "s/^$1=//p" .env.live 2>/dev/null | tail -n 1; }

# ---- this machine
os=$(. /etc/os-release 2>/dev/null && printf '%s' "${VERSION_ID:-unknown}")
if [[ $os == 24.04 && $(uname -m) == x86_64 ]]; then
  ok "Ubuntu 24.04 x86-64"
else
  fail "needs Ubuntu 24.04 x86-64, found ${os} $(uname -m)"
fi
if ./bin/sniper-cpp --self-test >/dev/null 2>&1; then
  ok "bot self-test passed"
else
  fail "bot self-test failed"
fi
if ./bin/sniper-cpp --help 2>/dev/null | grep -q -- --dry-run; then
  ok "programs include the live fixes"
else
  fail "bin/ holds an older build: update it first (git pull)"
fi
if [[ $(timedatectl show -p NTPSynchronized --value 2>/dev/null) == yes ]]; then
  ok "system clock is synchronized"
else
  fail "system clock is not synchronized (timedatectl set-ntp true)"
fi
free_kb=$(df --output=avail -k . 2>/dev/null | tail -n 1 | tr -d ' ')
if [[ ${free_kb:-0} -ge 1048576 ]]; then
  ok "at least 1 GB of disk free"
else
  warn "under 1 GB of disk free"
fi

# ---- settings. The check below reads the key's presence, and never prints it.
if [[ ! -f .env.live ]]; then
  fail ".env.live is missing: run bash scripts/prepare-live.sh"
elif [[ $(stat -c %a .env.live) != 600 ]]; then
  fail ".env.live must have mode 600: chmod 600 .env.live"
else
  ok ".env.live exists with mode 600"
  if [[ ${SNIPER_LIVE_CANARY:-0} != 1 ]]; then
    fail "SNIPER_LIVE_CANARY=1 is not exported"
  elif [[ -z ${POLY_PRIVATE_KEY:-} ]]; then
    fail "POLY_PRIVATE_KEY is not exported (read it with: read -rsp 'Key: ' POLY_PRIVATE_KEY; export POLY_PRIVATE_KEY)"
  elif answer=$(./bin/sniper-cpp --config .env.live --live --check 2>&1 >/dev/null); then
    ok "live settings accepted"
  else
    fail "live settings refused: ${answer}"
  fi
  loss=$(setting SNIPER_CANARY_MAX_LOSS_USD)
  if [[ ! -e state/live/live_canary_state.json ]] &&
    ! awk -v v="${loss:-999}" 'BEGIN { exit !(v + 0 <= 5) }'; then
    fail "first live session: the loss budget is \$${loss:-unset}, above \$5 (run bash scripts/prepare-live.sh)"
  else
    ok "loss budget is \$${loss:-unset}"
  fi
  if [[ $(setting SNIPER_CANARY_HARD_STOP) == 0 ]]; then
    warn "SNIPER_CANARY_HARD_STOP=0: a reached budget grows after each pause"
  fi
fi
if [[ -e STOP_LIVE ]]; then
  fail "STOP_LIVE exists, which blocks new orders: rm STOP_LIVE when you are ready"
fi

# ---- the venue, from this server
server=$(curl -sS --max-time 10 https://clob.polymarket.com/time 2>/dev/null | tr -cd '0-9')
if [[ -n $server ]]; then
  skew=$(($(date +%s) - server))
  if ((skew >= -2 && skew <= 2)); then
    ok "venue clock within 2 seconds of this server"
  else
    fail "venue clock is ${skew}s from this server"
  fi
else
  fail "could not reach the order API"
fi

# ---- the 24-hour paper run (required unless explicitly skipped)
# The longest unbroken stretch of the once-a-minute summaries, so short runs days
# apart do not add up to 24 hours.
journal=state/paper/sniper.jsonl
span=0
if [[ -f $journal ]]; then
  span=$(grep '"result":"summary"' "$journal" | grep -o '"wall":[0-9.]*' | cut -d: -f2 |
    awk 'NR > 1 { gap = $1 - prev; run = gap <= 180 ? run + gap : 0; if (run > best) best = run }
         { prev = $1 } END { printf "%d", best }')
fi
span=${span:-0}
hours=$((span / 3600))
if ((span >= 86400 - 300)); then
  ok "longest paper run is ${hours} hours"
elif ((skip_paper)); then
  warn "24-hour paper requirement skipped (--skip-paper-check); longest run is ${hours} hours"
elif ((dry)); then
  warn "longest paper run is ${hours} hours (24 are required before real orders)"
else
  fail "longest paper run is ${hours} hours: finish the 24-hour paper run first"
fi
if [[ -f $journal ]]; then
  bad=$(grep -c '"result":"error"' "$journal")
  if ((bad > 0)); then
    warn "${bad} error rows in the paper journal: read them before going live"
  fi
fi

printf '\n%d failed, %d warnings\n' "$fails" "$warns"
((fails == 0))
