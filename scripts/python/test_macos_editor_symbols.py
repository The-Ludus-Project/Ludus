"""Regression tests for the macOS Editor symbol publication gate."""

import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import call, patch

from macos_editor_symbols import upload_symbols


class MacOSEditorSymbolsTest(unittest.TestCase):
    cli = Path("/test/sentry-cli")
    executable = Path("/test/Ludus Editor.app/Contents/MacOS/ludus_editor")
    dwarf = Path("/test/ludus_editor.app.dSYM/Contents/Resources/DWARF/ludus_editor")
    arm_uuid = "7d4c17dc-b17b-3c37-87d3-9d99940a404e"
    intel_uuid = "35decfe3-99bf-3a77-b46f-c018dbae2bd2"

    def setUp(self):
        environment = patch.dict(os.environ, SENTRY_AUTH_TOKEN="test-only")
        environment.start()
        self.addCleanup(environment.stop)
        runner = patch("macos_editor_symbols.subprocess.run")
        self.run_command = runner.start()
        self.addCleanup(runner.stop)

    @staticmethod
    def output(stdout):
        return subprocess.CompletedProcess([], 0, stdout, "")

    def uuids(self, path, *slices):
        return self.output("".join(f"UUID: {identifier.upper()} ({arch}) {path}\n" for arch, identifier in slices))

    def test_uploads_matching_debug_files_for_each_slice(self):
        slices = (("arm64", self.arm_uuid), ("x86_64", self.intel_uuid))
        line_table = self.output("SECTION           SIZE (b)\n__debug_line  9000 (8.17%)\n")
        self.run_command.side_effect = [
            self.uuids(self.executable, *slices), self.uuids(self.dwarf, *reversed(slices)),
            line_table, line_table, self.output(""), self.output(""),
        ]

        upload_symbols(self.cli, self.executable, self.dwarf)

        command = [
            str(self.cli), "debug-files", "upload", "--org", "the-ludus-project", "--project", "ludus-editor",
            "--wait", "--require-all", "--no-sources", "--id", self.arm_uuid, "--id", self.intel_uuid,
        ]
        self.assertEqual(self.run_command.call_args_list[-2:], [
            call([*command, str(self.dwarf)], check=True),
            call([*command, "--no-debug", str(self.executable)], check=True),
        ])

    def test_rejects_a_mismatched_slice_without_uploading(self):
        self.run_command.side_effect = [
            self.uuids(self.executable, ("arm64", self.arm_uuid), ("x86_64", self.intel_uuid)),
            self.uuids(self.dwarf, ("arm64", self.arm_uuid)),
        ]
        with self.assertRaisesRegex(ValueError, "differ"):
            upload_symbols(self.cli, self.executable, self.dwarf)
        self.assertEqual(self.run_command.call_count, 2)

    def test_rejects_dwarf_without_line_info(self):
        slices = (("arm64", self.arm_uuid), ("x86_64", self.intel_uuid))
        self.run_command.side_effect = [
            self.uuids(self.executable, *slices), self.uuids(self.dwarf, *slices),
            self.output("__debug_line 9000 (8.17%)\n"), self.output("__debug_line 0 (0.00%)\n"),
        ]
        with self.assertRaisesRegex(ValueError, "no DWARF line table for x86_64"):
            upload_symbols(self.cli, self.executable, self.dwarf)
        self.assertEqual(self.run_command.call_count, 4)

    def test_server_failure_blocks_the_remaining_upload(self):
        slices = (("arm64", self.arm_uuid),)
        self.run_command.side_effect = [
            self.uuids(self.executable, *slices), self.uuids(self.dwarf, *slices),
            self.output("__debug_line 9000 (8.17%)\n"), subprocess.CalledProcessError(1, str(self.cli)),
        ]
        with self.assertRaises(subprocess.CalledProcessError):
            upload_symbols(self.cli, self.executable, self.dwarf)
        self.assertEqual(self.run_command.call_count, 4)

    def test_requires_ci_token(self):
        with patch.dict(os.environ, SENTRY_AUTH_TOKEN=""):
            with self.assertRaisesRegex(ValueError, "SENTRY_AUTH_TOKEN"):
                upload_symbols(self.cli, self.executable, self.dwarf)
        self.run_command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
