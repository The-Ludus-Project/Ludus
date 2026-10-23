"""Explain a failed ./init.sh run: which step stopped, what the output says and what to try.

The setup window and the terminal runner share this module, so both show the
same failing step, the same plain-language hint and the same log location.
Hints come from ``FAILURE_HINTS``, an ordered table of output patterns: the
first entry whose pattern matches any recent output line wins, so specific
causes (a full disk, a stale virtual environment) are listed before generic
ones (a failed package manager). The module has no Tk dependency so it can be
tested without a desktop.
"""
from __future__ import annotations

import os
import platform
import re
import subprocess
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence


# Lines of output shown in the failure view and copied into an error report.
FAILURE_TAIL_LINES = 18
# Recent lines searched for a recognised cause; a failing tool's own message
# is usually a few dozen lines above the final "command failed" line.
HINT_SCAN_LINES = 400
LOG_RELATIVE_PATH = Path("out/init/setup.log")
HINT_PREFIX = "Hint: "
LOG_PREFIX = "Full log: "
LAST_LINES_HEADING = "Last lines of output:"


@dataclass(frozen=True)
class FailureHint:
    """One recognised failure: a pattern over output lines and the advice to show."""

    key: str
    pattern: re.Pattern[str]
    advice: str


def _pattern(*alternatives: str) -> re.Pattern[str]:
    return re.compile("|".join(f"(?:{alternative})" for alternative in alternatives), re.IGNORECASE)


FAILURE_HINTS: tuple[FailureHint, ...] = (
    FailureHint(
        "disk_full",
        _pattern(r"No space left on device", r"\bENOSPC\b", r"Disk quota exceeded", r"not enough (?:free )?disk space"),
        "The disk is full. Free some space (build trees under out/ and the Conan cache in ~/.conan2 are the "
        "largest Ludus files), then retry.",
    ),
    FailureHint(
        "stale_venv",
        _pattern(r"No module named '?encodings'?", r"init_fs_encoding", r"failed to get the Python codec",
                 r"out/host-tools/venv/bin/python\S*: (?:bad interpreter|No such file)"),
        "The Ludus Python environment (out/host-tools/venv) was created by a Python that has since been "
        "upgraded or removed. Delete it with `rm -rf out/host-tools/venv`, then retry; setup recreates it.",
    ),
    FailureHint(
        "xcode_command_line_tools",
        _pattern(r"xcrun: error: invalid active developer path", r"no developer tools were found",
                 r"xcode-select: (?:error|note)", r"requires (?:Homebrew and )?Xcode Command Line Tools",
                 r"CommandLineTools.*(?:not found|missing)"),
        "The Xcode Command Line Tools are missing or broken. Run `xcode-select --install` (or reinstall them "
        "after a macOS upgrade), then retry.",
    ),
    FailureHint(
        "missing_compiler",
        _pattern(r"No CMAKE_(?:C|CXX)_COMPILER could be found", r"is not a full path to an existing compiler",
                 r"(?:clang|clang\+\+|ld\.lld)(?:-\d+)?: (?:command )?not found",
                 r"command not found while running: \S*(?:clang|lld)"),
        "The pinned Clang/LLVM compiler was not found. Run ./init.sh without --no-system-install so setup "
        "installs it, or install the LLVM version in config/tool_versions.json, then retry.",
    ),
    FailureHint(
        "sudo_password",
        _pattern(r"sudo: a password is required", r"sudo: (?:a terminal is required|no tty present)",
                 r"incorrect password attempts?"),
        "Installing system packages needs your sudo password. Run ./init.sh from a terminal, type the password "
        "there when asked, then retry.",
    ),
    FailureHint(
        "apt_lock",
        _pattern(r"Could not get lock /var/lib/(?:dpkg|apt)", r"Unable to acquire the dpkg frontend lock",
                 r"dpkg was interrupted"),
        "Another package manager is running, or an earlier one was interrupted. Wait for software updates to "
        "finish (or run `sudo dpkg --configure -a` if dpkg was interrupted), then retry.",
    ),
    FailureHint(
        "conan_remote",
        _pattern(r"Unable to find '[^']+' in remotes", r"Package '[^']+' not resolved", r"Remote '[^']+' not found"),
        "Conan could not find a required package in its remotes. Check `conan remote list` (ConanCenter must be "
        "enabled), your network connection or proxy, then retry.",
    ),
    FailureHint(
        "network",
        _pattern(r"Temporary failure in name resolution", r"Could not resolve host", r"Name or service not known",
                 r"nodename nor servname provided", r"Network is unreachable", r"Connection (?:timed out|refused)",
                 r"Max retries exceeded", r"HTTPSConnectionPool", r"certificate verify failed",
                 r"Failed to establish a new connection", r"urlopen error"),
        "A download failed because the network could not be reached. Check your internet connection, VPN or "
        "proxy settings (HTTPS_PROXY), then retry; finished steps are reused.",
    ),
    FailureHint(
        "apt",
        _pattern(r"^E: ", r"Unable to locate package", r"Unmet dependencies", r"exit code \d+: (?:sudo )?\S*apt-get\b"),
        "The Ubuntu package manager (apt) failed. Run `sudo apt-get update` in a terminal, fix what it reports, "
        "then retry.",
    ),
    FailureHint(
        "homebrew",
        _pattern(r"exit code \d+: \S*\bbrew\b", r"Error: (?:No such keg|Cannot install|The following directories are not writable)",
                 r"Homebrew.*(?:failed|error)"),
        "Homebrew failed. Run `brew update` and `brew doctor` in a terminal, fix what they report, then retry.",
    ),
)


