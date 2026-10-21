"""Explicitly opted-in editor setup, separate from the default engine tooling."""
from __future__ import annotations

import platform
import shutil
from pathlib import Path

import cmake_targets
import init_progress


SUPPORTED_PRESETS = tuple(f"{family}-clang-{flavor}" for family in ("linux", "macos")
                          for flavor in ("debug", "development", "asan-ubsan"))
QT_PACKAGES = ("qt6-base-dev", "qt6-wayland")
QT_PREVIEW_PACKAGES = ("qt6-declarative-dev",)


def supported_host() -> bool:
    machine = platform.machine().lower()
    return ((platform.system() == "Linux" and machine in ("x86_64", "amd64"))
            or (platform.system() == "Darwin" and machine in ("arm64", "aarch64", "x86_64", "amd64")))


def validate_editor_options(args, engine) -> None:
    if not args.with_editor:
        return
    if not supported_host():
        raise engine.EngineError("The optional Ludus editor supports Linux x64 and macOS arm64/x64")
    family = "macos-" if platform.system() == "Darwin" else "linux-"
    if args.preset not in SUPPORTED_PRESETS or not args.preset.startswith(family):
        raise engine.EngineError("Editor setup requires " + ", ".join(SUPPORTED_PRESETS) + "; browser editor setup is unsupported")
    if args.with_editor_preview and platform.system() == "Darwin":
        version = platform.mac_ver()[0]
        if version and int(version.split(".")[0]) < 15:
            raise engine.EngineError("Editor 3D preview requires macOS 15 or newer for live shader compilation; "
                                     "use --no-editor-preview for a document-only editor")


def preview_tool_args(root: Path) -> list[str]:
    """Resolve repository-local pinned tools for native editor configuration."""
    result = [f"-DLUDUS_SLANG_COMPILER={root / 'out/shader-tools/slang/bin/slangc'}"]
    if platform.system() != "Darwin":
        result.append(f"-DLUDUS_SPIRV_VALIDATOR={root / 'out/shader-tools/spirv-tools/usr/bin/spirv-val'}")
    return result


def prepare_editor_preview(args, engine) -> None:
    """Acquire verified shader tools only for selected rendered editor setup."""
    if args.with_editor and args.with_editor_preview:
        validate_editor_options(args, engine)
        root = engine.repo_root()
        engine.run([engine.venv_python(root), root / "scripts/shader-probe", "bootstrap"], cwd=root)


def setup_editor(args, engine) -> None:
    if not args.with_editor:
        return
    build_editor(args, engine)
    refresh_editor_engine_installs(args, engine)


def refresh_editor_engine_installs(args, engine) -> list[Path]:
    """Reinstall this checkout's engine into its own installs built from another revision.

    Projects pin installs below ``out/install`` through ``.ludus/local.json``.
    Building only ``ludus_editor`` used to leave those installs at the old
    revision, so games silently built against an older engine than the Editor.
    Installs of another ABI variant or another target/flavor are left alone;
    installs outside this checkout are never touched. Returns refreshed prefixes.
    """
    if getattr(args, "skip_engine_refresh", False):
        print("Skipping the engine install refresh (--no-engine-refresh).")
        return []
    init_progress.begin("engine_refresh")
    from ludus_tools.engine_pin import stale_editor_installs
    from ludus_tools.errors import ToolingError
    from ludus_tools.identity import load_manifest_file

    root = engine.repo_root()
    build_dir = engine.build_dir_for_preset(root, args.preset)
    try:
        built = load_manifest_file(build_dir / "cmake/LudusSdkManifest.json")
    except ToolingError:
        return []
    stale = stale_editor_installs(root, target=built.target_triple, flavor=built.flavor,
                                  revision=built.source_revision, abi_key=built.abi_key())
    if not stale:
        return []
    print("Refreshing engine installs built from an older revision so projects match this editor:")
    for prefix in stale:
        print(f"  {prefix}")
    # Install every engine target, not just the editor's dependencies.
    engine.cmake_build(root, args.preset)
    for prefix in stale:
        engine.run([engine.cmake(root), "--install", build_dir, "--prefix", prefix], cwd=root, env=engine.tool_env(root))
        engine.bundle_sdk_dependencies(root, args.preset, prefix)
    print("Engine installs refreshed. Open each project and choose Check Setup; Repair refreshes its build settings.")
    return stale


