"""Verify every installed/public engine header compiles standalone.

Header self-sufficiency contract (docs/architecture/foundational-headers.md
Section 14, ADR 0007): a public header must compile on its own, i.e. it must
include everything it uses and MUST NOT rely on the consumer having included
something else first. The one sanctioned universally-available header is
<ludus/foundation/base/core.h>; a public header that uses the foundational
vocabulary includes core.h (or the specific Band 0 header) itself rather than
assuming it arrived transitively.

This is a mechanical gate, not prose: for each public header under
modules/**/include/ludus/** (and tools/**/include) it compiles a tiny
translation unit that includes ONLY that header (with -fsyntax-only).

The compile flags are reconstructed from compile_commands.json so the check
sees exactly what the build sees (the C++ standard, warnings-as-errors, the
generated-header include dirs, etc.). Because each module target carries only
its own include directories, a single translation unit's flags cannot resolve
headers from other modules; this script therefore takes a representative TU's
non-path flags and unions in the include directories (-I / -isystem / -iquote /
-idirafter and their generated-header variants) from EVERY entry in the compile
database, plus public source include roots for optional modules with no compiled
target in this profile, so every discovered public header can still be checked.

Each probe is an independent compiler process, so the probes run concurrently
on a bounded worker pool (LUDUS_HEADER_CHECK_JOBS, default: the CPUs available to
this process). Serial probing spent most of its wall time waiting for a CPU on
heavily shared hosts; every header still gets the same flags and its own
pass/fail result, and failures are reported in header order.

Usage: verify_header_self_sufficiency.py <build_dir> [module/submodule]
The optional module selects only public headers exported by a partial SDK.
"""

from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_compile_policy import compiler_options


# Public headers that are intentionally NOT standalone-compilable and are
# excluded with a documented reason. Keep this list empty unless a header has a
# genuine, explained reason.
KNOWN_NON_STANDALONE: dict[str, str] = {
    # These backend window headers #error out unless the build system selects
    # the backend (LUDUS_PLATFORM_WAYLAND / LUDUS_PLATFORM_HEADLESS). They are
    # only ever compiled inside the chosen backend's translation unit, which the
    # build graph already covers; a bare-include probe has no backend selected.
    "ludus/platform/wayland/window.h": "requires LUDUS_PLATFORM_WAYLAND from the build system",
    "ludus/platform/headless/window.h": "requires LUDUS_PLATFORM_HEADLESS from the build system",
}

# Header-SEARCH-PATH flags that take a following path argument. Deliberately
# excludes force-include flags (-include / -imacros / -include-pch): those pull
# a specific header/PCH into the TU rather than adding a search directory, and
# the whole point of the probe is a bare #include of one header. In particular a
# PCH -include (present when LUDUS_ENABLE_PCH is ON) must not be aggregated.
_INCLUDE_FLAGS_WITH_ARG = ("-I", "-isystem", "-iquote", "-idirafter")
# Joined spellings, e.g. -I/abs/path.
_INCLUDE_FLAG_PREFIXES = ("-I", "-isystem")


def arguments_of(entry) -> list[str]:
    return entry.get("arguments") or shlex.split(entry["command"])


def include_flags(entry) -> list[str]:
    """Extract only the header-search flags (with their path args) from an entry."""
    args = arguments_of(entry)
    result: list[str] = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg in _INCLUDE_FLAGS_WITH_ARG and index + 1 < len(args):
            result.append(arg)
            result.append(args[index + 1])
            index += 2
            continue
        if any(arg.startswith(prefix) and len(arg) > len(prefix) for prefix in _INCLUDE_FLAG_PREFIXES):
            result.append(arg)
        index += 1
    return result


def public_headers(root: Path) -> list[Path]:
    headers: list[Path] = []
    for module_include in (*root.glob("modules/*/include"), *root.glob("modules/*/*/include")):
        headers.extend(module_include.rglob("*.h"))
        headers.extend(module_include.rglob("*.hpp"))
    for tool_include in root.glob("tools/*/include"):
        headers.extend(tool_include.rglob("*.h"))
        headers.extend(tool_include.rglob("*.hpp"))
    # detail/ headers ship with the SDK and are instantiation points, so they
    # must also be self-sufficient; internal/ headers are private and excluded.
    return sorted(h for h in headers if "internal" not in h.parts)


