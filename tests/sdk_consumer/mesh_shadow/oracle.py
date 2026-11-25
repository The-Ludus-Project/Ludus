"""Independent rectangular-emitter geometry, ray/box tests, and cube depth comparisons."""

import math

import numpy as np

from fixture import BEND, BOX_CENTER, BOX_HALF, CAMERA_Z, EXTENT, HALF_VIEW, LIGHT_Z

FACE_SIZE = 512
GUTTER = 4
TANGENT = FACE_SIZE / (FACE_SIZE - 2 * GUTTER)
GOLDEN = math.pi * (3 - math.sqrt(5))
WIDE_SEARCH_RINGS = 4
COMPACT_SEARCH_RADII = (0.5, 0.25)
SEARCH_SPOKES = 3


def receiver(case, columns, rows):
    """Intersect saved pinhole rays with either face of the receiver."""
    x = ((columns + 0.5) * 2 / EXTENT - 1) * HALF_VIEW
    y = (1 - (rows + 0.5) * 2 / EXTENT) * HALF_VIEW
    slope = np.where((y > 0) & case.bent, BEND, 0)
    scale = CAMERA_Z / (CAMERA_Z - slope * y)
    points = np.stack(
        [case.camera_x + x * scale, y * scale, -slope * y * scale], axis=-1
    )
    normals = np.stack([0 * slope, slope, np.ones_like(slope)], axis=-1)
    return points, normals / np.linalg.norm(normals, axis=-1, keepdims=True)


def footprint(point, emitter, plate):
    """Clip the blocker projected from the receiver to the light's rectangle."""
    x, y, z = point
    w, h = emitter
    scale = (LIGHT_Z - z) / (plate.z - z)
    return (
        max(-w / 2, x + (plate.left - x) * scale),
        min(w / 2, x + (plate.right - x) * scale),
        max(-h / 2, y + (plate.bottom - y) * scale),
        min(h / 2, y + (plate.top - y) * scale),
    )


def intersection(a, b):
    return (max(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), min(a[3], b[3]))


def area(rect):
    return max(0, rect[1] - rect[0]) * max(0, rect[3] - rect[2])


def union_coverage(point, case):
    rectangles = [footprint(point, case.emitter, p) for p in case.plates]
    total = sum(map(area, rectangles))
    if len(rectangles) == 2:
        total -= area(intersection(*rectangles))
    return np.clip(1 - total / math.prod(case.emitter), 0, 1)


def hit(case, origin, direction):
    """First positive ray parameter at a blocker. Slabs are exact for the oriented box."""
    result = np.full(direction.shape[:-1], np.inf)
    for plate in case.plates:
        t = (plate.z - origin[..., 2]) / direction[..., 2]
        x = origin[..., 0] + t * direction[..., 0]
        y = origin[..., 1] + t * direction[..., 1]
        inside = (
            (x >= plate.left)
            & (x <= plate.right)
            & (y >= plate.bottom)
            & (y <= plate.top)
            & (t > 0)
        )
        result = np.minimum(result, np.where(inside, t, np.inf))
    if case.angle is not None:
        theta = math.radians(case.angle)
        c, s = math.cos(theta), math.sin(theta)
        basis = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
        start = (origin - BOX_CENTER) @ basis
        ray = direction @ basis
        with np.errstate(divide="ignore", invalid="ignore"):
            a = (-np.array(BOX_HALF) - start) / ray
            b = (np.array(BOX_HALF) - start) / ray
        parallel = abs(ray) < 1e-12
        enter = np.where(parallel, -np.inf, np.minimum(a, b)).max(axis=-1)
        leave = np.where(parallel, np.inf, np.maximum(a, b)).min(axis=-1)
        misses = (parallel & (abs(start) > BOX_HALF)).any(axis=-1)
        result = np.where((~misses) & (leave >= enter) & (enter > 0), enter, np.inf)
    return result


def physical(case, points, normals, side=48):
    """Integrate the visible emitter with both cosines and inverse-square falloff."""
    u = (np.arange(side) + 0.5) / side - 0.5
    x, y = np.meshgrid(u * case.emitter[0], u * case.emitter[1])
    light = np.stack([x.ravel(), y.ravel(), np.full(side * side, LIGHT_Z)], axis=-1)
    visibilities, coverages = [], []
    for start in range(0, len(points), 32):
        p = points[start : start + 32, None, :]
        ray = light - p
        weights = (
            np.maximum((normals[start : start + 32, None, :] * ray).sum(axis=-1), 0)
            * ray[..., 2]
            / (ray * ray).sum(axis=-1) ** 2
        )
        clear = hit(case, p, ray) >= 1
        visibilities.extend((weights * clear).sum(axis=1) / weights.sum(axis=1))
        coverages.extend(clear.mean(axis=1))
    return np.array(visibilities), np.array(coverages)


def disk_geometry(case):
    enclosure = math.hypot(*case.emitter) / 2
    offset = max(0.01, 0.05 * enclosure)
    return np.array([0, 0, LIGHT_Z - offset]), math.sqrt(
        math.prod(case.emitter) / math.pi
    )


def texel_ray(directions):
    """Round both face coordinates to texel centres after selecting the dominant axis."""
    major = np.argmax(abs(directions), axis=-1)
    forward = abs(np.take_along_axis(directions, major[..., None], axis=-1))
    slopes = directions / forward
    bins = np.floor((slopes / TANGENT + 1) * FACE_SIZE / 2)
    rays = ((np.clip(bins, 0, FACE_SIZE - 1) + 0.5) * 2 / FACE_SIZE - 1) * TANGENT
    return np.where(np.arange(3) == major[..., None], slopes, rays)


