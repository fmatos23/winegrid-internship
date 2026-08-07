#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de
# nenhum repositório.
# Build (and optionally flash) the Zephyr + ThingsBoard PoC for
# esp32_devkitc_wroom/esp32/procpu.
# Usage: ./build.sh [build|flash|all]

set -e

source ~/zephyr-env/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.8
cd ~

BOARD=esp32_devkitc_wroom/esp32/procpu
WINEGRID_DIR=~/Desktop/winegrid
PROJ_DIR="$WINEGRID_DIR/zephyr-thingsboard"
STEP=${1:-all}

build() {
    echo "==> Building zephyr-thingsboard"
    west build -b "$BOARD" -d ~/build_zephyr_tb \
        "$PROJ_DIR" \
        -- -DEXTRA_CONF_FILE="overlay-wifi.conf;overlay-tb.conf" \
           -DDTC_OVERLAY_FILE="$PROJ_DIR/esp32_wifi.overlay"
}

flash() {
    echo "==> Flashing zephyr-thingsboard"
    west flash -d ~/build_zephyr_tb
}

case "$STEP" in
    build) build ;;
    flash) flash ;;
    all)
        build
        flash
        ;;
    *)
        echo "Uso: $0 [build|flash|all]"
        exit 1
        ;;
esac