_recent_output: deque[str] = deque(maxlen=HINT_SCAN_LINES)


def remember_output(text: str) -> None:
    """Keep a failed command's captured output so a terminal run can still match hints against it."""
    _recent_output.extend(text.splitlines())


def recent_output() -> list[str]:
    return list(_recent_output)


def find_hint(lines: Iterable[str], hints: Sequence[FailureHint] = FAILURE_HINTS) -> FailureHint | None:
    """Return the first hint in table order whose pattern matches one of the recent ``lines``."""
    recent = list(deque(lines, maxlen=HINT_SCAN_LINES))
    for hint in hints:
        if any(hint.pattern.search(line) for line in recent):
            return hint
    return None


def tail(lines: Iterable[str], count: int = FAILURE_TAIL_LINES) -> list[str]:
    """The last ``count`` lines, with trailing blank lines dropped."""
    kept = list(lines)
    while kept and not kept[-1].strip():
        kept.pop()
    return kept[-count:] if count > 0 else []


def read_log_lines(path: Path) -> list[str]:
    """Return the log's lines, or nothing when it cannot be read."""
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def open_command(path: Path, system: str | None = None) -> list[str] | None:
    """The command that opens ``path`` in the desktop's default viewer; None on Windows (use ``os.startfile``)."""
    system = system or platform.system()
    if system == "Darwin":
        return ["open", str(path)]
    if system == "Windows":
        return None
    return ["xdg-open", str(path)]


def open_in_viewer(path: Path, *, system: str | None = None, popen: Callable = subprocess.Popen,
                   start_file: Callable | None = None) -> bool:
    """Open ``path`` without waiting for the viewer; return False when that is not possible."""
    if not path.is_file():
        return False
    command = open_command(path, system)
    try:
        if command is None:
            (start_file or getattr(os, "startfile"))(str(path))
        else:
            popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                  start_new_session=True)
    except (OSError, AttributeError):
        return False
    return True


def report_lines(*, step_label: str | None, error: str, hint: FailureHint | None, log_path: Path | None,
                 last_lines: Sequence[str] = ()) -> list[str]:
    """A plain-text failure report: failing step, error, hint, log location and the output's tail."""
    lines = [f"Setup failed during: {step_label}" if step_label else "Setup failed."]
    if error:
        lines.append(f"Error: {error}")
    if hint is not None:
        lines.append(HINT_PREFIX + hint.advice)
    if log_path is not None:
        lines.append(LOG_PREFIX + str(log_path))
    if last_lines:
        lines.append("")
        lines.append(LAST_LINES_HEADING)
        lines.extend("  " + line for line in last_lines)
    return lines
