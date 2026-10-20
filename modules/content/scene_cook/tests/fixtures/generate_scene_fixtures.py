"""Regenerate the glTF/GLB and OBJ/MTL importer fixtures (not run by the build).

Every fixture is synthetic and CC0 (see the LICENSE.txt written beside each
scene). The OBJ and MTL text below is the fixture source verbatim; the glTF
buffers are packed here so offsets stay consistent; textures come from Pillow.

Usage: python3 generate_scene_fixtures.py   (run from this directory; needs Pillow)
"""

import base64
import io
import json
import struct
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
LICENSE = "CC0 1.0: synthetic Ludus test fixture, dedicated to the public domain.\n"


def png_bytes(width, height, pixel):
    image = Image.new("RGBA", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), pixel(x, y))
    stream = io.BytesIO()
    image.save(stream, format="PNG", optimize=False)
    return stream.getvalue()


def jpeg_bytes(width, height, pixel):
    image = Image.new("RGB", (width, height))
    for y in range(height):
        for x in range(width):
            image.putpixel((x, y), pixel(x, y)[:3])
    stream = io.BytesIO()
    image.save(stream, format="JPEG", quality=90, subsampling=0)
    return stream.getvalue()


def checker(x, y):
    return (230, 230, 230, 255) if (x + y) % 2 == 0 else (40, 40, 40, 255)


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))


# ---------------------------------------------------------------------------
# OBJ + MTL: floor (textured, quad), mirror wall (negative indices, no vt/vn),
# ceiling light (Ke), PBR box (smoothing groups, normal map, dissolve).

OBJ = """# Synthetic OBJ/MTL fixture for the Ludus best-effort importer (CC0).
mtllib room.mtl
o Floor
v -2 0 -2
v 2 0 -2
v 2 0 2
v -2 0 2
vt 0 0
vt 4 0
vt 4 4
vt 0 4
vn 0 1 0
usemtl Checker
s off
f 1/1/1 4/4/1 3/3/1 2/2/1
o MirrorWall
v -2 0 -2
v 2 0 -2
v 2 2 -2
v -2 2 -2
usemtl Mirror
f -4 -3 -2 -1
o Light
v -0.5 1.99 -0.5
v 0.5 1.99 -0.5
v 0.5 1.99 0.5
v -0.5 1.99 0.5
usemtl Light
f -4 -3 -2 -1
o Box
g box_top box
usemtl BoxPbr
s 1
v -0.5 0 -0.5
v 0.5 0 -0.5
v 0.5 1 -0.5
v -0.5 1 -0.5
v -0.5 0 0.5
v 0.5 0 0.5
v 0.5 1 0.5
v -0.5 1 0.5
vt 0 0
vt 1 0
vt 1 1
vt 0 1
f -4/-4 -3/-3 -2/-2 -1/-1
f -5/-4 -8/-3 -7/-2 -6/-1
f -8/-4 -4/-3 -1/-2 -7/-1
f -6/-4 -2/-3 -3/-2 -5/-1
f -7/-4 -1/-3 -2/-2 -6/-1
f -5/-4 -3/-3 -4/-2 -8/-1
l 1 2
"""

MTL = """# Materials for room.obj (CC0).
newmtl Checker
Kd 1 1 1
Ks 0.04 0.04 0.04
Ns 10
illum 2
map_Kd -clamp off textures\\checker.png

newmtl Mirror
Kd 0 0 0
Ks 0.9 0.9 0.9
Ns 1000
illum 3

newmtl Light
Kd 0.8 0.8 0.8
Ke 17 12 4
illum 1

newmtl BoxPbr
Kd 0.2 0.4 0.8
Pr 0.5
Pm 0
map_Pr -imfchan g textures/rough.png
norm textures/normal.png
d 0.5
"""


def write_obj():
    root = HERE / "obj" / "room"
    write(root / "room.obj", OBJ)
    write(root / "room.mtl", MTL)
    write(root / "LICENSE.txt", LICENSE)
    write(root / "textures" / "checker.png", png_bytes(8, 8, checker))
    # Green 128 everywhere: map_Pr -imfchan g halves the roughness factor.
    write(
        root / "textures" / "rough.png", png_bytes(4, 4, lambda x, y: (0, 128, 0, 255))
    )
    write(
        root / "textures" / "normal.png",
        png_bytes(4, 4, lambda x, y: (128, 128, 255, 255)),
    )


