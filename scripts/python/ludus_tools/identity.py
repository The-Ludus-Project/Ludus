"""SDK identity and manifest model (P03/P11).

SDK identity is derived from the *actual build inputs*, never inferred from a
version string. Two SDKs are compatible for a given project operation only when
their identity agrees on the fields that affect ABI and policy: target triple,
compiler id/version, C++ runtime ABI tag, flavor, the ``LUDUS_SDK_VARIANT``
compatibility key, assertion policy and the feature set.

This module parses and validates an installed/candidate SDK manifest
(``share/Ludus/LudusSdkManifest.json`` inside a prefix) into a typed
``SdkIdentity`` and provides a precise compatibility check that returns the
mismatching fields (expected vs actual) rather than a bare bool.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

from .errors import MANIFEST_INVALID, SDK_INCOMPATIBLE, ToolingError

MANIFEST_RELPATH = "share/Ludus/LudusSdkManifest.json"
MAX_MANIFEST_BYTES = 256 * 1024


@dataclass
class AssertPolicy:
    policy_version: int
    enable_asserts: bool
    break_on_check: bool
    dialogs_available: bool

    @staticmethod
    def from_json(obj: dict) -> "AssertPolicy":
        return AssertPolicy(
            policy_version=int(obj["policy_version"]),
            enable_asserts=bool(obj["enable_asserts"]),
            break_on_check=bool(obj["break_on_check"]),
            dialogs_available=bool(obj["dialogs_available"]),
        )

    def to_json(self) -> dict:
        return {
            "policy_version": self.policy_version,
            "enable_asserts": self.enable_asserts,
            "break_on_check": self.break_on_check,
            "dialogs_available": self.dialogs_available,
        }


@dataclass
class SdkIdentity:
    """Typed view of an installed/candidate SDK manifest."""

    schema_version: int
    name: str
    engine_version: str
    source_revision: str
    target_triple: str
    compiler_id: str
    compiler_version: str
    cxx_runtime_abi: str
    distro_baseline: str
    flavor: str
    sdk_variant: str
    cxx_standard: str
    library_type: str
    features: list[str]
    assert_policy: AssertPolicy
    package_format: str
    components: list[str] = field(default_factory=list)
    dependencies: list[dict] = field(default_factory=list)
    system_prerequisites: list[str] = field(default_factory=list)

    # Identity fields that must match exactly for binary compatibility. Notably
    # engine_version is NOT here on its own: it is advisory; the variant/compiler
    # fields are what actually gate ABI. source_revision is recorded, not gated.
    ABI_FIELDS = (
        "target_triple",
        "compiler_id",
        "compiler_version",
        "cxx_runtime_abi",
        "flavor",
        "sdk_variant",
        "cxx_standard",
    )

    @staticmethod
    def from_json(obj: dict) -> "SdkIdentity":
        if not isinstance(obj, dict):
            raise ToolingError(MANIFEST_INVALID, "manifest must be a JSON object")
        try:
            return SdkIdentity(
                schema_version=int(obj.get("schema_version", 1)),
                name=str(obj["name"]),
                engine_version=str(obj["version"]),
                source_revision=str(obj.get("source_revision", "")),
                target_triple=str(obj.get("target_triple", "")),
                compiler_id=str(obj.get("compiler_id", "")),
                compiler_version=str(obj.get("compiler_version", "")),
                cxx_runtime_abi=str(obj.get("cxx_runtime_abi", "")),
                distro_baseline=str(obj.get("distro_baseline", "")),
                flavor=str(obj["build_flavor"]),
                sdk_variant=str(obj["sdk_variant"]),
                cxx_standard=str(obj.get("cxx_standard", "")),
                library_type=str(obj.get("library_type", "")),
                features=sorted(str(f) for f in obj.get("features", [])),
                assert_policy=AssertPolicy.from_json(_assert_block(obj)),
                package_format=str(obj.get("package_format", "")),
                components=sorted(str(c) for c in obj.get("components", [])),
                dependencies=list(obj.get("dependencies", [])),
                system_prerequisites=sorted(str(p) for p in obj.get("system_prerequisites", [])),
            )
        except KeyError as exc:
            raise ToolingError(MANIFEST_INVALID, f"manifest missing required field {exc}") from exc

    def abi_key(self) -> dict:
        return {name: getattr(self, name) for name in self.ABI_FIELDS}


def _assert_block(obj: dict) -> dict:
    """Accept both the extended nested policy block and the legacy flat fields."""
    if isinstance(obj.get("assert_policy"), dict):
        return obj["assert_policy"]
    # Legacy flat manifest (pre-schema-2) shape.
    return {
        "policy_version": obj.get("assert_policy_version", 0),
        "enable_asserts": obj.get("enable_asserts", 0),
        "break_on_check": obj.get("break_on_check", 0),
        "dialogs_available": obj.get("assert_dialogs_available", 0),
    }


def load_manifest_file(path: Path) -> SdkIdentity:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise ToolingError(MANIFEST_INVALID, f"cannot read SDK manifest: {exc}") from exc
    if len(data) > MAX_MANIFEST_BYTES:
        raise ToolingError(MANIFEST_INVALID, "SDK manifest exceeds size bound")
    try:
        obj = json.loads(data.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise ToolingError(MANIFEST_INVALID, f"malformed SDK manifest: {exc}") from exc
    return SdkIdentity.from_json(obj)


def load_prefix_manifest(prefix: Path) -> SdkIdentity:
    """Load the manifest from an installed SDK prefix."""
    path = prefix / MANIFEST_RELPATH
    if not path.is_file():
        raise ToolingError(MANIFEST_INVALID, f"no SDK manifest at {path}")
    return load_manifest_file(path)


@dataclass
class CompatMismatch:
    field_name: str
    expected: Any
    actual: Any


def check_compatible(
    installed: SdkIdentity,
    *,
    required_flavor: Optional[str] = None,
    required_features: Optional[list[str]] = None,
    reference: Optional[SdkIdentity] = None,
) -> list[CompatMismatch]:
    """Return the list of mismatching fields (empty when compatible).

    ``reference`` compares ABI fields against another identity (e.g. the Editor's
    own libraries, or a previously resolved SDK). ``required_flavor`` and
    ``required_features`` express the project's requested policy/features.
    """
    mismatches: list[CompatMismatch] = []
    if required_flavor is not None and installed.flavor != required_flavor:
        mismatches.append(CompatMismatch("flavor", required_flavor, installed.flavor))
    if required_features:
        missing = sorted(set(required_features) - set(installed.features))
        if missing:
            mismatches.append(CompatMismatch("features", required_features, installed.features))
    if reference is not None:
        for name in SdkIdentity.ABI_FIELDS:
            exp = getattr(reference, name)
            act = getattr(installed, name)
            if exp != act:
                mismatches.append(CompatMismatch(name, exp, act))
    return mismatches


def require_compatible(installed: SdkIdentity, **kwargs: Any) -> None:
    mismatches = check_compatible(installed, **kwargs)
    if mismatches:
        detail = "; ".join(
            f"{m.field_name}: expected {m.expected!r}, got {m.actual!r}" for m in mismatches
        )
        raise ToolingError(SDK_INCOMPATIBLE, f"SDK is not compatible: {detail}")


# --- Host toolchain detection ------------------------------------------------
# A partial identity describing the CONSUMING machine's toolchain, used as the
# ``reference`` in resolution so an SDK built with an incompatible compiler /
# C++ runtime ABI is rejected before configure (P03). Only the ABI-relevant
# fields are populated; the rest are left as the SDK's own values so they do not
# spuriously mismatch (the gate compares the ABI fields that actually matter).

_RUNTIME_ABI_FALLBACK = "libstdc++-cxx11"


def detect_msvc_version(cxx: str, env: dict) -> str:
    """Read the MSVC banner. cl without sources emits it and exits with an error."""
    import re
    import subprocess

    try:
        result = subprocess.run([cxx], capture_output=True, text=True, errors="replace",
                                timeout=10, env=dict(env, VSLANG="1033"))
    except (OSError, subprocess.SubprocessError):
        return ""
    match = re.search(r"Microsoft[^\r\n]*?(\d+\.\d+\.\d+)(?:\.\d+)? for x64",
                      result.stdout + result.stderr)
    return match.group(1) if match else ""


def _detect_compiler(cxx: str) -> tuple[str, str]:
    """Return (compiler_id, compiler_version) for a C++ compiler, best-effort."""
    import re
    import subprocess

    try:
        import os

        env = {key: value for key, value in os.environ.items() if key != "BUTLER_API_KEY"}
        out = subprocess.run(
            [cxx, "--version"], capture_output=True, text=True, timeout=10, env=env
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return ("", "")
    lowered = out.lower()
    if "clang" in lowered:
        compiler_id = "Clang"
    elif "g++" in lowered or "gcc" in lowered or "free software foundation" in lowered:
        compiler_id = "GNU"
    else:
        compiler_id = ""
    match = re.search(r"(\d+\.\d+\.\d+)", out)
    version = match.group(1) if match else ""
    return (compiler_id, version)


def detect_host_toolchain(
    reference: SdkIdentity, *, cxx: Optional[str] = None, strict: bool = False
) -> SdkIdentity:
    """Build a host-toolchain reference identity for compatibility checking.

    ``reference`` supplies the non-ABI fields (so only ABI fields can mismatch).
    ``cxx`` is the C++ compiler to probe (defaults to $CXX, then ``clang++``,
    then ``c++``). The runtime ABI is taken from $LUDUS_CXX_RUNTIME_ABI when set,
    else inferred to match the reference's family. When the compiler cannot be
    probed, the fields are left equal to the reference so detection failure never
    produces a spurious rejection.
    """
    import os
    import shutil

    candidate = cxx or os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++")
    compiler_id, compiler_version = ("", "")
    from .native import WINDOWS_TARGET

    if reference.target_triple == WINDOWS_TARGET:
        candidate = cxx or shutil.which("cl.exe")
        env = {key: value for key, value in os.environ.items() if key != "BUTLER_API_KEY"}
        if candidate:
            compiler_version = detect_msvc_version(candidate, env)
            if compiler_version:
                compiler_id = "MSVC"
                # The package admits the pinned 14.44 series. CMake and the
                # cl banner can also represent the patch with different precision.
                if compiler_version.startswith("19.44.") and reference.compiler_version.startswith("19.44."):
                    compiler_version = reference.compiler_version
    elif candidate:
        compiler_id, compiler_version = _detect_compiler(candidate)
    if strict and (not compiler_id or not compiler_version):
        raise ToolingError(SDK_INCOMPATIBLE, "cannot identify the selected C++ compiler; prepare the pinned toolchain")

    runtime_abi = os.environ.get("LUDUS_CXX_RUNTIME_ABI", "")
    if reference.target_triple.endswith("-apple-darwin"):
        runtime_abi = runtime_abi or "libc++"

    # Clone the reference and overlay only the detected ABI-relevant fields.
    host = SdkIdentity.from_json(
        {
            "name": reference.name,
            "version": reference.engine_version,
            "source_revision": reference.source_revision,
            "target_triple": reference.target_triple,
            "compiler_id": compiler_id or reference.compiler_id,
            "compiler_version": compiler_version or reference.compiler_version,
            "cxx_runtime_abi": runtime_abi or reference.cxx_runtime_abi,
            "distro_baseline": reference.distro_baseline,
            "build_flavor": reference.flavor,
            "sdk_variant": reference.sdk_variant,
            "cxx_standard": reference.cxx_standard,
            "library_type": reference.library_type,
            "features": reference.features,
            "assert_policy": reference.assert_policy.to_json(),
            "package_format": reference.package_format,
        }
    )
    return host
