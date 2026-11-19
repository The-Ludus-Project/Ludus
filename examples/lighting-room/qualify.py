"""Exercise the player's interactive render path and check its scene-linear readbacks."""
import argparse
import hashlib
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def check_output(text):
    """Verify the Forward Lit fallback has the expected isolated light contributions."""
    samples = {}
    for name, red, green, blue, charts, probes, milliseconds in re.findall(
        r"^([\w-]+) rgb8=(\d+) (\d+) (\d+) chart_draws=(\d+) probe_draws=(\d+) gpu_ms=([\d.]+)$",
        text, re.MULTILINE
    ):
        if name in samples or int(charts) or int(probes) or not (0 < float(milliseconds) < math.inf):
            raise ValueError(f"{name}: duplicate sample, unexpected bake inputs or invalid GPU measurement")
        samples[name] = tuple(map(int, (red, green, blue)))

    def require(condition, message):
        if not condition:
            raise ValueError(message)

    require(re.search(r"^Metal adapter=.+$", text, re.MULTILINE), "Missing measured Metal adapter")
    world = Path(__file__).parent / "authoring" / "lighting.world.json"
    digest = hashlib.sha256(world.read_bytes()).hexdigest()
    identity = re.search(rf"^Forward Lit world={digest} hosted=[01] size=(\d+)x(\d+) startup_ms=([\d.]+)$",
                         text, re.MULTILINE)
    require(identity and all(float(value) > 0 for value in identity.groups()),
            "Player must report the measured drawable and startup time for this authored World")
    for name in ("saturated", "neutral", "blocked", "dark", "lit-side", "opening-left",
                 "opening-right", "dark-side", "divider", "outside"):
        require(max(samples[name]) <= 1, f"{name}: direct light grazes the receiver and the environment is black")
    for name in ("bounce-live", "outside-live"):
        require(samples[name][2] > samples[name][0] + 2, f"{name}: the switchable blue point light must reach the receiver")
    require(samples["peak"][0] > samples["oblique"][0] + 2 and samples["oblique"][0] > 2,
            "The rough patch must retain both diffuse light and its view-dependent specular peak")
    # The down-facing 2 lux white source gives the 0.5 albedo plane 1/pi radiance
    # before Fresnel transmission. For a dielectric the direct diffuse weight uses
    # the view/half-vector cosine. The remaining signal is the live specular lobe.
    specular = {}
    for name, height, depth in (("peak", 2, .01), ("oblique", 1, -2)):
        view_cosine = height / math.hypot(height, depth)
        half_cosine = math.sqrt((1 + view_cosine) / 2)
        diffuse = .96 * (1 - (1 - half_cosine) ** 5) / math.pi
        specular[name] = samples[name][0] / 255 - diffuse
    require(specular["oblique"] >= -1 / 255 and specular["peak"] > specular["oblique"] + .06,
            "The selected light must produce a live specular peak above its diffuse component")
    for base, live, combined in zip(samples["peak"], samples["live-only"], samples["rough-live"]):
        require(abs(base + live - combined) <= 2, "Isolated light contributions must add within two RGBA8 codes")
    return samples


def run(player):
    with tempfile.TemporaryDirectory(prefix="lighting-room-launch-") as cwd:
        # The package owns its content. Preserve the display/host connection, while removing SDK and source hints.
        environment = {k: v for k, v in os.environ.items()
                       if not k.startswith(("LUDUS_", "DYLD_", "CMAKE_")) or k == "LUDUS_HOSTED_WINDOW"}
        result = subprocess.run([str(player.resolve()), "--qualify"], cwd=cwd, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        print(result.stdout, end="")
        result.check_returncode()
        check_output(result.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("player", type=Path)
    try:
        run(parser.parse_args().player)
    except (OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        sys.exit(str(error))
