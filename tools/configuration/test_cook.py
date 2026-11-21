"""Cooker integration tests; supplied validator is the actual engine executable."""
import json
from pathlib import Path
import tempfile
import cook
import subprocess
import sys
import unittest

VALIDATOR = sys.argv.pop(1)

class NativeTests(unittest.TestCase):
    def test_schema_and_validate(self):
        schema = json.loads(subprocess.check_output([VALIDATOR, "--schema"]))
        self.assertEqual(schema["schema"], "ludus.host.v1")
        bundle = {"version": 1, "schema": schema["schema"], "layer": "project", "values": [
            {"key": "host.max_frames", "type": "uint64", "value": "18446744073709551615", "source": "test", "line": 0}]}
        result = subprocess.run([VALIDATOR, "--validate"], input=json.dumps(bundle).encode(), capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), bundle)
        bundle["values"][0]["value"] = "18446744073709551616"
        self.assertNotEqual(subprocess.run([VALIDATOR, "--validate"], input=json.dumps(bundle).encode(), capture_output=True).returncode, 0)

    @unittest.skipIf(sys.version_info < (3, 11), "optional TOML tool requires Python 3.11+")
    def test_toml_and_native_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "project.toml"
            source.write_text("[host]\nheadless = true\nmax_frames = 3\n", encoding="utf-8")
            first = cook.cook(source, Path(VALIDATOR), "project")
            self.assertEqual(first, cook.cook(source, Path(VALIDATOR), "project"))
            bundle = json.loads(first)
            self.assertEqual(bundle["values"][1]["value"], "3")
            for bad in ("[host]\nheadless=1", "[host]\nunknown=3", "[host]\nheadless=true\nheadless=false",
                        '"host.max_frames"=1\n[host]\nmax_frames=2', "[host]\nmax_frames=-1", "[host]\nmax_frames=18446744073709551616"):
                source.write_text(bad, encoding="utf-8")
                with self.assertRaises(ValueError):
                    cook.cook(source, Path(VALIDATOR), "project")

    @unittest.skipIf(sys.version_info < (3, 11), "optional TOML tool requires Python 3.11+")
    def test_failed_cook_preserves_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.toml"
            output = Path(directory) / "output.json"
            source.write_text("[host]\nheadless=1", encoding="utf-8")
            output.write_bytes(b"original")
            result = subprocess.run([sys.executable, str(Path(cook.__file__)), str(source), str(output),
                "--validator", VALIDATOR], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"original")

@unittest.skipIf(sys.version_info < (3, 11), "optional TOML tool requires Python 3.11+")
class StartupManifestTests(unittest.TestCase):
    MANIFEST = """version = 1
engine = ["frames.toml", "headless.toml"]
project = ["project.toml"]

[profiles.preview]
files = ["profile.toml"]
devices = ["portable"]
qualities = ["low"]

[devices]
portable = ["device.toml"]
other = ["unused.toml"]

[qualities]
low = ["quality.toml"]
high = ["unused.toml"]
"""

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.manifest = self.root / "startup.toml"
        self.output = self.root / "startup.json"
        self.manifest.write_text(self.MANIFEST, encoding="utf-8")
        for index, filename in enumerate(("frames", "project", "profile", "device", "quality"), 1):
            (self.root / f"{filename}.toml").write_text(f"[host]\nmax_frames = {index}\n", encoding="utf-8")
        (self.root / "headless.toml").write_text("[host]\nheadless = true\n", encoding="utf-8")

    def run_cooker(self, *selection):
        return subprocess.run(
            [sys.executable, str(Path(cook.__file__).resolve()), str(self.manifest), str(self.output),
             "--validator", str(Path(VALIDATOR).resolve()), *selection],
            cwd=self.root.parent, capture_output=True, timeout=15,
        )

    def test_selected_layers_are_canonical_and_independent_of_input_order(self):
        result = self.run_cooker("--profile", "preview", "--device", "portable", "--quality", "low")
        self.assertEqual(result.returncode, 0, result.stderr)
        first = self.output.read_bytes()
        layers = json.loads(first)["layers"]
        self.assertEqual(list(layers), ["engine", "project", "profile", "device", "quality"])
        for index, (name, layer) in enumerate(layers.items(), 1):
            self.assertEqual(layer["layer"], name)
            self.assertEqual(layer["values"][-1]["value"], str(index))
        self.assertEqual([record["source"] for record in layers["engine"]["values"]],
                         ["headless.toml", "frames.toml"])
        self.manifest.write_text(self.MANIFEST.replace('"frames.toml", "headless.toml"',
                                                       '"headless.toml", "frames.toml"'), encoding="utf-8")
        result = self.run_cooker("--quality", "low", "--device", "portable", "--profile", "preview")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.output.read_bytes(), first)

    def test_unknown_incompatible_and_malformed_selections_preserve_output(self):
        self.output.write_bytes(b"previous")
        for selection, message in (
            (("missing", None, None), "unknown profile"),
            (("preview", "missing", None), "unknown device"),
            (("preview", "other", None), "incompatible"),
            (("preview", None, "high"), "incompatible"),
        ):
            with self.subTest(selection=selection):
                with self.assertRaisesRegex(ValueError, message):
                    cook.cook_startup(self.manifest, self.output, Path(VALIDATOR), *selection)

        for options, message in (
            (("--profile", "preview", "--device", "other"), b"incompatible"),
            (("--device", "portable"), b"require --profile"),
            (("--profile", "preview", "--profile", "missing"), b"at most one --profile"),
        ):
            with self.subTest(options=options):
                result = self.run_cooker(*options)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)
                self.assertEqual(self.output.read_bytes(), b"previous")

        for content, message in (
            (self.MANIFEST.replace("version = 1", "version = true"), "use version = 1"),
            (self.MANIFEST.replace('"portable"', '"unlisted"', 1), "corresponding manifest table"),
            (self.MANIFEST.replace("frames.toml", "../frames.toml"), "relative .toml paths"),
            ("#" * (cook.MAX_BYTES + 1), "TOML input exceeds"),
            (self.MANIFEST.replace('"frames.toml", "headless.toml"',
                                   ", ".join(f'"source-{i}.toml"' for i in range(cook.MAX_FILES_PER_LAYER + 1))),
             "at most"),
        ):
            with self.subTest(message=message):
                self.manifest.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(ValueError, message):
                    cook.cook_startup(self.manifest, self.output, Path(VALIDATOR), "preview", None, None)

    def test_same_rank_duplicates_and_masked_invalid_values_are_rejected(self):
        self.output.write_bytes(b"previous")
        source = self.root / "headless.toml"
        source.write_text("[host]\nmax_frames = 2\n", encoding="utf-8")
        result = self.run_cooker("--profile", "preview")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"duplicate engine setting 'host.max_frames'", result.stderr)
        self.assertEqual(self.output.read_bytes(), b"previous")

        source.write_text("[host]\nheadless = true\n", encoding="utf-8")
        (self.root / "frames.toml").write_text("[host]\nmax_frames = -1\n", encoding="utf-8")
        result = self.run_cooker("--profile", "preview", "--quality", "low")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"engine layer", result.stderr)
        self.assertEqual(self.output.read_bytes(), b"previous")


if __name__ == "__main__":
    unittest.main()
