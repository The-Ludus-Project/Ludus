"""Build the host luslc that cross builds lower LUSL shader sources with.

An Emscripten build compiles luslc for the browser, so ``ludus_compile_shader`` takes a host LUSL
compiler through ``LUDUS_LUSLC`` (cmake/LudusShaders.cmake), as it takes ``LUDUS_SLANG_COMPILER``.
This module builds that compiler natively from the same checkout with the pinned host tools
(``LUDUS_LUSLC_ONLY``). Every call runs the incremental build, so a change to the LUSL front end
reaches the next cross build; an up-to-date tree costs one Ninja no-op. Nothing is downloaded.
"""
from __future__ import annotations

import json
import os
import platform
from pathlib import Path
import subprocess

BUILD = Path("out/build/host-luslc")


class HostLuslcError(RuntimeError):
    """The host luslc cannot be configured or built; the message names the repair."""


def binary_path(root: Path) -> Path:
    """Return where ``ensure`` places the host luslc in ``root``'s build tree."""
    suffix = ".exe" if platform.system() == "Windows" else ""
    return root / BUILD / "tools/luslc" / ("luslc" + suffix)


def configure_arguments(root: Path) -> list[str]:
    """Return the CMake configure command for the luslc-only host tree."""
    tools = root / "out/host-tools"
    cmake = tools / "venv/bin/cmake"
    ninja = tools / "venv/bin/ninja"
    system = platform.system()
    if system not in ("Linux", "Darwin"):
        raise HostLuslcError("The host luslc bootstrap supports Linux and macOS hosts; on Windows, "
                             "build tools/luslc with a native preset and set LUDUS_LUSLC to it.")
    for path in (cmake, ninja, tools / "bin/clang++", tools / "bin/clang"):
        if not path.is_file():
            raise HostLuslcError(f"Missing pinned host tool {path}; run ./init.sh first.")
    arguments = [str(cmake), "-S", str(root), "-B", str(root / BUILD), "-G", "Ninja",
                 f"-DCMAKE_MAKE_PROGRAM={ninja}", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DCMAKE_CXX_COMPILER={tools / 'bin/clang++'}", f"-DCMAKE_C_COMPILER={tools / 'bin/clang'}",
                 "-DLUDUS_BUILD_FLAVOR=Release", "-DLUDUS_LUSLC_ONLY=ON", "-DLUDUS_BUILD_TESTS=OFF",
                 "-DLUDUS_USE_INIT_OPTIONS=OFF"]
    if system == "Darwin":
        # The macOS presets' pinned SDK and libc++ headers (CMakePresets.json, macos-clang-base).
        flags = f'-stdlib=libc++ -nostdinc++ -isystem "{tools / "libcxx-include"}"'
        arguments += [f"-DCMAKE_OSX_SYSROOT={tools / 'macos-sdk'}", "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0",
                      f"-DCMAKE_CXX_FLAGS={flags}", f"-DCMAKE_OBJCXX_COMPILER={tools / 'bin/clang++'}",
                      f"-DCMAKE_OBJCXX_FLAGS={flags}"]
    return arguments


def host_environment() -> dict[str, str]:
    """Return the caller's environment without cross-compiler selections (emsdk sets none today)."""
    environment = dict(os.environ)
    for name in ("CC", "CXX", "AR", "RANLIB", "LDFLAGS", "CFLAGS", "CXXFLAGS"):
        environment.pop(name, None)
    return environment


def ensure(root: Path) -> Path:
    """Configure once, then incrementally build the host luslc; return its absolute path."""
    root = root.resolve()
    environment = host_environment()
    # Configure again whenever the recorded arguments differ, including after a failed configure.
    arguments = configure_arguments(root)
    stamp = root / BUILD / "ludus-host-luslc.json"
    try:
        is_configured = json.loads(stamp.read_text()) == arguments
    except (OSError, ValueError):
        is_configured = False
    if not is_configured:
        stamp.unlink(missing_ok=True)
        result = subprocess.run(arguments, cwd=root, env=environment)
        if result.returncode != 0:
            raise HostLuslcError(f"Host luslc configure failed in {root / BUILD}; fix the error above "
                                 f"and rerun ./scripts/bootstrap-luslc.")
        stamp.write_text(json.dumps(arguments, indent=2) + "\n")
    cmake = root / "out/host-tools/venv/bin/cmake"
    result = subprocess.run([str(cmake), "--build", str(root / BUILD), "--target", "luslc"],
                            cwd=root, env=environment)
    path = binary_path(root)
    if result.returncode != 0 or not path.is_file():
        raise HostLuslcError(f"Host luslc build failed in {root / BUILD}; fix the error above and rerun "
                             f"./scripts/bootstrap-luslc.")
    return path
