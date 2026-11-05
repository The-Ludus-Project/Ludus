"""Shared CMake File API codemodel query/read helpers.

This module holds the ONLY copy of the File API named-query creation and
codemodel/target/artifact resolution logic. It was extracted from rad_debugger.py
so both the RAD debugger integration and the editor workspace adapter resolve a
CMake executable target through one tested implementation, parameterized by the
File API named-client directory.

RAD delegates here with its historical client name ("ludus-debug"); the editor
uses "ludus-editor". The editor path additionally enforces the bounded-reply
limits and executable-only listing described in
.kiro/specs/editor-workspace/design.md section 7. RAD keeps its own argument
restrictions, sessions and launch behavior; only the File API logic is shared.

No exceptions cross into engine C++: this is Python tooling. The caller supplies
an `engine`-like module exposing EngineError and read_json; failures are raised
as EngineError with actionable messages.
"""
from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any


def read_json(path: Path, engine: Any) -> Any:
    """Read and parse a UTF-8 JSON file, translating I/O/parse errors.

    Local to this module so the shared File API reader does not depend on a
    particular helper existing on the caller's `engine` module.
    """
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise engine.EngineError(f"cannot read {path}: {exc}") from exc


# Bounded File API read limits (design section 7). Oversized/invalid replies
# fail with a path-specific diagnosis rather than being silently truncated.
MAX_JSON_FILE_BYTES = 1 * 1024 * 1024          # 1 MiB per JSON file
MAX_AGGREGATE_BYTES = 16 * 1024 * 1024         # 16 MiB aggregate across the reply
MAX_TARGET_ENTRIES = 4096                      # at most 4096 target entries
MAX_EXECUTABLE_TARGETS = 128                   # at most 128 executable targets

# A CMake target name accepted by the descriptor contract. Shared with the
# descriptor validator and the editor UI (design section 4).
TARGET_NAME_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_.+-]*")


# Message overrides let a caller (RAD) keep its historical, user-facing error
# strings while sharing the resolution logic. Keys name the failure site; a
# missing key falls back to the shared default message.
_DEFAULT_MESSAGES = {
    "no_reply": "no CMake File API reply; configure the build tree first",
    "bad_reply": "invalid CMake File API reply; reconfigure",
    "not_one": "CMake target {target!r} does not resolve to exactly one target in {build_dir}",
    "not_executable": "CMake target {target!r} is not an executable",
}


def query_codemodel(build_dir: Path, client: str) -> None:
    """Create the named codemodel-v2 query in `build_dir` for `client`.

    `client` is the File API named-client suffix (e.g. "ludus-debug" or
    "ludus-editor"); the directory becomes `client-<client>`. Creating the query
    is idempotent and must happen before configure so the reply exists after it.
    """
    query = build_dir / ".cmake" / "api" / "v1" / "query" / f"client-{client}"
    query.mkdir(parents=True, exist_ok=True)
    (query / "codemodel-v2").touch()


def _bounded_read_json(path: Path, aggregate: dict[str, int], engine: Any) -> Any:
    """Read a File API JSON file enforcing per-file and aggregate byte bounds."""
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise engine.EngineError(f"cannot stat CMake File API file {path}: {exc}") from exc
    if size > MAX_JSON_FILE_BYTES:
        raise engine.EngineError(
            f"CMake File API file exceeds the 1 MiB bound: {path} ({size} bytes)"
        )
    aggregate["total"] += size
    if aggregate["total"] > MAX_AGGREGATE_BYTES:
        raise engine.EngineError("CMake File API reply exceeds the 16 MiB aggregate bound")
    return read_json(path, engine)


def _reply_reference(reply_dir: Path, json_file: str, engine: Any) -> Path:
    """Resolve and validate that `json_file` stays within the reply directory."""
    candidate = (reply_dir / json_file).resolve()
    reply_root = reply_dir.resolve()
    try:
        candidate.relative_to(reply_root)
    except ValueError as exc:
        raise engine.EngineError(
            f"CMake File API reference escapes the reply directory: {json_file}"
        ) from exc
    if not candidate.is_file():
        raise engine.EngineError(f"CMake File API referenced file is missing: {json_file}")
    return candidate


