"""Editor command-line exits return from main instead of calling exit() (issue 2188).

QCommandLineParser::process() calls exit() for an unknown option or --help. That
skipped LogSystem::Shutdown() and aborted in FoundationLogging's teardown, so
the editor died with SIGABRT and a crash report instead of an error status.
Runs the built editor offscreen with a private home directory, so a regression
never touches the developer's settings or crash-report database.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EDITOR = Path(sys.argv.pop(1)).resolve()
# Startup before parsing is Qt and logging initialization; an abort is immediate.
LAUNCH_TIMEOUT_SECONDS = 60


def run_editor(*arguments: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="ludus-editor-command-line-") as home:
        environment = dict(os.environ)
        environment.update(
            {
                "QT_QPA_PLATFORM": "offscreen",
                "HOME": home,
                "XDG_CONFIG_HOME": str(Path(home) / "config"),
                "XDG_DATA_HOME": str(Path(home) / "data"),
                "XDG_CACHE_HOME": str(Path(home) / "cache"),
            }
        )
        return subprocess.run(
            [str(EDITOR), *arguments],
            env=environment,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=LAUNCH_TIMEOUT_SECONDS,
            check=False,
        )


class CommandLineExitTests(unittest.TestCase):
    def test_unknown_option_exits_with_error_status(self) -> None:
        result = run_editor("--no-such-option")
        # A negative code is a signal (SIGABRT from std::terminate was -6).
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Unknown option 'no-such-option'.", result.stderr)
        self.assertIn("Run with --help to list the supported options.", result.stderr)
        self.assertNotIn("terminating", result.stderr)

    def test_missing_option_value_exits_with_error_status(self) -> None:
        result = run_editor("--project")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("Missing value after '--project'.", result.stderr)

    def test_help_prints_options_and_succeeds(self) -> None:
        result = run_editor("--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Ludus Editor workspace", result.stdout)
        self.assertIn("--tooling-root <path>", result.stdout)
        self.assertNotIn("terminating", result.stderr)


if __name__ == "__main__":
    unittest.main()
