"""Validate and upload the debug files for a staged macOS Editor executable."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
from uuid import UUID


def macho_uuids(path: Path) -> dict[str, UUID]:
    result = subprocess.run(
        ["xcrun", "dwarfdump", "--uuid", str(path)], check=True, capture_output=True, text=True
    )
    uuids = {}
    for line in result.stdout.splitlines():
        match = re.fullmatch(r"UUID: ([\da-fA-F-]+) \(([^)]+)\) .+", line)
        if match is None or match[2] in uuids:
            raise ValueError(f"Unexpected UUID output for {path}: {line}")
        uuids[match[2]] = UUID(match[1])
    if not uuids:
        raise ValueError(f"No Mach-O UUIDs found in {path}")
    return uuids


def upload_symbols(cli: Path, executable: Path, dwarf: Path) -> None:
    if not os.environ.get("SENTRY_AUTH_TOKEN"):
        raise ValueError("Set SENTRY_AUTH_TOKEN in the macos-editor-distribution environment.")

    executable_uuids = macho_uuids(executable)
    dwarf_uuids = macho_uuids(dwarf)
    if executable_uuids != dwarf_uuids:
        raise ValueError(f"Editor UUIDs {executable_uuids} differ from dSYM UUIDs {dwarf_uuids}")

    for architecture, identifier in executable_uuids.items():
        # LLVM's summary lists the size of the section without dumping line tables.
        sections = subprocess.run(
            ["xcrun", "dwarfdump", "--arch", architecture, "--show-section-sizes", str(dwarf)],
            check=True, capture_output=True, text=True,
        ).stdout
        line_section = re.search(r"^\s*__debug_line[ \t]+(\d+)\b", sections, re.MULTILINE)
        if line_section is None or int(line_section[1]) == 0:
            raise ValueError(f"The Editor dSYM has no DWARF line table for {architecture}")
        print(f"Verified Editor dSYM for {architecture}: {identifier}", flush=True)

    upload = [
        str(cli), "debug-files", "upload", "--org", "the-ludus-project",
        "--project", "ludus-editor", "--wait", "--require-all", "--no-sources",
    ]
    for identifier in executable_uuids.values():
        upload.extend(("--id", str(identifier)))
    # Check the dSYM by itself so an executable with the same ID cannot satisfy
    # the CLI's required-ID check if the dSYM was not accepted.
    subprocess.run([*upload, str(dwarf)], check=True)
    subprocess.run([*upload, "--no-debug", str(executable)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sentry_cli", type=Path)
    parser.add_argument("executable", type=Path)
    parser.add_argument("dwarf", type=Path)
    arguments = parser.parse_args()
    try:
        upload_symbols(arguments.sentry_cli, arguments.executable, arguments.dwarf)
    except subprocess.CalledProcessError as error:
        print(error.stderr or error, file=sys.stderr)
        return 1
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
