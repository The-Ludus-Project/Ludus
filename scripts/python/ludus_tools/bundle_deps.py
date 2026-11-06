"""Bundle the redistributable dependency closure into an SDK prefix (P02).

A relocatable SDK must link from a fresh extraction with no producer Conan cache
on the search path. The engine's exported ``LudusConfig.cmake`` therefore calls
``find_dependency(volk/freetype/harfbuzz)`` against a **package-local** search
root, ``<prefix>/lib/cmake/Ludus/dependencies``. This module populates that root
at install time by copying, out of the Conan install:

* the generator-produced CMake package files (``*-config.cmake``,
  ``*Targets*.cmake``, ``*-data.cmake``, ``cmakedeps_macros.cmake`` …) that make
  ``find_package(<dep>)`` work — these live in the Conan *generators output
  folder*, NOT inside the package directories; and
* each dependency's package payload (``lib/`` static libs, ``include/`` headers,
  any ``lib/cmake`` build modules), copied under ``dependencies/packages/<dep>``.

Every absolute producer path (the Conan cache package roots and the generators
folder) is then rewritten to a path relative to the bundled location so the
bundle relocates. The rewrite is the error-prone part and is unit-tested; the
copy orchestration runs during ``install-sdk`` on a machine with Conan.

Nothing here depends on Qt or the engine; it uses only the standard library.
"""

from __future__ import annotations

import re
import shutil
from pathlib import Path
from typing import Iterable

from .errors import ARCHIVE_INVALID, ToolingError

# The dependencies whose closure must travel with the SDK (see conanfile.py and
# the manifest inventory). volk is a public CONFIG dependency; freetype/harfbuzz
# are PRIVATE deps of Ludus::Text that remain in the final static link line.
# volk's CMake config transitively finds VulkanHeaders, so its generator config
# must be bundled too even though it is header-only.
BUNDLED_DEPENDENCIES = ("volk", "freetype", "harfbuzz")

# Package names whose GENERATOR cmake files are part of the find_package closure
# of the bundled dependencies (bundled deps + their transitive CONFIG deps).
# Matched case-insensitively against the generator filename prefix.
_GENERATOR_PACKAGE_NAMES = ("volk", "vulkanheaders", "freetype", "harfbuzz")

# Shared CMakeDeps helper file that the per-package configs include.
_SHARED_GENERATOR_FILES = ("cmakedeps_macros.cmake",)

# Generator files that must NOT be bundled: build-only toolchain/runtime files
# and test-only dependencies. They embed producer paths and are irrelevant to a
# consumer linking the runtime SDK.
_EXCLUDED_GENERATOR_SUBSTRINGS = (
    "conan_toolchain",
    "conanbuild",
    "conanrun",
    "conandeps",
    "catch2",
)

# Bundled layout, relative to the dependency search root
# <prefix>/lib/cmake/Ludus/dependencies:
#   cmake/      <- the Conan generator CMake files (find_package entry points)
#   packages/<dep>/  <- each dependency's lib/include/cmake payload
_SELF_DIR = "${CMAKE_CURRENT_LIST_DIR}"


def path_spellings(path: Path | str) -> list[str]:
    """Return the spellings of ``path`` that generated CMake files may contain.

    CMakeDeps writes forward slashes on every host, while ``str(Path)`` uses the
    native separator. On Windows both spellings must be rewritten and audited.
    """
    native = str(path)
    # Spelled out rather than PurePath.as_posix(), which keeps backslashes on POSIX hosts.
    portable = native.replace("\\", "/")
    return list(dict.fromkeys(spelling for spelling in (native, portable) if spelling))


def _spelled_replacements(root: Path | str, replacement: str) -> list[tuple[str, str]]:
    return [(spelling, replacement) for spelling in path_spellings(root)]


def rewrite_producer_paths(text: str, replacements: list[tuple[str, str]]) -> str:
    """Replace each absolute producer root with its bundled replacement token.

    ``replacements`` is a list of (absolute_root, replacement) pairs. Longer
    roots are applied first so a nested root is not partially rewritten.
    """
    for root, replacement in sorted(replacements, key=lambda rp: len(rp[0]), reverse=True):
        if root:
            text = text.replace(root, replacement)
    return text


def find_producer_roots(text: str) -> list[str]:
    """Heuristically find absolute Conan-cache / build package roots in a config.

    Returns the distinct absolute directory prefixes that look like a Conan
    package root (``…/p/<folder>/p``) or a build root (``…/b/<folder>/p``). Used
    by the audit so a leaked path is reported precisely.
    """
    roots: set[str] = set()
    # An optional drive letter keeps Windows roots (C:/Users/.../p/<folder>/p) whole.
    for m in re.finditer(r"((?:[A-Za-z]:)?/[^\s\"';]*?/p/[0-9a-z]+/p)(?=/|\"|'|\s|$)", text):
        roots.add(m.group(1))
    for m in re.finditer(r"((?:[A-Za-z]:)?/[^\s\"';]*?/b/[0-9a-z]+/p)(?=/|\"|'|\s|$)", text):
        roots.add(m.group(1))
    return sorted(roots)


