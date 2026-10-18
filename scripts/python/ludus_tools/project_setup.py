"""Shared read-only setup diagnosis and explicit local repair for CLI/Editor.

Only trusted tooling is executed. Open never imports project-owned Python or
runs bootstrap hooks. Repair owns marked presets and local SDK overrides only.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from dataclasses import replace

from .buildlock import BuildTreeLock
from .errors import INVALID_PROJECT, ToolingError
from .identity import load_prefix_manifest
from .lockfile import parse_local_settings_file
from .operations import PRESET_FLAVOR, locate_project, resolve_project
from .resolve import compute_stamp, write_stamp
from .sdkstore import SdkStore
from .descriptor import parse_descriptor_file
from .engine_pin import validate_install_prefix
from .native import validate_target
from .toolchain import PINNED_LLVM_MAJOR

OWNER = "ludus.dev/project-setup/1"


def read_object(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        value = json.loads(path.read_text())
    except (ValueError, OSError) as exc:
        raise ToolingError(INVALID_PROJECT, f"Cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ToolingError(INVALID_PROJECT, f"Expected JSON object: {path}")
    return value


def write_object(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".setup-tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def preset_for(source: Path, profile: str) -> str:
    for item in read_object(source / "CMakeUserPresets.json").get("configurePresets", []):
        if isinstance(item, dict) and item.get("vendor", {}).get(OWNER) == profile:
            return item["name"]
    return profile


def cmake_for(source: Path, profile: str, fallback: str) -> str:
    """Select the same owned CMake for CLI, Editor and generation builds."""
    selected = preset_for(source, profile)
    for item in read_object(source / "CMakeUserPresets.json").get("configurePresets", []):
        if item.get("name") == selected and OWNER in item.get("vendor", {}):
            return str(executable(Path(item["cmakeExecutable"])))
    return fallback


def preset_names(path: Path, seen=None) -> set[str]:
    seen = set() if seen is None else seen
    path = path.resolve()
    if path in seen:
        return set()
    if len(seen) >= 32:
        raise ToolingError(INVALID_PROJECT, "Too many preset includes")
    seen.add(path)
    obj = read_object(path)
    names = {p["name"] for p in obj.get("configurePresets", [])}
    for include in obj.get("include", []):
        names.update(preset_names(path.parent / include, seen))
    return names


def executable(path: Path) -> Path:
    # Keep the managed path, including symlinks, for SDK ABI/tool selection.
    if not path.is_file() or not os.access(path, os.X_OK):
        raise ToolingError("MissingTools", f"Missing executable: {path}; prepare the tooling checkout")
    return path


def tools(root: Path) -> dict:
    venv = root / "out/host-tools/venv/bin"
    return {"cmake": executable(venv / "cmake"), "ctest": executable(venv / "ctest"),
            "ninja": executable(venv / "ninja"),
            "compiler": executable(root / "out/host-tools/bin/clang++")}


def run_command(argv, *, cwd, env):
    result = subprocess.run(argv, cwd=cwd, env=env, text=True, capture_output=True, timeout=600)
    if result.returncode:
        raise ToolingError("ConfigureFailed", result.stderr[-4096:] or result.stdout[-4096:])
    print(result.stdout, end="")
    return result.stdout


def environment(root: Path) -> dict:
    env = os.environ.copy()
    entries = (root / "out/host-tools/bin", root / "out/host-tools/venv/bin")
    env["PATH"] = os.pathsep.join(map(str, entries)) + os.pathsep + env.get("PATH", "")
    env["CONAN_HOME"] = str(root / "out/conan/home")
    env["CONAN_NON_INTERACTIVE"] = "1"
    env.pop("BUTLER_API_KEY", None)
    return env


def supported_project(path: Path, profile: str | None = None):
    paths = locate_project(path)
    descriptor = parse_descriptor_file(paths.descriptor_path)
    if profile is not None:
        descriptor = replace(descriptor, preset=profile)
    if descriptor.version != 2 or descriptor.provider != "cmake" or descriptor.preset not in PRESET_FLAVOR:
        raise ToolingError(INVALID_PROJECT, "Setup requires a saved version-2 native CMake project")
    source = (paths.project_dir / descriptor.source_dir).resolve()
    if not (source / "CMakeLists.txt").is_file():
        raise ToolingError(INVALID_PROJECT, "Missing CMakeLists.txt")
    return paths, descriptor, source


def validate_native_identity(identity, descriptor):
    validate_target(identity, descriptor.preset)
    if identity.engine_version != descriptor.engine.version:
        raise ToolingError(INVALID_PROJECT, "Selected native SDK must match the project's engine version and native target")
    missing = set(descriptor.engine.components) - set(identity.components)
    if missing:
        raise ToolingError(INVALID_PROJECT, f"SDK lacks required components: {sorted(missing)}")


def _inputs(root: Path, source: Path, sdk: Path, profile: str, web_sdk: Path | None):
    native = load_prefix_manifest(sdk)
    configured = tools(root)
    cache = {"CMAKE_MAKE_PROGRAM": str(configured["ninja"]),
             "CMAKE_CXX_COMPILER": str(configured["compiler"]),
             "CMAKE_PREFIX_PATH": str(sdk),
             "CMAKE_EXPORT_COMPILE_COMMANDS": "ON"}
    # Package discovery remains in CMake; include prepared dependencies needed by
    # exported native libraries, without baking machine paths into tracked files.
    dependencies = root / "out/conan" / profile
    if profile.startswith("linux-") and dependencies.is_dir():
        cache["CMAKE_PREFIX_PATH"] += ";" + str(dependencies)
    if profile.startswith("macos-"):
        sysroot = root / "out/host-tools/macos-sdk"
        libcxx = root / "out/host-tools/libcxx-include"
        for directory in (sysroot, libcxx):
            if not directory.is_dir():
                raise ToolingError("MissingTools", f"Missing macOS toolchain input: {directory}; run ./init.sh")
        # Record real symlink destinations and policy headers, so retargeting a
        # managed SDK/header link invalidates an existing setup at the same path.
        configured.update(sysroot=sysroot, libcxx=libcxx)
        for key, file in (("sdk_settings", sysroot / "SDKSettings.json"),
                          ("libcxx_config", libcxx / "__config")):
            if not file.is_file():
                raise ToolingError("MissingTools", f"Incomplete macOS toolchain input: {file}; run ./init.sh")
            configured[key] = file
        baseline = re.fullmatch(r"macos-(\d+(?:\.\d+)*)", native.distro_baseline)
        if not baseline:
            raise ToolingError(INVALID_PROJECT, "macOS SDK must record its deployment baseline")
        architecture = native.target_triple.split("-", 1)[0]
        # CMake resolves an SDK symlink in its cache. Generate that same path;
        # configured above retains the managed link for retarget detection.
        cache.update(CMAKE_OSX_SYSROOT=str(sysroot.resolve()), CMAKE_OSX_ARCHITECTURES=architecture,
                     CMAKE_OSX_DEPLOYMENT_TARGET=baseline.group(1),
                     CMAKE_CXX_FLAGS=f'-stdlib=libc++ -nostdinc++ -isystem "{libcxx}"')
    shaders = "ludus_compile_shader" in (source / "CMakeLists.txt").read_text()
    # Shader includes are also common (Sandbox owns cmake/Shaders.cmake).
    shaders |= any("ludus_compile_shader" in p.read_text() for p in (source / "cmake").glob("*.cmake"))
    if shaders:
        cache["LUDUS_SLANG_COMPILER"] = str(executable(root / "out/shader-tools/slang/bin/slangc"))
        if not profile.startswith("macos-"):
            cache["LUDUS_SPIRV_VALIDATOR"] = str(spirv_validator(root))
    records = {profile: {"cacheVariables": cache}}
    stamps = {str(sdk): compute_stamp(sdk)}
    if web_sdk:
        web_identity = load_prefix_manifest(web_sdk)
        if web_identity.engine_version != native.engine_version or web_identity.target_triple != "wasm32-unknown-emscripten":
            raise ToolingError(INVALID_PROJECT, "Web SDK must match the engine release and wasm32-unknown-emscripten target")
        toolchain = root / "out/host-tools/emsdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"
        if not toolchain.is_file():
            raise ToolingError("MissingTools", "Web toolchain is missing; prepare Emscripten")
        cross = executable(root / "out/shader-tools/spirv-cross/bin/spirv-cross")
        webcache = {k: v for k, v in cache.items()
                    if k not in ("CMAKE_CXX_COMPILER", "CMAKE_CXX_FLAGS") and not k.startswith("CMAKE_OSX_")}
        if shaders:
            webcache["LUDUS_SPIRV_VALIDATOR"] = str(spirv_validator(root))
        webcache.update(CMAKE_PREFIX_PATH=str(web_sdk), CMAKE_FIND_ROOT_PATH=str(web_sdk), LUDUS_SPIRV_CROSS=str(cross))
        for webprofile, buildtype in (("web-emscripten-development", "RelWithDebInfo"), ("web-emscripten-release", "Release")):
            records[webprofile] = {"toolchainFile": str(toolchain),
                                   "cacheVariables": dict(webcache, CMAKE_BUILD_TYPE=buildtype)}
        stamps[str(web_sdk)] = compute_stamp(web_sdk)
    return configured, records, stamps


def spirv_validator(root: Path) -> Path:
    candidates = sorted((root / "out/shader-tools/spirv-tools").glob("**/spirv-val"))
    if not candidates:
        raise ToolingError("MissingTools", "Missing pinned spirv-val; prepare shader tools")
    return executable(candidates[0])


def verify_tool_versions(configured, root: Path, runner, env, identity=None):
    versions = read_object(root / "config/tool_versions.json")
    pins = versions.get("managed", {})
    clang = versions.get("minimum", {}).get("clang", "")
    expected = {"cmake": pins.get("cmake", "3.29.6"), "ninja": pins.get("ninja", "1.11.1.3").rsplit(".", 1)[0],
                "compiler": clang.split(".", 1)[0] if clang else str(PINNED_LLVM_MAJOR)}
    for key, pattern in (("cmake", r"cmake version (\d+\.\d+\.\d+)"),
                         ("ninja", r"(\d+\.\d+\.\d+)"),
                         ("compiler", r"clang version (\d+)")):
        output = runner([str(configured[key]), "--version"], cwd=root, env=env)
        match = re.search(pattern, output)
        wanted = expected[key]
        if not match or match.group(1) != wanted:
            raise ToolingError("MissingTools", f"{key} must use pinned version {wanted}; prepare the tooling checkout")
        if key == "compiler" and identity is not None:
            version = re.search(r"clang version (\d+\.\d+\.\d+)", output)
            if identity.compiler_id != "Clang" or not version or version.group(1) != identity.compiler_version:
                raise ToolingError("SdkIncompatible", f"SDK compiler: expected {identity.compiler_id} {identity.compiler_version}; got {output.strip()}")


def cache_value(path: Path, key: str):
    for line in path.read_text().splitlines():
        if line.startswith(key + ":") and "=" in line:
            return line.split("=", 1)[1]
    return None


def _signature(configured, records, stamps):
    payload = {"tools": {k: (str(v), str(v.resolve()), v.stat().st_size, v.stat().st_mtime_ns) for k, v in configured.items()}, "presets": records, "sdks": stamps}
    return hashlib.sha256(json.dumps(payload, sort_keys=True).encode()).hexdigest()


def _list(source: Path, configured, profiles, runner, env):
    for kind in ("configure", "build", "test"):
        output = runner([str(configured["cmake"]), f"--list-presets={kind}"], cwd=source, env=env)
        visible = set(re.findall(r'^  "([^"\n]+)"', output, re.MULTILINE))
        required = {preset_for(source, p) for p in profiles if kind != "test" or not p.startswith("web-")}
        if not required <= visible:
            raise ToolingError(INVALID_PROJECT, f"Missing selectable {kind} presets: {sorted(required - visible)}; run repair")


def setup_for_profile(settings: dict, profile: str) -> dict:
    if settings.get("profile", profile) == profile:
        return settings
    return settings.get("profiles", {}).get(profile, {})


def stale_revision_warning(tooling_root: Path, sdk_revision: str | None, sdk: Path) -> str:
    """Warn when the selected SDK was installed from a different engine revision than this checkout."""
    from .engine_pin import are_revisions_equal, checkout_revision, is_editor_owned, is_known_revision
    if not is_known_revision(sdk_revision):
        return ""
    head = checkout_revision(tooling_root)
    if not head or are_revisions_equal(head, sdk_revision):
        return ""
    if is_editor_owned(sdk, tooling_root):
        action = ("Use this editor's engine (Project > Repair Project Setup) to rebuild and reinstall it; "
                  "Automatic repair also refreshes it.")
    else:
        action = ("It was selected explicitly, so repair keeps it. Choose Use this editor's engine "
                  "(Project > Repair Project Setup) to build, install and select this checkout's engine.")
    return (f"Warning: the selected engine install ({sdk}) was built from {sdk_revision[:7]}, but this checkout is at "
            f"{head[:7]}. The Editor itself runs from this checkout; games build against the older install. "
            f"{action}\n")


def check_project(path: Path, *, tooling_root: Path, profile: str | None = None,
                  runner=run_command, cancel_check=lambda: None):
    """Read metadata and ask CMake to list presets; never configure or write."""
    paths, descriptor, source = supported_project(path, profile)
    settings = setup_for_profile(read_object(paths.project_dir / ".ludus/setup.json"), descriptor.preset)
    if not settings:
        raise ToolingError(INVALID_PROJECT, "Project setup is missing; use Project > Repair Project Setup")
    resolved = resolve_project(path, store=SdkStore(), profile=descriptor.preset, enforce_host_toolchain=False)
    sdk = resolved.resolution.prefix.resolve()
    validate_native_identity(resolved.resolution.identity, descriptor)
    web = Path(settings["web_sdk"]) if settings.get("web_sdk") else None
    configured, records, stamps = _inputs(tooling_root, source, sdk, descriptor.preset, web)
    if settings.get("signature") != _signature(configured, records, stamps):
        raise ToolingError(INVALID_PROJECT, "SDK or tool inputs changed; run repair to refresh presets and caches")
    local = read_object(source / "CMakeUserPresets.json")
    for profile, fields in records.items():
        item = next((p for p in local.get("configurePresets", []) if p.get("name") == preset_for(source, profile)), None)
        if not item or item.get("hidden", False) or item.get("cmakeExecutable") != str(configured["cmake"]) or any(item.get(k) != v for k, v in fields.items()):
            raise ToolingError(INVALID_PROJECT, f"Stale local preset for {profile}; run repair")
        cache_path = source / "out/build" / profile / "CMakeCache.txt"
        if cache_path.is_file():
            for key in ("CMAKE_PREFIX_PATH", "CMAKE_MAKE_PROGRAM", "CMAKE_CXX_COMPILER",
                        "CMAKE_CXX_FLAGS", "CMAKE_OSX_SYSROOT", "CMAKE_OSX_ARCHITECTURES",
                        "CMAKE_OSX_DEPLOYMENT_TARGET"):
                expected = fields["cacheVariables"].get(key)
                if expected and cache_value(cache_path, key) != expected:
                    raise ToolingError(INVALID_PROJECT, f"Stale CMake cache for {profile}: {key}; run repair")
    ide = read_object(source / ".vscode/settings.json")
    if ide.get("cmake.useCMakePresets") != "always" or ide.get("cmake.cmakePath", str(configured["cmake"])) != str(configured["cmake"]):
        raise ToolingError(INVALID_PROJECT, "IDE CMake path or preset mode is stale; run repair")
    cancel_check()
    env = environment(tooling_root)
    verify_tool_versions(configured, tooling_root, runner, env, resolved.resolution.identity)
    _list(source, configured, records, runner, env)
    identity = resolved.resolution.identity
    warning = stale_revision_warning(tooling_root, identity.source_revision, sdk).rstrip()
    warning = "\n" + warning if warning else ""
    return ("Setup checked: selectable presets, SDKs, tools and IDE settings\n"
            f"Selected engine: Ludus {identity.engine_version} · {identity.flavor} · {identity.target_triple}\n"
            f"Engine revision: {identity.source_revision or 'unknown'}\n"
            f"Selection: {resolved.resolution.source}"
            f"{' (local override)' if resolved.resolution.is_override else ''}\n"
            f"Installation: {sdk}"
            f"{warning}")


def select_repair_sdk(path: Path, *, tooling_root: Path, prepare=None) -> Path:
    """Reuse selected/locked engines; prepare the editor engine when needed.

    Explicit local overrides and release locks retain their existing precedence
    when they point outside this tooling checkout: a corrupt, moved or
    incompatible external selection requires an explicit replacement, and an
    older external install is kept (Check Setup reports it). Installs below
    ``<tooling>/out/install`` belong to this editor, so a stale or broken one is
    rebuilt from this checkout and selected instead (``engine_pin``).
    Called only by explicit repair, never by read-only project loading.
    """
    from .creation_engine import select_creation_sdk
    from .engine_pin import diagnose_engine_pin, should_refresh_automatically
    _, descriptor, _ = supported_project(path)
    pin = diagnose_engine_pin(path, tooling_root=tooling_root, profile=descriptor.preset)
    if should_refresh_automatically(pin):
        print(f"Refreshing this editor's engine: {pin.message}")
        return select_creation_sdk(tooling_root, profile=descriptor.preset, prepare=prepare, environ={})
    try:
        return resolve_project(path, store=SdkStore(), profile=descriptor.preset,
                               enforce_host_toolchain=False).resolution.prefix
    except ToolingError as exc:
        if exc.code != "UnresolvedLock":
            raise
    return select_creation_sdk(tooling_root, profile=descriptor.preset, prepare=prepare)


def repair_project(path: Path, *, tooling_root: Path, sdk: Path | None = None,
                   web_sdk: Path | None = None, disable_web: bool = False, profile: str | None = None,
                   runner=run_command, cancel_check=lambda: None):
    if disable_web and web_sdk is not None:
        raise ToolingError(INVALID_PROJECT, "Choose browser setup or desktop-only repair, not both")
    paths, descriptor, source = supported_project(path, profile)
    if sdk is None:
        sdk = resolve_project(path, store=SdkStore(), profile=descriptor.preset, enforce_host_toolchain=False).resolution.prefix
    sdk = sdk.resolve()
    # Reject build folders and empty folders before any project file is touched.
    validate_install_prefix(sdk)
    resolved = resolve_project(path, store=SdkStore(), cli_sdk_prefix=sdk, profile=descriptor.preset, enforce_host_toolchain=False)
    identity = resolved.resolution.identity
    validate_native_identity(identity, descriptor)
    all_settings = read_object(paths.project_dir / ".ludus/setup.json")
    previous = setup_for_profile(all_settings, descriptor.preset)
    if not disable_web and web_sdk is None and previous.get("web_sdk"):
        web_sdk = Path(previous["web_sdk"])
    if web_sdk:
        web_sdk = web_sdk.resolve()
    configured, records, stamps = _inputs(tooling_root, source, sdk, descriptor.preset, web_sdk)
    local = read_object(source / "CMakeUserPresets.json")
    ide = read_object(source / ".vscode/settings.json")
    # CMake validates included roots/inheritance/conditions with real list calls.
    roots = preset_names(source / "CMakePresets.json")
    for key in ("configurePresets", "buildPresets", "testPresets"):
        items = local.get(key, [])
        if not isinstance(items, list) or any(not isinstance(p, dict) for p in items):
            raise ToolingError(INVALID_PROJECT, f"Invalid {key}; fix JSON before repair")
        local[key] = [p for p in items
                      if p.get("vendor", {}).get(OWNER) not in records
                      and not str(p.get("vendor", {}).get(OWNER, "")).startswith("web-")]
        if any(p.get("name") == "ludus-local-" + profile for p in local[key] for profile in records):
            raise ToolingError(INVALID_PROJECT, "Custom preset occupies a managed name; rename it before repair")
    local["version"] = max(local.get("version", 6), 6)
    for profile, fields in records.items():
        if "ludus-local-" + profile in roots:
            raise ToolingError(INVALID_PROJECT, "Custom root preset occupies a managed name")
        inherited = profile + "-base" if profile + "-base" in roots else profile
        if inherited not in roots:
            raise ToolingError(INVALID_PROJECT, f"Missing supported base preset: {profile}")
        name = "ludus-local-" + profile
        marker = {OWNER: profile}
        local["configurePresets"].append(dict(fields, name=name, inherits=inherited, hidden=False,
            binaryDir="${sourceDir}/out/build/" + profile, cmakeExecutable=str(configured["cmake"]), vendor=marker))
        local["buildPresets"].append(dict(name=name, configurePreset=name, vendor=marker))
        if not profile.startswith("web-"):
            local["testPresets"].append(dict(name=name, configurePreset=name, vendor=marker,
                output={"outputOnFailure": True}, execution={"noTestsAction": "error"}))
    local_settings = parse_local_settings_file(paths.local_settings_path)
    local_settings.set_override(target=identity.target_triple, flavor=identity.flavor, prefix=str(sdk))
    env = environment(tooling_root)
    env["LUDUS_SDK_PREFIX"] = str(sdk)
    cancel_check()
    verify_tool_versions(configured, tooling_root, runner, env, resolved.resolution.identity)
    # Own the selected native tree for the complete configure/build/test sequence.
    with BuildTreeLock(resolved.paths.build_dir):
        # A failed repair must not leave a previous verification stamp valid.
        (paths.project_dir / ".ludus/setup.json").unlink(missing_ok=True)
        write_object(source / "CMakeUserPresets.json", local)
        ide["cmake.useCMakePresets"] = "always"
        # Machine CMake paths belong to ignored presets, not tracked IDE files.
        ide.pop("cmake.cmakePath", None)
        write_object(source / ".vscode/settings.json", ide)
        write_object(paths.local_settings_path, json.loads(local_settings.serialize()))
        ignore = source / ".gitignore"
        contents = ignore.read_text() if ignore.exists() else ""
        for entry in ("/CMakeUserPresets.json", "/.ludus/"):
            if entry not in contents.splitlines():
                contents += "\n" + entry + "\n"
        ignore.write_text(contents)
        _list(source, configured, records, runner, env)
        for profile in records:
            cancel_check()
            name = preset_for(source, profile)
            def verify():
                from cmake_targets import query_codemodel
                tree = source / "out/build" / profile
                for client in ("ludus-cli", "ludus-editor"):
                    query_codemodel(tree, client)
                runner([str(configured["cmake"]), "--fresh", "--preset", name], cwd=source, env=env)
                runner([str(configured["cmake"]), "--build", "--preset", name], cwd=source, env=env)
                if not profile.startswith("web-"):
                    runner([str(configured["ctest"]), "--preset", name], cwd=source, env=env)
            if profile == descriptor.preset:
                verify()
            else:
                with BuildTreeLock(source / "out/build" / profile):
                    verify()
        cancel_check()
        if any(compute_stamp(Path(prefix)) != stamp for prefix, stamp in stamps.items()):
            raise ToolingError("StampChanged", "SDK changed during repair; retry with stable inputs")
        # A verified repair replaces the old mutable-SDK stamp, allowing the
        # ordinary Editor/CLI configure path to use the updated SDK afterward.
        write_stamp(resolved.paths.build_dir, resolved.resolution)
        state = {"signature": _signature(configured, records, stamps), "web_sdk": str(web_sdk) if web_sdk else ""}
        profiles = dict(all_settings.get("profiles", {}))
        old_profile = all_settings.get("profile", descriptor.preset)
        if all_settings.get("signature"):
            profiles[old_profile] = {key: all_settings.get(key, "") for key in ("signature", "web_sdk")}
        profiles[descriptor.preset] = state
        write_object(paths.project_dir / ".ludus/setup.json",
                     dict(state, profile=descriptor.preset, profiles=profiles))
    return check_project(path, tooling_root=tooling_root, profile=descriptor.preset, runner=runner, cancel_check=cancel_check)
