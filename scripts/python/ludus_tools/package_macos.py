"""Bounded Mach-O inspection and local ad-hoc app signing; never run the game."""
from __future__ import annotations

import hashlib
import os
import plistlib
import struct
import sys
import subprocess
import tempfile
from pathlib import Path

from .package_limits import MAX_MACHO_LOAD_COMMAND_BYTES, MAX_NATIVE_INSPECTION_OUTPUT_BYTES
from .release_model import fail

# Thanks to Apple, XNU mach-o/loader.h (mach_header_64, segment_command_64,
# section_64 and load commands), and Dynamic Library Programming Topics,
# "Run-Path Dependent Libraries": these layouts and loader tokens inform this
# independent bounded reader. This policy requires each image's own relative
# search paths; it does not emulate dyld's inherited run-path stack.
# https://github.com/apple-oss-distributions/xnu/blob/main/EXTERNAL_HEADERS/mach-o/loader.h
# https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/DynamicLibraries/100-Articles/RunpathDependentLibraries.html
POLICY = "macos-release-adhoc-1"
CPUS = {"macos-arm64": 0x100000C, "macos-x64": 0x1000007}
SYSTEM_LIBRARIES = frozenset({"/usr/lib/libSystem.B.dylib", "/usr/lib/libc++.1.dylib",
                              "/usr/lib/libc++abi.dylib", "/usr/lib/libobjc.A.dylib",
                              # Platform's hosted Mach rendezvous reads the sender's audit token.
                              "/usr/lib/libbsm.0.dylib"})
FRAMEWORKS = frozenset({"Cocoa", "AppKit", "Foundation", "CoreFoundation", "CoreGraphics",
                       "CoreAudio", "AudioToolbox", "CoreServices", "QuartzCore", "Metal",
                       "IOKit", "Security", "CoreVideo", "CoreText", "AVFAudio",
                       # GraphicsRhi's Metal backend: MetalFX upscaling and hosted IOSurface images.
                       "MetalFX", "IOSurface",
                       # The SDK's OIDN denoiser runtime and its Metal/CPU device modules.
                       "Accelerate", "MetalPerformanceShaders", "MetalPerformanceShadersGraph"})
# Allowed frameworks newer than the oldest accepted image baseline. A required
# (non-weak) load needs an image baseline that includes the framework.
FRAMEWORK_MINIMUMS = {"MetalFX": 0x000d0000}  # macOS 13.0
MAGIC = b"\xcf\xfa\xed\xfe"


def _invalid(message):
    fail(message, "InvalidPackage")