def _copy_tree(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True, symlinks=True)
    elif src.is_file():
        shutil.copy2(src, dst)


def relocate_apple_framework_links(text: str) -> str:
    """Replace absolute system-framework link entries with consumer SDK lookup.

    Only link-interface entries are rewritten. Include paths, arbitrary package
    paths and private frameworks retain their existing validation/audit behavior.
    """
    # Thanks to Kitware's cmake-generator-expressions(7), Link Features/FRAMEWORK:
    # a bare framework name uses the consuming Apple toolchain's search paths.
    # https://cmake.org/cmake/help/v3.29/manual/cmake-generator-expressions.7.html#link-features
    # Adapt the link interface rather than bundling Apple's SDK frameworks.
    def relocate(match):
        links = []
        for link in match.group(2).split(";"):
            framework = re.fullmatch(r"/[^\n;\"]*/System/Library/Frameworks/([A-Za-z0-9_]+)\.framework", link)
            links.append(f"$<LINK_LIBRARY:FRAMEWORK,{framework.group(1)}>" if framework else link)
        return match.group(1) + ";".join(links) + match.group(3)
    return re.sub(r'(INTERFACE_LINK_LIBRARIES\s+")([^"]*)(")', relocate, text)


def _rewrite_cmake_files_in(root: Path, replacements: list[tuple[str, str]]) -> int:
    rewritten = 0
    for cmake_file in list(root.rglob("*.cmake")) + list(root.rglob("*.cmake.py")):
        try:
            text = cmake_file.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        new = relocate_apple_framework_links(rewrite_producer_paths(text, replacements))
        if new != text:
            cmake_file.write_text(new, encoding="utf-8")
            rewritten += 1
    return rewritten


def conan_package_dirs(generators_dir: Path) -> dict[str, Path]:
    """Discover each dependency's Conan package root from CMakeDeps data files.

    CMakeDeps emits ``<pkg>-<config>-*-data.cmake`` files that set
    ``<pkg>_PACKAGE_FOLDER_<CONFIG> "<abs package root>"``. Reading them finds
    each dependency's lib/include/cmake files without hard-coding the Conan
    cache layout. Roots that no longer exist are skipped.
    """
    package_dirs: dict[str, Path] = {}
    generators_dir = Path(generators_dir)
    if not generators_dir.is_dir():
        return package_dirs
    pattern = re.compile(r'set\(([A-Za-z0-9_]+)_PACKAGE_FOLDER_[A-Z]+\s+"([^"]+)"')
    for data_file in sorted(generators_dir.rglob("*-data.cmake")):
        text = data_file.read_text(encoding="utf-8")
        for match in pattern.finditer(text):
            folder = Path(match.group(2))
            if folder.is_dir():
                package_dirs.setdefault(match.group(1).lower(), folder)
    return package_dirs