# ---------------------------------------------------------------------------
# Cut-outs (LUSCN v5 alpha masks): an OBJ leaf with a separate map_d mask at a
# different resolution, an all-opaque mask (cooked opaque) and an untextured
# halo whose mask is the alpha (-imfchan m) of an RGBA image; a glTF quad with
# alphaMode MASK, a texture alpha ramp and baseColorFactor alpha, beside BLEND.

CUTOUT_OBJ = """# Synthetic cut-out OBJ/MTL fixture for the Ludus best-effort importer (CC0).
mtllib cutout.mtl
vt 0 0
vt 1 0
vt 1 1
vt 0 1
usemtl Leaf
v 0 0 0
v 2 0 0
v 2 2 0
v 0 2 0
f -4/1 -3/2 -2/3 -1/4
usemtl Halo
v 3 0 0
v 5 0 0
v 5 1 0
v 3 1 0
f -4/1 -3/2 -2/3 -1/4
usemtl Solid
v 6 0 0
v 7 0 0
v 7 1 0
v 6 1 0
f -4/1 -3/2 -2/3 -1/4
"""

CUTOUT_MTL = """# Materials for cutout.obj (CC0).
newmtl Leaf
Kd 1 1 1
map_Kd textures/leaf.png
map_d textures/leaf_mask.png

newmtl Halo
Kd 1 0 0
map_d -imfchan m textures/halo_mask.png

newmtl Solid
Kd 0.5 0.5 0.5
map_d textures/solid_mask.png
"""


def write_cutouts():
    root = HERE / "obj" / "cutout"
    write(root / "cutout.obj", CUTOUT_OBJ)
    write(root / "cutout.mtl", CUTOUT_MTL)
    write(root / "LICENSE.txt", LICENSE)
    write(root / "textures" / "leaf.png", png_bytes(8, 8, checker))
    # Left half transparent, right half opaque, as grey (red) values.
    write(
        root / "textures" / "leaf_mask.png",
        png_bytes(4, 4, lambda x, y: (0, 0, 0, 255) if x < 2 else (255, 255, 255, 255)),
    )
    # Opacity in alpha only: the top row is transparent.
    write(
        root / "textures" / "halo_mask.png",
        png_bytes(2, 2, lambda x, y: (255, 255, 255, 0 if y == 0 else 255)),
    )
    write(
        root / "textures" / "solid_mask.png",
        png_bytes(2, 2, lambda x, y: (255, 255, 255, 255)),
    )

    # Alpha ramp 0, 64, 128, 255 (row-major) times baseColorFactor alpha 0.5.
    ramp = [0, 64, 128, 255]
    ramp_png = png_bytes(2, 2, lambda x, y: (200, 200, 200, ramp[y * 2 + x]))
    payload = floats([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]) + floats(
        [0, 1, 1, 1, 1, 0, 0, 0]
    )
    payload += struct.pack("<6H", 0, 1, 2, 0, 2, 3)
    document = {
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [{"mesh": 0}, {"mesh": 1, "translation": [2, 0, 0]}],
        "meshes": [
            {
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                        "indices": 2,
                        "material": 0,
                    }
                ]
            },
            {
                "primitives": [
                    {
                        "attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                        "indices": 2,
                        "material": 1,
                    }
                ]
            },
        ],
        "materials": [
            {
                "name": "Cutout",
                "alphaMode": "MASK",
                "alphaCutoff": 0.25,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1, 1, 1, 0.5],
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0,
                },
            },
            {
                "name": "Glass",
                "alphaMode": "BLEND",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.2, 0.4, 0.8, 0.3],
                    "metallicFactor": 0,
                },
            },
        ],
        "textures": [{"source": 0}],
        "images": [
            {
                "uri": "data:image/png;base64,"
                + base64.b64encode(ramp_png).decode("ascii")
            }
        ],
        "buffers": [
            {
                "byteLength": len(payload),
                "uri": "data:application/octet-stream;base64,"
                + base64.b64encode(payload).decode("ascii"),
            }
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 48},
            {"buffer": 0, "byteOffset": 48, "byteLength": 32},
            {"buffer": 0, "byteOffset": 80, "byteLength": 12},
        ],
        "accessors": [
            {
                "bufferView": 0,
                "componentType": FLOAT,
                "count": 4,
                "type": "VEC3",
                "min": [0, 0, 0],
                "max": [1, 1, 0],
            },
            {"bufferView": 1, "componentType": FLOAT, "count": 4, "type": "VEC2"},
            {"bufferView": 2, "componentType": USHORT, "count": 6, "type": "SCALAR"},
        ],
    }
    root = HERE / "gltf" / "cutout"
    write(root / "scene.gltf", json.dumps(document, indent=1) + "\n")
    write(root / "LICENSE.txt", LICENSE)


