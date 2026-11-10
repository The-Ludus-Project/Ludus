"""Portable Windows Release distributions from explicitly installed payloads.

Does not download, sign, install prerequisites, run applications or publish.
The input spec is a release engineer's declaration, not qualification evidence.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import asdict, fields as dataclass_fields
from pathlib import Path

from .identity import SdkIdentity
from .package_verify import MAX_ENTRIES, MAX_EXPANDED, file_digest
from .release import _publish_directory, _version
from .release_model import canonical, fail, fields, payload_path, read_json, string
from .windows_binary import inspect_image, inspect_pdb

POLICY = "windows-x64-portable-release-1"
# An explicit OS closure, never the producer's PATH or System32 inventory.
SYSTEM_DLLS = frozenset((
    "advapi32.dll", "avrt.dll", "bcrypt.dll", "bcryptprimitives.dll", "cfgmgr32.dll",
    "combase.dll", "comctl32.dll", "comdlg32.dll", "crypt32.dll", "d3d11.dll",
    "d3d12.dll", "d3dcompiler_47.dll", "dbghelp.dll", "dcomp.dll", "dnsapi.dll",
    "dwmapi.dll", "dwrite.dll", "dxgi.dll", "dxguid.dll", "gdi32.dll", "imm32.dll",
    "iphlpapi.dll", "kernel32.dll", "mpr.dll", "msimg32.dll", "msvcrt.dll", "ncrypt.dll",
    "netapi32.dll", "normaliz.dll", "ntdll.dll", "ole32.dll", "oleaut32.dll",
    "opengl32.dll", "powrprof.dll", "propsys.dll", "psapi.dll", "rpcrt4.dll",
    "secur32.dll", "setupapi.dll", "shell32.dll", "shlwapi.dll", "ucrtbase.dll",
    "user32.dll", "userenv.dll", "usp10.dll", "uxtheme.dll", "version.dll",
    "winhttp.dll", "wininet.dll", "winmm.dll", "winspool.drv", "wintrust.dll",
    "wldap32.dll", "ws2_32.dll", "wtsapi32.dll",
))
CRT_DLLS = frozenset(("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
                      "msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll",
                      "msvcp140_codecvt_ids.dll", "concrt140.dll"))
RESERVED_NAMES = frozenset(("CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$",
                            *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10)),
                            *(f"COM{i}" for i in "¹²³"), *(f"LPT{i}" for i in "¹²³")))
MAX_BUNDLE_PATH = 200  # Leave room for a short extraction root under legacy MAX_PATH.


def check_windows_path(value: str) -> str:
    payload_path(value)
    if len(value.encode("utf-16-le")) // 2 > MAX_BUNDLE_PATH:
        fail(f"Windows path exceeds {MAX_BUNDLE_PATH} characters; shorten it: {value}", "InvalidPackage")
    for part in value.split("/"):
        if (part.endswith((".", " ")) or any(char in part for char in '<>"|?*')
                or part.split(".")[0].rstrip(" ").upper() in RESERVED_NAMES):
            fail(f"Windows cannot extract {value!r}; rename the entry", "InvalidPackage")
    return value


def inventory_tree(root: Path) -> list[dict]:
    """Inventory regular files, rejecting links, reparse points and collisions."""
    seen = set()
    records = []
    total = 0
    if (root.is_symlink() or not root.is_dir()
            or getattr(root.lstat(), "st_file_attributes", 0) & 0x400):
        fail("payload must be a regular directory", "InvalidPackage")
    for directory, directories, files in os.walk(root, followlinks=False):
        for name in sorted(directories + files):
            path = Path(directory) / name
            relative = check_windows_path(path.relative_to(root).as_posix())
            status = path.lstat()
            if (not (stat.S_ISREG(status.st_mode) or stat.S_ISDIR(status.st_mode))
                    or getattr(status, "st_file_attributes", 0) & 0x400):
                fail(f"linked/special entry {relative}; install regular files", "InvalidPackage")
            if relative.casefold() in seen:
                fail(f"case-colliding entry {relative}; rename it", "InvalidPackage")
            seen.add(relative.casefold())
            if len(seen) > MAX_ENTRIES:
                fail("too many distribution entries", "InvalidPackage")
            if stat.S_ISDIR(status.st_mode):
                continue
            total += status.st_size
            if total > MAX_EXPANDED:
                fail("distribution exceeds expanded-byte budget", "InvalidPackage")
            records.append({"path": relative, "size": status.st_size, "sha256": file_digest(path)})
    return sorted(records, key=lambda record: record["path"])


def _require_file(root: Path, relative: str) -> Path:
    path = root / check_windows_path(relative)
    if not path.is_file() or path.is_symlink() or path.stat().st_size == 0:
        fail(f"missing/empty {relative}; install it before packaging", "InvalidPackage")
    return path


def _verify_signature(path: Path, signtool: str | None) -> None:
    if sys.platform != "win32" or not signtool:
        fail("public bundles require Windows and --signtool to verify Authenticode", "MissingTools")
    # Thanks to Microsoft, "SignTool", verify /pa /all:
    # https://learn.microsoft.com/windows/win32/seccrypto/signtool
    try:
        result = subprocess.run([signtool, "verify", "/pa", "/all", str(path)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                check=False, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as exc:
        fail(f"cannot verify signature for {path.name}: {exc}", "InvalidPackage")
    if result.returncode != 0:
        fail(f"invalid Authenticode signature for {path.name}; sign the final image and retry", "InvalidPackage")


def _validate_payload(root: Path, spec: dict, *, signtool=None) -> dict:
    records = inventory_tree(root)
    paths = {record["path"] for record in records}
    _require_file(root, "NOTICE.txt")
    entry = spec.get("entryPoint")
    if spec["kind"] != "sdk":
        if not entry or Path(entry).suffix.lower() != ".exe":
            fail("game/editor bundles require an .exe entryPoint", "InvalidPackage")
        _require_file(root, entry)
    if any(Path(path).suffix.lower() in (".pdb", ".obj", ".ilk", ".exp") for path in paths):
        fail("payload contains intermediate/debug files; keep PDBs in the symbol input", "InvalidPackage")
    if spec["kind"] != "sdk" and any(Path(path).suffix.lower() in (".h", ".hpp", ".lib") for path in paths):
        fail("player/editor payload contains SDK authoring files; install a runtime component", "InvalidPackage")
    _require_file(root, spec["license"])
    for relative in spec["requiredFiles"]:
        _require_file(root, relative)
    owned = set(spec["symbols"])
    dependencies = spec["dependencies"]
    if not isinstance(dependencies, list) or len(dependencies) > MAX_ENTRIES:
        fail("dependencies must be a bounded list", "InvalidPackage")
    covered = set()
    for dependency in dependencies:
        fields(dependency, {"name", "version", "license", "files"}, set(), "dependency")
        string(dependency["name"], "dependency name")
        string(dependency["version"], "dependency version")
        _require_file(root, dependency["license"])
        if not isinstance(dependency["files"], list) or not dependency["files"]:
            fail("dependency files must be a nonempty list", "InvalidPackage")
        for path in dependency["files"]:
            _require_file(root, path)
            if path in covered or path in owned:
                fail("dependency image ownership overlaps; declare one owner", "InvalidPackage")
            covered.add(path)
    images = {}
    for relative in paths:
        path = root / relative
        with path.open("rb") as handle:
            is_image = handle.read(2) == b"MZ"
        if is_image or path.suffix.lower() in (".exe", ".dll", ".pyd"):
            images[relative] = inspect_image(path)
    if entry and (entry not in owned or images.get(entry, {}).get("isDll", True)):
        fail("entryPoint requires matching symbols for an executable image", "InvalidPackage")
    if not owned <= images.keys():
        fail("symbol map names a missing/non-PE payload image", "InvalidPackage")
    external = set()
    executable_dirs = {str(Path(path).parent) for path in images if not images[path]["isDll"]}
    available = {}
    for path in images:
        available.setdefault(str(Path(path).parent), set()).add(Path(path).name.casefold())
    for relative, image in sorted(images.items()):
        if relative not in owned | covered:
            fail(f"{relative} has no symbol/license owner; add symbols or a dependency record", "InvalidPackage")
        if relative in owned and image["pdb"] is None:
            fail(f"{relative} lacks RSDS identity; link with /DEBUG:FULL", "InvalidPackage")
        for name in image["imports"]:
            if ("/" in name or "\\" in name or ":" in name or not name
                    or name.endswith(("140d.dll", "ucrtbased.dll"))):
                fail(f"invalid/debug runtime import {name!r}; rebuild Release", "InvalidPackage")
            if name in SYSTEM_DLLS or re.fullmatch(r"(?:api|ext)-ms-win-[a-z0-9-]+\.dll", name):
                external.add(name)
                continue
            if name in CRT_DLLS:
                external.add(name)
                continue
            # No PATH lookup: permit the image's directory or an executable's
            # directory (plugins), conservatively requiring every possible host.
            local = name in available.get(str(Path(relative).parent), set())
            if not local and not (executable_dirs and all(name in available.get(item, set()) for item in executable_dirs)):
                fail(f"unresolved {name!r} imported by {relative}; install it beside its host", "InvalidPackage")
        if spec["channel"] == "public":
            _verify_signature(root / relative, signtool)
    if spec["kind"] == "editor":
        import configparser
        _require_file(root, (Path(entry).parent / "platforms/qwindows.dll").as_posix())
        configuration = _require_file(root, (Path(entry).parent / "qt.conf").as_posix())
        parser = configparser.ConfigParser(interpolation=None)
        try:
            parser.read_string(configuration.read_text(encoding="utf-8"))
        except (configparser.Error, UnicodeError):
            fail("invalid qt.conf; deploy a relative [Paths] configuration", "InvalidPackage")
        if (not parser.has_section("Paths") or parser.get("Paths", "Prefix", fallback="") != "."
                or parser.get("Paths", "Plugins", fallback="") != "."
                or any(value != "." and (":" in value or "\\" in value or value.startswith("/")
                       or ".." in value.split("/")) for _, value in parser.items("Paths"))):
            fail("qt.conf must use Prefix=. and Plugins=. with package-relative paths", "InvalidPackage")
    if "redistributable" in spec or any(name in CRT_DLLS for name in external):
        redist = spec.get("redistributable")
        fields(redist, {"sha256", "version", "license"}, {"path"}, "redistributable")
        if not isinstance(redist["sha256"], str) or re.fullmatch(r"[a-f0-9]{64}", redist["sha256"]) is None:
            fail("declare the reviewed VC Redistributable SHA-256", "InvalidPackage")
        string(redist["version"], "VC Redistributable version")
        _require_file(root, redist["license"])
    return {"images": images, "systemLibraries": sorted(external), "files": records}


def _validate_sdk(sdk: SdkIdentity) -> None:
    policy = sdk.assert_policy if isinstance(sdk.assert_policy, dict) else sdk.assert_policy.to_json()
    if (sdk.target_triple != "x86_64-pc-windows-msvc" or sdk.flavor != "Release"
            or sdk.compiler_id != "MSVC" or sdk.cxx_standard != "C++23" or sdk.cxx_runtime_abi != "msvc-stl"
            or not isinstance(sdk.compiler_version, str) or not sdk.compiler_version.startswith("19.44.")
            or not isinstance(sdk.sdk_variant, str)
            or re.fullmatch(r"assert-v[0-9]+-Release-(?:Release|MinSizeRel)-dialogs-0-asan-0-ubsan-0-tsan-0", sdk.sdk_variant) is None
            or policy.get("enable_asserts") is not False or policy.get("break_on_check") is not False
            or policy.get("dialogs_available") is not False):
        fail("use a Windows x64 MSVC C++23 Release SDK without sanitizers", "SdkIncompatible")


def create_distribution(payload: Path, destination: Path, spec_path: Path, *, sdk_manifest: Path,
                        docs: Path, signtool: str | None = None) -> Path:
    """Publish a versioned portable bundle only after checking a clean extraction."""
    spec = read_json(spec_path)
    fields(spec, {"schemaVersion", "kind", "version", "revision", "channel", "symbols", "dependencies", "license", "requiredFiles"},
           {"entryPoint", "redistributable"}, "Windows distribution spec")
    if (spec["schemaVersion"] != 1 or type(spec["schemaVersion"]) is not int
            or spec["kind"] not in ("sdk", "game", "editor") or spec["channel"] not in ("internal", "public")
            or not isinstance(spec["revision"], str) or not re.fullmatch(r"[a-f0-9]{40}", spec["revision"])
            or not isinstance(spec["symbols"], dict)
            or len(spec["symbols"]) > MAX_ENTRIES
            or any(not isinstance(value, str) for value in spec["symbols"].values())
            or not isinstance(spec["requiredFiles"], list)
            or any(not isinstance(path, str) for path in spec["requiredFiles"])):
        fail("invalid Windows release spec; select kind/channel and a full candidate revision", "InvalidPackage")
    _version(spec["version"])
    if destination.resolve().is_relative_to(payload.resolve()):
        fail("distribution output must be outside the installed payload", "InvalidPackage")
    sdk = SdkIdentity.from_json(read_json(sdk_manifest))
    _validate_sdk(sdk)
    if destination.exists() or destination.is_symlink():
        fail("distribution destination exists; choose a new version directory", "DestinationExists")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".windows-release-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        completed = staging / "completed"
        completed.mkdir()
        root = staging / "bundle"
        root.mkdir()
        # Inventory first, so copytree cannot follow an input symlink/junction.
        original = inventory_tree(payload)
        shutil.copytree(payload, root / "payload")
        if inventory_tree(root / "payload") != original:
            fail("payload changed while staging; rebuild and retry", "Conflict")
        checked = _validate_payload(root / "payload", spec, signtool=signtool)
        symbols = []
        for image, source in sorted(spec["symbols"].items()):
            identity = inspect_pdb(Path(source))
            if identity != checked["images"][image]["pdb"]:
                fail(f"PDB does not match {image}; retain the PDB from the same link", "InvalidPackage")
            relative = check_windows_path("symbols/" + image + ".pdb")
            output = root / relative
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, output)
            if inspect_pdb(output) != identity:
                fail("PDB changed during staging", "Conflict")
            symbols.append({"image": image, "pdb": relative, **identity})
        if "redistributable" in spec or any(name in CRT_DLLS for name in checked["systemLibraries"]):
            redist = spec.get("redistributable")
            fields(redist, {"path", "sha256", "version", "license"}, set(), "redistributable")
            source = Path(redist["path"])
            if file_digest(source) != redist["sha256"]:
                fail("VC Redistributable digest differs; supply the reviewed installer", "InvalidPackage")
            string(redist["version"], "VC Redistributable version")
            _require_file(root / "payload", redist["license"])
            target = root / "prerequisites/VC_redist.x64.exe"
            target.parent.mkdir()
            shutil.copyfile(source, target)
            if file_digest(target) != redist["sha256"]:
                fail("VC Redistributable changed while staging", "Conflict")
            if spec["channel"] == "public":
                _verify_signature(target, signtool)
        # package-docs remains the sole producer of the checked offline archive.
        # It contains both canonical Markdown and the HTML site's source ledger.
        with zipfile.ZipFile(docs) as reader:
            required = {"docs/README.md", "site/index.html", "site/documentation-sources.json"}
            if not required <= set(reader.namelist()):
                fail("offline docs are incomplete; run scripts/package-docs after wiki checks", "InvalidPackage")
        shutil.copyfile(docs, root / "ludus-docs-offline.zip")
        (root / "README.txt").write_text(
            "Ludus Windows portable Release\n\n"
            "Extract into a new, short directory (for example C:\\Ludus\\version).\n"
            "If prerequisites/VC_redist.x64.exe is present, run the reviewed Microsoft\n"
            "installer interactively before launch; a reboot may be required.\n"
            "Launch the declared entryPoint under payload, or consume the SDK there.\n"
            "Keep projects, saves and settings outside this version directory.\n"
            "Upgrade: extract the new version beside the old one, close the old app,\n"
            "verify the new version, then select its SDK/app explicitly. No authored\n"
            "data is migrated or deleted. Roll back by selecting the old directory.\n"
            "Uninstall: close processes and delete only this version directory.\n"
            "Do not remove the shared Microsoft runtime or user projects/saves.\n"
            "Missing DLL: reinstall this complete bundle and its prerequisite.\n"
            "Launch failure: retain distribution.json, Windows event/crash details\n"
            "and matching symbols for a report. Read docs offline from the docs ZIP.\n"
            "Internal bundles have no Authenticode acceptance. Static validation\n"
            "does not establish D7 hardware or clean-machine qualification.\n", encoding="utf-8")
        manifest = {"schemaVersion": 1, "policy": POLICY, "kind": spec["kind"], "version": spec["version"],
                    "revision": spec["revision"], "channel": spec["channel"], "entryPoint": spec.get("entryPoint"),
                    "license": spec["license"], "requiredFiles": spec["requiredFiles"],
                    "sdk": asdict(sdk), "dependencies": spec["dependencies"], "symbols": symbols,
                    "systemLibraries": checked["systemLibraries"], "images": checked["images"],
                    "qualification": "pending-clean-machine-and-D7-acceptance", "files": inventory_tree(root)}
        if "redistributable" in spec:
            manifest["redistributable"] = {key: spec["redistributable"][key] for key in ("sha256", "version", "license")}
        if spec["kind"] == "sdk":
            installed = read_json(_require_file(root / "payload", "share/Ludus/LudusSdkManifest.json"))
            if asdict(SdkIdentity.from_json(installed)) != asdict(sdk):
                fail("installed SDK identity differs from selected Release SDK", "SdkIncompatible")
            if spec["revision"] != sdk.source_revision:
                fail("SDK candidate revision differs from its installed identity", "SdkIncompatible")
        (root / "distribution.json").write_bytes(canonical(manifest))
        archive = completed / "windows.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as writer:
            for record in inventory_tree(root):
                info = zipfile.ZipInfo(record["path"], (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with (root / record["path"]).open("rb") as source, writer.open(info, "w", force_zip64=True) as target:
                    shutil.copyfileobj(source, target)
        verify_distribution(archive, signtool=signtool)
        (completed / "SHA256SUMS").write_text(file_digest(archive) + "  windows.zip\n", encoding="ascii")
        _publish_directory(completed, destination)
    return destination


def verify_distribution(archive: Path, *, signtool=None) -> dict:
    """Verify bytes, loader closure and matching symbols after private extraction."""
    from .package_limits import MAX_CENTRAL_DIRECTORY_BYTES
    with archive.open("rb") as handle:
        end = zipfile._EndRecData(handle)
    if (end is None or end[zipfile._ECD_ENTRIES_TOTAL] > MAX_ENTRIES
            or end[zipfile._ECD_SIZE] > MAX_CENTRAL_DIRECTORY_BYTES):
        fail("distribution ZIP metadata exceeds limits", "InvalidPackage")
    with tempfile.TemporaryDirectory(prefix="ludus-windows-verify-") as temporary:
        root = Path(temporary)
        with zipfile.ZipFile(archive) as reader:
            seen = set()
            total = 0
            parents = {}
            for item in reader.infolist():
                name = check_windows_path(item.filename)
                mode = item.external_attr >> 16
                if (name.casefold() in seen or stat.S_IFMT(mode) != stat.S_IFREG
                        or item.flag_bits & 1 or item.compress_type != zipfile.ZIP_DEFLATED):
                    fail("duplicate or unsupported ZIP entry", "InvalidPackage")
                seen.add(name.casefold())
                for parent in Path(name).parents:
                    if parent == Path("."):
                        continue
                    spelling = parent.as_posix()
                    if parents.setdefault(spelling.casefold(), spelling) != spelling:
                        fail("case-colliding ZIP parents", "InvalidPackage")
                total += item.file_size
                if total > MAX_EXPANDED:
                    fail("ZIP exceeds expanded-byte budget", "InvalidPackage")
            if seen & parents.keys():
                fail("ZIP file collides with directory", "InvalidPackage")
            for item in reader.infolist():
                output = root / item.filename
                output.parent.mkdir(parents=True, exist_ok=True)
                with reader.open(item) as source, output.open("xb") as target:
                    shutil.copyfileobj(source, target)
        from .release_model import json_bytes
        from .package_verify import MAX_MANIFEST
        with (root / "distribution.json").open("rb") as handle:
            manifest = json_bytes(handle.read(MAX_MANIFEST + 1), "distribution.json", maximum=MAX_MANIFEST)
        if (manifest.get("policy") != POLICY or type(manifest.get("schemaVersion")) is not int
                or manifest["schemaVersion"] != 1 or manifest.get("kind") not in ("sdk", "game", "editor")
                or manifest.get("channel") not in ("internal", "public")):
            fail("unsupported Windows distribution policy", "InvalidPackage")
        fields(manifest, {"schemaVersion", "policy", "kind", "version", "revision", "channel", "entryPoint",
                          "license", "requiredFiles", "sdk", "dependencies", "symbols", "systemLibraries",
                          "images", "qualification", "files"}, {"redistributable"}, "distribution manifest")
        _version(manifest["version"])
        if (not isinstance(manifest["revision"], str) or re.fullmatch(r"[a-f0-9]{40}", manifest["revision"]) is None
                or not isinstance(manifest["symbols"], list) or not isinstance(manifest["requiredFiles"], list)):
            fail("invalid distribution provenance/symbol inventory", "InvalidPackage")
        fields(manifest["sdk"], {field.name for field in dataclass_fields(SdkIdentity)}, set(), "SDK identity")
        _validate_sdk(SdkIdentity(**manifest["sdk"]))
        for item in manifest["symbols"]:
            fields(item, {"image", "pdb", "guid", "age"}, set(), "symbol record")
            check_windows_path(item["image"])
            check_windows_path(item["pdb"])
        actual = [record for record in inventory_tree(root) if record["path"] != "distribution.json"]
        if actual != manifest.get("files"):
            fail("distribution inventory differs; reacquire the original bundle", "InvalidPackage")
        spec = {**manifest, "symbols": {item["image"]: item["pdb"] for item in manifest["symbols"]}}
        if len(spec["symbols"]) != len(manifest["symbols"]):
            fail("duplicate symbol owner", "InvalidPackage")
        checked = _validate_payload(root / "payload", spec, signtool=signtool)
        if checked["images"] != manifest["images"] or checked["systemLibraries"] != manifest["systemLibraries"]:
            fail("distribution loader metadata differs", "InvalidPackage")
        for item in manifest["symbols"]:
            identity = inspect_pdb(_require_file(root, item["pdb"]))
            if identity != manifest["images"][item["image"]]["pdb"] or identity != {"guid": item["guid"], "age": item["age"]}:
                fail("distribution symbols do not match image", "InvalidPackage")
        if "redistributable" in manifest or any(name in CRT_DLLS for name in checked["systemLibraries"]):
            if file_digest(_require_file(root, "prerequisites/VC_redist.x64.exe")) != manifest["redistributable"]["sha256"]:
                fail("distribution VC Redistributable digest differs", "InvalidPackage")
        if manifest["kind"] == "sdk":
            installed = SdkIdentity.from_json(read_json(_require_file(root / "payload", "share/Ludus/LudusSdkManifest.json")))
            if asdict(installed) != manifest["sdk"] or installed.source_revision != manifest["revision"]:
                fail("distribution SDK identity disagrees with installed SDK", "SdkIncompatible")
        with zipfile.ZipFile(_require_file(root, "ludus-docs-offline.zip")) as reader:
            if not {"docs/README.md", "site/index.html", "site/documentation-sources.json"} <= set(reader.namelist()):
                fail("distribution offline docs are incomplete", "InvalidPackage")
        if manifest["channel"] == "public" and (root / "prerequisites/VC_redist.x64.exe").exists():
            _verify_signature(root / "prerequisites/VC_redist.x64.exe", signtool)
        return manifest


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="operation", required=True)
    create = subparsers.add_parser("create")
    for name in ("payload", "output", "spec", "sdk-manifest", "docs"):
        create.add_argument("--" + name, required=True, type=Path)
    create.add_argument("--signtool")
    verify = subparsers.add_parser("verify")
    verify.add_argument("archive", type=Path)
    verify.add_argument("--signtool")
    args = parser.parse_args(argv)
    from .errors import ToolingError
    try:
        if args.operation == "create":
            print(create_distribution(args.payload, args.output, args.spec, sdk_manifest=args.sdk_manifest,
                                      docs=args.docs, signtool=args.signtool))
        else:
            manifest = verify_distribution(args.archive, signtool=args.signtool)
            print(f"Verified {manifest['kind']} {manifest['version']}: {file_digest(args.archive)}")
    except (ToolingError, OSError, ValueError, zipfile.BadZipFile) as exc:
        print(f"Windows packaging failed: {exc}", file=sys.stderr)
        return 1
    return 0
