#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

cd "$ZEPHYR_WORKSPACE"

west build -p always \
  -b nrf54l15dk/nrf54l15/cpuapp \
  -s "$GATEWAY/ports/zephyr" \
  -d "$GATEWAY/build-nordic-dk" \
  -- \
  -DDTC_OVERLAY_FILE="$GATEWAY/ports/zephyr/nrf54l15dk.overlay" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON