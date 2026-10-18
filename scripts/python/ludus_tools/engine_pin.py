"""Engine pin diagnosis shared by the CLI, the Editor adapter and ``init``.

A version-2 project builds against a machine-local engine install recorded in
ignored ``.ludus/local.json`` (``sdk_overrides``: target, flavor, prefix). The
Editor and CLI run from a trusted tooling checkout; the engine install records
the revision it was built from in ``share/Ludus/LudusSdkManifest.json``.

The tooling checkout's ``HEAD`` is the single source of truth for "this
editor's engine". Comparing it with the pinned install's manifest revision
detects drift without guessing from paths. Installs below
``<tooling>/out/install`` are owned by that checkout, so automatic repair and
``init`` may refresh them; any other install was chosen explicitly and is only
reported, never silently replaced.

Read-only diagnosis never configures, builds or writes files.
"""
from __future__ import annotations

import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from .errors import INVALID_PROJECT, MANIFEST_INVALID, ToolingError
from .identity import MANIFEST_RELPATH, SdkIdentity, load_prefix_manifest

# Pin states reported to the CLI and Editor. The strings are protocol values.
PIN_CURRENT = "current"
PIN_STALE = "stale"
PIN_INVALID = "invalid"
PIN_UNKNOWN = "unknown"
PIN_STATES = (PIN_CURRENT, PIN_STALE, PIN_INVALID, PIN_UNKNOWN)

# Revisions written when git was unavailable at configure time.
UNKNOWN_REVISIONS = ("", "unknown")
# Shortest abbreviated revision accepted as a prefix match.
MINIMUM_REVISION_PREFIX = 7
GIT_TIMEOUT_SECONDS = 10

EDITOR_INSTALL_RELPATH = Path("out/install")
# Files that only a CMake build tree contains; an install never has them.
BUILD_TREE_MARKERS = ("CMakeCache.txt", "build.ninja", "CMakeFiles")
# The build tree's configured manifest lives here before installation.
BUILD_TREE_MANIFEST_RELPATH = Path("cmake/LudusSdkManifest.json")
NOT_AN_INSTALL = "NotAnInstall"


def checkout_revision(root: Path) -> str:
    """Return the trusted tooling checkout's HEAD, or "" when git is unavailable."""
    try:
        result = subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True,
                                text=True, check=False, timeout=GIT_TIMEOUT_SECONDS)
    except (OSError, subprocess.SubprocessError):
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def is_known_revision(revision: Optional[str]) -> bool:
    return revision is not None and revision.strip() not in UNKNOWN_REVISIONS


def are_revisions_equal(first: Optional[str], second: Optional[str]) -> bool:
    """Compare full or abbreviated revisions; unknown revisions never match."""
    if not is_known_revision(first) or not is_known_revision(second):
        return False
    first = first.strip().lower()
    second = second.strip().lower()
    shorter = min(len(first), len(second))
    if shorter < MINIMUM_REVISION_PREFIX:
        return first == second
    return first[:shorter] == second[:shorter]


def editor_install_root(tooling_root: Path) -> Path:
    return (Path(tooling_root) / EDITOR_INSTALL_RELPATH).resolve()


def is_editor_owned(prefix: Path, tooling_root: Path) -> bool:
    """True when ``prefix`` is an install directly below the checkout's out/install."""
    try:
        resolved = Path(prefix).resolve()
    except OSError:
        return False
    return resolved.parent == editor_install_root(tooling_root)


def is_inside_checkout_outputs(prefix: Path, tooling_root: Path) -> bool:
    """True for any folder below ``<tooling>/out`` (build trees, installs, staging)."""
    try:
        Path(prefix).resolve().relative_to((Path(tooling_root) / "out").resolve())
    except (OSError, ValueError):
        return False
    return True


def describe_prefix_problem(prefix: Path) -> str:
    """Explain why ``prefix`` is not an engine install, or return "" when it is one.

    Hand-edited overrides commonly point at the build folder (``out/build/...``)
    or at a folder that was never installed; both are named explicitly.
    """
    prefix = Path(prefix)
    if not prefix.is_dir():
        return f"Engine install folder does not exist: {prefix}"
    if (prefix / MANIFEST_RELPATH).is_file():
        return ""
    if any((prefix / marker).exists() for marker in BUILD_TREE_MARKERS) or (prefix / BUILD_TREE_MANIFEST_RELPATH).is_file():
        return (f"{prefix} is a CMake build folder, not an engine install. Choose an install folder "
                "(for example out/install/<profile>-<id>), or use Project > Repair Project Setup > "
                "Build and install the engine used by this editor.")
    return f"{prefix} is not a Ludus engine install: {MANIFEST_RELPATH} is missing."


def validate_install_prefix(prefix: Path) -> SdkIdentity:
    """Load an install's identity, rejecting build folders with an actionable message."""
    problem = describe_prefix_problem(Path(prefix))
    if problem:
        raise ToolingError(NOT_AN_INSTALL if "build folder" in problem else MANIFEST_INVALID, problem)
    return load_prefix_manifest(Path(prefix))


@dataclass
class EnginePin:
    """The project's selected engine install compared with this editor's engine."""

    state: str
    installation: str
    pinned_revision: str
    editor_revision: str
    is_editor_owned: bool
    message: str

    def to_json(self) -> dict:
        return {
            "state": self.state,
            "installation": self.installation,
            "pinned_revision": self.pinned_revision,
            "editor_revision": self.editor_revision,
            "editor_owned": self.is_editor_owned,
            "message": self.message,
        }