def _latest_codemodel(
    build_dir: Path,
    client: str,
    engine: Any,
    *,
    require_version: bool = True,
    messages: dict[str, str] | None = None,
) -> tuple[Path, Any, dict[str, int]]:
    """Return (reply_dir, codemodel, aggregate) following the latest index.

    When require_version is True the codemodel major version must be 2 (the
    editor path). RAD historically did not perform that check and relied on the
    structural KeyError path, so it passes require_version=False and its own
    `messages` to preserve exact behavior and error strings.
    """
    text = dict(_DEFAULT_MESSAGES)
    if messages:
        text.update(messages)
    reply = build_dir / ".cmake" / "api" / "v1" / "reply"
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise engine.EngineError(text["no_reply"])
    aggregate = {"total": 0}
    index = _bounded_read_json(indexes[-1], aggregate, engine)
    try:
        reference = index["reply"][f"client-{client}"]["codemodel-v2"]["jsonFile"]
    except (KeyError, TypeError) as exc:
        raise engine.EngineError(text["bad_reply"]) from exc
    model_path = _reply_reference(reply, reference, engine)
    model = _bounded_read_json(model_path, aggregate, engine)
    if require_version:
        major = (((model or {}).get("version") or {}).get("major"))
        if major != 2:
            raise engine.EngineError(
                f"unsupported CMake codemodel version (expected major 2, got {major!r})"
            )
    return reply, model, aggregate


def _select_configuration(model: Any, build_dir: Path, engine: Any,
                          configuration: str | None = None) -> Any:
    """Select an exact configuration, or the sole configuration by default."""
    configurations = (model or {}).get("configurations")
    if not isinstance(configurations, list) or not configurations:
        raise engine.EngineError("CMake codemodel has no configurations; reconfigure")
    if configuration is not None:
        names = [entry.get("name") for entry in configurations if isinstance(entry, dict)]
        matches = [entry for entry in configurations if isinstance(entry, dict) and entry.get("name") == configuration]
        if len(matches) != 1:
            raise engine.EngineError(
                f"CMake configuration {configuration!r} must match exactly one entry in {build_dir}; available: {names}"
            )
        return matches[0]
    if len(configurations) != 1:
        raise engine.EngineError(
            "the editor requires a single-config (Ninja) build tree; "
            f"found {len(configurations)} configurations in {build_dir}"
        )
    return configurations[0]


def target_executable(
    build_dir: Path,
    target: str,
    engine: Any,
    client: str = "ludus-editor",
    *,
    configuration: str | None = None,
    require_version: bool = True,
    bounded: bool = True,
    messages: dict[str, str] | None = None,
) -> Path:
    """Resolve `target` to its single executable artifact via the File API.

    Reads the latest named reply for `client`, (optionally) verifies codemodel
    major 2, selects the explicitly named configuration or the sole one,
    finds exactly one target, and resolves its primary EXECUTABLE artifact. CMake's
    codemodel v2 target object names that artifact with `nameOnDisk` when
    there are additional outputs such as a Windows PDB. Artifact paths may
    be absolute or in custom output directories;
    they are returned as a resolved absolute path and never guessed from a
    conventional layout.

    `require_version`/`bounded`/`messages` exist so RAD can reuse this logic with
    its historical, laxer behavior and exact error strings while the editor path
    enforces the stricter bounded/version-checked contract.
    """
    text = dict(_DEFAULT_MESSAGES)
    if messages:
        text.update(messages)
    reply, model, aggregate = _latest_codemodel(
        build_dir, client, engine, require_version=require_version, messages=text
    )
    try:
        if require_version or configuration is not None:
            selected = _select_configuration(model, build_dir, engine, configuration)
            targets = selected["targets"]
        else:
            # RAD's historical shape: flatten targets across configurations.
            targets = [entry for config in model["configurations"] for entry in config["targets"]]
    except (KeyError, TypeError) as exc:
        raise engine.EngineError(text["bad_reply"]) from exc
    if bounded and len(targets) > MAX_TARGET_ENTRIES:
        raise engine.EngineError(
            f"CMake codemodel lists {len(targets)} targets, exceeding the {MAX_TARGET_ENTRIES} bound"
        )
    matches = [entry for entry in targets if isinstance(entry, dict) and entry.get("name") == target]
    if len(matches) != 1:
        raise engine.EngineError(text["not_one"].format(target=target, build_dir=build_dir))
    try:
        detail_path = _reply_reference(reply, matches[0]["jsonFile"], engine)
        detail = _bounded_read_json(detail_path, aggregate, engine) if bounded else read_json(detail_path, engine)
    except (KeyError, TypeError) as exc:
        raise engine.EngineError(text["bad_reply"]) from exc
    artifacts = detail.get("artifacts", [])
    if require_version and len(artifacts) > 1:
        # CMake File API, codemodel v2 target object: nameOnDisk identifies
        # the primary output even when the target also produces debug symbols.
        # https://cmake.org/cmake/help/v3.31/manual/cmake-file-api.7.html#codemodel-version-2-target-object
        primary = detail.get("nameOnDisk")
        artifacts = [artifact for artifact in artifacts if Path(artifact["path"]).name == primary]
    if detail.get("type") != "EXECUTABLE" or len(artifacts) != 1:
        raise engine.EngineError(text["not_executable"].format(target=target, build_dir=build_dir))
    return (build_dir / artifacts[0]["path"]).resolve()


