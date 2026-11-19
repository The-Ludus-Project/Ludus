"""Run the installed renderer and compare scene-linear probes with independent oracles."""

import argparse
import json
import math
import os
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from fixture import (
    CENTER,
    FEATURES,
    SIZE,
    analytic,
    basis,
    direction,
    dot,
    radiance,
    reflection,
    unit,
    write_map,
    write_plane,
)


def check(label, actual, expected, tolerance):
    if len(actual) != len(expected) or any(
        not math.isfinite(a) or abs(a - b) > tolerance for a, b in zip(actual, expected)
    ):
        raise ValueError(
            f"{label}: measured {actual}, expected {expected} +/- {tolerance}"
        )


def capture(executable, root, normal, view, *, width=64, samples=128, **surface):
    write_plane(root, normal, view, **surface)
    result = subprocess.run(
        [executable],
        cwd=root,
        input=f"{width} {samples}\n",
        text=True,
        timeout=150,
        check=True,
        stdout=subprocess.PIPE,
    )
    print(
        result.stdout.strip(),
        "normal",
        normal,
        "view",
        view,
        "filter",
        width,
        samples,
        flush=True,
    )
    image = list(struct.iter_unpack("=fff", (root / "linear.rgb").read_bytes()))
    if len(image) != SIZE * SIZE:
        raise ValueError("Incomplete linear readback")
    return image


def probe(image, x=CENTER, y=CENTER, radius=0):
    values = [
        image[j * SIZE + i]
        for j in range(y - radius, y + radius + 1)
        for i in range(x - radius, x + radius + 1)
    ]
    return tuple(sum(channel) / len(values) for channel in zip(*values))


def cooked(root):
    w, h = map(int, (root / "dimensions.txt").read_text().split())
    pixels = list(struct.iter_unpack("=fff", (root / "cooked.rgb").read_bytes()))
    if len(pixels) != w * h:
        raise ValueError("Incomplete cooked environment")
    return w, h, pixels


