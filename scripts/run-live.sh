#!/usr/bin/env bash
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$sniper_root"
if [[ ${SNIPER_LIVE_CANARY:-0} != 1 ]]; then
  printf '%s\n' 'Live requires an explicitly exported SNIPER_LIVE_CANARY=1 and completed acceptance checks. Read docs/GO_LIVE.md.' >&2
  exit 1
fi
preflight=()
runtime=()
for argument in "$@"; do
  case "$argument" in
    --skip-paper-check) preflight+=("$argument") ;;
    --dry-run)
      preflight+=("$argument")
      runtime+=("$argument")
      ;;
    *) runtime+=("$argument") ;;
  esac
done
if ! bash "$sniper_root/scripts/live-preflight.sh" "${preflight[@]+"${preflight[@]}"}"; then
  printf '%s\n' 'Live start refused: fix the FAIL lines above.' >&2
  exit 1
fi
exec "$sniper_root/bin/sniper-cpp" --config "$sniper_root/.env.live" --live "${runtime[@]+"${runtime[@]}"}"
