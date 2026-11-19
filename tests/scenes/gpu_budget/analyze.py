"""Decode the arm64 pilot stream, preserving every timing state and the display texture."""
import argparse
import csv
import gzip
import io
import json
import math
from pathlib import Path
import struct
import zlib

SIZES = {"full": (960, 640), "medium": (720, 480), "half": (480, 320)}
SEGMENTS = ("authored", "wide", "holdout")
FRAMES = 1200
BUDGET_MS = 12.5
RECORD = struct.Struct("<8Q")


def percentile(values, fraction):
    return sorted(values)[math.ceil(fraction * len(values)) - 1] if values else None


def verdict(segments):
    if any(len(segments.get(key, [])) != FRAMES for key in SEGMENTS):
        return "inconclusive"
    return "pass" if all(percentile(samples, .95) <= BUDGET_MS for samples in segments.values()) else "miss"


def png(path, width, height, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    stride = width * 4
    scanlines = b"".join(b"\0" + rgba[n:n + stride] for n in range(0, len(rgba), stride))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(scanlines, 9)) + chunk(b"IEND", b""))


def summarize(values):
    return {key: percentile(values, quantile) for key, quantile in (("p50", .5), ("p95", .95), ("p99", .99))}


def decode(path, destination, size):
    width, height = SIZES[size]
    valid = {}
    summary = {}
    rows = io.StringIO()
    writer = csv.writer(rows)
    writer.writerow(("segment", "frame", "scene_quads", "scene_draws", "present_return_ns", "timing_state", "gpu_ns",
                     "arm_status", "query_status"))
    with path.open("rb") as stream:
        for segment in SEGMENTS:
            durations = []
            intervals = []
            counts = set()
            draws = set()
            for index in range(FRAMES):
                frame, quads, packets, interval, state, nanoseconds, arm, query = RECORD.unpack(stream.read(RECORD.size))
                if frame != index or interval == 0:
                    raise ValueError(f"Invalid frame or interval at {segment}/{index}")
                available = (state, arm, query) == (3, 0, 0)
                if available:
                    durations.append(nanoseconds / 1e6)
                intervals.append(interval / 1e6)
                counts.add(quads)
                draws.add(packets)
                writer.writerow((segment, frame, quads, packets, interval, state, nanoseconds if available else "", arm, query))
            pixels = stream.read(width * height * 4)
            if len(pixels) != width * height * 4:
                raise ValueError(f"Incomplete display image at {segment}")
            png(destination / f"{path.stem}-{segment}.png", width, height, pixels)
            rgb = struct.iter_unpack("4B", pixels)
            lit = sum(any((r, g, b)) for r, g, b, _ in rgb)
            valid[segment] = durations
            summary[segment] = dict(frames=FRAMES, available=len(durations), scene_quads=sorted(counts), scene_draws=sorted(draws),
                                    nonblack_pixels=lit, gpu_ms=summarize(durations), interval_ms=summarize(intervals))
        if stream.read(1):
            raise ValueError("Unexpected trailing records")
    (destination / f"{path.stem}.csv.gz").write_bytes(gzip.compress(rows.getvalue().encode(), mtime=0))
    return {"verdict": verdict(valid), "segments": summary}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stream", type=Path)
    parser.add_argument("size", choices=SIZES)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    args.destination.mkdir(exist_ok=True, parents=True)
    result = decode(args.stream, args.destination, args.size)
    (args.destination / f"{args.stream.stem}.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
