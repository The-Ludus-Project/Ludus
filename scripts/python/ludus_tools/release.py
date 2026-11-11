"""Native release packaging and offline publication planning for host tools."""
from __future__ import annotations

import ctypes
import errno
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import asdict, replace
from pathlib import Path
from types import SimpleNamespace

from . import __version__, descriptor, operations
from .buildlock import BuildTreeLock
from .package_native import POLICY, executable_identity, validate_native
from .package_verify import MAX_MANIFEST, file_digest, inventory, verify_package
from .release_model import canonical, fail, load_release, sha256, string
from .resolve import assert_stamp_unchanged, write_stamp
from .sdkstore import SdkStore


def _version(value: str) -> str:
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+-]{0,127}", string(value, "version", 128)):
        fail("version must be an alphanumeric release label")
    return value


def _source_state(root: Path) -> dict:
    env = {key: value for key, value in os.environ.items() if key != "BUTLER_API_KEY"}
    try:
        revision = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                                  env=env, timeout=10, check=False)
        dirty = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--untracked-files=normal"],
                               capture_output=True, env=env, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        return {"revision": "unknown", "dirty": True}
    text = revision.stdout.decode("ascii", errors="replace").strip()
    if revision.returncode or dirty.returncode or not re.fullmatch(r"[a-f0-9]{40,64}", text):
        return {"revision": "unknown", "dirty": True}
    return {"revision": text, "dirty": bool(dirty.stdout)}


def _publish_directory(staging: Path, destination: Path) -> None:
    """Native atomic no-replace rename; never expose an incomplete directory."""
    if sys.platform == "win32":
        # Python os.rename on Windows fails when the destination exists.
        # https://docs.python.org/3/library/os.html#os.rename
        try:
            os.rename(staging, destination)
        except FileExistsError:
            fail("package destination already exists", "DestinationExists")
        except OSError as exc:
            fail(f"cannot atomically publish package: {exc}", "GenerationFailed")
        return
    libc = ctypes.CDLL(None, use_errno=True)
    if sys.platform == "darwin":
        # Apple XNU sys/stdio.h, renamex_np(RENAME_EXCL=4): same-filesystem,
        # atomic no-replace publication, including a destination created in a race.
        # https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/stdio.h
        rename = getattr(libc, "renamex_np", None)
        if rename is None:
            fail("atomic package publication requires Darwin renamex_np", "MissingTools")
        rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
        argv = [os.fsencode(staging), os.fsencode(destination), 4]
    else:
        rename = getattr(libc, "renameat2", None)
        if rename is None:
            fail("atomic package publication requires Linux renameat2", "MissingTools")
        rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
        argv = [-100, os.fsencode(staging), -100, os.fsencode(destination), 1]
    rename.restype = ctypes.c_int
    if rename(*argv) != 0:
        if ctypes.get_errno() == errno.EEXIST:
            fail("package destination already exists", "DestinationExists")
        fail(f"cannot atomically publish package: {os.strerror(ctypes.get_errno())}", "GenerationFailed")


def _archive(payload: Path, archive: Path, records: list[dict], manifest: dict) -> None:
    metadata = canonical(manifest)
    if len(metadata) > MAX_MANIFEST:
        fail("payload manifest exceeds size limit", "InvalidPackage")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as writer:
        for record in sorted([*records, {"path": "build-info.json", "mode": 0o644}], key=lambda r: r["path"]):
            info = zipfile.ZipInfo(record["path"], date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o100000 | record["mode"]) << 16
            if record["path"] == "build-info.json":
                writer.writestr(info, metadata, compresslevel=9)
            else:
                with (payload / record["path"]).open("rb") as src, writer.open(info, "w", force_zip64=True) as dst:
                    while block := src.read(1024 * 1024):
                        dst.write(block)


