#!/usr/bin/env python3
"""Claude usage daemon using the ESP32 USB serial connection on Windows."""

import asyncio
import json
import os
import signal
import threading
import time
from pathlib import Path

import serial
from serial.tools import list_ports

from daemon.claude_usage_daemon_windows import (
    AuthError,
    POLL_INTERVAL,
    TICK,
    _wait_first,
    log,
    poll_api,
    read_token,
)
from daemon.dashboard_payload import build_dashboard_payload

BAUD_RATE = 115200
IDENTIFY_TIMEOUT = 4.0
ACK_TIMEOUT = 2.0


def candidate_serial_ports(ports=None) -> list[str]:
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
    def __init__(self, port) -> None:
        self.port = port

    def write_payload(self, payload: dict) -> bool:
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


async def connect_and_run(session: SerialSession, stop_event: asyncio.Event, tray_state=None) -> bool:
    last_poll = 0.0
    used_successfully = False
    try:
        while not stop_event.is_set():
            if time.time() - last_poll >= POLL_INTERVAL:
                claude_payload = None
                token = read_token()
                if not token:
                    log("No token; sending local dashboard data only")
                    if tray_state:
                        tray_state.set_error("token expired - run claude login")
                else:
                    try:
                        claude_payload = await poll_api(token)
                    except AuthError:
                        if tray_state:
                            tray_state.set_error("token expired - run claude login")

                payload = await asyncio.to_thread(
                    build_dashboard_payload,
                    claude_payload,
                    Path.home(),
                )
                if not await asyncio.to_thread(session.write_payload, payload):
                    log("USB serial acknowledgement failed; reconnecting")
                    break
                last_poll = time.time()
                used_successfully = True
                if tray_state and claude_payload is not None:
                    tray_state.set_connected(last_poll)
            await _wait_first(stop_event, timeout=TICK)
    finally:
        await asyncio.to_thread(session.close)
    return used_successfully


async def main(tray_state=None) -> None:
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()
    if tray_state is not None:
        tray_state.loop = loop
        tray_state.stop_event = stop_event

    def _stop(*_args: object) -> None:
        log("Daemon stopping")
        stop_event.set()

    if threading.current_thread() is threading.main_thread():
        for sig in (signal.SIGINT, signal.SIGTERM):
            try:
                loop.add_signal_handler(sig, _stop)
            except NotImplementedError:
                signal.signal(sig, _stop)

    log("=== Claude Usage Tracker Daemon (USB serial, Windows) ===")
    backoff = 1
    while not stop_event.is_set():
        session = await asyncio.to_thread(find_clawdmeter_session)
        if session is None:
            if tray_state:
                tray_state.set_scanning()
            log(f"Clawdmeter USB serial not found, retrying in {backoff}s...")
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=backoff)
            except asyncio.TimeoutError:
                pass
            backoff = min(backoff * 2, 30)
            continue

        await connect_and_run(session, stop_event, tray_state)
        backoff = 1


if __name__ == "__main__":
    asyncio.run(main())
