"""Deterministic literal-UTF8 localization cooker. No linguistic pattern parser.

Thanks to Martin Brownlow, "Localization of Text Assets", Game Programming Golden
Rules, pp.91-92: keep identifiers separate from language-specific wording. Context
and review receipts are Ludus's extension. Thanks to James Boer, "A Flexible Text
Parsing System", Game Programming Gems 2, section 1.17, pp.112-117: validate
authoring data and generate synchronized
bindings offline. Original code; see docs/architecture/localization-gems-review.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import tempfile


PROFILE = "ludus-static-utf8-v1"
MAX_JSON_BYTES = 8 * 1024 * 1024
MAX_CATALOG_BYTES = 32 * 1024 * 1024
MAX_MESSAGES = 65536
MAX_TEXT_BYTES = 64 * 1024
HEADER_BYTES = 48
RECORD_BYTES = 20
ID = re.compile(r"[a-z0-9-]+(?:/[a-z0-9-]+)*\Z", re.ASCII)
LOCALE = re.compile(r"[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8})*\Z", re.ASCII)
NAMESPACE = re.compile(r"[a-z][a-z0-9_]*(?:::[a-z][a-z0-9_]*)*\Z", re.ASCII)
SYMBOL = re.compile(r"[A-Z][A-Za-z0-9]*\Z", re.ASCII)
# Reject keywords instead of emitting a syntactically invalid C++ namespace.
CPP_KEYWORDS = frozenset({
    "alignas", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char",
    "char16_t", "char32_t", "char8_t", "class", "co_await", "co_return", "co_yield", "compl", "concept", "const",
    "const_cast", "consteval", "constexpr", "constinit", "continue", "decltype", "default", "delete", "do",
    "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast", "requires",
    "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template",
    "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"})


class CookError(ValueError):
    """A bounded authoring or translation validation failure."""


def fail(message: str) -> None:
    raise CookError(message)


def _pairs(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            fail(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def read_json(path: Path) -> dict:
    with path.open("rb") as stream:
        data = stream.read(MAX_JSON_BYTES + 1)
    if len(data) > MAX_JSON_BYTES:
        fail(f"{path}: JSON exceeds {MAX_JSON_BYTES} bytes")
    # Bound nesting BEFORE json.loads, accounting for brackets inside strings.
    depth, quoted, escaped = 0, False, False
    for byte in data:
        if quoted:
            if escaped:
                escaped = False
            elif byte == 92:
                escaped = True
            elif byte == 34:
                quoted = False
        elif byte == 34:
            quoted = True
        elif byte in (91, 123):
            depth += 1
            if depth > 32:
                fail(f"{path}: JSON nesting exceeds 32")
        elif byte in (93, 125):
            depth -= 1
    try:
        value = json.loads(data.decode("utf-8"), object_pairs_hook=_pairs,
                           parse_constant=lambda value: fail(f"invalid JSON constant: {value}"))
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise CookError(f"{path}: {error}") from error
    if type(value) is not dict:
        fail(f"{path}: expected a JSON object")
    return value


def fields(value: object, required: set[str], optional: set[str], where: str) -> dict:
    if type(value) is not dict:
        fail(f"{where}: expected an object")
    missing = required - value.keys()
    unknown = value.keys() - required - optional
    if missing or unknown:
        fail(f"{where}: missing {sorted(missing)}, unknown {sorted(unknown)}")
    return value


def utf8(value: object, where: str, limit: int = MAX_TEXT_BYTES) -> bytes:
    if type(value) is not str:
        fail(f"{where}: expected a string")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise CookError(f"{where}: invalid Unicode scalar") from error
    if b"\0" in encoded or len(encoded) > limit:
        fail(f"{where}: embedded zero or exceeds {limit} UTF-8 bytes")
    return encoded


def identifier(value: object, where: str) -> str:
    utf8(value, where, 128)
    if not ID.fullmatch(value):
        fail(f"{where}: invalid Content ID grammar")
    return value


def locale(value: object, where: str) -> str:
    utf8(value, where, 128)
    if not LOCALE.fullmatch(value):
        fail(f"{where}: invalid ASCII locale subtag shape")
    return value


def revision(value: object, where: str) -> int:
    if type(value) is not int or not 1 <= value <= 0xffffffff:
        fail(f"{where}: expected a positive uint32 revision")
    return value


def document(value: dict, translation: bool) -> dict:
    required = {"version", "domain", "source_locale", "profile", "messages"}
    if translation:
        required.add("locale")
    fields(value, required, set(), "document")
    if type(value["version"]) is not int or value["version"] != 1 or value["profile"] != PROFILE:
        fail("document: unsupported version/profile; this cooker accepts literal UTF-8 only")
    identifier(value["domain"], "domain")
    locale(value["source_locale"], "source_locale")
    if translation:
        locale(value["locale"], "locale")
        if value["locale"] == value["source_locale"]:
            fail("translation locale must differ from source_locale")
    messages = value["messages"]
    if type(messages) is not list or len(messages) > MAX_MESSAGES:
        fail(f"messages: expected an array of at most {MAX_MESSAGES} records")
    keys = set()
    for message in messages:
        required = {"key", "source_revision", "text"}
        optional = {"context", "symbol"}
        if translation:
            required |= {"status", "reviewed_source_digest"}
            optional = {"source_text", "context"}
        else:
            required.add("context")
        fields(message, required, optional, "message")
        key = identifier(message["key"], "key")
        if key in keys:
            fail(f"duplicate message key: {key}")
        keys.add(key)
        revision(message["source_revision"], f"{key}: source_revision")
        utf8(message["text"], f"{key}: text")
        if "context" in message:
            utf8(message["context"], f"{key}: context", 16 * 1024)
        if not translation and not message["context"].strip():
            fail(f"{key}: translator context must not be empty")
        if "source_text" in message:
            utf8(message["source_text"], f"{key}: source_text")
        if translation:
            if message["status"] not in ("approved", "needs-review"):
                fail(f"{key}: unsupported review status")
            digest = message["reviewed_source_digest"]
            if type(digest) is not str or not re.fullmatch(r"[0-9a-f]{64}", digest, re.ASCII):
                fail(f"{key}: expected a SHA-256 source review receipt")
        elif "symbol" in message:
            if type(message["symbol"]) is not str or not SYMBOL.fullmatch(message["symbol"]):
                fail(f"{key}: symbol must be a PascalCase alphanumeric suffix")
    return value


def source_digest(source: dict, message: dict) -> str:
    # Canonical length-delimited JSON, not ambiguous concatenation. Context edits
    # invalidate review; code-only binding symbols are not linguistic inputs.
    receipt = {name: source[name] for name in ("version", "domain", "source_locale", "profile")}
    receipt.update({name: message[name] for name in ("key", "source_revision", "text", "context")})
    data = json.dumps(receipt, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def export_review(source: dict, target: str) -> dict:
    document(source, False)
    locale(target, "locale")
    if target == source["source_locale"]:
        fail("translation locale must differ from source_locale")
    result = {name: source[name] for name in ("version", "domain", "source_locale", "profile")}
    result["locale"] = target
    result["messages"] = [
        {"key": message["key"], "source_revision": message["source_revision"],
         "reviewed_source_digest": source_digest(source, message), "status": "needs-review", "text": "",
         "source_text": message["text"], "context": message["context"]}
        for message in sorted(source["messages"], key=lambda message: message["key"])
    ]
    return result


def cook(source: dict, translation: dict | None = None, *, allow_source_fallback: bool = False) -> bytes:
    document(source, False)
    targets = {}
    selected = source["source_locale"]
    if translation is not None:
        document(translation, True)
        for name in ("version", "domain", "source_locale", "profile"):
            if translation[name] != source[name]:
                fail(f"translation {name} disagrees with source")
        selected = translation["locale"]
        targets = {message["key"]: message for message in translation["messages"]}
    source_keys = {message["key"] for message in source["messages"]}
    if targets.keys() - source_keys:
        fail(f"unknown translated keys: {sorted(targets.keys() - source_keys)}")
    domain_bytes = source["domain"].encode("ascii")
    locale_bytes = selected.encode("ascii")
    source_bytes = source["source_locale"].encode("ascii")
    prefix = domain_bytes + locale_bytes + source_bytes
    ordered = sorted(source["messages"], key=lambda message: message["key"])
    payload_offset = HEADER_BYTES + len(prefix) + len(ordered) * RECORD_BYTES
    records, payload = bytearray(), bytearray()
    for message in ordered:
        key = message["key"]
        target = targets.get(key)
        text, origin = message["text"], 0
        if target is not None:
            if (target["source_revision"] != message["source_revision"] or
                    target["reviewed_source_digest"] != source_digest(source, message)):
                fail(f"{key}: stale source review receipt/revision")
            for name, source_name in (("source_text", "text"), ("context", "context")):
                if name in target and target[name] != message[source_name]:
                    fail(f"{key}: stale exported {name}")
        if translation is not None:
            if target is not None and target["status"] == "approved":
                text, origin = target["text"], 1
            elif allow_source_fallback:
                origin = 2
            else:
                fail(f"{key}: missing approved translation (explicit source fallback is disabled)")
        key_bytes, text_bytes = key.encode("ascii"), text.encode("utf-8")
        key_offset = payload_offset + len(payload)
        payload.extend(key_bytes)
        text_offset = payload_offset + len(payload)
        payload.extend(text_bytes)
        records.extend(struct.pack("<5I", key_offset, len(key_bytes), text_offset, len(text_bytes), origin))
        if payload_offset + len(payload) > MAX_CATALOG_BYTES:
            fail(f"catalog exceeds {MAX_CATALOG_BYTES} bytes")
    header = struct.pack("<12I", 0x434f4c4c, 1, payload_offset + len(payload), len(ordered),
                         len(domain_bytes), len(locale_bytes), len(source_bytes), RECORD_BYTES, 1, 0, 0, 0)
    return header + prefix + records + payload


def bindings(source: dict, namespace: str) -> str:
    document(source, False)
    if not NAMESPACE.fullmatch(namespace):
        fail("bindings namespace must contain lowercase C++ identifiers")
    if any(part in CPP_KEYWORDS or "__" in part for part in namespace.split("::")):
        fail("bindings namespace contains a reserved C++ identifier")
    lines = ["#pragma once", "", "// Generated from the source key schema; translation edits do not change this file.",
             "#include <ludus/localization/catalog.hpp>", "", f"namespace {namespace}", "{"]
    names = set()
    for message in sorted(source["messages"], key=lambda message: message["key"]):
        suffix = message.get("symbol", "".join(part.capitalize() for part in re.split(r"[-/]", message["key"]) if part))
        name = "k" + suffix
        if name in names:
            fail(f"{message['key']}: generated symbol collision {name}; supply distinct source symbols")
        names.add(name)
        lines.append(f'inline constexpr ludus::localization::MessageKey {name}'
                     f'{{"{source["domain"]}", "{message["key"]}"}};')
    return "\n".join(lines + [f"}} // namespace {namespace}", ""])


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Avoid needless rebuilds when a deterministic output is unchanged.
    if path.is_file() and path.stat().st_size == len(data) and path.read_bytes() == data:
        return
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    release = commands.add_parser("cook-release", help="cook a ludus-icu-mf1-v1 release manifest")
    release.add_argument("--manifest", type=Path, required=True)
    release.add_argument("--output", type=Path, required=True, help="release directory; release.json is written last")
    release.add_argument("--profile-checker", type=Path, help="pinned ICU ludus_localization_profile executable")
    release.add_argument("--profile-results", type=Path, help="verdicts recorded earlier by the native checker")
    release.add_argument("--record-profile-results", type=Path, help="record the native checker's verdicts")
    release.add_argument("--bindings", action="append", metavar="DOMAIN=NAMESPACE",
                         help="generate bindings/<domain>.hpp descriptors in this C++ namespace")
    inspect = commands.add_parser("inspect", help="dump a cooked package or verify a release.json")
    inspect.add_argument("path", type=Path)
    inspect.add_argument("--json", action="store_true")
    for name in ("cook", "export-review"):
        command = commands.add_parser(name)
        command.add_argument("--source", type=Path, required=True)
        command.add_argument("--output", type=Path, required=True)
        if name == "cook":
            command.add_argument("--translation", type=Path)
            command.add_argument("--allow-source-fallback", action="store_true")
            command.add_argument("--header", type=Path)
            command.add_argument("--namespace")
        else:
            command.add_argument("--locale", required=True)
    args = parser.parse_args(argv)
    if args.command in ("cook-release", "inspect"):
        # The mf1 release path lives in its own module; it imports this one.
        import localization_release
        try:
            if args.command == "cook-release":
                return localization_release.run_cook_release(args)
            return localization_release.run_inspect(args)
        except (CookError, OSError) as error:
            print(f"localization: {error}", file=sys.stderr)
            return 1
    try:
        inputs = [args.source] + ([args.translation] if getattr(args, "translation", None) else [])
        outputs = [args.output] + ([args.header] if getattr(args, "header", None) else [])
        resolved_inputs, resolved_outputs = {path.resolve() for path in inputs}, [path.resolve() for path in outputs]
        if resolved_inputs.intersection(resolved_outputs) or len(set(resolved_outputs)) != len(resolved_outputs):
            fail("output paths must be distinct and must not overwrite authoring inputs")
        source = document(read_json(args.source), False)
        if args.command == "export-review":
            result = export_review(source, args.locale)
            data = (json.dumps(result, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
        else:
            translation = read_json(args.translation) if args.translation else None
            data = cook(source, translation, allow_source_fallback=args.allow_source_fallback)
            header = None
            if args.header:
                if not args.namespace:
                    fail("--header requires an explicit --namespace")
                header = bindings(source, args.namespace).encode("utf-8")
            elif args.namespace:
                fail("--namespace requires --header")
            if header is not None:
                write_atomic(args.header, header)
        write_atomic(args.output, data)
        return 0
    except (CookError, OSError) as error:
        print(f"localization: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
