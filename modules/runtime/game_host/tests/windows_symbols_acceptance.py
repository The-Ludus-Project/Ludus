#!/usr/bin/env python3
"""Windows fixture DLLs and their linked PDBs pass the live-play symbol check; swapped PDBs fail."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts/python"))
sys.dont_write_bytecode = True

from play_probe import ProbeError  # noqa: E402
import play_windows  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--a", type=Path, required=True)
    parser.add_argument("--a-pdb", type=Path, required=True)
    parser.add_argument("--b", type=Path, required=True)
    parser.add_argument("--b-pdb", type=Path, required=True)
    args = parser.parse_args()
    first = play_windows.identity(args.a, args.a_pdb)
    second = play_windows.identity(args.b, args.b_pdb)
    if first["artifact_type"] != "module" or second["artifact_type"] != "module":
        raise SystemExit("fixture DLLs must be identified as gameplay modules")
    if first["build_id"] == second["build_id"]:
        raise SystemExit("distinct fixture builds share a CodeView identity")
    for image, symbols in ((args.a, args.b_pdb), (args.b, args.a_pdb)):
        try:
            play_windows.identity(image, symbols)
        except ProbeError:
            continue
        raise SystemExit(f"{symbols.name} was accepted for {image.name}")
    print("Fixture DLLs match their PDBs; swapped PDBs are rejected.")


if __name__ == "__main__":
    main()
