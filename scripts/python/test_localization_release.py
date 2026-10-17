"""Tests for the L1 `ludus-icu-mf1-v1` release cooker, descriptors and inspector.

Ordinary runs replay the committed, recorded pinned-ICU verdicts. With
LUDUS_LOCALIZATION_PROFILE_CHECKER naming a built ludus_localization_profile,
the native tests also prove those recordings are current and check the ICU
profile walk directly (ctest -R ludus_localization_profile).
"""
from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import localization  # noqa: E402
from localization import CookError  # noqa: E402
from localization_corpus import source_digest  # noqa: E402
import localization_release as release  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "modules/localization/corpus"
CORPUS_RESULTS = ROOT / "modules/localization/tests/corpus-profile-results.json"
FIXTURE = ROOT / "modules/localization/tests/release"
FIXTURE_RESULTS = FIXTURE / "profile-results.json"
NATIVE = os.environ.get("LUDUS_LOCALIZATION_PROFILE_CHECKER")
IDENTITY = {"protocol": 1, "icu": "78.3", "cldr": "48", "unicode": "17.0", "data_bytes": 1,
            "data_fingerprint": "0" * 32}


class TableChecker:
    """Test double returning scripted verdicts; it never interprets pattern syntax."""

    kind = "test"

    def __init__(self, verdicts=None, statuses=None, identity=None, fallback=None):
        self.verdicts = verdicts or {}
        self.statuses = statuses or {}
        self.identity = identity or IDENTITY
        self.fallback = fallback

    def check(self, locales, items):
        results = []
        for item in items:
            key = (item["locale"], item["key"])
            if key in self.verdicts:
                results.append(self.verdicts[key])
            elif self.fallback is not None:
                results.append(self.fallback(item))
            else:
                raise AssertionError(f"unscripted item {key}")
        return self.identity, {tag: self.statuses.get(tag, "ok") for tag in locales}, results


def recorded_with(overrides: dict) -> TableChecker:
    """Replay the committed fixture verdicts, overriding selected (locale, key) items."""
    recorded = release.RecordedProfileChecker(FIXTURE_RESULTS)

    def replay(item):
        return recorded.check([item["locale"]], [item])[2][0]

    return TableChecker(overrides, fallback=replay, identity=json.loads(FIXTURE_RESULTS.read_text())["checker"])


def cook_fixture(checker=None, manifest=FIXTURE / "release.json", **options):
    return release.cook_release(manifest, checker or release.RecordedProfileChecker(FIXTURE_RESULTS), **options)


def decoded(cooked: dict, locale: str) -> dict:
    package = next(p for p in cooked["packages"] if p["locale"] == locale)
    return {r["key"]: r for r in release.decode_package(package["data"])["records"]}


class CopiedFixture:
    """A writable copy of the fixture release for negative edits."""

    def __enter__(self) -> Path:
        self.directory = tempfile.TemporaryDirectory()
        root = Path(self.directory.name) / "release"
        shutil.copytree(FIXTURE, root)
        return root

    def __exit__(self, *_) -> None:
        self.directory.cleanup()


def edit(path: Path, change) -> None:
    value = json.loads(path.read_text())
    change(value)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def message(document: dict, key: str) -> dict:
    return next(m for m in document["messages"] if m["key"] == key)


def run_cli(*arguments: str) -> tuple[int, str, str]:
    stdout, stderr = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        status = localization.main(list(arguments))
    return status, stdout.getvalue(), stderr.getvalue()


