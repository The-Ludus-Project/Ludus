"""Host selection regressions for the compile-only macOS slice."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import engine
import init_ui

LLVM_MAJOR = engine.pinned_llvm_major(engine.repo_root())


class MacosSetupTests(unittest.TestCase):
    def test_native_profiles_distinguish_mac_architectures(self):
        for arch in ("arm64", "x86_64"):
            with self.subTest(arch=arch), patch.object(engine.platform, "system", return_value="Darwin"), \
                    patch.object(engine.platform, "machine", return_value=arch):
                self.assertEqual(engine.native_profile_name(), "macos-clang-" + arch)
        with patch.object(engine.platform, "system", return_value="Darwin"), \
                patch.object(engine.platform, "machine", return_value="unsupported"):
            with self.assertRaises(engine.EngineError):
                engine.native_profile_name()
        with patch.object(engine.platform, "system", return_value="Linux"):
            self.assertEqual(engine.native_profile_name(), "linux-clang-x86_64")

    def test_profile_install_is_repeatable_and_tracks_changes(self):
        with tempfile.TemporaryDirectory() as temporary, \
                patch.object(engine.platform, "system", return_value="Darwin"), \
                patch.object(engine.platform, "machine", return_value="arm64"):
            root = Path(temporary)
            source = root / "config/conan/profiles/macos-clang-arm64"
            source.parent.mkdir(parents=True)
            source.write_text("first\n")
            installed = engine.install_conan_profile(root)
            self.assertTrue(installed.read_text().startswith("first\n"))
            self.assertIn("tools.apple:sdk_path=", installed.read_text())
            self.assertIn("tools.build:cxxflags=", installed.read_text())
            source.write_text("updated\n")
            self.assertTrue(engine.install_conan_profile(root).read_text().startswith("updated\n"))

    def test_sdk_selection_tracks_clang_and_rejects_missing_sdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "toolchain/bin/clang++"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            (root / "toolchain/include/c++/v1").mkdir(parents=True)
            selected = patch.object(engine, "clang_cxx", return_value=compiler)
            selected.start()
            self.addCleanup(selected.stop)
            first = root / "SDK with spaces"
            second = root / "relocated SDK"
            first.mkdir()
            second.mkdir()
            for sdk in (first, second):
                (sdk / "SDKSettings.json").write_text("{}")
            (root / "toolchain/include/c++/v1/__config").write_text("fixture")
            with patch.dict(engine.os.environ, {"SDKROOT": ""}), \
                    patch.object(engine, "capture_command_quiet", return_value=(0, f'"-isysroot" "{first}"')):
                engine.configure_macos_sdk(root, LLVM_MAJOR)
            link = root / "out/host-tools/macos-sdk"
            self.assertEqual(link.resolve(), first.resolve())
            with patch.dict(engine.os.environ, {"SDKROOT": str(second)}), \
                    patch.object(engine, "probe_macos_sdk", return_value=(0, "")):
                engine.configure_macos_sdk(root, LLVM_MAJOR)
                engine.configure_macos_sdk(root, LLVM_MAJOR)
            self.assertEqual(link.resolve(), second.resolve())
            with patch.dict(engine.os.environ, {"SDKROOT": str(root / "missing")}):
                with self.assertRaises(engine.EngineError):
                    engine.configure_macos_sdk(root, LLVM_MAJOR)
            self.assertEqual(link.resolve(), second.resolve())

    def test_incompatible_default_sdk_falls_back_to_validated_sibling(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "llvm/bin/clang++"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            (root / "llvm/include/c++/v1").mkdir(parents=True)
            newest = root / "SDKs/MacOSX27.0.sdk"
            older = root / "SDKs/MacOSX26.5.sdk"
            for sdk, version in ((newest, "27.0"), (older, "26.5")):
                sdk.mkdir(parents=True)
                (sdk / "SDKSettings.json").write_text('{"Version": "' + version + '"}')
            with patch.dict(engine.os.environ, {"SDKROOT": ""}), \
                    patch.object(engine, "clang_cxx", return_value=compiler), \
                    patch.object(engine, "capture_command_quiet", return_value=(0, f'"-isysroot" "{newest}"')), \
                    patch.object(engine, "probe_macos_sdk", side_effect=[(1, "NAN missing"), (0, "")]) as probe:
                engine.configure_macos_sdk(root, LLVM_MAJOR)
            self.assertEqual((root / "out/host-tools/macos-sdk").resolve(), older.resolve())
            self.assertEqual([call.args[1] for call in probe.call_args_list], [newest.resolve(), older.resolve()])

    def test_explicit_incompatible_sdk_fails_before_changing_valid_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "llvm/bin/clang++"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            (root / "llvm/include/c++/v1").mkdir(parents=True)
            old = root / "validated SDK"
            new = root / "incompatible SDK"
            old.mkdir()
            new.mkdir()
            link = root / "out/host-tools/macos-sdk"
            engine.link_or_replace_symlink(link, old)
            with patch.dict(engine.os.environ, {"SDKROOT": str(new)}), \
                    patch.object(engine, "clang_cxx", return_value=compiler), \
                    patch.object(engine, "probe_macos_sdk", return_value=(1, "NAN missing")) as probe:
                with self.assertRaisesRegex(engine.EngineError, "Explicit SDKROOT is incompatible") as failure:
                    engine.configure_macos_sdk(root, LLVM_MAJOR)
            self.assertIn("SDKROOT=/absolute/path", str(failure.exception))
            self.assertIn("before dependency downloads", str(failure.exception))
            self.assertEqual(probe.call_count, 1)
            self.assertEqual(link.resolve(), old.resolve())

    def test_no_compatible_installed_sdk_reports_recovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "llvm/bin/clang++"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            (root / "llvm/include/c++/v1").mkdir(parents=True)
            sdk = root / "SDKs/MacOSX27.sdk"
            sdk.mkdir(parents=True)
            with patch.dict(engine.os.environ, {"SDKROOT": ""}), \
                    patch.object(engine, "clang_cxx", return_value=compiler), \
                    patch.object(engine, "capture_command_quiet", return_value=(0, f'"-isysroot" "{sdk}"')), \
                    patch.object(engine, "probe_macos_sdk", return_value=(1, "NAN missing")):
                with self.assertRaisesRegex(engine.EngineError, "No installed compatible") as failure:
                    engine.configure_macos_sdk(root, LLVM_MAJOR)
            self.assertIn("Changing only the deployment target", str(failure.exception))
            self.assertFalse((root / "out/host-tools/macos-sdk").exists())

    def test_probe_uses_exact_headers_and_bounded_compile_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "SDK with spaces"
            libcxx = root / "libcxx with spaces"
            sdk.mkdir()
            libcxx.mkdir()
            (sdk / "SDKSettings.json").write_text("{}")
            (libcxx / "__config").write_text("fixture")
            with patch.object(engine, "capture_command_quiet", return_value=(0, "")) as capture:
                engine.probe_macos_sdk(root, sdk, libcxx)
            command = capture.call_args.args[0]
            self.assertIn("-nostdinc++", command)
            self.assertIn(root / "SDK with spaces", command)
            self.assertIn(root / "libcxx with spaces", command)
            self.assertIn("-o", command)
            self.assertEqual(capture.call_args.kwargs["timeout"], 30)
            self.assertIn("LudusSDK_missing_NAN", capture.call_args.kwargs["input_text"])

    def test_probe_requires_cfloat_limits_after_cmath(self):
        # libc++ 22 with the macOS 27 SDK leaves <cfloat> empty after <cmath>
        # (llvm/llvm-project#175849); the probe must reject that pairing.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "SDKSettings.json").write_text("{}")
            (root / "__config").write_text("fixture")
            with patch.object(engine, "capture_command_quiet", return_value=(0, "")) as capture:
                engine.probe_macos_sdk(root, root, root)
            source = capture.call_args.kwargs["input_text"]
            self.assertLess(source.index("#include <cmath>"), source.index("#include <cfloat>"))
            self.assertIn("#ifndef DBL_MAX_10_EXP\n#error LudusSDK_missing_DBL_MAX_10_EXP", source)

    def test_sdk_recovery_names_the_pinned_llvm(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compiler = root / "llvm/bin/clang++"
            compiler.parent.mkdir(parents=True)
            compiler.touch()
            with patch.dict(engine.os.environ, {"SDKROOT": str(root)}), \
                    patch.object(engine, "clang_cxx", return_value=compiler):
                with self.assertRaisesRegex(engine.EngineError, "Restore llvm@23 "):
                    engine.configure_macos_sdk(root, 23)

    def test_homebrew_setup_installs_llvm_and_lld_for_the_pinned_major(self):
        self.assertEqual(engine.homebrew_llvm_formulae(23), (("llvm@23", "clang"), ("lld@23", "ld.lld")))

    def test_homebrew_alias_drift_to_another_major_is_rejected(self):
        # llvm@<major> aliases the unversioned formula until Homebrew versions
        # it, so `brew upgrade` can move it to the next major.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tool = root / "opt/llvm/bin/clang"
            tool.parent.mkdir(parents=True)
            tool.touch()
            with patch.object(engine, "executable_version", return_value="Homebrew clang version 24.1.0"):
                with self.assertRaisesRegex(engine.EngineError, "llvm@23 resolves to .* LLVM 24.1.0"):
                    engine.check_homebrew_llvm_major(root, "llvm@23", tool, 23)
            with patch.object(engine, "executable_version", return_value="Homebrew clang version 23.1.3"):
                engine.check_homebrew_llvm_major(root, "llvm@23", tool, 23)
            engine.check_homebrew_llvm_major(root, "lld@23", root / "missing/ld.lld", 23)

    def test_bootstrap_identity_changes_when_sdk_moves_at_same_link(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first SDK"
            second = root / "second SDK"
            for sdk in (first, second):
                sdk.mkdir()
                (sdk / "SDKSettings.json").write_text("{}")
            link = root / "out/host-tools/macos-sdk"
            engine.link_or_replace_symlink(link, first)
            with patch.object(engine.platform, "system", return_value="Darwin"):
                before = engine.bootstrap_fingerprint(root)
                engine.link_or_replace_symlink(link, second)
                after = engine.bootstrap_fingerprint(root)
            self.assertNotEqual(before, after)

    def test_cache_repair_tracks_sdk_changes_and_accepts_equivalent_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            preset = "macos-clang-development"
            cache = engine.build_dir_for_preset(root, preset) / "CMakeCache.txt"
            cache.parent.mkdir(parents=True)
            cache.write_text("\n".join(
                f"{key}:FILEPATH={path.resolve()}" for key, path in (
                    ("CMAKE_CXX_COMPILER", engine.clang_cxx(root)),
                    ("CMAKE_MAKE_PROGRAM", engine.ninja(root)),
                    ("CMAKE_TOOLCHAIN_FILE", root / "out/conan" / preset / "conan_toolchain.cmake"))))
            stamp = cache.parent / ".ludus-macos-toolchain.json"
            stamp.write_text(json.dumps(engine.macos_toolchain_identity(root)))
            self.assertEqual(engine.cmake_cache_repair_reasons(root, preset), [])
            first = root / "first SDK"
            first.mkdir()
            engine.link_or_replace_symlink(root / "out/host-tools/macos-sdk", first)
            self.assertIn("macOS SDK/compiler/libc++ inputs changed or have not been recorded",
                          engine.cmake_cache_repair_reasons(root, preset))

    def test_all_mac_profiles_have_build_types_and_host_defaults(self):
        for flavor in ("debug", "development", "profile", "release", "asan-ubsan"):
            self.assertIn("macos-clang-" + flavor, engine.PRESET_BUILD_TYPES)
        args = init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--cli"]))
        self.assertEqual(args.preset, engine.DEFAULT_PRESET)
        self.assertTrue(all(name.startswith(engine.NATIVE_PRESET_PREFIX) for name in engine.HOST_PRESETS))
