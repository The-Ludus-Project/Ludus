"""Skip ./init.sh work whose inputs have not changed since it last succeeded.

A warm ``./init.sh`` rerun used to repeat every step: pip reinstalled the
pinned CMake, Ninja and Conan, Conan re-exported recipes, re-resolved the lock
and reinstalled every native preset, and CMake reconfigured the editor even
when nothing it reads had changed. Each skippable step now records a
fingerprint of its inputs in ``out/init/stamps/<name>.json`` after it succeeds
and is skipped while the fingerprint still matches.

A stamp is only a hint to skip work that would produce the same result; it is
never trusted for integrity. Downloads are still verified against their pins,
and the build still reruns CMake whenever a CMake input changes. Set
``LUDUS_INIT_FORCE=1`` to ignore every stamp and redo all steps.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Iterable, Mapping

FORCE_ENVIRONMENT = "LUDUS_INIT_FORCE"
STAMP_DIRECTORY = Path("out") / "init" / "stamps"
# Bump to invalidate every stamp written by an older ./init.sh.
STAMP_FORMAT_VERSION = 1
READ_CHUNK_BYTES = 1024 * 1024
# Directories that never hold recipe inputs.
IGNORED_DIRECTORY_NAMES = frozenset({"__pycache__", ".git"})


def is_forced(environment: Mapping[str, str] | None = None) -> bool:
    """Whether ``LUDUS_INIT_FORCE`` asks to ignore every stamp."""
    environment = os.environ if environment is None else environment
    value = environment.get(FORCE_ENVIRONMENT, "").strip().lower()
    return value not in ("", "0", "false", "no", "off")


def file_digest(path: Path) -> str:
    """SHA-256 of a file, or ``missing`` when it is not a regular file."""
    if not path.is_file():
        return "missing"
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(READ_CHUNK_BYTES), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_digest(directory: Path) -> str:
    """SHA-256 over the relative names and contents of every file below ``directory``."""
    digest = hashlib.sha256()
    if not directory.is_dir():
        return "missing"
    for path in sorted(directory.rglob("*")):
        if not path.is_file() or IGNORED_DIRECTORY_NAMES.intersection(path.relative_to(directory).parts):
            continue
        digest.update(path.relative_to(directory).as_posix().encode("utf-8") + b"\0")
        digest.update(file_digest(path).encode("ascii") + b"\0")
    return digest.hexdigest()


def text_digest(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def fingerprint(inputs: Mapping[str, object]) -> str:
    """A stable digest of JSON-serializable ``inputs``."""
    canonical = json.dumps({"format": STAMP_FORMAT_VERSION, "inputs": inputs}, sort_keys=True, default=str)
    return text_digest(canonical)


def stamp_path(root: Path, name: str) -> Path:
    return root / STAMP_DIRECTORY / f"{name}.json"


def is_current(path: Path, inputs: Mapping[str, object], *, environment: Mapping[str, str] | None = None) -> bool:
    """Whether the stamp at ``path`` records exactly ``inputs`` and nothing forces a rerun."""
    if is_forced(environment):
        return False
    try:
        recorded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return isinstance(recorded, dict) and recorded.get("fingerprint") == fingerprint(inputs)


def record(path: Path, inputs: Mapping[str, object]) -> None:
    """Write the stamp after the step succeeded; a failed step must never call this."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps({"fingerprint": fingerprint(inputs), "inputs": inputs}, indent=2,
                                    sort_keys=True, default=str) + "\n", encoding="utf-8")
    temporary.replace(path)


def forget(paths: Iterable[Path]) -> None:
    """Remove stamps, for example before a step that may leave its outputs half written."""
    for path in paths:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
