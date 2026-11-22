"""Tests for the mobile M0 probes: record sanitization, missing-tool paths and parsers."""
from __future__ import annotations

import io
import json
import tempfile
import unittest
from unittest.mock import patch
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import mobile_device_runner
import mobile_probe
from mobile_probe import Result

SERIAL = "R5CTA1B2C3D"
TEAM_ID = "AB12CD34EF"
IDENTITY = "Apple Development: Jane Doe (AB12CD34EF)"
APPLE_DEVICE_ID = "00008140-001A2B3C4D5E6F78"
CLANG_VERSION = "Homebrew clang version 23.1.3 (https://example.invalid)"


class FakeHost(mobile_probe.Host):
    """A host whose commands are answered by a function and whose tools come from a table."""

    def __init__(self, home: Path, answers=None, tools=None, environment=None):
        super().__init__(environment={"PATH": ""} if environment is None else environment, home=home, root=home)
        self.answers = answers or (lambda command: Result(127, ""))
        self.tools = tools or {}
        self.commands: list[list[str]] = []

    def run(self, command, timeout=mobile_probe.QUERY_TIMEOUT_SECONDS, input_text=None):
        self.commands.append([str(part) for part in command])
        return self.answers([str(part) for part in command])

    def which(self, name):
        return self.tools.get(name)


def options(**overrides):
    parsed = mobile_probe.build_parser().parse_args(["all"])
    for key, value in overrides.items():
        setattr(parsed, key, value)
    return parsed


class SanitizationTests(unittest.TestCase):
    def test_rewrites_paths_identities_and_identifiers(self):
        text = (
            f"-isysroot /Applications/Xcode.app/Contents/SDK -I/Users/jane/Ludus/include "
            f"{IDENTITY} OU={TEAM_ID} {APPLE_DEVICE_ID} 3f2a1b4c-1111-2222-3333-444455556666 jane@example.com"
        )
        clean = mobile_probe.sanitize_text(text)
        for secret in ("/Applications", "/Users", "Jane", TEAM_ID, APPLE_DEVICE_ID, "3f2a1b4c", "jane@example.com"):
            self.assertNotIn(secret, clean)
        self.assertIn("<signing-identity>", clean)
        self.assertIn("<team-id>", clean)

    def test_keeps_system_library_paths_and_placeholders(self):
        clean = mobile_probe.sanitize_text("links /usr/lib/libc++.1.dylib and <ndk-sysroot>/usr/include/c++/v1")
        self.assertIn("/usr/lib/libc++.1.dylib", clean)
        self.assertIn("<ndk-sysroot>/usr/include/c++/v1", clean)

    def test_keeps_build_strings_that_only_look_like_team_ids(self):
        self.assertEqual(mobile_probe.sanitize_text("F741BXXU1A"), "F741BXXU1A")

    def test_private_values_are_replaced_everywhere(self):
        record = {"note": f"device {SERIAL} is ready", "list": [SERIAL]}
        clean = mobile_probe.finalize_record(record, (SERIAL,))
        self.assertNotIn(SERIAL, json.dumps(clean))

    def test_forbidden_keys_are_refused(self):
        for key in ("serial", "teamId", "udid", "path"):
            with self.assertRaises(mobile_probe.LeakError):
                mobile_probe.sanitize({key: "value"})

    def test_verify_refuses_leftovers(self):
        with self.assertRaises(mobile_probe.LeakError):
            mobile_probe.verify_record({"x": "/Users/jane/file"})
        with self.assertRaises(mobile_probe.LeakError):
            mobile_probe.verify_record({"x": "fine"}, (SERIAL, "fine"))


