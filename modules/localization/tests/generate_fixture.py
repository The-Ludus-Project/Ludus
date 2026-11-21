"""Generate native/wasm test bytes through the production cookers, not a mock codec.

Version 1 fixtures come from the literal cooker. Version 2 fixtures come from the
release cooker replaying the pinned ICU checker's recorded verdicts, so ordinary
builds need no ICU; the ICU-enabled test proves those recordings are current.
"""
from pathlib import Path
import json
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/python"))
from localization import bindings, cook, document, export_review, read_json, write_atomic  # noqa: E402
from localization_release import (KIND_DYNAMIC, KIND_STATIC, RecordedProfileChecker, _pascal,  # noqa: E402
                                  cook_release, decode_package, encode_package)

source = document(read_json(Path(__file__).parent / "source.json"), False)
translation = export_review(source, "fr-FR")
translated = {"empty": "", "menu/play": "Jouer", "literal": "L'apostrophe {player} reste littérale.\n"}
for message in translation["messages"]:
    message["text"] = translated.get(message["key"], message["source_text"])
    message["status"] = "approved"
fallback = export_review(source, "ja-JP")
lines: list[str] = []
includes: list[str] = []


def append_bindings(header: str) -> None:
    # Each cooker returns a standalone header. Combine their prerequisites once
    # at the top so the assembled fixture remains clean under clang-tidy too.
    for line in header.splitlines():
        if line == "#pragma once":
            continue
        if line.startswith("#include "):
            if line not in includes:
                includes.append(line)
        else:
            lines.append(line)


append_bindings(bindings(source, "localization_fixture"))


def emit(name: str, value: bytes) -> None:
    lines.append(f"inline constexpr ludus::foundation::uint8 {name}[] = {{")
    for start in range(0, len(value), 16):
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in value[start:start + 16]) + ",")
    lines.append("};\n")


for name, value in (("kSourceCatalog", cook(source)), ("kFrenchCatalog", cook(source, translation)),
                    ("kFallbackCatalog", cook(source, fallback, allow_source_fallback=True))):
    emit(name, value)

TESTS = Path(__file__).resolve().parent
# Recorded verdicts live beside the tests, not in corpus/, whose bytes the L0 probe fingerprints.
releases = (
    ("kCorpus", ROOT / "modules/localization/corpus/release.json", TESTS / "corpus-profile-results.json",
     "game/ui", "localization_corpus"),
    ("kRelease", TESTS / "release/release.json", TESTS / "release/profile-results.json", "test/l1",
     "localization_release_fixture"),
)
packages: dict[str, list[tuple[str, str]]] = {}
for prefix, manifest, results, domain, namespace in releases:
    cooked = cook_release(manifest, RecordedProfileChecker(results), namespaces={domain: namespace})
    append_bindings(cooked["bindings"][0]["data"].decode("utf-8"))
    for package in cooked["packages"]:
        name = prefix + "".join(part.capitalize() for part in package["locale"].split("-"))
        emit(name, package["data"])
        packages.setdefault(prefix, []).append((package["locale"], name))

# A second verified release of the same domain and locale with changed wording.
# Both strings have recorded verdicts for the same locale and empty schema.
output_path = Path(sys.argv[1])
output_path.parent.mkdir(parents=True, exist_ok=True)
with TemporaryDirectory(dir=output_path.parent) as directory:
    reload_root = Path(directory)
    for name in ("release.json", "test-l1.source.json", "test-l1.fr-CA.json", "test-l1.fr-FR.json"):
        data = read_json(TESTS / "release" / name)
        if name == "test-l1.fr-FR.json":
            messages = {message["key"]: message for message in data["messages"]}
            start = messages["menu/start"]
            start["pattern"] = messages["menu/credits"]["pattern"]
            start["translation_revision"] += 1
        (reload_root / name).write_text(json.dumps(data), encoding="utf-8")
    checker = RecordedProfileChecker(TESTS / "release/profile-results.json")
    reloaded = cook_release(reload_root / "release.json", checker)
    french = [package["data"] for package in reloaded["packages"] if package["locale"] == "fr-FR"]
    if len(french) != 1:
        raise ValueError("expected exactly one reloaded French package")
    emit("kReloadedReleaseFrFr", french[0])

