"""Setup regressions use real CMake preset discovery, fake build execution."""
import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ludus_tools.create import create_project
from ludus_tools.errors import ToolingError
from ludus_tools import project_setup as setup
from ludus_tools.toolchain import PINNED_LLVM_MAJOR, pinned_llvm_tool
from test_ludus_tools import _manifest_json, _write_sdk_prefix


def write_translator(root: Path, sdk: Path, *, binary: Path | None = None) -> Path:
    """Independent verified-tool fixture; never invokes a downloaded executable."""
    binary = binary or root / "out/shader-tools/spirv-cross/bin/spirv-cross"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_text("#!/bin/sh\nexit 0\n")
    binary.chmod(0o755)
    pin = Path(__file__).resolve().parents[2] / "config/spirv_cross_toolchain.json"
    destination = sdk / "lib/cmake/Ludus/shaders/spirv_cross_toolchain.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(pin.read_bytes())
    record = json.loads(pin.read_text())["spirv_cross"]
    built = {key: record[key] for key in ("tag", "commit", "source_sha256")}
    built["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    Path(str(binary) + ".build.json").write_text(json.dumps(built))
    return binary


class ProjectSetupTests(unittest.TestCase):
    profile = "linux-clang-development"

    def manifest(self):
        from ludus_tools.identity import _detect_compiler
        return _manifest_json(compiler_version=_detect_compiler(str(self.tools / "out/host-tools/bin/clang++"))[1])

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name).resolve()
        self.tools = self.root / "tools"
        cmake = os.environ.get("LUDUS_TEST_CMAKE", shutil.which("cmake"))
        for name in ("cmake", "ctest", "ninja"):
            path = self.tools / "out/host-tools/venv/bin" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(cmake if name == "cmake" else shutil.which(name) or cmake)
        path = self.tools / "out/host-tools/bin/clang++"
        path.parent.mkdir(parents=True)
        path.symlink_to(shutil.which(pinned_llvm_tool("clang++")) or shutil.which("clang++"))
        self.sdk = _write_sdk_prefix(self.root / "sdk", self.manifest())
        self.project = self.root / "Game"
        create_project(self.project, name="Game", template_id="minimal", engine_version="", local_sdk_prefix=self.sdk, preset=self.profile)
        self.calls = []

    def runner(self, argv, *, cwd, env):
        self.calls.append(list(argv))
        if "--version" in argv or any(arg.startswith("--list-presets=") for arg in argv):
            result = subprocess.run(argv, cwd=cwd, env=env, capture_output=True, text=True)
            if result.returncode:
                raise ToolingError("InvalidProject", result.stderr)
            return result.stdout
        return ""

    def repair(self):
        return setup.repair_project(self.project, tooling_root=self.tools, sdk=self.sdk, runner=self.runner)

    def snapshot(self):
        return {str(p.relative_to(self.project)): p.read_bytes() for p in self.project.rglob("*") if p.is_file()}

    def test_automatic_repair_reuses_selection_without_preparing(self):
        prepared = []
        selected = setup.select_repair_sdk(self.project, tooling_root=self.tools,
                                          prepare=lambda p: prepared.append(p))
        self.assertEqual(selected, self.sdk)
        self.assertEqual(prepared, [])

    def test_automatic_repair_prepares_only_unresolved_projects(self):
        with patch.object(setup, "resolve_project", side_effect=ToolingError("UnresolvedLock", "new project")), \
             patch("ludus_tools.creation_engine.select_creation_sdk", return_value=self.sdk) as select:
            callback = lambda p: p
            self.assertEqual(setup.select_repair_sdk(self.project, tooling_root=self.tools, prepare=callback), self.sdk)
            select.assert_called_once_with(self.tools, profile=self.profile, prepare=callback)
        for code in ("SdkNotFound", "SdkCorrupt", "SdkIncompatible", "LockMismatch"):
            with patch.object(setup, "resolve_project", side_effect=ToolingError(code, "selected engine failed")), \
                 patch("ludus_tools.creation_engine.select_creation_sdk") as select:
                with self.assertRaises(ToolingError):
                    setup.select_repair_sdk(self.project, tooling_root=self.tools)
                select.assert_not_called()

    def test_fresh_clone_missing_local_setup_and_open_are_read_only(self):
        before = self.snapshot()
        with self.assertRaisesRegex(ToolingError, "setup is missing"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertEqual(before, self.snapshot())
        self.assertEqual([], self.calls)
        self.repair()
        before = self.snapshot()
        self.calls.clear()
        setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertEqual(before, self.snapshot())
        self.assertEqual(6, len(self.calls))
        self.assertTrue(all("--list-presets=" in a[-1] or a[-1] == "--version" for a in self.calls))

    def test_checked_identity_is_visible_and_read_only(self):
        self.repair()
        before = self.snapshot()
        self.calls.clear()
        result = setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertIn("Selected engine: Ludus " + self.manifest()["version"], result)
        self.assertIn(self.manifest()["source_revision"], result)
        self.assertIn("Installation: " + str(self.sdk.resolve()), result)
        self.assertIn("(local override)", result)
        self.assertEqual(before, self.snapshot())
        self.assertTrue(all("--list-presets=" in a[-1] or a[-1] == "--version" for a in self.calls))

    def test_repeated_repair_preserves_custom_presets_and_editor_settings(self):
        custom = {"name": "my-build", "configurePreset": self.profile}
        setup.write_object(self.project / "CMakeUserPresets.json", {"version": 6, "buildPresets": [custom]})
        setup.write_object(self.project / ".vscode/settings.json", {"editor.tabSize": 8})
        self.repair()
        before = self.snapshot()
        self.repair()
        self.assertEqual(before, self.snapshot())
        self.assertIn(custom, setup.read_object(self.project / "CMakeUserPresets.json")["buildPresets"])
        self.assertEqual(8, setup.read_object(self.project / ".vscode/settings.json")["editor.tabSize"])
        self.assertTrue(any("--fresh" in a for a in self.calls))
        self.assertTrue(any(Path(a[0]).name == "ctest" for a in self.calls))

    def test_moved_sdk_tools_and_changed_cache_require_repair(self):
        self.repair()
        moved = self.root / "new-sdk"
        self.sdk.rename(moved)
        before = self.snapshot()
        with self.assertRaises(ToolingError):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.assertEqual(before, self.snapshot())
        self.sdk = moved
        self.repair()
        moved_tools = self.root / "new-tools"
        self.tools.rename(moved_tools)
        self.tools = moved_tools
        with self.assertRaisesRegex(ToolingError, "inputs changed"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.repair()
        cache = self.project / "out/build" / self.profile / "CMakeCache.txt"
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text("CMAKE_PREFIX_PATH:STRING=/gone/sdk\n")
        with self.assertRaisesRegex(ToolingError, "Stale CMake cache"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_hidden_condition_and_missing_test_are_checked_by_cmake(self):
        self.repair()
        path = self.project / "CMakeUserPresets.json"
        local = setup.read_object(path)
        local["testPresets"] = []
        setup.write_object(path, local)
        with self.assertRaisesRegex(ToolingError, "selectable test"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.repair()
        local = setup.read_object(path)
        local["configurePresets"][0]["condition"] = False
        setup.write_object(path, local)
        with self.assertRaisesRegex(ToolingError, "selectable configure"):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_play_builds_select_the_repaired_owned_presets_and_cmake(self):
        from ludus_tools.cmake_setup import validate_project_presets
        self.repair()
        chosen = setup.cmake_for(self.project, self.profile, "/missing/system-cmake")
        self.assertEqual(str(self.tools / "out/host-tools/venv/bin/cmake"), chosen)
        validate_project_presets(chosen, self.project, setup.environment(self.tools), self.profile)
        local = setup.read_object(self.project / "CMakeUserPresets.json")
        local["buildPresets"] = []
        setup.write_object(self.project / "CMakeUserPresets.json", local)
        with self.assertRaisesRegex(ValueError, "selectable build preset.*ludus-local-"):
            validate_project_presets(chosen, self.project, setup.environment(self.tools), self.profile)

    def test_web_profiles_use_the_sdk_target_and_real_selectable_presets(self):
        web = _write_sdk_prefix(self.root / "web-sdk", _manifest_json(target_triple="wasm32-unknown-emscripten"))
        toolchain = self.tools / "out/host-tools/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
        toolchain.parent.mkdir(parents=True)
        toolchain.write_text("# prepared toolchain\n")
        write_translator(self.tools, self.sdk)
        path = self.project / "CMakePresets.json"
        base = setup.read_object(path)
        base["configurePresets"].extend([
            {"name": name + "-base", "hidden": True, "generator": "Ninja"}
            for name in ("web-emscripten-development", "web-emscripten-release")])
        setup.write_object(path, base)
        setup.repair_project(self.project, tooling_root=self.tools, sdk=self.sdk, web_sdk=web, runner=self.runner)
        self.assertTrue(any("ludus-local-web-emscripten-release" in a and "--build" in a for a in self.calls))
        self.assertFalse(any("ludus-local-web-emscripten-release" in a and Path(a[0]).name == "ctest" for a in self.calls))

    def test_stale_tool_version_is_reported_before_modifying_settings(self):
        path = self.tools / "out/host-tools/bin/clang++"
        path.unlink()
        path.write_text("#!/bin/sh\necho clang version 17.0.0\n")
        path.chmod(0o755)
        before = self.snapshot()
        with self.assertRaisesRegex(ToolingError, f"pinned version {PINNED_LLVM_MAJOR}"):
            self.repair()
        self.assertEqual(before, self.snapshot())

    def test_desktop_only_repair_clears_missing_saved_web_sdk_and_preserves_custom_presets(self):
        self.repair()
        saved = setup.read_object(self.project / ".ludus/setup.json")
        saved["web_sdk"] = str(self.root / "missing-browser-sdk")
        setup.write_object(self.project / ".ludus/setup.json", saved)
        path = self.project / "CMakeUserPresets.json"
        local = setup.read_object(path)
        web_profile = "web-emscripten-development"
        for key in ("configurePresets", "buildPresets"):
            local[key].append({"name": "ludus-local-" + web_profile, "vendor": {setup.OWNER: web_profile}})
        custom = {"name": "custom", "configurePreset": self.profile}
        local["buildPresets"].append(custom)
        setup.write_object(path, local)
        with self.assertRaises(ToolingError):
            setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)
        self.calls.clear()
        # The saved native override suffices: desktop-only repair needs no paths.
        setup.repair_project(self.project, tooling_root=self.tools, disable_web=True, runner=self.runner)
        self.assertEqual("", setup.read_object(self.project / ".ludus/setup.json")["web_sdk"])
        local = setup.read_object(path)
        self.assertIn(custom, local["buildPresets"])
        self.assertFalse(any("web-emscripten" in p["name"] for key in ("configurePresets", "buildPresets") for p in local[key]))
        self.assertTrue(any("--fresh" in argv for argv in self.calls))
        self.assertTrue(any(Path(argv[0]).name == "ctest" for argv in self.calls))
        self.calls.clear()
        setup.repair_project(self.project, tooling_root=self.tools, disable_web=True, runner=self.runner)
        setup.check_project(self.project, tooling_root=self.tools, runner=self.runner)

    def test_disabling_browser_and_selecting_web_sdk_is_rejected_without_writes(self):
        before = self.snapshot()
        with self.assertRaisesRegex(ToolingError, "desktop-only"):
            setup.repair_project(self.project, tooling_root=self.tools, sdk=self.sdk,
                                 web_sdk=self.sdk, disable_web=True, runner=self.runner)
        self.assertEqual(before, self.snapshot())
        self.assertEqual([], self.calls)

    def test_invalid_settings_and_custom_collision_are_preserved(self):
        path = self.project / ".vscode/settings.json"
        path.parent.mkdir()
        path.write_text("{broken")
        before = self.snapshot()
        with self.assertRaises(ToolingError):
            self.repair()
        self.assertEqual(before, self.snapshot())
        path.unlink()
        setup.write_object(self.project / "CMakeUserPresets.json", {"configurePresets": [{"name": "ludus-local-" + self.profile}]})
        before = self.snapshot()
        with self.assertRaisesRegex(ToolingError, "Custom preset"):
            self.repair()
        self.assertEqual(before, self.snapshot())

    def test_failed_and_cancelled_creation_leave_no_destination_or_stage(self):
        destination = self.root / "new"
        def fail(_):
            raise RuntimeError("configure failed")
        with self.assertRaises(RuntimeError):
            create_project(destination, name="New", template_id="minimal", engine_version="", local_sdk_prefix=self.sdk, verify_staged=fail)
        self.assertFalse(destination.exists())
        self.assertFalse(list(self.root.glob(".new.staging-*")))
        def cancel():
            raise ToolingError("Cancelled", "cancelled")
        with self.assertRaises(ToolingError):
            create_project(destination, name="New", template_id="minimal", engine_version="", local_sdk_prefix=self.sdk, cancel_check=cancel)
        self.assertFalse(destination.exists())


class StaleRevisionWarningTests(unittest.TestCase):
    def test_matching_or_unknown_revision_is_silent(self):
        root = Path(__file__).resolve().parents[2]
        head = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        self.assertEqual(setup.stale_revision_warning(root, head[:7], root), "")
        self.assertEqual(setup.stale_revision_warning(root, None, root), "")

    def test_different_revision_warns(self):
        root = Path(__file__).resolve().parents[2]
        text = setup.stale_revision_warning(root, "0000000", Path("/sdk"))
        self.assertIn("built from 0000000", text)


if __name__ == "__main__":
    unittest.main()
