"""Shared lexical naming rules; callers mask comments/literals before scanning."""
import re

# Require a type before the stars: *required = nullptr writes a pointee,
# and does not establish that required itself is nullable.
NULL_POINTER_DECLARATION = re.compile(
    r"\b(?!(?:return|co_return|delete)\b)[A-Za-z_]\w*(?:::\w+)*(?:<[^;{}()]*>)?\s*"
    r"(?:\*\s*(?:(?:const|volatile)\s+)*)+"
    r"(\w+)\s*(?:=\s*nullptr|\{\s*(?:nullptr)?\s*\})"
)
MOVE_PARAMETER = re.compile(r"(?:\(|,)\s*(?:const\s+)?[\w:]+(?:<[^()]*?>)?\s*&&\s*(\w+)\s*(?=[,)=])")


def pointer_findings(code):
    """Return offsets/rules/messages for explicit null initializers and move parameters.

    This does not infer nullable return values or prove a pointer's lifetime.
    """
    findings = []
    for match in NULL_POINTER_DECLARATION.finditer(code):
        if not match[1].endswith("OrNull"):
            findings.append((match.start(1), "N4", f"nullable pointer `{match[1]}` must end in OrNull"))
    for match in MOVE_PARAMETER.finditer(code):
        if re.match(r"(?:out|inout)(?:[A-Z]|$)", match[1]):
            findings.append((match.start(1), "N2", "rvalue reference parameters must omit out/inout prefixes"))
    return findings


QUESTION = re.compile(r"^m?(is|has|should|can|was|are|does|did|Is|Has|Should|Can|Was|Are|Does|Did)[A-Z0-9]")
OUT_QUESTION = re.compile(r"^(out|inout)(Is|Has|Should|Can|Was|Are|Does|Did)[A-Z]")
# `Type& name` followed by , ) or = in a parameter list (after `(` or `,`).
REFERENCE_PARAMETER = re.compile(
    r"(?:\(|,)\s*((?:[\w:]+(?:<[^()]*?>)?)(?:\s*\[[^\]]*\])?)&\s+([A-Za-z_]\w*)\s*(?=[,)=])"
)
BOOLEAN_DECLARATION = re.compile(r"\bbool\s+([A-Za-z_]\w*)\s*(?=[=;{,)\[])")


def reference_and_boolean_findings(code):
    """N2 (out/inout presence) and N3 (boolean names read as questions).

    Lexical: N2 cannot tell `out` from `inout`, and N3 only sees declarations
    spelled `bool`; both remain review items beyond these spellings.
    """
    findings = []
    for match in REFERENCE_PARAMETER.finditer(code):
        type_text, name = match.group(1), match.group(2)
        prefix = code[max(0, match.start() - 16):match.start(1)]
        if re.search(r"\bconst\s*$", prefix) or type_text.startswith("const"):
            continue
        if re.match(r"^(out|inout)[A-Z]", name):
            continue
        findings.append((match.start(2), "N2", f"non-const reference parameter `{name}` needs an out/inout prefix"))
    for match in BOOLEAN_DECLARATION.finditer(code):
        name = match.group(1)
        if name == "operator" or QUESTION.match(name) or OUT_QUESTION.match(name):
            continue
        findings.append((match.start(1), "N3", f"boolean `{name}` should read as a question (is/has/should/can...)"))
    return findings


# Abbreviations AGENTS.md "Spell names out" rejects. Whole camelCase/snake_case
# words only, so `argc`, `Parameter` and `Source` are unaffected.
ABBREVIATIONS = {
    "arg": "argument", "args": "arguments", "param": "parameter", "params": "parameters",
    "ctx": "context", "src": "source", "dst": "destination", "tmp": "temporary",
    "buf": "buffer", "cnt": "count", "idx": "index", "ptr": "pointer", "msg": "message",
    "cfg": "configuration", "mgr": "manager", "num": "count", "val": "value",
    "len": "length", "attr": "attribute", "elem": "element", "prev": "previous",
    "curr": "current", "str": "string", "btn": "button", "wnd": "window", "img": "image",
    "ret": "result", "obj": "object", "fn": "function", "func": "function",
    "cb": "callback", "evt": "event", "req": "request", "resp": "response",
    "addr": "address", "desc": "description", "conf": "configuration",
}
DECLARATION = re.compile(
    r"(?<![\w.>:])(?!(?:return|co_return|delete|new|else|case|goto|throw|using|namespace|sizeof|"
    r"alignof|decltype|typedef|operator|public|private|protected|template|if|while|for|switch)\b)"
    r"[A-Za-z_]\w*(?:::\w+)*(?:<[^;{}()]*?>)?(?:\s*[*&]+\s*|\s+)"
    r"(?!(?:const|override|final|noexcept|volatile)\b)([A-Za-z_]\w*)\s*(?=[=;,){\[(:])"
)


def identifier_words(name):
    name = re.sub(r"^m(?=[A-Z])", "", name)
    words = []
    for part in name.split("_"):
        words.extend(re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", part))
    return [word.lower() for word in words]


# Established engine type names and C/C++ entry-point conventions.
ABBREVIATION_ALLOWED = {"UniquePtr", "argc", "argv"}


def abbreviation_findings(code):
    """Declared names containing a rejected abbreviation (N5).

    Only declarations are inspected, so uses of third-party APIs are unaffected.
    The declaration pattern is lexical; it is a changed-line ratchet, not a parser.
    """
    findings = []
    for match in DECLARATION.finditer(code):
        name = match.group(1)
        if name in ABBREVIATION_ALLOWED:
            continue
        for word in identifier_words(name):
            if word in ABBREVIATIONS:
                findings.append((match.start(1), "N5", f"spell `{name}` out: `{word}` -> `{ABBREVIATIONS[word]}`"))
                break
    return findings
