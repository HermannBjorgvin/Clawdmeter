#!/usr/bin/env python3
"""Tests for the Windows USB serial transport."""

import asyncio
import json
from types import SimpleNamespace
from unittest.mock import MagicMock

from daemon import claude_usage_daemon_serial_windows as serial_daemon
from daemon.claude_usage_daemon_serial_windows import (
    SerialSession,
    candidate_serial_ports,
    is_clawdmeter_identity,
)


def test_candidate_serial_ports_prefers_real_usb_ports(monkeypatch):
    ports = [
        SimpleNamespace(device="COM10", description="Standard Serial over Bluetooth", hwid="BTHENUM", vid=None),
        SimpleNamespace(device="COM3", description="USB-SERIAL CH340", hwid="USB VID:PID=1A86:7523", vid=0x1A86),
    ]

    monkeypatch.delenv("CLAWDMETER_SERIAL_PORT", raising=False)

    assert candidate_serial_ports(ports) == ["COM3"]


def test_candidate_serial_ports_honors_explicit_override(monkeypatch):
    monkeypatch.setenv("CLAWDMETER_SERIAL_PORT", "COM7")

    assert candidate_serial_ports([]) == ["COM7"]


def test_identity_requires_clawdmeter_device_name():
    assert is_clawdmeter_identity('{"device":"Clawdmeter","board":"ESP32-2432S024C"}')
    assert not is_clawdmeter_identity('{"ready":true}')
    assert not is_clawdmeter_identity("boot noise")


def test_serial_session_sends_compact_json_line_and_accepts_ack():
    port = MagicMock()
    port.readline.side_effect = [b"debug line\r\n", b'{"ack":true}\r\n']
    session = SerialSession(port)
    payload = {"s": 12.5, "w": 34.0, "ok": True}

    assert session.write_payload(payload)
    assert port.write.call_args.args[0] == (
        json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
    )


def test_serial_session_rejects_negative_ack():
    port = MagicMock()
    port.readline.return_value = b'{"ack":false}\n'

    assert not SerialSession(port).write_payload({"s": 1})


def test_connect_and_run_sends_local_dashboard_data_without_token(monkeypatch):
    stop_event = asyncio.Event()
    session = SimpleNamespace(write_payload=MagicMock(return_value=True), close=MagicMock())

    async def stop_after_first_iteration(_event, timeout):
        stop_event.set()

    monkeypatch.setattr(serial_daemon, "read_token", lambda: None)
    monkeypatch.setattr(
        serial_daemon,
        "build_dashboard_payload",
        lambda claude_payload, _home: {"v": 2, "claude": claude_payload},
        raising=False,
    )
    monkeypatch.setattr(serial_daemon, "_wait_first", stop_after_first_iteration)

    result = asyncio.run(serial_daemon.connect_and_run(session, stop_event))

    assert result is True
    session.write_payload.assert_called_once_with({"v": 2, "claude": None})


def test_extended_dashboard_payload_fits_firmware_command_buffer():
    payload = {
        "s": 100,
        "sr": 10080,
        "w": 100,
        "wr": 10080,
        "st": "allowed",
        "ok": True,
        "v": 2,
        "ts": 2147483647,
        "x": {
            "l": [
                {"p": 100, "wm": 10080, "rm": 10080},
                {"p": 100, "wm": 10080, "rm": 10080},
            ],
            "td": 2147483647,
            "pl": "enterprise",
        },
        "a": {"cl": {"o": 999, "b": 999, "w": 999}, "cx": {"u": 999}, "ts": 2147483647},
    }
    wire = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
    assert len(wire) < 768