# Target types the project-live-reload build generation resolver accepts
# (design section 5): the gameplay module (MODULE_LIBRARY) and the host
# EXECUTABLE. Resolving the actual artifact path from the codemodel avoids
# guessing platform suffixes or deriving output paths from target names.
_ARTIFACT_TYPES = ("EXECUTABLE", "MODULE_LIBRARY", "SHARED_LIBRARY")


def target_artifact(
    build_dir: Path,
    target: str,
    engine: Any,
    *,
    expected_type: str,
    client: str = "ludus-editor",
) -> Path:
    """Resolve `target` to its single artifact of `expected_type` via the File API.

    A strict, bounded, version-checked sibling of `target_executable` that also
    resolves MODULE_LIBRARY artifacts (the reloadable gameplay module) and the
    host EXECUTABLE (project-live-reload design section 5). Returns the resolved
    absolute artifact path; it never guesses a conventional layout or a platform
    suffix. `expected_type` must be one of _ARTIFACT_TYPES.

    Does NOT replace `target_executable`: the editor/RAD executable path is
    unchanged. New-path callers (the play-session build) use this to resolve the
    module and host artifacts of one generation.
    """
    if expected_type not in _ARTIFACT_TYPES:
        raise engine.EngineError(f"unsupported artifact type requested: {expected_type!r}")
    reply, model, aggregate = _latest_codemodel(build_dir, client, engine, require_version=True)
    try:
        configuration = _select_configuration(model, build_dir, engine)
        targets = configuration["targets"]
    except (KeyError, TypeError) as exc:
        raise engine.EngineError(_DEFAULT_MESSAGES["bad_reply"]) from exc
    if len(targets) > MAX_TARGET_ENTRIES:
        raise engine.EngineError(
            f"CMake codemodel lists {len(targets)} targets, exceeding the {MAX_TARGET_ENTRIES} bound"
        )
    matches = [entry for entry in targets if isinstance(entry, dict) and entry.get("name") == target]
    if len(matches) != 1:
        raise engine.EngineError(
            f"CMake target {target!r} does not resolve to exactly one target in {build_dir}"
        )
    try:
        detail_path = _reply_reference(reply, matches[0]["jsonFile"], engine)
        detail = _bounded_read_json(detail_path, aggregate, engine)
    except (KeyError, TypeError) as exc:
        raise engine.EngineError(_DEFAULT_MESSAGES["bad_reply"]) from exc
    actual_type = detail.get("type")
    if actual_type != expected_type:
        raise engine.EngineError(
            f"CMake target {target!r} is a {actual_type!r}, expected {expected_type!r}"
        )
    artifacts = detail.get("artifacts", [])
    if len(artifacts) != 1:
        raise engine.EngineError(
            f"CMake target {target!r} does not have exactly one artifact ({len(artifacts)})"
        )
    return (build_dir / artifacts[0]["path"]).resolve()


