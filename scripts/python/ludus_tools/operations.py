"""Shared project operation backend: configure / build / run (P07/P10).

This is the backend the installed CLI calls directly. The Editor's streaming E0
adapter (scripts/python/editor_tool.py) implements the same semantics for its
private protocol; both share: the SDK resolution precedence (resolve.py), the
per-build-tree cooperative lock (buildlock.py), the CMake File API artifact
resolver (cmake_targets.py), exact argv arrays with no shell, output bounds,
process cleanup and the rule that a failed/cancelled build can never launch an
older binary.

The CLI backend here is synchronous (prints to the terminal); it never downloads
an SDK, refreshes the lock or compiles engine sources during configure/build/run.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Optional, Sequence

from . import descriptor as desc
from .buildlock import BuildTreeLock
from .errors import (
    BUSY,
    GENERATION_FAILED,
    INVALID_PROJECT,
    ToolingError,
)
from .lockfile import LocalSettings, parse_lock_file, parse_local_settings_file, validate_descriptor_lock_agreement
from .resolve import Resolution, assert_stamp_unchanged, needs_reconfigure, resolve_sdk, write_stamp
from .sdkstore import SdkStore

# Map a descriptor preset to the native flavor and CMake build type.
from .native import (PRESET_FLAVOR, WINDOWS_CONFIGURATIONS, is_visual_studio_profile,
                     target_for_profile, validate_target)

DESCRIPTOR_NAME = "ludus.project.json"
LOCK_NAME = "ludus.lock.json"
LOCAL_SETTINGS_RELPATH = ".ludus/local.json"

# Bounded captured output per stream (defensive; the CLI streams live output).
MAX_CAPTURED_BYTES = 8 * 1024 * 1024


@dataclass
class ProjectPaths:
    project_dir: Path
    descriptor_path: Path
    source_dir: Path
    build_dir: Path

    @property
    def lock_path(self) -> Path:
        return self.project_dir / LOCK_NAME

    @property
    def local_settings_path(self) -> Path:
        return self.project_dir / LOCAL_SETTINGS_RELPATH


def locate_project(path: Path) -> ProjectPaths:
    """Accept either a project directory or a descriptor file (design: both)."""
    path = Path(path)
    if path.is_file():
        project_dir = path.parent
        descriptor_path = path
    else:
        project_dir = path
        descriptor_path = path / DESCRIPTOR_NAME
    if not descriptor_path.is_file():
        raise ToolingError(INVALID_PROJECT, f"no descriptor at {descriptor_path}")
    return ProjectPaths(
        project_dir=project_dir,
        descriptor_path=descriptor_path,
        source_dir=project_dir,  # resolved below once descriptor is parsed
        build_dir=project_dir,
    )


@dataclass
class ResolvedProject:
    paths: ProjectPaths
    descriptor: desc.Descriptor
    flavor: str
    build_type: str
    resolution: Resolution


def _build_dir_for(project_dir: Path, source_dir: Path, preset: str) -> Path:
    # Mirror the template preset binaryDir: <source>/out/build/<preset>.
    return (source_dir / "out" / "build" / preset).resolve()


def resolve_project(
    path: Path,
    *,
    store: SdkStore,
    cli_sdk_prefix: Optional[Path] = None,
    profile: Optional[str] = None,
    enforce_host_toolchain: bool = True,
) -> ResolvedProject:
    """Open+validate a project and resolve its SDK for the requested profile.

    Open reads/validates metadata only; this does not configure or build. When
    ``enforce_host_toolchain`` is set (the default for configure/build/run), the
    resolved SDK's compiler id/version and C++ runtime ABI are checked against
    the consuming toolchain before configure, so an incompatible SDK is rejected
    with expected-vs-actual rather than failing at link time (P03).
    """
    paths = locate_project(path)
    descriptor = desc.parse_descriptor_file(paths.descriptor_path)

    if descriptor.version == 1:
        raise ToolingError(
            INVALID_PROJECT,
            "this is a version-1 project; run 'ludus project migrate' before configure/build/run",
        )
    if descriptor.engine is None:
        raise ToolingError(INVALID_PROJECT, "version-2 'cmake' project is missing its engine requirement")

    preset = profile or descriptor.preset
    if preset not in PRESET_FLAVOR:
        raise ToolingError(INVALID_PROJECT, f"unknown profile/preset {preset!r}")
    flavor = PRESET_FLAVOR[preset]
    descriptor = replace(descriptor, preset=preset)

    source_dir = (paths.project_dir / descriptor.source_dir).resolve()
    build_dir = _build_dir_for(paths.project_dir, source_dir, preset)
    paths.source_dir = source_dir
    paths.build_dir = build_dir

    # Validate the lock agrees with the descriptor (never refreshed here).
    lock = parse_lock_file(paths.lock_path)
    validate_descriptor_lock_agreement(descriptor.engine.version, lock)
    local_settings = parse_local_settings_file(paths.local_settings_path)

    requested_target = target_for_profile(preset)
    target_triple = _target_triple_for_lock(lock, flavor, requested_target) or requested_target
    # Resolve once without the host gate to learn the SDK identity, then (when
    # enforcing) re-resolve with a host-toolchain reference so an incompatible
    # compiler/runtime ABI is rejected before configure. Resolving twice is cheap
    # (metadata + a content stamp) and keeps the precedence logic in one place.
    resolution = resolve_sdk(
        target=target_triple,
        flavor=flavor,
        lock=lock,
        local_settings=local_settings,
        store=store,
        cli_prefix=cli_sdk_prefix,
        required_features=descriptor.engine.features or None,
    )
    validate_target(resolution.identity, preset)
    if resolution.identity.engine_version != descriptor.engine.version:
        raise ToolingError(INVALID_PROJECT, "SDK engine version does not match the project requirement")
    missing = set(descriptor.engine.components) - set(resolution.identity.components)
    if missing:
        raise ToolingError(INVALID_PROJECT, f"SDK lacks required components: {sorted(missing)}")
    if enforce_host_toolchain:
        from .identity import detect_host_toolchain

        from .project_setup import read_object, preset_for

        selected = preset_for(source_dir, preset)
        entries = read_object(source_dir / "CMakeUserPresets.json").get("configurePresets", [])
        compiler = next((p.get("cacheVariables", {}).get("CMAKE_CXX_COMPILER")
                         for p in entries if p.get("name") == selected), None)
        host = detect_host_toolchain(resolution.identity, cxx=compiler, strict=True)
        resolution = resolve_sdk(
            target=target_triple,
            flavor=flavor,
            lock=lock,
            local_settings=local_settings,
            store=store,
            cli_prefix=cli_sdk_prefix,
            required_features=descriptor.engine.features or None,
            host=host,
        )
    return ResolvedProject(
        paths=paths,
        descriptor=descriptor,
        flavor=flavor,
        build_type=_build_type(flavor),
        resolution=resolution,
    )


def _target_triple_for_lock(lock, flavor, target: Optional[str] = None) -> Optional[str]:
    aliases = {"x86_64-linux-gnu": "linux-x64", "arm64-apple-darwin": "macos-arm64",
               "x86_64-apple-darwin": "macos-x64"}
    for p in lock.packages:
        if p.flavor == flavor and (target is None or p.target in (target, aliases.get(target))):
            return p.target
    return None


def _build_type(flavor: str) -> str:
    return WINDOWS_CONFIGURATIONS[flavor]


# --- CMake drivers (exact argv, no shell) ------------------------------------


def _cmake_executable() -> str:
    exe = shutil.which("cmake")
    if exe is None:
        raise ToolingError("MissingTools", "cmake was not found on PATH")
    return exe


def _project_cmake(resolved: ResolvedProject) -> str:
    from .project_setup import cmake_for
    selected = cmake_for(resolved.paths.source_dir, resolved.descriptor.preset, "")
    return selected or _cmake_executable()


def configure_argv(cmake: str, preset: str, source_dir: Path, build_dir: Path) -> list[str]:
    from .project_setup import preset_for
    return [cmake, "--preset", preset_for(source_dir, preset), "-S", str(source_dir), "-B", str(build_dir)]


def _project_configure_argv(cmake: str, resolved: ResolvedProject) -> list[str]:
    paths = resolved.paths
    argv = configure_argv(cmake, resolved.descriptor.preset, paths.source_dir, paths.build_dir)
    if is_visual_studio_profile(resolved.descriptor.preset):
        # Explicit SDK resolution also takes precedence over a repaired local preset.
        prefix = resolved.resolution.prefix
        argv.extend((f"-DCMAKE_PREFIX_PATH={prefix.as_posix()}",
                     f"-DLudus_DIR={(prefix / 'lib/cmake/Ludus').as_posix()}"))
    return argv


def _project_needs_reconfigure(resolved: ResolvedProject) -> bool:
    build_dir = resolved.paths.build_dir
    if needs_reconfigure(build_dir, resolved.resolution):
        return True
    if is_visual_studio_profile(resolved.descriptor.preset):
        from .project_setup import cache_value

        cache = build_dir / "CMakeCache.txt"
        if not cache.is_file():
            return True
        configured_sdk = cache_value(cache, "Ludus_DIR")
        return not configured_sdk or Path(configured_sdk) != resolved.resolution.prefix / "lib/cmake/Ludus"
    return False


def build_argv(cmake: str, build_dir: Path, target: str, *, configuration: str | None = None) -> list[str]:
    argv = [cmake, "--build", str(build_dir), "--target", target]
    if configuration is not None:
        argv.extend(("--config", configuration))
    return argv


def _project_configuration(resolved: ResolvedProject) -> str | None:
    return resolved.build_type if is_visual_studio_profile(resolved.descriptor.preset) else None


def _run(argv: Sequence[str], *, cwd: Path, env: dict, echo: bool = True) -> int:
    """Run a command with exact argv, no shell. Streams output live.

    Returns the child's exit status. A child terminated by a signal yields a
    negative status (nonzero), which callers treat as failure. On Ctrl-C
    (KeyboardInterrupt) the child is terminated and the interrupt re-raised so an
    operation unwinds without ever proceeding to a launch.
    """
    if echo:
        print("+ " + " ".join(argv), file=sys.stderr)
    proc = subprocess.Popen(list(argv), cwd=str(cwd), env=env, shell=False)
    try:
        return proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        raise


def _sdk_env(resolution: Resolution) -> dict:
    env = {key: value for key, value in os.environ.items() if key != "BUTLER_API_KEY"}
    # The generated preset reads $env{LUDUS_SDK_PREFIX}. The backend supplies the
    # validated prefix; raw CMake users set it themselves (documented).
    env["LUDUS_SDK_PREFIX"] = str(resolution.prefix)
    return env


def _validate_selected_presets(cmake: str, resolved: ResolvedProject, env: dict) -> None:
    from .cmake_setup import validate_project_presets

    try:
        validate_project_presets(cmake, resolved.paths.source_dir, env, resolved.descriptor.preset)
    except ValueError as exc:
        raise ToolingError(INVALID_PROJECT, str(exc)) from exc


# --- Public operations -------------------------------------------------------


def op_configure(resolved: ResolvedProject) -> int:
    cmake = _project_cmake(resolved)
    env = _sdk_env(resolved.resolution)
    _validate_selected_presets(cmake, resolved, env)
    print(resolved.resolution.describe(), file=sys.stderr)
    with BuildTreeLock(resolved.paths.build_dir):
        try:
            from cmake_targets import query_codemodel  # type: ignore

            query_codemodel(resolved.paths.build_dir, "ludus-cli")
        except Exception:
            pass  # File API query is best-effort before first configure
        code = _run(
            _project_configure_argv(cmake, resolved),
            cwd=resolved.paths.source_dir,
            env=env,
        )
        if code == 0:
            write_stamp(resolved.paths.build_dir, resolved.resolution)
        return code


def op_build(resolved: ResolvedProject) -> int:
    cmake = _project_cmake(resolved)
    env = _sdk_env(resolved.resolution)
    _validate_selected_presets(cmake, resolved, env)
    print(resolved.resolution.describe(), file=sys.stderr)
    with BuildTreeLock(resolved.paths.build_dir):
        # Reconfigure if the SDK inputs changed since last configure (mutable
        # local prefixes). Path equality is insufficient; we compare the stamp.
        if _project_needs_reconfigure(resolved):
            print("SDK inputs changed; reconfiguring", file=sys.stderr)
            try:
                from cmake_targets import query_codemodel  # type: ignore

                query_codemodel(resolved.paths.build_dir, "ludus-cli")
            except Exception:
                pass
            code = _run(
                _project_configure_argv(cmake, resolved),
                cwd=resolved.paths.source_dir,
                env=env,
            )
            if code != 0:
                return code
            write_stamp(resolved.paths.build_dir, resolved.resolution)
        # The SDK must not change under us mid-operation.
        assert_stamp_unchanged(resolved.paths.build_dir, resolved.resolution)
        code = _run(
            build_argv(cmake, resolved.paths.build_dir, resolved.descriptor.target,
                       configuration=_project_configuration(resolved)),
            cwd=resolved.paths.source_dir,
            env=env,
        )
        return code


def op_run(resolved: ResolvedProject) -> int:
    # run ensures a successful current build first (E0 build-and-run semantics).
    with BuildTreeLock(resolved.paths.build_dir):
        pass  # ensure the tree is free before we start (lock re-taken per op)
    # A failed OR cancelled build must never launch an older binary (P10). A
    # nonzero code covers a compile failure; a signal-terminated build yields a
    # negative (nonzero) code; and a Ctrl-C raises KeyboardInterrupt out of
    # op_build before we reach here — in every case we return/propagate without
    # launching.
    build_code = op_build(resolved)
    if build_code != 0:
        print("build failed or was cancelled; refusing to launch a stale binary", file=sys.stderr)
        return build_code

    artifact = resolve_artifact(resolved)
    if not artifact.is_file():
        raise ToolingError(GENERATION_FAILED, f"Built executable is missing: {artifact}; reconfigure and rebuild")
    if not os.access(artifact, os.X_OK):
        raise ToolingError(GENERATION_FAILED, f"resolved artifact is not executable: {artifact}")

    run_cwd = (resolved.paths.project_dir / resolved.descriptor.run_cwd).resolve()
    argv = [str(artifact), *resolved.descriptor.run_args]
    env = _sdk_env(resolved.resolution)
    return _run(argv, cwd=run_cwd, env=env)


def resolve_artifact(resolved: ResolvedProject) -> Path:
    """Resolve the executable artifact via the CMake File API (never guessed)."""
    from types import SimpleNamespace

    from cmake_targets import target_executable  # type: ignore

    engine = SimpleNamespace(EngineError=ValueError)
    try:
        return target_executable(
            resolved.paths.build_dir,
            resolved.descriptor.target,
            engine,
            client="ludus-cli",
            configuration=_project_configuration(resolved),
        )
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise ToolingError(GENERATION_FAILED, f"Cannot resolve the built executable: {exc}") from exc
