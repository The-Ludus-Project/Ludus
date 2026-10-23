"""Share one machine between concurrent Ludus builds.

Every scripted build starts one compiler per core, so several checkouts that
build at once (agent worktrees, a second terminal) oversubscribe the cores and
the memory: each compile then runs many times slower and the machine swaps.
A scripted build therefore holds a slot from a pool shared by every checkout of
the current user, and a build started from a linked Git worktree (where coding
agents usually work) runs at lower priority with a job count that leaves room
for the primary checkout's build.

Two pools exist. Builds from the primary checkout share the interactive pool
of one slot, so two of them take turns. Builds from linked worktrees share the
agent pool, whose size follows the machine's cores and memory, so agents never
take the primary checkout's slot. See docs/development/building.md, "Sharing
the machine between builds".
"""
from __future__ import annotations

import contextlib
import os
import platform
import shutil
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator, Mapping, Sequence

SLOTS_ENVIRONMENT = "LUDUS_BUILD_SLOTS"
JOBS_ENVIRONMENT = "LUDUS_BUILD_JOBS"
PRIORITY_ENVIRONMENT = "LUDUS_BUILD_PRIORITY"
ROLE_ENVIRONMENT = "LUDUS_BUILD_ROLE"
DIRECTORY_ENVIRONMENT = "LUDUS_BUILD_SLOT_DIRECTORY"
# Set for the commands a build runs, so a nested scripted build inside a held
# slot does not wait for a second one.
HELD_ENVIRONMENT = "LUDUS_BUILD_SLOT_HELD"
PARALLEL_LEVEL_ENVIRONMENT = "CMAKE_BUILD_PARALLEL_LEVEL"

AGENT_ROLE = "agent"
INTERACTIVE_ROLE = "interactive"
ROLES = (AGENT_ROLE, INTERACTIVE_ROLE)

NORMAL_PRIORITY = "normal"
LOW_PRIORITY = "low"
BACKGROUND_PRIORITY = "background"
PRIORITIES = (NORMAL_PRIORITY, LOW_PRIORITY, BACKGROUND_PRIORITY)

# One agent slot per this many cores and this much memory; a debug build of the
# editor with tests peaks at about one gibibyte per compile job, and the primary
# checkout's build needs the rest of the machine.
CORES_PER_AGENT_SLOT = 8
MEMORY_GIBIBYTES_PER_AGENT_SLOT = 16
MEMORY_GIBIBYTES_PER_COMPILE_JOB = 2
MINIMUM_SLOTS = 1
MINIMUM_JOBS = 1
INTERACTIVE_SLOTS = 1
BYTES_PER_GIBIBYTE = 1024 ** 3

# nice increments and Linux I/O scheduling for the lowered priorities.
LOW_NICE_INCREMENT = 10
BACKGROUND_NICE_INCREMENT = 19
BEST_EFFORT_INPUT_OUTPUT_CLASS = 2
LOWEST_BEST_EFFORT_INPUT_OUTPUT_LEVEL = 7
IDLE_INPUT_OUTPUT_CLASS = 3

WAIT_POLL_SECONDS = 1.0
WAIT_REPORT_SECONDS = 60.0
SECONDS_PER_MINUTE = 60

TRUE_WORDS = frozenset({"1", "on", "yes", "true"})


class BuildSlotError(RuntimeError):
    """A build slot setting in the environment is invalid."""


@dataclass(frozen=True)
class Machine:
    cores: int
    memory_gibibytes: int  # 0 when unknown


@dataclass(frozen=True)
class BuildPolicy:
    role: str
    slots: int  # 0 builds without waiting for a slot
    jobs: int | None  # None keeps the build tool's default
    priority: str


def detect_machine() -> Machine:
    """The cores this process may use and the machine's physical memory."""
    if hasattr(os, "sched_getaffinity"):
        cores = len(os.sched_getaffinity(0))
    else:
        cores = os.cpu_count() or MINIMUM_JOBS
    return Machine(cores=max(MINIMUM_JOBS, cores), memory_gibibytes=physical_memory_bytes() // BYTES_PER_GIBIBYTE)


def physical_memory_bytes() -> int:
    if platform.system() == "Darwin":
        try:
            result = subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True, check=True)
            return int(result.stdout.strip())
        except (OSError, subprocess.CalledProcessError, ValueError):
            return 0
    try:
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (AttributeError, OSError, ValueError):
        return 0


def is_linked_worktree(root: Path) -> bool:
    """Whether ``root`` is a linked Git worktree; its ``.git`` is a file naming the main repository."""
    return (root / ".git").is_file()


