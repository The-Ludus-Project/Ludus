"""L1 release cooker, descriptor generator and inspector for `ludus-icu-mf1-v1`.

The cooker reads a release manifest, validates its documents, receipts and
fallback graph through localization_corpus, and asks the pinned ICU4C host
profile checker (tools/icu-probe/profile_checker.cpp) to validate every
resolved pattern. It never parses MessageFormat syntax itself: argument-free
messages are cooked as the static UTF-8 text ICU decoded, and messages with
arguments keep their pattern and argument schema for L2 formatting. Outputs are
deterministic package version 2 files, per-domain descriptor headers, a source
map and a completion manifest written last. See docs/modules/localization.md.

Thanks to Jason Hughes, "Pointer Patching Assets", Game Engine Gems 2, chapter
20, pp.345-357, and James Boer, "A Flexible Text Parsing System", Game
Programming Gems 2, section 1.17, pp.112-117: prepare bounded offset-based data
and synchronized bindings offline. Original code; checked offsets replace
pointer patching. See docs/architecture/localization-gems-review.md.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from localization import (CPP_KEYWORDS, CookError, ID, LOCALE, NAMESPACE, SYMBOL, fail, read_json,  # noqa: E402
                          write_atomic)
from localization_corpus import (MAX_PATTERN_BYTES, MAX_TEXT_ARGUMENT_BYTES, PROFILE,  # noqa: E402
                                 REJECTIONS, load_release)

MAGIC = 0x434f4c4c  # "LLOC" in little endian; shared with package version 1.
PACKAGE_VERSION = 2
PROFILE_ID = 2  # ludus-icu-mf1-v1; profile 1 is ludus-static-utf8-v1.
HEADER_BYTES = 64
RECORD_BYTES = 40
ARGUMENT_BYTES = 36
SLICE_BYTES = 8
MAX_PACKAGE_BYTES = 32 * 1024 * 1024
MAX_MESSAGES = 65536
MAX_LABEL_BYTES = 128
MAX_STATIC_BYTES = 64 * 1024
MAX_PROVIDERS = 8
MAX_ARGUMENTS = 32
MAX_ENUM_VALUES = 64
COUNT_LIMIT = 1_000_000_000
KIND_STATIC = 0
KIND_DYNAMIC = 1
FLAG_REQUIRED = 1
ARGUMENT_TYPES = ("text", "count", "number", "enum")
ARGUMENT_NAME = re.compile(r"[a-z][a-z0-9_]{0,63}\Z", re.ASCII)
LINGUISTIC_LABEL = re.compile(r"[\x21-\x7e]{1,128}\Z", re.ASCII)

REQUEST_MAGIC = 0x5152504c  # "LPRQ"
RESPONSE_MAGIC = 0x5352504c  # "LPRS"
PROTOCOL_VERSION = 1
LOCALE_STATUSES = ("ok", "invalid-tag", "missing-data")
VERDICTS = ("Accepted",) + REJECTIONS
RECORDED_VERSION = 1
CHECKER_TIMEOUT_SECONDS = 600


# ---------------------------------------------------------------------------
# Argument schemas: one normalized form for the checker, packages and receipts.
# ---------------------------------------------------------------------------

def normalize_argument(argument: dict) -> dict:
    """Spell every field explicitly; defaults never differ between consumers. Idempotent."""
    if "policy" in argument:
        return dict(argument, values=list(argument["values"]))
    kind = argument["type"]
    policy = 0
    if kind == "text":
        policy = 1 if argument["origin"] == "user" else 0
    elif kind == "enum":
        policy = 1 if argument["unknown"] == "reject" else 0
    return {
        "name": argument["name"],
        "type": kind,
        "policy": policy,
        "max_bytes": argument.get("max_bytes", MAX_TEXT_ARGUMENT_BYTES) if kind == "text" else 0,
        "min": argument.get("min", 0),
        "max": argument.get("max", 0),
        "values": list(argument.get("values", [])),
    }


def schema_digest(domain: str, messages: list[dict]) -> str:
    """SHA-256 of the ordered key/argument schema shared by every locale of a domain."""
    schema = [[m["key"], [normalize_argument(a) for a in m["arguments"]]] for m in messages]
    data = json.dumps([domain, schema], ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(data.encode("utf-8")).hexdigest()


def item_digest(locale: str, arguments: list[dict], pattern: str) -> str:
    data = json.dumps([locale, arguments, pattern], ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(data.encode("utf-8")).hexdigest()


# ---------------------------------------------------------------------------
# Profile checkers: pinned native ICU, or its exactly recorded verdicts.
# ---------------------------------------------------------------------------

def _word(value: int) -> bytes:
    return struct.pack("<I", value)


def _signed(value: int) -> bytes:
    return struct.pack("<i", value)


def _counted(data: bytes) -> bytes:
    return _word(len(data)) + data


def encode_request(locales: list[str], items: list[dict]) -> bytes:
    index = {tag: i for i, tag in enumerate(locales)}
    out = bytearray(_word(REQUEST_MAGIC) + _word(PROTOCOL_VERSION) + _word(len(locales)))
    for tag in locales:
        out += _counted(tag.encode("ascii"))
    out += _word(len(items))
    for item in items:
        out += _word(index[item["locale"]]) + _counted(item["pattern"].encode("utf-8"))
        out += _word(len(item["arguments"]))
        for argument in item["arguments"]:
            out += _counted(argument["name"].encode("ascii"))
            out += _word(ARGUMENT_TYPES.index(argument["type"])) + _word(argument["policy"])
            out += _word(argument["max_bytes"]) + _signed(argument["min"]) + _signed(argument["max"])
            out += _word(len(argument["values"]))
            for value in argument["values"]:
                out += _counted(value.encode("ascii"))
    return bytes(out)


class _Reader:
    def __init__(self, data: bytes, what: str) -> None:
        self.data, self.cursor, self.what = data, 0, what

    def word(self) -> int:
        if len(self.data) - self.cursor < 4:
            fail(f"{self.what}: truncated")
        (value,) = struct.unpack_from("<I", self.data, self.cursor)
        self.cursor += 4
        return value

    def counted(self) -> bytes:
        length = self.word()
        if len(self.data) - self.cursor < length:
            fail(f"{self.what}: truncated")
        value = self.data[self.cursor:self.cursor + length]
        self.cursor += length
        return value


def decode_response(data: bytes, locales: list[str], items: list[dict]) -> tuple[dict, dict, list[dict]]:
    reader = _Reader(data, "profile checker response")
    if reader.word() != RESPONSE_MAGIC or reader.word() != PROTOCOL_VERSION:
        fail("profile checker response: unsupported magic or protocol version")
    identity = {"protocol": PROTOCOL_VERSION}
    for name in ("icu", "cldr", "unicode"):
        identity[name] = reader.counted().decode("ascii")
    identity["data_bytes"] = reader.word()
    identity["data_fingerprint"] = b"".join(_word(reader.word()) for _ in range(4)).hex()
    if reader.word() != len(locales):
        fail("profile checker response: locale count mismatch")
    statuses = {}
    for tag in locales:
        status = reader.word()
        if status >= len(LOCALE_STATUSES):
            fail("profile checker response: unknown locale status")
        statuses[tag] = LOCALE_STATUSES[status]
    if reader.word() != len(items):
        fail("profile checker response: item count mismatch")
    results = []
    for item in items:
        verdict, has_text = reader.word(), reader.word()
        text = reader.counted()
        if verdict >= len(VERDICTS) or has_text not in (0, 1) or (has_text == 0 and text):
            fail("profile checker response: malformed verdict")
        result = {"verdict": VERDICTS[verdict]}
        if has_text:
            try:
                result["text"] = text.decode("utf-8", errors="strict")
            except UnicodeError as error:
                raise CookError("profile checker response: decoded text is not UTF-8") from error
        results.append(result)
    if reader.cursor != len(data):
        fail("profile checker response: trailing bytes")
    return identity, statuses, results


class NativeProfileChecker:
    """Runs the pinned ICU host checker once for a whole cook."""

    kind = "native"

    def __init__(self, executable: Path) -> None:
        self.executable = executable

    def check(self, locales: list[str], items: list[dict]) -> tuple[dict, dict, list[dict]]:
        if not self.executable.is_file():
            fail(f"profile checker {self.executable} does not exist; build ludus_localization_profile")
        with tempfile.TemporaryDirectory(prefix="ludus-localization-profile-") as directory:
            root = Path(directory).resolve()
            (root / "request.bin").write_bytes(encode_request(locales, items))
            try:
                completed = subprocess.run([str(self.executable), str(root), "request.bin", "response.bin"],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                           timeout=CHECKER_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired as error:
                raise CookError("profile checker timed out") from error
            if completed.returncode != 0:
                output = completed.stdout.decode("utf-8", errors="replace").strip()[-2000:]
                fail(f"profile checker exited {completed.returncode}: {output}")
            return decode_response((root / "response.bin").read_bytes(), locales, items)


class RecordedProfileChecker:
    """Replays verdicts recorded by the native checker for exactly these inputs.

    A recording is not a grammar: an item absent from it fails the cook. The
    ICU-enabled test regenerates the committed corpus recording and requires
    byte equality, so a stale or edited recording fails there.
    """

    kind = "recorded"

    def __init__(self, path: Path) -> None:
        self.path = path

    def check(self, locales: list[str], items: list[dict]) -> tuple[dict, dict, list[dict]]:
        recorded = read_json(self.path)
        if (type(recorded) is not dict or set(recorded) != {"version", "profile", "checker", "locales", "results"} or
                recorded["version"] != RECORDED_VERSION or recorded["profile"] != PROFILE):
            fail(f"{self.path}: unsupported recorded profile results")
        verdicts = {}
        for entry in recorded["results"]:
            if type(entry) is not dict or entry.get("verdict") not in VERDICTS:
                fail(f"{self.path}: malformed recorded verdict")
            verdicts[entry["item"]] = {name: entry[name] for name in ("verdict", "text") if name in entry}
        statuses = {}
        for tag in locales:
            if recorded["locales"].get(tag) not in LOCALE_STATUSES:
                fail(f"{self.path}: no recorded locale data verdict for {tag}; rerun the native checker")
            statuses[tag] = recorded["locales"][tag]
        results = []
        for item in items:
            digest = item_digest(item["locale"], item["arguments"], item["pattern"])
            if digest not in verdicts:
                fail(f"{self.path}: no recorded verdict for {item['locale']} {item['key']}; rerun the native checker")
            results.append(verdicts[digest])
        return recorded["checker"], statuses, results


def recording(identity: dict, statuses: dict, items: list[dict], results: list[dict]) -> bytes:
    entries = {}
    for item, result in zip(items, results):
        entries[item_digest(item["locale"], item["arguments"], item["pattern"])] = result
    document = {
        "version": RECORDED_VERSION,
        "profile": PROFILE,
        "checker": identity,
        "locales": dict(sorted(statuses.items())),
        "results": [{"item": digest, **entries[digest]} for digest in sorted(entries)],
    }
    return (json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


# ---------------------------------------------------------------------------
# Package version 2 encoder and strict decoder.
# ---------------------------------------------------------------------------

def linguistic_label(data: dict) -> str:
    return f"{data['backend']}-{data['icu']}/cldr-{data['cldr']}/unicode-{data['unicode']}"


def encode_package(domain: str, locale: str, source_locale: str, providers: list[str], linguistic: str,
                   records: list[dict]) -> bytes:
    """Encode one (domain, locale) package; records must already be key-sorted."""
    prefix = b"".join(value.encode("ascii") for value in (domain, locale, source_locale, linguistic))
    arguments = [a for record in records for a in record["arguments"]]
    values = [v for a in arguments for v in a["values"]]
    providers_at = HEADER_BYTES + len(prefix)
    records_at = providers_at + len(providers) * SLICE_BYTES
    arguments_at = records_at + len(records) * RECORD_BYTES
    values_at = arguments_at + len(arguments) * ARGUMENT_BYTES
    payload_at = values_at + len(values) * SLICE_BYTES
    payload = bytearray()

    def place(data: bytes) -> tuple[int, int]:
        offset = payload_at + len(payload)
        payload.extend(data)
        if payload_at + len(payload) > MAX_PACKAGE_BYTES:
            fail(f"{domain} {locale}: package exceeds {MAX_PACKAGE_BYTES} bytes")
        return offset, len(data)

    provider_table = b"".join(struct.pack("<2I", *place(tag.encode("ascii"))) for tag in providers)
    record_table, argument_table, value_table = bytearray(), bytearray(), bytearray()
    first_argument, first_value = 0, 0
    for record in records:
        key = place(record["key"].encode("ascii"))
        value = place(record["value"])
        flags = FLAG_REQUIRED if record["required"] else 0
        record_table += struct.pack("<10I", *key, *value, record["kind"], record["provider"], first_argument,
                                    len(record["arguments"]), record["source_revision"], flags)
        first_argument += len(record["arguments"])
        for argument in record["arguments"]:
            name = place(argument["name"].encode("ascii"))
            argument_table += struct.pack("<5I2i2I", *name, ARGUMENT_TYPES.index(argument["type"]), argument["policy"],
                                          argument["max_bytes"], argument["min"], argument["max"], first_value,
                                          len(argument["values"]))
            first_value += len(argument["values"])
            for token in argument["values"]:
                value_table += struct.pack("<2I", *place(token.encode("ascii")))
    total = payload_at + len(payload)
    header = struct.pack("<16I", MAGIC, PACKAGE_VERSION, total, len(records), len(domain), len(locale),
                         len(source_locale), RECORD_BYTES, PROFILE_ID, len(providers), len(arguments), ARGUMENT_BYTES,
                         len(values), len(linguistic), 0, 0)
    data = header + prefix + provider_table + record_table + argument_table + value_table + payload
    assert len(data) == total
    return bytes(data)


def _ascii(data: bytes, pattern: re.Pattern, what: str) -> str:
    try:
        text = data.decode("ascii")
    except UnicodeError as error:
        raise CookError(f"{what}: not ASCII") from error
    if not pattern.fullmatch(text):
        fail(f"{what}: invalid grammar")
    return text


def _utf8(data: bytes, what: str) -> str:
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise CookError(f"{what}: invalid UTF-8") from error
    if "\0" in text:
        fail(f"{what}: embedded zero")
    return text


def decode_package(data: bytes) -> dict:
    """Strictly decode a version 2 package; mirrors the runtime reader's rules."""
    if len(data) > MAX_PACKAGE_BYTES:
        fail("package exceeds the size ceiling")
    if len(data) < HEADER_BYTES:
        fail("package is shorter than its header")
    header = struct.unpack_from("<16I", data, 0)
    (magic, version, total, count, domain_length, locale_length, source_length, record_width, profile, provider_count,
     argument_count, argument_width, value_count, label_length, reserved0, reserved1) = header
    if magic != MAGIC:
        fail("not a localization package")
    if version != PACKAGE_VERSION or record_width != RECORD_BYTES or profile != PROFILE_ID or \
            argument_width != ARGUMENT_BYTES:
        fail("unsupported package version, record width or profile")
    if total != len(data) or reserved0 or reserved1:
        fail("package size or reserved fields are invalid")
    if count > MAX_MESSAGES or max(domain_length, locale_length, source_length, label_length) > MAX_LABEL_BYTES or \
            not 1 <= provider_count <= MAX_PROVIDERS or argument_count > count * MAX_ARGUMENTS or \
            value_count > argument_count * MAX_ENUM_VALUES:
        fail("package counts exceed their limits")
    cursor = HEADER_BYTES

    def take(length: int) -> bytes:
        nonlocal cursor
        if len(data) - cursor < length:
            fail("package is truncated")
        value = data[cursor:cursor + length]
        cursor += length
        return value

    domain = _ascii(take(domain_length), ID, "domain")
    locale = _ascii(take(locale_length), LOCALE, "locale")
    source_locale = _ascii(take(source_length), LOCALE, "source locale")
    linguistic = _ascii(take(label_length), LINGUISTIC_LABEL, "linguistic data")
    tables = take(provider_count * SLICE_BYTES + count * RECORD_BYTES + argument_count * ARGUMENT_BYTES +
                  value_count * SLICE_BYTES)
    position = 0

    def row(fmt: str) -> tuple:
        nonlocal position
        value = struct.unpack_from(fmt, tables, position)
        position += struct.calcsize(fmt)
        return value

    payload = cursor

    def slice_at(offset: int, length: int, limit: int, what: str) -> bytes:
        nonlocal payload
        if length > limit:
            fail(f"{what}: exceeds {limit} bytes")
        if offset != payload or length > len(data) - payload:
            fail(f"{what}: offset is not the next packed slice")
        payload += length
        return data[offset:offset + length]

    provider_rows = [row("<2I") for _ in range(provider_count)]
    record_rows = [row("<10I") for _ in range(count)]
    argument_rows = [row("<5I2i2I") for _ in range(argument_count)]
    value_rows = [row("<2I") for _ in range(value_count)]
    providers = [_ascii(slice_at(*r, MAX_LABEL_BYTES, "provider"), LOCALE, "provider") for r in provider_rows]
    if len(set(providers)) != len(providers) or providers[0] != locale or providers[-1] != source_locale or \
            (locale == source_locale) != (provider_count == 1):
        fail("provider path must start at the locale, end at the source locale and be unique")
    records, previous, next_argument, next_value = [], None, 0, 0
    for index, (key_offset, key_length, value_offset, value_length, kind, provider, first, arguments_in_record,
                source_revision, flags) in enumerate(record_rows):
        where = f"record {index}"
        key = _ascii(slice_at(key_offset, key_length, MAX_LABEL_BYTES, where), ID, f"{where} key")
        if previous is not None and previous >= key.encode("ascii"):
            fail(f"{where}: keys are not strictly sorted")
        previous = key.encode("ascii")
        limit = MAX_STATIC_BYTES if kind == KIND_STATIC else MAX_PATTERN_BYTES
        if kind not in (KIND_STATIC, KIND_DYNAMIC):
            fail(f"{where}: unknown kind")
        value = _utf8(slice_at(value_offset, value_length, limit, f"{where} value"), f"{where} value")
        if provider >= provider_count or first != next_argument or arguments_in_record > MAX_ARGUMENTS or \
                (kind == KIND_STATIC) != (arguments_in_record == 0) or source_revision == 0 or flags & ~FLAG_REQUIRED:
            fail(f"{where}: invalid provider, argument range, kind, revision or flags")
        next_argument += arguments_in_record
        schema, names = [], set()
        for argument_index in range(first, first + arguments_in_record):
            if argument_index >= argument_count:
                fail(f"{where}: argument range exceeds the argument table")
            (name_offset, name_length, kind_index, policy, max_bytes, low, high, first_value,
             values_in_argument) = argument_rows[argument_index]
            name = _ascii(slice_at(name_offset, name_length, 64, f"{where} argument"), ARGUMENT_NAME,
                          f"{where} argument")
            if name in names or kind_index >= len(ARGUMENT_TYPES):
                fail(f"{where}: duplicate argument name or unknown type")
            names.add(name)
            argument_type = ARGUMENT_TYPES[kind_index]
            is_text, is_enum, is_number = argument_type == "text", argument_type == "enum", \
                argument_type in ("count", "number")
            if policy > 1 or (is_text != (1 <= max_bytes <= MAX_TEXT_ARGUMENT_BYTES)) or \
                    (not is_text and max_bytes) or (not is_number and (low or high)) or \
                    (is_number and not -COUNT_LIMIT <= low <= high <= COUNT_LIMIT) or \
                    (is_enum != (1 <= values_in_argument <= MAX_ENUM_VALUES)) or (not is_enum and values_in_argument) or \
                    (is_number and policy) or first_value != next_value:
                fail(f"{where}: argument {name} has an invalid schema")
            next_value += values_in_argument
            tokens = []
            for value_index in range(first_value, first_value + values_in_argument):
                if value_index >= value_count:
                    fail(f"{where}: enum range exceeds the value table")
                token = _ascii(slice_at(*value_rows[value_index], 64, f"{where} enum value"), ARGUMENT_NAME,
                               f"{where} enum value")
                if token == "other" or token in tokens:
                    fail(f"{where}: enum value {token} is reserved or duplicated")
                tokens.append(token)
            schema.append({"name": name, "type": argument_type, "policy": policy, "max_bytes": max_bytes,
                           "min": low, "max": high, "values": tokens})
        records.append({"key": key, "kind": "static" if kind == KIND_STATIC else "dynamic", "value": value,
                        "provider": providers[provider], "origin": _origin(locale, source_locale, providers, provider),
                        "source_revision": source_revision, "required": bool(flags & FLAG_REQUIRED),
                        "arguments": schema})
    if next_argument != argument_count or sum(r[8] for r in argument_rows) != value_count:
        fail("argument or enum value tables contain unreferenced entries")
    if payload != len(data):
        fail("package has trailing bytes")
    return {"version": version, "profile": PROFILE, "domain": domain, "locale": locale,
            "source_locale": source_locale, "linguistic_data": linguistic, "providers": providers,
            "records": records, "bytes": len(data), "payload_bytes": len(data) - cursor,
            "directory_bytes": cursor - HEADER_BYTES, "schema_sha256": schema_digest(domain, records)}