# Encode a valid v2 package with incompatible arguments on a message hidden from the reload view.
decoded = decode_package(french[0])
providers = decoded["providers"]
records = decoded["records"]
for record in records:
    record["kind"] = KIND_STATIC if record["kind"] == "static" else KIND_DYNAMIC
    record["value"] = record["value"].encode("utf-8")
    record["provider"] = providers.index(record["provider"])
    if record["key"] == "hud/lives":
        record["arguments"][0]["max"] -= 1
emit("kIncompatibleReleaseFrFr", encode_package(decoded["domain"], decoded["locale"], decoded["source_locale"],
                                                 providers, decoded["linguistic_data"], records))


def literal(text: str) -> str:
    """Emit the UTF-8 bytes as a portable C++ string_view literal."""
    data = text.encode("utf-8")
    if len(data) == 0:
        # An empty (pointer, length) pair trips clang-tidy bugprone-string-constructor.
        return "std::string_view{}"
    parts = []
    for byte in data:
        character = chr(byte)
        is_printable = 0x20 <= byte < 0x7f and character not in '"\\?'
        # MSVC warns when a decimal digit immediately follows an octal escape,
        # even after three octal digits. Start a new literal at that boundary.
        if is_printable and character.isdecimal() and parts and parts[-1].startswith("\\"):
            parts.append('""')
        parts.append(character if is_printable else f"\\{byte:03o}")
    body = "".join(parts)
    return f'std::string_view{{"{body}", {len(data)}}}'


def argument_value(name: str, value: object) -> str:
    qualified = "ludus::localization"
    if isinstance(value, bool):
        raise ValueError(f"{name}: boolean corpus arguments are not representable")
    if isinstance(value, int):
        return f"{{{literal(name)}, {qualified}::ArgumentValueKind::Integer, {value}, 0.0, {{}}}}"
    if isinstance(value, float):
        return f"{{{literal(name)}, {qualified}::ArgumentValueKind::Number, 0, {value!r}, {{}}}}"
    return f"{{{literal(name)}, {qualified}::ArgumentValueKind::Text, 0, 0.0, {literal(value)}}}"


# The L0/L1 corpus expectations as typed L2 format cases over the cooked packages.
corpus_root = ROOT / "modules/localization/corpus"
expectations = read_json(corpus_root / "expectations.json")
corpus_source = read_json(corpus_root / "game-ui.source.json")
symbols = {m["key"]: "k" + m.get("symbol", _pascal(m["key"], "/-")) for m in corpus_source["messages"]}
lines += ["namespace localization_corpus_cases", "{",
          "/// One corpus expectation: a formatted output or InvalidArguments.",
          "struct FormatCase", "{", "    std::string_view Locale;",
          "    const ludus::localization::MessageDescriptor* Descriptor;",
          "    std::span<const ludus::localization::ArgumentValue> Arguments;", "    bool IsFormatted;",
          "    std::string_view Expected;", "};", "",
          "/// One cooked corpus package and its locale.", "struct CorpusPackage", "{",
          "    std::string_view Locale;", "    std::span<const ludus::foundation::uint8> Bytes;", "};",
          "inline constexpr CorpusPackage kCorpusPackages[] = {",
          *[f"    {{{literal(locale)}, {name}}}," for locale, name in packages["kCorpus"]], "};", ""]
rows = []
for index, case in enumerate(expectations["cases"]):
    arguments = case["arguments"]
    span = "{}"
    if arguments:
        lines.append(f"inline constexpr ludus::localization::ArgumentValue kCase{index}Arguments[] = {{")
        lines += [f"    {argument_value(name, value)}," for name, value in arguments.items()]
        lines.append("};")
        span = f"kCase{index}Arguments"
    is_formatted = "expected" in case
    expected = literal(case["expected"]) if is_formatted else "{}"
    rows.append(f"    {{{literal(case['locale'])}, &localization_corpus::{symbols[case['key']]}, {span}, "
                f"{'true' if is_formatted else 'false'}, {expected}}},")
lines += ["inline constexpr FormatCase kFormatCases[] = {", *rows, "};", "} // namespace localization_corpus_cases", ""]
write_atomic(Path(sys.argv[1]), ("#pragma once\n\n" + "\n".join(includes) + "\n\n" + "\n".join(lines)).encode("utf-8"))