def inspect(path: Path) -> dict:
    """Read thin little-endian 64-bit images with bounded command/section tables."""
    size = path.stat().st_size
    with path.open("rb") as handle:
        header = handle.read(32)
        if len(header) != 32 or header[:4] != MAGIC:
            _invalid("macOS payload requires a thin little-endian Mach-O64 image")
        _, cpu, subtype, kind, count, length, flags, _ = struct.unpack("<8I", header)
        if cpu not in CPUS.values() or kind not in (2, 6, 8) or not 1 <= count <= 4096 or length > MAX_MACHO_LOAD_COMMAND_BYTES or 32 + length > size:
            _invalid("unsupported or malformed Mach-O header")
        commands = handle.read(length)
        offset, sections, dependencies, rpaths, minimum, entry = 0, {}, [], [], None, None
        weak_dependencies = []
        code_ranges, file_ranges = [], []
        symbol_table = False
        for _ in range(count):
            if offset + 8 > length:
                _invalid("truncated Mach-O load command")
            command, stride = struct.unpack_from("<II", commands, offset)
            if stride < 8 or stride % 8 or offset + stride > length:
                _invalid("invalid Mach-O load command size")
            data = commands[offset:offset + stride]
            if command == 0x19:  # LC_SEGMENT_64
                if stride < 72:
                    _invalid("truncated Mach-O segment")
                number = struct.unpack_from("<I", data, 64)[0]
                if len(sections) + number > 4096 or 72 + number * 80 != stride:
                    _invalid("invalid Mach-O section table")
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<4Q", data, 24)
                if fileoff + filesize > size:
                    _invalid("Mach-O segment exceeds image")
                for index in range(number):
                    section = struct.unpack_from("<16s16sQQIIIIIIII", data, 72 + index * 80)
                    name, segment = section[0].rstrip(b"\0"), section[1].rstrip(b"\0")
                    if segment == b"__DWARF" or name.startswith(b"__debug"):
                        _invalid("macOS release contains debug sections")
                    if segment == b"__LINKEDIT":
                        continue  # Codesign and install-name edits change loader/signature metadata.
                    key = (segment, name)
                    if key in sections or section[2] < vmaddr or section[2] + section[3] > vmaddr + vmsize:
                        _invalid("invalid or duplicate Mach-O section")
                    digest = hashlib.sha256()
                    if section[8] & 0xff not in (1, 0xc, 0x12):  # zero-fill section kinds
                        position, remaining = section[4], section[3]
                        if remaining and (position < 32 + length or position + remaining > size or position < fileoff or position + remaining > fileoff + filesize):
                            _invalid("Mach-O section exceeds image")
                        if remaining and any(position < end and position + remaining > start for start, end in file_ranges):
                            _invalid("overlapping Mach-O sections")
                        if remaining:
                            file_ranges.append((position, position + remaining))
                        handle.seek(position)
                        while remaining:
                            block = handle.read(min(1024 * 1024, remaining))
                            if not block:
                                _invalid("truncated Mach-O section")
                            digest.update(block)
                            remaining -= len(block)
                    if key == (b"__TEXT", b"__text"):
                        code_ranges.append((section[4], section[3]))
                    sections[key] = (section[2], section[3], section[8], digest.hexdigest())
            elif command in (0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023, 0xd, 0x8000001c, 0xe):
                required = 24 if command in (0xc, 0x80000018, 0x8000001f, 0x20, 0x80000023, 0xd) else 12
                if stride < required:
                    _invalid("truncated Mach-O loader path")
                start = struct.unpack_from("<I", data, 8)[0]
                if not required <= start < stride or b"\0" not in data[start:]:
                    _invalid("invalid Mach-O loader string")
                try:
                    text = data[start:].split(b"\0", 1)[0].decode("utf-8")
                except UnicodeError:
                    _invalid("invalid Mach-O loader string encoding")
                if not text or any(ord(c) < 32 for c in text):
                    _invalid("invalid Mach-O loader path")
                if command == 0x8000001c:
                    rpaths.append(text)
                elif command == 0xe:
                    if text != "/usr/lib/dyld":
                        _invalid("unsupported Mach-O interpreter")
                elif command == 0xd:
                    if not text.startswith(("@rpath/", "@loader_path/")):
                        _invalid("bundled dylib identity must be package-relative")
                else:
                    dependencies.append(text)
                    if command == 0x80000018:  # LC_LOAD_WEAK_DYLIB
                        weak_dependencies.append(text)
            elif command in (0x24, 0x32):
                if stride < (16 if command == 0x24 else 24) or minimum is not None:
                    _invalid("invalid Mach-O deployment command")
                if command == 0x32 and struct.unpack_from("<I", data, 8)[0] != 1:
                    _invalid("Mach-O image is not built for macOS")
                if command == 0x32 and 24 + struct.unpack_from("<I", data, 20)[0] * 8 != stride:
                    _invalid("invalid Mach-O build-tool table")
                minimum = struct.unpack_from("<I", data, 8 if command == 0x24 else 12)[0]
            elif command == 0x80000028:
                if stride != 24 or entry is not None:
                    _invalid("invalid Mach-O entry point")
                entry = struct.unpack_from("<QQ", data, 8)
            elif command == 0x2:  # LC_SYMTAB; reject debug/STAB records, not ordinary names.
                if stride != 24 or symbol_table:
                    _invalid("invalid or duplicate Mach-O symbol command")
                symbol_table = True
                symbols, number, strings, string_size = struct.unpack_from("<4I", data, 8)
                if number > 1024 * 1024 or symbols + number * 16 > size or strings + string_size > size:
                    _invalid("invalid Mach-O symbol table")
                handle.seek(symbols)
                for _ in range(number):
                    symbol = handle.read(16)
                    if len(symbol) != 16:
                        _invalid("truncated Mach-O symbol table")
                    if symbol[4] & 0xe0:
                        _invalid("macOS release contains debug symbols")
            elif command == 0x27:
                _invalid("Mach-O loader environment commands are unsupported")
            elif command & 0x80000000 and command not in (0x80000022, 0x80000033, 0x80000034):
                _invalid("unsupported required Mach-O load command")
            elif command in (0x6, 0x7, 0x10):
                _invalid("legacy Mach-O dependency commands are unsupported")
            offset += stride
        if offset != length or (b"__TEXT", b"__text") not in sections:
            _invalid("invalid Mach-O command table or missing code")
        if kind == 2 and (entry is None or not any(start <= entry[0] < start + length for start, length in code_ranges)):
            _invalid("Mach-O executable entry point is missing or outside code")
        if minimum is None or not 0x000a0900 <= minimum <= 0x000e0000:
            _invalid("macOS release requires a deployment baseline no newer than 14.0")
        return {"cpu": cpu, "subtype": subtype, "kind": kind, "entry": entry,
                "minimum": minimum, "sections": sections, "dependencies": dependencies,
                "weak_dependencies": weak_dependencies, "rpaths": rpaths}


