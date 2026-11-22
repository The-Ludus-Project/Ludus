#!/usr/bin/env python3
"""Mobile M0 acceptance probes: Apple and Android toolchains, signing and devices.

Each probe writes one sanitized JSON record to ``out/mobile-probes/``. A record
never holds a serial number, a signing identity name, a team ID, a device
identifier or a machine path: values are collected from an allowlist of fields,
rewritten by ``sanitize`` and refused by ``verify_record`` if anything private is
left. A probe whose tool is missing reports the status ``not-installed`` and says
what to install; it never invents a result. Policy owner:
docs/development/mobile-support-plan.md ("Acceptance probes").

Exit statuses: 0 passed, 1 failed, 2 usage error, 3 not-installed, 4 blocked
(the tools work, but signing material or a connected device is missing).
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import datetime
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

from android_tools import managed_java_home, managed_sdk, read_android_pins, resolve_sdk

ROOT = Path(__file__).resolve().parents[2]
PROBE_SOURCES = ROOT / "tools" / "mobile-probe"
FOUNDATION_BASE_INCLUDE = ROOT / "modules" / "foundation" / "base" / "include"
TOOL_VERSIONS = ROOT / "config" / "tool_versions.json"
DEFAULT_OUTPUT_DIRECTORY = ROOT / "out" / "mobile-probes"

SCHEMA_VERSION = 1
STATUS_PASSED = "passed"
STATUS_FAILED = "failed"
STATUS_NOT_INSTALLED = "not-installed"
STATUS_BLOCKED = "blocked"
EXIT_CODES = {STATUS_PASSED: 0, STATUS_FAILED: 1, STATUS_NOT_INSTALLED: 3, STATUS_BLOCKED: 4}
EXIT_USAGE = 2

# Provisional inputs until the probes record the minimum OS and API (plan, Target
# profiles). They are options on the command line and appear in every record.
DEFAULT_IOS_MINIMUM = "17.0"
DEFAULT_ANDROID_API = 29
PAGE_SIZE_16_KIB = 0x4000
DIAGNOSTIC_LINE_LIMIT = 40
COMMAND_TIMEOUT_SECONDS = 300
QUERY_TIMEOUT_SECONDS = 60

WARNING_FLAGS = ["-Wall", "-Wextra", "-Wpedantic", "-Wshadow", "-Wundef", "-Wformat=2", "-Werror"]
CXX_FLAGS = ["-std=c++23", "-fno-exceptions"]
OBJCXX_FLAGS = ["-x", "objective-c++", "-fobjc-arc", "-fno-objc-exceptions"]

PROBES = ("apple-toolchain", "apple-signing", "android-toolchain", "android-signing", "devices")


# --------------------------------------------------------------------------- host


@dataclass
class Result:
    returncode: int
    output: str


class Host:
    """The machine under test: commands, environment and home directory.

    Tests replace it with a scripted host so probes run without any SDK.
    """

    def __init__(self, environment: dict[str, str] | None = None, home: Path | None = None, root: Path = ROOT):
        self.environment = dict(os.environ) if environment is None else environment
        self.home = Path.home() if home is None else home
        self.root = root

    def run(self, command: list[str], timeout: int = QUERY_TIMEOUT_SECONDS, input_text: str | None = None) -> Result:
        """Run a command; a missing executable or a timeout is a non-zero result, never an exception."""
        try:
            completed = subprocess.run(
                [str(part) for part in command],
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
                env=self.environment,
                input=input_text,
            )
        except FileNotFoundError:
            return Result(127, "")
        except subprocess.TimeoutExpired:
            return Result(124, "")
        return Result(completed.returncode, completed.stdout + completed.stderr)

    def which(self, name: str) -> str | None:
        return shutil.which(name, path=self.environment.get("PATH"))


# ------------------------------------------------------------------ sanitization

SYSTEM_PATH_PREFIXES = ("/usr/lib/", "/System/Library/", "/usr/bin/", "/bin/", "/Library/Developer/CommandLineTools")
PATH_PATTERN = re.compile(r"(?:(?<![A-Za-z0-9_.+)\]>~-])|(?<=-I)|(?<=-L)|(?<=-isysroot ))/[^\s'\"<>;,)]+|~/[^\s'\"<>;,)]+")
WINDOWS_PATH_PATTERN = re.compile(r"\b[A-Za-z]:\\[^\s'\"<>;,)]+")
SIGNING_IDENTITY_PATTERN = re.compile(
    r"(?:Apple Development|Apple Distribution|iPhone Developer|iPhone Distribution|"
    r"Developer ID (?:Application|Installer)|Mac Developer|3rd Party Mac Developer \w+)"
    r"(?:\s*:\s*[^\"\n]*?\([A-Z0-9]{10}\)|\s*:\s*[^\"\n]+)"
)
# A team ID is only recognizable in context ("(ABCDE12345)", "OU=ABCDE12345"); a bare
# ten-character token also matches harmless Samsung build strings.
TEAM_ID_PATTERN = re.compile(r"(\(|OU\s*=\s*|[Tt]eam\w*\W{1,4})(?=[A-Z0-9]*\d)(?=[A-Z0-9]*[A-Z])[A-Z0-9]{10}\b")
UUID_PATTERN = re.compile(r"\b[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}\b")
APPLE_DEVICE_ID_PATTERN = re.compile(r"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}\b")
HEX_40_PATTERN = re.compile(r"\b[0-9A-Fa-f]{40}\b")
EMAIL_PATTERN = re.compile(r"[\w.+-]+@[\w-]+\.[\w.-]+")
FORBIDDEN_KEYS = frozenset(
    {"serial", "serialnumber", "udid", "ecid", "teamid", "team", "identity", "signingidentity", "path", "devicename"}
)
REPLACEMENTS = (
    (SIGNING_IDENTITY_PATTERN, "<signing-identity>"),
    (EMAIL_PATTERN, "<email>"),
    (APPLE_DEVICE_ID_PATTERN, "<device-id>"),
    (UUID_PATTERN, "<device-id>"),
    (HEX_40_PATTERN, "<hash>"),
)


class LeakError(RuntimeError):
    """A record still held private material after sanitization; it must not be written."""


def sanitize_text(text: str, private_values: tuple[str, ...] = ()) -> str:
    """Rewrite every private-looking token of ``text`` to a placeholder."""
    for value in sorted((value for value in private_values if value), key=len, reverse=True):
        text = text.replace(value, "<private>")

    def replace_path(match: re.Match[str]) -> str:
        token = match.group(0)
        return token if token.startswith(SYSTEM_PATH_PREFIXES) else "<path>"

    text = PATH_PATTERN.sub(replace_path, text)
    text = WINDOWS_PATH_PATTERN.sub("<path>", text)
    for pattern, placeholder in REPLACEMENTS:
        text = pattern.sub(placeholder, text)
    return TEAM_ID_PATTERN.sub(lambda match: match.group(1) + "<team-id>", text)


def sanitize(value, private_values: tuple[str, ...] = ()):
    """Recursively sanitize the strings of a JSON-like value; forbidden keys are an error."""
    if isinstance(value, str):
        return sanitize_text(value, private_values)
    if isinstance(value, list):
        return [sanitize(item, private_values) for item in value]
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key.replace("_", "").replace("-", "").lower() in FORBIDDEN_KEYS:
                raise LeakError(f"record key '{key}' would carry private material")
            result[key] = sanitize(item, private_values)
        return result
    return value


def verify_record(record: dict, private_values: tuple[str, ...] = ()) -> None:
    """Fail closed: refuse a record that still holds a private value, a path or an identity."""
    text = json.dumps(record, sort_keys=True)
    for value in private_values:
        if value and value in text:
            raise LeakError("a private value survived sanitization")
    for pattern in (PATH_PATTERN, WINDOWS_PATH_PATTERN, SIGNING_IDENTITY_PATTERN, TEAM_ID_PATTERN, UUID_PATTERN, APPLE_DEVICE_ID_PATTERN, EMAIL_PATTERN):
        for match in pattern.finditer(text):
            token = match.group(0)
            if pattern is PATH_PATTERN and token.startswith(SYSTEM_PATH_PREFIXES):
                continue
            raise LeakError(f"record holds an unsanitized token ({pattern.pattern[:24]}...)")


def finalize_record(record: dict, private_values: tuple[str, ...] = ()) -> dict:
    clean = sanitize(record, private_values)
    verify_record(clean, private_values)
    return clean


# ------------------------------------------------------------------------ records


def read_pinned_llvm_major() -> int:
    minimum = json.loads(TOOL_VERSIONS.read_text())["minimum"]["clang"]
    return int(minimum.split(".")[0])


def git_commit(host: Host) -> str:
    result = host.run(["git", "-C", str(ROOT), "rev-parse", "--short=9", "HEAD"])
    commit = result.output.strip()
    return commit if result.returncode == 0 and re.fullmatch(r"[0-9a-f]{7,40}", commit) else "unknown"


def new_record(probe: str, host: Host) -> dict:
    return {
        "schema": SCHEMA_VERSION,
        "probe": probe,
        "status": STATUS_PASSED,
        "checkedOn": datetime.date.today().isoformat(),
        "ludusCommit": git_commit(host),
        "hostOs": f"macOS {platform.mac_ver()[0]}" if platform.system() == "Darwin" else platform.system(),
        "hostArchitecture": platform.machine(),
    }


def not_installed(record: dict, missing: list[str], remedy: str) -> dict:
    record["status"] = STATUS_NOT_INSTALLED
    record["missing"] = missing
    record["reason"] = "Not installed: " + ", ".join(missing) + ". " + remedy
    return record


def tail_lines(text: str, limit: int = DIAGNOSTIC_LINE_LIMIT) -> list[str]:
    lines = [line for line in text.splitlines() if line.strip()]
    return lines[:limit]


# ----------------------------------------------------------------- compiler search


def clang_major(host: Host, clang: str) -> int | None:
    result = host.run([clang, "--version"])
    match = re.search(r"clang version (\d+)\.", result.output)
    return int(match.group(1)) if result.returncode == 0 and match else None


def find_pinned_clang(host: Host, explicit: str | None) -> tuple[str | None, int | None]:
    """Return the pinned-major upstream clang++ (or None) and the major found for the wrong candidates."""
    pinned = read_pinned_llvm_major()
    candidates = [
        explicit,
        host.environment.get("LUDUS_MOBILE_CLANG"),
        str(ROOT / "out" / "host-tools" / "bin" / "clang++"),
        f"/opt/homebrew/opt/llvm@{pinned}/bin/clang++",
        f"/usr/local/opt/llvm@{pinned}/bin/clang++",
        host.which(f"clang++-{pinned}"),
    ]
    wrong_major = None
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            major = clang_major(host, candidate)
            if major == pinned:
                return candidate, None
            wrong_major = major if major is not None else wrong_major
    return None, wrong_major


@dataclass
class Step:
    """One compile or link command whose outcome goes into a record."""

    name: str
    command: list[str]
    tokens: dict[str, str]


def describe_command(command: list[str], tokens: dict[str, str]) -> str:
    """Render a command with known roots replaced by names, so it is reproducible and path-free."""
    text = shlex.join(str(part) for part in command)
    for real, name in sorted(tokens.items(), key=lambda item: len(item[0]), reverse=True):
        text = text.replace(real, name)
    return text


def run_step(host: Host, step: Step, private_values: tuple[str, ...]) -> dict:
    result = host.run(step.command, timeout=COMMAND_TIMEOUT_SECONDS)
    entry = {
        "step": step.name,
        "ok": result.returncode == 0,
        "command": sanitize_text(describe_command(step.command, step.tokens), private_values),
    }
    if result.returncode != 0:
        entry["diagnostics"] = [sanitize_text(line, private_values) for line in tail_lines(result.output)]
    return entry


# --------------------------------------------------------------------- Apple probes

XCODE_REMEDY = (
    "Install Xcode, select it with `xcode-select` and accept its "
    "license; Command Line Tools have no iOS SDK."
)


def parse_xcode_version(text: str) -> tuple[str, str] | None:
    version = re.search(r"Xcode\s+([\d.]+)", text)
    build = re.search(r"Build version\s+(\w+)", text)
    return (version.group(1), build.group(1)) if version and build else None


def query_sdk(host: Host, sdk: str) -> dict | None:
    version = host.run(["xcrun", "--sdk", sdk, "--show-sdk-version"])
    path = host.run(["xcrun", "--sdk", sdk, "--show-sdk-path"])
    build = host.run(["xcrun", "--sdk", sdk, "--show-sdk-build-version"])
    if version.returncode != 0 or path.returncode != 0:
        return None
    return {
        "sdk": sdk,
        "version": version.output.strip(),
        "build": build.output.strip() if build.returncode == 0 else "unknown",
        "_path": path.output.strip(),
    }


def libcxx_header_origin(host: Host, clang: str, arguments: list[str], sdk_path: str) -> str:
    """Name which libc++ header directory the compiler searched first for the probe command line."""
    result = host.run([clang, *arguments, "-x", "c++", "-E", "-v", os.devnull])
    toolchain_root = str(Path(clang).resolve().parent.parent)
    in_list = False
    for line in result.output.splitlines():
        if line.startswith("#include <...> search starts here"):
            in_list = True
        elif line.startswith("End of search list"):
            break
        elif in_list and line.strip().endswith("c++/v1"):
            location = str(Path(line.strip()).resolve()) if Path(line.strip()).exists() else line.strip()
            if location.startswith(sdk_path):
                return "SDK"
            if location.startswith(toolchain_root):
                return "pinned toolchain"
            return "other"
    return "none"


def apple_toolchain(host: Host, options: argparse.Namespace) -> dict:
    record = new_record("apple-toolchain", host)
    record["deploymentTarget"] = options.ios_minimum
    record["deploymentTargetSource"] = "command line" if options.ios_minimum != DEFAULT_IOS_MINIMUM else "provisional default"
    missing = []
    developer = host.run(["xcode-select", "-p"])
    xcodebuild = host.run(["xcodebuild", "-version"])
    xcode = parse_xcode_version(xcodebuild.output) if xcodebuild.returncode == 0 else None
    if developer.returncode != 0 or xcode is None:
        missing.append("Xcode")
    sdks = {}
    if xcode is not None:
        for sdk in ("iphoneos", "iphonesimulator"):
            info = query_sdk(host, sdk)
            if info is None:
                missing.append(f"{sdk} SDK")
            else:
                sdks[sdk] = info
    if missing:
        return not_installed(record, missing, XCODE_REMEDY)
    record["xcode"] = {"version": xcode[0], "build": xcode[1]}
    record["sdks"] = [{key: value for key, value in info.items() if not key.startswith("_")} for info in sdks.values()]

    clang, wrong_major = find_pinned_clang(host, options.clang)
    pinned = read_pinned_llvm_major()
    if clang is None:
        found = f" (found major {wrong_major})" if wrong_major is not None else ""
        return not_installed(
            record,
            [f"upstream Clang {pinned}{found}"],
            "Run ./init.sh, or pass --clang or set LUDUS_MOBILE_CLANG to the pinned clang++.",
        )
    record["clang"] = {"major": pinned, "version": host.run([clang, "--version"]).output.splitlines()[0]}
    linker = host.run(["xcrun", "ld", "-version_details"])
    version_line = next((line for line in (linker.output.splitlines() or [""]) if "PROGRAM" in line or "ld" in line.lower()), "")
    record["linker"] = {"tool": "Xcode ld", "version": sanitize_text(version_line.strip())}

    private_values: tuple[str, ...] = ()
    steps: list[dict] = []
    with tempfile.TemporaryDirectory(prefix="ludus-mobile-probe-") as work_name:
        work = Path(work_name)
        for sdk, info in sdks.items():
            suffix = "-simulator" if sdk == "iphonesimulator" else ""
            triple = f"arm64-apple-ios{options.ios_minimum}{suffix}"
            tokens = {
                info["_path"]: f"<{sdk}-sdk>",
                str(FOUNDATION_BASE_INCLUDE): "<foundation-base-include>",
                str(PROBE_SOURCES): "<probe-sources>",
                str(work): "<work>",
                clang: "<clang>",
            }
            base = ["-target", triple, "-isysroot", info["_path"], f"-I{FOUNDATION_BASE_INCLUDE}", *CXX_FLAGS, *WARNING_FLAGS]
            cxx_object = work / f"{sdk}-cxx23.o"
            objcxx_object = work / f"{sdk}-objcxx.o"
            library = work / f"{sdk}-probe.dylib"
            compile_steps = [
                Step(f"{sdk}: compile C++23", [clang, *base, "-c", str(PROBE_SOURCES / "probe_cxx23.cpp"), "-o", str(cxx_object)], tokens),
                Step(
                    f"{sdk}: compile ARC Objective-C++",
                    [clang, *base, *OBJCXX_FLAGS, "-DLUDUS_PROBE_REQUIRE_UIKIT=1", "-c", str(PROBE_SOURCES / "probe_objcxx_arc.mm"), "-o", str(objcxx_object)],
                    tokens,
                ),
                Step(
                    f"{sdk}: link dylib with Xcode ld",
                    [clang, "-target", triple, "-isysroot", info["_path"], "-dynamiclib", "-stdlib=libc++",
                     "-framework", "Foundation", "-framework", "UIKit", str(cxx_object), str(objcxx_object), "-o", str(library)],
                    tokens,
                ),
            ]
            for step in compile_steps:
                outcome = run_step(host, step, private_values)
                steps.append(outcome)
                if outcome["ok"] is False:
                    break
            else:
                record.setdefault("libcxx", {})[sdk] = {
                    "headersFrom": libcxx_header_origin(host, clang, base, info["_path"]),
                    "runtime": "libc++ from the SDK",
                }
                dependencies = host.run(["otool", "-L", str(library)])
                record["libcxx"][sdk]["linkedLibraries"] = sorted(
                    {match.group(1) for match in re.finditer(r"^\s+(/usr/lib/[^\s]+)", dependencies.output, re.MULTILINE)}
                )
    record["steps"] = steps
    if any(step["ok"] is False for step in steps):
        record["status"] = STATUS_FAILED
        record["reason"] = "A probe compile or link failed with option A; see the failing step and record it before choosing option B."
    return record


PROFILE_DIRECTORIES = (
    Path("Library/Developer/Xcode/UserData/Provisioning Profiles"),
    Path("Library/MobileDevice/Provisioning Profiles"),
)


def decode_profile(host: Host, path: Path) -> dict | None:
    result = host.run(["security", "cms", "-D", "-i", str(path)])
    if result.returncode != 0:
        return None
    try:
        return plistlib.loads(result.output.encode("utf-8"))
    except Exception:  # a malformed profile (expat, plist or value errors) is simply not usable
        return None


def count_development_profiles(host: Host, now: datetime.datetime) -> tuple[int, bool]:
    """Count unexpired development profiles, and whether one of them has a wildcard application ID."""
    valid = 0
    has_wildcard = False
    for directory in PROFILE_DIRECTORIES:
        for path in sorted((host.home / directory).glob("*.mobileprovision")):
            profile = decode_profile(host, path)
            if profile is None:
                continue
            entitlements = profile.get("Entitlements", {})
            expires = profile.get("ExpirationDate")
            is_development = "ProvisionedDevices" in profile and entitlements.get("get-task-allow") is True
            is_current = expires is None or expires.replace(tzinfo=None) > now
            if is_development and is_current:
                valid += 1
                identifier = str(entitlements.get("application-identifier", ""))
                has_wildcard = has_wildcard or identifier.endswith(".*")
    return valid, has_wildcard


def apple_signing(host: Host, options: argparse.Namespace) -> dict:
    record = new_record("apple-signing", host)
    xcodebuild = host.run(["xcodebuild", "-version"])
    if xcodebuild.returncode != 0 or parse_xcode_version(xcodebuild.output) is None:
        return not_installed(record, ["Xcode"], XCODE_REMEDY)
    identities = host.run(["security", "find-identity", "-v", "-p", "codesigning"])
    has_identity = "Apple Development" in identities.output or "iPhone Developer" in identities.output
    has_team = False
    if has_identity:
        certificate = host.run(["security", "find-certificate", "-c", "Apple Development", "-p"])
        if certificate.returncode == 0:
            subject = host.run(["openssl", "x509", "-noout", "-subject"], input_text=certificate.output)
            has_team = subject.returncode == 0 and re.search(r"OU\s*=\s*[A-Z0-9]{10}", subject.output) is not None
    now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    profiles, has_wildcard = count_development_profiles(host, now)
    record["hasDevelopmentIdentity"] = has_identity
    record["hasDevelopmentTeam"] = has_team
    record["validDevelopmentProfileCount"] = profiles
    if has_identity and has_team and has_wildcard:
        record["canSignProbeApp"] = "yes"
    elif has_identity and has_team:
        record["canSignProbeApp"] = "unknown"
        record["note"] = "No wildcard development profile; Xcode automatic signing may create one when the M1 probe app first builds."
    else:
        record["canSignProbeApp"] = "no"
    if record["canSignProbeApp"] == "no":
        record["status"] = STATUS_BLOCKED
        record["reason"] = "No Apple development signing identity and team; create one in Xcode (Settings > Accounts) and keep it out of the repository."
    return record


# ------------------------------------------------------------------- Android probes

ANDROID_REMEDY = "Run ./init.sh --cli --android-only, or select Install Android SDK and NDK in ./init.sh."


def android_sdk_root(host: Host) -> Path | None:
    return resolve_sdk(host.root, host.home, host.environment)


def natural_key(name: str) -> list:
    return [int(part) if part.isdigit() else part for part in re.split(r"(\d+)", name)]


def newest_child(directory: Path) -> Path | None:
    if not directory.is_dir():
        return None
    children = sorted((child for child in directory.iterdir() if child.is_dir()), key=lambda child: natural_key(child.name))
    return children[-1] if children else None


def read_revision(directory: Path) -> str | None:
    properties = directory / "source.properties"
    if not properties.is_file():
        return None
    match = re.search(r"^Pkg\.Revision\s*=\s*(\S+)", properties.read_text(), re.MULTILINE)
    return match.group(1) if match else None


def find_ndk(host: Host, sdk: Path | None, explicit: str | None) -> Path | None:
    for value in (explicit, host.environment.get("ANDROID_NDK_HOME"), host.environment.get("ANDROID_NDK_ROOT")):
        if value and (Path(value) / "source.properties").is_file():
            return Path(value)
    if sdk is not None:
        if sdk == managed_sdk(host.root):
            ndk = sdk / "ndk" / read_android_pins(ROOT)["ndk"]
            return ndk if (ndk / "source.properties").is_file() else None
        newest = newest_child(sdk / "ndk")
        if newest is not None and (newest / "source.properties").is_file():
            return newest
    return None


def find_adb(host: Host, sdk: Path | None) -> str | None:
    if sdk is not None and (sdk / "platform-tools" / "adb").is_file():
        return str(sdk / "platform-tools" / "adb")
    return host.which("adb")


def ndk_prebuilt(ndk: Path) -> Path | None:
    for name in ("darwin-x86_64", "linux-x86_64"):
        candidate = ndk / "toolchains" / "llvm" / "prebuilt" / name
        if candidate.is_dir():
            return candidate
    return None


def parse_load_alignments(readelf_output: str) -> list[int]:
    """Return the alignment of each LOAD program header from ``llvm-readelf -l`` output."""
    alignments = []
    for line in readelf_output.splitlines():
        fields = line.split()
        if fields and fields[0] == "LOAD":
            try:
                alignments.append(int(fields[-1], 16))
            except ValueError:
                continue
    return alignments


def check_page_alignment(host: Host, readelf: str, library: Path) -> dict:
    result = host.run([readelf, "-l", str(library)])
    alignments = parse_load_alignments(result.output)
    return {
        "loadSegmentAlignments": [hex(value) for value in alignments],
        "isSixteenKilobyteAligned": bool(alignments) and all(value >= PAGE_SIZE_16_KIB for value in alignments),
    }


def android_java_tool(host: Host, name: str) -> str | None:
    java_home = host.environment.get("JAVA_HOME")
    if java_home:
        candidate = Path(java_home) / "bin" / name
        if candidate.is_file():
            return str(candidate)
    local_java = managed_java_home(host.root, read_android_pins(ROOT)["jdk"], platform.system()) / "bin" / name
    if local_java.is_file():
        return str(local_java)
    return host.which(name)


def java_version(host: Host) -> str | None:
    java = android_java_tool(host, "java")
    if java is None:
        return None
    result = host.run([java, "-version"])
    match = re.search(r'version "([^"]+)"', result.output)
    return match.group(1) if result.returncode == 0 and match else None


def gradle_version(host: Host) -> str | None:
    result = host.run(["gradle", "--version"])
    match = re.search(r"^Gradle\s+(\S+)", result.output, re.MULTILINE)
    return match.group(1) if result.returncode == 0 and match else None


def android_toolchain(host: Host, options: argparse.Namespace) -> dict:
    record = new_record("android-toolchain", host)
    record["apiLevel"] = options.android_api
    record["apiLevelSource"] = "command line" if options.android_api != DEFAULT_ANDROID_API else "provisional default"
    sdk = android_sdk_root(host)
    ndk = find_ndk(host, sdk, options.ndk)
    adb = find_adb(host, sdk)
    missing = []
    if sdk is None:
        missing.append("Android SDK")
    if ndk is None:
        missing.append("Android NDK")
    if adb is None:
        missing.append("adb")
    if missing:
        return not_installed(record, missing, ANDROID_REMEDY)
    assert sdk is not None and ndk is not None
    prebuilt = ndk_prebuilt(ndk)
    if prebuilt is None:
        return not_installed(record, ["NDK LLVM toolchain for this host"], ANDROID_REMEDY)

    command_line_tools = newest_child(sdk / "cmdline-tools")
    platform_directory = newest_child(sdk / "platforms")
    build_tools = newest_child(sdk / "build-tools")
    record["components"] = {
        "commandLineTools": read_revision(command_line_tools) if command_line_tools else None,
        "platform": platform_directory.name if platform_directory else None,
        "buildTools": build_tools.name if build_tools else None,
        "ndk": read_revision(ndk),
        "jdk": java_version(host),
        "gradle": gradle_version(host),
        "androidGradlePlugin": "not detectable without a project; pinned in config/android_toolchain.json at M1",
    }

    pinned = read_pinned_llvm_major()
    clang, wrong_major = find_pinned_clang(host, options.clang)
    if clang is None:
        found = f" (found major {wrong_major})" if wrong_major is not None else ""
        return not_installed(record, [f"upstream Clang {pinned}{found}"], "Run ./init.sh, or pass --clang or set LUDUS_MOBILE_CLANG.")
    record["clang"] = {"major": pinned, "version": host.run([clang, "--version"]).output.splitlines()[0]}

    triple = f"aarch64-linux-android{options.android_api}"
    sysroot = prebuilt / "sysroot"
    ndk_clang = prebuilt / "bin" / f"{triple}-clang++"
    ndk_lld = prebuilt / "bin" / "ld.lld"
    pinned_lld = Path(clang).parent / "ld.lld"
    if not pinned_lld.is_file():
        shim = ROOT / "out/host-tools/bin/ld.lld"
        if shim.is_file():
            pinned_lld = shim
    readelf = prebuilt / "bin" / "llvm-readelf"
    record["linker"] = {
        "optionA": "pinned LLD" if pinned_lld.exists() else "NDK-bundled LLD (pinned LLD not found beside the pinned Clang)",
        "version": (host.run([str(pinned_lld if pinned_lld.exists() else ndk_lld), "--version"]).output.strip().splitlines() or ["unknown"])[0],
    }
    builtins = host.run([str(ndk_clang), "--rtlib=compiler-rt", "--print-libgcc-file-name"]).output.strip()
    libcxx_archive = sysroot / "usr" / "lib" / "aarch64-linux-android" / "libc++_static.a"
    page_flags = ["-Wl,-z,max-page-size=16384", "-Wl,-z,common-page-size=16384"]
    jni_include = [f"-I{FOUNDATION_BASE_INCLUDE}"]
    steps: list[dict] = []
    alignment: dict = {}
    private_values: tuple[str, ...] = ()
    with tempfile.TemporaryDirectory(prefix="ludus-mobile-probe-") as work_name:
        work = Path(work_name)
        tokens = {
            str(sysroot): "<ndk-sysroot>",
            str(prebuilt): "<ndk-llvm>",
            str(FOUNDATION_BASE_INCLUDE): "<foundation-base-include>",
            str(PROBE_SOURCES): "<probe-sources>",
            str(work): "<work>",
            clang: "<clang>",
        }
        sources = [PROBE_SOURCES / "probe_cxx23.cpp", PROBE_SOURCES / "probe_jni.cpp"]
        variants = {
            "option A (pinned Clang, NDK sysroot and libc++ headers)": (
                [clang, f"--target={triple}", f"--sysroot={sysroot}", "-nostdinc++", "-isystem", str(sysroot / "usr/include/c++/v1")],
                [clang, f"--target={triple}", f"--sysroot={sysroot}", "-shared", "-nodefaultlibs",
                 f"--ld-path={pinned_lld if pinned_lld.exists() else ndk_lld}", *page_flags],
                [str(libcxx_archive), builtins, "-lc", "-lm", "-ldl", "-llog"],
            ),
            "NDK Clang (comparison)": (
                [str(ndk_clang)],
                [str(ndk_clang), "-shared", "-static-libstdc++", *page_flags],
                [],
            ),
        }
        for variant_index, (label, (compiler, linker, libraries)) in enumerate(variants.items()):
            stem = f"variant{variant_index}"
            objects = []
            failed = False
            for source in sources:
                object_path = work / f"{stem}-{source.stem}.o"
                step = Step(
                    f"{label}: compile {source.name}",
                    [*compiler, *jni_include, *CXX_FLAGS, *WARNING_FLAGS, "-fPIC", "-c", str(source), "-o", str(object_path)],
                    tokens,
                )
                outcome = run_step(host, step, private_values)
                steps.append(outcome)
                objects.append(str(object_path))
                if outcome["ok"] is False:
                    failed = True
                    break
            if failed:
                continue
            library = work / f"{stem}-libludus_probe.so"
            outcome = run_step(host, Step(f"{label}: link shared library", [*linker, *objects, *libraries, "-o", str(library)], tokens), private_values)
            steps.append(outcome)
            if outcome["ok"]:
                reader = str(readelf) if readelf.exists() else (host.which("llvm-readelf") or "llvm-readelf")
                alignment[label] = check_page_alignment(host, reader, library)
    record["steps"] = steps
    record["pageAlignment"] = alignment
    option_a_failed = any(step["ok"] is False and step["step"].startswith("option A") for step in steps)
    misaligned = [name for name, entry in alignment.items() if entry["isSixteenKilobyteAligned"] is False]
    if option_a_failed or misaligned or not alignment:
        record["status"] = STATUS_FAILED
        record["reason"] = "Option A failed a compile or link, or a library has LOAD segments below 16 KiB alignment; record it before choosing option B."
    return record


def android_signing(host: Host, options: argparse.Namespace) -> dict:
    record = new_record("android-signing", host)
    sdk = android_sdk_root(host)
    keytool = android_java_tool(host, "keytool")
    missing = []
    if sdk is None:
        missing.append("Android SDK")
    if keytool is None:
        missing.append("keytool (JDK)")
    if missing:
        return not_installed(record, missing, ANDROID_REMEDY)
    user_directory = host.environment.get("ANDROID_USER_HOME")
    android_home = Path(user_directory) if user_directory else host.home / ".android"
    keystore = android_home / "debug.keystore"
    record["hasDebugKeystore"] = False
    if keystore.is_file():
        listing = host.run([keytool, "-list", "-keystore", str(keystore), "-storepass", "android"])
        record["hasDebugKeystore"] = listing.returncode == 0
    if record["hasDebugKeystore"] is False:
        record["status"] = STATUS_BLOCKED
        record["reason"] = "No usable debug keystore; Android Studio or the first Gradle debug build creates one. Release signing belongs to M6."
    return record


def parse_adb_devices(text: str) -> list[str]:
    """Return the serials of devices in the ``device`` state; they are private values."""
    serials = []
    for line in text.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "device":
            serials.append(fields[0])
    return serials


def decode_vulkan_version(api_version: int) -> str:
    version = api_version & 0x1FFFFFFF
    return f"{(version >> 22) & 0x7F}.{(version >> 12) & 0x3FF}.{version & 0xFFF}"


def parse_vkjson(text: str) -> dict | None:
    start = text.find("{")
    if start < 0:
        return None
    try:
        document = json.loads(text[start:])
        device = document["devices"][0]
        device_extensions = {entry.get("extensionName") for entry in device.get("extensions", [])}
        instance_entries = document.get("extensions", document.get("instance", {}).get("extensions", []))
        instance_extensions = {entry.get("extensionName") for entry in instance_entries}
        return {
            "apiVersion": decode_vulkan_version(int(device["properties"]["apiVersion"])),
            "gpu": str(device["properties"].get("deviceName", "unknown")),
            "hasAndroidSurface": "VK_KHR_android_surface" in instance_extensions,
            "hasSwapchainMaintenance1": "VK_EXT_swapchain_maintenance1" in device_extensions,
        }
    except (ValueError, KeyError, IndexError, TypeError):
        return None


def android_devices(host: Host, adb: str) -> tuple[list[dict], tuple[str, ...]]:
    listing = host.run([adb, "devices"])
    serials = parse_adb_devices(listing.output)
    devices = []
    for index, serial in enumerate(serials, start=1):
        def getprop(name: str) -> str:
            return host.run([adb, "-s", serial, "shell", "getprop", name]).output.strip()

        page_size = host.run([adb, "-s", serial, "shell", "getconf", "PAGE_SIZE"]).output.strip()
        vulkan = parse_vkjson(host.run([adb, "-s", serial, "shell", "cmd", "gpu", "vkjson"]).output)
        devices.append(
            {
                "label": f"android-{index}",
                "kind": "emulator" if serial.startswith("emulator-") else "physical",
                "manufacturer": getprop("ro.product.manufacturer"),
                "marketingName": getprop("ro.product.marketname") or None,
                "model": getprop("ro.product.model"),
                "osVersion": getprop("ro.build.version.release"),
                "apiLevel": getprop("ro.build.version.sdk"),
                "build": getprop("ro.build.display.id"),
                "abi": getprop("ro.product.cpu.abi"),
                "pageSizeBytes": int(page_size) if page_size.isdigit() else None,
                "vulkan": vulkan or {"source": "unavailable here; read it from the M1 probe app"},
            }
        )
    return devices, tuple(serials)


def apple_devices(host: Host) -> tuple[list[dict], tuple[str, ...]]:
    with tempfile.TemporaryDirectory(prefix="ludus-mobile-probe-") as work:
        output = Path(work) / "devices.json"
        result = host.run(["xcrun", "devicectl", "list", "devices", "--json-output", str(output)])
        if result.returncode != 0 or not output.is_file():
            return [], ()
        document = json.loads(output.read_text())
    devices = []
    private: list[str] = []
    for index, entry in enumerate(document.get("result", {}).get("devices", []), start=1):
        hardware = entry.get("hardwareProperties", {})
        properties = entry.get("deviceProperties", {})
        private.extend(
            str(value) for value in (entry.get("identifier"), hardware.get("udid"), hardware.get("serialNumber"), properties.get("name")) if value
        )
        devices.append(
            {
                "label": f"apple-{index}",
                "marketingName": hardware.get("marketingName"),
                "model": hardware.get("productType"),
                "platform": hardware.get("platform"),
                "osVersion": properties.get("osVersionNumber"),
                "build": properties.get("osBuildUpdate"),
                "pageSizeBytes": None,
                "pageSizeSource": "on-device probe in M1",
            }
        )
    return devices, tuple(private)


def devices_probe(host: Host, options: argparse.Namespace) -> dict:
    record = new_record("devices", host)
    sdk = android_sdk_root(host)
    adb = find_adb(host, sdk)
    xcodebuild = host.run(["xcodebuild", "-version"])
    has_xcode = xcodebuild.returncode == 0 and parse_xcode_version(xcodebuild.output) is not None
    missing = []
    if has_xcode is False:
        missing.append("Xcode (devicectl)")
    if adb is None:
        missing.append("adb")
    if missing:
        record["missing"] = missing
    devices: list[dict] = []
    private: tuple[str, ...] = ()
    if has_xcode:
        found, secrets = apple_devices(host)
        devices += found
        private += secrets
    if adb is not None:
        found, secrets = android_devices(host, adb)
        devices += found
        private += secrets
    record["devices"] = devices
    record["_private"] = list(private)
    if len(missing) == 2:
        return not_installed(record, missing, XCODE_REMEDY + " " + ANDROID_REMEDY)
    if not devices:
        record["status"] = STATUS_BLOCKED
        record["reason"] = "No device is connected and trusted: " + ("missing " + ", ".join(missing) + "; " if missing else "") + "connect the iPhone, iPad and Galaxy by cable."
    elif missing:
        record["status"] = STATUS_BLOCKED
        record["reason"] = "Only part of the inventory ran; missing " + ", ".join(missing) + "."
    return record


# ------------------------------------------------------------------------- driver

RUNNERS = {
    "apple-toolchain": apple_toolchain,
    "apple-signing": apple_signing,
    "android-toolchain": android_toolchain,
    "android-signing": android_signing,
    "devices": devices_probe,
}


def run_probe(name: str, host: Host, options: argparse.Namespace) -> dict:
    """Run one probe and return its sanitized record (raises LeakError rather than return a leaky one)."""
    record = RUNNERS[name](host, options)
    private = tuple(record.pop("_private", []))
    return finalize_record(record, private)


def write_record(record: dict, directory: Path) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{record['probe']}.json"
    path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    return path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("probe", choices=(*PROBES, "all"), help="probe to run, or all of them")
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIRECTORY, help="where records are written (default: out/mobile-probes)")
    parser.add_argument("--clang", help="pinned upstream clang++ (default: out/host-tools, Homebrew llvm@<pin>, PATH)")
    parser.add_argument("--ndk", help="Android NDK directory (default: ANDROID_NDK_HOME or the newest under the SDK)")
    parser.add_argument("--ios-minimum", default=DEFAULT_IOS_MINIMUM, help="iOS deployment target to compile for (provisional)")
    parser.add_argument("--android-api", type=int, default=DEFAULT_ANDROID_API, help="Android API level to compile for (provisional)")
    return parser


def main(argv: list[str], host: Host | None = None) -> int:
    options = build_parser().parse_args(argv)
    host = host or Host()
    names = PROBES if options.probe == "all" else (options.probe,)
    worst = STATUS_PASSED
    order = (STATUS_PASSED, STATUS_BLOCKED, STATUS_NOT_INSTALLED, STATUS_FAILED)
    for name in names:
        try:
            record = run_probe(name, host, options)
        except LeakError as error:
            print(f"{name}: refused to write a record: {error}", file=sys.stderr)
            return EXIT_USAGE
        path = write_record(record, options.output_dir)
        print(f"{name}: {record['status']}" + (f" - {record['reason']}" if "reason" in record else ""))
        print(f"  record: {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path.name}")
        if order.index(record["status"]) > order.index(worst):
            worst = record["status"]
    return EXIT_CODES[worst]


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
