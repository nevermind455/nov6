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
stop_file="$sniper_root/STOP_LIVE"
journal="$sniper_root/state/paper/sniper.jsonl"
if command -v jq >/dev/null 2>&1; then
  have_jq=1
else
  have_jq=0
  fail "jq is required for preflight: install it with sudo apt-get install jq"
fi

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
  else
    mode=(--live)
    ((dry == 0)) || mode+=(--dry-run)
    if answer=$(./bin/sniper-cpp --config .env.live "${mode[@]}" --check 2>&1); then
      if ((have_jq)) &&
        jq -e '.live_canary.max_loss_usd | type == "number"' <<< "$answer" >/dev/null &&
        jq -e '.live_canary.hard_stop | type == "boolean"' <<< "$answer" >/dev/null &&
        jq -e '.paths | [.state_dir, .stop_file, .paper_journal] | all(type == "string" and length > 0)' <<< "$answer" >/dev/null; then
        ok "live settings accepted"
        loss=$(jq -r '.live_canary.max_loss_usd' <<< "$answer")
        state_dir=$(jq -r '.paths.state_dir' <<< "$answer")
        stop_file=$(jq -r '.paths.stop_file' <<< "$answer")
        journal=$(jq -r '.paths.paper_journal' <<< "$answer")
        if [[ ! -e "$state_dir/live_canary_state.json" ]] &&
          ! jq -e '.live_canary.max_loss_usd <= 5' <<< "$answer" >/dev/null; then
          fail "first live session: the loss budget is \$${loss}, above \$5 (run bash scripts/prepare-live.sh)"
        else
          ok "loss budget is \$${loss}"
        fi
        if jq -e '.live_canary.hard_stop == false' <<< "$answer" >/dev/null; then
          warn "SNIPER_CANARY_HARD_STOP=0: a reached budget grows after each pause"
        fi
      else
        fail "could not read effective live settings; rebuild bin/ and install jq"
      fi
    else
      fail "live settings refused (inspect with ./bin/sniper-cpp --config .env.live --live --check)"
    fi
  fi
fi
if [[ -e STOP_LIVE || -e $stop_file ]]; then
  fail "STOP_LIVE exists ($stop_file), which blocks new orders: remove it only when you are ready"
fi

# ---- the venue, from this server
server=$(curl --fail --silent --show-error --max-time 10 https://clob.polymarket.com/time 2>/dev/null) || server=""
if [[ $server =~ ^[0-9]{9,12}$ ]]; then
  server=$((10#$server))
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
span=0
if [[ -f $journal ]]; then
  span=$(awk '
    /"result"[[:space:]]*:[[:space:]]*"summary"/ {
      if ($0 ~ /"final"[[:space:]]*:[[:space:]]*true/) { run = 0; prev = 0; next }
      if (!match($0, /"wall"[[:space:]]*:[[:space:]]*[0-9]+([.][0-9]+)?/)) { run = 0; prev = 0; next }
      stamp = substr($0, RSTART, RLENGTH); sub(/.*:/, "", stamp); now = stamp + 0
      gap = now - prev
      run = prev > 0 && gap > 0 && gap <= 180 ? run + gap : 0
      if (run > best) best = run
      prev = now
    }
    END { printf "%d", best }' "$journal")
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
