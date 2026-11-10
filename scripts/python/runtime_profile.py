"""Runtime profiling scenarios: manifest, execution, parsing and report-only comparison.

``./scripts/profile-engine`` builds the targets declared in
``config/runtime_profile_scenarios.json`` and calls :func:`run_scenarios`; the
scheduled ``performance`` workflow runs the same command on hosted runners and
on opted-in self-hosted device lanes that declare a GPU class and identity.
Correctness failures (non-zero exits, missing traces, unparseable or incomplete
output, a software rasterizer where a GPU was declared) fail the run.
Timings never do: deltas against a baseline are reported until a device lane
calibrates a variability bound (docs/architecture/engineering-quality.md,
sections 4 and 6; docs/development/runtime-profiling.md).

Percentiles use the nearest-rank convention shared with scripts/benchmark-rhi.
"""
from __future__ import annotations

import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import io
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import time
from typing import Callable, Mapping, Sequence

SCHEMA_VERSION = 1
PERCENTILE_METHOD = "nearest-rank"
MANIFEST_PATH = Path("config/runtime_profile_scenarios.json")
OUTPUT_MARKER = ".ludus-runtime-profile"
PARSER_KINDS = ("perfetto-scopes", "key-value", "csv", "regex", "tagged-rows")
SCENARIO_KEYS = frozenset({
    "id", "owner", "description", "target", "executable", "arguments", "cmake_options",
    "environment", "requires", "trace", "parser", "warmup", "repetitions", "timeout_seconds",
})
PARSER_KEYS = {
    "perfetto-scopes": frozenset({"kind"}),
    "key-value": frozenset({"kind", "identity", "metrics"}),
    "csv": frozenset({"kind", "header", "key", "metrics", "ignore"}),
    "regex": frozenset({"kind", "pattern"}),
    "tagged-rows": frozenset({"kind", "tables", "observe"}),
}
TABLE_KEYS = frozenset({"tag", "columns", "key", "metrics", "where", "rows"})
# Capabilities the runner detects itself; anything else (presentation, a GPU
# class) must be declared by the lane with --capability.
DETECTED_CAPABILITIES = ("shader-tools",)
# Declaring a GPU class (gpu-<class>) also provides "gpu", the capability GPU
# scenarios require, and needs a --device identity so reports name the adapter.
GPU_CAPABILITY = "gpu"
GPU_CLASS = re.compile(r"^gpu-[a-z0-9][a-z0-9-]{0,31}$")
CAPABILITY = re.compile(r"^[a-z][a-z0-9-]{0,47}$")
SCENARIO_ID = re.compile(r"^[a-z][a-z0-9-]{0,47}$")
CMAKE_OPTION = re.compile(r"^LUDUS_[A-Z0-9_]+$")
ENVIRONMENT_NAME = CMAKE_OPTION
ROW_TAG = re.compile(r"^[A-Z][A-Z0-9]{0,15}$")
MAX_ROWS = 100_000
MAX_DEVICE_TEXT = 200
# ludus_rhi_render_tests prints the selected adapter as
# R7ADAPTER,<hex name>,<vendor>,<device>,<driver>,<software known>,<software>.
ADAPTER_TAG = "R7ADAPTER"
ADAPTER_FIELDS = 6
SYSTEM_PROBE_TIMEOUT_SECONDS = 30
MAX_REPETITIONS = 50
MAX_WARMUP = 10
MAX_TIMEOUT_SECONDS = 1800
NANOSECONDS_PER_MICROSECOND = 1000.0
FRAME_MARK = "FrameMark"
# A median shift is called out only above this relative size, and only when the
# repetition ranges do not overlap. It is a reporting aid, not a gate.
DEFAULT_NOTICE_FRACTION = 0.10
# Trace timestamps are quantized by the platform clock (Apple Silicon's 24 MHz
# timebase is 41.67 ns per tick), so smaller trace-derived shifts are resolution
# noise rather than a practical effect.
TRACE_MINIMUM_EFFECT_NS = 250.0
TRACE_METRIC_PREFIXES = ("scope=", "frames/")
NANOSECOND_SUFFIXES = ("_ns", "ns_per_operation")


class ProfileError(ValueError):
    """Invalid manifest, selection, output or report; the run cannot be trusted."""


@dataclass(frozen=True)
class Scenario:
    id: str
    owner: str
    description: str
    target: str
    executable: str
    arguments: tuple[str, ...]
    cmake_options: tuple[tuple[str, str], ...]
    environment: tuple[tuple[str, str], ...]
    requires: tuple[str, ...]
    trace: bool
    parser: Mapping[str, object]
    warmup: int
    repetitions: int
    timeout_seconds: int


def percentile(values: Sequence[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, math.ceil(len(ordered) * fraction) - 1))]


def median(values: Sequence[float]) -> float:
    return percentile(values, 0.5)


def require_strings(value: object, where: str) -> tuple[str, ...]:
    if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
        raise ProfileError(f"{where} must be a list of non-empty strings")
    return tuple(value)


