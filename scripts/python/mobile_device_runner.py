#!/usr/bin/env python3
"""Run one target test executable on a selected mobile device, simulator or emulator.

The mobile CMake toolchain files set CMAKE_CROSSCOMPILING_EMULATOR to this
runner, so CTest never executes an iOS or Android binary on the Mac. The runner
refuses to run unless LUDUS_MOBILE_DEVICE names a connected target of the right
kind; it never picks a device by itself. Exit statuses: 78 no device selected or
the selection does not match the preset, 69 a required tool is missing, 70 the
target cannot run a bare executable, 74 the device is not reachable; otherwise
the executable's own status. Policy owner:
docs/development/mobile-support-plan.md (Target profiles, Test execution).
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

DEVICE_ENVIRONMENT_VARIABLE = "LUDUS_MOBILE_DEVICE"
PLATFORMS = ("ios", "ios-simulator", "android", "android-emulator")
EXIT_NO_DEVICE = 78
EXIT_TOOL_MISSING = 69
EXIT_UNSUPPORTED = 70
EXIT_UNREACHABLE = 74
ANDROID_EMULATOR_PREFIX = "emulator-"
ANDROID_REMOTE_DIRECTORY = "/data/local/tmp/ludus-tests"
EXIT_MARKER = "__LUDUS_EXIT__:"
RUN_TIMEOUT_SECONDS = 600
QUERY_TIMEOUT_SECONDS = 60


@dataclass
class Result:
    returncode: int
    output: str


def run_command(command: list[str], timeout: int = QUERY_TIMEOUT_SECONDS) -> Result:
    """Run a command and capture its output; a missing tool or timeout becomes a non-zero result."""
    try:
        completed = subprocess.run(
            command, capture_output=True, text=True, timeout=timeout, check=False
        )
    except FileNotFoundError:
        return Result(127, "")
    except subprocess.TimeoutExpired:
        return Result(124, "")
    return Result(completed.returncode, completed.stdout + completed.stderr)


def report(message: str) -> None:
    print(f"mobile-device-runner: {message}", file=sys.stderr)


def find_adb(environment: dict[str, str]) -> str | None:
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        root = environment.get(variable)
        if root:
            candidate = Path(root) / "platform-tools" / "adb"
            if candidate.is_file():
                return str(candidate)
    return shutil.which("adb")


def select_device(environment: dict[str, str], platform: str) -> str | None:
    """Return the selected device identifier, or None when the selection is missing or empty."""
    selected = environment.get(DEVICE_ENVIRONMENT_VARIABLE, "").strip()
    return selected if selected else None


def run_android(device: str, executable: str, arguments: list[str], emulator: bool, environment: dict[str, str]) -> int:
    is_emulator_device = device.startswith(ANDROID_EMULATOR_PREFIX)
    if emulator != is_emulator_device:
        kind = "an emulator" if emulator else "a physical device"
        report(f"this preset needs {kind}, but {DEVICE_ENVIRONMENT_VARIABLE} selects the other kind.")
        return EXIT_NO_DEVICE
    adb = find_adb(environment)
    if adb is None:
        report("adb is not installed. Install the Android SDK platform-tools; see scripts/mobile-probe android-toolchain.")
        return EXIT_TOOL_MISSING
    state = run_command([adb, "-s", device, "get-state"])
    if state.returncode != 0 or state.output.strip() != "device":
        report(f"the selected device is not connected and authorized (adb state: {state.output.strip() or 'unknown'}).")
        return EXIT_UNREACHABLE
    remote_directory = f"{ANDROID_REMOTE_DIRECTORY}/{uuid.uuid4().hex}"
    remote_executable = f"{remote_directory}/{Path(executable).name}"
    setup = run_command([adb, "-s", device, "shell", f"mkdir -p {remote_directory}"])
    pushed = run_command([adb, "-s", device, "push", executable, remote_executable])
    if setup.returncode != 0 or pushed.returncode != 0:
        report("could not copy the test executable to the device.")
        return EXIT_UNREACHABLE
    quoted_arguments = " ".join("'" + argument.replace("'", "'\\''") + "'" for argument in arguments)
    script = f"chmod +x {remote_executable} && {remote_executable} {quoted_arguments}; echo {EXIT_MARKER}$?"
    ran = run_command([adb, "-s", device, "shell", script], timeout=RUN_TIMEOUT_SECONDS)
    run_command([adb, "-s", device, "shell", f"rm -rf {remote_directory}"])
    status = 1
    for line in ran.output.splitlines():
        if line.startswith(EXIT_MARKER):
            status = int(line[len(EXIT_MARKER):])
        else:
            print(line)
    return status


def run_simulator(device: str, executable: str, arguments: list[str]) -> int:
    if shutil.which("xcrun") is None:
        report("xcrun is not installed. Install Xcode; see scripts/mobile-probe apple-toolchain.")
        return EXIT_TOOL_MISSING
    listing = run_command(["xcrun", "simctl", "list", "devices", "booted", "--json"])
    try:
        booted = json.loads(listing.output)["devices"]
    except (ValueError, KeyError):
        report("could not list booted simulators; is Xcode selected with xcode-select?")
        return EXIT_UNREACHABLE
    identifiers = [entry["udid"] for runtime in booted.values() for entry in runtime]
    if device not in identifiers:
        report("the selected simulator is not booted.")
        return EXIT_UNREACHABLE
    completed = subprocess.run(["xcrun", "simctl", "spawn", device, executable, *arguments], check=False)
    return completed.returncode


def main(argv: list[str], environment: dict[str, str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- executable [arguments...]")
    options = parser.parse_args(argv)
    command = [part for index, part in enumerate(options.command) if not (index == 0 and part == "--")]
    environment = dict(os.environ) if environment is None else environment
    if not command:
        report("no executable was given.")
        return EXIT_NO_DEVICE
    device = select_device(environment, options.platform)
    if device is None:
        report(
            f"no device selected, so nothing was run. Set {DEVICE_ENVIRONMENT_VARIABLE} to a connected "
            f"{options.platform} target (list them with scripts/mobile-probe devices). "
            "CTest never runs a mobile binary on this computer."
        )
        return EXIT_NO_DEVICE
    executable, arguments = command[0], command[1:]
    if options.platform == "ios":
        report(
            "an iOS device cannot run a bare executable; device tests need an app bundle installed "
            "with devicectl (planned for M1). Use ios-simulator-clang-development for headless runs."
        )
        return EXIT_UNSUPPORTED
    if options.platform == "ios-simulator":
        return run_simulator(device, executable, arguments)
    return run_android(device, executable, arguments, options.platform == "android-emulator", environment)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
