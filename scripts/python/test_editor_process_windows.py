"""Windows acceptance for the actual adapter's stdio and owned process job."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


@unittest.skipUnless(os.name == "nt", "requires the Win32 process and pipe APIs")
class WindowsEditorProcessTests(unittest.TestCase):
    def test_stop_reaps_a_game_and_its_descendant_and_releases_inherited_guard(self):
        import _winapi
        from editor_process_windows import OwnedWindowsProcess, assert_unleased, open_guard

        with tempfile.TemporaryDirectory(prefix="Ludus process test ", ignore_cleanup_errors=True) as directory:
            guard_path = Path(directory) / ".lease"
            program = (
                "import subprocess,sys,time; "
                "child=subprocess.Popen([sys.executable,'-c','import time;time.sleep(60)']); "
                "print(child.pid,flush=True);time.sleep(60)"
            )
            with open_guard(guard_path) as lease:
                game = OwnedWindowsProcess([sys.executable, "-c", program], directory, os.environ,
                                           pass_fds=(lease.fileno(),))
            descendant = None
            try:
                data = bytearray()
                deadline = time.monotonic() + 10
                while b"\n" not in data and time.monotonic() < deadline:
                    try:
                        data.extend(game.read_stdout(256))
                    except BlockingIOError:
                        time.sleep(.01)
                self.assertIn(b"\n", data)
                descendant = _winapi.OpenProcess(0x00100000, False, int(data.splitlines()[0]))
                with self.assertRaises(BlockingIOError):
                    assert_unleased(guard_path)
                report = game.cleanup(cancelled=True)
                self.assertTrue(report.confirmed, report)
                self.assertEqual(0, _winapi.WaitForSingleObject(descendant, 1000))
                assert_unleased(guard_path)
            finally:
                game.cleanup(cancelled=True)
                if descendant is not None:
                    _winapi.CloseHandle(descendant)

    def test_killing_the_supervisor_closes_its_job(self):
        import _winapi

        code = (
            "import os,sys,time;from editor_process_windows import OwnedWindowsProcess;"
            "child=OwnedWindowsProcess([sys.executable,'-c','import time;time.sleep(60)'],os.getcwd(),os.environ);"
            "print(child.pid,flush=True);time.sleep(60)"
        )
        supervisor = subprocess.Popen([sys.executable, "-c", code], cwd=Path(__file__).parent,
                                      stdout=subprocess.PIPE, text=True)
        descendant = None
        try:
            line = supervisor.stdout.readline()
            self.assertTrue(line)
            descendant = _winapi.OpenProcess(0x00100000, False, int(line))
            supervisor.kill()
            supervisor.wait(timeout=10)
            self.assertEqual(0, _winapi.WaitForSingleObject(descendant, 10000))
        finally:
            if supervisor.poll() is None:
                supervisor.kill()
            supervisor.wait(timeout=10)
            supervisor.stdout.close()
            if descendant is not None:
                _winapi.CloseHandle(descendant)

    def test_adapter_reports_a_bounded_stdio_protocol_failure(self):
        root = Path(__file__).resolve().parents[2]
        process = subprocess.run(
            [sys.executable, str(root / "scripts/python/editor_tool.py"), "--stdio", "--tooling-root", str(root)],
            input=b"{}\n", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
        )
        self.assertEqual(2, process.returncode, process.stderr)
        self.assertIn(b'"ready"', process.stdout)
        self.assertIn(b'"ProtocolError"', process.stdout)


if __name__ == "__main__":
    unittest.main()
