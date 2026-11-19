"""Generate matched glTF inputs and ForwardLit graph variants for the normal filter study."""
import argparse
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import zlib

HERE = Path(__file__).resolve().parent
GRAPH = HERE.parents[3] / "modules/graphics/renderer/render_graphs/forward_lit"
PERIOD = 8
SCALES = (0, 1, 2)
# Thanks to Blinn, "Simulation of Wrinkled Surfaces" (SIGGRAPH 1978, §§2,5),
# for height-gradient shading directions, and to Kautz, Heidrich and Seidel,
# "Real-Time Bump Map Synthesis" (GH 2001, §§3–5), for prescribed densities.
# The benchmark-scenes guide links the papers and records the measured limits.
# Each row is a compact discrete gradient distribution. The two equal-moment
# patterns have equal mean and variance before packing, and unequal fourth moments.
PATTERNS = ("flat", "tilt", "tilt-wave", "wave", "shoulders", "tails",
            "axis-x", "axis-y", "isotropic", "smooth-left", "smooth-right")


def unit(vector):
    length = math.sqrt(sum(value * value for value in vector))
    return tuple(value / length for value in vector)


def byte(value):
    if not 0 <= value <= 1:
        raise ValueError(f"channel {value} is outside the fixture encoding")
    return math.floor(value * 255 + 0.5)


def srgb(value):
    return byte(12.92 * value if value < 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055)