def _origin(locale: str, source_locale: str, providers: list[str], provider: int) -> str:
    if locale == source_locale:
        return "source"
    if provider == 0:
        return "translation"
    return "source-fallback" if provider == len(providers) - 1 else "fallback"


# ---------------------------------------------------------------------------
# Generated per-domain descriptors.
# ---------------------------------------------------------------------------

def _pascal(text: str, separators: str) -> str:
    return "".join(part.capitalize() for part in re.split(f"[{separators}]", text) if part)


def descriptors(source: dict, namespace: str) -> str:
    """Exact keys and argument schemas only; translation edits never change the header."""
    if not NAMESPACE.fullmatch(namespace) or \
            any(part in CPP_KEYWORDS or "__" in part for part in namespace.split("::")):
        fail(f"bindings namespace {namespace!r} must contain lowercase, non-reserved C++ identifiers")
    domain = source["domain"]
    lines = ["#pragma once", "",
             f"// Generated from the {domain} source schema by scripts/localization cook-release.",
             "// Translation edits do not change this file; key or argument schema edits do.",
             "#include <ludus/localization/catalog.hpp>", "", "#include <string_view>", "",
             f"namespace {namespace}", "{"]
    names: dict[str, str] = {}

    def claim(name: str, owner: str) -> str:
        if name in names:
            fail(f"{owner}: generated symbol collision {name} with {names[name]}; supply distinct source symbols")
        names[name] = owner
        return name

    qualified = "ludus::localization"
    for message in sorted(source["messages"], key=lambda m: m["key"]):
        key = message["key"]
        suffix = message.get("symbol", _pascal(key, "/-"))
        if not SYMBOL.fullmatch(suffix):
            fail(f"{key}: generated symbol suffix {suffix!r} is not PascalCase")
        descriptor = claim("k" + suffix, key)
        arguments = [normalize_argument(a) for a in message["arguments"]]
        if not arguments:
            lines.append(f'inline constexpr {qualified}::MessageDescriptor {descriptor}{{{{"{domain}", "{key}"}}, {{}}}};')
            continue
        rows = []
        for argument in arguments:
            # Spell every field in declaration order so consumers building with
            # -Wmissing-field-initializers stay warning-clean.
            kind = argument["type"]
            values = "{}"
            if kind == "enum":
                values = claim(f"{descriptor}{_pascal(argument['name'], '_')}Values", key)
                tokens = ", ".join(f'"{token}"' for token in argument["values"])
                lines.append(f"inline constexpr std::string_view {values}[] = {{{tokens}}};")
            origin = "User" if kind == "text" and argument["policy"] else "Game"
            unknown = "Other" if kind == "enum" and not argument["policy"] else "Reject"
            fields = [f'.Name = "{argument["name"]}"', f".Type = {qualified}::ArgumentType::{kind.capitalize()}",
                      f".Origin = {qualified}::TextOrigin::{origin}",
                      f".Unknown = {qualified}::UnknownPolicy::{unknown}", f".MaxBytes = {argument['max_bytes']}",
                      f".Minimum = {argument['min']}", f".Maximum = {argument['max']}", f".Values = {values}"]
            rows.append("    {" + ", ".join(fields) + "},")
        table = claim(f"{descriptor}Arguments", key)
        lines += [f"inline constexpr {qualified}::ArgumentDescriptor {table}[] = {{", *rows, "};",
                  f'inline constexpr {qualified}::MessageDescriptor {descriptor}{{{{"{domain}", "{key}"}}, {table}}};']
    return "\n".join(lines + [f"}} // namespace {namespace}", ""])


