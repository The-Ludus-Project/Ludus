"""Install the optional Android SDK and Java used by the mobile probes."""
from __future__ import annotations

from collections.abc import Callable
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import xml.etree.ElementTree as ElementTree
import zipfile

import build_slots
import init_progress
from android_tools import existing_sdk_roots, managed_sdk, managed_java_home, read_android_pins, read_property, read_revision


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
    if is_pinned_java(java_home, pin["version"]):
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
        if not is_pinned_java(home, pin["version"]):
            raise engine.EngineError("The Android Java archive is missing its pinned runtime or JDK executables.")
        if prefix.exists():
            prefix.rename(staging / "old")
        unpacked.replace(prefix)
    return java_home


def is_pinned_java(home: Path, version: str) -> bool:
    runtime = read_property(home / "release", "JAVA_RUNTIME_VERSION")
    return (runtime is not None and runtime.removesuffix("-LTS") == version
            and all(is_executable(home / "bin" / name) for name in ("java", "keytool", "javac")))


def is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def has_pinned_sdkmanager(directory: Path, version: str) -> bool:
    return (is_executable(directory / "bin/sdkmanager")
            and (directory / "lib/sdkmanager-classpath.jar").is_file()
            and read_revision(directory) == version)


def publish_package(staged: Path, destination: Path, working: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    previous = working / "previous"
    if destination.exists() or destination.is_symlink():
        destination.rename(previous)
    try:
        staged.replace(destination)
    except OSError:
        if previous.exists() or previous.is_symlink():
            previous.rename(destination)
        raise


def install_sdkmanager(sdk: Path, pin: dict, system: str, engine) -> Path:
    directory = sdk / "cmdline-tools" / pin["version"]
    manager = directory / "bin/sdkmanager"
    if has_pinned_sdkmanager(directory, pin["version"]):
        return manager
    directory.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".install-", dir=directory.parent) as temporary:
        staging = Path(temporary)
        archive = download_verified(pin["archives"][system], staging, engine)
        with zipfile.ZipFile(archive) as contents:
            contents.extractall(staging)
        unpacked = staging / "cmdline-tools"
        for executable in (unpacked / "bin").iterdir():
            executable.chmod(0o755)
        if not has_pinned_sdkmanager(unpacked, pin["version"]):
            raise engine.EngineError("The Android command-line tools archive is missing its pinned SDK Manager.")
        publish_package(unpacked, directory, staging)
    return manager


def stable_version(version: str | None) -> tuple[int, ...]:
    if version is None or re.fullmatch(r"\d+\.\d+\.\d+", version) is None:
        return ()
    return tuple(map(int, version.split(".")))


def missing_packages(sdk: Path, pins: dict, system: str) -> dict[str, Path]:
    platform_tools = sdk / "platform-tools"
    platform = sdk / "platforms" / pins["platform"]
    build_tools = sdk / "build-tools" / pins["buildTools"]
    ndk = sdk / "ndk" / pins["ndk"]
    ndk_bin = ndk / "toolchains/llvm/prebuilt" / NDK_HOSTS[system] / "bin"
    packages = {
        "platform-tools": (
            platform_tools,
            is_executable(platform_tools / "adb")
            and stable_version(read_revision(platform_tools)) >= stable_version(pins["minimumPlatformTools"]),
        ),
        f"platforms;{pins['platform']}": (
            platform,
            (platform / "android.jar").is_file()
            and read_property(platform / "source.properties", "AndroidVersion.ApiLevel") == pins["platform"].removeprefix("android-"),
        ),
        f"build-tools;{pins['buildTools']}": (
            build_tools, is_executable(build_tools / "aapt2") and read_revision(build_tools) == pins["buildTools"],
        ),
        f"ndk;{pins['ndk']}": (
            ndk, is_executable(ndk_bin / "clang++") and read_revision(ndk) == pins["ndk"],
        ),
    }
    return {package: directory for package, (directory, is_installed) in packages.items() if not is_installed}


def check_package_links(package: Path) -> None:
    """Approve only relative symlinks that still resolve within the copied package."""
    for directory, children, files in os.walk(package):
        for name in (*children, *files):
            path = Path(directory) / name
            if path.is_symlink() and (path.readlink().is_absolute() or not path.resolve(strict=True).is_relative_to(package)):
                raise ValueError(f"{path.relative_to(package)} links outside its package")


def copy_sdk_package(sdk: Path, name: str, source: Path, destination: Path,
                     validate: Callable[[Path], bool]) -> bool:
    """Import one SDK package from a read-only source, publishing after validation."""
    try:
        with tempfile.TemporaryDirectory(prefix=".android-import-", dir=sdk.parent) as temporary:
            working = Path(temporary)
            staging_sdk = working / "sdk"
            staged = staging_sdk / destination.relative_to(sdk)
            shutil.copytree(source, staged, symlinks=True)
            check_package_links(staged.resolve())
            if source.name == "latest" and destination.parent.name == "cmdline-tools":
                # SDK Manager may have recorded this directory under the "latest"
                # package ID. The copied source.properties describes the actual revision.
                (staged / "package.xml").unlink(missing_ok=True)
            if not validate(staging_sdk):
                raise ValueError("the copy is missing required files or version metadata")
            publish_package(staged, destination, working)
    except (OSError, RuntimeError, ValueError) as error:
        print(f"Could not reuse Android package {name} from {source}: {error}. Trying another source or downloading it.",
              flush=True)
        return False
    print(f"Reused Android package {name} from {source}.", flush=True)
    return True