def sample(pattern, x, y):
    # Each tangent slope defines the local height gradient (-dh/du, -dh/dv).
    # The tilt and wave controls are samples of separable piecewise-linear heights.
    sign_x = 1 if x % 2 else -1
    sign_y = 1 if y % 2 else -1
    if pattern == "flat":
        sx, sy = 0, 0
    elif pattern in ("tilt", "tilt-wave"):
        sx, sy = 0.125, 0.22 * sign_y if pattern == "tilt-wave" else 0
    elif pattern == "tails":
        sx, sy = (0.36, -0.36, 0, 0, 0, 0, 0, 0)[x % PERIOD], 0
    elif pattern == "shoulders":
        sx, sy = 0.18 * sign_x, 0
    elif pattern in ("axis-x", "axis-y"):
        sx, sy = (0.24 * sign_x, 0.06 * sign_y) if pattern == "axis-x" else (0.06 * sign_x, 0.24 * sign_y)
    elif pattern == "isotropic":
        angle = math.tau * ((x + 3 * y) % PERIOD) / PERIOD
        radius = math.hypot(0.24, 0.06)
        sx, sy = radius * math.cos(angle), radius * math.sin(angle)
    elif pattern == "wave":
        sx, sy = 0.255 * (1 if (x // 2) % 2 else -1), 0
    else:
        sx, sy = 0.255 * sign_x, 0
    nx, ny, nz = unit((sx, sy, 1))
    normal = (byte(nx * 0.5 + 0.5), byte(ny * 0.5 + 0.5), byte(nz * 0.5 + 0.5))
    # Fit moments to the actual packed normal, including its mid-code offset.
    nx, ny = (2 * c / 255 - 1 for c in normal[:2])
    nz = math.sqrt(1 - nx * nx - ny * ny)
    sx, sy = nx / nz, ny / nz
    moments = (srgb(0.5 + sx), srgb(0.5 + sy), srgb(sx * sx + sy * sy))
    roughness = 255
    if pattern in ("smooth-left", "smooth-right"):
        roughness = 96 if (sign_x < 0) == (pattern == "smooth-left") else 224
    return normal, moments, (0, roughness, 255)


def write_png(path, width, height, tile):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    pixels = b"".join(b"\0" + tile[y % PERIOD] * (width // PERIOD) for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                     chunk(b"IDAT", zlib.compress(pixels)) + chunk(b"IEND", b""))


def write_scene(directory, pattern, width):
    directory.mkdir(parents=True, exist_ok=True)
    height = 64
    names = ("normal", "moments", "scalars")
    for i, name in enumerate(names):
        rows = [bytes(c for x in range(PERIOD) for c in sample(pattern, x, y)[i]) for y in range(PERIOD)]
        write_png(directory / (name + ".png"), width, height, rows)
    # All three instances have identical texel density and an explicit right-handed tangent frame.
    fields = [("POSITION", 3, [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0]),
              ("NORMAL", 3, [0,0,1] * 4), ("TANGENT", 4, [1,0,0,1] * 4),
              ("TEXCOORD_0", 2, [0,0, 64/width,0, 64/width,1, 0,1])]
    blob = bytearray()
    views, accessors, attributes = [], [], {}
    for name, components, values in fields:
        data = struct.pack(f"<{len(values)}f", *values)
        attributes[name] = len(views)
        views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(data)))
        blob.extend(data)
        accessors.append(dict(bufferView=len(views)-1, componentType=5126, count=4, type=f"VEC{components}"))
    accessors[0].update(min=[-1,-1,0], max=[1,1,0])
    views.append(dict(buffer=0, byteOffset=len(blob), byteLength=12))
    blob.extend(struct.pack("<6H", 0,1,2, 0,2,3))
    accessors.append(dict(bufferView=4, componentType=5123, count=6, type="SCALAR"))
    materials = [
        dict(
            name=f"Strength{scale}",
            pbrMetallicRoughness=dict(
                baseColorTexture=dict(index=1),
                metallicRoughnessTexture=dict(index=2),
                metallicFactor=1,
                roughnessFactor=0.25,
            ),
            normalTexture=dict(index=0, scale=scale),
        )
        for scale in SCALES
    ]
    nodes = [dict(mesh=i, translation=[(i - 1) * 3, 0, 0]) for i in range(3)]
    nodes.append(dict(camera=0, translation=[0, 0, 3]))
    nodes.append(dict(extensions={"KHR_lights_punctual": dict(light=0)}))
    scene = dict(
        asset=dict(version="2.0"),
        scene=0,
        scenes=[dict(nodes=list(range(len(nodes))))],
        extensionsUsed=["KHR_lights_punctual"],
        extensions={"KHR_lights_punctual": dict(lights=[dict(type="directional", color=[1, 1, 1], intensity=1)])},
        nodes=nodes,
        cameras=[dict(type="perspective", perspective=dict(yfov=0.7, znear=0.01))],
        meshes=[dict(primitives=[dict(attributes=attributes, indices=4, material=i)]) for i in range(3)],
        materials=materials,
        images=[dict(uri=name + ".png") for name in names],
        textures=[dict(source=i) for i in range(3)],
        buffers=[dict(uri="quad.bin", byteLength=len(blob))],
        bufferViews=views,
        accessors=accessors,
    )
    (directory / "quad.bin").write_bytes(blob)
    (directory / "scene.gltf").write_text(json.dumps(scene, indent=2) + "\n")


def write_graph(directory, method, profile):
    shaders = directory / "main_shaders"
    shaders.mkdir(parents=True, exist_ok=True)
    for path in GRAPH.glob("*.lurender.json"):
        document = path.read_text()
        (directory / path.name).write_text(document)
    manifest = (GRAPH / "main.lushader.json.in").read_text().replace(
        "@LUDUS_DEFAULT_RENDER_GRAPH_PROFILES@", json.dumps([profile, "wgsl"]))
    (directory / "main.lushader.json").write_text(manifest)
    shutil.copyfile(GRAPH / "main_shaders/geometry.slang", shaders / "geometry.slang")
    shutil.copyfile(HERE / "radiance.slang", shaders / "radiance.slang")
    shutil.copyfile(HERE / (method + ".slang"), shaders / "color.slang")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--profile", choices=["metal", "spirv", "dxil"], default="metal")
    parser.add_argument("--cooker", type=Path, help="also cook every scene and verify the requested reduction")
    args = parser.parse_args()
    for pattern in PATTERNS:
        for route, width in (("runtime", 64), ("cooker", 8192)):
            directory = args.output / route / pattern
            write_scene(directory, pattern, width)
            if args.cooker:
                result = subprocess.run([args.cooker, "gltf", directory / "scene.gltf", directory / "cooked"],
                                        check=True, stdout=subprocess.PIPE)
                report = json.loads(result.stdout)
                expected_width = 4096 if route == "cooker" else width
                expected_height = 32 if route == "cooker" else 64
                if len(report["materials"]) != len(SCALES) or any(
                    (slot["texture_width"], slot["texture_height"], slot["roughness"], slot["metallic"])
                    != (expected_width, expected_height, 0.25, 1)
                    for slot in report["materials"]
                ):
                    raise ValueError(f"{directory}: unexpected cooked texture size or material factors")
    for method in ("baseline", "candidate", "reference"):
        write_graph(args.output / method, method, args.profile)


if __name__ == "__main__":
    main()
