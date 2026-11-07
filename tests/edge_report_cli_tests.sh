#!/usr/bin/env bash
# Reporting reads only; malformed evidence and invalid options must fail.
set -euo pipefail
report=$1
test_dir=$(mktemp -d)
trap 'rm -rf -- "$test_dir"' EXIT
printf '{"result":"summary"}\n' > "$test_dir/journal.jsonl"
"$report" "$test_dir/journal.jsonl" > "$test_dir/out"
for value in 0 -1 3junk; do
  if "$report" "$test_dir/journal.jsonl" --min-rounds "$value" > /dev/null 2>&1; then
    printf 'invalid minimum accepted: %s\n' "$value" >&2
    exit 1
  fi
done
for companion in --ledger --audit; do
  if "$report" "$test_dir/journal.jsonl" "$companion" "$test_dir/missing" > /dev/null 2>&1; then
    printf 'missing explicit companion accepted: %s\n' "$companion" >&2
    exit 1
  fi
done
printf '{broken\n' > "$test_dir/journal.jsonl"
if "$report" "$test_dir/journal.jsonl" > /dev/null 2>&1; then
  printf '%s\n' 'malformed evidence accepted as a clean report' >&2
  exit 1
fi
printf '%s\n' 'Edge report input and argument checks passed.'
