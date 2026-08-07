#!/usr/bin/env bash
# Origem: escrito de raiz nesta colaboração (Claude), não copiado de nenhum
# repositório - orquestra os exemplos oficiais MCUboot/smp_svr já presentes
# no projeto (ver os próprios ficheiros para a origem deles).
# Build, flash and test MCUboot + smp_svr (mcumgr OTA) on esp32_devkitc_wroom/esp32/procpu
# Usage: ./esp32_ota_test.sh [mcuboot|app|test|update|all]
# Default: all

set -e

source ~/zephyr-env/bin/activate
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.16.8
cd ~

BOARD=esp32_devkitc_wroom/esp32/procpu
WINEGRID_DIR=~/Desktop/winegrid
STEP=${1:-all}

build_flash_mcuboot() {
    # MCUboot's boot/zephyr app depends on sibling dirs (bootutil/, ext/tinycrypt/, ...)
    # so it's built from the external repo, not vendored into winegrid/.
    echo "==> Building MCUboot"
    west build -b "$BOARD" -d ~/build_mcuboot bootloader/mcuboot/boot/zephyr
    echo "==> Flashing MCUboot"
    west flash -d ~/build_mcuboot
}

build_flash_app() {
    echo "==> Building smp_svr"
    west build -b "$BOARD" -d ~/build_smpsvr \
        "$WINEGRID_DIR/smp_svr" \
        -- -DEXTRA_CONF_FILE=overlay-serial.conf -DDTC_OVERLAY_FILE="$WINEGRID_DIR/esp32_mcumgr.overlay"
    echo "==> Flashing smp_svr (signed image)"
    west flash -d ~/build_smpsvr --bin-file ~/build_smpsvr/zephyr/zephyr.signed.bin
}

test_device() {
    echo "==> Testing echo + image list over serial"
    python3 << 'EOF'
import asyncio
from smpclient import SMPClient
from smpclient.transport.serial import SMPSerialTransport
from smpclient.requests.os_management import EchoWrite
from smpclient.requests.image_management import ImageStatesRead

async def main():
    client = SMPClient(SMPSerialTransport(), '/dev/ttyACM0')
    async with client:
        print(await client.request(EchoWrite(d='hello')))
        print(await client.request(ImageStatesRead()))

asyncio.run(main())
EOF
}

ota_update() {
    echo "==> Rebuilding smp_svr with current source (new image for OTA)"
    west build -b "$BOARD" -d ~/build_smpsvr \
        "$WINEGRID_DIR/smp_svr" \
        -- -DEXTRA_CONF_FILE=overlay-serial.conf -DDTC_OVERLAY_FILE="$WINEGRID_DIR/esp32_mcumgr.overlay"
    echo "==> Pushing OTA update over serial and confirming the swap"
    python3 "$WINEGRID_DIR/ota_update.py"
}

case "$STEP" in
    mcuboot) build_flash_mcuboot ;;
    app) build_flash_app ;;
    test) test_device ;;
    update) ota_update ;;
    all)
        build_flash_mcuboot
        build_flash_app
        test_device
        ;;
    *)
        echo "Uso: $0 [mcuboot|app|test|update|all]"
        exit 1
        ;;
esac