def package_project(project: Path, *, profile: str, version: str,
                    store: SdkStore, sdk: Path | None = None, run_command=None, cancel_check=None) -> Path:
    """Build, install, validate and atomically publish an exact Release package."""
    version = _version(version)
    paths = operations.locate_project(project)
    root = paths.project_dir.resolve()
    config = load_release(root)
    if profile not in config.profiles:
        fail(f"unknown release profile {profile!r}")
    selected = config.profiles[profile]
    if selected.target_platform == "web":
        from .package_web import package_web
        return package_web(project, profile=profile, version=version, sdk=sdk, run_command=run_command, cancel_check=cancel_check)
    macos = selected.target_platform.startswith("macos-")
    if macos and sys.platform != "darwin":
        fail("macOS packaging requires a macOS host", "UnsupportedReleaseTarget")
    if macos:
        from .package_macos import POLICY as policy, executable_identity as identity, sign_payload, validate_native as validate_macos
        validator = lambda root, entry: validate_macos(root, entry, selected.target_platform, runner=runner, env=env, cancel_check=cancel_check)
    else:
        policy, identity, validator = POLICY, executable_identity, validate_native
    resolved = operations.resolve_project(project, store=store, cli_sdk_prefix=sdk, profile=selected.configure_preset)
    triples = {"linux-x64": "x86_64-linux-gnu", "macos-arm64": "arm64-apple-darwin", "macos-x64": "x86_64-apple-darwin"}
    if resolved.resolution.identity.target_triple != triples[selected.target_platform]:
        fail("SDK target does not match release profile", "SdkIncompatible")
    if resolved.resolution.identity.engine_version != resolved.descriptor.engine.version:
        fail("SDK engine version does not match project requirement", "SdkIncompatible")
    resolved.descriptor = replace(resolved.descriptor, target=selected.target, preset=selected.configure_preset)
    lock_digest = file_digest(paths.lock_path)
    descriptor_digest = file_digest(paths.descriptor_path)
    source = _source_state(root)
    runner = run_command or operations._run
    cmake = operations._cmake_executable()
    env = operations._sdk_env(resolved.resolution)
    package_parent = root / "out" / "packages" / profile
    package_parent.mkdir(parents=True, exist_ok=True)
    with BuildTreeLock(resolved.paths.build_dir), tempfile.TemporaryDirectory(prefix=".ludus-release-", dir=package_parent) as temp:
        from cmake_targets import query_codemodel, target_executable, verify_identity

        shim = SimpleNamespace(EngineError=ValueError)
        query_codemodel(resolved.paths.build_dir, "ludus-cli")
        # Configure explicitly each time: CMake/source/preset edits also matter.
        for argv in (
            [*operations.configure_argv(cmake, selected.configure_preset, resolved.paths.source_dir, resolved.paths.build_dir),
             "--fresh", "-DLudus_DIR=" + str(resolved.resolution.prefix / "lib/cmake/Ludus")],
            operations.build_argv(cmake, resolved.paths.build_dir, selected.target),
        ):
            if runner(argv, cwd=resolved.paths.source_dir, env=env) != 0:
                fail("release build failed; refusing to package a stale executable", "BuildFailed")
            assert_stamp_unchanged(resolved.paths.build_dir, resolved.resolution)
        write_stamp(resolved.paths.build_dir, resolved.resolution)
        try:
            verify_identity(resolved.paths.build_dir, resolved.paths.source_dir, shim, client="ludus-cli")
            artifact = target_executable(resolved.paths.build_dir, selected.target, shim, client="ludus-cli")
        except ValueError as exc:
            fail(str(exc), "BuildFailed")
        if not artifact.is_file() or not os.access(artifact, os.X_OK):
            fail("post-build artifact is missing or not executable", "BuildFailed")
        payload = Path(temp) / "payload"
        payload.mkdir()
        install = [cmake, "--install", str(resolved.paths.build_dir), "--config", "Release",
                   "--component", selected.install_component, "--prefix", str(payload)]
        if runner(install, cwd=resolved.paths.source_dir, env=env) != 0:
            fail("release installation failed", "BuildFailed")
        records = inventory(payload)
        if macos:
            sign_payload(payload, selected.entry_point, runner, env, cancel_check)
            records = inventory(payload)  # signing adds the sealed-resource manifest
        external = validator(payload, selected.entry_point)
        if identity(payload / selected.entry_point) != identity(artifact):
            fail("installed program sections differ from the selected CMake artifact", "InvalidPackage")
        # Policy prohibits source/build/SDK machine paths in the actual payload.
        needles = [str(p.resolve()).encode("utf-8") for p in (root, resolved.paths.source_dir,
                    resolved.paths.build_dir, resolved.resolution.prefix)]
        for record in records:
            with (payload / record["path"]).open("rb") as handle:
                tail = b""
                while block := handle.read(1024 * 1024):
                    data = tail + block
                    if any(needle in data for needle in needles):
                        fail(f"producer path in payload {record['path']!r}", "InvalidPackage")
                    tail = data[-max(map(len, needles)):]
        if (load_release(root).digest != config.digest or file_digest(paths.lock_path) != lock_digest
                or file_digest(paths.descriptor_path) != descriptor_digest or _source_state(root) != source):
            fail("project inputs changed during packaging", "Conflict")
        assert_stamp_unchanged(resolved.paths.build_dir, resolved.resolution)
        manifest = {"schemaVersion": 1, "profile": profile, "targetPlatform": selected.target_platform,
                    "entryPoint": selected.entry_point, "version": version, "source": source,
                    "engineLockSha256": lock_digest, "releaseConfigSha256": config.digest,
                    "sdk": asdict(resolved.resolution.identity), "localInputs": source["dirty"] or resolved.resolution.is_override,
                    "policy": policy, "systemLibraries": external, "files": records}
        completed = Path(temp) / "package"
        completed.mkdir()
        archive = completed / "game.zip"
        _archive(payload, archive, records, manifest)
        archive_digest = file_digest(archive)
        sidecar = {"schemaVersion": 1, "archiveSha256": archive_digest,
                   "manifestSha256": sha256(canonical(manifest)), "profile": profile, "version": version,
                   "releaseConfigSha256": config.digest, "toolVersion": __version__}
        (completed / "package.json").write_bytes(canonical(sidecar))
        (completed / "validation.json").write_bytes(canonical({
            "schemaVersion": 1, "archiveSha256": archive_digest, "policy": policy,
            "toolVersion": __version__, "checks": ["payload", "native", "clean-extraction"],
        }))
        verification = {"run_command": runner, "env": env, "cancel_check": cancel_check} if macos else {}
        verify_package(completed, **verification)
        if cancel_check:
            cancel_check()
        destination = package_parent / archive_digest
        # Lock package publication independently of the build tree in case
        # two valid projects share a chosen output profile.
        with BuildTreeLock(package_parent):
            if destination.exists() or destination.is_symlink():
                if destination.is_symlink() or verify_package(destination, **verification) != verify_package(completed, **verification):
                    fail("existing package conflicts with generated package", "Conflict")
                return destination
            _publish_directory(completed, destination)
        return destination