class ReleaseCookTests(unittest.TestCase):
    def test_static_text_is_the_icu_decoded_output(self):
        cooked = cook_fixture()
        source = decoded(cooked, "en-US")
        self.assertEqual(source["escape/braces"]["value"], "Press {jump} to jump.")
        self.assertEqual(source["escape/apostrophes"]["value"], "It's literal: {} and don't.")
        self.assertEqual(source["escape/unterminated"]["value"], "Open {to the end")
        self.assertEqual(source["unicode/mixed"]["value"], "العربية 日本語 😀 é")
        french = decoded(cooked, "fr-FR")
        self.assertEqual(french["escape/unterminated"]["value"], "Ouvrir {jusqu'à la fin")
        self.assertEqual(french["escape/apostrophes"]["value"], "C'est littéral : {} et l'apostrophe reste.")
        corpus = release.cook_release(CORPUS / "release.json", release.RecordedProfileChecker(CORPUS_RESULTS))
        self.assertEqual(decoded(corpus, "en-US")["literal/braces"]["value"],
                         "Type {name} to insert a name. Don't worry: it's literal.")
        self.assertEqual(decoded(corpus, "ja-JP")["menu/subtitle"]["value"], "")

    def test_dynamic_records_keep_pattern_and_complete_schema(self):
        cooked = cook_fixture()
        canadian = decoded(cooked, "fr-CA")
        greeting = canadian["greeting/name"]
        # The fr-CA translation omits {name}: the record stays dynamic under the source schema.
        self.assertEqual((greeting["kind"], greeting["value"]), ("dynamic", "Bonjour!"))
        self.assertEqual(greeting["arguments"], [{"name": "name", "type": "text", "policy": 1, "max_bytes": 64,
                                                  "min": 0, "max": 0, "values": []}])
        mood = canadian["mood/select"]
        self.assertEqual(mood["arguments"][0]["values"], ["calm", "tense"])
        self.assertEqual(mood["provider"], "fr-FR")
        self.assertIn("{mood, select,", mood["value"])
        schemas = {p["schema_sha256"] for p in cooked["packages"]}
        self.assertEqual(len(schemas), 1)

    def test_fallback_providers_and_origins_are_baked_per_record(self):
        canadian = decoded(cook_fixture(), "fr-CA")
        self.assertEqual((canadian["menu/start"]["origin"], canadian["menu/start"]["provider"]), ("fallback", "fr-FR"))
        self.assertEqual((canadian["menu/quit"]["origin"], canadian["menu/quit"]["provider"]),
                         ("source-fallback", "en-US"))
        self.assertEqual((canadian["menu/credits"]["origin"], canadian["menu/credits"]["value"]), ("translation", ""))
        # Approved status with a stale receipt is derived stale and never provides text.
        self.assertEqual(canadian["escape/braces"]["provider"], "fr-FR")
        self.assertFalse(canadian["menu/quit"]["required"])
        package = next(p for p in cook_fixture()["packages"] if p["locale"] == "fr-CA")
        self.assertEqual(package["providers"], ["fr-CA", "fr-FR", "en-US"])

    def test_outputs_are_deterministic_and_machine_independent(self):
        first, second = cook_fixture(namespaces={"test/l1": "game::l10n::test"}), \
            cook_fixture(namespaces={"test/l1": "game::l10n::test"})
        self.assertEqual(first["completion"], second["completion"])
        self.assertEqual(first["source_map"], second["source_map"])
        self.assertEqual([p["data"] for p in first["packages"]], [p["data"] for p in second["packages"]])
        with tempfile.TemporaryDirectory() as one, tempfile.TemporaryDirectory() as two:
            for directory in (one, two):
                release.write_release(Path(directory), first, [])
            names = sorted(str(p.relative_to(one)) for p in Path(one).rglob("*") if p.is_file())
            self.assertEqual(names, ["bindings/test/l1.hpp", "packages/test/l1/en-US.loc",
                                     "packages/test/l1/fr-CA.loc", "packages/test/l1/fr-FR.loc", "release.json",
                                     "source-map.json"])
            for name in names:
                self.assertEqual((Path(one) / name).read_bytes(), (Path(two) / name).read_bytes())
            text = (Path(one) / "release.json").read_text()
            self.assertNotIn(str(ROOT), text)
            self.assertNotIn(one, text)
        completion = json.loads(first["completion"])
        self.assertEqual(completion["profile_checker"]["kind"], "recorded")
        self.assertEqual(sorted(completion["inputs"]["documents"]),
                         ["test-l1.fr-CA.json", "test-l1.fr-FR.json", "test-l1.source.json"])
        provenance = json.loads(first["source_map"])["packages"]
        canadian = next(p for p in provenance if p["locale"] == "fr-CA")
        quit_entry = next(r for r in canadian["records"] if r["key"] == "menu/quit")
        self.assertEqual((quit_entry["document"], quit_entry["translation_revision"]), ("test-l1.source.json", None))

    def test_stale_and_reused_receipts_fail_required_messages(self):
        with CopiedFixture() as root:
            # A meaning-bearing context edit without a new receipt makes fr-FR stale.
            edit(root / "test-l1.source.json",
                 lambda value: message(value, "menu/start").update(context="Starts a new game from chapter one."))
            with self.assertRaisesRegex(CookError, "fr-FR menu/start: required message lacks an approved"):
                cook_fixture(recorded_with({}), root / "release.json")
        with CopiedFixture() as root:
            # Reusing the revision while changing wording is still detected through the digest.
            edit(root / "test-l1.source.json",
                 lambda value: message(value, "menu/credits").update(pattern="Credits and thanks"))
            with self.assertRaisesRegex(CookError, "menu/credits"):
                cook_fixture(recorded_with({}), root / "release.json")

    def test_recorded_verdicts_cover_only_exact_inputs(self):
        with CopiedFixture() as root:
            # A correctly re-reviewed wording edit still needs a fresh pinned-ICU verdict.
            edit(root / "test-l1.source.json", lambda value: message(value, "menu/start").update(pattern="Begin"))
            source = json.loads((root / "test-l1.source.json").read_text())
            digest = source_digest(source, message(source, "menu/start"))
            edit(root / "test-l1.fr-FR.json",
                 lambda value: message(value, "menu/start").update(reviewed_source_digest=digest))
            with self.assertRaisesRegex(CookError, "no recorded verdict for en-US menu/start"):
                cook_fixture(manifest=root / "release.json")

    def test_profile_rejections_and_checker_identity_fail_the_cook(self):
        with self.assertRaisesRegex(CookError, r"fr-FR hud/lives: pattern rejected: DeadPluralKeyword"):
            cook_fixture(recorded_with({("fr-FR", "hud/lives"): {"verdict": "DeadPluralKeyword"}}))
        stale_icu = dict(json.loads(FIXTURE_RESULTS.read_text())["checker"], icu="79.1")
        with self.assertRaisesRegex(CookError, "reports icu '79.1', manifest pins '78.3'"):
            cook_fixture(TableChecker(identity=stale_icu, fallback=recorded_with({}).fallback))
        missing = recorded_with({})
        missing.statuses = {"fr-CA": "missing-data"}
        with self.assertRaisesRegex(CookError, "fr-CA: profile checker has no usable linguistic data"):
            cook_fixture(missing)
        with self.assertRaisesRegex(CookError, "unexpected decoded-text shape"):
            cook_fixture(recorded_with({("en-US", "menu/start"): {"verdict": "Accepted"}}))

    def test_invalid_utf8_and_limits_fail_before_checking(self):
        with CopiedFixture() as root:
            path = root / "test-l1.source.json"
            path.write_text(path.read_text().replace('"Start"', '"St\\udc80art"'))
            with self.assertRaisesRegex(CookError, "invalid Unicode scalar"):
                cook_fixture(TableChecker(), root / "release.json")
        with CopiedFixture() as root:
            edit(root / "test-l1.source.json",
                 lambda value: message(value, "menu/start").update(pattern="x" * (16 * 1024 + 1)))
            with self.assertRaisesRegex(CookError, "exceeds 16384"):
                cook_fixture(TableChecker(), root / "release.json")

    def test_bindings_reject_undeclared_domains_and_shared_namespaces(self):
        with self.assertRaisesRegex(CookError, "undeclared domains"):
            cook_fixture(namespaces={"game/ui": "game::ui"})
        with self.assertRaisesRegex(CookError, "non-reserved"):
            cook_fixture(namespaces={"test/l1": "game::class"})


