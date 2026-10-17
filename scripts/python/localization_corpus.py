"""Validate L0 `ludus-icu-mf1-v1` authoring documents and generate probe fixtures.

This checker owns the structural document, receipt and release-manifest rules in
docs/architecture/localization-l0.md. It never parses MessageFormat syntax: the
pinned ICU4C probe validates every pattern against the profile with ICU's own
MessagePattern, so Ludus does not maintain a second grammar implementation.
Original code; it reuses the bounded JSON reader of the static cooker.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from localization import (CookError, fail, fields, identifier, locale, read_json,  # noqa: E402
                          revision, utf8, write_atomic)

PROFILE = "ludus-icu-mf1-v1"
MAX_MESSAGES = 65536
MAX_LOCALES = 128
MAX_PATTERN_BYTES = 16 * 1024
MAX_CONTEXT_BYTES = 16 * 1024
MAX_ARGUMENTS = 32
MAX_TEXT_ARGUMENT_BYTES = 4 * 1024
MAX_ENUM_VALUES = 64
MAX_FALLBACK_PATH = 8
COUNT_LIMIT = 1_000_000_000
STATUSES = ("draft", "translated", "reviewed", "approved")
ARGUMENT = re.compile(r"[a-z][a-z0-9_]{0,63}\Z", re.ASCII)
DIGEST = re.compile(r"[0-9a-f]{64}\Z", re.ASCII)
REJECTIONS = ("SyntaxError", "NumberedArgument", "UndeclaredArgument", "UnsupportedArgumentType",
              "ArgumentTypeMismatch", "UnapprovedNumberStyle", "DeadPluralKeyword", "ExactValueOutOfRange",
              "NonIntegerExactValue", "InvalidPluralOffset", "UndeclaredEnumValue", "NestingTooDeep",
              "TooManyBranches", "PatternTooLarge")


def integer(value: object, where: str, low: int, high: int) -> int:
    # bool is an int subclass in Python; JSON true/false are never numbers here.
    if type(value) is not int or not low <= value <= high:
        fail(f"{where}: expected an integer in [{low}, {high}]")
    return value


def argument_schema(value: object, where: str) -> dict:
    if type(value) is not dict or "type" not in value:
        fail(f"{where}: expected an argument object with a type")
    kind = value["type"]
    if kind == "text":
        fields(value, {"name", "type", "origin"}, {"max_bytes"}, where)
        if value["origin"] not in ("game", "user"):
            fail(f"{where}: text origin must be game or user")
        integer(value.get("max_bytes", MAX_TEXT_ARGUMENT_BYTES), f"{where}: max_bytes", 1, MAX_TEXT_ARGUMENT_BYTES)
    elif kind in ("count", "number"):
        # Version 1 keeps receipts canonical by spelling every bound as a JSON integer.
        fields(value, {"name", "type", "min", "max"}, set(), where)
        low = integer(value["min"], f"{where}: min", -COUNT_LIMIT, COUNT_LIMIT)
        if integer(value["max"], f"{where}: max", -COUNT_LIMIT, COUNT_LIMIT) < low:
            fail(f"{where}: max must not be below min")
    elif kind == "enum":
        fields(value, {"name", "type", "values", "unknown"}, set(), where)
        values = value["values"]
        if type(values) is not list or not 1 <= len(values) <= MAX_ENUM_VALUES:
            fail(f"{where}: values must contain 1-{MAX_ENUM_VALUES} tokens")
        for token in values:
            if type(token) is not str or not ARGUMENT.fullmatch(token) or token == "other":
                fail(f"{where}: enum token {token!r} must be a lowercase identifier other than other")
        if len(set(values)) != len(values):
            fail(f"{where}: duplicate enum token")
        if value["unknown"] not in ("other", "reject"):
            fail(f"{where}: unknown policy must be other or reject")
    else:
        fail(f"{where}: unsupported argument type {kind!r}")
    if type(value["name"]) is not str or not ARGUMENT.fullmatch(value["name"]):
        fail(f"{where}: argument name must match [a-z][a-z0-9_]* within 64 bytes")
    return value


def header(value: object, translation: bool, where: str) -> dict:
    required = {"version", "domain", "source_locale", "profile", "messages"} | ({"locale"} if translation else set())
    fields(value, required, set(), where)
    if type(value["version"]) is not int or value["version"] != 1:
        fail(f"{where}: unsupported version")
    if value["profile"] != PROFILE:
        fail(f"{where}: profile must be {PROFILE}")
    identifier(value["domain"], f"{where}: domain")
    locale(value["source_locale"], f"{where}: source_locale")
    if translation:
        locale(value["locale"], f"{where}: locale")
        if value["locale"] == value["source_locale"]:
            fail(f"{where}: translation locale must differ from source_locale")
    if type(value["messages"]) is not list or len(value["messages"]) > MAX_MESSAGES:
        fail(f"{where}: messages must be an array of at most {MAX_MESSAGES} records")
    return value


def unique_key(message: dict, keys: set[str], where: str) -> str:
    key = identifier(message["key"], f"{where}: key")
    if key in keys:
        fail(f"{where}: duplicate message key {key}")
    keys.add(key)
    return key


def validate_source(value: object, where: str = "source") -> dict:
    header(value, False, where)
    keys: set[str] = set()
    for message in value["messages"]:
        fields(message, {"key", "source_revision", "pattern", "arguments", "context"}, {"required", "symbol"}, where)
        key = unique_key(message, keys, where)
        revision(message["source_revision"], f"{key}: source_revision")
        utf8(message["pattern"], f"{key}: pattern", MAX_PATTERN_BYTES)
        if not utf8(message["context"], f"{key}: context", MAX_CONTEXT_BYTES).strip():
            fail(f"{key}: translator context must not be empty")
        if type(message.get("required", True)) is not bool:
            fail(f"{key}: required must be a boolean")
        if "symbol" in message and (type(message["symbol"]) is not str or
                                    not re.fullmatch(r"[A-Z][A-Za-z0-9]*", message["symbol"], re.ASCII)):
            fail(f"{key}: symbol must be a PascalCase alphanumeric suffix")
        arguments = message["arguments"]
        if type(arguments) is not list or len(arguments) > MAX_ARGUMENTS:
            fail(f"{key}: arguments must be an array of at most {MAX_ARGUMENTS} declarations")
        names = [argument_schema(argument, f"{key}: argument")["name"] for argument in arguments]
        if len(set(names)) != len(names):
            fail(f"{key}: duplicate argument name")
    return value


def source_digest(source: dict, message: dict) -> str:
    """Hash every linguistic input a reviewer approved, as canonical integer/string JSON."""
    receipt = {name: source[name] for name in ("version", "domain", "source_locale", "profile")}
    receipt.update({name: message[name] for name in ("key", "source_revision", "pattern", "arguments", "context")})
    data = json.dumps(receipt, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def validate_translation(value: object, source: dict, where: str = "translation") -> dict[str, str]:
    """Validate one translation and return each key's derived review state."""
    header(value, True, where)
    for name in ("version", "domain", "source_locale", "profile"):
        if value[name] != source[name]:
            fail(f"{where}: {name} disagrees with source")
    records = {message["key"]: message for message in source["messages"]}
    keys: set[str] = set()
    states = {}
    for message in value["messages"]:
        fields(message, {"key", "source_revision", "reviewed_source_digest", "translation_revision", "status",
                         "pattern"}, set(), where)
        key = unique_key(message, keys, where)
        if key not in records:
            fail(f"{where}: unknown translated key {key}")
        revision(message["source_revision"], f"{key}: source_revision")
        revision(message["translation_revision"], f"{key}: translation_revision")
        if message["status"] not in STATUSES:
            fail(f"{key}: status must be one of {', '.join(STATUSES)}")
        if type(message["reviewed_source_digest"]) is not str or not DIGEST.fullmatch(message["reviewed_source_digest"]):
            fail(f"{key}: reviewed_source_digest must be a lowercase SHA-256 receipt")
        utf8(message["pattern"], f"{key}: pattern", MAX_PATTERN_BYTES)
        current = (message["source_revision"] == records[key]["source_revision"] and
                   message["reviewed_source_digest"] == source_digest(source, records[key]))
        # A stale receipt is derived, never authored: revision reuse cannot hide an edit.
        states[key] = message["status"] if current else "stale"
    return states


