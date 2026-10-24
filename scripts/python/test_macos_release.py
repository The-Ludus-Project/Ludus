"""Portable malformed Mach-O contracts and native ad-hoc bundle round trips."""
import json
import os
import plistlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from dataclasses import asdict
from unittest.mock import patch

from ludus_tools import create, release, release_model
from ludus_tools.errors import ToolingError
from ludus_tools.identity import SdkIdentity
from ludus_tools import package_macos as mac
from ludus_tools.package_verify import inventory, file_digest, extract_archive, verify_package
from ludus_tools.release_setup import setup_release, workflow_text
from ludus_tools.release_template import release_files
from test_ludus_tools import _manifest_json


def synthetic(*, cpu=0x100000c, minimum=0xe0000, segment=b"__TEXT", dependency=None):
    extra = b""
    if dependency:
        text = dependency.encode() + b"\0"
        stride = (24 + len(text) + 7) & ~7
        extra = struct.pack("<6I", 0xc, stride, 24, 0, 0, 0) + text + b"\0" * (stride - 24 - len(text))
    length = 152 + 24 + 24 + len(extra)
    position = 32 + length
    seg = struct.pack("<II16s4Q4I", 0x19, 152, segment, 0, position + 4, 0, position + 4, 7, 5, 1, 0)
    sec = struct.pack("<16s16sQQ8I", b"__text", segment, position, 4, position, 2, 0, 0, 0, 0, 0, 0)
    build = struct.pack("<6I", 0x32, 24, 1, minimum, minimum, 0)
    entry = struct.pack("<IIQQ", 0x80000028, 24, position, 0)
    return struct.pack("<8I", 0xfeedfacf, cpu, 0, 2, 3 + bool(extra), length, 0, 0) + seg + sec + build + entry + extra + b"code"