def depth_taps(case, origin, directions, receiver_offset, normals):
    rays = texel_ray(directions)
    plane = np.sum(receiver_offset * normals, axis=-1) / np.sum(rays * normals, axis=-1)
    depth = hit(case, origin, rays)
    # A visible receiver writes its own depth, beyond the offset comparison plane.
    distance = np.where(depth < plane, depth, 0)
    heights = -rays[..., 2] * distance
    return heights, plane, np.minimum(depth, plane)


def compare_then_average(plane, stored, weights):
    return np.sum(weights * (stored >= plane), axis=-1) / np.sum(weights, axis=-1)


def tap_visibility(case, points, normals, columns, rows):
    """Synthesize depth at the admitted cube texels by ray intersection with the fixture.

    Use the same documented disk distribution, layer criterion and pixel rotation.
    The second estimate pools the found blocker heights into one arithmetic mean (PCSS).
    """
    origin, radius = disk_geometry(case)
    texel = np.linalg.norm(points - origin, axis=-1) * 2 * TANGENT / FACE_SIZE
    delta = points - origin + normals * (1.5 * texel[:, None])
    height = -delta[:, 2]

    # Round at the float32 stages used by the shader's per-pixel hash.
    def noise(a, b):
        return np.mod(
            np.float32(52.9829189)
            * np.mod(
                np.float32(0.06711056) * a.astype("f")
                + np.float32(0.00583715) * b.astype("f"),
                1,
            ),
            1,
        )

    angle = 2 * math.pi * noise(columns, rows)
    jitter = noise(rows, columns)
    widest = 2 * np.linalg.norm(delta, axis=-1)
    widths = np.minimum(radius * (height / ((LIGHT_Z - origin[2]) / 2) - 1), widest)
    octave = np.minimum(
        1, np.log2(np.maximum(widths / texel, 1)) / WIDE_SEARCH_RINGS
    )
    wide_widths = [
        widths * 2 ** (-(ring + jitter) * octave) for ring in range(WIDE_SEARCH_RINGS)
    ]
    compact_widths = [
        np.full_like(widths, radius * scale) for scale in COMPACT_SEARCH_RADII
    ]
    searches = [delta]
    active = [np.ones_like(widths, dtype=bool)]
    for ring, width in enumerate(wide_widths + compact_widths):
        enabled = widths > 2 * texel
        if ring >= WIDE_SEARCH_RINGS:
            enabled &= (width >= texel) & (width < widths)
        for spoke in range(SEARCH_SPOKES):
            theta = angle + ring * GOLDEN + spoke * 2 * math.pi / SEARCH_SPOKES
            active.append(enabled)
            searches.append(
                delta
                + np.stack(
                    [np.sin(theta) * width, np.cos(theta) * width, 0 * width], axis=-1
                )
            )
    search = np.stack(searches, axis=1)
    heights, _, _ = depth_taps(
        case, origin, search, delta[:, None, :], normals[:, None, :]
    )
    rays = texel_ray(search)
    positions = rays * (heights / -rays[..., 2])[..., None]
    projected = (
        delta[:, None, :]
        + (positions - delta[:, None, :])
        * (height[:, None] / np.maximum(height[:, None] - heights, 1e-9))[..., None]
    )
    admitted = np.stack(active, axis=1) & (heights > 0) & (heights < height[:, None])
    admitted[:, 1:] &= (projected[:, 1:] ** 2).sum(axis=-1) <= radius**2
    count = admitted.sum(axis=1)
    safe = np.where(admitted, heights, 1)
    near = np.min(np.where(admitted, heights, np.inf), axis=1)
    far = np.max(np.where(admitted, heights, 0), axis=1)
    near = np.where(count, near, height)
    far = np.where(count, far, height)
    inverse = np.where(admitted, 1 / safe, 0).sum(axis=1) / np.maximum(count, 1)
    pooled = np.where(admitted, heights, 0).sum(axis=1) / np.maximum(count, 1)

    def width(inverse_depth):
        return np.clip(radius * (height * inverse_depth - 1), texel, widest)

    close_width, far_width = width(1 / near), width(1 / far)
    layered = (close_width > 1.5 * far_width) & (close_width - far_width > texel)
    split = 2 / (1 / near + 1 / far)
    index = np.arange(12)
    turn = angle[:, None] + GOLDEN * index
    offsets = (
        np.stack([np.sin(turn), np.cos(turn), 0 * turn], axis=-1)
        * np.sqrt((index + 0.5) / 12)[None, :, None]
    )
    to_light = radius * offsets - delta[:, None, :]
    weights = (
        np.maximum((to_light * normals[:, None, :]).sum(axis=-1), 0)
        * to_light[..., 2]
        / (to_light**2).sum(axis=-1) ** 2
    )

    def taps(widths):
        return depth_taps(
            case,
            origin,
            delta[:, None, :] + offsets * widths[:, None, None],
            delta[:, None, :],
            normals[:, None, :],
        )

    close, _, _ = taps(close_width)
    distant, _, _ = taps(far_width)
    _, plane, stored = taps(width(inverse))
    flat = compare_then_average(plane, stored, weights)
    blocked = ((close > 0) & (close < split[:, None])) | (distant >= split[:, None])
    layered_visibility = (weights * (~blocked)).sum(axis=1) / weights.sum(axis=1)
    _, plane, stored = taps(width(1 / np.where(count, pooled, height)))
    single = compare_then_average(plane, stored, weights)
    return (
        np.where(count, np.where(layered, layered_visibility, flat), 1),
        np.where(count, single, 1),
        layered,
    )
