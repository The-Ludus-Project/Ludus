#!/usr/bin/env python3
"""Reject raw target/compiler macro tests outside FoundationBase detection.

Policy owner: docs/architecture/platform-detection.md. Target and compiler-frontend
classification belongs to Band 0 (`ludus/foundation/base/config.h` and
`compiler.h`). Engine and Editor sources test `LUDUS_TARGET_OS`,
`LUDUS_TARGET_ARCH`, `LUDUS_TARGET_COMPILER` and the derived `LUDUS_*` flags
instead of compiler-predefined macros such as `__linux__` or `_MSC_VER`.

The scan is lexical and needs no compiler, so the pull-request Source formatting
job runs it on every change; `ludus_target_detection` runs the same scan after
its compile cases. Only conditional directives are checked: line continuations
are joined first, so a directive split across lines is still found, and a macro
named in a comment or string outside a directive is not a finding.

Exit code 0 = clean, 1 = violations found (path:line and the macro).

Usage: check_target_macros.py [repo_root]
"""

import re
import sys
from pathlib import Path

ROOTS = ("modules", "apps")
EXTENSIONS = {".cpp", ".h", ".hpp", ".mm"}
DETECTORS = {
    "modules/foundation/base/include/ludus/foundation/base/config.h",
    "modules/foundation/base/include/ludus/foundation/base/compiler.h",
}
RAW_MACROS = (
    "_WIN32", "_WIN64", "__linux__", "__APPLE__", "__ANDROID__", "__EMSCRIPTEN__",
    "__clang__", "__GNUC__", "_MSC_VER", "__x86_64__", "__aarch64__", "__wasm32__",
    "__wasm64__", "_M_X64", "_M_ARM64", "_M_ARM64EC",
)
RAW_IDENTITY = re.compile(
    r"^[ \t]*#[ \t]*(?:if|elif|ifdef|ifndef)\b[^\n]*?\b(" + "|".join(RAW_MACROS) + r")\b",
    re.MULTILINE,
)
CONTINUATION = re.compile(r"\\\r?\n")


def raw_identity_lines(source: str) -> list[tuple[int, str]]:
    """Return (1-based line, macro) for each conditional directive testing a raw macro."""
    findings = []
    # Join continuations without losing the physical line of each directive.
    joined = CONTINUATION.sub(lambda match: " " + "\x00" * match.group(0).count("\n"), source)
    for match in RAW_IDENTITY.finditer(joined):
        line = 1 + joined.count("\n", 0, match.start()) + joined.count("\x00", 0, match.start())
        findings.append((line, match.group(1)))
    return findings


def is_scanned(relative: Path) -> bool:
    return (
        relative.suffix in EXTENSIONS
        and "tests" not in relative.parts
        and relative.as_posix() not in DETECTORS
    )


def violations(root: Path) -> list[str]:
    failures = []
    for tree in ROOTS:
        for path in sorted((root / tree).rglob("*")):
            relative = path.relative_to(root)
            if not path.is_file() or not is_scanned(relative):
                continue
            source = path.read_text(encoding="utf-8")
            for line, macro in raw_identity_lines(source):
                failures.append(
                    f"{relative.as_posix()}:{line}: tests {macro}; use FoundationBase detection "
                    "(LUDUS_TARGET_OS, LUDUS_TARGET_ARCH, LUDUS_TARGET_COMPILER) instead of raw target/compiler macros"
                )
    return failures


def main() -> int:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]).resolve()
    failures = violations(root)
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print("Target macros: no raw target/compiler macro tests outside FoundationBase detection")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
