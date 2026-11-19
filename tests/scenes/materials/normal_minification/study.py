"""Run the same requests through the SDK or World viewport and measure the exported radiance."""
import argparse
import json
import math
import os
from pathlib import Path
import statistics
import struct
import subprocess

import numpy as np

from fixture import PATTERNS, PERIOD, SCALES, sample

EXTENT = 64
FRAME_RECORD = np.dtype([("ns", "<f8"), ("rgba", "u1", (EXTENT, EXTENT, 4))])
FOV = 0.7
TEXELS_PER_METRE = 32
AZIMUTHS = (0, 45, 90)
LIGHT_ANGLES = tuple(range(-54, 55, 3))
# Thanks to Becker and Max, "Smooth Transitions Between Bump Rendering Algorithms"
# (SIGGRAPH 1993, pp. 183–190), for the reflected-intensity continuity criterion.
FOOTPRINTS = tuple(2 ** (0.5 + step / 16) for step in range(17))


def requests(pattern):
    for scale in (SCALES if pattern in ("flat", "wave") else (1,)):
        # Recenter each binding so that the viewing and lighting directions match.
        origin = (scale - 1) * 3
        for phase in (0.125, 0.625):
            for footprint in FOOTPRINTS:
                z = footprint * EXTENT / (2 * TEXELS_PER_METRE * math.tan(FOV / 2))
                x = origin + phase / TEXELS_PER_METRE
                yield ("distance", scale, phase, footprint), (x, 0, z, x, 0, 0, 0, 0, 1)
        for azimuth in AZIMUTHS:
            phi = math.radians(azimuth)
            cx, cy = math.cos(phi), math.sin(phi)
            # A footprint wider than the eight-texel period exposes the limiting
            # filtered summaries. Keep the oblique view fixed across azimuths.
            for angle in LIGHT_ANGLES:
                theta = math.radians(angle)
                yield ("angle", scale, azimuth, angle), (
                    origin + 3.6 * cx, 3.6 * cy, 24, origin, 0, 0,
                    math.sin(theta) * cx, math.sin(theta) * cy, math.cos(theta))


def verify_patterns():
    measured = {}
    for name in PATTERNS:
        slopes = []
        for y in range(PERIOD):
            for x in range(PERIOD):
                packed = sample(name, x, y)[0]
                xy = np.array(packed[:2]) * (2 / 255) - 1
                slopes.append(xy / math.sqrt(1 - xy @ xy))
        slopes = np.array(slopes)
        centred = slopes - slopes.mean(axis=0)
        measured[name] = dict(mean=slopes.mean(axis=0).tolist(), covariance=(centred.T @ centred / len(slopes)).tolist(),
                              fourth=float(np.mean(np.sum(centred**2, axis=1)**2)))
    limits = dict(mean=0.005, covariance=0.002)
    for field, tol in limits.items():
        if not np.allclose(measured["tails"][field], measured["shoulders"][field], atol=tol, rtol=0):
            raise ValueError(f"packed {field} differs between lobe shapes")
    if measured["tails"]["fourth"] <= 2 * measured["shoulders"]["fourth"]:
        raise ValueError("packed distributions lost their distinct tails")
    if not np.allclose(measured["tilt"]["mean"], measured["tilt-wave"]["mean"], atol=limits["mean"], rtol=0):
        raise ValueError("tilted control lost its matched mean")
    a = np.array(measured["axis-x"]["covariance"])
    b = np.array(measured["axis-y"]["covariance"])
    if not np.allclose(a[::-1, ::-1], b, atol=limits["covariance"], rtol=0) or a[0, 0] < 8 * a[1, 1]:
        raise ValueError("rotated axes do not survive packing")
    isotropic = np.array(measured["isotropic"]["covariance"])
    if (abs(isotropic[0, 0] - isotropic[1, 1]) > limits["covariance"]
            or abs(isotropic[0, 1]) > limits["covariance"]
            or abs(isotropic.trace() - a.trace()) > 2 * limits["covariance"]):
        raise ValueError("isotropic control lost its matched radial variance")
    def joint_samples(name):
        return [(normal[:2], scalars[1:]) for y in range(PERIOD) for x in range(PERIOD)
                for normal, _, scalars in [sample(name, x, y)]]
    left = joint_samples("smooth-left")
    right = joint_samples("smooth-right")
    for channel in (0, 1):
        if sorted(pair[channel] for pair in left) != sorted(pair[channel] for pair in right):
            raise ValueError("correlation controls lost their shared marginal samples")
    if sorted(left) == sorted(right):
        raise ValueError("correlation controls lost their distinct joint samples")
    return dict(tolerances=limits, measurements=measured)