# ---------------------------------------------------------------------------
# Release cooking.
# ---------------------------------------------------------------------------

def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _check_items(release: dict) -> tuple[list[str], list[dict]]:
    """One check per (provider, key): a pattern is validated in the locale that provides it."""
    locales = list(release["paths"])
    items, seen = [], set()
    for domain in release["domains"]:
        schemas = {m["key"]: [normalize_argument(a) for a in m["arguments"]] for m in domain["source"]["messages"]}
        for (tag, key), (pattern, provider) in sorted(domain["resolved"].items()):
            if (domain["domain"], provider, key) in seen:
                continue
            seen.add((domain["domain"], provider, key))
            items.append({"domain": domain["domain"], "locale": provider, "key": key, "pattern": pattern,
                          "arguments": schemas[key]})
    return locales, items


def cook_release(manifest_path: Path, checker, *, namespaces: dict[str, str] | None = None) -> dict:
    """Validate and cook a whole release in memory; nothing is written here."""
    namespaces = dict(namespaces or {})
    release = load_release(manifest_path)
    manifest = release["manifest"]
    known = {d["domain"] for d in release["domains"]}
    if namespaces.keys() - known:
        fail(f"bindings name undeclared domains: {sorted(namespaces.keys() - known)}")
    if len(set(namespaces.values())) != len(namespaces):
        fail("each domain's bindings need a distinct C++ namespace")
    locales, items = _check_items(release)
    identity, statuses, results = checker.check(locales, items)
    data = manifest["linguistic_data"]
    for name in ("icu", "cldr", "unicode"):
        if identity.get(name) != data[name]:
            fail(f"profile checker reports {name} {identity.get(name)!r}, manifest pins {data[name]!r}")
    problems = [f"{tag}: profile checker has no usable linguistic data ({statuses[tag]})"
                for tag in locales if statuses[tag] != "ok"]
    verdicts = {}
    for item, result in zip(items, results):
        if result["verdict"] != "Accepted":
            problems.append(f"{item['domain']} {item['locale']} {item['key']}: pattern rejected: {result['verdict']}")
        elif bool(item["arguments"]) == ("text" in result):
            fail(f"{item['locale']} {item['key']}: checker returned an unexpected decoded-text shape")
        verdicts[(item["domain"], item["locale"], item["key"])] = result
    if problems:
        fail("profile validation failed:\n  " + "\n  ".join(problems))
    label = linguistic_label(data)
    if not LINGUISTIC_LABEL.fullmatch(label):
        fail(f"linguistic data label {label!r} is not printable ASCII within {MAX_LABEL_BYTES} bytes")
    root = manifest_path.parent
    packages, bindings, source_map = [], [], []
    for domain in release["domains"]:
        name = domain["domain"]
        source = domain["source"]
        ordered = sorted(source["messages"], key=lambda m: m["key"])
        documents = domain["documents"]
        for tag, path in release["paths"].items():
            records, provenance = [], []
            for message in ordered:
                key = message["key"]
                pattern, provider = domain["resolved"][(tag, key)]
                arguments = [normalize_argument(a) for a in message["arguments"]]
                result = verdicts[(name, provider, key)]
                if arguments:
                    value, kind = pattern.encode("utf-8"), KIND_DYNAMIC
                else:
                    value, kind = result["text"].encode("utf-8"), KIND_STATIC
                    if len(value) > MAX_STATIC_BYTES:
                        fail(f"{tag} {key}: static text exceeds {MAX_STATIC_BYTES} bytes")
                records.append({"key": key, "kind": kind, "value": value, "provider": path.index(provider),
                                "source_revision": message["source_revision"],
                                "required": message.get("required", True), "arguments": arguments})
                relative, document = documents[provider]
                translated = next((m for m in document["messages"] if m["key"] == key), None) \
                    if provider != release["manifest"]["source_locale"] else None
                provenance.append({"key": key, "provider": provider, "document": relative,
                                   "source_revision": message["source_revision"],
                                   "translation_revision": translated["translation_revision"] if translated else None,
                                   "kind": "dynamic" if arguments else "static"})
            package = encode_package(name, tag, manifest["source_locale"], path, label, records)
            relative_path = f"packages/{name}/{tag}.loc"
            packages.append({"domain": name, "locale": tag, "path": relative_path, "bytes": len(package),
                             "sha256": _sha(package), "messages": len(records),
                             "static": sum(r["kind"] == KIND_STATIC for r in records),
                             "dynamic": sum(r["kind"] == KIND_DYNAMIC for r in records),
                             "providers": path, "schema_sha256": schema_digest(name, ordered), "data": package})
            source_map.append({"domain": name, "locale": tag, "records": provenance})
        if name in namespaces:
            header = descriptors(source, namespaces[name]).encode("utf-8")
            bindings.append({"domain": name, "namespace": namespaces[name], "path": f"bindings/{name}.hpp",
                             "sha256": _sha(header), "data": header})
    inputs = {"manifest": _sha(manifest_path.read_bytes()), "documents": {}}
    for domain in release["domains"]:
        for relative, _ in domain["documents"].values():
            inputs["documents"][relative] = _sha((root / relative).read_bytes())
    tools = {path.name: _sha(path.read_bytes())
             for path in sorted(Path(__file__).resolve().parent.glob("localization*.py"))}
    source_map_bytes = _json_bytes({"version": 1, "packages": source_map})
    completion = {
        "version": 1,
        "profile": PROFILE,
        "package_version": PACKAGE_VERSION,
        "linguistic_data": data,
        "profile_checker": {"kind": checker.kind, **identity},
        "inputs": inputs,
        "cooker": tools,
        "packages": [{k: v for k, v in p.items() if k != "data"} for p in packages],
        "bindings": [{k: v for k, v in b.items() if k != "data"} for b in bindings],
        "source_map": {"path": "source-map.json", "sha256": _sha(source_map_bytes)},
    }
    return {"release": release, "packages": packages, "bindings": bindings, "source_map": source_map_bytes,
            "completion": _json_bytes(completion), "items": items, "identity": identity, "statuses": statuses,
            "results": results}