def public_include_root(header: Path) -> Path:
    for parent in header.parents:
        if parent.name == "include":
            return parent
    raise ValueError(f"Public header has no include root: {header}")


def logical_include(header: Path) -> str:
    return str(header.relative_to(public_include_root(header)))


JOBS_VARIABLE = "LUDUS_HEADER_CHECK_JOBS"


def available_cpu_count() -> int:
    # Honour an affinity/cgroup-restricted CPU set where the platform exposes it.
    if hasattr(os, "sched_getaffinity"):
        return max(1, len(os.sched_getaffinity(0)))
    return max(1, os.cpu_count() or 1)


def requested_jobs() -> int:
    """Return the worker bound, or 0 when the override is not a positive integer."""
    requested = os.environ.get(JOBS_VARIABLE, "")
    if requested == "":
        return available_cpu_count()
    try:
        jobs = int(requested)
    except ValueError:
        return 0
    return jobs if jobs >= 1 else 0


def compile_probe(options: list[str], directory: str, include: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        options + ["-x", "c++", "-fsyntax-only", "-"],
        input=f"#include <{include}>\n", text=True, capture_output=True, cwd=directory,
    )


def representative_entry(entries):
    # Any engine TU's flags carry the C++ standard, defines, and warning policy.
    for entry in entries:
        if entry["file"].endswith("base/src/version.cpp"):
            return entry
    return entries[0]


def main() -> int:
    jobs = requested_jobs()
    if jobs == 0:
        print(f"{JOBS_VARIABLE} must be a positive integer, got {os.environ.get(JOBS_VARIABLE)!r}.")
        return 1
    build = Path(sys.argv[1]).resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    if not entries:
        print("No compile_commands.json entries; cannot verify header self-sufficiency.")
        return 1

    representative = representative_entry(entries)
    directory = representative["directory"]
    # Non-path compiler flags (standard, warnings, defines, sysroot, ...) from a
    # representative TU. compiler_options() already strips -o/-c and the source.
    base_options = compiler_options(representative)

    root = Path(__file__).resolve().parents[2]
    headers = public_headers(root)

    # Union of every entry's header-search flags so a probe for any module's
    # header resolves. Deduplicate while preserving order.
    seen: set[str] = set()
    aggregated_includes: list[str] = []
    tokens: list[str] = []
    for entry in entries:
        tokens.extend(include_flags(entry))
    # Default-off modules contribute no TU/include flags to the database. Their
    # public headers are nevertheless discovered and must compile standalone.
    for include_root in sorted({public_include_root(header) for header in headers}):
        tokens.extend(["-I", str(include_root)])
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in _INCLUDE_FLAGS_WITH_ARG and index + 1 < len(tokens):
            pair = (token, tokens[index + 1])
            key = f"{token}\0{tokens[index + 1]}"
            if key not in seen:
                seen.add(key)
                aggregated_includes.extend(pair)
            index += 2
            continue
        if token not in seen:
            seen.add(token)
            aggregated_includes.append(token)
        index += 1

    options = base_options + aggregated_includes

    failures: list[str] = []
    skipped: list[str] = []
    if len(sys.argv) > 2:
        module_root = (root / "modules" / sys.argv[2] / "include").resolve()
        headers = [header for header in headers if module_root in header.parents]
        if not headers:
            print("No public headers found for selected module")
            return 1
    includes: list[str] = []
    for header in headers:
        include = logical_include(header)
        if include in KNOWN_NON_STANDALONE:
            skipped.append(include)
            continue
        includes.append(include)
    checked = len(includes)

    jobs = max(1, min(jobs, checked))
    # Printed before compiling so a CTest timeout still shows the probe scale.
    print(f"Checking {checked} public headers with {jobs} concurrent compiler process(es).", flush=True)
    started = time.monotonic()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda include: compile_probe(options, directory, include), includes))
    elapsed = time.monotonic() - started
    for include, result in zip(includes, results):
        if result.returncode != 0:
            failures.append(f"{include}\n{result.stderr.strip()}")

    if failures:
        print(f"Header self-sufficiency FAILED for {len(failures)} of {checked} public headers:\n")
        for failure in failures:
            print(failure + "\n")
        return 1
    print(f"Header self-sufficiency OK: {checked} public headers each compile standalone "
          f"({len(skipped)} build-selected backend header(s) skipped; {elapsed:.1f} s wall).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