def metrics(image):
    # Common central angular window. It lies inside the intended patch for every distance.
    patch = image[24:40, 24:40]
    energy = float(patch.sum())
    if not math.isfinite(energy) or energy <= 0:
        raise ValueError("missing or invalid scene-linear highlight")
    y, x = np.indices(patch.shape)
    centre = np.array([(patch * x).sum(), (patch * y).sum()]) / energy
    width = math.sqrt(float((patch * ((x - centre[0])**2 + (y - centre[1])**2)).sum()) / energy)
    # Use the centroid of tied peak pixels so a flat plateau has a stable position.
    peak = np.mean(np.argwhere(patch == patch.max()), axis=0)[::-1]
    return dict(energy=energy, centre=centre.tolist(), width=width, peak=peak.tolist(),
                centre_radiance=float(image[31:33, 31:33].mean()))


def measure(path, keys):
    frames = np.memmap(path, dtype=FRAME_RECORD, mode="r")
    if len(frames) != len(keys) or path.stat().st_size != FRAME_RECORD.itemsize * len(keys):
        raise ValueError(f"incomplete capture {path}")
    rgb = frames["rgba"].astype(np.uint32)
    linear = ((rgb[..., 0] << 24) | (rgb[..., 1] << 16) | (rgb[..., 2] << 8)).view(np.float32)
    return [(metrics(image), float(ns)) for image, ns in zip(linear, frames["ns"])]


def summarize(keys, rows, reference):
    times = [ns for _, ns in rows[3:] if ns >= 0]
    reference_times = [ns for _, ns in reference[3:] if ns >= 0]
    report = dict(gpu_frame_median_us=statistics.median(times) / 1000 if times else None,
                  reference_gpu_frame_median_us=statistics.median(reference_times) / 1000 if reference_times else None,
                  gpu_timed_frames=len(times), distance={}, angular={})
    for scale in sorted({k[1] for k in keys}):
        for phase in (0.125, 0.625):
            selected = [i for i, k in enumerate(keys) if k[:3] == ("distance", scale, phase)]
            errors = {}
            for field in ("energy", "centre", "width", "peak"):
                measured = np.array([rows[i][0][field] for i in selected])
                matched = np.array([reference[i][0][field] for i in selected])
                delta = measured - matched
                length = np.abs if delta.ndim == 1 else lambda v: np.linalg.norm(v, axis=1)
                # Exclude the exact mip level; all other samples blend levels.
                interior = np.array([abs(math.log2(keys[i][3]) - round(math.log2(keys[i][3]))) > 1e-7 for i in selected])
                errors[field] = dict(max_blend_error=float(length(delta)[interior].max()),
                                     max_adjacent_excess=float(length(np.diff(delta, axis=0)).max()))
            report["distance"][f"{scale}/{phase}"] = errors
        for azimuth in AZIMUTHS:
            selected = [i for i, k in enumerate(keys) if k[:3] == ("angle", scale, azimuth)]
            values = np.array([rows[i][0]["centre_radiance"] for i in selected])
            matched = np.array([reference[i][0]["centre_radiance"] for i in selected])
            angles = np.array(LIGHT_ANGLES)
            def shape(profile):
                centre = np.average(angles, weights=profile)
                return dict(centre_deg=float(centre), peak_deg=float(angles[profile.argmax()]),
                            rms_width_deg=float(np.sqrt(np.average((angles - centre)**2, weights=profile))),
                            integral=float(np.trapezoid(profile, np.deg2rad(angles))))
            shoulders = (np.abs(angles) >= 18) & (np.abs(angles) <= 42)
            report["angular"][f"{scale}/{azimuth}"] = dict(fit=shape(values), reference=shape(matched),
                    max_shoulder_error=float(np.max(np.abs(values[shoulders] - matched[shoulders]))),
                    radiance=values.tolist(), supersampled=matched.tolist())
    return report