def write_release(output: Path, cooked: dict, inputs: list[Path]) -> None:
    """Write every validated output, then the completion manifest last."""
    files = [(output / p["path"], p["data"]) for p in cooked["packages"]]
    files += [(output / b["path"], b["data"]) for b in cooked["bindings"]]
    files += [(output / "source-map.json", cooked["source_map"]), (output / "release.json", cooked["completion"])]
    resolved = {path.resolve() for path in inputs}
    if any(path.resolve() in resolved for path, _ in files):
        fail("release outputs must not overwrite authoring inputs")
    for path, data in files:
        write_atomic(path, data)


# ---------------------------------------------------------------------------
# Inspection.
# ---------------------------------------------------------------------------

def inspect_package(data: bytes) -> dict:
    package = decode_package(data)
    origins: dict[str, int] = {}
    for record in package["records"]:
        origins[record["origin"]] = origins.get(record["origin"], 0) + 1
    package["summary"] = {
        "messages": len(package["records"]),
        "static": sum(r["kind"] == "static" for r in package["records"]),
        "dynamic": sum(r["kind"] == "dynamic" for r in package["records"]),
        "origins": dict(sorted(origins.items())),
        "value_bytes": sum(len(r["value"].encode("utf-8")) for r in package["records"]),
        # The runtime keeps one owned copy of the package bytes per prepared catalog.
        "resident_estimate_bytes": package["bytes"],
    }
    return package


