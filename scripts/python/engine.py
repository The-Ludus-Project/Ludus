#!/usr/bin/env python3
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import difflib
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import tempfile
import urllib.request
import venv
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence

from formatting import FormattingError, format_source
from analysis_inventory import compilation_entries, inventory, request_codemodel, SOURCE_ROOTS
from quality_process import ANALYSIS_SECONDS, LANE_BYTES, LOG_BYTES, LogBudget, run_bounded


NATIVE_PRESET_PREFIX = "macos-clang" if platform.system() == "Darwin" else "linux-clang"
DEFAULT_PRESET = NATIVE_PRESET_PREFIX + "-development"


def native_profile_name() -> str:
    if platform.system() == "Darwin":
        arch = platform.machine().lower()
        if arch not in ("arm64", "x86_64"):
            raise EngineError(f"Unsupported macOS architecture: {arch}")
        return f"macos-clang-{arch}"
    return "linux-clang-x86_64"


PROJECT_CXX_STANDARD = "23"
BOOTSTRAP_STATE_VERSION = 2

# ClangBuildAnalyzer aggregates Clang -ftime-trace outputs into a single ranked
# report. Pinned so the profiling harness is reproducible. It is fetched/built
# on demand by `profile-build` only, never by init.sh.
# Thanks to Aras Pranckevičius and contributors, v1.6.0 updates simdjson for
# newer compilers and accounts for Clang 19+ header trace events.
# https://github.com/aras-p/ClangBuildAnalyzer/releases/tag/v1.6.0
CLANG_BUILD_ANALYZER_VERSION = "1.6.0"
CLANG_BUILD_ANALYZER_REPO = "https://github.com/aras-p/ClangBuildAnalyzer"

PRESET_BUILD_TYPES: dict[str, str] = {
    "linux-clang-debug": "Debug",
    "linux-clang-development": "RelWithDebInfo",
    "linux-clang-asan-ubsan": "RelWithDebInfo",
    "linux-clang-profile": "RelWithDebInfo",
    "linux-clang-release": "Release",
}

PRESET_BUILD_TYPES.update({
    name.replace("linux-clang", "macos-clang"): value
    for name, value in list(PRESET_BUILD_TYPES.items())
})
HOST_PRESETS = tuple(name for name in PRESET_BUILD_TYPES if name.startswith(NATIVE_PRESET_PREFIX))

