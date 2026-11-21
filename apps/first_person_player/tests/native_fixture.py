"""Drive a cooked floor and wall through the actual macOS player."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def make_fixture(path):
    # Version 4 indexed LUSCN. The wall faces +Z two metres in front of spawn.
    scene = bytearray(b"LUSCN01\0")

    def put(schema, *values):
        scene.extend(struct.pack("<" + schema, *values))

    put("5I10f", 4, 2, 1, 1, 2, 0, 1.6, 0, 0, 1.6, -1, 0, 1, 0, 1)
    put("3I4B", 1, 1, 4, 180, 130, 70, 255)
    put("2I5fIQfI", 0, 0, 0, .6, 0, 0, 0, 0, 0, 0, 0)
    for vertices, normal in [
        ([(-8, 0, -8), (-8, 0, 8), (8, 0, 8), (8, 0, -8)], (0, 1, 0)),
        ([(-1, 0, -2), (1, 0, -2), (1, 4, -2), (-1, 4, -2)], (0, 0, 1)),
    ]:
        put("2I", 4, 6)
        for position in vertices:
            put("8f", *position, *normal, 0, 0)
        put("6I", 0, 1, 2, 0, 2, 3)
    for index in range(2):
        put("2I12fQ", index, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, index + 1)
    put("4I3f", 1, 1, 0, 0, 0, 0, 0)
    put("Q2I12f", 3, 0, 1, 100, 100, 100, 0, 0, 0, 0, -.8, -.6, 100, .9, .8)
    zero = [0, 0, 0]
    sky = dict(enabled=True, anchorWorld=zero, upWorld=[0, 1, 0], toSunWorld=[0, .8, .6],
               solarIlluminance=[100]*3, solarRadiusRadians=.004675, sunSourceId="3", lightScene=True,
               celestialEnabled=False, starRadiance=0, toMoonWorld=[0, 0, 1], lunarPeakRadiance=zero,
               lunarRadiusRadians=0, milkyWayRadiance=0, galacticNorthWorld=[0, 1, 0],
               galacticCenterWorld=[0, 0, -1], toSunAtMoonWorld=zero,
               planets=[dict(directionWorld=[0, 1, 0], illuminance=zero)] * 8)
    digest = hashlib.sha256(scene).hexdigest()
    header = dict(version=1, renderer="raster", world="room", source="room.world.json", worldSha256=digest,
                  sceneSha256=digest, entities=[dict(id="room"), dict(id="start", spawn=dict(
                      eyeWorld=[0, 1.601, 0], orientation=[0, 0, 0, 1], speed=4))],
                  objects=[dict(entity="room", sourceId=str(i), staticCollider=True) for i in (1, 2)],
                  artifacts=[], warnings=[], earthObserver={}, atmosphere=sky)
    path.write_bytes(json.dumps(header).encode() + b"\n" + scene)


def verify(player):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "room.luplay"
        make_fixture(path)
        for file, spawn, repair in [(path, "absent", "Choose an existing"), (path.with_suffix(".missing"), "start", "Prepare the World")]:
            failed = subprocess.run([player, file, spawn], capture_output=True, text=True, timeout=15)
            if failed.returncode == 0 or repair not in failed.stdout + failed.stderr:
                raise RuntimeError(f"Expected repair '{repair}': {failed.stdout}{failed.stderr}")
        run = subprocess.run([player, path, "start", "--verify-fixture"], capture_output=True, text=True, timeout=180)
        output = run.stdout + run.stderr
        if run.returncode:
            raise RuntimeError(f"Player exited {run.returncode}: {output[-4000:]}")
        poses = {int(frame): tuple(map(float, values)) for frame, *values in
                 re.findall(r"Fixture frame (\d+) eye (\S+) (\S+) (\S+) yaw (\S+)", output)}
        # Walk into the wall, then strafe right along an unobstructed route.
        if not (-1.80 < poses[59][2] < -1.70 and poses[120][0] > 3.5
                and max(p[1] for p in poses.values()) > 2.3 and poses[120][3] < 0):
            raise RuntimeError(f"Unexpected movement: {poses[59]}, {poses[120]}")
        samples = [tuple(map(int, values)) for values in
                   re.findall(r"Fixture sample (\d+) rgb (\d+) textures (\d+) shadow lights (\d+)", output)]
        if (len(samples) != 2 or samples[0][1] == samples[1][1]
                or any(not (rgb and textures and lights) for _, rgb, textures, lights in samples)):
            raise RuntimeError(f"Expected distinct wall and sky pixels with textures and shadow maps: {samples}")
        print(f"Metal presented {len(poses)} frames. Wall hit z={poses[59][2]}, clear strafe x={poses[120][0]}.")


if __name__ == "__main__":
    if sys.argv[1] == "--write":
        make_fixture(Path(sys.argv[2]))
    else:
        verify(sys.argv[1])
