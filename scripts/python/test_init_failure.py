"""Failure explanations for ./init.sh: hint table, log tail, log viewer and reports."""
from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

import engine
import init_failure
import init_progress


# One representative output line per hint; every table entry must appear here.
SAMPLES = {
    "disk_full": "OSError: [Errno 28] No space left on device: 'out/build/linux-clang-development'",
    "stale_venv": "ModuleNotFoundError: No module named 'encodings'",
    "xcode_command_line_tools": "xcrun: error: invalid active developer path (/Library/Developer/CommandLineTools)",
    "missing_compiler": "CMake Error: No CMAKE_CXX_COMPILER could be found.",
    "sudo_password": "sudo: a password is required",
    "apt_lock": "E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 1234 (unattended-upgr)",
    "conan_remote": "ERROR: Package 'zlib/1.3.1' not resolved: Unable to find 'zlib/1.3.1' in remotes",
    "network": "urllib3.exceptions.MaxRetryError: HTTPSConnectionPool(host='center2.conan.io', port=443): "
               "Max retries exceeded",
    "apt": "error: command failed with exit code 100: sudo apt-get install -y ninja-build",
    "homebrew": "error: command failed with exit code 1: /opt/homebrew/bin/brew install llvm@23",
}


class HintTableTests(unittest.TestCase):
    def test_every_hint_has_a_sample_and_unique_key(self):
        keys = [hint.key for hint in init_failure.FAILURE_HINTS]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(set(keys), set(SAMPLES))

    def test_each_sample_selects_its_hint(self):
        for key, line in SAMPLES.items():
            with self.subTest(key=key):
                hint = init_failure.find_hint(["+ some command", line, "error: command failed with exit code 1"])
                self.assertIsNotNone(hint)
                self.assertEqual(hint.key, key)

    def test_python_fatal_error_from_an_upgraded_interpreter(self):
        lines = ["Fatal Python error: init_fs_encoding: failed to get the Python codec of the filesystem encoding",
                 "Python runtime state: core initialized"]
        self.assertEqual(init_failure.find_hint(lines).key, "stale_venv")

    def test_specific_cause_outranks_the_generic_package_manager_failure(self):
        lines = ["dpkg: error processing archive: No space left on device",
                 "error: command failed with exit code 100: sudo apt-get install -y clang-23"]
        self.assertEqual(init_failure.find_hint(lines).key, "disk_full")

    def test_unrecognised_output_has_no_hint(self):
        self.assertIsNone(init_failure.find_hint(["error: command failed with exit code 1: conan install"]))
        self.assertIsNone(init_failure.find_hint([]))

    def test_only_recent_lines_are_searched(self):
        old = [SAMPLES["disk_full"]] + ["ordinary output"] * init_failure.HINT_SCAN_LINES
        self.assertIsNone(init_failure.find_hint(old))

    def test_advice_is_plain_and_actionable(self):
        for hint in init_failure.FAILURE_HINTS:
            with self.subTest(key=hint.key):
                self.assertTrue(hint.advice.endswith("."))
                self.assertIn("retry", hint.advice.lower())


class TailAndReportTests(unittest.TestCase):
    def test_tail_keeps_the_last_lines_without_trailing_blanks(self):
        lines = [f"line {i}" for i in range(50)] + ["", "  "]
        last = init_failure.tail(lines)
        self.assertEqual(len(last), init_failure.FAILURE_TAIL_LINES)
        self.assertEqual(last[-1], "line 49")
        self.assertEqual(init_failure.tail(["a", "b"], count=5), ["a", "b"])
        self.assertEqual(init_failure.tail(["a"], count=0), [])

    def test_read_log_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "setup.log"
            self.assertEqual(init_failure.read_log_lines(path), [])
            path.write_bytes(b"first\nsecond \xff\n")
            self.assertEqual(init_failure.read_log_lines(path), ["first", "second \ufffd"])

    def test_report_lines(self):
        hint = init_failure.find_hint([SAMPLES["network"]])
        report = init_failure.report_lines(step_label="Conan install (all native presets)", error="conan failed",
                                           hint=hint, log_path=Path("out/init/setup.log"), last_lines=["x", "y"])
        self.assertEqual(report[0], "Setup failed during: Conan install (all native presets)")
        self.assertIn("Error: conan failed", report)
        self.assertIn(init_failure.HINT_PREFIX + hint.advice, report)
        self.assertIn(init_failure.LOG_PREFIX + "out/init/setup.log", report)
        self.assertEqual(report[-3:], [init_failure.LAST_LINES_HEADING, "  x", "  y"])
        self.assertEqual(init_failure.report_lines(step_label=None, error="", hint=None, log_path=None),
                         ["Setup failed."])


class OpenLogTests(unittest.TestCase):
    def test_open_command_per_platform(self):
        path = Path("/tmp/setup.log")
        self.assertEqual(init_failure.open_command(path, "Darwin"), ["open", "/tmp/setup.log"])
        self.assertEqual(init_failure.open_command(path, "Linux"), ["xdg-open", "/tmp/setup.log"])
        self.assertIsNone(init_failure.open_command(path, "Windows"))

    def test_open_in_viewer(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "setup.log"
            popen = MagicMock()
            self.assertFalse(init_failure.open_in_viewer(path, system="Linux", popen=popen))
            popen.assert_not_called()
            path.write_text("log\n", encoding="utf-8")
            self.assertTrue(init_failure.open_in_viewer(path, system="Linux", popen=popen))
            self.assertEqual(popen.call_args.args[0], ["xdg-open", str(path)])
            self.assertTrue(popen.call_args.kwargs["start_new_session"])
            start_file = MagicMock()
            self.assertTrue(init_failure.open_in_viewer(path, system="Windows", start_file=start_file))
            start_file.assert_called_once_with(str(path))
            missing = MagicMock(side_effect=FileNotFoundError("xdg-open"))
            self.assertFalse(init_failure.open_in_viewer(path, system="Linux", popen=missing))


class TerminalReportTests(unittest.TestCase):
    def test_terminal_failure_prints_step_and_hint(self):
        steps = [init_progress.Step("system", "Installing system prerequisites")]

        def failing_init(_args):
            init_progress.start(steps, environment={})
            try:
                init_progress.begin("system")
                init_progress.fail()
                raise engine.EngineError("macOS setup requires Homebrew and Xcode Command Line Tools; install llvm")
            finally:
                init_progress.stop()

        output = io.StringIO()
        with patch.dict("os.environ", {init_progress.PROGRESS_ENVIRONMENT: ""}), \
                patch("init_ui.prepare_init", side_effect=lambda args, _engine: args), \
                contextlib.redirect_stdout(output):
            parser_args = engine.make_parser().parse_args(["init", "--cli"])
            parser_args.func = failing_init
            with patch.object(engine, "make_parser") as make_parser:
                make_parser.return_value.parse_args.return_value = parser_args
                self.assertEqual(engine.main(["init", "--cli"]), 1)
        text = output.getvalue()
        self.assertIn("Setup stopped at step 1/1: Installing system prerequisites", text)
        self.assertIn(init_failure.HINT_PREFIX + "The Xcode Command Line Tools", text)

    def test_captured_output_of_a_failed_command_feeds_the_hint(self):
        init_failure._recent_output.clear()
        init_failure.remember_output("Resolving deps\nTemporary failure in name resolution\n")
        try:
            hint = init_failure.find_hint([*init_failure.recent_output(), "command failed with exit code 1: x"])
            self.assertEqual(hint.key, "network")
        finally:
            init_failure._recent_output.clear()


if __name__ == "__main__":
    unittest.main()
