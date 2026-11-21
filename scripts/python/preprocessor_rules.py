"""Lexical rules for code that only some builds compile or that MSVC treats as constant.

Pull requests build on Linux; the MSVC and Emscripten builds run only in batch
validation. Two failure kinds recur there and are visible in the source alone:

- An internal-linkage constant or function, or a parameter, that is used only inside
  a preprocessor branch that does not enclose its declaration. Builds that compile the
  other branch report it as unused (-Wunused-const-variable, -Wunused-function,
  -Wunused-parameter, MSVC C4100).
- A run-time `if`/`while` whose condition names only UPPER_CASE constants. MSVC /W4
  reports it as C4127 (conditional expression is constant).

Both rules approximate the preprocessor: every branch counts as selectable, even one
that no supported build selects. Declare the symbol under the same condition as its
uses, discard a parameter in the other branches, use `if constexpr`, or waive a
deliberate case on the reported line. See
docs/development/continuous-integration.md#platform-only-warnings.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass, field
import itertools
import re

from formatting import TOKENS

CONDITIONAL_USE_WAIVER = re.compile(r"// ludus-style: conditional-use .{12,}")
CONSTANT_CONDITION_WAIVER = re.compile(r"// ludus-style: constant-condition .{12,}")
DIRECTIVE = re.compile(r"[ \t]*#[ \t]*(ifdef|ifndef|if|elifdef|elifndef|elif|else|endif|define|error)\b(.*)", re.DOTALL)
IDENTIFIER = re.compile(r"\b[A-Za-z_]\w*\b")
CONSTANT_NAME = re.compile(r"[A-Z][A-Z0-9_]*")
# Every combination of the conditions around a symbol's uses is tried; a symbol whose
# uses depend on more independent conditions than this is left to review.
MAXIMUM_CONDITIONS = 10
# Only a main file's internal-linkage symbols are reported as unused by Clang.
MAIN_FILE_SUFFIXES = (".cpp", ".cc", ".cxx", ".mm")
CONTROL_WORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype", "catch",
    "static_assert", "noexcept", "requires", "new", "delete", "throw",
}
QUALIFIER_WORDS = {"const", "volatile", "struct", "class", "enum", "typename", "signed", "unsigned"}
BUILTIN_TYPES = {"int", "char", "short", "long", "float", "double", "bool", "void", "auto", "wchar_t"}
FUNCTION_HEADER = re.compile(
    r"(~?[A-Za-z_][\w:~]*)\s*\(((?:[^()]|\((?:[^()]|\([^()]*\))*\))*)\)"
    r"(?:\s*(?:const|noexcept(?:\([^()]*\))?|override|final|&&?|->\s*[^{;]+?))*"
    r"(?:\s*:(?:[^{;]|\{[^{}]*\})*)?\s*$",
    re.DOTALL,
)
NAMESPACE_HEADER = re.compile(r"(?:inline\s+)?namespace(?:\s+[\w:]+)?|extern")
CLASS_HEADER = re.compile(r"(?:^|[\s>])(?:class|struct|union)\s*(?:[\w:]+)?(?:\s*final)?(?:\s*:[^;(){}]*)?$")
CONSTANT_DECLARATION = re.compile(
    r"(?m)^[ \t]*(static\s+)?(inline\s+|extern\s+)?(?:constexpr|const)\b[^;(){}=]*?"
    r"\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)?(?:=|\{)"
)


@dataclass(frozen=True)
class Branch:
    """One arm of a preprocessor conditional chain."""

    chain: int
    index: int


@dataclass
class Chain:
    condition: str
    branches: int = 1
    has_else: bool = False
    # Branches that stop the build with #error select no compiled configuration.
    rejected: set[int] = field(default_factory=set)


def normalize(kind: str, text: str) -> str:
    """Spell a condition so that equal and negated conditions compare equal."""
    text = re.sub(r"\s+", "", text.replace("\\\n", " "))
    text = re.sub(r"\bdefined\(?([A-Za-z_]\w*)\)?", r"defined(\1)", text)
    if kind in ("ifdef", "elifdef"):
        return f"defined({text})"
    if kind in ("ifndef", "elifndef"):
        return f"!defined({text})"
    return text


def is_atom(text: str) -> bool:
    """True for defined(X), a bare name or a fully parenthesized expression."""
    if re.fullmatch(r"defined\(\w+\)|\w+", text):
        return True
    if text.startswith("(") is False or text.endswith(")") is False:
        return False
    depth = 0
    for position, character in enumerate(text):
        depth += (character == "(") - (character == ")")
        if depth == 0 and position != len(text) - 1:
            return False
    return True


def split_polarity(condition: str) -> tuple[str, bool]:
    """Return a condition's positive spelling and whether the written form is positive."""
    if condition.startswith("!") and is_atom(condition[1:]):
        inner = condition[1:]
        if inner.startswith("(") and inner.endswith(")") and is_atom(inner[1:-1]):
            inner = inner[1:-1]
        return inner, False
    return condition, True


