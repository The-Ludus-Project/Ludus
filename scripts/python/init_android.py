"""Install the optional Android SDK and Java used by the mobile probes."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ElementTree
import zipfile

import build_slots
import init_progress
from android_tools import managed_sdk, managed_java_home, read_android_pins


RETRY_COMMAND = "./init.sh --cli --android-only"
NDK_HOSTS = {"Darwin": "darwin-x86_64", "Linux": "linux-x86_64"}
DOWNLOAD_SOCKET_TIMEOUT_SECONDS = 60
LICENSE_TIMEOUT_SECONDS = 10
SUPPORTED_HOSTS = ("Darwin-arm64", "Darwin-x86_64", "Linux-x86_64")


def host_key() -> str:
    return f"{platform.system()}-{platform.machine().lower()}"


def download_verified(archive_pin: dict, directory: Path, engine) -> Path:
    url = archive_pin["url"]
    archive = directory / url.rsplit("/", 1)[-1]
    print(f"Network access: downloading {url}", flush=True)
    with urllib.request.urlopen(url, timeout=DOWNLOAD_SOCKET_TIMEOUT_SECONDS) as response, archive.open("wb") as output:
        shutil.copyfileobj(response, output)
    if engine.file_sha256(archive) != archive_pin["sha256"]:
        raise engine.EngineError(f"Checksum mismatch for {archive.name}; extraction refused.")
    return archive


def install_java(root: Path, pin: dict, key: str, engine) -> Path:
    java_home = managed_java_home(root, pin, key.split("-", 1)[0])
    if all(is_executable(java_home / "bin" / name) for name in ("java", "keytool", "javac")):
        return java_home
    prefix = root / "out/host-tools/android-jdk" / pin["version"]
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".install-", dir=prefix.parent) as temporary:
        staging = Path(temporary)
        archive = download_verified(pin["archives"][key], staging, engine)
        # The upstream archive is verified above and is extracted into private staging.
        engine.run(["tar", "-xf", archive], cwd=staging)
        unpacked = staging / f"jdk-{pin['version']}"
        home = unpacked / java_home.relative_to(prefix)
        if not all(is_executable(home / "bin" / name) for name in ("java", "keytool", "javac")):
            raise engine.EngineError("The Android Java archive is missing required JDK executables.")
        if prefix.exists():
            prefix.rename(staging / "old")
        unpacked.replace(prefix)
    return java_home


def is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def install_sdkmanager(sdk: Path, pin: dict, system: str, engine) -> Path:
    directory = sdk / "cmdline-tools" / pin["version"]
    manager = directory / "bin/sdkmanager"
    if is_executable(manager) and (directory / "source.properties").is_file():
        return manager
    directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".install-", dir=directory.parent) as temporary:
        staging = Path(temporary)
        archive = download_verified(pin["archives"][system], staging, engine)
        with zipfile.ZipFile(archive) as contents:
            contents.extractall(staging)
        unpacked = staging / "cmdline-tools"
        if not (unpacked / "bin/sdkmanager").is_file() or not (unpacked / "source.properties").is_file():
            raise engine.EngineError("The Android command-line tools archive is missing sdkmanager or its metadata.")
        for executable in (unpacked / "bin").iterdir():
            executable.chmod(0o755)
        if directory.exists():
            directory.rename(staging / "old")
        unpacked.replace(directory)
    return manager


def missing_packages(sdk: Path, pins: dict, system: str) -> list[str]:
    ndk = sdk / "ndk" / pins["ndk"]
    ndk_bin = ndk / "toolchains/llvm/prebuilt" / NDK_HOSTS[system] / "bin"
    packages = {
        "platform-tools": is_executable(sdk / "platform-tools/adb"),
        f"platforms;{pins['platform']}": (sdk / "platforms" / pins["platform"] / "android.jar").is_file(),
        f"build-tools;{pins['buildTools']}": is_executable(sdk / "build-tools" / pins["buildTools"] / "aapt2"),
        f"ndk;{pins['ndk']}": (ndk / "source.properties").is_file() and is_executable(ndk_bin / "clang++"),
    }
    return [package for package, is_installed in packages.items() if not is_installed]


def license_path(sdk: Path, license_pin: dict) -> Path:
    return sdk / "licenses" / license_pin["id"]


def has_accepted_license(sdk: Path, license_pin: dict) -> bool:
    path = license_path(sdk, license_pin)
    return path.is_file() and license_pin["acceptanceSha1"] in path.read_text(encoding="utf-8").splitlines()


def record_license(sdk: Path, license_pin: dict) -> None:
    # Google SDK Manager stores SHA-1 of the accepted UTF-8 license text, scoped
    # by license ID. Only record the specific agreement reviewed in setup.
    # https://android.googlesource.com/platform/tools/base/+/mirror-goog-studio-master-dev/repository/src/main/java/com/android/repository/api/License.java
    path = license_path(sdk, license_pin)
    path.parent.mkdir(parents=True, exist_ok=True)
    previous = path.read_text(encoding="utf-8") if path.is_file() else ""
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent, delete=False) as output:
        temporary = Path(output.name)
        if previous:
            output.write(previous.rstrip("\n") + "\n")
        output.write(license_pin["acceptanceSha1"] + "\n")
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def load_license_text(license_pin: dict) -> str:
    with urllib.request.urlopen(license_pin["repository"], timeout=LICENSE_TIMEOUT_SECONDS) as response:
        repository = ElementTree.parse(response).getroot()
    for license_element in repository.findall("license"):
        if license_element.get("id") == license_pin["id"]:
            agreement = (license_element.text or "").strip()
            # The repository feed and sdkmanager wrap the same terms differently.
            words = " ".join(agreement.split()).encode("utf-8")
            if hashlib.sha256(words).hexdigest() == license_pin["reviewTextSha256"]:
                return agreement
    raise ValueError("The Android SDK agreement in Google's repository differs from the setup pin. "
                     "Update Ludus before accepting the new agreement.")


def confirm_license(sdk: Path, license_pin: dict, is_accepted: bool, engine) -> None:
    if has_accepted_license(sdk, license_pin):
        return
    if not is_accepted and sys.stdin.isatty():
        print(load_license_text(license_pin), flush=True)
        is_accepted = input("Accept the Android SDK License Agreement and install Android tools? [y/N] ").strip().lower() == "y"
    if not is_accepted:
        raise engine.EngineError(f"Android setup requires acceptance of the SDK license. "
                                 f"Rerun {RETRY_COMMAND} in an interactive terminal or use graphical setup.")
    record_license(sdk, license_pin)


def install_packages(root: Path, sdk: Path, manager: Path, java_home: Path, packages: list[str], engine) -> None:
    environment = os.environ.copy()
    environment.update(JAVA_HOME=str(java_home), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
                       ANDROID_USER_HOME=str(root / "out/host-tools/android-user"))
    command = [str(manager), f"--sdk_root={sdk}", "--install", *packages]
    completed = subprocess.run(command, cwd=root, env=environment, check=False,
                               stdin=None if sys.stdin.isatty() else subprocess.DEVNULL)
    if completed.returncode != 0:
        raise engine.EngineError(f"sdkmanager exited with status {completed.returncode}. "
                                 f"Check the output and rerun {RETRY_COMMAND} to retry.")


def install_android(root: Path, engine, *, is_license_accepted: bool = False) -> None:
    init_progress.begin("android")
    key = host_key()
    if key not in SUPPORTED_HOSTS:
        raise engine.EngineError("Android setup supports macOS (Apple silicon or Intel) and Linux x64.")
    system = key.split("-", 1)[0]
    sdk = managed_sdk(root)
    try:
        pins = read_android_pins(root)
        sdk.parent.mkdir(parents=True, exist_ok=True)
        lock = build_slots.try_lock(sdk.parent / ".android-setup.lock")
        if lock is None:
            raise engine.EngineError(f"Android setup is already running for this checkout. Wait, then rerun {RETRY_COMMAND}.")
        try:
            confirm_license(sdk, pins["sdkLicense"], is_license_accepted, engine)
            java_home = install_java(root, pins["jdk"], key, engine)
            manager = install_sdkmanager(sdk, pins["commandLineTools"], system, engine)
            packages = missing_packages(sdk, pins, system)
            if not packages:
                init_progress.up_to_date("Android SDK, NDK and Java are installed")
                return
            install_packages(root, sdk, manager, java_home, packages, engine)
            missing = missing_packages(sdk, pins, system)
            if missing:
                raise engine.EngineError(f"Android setup did not install {', '.join(missing)}. "
                                         f"Check the license and download output, then rerun {RETRY_COMMAND}.")
        finally:
            build_slots.unlock(lock)
    except (OSError, ValueError, EOFError, ElementTree.ParseError, zipfile.BadZipFile) as error:
        raise engine.EngineError(f"Android setup failed: {error}. Check network and disk space, then rerun {RETRY_COMMAND}.") from error
    print(f"Installed the Android SDK and NDK at {sdk}")
    print("Connect your device with USB debugging enabled, then run ./scripts/mobile-probe devices.")
