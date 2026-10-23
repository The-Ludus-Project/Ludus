"""Step plan, progress markers and their parser for ./init.sh.

The engine runner announces each setup step on stdout. In a terminal the
announcement is a readable ``==> [3/9] Label`` header (styled by ``init_terminal``). When the graphical setup
window runs the engine as a child process it sets ``LUDUS_INIT_PROGRESS=1``
and the runner prints machine-readable lines instead::

    LUDUS_PROGRESS <index> <total> <label>         a step began (1-based index)
    LUDUS_PROGRESS_DETAIL <done> <total> <text>    progress inside the step
    LUDUS_PROGRESS_NOTE <text>                     a transient notice, such as a
                                                   sudo password prompt
    LUDUS_PROGRESS_UP_TO_DATE <text>               the current step found its inputs
                                                   unchanged and skipped its work

``SetupProgress`` folds those lines, and Ninja's ``[done/total]`` status lines,
into the state the window displays. It has no Tk dependency so it can be tested
without a desktop.
"""
from __future__ import annotations

import os
import platform
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Sequence

import init_preflight
import init_terminal


PROGRESS_ENVIRONMENT = "LUDUS_INIT_PROGRESS"
STEP_MARKER = "LUDUS_PROGRESS"
DETAIL_MARKER = "LUDUS_PROGRESS_DETAIL"
NOTE_MARKER = "LUDUS_PROGRESS_NOTE"
UP_TO_DATE_MARKER = "LUDUS_PROGRESS_UP_TO_DATE"
UP_TO_DATE_LABEL = "up to date"

# Ninja prints "[12/345] Building CXX object ..." for every edge when its
# output is not a terminal.
NINJA_STATUS = re.compile(r"^\[(\d+)/(\d+)\]\s+(.*)$")
# Colour and cursor escapes from tools that ignore the missing terminal.
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
STEP_LINE = re.compile(r"^" + STEP_MARKER + r" (\d+) (\d+) (.+)$")
DETAIL_LINE = re.compile(r"^" + DETAIL_MARKER + r" (\d+) (\d+) ?(.*)$")
NOTE_LINE = re.compile(r"^" + NOTE_MARKER + r" (.+)$")
UP_TO_DATE_LINE = re.compile(r"^" + UP_TO_DATE_MARKER + r" ?(.*)$")
ERROR_PREFIX = "error: "
COMPLETION_PREFIX = "Ludus initialization"
PREFLIGHT_LABEL = "Checking this computer"


@dataclass(frozen=True)
class Step:
    key: str
    label: str


def native_plan(args, *, system: str | None = None, environment=None) -> list[Step]:
    """Return the ordered steps ``engine.command_init`` runs for ``args``."""
    system = system or platform.system()
    environment = os.environ if environment is None else environment
    validating = bool(args.validate or args.ci)
    with_editor = bool(getattr(args, "with_editor", False))
    steps = []
    if not init_preflight.is_skipped(environment):
        steps.append(Step("preflight", PREFLIGHT_LABEL))
    if args.no_system_install:
        steps.append(Step("system", "Checking system tools"))
    else:
        steps.append(Step("system", "Installing system prerequisites"))
    single = args.preset_only and not validating
    steps.append(Step("host_tools", "Installing pinned CMake, Ninja and Conan"))
    steps.append(Step("conan", "Conan install" + (" (" + args.preset + ")" if single else " (all native presets)")))
    steps.append(Step("cmake_caches", "Checking CMake caches"))
    if args.with_rad_debugger:
        steps.append(Step("rad", "Building RAD Debugger"))
    steps.append(Step("doctor", "Verifying tools"))
    if not args.ci and not environment.get("CI"):
        steps.append(Step("hooks", "Installing Git hooks"))
    if with_editor:
        if not args.no_system_install:
            steps.append(Step("editor_packages", "Installing Qt for the editor" + (" (Homebrew)" if system == "Darwin" else "")))
        if args.with_editor_preview:
            # Fetched in the background from the Conan step on; this step
            # waits for the download and verification to finish.
            steps.append(Step("preview_tools", "Fetching pinned shader tools"))
        steps.append(Step("editor_configure", "CMake configure (editor)"))
        steps.append(Step("editor_build", "Building the editor"))
        if not getattr(args, "skip_engine_refresh", False):
            steps.append(Step("engine_refresh", "Refreshing project engine installs"))
    if getattr(args, "run_tests", False) and not validating:
        steps.append(Step("tests", "Building and running tests"))
    if validating:
        steps.append(Step("validate_build", "Building " + args.preset))
        steps.append(Step("validate_test", "Running tests"))
        if not args.skip_checks:
            steps.append(Step("checks", "Formatting and static analysis"))
        if not args.skip_sanitizers:
            steps.append(Step("sanitizers", "Sanitizer build and tests"))
        if not args.skip_sdk:
            steps.append(Step("sdk", "SDK install and consumer test"))
    return steps


