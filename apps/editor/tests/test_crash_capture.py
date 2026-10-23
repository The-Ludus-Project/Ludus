"""Native macOS crash-to-HTTP acceptance against the real bundled handler.

Only child fixtures are crashed. Reports stay in a private temporary directory;
the receiver listens on loopback and never contacts a third-party service.
"""
from __future__ import annotations

import gzip
import http.server
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

PROBE = Path(sys.argv.pop(1)).resolve()
HANDLER = Path(sys.argv.pop(1)).resolve()


class Receiver(http.server.BaseHTTPRequestHandler):
    events: queue.Queue = queue.Queue()
    status_code = 200

    def do_POST(self):
        # NSURLSession streams Crashpad's multipart payload using chunked HTTP.
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            chunks = []
            while True:
                length = int(self.rfile.readline().strip().split(b";", 1)[0], 16)
                if length == 0:
                    self.rfile.readline()
                    break
                chunks.append(self.rfile.read(length))
                self.rfile.read(2)
            body = b"".join(chunks)
        else:
            body = self.rfile.read(int(self.headers["Content-Length"]))
        if self.headers.get("Content-Encoding") == "gzip":
            body = gzip.decompress(body)
        self.events.put((self.path, body))
        self.send_response(self.status_code)
        self.end_headers()
        self.wfile.write(b"0123456789abcdef0123456789abcdef")

    def log_message(self, *_arguments):
        pass


