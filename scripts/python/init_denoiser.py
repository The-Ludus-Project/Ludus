"""Default-on OIDN denoiser setup: a pinned, verified release under out/denoiser.

Init downloads Intel Open Image Denoise from the pin in
config/tool_versions.json, verifies its SHA-256 and unpacks it to
``out/denoiser/oidn``. Renderer's CMake looks there, so a configure never
downloads anything. ``--no-denoiser`` skips the download and builds without it.
"""
from __future__ import annotations

import json
import os
import platform
import shutil
import tarfile
import urllib.request
from pathlib import Path, PurePosixPath


INSTALL_DIRECTORY = Path("out/denoiser/oidn")
MARKER_NAME = ".ludus-oidn.json"
# The runtime the SDK and packaged games redistribute, and the notices that
# travel with it. Device modules are loaded by the core library at run time.
RUNTIME_LIBRARIES = ("libOpenImageDenoise.2.dylib", "libOpenImageDenoise_core.2.5.1.dylib",
                     "libOpenImageDenoise_device_metal.2.5.1.dylib", "libOpenImageDenoise_device_cpu.2.5.1.dylib",
                     "libtbb.12.dylib")
LICENSE_FILES = ("LICENSE.txt", "third-party-programs.txt", "third-party-programs-oneTBB.txt")
UNSUPPORTED_NOTE = ("The OIDN denoiser is available only on macOS with Apple silicon (Metal); "
                    "this setup builds without it and render graphs preview Denoise nodes unfiltered.")


def host_key(system: str | None = None, machine: str | None = None) -> str | None:
    """The pinned archive key for this host, or None when no supported archive exists."""
    system = system or platform.system()
    machine = (machine or platform.machine()).lower()
    if system == "Darwin" and machine in ("arm64", "aarch64"):
        return "darwin_arm64"
    return None


def preset_supports(preset: str, *, system: str | None = None, machine: str | None = None) -> bool:
    """Whether ``preset`` can build with the denoiser on this host (native Metal on Apple silicon)."""
    return host_key(system, machine) is not None and preset.startswith("macos-clang-")


def validate_denoiser_options(args, engine) -> None:
    """Reject an explicit ``--with-denoiser`` the selected preset cannot honor."""
    if getattr(args, "with_denoiser", False) and not preset_supports(args.preset):
        raise engine.EngineError("--with-denoiser requires a macOS Apple silicon native preset because the OIDN "
                                 "denoiser uses Metal; rerun ./init.sh without --with-denoiser.")


def pin(versions: dict) -> dict:
    """The OIDN pin from config/tool_versions.json."""
    return versions["denoiser"]["oidn"]


def install_prefix(root: Path) -> Path:
    return root / INSTALL_DIRECTORY


