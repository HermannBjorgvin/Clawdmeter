#!/usr/bin/env python3
"""Tests for the `status_line` config option and its "sl" payload field.

`status_line = off` adds "sl":0 to every data payload so the firmware keeps the
bottom status line blank; on/unset/invalid adds nothing (byte-for-byte as before).
Covers both the macOS and Windows daemons. httpx is mocked; no network calls.

Run: python -m pytest daemon/tests/test_status_line.py -q
"""
import asyncio
import json
import time
from unittest.mock import AsyncMock, MagicMock, patch

import pytest

import daemon.claude_usage_daemon as mac
import daemon.claude_usage_daemon_windows as win

MODULES = [pytest.param(mac, id="macos"), pytest.param(win, id="windows")]


def _run(coro):
    loop = asyncio.new_event_loop()
    try:
        return loop.run_until_complete(coro)
    finally:
        loop.close()


def _pro_headers():
    now = time.time()
    return {
        "anthropic-ratelimit-unified-5h-utilization": "0.25",
        "anthropic-ratelimit-unified-5h-reset": str(now + 3600),
        "anthropic-ratelimit-unified-7d-utilization": "0.50",
        "anthropic-ratelimit-unified-7d-reset": str(now + 86400),
        "anthropic-ratelimit-unified-5h-status": "allowed",
    }


def _ent_headers():
    return {
        "anthropic-ratelimit-unified-overage-utilization": "0.10",
        "anthropic-ratelimit-unified-overage-reset": str(time.time() + 7 * 86400),
        "anthropic-ratelimit-unified-status": "allowed",
    }


def _poll(mod, headers):
    resp = MagicMock()
    resp.status_code = 200
    resp.text = "mocked"
    resp.headers = MagicMock()
    resp.headers.get = lambda name, default=None: headers.get(name.lower(), default)

    async def fake_post(*args, **kwargs):
        return resp

    client = AsyncMock()
    client.__aenter__ = AsyncMock(return_value=client)
    client.__aexit__ = AsyncMock(return_value=False)
    client.post = fake_post
    with patch("httpx.AsyncClient", return_value=client):
        return _run(mod.poll_api("fake-token"))


@pytest.fixture
def config(tmp_path, monkeypatch):
    """Point both daemons at a temp config; returns a writer (None = no file)."""
    cfg = tmp_path / "config"
    monkeypatch.setattr(mac, "CONFIG_FILE", cfg)
    monkeypatch.setattr(win, "CONFIG_FILE", cfg)

    def write(text):
        if text is None:
            cfg.unlink(missing_ok=True)
        else:
            cfg.write_text(text)
    return write


# --- setting parser -----------------------------------------------------------

@pytest.mark.parametrize("mod", MODULES)
@pytest.mark.parametrize("text, want", [
    (None, "on"),
    ("chime = on\n", "on"),
    ("status_line = on\n", "on"),
    ("status_line = off\n", "off"),
    ("  STATUS_LINE=OFF   # hide it\r\n", "off"),
    ("status_line = maybe\n", "on"),
    ("status_line = 0\n", "on"),
    ("status_line =\n", "on"),
])
def test_read_status_line_setting(mod, config, text, want):
    config(text)
    assert mod.read_status_line_setting() == want


@pytest.mark.parametrize("mod", MODULES)
def test_read_status_line_setting_unreadable_defaults_on(mod, config, tmp_path):
    (tmp_path / "config").mkdir()  # a directory: read_text raises OSError
    assert mod.read_status_line_setting() == "on"


# --- payloads -----------------------------------------------------------------

@pytest.mark.parametrize("mod", MODULES)
@pytest.mark.parametrize("headers", [_pro_headers, _ent_headers], ids=["pro", "ent"])
@pytest.mark.parametrize("text", [None, "status_line = on\n", "status_line = sideways\n"],
                         ids=["unset", "on", "garbage"])
def test_no_sl_unless_off(mod, config, headers, text):
    config(text)
    payload = _poll(mod, headers())
    assert payload["ok"] is True
    assert "sl" not in payload


@pytest.mark.parametrize("mod", MODULES)
@pytest.mark.parametrize("headers, acct", [(_pro_headers, "pro"), (_ent_headers, "ent")],
                         ids=["pro", "ent"])
def test_off_adds_sl_zero(mod, config, headers, acct):
    config("status_line = off\n")
    payload = _poll(mod, headers())
    assert payload["acct"] == acct
    assert payload["ok"] is True
    assert payload["sl"] == 0


@pytest.mark.parametrize("mod", MODULES)
def test_unset_pro_payload_unchanged(mod, config):
    """Users who never touch the option get exactly the pre-existing key set."""
    config(None)
    payload = _poll(mod, _pro_headers())
    assert list(payload) == ["s", "sr", "w", "wr", "st", "acct", "ok"]


@pytest.mark.parametrize("mod", MODULES)
def test_off_composes_with_chime_and_reread_each_poll(mod, config):
    config("chime = on\nstatus_line = off\n")
    payload = _poll(mod, _pro_headers())
    assert payload["c"] == 1 and payload["sl"] == 0
    config("chime = on\nstatus_line = on\n")
    payload = _poll(mod, _pro_headers())
    assert payload["c"] == 1 and "sl" not in payload


@pytest.mark.parametrize("mod", MODULES)
def test_off_wire_json(mod, config):
    config("status_line = off\n")
    wire = json.dumps(_poll(mod, _pro_headers()), separators=(",", ":"))
    assert wire.endswith(',"acct":"pro","ok":true,"sl":0}')


# --- the {"ok": false} no-data beat never carries "sl" --------------------------

def _capture_writes(mod, target, patches):
    """Run one connect_and_run cycle with BleakClient mocked; return decoded writes."""
    writes = []
    client = AsyncMock()
    client.connect = AsyncMock(return_value=None)
    client.is_connected = True
    client.disconnect = AsyncMock()
    client.start_notify = AsyncMock()

    async def cap_write(uuid, data, response=False):
        writes.append(json.loads(bytes(data).decode()))
    client.write_gatt_char = AsyncMock(side_effect=cap_write)

    async def go():
        stop_event = asyncio.Event()
        extra = patches(stop_event)
        with patch.object(mod, "BleakClient", return_value=client):
            with extra[0], extra[1]:
                await mod.connect_and_run(target, stop_event)

    _run(go())
    return writes


def test_no_data_beat_never_carries_sl_macos(config):
    config("status_line = off\n")

    def patches(stop_event):
        async def fake_poll_active(*a, **k):
            stop_event.set()
            return None, True  # no live token anywhere -> no-data beat
        return (patch.object(mac, "poll_active", new=fake_poll_active),
                patch.object(mac, "log", lambda *a, **k: None))

    device = MagicMock(); device.address = "00:00:00:00:00:00"
    assert _capture_writes(mac, device, patches) == [{"ok": False}]


def test_no_data_beat_never_carries_sl_windows(config):
    config("status_line = off\n")

    def patches(stop_event):
        def no_token():
            stop_event.set()
            return None
        return (patch.object(win, "read_token", new=no_token),
                patch.object(win, "log", lambda *a, **k: None))

    device = MagicMock(); device.address = "00:00:00:00:00:00"
    assert _capture_writes(win, device, patches) == [{"ok": False}]
