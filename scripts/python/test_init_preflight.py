"""Preflight checks that run before ./init.sh installs anything; every probe is mocked."""
from __future__ import annotations

import argparse
import contextlib
import io
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import engine
import init_cache
import init_preflight
import init_progress
import init_terminal

GIGABYTE = init_preflight.BYTES_PER_GIGABYTE
UBUNTU_24 = 'NAME="Ubuntu"\nVERSION_ID="24.04"\nID=ubuntu\nPRETTY_NAME="Ubuntu 24.04.1 LTS"\n'
UBUNTU_22 = 'NAME="Ubuntu"\nVERSION_ID="22.04"\nID=ubuntu\nPRETTY_NAME="Ubuntu 22.04.4 LTS"\n'
UBUNTU_26 = 'ID=ubuntu\nVERSION_ID="26.04"\nPRETTY_NAME="Ubuntu 26.04 LTS"\n'
FEDORA = 'NAME="Fedora Linux"\nID=fedora\nVERSION_ID=41\nPRETTY_NAME="Fedora Linux 41"\n'


def usage(free_gigabytes: float):
    return lambda _path: SimpleNamespace(free=int(free_gigabytes * GIGABYTE))


class DiskSpaceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def test_thresholds(self):
        cases = ((init_preflight.MINIMUM_FREE_GIGABYTES - 1, init_preflight.FAILED),
                 (init_preflight.MINIMUM_FREE_GIGABYTES + 1, init_preflight.WARNING),
                 (init_preflight.RECOMMENDED_FREE_GIGABYTES + 1, init_preflight.PASSED))
        for free, status in cases:
            with self.subTest(free=free):
                result = init_preflight.check_disk_space(self.root, disk_usage=usage(free))
                self.assertEqual(result.status, status)
        failed = init_preflight.check_disk_space(self.root, disk_usage=usage(1))
        self.assertIn(f"at least {init_preflight.MINIMUM_FREE_GIGABYTES} GB", failed.message)

    def test_measures_the_out_volume_even_before_out_exists(self):
        seen = []

        def disk_usage(path):
            seen.append(path)
            return SimpleNamespace(free=100 * GIGABYTE)
        init_preflight.check_disk_space(self.root, disk_usage=disk_usage)
        self.assertEqual(seen, [str(self.root)])
        (self.root / "out").mkdir()
        init_preflight.check_disk_space(self.root, disk_usage=disk_usage)
        self.assertEqual(seen[-1], str(self.root / "out"))

    def test_unmeasurable_disk_is_a_warning(self):
        result = init_preflight.check_disk_space(self.root, disk_usage=MagicMock(side_effect=OSError("denied")))
        self.assertEqual(result.status, init_preflight.WARNING)


