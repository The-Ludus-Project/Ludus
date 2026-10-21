"""Automatic engine selection for explicit, staged CLI/Editor creation.

Discovery reads only named locations; preparation runs only after Create.
An explicit SDK/environment override is never silently replaced on failure.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path

from .errors import INVALID_PROJECT, ToolingError
from .identity import load_manifest_file, load_prefix_manifest

from .native import default_profile, validate_target

PROFILE = default_profile()


def prepared_sdk_prefix(root: Path, profile: str | None = None) -> Path:
    profile = profile or PROFILE
    # Separate ABI variants without removing SDKs used by existing projects.
    # Hash the build's actual identity, never a guessed assertion/toolchain key.
    identity = load_manifest_file(root / "out/build" / profile / "cmake/LudusSdkManifest.json")
    key = json.dumps(identity.abi_key(), sort_keys=True).encode("utf-8")
    suffix = hashlib.sha256(key).hexdigest()[:16]
    return root / "out/install" / f"{profile}-{suffix}"


def validate_creation_sdk(prefix: Path, profile: str | None = None):
    identity = load_prefix_manifest(prefix)
    validate_target(identity, profile or PROFILE)
    if "FoundationBase" not in identity.components:
        raise ToolingError(INVALID_PROJECT, "Select a matching native SDK containing FoundationBase")
    return identity


def source_revision(root: Path) -> str:
    # Trusted editor/CLI tooling checkout, never a game-owned script or hook.
    try:
        result = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"],
                                capture_output=True, text=True, timeout=10, check=False)
        return result.stdout.strip() if result.returncode == 0 else ""
    except (OSError, subprocess.TimeoutExpired):
        return ""


def select_creation_sdk(root: Path, *, explicit: Path | None = None,
                        prepare=None, environ=None, profile: str | None = None) -> Path:
    """Explicit > environment > current editor install > prepare editor engine.

    ABI/compiler and real configure/build/test validation still run in repair;
    discovery alone never reports a project ready or publishes a destination.
    """
    profile = profile or PROFILE
    env = os.environ if environ is None else environ
    override = env.get("LUDUS_SDK_PREFIX", "").strip()
    if explicit is not None or override:
        prefix = Path(explicit) if explicit is not None else Path(override)
        if explicit is None and not prefix.is_absolute():
            raise ToolingError(INVALID_PROJECT, "LUDUS_SDK_PREFIX must be an absolute SDK directory")
        prefix = prefix.resolve()
        validate_creation_sdk(prefix, profile)
        return prefix
    root = root.resolve()
    prefix = root / "out/install" / profile
    try:
        cmake = (root / "CMakeLists.txt").read_text()
    except OSError as exc:
        raise ToolingError("MissingTools", "Cannot identify the editor engine checkout") from exc
    version = re.search(r"project\([^\n]*\bVERSION\s+([0-9.]+)", cmake)
    if not version:
        raise ToolingError("MissingTools", "Cannot identify the editor engine version")
    revision = source_revision(root)
    candidates = [prefix]
    if (root / "out/build" / profile / "cmake/LudusSdkManifest.json").is_file():
        candidates.insert(0, prepared_sdk_prefix(root, profile))
    for candidate in candidates:
        try:
            identity = validate_creation_sdk(candidate, profile)
            if (identity.engine_version == version.group(1)
                    and revision and identity.source_revision == revision):
                return candidate
        except ToolingError:
            pass  # A missing/stale editor-owned install is prepared automatically.
    if prepare is None:
        raise ToolingError("MissingTools", "The editor engine needs preparation; use Create with prepared tooling")
    prefix = prepare(prefix) or prefix
    identity = validate_creation_sdk(prefix, profile)
    if (identity.engine_version != version.group(1)
            or (revision and identity.source_revision != revision)):
        raise ToolingError("SdkIncompatible", "Prepared engine does not match the editor tooling checkout")
    return prefix


def prepare_creation_sdk(root: Path, prefix: Path, *, runner, cancel_check=None,
                         profile: str | None = None, environ=None):
    """Prepare explicitly, using a caller-selected environment when supplied.

    The Editor supplies its managed Apple SDK policy; CLI callers retain their
    shell overrides. Copy the environment without changing the caller's mapping.
    No system installs or project hooks run.
    """
    profile = profile or PROFILE
    from .project_setup import environment
    env = environment(root) if environ is None else dict(environ)
    env["CI"] = "true"
    from .shader_tools import preparation_steps
    # Metal ray recipes may be configured while preparing the engine itself, before a
    # project or installed SDK exists. Use the trusted checkout's pinned bootstrap.
    rendering = ([command for _, command in preparation_steps(root, {"compiler", "spirv_cross"})]
                 if profile.startswith("macos-") else [])
    commands = ([str(root / "scripts/init"), profile, "--preset-only", "--cli", "--no-system-install"],
                *rendering, [str(root / "scripts/build"), profile])
    for argv in commands:
        if cancel_check:
            cancel_check()
        runner(argv, cwd=root, env=env)
    if cancel_check:
        cancel_check()
    # Configuration can change the variant, so choose the installation only
    # after building. Keep the old profile prefix intact, including legacy SDKs.
    prefix = prepared_sdk_prefix(root, profile)
    runner([str(root / "out/host-tools/venv/bin/cmake"), "--install",
            str(root / "out/build" / profile), "--prefix", str(prefix)], cwd=root, env=env)
    if cancel_check:
        cancel_check()
    runner([str(root / "out/host-tools/venv/bin/python"), str(root / "scripts/python/engine.py"),
            "bundle-sdk-dependencies", profile, "--prefix", str(prefix)], cwd=root, env=env)
    if cancel_check:
        cancel_check()
    return prefix
