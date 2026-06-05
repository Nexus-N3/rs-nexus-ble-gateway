#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

cd "$ZEPHYR_WORKSPACE"

west build -p always \
  -b nrf54l15dk/nrf54l15/cpuapp \
  "$GATEWAY/ports/zephyr" \
  --build-dir "$GATEWAY/build" \
  -- \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON