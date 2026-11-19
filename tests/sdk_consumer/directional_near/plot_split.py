"""Plot the doorway split from a measured Ludus directional replay (requires Matplotlib)."""

import argparse
import csv
import math
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
from matplotlib import pyplot as plt


def read_crossing(path):
    footprints = {}
    blocked = {}
    errors = {0: [], 1: []}
    with path.open() as stream:
        for row in csv.reader(stream):
            if row[:1] == ["footprint"]:
                footprints[int(row[1]), int(row[2])] = tuple(map(float, row[4:8]))
            if row[:1] != ["cascade"] or row[3] != "1":
                continue
            mode, revision, light = int(row[1]), int(row[2]), int(row[4])
            eye_x, horizontal_distance, eye_height = map(float, row[6:9])
            if eye_x != 0 or horizontal_distance <= 0:
                continue
            visibility = int(row[10]) / 255
            expected = 1 - int(row[9])
            errors[mode].append(abs(visibility - expected))
            if light == 0 and expected == 0:
                # Even-sized image, first pixel below centre. Intersect its ray with the floor.
                distance = math.hypot(horizontal_distance, eye_height)
                depth = distance / (1 + horizontal_distance * math.tan(0.1) / (64 * eye_height))
                blocked[mode, depth] = (visibility, footprints[mode, revision], eye_height)
    depths = sorted(depth for mode, depth in blocked if mode == 0)
    if len(depths) < 3 or any((1, depth) not in blocked for depth in depths):
        raise ValueError("Expected a measured split crossing in both near and whole-scene modes")
    return depths, blocked, errors


def plot_split(log, output):
    depths, blocked, errors = read_crossing(log)
    focal_length = 32 / math.tan(0.1)
    figure, (footprint, visibility) = plt.subplots(2, 1, figsize=(8, 6.5), sharex=True, layout="constrained")
    for mode, label, color in ((1, "Whole scene", "#9b4118"), (0, "Near", "#006c76")):
        active = depths if mode == 1 else [depth for depth in depths if depth < 12]
        across, along = [], []
        for depth in active:
            _, ground, height = blocked[mode, depth]
            ground_x, ground_y = ground[2:] if mode == 1 else ground[:2]
            across.append(focal_length * ground_x / depth)
            along.append(focal_length * height * ground_y / depth**2)
        footprint.plot(active, across, "o-", color=color, label=f"{label}, across view")
        footprint.plot(active, along, "s--", color=color, label=f"{label}, along view")

    footprint.set_yscale("log")
    footprint.set_ylabel("Receiver pixels per shadow texel")
    footprint.legend(ncols=2, fontsize=9)
    footprint.set_title("Local sampling on the doorway floor", loc="left")

    visibility.plot(depths, [blocked[1, depth][0] for depth in depths], "s--", color="#9b4118",
                    label="Whole scene (GPU)")
    visibility.plot(depths, [blocked[0, depth][0] for depth in depths], "o-", color="#006c76",
                    label="10–12 m split (GPU)")
    sample_depths = [depths[0] + i * (depths[-1] - depths[0]) / 200 for i in range(201)]
    blend = [min(1, max(0, (depth - 14) / 2)) for depth in sample_depths]
    visibility.plot(sample_depths, [t * t * (3 - 2 * t) for t in blend], ":", color="#5d448a",
                    linewidth=2.5, label="14–16 m split (offline)")
    visibility.axhline(0, color="#202020", linewidth=1, label="Geometric visibility", zorder=0)
    visibility.set_ylabel("Direct visibility")
    visibility.set_xlabel("Camera-forward depth at the sampled floor pixel (metres)")
    visibility.set_title("Blocked probe through the split", loc="left")
    visibility.legend(fontsize=9, loc="center left")
    for axis in (footprint, visibility):
        axis.axvspan(10, 12, color="#e3ebec", zorder=-2)
        axis.grid(axis="y", alpha=0.2)
        axis.spines[["right", "top"]].set_visible(False)
    figure.savefig(output, dpi=150)
    plt.close(figure)
    for mode, values in errors.items():
        values.sort()
        p95 = values[math.ceil(0.95 * len(values)) - 1]
        print(f"Mode {mode}, {len(values)} samples, median {statistics.median(values):.4f}, "
              f"p95 {p95:.4f}, maximum {max(values):.4f}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay_log", type=Path)
    parser.add_argument("output_png", type=Path)
    arguments = parser.parse_args()
    plot_split(arguments.replay_log, arguments.output_png)