class Regions:
    """The preprocessor branches that enclose each line of a source."""

    def __init__(self, source: str):
        self.chains: dict[int, Chain] = {}
        self.definitions: list[tuple[int, str]] = []
        directives: dict[int, tuple[str, str, int]] = {}
        for token in TOKENS.finditer(source):
            match = DIRECTIVE.match(token.group())
            if match is not None:
                first = source.count("\n", 0, token.start()) + 1
                directives[first] = (match.group(1), match.group(2), first + token.group().count("\n"))
        stack: list[Branch] = []
        self.lines: list[tuple[Branch, ...]] = [()]
        line_count = source.count("\n") + 1
        number = 1
        while number <= line_count:
            if number not in directives:
                self.lines.append(tuple(stack))
                number += 1
                continue
            kind, text, last = directives[number]
            if kind == "define":
                self.definitions.append((number, text))
            elif kind == "error" and stack:
                self.chains[stack[-1].chain].rejected.add(stack[-1].index)
            elif kind.startswith("if"):
                chain = len(self.chains) + 1
                self.chains[chain] = Chain(normalize(kind, text))
                stack.append(Branch(chain, 0))
            elif kind.startswith("el") and stack:
                branch = stack.pop()
                self.chains[branch.chain].branches += 1
                self.chains[branch.chain].has_else |= kind == "else"
                stack.append(Branch(branch.chain, branch.index + 1))
            elif kind == "endif" and stack:
                stack.pop()
            self.lines.extend(tuple(stack) for _ in range(number, last + 1))
            number = last + 1

    def at(self, line: int) -> tuple[Branch, ...]:
        return self.lines[min(line, len(self.lines) - 1)]

    def has_unused_configuration(self, line: int, use_lines: list[int]) -> bool:
        """Whether some choice of branches compiles the declaration but none of its uses."""
        declaration = self.at(line)
        requirements = []
        variables: dict[str, list[object]] = {}
        chains: dict[int, Chain] = {}
        for use in use_lines:
            path = self.at(use)
            if path[: len(declaration)] != declaration:
                return False  # Used in a differently nested branch; leave it to review.
            if len(path) == len(declaration):
                return False  # Used wherever it is declared.
            needs = []
            for branch in path[len(declaration):]:
                chain = self.chains[branch.chain]
                chains[branch.chain] = chain
                if chain.branches == 1 or (chain.branches == 2 and chain.has_else):
                    condition, is_positive = split_polarity(chain.condition)
                    variables.setdefault(condition, [True, False])
                    needs.append((condition, is_positive == (branch.index == 0)))
                else:
                    key = f"#chain{branch.chain}"
                    variables.setdefault(key, list(range(chain.branches + (chain.has_else is False))))
                    needs.append((key, branch.index))
            requirements.append(needs)
        if len(variables) > MAXIMUM_CONDITIONS:
            return False
        names = list(variables)
        for values in itertools.product(*(variables[name] for name in names)):
            selection = dict(zip(names, values))
            if any(self.active_branch(number, chain, selection) in chain.rejected for number, chain in chains.items()):
                continue
            if all(any(selection[name] != value for name, value in needs) for needs in requirements):
                return True
        return False


    @staticmethod
    def active_branch(number: int, chain: Chain, selection: dict[str, object]) -> object:
        """The branch of a chain that a selection compiles, or None when it compiles none."""
        if chain.branches == 1 or (chain.branches == 2 and chain.has_else):
            condition, is_positive = split_polarity(chain.condition)
            if selection[condition] == is_positive:
                return 0
            return 1 if chain.has_else else None
        return selection[f"#chain{number}"]


