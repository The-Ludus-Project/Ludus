"""Warn when a CTest entry uses most of its TIMEOUT.

A CTest TIMEOUT bounds a whole test process, so a Catch2 executable that gains
cases creeps toward its deadline unnoticed until a batch fails on it. This check
runs after a ctest invocation and compares each test's measured time, read from
``ctest --output-junit``, with its effective timeout:

* the test's TIMEOUT property, from ``ctest --show-only=json-v1``;
* otherwise the build tree's default (``TimeOut:`` in DartConfiguration.tcl,
  1500 s unless DART_TESTING_TIMEOUT changes it), or ``--default-timeout`` when
  the ctest command line passed ``--timeout``.

Each test above ``--threshold`` of its timeout gets a GitHub ``::warning``
annotation, and the closest tests are tabulated in ``$GITHUB_STEP_SUMMARY``. The
check never fails a job: unreadable or missing inputs become warnings and the exit
status is zero, so headroom is visible without becoming a gate. A test that timed
out has already failed its ctest step; it is reported here at 100% or more.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass
from pathlib import Path

DEFAULT_THRESHOLD = 0.75
DEFAULT_SUMMARY_ROWS = 10
DART_CONFIGURATION = "DartConfiguration.tcl"
DART_TIMEOUT_PATTERN = re.compile(r"^TimeOut:\s*([0-9.]+)\s*$", re.MULTILINE)
ANNOTATION_TITLE = "CTest timeout headroom"


@dataclass(frozen=True)
class Measurement:
    """One test's measured time against its effective timeout."""

    name: str
    elapsed_seconds: float
    timeout_seconds: float | None
    timeout_source: str
    has_timed_out: bool

    @property
    def fraction(self) -> float | None:
        if self.timeout_seconds is None or self.timeout_seconds <= 0:
            return None
        return self.elapsed_seconds / self.timeout_seconds


def read_timeouts(listing: dict) -> dict[str, float]:
    """Return each test's TIMEOUT property from a ctest json-v1 listing."""
    timeouts = {}
    for test in listing.get("tests", []):
        for entry in test.get("properties", []):
            if entry.get("name") == "TIMEOUT":
                try:
                    timeouts[test["name"]] = float(entry["value"])
                except (KeyError, TypeError, ValueError):
                    pass
    return timeouts


def read_default_timeout(text: str) -> float | None:
    """Return the ``TimeOut:`` value of a DartConfiguration.tcl, if it has one."""
    match = DART_TIMEOUT_PATTERN.search(text)
    if match is None:
        return None
    value = float(match.group(1))
    return value if value > 0 else None


def read_results(text: str) -> dict[str, tuple[float, bool]]:
    """Return ``{name: (seconds, timed out)}`` for each test case that ran.

    Cases CTest did not run (``notrun``/``disabled``) or that lack a time are
    omitted. A repeated name (``ctest --repeat``) keeps its longest run.
    """
    root = ElementTree.fromstring(text)
    results: dict[str, tuple[float, bool]] = {}
    for case in root.iter("testcase"):
        name = case.get("name")
        if name is None or case.get("status") in ("notrun", "disabled"):
            continue
        try:
            seconds = float(case.get("time", ""))
        except ValueError:
            continue
        failure = case.find("failure")
        has_timed_out = failure is not None and failure.get("message") == "Timeout"
        previous = results.get(name)
        if previous is None or seconds > previous[0]:
            results[name] = (seconds, has_timed_out)
    return results


def measure(
    results: dict[str, tuple[float, bool]],
    timeouts: dict[str, float],
    default_timeout: float | None,
) -> list[Measurement]:
    """Pair each result with its effective timeout, closest to the deadline first."""
    measurements = []
    for name, (seconds, has_timed_out) in results.items():
        timeout = timeouts.get(name)
        source = "TIMEOUT"
        if timeout is None or timeout <= 0:
            timeout = default_timeout
            source = "default" if default_timeout is not None else "none"
        measurements.append(Measurement(name, seconds, timeout, source, has_timed_out))
    return sorted(measurements, key=lambda item: (-(item.fraction or -1.0), item.name))


def is_over(measurement: Measurement, threshold: float) -> bool:
    fraction = measurement.fraction
    return measurement.has_timed_out or (fraction is not None and fraction > threshold)


def format_warning(measurement: Measurement, threshold: float) -> str:
    """Return the GitHub annotation for a test over the threshold."""
    fraction = measurement.fraction
    state = "timed out after" if measurement.has_timed_out else "used"
    if fraction is None:
        budget = "an unknown timeout"
    else:
        owner = "its" if measurement.timeout_source == "TIMEOUT" else "the default"
        budget = f"{owner} {measurement.timeout_seconds:g} s timeout ({fraction:.0%})"
    return (
        f"::warning title={ANNOTATION_TITLE}::{measurement.name} {state} "
        f"{measurement.elapsed_seconds:.1f} s of {budget}; warning above {threshold:.0%}. "
        f"Split the test or raise its TIMEOUT with a measurement before it fails a batch."
    )


