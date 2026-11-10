"""Win32 owners for the editor's child processes, stdio pipes and generation guards."""

import ctypes
import errno
import msvcrt
import os
import selectors
import subprocess
import time
import _winapi
from ctypes import wintypes as win


# Thanks to Microsoft, "Job Objects" and "Process Creation Flags", for nested
# kill-on-close jobs and assigning a suspended child before its first instruction.
# https://learn.microsoft.com/windows/win32/procthread/job-objects
# https://learn.microsoft.com/windows/win32/procthread/process-creation-flags
kernel = ctypes.WinDLL("kernel32", use_last_error=True)
SIZE_T = ctypes.c_size_t
DWORD = win.DWORD
HANDLE = win.HANDLE
BOOL = win.BOOL
BROKEN_PIPE = {109, 232, 233}
WAIT_TIMEOUT = 258
CREATE_SUSPENDED = 0x00000004
KILL_ON_JOB_CLOSE = 0x00002000
JOB_BASIC_ACCOUNTING = 1
JOB_EXTENDED_LIMIT = 9
FILE_SHARE_ALL = 7
DELETE = 0x00010000
OPEN_EXISTING = 3
OPEN_ALWAYS = 4
READ_WRITE = 0xc0000000
FILE_SHARE_READ_WRITE = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID_HANDLE = ctypes.c_void_p(-1).value
MAX_IO_SLICE = 0.01


def bind(name, result, *arguments):
    function = getattr(kernel, name)
    function.restype = result
    function.argtypes = arguments
    return function


class BasicLimit(ctypes.Structure):
    _fields_ = [
        ("per_process_user_time", ctypes.c_longlong), ("per_job_user_time", ctypes.c_longlong),
        ("flags", DWORD), ("minimum_working_set", SIZE_T), ("maximum_working_set", SIZE_T),
        ("active_process_limit", DWORD), ("affinity", SIZE_T), ("priority", DWORD),
        ("scheduling_class", DWORD),
    ]


class ExtendedLimit(ctypes.Structure):
    _fields_ = [
        ("basic", BasicLimit), ("io_counters", ctypes.c_ulonglong * 6),
        ("process_memory", SIZE_T), ("job_memory", SIZE_T),
        ("peak_process_memory", SIZE_T), ("peak_job_memory", SIZE_T),
    ]


class BasicAccounting(ctypes.Structure):
    _fields_ = [("times", ctypes.c_longlong * 4), ("page_faults", DWORD),
                ("total_processes", DWORD), ("active_processes", DWORD), ("terminated_processes", DWORD)]


class Overlapped(ctypes.Structure):
    _fields_ = [("internal", SIZE_T), ("internal_high", SIZE_T), ("offset", DWORD),
                ("offset_high", DWORD), ("event", HANDLE)]


create_job = bind("CreateJobObjectW", HANDLE, ctypes.c_void_p, win.LPCWSTR)
set_job = bind("SetInformationJobObject", BOOL, HANDLE, ctypes.c_int, ctypes.c_void_p, DWORD)
query_job = bind("QueryInformationJobObject", BOOL, HANDLE, ctypes.c_int, ctypes.c_void_p, DWORD, ctypes.c_void_p)
assign_job = bind("AssignProcessToJobObject", BOOL, HANDLE, HANDLE)
terminate_job = bind("TerminateJobObject", BOOL, HANDLE, win.UINT)
resume_thread = bind("ResumeThread", DWORD, HANDLE)
peek_pipe = bind("PeekNamedPipe", BOOL, HANDLE, ctypes.c_void_p, DWORD, ctypes.c_void_p,
                 ctypes.POINTER(DWORD), ctypes.c_void_p)
set_pipe = bind("SetNamedPipeHandleState", BOOL, HANDLE, ctypes.POINTER(DWORD), ctypes.c_void_p, ctypes.c_void_p)
write_file = bind("WriteFile", BOOL, HANDLE, ctypes.c_char_p, DWORD, ctypes.POINTER(DWORD), ctypes.c_void_p)
lock_range = bind("LockFileEx", BOOL, HANDLE, DWORD, DWORD, DWORD, DWORD, ctypes.POINTER(Overlapped))
open_file = bind("CreateFileW", HANDLE, win.LPCWSTR, DWORD, DWORD, ctypes.c_void_p, DWORD, DWORD, HANDLE)


def set_nonblocking(fd, *, write=False):
    msvcrt.setmode(fd, os.O_BINARY)
    if not write:
        return
    mode = DWORD(1)  # PIPE_NOWAIT in byte mode
    if not set_pipe(msvcrt.get_osfhandle(fd), ctypes.byref(mode), None, None):
        raise ctypes.WinError(ctypes.get_last_error())


