"""Capture the shadow Worlds or analyze existing linear RGBA8 captures. Requires NumPy."""

import argparse
import csv
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess

import numpy as np

from fixture import (
    CENTRE_PROBE_X,
    LIGHT_Z,
    CAMERA_Z,
    EXTENT,
    HALF_VIEW,
    SHIFT_PIXELS,
    cases,
    write_scene,
)
from oracle import (
    compare_then_average,
    disk_geometry,
    physical,
    receiver,
    tap_visibility,
    union_coverage,
    FACE_SIZE,
    TANGENT,
)


FULL_SHADOW_VISIBILITY = 0.05
# Finite disk taps approximate the rectangular emitter, and each cube texel
# retains only the nearest blocker. Compare absolute normalized visibility.
OVERLAP_WEIGHTED_TOLERANCE = 0.15


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def read_frame(directory, name):
    raw = (directory / f"{name}.rgba").read_bytes()
    require(len(raw) == EXTENT**2 * 4, f"{directory}: incomplete {name} readback")
    return np.frombuffer(raw, dtype="u1").reshape(EXTENT, EXTENT, 4)


def capture(root, editor, sdk, graph):
    root.mkdir(parents=True, exist_ok=False)
    for case in cases():
        write_scene(case, root)
    env = dict(
        os.environ,
        QT_QPA_PLATFORM="offscreen",
        LUDUS_SHADOW_CASES=str(root),
        LUDUS_SHADOW_GRAPH=str(graph),
        LUDUS_SHADOW_EXTENT=str(EXTENT),
    )
    subprocess.run([editor, "[.mesh-shadow-world]"], env=env, check=True)
    for case in cases():
        directory = root / case.name
        with (directory / "sdk.log").open("w") as out:
            result = subprocess.run(
                [sdk, graph, str(EXTENT)],
                cwd=directory,
                stdout=out,
                stderr=subprocess.STDOUT,
                check=False,
            )
        if result.returncode:
            raise RuntimeError(
                f"{case.name}: SDK capture failed\n{(directory / 'sdk.log').read_text()}"
            )


def profile(coord, values):
    # Trace the outward edge from the fully lit end. Select the last 90% crossing
    # and the last 10% crossing before it, so a box's other edge cannot give a negative width.
    curve = values.reshape(5, -1).mean(axis=0)
    axis = coord.reshape(5, -1).mean(axis=0)
    order = np.argsort(axis)
    axis, curve = axis[order], curve[order]
    if curve[-1] < 0.9:
        return None
    crossings = []
    limit = len(curve) - 1
    for threshold in (0.9, 0.1):
        upward = np.flatnonzero(
            (curve[:limit] < threshold) & (curve[1 : limit + 1] >= threshold)
        )
        if not upward.size:
            return None
        i = upward[-1]
        crossings.append(float(np.interp(threshold, curve[i : i + 2], axis[i : i + 2])))
        limit = i + 1
    end, start = crossings
    return dict(start=start, end=end, width=end - start)


