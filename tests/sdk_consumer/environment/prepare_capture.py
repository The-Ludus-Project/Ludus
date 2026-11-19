"""Prepare the Illinois IBL mirror-ball bracket externally. Requires NumPy and Pillow."""

import argparse
import hashlib
import io
import json
import zipfile
from pathlib import Path

import numpy as np
from PIL import Image

ARCHIVE_SHA256 = "c79d86ba296b31222ba6e978029ecc5173ef76921182c30af54ce063567a75b8"
DENOMINATORS = (24, 60, 120, 205, 553)
WIDTH, HEIGHT = 4352, 512  # deliberately crosses the cooker's 4096-column limit
CHANNEL = 1  # relative green radiance


def solve_response(images, seconds):
    # Debevec and Malik (1997), equations 3-4. Fix g(128)=0, smooth second differences.
    # Use deterministic exposure-stratified locations inside the chrome ball.
    yy, xx = np.indices(images.shape[1:])
    inside = ((xx - xx.mean()) / xx.mean()) ** 2 + (
        (yy - yy.mean()) / yy.mean()
    ) ** 2 < 0.90
    middle = images[2]
    locations = []
    for code in range(4, 252, 2):
        matches = np.flatnonzero(inside & (middle >= code) & (middle < code + 2))
        if len(matches):
            locations.append(matches[len(matches) // 2])
    values = images.reshape(len(images), -1)[:, locations]
    count = len(locations)
    rows, rhs = [], []
    for exposure, codes in enumerate(values):
        for point, z in enumerate(codes):
            z = int(z)
            weight = min(z, 255 - z) / 127
            row = np.zeros(256 + count)
            row[z], row[256 + point] = weight, -weight
            rows.append(row)
            rhs.append(weight * np.log(seconds[exposure]))
    anchor = np.zeros(256 + count)
    anchor[128] = 1
    rows.append(anchor)
    rhs.append(0)
    for z in range(1, 255):
        row = np.zeros(256 + count)
        row[z - 1 : z + 2] = np.array([1, -2, 1]) * 20 * min(z, 255 - z) / 127
        rows.append(row)
        rhs.append(0)
    return np.linalg.lstsq(rows, rhs, rcond=None)[0][:256]


def prepare(archive, root):
    archive_digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if archive_digest != ARCHIVE_SHA256:
        raise ValueError(
            f"Expected the Illinois Project IBL archive {ARCHIVE_SHA256}, got {archive_digest}"
        )
    with zipfile.ZipFile(archive) as files:
        photos = np.stack(
            [
                np.asarray(
                    Image.open(io.BytesIO(files.read(f"proj4_materials/{d:04d}.jpg")))
                    .convert("RGB")
                    .resize((384, 384), Image.Resampling.BILINEAR)
                )
                for d in DENOMINATORS
            ]
        )
    seconds = 1 / np.array(DENOMINATORS)
    height, width = photos.shape[1:3]
    log_radiance = np.zeros((height, width, 3))
    references = []
    for k in range(3):
        codes = photos[:, :, :, k]
        response = solve_response(codes, seconds)
        weights = np.minimum(codes, 255 - codes).astype(float)
        estimates = response[codes] - np.log(seconds[:, None, None])
        reliable = (codes >= 12) & (codes <= 245)
        # JPEG shadows and near-saturation shoulder have poor exposure consistency.
        weights = np.where(reliable.any(axis=0), weights * reliable, weights)
        total = weights.sum(axis=0)
        log_radiance[:, :, k] = np.where(
            total > 0,
            (weights * estimates).sum(axis=0) / np.maximum(total, 1),
            np.where(codes[0] == 0, estimates[0], estimates[-1]),
        )
        if k == CHANNEL:
            references = np.exp(estimates)
    ball = np.exp(log_radiance)
    # Inverse orthographic mirror-ball projection: normal bisects +Z (eye) and incident light.
    theta = (np.arange(HEIGHT)[:, None] + 0.5) * np.pi / HEIGHT
    phi = (np.arange(WIDTH)[None, :] + 0.5) * 2 * np.pi / WIDTH
    dx, dy, dz = np.sin(theta) * np.cos(phi), np.cos(theta), np.sin(theta) * np.sin(phi)
    norm = np.sqrt(2 * (1 + dz))
    x = np.rint((1 + dx / norm) * (width - 1) / 2).astype(int)
    y = np.rint((1 - dy / norm) * (height - 1) / 2).astype(int)
    linear = ball[y, x]
    # Keep all fixture values in the SDK's linear readback range. Ratios are unchanged.
    scale = 6 / linear.max()
    linear *= scale
    _, exponent = np.frexp(linear.max(axis=2))
    encoded = np.empty((HEIGHT, WIDTH, 4), dtype=np.uint8)
    encoded[:, :, :3] = np.floor(linear * np.exp2(8 - exponent[:, :, None]))
    encoded[:, :, 3] = exponent + 128
    with (root / "environment.hdr").open("wb") as out:
        out.write(
            f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {HEIGHT} +X {WIDTH}\n".encode()
        )
        out.write(encoded.tobytes())
    codes = photos[:, y, x, CHANNEL]
    # Select broad, locally uniform areas whose indicated exposure is measurable. Keep probes
    # away from the mirror silhouette and use only the upper-left pixel of a cooked 2x2 cell.
    green = linear[:, :, CHANNEL]
    spread = np.ptp(green.reshape(HEIGHT // 2, 2, WIDTH // 2, 2), axis=(1, 3))
    uniform = spread < 0.01 * np.maximum(green[::2, ::2], 1e-6)
    usable = uniform & (norm[::2, ::2] > 0.45)
    probes = []
    for name, exposure, mask in [
        ("bright", 4, (codes[0] == 255) & (codes[4] > 32) & (codes[4] < 225)),
        ("overlap", 2, (codes[0] < 230) & (codes[1] > 50) & (codes[2] > 25)),
        ("dark", 0, (codes[0] > 18) & (codes[0] < 85) & (codes[4] < 20)),
    ]:
        candidates = np.argwhere(usable & mask[::2, ::2])
        if len(candidates) == 0:
            raise ValueError(f"No measurable {name} region")
        row, col = candidates[len(candidates) // 2] * 2
        probes.append(
            dict(
                name=name,
                x=int(col),
                y=int(row),
                codes=codes[:, row, col].tolist(),
                reference=float(references[exposure, y[row, col], x[row, col]] * scale),
            )
        )
    reference = probes[1]["reference"]
    for p in probes:
        p["ratio"] = p["reference"] / reference
    # Integrate the green seam strip with exact row solid angles in the consumer check.
    decoded = encoded[:, :, :3] * np.exp2(encoded[:, :, 3:].astype(float) - 136)
    green = decoded[:, :, CHANNEL]
    rows = np.cos(np.arange(HEIGHT + 1) * np.pi / HEIGHT)
    stripe = np.concatenate((green[:, : WIDTH // 16], green[:, -WIDTH // 16 :]), axis=1)
    report = dict(
        archive_sha256=archive_digest,
        exposures_seconds=seconds.tolist(),
        width=WIDTH,
        height=HEIGHT,
        probes=probes,
        peak=float(green.max()),
        seam_integral=float(
            np.sum(stripe * (rows[:-1] - rows[1:])[:, None]) * 2 * np.pi / WIDTH
        ),
    )
    (root / "capture.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("archive", type=Path)
    p.add_argument("directory", type=Path)
    args = p.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    prepare(args.archive, args.directory)
