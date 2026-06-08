#!/usr/bin/env python3
"""Unit tests for the bonded-device address fallback — BLE-04.

A Clawdmeter that is paired AND connected to Windows (as a bonded HID
keyboard) no longer advertises, so BleakScanner.find_device_by_name() never
returns it. The daemon must then connect directly by the device's address,
which it recovers from the Windows PnP instance id.

These tests cover the pure parsing seam — recovering a canonical BLE MAC
("AA:BB:CC:DD:EE:FF") from a PnP instance id string.

Run: python -m pytest daemon/tests/test_windows_bonded.py -x -q
"""
import asyncio
from unittest.mock import patch

import pytest

from daemon.claude_usage_daemon_windows import (
    _mac_from_pnp_instance_id,
    acquire_target,
)


def _run(coro):
    return asyncio.run(coro)


def test_recovers_mac_from_standard_bthle_instance_id():
    instance_id = r"BTHLE\DEV_98A316A5D706\7&B8081D1&0&98A316A5D706"
    assert _mac_from_pnp_instance_id(instance_id) == "98:A3:16:A5:D7:06"


def test_uppercases_lowercase_hex():
    instance_id = r"BTHLE\DEV_aabbccddeeff\7&x&0&aabbccddeeff"
    assert _mac_from_pnp_instance_id(instance_id) == "AA:BB:CC:DD:EE:FF"


def test_returns_none_when_no_dev_token_present():
    assert _mac_from_pnp_instance_id(r"USB\VID_1234&PID_5678\ABC") is None


def test_returns_none_for_empty_string():
    assert _mac_from_pnp_instance_id("") is None


def test_ignores_short_hex_run_that_is_not_a_mac():
    # DEV_ must be followed by exactly 12 hex digits to be a BLE MAC.
    assert _mac_from_pnp_instance_id(r"BTHLE\DEV_98A3\junk") is None


# ---------------------------------------------------------------------------
# acquire_target: scan first, bonded address as fallback
# ---------------------------------------------------------------------------

def test_acquire_target_returns_scanned_device_without_bonded_lookup():
    """A device found by advertisement scan is returned; bonded lookup is skipped."""
    sentinel_device = object()

    async def fake_scan():
        return sentinel_device

    with patch("daemon.claude_usage_daemon_windows.scan_for_device", side_effect=fake_scan), \
         patch("daemon.claude_usage_daemon_windows.discover_bonded_address") as disc:
        result = _run(acquire_target())

    assert result is sentinel_device
    disc.assert_not_called()  # never look up the bonded address when scan hits


def test_acquire_target_falls_back_to_bonded_address_when_scan_misses():
    """When the device isn't advertising, the bonded address is wrapped in a BLEDevice.

    A BLEDevice (not a bare string) is required so WinRT skips its advertisement
    scan and connects directly to the bonded device by address.
    """
    from bleak.backends.device import BLEDevice

    async def fake_scan():
        return None

    with patch("daemon.claude_usage_daemon_windows.scan_for_device", side_effect=fake_scan), \
         patch("daemon.claude_usage_daemon_windows.discover_bonded_address",
               return_value="98:A3:16:A5:D7:06"):
        result = _run(acquire_target())

    assert isinstance(result, BLEDevice)
    assert result.address == "98:A3:16:A5:D7:06"


def test_acquire_target_returns_none_when_scan_and_bonded_both_miss():
    """Neither advertising nor bonded -> None so the caller backs off."""
    async def fake_scan():
        return None

    with patch("daemon.claude_usage_daemon_windows.scan_for_device", side_effect=fake_scan), \
         patch("daemon.claude_usage_daemon_windows.discover_bonded_address", return_value=None):
        result = _run(acquire_target())

    assert result is None
