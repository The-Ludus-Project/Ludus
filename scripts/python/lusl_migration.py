"""The LUSL migration gate: migrated directories admit no Slang source outside the retained list.

`config/lusl_migration.json` lists the directories whose shaders are LUSL and the Slang files the
repository keeps, each with its reason (docs/architecture/shading-language.md#l5d-migration-waves).
Every Slang source under a migrated directory must be retained; any other is a failure, so a new
`.slang`/`.slangh` file there is rejected however it arrived (added, renamed or copied). The check
reads the whole tree, not only the changed files, and also validates the list itself.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path, PurePosixPath

CONFIGURATION = "config/lusl_migration.json"
SLANG_SUFFIXES = (".slang", ".slangh")
# A reason must say why the file stays Slang; a word or two does not.
MINIMUM_REASON_CHARACTERS = 20
GUIDANCE = (
    "write it in LUSL (.lusl/.luslh, docs/modules/tooling/shader-language.md), or retain it in "
    f"{CONFIGURATION} with the reason it stays Slang"
)


def is_slang(name: str) -> bool:
    return name.endswith(SLANG_SUFFIXES)


def is_under(name: str, directory: str) -> bool:
    return PurePosixPath(name).is_relative_to(PurePosixPath(directory))


def tree_files(root: Path, head: str | None) -> list[str]:
    """Files of the PR head tree, or of the work tree including untracked, undeleted files."""
    if head:
        command = ["git", "ls-tree", "-r", "--name-only", head]
    else:
        command = ["git", "ls-files", "--cached", "--others", "--exclude-standard"]
    names = subprocess.run(
        command, cwd=root, text=True, capture_output=True, check=True
    ).stdout.splitlines()
    if head:
        return names
    return [name for name in names if (root / name).is_file()]


def read_configuration(root: Path, head: str | None) -> str | None:
    if head:
        shown = subprocess.run(
            ["git", "show", f"{head}:{CONFIGURATION}"], cwd=root, text=True, capture_output=True
        )
        return shown.stdout if shown.returncode == 0 else None
    path = root / CONFIGURATION
    return path.read_text(encoding="utf-8") if path.is_file() else None


def configuration_findings(configuration: object, files: set[str]) -> list[str]:
    """Validate the list's shape: each entry names an existing path, and retained ones a reason."""
    if not isinstance(configuration, dict):
        return [f"{CONFIGURATION}: expected an object with 'migrated' and 'retained' lists"]
    failures = []
    directories = {str(parent) for name in files for parent in PurePosixPath(name).parents}
    for key in ("migrated", "retained"):
        entries = configuration.get(key)
        if not isinstance(entries, list):
            failures.append(f"{CONFIGURATION}: '{key}' must be a list")
            continue
        seen = set()
        for index, entry in enumerate(entries):
            where = f"{CONFIGURATION}: {key}[{index}]"
            path = entry.get("path") if isinstance(entry, dict) else None
            if not isinstance(path, str) or path == "" or path != str(PurePosixPath(path)):
                failures.append(f"{where}: 'path' must be a normalized checkout-relative path")
                continue
            if path in seen:
                failures.append(f"{where}: '{path}' is listed twice")
            seen.add(path)
            if key == "migrated" and path not in directories:
                failures.append(f"{where}: '{path}' is not a directory of the tree")
            if key == "retained":
                if path not in files and path not in directories:
                    failures.append(f"{where}: '{path}' does not exist; remove the stale entry")
                elif path in files and not is_slang(path):
                    failures.append(f"{where}: '{path}' is not a Slang source")
                reason = entry.get("reason")
                if not isinstance(reason, str) or len(reason.strip()) < MINIMUM_REASON_CHARACTERS:
                    failures.append(f"{where}: '{path}' needs a reason it stays Slang")
    return failures


def check_migration(root: Path, head: str | None = None) -> list[str]:
    """Report Slang sources under migrated directories that the retained list does not name."""
    text = read_configuration(root, head)
    if text is None:
        return []
    try:
        configuration = json.loads(text)
    except json.JSONDecodeError as error:
        return [f"{CONFIGURATION}: invalid JSON: {error}"]
    files = set(tree_files(root, head))
    failures = configuration_findings(configuration, files)
    if failures:
        return failures
    migrated = [entry["path"] for entry in configuration["migrated"]]
    retained = [entry["path"] for entry in configuration["retained"]]
    for name in sorted(files):
        if not is_slang(name):
            continue
        scope = next((directory for directory in migrated if is_under(name, directory)), None)
        if scope is None or any(is_under(name, path) for path in retained):
            continue
        failures.append(f"{name}: Slang source in the LUSL-migrated directory {scope}; {GUIDANCE}")
    return failures
