import importlib.machinery
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import wave
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "examples/audio-content"))
from generate_fixtures import generate
from generate_transition_fixture import generate as generate_score
loader = importlib.machinery.SourceFileLoader("package_audio", str(ROOT / "scripts/package-audio-content"))
spec = importlib.util.spec_from_loader(loader.name, loader)
pack = importlib.util.module_from_spec(spec)
loader.exec_module(pack)
VALIDATOR = sys.argv.pop(1)


class PackageTests(unittest.TestCase):
    def test_dynamic_dependencies_and_capacity_preserve_last_good_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            content = Path(temporary) / "content"
            output = Path(temporary) / "release"
            generate_score(content)
            # Add a resident stinger and a third source; only the stinger references it.
            bank = Path(temporary) / "bank"
            generate(bank)
            catalog = json.loads((content / "catalog.json").read_text())
            for item in json.loads((bank / "catalog.json").read_text())["resources"]:
                if item["id"] in ("source/impact", "sound/impact"):
                    (content / item["path"]).write_bytes((bank / item["path"]).read_bytes())
                    catalog["resources"].append(item)
            (content / "catalog.json").write_text(json.dumps(catalog))
            path = content / "journey.json"
            score = json.loads(path.read_text())
            score["max_stingers"] = 1
            score["stingers"] = [{"id": "win", "sound": "sound/impact", "quantize": "bar"}]
            path.write_text(json.dumps(score))
            published = pack.package(VALIDATOR, content, output, ["music/journey"])
            self.assertTrue((published / "impact.wav").is_file())
            self.assertTrue((published / "arrive.wav").is_file())
            last_good = (output / "current.json").read_bytes()
            layers = score["sections"][0]["layers"]
            layers.append(layers[0])  # five streams including the prepared destination
            path.write_text(json.dumps(score))
            with self.assertRaises(subprocess.CalledProcessError):
                pack.package(VALIDATOR, content, output, ["music/journey"])
            layers.pop()
            # Two stems in one section must have equal lengths and sample rates.
            layers[1]["source"] = "source/arrive"
            path.write_text(json.dumps(score))
            source = content / "arrive.wav"
            with wave.open(str(source), "rb") as original:
                samples = original.readframes(original.getnframes())
            for rate, data in ((44100, samples), (48000, samples[:-4])):
                with self.subTest(rate=rate), wave.open(str(source), "wb") as changed:
                    changed.setparams((2, 2, rate, 0, "NONE", "not compressed"))
                    changed.writeframes(data)
                with self.assertRaises(subprocess.CalledProcessError):
                    pack.package(VALIDATOR, content, output, ["music/journey"])
            self.assertEqual(last_good, (output / "current.json").read_bytes())

    def test_closure_reproducibility_and_last_good(self):
        with tempfile.TemporaryDirectory() as temporary:
            content = Path(temporary) / "content"
            output = Path(temporary) / "release"
            generate(content)
            (content / "unused.wav").write_bytes(b"unused asset")
            roots = ["sound/impact", "music/theme"]
            first = pack.package(VALIDATOR, content, output, roots)
            pointer = (output / "current.json").read_bytes()
            self.assertFalse((first / "unused.wav").exists())
            self.assertEqual(first, pack.package(VALIDATOR, content, output, list(reversed(roots))))
            subprocess.run([str(Path(VALIDATOR).parent.parent / "audio_content_demo/ludus_audio_content_demo"), "--offline", str(first)], check=True)
            (content / "theme.wav").unlink()
            with self.assertRaises(subprocess.CalledProcessError):
                pack.package(VALIDATOR, content, output, roots)
            self.assertEqual(pointer, (output / "current.json").read_bytes())
            self.assertTrue((first / "theme.wav").exists())

    def test_source_edit_and_interrupted_publish_preserve_last_good(self):
        with tempfile.TemporaryDirectory() as temporary:
            content = Path(temporary) / "content"
            output = Path(temporary) / "release"
            generate(content)
            roots = ["music/theme"]
            first = pack.package(VALIDATOR, content, output, roots)
            pointer = (output / "current.json").read_bytes()
            original = pack.fingerprint
            def changed(root_fd, path, destination=None):
                if destination and path == "theme.wav":
                    with open(content / path, "ab") as target:
                        target.write(b"changed during packaging")
                return original(root_fd, path, destination)
            with patch.object(pack, "fingerprint", changed):
                with self.assertRaises(ValueError):
                    pack.package(VALIDATOR, content, output, roots)
            self.assertEqual(pointer, (output / "current.json").read_bytes())
            generate(content)
            with patch.object(pack.os, "replace", side_effect=OSError("interrupted publication")):
                with self.assertRaises(OSError):
                    pack.package(VALIDATOR, content, output, roots)
            self.assertEqual(pointer, (output / "current.json").read_bytes())
            self.assertTrue((first / "theme.wav").exists())

    def test_symlink_and_unsupported_definition_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            content = Path(temporary) / "content"
            generate(content)
            (content / "theme.wav").unlink()
            (content / "theme.wav").symlink_to("/etc/passwd")
            with self.assertRaises(subprocess.CalledProcessError):
                pack.package(VALIDATOR, content, Path(temporary) / "release", ["music/theme"])
            document = json.loads((content / "impact.json").read_text())
            document["version"] = 2
            (content / "impact.json").write_text(json.dumps(document))
            with self.assertRaises(subprocess.CalledProcessError):
                pack.package(VALIDATOR, content, Path(temporary) / "release", ["sound/impact"])


if __name__ == "__main__":
    unittest.main()