def quadrature(root, subdivisions=1):
    w, h, pixels = cooked(root)
    rows = h * subdivisions
    columns = w * subdivisions
    for j in range(rows):
        area = (
            math.tau
            / columns
            * (math.cos(math.pi * j / rows) - math.cos(math.pi * (j + 1) / rows))
        )
        for i in range(columns):
            yield (
                direction((i + 0.5) / columns, (j + 0.5) / rows),
                area,
                pixels[j // subdivisions * w + i // subdivisions],
            )


def directional(executable, root):
    write_map(root, "bracket", 128)
    previous = []
    for roll in (False, True):
        n = (1, 0, 0)
        image = capture(executable, root, n, n, roll=roll)
        for i, offset in enumerate((-18, -2, 2, 18)):
            x, y = (CENTER, CENTER + offset) if roll else (CENTER + offset, CENTER)
            point, ray = reflection(x, y, n, n, roll)
            expected = radiance("bracket", ray)
            measured = probe(image, x, y, 1)
            # Broad features, finite GGX footprint and DFG attenuation at the mirror floor.
            check(
                f"mirror at {point}", measured, tuple(0.96 * c for c in expected), 0.30
            )
            if roll:
                check("rolled world point", point, previous[i][0], 1e-12)
                check("rolled radiance", measured, previous[i][1], 0.035)
            else:
                previous.append((point, measured))
    check("longitude continuity", previous[1][1], previous[2][1], 0.035)
    north = (0, 1, 0)
    check(
        "north",
        probe(capture(executable, root, north, north), radius=1),
        tuple(0.96 * c for c in FEATURES[-1][2]),
        0.22,
    )
    w, h, pixels = cooked(root)
    if (w, h) != (128, 64):
        raise ValueError("Unexpected bracket resolution")
    for axis, _, expected in FEATURES:
        x, y, z = unit(axis)
        i = int((math.atan2(z, x) / math.tau % 1) * w)
        j = min(int(math.acos(y) / math.pi * h), h - 1)
        # Relative to the red channel of the dark background.
        reference = pixels[h // 2 * w + w // 2][0]
        check(
            "bracket exposure ratio",
            tuple(c / reference for c in pixels[j * w + i]),
            tuple(c / 0.125 for c in expected),
            0.4,
        )


def diffuse(executable, root):
    normals = [
        (1, 0, 0),
        (-1, 0, 0),
        (0, 1, 0),
        (0, -1, 0),
        (0, 0, 1),
        (0, 0, -1),
        (1, 1, 0),
        (1, -1, 0),
        (0, 1, 1),
        (0, 1, -1),
        (1, 0, 1),
        (-1, 0, 1),
    ]
    for mode, width in [("constant", 32), ("analytic", 32), ("analytic", 128)]:
        write_map(root, mode, width)
        grid = None
        projection_error = 0.0
        raster_error = 0.0
        for axis in normals if mode == "analytic" else normals[:1]:
            n = unit(axis)
            # Subtraction cancels the dielectric's base-colour-independent specular term.
            bright = probe(capture(executable, root, n, n, metallic=0, white=1))
            dark = probe(capture(executable, root, n, n, metallic=0, white=0))
            if grid is None:
                grid = list(quadrature(root))
            expected = analytic(n, 2 / 3, 1 / 4) if mode == "analytic" else (1, 1, 1)
            # Irradiance / pi, integrated on the cooked lat/long cells in binary64.
            projected = [
                sum(
                    rgb[k] * area * max(0, dot(n, l)) / math.pi for l, area, rgb in grid
                )
                for k in range(3)
            ]
            projection_error = max(
                projection_error, *(abs(a - b) for a, b in zip(projected, expected))
            )
            raster_error = max(
                raster_error,
                *(abs(a - b - 0.96 * c) for a, b, c in zip(bright, dark, expected)),
            )
            check(f"{width} projection {axis}", projected, expected, 0.015)
            check(
                f"{width} matte {axis}",
                tuple(a - b for a, b in zip(bright, dark)),
                tuple(0.96 * x for x in expected),
                0.048,
            )
        print(
            mode,
            width,
            "diffuse max error: projection",
            projection_error,
            "material",
            raster_error,
        )


def integrate_prefilter(grid, n, roughness):
    # The GGX half-vector density D(H)*N.H and reflection Jacobian 1/(4*V.H)
    # reduce to D(H)/4 for V=N. Integrate the cosine weight and its normalization.
    alpha2 = roughness**4
    total = [0.0, 0.0, 0.0]
    weight = 0.0
    for l, area, rgb in grid:
        nl = dot(n, l)
        if nl > 0:
            nh2 = (1 + nl) / 2
            kernel = area * nl * alpha2 / (4 * math.pi * (1 + (alpha2 - 1) * nh2) ** 2)
            weight += kernel
            for k in range(3):
                total[k] += kernel * rgb[k]
    return tuple(c / weight for c in total)


def integrate_direct(source, n, v, roughness, steps):
    # Binary64 midpoint quadrature in half-vector coordinates resolves narrow lobes
    # at grazing angles. Integrating over the GGX CDF cancels D(H)*N.H.
    width, height, pixels = source
    tangent, bitangent = basis(n)
    alpha2 = roughness**4
    nv = dot(n, v)
    total = [0.0, 0.0, 0.0]
    ring = [
        (
            math.cos(math.tau * (i + 0.5) / (2 * steps)),
            math.sin(math.tau * (i + 0.5) / (2 * steps)),
        )
        for i in range(2 * steps)
    ]
    for j in range(steps):
        q = (j + 0.5) / steps
        nh = math.sqrt((1 - q) / (1 + (alpha2 - 1) * q))
        radius = math.sqrt(1 - nh * nh)
        for cos_phi, sin_phi in ring:
            half = tuple(
                nh * a + radius * (cos_phi * b + sin_phi * c)
                for a, b, c in zip(n, tangent, bitangent)
            )
            vh = dot(v, half)
            nl = 2 * vh * nh - nv
            if nl <= 0 or vh <= 0:
                continue
            x, y, z = (2 * vh * a - b for a, b in zip(half, v))
            col = int((math.atan2(z, x) / math.tau % 1) * width) % width
            row = min(height - 1, int(math.acos(max(-1, min(1, y))) / math.pi * height))
            rgb = pixels[row * width + col]
            smith = 0.5 / (
                nl * math.sqrt(alpha2 + (1 - alpha2) * nv * nv)
                + nv * math.sqrt(alpha2 + (1 - alpha2) * nl * nl)
            )
            weight = 4 * vh * nl * smith / nh
            for k in range(3):
                total[k] += rgb[k] * weight
    return tuple(c / (2 * steps * steps) for c in total)


def glossy(executable, root):
    for mode in ("constant", "glossy"):
        write_map(root, mode, 128)
        grid = None
        fine_grid = None
        for width in (32, 64):
            for mip in (1, 2):
                mip_width = width >> mip
                # Address an exact texel centre at mip LOD, near the compact patch.
                reflected = direction(
                    0.5 / mip_width, (mip_width // 4 + 0.5) / (mip_width // 2)
                )
                rough = mip / math.log2(width)
                for samples in (32, 256):
                    rendered = probe(
                        capture(
                            executable,
                            root,
                            reflected,
                            reflected,
                            roughness=rough,
                            width=width,
                            samples=samples,
                        )
                    )
                    if grid is None:
                        grid = list(quadrature(root, 2))
                        fine_grid = list(quadrature(root, 4))
                    if samples == 32:
                        exact = integrate_prefilter(fine_grid, reflected, rough)
                        check(
                            "prefilter quadrature convergence",
                            integrate_prefilter(grid, reflected, rough),
                            exact,
                            0.012,
                        )
                    # For a unit white conductor the two terms of the published Lazarov
                    # fit sum to (1 - .55*r). Divide it out to observe the stored mip.
                    dfg = 1 - 0.55 * rough
                    measured = tuple(x / dfg for x in rendered)
                    print(
                        mode,
                        width,
                        mip,
                        samples,
                        "prefilter max error",
                        max(abs(a - b) for a, b in zip(measured, exact)),
                        flush=True,
                    )
                    check(
                        "stored mip",
                        measured,
                        exact,
                        (0.18 if samples == 32 else 0.085)
                        if mode == "glossy"
                        else 0.012,
                    )
                tangent, _ = basis(reflected)
                for cosine in (1, 0.30):
                    # Hold R fixed while changing N and V: rotating N by a yields V at 2a.
                    sine = math.sqrt(1 - cosine * cosine)
                    n = tuple(cosine * x + sine * t for x, t in zip(reflected, tangent))
                    v = tuple(2 * cosine * a - r for a, r in zip(n, reflected))
                    image = capture(
                        executable,
                        root,
                        n,
                        v,
                        roughness=rough,
                        width=width,
                        samples=256,
                    )
                    final = probe(image)
                    source = cooked(root)
                    direct = integrate_direct(source, n, v, rough, 128)
                    check(
                        "direct quadrature convergence",
                        integrate_direct(source, n, v, rough, 64),
                        direct,
                        0.035,
                    )
                    model = tuple(dfg * c for c in exact)
                    error = tuple(a - b for a, b in zip(model, direct))
                    print(
                        mode,
                        width,
                        rough,
                        cosine,
                        "split-sum minus direct",
                        error,
                        "raster minus direct",
                        tuple(a - b for a, b in zip(final, direct)),
                        flush=True,
                    )
                    # The narrow patch seen at grazing incidence has the largest
                    # split-sum bias. Bound that approximation independently of mip error.
                    direct_tolerance = 0.65 if cosine < 1 and mode == "glossy" else 0.32
                    check(
                        "single-scatter direct integral",
                        final,
                        direct,
                        direct_tolerance,
                    )
                    check("split-sum raster", final, model, 0.18)


def editor_capture(editor, root, probes):
    if editor is None:
        return
    (root / "probes.txt").write_text(
        str(len(probes)) + "\n" + "".join(f"{x} {y}\n" for x, y in probes)
    )
    subprocess.run(
        [editor, "[.environment-qualification]"],
        cwd=root,
        timeout=150,
        check=True,
        env={
            **os.environ,
            "LUDUS_ENVIRONMENT_FIXTURE": str(root),
            "QT_QPA_PLATFORM": "offscreen",
            "LUDUS_EDITOR_VIEWPORT": "disabled",
        },
    )


def world(executable, root, editor):
    # Match the native World's 64-column, 16-sample environment preparation.
    for mode, width, n, v, rough, metal, roll in (
        ("bracket", 128, (1, 0, 0), (1, 0, 0), 0.045, 1, False),
        ("bracket", 128, (1, 0, 0), (1, 0, 0), 0.045, 1, True),
        ("bracket", 128, (0, 1, 0), (0, 1, 0), 0.045, 1, False),
        ("analytic", 32, (1, -1, 0), (1, -1, 0), 1, 0, False),
        ("analytic", 128, (0, 1, 1), (0, 1, 1), 1, 0, False),
        ("constant", 32, (0, 0, 1), (0, 0, 1), 1, 0, False),
        ("glossy", 128, (1, 0, 0), (1, 0, 0), 0.33, 1, False),
        ("glossy", 128, (1, 0, 0), (0.3, 0, 0.954), 0.4, 1, False),
    ):
        write_map(root, mode, width)
        capture(
            executable,
            root,
            n,
            v,
            width=64,
            samples=16,
            roughness=rough,
            metallic=metal,
            roll=roll,
        )
        probes = [(CENTER, i) if roll else (i, CENTER) for i in (14, 30, 34, 50)]
        editor_capture(editor, root, probes)


def photograph(executable, root, prepared, editor):
    report = json.loads((prepared / "capture.json").read_text())
    shutil.copyfile(prepared / "environment.hdr", root / "environment.hdr")
    width, height = report["width"], report["height"]
    encoded = (
        (root / "environment.hdr").read_bytes().split(f"+X {width}\n".encode(), 1)[1]
    )

    def source(x, y):
        at = (y * width + x) * 4
        return math.ldexp(encoded[at + 1], encoded[at + 3] - 136)

    results = []
    source_values = []
    for p in report["probes"]:
        x, y = p["x"], p["y"]
        n = direction((x + 0.5) / width, (y + 0.5) / height)
        image = capture(executable, root, n, n, width=64, samples=16)
        w, h, pixels = cooked(root)
        if (w, h) != (width // 2, height // 2):
            raise ValueError(f"Expected one box reduction, got {w}x{h}")
        measured = pixels[y // 2 * w + x // 2][1]
        check(
            p["name"] + " box average",
            [measured],
            [sum(source(x + i, y + j) for i in range(2) for j in range(2)) / 4],
            1e-6,
        )
        results.append(measured)
        source_values.append(source(x, y))
        # Read the same region through the reopened World and its Lit viewport.
        editor_capture(editor, root, [(CENTER, CENTER)])
        check(
            p["name"] + " reflected green",
            [probe(image)[1]],
            [measured],
            0.17 * measured + 0.0002,
        )
        print(
            p["name"],
            "source green",
            source(x, y),
            "cooked",
            measured,
            "reflected RGB",
            probe(image),
        )
    for stage, values in (("source", source_values), ("cook", results)):
        for p, value in zip(report["probes"], values):
            check(
                f"{stage} {p['name']} photographic ratio",
                [value / values[1]],
                [p["ratio"]],
                0.25 * p["ratio"],
            )
    peak = max(pixel[1] for pixel in pixels)
    integral = 0.0
    for y in range(h):
        row = pixels[y * w : (y + 1) * w]
        area = (
            math.tau / w * (math.cos(math.pi * y / h) - math.cos(math.pi * (y + 1) / h))
        )
        integral += area * sum(p[1] for p in row[: w // 16] + row[-w // 16 :])
    if peak > report["peak"] + 1e-6 or peak <= 0:
        raise ValueError("Invalid peak after box reduction")
    check(
        "seam strip integrated radiance",
        [integral],
        [report["seam_integral"]],
        0.002 * integral,
    )
    print(
        f"{width}x{height} to {w}x{h}; green peak {report['peak']} to {peak}; "
        f"seam integral {report['seam_integral']} to {integral}"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("consumer", type=Path)
    parser.add_argument(
        "--case", choices=["directional", "diffuse", "glossy", "world", "photograph"]
    )
    parser.add_argument(
        "--editor",
        type=Path,
        help="also exercise the World picker, save/reopen and Lit viewport",
    )
    parser.add_argument(
        "--capture", type=Path, help="directory produced by prepare_capture.py"
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        for run in (directional, diffuse, glossy):
            if args.case in (None, run.__name__):
                run(args.consumer.resolve(), Path(folder))
        editor = args.editor.resolve() if args.editor else None
        if args.case == "world" and editor is None:
            parser.error("world requires --editor")
        if editor and args.case in (None, "world"):
            world(args.consumer.resolve(), Path(folder), editor)
        if args.case == "photograph" and args.capture is None:
            parser.error("photograph requires --capture")
        if args.capture and args.case in (None, "photograph"):
            photograph(args.consumer.resolve(), Path(folder), args.capture, editor)


if __name__ == "__main__":
    main()
