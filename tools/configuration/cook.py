#!/usr/bin/env python3
"""Cook bounded TOML through the engine's actual schema and validator.

Python 3.11+ is required for this optional authoring tool (stdlib tomllib).
No TOML dependency is linked into the runtime. No include/import/environment
expansion: the command explicitly names one source and one rank.
"""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile

MAX_BYTES = 262144


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


def cook(source: Path, validator: Path, layer: str, schema_id: str | None = None) -> bytes:
    try:
        import tomllib
    except ImportError as error:
        raise ValueError("TOML cooking requires Python 3.11+; use a newer interpreter") from error
    with source.open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    if len(data) > MAX_BYTES:
        raise ValueError("source exceeds 256 KiB")
    selection = ["--schema-id", schema_id] if schema_id else []
    schema_run = subprocess.run([str(validator), *selection, "--schema"], capture_output=True, timeout=15)
    if schema_run.returncode:
        raise ValueError(schema_run.stderr.decode("utf-8", errors="replace").strip() or "schema export failed")
    schema = json.loads(schema_run.stdout)
    descriptors = {setting["key"]: setting for setting in schema["settings"]}
    values = flatten(tomllib.loads(data.decode("utf-8")))
    records = []
    for key, value in sorted(values.items()):
        if key not in descriptors:
            raise ValueError(f"unknown setting: {key}")
        descriptor = descriptors[key]
        kind = descriptor["type"]
        if layer == "preference" and not descriptor["persistent"]:
            raise ValueError(f"setting is not persistent: {key}")
        if kind == "bool":
            valid = type(value) is bool
        elif kind in ("int32", "uint32", "int64", "uint64"):
            valid = type(value) is int
            if valid and kind in ("int64", "uint64"):
                value = str(value)
        elif kind in ("float32", "float64"):
            valid = type(value) in (int, float) and math.isfinite(value)
        else:
            valid = type(value) is str
        if not valid:
            raise ValueError(f"wrong TOML type for {key}: expected {kind}")
        records.append({"key": key, "type": kind, "value": value, "source": source.name, "line": 0})
    bundle = {"version": 1, "schema": schema["schema"], "layer": layer, "values": records}
    encoded = json.dumps(bundle, ensure_ascii=False, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(encoded) > MAX_BYTES:
        raise ValueError("bundle exceeds 256 KiB")
    validated = subprocess.run([str(validator), *selection, "--validate", layer], input=encoded, capture_output=True,
                               timeout=15)
    if validated.returncode:
        raise ValueError(validated.stderr.decode("utf-8", errors="replace").strip() or "engine validation failed")
    return validated.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--validator", required=True, type=Path, help="path to ludus-config from the matching SDK")
    parser.add_argument("--layer", choices=("project", "preference"), default="project")
    parser.add_argument("--schema-id", help="engine schema identity, such as ludus.audio.v1 (default: GameHost)")
    args = parser.parse_args()
    temporary = None
    try:
        if args.output.resolve() == args.source.resolve() or args.output.is_symlink():
            raise ValueError("output must be a separate regular file")
        result = cook(args.source, args.validator.resolve(), args.layer, args.schema_id)
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