FORMAT_SUFFIXES = {".m", ".mm", ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
TIDY_ROOTS = SOURCE_ROOTS

BOOTSTRAP_INPUTS = (
    "config/tool_versions.json",
    "config/conan/profiles/linux-clang-x86_64",
    "config/conan/profiles/macos-clang-arm64",
    "config/conan/profiles/macos-clang-x86_64",
    "conanfile.py",
    "conan.lock",
    "third_party/catch2/conanfile.py",
    "tests/support/prepare_catch_headers.py",
    "tests/support/catch_stream.hpp",
    "tests/support/catch_stream.cpp",
    "CMakePresets.json",
)


class EngineError(RuntimeError):
    """Exception raised for engine-related errors."""


@dataclass(frozen=True)
class ToolStatus:
    """Represents the status of a development tool."""
    name: str
    required: bool
    found: bool
    version: str
    path: str
    requirement: str
    ok: bool


@dataclass(frozen=True)
class AptPackageGroup:
    """Represents one prerequisite that can be satisfied by one of several apt packages."""
    purpose: str
    candidates: tuple[str, ...]
    # Optional groups are installed when a candidate is available but do not fail
    # onboarding when none is found. Used for prerequisites the build can do
    # without (e.g. Wayland, which the platform module falls back from).
    optional: bool = False


def find_repo_root(start: Path) -> Path:
    """Find the repository root by searching for tool_versions.json in the config directory."""
    current = start.resolve()
    for candidate in (current, *current.parents):
        if (candidate / "config" / "tool_versions.json").is_file():
            return candidate
    raise EngineError("could not locate repository root from " + str(start))


def repo_root() -> Path:
    """Get the repository root from the current module's location."""
    return find_repo_root(Path(__file__).resolve())


def load_tool_versions(root: Path) -> dict[str, dict[str, str]]:
    """Load tool versions configuration from tool_versions.json."""
    versions_path = root / "config" / "tool_versions.json"
    try:
        data = json.loads(versions_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise EngineError(f"failed to read {versions_path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise EngineError(f"failed to parse {versions_path}: {exc}") from exc

    if not isinstance(data.get("managed"), dict) or not isinstance(data.get("minimum"), dict):
        raise EngineError(f"{versions_path} must contain 'managed' and 'minimum' objects")
    return data


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def bootstrap_fingerprint(root: Path) -> dict[str, object]:
    files: dict[str, str] = {}
    for relative_path in BOOTSTRAP_INPUTS:
        path = root / relative_path
        files[relative_path] = file_sha256(path) if path.is_file() else "missing"
    result = {
        "version": BOOTSTRAP_STATE_VERSION,
        "cxx_standard": PROJECT_CXX_STANDARD,
        "files": files,
    }
    if platform.system() == "Darwin":
        result["macos_toolchain"] = macos_toolchain_identity(root)
    return result


def bootstrap_marker_path(root: Path, preset: str) -> Path:
    return root / "out" / "conan" / preset / ".ludus-bootstrap.json"


def preset_bootstrap_fingerprint(root: Path, preset: str) -> dict[str, object]:
    from init_options import read_options
    result = bootstrap_fingerprint(root)
    result["build_tests"] = read_options(root, preset).get("LUDUS_BUILD_TESTS", True)
    return result


def write_bootstrap_marker(root: Path, preset: str) -> None:
    marker = bootstrap_marker_path(root, preset)
    marker.write_text(
        json.dumps(preset_bootstrap_fingerprint(root, preset), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def bootstrap_marker_is_current(root: Path, preset: str) -> bool:
    marker = bootstrap_marker_path(root, preset)
    if not marker.is_file():
        return False
    try:
        actual = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return actual == preset_bootstrap_fingerprint(root, preset)


def version_tuple(version: str) -> tuple[int, ...]:
    """Extract version numbers from a version string."""
    match = re.search(r"(\d+(?:\.\d+)*)", version)
    if match is None:
        return ()
    return tuple(int(part) for part in match.group(1).split("."))


def version_at_least(actual: str, minimum: str) -> bool:
    """Check if actual version is at least the minimum version."""
    actual_parts = version_tuple(actual)
    minimum_parts = version_tuple(minimum)
    if not actual_parts:
        return False

    width = max(len(actual_parts), len(minimum_parts))
    return actual_parts + (0,) * (width - len(actual_parts)) >= minimum_parts + (0,) * (width - len(minimum_parts))


def command_line(argv: Sequence[object]) -> str:
    """Format command arguments as a quoted command line string."""
    return " ".join(shlex_quote(str(arg)) for arg in argv)


def shlex_quote(value: str) -> str:
    """Quote a string for safe use in shell commands."""
    if re.fullmatch(r"[A-Za-z0-9_@%+=:,./-]+", value):
        return value
    return "'" + value.replace("'", "'\"'\"'") + "'"


def run(
    argv: Sequence[object],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    """Run a command and return the result."""
    print(f"+ {command_line(argv)}", flush=True)
    try:
        return subprocess.run(
            [str(arg) for arg in argv],
            cwd=str(cwd),
            env=env,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
        )
    except FileNotFoundError as exc:
        raise EngineError(f"command not found while running: {command_line(argv)}") from exc
    except subprocess.CalledProcessError as exc:
        if exc.stdout:
            print(exc.stdout, end="")
        raise EngineError(f"command failed with exit code {exc.returncode}: {command_line(argv)}") from exc


def capture_command(argv: Sequence[object], *, cwd: Path, env: dict[str, str] | None = None) -> str:
    """Run a command and return its output."""
    completed = run(argv, cwd=cwd, env=env, capture=True)
    return completed.stdout.strip()


def capture_command_quiet(
    argv: Sequence[object],
    *,
    cwd: Path,
    env: dict[str, str] | None = None,
    input_text: str | None = None,
    timeout: int | None = None,
) -> tuple[int, str]:
    """Run a command quietly and return exit code and output."""
    try:
        completed = subprocess.run(
            [str(arg) for arg in argv],
            cwd=str(cwd),
            env=env,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            input=input_text,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return 127, str(exc)
    return completed.returncode, completed.stdout.strip()


def venv_dir(root: Path) -> Path:
    """Return the virtual environment directory path."""
    return root / "out" / "host-tools" / "venv"


def venv_bin_dir(root: Path) -> Path:
    """Return the virtual environment bin directory path."""
    return venv_dir(root) / ("Scripts" if os.name == "nt" else "bin")


def host_tools_bin_dir(root: Path) -> Path:
    return root / "out" / "host-tools" / "bin"


def venv_python(root: Path) -> Path:
    """Return the path to the Python executable in the virtual environment."""
    return venv_bin_dir(root) / ("python.exe" if os.name == "nt" else "python")


def managed_executable(root: Path, name: str) -> Path:
    """Return the path to a managed executable in the virtual environment."""
    suffix = ".exe" if os.name == "nt" else ""
    return venv_bin_dir(root) / f"{name}{suffix}"


# Opt-in Conan home shared by every checkout of this repository on the host
# (for example concurrent agent worktrees), so third-party packages are built
# once instead of once per checkout. See docs/development/building.md.
SHARED_CONAN_HOME_VARIABLE = "LUDUS_CONAN_HOME"


def shared_conan_root() -> Path | None:
    value = os.environ.get(SHARED_CONAN_HOME_VARIABLE, "")
    if not value:
        return None
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise EngineError(f"{SHARED_CONAN_HOME_VARIABLE} must be an absolute path, not {value!r}")
    if os.name == "nt":
        raise EngineError(f"{SHARED_CONAN_HOME_VARIABLE} is supported on macOS and Linux hosts only")
    return path


def conan_home(root: Path) -> Path:
    shared = shared_conan_root()
    if shared is None:
        return root / "out" / "conan" / "home"
    # One home per pinned Conan version: checkouts on different pins must not
    # migrate the same cache database back and forth.
    return shared / ("conan-" + load_tool_versions(root)["managed"]["conan"])


@contextlib.contextmanager
def conan_cache_lock(root: Path) -> Iterator[None]:
    """Serialize Conan cache writes when the Conan home is shared.

    Conan 2 does not lock its package cache between processes, so exports,
    lock creation and --build=missing installs from concurrent checkouts must
    not overlap. A checkout-local home needs no lock."""
    if shared_conan_root() is None:
        yield
        return
    import fcntl

    home = conan_home(root)
    home.mkdir(parents=True, exist_ok=True)
    with (home / ".ludus-cache.lock").open("a") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            print(f"Waiting for another checkout to finish with the shared Conan cache: {home}", flush=True)
            fcntl.flock(handle, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(handle, fcntl.LOCK_UN)


def tool_env(root: Path) -> dict[str, str]:
    env = os.environ.copy()
    path_entries = [host_tools_bin_dir(root), venv_bin_dir(root)]
    env["PATH"] = os.pathsep.join(str(path) for path in path_entries) + os.pathsep + env.get("PATH", "")
    env["CONAN_HOME"] = str(conan_home(root))
    env["CONAN_NON_INTERACTIVE"] = "1"
    return env


def managed_package_version(root: Path, package_name: str) -> str:
    python = venv_python(root)
    if not python.exists():
        return ""

    script = (
        "from importlib import metadata; "
        f"print(metadata.version({package_name!r}))"
    )
    return_code, output = capture_command_quiet([python, "-c", script], cwd=root)
    if return_code != 0:
        return ""
    return output


def venv_has_pip(root: Path) -> bool:
    python = venv_python(root)
    if not python.exists():
        return False
    completed = subprocess.run(
        [str(python), "-m", "pip", "--version"],
        cwd=str(root),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
        check=False,
    )
    return completed.returncode == 0


def system_python_has_venv_support() -> bool:
    try:
        import ensurepip  # noqa: F401
    except ModuleNotFoundError:
        return False
    return True


def check_current_python(_root: Path, minimum: str) -> None:
    actual = platform.python_version()
    if not version_at_least(actual, minimum):
        print_missing_tool_help("Python", actual, sys.executable, minimum)
        raise EngineError(f"Python {minimum} or newer is required")
    print(f"Python {actual} satisfies minimum {minimum}: {sys.executable}")


def create_or_update_venv(root: Path) -> None:
    directory = venv_dir(root)
    directory.parent.mkdir(parents=True, exist_ok=True)
    if directory.is_dir() and venv_has_pip(root):
        print(f"Project-managed virtual environment is already usable: {directory}")
        return

    print(f"Creating or updating project-managed virtual environment: {directory}")
    try:
        import ensurepip  # noqa: F401
    except ModuleNotFoundError as exc:
        print("")
        print("Missing required Python virtual-environment support.")
        print(f"Detected host: {platform.platform()}")
        print(f"Detected Python: {platform.python_version()} at {sys.executable}")
        print("Ubuntu install command:")
        print("  sudo apt-get update && sudo apt-get install -y python3-venv")
        print("Rerun after installing prerequisites:")
        print("  ./init.sh")
        raise EngineError("python3-venv or ensurepip support is required") from exc

    try:
        venv.EnvBuilder(with_pip=True, clear=False, upgrade=False).create(directory)
    except Exception as exc:
        print("")
        print("Failed to create the project-managed virtual environment.")
        print(f"Detected host: {platform.platform()}")
        print(f"Target environment: {directory}")
        print("Ubuntu install command:")
        print("  sudo apt-get update && sudo apt-get install -y python3-venv")
        print("Rerun after installing prerequisites:")
        print("  ./init.sh")
        raise EngineError("virtual environment creation failed") from exc


def install_managed_tools(root: Path, versions: dict[str, dict[str, str]]) -> None:
    managed = versions["managed"]
    packages = [
        f"cmake=={managed['cmake']}",
        f"ninja=={managed['ninja']}",
        f"conan=={managed['conan']}",
    ]
    print("Network access: installing pinned Python packages for CMake, Ninja, and Conan.")
    run(
        [
            venv_python(root),
            "-m",
            "pip",
            "install",
            "--disable-pip-version-check",
            "--quiet",
            "--upgrade",
            *packages,
        ],
        cwd=root,
    )


def find_executable(candidates: Sequence[str]) -> str:
    for candidate in candidates:
        found = shutil.which(candidate)
        if found:
            return found
    return ""


def find_satisfying_executable(
    root: Path,
    candidates: Sequence[str],
    version_args: Sequence[str],
    minimum_version: str,
) -> str:
    for candidate in candidates:
        path = str(candidate)
        if not Path(path).is_file():
            found = shutil.which(path)
            if not found:
                continue
            path = found

        version = executable_version(path, version_args, root)
        if version and version_at_least(version, minimum_version):
            return path
    return ""


def link_or_replace_symlink(link_path: Path, target_path: Path) -> None:
    link_path.parent.mkdir(parents=True, exist_ok=True)
    if link_path.is_symlink():
        if link_path.resolve() == target_path.resolve():
            return
        link_path.unlink()
    elif link_path.exists():
        raise EngineError(f"cannot create tool shim because a non-symlink already exists: {link_path}")
    link_path.symlink_to(target_path)


def homebrew_llvm_formulae(llvm_major: int) -> tuple[tuple[str, str], ...]:
    """Return the Homebrew formulae providing the pinned LLVM and the executable that identifies each.

    Homebrew ships LLD as a separate formula for the pinned LLVM, so both are required.
    """
    return ((f"llvm@{llvm_major}", "clang"), (f"lld@{llvm_major}", "ld.lld"))


def check_homebrew_llvm_major(root: Path, formula: str, tool: Path, llvm_major: int) -> None:
    """Reject a Homebrew ``formula`` whose ``tool`` reports an LLVM major other than ``llvm_major``.

    Until Homebrew publishes a versioned formula for the newest LLVM major,
    ``llvm@<major>`` and ``lld@<major>`` are aliases of the unversioned formulae,
    so ``brew upgrade`` can silently move them to the next major. A missing
    ``tool`` is left to the ordinary missing-tool diagnostics.
    """
    if not tool.is_file():
        return
    parts = version_tuple(executable_version(str(tool), ("--version",), root))
    if parts and parts[0] != llvm_major:
        raise EngineError(
            f"Homebrew {formula} resolves to {tool.parent.parent} with LLVM {'.'.join(map(str, parts))}, "
            f"but Ludus pins LLVM {llvm_major}.\n"
            f"Homebrew aliases {formula} to its unversioned formula until a newer LLVM major ships, so "
            "`brew upgrade` can move it. Install the versioned formulae once they exist "
            f"(`brew install llvm@{llvm_major} lld@{llvm_major}`) and rerun ./init.sh. While the unversioned "
            f"formulae still provide LLVM {llvm_major}, `brew pin llvm lld` prevents the move.")


def configure_system_tool_shims(root: Path, versions: dict[str, dict[str, str]]) -> None:
    if os.name == "nt":
        return

    minimum = versions["minimum"]
    clang_major = llvm_major_version(versions)
    if platform.system() == "Darwin":
        brew = shutil.which("brew")
        if brew:
            for formula, tool in homebrew_llvm_formulae(clang_major):
                code, prefix = capture_command_quiet([brew, "--prefix", formula], cwd=root)
                if code == 0:
                    bin_dir = Path(prefix.strip()) / "bin"
                    check_homebrew_llvm_major(root, formula, bin_dir / tool, clang_major)
                    os.environ["PATH"] = str(bin_dir) + os.pathsep + os.environ.get("PATH", "")
    specs: list[tuple[str, Sequence[str], str]] = [
        ("clang", (f"clang-{clang_major}", "clang"), minimum["clang"]),
        ("clang++", (f"clang++-{clang_major}", "clang++"), minimum["clang"]),
        ("ld.lld", (f"ld.lld-{clang_major}", "ld.lld"), minimum["lld"]),
        ("clang-format", (f"clang-format-{clang_major}", "clang-format"), minimum["clang-format"]),
        ("clang-tidy", (f"clang-tidy-{clang_major}", "clang-tidy"), minimum["clang-tidy"]),
        ("clangd", (f"clangd-{clang_major}", "clangd"), minimum["clang"]),
    ]

    for shim_name, candidates, minimum_version in specs:
        target = find_satisfying_executable(root, candidates, ("--version",), minimum_version)
        if target:
            link_or_replace_symlink(host_tools_bin_dir(root) / shim_name, Path(target))

    if platform.system() == "Darwin" and clang_cxx(root).is_file():
        configure_macos_sdk(root, clang_major)


def macos_toolchain_identity(root: Path) -> dict[str, str]:
    """Record real toolchain inputs so SDK/header retargeting invalidates setup."""
    paths = {
        "compiler": clang_cxx(root),
        "sdk": root / "out/host-tools/macos-sdk",
        "libcxx": root / "out/host-tools/libcxx-include",
    }
    result = {name: str(path.resolve()) for name, path in paths.items()}
    for name, path in (("sdk_settings", paths["sdk"] / "SDKSettings.json"),
                       ("libcxx_config", paths["libcxx"] / "__config")):
        result[name] = file_sha256(path) if path.is_file() else "missing"
    return result


def macos_sdk_candidates(preferred: Path) -> list[Path]:
    """Prefer Clang's SDK, then test installed sibling SDKs newest first."""
    siblings = list(preferred.parent.glob("MacOSX*.sdk"))
    def sdk_version(path: Path) -> tuple[int, ...]:
        try:
            return version_tuple(str(json.loads((path / "SDKSettings.json").read_text())["Version"]))
        except (OSError, KeyError, ValueError, TypeError):
            return ()
    ordered = [preferred, *sorted(siblings, key=sdk_version, reverse=True)]
    result = []
    for path in ordered:
        resolved = path.resolve()
        if resolved.is_dir() and resolved not in result:
            result.append(resolved)
    return result


def probe_macos_sdk(root: Path, sdk: Path, libcxx: Path) -> tuple[int, str]:
    # Compile and link rather than checking a version label: Apple SDK math
    # headers can require compiler-resource macros absent from older Clang.
    # This exercises the NAN/INFINITY contract that HarfBuzz's source uses.
    # <cfloat> after <cmath> catches libc++ 22 with the macOS 27 SDK (C++23): math.h's
    # partial float.h include sets libc++'s guard, leaving <cfloat> empty
    # (Catch2 needs DBL_MAX_10_EXP). Thanks to Ian Anderson (LLVM), "[libc++]
    # Remove libc++'s <float.h>", llvm/llvm-project#175849 (LLVM 23):
    # https://github.com/llvm/llvm-project/pull/175849
    for metadata in (sdk / "SDKSettings.json", libcxx / "__config"):
        if not metadata.is_file():
            return 1, f"Incomplete SDK/header installation: missing {metadata}"
    source = ("#include <cmath>\n#include <cfloat>\n#include <string_view>\n"
              "#ifndef NAN\n#error LudusSDK_missing_NAN\n#endif\n"
              "#ifndef INFINITY\n#error LudusSDK_missing_INFINITY\n#endif\n"
              "#ifndef DBL_MAX_10_EXP\n#error LudusSDK_missing_DBL_MAX_10_EXP\n#endif\n"
              "int main() { volatile double n = NAN; volatile double i = INFINITY; "
              "return std::isnan(n) && std::isinf(i) && std::string_view(\"sdk\").size() == 3 ? 0 : 1; }\n")
    import tempfile
    with tempfile.TemporaryDirectory(prefix="ludus-sdk-probe-") as temporary:
        return capture_command_quiet(
            [clang_cxx(root), "-x", "c++", "-std=c++23", "-stdlib=libc++",
             "-nostdinc++", "-isystem", libcxx, "-isysroot", sdk,
             "-mmacosx-version-min=14.0", "-", "-o", Path(temporary) / "probe"],
            cwd=root, input_text=source, timeout=30)


def configure_macos_sdk(root: Path, llvm_major: int) -> None:
    """Select and link a macOS SDK that the pinned Clang ``llvm_major`` can compile and link with."""
    explicit = os.environ.get("SDKROOT", "")
    sdk = explicit
    if not sdk:
        code, output = capture_command_quiet(
            [clang_cxx(root), "-###", "-x", "c++", "-c", os.devnull], cwd=root)
        match = re.search(r'"-isysroot" "([^"\n]+)"', output)
        if code != 0 or not match:
            raise EngineError("Clang could not select a macOS SDK. Install Xcode Command Line Tools, "
                              "or set SDKROOT to an installed compatible SDK and rerun ./init.sh.")
        sdk = match.group(1)
    if not Path(sdk).is_dir():
        raise EngineError(f"macOS SDK is missing: {sdk}. Set SDKROOT to an installed SDK and rerun ./init.sh.")
    libcxx = clang_cxx(root).resolve().parent.parent / "include/c++/v1"
    if not libcxx.is_dir():
        raise EngineError(f"Clang libc++ headers are missing: {libcxx}. "
                          f"Restore llvm@{llvm_major} and rerun ./init.sh.")
    candidates = [Path(sdk).resolve()] if explicit else macos_sdk_candidates(Path(sdk))
    failures = []
    for candidate in candidates:
        code, output = probe_macos_sdk(root, candidate, libcxx)
        if code == 0:
            link_or_replace_symlink(root / "out/host-tools/macos-sdk", candidate)
            link_or_replace_symlink(root / "out/host-tools/libcxx-include", libcxx)
            print(f"Validated macOS SDK: {candidate} (Clang compile/link, NAN/INFINITY and <cfloat> checks passed)")
            if failures:
                print(f"Skipped incompatible SDK: {Path(sdk).resolve()}")
            return
        failures.append(f"  {candidate}:\n{output[-2000:]}")
    selection = "Explicit SDKROOT is incompatible" if explicit else "No installed compatible macOS SDK was found"
    raise EngineError(
        f"{selection} with {clang_cxx(root).resolve()}.\n"
        "Compiler/SDK compile-link validation failed before dependency downloads or HarfBuzz builds.\n"
        + "\n".join(failures) + "\nHow to fix:\n"
        f"  Install a macOS SDK compatible with Clang {llvm_major} "
        "(the current Command Line Tools SDK passes the math-header probe with LLVM 23).\n"
        "  SDKROOT=/absolute/path/to/MacOSX.sdk ./init.sh\n"
        "  If SDKROOT was set accidentally, unset SDKROOT and rerun ./init.sh to test installed sibling SDKs.\n"
        "Changing only the deployment target does not change the SDK headers.")


def native_profile_contents(root: Path) -> str:
    source = root / "config/conan/profiles" / native_profile_name()
    text = source.read_text(encoding="utf-8")
    if platform.system() == "Darwin":
        text += f"tools.apple:sdk_path={root / 'out/host-tools/macos-sdk'}\n"
        text += f"tools.build:cxxflags={['-nostdinc++', '-isystem', str(root / 'out/host-tools/libcxx-include')]!r}\n"
        text = text.replace('tools.build:compiler_executables={"c":"clang","cpp":"clang++"}',
                            f"tools.build:compiler_executables={dict(c=str(host_tools_bin_dir(root) / 'clang'), cpp=str(clang_cxx(root)))!r}")
    return text


def executable_version(path: str, args: Sequence[str], root: Path) -> str:
    if not path:
        return ""
    return_code, output = capture_command_quiet([path, *args], cwd=root)
    if return_code != 0 or not output:
        return ""
    return output.splitlines()[0]


def system_tool_statuses(root: Path, versions: dict[str, dict[str, str]]) -> list[ToolStatus]:
    minimum = versions["minimum"]
    clang_major = llvm_major_version(versions)
    shim_dir = host_tools_bin_dir(root)
    tool_specs: list[tuple[str, bool, Sequence[str], Sequence[str], str]] = [
        ("Git", True, ("git",), ("--version",), ""),
        ("Python", True, (sys.executable,), ("--version",), minimum["python"]),
        ("Clang", True, (str(shim_dir / "clang++"), f"clang++-{clang_major}", "clang++"), ("--version",), minimum["clang"]),
        ("LLD", True, (str(shim_dir / "ld.lld"), f"ld.lld-{clang_major}", "ld.lld"), ("--version",), minimum["lld"]),
        (
            "clang-format",
            True,
            (str(shim_dir / "clang-format"), f"clang-format-{clang_major}", "clang-format"),
            ("--version",),
            minimum["clang-format"],
        ),
        (
            "clang-tidy",
            True,
            (str(shim_dir / "clang-tidy"), f"clang-tidy-{clang_major}", "clang-tidy"),
            ("--version",),
            minimum["clang-tidy"],
        ),
        ("ccache", False, ("ccache",), ("--version",), ""),
        ("LLDB", False, (f"lldb-{clang_major}", "lldb"), ("--version",), ""),
        ("GDB", False, ("gdb",), ("--version",), ""),
        ("rr", False, ("rr",), ("--version",), ""),
        ("Valgrind", False, ("valgrind",), ("--version",), ""),
    ]

    statuses: list[ToolStatus] = []
    for name, required, candidates, version_args, minimum_version in tool_specs:
        path = candidates[0] if Path(candidates[0]).is_file() else find_executable(candidates)
        version = executable_version(path, version_args, root) if path else ""
        if name == "Python":
            version = platform.python_version()
            path = sys.executable
        ok = bool(path)
        if ok and minimum_version:
            ok = version_at_least(version, minimum_version)
        requirement = f">= {minimum_version}" if minimum_version else "present"
        statuses.append(ToolStatus(name, required, bool(path), version, path, requirement, ok))

    return statuses


def managed_tool_statuses(root: Path, versions: dict[str, dict[str, str]]) -> list[ToolStatus]:
    managed = versions["managed"]
    specs = [
        ("Project-managed CMake", "cmake", "cmake"),
        ("Project-managed Ninja", "ninja", "ninja"),
        ("Project-managed Conan", "conan", "conan"),
    ]

    statuses: list[ToolStatus] = []
    for label, executable, package_name in specs:
        path = managed_executable(root, executable)
        expected = managed[package_name]
        package_version = managed_package_version(root, package_name)
        found = path.exists() and bool(package_version)
        statuses.append(
            ToolStatus(
                label,
                True,
                found,
                package_version,
                str(path) if path.exists() else "",
                f"== {expected}",
                found and package_version == expected,
            )
        )
    return statuses


def project_state_statuses(root: Path) -> list[ToolStatus]:
    installed_profile = conan_profile_path(root)
    source_profile = root / "config" / "conan" / "profiles" / native_profile_name()
    lockfile = root / "conan.lock"
    environment = venv_dir(root)
    environment_ok = environment.is_dir() and venv_python(root).exists() and venv_has_pip(root)
    profile_installed = installed_profile.is_file()
    profile_current = (
        profile_installed
        and source_profile.is_file()
        and installed_profile.read_text(encoding="utf-8") == native_profile_contents(root)
    )
    return [
        ToolStatus(
            "Conan profile",
            True,
            profile_installed or source_profile.is_file(),
            "installed/current" if profile_current else "installed/stale" if profile_installed else "source" if source_profile.is_file() else "",
            str(installed_profile if installed_profile.is_file() else source_profile if source_profile.is_file() else ""),
            "installed and current",
            profile_current,
        ),
        ToolStatus(
            "Conan lockfile",
            True,
            lockfile.is_file(),
            "present" if lockfile.is_file() else "",
            str(lockfile) if lockfile.is_file() else "",
            "present",
            lockfile.is_file(),
        ),
        ToolStatus(
            "Project-managed virtual environment",
            True,
            environment.is_dir(),
            "pip available" if environment_ok else "missing pip" if environment.is_dir() else "",
            str(environment) if environment.is_dir() else "",
            "present",
            environment_ok,
        ),
    ]


def print_status_table(statuses: Sequence[ToolStatus]) -> None:
    rows = [
        (
            status.name,
            "found" if status.found else "missing",
            status.version or "-",
            status.path or "-",
            status.requirement,
            "yes" if status.ok else "no",
        )
        for status in statuses
    ]
    headers = ("Item", "Status", "Version", "Path", "Requirement", "Satisfies")
    widths = [len(header) for header in headers]
    for row in rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))

    print(" | ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
    print("-+-".join("-" * width for width in widths))
    for row in rows:
        print(" | ".join(value.ljust(widths[index]) for index, value in enumerate(row)))


def print_missing_tool_help(name: str, version: str, path: str, minimum: str,
                            llvm_major: int | None = None) -> None:
    print("")
    print(f"Missing or invalid required tool: {name}")
    print(f"Detected host: {platform.platform()}")
    print(f"Detected version: {version or 'not found'}")
    print(f"Detected path: {path or 'not found'}")
    print(f"Minimum required version: {minimum}")
    print("One-command onboarding command:")
    print("  ./init.sh")
    if name != "Python" and llvm_major is not None and platform.system() == "Darwin":
        formulae = " ".join(formula for formula, _ in homebrew_llvm_formulae(llvm_major))
        print("Manual Homebrew install or upgrade (refresh first: stale formula metadata can install an older patch):")
        print(f"  brew update && brew install {formulae} && brew upgrade {formulae}")
    elif name != "Python" and llvm_major is not None:
        print("Manual Ubuntu fallback, if automatic sudo installation is unavailable:")
        print(
            "  sudo apt-get update && sudo apt-get install -y "
            f"git python3 python3-venv ca-certificates clang-{llvm_major} lld-{llvm_major} "
            f"clang-format-{llvm_major} clang-tidy-{llvm_major}"
        )
        print("  If apt cannot find those packages, register the pinned apt.llvm.org suite first:")
        print("  python3 scripts/python/llvm_apt_source.py && sudo apt-get update")


def llvm_major_version(versions: dict[str, dict[str, str]]) -> int:
    """Return the pinned LLVM major from ``minimum.clang`` in config/tool_versions.json."""
    parts = version_tuple(versions["minimum"].get("clang", ""))
    if not parts:
        raise EngineError("config/tool_versions.json must pin minimum.clang as a version")
    return parts[0]


def pinned_llvm_major(root: Path) -> int:
    """Read the pinned LLVM major for the checkout at ``root``."""
    return llvm_major_version(load_tool_versions(root))


def pinned_llvm_tool(root: Path, tool: str) -> str:
    """Return the versioned executable name of a pinned LLVM tool, such as ``clang-format-<major>``."""
    return f"{tool}-{pinned_llvm_major(root)}"


def host_is_ubuntu() -> bool:
    os_release = read_os_release()
    return os_release.get("ID", "").lower() == "ubuntu"


def python_venv_package_candidates() -> tuple[str, ...]:
    return (f"python{sys.version_info.major}.{sys.version_info.minor}-venv", "python3-venv")


# Package groups served by apt.llvm.org when the Ubuntu archive lacks the pinned LLVM.
LLVM_PACKAGE_GROUPS = frozenset(("Clang", "LLD", "clang-format", "clang-tidy"))


def required_ubuntu_package_groups(root: Path, versions: dict[str, dict[str, str]]) -> list[AptPackageGroup]:
    major = llvm_major_version(versions)
    groups = [
        AptPackageGroup("CA certificates for HTTPS package downloads", ("ca-certificates",)),
        # Wayland is the Linux windowing backend for modules/platform. The dev
        # headers, the xdg-shell protocol XML, and the wayland-scanner code
        # generator are build-time prerequisites; without them the platform
        # module cannot be configured. If Wayland is unavailable the platform
        # module still builds (it falls back to a headless window backend), so
        # these are best-effort: missing candidates are tolerated below.
        AptPackageGroup("Wayland client development headers", ("libwayland-dev",), optional=True),
        AptPackageGroup("Wayland protocol definitions", ("wayland-protocols",), optional=True),
        AptPackageGroup("Wayland protocol code generator", ("wayland-scanner", "libwayland-bin"), optional=True),
    ]

    for status in system_tool_statuses(root, versions):
        if not status.required or status.ok:
            continue
        if status.name == "Git":
            groups.append(AptPackageGroup("Git", ("git",)))
        elif status.name == "Clang":
            groups.append(AptPackageGroup("Clang", (f"clang-{major}",)))
        elif status.name == "LLD":
            groups.append(AptPackageGroup("LLD", (f"lld-{major}",)))
        elif status.name == "clang-format":
            groups.append(AptPackageGroup("clang-format", (f"clang-format-{major}",)))
        elif status.name == "clang-tidy":
            groups.append(AptPackageGroup("clang-tidy", (f"clang-tidy-{major}",)))

    if not system_python_has_venv_support() and not venv_has_pip(root):
        groups.append(AptPackageGroup("Python virtual-environment support", python_venv_package_candidates()))

    return groups


def apt_package_has_candidate(root: Path, package: str) -> bool:
    return_code, output = capture_command_quiet(["apt-cache", "policy", package], cwd=root)
    if return_code != 0:
        return False
    for line in output.splitlines():
        stripped = line.strip()
        if stripped.startswith("Candidate:"):
            return stripped.partition(":")[2].strip() != "(none)"
    return False


def resolve_apt_package_groups(root: Path, groups: Sequence[AptPackageGroup]) -> tuple[list[str], list[AptPackageGroup]]:
    """Resolve each group to an installable package name.

    Returns the selected package names and the list of REQUIRED groups that could
    not be satisfied. Optional groups that cannot be satisfied are skipped
    silently (with a note) rather than reported as missing, so a host without
    those packages still completes onboarding.
    """
    selected: list[str] = []
    missing: list[AptPackageGroup] = []
    for group in groups:
        package = next((candidate for candidate in group.candidates if apt_package_has_candidate(root, candidate)), "")
        if package:
            selected.append(package)
        elif group.optional:
            print(f"Optional prerequisite not available, skipping: {group.purpose} ({' or '.join(group.candidates)})")
        else:
            missing.append(group)
    return selected, missing


def print_missing_apt_packages(root: Path, groups: Sequence[AptPackageGroup]) -> None:
    print("")
    print("Apt could not find installable packages for required prerequisites.")
    print(f"Detected host: {platform.platform()}")
    print("Missing package groups:")
    for group in groups:
        print(f"  {group.purpose}: {' or '.join(group.candidates)}")
        for candidate in group.candidates:
            return_code, output = capture_command_quiet(["apt-cache", "policy", candidate], cwd=root)
            if return_code == 0 and output:
                compact = "; ".join(line.strip() for line in output.splitlines() if line.strip())
                print(f"    apt-cache policy {candidate}: {compact}")
    if host_is_ubuntu():
        print("The standard Ubuntu 'universe' component may be disabled or unavailable from this mirror.")


def ubuntu_sources_file() -> Path:
    return Path("/etc/apt/sources.list.d/ubuntu.sources")


def ubuntu_stanza_has_component(stanza: str, component: str) -> bool:
    if re.search(r"(?im)^Enabled:\s*no\s*$", stanza):
        return False
    if not re.search(r"(?im)^Types:\s*.*\bdeb\b", stanza):
        return False
    if not re.search(r"(?im)^URIs:\s*.*ubuntu", stanza):
        return False
    components_match = re.search(r"(?im)^Components:\s*(.+)$", stanza)
    if not components_match:
        return False
    return component in components_match.group(1).split()


def ubuntu_apt_component_enabled(component: str) -> bool:
    source_path = ubuntu_sources_file()
    if source_path.is_file():
        text = source_path.read_text(encoding="utf-8")
        if any(ubuntu_stanza_has_component(stanza, component) for stanza in re.split(r"\n\s*\n", text)):
            return True

    list_path = Path("/etc/apt/sources.list")
    if list_path.is_file():
        for raw_line in list_path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#") or not line.startswith("deb "):
                continue
            if "ubuntu" in line and component in line.split():
                return True
    return False


def add_component_to_ubuntu_sources_text(text: str, component: str) -> tuple[str, bool]:
    stanzas = re.split(r"(\n\s*\n)", text)
    changed = False
    updated_parts: list[str] = []

    for part in stanzas:
        if not part.strip():
            updated_parts.append(part)
            continue
        if re.search(r"(?im)^Enabled:\s*no\s*$", part):
            updated_parts.append(part)
            continue
        if not re.search(r"(?im)^Types:\s*.*\bdeb\b", part) or not re.search(r"(?im)^URIs:\s*.*ubuntu", part):
            updated_parts.append(part)
            continue

        def replace_components(match: re.Match[str]) -> str:
            nonlocal changed
            components = match.group(1).split()
            if component in components:
                return match.group(0)
            changed = True
            return "Components: " + " ".join([*components, component])

        updated_parts.append(re.sub(r"(?im)^Components:\s*(.+)$", replace_components, part))

    return "".join(updated_parts), changed


def patch_ubuntu_sources_component(root: Path, prefix: Sequence[str], component: str) -> bool:
    source_path = ubuntu_sources_file()
    if not source_path.is_file():
        return False

    original = source_path.read_text(encoding="utf-8")
    updated, changed = add_component_to_ubuntu_sources_text(original, component)
    if not changed:
        return False

    staging_path = root / "out" / "generated" / "apt" / source_path.name
    staging_path.parent.mkdir(parents=True, exist_ok=True)
    staging_path.write_text(updated, encoding="utf-8")

    backup_path = source_path.with_name(source_path.name + ".ludus-backup")
    print(f"Patching Ubuntu apt source to enable '{component}': {source_path}")
    print(f"Original source backup, if not already present: {backup_path}")
    run([*prefix, "cp", "-n", source_path, backup_path], cwd=root)
    run([*prefix, "install", "-m", "0644", staging_path, source_path], cwd=root)
    return True


def enable_ubuntu_universe(root: Path, prefix: Sequence[str]) -> None:
    if not host_is_ubuntu():
        return
    if ubuntu_apt_component_enabled("universe"):
        return

    print("Ubuntu package component 'universe' is needed for LLVM/Python development packages.")
    if not shutil.which("add-apt-repository"):
        run([*prefix, "apt-get", "install", "-y", "software-properties-common"], cwd=root)

    repository_tool = shutil.which("add-apt-repository") or "add-apt-repository"
    run([*prefix, repository_tool, "-y", "universe"], cwd=root)
    if not ubuntu_apt_component_enabled("universe"):
        if not patch_ubuntu_sources_component(root, prefix, "universe"):
            raise EngineError("could not enable Ubuntu universe package component")


def read_os_release() -> dict[str, str]:
    path = Path("/etc/os-release")
    if not path.is_file():
        return {}

    values: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in raw_line:
            continue
        key, value = raw_line.split("=", 1)
        values[key] = value.strip().strip('"')
    return values


def host_supports_apt_install() -> bool:
    if platform.system() != "Linux":
        return False
    os_release = read_os_release()
    distro_markers = " ".join([os_release.get("ID", ""), os_release.get("ID_LIKE", "")]).lower()
    return bool({"ubuntu", "debian"} & set(distro_markers.split())) and shutil.which("apt-get") is not None


def sudo_command_prefix() -> list[str]:
    if hasattr(os, "geteuid") and os.geteuid() == 0:
        return []
    sudo = shutil.which("sudo")
    if not sudo:
        raise EngineError("automatic system package installation needs sudo or a root shell")
    return [sudo]


def system_prerequisites_need_install(root: Path, versions: dict[str, dict[str, str]]) -> bool:
    required_tools_ok = all(status.ok for status in system_tool_statuses(root, versions) if status.required)
    venv_support_ok = system_python_has_venv_support() or venv_has_pip(root)
    return not required_tools_ok or not venv_support_ok


def install_ubuntu_system_prerequisites(root: Path, versions: dict[str, dict[str, str]]) -> None:
    configure_system_tool_shims(root, versions)
    if not system_prerequisites_need_install(root, versions):
        print("Ubuntu system prerequisites already satisfy Milestone 0.")
        return

    if not host_supports_apt_install():
        raise EngineError("automatic system prerequisite installation currently supports apt-based Ubuntu/Debian hosts")

    prefix = sudo_command_prefix()
    print("Installing Ubuntu system prerequisites. This may ask for your sudo password once.")
    package_groups = required_ubuntu_package_groups(root, versions)
    run([*prefix, "apt-get", "update"], cwd=root)

    packages, missing_groups = resolve_apt_package_groups(root, package_groups)
    if missing_groups and host_is_ubuntu():
        enable_ubuntu_universe(root, prefix)
        run([*prefix, "apt-get", "update"], cwd=root)
        packages, missing_groups = resolve_apt_package_groups(root, package_groups)
    if any(group.purpose in LLVM_PACKAGE_GROUPS for group in missing_groups) and host_is_ubuntu():
        # The Ubuntu archive does not package the pinned LLVM major.
        import llvm_apt_source
        try:
            llvm_apt_source.register(root, llvm_apt_source.os_release_codename(),
                                     llvm_major_version(versions), list(prefix))
        except llvm_apt_source.SourceError as error:
            raise EngineError(str(error)) from error
        run([*prefix, "apt-get", "update"], cwd=root)
        packages, missing_groups = resolve_apt_package_groups(root, package_groups)

    if missing_groups:
        print_missing_apt_packages(root, missing_groups)
        raise EngineError("apt prerequisites are unavailable from the configured package sources")

    print("Packages: " + " ".join(packages))
    run([*prefix, "apt-get", "install", "-y", *packages], cwd=root)
    configure_system_tool_shims(root, versions)


def validate_required_system_tools(root: Path, versions: dict[str, dict[str, str]]) -> None:
    statuses = [status for status in system_tool_statuses(root, versions) if status.required]
    failures = [status for status in statuses if not status.ok]
    if not failures:
        return

    print_status_table(statuses)
    for failure in failures:
        minimum = versions["minimum"].get(failure.name.lower(), failure.requirement.removeprefix(">= "))
        print_missing_tool_help(failure.name, failure.version, failure.path, minimum,
                                llvm_major_version(versions))
    raise EngineError("required system-level tools are missing or below the supported version")


def validate_managed_tools(root: Path, versions: dict[str, dict[str, str]]) -> None:
    failures = [status for status in managed_tool_statuses(root, versions) if not status.ok]
    if failures:
        print_status_table(managed_tool_statuses(root, versions))
        raise EngineError("project-managed tools are missing or do not match pinned versions")


def conan_profile_path(root: Path) -> Path:
    # The profile names this checkout's compiler, SDK and libc++ links, so it
    # stays beside the checkout even when the Conan home is shared.
    return root / "out" / "conan" / "profiles" / native_profile_name()


def install_conan_profile(root: Path) -> Path:
    destination = conan_profile_path(root)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(native_profile_contents(root), encoding="utf-8")
    print(f"Installed Conan profile: {destination}")
    return destination


def conan(root: Path) -> Path:
    return managed_executable(root, "conan")


def cmake(root: Path) -> Path:
    return managed_executable(root, "cmake")


def ninja(root: Path) -> Path:
    return managed_executable(root, "ninja")


def clang_cxx(root: Path) -> Path:
    return host_tools_bin_dir(root) / ("clang++.exe" if os.name == "nt" else "clang++")


# Local Conan recipes for pinned sources that ConanCenter does not publish at
# the exact version the engine requires. These are exported into the project
# Conan cache before the lockfile is resolved so the graph can see them. See
# third_party/<name>/conanfile.py and the text/font rendering design (section 2).
LOCAL_CONAN_RECIPES: tuple[str, ...] = ("third_party/harfbuzz", "third_party/catch2")


def locked_recipe_revisions(root: Path) -> dict[str, set[str]]:
    lockfile = root / "conan.lock"
    try:
        data = json.loads(lockfile.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise EngineError(f"failed to read {lockfile}: {exc}") from exc
    result: dict[str, set[str]] = {}
    for section in ("requires", "build_requires", "python_requires"):
        for entry in data.get(section, []):
            reference, _, revision = entry.partition("%")[0].partition("#")
            if revision:
                result.setdefault(reference, set()).add(revision)
    return result


def export_local_recipes(root: Path, *, locked: bool = False) -> None:
    # A locked install must use the recipes in this checkout. A cache that
    # already holds the pinned revision (a shared home, or one used by another
    # branch) would otherwise hide a recipe edit that conan.lock lacks.
    pinned = locked_recipe_revisions(root) if locked else {}
    for recipe in LOCAL_CONAN_RECIPES:
        recipe_dir = root / recipe
        if not (recipe_dir / "conanfile.py").is_file():
            continue
        print(f"Exporting local Conan recipe: {recipe}")
        argv = [conan(root), "export", str(recipe_dir), "--format", "json"]
        print(f"+ {command_line(argv)}", flush=True)
        # Conan logs to stderr (left on the terminal) and prints the exported
        # reference as JSON on stdout.
        try:
            completed = subprocess.run([str(arg) for arg in argv], cwd=str(root), env=tool_env(root),
                                       check=False, text=True, stdout=subprocess.PIPE)
        except OSError as exc:
            raise EngineError(f"command not found while running: {command_line(argv)}") from exc
        if completed.returncode != 0:
            raise EngineError(f"command failed with exit code {completed.returncode}: {command_line(argv)}")
        reference, _, revision = json.loads(completed.stdout)["reference"].partition("#")
        expected = pinned.get(reference)
        if expected is not None and revision not in expected:
            raise EngineError(
                f"conan.lock pins {reference}#{', #'.join(sorted(expected))}, but {recipe} exports "
                f"#{revision}. Run ./init.sh without --locked to update conan.lock.")


def prepare_conan_artifacts(
    root: Path,
    versions: dict[str, dict[str, str]],
    presets: Sequence[str],
    *,
    locked: bool = False,
) -> None:
    if locked and not (root / "conan.lock").is_file():
        raise EngineError("Locked initialization requires the committed conan.lock")
    check_current_python(root, versions["minimum"]["python"])
    # Fail before creating a venv or downloading managed tools when the host
    # toolchain is unavailable (especially with --no-system-install).
    configure_system_tool_shims(root, versions)
    validate_required_system_tools(root, versions)
    create_or_update_venv(root)
    install_managed_tools(root, versions)
    validate_managed_tools(root, versions)
    profile_path = install_conan_profile(root)
    # Exporting makes this checkout's recipe revisions the latest in the cache,
    # so a shared cache must not interleave another checkout's export before
    # this one resolves its lock and installs.
    with conan_cache_lock(root):
        run(
            [conan(root), "remote", "update", "conancenter", "--url", "https://center2.conan.io"],
            cwd=root,
            env=tool_env(root),
        )
        export_local_recipes(root, locked=locked)
        if not locked:
            create_conan_lock(root, profile_path)
        for preset in presets:
            conan_install_for_preset(root, profile_path, preset, locked=locked)


def cmake_cache_value(cache_path: Path, name: str) -> str | None:
    if not cache_path.is_file():
        return None
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(f"{name}:"):
            return line.partition("=")[2]
    return None


def cmake_cache_repair_reasons(root: Path, preset: str) -> list[str]:
    cache_path = build_dir_for_preset(root, preset) / "CMakeCache.txt"
    if not cache_path.is_file():
        return []

    expected_compiler = str(clang_cxx(root))
    expected_ninja = str(ninja(root))
    expected_toolchain = str(root / "out" / "conan" / preset / "conan_toolchain.cmake")
    checks = (
        ("CMAKE_CXX_COMPILER", expected_compiler),
        ("CMAKE_MAKE_PROGRAM", expected_ninja),
        ("CMAKE_TOOLCHAIN_FILE", expected_toolchain),
    )

    reasons: list[str] = []
    for variable, expected in checks:
        actual = cmake_cache_value(cache_path, variable)
        if actual is None:
            reasons.append(f"{variable} is missing")
        elif actual.endswith("-NOTFOUND"):
            reasons.append(f"{variable} is {actual}")
        elif Path(actual).resolve() != Path(expected).resolve():
            reasons.append(f"{variable} is {actual}, expected {expected}")

    if preset.startswith("macos-"):
        identity_file = cache_path.parent / ".ludus-macos-toolchain.json"
        try:
            previous = json.loads(identity_file.read_text())
        except (OSError, ValueError):
            previous = None
        if previous != macos_toolchain_identity(root):
            reasons.append("macOS SDK/compiler/libc++ inputs changed or have not been recorded")

    generator = cmake_cache_value(cache_path, "CMAKE_GENERATOR")
    if generator is not None and generator != "Ninja":
        reasons.append(f"CMAKE_GENERATOR is {generator}, expected Ninja")

    return reasons


def repair_existing_cmake_caches(root: Path, presets: Sequence[str]) -> None:
    for preset in presets:
        reasons = cmake_cache_repair_reasons(root, preset)
        if not reasons:
            continue
        print(f"Refreshing stale CMake cache for {preset}:")
        for reason in reasons:
            print(f"  - {reason}")
        cmake_configure(root, preset)


def create_conan_lock(root: Path, profile_path: Path) -> None:
    print("Network access: resolving the Conan dependency graph and updating conan.lock.")
    env = tool_env(root)
    lock_work_dir = root / "out" / "conan" / "lock-work"
    lock_work_dir.mkdir(parents=True, exist_ok=True)
    lockfile = root / "conan.lock"
    generated = lock_work_dir / "conan.lock.generated"
    merged = lock_work_dir / "conan.lock.merged"
    run(
        [
            conan(root),
            "lock",
            "create",
            str(root),
            "--profile:host",
            str(profile_path),
            "--profile:build",
            str(profile_path),
            "--settings:host",
            "build_type=Release",
            "--settings:build",
            "build_type=Release",
            "--lockfile=",
            "--lockfile-out",
            str(generated),
        ],
        cwd=lock_work_dir,
        env=env,
    )
    if lockfile.exists():
        # Conditional requirements differ by host (Linux Vulkan vs. macOS).
        # Keep pins for other supported hosts, and publish only after both
        # graph resolution and merging succeed. Failures preserve the old lock.
        run([conan(root), "lock", "merge", "--lockfile", str(lockfile),
             "--lockfile", str(generated), "--lockfile-out", str(merged)],
            cwd=lock_work_dir, env=env)
        merged.replace(lockfile)
    else:
        generated.replace(lockfile)


def conan_install_for_preset(root: Path, profile_path: Path, preset: str, *, locked: bool = False) -> None:
    build_type = PRESET_BUILD_TYPES[preset]
    output_dir = root / "out" / "conan" / preset
    output_dir.mkdir(parents=True, exist_ok=True)
    print(f"Network access: populating Conan cache and generator files for {preset}.")
    from init_options import read_options
    build_tests = read_options(root, preset).get("LUDUS_BUILD_TESTS", True)
    print("Conan may build missing third-party packages; test dependencies are included only when tests are enabled.")
    run(
        [
            conan(root),
            "install",
            str(root),
            "--output-folder",
            str(output_dir),
            "--profile:host",
            str(profile_path),
            "--profile:build",
            str(profile_path),
            "--settings:host",
            f"build_type={build_type}",
            "--settings:build",
            "build_type=Release",
            "--lockfile",
            str(root / "conan.lock"),
            *([] if locked else ["--lockfile-partial"]),
            "--build=missing",
            "--conf:host",
            f"user.ludus:build_tests={build_tests}",
        ],
        cwd=root,
        env=tool_env(root),
    )
    write_bootstrap_marker(root, preset)


def build_dir_for_preset(root: Path, preset: str) -> Path:
    return root / "out" / "build" / preset


def configure_needs_fresh_cache(root: Path, preset: str) -> bool:
    cache_path = build_dir_for_preset(root, preset) / "CMakeCache.txt"
    if not cache_path.is_file():
        return False
    cache_text = cache_path.read_text(encoding="utf-8", errors="replace")
    stale_package_cache_entries = ("Catch2_DIR:PATH=Catch2_DIR-NOTFOUND",)
    return any(entry in cache_text for entry in stale_package_cache_entries) or bool(cmake_cache_repair_reasons(root, preset))


def cmake_configure(root: Path, preset: str, extra_args: Sequence[str] = ()) -> None:
    from init_options import read_options
    from init_editor import preview_tool_args
    options = read_options(root, preset)
    if options.get("LUDUS_EDITOR_RENDER_PREVIEW", False):
        extra_args = [*preview_tool_args(root), *extra_args]
    request_codemodel(build_dir_for_preset(root, preset))
    args: list[object] = [cmake(root)]
    if configure_needs_fresh_cache(root, preset):
        print(f"Refreshing stale CMake cache for {preset}.")
        args.append("--fresh")
    args.extend(["-U", "Catch2_DIR", "--preset", preset])
    if options:
        # Scripted builds use the same saved selection as Conan, even after a
        # direct CMake invocation opted out in the existing cache.
        args.append("-DLUDUS_USE_INIT_OPTIONS=ON")
    args.extend(extra_args)
    run(args, cwd=root, env=tool_env(root))
    if preset.startswith("macos-"):
        (build_dir_for_preset(root, preset) / ".ludus-macos-toolchain.json").write_text(
            json.dumps(macos_toolchain_identity(root), sort_keys=True) + "\n")


def cmake_build(root: Path, preset: str, extra_args: Sequence[str] = ()) -> None:
    run([cmake(root), "--build", "--preset", preset, *extra_args], cwd=root, env=tool_env(root))


def ctest(root: Path, preset: str, label: str | None = None) -> None:
    args: list[object] = [cmake(root), "--build", "--preset", preset]
    run(args, cwd=root, env=tool_env(root))
    test_args: list[object] = [cmake(root), "-E", "env", "CTEST_OUTPUT_ON_FAILURE=1", managed_executable(root, "ctest"), "--preset", preset]
    if label:
        test_args.extend(["-L", label])
    run(test_args, cwd=root, env=tool_env(root))


def ensure_known_preset(preset: str) -> None:
    if preset not in PRESET_BUILD_TYPES:
        known = ", ".join(PRESET_BUILD_TYPES)
        raise EngineError(f"unknown preset '{preset}'. Known presets: {known}")


def ensure_bootstrap_for_preset(root: Path, preset: str) -> None:
    ensure_known_preset(preset)
    missing: list[Path] = [
        path
        for path in (
            cmake(root),
            ninja(root),
            conan(root),
            root / "conan.lock",
            root / "out" / "conan" / preset / "conan_toolchain.cmake",
        )
        if not path.exists()
    ]
    if missing:
        for path in missing:
            print(f"Missing bootstrap artifact: {path}")
        raise EngineError(f"bootstrap has not completed for this preset; run ./init.sh")
    if not bootstrap_marker_is_current(root, preset):
        print(f"Stale bootstrap artifact: {bootstrap_marker_path(root, preset)}")
        raise EngineError(f"bootstrap artifacts are stale; run ./init.sh")


def command_bootstrap(args: argparse.Namespace) -> int:
    root = repo_root()
    versions = load_tool_versions(root)
    prepare_conan_artifacts(root, versions, HOST_PRESETS)
    repair_existing_cmake_caches(root, HOST_PRESETS)
    print(f"Running minimal CMake configuration smoke test for {DEFAULT_PRESET}.")
    cmake_configure(root, DEFAULT_PRESET)
    if getattr(args, "show_next_commands", True):
        print("")
        print("Bootstrap complete. Next commands:")
        print("  ./scripts/build")
        print("  ./scripts/test")
        print("  ./scripts/install-sdk")
    return 0


def command_init(args: argparse.Namespace) -> int:
    root = repo_root()
    versions = load_tool_versions(root)
    preset = args.preset
    ensure_known_preset(preset)
    run_validation = args.validate or args.ci
    if args.all_presets and args.preset_only:
        raise EngineError("--all-presets and --preset-only describe conflicting initialization scopes")

    if args.ci:
        os.environ["CI"] = "true"

    print("Ludus one-command onboarding")
    print(f"Repository: {root}")
    print(f"Preset: {preset}")

    check_current_python(root, versions["minimum"]["python"])
    if args.no_system_install:
        print("Skipping automatic system package installation because --no-system-install was supplied.")
        configure_system_tool_shims(root, versions)
    else:
        if platform.system() == "Darwin":
            configure_system_tool_shims(root, versions)
            if system_prerequisites_need_install(root, versions):
                brew = shutil.which("brew")
                formulae = [formula for formula, _ in homebrew_llvm_formulae(llvm_major_version(versions))]
                if not brew:
                    raise EngineError("macOS setup requires Homebrew and Xcode Command Line Tools; "
                                      f"install {' '.join(formulae)} first")
                run([brew, "install", *formulae], cwd=root)
                configure_system_tool_shims(root, versions)
        else:
            install_ubuntu_system_prerequisites(root, versions)

    if args.preset_only and not run_validation:
        presets = (preset,)
    else:
        presets = HOST_PRESETS
    from init_options import save_options
    save_options(root, presets, args)
    prepare_conan_artifacts(root, versions, presets, locked=args.locked)
    from init_editor import prepare_editor_preview
    prepare_editor_preview(args, sys.modules[__name__])
    repair_existing_cmake_caches(root, presets)
    if args.with_rad_debugger:
        command_setup_rad_debugger(args)
    if command_doctor(argparse.Namespace()) != 0:
        raise EngineError("doctor reported an invalid required tool or project state")

    if not args.ci and not os.environ.get("CI"):
        command_install_hooks(argparse.Namespace())

    if getattr(args, "with_editor", False):
        from init_editor import setup_editor
        setup_editor(args, sys.modules[__name__])

    if getattr(args, "run_tests", False) and not run_validation:
        command_test(argparse.Namespace(preset=preset, label=None))

    if run_validation:
        command_build(argparse.Namespace(preset=preset, extra=[]))
        command_test(argparse.Namespace(preset=preset, label=None))

        if not args.skip_checks:
            command_check(
                argparse.Namespace(
                    preset=preset, format=False, tidy=False, include_cleaner=False, all=True, fix=False
                )
            )

        if not args.skip_sanitizers:
            command_build(argparse.Namespace(preset=NATIVE_PRESET_PREFIX + "-asan-ubsan", extra=[]))
            command_test(argparse.Namespace(preset=NATIVE_PRESET_PREFIX + "-asan-ubsan", label=None))

        if not args.skip_sdk:
            command_install_sdk(argparse.Namespace(preset=preset))

    print("")
    if run_validation:
        print("Ludus initialization and validation complete.")
    else:
        if getattr(args, "run_tests", False):
            print("Ludus initialization complete. Opted-in tests were built and executed.")
        elif getattr(args, "with_editor", False):
            print("Ludus initialization complete. The optional editor and its dependencies were built.")
        else:
            print("Ludus initialization complete. No engine targets were built.")
        if getattr(args, "with_editor", False):
            print(f"Launch the editor: ./scripts/editor --preset {preset}")
        print("Prepared presets: " + ", ".join(presets))
        print("VS Code CMake Tools is configured to use:")
        print(f"  {cmake(root)}")
        print("If VS Code was already open, reload the window before using the CMake Tools Build button.")
        print("Build from VS Code CMake Tools, or run:")
        print("  ./scripts/build")
        print("Run the full build/test/SDK validation with:")
        print("  ./init.sh --validate")
    return 0


def command_doctor(_args: argparse.Namespace) -> int:
    import rad_debugger

    root = repo_root()
    versions = load_tool_versions(root)
    statuses = [
        *[status for status in system_tool_statuses(root, versions) if status.required],
        *managed_tool_statuses(root, versions),
        *project_state_statuses(root),
        *[status for status in system_tool_statuses(root, versions) if not status.required],
        rad_debugger.tool_status(root, sys.modules[__name__]),
    ]
    print_status_table(statuses)
    return 1 if any(status.required and not status.ok for status in statuses) else 0


def command_setup_rad_debugger(args: argparse.Namespace) -> int:
    import rad_debugger

    try:
        return rad_debugger.setup(args, sys.modules[__name__])
    except OSError as exc:
        raise EngineError(f"RAD setup failed: {exc}") from exc


def command_debug(args: argparse.Namespace) -> int:
    import rad_debugger

    try:
        return rad_debugger.launch(args, sys.modules[__name__])
    except OSError as exc:
        raise EngineError(f"RAD launch failed: {exc}") from exc


def command_build(args: argparse.Namespace) -> int:
    root = repo_root()
    ensure_bootstrap_for_preset(root, args.preset)
    cmake_configure(root, args.preset)
    cmake_build(root, args.preset, args.extra)
    return 0


def command_test(args: argparse.Namespace) -> int:
    root = repo_root()
    from init_options import read_options
    if not read_options(root, args.preset).get("LUDUS_BUILD_TESTS", True):
        raise EngineError("Tests are disabled for this preset; opt in with ./init.sh --cli --with-tests " + args.preset + " --preset-only")
    ensure_bootstrap_for_preset(root, args.preset)
    cmake_configure(root, args.preset)
    ctest(root, args.preset, args.label)
    return 0


def source_files(root: Path, suffixes: set[str], roots: Iterable[str]) -> list[Path]:
    ignored_parts = {".git", "out", "__pycache__"}
    files: list[Path] = []
    for relative_root in roots:
        base = root / relative_root
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in suffixes:
                continue
            if any(part in ignored_parts for part in path.relative_to(root).parts):
                continue
            files.append(path)
    return sorted(files)


def find_system_tool(root: Path, name: str, versions: dict[str, dict[str, str]]) -> str:
    for status in system_tool_statuses(root, versions):
        if status.name == name:
            if status.ok:
                return status.path
            minimum = versions["minimum"].get(name.lower(), "")
            print_missing_tool_help(name, status.version, status.path, minimum, llvm_major_version(versions))
            raise EngineError(f"{name} is required")
    raise EngineError(f"unknown tool status requested: {name}")


def command_install_hooks(args: argparse.Namespace) -> int:
    root = repo_root()
    if not (root / ".git").exists():
        print("Skipping Git hooks outside a checkout.")
        return 0
    configured = subprocess.run(
        ["git", "config", "--get", "core.hooksPath"], cwd=root, text=True, capture_output=True,
    ).stdout.strip()
    if configured and configured != ".githooks":
        print(f"Keeping existing hooksPath {configured}; add .githooks/pre-commit to your hook chain.")
        return 0
    hook_path = run(["git", "rev-parse", "--git-path", "hooks/pre-commit"], cwd=root,
                    capture=True).stdout.strip()
    if not configured and (root / hook_path).exists():
        print("Keeping existing pre-commit hook; add .githooks/pre-commit to your hook chain.")
        return 0
    run(["git", "config", "--local", "core.hooksPath", ".githooks"], cwd=root)
    print("Installed automatic staged C/C++ formatting hook.")
    return 0


def run_format_check(root: Path, *, fix: bool) -> None:
    versions = load_tool_versions(root)
    clang_format = find_system_tool(root, "clang-format", versions)
    files = source_files(root, FORMAT_SUFFIXES, ("modules", "apps", "tests"))
    if not files:
        print("No source files found for clang-format.")
        return

    failures = []
    for path in files:
        source = path.read_text(encoding="utf-8")
        try:
            formatted = format_source(source, path, clang_format)
        except subprocess.CalledProcessError as exc:
            raise EngineError(exc.stderr) from exc
        except FormattingError as exc:
            raise EngineError(str(exc)) from exc
        if source == formatted:
            continue
        if fix:
            path.write_text(formatted, encoding="utf-8")
            print(f"Formatted {path.relative_to(root)}")
        else:
            failures.append(path)
            sys.stdout.writelines(difflib.unified_diff(
                source.splitlines(keepends=True), formatted.splitlines(keepends=True),
                fromfile=str(path.relative_to(root)), tofile="formatted",
            ))
    if failures:
        raise EngineError("formatting failed; run ./scripts/check --format --fix")


def compile_database_files(build_dir: Path) -> set[Path]:
    try:
        return {Path(entry["file"]) for entry in compilation_entries(build_dir)}
    except ValueError as exc:
        raise EngineError(str(exc)) from exc


def run_tidy(root: Path, preset: str, *, analyzer: bool = False) -> None:
    versions = load_tool_versions(root)
    clang_tidy = find_system_tool(root, "clang-tidy", versions)
    actual_version = executable_version(clang_tidy, ("--version",), root)
    if not version_tuple(actual_version) or version_tuple(actual_version)[0] != llvm_major_version(versions):
        raise EngineError(f"Static analysis requires reference clang-tidy {llvm_major_version(versions)}")
    build_dir = build_dir_for_preset(root, preset)
    if not (build_dir / "compile_commands.json").is_file() or not list(
            (build_dir / ".cmake/api/v1/reply").glob("index-*.json")):
        cmake_configure(root, preset)
    try:
        entries, report = inventory(root, build_dir)
    except ValueError as exc:
        raise EngineError(str(exc)) from exc
    directory = analysis_directory(root)
    policy = root / ("config/clang-analyzer.yml" if analyzer else ".clang-tidy")
    report["analysis"] = {"tool_version": actual_version, "tool_sha256": file_sha256(Path(clang_tidy)),
                          "policy": str(policy.relative_to(root)), "policy_sha256": file_sha256(policy)}
    (directory / "inventory.json").write_text(json.dumps(report, indent=2) + "\n")
    header_roots = sorted({str(root.resolve()), report["source_root"]})
    header_filter = f"^({'|'.join(re.escape(path) for path in header_roots)})/({'|'.join(TIDY_ROOTS)})/.*"
    commands = []
    # A single-entry database makes each compiled configuration explicit; tidy
    # cannot silently choose another target's defines for the same source file.
    for index, entry in enumerate(entries):
        database = directory / "commands" / str(index)
        database.mkdir(parents=True)
        (database / "compile_commands.json").write_text(json.dumps([entry]) + "\n")
        options = [f"--config-file={root / 'config/clang-analyzer.yml'}"] if analyzer else []
        commands.append([clang_tidy, "-p", database, "--quiet", "--warnings-as-errors=*",
                         *options, f"--header-filter={header_filter}", entry["file"]])
    print(f"Analysis inventory: {len(report['sources'])} sources, {len(entries)} compile commands", flush=True)
    run_analysis_commands(root, commands, directory=directory)


def analysis_directory(root: Path) -> Path:
    parent = root / "out/quality/analysis"
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix="run-", dir=parent))


def run_analysis_commands(root: Path, commands: Sequence[Sequence[object]], *,
                          env: dict[str, str] | None = None, directory: Path | None = None) -> None:
    """Bound concurrency, output and runtime; incomplete analysis always fails."""
    try:
        jobs = int(os.environ.get("LUDUS_TIDY_JOBS", "1"))
    except ValueError as exc:
        raise EngineError("LUDUS_TIDY_JOBS must be a positive integer") from exc
    if jobs < 1:
        raise EngineError("LUDUS_TIDY_JOBS must be a positive integer")
    try:
        shard_count = int(os.environ.get("LUDUS_TIDY_SHARD_COUNT", "1"))
        shard_index = int(os.environ.get("LUDUS_TIDY_SHARD_INDEX", "0"))
    except ValueError as exc:
        raise EngineError("Analysis shard count/index must be integers") from exc
    if shard_count < 1 or not 0 <= shard_index < shard_count:
        raise EngineError("Analysis requires a positive shard count and 0 <= index < count")
    total = len(commands)
    commands = commands[shard_index::shard_count]
    if not commands:
        raise EngineError("Analysis shard has no translation units; required analysis cannot be empty")
    if shard_count > 1:
        print(f"Analysis shard {shard_index + 1}/{shard_count}: {len(commands)} of {total} compile commands")
    directory = directory or analysis_directory(root)
    budget = LogBudget(LANE_BYTES)
    print(f"Analysis evidence: {directory}", flush=True)

    def analyze(item):
        index, command = item
        try:
            return run_bounded(command, cwd=root, env=env, log=directory / f"{index}.log",
                               timeout=ANALYSIS_SECONDS, limit=LOG_BYTES, budget=budget), None
        except OSError as exc:
            return None, f"Cannot launch analysis: {command_line(command)}: {exc}"

    records = []
    with ThreadPoolExecutor(max_workers=min(jobs, len(commands))) as pool:
        for command, (result, error) in zip(commands, pool.map(analyze, enumerate(commands))):
            print(f"+ {command_line(command)}", flush=True)
            if result is not None:
                if result.log.is_file():
                    print(result.log.read_text(encoding="utf-8", errors="replace"), end="", flush=True)
                state = "pass" if result.complete and result.returncode == 0 else "fail"
                record = {"argv": [str(arg) for arg in command], "state": state,
                          "exit_code": result.returncode, "log": result.log.name,
                          "bytes": result.bytes_written, "truncated": result.truncated,
                          "timed_out": result.timed_out, "complete": result.complete}
                if state != "pass":
                    print(f"Analysis exited with code {result.returncode}; complete={result.complete}", flush=True)
            else:
                record = {"argv": [str(arg) for arg in command], "state": "error", "error": error}
                print(error, flush=True)
            records.append(record)
    report = {"version": 1, "total_commands": total, "shard_index": shard_index,
              "shard_count": shard_count, "expected": len(commands), "completed": len(records),
              "results": records}
    (directory / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    failures = sum(record["state"] != "pass" for record in records)
    if len(records) != len(commands):
        raise EngineError("Static analysis omitted required compile commands")
    if failures:
        details = next((record["error"] for record in records if "error" in record), "")
        raise EngineError(f"Static analysis failed for {failures} translation unit(s). {details}")


def run_include_cleaner(root: Path, preset: str) -> None:
    """Advisory include-hygiene report using clang-tidy's misc-include-cleaner.

    This is intentionally NON-gating: misc-include-cleaner is noisy for a codebase
    that uses aggregation headers and re-exported type aliases, so it is a tool
    for pruning includes on demand, not a CI gate. The build-time budget
    (check-build-budget) is the enforced mechanism (see ADR 0005).
    """
    versions = load_tool_versions(root)
    clang_tidy = find_system_tool(root, "clang-tidy", versions)
    build_dir = root / "out" / "build" / preset
    if not (build_dir / "compile_commands.json").is_file():
        cmake_configure(root, preset)

    compiled_files = compile_database_files(build_dir)
    requested_files = [path.resolve() for path in source_files(root, {".cpp", ".cc", ".cxx", ".mm"}, TIDY_ROOTS)]
    targets = [path for path in requested_files if path in compiled_files]
    header_filter = f"^{re.escape(str(root))}/(modules|apps)/.*"
    print("Advisory include-cleaner report (not a gate; see check-build-budget for enforcement):")
    for path in targets:
        # No --warnings-as-errors: report only.
        return_code, output = capture_command_quiet(
            [
                clang_tidy,
                "-p",
                build_dir,
                "--quiet",
                "--checks=-*,misc-include-cleaner",
                f"--header-filter={header_filter}",
                path,
            ],
            cwd=root,
        )
        findings = [line for line in output.splitlines() if "include-cleaner" in line]
        if findings:
            print(f"  {path.relative_to(root)}: {len(findings)} suggestion(s)")
            for line in findings:
                print(f"    {line.replace(str(root) + '/', '')}")


def run_foundational_includes(root: Path) -> None:
    """Enforce the foundational-header include boundary (ADR 0007).

    Fast, text-only, toolchain-independent gate: the foundational headers must
    not pull strings/containers/heavy STL, and no public header may include a
    heavy STL header or a private implementation header. See
    tools/check_foundational_includes.py and
    docs/architecture/foundational-headers.md.
    """
    script = root / "tools" / "check_foundational_includes.py"
    run([sys.executable, str(script), str(root)], cwd=root)


def run_naming_check(root: Path) -> None:
    """Enforce the mechanically checkable naming/boolean rules (AGENTS.md "Naming").

    Text-only and toolchain-independent; scoped by config/naming_scopes.txt.
    See tools/check_naming.py.
    """
    script = root / "tools" / "check_naming.py"
    run([sys.executable, str(script), str(root)], cwd=root)


def run_container_policy(root: Path) -> None:
    """Reject first-party std::vector outside the documented exceptions.

    Text-only and toolchain-independent; see tools/check_container_policy.py
    and docs/architecture/containers.md section 34.3.
    """
    script = root / "tools" / "check_container_policy.py"
    run([sys.executable, str(script), str(root)], cwd=root)


def command_check(args: argparse.Namespace) -> int:
    root = repo_root()
    # getattr guards callers that build the Namespace by hand (e.g. command_init).
    include_cleaner = getattr(args, "include_cleaner", False)
    analyzer = getattr(args, "analyzer", False)
    tooling = getattr(args, "tooling", False)
    inventory_only = getattr(args, "inventory", False)
    explicit = args.format or args.tidy or include_cleaner or analyzer or tooling or inventory_only
    run_all = args.all or not explicit

    if args.format or run_all:
        run_format_check(root, fix=args.fix)
    # Foundational include-boundary gate runs on format-or-all: it is cheap and
    # needs no configured build.
    if args.format or run_all:
        run_foundational_includes(root)
        run_naming_check(root)
        run_container_policy(root)
    if args.tidy or run_all or include_cleaner or analyzer or inventory_only:
        ensure_bootstrap_for_preset(root, args.preset)
    if inventory_only:
        try:
            entries, report = inventory(root, build_dir_for_preset(root, args.preset))
        except ValueError as error:
            raise EngineError(str(error)) from error
        directory = analysis_directory(root)
        (directory / "inventory.json").write_text(json.dumps(report, indent=2) + "\n")
        print(f"Validated source inventory: {len(report['sources'])} sources, {len(entries)} compile commands")
    if args.tidy or run_all:
        cmake_configure(root, args.preset)
        run_tidy(root, args.preset)
    if analyzer:
        run_tidy(root, args.preset, analyzer=True)
    if tooling:
        from tooling_quality import check
        try:
            check(root)
        except RuntimeError as error:
            # CLI __main__ and importable engine have distinct exception classes.
            raise EngineError(str(error)) from error
    # Advisory: only when explicitly requested, never part of --all / the default.
    if include_cleaner:
        cmake_configure(root, args.preset)
        run_include_cleaner(root, args.preset)
    return 0


def verify_sdk_install(root: Path, prefix: Path, build_dir: Path) -> None:
    required_paths = [
        prefix / "include" / "ludus" / "foundation" / "base" / "version.hpp",
        prefix / "include" / "ludus" / "foundation" / "base" / "build_metadata.hpp",
        prefix / "include" / "ludus" / "foundation" / "base" / "assert_config.hpp",
        prefix / "include" / "ludus" / "foundation" / "base" / "assert.hpp",
        prefix / "include" / "ludus" / "foundation" / "base" / "assert_format.hpp",
        prefix / "include" / "ludus" / "foundation" / "base" / "core.h",
        prefix / "include" / "ludus" / "foundation" / "base" / "config.h",
        prefix / "include" / "ludus" / "foundation" / "base" / "compiler.h",
        prefix / "include" / "ludus" / "foundation" / "base" / "diagnostic_output.hpp",
        prefix / "lib" / "libludus_foundation_base.a",
        prefix / "include" / "ludus" / "graphics" / "rhi" / "rhi.h",
        prefix / "include" / "ludus" / "graphics" / "rhi" / "render.h",
        prefix / "lib" / "libludus_graphics_rhi.a",
        prefix / "lib" / "cmake" / "Ludus" / "LudusTargets.cmake",
        prefix / "lib" / "cmake" / "Ludus" / "LudusConfig.cmake",
        prefix / "lib" / "cmake" / "Ludus" / "LudusConfigVersion.cmake",
        prefix / "lib" / "cmake" / "Ludus" / "LudusShaders.cmake",
        prefix / "lib" / "cmake" / "Ludus" / "LudusRendererShaders.cmake",
        prefix / "lib" / "cmake" / "Ludus" / "shaders" / "compile_shader.py",
        prefix / "lib" / "cmake" / "Ludus" / "shaders" / "shader_toolchain.json",
        prefix / "lib" / "cmake" / "Ludus" / "shaders" / "spirv_cross_toolchain.json",
        prefix / "share" / "Ludus" / "licenses" / "LICENSE",
        prefix / "share" / "Ludus" / "licenses" / "THIRD_PARTY_NOTICES.md",
        prefix / "share" / "Ludus" / "LudusSdkManifest.json",
    ]

    missing = [path for path in required_paths if not path.exists()]
    if missing:
        for path in missing:
            print(f"Missing installed SDK artifact: {path}")
        raise EngineError("installed SDK is incomplete")

    private_headers = [path for path in (prefix / "include").rglob("*") if path.is_file() and "internal" in path.parts]
    if private_headers:
        for path in private_headers:
            print(f"Private header leaked into SDK install: {path}")
        raise EngineError("private headers must not be installed")

    forbidden_roots = [root / "modules", root / "apps", root / "cmake", root / "out" / "build"]
    package_files = list((prefix / "lib" / "cmake" / "Ludus").glob("*.cmake"))
    for package_file in package_files:
        text = package_file.read_text(encoding="utf-8")
        for forbidden in forbidden_roots:
            if str(forbidden) in text:
                raise EngineError(f"installed CMake package references source-tree path {forbidden} in {package_file}")

    # Relocatability audit (P02): no installed CMake metadata, anywhere under the
    # package's cmake tree (including bundled dependency configs), may embed an
    # absolute producer path. This is stricter than the Ludus-only check above.
    dep_cmake_root = prefix / "lib" / "cmake" / "Ludus"
    producer_markers = [str(root), str(root / "out")]
    for package_file in dep_cmake_root.rglob("*.cmake"):
        text = package_file.read_text(encoding="utf-8")
        for marker in producer_markers:
            if marker and marker in text:
                raise EngineError(
                    f"installed CMake metadata leaks producer path {marker} in {package_file}"
                )

    manifest = json.loads((prefix / "share" / "Ludus" / "LudusSdkManifest.json").read_text())
    expected = json.loads((build_dir / "cmake" / "LudusSdkManifest.json").read_text())
    if manifest != expected:
        raise EngineError("installed SDK manifest does not match the built variant")

    # Identity must be derived from real build inputs, not placeholders (P03).
    for required_field in ("target_triple", "compiler_id", "compiler_version", "sdk_variant"):
        value = manifest.get(required_field, "")
        if not value or "@" in str(value) or value == "unknown":
            raise EngineError(f"SDK manifest identity field {required_field!r} is unresolved: {value!r}")
    if not isinstance(manifest.get("dependencies"), list) or not manifest["dependencies"]:
        raise EngineError("SDK manifest is missing its redistributable dependency inventory")
    config = (prefix / "include" / "ludus" / "foundation" / "base" / "assert_config.hpp").read_text()
    for macro, key in (("LUDUS_ENABLE_ASSERTS", "enable_asserts"), ("LUDUS_BREAK_ON_CHECK", "break_on_check"),
                       ("LUDUS_BUILD_FLAVOR_ID", "build_flavor_id"), ("LUDUS_ASSERT_POLICY_VERSION", "assert_policy_version"),
                       ("LUDUS_ASSERT_DIALOGS_AVAILABLE", "assert_dialogs_available")):
        match = re.search(rf"^#define {macro} ([0-9]+)$", config, re.MULTILINE)
        if not match or int(match.group(1)) != manifest[key]:
            raise EngineError(f"installed assertion policy mismatch: {macro}")
    print(f"Installed SDK artifacts and assertion policy verified: {prefix}")


def _conan_package_dirs(conan_dir: Path) -> dict:
    """Discover each dependency's Conan package root from CMakeDeps data files.

    CMakeDeps emits ``<pkg>-release-*-data.cmake`` (and config files) that set
    ``<pkg>_PACKAGE_FOLDER_<CONFIG> "<abs package root>"``. We read those to learn
    where each dependency's lib/include/cmake files live, without hard-coding the
    Conan cache layout.
    """
    package_dirs: dict = {}
    if not conan_dir.is_dir():
        return package_dirs
    pattern = re.compile(r'set\(([A-Za-z0-9_]+)_PACKAGE_FOLDER_[A-Z]+\s+"([^"]+)"')
    for data_file in conan_dir.rglob("*-data.cmake"):
        text = data_file.read_text(encoding="utf-8")
        for match in pattern.finditer(text):
            name = match.group(1).lower()
            folder = Path(match.group(2))
            if folder.is_dir():
                package_dirs.setdefault(name, folder)
    return package_dirs


def bundle_sdk_dependencies(root: Path, preset: str, prefix: Path) -> None:
    """Copy the redistributable dependency closure into the installed prefix and
    rewrite absolute producer paths so the SDK relocates (P02)."""
    try:
        from ludus_tools import bundle_deps
    except ImportError:
        sys.path.insert(0, str(root / "scripts" / "python"))
        from ludus_tools import bundle_deps

    conan_dir = root / "out" / "conan" / preset
    package_dirs = _conan_package_dirs(conan_dir)
    # Absolute producer roots that must never survive into the relocated bundle.
    producer_roots = [str(root), str(root / "out"), str(conan_dir)]
    for folder in package_dirs.values():
        producer_roots.append(str(folder))
    bundled = bundle_deps.bundle_from_conan(
        prefix=prefix,
        generators_dir=conan_dir,
        package_dirs=package_dirs,
    )
    print(f"Bundled SDK dependencies: {', '.join(bundled) if bundled else '(none found)'}")
    leaks = bundle_deps.audit_no_producer_paths(prefix, producer_roots)
    if leaks:
        for leak in leaks:
            print(f"Leaked producer path in bundled dependency: {leak}")
        raise EngineError("bundled dependency metadata leaks a producer path")


def command_bundle_sdk_dependencies(args: argparse.Namespace) -> int:
    bundle_sdk_dependencies(repo_root(), args.preset, Path(args.prefix).resolve())
    return 0


def command_install_sdk(args: argparse.Namespace) -> int:
    root = repo_root()
    preset = args.preset
    ensure_bootstrap_for_preset(root, preset)
    cmake_configure(root, preset)
    cmake_build(root, preset)

    build_dir = root / "out" / "build" / preset
    prefix = root / "out" / "install" / preset
    run([cmake(root), "--install", build_dir, "--prefix", prefix], cwd=root, env=tool_env(root))

    # Bundle the redistributable dependency closure into the prefix so the SDK is
    # relocatable without the producer Conan cache (P02). Done before verify so
    # the relocation audit covers the bundled dependency metadata too.
    bundle_sdk_dependencies(root, preset, prefix)

    verify_sdk_install(root, prefix, build_dir)

    consumer_source = root / "tests" / "sdk_consumer"
    consumer_build = root / "out" / "build" / "sdk-consumer" / preset
    native_flags = []
    if preset.startswith("macos-"):
        from ludus_tools.project_setup import _inputs
        _, records, _ = _inputs(root, consumer_source, prefix, preset, None)
        native_flags = [f"-D{key}={value}" for key, value in records[preset]["cacheVariables"].items()
                        if key.startswith("CMAKE_OSX_") or key == "CMAKE_CXX_FLAGS"]
    run(
        [
            cmake(root),
            "-S",
            consumer_source,
            "-B",
            consumer_build,
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja(root)}",
            f"-DCMAKE_CXX_COMPILER={clang_cxx(root)}",
            f"-DCMAKE_BUILD_TYPE={PRESET_BUILD_TYPES[preset]}",
            f"-DCMAKE_PREFIX_PATH={prefix}",
            *native_flags,
        ],
        cwd=root,
        env=tool_env(root),
    )
    run([cmake(root), "--build", consumer_build], cwd=root, env=tool_env(root))
    result = run([consumer_build / "ludus_sdk_consumer"], cwd=root, env=tool_env(root), capture=True)
    print(result.stdout, end="")
    manifest = json.loads((prefix / "share" / "Ludus" / "LudusSdkManifest.json").read_text())
    runtime_policy = (f"flavor={manifest['build_flavor']} asserts={manifest['enable_asserts']} "
                      f"policy-version={manifest['assert_policy_version']} check-break={manifest['break_on_check']}")
    if runtime_policy not in result.stdout:
        raise EngineError("installed assertion runtime policy does not match the SDK header/manifest")
    return 0


# --------------------------------------------------------------------------- #
# Build-time profiling
# --------------------------------------------------------------------------- #


def clang_build_analyzer_dir(root: Path) -> Path:
    return root / "out" / "host-tools" / f"clang-build-analyzer-{CLANG_BUILD_ANALYZER_VERSION}"


def clang_build_analyzer_binary(root: Path) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    return clang_build_analyzer_dir(root) / "build" / f"ClangBuildAnalyzer{suffix}"


def prepare_clang_build_analyzer_includes(source_dir: Path) -> None:
    """Make the pinned host tool's standard dependencies explicit for libc++ 23.

    Thanks to Doug Binks/enkiTS and LLVM's demangler contributors, as bundled
    by Aras Pranckevičius in ClangBuildAnalyzer v1.6.0. Preserve their licensed
    bodies; these marked edits only add the headers declaring existing uses.
    https://github.com/aras-p/ClangBuildAnalyzer/tree/v1.6.0/src/external
    """
    profiles = (
        ("enkiTS/TaskScheduler.cpp", '#include "TaskScheduler.h"\n', "#include <cstdlib>\n#include <type_traits>\n"),
        ("llvm-Demangle/include/Utility.h", '#include "StringView.h"\n', "#include <exception>\n"),
        ("llvm-Demangle/include/ItaniumDemangle.h", '#include "DemangleConfig.h"\n', "#include <type_traits>\n"),
    )
    marker = "// Ludus host-tool include profile: explicit standard dependencies.\n"
    for name, anchor, includes in profiles:
        path = source_dir / "src/external" / name
        original = path.read_text()
        replacement = anchor + marker + includes
        if original.count(replacement) == 1:
            continue
        if original.count(anchor) != 1 or marker in original:
            raise EngineError(f"ClangBuildAnalyzer include profile no longer matches {name}")
        path.write_text(original.replace(anchor, replacement))


def ensure_clang_build_analyzer(root: Path) -> Path:
    """Fetch and build the pinned ClangBuildAnalyzer on demand.

    Kept out of the init.sh path deliberately: it is only needed for profiling,
    so we build it lazily into out/host-tools/ using the project-managed CMake,
    Ninja, and Clang toolchain. Returns the path to the built binary.
    """
    binary = clang_build_analyzer_binary(root)
    if binary.is_file():
        return binary

    source_dir = clang_build_analyzer_dir(root)
    if not (source_dir / "CMakeLists.txt").is_file():
        archive_url = f"{CLANG_BUILD_ANALYZER_REPO}/archive/refs/tags/v{CLANG_BUILD_ANALYZER_VERSION}.tar.gz"
        archive_path = root / "out" / "host-tools" / f"clang-build-analyzer-{CLANG_BUILD_ANALYZER_VERSION}.tar.gz"
        archive_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"Network access: downloading ClangBuildAnalyzer {CLANG_BUILD_ANALYZER_VERSION}.")
        try:
            with urllib.request.urlopen(archive_url) as response:  # noqa: S310 (pinned https URL)
                archive_path.write_bytes(response.read())
        except OSError as exc:
            raise EngineError(f"failed to download ClangBuildAnalyzer: {exc}") from exc

        extract_root = root / "out" / "host-tools"
        import tarfile

        with tarfile.open(archive_path, "r:gz") as tar:
            tar.extractall(extract_root)  # noqa: S202 (trusted pinned release)
        extracted = extract_root / f"ClangBuildAnalyzer-{CLANG_BUILD_ANALYZER_VERSION}"
        if source_dir.exists():
            shutil.rmtree(source_dir)
        extracted.rename(source_dir)

    prepare_clang_build_analyzer_includes(source_dir)
    build_dir = source_dir / "build"
    build_dir.mkdir(parents=True, exist_ok=True)
    env = tool_env(root)
    run(
        [
            cmake(root),
            "-S",
            source_dir,
            "-B",
            build_dir,
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja(root)}",
            f"-DCMAKE_CXX_COMPILER={clang_cxx(root)}",
            "-DCMAKE_BUILD_TYPE=Release",
        ],
        cwd=root,
        env=env,
    )
    run([cmake(root), "--build", build_dir], cwd=root, env=env)
    if not binary.is_file():
        raise EngineError(f"ClangBuildAnalyzer build did not produce {binary}")
    return binary


def summarize_ninja_log(build_dir: Path) -> dict[str, object]:
    """Parse .ninja_log into per-target durations and the total/critical figures.

    .ninja_log v5 lines are: start_ms  end_ms  restat_mtime  output  cmdhash.
    The build wall-time is the span from the earliest start to the latest end;
    the summed per-edge time divided by the wall-time approximates achieved
    parallelism.
    """
    log_path = build_dir / ".ninja_log"
    if not log_path.is_file():
        return {}

    edges: list[tuple[int, int, str]] = []
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        try:
            start_ms = int(parts[0])
            end_ms = int(parts[1])
        except ValueError:
            continue
        edges.append((start_ms, end_ms, parts[3]))

    if not edges:
        return {}

    wall_ms = max(end for _, end, _ in edges) - min(start for start, _, _ in edges)
    summed_ms = sum(end - start for start, end, _ in edges)
    slowest = sorted(edges, key=lambda e: e[1] - e[0], reverse=True)[:15]
    return {
        "edge_count": len(edges),
        "wall_ms": wall_ms,
        "summed_ms": summed_ms,
        "parallelism": round(summed_ms / wall_ms, 2) if wall_ms else 0,
        "slowest_targets": [{"target": t, "ms": end - start} for start, end, t in slowest],
    }


def command_profile_build(args: argparse.Namespace) -> int:
    """Profile a clean build: time configure and build separately, capture Ninja
    edge timings, and (with Clang -ftime-trace) aggregate per-TU compile costs
    via ClangBuildAnalyzer. Writes a machine-readable report to out/profile/."""
    root = repo_root()
    preset = args.preset
    ensure_bootstrap_for_preset(root, preset)

    build_dir = build_dir_for_preset(root, preset)
    profile_dir = root / "out" / "profile"
    profile_dir.mkdir(parents=True, exist_ok=True)

    # Always start from a clean build tree so numbers are comparable run to run.
    if build_dir.exists():
        print(f"Removing existing build tree for a clean profile: {build_dir}")
        shutil.rmtree(build_dir)

    extra_configure = ["-DLUDUS_ENABLE_TIME_TRACE=ON"] if not args.no_time_trace else []
    extra_configure.append("-DLUDUS_ENABLE_CCACHE=" + ("ON" if args.ccache else "OFF"))

    print(f"Profiling clean build of preset '{preset}'.")
    configure_start = time.monotonic()
    cmake_configure(root, preset, extra_configure)
    configure_seconds = time.monotonic() - configure_start

    build_start = time.monotonic()
    cmake_build(root, preset)
    build_seconds = time.monotonic() - build_start

    report: dict[str, object] = {
        "preset": preset,
        "build_type": PRESET_BUILD_TYPES[preset],
        "time_trace": not args.no_time_trace,
        "ccache": bool(args.ccache),
        "configure_seconds": round(configure_seconds, 2),
        "build_seconds": round(build_seconds, 2),
        "ninja": summarize_ninja_log(build_dir),
    }

    # Aggregate -ftime-trace outputs into a ranked report of the most expensive
    # headers and template instantiations across the whole build.
    if not args.no_time_trace:
        analyzer = ensure_clang_build_analyzer(root)
        capture_file = profile_dir / f"{preset}-cba.bin"
        run([analyzer, "--all", build_dir, capture_file], cwd=root, env=tool_env(root))
        analysis_text = capture_command([analyzer, "--analyze", capture_file], cwd=root, env=tool_env(root))
        analysis_path = profile_dir / f"{preset}-analysis.txt"
        analysis_path.write_text(analysis_text + "\n", encoding="utf-8")
        report["clang_build_analyzer_report"] = str(analysis_path.relative_to(root))
        print("")
        print(analysis_text)

    report_path = profile_dir / f"{preset}-profile.json"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print("")
    print("Build profile summary")
    print(f"  preset:            {preset} ({PRESET_BUILD_TYPES[preset]})")
    print(f"  configure:         {configure_seconds:.2f}s")
    print(f"  build:             {build_seconds:.2f}s")
    ninja_summary = report["ninja"]
    if isinstance(ninja_summary, dict) and ninja_summary:
        print(f"  compile edges:     {ninja_summary.get('edge_count')}")
        print(f"  build wall (ninja):{int(ninja_summary.get('wall_ms', 0)) / 1000:.2f}s")
        print(f"  achieved parallel: {ninja_summary.get('parallelism')}x")
    print(f"  report:            {report_path.relative_to(root)}")
    return 0


def command_profile_engine(args: argparse.Namespace) -> int:
    """Build and run the runtime profiling scenarios from config/runtime_profile_scenarios.json.

    Writes report.json, summary.md, Perfetto traces and per-run logs. Fails on
    scenario correctness failures only; timings are report-only."""
    import runtime_profile

    root = repo_root()
    preset = args.preset or NATIVE_PRESET_PREFIX + "-profile"
    ensure_known_preset(preset)
    if not preset.startswith(NATIVE_PRESET_PREFIX):
        raise EngineError(f"profile-engine runs host presets only ({NATIVE_PRESET_PREFIX}-*), got {preset}")
    try:
        scenarios = runtime_profile.load_manifest(root / (args.manifest or runtime_profile.MANIFEST_PATH))
        system = platform.system()
        shader_options = runtime_profile.shader_tool_options(root, system)
        capabilities = runtime_profile.resolve_capabilities(
            runtime_profile.detect_capabilities(root, system), args.capability or (), args.device)
        if args.list:
            for scenario in scenarios:
                needs = ", ".join(scenario.requires) or "none"
                print(f"{scenario.id:<26} {scenario.owner:<32} requires: {needs}")
                print(f"{'':<26} {scenario.description}")
            return 0
        selection = runtime_profile.select_scenarios(scenarios, args.scenario or (), capabilities)
        runnable = [scenario for scenario, reason in selection if reason is None]
        if not runnable:
            raise EngineError("no runnable scenarios for this host; see --list and --capability")
        if not args.no_build:
            ensure_bootstrap_for_preset(root, preset)
            cmake_configure(root, preset, runtime_profile.configure_arguments(runnable, shader_options))
            targets = sorted({scenario.target for scenario in runnable})
            cmake_build(root, preset, ["--target", *targets])
        output_dir = Path(args.output).resolve() if args.output else root / "out" / "runtime-profile" / preset
        compiler = executable_version(str(clang_cxx(root)), ["--version"], root) if clang_cxx(root).exists() else "unknown"
        report = runtime_profile.run_scenarios(
            root, preset, build_dir_for_preset(root, preset), output_dir, selection, capabilities,
            compiler=compiler, repetitions=args.repetitions,
            baseline_path=Path(args.baseline).resolve() if args.baseline else None, device=args.device,
            controlled_environment=runtime_profile.environment_names(scenarios))
    except runtime_profile.ProfileError as error:
        raise EngineError(str(error)) from error
    failed = [entry["id"] for entry in report["scenarios"] if entry["status"] == "failed"]
    print(f"Runtime profile report: {output_dir / 'report.json'}")
    print(f"Summary: {output_dir / 'summary.md'}")
    if failed:
        raise EngineError(f"runtime profile scenario(s) failed: {', '.join(failed)}")
    return 0


# --------------------------------------------------------------------------- #
# Build-time budget gate
# --------------------------------------------------------------------------- #

# Matches a ClangBuildAnalyzer "Expensive headers" line, e.g.:
#   7794 ms: /abs/path/log.hpp (included 7 times, avg 1113 ms), included via:
_EXPENSIVE_HEADER_RE = re.compile(
    r"^\s*(\d+)\s*ms:\s*(?P<path>.+?)\s*\(included\s+(?P<count>\d+)\s+times,\s*avg\s+(?P<avg>\d+)\s*ms\)"
)


def load_build_budget(root: Path) -> dict[str, object]:
    budget_path = root / "config" / "build_budget.json"
    try:
        data = json.loads(budget_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise EngineError(f"failed to read {budget_path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise EngineError(f"failed to parse {budget_path}: {exc}") from exc
    return data


def parse_expensive_headers(analysis_text: str) -> list[tuple[str, int, int]]:
    """Return (absolute_path, include_count, avg_ms) for each Expensive-headers entry."""
    results: list[tuple[str, int, int]] = []
    in_section = False
    for line in analysis_text.splitlines():
        if line.startswith("**** Expensive headers"):
            in_section = True
            continue
        if in_section and line.startswith("****"):
            break
        match = _EXPENSIVE_HEADER_RE.match(line)
        if match:
            results.append((match.group("path"), int(match.group("count")), int(match.group("avg"))))
    return results


def command_check_build_budget(args: argparse.Namespace) -> int:
    """Fail if a project header's average per-include parse time, or total frontend
    parse time, exceeds the committed budget in config/build_budget.json. Consumes
    the reports produced by `profile-build`; run that first (or pass --profile to
    run it here). See docs/decisions/0005-build-time-budgets.md."""
    root = repo_root()
    budget = load_build_budget(root)
    preset = args.preset or str(budget.get("preset", DEFAULT_PRESET))

    profile_dir = root / "out" / "profile"
    analysis_path = profile_dir / f"{preset}-analysis.txt"
    report_path = profile_dir / f"{preset}-profile.json"

    if args.profile or not analysis_path.is_file() or not report_path.is_file():
        print("Profiling build to produce budget inputs.")
        command_profile_build(argparse.Namespace(preset=preset, no_time_trace=False, ccache=False))

    if not analysis_path.is_file():
        raise EngineError(f"missing profiling report: {analysis_path}; run ./scripts/profile-build {preset}")

    default_avg = int(budget.get("default_header_avg_ms", 0))
    overrides = {str(k): int(v) for k, v in dict(budget.get("header_overrides_avg_ms", {})).items()}
    ignored = {str(p) for p in list(budget.get("ignore_headers", []))}
    total_frontend_budget_s = float(budget.get("total_frontend_seconds", 0))

    analysis_text = analysis_path.read_text(encoding="utf-8")
    headers = parse_expensive_headers(analysis_text)

    root_str = str(root) + "/"
    breaches: list[str] = []
    checked = 0
    for absolute_path, _count, avg_ms in headers:
        # Only budget the project's own headers (under modules/); third-party and
        # standard headers are out of our control and handled by include hygiene.
        if root_str not in absolute_path:
            continue
        relative = absolute_path.replace(root_str, "")
        if not relative.startswith("modules/"):
            continue
        if relative in ignored:
            continue
        checked += 1
        limit = overrides.get(relative, default_avg)
        status = "OK" if avg_ms <= limit else "OVER"
        print(f"  [{status}] {relative}: avg {avg_ms} ms (budget {limit} ms)")
        if avg_ms > limit:
            breaches.append(
                f"{relative}: average per-include parse {avg_ms} ms exceeds budget {limit} ms"
            )

    # Total frontend parse budget from the ClangBuildAnalyzer time summary.
    frontend_match = re.search(r"Parsing \(frontend\):\s*([\d.]+)\s*s", analysis_text)
    if frontend_match and total_frontend_budget_s > 0:
        frontend_s = float(frontend_match.group(1))
        status = "OK" if frontend_s <= total_frontend_budget_s else "OVER"
        print(f"  [{status}] total frontend parsing: {frontend_s:.1f} s (budget {total_frontend_budget_s:.0f} s)")
        if frontend_s > total_frontend_budget_s:
            breaches.append(
                f"total frontend parsing {frontend_s:.1f} s exceeds budget {total_frontend_budget_s:.0f} s"
            )

    print("")
    if breaches:
        print("Build-time budget exceeded:")
        for breach in breaches:
            print(f"  - {breach}")
        print("")
        print("A heavy include likely leaked into a header. Investigate with")
        print(f"  ./scripts/profile-build {preset}")
        print("then reduce the include (type-erase / move to .cpp) or, if justified,")
        print("raise the budget in config/build_budget.json with a note in the PR.")
        raise EngineError("build-time budget check failed")

    print(f"Build-time budget check passed ({checked} project headers within budget).")
    return 0


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Ludus development orchestration")
    subparsers = parser.add_subparsers(dest="command", required=True)

    init_parser = subparsers.add_parser("init", help="install prerequisites and prepare local build dependencies")
    init_parser.add_argument("preset", nargs="?", default=None)
    interface = init_parser.add_mutually_exclusive_group()
    interface.add_argument("--cli", action="store_true", help="use terminal setup without opening a window")
    interface.add_argument("--gui", action="store_true", help="require the graphical setup window")
    init_parser.add_argument("--persona", choices=("contributor", "application", "browser", "validation"),
                             default="contributor", help="setup defaults (default: contributor); explicit options override them")
    init_parser.add_argument("--no-system-install", action="store_true", help="do not install Ubuntu packages automatically")
    init_parser.add_argument("--with-rad-debugger", action="store_true", help="also build the optional pinned Linux RAD Debugger")
    editor = init_parser.add_mutually_exclusive_group()
    editor.add_argument("--with-editor", dest="with_editor", action="store_true",
                        help="install Qt and pinned shader tools, then build the native editor with 3D preview")
    editor.add_argument("--no-editor", dest="with_editor", action="store_false",
                        help="skip optional editor setup (default)")
    init_parser.set_defaults(with_editor=False)
    preview = init_parser.add_mutually_exclusive_group()
    preview.add_argument("--with-editor-preview", dest="with_editor_preview", action="store_true",
                         help="include native editor 3D preview and prepare pinned shader tools (default)")
    preview.add_argument("--no-editor-preview", dest="with_editor_preview", action="store_false",
                         help="build a lightweight document-only editor without shader tools")
    init_parser.set_defaults(with_editor_preview=True)
    for name, label in (("tests", "native test targets and Catch2 dependencies"),
                        ("smoke-app", "sample applications (smoke and native input demo)"),
                        ("web-probes", "browser feasibility probes"),
                        ("shader-probe", "isolated native shader feasibility probe")):
        group = init_parser.add_mutually_exclusive_group()
        dest = "with_" + name.replace("-", "_")
        group.add_argument("--with-" + name, dest=dest, action="store_true", help="enable " + label)
        group.add_argument("--no-" + name, dest=dest, action="store_false", help="disable " + label)
        init_parser.set_defaults(**{dest: None})
    init_parser.add_argument("--run-tests", action="store_true", help="build and execute native tests after setup (implies --with-tests)")
    init_parser.add_argument("--all-presets", action="store_true", help="prepare Conan files for all native presets (contributor default)")
    init_parser.add_argument("--preset-only", action="store_true", help="prepare only the selected preset")
    init_parser.add_argument("--validate", "--full", action="store_true", help="also build, test, check, and validate the SDK")
    init_parser.add_argument("--skip-checks", action="store_true", help="skip clang-format and clang-tidy checks during validation")
    init_parser.add_argument("--skip-sanitizers", action="store_true", help="skip the ASan/UBSan preset during validation")
    init_parser.add_argument("--skip-sdk", action="store_true", help="skip the installed SDK consumer test during validation")
    init_parser.add_argument("--locked", action="store_true", help="consume conan.lock without updating it or permitting unlocked dependencies")
    init_parser.add_argument("--ci", action="store_true", help="run validation with CI=true for warnings-as-errors")
    init_parser.set_defaults(func=command_init)

    bootstrap_parser = subparsers.add_parser("bootstrap", help="install managed tools and prepare Conan/CMake")
    bootstrap_parser.add_argument("preset", nargs="?", default=None)
    bootstrap_parser.set_defaults(func=command_bootstrap)

    hooks_parser = subparsers.add_parser("install-hooks", help="enable automatic formatting before commits")
    hooks_parser.set_defaults(func=command_install_hooks)

    doctor_parser = subparsers.add_parser("doctor", help="diagnose host and project tool state")
    doctor_parser.add_argument("preset", nargs="?", default=None)
    doctor_parser.set_defaults(func=command_doctor)

    rad_parser = subparsers.add_parser("setup-rad-debugger", help="install the optional pinned Linux RAD Debugger")
    rad_parser.add_argument("--no-system-install", action="store_true", help="require already-installed RAD build prerequisites")
    rad_parser.set_defaults(func=command_setup_rad_debugger)

    debug_parser = subparsers.add_parser("debug", help="build a native executable target and open RAD (options before preset)")
    debug_parser.add_argument("--debugger", help="RAD executable path (overrides LUDUS_RAD_DEBUGGER)")
    debug_parser.add_argument("--cwd", help="game working directory (default: repository root)")
    debug_parser.add_argument("--no-build", action="store_true", help="reuse an existing CMake File API reply and executable")
    debug_parser.add_argument("--dry-run", action="store_true", help="prepare the launch and print the command without opening RAD")
    debug_parser.add_argument("preset", help="linux-clang-debug or linux-clang-development")
    debug_parser.add_argument("target", help="CMake executable target name")
    debug_parser.add_argument("arguments", nargs=argparse.REMAINDER, help="game arguments after --")
    debug_parser.set_defaults(func=command_debug)

    build_parser = subparsers.add_parser("build", help="configure and build a preset")
    build_parser.add_argument("preset", nargs="?", default=DEFAULT_PRESET)
    build_parser.add_argument("extra", nargs=argparse.REMAINDER)
    build_parser.set_defaults(func=command_build)

    test_parser = subparsers.add_parser("test", help="build tests and run CTest")
    test_parser.add_argument("preset", nargs="?", default=DEFAULT_PRESET)
    test_parser.add_argument("--label", "-L", help="CTest label regex")
    test_parser.set_defaults(func=command_test)

    check_parser = subparsers.add_parser("check", help="run formatting and clang-tidy checks")
    check_parser.add_argument("preset", nargs="?", default=DEFAULT_PRESET)
    check_parser.add_argument("--format", action="store_true", help="verify clang-format")
    check_parser.add_argument("--tidy", action="store_true", help="run targeted clang-tidy")
    check_parser.add_argument("--analyzer", action="store_true", help="run advisory stable LLVM analyzer rules on the complete configured inventory")
    check_parser.add_argument("--tooling", action="store_true", help="check Python and workflows with prepared pinned Ruff/actionlint")
    check_parser.add_argument("--inventory", action="store_true", help="validate complete configured source inventory without running analyzers")
    check_parser.add_argument(
        "--include-cleaner",
        action="store_true",
        dest="include_cleaner",
        help="advisory include-hygiene report (misc-include-cleaner); not a gate, not part of --all",
    )
    check_parser.add_argument("--all", action="store_true", help="run all checks")
    check_parser.add_argument("--fix", action="store_true", help="allow clang-format to update files")
    check_parser.set_defaults(func=command_check)

    install_parser = subparsers.add_parser("install-sdk", help="install the SDK and run the external consumer")
    install_parser.add_argument("preset", nargs="?", default=DEFAULT_PRESET)
    install_parser.set_defaults(func=command_install_sdk)

    bundle_parser = subparsers.add_parser("bundle-sdk-dependencies", help="bundle prepared SDK dependencies without rebuilding")
    bundle_parser.add_argument("preset")
    bundle_parser.add_argument("--prefix", required=True)
    bundle_parser.set_defaults(func=command_bundle_sdk_dependencies)

    profile_parser = subparsers.add_parser(
        "profile-build", help="profile a clean build (configure/build timing, Ninja + Clang -ftime-trace)"
    )
    profile_parser.add_argument("preset", nargs="?", default=DEFAULT_PRESET)
    profile_parser.add_argument(
        "--no-time-trace", action="store_true", help="skip Clang -ftime-trace and ClangBuildAnalyzer aggregation"
    )
    profile_parser.add_argument("--ccache", action="store_true", help="enable ccache for the profiled build")
    profile_parser.set_defaults(func=command_profile_build)

    runtime_parser = subparsers.add_parser(
        "profile-engine", help="build and run runtime profiling scenarios (report-only timings, Perfetto traces)"
    )
    runtime_parser.add_argument("preset", nargs="?", default=None, help="host preset (default: the native -profile preset)")
    runtime_parser.add_argument("--scenario", action="append", help="run only this scenario id (repeatable)")
    runtime_parser.add_argument(
        "--capability", action="append",
        help="declare a lane capability such as presentation or a GPU class gpu-<class> (repeatable)"
    )
    runtime_parser.add_argument(
        "--device", help="declared GPU adapter, OS and driver identity recorded in the report (required with gpu-<class>)"
    )
    runtime_parser.add_argument("--repetitions", type=int, choices=range(1, 51), metavar="1..50",
                                help="override each scenario's measured repetitions")
    runtime_parser.add_argument("--output", help="report directory (default: out/runtime-profile/<preset>)")
    runtime_parser.add_argument("--baseline", help="earlier report.json to compare medians against")
    runtime_parser.add_argument("--manifest", help="scenario manifest (default: config/runtime_profile_scenarios.json)")
    runtime_parser.add_argument("--no-build", action="store_true", help="run already-built executables")
    runtime_parser.add_argument("--list", action="store_true", help="list scenarios and exit")
    runtime_parser.set_defaults(func=command_profile_engine)

    budget_parser = subparsers.add_parser(
        "check-build-budget", help="fail if project headers or total parse time exceed config/build_budget.json"
    )
    budget_parser.add_argument("preset", nargs="?", default=None)
    budget_parser.add_argument(
        "--profile", action="store_true", help="run profile-build first instead of reusing existing reports"
    )
    budget_parser.set_defaults(func=command_check_build_budget)

    return parser


def main(argv: Sequence[str]) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(line_buffering=True)
    parser = make_parser()
    args = parser.parse_args(argv)
    from init_options import OptionsError
    try:
        if args.command == "init":
            from init_ui import prepare_init
            args = prepare_init(args, sys.modules[__name__])
            if args is None:
                print("Initialization cancelled. No setup actions were run.")
                return 0
        if args.command != "debug" and str(getattr(args, "preset", "")).startswith("web-emscripten-"):
            from web_build import command
            return command(args, sys.modules[__name__])
        return int(args.func(args))
    except (EngineError, OptionsError) as exc:
        sys.stdout.flush()
        print(f"error: {exc}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
