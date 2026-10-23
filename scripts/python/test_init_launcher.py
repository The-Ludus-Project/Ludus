"""Startup selection must bypass Apple's old Python before opening the GUI."""
import json
import os
from pathlib import Path
import platform
import plistlib
import shlex
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import unittest
from unittest.mock import patch

import init_launcher


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "config").mkdir()
        (self.root / "config/tool_versions.json").write_text(json.dumps({"minimum": {"python": "3.10.0"}}))

    def test_macos_prefers_prepared_interpreter(self):
        with patch.object(init_launcher.platform, "system", return_value="Darwin"), \
                patch.object(init_launcher.os, "get_exec_path", return_value=[]):
            candidates = init_launcher.interpreter_candidates(self.root)
        self.assertEqual(candidates[0], str(self.root / "out/host-tools/venv/bin/python"))

    def test_old_default_falls_back_to_supported_versioned_python(self):
        old = self.root / "python3"
        modern = self.root / "python3.12"
        old.touch()
        modern.touch()
        with patch.object(init_launcher, "interpreter_candidates", return_value=[str(old), str(modern)]), \
                patch.object(init_launcher.subprocess, "run", side_effect=[
                    subprocess.CompletedProcess([], 0, stdout="[3, 9, 6]"),
                    subprocess.CompletedProcess([], 0, stdout="[3, 12, 15]")]):
            self.assertEqual(init_launcher.select_interpreter(self.root), str(modern))

    def test_fresh_checkout_discovers_versioned_python_on_path(self):
        binaries = self.root / "bin"
        binaries.mkdir()
        old = binaries / "python3"
        modern = binaries / "python3.12"
        config = binaries / "python3.12-config"
        for path in (old, modern, config):
            path.touch()
            path.chmod(0o755)
        with patch.object(init_launcher.platform, "system", return_value="Darwin"), \
                patch.object(init_launcher.sys, "executable", str(old)), \
                patch.object(init_launcher.os, "get_exec_path", return_value=[str(binaries)]), \
                patch.object(init_launcher.subprocess, "run", side_effect=[
                    subprocess.CompletedProcess([], 0, stdout="[3, 9, 6]"),
                    subprocess.CompletedProcess([], 0, stdout="[3, 12, 15]")]) as run:
            self.assertEqual(init_launcher.select_interpreter(self.root), str(modern))
        self.assertEqual(run.call_count, 2)

    def test_broken_or_unsupported_interpreters_fail_with_setup_instruction(self):
        broken = self.root / "broken-python"
        broken.touch()
        for result in (subprocess.TimeoutExpired([], 5), OSError("missing library"),
                       subprocess.CompletedProcess([], 0, stdout="[3, 9, 6]")):
            with self.subTest(result=result), \
                    patch.object(init_launcher, "interpreter_candidates", return_value=[str(broken)]), \
                    patch.object(init_launcher.subprocess, "run") as run:
                if isinstance(result, Exception):
                    run.side_effect = result
                else:
                    run.return_value = result
                with self.assertRaisesRegex(RuntimeError, "Python 3.10.0 or later.*rerun ./init.sh"):
                    init_launcher.select_interpreter(self.root)

    def test_launcher_forwards_flags_and_arguments_without_starting_setup_itself(self):
        with patch.object(init_launcher.platform, "system", return_value="Linux"), \
                patch.object(init_launcher, "select_interpreter", return_value="/prepared/python"), \
                patch.object(init_launcher.sys, "argv", ["init_launcher.py", "--gui", "--preset", "macos-clang-debug"]), \
                patch.object(init_launcher.os, "execv") as execute:
            init_launcher.main()
        executable, arguments = execute.call_args.args
        self.assertEqual(executable, "/prepared/python")
        self.assertEqual(arguments[2:], ["init", "--gui", "--preset", "macos-clang-debug"])

    def test_macos_window_setup_runs_from_named_application(self):
        with patch.object(init_launcher.platform, "system", return_value="Darwin"), \
                patch.object(init_launcher, "select_interpreter", return_value="/prepared/python"), \
                patch.object(init_launcher, "prepare_macos_application", return_value="/app/ludus-setup"), \
                patch.object(init_launcher.sys, "argv", ["init_launcher.py", "--gui"]), \
                patch.dict(init_launcher.os.environ, {}, clear=False), \
                patch.object(init_launcher.os, "execv") as execute:
            init_launcher.main()
            launcher = init_launcher.os.environ.get("__PYVENV_LAUNCHER__")
        executable, arguments = execute.call_args.args
        self.assertEqual(executable, "/app/ludus-setup")
        self.assertEqual(arguments[0], "/app/ludus-setup")
        self.assertEqual(arguments[2:], ["init", "--gui"])
        self.assertEqual(launcher, "/prepared/python")

    def test_macos_terminal_setup_or_unavailable_application_runs_interpreter(self):
        for flags, application in ((["--cli"], "/app/ludus-setup"), (["--ci"], "/app/ludus-setup"), ([], None)):
            with self.subTest(flags=flags, application=application):
                with patch.object(init_launcher.platform, "system", return_value="Darwin"), \
                        patch.object(init_launcher, "select_interpreter", return_value="/prepared/python"), \
                        patch.object(init_launcher, "prepare_macos_application",
                                     return_value=application) as prepare, \
                        patch.object(init_launcher.sys, "argv", ["init_launcher.py", *flags]), \
                        patch.object(init_launcher.os, "execv") as execute:
                    init_launcher.main()
                self.assertEqual(execute.call_args.args[0], "/prepared/python")
                self.assertEqual(prepare.called, flags == [])

    def fake_framework(self, name):
        prefix = self.root / name / "Python.framework/Versions/3.12"
        executable = prefix / "Resources/Python.app/Contents/MacOS/Python"
        executable.parent.mkdir(parents=True)
        executable.write_text("")
        return prefix, executable

    def test_application_bundle_names_setup_and_follows_interpreter(self):
        bundle = self.root / "out/init/Ludus Setup.app/Contents"
        for name in ("3.12.14", "3.12.15"):
            with self.subTest(interpreter=name):
                prefix, target = self.fake_framework(name)
                with patch.object(init_launcher.subprocess, "run",
                                  return_value=subprocess.CompletedProcess([], 0, stdout=str(prefix) + "\n")):
                    executable = init_launcher.prepare_macos_application(self.root, "/prepared/python")
                self.assertEqual(executable, str(bundle / "MacOS/ludus-setup"))
                self.assertEqual(os.readlink(executable), str(target))
                information = plistlib.loads((bundle / "Info.plist").read_bytes())
                self.assertEqual(information["CFBundleName"], "Ludus Setup")
                self.assertEqual(information["CFBundleExecutable"], "ludus-setup")
        self.assertEqual(sorted(path.name for path in (bundle / "MacOS").iterdir()), ["ludus-setup"])

    def test_non_framework_or_broken_interpreter_runs_without_application(self):
        for result in (subprocess.CompletedProcess([], 0, stdout=str(self.root) + "\n"),
                       subprocess.CompletedProcess([], 1, stdout=""),
                       OSError("missing")):
            with self.subTest(result=result):
                with patch.object(init_launcher.subprocess, "run") as run:
                    if isinstance(result, Exception):
                        run.side_effect = result
                    else:
                        run.return_value = result
                    self.assertIsNone(init_launcher.prepare_macos_application(self.root, "/prepared/python"))
        self.assertFalse((self.root / "out/init").exists())

    @unittest.skipUnless(platform.system() == "Darwin" and sysconfig.get_config_var("PYTHONFRAMEWORK"),
                         "requires a macOS framework Python")
    def test_application_runs_the_selected_interpreter(self):
        executable = init_launcher.prepare_macos_application(self.root, sys.executable)
        if executable is None:
            self.skipTest("this framework Python has no Python.app")
        environment = os.environ.copy()
        environment["__PYVENV_LAUNCHER__"] = sys.executable
        result = subprocess.run([executable, "-I", "-c", "import json,sys; print(json.dumps([sys.executable, sys.prefix]))"],
                                env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), [sys.executable, sys.prefix])

    @unittest.skipUnless(shutil.which("bash"), "shell launcher requires bash")
    def test_shell_sdk_choice_survives_actual_python_launcher(self):
        source = Path(init_launcher.__file__).resolve().parents[2]
        shutil.copy2(source / "init.sh", self.root)
        scripts = self.root / "scripts/python"
        scripts.mkdir(parents=True)
        shutil.copy2(source / "scripts/python/init_launcher.py", scripts)
        (scripts / "engine.py").write_text(
            'import json,os\nprint(json.dumps({"sdk":os.environ.get("SDKROOT"),'
            '"transport":os.environ.get("LUDUS_INIT_SHELL_SDKROOT")}))\n')
        binaries = self.root / "bin"
        binaries.mkdir()
        # Exercise Apple's injection on macOS, not just a mocked environment.
        python = "/usr/bin/python3" if platform.system() == "Darwin" else sys.executable
        bootstrap = binaries / "python3"
        bootstrap.write_text('#!/bin/sh\n'
                             'if [ "${SDKROOT+x}" = x ]; then exit 72; fi\n'
                             f'exec {shlex.quote(python)} "$@"\n')
        bootstrap.chmod(0o755)
        prepared = self.root / "out/host-tools/venv/bin/python"
        prepared.parent.mkdir(parents=True)
        prepared.symlink_to(sys.executable)
        for choice in (None, "", "/SDK with spaces/explicit.sdk"):
            with self.subTest(choice=choice):
                env = os.environ.copy()
                env["PATH"] = str(binaries) + os.pathsep + env.get("PATH", "")
                env.pop("SDKROOT", None)
                if choice is not None:
                    env["SDKROOT"] = choice
                result = subprocess.run(["bash", str(self.root / "init.sh"), "--cli"],
                                        env=env, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {"sdk": choice or None, "transport": None})


if __name__ == "__main__":
    unittest.main()
