#!/usr/bin/env bash
# Offline journal and replay I/O checks. No wallet, feeds or order sender.
set -euo pipefail
bot=$1 replay=$2
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
cat > "$test_dir/a.jsonl" <<'EOF'
{"result":"filled","side":"UP","cap":0.5,"fair_limit":0.52,"shares":6}
EOF
cp "$test_dir/a.jsonl" "$test_dir/b.jsonl"
"$bot" --compare "$test_dir/a.jsonl" "$test_dir/b.jsonl" >/dev/null
sed 's/"shares":6/"shares":7/' "$test_dir/a.jsonl" > "$test_dir/b.jsonl"
if "$bot" --compare "$test_dir/a.jsonl" "$test_dir/b.jsonl" >/dev/null; then
  printf '%s\n' 'different filled decisions were accepted' >&2
  exit 1
fi
: > "$test_dir/a.jsonl"
: > "$test_dir/b.jsonl"
if "$bot" --compare "$test_dir/a.jsonl" "$test_dir/b.jsonl" >/dev/null; then
  printf '%s\n' 'empty journals were accepted as parity evidence' >&2
  exit 1
fi
cat > "$test_dir/recording.jsonl" <<'EOF'
{"k":"cbconn","t":0,"w":1900000000,"up":true}
EOF
if "$replay" "$test_dir/recording.jsonl" "$test_dir/missing/out.jsonl" >/dev/null 2>&1; then
  printf '%s\n' 'replay ignored an output open failure' >&2
  exit 1
fi
printf '%s\n' 'Native comparison and replay I/O checks passed.'
cat > "$test_dir/nullable.jsonl" <<'EOF'
{"k":"round","t":0,"w":1900000000,"current":true,"up":"123","down":"456","cond":"test","start":1899999900,"end":1900000200,"rules":{"condition_id":"test","fee_rate":"0.07","fee_exponent":1,"min_order_size":"5","tick_size":"0.01","up_token_id":"123","down_token_id":"456","taker_delay_ms":0,"taker_delay_flagged":true}}
{"k":"rest_req","t":1,"w":1900000001,"token":"123"}
{"k":"rest_resp","t":1.1,"w":1900000001.1,"token":"123","bids":[{"price":"0.4","size":"10"}],"asks":[{"price":"0.5","size":"10"}],"timestamp":"1900000001000","tick":null,"hash":null}
EOF
"$replay" "$test_dir/nullable.jsonl" "$test_dir/nullable-out.jsonl"
"$replay" "$(dirname -- "$0")/fixtures/replay_wall_jump.jsonl" "$test_dir/clock-out.jsonl"
awk 'index($0, "\"wall\":1900000060.4") { found=1 } END { exit !found }' "$test_dir/clock-out.jsonl"
"$replay" "$(dirname -- "$0")/fixtures/replay_rules_refresh.jsonl" "$test_dir/rules-out.jsonl" \
  "$(dirname -- "$0")/fixtures/replay_rules_refresh_config.json"
awk 'index($0,"\"order_shares\":100") && index($0,"\"condition_id\":\"test\"") && index($0,"\"side\":\"UP\"") { found=1 } END { exit !found }' "$test_dir/rules-out.jsonl"
