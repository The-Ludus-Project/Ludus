"""Review-derived source rules; changed-code ratchet, without a debt baseline."""

from __future__ import annotations

import argparse
import difflib
import re
import subprocess
from pathlib import Path

from formatting import TOKENS, format_source
from naming_rules import (
    abbreviation_findings,
    pointer_findings,
    reference_and_boolean_findings,
)

CPP = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".mm"}
ROOTS = {"modules", "apps", "examples", "tests", "tools", "cmake"}
BINDING_BOUNDARY = "modules/graphics/rhi/shaders/raster_bindings.slang"
SCENE_SHADER_INTERFACE = "modules/graphics/renderer/shaders/scene_shader.slang"
LEGACY_SCENE_ALIASES = r"\bSCENE_QUERY_(?:MISS|SURFACE|FAILURE)\b|\bSCENE_LIGHT_(?:DIRECTIONAL|POINT|SPOT|MESH|ENVIRONMENT)\b"


# Thanks to Kitware and CMake contributors, "cmake-language(7)", Syntax:
# https://cmake.org/cmake/help/latest/manual/cmake-language.7.html#syntax
# Use its argument/comment forms for masking; this guard does not evaluate CMake.
def cmake_code_only(source: str) -> str:
    """Keep offsets while hiding CMake comments, literals and opaque expansions."""
    literals = re.compile(
        r'#?\[(?P<equals>=*)\[[\s\S]*?\](?P=equals)\]'
        r'|"(?:\\[\s\S]|[^"\\])*"|#[^\n]*|\\[\s\S]'
    )
    code = literals.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), source)
    variables = re.compile(r"\$(?:ENV|CACHE)?\{[^{}]*\}")
    while variables.search(code):
        code = variables.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), code)
    depth = 0
    beginning = 0
    pieces = list(code)
    for match in re.finditer(r"\$<|>", code):
        if match.group() == "$<":
            if depth == 0:
                beginning = match.start()
            depth += 1
        elif depth > 0:
            depth -= 1
            if depth == 0:
                for index in range(beginning, match.end()):
                    if pieces[index] != "\n":
                        pieces[index] = " "
    return "".join(pieces)


def nested_cmake_build_commands(code: str) -> list[tuple[int, int, int, str, str]]:
    """Find direct build commands embedded in source lists, without evaluating CMake."""
    found = []
    for outer in re.finditer(r"\b(add_executable|add_library|target_sources)\s*\(", code, re.I):
        depth = 1
        ending = len(code)
        for parenthesis in re.finditer(r"[()]", code[outer.end():]):
            depth += 1 if parenthesis.group() == "(" else -1
            if depth == 0:
                ending = outer.end() + parenthesis.start()
                break
        for inner in re.finditer(
            r"\b((?:add|target|ludus)_\w+|set_(?:target|source_files|tests)_properties)\s*\(",
            code[outer.end():ending], re.I,
        ):
            found.append((outer.start(), ending, outer.end() + inner.start(),
                          outer.group(1), inner.group(1)))
    return found


def code_only(source: str) -> str:
    """Preserve offsets/newlines while removing comments, literals and directives."""
    pieces = []
    end = 0
    for match in TOKENS.finditer(source):
        token = match.group()
        pieces.append(source[end : match.start()])
        if (
            token.startswith(("//", "/*"))
            or token.lstrip().startswith("#")
            or '"' in token
            or "'" in token
        ):
            pieces.append(re.sub(r"[^\n]", " ", token))
        else:
            pieces.append(token)
        end = match.end()
    pieces.append(source[end:])
    return "".join(pieces)


