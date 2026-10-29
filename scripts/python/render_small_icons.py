"""Render the optically tuned 16, 24 and 32 pixel Ludus icons.

Scaling the 1254 pixel reference PNG down smears the extruded L: the planes
blur into one another and the tile edge turns grey. These sizes are drawn
instead from the reference's own construction (the tile and the five planes of
the L, in reference pixels below), with the shading simplified to the four flat
brand colours of docs/architecture/editor-art-direction.md, every corner snapped
to the pixel grid and every plane anti-aliased by 8x supersampling.

    python3 scripts/python/render_small_icons.py          # write the PNGs
    python3 scripts/python/render_small_icons.py --check  # fail when they are stale

The PNGs go to ``docs/wiki/assets/small`` (the shared artwork source, read by
the Editor's Qt resources) and ``scripts/python/init_assets`` (the setup window).
"""
from __future__ import annotations

import argparse
import sys
from typing import TYPE_CHECKING
from pathlib import Path

if TYPE_CHECKING:
    from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIRECTORIES = (ROOT / "docs" / "wiki" / "assets" / "small", ROOT / "scripts" / "python" / "init_assets")
SIZES = (16, 24, 32)

REFERENCE_SIZE = 1254
SUPERSAMPLE = 8

TILE = "#18232c"
FRONT = "#5dd2c5"
PALE = "#b6f3e8"
SIDE = "#229e95"

# The tile of the reference, in reference pixels, and its corner radius.
TILE_BOX = (87, 105, 1167, 1171)
TILE_RADIUS = 220

# The planes of the L in reference pixels. Neighbours share their corner points exactly, so no seam shows.
PLANES = (
    (FRONT, ((304, 334), (500, 366), (500, 802), (800, 866), (784, 1040), (316, 944))),  # front
    (SIDE, ((500, 366), (656, 282), (656, 722), (500, 802))),                           # stem, right side
    (PALE, ((304, 334), (480, 240), (656, 282), (500, 366))),                           # stem, top
    (PALE, ((500, 802), (656, 722), (940, 778), (800, 866))),                           # foot, top
    (SIDE, ((800, 866), (940, 778), (950, 940), (784, 1040))),                          # foot, end
)

# The L is a little larger than its share of the reference so it still reads at
# 16 pixels; the tile grows by the same margin.
GROW = {16: 1.12, 24: 1.07, 32: 1.04}
# Smaller sizes lose the tile's rounding first.
TILE_RADIUS_SCALE = {16: 0.8, 24: 0.9, 32: 1.0}


def rendered_size(size: int) -> int:
    return size * SUPERSAMPLE


def build(size: int) -> "Image.Image":
    """The icon at ``size`` pixels as an RGBA image (needs Pillow)."""
    from PIL import Image, ImageDraw

    scale = rendered_size(size) / REFERENCE_SIZE
    centre = REFERENCE_SIZE / 2.0
    grow = GROW[size]

    def place(point: tuple[float, float]) -> tuple[float, float]:
        # Reference to target pixels, then snapped to whole target pixels so
        # plane edges fall between pixels instead of straddling them.
        x = (centre + (point[0] - centre) * grow) * scale / SUPERSAMPLE
        y = (centre + (point[1] - centre) * grow) * scale / SUPERSAMPLE
        return round(x) * SUPERSAMPLE, round(y) * SUPERSAMPLE

    canvas = Image.new("RGBA", (rendered_size(size),) * 2, (0, 0, 0, 0))
    draw = ImageDraw.Draw(canvas)
    left, top = place((TILE_BOX[0], TILE_BOX[1]))
    right, bottom = place((TILE_BOX[2], TILE_BOX[3]))
    radius = round(TILE_RADIUS * grow * scale * TILE_RADIUS_SCALE[size] / SUPERSAMPLE) * SUPERSAMPLE
    draw.rounded_rectangle((left, top, right - 1, bottom - 1), radius=radius, fill=TILE)
    for colour, points in PLANES:
        draw.polygon([place(point) for point in points], fill=colour)
    return canvas.resize((size, size), Image.BOX)


def render_all() -> dict[int, "Image.Image"]:
    return {size: build(size) for size in SIZES}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when the committed PNGs differ from a fresh render")
    arguments = parser.parse_args(argv)
    try:
        from PIL import Image
    except ImportError:
        print("render_small_icons needs Pillow (pip install pillow)", file=sys.stderr)
        return 2
    stale = []
    for size, image in render_all().items():
        for directory in OUTPUT_DIRECTORIES:
            path = directory / f"ludus-icon-{size}.png"
            if arguments.check:
                # Compare pixels: the PNG encoding differs between Pillow versions.
                if not path.exists() or Image.open(path).convert("RGBA").tobytes() != image.tobytes():
                    stale.append(str(path.relative_to(ROOT)))
            else:
                directory.mkdir(parents=True, exist_ok=True)
                image.save(path, format="PNG")
    if stale:
        print("stale icons (run scripts/python/render_small_icons.py):\n  " + "\n  ".join(stale), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
