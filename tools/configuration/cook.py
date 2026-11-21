#!/usr/bin/env python3
"""Cook bounded TOML through the engine's actual schema and validator.

Python 3.11+ is required for this optional authoring tool (stdlib tomllib).
The runtime consumes cooked JSON. Pass --profile to cook an explicit startup
manifest instead of a single Project or Preference file.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile

MAX_BYTES = 262144
MAX_FILES_PER_LAYER = 32
STARTUP_VERSION = 1
SELECTION_NAME = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}")


def flatten(table: dict, prefix: str = "", depth: int = 0) -> dict:
    if depth > 16:
        raise ValueError("TOML table depth exceeds 16")
    values = {}
    for name, value in table.items():
        key = f"{prefix}.{name}" if prefix else name
        if isinstance(value, dict):
            nested = flatten(value, key, depth + 1)
            if values.keys() & nested.keys():
                raise ValueError("duplicate flattened setting")
            values.update(nested)
        else:
            if key in values:
                raise ValueError(f"duplicate flattened setting: {key}")
            values[key] = value
    return values


def read_toml(source: Path) -> dict:
    try:
        import tomllib
    except ImportError as error:
        raise ValueError("TOML cooking requires Python 3.11+; use a newer interpreter") from error
    with source.open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError(f"{source}: TOML input exceeds {MAX_BYTES} bytes")
    try:
        return tomllib.loads(data.decode("utf-8"))
    except ValueError as error:
        raise ValueError(f"{source}: {error}") from error


def read_schema(validator: Path) -> dict:
    invocation = subprocess.run([str(validator), "--schema"], capture_output=True, check=True, timeout=15)
    return json.loads(invocation.stdout)


def encode(document: dict) -> bytes:
    encoded = json.dumps(document, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(encoded) > MAX_BYTES:
        raise ValueError(f"cooked configuration exceeds {MAX_BYTES} bytes; reduce its settings or source paths")
    return encoded


def cook_layer(sources: list[tuple[Path, str]], validator: Path, schema: dict, layer: str) -> bytes:
    descriptors = {setting["key"]: setting for setting in schema["settings"]}
    values = {}
    for source, origin in sorted(sources, key=lambda file: file[1]):
        for key, value in flatten(read_toml(source)).items():
            if key in values:
                previous_origin = values[key][1]
                raise ValueError(f"duplicate {layer} setting '{key}' in '{previous_origin}' and '{origin}'; "
                                 "keep one assignment in this layer")
            if key not in descriptors:
                raise ValueError(f"{origin}: unknown setting '{key}'; check ludus-config --schema")
            values[key] = (value, origin)
    records = []
    for key, (value, origin) in sorted(values.items()):
        descriptor = descriptors[key]
        kind = descriptor["type"]
        if layer == "preference" and not descriptor["persistent"]:
            raise ValueError(f"{origin}: setting {key} cannot be stored in preferences; check ludus-config --schema")
        if kind == "bool":
            valid = type(value) is bool
        elif kind in ("int32", "uint32", "int64", "uint64"):
            valid = type(value) is int
            if valid and kind in ("int64", "uint64"):
                value = str(value)
        elif kind in ("float32", "float64"):
            valid = type(value) is int or (type(value) is float and math.isfinite(value))
        else:
            valid = type(value) is str
        if not valid:
            raise ValueError(f"{origin}: wrong TOML type for {key}; use {kind}")
        records.append({"key": key, "type": kind, "value": value, "source": origin, "line": 0})
    bundle = {"version": 1, "schema": schema["schema"], "layer": layer, "values": records}
    validated = subprocess.run([str(validator), "--validate", layer], input=encode(bundle),
                               capture_output=True, timeout=15)
    if validated.returncode:
        details = validated.stderr.decode("utf-8", errors="replace").strip()
        origins = ", ".join(origin for _, origin in sources)
        raise ValueError(f"{layer} layer ({origins}): {details or 'engine validation failed'}; "
                         "check the assignments against ludus-config --schema")
    return validated.stdout


def cook(source: Path, validator: Path, layer: str) -> bytes:
    return cook_layer([(source, source.name)], validator, read_schema(validator), layer)


def manifest_files(value: object, location: str) -> list[str]:
    if type(value) is not list or len(value) > MAX_FILES_PER_LAYER:
        raise ValueError(f"{location} must list at most {MAX_FILES_PER_LAYER} source paths")
    paths = []
    for name in value:
        if (type(name) is not str or not name or any(character in name for character in ("\\", ":", "\0"))
                or any(part in ("", ".", "..") for part in name.split("/"))
                or PurePosixPath(name).suffix != ".toml"):
            raise ValueError(f"{location}: use explicit relative .toml paths beneath the manifest directory")
        paths.append(name)
    if len(set(paths)) != len(paths):
        raise ValueError(f"{location}: list each source file once")
    return paths


def named_files(value: object, location: str) -> dict[str, list[str]]:
    if type(value) is not dict or any(not SELECTION_NAME.fullmatch(name) for name in value):
        raise ValueError(f"{location}: use a table of named selections with letters, digits, hyphens or underscores")
    return {name: manifest_files(files, f"{location}.{name}") for name, files in value.items()}


def compatible_names(value: object, choices: dict, location: str) -> list[str]:
    if type(value) is not list or any(type(name) is not str or name not in choices for name in value):
        raise ValueError(f"{location}: list only names declared in the corresponding manifest table")
    if len(set(value)) != len(value):
        raise ValueError(f"{location}: list each compatible name once")
    return value


def cook_startup(manifest: Path, output: Path, validator: Path, profile: str,
                 device: str | None, quality: str | None) -> bytes:
    document = read_toml(manifest)
    if (document.keys() - {"version", "engine", "project", "profiles", "devices", "qualities"}
            or type(document.get("version")) is not int or document["version"] != STARTUP_VERSION):
        raise ValueError("startup manifest must use version = 1 and only documented fields")
    profiles = document.get("profiles")
    if type(profiles) is not dict or not profiles:
        raise ValueError("declare at least one profile in the startup manifest")
    devices = named_files(document.get("devices", {}), "devices")
    qualities = named_files(document.get("qualities", {}), "qualities")
    selected = {}
    all_files = set()
    for layer in ("engine", "project"):
        selected[layer] = manifest_files(document.get(layer, []), layer)
        all_files.update(selected[layer])
    for name, definition in profiles.items():
        if (not SELECTION_NAME.fullmatch(name) or type(definition) is not dict
                or "files" not in definition or definition.keys() - {"files", "devices", "qualities"}):
            raise ValueError(f"profiles.{name}: declare files and optional devices and qualities lists")
        files = manifest_files(definition["files"], f"profiles.{name}.files")
        allowed_devices = compatible_names(definition.get("devices", []), devices, f"profiles.{name}.devices")
        allowed_qualities = compatible_names(definition.get("qualities", []), qualities, f"profiles.{name}.qualities")
        all_files.update(files)
        if name == profile:
            selected["profile"] = files
            for layer, choice, candidates, allowed in (
                ("device", device, devices, allowed_devices),
                ("quality", quality, qualities, allowed_qualities),
            ):
                if choice is None:
                    continue
                if choice not in candidates:
                    raise ValueError(f"unknown {layer} '{choice}'; choose a declared {layer}")
                if choice not in allowed:
                    raise ValueError(f"{layer} '{choice}' is incompatible with profile '{profile}'; "
                                     f"choose from {allowed} or omit --{layer}")
                selected[layer] = candidates[choice]
    if profile not in profiles:
        raise ValueError(f"unknown profile '{profile}'; choose a profile declared in the manifest")
    for files in (*devices.values(), *qualities.values()):
        all_files.update(files)
    destination = output.resolve()
    if any((manifest.parent / name).resolve() == destination for name in all_files):
        raise ValueError("output must be separate from every declared TOML source")
    schema = read_schema(validator)
    layers = {}
    for layer, files in selected.items():
        if files:
            sources = [(manifest.parent / name, name) for name in files]
            layers[layer] = json.loads(cook_layer(sources, validator, schema, layer))
    return encode({"version": STARTUP_VERSION, "layers": layers})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--validator", required=True, type=Path, help="path to ludus-config from the matching SDK")
    parser.add_argument("--layer", choices=("project", "preference"), help="rank for a single TOML file (default: project)")
    parser.add_argument("--profile", action="append", help="cook the startup manifest using this named profile")
    parser.add_argument("--device", action="append", help="select a compatible named device")
    parser.add_argument("--quality", action="append", help="select a compatible named quality preset")
    args = parser.parse_args()
    for option in ("profile", "device", "quality"):
        if len(getattr(args, option) or []) > 1:
            parser.error(f"choose at most one --{option}")
    if args.profile is None and (args.device is not None or args.quality is not None):
        parser.error("--device and --quality require --profile")
    if args.profile is not None and args.layer is not None:
        parser.error("--layer applies to single-file cooking; omit it when using --profile")
    temporary = None
    try:
        if args.output.resolve() == args.source.resolve() or args.output.is_symlink():
            raise ValueError("output must be a separate regular file")
        validator = args.validator.resolve()
        if args.profile is None:
            result = cook(args.source, validator, args.layer or "project")
        else:
            result = cook_startup(args.source, args.output, validator, args.profile[0],
                                  (args.device or [None])[0], (args.quality or [None])[0])
        with tempfile.NamedTemporaryFile(dir=args.output.parent, prefix=f".{args.output.name}.", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(result)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, args.output)
        temporary = None
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"configuration: {error}", file=sys.stderr)
        return 1
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