# ---------------------------------------------------------------------------
# glTF: hierarchy (matrix + TRS), an instanced cube (one placement mirrored),
# interleaved attributes, uint8/uint16/no indices, a triangle strip, external
# buffer and image, data-URI image, camera, KHR_lights_punctual, emissive
# strength, alpha mask, an optional unknown extension.


class BufferBuilder:
    def __init__(self):
        self.data = bytearray()
        self.views = []
        self.accessors = []

    def view(self, payload, stride=None, target=None):
        while len(self.data) % 4:
            self.data.append(0)
        entry = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(payload)}
        if stride:
            entry["byteStride"] = stride
        if target:
            entry["target"] = target
        self.data += payload
        self.views.append(entry)
        return len(self.views) - 1

    def accessor(self, view, component, count, kind, offset=0, extra=None):
        entry = {
            "bufferView": view,
            "componentType": component,
            "count": count,
            "type": kind,
        }
        if offset:
            entry["byteOffset"] = offset
        if extra:
            entry.update(extra)
        self.accessors.append(entry)
        return len(self.accessors) - 1


FLOAT = 5126
UBYTE = 5121
USHORT = 5123


def floats(values):
    return struct.pack("<%df" % len(values), *values)


def cube_vertices():
    # 24 vertices: position, normal, uv per face (counter-clockwise outward).
    faces = [
        ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
        ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
        ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
        ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
        ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
        ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
    ]
    uvs = [(0, 1), (1, 1), (1, 0), (0, 0)]
    interleaved = []
    indices = []
    for face, (normal, corners) in enumerate(faces):
        for corner, position in enumerate(corners):
            interleaved += (
                [c * 0.5 for c in position] + list(normal) + list(uvs[corner])
            )
        base = face * 4
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return interleaved, indices


