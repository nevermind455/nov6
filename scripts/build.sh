#!/usr/bin/env bash
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sniper_build=${SNIPER_BUILD_DIR:-"$sniper_root/build"}
cmake -S "$sniper_root" -B "$sniper_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSNIPER_TESTS=ON "$@"
cmake --build "$sniper_build" --parallel "${SNIPER_BUILD_JOBS:-1}"
ctest --test-dir "$sniper_build" --output-on-failure
mkdir -p "$sniper_root/bin" "$sniper_root/lib"
cmake --install "$sniper_build" --prefix "$sniper_root/bin" --strip
sniper_simdjson=$(ldd "$sniper_build/sniper-cpp" | awk '/libsimdjson.so.19 =>/ {print $3}')
if [[ -z "$sniper_simdjson" || ! -f "$sniper_simdjson" ]]; then
  printf '%s\n' 'Could not locate the required simdjson 3.6.4 shared library.' >&2
  exit 1
fi
cp -L "$sniper_simdjson" "$sniper_root/lib/libsimdjson.so.19"
"$sniper_root/bin/sniper-cpp" --self-test
