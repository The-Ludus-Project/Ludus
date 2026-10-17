"""Pinned LLVM toolchain identity for the redistributable host tooling.

The ludus-tools wheel runs without a Ludus engine checkout, so it cannot read
``config/tool_versions.json``. ``PINNED_LLVM_MAJOR`` mirrors that pin's
``minimum.clang`` major; ``test_ci.ToolchainPinTests`` fails when they
disagree, so a toolchain bump changes the JSON pin and this constant together.
"""

from __future__ import annotations

PINNED_LLVM_MAJOR = 23

# Ubuntu 24.04 does not package the pinned LLVM; generated Linux release
# workflows install it from apt.llvm.org after checking this published archive
# key fingerprint. ``test_ci.ToolchainPinTests`` keeps it equal to
# ``llvm_apt_source.KEY_FINGERPRINT``, which checks the engine's committed key.
LLVM_APT_KEY_FINGERPRINT = "6084F3CF814B57C1CF12EFD515CF4D18AF4F7421"


def pinned_llvm_tool(tool: str) -> str:
    """Return the versioned executable name of a pinned LLVM tool, such as ``clang++-<major>``."""
    return f"{tool}-{PINNED_LLVM_MAJOR}"

