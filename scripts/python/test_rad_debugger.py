"""RAD tooling contract tests run without a desktop, downloads, or apt changes."""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import engine
import rad_debugger as rad


class RadTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="ludus rad ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "config").mkdir()
        self.data = json.loads((engine.repo_root() / "config" / "rad_debugger.json").read_text())
        (self.root / "config" / "rad_debugger.json").write_text(json.dumps(self.data))
        (self.root / "config" / "tool_versions.json").write_text(
            (engine.repo_root() / "config" / "tool_versions.json").read_text()
        )
        self.rad = self.make_executable(self.root / "external RAD")
        self.build = self.root / "out" / "build" / "linux-clang-debug"
        self.exe = self.make_executable(self.build / "apps" / "smoke" / "custom output")
        reply = self.build / ".cmake" / "api" / "v1" / "reply"
        reply.mkdir(parents=True)
        documents = {
            "index-1.json": {"reply": {"client-ludus-debug": {"codemodel-v2": {"jsonFile": "model.json"}}}},
            "model.json": {"configurations": [{"targets": [{"name": "smoke", "jsonFile": "target.json"}]}]},
            "target.json": {"type": "EXECUTABLE", "artifacts": [{"path": "apps/smoke/custom output"}]},
        }
        for name, content in documents.items():
            (reply / name).write_text(json.dumps(content))

    def make_executable(self, path: Path) -> Path:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
        return path

    def args(self, **changes: object) -> argparse.Namespace:
        values = dict(preset="linux-clang-debug", target="smoke", debugger=str(self.rad),
                      cwd=None, no_build=True, dry_run=True, arguments=["--level", "one", "$(literal)"])
        values.update(changes)
        return argparse.Namespace(**values)

    def launch(self, **changes: object) -> int:
        with patch.object(engine, "repo_root", return_value=self.root), contextlib.redirect_stdout(io.StringIO()):
            return rad.launch(self.args(**changes), engine)

    def session(self, binary: Path | None = None) -> Path:
        return (self.root / "out/debug/rad/linux-clang-debug/smoke/versions"
                / rad.debugger_identity(self.root, binary or self.rad))

    def install_fixture(self, data: dict[str, str]) -> Path:
        directory = rad.install_dir(self.root, data)
        binary = self.make_executable(directory / "source/build/raddbg")
        (directory / "build.json").write_text(json.dumps({"pin": data}))
        return binary

    def test_explicit_invalid_preference_does_not_fall_back(self) -> None:
        with patch.dict(os.environ, {"LUDUS_RAD_DEBUGGER": str(self.rad)}):
            self.assertEqual(rad.resolve_binary(self.root, engine), self.rad)
            with self.assertRaisesRegex(engine.EngineError, "preference is invalid"):
                rad.resolve_binary(self.root, engine, "/missing/rad")

    def test_managed_pin_is_required_and_external_fallback_is_available(self) -> None:
        directory = rad.install_dir(self.root, self.data)
        managed = self.make_executable(directory / "source" / "build" / "raddbg")
        self.assertIsNone(rad.managed_binary(self.root, engine))
        (directory / "build.json").write_text(json.dumps({"pin": self.data}))
        self.assertEqual(rad.managed_binary(self.root, engine), managed)
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(rad.resolve_binary(self.root, engine), managed)
            (directory / "build.json").write_text("broken json")
            with patch.object(rad.shutil, "which", return_value=str(self.rad)):
                self.assertEqual(rad.resolve_binary(self.root, engine), self.rad)

    def test_doctor_absence_is_optional(self) -> None:
        with patch.dict(os.environ, {}, clear=True), patch.object(rad.shutil, "which", return_value=None):
            status = rad.tool_status(self.root, engine)
        self.assertFalse(status.required)
        self.assertFalse(status.found)

    def test_file_api_resolves_custom_artifact_path_and_rejects_libraries(self) -> None:
        self.assertEqual(rad.target_executable(self.build, "smoke", engine), self.exe)
        with self.assertRaisesRegex(engine.EngineError, "does not resolve"):
            rad.target_executable(self.build, "missing", engine)
        detail = self.build / ".cmake/api/v1/reply/target.json"
        detail.write_text(json.dumps({"type": "STATIC_LIBRARY"}))
        with self.assertRaisesRegex(engine.EngineError, "not an executable"):
            rad.target_executable(self.build, "smoke", engine)

    def test_missing_or_invalid_file_api_has_actionable_errors(self) -> None:
        with self.assertRaisesRegex(engine.EngineError, "without --no-build"):
            rad.target_executable(self.root / "unconfigured", "smoke", engine)
        (self.build / ".cmake/api/v1/reply/index-1.json").write_text("{}")
        with self.assertRaisesRegex(engine.EngineError, "reconfigure"):
            rad.target_executable(self.build, "smoke", engine)

    def test_launch_preserves_project_and_user_state_and_records_argv(self) -> None:
        self.launch()
        session = self.session()
        project = session / "session.raddbg_project"
        user = session / "session.raddbg_user"
        project.write_text("// raddbg 0.9.29 project\nbreakpoint: {label: test}\n")
        user.write_text("// raddbg 0.9.29 user\nwindow: {}\n")
        self.launch(arguments=["--different"])
        self.assertIn("breakpoint", project.read_text())
        self.assertIn("window", user.read_text())
        description = json.loads((session / "launch.json").read_text())
        self.assertEqual(description["arguments"], ["--different"])
        self.assertEqual(description["executable"], str(self.exe))

    def test_managed_upgrade_and_rollback_keep_separate_session_state(self) -> None:
        old_binary = self.install_fixture(self.data)
        self.launch(debugger=str(old_binary))
        old_session = self.session(old_binary)
        project = old_session / "session.raddbg_project"
        project.write_text("old breakpoints\n")
        newer_pin = dict(self.data, version="upgrade-test", revision="a" * 40)
        (self.root / "config/rad_debugger.json").write_text(json.dumps(newer_pin))
        new_binary = self.install_fixture(newer_pin)
        with patch.dict(os.environ, {}, clear=True):
            self.launch(debugger=None)
        new_session = self.session(new_binary)
        self.assertNotEqual(new_session, old_session)
        (new_session / "session.raddbg_project").write_text("new format\n")
        self.assertEqual(project.read_text(), "old breakpoints\n")
        description = json.loads((new_session / "launch.json").read_text())
        self.assertEqual(description["managed_pin"], newer_pin)
        # Explicit rollback works even while the repository pin remains newer.
        self.launch(debugger=str(old_binary))
        self.assertEqual(project.read_text(), "old breakpoints\n")
        self.assertEqual((new_session / "session.raddbg_project").read_text(), "new format\n")

    def test_external_replacement_at_same_path_isolates_state_and_supports_rollback(self) -> None:
        original = self.rad.read_bytes()
        self.launch()
        old_session = self.session()
        project = old_session / "session.raddbg_project"
        project.write_text("old breakpoints\n")
        self.rad.write_text("#!/bin/sh\n# updated debugger\nexit 0\n")
        self.launch()
        new_session = self.session()
        self.assertNotEqual(new_session, old_session)
        self.assertNotIn("old breakpoints", (new_session / "session.raddbg_project").read_text())
        self.rad.write_bytes(original)
        self.launch()
        self.assertEqual(self.session(), old_session)
        self.assertEqual(project.read_text(), "old breakpoints\n")

    def test_real_subprocess_receives_separate_arguments_and_cwd(self) -> None:
        result = self.root / "observed.json"
        # This fake debugger observes the actual process boundary, including
        # spaces in executable/session paths and shell-looking argument data.
        self.rad.write_text(
            "#!/usr/bin/env python3\nimport json,os,sys\n"
            f"open({str(result)!r}, 'w').write(json.dumps([sys.argv, os.getcwd()]))\n"
        )
        cwd = self.root / "game content"
        cwd.mkdir()
        with patch.dict(os.environ, {"DISPLAY": ":test"}):
            self.launch(dry_run=False, cwd=str(cwd))
        argv, actual_cwd = json.loads(result.read_text())
        self.assertEqual(actual_cwd, str(cwd))
        self.assertEqual(argv[-3:], ["--level", "one", "$(literal)"])
        self.assertEqual(argv[-4], "./" + os.path.relpath(self.exe, cwd))
        self.assertEqual(argv[-5], "--")

    def test_unrepresentable_rad_arguments_fail_before_build(self) -> None:
        for value in ("", "two words", 'a"b', "a\tb", "a\nb", "nul\x00"):
            with self.subTest(value=value), self.assertRaisesRegex(engine.EngineError, "cannot preserve"):
                self.launch(arguments=[value])

    def test_headless_launch_fails_but_dry_run_succeeds(self) -> None:
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(engine.EngineError, "X11 display"):
                self.launch(dry_run=False)
            self.assertEqual(self.launch(), 0)

    def test_unsupported_presets_and_missing_artifacts_fail(self) -> None:
        for preset in ("linux-clang-release", "linux-clang-asan-ubsan", "web-emscripten-debug"):
            with self.subTest(preset=preset), self.assertRaisesRegex(engine.EngineError, "stepping presets"):
                self.launch(preset=preset)
        self.exe.unlink()
        with self.assertRaisesRegex(engine.EngineError, "not built"):
            self.launch()

    def test_session_lock_prevents_overwrite(self) -> None:
        import fcntl

        self.launch()
        session = self.root / "out/debug/rad/linux-clang-debug/smoke"
        with (session / "session.lock").open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.rad.write_text("#!/bin/sh\n# updated debugger\nexit 0\n")
            with self.assertRaisesRegex(engine.EngineError, "already open"):
                self.launch()

    def test_managed_launch_refuses_an_in_progress_install(self) -> None:
        import fcntl

        directory = rad.install_dir(self.root, self.data)
        binary = self.make_executable(directory / "source/build/raddbg")
        (directory / "build.json").write_text(json.dumps({"pin": self.data}))
        (self.root / "config/rad_debugger.json").write_text(json.dumps(
            dict(self.data, version="upgrade-test", revision="a" * 40)
        ))
        with (directory / "setup.lock").open("a") as lock, patch.dict(os.environ, {}, clear=True):
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            with self.assertRaisesRegex(engine.EngineError, "RAD setup is running"):
                self.launch(debugger=str(binary))

    def test_default_launch_builds_only_the_selected_target(self) -> None:
        with patch.object(engine, "ensure_bootstrap_for_preset"), \
                patch.object(engine, "cmake_configure") as configure, \
                patch.object(engine, "cmake_build") as build:
            self.launch(no_build=False)
        configure.assert_called_once_with(self.root, "linux-clang-debug")
        build.assert_called_once_with(self.root, "linux-clang-debug", ["--target", "smoke"])
        self.assertTrue((self.build / ".cmake/api/v1/query/client-ludus-debug/codemodel-v2").exists())

    def test_no_system_install_never_runs_apt_or_fetch(self) -> None:
        with patch.object(rad, "build_tools", return_value=(None, None, False)), \
                patch.object(engine, "run") as run, \
                self.assertRaisesRegex(engine.EngineError, "prerequisites"):
            rad.setup_locked(self.root, self.root, self.data, argparse.Namespace(no_system_install=True), engine)
        run.assert_not_called()

    def test_ubuntu_setup_registers_the_pinned_llvm_source_before_apt(self) -> None:
        import llvm_apt_source

        major = engine.llvm_major_version(engine.load_tool_versions(self.root))
        with patch.object(rad, "build_tools", return_value=(None, None, False)), \
                patch.object(engine, "host_supports_apt_install", return_value=True), \
                patch.object(engine, "host_is_ubuntu", return_value=True), \
                patch.object(engine, "apt_package_has_candidate", return_value=False), \
                patch.object(engine, "sudo_command_prefix", return_value=["sudo"]), \
                patch.object(llvm_apt_source, "os_release_codename", return_value="noble"), \
                patch.object(llvm_apt_source, "register") as register, \
                patch.object(engine, "run") as run, \
                self.assertRaisesRegex(engine.EngineError, "still unavailable"):
            rad.setup_locked(self.root, self.root, self.data, argparse.Namespace(no_system_install=False), engine)
        register.assert_called_once_with(self.root, "noble", major, ["sudo"])
        self.assertIn(f"clang-{major}", run.call_args_list[-1].args[0])

    def setup_fixture(self) -> Path:
        source = rad.install_dir(self.root, self.data) / "source"
        (source / ".git").mkdir(parents=True)
        return source

    def git_reply(self, argv: list[str], **kwargs: object) -> tuple[int, str]:
        if argv[1] == "remote":
            return 0, self.data["repository"]
        if argv[1] == "rev-parse":
            return 0, self.data["revision"]
        return 0, ""

    def test_setup_reuses_successful_pin_without_download_or_rebuild(self) -> None:
        source = self.setup_fixture()
        self.make_executable(source / "build/raddbg")
        (source.parent / "build.json").write_text(json.dumps(
            {"pin": self.data, "compiler": "/pinned/clang", "archiver": "/pinned/llvm-ar"}
        ))
        with patch.object(rad, "build_tools", return_value=("/pinned/clang", "/pinned/llvm-ar", True)), \
                patch.object(engine, "capture_command_quiet", side_effect=self.git_reply), \
                patch.object(engine, "run") as run, contextlib.redirect_stdout(io.StringIO()):
            rad.setup_locked(self.root, source.parent, self.data, argparse.Namespace(no_system_install=True), engine)
        run.assert_not_called()

    def test_failed_build_leaves_no_success_stamp_and_can_be_retried(self) -> None:
        source = self.setup_fixture()
        stamp = source.parent / "build.json"
        with patch.object(rad, "build_tools", return_value=("/pinned/clang", "/pinned/llvm-ar", True)), \
                patch.object(engine, "capture_command_quiet", side_effect=self.git_reply), \
                patch.object(engine, "run", side_effect=engine.EngineError("failed build")), \
                self.assertRaisesRegex(engine.EngineError, "failed build"):
            rad.setup_locked(self.root, source.parent, self.data, argparse.Namespace(no_system_install=True), engine)
        self.assertFalse(stamp.exists())
        with patch.object(rad, "build_tools", return_value=("/pinned/clang", "/pinned/llvm-ar", True)), \
                patch.object(engine, "capture_command_quiet", side_effect=self.git_reply), \
                patch.object(engine, "run", side_effect=lambda *a, **kw: self.make_executable(source / "build/raddbg")), \
                contextlib.redirect_stdout(io.StringIO()):
            rad.setup_locked(self.root, source.parent, self.data, argparse.Namespace(no_system_install=True), engine)
        self.assertEqual(json.loads(stamp.read_text())["pin"], self.data)

    def test_setup_preserves_unexpected_source_edits(self) -> None:
        source = self.setup_fixture()

        def dirty_reply(argv: list[str], **kwargs: object) -> tuple[int, str]:
            return (0, " M build.sh") if argv[1] == "status" else self.git_reply(argv)

        with patch.object(rad, "build_tools", return_value=("/pinned/clang", "/pinned/llvm-ar", True)), \
                patch.object(engine, "capture_command_quiet", side_effect=dirty_reply), \
                patch.object(engine, "run") as run, self.assertRaisesRegex(engine.EngineError, "clean pinned checkout"):
            rad.setup_locked(self.root, source.parent, self.data, argparse.Namespace(no_system_install=True), engine)
        run.assert_not_called()

    def test_failed_upgrade_preserves_previous_install_without_silent_downgrade(self) -> None:
        old_pin = self.data.copy()
        old_binary = self.install_fixture(old_pin)
        old_stamp = old_binary.parent.parent.parent / "build.json"
        old_contents = old_stamp.read_bytes()
        self.data = dict(old_pin, version="upgrade-test", revision="a" * 40)
        (self.root / "config/rad_debugger.json").write_text(json.dumps(self.data))
        source = self.setup_fixture()
        with patch.object(rad, "build_tools", return_value=("/pinned/clang", "/pinned/llvm-ar", True)), \
                patch.object(engine, "capture_command_quiet", side_effect=self.git_reply), \
                patch.object(engine, "run", side_effect=engine.EngineError("failed upgrade")), \
                self.assertRaisesRegex(engine.EngineError, "failed upgrade"):
            rad.setup_locked(self.root, source.parent, self.data, argparse.Namespace(no_system_install=True), engine)
        self.assertTrue(rad.executable(old_binary))
        self.assertEqual(old_stamp.read_bytes(), old_contents)
        self.assertFalse((source.parent / "build.json").exists())
        with patch.dict(os.environ, {}, clear=True), patch.object(rad.shutil, "which", return_value=None):
            with self.assertRaisesRegex(engine.EngineError, "Pinned RAD upgrade-test"):
                rad.resolve_binary(self.root, engine)
            self.assertEqual(rad.resolve_binary(self.root, engine, str(old_binary)), old_binary)
            (self.root / "config/rad_debugger.json").write_text(json.dumps(old_pin))
            self.assertEqual(rad.resolve_binary(self.root, engine), old_binary)

    def test_init_opt_in_and_argument_separator(self) -> None:
        parser = engine.make_parser()
        self.assertFalse(parser.parse_args(["init"]).with_rad_debugger)
        self.assertTrue(parser.parse_args(["init", "--with-rad-debugger", "--no-system-install"]).with_rad_debugger)
        args = parser.parse_args(["debug", "--no-build", "linux-clang-debug", "smoke", "--", "--help"])
        self.assertEqual(args.arguments, ["--help"])
        self.assertTrue(args.no_build)

    def test_browser_init_does_not_silently_ignore_rad_opt_in(self) -> None:
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = engine.main(["init", "web-emscripten-debug", "--with-rad-debugger"])
        self.assertEqual(result, 1)
        self.assertIn("setup-rad-debugger separately", output.getvalue())


if __name__ == "__main__":
    unittest.main()
