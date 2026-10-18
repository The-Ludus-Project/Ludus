"""Changed-line clang-tidy: naming, magic-number and implicit-bool rules.

The legacy code base predates these AGENTS.md rules, so they run only on the
lines a change adds or modifies (clang-tidy --line-filter). Translation units
are the changed first-party sources found in the configured compile database;
changed headers are checked through every changed source that includes them.
A header changed without any changed includer is not analyzed here; the lexical
source_policy.py gate still covers it.
"""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

from source_policy import is_test_source

CPP_SUFFIXES = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".mm"}
TRANSLATION_UNIT_SUFFIXES = {".cpp", ".cc", ".cxx", ".mm"}
CHANGED_ROOTS = ("modules", "apps", "examples", "tests", "tools")
HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", re.MULTILINE)


def parse_changed_lines(diff: str) -> dict[str, list[list[int]]]:
    """Map each file in a zero-context unified diff to its added line ranges."""
    changed: dict[str, list[list[int]]] = {}
    for section in re.split(r"^diff --git ", diff, flags=re.MULTILINE)[1:]:
        target = re.search(r"^\+\+\+ b/(.+)$", section, re.MULTILINE)
        if target is None:
            continue
        ranges = []
        for hunk in HUNK.finditer(section):
            start = int(hunk.group(1))
            count = 1 if hunk.group(2) is None else int(hunk.group(2))
            if count > 0:
                ranges.append([start, start + count - 1])
        if ranges:
            changed[target.group(1)] = ranges
    return changed


def changed_lines(root: Path, base: str) -> dict[str, list[list[int]]]:
    merge_base = subprocess.run(
        ["git", "merge-base", base, "HEAD"], cwd=root, text=True, capture_output=True, check=True
    ).stdout.strip()
    diff = subprocess.run(
        ["git", "diff", "--unified=0", "--no-color", "--diff-filter=ACMR", merge_base, "--",
         *CHANGED_ROOTS],
        cwd=root, text=True, capture_output=True, check=True,
    ).stdout
    return {
        name: ranges for name, ranges in parse_changed_lines(diff).items()
        if Path(name).suffix in CPP_SUFFIXES
    }


def line_filter(root: Path, changed: dict[str, list[list[int]]]) -> str:
    # clang-tidy matches line-filter names against the diagnostic's file path.
    entries = []
    for name, ranges in sorted(changed.items()):
        entries.append({"name": str((root / name).resolve()), "lines": ranges})
    return json.dumps(entries)


def plan(root: Path, entries: list[dict], changed: dict[str, list[list[int]]]) -> list[tuple[dict, Path]]:
    """Pick (compile entry, policy file) pairs for changed translation units."""
    sources = {
        (root / name).resolve() for name in changed if Path(name).suffix in TRANSLATION_UNIT_SUFFIXES
    }
    selected = []
    seen = set()
    for entry in entries:
        source = Path(entry["file"]).resolve()
        if source not in sources or source in seen:
            continue
        seen.add(source)
        relative = source.relative_to(root.resolve()).as_posix()
        policy = "config/clang-tidy-changed-tests.yml" if is_test_source(relative) else "config/clang-tidy-changed.yml"
        selected.append((entry, root / policy))
    return selected