def executable_identity(path: Path) -> dict:
    image = inspect(path)
    return {key: image[key] for key in ("cpu", "subtype", "kind", "entry", "minimum", "sections")}


def _relative(root: Path, path: Path, executable: Path, value: str) -> Path:
    for prefix, base in (("@loader_path", path.parent), ("@executable_path", executable.parent)):
        if value == prefix or value.startswith(prefix + "/"):
            candidate = (base / value[len(prefix):].lstrip("/")).resolve()
            if candidate.is_relative_to(root.resolve()):
                return candidate
            _invalid("Mach-O loader path escapes package")
    _invalid("Mach-O search paths must use package-relative loader tokens")


def app_bundle(root: Path, entry_point: str) -> Path:
    parts = Path(entry_point).parts
    if len(parts) != 4 or not parts[0].endswith(".app") or parts[1:3] != ("Contents", "MacOS"):
        _invalid("macOS entry point must be App.app/Contents/MacOS/executable")
    bundle = root / parts[0]
    plist = bundle / "Contents/Info.plist"
    try:
        if plist.stat().st_size > 256 * 1024:
            _invalid("Info.plist exceeds package limit")
        info = plistlib.loads(plist.read_bytes())
    except (OSError, ValueError, plistlib.InvalidFileException):
        _invalid("missing or invalid app Info.plist")
    if not isinstance(info, dict) or info.get("CFBundleExecutable") != parts[3] or info.get("CFBundlePackageType") != "APPL":
        _invalid("app Info.plist disagrees with entry point")
    return bundle


