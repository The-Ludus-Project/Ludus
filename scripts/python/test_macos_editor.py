"""macOS Editor setup policy and Darwin process ownership regressions."""
from __future__ import annotations

import ctypes
import errno
import os
from pathlib import Path
import platform
import signal
import sys
import tempfile
import time
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

import editor_launch
import editor_tool
import engine
import init_editor
import init_ui


class SetupTests(unittest.TestCase):
    def test_automatic_create_and_repair_prepare_with_managed_sdk(self):
        from ludus_tools import creation_engine, project_setup
        for operation_name in ("project_create", "project_setup"):
            for prepared in (True, False):
                with self.subTest(operation=operation_name, prepared=prepared), tempfile.TemporaryDirectory() as temporary:
                    root = Path(temporary)
                    sdk = root / "Apple SDK with spaces"
                    sdk.mkdir()
                    if prepared:
                        link = root / "out/host-tools/macos-sdk"
                        link.parent.mkdir(parents=True)
                        link.symlink_to(sdk, target_is_directory=True)
                    context = editor_tool.ToolContext(root, engine, None)
                    operation = editor_tool.Operation(context, Mock(), "0000000000000001",
                        operation_name, root / "Game", "", -1,
                        options={"sdk": "", "web_sdk": "", "name": "Game", "prepare_engine": False})
                    commands = []
                    def run_stage(stage, argv, cwd, env, code):
                        commands.append((argv, env.copy()))
                    def select(root, *, prepare, **kwargs):
                        return prepare(root / "engine-install")
                    descriptor = SimpleNamespace(preset="macos-clang-development") if operation_name == "project_setup" else None
                    with patch.object(platform, "system", return_value="Darwin"), \
                         patch.dict(os.environ, {"SDKROOT": "/incompatible/MacOSX27.sdk",
                                                 "LUDUS_INIT_SHELL_SDKROOT": "/stale/sdk"}), \
                         patch.object(operation, "_check_release_cancel"), \
                         patch.object(operation, "_run_stage", side_effect=run_stage), \
                         patch.object(creation_engine, "select_creation_sdk", side_effect=select), \
                         patch.object(project_setup, "select_repair_sdk", side_effect=lambda path, *, tooling_root, prepare: prepare(root / "engine-install")), \
                         patch.object(creation_engine, "prepared_sdk_prefix", return_value=root / "engine-install"), \
                         patch.object(project_setup, "supported_project"), \
                         patch.object(project_setup, "repair_project", return_value="Verified"), \
                         patch("ludus_tools.create.create_project"):
                        self.assertEqual(operation._execute_setup(descriptor).outcome, "success")
                        self.assertEqual(os.environ["SDKROOT"], "/incompatible/MacOSX27.sdk")
                    self.assertEqual(len(commands), 4)
                    self.assertEqual(Path(commands[0][0][0]).name, "init")
                    for _, env in commands:
                        self.assertEqual(env.get("SDKROOT"), str(sdk.resolve()) if prepared else None)
                        self.assertNotIn("LUDUS_INIT_SHELL_SDKROOT", env)
                        self.assertEqual(env["CI"], "true")

    def test_repair_uses_prepared_sdk_instead_of_inherited_sdkroot(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "SDK with spaces"
            sdk.mkdir()
            link = root / "out/host-tools/macos-sdk"
            link.parent.mkdir(parents=True)
            link.symlink_to(sdk, target_is_directory=True)
            with patch.object(platform, "system", return_value="Darwin"), \
                 patch.dict(os.environ, {"SDKROOT": "/incompatible/MacOSX27.sdk",
                                         "LUDUS_INIT_SHELL_SDKROOT": "/stale/sdk"}):
                env = editor_tool.engine_setup_environment(root, engine)
                self.assertEqual(env["SDKROOT"], str(sdk.resolve()))
                self.assertNotIn("LUDUS_INIT_SHELL_SDKROOT", env)
                self.assertEqual(os.environ["SDKROOT"], "/incompatible/MacOSX27.sdk")
                link.unlink()
                env = editor_tool.engine_setup_environment(root, engine)
                self.assertNotIn("SDKROOT", env)
                self.assertNotIn("LUDUS_INIT_SHELL_SDKROOT", env)

    def test_linux_repair_preserves_environment(self):
        with patch.object(platform, "system", return_value="Linux"), \
             patch.dict(os.environ, {"SDKROOT": "/custom/sdk"}):
            self.assertEqual(editor_tool.engine_setup_environment(Path("/tmp"), engine)["SDKROOT"], "/custom/sdk")

    def args(self, preset="macos-clang-development", *options):
        return engine.make_parser().parse_args(["init", "--cli", "--with-editor", preset, *options])

    def test_macos_native_options_and_host_mismatch(self):
        for machine in ("arm64", "x86_64"):
            with patch.object(platform, "system", return_value="Darwin"), patch.object(platform, "machine", return_value=machine):
                for preset in ("macos-clang-debug", "macos-clang-development", "macos-clang-asan-ubsan"):
                    init_editor.validate_editor_options(self.args(preset), engine)
                for preset in ("linux-clang-development", "macos-clang-release", "web-emscripten-development"):
                    with self.assertRaises(engine.EngineError):
                        init_editor.validate_editor_options(self.args(preset), engine)
        self.assertIn("macos-clang-development", editor_launch.SUPPORTED_PRESETS)

    def test_older_macos_rejects_rendered_editor_before_shader_bootstrap(self):
        with patch.object(platform, "system", return_value="Darwin"), \
             patch.object(platform, "machine", return_value="arm64"), \
             patch.object(platform, "mac_ver", return_value=("14.7.1", (), "")), \
             patch.object(engine, "run") as run:
            with self.assertRaisesRegex(engine.EngineError, "macOS 15.*--no-editor-preview"):
                init_editor.prepare_editor_preview(self.args(), engine)
            run.assert_not_called()
            init_editor.validate_editor_options(self.args("macos-clang-development", "--no-editor-preview"), engine)
        for version in ("15.0", "26.0"):
            with self.subTest(version=version), patch.object(platform, "system", return_value="Darwin"), \
                 patch.object(platform, "machine", return_value="arm64"), \
                 patch.object(platform, "mac_ver", return_value=(version, (), "")):
                init_editor.validate_editor_options(self.args(), engine)

    def test_existing_qt_uses_local_cache_without_installing(self):
        with tempfile.TemporaryDirectory() as temporary:
            qt = Path(temporary) / "Qt with spaces"
            (qt / "lib/cmake/Qt6").mkdir(parents=True)
            with patch.object(platform, "system", return_value="Darwin"), patch.object(platform, "machine", return_value="arm64"), \
                 patch.object(init_editor.shutil, "which", return_value="/homebrew/bin/brew"), \
                 patch.object(engine, "capture_command_quiet", return_value=(0, str(qt))), \
                 patch.object(engine, "run") as run, patch.object(engine, "cmake_configure") as configure, \
                 patch.object(engine, "cmake_build") as build, patch.object(init_editor.cmake_targets, "query_codemodel"):
                init_editor.setup_editor(self.args("macos-clang-development", "--no-system-install"), engine)
                run.assert_not_called()
                self.assertEqual(configure.call_args.args[2], ["-DLUDUS_BUILD_EDITOR=ON", "-DLUDUS_EDITOR_RENDER_PREVIEW=ON",
                          *init_editor.preview_tool_args(engine.repo_root()), f"-DQt6_DIR={qt}/lib/cmake/Qt6"])
                self.assertEqual(build.call_args.args[1], "macos-clang-development")

    def test_preview_discovers_separate_homebrew_qt_quick_prefix(self):
        with tempfile.TemporaryDirectory() as temporary:
            qt_base = Path(temporary) / "Qt Base"
            qt_quick = Path(temporary) / "Qt Declarative"
            (qt_base / "lib/cmake/Qt6").mkdir(parents=True)
            (qt_quick / "lib/cmake/Qt6Quick").mkdir(parents=True)
            with patch.object(platform, "system", return_value="Darwin"), \
                 patch.object(platform, "machine", return_value="arm64"), \
                 patch.object(init_editor.shutil, "which", return_value="/brew"), \
                 patch.object(engine, "capture_command_quiet", side_effect=[(0, str(qt_base)), (0, str(qt_quick))]), \
                 patch.object(engine, "run") as run, patch.object(engine, "cmake_configure") as configure, \
                 patch.object(engine, "cmake_build"), patch.object(init_editor.cmake_targets, "query_codemodel"):
                init_editor.setup_editor(self.args("macos-clang-development", "--no-system-install"), engine)
                run.assert_not_called()
                self.assertIn(f"-DQt6_DIR={qt_base}/lib/cmake/Qt6", configure.call_args.args[2])
                self.assertIn(f"-DQt6Quick_DIR={qt_quick}/lib/cmake/Qt6Quick", configure.call_args.args[2])

    def test_preview_install_includes_qt_quick(self):
        with patch.object(platform, "system", return_value="Darwin"), patch.object(platform, "machine", return_value="arm64"), \
             patch.object(init_editor.shutil, "which", return_value="/brew"), \
             patch.object(engine, "capture_command_quiet", return_value=(1, "")), \
             patch.object(engine, "run") as run, patch.object(engine, "cmake_configure"), \
             patch.object(engine, "cmake_build"), patch.object(init_editor.cmake_targets, "query_codemodel"):
            init_editor.setup_editor(self.args(), engine)
            self.assertEqual(run.call_args.args[0], ["/brew", "install", "qtbase", "qtdeclarative"])


@unittest.skipUnless(sys.platform == "darwin", "Darwin libc process API")
class ProcessTests(unittest.TestCase):
    def test_observation_retains_zombie_until_finalize(self):
        child = editor_tool.OwnedProcess([sys.executable, "-c", "raise SystemExit(23)"], "/tmp", dict(os.environ))
        try:
            deadline = time.monotonic() + 5
            while child.wait_exit_nowait() is None and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(child.wait_exit_nowait(), (23, None))
            self.assertEqual(child.wait_exit_nowait(), (23, None))
            self.assertIsNone(child._proc.returncode)
            report = child.finalize_normal()
            self.assertTrue(report.confirmed)
            self.assertEqual(report.exit_code, 23)
            with self.assertRaises(ChildProcessError):
                os.waitpid(child.pid, os.WNOHANG)
        finally:
            if child._proc.returncode is None:
                child.cleanup(cancelled=True)

    def test_stopped_child_is_observed_and_can_be_cleaned(self):
        from editor_process_macos import process_stopped
        child = editor_tool.OwnedProcess([sys.executable, "-c", "import time; time.sleep(60)"], "/tmp", dict(os.environ))
        try:
            os.kill(child.pid, signal.SIGSTOP)
            deadline = time.monotonic() + 5
            while not process_stopped(child.pid) and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertTrue(process_stopped(child.pid))
            os.kill(child.pid, signal.SIGCONT)
            self.assertTrue(child.cleanup(cancelled=True).confirmed)
        finally:
            if child._proc.returncode is None:
                os.kill(child.pid, signal.SIGCONT)
                child.cleanup(cancelled=True)

    def test_inventory_failure_and_truncation_are_unknown(self):
        import editor_process_macos as native
        for count in (0, -1, 65536, 3):
            with patch.object(native, "_listpids", return_value=count):
                self.assertIsNone(native.group_members(123))
        def unreadable(_kind, _leader, pids, _capacity):
            pids[0], pids[1] = 123, 456
            return 8
        with patch.object(native, "_listpids", side_effect=unreadable), patch.object(native, "_pidinfo", return_value=0):
            ctypes.set_errno(errno.EPERM)
            # No info is not a successful proof of absence.
            self.assertIsNone(native.group_members(123))

    def test_unknown_inventory_never_reaps_leader(self):
        child = editor_tool.OwnedProcess([sys.executable, "-c", "raise SystemExit(0)"], "/tmp", dict(os.environ))
        try:
            with patch.object(child, "_group_members", return_value=None):
                report = child.cleanup(cancelled=True)
            self.assertFalse(report.confirmed)
            self.assertIsNone(child._proc.returncode)
            self.assertTrue(child.finalize_normal().confirmed)
        finally:
            if child._proc.returncode is None:
                child.cleanup(cancelled=True)
            if child in editor_tool._UNKNOWN_PROCESS_OWNERS:
                editor_tool._UNKNOWN_PROCESS_OWNERS.remove(child)


if __name__ == "__main__":
    unittest.main()
