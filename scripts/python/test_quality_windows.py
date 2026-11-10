"""Native Windows controls for admission and descendant cleanup."""

import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

from quality_process import run_bounded


@unittest.skipUnless(os.name == "nt", "Windows Job controls")
class WindowsQualityTests(unittest.TestCase):
    def test_parent_exit_cannot_leave_a_descendant_running(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            heartbeat = root / "heartbeat"
            child = ("import pathlib,time\n"
                     f"path=pathlib.Path({str(heartbeat)!r})\n"
                     "print('descendant admitted', flush=True)\n"
                     "while True:\n path.write_text(str(time.monotonic()))\n time.sleep(0.02)")
            parent = f"import subprocess,sys; subprocess.Popen([sys.executable, '-c', {child!r}])"
            result = run_bounded([sys.executable, "-c", parent], cwd=root,
                                 log=root / "descendant.log", timeout=3)
            self.assertTrue(result.timed_out)
            self.assertIn("descendant admitted", result.log.read_text())
            previous = heartbeat.read_text()
            time.sleep(0.1)
            self.assertEqual(heartbeat.read_text(), previous)

    def test_assignment_failure_prevents_child_execution(self):
        from unittest.mock import patch
        from quality_process_windows import ProcessJob
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            marker = root / "started"
            command = [sys.executable, "-c", f"from pathlib import Path; Path({str(marker)!r}).touch()"]
            with patch.object(ProcessJob, "assign", side_effect=OSError("assignment rejected")):
                with self.assertRaisesRegex(OSError, "assignment rejected"):
                    run_bounded(command, cwd=root, log=root / "rejected.log")
            self.assertFalse(marker.exists())


if __name__ == "__main__":
    unittest.main()
