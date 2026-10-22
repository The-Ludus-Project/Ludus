"""Explicitly opted-in editor setup, separate from the default engine tooling."""
from __future__ import annotations

import os
import platform
import shutil
import subprocess
import threading
from pathlib import Path

import cmake_targets
import init_progress
import init_terminal


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
    if platform.system() == "Darwin":
        result.append(f"-DLUDUS_SPIRV_CROSS={root / 'out/shader-tools/spirv-cross/bin/spirv-cross'}")
    return result


def preview_tool_commands(root: Path, engine) -> list[list[object]]:
    """The trusted commands that download, verify and unpack the pinned shader tools."""
    commands: list[list[object]] = [[engine.venv_python(root), root / "scripts/shader-probe", "bootstrap"]]
    if platform.system() == "Darwin":
        commands.append([engine.venv_python(root), root / "scripts/bootstrap-spirv-cross"])
    return commands


class BackgroundCommands:
    """Run commands one after another on a worker thread and replay their output when waited for.

    Their output is held back so it cannot interleave with the step running in
    the foreground; ``wait`` prints it under the step that needs the result and
    raises the first failure as ``EngineError``. Nothing here may touch what
    the foreground steps write (Conan, CMake and the managed environment).
    """

    def __init__(self, commands, *, cwd: Path, engine):
        self.commands = [list(command) for command in commands]
        self.cwd = cwd
        self.engine = engine
        self.environment = dict(os.environ)
        self.results: list[tuple[list[object], int | None, str]] = []
        self.process: subprocess.Popen | None = None
        self.is_cancelled = False
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self._work, name="ludus-init-preview-tools", daemon=True)

    def start(self) -> BackgroundCommands:
        self.thread.start()
        return self

    def _work(self) -> None:
        for command in self.commands:
            with self.lock:
                if self.is_cancelled:
                    return
                try:
                    self.process = subprocess.Popen([str(argument) for argument in command], cwd=str(self.cwd),
                                                    env=self.environment, stdout=subprocess.PIPE,
                                                    stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, text=True,
                                                    errors="replace")
                except OSError as error:
                    self.results.append((command, None, str(error)))
                    return
            output, _ = self.process.communicate()
            self.results.append((command, self.process.returncode, output))
            if self.process.returncode != 0:
                return

    def wait(self) -> None:
        """Wait for every command, print what each printed, and raise the first failure."""
        self.thread.join()
        painter = init_terminal.painter_for()
        for command, return_code, output in self.results:
            print(init_terminal.command_echo(self.engine.command_line(command), painter), flush=True)
            if output:
                print(output, end="" if output.endswith("\n") else "\n", flush=True)
            if return_code is None:
                raise self.engine.EngineError(f"command not found while running: {self.engine.command_line(command)}")
            if return_code != 0:
                raise self.engine.EngineError(
                    f"command failed with exit code {return_code}: {self.engine.command_line(command)}")

    def cancel(self) -> None:
        """Stop a command that is still running, for example after another step failed."""
        with self.lock:
            self.is_cancelled = True
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
        self.thread.join()


def prepare_editor_preview(args, engine) -> BackgroundCommands | None:
    """Start acquiring verified shader tools, only for selected rendered editor setup.

    The returned job runs in the background; the editor step waits for it
    (``BackgroundCommands.wait``) right before configuring.
    """
    if args.with_editor and args.with_editor_preview:
        validate_editor_options(args, engine)
        root = engine.repo_root()
        return BackgroundCommands(preview_tool_commands(root, engine), cwd=root, engine=engine).start()
    return None


def wait_for_preview_tools(args, engine, preparation: BackgroundCommands | None) -> None:
    """The ``preview_tools`` step: finish the shader tool download that ./init.sh started.

    Editor setup outside ./init.sh (``preparation`` is None) never fetched the
    shader tools itself and still does not.
    """
    if not args.with_editor_preview or preparation is None:
        return
    init_progress.begin("preview_tools")
    preparation.wait()


def compiler_cache_args(root: Path, preset: str, engine, environment=None) -> list[str]:
    """Turn on ccache for a new editor build tree when it is installed.

    ``LUDUS_ENABLE_CCACHE`` decides when it is set (``0`` turns ccache off). An
    existing build tree keeps its current choice, because switching the
    compiler launcher recompiles everything once; set the variable to switch it.
    """
    environment = os.environ if environment is None else environment
    requested = environment.get("LUDUS_ENABLE_CCACHE", "").strip()
    if requested:
        return [f"-DLUDUS_ENABLE_CCACHE={'OFF' if requested == '0' else 'ON'}"]
    cache = engine.build_dir_for_preset(root, preset) / "CMakeCache.txt"
    if engine.cmake_cache_value(cache, "LUDUS_ENABLE_CCACHE") is not None or shutil.which("ccache") is None:
        return []
    print("Using ccache for this new build tree so rebuilds reuse compiled objects "
          "(set LUDUS_ENABLE_CCACHE=0 to turn it off).")
    return ["-DLUDUS_ENABLE_CCACHE=ON"]