def publish_plan(project: Path, package: Path, *, destination: str, allow_local_inputs: bool = False, target_override: str | None = None) -> dict:
    """Offline plan only: never resolve an SDK, execute a game or authenticate."""
    paths = operations.locate_project(project)
    model = descriptor.parse_descriptor_file(paths.descriptor_path)
    if model.version != 2 or model.provider != "cmake":
        fail("publishing requires a version-2 cmake project; migrate explicitly")
    from .lockfile import parse_lock_file, validate_descriptor_lock_agreement
    if model.engine is None:
        fail("project has no engine requirement")
    validate_descriptor_lock_agreement(model.engine.version, parse_lock_file(paths.lock_path))
    config = load_release(paths.project_dir)
    from .release_model import ITCH_TARGET
    target = target_override or config.itch_target
    if target_override and (not ITCH_TARGET.fullmatch(target_override) or (config.itch_target and target_override != config.itch_target)):
        fail("itch target override is invalid or differs from configured destination", "Conflict")
    if destination not in config.destinations or target is None:
        fail(f"unknown itch.io destination {destination!r}")
    verified = verify_package(package)
    manifest = verified["manifest"]
    profile, channel = config.destinations[destination]
    if manifest["sdk"]["engine_version"] != model.engine.version:
        fail("package SDK engine version differs from project requirement", "Conflict")
    if (verified["releaseConfigSha256"] != config.digest or verified["profile"] != profile
            or manifest["engineLockSha256"] != file_digest(paths.lock_path)):
        fail("package does not match current release configuration/profile/lock", "Conflict")
    if manifest["localInputs"] and not allow_local_inputs:
        fail("package used dirty/unknown sources or an SDK override; pass --allow-local-inputs to inspect its upload plan")
    return {"operation": "offline-upload-plan", "destination": destination,
            "target": target, "channel": channel, "version": verified["version"],
            "archiveSha256": verified["archiveSha256"], "localInputs": manifest["localInputs"],
            "expandedBytes": sum(record["size"] for record in manifest["files"]),
            "argv": ["butler", "push", "<verified-private-snapshot>/game.zip",
                     f"{target}:{channel}", "--userversion", verified["version"]],
            "prerequisites": ["pinned butler setup and explicit upload authorization",
                              "existing itch.io project with appropriate visibility and authentication"]}