def reuse_sdk_packages(sdk: Path, pins: dict, system: str, sources: list[Path]) -> bool:
    sources = [source for source in sources if not source.is_relative_to(sdk) and not sdk.is_relative_to(source)]
    if not sources:
        return False

    is_reused = False
    version = pins["commandLineTools"]["version"]
    relative = Path("cmdline-tools") / version
    if not has_pinned_sdkmanager(sdk / relative, version):
        for source in (root / "cmdline-tools" / folder for root in sources for folder in (version, "latest")):
            try:
                is_compatible = has_pinned_sdkmanager(source, version)
            except (OSError, ValueError):
                continue
            if is_compatible and copy_sdk_package(
                    sdk, f"cmdline-tools;{version}", source, sdk / relative,
                    lambda staging: has_pinned_sdkmanager(staging / relative, version)):
                is_reused = True
                break

    packages = missing_packages(sdk, pins, system)
    if not packages:
        return is_reused
    source_missing = []
    for source in sources:
        try:
            source_missing.append((source, missing_packages(source, pins, system)))
        except (OSError, ValueError) as error:
            print(f"Could not read Android SDK metadata at {source}: {error}. Skipping that SDK.", flush=True)
    for name, destination in packages.items():
        for source, missing in source_missing:
            if name not in missing and copy_sdk_package(
                    sdk, name, source / destination.relative_to(sdk), destination,
                    lambda staging, package=name: package not in missing_packages(staging, pins, system)):
                is_reused = True
                break
    return is_reused


def check_managed_sdk_location(sdk: Path, engine) -> None:
    """Keep package manager operations in the checkout, including repairs."""
    if sdk != sdk.resolve():
        raise engine.EngineError(f"Android setup requires a checkout-local SDK directory at {sdk}. "
                                 "Move the link and rerun setup to keep the existing SDK intact.")
    versioned = ("cmdline-tools", "platforms", "build-tools", "ndk")
    versioned_roots = [sdk / name for name in versioned]
    directories = [*versioned_roots, sdk / "platform-tools", sdk / "licenses", sdk / ".temp"]
    for directory in versioned_roots:
        if directory.is_dir() and not directory.is_symlink():
            directories.extend(directory.iterdir())
    for directory in directories:
        if directory.is_symlink():
            raise engine.EngineError(f"Android setup cannot write through the link at {directory}. "
                                     "Move the link and rerun setup to keep the existing SDK intact.")


def repair_packages(root: Path, sdk: Path, manager: Path, java_home: Path, packages: dict[str, Path], engine) -> None:
    # SDK Manager can skip a damaged package if package.xml still marks it as installed.
    # Move only generated, incompatible package directories out of its way.
    with tempfile.TemporaryDirectory(prefix=".android-repair-", dir=sdk.parent) as temporary:
        retired = Path(temporary)
        try:
            for name, directory in packages.items():
                if directory.exists() or directory.is_symlink():
                    print(f"Replacing incompatible or incomplete Android package {name}.", flush=True)
                    directory.rename(retired / name)
            install_packages(root, sdk, manager, java_home, list(packages), engine)
        finally:
            for name, directory in packages.items():
                previous = retired / name
                if (previous.exists() or previous.is_symlink()) and not (directory.exists() or directory.is_symlink()):
                    previous.rename(directory)


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
    root = root.resolve()
    sdk = managed_sdk(root)
    try:
        pins = read_android_pins(root)
        check_managed_sdk_location(sdk, engine)
        sdk.parent.mkdir(parents=True, exist_ok=True)
        lock = build_slots.try_lock(sdk.parent / ".android-setup.lock")
        if lock is None:
            raise engine.EngineError(f"Android setup is already running for this checkout. Wait, then rerun {RETRY_COMMAND}.")
        try:
            confirm_license(sdk, pins["sdkLicense"], is_license_accepted, engine)
            sources = existing_sdk_roots(Path.home(), os.environ)
            is_reused = reuse_sdk_packages(sdk, pins, system, sources)
            java_home = install_java(root, pins["jdk"], key, engine)
            manager = install_sdkmanager(sdk, pins["commandLineTools"], system, engine)
            packages = missing_packages(sdk, pins, system)
            if packages:
                repair_packages(root, sdk, manager, java_home, packages, engine)
                missing = missing_packages(sdk, pins, system)
                if missing:
                    raise engine.EngineError(f"Android setup did not install {', '.join(missing)}. "
                                             f"Check the license and download output, then rerun {RETRY_COMMAND}.")
            elif not is_reused:
                init_progress.up_to_date("Android SDK, NDK and Java are installed")
                return
        finally:
            build_slots.unlock(lock)
    except (OSError, ValueError, EOFError, ElementTree.ParseError, zipfile.BadZipFile) as error:
        raise engine.EngineError(f"Android setup failed: {error}. Check network and disk space, then rerun {RETRY_COMMAND}.") from error
    print(f"Installed the Android SDK and NDK at {sdk}")
    print("Connect your device with USB debugging enabled, then run ./scripts/mobile-probe devices.")
