"""Run the same authored room loose and packed, then reject a corrupt archive."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(command, expected_success=True):
    result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=30)
    output = result.stdout + result.stderr
    if (result.returncode == 0) != expected_success:
        raise RuntimeError(f"Unexpected exit {result.returncode} for {command}\n{output}")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("player", type=Path)
    parser.add_argument("pack_tool", type=Path)
    parser.add_argument("levels", type=Path)
    parser.add_argument("--windowed", action="store_true")
    arguments = parser.parse_args()
    room = "first-room.world.json"
    with tempfile.TemporaryDirectory(prefix="ludus packed world ") as temporary:
        pack = Path(temporary) / "levels.lupak"
        run([sys.executable, arguments.pack_tool, arguments.levels, pack])
        packed = [arguments.player, "--level-pack", pack, room]
        loose = [arguments.player, "--level", arguments.levels / room]
        headless = ["--headless", "--frames", "12"]
        replay = re.compile(r"Headless reference: ticks=12 hash=(\d+)")
        packed_hash = replay.search(run([*packed, *headless]))
        loose_hash = replay.search(run([*loose, *headless]))
        if packed_hash is None or loose_hash is None or packed_hash[1] != loose_hash[1]:
            raise RuntimeError("Packed and loose rooms did not produce the same 12-tick replay hash.")
        if arguments.windowed:
            agility = arguments.player.parent / "D3D12"
            for filename in ("D3D12Core.dll", "d3d12SDKLayers.dll", "LICENSE.txt"):
                if not (agility / filename).is_file():
                    raise RuntimeError(f"Missing Agility runtime file {agility / filename}. Rebuild the player.")
            output = run([*packed, "--frames", "3", "--follow-camera"])
            if os.environ.get("LUDUS_TEST_REQUIRE_WARP") and "Direct3D 12 software adapter" not in output:
                raise RuntimeError(f"The windowed test did not select WARP.\n{output}")
            if "Reference presented 3 frames" not in output:
                raise RuntimeError(f"The packed room did not present three frames.\n{output}")
        pack.write_bytes(b"invalid pack")
        run([*packed, *headless], expected_success=False)


if __name__ == "__main__":
    main()
