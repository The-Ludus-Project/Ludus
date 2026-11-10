"""Windows Release archive, loader, symbol and portable upgrade regressions."""
from __future__ import annotations

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from ludus_tools import windows_distribution as distribution
from ludus_tools.errors import ToolingError
from ludus_tools.windows_binary import inspect_image, inspect_pdb, MSF_MAGIC
from test_ludus_tools import _manifest_json

GUID = bytes(range(16))
ROOT = Path(__file__).resolve().parents[2]


def make_image(path, imports=(), *, age=1, is_dll=False, delay=False):
    image = bytearray(4096)
    image[:2] = b"MZ"
    struct.pack_into("<I", image, 60, 128)
    image[128:132] = b"PE\0\0"
    struct.pack_into("<HH", image, 132, 0x8664, 1)
    struct.pack_into("<HH", image, 148, 240, 2 | (0x2000 if is_dll else 0))
    optional = 152
    struct.pack_into("<H", image, optional, 0x20b)
    struct.pack_into("<I", image, optional + 108, 16)
    section = optional + 240
    struct.pack_into("<IIII", image, section + 8, 3584, 4096, 3584, 512)
    if imports:
        stride = 32 if delay else 20
        directory = 13 if delay else 1
        struct.pack_into("<II", image, optional + 112 + directory * 8, 4096 + 256, stride * (len(imports) + 1))
        for index, name in enumerate(imports):
            descriptor = 512 + 256 + index * stride
            name_offset = 512 + 512 + index * 128
            if delay:
                struct.pack_into("<II", image, descriptor, 1, 4096 + name_offset - 512)
            else:
                struct.pack_into("<I", image, descriptor + 12, 4096 + name_offset - 512)
            value = name.encode() + b"\0"
            image[name_offset:name_offset + len(value)] = value
    struct.pack_into("<II", image, optional + 112 + 6 * 8, 4096 + 1536, 28)
    rsds = b"RSDS" + GUID + struct.pack("<I", age) + b"game.pdb\0"
    struct.pack_into("<IIII", image, 512 + 1536 + 12, 2, len(rsds), 4096 + 1600, 512 + 1600)
    image[512 + 1600:512 + 1600 + len(rsds)] = rsds
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(image)


def make_pdb(path, *, age=1):
    data = bytearray(4096 * 4)
    data[:32] = MSF_MAGIC
    struct.pack_into("<IIIIII", data, 32, 4096, 1, 4, 16, 0, 1)
    struct.pack_into("<I", data, 4096, 2)
    struct.pack_into("<IIII", data, 8192, 2, 0, 28, 3)
    struct.pack_into("<III", data, 12288, 20000404, 0, age)
    data[12300:12316] = GUID
    path.write_bytes(data)


class WindowsDistributionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.payload = self.root / "installed"
        self.payload.mkdir()
        make_image(self.payload / "bin/game.exe", ("KERNEL32.dll",))
        make_pdb(self.root / "game.pdb")
        (self.payload / "NOTICE.txt").write_text("Ludus notices\n")
        (self.payload / "LICENSE.txt").write_text("fixture license\n")
        self.manifest = self.root / "sdk.json"
        self.manifest.write_text(json.dumps(_manifest_json(build_flavor="Release", target_triple="x86_64-pc-windows-msvc",
                                               compiler_id="MSVC", compiler_version="19.44.35224", cxx_runtime_abi="msvc-stl",
                                               sdk_variant="assert-v2-Release-Release-dialogs-0-asan-0-ubsan-0-tsan-0",
                                               assert_policy={"policy_version": 2, "enable_asserts": False,
                                                              "break_on_check": False, "dialogs_available": False})))
        self.docs = self.root / "docs.zip"
        with zipfile.ZipFile(self.docs, "w") as writer:
            for name in ("docs/README.md", "site/index.html", "site/documentation-sources.json"):
                writer.writestr(name, "fixture")
        self.spec = {"schemaVersion": 1, "kind": "game", "version": "1.0", "revision": "a" * 40,
                     "channel": "internal", "entryPoint": "bin/game.exe", "license": "LICENSE.txt",
                     "requiredFiles": [], "symbols": {"bin/game.exe": str(self.root / "game.pdb")}, "dependencies": []}

    def create(self, name="release"):
        path = self.root / "spec.json"
        path.write_text(json.dumps(self.spec))
        return distribution.create_distribution(self.payload, self.root / name, path,
                                                sdk_manifest=self.manifest, docs=self.docs)

    def test_round_trip_is_reproducible_and_contains_matching_symbols(self):
        first = self.create()
        second = self.create("second")
        self.assertEqual((first / "windows.zip").read_bytes(), (second / "windows.zip").read_bytes())
        manifest = distribution.verify_distribution(first / "windows.zip")
        self.assertEqual(manifest["systemLibraries"], ["kernel32.dll"])
        self.assertEqual(manifest["symbols"][0]["guid"], GUID.hex())
        self.assertEqual(manifest["qualification"], "pending-clean-machine-and-D7-acceptance")
        self.assertNotIn(str(self.root), json.dumps(manifest))

    def test_normal_and_delay_import_closure(self):
        for delay in (False, True):
            with self.subTest(delay=delay):
                make_image(self.payload / "bin/game.exe", ("missing.dll",), delay=delay)
                with self.assertRaisesRegex(ToolingError, "unresolved.*missing.dll"):
                    self.create()
        make_image(self.payload / "bin/missing.dll", is_dll=True)
        self.spec["dependencies"] = [{"name": "fixture", "version": "1", "license": "LICENSE.txt", "files": ["bin/missing.dll"]}]
        self.create()

    def test_missing_stale_symbols_and_dependencies_are_rejected(self):
        make_pdb(self.root / "game.pdb", age=2)
        with self.assertRaisesRegex(ToolingError, "PDB does not match"):
            self.create()
        make_pdb(self.root / "game.pdb")
        make_image(self.payload / "bin/plugin.dll", is_dll=True)
        with self.assertRaisesRegex(ToolingError, "no symbol/license owner"):
            self.create()

    def test_debug_crt_non_x64_and_missing_assets_are_rejected(self):
        make_image(self.payload / "bin/game.exe", ("vcruntime140d.dll",))
        with self.assertRaisesRegex(ToolingError, "debug runtime"):
            self.create()
        make_image(self.payload / "bin/game.exe")
        self.spec["requiredFiles"] = ["shaders/default.dxil"]
        with self.assertRaisesRegex(ToolingError, "missing/empty.*dxil"):
            self.create()
        image = bytearray((self.payload / "bin/game.exe").read_bytes())
        struct.pack_into("<H", image, 132, 0xaa64)
        (self.payload / "bin/game.exe").write_bytes(image)
        with self.assertRaises(ToolingError):
            inspect_image(self.payload / "bin/game.exe")

    def test_public_signature_cannot_silently_be_skipped(self):
        self.spec["channel"] = "public"
        with patch.object(distribution.sys, "platform", "linux"):
            with self.assertRaisesRegex(ToolingError, "require Windows"):
                self.create()
        self.assertFalse((self.root / "release").exists())

    def test_crt_requires_reviewed_installer_and_rechecks_its_digest(self):
        make_image(self.payload / "bin/game.exe", ("vcruntime140.dll",))
        with self.assertRaisesRegex(ToolingError, "redistributable"):
            self.create()
        # A synthetic installer tests byte binding; production public bundles
        # additionally verify its Authenticode signature on Windows.
        installer = self.root / "redist.exe"
        installer.write_bytes(b"fixture installer")
        self.spec["redistributable"] = {"path": str(installer), "version": "14.44.fixture",
            "sha256": distribution.file_digest(installer), "license": "LICENSE.txt"}
        manifest = distribution.verify_distribution(self.create() / "windows.zip")
        self.assertNotIn("path", manifest["redistributable"])
        installer.write_bytes(b"changed")
        with self.assertRaisesRegex(ToolingError, "digest differs"):
            self.create("bad-installer")

    def test_sdk_identity_and_candidate_revision_must_agree(self):
        (self.payload / "bin/game.exe").unlink()
        installed = self.payload / "share/Ludus/LudusSdkManifest.json"
        installed.parent.mkdir(parents=True)
        identity = json.loads(self.manifest.read_text())
        identity["source_revision"] = "a" * 40
        self.manifest.write_text(json.dumps(identity))
        installed.write_text(json.dumps(identity))
        self.spec.update(kind="sdk", symbols={})
        del self.spec["entryPoint"]
        self.assertEqual(distribution.verify_distribution(self.create() / "windows.zip")["kind"], "sdk")
        self.spec["revision"] = "b" * 40
        with self.assertRaisesRegex(ToolingError, "candidate revision"):
            self.create("wrong-sdk")

    def test_cmake_off_sanitizer_variant_is_a_release_identity(self):
        identity = json.loads(self.manifest.read_text())
        identity["sdk_variant"] = "assert-v2-Release-Release-dialogs-0-asan-OFF-ubsan-OFF-tsan-OFF"
        self.manifest.write_text(json.dumps(identity))
        self.create()
        identity["sdk_variant"] = identity["sdk_variant"].replace("asan-OFF", "asan-ON")
        self.manifest.write_text(json.dumps(identity))
        with self.assertRaisesRegex(ToolingError, "without sanitizers"):
            self.create("sanitized")

    def test_editor_requires_deployed_platform_plugin_and_relative_qt_paths(self):
        self.spec["kind"] = "editor"
        with self.assertRaisesRegex(ToolingError, "qwindows.dll"):
            self.create()
        make_image(self.payload / "bin/platforms/qwindows.dll", is_dll=True)
        self.spec["dependencies"] = [{"name": "Qt", "version": "fixture", "license": "LICENSE.txt",
                                      "files": ["bin/platforms/qwindows.dll"]}]
        configuration = self.payload / "bin/qt.conf"
        configuration.write_text("[Paths]\nPrefix=C:/Qt\nPlugins=.\n")
        with self.assertRaisesRegex(ToolingError, "qt.conf must use"):
            self.create()
        configuration.write_text("[Paths]\nPrefix=.\nPlugins=.\n")
        self.create()

    def test_paths_links_and_case_collisions(self):
        for value in ("../bad", "bin/NUL.txt", "bin/a.", "a/COM1", "bin/a:b", "bin/a ", "a?.txt"):
            with self.subTest(value=value), self.assertRaises(ToolingError):
                distribution.check_windows_path(value)
        if os.name != "nt":
            (self.payload / "linked").symlink_to(self.root / "game.pdb")
            with self.assertRaises(ToolingError):
                self.create()
            (self.payload / "linked").unlink()
            (self.payload / "notice.TXT").write_text("collision")
            if len([path for path in self.payload.iterdir() if path.name.casefold() == "notice.txt"]) == 2:
                with self.assertRaises(ToolingError):
                    self.create()

    def test_offline_docs_and_release_identity_are_required(self):
        self.docs.write_bytes(b"broken archive")
        with self.assertRaises(zipfile.BadZipFile):
            self.create()
        self.manifest.write_text(json.dumps(_manifest_json(build_flavor="Development")))
        with self.assertRaisesRegex(ToolingError, "Release SDK"):
            self.create()

    def test_clean_extraction_detects_tampering(self):
        archive = self.create() / "windows.zip"
        corrupted = self.root / "corrupt.zip"
        with zipfile.ZipFile(archive) as reader, zipfile.ZipFile(corrupted, "w") as writer:
            for item in reader.infolist():
                writer.writestr(item, b"changed" if item.filename == "payload/NOTICE.txt" else reader.read(item))
        with self.assertRaisesRegex(ToolingError, "inventory differs"):
            distribution.verify_distribution(corrupted)

    def test_traversal_reserved_names_and_parent_collisions_fail_before_extraction(self):
        for names in (("../outside",), ("payload/CON",), ("A/file", "a/other"), ("parent", "parent/file")):
            archive = self.root / "bad.zip"
            with zipfile.ZipFile(archive, "w") as writer:
                for name in names:
                    info = zipfile.ZipInfo(name)
                    info.external_attr = 0o100644 << 16
                    info.compress_type = zipfile.ZIP_DEFLATED
                    writer.writestr(info, b"bad")
            with self.subTest(names=names), self.assertRaises(ToolingError):
                distribution.verify_distribution(archive)

    def test_update_rollback_uninstall_do_not_touch_authored_data(self):
        project = self.root / "user-data/project"
        project.parent.mkdir()
        project.write_bytes(b"authored data")
        old = self.create("v1")
        self.spec["version"] = "2.0"
        new = self.create("v2")
        self.assertEqual(distribution.verify_distribution(old / "windows.zip")["version"], "1.0")
        self.assertEqual(distribution.verify_distribution(new / "windows.zip")["version"], "2.0")
        shutil.rmtree(new)
        self.assertEqual(project.read_bytes(), b"authored data")
        self.assertTrue(old.is_dir())
        with self.assertRaisesRegex(ToolingError, "destination exists"):
            self.create("v1")

    def test_output_inside_payload_is_rejected_before_copying(self):
        with self.assertRaisesRegex(ToolingError, "outside the installed payload"):
            self.create("installed/output")

    def test_bounded_pe_and_pdb_readers_reject_truncation(self):
        for size in (0, 1, 55, 511):
            path = self.root / "truncated"
            path.write_bytes(bytes(size))
            for inspector in (inspect_image, inspect_pdb):
                with self.subTest(size=size, inspector=inspector), self.assertRaises(ToolingError):
                    inspector(path)

    @unittest.skipUnless(sys.platform == "win32" and shutil.which("cl") and shutil.which("cmake"), "requires Windows MSVC developer environment")
    def test_real_msvc_cmake_install_symbols_and_relocated_launch(self):
        source = self.root / "source"
        source.mkdir()
        (source / "main.cpp").write_text("int main() { return 0; }\n")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.29)\nproject(DistributionFixture LANGUAGES CXX)\n'
            'add_executable(game main.cpp)\n'
            f'include("{(ROOT / "cmake/LudusWindowsDistribution.cmake").as_posix()}")\n'
            'ludus_install_windows_application(game DESTINATION bin COMPONENT GameRelease SYMBOL_COMPONENT GameSymbols)\n')
        build = self.root / "build"
        for command in (["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                        ["cmake", "--build", str(build)],
                        ["cmake", "--install", str(build), "--component", "GameRelease", "--prefix", str(self.payload)],
                        ["cmake", "--install", str(build), "--component", "GameSymbols", "--prefix", str(self.root / "installed-symbols")]):
            subprocess.run(command, check=True)
        self.spec["symbols"]["bin/game.exe"] = str(self.root / "installed-symbols/bin/game.pdb")
        self.assertEqual(inspect_image(self.payload / "bin/game.exe")["pdb"],
                         inspect_pdb(Path(self.spec["symbols"]["bin/game.exe"])))
        # CI has the CRT: relocation evidence does not claim a pristine VM.
        extracted = self.root / "relocated"
        shutil.copytree(self.payload, extracted / "payload")
        clean_env = {**os.environ, "PATH": str(Path(os.environ["SystemRoot"]) / "System32")}
        subprocess.run([str(extracted / "payload/bin/game.exe")], cwd=self.root,
                       env=clean_env, check=True, timeout=30)


if __name__ == "__main__":
    unittest.main()