class DescriptorTests(unittest.TestCase):
    def setUp(self):
        self.source = json.loads((FIXTURE / "test-l1.source.json").read_text())

    def test_descriptors_carry_exact_keys_and_schemas(self):
        header = release.descriptors(self.source, "game::l10n::test")
        self.assertIn('inline constexpr ludus::localization::MessageDescriptor kMenuStart{{"test/l1", "menu/start"}, {}};',
                      header)
        self.assertIn('inline constexpr std::string_view kMoodSelectMoodValues[] = {"calm", "tense"};', header)
        self.assertIn('{.Name = "name", .Type = ludus::localization::ArgumentType::Text, '
                      '.Origin = ludus::localization::TextOrigin::User, '
                      '.Unknown = ludus::localization::UnknownPolicy::Reject, .MaxBytes = 64, .Minimum = 0, '
                      '.Maximum = 0, .Values = {}}', header)
        self.assertIn(".MaxBytes = 0, .Minimum = 0, .Maximum = 99, .Values = {}}", header)
        self.assertIn(".Unknown = ludus::localization::UnknownPolicy::Other, .MaxBytes = 0, .Minimum = 0, "
                      ".Maximum = 0, .Values = kMoodSelectMoodValues}", header)

    def test_only_schema_edits_change_the_header(self):
        header = release.descriptors(self.source, "game::l10n::test")
        message(self.source, "menu/start").update(pattern="Begin", context="Changed context.", source_revision=9)
        self.assertEqual(release.descriptors(self.source, "game::l10n::test"), header)
        message(self.source, "hud/lives")["arguments"][0]["max"] = 100
        self.assertNotEqual(release.descriptors(self.source, "game::l10n::test"), header)

    def test_combined_native_and_browser_fixture_has_unique_prerequisites(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "fixture.hpp"
            subprocess.run([sys.executable, str(ROOT / "modules/localization/tests/generate_fixture.py"),
                            str(output)], check=True, capture_output=True, text=True)
            header = output.read_text()
        includes = [line for line in header.splitlines() if line.startswith("#include ")]
        self.assertEqual(len(includes), len(set(includes)))
        self.assertIn("#include <ludus/localization/catalog.hpp>", includes)
        self.assertIn("#include <string_view>", includes)
        self.assertEqual(header.count("#pragma once"), 1)
        for namespace in ("localization_fixture", "localization_corpus", "localization_release_fixture"):
            self.assertIn(f"namespace {namespace}\n", header)
        for name in ("kSourceCatalog", "kFrenchCatalog", "kFallbackCatalog", "kCorpusFrFr", "kReleaseFrCa"):
            self.assertIn(f" {name}[]", header)

    def test_generated_symbol_collisions_fail(self):
        base = dict(self.source)
        record = message(self.source, "menu/start")
        base["messages"] = [record, dict(record, key="menu-start")]
        with self.assertRaisesRegex(CookError, "generated symbol collision kMenuStart"):
            release.descriptors(base, "game::ui")
        # A message's argument table name may collide with another message's descriptor.
        lives = dict(message(self.source, "hud/lives"), key="menu/start")
        base["messages"] = [lives, dict(record, key="menu/start-arguments")]
        with self.assertRaisesRegex(CookError, "kMenuStartArguments"):
            release.descriptors(base, "game::ui")
        base["messages"] = [record, dict(record, key="menu/other", symbol="MenuStart")]
        with self.assertRaisesRegex(CookError, "collision"):
            release.descriptors(base, "game::ui")


class PackageFormatTests(unittest.TestCase):
    def setUp(self):
        cooked = cook_fixture()
        self.package = next(p for p in cooked["packages"] if p["locale"] == "fr-CA")["data"]

    def test_round_trip_and_summary(self):
        package = release.inspect_package(self.package)
        self.assertEqual(package["linguistic_data"], "icu4c-78.3/cldr-48/unicode-17.0")
        self.assertEqual(package["summary"]["messages"], 10)
        self.assertEqual(package["summary"]["dynamic"], 3)
        self.assertEqual(package["summary"]["origins"], {"fallback": 7, "source-fallback": 1, "translation": 2})
        self.assertEqual(package["bytes"], len(self.package))

    def test_every_truncation_fails(self):
        for size in range(len(self.package)):
            with self.assertRaises(CookError, msg=f"truncated to {size}"):
                release.decode_package(self.package[:size])

    def test_targeted_corruptions_fail(self):
        def corrupt(offset: int, value: int) -> bytes:
            data = bytearray(self.package)
            struct.pack_into("<I", data, offset, value)
            return bytes(data)

        for offset, value in ((4, 3), (28, 20), (32, 1), (44, 32), (56, 1), (36, 9), (8, len(self.package) + 1)):
            with self.assertRaises(CookError, msg=f"header word {offset}"):
                release.decode_package(corrupt(offset, value))
        records = release.HEADER_BYTES + sum(struct.unpack_from("<I", self.package, o)[0] for o in (16, 20, 24, 52))
        records += struct.unpack_from("<I", self.package, 36)[0] * release.SLICE_BYTES
        for field, value in ((16, 2), (20, 7), (32, 0), (36, 2), (4, 129)):
            with self.assertRaises(CookError, msg=f"record field {field}"):
                release.decode_package(corrupt(records + field, value))
        with self.assertRaises(CookError):
            release.decode_package(self.package + b"\0")

    def test_single_byte_mutations_fail_or_decode(self):
        data = bytearray(self.package)
        for index in range(len(data)):
            data[index] ^= 0xa5
            try:
                release.decode_package(bytes(data))
            except CookError:
                pass
            data[index] ^= 0xa5


class CommandLineTests(unittest.TestCase):
    def test_cook_release_inspect_and_preserve_previous_outputs(self):
        with CopiedFixture() as root, tempfile.TemporaryDirectory() as output:
            status, _, error = run_cli("cook-release", "--manifest", str(root / "release.json"), "--output", output,
                                       "--profile-results", str(FIXTURE_RESULTS), "--bindings",
                                       "test/l1=game::l10n::test")
            self.assertEqual(status, 0, error)
            package = Path(output) / "packages/test/l1/fr-CA.loc"
            status, text, _ = run_cli("inspect", str(package))
            self.assertEqual(status, 0)
            self.assertIn("providers: fr-CA -> fr-FR -> en-US", text)
            self.assertIn("menu/start  static  fallback  provider=fr-FR", text)
            self.assertIn('"Ouvrir {jusqu\'\\u00e0 la fin"', text)
            status, text, _ = run_cli("inspect", str(package), "--json")
            self.assertEqual(json.loads(text)["locale"], "fr-CA")
            status, text, _ = run_cli("inspect", str(Path(output) / "release.json"))
            self.assertEqual(status, 0, text)
            self.assertIn("test/l1 fr-CA: 10 messages (7 static, 3 dynamic)", text)
            before = {p: p.read_bytes() for p in Path(output).rglob("*") if p.is_file()}
            # A failing cook writes nothing and leaves the previous release intact.
            edit(root / "test-l1.fr-FR.json", lambda value: value.update(domain="test/other"))
            status, _, error = run_cli("cook-release", "--manifest", str(root / "release.json"), "--output", output,
                                       "--profile-results", str(FIXTURE_RESULTS))
            self.assertEqual(status, 1)
            self.assertIn("domain disagrees with source", error)
            self.assertEqual({p: p.read_bytes() for p in Path(output).rglob("*") if p.is_file()}, before)
            package.write_bytes(package.read_bytes()[:-1] + b"!")
            status, text, _ = run_cli("inspect", str(Path(output) / "release.json"))
            self.assertEqual(status, 1)
            self.assertIn("packages/test/l1/fr-CA.loc: missing or does not match release.json", text)

    def test_argument_errors(self):
        manifest = str(FIXTURE / "release.json")
        with tempfile.TemporaryDirectory() as output:
            for arguments, message_text in (
                    ((), "exactly one of --profile-checker or --profile-results"),
                    (("--profile-results", str(FIXTURE_RESULTS), "--profile-checker", "x"), "exactly one"),
                    (("--profile-results", str(FIXTURE_RESULTS), "--record-profile-results", "x.json"),
                     "requires --profile-checker"),
                    (("--profile-results", str(FIXTURE_RESULTS), "--bindings", "test/l1"), "DOMAIN=NAMESPACE"),
                    (("--profile-checker", str(Path(output) / "missing")), "does not exist")):
                status, _, error = run_cli("cook-release", "--manifest", manifest, "--output", output, *arguments)
                self.assertEqual(status, 1, arguments)
                self.assertIn(message_text, error)

    def test_outputs_never_overwrite_inputs(self):
        cooked = cook_fixture()
        with tempfile.TemporaryDirectory() as output:
            authored = Path(output) / "source-map.json"
            authored.write_text("authored")
            with self.assertRaisesRegex(CookError, "must not overwrite authoring inputs"):
                release.write_release(Path(output), cooked, [authored])
            self.assertEqual(authored.read_text(), "authored")
            self.assertEqual(sorted(p.name for p in Path(output).iterdir()), ["source-map.json"])


class ProtocolTests(unittest.TestCase):
    def test_request_encoding_and_malformed_responses(self):
        items = [{"locale": "en-US", "key": "a", "pattern": "x", "arguments": []}]
        request = release.encode_request(["en-US"], items)
        self.assertEqual(request[:8], struct.pack("<2I", release.REQUEST_MAGIC, 1))
        prefix = (struct.pack("<2I", release.RESPONSE_MAGIC, 1) + b"".join(
            struct.pack("<I", len(v)) + v for v in (b"78.3", b"48", b"17.0")) + struct.pack("<5I", 9, 1, 2, 3, 4) +
            struct.pack("<3I", 1, 0, 1))
        good = prefix + struct.pack("<3I", 0, 1, 1) + b"x"
        identity, statuses, results = release.decode_response(good, ["en-US"], items)
        self.assertEqual((identity["icu"], statuses, results), ("78.3", {"en-US": "ok"}, [{"verdict": "Accepted",
                                                                                            "text": "x"}]))
        for broken in (good[:-1], good + b"\0", good[:4] + struct.pack("<I", 2) + good[8:],
                       prefix + struct.pack("<3I", 99, 1, 1) + b"x", prefix + struct.pack("<3I", 0, 1, 1) + b"\xff",
                       prefix + struct.pack("<3I", 0, 0, 1) + b"x", prefix + struct.pack("<3I", 0, 2, 0)):
            with self.assertRaises(CookError):
                release.decode_response(broken, ["en-US"], items)


@unittest.skipUnless(NATIVE, "set LUDUS_LOCALIZATION_PROFILE_CHECKER to a built ludus_localization_profile")
class NativeCheckerTests(unittest.TestCase):
    def setUp(self):
        self.checker = release.NativeProfileChecker(Path(NATIVE))

    def test_recorded_verdicts_are_current(self):
        for manifest, recorded in ((CORPUS / "release.json", CORPUS_RESULTS), (FIXTURE / "release.json",
                                                                              FIXTURE_RESULTS)):
            cooked = release.cook_release(manifest, self.checker)
            regenerated = release.recording(cooked["identity"], cooked["statuses"], cooked["items"], cooked["results"])
            self.assertEqual(regenerated.decode("utf-8"), recorded.read_text(),
                             f"{recorded} is stale; rerun cook-release with --record-profile-results")
            replayed = release.cook_release(manifest, release.RecordedProfileChecker(recorded))
            self.assertEqual([p["data"] for p in cooked["packages"]], [p["data"] for p in replayed["packages"]])

    def test_profile_rejection_fixtures(self):
        source = json.loads((CORPUS / "game-ui.source.json").read_text())
        cases = json.loads((CORPUS / "profile-rejections.json").read_text())["cases"]
        locales = sorted({case["locale"] for case in cases})
        items = [{"locale": case["locale"], "key": case["key"], "pattern": case["pattern"],
                  "arguments": [release.normalize_argument(a) for a in message(source, case["key"])["arguments"]]}
                 for case in cases]
        oversized = dict(items[0], pattern="a" * (16 * 1024 + 1), arguments=[])
        _, statuses, results = self.checker.check(locales, items + [oversized])
        self.assertEqual(set(statuses.values()), {"ok"})
        self.assertEqual([r["verdict"] for r in results], [case["reason"] for case in cases] + ["PatternTooLarge"])

    def test_static_decoding_and_locale_data(self):
        patterns = {"Press '{'jump'}' to jump.": "Press {jump} to jump.", "It''s": "It's", "'{'": "{",
                    "a '{b": "a {b", "don't": "don't", "a''b": "a'b", "'{a}'": "{a}", "#": "#", "": ""}
        items = [{"locale": "en-US", "key": "k", "pattern": p, "arguments": []} for p in patterns]
        _, statuses, results = self.checker.check(["en-US", "zz", "fr-CA"], items)
        self.assertEqual(statuses, {"en-US": "ok", "zz": "missing-data", "fr-CA": "ok"})
        self.assertEqual([r.get("text") for r in results], list(patterns.values()))
        dynamic = [{"locale": "en-US", "key": "k", "pattern": "{n, plural, one {#} other {#}}",
                    "arguments": [release.normalize_argument({"name": "n", "type": "count", "min": 0, "max": 9})]},
                   {"locale": "en-US", "key": "k", "pattern": "{x}", "arguments": []}]
        _, _, results = self.checker.check(["en-US"], dynamic)
        self.assertEqual(results, [{"verdict": "Accepted"}, {"verdict": "UndeclaredArgument"}])

    def test_malformed_request_fails_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "request.bin").write_bytes(b"LPRQ\x01\x00\x00\x00")
            import subprocess
            completed = subprocess.run([NATIVE, str(Path(directory).resolve()), "request.bin", "response.bin"],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
            self.assertEqual(completed.returncode, 2)
            self.assertFalse((Path(directory) / "response.bin").exists())


if __name__ == "__main__":
    unittest.main()
