#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de
# nenhum repositório.
# Build (only build - no physical RN2483 to test against) the RN2483
# out-of-tree LoRa driver proof of concept for esp32_devkitc_wroom.
# Usage: ./build.sh

set -e

source ~/zephyr-env/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.8
cd ~

BOARD=esp32_devkitc_wroom/esp32/procpu
POC_DIR=~/Desktop/winegrid/zephyr-rn2483-poc

west build -b "$BOARD" -d ~/build_rn2483_poc \
    "$POC_DIR/app" \
    -- -DZEPHYR_EXTRA_MODULES="$POC_DIR/module" \
    -DDTC_OVERLAY_FILE="$POC_DIR/app/esp32_rn2483.overlay"