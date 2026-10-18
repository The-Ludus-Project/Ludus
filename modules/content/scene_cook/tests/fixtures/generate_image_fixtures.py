"""Regenerate the image-decoder fixtures in images/ (not run by the build).

The fixtures come from independent encoders so the Ludus decoders are checked
against other implementations rather than their own round trip:
  * JPEG: Pillow (libjpeg/libjpeg-turbo), baseline and progressive.
  * OpenEXR: the official OpenEXR Python bindings (pip install OpenEXR numpy).
  * Radiance RGBE: Greg Ward's float-to-RGBE rule from "Real Pixels" (Graphics
    Gems II, 1991), written uncompressed here.
All pixels are synthetic; the files are CC0 like the rest of this directory.

Usage: python3 generate_image_fixtures.py   (run from this directory)
"""

import math
import struct
from pathlib import Path

import numpy
import OpenEXR
from PIL import Image

HERE = Path(__file__).resolve().parent / "images"
WIDTH = 8
HEIGHT = 4


def linear_value(x: int, y: int, channel: int) -> float:
    """Distinct, exactly half-representable values; the last pixel exceeds 1."""
    if (x, y) == (WIDTH - 1, HEIGHT - 1):
        return [100.0, 50.0, 25.0][channel]
    return [x / 8.0, y / 4.0, 0.25 + channel * 0.125][channel]


def write_jpegs() -> None:
    image = Image.new("RGB", (16, 8))
    for y in range(8):
        for x in range(16):
            image.putpixel((x, y), (x * 16, y * 32, 128))
    image.save(HERE / "gradient-baseline.jpg", quality=95, subsampling=0)
    image.save(
        HERE / "gradient-progressive.jpg", quality=95, subsampling=0, progressive=True
    )


def write_exrs() -> None:
    rgb = numpy.zeros((HEIGHT, WIDTH, 3), dtype=numpy.float32)
    for y in range(HEIGHT):
        for x in range(WIDTH):
            for c in range(3):
                rgb[y, x, c] = linear_value(x, y, c)
    variants = [
        (
            "rgb-half-none.exr",
            OpenEXR.NO_COMPRESSION,
            numpy.float16,
            OpenEXR.scanlineimage,
        ),
        (
            "rgb-half-rle.exr",
            OpenEXR.RLE_COMPRESSION,
            numpy.float16,
            OpenEXR.scanlineimage,
        ),
        (
            "rgb-half-zips.exr",
            OpenEXR.ZIPS_COMPRESSION,
            numpy.float16,
            OpenEXR.scanlineimage,
        ),
        (
            "rgb-float-zip.exr",
            OpenEXR.ZIP_COMPRESSION,
            numpy.float32,
            OpenEXR.scanlineimage,
        ),
        (
            "rgb-half-tiled-zip.exr",
            OpenEXR.ZIP_COMPRESSION,
            numpy.float16,
            OpenEXR.tiledimage,
        ),
        (
            "rgb-half-piz.exr",
            OpenEXR.PIZ_COMPRESSION,
            numpy.float16,
            OpenEXR.scanlineimage,
        ),
    ]
    for name, compression, dtype, kind in variants:
        header = {"compression": compression, "type": kind}
        if kind == OpenEXR.tiledimage:
            tiles = OpenEXR.TileDescription()
            tiles.xSize = 3
            tiles.ySize = 3
            tiles.mode = OpenEXR.ONE_LEVEL
            tiles.roundingMode = OpenEXR.ROUND_DOWN
            header["tiles"] = tiles
        channels = {
            "R": numpy.ascontiguousarray(rgb[:, :, 0]).astype(dtype),
            "G": numpy.ascontiguousarray(rgb[:, :, 1]).astype(dtype),
            "B": numpy.ascontiguousarray(rgb[:, :, 2]).astype(dtype),
            "A": numpy.ones((HEIGHT, WIDTH), dtype=dtype),
        }
        with OpenEXR.File(header, channels) as output:
            output.write(str(HERE / name))


def to_rgbe(r: float, g: float, b: float) -> bytes:
    largest = max(r, g, b)
    if largest < 1e-32:
        return bytes(4)
    mantissa, exponent = math.frexp(largest)
    scale = mantissa * 256.0 / largest
    return bytes([int(r * scale), int(g * scale), int(b * scale), exponent + 128])


def write_radiance() -> None:
    data = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n")
    data += f"-Y {HEIGHT} +X {WIDTH}\n".encode("ascii")
    for y in range(HEIGHT):
        for x in range(WIDTH):
            data += to_rgbe(*(linear_value(x, y, c) for c in range(3)))
    (HERE / "ramp.hdr").write_bytes(bytes(data))


if __name__ == "__main__":
    HERE.mkdir(exist_ok=True)
    write_jpegs()
    write_exrs()
    write_radiance()
    for path in sorted(HERE.iterdir()):
        print(path.name, path.stat().st_size)
