#!/usr/bin/env python3
"""Push a new signed image to the ESP32 over mcumgr/serial.

Usage: python3 ota_update.py [image_path] [serial_port] [--try-test-mode]

Default image_path: ~/build_smpsvr/zephyr/zephyr.signed.bin
Default serial_port: /dev/ttyACM0

The current MCUboot build for this board uses CONFIG_BOOT_UPGRADE_ONLY=y
(Espressif's default for ESP32), which has NO test/revert phase: an uploaded
image is confirmed and applied directly, with no rollback path. So the normal
flow here is upload -> confirm -> reset.

--try-test-mode: attempt the test-boot (confirm=False) step anyway, to
    demonstrate/confirm that it is rejected under upgrade-only mode
    (IMG_MGMT_ERR.IMAGE_SETTING_TEST_TO_ACTIVE_DENIED). Real rollback testing
    requires rebuilding MCUboot with a swap-capable mode, which crashed on
    this board when we tried CONFIG_BOOT_SWAP_USING_SCRATCH=y (see
    notas_estagio.md).
"""
import asyncio
import sys
from pathlib import Path

from smpclient import SMPClient
from smpclient.generics import error, success
from smpclient.requests.image_management import ImageStatesRead, ImageStatesWrite
from smpclient.requests.os_management import EchoWrite, ResetWrite
from smpclient.transport.serial import SMPSerialTransport

TRY_TEST_MODE = "--try-test-mode" in sys.argv
positional = [a for a in sys.argv[1:] if a != "--try-test-mode"]
IMAGE_PATH = Path(positional[0]) if len(positional) > 0 else Path.home() / "build_smpsvr/zephyr/zephyr.signed.bin"
PORT = positional[1] if len(positional) > 1 else "/dev/ttyACM0"
RECONNECT_TIMEOUT_S = 30
RECONNECT_POLL_S = 1


async def connect() -> SMPClient:
    client = SMPClient(SMPSerialTransport(), PORT)
    await client.connect()
    return client


async def wait_for_device() -> SMPClient:
    """Connect and wait until the app actually answers requests.

    Opening the serial port can succeed before the app's mcumgr threads are
    up (e.g. right after a flash or a reset), so this retries a real request
    (not just the port open) until the device responds or we time out.
    """
    deadline = asyncio.get_event_loop().time() + RECONNECT_TIMEOUT_S
    last_error: Exception | None = None
    while asyncio.get_event_loop().time() < deadline:
        try:
            client = await connect()
        except Exception as e:
            last_error = e
            await asyncio.sleep(RECONNECT_POLL_S)
            continue
        try:
            echo = await client.request(EchoWrite(d="ready-check"), timeout_s=2.0)
            if success(echo):
                return client
        except Exception as e:
            last_error = e
        await client.disconnect()
        await asyncio.sleep(RECONNECT_POLL_S)
    raise TimeoutError(f"Device did not respond on {PORT} within {RECONNECT_TIMEOUT_S}s ({last_error=})")


async def get_secondary_slot_hash(client: SMPClient) -> bytes:
    response = await client.request(ImageStatesRead())
    if error(response):
        raise RuntimeError(f"Failed to read image states: {response}")
    for image in response.images:
        if image.slot == 1:
            return image.hash
    raise RuntimeError(f"No image found in slot 1: {response.images}")


async def main() -> None:
    if not IMAGE_PATH.exists():
        raise FileNotFoundError(f"Image not found: {IMAGE_PATH}. Build it first (./esp32_ota_test.sh app).")
    image_bytes = IMAGE_PATH.read_bytes()
    print(f"==> Image: {IMAGE_PATH} ({len(image_bytes)} bytes)")

    client = await wait_for_device()
    try:
        print("==> Uploading image to slot 1 (secondary)")
        async for offset in client.upload(image_bytes, slot=1):
            print(f"\r    {offset}/{len(image_bytes)} bytes", end="", flush=True)
        print()

        image_hash = await get_secondary_slot_hash(client)

        if TRY_TEST_MODE:
            print(f"==> Trying test-boot (confirm=False) on {image_hash.hex()}")
            response = await client.request(ImageStatesWrite(hash=image_hash, confirm=False))
            if error(response):
                print(f"==> Rejected as expected under upgrade-only mode: {response}")
                print("    This board's MCUboot build has no test/revert phase; falling back")
                print("    to a direct confirm (upload -> confirm -> reset).")

        print(f"==> Confirming image {image_hash.hex()} (no test phase in upgrade-only mode)")
        response = await client.request(ImageStatesWrite(hash=image_hash, confirm=True))
        if error(response):
            raise RuntimeError(f"Failed to confirm image: {response}")

        print("==> Resetting device")
        response = await client.request(ResetWrite())
        if error(response):
            raise RuntimeError(f"Reset failed: {response}")
    finally:
        await client.disconnect()

    print("==> Waiting for device to reboot and apply the update")
    await asyncio.sleep(3)
    client = await wait_for_device()
    try:
        echo = await client.request(EchoWrite(d="post-ota-check"))
        if error(echo) or echo.r != "post-ota-check":
            raise RuntimeError(f"Device did not respond correctly after update: {echo}")
        print("==> Device is alive on the new image (echo OK)")

        states = await client.request(ImageStatesRead())
        for image in states.images:
            print(f"    slot={image.slot} active={image.active} confirmed={image.confirmed} permanent={image.permanent}")
    finally:
        await client.disconnect()

    print("==> OTA update complete")


if __name__ == "__main__":
    asyncio.run(main())