class NetworkTests(unittest.TestCase):
    def test_all_remotes_reached(self):
        connect = MagicMock(return_value=True)
        result = init_preflight.check_network(has_cached_setup=False, environment={}, connect=connect)
        self.assertEqual(result.status, init_preflight.PASSED)
        hosts = sorted(call.args[0] for call in connect.call_args_list)
        self.assertEqual(hosts, sorted(host for _label, host in init_preflight.REQUIRED_REMOTES))
        self.assertTrue(all(call.args[1] == init_preflight.HTTPS_PORT for call in connect.call_args_list))

    def test_offline_fails_on_a_fresh_checkout_and_warns_with_a_cache(self):
        def connect(host, _port, _timeout):
            return host != "center2.conan.io"
        fresh = init_preflight.check_network(has_cached_setup=False, environment={}, connect=connect)
        self.assertEqual(fresh.status, init_preflight.FAILED)
        self.assertIn("ConanCenter (center2.conan.io)", fresh.message)
        self.assertNotIn("github.com", fresh.message)
        cached = init_preflight.check_network(has_cached_setup=True, environment={}, connect=connect)
        self.assertEqual(cached.status, init_preflight.WARNING)

    def test_proxy_is_checked_instead_of_the_remotes(self):
        connect = MagicMock(return_value=True)
        result = init_preflight.check_network(has_cached_setup=False,
                                              environment={"HTTPS_PROXY": "http://proxy.example:3128"}, connect=connect)
        connect.assert_called_once_with("proxy.example", 3128, init_preflight.NETWORK_TIMEOUT_SECONDS)
        self.assertEqual(result.status, init_preflight.PASSED)
        self.assertEqual(init_preflight.proxy_address({"https_proxy": "proxy.example"}),
                         ("proxy.example", init_preflight.DEFAULT_PROXY_PORT))
        self.assertIsNone(init_preflight.proxy_address({}))

    def test_a_hanging_lookup_is_bounded_by_the_timeout(self):
        release = threading.Event()

        def connect(host, _port, _timeout):
            if host == "github.com":
                release.wait(timeout=10)
            return True
        timeout = 0.2
        started = time.monotonic()
        try:
            missing = init_preflight.unreachable_hosts([("pypi.org", 443), ("github.com", 443)], connect=connect,
                                                       timeout=timeout)
        finally:
            release.set()
        self.assertEqual(missing, ["github.com"])
        self.assertLess(time.monotonic() - started, timeout * 5)

    def test_cached_setup_needs_the_venv_and_conan_stamp_and_no_force(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertFalse(init_preflight.has_cached_setup(root, {}))
            (root / "out/host-tools/venv").mkdir(parents=True)
            self.assertFalse(init_preflight.has_cached_setup(root, {}))
            init_cache.record(init_cache.stamp_path(root, "conan"), {"inputs": 1})
            self.assertTrue(init_preflight.has_cached_setup(root, {}))
            self.assertFalse(init_preflight.has_cached_setup(root, {init_cache.FORCE_ENVIRONMENT: "1"}))


class OperatingSystemTests(unittest.TestCase):
    def test_ubuntu_versions(self):
        for text, status in ((UBUNTU_24, init_preflight.PASSED), (UBUNTU_26, init_preflight.PASSED),
                             (UBUNTU_22, init_preflight.FAILED), (FEDORA, init_preflight.WARNING), ("", init_preflight.WARNING)):
            with self.subTest(text=text):
                result = init_preflight.check_operating_system(system="Linux", os_release_text=text)
                self.assertEqual(result.status, status)
        self.assertIn("Ubuntu 24.04 or newer", init_preflight.check_operating_system(
            system="Linux", os_release_text=UBUNTU_22).message)

    def test_macos_versions(self):
        for version, status in (("26.6", init_preflight.PASSED), ("14.0", init_preflight.PASSED),
                                ("13.6.1", init_preflight.FAILED), ("", init_preflight.WARNING)):
            with self.subTest(version=version):
                self.assertEqual(init_preflight.check_operating_system(system="Darwin", macos_version=version).status,
                                 status)

    def test_other_systems_warn(self):
        self.assertEqual(init_preflight.check_operating_system(system="Windows").status, init_preflight.WARNING)

    def test_parse_version(self):
        self.assertEqual(init_preflight.parse_version("24.04"), (24, 4))
        self.assertEqual(init_preflight.parse_version("26.6.1"), (26, 6, 1))
        self.assertEqual(init_preflight.parse_version("rolling"), ())


class XcodeTests(unittest.TestCase):
    def test_installed(self):
        with tempfile.TemporaryDirectory() as directory:
            run = MagicMock(return_value=subprocess.CompletedProcess([], 0, stdout=directory + "\n", stderr=""))
            result = init_preflight.check_xcode_command_line_tools(run=run)
        self.assertEqual(result.status, init_preflight.PASSED)
        self.assertEqual(run.call_args.args[0], ["xcode-select", "-p"])
        self.assertEqual(run.call_args.kwargs["timeout"], init_preflight.XCODE_SELECT_TIMEOUT_SECONDS)

    def test_missing_or_broken(self):
        for run in (MagicMock(return_value=subprocess.CompletedProcess([], 2, stdout="", stderr="error")),
                    MagicMock(return_value=subprocess.CompletedProcess([], 0, stdout="/nonexistent/Developer\n")),
                    MagicMock(side_effect=FileNotFoundError("xcode-select")),
                    MagicMock(side_effect=subprocess.TimeoutExpired("xcode-select", 1))):
            with self.subTest(run=run):
                result = init_preflight.check_xcode_command_line_tools(run=run)
                self.assertEqual(result.status, init_preflight.FAILED)
                self.assertIn("xcode-select --install", result.message)


class PythonTests(unittest.TestCase):
    def test_version(self):
        def present(_name):
            return object()
        self.assertEqual(init_preflight.check_python("3.10.0", version=(3, 12, 15), find_spec=present, system="Linux",
                                                     will_install_system_packages=False).status, init_preflight.PASSED)
        old = init_preflight.check_python("3.10.0", version=(3, 9, 6), find_spec=present, system="Darwin",
                                          will_install_system_packages=True)
        self.assertEqual(old.status, init_preflight.FAILED)
        self.assertIn("older than the required 3.10.0", old.message)

    def test_missing_venv_support(self):
        def missing_ensurepip(name):
            return None if name == "ensurepip" else object()
        installing = init_preflight.check_python("3.10.0", version=(3, 12, 3), find_spec=missing_ensurepip,
                                                 system="Linux", will_install_system_packages=True)
        self.assertEqual(installing.status, init_preflight.PASSED)
        self.assertIn("will be installed", installing.message)
        manual = init_preflight.check_python("3.10.0", version=(3, 12, 3), find_spec=missing_ensurepip,
                                             system="Linux", will_install_system_packages=False)
        self.assertEqual(manual.status, init_preflight.FAILED)
        self.assertIn("python3-venv", manual.message)


class RunChecksTests(unittest.TestCase):
    def run_checks(self, system, *, is_ci=False, free=100, is_reachable=True):
        passed = init_preflight.CheckResult("x", init_preflight.PASSED, "ok")
        with tempfile.TemporaryDirectory() as directory, \
                patch.object(init_preflight, "check_operating_system", return_value=passed), \
                patch.object(init_preflight, "check_xcode_command_line_tools", return_value=passed) as xcode, \
                patch.object(init_preflight, "check_python", return_value=passed), \
                patch.object(init_preflight.shutil, "disk_usage", usage(free)), \
                patch.object(init_preflight, "can_connect", return_value=is_reachable):
            results = init_preflight.run_checks(Path(directory), minimum_python="3.10.0",
                                                will_install_system_packages=True, is_ci=is_ci, system=system,
                                                environment={})
        return results, xcode

    def test_xcode_only_on_macos(self):
        _results, xcode = self.run_checks("Linux")
        xcode.assert_not_called()
        results, xcode = self.run_checks("Darwin")
        xcode.assert_called_once_with()
        self.assertEqual(len(results), 5)

    def test_ci_turns_disk_and_network_failures_into_warnings(self):
        results, _xcode = self.run_checks("Linux", free=1, is_reachable=False)
        self.assertEqual([result.status for result in results[-2:]], [init_preflight.FAILED] * 2)
        results, _xcode = self.run_checks("Linux", is_ci=True, free=1, is_reachable=False)
        self.assertEqual([result.status for result in results[-2:]], [init_preflight.WARNING] * 2)

    def test_whole_preflight_is_fast(self):
        started = time.monotonic()
        self.run_checks("Linux")
        self.assertLess(time.monotonic() - started, init_preflight.NETWORK_TIMEOUT_SECONDS)


class EngineIntegrationTests(unittest.TestCase):
    VERSIONS = {"minimum": {"python": "3.10.0"}}

    def run_preflight(self, results, environment=None):
        args = argparse.Namespace(ci=False, no_system_install=False)
        output = io.StringIO()
        with patch.object(init_preflight, "run_checks", return_value=results) as run_checks, \
                patch.dict("os.environ", environment or {}), contextlib.redirect_stdout(output):
            try:
                engine.run_init_preflight(args, Path("/repo"), self.VERSIONS)
                error = None
            except engine.EngineError as raised:
                error = raised
        return output.getvalue(), error, run_checks

    def test_failure_stops_setup_with_every_failed_check(self):
        results = [init_preflight.CheckResult("Network", init_preflight.WARNING, "slow"),
                   init_preflight.CheckResult("Free disk space", init_preflight.FAILED, "only 2.0 GB free"),
                   init_preflight.CheckResult("Python", init_preflight.FAILED, "too old")]
        output, error, _run_checks = self.run_preflight(results)
        self.assertIsNotNone(error)
        self.assertIn("2 preflight checks failed", str(error))
        self.assertIn("Free disk space: only 2.0 GB free", str(error))
        self.assertIn("Python: too old", str(error))
        self.assertIn(init_preflight.SKIP_ENVIRONMENT, str(error))
        self.assertIn("[warning]", output)
        self.assertIn("[failed]", output)

    def test_warnings_continue(self):
        results = [init_preflight.CheckResult("Network", init_preflight.WARNING, "offline")]
        _output, error, _run_checks = self.run_preflight(results)
        self.assertIsNone(error)

    def test_skip_environment(self):
        output, error, run_checks = self.run_preflight([], {init_preflight.SKIP_ENVIRONMENT: "1"})
        self.assertIsNone(error)
        run_checks.assert_not_called()
        self.assertIn("Skipping preflight checks", output)

    def test_preflight_is_the_first_planned_step(self):
        args = init_preflight_args()
        with patch.dict("os.environ", {init_preflight.SKIP_ENVIRONMENT: ""}):
            self.assertEqual(init_progress.native_plan(args, system="Linux")[0],
                             init_progress.Step("preflight", init_progress.PREFLIGHT_LABEL))
        skipped = init_progress.native_plan(args, system="Linux", environment={init_preflight.SKIP_ENVIRONMENT: "1"})
        self.assertNotIn("preflight", [step.key for step in skipped])

    def test_terminal_check_line(self):
        painter = init_terminal.Painter(False)
        self.assertEqual(init_terminal.check_line("Python", init_preflight.PASSED, "Python 3.12", painter, width=8),
                         "  [ok]      Python    Python 3.12")
        self.assertIn("\x1b[", init_terminal.check_line("Python", init_preflight.FAILED, "old",
                                                         init_terminal.Painter(True)))


def init_preflight_args():
    import init_ui
    return init_ui.apply_defaults(engine.make_parser().parse_args(["init", "--cli", "linux-clang-development"]))


if __name__ == "__main__":
    unittest.main()
