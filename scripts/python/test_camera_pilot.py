"""Exercise the report a reviewer sees, including successful processes with missing evidence."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import camera_pilot as pilot


class CameraPilotTests(unittest.TestCase):
    def report(self, omit=None, skip=None, fail=None):
        def run(command, **kwargs):
            name = command[1]
            root = ET.Element("Catch2TestRun")
            if name != omit:
                case = ET.SubElement(root, "TestCase", name=name)
                if name == skip:
                    ET.SubElement(case, "Skip").text = pilot.ADAPTER_SKIP
                ET.SubElement(case, "OverallResult", success="false" if name == fail else "true",
                              skips="1" if name == skip else "0")
            ET.ElementTree(root).write(Path(command[-1].split("xml::out=")[1]))
            return subprocess.CompletedProcess(command, 4 if name == skip else int(name == fail))

        with (patch.object(sys, "argv", ["camera_pilot.py"]), patch.dict(os.environ, GITHUB_STEP_SUMMARY=""),
              patch.object(subprocess, "run", side_effect=run) as runner, contextlib.redirect_stdout(io.StringIO()) as log):
            code = pilot.main()
        self.assertEqual(runner.call_count, 3)
        return code, log.getvalue()

    def test_green_process_omitted_required_case(self):
        code, report = self.report(omit=pilot.REVERSE)
        self.assertNotEqual(code, 0)
        for case in ("Finite reverse-Z", "Infinite far plane"):
            row = next(row for row in report.splitlines() if row.startswith(f"| {case} |"))
            self.assertIn("Incomplete", row)
            self.assertIn(str(pilot.RENDERER), row)
            self.assertIn(pilot.REVERSE, row)

    def test_declared_adapter_skip_is_visible(self):
        code, report = self.report(skip=pilot.CAPTURE)
        self.assertEqual(code, 0)
        self.assertIn(f"Inapplicable ({pilot.ADAPTER_SKIP})", report)

    def test_unfiltered_failure_missed_even_when_selected_capture_also_fails(self):
        root = ET.Element("Catch2TestRun")
        for name in (pilot.REVERSE, pilot.CAPTURE, "Additional camera regression"):
            case = ET.SubElement(root, "TestCase", name=name)
            ET.SubElement(case, "OverallResult", success="false")
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "unfiltered.xml"
            ET.ElementTree(root).write(report)
            rows, agrees = pilot.compare_unfiltered(
                {pilot.REVERSE: "Passed", pilot.CAPTURE: "Failed"}, [report], ["Additional camera regression"])
        self.assertFalse(agrees)
        self.assertIn(f"| {pilot.REVERSE} | Passed | Failed | Selection miss |", rows)
        self.assertIn("| Additional camera regression | Outside selection | Failed | Selection miss |", rows)
        self.assertIn(f"| {pilot.CAPTURE} | Failed | Failed | Agrees |", rows)

    def test_missing_case_and_failure_both_remain_visible(self):
        code, report = self.report(omit=pilot.CONVENTIONAL, fail=pilot.CAPTURE)
        self.assertNotEqual(code, 0)
        self.assertIn("| Incomplete", report)
        self.assertIn("| Failed |", report)


if __name__ == "__main__":
    unittest.main()