@dataclass(frozen=True)
class Scope:
    kind: str  # "namespace", "anonymous", "class", "function" or "other"
    opening: int
    closing: int


class Scopes:
    """Brace scopes of code (comments, literals and directives blanked)."""

    def __init__(self, code: str):
        self.code = code
        self.functions: list[tuple[Scope, re.Match, int]] = []
        self.offsets: list[int] = []
        self.snapshots: list[tuple[Scope, ...]] = []
        stack: list[Scope] = []
        openings: list[int] = []
        closings: dict[int, int] = {}
        for match in re.finditer(r"[{}]", code):
            if match.group() == "{":
                openings.append(match.start())
            elif openings:
                closings[openings.pop()] = match.start()
        for match in re.finditer(r"[{}]", code):
            if match.group() == "}":
                if stack:
                    stack.pop()
            else:
                start = statement_start(code, match.start())
                header = code[start:match.start()].strip()
                enclosing = stack[-1].kind if stack else "namespace"
                function = FUNCTION_HEADER.search(code, start, match.start())
                if header == "namespace":
                    kind = "anonymous"
                elif NAMESPACE_HEADER.fullmatch(header):
                    kind = "namespace"
                elif CLASS_HEADER.search(header):
                    kind = "class"
                elif (enclosing in ("namespace", "anonymous", "class") and function is not None
                      and function.group(1).split("::")[-1] not in CONTROL_WORDS
                      and "operator" not in header
                      and re.search(r"\]\s*$", code[start:function.start()]) is None):
                    kind = "function"
                else:
                    kind = "other"
                scope = Scope(kind, match.start(), closings.get(match.start(), len(code) - 1))
                if kind == "function":
                    self.functions.append((scope, function, start))
                stack.append(scope)
            self.offsets.append(match.start())
            self.snapshots.append(tuple(stack))

    def enclosing(self, offset: int) -> tuple[Scope, ...]:
        position = bisect.bisect_right(self.offsets, offset) - 1
        return self.snapshots[position] if position >= 0 else ()


def statement_start(code: str, offset: int) -> int:
    position = offset - 1
    while position >= 0 and code[position] not in ";{}":
        position -= 1
    return position + 1


def parameter_names(parameters: str) -> list[tuple[int, str]]:
    """Named parameters of a parameter list, with their offsets inside it."""
    pieces = []
    depth = 0
    start = 0
    for position, character in enumerate(parameters + ","):
        if character in "(<[{":
            depth += 1
        elif character in ")>]}":
            depth -= 1
        elif character == "," and depth == 0:
            pieces.append((start, parameters[start:position]))
            start = position + 1
    names = []
    for offset, piece in pieces:
        declarator = piece.split("=", 1)[0]
        if "(" in declarator or "..." in declarator or "maybe_unused" in declarator:
            continue
        declarator = re.sub(r"\[[^\]]*\]\s*$", "", declarator.rstrip())
        words = list(IDENTIFIER.finditer(declarator))
        if len(words) < 2 or declarator[words[-1].end():].strip() != "":
            continue
        name = words[-1]
        preceding = {word.group() for word in words[:-1]}
        if name.group() in QUALIFIER_WORDS | BUILTIN_TYPES:
            continue
        if preceding <= QUALIFIER_WORDS and re.search(r"[*&>]\s*$", declarator[: name.start()]) is None:
            continue  # `const Type` or `struct Type` names no parameter.
        names.append((offset + name.start(), name.group()))
    return names


