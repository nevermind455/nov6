#!/usr/bin/env bash
# Exercise process shutdown after asynchronous audit I/O failure. No wallet.
set -euo pipefail
bot=$1
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
cat > "$test_dir/.env" <<EOF
PAPER_AUDIT_PATH=$test_dir/audit.jsonl
SNIPER_COINBASE_CONNS=1
EOF
chmod 600 "$test_dir/.env"
# Pre-existing valid audit history already reaches the child's file-size limit.
printf '{"padding":"%2048s"}\n' '' > "$test_dir/audit.jsonl"
set +e
(
trap '' XFSZ
ulimit -f 1
HTTPS_PROXY=http://127.0.0.1:1 https_proxy=http://127.0.0.1:1 \
HTTP_PROXY=http://127.0.0.1:1 http_proxy=http://127.0.0.1:1 \
ALL_PROXY=http://127.0.0.1:1 all_proxy=http://127.0.0.1:1 \
NO_PROXY= no_proxy= timeout 30 "$bot" --config "$test_dir/.env" \
  --health --run-for 2 2> "$test_dir/err"
) | cat > "$test_dir/out"
status=${PIPESTATUS[0]}
set -e
if [[ "$status" != 1 ]]; then
  cat "$test_dir/out" "$test_dir/err" >&2
  printf 'expected audit failure exit 1, got %s\n' "$status" >&2
  exit 1
fi
tail -n 1 "$test_dir/out" | awk 'index($0,"\"exit_code\":1") && index($0,"\"orders\":0") && index($0,"\"stopped\":true") { ok=1 } END { exit !ok }'
printf '%s\n' 'Runtime audit failure stopped without orders.'
