"""Persist local target selection for scripts and direct CMake/IDE builds."""
from __future__ import annotations

import json
from pathlib import Path

from init_denoiser import preset_supports


VARIABLES = {
    "with_tests": "LUDUS_BUILD_TESTS",
    "with_smoke_app": "LUDUS_BUILD_SMOKE_APP",
    "with_web_probes": "LUDUS_BUILD_WEB_PROBES",
    "with_editor": "LUDUS_BUILD_EDITOR",
    "with_editor_preview": "LUDUS_EDITOR_RENDER_PREVIEW",
    "with_shader_probe": "LUDUS_BUILD_SHADER_PROBE",
    "with_denoiser": "LUDUS_ENABLE_OIDN",
}


class OptionsError(RuntimeError):
    """Saved local target choices could not be read safely."""


def options_path(root: Path) -> Path:
    return root / "out" / "init" / "options.json"


def read_all_options(root: Path) -> dict[str, dict[str, bool]]:
    path = options_path(root)
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise OptionsError(f"Cannot read saved setup choices at {path}: {error}") from error
    if not isinstance(data, dict) or any(
        not isinstance(options, dict) or any(variable not in VARIABLES.values() or not isinstance(value, bool)
                                            for variable, value in options.items())
        for options in data.values()
    ):
        raise OptionsError(f"Invalid saved setup choices at {path}; expected preset names and boolean target options")
    return data


def read_options(root: Path, preset: str) -> dict[str, bool]:
    return read_all_options(root).get(preset, {})


def save_options(root: Path, presets, args) -> None:
    path = options_path(root)
    data = read_all_options(root)
    for preset in presets:
        data[preset] = {variable: bool(getattr(args, name)) for name, variable in VARIABLES.items()
                        if name != "with_denoiser"}
        # Enable Qt only for the explicitly selected supported editor preset.
        data[preset]["LUDUS_BUILD_EDITOR"] = args.with_editor and preset == args.preset
        data[preset]["LUDUS_EDITOR_RENDER_PREVIEW"] = data[preset]["LUDUS_BUILD_EDITOR"] and args.with_editor_preview
        # Init installs OIDN only where its Metal device exists; other presets build without it.
        data[preset]["LUDUS_ENABLE_OIDN"] = bool(getattr(args, "with_denoiser", False)) and preset_supports(preset)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)
