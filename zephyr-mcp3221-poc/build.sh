#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de
# nenhum repositório.
# Compila (só build - sem sensor físico não há o que testar em runtime) a
# prova de conceito do driver out-of-tree MCP3221 para o
# esp32_devkitc_wroom/esp32/procpu.
# Usage: ./build.sh

set -e

source ~/zephyr-env/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.8
cd ~

BOARD=esp32_devkitc_wroom/esp32/procpu
POC_DIR=~/Desktop/winegrid/zephyr-mcp3221-poc

west build -b "$BOARD" -d ~/build_mcp3221_poc \
    "$POC_DIR/app" \
    -- -DZEPHYR_EXTRA_MODULES="$POC_DIR/module" \
    -DDTC_OVERLAY_FILE="$POC_DIR/app/esp32_i2c.overlay"