def format_summary(
    heading: str, measurements: list[Measurement], threshold: float, rows: int
) -> str:
    """Return a Markdown summary of the tests closest to their timeouts."""
    over = sum(1 for item in measurements if is_over(item, threshold))
    unbounded = sum(1 for item in measurements if item.fraction is None)
    lines = [
        f"### {heading}",
        "",
        f"{len(measurements)} tests measured; {over} above {threshold:.0%} of their timeout"
        + (f"; {unbounded} without a timeout." if unbounded else "."),
        "",
    ]
    bounded = [item for item in measurements if item.fraction is not None][:rows]
    if bounded:
        lines += ["| Test | Elapsed (s) | Timeout (s) | Used | |", "| --- | ---: | ---: | ---: | --- |"]
        for item in bounded:
            source = "" if item.timeout_source == "TIMEOUT" else " (default)"
            flag = "timed out" if item.has_timed_out else ("warning" if is_over(item, threshold) else "")
            lines.append(
                f"| `{item.name}` | {item.elapsed_seconds:.1f} | {item.timeout_seconds:g}{source} "
                f"| {item.fraction:.0%} | {flag} |"
            )
        lines.append("")
    return "\n".join(lines) + "\n"


def list_tests(ctest: str, build: Path) -> dict | None:
    try:
        listing = subprocess.run(
            [ctest, "--test-dir", str(build), "--show-only=json-v1"],
            check=True,
            capture_output=True,
            text=True,
        )
        return json.loads(listing.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"::warning title={ANNOTATION_TITLE}::could not list tests in {build}: {error}")
        return None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build", type=Path, help="configured build tree the tests came from")
    parser.add_argument(
        "--junit", type=Path, action="append", required=True, help="ctest --output-junit file (repeatable)"
    )
    parser.add_argument("--ctest", default="ctest", help="ctest executable used to list timeouts")
    parser.add_argument("--tests-json", type=Path, help="saved ctest --show-only=json-v1 output instead of --ctest")
    parser.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD, help="warn above this fraction")
    parser.add_argument("--default-timeout", type=float, help="timeout of tests without TIMEOUT (ctest --timeout)")
    parser.add_argument("--summary", type=Path, help="Markdown summary file (default: $GITHUB_STEP_SUMMARY)")
    parser.add_argument("--title", default=ANNOTATION_TITLE, help="summary heading")
    parser.add_argument("--rows", type=int, default=DEFAULT_SUMMARY_ROWS, help="summary rows")
    arguments = parser.parse_args(argv)

    results: dict[str, tuple[float, bool]] = {}
    for path in arguments.junit:
        try:
            file_results = read_results(path.read_text(encoding="utf-8"))
        except (OSError, ElementTree.ParseError) as error:
            # The ctest step did not run or stopped early; its own status reports that.
            print(f"::notice title={ANNOTATION_TITLE}::no CTest results in {path}: {error}")
            continue
        for name, result in file_results.items():
            if name not in results or result[0] > results[name][0]:
                results[name] = result
    if not results:
        return 0

    if arguments.tests_json is not None:
        try:
            listing = json.loads(arguments.tests_json.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"::warning title={ANNOTATION_TITLE}::could not read {arguments.tests_json}: {error}")
            listing = None
    else:
        listing = list_tests(arguments.ctest, arguments.build)
    timeouts = read_timeouts(listing) if listing is not None else {}

    default_timeout = arguments.default_timeout
    if default_timeout is None:
        try:
            default_timeout = read_default_timeout(
                (arguments.build / DART_CONFIGURATION).read_text(encoding="utf-8")
            )
        except OSError:
            default_timeout = None

    measurements = measure(results, timeouts, default_timeout)
    for item in measurements:
        if is_over(item, arguments.threshold):
            print(format_warning(item, arguments.threshold))
    summary = format_summary(arguments.title, measurements, arguments.threshold, arguments.rows)
    print(summary, end="")
    summary_path = arguments.summary
    if summary_path is None and os.environ.get("GITHUB_STEP_SUMMARY"):
        summary_path = Path(os.environ["GITHUB_STEP_SUMMARY"])
    if summary_path is not None:
        try:
            with summary_path.open("a", encoding="utf-8") as summary_file:
                summary_file.write(summary)
        except OSError as error:
            print(f"::warning title={ANNOTATION_TITLE}::could not write {summary_path}: {error}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
