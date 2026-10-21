"""macOS native setup contracts; actual installed CLI journey runs in native CI."""
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ludus_tools import creation_engine, native, operations, project_setup as setup
from ludus_tools.create import create_project
from ludus_tools.errors import ToolingError
from ludus_tools.identity import load_prefix_manifest
from ludus_tools.sdkstore import SdkStore, default_store_root
from test_ludus_tools import _manifest_json, _write_sdk_prefix
import test_project_setup as fixtures


def mac_manifest(**changes):
    return _manifest_json(target_triple="arm64-apple-darwin", cxx_runtime_abi="libc++",
                          distro_baseline="macos-14.0", **changes)


class MacosProjectSetupTests(fixtures.ProjectSetupTests):
    # Reuse every setup contract: read-only open, fresh clone, moved tools/SDK,
    # real CMake discovery, custom settings, repeated repair and failed creation.
    profile = "macos-clang-development"

    def manifest(self):
        from ludus_tools.identity import _detect_compiler
        return mac_manifest(compiler_version=_detect_compiler(str(self.tools / "out/host-tools/bin/clang++"))[1])

    def setUp(self):
        architecture = patch("ludus_tools.native.platform.machine", return_value="arm64")
        architecture.start()
        self.addCleanup(architecture.stop)
        super().setUp()
        for directory, marker in (("macos-sdk", "SDKSettings.json"), ("libcxx-include", "__config")):
            path = self.tools / "out/host-tools" / directory
            path.mkdir()
            (path / marker).write_text("prepared toolchain fixture\n")
        # Exercise Mac preset structure with real CMake even on the Linux unit
        # runner. Native acceptance separately verifies the Darwin condition.
        path = self.project / "CMakePresets.json"
        presets = setup.read_object(path)
        for preset in presets["configurePresets"]:
            if preset["name"].startswith("macos-"):
                preset["condition"] = True
        setup.write_object(path, presets)

    def test_mac_flags_match_sdk_and_avoid_producer_conan_paths(self):
        producer = self.tools / "out/conan" / self.profile
        producer.mkdir(parents=True)
        self.repair()
        cache = setup.read_object(self.project / "CMakeUserPresets.json")["configurePresets"][0]["cacheVariables"]
        self.assertEqual(str(self.sdk), cache["CMAKE_PREFIX_PATH"])
        self.assertEqual("14.0", cache["CMAKE_OSX_DEPLOYMENT_TARGET"])
        self.assertEqual("arm64", cache["CMAKE_OSX_ARCHITECTURES"])
        self.assertIn("-nostdinc++", cache["CMAKE_CXX_FLAGS"])
        self.assertIn(str(self.tools / "out/host-tools/libcxx-include"), cache["CMAKE_CXX_FLAGS"])

    def test_canonical_sysroot_cache_does_not_reject_fresh_setup(self):
        sysroot = self.tools / "out/host-tools/macos-sdk"
        actual = self.root / "Apple SDK"
        sysroot.rename(actual)
        sysroot.symlink_to(actual)
        self.repair()
        cache = setup.read_object(self.project / "CMakeUserPresets.json")["configurePresets"][0]["cacheVariables"]
        self.assertEqual(str(actual.resolve()), cache["CMAKE_OSX_SYSROOT"])
        # Reproduce CMake's observed canonical cache value from the native journey.
        cached = self.project / "out/build" / self.profile / "CMakeCache.txt"
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_text("".join(f"{key}:STRING={value}\n" for key, value in cache.items()))
        before = self.snapshot()
        setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertEqual(before, self.snapshot())

    def test_retargeted_sysroot_symlink_requires_explicit_repair(self):
        sysroot = self.tools / "out/host-tools/macos-sdk"
        original = self.root / "sdk-one"
        sysroot.rename(original)
        sysroot.symlink_to(original)
        self.repair()
        replacement = self.root / "sdk-two"
        shutil.copytree(original, replacement)
        sysroot.unlink()
        sysroot.symlink_to(replacement)
        before = self.snapshot()
        with self.assertRaisesRegex(ToolingError, "inputs changed"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertEqual(before, self.snapshot())
        self.repair()
        setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_stale_mac_deployment_cache_is_reported(self):
        self.repair()
        _, records, _ = setup._inputs(self.tools, self.project, self.sdk, self.profile, None)
        values = records[self.profile]["cacheVariables"]
        cache = self.project / "out/build" / self.profile / "CMakeCache.txt"
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text("".join(f"{key}:STRING={value}\n" for key, value in values.items()).replace("TARGET:STRING=14.0", "TARGET:STRING=13.0"))
        with self.assertRaisesRegex(ToolingError, "CMAKE_OSX_DEPLOYMENT_TARGET"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_missing_macos_sdk_is_actionable_and_preserves_project(self):
        shutil.rmtree(self.tools / "out/host-tools/macos-sdk")
        before = self.snapshot()
        with self.assertRaisesRegex(ToolingError, "run ./init.sh"):
            self.repair()
        self.assertEqual(before, self.snapshot())

    def test_metal_shader_setup_does_not_require_spirv_validator(self):
        path = self.project / "CMakeLists.txt"
        path.write_text(path.read_text() + "\n# ludus_compile_shader metal consumer\n")
        slang = self.tools / "out/shader-tools/slang/bin/slangc"
        slang.parent.mkdir(parents=True)
        slang.write_text("#!/bin/sh\nexit 0\n")
        slang.chmod(0o755)
        _, records, _ = setup._inputs(self.tools, self.project, self.sdk, self.profile, None)
        self.assertIn("LUDUS_SLANG_COMPILER", records[self.profile]["cacheVariables"])
        self.assertNotIn("LUDUS_SPIRV_VALIDATOR", records[self.profile]["cacheVariables"])

    def test_web_presets_do_not_inherit_mac_runtime_flags(self):
        self.test_web_profiles_use_the_sdk_target_and_real_selectable_presets()
        presets = setup.read_object(self.project / "CMakeUserPresets.json")
        for preset in presets["configurePresets"]:
            if "web-emscripten" in preset["name"]:
                cache = preset["cacheVariables"]
                self.assertFalse(any(key.startswith("CMAKE_OSX_") for key in cache))
                self.assertNotIn("CMAKE_CXX_FLAGS", cache)
                self.assertNotIn("CMAKE_CXX_COMPILER", cache)

    def test_multiple_native_flavors_keep_owned_presets_and_separate_checks(self):
        self.repair()
        manifest = self.manifest()
        manifest["build_flavor"] = "Debug"
        debug = _write_sdk_prefix(self.root / "debug-sdk", manifest)
        setup.repair_project(self.project, tooling_root=self.tools, sdk=debug,
                             profile="macos-clang-debug", runner=self.runner)
        for profile in (self.profile, "macos-clang-debug"):
            setup.check_project(self.project, tooling_root=self.tools, profile=profile, runner=self.runner)
            self.assertEqual("ludus-local-" + profile, setup.preset_for(self.project, profile))
        self.repair()
        setup.check_project(self.project, tooling_root=self.tools, profile="macos-clang-debug", runner=self.runner)

    def test_same_flavor_linux_override_does_not_shadow_selected_mac_target(self):
        from ludus_tools.lockfile import parse_local_settings_file
        linux = _write_sdk_prefix(self.root / "linux-sdk", _manifest_json())
        path = self.project / ".ludus/local.json"
        settings = parse_local_settings_file(path)
        settings.set_override("x86_64-linux-gnu", "Development", str(linux))
        path.write_bytes(settings.serialize())
        resolved = operations.resolve_project(self.project, store=SdkStore(self.root / "store"),
                                              enforce_host_toolchain=False)
        self.assertEqual(self.sdk, resolved.resolution.prefix)

    def test_operations_probe_owned_compiler_and_use_requested_profile(self):
        self.repair()
        expected = str(self.tools / "out/host-tools/bin/clang++")
        with patch("ludus_tools.identity.detect_host_toolchain", side_effect=lambda identity, **kw: identity) as detect:
            resolved = operations.resolve_project(self.project, store=SdkStore(self.root / "store"))
        self.assertEqual(expected, detect.call_args.kwargs["cxx"])
        self.assertTrue(detect.call_args.kwargs["strict"])
        # Select a different profile explicitly; descriptor's launch/build preset
        # must follow it, rather than retaining Development's CMake arguments.
        debug = _write_sdk_prefix(self.root / "debug-sdk", mac_manifest(build_flavor="Debug"))
        resolved = operations.resolve_project(self.project, store=SdkStore(self.root / "store"),
            profile="macos-clang-debug", cli_sdk_prefix=debug, enforce_host_toolchain=False)
        self.assertEqual("macos-clang-debug", resolved.descriptor.preset)
        self.assertEqual("macos-clang-debug", resolved.paths.build_dir.name)


class NativeIdentityTests(unittest.TestCase):
    def test_arm64_and_intel_mac_identities_and_incompatible_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for architecture in ("arm64", "x86_64"):
                manifest = mac_manifest()
                manifest["target_triple"] = architecture + "-apple-darwin"
                sdk = _write_sdk_prefix(root / architecture, manifest)
                with patch("ludus_tools.native.platform.machine", return_value=architecture):
                    native.validate_target(load_prefix_manifest(sdk), "macos-clang-development")
                    selected = creation_engine.select_creation_sdk(root, explicit=sdk, profile="macos-clang-development", environ={})
                    self.assertEqual(sdk.resolve(), selected)
                other = "x86_64" if architecture == "arm64" else "arm64"
                with patch("ludus_tools.native.platform.machine", return_value=other):
                    with self.assertRaisesRegex(ToolingError, "SDK target"):
                        native.validate_target(load_prefix_manifest(sdk), "macos-clang-development")

    def test_mac_release_creation_is_explicit_and_uses_app_entry(self):
        from ludus_tools.release_model import load_release
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = _write_sdk_prefix(root / "sdk", mac_manifest())
            destination = root / "Game"
            create_project(destination, name="Game", template_id="minimal", engine_version="",
                           local_sdk_prefix=sdk, release=True)
            profile = load_release(destination).profiles["macos-release"]
            self.assertEqual("Game.app/Contents/MacOS/Game", profile.entry_point)
            self.assertEqual("macos-clang-release", profile.configure_preset)

    def test_host_default_and_store_precedence(self):
        with patch("ludus_tools.native.platform.system", return_value="Darwin"):
            self.assertEqual("macos-clang-development", native.default_profile())
            with patch.dict(os.environ, {}, clear=True):
                self.assertEqual(Path.home() / "Library/Application Support/Ludus", default_store_root())
            with patch.dict(os.environ, {"XDG_DATA_HOME": "/portable"}, clear=True):
                self.assertEqual(Path("/portable/ludus"), default_store_root())
            with patch.dict(os.environ, {"LUDUS_SDK_STORE": "/chosen"}, clear=True):
                self.assertEqual(Path("/chosen"), default_store_root())

    def test_explicit_debug_preparation_uses_mac_profile_and_separate_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            profile = "macos-clang-debug"
            manifest = mac_manifest(build_flavor="Debug")
            commands = []
            def runner(argv, **kwargs):
                commands.append(argv)
                if Path(argv[0]).name == "build":
                    generated = root / "out/build" / profile / "cmake/LudusSdkManifest.json"
                    generated.parent.mkdir(parents=True)
                    generated.write_text(json.dumps(manifest))
                if "--install" in argv:
                    _write_sdk_prefix(Path(argv[-1]), manifest)
            selected = creation_engine.prepare_creation_sdk(root, root / "legacy", runner=runner, profile=profile)
            self.assertEqual(profile, commands[0][1])
            build = next(command for command in commands if Path(command[0]).name == "build")
            self.assertEqual(profile, build[1])
            self.assertEqual(Path(commands[1][1]).name, "shader-probe")
            self.assertEqual(Path(commands[2][1]).name, "bootstrap-spirv-cross")
            self.assertLess(commands.index(commands[2]), commands.index(build))
            self.assertIn("--no-system-install", commands[0])
            self.assertTrue(selected.name.startswith(profile + "-"))
            self.assertFalse((root / "legacy").exists())
            self.assertIn("bundle-sdk-dependencies", commands[-1])


if __name__ == "__main__":
    unittest.main()
