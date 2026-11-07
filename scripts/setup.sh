#!/usr/bin/env bash
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$sniper_root"
umask 077
if [[ ! -e .env ]]; then
  cp .env.example .env
fi
chmod 600 .env
chmod 755 bin/sniper-cpp bin/sniper-tests
./bin/sniper-cpp --self-test
./bin/sniper-cpp --check
for utility in curl jq; do
  if ! command -v "$utility" >/dev/null 2>&1; then
    printf 'Live preflight needs %s; install with: sudo apt-get install curl jq\n' "$utility"
  fi
done
printf '%s\n' 'Ready for paper evaluation: bash scripts/run-paper.sh'
