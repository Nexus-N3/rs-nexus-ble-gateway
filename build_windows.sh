
#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

source "$SCRIPT_DIR/env.windows.sh"

cd "$ZEPHYR_WORKSPACE"

west build -p always \
  -b nrf54l15_connectkit/nrf54l15/cpuapp \
  -s "$GATEWAY/ports/zephyr" \
  -d "$GATEWAY/build" \
  -- \
  -DBOARD_ROOT="$BOARD_ROOT" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