class ParserTests(unittest.TestCase):
    def test_xcode_version(self):
        self.assertEqual(mobile_probe.parse_xcode_version("Xcode 16.2\nBuild version 16C5032a\n"), ("16.2", "16C5032a"))
        self.assertIsNone(mobile_probe.parse_xcode_version("xcode-select: error"))

    def test_load_alignments(self):
        output = (
            "  LOAD           0x000000 0x0000000000000000 0x0000000000000000 0x001000 0x001000 R   0x4000\n"
            "  LOAD           0x001000 0x0000000000005000 0x0000000000005000 0x002000 0x002000 R E 0x1000\n"
            "  DYNAMIC        0x003000 0x0000000000008000 0x0000000000008000 0x000100 0x000100 RW  0x8\n"
        )
        self.assertEqual(mobile_probe.parse_load_alignments(output), [0x4000, 0x1000])

    def test_adb_devices_lists_only_ready_devices(self):
        text = f"List of devices attached\n{SERIAL}\tdevice usb:1\nother\tunauthorized\n"
        self.assertEqual(mobile_probe.parse_adb_devices(text), [SERIAL])

    def test_vulkan_version_and_vkjson(self):
        self.assertEqual(mobile_probe.decode_vulkan_version((1 << 22) | (3 << 12) | 231), "1.3.231")
        document = {
            "devices": [
                {
                    "properties": {"apiVersion": (1 << 22) | (1 << 12), "deviceName": "Example GPU"},
                    "extensions": [{"extensionName": "VK_EXT_swapchain_maintenance1"}],
                }
            ],
            "extensions": [{"extensionName": "VK_KHR_android_surface"}],
        }
        parsed = mobile_probe.parse_vkjson("noise " + json.dumps(document))
        self.assertEqual(parsed["apiVersion"], "1.1.0")
        self.assertTrue(parsed["hasAndroidSurface"])
        self.assertTrue(parsed["hasSwapchainMaintenance1"])
        self.assertIsNone(mobile_probe.parse_vkjson("not json"))


class MissingToolTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.home = Path(self.directory.name)

    def test_every_probe_reports_not_installed_without_sdks(self):
        for name in mobile_probe.PROBES:
            with self.subTest(probe=name):
                record = mobile_probe.run_probe(name, FakeHost(self.home), options())
                self.assertEqual(record["status"], mobile_probe.STATUS_NOT_INSTALLED)
                self.assertTrue(record["reason"].startswith("Not installed:"))
                self.assertTrue(record["missing"])

    def test_apple_probes_name_xcode(self):
        for name in ("apple-toolchain", "apple-signing"):
            record = mobile_probe.run_probe(name, FakeHost(self.home), options())
            self.assertEqual(record["missing"], ["Xcode"])

    def test_android_toolchain_names_sdk_ndk_and_adb(self):
        record = mobile_probe.run_probe("android-toolchain", FakeHost(self.home), options())
        self.assertEqual(record["missing"], ["Android SDK", "Android NDK", "adb"])

    def test_command_line_tools_alone_are_not_xcode(self):
        def answers(command):
            if command[0] == "xcode-select":
                return Result(0, "/Library/Developer/CommandLineTools\n")
            return Result(1, "xcode-select: error: tool 'xcodebuild' requires Xcode")

        record = mobile_probe.run_probe("apple-toolchain", FakeHost(self.home, answers), options())
        self.assertEqual(record["status"], mobile_probe.STATUS_NOT_INSTALLED)

    def test_missing_iphoneos_sdk_is_reported(self):
        def answers(command):
            if command[0] == "xcodebuild":
                return Result(0, "Xcode 16.2\nBuild version 16C5032a\n")
            if command[0] == "xcode-select":
                return Result(0, "/x\n")
            return Result(1, "")

        record = mobile_probe.run_probe("apple-toolchain", FakeHost(self.home, answers), options())
        self.assertEqual(record["missing"], ["iphoneos SDK", "iphonesimulator SDK"])

    def test_wrong_clang_major_is_not_accepted(self):
        clang = self.home / "clang++"
        clang.write_text("")

        def answers(command):
            if command[0] == "xcodebuild":
                return Result(0, "Xcode 16.2\nBuild version 16C5032a\n")
            if command[0] == "xcode-select":
                return Result(0, "/x\n")
            if command[0] == "xcrun":
                return Result(0, "17.2\n")
            if command[0] == str(clang):
                return Result(0, "Apple clang version 17.0.0\n")
            return Result(1, "")

        record = mobile_probe.run_probe("apple-toolchain", FakeHost(self.home, answers), options(clang=str(clang)))
        self.assertEqual(record["status"], mobile_probe.STATUS_NOT_INSTALLED)
        self.assertIn("found major 17", record["missing"][0])

    def test_failed_compile_is_a_failed_probe_with_sanitized_diagnostics(self):
        clang = self.home / "clang++"
        clang.write_text("")

        def answers(command):
            if command[0] == "xcodebuild":
                return Result(0, "Xcode 16.2\nBuild version 16C5032a\n")
            if command[0] == "xcode-select":
                return Result(0, "/x\n")
            if command[0] == "xcrun":
                if "--show-sdk-path" in command:
                    return Result(0, "/Applications/Xcode.app/SDKs/iPhoneOS.sdk\n")
                return Result(0, "18.2\n")
            if command[0] == str(clang) and "--version" in command:
                return Result(0, f"clang version {mobile_probe.read_pinned_llvm_major()}.1.3\n")
            if command[0] == str(clang):
                return Result(1, "/Users/jane/Ludus/tools/mobile-probe/probe_cxx23.cpp:1:1: error: boom\n")
            return Result(0, "")

        record = mobile_probe.run_probe("apple-toolchain", FakeHost(self.home, answers), options(clang=str(clang)))
        self.assertEqual(record["status"], mobile_probe.STATUS_FAILED)
        text = json.dumps(record)
        self.assertNotIn("/Users/jane", text)
        self.assertNotIn("/Applications", text)
        self.assertIn("error: boom", text)

    def test_android_signing_without_a_keystore_is_blocked(self):
        (self.home / "Android" / "Sdk").mkdir(parents=True)
        host = FakeHost(self.home, tools={"keytool": "/usr/bin/keytool"})
        record = mobile_probe.run_probe("android-signing", host, options())
        self.assertEqual(record["status"], mobile_probe.STATUS_BLOCKED)
        self.assertIs(record["hasDebugKeystore"], False)

    def test_android_signing_records_only_a_boolean(self):
        (self.home / "Android" / "Sdk").mkdir(parents=True)
        (self.home / ".android").mkdir()
        (self.home / ".android" / "debug.keystore").write_text("x")
        host = FakeHost(self.home, lambda command: Result(0, "Alias name: androiddebugkey\n"), {"keytool": "/usr/bin/keytool"})
        record = mobile_probe.run_probe("android-signing", host, options())
        self.assertEqual(record["status"], mobile_probe.STATUS_PASSED)
        self.assertNotIn("androiddebugkey", json.dumps(record))

    def test_apple_signing_records_yes_or_no_only(self):
        def answers(command):
            if command[0] == "xcodebuild":
                return Result(0, "Xcode 16.2\nBuild version 16C5032a\n")
            if command[:2] == ["security", "find-identity"]:
                return Result(0, f'  1) ABCDEF "{IDENTITY}"\n     1 valid identities found\n')
            if command[:2] == ["security", "find-certificate"]:
                return Result(0, "-----BEGIN CERTIFICATE-----\n")
            if command[0] == "openssl":
                return Result(0, f"subject=UID=X, OU={TEAM_ID}, CN=Jane Doe\n")
            return Result(1, "")

        record = mobile_probe.run_probe("apple-signing", FakeHost(self.home, answers), options())
        text = json.dumps(record)
        self.assertIs(record["hasDevelopmentIdentity"], True)
        self.assertIs(record["hasDevelopmentTeam"], True)
        for secret in (TEAM_ID, "Jane", "Doe"):
            self.assertNotIn(secret, text)

    def test_devices_never_record_serials(self):
        sdk = self.home / "Android" / "Sdk"
        (sdk / "platform-tools").mkdir(parents=True)
        (sdk / "platform-tools" / "adb").write_text("")
        vkjson = json.dumps({"devices": [{"properties": {"apiVersion": 1 << 22, "deviceName": "GPU"}, "extensions": []}]})
        properties = {
            "ro.product.manufacturer": "samsung",
            "ro.product.model": "SM-F000X",
            "ro.build.version.release": "15",
            "ro.build.version.sdk": "35",
            "ro.build.display.id": f"BUILD.{SERIAL}.1",
            "ro.product.cpu.abi": "arm64-v8a",
        }

        def answers(command):
            if command[1:] == ["devices"]:
                return Result(0, f"List of devices attached\n{SERIAL}\tdevice\n")
            if "getprop" in command:
                return Result(0, properties.get(command[-1], "") + "\n")
            if "getconf" in command:
                return Result(0, "16384\n")
            if "vkjson" in command:
                return Result(0, vkjson)
            return Result(1, "")

        record = mobile_probe.run_probe("devices", FakeHost(self.home, answers), options())
        self.assertNotIn(SERIAL, json.dumps(record))
        self.assertEqual(record["devices"][0]["pageSizeBytes"], 16384)
        self.assertEqual(record["devices"][0]["label"], "android-1")
        self.assertEqual(record["status"], mobile_probe.STATUS_BLOCKED)  # Xcode is missing, so the inventory is partial


