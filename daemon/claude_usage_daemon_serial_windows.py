#!/usr/bin/env python3
"""Claude usage daemon using the ESP32 USB serial connection on Windows."""

import asyncio
import json
import os
import time
from collections.abc import Iterable

import serial
from serial.tools import list_ports

from daemon.claude_usage_daemon_windows import (
    AuthError,
    POLL_INTERVAL,
    TICK,
    log,
    poll_api,
    read_token,
)

BAUD_RATE = 115200
IDENTIFY_TIMEOUT = 4.0
ACK_TIMEOUT = 2.0


def candidate_serial_ports(ports: Iterable[object] | None = None) -> list[str]:
    """Return physical USB serial ports, excluding Windows Bluetooth COM ports."""
    if override := os.environ.get("CLAWDMETER_SERIAL_PORT"):
        return [override.strip().upper()]

    detected = list(list_ports.comports() if ports is None else ports)
    candidates: list[str] = []
    for port in detected:
        description = str(getattr(port, "description", "")).lower()
        hwid = str(getattr(port, "hwid", "")).lower()
        is_bluetooth = "bluetooth" in description or "bthenum" in hwid
        is_usb = getattr(port, "vid", None) is not None or "usb" in hwid
        if is_usb and not is_bluetooth:
            candidates.append(str(port.device).upper())
    return candidates


def is_clawdmeter_identity(line: str) -> bool:
    try:
        value = json.loads(line)
    except (json.JSONDecodeError, TypeError):
        return False
    return isinstance(value, dict) and value.get("device") == "Clawdmeter"


class SerialSession:
    def __init__(self, port: serial.Serial) -> None:
        self.port: serial.Serial = port

    def write_payload(self, payload: dict[str, object]) -> bool:
        data = json.dumps(payload, separators=(",", ":")).encode("utf-8") + b"\n"
        log(f"Sending by USB serial: {data.decode().strip()}")
        try:
            self.port.write(data)
            self.port.flush()
            deadline = time.monotonic() + ACK_TIMEOUT
            while time.monotonic() < deadline:
                line = self.port.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                try:
                    reply = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(reply, dict) and "ack" in reply:
                    return reply["ack"] is True
        except (OSError, serial.SerialException) as exc:
            log(f"USB serial write failed: {exc}")
        return False

    def close(self) -> None:
        try:
            self.port.close()
        except (OSError, serial.SerialException):
            pass


def _open_port(device: str) -> SerialSession | None:
    port = serial.Serial()
    port.port = device
    port.baudrate = BAUD_RATE
    port.timeout = 0.25
    port.write_timeout = 2.0
    port.dtr = False
    port.rts = False
    try:
        port.open()
        deadline = time.monotonic() + IDENTIFY_TIMEOUT
        next_request = 0.0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now >= next_request:
                port.write(b"identify\n")
                port.flush()
                next_request = now + 0.5
            line = port.readline().decode("utf-8", errors="replace").strip()
            if is_clawdmeter_identity(line):
                log(f"Clawdmeter identified on {device}")
                return SerialSession(port)
    except (OSError, serial.SerialException) as exc:
        log(f"Could not open {device}: {exc}")
    try:
        port.close()
    except (OSError, serial.SerialException):
        pass
    return None


def find_clawdmeter_session() -> SerialSession | None:
    for device in candidate_serial_ports():
        if session := _open_port(device):
            return session
    return None


async def connect_and_run(session: SerialSession) -> None:
    last_poll = 0.0
    try:
        while True:
            if time.time() - last_poll >= POLL_INTERVAL:
                token = read_token()
                if not token:
                    log("No token; skipping poll")
                else:
                    try:
                        payload = await poll_api(token)
                    except AuthError:
                        log("Token expired; run claude login")
                        payload = None
                    if payload is not None:
                        if not await asyncio.to_thread(session.write_payload, payload):
                            log("USB serial acknowledgement failed; reconnecting")
                            return
                        last_poll = time.time()
            await asyncio.sleep(TICK)
    finally:
        await asyncio.to_thread(session.close)


async def main() -> None:
    log("=== Claude Usage Tracker Daemon (USB serial, Windows) ===")
    backoff = 1
    while True:
        session = await asyncio.to_thread(find_clawdmeter_session)
        if session is None:
            log(f"Clawdmeter USB serial not found, retrying in {backoff}s...")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)
            continue

        await connect_and_run(session)
        backoff = 1


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log("Daemon stopping")