NUMBER = re.compile(
    r"(?<![\w.'])(?:0[xX][0-9A-Fa-f']+|0[bB][01']+|(?:\d[\d']*)?\.?\d[\d']*(?:[eE][+-]?\d+)?)"
    r"[uUlLfFzZ]*(?![\w.'])"
)
# Values that need no name: identity/zero, halving/doubling and their float spellings.
# clang-tidy readability-magic-numbers applies the same idea with its ignore lists.
PERMITTED_NUMBERS = {0.0, 1.0, 2.0, 0.5}
MAGIC_NUMBER_WAIVER = re.compile(r"// ludus-style: magic-number .{12,}")
ABBREVIATION_WAIVER = re.compile(r"// ludus-style: abbreviation .{12,}")
NAMED_CONSTANT = re.compile(
    r"\b(?:constexpr|constinit|const)\b[^;{}()]*?[A-Za-z_]\w*\s*(?:\[[^\]]*\]\s*)?(?:=|\{)[^;]*$"
)


def numeric_value(text: str) -> float | None:
    literal = text.replace("'", "").rstrip("uUlLfFzZ")
    try:
        if re.match(r"0[xXbB]", literal) or re.fullmatch(r"0\d+", literal):
            return float(int(literal, 0 if literal[1:2].isalpha() else 8))
        return float(literal)
    except ValueError:
        return None


def is_test_source(path: str) -> bool:
    parts = Path(path).parts
    name = Path(path).stem
    return (
        "tests" in parts
        or "benchmarks" in parts
        or name.endswith(("_tests", "_test", "_bench", "_oracle"))
        or path.startswith("tools/")
    )


BLOCK_KEYWORDS = {"else", "do", "try", "struct", "class", "union", "namespace", "enum", "extern"}


def is_braced_initializer(code: str, opening: int) -> bool:
    """Whether the `{` at opening starts an initializer rather than a block."""
    before = code[:opening].rstrip()
    if before.endswith(("=", ",", "(", "{", ">")):
        return True
    word = re.search(r"(\w+)$", before)
    return word is not None and word.group(1) not in BLOCK_KEYWORDS


def statement_begin(code: str, offset: int) -> int:
    """Start of the statement containing offset; braced initializers stay inside."""
    index = offset - 1
    while index >= 0:
        character = code[index]
        if character == ";":
            return index + 1
        if character == "{":
            if is_braced_initializer(code, index) is False:
                return index + 1
            index -= 1
            continue
        if character == "}":
            depth = 0
            opening = index
            while opening >= 0:
                depth += (code[opening] == "}") - (code[opening] == "{")
                if depth == 0:
                    break
                opening -= 1
            if opening < 0 or is_braced_initializer(code, opening) is False:
                return index + 1
            index = opening - 1
            continue
        index -= 1
    return 0


def magic_numbers(code: str) -> list[int]:
    """Offsets of unnamed numeric literals (AGENTS.md "No magic numbers").

    Exempt: 0, 1, 2 and 0.5 (any spelling), named-constant initializers
    (`constexpr`/`const` declarations), enumerators and static_assert. This is a
    lexical changed-line ratchet; whether a name explains a value stays a review
    question. Template-argument sizes are deliberately reported.
    """
    offsets = []
    for match in NUMBER.finditer(code):
        value = numeric_value(match.group())
        if value is None or value in PERMITTED_NUMBERS:
            continue
        statement = code[statement_begin(code, match.start()) : match.start()]
        # A brace-initialized constant's own `{` opens its initializer.
        if NAMED_CONSTANT.search(statement.replace("\n", " ")):
            continue
        if re.search(r"\bstatic_assert\s*\(", statement):
            continue
        depth = 0
        index = match.start() - 1
        enclosing = -1
        while index >= 0:
            character = code[index]
            if character == "}":
                depth += 1
            elif character == "{":
                if depth == 0:
                    enclosing = index
                    break
                depth -= 1
            index -= 1
        if enclosing >= 0:
            head_start = max(code.rfind(";", 0, enclosing), code.rfind("}", 0, enclosing), code.rfind("{", 0, enclosing))
            if re.search(r"\benum\b", code[head_start + 1 : enclosing]):
                continue
        offsets.append(match.start())
    return offsets