class DriverTests(unittest.TestCase):
    def test_main_writes_records_and_returns_not_installed(self):
        with tempfile.TemporaryDirectory() as home, tempfile.TemporaryDirectory() as output:
            out = io.StringIO()
            with redirect_stdout(out):
                code = mobile_probe.main(["apple-toolchain", "--output-dir", output], FakeHost(Path(home)))
            self.assertEqual(code, mobile_probe.EXIT_CODES[mobile_probe.STATUS_NOT_INSTALLED])
            record = json.loads((Path(output) / "apple-toolchain.json").read_text())
            self.assertEqual(record["status"], mobile_probe.STATUS_NOT_INSTALLED)
            self.assertIn("not-installed", out.getvalue())


class DeviceRunnerTests(unittest.TestCase):
    def run_runner(self, arguments, environment):
        errors = io.StringIO()
        with redirect_stderr(errors):
            code = mobile_device_runner.main(arguments, environment)
        return code, errors.getvalue()

    def test_refuses_without_a_selected_device(self):
        for platform_name in mobile_device_runner.PLATFORMS:
            code, message = self.run_runner(["--platform", platform_name, "--", "/bin/true"], {})
            self.assertEqual(code, mobile_device_runner.EXIT_NO_DEVICE)
            self.assertIn("no device selected", message)

    def test_blank_selection_counts_as_none(self):
        code, _ = self.run_runner(["--platform", "android", "--", "/bin/true"], {mobile_device_runner.DEVICE_ENVIRONMENT_VARIABLE: "  "})
        self.assertEqual(code, mobile_device_runner.EXIT_NO_DEVICE)

    def test_emulator_and_device_presets_do_not_mix(self):
        environment = {mobile_device_runner.DEVICE_ENVIRONMENT_VARIABLE: "emulator-5554"}
        code, message = self.run_runner(["--platform", "android", "--", "/bin/true"], environment)
        self.assertEqual(code, mobile_device_runner.EXIT_NO_DEVICE)
        self.assertIn("physical device", message)

    def test_ios_device_cannot_run_a_bare_executable(self):
        environment = {mobile_device_runner.DEVICE_ENVIRONMENT_VARIABLE: "device"}
        code, message = self.run_runner(["--platform", "ios", "--", "/bin/true"], environment)
        self.assertEqual(code, mobile_device_runner.EXIT_UNSUPPORTED)
        self.assertIn("app bundle", message)

    def test_missing_adb_is_reported(self):
        environment = {mobile_device_runner.DEVICE_ENVIRONMENT_VARIABLE: "device", "PATH": ""}
        with patch.object(mobile_device_runner, "resolve_sdk", return_value=None):
            code, message = self.run_runner(["--platform", "android", "--", "/bin/true"], environment)
        self.assertEqual(code, mobile_device_runner.EXIT_TOOL_MISSING)
        self.assertIn("adb is not installed", message)


if __name__ == "__main__":
    unittest.main()
