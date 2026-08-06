#!/usr/bin/env python3
"""Regenerate include/BootLogo.h from src/logo/AE.png.

The artwork is cropped to a tight square around its visible pixels -- so the
logo fills the whole panel rather than floating inside a black border -- then
scaled to SCREEN_SIZE and stored as RGB565 run-length pairs that ShowBootLogo()
in src/main.cpp decodes one scanline at a time.

The panel is round, so a circular logo scaled to the full framebuffer lands
exactly on the visible disc; only the (black) framebuffer corners fall outside.

Requires pillow and numpy. Run from the repo root:

    python3 scripts/generate_boot_logo.py
"""

import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "logo" / "AE.png"
DST = ROOT / "include" / "BootLogo.h"

# The only panel: 2.1" 360x360 GC9B72. Must match SCREEN_SIZE in
# include/DisplayConfig.h.
SCREEN_SIZE = 360

# The source PNG carries a few stray pixels at alpha 1-3 far outside the
# artwork; they are invisible but would blow up a naive bounding box.
ALPHA_FLOOR = 8

RUNS_PER_LINE = 6
MAX_RUN = 0xFFFF  # Run.length is a uint16_t


def content_square(alpha):
    """Tight square crop, centred on the artwork, containing every visible pixel."""
    ys, xs = np.nonzero(alpha >= ALPHA_FLOOR)
    x0, x1 = xs.min(), xs.max()
    y0, y1 = ys.min(), ys.max()
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    half = max(x1 - x0, y1 - y0) / 2.0
    print(f"  content bbox x {x0}-{x1} y {y0}-{y1} -> centre ({cx:.1f}, {cy:.1f}) half {half:.1f}")
    return cx - half, cy - half, cx + half, cy + half


def to_rgb565(rgb):
    r = (rgb[..., 0].astype(np.uint16) >> 3) << 11
    g = (rgb[..., 1].astype(np.uint16) >> 2) << 5
    b = rgb[..., 2].astype(np.uint16) >> 3
    return r | g | b


def rle(pixels):
    """Run-length encode in raster order, splitting runs longer than a uint16_t."""
    flat = pixels.ravel()
    change = np.flatnonzero(np.diff(flat)) + 1
    starts = np.concatenate(([0], change))
    lengths = np.diff(np.concatenate((starts, [flat.size])))

    runs = []
    for color, length in zip(flat[starts], lengths):
        while length > MAX_RUN:
            runs.append((int(color), MAX_RUN))
            length -= MAX_RUN
        runs.append((int(color), int(length)))
    assert sum(l for _, l in runs) == flat.size
    return runs


def render(img, box, size):
    """Composite on black and scale, with a sub-pixel-accurate source box."""
    src = np.asarray(img).astype(np.float64)
    a = src[..., 3:4] / 255.0
    black = (src[..., :3] * a).astype(np.float32)  # premultiply == composite on black

    out = np.zeros((size, size, 3), dtype=np.float64)
    for c in range(3):
        plane = Image.fromarray(black[..., c], "F")
        out[..., c] = np.asarray(plane.resize((size, size), Image.LANCZOS, box=box))
    return np.clip(out, 0, 255).round().astype(np.uint8)


def emit(size, runs):
    out = [
        "#pragma once",
        "",
        "#include <Arduino.h>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "// Generated from src/logo/AE.png as RGB565 RLE by scripts/generate_boot_logo.py.",
        "// Cropped square to the artwork so it fills the panel edge to edge.",
        "// Stored in flash; decoded one scanline at a time at boot.",
        "namespace BootLogo {",
        "",
        "struct Run {",
        "  uint16_t color;",
        "  uint16_t length;",
        "};",
        "",
    ]

    out.append(f"constexpr uint16_t Width = {size};")
    out.append(f"constexpr uint16_t Height = {size};")
    out.append(f"constexpr size_t RunCount = {len(runs)};")
    out.append("static const Run Runs[] PROGMEM = {")
    cells = [f"{{0x{c:04X}, {l}}}" for c, l in runs]
    for j in range(0, len(cells), RUNS_PER_LINE):
        out.append("  " + ", ".join(cells[j:j + RUNS_PER_LINE]) + ",")
    out.append("};")

    out += ["", "}  // namespace BootLogo", ""]
    return "\n".join(out)


def main():
    if not SRC.exists():
        sys.exit(f"missing {SRC}")
    img = Image.open(SRC).convert("RGBA")
    print(f"{SRC.relative_to(ROOT)}: {img.width}x{img.height}")

    alpha = np.asarray(img)[..., 3]
    box = content_square(alpha)

    rgb = render(img, box, SCREEN_SIZE)
    runs = rle(to_rgb565(rgb))
    print(f"  {SCREEN_SIZE}x{SCREEN_SIZE}  {len(runs)} runs  "
          f"{len(runs) * 4} bytes  (raw {SCREEN_SIZE * SCREEN_SIZE * 2})")

    DST.write_text(emit(SCREEN_SIZE, runs))
    print(f"wrote {DST.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
