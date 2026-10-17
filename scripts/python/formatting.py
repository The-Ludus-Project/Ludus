"""Clang-format 18 plus Ludus's designated-initializer brace convention."""
from __future__ import annotations

import re
import subprocess
from pathlib import Path


# Recognize literals and comments before punctuation so their braces are opaque.
TOKENS = re.compile(
    r'^[ \t]*\#(?:[^\n]*\\\n)*[^\n]*'
    r'|(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|//[^\n]*|/\*.*?\*/'
    r'|(?:u8|u|U|L)?"(?:\\.|[^"\\])*"'
    r"|(?:u8|u|U|L)?'(?:\\.|[^'\\])*'"
    r"|[0-9][a-zA-Z0-9_'.]*|[a-zA-Z_][a-zA-Z0-9_]*|[^\s]",
    re.MULTILINE | re.DOTALL,
)


class FormattingError(RuntimeError):
    """Raised when formatting would change a source file's meaningful tokens."""


def significant_tokens(source: str) -> list[str]:
    """Return the token sequence, ignoring whitespace except inside literals."""
    tokens = []
    for match in TOKENS.finditer(source):
        token = match.group()
        # Reindentation may change whitespace in comments and directives only.
        if token.lstrip().startswith(("#", "//", "/*")):
            token = "".join(token.split())
        tokens.append(token)
    return tokens


def designated_lists(source: str) -> list[tuple[int, int, int]]:
    """Return opening/closing brace and last-token offsets outside disabled regions."""
    tokens = []
    enabled = True
    for match in TOKENS.finditer(source):
        token = match.group()
        if token.startswith(("//", "/*")):
            if "clang-format off" in token:
                enabled = False
            elif "clang-format on" in token:
                enabled = True
            continue
        if token.lstrip().startswith("#"):
            continue
        tokens.append((token, match.start(), match.end(), enabled))

    stack = []
    lists = []
    for index, (token, start, _, active) in enumerate(tokens):
        if token == "{":
            stack.append(index)
        elif token == "}" and stack:
            opening = stack.pop()
            members = tokens[opening + 1:index]
            if (len(members) >= 3 and members[0][0] == "."
                    and re.fullmatch(r"[a-zA-Z_][a-zA-Z0-9_]*", members[1][0])
                    and members[2][0] == "=" and active and tokens[opening][3]
                    and all(member[3] for member in members)):
                lists.append((tokens[opening][1], start, members[-1][2]))
    return lists


def add_multiline_commas(source: str) -> str:
    # Preserve compact lists; a trailing comma keeps multiline members separated.
    offsets = [last for opening, closing, last in designated_lists(source)
               if "\n" in source[opening:closing]]
    for last in sorted(offsets, reverse=True):
        if source[last - 1] != ",":
            source = source[:last] + "," + source[last:]
    return source


def format_source(source: str, filename: Path, clang_format: str) -> str:
    def clang(source: str) -> str:
        return subprocess.run(
            [clang_format, "--style=file", f"--assume-filename={filename}"],
            input=source, text=True, capture_output=True, check=True,
        ).stdout

    style_path = next(parent / ".clang-format" for parent in filename.parents
                      if (parent / ".clang-format").is_file())
    style = style_path.read_text(encoding="utf-8")
    column_limit = int(re.search(r"^ColumnLimit:\s*(\d+)", style, re.MULTILINE)[1])
    result = clang(add_multiline_commas(source))
    compact = [(opening, closing, last) for opening, closing, last in designated_lists(result)
               if "\n" not in result[opening:closing]]
    widths = {}
    for opening, closing, _ in compact:
        start = result.rfind("\n", 0, opening) + 1
        end = result.find("\n", closing)
        if end < 0:
            end = len(result)
        body = result[opening + 1:closing]
        widths[start] = widths.get(start, end - start) + 2 - len(body) + len(body.strip())
    for opening, _, last in sorted(compact, key=lambda item: item[2], reverse=True):
        start = result.rfind("\n", 0, opening) + 1
        if column_limit and widths[start] > column_limit and result[last - 1] != ",":
            result = result[:last] + "," + result[last:]
    expanded = add_multiline_commas(result)
    if expanded != result or any(column_limit and width > column_limit for width in widths.values()):
        # Lists that overflow the column limit become multiline on the first pass.
        result = clang(expanded)

    # Clang-format 18 cannot wrap initializer opening braces. Change only the
    # whitespace before those braces after it has formatted the member bodies.
    expected = significant_tokens(result)
    count = len(designated_lists(result))
    for index in range(count):
        # Recompute offsets after each outer list so nested lists inherit its
        # indentation, then normalize their own member bodies independently.
        opening, closing, _ = sorted(designated_lists(result))[index]
        body = result[opening + 1:closing]
        if "\n" not in body:
            result = result[:opening + 1] + " " + body.strip() + " " + result[closing:]
            continue
        if body[body.rfind("\n") + 1:].strip():
            # Clang-format keeps the closing brace after the last member in some
            # contexts, such as ternary operands. Keep its layout rather than
            # rewriting that line, which would drop the member.
            continue
        line_start = result.rfind("\n", 0, opening) + 1
        prefix = result[line_start:opening]
        indentation = prefix[:len(prefix) - len(prefix.lstrip())]
        if not prefix.strip():
            previous_start = result.rfind("\n", 0, line_start - 1) + 1
            previous = result[previous_start:line_start].rstrip()
            if previous.endswith("="):
                indentation = previous[:len(previous) - len(previous.lstrip())]

        member = re.search(r"\n([ \t]*)\.", body)
        if member:
            delta = len(indentation) + 4 - len(member[1])
            protected = set()
            for token in TOKENS.finditer(body):
                if ('\n' in token.group() and ('"' in token.group() or "'" in token.group())
                        and not token.group().startswith(("//", "/*"))):
                    protected.update(range(body.count("\n", 0, token.start()) + 1,
                                           body.count("\n", 0, token.end()) + 1))
            lines = body.split("\n")
            for number in range(1, len(lines) - 1):
                line = lines[number]
                if line.strip() and number not in protected:
                    width = len(line) - len(line.lstrip())
                    lines[number] = " " * max(0, width + delta) + line.lstrip()
            lines[-1] = indentation
            body = "\n".join(lines)

        result = result[:opening + 1] + body + result[closing:]
        if prefix.strip():
            before = result[:opening].rstrip(" \t")
            result = before + "\n" + indentation + result[opening:]
        else:
            result = result[:line_start] + indentation + result[opening:]
    if significant_tokens(result) != expected:
        raise FormattingError(f"{filename}: designated-initializer brace normalization changed "
                              "non-whitespace tokens; the file was left unformatted")
    return result
