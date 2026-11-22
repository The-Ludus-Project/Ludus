"""Locations of the checkout-local Android tools used by setup and probes."""
from __future__ import annotations

import json
import re
from pathlib import Path


def read_android_pins(root: Path) -> dict:
    return json.loads((root / "config/android_toolchain.json").read_text(encoding="utf-8"))


def managed_sdk(root: Path) -> Path:
    return root / "out/host-tools/android-sdk"


def managed_java_home(root: Path, pin: dict, system: str) -> Path:
    prefix = root / "out/host-tools/android-jdk" / pin["version"]
    return prefix / "Contents/Home" if system == "Darwin" else prefix


def read_property(path: Path, name: str) -> str | None:
    if not path.is_file():
        return None
    match = re.search(rf"^[ \t]*{re.escape(name)}[ \t]*=[ \t]*([^\r\n]+)", path.read_text(encoding="utf-8"), re.MULTILINE)
    return match.group(1).strip().strip('"') if match else None


def read_revision(directory: Path) -> str | None:
    return read_property(directory / "source.properties", "Pkg.Revision")


def existing_sdk_roots(home: Path, environment: dict[str, str]) -> list[Path]:
    configured = [Path(environment[name]) for name in ("ANDROID_HOME", "ANDROID_SDK_ROOT") if environment.get(name)]
    candidates = [*configured, home / "Library/Android/sdk", home / "Android/Sdk"]
    return list(dict.fromkeys(path.resolve() for path in candidates if path.is_dir()))


def resolve_sdk(root: Path, home: Path, environment: dict[str, str]) -> Path | None:
    local_sdk = managed_sdk(root)
    if (local_sdk / "platform-tools/adb").is_file():
        return local_sdk
    return next(iter(existing_sdk_roots(home, environment)), None)
