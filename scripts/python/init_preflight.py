"""Fast checks that run before ./init.sh installs anything.

Setup can take many minutes, so problems that are cheap to detect are reported
in the first second instead of halfway through: too little free disk space,
no network for the downloads setup still needs, missing Xcode Command Line
Tools, an unsupported operating system and an unsuitable Python. Every check
returns a ``CheckResult``; a failed check stops setup with an actionable
message, a warning is printed and setup continues.

Probes (disk usage, sockets, commands, the operating system) are injected so
each check is unit tested without touching the machine. Set
``LUDUS_INIT_SKIP_PREFLIGHT=1`` to skip the checks, for example on an
unsupported host you know works.
"""
from __future__ import annotations

import importlib.util
import os
import platform
import shutil
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping, Sequence
from urllib.parse import urlsplit


SKIP_ENVIRONMENT = "LUDUS_INIT_SKIP_PREFLIGHT"
PASSED = "passed"
WARNING = "warning"
FAILED = "failed"

BYTES_PER_GIGABYTE = 1024 ** 3
# A first editor setup (Conan packages, Qt, two build trees) needs roughly this
# much; below it setup stops before downloading anything.
MINIMUM_FREE_GIGABYTES = 15
# Enough for a first setup, but rebuilding every preset may run out.
RECOMMENDED_FREE_GIGABYTES = 30

# Each remote gets one TCP connection attempt; all run at once, so the whole
# network check takes at most this long.
NETWORK_TIMEOUT_SECONDS = 1.0
HTTPS_PORT = 443
# Hosts the downloads of a first setup come from: pip (CMake, Ninja, Conan),
# ConanCenter and the pinned tool archives on GitHub.
REQUIRED_REMOTES: tuple[tuple[str, str], ...] = (
    ("Python packages", "pypi.org"),
    ("Python package files", "files.pythonhosted.org"),
    ("ConanCenter", "center2.conan.io"),
    ("GitHub", "github.com"),
)
PROXY_ENVIRONMENT_NAMES = ("HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy")
DEFAULT_PROXY_PORT = 8080

MINIMUM_UBUNTU_RELEASE = "24.04"
MINIMUM_MACOS_RELEASE = "14.0"
OS_RELEASE_PATH = Path("/etc/os-release")
XCODE_SELECT_TIMEOUT_SECONDS = 5

DISK_LABEL = "Free disk space"
NETWORK_LABEL = "Network"
OPERATING_SYSTEM_LABEL = "Operating system"
XCODE_LABEL = "Xcode Command Line Tools"
PYTHON_LABEL = "Python"


@dataclass(frozen=True)
class CheckResult:
    """The outcome of one preflight check, with the message to show."""

    label: str
    status: str
    message: str

    @property
    def is_failure(self) -> bool:
        return self.status == FAILED


def is_skipped(environment: Mapping[str, str] | None = None) -> bool:
    environment = os.environ if environment is None else environment
    return environment.get(SKIP_ENVIRONMENT, "").strip().lower() not in ("", "0", "false", "no", "off")


def parse_version(text: str) -> tuple[int, ...]:
    """``"24.04"`` -> ``(24, 4)``; non-numeric parts end the version."""
    parts: list[int] = []
    for part in text.strip().split("."):
        if not part.isdigit():
            break
        parts.append(int(part))
    return tuple(parts)


# -- disk ----------------------------------------------------------------------

def existing_ancestor(path: Path) -> Path:
    """``path`` or its nearest existing parent, so a fresh checkout without out/ can be measured."""
    for candidate in (path, *path.parents):
        if candidate.exists():
            return candidate
    return path


def check_disk_space(root: Path, *, disk_usage: Callable | None = None) -> CheckResult:
    disk_usage = disk_usage or shutil.disk_usage
    target = root / "out"
    try:
        free = disk_usage(str(existing_ancestor(target))).free
    except OSError as error:
        return CheckResult(DISK_LABEL, WARNING, f"could not measure free space for {target}: {error}")
    free_gigabytes = free / BYTES_PER_GIGABYTE
    summary = f"{free_gigabytes:.1f} GB free for {target}"
    if free_gigabytes < MINIMUM_FREE_GIGABYTES:
        return CheckResult(DISK_LABEL, FAILED,
                           f"only {summary}; setup needs at least {MINIMUM_FREE_GIGABYTES} GB. Free some space "
                           "(old build trees under out/ and the Conan cache in ~/.conan2 are the largest), then run "
                           "./init.sh again.")
    if free_gigabytes < RECOMMENDED_FREE_GIGABYTES:
        return CheckResult(DISK_LABEL, WARNING,
                           f"{summary}; {RECOMMENDED_FREE_GIGABYTES} GB is recommended, so a full rebuild may run out.")
    return CheckResult(DISK_LABEL, PASSED, summary)


# -- network -------------------------------------------------------------------

