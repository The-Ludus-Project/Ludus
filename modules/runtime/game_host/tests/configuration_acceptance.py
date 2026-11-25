"""Exercise configured startup through the real host and real gameplay module."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def bundle(layer, frames):
    return {"version": 1, "schema": "ludus.host.v1", "layer": layer, "values": [
        {"key": "host.headless", "type": "bool", "value": True, "source": "test", "line": 0},
        {"key": "host.max_frames", "type": "uint64", "value": str(frames), "source": "test", "line": 0},
    ]}


def write(destination, value):
    destination.write_text(json.dumps(value), encoding="utf-8")


def run(flags, *, rejection=None):
    result = subprocess.run([host, "--module", module, *flags], capture_output=True, timeout=5)
    if rejection is None and result.returncode != 0:
        raise RuntimeError(f"Configured host failed for {flags}: {result.stderr!r}")
    if rejection is not None and (result.returncode == 0 or rejection not in result.stderr):
        raise RuntimeError(f"Expected {rejection!r} for {flags}: {result.stderr!r}")


host, module = sys.argv[1:]
with tempfile.TemporaryDirectory() as directory:
    project = Path(directory) / "project.json"
    preference = Path(directory) / "preference.json"
    startup = Path(directory) / "startup.json"
    write(project, bundle("project", 1))
    write(preference, bundle("preference", 0))  # Unlimited unless Launch wins.
    run(["--config", str(project)])
    run(["--max-frames", "2", "--preferences", str(preference), "--config", str(project)])

    # For each startup rank the higher rank supplies the only finite limit.
    # Reverse JSON declaration order to exercise precedence independently of parsing.
    ranks = ("engine", "project", "profile", "device", "quality")
    startup_flags = ["--startup-config", str(startup)]
    for count in range(1, len(ranks) + 1):
        layers = {rank: bundle(rank, int(rank == ranks[count - 1])) for rank in reversed(ranks[:count])}
        write(startup, {"version": 1, "layers": layers})
        run(startup_flags)

    layers["quality"] = bundle("quality", 0)
    write(startup, {"version": 1, "layers": layers})
    write(preference, bundle("preference", 1))
    run(["--preferences", str(preference), *startup_flags])
    write(preference, bundle("preference", 0))
    run(["--max-frames", "1", *startup_flags, "--preferences", str(preference)])
    run([*startup_flags, "--config", str(project)], rejection=b"Choose either --config")

    write(project, {})
    run(["--headless", "--max-frames", "1", "--config", str(project)], rejection=b"recook the project layer")
    launch = ["--headless", "--max-frames", "1", *startup_flags]
    for document, message in (
        ({"version": 2, "layers": {}}, b"Invalid startup configuration"),
        ({"version": 1, "layers": {"preference": bundle("preference", 1)}}, b"Unsupported rank"),
        ({"version": 1, "layers": {"quality": bundle("project", 1)}}, b"recook the quality layer"),
        ({"version": 1, "layers": {"engine": bundle("engine", -1), "quality": bundle("quality", 1)}},
         b"recook the engine layer"),
    ):
        write(startup, document)
        run(launch, rejection=message)
    startup.write_text(" " * 262145, encoding="utf-8")
    run(launch, rejection=b"Cannot read configuration")
