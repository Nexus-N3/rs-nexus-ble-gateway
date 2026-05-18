#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "$0")/env.sh"

cd "$ZEPHYR_WORKSPACE"

west flash --build-dir "$GATEWAY/build"