def build_gltf_document(images_inline):
    """Return (document, buffer bytes, external images) for the instanced scene."""
    builder = BufferBuilder()
    # Floor quad: separate arrays, uint16 indices.
    floor_positions = builder.view(floats([-3, 0, 3, 3, 0, 3, 3, 0, -3, -3, 0, -3]))
    floor_normals = builder.view(floats([0, 1, 0] * 4))
    floor_uvs = builder.view(floats([0, 0, 3, 0, 3, 3, 0, 3]))
    floor_indices = builder.view(struct.pack("<6H", 0, 1, 2, 0, 2, 3))
    a_floor_p = builder.accessor(
        floor_positions, FLOAT, 4, "VEC3", extra={"min": [-3, 0, -3], "max": [3, 0, 3]}
    )
    a_floor_n = builder.accessor(floor_normals, FLOAT, 4, "VEC3")
    a_floor_t = builder.accessor(floor_uvs, FLOAT, 4, "VEC2")
    a_floor_i = builder.accessor(floor_indices, USHORT, 6, "SCALAR")
    # Cube: interleaved position/normal/uv with stride 32, uint8 indices.
    interleaved, indices = cube_vertices()
    cube_view = builder.view(floats(interleaved), stride=32)
    cube_index_view = builder.view(struct.pack("<%dB" % len(indices), *indices))
    a_cube_p = builder.accessor(
        cube_view, FLOAT, 24, "VEC3", extra={"min": [-0.5] * 3, "max": [0.5] * 3}
    )
    a_cube_n = builder.accessor(cube_view, FLOAT, 24, "VEC3", offset=12)
    a_cube_t = builder.accessor(cube_view, FLOAT, 24, "VEC2", offset=24)
    a_cube_i = builder.accessor(cube_index_view, UBYTE, len(indices), "SCALAR")
    # Lamp: one non-indexed triangle facing +z.
    lamp_view = builder.view(floats([-0.2, 2, 0, 0.2, 2, 0, 0, 2.3, 0]))
    a_lamp_p = builder.accessor(
        lamp_view, FLOAT, 3, "VEC3", extra={"min": [-0.2, 2, 0], "max": [0.2, 2.3, 0]}
    )
    # Strip: four vertices, two triangles.
    strip_view = builder.view(floats([2, 0, 0, 2, 1, 0, 2.5, 0, 0, 2.5, 1, 0]))
    a_strip_p = builder.accessor(
        strip_view, FLOAT, 4, "VEC3", extra={"min": [2, 0, 0], "max": [2.5, 1, 0]}
    )

    base_png = png_bytes(8, 8, checker)
    metal_rough_png = png_bytes(2, 2, lambda x, y: (0, 128, 255, 255))
    normal_png = png_bytes(2, 2, lambda x, y: (128, 128, 255, 255))
    external = {}
    images = []
    if images_inline:
        image_view = builder.view(base_png)
        images.append({"bufferView": image_view, "mimeType": "image/png"})
    else:
        external["textures/base.png"] = base_png
        images.append({"uri": "textures/base.png"})
    images.append(
        {
            "uri": "data:image/png;base64,"
            + base64.b64encode(metal_rough_png).decode("ascii")
        }
    )
    images.append(
        {"uri": "data:image/png;base64," + base64.b64encode(normal_png).decode("ascii")}
    )

    document = {
        "asset": {"version": "2.0", "generator": "Ludus fixture generator"},
        "extensionsUsed": [
            "KHR_lights_punctual",
            "KHR_materials_emissive_strength",
            "EXT_ludus_fixture_note",
        ],
        "extensionsRequired": ["KHR_lights_punctual"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {
                "name": "Root",
                "matrix": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
                "children": [1, 2, 3, 5, 6, 7, 8],
            },
            {"name": "Floor", "mesh": 0},
            {"name": "CubeA", "mesh": 1, "translation": [-1, 0.5, 0]},
            {
                "name": "Group",
                "rotation": [0, 0.7071067811865476, 0, 0.7071067811865476],
                "children": [4],
            },
            {
                "name": "CubeMirrored",
                "mesh": 1,
                "translation": [1, 0.5, 0],
                "scale": [-1, 1, 1],
            },
            {"name": "Camera", "camera": 0, "translation": [0, 1.5, 6]},
            {
                "name": "Spot",
                "translation": [0, 3, 0],
                "rotation": [-0.7071067811865476, 0, 0, 0.7071067811865476],
                "extensions": {"KHR_lights_punctual": {"light": 0}},
            },
            {
                "name": "Lamp",
                "mesh": 2,
                "extensions": {"KHR_lights_punctual": {"light": 1}},
            },
            {"name": "Strip", "mesh": 3},
        ],
        "cameras": [
            {
                "type": "perspective",
                "perspective": {
                    "yfov": 0.8,
                    "aspectRatio": 1.5,
                    "znear": 0.1,
                    "zfar": 100,
                },
            }
        ],
        "extensions": {
            "KHR_lights_punctual": {
                "lights": [
                    {
                        "type": "spot",
                        "color": [1, 1, 1],
                        "intensity": 50,
                        "spot": {"innerConeAngle": 0.3, "outerConeAngle": 0.6},
                    },
                    {"type": "directional", "color": [1, 0.9, 0.8], "intensity": 3},
                ]
            }
        },
        "meshes": [
            {
                "name": "Floor",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": a_floor_p,
                            "NORMAL": a_floor_n,
                            "TEXCOORD_0": a_floor_t,
                        },
                        "indices": a_floor_i,
                        "material": 0,
                    }
                ],
            },
            {
                "name": "Cube",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": a_cube_p,
                            "NORMAL": a_cube_n,
                            "TEXCOORD_0": a_cube_t,
                        },
                        "indices": a_cube_i,
                        "material": 1,
                    }
                ],
            },
            {
                "name": "Lamp",
                "primitives": [{"attributes": {"POSITION": a_lamp_p}, "material": 2}],
            },
            {
                "name": "Strip",
                "primitives": [{"attributes": {"POSITION": a_strip_p}, "mode": 5}],
            },
        ],
        "materials": [
            {
                "name": "Floor",
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0,
                    "roughnessFactor": 0.8,
                },
            },
            {
                "name": "Metal",
                "doubleSided": True,
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.9, 0.6, 0.3, 1],
                    "metallicRoughnessTexture": {"index": 1},
                },
                "normalTexture": {"index": 2, "scale": 1},
            },
            {
                "name": "Glow",
                "alphaMode": "MASK",
                "emissiveFactor": [1, 0.5, 0.25],
                "extensions": {
                    "KHR_materials_emissive_strength": {"emissiveStrength": 4}
                },
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.1, 0.1, 0.1, 1],
                    "metallicFactor": 0,
                },
            },
        ],
        "textures": [{"source": 0, "sampler": 0}, {"source": 1}, {"source": 2}],
        "samplers": [{"wrapS": 10497, "wrapT": 10497}],
        "images": images,
        "accessors": builder.accessors,
        "bufferViews": builder.views,
    }
    while len(builder.data) % 4:
        builder.data.append(0)
    return document, bytes(builder.data), external