def resolve_path(root: Path, value: object, where: str) -> Path:
    if type(value) is not str or not value or Path(value).is_absolute() or ".." in Path(value).parts:
        fail(f"{where}: expected a relative path inside the manifest directory")
    return root / value


def fallback_paths(locales: list, source_locale: str) -> dict[str, list[str]]:
    parents = {}
    for entry in locales:
        fields(entry, {"locale", "parent"}, set(), "locale entry")
        tag = locale(entry["locale"], "locale entry")
        if tag in parents:
            fail(f"duplicate locale {tag}")
        parents[tag] = entry["parent"]
    if source_locale not in parents or parents[source_locale] is not None:
        fail("the source locale must be declared without a parent")
    paths = {}
    for tag in parents:
        path, current = [tag], parents[tag]
        while current is not None:
            if type(current) is not str or current not in parents:
                fail(f"{tag}: undeclared fallback parent {current!r}")
            if current in path:
                fail(f"{tag}: fallback cycle through {current}")
            path.append(current)
            if len(path) > MAX_FALLBACK_PATH:
                fail(f"{tag}: fallback path exceeds {MAX_FALLBACK_PATH} catalogs")
            current = parents[current]
        if path[-1] != source_locale:
            fail(f"{tag}: fallback path must end at the source locale")
        paths[tag] = path
    return paths


