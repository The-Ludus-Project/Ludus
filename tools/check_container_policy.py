#!/usr/bin/env python3
"""Reject first-party std::vector; owning sequences use Ludus Array.

Policy owner: docs/architecture/containers.md section 34.3, tracked as milestone
M1 in docs/architecture/container-systems.md. std::vector is prohibited in all
first-party code, engine sources and tests alike. The only exceptions are the
benchmark comparison baselines and the private Crashpad ABI adapter listed in
EXCEPTIONS, and third-party/vendor or
generated trees, which are never scanned.

The scan is lexical: comments are removed and string/character literals are
masked, so documentation that names the type is not a finding. An exception
that no longer uses std::vector is reported as stale so the list stays exact.

Exit code 0 = clean, 1 = violations found (file:line and the rule).

Usage: check_container_policy.py [repo_root]
"""

import re
import sys
from pathlib import Path

ROOTS = ("modules", "apps", "examples", "tools", "tests")
EXTENSIONS = {".h", ".hh", ".hpp", ".hxx", ".c", ".cc", ".cpp", ".cxx", ".mm", ".inl"}
# Vendored, generated and build-output trees are not first-party sources.
SKIPPED_PARTS = {"out", "build", "third_party", "vendor", "generated", "node_modules"}

# Repository-relative path -> reason. Keep this list exact; a stale entry fails.
EXCEPTIONS = {
    "apps/editor/src/crash_capture_macos.mm": (
        "private macOS Crashpad ABI adapter; vectors are API arguments/results only, as scoped in ADR 0003"
    ),
    "modules/foundation/containers/benchmarks/array_bench.cpp": (
        "benchmark baseline that measures Ludus Array/StaticArray against the standard containers"
    ),
}

TOKENS = re.compile(
    r'(?P<string>R"(?P<delimiter>[^()\\\s]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\\n])*"'
    r"|'(?:\\.|[^'\\\n])*')"
    r"|(?P<comment>/\*.*?\*/|//[^\n]*)",
    re.DOTALL,
)
VECTOR = re.compile(r"\bstd\s*::\s*(?:pmr\s*::\s*)?vector\b")
INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<vector>", re.MULTILINE)


def mask(source: str) -> str:
    """Blank comments and literals while preserving offsets and newlines.

    Include directives keep their <...> spelling; quoted includes are masked,
    which is acceptable because <vector> is the only spelling checked.
    """

    def blank(match: re.Match) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    return TOKENS.sub(blank, source)


def findings(relative: str, source: str) -> list[str]:
    """Return policy findings for one first-party source."""
    code = mask(source)
    result = []
    for pattern, rule in (
        (INCLUDE, "#include <vector> is prohibited; include ludus/foundation/containers/array.hpp"),
        (VECTOR, "std::vector is prohibited; use ludus::foundation::Array (or StaticArray)"),
    ):
        for match in pattern.finditer(code):
            line = code.count("\n", 0, match.start()) + 1
            result.append(f"{relative}:{line}: {rule}")
    return result


def sources(root: Path):
    for directory in ROOTS:
        base = root / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in EXTENSIONS or not path.is_file():
                continue
            relative = path.relative_to(root)
            if SKIPPED_PARTS.intersection(relative.parts):
                continue
            yield relative.as_posix(), path


def violations(root: Path) -> list[str]:
    result = []
    used_exceptions = set()
    for relative, path in sources(root):
        found = findings(relative, path.read_text(encoding="utf-8", errors="replace"))
        if relative in EXCEPTIONS:
            if found:
                used_exceptions.add(relative)
            continue
        result.extend(found)
    for relative in sorted(set(EXCEPTIONS) - used_exceptions):
        result.append(f"{relative}: stale container-policy exception; remove it from EXCEPTIONS")
    return result


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    problems = violations(root)
    for problem in problems:
        print(problem)
    if problems:
        print(f"Container policy: {len(problems)} violation(s); see docs/architecture/containers.md section 34.3.")
        return 1
    print("Container policy OK: no first-party std::vector outside the documented exceptions.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
