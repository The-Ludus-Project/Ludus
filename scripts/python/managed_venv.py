"""Validate, create and repair the Python virtual environments that Ludus manages.

A managed environment (for example ``out/host-tools/venv``) is only reusable
while the interpreter that built it still exists. After an operating-system or
Homebrew upgrade replaces Python 3.12 with 3.14, the environment's copied
``bin/python`` still looks for ``/usr/lib/python3.12`` and fails before running
any code (``No module named 'encodings'``). Recreating such an environment in
place is not enough: ``venv`` keeps an existing ``bin/python`` and only replaces
``bin/python3``, so the stale interpreter survives. ``ensure`` therefore checks
the environment against the running interpreter and rebuilds it from scratch
when it no longer matches.

Only directories this module is pointed at are removed, and only after they
are confirmed to be a real directory rather than a link to somewhere else.
"""
from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import subprocess
import sys
import venv

CONFIGURATION_FILE_NAME = "pyvenv.cfg"
# virtualenv and uv write ``version_info``; the standard library writes ``version``.
VERSION_KEYS = ("version_info", "version")
HOME_KEY = "home"
# Version components compared between an environment and the running interpreter.
COMPARED_VERSION_COMPONENTS = 2
HEALTH_PROBE_TIMEOUT_SECONDS = 60
# ``encodings`` is the first standard-library module an interpreter imports, so
# a missing or mismatched standard library fails here; pip must also be present
# because every caller installs pinned packages next.
HEALTH_PROBE_SCRIPT = (
    "import encodings, importlib.util, sys; "
    "print('%d.%d' % sys.version_info[:2]); "
    "print('pip' if importlib.util.find_spec('pip') else 'no-pip')"
)
PIP_PRESENT_MARKER = "pip"
MAXIMUM_REASON_CHARACTERS = 200


class ManagedEnvironmentError(RuntimeError):
    """A managed virtual environment could not be made usable."""


@dataclass(frozen=True)
class EnvironmentHealth:
    """What ``inspect`` found; ``reason`` is empty exactly when the environment is usable."""

    is_present: bool
    is_usable: bool
    reason: str = ""
    recorded_version: str = ""


Runner = Callable[..., subprocess.CompletedProcess]


def current_version() -> str:
    return version_text(sys.version_info[:COMPARED_VERSION_COMPONENTS])


def version_text(components) -> str:
    return ".".join(str(component) for component in components)


def major_minor(version: str) -> str:
    """Return ``"3.12"`` for ``"3.12.15"`` or ``"3.12.15.final.0"``; empty when unreadable."""
    components = version.strip().split(".")[:COMPARED_VERSION_COMPONENTS]
    if len(components) < COMPARED_VERSION_COMPONENTS or not all(component.isdigit() for component in components):
        return ""
    return ".".join(components)


def python_path(directory: Path, *, is_windows: bool | None = None) -> Path:
    is_windows = os.name == "nt" if is_windows is None else is_windows
    if is_windows:
        return directory / "Scripts" / "python.exe"
    return directory / "bin" / "python"


def read_configuration(directory: Path) -> dict[str, str]:
    """Parse ``pyvenv.cfg`` (``key = value`` lines); empty when absent or unreadable."""
    try:
        text = (directory / CONFIGURATION_FILE_NAME).read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return {}
    values = {}
    for line in text.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip().lower()] = value.strip()
    return values


def recorded_version(configuration: dict[str, str]) -> str:
    for key in VERSION_KEYS:
        version = major_minor(configuration.get(key, ""))
        if version:
            return version
    return ""


def summarize_error(text: str) -> str:
    """Pick the line that names the failure, such as ``ModuleNotFoundError: ...``."""
    lines = [line.strip() for line in (text or "").splitlines() if line.strip()]
    for line in lines:
        if "Error:" in line or "error:" in line:
            return line[:MAXIMUM_REASON_CHARACTERS]
    return lines[-1][:MAXIMUM_REASON_CHARACTERS] if lines else ""