def is_installed(root: Path, oidn: dict, key: str) -> bool:
    """Whether ``out/denoiser/oidn`` holds the verified pinned archive's contents."""
    prefix = install_prefix(root)
    try:
        marker = json.loads((prefix / MARKER_NAME).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return False
    return (marker == {"version": oidn["version"], "sha256": oidn["archives"][key]["sha256"]}
            and all((prefix / "lib" / name).is_file() for name in RUNTIME_LIBRARIES)
            and all((prefix / "doc" / name).is_file() for name in LICENSE_FILES))


def check_members(archive: tarfile.TarFile) -> str:
    """Return the archive's single top-level directory after rejecting unsafe members.

    Regular files and directories must stay below that directory. The release's
    library version links are allowed only as links to a sibling file name.
    """
    roots = set()
    for member in archive.getmembers():
        path = PurePosixPath(member.name)
        if path.is_absolute() or ".." in path.parts or not path.parts:
            raise ValueError(f"unsafe archive member {member.name!r}")
        roots.add(path.parts[0])
        if member.issym():
            target = PurePosixPath(member.linkname)
            if len(target.parts) != 1 or target.name in ("", ".", ".."):
                raise ValueError(f"archive link {member.name!r} leaves its directory")
        elif not (member.isfile() or member.isdir()):
            raise ValueError(f"unsupported archive member type {member.name!r}")
    if len(roots) != 1:
        raise ValueError("expected one top-level directory in the OIDN archive")
    return roots.pop()


def download(url: str, destination: Path) -> None:
    temporary = destination.with_suffix(".download")
    with urllib.request.urlopen(url) as response, temporary.open("wb") as file:  # noqa: S310 (pinned https URL)
        shutil.copyfileobj(response, file)
    temporary.replace(destination)


def install_denoiser(root: Path, versions: dict, engine) -> Path:
    """Install the pinned OIDN release for this host and return its prefix.

    An existing verified installation is kept. The archive is downloaded only
    when missing, checked against the pinned SHA-256 before extraction, and
    removed after a successful install.
    """
    import init_progress
    key = host_key()
    if key is None:
        raise engine.EngineError(UNSUPPORTED_NOTE)
    oidn = pin(versions)
    archive_pin = oidn["archives"][key]
    prefix = install_prefix(root)
    if is_installed(root, oidn, key):
        init_progress.up_to_date(f"OIDN {oidn['version']} is installed in {INSTALL_DIRECTORY}")
        return prefix
    work = prefix.parent
    work.mkdir(parents=True, exist_ok=True)
    archive = work / PurePosixPath(archive_pin["url"]).name
    if not archive.is_file() or engine.file_sha256(archive) != archive_pin["sha256"]:
        print(f"Network access: downloading OIDN {oidn['version']} ({oidn['license']}) from {archive_pin['url']}")
        try:
            download(archive_pin["url"], archive)
        except OSError as error:
            raise engine.EngineError(f"OIDN download failed: {error}. Check the network and rerun ./init.sh, "
                                     "or rerun it with --no-denoiser to build without the denoiser.") from error
    actual = engine.file_sha256(archive)
    if actual != archive_pin["sha256"]:
        archive.unlink()
        raise engine.EngineError(f"OIDN archive integrity failure: expected SHA-256 {archive_pin['sha256']}, "
                                 f"got {actual}. The download was removed; rerun ./init.sh to retry.")
    staging = work / f".staging-{os.getpid()}"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir()
    try:
        with tarfile.open(archive) as contents:
            try:
                top = check_members(contents)
            except ValueError as error:
                raise engine.EngineError(f"OIDN archive rejected: {error}") from error
            if hasattr(tarfile, "data_filter"):
                contents.extractall(staging, filter="data")
            else:
                contents.extractall(staging)  # Members were checked above (Python 3.10 has no filters).
        retired = work / f".retired-{os.getpid()}"
        if prefix.exists():
            prefix.replace(retired)
        (staging / top).replace(prefix)
        shutil.rmtree(retired, ignore_errors=True)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    (prefix / MARKER_NAME).write_text(json.dumps({"version": oidn["version"], "sha256": archive_pin["sha256"]},
                                                 sort_keys=True) + "\n", encoding="utf-8")
    if not is_installed(root, oidn, key):
        raise engine.EngineError(f"The OIDN {oidn['version']} archive lacks its expected runtime libraries or notices")
    archive.unlink()
    print(f"Installed OIDN {oidn['version']} in {prefix}")
    return prefix


def setup_denoiser(args, root: Path, versions: dict, engine) -> None:
    """The ``denoiser`` step: install OIDN when selected, or say why the build goes without it."""
    if getattr(args, "with_denoiser", False):
        import init_progress
        init_progress.begin("denoiser")
        install_denoiser(root, versions, engine)
    elif not preset_supports(args.preset):
        print(UNSUPPORTED_NOTE)
    else:
        print("Building without the OIDN denoiser (--no-denoiser); render graphs preview Denoise nodes unfiltered.")
