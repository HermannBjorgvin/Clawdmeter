#!/usr/bin/env python3
"""Send one serial command to a Clawdmeter board and print its reply.

    serial_cmd.py <command...> [--port PORT] [--quiet-ms 600] [--timeout 5]

Examples:
    serial_cmd.py info
    serial_cmd.py clawd show
    serial_cmd.py chime 0.7 77 --port /dev/cu.usbmodem101

The port defaults to the only /dev/ttyACM* (Linux) or /dev/cu.usbmodem*
(macOS) present. Output is every line the board prints until it has been
quiet for --quiet-ms (log lines from the firmware loop can be interleaved
with the reply). Opening the port doesn't reset the board. Needs pyserial: PlatformIO's Python has it
(~/.platformio/penv/bin/python).
"""

import argparse
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: run with ~/.platformio/penv/bin/python, "
             "or pip install pyserial")

BAUD = 115200  # monitor_speed of every env in firmware/platformio.ini


def default_port():
    found = sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbmodem*"))
    if len(found) == 1:
        return found[0]
    if not found:
        sys.exit("no board found (no /dev/ttyACM* or /dev/cu.usbmodem*); "
                 "plug one in or pass --port")
    sys.exit(f"several serial ports: {', '.join(found)}; pass --port")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", nargs="+", help="command and arguments, e.g. clawd show")
    ap.add_argument("--port")
    ap.add_argument("--quiet-ms", type=int, default=600,
                    help="stop after this long without output (default 600)")
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="give up after this many seconds (default 5)")
    args = ap.parse_args()

    port_path = args.port or default_port()
    cmd = " ".join(args.command)
    if cmd == "screenshot":
        sys.exit("screenshot is a binary framebuffer dump: use the capturing-device-screenshots skill")

    try:
        port = serial.Serial(port_path, BAUD, timeout=0.05)
    except serial.SerialException as e:
        sys.exit(f"cannot open {port_path}: {e} (is a serial monitor holding it?)")

    port.reset_input_buffer()
    port.write(cmd.encode() + b"\n")
    port.flush()

    start = last = time.monotonic()
    got = False
    while True:
        line = port.readline()
        now = time.monotonic()
        if line:
            got = True
            last = now
            print(line.decode("utf-8", errors="replace").rstrip())
        elif got and (now - last) * 1000 >= args.quiet_ms:
            break
        if now - start >= args.timeout:
            break
    port.close()
    if not got:
        sys.exit(f"no reply to {cmd!r} on {port_path} within {args.timeout}s")


if __name__ == "__main__":
    main()
