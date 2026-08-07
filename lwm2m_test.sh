#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de nenhum
# repositório.
# Build and flash the LwM2M client (Wi-Fi) on esp32_devkitc_wroom/esp32/procpu
# Usage: ./lwm2m_test.sh [build|flash|all]
# Default: all

set -e

source ~/zephyr-env/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.8
cd ~

BOARD=esp32_devkitc_wroom/esp32/procpu
WINEGRID_DIR=~/Desktop/winegrid
STEP=${1:-all}

build() {
    echo "==> Building lwm2m_client"
    west build -b "$BOARD" -d ~/build_lwm2m \
        "$WINEGRID_DIR/lwm2m" \
        -- -DEXTRA_CONF_FILE="overlay-wifi.conf;overlay-ota.conf" -DDTC_OVERLAY_FILE="$WINEGRID_DIR/lwm2m/esp32_wifi.overlay"
}

flash() {
    # NOTE: MCUboot itself must already be flashed - it's board-level, shared
    # with the smp_svr PoC, so run `./esp32_ota_test.sh mcuboot` once first
    # if this board has never had it flashed.
    echo "==> Flashing lwm2m_client (signed image)"
    west flash -d ~/build_lwm2m --bin-file ~/build_lwm2m/zephyr/zephyr.signed.bin
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
