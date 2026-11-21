"""Download fallback contracts for the pinned shader tools in scripts/shader-probe."""
import hashlib
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import urllib.error

ROOT = Path(__file__).resolve().parents[2]
LOADER = importlib.machinery.SourceFileLoader("ludus_shader_probe", str(ROOT / "scripts/shader-probe"))
SPEC = importlib.util.spec_from_loader("ludus_shader_probe", LOADER)
PROBE = importlib.util.module_from_spec(SPEC)
LOADER.exec_module(PROBE)

PAYLOAD = b"pinned package bytes"
PACKAGE = "spirv-tools_2025.1~rc1-1~ubuntu0.24.04.2_amd64.deb"
SECURITY_URL = "https://security.ubuntu.com/ubuntu/pool/universe/s/spirv-tools/" + PACKAGE


class Response:
    """Minimal urlopen context manager that serves fixed bytes."""

    def __init__(self, payload: bytes):
        self.payload = payload
        self.offset = 0

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        return False

    def read(self, size: int = -1) -> bytes:
        end = len(self.payload) if size < 0 else min(len(self.payload), self.offset + size)
        chunk = self.payload[self.offset:end]
        self.offset = end
        return chunk


def forbidden(url: str) -> urllib.error.HTTPError:
    return urllib.error.HTTPError(url, 403, "Forbidden", {}, None)


class ShaderProbeFetchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.archive = Path(self.directory.name) / "spirv-tools.deb"
        self.pin = {"url": SECURITY_URL, "sha256": hashlib.sha256(PAYLOAD).hexdigest()}
        self.requested = []
        self.sleeps = []

    def tearDown(self):
        self.directory.cleanup()

    def fetch(self, serve):
        def urlopen(request, timeout):
            self.requested.append(request.full_url)
            return serve(request.full_url)
        with mock.patch.object(PROBE.urllib.request, "urlopen", urlopen):
            PROBE.fetch("spirv_tools", self.pin, self.archive, sleep=self.sleeps.append)

    def test_ubuntu_pool_package_lists_official_mirrors_after_the_pin(self):
        sources = PROBE.download_sources(self.pin)
        self.assertEqual(sources[0], SECURITY_URL)
        self.assertIn("http://azure.archive.ubuntu.com/ubuntu/pool/universe/s/spirv-tools/" + PACKAGE, sources)
        self.assertEqual(sources[-1], "https://launchpad.net/ubuntu/+archive/primary/+files/" + PACKAGE)
        self.assertEqual(len(sources), len(set(sources)))

    def test_release_assets_keep_api_then_release_url_without_mirrors(self):
        pin = {"release_asset_api": "https://api.github.com/asset/1", "url": "https://github.com/release.tar.gz"}
        self.assertEqual(PROBE.download_sources(pin), [pin["release_asset_api"], pin["url"]])

    def test_forbidden_security_mirror_falls_back_to_a_verified_mirror(self):
        def serve(url):
            if url.startswith("https://security.ubuntu.com/"):
                raise forbidden(url)
            return Response(PAYLOAD)
        self.fetch(serve)
        self.assertEqual(self.archive.read_bytes(), PAYLOAD)
        self.assertEqual(self.requested[:2], [SECURITY_URL, SECURITY_URL])
        self.assertTrue(self.requested[2].startswith("http://azure.archive.ubuntu.com/"))
        self.assertEqual(self.sleeps, [PROBE.RETRY_DELAY_SECONDS])

    def test_transient_failure_retries_the_same_source(self):
        failures = [forbidden(SECURITY_URL)]

        def serve(url):
            if failures:
                raise failures.pop()
            return Response(PAYLOAD)
        self.fetch(serve)
        self.assertEqual(self.requested, [SECURITY_URL, SECURITY_URL])
        self.assertEqual(self.archive.read_bytes(), PAYLOAD)

    def test_mismatched_bytes_move_to_the_next_source_and_are_never_kept(self):
        def serve(url):
            return Response(b"tampered" if url == SECURITY_URL else PAYLOAD)
        self.fetch(serve)
        self.assertEqual(self.requested.count(SECURITY_URL), 1)
        self.assertEqual(self.archive.read_bytes(), PAYLOAD)

    def test_every_source_failing_reports_each_and_leaves_no_archive(self):
        def serve(url):
            raise forbidden(url)
        with self.assertRaises(RuntimeError) as failure:
            self.fetch(serve)
        sources = PROBE.download_sources(self.pin)
        self.assertEqual(len(self.requested), len(sources) * PROBE.DOWNLOAD_ATTEMPTS)
        for source in sources:
            self.assertIn(source, str(failure.exception))
        self.assertFalse(self.archive.exists())
        self.assertFalse(self.archive.with_suffix(".download").exists())

    def test_verified_existing_archive_skips_the_network(self):
        self.archive.write_bytes(PAYLOAD)
        self.fetch(lambda url: self.fail(f"unexpected download of {url}"))
        self.assertEqual(self.requested, [])


if __name__ == "__main__":
    unittest.main()
