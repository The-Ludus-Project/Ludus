"""Relocate the installed Windows runtime SDK and consume it with Ninja and MSBuild (D6b).

The SDK comes from ``scripts/install-sdk.ps1 windows-msvc-runtime-sdk``. This
moves it to a path with spaces, then builds the external runtime consumer and a
project created by the shipped CLI against the relocated prefix only.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

FIXTURE = Path(__file__).resolve().parent
ROOT = FIXTURE.parents[2]
PRESET = "windows-msvc-runtime-sdk"
TARGET = "ludus_windows_runtime_consumer"
CONFIGURATION = "RelWithDebInfo"
RUNTIME_COMPONENTS = "GraphicsRenderer,GraphicsText,Text,Resources,GameplayWorld,Ui,GameHost"


def run(*arguments, cwd=ROOT, contains=None, should_fail=False):
    result = subprocess.run(list(map(str, arguments)), cwd=cwd, text=True, errors="replace",
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
    print(result.stdout, end="", flush=True)
    if should_fail:
        require(result.returncode != 0, f"Expected a failure from {arguments}")
    else:
        result.check_returncode()
    if contains is not None and contains not in result.stdout:
        raise RuntimeError(f"Expected {contains!r} from {arguments}")
    return result.stdout


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def relocate(workspace):
    installed = ROOT / "out/install" / PRESET
    require((installed / "lib/cmake/Ludus/LudusConfig.cmake").is_file(), f"Install {PRESET} first")
    relocated = workspace / "relocated runtime SDK" / "Development"
    if relocated.exists():
        shutil.rmtree(relocated)
    relocated.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(installed, relocated)
    # The original prefix disappears, so any absolute reference would fail to resolve.
    shutil.rmtree(installed)
    producer = (str(installed), installed.as_posix(), str(ROOT), ROOT.as_posix())
    for package_file in (relocated / "lib/cmake").rglob("*.cmake"):
        text = package_file.read_text(encoding="utf-8", errors="replace")
        leaked = [path for path in producer if path in text]
        require(not leaked, f"{package_file} references the producer path {leaked}")
    return relocated


def consume(sdk, workspace, visual_studio):
    generator = "MSBuild" if visual_studio else "Ninja"
    print(f"\n=== Runtime consumer / {generator} ===", flush=True)
    source = workspace / "runtime consumer" / generator / "source files"
    build = source.parent / "build"
    shutil.copytree(FIXTURE / "runtime", source, dirs_exist_ok=True)
    if visual_studio:
        toolset = os.environ["VCToolsVersion"].rstrip("\\")
        options = ("-G", "Visual Studio 17 2022", "-A", "x64,version=10.0.26100.0",
                   "-T", f"v143,version={toolset},host=x64", f"-DCMAKE_CONFIGURATION_TYPES={CONFIGURATION}")
    else:
        options = ("-G", "Ninja", "-DCMAKE_CXX_COMPILER=cl.exe", f"-DCMAKE_BUILD_TYPE={CONFIGURATION}")
    shader_tools = ROOT / "out/shader-tools"
    run("cmake", "-S", source, "-B", build, *options, f"-DCMAKE_PREFIX_PATH={sdk}",
        f"-DLUDUS_SLANG_COMPILER={shader_tools / 'slang/bin/slangc.exe'}",
        f"-DLUDUS_DXC_DIRECTORY={shader_tools / 'dxc'}")
    run("cmake", "--build", build, "--config", CONFIGURATION, "--target", TARGET)
    shader = build / "ludus-shaders" / TARGET / "renderer_flat"
    for stage in ("vertex", "fragment"):
        require((shader / f"renderer_flat.{stage}.dxil").is_file(), f"Missing renderer_flat {stage} DXIL")
    executable = (build / CONFIGURATION if visual_studio else build) / (TARGET + ".exe")
    require((executable.parent / "D3D12" / "D3D12Core.dll").is_file(), "The Agility runtime was not staged")
    run("ctest", "--test-dir", build, "-C", CONFIGURATION, "--output-on-failure", "--no-tests=error",
        contains="100% tests passed")


def consume_behavior(sdk, workspace):
    print("\n=== Behavior SDK / cook, play and debug ===", flush=True)
    source = workspace / "scripted game" / "source files"
    build = source.parent / "build"
    shutil.copytree(ROOT / "examples/scripted-game", source, dirs_exist_ok=True)
    run("cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_CXX_COMPILER=cl.exe",
        f"-DCMAKE_BUILD_TYPE={CONFIGURATION}", f"-DCMAKE_PREFIX_PATH={sdk}")
    run("cmake", "--build", build, "--config", CONFIGURATION)
    run("ctest", "--test-dir", build, "-C", CONFIGURATION, "--output-on-failure", "--no-tests=error",
        contains="100% tests passed")
    module = build / "scripted_game.dll"
    first_generation = build / "scripted_game_initial.dll"
    shutil.copyfile(module, first_generation)
    cook = build / "encounter"
    pointer = cook / "current.json"
    original_pointer = pointer.read_bytes()
    evidence = cook / json.loads(original_pointer)["directory"] / "evidence.json"
    script = source / "behaviors/encounter.luau"
    valid_source = script.read_text(encoding="utf-8")
    script.write_text("--!strict\nreturn function(\n", encoding="utf-8")
    run("cmake", "--build", build, "--config", CONFIGURATION, "--target", "scripted_game_module",
        contains="SyntaxError", should_fail=True)
    require(pointer.read_bytes() == original_pointer, "Failed cook changed the current script package")
    require(module.read_bytes() == first_generation.read_bytes(), "Failed cook changed the game DLL")
    changed = valid_source.replace("state.Interactions += event.Amount", "state.Interactions += event.Amount * 2")
    require(changed != valid_source, "The scripted-game fixture no longer has the expected increment")
    script.write_text(changed, encoding="utf-8")
    run("cmake", "--build", build, "--config", CONFIGURATION, "--target", "scripted_game_module")
    supervisor = ROOT / "modules/runtime/game_host/tests/windows_supervisor_acceptance.py"
    run(sys.executable, supervisor, "--host", build / "scripted_game_host.exe",
        "--scripted-module", first_generation, "--scripted-replacement", module, "--script-evidence", evidence)


def run_project(*arguments, contains=None):
    environment = dict(os.environ, PYTHONPATH=str(ROOT / "scripts/python"))
    result = subprocess.run([sys.executable, "-m", "ludus_tools", "project", *map(str, arguments)], cwd=ROOT,
                            env=environment, text=True, errors="replace", stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=1800)
    print(result.stdout, end="", flush=True)
    result.check_returncode()
    if contains is not None and contains not in result.stdout:
        raise RuntimeError(f"Expected {contains!r} from project {arguments}")
    return result.stdout


def verify_project(sdk, workspace):
    print("\n=== Runtime project / Ninja and MSBuild ===", flush=True)
    project = workspace / "runtime project"
    if project.exists():
        shutil.rmtree(project)
    run_project("create", project, "--name", "LudusRuntimeProject", "--tools", ROOT, "--sdk", sdk,
                "--components", RUNTIME_COMPONENTS, "--profile", "windows-msvc-development",
                contains="version_query")
    for profile in ("windows-msvc-development", "windows-msvc-visual-studio-development"):
        run_project("repair", project, "--tools", ROOT, "--profile", profile, "--sdk", sdk,
                    contains="100% tests passed")
        run_project("check", project, "--tools", ROOT, "--profile", profile,
                    contains=" · Development · x86_64-pc-windows-msvc")
        run_project("run", project, "--profile", profile)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("workspace", type=Path)
    parser.add_argument("--behavior", action="store_true", help="exercise the opt-in installed Behavior component")
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    sdk = relocate(workspace)
    for visual_studio in (False, True):
        consume(sdk, workspace, visual_studio)
    verify_project(sdk, workspace)
    if args.behavior:
        consume_behavior(sdk, workspace)
