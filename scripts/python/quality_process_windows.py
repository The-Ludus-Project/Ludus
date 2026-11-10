"""Windows Job containment for bounded quality subprocesses."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

from quality_process import ProcessResult

# Thanks to Microsoft, "Job Objects", for inherited membership and closing a
# kill-on-close job to contain all descendants, including pipe-holding children.
# https://learn.microsoft.com/windows/win32/procthread/job-objects
JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x2000
PROCESS_TERMINATE = 0x0001
PROCESS_SET_QUOTA = 0x0100


class BasicLimits(ctypes.Structure):
    _fields_ = [
        ("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
        ("flags", wintypes.DWORD), ("minimum_working_set", ctypes.c_size_t),
        ("maximum_working_set", ctypes.c_size_t), ("active_processes", wintypes.DWORD),
        ("affinity", ctypes.c_size_t), ("priority", wintypes.DWORD),
        ("scheduling", wintypes.DWORD),
    ]


class IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in (
        "read_operations", "write_operations", "other_operations",
        "read_bytes", "write_bytes", "other_bytes",
    )]


class ExtendedLimits(ctypes.Structure):
    _fields_ = [
        ("basic", BasicLimits), ("io", IoCounters),
        ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
        ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t),
    ]


class ProcessJob:
    def __init__(self):
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        signatures = (
            ("CreateJobObjectW", [ctypes.c_void_p, wintypes.LPCWSTR], wintypes.HANDLE),
            ("SetInformationJobObject", [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD], wintypes.BOOL),
            ("OpenProcess", [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
            ("AssignProcessToJobObject", [wintypes.HANDLE, wintypes.HANDLE], wintypes.BOOL),
            ("CloseHandle", [wintypes.HANDLE], wintypes.BOOL),
        )
        for name, arguments, result in signatures:
            function = getattr(self.kernel, name)
            function.argtypes = arguments
            function.restype = result
        self.handle = self.kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimits()
        limits.basic.flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.kernel.SetInformationJobObject(self.handle, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
                                                   ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.get_last_error()
            self.close()
            raise ctypes.WinError(error)

    def assign(self, process):
        handle = self.kernel.OpenProcess(PROCESS_TERMINATE | PROCESS_SET_QUOTA, False, process.pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            if not self.kernel.AssignProcessToJobObject(self.handle, handle):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            self.kernel.CloseHandle(handle)

    def close(self):
        if self.handle:
            handle, self.handle = self.handle, None
            if not self.kernel.CloseHandle(handle):
                raise ctypes.WinError(ctypes.get_last_error())


def run_bounded_windows(argv, *, cwd, log, env, timeout, limit, budget):
    log.parent.mkdir(parents=True, exist_ok=True)
    job = ProcessJob()
    process = None
    reader = None
    completed = threading.Event()
    errors = []
    written = 0
    is_truncated = False
    has_timed_out = False
    deadline = time.monotonic() + timeout
    try:
        with log.open("xb") as output:
            # The wrapper waits for admission before launching the requested
            # child. This closes the spawn-before-Job-assignment race.
            process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), *map(str, argv)],
                                       cwd=cwd, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            try:
                job.assign(process)
                process.stdin.write(b"1")
                process.stdin.flush()
            finally:
                process.stdin.close()

            def read_output():
                nonlocal written, is_truncated
                try:
                    while True:
                        chunk = os.read(process.stdout.fileno(), 64 * 1024)
                        if not chunk:
                            return
                        allowed = min(len(chunk), limit - written)
                        if budget is not None:
                            allowed = budget.take(allowed)
                        output.write(chunk[:allowed])
                        written += allowed
                        if allowed < len(chunk):
                            is_truncated = True
                            return
                except OSError as error:
                    errors.append(error)
                finally:
                    completed.set()

            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            try:
                if not completed.wait(max(0, deadline - time.monotonic())):
                    has_timed_out = True
                if not is_truncated and not has_timed_out and not errors:
                    try:
                        process.wait(timeout=max(0, deadline - time.monotonic()))
                    except subprocess.TimeoutExpired:
                        has_timed_out = True
            finally:
                job.close()
                process.wait(timeout=5)
                reader.join(timeout=5)
                if reader.is_alive():
                    raise OSError("Windows quality output reader did not stop after Job cleanup")
                process.stdout.close()
            if errors:
                raise errors[0]
    finally:
        job.close()
        if process is not None and process.poll() is None:
            process.kill()
            process.wait(timeout=5)
        if process is not None and reader is None:
            process.stdout.close()
    return ProcessResult(process.returncode, log, written, is_truncated, has_timed_out)


if __name__ == "__main__":
    # Native children inherit the wrapper's job and output handles.
    if sys.stdin.buffer.read(1) != b"1":
        sys.exit(1)
    sys.exit(subprocess.call(sys.argv[1:]))