def _images(root):
    for path in sorted(root.rglob("*")):
        if path.is_file():
            with path.open("rb") as handle:
                magic = handle.read(4)
            if magic == MAGIC or magic in (b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\x7fELF", b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
                yield path


def _codesign(argv, *, runner=None, env=None, cancel_check=None):
    if sys.platform != "darwin":
        fail("macOS signature validation requires a macOS host", "MissingTools")
    env = {key: value for key, value in (env if env is not None else os.environ).items()
           if key not in ("BUTLER_API_KEY", "CODESIGN_ALLOCATE") and not key.startswith("DYLD_")}
    if cancel_check:
        cancel_check()
    if runner is not None:
        if runner(["/usr/bin/codesign", *argv], cwd=Path.cwd(), env=env):
            _invalid("invalid macOS code signature")
        if cancel_check:
            cancel_check()
        return
    with tempfile.TemporaryFile() as output:
        try:
            result = subprocess.run(["/usr/bin/codesign", *argv], env=env, stdout=output,
                                    stderr=output, timeout=30, check=False)
        except (OSError, subprocess.TimeoutExpired):
            _invalid("macOS signature verification failed or timed out")
        if result.returncode or output.tell() > MAX_NATIVE_INSPECTION_OUTPUT_BYTES:
            _invalid("invalid macOS code signature")


def sign_payload(root: Path, entry_point: str, runner, env: dict, cancel_check=None) -> None:
    # Apple, Creating distribution-signed code for macOS: sign nested code first.
    # Local ad-hoc signatures use no identity/keychain, network or secure timestamp;
    # Developer ID / notarization are a separate distribution workflow.
    # https://developer.apple.com/documentation/xcode/creating-distribution-signed-code-for-the-mac/
    bundle = app_bundle(root, entry_point)
    env = {key: value for key, value in env.items() if key not in ("BUTLER_API_KEY", "CODESIGN_ALLOCATE") and not key.startswith("DYLD_")}
    for path in [*list(_images(root)), bundle]:
        if cancel_check:
            cancel_check()
        if runner(["/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(path)], cwd=root, env=env):
            fail("macOS ad-hoc signing failed", "BuildFailed")


def validate_native(root: Path, entry_point: str, target_platform: str, *, runner=None, env=None, cancel_check=None) -> list[str]:
    root = root.resolve()
    bundle = app_bundle(root, entry_point)
    executable = root / entry_point
    if not executable.is_file() or not os.access(executable, os.X_OK):
        _invalid("macOS entry point is missing or not executable")
    external, images = set(), {path: inspect(path) for path in _images(root)}
    if executable not in images or images[executable]["kind"] != 2:
        _invalid("macOS entry point is not an executable Mach-O image")
    for path, image in images.items():
        if image["cpu"] != CPUS.get(target_platform):
            _invalid("Mach-O architecture disagrees with release platform")
        search = [_relative(root, path, executable, value) for value in image["rpaths"]]
        for needed in image["dependencies"]:
            framework = needed.split("/")
            system = needed in SYSTEM_LIBRARIES or (len(framework) == 8 and framework[:4] == ["", "System", "Library", "Frameworks"] and framework[4] == framework[7] + ".framework" and framework[5] == "Versions" and framework[6] in ("A", "C") and framework[7] in FRAMEWORKS)
            if system:
                introduced = FRAMEWORK_MINIMUMS.get(framework[-1], 0) if len(framework) == 8 else 0
                if image["minimum"] < introduced and needed not in image["weak_dependencies"]:
                    _invalid(f"{framework[-1]} requires macOS {introduced >> 16}.0 or later; raise the deployment baseline or weak-link it")
                external.add(needed)
                continue
            candidates = ([directory / needed[7:] for directory in search] if needed.startswith("@rpath/") else [_relative(root, path, executable, needed)])
            candidates = [candidate.resolve() for candidate in candidates]
            if any(not candidate.is_relative_to(root.resolve()) for candidate in candidates):
                _invalid("Mach-O dependency escapes package")
            loaded = next((candidate for candidate in candidates if candidate.exists()), None)
            if loaded is None or images[loaded]["kind"] != 6:
                _invalid(f"unresolved or invalid Mach-O dependency {needed!r}")
        _codesign(["--verify", "--strict", str(path)], runner=runner, env=env, cancel_check=cancel_check)
    _codesign(["--verify", "--strict", "--deep", str(bundle)], runner=runner, env=env, cancel_check=cancel_check)
    if not (root / "NOTICE.txt").is_file() or not (root / "NOTICE.txt").stat().st_size or not any(path.is_file() and path.stat().st_size for path in (root / "licenses").rglob("*")):
        _invalid("package requires NOTICE.txt and nonempty licenses/")
    return sorted(external)
