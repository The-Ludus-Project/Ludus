"""Native game profiles shared by installed tooling and its Editor adapter."""
from __future__ import annotations

import platform

from .errors import INVALID_PROJECT, ToolingError

WINDOWS_TARGET = "x86_64-pc-windows-msvc"
WINDOWS_CONFIGURATIONS = {
    "Debug": "Debug", "Development": "RelWithDebInfo", "Profile": "RelWithDebInfo", "Release": "Release",
}

PRESET_FLAVOR = {
    f"{family}-clang-{flavor.lower()}": flavor
    for family in ("linux", "macos")
    for flavor in ("Debug", "Development", "Release")
}
PRESET_FLAVOR.update({f"windows-msvc-{flavor.lower()}": flavor for flavor in WINDOWS_CONFIGURATIONS})


def default_profile() -> str:
    system = platform.system()
    if system == "Windows":
        return "windows-msvc-development"
    if system not in ("Darwin", "Linux"):
        raise ToolingError(INVALID_PROJECT, f"unsupported native tooling host {system!r}")
    return ("macos" if system == "Darwin" else "linux") + "-clang-development"


def target_for_profile(profile: str) -> str:
    if profile not in PRESET_FLAVOR:
        raise ToolingError(INVALID_PROJECT, f"unknown native profile {profile!r}")
    if profile.startswith("linux-"):
        return "x86_64-linux-gnu"
    if profile.startswith("windows-msvc-"):
        return WINDOWS_TARGET
    architecture = platform.machine().lower()
    if architecture in ("arm64", "aarch64"):
        return "arm64-apple-darwin"
    if architecture in ("x86_64", "amd64"):
        return "x86_64-apple-darwin"
    raise ToolingError(INVALID_PROJECT, f"unsupported macOS architecture {architecture!r}")


def profile_for_identity(identity) -> str:
    if identity.target_triple in ("arm64-apple-darwin", "x86_64-apple-darwin"):
        family = "macos-clang"
    elif identity.target_triple == "x86_64-linux-gnu":
        family = "linux-clang"
    elif identity.target_triple == WINDOWS_TARGET:
        family = "windows-msvc"
    else:
        raise ToolingError(INVALID_PROJECT, f"unsupported native SDK target {identity.target_triple!r}")
    profile = f"{family}-{identity.flavor.lower()}"
    if profile not in PRESET_FLAVOR:
        raise ToolingError(INVALID_PROJECT, f"unsupported native SDK flavor {identity.flavor!r}")
    return profile


def validate_target(identity, profile: str) -> None:
    expected = target_for_profile(profile)
    if identity.target_triple != expected:
        raise ToolingError(INVALID_PROJECT, f"SDK target: expected {expected!r}, got {identity.target_triple!r}")
    if identity.flavor != PRESET_FLAVOR[profile]:
        raise ToolingError(INVALID_PROJECT, f"SDK flavor: expected {PRESET_FLAVOR[profile]!r}, got {identity.flavor!r}")
    if expected == WINDOWS_TARGET and (identity.compiler_id != "MSVC" or
                                       identity.cxx_runtime_abi != "msvc-stl" or
                                       not identity.compiler_version.startswith("19.44.")):
        raise ToolingError(INVALID_PROJECT, "Select a Windows x64 SDK built with MSVC 14.44 and the Microsoft STL")
    if profile.startswith("macos-") and identity.cxx_runtime_abi != "libc++":
        raise ToolingError(INVALID_PROJECT, f"macOS SDK requires libc++; got {identity.cxx_runtime_abi!r}")
