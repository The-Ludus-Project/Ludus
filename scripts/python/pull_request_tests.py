"""Check the quick unit and Editor tests that every pull request runs.

config/pull_request_tests.txt names CTest entries that pull requests run on the
Linux Editor build, which already compiles them. CTest's ``--tests-from-file``
silently skips a name that no longer exists, so a renamed test would drop out of
pull-request coverage. This check runs before those tests and fails instead.

It also keeps the list quick and self-contained: each entry must be a ``unit`` or
``editor`` test (the batch's run of those labels excludes the list, so each test
runs once there), must not need a GPU, fixtures or a resource lock, and must not
declare a timeout above MAX_TIMEOUT_SECONDS. Measured duration is not checked
here; the list's header records how its entries were chosen.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIST = ROOT / "config/pull_request_tests.txt"
# The bound quick generated-source checks also keep; pull requests pass it to ctest --timeout
# for entries that declare none.
MAX_TIMEOUT_SECONDS = 120
REQUIRED_LABELS = {"unit", "editor"}
REJECTED_LABELS = {"gpu", "build-contract", "generated-source"}
REJECTED_PROPERTIES = ("FIXTURES_REQUIRED", "FIXTURES_SETUP", "FIXTURES_CLEANUP", "RESOURCE_LOCK")


def read_names(text: str) -> list[str]:
    """Return the listed test names, ignoring comments and blank lines."""
    return [line.strip() for line in text.splitlines() if line.strip() and not line.startswith("#")]


def list_failures(names: list[str]) -> list[str]:
    failures = []
    if names != sorted(set(names)):
        failures.append(f"{LIST.relative_to(ROOT)}: keep one name per line, sorted and unique")
    return failures


def test_failures(names: list[str], tests: list[dict]) -> list[str]:
    """Compare the listed names with ctest --show-only=json-v1 test records."""
    records = {test["name"]: test for test in tests}
    failures = []
    for name in names:
        record = records.get(name)
        if record is None:
            failures.append(
                f"{name}: no such CTest entry; rename or remove it in {LIST.relative_to(ROOT)}"
            )
            continue
        properties = {entry["name"]: entry["value"] for entry in record.get("properties", [])}
        labels = set(properties.get("LABELS", []))
        if not labels & REQUIRED_LABELS:
            failures.append(f"{name}: label it unit or editor, or remove it from the list")
        rejected = sorted(labels & REJECTED_LABELS)
        if rejected:
            failures.append(f"{name}: labelled {', '.join(rejected)}; it belongs to a batch suite")
        for key in REJECTED_PROPERTIES:
            if key in properties:
                failures.append(f"{name}: uses {key}; quick tests run alone, without fixtures or locks")
        timeout = float(properties.get("TIMEOUT", 0))
        if timeout > MAX_TIMEOUT_SECONDS:
            failures.append(
                f"{name}: TIMEOUT {timeout:g} s exceeds {MAX_TIMEOUT_SECONDS} s; keep it in the batch"
            )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="configured build tree that defines the tests")
    parser.add_argument("--ctest", default="ctest", help="ctest executable")
    arguments = parser.parse_args()
    names = read_names(LIST.read_text(encoding="utf-8"))
    listing = subprocess.run(
        [arguments.ctest, "--test-dir", str(arguments.build), "--show-only=json-v1"],
        text=True,
        capture_output=True,
        check=True,
    ).stdout
    failures = list_failures(names) + test_failures(names, json.loads(listing)["tests"])
    for failure in failures:
        print(f"error: {failure}", file=sys.stderr)
    if not failures:
        print(f"Quick pull-request tests: {len(names)} entries checked.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