def list_executable_targets(build_dir: Path, engine: Any, client: str = "ludus-editor") -> list[str]:
    """Return sorted unique executable target names satisfying the name contract.

    Enforces the bounded-reply limits and the executable-target cap. An excessive
    executable list or an unsupported target name is a clear capability error, not
    silently filtered data: a reply with more than MAX_EXECUTABLE_TARGETS
    executables fails. Non-executable targets are simply not listed.
    """
    reply, model, aggregate = _latest_codemodel(build_dir, client, engine)
    configuration = _select_configuration(model, build_dir, engine)
    targets = configuration.get("targets")
    if not isinstance(targets, list):
        raise engine.EngineError("invalid CMake File API reply; reconfigure")
    if len(targets) > MAX_TARGET_ENTRIES:
        raise engine.EngineError(
            f"CMake codemodel lists {len(targets)} targets, exceeding the {MAX_TARGET_ENTRIES} bound"
        )
    executables: set[str] = set()
    for entry in targets:
        if not isinstance(entry, dict):
            continue
        name = entry.get("name")
        json_file = entry.get("jsonFile")
        if not isinstance(name, str) or not isinstance(json_file, str):
            continue
        detail = _bounded_read_json(_reply_reference(reply, json_file, engine), aggregate, engine)
        if detail.get("type") != "EXECUTABLE":
            continue
        if not TARGET_NAME_RE.fullmatch(name) or len(name.encode("utf-8")) > 256:
            raise engine.EngineError(
                f"CMake executable target name is unsupported by the descriptor contract: {name!r}"
            )
        executables.add(name)
        if len(executables) > MAX_EXECUTABLE_TARGETS:
            raise engine.EngineError(
                f"too many executable targets (> {MAX_EXECUTABLE_TARGETS}); refine the project"
            )
    return sorted(executables)


def verify_identity(build_dir: Path, source_dir: Path, engine: Any, client: str = "ludus-editor") -> None:
    """Verify the codemodel's source/build roots match the intended directories.

    Guards against reading a reply from an unrelated tree (design section 7:
    verify source/build identity before Run).
    """
    _, model, _ = _latest_codemodel(build_dir, client, engine)
    _verify_model_identity(model, build_dir, source_dir, engine)


def _verify_model_identity(model: Any, build_dir: Path, source_dir: Path, engine: Any) -> None:
    paths = (model or {}).get("paths") or {}
    reported_source = paths.get("source")
    reported_build = paths.get("build")
    if reported_source is None or reported_build is None:
        raise engine.EngineError("CMake codemodel is missing source/build paths; reconfigure")
    if Path(reported_source).resolve() != source_dir.resolve():
        raise engine.EngineError(
            f"CMake codemodel source root {reported_source!r} does not match expected {source_dir}"
        )
    if Path(reported_build).resolve() != build_dir.resolve():
        raise engine.EngineError(
            f"CMake codemodel build root {reported_build!r} does not match expected {build_dir}"
        )


def compiled_sources(build_dir: Path, source_dir: Path, engine: Any,
                     client: str = "ludus-quality") -> tuple[list[dict], str]:
    """Return compiled target sources, preserving repeated per-target instances.

    Shares the version, identity, reference confinement and bounded JSON reader
    with editor/debugger discovery. Headers/resources without compileGroupIndex
    are excluded; generated source ownership is retained for the caller to report.
    """
    # Thanks to Kitware, "cmake-file-api(7)", codemodel v2 target sources:
    # compileGroupIndex identifies compiled sources, independently of the database.
    # https://cmake.org/cmake/help/v3.29/manual/cmake-file-api.7.html
    reply, model, aggregate = _latest_codemodel(build_dir, client, engine)
    _verify_model_identity(model, build_dir, source_dir, engine)
    configuration = _select_configuration(model, build_dir, engine)
    sources = []
    try:
        targets = configuration["targets"]
        if not isinstance(targets, list) or len(targets) > MAX_TARGET_ENTRIES:
            raise engine.EngineError("invalid or oversized CMake source target inventory")
        for entry in targets:
            target = _bounded_read_json(_reply_reference(reply, entry["jsonFile"], engine), aggregate, engine)
            for item in target.get("sources", []):
                if "compileGroupIndex" not in item:
                    continue
                group = item["compileGroupIndex"]
                groups = target.get("compileGroups", [])
                if type(group) is not int or not isinstance(groups, list) or not 0 <= group < len(groups):
                    raise engine.EngineError("invalid CMake source compile-group reference")
                if not isinstance(item["path"], str) or not item["path"]:
                    raise engine.EngineError("invalid CMake compiled source path")
                if type(item.get("isGenerated", False)) is not bool:
                    raise engine.EngineError("invalid CMake generated-source ownership")
                sources.append({"path": str((source_dir / item["path"]).resolve()),
                                "generated": item.get("isGenerated", False)})
    except (KeyError, TypeError, AttributeError) as exc:
        raise engine.EngineError("invalid CMake compiled source inventory; reconfigure") from exc
    return sources, model["paths"]["source"]
