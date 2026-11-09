#!/usr/bin/env bash
# The probe uses public GET /time only, with no wallet or trading runtime.
set -euo pipefail
probe=$1
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT

"$probe" --help > "$test_dir/help"
grep -qF 'sniper-net-probe' "$test_dir/help"

# Any accidentally accepted option must fail locally rather than contact a
# public service. Valid loopback probes bypass this proxy by transport policy.
export HTTPS_PROXY=http://127.0.0.1:9 https_proxy=http://127.0.0.1:9
invalid() {
  local status=0
  timeout 5 "$probe" "$@" > "$test_dir/out" 2> "$test_dir/err" || status=$?
  if [[ $status != 2 ]]; then
    cat "$test_dir/out" "$test_dir/err" >&2
    printf 'Expected invalid-option exit 2, got %s\n' "$status" >&2
    exit 1
  fi
}
for value in 0 -1 10001 1junk nan; do invalid --samples "$value"; done
for value in -1 60001 1.5; do invalid --interval-ms "$value"; done
for value in 0 61 1junk; do invalid --timeout-s "$value"; done
invalid --samples
invalid --interval-ms
invalid --timeout-s
invalid --label
invalid --endpoint
invalid --unknown
invalid --endpoint http://127.0.0.1
invalid --endpoint https://127.0.0.1/order
invalid --endpoint 'https://127.0.0.1/time?order=1'
invalid --endpoint 'https://probe-test-user:DO_NOT_PRINT_PASSWORD@127.0.0.1'
if grep -qF DO_NOT_PRINT_PASSWORD "$test_dir/out" "$test_dir/err"; then
  printf '%s\n' 'Invalid URL userinfo appeared in diagnostics.' >&2
  exit 1
fi

# A failed connection must produce an explicit incomplete JSON result, not a
# fabricated successful latency sample. The independent deadline bounds it.
status=0
timeout 5 "$probe" --endpoint https://127.0.0.1:9 --samples 1 \
  --interval-ms 0 --timeout-s 1 --label loopback-unreachable \
  > "$test_dir/out" 2> "$test_dir/err" || status=$?
if [[ $status != 1 ]]; then
  cat "$test_dir/out" "$test_dir/err" >&2
  printf 'Expected connection-failure exit 1, got %s\n' "$status" >&2
  exit 1
fi
grep -Eq '"native_cpp"[[:space:]]*:[[:space:]]*true' "$test_dir/out"
grep -Eq '"success"[[:space:]]*:[[:space:]]*false' "$test_dir/out"
grep -Eq '"samples_completed"[[:space:]]*:[[:space:]]*0' "$test_dir/out"
grep -Eq '"order_capable_calls"[[:space:]]*:[[:space:]]*0' "$test_dir/out"
printf '%s\n' 'Native read-only network probe CLI checks passed.'