def proxy_address(environment: Mapping[str, str]) -> tuple[str, int] | None:
    """The configured HTTPS proxy's host and port; setup's downloads go through it."""
    for name in PROXY_ENVIRONMENT_NAMES:
        value = environment.get(name, "").strip()
        if not value:
            continue
        parts = urlsplit(value if "://" in value else "http://" + value)
        if parts.hostname:
            try:
                port = parts.port
            except ValueError:
                port = None
            return parts.hostname, port or DEFAULT_PROXY_PORT
    return None


def can_connect(host: str, port: int, timeout: float) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def unreachable_hosts(addresses: Sequence[tuple[str, int]], *, connect: Callable | None = None,
                      timeout: float = NETWORK_TIMEOUT_SECONDS, clock: Callable[[], float] = time.monotonic
                      ) -> list[str]:
    """Try every address at once; return those not reached within ``timeout``.

    Daemon threads bound the wait even when name resolution, which has no
    timeout of its own, hangs; an unfinished attempt counts as unreachable.
    """
    connect = connect or can_connect
    reached: dict[str, bool] = {}
    lock = threading.Lock()

    def attempt(host: str, port: int) -> None:
        is_reached = connect(host, port, timeout)
        with lock:
            reached[host] = is_reached

    threads = [threading.Thread(target=attempt, args=address, daemon=True, name=f"ludus-preflight-{address[0]}")
               for address in addresses]
    for thread in threads:
        thread.start()
    deadline = clock() + timeout
    for thread in threads:
        thread.join(max(0.0, deadline - clock()))
    with lock:
        return [host for host, _port in addresses if reached.get(host) is not True]


def check_network(*, has_cached_setup: bool, environment: Mapping[str, str] | None = None,
                  connect: Callable | None = None, timeout: float = NETWORK_TIMEOUT_SECONDS) -> CheckResult:
    environment = os.environ if environment is None else environment
    proxy = proxy_address(environment)
    if proxy is not None:
        addresses = [proxy]
        names = {proxy[0]: f"proxy {proxy[0]}:{proxy[1]}"}
    else:
        addresses = [(host, HTTPS_PORT) for _label, host in REQUIRED_REMOTES]
        names = {host: f"{label} ({host})" for label, host in REQUIRED_REMOTES}
    missing = unreachable_hosts(addresses, connect=connect, timeout=timeout)
    if not missing:
        reached = "the HTTPS proxy" if proxy is not None else "all download servers"
        return CheckResult(NETWORK_LABEL, PASSED, f"reached {reached}")
    listing = ", ".join(names[host] for host in missing)
    if has_cached_setup:
        return CheckResult(NETWORK_LABEL, WARNING,
                           f"could not reach {listing}. Continuing with what an earlier setup downloaded; a step "
                           "that needs a new download will fail until the network is back.")
    return CheckResult(NETWORK_LABEL, FAILED,
                       f"could not reach {listing}, and this checkout has nothing downloaded yet. Check your "
                       "internet connection, VPN or proxy (set HTTPS_PROXY if you use one), then run ./init.sh again.")


def has_cached_setup(root: Path, environment: Mapping[str, str] | None = None) -> bool:
    """Whether an earlier setup left the managed tools and Conan packages that a rerun can reuse offline."""
    import init_cache
    if init_cache.is_forced(environment):
        return False
    return (root / "out/host-tools/venv").is_dir() and init_cache.stamp_path(root, "conan").is_file()


# -- operating system ------------------------------------------------------------

