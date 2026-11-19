"""Lines a change authored, shared by the changed-line gates.

source_policy.py and changed_tidy.py apply rules only to the lines a change adds
or modifies. A line diff between two trees cannot tell those lines apart from
unchanged lines that moved: how it aligns a moved block depends on everything
else that changed in the file. A pull request and a batch that combines it with
other pull requests can therefore disagree about the same, untouched line.

Instead, ``git blame -M`` attributes every line of the inspected file to the
commit that wrote it, following lines moved within the file to their origin. A
line counts as authored when that commit is newer than the base. Each line keeps
one author however the commits are grouped, so a pull request and the batch that
contains it select the same lines.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

# Thanks to the Git project, "git-blame(1)", The Porcelain Format: each line starts
# with a header naming its commit and its final line number, and a commit at the
# range's lower bound is marked "boundary".
# https://git-scm.com/docs/git-blame#_the_porcelain_format
LINE_HEADER = re.compile(r"^([0-9a-f]{40}) \d+ (\d+)(?: \d+)?$")


def parse_authored_lines(porcelain: str) -> set[int]:
    """Return the final line numbers whose commit is not a range boundary."""
    commits: dict[int, str] = {}
    boundaries: set[str] = set()
    commit = ""
    for line in porcelain.splitlines():
        header = LINE_HEADER.match(line)
        if header is not None:
            commit = header.group(1)
            commits[int(header.group(2))] = commit
        elif line == "boundary" and commit != "":
            boundaries.add(commit)
    return {number for number, author in commits.items() if author not in boundaries}


def is_tracked(root: Path, revision: str, name: str) -> bool:
    return (
        subprocess.run(
            ["git", "cat-file", "-e", f"{revision}:{name}"], cwd=root, capture_output=True
        ).returncode
        == 0
    )


def authored_lines(root: Path, base: str, name: str, head: str | None = None) -> set[int]:
    """Return the 1-based lines of ``name`` authored after ``base``.

    With ``head``, inspect that commit's file; otherwise inspect the work tree,
    where uncommitted lines count as authored. ``base`` must be an ancestor of the
    inspected revision (callers pass a merge base). A file absent from that
    revision is new, so all of its lines are authored.
    """
    revision = head if head else "HEAD"
    if not is_tracked(root, revision, name):
        path = root / name
        text = (
            subprocess.run(
                ["git", "show", f"{revision}:{name}"], cwd=root, text=True, capture_output=True
            ).stdout
            if head
            else path.read_text(encoding="utf-8")
        )
        return set(range(1, len(text.splitlines()) + 1))
    command = ["git", "blame", "-M", "--porcelain"]
    if head is None:
        command += ["--contents", str(root / name)]
    command += [f"{base}..{revision}", "--", name]
    porcelain = subprocess.run(
        command, cwd=root, text=True, capture_output=True, check=True, errors="replace"
    ).stdout
    return parse_authored_lines(porcelain)


def to_ranges(lines: set[int]) -> list[list[int]]:
    """Group line numbers into sorted inclusive [first, last] ranges."""
    ranges: list[list[int]] = []
    for number in sorted(lines):
        if ranges and ranges[-1][1] + 1 == number:
            ranges[-1][1] = number
        else:
            ranges.append([number, number])
    return ranges