def bundle_from_conan(
    *,
    prefix: Path,
    generators_dir: Path,
    package_dirs: dict[str, Path],
    dependencies: Iterable[str] = BUNDLED_DEPENDENCIES,
) -> list[str]:
    """Bundle the Conan generator CMake files + dependency package payloads.

    ``generators_dir`` is the Conan ``--output-folder`` (e.g.
    ``out/conan/<preset>``) containing the ``*-config.cmake`` entry points.
    ``package_dirs`` maps a dependency name to its Conan package root. After
    copying, every absolute reference to the generators folder or a package root
    is rewritten to the bundled, ``CMAKE_CURRENT_LIST_DIR``-relative location so
    the SDK relocates. Returns the dependencies actually bundled.
    """
    dep_root = Path(prefix) / "lib" / "cmake" / "Ludus" / "dependencies"
    cmake_dst = dep_root / "cmake"
    packages_dst = dep_root / "packages"
    cmake_dst.mkdir(parents=True, exist_ok=True)
    packages_dst.mkdir(parents=True, exist_ok=True)

    # 1) Copy ONLY the generator CMake files that belong to the bundled
    #    dependencies' find_package closure (plus the shared macros file). Copying
    #    the whole folder would drag in build-only (conan_toolchain) and test-only
    #    (Catch2) files that embed producer paths and are not part of the runtime
    #    SDK's link closure.
    generators_dir = Path(generators_dir)
    if generators_dir.is_dir():
        for item in generators_dir.iterdir():
            if not (item.is_file() and item.suffix == ".cmake"):
                continue
            lower = item.name.lower()
            if item.name in _SHARED_GENERATOR_FILES:
                shutil.copy2(item, cmake_dst / item.name)
                continue
            if any(ex in lower for ex in _EXCLUDED_GENERATOR_SUBSTRINGS):
                continue
            # Keep a generator file only when its name starts with a package we
            # bundle (e.g. volk-config.cmake, volkTargets.cmake,
            # VulkanHeaders-release-...-data.cmake, Freetype*.cmake).
            if any(lower.startswith(name) for name in _GENERATOR_PACKAGE_NAMES):
                shutil.copy2(item, cmake_dst / item.name)

    # 2) Copy each dependency package payload, plus any transitive CONFIG-dep
    #    package whose generator config we kept (e.g. volk pulls VulkanHeaders).
    #    Every package root we copy must also be rewritten in the generator files
    #    so no producer path survives.
    payload_names = list(dict.fromkeys(list(dependencies) + list(package_dirs.keys())))
    bundled: list[str] = []
    requested = set(dependencies)
    package_replacements: list[tuple[str, str]] = []
    for name in payload_names:
        src = package_dirs.get(name)
        if src is None or not Path(src).exists():
            continue
        # Only bundle a package payload when it is a requested dependency OR a
        # transitive package whose generator config was kept above.
        if name not in requested and not any(
            n in name.lower() or name.lower() in n for n in _GENERATOR_PACKAGE_NAMES
        ):
            continue
        dest = packages_dst / name
        _copy_tree(Path(src), dest)
        if name in requested:
            bundled.append(name)
        # The generator cmake files sit in dependencies/cmake/, so the bundled
        # package is at ../packages/<name> relative to a generator cmake file.
        package_replacements.extend(_spelled_replacements(src, f"{_SELF_DIR}/../packages/{name}"))

    # 3) Rewrite producer paths in the copied generator CMake files: the
    #    generators folder itself and every package root -> bundled locations.
    generator_replacements = list(package_replacements)
    if generators_dir:
        generator_replacements.extend(_spelled_replacements(generators_dir, _SELF_DIR))
    _rewrite_cmake_files_in(cmake_dst, generator_replacements)

    # 4) Rewrite any absolute self-references inside the copied package trees
    #    (e.g. build modules that embed their own package root).
    for name in bundled:
        src = package_dirs[name]
        _rewrite_cmake_files_in(packages_dst / name, _spelled_replacements(src, _SELF_DIR))

    # 5) Final pass: a kept generator file can still reference an absolute Conan
    #    package root that step 2 did not copy (e.g. a transitive, header-only
    #    dependency like VulkanHeaders whose _PACKAGE_FOLDER_ was not discovered
    #    in package_dirs). For EACH residual root, bundle that package payload
    #    FROM THE ROOT ITSELF (the root is the package dir) into
    #    packages/<token>, then rewrite the reference to it — so the path both
    #    relocates AND points at the real payload. Only fail if the root does not
    #    exist on disk (then we cannot bundle it and must not ship a broken path).
    _bundle_and_rewrite_residual_roots(cmake_dst, packages_dst)

    return bundled


def _residual_payload_name(root: str) -> str:
    """Derive a stable payload directory name from a Conan package root.

    ``.../p/vulka2b5ed468eaac1/p`` -> ``vulka2b5ed468eaac1`` (the folder token
    before the trailing ``/p``), which is unique per package build.
    """
    parts = [p for p in root.split("/") if p]
    return parts[-2] if len(parts) >= 2 else parts[-1]


def _bundle_and_rewrite_residual_roots(cmake_dir: Path, packages_dst: Path) -> None:
    # First collect every residual root referenced by any kept generator file.
    residual: set[str] = set()
    for cmake_file in cmake_dir.rglob("*.cmake"):
        try:
            text = cmake_file.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        residual.update(find_producer_roots(text))

    # Bundle each residual root's payload (keyed by its unique folder token) and
    # build the rewrite map. A root that no longer exists on disk cannot be
    # bundled: fail rather than ship a broken relocatable path.
    replacements: list[tuple[str, str]] = []
    for root in sorted(residual):
        name = _residual_payload_name(root)
        dest = packages_dst / name
        if not dest.exists():
            if not Path(root).exists():
                raise ToolingError(
                    ARCHIVE_INVALID,
                    f"bundled dependency references a Conan root {root!r} that does not exist; "
                    "refusing to ship a broken relocatable path",
                )
            _copy_tree(Path(root), dest)
        replacements.append((root, f"{_SELF_DIR}/../packages/{name}"))

    if not replacements:
        return
    for cmake_file in cmake_dir.rglob("*.cmake"):
        try:
            text = cmake_file.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        new = relocate_apple_framework_links(rewrite_producer_paths(text, replacements))
        if new != text:
            cmake_file.write_text(new, encoding="utf-8")


def audit_no_producer_paths(prefix: Path, producer_roots: Iterable[str]) -> list[str]:
    """Return (file: leaked-path) strings for any producer path that survived in
    the bundled dependency CMake files. Empty means clean."""
    dep_root = Path(prefix) / "lib" / "cmake" / "Ludus" / "dependencies"
    leaks: list[str] = []
    roots = list(dict.fromkeys(spelling for r in producer_roots if r for spelling in path_spellings(r)))
    if not dep_root.is_dir():
        return leaks
    for cmake_file in dep_root.rglob("*.cmake"):
        try:
            text = cmake_file.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for root in roots:
            if root in text:
                leaks.append(f"{cmake_file}: {root}")
    return leaks