def build_editor(args, engine) -> None:
    validate_editor_options(args, engine)
    root = engine.repo_root()
    preview_args = [f"-DLUDUS_EDITOR_RENDER_PREVIEW={'ON' if args.with_editor_preview else 'OFF'}"]
    if args.with_editor_preview:
        preview_args.extend(preview_tool_args(root))
    if platform.system() == "Darwin":
        brew = shutil.which("brew")
        if not args.no_system_install:
            init_progress.begin("editor_packages")
            if not brew:
                raise engine.EngineError("Install Qt 6.4+ manually, or install Homebrew, then rerun editor setup")
            engine.run([brew, "install", "qtbase", *(["qtdeclarative"] if args.with_editor_preview else [])], cwd=root)
        else:
            print("Using existing Qt 6.4+ with the Cocoa plugin (--no-system-install).")
        # Homebrew Qt is optional and may be keg-only. Keep its machine path in
        # the local CMake cache, never in committed presets or the engine SDK.
        qt_args = []
        if brew:
            status, output = engine.capture_command_quiet([brew, "--prefix", "qtbase"], cwd=root)
            if status == 0 and (Path(output.strip()) / "lib/cmake/Qt6").is_dir():
                qt_args = [f"-DQt6_DIR={output.strip()}/lib/cmake/Qt6"]
            if args.with_editor_preview:
                status, output = engine.capture_command_quiet([brew, "--prefix", "qtdeclarative"], cwd=root)
                quick_directory = Path(output.strip()) / "lib/cmake/Qt6Quick"
                if status == 0 and quick_directory.is_dir():
                    # Homebrew's split formula is not beneath qtbase's Qt6_DIR.
                    qt_args.append(f"-DQt6Quick_DIR={quick_directory}")
        init_progress.begin("editor_configure")
        cmake_targets.query_codemodel(engine.build_dir_for_preset(root, args.preset), "ludus-editor")
        engine.cmake_configure(root, args.preset, ["-DLUDUS_BUILD_EDITOR=ON", *preview_args, *qt_args])
        init_progress.begin("editor_build")
        engine.cmake_build(root, args.preset, ["--target", "ludus_editor"])
        return
    if not args.no_system_install:
        init_progress.begin("editor_packages")
        if not engine.host_supports_apt_install():
            raise engine.EngineError("Automatic editor prerequisites require Ubuntu/Debian; install Qt 6.4+ manually and use --no-system-install")
        missing = []
        for package in (*QT_PACKAGES, *(QT_PREVIEW_PACKAGES if args.with_editor_preview else ())):
            status, output = engine.capture_command_quiet(
                ["dpkg-query", "--show", "--showformat=${Status}", package], cwd=root)
            if status != 0 or output.strip() != "install ok installed":
                missing.append(package)
        if missing:
            prefix = engine.sudo_command_prefix()
            print("Installing optional editor Qt packages. This may ask for your sudo password.")
            engine.run([*prefix, "apt-get", "update"], cwd=root)
            engine.run([*prefix, "apt-get", "install", "-y", *missing], cwd=root)
    else:
        print("Using existing Qt 6.4+ packages for the optional editor (--no-system-install).")

    init_progress.begin("editor_configure")
    cmake_targets.query_codemodel(engine.build_dir_for_preset(root, args.preset), "ludus-editor")
    engine.cmake_configure(root, args.preset, ["-DLUDUS_BUILD_EDITOR=ON", *preview_args])
    init_progress.begin("editor_build")
    engine.cmake_build(root, args.preset, ["--target", "ludus_editor"])
