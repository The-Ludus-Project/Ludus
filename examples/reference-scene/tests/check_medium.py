"""Capture an independently encoded LUSCN v12 bundle with a bounded absorbing medium through the installed SDK.

The bundle holds a two-sided emitter behind a unit box of homogeneous absorber, seen from a pinhole on the box
axis. The CPU oracle (default mode) and, where the device offers compute storage, the GPU-compute reference
(`gpu-compute`) must both attenuate the centre pixel by Beer-Lambert transmittance, leave the image corners
unattenuated and agree with each other. Modes without medium transport must stop at step 18.
"""

import importlib.machinery
import importlib.util
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

VERSION = 12
NO_TEXTURE = 0xFFFFFFFF
NAME_BYTES = 128
EMISSION_TWO_SIDED = 1
DIFFUSE = 1
MEDIUM_INTERFACE = 10
DEFAULT_COATING_ETA = 1.5
DEFAULT_COATING_THICKNESS = 0.01
EMISSION = (2.0, 3.0, 4.0)
ABSORPTION = (0.5, 1.0, 2.0)
BOX_HALF_EXTENT = 0.5
EXTENT = 16
SAMPLES = 4
# Unattenuated pixels match to float32 rounding; jittered centre rays cross the box within this relative slack.
CENTER_TOLERANCE = 2e-3
PARITY_TOLERANCE = 1e-5
MEDIUM_STEP = 18
UNAVAILABLE_STEPS = (4, 6)
# A cold GPU-compute capture compiles the reference kernel. CMake passes the renderer's reference cold-preparation
# budget as the second argument; this default is the unsanitized value.
DEFAULT_CAPTURE_TIMEOUT_SECONDS = 180


def material(name, emission, flags, area_source, model, reflectance, k):
    record = struct.pack("<2I2f3fIQfIf2If", NO_TEXTURE, 0, 0, 1, *emission, flags, area_source, 0, 0, 0,
                         NO_TEXTURE, NO_TEXTURE, 0)
    encoded = name.encode("utf-8")
    record += struct.pack("<2I", 0, len(encoded)) + encoded.ljust(NAME_BYTES, b"\0")
    record += struct.pack("<I3f3f3f4f", model, *reflectance, 1, 1, 1, *k, 1, 1, DEFAULT_COATING_ETA,
                          DEFAULT_COATING_THICKNESS)
    return record


def mesh(quads):
    """Each quad is four corners whose front face (outward normal) is cross(c1 - c0, c3 - c0)."""
    vertices = b""
    indices = b""
    for index, corners in enumerate(quads):
        a, b, _, d = corners
        u = [b[i] - a[i] for i in range(3)]
        v = [d[i] - a[i] for i in range(3)]
        normal = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        for corner in corners:
            vertices += struct.pack("<3f3f2f4f", *corner, *normal, 0, 0, 0, 0, 0, 0)
        first = 4 * index
        indices += struct.pack("<6I", first, first + 1, first + 2, first, first + 2, first + 3)
    return struct.pack("<2I", 4 * len(quads), 6 * len(quads)) + vertices + indices


def box(h):
    return [
        ((-h, -h, -h), (-h, h, -h), (h, h, -h), (h, -h, -h)),
        ((-h, -h, h), (h, -h, h), (h, h, h), (-h, h, h)),
        ((-h, -h, -h), (-h, -h, h), (-h, h, h), (-h, h, -h)),
        ((h, -h, -h), (h, h, -h), (h, h, h), (h, -h, h)),
        ((-h, -h, -h), (h, -h, -h), (h, -h, h), (-h, -h, h)),
        ((-h, h, -h), (-h, h, h), (h, h, h), (h, h, -h)),
    ]


def fixture():
    data = bytearray(b"LUSCN01\0")
    # Version, meshes, materials, textures, objects, then eye, target, up and vertical field of view.
    data += struct.pack("<5I10f", VERSION, 2, 2, 0, 2, 0, 0, -3, 0, 0, 0, 0, 1, 0, 0.5)
    data += material("Emitter", EMISSION, EMISSION_TWO_SIDED, 1, DIFFUSE, (0, 0, 0), (0, 0, 0))
    data += material("Ink", (0, 0, 0), 0, 0, MEDIUM_INTERFACE, (0, 0, 0), ABSORPTION)
    data += mesh([((-3, -3, 2.5), (-3, 3, 2.5), (3, 3, 2.5), (3, -3, 2.5))])
    data += mesh(box(BOX_HALF_EXTENT))
    for index in range(2):
        data += struct.pack("<2I12fQ", index, index, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, index + 1)
    # Lighting: photometric profile, no analytic lights and no environment.
    data += struct.pack("<4I3f", 1, 0, 0, 0, 0, 0, 0)
    return data


def capture(binary, root, mode, timeout_seconds):
    arguments = [binary, str(root), str(root), str(EXTENT), str(SAMPLES)] + ([mode] if mode else [])
    output = root / "capture.luhdr"
    output.unlink(missing_ok=True)
    result = subprocess.run(arguments, timeout=timeout_seconds)
    return result.returncode, output.read_bytes() if result.returncode == 0 else None


def main():
    binary = str(Path(sys.argv[1]).resolve())
    timeout_seconds = float(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_CAPTURE_TIMEOUT_SECONDS
    loader = importlib.machinery.SourceFileLoader(
        "reference_hdr",
        str(Path(__file__).resolve().parents[3] / "scripts/reference-hdr"),
    )
    decoder = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
    loader.exec_module(decoder)
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        (root / "scene.luscn").write_bytes(fixture())
        images = {}
        for mode, traversal in ((None, "cpu_oracle"), ("gpu-compute", "gpu_compute")):
            status, data = capture(binary, root, mode, timeout_seconds)
            if mode is not None and status in UNAVAILABLE_STEPS:
                print(f"{mode}: no compute device (step {status}); CPU oracle checked only")
                continue
            assert status == 0, (mode, status)
            metadata, pixels = decoder.decode(data)
            assert metadata["traversal"] == traversal, metadata
            assert metadata["medium"] == dict(absorption_per_world=list(ABSORPTION), boundary_triangles=12), metadata
            center = pixels[(EXTENT // 2) * EXTENT + EXTENT // 2]
            for channel in range(3):
                expected = EMISSION[channel] * math.exp(-ABSORPTION[channel] * 2 * BOX_HALF_EXTENT)
                assert abs(center[channel] - expected) <= CENTER_TOLERANCE * expected, (mode, center, expected)
            for corner in (0, EXTENT - 1, EXTENT * (EXTENT - 1), EXTENT * EXTENT - 1):
                for channel in range(3):
                    assert abs(pixels[corner][channel] - EMISSION[channel]) <= 1e-6 * EMISSION[channel], (
                        mode, corner, pixels[corner])
            images[mode] = pixels
        if "gpu-compute" in images:
            difference = max(
                abs(a - b) for cpu, gpu in zip(images[None], images["gpu-compute"]) for a, b in zip(cpu[:3], gpu[:3]))
            assert difference <= PARITY_TOLERANCE, difference
        # Raster acceptance would drop the medium, so the sample stops explicitly instead.
        status, _ = capture(binary, root, "acceptance", timeout_seconds)
        assert status == MEDIUM_STEP, status


if __name__ == "__main__":
    main()