def load_release(manifest_path: Path) -> dict:
    """Validate a release manifest and resolve every (locale, key) to a provider."""
    manifest = read_json(manifest_path)
    root = manifest_path.parent
    fields(manifest, {"version", "source_locale", "profiles", "linguistic_data", "locales", "domains"}, set(),
           "manifest")
    if manifest["version"] != 1 or type(manifest["version"]) is not int:
        fail("manifest: unsupported version")
    source_locale = locale(manifest["source_locale"], "manifest: source_locale")
    if manifest["profiles"] != [PROFILE]:
        fail(f"manifest: profiles must be [{PROFILE!r}]")
    data = fields(manifest["linguistic_data"], {"backend", "icu", "cldr", "unicode"}, set(),
                  "linguistic_data")
    if data["backend"] != "icu4c" or any(type(data[name]) is not str or not data[name]
                                         for name in ("icu", "cldr", "unicode")):
        fail("linguistic_data: expected the icu4c backend and nonempty pinned versions")
    if type(manifest["locales"]) is not list or not 1 <= len(manifest["locales"]) <= MAX_LOCALES:
        fail(f"manifest: locales must contain 1-{MAX_LOCALES} entries")
    paths = fallback_paths(manifest["locales"], source_locale)
    if type(manifest["domains"]) is not list or not manifest["domains"]:
        fail("manifest: domains must be a nonempty array")
    domains = []
    seen = set()
    for entry in manifest["domains"]:
        fields(entry, {"domain", "source", "translations", "allow_required_fallback"}, set(), "domain entry")
        domain = identifier(entry["domain"], "domain entry")
        if domain in seen:
            fail(f"duplicate domain {domain}")
        seen.add(domain)
        source = validate_source(read_json(resolve_path(root, entry["source"], domain)), entry["source"])
        if source["domain"] != domain or source["source_locale"] != source_locale:
            fail(f"{domain}: source document disagrees with the manifest")
        allowed = entry["allow_required_fallback"]
        if type(allowed) is not list or any(tag not in paths or tag == source_locale for tag in allowed):
            fail(f"{domain}: allow_required_fallback must list declared translated locales")
        translations = entry["translations"]
        if type(translations) is not dict or set(translations) != set(paths) - {source_locale}:
            fail(f"{domain}: translations must name one document for every non-source locale")
        catalogs = {source_locale: {m["key"]: (m["pattern"], "approved") for m in source["messages"]}}
        documents = {source_locale: (entry["source"], source)}
        for tag, relative in sorted(translations.items()):
            document = read_json(resolve_path(root, relative, domain))
            states = validate_translation(document, source, relative)
            if document["locale"] != tag:
                fail(f"{relative}: locale disagrees with the manifest")
            catalogs[tag] = {m["key"]: (m["pattern"], states[m["key"]]) for m in document["messages"]}
            documents[tag] = (relative, document)
        resolved = {}
        for tag, path in paths.items():
            for message in source["messages"]:
                key = message["key"]
                provider = next((step for step in path if catalogs[step].get(key, ("", ""))[1] == "approved"), None)
                if provider is None:
                    fail(f"{tag} {key}: no approved provider on the fallback path")
                if provider != tag and message.get("required", True) and tag not in allowed:
                    fail(f"{tag} {key}: required message lacks an approved translation")
                resolved[(tag, key)] = (catalogs[provider][key][0], provider)
        domains.append({"domain": domain, "source": source, "resolved": resolved, "documents": documents})
    return {"manifest": manifest, "paths": paths, "domains": domains}