def write_gltf():
    document, buffer, external = build_gltf_document(images_inline=False)
    document["buffers"] = [{"uri": "scene.bin", "byteLength": len(buffer)}]
    root = HERE / "gltf" / "instanced"
    write(root / "scene.gltf", json.dumps(document, indent=1) + "\n")
    write(root / "scene.bin", buffer)
    write(root / "LICENSE.txt", LICENSE)
    for name, data in external.items():
        write(root / name, data)

    # The same scene as one self-contained GLB: BIN chunk buffer, bufferView image.
    document, buffer, _ = build_gltf_document(images_inline=True)
    document["buffers"] = [{"byteLength": len(buffer)}]
    json_bytes = json.dumps(document, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    length = 12 + 8 + len(json_bytes) + 8 + len(buffer)
    glb = struct.pack("<III", 0x46546C67, 2, length)
    glb += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes
    glb += struct.pack("<II", len(buffer), 0x004E4942) + buffer
    root = HERE / "gltf" / "embedded"
    write(root / "scene.glb", glb)
    write(root / "LICENSE.txt", LICENSE)

    # A data-URI-only .gltf with a JPEG base colour, for the JPEG decode path.
    jpeg = jpeg_bytes(8, 8, checker)
    data_uri = {
        "asset": {"version": "2.0"},
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [
            {
                "primitives": [
                    {"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "material": 0}
                ]
            }
        ],
        "materials": [
            {
                "pbrMetallicRoughness": {
                    "baseColorTexture": {"index": 0},
                    "metallicFactor": 0,
                }
            }
        ],
        "textures": [{"source": 0}],
        "images": [
            {"uri": "data:image/jpeg;base64," + base64.b64encode(jpeg).decode("ascii")}
        ],
    }
    payload = floats([0, 0, 0, 1, 0, 0, 0, 1, 0]) + floats([0, 1, 1, 1, 0, 0])
    data_uri["buffers"] = [
        {
            "byteLength": len(payload),
            "uri": "data:application/octet-stream;base64,"
            + base64.b64encode(payload).decode("ascii"),
        }
    ]
    data_uri["bufferViews"] = [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 24},
    ]
    data_uri["accessors"] = [
        {
            "bufferView": 0,
            "componentType": FLOAT,
            "count": 3,
            "type": "VEC3",
            "min": [0, 0, 0],
            "max": [1, 1, 0],
        },
        {"bufferView": 1, "componentType": FLOAT, "count": 3, "type": "VEC2"},
    ]
    root = HERE / "gltf" / "data-uri"
    write(root / "scene.gltf", json.dumps(data_uri, indent=1) + "\n")
    write(root / "LICENSE.txt", LICENSE)


if __name__ == "__main__":
    write_obj()
    write_gltf()
    write_cutouts()
    for path in sorted(HERE.rglob("*")):
        if path.is_file() and path.suffix not in (".py",):
            print(path.relative_to(HERE), path.stat().st_size)
