#!/usr/bin/env bash
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$sniper_root"
if [[ ! -f .env ]]; then
  printf '%s\n' 'Run bash scripts/setup.sh first.' >&2
  exit 1
fi
exec "$sniper_root/bin/sniper-cpp" --config "$sniper_root/.env" --paper "$@"