def browser_plan(args) -> list[Step]:
    """Return the ordered steps of browser ``init`` in ``web_build.command``."""
    steps = [
        Step("host_tools", "Installing pinned CMake and Ninja"),
        Step("emsdk", "Installing the Emscripten SDK"),
        Step("verify", "Verifying browser tools"),
    ]
    if args.with_smoke_app:
        steps.append(Step("shader_tools", "Fetching pinned shader tools"))
    steps.append(Step("configure", "CMake configure (" + args.preset + ")"))
    return steps


def plan(args, *, system: str | None = None, environment=None) -> list[Step]:
    if str(args.preset).startswith("web-emscripten-"):
        return browser_plan(args)
    return native_plan(args, system=system, environment=environment)


def is_machine_readable(environment=None) -> bool:
    environment = os.environ if environment is None else environment
    return environment.get(PROGRESS_ENVIRONMENT) == "1"


class Reporter:
    """Announce the steps of one plan; unknown keys are ignored."""

    def __init__(self, steps: Sequence[Step], *, machine_readable: bool, write=print, clock=time.monotonic):
        self.steps = list(steps)
        self.machine_readable = machine_readable
        self.write = write
        self.clock = clock
        self.started_at = clock()
        self.current = 0

    def begin(self, key: str) -> None:
        for index, step in enumerate(self.steps, start=1):
            if step.key == key:
                self.current = index
                if self.machine_readable:
                    self.write(f"{STEP_MARKER} {index} {len(self.steps)} {step.label}", flush=True)
                else:
                    self.write("", flush=True)
                    self.write(init_terminal.step_line(index, len(self.steps), step.label, init_terminal.painter_for()),
                               flush=True)
                return

    def elapsed(self) -> str:
        """Time since the plan started, as ``mm:ss`` or ``h:mm:ss``."""
        return format_elapsed(self.clock() - self.started_at)

    def fail(self) -> None:
        """Name the step that stopped a terminal run; the setup window shows its own failure state."""
        if self.machine_readable or self.current == 0:
            return
        step = self.steps[self.current - 1]
        painter = init_terminal.painter_for()
        self.write("", flush=True)
        self.write(init_terminal.failure(
            f"Setup stopped at step {self.current}/{len(self.steps)}: {step.label} (after {self.elapsed()}).", painter),
            flush=True)

    def up_to_date(self, text: str) -> None:
        """Say that the current step skipped its work because its inputs are unchanged."""
        if self.machine_readable:
            self.write(f"{UP_TO_DATE_MARKER} {text}", flush=True)
        else:
            self.write(init_terminal.up_to_date(text, init_terminal.painter_for()), flush=True)

    def detail(self, done: int, total: int, text: str) -> None:
        if self.machine_readable:
            self.write(f"{DETAIL_MARKER} {done} {total} {text}", flush=True)

    def note(self, text: str) -> None:
        if self.machine_readable:
            self.write(f"{NOTE_MARKER} {text}", flush=True)


_active: Reporter | None = None


def start(steps: Sequence[Step], *, environment=None) -> Reporter:
    """Make ``steps`` the current plan; later ``begin`` calls announce them."""
    global _active
    _active = Reporter(steps, machine_readable=is_machine_readable(environment))
    return _active


def stop() -> None:
    global _active
    _active = None


def fail() -> None:
    """Report which step of the current plan failed, in a terminal run."""
    if _active is not None:
        _active.fail()


def elapsed() -> str:
    """Time since the current plan started; empty without a plan."""
    return _active.elapsed() if _active is not None else ""


def begin(key: str) -> None:
    """Announce step ``key`` of the current plan; without a plan (bootstrap, tests) do nothing."""
    if _active is not None:
        _active.begin(key)


def up_to_date(text: str) -> None:
    """Report that the current step is skipped; without a plan just print ``text``."""
    if _active is not None:
        _active.up_to_date(text)
    else:
        print(text, flush=True)


def detail(done: int, total: int, text: str) -> None:
    if _active is not None:
        _active.detail(done, total, text)


def note(text: str) -> None:
    if _active is not None:
        _active.note(text)


def announce_privileged_command(argv: Sequence[object], probe=None) -> None:
    """Tell the setup window that sudo is about to ask for a password in the terminal."""
    if _active is None or not _active.machine_readable or not argv:
        return
    if Path(str(argv[0])).name != "sudo":
        return
    if probe is None:
        import subprocess

        def probe():
            try:
                # -n never prompts; success means the credentials are cached.
                return subprocess.run([str(argv[0]), "-n", "true"], stdin=subprocess.DEVNULL,
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False).returncode == 0
            except OSError:
                return False
    if not probe():
        _active.note("Waiting for your sudo password in the terminal that launched setup")


# ---------------------------------------------------------------------------
# Parsing (the window side)

PENDING = "pending"
RUNNING = "running"
DONE = "done"
FAILED = "failed"
CANCELLED = "cancelled"


@dataclass
class StepState:
    label: str
    status: str = PENDING
    started_at: float | None = None
    finished_at: float | None = None
    is_up_to_date: bool = False


