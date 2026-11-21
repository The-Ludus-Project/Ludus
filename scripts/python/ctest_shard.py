"""Select one CTest shard's tests from a listing every shard takes identically.

A CI job split round-robin across a matrix of runners must give each test exactly
one shard. ``ctest --tests-information <shard>,,<count>`` strides over the test
numbers of the tree it runs in, so a shard whose earlier steps reconfigure that
tree numbers the tests differently: the Linux ASan/UBSan shard 1 ran scripting and
ICU probes that registered about 17 more tests, and the four shards then ran 87
tests twice and never ran as many others.

Run this while every shard's build tree is still identical: after the build,
before any shard-specific step. It lists the suite with ``ctest --show-only=json-v1``
(in registration order, the order ``--tests-information`` numbers), writes every
``--shards``-th name from ``--shard`` to ``--output``, one per line, for
``ctest --tests-from-file``, and records the listing's size and SHA-256 in the
step summary so the shards can be compared. New tests join the rotation without
a list to maintain. Tests registered after the listing run only where the step
that registered them runs them.

Failures are errors: an empty listing, an empty shard, an invalid shard number or
an unreadable listing exits non-zero, so a shard cannot pass by running nothing.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ANNOTATION_TITLE = "CTest shard"


class ShardError(Exception):
    """A listing or shard request that cannot produce a disjoint, complete split."""


def read_names(listing: dict) -> list[str]:
    """Return the test names of a ctest json-v1 listing in listing order."""
    names = []
    for test in listing.get("tests", []):
        name = test.get("name")
        if isinstance(name, str) == False or name == "" or "\n" in name:
            raise ShardError(f"the listing has an unusable test name: {name!r}")
        names.append(name)
    if len(set(names)) != len(names):
        raise ShardError("the listing names a test more than once")
    return names


def select_shard(names: list[str], shard: int, shards: int) -> list[str]:
    """Return every ``shards``-th name starting at 1-based ``shard``.

    For ``shard`` 1 to ``shards`` the selections are disjoint and together hold
    every name, as ``--tests-information shard,,shards`` does for one numbering.
    """
    if shards < 1:
        raise ShardError(f"the shard count must be positive, not {shards}")
    if shard < 1 or shard > shards:
        raise ShardError(f"shard {shard} is outside 1 to {shards}")
    return names[shard - 1 :: shards]


def compute_digest(names: list[str]) -> str:
    """Return the SHA-256 of the newline-terminated names, to compare shards' listings."""
    return hashlib.sha256("".join(f"{name}\n" for name in names).encode("utf-8")).hexdigest()


def list_tests(ctest: str, build: Path, exclude_regex: str | None) -> dict:
    command = [ctest, "--test-dir", str(build), "--show-only=json-v1"]
    if exclude_regex is not None:
        command += ["--exclude-regex", exclude_regex]
    try:
        listing = subprocess.run(command, check=True, capture_output=True, text=True)
        return json.loads(listing.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        raise ShardError(f"could not list the tests in {build}: {error}") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build", type=Path, help="configured build tree to list")
    parser.add_argument("--shard", type=int, required=True, help="1-based shard number")
    parser.add_argument("--shards", type=int, required=True, help="number of shards")
    parser.add_argument("--output", type=Path, required=True, help="file for ctest --tests-from-file")
    parser.add_argument("--ctest", default="ctest", help="ctest executable that lists the tests")
    parser.add_argument("--tests-json", type=Path, help="saved ctest --show-only=json-v1 output instead of --ctest")
    parser.add_argument("--exclude-regex", help="tests to leave out of every shard (ctest --exclude-regex)")
    parser.add_argument("--summary", type=Path, help="Markdown summary file (default: $GITHUB_STEP_SUMMARY)")
    arguments = parser.parse_args(argv)

    try:
        if arguments.tests_json is not None:
            if arguments.exclude_regex is not None:
                raise ShardError("--exclude-regex needs a live ctest listing, not --tests-json")
            try:
                listing = json.loads(arguments.tests_json.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise ShardError(f"could not read {arguments.tests_json}: {error}") from error
        else:
            listing = list_tests(arguments.ctest, arguments.build, arguments.exclude_regex)
        names = read_names(listing)
        if names == []:
            raise ShardError(f"{arguments.build} lists no tests")
        selected = select_shard(names, arguments.shard, arguments.shards)
        if selected == []:
            raise ShardError(
                f"shard {arguments.shard} of {arguments.shards} has no tests; "
                f"the listing holds only {len(names)}"
            )
        try:
            arguments.output.parent.mkdir(parents=True, exist_ok=True)
            arguments.output.write_text("".join(f"{name}\n" for name in selected), encoding="utf-8")
        except OSError as error:
            raise ShardError(f"could not write {arguments.output}: {error}") from error
    except ShardError as error:
        print(f"::error title={ANNOTATION_TITLE}::{error}")
        return 1

    summary = (
        f"CTest shard {arguments.shard}/{arguments.shards}: {len(selected)} of {len(names)} tests; "
        f"listing SHA-256 `{compute_digest(names)}`\n"
    )
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
