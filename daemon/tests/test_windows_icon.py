#!/usr/bin/env python3
"""Unit tests for daemon/icon_assets.py — APP-01 tray icon asset layer.

Run: python -m pytest daemon/tests/test_windows_icon.py -x -q
"""
from pathlib import Path

import pytest

from daemon.icon_assets import _expand565, load_logo_rgba

# The "fixture" is the real in-repo firmware logo header — trusted asset.
LOGO_H = Path(__file__).parent.parent.parent / "firmware" / "src" / "logo.h"


# ---------------------------------------------------------------------------
# Task 1: logo parse + RGB565->RGB888 expand
# ---------------------------------------------------------------------------

def test_logo_parse():
    """load_logo_rgba returns an 80x80 RGBA image; dominant opaque color is #DE7552."""
    img = load_logo_rgba(str(LOGO_H))
    assert img.mode == "RGBA", f"Expected RGBA, got {img.mode}"
    assert img.size == (80, 80), f"Expected (80,80), got {img.size}"

    # Collect all fully-opaque pixels and find the dominant RGB.
    from collections import Counter
    px = img.load()
    opaque = [
        (px[x, y][0], px[x, y][1], px[x, y][2])
        for y in range(80)
        for x in range(80)
        if px[x, y][3] == 255
    ]
    assert opaque, "No fully-opaque pixels found in logo"
    dominant = Counter(opaque).most_common(1)[0][0]
    # Brand hex #DE7552 = (222, 117, 82)
    assert dominant == (222, 117, 82), (
        f"Expected dominant opaque color (222,117,82), got {dominant}"
    )


def test_rgb565_expand():
    """_expand565 uses proper rounding, not a *8 bit-shift."""
    assert _expand565(0xDBAA) == (222, 117, 82), (
        f"0xDBAA should be (222,117,82), got {_expand565(0xDBAA)}"
    )
    assert _expand565(0x0000) == (0, 0, 0), (
        f"0x0000 should be (0,0,0), got {_expand565(0x0000)}"
    )
    assert _expand565(0xFFFF) == (255, 255, 255), (
        f"0xFFFF should be (255,255,255), got {_expand565(0xFFFF)}"
    )


def test_logo_parse_bounds_check():
    """load_logo_rgba raises ValueError if the data array length != W*H*3."""
    import tempfile, os

    # Write a malformed header with fewer bytes than expected
    malformed = (
        "#pragma once\n"
        "static const uint8_t logo_data[100] = {\n"
        "    0x00, 0x01, 0x02\n"
        "};\n"
    )
    with tempfile.NamedTemporaryFile(mode="w", suffix=".h", delete=False) as f:
        f.write(malformed)
        path = f.name
    try:
        with pytest.raises(ValueError):
            load_logo_rgba(path)
    finally:
        os.unlink(path)


# ---------------------------------------------------------------------------
# Task 2: state->image corner-bubble compositor
# ---------------------------------------------------------------------------

def test_state_icon_bubble():
    """state_icon returns distinct 32x32 RGBA images per state with correct bubble color."""
    from daemon.icon_assets import state_icon

    base = load_logo_rgba(str(LOGO_H))
    connected = state_icon(base, "connected", 32)
    scanning  = state_icon(base, "scanning",  32)
    error     = state_icon(base, "error",     32)

    # All are 32x32 RGBA
    for name, img in [("connected", connected), ("scanning", scanning), ("error", error)]:
        assert img.mode == "RGBA", f"{name}: expected RGBA, got {img.mode}"
        assert img.size == (32, 32), f"{name}: expected (32,32), got {img.size}"

    # The three images must be pixel-distinct from each other
    def img_bytes(img):
        return img.tobytes()

    assert img_bytes(connected) != img_bytes(scanning), "connected and scanning should differ"
    assert img_bytes(connected) != img_bytes(error),    "connected and error should differ"
    assert img_bytes(scanning)  != img_bytes(error),    "scanning and error should differ"

    # Sample the bottom-right corner region; the closest bubble color should match the state.
    # Bubble colors: connected (60,200,90), scanning (240,180,40), error (220,60,60)
    BUBBLE_COLORS = {
        "connected": (60, 200, 90),
        "scanning":  (240, 180, 40),
        "error":     (220, 60, 60),
    }

    def color_distance(c1, c2):
        return sum((a - b) ** 2 for a, b in zip(c1, c2)) ** 0.5

    def nearest_bubble(pixel_rgb):
        return min(BUBBLE_COLORS.items(), key=lambda kv: color_distance(pixel_rgb, kv[1]))[0]

    size = 32
    r = size // 3
    # Sample the center of the expected bubble region (bottom-right corner)
    bx = size - r // 2 - 2
    by = size - r // 2 - 2
    bx = max(0, min(bx, size - 1))
    by = max(0, min(by, size - 1))

    for state_name, img in [("connected", connected), ("scanning", scanning), ("error", error)]:
        px = img.load()
        pixel = px[bx, by][:3]  # RGB only
        nearest = nearest_bubble(pixel)
        assert nearest == state_name, (
            f"State '{state_name}': bottom-right corner pixel {pixel} is nearest to "
            f"'{nearest}' bubble, expected '{state_name}'"
        )


def test_build_icons_once():
    """build_state_icons returns a dict with connected/scanning/error as distinct Images."""
    from daemon.icon_assets import build_state_icons

    base = load_logo_rgba(str(LOGO_H))
    icons = build_state_icons(base)

    assert set(icons.keys()) == {"connected", "scanning", "error"}, (
        f"Expected keys connected/scanning/error, got {set(icons.keys())}"
    )

    # All distinct
    def img_bytes(img):
        return img.tobytes()

    imgs = list(icons.values())
    assert img_bytes(imgs[0]) != img_bytes(imgs[1])
    assert img_bytes(imgs[0]) != img_bytes(imgs[2])
    assert img_bytes(imgs[1]) != img_bytes(imgs[2])


def test_state_icon_unknown_state():
    """state_icon raises KeyError/ValueError on an unknown state string."""
    from daemon.icon_assets import state_icon

    base = load_logo_rgba(str(LOGO_H))
    with pytest.raises((KeyError, ValueError)):
        state_icon(base, "unknown_state", 32)
