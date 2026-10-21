"""Shared shader dependency selection for project readiness and offline graph builds.

Only explicit setup/repair acquires tools. Manifest inspection and pin checks are read-only.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path

from .errors import ToolingError
from .render_graph_ray import TECHNIQUE as RAY_TECHNIQUE

REPAIR_MESSAGE = ("Rendering tools need preparation. Choose Project > Repair Project Setup "
                  "to prepare them automatically.")


def required_shader_tools(manifest: dict) -> set[str]:
    """Dependencies of a validated graph build manifest, including native Metal ray queries."""
    profiles = manifest["profiles"]
    required = {"compiler"}
    if any(profile in profiles for profile in ("spirv", "glsl")):
        required.add("validator")
    has_native_rays = any(technique.get("id") == RAY_TECHNIQUE for technique in manifest["techniques"])
    if "glsl" in profiles or ("metal" in profiles and has_native_rays):
        required.add("spirv_cross")
    return required


def preparation_steps(root: Path, required: set[str]):
    """Trusted, cancellable setup steps; callers must never use this during ordinary builds."""
    python = root / "out/host-tools/venv/bin/python"
    for tools, script, arguments in (({"compiler", "validator"}, "shader-probe", ["bootstrap"]),
                                     ({"spirv_cross"}, "bootstrap-spirv-cross", [])):
        selected = required & tools
        if selected:
            yield selected, [str(python), str(root / "scripts" / script), *arguments]


def project_shader_tools(project: Path, source: Path, profile: str, has_web_sdk: bool) -> set[str]:
    """Inspect saved editor graph packages and existing CMake shader consumers without execution.

    Invalid/oversized graph drafts remain the graph validator's responsibility. Discovery uses
    the Editor's graphs/<package> layout and never follows a package/manifest outside the project.
    """
    from .render_graph_build import MAX_FILE, document, read, validate_manifest

    required = set()
    cmake_files = [source / "CMakeLists.txt", *(source / "cmake").glob("*.cmake")]
    if any("ludus_compile_shader" in file.read_text() for file in cmake_files):
        required.add("compiler")
        if not profile.startswith("macos-"):
            required.add("validator")
    graphs = project / "graphs"
    for manifest in sorted(graphs.glob("*/*.lushader.json")):
        if not manifest.resolve().is_relative_to(project.resolve()):
            continue
        try:
            value = document(read(manifest, MAX_FILE), str(manifest))
            validate_manifest(value)
        except (ToolingError, OSError):
            continue
        required.update(required_shader_tools(value))
    if has_web_sdk:
        required.add("spirv_cross")
        if "compiler" in required:
            required.add("validator")
    return required


def verify_translator(binary: Path, pin_path: Path) -> None:
    """Check the installed SDK's exact source pin and verified binary hash without running it."""
    try:
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError("translator is not executable")
        pin = json.loads(pin_path.read_text())["spirv_cross"]
        stamp = Path(str(binary.resolve()) + ".build.json")
        built = json.loads(stamp.read_text())
        if (any(built.get(key) != pin[key] for key in ("tag", "commit", "source_sha256"))
                or built.get("binary_sha256") != hashlib.sha256(binary.read_bytes()).hexdigest()):
            raise ValueError("translator differs from the selected SDK pin")
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise ToolingError("MissingTools", REPAIR_MESSAGE) from error
