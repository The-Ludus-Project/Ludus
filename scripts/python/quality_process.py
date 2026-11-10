"""Bound diagnostic subprocess time, disk output and descendant lifetime."""
# Thanks to Steve Rabin, "The Science of Debugging Games", Game Programming
# Gems 4, section 1.1, pp. 5-18: the repository's chapter review informs preserving
# original failure evidence. This is design inspiration, not adapted chapter code.
# See docs/architecture/engineering-quality-gems-review.md#the-science-of-debugging-games.
from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import threading
import time
from typing import Sequence


LOG_BYTES = 1024 * 1024
LANE_BYTES = 100 * 1024 * 1024
ANALYSIS_SECONDS = 300


class LogBudget:
    """Share a byte ceiling across concurrent writers; never reserve twice."""

    def __init__(self, limit: int):
        if limit < 1:
            raise ValueError("Log budget must be positive")
        self.remaining = limit
        self.lock = threading.Lock()

    def take(self, requested: int) -> int:
        with self.lock:
            allowed = min(requested, self.remaining)
            self.remaining -= allowed
            return allowed


@dataclass(frozen=True)
class ProcessResult:
    returncode: int
    log: Path
    bytes_written: int
    truncated: bool
    timed_out: bool

    @property
    def complete(self) -> bool:
        return not self.truncated and not self.timed_out and self.log.is_file()


def run_bounded(argv: Sequence[object], *, cwd: Path, log: Path,
                env: dict[str, str] | None = None, timeout: float = ANALYSIS_SECONDS,
                limit: int = LOG_BYTES, budget: LogBudget | None = None) -> ProcessResult:
    """Keep a bounded original byte prefix. Overflow/timeout invalidate the result.

    POSIX children own a process group so timeout, overflow and interruption
    kill their descendants, including descendants holding the diagnostic pipe.
    Output is drained in fixed chunks rather than accumulated in Python memory.
    """
    if timeout <= 0 or limit < 1:
        raise ValueError("Process timeout and log limit must be positive")
    if os.name == "nt":
        from quality_process_windows import run_bounded_windows
        return run_bounded_windows(argv, cwd=cwd, log=log, env=env, timeout=timeout, limit=limit, budget=budget)
    if os.name != "posix":
        raise OSError("Bounded quality processes require Windows or POSIX")
    log.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    truncated = False
    timed_out = False

    with log.open("xb") as output:
        process = subprocess.Popen([str(arg) for arg in argv], cwd=cwd, env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        stopped = False

        def kill_group() -> None:
            nonlocal stopped
            if stopped:
                return
            process.poll()
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except PermissionError:
                if sys.platform != "darwin" or process.poll() is None:
                    raise
                # Darwin refuses signals to orphaned zombie symbolizers.
                # Ignore only a verified dead group; a live/unknown group
                # is still a cleanup failure. Do not weaken the log gate.
                states = subprocess.run(["/bin/ps", "-axo", "pgid=,stat="],
                                        check=True, capture_output=True, text=True, timeout=2).stdout
                if group_is_live(process.pid, states):
                    raise
            stopped = True

        deadline = time.monotonic() + timeout
        try:
            assert process.stdout is not None
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        timed_out = True
                        kill_group()
                        break
                    if not selector.select(remaining):
                        continue
                    chunk = os.read(process.stdout.fileno(), 64 * 1024)
                    if not chunk:
                        break
                    allowed = min(len(chunk), limit - written)
                    if budget is not None:
                        allowed = budget.take(allowed)
                    output.write(chunk[:allowed])
                    written += allowed
                    if allowed < len(chunk):
                        truncated = True
                        kill_group()
                        break
            if not stopped:
                try:
                    process.wait(timeout=max(0, deadline - time.monotonic()))
                except subprocess.TimeoutExpired:
                    timed_out = True
                    kill_group()
        finally:
            try:
                # Also reap descendants after a successful parent or reader failure.
                kill_group()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired as error:
                    raise OSError("Quality child did not exit after process-group cleanup") from error
            finally:
                process.stdout.close()
    return ProcessResult(process.returncode, log, written, truncated, timed_out)


def group_is_live(group: int, states: str) -> bool:
    """Read only process group IDs/statuses; unknown rows fail closed."""
    for line in states.splitlines():
        fields = line.split()
        if len(fields) != 2 or not fields[0].isdigit():
            raise OSError("Cannot establish process-group cleanup state")
        if int(fields[0]) == group and not fields[1].startswith("Z"):
            return True
    return False