def inspect_release(path: Path) -> dict:
    completion = read_json(path)
    if completion.get("version") != 1 or completion.get("profile") != PROFILE:
        fail(f"{path}: unsupported release manifest")
    root = path.parent
    report = {"release": str(path), "packages": [], "problems": []}
    for entry in completion["packages"] + completion["bindings"] + [completion["source_map"]]:
        file = root / entry["path"]
        if not file.is_file() or _sha(file.read_bytes()) != entry["sha256"]:
            report["problems"].append(f"{entry['path']}: missing or does not match release.json")
    for entry in completion["packages"]:
        file = root / entry["path"]
        if not file.is_file():
            continue
        try:
            summary = inspect_package(file.read_bytes())["summary"]
        except CookError as error:
            report["problems"].append(f"{entry['path']}: {error}")
            continue
        report["packages"].append({"domain": entry["domain"], "locale": entry["locale"],
                                   "providers": entry["providers"], "bytes": entry["bytes"], **summary})
    return report


def _visible(text: str) -> str:
    """Show invisible and control characters explicitly, as inspectors must."""
    return json.dumps(text, ensure_ascii=True)


def format_package(package: dict) -> str:
    lines = [f"package v{package['version']} {package['profile']} {package['domain']} {package['locale']} "
             f"(source {package['source_locale']}, {package['linguistic_data']})",
             f"providers: {' -> '.join(package['providers'])}",
             f"bytes: {package['bytes']} (directory {package['directory_bytes']}, payload {package['payload_bytes']})",
             f"schema: {package['schema_sha256']}", ""]
    for record in package["records"]:
        flags = "required" if record["required"] else "optional"
        lines.append(f"{record['key']}  {record['kind']}  {record['origin']}  provider={record['provider']}  "
                     f"source_revision={record['source_revision']}  {flags}")
        for argument in record["arguments"]:
            lines.append(f"    argument {argument['name']}: {argument['type']} policy={argument['policy']} "
                         f"max_bytes={argument['max_bytes']} range=[{argument['min']}, {argument['max']}] "
                         f"values={argument['values']}")
        lines.append(f"    {_visible(record['value'])}")
    summary = package["summary"]
    lines += ["", f"summary: {summary['messages']} messages, {summary['static']} static, {summary['dynamic']} dynamic, "
              f"origins {summary['origins']}, value bytes {summary['value_bytes']}, "
              f"resident estimate {summary['resident_estimate_bytes']} bytes"]
    return "\n".join(lines) + "\n"


