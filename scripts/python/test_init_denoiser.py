"""Check the default-on OIDN denoiser choice, its pinned installation and CMake default."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import engine
import init_denoiser
import init_options
import init_progress
import init_ui


ROOT = Path(__file__).resolve().parents[2]
APPLE_SILICON = {"system": "Darwin", "machine": "arm64"}


def resolved(*options):
    return init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--cli", *options]))


@contextlib.contextmanager
def host(system: str, machine: str):
    with patch.object(init_denoiser.platform, "system", return_value=system), \
            patch.object(init_denoiser.platform, "machine", return_value=machine):
        yield


class OptionTests(unittest.TestCase):
    def test_only_native_apple_silicon_presets_support_the_denoiser(self):
        for system, machine, preset, expected in (
                ("Darwin", "arm64", "macos-clang-development", True),
                ("Darwin", "arm64", "macos-clang-asan-ubsan", True),
                ("Darwin", "arm64", "web-emscripten-development", False),
                ("Darwin", "x86_64", "macos-clang-development", False),
                ("Linux", "x86_64", "linux-clang-development", False),
                ("Windows", "AMD64", "linux-clang-development", False)):
            with self.subTest(system=system, machine=machine, preset=preset):
                self.assertEqual(init_denoiser.preset_supports(preset, system=system, machine=machine), expected)

    def test_default_is_on_where_supported_and_off_with_a_note_elsewhere(self):
        with host("Darwin", "arm64"):
            self.assertTrue(resolved("macos-clang-development").with_denoiser)
            self.assertFalse(resolved("macos-clang-development", "--no-denoiser").with_denoiser)
            self.assertFalse(resolved("--persona", "browser").with_denoiser)
            init_ui.validate_options(resolved("macos-clang-development", "--with-denoiser"), engine)
        for system, machine, preset in (("Linux", "x86_64", "linux-clang-development"),
                                        ("Darwin", "x86_64", "macos-clang-development")):
            with self.subTest(system=system), host(system, machine):
                args = resolved(preset)
                self.assertFalse(args.with_denoiser)
                init_ui.validate_options(args, engine)
                output = io.StringIO()
                with contextlib.redirect_stdout(output), \
                        patch.object(init_denoiser, "install_denoiser") as install:
                    init_denoiser.setup_denoiser(args, ROOT, {}, engine)
                install.assert_not_called()
                self.assertIn("available only on macOS with Apple silicon", output.getvalue())

    def test_explicit_request_on_an_unsupported_preset_fails_before_setup(self):
        for system, machine, options in (("Darwin", "arm64", ("--persona", "browser", "--with-denoiser")),
                                         ("Linux", "x86_64", ("linux-clang-development", "--with-denoiser"))):
            with self.subTest(options=options), host(system, machine), \
                    self.assertRaisesRegex(engine.EngineError, "--with-denoiser requires"):
                init_ui.validate_options(resolved(*options), engine)

    def test_choice_is_passed_to_the_runner_and_planned(self):
        with host("Darwin", "arm64"):
            self.assertIn("--with-denoiser", init_ui.selection_to_argv(resolved("macos-clang-development")))
            self.assertIn("--no-denoiser", init_ui.selection_to_argv(resolved("macos-clang-development",
                                                                              "--no-denoiser")))
            for options, planned in (((), True), (("--no-denoiser",), False)):
                keys = [step.key for step in init_progress.native_plan(resolved("macos-clang-development", *options),
                                                                       environment={})]
                self.assertEqual("denoiser" in keys, planned)
                if planned:
                    self.assertEqual(keys.index("denoiser"), keys.index("conan") + 1)

    def test_saved_choice_enables_oidn_only_for_supported_presets(self):
        with tempfile.TemporaryDirectory() as temporary, host("Darwin", "arm64"):
            root = Path(temporary)
            presets = ("macos-clang-development", "macos-clang-debug")
            init_options.save_options(root, presets, resolved("macos-clang-development"))
            for preset in presets:
                self.assertTrue(init_options.read_options(root, preset)["LUDUS_ENABLE_OIDN"])
            init_options.save_options(root, presets, resolved("macos-clang-development", "--no-denoiser"))
            self.assertFalse(any(init_options.read_options(root, preset)["LUDUS_ENABLE_OIDN"] for preset in presets))
            browser = resolved("--persona", "browser")
            init_options.save_options(root, (browser.preset,), browser)
            self.assertFalse(init_options.read_options(root, browser.preset)["LUDUS_ENABLE_OIDN"])

    def test_pin_matches_the_cmake_package_version(self):
        oidn = init_denoiser.pin(engine.load_tool_versions(ROOT))
        self.assertEqual(oidn["license"], "Apache-2.0")
        archive = oidn["archives"]["darwin_arm64"]
        self.assertRegex(archive["sha256"], "^[0-9a-f]{64}$")
        self.assertTrue(archive["url"].startswith("https://github.com/RenderKit/oidn/releases/download/v"
                                                  + oidn["version"] + "/"))
        for path in ("cmake/EngineDenoiserOptions.cmake", "cmake/LudusConfig.cmake.in"):
            self.assertIn(f"OpenImageDenoise {oidn['version']} EXACT", (ROOT / path).read_text(encoding="utf-8"))
        # Init's installation check and the CMake/SDK layout check name the same files.
        options = (ROOT / "cmake/EngineDenoiserOptions.cmake").read_text(encoding="utf-8")
        for name in (*init_denoiser.RUNTIME_LIBRARIES, *init_denoiser.LICENSE_FILES):
            self.assertIn(name, options)


def write_release(directory: Path, *, extra=None) -> Path:
    """Write a release-shaped archive with the runtime, notices and a sibling version link."""
    top = directory / "oidn-test.arm64.macos"
    (top / "lib").mkdir(parents=True)
    (top / "doc").mkdir()
    for name in init_denoiser.RUNTIME_LIBRARIES:
        (top / "lib" / name).write_bytes(name.encode())
    for name in init_denoiser.LICENSE_FILES:
        (top / "doc" / name).write_text("notice\n", encoding="utf-8")
    (top / "lib" / "libOpenImageDenoise.dylib").symlink_to("libOpenImageDenoise.2.dylib")
    archive = directory / "oidn-test.arm64.macos.tar.gz"
    with tarfile.open(archive, "w:gz") as contents:
        contents.add(top, arcname=top.name)
        if extra is not None:
            extra(contents)
    return archive


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo"
        self.root.mkdir()
        self.releases = Path(self.temporary.name) / "releases"
        self.releases.mkdir()

    def versions(self, archive: Path, digest: str | None = None) -> dict:
        digest = digest or hashlib.sha256(archive.read_bytes()).hexdigest()
        return {"denoiser": {"oidn": {"version": "2.5.1", "license": "Apache-2.0", "archives": {
            "darwin_arm64": {"url": f"https://example.invalid/{archive.name}", "sha256": digest}}}}}

    def install(self, versions: dict, archive: Path):
        def copy(_url, destination):
            shutil.copyfile(archive, destination)
        with host("Darwin", "arm64"), patch.object(init_denoiser, "download", side_effect=copy) as download, \
                contextlib.redirect_stdout(io.StringIO()):
            init_denoiser.install_denoiser(self.root, versions, engine)
        return download

    def test_verified_release_is_installed_once(self):
        archive = write_release(self.releases)
        versions = self.versions(archive)
        self.assertEqual(self.install(versions, archive).call_count, 1)
        prefix = self.root / init_denoiser.INSTALL_DIRECTORY
        self.assertTrue((prefix / "lib/libOpenImageDenoise.dylib").is_symlink())
        self.assertTrue(init_denoiser.is_installed(self.root, init_denoiser.pin(versions), "darwin_arm64"))
        self.assertEqual(list(prefix.parent.glob("*.tar.gz")), [])
        self.assertEqual(self.install(versions, archive).call_count, 0)
        # A changed pin replaces the previous installation.
        replacement = write_release(Path(tempfile.mkdtemp(dir=self.temporary.name)))
        self.assertEqual(self.install(self.versions(replacement), replacement).call_count, 1)
        self.assertEqual(sorted(path.name for path in prefix.parent.iterdir()), ["oidn"])

    def test_integrity_failure_removes_the_download_and_installs_nothing(self):
        archive = write_release(self.releases)
        with self.assertRaisesRegex(engine.EngineError, "integrity failure"):
            self.install(self.versions(archive, "0" * 64), archive)
        self.assertEqual(list((self.root / "out/denoiser").iterdir()), [])

    def test_unsafe_archive_members_are_rejected(self):
        def escape(contents):
            member = tarfile.TarInfo("oidn-test.arm64.macos/lib/escape.dylib")
            member.type, member.linkname = tarfile.SYMTYPE, "../../outside"
            contents.addfile(member)

        def traversal(contents):
            member = tarfile.TarInfo("../outside.txt")
            contents.addfile(member, io.BytesIO(b""))
        for extra in (escape, traversal):
            with self.subTest(extra=extra.__name__):
                directory = Path(tempfile.mkdtemp(dir=self.temporary.name))
                archive = write_release(directory, extra=extra)
                with self.assertRaisesRegex(engine.EngineError, "archive rejected"):
                    self.install(self.versions(archive), archive)
                self.assertFalse((self.root / init_denoiser.INSTALL_DIRECTORY).exists())

    def test_unsupported_host_is_not_installed(self):
        with host("Linux", "x86_64"), self.assertRaisesRegex(engine.EngineError, "Apple silicon"):
            init_denoiser.install_denoiser(self.root, {}, engine)


class CMakeDefaultTests(unittest.TestCase):
    """The configure-time default follows the init record or a found package and never downloads."""

    def setUp(self):
        self.cmake = shutil.which("cmake")
        if not self.cmake:
            self.skipTest("CMake unavailable")
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "out/build" / "macos-clang-development"
        modules = ROOT / "cmake"
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(DenoiserOptions NONE)\n"
            f'include("{modules / "EngineInitOptions.cmake"}")\n'
            f'include("{modules / "EngineDenoiserOptions.cmake"}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/oidn.txt" "${LUDUS_ENABLE_OIDN}")\n', encoding="utf-8")

    def install_package(self, *, complete=True):
        prefix = self.root / "out/denoiser/oidn"
        package = prefix / "lib/cmake/OpenImageDenoise-2.5.1"
        package.mkdir(parents=True)
        if complete:
            (prefix / "doc").mkdir()
            for name in init_denoiser.RUNTIME_LIBRARIES:
                (prefix / "lib" / name).write_bytes(b"")
            for name in init_denoiser.LICENSE_FILES:
                (prefix / "doc" / name).write_text("notice\n", encoding="utf-8")
        (package / "OpenImageDenoiseConfig.cmake").write_text("", encoding="utf-8")
        (package / "OpenImageDenoiseConfigVersion.cmake").write_text(
            'set(PACKAGE_VERSION "2.5.1")\nif(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)\n'
            '    set(PACKAGE_VERSION_COMPATIBLE TRUE)\n    set(PACKAGE_VERSION_EXACT TRUE)\nendif()\n',
            encoding="utf-8")

    def configure(self, *options, check=True):
        result = subprocess.run([self.cmake, "-S", str(self.root), "-B", str(self.build), "-DAPPLE=ON",
                                 "-DCMAKE_OSX_ARCHITECTURES=arm64", *options], capture_output=True, text=True)
        if check:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return (self.build / "oidn.txt").read_text(encoding="utf-8")
        return result

    def test_plain_configure_without_init_keeps_working_with_the_denoiser_off(self):
        self.assertEqual(self.configure(), "OFF")
        self.assertEqual(self.configure("--fresh", "-DCMAKE_OSX_ARCHITECTURES=x86_64"), "OFF")

    def test_found_package_turns_the_default_on(self):
        self.install_package()
        self.assertEqual(self.configure(), "ON")
        self.assertEqual(self.configure("--fresh", "-DCMAKE_OSX_ARCHITECTURES=x86_64"), "OFF")
        self.assertEqual(self.configure("--fresh", "-DLUDUS_ENABLE_OIDN=OFF"), "OFF")

    def test_incomplete_package_keeps_the_default_off_and_rejects_explicit_on(self):
        # For example a package manager's OIDN without the release's runtime layout and notices.
        self.install_package(complete=False)
        self.assertEqual(self.configure(), "OFF")
        result = self.configure("--fresh", "-DLUDUS_ENABLE_OIDN=ON", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("lacks", result.stderr)
        self.assertIn("official 2.5.1 release", result.stderr)

    def test_explicit_on_without_the_package_fails_clearly(self):
        result = self.configure("-DLUDUS_ENABLE_OIDN=ON", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OpenImageDenoise 2.5.1 was not found", result.stderr)
        self.assertIn("--no-denoiser", result.stderr)

    def test_saved_init_choice_overrides_the_cache(self):
        options = self.root / "out/init/options.json"
        options.parent.mkdir(parents=True)
        options.write_text(json.dumps({"macos-clang-development": {"LUDUS_ENABLE_OIDN": False}}), encoding="utf-8")
        self.install_package()
        self.assertEqual(self.configure(), "OFF")
        options.write_text(json.dumps({"macos-clang-development": {"LUDUS_ENABLE_OIDN": True}}), encoding="utf-8")
        self.assertEqual(self.configure(), "ON")


if __name__ == "__main__":
    unittest.main()
