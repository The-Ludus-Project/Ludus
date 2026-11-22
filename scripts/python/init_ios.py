"""Provision Xcode's iOS tools for the optional macOS setup step."""
from __future__ import annotations

import os
from argparse import Namespace
from pathlib import Path
import shlex
import subprocess
import sys

import init_progress
import mobile_probe


XCODE_APPLICATION = Path("/Applications/Xcode.app")
XCODE_STORE_URL = "https://apps.apple.com/app/xcode/id497799835"
XCODE_STORE_LINK = "macappstore://itunes.apple.com/app/id497799835?mt=12"
XCODE_SELECT = "/usr/bin/xcode-select"
XCODEBUILD = "/usr/bin/xcodebuild"
XCRUN = "/usr/bin/xcrun"
OSASCRIPT = "/usr/bin/osascript"
APPLESCRIPT_CANCELLED = "(-128)"
IOS_SDKS = ("iphoneos", "iphonesimulator")
QUERY_TIMEOUT_SECONDS = 30


def xcode_developer_directory(path: Path) -> Path | None:
    if path.suffix == ".app":
        path = path / "Contents" / "Developer"
    return path if (path / "usr/bin/xcodebuild").is_file() else None


def missing_ios_tools(query) -> list[str]:
    missing = []
    for sdk in IOS_SDKS:
        status, path = query([XCRUN, "--sdk", sdk, "--show-sdk-path"])
        if status != 0 or not path or not Path(path).is_dir():
            missing.append(sdk + " SDK")
    status, path = query([XCRUN, "--find", "devicectl"])
    if status != 0 or not path or not Path(path).is_file():
        missing.append("devicectl")
    return missing


def require_xcode(args, root: Path, engine, query) -> tuple[Path, bool]:
    """Return the full Xcode developer directory and whether it needs selection."""
    configured = os.environ.get("DEVELOPER_DIR")
    status, path = query([XCODE_SELECT, "--print-path"])
    developer = xcode_developer_directory(Path(path)) if status == 0 and path else None
    if developer is not None:
        return developer, False
    if configured:
        raise engine.EngineError("DEVELOPER_DIR must point to a full Xcode installation for iOS setup. "
                                 "Correct it or unset it and retry.")

    installed = xcode_developer_directory(XCODE_APPLICATION)
    if installed is not None:
        if args.no_system_install:
            raise engine.EngineError("Select the installed Xcode with `sudo xcode-select --switch /Applications/Xcode.app`, "
                                     "or retry iOS setup without --no-system-install.")
        return installed, True

    remedy = f"Install Xcode from the Mac App Store ({XCODE_STORE_URL}), then retry iOS setup."
    if not args.no_system_install and not os.environ.get("CI") and (
        sys.stdin.isatty() or init_progress.is_machine_readable()
    ):
        try:
            engine.run(["/usr/bin/open", XCODE_STORE_LINK], cwd=root)
        except engine.EngineError as error:
            raise engine.EngineError(f"{remedy} The App Store could not be opened. {error}") from error
        remedy = "Finish installing Xcode in the App Store, then choose Retry or rerun `./init.sh --with-ios`."
    raise engine.EngineError(f"iOS setup needs the full Xcode app. {remedy}")


def run_privileged(engine, root: Path, command: list[object], *, action: str, recovery: str) -> None:
    """Run an Apple setup operation with administrator authorization when required."""
    is_root = os.geteuid() == 0
    is_graphical = not is_root and os.environ.get(init_progress.GUI_ENVIRONMENT) == "1" and not os.environ.get("CI")
    try:
        if is_graphical:
            # Thanks to Apple's AppleScript Language Guide, Commands Reference, for system
            # authentication through Standard Additions without a password argument.
            # https://developer.apple.com/library/archive/documentation/AppleScript/Conceptual/AppleScriptLangGuide/reference/ASLR_cmds.html
            script = [OSASCRIPT, "-e", "on run argv",
                      "-e", "do shell script (item 1 of argv) with administrator privileges",
                      "-e", "end run", shlex.join(str(part) for part in command)]
            with init_progress.show_user_action(f"Approve the macOS administrator prompt to {action}."):
                completed = engine.run(script, cwd=root, capture=True)
                if completed.stdout:
                    print(completed.stdout, end="", flush=True)
        else:
            prefix = [] if is_root else ["/usr/bin/sudo"]
            if prefix and not sys.stdin.isatty():
                prefix.append("-n")
            engine.run([*prefix, *command], cwd=root)
    except engine.EngineError as error:
        cause = error.__cause__
        if is_graphical and isinstance(cause, subprocess.CalledProcessError) and (
            (cause.stdout or "").rstrip().endswith(APPLESCRIPT_CANCELLED)
        ):
            raise engine.EngineError(f"Administrator approval was canceled. Choose Retry to {action}.") from error
        raise engine.EngineError(f"{recovery} {error}") from error