def conditional_use_findings(path: str, source: str, code: str) -> list[tuple[int, list[int], str]]:
    """Declarations used only under preprocessor branches: (declaration line, use lines, message)."""
    regions = Regions(source)
    scopes = Scopes(code)
    source_lines = source.splitlines()
    code_lines = code.splitlines()
    is_main_file = path.endswith(MAIN_FILE_SUFFIXES)
    found = []

    def line_of(offset: int) -> int:
        return code.count("\n", 0, offset) + 1

    def judge(name: str, declaration: int, first: int, last: int, message: str) -> None:
        line = line_of(declaration)
        text = source_lines[line - 1] if line <= len(source_lines) else ""
        if CONDITIONAL_USE_WAIVER.search(text) or "maybe_unused" in code_lines[line - 1]:
            return
        pattern = re.compile(rf"\b{re.escape(name)}\b")
        uses = [line_of(match.start()) for match in pattern.finditer(code, first, last) if match.start() != declaration]
        first_line = line_of(first)
        last_line = line_of(last)
        uses += [number for number, body in regions.definitions
                 if first_line <= number <= last_line and pattern.search(body)]
        if len(uses) == 0:
            return  # Wholly unused: every build already reports it.
        if regions.has_unused_configuration(line, uses):
            found.append((line, sorted(set(uses)), message))

    for scope, function, start in scopes.functions:
        signature = function.start()
        for offset, parameter in parameter_names(function.group(2)):
            judge(parameter, function.start(2) + offset, signature, scope.closing + 1,
                  f"parameter {parameter} is used only under a preprocessor branch; "
                  f"discard it in the other branches with static_cast<void>({parameter})")
        enclosing = scopes.enclosing(start)
        prefix = code[start:function.start()]
        is_internal = re.search(r"\bstatic\b", prefix) is not None or any(item.kind == "anonymous" for item in enclosing)
        is_namespace_scope = all(item.kind in ("namespace", "anonymous") for item in enclosing)
        if (is_main_file and is_internal and is_namespace_scope and "::" not in function.group(1)
                and re.search(r"\btemplate\b", prefix) is None):
            name = function.group(1)
            judge(name, function.start(1), 0, len(code),
                  f"{name} is used only under a preprocessor branch; declare it under the same condition as its uses")

    if is_main_file:
        # Namespace-scope constants have internal linkage unless declared inline or extern.
        for match in CONSTANT_DECLARATION.finditer(code):
            if match.group(2) is not None:
                continue
            if any(item.kind not in ("namespace", "anonymous") for item in scopes.enclosing(match.start())):
                continue
            name = match.group(3)
            judge(name, match.start(3), 0, len(code),
                  f"{name} is used only under a preprocessor branch; declare it under the same condition as its uses")
    return found


def constant_condition_findings(source: str, code: str) -> list[tuple[int, str]]:
    """Run-time if/while conditions that name only UPPER_CASE constants (MSVC C4127)."""
    source_lines = source.splitlines()
    found = []
    for match in re.finditer(r"\b(if|while)\s*\(", code):
        if re.match(r"\s*constexpr\b", code[match.end():]) or re.search(r"\bconstexpr\s*$", code[:match.start()][-20:]):
            continue
        depth = 1
        end = match.end()
        while depth and end < len(code):
            depth += (code[end] == "(") - (code[end] == ")")
            end += 1
        condition = code[match.end():end - 1]
        if re.search(r"[;{}\[\].]|->|\b(?:true|false|this|nullptr)\b", condition):
            continue
        names = list(IDENTIFIER.finditer(condition))
        is_constant = len(names) > 0
        for name in names:
            following = condition[name.end():].lstrip()
            if following.startswith("::"):
                continue  # A namespace or class qualifier.
            # A call, or a template argument list written without a space (`A < B` compares).
            if following.startswith("(") or condition[name.end():].startswith("<") or CONSTANT_NAME.fullmatch(name.group()) is None:
                is_constant = False
                break
        line = code.count("\n", 0, match.start())
        if is_constant and (line >= len(source_lines) or CONSTANT_CONDITION_WAIVER.search(source_lines[line]) is None):
            found.append((match.start(),
                          f"this {match.group(1)} condition names only constants, which MSVC reports as C4127; "
                          "use if constexpr"))
    return found
