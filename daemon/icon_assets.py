#!/usr/bin/env python3
"""Icon asset layer for the Windows tray app.

Parses the firmware/src/logo.h RGB565A8 brand logo into a Pillow RGBA image,
expands RGB565->RGB888 with correct rounding, and composites per-state corner
bubbles (green=connected / amber=scanning / red=error) onto the constant brand
mark. All logic is pure and unit-testable off-Windows (Pillow only, no pystray
or winreg here).

Usage::

    from daemon.icon_assets import load_logo_rgba, build_state_icons

    base = load_logo_rgba("firmware/src/logo.h")
    icons = build_state_icons(base)   # dict: "connected"/"scanning"/"error" -> Image
"""
import re
from PIL import Image, ImageDraw

# Logo dimensions from logo.h #defines.
W: int = 80
H: int = 80

# Brand hex derived from the dominant opaque RGB565 color in logo.h (D-03).
# 0xDBAA -> RGB888 (222, 117, 82) -> #DE7552
BRAND_HEX: str = "#DE7552"

# Locked corner-bubble RGBA colors (from RESEARCH, verified end-to-end).
BUBBLE: dict[str, tuple[int, int, int, int]] = {
    "connected": (60, 200, 90, 255),   # green
    "scanning":  (240, 180, 40, 255),  # amber
    "error":     (220, 60, 60, 255),   # red
}


def _expand565(v: int) -> tuple[int, int, int]:
    """Expand a 16-bit RGB565 value to an (R, G, B) tuple using proper rounding.

    Uses ``(channel * 255 + max // 2) // max`` per channel, NOT a *8 bit-shift.
    A *8 shift loses the low bits and does not correctly round to 255 for 0xFFFF.

    Examples::

        _expand565(0x0000) == (0, 0, 0)
        _expand565(0xFFFF) == (255, 255, 255)
        _expand565(0xDBAA) == (222, 117, 82)   # brand hex
    """
    r5 = (v >> 11) & 0x1F   # 5-bit red channel   (max 31)
    g6 = (v >> 5)  & 0x3F   # 6-bit green channel (max 63)
    b5 =  v        & 0x1F   # 5-bit blue channel  (max 31)
    r = (r5 * 255 + 15) // 31
    g = (g6 * 255 + 31) // 63
    b = (b5 * 255 + 15) // 31
    return (r, g, b)


def load_logo_rgba(header_path: str) -> Image.Image:
    """Parse the firmware logo.h C header and return an 80x80 Pillow RGBA Image.

    The logo.h layout (RGB565A8 planar, little-endian RGB565):
      - First ``W * H * 2`` bytes: little-endian RGB565 pixel data
      - Next  ``W * H``     bytes: 8-bit alpha plane

    Args:
        header_path: Path to ``firmware/src/logo.h`` (or any compatible header).

    Returns:
        An ``Image.Image`` of mode ``"RGBA"`` and size ``(W, H)``.

    Raises:
        ValueError: If the extracted byte array length != ``W * H * 3`` (ASVS V5
            bound-check before indexing).
    """
    with open(header_path) as f:
        txt = f.read()

    # Extract the byte array body from: logo_data[N] = { 0xFF, 0xAA, ... };
    match = re.search(r'logo_data\[\d+\]\s*=\s*\{(.*?)\};', txt, re.S)
    if not match:
        raise ValueError(f"Could not find logo_data[] array in {header_path!r}")

    body = match.group(1)
    raw_bytes = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]

    # Bound-check before indexing (ASVS V5).
    expected = W * H * 3  # W*H*2 RGB565 bytes + W*H alpha bytes = 19200
    if len(raw_bytes) != expected:
        raise ValueError(
            f"logo_data byte count mismatch: expected {expected}, got {len(raw_bytes)}"
        )

    n = W * H
    rgb_bytes = raw_bytes[:n * 2]      # first 12800 bytes: little-endian RGB565
    alpha_bytes = raw_bytes[n * 2:]    # last  6400 bytes:  8-bit alpha

    img = Image.new("RGBA", (W, H))
    px = img.load()
    for i in range(n):
        # Little-endian: low byte first, high byte second.
        v = rgb_bytes[i * 2] | (rgb_bytes[i * 2 + 1] << 8)
        r, g, b = _expand565(v)
        px[i % W, i // W] = (r, g, b, alpha_bytes[i])

    return img


def state_icon(base: Image.Image, state: str, size: int = 32) -> Image.Image:
    """Composite a colored corner bubble onto the brand mark for the given state.

    Args:
        base:  The RGBA brand image (as returned by ``load_logo_rgba``).
        state: One of ``"connected"``, ``"scanning"``, or ``"error"``.
               Any other value raises ``KeyError`` — no silent fallback.
        size:  Target icon edge length in pixels (default 32).

    Returns:
        A new ``Image.Image`` of mode ``"RGBA"`` and size ``(size, size)``.

    Raises:
        KeyError: If ``state`` is not one of the three known states.
    """
    # Raises KeyError on unknown state — no silent default (per plan anti-pattern).
    bubble_color = BUBBLE[state]

    icon = base.resize((size, size), Image.LANCZOS).convert("RGBA")
    draw = ImageDraw.Draw(icon)

    # Corner bubble: ~1/3 of the icon, drawn in the bottom-right corner.
    r = size // 3
    x0 = size - r - 1
    y0 = size - r - 1
    x1 = size - 2
    y1 = size - 2
    draw.ellipse([x0, y0, x1, y1], fill=bubble_color)

    return icon


def build_state_icons(
    base: Image.Image,
    size: int = 32,
) -> dict[str, Image.Image]:
    """Build all three connection-state icons from the brand base image.

    Build them once at startup; swap ``icon.icon = icons[state]`` in the tray
    loop — never recomposite per tick (per RESEARCH anti-pattern).

    Args:
        base: The RGBA brand image (as returned by ``load_logo_rgba``).
        size: Target icon edge length in pixels (default 32).

    Returns:
        A dict mapping ``"connected"``, ``"scanning"``, and ``"error"`` to
        their respective composited ``Image.Image`` objects.
    """
    return {state: state_icon(base, state, size) for state in BUBBLE}
