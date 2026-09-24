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

# The two panels report in different formats, neither of them the advertised
# image/bmp. Front: RGB888, 3 bytes a pixel, 72x16. Back: 8-bit greyscale at
# 80x80 (6400 bytes) even though the panel is 160x80 -- the capture is half
# width, so it is doubled horizontally to restore the aspect. Rendering the
# 6400 bytes as 4bpp across 160 columns also "works" and is wrong: it doubles
# every pixel by accident and happens to look right.
FRONT = (72, 16)
BACK = (80, 80)
BACK_PANEL = (160, 80)
SCALE = 10


def capture(base_url: str, back: bool = False) -> Image.Image:
    w, h = BACK if back else FRONT
    url = f"{base_url.rstrip('/')}/api/screen?display={1 if back else 0}"
    with urllib.request.urlopen(url, timeout=25) as resp:
        raw = base64.b64decode(resp.read())

    if back:
        if len(raw) != w * h:
            raise SystemExit(f"expected {w * h} bytes for the back screen, "
                             f"got {len(raw)}")
        return Image.frombytes("L", (w, h), raw).resize(BACK_PANEL, Image.NEAREST)

    if len(raw) != w * h * 3:
        raise SystemExit(f"expected {w * h * 3} bytes, got {len(raw)} — the "
                         "frame format may have changed")
    b, g, r = Image.frombytes("RGB", (w, h), raw).split()
    return Image.merge("RGB", (r, g, b))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    out = args[0] if args else "busybar.png"
    url = args[1] if len(args) > 1 else "http://10.0.4.20"
    img = capture(url, back="--back" in sys.argv)
    scale = SCALE if img.width == FRONT[0] else 4
    img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(out)
    print(f"Saved: {out} ({img.width}x{img.height}, scaled {scale}x)")
