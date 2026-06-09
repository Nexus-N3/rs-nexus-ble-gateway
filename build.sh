#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

cd "$ZEPHYR_WORKSPACE"

# Nordic DK target, not used:
# nrf54l15dk/nrf54l15/cpuapp

west build -p always \
  -b nrf54l15_connectkit/nrf54l15/cpuapp \
  -s "$GATEWAY/ports/zephyr" \
  -d "$GATEWAY/build" \
  -- \
  -DBOARD_ROOT="$HOME/Desktop/nrf54l15-connectkit" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON