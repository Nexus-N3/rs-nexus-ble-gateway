#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/env.windows.sh"

cd "$ZEPHYR_WORKSPACE"

west flash \
  --runner pyocd \
  --build-dir "$(cygpath -w "$GATEWAY/build")" \
  --no-erase