def added_lines(before: str, after: str) -> set[int]:
    lines = set()
    for operation, _, _, start, end in difflib.SequenceMatcher(
        None, before.splitlines(), after.splitlines(), autojunk=False
    ).get_opcodes():
        if operation in {"insert", "replace"}:
            lines.update(range(start + 1, end + 1))
    return lines


def declaration_commas(code: str) -> list[int]:
    """Recognize ordinary declarations and their top-level declarator commas.

    Nested calls, lists, templates and loop increment expressions are excluded.
    This is a lexical ratchet, not a C++/Slang parser; unusual declarators still
    require review. Offsets refer to code_only's comment/literal-masked input.
    """
    commas = []
    pattern = re.compile(
        r"(?:^|(?<=[;{}])|\bfor\s*\()\s*"
        r"(?:(?:const|static|constexpr|inline|volatile)\s+)*"
        # A namespace block has no terminating semicolon, so the scan would run
        # past its closing brace into later template parameter lists.
        r"(?!return\b|else\b|case\b|using\b|namespace\b)"
        r"[A-Za-z_]\w*(?:::\w+)*(?:<[^;{}]*?>)?(?:\s+|(?=[*&]))"
        r"[*&]*\s*[A-Za-z_]\w*\s*(?=[=,;\[{])",
    )
    for declaration in pattern.finditer(code):
        stack = []
        for index in range(declaration.end(), len(code)):
            token = code[index]
            if token in "([{":
                stack.append(token)
            elif token in ")]}":
                if not stack:
                    break
                stack.pop()
            elif token == "<" and index and code[index - 1].isalnum():
                # Template calls such as Make<Pair<int,int>>(); comparison
                # expressions normally have whitespace or no closing angle.
                if re.match(r"[^;{}]*>", code[index + 1 :]):
                    stack.append(token)
            elif token == ">" and stack and stack[-1] == "<":
                stack.pop()
            elif not stack and token == ";":
                break
            elif not stack and token == ",":
                commas.append(index)
    return sorted(set(commas))


# Directories whose implementations may include <thread> or <chrono> (ADR 0003).
STANDARD_THREAD_CLOCK_OWNERS = (
    "modules/foundation/time/src/",
    "modules/foundation/logging/src/",
    "apps/editor/",
)