def validate_expectations(value: object, release: dict) -> list[dict]:
    fields(value, {"version", "domain", "cases"}, set(), "expectations")
    domain = next((d for d in release["domains"] if d["domain"] == value["domain"]), None)
    if value["version"] != 1 or domain is None or type(value["cases"]) is not list:
        fail("expectations: unsupported version, unknown domain or missing cases")
    records = {m["key"]: m for m in domain["source"]["messages"]}
    for case in value["cases"]:
        fields(case, {"locale", "key", "arguments"}, {"expected", "status", "reason"}, "case")
        if case["locale"] not in release["paths"] or case["key"] not in records:
            fail(f"case: undeclared locale/key {case['locale']} {case['key']}")
        if type(case["arguments"]) is not dict:
            fail("case: arguments must be an object")
        for name, argument in case["arguments"].items():
            if type(argument) not in (str, int, float) or (type(argument) is float and not math.isfinite(argument)):
                fail(f"case {case['key']}: argument {name} must be a string or finite number")
            if type(argument) is str:
                utf8(argument, f"case {case['key']}: {name}", MAX_TEXT_ARGUMENT_BYTES)
        if ("expected" in case) == ("status" in case):
            fail(f"case {case['key']}: specify exactly one of expected or status")
        if "expected" in case:
            utf8(case["expected"], f"case {case['key']}: expected", 64 * 1024)
        elif case["status"] != "InvalidArguments" or type(case.get("reason")) is not str:
            fail(f"case {case['key']}: failures use status InvalidArguments and a reason")
    return value["cases"]


def validate_rejections(value: object, release: dict) -> list[dict]:
    fields(value, {"version", "domain", "cases"}, set(), "rejections")
    domain = next((d for d in release["domains"] if d["domain"] == value["domain"]), None)
    if value["version"] != 1 or domain is None or type(value["cases"]) is not list:
        fail("rejections: unsupported version, unknown domain or missing cases")
    keys = {m["key"] for m in domain["source"]["messages"]}
    for case in value["cases"]:
        fields(case, {"locale", "key", "pattern", "reason", "note"}, set(), "rejection")
        if case["locale"] not in release["paths"] or case["key"] not in keys or case["reason"] not in REJECTIONS:
            fail(f"rejection {case['key']}: undeclared locale/key or unknown reason {case['reason']!r}")
        utf8(case["pattern"], f"rejection {case['key']}: pattern", MAX_PATTERN_BYTES)
    return value["cases"]


def literal(text: str) -> str:
    """Emit counted UTF-8 as a C++ literal; octal escapes never absorb later digits."""
    data = text.encode("utf-8")
    body = "".join(chr(b) if 0x20 <= b < 0x7f and b not in (0x22, 0x5c, 0x3f) else f"\\{b:03o}" for b in data)
    return f'Bytes{{"{body}", {len(data)}}}'