def readable_bytes(fd):
    count = DWORD()
    if not peek_pipe(msvcrt.get_osfhandle(fd), None, 0, None, ctypes.byref(count), None):
        error = ctypes.get_last_error()
        if error in BROKEN_PIPE:
            return None
        raise ctypes.WinError(error)
    return count.value


def read_pipe(fd, size):
    available = readable_bytes(fd)
    if available is None:
        return b""
    if not available:
        raise BlockingIOError(errno.EAGAIN, "pipe is empty")
    return os.read(fd, min(size, available))


def write_pipe(fd, data):
    count = DWORD()
    if not write_file(msvcrt.get_osfhandle(fd), data, len(data), ctypes.byref(count), None):
        raise ctypes.WinError(ctypes.get_last_error())
    if not count.value:
        raise BlockingIOError(errno.EAGAIN, "pipe is full")
    return count.value


class PipeSelector(selectors.BaseSelector):
    """Poll the bounded set of anonymous Windows stdio pipes.

    Windows select only accepts sockets. The writer's retry path waits one
    slice before testing WriteFile again, which keeps backpressure bounded.
    """
    def __init__(self):
        self.keys = {}

    def get_map(self):
        return self.keys

    def register(self, fileobj, events, data=None):
        if fileobj in self.keys:
            raise KeyError(fileobj)
        key = selectors.SelectorKey(fileobj, fileobj, events, data)
        self.keys[fileobj] = key
        return key

    def unregister(self, fileobj):
        return self.keys.pop(fileobj)

    def select(self, timeout=None):
        if timeout is not None and timeout > 0:
            time.sleep(min(timeout, MAX_IO_SLICE))
        ready = []
        for key in self.keys.values():
            if key.events == selectors.EVENT_WRITE or readable_bytes(key.fd) != 0:
                ready.append((key, key.events))
        return ready

    def close(self):
        self.keys.clear()


def lock_file(handle, *, blocking=True):
    flags = 2 | (0 if blocking else 1)  # Exclusive, optionally fail immediately.
    if not lock_range(msvcrt.get_osfhandle(handle.fileno()), flags, 0, 1, 0, ctypes.byref(Overlapped())):
        error = ctypes.get_last_error()
        if error == 33:  # ERROR_LOCK_VIOLATION
            raise BlockingIOError(errno.EAGAIN, "lock is held")
        raise ctypes.WinError(error)


def open_guard(path):
    """Open a lease guard whose handle denies deletion and can be inherited."""
    handle = open_file(os.fspath(path), READ_WRITE, FILE_SHARE_READ_WRITE, None, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, None)
    if handle == INVALID_HANDLE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
    except BaseException:
        _winapi.CloseHandle(handle)
        raise
    return os.fdopen(fd, "r+b")


