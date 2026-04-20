#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pillow"]
# ///
"""Render the Clew icon as a multi-size ICO using Pillow.

We draw directly with ImageDraw rather than rasterizing the SVG, so
contributors don't need cairo/imagemagick. The shape mirrors assets/clew.svg
(a stylized C with a thread-and-needle tail).

Invoke from the repo root:

    uv run --script assets/_make_ico.py
"""
from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw

HERE = Path(__file__).parent
ICO = HERE / "clew.ico"
PNG_PREVIEW = HERE / "clew.png"
SIZES = (16, 32, 48, 64, 128, 256)

BG = (244, 236, 221, 255)      # cream
FG = (26, 26, 26, 255)         # dark gray


def render(size: int) -> Image.Image:
    # Render at 4x for antialiasing then downsample.
    scale = 4
    w = h = size * scale
    img = Image.new("RGBA", (w, h), BG)
    draw = ImageDraw.Draw(img)

    # Stroke width ~6.5% of canvas, scaled up.
    sw = max(int(round(w * 0.065)), 2)

    cx, cy = w * 0.47, h * 0.51
    radius = w * 0.27

    # --- Main C: arc opening toward the upper-right (~320° sweep). ---
    start_angle = -50   # upper right (0° = 3 o'clock, angles clockwise)
    end_angle = 260     # back around via left side to lower right
    bbox = (cx - radius, cy - radius, cx + radius, cy + radius)
    draw.arc(bbox, start=start_angle, end=end_angle, fill=FG, width=sw)

    # Helper: point on the arc at a given angle.
    def on_arc(angle_deg: float) -> tuple[float, float]:
        a = math.radians(angle_deg)
        return cx + radius * math.cos(a), cy + radius * math.sin(a)

    # --- Thread tail from top end of C to the needle head. ---
    x0, y0 = on_arc(start_angle)
    nx, ny = cx + w * 0.33, cy - h * 0.30
    draw.line([(x0, y0), (nx, ny)], fill=FG, width=sw)

    # --- Needle head (filled dot). ---
    dot_r = sw * 1.35
    draw.ellipse([nx - dot_r, ny - dot_r, nx + dot_r, ny + dot_r], fill=FG)

    # --- Decorative inner curl: small comma inside the C's right opening. ---
    cux, cuy = cx + radius * 0.35, cy + radius * 0.15
    curl_r = radius * 0.22
    cbbox = (cux - curl_r, cuy - curl_r, cux + curl_r, cuy + curl_r)
    draw.arc(cbbox, start=200, end=30, fill=FG, width=sw)

    # Downsample with high-quality filter.
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    images = [render(s) for s in SIZES]
    images[0].save(
        ICO,
        format="ICO",
        sizes=[(s, s) for s in SIZES],
        append_images=images[1:],
    )
    images[-1].save(PNG_PREVIEW)
    print(f"Wrote {ICO}")
    print(f"Preview {PNG_PREVIEW}")


if __name__ == "__main__":
    main()