def format_release(report: dict) -> str:
    lines = [f"release {report['release']}"]
    for package in report["packages"]:
        lines.append(f"{package['domain']} {package['locale']}: {package['messages']} messages "
                     f"({package['static']} static, {package['dynamic']} dynamic), origins {package['origins']}, "
                     f"providers {' -> '.join(package['providers'])}, {package['bytes']} bytes")
    lines += [f"problem: {problem}" for problem in report["problems"]]
    return "\n".join(lines) + "\n"


def run_cook_release(args) -> int:
    namespaces = {}
    for binding in args.bindings or []:
        domain, separator, namespace = binding.partition("=")
        if not separator or domain in namespaces:
            fail(f"--bindings expects distinct DOMAIN=NAMESPACE pairs, got {binding!r}")
        namespaces[domain] = namespace
    if (args.profile_checker is None) == (args.profile_results is None):
        fail("choose exactly one of --profile-checker or --profile-results")
    if args.record_profile_results is not None and args.profile_checker is None:
        fail("--record-profile-results requires --profile-checker")
    checker = NativeProfileChecker(args.profile_checker) if args.profile_checker else \
        RecordedProfileChecker(args.profile_results)
    cooked = cook_release(args.manifest, checker, namespaces=namespaces)
    inputs = [args.manifest] + [args.manifest.parent / relative
                                for relative in json.loads(cooked["completion"])["inputs"]["documents"]]
    recorded = None
    if args.record_profile_results is not None:
        recorded = recording(cooked["identity"], cooked["statuses"], cooked["items"], cooked["results"])
        if args.record_profile_results.resolve() in {path.resolve() for path in inputs}:
            fail("--record-profile-results must not overwrite authoring inputs")
    write_release(args.output, cooked, inputs)
    if recorded is not None:
        write_atomic(args.record_profile_results, recorded)
    return 0


def run_inspect(args) -> int:
    """Dump a package, or verify and summarize a release directory's release.json."""
    if args.path.suffix == ".json":
        report = inspect_release(args.path)
        text = format_release(report)
    else:
        if args.path.stat().st_size > MAX_PACKAGE_BYTES:
            fail(f"{args.path}: package exceeds {MAX_PACKAGE_BYTES} bytes")
        report = inspect_package(args.path.read_bytes())
        text = format_package(report)
    sys.stdout.write(json.dumps(report, ensure_ascii=False, indent=2) + "\n" if args.json else text)
    return 1 if report.get("problems") else 0