def check_controls(pattern, keys, captures):
    for route in ("runtime", "cooker"):
        baseline = captures[route, "baseline"]
        candidate = captures[route, "candidate"]
        for i, key in enumerate(keys):
            # With a constant normal or zero strength the BRDF must be identical.
            # The zero-strength binding is surrounded by active bindings, hence
            # compare the measured patch instead of unrelated pixels in its frame.
            if pattern in ("flat", "tilt") or key[1] == 0:
                first = baseline[i][0]
                second = candidate[i][0]
                if key[0] == "angle":
                    first = first["centre_radiance"]
                    second = second["centre_radiance"]
                if first != second:
                    raise ValueError(f"{route}/{pattern}/{key}: candidate changed a control")


def capture(host, executable, cooked, package, request_file, destination):
    if host == "sdk":
        subprocess.run(
            [executable, cooked, package.parent, package.name, request_file, destination],
            check=True,
        )
    else:
        env = dict(
            os.environ,
            LUDUS_MINIFICATION_SCENE=str(cooked / "scene.luscn"),
            LUDUS_MINIFICATION_GRAPH=str(package),
            LUDUS_MINIFICATION_REQUESTS=str(request_file),
            LUDUS_MINIFICATION_FRAMES=str(destination),
        )
        subprocess.run([executable, "[.normal-minification]"], env=env, check=True)


def run(args):
    root = args.directory.resolve()
    output = root / args.host
    output.mkdir(exist_ok=True)
    result = dict(host=args.host, packed=verify_patterns(), cases={})
    if args.compare_hosts:
        result["host_comparison"] = {"frames": 0, "different_frames": 0}
    for pattern in args.pattern or PATTERNS:
        if args.host == "sdk":
            packed = output / (pattern + ".rgba")
            subprocess.run(
                [args.executable, "--read-packed", root / "runtime" / pattern / "cooked", packed],
                check=True,
            )
            expected = bytes(
                channel for y in range(EXTENT) for x in range(EXTENT)
                for normal, _, scalars in [sample(pattern, x, y)]
                for channel in (*normal[:2], scalars[1], scalars[2])
            )
            if packed.read_bytes() != expected:
                raise ValueError(f"{pattern}: cooked RG normals or BA material channels differ from the authored tile")
        pairs = list(requests(pattern))
        keys = [key for key, _ in pairs]
        request_file = output / (pattern + ".requests")
        encoded = b"".join(struct.pack("<9f", *request) for _, request in pairs)
        if args.measure_existing:
            if request_file.read_bytes() != encoded:
                raise ValueError(f"stored requests differ from this sweep: {request_file}")
        else:
            request_file.write_bytes(encoded)

        captures = {}
        for route in ("runtime", "cooker"):
            cooked = root / route / pattern / "cooked"
            methods = ("reference", "baseline", "candidate") if route == "runtime" else ("baseline", "candidate")
            for method in methods:
                package = root / (method + ".lurgrt")
                dest = output / f"{route}-{pattern}-{method}.frames"
                if not args.measure_existing:
                    capture(args.host, args.executable, cooked, package, request_file, dest)
                if args.compare_hosts:
                    other = "sdk" if args.host == "world" else "world"
                    other_directory = root / other
                    if (other_directory / request_file.name).read_bytes() != encoded:
                        raise ValueError(f"{pattern}: hosts used different frame requests")
                    first = np.memmap(dest, dtype=FRAME_RECORD, mode="r")["rgba"]
                    second = np.memmap(other_directory / dest.name, dtype=FRAME_RECORD, mode="r")["rgba"]
                    if first.shape != second.shape:
                        raise ValueError(f"{dest.name}: host frame counts differ")
                    result["host_comparison"]["frames"] += len(first)
                    result["host_comparison"]["different_frames"] += int(np.any(first != second, axis=(1, 2, 3)).sum())
                rows = measure(dest, keys)
                captures[route, method] = rows
                if method != "reference":
                    result["cases"][f"{route}/{pattern}/{method}"] = summarize(
                        keys, rows, captures["runtime", "reference"]
                    )
        check_controls(pattern, keys, captures)
        print(pattern, flush=True)

    name = "report-" + "-".join(args.pattern) + ".json" if args.pattern else "report.json"
    (output / name).write_text(json.dumps(result, indent=2, allow_nan=False) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", choices=["sdk", "world"])
    parser.add_argument("directory", type=Path)
    parser.add_argument("executable", type=Path)
    parser.add_argument("--pattern", action="append", choices=PATTERNS)
    parser.add_argument("--measure-existing", action="store_true", help="analyze previously captured frames")
    parser.add_argument("--compare-hosts", action="store_true", help="compare exported pixels with the other host’s existing frames")
    run(parser.parse_args())
