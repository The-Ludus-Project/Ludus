"""Check a constant-geometry, pixel-work estimate on the withheld camera view."""
import json
from pathlib import Path
import statistics
import sys

from analyze import BUDGET_MS, SIZES


def fit(rows):
    # At fixed geometry the intercept includes its constant cost. Internal pixels
    # account for full-screen passes, nonblack pixels proxy projected room area.
    x, y, t = zip(*rows)
    mx, my, mt = map(statistics.mean, (x, y, t))
    xx = sum((v - mx)**2 for v in x)
    yy = sum((v - my)**2 for v in y)
    xy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    xt = sum((a - mx) * (c - mt) for a, c in zip(x, t))
    yt = sum((b - my) * (c - mt) for b, c in zip(y, t))
    determinant = xx * yy - xy * xy
    if determinant <= 0:
        raise ValueError("Independent pixel-work observations required")
    a = (yy * xt - xy * yt) / determinant
    b = (xx * yt - xy * xt) / determinant
    return mt - a * mx - b * my, a, b


def main(directory):
    training = []
    heldout = []
    for size, (width, height) in SIZES.items():
        record = json.loads((directory / f"{size}-timed.json").read_text())
        if record["verdict"] == "inconclusive":
            raise ValueError(f"Insufficient timing coverage in {size}")
        for segment, sample in record["segments"].items():
            row = width * height / 1e6, sample["nonblack_pixels"] / 1e6, sample["gpu_ms"]["p95"]
            if segment == "holdout":
                heldout.append((size, row))
            else:
                training.append(row)
    intercept, pixels, room = fit(training)
    predictions = []
    for size, (x, y, actual) in heldout:
        predicted = intercept + x * pixels + y * room
        predictions.append(dict(size=size, measured_ms=actual, predicted_ms=predicted,
                                absolute_error_ms=abs(actual - predicted),
                                false_under_budget=predicted <= BUDGET_MS < actual))
    print(json.dumps(dict(intercept_ms=intercept, ms_per_megapixel=pixels, room_ms_per_megapixel=room,
                          heldout=predictions), indent=2))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
