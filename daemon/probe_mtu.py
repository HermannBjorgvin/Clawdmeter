#!/usr/bin/env python3
"""Measure the negotiated BLE ATT MTU against the Clawdmeter device.

Why this exists: the session-list payload budget (~200 bytes, 5 rows, 14-char
labels) was picked from an assumption, not a measurement. The evidence for it was
indirect -- the existing usage payload is 121 bytes worst case and works on all
three platforms, so ATT_MTU must be at least ~124 -- but that is a floor, not the
actual value. This reports the real number so the budget can be set from fact.

Needs no firmware change and no reflash: MTU negotiation happens at connection
setup and is independent of which characteristics exist.

IMPORTANT: stop the daemon first. It holds the GATT link, and a second central
competing for it will at best give you a misleading number.

    python daemon/probe_mtu.py                  # discover, report MTU
    python daemon/probe_mtu.py --address AA:BB:CC:DD:EE:FF
    python daemon/probe_mtu.py --write-probe    # ALSO find the real write ceiling

`--write-probe` is opt-in because it writes throwaway payloads to the usage
characteristic. Current firmware answers those with a nack and keeps showing the
last good data, so it is harmless -- but it is a write to your live device, so it
does not happen unless you ask.
"""
from __future__ import annotations

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

DEVICE_NAME = "Clawdmeter"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"

# What the budget currently assumes, for comparison against the measurement.
ASSUMED_USABLE = 200
CURRENT_ROWS_BUDGET = 190
USAGE_PAYLOAD_WORST_CASE = 121


async def find_device(address: str | None):
    if address:
        return address

    # On Windows a bonded device stops advertising, so a scan will never find it.
    # The daemon already solves this by reading the MAC out of PnP; reuse that.
    if sys.platform == "win32":
        try:
            sys.path.insert(0, str(__import__("pathlib").Path(__file__).resolve().parent.parent))
            from daemon.claude_usage_daemon_windows import discover_bonded_address
            if found := discover_bonded_address():
                print(f"Bonded address from PnP: {found}")
                return found
        except Exception as e:  # noqa: BLE001 - diagnostic tool, report and fall through
            print(f"PnP lookup unavailable ({e}); falling back to a scan")

    print(f"Scanning for {DEVICE_NAME} (10s)...")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
    if dev is None:
        print(f"Not found. Is it powered on and in range? Pass --address to skip discovery.")
        return None
    print(f"Found {dev.address}")
    return dev


def report(mtu: int | None) -> None:
    print()
    print("=" * 58)
    if mtu is None:
        print("Could not read the negotiated MTU from this backend.")
        print("Fall back to --write-probe for an empirical ceiling.")
        print("=" * 58)
        return

    usable = mtu - 3          # ATT opcode + attribute handle
    print(f"Negotiated ATT_MTU        : {mtu}")
    print(f"Usable per write          : {usable}  (MTU - 3)")
    print(f"Budget currently assumes  : {ASSUMED_USABLE}")
    print()
    if mtu == 23:
        print("!! That is the BLE default -- no MTU negotiation happened.")
        print("   The existing 121-byte usage payload could not work at this MTU,")
        print("   so treat this reading as wrong rather than as bad news.")
    elif usable < USAGE_PAYLOAD_WORST_CASE:
        print(f"!! Below the {USAGE_PAYLOAD_WORST_CASE}-byte usage payload, which is known to work.")
        print("   The reading is suspect; cross-check with --write-probe.")
    elif usable < CURRENT_ROWS_BUDGET:
        print(f"!! SMALLER than the {CURRENT_ROWS_BUDGET}-byte session-row guard.")
        print("   Lower BLE_ROWS_BUDGET_BYTES in daemon/hook_listener.py to match,")
        print("   or rows will be silently dropped on the wire.")
    else:
        headroom = usable - CURRENT_ROWS_BUDGET
        print(f"OK: {headroom} bytes of headroom over the current {CURRENT_ROWS_BUDGET}-byte guard.")
        print()
        print("What the extra room could buy (rough, JSON positional rows):")
        # A row is label-dominated: ~21 bytes of scaffolding plus the label.
        for label_len in (14, 20, 28):
            row = 21 + label_len
            fits = max(0, (usable - 16) // (row + 1))
            print(f"  {label_len}-char labels -> ~{row}B/row -> about {fits} rows")
    print("=" * 58)


async def write_probe(client: BleakClient) -> None:
    """Find the real write ceiling by escalating until the peer refuses.

    Uses response=True deliberately. Write-without-response is fire-and-forget:
    an over-MTU payload is dropped with no error anywhere, which is exactly the
    silent failure this whole exercise is about. A write request gives an error.
    """
    print()
    print("Write probe (response=True, so failures are visible)...")
    # Valid JSON the firmware will parse and nack, padded to length. Never a
    # partial payload that might parse as real usage data.
    last_ok = 0
    for size in (20, 50, 100, 150, 180, 200, 250, 300, 400, 500):
        pad = max(0, size - len(b'{"probe":"","ok":false}'))
        body = b'{"probe":"' + b"x" * pad + b'","ok":false}'
        try:
            await client.write_gatt_char(RX_CHAR_UUID, body, response=True)
            last_ok = len(body)
            print(f"  {len(body):4d} B  ok")
        except (BleakError, OSError) as e:
            print(f"  {len(body):4d} B  FAILED -- {type(e).__name__}: {e}")
            break
        await asyncio.sleep(0.2)
    print()
    print(f"Largest write accepted    : {last_ok} bytes")
    print("Note: a write REQUEST can exceed MTU via the long-write (prepare/execute)")
    print("mechanism, so this can read higher than a single write-without-response")
    print("will actually carry. Trust the MTU figure above for the daemon's writes.")


async def main() -> int:
    ap = argparse.ArgumentParser(description="Measure the Clawdmeter BLE MTU.")
    ap.add_argument("--address", help="skip discovery and connect to this address")
    ap.add_argument("--write-probe", action="store_true",
                    help="also escalate write sizes to find the real ceiling")
    args = ap.parse_args()

    target = await find_device(args.address)
    if target is None:
        return 1

    print("Connecting...")
    try:
        async with BleakClient(target) as client:
            print(f"Connected: {client.is_connected}")

            mtu = None
            try:
                mtu = client.mtu_size
            except Exception as e:  # noqa: BLE001 - backend-dependent attribute
                print(f"client.mtu_size unavailable: {type(e).__name__}: {e}")

            report(mtu)

            if args.write_probe:
                await write_probe(client)
    except (BleakError, OSError) as e:
        print(f"Connection failed: {type(e).__name__}: {e}")
        print("If the daemon is running, stop it first -- it holds the link.")
        return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(asyncio.run(main()))
    except KeyboardInterrupt:
        pass
