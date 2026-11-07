#!/usr/bin/env bash
# Offline launcher checks: a fake executable records arguments and sends no orders.
# Run on the supported Ubuntu 24.04 host, as in the GitHub Actions workflow.
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
mkdir -p "$fixture/scripts" "$fixture/bin" "$fixture/fake-bin" "$fixture/state/paper"
cp "$sniper_root/scripts/run-live.sh" "$sniper_root/scripts/live-preflight.sh" "$fixture/scripts/"
printf 'SNIPER_CANARY_MAX_LOSS_USD=5\nSNIPER_CANARY_HARD_STOP=1\n' > "$fixture/.env.live"
chmod 600 "$fixture/.env.live"
cat > "$fixture/bin/sniper-cpp" <<'STUB'
#!/usr/bin/env bash
case ${1:-} in
  --self-test) exit 0 ;;
  --help) printf '%s\n' --dry-run; exit 0 ;;
esac
for argument in "$@"; do
  if [[ $argument == --check ]]; then
    loss=${SNIPER_CANARY_MAX_LOSS_USD:-$(sed -n 's/^SNIPER_CANARY_MAX_LOSS_USD=//p' .env.live)}
    hard=${SNIPER_CANARY_HARD_STOP:-$(sed -n 's/^SNIPER_CANARY_HARD_STOP=//p' .env.live)}
    [[ $hard != 0 ]] || hard=false
    [[ $hard != 1 ]] || hard=true
    jq -n --argjson loss "$loss" --argjson hard "$hard" \
      --arg state "$PWD/state/live" --arg stop "${SNIPER_CANARY_STOP_FILE:-$PWD/STOP_LIVE}" \
      --arg paper "${PAPER_AUDIT_PATH:-$PWD/state/paper/sniper.jsonl}" \
      '{live_canary:{max_loss_usd:$loss, hard_stop:$hard}, paths:{state_dir:$state,stop_file:$stop,paper_journal:$paper}}'
    exit
  fi
done
printf '%s\n' "$@" > "$SNIPER_TEST_ARGUMENTS"
STUB
cat > "$fixture/fake-bin/timedatectl" <<'STUB'
#!/usr/bin/env bash
printf 'yes\n'
STUB
cat > "$fixture/fake-bin/curl" <<'STUB'
#!/usr/bin/env bash
[[ ${SNIPER_TEST_CURL_FAIL:-0} != 1 ]] || exit 1
[[ -z ${SNIPER_TEST_CURL_BODY:-} ]] || { printf '%s\n' "$SNIPER_TEST_CURL_BODY"; exit 0; }
date +%s
STUB
cat > "$fixture/fake-bin/df" <<'STUB'
#!/usr/bin/env bash
printf 'Avail\n2097152\n'
STUB
cat > "$fixture/fake-bin/uname" <<'STUB'
#!/usr/bin/env bash
printf 'x86_64\n'
STUB
chmod +x "$fixture/bin/sniper-cpp" "$fixture/fake-bin/"*
export PATH="$fixture/fake-bin:$PATH"
export SNIPER_LIVE_CANARY=1 POLY_PRIVATE_KEY=offline-test-placeholder
export SNIPER_TEST_ARGUMENTS="$fixture/arguments"
unset SNIPER_TEST_CURL_FAIL SNIPER_TEST_CURL_BODY SNIPER_CANARY_MAX_LOSS_USD SNIPER_CANARY_HARD_STOP SNIPER_CANARY_STOP_FILE PAPER_AUDIT_PATH

run() {
  local expected=$1 actual=0
  shift
  rm -f -- "$SNIPER_TEST_ARGUMENTS"
  bash "$fixture/scripts/run-live.sh" "$@" > "$fixture/output" 2>&1 || actual=$?
  if [[ $actual != "$expected" ]]; then
    cat "$fixture/output"
    printf 'Expected exit %s, got %s\n' "$expected" "$actual" >&2
    exit 1
  fi
}
expect_arguments() {
  diff -u <(printf '%s\n' --config "$fixture/.env.live" --live "$@") "$SNIPER_TEST_ARGUMENTS"
}

# No paper history blocks the default command before the executable can start.
run 1 --new-canary-session --dash
grep -qF 'finish the 24-hour paper run first' "$fixture/output"
[[ ! -f $SNIPER_TEST_ARGUMENTS ]]

# The launcher-only flag is consumed; other arguments keep their order.
run 0 --skip-paper-check
expect_arguments
run 0 --skip-paper-check --new-canary-session --dash
grep -qF '24-hour paper requirement skipped' "$fixture/output"
expect_arguments --new-canary-session --dash

# Dry-run behavior and both flag orders remain supported.
run 0 --dry-run --run-for 300
expect_arguments --dry-run --run-for 300
run 0 --dry-run --run-for 300 --skip-paper-check
expect_arguments --dry-run --run-for 300
run 0 --skip-paper-check --dry-run --run-for 300
expect_arguments --dry-run --run-for 300

