"""Ludus editor owned tooling adapter (build/run, external debugging, project setup).

This is a one-operation process, not a persistent daemon. The GUI (or a CLI test
harness) launches it with --stdio, sends exactly one request on stdin, keeps that
pipe open for cancellation / output credit / parent-lifetime detection, and reads
newline-delimited UTF-8 JSON events on stdout. stderr carries bounded adapter
diagnostics only. See .kiro/specs/editor-workspace/design.md sections 7-10.

Design guarantees implemented here:
  * Asynchronous selectors supervisor; no worker threads, no communicate() full
    buffers, no GUI-thread-equivalent blocking waits.
  * Each long command (configure/build/game) is spawned with Popen, shell=False,
    close_fds=True, stdin=DEVNULL, start_new_session=True, so the adapter owns a
    process group containing ordinary descendants.
  * Cancellation (explicit cancel message, stdin EOF, SIGTERM/SIGINT) latches and
    cleans up: TERM the group, drain, escalate to KILL after a deadline, observe
    termination, and only then emit the terminal result.
  * Non-reaping exit observation on Linux and macOS (waitid WNOWAIT) keeps the leader's
    PID valid until descendant cleanup is complete.
  * Bounded output credit window and bounded control output; no unbounded buffers.
  * Missing result / abnormal exit is never reported as success.

Bridge exit codes: 0 success, 1 failed, 2 protocol failure, 130 cancelled.
"""
from __future__ import annotations

import argparse
from collections import deque
import errno
import json
import os
import platform
import selectors
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional

import editor_project
from ludus_tools.descriptor import parse_descriptor_bytes
from ludus_tools.errors import ToolingError


PROTOCOL_VERSION = 1

# Bounded resource limits (design section 10), conservative initial values.
OUTPUT_CREDIT_WINDOW = 256 * 1024       # encoded output bytes in flight
CONTROL_RESERVE_BYTES = 4 * 1024        # reserved for the terminal result frame
CONTROL_AGGREGATE_CAP = 1 * 1024 * 1024 # aggregate control output per job
ENGINE_PIN_MAX_BYTES = 8 * 1024        # one engine_pin control frame
CHILD_READ_CHUNK = 4 * 1024             # raw bytes per child read
MAX_PROTOCOL_LINE = 256 * 1024          # inbound control line cap (framing)
STDERR_CAP = 64 * 1024                  # adapter stderr per operation
READY_REQUEST_TIMEOUT = 5.0             # seconds to receive the one request
TERM_DEADLINE = 2.0                     # seconds before escalating TERM -> KILL
KILL_OBSERVE_DEADLINE = 1.0             # seconds to observe termination after KILL

# Keep uncertain leaders waitable. Reaping before descendant absence is proved
# would allow PID/process-group reuse while a caller still believes it owns it.
_UNKNOWN_PROCESS_OWNERS: list["OwnedProcess"] = []

# File API named client for the editor.
FILE_API_CLIENT = "ludus-editor"


def engine_setup_environment(root: Path, engine) -> dict[str, str]:
    """Prepare the editor's own engine using its managed Apple toolchain.

    GUI launch environments can contain an unrelated SDKROOT. This operation
    explicitly chooses the editor's engine, so reuse its prepared SDK instead.
    Without a prepared SDK, let init probe installed candidates normally.
    Shell init keeps honoring explicit SDKROOT overrides independently.
    """
    env = engine.tool_env(root)
    if platform.system() == "Darwin":
        sdk = root / "out/host-tools/macos-sdk"
        env.pop("LUDUS_INIT_SHELL_SDKROOT", None)
        if sdk.is_dir():
            env["SDKROOT"] = str(sdk.resolve())
        else:
            env.pop("SDKROOT", None)
    env["CI"] = "true"
    return env


class ProtocolError(Exception):
    """Fatal protocol framing/version/type failure; cleans up then exits 2."""


class BuildTreeBusy(Exception):
    """Raised when another editor instance already owns this build tree."""


class BuildTreeLock:
    """Cooperative, nonblocking per-build-tree exclusion held for one operation.

    A single editor instance owns a build tree for the whole configure/build/run
    operation, including runtime ownership (design section 7). A second instance
    attempting the same tree gets Busy rather than racing configure/build/run.
    The lock is a flock on a file inside the tree's `.cmake` directory, so it is
    scoped to exactly one binary tree and released when the fd is closed.

    This is cooperative: existing external CLI invocations do not honor it, which
    is a documented limitation (concurrently modifying the same build tree with a
    separate CLI build is unsupported).
    """

    def __init__(self, build_dir: Path) -> None:
        self._build_dir = build_dir
        self._handle = None

    def acquire(self) -> None:
        import fcntl

        lock_dir = self._build_dir / ".cmake"
        lock_dir.mkdir(parents=True, exist_ok=True)
        handle = (lock_dir / ".ludus-editor.lock").open("a")
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            handle.close()
            raise BuildTreeBusy(
                "another editor instance is using this build tree; "
                "concurrent operations on the same tree are not supported"
            ) from exc
        self._handle = handle

    def release(self) -> None:
        if self._handle is None:
            return
        import fcntl

        try:
            fcntl.flock(self._handle, fcntl.LOCK_UN)
        except OSError:
            pass
        try:
            self._handle.close()
        except OSError:
            pass
        self._handle = None

    def __enter__(self) -> "BuildTreeLock":
        self.acquire()
        return self

    def __exit__(self, *_exc) -> None:
        self.release()


# --------------------------------------------------------------------------- #
# Encoding helpers
# --------------------------------------------------------------------------- #
def hex16(value: int) -> str:
    """Encode a nonnegative integer as a 16-char lowercase hex string."""
    if value < 0 or value >= (1 << 64):
        raise ValueError("value out of range for 16-hex encoding")
    return f"{value:016x}"


def parse_hex16(text: Any) -> int:
    if not isinstance(text, str) or len(text) != 16:
        raise ProtocolError("expected a 16-character hex string")
    try:
        return int(text, 16)
    except ValueError as exc:
        raise ProtocolError("invalid hex string") from exc


class IncrementalDecoder:
    """UTF-8 decoder that replaces invalid sequences and never buffers unboundedly.

    Streaming child output may split multibyte characters across read chunks and
    may contain invalid UTF-8; this uses Python's incremental decoder with the
    'replace' error handler so a long line or invalid bytes cannot cause
    unbounded buffering (design section 10).
    """

    def __init__(self) -> None:
        import codecs

        self._decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")

    def decode(self, data: bytes, final: bool = False) -> str:
        return self._decoder.decode(data, final)


