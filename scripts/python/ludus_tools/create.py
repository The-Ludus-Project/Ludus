"""Project creation and explicit v1->v2 migration (P08/P09).

Creation stages a complete project (descriptor + unresolved/resolved lock +
rendered template) into a sibling directory and atomically publishes it to an
absent destination. Failure or cancellation leaves no partial project and never
overwrites unrelated files.

``create --engine <release>`` derives the engine requirement from the release and
writes an unresolved lock when no published release metadata is available yet.
``create --sdk <prefix>`` derives the requirement from a validated local SDK
manifest and records the local prefix ONLY in ignored ``.ludus/local.json`` — the
committed lock stays release-oriented (and unresolved until a release exists).

Migration is an explicit, failure-preserving command: it reads a v1 descriptor,
requires an engine selection, and stages BOTH the v2 descriptor and the lock
under a project lock, recovering safely from a partial multi-file commit. Open
never migrates silently.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from . import descriptor as desc
from .buildlock import BuildTreeLock
from .errors import INVALID_PROJECT, MIGRATION_FAILED, ToolingError
from .identity import SdkIdentity, load_prefix_manifest
from .lockfile import Lock, LocalSettings, unresolved_lock
from .templates import ProjectInputs, get_template, publish_project, render_files, stage_project, TemplateFile

DESCRIPTOR_NAME = "ludus.project.json"
LOCK_NAME = "ludus.lock.json"
LOCAL_SETTINGS_RELPATH = ".ludus/local.json"


@dataclass
class CreateResult:
    destination: Path
    descriptor: desc.Descriptor
    lock: Lock


def _canonical_descriptor_json(d: desc.Descriptor) -> str:
    obj: dict = {
        "version": 2,
        "name": d.name,
        "provider": d.provider,
        "source_dir": d.source_dir,
        "preset": d.preset,
        "target": d.target,
        "run": {"cwd": d.run_cwd, "args": list(d.run_args)},
    }
    if d.engine is not None:
        obj["engine"] = {
            "version": d.engine.version,
            "components": list(d.engine.components),
            "features": list(d.engine.features),
        }
    if d.template is not None:
        obj["template"] = {"id": d.template.id, "version": d.template.version}
    return json.dumps(obj, indent=2, ensure_ascii=False) + "\n"


def create_project(
    destination: Path,
    *,
    name: str,
    template_id: str,
    engine_version: str,
    components: Optional[list[str]] = None,
    preset: Optional[str] = None,
    local_sdk_prefix: Optional[Path] = None,
    cancel_check: Optional[Callable[[], None]] = None,
    verify_staged: Optional[Callable[[Path], None]] = None,
    release: bool = False,
    itch_target: Optional[str] = None,
    ci: Optional[str] = None,
    tools_ref: Optional[str] = None,
) -> CreateResult:
    """Create a new version-2 project at an absent destination, atomically.

    ``ci="github"`` also stages the build/test workflow pinned to ``tools_ref``;
    it is refused before any work when the destination is inside a Git work tree.
    """
    destination = Path(destination)
    from .ci_setup import validate_creation_ci

    validate_creation_ci(destination, ci, tools_ref)
    components = components or ["FoundationBase"]
    template = get_template(template_id)

    # If a local SDK prefix is given, derive/confirm identity from its manifest.
    local_identity: Optional[SdkIdentity] = None
    if local_sdk_prefix is not None:
        local_identity = load_prefix_manifest(Path(local_sdk_prefix))
        # Derive the engine version from the validated local manifest (design:
        # "create --sdk derives the engine requirement from the validated local
        # manifest").
        engine_version = engine_version or local_identity.engine_version

    from .native import default_profile, profile_for_identity

    preset = preset or (profile_for_identity(local_identity) if local_identity else default_profile())
    target = _derive_target(name)

    engine_req = desc.EngineRequirement(version=engine_version, components=list(components), features=[])
    descriptor = desc.Descriptor(
        version=2,
        name=name,
        provider="cmake",
        source_dir=".",
        preset=preset,
        target=target,
        run_cwd=".",
        run_args=[],
        engine=engine_req,
        template=desc.TemplateRef(id=template.id, version=template.version),
    )
    # Validate the descriptor we are about to write through the real parser.
    desc.parse_descriptor_bytes(_canonical_descriptor_json(descriptor).encode("utf-8"))

    # Lock: unresolved until a published release exists (never fabricate hashes).
    lock = unresolved_lock(engine_version, template.version)

    rendered = render_files(template, ProjectInputs(
        name=name, target=target, engine_version=engine_version, components=components, preset=preset,
    ))
    if itch_target is not None and not release:
        raise ToolingError(INVALID_PROJECT, "--itch-target requires --release")
    if release:
        from .release_template import release_files

        platform = "linux-x64"
        if preset.startswith("macos-"):
            from .native import target_for_profile
            triple = local_identity.target_triple if local_identity else target_for_profile(preset)
            platforms = {"arm64-apple-darwin": "macos-arm64", "x86_64-apple-darwin": "macos-x64"}
            if triple not in platforms:
                raise ToolingError(INVALID_PROJECT, "macOS release creation requires a macOS SDK")
            platform = platforms[triple]
        rendered.extend(release_files(target, itch_target, platform=platform))
        for file in rendered:
            if file.relpath == "CMakeLists.txt":
                file.content += '\ninclude(cmake/GameRelease.cmake)\n'
    if ci is not None:
        from .ci_setup import ci_files, platform_for_preset

        rendered.extend(ci_files(platform_for_preset(preset, local_identity.target_triple if local_identity else None),
                                 tools_ref))
    extra = [
        TemplateFile(DESCRIPTOR_NAME, _canonical_descriptor_json(descriptor)),
        TemplateFile(LOCK_NAME, lock.serialize().decode("utf-8")),
    ]
    # If a local SDK was supplied, write it into the ignored local settings so a
    # build works immediately against the override. The committed lock stays
    # release-oriented and unresolved.
    if local_sdk_prefix is not None and local_identity is not None:
        settings = LocalSettings()
        settings.set_override(
            target=local_identity.target_triple or "linux-x64",
            flavor=local_identity.flavor,
            prefix=str(Path(local_sdk_prefix).resolve()),
        )
        extra.append(TemplateFile(LOCAL_SETTINGS_RELPATH, settings.serialize().decode("utf-8")))

    staged = stage_project(rendered, destination, extra_files=extra, cancel_check=cancel_check)
    if cancel_check is not None or verify_staged is not None:
        try:
            if verify_staged is not None:
                verify_staged(staged.staging)
                # Build trees and presets contain staging paths. Regenerate at
                # the published location on first configure, never reuse them.
                import shutil
                shutil.rmtree(staged.staging / "out", ignore_errors=True)
            if cancel_check is not None:
                cancel_check()
        except BaseException:
            from .templates import discard_staging

            discard_staging(staged)
            raise
    published = publish_project(staged)
    return CreateResult(destination=published, descriptor=descriptor, lock=lock)


def _derive_target(name: str) -> str:
    # Produce a valid CMake target id (ASCII per TARGET_NAME_RE) from the human
    # name. Non-ASCII characters (e.g. Unicode project names) are replaced so the
    # derived target still matches [A-Za-z0-9_][A-Za-z0-9_.+-]*.
    cleaned = []
    for ch in name:
        if ("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9") or ch in "_.+-":
            cleaned.append(ch)
        else:
            cleaned.append("_")
    token = "".join(cleaned).strip("._+-")
    if not token or not (("a" <= token[0] <= "z") or ("A" <= token[0] <= "Z")
                         or ("0" <= token[0] <= "9") or token[0] == "_"):
        token = f"app_{token}" if token else "app"
    return token[:128]


# --- Migration ---------------------------------------------------------------


def migrate_v1_to_v2(
    project_dir: Path,
    *,
    engine_version: str,
    components: Optional[list[str]] = None,
) -> CreateResult:
    """Explicitly migrate a v1 project to v2, staging descriptor+lock together.

    Preserves relative paths and launch settings. Writes both files via a
    staged, recoverable paired commit: a crash mid-commit leaves recoverable
    ``.migrating`` artifacts rather than a half-migrated project.
    """
    project_dir = Path(project_dir)
    descriptor_path = project_dir / DESCRIPTOR_NAME
    if not descriptor_path.is_file():
        raise ToolingError(INVALID_PROJECT, f"no descriptor at {descriptor_path}")

    current = desc.parse_descriptor_file(descriptor_path)
    if current.version == 2:
        raise ToolingError(MIGRATION_FAILED, "project is already version 2")
    if current.version != 1:
        raise ToolingError(MIGRATION_FAILED, f"cannot migrate version {current.version}")
    if not engine_version:
        raise ToolingError(MIGRATION_FAILED, "migration requires an engine selection (--engine)")

    components = components or ["FoundationBase"]
    # v1 provider "ludus" is the engine-developer workflow and never acquires a
    # downloaded engine dependency; refuse to attach an engine requirement to it.
    if current.provider == "ludus":
        raise ToolingError(
            MIGRATION_FAILED,
            "provider 'ludus' projects stay engine-developer projects and do not migrate to a "
            "release-locked v2 'cmake' project; convert the provider explicitly if intended",
        )

    engine_req = desc.EngineRequirement(version=engine_version, components=list(components), features=[])
    migrated = desc.Descriptor(
        version=2,
        name=current.name,
        provider=current.provider,
        source_dir=current.source_dir,
        preset=current.preset if current.preset in desc.PRESETS_V2 else "linux-clang-development",
        target=current.target,
        run_cwd=current.run_cwd,
        run_args=list(current.run_args),
        engine=engine_req,
        template=desc.TemplateRef(id="migrated", version=1),
    )
    desc.parse_descriptor_bytes(_canonical_descriptor_json(migrated).encode("utf-8"))
    lock = unresolved_lock(engine_version, template_version=1)

    # Hold the SAME cooperative build-tree lock the Editor/CLI use so a migration
    # cannot interleave with a concurrent save/build on the project, then commit
    # the descriptor+lock as a recoverable paired write (design: "stage both
    # descriptor and lock under a project lock, recover safely from partial
    # multi-file commits").
    build_dir = (project_dir / current.source_dir / "out" / "build" / migrated.preset).resolve()
    try:
        with BuildTreeLock(build_dir):
            _paired_commit(
                project_dir,
                {
                    DESCRIPTOR_NAME: _canonical_descriptor_json(migrated).encode("utf-8"),
                    LOCK_NAME: lock.serialize(),
                },
            )
    except ToolingError:
        raise
    return CreateResult(destination=project_dir, descriptor=migrated, lock=lock)


# Sentinel/journal file names used to make the paired descriptor+lock commit
# crash-recoverable. The backup preserves the pre-migration descriptor so an
# interrupted commit can be rolled back to a consistent v1 state.
_MIGRATING_SUFFIX = ".migrating"
_BACKUP_SUFFIX = ".premigrate"


def _paired_commit(project_dir: Path, files: dict[str, bytes]) -> None:
    """Commit descriptor+lock as a crash-recoverable paired write.

    Protocol:
      1. Write each new file to ``<name>.migrating``.
      2. Back up the current descriptor to ``<descriptor>.premigrate`` (so a
         crash mid-commit can be rolled back to the original v1 state).
      3. ``os.replace`` the lock first, then the descriptor LAST. The descriptor
         is the commit point: a reader treats a v2 descriptor as authoritative,
         so the lock must already be in place when the descriptor flips.
      4. On success remove the temporaries and the backup.

    If the process dies between steps, ``recover_partial_migration`` restores a
    consistent state: a surviving backup + an un-flipped descriptor means roll
    back; a flipped v2 descriptor with its lock present means roll forward
    (just clean the journal).
    """
    import os as _os

    descriptor_path = project_dir / DESCRIPTOR_NAME
    lock_path = project_dir / LOCK_NAME
    descriptor_tmp = project_dir / f"{DESCRIPTOR_NAME}{_MIGRATING_SUFFIX}"
    lock_tmp = project_dir / f"{LOCK_NAME}{_MIGRATING_SUFFIX}"
    backup = project_dir / f"{DESCRIPTOR_NAME}{_BACKUP_SUFFIX}"

    original = descriptor_path.read_bytes() if descriptor_path.is_file() else None
    try:
        descriptor_tmp.write_bytes(files[DESCRIPTOR_NAME])
        lock_tmp.write_bytes(files[LOCK_NAME])
        if original is not None:
            backup.write_bytes(original)
        # Lock first, descriptor (commit point) last.
        _os.replace(lock_tmp, lock_path)
        _os.replace(descriptor_tmp, descriptor_path)
    except OSError as exc:
        # Roll back: restore the original descriptor if we had flipped nothing,
        # and remove the journal artifacts.
        if original is not None and backup.is_file():
            try:
                _os.replace(backup, descriptor_path)
            except OSError:
                pass
        for tmp in (descriptor_tmp, lock_tmp):
            try:
                if tmp.exists():
                    tmp.unlink()
            except OSError:
                pass
        raise ToolingError(MIGRATION_FAILED, f"migration commit failed: {exc}") from exc

    # Success: clear the journal.
    for artifact in (descriptor_tmp, lock_tmp, backup):
        try:
            if artifact.exists():
                artifact.unlink()
        except OSError:
            pass


def recover_partial_migration(project_dir: Path) -> list[str]:
    """Restore a consistent state after an interrupted migration.

    Returns a list of human-readable actions taken. Logic:

    * If a ``<descriptor>.premigrate`` backup exists, a commit was interrupted.
      Decide roll-forward vs roll-back by whether the migration actually
      completed: a committed v2 descriptor WITH its lock present is complete
      (roll forward — just clear the journal); otherwise restore the original
      descriptor from the backup (roll back) and remove any partial lock.
    * Always discard leftover ``.migrating`` temporaries.
    """
    import os as _os

    project_dir = Path(project_dir)
    actions: list[str] = []
    descriptor_path = project_dir / DESCRIPTOR_NAME
    lock_path = project_dir / LOCK_NAME
    backup = project_dir / f"{DESCRIPTOR_NAME}{_BACKUP_SUFFIX}"

    if backup.is_file():
        committed_v2 = False
        if descriptor_path.is_file():
            try:
                parsed = desc.parse_descriptor_file(descriptor_path)
                committed_v2 = parsed.version == 2 and lock_path.is_file()
            except ToolingError:
                committed_v2 = False
        if committed_v2:
            # Roll forward: the descriptor flipped to v2 and the lock is present.
            try:
                backup.unlink()
                actions.append("completed migration confirmed; cleared backup")
            except OSError:
                pass
        else:
            # Roll back to the pre-migration descriptor and drop a partial lock.
            try:
                _os.replace(backup, descriptor_path)
                actions.append("rolled back descriptor to pre-migration state")
            except OSError:
                pass
            if lock_path.is_file():
                try:
                    lock_path.unlink()
                    actions.append("removed partially written lock")
                except OSError:
                    pass

    for tmp in project_dir.glob(f"*{_MIGRATING_SUFFIX}"):
        try:
            tmp.unlink()
            actions.append(f"discarded {tmp.name}")
        except OSError:
            pass
    return actions
