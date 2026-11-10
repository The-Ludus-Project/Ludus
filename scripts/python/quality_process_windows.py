"""Bounded output collection using the shared Windows process owner."""

from __future__ import annotations

import os
import selectors
import time

from editor_process_windows import OwnedWindowsProcess, PipeSelector, MAX_IO_SLICE
from quality_process import ProcessResult


def run_bounded_windows(argv, *, cwd, log, env, timeout, limit, budget):
    log.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    is_truncated = False
    has_timed_out = False
    deadline = time.monotonic() + timeout
    with log.open("xb") as output:
        process = OwnedWindowsProcess(list(map(str, argv)), str(cwd), dict(os.environ if env is None else env))
        try:
            with PipeSelector() as selector:
                readers = {
                    process.stdout_fd: process.read_stdout,
                    process.stderr_fd: process.read_stderr,
                }
                for descriptor in readers:
                    selector.register(descriptor, selectors.EVENT_READ)
                while selector.get_map():
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        has_timed_out = True
                        break
                    for key, _ in selector.select(remaining):
                        try:
                            chunk = readers[key.fd](64 * 1024)
                        except BlockingIOError:
                            continue
                        if not chunk:
                            selector.unregister(key.fd)
                            continue
                        allowed = min(len(chunk), limit - written)
                        if budget is not None:
                            allowed = budget.take(allowed)
                        output.write(chunk[:allowed])
                        written += allowed
                        if allowed < len(chunk):
                            is_truncated = True
                            break
                    if is_truncated:
                        break
                if not is_truncated and not has_timed_out:
                    while process.wait_exit_nowait() is None:
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            has_timed_out = True
                            break
                        time.sleep(min(remaining, MAX_IO_SLICE))
        finally:
            report = process.cleanup(cancelled=has_timed_out or is_truncated)
            if not report.confirmed or report.exit_code is None:
                raise OSError("Windows quality process Job cleanup was not confirmed")
    return ProcessResult(report.exit_code, log, written, is_truncated, has_timed_out)
