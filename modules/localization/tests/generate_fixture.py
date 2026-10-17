"""Generate native/wasm test bytes through the production cookers, not a mock codec.

Version 1 fixtures come from the literal cooker. Version 2 fixtures come from the
release cooker replaying the pinned ICU checker's recorded verdicts, so ordinary
builds need no ICU; the ICU-enabled test proves those recordings are current.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts/python"))
from localization import bindings, cook, document, export_review, read_json, write_atomic  # noqa: E402
from localization_release import RecordedProfileChecker, cook_release  # noqa: E402

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
for prefix, manifest, results, domain, namespace in releases:
    cooked = cook_release(manifest, RecordedProfileChecker(results), namespaces={domain: namespace})
    append_bindings(cooked["bindings"][0]["data"].decode("utf-8"))
    for package in cooked["packages"]:
        emit(prefix + "".join(part.capitalize() for part in package["locale"].split("-")), package["data"])
write_atomic(Path(sys.argv[1]), ("#pragma once\n\n" + "\n".join(includes) + "\n\n" + "\n".join(lines)).encode("utf-8"))