def setup_editor(args, engine, *, preparation: BackgroundCommands | None = None) -> None:
    if not args.with_editor:
        return
    build_editor(args, engine, preparation=preparation)
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


def homebrew_qt_prefixes(brew: str, engine, root: Path, *, with_preview: bool) -> tuple[Path | None, Path | None]:
    """Installed Homebrew Qt CMake package directories (Qt6, Qt6Quick), or None for each one missing."""
    base = quick = None
    status, output = engine.capture_command_quiet([brew, "--prefix", "qtbase"], cwd=root)
    if status == 0 and (Path(output.strip()) / "lib/cmake/Qt6").is_dir():
        base = Path(output.strip()) / "lib/cmake/Qt6"
    if with_preview:
        status, output = engine.capture_command_quiet([brew, "--prefix", "qtdeclarative"], cwd=root)
        if status == 0 and (Path(output.strip()) / "lib/cmake/Qt6Quick").is_dir():
            quick = Path(output.strip()) / "lib/cmake/Qt6Quick"
    return base, quick


def build_editor(args, engine, *, preparation: BackgroundCommands | None = None) -> None:
    validate_editor_options(args, engine)
    root = engine.repo_root()
    preview_args = [f"-DLUDUS_EDITOR_RENDER_PREVIEW={'ON' if args.with_editor_preview else 'OFF'}"]
    if args.with_editor_preview:
        preview_args.extend(preview_tool_args(root))
    if platform.system() == "Darwin":
        brew = shutil.which("brew")
        base = quick = None
        if not args.no_system_install:
            init_progress.begin("editor_packages")
            if not brew:
                raise engine.EngineError("Install Qt 6.4+ manually, or install Homebrew, then rerun editor setup")
            base, quick = homebrew_qt_prefixes(brew, engine, root, with_preview=args.with_editor_preview)
            if base is not None and (quick is not None or not args.with_editor_preview):
                # `brew install` of an installed formula still refreshes Homebrew's
                # package index and may upgrade Qt; neither is needed to build.
                init_progress.up_to_date("Homebrew Qt is installed (run `brew upgrade qtbase` to update it)")
            else:
                engine.run([brew, "install", "qtbase", *(["qtdeclarative"] if args.with_editor_preview else [])],
                           cwd=root)
                base, quick = homebrew_qt_prefixes(brew, engine, root, with_preview=args.with_editor_preview)
        else:
            print("Using existing Qt 6.4+ with the Cocoa plugin (--no-system-install).")
            if brew:
                base, quick = homebrew_qt_prefixes(brew, engine, root, with_preview=args.with_editor_preview)
        # Homebrew Qt is optional and may be keg-only. Keep its machine path in
        # the local CMake cache, never in committed presets or the engine SDK.
        qt_args = []
        if base is not None:
            qt_args.append(f"-DQt6_DIR={base}")
        if quick is not None:
            # Homebrew's split formula is not beneath qtbase's Qt6_DIR.
            qt_args.append(f"-DQt6Quick_DIR={quick}")
        wait_for_preview_tools(args, engine, preparation)
        init_progress.begin("editor_configure")
        cmake_targets.query_codemodel(engine.build_dir_for_preset(root, args.preset), "ludus-editor")
        engine.cmake_configure(root, args.preset, ["-DLUDUS_BUILD_EDITOR=ON", *preview_args, *qt_args],
                               reuse_current_cache=True)
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
            init_progress.up_to_date("the editor's Qt packages are installed")
    else:
        print("Using existing Qt 6.4+ packages for the optional editor (--no-system-install).")

    wait_for_preview_tools(args, engine, preparation)
    init_progress.begin("editor_configure")
    cmake_targets.query_codemodel(engine.build_dir_for_preset(root, args.preset), "ludus-editor")
    engine.cmake_configure(root, args.preset, ["-DLUDUS_BUILD_EDITOR=ON", *preview_args],
                           reuse_current_cache=True)
    init_progress.begin("editor_build")
    engine.cmake_build(root, args.preset, ["--target", "ludus_editor"])
