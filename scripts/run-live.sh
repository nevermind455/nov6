#!/usr/bin/env bash
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$sniper_root"
if [[ ${SNIPER_LIVE_CANARY:-0} != 1 ]]; then
  printf '%s\n' 'Live requires an explicitly exported SNIPER_LIVE_CANARY=1 and completed acceptance checks. Read README.md.' >&2
  exit 1
fi
exec "$sniper_root/bin/sniper-cpp" --config "$sniper_root/.env" --live "$@"