# --------------------------------------------------------------------------- #
# Protocol writer with bounded output-credit and control accounting
# --------------------------------------------------------------------------- #
class ProtocolWriter:
    """Writes JSON-line events to stdout honoring the output-credit window.

    Output frames consume credit (their complete encoded byte count including the
    newline). Control events (phase/command/targets/runtime_started/result) do
    not consume credit but their aggregate encoded size is capped, reserving
    space for the terminal result. When output credit is exhausted, ordinary
    output is dropped and counted rather than buffered.
    """

    def __init__(self, job: str, write: Callable[[bytes], None],
                 flush: Callable[[], None] = lambda: None) -> None:
        self._job = job
        self._write = write
        self.flush = flush
        self._credit = OUTPUT_CREDIT_WINDOW
        self._output_total = 0        # cumulative encoded output bytes emitted
        self._acknowledged = 0        # last acknowledged boundary
        self._control_total = 0       # aggregate control bytes emitted
        self.dropped_blocks = 0
        self.dropped_bytes = 0

    @property
    def output_total(self) -> int:
        return self._output_total

    def grant_credit(self, through: int) -> None:
        """Apply an acknowledgement up to `through` cumulative output bytes."""
        if through > self._output_total:
            raise ProtocolError("credit acknowledges a future/non-boundary offset")
        if through < self._acknowledged:
            return  # stale/duplicate acknowledgements are idempotent
        self._acknowledged = through
        # Credit equals the fixed window minus outstanding (unacknowledged) bytes.
        outstanding = self._output_total - self._acknowledged
        self._credit = OUTPUT_CREDIT_WINDOW - outstanding

    def _emit(self, obj: dict) -> bytes:
        obj["protocol"] = PROTOCOL_VERSION
        line = (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        return line

    def control(self, obj: dict, *, reserved: bool = False) -> None:
        obj.setdefault("job", self._job)
        line = self._emit(obj)
        budget = CONTROL_AGGREGATE_CAP if reserved else (CONTROL_AGGREGATE_CAP - CONTROL_RESERVE_BYTES)
        if self._control_total + len(line) > budget:
            raise ProtocolError("aggregate control output exceeded its bound")
        self._control_total += len(line)
        self._write(line)

    def output(self, stage: str, stream: str, text: str) -> None:
        if not text:
            return
        obj = {"job": self._job, "type": "output", "stage": stage, "stream": stream, "text": text}
        obj["end_offset"] = hex16(0)
        line = self._emit(obj)
        # Determine the encoded size up front so end_offset reflects it.
        size = len(line)
        if size > self._credit:
            # No credit: drop and count rather than buffer (keep draining child).
            self.dropped_blocks += 1
            self.dropped_bytes += len(text.encode("utf-8"))
            return
        self._output_total += size
        obj["end_offset"] = hex16(self._output_total)
        self._credit -= size
        self._write(self._emit(obj))

    def ready(self) -> None:
        # ready has protocol/type only; it does not echo the job.
        self._write(self._emit({"type": "ready"}))

    def phase(self, stage: str) -> None:
        self.control({"type": "phase", "stage": stage})

    def command(self, stage: str, argv: list[str], cwd: str) -> None:
        self.control({"type": "command", "stage": stage, "argv": list(argv), "cwd": cwd})

    def targets(self, names: list[str], preset: str) -> None:
        self.control({"type": "targets", "targets": list(names), "preset": preset})

    def runtime_started(self, pid: int, executable: str, cwd: str, args: list[str]) -> None:
        self.control({"type": "runtime_started", "pid": pid, "executable": executable,
                      "cwd": cwd, "args": list(args)})

    def debugger_started(self, pid: int, debugger: str, executable: str) -> None:
        self.control({"type": "debugger_started", "pid": pid, "debugger": debugger,
                      "executable": executable})

    def setup_details(self, text: str) -> None:
        # Separate from the reserved terminal summary and lossy output credit.
        if not text or len(text.encode("utf-8")) > 16 * 1024:
            raise ProtocolError("setup details exceeded 16 KiB or were empty")
        self.control({"type": "setup_details", "text": text})

    def engine_pin(self, pin: dict) -> None:
        # The project's selected engine install versus this editor's checkout
        # (ludus_tools.engine_pin). Bounded like setup details; sent before the
        # check runs so a failing check still offers the matching repair.
        from ludus_tools.engine_pin import PIN_STATES
        if pin.get("state") not in PIN_STATES:
            raise ProtocolError("engine pin state is invalid")
        frame = {"type": "engine_pin", **{key: pin[key] for key in
                 ("state", "installation", "pinned_revision", "editor_revision", "editor_owned", "message")}}
        if len(json.dumps(frame, ensure_ascii=False).encode("utf-8")) > ENGINE_PIN_MAX_BYTES:
            raise ProtocolError("engine pin exceeded its bound")
        self.control(frame)

    def result(self, outcome: str, stage: str, code: str, message: str,
               cleanup_confirmed: bool, exit_code: Optional[int], sig: Optional[int]) -> None:
        message = message.encode("utf-8")[:256].decode("utf-8", "ignore")
        self.control({"type": "result", "outcome": outcome, "stage": stage, "code": code,
                      "message": message, "cleanup_confirmed": cleanup_confirmed,
                      "exit_code": exit_code, "signal": sig}, reserved=True)


# --------------------------------------------------------------------------- #
# Owned child process group and non-reaping cleanup (POSIX)
# --------------------------------------------------------------------------- #
@dataclass
class CleanupReport:
    confirmed: bool
    forced: bool = False
    exit_code: Optional[int] = None
    signal: Optional[int] = None


class OwnedProcess:
    """One Popen child in its own session/process group, with group cleanup.

    The child is started with start_new_session=True so it leads a new process
    group that includes its ordinary descendants. Cleanup signals the whole group
    and uses non-reaping exit observation so the leader's PID stays valid (not
    reusable as a new group) until descendant cleanup completes.
    """

    def __init__(self, argv: list[str], cwd: str, env: dict[str, str], *, pass_fds: tuple[int, ...] = ()) -> None:
        self.argv = list(argv)
        self.cwd = cwd
        env = {key: value for key, value in env.items() if key != "BUTLER_API_KEY"}
        self._proc = subprocess.Popen(
            self.argv,
            cwd=cwd,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            close_fds=True,
            start_new_session=True,
            pass_fds=pass_fds,
        )
        self.pid = self._proc.pid

    @property
    def stdout_fd(self) -> int:
        return self._proc.stdout.fileno()

    @property
    def stderr_fd(self) -> int:
        return self._proc.stderr.fileno()

    def read_stdout(self, size: int) -> bytes:
        return os.read(self.stdout_fd, size)

    def read_stderr(self, size: int) -> bytes:
        return os.read(self.stderr_fd, size)

    def _observe_exit_nowait(self) -> Optional[tuple[Optional[int], Optional[int]]]:
        """Observe the leader's exit WITHOUT reaping it (WNOWAIT).

        Returns (exit_code, signal) if the leader has exited, else None. Using
        waitid(WNOWAIT) keeps the zombie leader present so its PID cannot be
        reused as a new, unrelated process group during cleanup.
        """
        if sys.platform == "darwin":
            from editor_process_macos import observe_exit
            return observe_exit(self.pid)
        try:
            info = os.waitid(os.P_PID, self.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        except ChildProcessError:
            return (None, None)
        except OSError as exc:
            if exc.errno == errno.ECHILD:
                return (None, None)
            raise
        if info is None:
            return None
        if info.si_code == os.CLD_EXITED:
            return (info.si_status, None)
        # Killed/dumped by a signal.
        return (None, info.si_status)

    def _group_members(self) -> Optional[list[int]]:
        """Live members of the owned group (excluding the zombie leader).

        Reads /proc to find processes whose process-group id equals the leader's
        pid. Inability to inspect is NOT treated as proof of absence; the caller
        distinguishes that from confirmed emptiness.
        """
        if sys.platform == "darwin":
            from editor_process_macos import group_members
            return group_members(self.pid)
        members: list[int] = []
        proc_root = Path("/proc")
        try:
            entries = [p for p in proc_root.iterdir() if p.name.isdigit()]
        except OSError:
            return None
        for entry in entries:
            pid = int(entry.name)
            if pid == self.pid:
                continue  # exclude the zombie leader itself
            try:
                stat = (entry / "stat").read_text()
            except FileNotFoundError:
                continue
            except OSError:
                return None
            # pgid is the 5th field after the (comm) which may contain spaces;
            # split on the last ')' to skip the comm field safely.
            try:
                after = stat.rsplit(")", 1)[1].split()
                pgid = int(after[2])  # fields: state ppid pgrp ...
            except (IndexError, ValueError):
                return None
            if pgid == self.pid and after[0] != "Z":
                members.append(pid)
        return members

    def _signal_group(self, sig: int) -> None:
        try:
            os.killpg(self.pid, sig)
        except ProcessLookupError:
            pass
        except PermissionError:
            pass

    def request_terminate(self) -> None:
        """Signal the still-owned, unreaped group without waiting for cleanup."""
        if self._proc.returncode is None:
            self._signal_group(signal.SIGTERM)

    def _close_pipes(self) -> None:
        for stream in (self._proc.stdout, self._proc.stderr):
            try:
                if stream is not None and not stream.closed:
                    stream.close()
            except OSError:
                pass

    def _reap_leader(self) -> tuple[Optional[int], Optional[int]]:
        self._close_pipes()
        try:
            _, status = os.waitpid(self.pid, 0)
        except ChildProcessError:
            if self._proc.returncode is None:
                self._proc.returncode = 0
            return (self._proc.returncode, None)
        # Record the outcome on the Popen so its finalizer does not warn or try to
        # reap an already-reaped pid.
        if os.WIFEXITED(status):
            self._proc.returncode = os.WEXITSTATUS(status)
            return (os.WEXITSTATUS(status), None)
        if os.WIFSIGNALED(status):
            self._proc.returncode = -os.WTERMSIG(status)
            return (None, os.WTERMSIG(status))
        self._proc.returncode = 0
        return (None, None)

    def cleanup(self, *, cancelled: bool, now: Callable[[], float] = time.monotonic,
                sleep: Callable[[float], None] = lambda s: time.sleep(min(s, 0.02))) -> CleanupReport:
        """Terminate the owned group and confirm cleanup within deadlines.

        Returns a CleanupReport. confirmed=False (CleanupUnknown) when a member
        could not be observed to terminate within the deadlines; this is never
        reported as success.
        """
        forced = False
        # Signal the whole group with TERM, then wait for the leader to exit.
        self._signal_group(signal.SIGTERM)
        deadline = now() + TERM_DEADLINE
        leader_exit: Optional[tuple[Optional[int], Optional[int]]] = None
        while now() < deadline:
            observed = self._observe_exit_nowait()
            if observed is not None and (observed[0] is not None or observed[1] is not None):
                leader_exit = observed
                break
            sleep(0.01)
        if leader_exit is None or (leader_exit[0] is None and leader_exit[1] is None):
            # Escalate to KILL and observe for a further bounded window.
            forced = True
            self._signal_group(signal.SIGKILL)
            kill_deadline = now() + KILL_OBSERVE_DEADLINE
            while now() < kill_deadline:
                observed = self._observe_exit_nowait()
                if observed is not None and (observed[0] is not None or observed[1] is not None):
                    leader_exit = observed
                    break
                sleep(0.01)

        # Clean up any surviving descendants with the same escalation, keeping
        # the leader waitable (not reaped) so its pid is not reused as a group.
        members = self._group_members()
        if members != []:
            self._signal_group(signal.SIGKILL)
            member_deadline = now() + KILL_OBSERVE_DEADLINE
            while now() < member_deadline and self._group_members() != []:
                sleep(0.01)
            members = self._group_members()

        # Now reap the leader (the zombie) to release its pid.
        exit_code, sig = (None, None)
        if leader_exit is not None:
            exit_code, sig = leader_exit
        confirmed = leader_exit is not None and members == []
        if confirmed:
            reaped = self._reap_leader()
            if exit_code is None and sig is None:
                exit_code, sig = reaped
        else:
            self._close_pipes()
            if self not in _UNKNOWN_PROCESS_OWNERS:
                _UNKNOWN_PROCESS_OWNERS.append(self)
        return CleanupReport(confirmed=confirmed, forced=forced, exit_code=exit_code, signal=sig)

    def wait_exit_nowait(self) -> Optional[tuple[Optional[int], Optional[int]]]:
        """Public non-reaping exit check for the normal (non-cancel) path."""
        return self._observe_exit_nowait()

    def finalize_normal(self) -> CleanupReport:
        """Normal direct-child exit still requires descendant cleanup.

        The leader has exited; observe it without reaping, verify no descendants
        remain in the owned group, then reap the leader.
        """
        observed = self._observe_exit_nowait()
        if observed is None or observed == (None, None):
            return self.cleanup(cancelled=False)
        members = self._group_members()
        if members != []:
            # Ordinary descendants outlived the leader (e.g. a surviving writer):
            # terminate them with deadlines before reaping the leader.
            self._signal_group(signal.SIGTERM)
            deadline = time.monotonic() + TERM_DEADLINE
            while time.monotonic() < deadline and self._group_members() != []:
                time.sleep(0.01)
            if self._group_members() != []:
                self._signal_group(signal.SIGKILL)
                kdeadline = time.monotonic() + KILL_OBSERVE_DEADLINE
                while time.monotonic() < kdeadline and self._group_members() != []:
                    time.sleep(0.01)
            members = self._group_members()
        exit_code, sig = observed
        if members == []:
            reaped = self._reap_leader()
            if exit_code is None and sig is None:
                exit_code, sig = reaped
        else:
            self._close_pipes()
            if self not in _UNKNOWN_PROCESS_OWNERS:
                _UNKNOWN_PROCESS_OWNERS.append(self)
        return CleanupReport(confirmed=members == [], forced=False, exit_code=exit_code, signal=sig)


# --------------------------------------------------------------------------- #
# Command planning for the two providers
# --------------------------------------------------------------------------- #
@dataclass
class ToolContext:
    tooling_root: Path
    engine: Any
    cmake_targets: Any


@dataclass
class Plan:
    """Resolved, immutable plan for one operation built from a saved descriptor."""
    descriptor: editor_project.Descriptor
    source_dir: Path
    build_dir: Path
    cmake: list[str]            # absolute managed cmake argv prefix
    env: dict[str, str]
    run_cwd: Path
    resolution: Any = None     # canonical installed SDK resolution for v2 projects
    compiler: Optional[Path] = None
    runtime_identity: Optional[str] = None


def _resolve_managed_cmake(context: ToolContext) -> str:
    cmake_path = context.engine.cmake(context.tooling_root)
    if not Path(cmake_path).exists():
        raise editor_project.ProjectError(
            "MissingTools",
            "the managed CMake is not available; run ./init.sh in the tooling checkout first",
        )
    return str(cmake_path)


def build_plan(context: ToolContext, descriptor: editor_project.Descriptor, project_dir: Path,
               *, descriptor_path: Optional[Path] = None) -> Plan:
    """Resolve directories, tools and environment for the descriptor's provider.

    Paths are resolved relative to the descriptor directory (source_dir) and the
    resolved source (run.cwd), never relative to the adapter's launch directory.
    """
    engine = context.engine
    source_dir = (project_dir / descriptor.source_dir).resolve()
    if not source_dir.is_dir():
        raise editor_project.ProjectError("InvalidProject", f"source directory does not exist: {source_dir}")
    if len(str(source_dir).encode("utf-8")) > 4096:
        raise editor_project.ProjectError("InvalidProject", "resolved source path exceeds 4096 bytes")
    if not (source_dir / "CMakeLists.txt").is_file() or not (source_dir / "CMakePresets.json").is_file():
        raise editor_project.ProjectError(
            "InvalidProject", "source must contain CMakeLists.txt and CMakePresets.json")

    run_cwd = (source_dir / descriptor.run_cwd).resolve()

    cmake = [_resolve_managed_cmake(context)]

    if descriptor.provider == "ludus":
        # Verify the opened source is a Ludus checkout and reuse its bootstrap,
        # managed tools and prepared environment. Missing/stale preparation is an
        # actionable error that directs the user to run init explicitly.
        if not (source_dir / "config" / "tool_versions.json").is_file():
            raise editor_project.ProjectError(
                "InvalidProject", "provider 'ludus' requires a Ludus engine checkout as source")
        try:
            engine.ensure_bootstrap_for_preset(source_dir, descriptor.preset)
        except engine.EngineError as exc:
            raise editor_project.ProjectError("BootstrapStale", str(exc)) from exc
        build_dir = engine.build_dir_for_preset(source_dir, descriptor.preset)
        env = engine.tool_env(source_dir)
    else:
        # External project: use the trusted tooling checkout's managed tools with
        # the external source's own CMakePresets.json. The two supported native
        # presets must produce single-config Ninja trees under out/build/<preset>.
        build_dir = source_dir / "out" / "build" / descriptor.preset
        env = engine.tool_env(context.tooling_root)
        # The external sample supplies CMAKE_PREFIX_PATH via $env{LUDUS_SDK_PREFIX}
        # in its preset; the adapter does not choose/build/install an SDK.

    resolution = None
    compiler = None
    if getattr(descriptor, "version", 1) == 2 and descriptor.provider == "cmake":
        from ludus_tools.identity import detect_host_toolchain, require_compatible
        from ludus_tools.operations import resolve_project
        from ludus_tools.sdkstore import SdkStore, default_store_root

        # Resolve metadata from the same backend as the installed CLI. The
        # adapter owns streaming process supervision, not another SDK resolver.
        resolved = resolve_project(descriptor_path or project_dir / "ludus.project.json",
                                   store=SdkStore(default_store_root()), enforce_host_toolchain=False)
        resolution = resolved.resolution
        compiler = context.tooling_root / "out" / "host-tools" / "bin" / "clang++"
        if not compiler.is_file():
            raise editor_project.ProjectError("MissingTools", "prepare the pinned Clang toolchain before building")
        host = detect_host_toolchain(resolution.identity, cxx=str(compiler), strict=True)
        require_compatible(resolution.identity, required_flavor=resolved.flavor,
                           required_features=descriptor.engine.features or None, reference=host)
        source_dir = resolved.paths.source_dir
        build_dir = resolved.paths.build_dir
        from ludus_tools.project_setup import cmake_for
        cmake = [cmake_for(source_dir, descriptor.preset, cmake[0])]
        run_cwd = (project_dir / descriptor.run_cwd).resolve()
        env["LUDUS_SDK_PREFIX"] = str(resolution.prefix)

    env.pop("BUTLER_API_KEY", None)

    sdk_identity = resolution.identity if resolution is not None else None
    if sdk_identity is None and env.get("LUDUS_SDK_PREFIX"):
        from ludus_tools.identity import load_prefix_manifest
        sdk_identity = load_prefix_manifest(Path(env["LUDUS_SDK_PREFIX"]))
    runtime_identity = None
    if sdk_identity is not None:
        runtime_identity = (f"{sdk_identity.source_revision}|{sdk_identity.sdk_variant}|{sdk_identity.cxx_runtime_abi}|"
                            f"{sdk_identity.compiler_id} {sdk_identity.compiler_version}|{sdk_identity.target_triple}|abi-1.1")

    return Plan(
        descriptor=descriptor,
        source_dir=source_dir,
        build_dir=build_dir,
        cmake=cmake,
        env=env,
        run_cwd=run_cwd,
        resolution=resolution,
        compiler=compiler,
        runtime_identity=runtime_identity,
    )


def configure_argv(plan: Plan) -> list[str]:
    from ludus_tools.project_setup import preset_for
    argv = [*plan.cmake, "--preset", preset_for(plan.source_dir, plan.descriptor.preset), "-B", str(plan.build_dir)]
    if plan.resolution is not None:
        from ludus_tools.operations import configure_argv as sdk_configure_argv
        argv = sdk_configure_argv(plan.cmake[0], plan.descriptor.preset, plan.source_dir, plan.build_dir)
        # Resolution compared this exact compiler with the SDK. Do not let a
        # preset silently choose a different system compiler during configure.
        argv.append(f"-DCMAKE_CXX_COMPILER={plan.compiler}")
    return argv


def build_argv(plan: Plan) -> list[str]:
    return [*plan.cmake, "--build", str(plan.build_dir), "--target", plan.descriptor.target]


# --------------------------------------------------------------------------- #
# Supervisor: runs one owned command to completion while servicing control I/O
# --------------------------------------------------------------------------- #
class Cancelled(Exception):
    """Raised internally when cancellation is latched; drives orderly cleanup.

    When raised after a child group has been cleaned up, `report` carries that
    CleanupReport so the caller can report cleanup confirmation accurately.
    """

    def __init__(self, report: Optional["CleanupReport"] = None) -> None:
        super().__init__("cancelled")
        self.report = report


class Supervisor:
    """Event loop around one OwnedProcess plus the control (stdin) pipe.

    Control messages (cancel/credit) and parent EOF are serviced while the child
    runs. No worker threads; a single selectors loop multiplexes the control fd
    and both child pipes with monotonic deadlines.
    """

    def __init__(self, job: str, writer: ProtocolWriter, control_fd: int,
                 on_control: Callable[[dict], None]) -> None:
        self._job = job
        self._writer = writer
        self._control_fd = control_fd
        self._on_control = on_control
        self._control_buf = b""
        self.cancel_latched = False
        self.captured_stdout = ""
        self.capture_stdout = False
        self._stderr_decoders: dict[int, IncrementalDecoder] = {}

    def _output(self, stage, stream, text):
        if self.capture_stdout and stream == "stdout":
            if len((self.captured_stdout + text).encode()) > MAX_PROTOCOL_LINE:
                raise ProtocolError("captured command output exceeded its bound")
            self.captured_stdout += text
        self._writer.output(stage, stream, text)

    def latch_cancel(self) -> None:
        self.cancel_latched = True

    def _read_control(self) -> None:
        self._dispatch_control_lines()
        try:
            chunk = os.read(self._control_fd, CHILD_READ_CHUNK)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            chunk = b""
        if not chunk:
            # stdin EOF is parent loss and cancels the operation.
            self.latch_cancel()
            raise Cancelled()
        self._control_buf += chunk
        if len(self._control_buf) > MAX_PROTOCOL_LINE:
            raise ProtocolError("inbound control line exceeded its bound")
        self._dispatch_control_lines()

    def _dispatch_control_lines(self) -> None:
        while b"\n" in self._control_buf:
            line, self._control_buf = self._control_buf.split(b"\n", 1)
            if not line.strip():
                continue
            try:
                message = json.loads(line.decode("utf-8"))
            except (ValueError, UnicodeDecodeError) as exc:
                raise ProtocolError("malformed control frame") from exc
            if not isinstance(message, dict) or message.get("protocol") != PROTOCOL_VERSION:
                raise ProtocolError("bad control protocol/version")
            mtype = message.get("type")
            if mtype == "cancel":
                if message.get("job") != self._job:
                    raise ProtocolError("control job mismatch")
                self.latch_cancel()
                raise Cancelled()
            if mtype == "credit":
                if message.get("job") != self._job:
                    raise ProtocolError("control job mismatch")
                self._writer.grant_credit(parse_hex16(message.get("through")))
                continue
            raise ProtocolError(f"unknown control type {mtype!r}")

    def run_command(self, stage: str, child: OwnedProcess, *, timeout: Optional[float] = None) -> CleanupReport:
        """Drain the child's streams while servicing control, until it exits.

        Returns the finalized CleanupReport. On cancellation or a protocol error
        the owned child group is cleaned up with deadlines before the exception
        propagates, so a long-running configure/build child is never leaked.
        """
        try:
            return self._run_command_loop(stage, child, timeout=timeout)
        except Cancelled:
            # Clean up the owned group with deadlines, then re-raise carrying the
            # cleanup report so the caller can report confirmation accurately.
            report = child.cleanup(cancelled=True)
            raise Cancelled(report)
        except ProtocolError as exc:
            report = child.cleanup(cancelled=True)
            if not report.confirmed:
                raise _CleanupUnknown(stage, report) from exc
            raise
        except OSError as exc:
            report = child.cleanup(cancelled=True)
            if not report.confirmed:
                raise _CleanupUnknown(stage, report) from exc
            raise ProtocolError(f"child I/O failed: {exc}") from exc

    def _run_command_loop(self, stage: str, child: OwnedProcess, *, timeout: Optional[float] = None) -> CleanupReport:
        deadline = time.monotonic() + timeout if timeout is not None else None
        sel = selectors.DefaultSelector()
        os.set_blocking(self._control_fd, False)
        os.set_blocking(child.stdout_fd, False)
        os.set_blocking(child.stderr_fd, False)
        sel.register(self._control_fd, selectors.EVENT_READ, "control")
        sel.register(child.stdout_fd, selectors.EVENT_READ, "stdout")
        sel.register(child.stderr_fd, selectors.EVENT_READ, "stderr")
        out_decoder = IncrementalDecoder()
        err_decoder = IncrementalDecoder()
        stderr_bytes = 0
        open_streams = {"stdout", "stderr"}
        # Once the leader has exited we stop waiting for stream EOF: a surviving
        # descendant may still hold the inherited pipe open (that is exactly the
        # "exiting leader with a surviving writer" case). We drain whatever is
        # immediately available, then let finalize_normal clean up descendants
        # with deadlines. This also avoids a pipe deadlock on a slow descendant.
        leader_exited = False
        try:
            while True:
                self._writer.flush()
                if deadline is not None and time.monotonic() >= deadline:
                    raise ProtocolError(f"{stage} exceeded its {timeout:g}-second deadline")
                for key, _ in sel.select(timeout=0.1):
                    tag = key.data
                    if tag == "control":
                        self._read_control()
                    elif tag == "stdout":
                        data = child.read_stdout(CHILD_READ_CHUNK)
                        if not data:
                            open_streams.discard("stdout")
                            try:
                                sel.unregister(child.stdout_fd)
                            except KeyError:
                                pass
                        else:
                            self._output(stage, "stdout", out_decoder.decode(data))
                    elif tag == "stderr":
                        data = child.read_stderr(CHILD_READ_CHUNK)
                        if not data:
                            open_streams.discard("stderr")
                            try:
                                sel.unregister(child.stderr_fd)
                            except KeyError:
                                pass
                        else:
                            stderr_bytes += len(data)
                            self._output(stage, "stderr", err_decoder.decode(data))

                # Completion is driven by the LEADER's exit, not by stream EOF.
                if not leader_exited:
                    observed = child.wait_exit_nowait()
                    if observed is not None and (observed[0] is not None or observed[1] is not None):
                        leader_exited = True
                if leader_exited:
                    # Both pipes already at EOF: nothing more will arrive. Done.
                    if not open_streams:
                        break
                    # Otherwise a descendant still holds a pipe. Give one more
                    # non-blocking drain pass, then stop waiting and let
                    # finalize_normal terminate the surviving descendants.
                    drained_any = False
                    for fd, tag in ((child.stdout_fd, "stdout"), (child.stderr_fd, "stderr")):
                        if tag not in open_streams:
                            continue
                        try:
                            data = os.read(fd, CHILD_READ_CHUNK)
                        except (BlockingIOError, InterruptedError):
                            continue
                        except OSError:
                            data = b""
                        if not data:
                            open_streams.discard(tag)
                            continue
                        drained_any = True
                        decoder = out_decoder if tag == "stdout" else err_decoder
                        self._output(stage, tag, decoder.decode(data))
                    if not drained_any:
                        break
        finally:
            sel.close()
        return child.finalize_normal()


# --------------------------------------------------------------------------- #
# Operation driver
# --------------------------------------------------------------------------- #
@dataclass
class OperationResult:
    outcome: str          # success/failed/cancelled
    stage: str
    code: str
    message: str
    cleanup_confirmed: bool = True
    exit_code: Optional[int] = None
    signal: Optional[int] = None


class Operation:
    """Runs one configure/build/build_run operation under a Supervisor."""

    def __init__(self, context: ToolContext, writer: ProtocolWriter, job: str,
                 operation: str, project_path: Path, expected_sha256: str,
                 control_fd: int, options: dict | None = None) -> None:
        self._options = options or {}
        self._context = context
        self._writer = writer
        self._job = job
        self._operation = operation
        self._project_path = project_path
        self._expected = expected_sha256
        self._supervisor = Supervisor(job, writer, control_fd, lambda _m: None)
        self._stage = "configuring"

    def latch_cancel(self) -> None:
        self._supervisor.latch_cancel()

    def _check_cancel(self) -> None:
        self._writer.flush()
        self._supervisor._read_control()
        if self._supervisor.cancel_latched:
            raise Cancelled()

    def _load_clean_descriptor(self) -> editor_project.Descriptor:
        import hashlib

        try:
            data = self._project_path.read_bytes()
        except OSError as exc:
            raise editor_project.ProjectError("InvalidProject", f"cannot read descriptor: {exc}") from exc
        digest = hashlib.sha256(data).hexdigest()
        if self._expected and digest != self._expected:
            raise editor_project.ProjectError(
                "Conflict", "descriptor digest does not match the expected clean snapshot")
        # Revalidate all fields before executing any tool.
        return parse_descriptor_bytes(data)

    def _run_stage(self, stage: str, argv: list[str], cwd: Path, env: dict[str, str],
                   code_on_fail: str, *, timeout: Optional[float] = None) -> None:
        self._stage = stage
        self._writer.phase(stage)
        self._writer.command(stage, argv, str(cwd))
        self._check_cancel()
        child = OwnedProcess(argv, str(cwd), env)
        report = self._supervisor.run_command(stage, child, timeout=timeout)
        if not report.confirmed:
            raise _CleanupUnknown(stage, report)
        if report.exit_code not in (0, None) or report.signal is not None:
            raise _StageFailed(stage, code_on_fail, report)

    def execute(self) -> OperationResult:
        try:
            if self._operation == "project_create":
                return self._execute_setup(None)
            descriptor = self._load_clean_descriptor()
            if self._operation in ("project_check", "project_setup"):
                return self._execute_setup(descriptor)
            if self._operation in ("release_init", "package"):
                return self._execute_release(descriptor)
            if self._operation == "build_debug":
                if platform.system() == "Darwin" or descriptor.preset.startswith("macos-"):
                    raise editor_project.ProjectError("MissingDebugger", "RAD debugging is available for Linux x64 only")
                return self._execute_debug(descriptor)
            plan = build_plan(self._context, descriptor, self._project_path.parent,
                              descriptor_path=self._project_path)
            from ludus_tools.cmake_setup import validate_project_presets

            try:
                validate_project_presets(plan.cmake[0], plan.source_dir, plan.env, descriptor.preset)
            except ValueError as exc:
                raise editor_project.ProjectError("InvalidProject", str(exc)) from exc

            if self._operation == "inspect_setup":
                from ludus_tools.cmake_setup import inspect_project_setup

                compiler = plan.compiler
                if compiler is None:
                    pinned = self._context.engine.pinned_llvm_tool(self._context.tooling_root, "clang++")
                    compiler = shutil.which(pinned, path=plan.env.get("PATH"))
                sdk_prefix = str(plan.resolution.prefix) if plan.resolution is not None else None
                try:
                    inspect_project_setup(
                        plan.cmake[0], str(self._context.engine.ninja(self._context.tooling_root)),
                        plan.source_dir, plan.build_dir, plan.env, descriptor.preset,
                        compiler=str(compiler) if compiler is not None else None,
                        sdk_prefix=sdk_prefix,
                    )
                except ValueError as exc:
                    raise editor_project.ProjectError("InvalidProject", str(exc)) from exc
                return OperationResult("success", "configuring", "Ok", "CMake project setup is ready")

            # Acquire the cooperative per-build-tree lock for the WHOLE operation,
            # including runtime ownership. A second editor instance on the same
            # tree gets Busy here rather than racing configure/build/run.
            with BuildTreeLock(plan.build_dir):
                return self._execute_locked(descriptor, plan)
        except BuildTreeBusy as exc:
            return OperationResult("failed", self._stage, "Busy", str(exc))
        except Cancelled as exc:
            confirmed = exc.report.confirmed if exc.report is not None else True
            code = "Cancelled" if confirmed else "CleanupUnknown"
            outcome = "cancelled" if confirmed else "failed"
            exit_code = exc.report.exit_code if exc.report else None
            sig = exc.report.signal if exc.report else None
            return OperationResult(outcome, self._stage, code, "operation cancelled",
                                   cleanup_confirmed=confirmed, exit_code=exit_code, signal=sig)
        except _CleanupUnknown as exc:
            return OperationResult("failed", exc.stage, "CleanupUnknown", "cleanup could not be confirmed",
                                   cleanup_confirmed=False,
                                   exit_code=exc.report.exit_code, signal=exc.report.signal)
        except _StageFailed as exc:
            return OperationResult("failed", exc.stage, exc.code, exc.message,
                                   exit_code=exc.exit_code, signal=exc.signal)
        except (OSError, subprocess.CalledProcessError) as exc:
            return OperationResult("failed", self._stage, "ReleaseFailed" if self._operation in ("release_init", "package") else "SpawnFailed", str(exc))
        except ToolingError as exc:
            code = "ReleaseFailed" if self._operation in ("release_init", "package") else exc.code if exc.code in ("MissingTools", "MissingDebugger", "ArtifactInvalid", "ConfigureFailed", "BuildFailed", "Busy", "Conflict") else "InvalidProject"
            return OperationResult("failed", self._stage, code, f"{exc.code}: {exc.message}")
        except editor_project.ProjectError as exc:
            return OperationResult("failed", self._stage, exc.code, exc.message)
        except self._context.engine.EngineError as exc:
            return OperationResult("failed", self._stage, "ReplyInvalid", str(exc))

    def _execute_debug(self, descriptor) -> OperationResult:
        import rad_debugger as rad
        import editor_debugger
        root = self._context.tooling_root
        engine = self._context.engine
        self._writer.phase("configuring")
        self._check_release_cancel()
        try:
            rad.require_host(engine)
            if descriptor.preset not in rad.DEBUG_PRESETS:
                raise engine.EngineError("Debugging requires a native Debug or Development preset")
            rad.validate_arguments(descriptor.run_args, engine)
        except engine.EngineError as exc:
            raise _StageFailed(self._stage, "InvalidProject", None, str(exc)) from exc
        if not os.environ.get("DISPLAY"):
            raise _StageFailed(self._stage, "MissingTools", None, "RAD needs an X11/XWayland display")
        preference = self._project_path.parent / ".ludus/debugger.json"
        if self._options.get("setup_debugger"):
            self._run_stage("configuring", [str(root / "scripts/setup-rad-debugger"), "--no-system-install"],
                            root, engine.tool_env(root), "DebuggerFailed")
        binary = editor_debugger.resolve_debugger(root, preference, engine,
                   override=self._options.get("debugger") or None,
                   managed=self._options.get("setup_debugger", False))
        self._check_release_cancel()
        plan = build_plan(self._context, descriptor, self._project_path.parent,
                          descriptor_path=self._project_path)
        from ludus_tools.cmake_setup import validate_project_presets
        try:
            validate_project_presets(plan.cmake[0], plan.source_dir, plan.env, descriptor.preset)
        except ValueError as exc:
            raise editor_project.ProjectError("InvalidProject", str(exc)) from exc
        if not plan.run_cwd.is_dir():
            raise _StageFailed(self._stage, "InvalidProject", None, "Game working directory does not exist")
        # The same build-tree and RAD session locks cover configure, build and
        # the complete session. CLI and Editor launches cannot race each other.
        target_session = plan.source_dir / "out/debug/rad" / descriptor.preset / descriptor.target
        try:
            with BuildTreeLock(plan.build_dir), rad.debug_session(root, binary, descriptor.preset,
                    descriptor.target, engine, target_session=target_session) as session:
                if self._options.get("debugger") or self._options.get("setup_debugger"):
                    editor_debugger.save_preference(preference, binary,
                        managed=bool(self._options.get("setup_debugger")))
                self._debug_session = session
                return self._execute_locked(descriptor, plan)
        except rad.SessionBusy as exc:
            raise _StageFailed(self._stage, "Busy", None, str(exc)) from exc

    def _launch_debugger(self, plan: Plan, artifact: Path) -> OperationResult:
        import editor_debugger
        editor_debugger.validate_symbols(artifact)
        self._stage = "launching"
        self._writer.phase(self._stage)
        session = self._debug_session
        argv = session.prepare_launch(artifact, plan.run_cwd, plan.descriptor.run_args)
        provider = "managed " + session.managed_pin["version"] if session.managed_pin else "external (unverified)"
        self._writer.output(self._stage, "stdout",
            f"RAD: {session.binary} ({provider})\nSession: {session.directory}\n"
            "Set breakpoints and Run/Step in RAD. The Editor does not observe game pause/run state.\n"
            "Use Stop to end this session and its game. Do not Save To Project for the temporary target.\n")
        self._writer.command(self._stage, argv, str(plan.run_cwd))
        self._check_release_cancel()
        child = OwnedProcess(argv, str(plan.run_cwd), plan.env)
        self._stage = "debugging"
        self._writer.debugger_started(child.pid, str(session.binary), str(artifact))
        report = self._supervisor.run_command(self._stage, child)
        if not report.confirmed:
            raise _CleanupUnknown(self._stage, report)
        if report.exit_code not in (0, None) or report.signal is not None:
            raise _StageFailed(self._stage, "DebuggerFailed", report, "RAD session exited abnormally")
        return OperationResult("success", self._stage, "Ok", "RAD session closed; game outcome is not observed",
                               exit_code=report.exit_code)

    def _check_release_cancel(self) -> None:
        self._supervisor._read_control()
        self._check_cancel()

    def _execute_setup(self, descriptor) -> OperationResult:
        from ludus_tools.project_setup import check_project, repair_project, supported_project, select_repair_sdk
        from ludus_tools.create import create_project
        self._stage = "configuring"
        self._writer.phase(self._stage)
        self._check_release_cancel()
        if descriptor is not None:
            supported_project(self._project_path)
        elif self._project_path.exists():
            raise ToolingError("DestinationExists", "New Project destination already exists")
        def runner(argv, *, cwd, env):
            capture = "--version" in argv or any(arg.startswith("--list-presets=") for arg in argv)
            self._supervisor.capture_stdout = capture
            self._supervisor.captured_stdout = ""
            try:
                self._check_release_cancel()
                building = "--build" in argv or "--install" in argv or Path(argv[0]).name == "build" or "ctest" in Path(argv[0]).name
                self._run_stage("building" if building else "configuring", list(argv), cwd, env,
                                "BuildFailed" if building else "ConfigureFailed")
                return self._supervisor.captured_stdout
            finally:
                self._supervisor.capture_stdout = False
        options = dict(tooling_root=self._context.tooling_root, runner=runner, cancel_check=self._check_release_cancel)
        if descriptor is not None and self._operation == "project_check":
            from ludus_tools.engine_pin import diagnose_engine_pin
            pin = diagnose_engine_pin(self._project_path, tooling_root=self._context.tooling_root)
            if pin is not None:
                self._writer.engine_pin(pin.to_json())
        if self._operation == "project_check":
            message = check_project(self._project_path, **options)
        else:
            sdk = Path(self._options["sdk"]) if self._options["sdk"] else None
            web = Path(self._options["web_sdk"]) if self._options["web_sdk"] else None
            options["disable_web"] = self._options.get("disable_web", False)
            if self._options.get("prepare_engine"):
                root = self._context.tooling_root
                from ludus_tools.native import default_profile
                profile = descriptor.preset if descriptor else default_profile()
                env = engine_setup_environment(root, self._context.engine)
                self._run_stage("configuring", [str(root / "scripts/init"), profile, "--preset-only", "--cli", "--no-system-install"], root, env, "ConfigureFailed")
                self._run_stage("building", [str(root / "scripts/build"), profile], root, env, "BuildFailed")
                if descriptor and "GraphicsRhi" in descriptor.engine.components:
                    self._run_stage("building", [str(root / "scripts/shader-probe"), "bootstrap"], root, env, "BuildFailed")
                sdk = sdk or root / "out/install" / profile
                self._run_stage("building", [str(self._context.engine.cmake(root)), "--install", str(root / "out/build" / profile), "--prefix", str(sdk)], root, env, "BuildFailed")
                self._run_stage("building", [str(self._context.engine.venv_python(root)), str(root / "scripts/python/engine.py"),
                                            "bundle-sdk-dependencies", profile, "--prefix", str(sdk)], root, env, "BuildFailed")
            if self._operation == "project_create":
                from ludus_tools.creation_engine import select_creation_sdk, prepare_creation_sdk
                root = self._context.tooling_root
                sdk = select_creation_sdk(root, explicit=sdk,
                    prepare=lambda prefix: prepare_creation_sdk(root, prefix, runner=runner,
                                                               environ=engine_setup_environment(root, self._context.engine),
                                                               cancel_check=self._check_release_cancel))
                self._writer.output(self._stage, "stdout", f"Selected Development engine: {sdk}\n")
                create_project(self._project_path, name=self._options["name"], template_id="minimal",
                               engine_version="", local_sdk_prefix=sdk, cancel_check=self._check_release_cancel,
                               verify_staged=lambda staged: repair_project(staged, sdk=sdk, web_sdk=web, **options))
                message = "Project created and verified; ready to open"
            else:
                if sdk is None:
                    from ludus_tools.creation_engine import prepare_creation_sdk
                    root = self._context.tooling_root
                    sdk = select_repair_sdk(self._project_path, tooling_root=root,
                        prepare=lambda prefix: prepare_creation_sdk(root, prefix, runner=runner,
                            environ=engine_setup_environment(root, self._context.engine),
                            cancel_check=self._check_release_cancel, profile=descriptor.preset))
                    self._writer.output(self._stage, "stdout", f"Selected engine: {sdk}\n")
                message = repair_project(self._project_path, sdk=sdk, web_sdk=web, **options)
        self._writer.output(self._stage, "stdout", message + "\n")
        if self._operation in ("project_check", "project_setup"):
            self._writer.setup_details(message)
        return OperationResult("success", self._stage, "Ok", message.split("\n", 1)[0])

    def _execute_release(self, descriptor) -> OperationResult:
        from ludus_tools.release_setup import setup_release
        from ludus_tools.release import package_project
        from ludus_tools.sdkstore import SdkStore
        if descriptor.version != 2 or descriptor.provider != "cmake":
            raise editor_project.ProjectError("InvalidProject", "release actions require a saved version-2 CMake project")
        self._stage = "configuring"
        self._writer.phase(self._stage)
        self._check_release_cancel()
        if self._operation == "release_init":
            env = {k: v for k, v in os.environ.items() if k != "BUTLER_API_KEY"}
            ref = subprocess.run(["git", "-C", str(self._context.tooling_root), "rev-parse", "HEAD"],
                                 env=env, capture_output=True, text=True, check=True).stdout.strip()
            files = setup_release(self._project_path, platform=self._options["platform"],
                                  itch_target=self._options["itch_target"] or None, tools_ref=ref,
                                  expected_descriptor_digest=self._expected, cancel_check=self._check_release_cancel)
            self._writer.output(self._stage, "stdout", "Saved release files: " + ", ".join(files) + "\n")
            return OperationResult("success", self._stage, "Ok", "release setup saved; review and commit generated files")
        count = 0
        def runner(argv, *, cwd, env):
            nonlocal count
            # Configure/build/install share the existing supervised process-group
            # ownership, bounded output and parent-loss cancellation mechanism.
            stage = "configuring" if count == 0 else "building"
            count += 1
            self._check_release_cancel()
            self._run_stage(stage, list(argv), cwd, env, "BuildFailed")
            return 0
        # Use trusted managed tools for the shared backend's CMake/compiler lookup.
        original_env = os.environ.copy()
        try:
            os.environ.update(self._context.engine.tool_env(self._context.tooling_root))
            os.environ.pop("BUTLER_API_KEY", None)
            package = package_project(self._project_path, profile=self._options["profile"],
                                      version=self._options["version"], store=SdkStore(),
                                      sdk=Path(self._options["sdk"]) if self._options["sdk"] else None,
                                      run_command=runner, cancel_check=self._check_release_cancel)
        finally:
            os.environ.clear()
            os.environ.update(original_env)
        self._writer.output(self._stage, "stdout", f"Package: {package}\nSHA256: {package.name}\n")
        return OperationResult("success", self._stage, "Ok", "release package verified; directory shown in Output")

    def _execute_locked(self, descriptor: editor_project.Descriptor, plan: Plan) -> OperationResult:
        """Run the operation while holding the per-build-tree lock."""
        if self._operation == "cook_scripts":
            from ludus_tools.behavior_workspace import arguments
            try:
                sdk = plan.resolution.prefix if plan.resolution is not None else self._context.tooling_root/"out/install"/descriptor.preset
                argv = arguments(self._project_path.parent, plan.build_dir, sdk)
            except (ValueError, OSError) as error:
                raise _StageFailed("building", "InvalidProject", None, str(error)) from error
            self._run_stage("building", [sys.executable, str(sdk/"share/Ludus/behavior/behavior_cook.py"), *argv],
                            plan.source_dir, plan.env, "BuildFailed", timeout=60)
            if plan.resolution is not None:
                from ludus_tools.resolve import assert_stamp_unchanged
                assert_stamp_unchanged(plan.build_dir, plan.resolution)
            return OperationResult("success", "building", "Ok", "Scripts cooked; build/reload to activate this candidate")
        # Named codemodel query before configure; the reply is read after a
        # successful configure.
        self._context.cmake_targets.query_codemodel(plan.build_dir, FILE_API_CLIENT)
        self._check_cancel()
        self._run_stage("configuring", configure_argv(plan), plan.source_dir, plan.env, "ConfigureFailed")
        if plan.resolution is not None:
            from ludus_tools.resolve import assert_stamp_unchanged, write_stamp
            assert_stamp_unchanged(plan.build_dir, plan.resolution)
            write_stamp(plan.build_dir, plan.resolution)
        # Discover executable targets; verify source/build identity.
        self._context.cmake_targets.verify_identity(plan.build_dir, plan.source_dir,
                                                    self._context.engine, client=FILE_API_CLIENT)
        names = self._context.cmake_targets.list_executable_targets(
            plan.build_dir, self._context.engine, client=FILE_API_CLIENT)
        self._writer.targets(names, descriptor.preset)

        if self._operation == "configure":
            return OperationResult("success", "configuring", "Ok", "configure complete")

        # Resolve the selected executable target before building it.
        self._resolve_executable(plan)
        self._check_cancel()
        if plan.resolution is not None:
            from ludus_tools.resolve import assert_stamp_unchanged
            assert_stamp_unchanged(plan.build_dir, plan.resolution)
        self._run_stage("building", build_argv(plan), plan.source_dir, plan.env, "BuildFailed")
        if plan.resolution is not None:
            assert_stamp_unchanged(plan.build_dir, plan.resolution)

        # CMake may regenerate during build: reread the current codemodel and
        # re-resolve the artifact before Run.
        self._context.cmake_targets.verify_identity(plan.build_dir, plan.source_dir,
                                                    self._context.engine, client=FILE_API_CLIENT)
        artifact = self._resolve_executable(plan)
        if not (artifact.is_file() and os.access(artifact, os.X_OK)):
            raise _StageFailed("building", "ArtifactInvalid", None,
                               message=f"built artifact is missing or not executable: {artifact}")

        if self._operation == "build":
            return OperationResult("success", "building", "Ok", "build complete")

        # Cancellation wins if accepted before runtime spawn.
        self._check_cancel()
        if self._operation == "build_debug":
            return self._launch_debugger(plan, artifact)
        return self._launch_runtime(plan, artifact)

    def _resolve_executable(self, plan: Plan) -> Path:
        try:
            return self._context.cmake_targets.target_executable(
                plan.build_dir, plan.descriptor.target, self._context.engine, client=FILE_API_CLIENT)
        except self._context.engine.EngineError as exc:
            raise _StageFailed(self._stage, "TargetInvalid", None, message=str(exc)) from exc

    def _launch_runtime(self, plan: Plan, artifact: Path) -> OperationResult:
        self._stage = "launching"
        self._writer.phase("launching")
        run_argv = [str(artifact), *plan.descriptor.run_args]
        self._writer.command("launching", run_argv, str(plan.run_cwd))
        if not plan.run_cwd.is_dir():
            raise _StageFailed("launching", "SpawnFailed", None,
                               message=f"run working directory does not exist: {plan.run_cwd}")
        self._check_cancel()
        try:
            child = OwnedProcess(run_argv, str(plan.run_cwd), plan.env)
        except OSError as exc:
            raise _StageFailed("launching", "SpawnFailed", None, message=str(exc)) from exc
        self._stage = "running"
        self._writer.phase("running")
        self._writer.runtime_started(child.pid, str(artifact), str(plan.run_cwd), plan.descriptor.run_args)
        try:
            report = self._supervisor.run_command("running", child)
        except Cancelled as exc:
            # run_command already cleaned up the owned group with deadlines.
            report = exc.report if exc.report is not None else child.cleanup(cancelled=True)
            self._stage = "stopping"
            self._writer.phase("stopping")
            if not report.confirmed:
                return OperationResult("failed", "stopping", "CleanupUnknown",
                                       "cleanup could not be confirmed", cleanup_confirmed=False,
                                       exit_code=report.exit_code, signal=report.signal)
            return OperationResult("cancelled", "stopping", "Cancelled", "runtime stopped",
                                   cleanup_confirmed=True, exit_code=report.exit_code, signal=report.signal)
        if not report.confirmed:
            return OperationResult("failed", "running", "CleanupUnknown",
                                   "cleanup could not be confirmed", cleanup_confirmed=False,
                                   exit_code=report.exit_code, signal=report.signal)
        if report.signal is not None:
            return OperationResult("failed", "running", "RuntimeSignaled",
                                   f"runtime terminated by signal {report.signal}",
                                   exit_code=None, signal=report.signal)
        if report.exit_code not in (0, None):
            return OperationResult("failed", "running", "RuntimeFailed",
                                   f"runtime exited with code {report.exit_code}",
                                   exit_code=report.exit_code)
        return OperationResult("success", "running", "Ok", "runtime exited cleanly",
                               exit_code=report.exit_code)


class _StageFailed(Exception):
    def __init__(self, stage: str, code: str, report: Optional[CleanupReport], message: str = "") -> None:
        super().__init__(message or code)
        self.stage = stage
        self.code = code
        self.message = message or f"{stage} failed"
        self.exit_code = report.exit_code if report else None
        self.signal = report.signal if report else None


class _CleanupUnknown(Exception):
    def __init__(self, stage: str, report: CleanupReport) -> None:
        super().__init__("cleanup unknown")
        self.stage = stage
        self.report = report


# --------------------------------------------------------------------------- #
# stdio handshake and main
# --------------------------------------------------------------------------- #
def _read_one_request(control_fd: int, deadline: float) -> tuple[dict, bytes]:
    """Read exactly one request line within the deadline. Raises on timeout/EOF."""
    buf = b""
    os.set_blocking(control_fd, False)
    sel = selectors.DefaultSelector()
    sel.register(control_fd, selectors.EVENT_READ)
    try:
        while time.monotonic() < deadline:
            for _ in sel.select(timeout=0.1):
                try:
                    chunk = os.read(control_fd, CHILD_READ_CHUNK)
                except (BlockingIOError, InterruptedError):
                    continue
                if not chunk:
                    raise ProtocolError("stdin closed before a request was received")
                buf += chunk
                if len(buf) > MAX_PROTOCOL_LINE:
                    raise ProtocolError("request line exceeded its bound")
                if b"\n" in buf:
                    line, remaining = buf.split(b"\n", 1)
                    try:
                        message = json.loads(line.decode("utf-8"))
                    except (ValueError, UnicodeDecodeError) as exc:
                        raise ProtocolError("malformed request frame") from exc
                    return message, remaining
        raise ProtocolError("no request received within 5 seconds")
    finally:
        sel.close()


def _validate_request(message: dict) -> tuple[str, str, Path, str]:
    if not isinstance(message, dict) or message.get("protocol") != PROTOCOL_VERSION:
        raise ProtocolError("bad request protocol/version")
    job = message.get("job")
    if not isinstance(job, str) or len(job) != 16:
        raise ProtocolError("request job must be a 16-character hex string")
    try:
        int(job, 16)
    except ValueError as exc:
        raise ProtocolError("request job is not hex") from exc
    operation = message.get("operation")
    if operation not in ("configure", "build", "build_run", "build_debug", "build_generation", "cook_scripts", "inspect_setup", "release_init", "package", "project_check", "project_setup", "project_create"):
        raise ProtocolError("unknown operation")
    project = message.get("project")
    if not isinstance(project, str) or not os.path.isabs(project):
        raise ProtocolError("request project must be an absolute path")
    expected = message.get("expected_sha256", "")
    if not isinstance(expected, str) or (expected and len(expected) != 64):
        raise ProtocolError("expected_sha256 must be 64 hex characters")
    fields = {"platform": 16, "itch_target": 128} if operation == "release_init" else {"profile": 64, "version": 128, "sdk": 4096} if operation == "package" else {}
    if operation in ("project_setup", "project_create"):
        fields = {"sdk": 4096, "web_sdk": 4096}
        if operation == "project_create":
            fields["name"] = 256
        if not isinstance(message.get("prepare_engine"), bool):
            raise ProtocolError("prepare_engine must be boolean")
        if not isinstance(message.get("disable_web", False), bool):
            raise ProtocolError("disable_web must be boolean")
        if message.get("disable_web") and message.get("web_sdk"):
            raise ProtocolError("Choose browser setup or desktop-only repair, not both")
    if operation == "build_debug":
        fields = {"debugger": 4096}
        if not isinstance(message.get("setup_debugger"), bool):
            raise ProtocolError("setup_debugger must be boolean")
        if message.get("setup_debugger") and message.get("debugger"):
            raise ProtocolError("Choose setup or an existing debugger, not both")
    for key, maximum in fields.items():
        value = message.get(key)
        if not isinstance(value, str) or len(value.encode()) > maximum or any(ord(c) < 32 for c in value):
            raise ProtocolError("invalid release request field: " + key)
    return job, operation, Path(project), expected


def run_stdio(context: ToolContext, in_fd: int, out_fd: int) -> int:
    """Run the single-operation stdio protocol. Returns the bridge exit code."""
    frames: deque[bytes] = deque()
    queued_bytes = 0
    os.set_blocking(out_fd, False)
    def flush() -> None:
        nonlocal queued_bytes
        for _ in range(64):
            if not frames:
                break
            try:
                count = os.write(out_fd, frames[0])
            except (BlockingIOError, InterruptedError):
                break
            if count <= 0:
                raise ProtocolError("frontend output pipe closed")
            queued_bytes -= count
            if count == len(frames[0]):
                frames.popleft()
            else:
                frames[0] = frames[0][count:]
                break
    def write(data: bytes) -> None:
        nonlocal queued_bytes
        # Aggregate control + the fixed output-credit window + terminal reserve.
        if queued_bytes+len(data) > CONTROL_AGGREGATE_CAP+OUTPUT_CREDIT_WINDOW+CONTROL_RESERVE_BYTES:
            raise ProtocolError("frontend output queue exceeded its bound")
        frames.append(data)
        queued_bytes += len(data)
        flush()

    def finish(code: int) -> int:
        deadline = time.monotonic()+2
        with selectors.DefaultSelector() as selector:
            selector.register(out_fd, selectors.EVENT_WRITE)
            while frames and time.monotonic() < deadline:
                flush()
                if frames:
                    selector.select(0.02)
        return code if not frames else 1

    # Minimal pre-handshake writer (no job yet) just for the `ready` frame.
    def write_ready() -> None:
        write((json.dumps({"protocol": PROTOCOL_VERSION, "type": "ready"}) + "\n").encode("utf-8"))

    try:
        write_ready()
        message, remaining = _read_one_request(in_fd, time.monotonic() + READY_REQUEST_TIMEOUT)
        job, operation, project, expected = _validate_request(message)
    except ProtocolError as exc:
        try:
            write((json.dumps({"protocol": PROTOCOL_VERSION, "type": "result", "outcome": "failed",
                               "stage": "configuring", "code": "ProtocolError", "message": str(exc)[:256],
                               "cleanup_confirmed": True, "exit_code": None, "signal": None}) + "\n")
                  .encode("utf-8"))
        except OSError:
            pass
        return finish(2)

    writer = ProtocolWriter(job, write, flush)
    if operation == "build_generation":
        from play_build import GenerationOperation
        operation_obj = GenerationOperation(context, writer, job, operation, project, expected, in_fd, message)
    else:
        operation_obj = Operation(context, writer, job, operation, project, expected, in_fd, message)
    operation_obj._supervisor._control_buf = remaining

    # SIGTERM/SIGINT only latch cancellation; cleanup runs before exit.
    def _handle_signal(_signum, _frame):
        operation_obj.latch_cancel()

    try:
        signal.signal(signal.SIGTERM, _handle_signal)
        signal.signal(signal.SIGINT, _handle_signal)
    except (ValueError, OSError):
        pass  # not on the main thread (e.g. under test); the loop still checks EOF

    try:
        result = operation_obj.execute()
    except ProtocolError as exc:
        writer.result("failed", operation_obj._stage, "ProtocolError", str(exc), True, None, None)
        return finish(2)

    writer.result(result.outcome, result.stage, result.code, result.message,
                  result.cleanup_confirmed, result.exit_code, result.signal)

    if not result.cleanup_confirmed:
        return finish(1)
    if result.outcome == "cancelled":
        return finish(130)
    if result.outcome == "success":
        return finish(0)
    return finish(1)


def _make_context(tooling_root: Path) -> ToolContext:
    import engine as engine_module
    import cmake_targets as cmake_targets_module

    return ToolContext(tooling_root=tooling_root, engine=engine_module, cmake_targets=cmake_targets_module)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="Ludus editor tooling adapter")
    parser.add_argument("--stdio", action="store_true", required=True,
                        help="run the one-operation stdio protocol")
    parser.add_argument("--tooling-root", required=True, help="absolute trusted Ludus tooling checkout")
    args = parser.parse_args(argv)

    tooling_root = Path(args.tooling_root).resolve()
    context = _make_context(tooling_root)
    return run_stdio(context, sys.stdin.fileno(), sys.stdout.fileno())


if __name__ == "__main__":
    sys.exit(main())