def probe_header(manifest_path: Path) -> str:
    release = load_release(manifest_path)
    root = manifest_path.parent
    expectations = validate_expectations(read_json(root / "expectations.json"), release)
    rejections = validate_rejections(read_json(root / "profile-rejections.json"), release)
    domain = release["domains"][0]
    locales = list(release["paths"])
    messages = domain["source"]["messages"]
    index = {m["key"]: i for i, m in enumerate(messages)}
    data = release["manifest"]["linguistic_data"]
    out = ["#pragma once", "// Generated by scripts/python/localization_corpus.py from modules/localization/corpus.",
           "// Private to the L0 ICU probe; never installed.", '#include "corpus_types.hpp"', "",
           "namespace ludus::icu_probe::corpus", "{"]
    out.append("inline constexpr Bytes LOCALES[] = {" + ", ".join(literal(tag) for tag in locales) + "};")
    out.append("inline constexpr LinguisticData PINNED = {" + ", ".join(
        literal(data[name]) for name in ("icu", "cldr", "unicode")) + "};")
    for i, message in enumerate(messages):
        for j, argument in enumerate(message["arguments"]):
            if argument["type"] == "enum":
                out.append(f"inline constexpr Bytes VALUES_{i}_{j}[] = {{" +
                           ", ".join(literal(v) for v in argument["values"]) + "};")
        if message["arguments"]:
            rows = []
            for j, argument in enumerate(message["arguments"]):
                kind = argument["type"]
                values = (f"VALUES_{i}_{j}, {len(argument['values'])}" if kind == "enum" else "nullptr, 0")
                rows.append(
                    f"    {{{literal(argument['name'])}, ArgumentType::{kind.capitalize()}, "
                    f"TextOrigin::{argument.get('origin', 'game').capitalize()}, "
                    f"{argument.get('max_bytes', MAX_TEXT_ARGUMENT_BYTES)}, {argument.get('min', 0)}, "
                    f"{argument.get('max', 0)}, {values}, UnknownPolicy::{argument.get('unknown', 'reject').capitalize()}}},")
            out += [f"inline constexpr ArgumentSchema ARGUMENTS_{i}[] = {{", *rows, "};"]
    out.append("inline constexpr MessageSchema MESSAGES[] = {")
    for i, message in enumerate(messages):
        arguments = f"ARGUMENTS_{i}, {len(message['arguments'])}" if message["arguments"] else "nullptr, 0"
        out.append(f"    {{{literal(message['key'])}, {arguments}}},")
    out.append("};")
    out.append("inline constexpr Pattern PATTERNS[] = {")
    for li, tag in enumerate(locales):
        for message in messages:
            text, provider = domain["resolved"][(tag, message["key"])]
            out.append(f"    {{{li}, {index[message['key']]}, {literal(text)}, {locales.index(provider)}}},")
    out.append("};")
    for c, case in enumerate(expectations):
        if case["arguments"]:
            rows = []
            for name, argument in case["arguments"].items():
                if type(argument) is str:
                    rows.append(f"    {{{literal(name)}, ValueKind::Text, 0, 0.0, {literal(argument)}}},")
                elif type(argument) is int:
                    rows.append(f"    {{{literal(name)}, ValueKind::Integer, {argument}, 0.0, Bytes{{}}}},")
                else:
                    rows.append(f"    {{{literal(name)}, ValueKind::Number, 0, {float(argument).hex()}, Bytes{{}}}},")
            out += [f"inline constexpr ArgumentValue VALUES_CASE_{c}[] = {{", *rows, "};"]
    out.append("inline constexpr Case CASES[] = {")
    for c, case in enumerate(expectations):
        values = f"VALUES_CASE_{c}, {len(case['arguments'])}" if case["arguments"] else "nullptr, 0"
        outcome, text = ("Formatted", case["expected"]) if "expected" in case else ("InvalidArguments", "")
        out.append(f"    {{{locales.index(case['locale'])}, {index[case['key']]}, {values}, "
                   f"Outcome::{outcome}, {literal(text)}}},")
    out.append("};")
    out.append("inline constexpr RejectionCase REJECTIONS[] = {")
    for case in rejections:
        out.append(f"    {{{locales.index(case['locale'])}, {index[case['key']]}, {literal(case['pattern'])}, "
                   f"Rejection::{case['reason']}}},")
    out += ["};", "} // namespace ludus::icu_probe::corpus", ""]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    validate = commands.add_parser("validate", help="validate a release manifest, its documents and fixtures")
    validate.add_argument("--manifest", type=Path, required=True)
    generate = commands.add_parser("probe-header", help="generate the private L0 ICU probe fixture header")
    generate.add_argument("--manifest", type=Path, required=True)
    generate.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        text = probe_header(args.manifest)
        if args.command == "probe-header":
            write_atomic(args.output, text.encode("utf-8"))
        return 0
    except (CookError, OSError) as error:
        print(f"localization corpus: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