def classify_pin(prefix: Path, identity: Optional[SdkIdentity], *, tooling_root: Path,
                 editor_revision: str, problem: str = "") -> EnginePin:
    """Classify one selected install against the tooling checkout revision."""
    owned = is_editor_owned(prefix, tooling_root)
    short_editor = editor_revision[:MINIMUM_REVISION_PREFIX]
    if identity is None:
        # A broken selection inside this checkout's outputs (typically a
        # hand-edited path to out/build/<profile>) is this editor's to replace.
        owned = owned or is_inside_checkout_outputs(prefix, tooling_root)
        return EnginePin(PIN_INVALID, str(prefix), "", editor_revision, owned,
                         (problem or f"The selected engine install cannot be used: {prefix}") +
                         " Use this editor's engine to rebuild, install and select a matching engine.")
    pinned = identity.source_revision
    if not is_known_revision(pinned) or not is_known_revision(editor_revision):
        return EnginePin(PIN_UNKNOWN, str(prefix), pinned, editor_revision, owned,
                         "The engine revision cannot be compared; the install or this checkout has no git revision.")
    if are_revisions_equal(pinned, editor_revision):
        return EnginePin(PIN_CURRENT, str(prefix), pinned, editor_revision, owned,
                         f"The project's engine matches this editor ({short_editor}).")
    short_pinned = pinned[:MINIMUM_REVISION_PREFIX]
    if owned:
        detail = ("This editor owns that install, so Use this editor's engine (or Automatic repair) "
                  "rebuilds and reinstalls it.")
    else:
        detail = ("That install was selected explicitly and is left unchanged. Use this editor's engine to "
                  "build, install and select the engine from this checkout instead.")
    return EnginePin(PIN_STALE, str(prefix), pinned, editor_revision, owned,
                     f"The project's engine install was built from {short_pinned}, but this editor runs "
                     f"{short_editor}. Games build against the older engine. {detail}")


def selected_override_prefix(project_path: Path, profile: Optional[str] = None) -> Optional[Path]:
    """Return the saved local override prefix for the project's profile, without validating it."""
    from .descriptor import parse_descriptor_file
    from .lockfile import parse_local_settings_file
    from .native import PRESET_FLAVOR, target_for_profile
    from .operations import locate_project

    paths = locate_project(Path(project_path))
    preset = profile or parse_descriptor_file(paths.descriptor_path).preset
    flavor = PRESET_FLAVOR.get(preset)
    if flavor is None:
        raise ToolingError(INVALID_PROJECT, f"unknown profile/preset {preset!r}")
    settings = parse_local_settings_file(paths.local_settings_path)
    override = settings.find(target_for_profile(preset), flavor)
    if override is None:
        # Older local settings may carry a lock alias target; match the flavor alone.
        override = next((entry for entry in settings.overrides if entry.flavor == flavor), None)
    return Path(override.prefix) if override is not None else None


def diagnose_engine_pin(project_path: Path, *, tooling_root: Path, profile: Optional[str] = None,
                        editor_revision: Optional[str] = None) -> Optional[EnginePin]:
    """Compare the project's selected engine with this editor's engine; never raises or writes.

    Returns None when the project selects no local install (for example a locked
    release from the shared store) or cannot be read; setup checks report those.
    """
    from .operations import resolve_project
    from .sdkstore import SdkStore

    revision = checkout_revision(Path(tooling_root)) if editor_revision is None else editor_revision
    try:
        prefix = selected_override_prefix(project_path, profile)
    except ToolingError:
        return None
    if prefix is None:
        return None
    problem = describe_prefix_problem(prefix)
    if problem:
        return classify_pin(prefix, None, tooling_root=tooling_root, editor_revision=revision, problem=problem)
    try:
        resolved = resolve_project(Path(project_path), store=SdkStore(), profile=profile,
                                   enforce_host_toolchain=False)
    except ToolingError as exc:
        return classify_pin(prefix, None, tooling_root=tooling_root, editor_revision=revision,
                            problem=f"The selected engine install does not match this project: {exc.message}")
    return classify_pin(resolved.resolution.prefix, resolved.resolution.identity, tooling_root=tooling_root,
                        editor_revision=revision)


def should_refresh_automatically(pin: Optional[EnginePin]) -> bool:
    """Automatic repair refreshes only stale or broken installs owned by this checkout."""
    return pin is not None and pin.is_editor_owned and pin.state in (PIN_STALE, PIN_INVALID)


def stale_editor_installs(tooling_root: Path, *, target: str, flavor: str, revision: str,
                          abi_key: Optional[dict] = None) -> list[Path]:
    """List installs below out/install for target/flavor built from another revision.

    ``abi_key`` (from the current build's manifest) excludes installs of another
    ABI variant; reinstalling a different variant over them would mix identities.
    """
    root = editor_install_root(tooling_root)
    if not root.is_dir() or not is_known_revision(revision):
        return []
    stale = []
    for candidate in sorted(path for path in root.iterdir() if path.is_dir()):
        try:
            identity = load_prefix_manifest(candidate)
        except ToolingError:
            continue
        if identity.target_triple != target or identity.flavor != flavor:
            continue
        if abi_key is not None and identity.abi_key() != abi_key:
            continue
        if not are_revisions_equal(identity.source_revision, revision):
            stale.append(candidate)
    return stale
