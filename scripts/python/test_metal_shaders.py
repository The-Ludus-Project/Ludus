"""Admission checks for Metal's independently reflected shader layout."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ludus_shader_driver", ROOT / "cmake/shaders/compile_shader.py")
DRIVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRIVER)


class MetalReflectionTests(unittest.TestCase):
    def size(self, data):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "reflection.json"
            path.write_text(json.dumps(data))
            return DRIVER.metal_uniform_size(path)

    def fixture(self):
        return {"parameters": [{"binding": {"kind": "constantBuffer", "index": 0},
                "type": {"kind": "constantBuffer", "elementVarLayout": {"binding": {"size": 48}}}}]}

    def test_empty_uniform_and_actual_metal_binding_category(self):
        self.assertEqual(self.size({"parameters": []}), 0)
        self.assertEqual(self.size(self.fixture()), 48)
        data = self.fixture()
        data["parameters"][0]["binding"]["kind"] = "descriptorTableSlot"
        with self.assertRaisesRegex(RuntimeError, "buffer 0"):
            self.size(data)

    def test_capacity_boundaries(self):
        for size in (1, 48, 16384):
            data = self.fixture()
            data["parameters"][0]["type"]["elementVarLayout"]["binding"]["size"] = size
            self.assertEqual(self.size(data), size)
        for size in (0, 16385):
            data = self.fixture()
            data["parameters"][0]["type"]["elementVarLayout"]["binding"]["size"] = size
            with self.assertRaisesRegex(RuntimeError, "bounds"):
                self.size(data)

    def test_extra_resources_and_nonzero_bindings_are_rejected(self):
        data = self.fixture()
        data["parameters"].append(copy.deepcopy(data["parameters"][0]))
        with self.assertRaisesRegex(RuntimeError, "one uniform"):
            self.size(data)
        for changes in ({"index": 1}, {"space": 1}, {"kind": "texture"}):
            data = self.fixture()
            data["parameters"][0]["binding"].update(changes)
            with self.assertRaisesRegex(RuntimeError, "buffer 0"):
                self.size(data)

    def test_host_archives_are_independently_pinned(self):
        pin = json.loads((ROOT / "config/shader_toolchain.json").read_text())["slang"]
        self.assertEqual(pin["version"], "2026.1.2")
        self.assertEqual(set(pin["macos"]), {"arm64", "x86_64"})
        for architecture, asset in pin["macos"].items():
            self.assertRegex(asset["sha256"], r"^[0-9a-f]{64}$")
            self.assertIn("macos-" + ("aarch64" if architecture == "arm64" else architecture), asset["url"])
            self.assertNotEqual(asset["sha256"], pin["sha256"])


FAKE_SLANG = '''#!/usr/bin/env python3
"""Stand-in slangc: emits Metal text naming its inputs by absolute path."""
import sys
arguments = sys.argv[1:]
source = arguments[0]
option = lambda flag: arguments[arguments.index(flag) + 1]
stage = option("-stage")
include = option("-I")
entry = option("-entry")
with open(option("-o"), "w") as artifact:
    artifact.write("#include <metal_stdlib>\\n#line 90 \\"core\\"\\nstruct Varying_0 { float4 position_0 [[position]]; };\\n"
                   f"#line 3 \\"{source}\\"\\nstruct Parameters_0 {{ float4 Output_0; }};\\n"
                   f"#line 7 \\"{include}/shared.slang\\"\\nfloat Shared_0(float x) {{ return x; }}\\n"
                   f"#line 15 \\"{source}\\"\\n[[{stage}]] float4 {entry}(Parameters_0 constant* parameters_0 [[buffer(0)]]) {{\\n"
                   "#line 16\\n    return float4(Shared_0(1.0));\\n}\\n")
with open(option("-reflection-json"), "w") as reflection:
    reflection.write('{"entryPoints": [{"stage": "%s"}], "parameters": [{"binding": {"kind": "constantBuffer", "index": 0},'
                     ' "type": {"kind": "constantBuffer", "elementVarLayout": {"binding": {"size": 16}}}}]}' % stage)
with open(option("-depfile"), "w") as depfile:
    depfile.write(f"{option('-o')}: {source} {include}/shared.slang\\n")
'''


class LineDirectiveTests(unittest.TestCase):
    """Generated Metal text must be identical in every checkout.

    A compiler cache shared between checkouts hashes preprocessed output with
    every line-start #line path relativized, so a header whose embedded MSL
    differs only by a checkout path pairs one checkout's literal with another
    checkout's folded length. The helpers name emitted files relative to the
    captured source and include directories instead.
    """

    def test_line_directive_files_are_named_relative_to_the_shortest_captured_root(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source_root = root / "checkout-a" / "shaders"
            include_root = root / "checkout-a" / "math"
            code = (f'#line 90 "core"\n#line 5044 "core.meta.slang"\n#line 3 "{source_root}/output.slang"\n'
                    f'  #line 7 "{include_root}/nested/random.slang"\n#line 12 "{source_root}/sub/../output.slang"\n'
                    f'const char* text = "#line 1 \\"{root}/elsewhere.slang\\"";\n')
            rewritten = DRIVER.relative_line_directives(code, [source_root, str(include_root)])
            self.assertEqual(rewritten, ('#line 90 "core"\n#line 5044 "core.meta.slang"\n#line 3 "output.slang"\n'
                                         '  #line 7 "nested/random.slang"\n#line 12 "output.slang"\n'
                                         f'const char* text = "#line 1 \\"{root}/elsewhere.slang\\"";\n'))

    def test_line_directive_outside_the_captured_roots_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            code = f'#line 3 "{root}/other/shared.slang"\n'
            with self.assertRaisesRegex(RuntimeError, "outside the captured source and include directories"):
                DRIVER.relative_line_directives(code, [root / "shaders"])

    def test_checkout_directory_left_elsewhere_in_the_text_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve() / "shaders"
            code = f'#line 3 "{root}/output.slang"\n// compiled from {root}/output.slang\n'
            with self.assertRaisesRegex(RuntimeError, "still names the checkout directory"):
                DRIVER.relative_line_directives(code, [root])

    def test_metal_headers_and_manifests_are_identical_across_checkout_paths(self):
        results = []
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            compiler = root / "slangc"
            compiler.write_text(FAKE_SLANG)
            compiler.chmod(0o755)
            for checkout in ("checkout-a", "checkout-with-a-longer-name-b"):
                source_dir = root / checkout / "shaders"
                include_dir = root / checkout / "math"
                output = root / checkout / "out"
                for directory in (source_dir, include_dir):
                    directory.mkdir(parents=True)
                (source_dir / "fullscreen.slang").write_text("// source\n")
                arguments = argparse.Namespace(source=str(source_dir / "fullscreen.slang"), output=str(output), name="fullscreen",
                                               compiler=str(compiler), include=[str(include_dir)], define=[],
                                               vertex="vertexMain", fragment="fragmentMain")
                DRIVER.compile_metal(arguments, {"slang": {"version": "test"}})
                header = (output / "fullscreen.h").read_text()
                manifest = json.loads((output / "manifest.json").read_text())
                self.assertNotIn(str(root), header)
                self.assertIn('#line 3 "fullscreen.slang"', header)
                self.assertIn('#line 7 "shared.slang"', header)
                self.assertNotIn(str(root), (output / "fullscreen.fragment.metal").read_text())
                self.assertIn(str(root / checkout), manifest["commands"][0][1])
                results.append((header, manifest["sha256"]))
        self.assertEqual(results[0], results[1])


if __name__ == "__main__":
    unittest.main()
