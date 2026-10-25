
#!/usr/bin/env bash

export PATH="/c/Program Files/CMake/bin:$PATH"
export GATEWAY="$HOME/nexus-gateway"

export ZEPHYR_WORKSPACE="$HOME/zephyrproject"
export ZEPHYR_BASE="$ZEPHYR_WORKSPACE/zephyr"

export ZEPHYR_SDK_INSTALL_DIR="$HOME/zephyr-sdk-1.0.1"
export ZEPHYR_TOOLCHAIN_VARIANT="zephyr"

export BOARD_ROOT="$HOME/Desktop/nrf54l15-connectkit"

# Windows virtual environment
export PATH="$ZEPHYR_WORKSPACE/.venv/Scripts:$PATH"

echo "Using $(python --version)"
echo "python: $(command -v python)"
echo "west: $(command -v west)"
echo "ZEPHYR_BASE=$ZEPHYR_BASE"
echo "ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR"
echo "BOARD_ROOT=$BOARD_ROOT"