def parse_os_release(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in text.splitlines():
        name, separator, value = line.partition("=")
        if separator and name.strip():
            values[name.strip()] = value.strip().strip('"').strip("'")
    return values


def check_operating_system(*, system: str, os_release_text: str = "", macos_version: str = "") -> CheckResult:
    if system == "Darwin":
        version = parse_version(macos_version)
        if not version:
            return CheckResult(OPERATING_SYSTEM_LABEL, WARNING, "could not read the macOS version")
        name = f"macOS {macos_version}"
        if version < parse_version(MINIMUM_MACOS_RELEASE):
            return CheckResult(OPERATING_SYSTEM_LABEL, FAILED,
                               f"{name} is not supported; Ludus needs macOS {MINIMUM_MACOS_RELEASE} or newer. "
                               "Update macOS, then run ./init.sh again.")
        return CheckResult(OPERATING_SYSTEM_LABEL, PASSED, name)
    if system == "Linux":
        release = parse_os_release(os_release_text)
        name = release.get("PRETTY_NAME") or release.get("NAME") or "Linux"
        if release.get("ID") != "ubuntu":
            return CheckResult(OPERATING_SYSTEM_LABEL, WARNING,
                               f"{name} is not tested; Ludus supports Ubuntu {MINIMUM_UBUNTU_RELEASE} or "
                               "newer. Automatic package installation uses apt and may not work here; "
                               "--no-system-install skips it.")
        version = parse_version(release.get("VERSION_ID", ""))
        if version and version < parse_version(MINIMUM_UBUNTU_RELEASE):
            return CheckResult(OPERATING_SYSTEM_LABEL, FAILED,
                               f"{name} is too old; Ludus needs Ubuntu {MINIMUM_UBUNTU_RELEASE} or "
                               "newer for its compiler packages. Upgrade Ubuntu (or use the .devcontainer), then run "
                               "./init.sh again.")
        return CheckResult(OPERATING_SYSTEM_LABEL, PASSED, name)
    return CheckResult(OPERATING_SYSTEM_LABEL, WARNING, f"{system or 'this system'} is not a tested native host")


def read_os_release(path: Path = OS_RELEASE_PATH) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


# -- Xcode Command Line Tools ----------------------------------------------------

def check_xcode_command_line_tools(*, run: Callable = subprocess.run) -> CheckResult:
    """``xcode-select -p`` prints the active developer directory when the tools are installed."""
    try:
        completed = run(["xcode-select", "-p"], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True, check=False, timeout=XCODE_SELECT_TIMEOUT_SECONDS)
    except (OSError, subprocess.TimeoutExpired) as error:
        return CheckResult(XCODE_LABEL, FAILED, f"xcode-select could not run ({error}). Install the tools with "
                           "`xcode-select --install`, then run ./init.sh again.")
    developer_directory = (completed.stdout or "").strip()
    if completed.returncode != 0 or not developer_directory or not Path(developer_directory).is_dir():
        return CheckResult(XCODE_LABEL, FAILED,
                           "not installed. Run `xcode-select --install`, finish the installer, then run ./init.sh "
                           "again. After a macOS upgrade, reinstalling them fixes `invalid active developer path`.")
    return CheckResult(XCODE_LABEL, PASSED, developer_directory)


# -- Python ------------------------------------------------------------------------

def check_python(minimum: str, *, version: Sequence[int] | None = None, find_spec: Callable = importlib.util.find_spec,
                 system: str, will_install_system_packages: bool, executable: str = sys.executable) -> CheckResult:
    version = tuple(sys.version_info[:3]) if version is None else tuple(version)
    actual = ".".join(map(str, version))
    if version < parse_version(minimum):
        return CheckResult(PYTHON_LABEL, FAILED,
                           f"Python {actual} ({executable}) is older than the required {minimum}. Install a newer "
                           "Python (python3 from apt, Homebrew or python.org) and put it first on PATH, then run "
                           "./init.sh again.")
    if find_spec("venv") is None or find_spec("ensurepip") is None:
        if system == "Linux" and will_install_system_packages:
            return CheckResult(PYTHON_LABEL, PASSED,
                               f"Python {actual}; virtual-environment support (python3-venv) will be installed")
        return CheckResult(PYTHON_LABEL, FAILED,
                           f"Python {actual} lacks virtual-environment support. On Ubuntu run "
                           "`sudo apt-get install -y python3-venv`, then run ./init.sh again.")
    return CheckResult(PYTHON_LABEL, PASSED, f"Python {actual}")


# -- all checks -------------------------------------------------------------------------

def run_checks(root: Path, *, minimum_python: str, will_install_system_packages: bool, is_ci: bool = False,
               system: str | None = None, environment: Mapping[str, str] | None = None) -> list[CheckResult]:
    """Run every check for this host in a fixed order; network attempts run in parallel.

    CI runners have small but sufficient disks and their own network policy, so
    there disk and network problems are warnings; the build itself still fails
    if space or a download really runs out.
    """
    system = system or platform.system()
    environment = os.environ if environment is None else environment
    results = [
        check_operating_system(system=system, os_release_text=read_os_release() if system == "Linux" else "",
                               macos_version=platform.mac_ver()[0] if system == "Darwin" else ""),
    ]
    if system == "Darwin":
        results.append(check_xcode_command_line_tools())
    results.append(check_python(minimum_python, system=system,
                                will_install_system_packages=will_install_system_packages))
    environmental = [check_disk_space(root),
                     check_network(has_cached_setup=has_cached_setup(root, environment), environment=environment)]
    if is_ci:
        environmental = [as_warning(result) for result in environmental]
    return results + environmental


def as_warning(result: CheckResult) -> CheckResult:
    return CheckResult(result.label, WARNING, result.message) if result.is_failure else result


def failure_message(results: Sequence[CheckResult]) -> str:
    failed = [result for result in results if result.is_failure]
    noun = "check" if len(failed) == 1 else "checks"
    details = " ".join(f"{result.label}: {result.message}" for result in failed)
    return f"{len(failed)} preflight {noun} failed before anything was installed. {details} " \
           f"(Set {SKIP_ENVIRONMENT}=1 to skip these checks.)"