def require_integer(value: object, where: str, minimum: int, maximum: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or not minimum <= value <= maximum:
        raise ProfileError(f"{where} must be an integer in {minimum}..{maximum}")
    return value


def validate_parser(parser: object, where: str) -> Mapping[str, object]:
    if not isinstance(parser, dict) or parser.get("kind") not in PARSER_KINDS:
        raise ProfileError(f"{where}.kind must be one of {', '.join(PARSER_KINDS)}")
    kind = str(parser["kind"])
    if set(parser) != PARSER_KEYS[kind]:
        raise ProfileError(f"{where} for {kind} requires exactly {sorted(PARSER_KEYS[kind])}")
    if kind == "key-value":
        require_strings(parser["identity"], f"{where}.identity")
        if not require_strings(parser["metrics"], f"{where}.metrics"):
            raise ProfileError(f"{where}.metrics must name at least one metric")
    elif kind == "csv":
        if not isinstance(parser["header"], str) or not parser["header"]:
            raise ProfileError(f"{where}.header must be the exact CSV header line")
        columns = parser["header"].split(",")
        selected = [*require_strings(parser["key"], f"{where}.key"), *require_strings(parser["metrics"], f"{where}.metrics")]
        require_strings(parser["ignore"], f"{where}.ignore")
        if not parser["key"] or not parser["metrics"] or any(column not in columns for column in selected):
            raise ProfileError(f"{where} key and metric columns must be non-empty and present in the header")
    elif kind == "regex":
        if not isinstance(parser["pattern"], str):
            raise ProfileError(f"{where}.pattern must be a string")
        try:
            pattern = re.compile(parser["pattern"])
        except re.error as error:
            raise ProfileError(f"{where}.pattern is invalid: {error}") from error
        if not set(pattern.groupindex) - {"label"}:
            raise ProfileError(f"{where}.pattern needs at least one named metric group besides label")
    elif kind == "tagged-rows":
        tables = parser["tables"]
        if not isinstance(tables, list) or not tables:
            raise ProfileError(f"{where}.tables must list at least one table")
        tags = [validate_table(table, f"{where}.tables[{index}]") for index, table in enumerate(tables)]
        observed = require_strings(parser["observe"], f"{where}.observe")
        if any(not ROW_TAG.match(tag) for tag in observed) or len(set(tags) | set(observed)) != len(tags) + len(observed):
            raise ProfileError(f"{where} table and observation tags must be distinct and match {ROW_TAG.pattern}")
    return dict(parser)


def validate_table(table: object, where: str) -> str:
    """Check one tagged-rows table and return its tag."""
    if not isinstance(table, dict) or set(table) != TABLE_KEYS:
        raise ProfileError(f"{where} requires exactly {sorted(TABLE_KEYS)}")
    if not isinstance(table["tag"], str) or not ROW_TAG.match(table["tag"]):
        raise ProfileError(f"{where}.tag must match {ROW_TAG.pattern}")
    columns = require_strings(table["columns"], f"{where}.columns")
    if not columns or len(set(columns)) != len(columns):
        raise ProfileError(f"{where}.columns must name each column once")
    key = require_strings(table["key"], f"{where}.key")
    metrics = require_strings(table["metrics"], f"{where}.metrics")
    where_filter = table["where"]
    if not isinstance(where_filter, dict) or not all(isinstance(value, str) for value in where_filter.values()):
        raise ProfileError(f"{where}.where must map columns to exact string values")
    if not metrics or any(column not in columns for column in (*key, *metrics, *where_filter)):
        raise ProfileError(f"{where} key, metric and where columns must be present in columns")
    if set(key) & set(metrics):
        raise ProfileError(f"{where} key and metric columns must differ")
    require_integer(table["rows"], f"{where}.rows", 1, MAX_ROWS)
    return table["tag"]


def load_manifest(path: Path) -> list[Scenario]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ProfileError(f"cannot read scenario manifest {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema") != SCHEMA_VERSION:
        raise ProfileError(f"{path} must declare schema {SCHEMA_VERSION}")
    if set(document) - {"schema", "description", "scenarios"}:
        raise ProfileError(f"{path} has unknown top-level keys")
    entries = document.get("scenarios")
    if not isinstance(entries, list) or not entries:
        raise ProfileError(f"{path} must list at least one scenario")
    scenarios: list[Scenario] = []
    for index, entry in enumerate(entries):
        where = f"scenarios[{index}]"
        if not isinstance(entry, dict) or set(entry) != SCENARIO_KEYS:
            missing = sorted(SCENARIO_KEYS - set(entry)) if isinstance(entry, dict) else []
            unknown = sorted(set(entry) - SCENARIO_KEYS) if isinstance(entry, dict) else []
            raise ProfileError(f"{where} keys differ from the schema (missing {missing}, unknown {unknown})")
        scenario_id = entry["id"]
        if not isinstance(scenario_id, str) or not SCENARIO_ID.match(scenario_id):
            raise ProfileError(f"{where}.id must match {SCENARIO_ID.pattern}")
        where = f"scenario {scenario_id}"
        for key in ("owner", "description", "target", "executable"):
            if not isinstance(entry[key], str) or not entry[key]:
                raise ProfileError(f"{where}.{key} must be a non-empty string")
        executable = Path(entry["executable"])
        if executable.is_absolute() or ".." in executable.parts:
            raise ProfileError(f"{where}.executable must be relative to the preset build directory")
        options = entry["cmake_options"]
        if not isinstance(options, dict) or not all(
                isinstance(name, str) and CMAKE_OPTION.match(name) and value in ("ON", "OFF")
                for name, value in options.items()):
            raise ProfileError(f"{where}.cmake_options must map LUDUS_* options to ON or OFF")
        environment = entry["environment"]
        if not isinstance(environment, dict) or not all(
                isinstance(name, str) and ENVIRONMENT_NAME.match(name) and isinstance(value, str) and value
                for name, value in environment.items()):
            raise ProfileError(f"{where}.environment must map LUDUS_* variables to non-empty strings")
        if not isinstance(entry["trace"], bool):
            raise ProfileError(f"{where}.trace must be a boolean")
        parser = validate_parser(entry["parser"], f"{where}.parser")
        if parser["kind"] == "perfetto-scopes" and entry["trace"] is False:
            raise ProfileError(f"{where} perfetto-scopes requires trace to be true")
        scenarios.append(Scenario(
            id=scenario_id,
            owner=entry["owner"],
            description=entry["description"],
            target=entry["target"],
            executable=entry["executable"],
            arguments=require_strings(entry["arguments"], f"{where}.arguments"),
            cmake_options=tuple(sorted(options.items())),
            environment=tuple(sorted(environment.items())),
            requires=require_strings(entry["requires"], f"{where}.requires"),
            trace=entry["trace"],
            parser=parser,
            warmup=require_integer(entry["warmup"], f"{where}.warmup", 0, MAX_WARMUP),
            repetitions=require_integer(entry["repetitions"], f"{where}.repetitions", 1, MAX_REPETITIONS),
            timeout_seconds=require_integer(entry["timeout_seconds"], f"{where}.timeout_seconds", 1, MAX_TIMEOUT_SECONDS),
        ))
    identifiers = [scenario.id for scenario in scenarios]
    if len(set(identifiers)) != len(identifiers):
        raise ProfileError(f"{path} repeats a scenario id")
    return scenarios


def shader_tool_options(root: Path, system: str) -> list[str] | None:
    """CMake cache arguments for the pinned shader tools, or None when they are not prepared."""
    slang = root / "out/shader-tools/slang/bin/slangc"
    if not slang.is_file():
        return None
    options = [f"-DLUDUS_SLANG_COMPILER={slang}"]
    if system != "Darwin":
        validator = root / "out/shader-tools/spirv-tools/usr/bin/spirv-val"
        if not validator.is_file():
            return None
        options.append(f"-DLUDUS_SPIRV_VALIDATOR={validator}")
    return options


def detect_capabilities(root: Path, system: str) -> set[str]:
    return {"shader-tools"} if shader_tool_options(root, system) is not None else set()


def resolve_capabilities(detected: set[str], declared: Sequence[str], device: str | None) -> set[str]:
    """Combine detected and lane-declared capabilities; a GPU class implies gpu and needs a device identity."""
    for capability in declared:
        if not CAPABILITY.match(capability):
            raise ProfileError(f"capability '{capability}' must match {CAPABILITY.pattern}")
        if capability == GPU_CAPABILITY:
            raise ProfileError("declare a GPU class such as gpu-apple-m5 instead of bare gpu")
    if device is not None and (not device.strip() or len(device) > MAX_DEVICE_TEXT or not device.isprintable()):
        raise ProfileError(f"--device must be printable text of 1..{MAX_DEVICE_TEXT} characters")
    capabilities = set(detected) | set(declared)
    classes = [capability for capability in capabilities if capability.startswith("gpu-")]
    if any(not GPU_CLASS.match(capability) for capability in classes):
        raise ProfileError(f"GPU classes must match {GPU_CLASS.pattern}")
    if classes:
        if device is None:
            raise ProfileError("a GPU class capability requires --device with the adapter and driver identity")
        capabilities.add(GPU_CAPABILITY)
    return capabilities


def environment_names(scenarios: Sequence[Scenario]) -> frozenset[str]:
    """Variables some scenario sets; runs never inherit them, so one scenario's switches cannot leak into another."""
    return frozenset(name for scenario in scenarios for name, _ in scenario.environment)


def select_scenarios(scenarios: Sequence[Scenario], requested: Sequence[str],
                     capabilities: set[str]) -> list[tuple[Scenario, str | None]]:
    """Pair each selected scenario with a skip reason, or None when it can run.

    Explicitly requested scenarios must be runnable; the default selection skips
    scenarios whose capabilities this lane does not provide.
    """
    known = {scenario.id: scenario for scenario in scenarios}
    unknown = [identifier for identifier in requested if identifier not in known]
    if unknown:
        raise ProfileError(f"unknown scenario(s) {', '.join(unknown)}; known: {', '.join(known)}")
    chosen = [known[identifier] for identifier in dict.fromkeys(requested)] if requested else list(scenarios)
    selection: list[tuple[Scenario, str | None]] = []
    for scenario in chosen:
        missing = [capability for capability in scenario.requires if capability not in capabilities]
        if missing and requested:
            raise ProfileError(f"scenario {scenario.id} requires unavailable capability {', '.join(missing)}")
        selection.append((scenario, f"missing capability {', '.join(missing)}" if missing else None))
    return selection


def configure_arguments(runnable: Sequence[Scenario], shader_options: Sequence[str] | None) -> list[str]:
    merged: dict[str, str] = {}
    for scenario in runnable:
        for name, value in scenario.cmake_options:
            if merged.setdefault(name, value) != value:
                raise ProfileError(f"scenarios disagree on CMake option {name}")
    arguments = [f"-D{name}={value}" for name, value in sorted(merged.items())]
    if any("shader-tools" in scenario.requires for scenario in runnable):
        arguments.extend(shader_options or ())
    return arguments


def number(text: str, where: str) -> float:
    try:
        value = float(text.rstrip(","))
    except ValueError as error:
        raise ProfileError(f"{where}: '{text}' is not a number") from error
    if not math.isfinite(value):
        raise ProfileError(f"{where}: non-finite value '{text}'")
    return value


def add_metric(metrics: dict[str, float], name: str, value: float) -> None:
    if name in metrics:
        raise ProfileError(f"output repeats metric {name}")
    metrics[name] = value


def parse_key_value(output: str, identity: Sequence[str], selected: Sequence[str]) -> dict[str, float]:
    metrics: dict[str, float] = {}
    for line in output.splitlines():
        fields = dict(token.split("=", 1) for token in line.split() if "=" in token)
        if not all(key in fields for key in identity) or not any(key in fields for key in selected):
            continue
        prefix = "/".join(f"{key}={fields[key]}" for key in identity)
        for key in selected:
            if key in fields:
                add_metric(metrics, f"{prefix}/{key}" if prefix else key, number(fields[key], key))
    return metrics


def parse_csv(output: str, header: str, key: Sequence[str], selected: Sequence[str],
              ignore: Sequence[str]) -> dict[str, float]:
    lines = output.splitlines()
    try:
        start = [line.strip() for line in lines].index(header)
    except ValueError as error:
        raise ProfileError(f"CSV header '{header}' not found") from error
    columns = header.split(",")
    metrics: dict[str, float] = {}
    for row in csv.reader(io.StringIO("\n".join(lines[start + 1:]))):
        if not row or len(row) != len(columns):
            break
        values = dict(zip(columns, (cell.strip() for cell in row)))
        if any(values[column] in ignore for column in key):
            continue
        prefix = "/".join(f"{column}={values[column]}" for column in key)
        for column in selected:
            add_metric(metrics, f"{prefix}/{column}", number(values[column], column))
    return metrics


def parse_regex(output: str, pattern: str) -> dict[str, float]:
    expression = re.compile(pattern)
    metrics: dict[str, float] = {}
    for line in output.splitlines():
        match = expression.match(line.strip())
        if match is None:
            continue
        groups = match.groupdict()
        label = " ".join((groups.pop("label", None) or "").split())
        for name, text in groups.items():
            if text is not None:
                add_metric(metrics, f"{label}/{name}" if label else name, number(text, name))
    return metrics


def tagged_lines(output: str, tag: str) -> list[list[str]]:
    prefix = f"{tag},"
    return [row[1:] for row in csv.reader(io.StringIO("\n".join(
        line.strip() for line in output.splitlines() if line.strip().startswith(prefix))))]


def distribution_names(column: str) -> tuple[str, str]:
    """Median and p95 names that keep the nanosecond suffix last, so reports format units."""
    if column.endswith("_ns"):
        return f"{column[:-3]}_median_ns", f"{column[:-3]}_p95_ns"
    return f"{column}_median", f"{column}_p95"


def parse_tagged_rows(output: str, tables: Sequence[Mapping[str, object]]) -> dict[str, float]:
    """Per-key median, p95 and sample count of tagged CSV sample rows (for example R5,... or L5,...).

    Each table must print exactly its declared number of rows: an incomplete
    sample set is a correctness failure, as in scripts/benchmark-rhi. ``where``
    keeps only rows whose columns equal the given values (for example GPU timing
    samples that are available) before grouping; a filter that keeps no row means
    the run did not measure what the scenario declares and is a failure too.
    """
    metrics: dict[str, float] = {}
    for table in tables:
        tag = str(table["tag"])
        columns = list(table["columns"])
        rows = tagged_lines(output, tag)
        if len(rows) != table["rows"]:
            raise ProfileError(f"{tag}: expected {table['rows']} rows, found {len(rows)}")
        groups: dict[str, dict[str, list[float]]] = {}
        for row in rows:
            if len(row) != len(columns):
                raise ProfileError(f"{tag}: row has {len(row)} fields, expected {len(columns)}")
            values = dict(zip(columns, (cell.strip() for cell in row)))
            if any(values[column] != expected for column, expected in table["where"].items()):
                continue
            prefix = "/".join([tag, *(f"{column}={values[column]}" for column in table["key"])])
            group = groups.setdefault(prefix, {})
            for column in table["metrics"]:
                group.setdefault(column, []).append(number(values[column], f"{tag} {column}"))
        if not groups:
            raise ProfileError(f"{tag}: no rows match {dict(table['where'])}")
        for prefix, group in sorted(groups.items()):
            for column, samples in group.items():
                median_name, p95_name = distribution_names(column)
                add_metric(metrics, f"{prefix}/{median_name}", median(samples))
                add_metric(metrics, f"{prefix}/{p95_name}", percentile(samples, 0.95))
                add_metric(metrics, f"{prefix}/{column}_minimum", min(samples))
                add_metric(metrics, f"{prefix}/{column}_maximum", max(samples))
            add_metric(metrics, f"{prefix}/samples", float(len(next(iter(group.values())))))
    return metrics


def parse_observations(scenario: Scenario, output: str) -> dict[str, list[str]]:
    """Identity lines a tagged-rows scenario declares in ``observe``; each must appear exactly once."""
    if scenario.parser["kind"] != "tagged-rows":
        return {}
    observations: dict[str, list[str]] = {}
    for tag in scenario.parser["observe"]:
        lines = tagged_lines(output, tag)
        if len(lines) != 1:
            raise ProfileError(f"expected one {tag} observation, found {len(lines)}")
        observations[tag] = [cell.strip() for cell in lines[0]]
    return observations


def decode_adapter(fields: Sequence[str]) -> dict[str, object]:
    """The selected RHI adapter from an R7ADAPTER observation."""
    if len(fields) != ADAPTER_FIELDS:
        raise ProfileError(f"{ADAPTER_TAG} has {len(fields)} fields, expected {ADAPTER_FIELDS}")
    try:
        name = bytes.fromhex(fields[0]).decode("utf-8", errors="replace")
        vendor, device, driver, software_known, software = (int(field) for field in fields[1:])
    except ValueError as error:
        raise ProfileError(f"{ADAPTER_TAG} is malformed: {error}") from error
    return {"name": name, "vendor_id": vendor, "device_id": device, "driver_version_raw": driver or None,
            "software": bool(software) if software_known else None}


def parse_perfetto_scopes(trace: Mapping[str, object] | Sequence[object]) -> dict[str, float]:
    """Per-scope duration distributions and frame intervals from Chrome/Perfetto JSON."""
    events = trace.get("traceEvents") if isinstance(trace, Mapping) else trace
    if not isinstance(events, list):
        raise ProfileError("trace has no traceEvents list")
    stacks: dict[tuple[object, object], list[tuple[str, float]]] = {}
    durations: dict[str, list[float]] = {}
    frames: list[float] = []
    for event in events:
        if not isinstance(event, dict):
            raise ProfileError("trace event is not an object")
        phase = event.get("ph")
        name = str(event.get("name", ""))
        if phase not in ("B", "E", "X", "i", "I"):
            continue
        timestamp = number(str(event.get("ts")), f"trace event {name} ts")
        stack = stacks.setdefault((event.get("pid"), event.get("tid")), [])
        if phase == "B":
            stack.append((name, timestamp))
        elif phase == "E":
            if not stack or stack[-1][0] != name:
                raise ProfileError(f"trace end event {name} does not close the innermost open scope")
            durations.setdefault(name, []).append(timestamp - stack.pop()[1])
        elif phase == "X":
            durations.setdefault(name, []).append(number(str(event.get("dur")), f"trace event {name} dur"))
        elif name == FRAME_MARK:
            frames.append(timestamp)
    metrics: dict[str, float] = {}
    for name, values in sorted(durations.items()):
        nanoseconds = [value * NANOSECONDS_PER_MICROSECOND for value in values]
        metrics[f"scope={name}/count"] = float(len(values))
        metrics[f"scope={name}/median_ns"] = median(nanoseconds)
        metrics[f"scope={name}/p95_ns"] = percentile(nanoseconds, 0.95)
    if frames:
        metrics["frames/count"] = float(len(frames))
    if len(frames) > 1:
        ordered = sorted(frames)
        intervals = [(b - a) * NANOSECONDS_PER_MICROSECOND for a, b in zip(ordered, ordered[1:])]
        metrics["frames/interval_median_ns"] = median(intervals)
        metrics["frames/interval_p95_ns"] = percentile(intervals, 0.95)
    if not metrics:
        raise ProfileError("trace contains no scopes or frame marks")
    return metrics


def parse_metrics(scenario: Scenario, output: str, trace_path: Path | None) -> dict[str, float]:
    parser = scenario.parser
    kind = parser["kind"]
    trace_metrics = {}
    if scenario.trace:
        if trace_path is None or not trace_path.is_file():
            raise ProfileError("scenario did not write its requested trace")
        try:
            trace = json.loads(trace_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            raise ProfileError(f"trace is not valid JSON: {error}") from error
        trace_metrics = parse_perfetto_scopes(trace)
    if kind == "perfetto-scopes":
        metrics = trace_metrics
    elif kind == "key-value":
        metrics = parse_key_value(output, parser["identity"], parser["metrics"])
    elif kind == "csv":
        metrics = parse_csv(output, parser["header"], parser["key"], parser["metrics"], parser["ignore"])
    elif kind == "tagged-rows":
        metrics = parse_tagged_rows(output, parser["tables"])
    else:
        metrics = parse_regex(output, parser["pattern"])
    if not metrics:
        raise ProfileError("no metrics parsed from scenario output")
    return metrics


def summarize(samples: Sequence[float]) -> dict[str, object]:
    middle = median(samples)
    low = min(samples)
    high = max(samples)
    return {
        "median": middle,
        "min": low,
        "max": high,
        "spread": (high - low) / middle if middle > 0 else None,
        "samples": list(samples),
    }


Runner = Callable[..., subprocess.CompletedProcess]


def run_scenario(scenario: Scenario, build_dir: Path, output_dir: Path,
                 repetitions: int | None = None, runner: Runner = subprocess.run,
                 controlled_environment: frozenset[str] = frozenset()) -> dict[str, object]:
    """Run one scenario's warmups and measured repetitions; never raises for scenario failures.

    Inherited variables named in ``controlled_environment`` are removed before
    the scenario's own ``environment`` is applied.
    """
    count = repetitions if repetitions is not None else scenario.repetitions
    executable = build_dir / scenario.executable
    if os.name == "nt":
        executable = executable.with_suffix(".exe")
    work = output_dir / "work" / scenario.id
    logs = output_dir / "logs"
    traces = output_dir / "traces"
    for directory in (work, logs, traces):
        directory.mkdir(parents=True, exist_ok=True)
    result: dict[str, object] = {
        "id": scenario.id, "owner": scenario.owner, "description": scenario.description,
        "command": [scenario.executable, *scenario.arguments], "warmup": scenario.warmup,
        "repetitions": count, "status": "failed", "metrics": {}, "trace": None, "logs": [],
    }
    if scenario.environment:
        result["environment"] = dict(scenario.environment)
    if not executable.is_file():
        result["reason"] = f"executable {scenario.executable} was not built"
        return result
    removed = controlled_environment | environment_names([scenario])
    environment = {**{name: value for name, value in os.environ.items() if name not in removed},
                   "LUDUS_DIAGNOSTIC_INTERACTIVE": "0", **dict(scenario.environment)}
    per_metric: dict[str, list[float]] = {}
    observations: dict[str, list[str]] | None = None
    for index in range(scenario.warmup + count):
        measured = index >= scenario.warmup
        label = f"{scenario.id}-{'run' if measured else 'warmup'}-{index - scenario.warmup if measured else index}"
        trace_path = work / f"{label}.json" if scenario.trace else None
        command = [str(executable), *scenario.arguments]
        if trace_path is not None:
            command.extend(["--trace-output", str(trace_path)])
        log_path = logs / f"{label}.log"
        result["logs"].append(log_path.relative_to(output_dir).as_posix())
        started = time.perf_counter_ns()
        try:
            process = runner(command, cwd=work, env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, errors="replace", timeout=scenario.timeout_seconds, check=False)
        except subprocess.TimeoutExpired as error:
            log_path.write_text(str(error.output or ""), encoding="utf-8")
            result["reason"] = f"{label} exceeded {scenario.timeout_seconds}s"
            return result
        except OSError as error:
            result["reason"] = f"{label} could not start: {error}"
            return result
        elapsed = time.perf_counter_ns() - started
        log_path.write_text(process.stdout or "", encoding="utf-8")
        if process.returncode != 0:
            result["reason"] = f"{label} exited with {process.returncode}"
            return result
        try:
            metrics = parse_metrics(scenario, process.stdout or "", trace_path)
            observed = parse_observations(scenario, process.stdout or "")
            adapter = decode_adapter(observed[ADAPTER_TAG]) if ADAPTER_TAG in observed else None
        except ProfileError as error:
            result["reason"] = f"{label}: {error}"
            return result
        if observations is not None and observed != observations:
            result["reason"] = f"{label} observed a different adapter, profile or backend than earlier runs"
            return result
        observations = observed
        if adapter is not None and GPU_CAPABILITY in scenario.requires and adapter["software"] is True:
            result["reason"] = f"{label} selected software adapter '{adapter['name']}', not GPU evidence"
            return result
        if observed:
            result["observations"] = observed
        if adapter is not None:
            result["adapter"] = adapter
        if not measured:
            continue
        metrics["process/wall_ns"] = float(elapsed)
        if per_metric and set(metrics) != set(per_metric):
            result["reason"] = f"{label} reported a different metric set than earlier repetitions"
            return result
        for name, value in metrics.items():
            per_metric.setdefault(name, []).append(value)
        if trace_path is not None:
            kept = traces / f"{scenario.id}.json"
            shutil.move(str(trace_path), kept)
            result["trace"] = kept.relative_to(output_dir).as_posix()
    shutil.rmtree(work, ignore_errors=True)
    result["metrics"] = {name: summarize(values) for name, values in sorted(per_metric.items())}
    result["status"] = "passed"
    return result


def git_identity(root: Path) -> dict[str, object]:
    def git(*arguments: str) -> str:
        completed = subprocess.run(["git", *arguments], cwd=root, stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True, check=False)
        return completed.stdout.strip() if completed.returncode == 0 else ""
    return {"commit": git("rev-parse", "HEAD") or None, "dirty": bool(git("status", "--porcelain"))}


def processor_name(system: str) -> str:
    if system == "Darwin":
        completed = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, text=True, check=False)
        if completed.returncode == 0 and completed.stdout.strip():
            return completed.stdout.strip()
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(errors="replace").splitlines():
            if line.lower().startswith("model name") and ":" in line:
                return line.split(":", 1)[1].strip()
    return platform.processor() or "unknown"


def host_identity() -> dict[str, object]:
    system = platform.system()
    return {
        "system": system, "release": platform.release(), "machine": platform.machine(),
        "processor": processor_name(system), "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
    }


def ci_identity() -> dict[str, str] | None:
    names = ("GITHUB_WORKFLOW", "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "GITHUB_EVENT_NAME", "GITHUB_REF", "RUNNER_OS",
             "RUNNER_ARCH", "RUNNER_NAME", "RUNNER_ENVIRONMENT", "ImageOS", "ImageVersion")
    values = {name: os.environ[name] for name in names if os.environ.get(name)}
    return values or None


def parse_vulkan_summary(text: str) -> list[dict[str, str]]:
    """Devices from ``vulkaninfo --summary``: one entry per GPUn block."""
    fields = ("deviceName", "deviceType", "vendorID", "deviceID", "driverName", "driverInfo", "apiVersion")
    devices: list[dict[str, str]] = []
    for line in text.splitlines():
        stripped = line.strip()
        if re.match(r"^GPU[0-9]+:$", stripped):
            devices.append({})
        elif devices and "=" in stripped:
            name, value = (part.strip() for part in stripped.split("=", 1))
            if name in fields:
                devices[-1][name] = value
    return [device for device in devices if device]


def parse_display_profile(text: str) -> list[dict[str, str]]:
    """GPUs from macOS ``system_profiler SPDisplaysDataType -json``."""
    try:
        entries = json.loads(text).get("SPDisplaysDataType", [])
    except (ValueError, AttributeError):
        return []
    fields = {"sppci_model": "model", "spdisplays_vendor": "vendor", "sppci_cores": "cores",
              "spdisplays_mtlgpufamilysupport": "metal_family", "sppci_device_type": "type"}
    return [{label: str(entry[key]) for key, label in fields.items() if key in entry}
            for entry in entries if isinstance(entry, dict)]


def observe_system_gpus(system: str, runner: Runner = subprocess.run) -> list[dict[str, str]]:
    """Best-effort OS report of the host GPUs; empty when the tool is missing or fails. Never raises."""
    if system == "Darwin":
        command, parse = ["system_profiler", "SPDisplaysDataType", "-json"], parse_display_profile
    elif shutil.which("vulkaninfo"):
        command, parse = ["vulkaninfo", "--summary"], parse_vulkan_summary
    else:
        return []
    try:
        completed = runner(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, errors="replace",
                           timeout=SYSTEM_PROBE_TIMEOUT_SECONDS, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return []
    return parse(completed.stdout or "") if completed.returncode == 0 else []


def device_identity(declared: str | None, capabilities: set[str], system_gpus: Sequence[Mapping[str, str]],
                    results: Sequence[Mapping[str, object]]) -> dict[str, object]:
    adapters: list[object] = []
    for entry in results:
        adapter = entry.get("adapter")
        if adapter is not None and adapter not in adapters:
            adapters.append(adapter)
    return {
        "declared": declared,
        "gpu_classes": sorted(capability for capability in capabilities if GPU_CLASS.match(capability)),
        "system_reported": list(system_gpus),
        "engine_adapters": adapters,
    }


def prepare_output(output_dir: Path) -> None:
    """Reset a directory this runner owns; refuse to clear anything else."""
    if output_dir.exists():
        if not output_dir.is_dir():
            raise ProfileError(f"{output_dir} is not a directory")
        if any(output_dir.iterdir()) and not (output_dir / OUTPUT_MARKER).is_file():
            raise ProfileError(f"{output_dir} is not empty and was not written by profile-engine; choose another --output")
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True)
    (output_dir / OUTPUT_MARKER).write_text("Generated by ./scripts/profile-engine; safe to delete.\n", encoding="utf-8")


def load_report(path: Path) -> dict[str, object]:
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ProfileError(f"cannot read report {path}: {error}") from error
    if not isinstance(report, dict) or report.get("schema") != SCHEMA_VERSION or not isinstance(report.get("scenarios"), list):
        raise ProfileError(f"{path} is not a schema {SCHEMA_VERSION} runtime profile report")
    return report


def compare(report: Mapping[str, object], baseline: Mapping[str, object],
            notice_fraction: float = DEFAULT_NOTICE_FRACTION) -> dict[str, dict[str, dict[str, object]]]:
    """Median deltas for metrics present in both passed runs, keyed by scenario then metric."""
    previous = {entry["id"]: entry for entry in baseline["scenarios"]
                if isinstance(entry, dict) and entry.get("status") == "passed"}
    deltas: dict[str, dict[str, dict[str, object]]] = {}
    for entry in report["scenarios"]:
        old = previous.get(entry["id"])
        if entry["status"] != "passed" or old is None:
            continue
        for name, current in entry["metrics"].items():
            is_tail = "/p95_" in name or "_p95_" in name or name.startswith("p95_")
            if is_tail and report.get("percentile_method") != baseline.get("percentile_method"):
                continue
            before = old.get("metrics", {}).get(name)
            if not isinstance(before, dict) or not before.get("median"):
                continue
            fraction = (current["median"] - before["median"]) / before["median"]
            separated = current["min"] > before["max"] or current["max"] < before["min"]
            resolvable = (not name.startswith(TRACE_METRIC_PREFIXES) or not name.endswith(NANOSECOND_SUFFIXES)
                          or abs(current["median"] - before["median"]) >= TRACE_MINIMUM_EFFECT_NS)
            deltas.setdefault(entry["id"], {})[name] = {
                "baseline_median": before["median"],
                "fraction": fraction,
                "notable": separated and resolvable and abs(fraction) >= notice_fraction,
            }
    return deltas


def same_host(report: Mapping[str, object], baseline: Mapping[str, object]) -> bool:
    """Same host and declared device; reports from before device identity count as undeclared."""
    keys = ("system", "machine", "processor", "logical_cpus")
    device = (report.get("device") or {}).get("declared")
    return (all(report["host"].get(key) == baseline.get("host", {}).get(key) for key in keys)
            and device == (baseline.get("device") or {}).get("declared"))


def format_value(name: str, value: float) -> str:
    if name.endswith(NANOSECOND_SUFFIXES):
        for unit, scale in (("s", 1e9), ("ms", 1e6), ("µs", 1e3)):
            if abs(value) >= scale:
                return f"{value / scale:.3g} {unit}"
        return f"{value:.3g} ns"
    return f"{value:.6g}"


def render_summary(report: Mapping[str, object]) -> str:
    scenarios = report["scenarios"]
    counts = {status: sum(entry["status"] == status for entry in scenarios) for status in ("passed", "failed", "skipped")}
    commit = (report["git"].get("commit") or "unknown")[:12]
    host = report["host"]
    lines = [
        f"## Runtime profile: {report['preset']}",
        "",
        f"Commit `{commit}`{' (dirty)' if report['git'].get('dirty') else ''} on {host['system']} {host['machine']}, "
        f"{host['processor']}, {host['logical_cpus']} logical CPUs. "
        f"{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped.",
    ]
    device = report.get("device") or {}
    if device.get("declared"):
        classes = ", ".join(f"`{name}`" for name in device.get("gpu_classes", [])) or "no GPU class"
        adapters = "; ".join(f"{adapter['name']}{' (software)' if adapter.get('software') else ''}"
                             for adapter in device.get("engine_adapters", [])) or "not observed"
        lines += ["", f"Device: {device['declared']} ({classes}). Engine-selected adapter: {adapters}."]
    lines += [
        "",
        "Timings are report-only until a device lane calibrates a variability bound. A change is marked notable when the "
        f"median moved at least {DEFAULT_NOTICE_FRACTION:.0%}, the repetition ranges do not overlap and a "
        f"trace-derived shift exceeds {TRACE_MINIMUM_EFFECT_NS:.0f} ns of clock resolution.",
    ]
    baseline = report.get("baseline")
    if baseline:
        lines += ["", f"Baseline: commit `{(baseline.get('commit') or 'unknown')[:12]}`"
                  + ("" if baseline.get("same_host") else " (different host identity; deltas are indicative only)") + "."]
        if baseline.get("same_percentiles", True) is False:
            lines += ["Percentile convention changed; p95 deltas are omitted until the next matching baseline."]
    lines += ["", "| Scenario | Status | Runs | Wall median | Notable changes |", "| --- | --- | --- | --- | --- |"]
    deltas = report.get("comparison", {})
    for entry in scenarios:
        wall = entry["metrics"].get("process/wall_ns") if entry["status"] == "passed" else None
        notable = [name for name, delta in deltas.get(entry["id"], {}).items() if delta["notable"]]
        detail = entry.get("reason", "") if entry["status"] != "passed" else (", ".join(notable) or "none")
        lines.append(f"| `{entry['id']}` | {entry['status']} | {entry['repetitions']} | "
                     f"{format_value('wall_ns', wall['median']) if wall else '-'} | {detail} |")
    for entry in scenarios:
        if entry["status"] != "passed":
            continue
        lines += ["", f"<details><summary><code>{entry['id']}</code> metrics</summary>", "",
                  "| Metric | Median | Min | Max | Spread | Baseline | Change |", "| --- | --- | --- | --- | --- | --- | --- |"]
        for name, metric in entry["metrics"].items():
            delta = deltas.get(entry["id"], {}).get(name)
            spread = f"{metric['spread']:.1%}" if metric["spread"] is not None else "-"
            change = "-"
            if delta:
                change = f"{delta['fraction']:+.1%}{' **notable**' if delta['notable'] else ''}"
            lines.append(f"| `{name}` | {format_value(name, metric['median'])} | {format_value(name, metric['min'])} | "
                         f"{format_value(name, metric['max'])} | {spread} | "
                         f"{format_value(name, delta['baseline_median']) if delta else '-'} | {change} |")
        lines += ["", "</details>"]
    return "\n".join(lines) + "\n"


def run_scenarios(root: Path, preset: str, build_dir: Path, output_dir: Path,
                  selection: Sequence[tuple[Scenario, str | None]], capabilities: set[str], *,
                  compiler: str, repetitions: int | None = None, baseline_path: Path | None = None,
                  device: str | None = None, controlled_environment: frozenset[str] | None = None,
                  runner: Runner = subprocess.run,
                  system_gpus: Callable[[], list[dict[str, str]]] | None = None) -> dict[str, object]:
    """Run the selected scenarios into a fresh output directory and write report.json and summary.md.

    ``device`` is the lane's declared adapter/driver identity. ``controlled_environment``
    defaults to the variables the selected scenarios set. ``system_gpus`` overrides
    the OS GPU probe (tests pass a stub).
    """
    if controlled_environment is None:
        controlled_environment = environment_names([scenario for scenario, _ in selection])
    baseline = load_report(baseline_path) if baseline_path is not None else None
    prepare_output(output_dir)
    results: list[dict[str, object]] = []
    for scenario, skip_reason in selection:
        if skip_reason is not None:
            print(f"Skipping {scenario.id}: {skip_reason}", flush=True)
            results.append({"id": scenario.id, "owner": scenario.owner, "description": scenario.description,
                            "status": "skipped", "reason": skip_reason, "repetitions": 0, "metrics": {}})
            continue
        print(f"Running {scenario.id} ({scenario.warmup} warmup, "
              f"{repetitions if repetitions is not None else scenario.repetitions} measured)", flush=True)
        result = run_scenario(scenario, build_dir, output_dir, repetitions, runner, controlled_environment)
        print(f"  {result['status']}{': ' + str(result['reason']) if result.get('reason') else ''}", flush=True)
        results.append(result)
    shutil.rmtree(output_dir / "work", ignore_errors=True)
    report: dict[str, object] = {
        "schema": SCHEMA_VERSION,
        "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "timing_policy": "report-only",
        "percentile_method": PERCENTILE_METHOD,
        "preset": preset,
        "compiler": compiler,
        "git": git_identity(root),
        "host": host_identity(),
        "ci": ci_identity(),
        "capabilities": sorted(capabilities),
        "device": device_identity(device, capabilities,
                                  system_gpus() if system_gpus is not None else observe_system_gpus(platform.system()),
                                  results),
        "scenarios": results,
    }
    if baseline is not None:
        report["baseline"] = {"commit": baseline.get("git", {}).get("commit"), "created_utc": baseline.get("created_utc"),
                              "same_host": same_host(report, baseline),
                              "same_percentiles": report["percentile_method"] == baseline.get("percentile_method")}
        report["comparison"] = compare(report, baseline)
    (output_dir / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (output_dir / "summary.md").write_text(render_summary(report), encoding="utf-8")
    return report
