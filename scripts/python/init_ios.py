"""Provision Xcode's iOS tools for the optional macOS setup step."""
from __future__ import annotations

import os
from pathlib import Path
import sys

import init_progress


XCODE_APPLICATION = Path("/Applications/Xcode.app")
XCODE_STORE_URL = "https://apps.apple.com/app/xcode/id497799835"
XCODE_STORE_LINK = "macappstore://itunes.apple.com/app/id497799835?mt=12"
XCODE_SELECT = "/usr/bin/xcode-select"
XCODEBUILD = "/usr/bin/xcodebuild"
XCRUN = "/usr/bin/xcrun"
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


def run_privileged(engine, root: Path, command: list[object]) -> None:
    prefix = [] if os.geteuid() == 0 else ["/usr/bin/sudo"]
    if prefix and not sys.stdin.isatty():
        prefix.append("-n")
    engine.run([*prefix, *command], cwd=root)


def setup_ios(args, root: Path, engine) -> None:
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
        try:
            run_privileged(engine, root, ["/usr/bin/env", f"DEVELOPER_DIR={developer}",
                                          XCODEBUILD, "-runFirstLaunch"])
        except engine.EngineError as error:
            raise engine.EngineError(f"Open Xcode and finish its first-launch setup, then retry. {error}") from error

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
    if should_select:
        # Keep Command Line Tools active until Xcode works. A failed first
        # launch can otherwise keep Apple's Python launcher from starting init.
        try:
            run_privileged(engine, root, [XCODE_SELECT, "--switch", developer])
        except engine.EngineError as error:
            raise engine.EngineError("Select the verified Xcode in a terminal with "
                                     "`sudo xcode-select --switch /Applications/Xcode.app`, then retry. "
                                     f"{error}") from error
    print("Verified the iOS device and Simulator SDKs and devicectl.", flush=True)
