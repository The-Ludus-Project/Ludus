"""Redistributable scene-linear panoramas and a perspective glTF plane in metres."""

import json
import math
import struct

SIZE = 65
CENTER = SIZE // 2
FOV = math.pi / 2
PLANE_HEIGHT = 4
FLOOR = (0.125,) * 3
# (world direction, angular radius in degrees, RGB radiance). Longitude zero is +X.
FEATURES = (
    ((1, 0, 0), 11, (4, 1, 0.25)),  # straddles u=1/0
    ((1, 0, 0.55), 10, (0.25, 3, 0.5)),
    ((1, 0, -0.55), 10, (0.5, 0.25, 2)),
    ((0, 1, 0), 17, (2, 0.5, 3)),  # all longitudes at north
)
BRACKETS = (1 / 16, 1 / 4, 1)  # seconds, fixed gain and aperture


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def unit(a):
    length = math.sqrt(dot(a, a))
    return tuple(x / length for x in a)


def cross(a, b):
    x, y, z = a
    u, v, w = b
    return (y * w - z * v, z * u - x * w, x * v - y * u)


def basis(n, roll=False):
    up = (0, 0, 1) if abs(n[1]) > 0.95 else (0, 1, 0)
    right = unit(cross(up, n))
    up = cross(n, right)
    return (up, tuple(-v for v in right)) if roll else (right, up)


def direction(u, v):
    phi = math.tau * u
    theta = math.pi * v
    return (
        math.sin(theta) * math.cos(phi),
        math.cos(theta),
        math.sin(theta) * math.sin(phi),
    )


def analytic(d, linear=1, quadratic=1):
    x, y, z = d
    return tuple(
        1 + linear * 0.45 * a + quadratic * 0.7 * b
        for a, b in zip((x, y, z), (x * y, y * z, z * x))
    )


def merge_channel(radiance):
    # Quantize a known square-root response. Recover from the longest unclipped exposure.
    for seconds in reversed(BRACKETS):
        code = round(255 * math.sqrt(min(1, radiance * seconds)))
        if code < 255:
            return (code / 255) ** 2 / seconds
    raise ValueError("All three exposures clipped")


def radiance(mode, d):
    if mode == "constant":
        return (1, 1, 1)
    if mode == "analytic":
        return analytic(d)
    for axis, radius, color in FEATURES[:1] if mode == "glossy" else FEATURES:
        if dot(d, unit(axis)) > math.cos(math.radians(radius)):
            return color
    return FLOOR


def rgbe(rgb):
    maximum = max(rgb)
    if maximum == 0:
        return bytes(4)
    mantissa, exponent = math.frexp(maximum)
    return bytes([int(x * mantissa * 256 / maximum) for x in rgb] + [exponent + 128])


def write_map(root, mode, width):
    height = width // 2
    with (root / "environment.hdr").open("wb") as out:
        out.write(
            f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode()
        )
        for j in range(height):
            for i in range(width):
                rgb = radiance(mode, direction((i + 0.5) / width, (j + 0.5) / height))
                if mode == "bracket":
                    rgb = tuple(map(merge_channel, rgb))
                out.write(rgbe(rgb))


def write_plane(
    root, normal, eye_direction, roughness=0.045, metallic=1, white=1, roll=False
):
    n = unit(normal)
    view = unit(eye_direction)
    right, up = basis(n)
    vertices = [
        tuple(x * a + y * b for a, b in zip(right, up))
        for x, y in ((-2, -2), (2, -2), (2, 2), (-2, -2), (2, 2), (-2, 2))
    ]
    raw = struct.pack("<36f", *(v for p in vertices + [n] * 6 for v in p))
    (root / "plane.bin").write_bytes(raw)
    right, up = basis(view, roll)
    matrix = [v for column in (right, up, view, view) for v in (*column, 0)]
    # Keep the receiver and probes above the World viewport grid.
    matrix[13] += PLANE_HEIGHT
    matrix[-1] = 1
    shape = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"mesh": 0, "translation": [0, PLANE_HEIGHT, 0]},
            {"camera": 0, "matrix": matrix},
        ],
        "cameras": [
            {"type": "perspective", "perspective": {"yfov": FOV, "znear": 0.05}}
        ],
        "meshes": [
            {
                "primitives": [
                    {"attributes": {"POSITION": 0, "NORMAL": 1}, "material": 0}
                ]
            }
        ],
        "materials": [
            {
                "pbrMetallicRoughness": {
                    "metallicFactor": metallic,
                    "roughnessFactor": roughness,
                    "baseColorFactor": [white, white, white, 1],
                }
            }
        ],
        "buffers": [{"uri": "plane.bin", "byteLength": len(raw)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": i * 72, "byteLength": 72} for i in range(2)
        ],
        "accessors": [
            {"bufferView": i, "componentType": 5126, "count": 6, "type": "VEC3"}
            for i in range(2)
        ],
    }
    shape["accessors"][0].update(
        min=list(map(min, zip(*vertices))), max=list(map(max, zip(*vertices)))
    )
    (root / "receiver.gltf").write_text(json.dumps(shape))
    (root / "LICENSE.txt").write_text("CC0 generated receiver geometry and material\n")


def reflection(x, y, n, view, roll=False):
    # The eye is one metre from the plane origin. Intersect its pixel-centre ray with n.p = 0.
    right, up = basis(view, roll)
    ray = unit(
        tuple(
            -v + (2 * (x + 0.5) / SIZE - 1) * r + (1 - 2 * (y + 0.5) / SIZE) * u
            for v, r, u in zip(view, right, up)
        )
    )
    t = -dot(n, view) / dot(n, ray)
    point = tuple(
        origin + v + t * q for origin, v, q in zip((0, PLANE_HEIGHT, 0), view, ray)
    )
    reflected = tuple(q - 2 * dot(n, ray) * a for q, a in zip(ray, n))
    return point, reflected