def default_agent_slots(machine: Machine) -> int:
    slots = machine.cores // CORES_PER_AGENT_SLOT
    if machine.memory_gibibytes > 0:
        slots = min(slots, machine.memory_gibibytes // MEMORY_GIBIBYTES_PER_AGENT_SLOT)
    return max(MINIMUM_SLOTS, slots)


def default_agent_jobs(machine: Machine) -> int:
    jobs = machine.cores
    if machine.memory_gibibytes > 0:
        jobs = min(jobs, machine.memory_gibibytes // MEMORY_GIBIBYTES_PER_COMPILE_JOB)
    return max(MINIMUM_JOBS, jobs)


def whole_number(environment: Mapping[str, str], name: str, minimum: int) -> int | None:
    text = environment.get(name, "").strip()
    if text == "":
        return None
    if not text.isdigit() or int(text) < minimum:
        raise BuildSlotError(f"{name}={text!r} must be a whole number of at least {minimum}")
    return int(text)


def choice(environment: Mapping[str, str], name: str, choices: Sequence[str]) -> str | None:
    text = environment.get(name, "").strip().lower()
    if text == "":
        return None
    if text not in choices:
        raise BuildSlotError(f"{name}={text!r} must be one of: {', '.join(choices)}")
    return text


def policy_for(root: Path, environment: Mapping[str, str], machine: Machine) -> BuildPolicy:
    """How a scripted build from ``root`` shares the machine, after the environment overrides."""
    role = choice(environment, ROLE_ENVIRONMENT, ROLES)
    if role is None:
        role = AGENT_ROLE if is_linked_worktree(root) else INTERACTIVE_ROLE
    slots = whole_number(environment, SLOTS_ENVIRONMENT, 0)
    jobs = whole_number(environment, JOBS_ENVIRONMENT, MINIMUM_JOBS)
    priority = choice(environment, PRIORITY_ENVIRONMENT, PRIORITIES)
    is_agent = role == AGENT_ROLE
    if slots is None:
        slots = default_agent_slots(machine) if is_agent else INTERACTIVE_SLOTS
    is_nested = environment.get(HELD_ENVIRONMENT, "").strip().lower() in TRUE_WORDS
    if is_nested:
        # The outer build already holds a slot and set the priority, which
        # every child process inherits.
        slots = 0
        priority = priority or NORMAL_PRIORITY
    if jobs is None and is_agent:
        jobs = default_agent_jobs(machine)
    if priority is None:
        priority = LOW_PRIORITY if is_agent else NORMAL_PRIORITY
    return BuildPolicy(role=role, slots=slots, jobs=jobs, priority=priority)


def priority_prefix(priority: str, system: str | None = None,
                    which: Callable[[str], str | None] = shutil.which) -> list[str]:
    """The command that runs a build at ``priority``; empty when it is normal or no tool is available."""
    if priority == NORMAL_PRIORITY:
        return []
    is_background = priority == BACKGROUND_PRIORITY
    system = platform.system() if system is None else system
    if system == "Darwin":
        # The QoS clamp also reaches every compiler that Ninja starts. The
        # background clamp keeps a build on the efficiency cores.
        taskpolicy = which("taskpolicy")
        if taskpolicy:
            return [taskpolicy, "-b"] if is_background else [taskpolicy, "-c", "utility"]
    prefix: list[str] = []
    if system == "Linux":
        ionice = which("ionice")
        if ionice:
            if is_background:
                prefix += [ionice, "-c", str(IDLE_INPUT_OUTPUT_CLASS)]
            else:
                prefix += [ionice, "-c", str(BEST_EFFORT_INPUT_OUTPUT_CLASS),
                           "-n", str(LOWEST_BEST_EFFORT_INPUT_OUTPUT_LEVEL)]
    nice = which("nice")
    if nice:
        increment = BACKGROUND_NICE_INCREMENT if is_background else LOW_NICE_INCREMENT
        prefix += [nice, "-n", str(increment)]
    return prefix


def has_parallel_argument(arguments: Sequence[object]) -> bool:
    for argument in map(str, arguments):
        if argument in ("-j", "--parallel") or argument.startswith(("-j", "--parallel=")):
            return True
    return False


def parallel_arguments(policy: BuildPolicy, extra_arguments: Sequence[object],
                       environment: Mapping[str, str]) -> list[str]:
    """``--parallel N`` for ``cmake --build`` unless the caller already chose a job count."""
    if policy.jobs is None or has_parallel_argument(extra_arguments):
        return []
    if environment.get(PARALLEL_LEVEL_ENVIRONMENT, "").strip() != "":
        return []
    return ["--parallel", str(policy.jobs)]


def slot_directory(environment: Mapping[str, str], system: str | None = None) -> Path:
    """The per-user directory of slot lock files, shared by every checkout."""
    configured = environment.get(DIRECTORY_ENVIRONMENT, "").strip()
    if configured != "":
        return Path(configured)
    system = platform.system() if system is None else system
    home = Path(environment.get("HOME") or Path.home())
    if system == "Darwin":
        return home / "Library" / "Caches" / "Ludus" / "build-slots"
    cache_home = environment.get("XDG_CACHE_HOME", "").strip()
    return (Path(cache_home) if cache_home != "" else home / ".cache") / "ludus" / "build-slots"


def try_lock(path: Path):
    """An open, exclusively locked handle on ``path``, or None while another build holds it."""
    import fcntl
    handle = open(path, "a+", encoding="utf-8")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        handle.close()
        return None
    return handle


def unlock(handle) -> None:
    import fcntl
    try:
        handle.seek(0)
        handle.truncate()
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
    finally:
        handle.close()


def holder_description(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def duration_text(seconds: float) -> str:
    minutes = int(seconds // SECONDS_PER_MINUTE)
    if minutes == 0:
        return f"{int(seconds)} s"
    return f"{minutes} min {int(seconds % SECONDS_PER_MINUTE)} s"


@contextlib.contextmanager
def hold_slot(directory: Path, pool: str, count: int, description: str, *,
              report: Callable[[str], None] | None = None,
              sleep: Callable[[float], None] = time.sleep,
              clock: Callable[[], float] = time.monotonic) -> Iterator[int | None]:
    """Hold one of ``count`` slots of ``pool`` while the block runs; yields the slot, or None without a pool.

    The lock belongs to this process's open file, so the operating system
    releases it when the build ends, even after a crash or Ctrl+C.
    """
    report = report_progress if report is None else report
    if count <= 0 or os.name != "posix":
        yield None
        return
    directory.mkdir(parents=True, exist_ok=True)
    paths = [directory / f"{pool}-{index}.lock" for index in range(count)]
    started = clock()
    reported = None
    while True:
        for index, path in enumerate(paths):
            handle = try_lock(path)
            if handle is None:
                continue
            try:
                handle.seek(0)
                handle.truncate()
                handle.write(description + "\n")
                handle.flush()
                if reported is not None:
                    report(f"Got an {pool} build slot after waiting {duration_text(clock() - started)}.")
                yield index
            finally:
                unlock(handle)
            return
        now = clock()
        if reported is None or now - reported >= WAIT_REPORT_SECONDS:
            holders = "; ".join(filter(None, (holder_description(path) for path in paths))) or "unknown builds"
            report(f"Waiting for an {pool} build slot (all {count} in use: {holders}). "
                   f"Set {SLOTS_ENVIRONMENT}=0 to build without waiting.")
            reported = now
        sleep(WAIT_POLL_SECONDS)


def report_progress(text: str) -> None:
    print(text, flush=True)


def describe(policy: BuildPolicy) -> str:
    jobs = "the default job count" if policy.jobs is None else f"{policy.jobs} jobs"
    return f"{policy.role} build: {jobs}, {policy.priority} priority"


@contextlib.contextmanager
def machine_share(root: Path, preset: str, policy: BuildPolicy, environment: dict[str, str] | None = None, *,
                  report: Callable[[str], None] | None = None) -> Iterator[None]:
    """Hold a slot for a scripted build of ``preset`` and mark the environment while it runs."""
    environment = os.environ if environment is None else environment
    report = report_progress if report is None else report
    is_nested = environment.get(HELD_ENVIRONMENT, "").strip().lower() in TRUE_WORDS
    if policy.role == AGENT_ROLE and not is_nested:
        report(f"Building as an {describe(policy)}; set {ROLE_ENVIRONMENT}={INTERACTIVE_ROLE} "
               "to build as the primary checkout.")
    description = f"{root} {preset} (process {os.getpid()})"
    with hold_slot(slot_directory(environment), policy.role, policy.slots, description, report=report):
        previous = environment.get(HELD_ENVIRONMENT)
        environment[HELD_ENVIRONMENT] = "1"
        try:
            yield
        finally:
            if previous is None:
                environment.pop(HELD_ENVIRONMENT, None)
            else:
                environment[HELD_ENVIRONMENT] = previous
