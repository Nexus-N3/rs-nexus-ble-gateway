#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

# can only run west commands from the zephyr workspace.
cd "$ZEPHYR_WORKSPACE"

# uses the build directory that is in the application project 
west flash --build-dir "$GATEWAY/build"