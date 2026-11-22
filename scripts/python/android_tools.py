"""Locations of the checkout-local Android tools used by setup and probes."""
from __future__ import annotations

import json
from pathlib import Path


def read_android_pins(root: Path) -> dict:
    return json.loads((root / "config/android_toolchain.json").read_text(encoding="utf-8"))


def managed_sdk(root: Path) -> Path:
    return root / "out/host-tools/android-sdk"


def managed_java_home(root: Path, pin: dict, system: str) -> Path:
    prefix = root / "out/host-tools/android-jdk" / pin["version"]
    return prefix / "Contents/Home" if system == "Darwin" else prefix


def resolve_sdk(root: Path, home: Path, environment: dict[str, str]) -> Path | None:
    for variable in ("ANDROID_HOME", "ANDROID_SDK_ROOT"):
        value = environment.get(variable)
        if value and Path(value).is_dir():
            return Path(value)
    local_sdk = managed_sdk(root)
    if (local_sdk / "platform-tools/adb").is_file():
        return local_sdk
    for candidate in (home / "Library/Android/sdk", home / "Android/Sdk"):
        if candidate.is_dir():
            return candidate
    return None
