#!/usr/bin/env python3
"""Regenerate include/WebLogo.h from src/logo/AE.png.

The same artwork the panel shows at boot, sized for the configuration page's
masthead and served from /logo.png by ConfigurationWebServer. The boot logo is
RGB565 run-length pairs because BootScreen paints straight into a framebuffer;
this one stays a PNG, because the thing consuming it is a browser and a browser
decodes PNG far better than the firmware could unpack anything else.

Two details do the work:

  * Premultiplied resampling. Every fully transparent pixel in the source is
    black, so resizing straight RGBA blends that black into all the soft edges
    of the particle ring and the result looks muddy and dark. Premultiplying
    before the resize and dividing it back out afterwards keeps the edges the
    colour they actually are.

  * Palette quantisation. The shattered ring is high-frequency noise, which is
    the worst case for PNG's filters -- a full-colour 224px export is 40 KB.
    64 colours is indistinguishable at the size it is displayed and costs a
    quarter of that.

Requires pillow and numpy. Run from the repo root:

    python3 scripts/generate_web_logo.py
"""

import sys
import textwrap
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "logo" / "AE.png"
DST = ROOT / "include" / "WebLogo.h"

# Displayed at 104 CSS pixels, so this covers a 2x screen with a little spare.
# Larger buys nothing: past roughly this size the particle ring is already
# resolving everything the eye picks up at 104px.
SIZE = 224

# Below about 48 the deep blues of the ring start to band. 64 is where the
# file stops getting smaller for any visible gain.
COLORS = 64

# The source PNG carries a few stray pixels at alpha 1-3 far outside the
# artwork; they are invisible but would blow up a naive bounding box.
ALPHA_FLOOR = 8

BYTES_PER_LINE = 16


def content_square(alpha):
    """Tight square crop, centred on the artwork, containing every visible pixel."""
    ys, xs = np.nonzero(alpha >= ALPHA_FLOOR)
    x0, x1 = xs.min(), xs.max()
    y0, y1 = ys.min(), ys.max()
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    half = max(x1 - x0, y1 - y0) / 2.0
    print(f"  content bbox x {x0}-{x1} y {y0}-{y1} -> centre ({cx:.1f}, {cy:.1f}) half {half:.1f}")
    return cx - half, cy - half, cx + half, cy + half


def downscale(img, box, size):
    """Lanczos in premultiplied space, unpremultiplied back to straight RGBA."""
    src = np.asarray(img).astype(np.float64)
    alpha = src[..., 3:4] / 255.0
    premultiplied = np.concatenate([src[..., :3] * alpha, src[..., 3:4]], axis=2)

    out = np.zeros((size, size, 4), dtype=np.float64)
    for c in range(4):
        plane = Image.fromarray(premultiplied[..., c].astype(np.float32), "F")
        out[..., c] = np.asarray(plane.resize((size, size), Image.LANCZOS, box=box))

    out_alpha = np.clip(out[..., 3:4], 0, 255)
    rgb = np.where(
        out_alpha > 0.5,
        np.clip(out[..., :3] / np.maximum(out_alpha / 255.0, 1e-6), 0, 255),
        0,
    )
    return Image.fromarray(
        np.concatenate([rgb, out_alpha], axis=2).round().astype(np.uint8), "RGBA"
    )


def encode(image):
    """Quantise and return the PNG bytes, via a temporary file PIL will write."""
    from io import BytesIO

    # FASTOCTREE rather than the default: it is the only one of PIL's methods
    # that quantises the alpha channel too, which this artwork is entirely
    # made of.
    buffer = BytesIO()
    image.quantize(colors=COLORS, method=Image.FASTOCTREE).save(
        buffer, format="PNG", optimize=True
    )
    return buffer.getvalue()


def emit(png, size):
    header = textwrap.dedent(
        f"""\
        #pragma once

        #include <Arduino.h>
        #include <cstddef>
        #include <cstdint>

        // Generated from src/logo/AE.png by scripts/generate_web_logo.py.
        // The same mark the panel shows at boot, as a {size}x{size} PNG served from
        // /logo.png by ConfigurationWebServer -- the configuration page's masthead
        // and its favicon are both this one asset.
        namespace WebLogo {{

        constexpr uint16_t Width = {size};
        constexpr uint16_t Height = {size};
        constexpr size_t Length = {len(png)};
        static const uint8_t Png[] PROGMEM = {{
        """
    )

    lines = []
    for i in range(0, len(png), BYTES_PER_LINE):
        chunk = png[i:i + BYTES_PER_LINE]
        lines.append("  " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")

    return header + "\n".join(lines) + "\n};\n\n}  // namespace WebLogo\n"


def main():
    if not SRC.exists():
        sys.exit(f"missing {SRC}")

    img = Image.open(SRC).convert("RGBA")
    print(f"{SRC.relative_to(ROOT)}: {img.width}x{img.height}")

    box = content_square(np.asarray(img)[..., 3])
    png = encode(downscale(img, box, SIZE))
    print(f"  {SIZE}x{SIZE}  {COLORS} colours  {len(png)} bytes")

    DST.write_text(emit(png, SIZE))
    print(f"wrote {DST.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
