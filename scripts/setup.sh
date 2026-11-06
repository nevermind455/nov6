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
printf '%s\n' 'Ready for paper evaluation: bash scripts/run-paper.sh'
