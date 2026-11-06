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
  [[ $argument != --check ]] || exit 0
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
unset SNIPER_TEST_CURL_FAIL

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

# Completing the paper history continues to allow the default command.
awk 'BEGIN { for (i = 0; i <= 1440; ++i) printf "{\"result\":\"summary\",\"wall\":%d}\n", 1700000000 + i * 60 }' > "$fixture/state/paper/sniper.jsonl"
run 0 --dash
expect_arguments --dash
printf 'Live launcher checks passed (10 cases; no real network or orders).\n'
