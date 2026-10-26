"""Admission checks for the DXIL shader artifacts compiled on Windows hosts."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import sys
import copy
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ludus_shader_driver", ROOT / "cmake/shaders/compile_shader.py")
DRIVER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DRIVER)
sys.path.insert(0, str(ROOT / "cmake/shaders"))

FAKE_SLANG = '''#!/usr/bin/env python3
"""Stand-in slangc: emits a minimal DXBC container with a recorded hash."""
import json
import struct
import sys
arguments = sys.argv[1:]
option = lambda flag: arguments[arguments.index(flag) + 1]
stage = option("-stage")
entry = option("-entry")
assert option("-target") == "dxil" and option("-D") == "LUDUS_DXIL=1" and option("-dxc-path")
digest = bytes(range(1, 17)) if "UNSIGNED" not in option("-dxc-path") else bytes(16)
body = b"DXBC" + digest + struct.pack("<HHII", 1, 0, 32, 0)
with open(option("-o"), "wb") as artifact:
    artifact.write(body)
size = 0 if stage == "vertex" else 48
parameters = [] if size == 0 else [{"binding": {"kind": "constantBuffer", "index": 0},
              "type": {"kind": "constantBuffer", "elementVarLayout": {"binding": {"size": size}}}}]
bindings = []
if "-DRASTER=1" in arguments and stage == "fragment":
    parameters = [{"name": "settings", "binding": {"kind": "constantBuffer", "index": 3},
                   "type": {"kind": "constantBuffer", "elementVarLayout": {"binding": {"size": 48}},
                            "elementType": {"fields": [{"name": "color", "binding": {"offset": 16},
                                "type": {"kind": "vector", "elementCount": 4, "elementType": {"scalarType": "float32"}}}]}}}]
    bindings = [{"name": "settings", "binding": {"kind": "constantBuffer", "index": 3, "used": 1}}]
with open(option("-reflection-json"), "w") as reflection:
    json.dump({"entryPoints": [{"name": entry, "stage": stage, "bindings": bindings}], "parameters": parameters}, reflection)
with open(option("-depfile"), "w") as depfile:
    # Windows drive letters must not be mistaken for the target separator.
    depfile.write("C:/checkout/out/" + stage + ".dxil: C:/checkout/shaders/fullscreen.slang\\n")
'''


class DxilDriverTests(unittest.TestCase):
    def compile(self, root, dxc_name="dxc", libraries=None, raster=False):
        compiler = root / "slangc"
        compiler.write_text(FAKE_SLANG)
        compiler.chmod(0o755)
        dxc = root / dxc_name
        dxc.mkdir()
        contents = libraries or {"dxcompiler.dll": b"compiler", "dxil.dll": b"validator"}
        for name, data in contents.items():
            (dxc / name).write_bytes(data)
        lock = {"slang": {"version": "test"},
                "dxc": {"version": "test", "sha256": "0" * 64,
                        "dxcompiler_sha256": hashlib.sha256(b"compiler").hexdigest(),
                        "dxil_sha256": hashlib.sha256(b"validator").hexdigest()}}
        source = root / "fullscreen.slang"
        source.write_text("// source\n")
        arguments = argparse.Namespace(source=str(source), output=str(root / "out"), name="fullscreen",
                                       compiler=str(compiler), include=[], define=["RASTER=1"] if raster else [], dxc_path=str(dxc), raster=raster,
                                       vertex="vertexMain", fragment="fragmentMain")
        DRIVER.compile_dxil(arguments, lock)
        return root / "out"

    def test_header_carries_signed_containers_entries_and_reflected_sizes(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.compile(Path(temporary))
            header = (output / "fullscreen.h").read_text()
            self.assertIn("alignas(4) inline constexpr foundation::uint8 VERTEX_DXIL[]", header)
            self.assertIn('result.Dxil = FRAGMENT_DXIL; result.DxilEntry = "fragmentMain";', header)
            self.assertIn("result.UniformSize = 0; result.Dxil = VERTEX_DXIL;", header)
            self.assertIn("result.UniformSize = 48; result.Dxil = FRAGMENT_DXIL;", header)
            self.assertIn("0x44U, 0x58U, 0x42U, 0x43U", header)
            manifest = json.loads((output / "manifest.json").read_text())
            self.assertEqual(manifest["dxil_entries"], {"vertex": "vertexMain", "fragment": "fragmentMain"})
            self.assertEqual(manifest["profiles"], {"dxil": "sm_6_0"})
            dependencies = (output / "shader.d").read_text().split(": ", 1)[1]
            self.assertEqual(dependencies.split(), ["C:/checkout/shaders/fullscreen.slang"] * 2)

    def test_unsigned_containers_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "not signed"):
                self.compile(Path(temporary), dxc_name="UNSIGNED")
            self.assertFalse((Path(temporary) / "out/fullscreen.h").exists())

    def test_dxc_libraries_must_match_the_pin(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "dxil.dll differs from SDK pin"):
                self.compile(Path(temporary), libraries={"dxcompiler.dll": b"compiler", "dxil.dll": b"other"})

    def test_container_size_must_match_the_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "shader.dxil"
            path.write_bytes(b"DXBC" + bytes(range(1, 17)) + struct.pack("<HHII", 1, 0, 64, 0))
            with self.assertRaisesRegex(RuntimeError, "size differs"):
                DRIVER.dxil_container(path)
            path.write_bytes(b"SPIR" + bytes(28))
            with self.assertRaisesRegex(RuntimeError, "not a DXIL container"):
                DRIVER.dxil_container(path)

    def test_raster_headers_use_native_registers_and_target_uniform_offsets(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = self.compile(Path(temporary), raster=True)
            header = (output / "fullscreen.h").read_text()
            self.assertIn("graphics::rhi::RasterShaderDescription Fragment()", header)
            self.assertIn(".Binding=3,.Kind=graphics::rhi::RasterBindingKind::UniformBuffer", header)
            self.assertIn('{3,"color",16,graphics::rhi::RasterUniformShape::Float4}', header)
            self.assertIn('result.Artifact.Dxil = FRAGMENT_DXIL;', header)
            self.assertIn('result.UniformMembers = FRAGMENT_DXIL_MEMBERS;', header)
            interface = json.loads((output / "manifest.json").read_text())["interfaces"]["fragment"]
            self.assertEqual(interface["entries"][0]["binding"], 3)
            self.assertEqual(interface["entries"][0]["offsets"], {"color": 16})

    def test_raster_native_reflection_rejects_spaces_collisions_arrays_and_storage(self):
        from compile_raster import raster_interface
        document = {"parameters": [{"name": "image", "binding": {"kind": "shaderResource", "index": 1},
                    "type": {"kind": "resource", "baseShape": "texture2D", "resultType": {
                        "kind": "vector", "elementType": {"scalarType": "float32"}}}}],
                    "entryPoints": [{"stage": "fragment", "bindings": [{"name": "image", "binding": {"used": 1}}]}]}
        self.assertEqual(raster_interface(document, "fragment", dxil=True)["entries"][0]["kind"], "Texture2D")
        for mutation in ("space", "collision", "array", "storage"):
            changed = copy.deepcopy(document)
            resource = changed["parameters"][0]
            if mutation == "space":
                resource["binding"]["space"] = 1
            elif mutation == "collision":
                changed["parameters"].append(copy.deepcopy(resource))
            elif mutation == "array":
                resource["type"] = {"kind": "array", "elementCount": 2, "elementType": resource["type"]}
            else:
                resource["type"] = {"kind": "resource", "baseShape": "structuredBuffer", "resultType": {"kind": "scalar", "scalarType": "float32"}}
            with self.subTest(mutation=mutation), self.assertRaises(RuntimeError):
                raster_interface(changed, "fragment", dxil=True)

    def test_windows_tools_are_independently_pinned(self):
        lock = json.loads((ROOT / "config/shader_toolchain.json").read_text())
        slang = lock["slang"]["windows"]["x86_64"]
        self.assertIn("windows-x86_64.zip", slang["url"])
        self.assertIn(lock["slang"]["version"], slang["url"])
        dxc = lock["dxc"]
        self.assertIn("/v" + dxc["version"] + "/", dxc["url"])
        for key in ("sha256", "dxcompiler_sha256", "dxil_sha256"):
            self.assertRegex(dxc[key], r"^[0-9a-f]{64}$")
        self.assertEqual(len({slang["sha256"], dxc["sha256"], dxc["dxcompiler_sha256"], dxc["dxil_sha256"]}), 4)


if __name__ == "__main__":
    unittest.main()