def prepare_ios(args, root: Path, engine) -> tuple[Path, bool]:
    environment = dict(os.environ)

    def query(command):
        return engine.capture_command_quiet(command, cwd=root, env=environment, timeout=QUERY_TIMEOUT_SECONDS)

    # Thanks to Apple Developer Documentation, "Downloading and installing
    # additional Xcode components", for the first-launch and platform commands.
    # https://developer.apple.com/documentation/xcode/downloading-and-installing-additional-xcode-components
    developer, should_select = require_xcode(args, root, engine, query)
    environment["DEVELOPER_DIR"] = str(developer)
    first_launch_status, _ = query([XCODEBUILD, "-checkFirstLaunchStatus"])
    if first_launch_status != 0:
        if args.no_system_install:
            raise engine.EngineError("Xcode first-launch setup is incomplete. Open Xcode and finish setup, "
                                     "or retry iOS setup without --no-system-install.")
        run_privileged(engine, root, ["/usr/bin/env", f"DEVELOPER_DIR={developer}", XCODEBUILD, "-runFirstLaunch"],
                       action="complete Xcode first-launch setup",
                       recovery="Open Xcode and finish its first-launch setup, then retry.")

    missing = missing_ios_tools(query)
    if any(sdk + " SDK" in missing for sdk in IOS_SDKS) and not args.no_system_install:
        print("Downloading iOS support with Xcode. This may take several minutes.", flush=True)
        try:
            engine.run([XCODEBUILD, "-downloadPlatform", "iOS"], cwd=root, env=environment)
        except engine.EngineError as error:
            raise engine.EngineError("Download iOS support in Xcode Settings > Components, then retry. "
                                     f"{error}") from error
        missing = missing_ios_tools(query)
    if missing:
        raise engine.EngineError(f"Xcode is missing {', '.join(missing)}. "
                                 "Open Xcode, finish first-launch setup and install iOS support in Settings > Components, "
                                 "then retry.")
    return developer, should_select


def verify_ios_toolchain(selection: tuple[Path, bool], root: Path, engine) -> None:
    developer, should_select = selection
    environment = {**os.environ, "DEVELOPER_DIR": str(developer)}
    options = Namespace(clang=str(engine.clang_cxx(root)), ios_minimum=mobile_probe.DEFAULT_IOS_MINIMUM)
    try:
        record = mobile_probe.run_probe("apple-toolchain", mobile_probe.Host(environment), options)
        path = mobile_probe.write_record(record, root / mobile_probe.OUTPUT_DIRECTORY).relative_to(root)
    except (mobile_probe.LeakError, OSError) as error:
        raise engine.EngineError(f"Could not write the Apple toolchain probe record. {error}") from error
    print(f"Apple toolchain probe record: {path}", flush=True)
    if "xcode" in record:
        xcode = record["xcode"]
        sdks = ", ".join(f"{sdk['sdk']} {sdk['version']}" for sdk in record["sdks"])
        print(f"Xcode {xcode['version']} ({xcode['build']}) with {sdks}. "
              f"Provisional deployment target iOS {record['deploymentTarget']}.", flush=True)
    if record["status"] != mobile_probe.STATUS_PASSED:
        raise engine.EngineError(report_probe_failure(record, path))

    if should_select:
        # Keep Command Line Tools active until Xcode passes the compile/link probe.
        run_privileged(engine, root, [XCODE_SELECT, "--switch", developer], action="select Xcode for command-line tools",
                       recovery="Select Xcode with `sudo xcode-select --switch /Applications/Xcode.app`, then retry.")
    print("Apple toolchain probe passed for the iOS device and Simulator SDKs.", flush=True)


def report_probe_failure(record: dict, path: Path) -> str:
    failed = [step for step in record.get("steps", ()) if not step["ok"]]
    if failed:
        for step in failed:
            print(f"{step['step']} failed.", flush=True)
            for diagnostic in step.get("diagnostics", ()):
                print(f"  {diagnostic}", flush=True)
        names = ", ".join(step["step"] for step in failed)
        return (f"Apple toolchain probe failed at {names}. For missing SDK headers or libraries, reinstall iOS "
                "support in Xcode Settings > Components. For a compiler or linker incompatibility, update Xcode "
                "or select another Xcode.app with DEVELOPER_DIR. Rerun ./init.sh --with-ios after the repair. "
                f"See {path} for diagnostics.")
    missing = record.get("missing", ())
    if missing and missing[0].startswith("upstream Clang"):
        remedy = "Install the required upstream LLVM, add its bin directory to PATH, then retry."
    else:
        remedy = "Open the selected Xcode, finish first launch and install iOS support in Settings > Components, then retry."
    return f"Apple toolchain probe could not resolve {', '.join(missing)}. {remedy} See {path}."
