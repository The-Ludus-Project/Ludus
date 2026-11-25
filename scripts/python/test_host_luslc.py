"""Tests for the host luslc bootstrap that cross builds lower LUSL shaders with."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import host_luslc


class HostLuslcTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for relative in ("venv/bin/cmake", "venv/bin/ninja", "bin/clang++", "bin/clang"):
            path = self.root / "out/host-tools" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("")

    def arguments(self, system):
        with mock.patch.object(host_luslc.platform, "system", return_value=system):
            return host_luslc.configure_arguments(self.root)

    def test_configures_only_luslc_with_the_pinned_host_tools(self):
        for system in ("Linux", "Darwin"):
            with self.subTest(system=system):
                arguments = self.arguments(system)
                self.assertEqual(arguments[0], str(self.root / "out/host-tools/venv/bin/cmake"))
                for expected in ("-DLUDUS_LUSLC_ONLY=ON", "-DLUDUS_BUILD_TESTS=OFF", "-DLUDUS_USE_INIT_OPTIONS=OFF",
                                 "-DLUDUS_BUILD_FLAVOR=Release",
                                 f"-DCMAKE_CXX_COMPILER={self.root / 'out/host-tools/bin/clang++'}"):
                    self.assertIn(expected, arguments)
                self.assertIn(str(self.root / host_luslc.BUILD), arguments)
                has_sysroot = any(argument.startswith("-DCMAKE_OSX_SYSROOT=") for argument in arguments)
                self.assertEqual(has_sysroot, system == "Darwin")

    def test_unsupported_host_and_missing_tools_name_the_repair(self):
        with self.assertRaisesRegex(host_luslc.HostLuslcError, "LUDUS_LUSLC"):
            self.arguments("Windows")
        (self.root / "out/host-tools/bin/clang++").unlink()
        with self.assertRaisesRegex(host_luslc.HostLuslcError, r"init\.sh"):
            self.arguments("Linux")

    def test_configures_once_then_builds_incrementally(self):
        calls = []

        def run(command, **_):
            calls.append(command)
            if "--build" in command:
                binary = host_luslc.binary_path(self.root)
                binary.parent.mkdir(parents=True, exist_ok=True)
                binary.write_text("")
            else:
                (self.root / host_luslc.BUILD).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(command, 0)

        with mock.patch.object(host_luslc.platform, "system", return_value="Linux"), \
                mock.patch.object(host_luslc.subprocess, "run", side_effect=run):
            first = host_luslc.ensure(self.root)
            second = host_luslc.ensure(self.root)
        self.assertEqual(first, second)
        self.assertEqual(first, host_luslc.binary_path(self.root.resolve()))
        configures = [command for command in calls if "--build" not in command]
        builds = [command for command in calls if "--build" in command]
        self.assertEqual(len(configures), 1)
        self.assertEqual(len(builds), 2)
        self.assertEqual(builds[0][-2:], ["--target", "luslc"])
        stamp = json.loads((self.root / host_luslc.BUILD / "ludus-host-luslc.json").read_text())
        self.assertEqual(stamp, configures[0])

    def test_failed_configure_leaves_no_stamp_and_reports_the_tree(self):
        def run(command, **_):
            (self.root / host_luslc.BUILD).mkdir(parents=True, exist_ok=True)
            return subprocess.CompletedProcess(command, 1)

        with mock.patch.object(host_luslc.platform, "system", return_value="Linux"), \
                mock.patch.object(host_luslc.subprocess, "run", side_effect=run):
            with self.assertRaisesRegex(host_luslc.HostLuslcError, "bootstrap-luslc"):
                host_luslc.ensure(self.root)
        self.assertFalse((self.root / host_luslc.BUILD / "ludus-host-luslc.json").exists())

    def test_host_build_drops_cross_compiler_selections(self):
        with mock.patch.dict(host_luslc.os.environ, {"CC": "emcc", "CXX": "em++", "KEEP": "1"}):
            environment = host_luslc.host_environment()
        self.assertNotIn("CC", environment)
        self.assertNotIn("CXX", environment)
        self.assertEqual(environment["KEEP"], "1")


if __name__ == "__main__":
    unittest.main()