def assert_unleased(path):
    """Probe DELETE sharing while the caller holds the publication lock.

    The supervisor opens the generation guard without FILE_SHARE_DELETE. An inherited
    handle keeps that denial in force even after its supervisor has exited.
    The publication lock prevents any new guard from opening during collection.
    """
    handle = open_file(os.fspath(path), DELETE, FILE_SHARE_ALL, None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
    if handle == INVALID_HANDLE:
        error = ctypes.get_last_error()
        if error == 2:  # ERROR_FILE_NOT_FOUND means no guard exists.
            return
        if error == 32:  # ERROR_SHARING_VIOLATION
            raise BlockingIOError(errno.EAGAIN, "generation guard is leased")
        raise ctypes.WinError(error)
    _winapi.CloseHandle(handle)


_UNCONFIRMED = []


class OwnedWindowsProcess:
    """A native child and all descendants in a kernel-owned process job."""

    def __init__(self, argv, cwd, env, *, pass_fds=(), pass_handles=()):
        self.argv = list(argv)
        self.cwd = cwd
        self.pid = 0
        self._job = None
        self._process = None
        self._stdout = None
        self._stderr = None
        self._exit = None
        self._was_terminated = False
        environment = {key: value for key, value in env.items() if key != "BUTLER_API_KEY"}
        self._job = create_job(None, None)
        if not self._job:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = ExtendedLimit()
        limits.basic.flags = KILL_ON_JOB_CLOSE
        inherited = {}
        raw_pipes = []
        duplicates = []
        try:
            if not set_job(self._job, JOB_EXTENDED_LIMIT, ctypes.byref(limits), ctypes.sizeof(limits)):
                raise ctypes.WinError(ctypes.get_last_error())
            with open(os.devnull, "rb") as null:
                stdout_read, stdout_write = _winapi.CreatePipe(None, 0)
                raw_pipes.extend((stdout_read, stdout_write))
                stderr_read, stderr_write = _winapi.CreatePipe(None, 0)
                raw_pipes.extend((stderr_read, stderr_write))
                startup = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESTDHANDLES)
                current = _winapi.GetCurrentProcess()
                for attribute, handle in (("hStdInput", msvcrt.get_osfhandle(null.fileno())),
                                          ("hStdOutput", stdout_write), ("hStdError", stderr_write)):
                    duplicate = _winapi.DuplicateHandle(current, handle, current, 0, True, _winapi.DUPLICATE_SAME_ACCESS)
                    duplicates.append(duplicate)
                    setattr(startup, attribute, duplicate)
                for fd in pass_fds:
                    duplicates.append(_winapi.DuplicateHandle(
                        current, msvcrt.get_osfhandle(fd), current, 0, True, _winapi.DUPLICATE_SAME_ACCESS))
                for handle in pass_handles:
                    inherited[handle] = os.get_handle_inheritable(handle)
                    os.set_handle_inheritable(handle, True)
                startup.lpAttributeList = {"handle_list": [*duplicates, *pass_handles]}
                process, thread, self.pid, _ = _winapi.CreateProcess(
                    self.argv[0], subprocess.list2cmdline(self.argv), None, None, True,
                    CREATE_SUSPENDED | subprocess.CREATE_NO_WINDOW, environment, cwd, startup)
                self._process = process
                try:
                    if not assign_job(self._job, process) or resume_thread(thread) == 0xffffffff:
                        error = ctypes.get_last_error()
                        _winapi.TerminateProcess(process, 1)
                        raise ctypes.WinError(error)
                finally:
                    _winapi.CloseHandle(thread)
                self._stdout = msvcrt.open_osfhandle(stdout_read, os.O_RDONLY | os.O_BINARY)
                raw_pipes.remove(stdout_read)
                self._stderr = msvcrt.open_osfhandle(stderr_read, os.O_RDONLY | os.O_BINARY)
                raw_pipes.remove(stderr_read)
        except BaseException:
            self._close()
            raise
        finally:
            for handle, was_inheritable in inherited.items():
                os.set_handle_inheritable(handle, was_inheritable)
            for handle in [*duplicates, *raw_pipes]:
                _winapi.CloseHandle(handle)

    @property
    def stdout_fd(self):
        return self._stdout

    @property
    def stderr_fd(self):
        return self._stderr

    def read_stdout(self, size):
        return read_pipe(self.stdout_fd, size)

    def read_stderr(self, size):
        return read_pipe(self.stderr_fd, size)

    def wait_exit_nowait(self):
        if self._exit is not None:
            return self._exit, None
        if _winapi.WaitForSingleObject(self._process, 0) == WAIT_TIMEOUT:
            return None
        self._exit = _winapi.GetExitCodeProcess(self._process)
        return self._exit, None

    def _active_processes(self):
        accounting = BasicAccounting()
        if not query_job(self._job, JOB_BASIC_ACCOUNTING, ctypes.byref(accounting), ctypes.sizeof(accounting), None):
            return None
        return accounting.active_processes

    def request_terminate(self):
        if self._job and terminate_job(self._job, 1):
            self._was_terminated = True

    def _close(self):
        for attribute in ("_stdout", "_stderr"):
            fd = getattr(self, attribute, None)
            if fd is not None:
                os.close(fd)
                setattr(self, attribute, None)
        for attribute in ("_job", "_process"):
            handle = getattr(self, attribute, None)
            if handle:
                _winapi.CloseHandle(handle)
                setattr(self, attribute, None)

    def cleanup(self, *, cancelled, now=time.monotonic, sleep=time.sleep):
        from editor_tool import CleanupReport, KILL_OBSERVE_DEADLINE

        if self._job is None:
            return CleanupReport(True, forced=self._was_terminated, exit_code=self._exit)
        if self._active_processes() != 0:
            self.request_terminate()
        deadline = now() + KILL_OBSERVE_DEADLINE
        while self._active_processes() != 0 and now() < deadline:
            sleep(MAX_IO_SLICE)
        observed = self.wait_exit_nowait()
        confirmed = observed is not None and self._active_processes() == 0
        if confirmed:
            self._close()
        elif self not in _UNCONFIRMED:
            _UNCONFIRMED.append(self)
        return CleanupReport(confirmed, forced=self._was_terminated, exit_code=self._exit)

    def finalize_normal(self):
        return self.cleanup(cancelled=False)

    def __del__(self):
        self._close()