class MachoTests(unittest.TestCase):
    def inspect(self, contents):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "game"
            path.write_bytes(contents)
            return mac.inspect(path)

    def test_arm64_and_x64_and_entry_identity(self):
        for cpu in mac.CPUS.values():
            self.assertEqual(cpu, self.inspect(synthetic(cpu=cpu))["cpu"])
        self.assertNotEqual(self.inspect(synthetic())["sections"], self.inspect(synthetic()[:-4] + b"edit")["sections"])

    def test_malformed_foreign_debug_and_new_baseline_fail(self):
        cases = [b"", b"\xca\xfe\xba\xbe" + synthetic()[4:], synthetic(cpu=7), synthetic(minimum=0xf0000),
                 synthetic(segment=b"__DWARF"), synthetic()[:-1]]
        for offset, value in ((16, 4097), (20, 0xffffffff), (36, 7), (72, 0xffffffff), (96, 4097), (216, 0)):
            value_bytes = bytearray(synthetic())
            struct.pack_into("<I", value_bytes, offset, value)
            cases.append(value_bytes)
        for contents in cases:
            with self.subTest(contents=contents[:32]), self.assertRaises(ToolingError):
                self.inspect(contents)

    def test_loader_strings_are_bounded_and_dyld_environment_is_rejected(self):
        data = synthetic(dependency="/usr/lib/libSystem.B.dylib")
        self.assertEqual(["/usr/lib/libSystem.B.dylib"], self.inspect(data)["dependencies"])
        bad = bytearray(data)
        struct.pack_into("<I", bad, 32 + 200 + 8, 4)
        with self.assertRaises(ToolingError): self.inspect(bad)
        bad = bytearray(synthetic())
        struct.pack_into("<I", bad, 32 + 152, 0x27)
        with self.assertRaises(ToolingError): self.inspect(bad)

    def test_loader_paths_cannot_escape_or_use_producer_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            exe = root / "Game.app/Contents/MacOS/Game"
            self.assertEqual(root / "Game.app/Contents/Frameworks", mac._relative(root, exe, exe, "@executable_path/../Frameworks"))
            for value in ("/opt/homebrew/lib", "@loader_path/../../../../outside", "@rpath/libgame.dylib"):
                with self.assertRaises(ToolingError): mac._relative(root, exe, exe, value)

    def test_macos_release_installs_the_denoiser_runtime_only_through_the_sdk_helper(self):
        for platform in mac.CPUS:
            helper = next(item.content for item in release_files("Game", platform=platform)
                          if item.relpath == "cmake/GameRelease.cmake")
            self.assertIn("if(COMMAND ludus_install_denoiser_runtime)\n    ludus_install_denoiser_runtime(TARGET Game "
                          "DESTINATION Game.app/Contents/Frameworks COMPONENT GameRelease)", helper)
        linux = next(item.content for item in release_files("Game") if item.relpath == "cmake/GameRelease.cmake")
        self.assertNotIn("ludus_install_denoiser_runtime", linux)
        # The bundled OIDN runtime and its device modules link these system frameworks.
        self.assertTrue({"Accelerate", "Metal", "MetalPerformanceShaders", "MetalPerformanceShadersGraph"} <= mac.FRAMEWORKS)

    def test_both_profiles_and_preset_mismatch(self):
        for platform in mac.CPUS:
            with tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                config = json.loads(release_files("Game", "tester/game", platform=platform)[0].content)
                path = root / "ludus.release.json"
                path.write_text(json.dumps(config))
                parsed = release_model.load_release(root)
                self.assertEqual("macos-clang-release", parsed.profiles["macos-release"].configure_preset)
                self.assertEqual(("macos-release", "macos-stable"), parsed.destinations["macos"])
                config["profiles"]["macos-release"]["configurePreset"] = "linux-clang-release"
                path.write_text(json.dumps(config))
                with self.assertRaises(ToolingError): release_model.load_release(root)

    def test_setup_is_no_overwrite_and_ci_has_no_transport_or_secret(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "game"
            create.create_project(root, name="Game", template_id="minimal", engine_version="0.1.0", preset="macos-clang-development")
            before = (root / "src/main.cpp").read_bytes()
            setup_release(root, platform="macos-arm64", tools_ref="a" * 40)
            self.assertEqual(before, (root / "src/main.cpp").read_bytes())
            workflow = (root / ".github/workflows/itch-release.yml").read_text()
            self.assertIn("macos-14", workflow)
            self.assertIn("LUDUS_RELEASE_SDK_SHA256", workflow)
            self.assertNotIn("BUTLER_API_KEY", workflow)
            self.assertNotIn("publish upload", workflow)
            snapshot = {str(p): p.read_bytes() for p in root.rglob("*") if p.is_file()}
            with self.assertRaises(ToolingError): setup_release(root, platform="macos-arm64")
            self.assertEqual(snapshot, {str(p): p.read_bytes() for p in root.rglob("*") if p.is_file()})

    def test_debug_sidecar_directory_is_not_runtime_payload(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            file = root / "Game.app.dSYM/Contents/Resources/DWARF/Game"
            file.parent.mkdir(parents=True)
            file.write_bytes(b"unrecognized debug data")
            with self.assertRaisesRegex(ToolingError, "debug file"):
                inventory(root)

    def test_x64_workflow_uses_native_intel_runner(self):
        self.assertIn("runs-on: macos-15-intel", workflow_text("macos-release", "macos", "macos-x64"))

    def test_upload_guard_does_not_install_or_read_credentials(self):
        from ludus_tools.itch import upload
        with patch("sys.platform", "darwin"), patch("ludus_tools.itch.install_butler") as install:
            with self.assertRaisesRegex(ToolingError, "transport requires Linux"):
                upload(Path("missing"), Path("missing"), destination="macos")
            install.assert_not_called()


@unittest.skipUnless(sys.platform == "darwin", "native codesign requires macOS")
class NativeBundleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.payload = self.root / "payload"
        self.bundle = self.payload / "Game.app"
        self.entry = "Game.app/Contents/MacOS/Game"
        self.exe = self.payload / self.entry
        self.exe.parent.mkdir(parents=True)
        source = self.root / "main.cpp"
        source.write_text("int main() { return 0; }\n")
        compiler = shutil.which("clang++")
        subprocess.run([compiler, "-std=c++23", "-fno-exceptions", "-g0", "-mmacosx-version-min=14.0", str(source), "-o", str(self.exe)], check=True, capture_output=True)
        (self.bundle / "Contents/Info.plist").write_bytes(plistlib.dumps({"CFBundleExecutable": "Game", "CFBundlePackageType": "APPL", "CFBundleIdentifier": "org.ludus.fixture"}))
        (self.payload / "NOTICE.txt").write_text("test notices")
        (self.payload / "licenses").mkdir()
        (self.payload / "licenses/LICENSE").write_text("test license")
        self.platform = "macos-arm64" if mac.inspect(self.exe)["cpu"] == mac.CPUS["macos-arm64"] else "macos-x64"

    def sign(self, runner=None, cancel=None):
        def run(argv, **kwargs):
            return subprocess.run(argv, capture_output=True, **kwargs).returncode
        mac.sign_payload(self.payload, self.entry, runner or run, os.environ.copy(), cancel)

    def package(self):
        self.sign()
        external = mac.validate_native(self.payload, self.entry, self.platform)
        identity = SdkIdentity.from_json(_manifest_json(build_flavor="Release", target_triple="arm64-apple-darwin" if self.platform == "macos-arm64" else "x86_64-apple-darwin", cxx_runtime_abi="libc++", distro_baseline="macos-14.0"))
        manifest = {"schemaVersion": 1, "profile": "macos-release", "targetPlatform": self.platform,
                    "entryPoint": self.entry, "version": "0.1.0", "source": {"revision": "unknown", "dirty": True},
                    "engineLockSha256": "a" * 64, "releaseConfigSha256": "b" * 64, "sdk": asdict(identity),
                    "localInputs": True, "policy": mac.POLICY, "systemLibraries": external, "files": inventory(self.payload)}
        package = self.root / "package"
        package.mkdir()
        release._archive(self.payload, package / "game.zip", manifest["files"], manifest)
        digest = file_digest(package / "game.zip")
        (package / "package.json").write_bytes(release_model.canonical({"schemaVersion": 1, "archiveSha256": digest,
            "manifestSha256": release_model.sha256(release_model.canonical(manifest)), "profile": "macos-release", "version": "0.1.0", "releaseConfigSha256": "b" * 64, "toolVersion": "0.1.0"}))
        (package / "validation.json").write_bytes(release_model.canonical({"schemaVersion": 1, "archiveSha256": digest,
            "policy": mac.POLICY, "toolVersion": "0.1.0", "checks": ["payload", "native", "clean-extraction"]}))
        return package, manifest

    def test_signed_roundtrip_relocation_execution_and_determinism(self):
        before = mac.executable_identity(self.exe)
        package, manifest = self.package()
        self.assertEqual(before, mac.executable_identity(self.exe))
        self.assertEqual(manifest, verify_package(package)["manifest"])
        extracted = self.root / "unrelated location"
        extracted.mkdir()
        extract_archive(package / "game.zip", extracted)
        result = subprocess.run([str(extracted / self.entry)], cwd=self.root, env={"PATH": os.defpath}, capture_output=True)
        self.assertEqual(0, result.returncode)
        self.sign()
        self.assertEqual(manifest["files"], inventory(self.payload))
        release._archive(self.payload, self.root / "again.zip", manifest["files"], manifest)
        self.assertEqual(file_digest(package / "game.zip"), file_digest(self.root / "again.zip"))

    def test_relative_dylib_closure_and_missing_dependency(self):
        libdir = self.bundle / "Contents/Frameworks"
        libdir.mkdir()
        library = libdir / "libgame.dylib"
        source = self.root / "library.cpp"
        source.write_text('extern "C" int answer() noexcept { return 42; }\n')
        main = self.root / "main.cpp"
        main.write_text('extern "C" int answer() noexcept; int main() noexcept { return answer() == 42 ? 0 : 1; }\n')
        compiler = shutil.which("clang++")
        common = [compiler, "-g0", "-fno-exceptions", "-mmacosx-version-min=14.0"]
        subprocess.run([*common, "-dynamiclib", str(source), "-Wl,-install_name,@rpath/libgame.dylib", "-o", str(library)], check=True, capture_output=True)
        subprocess.run([*common, str(main), str(library), "-Wl,-rpath,@executable_path/../Frameworks", "-o", str(self.exe)], check=True, capture_output=True)
        package, _ = self.package()
        extracted = self.root / "moved"
        extracted.mkdir()
        extract_archive(package / "game.zip", extracted)
        self.assertEqual(0, subprocess.run([str(extracted / self.entry)], env={"PATH": os.defpath}, capture_output=True).returncode)
        library.unlink()
        with self.assertRaisesRegex(ToolingError, "unresolved"):
            mac.validate_native(self.payload, self.entry, self.platform)

    @unittest.skipUnless(shutil.which("cmake") and shutil.which("ninja"), "requires managed CMake/Ninja")
    def test_cmake_dylib_install_rewrites_loader_paths_without_changing_code(self):
        project = self.root / "cmake-source"
        project.mkdir()
        (project / "library.cpp").write_text('extern "C" int answer() noexcept { return 42; }\n')
        (project / "main.cpp").write_text('extern "C" int answer() noexcept; int main() noexcept { return answer() == 42 ? 0 : 1; }\n')
        (project / "CMakeLists.txt").write_text("""cmake_minimum_required(VERSION 3.29)
project(Fixture LANGUAGES CXX)
add_library(helper SHARED library.cpp)
set_target_properties(helper PROPERTIES OUTPUT_NAME game INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
add_executable(Game MACOSX_BUNDLE main.cpp)
set_target_properties(Game PROPERTIES MACOSX_BUNDLE_GUI_IDENTIFIER org.ludus.fixture INSTALL_RPATH "@executable_path/../Frameworks")
target_link_libraries(Game PRIVATE helper)
install(TARGETS Game BUNDLE DESTINATION .)
install(TARGETS helper LIBRARY DESTINATION Game.app/Contents/Frameworks)
""")
        tree = self.root / "cmake-build"
        subprocess.run(["cmake", "-S", str(project), "-B", str(tree), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                        "-DCMAKE_CXX_COMPILER=" + shutil.which("clang++"), "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0",
                        "-DCMAKE_CXX_FLAGS=-fno-exceptions -g0"], check=True, capture_output=True)
        subprocess.run(["cmake", "--build", str(tree)], check=True, capture_output=True)
        artifact = tree / "Game.app/Contents/MacOS/Game"
        identity = mac.executable_identity(artifact)
        self.assertTrue(any(str(tree) in path for path in mac.inspect(artifact)["rpaths"]))
        shutil.rmtree(self.bundle)  # Production installation stages into an empty payload.
        subprocess.run(["cmake", "--install", str(tree), "--prefix", str(self.payload)], check=True, capture_output=True)
        self.package()
        self.assertEqual(identity, mac.executable_identity(self.exe))
        self.assertEqual(["@executable_path/../Frameworks"], mac.inspect(self.exe)["rpaths"])

    def test_sealed_resources_and_architecture_are_checked(self):
        resource = self.bundle / "Contents/Resources/data.txt"
        resource.parent.mkdir()
        resource.write_text("before")
        self.sign()
        mac.validate_native(self.payload, self.entry, self.platform)
        resource.write_text("after")
        with self.assertRaisesRegex(ToolingError, "signature"):
            mac.validate_native(self.payload, self.entry, self.platform)
        other = "macos-x64" if self.platform == "macos-arm64" else "macos-arm64"
        with self.assertRaisesRegex(ToolingError, "architecture"):
            mac.validate_native(self.payload, self.entry, other)

    def test_signature_checks_share_supervised_runner_and_cancel(self):
        self.sign()
        commands = []
        def runner(argv, **kwargs):
            self.assertNotIn("BUTLER_API_KEY", kwargs["env"])
            self.assertNotIn("DYLD_LIBRARY_PATH", kwargs["env"])
            commands.append(argv)
            return subprocess.run(argv, capture_output=True, **kwargs).returncode
        mac.validate_native(self.payload, self.entry, self.platform, runner=runner,
                            env={**os.environ, "BUTLER_API_KEY": "not-forwarded", "DYLD_LIBRARY_PATH": "/unused"})
        self.assertEqual(2, len(commands))
        def cancel(): raise ToolingError("Cancelled", "stopped during verification")
        with self.assertRaisesRegex(ToolingError, "stopped during verification"):
            mac.validate_native(self.payload, self.entry, self.platform, runner=runner, cancel_check=cancel)
        self.assertEqual(2, len(commands))

    def test_failed_and_cancelled_signing_do_not_publish(self):
        with self.assertRaisesRegex(ToolingError, "signing failed"):
            self.sign(runner=lambda *args, **kwargs: 1)
        def cancel(): raise ToolingError("Cancelled", "stopped")
        with self.assertRaisesRegex(ToolingError, "stopped"):
            self.sign(cancel=cancel)
        self.assertFalse((self.root / "package").exists())

    def test_atomic_publication_preserves_existing_destination(self):
        staging, dest = self.root / "staging", self.root / "destination"
        staging.mkdir(); dest.mkdir()
        (dest / "marker").write_text("preserved")
        with self.assertRaises(ToolingError): release._publish_directory(staging, dest)
        self.assertTrue(staging.exists())
        self.assertEqual("preserved", (dest / "marker").read_text())
        shutil.rmtree(dest)
        release._publish_directory(staging, dest)
        self.assertFalse(staging.exists())
        self.assertTrue(dest.is_dir())


if __name__ == "__main__": unittest.main()
