"""Verify that unsafe owner uses fail to compile with actionable diagnostics.

docs/architecture/smart-pointers.md requires negative compilation for each
ownership slice:

- slice 1 ("Unique correctness"): throwing/mismatched deleters, unsafe ownership
  conversions, and an actionable diagnostic when the ordinary default deleter is
  instantiated for an incomplete type;
- slice 3 ("Shared and weak consumer"): forbidden raw-pointer adoption, arrays,
  unchecked downcasts, Derived-to-Base adjustment of a possibly expired weak
  value, and access through a weak value without Lock().

Constraint predicates are also checked by static_asserts in the unit tests
(modules/foundation/base/tests/pointer_tests.cpp,
modules/foundation/memory/tests/shared_ptr_tests.cpp); this gate proves the
corresponding real uses are rejected by the compiler.

Each suite's source is compiled with -fsyntax-only using the flags of a
representative translation unit of the owning module from
compile_commands.json. The file must compile without a case (control run), and
every case must fail with output matching its expected pattern.

Usage: verify_pointer_diagnostics.py <build_dir>
"""

from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_compile_policy import compiler_options, is_pch_entry


@dataclass(frozen=True)
class Suite:
    name: str
    source: str
    representative: str
    macro: str
    cases: dict[int, tuple[str, str]]


SUITES = (
    Suite(
        name="UniquePtr",
        source="modules/foundation/base/tests/pointer_compile_failures.cpp",
        representative="foundation/base/src/version.cpp",
        macro="LUDUS_POINTER_CASE",
        cases={
            1: ("incomplete default deletion", r"DefaultDeleter requires a complete type"),
            2: ("throwing deleter", r"constraints not satisfied|does not satisfy 'Deleter'"),
            3: ("non-virtual base conversion", r"no matching (constructor|conversion)|no viable conversion"),
            4: ("stateful bare release", r"Release.*(constraints not satisfied|no matching member)|"
                                         r"invalid reference to function 'Release'"),
            5: ("array element type", r"constraints not satisfied|is_array_v"),
        },
    ),
    Suite(
        name="SharedPtr/WeakPtr",
        source="modules/foundation/memory/tests/shared_ptr_compile_failures.cpp",
        representative="foundation/memory/src/shared_controller.cpp",
        macro="LUDUS_SHARED_POINTER_CASE",
        cases={
            1: ("raw pointer adoption", r"no matching (constructor|conversion)|no viable conversion"),
            2: ("raw pointer reset", r"no matching member function for call to 'Reset'|"
                                     r"too many arguments to function call"),
            3: ("array owner", r"SharedPtr does not own arrays"),
            4: ("array factory", r"no matching function for call to 'TryMakeShared'|constraints not satisfied"),
            5: ("unchecked downcast", r"no matching (constructor|conversion)|no viable conversion"),
            6: ("expired weak adjustment", r"no matching (constructor|conversion)|no viable conversion"),
            7: ("weak access without Lock", r"no member named 'Get'"),
        },
    ),
)


def representative_entry(entries, suffix: str):
    for entry in entries:
        if entry["file"].endswith(suffix) and not is_pch_entry(entry):
            return entry
    return None


def compile_case(options: list[str], directory: str, source: Path, macro: str, case: int | None):
    define = [] if case is None else [f"-D{macro}={case}"]
    return subprocess.run(options + define + ["-x", "c++", "-fsyntax-only", str(source)],
                          text=True, capture_output=True, cwd=directory)


def verify_suite(suite: Suite, entries, root: Path) -> list[str]:
    entry = representative_entry(entries, suite.representative)
    if entry is None:
        return [f"{suite.name}: {suite.representative} is missing from compile_commands.json"]
    source = root / suite.source
    options = compiler_options(entry)

    control = compile_case(options, entry["directory"], source, suite.macro, None)
    if control.returncode != 0:
        return [f"{suite.name}: control compilation of {suite.source} failed; the replayed flags are invalid:\n"
                f"{control.stderr}"]

    failures: list[str] = []
    for case, (name, pattern) in suite.cases.items():
        result = compile_case(options, entry["directory"], source, suite.macro, case)
        output = result.stdout + result.stderr
        if result.returncode == 0:
            failures.append(f"{suite.name} case {case} ({name}) compiled but must be rejected")
        elif re.search(pattern, output) is None:
            failures.append(f"{suite.name} case {case} ({name}) failed without the expected diagnostic "
                            f"/{pattern}/:\n{output}")
    return failures


def main() -> int:
    build = Path(sys.argv[1]).resolve()
    entries = json.loads((build / "compile_commands.json").read_text())
    root = Path(__file__).resolve().parents[2]

    failures: list[str] = []
    for suite in SUITES:
        failures.extend(verify_suite(suite, entries, root))

    if failures:
        print("Owner negative compilation FAILED:\n")
        for failure in failures:
            print(failure + "\n")
        return 1
    for suite in SUITES:
        print(f"{suite.name} negative compilation OK: {len(suite.cases)} unsafe uses rejected with expected "
              "diagnostics.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