def inspect(
    directory: Path,
    *,
    expected_version: str | None = None,
    runner: Runner = subprocess.run,
) -> EnvironmentHealth:
    """Check ``directory`` against the interpreter that will rebuild it."""
    expected_version = current_version() if expected_version is None else expected_version
    if not directory.exists() and not directory.is_symlink():
        return EnvironmentHealth(is_present=False, is_usable=False, reason="it does not exist")

    configuration = read_configuration(directory)
    if not configuration:
        return EnvironmentHealth(True, False, f"{CONFIGURATION_FILE_NAME} is missing or unreadable")

    version = recorded_version(configuration)
    if version and version != expected_version:
        return EnvironmentHealth(True, False, f"built with Python {version}, now {expected_version}", version)

    home = configuration.get(HOME_KEY, "")
    if home and not Path(home).is_dir():
        return EnvironmentHealth(True, False, f"its base Python directory no longer exists: {home}", version)

    interpreter = python_path(directory)
    if not interpreter.is_file():
        return EnvironmentHealth(True, False, f"its interpreter is missing: {interpreter}", version)

    try:
        completed = runner(
            [str(interpreter), "-c", HEALTH_PROBE_SCRIPT],
            cwd=str(directory),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=HEALTH_PROBE_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return EnvironmentHealth(True, False, f"its interpreter could not run: {exc}", version)

    if completed.returncode != 0:
        detail = summarize_error(completed.stderr) or f"exit code {completed.returncode}"
        return EnvironmentHealth(True, False, f"its interpreter is broken ({detail})", version)

    lines = (completed.stdout or "").split()
    probed_version = major_minor(lines[0]) if lines else ""
    if probed_version != expected_version:
        shown = probed_version or "an unknown version"
        return EnvironmentHealth(True, False, f"its interpreter runs Python {shown}, now {expected_version}",
                                 version or probed_version)

    has_pip = len(lines) > 1 and lines[1] == PIP_PRESENT_MARKER
    if not has_pip:
        return EnvironmentHealth(True, False, "pip is not installed", version or probed_version)
    return EnvironmentHealth(True, True, "", version or probed_version)


def is_running_inside(directory: Path, *, prefix: str | None = None) -> bool:
    """Whether the running interpreter belongs to ``directory``; deleting it would remove ourselves."""
    try:
        return Path(sys.prefix if prefix is None else prefix).resolve() == directory.resolve()
    except OSError:
        return False


def remove(directory: Path) -> None:
    """Delete exactly the managed environment directory; refuse links and plain files."""
    if directory.is_symlink() or not directory.is_dir():
        raise ManagedEnvironmentError(
            f"refusing to replace {directory}: it is not a plain directory; move it aside and rerun"
        )
    try:
        shutil.rmtree(directory)
    except OSError as exc:
        raise ManagedEnvironmentError(f"could not remove the stale virtual environment {directory}: {exc}") from exc


def ensure(
    directory: Path,
    *,
    announce: Callable[[str], None] = print,
    notify: Callable[[str], None] | None = None,
    builder: Callable[[Path], None] | None = None,
    expected_version: str | None = None,
    runner: Runner = subprocess.run,
) -> bool:
    """Make ``directory`` a working environment for this interpreter; return whether it was (re)built.

    ``notify`` receives the short recreation notice for a progress window.
    ``builder`` creates a fresh environment (default: ``venv`` with pip); it
    may raise, and the caller explains how to install venv support.
    """
    expected_version = current_version() if expected_version is None else expected_version
    health = inspect(directory, expected_version=expected_version, runner=runner)
    if health.is_usable:
        announce(f"Project-managed virtual environment is already usable: {directory}")
        return False

    if health.is_present and is_running_inside(directory):
        base = getattr(sys, "_base_executable", "") or "python3"
        raise ManagedEnvironmentError(
            f"the virtual environment at {directory} needs rebuilding ({health.reason}) but this command is "
            f"running from it; rerun with the system Python, for example: {base} scripts/python/engine.py init"
        )
    if health.is_present:
        notice = f"Recreating stale virtual environment ({health.reason})"
        announce(f"{notice}: {directory}")
        if notify is not None:
            notify(notice)
        remove(directory)
    else:
        announce(f"Creating project-managed virtual environment: {directory}")

    directory.parent.mkdir(parents=True, exist_ok=True)
    (builder or create)(directory)

    rebuilt = inspect(directory, expected_version=expected_version, runner=runner)
    if not rebuilt.is_usable:
        raise ManagedEnvironmentError(f"the new virtual environment at {directory} is unusable: {rebuilt.reason}")
    return True


def create(directory: Path) -> None:
    venv.EnvBuilder(with_pip=True, clear=True).create(directory)
