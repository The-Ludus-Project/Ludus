"""Verify the native Windows terminal assertion path in an isolated process."""

from pathlib import Path
import subprocess
import sys


def verify(probe: Path) -> None:
    result = subprocess.run([probe], capture_output=True, check=False, timeout=10)
    expected = (
        b"[LUDUS assertion report v1]",
        b"[LUDUS:REQUIRE]",
        b"windows_diagnostic_probe.cpp:",
        b"Windows terminal diagnostic 42",
    )
    if result.returncode == 0 or any(marker not in result.stderr for marker in expected):
        raise RuntimeError(f"Missing native terminal report ({result.returncode})\n{result.stderr!r}")
    if b"unexpected normal exit" in result.stderr:
        raise RuntimeError(f"The assertion ran normal exit handlers\n{result.stderr!r}")


if __name__ == "__main__":
    verify(Path(sys.argv[1]).resolve())