class CrashCaptureTests(unittest.TestCase):
    def setUp(self):
        Receiver.status_code = 200
        self.temporary = tempfile.TemporaryDirectory(prefix="ludus-crash-capture-")
        self.root = Path(self.temporary.name)
        self.database = self.root / "database"
        self.database.mkdir()
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Receiver)
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_port}/api/42/minidump/?sentry_key=test"
        self.children = []
        while not Receiver.events.empty():
            Receiver.events.get_nowait()

    def tearDown(self):
        sanitizer_errors = []
        for child in self.children:
            if child.poll() is None:
                child.terminate()
            _output, errors = child.communicate(timeout=15)
            if b"ERROR: AddressSanitizer" in errors or b"runtime error:" in errors:
                sanitizer_errors.append(errors.decode(errors="replace"))
        self.server.shutdown()
        self.server.server_close()
        self.server_thread.join(timeout=5)
        self.temporary.cleanup()
        self.assertEqual(sanitizer_errors, [])

    def run_probe(self, *arguments):
        result = subprocess.run([str(PROBE), *map(str, arguments)], capture_output=True, timeout=25)
        self.assertNotIn(b"ERROR: AddressSanitizer", result.stderr)
        self.assertNotIn(b"runtime error:", result.stderr)
        return result

    def launch(self, consent, crash=True, handler=HANDLER, action=None):
        child = subprocess.Popen([str(PROBE), "launch", str(handler), str(self.database),
                                  self.url, "yes" if consent else "no", action or ("crash" if crash else "wait")],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.children.append(child)
        return child

    def wait_for(self, predicate, seconds=15):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.1)
        self.fail("Timed out waiting for crash-report state")

    def counts(self):
        result = self.run_probe("counts", self.database)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return json.loads(result.stdout)

    def test_crash_is_captured_without_consent_then_submitted_after_restart(self):
        child = self.launch(False)
        child.communicate(timeout=15)
        self.assertEqual(child.returncode, -signal.SIGABRT)
        self.wait_for(lambda: self.counts()["pending"] == 1)
        self.assertTrue(Receiver.events.empty(), "Consent-off crash must never contact receiver")
        dump = next(self.database.rglob("*.dmp"))
        self.assertEqual(dump.read_bytes()[:4], b"MDMP")
        self.launch(True, crash=False)
        path, body = Receiver.events.get(timeout=15)
        self.assertIn("/api/42/minidump/", path)
        self.assertIn(b"upload_file_minidump", body)
        self.assertIn(b"MDMP", body)
        self.assertIn(b"ludus-crash-probe@fixture", body)
        self.wait_for(lambda: self.counts()["uploaded"] == 1)
        self.assertEqual(self.counts()["pending"], 0)

    def test_consented_crash_uploads_and_acknowledgement_is_retained(self):
        child = self.launch(True)
        child.communicate(timeout=15)
        self.assertEqual(child.returncode, -signal.SIGABRT)
        Receiver.events.get(timeout=15)
        self.wait_for(lambda: self.counts()["uploaded"] == 1)

    def test_missing_handler_fails_without_crashing_fixture(self):
        child = self.launch(False, handler=self.root / "missing-handler")
        _output, errors = child.communicate(timeout=15)
        self.assertEqual(child.returncode, 2, errors.decode())
        self.assertTrue(Receiver.events.empty())

    def test_transient_service_failure_retains_report_for_restart(self):
        Receiver.status_code = 503
        child = self.launch(True)
        child.communicate(timeout=15)
        self.assertEqual(child.returncode, -signal.SIGABRT)
        Receiver.events.get(timeout=15)
        self.wait_for(lambda: self.counts()["pending"] == 1)
        self.assertEqual(self.counts()["uploaded"], 0)
        Receiver.status_code = 200
        self.launch(True, crash=False)
        Receiver.events.get(timeout=15)
        self.wait_for(lambda: self.counts()["uploaded"] == 1)

    def test_revoking_consent_pauses_a_future_crash(self):
        child = self.launch(True, action="revoke")
        child.communicate(timeout=15)
        self.assertEqual(child.returncode, -signal.SIGABRT)
        self.wait_for(lambda: self.counts()["pending"] == 1)
        self.assertTrue(Receiver.events.empty())

    def test_enabling_consent_wakes_the_running_handler(self):
        child = self.launch(False)
        child.communicate(timeout=15)
        self.wait_for(lambda: self.counts()["pending"] == 1)
        self.launch(False, crash=False, action="enable")
        Receiver.events.get(timeout=15)
        self.wait_for(lambda: self.counts()["uploaded"] == 1)

    def test_dsn_routing_and_rejection(self):
        valid = self.run_probe("url", "https://abc123@ingest.example.test/prefix/42")
        self.assertEqual(valid.stdout.decode(),
                         "https://ingest.example.test/prefix/api/42/minidump/?sentry_key=abc123&sentry_version=7")
        for dsn in ("http://abc@localhost/42", "https://abc:secret@host/42", "https://host/42",
                    "https://abc@host/42?redirect=evil", "https://abc@host/not-a-project", "file:///42"):
            with self.subTest(dsn=dsn):
                self.assertEqual(self.run_probe("url", dsn).stdout, b"")

    def test_saved_consent_cannot_authorize_a_different_destination(self):
        result = self.run_probe("consent")
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))

    def test_production_startup_needs_no_launcher_environment(self):
        # Exercise the same before-QApplication initializer as main.cpp. HOME is
        # private so neither reports nor preferences touch the developer's account.
        environment = {key: value for key, value in os.environ.items()
                       if not key.startswith(("SENTRY_", "LUDUS_DIAGNOSTIC_"))}
        environment.update(HOME=str(self.root), CFFIXED_USER_HOME=str(self.root))
        # The missing plugin makes QApplication itself abort before its constructor
        # returns, verifying that capture is already registered at that boundary.
        for count, platform in enumerate(("offscreen", "ludus-missing-platform"), 1):
            with self.subTest(platform=platform):
                environment["QT_QPA_PLATFORM"] = platform
                child = subprocess.run([str(PROBE), "production"], env=environment, capture_output=True, timeout=20)
                self.assertNotIn(b"ERROR: AddressSanitizer", child.stderr)
                self.assertNotIn(b"runtime error:", child.stderr)
                self.assertEqual(child.returncode, -signal.SIGABRT, child.stderr.decode())
                self.wait_for(lambda: len(list(self.root.rglob("*.dmp"))) >= count)
        self.assertTrue(Receiver.events.empty())


if __name__ == "__main__":
    unittest.main()
