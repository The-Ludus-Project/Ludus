"""A Python upgrade must not leave init reusing a virtual environment that cannot start."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import shutil

import engine
import init_launcher
import managed_venv

BUILT_VERSION = "3.12"
CURRENT_VERSION = "3.14"
IS_WINDOWS = os.name == "nt"
# The messages Python 3.14 printed for an environment copied from a removed 3.12.
REMOVED_STANDARD_LIBRARY_OUTPUT = """Could not find platform independent libraries <prefix>
Python path configuration:
  stdlib dir = '/usr/lib/python3.12'
Fatal Python error: init_fs_encoding: failed to get the Python codec of the filesystem encoding
Python runtime state: core initialized
ModuleNotFoundError: No module named 'encodings'
"""
EXECUTABLE_PERMISSIONS = 0o755
FAILED_EXIT_CODE = 1
SCRIPTS = Path(__file__).resolve().parents[1]
WRAPPERS = (("clangd", "editor_setup.py"), ("editor-task", "editor_setup.py"),
            ("setup-editor", "editor_setup.py"), ("setup-quality-tools", "tooling_quality.py"))


def probe_result(version=CURRENT_VERSION, pip="pip", return_code=0, error=""):
    return subprocess.CompletedProcess([], return_code, stdout=f"{version}\n{pip}\n", stderr=error)


class ManagedEnvironmentTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.host_tools = self.root / "out" / "host-tools"
        self.directory = self.host_tools / "venv"
        self.home = self.root / "python-home"
        self.home.mkdir()

    def write_environment(self, version=f"{BUILT_VERSION}.15", home=None, key="version"):
        interpreter = managed_venv.python_path(self.directory)
        interpreter.parent.mkdir(parents=True)
        interpreter.write_text("")
        (self.directory / managed_venv.CONFIGURATION_FILE_NAME).write_text(
            f"home = {home or self.home}\ninclude-system-site-packages = false\n{key} = {version}\n",
            encoding="utf-8",
        )
        return interpreter

    def test_version_parsing(self):
        self.assertEqual(managed_venv.major_minor("3.12.15"), "3.12")
        self.assertEqual(managed_venv.major_minor("3.14.4.final.0"), "3.14")
        self.assertEqual(managed_venv.major_minor("3"), "")
        self.assertEqual(managed_venv.major_minor("unknown"), "")
        self.assertEqual(managed_venv.recorded_version({"version_info": "3.12.15.final.0"}), "3.12")

    def test_missing_environment_is_not_present(self):
        health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION)
        self.assertFalse(health.is_present)
        self.assertFalse(health.is_usable)

    def test_mismatched_configuration_is_stale_without_running_it(self):
        self.write_environment()
        with patch.object(managed_venv.subprocess, "run") as runner:
            health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION, runner=runner)
        runner.assert_not_called()
        self.assertFalse(health.is_usable)
        self.assertEqual(health.reason, "built with Python 3.12, now 3.14")
        self.assertEqual(health.recorded_version, BUILT_VERSION)

    def test_virtualenv_version_info_key_is_read(self):
        self.write_environment(version=f"{BUILT_VERSION}.15.final.0", key="version_info")
        health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION)
        self.assertEqual(health.reason, "built with Python 3.12, now 3.14")

    def test_removed_base_interpreter_is_stale(self):
        self.write_environment(version=f"{CURRENT_VERSION}.0", home=self.root / "removed")
        health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION)
        self.assertIn("base Python directory no longer exists", health.reason)

    def test_missing_configuration_is_stale(self):
        self.directory.mkdir(parents=True)
        health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION)
        self.assertTrue(health.is_present)
        self.assertIn(managed_venv.CONFIGURATION_FILE_NAME, health.reason)

    @unittest.skipIf(IS_WINDOWS, "the fake interpreter is a POSIX shell script")
    def test_broken_interpreter_binary_is_stale(self):
        interpreter = self.write_environment(version=f"{CURRENT_VERSION}.0")
        report = self.root / "report.txt"
        report.write_text(REMOVED_STANDARD_LIBRARY_OUTPUT)
        interpreter.write_text(f"#!/bin/sh\ncat '{report}' >&2\nexit {FAILED_EXIT_CODE}\n")
        interpreter.chmod(EXECUTABLE_PERMISSIONS)
        health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION)
        self.assertFalse(health.is_usable)
        self.assertIn("its interpreter is broken (Fatal Python error: init_fs_encoding", health.reason)

    def test_probe_results(self):
        self.write_environment(version=f"{CURRENT_VERSION}.0")
        cases = (
            (probe_result(), True, ""),
            (probe_result(version=BUILT_VERSION), False, "runs Python 3.12, now 3.14"),
            (probe_result(pip="no-pip"), False, "pip is not installed"),
            (probe_result(return_code=FAILED_EXIT_CODE, error=REMOVED_STANDARD_LIBRARY_OUTPUT), False,
             "interpreter is broken"),
        )
        for result, is_usable, reason in cases:
            with self.subTest(reason=reason):
                health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION,
                                              runner=lambda *arguments, **options: result)
                self.assertEqual(health.is_usable, is_usable)
                self.assertIn(reason, health.reason)

    def test_unrunnable_interpreter_is_stale(self):
        self.write_environment(version=f"{CURRENT_VERSION}.0")

        def runner(*arguments, **options):
            raise OSError("Exec format error")

        health = managed_venv.inspect(self.directory, expected_version=CURRENT_VERSION, runner=runner)
        self.assertIn("could not run: Exec format error", health.reason)

    def fake_builder(self, built):
        def builder(directory):
            built.append(directory)
            self.write_environment(version=f"{CURRENT_VERSION}.4")
        return builder

    def test_stale_environment_is_removed_and_rebuilt_alone(self):
        self.write_environment()
        (self.directory / "lib-sentinel").write_text("old packages")
        sibling = self.host_tools / "bin" / "clang++"
        sibling.parent.mkdir(parents=True)
        sibling.write_text("managed shim")
        messages, notices, built = [], [], []
        is_rebuilt = managed_venv.ensure(
            self.directory, announce=messages.append, notify=notices.append, builder=self.fake_builder(built),
            expected_version=CURRENT_VERSION, runner=lambda *arguments, **options: probe_result())
        self.assertTrue(is_rebuilt)
        self.assertEqual(built, [self.directory])
        self.assertFalse((self.directory / "lib-sentinel").exists())
        self.assertTrue(sibling.is_file())
        self.assertEqual(notices, ["Recreating stale virtual environment (built with Python 3.12, now 3.14)"])
        self.assertEqual(messages, [f"{notices[0]}: {self.directory}"])

    def test_usable_environment_is_kept(self):
        self.write_environment(version=f"{CURRENT_VERSION}.0")
        built = []
        is_rebuilt = managed_venv.ensure(
            self.directory, announce=lambda message: None, builder=built.append,
            expected_version=CURRENT_VERSION, runner=lambda *arguments, **options: probe_result())
        self.assertFalse(is_rebuilt)
        self.assertEqual(built, [])

    def test_missing_environment_is_created_without_a_stale_notice(self):
        notices, built = [], []
        managed_venv.ensure(self.directory, announce=lambda message: None, notify=notices.append,
                            builder=self.fake_builder(built), expected_version=CURRENT_VERSION,
                            runner=lambda *arguments, **options: probe_result())
        self.assertEqual(built, [self.directory])
        self.assertEqual(notices, [])

    def test_rebuild_that_stays_broken_fails_clearly(self):
        with self.assertRaisesRegex(managed_venv.ManagedEnvironmentError, "new virtual environment.*unusable"):
            managed_venv.ensure(self.directory, announce=lambda message: None, builder=lambda directory: None,
                                expected_version=CURRENT_VERSION)

    @unittest.skipIf(IS_WINDOWS, "creating symbolic links needs privileges on Windows")
    def test_linked_environment_is_never_deleted(self):
        target = self.root / "elsewhere"
        target.mkdir()
        (target / "keep").write_text("user data")
        self.host_tools.mkdir(parents=True)
        self.directory.symlink_to(target, target_is_directory=True)
        with self.assertRaisesRegex(managed_venv.ManagedEnvironmentError, "not a plain directory"):
            managed_venv.ensure(self.directory, announce=lambda message: None, builder=lambda directory: None,
                                expected_version=CURRENT_VERSION)
        self.assertTrue((target / "keep").is_file())

    def test_running_interpreter_is_never_deleted(self):
        self.write_environment()
        with patch.object(managed_venv.sys, "prefix", str(self.directory)), \
                self.assertRaisesRegex(managed_venv.ManagedEnvironmentError, "running from it"):
            managed_venv.ensure(self.directory, announce=lambda message: None, builder=lambda directory: None,
                                expected_version=CURRENT_VERSION)
        self.assertTrue(self.directory.is_dir())

    def test_real_environment_with_an_older_recorded_version_is_rebuilt(self):
        managed_venv.create(self.directory)
        configuration = self.directory / managed_venv.CONFIGURATION_FILE_NAME
        current = managed_venv.current_version()
        configuration.write_text(configuration.read_text().replace(f"version = {current}", "version = 3.0.1"))
        self.assertIn("built with Python 3.0", managed_venv.inspect(self.directory).reason)
        self.assertTrue(managed_venv.ensure(self.directory, announce=lambda message: None))
        self.assertTrue(managed_venv.inspect(self.directory).is_usable)


class EngineIntegrationTests(unittest.TestCase):
    def test_engine_reports_progress_notice_and_engine_errors(self):
        root = Path("/repository")
        with patch.object(engine.managed_venv, "ensure",
                          side_effect=managed_venv.ManagedEnvironmentError("could not remove")) as ensure, \
                self.assertRaisesRegex(engine.EngineError, "could not remove"):
            engine.create_or_update_venv(root)
        directory = ensure.call_args.args[0]
        self.assertEqual(directory, engine.venv_dir(root))
        self.assertIs(ensure.call_args.kwargs["notify"], engine.init_progress.note)
        self.assertIs(ensure.call_args.kwargs["builder"], engine.build_venv)

    def test_progress_window_shows_the_recreation_notice(self):
        notice = "Recreating stale virtual environment (built with Python 3.12, now 3.14)"
        lines = []
        reporter = engine.init_progress.start([engine.init_progress.Step("host_tools", "Host tools")])
        self.addCleanup(engine.init_progress.stop)
        reporter.machine_readable = True
        reporter.write = lambda line, flush=False: lines.append(line)
        engine.init_progress.begin("host_tools")
        engine.init_progress.note(notice)
        progress = engine.init_progress.SetupProgress.from_plan(reporter.steps)
        for line in lines:
            self.assertFalse(progress.feed(line, 0.0))
        self.assertEqual(progress.note, notice)
        self.assertEqual(progress.steps[0].status, engine.init_progress.RUNNING)


@unittest.skipIf(IS_WINDOWS, "the wrappers are POSIX shell scripts")
class WrapperTests(unittest.TestCase):
    def test_wrappers_skip_a_managed_interpreter_that_cannot_start(self):
        for wrapper, script in WRAPPERS:
            with self.subTest(wrapper=wrapper), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                (root / "scripts/python").mkdir(parents=True)
                shutil.copy2(SCRIPTS / wrapper, root / "scripts" / wrapper)
                (root / "scripts/python" / script).write_text("import sys\nprint('ran', sys.argv[1:])\n")
                broken = root / "out/host-tools/venv/bin/python"
                broken.parent.mkdir(parents=True)
                broken.write_text(f"#!/bin/sh\necho 'No module named encodings' >&2\nexit {FAILED_EXIT_CODE}\n")
                broken.chmod(EXECUTABLE_PERMISSIONS)
                completed = subprocess.run(["bash", str(root / "scripts" / wrapper)], capture_output=True, text=True,
                                           check=False)
                self.assertEqual(completed.returncode, 0, completed.stderr)
                self.assertIn("ran", completed.stdout)


class LauncherTests(unittest.TestCase):
    def test_broken_prepared_interpreter_falls_back_to_the_system_python(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "config").mkdir()
            (root / "config/tool_versions.json").write_text('{"minimum": {"python": "3.10.0"}}')
            prepared = root / "out/host-tools/venv/bin/python"
            prepared.parent.mkdir(parents=True)
            prepared.touch()
            system = root / "python3.14"
            system.touch()
            with patch.object(init_launcher, "interpreter_candidates", return_value=[str(prepared), str(system)]), \
                    patch.object(init_launcher.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], FAILED_EXIT_CODE, stdout="",
                                                    stderr=REMOVED_STANDARD_LIBRARY_OUTPUT),
                        subprocess.CompletedProcess([], 0, stdout="[3, 14, 4]")]):
                self.assertEqual(init_launcher.select_interpreter(root), str(system))


if __name__ == "__main__":
    unittest.main()