def analyze_case(root, case, writer):
    directory = root / case.name
    camera = np.fromstring((directory / "camera.txt").read_text(), sep=" ")
    require(camera.size == 19 and np.isfinite(camera).all(), f"{case.name}: camera")
    matrix = camera[:16].reshape(4, 4).T
    require(
        np.allclose(camera[16:], [case.camera_x, 0, CAMERA_Z], atol=2e-4),
        f"{case.name}: saved eye {camera[16:]}",
    )
    # The measured screen positions must refer to the same authored camera as the analytic rays.
    corners = np.array(
        [
            [case.camera_x - HALF_VIEW, -HALF_VIEW, 0, 1],
            [case.camera_x + HALF_VIEW, HALF_VIEW, 0, 1],
        ]
    )
    clip = corners @ matrix.T
    require(
        np.allclose(clip[:, :2] / clip[:, 3, None], [[-1, -1], [1, 1]], atol=2e-4),
        f"{case.name}: saved lens {matrix}",
    )
    images = {route: read_frame(directory, route) for route in ("sdk", "viewport")}
    clear = {route: read_frame(root / case.control.name, route) for route in images}
    origin, radius = disk_geometry(case)
    # Horizontal bands straddle the receiver bend. The third crosses the layer overlap,
    # and the vertical band measures the other emitter axis. Keep the whole pixel row for raw evidence.
    bands = [
        ("lower", int(EXTENT * 0.68)),
        ("upper", int(EXTENT * 0.32)),
        ("centre", EXTENT // 2),
        ("vertical", int(EXTENT * 0.22)),
    ]
    if case.name.startswith("layers-"):
        bands.extend(
            [
                ("near-receiver", int(EXTENT * 0.90)),
                ("near-emitter", int(EXTENT * 0.10)),
            ]
        )
    result = dict(
        scene_sha256=hashlib.sha256(
            (directory / "scene.luscn").read_bytes()
        ).hexdigest(),
        emitter=case.emitter,
        disk_radius_m=radius,
        camera_sha256=hashlib.sha256(
            (directory / "camera.txt").read_bytes()
        ).hexdigest(),
        sdk_device_and_atlas=(directory / "sdk.log").read_text().strip().splitlines(),
        bands={},
    )
    if len(case.plates) == 1 and not case.name.startswith(("overlap", "layers")):
        plate = case.plates[0]
        # Exact full-occlusion and clear limits at the outward edge on each planar axis.
        # A ray from an emitter extremity at e hits b when b=(1-z/H)*receiver+(z/H)*e.
        fraction = plate.z / LIGHT_Z
        result["geometric_edge_bounds_m"] = [
            [
                (boundary + sign * fraction * span / 2) / (1 - fraction)
                for sign in (-1, 1)
            ]
            for span, boundary in zip(case.emitter, (plate.right, plate.top))
        ]
    if len(case.plates) == 2:
        result["central_geometric_visibility"] = float(
            union_coverage((CENTRE_PROBE_X, 0, 0), case)
        )
    temporal = {}
    for name, centre in bands:
        col, row = np.meshgrid(np.arange(EXTENT), np.arange(centre - 2, centre + 3))
        col, row = col.ravel(), row.ravel()
        if name == "vertical":
            col, row = row, col
        points, normals = receiver(case, col, row)
        reference, sampled_coverage = physical(case, points, normals)
        exact = (
            np.array([union_coverage(p, case) for p in points])
            if case.angle is None
            else sampled_coverage
        )
        require(
            np.max(abs(sampled_coverage - exact)) <= 0.05,
            f"{case.name}: ray/clipped coverage disagree",
        )
        taps, pooled, layered = tap_visibility(case, points, normals, col, row)
        coord = points[:, 1 if name == "vertical" else 0]
        # Signed +X versus -Z major-component difference after the filter's receiver offset.
        displaced = (
            points
            - origin
            + normals
            * (
                1.5 * 2 * TANGENT / FACE_SIZE * np.linalg.norm(points - origin, axis=-1)
            )[:, None]
        )
        side = displaced[:, 0] + displaced[:, 2]
        band = dict(
            geometric_10_90=profile(coord, exact),
            weighted_10_90=profile(coord, reference),
            coverage_range=[float(exact.min()), float(exact.max())],
            layers=int(layered.sum()),
        )
        result["bands"][name] = band
        for route in images:
            raw = images[route][row, col, 0].astype(float)
            base = clear[route][row, col, 0].astype(float)
            require(
                np.all((base >= 5) & (base < 255)),
                f"{case.name} {name} {route}: clear codes {base.min()}..{base.max()}",
            )
            observed = raw / base
            error = observed - reference
            if route == "sdk":
                temporal[name] = (error, reference)
            stats = dict(
                raster_10_90=profile(coord, observed),
                physical_rmse=float(np.sqrt(np.mean(error**2))),
                disk_rmse=float(np.sqrt(np.mean((observed - taps) ** 2))),
                pooled_physical_rmse=float(np.sqrt(np.mean((pooled - reference) ** 2))),
                visibility_range=[float(observed.min()), float(observed.max())],
            )
            partial = (reference > 0.02) & (reference < 0.98)
            if partial.any():
                stats["partial"] = dict(
                    probes=int(partial.sum()),
                    split_probes=int((partial & layered).sum()),
                    raster_physical_rmse=float(np.sqrt(np.mean(error[partial] ** 2))),
                    single_mean_physical_rmse=float(
                        np.sqrt(np.mean((pooled[partial] - reference[partial]) ** 2))
                    ),
                )
            if name == "centre":
                middle = np.flatnonzero((col == EXTENT // 2) & (row == EXTENT // 2))[0]
                stats["centre_probe"] = dict(
                    shadow_linear=raw[middle] / 255,
                    clear_linear=base[middle] / 255,
                    visibility=observed[middle],
                    coverage=exact[middle],
                    weighted=reference[middle],
                    disk=taps[middle],
                    pcss=pooled[middle],
                )
            band[route] = stats
            if name == "centre" and case.bent:
                # The saved optical axis and the receiver crease coincide between the central two rows.
                fold = (row == EXTENT // 2 - 1) | (row == EXTENT // 2)
                pair = error[fold].reshape(2, -1)
                stats["bend_excess_max"] = float(np.max(abs(pair[1] - pair[0])))
                stats["bend_excess_rms"] = float(
                    np.sqrt(np.mean((pair[1] - pair[0]) ** 2))
                )
            if name != "vertical":
                # Average across parallel probes only after pairing the adjacent pixels
                # on opposite selected cube faces for each World-space receiver row.
                crosses = np.diff((side >= 0).reshape(5, -1), axis=1)
                require(
                    np.all(np.sum(crosses, axis=1) == 1),
                    f"{case.name}: sweep misses face transition",
                )
                jumps = np.diff(observed.reshape(5, -1), axis=1)[crosses]
                residual = np.diff(error.reshape(5, -1), axis=1)
                curve = observed.reshape(5, -1).mean(axis=0)
                axis = coord.reshape(5, -1).mean(axis=0)
                steepest = int(np.argmax(np.diff(curve)))
                stats["steepest_rise_x_m"] = float(
                    (axis[steepest] + axis[steepest + 1]) / 2
                )
                stats["steepest_rise"] = float(curve[steepest + 1] - curve[steepest])
                disk_jump = np.diff(taps.reshape(5, -1), axis=1)[crosses]
                stats.update(
                    disk_seam_jump=float(disk_jump.mean()),
                    seam_disk_residual=float(np.mean(jumps - disk_jump)),
                    seam_jump=float(np.mean(jumps)),
                    seam_excess=float(residual[crosses].mean()),
                    adjacent_excess_rms=float(
                        np.sqrt(np.mean(residual[~crosses] ** 2))
                    ),
                )
            for i in range(len(points)):
                writer.writerow(
                    (
                        case.name,
                        name,
                        route,
                        col[i],
                        row[i],
                        *points[i],
                        raw[i] / 255,
                        base[i] / 255,
                        observed[i],
                        exact[i],
                        reference[i],
                        taps[i],
                        pooled[i],
                        int(layered[i]),
                        side[i],
                    )
                )
    result["max_route_difference_codes"] = int(
        abs(images["sdk"].astype(int) - images["viewport"]).max()
    )
    return result, temporal


def analyze(root, graph):
    # Per-tap receiver depths deliberately differ. Comparing averaged depths erases the half-shadow.
    plane, stored, weights = np.array([2.0, 4.0]), np.array([1.0, 5.0]), np.ones(2)
    require(
        compare_then_average(plane, stored, weights) == 0.5
        and stored.mean() >= plane.mean(),
        "mixed-depth control",
    )
    report = dict(
        numpy_version=np.__version__,
        extent=EXTENT,
        face_texels=FACE_SIZE,
        gutter_texels=4,
        atlas_texels=2048,
        graph_sha256=hashlib.sha256(graph.read_bytes()).hexdigest(),
        cases={},
    )
    history = {}
    with gzip.open(root / "probes.csv.gz", "wt", compresslevel=4) as out:
        writer = csv.writer(out)
        writer.writerow(
            "case band route column row x_m y_m z_m shadow_linear clear_linear visibility coverage weighted_reference disk_pcf single_mean_pcss layered face_coordinate".split()
        )
        for case in cases():
            result, temporal = analyze_case(root, case, writer)
            report["cases"][case.name] = result
            if case.angle is not None:
                history[case.name] = temporal
    for first, second in zip((41, 43, 45, 47), (43, 45, 47, 49)):
        band_errors = []
        for name in history[f"rotate-{first}"]:
            a, va = history[f"rotate-{first}"][name]
            b, vb = history[f"rotate-{second}"][name]
            partial = ((va > 0.02) & (va < 0.98)) | ((vb > 0.02) & (vb < 0.98))
            if partial.any():
                band_errors.extend(abs(b[partial] - a[partial]))
        require(band_errors, f"{first} to {second}: no partial region")
        report["cases"][f"rotate-{second}"]["largest_temporal_excess"] = float(
            max(band_errors)
        )
    # The camera moves an integer number of pixels across a fixed plane. Compare the
    # same receiver points across both views, including the dithering's new pixel phase.
    report["registered_camera_shift"] = {}
    for route in ("viewport", "sdk"):
        before = read_frame(root / "square", route)[:, SHIFT_PIXELS:, 0].astype(float)
        before /= read_frame(root / "square-clear", route)[:, SHIFT_PIXELS:, 0]
        after = read_frame(root / "shift", route)[:, :-SHIFT_PIXELS, 0].astype(float)
        after /= read_frame(root / "shift-clear", route)[:, :-SHIFT_PIXELS, 0]
        delta = after - before
        report["registered_camera_shift"][route] = dict(
            pixels=SHIFT_PIXELS,
            mean_absolute=float(np.mean(abs(delta))),
            largest_absolute=float(np.max(abs(delta))),
        )
    (root / "measurements.json").write_text(json.dumps(report, indent=2) + "\n")
    for case in cases():
        result = report["cases"][case.name]
        require(
            result["max_route_difference_codes"] <= 2,
            f"{case.name}: native route discrepancy",
        )
        for name, band in result["bands"].items():
            for route in ("sdk", "viewport"):
                require(
                    band[route]["disk_rmse"] < 0.03,
                    f"{case.name} {name} {route}: per-tap RMS error",
                )
                if name != "vertical":
                    require(
                        abs(band[route]["seam_disk_residual"]) < 0.06,
                        f"{case.name} {name} {route}: excess face-boundary step",
                    )
    for name in ("lower", "upper"):
        for angle in (41, 43, 45, 47, 49):
            band = report["cases"][f"rotate-{angle}"]["bands"][name]
            require(
                band["geometric_10_90"] and band["sdk"]["raster_10_90"],
                f"rotate-{angle} {name}: incomplete outward transition",
            )
    for route in ("sdk", "viewport"):
        baseline = report["cases"]["square"]["bands"]["lower"][route][
            "steepest_rise_x_m"
        ]
        for name, move in (("edge-before-seam", -0.1), ("edge-after-seam", 0.1)):
            moved = report["cases"][name]["bands"]["lower"][route]["steepest_rise_x_m"]
            require(
                abs(moved - baseline - move) < 2 * (2 * HALF_VIEW / EXTENT),
                f"{route}: shadow edge did not follow {name}",
            )
    layers = report["cases"]
    require(
        layers["layers-0.88"]["bands"]["lower"]["layers"] == 0
        and layers["layers-1.45"]["bands"]["lower"]["sdk"]["partial"]["split_probes"]
        > 0,
        "depth sweep must exercise both sides of the layer split",
    )
    for i, visibility in enumerate((0.0, 0.25, 0.5)):
        probe = report["cases"][f"overlap-{i}"]["bands"]["centre"]["sdk"][
            "centre_probe"
        ]
        require(
            abs(probe["coverage"] - visibility) < 1e-10,
            f"overlap-{i}: misregistered footprints",
        )
    for name in ("square", "near-receiver", "long-x", "long-y", "shift"):
        band = report["cases"][name]["bands"]["lower"]
        for route in ("sdk", "viewport"):
            low, high = band[route]["visibility_range"]
            require(
                low < FULL_SHADOW_VISIBILITY and high > 0.95 and band[route]["raster_10_90"],
                f"{name}: lit/partial/blocked controls",
            )
    for route in ("sdk", "viewport"):
        probe = report["cases"]["overlap-0"]["bands"]["centre"][route]["centre_probe"]
        require(
            probe["visibility"] < FULL_SHADOW_VISIBILITY,
            f"overlap-0 centre {route}: visibility {probe['visibility']:.3f} must be "
            f"below {FULL_SHADOW_VISIBILITY:.2f} at ({EXTENT // 2}, {EXTENT // 2})",
        )
        for name in (
            "overlap-first",
            "overlap-0-second",
            "overlap-1-second",
            "overlap-2-second",
            "overlap-1",
            "overlap-2",
        ):
            probe = report["cases"][name]["bands"]["centre"][route]["centre_probe"]
            require(
                abs(probe["visibility"] - probe["weighted"]) < OVERLAP_WEIGHTED_TOLERANCE,
                f"{name} centre {route}: visibility {probe['visibility']:.3f} differs from "
                f"weighted reference {probe['weighted']:.3f} by at least "
                f"{OVERLAP_WEIGHTED_TOLERANCE:.2f}",
            )
        require(
            np.array_equal(
                read_frame(root / "rotate-45", route),
                read_frame(root / "static-repeat", route),
            ),
            f"{route}: static caster is unstable",
        )
    return report


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("root", type=Path)
    p.add_argument("graph", type=Path)
    p.add_argument("--editor", type=Path)
    p.add_argument("--sdk", type=Path)
    args = p.parse_args()
    if bool(args.editor) != bool(args.sdk):
        p.error("supply both capture binaries")
    root, graph = args.root.resolve(), args.graph.resolve()
    if args.editor:
        capture(root, args.editor.resolve(), args.sdk.resolve(), graph)
    analyze(root, graph)