def violations(path: str, source: str, changed: set[int]) -> list[tuple[int, str]]:
    """Check lexical conventions, not semantic coordinate correctness or C++ types."""
    is_cmake = Path(path).suffix == ".cmake" or Path(path).name == "CMakeLists.txt"
    code = cmake_code_only(source) if is_cmake else code_only(source)
    found = []

    def report(offset: int, message: str) -> None:
        line = code.count("\n", 0, offset) + 1
        if line in changed:
            found.append((line, message))

    if is_cmake:
        for opening, ending, nested, outer_name, inner_name in nested_cmake_build_commands(code):
            first_line = code.count("\n", 0, opening) + 1
            last_line = code.count("\n", 0, ending) + 1
            if changed.intersection(range(first_line, last_line + 1)):
                found.append((code.count("\n", 0, nested) + 1,
                              f"close {outer_name} before invoking {inner_name}; "
                              "build commands cannot be nested in a source list"))
        for match in re.finditer(
            r"\bludus_compile_shader\s*\([^)]*?\bNAME\s+(renderer_\w+)", code
        ):
            report(
                match.start(1),
                "select built-in renderer recipes through ludus_compile_renderer_shaders",
            )
        return found

    # Recognizable copies of FoundationMath PI are forbidden outside its owner.
    # Other numeric constants need semantic review; tolerances are not blindly banned.
    if Path(path).suffix == ".slang" and path != "modules/foundation/math/shaders/math.slang":
        for match in re.finditer(r"(?<![\w.])3\.14159\d*(?:[fF])?(?![\w.])", code):
            report(match.start(), "reuse FoundationMath PI instead of a duplicated pi literal")

    for offset in declaration_commas(code):
        report(
            offset,
            "declare one variable per statement; comma-separated declarations are forbidden",
        )

    if Path(path).suffix in CPP:
        # <thread> and <chrono> pull <format> into every including translation unit on
        # libstdc++ (ADR 0003, docs/development/build-profiling.md). Their approved owners
        # are the time and logging implementations and the assertion death-test child,
        # which cannot link FoundationBase; the editor follows its own Qt rules.
        if (
            path.startswith(STANDARD_THREAD_CLOCK_OWNERS) is False
            and path != "modules/foundation/base/tests/assert_child.cpp"
        ):
            # code_only blanks directives, so read them from the token stream; an include
            # spelled inside a comment or literal belongs to that token and is not matched.
            for token in TOKENS.finditer(source):
                include = re.match(r"[ \t]*#[ \t]*include[ \t]*<(thread|chrono)>", token.group())
                if include is not None:
                    report(
                        token.start(),
                        "use <ludus/foundation/threading/thread.hpp> and FoundationTime "
                        f"instead of <{include.group(1)}>",
                    )
        # A narrow changed-line guard for the consolidated admission paths.
        # Other spellings, derived expressions and unrelated policies still need review.
        if (
            path.startswith("modules/content/scene_cook/src/")
            or path in {"modules/content/src/files.cpp", "apps/editor/src/render_scene.cpp"}
        ):
            suffix = r"[uUlL]*"
            integer = r"(?:usize\s*\{\s*)?{number}" + suffix + r"\s*\}?"
            kibibyte = integer.replace("{number}", "1024")
            mebibytes = integer.replace("{number}", "(?:1|4|8|16|32|64)")
            patterns = (
                r"(?<![\w.])(?:500000|1048576|4194304|8388608|16777216|33554432|67108864)" + suffix + r"(?![\w.])",
                r"(?<![\w.])" + mebibytes + r"\s*\*\s*" + kibibyte + r"\s*\*\s*1024" + suffix + r"(?![\w.])",
                r"(?<![\w.])" + kibibyte + r"\s*\*\s*1024" + suffix + r"(?![\w.])",
                r"(?<![\w.])" + mebibytes + r"\s*<<\s*20" + suffix + r"(?![\w.])",
            )
            for match in re.finditer("|".join(patterns), code):
                # Do not mistake the tail of an unrelated product (e.g. 256 MiB)
                # for an independently written 1 MiB policy.
                if code[:match.start()].rstrip().endswith("*"):
                    continue
                report(match.start(), "reuse the owning admission-limit constant instead of a duplicated capacity literal")
        # int64 is long on Linux and long long on macOS. QJsonValue has int,
        # qint64 and double constructors, so a long argument is ambiguous there
        # and only the Linux editor build fails. Pass JSON integers as qint64.
        # Only Qt sources can build a QJsonValue; elsewhere `append` and `insert`
        # are ordinary engine or local functions that take int64 directly.
        uses_qt_json = path.startswith("apps/editor/") or re.search(r"\bQJson\w*", code) is not None
        json_value = (r"(?:\bQJsonValue\s*\(|\{\s*QStringLiteral\s*\([^()]*\)\s*,"
                      r"|\b(?:insert|append|contains)\s*\((?:\s*QStringLiteral\s*\([^()]*\)\s*,)?)")
        for match in re.finditer(json_value + r"\s*(static_cast<\s*(?:foundation::)?u?int64\s*>)",
                                 code if uses_qt_json else ""):
            report(match.start(1), "convert JSON integers with static_cast<qint64>; int64 is ambiguous for QJsonValue on Linux")
        for offset, _, message in pointer_findings(code):
            report(offset, message)
        for offset, _, message in reference_and_boolean_findings(code):
            report(offset, message)
        source_lines = source.splitlines()
        for offset, _, message in abbreviation_findings(code):
            line = code.count("\n", 0, offset)
            if line < len(source_lines) and ABBREVIATION_WAIVER.search(source_lines[line]):
                continue
            report(offset, message)
        if is_test_source(path) is False:
            for offset in magic_numbers(code):
                line = code.count("\n", 0, offset)
                if line < len(source_lines) and MAGIC_NUMBER_WAIVER.search(source_lines[line]):
                    continue
                report(
                    offset,
                    "name this numeric literal with a constant in its owner (0, 1, 2 and 0.5 need no name)",
                )
        for match in re.finditer(r"!(?!=)|\bnot\b", code):
            report(
                match.start(),
                "use an explicit comparison instead of logical negation (== false for booleans)",
            )
        for match in re.finditer(
            r"\bauto(?:\s+(?:const|volatile))*\s*[*&]*\s+(\w+|\[[^\]]+\])\s*(=|:|\(|\{)",
            code,
        ):
            # Lambda closure types cannot be spelled; generic parameters and
            # trailing return types are not variable declarations matched here.
            tail = code[match.end() :].lstrip()
            if match[2] == "(":
                depth, end = 1, match.end()
                while depth and end < len(code):
                    depth += (code[end] == "(") - (code[end] == ")")
                    end += 1
                following = code[end:].lstrip()
                if following.startswith(("->", "noexcept", "{")):
                    continue  # Function return deduction is not a variable.
            if match[2] in {"=", "{"} and tail.startswith("["):
                continue
            line = code.count("\n", 0, match.start())
            original = source.splitlines()[line]
            if re.search(
                r"// ludus-style: inferred-type: (iterator|dependent) .{12,}", original
            ):
                continue
            report(
                match.start(),
                "spell the variable type; auto requires an unspellable lambda or justified iterator/dependent type",
            )
        return found
    if not path.endswith(".slang"):
        return found
    if path != BINDING_BOUNDARY:
        # Inspect original annotation tokens, since code_only hides #defines.
        for match in re.finditer(r"\[\[\s*vk::(?:binding|location)\b", code):
            report(
                match.start(),
                "use the RHI shader interface macros, not target annotation vocabulary",
            )
    for declaration in re.finditer(r"\b(?:struct|cbuffer)\s+(\w+)", code):
        if not re.fullmatch(r"[A-Z][A-Za-z0-9]*", declaration[1]):
            report(
                declaration.start(1),
                "shader types and constant-buffer blocks use PascalCase",
            )
    # Closed sets are scoped Slang enums with an explicit width; whether a given
    # set of uint constants is closed remains a review decision.
    for declaration in re.finditer(r"\benum\s+(?:class\s+)?(\w+)\s*(:\s*\w+)?\s*\{([^{}]*)\}", code):
        if not re.fullmatch(r"[A-Z][A-Za-z0-9]*", declaration[1]):
            report(declaration.start(1), "shader types and constant-buffer blocks use PascalCase")
        if declaration[2] is None:
            report(declaration.start(1), "give shader enums an explicit underlying type (enum Name : uint)")
        for enumerator in re.finditer(r"(?:^|,)\s*(\w+)", declaration[3]):
            if not re.fullmatch(r"[A-Z](?:[A-Za-z0-9]*[a-z][A-Za-z0-9]*)?", enumerator[1]):
                report(declaration.start(3) + enumerator.start(1), "shader enumerators use PascalCase")
    if path != SCENE_SHADER_INTERFACE:
        for match in re.finditer(LEGACY_SCENE_ALIASES, code):
            report(match.start(), "name the SceneQuery or SceneLightKind enumerator, not its interface v1 alias")
    for match in re.finditer(
        r"\b(?:struct|cbuffer)\s+\w+\s*\{([^{}]*)\}", code, re.DOTALL
    ):
        for field in re.finditer(
            r"\b(?:column_major\s+)?\w+(?:<[^;]+>)?\s+(\w+)\s*(?:[:;\[=,])", match[1]
        ):
            offset = match.start(1) + field.start(1)
            name = field[1]
            if not re.fullmatch(r"[A-Z][A-Za-z0-9]*", name):
                report(offset, "shader members use PascalCase")
            if re.search(
                r"Position|Normal|Direction|Eye|Bounds", name
            ) and not re.search(r"Mesh|World|View|Clip|Ndc|Pixel|Uv|Tangent", name):
                report(
                    offset,
                    "coordinate-bearing members name their space (World, View, Clip, Pixel, etc.)",
                )
    for match in re.finditer(
        r"\b(\w+)\s+(\w+)\s*\(([^{};]*)\)\s*(?::\s*\w+)?\s*\{", code
    ):
        if match[1] in {"if", "else", "for", "while", "switch", "return"}:
            continue
        if not re.fullmatch(r"[A-Z][A-Za-z0-9]*", match[2]):
            report(match.start(2), "shader functions and entry points use PascalCase")
        for parameter in re.finditer(
            r"\b(?:in\s+|inout\s+|out\s+)?\w+\s+(\w+)\s*(?:[:,=]|$)", match[3]
        ):
            if not re.fullmatch(r"[a-z][A-Za-z0-9]*", parameter[1]):
                report(
                    match.start(3) + parameter.start(1),
                    "shader parameters use lowerCamelCase",
                )
        for parameter in re.finditer(r"\bout\s+\w+\s+(\w+)", match[3]):
            if not re.fullmatch(r"out[A-Z][A-Za-z0-9]*", parameter[1]):
                report(
                    match.start(3) + parameter.start(1),
                    "out parameters use outPascalCase",
                )
    return found