@dataclass
class SetupProgress:
    """Fold child output lines into step states, sub-progress and outcome."""

    steps: list[StepState] = field(default_factory=list)
    current: int = -1
    detail_done: int = 0
    detail_total: int = 0
    detail_text: str = ""
    has_runner_detail: bool = False
    note: str = ""
    last_line: str = ""
    is_last_line_status: bool = False
    error: str = ""
    summary: list[str] = field(default_factory=list)
    outcome: str = RUNNING

    @classmethod
    def from_plan(cls, steps: Sequence[Step]) -> SetupProgress:
        return cls(steps=[StepState(step.label) for step in steps])

    @property
    def has_started_step(self) -> bool:
        return self.current >= 0

    def fraction(self) -> float:
        """Overall completion in [0, 1]; the current step contributes its detail ratio."""
        if not self.steps:
            return 0.0
        if self.outcome == DONE:
            return 1.0
        completed = sum(1 for step in self.steps if step.status == DONE)
        partial = 0.0
        if self.current >= 0 and self.steps[self.current].status == RUNNING and self.detail_total > 0:
            partial = min(self.detail_done, self.detail_total) / self.detail_total
        return min(1.0, (completed + partial) / len(self.steps))

    def feed(self, raw_line: str, now: float) -> bool:
        """Consume one output line; return True when it belongs in the visible log."""
        line = clean_line(raw_line)
        match = STEP_LINE.match(line)
        if match:
            self._begin(int(match.group(1)), int(match.group(2)), match.group(3), now)
            return False
        match = DETAIL_LINE.match(line)
        if match:
            self.detail_done, self.detail_total, self.detail_text = int(match.group(1)), int(match.group(2)), match.group(3)
            self.has_runner_detail = True
            return False
        match = NOTE_LINE.match(line)
        if match:
            self.note = match.group(1)
            return False
        match = UP_TO_DATE_LINE.match(line)
        if match:
            if self.current >= 0:
                self.steps[self.current].is_up_to_date = True
            self.last_line = match.group(1)
            return True
        if not line.strip():
            return True
        self.note = ""
        self.last_line = line
        match = NINJA_STATUS.match(line)
        # A runner's own detail (Conan's preset count) outranks the Ninja
        # status of a third-party package it happens to be building.
        self.is_last_line_status = bool(match) and not self.has_runner_detail
        if self.is_last_line_status:
            self.detail_done, self.detail_total = int(match.group(1)), int(match.group(2))
            self.detail_text = match.group(3)
        if line.startswith(ERROR_PREFIX):
            self.error = line[len(ERROR_PREFIX):]
        if line.startswith(COMPLETION_PREFIX) or self.summary:
            self.summary.append(line)
        return True

    def finish(self, exit_code: int | None, now: float, *, cancelled: bool = False) -> None:
        """Record the child's end. ``exit_code`` None means it could not start."""
        if cancelled:
            self.outcome = CANCELLED
        elif exit_code == 0:
            self.outcome = DONE
        else:
            self.outcome = FAILED
            if not self.error:
                self.error = ("Setup could not start." if exit_code is None
                              else f"Setup stopped with exit code {exit_code}.")
        for step in self.steps:
            if step.status == RUNNING:
                step.status = DONE if self.outcome == DONE else (CANCELLED if cancelled else FAILED)
                step.finished_at = now
            elif self.outcome == DONE and step.status == PENDING:
                # A step the runner skipped at run time (for example nothing to refresh).
                step.status = DONE
        self.note = ""

    def _begin(self, index: int, total: int, label: str, now: float) -> None:
        if index < 1 or total < 1 or index > total:
            return
        if total != len(self.steps):
            # The runner's plan wins over the window's prediction.
            self.steps = [StepState(self.steps[i].label if i < len(self.steps) else "") for i in range(total)]
        for position in range(index - 1):
            step = self.steps[position]
            if step.status != DONE:
                step.status = DONE
                step.finished_at = now if step.finished_at is None else step.finished_at
        step = self.steps[index - 1]
        step.label = label
        step.status = RUNNING
        step.started_at = now
        self.current = index - 1
        self.detail_done = 0
        self.detail_total = 0
        self.detail_text = ""
        self.has_runner_detail = False
        self.note = ""


def clean_line(raw_line: str) -> str:
    """Drop escapes and keep only the final carriage-return redraw of a line."""
    line = ANSI_ESCAPE.sub("", raw_line.rstrip("\n"))
    if "\r" in line:
        parts = [part for part in line.split("\r") if part]
        line = parts[-1] if parts else ""
    return line


def format_elapsed(seconds: float) -> str:
    seconds = max(0, int(seconds))
    hours, remainder = divmod(seconds, 3600)
    minutes, seconds = divmod(remainder, 60)
    return f"{hours}:{minutes:02d}:{seconds:02d}" if hours else f"{minutes:02d}:{seconds:02d}"
