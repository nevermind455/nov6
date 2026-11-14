#!/usr/bin/env bash
set -euo pipefail
sniper_root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sniper_build=${SNIPER_BUILD_DIR:-"$sniper_root/build"}
cmake -S "$sniper_root" -B "$sniper_build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSNIPER_TESTS=ON "$@"
cmake --build "$sniper_build" --parallel "${SNIPER_BUILD_JOBS:-1}"
ctest --test-dir "$sniper_build" --output-on-failure
mkdir -p "$sniper_root/bin"
cmake --install "$sniper_build" --prefix "$sniper_root/bin" --strip
"$sniper_root/bin/sniper-cpp" --self-test