def check(
    root: Path, base: str, clang_format: str | None = None, head: str | None = None
) -> list[str]:
    """Inspect a PR head tree, or local edits; never attribute synthetic-merge main changes to a PR."""
    if head:
        base = subprocess.run(
            ["git", "merge-base", base, head],
            cwd=root,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.strip()
    comparison = [base, head] if head else [base]
    names = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=ACMR", *comparison],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.splitlines()
    if not head:
        names += subprocess.run(
            ["git", "ls-files", "--others", "--exclude-standard"],
            cwd=root,
            text=True,
            capture_output=True,
            check=True,
        ).stdout.splitlines()
    failures = []
    for name in sorted(set(names)):
        path = Path(name)
        is_cmake = path.suffix == ".cmake" or path.name == "CMakeLists.txt"
        if (
            not path.parts
            or (path.parts[0] not in ROOTS and name != "CMakeLists.txt")
            or (path.suffix not in CPP | {".slang"} and not is_cmake)
        ):
            continue
        source = (
            subprocess.run(
                ["git", "show", f"{head}:{name}"],
                cwd=root,
                text=True,
                capture_output=True,
                check=True,
            ).stdout
            if head
            else (root / path).read_text(encoding="utf-8")
        )
        if (
            clang_format
            and not is_cmake
            and format_source(source, (root / path).resolve(), clang_format) != source
        ):
            failures.append(
                f"{name}: source formatting differs; use the pinned formatter and formatting.format_source"
            )
        previous = subprocess.run(
            ["git", "show", f"{base}:{name}"], cwd=root, text=True, capture_output=True
        )
        before = previous.stdout if previous.returncode == 0 else ""
        for line, message in violations(name, source, added_lines(before, source)):
            failures.append(f"{name}:{line}: {message}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--base", required=True, help="PR base commit, or explicit local comparison ref"
    )
    parser.add_argument(
        "--clang-format",
        help="also verify formatting of every changed C++/Slang source",
    )
    parser.add_argument(
        "--head",
        help="inspect this exact PR head instead of the checked-out synthetic merge",
    )
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    failures = check(root, arguments.base, arguments.clang_format, arguments.head)
    for failure in failures:
        print(failure)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
