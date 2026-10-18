#!/usr/bin/env python3
"""Enforce the mechanically checkable Ludus naming and boolean rules.

Rule N1 applies to every first-party C++ source under modules/, apps/,
tools/, tests/ and examples/ (the legacy backlog was converted in one pass).
N2-N4 are checked only in the opt-in scopes listed in config/naming_scopes.txt
so existing code is migrated when it is touched rather than all at once;
scripts/python/source_policy.py applies N2-N5 to changed lines everywhere.

  N1  Use `condition == false` instead of `!condition` (or `not`); compare
      pointers with nullptr and integers with 0. Preprocessor directives such
      as `#if !defined(X)` and embedded JavaScript (EM_JS bodies) are exempt.
  N2  Non-const lvalue reference parameters start with `out` or `inout`.
      Rvalue reference parameters omit these prefixes.
  N3  Boolean variables, members and parameters read as questions
      (is/has/should/can/was/are/does/did prefix, optional `m` member prefix).
  N4  Ordinary pointer declarations explicitly initialized to null end in `OrNull`.

Private member functions in camelCase are enforced by clang-tidy
(readability-identifier-naming in the scoped .clang-tidy files). Rules that need
type or intent information (bare `condition == true`, verb-first names, no
abbreviations, out vs inout choice, non-defaulted nullable pointers) are
review-only.

The scan is textual on clang-formatted code: comments, string and character
literals are blanked first. Exit code 0 = clean, 1 = violations.

Usage: check_naming.py [repo_root]
"""

import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts/python"))
from naming_rules import pointer_findings, reference_and_boolean_findings  # noqa: E402
from source_policy import code_only  # noqa: E402

SOURCE_SUFFIXES = {".h", ".hpp", ".cpp", ".cc", ".mm"}
NEGATION_ROOTS = ("modules", "apps", "tools", "tests", "examples")
SKIPPED_DIRECTORIES = {"third_party", "out", "build", "node_modules"}
# Export sources of the local catch2 Conan recipe: editing them changes the
# recipe revision that conan.lock pins, so they keep Catch2's own style.
NEGATION_EXEMPT_FILES = {"tests/support/catch_stream.hpp", "tests/support/catch_stream.cpp"}

NEGATION = re.compile(r"!(?!=)|\bnot\b")


def load_scopes(root: Path) -> list[Path]:
    scopes = []
    config = root / "config" / "naming_scopes.txt"
    for line in config.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            scopes.append(root / line)
    return scopes


def scoped_files(root: Path) -> list[Path]:
    files = set()
    for scope in load_scopes(root):
        if scope.is_file():
            files.add(scope)
        elif scope.is_dir():
            files.update(p for p in scope.rglob("*") if p.suffix in SOURCE_SUFFIXES)
        else:
            raise SystemExit(f"naming scope does not exist: {scope.relative_to(root)}")
    return sorted(files)


EMBEDDED_JAVASCRIPT = re.compile(r"\bEM_(?:ASYNC_)?JS\s*\(")


def blank_embedded_javascript(code: str) -> str:
    """Blank Emscripten EM_JS/EM_ASYNC_JS bodies: they are JavaScript, not C++."""
    pieces = []
    end = 0
    for match in EMBEDDED_JAVASCRIPT.finditer(code):
        if match.start() < end:
            continue
        depth = 0
        index = match.end() - 1
        while index < len(code):
            depth += (code[index] == "(") - (code[index] == ")")
            if depth == 0:
                break
            index += 1
        pieces.append(code[end:match.start()])
        pieces.append(re.sub(r"[^\n]", " ", code[match.start():index + 1]))
        end = index + 1
    pieces.append(code[end:])
    return "".join(pieces)


def negation_files(root: Path) -> list[Path]:
    files = []
    for name in NEGATION_ROOTS:
        base = root / name
        if base.is_dir() is False:
            continue
        for path in base.rglob("*"):
            relative = path.relative_to(root).parts
            if (
                path.suffix in SOURCE_SUFFIXES
                and SKIPPED_DIRECTORIES.isdisjoint(relative)
                and "/".join(relative) not in NEGATION_EXEMPT_FILES
                and path.is_file()
            ):
                files.append(path)
    return sorted(files)


def check_negation(root: Path, path: Path, text: str | None = None) -> list[str]:
    """N1 only: used for every first-party source."""
    if text is None:
        text = blank_embedded_javascript(code_only(path.read_text(encoding="utf-8", errors="replace")))
    findings = []
    for match in NEGATION.finditer(text):
        if re.search(r"\boperator\s*$", text[:match.start()]):
            continue  # declaring or naming operator! itself
        line = text.count("\n", 0, match.start()) + 1
        findings.append(
            f"{path.relative_to(root)}:{line}: [N1] write `condition == false` instead of `!condition` "
            "(`pointer == nullptr`, `count == 0` for non-booleans)"
        )
    return findings


def check_file(root: Path, path: Path) -> list[str]:
    text = blank_embedded_javascript(code_only(path.read_text(encoding="utf-8", errors="replace")))
    findings = check_negation(root, path, text)

    def report(offset: int, rule: str, message: str) -> None:
        line = text.count("\n", 0, offset) + 1
        findings.append(f"{path.relative_to(root)}:{line}: [{rule}] {message}")

    for offset, rule, message in reference_and_boolean_findings(text):
        report(offset, rule, message)
    for offset, rule, message in pointer_findings(text):
        report(offset, rule, message)
    return findings


def main() -> int:
    root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
    findings = []
    files = scoped_files(root)
    scoped = set(files)
    for path in files:
        findings.extend(check_file(root, path))
    everywhere = [path for path in negation_files(root) if path not in scoped]
    for path in everywhere:
        findings.extend(check_negation(root, path))
    for finding in findings:
        print(finding)
    if findings:
        print(f"Naming check: {len(findings)} violation(s). See AGENTS.md \"Naming\".")
        return 1
    print(f"Naming check OK ({len(files)} scoped files; N1 in {len(files) + len(everywhere)} files).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