# Skipping paper must not override stop files, budgets or API failures.
touch "$fixture/STOP_LIVE"
run 1 --skip-paper-check
grep -qF 'STOP_LIVE exists' "$fixture/output"
[[ ! -f $SNIPER_TEST_ARGUMENTS ]]
rm -- "$fixture/STOP_LIVE"
sed -i 's/MAX_LOSS_USD=5/MAX_LOSS_USD=20/' "$fixture/.env.live"
run 1 --skip-paper-check
grep -qF 'above $5' "$fixture/output"
[[ ! -f $SNIPER_TEST_ARGUMENTS ]]
sed -i 's/MAX_LOSS_USD=20/MAX_LOSS_USD=5/' "$fixture/.env.live"
export SNIPER_TEST_CURL_FAIL=1
run 1 --skip-paper-check
grep -qF 'could not reach the order API' "$fixture/output"
[[ ! -f $SNIPER_TEST_ARGUMENTS ]]
unset SNIPER_TEST_CURL_FAIL

# Exported settings override file settings in the bot and in preflight.
export SNIPER_CANARY_MAX_LOSS_USD=20
run 1 --skip-paper-check
grep -qF 'above $5' "$fixture/output"
[[ ! -f $SNIPER_TEST_ARGUMENTS ]]
export SNIPER_CANARY_MAX_LOSS_USD=5
sed -i 's/MAX_LOSS_USD=5/MAX_LOSS_USD=20/' "$fixture/.env.live"
run 0 --skip-paper-check
expect_arguments
unset SNIPER_CANARY_MAX_LOSS_USD
sed -i 's/MAX_LOSS_USD=20/MAX_LOSS_USD=5/' "$fixture/.env.live"
export SNIPER_CANARY_HARD_STOP=0
run 0 --skip-paper-check
grep -qF 'a reached budget grows' "$fixture/output"
unset SNIPER_CANARY_HARD_STOP

# The selected stop path must be checked, and argv cannot change checked config.
export SNIPER_CANARY_STOP_FILE="$fixture/OTHER_STOP"
touch "$SNIPER_CANARY_STOP_FILE"
run 1 --skip-paper-check
grep -qF 'STOP_LIVE exists' "$fixture/output"
rm -- "$SNIPER_CANARY_STOP_FILE"
unset SNIPER_CANARY_STOP_FILE
run 2 --skip-paper-check --config /tmp/unchecked.env
grep -qF 'would bypass preflight' "$fixture/output"
run 2 --skip-paper-check --paper
grep -qF 'would bypass preflight' "$fixture/output"

# Digits inside error JSON are not a valid venue-clock response.
export SNIPER_TEST_CURL_BODY='{"error":500,"timestamp":1700000000}'
run 1 --skip-paper-check
grep -qF 'could not reach the order API' "$fixture/output"
unset SNIPER_TEST_CURL_BODY

# Separate short runs must not add up to a 24-hour unbroken run.
awk 'BEGIN { for (r = 0; r < 24; ++r) {
  for (i = 0; i <= 60; ++i) printf "{\"result\":\"summary\",\"wall\":%d}\n", 1700000000 + r * 3601 + i * 60
  printf "{\"result\":\"summary\",\"final\":true,\"wall\":%d}\n", 1700000000 + r * 3601 + 3600
} }' > "$fixture/state/paper/sniper.jsonl"
run 1
grep -qF 'finish the 24-hour paper run first' "$fixture/output"
[[ ! -f $SNIPER_TEST_ARGUMENTS ]]

# Completing the paper history continues to allow the default command.
awk 'BEGIN { for (i = 0; i <= 1440; ++i) printf "{\"result\":\"summary\",\"wall\":%d}\n", 1700000000 + i * 60 }' > "$fixture/state/paper/sniper.jsonl"
run 0 --dash
expect_arguments --dash

# The real preparation script must satisfy the first-session protection and
# retain the repository's higher budget once the installation has prior state.
cp "$sniper_root/scripts/prepare-live.sh" "$fixture/scripts/"
printf 'SNIPER_SLIP=0.02\n' > "$fixture/.env"
rm "$fixture/.env.live"
bash "$fixture/scripts/prepare-live.sh" > /dev/null
grep -qx 'SNIPER_CANARY_MAX_LOSS_USD=5' "$fixture/.env.live"
grep -qx 'SNIPER_KELLY=1' "$fixture/.env.live"
run 0
mkdir -p "$fixture/state/live"
printf '{}\n' > "$fixture/state/live/live_canary_state.json"
bash "$fixture/scripts/prepare-live.sh" > /dev/null
grep -qx 'SNIPER_CANARY_MAX_LOSS_USD=100' "$fixture/.env.live"
run 0
printf 'Live launcher checks passed (no real network or orders).\n'
