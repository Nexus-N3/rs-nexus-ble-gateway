#!/usr/bin/env bash

export GATEWAY="$HOME/Desktop/apps/dev/rs-nexus-project/rs-nexus-ble-gateway"
export ZEPHYR_WORKSPACE="$HOME/Desktop/zephyr-main"
export ZEPHYR_BASE="$ZEPHYR_WORKSPACE/zephyr"
export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"

# Needed for nrfutil if installed in ~/.local/bin.
export PATH="$PATH:$HOME/.local/bin"

source "$ZEPHYR_WORKSPACE/.venv/bin/activate"

echo "Using $(python --version)"
echo "python: $(which python)"
echo "west: $(which west)"
echo "nrfutil: $(which nrfutil 2>/dev/null || true)"
echo "ZEPHYR_BASE=$ZEPHYR_BASE"
