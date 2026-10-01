#!/usr/bin/env bash
set -euo pipefail

test_dir="$(cd "$(dirname "$0")" && pwd)"
gateway_dir="$(cd "$test_dir/../.." && pwd)"
test_binary="${TMPDIR:-/tmp}/nexus-gateway-uart-tx-state-test"

cc \
  -std=gnu11 \
  -Wall \
  -Wextra \
  -ffunction-sections \
  -fdata-sections \
  -I"$test_dir/fakes" \
  -I"$gateway_dir/src/interface" \
  -I"$gateway_dir/src/hardware" \
  "$test_dir/test_gateway_interface_uart_tx.c" \
  -Wl,--gc-sections \
  -o "$test_binary"

"$test_binary"
