#!/usr/bin/env python3
"""Capture a BUSY Bar's screen to a PNG — screenshot.sh for the other display.

    python3 tools/busybar_shot.py out.png [http://10.0.4.20] [--back]

The same rule the firmware has: QA your own UI changes, don't ask the user.
A 72x16 layout is too small to design blind, and the bar can hand you exactly
what it is showing.

TWO UNDOCUMENTED THINGS about `GET /api/screen`, both found the hard way:

  * The response is **base64 text**, not the `image/bmp` the spec advertises.
    4608 characters decode to 3456 bytes = 72 x 16 x 3.
  * Those bytes are **BGR**, not RGB. Sending green `#8FA76B` (143,167,107)
    and reading back (107,167,143) is what proves it. Render without the swap
    and every colour judgement you make is wrong -- amber looks blue.
"""
import base64
import sys
import urllib.request

from PIL import Image

WIDTH, HEIGHT = 72, 16
SCALE = 10


def capture(base_url: str, back: bool = False) -> Image.Image:
    url = f"{base_url.rstrip('/')}/api/screen?display={1 if back else 0}"
    with urllib.request.urlopen(url, timeout=25) as resp:
        raw = base64.b64decode(resp.read())
    expected = WIDTH * HEIGHT * 3
    if len(raw) != expected:
        raise SystemExit(f"expected {expected} bytes, got {len(raw)} — the "
                         "frame format may have changed")
    img = Image.frombytes("RGB", (WIDTH, HEIGHT), raw)
    b, g, r = img.split()
    return Image.merge("RGB", (r, g, b))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else "busybar.png"
    url = args[1] if len(args) > 1 else "http://10.0.4.20"
    img = capture(url, back="--back" in sys.argv)
    img.resize((WIDTH * SCALE, HEIGHT * SCALE), Image.NEAREST).save(out)
    print(f"Saved: {out} ({WIDTH}x{HEIGHT}, scaled {SCALE}x)")
