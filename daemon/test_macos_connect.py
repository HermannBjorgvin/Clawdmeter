#!/usr/bin/env python3
"""Quick end-to-end test of the macOS connected-peripheral path.

Discovers the HID-held 'Clawdmeter', connects without scanning,
finds the custom GATT characteristics, and writes one test payload.
Run from Terminal.app (which has Bluetooth permission):

    cd daemon && ./.venv/bin/python ./test_macos_connect.py
"""
import asyncio

from bleak import BleakClient

import claude_usage_daemon as d


async def main() -> None:
    d.log("Discovering target via macOS connected-peripheral path...")
    target = await d.discover_target()
    if not target:
        d.log("FAIL: no target found (device powered on and showing splash?)")
        return

    display = target if isinstance(target, str) else f"{target.name} [{target.address}]"
    d.log(f"Target: {display}")

    client = BleakClient(target)
    d.log("Connecting (should NOT scan)...")
    await client.connect()
    if not client.is_connected:
        d.log("FAIL: connected=False")
        return
    d.log("Connected. Enumerating services...")

    # List services/chars so we can confirm the custom service is reachable.
    found_rx = False
    for service in client.services:
        for ch in service.characteristics:
            if ch.uuid.lower() == d.RX_CHAR_UUID.lower():
                found_rx = True
    d.log(f"RX characteristic present: {found_rx}")

    if found_rx:
        payload = '{"s":42,"sr":120,"w":17,"wr":4320,"st":"ok_test","ok":true}'
        d.log(f"Writing test payload: {payload}")
        await client.write_gatt_char(d.RX_CHAR_UUID, payload.encode(), response=False)
        d.log("PASS: wrote test payload — check the device screen.")
    else:
        d.log("FAIL: custom RX characteristic not found on the peripheral.")

    await client.disconnect()
    d.log("Disconnected. Done.")


if __name__ == "__main__":
    asyncio.run(main())
